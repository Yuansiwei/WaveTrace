#include "wvz4_writer_typed.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wvz4::u64 kWriterTicksPerBusinessCycle = 10;

struct SignalMeta {
    wvz4::ValueType type = wvz4::ValueType::U64;
    wvz4::u32 bit_width = 64;
};

bool parse_u64_arg(const char* text, wvz4::u64& out) {
    if (!text || !text[0]) return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<wvz4::u64>(value);
    return true;
}

std::string indexed_name(const char* prefix, wvz4::u32 index) {
    std::ostringstream s;
    s << prefix << "_" << std::setw(4) << std::setfill('0') << index;
    return s.str();
}

SignalMeta signal_meta_for(wvz4::u32 signal_id) {
    SignalMeta meta;
    if (signal_id == 2 || signal_id % 10u == 0u) {
        meta.type = wvz4::ValueType::Bool;
        meta.bit_width = 1;
    } else if ((signal_id % 10u) <= 4u) {
        meta.type = wvz4::ValueType::U32;
        meta.bit_width = 32;
    } else {
        meta.type = wvz4::ValueType::U64;
        meta.bit_width = 64;
    }
    return meta;
}

std::size_t byte_width_for_meta(const SignalMeta& meta) {
    switch (meta.type) {
    case wvz4::ValueType::Bool:
    case wvz4::ValueType::U8:
        return 1;
    case wvz4::ValueType::U16:
        return 2;
    case wvz4::ValueType::U32:
        return 4;
    default:
        return 8;
    }
}

bool append_typed_update(wvz4::CycleSubmission& submission,
                         wvz4::u32 signal_id,
                         const SignalMeta& meta,
                         wvz4::u64 value) {
    wvz4::u8 bytes[wvz4::kMaxScalarBytes] = {};
    wvz4::u8 byte_count = 8;
    if (meta.type == wvz4::ValueType::Bool) {
        byte_count = 1;
        bytes[0] = (value & 1ull) != 0ull ? 1u : 0u;
    } else if (meta.type == wvz4::ValueType::U32) {
        byte_count = 4;
        const wvz4::u32 v = static_cast<wvz4::u32>(value);
        for (wvz4::u8 i = 0; i < byte_count; ++i) {
            bytes[i] = static_cast<wvz4::u8>((v >> (8u * i)) & 0xffu);
        }
    } else {
        byte_count = 8;
        for (wvz4::u8 i = 0; i < byte_count; ++i) {
            bytes[i] = static_cast<wvz4::u8>((value >> (8u * i)) & 0xffu);
        }
    }
    return submission.append_grouped_raw(byte_count, signal_id, bytes);
}

