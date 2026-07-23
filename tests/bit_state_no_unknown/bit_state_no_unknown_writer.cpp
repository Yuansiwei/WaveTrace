#include <wvz4_writer_typed.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

void add_bit_signal(wvz4::Layout& layout,
                    std::uint32_t signal_index,
                    const char* name) {
    const std::uint32_t node_id = signal_index + 2u;
    const std::uint32_t name_id = signal_index + 2u;
    wvz4::add_layout_name_blob_record(
        layout, name_id, name, static_cast<std::uint32_t>(std::char_traits<char>::length(name)));

    wvz4::NodeRecord node;
    node.node_id = node_id;
    node.parent_id = 1u;
    node.name_id = name_id;
    node.kind = wvz4::NodeKind::SignalLeaf;
    node.next_sibling = signal_index + 1u < 8u ? node_id + 1u : 0u;
    layout.nodes.push_back(node);

    wvz4::SignalDefinition signal;
    signal.signal_id = signal_index + 1u;
    signal.node_id = node_id;
    signal.type = wvz4::ValueType::Bool;
    signal.bit_width = 1u;
    signal.radix = wvz4::Radix::Bin;
    layout.signals.push_back(signal);
}

bool append_bit(wvz4::CycleSubmission& submission,
                std::uint32_t signal_id,
                bool value) {
    const std::uint8_t raw = value ? 1u : 0u;
    return submission.append_grouped_raw(1u, signal_id, &raw);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bit_state_no_unknown_writer <output.wvz4>\n";
        return 2;
    }

    constexpr std::uint32_t kCycles = 200000u;
    wvz4::Layout layout;
    wvz4::add_layout_name_blob_record(layout, 1u, "top", 3u);

    wvz4::NodeRecord root;
    root.node_id = 1u;
    root.name_id = 1u;
    root.kind = wvz4::NodeKind::Root;
    root.first_child = 2u;
    layout.nodes.push_back(root);

    add_bit_signal(layout, 0u, "zero_then_one_at_20k");
    add_bit_signal(layout, 1u, "one_then_zero_at_20k");
    add_bit_signal(layout, 2u, "always_zero");
    add_bit_signal(layout, 3u, "always_one");
    add_bit_signal(layout, 4u, "two_high_pulses");
    add_bit_signal(layout, 5u, "slow_toggle_every_10k");
    add_bit_signal(layout, 6u, "fast_toggle_first_30k_then_one");
    add_bit_signal(layout, 7u, "zero_then_one_at_cycle_1");

    wvz4::WriterOptions options;
    options.compression = wvz4::Compression::Zstd;
    options.zstd_level = 1;
    options.enable_block_pipeline = false;
    options.enable_lod_tables = true;
    options.lod_bucket_cycle_scale = 1u;
    options.target_block_span = 256u;

    std::string error;
    wvz4::Writer writer;
    if (!writer.open(argv[1], std::move(layout), options, error)) {
        std::cerr << error << '\n';
        return 3;
    }

    for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        wvz4::CycleSubmission submission;
        submission.cycle = static_cast<wvz4::i64>(cycle);

        if (cycle == 0u) {
            if (!append_bit(submission, 1u, false) ||
                !append_bit(submission, 2u, true) ||
                !append_bit(submission, 3u, false) ||
                !append_bit(submission, 4u, true) ||
                !append_bit(submission, 5u, false) ||
                !append_bit(submission, 6u, false) ||
                !append_bit(submission, 7u, false) ||
                !append_bit(submission, 8u, false)) return 4;
        }

        if (cycle == 20000u) {
            if (!append_bit(submission, 1u, true) ||
                !append_bit(submission, 2u, false)) return 4;
        }
        if (cycle == 30000u || cycle == 50000u ||
            cycle == 90000u || cycle == 110000u) {
            const bool high = cycle == 30000u || cycle == 90000u;
            if (!append_bit(submission, 5u, high)) return 4;
        }
        if (cycle != 0u && cycle % 10000u == 0u) {
            if (!append_bit(submission, 6u, ((cycle / 10000u) & 1u) != 0u)) return 4;
        }
        if (cycle > 0u && cycle < 30000u) {
            if (!append_bit(submission, 7u, (cycle & 1u) != 0u)) return 4;
        } else if (cycle == 30000u) {
            if (!append_bit(submission, 7u, true)) return 4;
        }
        if (cycle == 1u && !append_bit(submission, 8u, true)) return 4;

        if (!writer.submit_cycle(submission, error)) {
            std::cerr << error << '\n';
            return 5;
        }
    }

    if (!writer.close(error)) {
        std::cerr << error << '\n';
        return 6;
    }
    std::cout << "generated=" << argv[1] << " cycles=" << kCycles << " signals=8\n";
    return 0;
}
