#include <systemc>

#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

static const std::size_t kCreditPerfSignalCount = 1000;

static std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point begin,
                                std::chrono::steady_clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

static double ns_to_ms(std::uint64_t ns) {
    return static_cast<double>(ns) / 1000000.0;
}

struct PerfBreakdown {
    bool enabled = false;
    std::uint64_t preopen_ns = 0;
    std::uint64_t dut_update_ns = 0;
    std::uint64_t sample_total_ns = 0;
    std::uint64_t recorder_begin_ns = 0;
    std::uint64_t tracer_sample_ns = 0;
    std::uint64_t recorder_end_ns = 0;
};

struct CreditPerfDut : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    wave::array<std::uint64_t, kCreditPerfSignalCount> signals;
    std::uint64_t cycle = 0;
    PerfBreakdown* perf = NULL;

    SC_HAS_PROCESS(CreditPerfDut);

    explicit CreditPerfDut(sc_core::sc_module_name name)
        : sc_core::sc_module(name), clk("clk") {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        const std::uint64_t c = cycle++;
        const auto t0 = perf && perf->enabled ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point();
        update_signals(c);
        if (perf && perf->enabled) {
            perf->dut_update_ns += elapsed_ns(t0, std::chrono::steady_clock::now());
        }
    }

    void update_signals(std::uint64_t c) {
        for (std::size_t i = 0; i < kCreditPerfSignalCount; ++i) {
            const std::uint64_t idx = static_cast<std::uint64_t>(i);
            signals[i] = (c * 0x9e3779b97f4a7c15ull) ^
                         (idx * 0xbf58476d1ce4e5b9ull) ^
                         ((c + idx) << (idx & 7u));
        }
    }
};

namespace reflect {
template<> struct is_reflected<CreditPerfDut> : std::true_type {};
template<> struct reflected_visitor<CreditPerfDut> {
    template<class P, class V, class G>
    static void visit(const CreditPerfDut* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("signals", std::addressof(obj->signals));
    }
};
}

struct CreditPerfSampler : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    wave::WaveTap* tap;
    wave::Tracer* tracer;
    PathStableWvz4Recorder* recorder;
    std::uint64_t cycles_to_sample;
    std::string* error;
    PerfBreakdown* perf;

    SC_HAS_PROCESS(CreditPerfSampler);

    CreditPerfSampler(sc_core::sc_module_name name,
                      wave::WaveTap* wave_tap,
                      wave::Tracer* wave_tracer,
                      PathStableWvz4Recorder* wave_recorder,
                      std::uint64_t cycles,
                      std::string* error_out,
                      PerfBreakdown* perf_out)
        : sc_core::sc_module(name),
          clk("clk"),
          tap(wave_tap),
          tracer(wave_tracer),
          recorder(wave_recorder),
          cycles_to_sample(cycles),
          error(error_out),
          perf(perf_out) {
        SC_THREAD(run);
    }

    void run() {
        if (perf && perf->enabled && tracer) {
            tracer->attach_current_thread_for_dirty_peek();
        }
        for (std::uint64_t i = 0; i < cycles_to_sample; ++i) {
            if (perf && perf->enabled) {
                wait(clk.posedge_event());
                wait(sc_core::SC_ZERO_TIME);
                if (!sample_one_cycle_profiled(i)) {
                    sc_core::sc_stop();
                    return;
                }
            } else {
                wait(clk.negedge_event());
                wait(sc_core::SC_ZERO_TIME);
                if (!tap || !tap->last_error().empty()) {
                    if (error) *error = tap ? tap->last_error() : "missing WaveTap";
                    sc_core::sc_stop();
                    return;
                }
                if (tap->next_cycle() != static_cast<wave::Cycle>(i + 1u)) {
                    if (error) *error = "WaveTap automatic sample count mismatch";
                    sc_core::sc_stop();
                    return;
                }
            }
        }
        sc_core::sc_stop();
    }

    bool sample_one_cycle_profiled(std::uint64_t cycle) {
        if (!tracer || !recorder || !perf) {
            if (error) *error = "profiled sampler is not initialized";
            return false;
        }
        std::string local_error;
        const auto t0 = std::chrono::steady_clock::now();
        recorder->begin_cycle(static_cast<wave::Cycle>(cycle));
        const auto t1 = std::chrono::steady_clock::now();
        tracer->sample(static_cast<wave::Cycle>(cycle));
        const auto t2 = std::chrono::steady_clock::now();
        const bool ok = recorder->end_cycle(static_cast<wave::Cycle>(cycle), local_error);
        const auto t3 = std::chrono::steady_clock::now();

        perf->recorder_begin_ns += elapsed_ns(t0, t1);
        perf->tracer_sample_ns += elapsed_ns(t1, t2);
        perf->recorder_end_ns += elapsed_ns(t2, t3);
        perf->sample_total_ns += elapsed_ns(t0, t3);
        if (!ok) {
            if (error) *error = local_error.empty() ? "profiled sample failed" : local_error;
            return false;
        }
        return true;
    }
};

