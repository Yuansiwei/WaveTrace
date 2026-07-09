#include "wave_tap.h"

#include <cstdint>
#include <iostream>
#include <set>
#include <string>

struct DirtyHookPayload {
    bool flag;
    std::uint32_t count;
    int delta;
};

struct DirtyPeekChannel
    : wave::PeekTraceSourceFor<DirtyPeekChannel, DirtyHookPayload> {
    DirtyHookPayload value;

    const DirtyHookPayload* peek() const {
        return &value;
    }

    void write(bool flag, std::uint32_t count, int delta) {
        value.flag = flag;
        value.count = count;
        value.delta = delta;
        wave_dirty_hook()->mark_dirty();
    }
};

struct DirtyDynamicChannel
    : wave::DynamicTraceTargetFor<DirtyDynamicChannel> {
    DirtyHookPayload value;

    void write(bool flag, std::uint32_t count, int delta) {
        value.flag = flag;
        value.count = count;
        value.delta = delta;
        wave_dirty_hook()->mark_dirty();
    }
};

struct DirtyHookTop {
    DirtyPeekChannel peek;
    DirtyDynamicChannel dynamic;
};

namespace reflect {
template<> struct is_reflected<DirtyHookPayload> : std::true_type {};
template<> struct reflected_visitor<DirtyHookPayload> {
    template<class P, class V, class G>
    static void visit(const DirtyHookPayload* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("flag", std::addressof(obj->flag));
        on_ptr("count", std::addressof(obj->count));
        on_ptr("delta", std::addressof(obj->delta));
    }
};

template<> struct is_reflected<DirtyDynamicChannel> : std::true_type {};
template<> struct reflected_visitor<DirtyDynamicChannel> {
    template<class P, class V, class G>
    static void visit(const DirtyDynamicChannel* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(obj->value));
    }
};

template<> struct is_reflected<DirtyHookTop> : std::true_type {};
template<> struct reflected_visitor<DirtyHookTop> {
    template<class P, class V, class G>
    static void visit(const DirtyHookTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("peek", std::addressof(obj->peek));
        on_ptr("dynamic", std::addressof(obj->dynamic));
    }
};
}

class DirtyHookPathRecorder : public PathStableWvz4Recorder {
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

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main(int argc, char** argv) {
    const std::string out_path =
        (argc >= 2 && argv[1] && argv[1][0] != '\0')
            ? argv[1]
            : "build_vs\\dirty_peek_dynamic_wvz4_smoke.wvz4";

    DirtyHookTop top;
    top.peek.value.flag = false;
    top.peek.value.count = 100u;
    top.peek.value.delta = -10;
    top.dynamic.value.flag = false;
    top.dynamic.value.count = 200u;
    top.dynamic.value.delta = -20;

    std::string error;
    DirtyHookPathRecorder recorder;
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

    wave::ensure_dynamic_type_registered<DirtyHookPayload>();
    wave::ensure_dynamic_type_registered<DirtyDynamicChannel>();

    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_dirty_peek_groups = true;
    opt.enable_dirty_peek_memory_block_precheck = true;
    opt.enable_dirty_peek_memory_block_byte_map = true;
    opt.dirty_peek_parallel_threshold = 1;
    opt.enable_parallel_sampling = true;
    opt.sampling_threads = 4;
    opt.debug_log_duplicate_sample_stack = true;
    opt.debug_log_path = "build_vs\\dirty_peek_dynamic_duplicate_stack.log";
    opt.debug_log_max_events = 10000;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) return fail("cycle0 sample failed: " + tap.last_error());

    if (recorder.declared_paths.find("top.peek.count") == recorder.declared_paths.end()) {
        return fail("missing declared path top.peek.count");
    }
    if (recorder.declared_paths.find("top.dynamic.value.count") == recorder.declared_paths.end()) {
        return fail("missing declared path top.dynamic.value.count");
    }

    top.peek.write(true, 111u, -11);
    if (!tap.sample_one_cycle()) return fail("cycle1 sample failed: " + tap.last_error());

    if (!tap.sample_one_cycle()) return fail("cycle2 sample failed: " + tap.last_error());

    top.dynamic.write(true, 222u, -22);
    if (!tap.sample_one_cycle()) return fail("cycle3 sample failed: " + tap.last_error());

    if (!recorder.close(error)) {
        return fail("recorder.close failed: " + error);
    }

    std::cout << "dirty_peek_dynamic_wvz4_writer_ok file=" << out_path
              << " tracks=" << recorder.declared_paths.size()
              << " cycles=4\n";
    return 0;
}
