#include "wave_path_wvz4_recorder.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool parse_u64_arg(const char* text, std::uint64_t& out) {
    if (!text || !text[0]) return false;
    char* end = NULL;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

std::string signal_name(std::uint32_t index) {
    return std::string("sig_") + std::to_string(static_cast<unsigned long long>(index));
}

void declare_flat_topology(PathStableWvz4Recorder& recorder, std::uint32_t signal_count) {
    wave::NodeDecl root;
    root.node_id = 1;
    root.parent_id = 0;
    root.name = "top";
    root.kind = wave::NodeKind::Aggregate;
    recorder.on_node_declared(root);

    for (std::uint32_t i = 1; i <= signal_count; ++i) {
        const wave::NodeId node_id = static_cast<wave::NodeId>(i) + 1u;

        wave::NodeDecl node;
        node.node_id = node_id;
        node.parent_id = 1;
        node.name = signal_name(i);
        node.kind = wave::NodeKind::Leaf;
        recorder.on_node_declared(node);

        wave::TrackDecl track;
        track.track_id = i;
        track.storage_id = i;
        track.node_id = node_id;
        track.kind = (i % 10u == 0u) ? wave::ValueKind::Bool : wave::ValueKind::UnsignedInt;
        track.bit_width = (track.kind == wave::ValueKind::Bool) ? 1u : 32u;
        recorder.on_track_declared(track);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string output_path = "build_vs\\wvz4_recorder_1m_layout.wvz4";
    std::uint64_t signal_count_64 = 1000000ull;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: smoke_wvz4_recorder_1m_layout [out.wvz4] [signals]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() >= 1) output_path = positional[0];
    if (positional.size() >= 2 && !parse_u64_arg(positional[1].c_str(), signal_count_64)) {
        std::cerr << "invalid signal count: " << positional[1] << "\n";
        return 2;
    }
    if (positional.size() > 2) {
        std::cerr << "too many positional arguments\n";
        return 2;
    }
    if (signal_count_64 == 0 || signal_count_64 > 50000000ull) {
        std::cerr << "signal count must be in 1..50000000\n";
        return 2;
    }
    const std::uint32_t signal_count = static_cast<std::uint32_t>(signal_count_64);

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
    const auto start = std::chrono::steady_clock::now();
    if (!recorder.open(cfg, error)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }

    const auto declare_begin = std::chrono::steady_clock::now();
    declare_flat_topology(recorder, signal_count);
    const auto declare_end = std::chrono::steady_clock::now();

    if (!recorder.open_writer_if_needed(error)) {
        std::cerr << "open_writer_if_needed failed: " << error << "\n";
        return 3;
    }
    const auto writer_open_end = std::chrono::steady_clock::now();
    const std::size_t declared_nodes = recorder.declared_node_count();
    const std::size_t declared_tracks = recorder.declared_track_count();

    if (!recorder.close(error)) {
        std::cerr << "close failed: " << error << "\n";
        return 4;
    }
    const auto end = std::chrono::steady_clock::now();

    const auto declare_ms = std::chrono::duration_cast<std::chrono::milliseconds>(declare_end - declare_begin).count();
    const auto open_writer_ms = std::chrono::duration_cast<std::chrono::milliseconds>(writer_open_end - declare_end).count();
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "path=" << output_path << "\n";
    std::cout << "signals=" << signal_count << "\n";
    std::cout << "nodes=" << declared_nodes << "\n";
    std::cout << "tracks=" << declared_tracks << "\n";
    std::cout << "helper=auto\n";
    std::cout << "declare_ms=" << declare_ms << "\n";
    std::cout << "open_writer_ms=" << open_writer_ms << "\n";
    std::cout << "total_ms=" << total_ms << "\n";
    return 0;
}
