#include <systemc>

#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

struct PeekPayload {
    bool peek_flag = false;
    std::uint32_t peek_count = 0;
    int peek_delta = 0;
};

struct DynamicPayload {
    bool dynamic_flag = false;
    std::uint32_t dynamic_count = 0;
    int dynamic_delta = 0;
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

template<> struct is_reflected<DynamicPayload> : std::true_type {};
template<> struct reflected_visitor<DynamicPayload> {
    template<class P, class V, class G>
    static void visit(const DynamicPayload* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("dynamic_flag", std::addressof(obj->dynamic_flag));
        on_ptr("dynamic_count", std::addressof(obj->dynamic_count));
        on_ptr("dynamic_delta", std::addressof(obj->dynamic_delta));
    }
};
}

struct PeekPayloadIf : virtual sc_core::sc_interface {
    virtual const PeekPayload* peek() const = 0;
};

struct DynamicPayloadIf : virtual sc_core::sc_interface {
    virtual const DynamicPayload* debug_value() const = 0;
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

struct DynamicPayloadChannel
    : sc_core::sc_channel,
      DynamicPayloadIf,
      wave::DynamicTraceTargetFor<DynamicPayloadChannel> {
    DynamicPayload value;

    explicit DynamicPayloadChannel(sc_core::sc_module_name name)
        : sc_core::sc_channel(name) {}

    const DynamicPayload* debug_value() const override {
        return &value;
    }

    void write(bool flag, std::uint32_t count, int delta) {
        value.dynamic_flag = flag;
        value.dynamic_count = count;
        value.dynamic_delta = delta;
        wave_dirty_hook()->mark_dirty();
    }
};

namespace reflect {
template<> struct is_reflected<DynamicPayloadChannel> : std::true_type {};
template<> struct reflected_visitor<DynamicPayloadChannel> {
    template<class P, class V, class G>
    static void visit(const DynamicPayloadChannel* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(obj->value));
    }
};
}

struct DerivedPeekPayloadPort : sc_core::sc_port<PeekPayloadIf> {
    explicit DerivedPeekPayloadPort(const char* name)
        : sc_core::sc_port<PeekPayloadIf>(name) {}
};

struct DerivedDynamicPayloadPort : sc_core::sc_port<DynamicPayloadIf> {
    explicit DerivedDynamicPayloadPort(const char* name)
        : sc_core::sc_port<DynamicPayloadIf>(name) {}
};

static_assert(wave::detail::is_sc_port<DerivedPeekPayloadPort>::value,
              "Derived sc_port must be recognized by WaveTrace");
static_assert(wave::detail::is_sc_port<DerivedDynamicPayloadPort>::value,
              "Derived dynamic sc_port must be recognized by WaveTrace");

struct SystemCTraceDut : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    DerivedPeekPayloadPort derived_peek_port;
    DerivedDynamicPayloadPort derived_dynamic_port;
    sc_core::sc_signal<bool> valid;
    sc_core::sc_signal<unsigned int> counter;
    sc_core::sc_buffer<int> delta;

    int cycle = 0;

    SC_HAS_PROCESS(SystemCTraceDut);

    explicit SystemCTraceDut(sc_core::sc_module_name name)
        : sc_core::sc_module(name),
          clk("clk"),
          derived_peek_port("derived_peek_port"),
          derived_dynamic_port("derived_dynamic_port"),
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
        on_ptr("derived_dynamic_port", std::addressof(obj->derived_dynamic_port));
        on_ptr("valid", std::addressof(obj->valid));
        on_ptr("counter", std::addressof(obj->counter));
        on_ptr("delta", std::addressof(obj->delta));
    }
};
}

struct PeekPayloadProducer : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    PeekPayloadChannel& channel;
    DynamicPayloadChannel& dynamic_channel;
    int cycle = 0;

    SC_HAS_PROCESS(PeekPayloadProducer);

    PeekPayloadProducer(sc_core::sc_module_name name, PeekPayloadChannel& ch, DynamicPayloadChannel& dyn)
        : sc_core::sc_module(name),
          clk("clk"),
          channel(ch),
          dynamic_channel(dyn) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        channel.write((cycle & 1) == 0,
                      static_cast<std::uint32_t>(1000 + cycle * 11),
                      cycle - 9);
        dynamic_channel.write((cycle & 1) != 0,
                              static_cast<std::uint32_t>(2000 + cycle * 13),
                              cycle - 19);
        ++cycle;
    }
};

class CheckingPathStableWvz4Recorder : public PathStableWvz4Recorder {
public:
    std::set<std::string> declared_track_paths;

