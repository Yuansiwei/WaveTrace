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

static const WaveSignal* expectSignal(const WaveFile& wave, const QString& path) {
    const int index = findSignalByPath(wave, path);
    if (index < 0) {
        std::cerr << "missing path " << path.toLocal8Bit().constData() << "\n";
        return nullptr;
    }
    return &wave.signalList.at(index);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        return fail(QStringLiteral("usage: smoke_wvz4_complex_class_parser <complex_class.wvz4>"));
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

    const WaveSignal* epoch = expectSignal(wave, QStringLiteral("top.epoch"));
    const WaveSignal* mode = expectSignal(wave, QStringLiteral("top.flags.mode"));
    const WaveSignal* page = expectSignal(wave, QStringLiteral("top.flags.page"));
    const WaveSignal* aliasLo = expectSignal(wave, QStringLiteral("top.alias.lo"));
    const WaveSignal* aliasHi = expectSignal(wave, QStringLiteral("top.alias.hi"));
    const WaveSignal* slot3Id = expectSignal(wave, QStringLiteral("top.slots.[3].id"));
    const WaveSignal* slot7A = expectSignal(wave, QStringLiteral("top.slots.[7].a"));
    const WaveSignal* globalDirty = expectSignal(wave, QStringLiteral("top.global_dirty"));
    const WaveSignal* sampledCount = expectSignal(wave, QStringLiteral("top.sampled.count"));
    const WaveSignal* dynamicCount = expectSignal(wave, QStringLiteral("top.dynamic.payload.count"));
    if (!epoch || !mode || !page || !aliasLo || !aliasHi || !slot3Id || !slot7A ||
        !globalDirty || !sampledCount || !dynamicCount) {
        return 2;
    }

    if (expectValueAt(*epoch, 0, 1u, QStringLiteral("epoch@cycle0")) != 0) return 1;
    if (expectValueAt(*epoch, 10, 2u, QStringLiteral("epoch@cycle1")) != 0) return 1;
    if (expectValueAt(*epoch, 20, 2u, QStringLiteral("epoch@cycle2")) != 0) return 1;

    if (expectValueAt(*mode, 0, 1u, QStringLiteral("mode@cycle0")) != 0) return 1;
    if (expectValueAt(*mode, 10, 5u, QStringLiteral("mode@cycle1")) != 0) return 1;
    if (expectValueAt(*page, 0, 0x12u, QStringLiteral("page@cycle0")) != 0) return 1;
    if (expectValueAt(*page, 10, 0x5au, QStringLiteral("page@cycle1")) != 0) return 1;

    if (expectValueAt(*aliasLo, 0, 0x0304u, QStringLiteral("aliasLo@cycle0")) != 0) return 1;
    if (expectValueAt(*aliasHi, 0, 0x0102u, QStringLiteral("aliasHi@cycle0")) != 0) return 1;
    if (expectValueAt(*aliasLo, 10, 0xabcdu, QStringLiteral("aliasLo@cycle1")) != 0) return 1;
    if (expectValueAt(*aliasHi, 10, 0x1234u, QStringLiteral("aliasHi@cycle1")) != 0) return 1;
    if (expectValueAt(*aliasHi, 40, 0xfeedu, QStringLiteral("aliasHi@cycle4")) != 0) return 1;

    if (expectValueAt(*slot3Id, 0, 1003u, QStringLiteral("slot3Id@cycle0")) != 0) return 1;
    if (expectValueAt(*slot3Id, 10, 0x11112222u, QStringLiteral("slot3Id@cycle1")) != 0) return 1;
    if (expectValueAt(*slot7A, 0, 17u, QStringLiteral("slot7A@cycle0")) != 0) return 1;
    if (expectValueAt(*slot7A, 30, 0x7777u, QStringLiteral("slot7A@cycle3")) != 0) return 1;
    if (expectValueAt(*globalDirty, 0, 0x1000u, QStringLiteral("globalDirty@cycle0")) != 0) return 1;
    if (expectValueAt(*globalDirty, 10, 0x5555u, QStringLiteral("globalDirty@cycle1")) != 0) return 1;

    if (expectValueAt(*sampledCount, 0, 10u, QStringLiteral("sampled@cycle0")) != 0) return 1;
    if (expectValueAt(*sampledCount, 10, 111u, QStringLiteral("sampled@cycle1")) != 0) return 1;
    if (expectValueAt(*sampledCount, 20, 111u, QStringLiteral("sampled@cycle2")) != 0) return 1;
    if (expectValueAt(*sampledCount, 30, 333u, QStringLiteral("sampled@cycle3")) != 0) return 1;
    if (expectValueAt(*dynamicCount, 0, 20u, QStringLiteral("dynamic@cycle0")) != 0) return 1;
    if (expectValueAt(*dynamicCount, 10, 222u, QStringLiteral("dynamic@cycle1")) != 0) return 1;
    if (expectValueAt(*dynamicCount, 30, 222u, QStringLiteral("dynamic@cycle3")) != 0) return 1;
    if (expectValueAt(*dynamicCount, 40, 444u, QStringLiteral("dynamic@cycle4")) != 0) return 1;

    if (hasExactSample(*epoch, 20)) return fail(QStringLiteral("top.epoch has unexpected exact cycle2 sample"));
    if (hasExactSample(*slot3Id, 20)) return fail(QStringLiteral("top.slots.[3].id has unexpected exact cycle2 sample"));
    if (hasExactSample(*slot7A, 10)) return fail(QStringLiteral("top.slots.[7].a has unexpected exact cycle1 sample"));
    if (hasExactSample(*slot7A, 20)) return fail(QStringLiteral("top.slots.[7].a has unexpected exact cycle2 sample"));
    if (hasExactSample(*sampledCount, 20)) return fail(QStringLiteral("top.sampled.count has unexpected exact cycle2 sample"));
    if (hasExactSample(*dynamicCount, 30)) return fail(QStringLiteral("top.dynamic.payload.count has unexpected exact cycle3 sample"));

    std::cout << "complex_class_parser_ok signals=" << wave.signalList.size()
              << " epoch_samples=" << epoch->samples.size()
              << " slot7a_samples=" << slot7A->samples.size()
              << " sampled_samples=" << sampledCount->samples.size()
              << " dynamic_samples=" << dynamicCount->samples.size() << "\n";
    return 0;
}
