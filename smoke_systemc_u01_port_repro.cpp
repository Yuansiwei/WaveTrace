#include <systemc>

#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

typedef unsigned char U01;

struct U01Payload {
    U01 flag;
};

namespace reflect {
template<> struct is_reflected<U01Payload> : std::true_type {};
template<> struct reflected_visitor<U01Payload> {
    template<class P, class V, class G>
    static void visit(const U01Payload* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("flag", ::wave::as_bool_storage_ptr(std::addressof(obj->flag)));
    }
};
}

struct U01PayloadIf : virtual sc_core::sc_interface {
    virtual const U01Payload* peek() const = 0;
};

struct U01PayloadChannel
    : sc_core::sc_channel,
      U01PayloadIf,
      wave::PeekTraceSourceFor<U01PayloadChannel, U01Payload> {
    U01Payload value;

    explicit U01PayloadChannel(sc_core::sc_module_name name)
        : sc_core::sc_channel(name), value{0} {}

    const U01Payload* peek() const override { return &value; }

    void write(U01 next) {
        value.flag = next;
        wave_dirty_hook()->mark_dirty();
    }
};

struct U01PayloadPort : sc_core::sc_port<U01PayloadIf> {
    explicit U01PayloadPort(const char* name)
        : sc_core::sc_port<U01PayloadIf>(name) {}
};

template <typename T>
struct U01LeafIf : virtual sc_core::sc_interface {
    virtual const T* peek() const = 0;
};

struct U01LeafChannel
    : sc_core::sc_channel,
      U01LeafIf<U01>,
      wave::PeekTraceSourceFor<U01LeafChannel, U01> {
    U01 value;

    explicit U01LeafChannel(sc_core::sc_module_name name)
        : sc_core::sc_channel(name), value(0) {}

    const U01* peek() const override { return &value; }

    void write(U01 next) {
        value = next;
        wave_dirty_hook()->mark_dirty();
    }
};

struct ByteLeafChannel
    : sc_core::sc_channel,
      U01LeafIf<unsigned char>,
      wave::PeekTraceSourceFor<ByteLeafChannel, unsigned char> {
    unsigned char value;

    explicit ByteLeafChannel(sc_core::sc_module_name name)
        : sc_core::sc_channel(name), value(0) {}

    const unsigned char* peek() const override { return &value; }

    void write(unsigned char next) {
        value = next;
        wave_dirty_hook()->mark_dirty();
    }
};

struct U01PortDut : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<U01> input;
    sc_core::sc_port<U01LeafIf<U01> > leaf_port;
    sc_core::sc_port<U01LeafIf<unsigned char> > byte_port;
    U01PayloadPort payload_port;
    sc_core::sc_signal<U01> internal;
    U01 direct;
    int cycle;

    SC_HAS_PROCESS(U01PortDut);

    explicit U01PortDut(sc_core::sc_module_name name)
        : sc_core::sc_module(name),
          clk("clk"),
          input("input"),
          leaf_port("leaf_port"),
          byte_port("byte_port"),
          payload_port("payload_port"),
          internal("internal"),
          direct(0),
          cycle(0) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        direct = static_cast<U01>((cycle + 1) & 1);
        internal.write(static_cast<U01>((cycle + 1) & 1));
        ++cycle;
    }
};

namespace reflect {
template<> struct is_reflected<U01PortDut> : std::true_type {};
template<> struct reflected_visitor<U01PortDut> {
    template<class P, class V, class G>
    static void visit(const U01PortDut* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("input", std::addressof(obj->input), ::wave::BoolLeafSourceTag());
        on_ptr("leaf_port", std::addressof(obj->leaf_port), ::wave::BoolLeafSourceTag());
        on_ptr("byte_port", std::addressof(obj->byte_port));
        on_ptr("payload_port", std::addressof(obj->payload_port));
        on_ptr("internal", std::addressof(obj->internal), ::wave::BoolLeafSourceTag());
        // This is what ReflectGen emits for a field whose source spelling is U01.
        on_ptr("direct", ::wave::as_bool_storage_ptr(std::addressof(obj->direct)));
    }
};
}

struct U01Producer : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    sc_core::sc_signal<U01>& output;
    U01LeafChannel& leaf_channel;
    ByteLeafChannel& byte_channel;
    U01PayloadChannel& payload_channel;
    int cycle;

    SC_HAS_PROCESS(U01Producer);

    U01Producer(sc_core::sc_module_name name,
                sc_core::sc_signal<U01>& out,
                U01LeafChannel& leaf,
                ByteLeafChannel& byte,
                U01PayloadChannel& payload)
        : sc_core::sc_module(name),
          clk("clk"),
          output(out),
          leaf_channel(leaf),
          byte_channel(byte),
          payload_channel(payload),
          cycle(0) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void step() {
        const U01 next = static_cast<U01>((cycle + 1) & 1);
        output.write(next);
        leaf_channel.write(next);
        byte_channel.write(static_cast<unsigned char>(0xa0u + (cycle & 0x0fu)));
        payload_channel.write(next);
        ++cycle;
    }
};

struct DeclInfo {
    std::string path;
    wave::ValueKind kind;
    std::uint32_t width;
};

class CapturingRecorder : public PathStableWvz4Recorder {
public:
    std::vector<DeclInfo> declarations;

