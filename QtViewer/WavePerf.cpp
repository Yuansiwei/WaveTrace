#include "WaveParser4.h"
#include "WavePerfArchitecture.h"
#include "WavePerfBandwidth.h"
#include "WavePerfDiagnosis.h"
#include "WavePerfOutput.h"
#include "WavePerfScheduler.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

class WaveParseProgressBar {
public:
    explicit WaveParseProgressBar(bool enabled) : enabled_(enabled) {}

    bool enabled() const { return enabled_; }

    void start() {
        if (!enabled_) return;
        started_ = true;
        render(0, 1, true);
    }

    void update(quint64 completedBlocks, quint64 totalBlocks) {
        if (!enabled_) return;
        started_ = true;
        totalBlocks_ = qMax<quint64>(1, totalBlocks);
        completedBlocks_ = qMin(completedBlocks, totalBlocks_);
        render(completedBlocks_, totalBlocks_, false);
    }

    void finish(bool success) {
        if (!enabled_ || !started_) return;
        if (success) render(totalBlocks_, totalBlocks_, false);
        std::fputc('\n', stderr);
        std::fflush(stderr);
        started_ = false;
    }

private:
    void render(quint64 completedBlocks, quint64 totalBlocks, bool force) {
        const int percent = totalBlocks == 0
            ? 100
            : int(qMin<quint64>(100, completedBlocks * 100 / totalBlocks));
        if (!force && percent == lastPercent_ &&
            completedBlocks == lastCompletedBlocks_ &&
            totalBlocks == lastTotalBlocks_) {
            return;
        }
        lastPercent_ = percent;
        lastCompletedBlocks_ = completedBlocks;
        lastTotalBlocks_ = totalBlocks;

        constexpr int barWidth = 32;
        const int filled = percent * barWidth / 100;
        char bar[barWidth + 1];
        for (int i = 0; i < barWidth; ++i) {
            if (i < filled) {
                bar[i] = '=';
            } else if (i == filled && percent < 100) {
                bar[i] = '>';
            } else {
                bar[i] = ' ';
            }
        }
        bar[barWidth] = '\0';
        std::fprintf(stderr, "\rParsing waveform [%s] %3d%%  %llu/%llu blocks",
                     bar, percent,
                     static_cast<unsigned long long>(completedBlocks),
                     static_cast<unsigned long long>(totalBlocks));
        std::fflush(stderr);
    }

    bool enabled_ = false;
    bool started_ = false;
    int lastPercent_ = -1;
    quint64 lastCompletedBlocks_ = 0;
    quint64 lastTotalBlocks_ = 0;
    quint64 completedBlocks_ = 0;
    quint64 totalBlocks_ = 1;
};

void printProgressStage(bool enabled, const char* message) {
    if (!enabled) return;
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
}

struct Options {
    QString filePath;
    QString outputDirectory;
    qint64 startCycle = -1;
    qint64 endCycle = -1;
    qint64 ticksPerCycle = 10;
    quint64 maxDecodedSamples = 100000000ull;
    int maxSignals = 2000000;
    int timelineBins = 160;
    bool showProgress = true;
    bool dumpPredecodeFields = false;
};

struct PredecodeFieldCatalogEntry {
    int signalCount = 0;
    QSet<int> widths;
    QString examplePath;
};

int dumpPredecodeFieldCatalog(const WaveFile& directory) {
    QMap<QString, PredecodeFieldCatalogEntry> fields;
    QStringList fencePaths;
    for (int i = 0; i < directory.signalList.size(); ++i) {
        const QString path = waveSignalFullPath(directory, i);
        static const QString marker = QStringLiteral(".preDecode.");
        const int markerIndex = path.indexOf(marker, 0, Qt::CaseInsensitive);
        if (markerIndex < 0) continue;

        const QString fieldPath = path.mid(markerIndex + marker.size());
        if (fieldPath.compare(QStringLiteral("isFence"),
                              Qt::CaseInsensitive) == 0) {
            fencePaths.push_back(path);
        }
        PredecodeFieldCatalogEntry& entry = fields[fieldPath];
        ++entry.signalCount;
        entry.widths.insert(directory.signalList.at(i).width);
        if (entry.examplePath.isEmpty()) entry.examplePath = path;
    }

    QTextStream out(stdout);
    out << "preDecode field catalog: " << fields.size()
        << " unique field path(s)\n";
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        QList<int> widths = it.value().widths.values();
        std::sort(widths.begin(), widths.end());
        QStringList widthText;
        for (int width : widths) widthText.push_back(QString::number(width));
        out << it.key()
            << "\tcount=" << it.value().signalCount
            << "\twidth=" << widthText.join(QLatin1Char(','))
            << "\texample=" << it.value().examplePath << '\n';
    }
    out << "\nisFence full paths: " << fencePaths.size() << '\n';
    for (const QString& path : fencePaths) out << path << '\n';
    out.flush();
    return 0;
}

struct SignalSelection {
    int directoryIndex = -1;
    int signalId = -1;
    QString path;
    QString key;
    QString category;
    bool helper = false;
    waveperf::EventSemantics eventSemantics =
        waveperf::EventSemantics::None;
};

struct ValueState {
    bool known = false;
    quint64 value = 0;
};

struct FullWindow {
    qint64 fullTicks = 0;
    qint64 knownTicks = 0;
    qint64 expectedTicks = 0;
};

struct SignalMetrics {
    SignalSelection selection;
    int width = 0;
    int samplesInRange = 0;
    qint64 transitionCount = 0;
    qint64 activeTicks = 0;
    qint64 unknownTicks = 0;
    bool hasNumericValue = false;
    quint64 minValue = 0;
    quint64 maxValue = 0;
    quint64 lastValue = 0;
    long double weightedValueTicks = 0.0L;
    long double populationWeightedTicks = 0.0L;
    qint64 knownTicks = 0;
    quint64 positiveDelta = 0;
    qint64 decreaseEvents = 0;
};

struct IssueSlot {
    QString rootPath;
    QString contextPath;
    int slotIndex = -1;
    int slotCount = 0;
    const WaveSignal* valid = nullptr;
    const WaveSignal* mainType = nullptr;
    const WaveSignal* issueType = nullptr;
    const WaveSignal* globalMem = nullptr;
    const WaveSignal* localMem = nullptr;
    QHash<QString, const WaveSignal*> featureSignals;
};

struct IssueContextMetrics {
    waveperf::IssueContextView profile;
    qint64 globalMemIssueTicks = 0;
    qint64 localMemIssueTicks = 0;
    qint64 memoryClassifiedIssueTicks = 0;
    qint64 memoryUnclassifiedIssueTicks = 0;
    qint64 classifiedIssueTicks = 0;
    qint64 unclassifiedIssueTicks = 0;
    QMap<quint64, qint64> mainTypeTicks;
    QMap<quint64, qint64> issueTypeTicks;
    QMap<QString, qint64> issueClassTicks;
    QMap<QString, qint64> dualIssuePairTicks;
    QMap<QString, qint64> featureActiveIssueTicks;
    QMap<QString, qint64> featureClassifiedIssueTicks;
    QMap<QString, qint64> featureUnclassifiedIssueTicks;
};

struct IssueActivityWindow {
    bool found = false;
    qint64 firstActiveTick = 0;
    qint64 lastActiveTickExclusive = 0;
    qint64 alignedStartTick = 0;
    qint64 alignedEndTick = 0;
};

QString issueClassKey(quint64 issueType) {
    return waveperf::instIssueClassKey(issueType);
}

QString orderedIssuePairKey(const QString& mainClass,
                            const QString& shadowClass) {
    return mainClass + QLatin1Char('>') + shadowClass;
}

struct CoverageArray {
    int declaredSize = 0;
    QSet<int> indexes;
};

struct CoverageFamily {
    QHash<QString, CoverageArray> arrays;
    int declaredSize = 0;
    int tracedSize = 0;
    int incompleteArrays = 0;
    int missingSignalCount = 0;
    QStringList missingSignalPaths;
};

struct ThreadMaskSignals {
    QString qppuPath;
    int qppuIndex = -1;
    int sgIndex = -1;
    int laneCount = 32;
    const WaveSignal* validPacked = nullptr;
    const WaveSignal* activePacked = nullptr;
    const WaveSignal* executePacked = nullptr;
    QMap<int, const WaveSignal*> validLanes;
    QMap<int, const WaveSignal*> activeLanes;
    QMap<int, const WaveSignal*> executeLanes;
};

struct ThreadIssueStream {
    QString qppuPath;
    int qppuIndex = -1;
    const WaveSignal* readCounter = nullptr;
    const WaveSignal* sgId = nullptr;
};

struct MaskPopulation {
    bool covered = false;
    int activeLanes = 0;
    int tracedLanes = 0;
    int laneCount = 0;
    quint64 bits = 0;
};

struct SgThreadAccumulator {
    QString qppuPath;
    int qppuIndex = -1;
    int sgIndex = -1;
    quint64 threadInstructions = 0;
    quint64 validCoveredInstructions = 0;
    quint64 activeCoveredInstructions = 0;
    quint64 executeCoveredInstructions = 0;
    quint64 activePairedInstructions = 0;
    quint64 executePairedInstructions = 0;
    quint64 fullyCoveredInstructions = 0;
    long double validThreadSlots = 0.0L;
    long double validThreadSlotsForActive = 0.0L;
    long double validThreadSlotsForExecute = 0.0L;
    long double activeThreadSlots = 0.0L;
    long double executeThreadSlots = 0.0L;
    int laneCount = 32;
    int validLanesTraced = 0;
    int activeLanesTraced = 0;
    int executeLanesTraced = 0;
    bool activeMaskConsistent = true;
    bool executeMaskConsistent = true;
};

bool checkedMultiply(qint64 left, qint64 right, qint64& result) {
    if (left < 0 || right <= 0) return false;
    if (left > std::numeric_limits<qint64>::max() / right) return false;
    result = left * right;
    return true;
}

ValueState stateFromSample(const WaveSignal& signal, const WaveSample& input) {
    WaveSample sample = input;
    if (!sample.rawFieldsReady) {
        hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
    }
    if (sample.isZ || sample.isAbsent) return ValueState();
    ValueState state;
    state.known = true;
    state.value = sample.rawBits & waveBitMaskForWidth(signal.width);
    return state;
}

ValueState stateAtOrBefore(const WaveSignal& signal, qint64 time) {
    int lo = 0;
    int hi = signal.samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal.samples.at(mid).time <= time) lo = mid + 1;
        else hi = mid;
    }
    if (lo <= 0) return ValueState();
    return stateFromSample(signal, signal.samples.at(lo - 1));
}

ValueState stateBefore(const WaveSignal& signal, qint64 time) {
    int lo = 0;
    int hi = signal.samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal.samples.at(mid).time < time) lo = mid + 1;
        else hi = mid;
    }
    if (lo <= 0) return ValueState();
    return stateFromSample(signal, signal.samples.at(lo - 1));
}

IssueActivityWindow findGlobalIssueActivityWindow(
    const QVector<const WaveSignal*>& issueValidSignals,
    qint64 outerStart,
    qint64 outerEnd,
    qint64 ticksPerCycle) {
    IssueActivityWindow result;
    if (outerEnd <= outerStart || ticksPerCycle <= 0) return result;

    auto includeActiveInterval = [&](qint64 begin, qint64 end) {
        begin = qMax(begin, outerStart);
        end = qMin(end, outerEnd);
        if (end <= begin) return;
        if (!result.found) {
            result.found = true;
            result.firstActiveTick = begin;
            result.lastActiveTickExclusive = end;
            return;
        }
        result.firstActiveTick = qMin(result.firstActiveTick, begin);
        result.lastActiveTickExclusive =
            qMax(result.lastActiveTickExclusive, end);
    };

    for (const WaveSignal* signal : issueValidSignals) {
        if (!signal) continue;
        ValueState current = stateBefore(*signal, outerStart);
        qint64 cursor = outerStart;
        for (const WaveSample& sample : signal->samples) {
            if (sample.time < outerStart) continue;
            if (sample.time >= outerEnd) break;
            if (current.known && current.value != 0) {
                includeActiveInterval(cursor, sample.time);
            }
            current = stateFromSample(*signal, sample);
            cursor = sample.time;
        }
        if (current.known && current.value != 0) {
            includeActiveInterval(cursor, outerEnd);
        }
    }
    if (!result.found) return result;

    qint64 alignedStart =
        (result.firstActiveTick / ticksPerCycle) * ticksPerCycle;
    if (result.firstActiveTick < 0 &&
        result.firstActiveTick % ticksPerCycle != 0) {
        alignedStart -= ticksPerCycle;
    }
    qint64 alignedEnd = result.lastActiveTickExclusive;
    qint64 remainder = alignedEnd % ticksPerCycle;
    if (remainder < 0) remainder += ticksPerCycle;
    if (remainder != 0) {
        const qint64 increment = ticksPerCycle - remainder;
        alignedEnd =
            increment > outerEnd - alignedEnd
                ? outerEnd
                : alignedEnd + increment;
    }
    result.alignedStartTick = qMax(outerStart, alignedStart);
    result.alignedEndTick = qMin(outerEnd, alignedEnd);
    return result;
}

void includeNumericValue(SignalMetrics& metrics, const ValueState& state) {
    if (!state.known) return;
    if (!metrics.hasNumericValue) {
        metrics.hasNumericValue = true;
        metrics.minValue = state.value;
        metrics.maxValue = state.value;
    } else {
        metrics.minValue = qMin(metrics.minValue, state.value);
        metrics.maxValue = qMax(metrics.maxValue, state.value);
    }
    metrics.lastValue = state.value;
}

int bitPopulation(quint64 value, int width) {
    int result = 0;
    const int boundedWidth = qBound(0, width, 64);
    for (int bit = 0; bit < boundedWidth; ++bit) {
        if ((value >> bit) & 1ull) ++result;
    }
    return result;
}

void accumulateInterval(SignalMetrics& metrics,
                        const ValueState& state,
                        qint64 begin,
                        qint64 end) {
    if (end <= begin) return;
    const qint64 ticks = end - begin;
    if (!state.known) {
        metrics.unknownTicks += ticks;
        return;
    }
    metrics.knownTicks += ticks;
    if (state.value != 0) metrics.activeTicks += ticks;
    metrics.weightedValueTicks +=
        static_cast<long double>(state.value) * static_cast<long double>(ticks);
    metrics.populationWeightedTicks +=
        static_cast<long double>(bitPopulation(state.value, metrics.width)) *
        static_cast<long double>(ticks);
}

SignalMetrics analyzeSignal(const SignalSelection& selection,
                            const WaveSignal& signal,
                            qint64 start,
                            qint64 end) {
    SignalMetrics metrics;
    metrics.selection = selection;
    metrics.width = signal.width;

    ValueState current = stateBefore(signal, start);
    includeNumericValue(metrics, current);
    qint64 cursor = start;

    for (const WaveSample& sample : signal.samples) {
        if (sample.time < start || sample.time >= end) continue;
        ++metrics.samplesInRange;
        accumulateInterval(metrics, current, cursor, sample.time);
        const ValueState next = stateFromSample(signal, sample);
        if (current.known && next.known && current.value != next.value) {
            ++metrics.transitionCount;
            if (next.value >= current.value) {
                const quint64 delta = next.value - current.value;
                metrics.positiveDelta =
                    delta >
                            std::numeric_limits<quint64>::max() -
                                metrics.positiveDelta
                        ? std::numeric_limits<quint64>::max()
                        : metrics.positiveDelta + delta;
            } else {
                ++metrics.decreaseEvents;
            }
        }
        current = next;
        includeNumericValue(metrics, current);
        cursor = sample.time;
    }

    accumulateInterval(metrics, current, cursor, end);
    includeNumericValue(metrics, current);
    return metrics;
}

quint64 roundedCycleIntegral(long double valueTicks,
                             qint64 ticksPerCycle) {
    if (ticksPerCycle <= 0 || valueTicks <= 0.0L) return 0;
    const long double cycleValue =
        valueTicks / static_cast<long double>(ticksPerCycle);
    if (cycleValue >=
        static_cast<long double>(std::numeric_limits<quint64>::max())) {
        return std::numeric_limits<quint64>::max();
    }
    return static_cast<quint64>(std::floor(cycleValue + 0.5L));
}

