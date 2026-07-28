#include "wave_path_wvz4_recorder.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct FieldShape {
    const char* name;
    wave::ValueKind kind;
    std::uint32_t bit_width;
};

const FieldShape kFields[] = {
    {"valid", wave::ValueKind::Bool, 1u},
    {"opcode", wave::ValueKind::UnsignedInt, 16u},
    {"signed_delta", wave::ValueKind::SignedInt, 32u},
    {"pc", wave::ValueKind::UnsignedInt, 64u},
    {"score", wave::ValueKind::Float64, 64u},
    {"state", wave::ValueKind::Enum, 8u},
    {"target", wave::ValueKind::PointerAddress, 64u},
    {"mask", wave::ValueKind::UnsignedInt, 32u},
};

bool parse_u64_arg(const char* text, std::uint64_t& out) {
    if (!text || !text[0]) return false;
    char* end = NULL;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

wave::NodeId add_named_node(PathStableWvz4Recorder& recorder,
                            wave::NodeId& next_node_id,
                            wave::NodeId parent_id,
                            const char* name,
                            wave::NodeKind kind) {
    const wave::NodeId node_id = next_node_id++;
    recorder.on_node_declared_fast(node_id, parent_id, std::string(name), kind);
    return node_id;
}

wave::NodeId add_index_node(PathStableWvz4Recorder& recorder,
                            wave::NodeId& next_node_id,
                            wave::NodeId parent_id,
                            std::uint32_t index,
                            wave::NodeKind kind) {
    const wave::NodeId node_id = next_node_id++;
    recorder.on_node_declared_array_index_fast(node_id, parent_id, index, kind);
    return node_id;
}

void declare_complex_topology(PathStableWvz4Recorder& recorder,
                              std::uint32_t signal_count) {
    // 64 * 4 * 4 * 4 * 8 * 8 = 262,144 field groups.  At 20M
    // signals, every group contains roughly 76 visible leaves.
    const std::uint32_t cluster_count = 64u;
    const std::uint32_t dppu_count = 4u;
    const std::uint32_t ppu_count = 4u;
    const std::uint32_t qppu_count = 4u;
    const std::uint32_t bank_count = 8u;
    const std::uint32_t field_count =
        static_cast<std::uint32_t>(sizeof(kFields) / sizeof(kFields[0]));
    const std::uint64_t group_count =
        std::uint64_t(cluster_count) * dppu_count * ppu_count *
        qppu_count * bank_count * field_count;

    wave::NodeId next_node_id = 1u;
    wave::TrackId next_track_id = 1u;
    std::uint64_t group_ordinal = 0u;

    const wave::NodeId root = add_named_node(
        recorder, next_node_id, 0u, "top", wave::NodeKind::Aggregate);
    const wave::NodeId clusters = add_named_node(
        recorder, next_node_id, root, "cluster", wave::NodeKind::FixedIndexedContainer);

    for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
        const wave::NodeId cluster_node = add_index_node(
            recorder, next_node_id, clusters, cluster, wave::NodeKind::Aggregate);
        const wave::NodeId dppus = add_named_node(
            recorder, next_node_id, cluster_node, "dppu", wave::NodeKind::FixedIndexedContainer);
        for (std::uint32_t dppu = 0; dppu < dppu_count; ++dppu) {
            const wave::NodeId dppu_node = add_index_node(
                recorder, next_node_id, dppus, dppu, wave::NodeKind::Aggregate);
            const wave::NodeId ppus = add_named_node(
                recorder, next_node_id, dppu_node, "ppu", wave::NodeKind::FixedIndexedContainer);
            for (std::uint32_t ppu = 0; ppu < ppu_count; ++ppu) {
                const wave::NodeId ppu_node = add_index_node(
                    recorder, next_node_id, ppus, ppu, wave::NodeKind::Aggregate);
                const wave::NodeId qppus = add_named_node(
                    recorder, next_node_id, ppu_node, "qppu", wave::NodeKind::FixedIndexedContainer);
                for (std::uint32_t qppu = 0; qppu < qppu_count; ++qppu) {
                    const wave::NodeId qppu_node = add_index_node(
                        recorder, next_node_id, qppus, qppu, wave::NodeKind::Aggregate);
                    const wave::NodeId banks = add_named_node(
                        recorder, next_node_id, qppu_node, "bank", wave::NodeKind::FixedIndexedContainer);
                    for (std::uint32_t bank = 0; bank < bank_count; ++bank) {
                        const wave::NodeId bank_node = add_index_node(
                            recorder, next_node_id, banks, bank, wave::NodeKind::Aggregate);
                        for (std::uint32_t field = 0; field < field_count; ++field, ++group_ordinal) {
                            const FieldShape& shape = kFields[field];
                            const wave::NodeId field_node = add_named_node(
                                recorder, next_node_id, bank_node, shape.name,
                                wave::NodeKind::FixedIndexedContainer);

                            const std::uint64_t group_begin =
                                (std::uint64_t(signal_count) * group_ordinal) / group_count;
                            const std::uint64_t group_end =
                                (std::uint64_t(signal_count) * (group_ordinal + 1u)) / group_count;
                            for (std::uint64_t local = 0; local < group_end - group_begin; ++local) {
                                const wave::NodeId leaf = add_index_node(
                                    recorder, next_node_id, field_node,
                                    static_cast<std::uint32_t>(local), wave::NodeKind::Leaf);
                                recorder.on_track_declared_fast(
                                    next_track_id, next_track_id, leaf, shape.kind,
                                    shape.bit_width, 0u, false, std::string());
                                ++next_track_id;
                            }
                        }
                    }
                }
            }
        }
    }

    if (next_track_id != wave::TrackId(signal_count) + 1u) {
        std::cerr << "internal signal distribution mismatch: declared="
                  << static_cast<unsigned long long>(next_track_id - 1u)
                  << " expected=" << signal_count << "\n";
        std::exit(9);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string output_path = "build_vs\\viewer_20m_complex.wvz4";
    std::uint64_t signal_count_64 = 20000000ull;

    if (argc >= 2) output_path = argv[1] ? argv[1] : output_path;
    if (argc >= 3 && !parse_u64_arg(argv[2], signal_count_64)) {
        std::cerr << "invalid signal count\n";
        return 2;
    }
    if (argc > 3 || signal_count_64 == 0u || signal_count_64 > 50000000ull) {
        std::cerr << "usage: smoke_wvz4_viewer_20m_complex [out.wvz4] [signals<=50000000]\n";
        return 2;
    }

    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = output_path;
    cfg.emit_default_clk = false;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.enable_lod_tables = false;
    cfg.options.target_block_span = 8192;
    cfg.writer_process_connect_timeout_ms = 60000;

    std::string error;
    const auto started = std::chrono::steady_clock::now();
    if (!recorder.open(cfg, error)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }

    declare_complex_topology(recorder, static_cast<std::uint32_t>(signal_count_64));
    const auto declared = std::chrono::steady_clock::now();
    if (!recorder.open_writer_if_needed(error)) {
        std::cerr << "open_writer_if_needed failed: " << error << "\n";
        return 3;
    }
    const auto writer_opened = std::chrono::steady_clock::now();
    const std::size_t declared_nodes = recorder.declared_node_count();
    const std::size_t declared_tracks = recorder.declared_track_count();
    if (!recorder.close(error)) {
        std::cerr << "close failed: " << error << "\n";
        return 4;
    }
    const auto finished = std::chrono::steady_clock::now();

    std::cout << "path=" << output_path << "\n";
    std::cout << "signals=" << signal_count_64 << "\n";
    std::cout << "nodes=" << declared_nodes << "\n";
    std::cout << "tracks=" << declared_tracks << "\n";
    std::cout << "declare_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(declared - started).count()
              << "\n";
    std::cout << "open_writer_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(writer_opened - declared).count()
              << "\n";
    std::cout << "total_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count()
              << "\n";
    return 0;
}