    void on_track_declared(const wave::TrackDecl& decl) override {
        on_track_declared_fast(decl.track_id, decl.storage_id, decl.node_id,
                               decl.kind, decl.bit_width, decl.bit_offset,
                               decl.storage_only, decl.path);
    }

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        declarations.push_back(DeclInfo{path, kind, bit_width});
        PathStableWvz4Recorder::on_track_declared_fast(
            track_id, storage_id, node_id, kind, bit_width, bit_offset,
            storage_only, path);
    }
};

struct Stopper : sc_core::sc_module {
    sc_core::sc_in<bool> clk;
    wave::WaveTap& tap;
    int remaining;

    SC_HAS_PROCESS(Stopper);

    Stopper(sc_core::sc_module_name name, wave::WaveTap& wave_tap, int cycles)
        : sc_core::sc_module(name), clk("clk"), tap(wave_tap), remaining(cycles) {
        SC_THREAD(run);
    }

    void run() {
        while (remaining-- > 0) {
            wait(clk.negedge_event());
            wait(sc_core::SC_ZERO_TIME);
            if (tap.has_fatal_error()) {
                sc_core::sc_stop();
                return;
            }
        }
        sc_core::sc_stop();
    }
};

static const char* kind_name(wave::ValueKind kind) {
    switch (kind) {
    case wave::ValueKind::Bool: return "Bool";
    case wave::ValueKind::SignedInt: return "SignedInt";
    case wave::ValueKind::UnsignedInt: return "UnsignedInt";
    default: return "Other";
    }
}

int sc_main(int argc, char* argv[]) {
    const std::string output_path =
        argc >= 2 ? argv[1] : "build_vs\\systemc_u01_port_repro.wvz4";

    sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
    sc_core::sc_signal<U01> source("source");
    U01LeafChannel leaf_channel("leaf_channel");
    ByteLeafChannel byte_channel("byte_channel");
    U01PayloadChannel payload_channel("payload_channel");
    U01PortDut dut("dut");
    dut.clk(clk);
    dut.input(source);
    dut.leaf_port(leaf_channel);
    dut.byte_port(byte_channel);
    dut.payload_port(payload_channel);
    U01Producer producer("producer", source, leaf_channel, byte_channel, payload_channel);
    producer.clk(clk);

    std::string error;
    CapturingRecorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = output_path;
    cfg.emit_default_clk = false;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    if (!recorder.open(cfg, error)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }

    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.enable_parallel_sampling = false;
    wave::ensure_dynamic_type_registered<U01Payload>();
    wave::ensure_dynamic_type_registered<U01>();
    wave::Tracer tracer(recorder, options);
    tracer.add_root("dut", &dut);

    wave::WaveTap tap("wave_tap", tracer, recorder, clk);
    Stopper stopper("stopper", tap, 4);
    stopper.clk(clk);
    sc_core::sc_start();

    if (tap.has_fatal_error()) {
        std::cerr << "sample failed: " << tap.last_error() << "\n";
        return 2;
    }
    if (!recorder.close(error)) {
        std::cerr << "close failed: " << error << "\n";
        return 3;
    }

    bool direct_is_one_bit = false;
    bool generic_port_child_is_one_bit = false;
    bool input_is_one_bit = false;
    bool leaf_port_is_one_bit = false;
    bool internal_is_one_bit = false;
    bool byte_port_is_eight_bits = false;
    for (const DeclInfo& decl : recorder.declarations) {
        std::cout << decl.path << " kind=" << kind_name(decl.kind)
                  << " width=" << decl.width << "\n";
        if (decl.path == "dut.direct" &&
            decl.kind == wave::ValueKind::Bool && decl.width == 1) {
            direct_is_one_bit = true;
        }
        if (decl.path == "dut.payload_port.flag" &&
            decl.kind == wave::ValueKind::Bool && decl.width == 1) {
            generic_port_child_is_one_bit = true;
        }
        if (decl.path == "dut.input" && decl.kind == wave::ValueKind::Bool && decl.width == 1)
            input_is_one_bit = true;
        if (decl.path == "dut.leaf_port" && decl.kind == wave::ValueKind::Bool && decl.width == 1)
            leaf_port_is_one_bit = true;
        if (decl.path == "dut.internal" && decl.kind == wave::ValueKind::Bool && decl.width == 1)
            internal_is_one_bit = true;
        if (decl.path == "dut.byte_port" && decl.kind == wave::ValueKind::UnsignedInt && decl.width == 8)
            byte_port_is_eight_bits = true;
    }

    if (!direct_is_one_bit) {
        std::cerr << "control failure: direct reflected U01 is not one bit\n";
        return 4;
    }
    if (!generic_port_child_is_one_bit) {
        std::cerr << "control failure: reflected U01 under generic sc_port is not one bit\n";
        return 6;
    }
    if (!input_is_one_bit || !leaf_port_is_one_bit || !internal_is_one_bit) {
        std::cerr << "regression: tagged sc_port/sc_in/sc_signal<U01> is not one bit\n";
        return 5;
    }
    if (!byte_port_is_eight_bits) {
        std::cerr << "regression: ordinary unsigned-char sc_port is not eight bits\n";
        return 7;
    }

    std::cout << "FIXED: leaf-valued sc_port/sc_in/sc_signal<U01> was declared as Bool/1-bit; file="
              << output_path << "\n";
    return 0;
}
