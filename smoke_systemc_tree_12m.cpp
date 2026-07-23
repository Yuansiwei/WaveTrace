#include <systemc>

#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

static std::uint64_t elapsed_us(std::chrono::steady_clock::time_point begin,
                                std::chrono::steady_clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

static double us_to_ms(std::uint64_t us) {
    return static_cast<double>(us) / 1000.0;
}

static bool parse_u64(const char* text, std::uint64_t& out) {
    if (!text || !text[0]) return false;
    char* end = NULL;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

static std::uint64_t pow_u64(std::uint32_t base, std::uint32_t exp) {
    std::uint64_t value = 1;
    for (std::uint32_t i = 0; i < exp; ++i) value *= static_cast<std::uint64_t>(base);
    return value;
}

static std::uint64_t split_count(std::uint64_t total, std::uint32_t parts, std::uint32_t index) {
    const std::uint64_t base = total / parts;
    const std::uint64_t rem = total % parts;
    return base + (index < rem ? 1u : 0u);
}

static void format_prefixed_index(char prefix, std::uint64_t index, char* out, std::size_t out_size) {
    if (!out || out_size == 0) return;
    if (out_size == 1) { out[0] = '\0'; return; }
    out[0] = prefix;
    char digits[32];
    std::size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (index % 10u));
        index /= 10u;
    } while (index != 0 && digit_count < sizeof(digits));
    std::size_t pos = 1;
    while (digit_count != 0 && pos + 1u < out_size) {
        out[pos++] = digits[--digit_count];
    }
    out[pos] = '\0';
}

struct SystemCTreeNode : sc_core::sc_module {
    std::vector<std::unique_ptr<SystemCTreeNode> > children;
    std::vector<std::uint32_t> leaves;

    SystemCTreeNode(sc_core::sc_module_name name,
                    std::uint32_t depth,
                    std::uint32_t max_depth,
                    std::uint32_t fanout,
                    std::uint64_t leaf_count,
                    std::uint64_t* module_count,
                    std::uint64_t* leaf_bank_count)
        : sc_core::sc_module(name) {
        if (module_count) ++(*module_count);
        if (depth < max_depth) {
            children.reserve(fanout);
            for (std::uint32_t i = 0; i < fanout; ++i) {
                char child_name[32];
                format_prefixed_index('n', i, child_name, sizeof(child_name));
                children.push_back(std::unique_ptr<SystemCTreeNode>(
                    new SystemCTreeNode(child_name,
                                        depth + 1u,
                                        max_depth,
                                        fanout,
                                        split_count(leaf_count, fanout, i),
                                        module_count,
                                        leaf_bank_count)));
            }
        } else {
            if (leaf_bank_count) ++(*leaf_bank_count);
            leaves.resize(static_cast<std::size_t>(leaf_count));
        }
    }
};

namespace reflect {
template<> struct is_reflected<SystemCTreeNode> : std::true_type {};
template<> struct reflected_visitor<SystemCTreeNode> {
    template<class P, class V, class G>
    static void visit(const SystemCTreeNode* obj, P&& on_ptr, V&&, G&&) {
        for (std::size_t i = 0; i < obj->children.size(); ++i) {
            char name[32];
            format_prefixed_index('n', i, name, sizeof(name));
            on_ptr(name, obj->children[i].get());
        }
        for (std::size_t i = 0; i < obj->leaves.size(); ++i) {
            char name[32];
            format_prefixed_index('l', static_cast<std::uint64_t>(i), name, sizeof(name));
            on_ptr(name, std::addressof(obj->leaves[i]));
        }
    }
};
}

