#include "wave_tap.h"

#include <cstdint>
#include <iostream>
#include <string>

struct ParallelOverlapTop {
    wave::array<wave::array<std::uint32_t, 131072>, 4> rows;
};

namespace reflect {
template<> struct is_reflected<ParallelOverlapTop> : std::true_type {};
template<> struct reflected_visitor<ParallelOverlapTop> {
    template<class P, class V, class G>
    static void visit(const ParallelOverlapTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("rows", std::addressof(obj->rows));
    }
};
}

static ParallelOverlapTop g_top;

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main(int argc, char** argv) {
    bool use_parallel = true;
    int repeats = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--serial") use_parallel = false;
        if (arg == "--parallel") use_parallel = true;
        if (arg == "--repeat" && i + 1 < argc) repeats = std::atoi(argv[++i]);
    }

    for (std::size_t r = 0; r < g_top.rows.size(); ++r) {
        for (std::size_t c = 0; c < g_top.rows[r].size(); ++c) {
            g_top.rows[r][c] = static_cast<std::uint32_t>((r << 24) ^ c);
        }
    }

    for (int iter = 0; iter < repeats; ++iter) {
        std::string error;
        PathStableWvz4Recorder recorder;
        PathStableWvz4Recorder::OpenConfig cfg;
        cfg.file_path = use_parallel
            ? "build_vs\\dirty_array_parallel_overlap_repro.wvz4"
            : "build_vs\\dirty_array_serial_overlap_repro.wvz4";
        cfg.emit_default_clk = false;
        cfg.clk_period_ticks = 10;
        cfg.options.compression = wvz4::Compression::None;
        cfg.options.enable_stats_log = false;
        cfg.options.target_block_span = 256;

        if (!recorder.open(cfg, error)) {
            return fail("recorder.open failed: " + error);
        }

        wave::BuildOptions opt;
        opt.emit_track_decl_path = false;
        opt.enable_flat_leaf_fast_table = true;
        opt.enable_wave_array_dirty = true;
        opt.enable_wave_array_memory_block_precheck = true;
        opt.enable_wave_array_memory_block_byte_map = true;
        opt.enable_parallel_sampling = use_parallel;
        opt.enable_wave_array_parallel_sampling = use_parallel;
        opt.wave_array_parallel_threshold = 1u;
        opt.sampling_threads = 32u;

        wave::Tracer tracer(recorder, opt);
        tracer.add_root("top", &g_top);
        wave::WaveTap tap(tracer, recorder);

        if (!tap.sample_one_cycle()) {
            return fail(std::string(use_parallel ? "parallel" : "serial") +
                        "_cycle0_failed: " + tap.last_error());
        }

        if (!recorder.close(error)) {
            return fail("recorder.close failed: " + error);
        }
    }

    std::cout << (use_parallel ? "parallel" : "serial")
              << "_overlap_repro_ok repeats=" << repeats << "\n";
    return 0;
}