quint64 eventCountForMetrics(const SignalMetrics& metrics,
                             qint64 ticksPerCycle) {
    switch (metrics.selection.eventSemantics) {
    case waveperf::EventSemantics::PerCycleValue:
        return roundedCycleIntegral(metrics.weightedValueTicks,
                                    ticksPerCycle);
    case waveperf::EventSemantics::PerCycleMask:
        return roundedCycleIntegral(metrics.populationWeightedTicks,
                                    ticksPerCycle);
    case waveperf::EventSemantics::CumulativeCounter:
        return metrics.positiveDelta;
    case waveperf::EventSemantics::None:
        return 0;
    }
    return 0;
}

QVector<qint64> signalBoundaries(const QVector<const WaveSignal*>& signalSet,
                                 qint64 start,
                                 qint64 end) {
    QVector<qint64> boundaries;
    boundaries.push_back(start);
    boundaries.push_back(end);
    for (const WaveSignal* signal : signalSet) {
        if (!signal) continue;
        for (const WaveSample& sample : signal->samples) {
            if (sample.time > start && sample.time < end) boundaries.push_back(sample.time);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

IssueContextMetrics analyzeIssueContext(
    const QVector<const IssueSlot*>& contextSlots,
    int declaredSlotCount,
    qint64 start,
    qint64 end) {
    IssueContextMetrics result;
    result.profile.slotActiveTicks.resize(declaredSlotCount);
    result.profile.slotCapacityTicks.resize(declaredSlotCount);

    QSet<int> tracedSlots;
    QVector<const WaveSignal*> boundarySignals;
    boundarySignals.reserve(
        contextSlots.size() *
        (5 + waveperf::instructionFeatureSpecs().size()));
    for (const IssueSlot* slot : contextSlots) {
        if (!slot || !slot->valid || slot->slotIndex < 0 ||
            slot->slotIndex >= declaredSlotCount) {
            continue;
        }
        tracedSlots.insert(slot->slotIndex);
        const SignalSelection dummy;
        const SignalMetrics validMetrics =
            analyzeSignal(dummy, *slot->valid, start, end);
        result.profile.slotActiveTicks[slot->slotIndex] +=
            validMetrics.activeTicks;
        result.profile.slotCapacityTicks[slot->slotIndex] +=
            validMetrics.knownTicks;
        boundarySignals.push_back(slot->valid);
        if (slot->mainType) boundarySignals.push_back(slot->mainType);
        if (slot->issueType) boundarySignals.push_back(slot->issueType);
        if (slot->globalMem) boundarySignals.push_back(slot->globalMem);
        if (slot->localMem) boundarySignals.push_back(slot->localMem);
        for (const waveperf::InstructionFeatureSpec& spec :
             waveperf::instructionFeatureSpecs()) {
            const WaveSignal* feature =
                slot->featureSignals.value(spec.key, nullptr);
            if (feature) boundarySignals.push_back(feature);
        }
    }
    const bool allSlotsTraced = tracedSlots.size() == declaredSlotCount;
    const QVector<qint64> boundaries =
        signalBoundaries(boundarySignals, start, end);
    qint64 observedTicks = 0;
    for (int i = 0; i + 1 < boundaries.size(); ++i) {
        int activeSlots = 0;
        bool allSlotsKnown = allSlotsTraced;
        QVector<QPair<int, QString>> activeClasses;
        QVector<quint64> activeMainTypes;
        QVector<quint64> activeIssueTypes;
        int globalMemSlots = 0;
        int localMemSlots = 0;
        int memoryClassifiedSlots = 0;
        int memoryUnclassifiedSlots = 0;
        QMap<QString, int> featureActiveSlots;
        QMap<QString, int> featureClassifiedSlots;
        QMap<QString, int> featureUnclassifiedSlots;
        for (const IssueSlot* slot : contextSlots) {
            if (!slot || !slot->valid) {
                allSlotsKnown = false;
                continue;
            }
            const ValueState validState =
                stateAtOrBefore(*slot->valid, boundaries.at(i));
            if (!validState.known) {
                allSlotsKnown = false;
                continue;
            }
            if (validState.value == 0) continue;
            ++activeSlots;
            bool memoryClassKnown =
                slot->globalMem && slot->localMem;
            if (slot->mainType) {
                const ValueState mainState =
                    stateAtOrBefore(*slot->mainType, boundaries.at(i));
                if (mainState.known)
                    activeMainTypes.push_back(mainState.value);
            }
            QString issueClass;
            if (slot->issueType) {
                const ValueState typeState =
                    stateAtOrBefore(*slot->issueType, boundaries.at(i));
                if (typeState.known) {
                    activeIssueTypes.push_back(typeState.value);
                    issueClass = issueClassKey(typeState.value);
                }
            }
            if (slot->globalMem) {
                const ValueState state =
                    stateAtOrBefore(*slot->globalMem, boundaries.at(i));
                if (!state.known) {
                    memoryClassKnown = false;
                } else if (state.value != 0) {
                    ++globalMemSlots;
                }
            }
            if (slot->localMem) {
                const ValueState state =
                    stateAtOrBefore(*slot->localMem, boundaries.at(i));
                if (!state.known) {
                    memoryClassKnown = false;
                } else if (state.value != 0) {
                    ++localMemSlots;
                }
            }
            if (memoryClassKnown) {
                ++memoryClassifiedSlots;
            } else {
                ++memoryUnclassifiedSlots;
            }
            for (const waveperf::InstructionFeatureSpec& spec :
                 waveperf::instructionFeatureSpecs()) {
                const WaveSignal* feature =
                    slot->featureSignals.value(spec.key, nullptr);
                if (!feature) {
                    ++featureUnclassifiedSlots[spec.key];
                    continue;
                }
                const ValueState state =
                    stateAtOrBefore(*feature, boundaries.at(i));
                if (!state.known) {
                    ++featureUnclassifiedSlots[spec.key];
                    continue;
                }
                ++featureClassifiedSlots[spec.key];
                if (state.value != 0) {
                    ++featureActiveSlots[spec.key];
                }
            }
            activeClasses.push_back({slot->slotIndex, issueClass});
        }

        const qint64 ticks = boundaries.at(i + 1) - boundaries.at(i);
        if (!allSlotsKnown) continue;
        observedTicks += ticks;
        result.profile.issuedTicks += qint64(activeSlots) * ticks;
        if (activeSlots > 0) result.profile.issueActiveTicks += ticks;
        if (activeSlots > 1) result.profile.dualIssueTicks += ticks;
        for (quint64 value : activeMainTypes)
            result.mainTypeTicks[value] += ticks;
        for (quint64 value : activeIssueTypes)
            result.issueTypeTicks[value] += ticks;
        result.globalMemIssueTicks += qint64(globalMemSlots) * ticks;
        result.localMemIssueTicks += qint64(localMemSlots) * ticks;
        result.memoryClassifiedIssueTicks +=
            qint64(memoryClassifiedSlots) * ticks;
        result.memoryUnclassifiedIssueTicks +=
            qint64(memoryUnclassifiedSlots) * ticks;
        for (const waveperf::InstructionFeatureSpec& spec :
             waveperf::instructionFeatureSpecs()) {
            result.featureActiveIssueTicks[spec.key] +=
                qint64(featureActiveSlots.value(spec.key)) * ticks;
            result.featureClassifiedIssueTicks[spec.key] +=
                qint64(featureClassifiedSlots.value(spec.key)) * ticks;
            result.featureUnclassifiedIssueTicks[spec.key] +=
                qint64(featureUnclassifiedSlots.value(spec.key)) * ticks;
        }
        for (const QPair<int, QString>& activeClass : activeClasses) {
            if (activeClass.second.isEmpty() ||
                activeClass.second.startsWith(QStringLiteral("unknown_"))) {
                result.unclassifiedIssueTicks += ticks;
            } else {
                result.issueClassTicks[activeClass.second] += ticks;
                result.classifiedIssueTicks += ticks;
            }
        }
        if (activeClasses.size() == 2) {
            std::sort(
                activeClasses.begin(), activeClasses.end(),
                [](const QPair<int, QString>& left,
                   const QPair<int, QString>& right) {
                    return left.first < right.first;
                });
            QString mainClass = activeClasses.at(0).second;
            QString shadowClass = activeClasses.at(1).second;
            if (mainClass.isEmpty() ||
                mainClass.startsWith(QStringLiteral("unknown_"))) {
                mainClass = QStringLiteral("unknown");
            }
            if (shadowClass.isEmpty() ||
                shadowClass.startsWith(QStringLiteral("unknown_"))) {
                shadowClass = QStringLiteral("unknown");
            }
            result.dualIssuePairTicks[
                orderedIssuePairKey(mainClass, shadowClass)] += ticks;
        }
    }

    result.profile.idleTicks =
        observedTicks - result.profile.issueActiveTicks;
    for (qint64 ticks : result.profile.slotCapacityTicks)
        result.profile.capacityTicks += ticks;
    return result;
}

QString signalParentPath(const QString& path) {
    const int separator = path.lastIndexOf(QLatin1Char('.'));
    return separator >= 0 ? path.left(separator) : path;
}

FullWindow analyzeFullWindow(const WaveSignal* occupancy,
                             const WaveSignal* capacity,
                             qint64 start,
                             qint64 end) {
    FullWindow result;
    if (!occupancy || !capacity) return result;
    result.expectedTicks = qMax<qint64>(0, end - start);
    const QVector<qint64> boundaries =
        signalBoundaries({occupancy, capacity}, start, end);
    for (int i = 0; i + 1 < boundaries.size(); ++i) {
        const ValueState occupancyState =
            stateAtOrBefore(*occupancy, boundaries.at(i));
        const ValueState capacityState =
            stateAtOrBefore(*capacity, boundaries.at(i));
        if (!occupancyState.known || !capacityState.known ||
            capacityState.value == 0) {
            continue;
        }
        const qint64 ticks = boundaries.at(i + 1) - boundaries.at(i);
        result.knownTicks += ticks;
        if (occupancyState.value >= capacityState.value) {
            result.fullTicks += ticks;
        }
    }
    return result;
}

MaskPopulation sampleMask(const WaveSignal* packed,
                          const QMap<int, const WaveSignal*>& lanes,
                          int declaredLaneCount,
                          qint64 time) {
    MaskPopulation result;
    result.laneCount = declaredLaneCount;
    if (packed) {
        result.laneCount =
            result.laneCount > 0 ? result.laneCount : packed->width;
        const ValueState state = stateAtOrBefore(*packed, time);
        result.tracedLanes = qMin(packed->width, 64);
        result.covered =
            state.known && result.laneCount > 0 &&
            result.laneCount <= result.tracedLanes;
        if (state.known) {
            result.bits = state.value;
            result.activeLanes =
                bitPopulation(state.value, qMin(result.laneCount, 64));
        }
        return result;
    }

    if (result.laneCount <= 0 && !lanes.isEmpty()) {
        result.laneCount = lanes.lastKey() + 1;
    }
    result.tracedLanes = lanes.size();
    result.covered =
        result.laneCount > 0 && result.laneCount <= 64 &&
        lanes.size() >= result.laneCount;
    for (int lane = 0; lane < result.laneCount; ++lane) {
        const WaveSignal* signal = lanes.value(lane, nullptr);
        if (!signal) {
            result.covered = false;
            continue;
        }
        const ValueState state = stateAtOrBefore(*signal, time);
        if (!state.known) {
            result.covered = false;
        } else if (state.value != 0) {
            ++result.activeLanes;
            if (lane < 64) result.bits |= quint64(1) << lane;
        }
    }
    return result;
}

bool qppuIdentity(const QString& path, QString& qppuPath, int& qppuIndex) {
    static const QRegularExpression expression(
        QStringLiteral("^(.+\\.m_QPPUTOP(?:\\[size=\\d+\\])?\\."
                       "\\[(\\d+)\\])"));
    const QRegularExpressionMatch match = expression.match(path);
    if (!match.hasMatch()) return false;
    qppuPath = match.captured(1);
    qppuIndex = match.captured(2).toInt();
    return true;
}

QString sgThreadKey(const QString& qppuPath, int sgIndex) {
    return qppuPath + QLatin1Char('#') + QString::number(sgIndex);
}

QJsonObject buildSgThreadEfficiency(
    const QHash<QString, const WaveSignal*>& loadedByPath,
    qint64 start,
    qint64 end,
    qint64 ticksPerCycle,
    QStringList& warnings) {
    static const QRegularExpression contextExpression(
        QStringLiteral(
            "^(.+\\.m_QPPUTOP(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\])"
            "\\.m_EU\\.shader_group_context_"
            "(?:\\[size=(\\d+)\\])?\\.\\[(\\d+)\\]"
            "(?:\\[size=(\\d+)\\])?\\.\\[(\\d+)\\]\\.(.+)$"));
    static const QRegularExpression maskExpression(
        QStringLiteral(
            "^thread_(valid|active|execute)_(?:indicator|mask)"
            "(?:\\[size=(\\d+)\\])?(?:\\.\\[(\\d+)\\])?$"));

    QHash<QString, ThreadMaskSignals> masksBySg;
    QHash<QString, ThreadIssueStream> streamsByQppu;
    QSet<QString> observedQppuPaths;
    for (auto it = loadedByPath.constBegin(); it != loadedByPath.constEnd(); ++it) {
        QString qppuPath;
        int qppuIndex = -1;
        if (!qppuIdentity(it.key(), qppuPath, qppuIndex)) continue;
        observedQppuPaths.insert(qppuPath);

        if (it.key() ==
            qppuPath +
                QStringLiteral(
                    ".m_QPPUEU.pt_BE_ThdCore_new_inst.m_num_read")) {
            ThreadIssueStream& stream = streamsByQppu[qppuPath];
            stream.qppuPath = qppuPath;
            stream.qppuIndex = qppuIndex;
            stream.readCounter = it.value();
        } else if (
            it.key() ==
                qppuPath +
                    QStringLiteral(".m_QPPUEU.m_EUState.group_info.sgId") ||
            it.key() ==
                qppuPath +
                    QStringLiteral(
                        ".m_QPPUEU.m_EUState.group_info.local_sg_id")) {
            ThreadIssueStream& stream = streamsByQppu[qppuPath];
            stream.qppuPath = qppuPath;
            stream.qppuIndex = qppuIndex;
            stream.sgId = it.value();
        }

        const QRegularExpressionMatch contextMatch =
            contextExpression.match(it.key());
        if (!contextMatch.hasMatch()) continue;
        const int contextQppuIndex = contextMatch.captured(4).toInt();
        if (contextQppuIndex != qppuIndex) continue;
        const int sgIndex = contextMatch.captured(6).toInt();
        const QString leaf = contextMatch.captured(7);
        const QRegularExpressionMatch maskMatch =
            maskExpression.match(leaf);
        if (!maskMatch.hasMatch()) continue;
        ThreadMaskSignals& masks =
            masksBySg[sgThreadKey(qppuPath, sgIndex)];
        masks.qppuPath = qppuPath;
        masks.qppuIndex = qppuIndex;
        masks.sgIndex = sgIndex;
        const QString kind = maskMatch.captured(1);
        const int declaredLanes = maskMatch.captured(2).toInt();
        if (declaredLanes > 0) masks.laneCount = qMax(masks.laneCount, declaredLanes);
        const bool hasLaneIndex = !maskMatch.captured(3).isEmpty();
        const int laneIndex = maskMatch.captured(3).toInt();
        if (kind == QStringLiteral("valid")) {
            if (hasLaneIndex) masks.validLanes.insert(laneIndex, it.value());
            else masks.validPacked = it.value();
        } else if (kind == QStringLiteral("active")) {
            if (hasLaneIndex) masks.activeLanes.insert(laneIndex, it.value());
            else masks.activePacked = it.value();
        } else {
            if (hasLaneIndex) masks.executeLanes.insert(laneIndex, it.value());
            else masks.executePacked = it.value();
        }
    }

    QHash<QString, SgThreadAccumulator> accumulators;
    quint64 totalThreadInstructions = 0;
    quint64 expectedThreadSamples = 0;
    quint64 observedThreadSamples = 0;
    int completeStreams = 0;
    for (const ThreadIssueStream& stream : streamsByQppu) {
        if (!stream.readCounter || !stream.sgId ||
            ticksPerCycle <= 0) {
            continue;
        }
        ++completeStreams;
        for (qint64 time = start; time < end;) {
            ++expectedThreadSamples;
            const ValueState readState =
                stateAtOrBefore(*stream.readCounter, time);
            if (readState.known) ++observedThreadSamples;
            if (readState.known && readState.value > 0) {
                const quint64 instructionCount = readState.value;
                totalThreadInstructions += instructionCount;
                const ValueState sgState =
                    stateAtOrBefore(*stream.sgId, time);
                if (sgState.known &&
                    sgState.value <=
                        static_cast<quint64>(
                            std::numeric_limits<int>::max())) {
                    const int sgIndex =
                        static_cast<int>(sgState.value);
                    SgThreadAccumulator& accumulator =
                        accumulators[
                            sgThreadKey(stream.qppuPath, sgIndex)];
                    accumulator.qppuPath = stream.qppuPath;
                    accumulator.qppuIndex = stream.qppuIndex;
                    accumulator.sgIndex = sgIndex;
                    accumulator.threadInstructions += instructionCount;

                    const ThreadMaskSignals masks =
                        masksBySg.value(
                            sgThreadKey(stream.qppuPath, sgIndex));
                    accumulator.laneCount =
                        qMax(accumulator.laneCount, masks.laneCount);
                    const MaskPopulation valid =
                        sampleMask(masks.validPacked, masks.validLanes,
                                   masks.laneCount, time);
                    const MaskPopulation active =
                        sampleMask(masks.activePacked, masks.activeLanes,
                                   masks.laneCount, time);
                    const MaskPopulation execute =
                        sampleMask(masks.executePacked, masks.executeLanes,
                                   masks.laneCount, time);
                    accumulator.validLanesTraced =
                        qMax(accumulator.validLanesTraced,
                             valid.tracedLanes);
                    accumulator.activeLanesTraced =
                        qMax(accumulator.activeLanesTraced,
                             active.tracedLanes);
                    accumulator.executeLanesTraced =
                        qMax(accumulator.executeLanesTraced,
                             execute.tracedLanes);
                    if (valid.covered) {
                        accumulator.validCoveredInstructions +=
                            instructionCount;
                        accumulator.validThreadSlots +=
                            static_cast<long double>(
                                valid.activeLanes) *
                            static_cast<long double>(
                                instructionCount);
                    }
                    if (active.covered) {
                        accumulator.activeCoveredInstructions +=
                            instructionCount;
                    }
                    if (execute.covered) {
                        accumulator.executeCoveredInstructions +=
                            instructionCount;
                    }
                    if (valid.covered && active.covered) {
                        if ((active.bits & ~valid.bits) != 0) {
                            accumulator.activeMaskConsistent = false;
                        }
                        accumulator.activePairedInstructions +=
                            instructionCount;
                        accumulator.validThreadSlotsForActive +=
                            static_cast<long double>(
                                valid.activeLanes) *
                            static_cast<long double>(
                                instructionCount);
                        accumulator.activeThreadSlots +=
                            static_cast<long double>(
                                active.activeLanes) *
                            static_cast<long double>(
                                instructionCount);
                    }
                    if (valid.covered && execute.covered) {
                        if ((execute.bits & ~valid.bits) != 0) {
                            accumulator.executeMaskConsistent = false;
                        }
                        accumulator.executePairedInstructions +=
                            instructionCount;
                        accumulator.validThreadSlotsForExecute +=
                            static_cast<long double>(
                                valid.activeLanes) *
                            static_cast<long double>(
                                instructionCount);
                        accumulator.executeThreadSlots +=
                            static_cast<long double>(
                                execute.activeLanes) *
                            static_cast<long double>(
                                instructionCount);
                    }
                    if (valid.covered && active.covered &&
                        execute.covered) {
                        accumulator.fullyCoveredInstructions +=
                            instructionCount;
                    }
                }
            }
            if (time >
                std::numeric_limits<qint64>::max() -
                    ticksPerCycle) {
                break;
            }
            time += ticksPerCycle;
        }
    }

    QVector<SgThreadAccumulator> ordered = accumulators.values().toVector();
    std::sort(ordered.begin(), ordered.end(),
              [](const SgThreadAccumulator& left,
                 const SgThreadAccumulator& right) {
                  if (left.qppuIndex != right.qppuIndex) {
                      return left.qppuIndex < right.qppuIndex;
                  }
                  return left.sgIndex < right.sgIndex;
              });

    QJsonArray entries;
    quint64 measuredInstructions = 0;
    long double totalValidForExecute = 0.0L;
    long double totalExecute = 0.0L;
    int inconsistentMaskEntries = 0;
    for (const SgThreadAccumulator& accumulator : ordered) {
        QJsonObject entry;
        entry.insert(QStringLiteral("qppu_path"), accumulator.qppuPath);
        entry.insert(QStringLiteral("qppu_index"), accumulator.qppuIndex);
        entry.insert(QStringLiteral("sg_index"), accumulator.sgIndex);
        entry.insert(QStringLiteral("thread_instructions"),
                     QString::number(accumulator.threadInstructions));
        entry.insert(QStringLiteral("fully_covered_instructions"),
                     QString::number(accumulator.fullyCoveredInstructions));
        entry.insert(QStringLiteral("lane_count"), accumulator.laneCount);
        entry.insert(QStringLiteral("valid_lanes_traced"),
                     accumulator.validLanesTraced);
        entry.insert(QStringLiteral("active_lanes_traced"),
                     accumulator.activeLanesTraced);
        entry.insert(QStringLiteral("execute_lanes_traced"),
                     accumulator.executeLanesTraced);
        const bool validCovered =
            accumulator.validCoveredInstructions ==
                accumulator.threadInstructions &&
            accumulator.threadInstructions > 0 &&
            accumulator.laneCount > 0;
        const bool activeCovered =
            accumulator.activePairedInstructions ==
                accumulator.threadInstructions &&
            validCovered && accumulator.activeMaskConsistent;
        const bool executeCovered =
            accumulator.executePairedInstructions ==
                accumulator.threadInstructions &&
            validCovered && accumulator.executeMaskConsistent;
        entry.insert(QStringLiteral("active_mask_consistent"),
                     accumulator.activeMaskConsistent);
        entry.insert(QStringLiteral("execute_mask_consistent"),
                     accumulator.executeMaskConsistent);
        entry.insert(QStringLiteral("valid_covered"), validCovered);
        entry.insert(QStringLiteral("active_covered"), activeCovered);
        entry.insert(QStringLiteral("execute_covered"), executeCovered);
        entry.insert(QStringLiteral("covered"),
                     validCovered && activeCovered && executeCovered);
        if (!accumulator.activeMaskConsistent ||
            !accumulator.executeMaskConsistent) {
            ++inconsistentMaskEntries;
        }
        if (validCovered) {
            entry.insert(
                QStringLiteral("valid_thread_occupancy_percent"),
                accumulator.laneCount > 0
                    ? 100.0 *
                          double(accumulator.validThreadSlots) /
                          (double(accumulator.threadInstructions) *
                           double(accumulator.laneCount))
                    : 0.0);
        }
        if (activeCovered &&
            accumulator.validThreadSlotsForActive > 0.0L) {
            entry.insert(
                QStringLiteral("active_thread_efficiency_percent"),
                double(100.0L * accumulator.activeThreadSlots /
                       accumulator.validThreadSlotsForActive));
        }
        if (executeCovered &&
            accumulator.validThreadSlotsForExecute > 0.0L) {
            const double efficiency =
                double(100.0L * accumulator.executeThreadSlots /
                       accumulator.validThreadSlotsForExecute);
            entry.insert(QStringLiteral("thread_efficiency_percent"),
                         efficiency);
            entry.insert(QStringLiteral("divergence_loss_percent"),
                         qMax(0.0, 100.0 - efficiency));
            measuredInstructions += accumulator.threadInstructions;
            totalValidForExecute +=
                accumulator.validThreadSlotsForExecute;
            totalExecute += accumulator.executeThreadSlots;
        }
        entries.push_back(entry);
    }

    QJsonObject result;
    result.insert(
        QStringLiteral("definition"),
        QStringLiteral(
            "Thread有效率 = QPPUEU实际读取Thread指令时的execute线程槽 / "
            "valid线程槽；有效线程占用率以每SG 32线程槽为上限"));
    result.insert(QStringLiteral("total_thread_instructions"),
                  QString::number(totalThreadInstructions));
    result.insert(QStringLiteral("measured_thread_instructions"),
                  QString::number(measuredInstructions));
    result.insert(QStringLiteral("thread_streams"),
                  completeStreams);
    result.insert(QStringLiteral("expected_thread_streams"),
                  observedQppuPaths.size());
    result.insert(QStringLiteral("thread_stream_expected_cycles"),
                  QString::number(expectedThreadSamples));
    result.insert(QStringLiteral("thread_stream_observed_cycles"),
                  QString::number(observedThreadSamples));
    const bool streamCoverageComplete =
        !observedQppuPaths.isEmpty() &&
        completeStreams == observedQppuPaths.size() &&
        expectedThreadSamples > 0 &&
        observedThreadSamples == expectedThreadSamples;
    result.insert(QStringLiteral("thread_stream_coverage_complete"),
                  streamCoverageComplete);
    result.insert(QStringLiteral("thread_mask_sg_entries"),
                  masksBySg.size());
    result.insert(QStringLiteral("inconsistent_mask_entries"),
                  inconsistentMaskEntries);
    result.insert(QStringLiteral("entries"), entries);
    if (streamCoverageComplete &&
        measuredInstructions == totalThreadInstructions &&
        totalValidForExecute > 0.0L) {
        result.insert(QStringLiteral("overall_thread_efficiency_percent"),
                      double(100.0L * totalExecute /
                             totalValidForExecute));
    }
    QString status;
    if (observedQppuPaths.isEmpty() || completeStreams == 0) {
        status = QStringLiteral("thread_stream_missing");
    } else if (!streamCoverageComplete) {
        status = QStringLiteral("partial");
        warnings.push_back(
            QStringLiteral(
                "Thread 指令流只覆盖 %1/%2 个周期样本，或仅发现 %3/%4 个完整 QPPU 流；"
                "总体 Thread 有效率不发布")
                .arg(observedThreadSamples)
                .arg(expectedThreadSamples)
                .arg(completeStreams)
                .arg(observedQppuPaths.size()));
    } else if (totalThreadInstructions == 0) {
        status = QStringLiteral("no_thread_activity");
    } else if (measuredInstructions == 0) {
        if (inconsistentMaskEntries > 0) {
            status = QStringLiteral("thread_masks_inconsistent");
            warnings.push_back(
                QStringLiteral(
                    "%1 个 SG 的 active/execute 掩码不是 valid 掩码的子集；"
                    "Thread 有效率不发布")
                    .arg(inconsistentMaskEntries));
        } else {
            status = QStringLiteral("thread_masks_missing");
            warnings.push_back(
                QStringLiteral(
                    "检测到 %1 条 Thread 指令，但每 SG 的 valid/active/execute "
                    "线程掩码覆盖不足，Thread 有效率不可计算")
                    .arg(totalThreadInstructions));
        }
    } else if (measuredInstructions < totalThreadInstructions) {
        status = QStringLiteral("partial");
        warnings.push_back(
            QStringLiteral(
                "每 SG Thread 有效率只覆盖 %1/%2 条 Thread 指令")
                .arg(measuredInstructions)
                .arg(totalThreadInstructions));
    } else {
        status = QStringLiteral("measured");
    }
    result.insert(QStringLiteral("status"), status);
    return result;
}

bool fastPerformanceCandidate(const WaveFile& directory,
                              int signalIndex) {
    if (!directory.tree.valid ||
        signalIndex < 0 ||
        signalIndex >= directory.tree.signalIndexToNodeId.size()) {
        return true;
    }
    auto nodeName = [&](int nodeId) {
        if (nodeId <= 0 ||
            nodeId >= directory.tree.nodesById.size()) {
            return QByteArray();
        }
        const WaveTreeNode& node =
            directory.tree.nodesById.at(nodeId);
        if (!node.valid ||
            waveNameTokenIsArrayIndex(node.nameToken)) {
            return QByteArray();
        }
        const quint32 nameId =
            waveNameTokenValue(node.nameToken);
        if (nameId == 0 ||
            nameId >= quint32(directory.tree.namesById.size())) {
            return QByteArray();
        }
        return directory.tree.namesById.at(int(nameId)).toLower();
    };

    int nodeId =
        directory.tree.signalIndexToNodeId.at(signalIndex);
    if (nodeId <= 0 ||
        nodeId >= directory.tree.nodesById.size()) {
        return true;
    }
    QByteArray field;
    for (int climb = 0; field.isEmpty() && climb < 6 &&
                        nodeId > 0; ++climb) {
        field = nodeName(nodeId);
        const int annotation = field.indexOf('[');
        if (annotation >= 0) field.truncate(annotation);
        if (!field.isEmpty()) break;
        nodeId = directory.tree.nodesById.at(nodeId).parentId;
    }

    static const QSet<QByteArray> exactFields = [] {
        QSet<QByteArray> fields = {
            QByteArrayLiteral("m_count"),
            QByteArrayLiteral("m_numavail"),
            QByteArrayLiteral("m_num_readable"),
            QByteArrayLiteral("m_num_read"),
            QByteArrayLiteral("m_num_written"),
            QByteArrayLiteral("m_size"),
            QByteArrayLiteral("m_ri"),
            QByteArrayLiteral("m_read_index"),
            QByteArrayLiteral("m_readindex"),
            QByteArrayLiteral("m_head"),
            QByteArrayLiteral("m_front"),
            QByteArrayLiteral("pc_"),
            QByteArrayLiteral("instissuetype"),
            QByteArrayLiteral("maintype"),
            QByteArrayLiteral("sgid"),
            QByteArrayLiteral("local_sgid"),
            QByteArrayLiteral("local_sg_id"),
            QByteArrayLiteral("mask"),
            QByteArrayLiteral("smask"),
            QByteArrayLiteral("wmask"),
            QByteArrayLiteral("taken"),
            QByteArrayLiteral("ready")
        };
        for (const waveperf::InstructionFeatureSpec& spec :
             waveperf::instructionFeatureSpecs()) {
            fields.insert(spec.fieldPath.toLatin1().toLower());
        }
        return fields;
    }();
    if (exactFields.contains(field)) return true;
    if (field == QByteArrayLiteral("thread") ||
        field == QByteArrayLiteral("exethdunit")) {
        int contextNode = nodeId;
        for (int depth = 0; depth < 12 && contextNode > 0; ++depth) {
            if (nodeName(contextNode).contains(
                    QByteArrayLiteral("instr_queue_"))) {
                return true;
            }
            if (contextNode >= directory.tree.nodesById.size()) break;
            contextNode =
                directory.tree.nodesById.at(contextNode).parentId;
        }
        return false;
    }

    static const QVector<QByteArray> directTokens = {
        QByteArrayLiteral("thread_valid_"),
        QByteArrayLiteral("thread_active_"),
        QByteArrayLiteral("thread_execute_"),
        QByteArrayLiteral("stall"),
        QByteArrayLiteral("check_dep"),
        QByteArrayLiteral("sleep_cnt"),
        QByteArrayLiteral("flow_ctrl"),
        QByteArrayLiteral("barrier_pend"),
        QByteArrayLiteral("set_max_temp"),
        QByteArrayLiteral("function_unit"),
        QByteArrayLiteral("inflight_mem"),
        QByteArrayLiteral("credit"),
        QByteArrayLiteral("pending"),
        QByteArrayLiteral("busy"),
        QByteArrayLiteral("miss"),
        QByteArrayLiteral("hit"),
        QByteArrayLiteral("dls2l1lstxtaken"),
        QByteArrayLiteral("dls2l1lstxreqrdy")
    };
    for (const QByteArray& token : directTokens) {
        if (field.contains(token)) return true;
    }

    if (field != QByteArrayLiteral("valid") &&
        field != QByteArrayLiteral("vld")) {
        return false;
    }
    static const QVector<QByteArray> contextTokens = {
        QByteArrayLiteral("issue_inst_"),
        QByteArrayLiteral("sg_table_"),
        QByteArrayLiteral("phase1_req_"),
        QByteArrayLiteral("cache"),
        QByteArrayLiteral("icache"),
        QByteArrayLiteral("chdls2l1"),
        QByteArrayLiteral("chl1lstx2dls"),
        QByteArrayLiteral("usctxarb2l2"),
        QByteArrayLiteral("l2cache2usctxarb"),
        QByteArrayLiteral("dls2l1lstx"),
        QByteArrayLiteral("l1lstx2dls")
    };
    for (int depth = 0; depth < 10 && nodeId > 0; ++depth) {
        const QByteArray segment = nodeName(nodeId);
        for (const QByteArray& token : contextTokens) {
            if (segment.contains(token)) return true;
        }
        if (nodeId >= directory.tree.nodesById.size()) break;
        nodeId = directory.tree.nodesById.at(nodeId).parentId;
    }
    return false;
}

QVector<SignalSelection> selectSignals(const WaveFile& directory,
                                       const Options& options,
                                       QStringList& warnings,
                                       QHash<int, QString>& pathBySignalId,
                                       bool& selectionTruncated) {
    QVector<SignalSelection> selections;
    selectionTruncated = false;
    QSet<int> selectedIds;
    for (int i = 0; i < directory.signalList.size(); ++i) {
        const WaveSignal& signal = directory.signalList.at(i);
        if (!fastPerformanceCandidate(directory, i)) continue;
        const QString path = waveperf::canonicalArchitecturePath(
            waveSignalFullPath(directory, i));

        QString key;
        QString category;
        bool helper = false;
        waveperf::EventSemantics eventSemantics =
            waveperf::EventSemantics::None;
        const waveperf::ClassifiedSignal classified =
            waveperf::classifyArchitectureSignal(path);
        key = classified.key;
        category = classified.category;
        helper = classified.helper;
        eventSemantics = classified.eventSemantics;
        if (key.isEmpty() || selectedIds.contains(signal.signalId)) continue;

        SignalSelection selection;
        selection.directoryIndex = i;
        selection.signalId = signal.signalId;
        selection.path = path;
        selection.key = key;
        selection.category = category;
        selection.helper = helper;
        selection.eventSemantics = eventSemantics;
        selections.push_back(selection);
        selectedIds.insert(signal.signalId);
        pathBySignalId.insert(signal.signalId, path);
        if (selections.size() >= options.maxSignals) {
            for (int j = i + 1; j < directory.signalList.size(); ++j) {
                const WaveSignal& remaining = directory.signalList.at(j);
                if (selectedIds.contains(remaining.signalId) ||
                    !fastPerformanceCandidate(directory, j)) {
                    continue;
                }
                const waveperf::ClassifiedSignal remainingClassified =
                    waveperf::classifyArchitectureSignal(
                        waveperf::canonicalArchitecturePath(
                            waveSignalFullPath(directory, j)));
                if (!remainingClassified.key.isEmpty()) {
                    selectionTruncated = true;
                    warnings.push_back(
                        QStringLiteral(
                            "信号选择已在 --max-signals=%1 处停止")
                            .arg(options.maxSignals));
                    break;
                }
            }
            break;
        }
    }
    return selections;
}

void collectQppuCoverage(const QVector<SignalSelection>& selections,
                         CoverageFamily& issueCoverage,
                         CoverageFamily& sgCoverage,
                         CoverageFamily& qppuCoverage) {
    static const QRegularExpression issueExpression(
        QStringLiteral("^(.*issue_inst_)(?:\\[size=(\\d+)\\])?"
                       "\\.\\[(\\d+)\\]\\.vld$"));
    static const QRegularExpression sgExpression(
        QStringLiteral("^(.*sg_table_)(?:\\[size=(\\d+)\\])?"
                       "\\.\\[(\\d+)\\]\\.valid$"));
    static const QRegularExpression qppuExpression(
        QStringLiteral("^(.*m_QPPUTOP)(?:\\[size=(\\d+)\\])?"
                       "\\.\\[(\\d+)\\]"));

    auto collect =
        [](const QRegularExpressionMatch& match,
           CoverageFamily& family) {
            if (!match.hasMatch()) return;
            const QString key = match.captured(1);
            const int index = match.captured(3).toInt();
            const int declared =
                match.captured(2).isEmpty()
                    ? index + 1
                    : match.captured(2).toInt();
            CoverageArray& array = family.arrays[key];
            array.declaredSize = qMax(array.declaredSize, declared);
            array.indexes.insert(index);
        };

    for (const SignalSelection& selection : selections) {
        const QString& path = selection.path;
        collect(issueExpression.match(path), issueCoverage);
        collect(sgExpression.match(path), sgCoverage);
        collect(qppuExpression.match(path), qppuCoverage);
    }

    auto finalize =
        [](CoverageFamily& family, const QString& suffix) {
        for (auto it = family.arrays.constBegin();
             it != family.arrays.constEnd(); ++it) {
            family.declaredSize += it.value().declaredSize;
            family.tracedSize += it.value().indexes.size();
            if (it.value().indexes.size() < it.value().declaredSize) {
                ++family.incompleteArrays;
            }
            for (int index = 0; index < it.value().declaredSize; ++index) {
                if (it.value().indexes.contains(index)) continue;
                ++family.missingSignalCount;
                if (family.missingSignalPaths.size() < 64) {
                    family.missingSignalPaths.push_back(
                        it.key() + QStringLiteral(".[%1]").arg(index) +
                        suffix);
                }
            }
        }
    };
    finalize(issueCoverage, QStringLiteral(".vld"));
    finalize(sgCoverage, QStringLiteral(".valid"));
    finalize(qppuCoverage,
             QStringLiteral(".m_QPPUCtrl.issue_inst_.[0].vld"));
}

bool issueWindowCoverageIsComplete(
    bool selectionTruncated,
    const CoverageFamily& issueCoverage,
    const CoverageFamily& qppuCoverage,
    const QVector<int>& issueValidSignalIds) {
    return !selectionTruncated &&
           !issueValidSignalIds.isEmpty() &&
           qppuCoverage.tracedSize > 0 &&
           issueCoverage.incompleteArrays == 0 &&
           issueCoverage.arrays.size() == qppuCoverage.tracedSize;
}

bool parseOptions(QCoreApplication& app, Options& options, bool& selfTest) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("WVZ4 GPU 性能分析器"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("wave"),
                                 QStringLiteral("输入 WVZ4 文件"));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("o"), QStringLiteral("out")},
        QStringLiteral("输出目录；默认 <波形名>.perf"),
        QStringLiteral("directory")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("start-cycle"), QStringLiteral("起始业务周期"),
        QStringLiteral("cycle")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("end-cycle"), QStringLiteral("结束业务周期，不包含该周期"),
        QStringLiteral("cycle")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("ticks-per-cycle"),
        QStringLiteral("每个业务周期包含的 WVZ4 内部 tick 数"),
        QStringLiteral("count"), QStringLiteral("10")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("timeline-bins"),
        QStringLiteral("时间线分桶数，范围 1..1000"),
        QStringLiteral("count"), QStringLiteral("160")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("max-samples"),
        QStringLiteral("最多解码的原始采样数；0 表示不限制"),
        QStringLiteral("count"), QStringLiteral("100000000")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("max-signals"),
        QStringLiteral("最多匹配的性能信号数"),
        QStringLiteral("count"), QStringLiteral("2000000")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("no-progress"),
        QStringLiteral("关闭波形解析进度条")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("self-test"), QStringLiteral("运行性能引擎自测")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("dump-predecode-fields"),
        QStringLiteral("List Predecode field paths without decoding samples")));
    parser.process(app);

    selfTest = parser.isSet(QStringLiteral("self-test"));
    const QStringList positional = parser.positionalArguments();
    if (!selfTest && positional.isEmpty()) parser.showHelp(2);
    if (!positional.isEmpty()) options.filePath = positional.first();
    options.outputDirectory = parser.value(QStringLiteral("out"));

    bool ok = false;
    options.ticksPerCycle = parser.value(QStringLiteral("ticks-per-cycle")).toLongLong(&ok);
    if (!ok || options.ticksPerCycle <= 0) {
        QTextStream(stderr) << "error: invalid --ticks-per-cycle\n";
        return false;
    }
    if (parser.isSet(QStringLiteral("start-cycle"))) {
        options.startCycle = parser.value(QStringLiteral("start-cycle")).toLongLong(&ok);
        if (!ok || options.startCycle < 0) return false;
    }
    if (parser.isSet(QStringLiteral("end-cycle"))) {
        options.endCycle = parser.value(QStringLiteral("end-cycle")).toLongLong(&ok);
        if (!ok || options.endCycle < 0) return false;
    }
    options.maxDecodedSamples =
        parser.value(QStringLiteral("max-samples")).toULongLong(&ok);
    if (!ok) return false;
    options.maxSignals = parser.value(QStringLiteral("max-signals")).toInt(&ok);
    if (!ok || options.maxSignals <= 0) return false;
    options.timelineBins =
        parser.value(QStringLiteral("timeline-bins")).toInt(&ok);
    if (!ok || options.timelineBins < 1 || options.timelineBins > 1000) {
        QTextStream(stderr) << "error: invalid --timeline-bins\n";
        return false;
    }
    options.showProgress = !parser.isSet(QStringLiteral("no-progress"));
    options.dumpPredecodeFields =
        parser.isSet(QStringLiteral("dump-predecode-fields"));
    return true;
}

