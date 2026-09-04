#include <wvz4_writer_typed.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::uint64_t file_size(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    return input ? static_cast<std::uint64_t>(input.tellg()) : 0u;
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0u && b > (std::numeric_limits<std::uint64_t>::max)() / a) return false;
    result = a * b;
    return true;
}

bool append_u32(wvz4::CycleSubmission& cycle, std::uint32_t signal_id, std::uint32_t value) {
    return cycle.append_grouped_raw(4u, signal_id, &value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: deep_layout_writer_bench <output.wvz4> [fanout=10] [depth=7] [zstd|none]\n";
        return 2;
    }

    const char* output_path = argv[1];
    const std::uint32_t fanout = argc >= 3
        ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10))
        : 10u;
    const std::uint32_t depth = argc >= 4
        ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10))
        : 7u;
    const std::string compression_name = argc >= 5 ? argv[4] : "zstd";
    if (fanout < 2u || depth == 0u || depth > 30u) {
        std::cerr << "fanout must be >= 2 and depth must be in [1, 30]\n";
        return 2;
    }
    if (compression_name != "zstd" && compression_name != "none") {
        std::cerr << "compression must be zstd or none\n";
        return 2;
    }

    std::uint64_t leaves64 = 1u;
    std::uint64_t array_nodes64 = 0u;
    std::uint64_t named_nodes64 = 1u; // top
    std::uint64_t parents64 = 1u;
    for (std::uint32_t level = 0; level < depth; ++level) {
        named_nodes64 += parents64;
        std::uint64_t next = 0u;
        if (!checked_mul(parents64, fanout, next)) {
            std::cerr << "fanout^depth overflow\n";
            return 2;
        }
        array_nodes64 += next;
        parents64 = next;
    }
    leaves64 = parents64;
    const std::uint64_t nodes64 = named_nodes64 + array_nodes64;
    if (leaves64 >= UINT32_MAX || nodes64 >= UINT32_MAX) {
        std::cerr << "case exceeds WVZ4 uint32 id range\n";
        return 2;
    }
    const std::uint32_t leaf_count = static_cast<std::uint32_t>(leaves64);
    const std::uint32_t node_count = static_cast<std::uint32_t>(nodes64);

    static const char* const kLevelNames[] = {
        "chiplet", "subsystem", "pipeline", "stage", "bank", "row", "lane",
        "cluster", "engine", "slice", "unit", "group", "thread", "slot"
    };
    if (depth > sizeof(kLevelNames) / sizeof(kLevelNames[0])) {
        std::cerr << "depth exceeds available semantic level names\n";
        return 2;
    }

    const Clock::time_point total_begin = Clock::now();
    const Clock::time_point build_begin = Clock::now();
    wvz4::Layout layout;
    layout.names.reserve(static_cast<std::size_t>(depth) + 1u);
    layout.name_blob.reserve(128u);
    layout.nodes.reserve(node_count);
    layout.signals.reserve(leaf_count);

    wvz4::add_layout_name_blob_record(layout, 1u, "top", 3u);
    for (std::uint32_t level = 0; level < depth; ++level) {
        const std::string name(kLevelNames[level]);
        wvz4::add_layout_name_blob_record(layout, level + 2u, name.data(), name.size());
    }

    wvz4::NodeRecord root;
    root.node_id = 1u;
    root.parent_id = 0u;
    root.name_id = 1u;
    root.kind = wvz4::NodeKind::Root;
    root.first_child = 2u;
    layout.nodes.push_back(root);

    std::vector<std::uint32_t> parents(1u, 1u);
    std::vector<std::uint32_t> next_parents;
    std::uint32_t next_node_id = 2u;
    std::uint32_t next_signal_id = 1u;

    for (std::uint32_t level = 0; level < depth; ++level) {
        const bool leaf_level = level + 1u == depth;
        if (!leaf_level) {
            next_parents.clear();
            next_parents.reserve(parents.size() * static_cast<std::size_t>(fanout));
        }

        for (std::size_t parent_index = 0; parent_index < parents.size(); ++parent_index) {
            const std::uint32_t parent_id = parents[parent_index];
            const std::uint32_t label_id = next_node_id++;
            layout.nodes[static_cast<std::size_t>(parent_id - 1u)].first_child = label_id;

            wvz4::NodeRecord label;
            label.node_id = label_id;
            label.parent_id = parent_id;
            label.name_id = level + 2u;
            label.kind = wvz4::NodeKind::Container;
            label.first_child = next_node_id;
            layout.nodes.push_back(label);

            for (std::uint32_t index = 0; index < fanout; ++index) {
                const std::uint32_t node_id = next_node_id++;
                wvz4::NodeRecord node;
                node.node_id = node_id;
                node.parent_id = label_id;
                node.name_id = 0u;
                node.array_index = index;
                node.kind = leaf_level ? wvz4::NodeKind::SignalLeaf : wvz4::NodeKind::ArrayElem;
                node.next_sibling = index + 1u < fanout ? node_id + 1u : 0u;
                layout.nodes.push_back(node);

                if (leaf_level) {
                    wvz4::SignalDefinition signal;
                    signal.signal_id = next_signal_id++;
                    signal.node_id = node_id;
                    signal.type = wvz4::ValueType::U32;
                    signal.bit_width = 32u;
                    signal.radix = wvz4::Radix::Hex;
                    layout.signals.push_back(signal);
                } else {
                    next_parents.push_back(node_id);
                }
            }
        }
        if (!leaf_level) parents.swap(next_parents);
    }
    if (layout.nodes.size() != node_count || layout.signals.size() != leaf_count ||
        next_node_id != node_count + 1u || next_signal_id != leaf_count + 1u) {
        std::cerr << "internal topology count mismatch\n";
        return 3;
    }
    const Clock::time_point build_end = Clock::now();

    wvz4::WriterOptions options;
    options.compression = compression_name == "zstd"
        ? wvz4::Compression::Zstd
        : wvz4::Compression::None;
    options.zstd_level = 3;
    options.enable_block_pipeline = false;
    options.enable_lod_tables = false;
    options.enable_stats_log = true;
    options.stats_log_path = std::string(output_path) + ".writer.log";
    options.target_block_span = 100u;

    std::string error;
    wvz4::Writer writer;
    const Clock::time_point open_begin = Clock::now();
    if (!writer.open(output_path, std::move(layout), options, error)) {
        std::cerr << "writer.open failed: " << error << "\n";
        return 4;
    }
    const Clock::time_point open_end = Clock::now();

    const std::uint32_t probes[] = {
        1u,
        fanout,
        fanout + 1u,
        leaf_count / 2u + 1u,
        leaf_count - 1u,
        leaf_count
    };
    wvz4::CycleSubmission cycle10;
    cycle10.cycle = 10;
    for (std::uint32_t signal_id : probes) {
        const std::uint32_t value = 0xA5000000u ^ signal_id;
        if (!append_u32(cycle10, signal_id, value)) return 5;
    }
    if (!writer.submit_cycle(cycle10, error)) {
        std::cerr << "submit cycle 10 failed: " << error << "\n";
        return 5;
    }

    wvz4::CycleSubmission cycle20;
    cycle20.cycle = 20;
    const std::uint32_t first20 = 0x11111111u;
    const std::uint32_t last20 = 0xEEEEEEEEu;
    append_u32(cycle20, 1u, first20);
    append_u32(cycle20, leaf_count, last20);
    if (!writer.submit_cycle(cycle20, error)) {
        std::cerr << "submit cycle 20 failed: " << error << "\n";
        return 5;
    }

    const Clock::time_point close_begin = Clock::now();
    if (!writer.close(error)) {
        std::cerr << "writer.close failed: " << error << "\n";
        return 6;
    }
    const Clock::time_point close_end = Clock::now();

    std::cout << "format=" << wvz4::kFormatVersion
              << " fanout=" << fanout
              << " depth=" << depth
              << " path_segments=" << (1u + depth * 2u)
              << " compression=" << compression_name
              << " names=" << (depth + 1u)
              << " nodes=" << node_count
              << " leaves=" << leaf_count
              << " build_ms=" << elapsed_ms(build_begin, build_end)
              << " open_ms=" << elapsed_ms(open_begin, open_end)
              << " close_ms=" << elapsed_ms(close_begin, close_end)
              << " total_ms=" << elapsed_ms(total_begin, close_end)
              << " file_bytes=" << file_size(output_path)
              << "\n";
    return 0;
}
