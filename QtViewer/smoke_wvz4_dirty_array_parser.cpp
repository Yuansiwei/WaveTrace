#include "WaveParser4.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdint>
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
        parts.prepend(node.name);
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
                    QStringLiteral(": expected 0x") + QString::number(expected, 16) +
                    QStringLiteral(" got 0x") + QString::number(actual, 16) +
                    QStringLiteral(" at t=") + QString::number(time));
    }
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        return fail(QStringLiteral("usage: smoke_wvz4_dirty_array_parser <dirty_array.wvz4>"));
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

    const int f3Index = findSignalByPath(wave, QStringLiteral("top.slots.[7].f3"));
    const int f4Index = findSignalByPath(wave, QStringLiteral("top.slots.[9].f4"));
    const int stableIndex = findSignalByPath(wave, QStringLiteral("top.slots.[7].f4"));
    if (f3Index < 0) return fail(QStringLiteral("missing path top.slots.[7].f3"));
    if (f4Index < 0) return fail(QStringLiteral("missing path top.slots.[9].f4"));
    if (stableIndex < 0) return fail(QStringLiteral("missing path top.slots.[7].f4"));

    const WaveSignal& f3 = wave.signalList.at(f3Index);
    const WaveSignal& f4 = wave.signalList.at(f4Index);
    const WaveSignal& stable = wave.signalList.at(stableIndex);

    if (f3.samples.size() != 2) return fail(QStringLiteral("top.slots.[7].f3 expected 2 compacted samples"));
    if (f4.samples.size() != 2) return fail(QStringLiteral("top.slots.[9].f4 expected 2 compacted samples"));
    if (stable.samples.size() != 1) return fail(QStringLiteral("top.slots.[7].f4 expected only initial sample"));

    if (expectValueAt(f3, 0, 7003u, QStringLiteral("f3@cycle0")) != 0) return 1;
    if (expectValueAt(f3, 10, 0x12345678u, QStringLiteral("f3@cycle1")) != 0) return 1;
    if (expectValueAt(f3, 20, 0x12345678u, QStringLiteral("f3@cycle2")) != 0) return 1;
    if (expectValueAt(f3, 30, 0x12345678u, QStringLiteral("f3@cycle3")) != 0) return 1;

    if (expectValueAt(f4, 0, 9004u, QStringLiteral("f4@cycle0")) != 0) return 1;
    if (expectValueAt(f4, 20, 9004u, QStringLiteral("f4@cycle2")) != 0) return 1;
    if (expectValueAt(f4, 30, 0x87654321u, QStringLiteral("f4@cycle3")) != 0) return 1;

    if (expectValueAt(stable, 30, 7004u, QStringLiteral("stable@cycle3")) != 0) return 1;
    if (hasExactSample(f3, 20)) return fail(QStringLiteral("top.slots.[7].f3 has unexpected exact cycle2 sample"));
    if (hasExactSample(f4, 20)) return fail(QStringLiteral("top.slots.[9].f4 has unexpected exact cycle2 sample"));

    std::cout << "dirty_array_wvz4_parser_ok signals=" << wave.signalList.size()
              << " f3_samples=" << f3.samples.size()
              << " f4_samples=" << f4.samples.size() << "\n";
    return 0;
}
