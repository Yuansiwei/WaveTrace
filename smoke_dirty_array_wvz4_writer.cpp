#include "wave_tap.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

struct DirtyArrayWvz4Slot {
    std::uint32_t f0;
    std::uint32_t f1;
    std::uint32_t f2;
    std::uint32_t f3;
    std::uint32_t f4;
    std::uint32_t f5;
    std::uint32_t f6;
    std::uint32_t f7;
    std::uint32_t f8;
    std::uint32_t f9;
    std::uint32_t f10;
    std::uint32_t f11;
    std::uint32_t f12;
    std::uint32_t f13;
    std::uint32_t f14;
    std::uint32_t f15;
};

struct DirtyArrayWvz4Top {
    wave::array<DirtyArrayWvz4Slot, 128> slots;
};

namespace reflect {
template<> struct is_reflected<DirtyArrayWvz4Slot> : std::true_type {};
template<> struct reflected_visitor<DirtyArrayWvz4Slot> {
    template<class P, class V, class G>
    static void visit(const DirtyArrayWvz4Slot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("f0", std::addressof(obj->f0));
        on_ptr("f1", std::addressof(obj->f1));
        on_ptr("f2", std::addressof(obj->f2));
        on_ptr("f3", std::addressof(obj->f3));
        on_ptr("f4", std::addressof(obj->f4));
        on_ptr("f5", std::addressof(obj->f5));
        on_ptr("f6", std::addressof(obj->f6));
        on_ptr("f7", std::addressof(obj->f7));
        on_ptr("f8", std::addressof(obj->f8));
        on_ptr("f9", std::addressof(obj->f9));
        on_ptr("f10", std::addressof(obj->f10));
        on_ptr("f11", std::addressof(obj->f11));
        on_ptr("f12", std::addressof(obj->f12));
        on_ptr("f13", std::addressof(obj->f13));
        on_ptr("f14", std::addressof(obj->f14));
        on_ptr("f15", std::addressof(obj->f15));
    }
};

template<> struct is_reflected<DirtyArrayWvz4Top> : std::true_type {};
template<> struct reflected_visitor<DirtyArrayWvz4Top> {
    template<class P, class V, class G>
    static void visit(const DirtyArrayWvz4Top* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("slots", std::addressof(obj->slots));
    }
};
}

class DirtyArrayPathRecorder : public PathStableWvz4Recorder {
public:
    std::set<std::string> declared_paths;

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        declared_paths.insert(path);
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

static void init_top(DirtyArrayWvz4Top& top) {
    for (std::size_t i = 0; i < top.slots.size(); ++i) {
        DirtyArrayWvz4Slot& s = top.slots[i];
        const std::uint32_t base = static_cast<std::uint32_t>(i * 1000u);
        s.f0 = base + 0u;
        s.f1 = base + 1u;
        s.f2 = base + 2u;
        s.f3 = base + 3u;
        s.f4 = base + 4u;
        s.f5 = base + 5u;
        s.f6 = base + 6u;
        s.f7 = base + 7u;
        s.f8 = base + 8u;
        s.f9 = base + 9u;
        s.f10 = base + 10u;
        s.f11 = base + 11u;
        s.f12 = base + 12u;
        s.f13 = base + 13u;
        s.f14 = base + 14u;
        s.f15 = base + 15u;
    }
}

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main(int argc, char** argv) {
    const std::string out_path =
        (argc >= 2 && argv[1] && argv[1][0] != '\0')
            ? argv[1]
            : "build_vs\\dirty_array_wvz4_smoke.wvz4";

    DirtyArrayWvz4Top top;
    init_top(top);

    std::string error;
    DirtyArrayPathRecorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 256;

    if (!recorder.open(cfg, error)) {
        return fail("recorder.open failed: " + error);
    }

    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_wave_array_memory_block_precheck = true;
    opt.enable_wave_array_memory_block_byte_map = true;
    opt.enable_parallel_sampling = true;
    opt.enable_wave_array_parallel_sampling = true;
    opt.wave_array_parallel_threshold = 1;
    opt.sampling_threads = 4;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) return fail("cycle0 sample failed: " + tap.last_error());

    if (recorder.declared_paths.find("top.slots.[7].f3") == recorder.declared_paths.end()) {
        return fail("missing declared path top.slots.[7].f3");
    }
    if (recorder.declared_paths.find("top.slots.[9].f4") == recorder.declared_paths.end()) {
        return fail("missing declared path top.slots.[9].f4");
    }

    top.slots[7].f3 = 0x12345678u;
    if (!tap.sample_one_cycle()) return fail("cycle1 sample failed: " + tap.last_error());

    if (!tap.sample_one_cycle()) return fail("cycle2 sample failed: " + tap.last_error());

    DirtyArrayWvz4Slot* raw = top.slots.data();
    raw[9].f4 = 0x87654321u;
    (void)top.slots.data();
    if (!tap.sample_one_cycle()) return fail("cycle3 sample failed: " + tap.last_error());

    if (!recorder.close(error)) {
        return fail("recorder.close failed: " + error);
    }

    std::cout << "dirty_array_wvz4_writer_ok file=" << out_path
              << " tracks=" << recorder.declared_paths.size()
              << " cycles=4\n";
    return 0;
}
