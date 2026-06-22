#include "wvz4_writer_typed.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

std::string default_output_path() {
    const char* temp = std::getenv("TEMP");
    if (!temp || !*temp) temp = std::getenv("TMP");
    if (!temp || !*temp) return "bitfield_track.wvz4";
    std::string out(temp);
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out.push_back('\\');
    out += "bitfield_track.wvz4";
    return out;
}

void add_name(wvz4::Layout& layout, wvz4::u32 id, const char* text) {
    layout.names.push_back({id, text});
}

void add_node(wvz4::Layout& layout,
              wvz4::u32 node_id,
              wvz4::u32 parent_id,
              wvz4::u32 name_id,
              wvz4::NodeKind kind,
              wvz4::u32 first_child,
              wvz4::u32 next_sibling) {
    wvz4::NodeRecord n;
    n.node_id = node_id;
    n.parent_id = parent_id;
    n.name_id = name_id;
    n.kind = kind;
    n.first_child = first_child;
    n.next_sibling = next_sibling;
    layout.nodes.push_back(n);
}

void add_alias(wvz4::Layout& layout,
               wvz4::u32 signal_id,
               wvz4::u32 node_id,
               wvz4::u32 bit_offset,
               wvz4::u32 bit_width,
               wvz4::ValueType type,
               wvz4::Radix radix) {
    wvz4::SignalDefinition s;
    s.signal_id = signal_id;
    s.storage_id = 1;
    s.node_id = node_id;
    s.type = type;
    s.bit_width = bit_width;
    s.bit_offset = bit_offset;
    s.radix = radix;
    layout.signals.push_back(s);
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = (argc >= 2 && argv[1] && *argv[1]) ? argv[1] : default_output_path();

    wvz4::Layout layout;
    add_name(layout, 1, "top");
    add_name(layout, 2, "low4");
    add_name(layout, 3, "high4");
    add_name(layout, 4, "upper8");
    add_name(layout, 5, "mid8");
    add_name(layout, 6, "topbit");

    add_node(layout, 1, 0, 1, wvz4::NodeKind::Object, 2, 0);
    add_node(layout, 2, 1, 2, wvz4::NodeKind::SignalLeaf, 0, 3);
    add_node(layout, 3, 1, 3, wvz4::NodeKind::SignalLeaf, 0, 4);
    add_node(layout, 4, 1, 4, wvz4::NodeKind::SignalLeaf, 0, 5);
    add_node(layout, 5, 1, 5, wvz4::NodeKind::SignalLeaf, 0, 6);
    add_node(layout, 6, 1, 6, wvz4::NodeKind::SignalLeaf, 0, 0);

    wvz4::SignalDefinition storage;
    storage.signal_id = 1;
    storage.storage_id = 1;
    storage.node_id = 0;
    storage.type = wvz4::ValueType::U16;
    storage.bit_width = 16;
    storage.bit_offset = 0;
    storage.radix = wvz4::Radix::Hex;
    storage.storage_only = true;
    layout.signals.push_back(storage);

    add_alias(layout, 2, 2, 0, 4, wvz4::ValueType::U8, wvz4::Radix::Hex);
    add_alias(layout, 3, 3, 4, 4, wvz4::ValueType::U8, wvz4::Radix::Hex);
    add_alias(layout, 4, 4, 8, 8, wvz4::ValueType::U8, wvz4::Radix::Hex);
    add_alias(layout, 5, 5, 4, 8, wvz4::ValueType::U8, wvz4::Radix::Hex);
    add_alias(layout, 6, 6, 15, 1, wvz4::ValueType::Bool, wvz4::Radix::Bin);

    wvz4::WriterOptions opt;
    opt.compression = wvz4::Compression::None;
    opt.enable_lod_tables = true;
    opt.enable_stats_log = false;
    opt.target_block_span = 8;

    wvz4::Writer writer;
    std::string error;
    if (!writer.open(path, layout, opt, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::uint16_t last = 0;
    for (int i = 1; i < 5000; ++i) {
        std::uint16_t value = static_cast<std::uint16_t>((static_cast<unsigned>(i) * 251u + static_cast<unsigned>(i >> 1) * 17u) & 0xFFFFu);
        if (value == last) value = static_cast<std::uint16_t>(value ^ 1u);
        if (value == static_cast<std::uint16_t>(0xE1AFu)) value = static_cast<std::uint16_t>(value ^ 0x0100u);
        wvz4::CycleSubmission cycle;
        cycle.cycle = i;
        cycle.updates.push_back(wvz4::CycleValueUpdate::make<std::uint16_t>(1, value));
        if (!writer.submit_cycle(cycle, error)) {
            std::cerr << error << "\n";
            return 2;
        }
        last = value;
    }

    wvz4::CycleSubmission final_cycle;
    final_cycle.cycle = 5000;
    final_cycle.updates.push_back(wvz4::CycleValueUpdate::make<std::uint16_t>(1, static_cast<std::uint16_t>(0xE1AFu)));
    if (!writer.submit_cycle(final_cycle, error)) {
        std::cerr << error << "\n";
        return 2;
    }

    if (!writer.close(error)) {
        std::cerr << error << "\n";
        return 3;
    }

    std::cout << path << "\n";
    return 0;
}
