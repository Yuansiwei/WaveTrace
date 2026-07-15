#include <wvz4_writer_typed.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef BENCH_SHARED_NAMES
#define BENCH_SHARED_NAMES 0
#endif

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::uint64_t file_size(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    return input ? static_cast<std::uint64_t>(input.tellg()) : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: layout_writer_bench <output.wvz4> <leaf-count> [name-cardinality; 0=unique]\n";
        return 2;
    }

    const char* output_path = argv[1];
    const std::uint32_t leaf_count = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    if (leaf_count == 0 || leaf_count == UINT32_MAX) {
        std::cerr << "leaf-count must be in [1, UINT32_MAX-1]\n";
        return 2;
    }

    const std::uint32_t name_cardinality = argc == 4
        ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10))
        : 64u;
    const bool unique_names = name_cardinality == 0u;
    const std::uint32_t dictionary_name_count = unique_names ? leaf_count : name_cardinality;
    std::vector<std::string> repeated_names;
    repeated_names.reserve(unique_names ? 0u : name_cardinality);
    for (std::uint32_t i = 0; i < name_cardinality; ++i) {
        repeated_names.push_back("register_bank_lane_" + std::to_string(i));
    }

    const Clock::time_point total_begin = Clock::now();
    const Clock::time_point build_begin = Clock::now();
    wvz4::Layout layout;
    layout.nodes.reserve(static_cast<std::size_t>(leaf_count) + 1u);
    layout.signals.reserve(leaf_count);
#if BENCH_SHARED_NAMES
    layout.names.reserve(static_cast<std::size_t>(dictionary_name_count) + 1u);
    layout.name_blob.reserve(unique_names ? static_cast<std::size_t>(leaf_count) * 28u + 3u : 1600u);
    std::unordered_map<std::string, std::uint32_t> interned_names;
    interned_names.reserve(static_cast<std::size_t>(dictionary_name_count) + 1u);
    wvz4::add_layout_name_blob_record(layout, 1u, "top", 3u);
    interned_names.emplace("top", 1u);
    for (std::uint32_t i = 0; i < name_cardinality; ++i) {
        const std::uint32_t name_id = i + 2u;
        const std::string& name = repeated_names[i];
        wvz4::add_layout_name_blob_record(layout, name_id, name.data(), name.size());
        interned_names.emplace(name, name_id);
    }
#else
    layout.names.reserve(static_cast<std::size_t>(leaf_count) + 1u);
    layout.name_blob.reserve(static_cast<std::size_t>(leaf_count) * 22u + 3u);
    wvz4::add_layout_name_blob_record(layout, 1u, "top", 3u);
#endif

    wvz4::NodeRecord root;
    root.node_id = 1u;
    root.parent_id = 0u;
    root.name_id = 1u;
    root.kind = wvz4::NodeKind::Object;
    root.first_child = 2u;
    root.next_sibling = 0u;
    layout.nodes.push_back(root);

    for (std::uint32_t i = 0; i < leaf_count; ++i) {
        const std::uint32_t node_id = i + 2u;
        const std::string unique_name = unique_names
            ? "register_bank_lane_" + std::to_string(i)
            : std::string();
        const std::string& name = unique_names
            ? unique_name
            : repeated_names[i % name_cardinality];
#if BENCH_SHARED_NAMES
        auto found = interned_names.find(name);
        if (found == interned_names.end()) {
            const std::uint32_t new_name_id = static_cast<std::uint32_t>(layout.names.size()) + 1u;
            wvz4::add_layout_name_blob_record(layout, new_name_id, name.data(), name.size());
            found = interned_names.emplace(name, new_name_id).first;
        }
        const std::uint32_t name_id = found->second;
#else
        const std::uint32_t name_id = node_id;
        wvz4::add_layout_name_blob_record(layout, name_id, name.data(), name.size());
#endif

        wvz4::NodeRecord node;
        node.node_id = node_id;
        node.parent_id = 1u;
        node.name_id = name_id;
        node.kind = wvz4::NodeKind::SignalLeaf;
        node.first_child = 0u;
        node.next_sibling = (i + 1u < leaf_count) ? node_id + 1u : 0u;
        layout.nodes.push_back(node);

        wvz4::SignalDefinition signal;
        signal.signal_id = i + 1u;
        signal.storage_id = 0u;
        signal.node_id = node_id;
        signal.type = wvz4::ValueType::U32;
        signal.bit_width = 32u;
        signal.bit_offset = 0u;
        signal.radix = wvz4::Radix::Auto;
        signal.storage_only = false;
        layout.signals.push_back(signal);
    }
    const Clock::time_point build_end = Clock::now();

    wvz4::WriterOptions options;
    options.compression = wvz4::Compression::Zstd;
    options.zstd_level = 3;
    options.enable_block_pipeline = false;
    options.enable_lod_tables = false;
    options.enable_stats_log = true;
    options.stats_log_path = std::string(output_path) + ".log";

    std::string error;
    wvz4::Writer writer;
    const Clock::time_point open_begin = Clock::now();
    if (!writer.open(output_path, std::move(layout), options, error)) {
        std::cerr << "writer.open failed: " << error << "\n";
        return 4;
    }
    const Clock::time_point open_end = Clock::now();
    const Clock::time_point close_begin = Clock::now();
    if (!writer.close(error)) {
        std::cerr << "writer.close failed: " << error << "\n";
        return 5;
    }
    const Clock::time_point close_end = Clock::now();

    std::cout << "format=" << wvz4::kFormatVersion
              << " shared_names=" << BENCH_SHARED_NAMES
              << " leaves=" << leaf_count
              << " name_cardinality=" << (unique_names ? leaf_count : name_cardinality)
              << " build_ms=" << elapsed_ms(build_begin, build_end)
              << " open_ms=" << elapsed_ms(open_begin, open_end)
              << " close_ms=" << elapsed_ms(close_begin, close_end)
              << " total_ms=" << elapsed_ms(total_begin, close_end)
              << " file_bytes=" << file_size(output_path)
              << "\n";
    return 0;
}
