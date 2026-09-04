#include "WavePerfCBCtrl.h"

#include "WavePerfArchitecture.h"
#include "WaveTypes.h"

#include <QJsonArray>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace waveperf {
namespace {

enum class SlotAccess {
    Read,
    Write
};

struct EndpointSpec {
    const char* token;
    const char* stage;
    const char* stageName;
    SlotAccess access;
    int order;
};

const QVector<EndpointSpec>& endpointSpecs() {
    static const QVector<EndpointSpec> specs = {
        {"pt_be_cbctrl_new_inst", "request_ingress", "指令请求入口",
         SlotAccess::Read, 0},
        {"ptgrpcore2cbctrl", "group_ingress", "Group 请求入口",
         SlotAccess::Read, 0},
        {"pt_cbctrl_thdcore_uop", "read_issue", "寄存器读 uop 发出",
         SlotAccess::Write, 1},
        {"ptcbctrl2cbdatainstinfo", "cbdata_dispatch", "CBData 指令下发",
         SlotAccess::Write, 2},
        {"pteu2cbdata", "operand_return", "EU 操作数返回",
         SlotAccess::Write, 3},
        {"ptcbdata2eudstp", "writeback", "CBData 目的端写回",
         SlotAccess::Write, 4},
        {"ptcbdata2eu", "writeback", "CBData 写回",
         SlotAccess::Write, 4},
        {"pt_cbctrl_be_clr_pending", "pending_clear", "指令 Pending 清除",
         SlotAccess::Write, 5},
        {"pt_cbdata_cbctrl_credit", "credit_return", "CBData 资源返还",
         SlotAccess::Read, 6},
        {"pt_l1lstx_cbctrl_credit", "credit_return", "L1LSTX Credit 返回",
         SlotAccess::Read, 6},
        {"ptcbctrl2mmactrlinst", "auxiliary_dispatch", "MMA 下发",
         SlotAccess::Write, 2},
        {"ptcbctrl2sgmdtf", "auxiliary_dispatch", "DTF 下发",
         SlotAccess::Write, 2},
        {"ptcbctrl2barrierctrlarrive", "auxiliary_dispatch", "Barrier 到达",
         SlotAccess::Write, 2},
        {"ptcbctrl2barrierctrlwakeup", "auxiliary_dispatch", "Barrier 唤醒",
         SlotAccess::Write, 2},
        {"ptcbctrl2grpcore", "auxiliary_dispatch", "Group Core 返回",
         SlotAccess::Write, 2}
    };
    return specs;
}

struct SampleValue {
    bool known = false;
    quint64 value = 0;
};

SampleValue sampleValue(const WaveSignal* signal,
                        qint64 tick,
                        bool strictBefore) {
    if (!signal || signal->samples.isEmpty()) return SampleValue();
    int lo = 0;
    int hi = signal->samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const qint64 sampleTick = signal->samples.at(mid).time;
        if (strictBefore ? sampleTick < tick : sampleTick <= tick) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo <= 0) return SampleValue();
    WaveSample sample = signal->samples.at(lo - 1);
    if (!sample.rawFieldsReady) {
        hydrateWaveSampleRawFields(signal->kind, signal->width, sample);
    }
    if (sample.isZ || sample.isAbsent) return SampleValue();
    SampleValue result;
    result.known = true;
    result.value = sample.rawBits & waveBitMaskForWidth(signal->width);
    return result;
}

qint64 saturatingAdd(qint64 left, quint64 right) {
    if (right > quint64(std::numeric_limits<qint64>::max()) ||
        left > std::numeric_limits<qint64>::max() - qint64(right)) {
        return std::numeric_limits<qint64>::max();
    }
    return left + qint64(right);
}

struct Port {
    const EndpointSpec* spec = nullptr;
    QString base;
    QString ppuPath;
    int portQppu = -1;
    const WaveSignal* reads = nullptr;
    const WaveSignal* writes = nullptr;
    const WaveSignal* readable = nullptr;
    const WaveSignal* size = nullptr;
    const WaveSignal* readIndex = nullptr;
    QHash<int, QHash<QString, const WaveSignal*>> fieldsBySlot;
    int inferredSlots = 0;
};

struct StageStats {
    QString key;
    QString name;
    qint64 events = 0;
    qint64 observedTicks = 0;
    qint64 nonemptyTicks = 0;
    qint64 fullTicks = 0;
    int channels = 0;
    int occupancyChannels = 0;
};

struct LatencyStats {
    qint64 count = 0;
    long double total = 0.0L;
    qint64 maximum = 0;
    QVector<qint64> samples;

    void add(qint64 cycles, bool keepSamples) {
        if (cycles < 0) return;
        ++count;
        total += cycles;
        maximum = qMax(maximum, cycles);
        if (keepSamples) samples.push_back(cycles);
    }

    void sortSamples() {
        std::sort(samples.begin(), samples.end());
    }

    double average() const {
        return count > 0 ? double(total / count) : 0.0;
    }

    double percentile(double fraction) const {
        if (samples.isEmpty()) return 0.0;
        const int index = qBound(
            0, int(std::ceil(fraction * samples.size())) - 1,
            samples.size() - 1);
        return double(samples.at(index));
    }
};

struct QppuStats {
    QString ppuPath;
    int qppu = -1;
    qint64 requests = 0;
    qint64 firstUops = 0;
    qint64 readUops = 0;
    qint64 returns = 0;
    qint64 clears = 0;
    qint64 writebacks = 0;
    LatencyStats arbitration;
    LatencyStats pending;
};

struct ClientStats {
    int index = -1;
    qint64 readUops = 0;
    qint64 returns = 0;
    LatencyStats readLatency;
};

struct PcStats {
    QString ppuPath;
    int qppu = -1;
    int sg = -1;
    quint64 pc = 0;
    qint64 requests = 0;
    qint64 firstUops = 0;
    qint64 clears = 0;
    LatencyStats arbitration;
    LatencyStats pending;
};

struct Identity {
    QString ppuPath;
    int qppu = -1;
    int sg = -1;
    quint64 pc = 0;
    bool pcKnown = false;
    int client = -1;
    int mux = -1;
    bool firstKnown = false;
    bool first = false;
    bool validKnown = false;
    bool valid = true;

    bool instructionKeyKnown() const {
        return !ppuPath.isEmpty() && qppu >= 0 && sg >= 0 &&
               pcKnown;
    }
};

QString normalizedSegment(QString value) {
    value = value.toLower();
    value.remove(QRegularExpression(QStringLiteral("\\[size=\\d+\\]")));
    return value;
}

const EndpointSpec* endpointForPath(const QString& lowerPath) {
    for (const EndpointSpec& spec : endpointSpecs()) {
        if (lowerPath.contains(
                QLatin1Char('.') + QLatin1String(spec.token))) {
            return &spec;
        }
    }
    return nullptr;
}

