#include <wvz4_writer_typed.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 6) {
        std::cerr << "usage: zoom_boundary_writer <output.wvz4> [cycles=200000] [signals=256] [lod=1] [writer_ticks_per_cycle=1]\n";
        return 2;
    }
    const std::uint32_t cycle_count = argc >= 3
        ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 200000u;
    const std::uint32_t signal_count = argc >= 4
        ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 256u;
    const bool enable_lod = argc < 5 || std::strtoul(argv[4], nullptr, 10) != 0;
    const std::uint32_t writer_ticks_per_cycle = argc >= 6
        ? static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 10)) : 1u;
    if (cycle_count < 1000u || signal_count < 16u || signal_count > 65536u ||
        writer_ticks_per_cycle == 0u) return 2;

    wvz4::Layout layout;
    wvz4::add_layout_name_blob_record(layout, 1u, "top", 3u);
    wvz4::add_layout_name_blob_record(layout, 2u, "signal", 6u);

    wvz4::NodeRecord root;
    root.node_id = 1u;
    root.name_id = 1u;
    root.kind = wvz4::NodeKind::Root;
    root.first_child = 2u;
    layout.nodes.push_back(root);

    wvz4::NodeRecord array;
    array.node_id = 2u;
    array.parent_id = 1u;
    array.name_id = 2u;
    array.kind = wvz4::NodeKind::Container;
    array.first_child = 3u;
    layout.nodes.push_back(array);

    layout.nodes.reserve(static_cast<std::size_t>(signal_count) + 2u);
    layout.signals.reserve(signal_count);
    for (std::uint32_t i = 0; i < signal_count; ++i) {
        wvz4::NodeRecord node;
        node.node_id = i + 3u;
        node.parent_id = 2u;
        node.array_index = i;
        node.kind = wvz4::NodeKind::SignalLeaf;
        node.next_sibling = i + 1u < signal_count ? i + 4u : 0u;
        layout.nodes.push_back(node);

        wvz4::SignalDefinition signal;
        signal.signal_id = i + 1u;
        signal.node_id = node.node_id;
        signal.type = wvz4::ValueType::U32;
        signal.bit_width = 32u;
        signal.radix = wvz4::Radix::Hex;
        layout.signals.push_back(signal);
    }

    wvz4::WriterOptions options;
    options.compression = wvz4::Compression::Zstd;
    options.zstd_level = 1;
    options.enable_block_pipeline = false;
    options.enable_lod_tables = enable_lod;
    options.lod_bucket_cycle_scale = writer_ticks_per_cycle;
    options.target_block_span = 256u;

    std::string error;
    wvz4::Writer writer;
    if (!writer.open(argv[1], std::move(layout), options, error)) {
        std::cerr << error << '\n';
        return 3;
    }
    for (std::uint32_t cycle = 0; cycle < cycle_count; ++cycle) {
        wvz4::CycleSubmission submission;
        submission.cycle = static_cast<wvz4::i64>(cycle) * writer_ticks_per_cycle;
        for (std::uint32_t lane = 0; lane < 16u; ++lane) {
            const std::uint32_t signal_id = ((cycle * 17u + lane * 37u) % signal_count) + 1u;
            const std::uint32_t value = (cycle * 2654435761u) ^ (signal_id * 2246822519u);
            if (!submission.append_grouped_raw(4u, signal_id, &value)) return 4;
        }
        if (!writer.submit_cycle(submission, error)) {
            std::cerr << error << '\n';
            return 4;
        }
    }
    if (!writer.close(error)) {
        std::cerr << error << '\n';
        return 5;
    }
    std::cout << "cycles=" << cycle_count << " signals=" << signal_count
              << " writer_ticks_per_cycle=" << writer_ticks_per_cycle << '\n';
    return 0;
}
