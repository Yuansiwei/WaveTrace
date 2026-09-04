#include "wave_tap.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

struct ComplexFlags {
    unsigned mode : 3;
    unsigned ready : 1;
    unsigned code : 4;
    unsigned page : 8;
};

union ComplexUnion {
    std::uint32_t raw;
    struct {
        std::uint16_t lo;
        std::uint16_t hi;
    } words;
};

struct ComplexPayload {
    std::uint32_t count;
    std::int32_t delta;
    ComplexFlags flags;
};

struct ComplexSlot {
    std::uint32_t id;
    std::uint16_t a;
    std::uint16_t b;
    ComplexFlags flags;
    wave::WaveValue<std::uint32_t> counter;
};

struct ComplexPeek : public wave::PeekTraceSourceFor<ComplexPeek, ComplexPayload> {
    ComplexPayload payload;

    const ComplexPayload* peek() const {
        return &payload;
    }

    void write(std::uint32_t count, std::int32_t delta) {
        payload.count = count;
        payload.delta = delta;
        payload.flags.mode = count & 7u;
        if (wave_dirty_hook()) wave_dirty_hook()->mark_dirty();
    }
};

struct ComplexDynamic : public wave::DynamicTraceTargetFor<ComplexDynamic> {
    ComplexPayload payload;

    void write(std::uint32_t count, std::int32_t delta) {
        payload.count = count;
        payload.delta = delta;
        payload.flags.code = count & 15u;
        if (wave_dirty_hook()) wave_dirty_hook()->mark_dirty();
    }
};

struct ComplexTop {
    std::uint32_t epoch;
    ComplexFlags flags;
    ComplexUnion alias;
    wave::array<ComplexSlot, 16> slots;
    wave::WaveValue<std::uint32_t> global_dirty;
    ComplexPeek sampled;
    ComplexDynamic dynamic;
};

namespace wave {
template <> struct ReflectAccess<ComplexFlags> {
    static std::uint32_t mode(const ComplexFlags* p) { return p->mode; }
    static std::uint32_t ready(const ComplexFlags* p) { return p->ready; }
    static std::uint32_t code(const ComplexFlags* p) { return p->code; }
    static std::uint32_t page(const ComplexFlags* p) { return p->page; }
};
}

namespace reflect {
template <> struct is_reflected<ComplexFlags> : std::true_type {};
template <> struct reflected_visitor<ComplexFlags> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexFlags*, PtrVisitor&&, ValueVisitor&&, GetterVisitor&& on_getter) {
        on_getter("mode", &wave::ReflectAccess<ComplexFlags>::mode, 3u, 0u);
        on_getter("ready", &wave::ReflectAccess<ComplexFlags>::ready, 1u, 3u);
        on_getter("code", &wave::ReflectAccess<ComplexFlags>::code, 4u, 4u);
        on_getter("page", &wave::ReflectAccess<ComplexFlags>::page, 8u, 8u);
    }
};

template <> struct is_reflected<ComplexUnion> : std::true_type {};
template <> struct reflected_visitor<ComplexUnion> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexUnion* obj, PtrVisitor&& on_ptr, ValueVisitor&&, GetterVisitor&&) {
        wave::detail::invoke_ptr_visitor(on_ptr, "raw", &obj->raw,
                                          wave::detail::UnionFieldTag(), sizeof(ComplexUnion),
                                          wave::detail::UnionFieldBase(&obj->raw));
        wave::detail::invoke_ptr_visitor(on_ptr, "lo", &obj->words.lo,
                                          wave::detail::UnionFieldTag(), sizeof(ComplexUnion),
                                          wave::detail::UnionFieldBase(&obj->raw));
        wave::detail::invoke_ptr_visitor(on_ptr, "hi", &obj->words.hi,
                                          wave::detail::UnionFieldTag(), sizeof(ComplexUnion),
                                          wave::detail::UnionFieldBase(&obj->raw));
    }
};

