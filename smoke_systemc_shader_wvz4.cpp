#include <systemc>

#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

struct ShaderLaneTrace {
    std::uint32_t pc = 0;
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
    bool predicate = false;
    bool active = false;
};

struct ShaderTelemetry {
    std::uint32_t cycle = 0;
    std::uint32_t dispatch_id = 0;
    std::uint32_t warp_id = 0;
    std::uint32_t active_mask = 0;
    std::uint32_t stall_reason = 0;
    std::uint64_t texture_addr = 0;
    bool dispatch_valid = false;
    bool issue_valid = false;
    bool retire_valid = false;
    std::array<ShaderLaneTrace, 4> lanes;
};

namespace reflect {
template<> struct is_reflected<ShaderLaneTrace> : std::true_type {};
template<> struct reflected_visitor<ShaderLaneTrace> {
    template<class P, class V, class G>
    static void visit(const ShaderLaneTrace* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("pc", std::addressof(obj->pc));
        on_ptr("r0", std::addressof(obj->r0));
        on_ptr("r1", std::addressof(obj->r1));
        on_ptr("predicate", std::addressof(obj->predicate));
        on_ptr("active", std::addressof(obj->active));
    }
};

template<> struct is_reflected<ShaderTelemetry> : std::true_type {};
template<> struct reflected_visitor<ShaderTelemetry> {
    template<class P, class V, class G>
    static void visit(const ShaderTelemetry* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("cycle", std::addressof(obj->cycle));
        on_ptr("dispatch_id", std::addressof(obj->dispatch_id));
        on_ptr("warp_id", std::addressof(obj->warp_id));
        on_ptr("active_mask", std::addressof(obj->active_mask));
        on_ptr("stall_reason", std::addressof(obj->stall_reason));
        on_ptr("texture_addr", std::addressof(obj->texture_addr));
        on_ptr("dispatch_valid", std::addressof(obj->dispatch_valid));
        on_ptr("issue_valid", std::addressof(obj->issue_valid));
        on_ptr("retire_valid", std::addressof(obj->retire_valid));
        on_ptr("lanes", std::addressof(obj->lanes));
    }
};
}

struct ShaderTelemetryIf : virtual sc_core::sc_interface {
    virtual void publish(const ShaderTelemetry& value) = 0;
    virtual const ShaderTelemetry* peek() const = 0;
};

struct ShaderTelemetryChannel
    : sc_core::sc_channel,
      ShaderTelemetryIf,
      wave::PeekTraceSourceFor<ShaderTelemetryChannel, ShaderTelemetry> {
    ShaderTelemetry value;

    explicit ShaderTelemetryChannel(sc_core::sc_module_name name)
        : sc_core::sc_channel(name) {}

    void publish(const ShaderTelemetry& next) override {
        value = next;
        wave_dirty_hook()->mark_dirty();
    }

    const ShaderTelemetry* peek() const override {
        return &value;
    }
};

struct DerivedShaderTelemetryPort : sc_core::sc_port<ShaderTelemetryIf> {
    explicit DerivedShaderTelemetryPort(const char* name)
        : sc_core::sc_port<ShaderTelemetryIf>(name) {}
};

static_assert(wave::detail::is_sc_port<DerivedShaderTelemetryPort>::value,
              "Derived shader sc_port must be recognized by WaveTrace");

static std::uint32_t mix32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