int sc_main(int argc, char* argv[]) {
    std::string out_path = "build_vs\\systemc_tree_12m.wvz4";
    std::uint64_t requested_leaves = 12000000ull;
    std::uint32_t fanout = 32;
    std::uint32_t levels = 3;
    bool sample_one = false;
    bool enable_zstd = false;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--sample-one") sample_one = true;
        else if (arg == "--open-only") sample_one = false;
        else if (arg == "--zstd") enable_zstd = true;
        else if (arg == "--no-zstd") enable_zstd = false;
        else if (arg == "--leaves" && i + 1 < argc) {
            if (!parse_u64(argv[++i], requested_leaves)) {
                std::cerr << "invalid --leaves value\n";
                return 2;
            }
        } else if (arg == "--fanout" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parse_u64(argv[++i], value) || value < 2u || value > 128u) {
                std::cerr << "invalid --fanout value\n";
                return 2;
            }
            fanout = static_cast<std::uint32_t>(value);
        } else if (arg == "--levels" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parse_u64(argv[++i], value) || value > 8u) {
                std::cerr << "invalid --levels value\n";
                return 2;
            }
            levels = static_cast<std::uint32_t>(value);
        } else if ((arg == "--out" || arg == "-o") && i + 1 < argc) {
            out_path = argv[++i] ? argv[i] : "";
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: smoke_systemc_tree_12m [out.wvz4] [leaves]"
                      << " [--leaves N] [--fanout N] [--levels N]"
                      << " [--open-only|--sample-one] [--zstd|--no-zstd]\n";
            return 0;
        } else if (positional == 0) {
            out_path = arg;
            ++positional;
        } else if (positional == 1) {
            if (!parse_u64(arg.c_str(), requested_leaves)) {
                std::cerr << "invalid leaf count: " << arg << "\n";
                return 2;
            }
            ++positional;
        } else {
            std::cerr << "too many arguments\n";
            return 2;
        }
    }

    const std::uint64_t leaf_banks = pow_u64(fanout, levels);
    if (leaf_banks == 0 || requested_leaves > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "invalid tree size\n";
        return 2;
    }

    const auto construct_begin = std::chrono::steady_clock::now();
    std::uint64_t module_count = 0;
    std::uint64_t leaf_bank_count = 0;
    SystemCTreeNode top("top", 0, levels, fanout, requested_leaves, &module_count, &leaf_bank_count);
    const auto construct_end = std::chrono::steady_clock::now();

    std::string error;
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.clk_fall_offset_ticks = 5;
    cfg.options.compression = enable_zstd ? wvz4::Compression::Zstd : wvz4::Compression::None;
    cfg.options.zstd_level = 1;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 8192;
    cfg.options.signals_per_chunk = 64;
    cfg.options.enable_lod_tables = false;
    cfg.options.enable_block_pipeline = false;

    if (!recorder.open(cfg, error)) {
        std::cerr << "recorder.open failed: " << error << "\n";
        return 1;
    }

    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_flat_memory_block_precheck = true;
    opt.enable_dirty_peek_groups = false;
    opt.enable_wave_value_dirty = false;
    opt.enable_wave_value_address_hash = false;
    opt.enable_wave_array_dirty = false;
    opt.enable_parallel_sampling = false;
    opt.enable_union_fields = false;
    opt.enable_bitfield_fields = false;
    opt.dump_leaf_distribution_after_topology = true;
    opt.debug_log = false;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &top);

    const auto topology_begin = std::chrono::steady_clock::now();
    if (sample_one) {
        sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
        wave::WaveTap tap("wave_tap", tracer, recorder, clk);
        sc_core::sc_start(sc_core::SC_ZERO_TIME);
        if (!tap.last_error().empty()) error = tap.last_error();
        if (error.empty() && tap.next_cycle() != static_cast<wave::Cycle>(1)) {
            error = "WaveTap start_of_simulation sample count mismatch";
        }
    } else {
        tracer.prepare_topology(0);
        if (!recorder.open_writer_if_needed(error)) {
            std::cerr << "recorder.open_writer_if_needed failed: " << error << "\n";
            return 5;
        }
    }
    const auto topology_end = std::chrono::steady_clock::now();

    if (!error.empty()) {
        std::cerr << "sample failed: " << error << "\n";
        return 3;
    }
    if (!recorder.close(error)) {
        std::cerr << "recorder.close failed: " << error << "\n";
        return 4;
    }
    const auto close_end = std::chrono::steady_clock::now();

    std::cout << std::fixed << std::setprecision(3)
              << "systemc_tree_12m_ok"
              << " mode=" << (sample_one ? "sample_one" : "open_only")
              << " leaves=" << static_cast<unsigned long long>(requested_leaves)
              << " fanout=" << fanout
              << " levels=" << levels
              << " leaf_banks=" << static_cast<unsigned long long>(leaf_banks)
              << " modules=" << static_cast<unsigned long long>(module_count)
              << " built_leaf_banks=" << static_cast<unsigned long long>(leaf_bank_count)
              << " file=" << out_path
              << " zstd=" << (enable_zstd ? 1 : 0)
              << " construct_ms=" << us_to_ms(elapsed_us(construct_begin, construct_end))
              << " topology_open_ms=" << us_to_ms(elapsed_us(topology_begin, topology_end))
              << " close_ms=" << us_to_ms(elapsed_us(topology_end, close_end))
              << " sim_time=" << sc_core::sc_time_stamp()
              << "\n";
    return 0;
}
