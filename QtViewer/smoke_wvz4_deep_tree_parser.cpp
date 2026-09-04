#include "WaveParser4.h"

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <QVector>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace {

std::uint64_t peak_working_set_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#endif
    return 0u;
}

QString expected_path(std::uint32_t zero_based, std::uint32_t fanout, std::uint32_t depth) {
    static const char* const level_names[] = {
        "chiplet", "subsystem", "pipeline", "stage", "bank", "row", "lane",
        "cluster", "engine", "slice", "unit", "group", "thread", "slot"
    };
    QVector<std::uint32_t> digits;
    digits.resize(int(depth));
    for (int level = int(depth) - 1; level >= 0; --level) {
        digits[level] = zero_based % fanout;
        zero_based /= fanout;
    }
    QString path = QStringLiteral("top");
    for (std::uint32_t level = 0; level < depth; ++level) {
        path += QLatin1Char('.');
        path += QString::fromLatin1(level_names[level]);
        path += QStringLiteral(".[%1]").arg(digits[int(level)]);
    }
    return path;
}

bool checked_case_counts(std::uint32_t fanout, std::uint32_t depth,
                         std::uint64_t& leaves, std::uint64_t& nodes) {
    leaves = 1u;
    std::uint64_t arrays = 0u;
    std::uint64_t named = 1u;
    for (std::uint32_t level = 0; level < depth; ++level) {
        named += leaves;
        if (leaves > (std::numeric_limits<std::uint64_t>::max)() / fanout) return false;
        leaves *= fanout;
        arrays += leaves;
    }
    nodes = named + arrays;
    return leaves < UINT32_MAX && nodes < UINT32_MAX;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: smoke_wvz4_deep_tree_parser <input.wvz4> [fanout=10] [depth=7]\n";
        return 2;
    }
    const std::uint32_t fanout = argc >= 3
        ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 10u;
    const std::uint32_t depth = argc >= 4
        ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 7u;
    std::uint64_t expected_leaves = 0u;
    std::uint64_t expected_nodes = 0u;
    if (fanout < 2u || depth == 0u || depth > 14u ||
        !checked_case_counts(fanout, depth, expected_leaves, expected_nodes)) {
        std::cerr << "invalid fanout/depth\n";
        return 2;
    }

    const auto open_begin = std::chrono::steady_clock::now();
    WaveParser4Reader reader;
    QString error;
    if (!reader.open(QString::fromLocal8Bit(argv[1]), error)) {
        std::cerr << "reader.open failed: " << error.toLocal8Bit().constData() << "\n";
        return 3;
    }
    const auto open_end = std::chrono::steady_clock::now();
    const WaveFile& wave = reader.directoryWave();
    if (!wave.tree.valid || wave.tree.rootNodeIds.size() != 1 ||
        std::uint64_t(wave.tree.nodesById.size()) != expected_nodes + 1u ||
        std::uint64_t(wave.signalList.size()) != expected_leaves ||
        wave.tree.namesById.size() != int(depth + 2u)) {
        std::cerr << "topology count mismatch: names=" << wave.tree.namesById.size()
                  << " node_slots=" << wave.tree.nodesById.size()
                  << " signals=" << wave.signalList.size() << "\n";
        return 4;
    }

    QVector<std::uint32_t> zero_based_probes;
    zero_based_probes << 0u << (fanout - 1u) << fanout
                      << std::uint32_t(expected_leaves / 2u)
                      << std::uint32_t(expected_leaves - 2u)
                      << std::uint32_t(expected_leaves - 1u);
    QVector<int> signal_ids;
    for (std::uint32_t zero_based : zero_based_probes) {
        const int signal_index = int(zero_based);
        const QString actual = waveSignalFullPath(wave, signal_index);
        const QString expected = expected_path(zero_based, fanout, depth);
        if (actual != expected) {
            std::cerr << "path mismatch at signal index " << signal_index
                      << "\nexpected=" << expected.toLocal8Bit().constData()
                      << "\nactual=" << actual.toLocal8Bit().constData() << "\n";
            return 5;
        }

        int node_id = wave.tree.signalIndexToNodeId.at(signal_index);
        int segments = 0;
        while (node_id > 0 && node_id < wave.tree.nodesById.size()) {
            const WaveTreeNode& node = wave.tree.nodesById.at(node_id);
            if (!node.valid) break;
            ++segments;
            node_id = node.parentId;
        }
        if (segments != int(1u + depth * 2u)) {
            std::cerr << "wrong path depth at signal index " << signal_index
                      << ": " << segments << "\n";
            return 5;
        }
        signal_ids.push_back(wave.signalList.at(signal_index).signalId);
    }

    const auto sample_begin = std::chrono::steady_clock::now();
    WaveFile loaded;
    if (!reader.loadSignals(signal_ids, loaded, error, 128u, 0,
                            (std::numeric_limits<qint64>::max)())) {
        std::cerr << "loadSignals failed: " << error.toLocal8Bit().constData() << "\n";
        return 6;
    }
    const auto sample_end = std::chrono::steady_clock::now();
    if (loaded.signalList.size() != signal_ids.size()) {
        std::cerr << "selected signal count mismatch\n";
        return 6;
    }

    QHash<int, const WaveSignal*> loaded_by_id;
    for (const WaveSignal& signal : loaded.signalList) loaded_by_id.insert(signal.signalId, &signal);
    for (int signal_id : signal_ids) {
        const WaveSignal* signal = loaded_by_id.value(signal_id, nullptr);
        const int expected_samples = (signal_id == 1 || signal_id == int(expected_leaves)) ? 3 : 2;
        if (!signal || !signal->samplesLoaded || signal->samples.size() != expected_samples ||
            signal->samples.at(0).time != 0 || signal->samples.at(0).rawBits != 0u ||
            signal->samples.at(1).time != 10 ||
            signal->samples.at(1).rawBits != (0xA5000000u ^ std::uint32_t(signal_id))) {
            std::cerr << "sample mismatch for signal id " << signal_id << "\n";
            return 7;
        }
        if (expected_samples == 3) {
            const quint64 expected20 = signal_id == 1 ? 0x11111111u : 0xEEEEEEEEu;
            if (signal->samples.at(2).time != 20 || signal->samples.at(2).rawBits != expected20) {
                std::cerr << "cycle 20 sample mismatch for signal id " << signal_id << "\n";
                return 7;
            }
        }
    }

    const double open_ms = std::chrono::duration<double, std::milli>(open_end - open_begin).count();
    const double sample_ms = std::chrono::duration<double, std::milli>(sample_end - sample_begin).count();
    std::cout << "correctness=ok"
              << " fanout=" << fanout
              << " depth=" << depth
              << " path_segments=" << (1u + depth * 2u)
              << " names=" << (wave.tree.namesById.size() - 1)
              << " nodes=" << expected_nodes
              << " leaves=" << expected_leaves
              << " open_ms=" << open_ms
              << " selected_load_ms=" << sample_ms
              << " peak_working_set_bytes=" << peak_working_set_bytes()
              << "\n";
    return 0;
}