template <> struct is_reflected<ComplexPayload> : std::true_type {};
template <> struct reflected_visitor<ComplexPayload> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexPayload* obj, PtrVisitor&& on_ptr, ValueVisitor&&, GetterVisitor&&) {
        on_ptr("count", &obj->count);
        on_ptr("delta", &obj->delta);
        on_ptr("flags", &obj->flags);
    }
};

template <> struct is_reflected<ComplexSlot> : std::true_type {};
template <> struct reflected_visitor<ComplexSlot> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexSlot* obj, PtrVisitor&& on_ptr, ValueVisitor&&, GetterVisitor&&) {
        on_ptr("id", &obj->id);
        on_ptr("a", &obj->a);
        on_ptr("b", &obj->b);
        on_ptr("flags", &obj->flags);
        on_ptr("counter", &obj->counter);
    }
};

template <> struct is_reflected<ComplexPeek> : std::true_type {};
template <> struct reflected_visitor<ComplexPeek> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexPeek* obj, PtrVisitor&& on_ptr, ValueVisitor&&, GetterVisitor&&) {
        on_ptr("payload", &obj->payload);
    }
};

template <> struct is_reflected<ComplexDynamic> : std::true_type {};
template <> struct reflected_visitor<ComplexDynamic> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexDynamic* obj, PtrVisitor&& on_ptr, ValueVisitor&&, GetterVisitor&&) {
        on_ptr("payload", &obj->payload);
    }
};

template <> struct is_reflected<ComplexTop> : std::true_type {};
template <> struct reflected_visitor<ComplexTop> {
    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
    static void visit(const ComplexTop* obj, PtrVisitor&& on_ptr, ValueVisitor&&, GetterVisitor&&) {
        on_ptr("epoch", &obj->epoch);
        on_ptr("flags", &obj->flags);
        on_ptr("alias", &obj->alias);
        on_ptr("slots", &obj->slots);
        on_ptr("global_dirty", &obj->global_dirty);
        on_ptr("sampled", &obj->sampled);
        on_ptr("dynamic", &obj->dynamic);
    }
};
}

struct ComplexRecorder : public PathStableWvz4Recorder {
    struct Decl {
        std::uint64_t track_id;
        std::uint64_t storage_id;
        std::uint32_t bit_width;
        std::uint32_t bit_offset;
    };

    std::map<std::string, Decl> decls;

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        PathStableWvz4Recorder::on_track_declared_fast(
            track_id,
            storage_id,
            node_id,
            kind,
            bit_width,
            bit_offset,
            storage_only,
            path);
        if (!path.empty()) {
            decls[path] = Decl{track_id, storage_id, bit_width, bit_offset};
        }
    }
};

static void fail(const std::string& message) {
    std::cerr << "[complex] " << message << "\n";
    std::exit(2);
}

static void expect_path(const ComplexRecorder& recorder, const std::string& path) {
    if (recorder.decls.find(path) == recorder.decls.end()) {
        fail("missing path: " + path);
    }
}

static void expect_alias(const ComplexRecorder& recorder, const std::string& path) {
    std::map<std::string, ComplexRecorder::Decl>::const_iterator it = recorder.decls.find(path);
    if (it == recorder.decls.end()) fail("missing alias path: " + path);
    if (it->second.storage_id == 0 || it->second.storage_id == it->second.track_id) {
        fail("path is not backed by separate physical storage: " + path);
    }
}

static void init_slot(ComplexSlot& slot, std::uint32_t i) {
    slot.id = 1000u + i;
    slot.a = static_cast<std::uint16_t>(10u + i);
    slot.b = static_cast<std::uint16_t>(20u + i);
    slot.flags.mode = i & 7u;
    slot.flags.ready = i & 1u;
    slot.flags.code = i & 15u;
    slot.flags.page = i + 1u;
    slot.counter.raw_unsafe_for_initialization_only() = 2000u + i;
}

