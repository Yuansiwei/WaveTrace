#include <systemc>

#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

struct PeekPayload {
    bool peek_flag = false;
    std::uint32_t peek_count = 0;
    int peek_delta = 0;
};

namespace reflect {
template<> struct is_reflected<PeekPayload> : std::true_type {};
template<> struct reflected_visitor<PeekPayload> {
    template<class P, class V, class G>
    static void visit(const PeekPayload* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("peek_flag", std::addressof(obj->peek_flag));
        on_ptr("peek_count", std::addressof(obj->peek_count));
        on_ptr("peek_delta", std::addressof(obj->peek_delta));
    }
};
}

struct PeekPayloadIf : virtual sc_core::sc_interface {
    virtual const PeekPayload* peek() const = 0;
};

struct PeekPayloadChannel
    : sc_core::sc_channel,
      PeekPayloadIf,
      wave::PeekTraceSourceFor<PeekPayloadChannel, PeekPayload> {
    PeekPayload value;

    explicit PeekPayloadChannel(sc_core::sc_module_name name)
        : sc_core::sc_channel(name) {}

    const PeekPayload* peek() const override {
        return &value;
    }

    void write(bool flag, std::uint32_t count, int delta) {
        value.peek_flag = flag;
        value.peek_count = count;
        value.peek_delta = delta;
        wave_dirty_hook()->mark_dirty();
    }
};

struct DerivedPeekPayloadPort : sc_core::sc_port<PeekPayloadIf> {
    explicit DerivedPeekPayloadPort(const char* name)
        : sc_core::sc_port<PeekPayloadIf>(name) {}
};

static_assert(wave::detail::is_sc_port<DerivedPeekPayloadPort>::value,
              "Derived sc_port must be recognized by WaveTrace");

struct SystemCTraceDut : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    DerivedPeekPayloadPort derived_peek_port;
    sc_core::sc_signal<bool> valid;
    sc_core::sc_signal<unsigned int> counter;
    sc_core::sc_buffer<int> delta;

    int cycle = 0;

    SC_HAS_PROCESS(SystemCTraceDut);

    explicit SystemCTraceDut(sc_core::sc_module_name name)
        : sc_core::sc_module(name),
          clk("clk"),
          derived_peek_port("derived_peek_port"),
          valid("valid"),
          counter("counter"),
          delta("delta") {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        valid.write((cycle & 1) != 0);
        counter.write(static_cast<unsigned int>(cycle * 3 + 1));
        delta.write(cycle - 4);
        ++cycle;
    }
};

namespace reflect {
template<> struct is_reflected<SystemCTraceDut> : std::true_type {};
template<> struct reflected_visitor<SystemCTraceDut> {
    template<class P, class V, class G>
    static void visit(const SystemCTraceDut* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("clk", std::addressof(obj->clk));
        on_ptr("derived_peek_port", std::addressof(obj->derived_peek_port));
        on_ptr("valid", std::addressof(obj->valid));
        on_ptr("counter", std::addressof(obj->counter));
        on_ptr("delta", std::addressof(obj->delta));
    }
};
}

struct PeekPayloadProducer : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    PeekPayloadChannel& channel;
    int cycle = 0;

    SC_HAS_PROCESS(PeekPayloadProducer);

    PeekPayloadProducer(sc_core::sc_module_name name, PeekPayloadChannel& ch)
        : sc_core::sc_module(name),
          clk("clk"),
          channel(ch) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        channel.write((cycle & 1) == 0,
                      static_cast<std::uint32_t>(1000 + cycle * 11),
                      cycle - 9);
        ++cycle;
    }
};

struct SystemCWaveSampler : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    wave::WaveTap& tap;
    int cycles_to_sample;
    std::string* error;

    SC_HAS_PROCESS(SystemCWaveSampler);

    SystemCWaveSampler(sc_core::sc_module_name name,
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
        argc >= 2 ? argv[1] : "build_vs\\systemc_trace_smoke.wvz4";
    const int cycles =
        argc >= 3 ? std::max(1, std::atoi(argv[2])) : 32;
    bool enable_dirty_peek = true;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--poll-peek") enable_dirty_peek = false;
        if (arg == "--dirty-peek") enable_dirty_peek = true;
    }

    sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
    PeekPayloadChannel peek_channel("peek_channel");
    SystemCTraceDut dut("dut");
    dut.clk(clk);
    dut.derived_peek_port(peek_channel);

    PeekPayloadProducer producer("peek_producer", peek_channel);
    producer.clk(clk);

    std::string error;
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.async_writer = false;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.clk_fall_offset_ticks = 5;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 256;

    if (!recorder.open(cfg, error)) {
        std::cerr << "recorder.open failed: " << error << "\n";
        return 1;
    }

    wave::BuildOptions opt;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_flat_memory_block_precheck = true;
    opt.enable_dirty_peek_groups = enable_dirty_peek;
    opt.dirty_peek_parallel_threshold = 1;
    opt.enable_wave_value_dirty = true;
    opt.enable_wave_value_address_hash = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_parallel_sampling = false;
    opt.debug_log = true;
    opt.debug_log_path = out_path + ".runtime.log";

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("dut", &dut);

    wave::WaveTap tap(tracer, recorder);
    SystemCWaveSampler sampler("wave_sampler", tap, cycles, &error);
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

    std::cout << "systemc_wvz4_ok cycles=" << cycles
              << " file=" << out_path
              << " dirty_peek=" << (enable_dirty_peek ? 1 : 0)
              << " producer_cycles=" << producer.cycle
              << " peek_flag=" << (peek_channel.value.peek_flag ? 1 : 0)
              << " peek_count=" << peek_channel.value.peek_count
              << " peek_delta=" << peek_channel.value.peek_delta
              << " sim_time=" << sc_core::sc_time_stamp() << "\n";
    return 0;
}