struct ShaderCoreDut : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    DerivedShaderTelemetryPort telemetry;

    sc_core::sc_signal<bool> dispatch_valid;
    sc_core::sc_signal<bool> issue_valid;
    sc_core::sc_signal<bool> retire_valid;
    sc_core::sc_signal<bool> scoreboard_stall;
    sc_core::sc_signal<unsigned int> dispatch_id;
    sc_core::sc_signal<unsigned int> warp_id;
    sc_core::sc_signal<unsigned int> instruction;
    sc_core::sc_signal<unsigned int> active_mask;
    sc_core::sc_signal<unsigned int> stall_reason;
    sc_core::sc_buffer<std::uint64_t> frame_hash;

    std::uint32_t cycle = 0;

    SC_HAS_PROCESS(ShaderCoreDut);

    explicit ShaderCoreDut(sc_core::sc_module_name name)
        : sc_core::sc_module(name),
          clk("clk"),
          telemetry("telemetry"),
          dispatch_valid("dispatch_valid"),
          issue_valid("issue_valid"),
          retire_valid("retire_valid"),
          scoreboard_stall("scoreboard_stall"),
          dispatch_id("dispatch_id"),
          warp_id("warp_id"),
          instruction("instruction"),
          active_mask("active_mask"),
          stall_reason("stall_reason"),
          frame_hash("frame_hash") {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        const std::uint32_t warp = (cycle / 4u) & 7u;
        const bool texture_stall = (cycle % 11u) == 3u;
        const bool score_stall = (cycle % 17u) == 5u;
        const bool issue = !texture_stall && !score_stall;
        static const std::uint32_t kActiveMasks[] = {
            0xFu, 0x7u, 0xBu, 0xDu, 0xEu, 0x5u, 0xAu, 0x3u
        };
        const std::uint32_t mask = kActiveMasks[cycle & 7u];
        const std::uint32_t instr = 0xE0000000u | ((cycle & 0xFFu) << 8) | (warp << 4) | (issue ? 1u : 0u);
        const std::uint64_t tex_addr = 0x80000000ull +
            (static_cast<std::uint64_t>(warp) << 20) +
            (static_cast<std::uint64_t>(cycle & 0x3FFu) << 6);
        const std::uint64_t hash =
            (static_cast<std::uint64_t>(mix32(cycle)) << 32) |
            static_cast<std::uint64_t>(mix32(cycle ^ 0xA5A50000u));

        dispatch_valid.write((cycle % 3u) != 2u);
        issue_valid.write(issue);
        retire_valid.write((cycle % 5u) == 0u);
        scoreboard_stall.write(score_stall);
        dispatch_id.write(cycle / 3u);
        warp_id.write(warp);
        instruction.write(instr);
        active_mask.write(mask);
        stall_reason.write(texture_stall ? 2u : (score_stall ? 1u : 0u));
        frame_hash.write(hash);

        ShaderTelemetry t;
        t.cycle = cycle;
        t.dispatch_id = cycle / 3u;
        t.warp_id = warp;
        t.active_mask = mask;
        t.stall_reason = texture_stall ? 2u : (score_stall ? 1u : 0u);
        t.texture_addr = tex_addr;
        t.dispatch_valid = (cycle % 3u) != 2u;
        t.issue_valid = issue;
        t.retire_valid = (cycle % 5u) == 0u;
        for (std::size_t lane = 0; lane < t.lanes.size(); ++lane) {
            const std::uint32_t lane_id = static_cast<std::uint32_t>(lane);
            ShaderLaneTrace& l = t.lanes[lane];
            l.active = ((mask >> lane_id) & 1u) != 0u;
            l.predicate = ((cycle + lane_id) % 3u) != 1u;
            l.pc = 0x1000u + warp * 0x100u + cycle * 4u + lane_id;
            l.r0 = mix32(cycle ^ (lane_id << 8));
            l.r1 = mix32((cycle * 33u) ^ (lane_id << 12) ^ warp);
        }
        telemetry->publish(t);
        ++cycle;
    }
};

namespace reflect {
template<> struct is_reflected<ShaderCoreDut> : std::true_type {};
template<> struct reflected_visitor<ShaderCoreDut> {
    template<class P, class V, class G>
    static void visit(const ShaderCoreDut* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("clk", std::addressof(obj->clk));
        on_ptr("telemetry", std::addressof(obj->telemetry));
        on_ptr("dispatch_valid", std::addressof(obj->dispatch_valid));
        on_ptr("issue_valid", std::addressof(obj->issue_valid));
        on_ptr("retire_valid", std::addressof(obj->retire_valid));
        on_ptr("scoreboard_stall", std::addressof(obj->scoreboard_stall));
        on_ptr("dispatch_id", std::addressof(obj->dispatch_id));
        on_ptr("warp_id", std::addressof(obj->warp_id));
        on_ptr("instruction", std::addressof(obj->instruction));
        on_ptr("active_mask", std::addressof(obj->active_mask));
        on_ptr("stall_reason", std::addressof(obj->stall_reason));
        on_ptr("frame_hash", std::addressof(obj->frame_hash));
    }
};
}