wvz4::u64 mix64(wvz4::u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

bool append_update_once(wvz4::CycleSubmission& submission,
                        std::vector<int>& seen_epoch,
                        int epoch,
                        wvz4::u32 signal_id,
                        const std::vector<SignalMeta>& metas,
                        wvz4::u64 value) {
    if (signal_id == 0 || signal_id >= metas.size()) return false;
    if (seen_epoch[signal_id] == epoch) return true;
    seen_epoch[signal_id] = epoch;
    return append_typed_update(submission, signal_id, metas[signal_id], value);
}

wvz4::Layout make_business_layout(wvz4::u32 signal_count, std::vector<SignalMeta>& metas) {
    static const char* kGroups[] = {
        "market", "orders", "matching", "risk",
        "position", "settlement", "telemetry", "flags"
    };
    const wvz4::u32 group_count = static_cast<wvz4::u32>(sizeof(kGroups) / sizeof(kGroups[0]));

    metas.assign(static_cast<std::size_t>(signal_count) + 1u, SignalMeta());

    wvz4::Layout layout;
    layout.names.reserve(static_cast<std::size_t>(signal_count) + group_count + 1u);
    layout.nodes.reserve(static_cast<std::size_t>(signal_count) + group_count + 1u);
    layout.signals.reserve(signal_count);

    wvz4::u32 next_name_id = 1;
    layout.names.push_back({next_name_id++, "business_top"});
    const wvz4::u32 root_name_id = 1;

    std::vector<wvz4::u32> group_name_ids(group_count + 1u, 0);
    for (wvz4::u32 i = 1; i <= group_count; ++i) {
        group_name_ids[i] = next_name_id;
        layout.names.push_back({next_name_id++, kGroups[i - 1u]});
    }

    const wvz4::u32 root_node_id = 1;
    const wvz4::u32 first_group_node_id = 2;
    const wvz4::u32 first_leaf_node_id = first_group_node_id + group_count;

    wvz4::NodeRecord root;
    root.node_id = root_node_id;
    root.parent_id = 0;
    root.name_id = root_name_id;
    root.kind = wvz4::NodeKind::Object;
    root.first_child = first_group_node_id;
    layout.nodes.push_back(root);

    for (wvz4::u32 i = 1; i <= group_count; ++i) {
        wvz4::NodeRecord group;
        group.node_id = first_group_node_id + i - 1u;
        group.parent_id = root_node_id;
        group.name_id = group_name_ids[i];
        group.kind = wvz4::NodeKind::Container;
        group.first_child = (i <= signal_count) ? first_leaf_node_id + i - 1u : 0;
        group.next_sibling = (i == group_count) ? 0 : group.node_id + 1u;
        layout.nodes.push_back(group);
    }

    for (wvz4::u32 signal_id = 1; signal_id <= signal_count; ++signal_id) {
        const wvz4::u32 group_index = ((signal_id - 1u) % group_count) + 1u;
        const wvz4::u32 next_group_signal = signal_id + group_count;

        const wvz4::u32 name_id = next_name_id++;
        layout.names.push_back({name_id, indexed_name("metric", signal_id)});

        wvz4::NodeRecord leaf;
        leaf.node_id = first_leaf_node_id + signal_id - 1u;
        leaf.parent_id = first_group_node_id + group_index - 1u;
        leaf.name_id = name_id;
        leaf.kind = wvz4::NodeKind::SignalLeaf;
        leaf.first_child = 0;
        leaf.next_sibling = (next_group_signal <= signal_count)
            ? first_leaf_node_id + next_group_signal - 1u
            : 0;
        layout.nodes.push_back(leaf);

        metas[signal_id] = signal_meta_for(signal_id);

        wvz4::SignalDefinition sig;
        sig.signal_id = signal_id;
        sig.storage_id = signal_id;
        sig.node_id = leaf.node_id;
        sig.type = metas[signal_id].type;
        sig.bit_width = metas[signal_id].bit_width;
        sig.radix = (sig.type == wvz4::ValueType::Bool) ? wvz4::Radix::Bin : wvz4::Radix::Hex;
        layout.signals.push_back(sig);
    }

    return layout;
}

} // namespace