int main(int argc, char** argv) {
    const char* out_path = (argc > 1) ? argv[1] : "complex_class_wvz4_smoke.wvz4";

    wave::ensure_dynamic_type_registered<ComplexPayload>();
    wave::ensure_dynamic_type_registered<ComplexDynamic>();

    ComplexTop top = {};
    top.epoch = 1u;
    top.flags.mode = 1u;
    top.flags.ready = 1u;
    top.flags.code = 2u;
    top.flags.page = 0x12u;
    top.alias.raw = 0x01020304u;
    top.global_dirty.raw_unsafe_for_initialization_only() = 0x1000u;
    top.sampled.payload.count = 10u;
    top.sampled.payload.delta = -10;
    top.sampled.payload.flags.mode = 2u;
    top.dynamic.payload.count = 20u;
    top.dynamic.payload.delta = -20;
    top.dynamic.payload.flags.code = 3u;
    for (std::uint32_t i = 0; i < top.slots.size(); ++i) init_slot(top.slots[i], i);

    std::string error;
    ComplexRecorder recorder;
    PathStableWvz4Recorder::OpenConfig config;
    config.file_path = out_path;
    config.emit_default_clk = false;
    config.clk_period_ticks = 10;
    config.options.compression = wvz4::Compression::None;
    config.options.enable_stats_log = false;
    config.options.target_block_span = 256;
    if (!recorder.open(config, error)) fail("open failed: " + error);

    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.enable_flat_leaf_fast_table = true;
    options.enable_flat_memory_block_precheck = true;
    options.enable_wave_array_dirty = true;
    options.enable_wave_array_memory_block_precheck = true;
    options.enable_wave_array_memory_block_byte_map = true;
    options.enable_wave_array_parallel_sampling = true;
    options.wave_array_parallel_threshold = 1u;
    options.enable_dirty_peek_groups = true;
    options.enable_dynamic_dirty_groups = true;
    options.enable_dirty_peek_memory_block_precheck = true;
    options.enable_dirty_peek_memory_block_byte_map = true;
    options.dirty_peek_parallel_threshold = 1u;
    options.enable_wave_value_dirty = true;
    options.enable_union_fields = true;
    options.enable_bitfield_fields = true;
    options.enable_parallel_sampling = true;
    options.sampling_threads = 4u;

    wave::Tracer tracer(recorder, options);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) fail("cycle0 sample failed: " + tap.last_error());

    top.epoch = 2u;
    top.flags.mode = 5u;
    top.flags.ready = 0u;
    top.flags.code = 9u;
    top.flags.page = 0x5au;
    top.alias.raw = 0x1234abcdu;
    top.slots[3].id = 0x11112222u;
    top.slots[3].counter = 33u;
    top.global_dirty = 0x5555u;
    top.sampled.write(111u, -111);
    top.dynamic.write(222u, -222);
    if (!tap.sample_one_cycle()) fail("cycle1 sample failed: " + tap.last_error());

    if (!tap.sample_one_cycle()) fail("cycle2 sample failed: " + tap.last_error());

    ComplexSlot* raw_slots = top.slots.data();
    raw_slots[7].a = 0x7777u;
    raw_slots[7].flags.page = 0x77u;
    top.sampled.write(333u, -333);
    if (!tap.sample_one_cycle()) fail("cycle3 sample failed: " + tap.last_error());

    top.alias.words.hi = 0xfeedu;
    top.dynamic.write(444u, -444);
    if (!tap.sample_one_cycle()) fail("cycle4 sample failed: " + tap.last_error());

    if (!recorder.close(error)) fail("close failed: " + error);

    expect_path(recorder, "top.epoch");
    expect_path(recorder, "top.slots.[3].id");
    expect_path(recorder, "top.slots.[7].a");
    expect_path(recorder, "top.global_dirty");
    expect_path(recorder, "top.sampled.count");
    expect_path(recorder, "top.dynamic.payload.count");
    expect_alias(recorder, "top.flags.mode");
    expect_alias(recorder, "top.flags.page");
    expect_alias(recorder, "top.alias.lo");
    expect_alias(recorder, "top.alias.hi");

    std::cout << "complex_class_wvz4_writer_ok file=" << out_path
              << " declared_paths=" << recorder.decls.size() << "\n";
    return 0;
}