    void on_track_declared(const wave::TrackDecl& decl) override {
        on_track_declared_fast(
            decl.track_id,
            decl.storage_id,
            decl.node_id,
            decl.kind,
            decl.bit_width,
            decl.bit_offset,
            decl.storage_only,
            decl.path);
    }

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        declared_track_paths.insert(path);
        PathStableWvz4Recorder::on_track_declared_fast(
            track_id,
            storage_id,
            node_id,
            kind,
            bit_width,
            bit_offset,
            storage_only,
            path);
    }
};

class ThrowingPathStableWvz4Recorder : public PathStableWvz4Recorder {
public:
    void on_track_declared(const wave::TrackDecl&) override {
        throw std::runtime_error("injected topology declaration failure");
    }

    void on_track_declared_fast(wave::TrackId,
                                wave::TrackId,
                                wave::NodeId,
                                wave::ValueKind,
                                std::uint32_t,
                                std::uint32_t,
                                bool,
                                const std::string&) override {
        throw std::runtime_error("injected topology declaration failure");
    }
};

struct SystemCWaveStopper : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    wave::WaveTap& tap;
    int cycles_to_sample;
    std::string* error;

    SC_HAS_PROCESS(SystemCWaveStopper);

    SystemCWaveStopper(sc_core::sc_module_name name,
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
            wait(clk.negedge_event());
            wait(sc_core::SC_ZERO_TIME);
            if (!tap.last_error().empty()) {
                if (error) *error = tap.last_error();
                sc_core::sc_stop();
                return;
            }
            if (tap.next_cycle() != static_cast<wave::Cycle>(i + 2)) {
                if (error) {
                    *error = "WaveTap did not sample exactly once at start and once on the falling edge";
                }
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
    DynamicPayloadChannel dynamic_channel("dynamic_channel");
    SystemCTraceDut dut("dut");
    dut.clk(clk);
    dut.derived_peek_port(peek_channel);
    dut.derived_dynamic_port(dynamic_channel);

    PeekPayloadProducer producer("peek_producer", peek_channel, dynamic_channel);
    producer.clk(clk);

    std::string error;
    CheckingPathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
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

    wave::ensure_dynamic_type_registered<PeekPayload>();
    wave::ensure_dynamic_type_registered<DynamicPayloadChannel>();

    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
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

    // A WaveTrace exception must be converted into a fatal WaveTap error and
    // must never cross the SystemC callback boundary as E549.
    ThrowingPathStableWvz4Recorder throwing_recorder;
    wave::Tracer throwing_tracer(throwing_recorder, opt);
    std::uint32_t throwing_root = 0x1234u;
    throwing_tracer.add_root("throwing_root", &throwing_root);
    wave::WaveTap throwing_tap(
        "throwing_wave_tap", throwing_tracer, throwing_recorder, clk);
    if (throwing_tap.sample_one_cycle() ||
        !throwing_tap.has_fatal_error() ||
        throwing_tap.next_cycle() != 0u ||
        throwing_tap.last_error().find("injected topology declaration failure") ==
            std::string::npos) {
        std::cerr << "WaveTap exception boundary did not latch the injected failure: "
                  << throwing_tap.last_error() << "\n";
        return 6;
    }

    wave::WaveTap tap("wave_tap", tracer, recorder, clk);
    SystemCWaveStopper stopper("wave_stopper", tap, cycles, &error);
    stopper.clk(clk);

    sc_core::sc_start();

    if (!error.empty()) {
        std::cerr << "sample failed: " << error << "\n";
        return 2;
    }

    if (!recorder.close(error)) {
        std::cerr << "recorder.close failed: " << error << "\n";
        return 3;
    }

    if (recorder.declared_track_paths.find("dut.derived_peek_port.peek_count") == recorder.declared_track_paths.end()) {
        std::cerr << "missing PeekTraceSourceFor sc_port track declaration\n";
        return 4;
    }
    if (recorder.declared_track_paths.find("dut.derived_dynamic_port.value.dynamic_count") == recorder.declared_track_paths.end()) {
        std::cerr << "missing DynamicTraceTargetFor sc_port track declaration\n";
        return 5;
    }

    std::cout << "systemc_wvz4_ok cycles=" << cycles
              << " file=" << out_path
              << " dirty_peek=" << (enable_dirty_peek ? 1 : 0)
              << " producer_cycles=" << producer.cycle
              << " peek_flag=" << (peek_channel.value.peek_flag ? 1 : 0)
              << " peek_count=" << peek_channel.value.peek_count
              << " peek_delta=" << peek_channel.value.peek_delta
              << " dynamic_count=" << dynamic_channel.value.dynamic_count
              << " dynamic_delta=" << dynamic_channel.value.dynamic_delta
              << " sampled_cycles=" << tap.next_cycle()
              << " sim_time=" << sc_core::sc_time_stamp() << "\n";
    return 0;
}