int runSelfTest() {
    SignalSelection selection;
    selection.signalId = 1;
    selection.path = QStringLiteral("test.valid");
    selection.key = QStringLiteral("test");
    selection.category = QStringLiteral("test");

    WaveSignal signal;
    signal.signalId = 1;
    signal.kind = SignalKind::Bit;
    signal.width = 1;
    for (const auto& pair : QVector<QPair<qint64, quint64>>{{0, 0}, {10, 1}, {30, 0}}) {
        WaveSample sample;
        sample.time = pair.first;
        sample.rawBits = pair.second;
        sample.rawFieldsReady = true;
        signal.samples.push_back(sample);
    }
    const SignalMetrics metrics = analyzeSignal(selection, signal, 0, 50);
    if (metrics.activeTicks != 20 || metrics.transitionCount != 2) {
        QTextStream(stderr) << "self_test_failed: boolean integration\n";
        return 10;
    }
    auto makeBitSignal =
        [](int signalId,
           const QVector<QPair<qint64, quint64>>& values) {
            WaveSignal result;
            result.signalId = signalId;
            result.kind = SignalKind::Bit;
            result.width = 1;
            for (const auto& value : values) {
                WaveSample sample;
                sample.time = value.first;
                sample.rawBits = value.second;
                sample.rawFieldsReady = true;
                result.samples.push_back(sample);
            }
            return result;
        };
    WaveSignal issueWindowSlot0 = makeBitSignal(
        101, {{0, 0}, {23, 1}, {24, 0}, {90, 1}, {91, 0}});
    WaveSignal issueWindowSlot1 = makeBitSignal(
        102, {{0, 0}, {50, 1}, {60, 0}});
    const IssueActivityWindow issueWindow =
        findGlobalIssueActivityWindow(
            {&issueWindowSlot0, &issueWindowSlot1}, 0, 120, 10);
    if (!issueWindow.found ||
        issueWindow.firstActiveTick != 23 ||
        issueWindow.lastActiveTickExclusive != 91 ||
        issueWindow.alignedStartTick != 20 ||
        issueWindow.alignedEndTick != 100) {
        QTextStream(stderr)
            << "self_test_failed: global issue window\n";
        return 10;
    }
    const IssueActivityWindow clippedIssueWindow =
        findGlobalIssueActivityWindow(
            {&issueWindowSlot0}, 25, 95, 10);
    if (!clippedIssueWindow.found ||
        clippedIssueWindow.firstActiveTick != 90 ||
        clippedIssueWindow.lastActiveTickExclusive != 91 ||
        clippedIssueWindow.alignedStartTick != 90 ||
        clippedIssueWindow.alignedEndTick != 95) {
        QTextStream(stderr)
            << "self_test_failed: clipped global issue window\n";
        return 10;
    }
    WaveSignal noIssueWindow =
        makeBitSignal(103, {{0, 0}});
    if (findGlobalIssueActivityWindow(
            {&noIssueWindow}, 0, 120, 10).found) {
        QTextStream(stderr)
            << "self_test_failed: empty global issue window\n";
        return 10;
    }
    WaveSignal knownIssueSlot = signal;
    knownIssueSlot.samples[0].rawBits = 1;
    WaveSignal lateIssueSlot = signal;
    lateIssueSlot.samples.removeFirst();
    lateIssueSlot.samples.last().time = 20;
    IssueSlot issueSlot0;
    issueSlot0.slotIndex = 0;
    issueSlot0.slotCount = 2;
    issueSlot0.valid = &knownIssueSlot;
    IssueSlot issueSlot1;
    issueSlot1.slotIndex = 1;
    issueSlot1.slotCount = 2;
    issueSlot1.valid = &lateIssueSlot;
    const IssueContextMetrics partialIssue =
        analyzeIssueContext({&issueSlot0, &issueSlot1}, 2, 0, 40);
    const IssueContextMetrics missingIssue =
        analyzeIssueContext({&issueSlot0}, 2, 0, 40);
    WaveSignal alwaysValid = signal;
    for (WaveSample& sample : alwaysValid.samples) sample.rawBits = 1;
    IssueSlot featureIssueSlot;
    featureIssueSlot.slotIndex = 0;
    featureIssueSlot.slotCount = 1;
    featureIssueSlot.valid = &alwaysValid;
    featureIssueSlot.featureSignals.insert(
        QStringLiteral("barrier"), &signal);
    const IssueContextMetrics featureIssue =
        analyzeIssueContext({&featureIssueSlot}, 1, 0, 40);
    if (partialIssue.profile.issueActiveTicks != 20 ||
        partialIssue.profile.dualIssueTicks != 10 ||
        partialIssue.profile.idleTicks != 10 ||
        partialIssue.profile.capacityTicks != 70 ||
        partialIssue.profile.slotCapacityTicks != QVector<qint64>({40, 30}) ||
        partialIssue.profile.issuedTicks != 30 ||
        partialIssue.memoryClassifiedIssueTicks != 0 ||
        partialIssue.memoryUnclassifiedIssueTicks != 30 ||
        missingIssue.profile.issueActiveTicks != 0 ||
        missingIssue.profile.idleTicks != 0 ||
        featureIssue.featureActiveIssueTicks.value(
            QStringLiteral("barrier")) != 20 ||
        featureIssue.featureClassifiedIssueTicks.value(
            QStringLiteral("barrier")) != 40 ||
        featureIssue.featureUnclassifiedIssueTicks.value(
            QStringLiteral("barrier")) != 0 ||
        featureIssue.featureUnclassifiedIssueTicks.value(
            QStringLiteral("branch")) != 40) {
        QTextStream(stderr)
            << "self_test_failed: issue unknown-interval coverage\n";
        return 23;
    }
    SignalSelection cacheSelection = selection;
    cacheSelection.path = QStringLiteral("test.cache_hit");
    cacheSelection.key = QStringLiteral("cache_hit");
    cacheSelection.eventSemantics =
        waveperf::EventSemantics::PerCycleMask;
    const SignalMetrics mergedCachePulse =
        analyzeSignal(cacheSelection, signal, 0, 50);
    WaveSignal cacheMask = signal;
    cacheMask.kind = SignalKind::Bus;
    cacheMask.width = 4;
    cacheMask.samples[1].rawBits = 3;
    const SignalMetrics mergedCacheMask =
        analyzeSignal(cacheSelection, cacheMask, 0, 50);
    if (eventCountForMetrics(mergedCachePulse, 10) != 2 ||
        eventCountForMetrics(mergedCacheMask, 10) != 4) {
        QTextStream(stderr)
            << "self_test_failed: merged cache event pulse\n";
        return 20;
    }
    SignalSelection fifoEventSelection = selection;
    fifoEventSelection.path = QStringLiteral("test.m_num_read");
    fifoEventSelection.key = QStringLiteral("fifo_reads");
    fifoEventSelection.eventSemantics =
        waveperf::EventSemantics::PerCycleValue;
    WaveSignal fifoEvents = cacheMask;
    fifoEvents.samples[1].rawBits = 2;
    const SignalMetrics mergedFifoEvents =
        analyzeSignal(fifoEventSelection, fifoEvents, 0, 50);
    if (eventCountForMetrics(mergedFifoEvents, 10) != 4) {
        QTextStream(stderr)
            << "self_test_failed: merged per-cycle FIFO events\n";
        return 22;
    }

    WaveSignal counter;
    counter.signalId = 2;
    counter.kind = SignalKind::Bus;
    counter.width = 32;
    for (const auto& pair : QVector<QPair<qint64, quint64>>{{0, 2}, {20, 5}, {40, 3}}) {
        WaveSample sample;
        sample.time = pair.first;
        sample.rawBits = pair.second;
        sample.rawFieldsReady = true;
        counter.samples.push_back(sample);
    }
    const SignalMetrics counterMetrics = analyzeSignal(selection, counter, 0, 50);
    const SignalMetrics boundaryCounterMetrics =
        analyzeSignal(selection, counter, 20, 50);
    const double average =
        double(counterMetrics.weightedValueTicks /
               static_cast<long double>(counterMetrics.knownTicks));
    if (std::fabs(average - 3.4) > 1e-9 ||
        counterMetrics.positiveDelta != 3 ||
        boundaryCounterMetrics.positiveDelta != 3 ||
        counterMetrics.decreaseEvents != 1) {
        QTextStream(stderr) << "self_test_failed: numeric integration\n";
        return 11;
    }
    SignalSelection cacheCounterSelection = cacheSelection;
    cacheCounterSelection.path = QStringLiteral("test.cache_hit_count");
    cacheCounterSelection.eventSemantics =
        waveperf::EventSemantics::CumulativeCounter;
    const SignalMetrics cacheCounterMetrics =
        analyzeSignal(cacheCounterSelection, counter, 0, 50);
    if (eventCountForMetrics(cacheCounterMetrics, 10) != 3) {
        QTextStream(stderr)
            << "self_test_failed: cumulative cache event counter\n";
        return 21;
    }

    WaveSignal capacity;
    capacity.signalId = 3;
    capacity.kind = SignalKind::Bus;
    capacity.width = 32;
    WaveSample capacitySample;
    capacitySample.time = 0;
    capacitySample.rawBits = 4;
    capacitySample.rawFieldsReady = true;
    capacity.samples.push_back(capacitySample);
    const FullWindow fullWindow =
        analyzeFullWindow(&counter, &capacity, 0, 50);
    if (fullWindow.fullTicks != 20 || fullWindow.knownTicks != 50 ||
        fullWindow.expectedTicks != 50) {
        QTextStream(stderr) << "self_test_failed: FIFO full-rate integration\n";
        return 12;
    }
    WaveSignal lateCapacity = capacity;
    lateCapacity.samples[0].time = 20;
    const FullWindow partialFullWindow =
        analyzeFullWindow(&counter, &lateCapacity, 0, 50);
    if (partialFullWindow.knownTicks != 30 ||
        partialFullWindow.expectedTicks != 50) {
        QTextStream(stderr)
            << "self_test_failed: FIFO full-rate partial coverage\n";
        return 26;
    }

    if (issueClassKey(1) != QStringLiteral("thread") ||
        issueClassKey(2) != QStringLiteral("thread") ||
        issueClassKey(3) != QStringLiteral("group") ||
        issueClassKey(4) != QStringLiteral("group") ||
        issueClassKey(5) != QStringLiteral("cb") ||
        issueClassKey(6) != QStringLiteral("mma") ||
        issueClassKey(7) != QStringLiteral("mma") ||
        issueClassKey(0) != QStringLiteral("not_issue") ||
        !issueClassKey(99).startsWith(QStringLiteral("unknown_"))) {
        QTextStream(stderr) << "self_test_failed: issue type classification\n";
        return 13;
    }

    QVector<SignalSelection> coverageSelections;
    auto addCoveragePath =
        [&](const QString& path) {
            SignalSelection item;
            item.path = path;
            coverageSelections.push_back(item);
        };
    for (const QString& ppu :
         {QStringLiteral("gpu.m_ppu.[0]"),
          QStringLiteral("gpu.m_ppu.[1]")}) {
        addCoveragePath(
            ppu + QStringLiteral(
                      ".m_QPPUTOP[size=4].[0].m_QPPUCtrl."
                      "issue_inst_[size=2].[0].vld"));
        addCoveragePath(
            ppu + QStringLiteral(
                      ".m_QPPUTOP[size=4].[0].m_QPPUCtrl."
                      "sg_table_[size=16].[0].valid"));
    }
    addCoveragePath(
        QStringLiteral(
            "gpu.m_ppu.[1].m_QPPUTOP[size=4].[0].m_QPPUCtrl."
            "issue_inst_[size=2].[1].vld"));
    CoverageFamily issueCoverage;
    CoverageFamily sgCoverage;
    CoverageFamily qppuCoverage;
    collectQppuCoverage(coverageSelections, issueCoverage, sgCoverage,
                        qppuCoverage);
    if (issueCoverage.declaredSize != 4 ||
        issueCoverage.tracedSize != 3 ||
        issueCoverage.incompleteArrays != 1 ||
        sgCoverage.declaredSize != 32 ||
        sgCoverage.tracedSize != 2 ||
        sgCoverage.incompleteArrays != 2 ||
        qppuCoverage.declaredSize != 8 ||
        qppuCoverage.tracedSize != 2 ||
        qppuCoverage.incompleteArrays != 2) {
        QTextStream(stderr)
            << "self_test_failed: per-instance array coverage\n";
        return 19;
    }
    QVector<SignalSelection> missingWholeIssueArray =
        coverageSelections;
    missingWholeIssueArray.erase(
        std::remove_if(
            missingWholeIssueArray.begin(),
            missingWholeIssueArray.end(),
            [](const SignalSelection& item) {
                return item.path.startsWith(
                           QStringLiteral("gpu.m_ppu.[1]")) &&
                       item.path.contains(
                           QStringLiteral("issue_inst_"));
            }),
        missingWholeIssueArray.end());
    CoverageFamily missingWholeIssueCoverage;
    CoverageFamily missingWholeSgCoverage;
    CoverageFamily missingWholeQppuCoverage;
    collectQppuCoverage(
        missingWholeIssueArray, missingWholeIssueCoverage,
        missingWholeSgCoverage, missingWholeQppuCoverage);
    if (issueWindowCoverageIsComplete(
            false, missingWholeIssueCoverage,
            missingWholeQppuCoverage, {1})) {
        QTextStream(stderr)
            << "self_test_failed: missing whole issue array coverage\n";
        return 19;
    }

    QString architectureError;
    if (!waveperf::architectureProfilerSelfTest(architectureError)) {
        QTextStream(stderr) << "self_test_failed: " << architectureError << '\n';
        return 14;
    }

    QString bandwidthError;
    if (!waveperf::memoryBandwidthProfilerSelfTest(bandwidthError)) {
        QTextStream(stderr) << "self_test_failed: " << bandwidthError << '\n';
        return 15;
    }

    QVector<WaveSignal> threadSignals;
    QStringList threadPaths;
    threadSignals.reserve(100);
    threadPaths.reserve(100);
    auto appendThreadSignal =
        [&](const QString& path,
            int width,
            const QVector<QPair<qint64, quint64>>& samples) {
            WaveSignal item;
            item.signalId = 100 + threadSignals.size();
            item.kind = width == 1 ? SignalKind::Bit : SignalKind::Bus;
            item.width = width;
            for (const auto& pair : samples) {
                WaveSample waveSample;
                waveSample.time = pair.first;
                waveSample.rawBits = pair.second;
                waveSample.rawFieldsReady = true;
                item.samples.push_back(waveSample);
            }
            threadSignals.push_back(item);
            threadPaths.push_back(path);
        };
    const QString qppuPath =
        QStringLiteral("gpu.m_QPPUTOP[size=4].[0]");
    appendThreadSignal(
        qppuPath +
            QStringLiteral(
                ".m_QPPUEU.pt_BE_ThdCore_new_inst.m_num_read"),
        32, {{0, 0}, {10, 1}, {30, 0}});
    appendThreadSignal(
        qppuPath +
            QStringLiteral(".m_QPPUEU.m_EUState.group_info.sgId"),
        8, {{0, 3}});
    const QString maskBase =
        qppuPath +
        QStringLiteral(
            ".m_EU.shader_group_context_[size=4].[0][size=16].[3].");
    for (int lane = 0; lane < 32; ++lane) {
        appendThreadSignal(
            maskBase +
                QStringLiteral(
                    "thread_valid_indicator[size=32].[%1]")
                    .arg(lane),
            1, {{0, lane < 16 ? 1ull : 0ull}});
        appendThreadSignal(
            maskBase +
                QStringLiteral(
                    "thread_active_indicator[size=32].[%1]")
                    .arg(lane),
            1, {{0, lane < 12 ? 1ull : 0ull}});
        appendThreadSignal(
            maskBase +
                QStringLiteral(
                    "thread_execute_indicator[size=32].[%1]")
                    .arg(lane),
            1, {{0, lane < 8 ? 1ull : 0ull}});
    }
    QHash<QString, const WaveSignal*> threadSignalsByPath;
    for (int i = 0; i < threadSignals.size(); ++i) {
        threadSignalsByPath.insert(threadPaths.at(i),
                                   &threadSignals.at(i));
    }
    QStringList threadWarnings;
    const QJsonObject threadEfficiency =
        buildSgThreadEfficiency(threadSignalsByPath, 0, 30, 10,
                                threadWarnings);
    const QJsonObject threadEntry =
        threadEfficiency.value(QStringLiteral("entries"))
            .toArray().at(0).toObject();
    if (threadEfficiency.value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        threadEntry.value(QStringLiteral("thread_instructions")).toString() !=
            QStringLiteral("2") ||
        std::fabs(
            threadEntry
                    .value(QStringLiteral(
                        "valid_thread_occupancy_percent"))
                    .toDouble() -
            50.0) > 1e-9 ||
        std::fabs(
            threadEntry
                    .value(QStringLiteral(
                        "active_thread_efficiency_percent"))
                    .toDouble() -
            75.0) > 1e-9 ||
        std::fabs(
            threadEntry
                    .value(QStringLiteral("thread_efficiency_percent"))
                    .toDouble() -
            50.0) > 1e-9) {
        QTextStream(stderr)
            << "self_test_failed: per-SG thread efficiency\n";
        return 16;
    }

    QVector<WaveSignal> partialThreadSignals = threadSignals;
    partialThreadSignals[0].samples.removeFirst();
    QHash<QString, const WaveSignal*> partialThreadSignalsByPath;
    for (int i = 0; i < partialThreadSignals.size(); ++i) {
        partialThreadSignalsByPath.insert(
            threadPaths.at(i), &partialThreadSignals.at(i));
    }
    QStringList partialThreadWarnings;
    const QJsonObject partialThreadEfficiency =
        buildSgThreadEfficiency(
            partialThreadSignalsByPath, 0, 30, 10,
            partialThreadWarnings);
    if (partialThreadEfficiency
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("partial") ||
        partialThreadEfficiency
            .value(QStringLiteral(
                "thread_stream_coverage_complete")).toBool() ||
        partialThreadEfficiency.contains(
            QStringLiteral("overall_thread_efficiency_percent")) ||
        partialThreadEfficiency
            .value(QStringLiteral(
                "thread_stream_expected_cycles")).toString() !=
            QStringLiteral("3") ||
        partialThreadEfficiency
            .value(QStringLiteral(
                "thread_stream_observed_cycles")).toString() !=
            QStringLiteral("2")) {
        QTextStream(stderr)
            << "self_test_failed: partial thread stream coverage\n";
        return 25;
    }

    QVector<WaveSignal> inconsistentThreadSignals = threadSignals;
    const int lane20ExecuteSignal = 2 + 20 * 3 + 2;
    inconsistentThreadSignals[lane20ExecuteSignal]
        .samples[0].rawBits = 1;
    QHash<QString, const WaveSignal*> inconsistentThreadSignalsByPath;
    for (int i = 0; i < inconsistentThreadSignals.size(); ++i) {
        inconsistentThreadSignalsByPath.insert(
            threadPaths.at(i),
            &inconsistentThreadSignals.at(i));
    }
    QStringList inconsistentThreadWarnings;
    const QJsonObject inconsistentThreadEfficiency =
        buildSgThreadEfficiency(
            inconsistentThreadSignalsByPath, 0, 30, 10,
            inconsistentThreadWarnings);
    const QJsonObject inconsistentThreadEntry =
        inconsistentThreadEfficiency
            .value(QStringLiteral("entries")).toArray()
            .first().toObject();
    if (inconsistentThreadEfficiency
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("thread_masks_inconsistent") ||
        inconsistentThreadEfficiency.contains(
            QStringLiteral("overall_thread_efficiency_percent")) ||
        inconsistentThreadEntry.value(
            QStringLiteral("execute_mask_consistent")).toBool(true) ||
        inconsistentThreadEntry.contains(
            QStringLiteral("thread_efficiency_percent"))) {
        QTextStream(stderr)
            << "self_test_failed: inconsistent thread masks\n";
        return 26;
    }

    QString schedulerError;
    if (!waveperf::schedulerProfilerSelfTest(schedulerError)) {
        QTextStream(stderr) << "self_test_failed: " << schedulerError << '\n';
        return 17;
    }
    QString diagnosisError;
    if (!waveperf::performanceDiagnosisSelfTest(diagnosisError)) {
        QTextStream(stderr) << "self_test_failed: " << diagnosisError << '\n';
        return 24;
    }
    QString outputError;
    if (!waveperf::performanceOutputSelfTest(outputError)) {
        QTextStream(stderr) << "self_test_failed: " << outputError << '\n';
        return 18;
    }

    QTextStream(stdout) << "self_test_ok\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WavePerf"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2.0"));

    Options options;
    bool selfTest = false;
    if (!parseOptions(app, options, selfTest)) return 2;
    if (selfTest) return runSelfTest();

    QElapsedTimer totalTimer;
    totalTimer.start();
    QString error;
    WaveParser4Reader reader;
    QElapsedTimer openTimer;
    openTimer.start();
    printProgressStage(options.showProgress,
                       "[1/6] Loading waveform directory...");
    if (!reader.open(options.filePath, error)) {
        QTextStream(stderr) << "error: " << error << '\n';
        return 3;
    }
    const qint64 openMs = openTimer.elapsed();
    const WaveFile& directory = reader.directoryWave();
    if (options.showProgress) {
        std::fprintf(stderr,
                     "[1/6] Directory loaded: %llu signals in %lld ms\n",
                     static_cast<unsigned long long>(
                         directory.signalList.size()),
                     static_cast<long long>(openMs));
        std::fflush(stderr);
    }
    if (options.dumpPredecodeFields) {
        return dumpPredecodeFieldCatalog(directory);
    }

    qint64 startTick = directory.meta.start;
    qint64 endTick = directory.meta.end;
    if (options.startCycle >= 0 &&
        !checkedMultiply(options.startCycle, options.ticksPerCycle, startTick)) {
        QTextStream(stderr) << "error: --start-cycle overflow\n";
        return 4;
    }
    if (options.endCycle >= 0 &&
        !checkedMultiply(options.endCycle, options.ticksPerCycle, endTick)) {
        QTextStream(stderr) << "error: --end-cycle overflow\n";
        return 4;
    }
    startTick = qMax(startTick, directory.meta.start);
    endTick = qMin(endTick, directory.meta.end);
    if (endTick <= startTick) {
        QTextStream(stderr) << "error: empty analysis range\n";
        return 4;
    }
    const qint64 requestedStartTick = startTick;
    const qint64 requestedEndTick = endTick;
    QStringList warnings;
    QHash<int, QString> pathBySignalId;
    bool selectionTruncated = false;
    QElapsedTimer selectTimer;
    selectTimer.start();
    printProgressStage(options.showProgress,
                       "[2/6] Selecting performance signals...");
    QVector<SignalSelection> selections =
        selectSignals(directory, options, warnings, pathBySignalId,
                      selectionTruncated);
    const qint64 selectMs = selectTimer.elapsed();
    if (selections.isEmpty()) {
        QTextStream(stderr)
            << "error: no supported GPU performance signals were found\n";
        return 5;
    }
    if (options.showProgress) {
        std::fprintf(stderr,
                     "[2/6] Selected %d signals in %lld ms\n",
                     selections.size(), static_cast<long long>(selectMs));
        std::fflush(stderr);
    }

    CoverageFamily issueCoverage;
    CoverageFamily sgCoverage;
    CoverageFamily qppuCoverage;
    collectQppuCoverage(selections, issueCoverage, sgCoverage, qppuCoverage);

    static const QRegularExpression issueValidRegex(
        QStringLiteral(
            "^((.*)\\.issue_inst_(?:\\[size=(\\d+)\\])?"
            "\\.\\[(\\d+)\\])\\.vld$"));
    QVector<int> issueValidSignalIds;
    for (const SignalSelection& selection : selections) {
        if (issueValidRegex.match(selection.path).hasMatch()) {
            issueValidSignalIds.push_back(selection.signalId);
        }
    }

    bool issueActivityWindowFound = false;
    bool issueActivityWindowApplied = false;
    qint64 firstIssueTick = 0;
    qint64 lastIssueTickExclusive = 0;
    qint64 issueWindowScanMs = 0;
    const bool issueWindowCoverageComplete =
        issueWindowCoverageIsComplete(
            selectionTruncated, issueCoverage, qppuCoverage,
            issueValidSignalIds);
    if (issueWindowCoverageComplete) {
        printProgressStage(options.showProgress,
                           "[3/6] Locating global issue window...");
        QElapsedTimer issueWindowTimer;
        issueWindowTimer.start();
        WaveFile issueWindowWave;
        if (!reader.loadSignals(issueValidSignalIds, issueWindowWave, error,
                                options.maxDecodedSamples,
                                requestedStartTick, requestedEndTick,
                                WaveParser4Reader::LoadProgressCallback())) {
            QTextStream(stderr)
                << "error: failed to locate global issue window: "
                << error << '\n';
            return 6;
        }
        QVector<const WaveSignal*> issueValidSignals;
        issueValidSignals.reserve(issueValidSignalIds.size());
        for (const WaveSignal& signal : issueWindowWave.signalList) {
            issueValidSignals.push_back(&signal);
        }
        const IssueActivityWindow issueWindow =
            findGlobalIssueActivityWindow(
                issueValidSignals, requestedStartTick,
                requestedEndTick, options.ticksPerCycle);
        issueWindowScanMs = issueWindowTimer.elapsed();
        issueActivityWindowFound = issueWindow.found;
        if (issueWindow.found) {
            firstIssueTick = issueWindow.firstActiveTick;
            lastIssueTickExclusive =
                issueWindow.lastActiveTickExclusive;
            startTick = issueWindow.alignedStartTick;
            endTick = issueWindow.alignedEndTick;
            issueActivityWindowApplied =
                startTick != requestedStartTick ||
                endTick != requestedEndTick;
            if (options.showProgress) {
                std::fprintf(
                    stderr,
                    "[3/6] Global issue window: [%lld, %lld) -> "
                    "analysis [%lld, %lld) in %lld ms\n",
                    static_cast<long long>(firstIssueTick),
                    static_cast<long long>(lastIssueTickExclusive),
                    static_cast<long long>(startTick),
                    static_cast<long long>(endTick),
                    static_cast<long long>(issueWindowScanMs));
                std::fflush(stderr);
            }
        } else {
            warnings.push_back(
                QStringLiteral(
                    "外层分析范围内未观测到实际指令发射；无法按全局首末发射"
                    "收缩，保留原范围。"));
        }
    } else {
        warnings.push_back(
            QStringLiteral(
                "发射槽选择覆盖不完整；为避免截断真实首末发射，未自动收缩"
                "分析范围。"));
    }

    if (endTick <= startTick) {
        QTextStream(stderr)
            << "error: empty global issue analysis range\n";
        return 4;
    }
    const qint64 durationTicks = endTick - startTick;
    const double durationCycles =
        double(durationTicks) / double(options.ticksPerCycle);
    if (startTick % options.ticksPerCycle != 0 ||
        endTick % options.ticksPerCycle != 0) {
        warnings.push_back(
            QStringLiteral(
                "分析区间未与业务周期边界对齐；按周期解释的事件计数可能包含取整。"));
    }

    QVector<int> signalIds;
    signalIds.reserve(selections.size());
    for (const SignalSelection& selection : selections) signalIds.push_back(selection.signalId);

    WaveFile loaded;
    QElapsedTimer decodeTimer;
    decodeTimer.start();
    printProgressStage(options.showProgress,
                       "[4/6] Decoding waveform blocks...");
    WaveParseProgressBar progressBar(options.showProgress);
    progressBar.start();
    WaveParser4Reader::LoadProgressCallback progressCallback;
    if (progressBar.enabled()) {
        progressCallback = [&](quint64 completedBlocks, quint64 totalBlocks) {
            progressBar.update(completedBlocks, totalBlocks);
        };
    }
    if (!reader.loadSignals(signalIds, loaded, error, options.maxDecodedSamples,
                            startTick, endTick, progressCallback)) {
        progressBar.finish(false);
        QTextStream(stderr) << "error: " << error << '\n';
        return 6;
    }
    progressBar.finish(true);
    const qint64 decodeMs = decodeTimer.elapsed();
    if (options.showProgress) {
        std::fprintf(stderr, "[4/6] Waveform decoded in %lld ms\n",
                     static_cast<long long>(decodeMs));
        std::fflush(stderr);
    }

    QElapsedTimer analysisTimer;
    analysisTimer.start();
    printProgressStage(options.showProgress,
                       "[5/6] Analyzing performance...");
    QHash<int, const WaveSignal*> loadedById;
    const std::size_t loadedReserve =
        qMin<std::size_t>(
            loaded.signalList.size() * 2u + 1u,
            static_cast<std::size_t>((std::numeric_limits<int>::max)()));
    loadedById.reserve(static_cast<int>(loadedReserve));
    for (const WaveSignal& signal : loaded.signalList) {
        loadedById.insert(signal.signalId, &signal);
    }

    QVector<SignalMetrics> allMetrics;
    allMetrics.reserve(selections.size());
    qint64 decodedSamples = 0;
    qint64 samplesInRange = 0;
    for (const WaveSignal& signal : loaded.signalList) {
        decodedSamples += signal.samples.size();
    }
    int dynamicSignals = 0;
    int cumulativeCounterResetSignals = 0;
    QStringList cumulativeCounterResetExamples;
    for (const SignalSelection& selection : selections) {
        const WaveSignal* signal = loadedById.value(selection.signalId, nullptr);
        if (!signal) {
            warnings.push_back(
                QStringLiteral("选中的信号未被解码：%1").arg(selection.path));
            continue;
        }
        SignalMetrics metrics = analyzeSignal(selection, *signal, startTick, endTick);
        samplesInRange += metrics.samplesInRange;
        if (metrics.transitionCount > 0) ++dynamicSignals;
        if (selection.eventSemantics ==
                waveperf::EventSemantics::CumulativeCounter &&
            metrics.decreaseEvents > 0) {
            ++cumulativeCounterResetSignals;
            if (cumulativeCounterResetExamples.size() < 3) {
                cumulativeCounterResetExamples.push_back(
                    selection.path);
            }
        }
        if (!selection.helper) allMetrics.push_back(metrics);
    }
    if (cumulativeCounterResetSignals > 0) {
        warnings.push_back(
            QStringLiteral(
                "%1 个累计事件计数器在分析区间内下降；按复位或不连续处理，下降不计作事件；若实际发生回绕，事件数会偏低。示例：%2")
                .arg(cumulativeCounterResetSignals)
                .arg(cumulativeCounterResetExamples.join(
                    QStringLiteral("；"))));
    }

    auto appendMissingSignalWarnings =
        [&warnings](const CoverageFamily& family, const QString& familyName) {
            const int shown = qMin(16, family.missingSignalPaths.size());
            for (int i = 0; i < shown; ++i) {
                warnings.push_back(
                    QStringLiteral("Missing %1 target signal: %2")
                        .arg(familyName, family.missingSignalPaths.at(i)));
            }
            if (family.missingSignalCount > shown) {
                warnings.push_back(
                    QStringLiteral(
                        "Missing %1 target signals: %2 more path(s) omitted")
                        .arg(familyName)
                        .arg(family.missingSignalCount - shown));
            }
        };
    appendMissingSignalWarnings(issueCoverage, QStringLiteral("issue-slot"));
    appendMissingSignalWarnings(sgCoverage, QStringLiteral("SG-activity"));
    appendMissingSignalWarnings(qppuCoverage, QStringLiteral("QPPU"));

    if (issueCoverage.incompleteArrays > 0) {
        warnings.push_back(
            QStringLiteral("发射槽覆盖 %1/%2，%3 个 QPPU 的发射槽数组不完整")
                .arg(issueCoverage.tracedSize)
                .arg(issueCoverage.declaredSize)
                .arg(issueCoverage.incompleteArrays));
    }
    if (sgCoverage.incompleteArrays > 0) {
        warnings.push_back(
            QStringLiteral("SG 覆盖 %1/%2，%3 个 QPPU 的 SG 数组不完整")
                .arg(sgCoverage.tracedSize)
                .arg(sgCoverage.declaredSize)
                .arg(sgCoverage.incompleteArrays));
    }
    if (qppuCoverage.incompleteArrays > 0) {
        warnings.push_back(
            QStringLiteral("QPPU 覆盖 %1/%2，%3 个 PPU 的 QPPU 数组不完整")
                .arg(qppuCoverage.tracedSize)
                .arg(qppuCoverage.declaredSize)
                .arg(qppuCoverage.incompleteArrays));
    }
    if (dynamicSignals == 0) {
        warnings.push_back(QStringLiteral("所选范围内，匹配信号没有发生值变化"));
    }

    QHash<QString, const WaveSignal*> loadedByPath;
    for (const WaveSignal& signal : loaded.signalList) {
        loadedByPath.insert(pathBySignalId.value(signal.signalId), &signal);
    }

    QVector<IssueSlot> issueSlots;
    for (auto it = loadedByPath.constBegin(); it != loadedByPath.constEnd(); ++it) {
        const QRegularExpressionMatch match = issueValidRegex.match(it.key());
        if (!match.hasMatch()) continue;
        IssueSlot slot;
        slot.rootPath = match.captured(1);
        slot.contextPath = match.captured(2);
        slot.slotCount = match.captured(3).toInt();
        slot.slotIndex = match.captured(4).toInt();
        slot.valid = it.value();
        slot.mainType = loadedByPath.value(
            slot.rootPath + QStringLiteral(".preDecode.instType.mainType"), nullptr);
        slot.issueType = loadedByPath.value(
            slot.rootPath + QStringLiteral(".preDecode.instIssueType"), nullptr);
        slot.globalMem = loadedByPath.value(
            slot.rootPath + QStringLiteral(".preDecode.isGlobalMem"), nullptr);
        slot.localMem = loadedByPath.value(
            slot.rootPath + QStringLiteral(".preDecode.isLocalMem"), nullptr);
        for (const waveperf::InstructionFeatureSpec& spec :
             waveperf::instructionFeatureSpecs()) {
            slot.featureSignals.insert(
                spec.key,
                loadedByPath.value(
                    slot.rootPath + QStringLiteral(".preDecode.") +
                        spec.fieldPath,
                    nullptr));
        }
        issueSlots.push_back(slot);
    }
    std::sort(issueSlots.begin(), issueSlots.end(),
              [](const IssueSlot& left, const IssueSlot& right) {
                  return left.slotIndex < right.slotIndex;
              });

    qint64 issuedTicks = 0;
    qint64 anyIssueTicks = 0;
    qint64 dualIssueTicks = 0;
    qint64 globalMemIssueTicks = 0;
    qint64 localMemIssueTicks = 0;
    qint64 memoryClassifiedIssueTicks = 0;
    qint64 memoryUnclassifiedIssueTicks = 0;
    QMap<quint64, qint64> mainTypeTicks;
    QMap<quint64, qint64> issueTypeTicks;
    QMap<QString, qint64> issueClassTicks;
    QMap<QString, qint64> dualIssuePairTicks;
    QMap<QString, qint64> featureActiveIssueTicks;
    QMap<QString, qint64> featureClassifiedIssueTicks;
    QMap<QString, qint64> featureUnclassifiedIssueTicks;
    QMap<QString, int> featureSlotsTraced;
    qint64 classifiedIssueTicks = 0;
    qint64 unclassifiedIssueTicks = 0;
    int issueTypeSlotsTraced = 0;
    int memoryTypeSlotsTraced = 0;
    QHash<QString, QVector<const IssueSlot*>> issueSlotsByContext;
    QHash<QString, int> slotCapacityByContext;
    QHash<QString, QSet<int>> tracedSlotsByContext;
    QVector<waveperf::IssueContextView> issueContextProfiles;
    for (const IssueSlot& slot : issueSlots) {
        issueSlotsByContext[slot.contextPath].push_back(&slot);
        if (slot.issueType) ++issueTypeSlotsTraced;
        if (slot.globalMem && slot.localMem) ++memoryTypeSlotsTraced;
        for (const waveperf::InstructionFeatureSpec& spec :
             waveperf::instructionFeatureSpecs()) {
            if (slot.featureSignals.value(spec.key, nullptr)) {
                ++featureSlotsTraced[spec.key];
            }
        }
        tracedSlotsByContext[slot.contextPath].insert(slot.slotIndex);
        slotCapacityByContext[slot.contextPath] =
            qMax(slotCapacityByContext.value(slot.contextPath),
                 qMax(slot.slotCount, slot.slotIndex + 1));
    }

    for (auto contextIt = issueSlotsByContext.constBegin();
         contextIt != issueSlotsByContext.constEnd(); ++contextIt) {
        const int slotCount = slotCapacityByContext.value(contextIt.key());
        IssueContextMetrics context =
            analyzeIssueContext(contextIt.value(), slotCount,
                                startTick, endTick);
        context.profile.path = contextIt.key();
        issuedTicks += context.profile.issuedTicks;
        anyIssueTicks += context.profile.issueActiveTicks;
        dualIssueTicks += context.profile.dualIssueTicks;
        globalMemIssueTicks += context.globalMemIssueTicks;
        localMemIssueTicks += context.localMemIssueTicks;
        memoryClassifiedIssueTicks +=
            context.memoryClassifiedIssueTicks;
        memoryUnclassifiedIssueTicks +=
            context.memoryUnclassifiedIssueTicks;
        classifiedIssueTicks += context.classifiedIssueTicks;
        unclassifiedIssueTicks += context.unclassifiedIssueTicks;
        for (auto it = context.mainTypeTicks.constBegin();
             it != context.mainTypeTicks.constEnd(); ++it) {
            mainTypeTicks[it.key()] += it.value();
        }
        for (auto it = context.issueTypeTicks.constBegin();
             it != context.issueTypeTicks.constEnd(); ++it) {
            issueTypeTicks[it.key()] += it.value();
        }
        for (auto it = context.issueClassTicks.constBegin();
             it != context.issueClassTicks.constEnd(); ++it) {
            issueClassTicks[it.key()] += it.value();
        }
        for (auto it = context.dualIssuePairTicks.constBegin();
             it != context.dualIssuePairTicks.constEnd(); ++it) {
            dualIssuePairTicks[it.key()] += it.value();
        }
        for (const waveperf::InstructionFeatureSpec& spec :
             waveperf::instructionFeatureSpecs()) {
            featureActiveIssueTicks[spec.key] +=
                context.featureActiveIssueTicks.value(spec.key);
            featureClassifiedIssueTicks[spec.key] +=
                context.featureClassifiedIssueTicks.value(spec.key);
            featureUnclassifiedIssueTicks[spec.key] +=
                context.featureUnclassifiedIssueTicks.value(spec.key);
        }
        issueContextProfiles.push_back(context.profile);
    }

    const double issuedInstructions =
        double(issuedTicks) / double(options.ticksPerCycle);
    const double ipc = durationCycles > 0.0 ? issuedInstructions / durationCycles : 0.0;
    long double issueCapacityTicks = 0.0L;
    long double issueSlotActiveTicks = 0.0L;
    QVector<qint64> issueSlotTicks;
    QVector<qint64> issueSlotCapacityTicks;
    for (const waveperf::IssueContextView& context : issueContextProfiles) {
        if (issueSlotTicks.size() < context.slotActiveTicks.size()) {
            issueSlotTicks.resize(context.slotActiveTicks.size());
        }
        if (issueSlotCapacityTicks.size() < context.slotCapacityTicks.size()) {
            issueSlotCapacityTicks.resize(context.slotCapacityTicks.size());
        }
        for (int i = 0; i < context.slotActiveTicks.size(); ++i) {
            issueSlotTicks[i] += context.slotActiveTicks.at(i);
            issueSlotActiveTicks +=
                static_cast<long double>(context.slotActiveTicks.at(i));
        }
        for (int i = 0; i < context.slotCapacityTicks.size(); ++i) {
            issueSlotCapacityTicks[i] += context.slotCapacityTicks.at(i);
            issueCapacityTicks +=
                static_cast<long double>(context.slotCapacityTicks.at(i));
        }
    }
    long double observedQppuTicks = 0.0L;
    long double idleQppuTicks = 0.0L;
    for (const waveperf::IssueContextView& context : issueContextProfiles) {
        observedQppuTicks +=
            static_cast<long double>(context.issueActiveTicks) +
            static_cast<long double>(context.idleTicks);
        idleQppuTicks += static_cast<long double>(context.idleTicks);
    }
    const long double expectedQppuTicks =
        static_cast<long double>(durationTicks) *
        static_cast<long double>(issueSlotsByContext.size());
    const long double unknownQppuTicks =
        qMax(0.0L, expectedQppuTicks - observedQppuTicks);

    QHash<QString, const WaveSignal*> fifoOccupancyByResource;
    QHash<QString, const WaveSignal*> fifoCapacityByResource;
    QHash<QString, const WaveSignal*> queueOccupancyByResource;
    QHash<QString, const WaveSignal*> queueCapacityByResource;
    QHash<QString, const SignalMetrics*> fifoOccupancyMetricsByResource;
    QHash<QString, const SignalMetrics*> fifoCapacityMetricsByResource;
    QHash<QString, const SignalMetrics*> queueOccupancyMetricsByResource;
    QHash<QString, const SignalMetrics*> queueCapacityMetricsByResource;
    for (const SignalMetrics& metrics : allMetrics) {
        const QString resource = signalParentPath(metrics.selection.path);
        const WaveSignal* signal =
            loadedById.value(metrics.selection.signalId, nullptr);
        if (metrics.selection.key == QStringLiteral("fifo_occupancy")) {
            fifoOccupancyByResource.insert(resource, signal);
            fifoOccupancyMetricsByResource.insert(resource, &metrics);
        } else if (metrics.selection.key == QStringLiteral("fifo_capacity")) {
            fifoCapacityByResource.insert(resource, signal);
            fifoCapacityMetricsByResource.insert(resource, &metrics);
        } else if (metrics.selection.key == QStringLiteral("queue_occupancy")) {
            queueOccupancyByResource.insert(resource, signal);
            queueOccupancyMetricsByResource.insert(resource, &metrics);
        } else if (metrics.selection.key == QStringLiteral("queue_capacity")) {
            queueCapacityByResource.insert(resource, signal);
            queueCapacityMetricsByResource.insert(resource, &metrics);
        }
    }
    QHash<QString, FullWindow> fifoFullByResource;
    for (auto it = fifoOccupancyByResource.constBegin();
         it != fifoOccupancyByResource.constEnd(); ++it) {
        const WaveSignal* capacity = fifoCapacityByResource.value(it.key(), nullptr);
        if (!capacity) continue;
        fifoFullByResource.insert(
            it.key(),
            analyzeFullWindow(it.value(), capacity, startTick, endTick));
    }
    QHash<QString, FullWindow> queueFullByResource;
    for (auto it = queueOccupancyByResource.constBegin();
         it != queueOccupancyByResource.constEnd(); ++it) {
        const WaveSignal* capacity =
            queueCapacityByResource.value(it.key(), nullptr);
        if (!capacity) continue;
        queueFullByResource.insert(
            it.key(),
            analyzeFullWindow(it.value(), capacity, startTick, endTick));
    }

    auto averageValue = [](const SignalMetrics* metrics) {
        return metrics && metrics->knownTicks > 0
                   ? double(metrics->weightedValueTicks /
                            static_cast<long double>(metrics->knownTicks))
                   : 0.0;
    };
    auto fullPressureSection =
        [&](const QHash<QString, FullWindow>& windows,
            const QHash<QString, const SignalMetrics*>& occupancyByResource,
            const QHash<QString, const SignalMetrics*>& capacityByResource) {
            struct Entry {
                QString path;
                FullWindow window;
                double averageOccupancy = 0.0;
                double averageCapacity = 0.0;
            };
            QVector<Entry> pressured;
            int coveredResources = 0;
            int incompleteResources = 0;
            QSet<QString> candidateResources;
            for (auto it = occupancyByResource.constBegin();
                 it != occupancyByResource.constEnd(); ++it) {
                candidateResources.insert(it.key());
            }
            for (const QString& resource : candidateResources) {
                if (!windows.contains(resource)) {
                    ++incompleteResources;
                    continue;
                }
                const FullWindow window = windows.value(resource);
                if (window.expectedTicks <= 0 ||
                    window.knownTicks != window.expectedTicks) {
                    ++incompleteResources;
                    continue;
                }
                ++coveredResources;
                if (window.fullTicks <= 0) continue;
                pressured.push_back(
                    {resource, window,
                     averageValue(
                         occupancyByResource.value(resource, nullptr)),
                     averageValue(
                         capacityByResource.value(resource, nullptr))});
            }
            std::sort(
                pressured.begin(), pressured.end(),
                [](const Entry& left, const Entry& right) {
                    const long double leftRate =
                        static_cast<long double>(left.window.fullTicks) /
                        static_cast<long double>(left.window.knownTicks);
                    const long double rightRate =
                        static_cast<long double>(right.window.fullTicks) /
                        static_cast<long double>(right.window.knownTicks);
                    if (leftRate != rightRate) return leftRate > rightRate;
                    if (left.window.fullTicks != right.window.fullTicks) {
                        return left.window.fullTicks > right.window.fullTicks;
                    }
                    return left.path < right.path;
                });
            QJsonArray top;
            const int count = qMin(50, pressured.size());
            for (int i = 0; i < count; ++i) {
                const Entry& entry = pressured.at(i);
                QJsonObject object;
                object.insert(QStringLiteral("path"), entry.path);
                object.insert(
                    QStringLiteral("full_rate_percent"),
                    100.0 * double(entry.window.fullTicks) /
                        double(entry.window.knownTicks));
                object.insert(
                    QStringLiteral("full_cycles"),
                    double(entry.window.fullTicks) /
                        double(options.ticksPerCycle));
                object.insert(
                    QStringLiteral("observed_cycles"),
                    double(entry.window.knownTicks) /
                        double(options.ticksPerCycle));
                object.insert(QStringLiteral("average_occupancy"),
                              entry.averageOccupancy);
                object.insert(QStringLiteral("average_capacity"),
                              entry.averageCapacity);
                top.push_back(object);
            }
            QJsonObject section;
            section.insert(QStringLiteral("covered_resources"),
                           coveredResources);
            section.insert(QStringLiteral("incomplete_resources"),
                           incompleteResources);
            section.insert(QStringLiteral("pressured_resources"),
                           pressured.size());
            section.insert(QStringLiteral("top"), top);
            return section;
        };

    struct CreditPressureEntry {
        QString path;
        qint64 exhaustedTicks = 0;
        qint64 knownTicks = 0;
        double averageAvailable = 0.0;
        quint64 minimumAvailable = 0;
        quint64 maximumAvailable = 0;
    };
    QVector<CreditPressureEntry> creditPressureEntries;
    int coveredCreditResources = 0;
    int incompleteCreditResources = 0;
    for (const SignalMetrics& metrics : allMetrics) {
        const QString& key = metrics.selection.key;
        const bool availableCreditCounter =
            key == QStringLiteral("mma_ldmb_credit") ||
            key == QStringLiteral("mma_stmb_credit") ||
            key == QStringLiteral("icache_credit");
        if (!availableCreditCounter) continue;
        if (metrics.knownTicks != durationTicks) {
            ++incompleteCreditResources;
            continue;
        }
        ++coveredCreditResources;
        const qint64 exhaustedTicks =
            qMax<qint64>(0, metrics.knownTicks - metrics.activeTicks);
        if (exhaustedTicks <= 0) continue;
        creditPressureEntries.push_back(
            {metrics.selection.path,
             exhaustedTicks,
             metrics.knownTicks,
             averageValue(&metrics),
             metrics.minValue,
             metrics.maxValue});
    }
    std::sort(
        creditPressureEntries.begin(), creditPressureEntries.end(),
        [](const CreditPressureEntry& left,
           const CreditPressureEntry& right) {
            const long double leftRate =
                static_cast<long double>(left.exhaustedTicks) /
                static_cast<long double>(left.knownTicks);
            const long double rightRate =
                static_cast<long double>(right.exhaustedTicks) /
                static_cast<long double>(right.knownTicks);
            if (leftRate != rightRate) return leftRate > rightRate;
            if (left.exhaustedTicks != right.exhaustedTicks) {
                return left.exhaustedTicks > right.exhaustedTicks;
            }
            return left.path < right.path;
        });
    QJsonArray creditPressureTop;
    for (int i = 0; i < qMin(50, creditPressureEntries.size()); ++i) {
        const CreditPressureEntry& entry = creditPressureEntries.at(i);
        QJsonObject object;
        object.insert(QStringLiteral("path"), entry.path);
        object.insert(
            QStringLiteral("exhausted_rate_percent"),
            100.0 * double(entry.exhaustedTicks) / double(entry.knownTicks));
        object.insert(
            QStringLiteral("exhausted_cycles"),
            double(entry.exhaustedTicks) / double(options.ticksPerCycle));
        object.insert(
            QStringLiteral("observed_cycles"),
            double(entry.knownTicks) / double(options.ticksPerCycle));
        object.insert(QStringLiteral("average_available"),
                      entry.averageAvailable);
        object.insert(QStringLiteral("minimum_available"),
                      QString::number(entry.minimumAvailable));
        object.insert(QStringLiteral("maximum_available"),
                      QString::number(entry.maximumAvailable));
        creditPressureTop.push_back(object);
    }
    QJsonObject creditPressure;
    creditPressure.insert(QStringLiteral("covered_resources"),
                          coveredCreditResources);
    creditPressure.insert(QStringLiteral("incomplete_resources"),
                          incompleteCreditResources);
    creditPressure.insert(QStringLiteral("pressured_resources"),
                          creditPressureEntries.size());
    creditPressure.insert(QStringLiteral("top"), creditPressureTop);
    QJsonObject resourcePressure;
    resourcePressure.insert(
        QStringLiteral("definition"),
        QStringLiteral(
            "FIFO/Queue 满率 = occupancy >= capacity 的周期占比；"
            "Credit 耗尽率 = 可用 credit 为 0 的周期占比"));
    resourcePressure.insert(QStringLiteral("top_limit"), 50);
    resourcePressure.insert(
        QStringLiteral("fifo"),
        fullPressureSection(fifoFullByResource,
                            fifoOccupancyMetricsByResource,
                            fifoCapacityMetricsByResource));
    resourcePressure.insert(
        QStringLiteral("queue"),
        fullPressureSection(queueFullByResource,
                            queueOccupancyMetricsByResource,
                            queueCapacityMetricsByResource));
    resourcePressure.insert(QStringLiteral("credit"), creditPressure);

    QVector<waveperf::CounterView> architectureCounters;
    architectureCounters.reserve(allMetrics.size());
    for (const SignalMetrics& metrics : allMetrics) {
        waveperf::CounterView counter;
        counter.path = metrics.selection.path;
        counter.key = metrics.selection.key;
        counter.category = metrics.selection.category;
        counter.activeTicks = metrics.activeTicks;
        counter.unknownTicks = metrics.unknownTicks;
        counter.knownTicks = metrics.knownTicks;
        counter.transitions = metrics.transitionCount;
        counter.eventCount =
            eventCountForMetrics(metrics, options.ticksPerCycle);
        counter.eventDiscontinuities =
            metrics.selection.eventSemantics ==
                    waveperf::EventSemantics::CumulativeCounter
                ? metrics.decreaseEvents
                : 0;
        counter.weightedValueTicks = metrics.weightedValueTicks;
        const QString resource =
            signalParentPath(counter.path);
        if (counter.key == QStringLiteral("fifo_occupancy")) {
            const FullWindow full = fifoFullByResource.value(resource);
            if (fifoOccupancyMetricsByResource.value(resource, nullptr) ==
                    &metrics &&
                full.expectedTicks > 0 &&
                full.knownTicks == full.expectedTicks) {
                counter.fifoFullTicks = full.fullTicks;
                counter.fifoFullKnownTicks = full.knownTicks;
            }
        } else if (counter.key == QStringLiteral("queue_occupancy")) {
            const FullWindow full = queueFullByResource.value(resource);
            if (queueOccupancyMetricsByResource.value(resource, nullptr) ==
                    &metrics &&
                full.expectedTicks > 0 &&
                full.knownTicks == full.expectedTicks) {
                counter.queueFullTicks = full.fullTicks;
                counter.queueFullKnownTicks = full.knownTicks;
            }
        }
        architectureCounters.push_back(counter);
    }
    const waveperf::ArchitectureProfile architecture =
        waveperf::buildArchitectureProfile(
            architectureCounters, issueContextProfiles,
            durationTicks, options.ticksPerCycle);
    warnings.append(architecture.warnings);
    const QJsonObject sgThreadEfficiency =
        buildSgThreadEfficiency(
            loadedByPath, startTick, endTick,
            options.ticksPerCycle, warnings);
    const QJsonObject memoryBandwidth =
        waveperf::buildMemoryBandwidthProfile(
            loadedByPath, startTick, endTick, options.ticksPerCycle,
            dynamicSignals > 0, warnings);
    const QJsonObject scheduler =
        waveperf::buildSchedulerProfile(
            loadedByPath, startTick, endTick, options.ticksPerCycle,
            options.timelineBins, warnings);

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 7);
    QJsonObject fileObject;
    fileObject.insert(QStringLiteral("path"), QFileInfo(options.filePath).absoluteFilePath());
    fileObject.insert(QStringLiteral("bytes"), double(QFileInfo(options.filePath).size()));
    fileObject.insert(QStringLiteral("title"), directory.meta.title);
    fileObject.insert(QStringLiteral("timescale"), directory.meta.timescale);
    fileObject.insert(QStringLiteral("signals"),
                      QJsonValue(static_cast<double>(
                          directory.signalList.size())));
    fileObject.insert(QStringLiteral("start_tick"), QString::number(directory.meta.start));
    fileObject.insert(QStringLiteral("end_tick"), QString::number(directory.meta.end));
    root.insert(QStringLiteral("file"), fileObject);

    QJsonObject analysis;
    analysis.insert(QStringLiteral("requested_start_tick"),
                    QString::number(requestedStartTick));
    analysis.insert(QStringLiteral("requested_end_tick"),
                    QString::number(requestedEndTick));
    analysis.insert(QStringLiteral("start_tick"), QString::number(startTick));
    analysis.insert(QStringLiteral("end_tick"), QString::number(endTick));
    analysis.insert(QStringLiteral("range_basis"),
                    issueActivityWindowFound
                        ? QStringLiteral("global_issue_window")
                        : QStringLiteral("requested_range_fallback"));
    analysis.insert(QStringLiteral("global_issue_window_found"),
                    issueActivityWindowFound);
    analysis.insert(QStringLiteral("global_issue_window_applied"),
                    issueActivityWindowApplied);
    analysis.insert(QStringLiteral("global_issue_window_coverage_complete"),
                    issueWindowCoverageComplete);
    if (issueActivityWindowFound) {
        analysis.insert(QStringLiteral("first_issue_tick"),
                        QString::number(firstIssueTick));
        analysis.insert(QStringLiteral("last_issue_tick_exclusive"),
                        QString::number(lastIssueTickExclusive));
    }
    analysis.insert(QStringLiteral("ticks_per_cycle"), QString::number(options.ticksPerCycle));
    analysis.insert(QStringLiteral("duration_cycles"), durationCycles);
    analysis.insert(QStringLiteral("matched_signals"), selections.size());
    analysis.insert(QStringLiteral("reported_counters"), allMetrics.size());
    analysis.insert(QStringLiteral("decoded_samples"), double(decodedSamples));
    analysis.insert(QStringLiteral("samples_in_range"), double(samplesInRange));
    analysis.insert(QStringLiteral("dynamic_signals"), dynamicSignals);
    analysis.insert(QStringLiteral("open_ms"), double(openMs));
    analysis.insert(QStringLiteral("selection_ms"), double(selectMs));
    analysis.insert(QStringLiteral("issue_window_scan_ms"),
                    double(issueWindowScanMs));
    analysis.insert(QStringLiteral("decode_ms"), double(decodeMs));
    analysis.insert(QStringLiteral("analysis_ms"), double(totalTimer.elapsed()));
    root.insert(QStringLiteral("analysis"), analysis);

    QJsonObject coverage;
    QJsonArray missingSignalPaths;
    for (const CoverageFamily* family :
         {&issueCoverage, &sgCoverage, &qppuCoverage}) {
        for (const QString& path : family->missingSignalPaths) {
            missingSignalPaths.push_back(path);
        }
    }
    coverage.insert(QStringLiteral("missing_signal_count"),
                    issueCoverage.missingSignalCount +
                        sgCoverage.missingSignalCount +
                        qppuCoverage.missingSignalCount);
    coverage.insert(QStringLiteral("missing_signal_paths"),
                    missingSignalPaths);
    coverage.insert(QStringLiteral("issue_slots_declared"), issueCoverage.declaredSize);
    coverage.insert(QStringLiteral("signal_selection_complete"),
                    !selectionTruncated);
    coverage.insert(QStringLiteral("signal_selection_limit"),
                    options.maxSignals);
    coverage.insert(QStringLiteral("signal_selection_count"),
                    selections.size());
    coverage.insert(QStringLiteral("issue_slots_traced"), issueCoverage.tracedSize);
    coverage.insert(QStringLiteral("issue_slot_arrays"),
                    issueCoverage.arrays.size());
    coverage.insert(QStringLiteral("issue_slot_arrays_incomplete"),
                    issueCoverage.incompleteArrays);
    coverage.insert(
        QStringLiteral("issue_context_observed_cycles"),
        double(observedQppuTicks /
               static_cast<long double>(options.ticksPerCycle)));
    coverage.insert(
        QStringLiteral("issue_context_unknown_cycles"),
        double(unknownQppuTicks /
               static_cast<long double>(options.ticksPerCycle)));
    coverage.insert(QStringLiteral("sg_entries_declared"), sgCoverage.declaredSize);
    coverage.insert(QStringLiteral("sg_entries_traced"), sgCoverage.tracedSize);
    coverage.insert(QStringLiteral("sg_arrays"), sgCoverage.arrays.size());
    coverage.insert(QStringLiteral("sg_arrays_incomplete"),
                    sgCoverage.incompleteArrays);
    coverage.insert(QStringLiteral("qppu_elements_declared"), qppuCoverage.declaredSize);
    coverage.insert(QStringLiteral("qppu_elements_traced"), qppuCoverage.tracedSize);
    coverage.insert(QStringLiteral("qppu_arrays"), qppuCoverage.arrays.size());
    coverage.insert(QStringLiteral("qppu_arrays_incomplete"),
                    qppuCoverage.incompleteArrays);
    coverage.insert(
        QStringLiteral("sg_thread_streams"),
        sgThreadEfficiency.value(QStringLiteral("thread_streams")).toInt());
    coverage.insert(
        QStringLiteral("sg_thread_mask_entries"),
        sgThreadEfficiency.value(
            QStringLiteral("thread_mask_sg_entries")).toInt());
    root.insert(QStringLiteral("coverage"), coverage);

    QJsonObject summary;
    summary.insert(QStringLiteral("issued_instructions_estimate"), issuedInstructions);
    summary.insert(QStringLiteral("ipc_observed"), ipc);
    summary.insert(QStringLiteral("traced_issue_contexts"), issueSlotsByContext.size());
    summary.insert(
        QStringLiteral("issue_observed_cycles"),
        double(observedQppuTicks /
               static_cast<long double>(options.ticksPerCycle)));
    summary.insert(
        QStringLiteral("issue_utilization_percent"),
        observedQppuTicks > 0.0L
            ? double(100.0L * static_cast<long double>(issuedTicks) /
                     observedQppuTicks)
            : 0.0);
    summary.insert(QStringLiteral("issue_slot_utilization_percent"),
                   issueCapacityTicks > 0.0L
                       ? double(100.0L * issueSlotActiveTicks /
                                issueCapacityTicks)
                       : 0.0);
    const bool issueActivityCovered =
        !selectionTruncated &&
        !issueCoverage.arrays.isEmpty() &&
        issueCoverage.incompleteArrays == 0 &&
        issueCoverage.arrays.size() == qppuCoverage.tracedSize &&
        std::all_of(
            issueContextProfiles.constBegin(), issueContextProfiles.constEnd(),
            [durationTicks](const waveperf::IssueContextView& context) {
                return context.issueActiveTicks + context.idleTicks ==
                       durationTicks;
            });
    summary.insert(QStringLiteral("issue_activity_coverage_complete"),
                   issueActivityCovered);
    QJsonArray issueSlotSummary;
    const int issueSlotCount =
        qMax(issueSlotTicks.size(), issueSlotCapacityTicks.size());
    for (int i = 0; i < issueSlotCount; ++i) {
        const qint64 activeTicks =
            i < issueSlotTicks.size() ? issueSlotTicks.at(i) : 0;
        const qint64 capacityTicks =
            i < issueSlotCapacityTicks.size()
                ? issueSlotCapacityTicks.at(i)
                : 0;
        QJsonObject slot;
        slot.insert(QStringLiteral("index"), i);
        slot.insert(QStringLiteral("issued_cycles"),
                    double(activeTicks) / double(options.ticksPerCycle));
        slot.insert(QStringLiteral("observed_cycles"),
                    double(capacityTicks) / double(options.ticksPerCycle));
        slot.insert(
            QStringLiteral("utilization_percent"),
            capacityTicks > 0
                ? 100.0 * double(activeTicks) / double(capacityTicks)
                : 0.0);
        bool slotCovered = !slotCapacityByContext.isEmpty();
        qint64 expectedSlotCapacityTicks = 0;
        for (auto it = slotCapacityByContext.constBegin();
             it != slotCapacityByContext.constEnd(); ++it) {
            if (it.value() <= i) continue;
            expectedSlotCapacityTicks += durationTicks;
            if (!tracedSlotsByContext.value(it.key()).contains(i))
                slotCovered = false;
        }
        if (capacityTicks != expectedSlotCapacityTicks)
            slotCovered = false;
        slot.insert(QStringLiteral("covered"),
                    slotCovered && capacityTicks > 0);
        issueSlotSummary.push_back(slot);
    }
    summary.insert(QStringLiteral("issue_slots"), issueSlotSummary);
    const bool issueTypeCovered =
        issueActivityCovered && !issueSlots.isEmpty() &&
        issueTypeSlotsTraced == issueSlots.size() &&
        unclassifiedIssueTicks == 0 &&
        classifiedIssueTicks == issuedTicks;
    QJsonArray issueClassSummary;
    for (const QString& key :
         {QStringLiteral("thread"), QStringLiteral("group"),
          QStringLiteral("cb"), QStringLiteral("mma")}) {
        const qint64 classTicks = issueClassTicks.value(key);
        QJsonObject issueClass;
        issueClass.insert(QStringLiteral("key"), key);
        issueClass.insert(
            QStringLiteral("issued_cycles"),
            double(classTicks) / double(options.ticksPerCycle));
        issueClass.insert(
            QStringLiteral("qppu_cycle_rate_percent"),
            observedQppuTicks > 0.0L
                ? double(100.0L * static_cast<long double>(classTicks) /
                         observedQppuTicks)
                : 0.0);
        issueClass.insert(
            QStringLiteral("instruction_share_percent"),
            issuedTicks > 0
                ? 100.0 * double(classTicks) / double(issuedTicks)
                : 0.0);
        issueClass.insert(QStringLiteral("covered"), issueTypeCovered);
        issueClassSummary.push_back(issueClass);
    }
    summary.insert(QStringLiteral("issue_classes"), issueClassSummary);
    QJsonArray instructionFeatureSummary;
    QStringList incompleteFeatureFields;
    for (const waveperf::InstructionFeatureSpec& spec :
         waveperf::instructionFeatureSpecs()) {
        const qint64 activeTicks =
            featureActiveIssueTicks.value(spec.key);
        const qint64 classifiedTicks =
            featureClassifiedIssueTicks.value(spec.key);
        const qint64 unclassifiedTicks =
            featureUnclassifiedIssueTicks.value(spec.key);
        const int tracedSignals =
            featureSlotsTraced.value(spec.key);
        const bool covered =
            issueActivityCovered && !issueSlots.isEmpty() &&
            tracedSignals == issueSlots.size() &&
            unclassifiedTicks == 0 &&
            classifiedTicks == issuedTicks;
        if (!covered) incompleteFeatureFields.push_back(spec.fieldPath);

        QJsonObject feature;
        feature.insert(QStringLiteral("key"), spec.key);
        feature.insert(QStringLiteral("source_field"),
                       QStringLiteral("preDecode.") + spec.fieldPath);
        feature.insert(QStringLiteral("group"), spec.group);
        feature.insert(
            QStringLiteral("issued_cycles"),
            double(activeTicks) / double(options.ticksPerCycle));
        feature.insert(
            QStringLiteral("instruction_share_percent"),
            issuedTicks > 0
                ? 100.0 * double(activeTicks) / double(issuedTicks)
                : 0.0);
        feature.insert(
            QStringLiteral("classification_coverage_percent"),
            issuedTicks > 0
                ? 100.0 * double(classifiedTicks) / double(issuedTicks)
                : (covered ? 100.0 : 0.0));
        feature.insert(
            QStringLiteral("unclassified_issue_cycles"),
            double(unclassifiedTicks) / double(options.ticksPerCycle));
        feature.insert(QStringLiteral("signals_traced"), tracedSignals);
        feature.insert(QStringLiteral("signals_expected"),
                       issueSlots.size());
        feature.insert(QStringLiteral("covered"), covered);
        feature.insert(QStringLiteral("overlaps_other_features"), true);
        instructionFeatureSummary.push_back(feature);
    }
    summary.insert(QStringLiteral("instruction_features"),
                   instructionFeatureSummary);
    summary.insert(
        QStringLiteral("instruction_feature_coverage_complete"),
        incompleteFeatureFields.isEmpty() &&
        !waveperf::instructionFeatureSpecs().isEmpty());
    if (!incompleteFeatureFields.isEmpty() && !issueSlots.isEmpty()) {
        warnings.push_back(
            QStringLiteral(
                "Predecode feature coverage is partial for: %1")
                .arg(incompleteFeatureFields.join(
                    QStringLiteral(", "))));
    }
    summary.insert(
        QStringLiteral("issue_type_classification_coverage_percent"),
        issuedTicks > 0
            ? 100.0 * double(classifiedIssueTicks) / double(issuedTicks)
            : (issueTypeCovered ? 100.0 : 0.0));
    summary.insert(QStringLiteral("issue_type_signals_covered"),
                   issueTypeCovered);
    summary.insert(
        QStringLiteral("unclassified_issue_cycles"),
        double(unclassifiedIssueTicks) / double(options.ticksPerCycle));

    struct PairTicks {
        QString key;
        qint64 ticks = 0;
    };
    QVector<PairTicks> pairTicks;
    pairTicks.reserve(dualIssuePairTicks.size());
    for (auto it = dualIssuePairTicks.constBegin();
         it != dualIssuePairTicks.constEnd(); ++it) {
        pairTicks.push_back({it.key(), it.value()});
    }
    std::sort(pairTicks.begin(), pairTicks.end(),
              [](const PairTicks& left, const PairTicks& right) {
                  if (left.ticks != right.ticks) {
                      return left.ticks > right.ticks;
                  }
                  return left.key < right.key;
              });
    QJsonArray dualIssuePairs;
    for (const PairTicks& pair : pairTicks) {
        const int separator = pair.key.indexOf(QLatin1Char('>'));
        QJsonObject object;
        object.insert(QStringLiteral("main_class"),
                      separator >= 0 ? pair.key.left(separator) : pair.key);
        object.insert(QStringLiteral("shadow_class"),
                      separator >= 0 ? pair.key.mid(separator + 1)
                                     : QStringLiteral("unknown"));
        object.insert(QStringLiteral("cycles"),
                      double(pair.ticks) / double(options.ticksPerCycle));
        object.insert(
            QStringLiteral("dual_issue_share_percent"),
            dualIssueTicks > 0
                ? 100.0 * double(pair.ticks) / double(dualIssueTicks)
                : 0.0);
        object.insert(QStringLiteral("covered"), issueTypeCovered);
        dualIssuePairs.push_back(object);
    }
    summary.insert(QStringLiteral("dual_issue_pairs"), dualIssuePairs);
    summary.insert(QStringLiteral("dual_issue_type_coverage_complete"),
                   issueTypeCovered);
    summary.insert(QStringLiteral("issue_active_cycles"),
                   double(anyIssueTicks) / double(options.ticksPerCycle));
    summary.insert(QStringLiteral("dual_issue_cycles"),
                   double(dualIssueTicks) / double(options.ticksPerCycle));
    summary.insert(QStringLiteral("idle_cycles"),
                   double(idleQppuTicks /
                          static_cast<long double>(options.ticksPerCycle)));
    summary.insert(QStringLiteral("global_memory_issue_estimate"),
                   double(globalMemIssueTicks) / double(options.ticksPerCycle));
    summary.insert(QStringLiteral("local_memory_issue_estimate"),
                   double(localMemIssueTicks) / double(options.ticksPerCycle));
    const bool memoryIssueTypeCovered =
        issueActivityCovered && !issueSlots.isEmpty() &&
        memoryTypeSlotsTraced == issueSlots.size() &&
        memoryUnclassifiedIssueTicks == 0 &&
        memoryClassifiedIssueTicks == issuedTicks;
    summary.insert(
        QStringLiteral("memory_issue_classification_coverage_percent"),
        issuedTicks > 0
            ? 100.0 * double(memoryClassifiedIssueTicks) /
                  double(issuedTicks)
            : (memoryIssueTypeCovered ? 100.0 : 0.0));
    summary.insert(QStringLiteral("memory_issue_signals_covered"),
                   memoryIssueTypeCovered);
    summary.insert(
        QStringLiteral("unclassified_memory_issue_cycles"),
        double(memoryUnclassifiedIssueTicks) /
            double(options.ticksPerCycle));
    root.insert(QStringLiteral("summary"), summary);
    root.insert(QStringLiteral("sg_thread_efficiency"),
                sgThreadEfficiency);
    root.insert(QStringLiteral("memory_bandwidth"), memoryBandwidth);
    root.insert(QStringLiteral("scheduler"), scheduler);
    root.insert(QStringLiteral("resource_pressure"), resourcePressure);

    QJsonObject architectureObject;
    architectureObject.insert(QStringLiteral("traversal"), QStringLiteral("recursive"));
    architectureObject.insert(
        QStringLiteral("array_mode"),
        architecture.hasRepresentativeArrays
            ? QStringLiteral("array-first-representative")
            : QStringLiteral("explicit-elements"));
    architectureObject.insert(
        QStringLiteral("array_mode_cn"),
        architecture.hasRepresentativeArrays
            ? QStringLiteral("拍平数组：仅观测 [0]，其余实例为代表性投影")
            : QStringLiteral("正常数组：逐个观测显式数组元素"));
    architectureObject.insert(QStringLiteral("observed_class_instances"),
                              architecture.observedInstances);
    architectureObject.insert(QStringLiteral("represented_class_instances"),
                              QString::number(architecture.representedInstances));
    architectureObject.insert(QStringLiteral("roots"), architecture.roots);
    root.insert(QStringLiteral("architecture"), architectureObject);
    auto histogramJson = [options](const QMap<quint64, qint64>& histogram) {
        QJsonObject object;
        for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
            object.insert(QString::number(it.key()),
                          double(it.value()) / double(options.ticksPerCycle));
        }
        return object;
    };
    root.insert(QStringLiteral("issue_main_type_cycles"), histogramJson(mainTypeTicks));
    root.insert(QStringLiteral("issue_type_cycles"), histogramJson(issueTypeTicks));
    const qint64 notIssueValidTicks = issueTypeTicks.value(1u, 0);
    summary.insert(
        QStringLiteral("not_issue_valid_cycles"),
        double(notIssueValidTicks) / double(options.ticksPerCycle));
    root.insert(QStringLiteral("summary"), summary);
    if (notIssueValidTicks > 0) {
        warnings.push_back(
            QStringLiteral(
                "检测到 %1 个业务周期同时满足 issue_inst.vld=1 且 "
                "instIssueType=NotIssue(1)；该组合与发射槽语义矛盾，"
                "对应 PC/类型会原样显示，但不能当作有效指令分类。")
                .arg(double(notIssueValidTicks) /
                     double(options.ticksPerCycle)));
    }
    if (unclassifiedIssueTicks > 0) {
        warnings.push_back(
            QStringLiteral("存在 %1 个业务周期的有效发射无法映射到 "
                           "Thread/Group/CB/MMA；原始枚举直方图已保留")
                .arg(double(unclassifiedIssueTicks) /
                     double(options.ticksPerCycle)));
    }

    QJsonArray warningArray;
    for (const QString& warning : warnings) warningArray.push_back(warning);
    root.insert(QStringLiteral("warnings"), warningArray);
    if (options.showProgress) {
        std::fprintf(stderr, "[5/6] Analysis complete in %lld ms\n",
                     static_cast<long long>(analysisTimer.elapsed()));
        std::fflush(stderr);
    }

    if (options.outputDirectory.isEmpty()) {
        const QFileInfo inputInfo(options.filePath);
        options.outputDirectory =
            QDir(inputInfo.absolutePath())
                .filePath(inputInfo.completeBaseName() +
                          QStringLiteral(".perf"));
    }
    printProgressStage(options.showProgress,
                       "[6/6] Writing performance report...");
    if (!waveperf::writePerformanceBundle(
            options.outputDirectory, root, error)) {
        QTextStream(stderr) << "error: " << error << '\n';
        return 7;
    }
    if (options.showProgress) {
        const QByteArray outputPath =
            QDir(options.outputDirectory).absolutePath().toLocal8Bit();
        std::fprintf(stderr, "[6/6] Report written: %s\n",
                     outputPath.constData());
        std::fflush(stderr);
    }

    QTextStream out(stdout);
    out << waveperf::buildPerformanceConsoleSummary(
        root, options.outputDirectory);
    return 0;
}