struct ShaderWaveSampler : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    wave::WaveTap& tap;
    int cycles_to_sample;
    std::string* error;

    SC_HAS_PROCESS(ShaderWaveSampler);

    ShaderWaveSampler(sc_core::sc_module_name name,
                      wave::WaveTap& wave_tap,
                      int cycles,
                      std::string* error_out)
        : sc_core::sc_module(name),
          clk("clk"),
          tap(wave_tap),
          cycles_to_sample(cycles),
          error(error_out) {
        SC_THREAD(run);
    }

    void run() {
        for (int i = 0; i < cycles_to_sample; ++i) {
            wait(clk.posedge_event());
            wait(sc_core::SC_ZERO_TIME);
            if (!tap.sample_one_cycle()) {
                if (error) *error = tap.last_error();
                sc_core::sc_stop();
                return;
            }
        }
        sc_core::sc_stop();
    }
};

int sc_main(int argc, char* argv[]) {
    const std::string out_path =
        argc >= 2 ? argv[1] : "build_vs\\systemc_shader_smoke.wvz4";
    const int cycles =
        argc >= 3 ? std::max(1, std::atoi(argv[2])) : 1024;

    bool enable_dirty_peek = true;
    bool use_helper = true;
    bool debug_log = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--poll-peek") enable_dirty_peek = false;
        else if (arg == "--dirty-peek") enable_dirty_peek = true;
        else if (arg == "--direct-writer") use_helper = false;
        else if (arg == "--helper") use_helper = true;
        else if (arg == "--debug-log") debug_log = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: smoke_systemc_shader_wvz4 [out.wvz4] [cycles]"
                      << " [--dirty-peek|--poll-peek] [--helper|--direct-writer] [--debug-log]\n";
            return 0;
        }
    }

    sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
    ShaderTelemetryChannel telemetry_channel("shader_telemetry_channel");
    ShaderCoreDut shader("shader");
    shader.clk(clk);
    shader.telemetry(telemetry_channel);

    std::string error;
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.use_writer_process = use_helper;
    cfg.async_writer = false;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.clk_fall_offset_ticks = 5;
    cfg.options.compression = wvz4::Compression::Zstd;
    cfg.options.zstd_level = 1;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 512;

    if (!recorder.open(cfg, error)) {
        std::cerr << "recorder.open failed: " << error << "\n";
        return 1;
    }

    wave::BuildOptions opt;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_flat_memory_block_precheck = true;
    opt.enable_dirty_peek_groups = enable_dirty_peek;
    opt.dirty_peek_parallel_threshold = 1;
    opt.enable_dirty_peek_memory_block_precheck = true;
    opt.enable_wave_value_dirty = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_parallel_sampling = false;
    opt.debug_log = debug_log;
    opt.debug_log_path = out_path + ".runtime.log";

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("shader", &shader);

    wave::WaveTap tap(tracer, recorder);
    ShaderWaveSampler sampler("shader_wave_sampler", tap, cycles, &error);
    sampler.clk(clk);

    sc_core::sc_start();

    if (!error.empty()) {
        std::cerr << "sample failed: " << error << "\n";
        return 2;
    }
    if (!recorder.close(error)) {
        std::cerr << "recorder.close failed: " << error << "\n";
        return 3;
    }

    std::cout << "systemc_shader_wvz4_ok cycles=" << cycles
              << " file=" << out_path
              << " dirty_peek=" << (enable_dirty_peek ? 1 : 0)
              << " writer_mode=" << (use_helper ? "helper" : "direct")
              << " shader_cycles=" << shader.cycle
              << " sim_time=" << sc_core::sc_time_stamp() << "\n";
    return 0;
}
