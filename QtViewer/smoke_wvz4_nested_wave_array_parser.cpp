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

static const WaveSignal* expectSignal(const WaveFile& wave, const QString& path) {
    const int index = findSignalByPath(wave, path);
    if (index < 0) {
        std::cerr << "missing path " << path.toLocal8Bit().constData() << "\n";
        return nullptr;
    }
    return &wave.signalList.at(index);
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
        return fail(QStringLiteral("usage: smoke_wvz4_nested_wave_array_parser <nested_array.wvz4>"));
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

    const WaveSignal* m00 = expectSignal(wave, QStringLiteral("top.matrix.[0].[0]"));
    const WaveSignal* m01 = expectSignal(wave, QStringLiteral("top.matrix.[0].[1]"));
    const WaveSignal* m10 = expectSignal(wave, QStringLiteral("top.matrix.[1].[0]"));
    const WaveSignal* m11 = expectSignal(wave, QStringLiteral("top.matrix.[1].[1]"));
    const WaveSignal* c010 = expectSignal(wave, QStringLiteral("top.cube.[0].[1].[0]"));
    const WaveSignal* c011 = expectSignal(wave, QStringLiteral("top.cube.[0].[1].[1]"));
    const WaveSignal* c100 = expectSignal(wave, QStringLiteral("top.cube.[1].[0].[0]"));
    const WaveSignal* c101 = expectSignal(wave, QStringLiteral("top.cube.[1].[0].[1]"));
    if (!m00 || !m01 || !m10 || !m11 || !c010 || !c011 || !c100 || !c101) return 2;

    if (expectValueAt(*m00, 0, 0u, QStringLiteral("m00@cycle0")) != 0) return 1;
    if (expectValueAt(*m00, 10, 100u, QStringLiteral("m00@cycle1")) != 0) return 1;
    if (expectValueAt(*m01, 10, 1u, QStringLiteral("m01@cycle1")) != 0) return 1;
    if (expectValueAt(*m10, 30, 10u, QStringLiteral("m10@cycle3")) != 0) return 1;
    if (expectValueAt(*m11, 30, 211u, QStringLiteral("m11@cycle3")) != 0) return 1;
    if (expectValueAt(*c010, 10, 10u, QStringLiteral("c010@cycle1")) != 0) return 1;
    if (expectValueAt(*c011, 10, 1111u, QStringLiteral("c011@cycle1")) != 0) return 1;
    if (expectValueAt(*c100, 30, 2100u, QStringLiteral("c100@cycle3")) != 0) return 1;
    if (expectValueAt(*c101, 30, 101u, QStringLiteral("c101@cycle3")) != 0) return 1;

    if (m00->samples.size() != 2) return fail(QStringLiteral("m00 expected exactly 2 samples"));
    if (m01->samples.size() != 1) return fail(QStringLiteral("m01 should not be dirtied by matrix[0][0]"));
    if (m10->samples.size() != 1) return fail(QStringLiteral("m10 should not be dirtied by row data()[1]"));
    if (m11->samples.size() != 2) return fail(QStringLiteral("m11 expected exactly 2 samples"));
    if (c010->samples.size() != 1) return fail(QStringLiteral("c010 should not be dirtied by cube[0][1][1]"));
    if (c011->samples.size() != 2) return fail(QStringLiteral("c011 expected exactly 2 samples"));
    if (c100->samples.size() != 2) return fail(QStringLiteral("c100 expected exactly 2 samples"));
    if (c101->samples.size() != 1) return fail(QStringLiteral("c101 should not be dirtied by cube[1][0].data()[0]"));
    if (hasExactSample(*m01, 10)) return fail(QStringLiteral("m01 has unexpected exact cycle1 sample"));
    if (hasExactSample(*m10, 30)) return fail(QStringLiteral("m10 has unexpected exact cycle3 sample"));
    if (hasExactSample(*c010, 10)) return fail(QStringLiteral("c010 has unexpected exact cycle1 sample"));
    if (hasExactSample(*c101, 30)) return fail(QStringLiteral("c101 has unexpected exact cycle3 sample"));

    std::cout << "nested_wave_array_parser_ok signals=" << wave.signalList.size()
              << " m00_samples=" << m00->samples.size()
              << " m01_samples=" << m01->samples.size()
              << " m10_samples=" << m10->samples.size()
              << " m11_samples=" << m11->samples.size()
              << " c011_samples=" << c011->samples.size()
              << " c100_samples=" << c100->samples.size()
              << "\n";
    return 0;
}
