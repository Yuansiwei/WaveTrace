#include "WaveParser4.h"

#include <QCoreApplication>
#include <QStringList>

#include <iostream>

static QString fullPathForSignal(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();
    if (!wave.tree.valid ||
        signalIndex >= wave.tree.signalIndexToNodeId.size() ||
        wave.tree.signalIndexToNodeId.at(signalIndex) <= 0) {
        return wave.signalList.at(signalIndex).name;
    }

    QStringList parts;
    int nodeId = wave.tree.signalIndexToNodeId.at(signalIndex);
    int guard = 0;
    while (nodeId > 0 &&
           nodeId < wave.tree.nodesById.size() &&
           wave.tree.nodesById.at(nodeId).valid &&
           guard < wave.tree.nodesById.size()) {
        const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
        parts.prepend(waveTreeNodeSegmentName(wave.tree, nodeId));
        nodeId = node.parentId;
        ++guard;
    }
    return parts.join(QStringLiteral("."));
}

static int findSignalByPath(const WaveFile& wave, const QString& path) {
    for (int i = 0; i < wave.signalList.size(); ++i) {
        if (fullPathForSignal(wave, i) == path) return i;
    }
    return -1;
}

static bool valueAtOrBefore(const WaveSignal& sig, qint64 time, quint64& out) {
    bool found = false;
    WaveSample best;
    for (const WaveSample& s : sig.samples) {
        if (s.time > time) break;
        best = s;
        found = true;
    }
    if (!found || best.isAbsent || best.isZ || !best.rawFieldsReady) return false;
    out = best.rawBits;
    return true;
}

static bool hasExactSample(const WaveSignal& sig, qint64 time) {
    for (const WaveSample& s : sig.samples) {
        if (s.time == time) return true;
    }
    return false;
}

static int fail(const QString& msg) {
    std::cerr << msg.toLocal8Bit().constData() << "\n";
    return 1;
}

static int expectValueAt(const WaveSignal& sig,
                         qint64 time,
                         quint64 expected,
                         const QString& label) {
    quint64 actual = 0;
    if (!valueAtOrBefore(sig, time, actual)) {
        return fail(label + QStringLiteral(": missing sample at/before t=") + QString::number(time));
    }
    if (actual != expected) {
        return fail(label +
                    QStringLiteral(": expected ") + QString::number(expected) +
                    QStringLiteral(" got ") + QString::number(actual) +
                    QStringLiteral(" at t=") + QString::number(time));
    }
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        return fail(QStringLiteral("usage: smoke_wvz4_dirty_peek_dynamic_parser <dirty_peek_dynamic.wvz4>"));
    }

    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.loadRawSamples = true;
    options.autoLoadFirstSignalCount = -1;
    options.maxDecodedSamples = 1000000;

    WaveFile wave;
    QString error;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), wave, error, options)) {
        return fail(QStringLiteral("loadFromFile failed: ") + error);
    }

    const int peekIndex = findSignalByPath(wave, QStringLiteral("top.peek.count"));
    const int dynamicIndex = findSignalByPath(wave, QStringLiteral("top.dynamic.value.count"));
    const int stablePeekIndex = findSignalByPath(wave, QStringLiteral("top.peek.delta"));
    const int stableDynamicIndex = findSignalByPath(wave, QStringLiteral("top.dynamic.value.delta"));
    if (peekIndex < 0) return fail(QStringLiteral("missing path top.peek.count"));
    if (dynamicIndex < 0) return fail(QStringLiteral("missing path top.dynamic.value.count"));
    if (stablePeekIndex < 0) return fail(QStringLiteral("missing path top.peek.delta"));
    if (stableDynamicIndex < 0) return fail(QStringLiteral("missing path top.dynamic.value.delta"));

    const WaveSignal& peek = wave.signalList.at(peekIndex);
    const WaveSignal& dynamic = wave.signalList.at(dynamicIndex);
    const WaveSignal& stablePeek = wave.signalList.at(stablePeekIndex);
    const WaveSignal& stableDynamic = wave.signalList.at(stableDynamicIndex);

    if (peek.samples.size() != 2) return fail(QStringLiteral("top.peek.count expected 2 compacted samples"));
    if (dynamic.samples.size() != 2) return fail(QStringLiteral("top.dynamic.value.count expected 2 compacted samples"));
    if (stablePeek.samples.size() != 2) return fail(QStringLiteral("top.peek.delta expected 2 compacted samples"));
    if (stableDynamic.samples.size() != 2) return fail(QStringLiteral("top.dynamic.value.delta expected 2 compacted samples"));

    if (expectValueAt(peek, 0, 100u, QStringLiteral("peek@cycle0")) != 0) return 1;
    if (expectValueAt(peek, 10, 111u, QStringLiteral("peek@cycle1")) != 0) return 1;
    if (expectValueAt(peek, 20, 111u, QStringLiteral("peek@cycle2")) != 0) return 1;
    if (expectValueAt(peek, 30, 111u, QStringLiteral("peek@cycle3")) != 0) return 1;

    if (expectValueAt(dynamic, 0, 200u, QStringLiteral("dynamic@cycle0")) != 0) return 1;
    if (expectValueAt(dynamic, 20, 200u, QStringLiteral("dynamic@cycle2")) != 0) return 1;
    if (expectValueAt(dynamic, 30, 222u, QStringLiteral("dynamic@cycle3")) != 0) return 1;

    if (hasExactSample(peek, 20)) return fail(QStringLiteral("top.peek.count has unexpected exact cycle2 sample"));
    if (hasExactSample(dynamic, 10)) return fail(QStringLiteral("top.dynamic.value.count has unexpected exact cycle1 sample"));
    if (hasExactSample(dynamic, 20)) return fail(QStringLiteral("top.dynamic.value.count has unexpected exact cycle2 sample"));

    std::cout << "dirty_peek_dynamic_parser_ok signals=" << wave.signalList.size()
              << " peek_samples=" << peek.samples.size()
              << " dynamic_samples=" << dynamic.samples.size() << "\n";
    return 0;
}