bool parse_u64(const char* text, std::uint64_t& out) {
    if (!text || !text[0]) return false;
    char* end = NULL;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

int sc_main(int argc, char* argv[]) {
    std::string out_path = "build_vs\\systemc_helper_credit_perf.wvz4";
    std::uint64_t cycles = 100000ull;
    bool enable_lod = false;
    bool enable_zstd = false;
    bool enable_block_pipeline = false;
    bool profile_breakdown = false;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--lod") enable_lod = true;
        else if (arg == "--no-lod") enable_lod = false;
        else if (arg == "--zstd") enable_zstd = true;
        else if (arg == "--no-zstd") enable_zstd = false;
        else if (arg == "--block-pipeline") enable_block_pipeline = true;
        else if (arg == "--no-block-pipeline") enable_block_pipeline = false;
        else if (arg == "--profile-breakdown") profile_breakdown = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: smoke_systemc_helper_credit_perf [out.wvz4] [cycles]"
                      << " [--lod|--no-lod] [--zstd|--no-zstd]"
                      << " [--block-pipeline|--no-block-pipeline]"
                      << " [--profile-breakdown]\n";
            return 0;
        } else if (positional == 0) {
            out_path = arg;
            ++positional;
        } else if (positional == 1) {
            if (!parse_u64(arg.c_str(), cycles) || cycles == 0) {
                std::cerr << "invalid cycle count: " << arg << "\n";
                return 2;
            }
            ++positional;
        } else {
            std::cerr << "too many arguments\n";
            return 2;
        }
    }

    sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
    CreditPerfDut dut("dut");
    dut.clk(clk);
    PerfBreakdown perf;
    perf.enabled = profile_breakdown;
    dut.perf = profile_breakdown ? &perf : NULL;

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
    cfg.options.enable_lod_tables = enable_lod;
    cfg.options.enable_block_pipeline = enable_block_pipeline;
    cfg.options.block_pipeline_queue_limit = 16;

    if (!recorder.open(cfg, error)) {
        std::cerr << "recorder.open failed: " << error << "\n";
        return 1;
    }

    wave::BuildOptions opt;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_flat_memory_block_precheck = true;
    opt.enable_dirty_peek_groups = false;
    opt.enable_wave_value_dirty = true;
    opt.enable_wave_value_address_hash = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_parallel_sampling = false;
    opt.debug_log = false;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("dut", &dut);

    std::unique_ptr<wave::WaveTap> tap;
    if (!profile_breakdown) {
        tap.reset(new wave::WaveTap("wave_tap", tracer, recorder, clk));
    }
    if (profile_breakdown) {
        const auto t0 = std::chrono::steady_clock::now();
        tracer.prepare_topology(0);
        tracer.attach_current_thread_for_dirty_peek();
        if (!recorder.open_writer_if_needed(error)) {
            std::cerr << "recorder.open_writer_if_needed failed: " << error << "\n";
            return 5;
        }
        perf.preopen_ns = elapsed_ns(t0, std::chrono::steady_clock::now());
    }

    CreditPerfSampler sampler("credit_perf_sampler",
                              tap.get(),
                              profile_breakdown ? &tracer : NULL,
                              profile_breakdown ? &recorder : NULL,
                              cycles,
                              &error,
                              profile_breakdown ? &perf : NULL);
    sampler.clk(clk);

    const auto start = std::chrono::steady_clock::now();
    sc_core::sc_start();
    const auto after_sim = std::chrono::steady_clock::now();

    if (!error.empty()) {
        std::cerr << "sample failed: " << error << "\n";
        return 3;
    }
    if (!recorder.close(error)) {
        std::cerr << "recorder.close failed: " << error << "\n";
        return 4;
    }
    const auto end = std::chrono::steady_clock::now();

    const double sim_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(after_sim - start).count()) / 1000.0;
    const double close_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - after_sim).count()) / 1000.0;
    const double total_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) / 1000.0;
    const double cycles_per_sec = total_ms > 0.0
        ? (static_cast<double>(cycles) * 1000.0 / total_ms)
        : 0.0;

    std::cout << std::fixed << std::setprecision(3)
              << "systemc_helper_credit_perf_ok"
              << " cycles=" << cycles
              << " signals=" << static_cast<unsigned long long>(kCreditPerfSignalCount)
              << " file=" << out_path
              << " zstd=" << (enable_zstd ? 1 : 0)
              << " lod=" << (enable_lod ? 1 : 0)
              << " block_pipeline=" << (enable_block_pipeline ? 1 : 0)
              << " profile_breakdown=" << (profile_breakdown ? 1 : 0)
              << " sim_sample_ms=" << sim_ms
              << " close_ms=" << close_ms
              << " total_ms=" << total_ms
              << " cycles_per_sec=" << cycles_per_sec
              << " dut_cycles=" << dut.cycle
              << " sim_time=" << sc_core::sc_time_stamp();
    if (profile_breakdown) {
        const double dut_update_ms = ns_to_ms(perf.dut_update_ns);
        const double sample_total_profile_ms = ns_to_ms(perf.sample_total_ns);
        const double accounted_ms = dut_update_ms + sample_total_profile_ms;
        const double other_ms = sim_ms > accounted_ms ? (sim_ms - accounted_ms) : 0.0;
        std::cout << " preopen_ms=" << ns_to_ms(perf.preopen_ns)
                  << " dut_update_ms=" << dut_update_ms
                  << " sample_profile_ms=" << sample_total_profile_ms
                  << " recorder_begin_ms=" << ns_to_ms(perf.recorder_begin_ns)
                  << " tracer_inner_ms=" << ns_to_ms(perf.tracer_sample_ns)
                  << " recorder_end_submit_ms=" << ns_to_ms(perf.recorder_end_ns)
                  << " systemc_other_ms=" << other_ms;
    }
    std::cout << "\n";
    return 0;
}