QString ppuPathForPort(const QString& path) {
    static const QRegularExpression ppuExpression(
        QStringLiteral(
            "^(.*\\.m_ppu(?:\\[size=\\d+\\])?\\.\\[\\d+\\])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch ppuMatch =
        ppuExpression.match(path);
    if (ppuMatch.hasMatch()) return ppuMatch.captured(1);

    const QString lower = path.toLower();
    int owner = lower.indexOf(QStringLiteral(".m_cbctrl"));
    if (owner < 0) owner = lower.indexOf(QStringLiteral(".m_ppusdata"));
    if (owner < 0) owner = lower.indexOf(QStringLiteral(".m_qpputop"));
    if (owner < 0) {
        const EndpointSpec* spec = endpointForPath(lower);
        if (spec) {
            owner = lower.indexOf(
                QLatin1Char('.') + QLatin1String(spec->token));
        }
    }
    return owner > 0 ? path.left(owner) : QString();
}

int portQppuIndex(const QString& path,
                  const EndpointSpec& spec) {
    const QRegularExpression endpointIndex(
        QStringLiteral("\\.%1(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]")
            .arg(QRegularExpression::escape(
                QLatin1String(spec.token))),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = endpointIndex.match(path);
    if (match.hasMatch()) return match.captured(1).toInt();

    static const QRegularExpression qppuIndex(
        QStringLiteral(
            "\\.m_QPPUTOP(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]"),
        QRegularExpression::CaseInsensitiveOption);
    match = qppuIndex.match(path);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

QString fifoBase(const QString& path) {
    static const QVector<QString> markers = {
        QStringLiteral(".m_num_read"),
        QStringLiteral(".m_num_written"),
        QStringLiteral(".m_num_readable"),
        QStringLiteral(".m_size"),
        QStringLiteral(".m_ri"),
        QStringLiteral(".m_buf")
    };
    const QString lower = path.toLower();
    int cut = -1;
    for (const QString& marker : markers) {
        const int candidate = lower.indexOf(marker);
        if (candidate >= 0 && (cut < 0 || candidate < cut)) {
            cut = candidate;
        }
    }
    return cut >= 0 ? path.left(cut) : QString();
}

bool parseBufferField(const QString& path,
                      int& slot,
                      QString& field) {
    static const QRegularExpression expression(
        QStringLiteral(
            "\\.m_buf_?(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]\\.(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(path);
    if (!match.hasMatch()) return false;
    slot = match.captured(1).toInt();
    field = normalizedSegment(match.captured(2));
    return true;
}

const WaveSignal* fieldSignal(
    const Port& port,
    int slot,
    std::initializer_list<const char*> names) {
    const auto slotIt = port.fieldsBySlot.constFind(slot);
    if (slotIt == port.fieldsBySlot.constEnd()) return nullptr;
    for (const char* rawName : names) {
        const QString name = QLatin1String(rawName);
        auto exact = slotIt->constFind(name);
        if (exact != slotIt->constEnd()) return exact.value();
        const QString suffix = QLatin1Char('.') + name;
        for (auto it = slotIt->constBegin();
             it != slotIt->constEnd(); ++it) {
            if (it.key().endsWith(suffix)) return it.value();
        }
    }
    return nullptr;
}

SampleValue fieldValue(
    const Port& port,
    int slot,
    std::initializer_list<const char*> names,
    qint64 tick,
    bool strictBefore) {
    return sampleValue(
        fieldSignal(port, slot, names), tick, strictBefore);
}

QString instructionKey(const Identity& identity) {
    if (!identity.instructionKeyKnown()) return QString();
    return identity.ppuPath + QLatin1Char('|') +
           QString::number(identity.qppu) + QLatin1Char('|') +
           QString::number(identity.sg) + QLatin1Char('|') +
           QString::number(identity.pc);
}

QString qppuKey(const Identity& identity) {
    if (identity.ppuPath.isEmpty() || identity.qppu < 0) {
        return QString();
    }
    return identity.ppuPath + QLatin1Char('|') +
           QString::number(identity.qppu);
}

QString returnKey(const Identity& identity) {
    const QString instruction = instructionKey(identity);
    if (instruction.isEmpty() || identity.client < 0) return QString();
    return instruction + QLatin1Char('|') +
           QString::number(identity.client) + QLatin1Char('|') +
           QString::number(identity.mux);
}

QString pcText(quint64 pc) {
    return QStringLiteral("0x%1").arg(
        pc, 0, 16, QLatin1Char('0'));
}

void addLatencyJson(QJsonObject& object,
                    const QString& prefix,
                    const LatencyStats& stats,
                    bool includePercentiles) {
    object.insert(prefix + QStringLiteral("_matched"), double(stats.count));
    if (stats.count <= 0) return;
    object.insert(prefix + QStringLiteral("_average_cycles"),
                  stats.average());
    if (includePercentiles) {
        object.insert(prefix + QStringLiteral("_p50_cycles"),
                      stats.percentile(0.50));
        object.insert(prefix + QStringLiteral("_p95_cycles"),
                      stats.percentile(0.95));
    }
    object.insert(prefix + QStringLiteral("_maximum_cycles"),
                  double(stats.maximum));
}

qint64 alignAtOrAfter(qint64 tick,
                      qint64 start,
                      qint64 ticksPerCycle) {
    if (tick <= start) return start;
    const qint64 delta = tick - start;
    const qint64 remainder = delta % ticksPerCycle;
    return remainder == 0 ? tick
                          : tick + ticksPerCycle - remainder;
}

qint64 nextActiveTick(const WaveSignal* signal,
                      qint64 fromTick,
                      qint64 startTick,
                      qint64 endTick,
                      qint64 ticksPerCycle) {
    if (!signal || fromTick >= endTick) return endTick;
    qint64 candidate =
        alignAtOrAfter(fromTick, startTick, ticksPerCycle);
    if (candidate >= endTick) return endTick;
    const SampleValue current =
        sampleValue(signal, candidate, false);
    if (current.known && current.value > 0) {
        return candidate;
    }

    int lo = 0;
    int hi = signal->samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal->samples.at(mid).time <= candidate) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (int index = lo; index < signal->samples.size(); ++index) {
        const WaveSample& sample = signal->samples.at(index);
        if (sample.time >= endTick) break;
        WaveSample hydrated = sample;
        if (!hydrated.rawFieldsReady) {
            hydrateWaveSampleRawFields(
                signal->kind, signal->width, hydrated);
        }
        if (hydrated.isZ || hydrated.isAbsent ||
            (hydrated.rawBits &
             waveBitMaskForWidth(signal->width)) == 0) {
            continue;
        }
        candidate = alignAtOrAfter(
            qMax(fromTick, sample.time), startTick,
            ticksPerCycle);
        if (candidate >= endTick) return endTick;
        const SampleValue value =
            sampleValue(signal, candidate, false);
        if (value.known && value.value > 0) return candidate;
    }
    return endTick;
}

void accumulateOccupancy(const Port& port,
                         qint64 startTick,
                         qint64 endTick,
                         StageStats& stage) {
    if (!port.readable || !port.size || endTick <= startTick) return;
    QSet<qint64> boundarySet;
    boundarySet.insert(startTick);
    boundarySet.insert(endTick);
    for (const WaveSignal* signal :
         {port.readable, port.size}) {
        for (const WaveSample& sample : signal->samples) {
            if (sample.time > startTick && sample.time < endTick) {
                boundarySet.insert(sample.time);
            }
        }
    }
    QVector<qint64> boundaries = boundarySet.values().toVector();
    std::sort(boundaries.begin(), boundaries.end());
    for (int index = 0; index + 1 < boundaries.size(); ++index) {
        const qint64 begin = boundaries.at(index);
        const qint64 duration =
            boundaries.at(index + 1) - begin;
        const SampleValue occupancy =
            sampleValue(port.readable, begin, false);
        const SampleValue capacity =
            sampleValue(port.size, begin, false);
        if (!occupancy.known || !capacity.known ||
            capacity.value == 0) {
            continue;
        }
        stage.observedTicks += duration;
        if (occupancy.value > 0) stage.nonemptyTicks += duration;
        if (occupancy.value >= capacity.value) {
            stage.fullTicks += duration;
        }
    }
    ++stage.occupancyChannels;
}

int eventSlot(const Port& port,
              qint64 tick,
              int eventOffset,
              bool& usedFallback) {
    SampleValue size = sampleValue(port.size, tick, true);
    SampleValue readIndex =
        sampleValue(port.readIndex, tick, true);
    SampleValue readable =
        sampleValue(port.readable, tick, true);
    if (!size.known) {
        size = sampleValue(port.size, tick, false);
        usedFallback = usedFallback || size.known;
    }
    if (!readIndex.known) {
        readIndex = sampleValue(port.readIndex, tick, false);
        usedFallback = usedFallback || readIndex.known;
    }
    if (!readable.known) {
        readable = sampleValue(port.readable, tick, false);
        usedFallback = usedFallback || readable.known;
    }
    const int capacity =
        size.known && size.value > 0
            ? int(qMin<quint64>(
                  size.value,
                  quint64(std::numeric_limits<int>::max())))
            : port.inferredSlots;
    if (capacity <= 0) return -1;
    const quint64 head = readIndex.known ? readIndex.value : 0;
    const quint64 occupancy =
        port.spec->access == SlotAccess::Write &&
                readable.known
            ? readable.value
            : 0;
    return int((head + occupancy +
                quint64(qMax(0, eventOffset))) %
               quint64(capacity));
}

Identity identityForEvent(const Port& port,
                          int slot,
                          qint64 tick,
                          bool& payloadFallback) {
    Identity identity;
    identity.ppuPath = port.ppuPath;
    const bool strictBefore =
        port.spec->access == SlotAccess::Read;
    auto read = [&](std::initializer_list<const char*> names) {
        SampleValue value =
            fieldValue(port, slot, names, tick, strictBefore);
        if (!value.known && strictBefore) {
            value = fieldValue(port, slot, names, tick, false);
            payloadFallback = payloadFallback || value.known;
        }
        return value;
    };

    const SampleValue qppu =
        read({"qppu_id", "qppu_idx", "qppuid"});
    const SampleValue sg =
        read({"sg_id", "sgid", "local_sg_id"});
    const SampleValue pc = read({"pc_", "pc"});
    const SampleValue client =
        read({"client", "client_type", "instr_type"});
    const SampleValue mux = read({"mux_id"});
    const SampleValue first =
        read({"is_first_uop", "first_uop"});
    const SampleValue valid = read({"valid", "vld"});
    identity.qppu =
        qppu.known ? int(qppu.value) : port.portQppu;
    identity.sg = sg.known ? int(sg.value) : -1;
    identity.pc = pc.known ? pc.value : 0;
    identity.pcKnown = pc.known;
    identity.client =
        client.known ? int(client.value) : -1;
    identity.mux = mux.known ? int(mux.value) : -1;
    identity.firstKnown = first.known;
    identity.first = first.known && first.value != 0;
    identity.validKnown = valid.known;
    identity.valid = !valid.known || valid.value != 0;
    return identity;
}

QppuStats& qppuStatsFor(QHash<QString, QppuStats>& stats,
                        const Identity& identity) {
    const QString key = qppuKey(identity);
    QppuStats& result = stats[key];
    if (result.ppuPath.isEmpty()) {
        result.ppuPath = identity.ppuPath;
        result.qppu = identity.qppu;
    }
    return result;
}

PcStats& pcStatsFor(QHash<QString, PcStats>& stats,
                    const Identity& identity) {
    const QString key = instructionKey(identity);
    PcStats& result = stats[key];
    if (result.ppuPath.isEmpty()) {
        result.ppuPath = identity.ppuPath;
        result.qppu = identity.qppu;
        result.sg = identity.sg;
        result.pc = identity.pc;
    }
    return result;
}

void appendQueue(QHash<QString, QQueue<qint64>>& queues,
                 const QString& key,
                 qint64 tick) {
    if (!key.isEmpty()) queues[key].enqueue(tick);
}

bool consumeQueue(QHash<QString, QQueue<qint64>>& queues,
                  const QString& key,
                  qint64 tick,
                  qint64 ticksPerCycle,
                  LatencyStats& global,
                  LatencyStats* qppu,
                  LatencyStats* pc) {
    auto it = queues.find(key);
    if (it == queues.end() || it->isEmpty()) return false;
    const qint64 requestTick = it->dequeue();
    if (it->isEmpty()) queues.erase(it);
    const qint64 cycles =
        qMax<qint64>(0, (tick - requestTick) / ticksPerCycle);
    global.add(cycles, true);
    if (qppu) qppu->add(cycles, false);
    if (pc) pc->add(cycles, false);
    return true;
}

struct Cursor {
    int port = -1;
    const WaveSignal* signal = nullptr;
    qint64 nextTick = 0;
};

struct HeapItem {
    qint64 tick = 0;
    int order = 0;
    int cursor = -1;
};

struct HeapLater {
    bool operator()(const HeapItem& left,
                    const HeapItem& right) const {
        if (left.tick != right.tick) return left.tick > right.tick;
        if (left.order != right.order) return left.order > right.order;
        return left.cursor > right.cursor;
    }
};

QString stageName(const QString& key) {
    if (key == QStringLiteral("auxiliary_dispatch")) {
        return QStringLiteral("其他专用通路下发");
    }
    if (key == QStringLiteral("writeback")) {
        return QStringLiteral("CBData 写回");
    }
    for (const EndpointSpec& spec : endpointSpecs()) {
        if (key == QLatin1String(spec.stage)) {
            return QString::fromUtf8(spec.stageName);
        }
    }
    return key;
}

QJsonObject buildProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    bool hasDynamicWave,
    QStringList& warnings) {
    QJsonObject result;
    result.insert(
        QStringLiteral("definition"),
        QStringLiteral(
            "CBCtrl 请求、寄存器读 uop、EU 操作数返回、CBData 下发、"
            "Pending 清除和写回均按 FIFO 每周期事件计数；FIFO 读事件使用"
            "事件前读指针和槽内容，写事件使用事件前尾位置与事件时槽内容。"));
    result.insert(
        QStringLiteral("causality_boundary"),
        QStringLiteral(
            "波形未暴露 CBCtrl 内部 eligible 向量、TR/GR/FU 资源模拟器"
            "和 timeout 仲裁选择；这类原因只发布阶段相关性，不伪装为"
            "精确仲裁拒绝原因。"));

    QHash<QString, Port> portsByBase;
    for (auto it = signalsByPath.constBegin();
         it != signalsByPath.constEnd(); ++it) {
        const QString path = it.key();
        const QString lower = path.toLower();
        const EndpointSpec* spec = endpointForPath(lower);
        if (!spec) continue;
        const QString base = fifoBase(path);
        if (base.isEmpty()) continue;
        Port& port = portsByBase[base];
        if (!port.spec) {
            port.spec = spec;
            port.base = base;
            port.ppuPath = ppuPathForPort(path);
            port.portQppu = portQppuIndex(path, *spec);
        }
        if (lower.endsWith(QStringLiteral(".m_num_read"))) {
            port.reads = it.value();
        } else if (lower.endsWith(
                       QStringLiteral(".m_num_written"))) {
            port.writes = it.value();
        } else if (lower.endsWith(
                       QStringLiteral(".m_num_readable"))) {
            port.readable = it.value();
        } else if (lower.endsWith(QStringLiteral(".m_size"))) {
            port.size = it.value();
        } else if (lower.endsWith(QStringLiteral(".m_ri"))) {
            port.readIndex = it.value();
        } else {
            int slot = -1;
            QString field;
            if (parseBufferField(path, slot, field)) {
                port.fieldsBySlot[slot].insert(field, it.value());
                port.inferredSlots =
                    qMax(port.inferredSlots, slot + 1);
            }
        }
    }

    QVector<Port> allPorts = portsByBase.values().toVector();
    QSet<QString> ownerEndpointKeys;
    for (const Port& port : allPorts) {
        const QString lowerBase = port.base.toLower();
        if (lowerBase.contains(QStringLiteral(".m_cbctrl.")) ||
            lowerBase.contains(QStringLiteral(".m_ppusdata."))) {
            ownerEndpointKeys.insert(
                port.ppuPath + QLatin1Char('|') +
                QLatin1String(port.spec->token));
        }
    }
    QVector<Port> ports;
    ports.reserve(allPorts.size());
    for (const Port& port : allPorts) {
        const QString ownerKey =
            port.ppuPath + QLatin1Char('|') +
            QLatin1String(port.spec->token);
        const QString lowerBase = port.base.toLower();
        const bool ownerPath =
            lowerBase.contains(QStringLiteral(".m_cbctrl.")) ||
            lowerBase.contains(QStringLiteral(".m_ppusdata."));
        if (!ownerEndpointKeys.contains(ownerKey) || ownerPath) {
            ports.push_back(port);
        }
    }
    std::sort(ports.begin(), ports.end(),
              [](const Port& left, const Port& right) {
                  return left.base < right.base;
              });
    QHash<QString, StageStats> stages;
    for (const Port& port : ports) {
        StageStats& stage =
            stages[QLatin1String(port.spec->stage)];
        stage.key = QLatin1String(port.spec->stage);
        stage.name = stageName(stage.key);
        ++stage.channels;
        accumulateOccupancy(port, startTick, endTick, stage);
    }

    if (ports.isEmpty()) {
        result.insert(QStringLiteral("status"),
                      QStringLiteral("not_covered"));
        result.insert(
            QStringLiteral("reason"),
            QStringLiteral("波形未找到 CBCtrl/CBData 关键端口。"));
        result.insert(QStringLiteral("stages"), QJsonArray());
        return result;
    }

    QVector<Cursor> cursors;
    std::priority_queue<HeapItem, QVector<HeapItem>, HeapLater> heap;
    for (int portIndex = 0; portIndex < ports.size(); ++portIndex) {
        const Port& port = ports.at(portIndex);
        const WaveSignal* eventSignal =
            port.spec->access == SlotAccess::Read
                ? port.reads
                : port.writes;
        if (!eventSignal) {
            eventSignal =
                port.spec->access == SlotAccess::Read
                    ? port.writes
                    : port.reads;
        }
        if (!eventSignal) continue;
        Cursor cursor;
        cursor.port = portIndex;
        cursor.signal = eventSignal;
        cursor.nextTick = nextActiveTick(
            eventSignal, startTick, startTick, endTick,
            ticksPerCycle);
        const int cursorIndex = cursors.size();
        cursors.push_back(cursor);
        if (cursor.nextTick < endTick) {
            heap.push({cursor.nextTick, port.spec->order,
                       cursorIndex});
        }
    }

    QHash<QString, QQueue<qint64>> pendingGrant;
    QHash<QString, QQueue<qint64>> pendingClear;
    QHash<QString, QQueue<qint64>> pendingWriteback;
    QHash<QString, QQueue<qint64>> pendingReturn;
    QHash<QString, QppuStats> qppuStats;
    QHash<int, ClientStats> clientStats;
    QHash<QString, PcStats> pcStats;
    QHash<int, qint64> instructionQueueTaken;
    QHash<int, qint64> ramTaken;
    qint64 grRamTaken = 0;
    qint64 mfifoTaken = 0;
    qint64 l1CreditTokens = 0;
    qint64 readDependencyClears = 0;
    qint64 writeDependencyClears = 0;
    qint64 dependencyClearFieldUnknown = 0;
    QHash<int, qint64> readDependencyIndexes;
    QHash<int, qint64> writeDependencyIndexes;
    QHash<int, qint64> dependencyTypes;

    qint64 requestEvents = 0;
    qint64 groupRequestEvents = 0;
    qint64 readUops = 0;
    qint64 firstUops = 0;
    qint64 operandReturns = 0;
    qint64 cbdataDispatches = 0;
    qint64 pendingClears = 0;
    qint64 writebacks = 0;
    qint64 auxiliaryDispatches = 0;
    qint64 identityEvents = 0;
    qint64 identityKnown = 0;
    qint64 firstFlagKnown = 0;
    qint64 unmatchedGrants = 0;
    qint64 unmatchedClears = 0;
    qint64 unmatchedReturns = 0;
    qint64 unmatchedWritebacks = 0;
    qint64 pointerFallbacks = 0;
    qint64 payloadFallbacks = 0;
    LatencyStats arbitrationLatency;
    LatencyStats pendingLatency;
    LatencyStats readLatency;
    LatencyStats firstWritebackLatency;

    auto countResourceArray =
        [&](const Port& port,
            int slot,
            qint64 tick,
            const QString& fieldToken,
            QHash<int, qint64>& counts) {
            const auto slotIt =
                port.fieldsBySlot.constFind(slot);
            if (slotIt == port.fieldsBySlot.constEnd()) return;
            const QRegularExpression indexExpression(
                QStringLiteral("\\.%1\\.\\[(\\d+)\\]$")
                    .arg(QRegularExpression::escape(
                        fieldToken)));
            for (auto fieldIt = slotIt->constBegin();
                 fieldIt != slotIt->constEnd(); ++fieldIt) {
                const QRegularExpressionMatch match =
                    indexExpression.match(
                        QLatin1Char('.') + fieldIt.key());
                if (!match.hasMatch()) continue;
                const SampleValue value =
                    sampleValue(fieldIt.value(), tick, false);
                if (value.known && value.value != 0) {
                    counts[match.captured(1).toInt()] =
                        saturatingAdd(
                            counts.value(
                                match.captured(1).toInt()),
                            value.value);
                }
            }
        };

    while (!heap.empty()) {
        const HeapItem item = heap.top();
        heap.pop();
        Cursor& cursor = cursors[item.cursor];
        const Port& port = ports.at(cursor.port);
        const SampleValue countValue =
            sampleValue(cursor.signal, item.tick, false);
        const quint64 rawCount =
            countValue.known ? countValue.value : 0;
        const int eventCount =
            rawCount >
                    quint64(std::numeric_limits<int>::max())
                ? std::numeric_limits<int>::max()
                : int(rawCount);
        StageStats& stage =
            stages[QLatin1String(port.spec->stage)];
        stage.events =
            saturatingAdd(stage.events, rawCount);

        for (int event = 0; event < eventCount; ++event) {
            bool pointerFallback = false;
            const int slot =
                eventSlot(port, item.tick, event,
                          pointerFallback);
            if (pointerFallback) ++pointerFallbacks;
            bool payloadFallback = false;
            const Identity identity =
                slot >= 0
                    ? identityForEvent(
                          port, slot, item.tick,
                          payloadFallback)
                    : Identity();
            if (payloadFallback) ++payloadFallbacks;
            if (identity.validKnown && !identity.valid) continue;
            const QString stageKey =
                QLatin1String(port.spec->stage);
            const QString key = instructionKey(identity);
            const QString qKey = qppuKey(identity);
            if (stageKey != QStringLiteral("credit_return") &&
                stageKey != QStringLiteral("auxiliary_dispatch")) {
                ++identityEvents;
                if (!key.isEmpty()) ++identityKnown;
            }

            if (stageKey == QStringLiteral("request_ingress")) {
                ++requestEvents;
                appendQueue(pendingGrant, key, item.tick);
                appendQueue(pendingClear, key, item.tick);
                appendQueue(pendingWriteback, key, item.tick);
                if (!qKey.isEmpty()) {
                    ++qppuStatsFor(qppuStats, identity).requests;
                }
                if (!key.isEmpty()) {
                    ++pcStatsFor(pcStats, identity).requests;
                }
            } else if (stageKey ==
                       QStringLiteral("group_ingress")) {
                ++groupRequestEvents;
            } else if (stageKey ==
                       QStringLiteral("read_issue")) {
                ++readUops;
                if (!qKey.isEmpty()) {
                    ++qppuStatsFor(qppuStats, identity).readUops;
                }
                if (identity.client >= 0) {
                    ClientStats& client =
                        clientStats[identity.client];
                    client.index = identity.client;
                    ++client.readUops;
                }
                appendQueue(
                    pendingReturn, returnKey(identity),
                    item.tick);
                if (identity.firstKnown) ++firstFlagKnown;
                if (identity.firstKnown && identity.first) {
                    ++firstUops;
                    QppuStats* qppu = nullptr;
                    PcStats* pc = nullptr;
                    if (!qKey.isEmpty()) {
                        qppu =
                            &qppuStatsFor(qppuStats, identity);
                        ++qppu->firstUops;
                    }
                    if (!key.isEmpty()) {
                        pc = &pcStatsFor(pcStats, identity);
                        ++pc->firstUops;
                    }
                    if (!consumeQueue(
                            pendingGrant, key, item.tick,
                            ticksPerCycle, arbitrationLatency,
                            qppu ? &qppu->arbitration : nullptr,
                            pc ? &pc->arbitration : nullptr)) {
                        ++unmatchedGrants;
                    }
                }
            } else if (stageKey ==
                       QStringLiteral("operand_return")) {
                ++operandReturns;
                QppuStats* qppu = nullptr;
                if (!qKey.isEmpty()) {
                    qppu =
                        &qppuStatsFor(qppuStats, identity);
                    ++qppu->returns;
                }
                ClientStats* client = nullptr;
                if (identity.client >= 0) {
                    client = &clientStats[identity.client];
                    client->index = identity.client;
                    ++client->returns;
                }
                if (!consumeQueue(
                        pendingReturn, returnKey(identity),
                        item.tick, ticksPerCycle, readLatency,
                        nullptr,
                        client ? &client->readLatency : nullptr)) {
                    ++unmatchedReturns;
                }
            } else if (stageKey ==
                       QStringLiteral("cbdata_dispatch")) {
                ++cbdataDispatches;
            } else if (stageKey ==
                       QStringLiteral("pending_clear")) {
                ++pendingClears;
                const SampleValue depType = fieldValue(
                    port, slot, {"deptype"}, item.tick, false);
                const SampleValue readValid = fieldValue(
                    port, slot, {"rdchkdepcntvld"},
                    item.tick, false);
                const SampleValue readIndex = fieldValue(
                    port, slot, {"rdchkdepcntidx"},
                    item.tick, false);
                const SampleValue writeValid = fieldValue(
                    port, slot, {"wrchkdepcntvld"},
                    item.tick, false);
                const SampleValue writeIndex = fieldValue(
                    port, slot, {"wrchkdepcntidx"},
                    item.tick, false);
                if (depType.known) {
                    dependencyTypes[int(depType.value)] =
                        saturatingAdd(
                            dependencyTypes.value(
                                int(depType.value)),
                            1);
                }
                bool clearKindKnown = false;
                if (readValid.known) {
                    clearKindKnown = true;
                    if (readValid.value != 0) {
                        ++readDependencyClears;
                        if (readIndex.known) {
                            ++readDependencyIndexes[
                                int(readIndex.value)];
                        }
                    }
                }
                if (writeValid.known) {
                    clearKindKnown = true;
                    if (writeValid.value != 0) {
                        ++writeDependencyClears;
                        if (writeIndex.known) {
                            ++writeDependencyIndexes[
                                int(writeIndex.value)];
                        }
                    }
                }
                if (!clearKindKnown) ++dependencyClearFieldUnknown;
                QppuStats* qppu = nullptr;
                PcStats* pc = nullptr;
                if (!qKey.isEmpty()) {
                    qppu =
                        &qppuStatsFor(qppuStats, identity);
                    ++qppu->clears;
                }
                if (!key.isEmpty()) {
                    pc = &pcStatsFor(pcStats, identity);
                    ++pc->clears;
                }
                if (!consumeQueue(
                        pendingClear, key, item.tick,
                        ticksPerCycle, pendingLatency,
                        qppu ? &qppu->pending : nullptr,
                        pc ? &pc->pending : nullptr)) {
                    ++unmatchedClears;
                }
            } else if (stageKey ==
                       QStringLiteral("writeback")) {
                ++writebacks;
                if (!qKey.isEmpty()) {
                    ++qppuStatsFor(qppuStats, identity)
                          .writebacks;
                }
                if (!consumeQueue(
                        pendingWriteback, key, item.tick,
                        ticksPerCycle,
                        firstWritebackLatency, nullptr,
                        nullptr)) {
                    ++unmatchedWritebacks;
                }
            } else if (stageKey ==
                       QStringLiteral("credit_return")) {
                const QString lowerBase =
                    port.base.toLower();
                if (lowerBase.contains(
                        QStringLiteral(
                            "pt_cbdata_cbctrl_credit"))) {
                    countResourceArray(
                        port, slot, item.tick,
                        QStringLiteral("inst_queue_taken"),
                        instructionQueueTaken);
                    countResourceArray(
                        port, slot, item.tick,
                        QStringLiteral("ram_taken"),
                        ramTaken);
                    const SampleValue gr = fieldValue(
                        port, slot, {"gr_ram_taken"},
                        item.tick, false);
                    const SampleValue mfifo = fieldValue(
                        port, slot, {"mfifo_taken"},
                        item.tick, false);
                    if (gr.known) {
                        grRamTaken =
                            saturatingAdd(grRamTaken, gr.value);
                    }
                    if (mfifo.known) {
                        mfifoTaken =
                            saturatingAdd(mfifoTaken,
                                          mfifo.value);
                    }
                } else {
                    const SampleValue valid = fieldValue(
                        port, slot, {"vld", "valid"},
                        item.tick, false);
                    l1CreditTokens =
                        saturatingAdd(
                            l1CreditTokens,
                            valid.known ? valid.value : 1);
                }
            } else if (stageKey ==
                       QStringLiteral("auxiliary_dispatch")) {
                ++auxiliaryDispatches;
            }
        }

        cursor.nextTick = nextActiveTick(
            cursor.signal,
            item.tick + ticksPerCycle,
            startTick, endTick, ticksPerCycle);
        if (cursor.nextTick < endTick) {
            heap.push({cursor.nextTick, port.spec->order,
                       item.cursor});
        }
    }
    arbitrationLatency.sortSamples();
    pendingLatency.sortSamples();
    readLatency.sortSamples();
    firstWritebackLatency.sortSamples();

    QJsonArray stageArray;
    QStringList stageOrder = {
        QStringLiteral("request_ingress"),
        QStringLiteral("group_ingress"),
        QStringLiteral("read_issue"),
        QStringLiteral("operand_return"),
        QStringLiteral("cbdata_dispatch"),
        QStringLiteral("writeback"),
        QStringLiteral("pending_clear"),
        QStringLiteral("credit_return"),
        QStringLiteral("auxiliary_dispatch")
    };
    for (const QString& key : stageOrder) {
        if (!stages.contains(key)) continue;
        const StageStats& stage = stages.value(key);
        QJsonObject object;
        object.insert(QStringLiteral("key"), stage.key);
        object.insert(QStringLiteral("name"), stage.name);
        object.insert(QStringLiteral("channels"), stage.channels);
        object.insert(QStringLiteral("events"), double(stage.events));
        object.insert(QStringLiteral("occupancy_channels"),
                      stage.occupancyChannels);
        if (stage.observedTicks > 0) {
            object.insert(
                QStringLiteral("events_per_channel_cycle"),
                double(stage.events) * double(ticksPerCycle) /
                    double(stage.observedTicks));
            object.insert(
                QStringLiteral("nonempty_rate_percent"),
                100.0 * double(stage.nonemptyTicks) /
                    double(stage.observedTicks));
            object.insert(
                QStringLiteral("full_rate_percent"),
                100.0 * double(stage.fullTicks) /
                    double(stage.observedTicks));
            object.insert(
                QStringLiteral("observed_channel_cycles"),
                double(stage.observedTicks) /
                    double(ticksPerCycle));
        }
        stageArray.push_back(object);
    }

    QVector<QppuStats> qppus =
        qppuStats.values().toVector();
    std::sort(qppus.begin(), qppus.end(),
              [](const QppuStats& left,
                 const QppuStats& right) {
                  if (left.requests != right.requests) {
                      return left.requests > right.requests;
                  }
                  if (left.ppuPath != right.ppuPath) {
                      return left.ppuPath < right.ppuPath;
                  }
                  return left.qppu < right.qppu;
              });
    qint64 qppuRequests = 0;
    qint64 qppuGrants = 0;
    for (const QppuStats& qppu : qppus) {
        qppuRequests += qppu.requests;
        qppuGrants += qppu.firstUops;
    }
    QJsonArray qppuArray;
    double minimumFairness =
        std::numeric_limits<double>::infinity();
    QString minimumFairnessPath;
    for (const QppuStats& qppu : qppus) {
        QJsonObject object;
        object.insert(QStringLiteral("ppu_path"), qppu.ppuPath);
        object.insert(QStringLiteral("qppu_index"), qppu.qppu);
        object.insert(
            QStringLiteral("path"),
            qppu.ppuPath +
                QStringLiteral(".m_QPPUTOP[size=4].[%1]")
                    .arg(qppu.qppu));
        object.insert(QStringLiteral("requests"),
                      double(qppu.requests));
        object.insert(QStringLiteral("first_uop_instructions"),
                      double(qppu.firstUops));
        object.insert(QStringLiteral("read_uops"),
                      double(qppu.readUops));
        object.insert(QStringLiteral("operand_returns"),
                      double(qppu.returns));
        object.insert(QStringLiteral("pending_clears"),
                      double(qppu.clears));
        object.insert(QStringLiteral("writebacks"),
                      double(qppu.writebacks));
        object.insert(
            QStringLiteral("service_ratio_percent"),
            qppu.requests > 0
                ? 100.0 * double(qppu.firstUops) /
                      double(qppu.requests)
                : 0.0);
        const double demandShare =
            qppuRequests > 0
                ? 100.0 * double(qppu.requests) /
                      double(qppuRequests)
                : 0.0;
        const double grantShare =
            qppuGrants > 0
                ? 100.0 * double(qppu.firstUops) /
                      double(qppuGrants)
                : 0.0;
        object.insert(QStringLiteral("demand_share_percent"),
                      demandShare);
        object.insert(QStringLiteral("grant_share_percent"),
                      grantShare);
        if (demandShare > 0.0 && qppu.requests >= 10) {
            const double fairness = grantShare / demandShare;
            object.insert(QStringLiteral("fairness_ratio"),
                          fairness);
            if (fairness < minimumFairness) {
                minimumFairness = fairness;
                minimumFairnessPath =
                    object.value(
                        QStringLiteral("path")).toString();
            }
        }
        addLatencyJson(object, QStringLiteral("arbitration"),
                       qppu.arbitration, false);
        addLatencyJson(object, QStringLiteral("pending"),
                       qppu.pending, false);
        qppuArray.push_back(object);
    }

    QVector<ClientStats> clients =
        clientStats.values().toVector();
    std::sort(clients.begin(), clients.end(),
              [](const ClientStats& left,
                 const ClientStats& right) {
                  if (left.readUops != right.readUops) {
                      return left.readUops > right.readUops;
                  }
                  return left.index < right.index;
              });
    QJsonArray clientArray;
    for (const ClientStats& client : clients) {
        QJsonObject object;
        object.insert(QStringLiteral("index"), client.index);
        object.insert(QStringLiteral("name"),
                      cbDataClientName(client.index));
        object.insert(QStringLiteral("read_uops"),
                      double(client.readUops));
        object.insert(QStringLiteral("operand_returns"),
                      double(client.returns));
        addLatencyJson(object, QStringLiteral("read_latency"),
                       client.readLatency, false);
        clientArray.push_back(object);
    }

    QVector<PcStats> pcs = pcStats.values().toVector();
    std::sort(pcs.begin(), pcs.end(),
              [](const PcStats& left, const PcStats& right) {
                  if (left.pending.maximum !=
                      right.pending.maximum) {
                      return left.pending.maximum >
                             right.pending.maximum;
                  }
                  if (left.arbitration.maximum !=
                      right.arbitration.maximum) {
                      return left.arbitration.maximum >
                             right.arbitration.maximum;
                  }
                  if (left.requests != right.requests) {
                      return left.requests > right.requests;
                  }
                  return left.pc < right.pc;
              });
    QJsonArray pcArray;
    for (int index = 0;
         index < qMin(50, pcs.size()); ++index) {
        const PcStats& pc = pcs.at(index);
        QJsonObject object;
        object.insert(QStringLiteral("ppu_path"), pc.ppuPath);
        object.insert(QStringLiteral("qppu_index"), pc.qppu);
        object.insert(QStringLiteral("sg_index"), pc.sg);
        object.insert(QStringLiteral("pc"), pcText(pc.pc));
        object.insert(QStringLiteral("requests"),
                      double(pc.requests));
        object.insert(QStringLiteral("first_uop_instructions"),
                      double(pc.firstUops));
        object.insert(QStringLiteral("pending_clears"),
                      double(pc.clears));
        addLatencyJson(object, QStringLiteral("arbitration"),
                       pc.arbitration, false);
        addLatencyJson(object, QStringLiteral("pending"),
                       pc.pending, false);
        pcArray.push_back(object);
    }

    auto resourceArray =
        [](const QHash<int, qint64>& counts,
           bool instructionQueue) {
            QVector<QPair<int, qint64>> entries;
            for (auto it = counts.constBegin();
                 it != counts.constEnd(); ++it) {
                if (it.value() > 0) {
                    entries.push_back(
                        qMakePair(it.key(), it.value()));
                }
            }
            std::sort(
                entries.begin(), entries.end(),
                [](const QPair<int, qint64>& left,
                   const QPair<int, qint64>& right) {
                    if (left.second != right.second) {
                        return left.second > right.second;
                    }
                    return left.first < right.first;
                });
            QJsonArray array;
            for (const auto& entry : entries) {
                QJsonObject object;
                object.insert(QStringLiteral("index"),
                              entry.first);
                object.insert(
                    QStringLiteral("name"),
                    instructionQueue
                        ? cbDataInstQueueClientName(
                              entry.first)
                        : cbDataClientName(entry.first));
                object.insert(QStringLiteral("taken_events"),
                              double(entry.second));
                array.push_back(object);
            }
            return array;
        };

    QJsonObject summary;
    summary.insert(QStringLiteral("request_events"),
                   double(requestEvents));
    summary.insert(QStringLiteral("group_request_events"),
                   double(groupRequestEvents));
    summary.insert(QStringLiteral("read_uops"),
                   double(readUops));
    summary.insert(QStringLiteral("first_uop_instructions"),
                   double(firstUops));
    summary.insert(QStringLiteral("operand_return_events"),
                   double(operandReturns));
    summary.insert(QStringLiteral("cbdata_dispatch_events"),
                   double(cbdataDispatches));
    summary.insert(QStringLiteral("pending_clear_events"),
                   double(pendingClears));
    summary.insert(QStringLiteral("writeback_events"),
                   double(writebacks));
    summary.insert(QStringLiteral("auxiliary_dispatch_events"),
                   double(auxiliaryDispatches));
    summary.insert(
        QStringLiteral("request_service_ratio_percent"),
        requestEvents > 0
            ? 100.0 * double(firstUops) /
                  double(requestEvents)
            : 0.0);
    addLatencyJson(summary, QStringLiteral("arbitration_wait"),
                   arbitrationLatency, true);
    addLatencyJson(summary, QStringLiteral("pending_lifetime"),
                   pendingLatency, true);
    addLatencyJson(summary, QStringLiteral("eu_read_latency"),
                   readLatency, true);
    addLatencyJson(summary, QStringLiteral("first_writeback"),
                   firstWritebackLatency, true);
    summary.insert(QStringLiteral("unmatched_first_uops"),
                   double(unmatchedGrants));
    summary.insert(QStringLiteral("unmatched_pending_clears"),
                   double(unmatchedClears));
    summary.insert(QStringLiteral("unmatched_operand_returns"),
                   double(unmatchedReturns));
    summary.insert(QStringLiteral("unmatched_writebacks"),
                   double(unmatchedWritebacks));

    QJsonObject coverage;
    coverage.insert(QStringLiteral("endpoint_channels"),
                    ports.size());
    coverage.insert(QStringLiteral("event_channels"),
                    cursors.size());
    coverage.insert(QStringLiteral("identity_events"),
                    double(identityEvents));
    coverage.insert(QStringLiteral("identity_known_events"),
                    double(identityKnown));
    coverage.insert(
        QStringLiteral("identity_coverage_percent"),
        identityEvents > 0
            ? 100.0 * double(identityKnown) /
                  double(identityEvents)
            : 0.0);
    coverage.insert(QStringLiteral("first_uop_flag_events"),
                    double(firstFlagKnown));
    coverage.insert(
        QStringLiteral("first_uop_flag_coverage_percent"),
        readUops > 0
            ? 100.0 * double(firstFlagKnown) /
                  double(readUops)
            : 0.0);
    coverage.insert(QStringLiteral("pointer_boundary_fallbacks"),
                    double(pointerFallbacks));
    coverage.insert(QStringLiteral("payload_boundary_fallbacks"),
                    double(payloadFallbacks));
    coverage.insert(
        QStringLiteral("latency_pairing_is_fifo_ordered"),
        true);
    QJsonArray channelCoverage;
    for (const Port& port : ports) {
        auto hasField =
            [&](std::initializer_list<const char*> names) {
                for (auto it = port.fieldsBySlot.constBegin();
                     it != port.fieldsBySlot.constEnd(); ++it) {
                    if (fieldSignal(port, it.key(), names)) return true;
                }
                return false;
            };
        int payloadFields = 0;
        for (auto it = port.fieldsBySlot.constBegin();
             it != port.fieldsBySlot.constEnd(); ++it) {
            payloadFields += it.value().size();
        }
        QJsonObject channel;
        channel.insert(QStringLiteral("stage"),
                       QLatin1String(port.spec->stage));
        channel.insert(QStringLiteral("path"), port.base);
        channel.insert(
            QStringLiteral("event_counter"),
            (port.spec->access == SlotAccess::Read
                     ? port.reads
                     : port.writes)
                ? (port.spec->access == SlotAccess::Read
                       ? QStringLiteral("m_num_read")
                       : QStringLiteral("m_num_written"))
                : QStringLiteral("fallback"));
        channel.insert(QStringLiteral("payload_slots"),
                       port.fieldsBySlot.size());
        channel.insert(QStringLiteral("payload_fields"),
                       payloadFields);
        channel.insert(QStringLiteral("has_pc"),
                       hasField({"pc_", "pc"}));
        channel.insert(QStringLiteral("has_qppu"),
                       hasField(
                           {"qppu_id", "qppu_idx", "qppuid"}) ||
                           port.portQppu >= 0);
        channel.insert(QStringLiteral("has_sg"),
                       hasField(
                           {"sg_id", "sgid", "local_sg_id"}));
        channel.insert(QStringLiteral("has_client"),
                       hasField(
                           {"client", "client_type",
                            "instr_type"}));
        channel.insert(QStringLiteral("has_first_uop"),
                       hasField(
                           {"is_first_uop", "first_uop"}));
        channelCoverage.push_back(channel);
    }
    coverage.insert(QStringLiteral("channels"), channelCoverage);

    QJsonObject resources;
    resources.insert(
        QStringLiteral("instruction_queues"),
        resourceArray(instructionQueueTaken, true));
    resources.insert(QStringLiteral("ram_clients"),
                     resourceArray(ramTaken, false));
    resources.insert(QStringLiteral("gr_ram_taken_events"),
                     double(grRamTaken));
    resources.insert(QStringLiteral("mfifo_taken_events"),
                     double(mfifoTaken));
    resources.insert(QStringLiteral("l1_credit_tokens"),
                     double(l1CreditTokens));
    resources.insert(
        QStringLiteral("interpretation"),
        QStringLiteral(
            "taken 表示本周期资源被占用/返还的活动量，不等同于"
            "可用 Credit 余额，也不能单独证明 Credit 耗尽。"));

    auto dependencyIndexArray =
        [](const QHash<int, qint64>& counts) {
            QVector<QPair<int, qint64>> entries;
            for (auto it = counts.constBegin();
                 it != counts.constEnd(); ++it) {
                entries.push_back(
                    qMakePair(it.key(), it.value()));
            }
            std::sort(
                entries.begin(), entries.end(),
                [](const QPair<int, qint64>& left,
                   const QPair<int, qint64>& right) {
                    if (left.second != right.second) {
                        return left.second > right.second;
                    }
                    return left.first < right.first;
                });
            QJsonArray array;
            for (const auto& entry : entries) {
                QJsonObject object;
                object.insert(QStringLiteral("index"),
                              entry.first);
                object.insert(QStringLiteral("events"),
                              double(entry.second));
                array.push_back(object);
            }
            return array;
        };
    QJsonArray dependencyTypeArray;
    QVector<int> dependencyTypeKeys =
        dependencyTypes.keys().toVector();
    std::sort(dependencyTypeKeys.begin(),
              dependencyTypeKeys.end());
    for (int type : dependencyTypeKeys) {
        QJsonObject object;
        object.insert(QStringLiteral("value"), type);
        object.insert(QStringLiteral("events"),
                      double(dependencyTypes.value(type)));
        dependencyTypeArray.push_back(object);
    }
    QJsonObject pendingClearDetail;
    pendingClearDetail.insert(
        QStringLiteral("read_dependency_events"),
        double(readDependencyClears));
    pendingClearDetail.insert(
        QStringLiteral("write_dependency_events"),
        double(writeDependencyClears));
    pendingClearDetail.insert(
        QStringLiteral("kind_unknown_events"),
        double(dependencyClearFieldUnknown));
    pendingClearDetail.insert(
        QStringLiteral("read_counter_indexes"),
        dependencyIndexArray(readDependencyIndexes));
    pendingClearDetail.insert(
        QStringLiteral("write_counter_indexes"),
        dependencyIndexArray(writeDependencyIndexes));
    pendingClearDetail.insert(
        QStringLiteral("dependency_types"),
        dependencyTypeArray);
    pendingClearDetail.insert(
        QStringLiteral("interpretation"),
        QStringLiteral(
            "CBCtrl -> BE 端口在源码中用于清除读 ChkDep pending；"
            "若波形出现写有效位则原样报告，不擅自改写语义。"));

    auto stageFullRate =
        [&](const QString& key) {
            const StageStats stage = stages.value(key);
            return stage.observedTicks > 0
                ? 100.0 * double(stage.fullTicks) /
                      double(stage.observedTicks)
                : 0.0;
        };
    const double requestFull =
        stageFullRate(QStringLiteral("request_ingress"));
    const double cbdataFull =
        stageFullRate(QStringLiteral("cbdata_dispatch"));
    const double writebackFull =
        stageFullRate(QStringLiteral("writeback"));
    const double serviceRatio =
        requestEvents > 0
            ? 100.0 * double(firstUops) /
                  double(requestEvents)
            : 100.0;
    const bool firstUopCoverageComplete =
        readUops > 0 && firstFlagKnown == readUops;
    const bool hasCbActivity =
        requestEvents + groupRequestEvents + readUops +
            operandReturns + cbdataDispatches +
            pendingClears + writebacks +
            auxiliaryDispatches >
        0;

    QJsonObject bottleneck;
    QString bottleneckStage = QStringLiteral("none");
    QString bottleneckSeverity = QStringLiteral("info");
    QString bottleneckConfidence = QStringLiteral("low");
    QString bottleneckReason =
        QStringLiteral("现有 CBCtrl 阶段指标未形成明确瓶颈证据。");
    QString bottleneckEvidence =
        QStringLiteral("请求 %1，首 uop %2，Pending 清除 %3。")
            .arg(requestEvents)
            .arg(firstUops)
            .arg(pendingClears);
    bool directEvidence = false;
    if (hasCbActivity && firstUopCoverageComplete &&
        requestEvents >= 10 &&
        requestFull >= 10.0 &&
        serviceRatio < 90.0) {
        bottleneckStage = QStringLiteral("request_ingress");
        bottleneckSeverity =
            requestFull >= 30.0
                ? QStringLiteral("critical")
                : QStringLiteral("warning");
        bottleneckConfidence = QStringLiteral("high");
        bottleneckReason =
            QStringLiteral(
                "CBCtrl 指令入口出现可直接观测的 FIFO 满状态，且首 uop "
                "服务量落后于请求量。");
        bottleneckEvidence =
            QStringLiteral("入口满率 %1%，请求服务率 %2%。")
                .arg(requestFull, 0, 'f', 1)
                .arg(serviceRatio, 0, 'f', 1);
        directEvidence = true;
    } else if (hasCbActivity &&
               arbitrationLatency.count >= 10 &&
               arbitrationLatency.percentile(0.95) >= 8.0 &&
               serviceRatio < 95.0) {
        bottleneckStage = QStringLiteral("read_arbitration");
        bottleneckSeverity = QStringLiteral("warning");
        bottleneckConfidence = QStringLiteral("medium");
        bottleneckReason =
            QStringLiteral(
                "请求到首个寄存器读 uop 的等待较长；波形能定位到"
                " CBCtrl 仲裁/资源等待阶段，但不能区分内部具体拒绝条件。");
        bottleneckEvidence =
            QStringLiteral("仲裁等待 P95 %1 周期，平均 %2 周期，服务率 %3%。")
                .arg(arbitrationLatency.percentile(0.95),
                     0, 'f', 1)
                .arg(arbitrationLatency.average(),
                     0, 'f', 1)
                .arg(serviceRatio, 0, 'f', 1);
    } else if (hasCbActivity && readLatency.count >= 10 &&
               readLatency.percentile(0.95) >= 16.0) {
        bottleneckStage = QStringLiteral("eu_operand_return");
        bottleneckSeverity = QStringLiteral("warning");
        bottleneckConfidence = QStringLiteral("medium");
        bottleneckReason =
            QStringLiteral(
                "寄存器读 uop 到 EU 操作数返回的延迟较长，限制位于"
                "读请求之后、执行接收之前。");
        bottleneckEvidence =
            QStringLiteral("EU 读返回 P95 %1 周期，平均 %2 周期。")
                .arg(readLatency.percentile(0.95),
                     0, 'f', 1)
                .arg(readLatency.average(), 0, 'f', 1);
    } else if (hasCbActivity &&
               cbdataDispatches + writebacks >= 10 &&
               (cbdataFull >= 10.0 ||
                writebackFull >= 10.0)) {
        bottleneckStage =
            cbdataFull >= writebackFull
                ? QStringLiteral("cbdata_dispatch")
                : QStringLiteral("writeback");
        bottleneckSeverity =
            qMax(cbdataFull, writebackFull) >= 30.0
                ? QStringLiteral("critical")
                : QStringLiteral("warning");
        bottleneckConfidence = QStringLiteral("high");
        bottleneckReason =
            QStringLiteral(
                "CBData 下发或写回 FIFO 出现可直接观测的满状态。");
        bottleneckEvidence =
            QStringLiteral("CBData 下发满率 %1%，写回满率 %2%。")
                .arg(cbdataFull, 0, 'f', 1)
                .arg(writebackFull, 0, 'f', 1);
        directEvidence = true;
    } else if (hasCbActivity &&
               pendingLatency.count >= 10 &&
               pendingLatency.percentile(0.95) >= 32.0) {
        bottleneckStage = QStringLiteral("pending_lifetime");
        bottleneckSeverity = QStringLiteral("warning");
        bottleneckConfidence = QStringLiteral("medium");
        bottleneckReason =
            QStringLiteral(
                "CBCtrl 指令从进入到清除 Pending 的生命周期较长，"
                "但缺少内部仲裁原因，当前只能定位到 CBCtrl/CBData "
                "完成链。");
        bottleneckEvidence =
            QStringLiteral("Pending 生命周期 P95 %1 周期，平均 %2 周期。")
                .arg(pendingLatency.percentile(0.95),
                     0, 'f', 1)
                .arg(pendingLatency.average(), 0, 'f', 1);
    } else if (hasCbActivity && firstUopCoverageComplete &&
               std::isfinite(minimumFairness) &&
               minimumFairness < 0.50) {
        bottleneckStage = QStringLiteral("qppu_fairness");
        bottleneckSeverity = QStringLiteral("warning");
        bottleneckConfidence = QStringLiteral("medium");
        bottleneckReason =
            QStringLiteral(
                "某 QPPU 的首 uop 份额明显低于其请求份额，存在"
                " CBCtrl 仲裁公平性风险。");
        bottleneckEvidence =
            QStringLiteral("%1 的 grant/demand 份额比为 %2。")
                .arg(minimumFairnessPath)
                .arg(minimumFairness, 0, 'f', 2);
    }
    bottleneck.insert(QStringLiteral("stage"),
                      bottleneckStage);
    static const QHash<QString, QString> bottleneckStageNames = {
        {QStringLiteral("none"), QStringLiteral("未发现")},
        {QStringLiteral("request_ingress"), QStringLiteral("请求入口")},
        {QStringLiteral("read_arbitration"), QStringLiteral("读仲裁/资源等待")},
        {QStringLiteral("eu_operand_return"), QStringLiteral("EU 操作数返回")},
        {QStringLiteral("cbdata_dispatch"), QStringLiteral("CBData 下发")},
        {QStringLiteral("writeback"), QStringLiteral("CBData 写回")},
        {QStringLiteral("pending_lifetime"), QStringLiteral("Pending 完成链")},
        {QStringLiteral("qppu_fairness"), QStringLiteral("QPPU 仲裁公平性")}
    };
    bottleneck.insert(
        QStringLiteral("stage_name"),
        bottleneckStageNames.value(
            bottleneckStage, bottleneckStage));
    bottleneck.insert(QStringLiteral("severity"),
                      bottleneckSeverity);
    bottleneck.insert(QStringLiteral("confidence"),
                      bottleneckConfidence);
    bottleneck.insert(QStringLiteral("reason"),
                      bottleneckReason);
    bottleneck.insert(QStringLiteral("evidence"),
                      bottleneckEvidence);
    bottleneck.insert(QStringLiteral("direct_evidence"),
                      directEvidence);

    QString status;
    if (!hasDynamicWave && requestEvents == 0 &&
        readUops == 0 && pendingClears == 0) {
        status = QStringLiteral("static_snapshot");
    } else if (requestEvents == 0 && readUops == 0 &&
               pendingClears == 0) {
        status = QStringLiteral("no_activity");
    } else {
        const bool identityCovered =
            identityEvents == 0 ||
            identityKnown == identityEvents;
        const bool firstCovered =
            readUops == 0 || firstFlagKnown == readUops;
        status = identityCovered && firstCovered &&
                         pointerFallbacks == 0 &&
                         payloadFallbacks == 0
                     ? QStringLiteral("measured")
                     : QStringLiteral("partial");
    }
    result.insert(QStringLiteral("status"), status);
    result.insert(QStringLiteral("summary"), summary);
    result.insert(QStringLiteral("stages"), stageArray);
    result.insert(QStringLiteral("qppus"), qppuArray);
    result.insert(QStringLiteral("clients"), clientArray);
    result.insert(QStringLiteral("pc_hotspots"), pcArray);
    result.insert(QStringLiteral("resource_taken"), resources);
    result.insert(QStringLiteral("pending_clear_detail"),
                  pendingClearDetail);
    result.insert(QStringLiteral("coverage"), coverage);
    result.insert(QStringLiteral("bottleneck"), bottleneck);

    QJsonArray localWarnings;
    auto addWarning = [&](const QString& warning) {
        localWarnings.push_back(warning);
        warnings.push_back(warning);
    };
    if (status == QStringLiteral("partial")) {
        addWarning(
            QStringLiteral(
                "CBCtrl payload 或 first_uop 覆盖不完整；事件总数可用，"
                "身份归因和延迟只代表已配对部分。"));
    }
    if (pointerFallbacks > 0 || payloadFallbacks > 0) {
        addWarning(
            QStringLiteral(
                "分析窗口起点存在 %1 次指针、%2 次 payload 边界回退；"
                "对应事件没有严格的事件前快照。")
                .arg(pointerFallbacks)
                .arg(payloadFallbacks));
    }
    if (unmatchedGrants + unmatchedClears +
            unmatchedReturns + unmatchedWritebacks >
        0) {
        addWarning(
            QStringLiteral(
                "CBCtrl 阶段关联存在未配对事件：首 uop %1、Pending 清除 %2、"
                "EU 返回 %3、写回 %4；常见原因是分析窗口从在途指令中间开始"
                "或身份字段未覆盖。")
                .arg(unmatchedGrants)
                .arg(unmatchedClears)
                .arg(unmatchedReturns)
                .arg(unmatchedWritebacks));
    }
    result.insert(QStringLiteral("warnings"),
                  localWarnings);
    return result;
}

WaveSignal makeSignal(
    int id,
    int width,
    std::initializer_list<QPair<qint64, quint64>> samples) {
    WaveSignal signal;
    signal.signalId = id;
    signal.kind = width == 1 ? SignalKind::Bit : SignalKind::Bus;
    signal.width = width;
    for (const auto& pair : samples) {
        WaveSample sample;
        sample.time = pair.first;
        sample.rawBits = pair.second;
        sample.rawFieldsReady = true;
        signal.samples.push_back(sample);
    }
    return signal;
}

}  // namespace

bool isCBCtrlDetailLeafName(const QByteArray& lowerName) {
    static const QSet<QByteArray> names = {
        QByteArrayLiteral("pc"),
        QByteArrayLiteral("pc_"),
        QByteArrayLiteral("qppu_id"),
        QByteArrayLiteral("qppu_idx"),
        QByteArrayLiteral("qppuid"),
        QByteArrayLiteral("sg_id"),
        QByteArrayLiteral("sgid"),
        QByteArrayLiteral("local_sg_id"),
        QByteArrayLiteral("client"),
        QByteArrayLiteral("client_type"),
        QByteArrayLiteral("ram_client"),
        QByteArrayLiteral("mux_id"),
        QByteArrayLiteral("src_idx"),
        QByteArrayLiteral("reg_idx"),
        QByteArrayLiteral("tr_index"),
        QByteArrayLiteral("phase"),
        QByteArrayLiteral("num_tr_data"),
        QByteArrayLiteral("is_first_uop"),
        QByteArrayLiteral("is_last_uop"),
        QByteArrayLiteral("first_uop"),
        QByteArrayLiteral("last_uop"),
        QByteArrayLiteral("valid"),
        QByteArrayLiteral("vld"),
        QByteArrayLiteral("deptype"),
        QByteArrayLiteral("rdchkdepcntidx"),
        QByteArrayLiteral("rdchkdepcntvld"),
        QByteArrayLiteral("wrchkdepcntidx"),
        QByteArrayLiteral("wrchkdepcntvld"),
        QByteArrayLiteral("instr_type"),
        QByteArrayLiteral("gr_ram_taken"),
        QByteArrayLiteral("mfifo_taken"),
        QByteArrayLiteral("inst_queue_taken"),
        QByteArrayLiteral("ram_taken")
    };
    return names.contains(lowerName);
}

bool isCBCtrlEndpointSegment(const QByteArray& lowerName) {
    QByteArray base = lowerName;
    const int annotation = base.indexOf('[');
    if (annotation >= 0) base.truncate(annotation);
    for (const EndpointSpec& spec : endpointSpecs()) {
        if (base == spec.token) return true;
    }
    return false;
}

bool isCBCtrlDetailSignalPath(const QString& path) {
    const QString lower = path.toLower();
    if (!endpointForPath(lower)) return false;
    if (lower.endsWith(QStringLiteral(".m_num_read")) ||
        lower.endsWith(QStringLiteral(".m_num_written")) ||
        lower.endsWith(QStringLiteral(".m_num_readable")) ||
        lower.endsWith(QStringLiteral(".m_size")) ||
        lower.endsWith(QStringLiteral(".m_ri"))) {
        return true;
    }
    int slot = -1;
    QString field;
    if (!parseBufferField(path, slot, field)) return false;
    const QString leaf =
        field.section(QLatin1Char('.'), -1);
    if (isCBCtrlDetailLeafName(leaf.toLatin1())) return true;
    return field.contains(QStringLiteral("inst_queue_taken")) ||
           field.contains(QStringLiteral("ram_taken"));
}

QJsonObject buildCBCtrlProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    bool hasDynamicWave,
    QStringList& warnings) {
    if (ticksPerCycle <= 0 || endTick <= startTick) {
        QJsonObject result;
        result.insert(QStringLiteral("status"),
                      QStringLiteral("invalid_range"));
        result.insert(QStringLiteral("reason"),
                      QStringLiteral("CBCtrl 分析区间或业务周期无效。"));
        return result;
    }
    return buildProfile(
        signalsByPath, startTick, endTick,
        ticksPerCycle, hasDynamicWave, warnings);
}

bool cbCtrlProfilerSelfTest(QString& error) {
    QVector<WaveSignal> signalStorage;
    QStringList paths;
    signalStorage.reserve(64);
    paths.reserve(64);
    int nextId = 1;
    auto append =
        [&](const QString& path,
            int width,
            std::initializer_list<QPair<qint64, quint64>> values) {
            signalStorage.push_back(
                makeSignal(nextId++, width, values));
            paths.push_back(path);
        };

    const QString ppu =
        QStringLiteral("gpu.m_ppu[size=1].[0]");
    const QString request =
        ppu + QStringLiteral(
                  ".m_CBCtrl.pt_BE_CBCtrl_new_inst[size=4].[0]");
    const QString uop =
        ppu + QStringLiteral(
                  ".m_CBCtrl.pt_CBCtrl_ThdCore_uop[size=4].[0]");
    const QString clear =
        ppu + QStringLiteral(
                  ".m_CBCtrl.pt_CBCtrl_BE_clr_pending[size=4].[0]");
    auto appendReadPort =
        [&](const QString& base) {
            append(base + QStringLiteral(".m_num_read"), 4,
                   {{0, 0}, {10, 1}, {30, 0}});
            append(base + QStringLiteral(".m_num_written"), 4,
                   {{0, 0}});
            append(base + QStringLiteral(".m_num_readable"), 5,
                   {{0, 2}, {10, 1}, {20, 0}});
            append(base + QStringLiteral(".m_size"), 5,
                   {{0, 2}});
            append(base + QStringLiteral(".m_ri"), 5,
                   {{0, 0}, {10, 1}, {20, 0}});
        };
    appendReadPort(request);
    auto appendWritePort =
        [&](const QString& base, qint64 firstTick) {
            append(base + QStringLiteral(".m_num_read"), 4,
                   {{0, 0}});
            append(base + QStringLiteral(".m_num_written"), 4,
                   {{0, 0}, {firstTick, 1},
                    {firstTick + 20, 0}});
            append(base + QStringLiteral(".m_num_readable"), 5,
                   {{0, 0}});
            append(base + QStringLiteral(".m_size"), 5,
                   {{0, 2}});
            append(base + QStringLiteral(".m_ri"), 5,
                   {{0, 0}, {firstTick, 1},
                    {firstTick + 10, 0}});
        };
    appendWritePort(uop, 20);
    appendWritePort(clear, 30);

    auto appendIdentity =
        [&](const QString& base,
            int slot,
            quint64 pc,
            bool includeFirst) {
            const QString item =
                base + QStringLiteral(
                           ".m_buf[size=2].[%1].")
                           .arg(slot);
            append(item + QStringLiteral("PC"), 64,
                   {{0, pc}});
            append(item + QStringLiteral("sg_id"), 5,
                   {{0, 3}});
            append(item + QStringLiteral("qppu_id"), 3,
                   {{0, 0}});
            append(item + QStringLiteral("valid"), 1,
                   {{0, 1}});
            if (includeFirst) {
                append(item + QStringLiteral("is_first_uop"),
                       1, {{0, 1}});
                append(item + QStringLiteral("client"), 5,
                       {{0, 4}});
                append(item + QStringLiteral("mux_id"), 4,
                       {{0, quint64(slot)}});
            }
        };
    for (int slot = 0; slot < 2; ++slot) {
        const quint64 pc = 0x100 + quint64(slot) * 4;
        appendIdentity(request, slot, pc, false);
        appendIdentity(uop, slot, pc, true);
        appendIdentity(clear, slot, pc, false);
        const QString clearItem =
            clear + QStringLiteral(
                        ".m_buf[size=2].[%1].")
                        .arg(slot);
        append(clearItem + QStringLiteral("depType"), 3,
               {{0, 0}});
        append(clearItem + QStringLiteral("rdChkDepCntVld"), 1,
               {{0, 1}});
        append(clearItem + QStringLiteral("rdChkDepCntIdx"), 4,
               {{0, quint64(slot)}});
        append(clearItem + QStringLiteral("wrChkDepCntVld"), 1,
               {{0, 0}});
    }

    QHash<QString, const WaveSignal*> byPath;
    for (int index = 0; index < signalStorage.size(); ++index) {
        byPath.insert(paths.at(index), &signalStorage.at(index));
    }
    QStringList warnings;
    const QJsonObject profile =
        buildCBCtrlProfile(byPath, 0, 60, 10, true,
                           warnings);
    const QJsonObject summary =
        profile.value(QStringLiteral("summary")).toObject();
    const QJsonArray qppus =
        profile.value(QStringLiteral("qppus")).toArray();
    const QJsonObject clearDetail =
        profile.value(
            QStringLiteral("pending_clear_detail")).toObject();
    if (profile.value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        summary.value(QStringLiteral("request_events")).toDouble() !=
            2.0 ||
        summary.value(
            QStringLiteral("first_uop_instructions")).toDouble() !=
            2.0 ||
        summary.value(
            QStringLiteral("arbitration_wait_average_cycles")).toDouble() !=
            1.0 ||
        summary.value(
            QStringLiteral("pending_lifetime_average_cycles")).toDouble() !=
            2.0 ||
        clearDetail.value(
            QStringLiteral("read_dependency_events")).toDouble() !=
            2.0 ||
        qppus.size() != 1 ||
        qppus.first().toObject()
                .value(QStringLiteral("requests")).toDouble() !=
            2.0) {
        error = QStringLiteral(
            "CBCtrl FIFO event/payload correlation mismatch: "
            "status=%1 request=%2 first=%3 arb=%4 pending=%5 qppu=%6")
                    .arg(profile.value(
                             QStringLiteral("status")).toString())
                    .arg(summary.value(
                             QStringLiteral("request_events")).toDouble())
                    .arg(summary.value(
                             QStringLiteral(
                                 "first_uop_instructions")).toDouble())
                    .arg(summary.value(
                             QStringLiteral(
                                 "arbitration_wait_average_cycles")).toDouble())
                    .arg(summary.value(
                             QStringLiteral(
                                 "pending_lifetime_average_cycles")).toDouble())
                    .arg(qppus.size());
        return false;
    }
    return true;
}

}  // namespace waveperf
