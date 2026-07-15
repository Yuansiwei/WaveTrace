#include "WaveParser4.h"

#include <QCoreApplication>
#include <QString>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace {

void hash_bytes(std::uint64_t& hash, const char* data, int size) {
    for (int i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= UINT64_C(1099511628211);
    }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value) {
    hash_bytes(hash, reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
}

std::uint64_t peak_working_set_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#endif
    return 0u;
}

struct FixedProbe {
    int outer = 0;
    int inner = 0;
    int signalId = -1;
    int sampleCount = 0;
    qint64 times[3] = {};
    quint64 values[3] = {};
};

bool verifyFixedPressure(const WaveParser4Reader& reader,
                         const WaveFile& wave,
                         QString& error) {
    static const int kOuter = 64;
    static const int kInner = 16384;
    static const int kSignals = kOuter * kInner;
    static const int kTreeSlots = 1 + 1 + 1 + kOuter + kSignals;

    if (!wave.tree.valid || wave.tree.rootNodeIds.size() != 1 ||
        wave.signalList.size() != kSignals ||
        wave.tree.nodesById.size() != kTreeSlots ||
        wave.tree.signalIndexToNodeId.size() != kSignals) {
        error = QStringLiteral("unexpected fixed-array topology counts");
        return false;
    }

    const int rootId = wave.tree.rootNodeIds.constFirst();
    if (rootId <= 0 || rootId >= wave.tree.nodesById.size()) {
        error = QStringLiteral("invalid root id");
        return false;
    }
    const WaveTreeNode& root = wave.tree.nodesById.at(rootId);
    if (!root.valid || waveTreeNodeSegmentName(wave.tree, rootId) != QStringLiteral("top") || root.parentId != 0) {
        error = QStringLiteral("invalid top root");
        return false;
    }

    const int matrixId = root.firstChild;
    if (matrixId <= 0 || matrixId >= wave.tree.nodesById.size()) {
        error = QStringLiteral("missing top.matrix");
        return false;
    }
    const WaveTreeNode& matrix = wave.tree.nodesById.at(matrixId);
    if (!matrix.valid || waveTreeNodeSegmentName(wave.tree, matrixId) != QStringLiteral("matrix") ||
        matrix.parentId != rootId || matrix.nextSibling != 0) {
        error = QStringLiteral("invalid top.matrix node");
        return false;
    }

    FixedProbe probes[] = {
        {0, 0, -1, 3, {0, 10, 30}, {0, 0x11111111u, 0x33333333u}},
        {0, 16383, -1, 2, {0, 20, 0}, {0, 0x00016383u, 0}},
        {1, 0, -1, 2, {0, 30, 0}, {0, 0x10000000u, 0}},
        {31, 8192, -1, 2, {0, 10, 0}, {0, 0x31181920u, 0}},
        {63, 0, -1, 1, {0, 0, 0}, {0, 0, 0}},
        {63, 16383, -1, 2, {0, 20, 0}, {0, 0x06316383u, 0}},
    };

    QVector<quint8> seenSignals(kSignals, 0);
    int outerId = matrix.firstChild;
    for (int outer = 0; outer < kOuter; ++outer) {
        if (outerId <= 0 || outerId >= wave.tree.nodesById.size()) {
            error = QStringLiteral("missing outer array node %1").arg(outer);
            return false;
        }
        const WaveTreeNode& outerNode = wave.tree.nodesById.at(outerId);
        const QString expectedOuter = QStringLiteral("[%1]").arg(outer);
        if (!outerNode.valid || !waveNameTokenIsArrayIndex(outerNode.nameToken) ||
            waveNameTokenValue(outerNode.nameToken) != quint32(outer) ||
            waveTreeNodeSegmentName(wave.tree, outerId) != expectedOuter ||
            outerNode.parentId != matrixId || outerNode.signalIndex >= 0) {
            error = QStringLiteral("bad outer array node %1").arg(outer);
            return false;
        }

        int leafId = outerNode.firstChild;
        for (int inner = 0; inner < kInner; ++inner) {
            if (leafId <= 0 || leafId >= wave.tree.nodesById.size()) {
                error = QStringLiteral("missing leaf [%1][%2]").arg(outer).arg(inner);
                return false;
            }
            const WaveTreeNode& leaf = wave.tree.nodesById.at(leafId);
            const QString expectedLeaf = QStringLiteral("[%1]").arg(inner);
            if (!leaf.valid || !waveNameTokenIsArrayIndex(leaf.nameToken) ||
                waveNameTokenValue(leaf.nameToken) != quint32(inner) ||
                waveTreeNodeSegmentName(wave.tree, leafId) != expectedLeaf || leaf.parentId != outerId ||
                leaf.firstChild != 0 || leaf.signalIndex < 0 ||
                leaf.signalIndex >= wave.signalList.size()) {
                error = QStringLiteral("bad leaf [%1][%2]").arg(outer).arg(inner);
                return false;
            }

            const int signalIndex = leaf.signalIndex;
            if (seenSignals.at(signalIndex) != 0 ||
                wave.tree.signalIndexToNodeId.at(signalIndex) != leafId) {
                error = QStringLiteral("duplicate or mismatched signal mapping at [%1][%2]")
                            .arg(outer).arg(inner);
                return false;
            }
            seenSignals[signalIndex] = 1;
            const WaveSignal& signal = wave.signalList.at(signalIndex);
            if (signal.signalId != leaf.signalId || signal.width != 32 ||
                signal.bitOffset != 0 || signal.storageId < 0 || !signal.name.isEmpty()) {
                error = QStringLiteral("bad signal metadata at [%1][%2]").arg(outer).arg(inner);
                return false;
            }

            for (FixedProbe& probe : probes) {
                if (probe.outer == outer && probe.inner == inner) {
                    probe.signalId = signal.signalId;
                }
            }
            leafId = leaf.nextSibling;
        }
        if (leafId != 0) {
            error = QStringLiteral("extra leaf after outer index %1").arg(outer);
            return false;
        }
        outerId = outerNode.nextSibling;
    }
    if (outerId != 0) {
        error = QStringLiteral("extra outer array node");
        return false;
    }

    QVector<int> signalIds;
    signalIds.reserve(kSignals);
    for (const WaveSignal& signal : wave.signalList) signalIds.push_back(signal.signalId);
    for (const FixedProbe& probe : probes) {
        if (probe.signalId < 0) {
            error = QStringLiteral("probe signal was not found");
            return false;
        }
    }

    WaveFile loaded;
    if (!reader.loadSignals(signalIds, loaded, error, quint64(kSignals) + 32u, 0,
                            (std::numeric_limits<qint64>::max)())) {
        return false;
    }
    if (loaded.signalList.size() != kSignals) {
        error = QStringLiteral("full sample load returned the wrong signal count");
        return false;
    }
    for (int signalIndex = 0; signalIndex < loaded.signalList.size(); ++signalIndex) {
        const WaveSignal& signal = loaded.signalList.at(signalIndex);
        const FixedProbe* expectedProbe = nullptr;
        for (const FixedProbe& probe : probes) {
            if (probe.signalId == signal.signalId) {
                expectedProbe = &probe;
                break;
            }
        }
        const int expectedCount = expectedProbe ? expectedProbe->sampleCount : 1;
        if (!signal.samplesLoaded || signal.samples.size() != expectedCount) {
            error = QStringLiteral("wrong sample count for signal id %1").arg(signal.signalId);
            return false;
        }
        for (int i = 0; i < expectedCount; ++i) {
            const qint64 expectedTime = expectedProbe ? expectedProbe->times[i] : 0;
            const quint64 expectedValue = expectedProbe ? expectedProbe->values[i] : 0;
            const WaveSample& sample = signal.samples.at(i);
            if (!sample.rawFieldsReady || sample.isAbsent || sample.isZ ||
                sample.time != expectedTime || sample.rawBits != expectedValue) {
                error = QStringLiteral("wrong sample for signal id %1 at item %2")
                            .arg(signal.signalId).arg(i);
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: layout_parser_bench <input.wvz4> [verify-fixed-pressure]\n";
        return 2;
    }

    const auto begin = std::chrono::steady_clock::now();
    WaveParser4Reader reader;
    QString error;
    if (!reader.open(QString::fromLocal8Bit(argv[1]), error)) {
        std::cerr << "reader.open failed: " << error.toLocal8Bit().constData() << "\n";
        return 3;
    }
    const auto end = std::chrono::steady_clock::now();
    const WaveFile& wave = reader.directoryWave();
    if (argc == 3 && QString::fromLocal8Bit(argv[2]) == QStringLiteral("verify-fixed-pressure")) {
        if (!verifyFixedPressure(reader, wave, error)) {
            std::cerr << "fixed pressure verification failed: "
                      << error.toLocal8Bit().constData() << "\n";
            return 4;
        }
    }
    std::uint64_t semantic_hash = UINT64_C(1469598103934665603);
    for (int nodeId = 0; nodeId < wave.tree.nodesById.size(); ++nodeId) {
        const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
        hash_value(semantic_hash, node.nodeId);
        hash_value(semantic_hash, node.parentId);
        hash_value(semantic_hash, node.firstChild);
        hash_value(semantic_hash, node.nextSibling);
        hash_value(semantic_hash, node.signalId);
        const QByteArray name = waveTreeNodeSegmentName(wave.tree, nodeId).toUtf8();
        hash_bytes(semantic_hash, name.constData(), name.size());
    }
    for (int signalIndex = 0; signalIndex < wave.signalList.size(); ++signalIndex) {
        const WaveSignal& signal = wave.signalList.at(signalIndex);
        hash_value(semantic_hash, signal.signalId);
        hash_value(semantic_hash, signal.storageId);
        hash_value(semantic_hash, signal.bitOffset);
        hash_value(semantic_hash, signal.width);
        const QByteArray name = waveSignalSegmentName(wave, signalIndex).toUtf8();
        hash_bytes(semantic_hash, name.constData(), name.size());
    }
    const double elapsed = std::chrono::duration<double, std::milli>(end - begin).count();
    std::cout << "parse_ms=" << elapsed
              << " signals=" << wave.signalList.size()
              << " tree_slots=" << wave.tree.nodesById.size()
              << " roots=" << wave.tree.rootNodeIds.size()
              << " semantic_hash=" << semantic_hash
              << " parent_peak_bytes=" << peak_working_set_bytes()
              << " correctness=" << (argc == 3 ? "ok" : "not_requested")
              << "\n";
    return 0;
}