int main(int argc, char** argv) {
    std::string output_path = "build_vs/wvz4_business_1m_10000signals.wvz4";
    wvz4::u64 business_cycles = 1000000;
    wvz4::u64 signal_count_64 = 10000;
    wvz4::u64 rotating_updates_per_cycle = 1;
    wvz4::u64 progress_every = 100000;
    bool enable_lod_tables = true;
    bool enable_residual_lod_tables = false;
    bool all_signals_every_cycle = false;
    bool entropy_values = false;
    std::string helper_exe_path;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--helper") {
            // Helper mode is the only supported high-level writer path.
        } else if (arg == "--no-helper") {
            std::cerr << "--no-helper is no longer supported; smoke writer always uses the anti-kill helper\n";
            return 2;
        } else if (arg == "--no-lod") {
            enable_lod_tables = false;
            enable_residual_lod_tables = false;
        } else if (arg == "--lod") {
            enable_lod_tables = true;
            enable_residual_lod_tables = false;
        } else if (arg == "--residual-lod" || arg == "--experimental-residual-lod") {
            enable_lod_tables = true;
            enable_residual_lod_tables = true;
        } else if (arg == "--all-signals-every-cycle") {
            all_signals_every_cycle = true;
        } else if (arg == "--entropy-values" || arg == "--dense-random-values") {
            entropy_values = true;
        } else if (arg == "--helper-exe") {
            if (i + 1 >= argc) {
                std::cerr << "--helper-exe requires a path\n";
                return 2;
            }
            helper_exe_path = argv[++i];
        } else if (arg == "--progress") {
            if (i + 1 >= argc || !parse_u64_arg(argv[++i], progress_every)) {
                std::cerr << "--progress requires a positive cycle interval\n";
                return 2;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: smoke_business_1m_writer [out.wvz4] [cycles] [signals] [updates_per_cycle]"
                << " [--helper-exe path] [--progress cycles] [--no-lod|--lod]"
                << " [--residual-lod]"
                << " [--all-signals-every-cycle] [--entropy-values]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() >= 1) output_path = positional[0];
    if (positional.size() >= 2 && !parse_u64_arg(positional[1].c_str(), business_cycles)) {
        std::cerr << "invalid cycle count: " << positional[1] << "\n";
        return 2;
    }
    if (positional.size() >= 3 && !parse_u64_arg(positional[2].c_str(), signal_count_64)) {
        std::cerr << "invalid signal count: " << positional[2] << "\n";
        return 2;
    }
    if (positional.size() >= 4 && !parse_u64_arg(positional[3].c_str(), rotating_updates_per_cycle)) {
        std::cerr << "invalid rotating updates per cycle: " << positional[3] << "\n";
        return 2;
    }
    if (positional.size() > 4) {
        std::cerr << "too many positional arguments\n";
        return 2;
    }
    const wvz4::u64 max_writer_cycle = static_cast<wvz4::u64>((std::numeric_limits<wvz4::i64>::max)());
    if (business_cycles == 0 || business_cycles > max_writer_cycle / kWriterTicksPerBusinessCycle) {
        std::cerr << "cycle count must be in 1.." << (max_writer_cycle / kWriterTicksPerBusinessCycle)
                  << " when scaled by writer ticks per business cycle\n";
        return 2;
    }
    const wvz4::u64 min_signal_count = all_signals_every_cycle ? 1u : 32u;
    if (signal_count_64 < min_signal_count || signal_count_64 > 1000000u) {
        std::cerr << "signal count must be in " << min_signal_count << "..1000000\n";
        return 2;
    }
    if (!all_signals_every_cycle &&
        (rotating_updates_per_cycle == 0 || rotating_updates_per_cycle > signal_count_64 - 6u)) {
        std::cerr << "rotating updates per cycle must be in 1..signal_count-6\n";
        return 2;
    }
    if (progress_every == 0) {
        std::cerr << "--progress must be greater than zero\n";
        return 2;
    }

    const wvz4::u32 signal_count = static_cast<wvz4::u32>(signal_count_64);
    const wvz4::u32 rotating_update_count = static_cast<wvz4::u32>(rotating_updates_per_cycle);
    std::vector<SignalMeta> metas;
    wvz4::Layout layout = make_business_layout(signal_count, metas);

    wvz4::WriterOptions options;
    options.target_block_span = 8192;
    options.signals_per_chunk = 32;
    options.compression = wvz4::Compression::Zstd;
    options.zstd_level = 3;
    options.enable_stats_log = true;
    options.block_pipeline_queue_limit = 16;
    options.enable_lod_tables = enable_lod_tables;
    options.enable_residual_lod_tables = enable_residual_lod_tables;

    wvz4::WriterProcessClient helper_writer;
    std::string error;

    const auto start = std::chrono::steady_clock::now();
    if (!helper_writer.open(output_path, layout, options, error, helper_exe_path)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }

    std::vector<int> seen_epoch(signal_count + 1u, 0);
    wvz4::u64 submitted_updates = 0;
    int epoch = 1;
    wvz4::CycleSubmission submission;
    std::array<std::size_t, wvz4::kMaxScalarBytes + 1u> signal_count_by_width = {};
    for (wvz4::u32 signal_id = 1; signal_id <= signal_count; ++signal_id) {
        ++signal_count_by_width[byte_width_for_meta(metas[signal_id])];
    }
    const std::size_t sparse_reserve = static_cast<std::size_t>(
        (std::min)(signal_count_64, rotating_updates_per_cycle + 24u));
    for (std::size_t width = 1; width < submission.width_groups.size(); ++width) {
        const std::size_t count = all_signals_every_cycle
            ? signal_count_by_width[width]
            : (std::min)(signal_count_by_width[width], sparse_reserve);
        submission.width_groups[width].reserve(count, static_cast<wvz4::u8>(width));
    }

    for (wvz4::u64 cycle = 0; cycle < business_cycles; ++cycle, ++epoch) {
        if (epoch == (std::numeric_limits<int>::max)()) {
            std::fill(seen_epoch.begin(), seen_epoch.end(), 0);
            epoch = 1;
        }

        submission.clear_updates();
        submission.cycle = static_cast<wvz4::i64>(cycle * kWriterTicksPerBusinessCycle);

        if (all_signals_every_cycle) {
            for (wvz4::u32 signal_id = 1; signal_id <= signal_count; ++signal_id) {
                wvz4::u64 value = entropy_values
                    ? mix64((cycle + 1ull) * 0x9E3779B97F4A7C15ull ^
                            (static_cast<wvz4::u64>(signal_id) * 0xD1B54A32D192ED03ull))
                    : ((cycle << 20) ^ static_cast<wvz4::u64>(signal_id));
                if (metas[signal_id].type == wvz4::ValueType::Bool) {
                    value = cycle + signal_id; // force a toggle every business cycle.
                }
                append_update_once(submission, seen_epoch, epoch, signal_id, metas, value);
            }
        } else {
            append_update_once(submission, seen_epoch, epoch, 1, metas, cycle);
            append_update_once(submission, seen_epoch, epoch, 2, metas, cycle);
            if ((cycle % 4ull) == 0ull) append_update_once(submission, seen_epoch, epoch, 3, metas, cycle / 4ull);
            if ((cycle % 8ull) == 0ull) append_update_once(submission, seen_epoch, epoch, 4, metas, mix64(cycle));
            if ((cycle % 17ull) == 0ull) append_update_once(submission, seen_epoch, epoch, 5, metas, cycle / 17ull);
            if ((cycle % 31ull) == 0ull) append_update_once(submission, seen_epoch, epoch, 6, metas, mix64(cycle ^ 0x31ull));

            const wvz4::u64 rotating_slots = signal_count_64 - 6u;
            const wvz4::u64 rotating_base = (cycle * rotating_updates_per_cycle) % rotating_slots;
            for (wvz4::u32 i = 0; i < rotating_update_count; ++i) {
                const wvz4::u32 rotating = 7u + static_cast<wvz4::u32>((rotating_base + i) % rotating_slots);
                append_update_once(submission, seen_epoch, epoch, rotating, metas,
                                   mix64((cycle << 12) ^ (static_cast<wvz4::u64>(i) << 32) ^ rotating));
            }

            if ((cycle % 257ull) == 0ull) {
                const wvz4::u32 base = 7u + static_cast<wvz4::u32>(
                    ((cycle / 257ull) * 23ull) % (signal_count - 22u));
                for (wvz4::u32 i = 0; i < 16u; ++i) {
                    append_update_once(submission, seen_epoch, epoch, base + i, metas,
                                       mix64(cycle ^ (static_cast<wvz4::u64>(base + i) << 24)));
                }
            }
        }

        submitted_updates += submission.total_update_count();
        if (!helper_writer.submit_cycle(submission, error)) {
            std::cerr << "submit failed at cycle " << cycle << ": " << error << "\n";
            return 3;
        }

        if (((cycle + 1u) % progress_every) == 0u || cycle + 1u == business_cycles) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            std::cerr << "progress cycles=" << (cycle + 1u)
                      << "/" << business_cycles
                      << " submitted_updates=" << submitted_updates
                      << " elapsed_ms=" << elapsed_ms << "\n";
        }
    }

    const wvz4::u64 end_marker_cycle = business_cycles * kWriterTicksPerBusinessCycle;
    wvz4::CycleSubmission end_marker;
    end_marker.cycle = static_cast<wvz4::i64>(end_marker_cycle);
    if (!helper_writer.submit_cycle(end_marker, error)) {
        std::cerr << "submit end marker failed at writer cycle " << end_marker_cycle << ": " << error << "\n";
        return 3;
    }

    if (!helper_writer.close(error)) {
        std::cerr << "close failed: " << error << "\n";
        return 4;
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "path=" << output_path << "\n";
    std::cout << "writer_mode=helper\n";
    std::cout << "business_cycles=" << business_cycles << "\n";
    std::cout << "writer_ticks_per_business_cycle=" << kWriterTicksPerBusinessCycle << "\n";
    std::cout << "signals=" << signal_count << "\n";
    std::cout << "rotating_updates_per_cycle=" << rotating_updates_per_cycle << "\n";
    std::cout << "all_signals_every_cycle=" << (all_signals_every_cycle ? 1 : 0) << "\n";
    std::cout << "entropy_values=" << (entropy_values ? 1 : 0) << "\n";
    std::cout << "lod_tables=" << (enable_lod_tables ? 1 : 0) << "\n";
    std::cout << "residual_lod_tables=" << (enable_residual_lod_tables ? 1 : 0) << "\n";
    std::cout << "submitted_updates=" << submitted_updates << "\n";
    std::cout << "elapsed_ms=" << elapsed_ms << "\n";
    return 0;
}
