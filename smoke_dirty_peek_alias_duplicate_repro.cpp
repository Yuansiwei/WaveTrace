#include "wave_tap.h"

#include <cstdint>
#include <iostream>
#include <string>

struct SharedPeekPayload {
    bool flag;
    std::uint32_t count;
    int delta;
};

struct SharedPeekSource
    : wave::PeekTraceSourceFor<SharedPeekSource, SharedPeekPayload> {
    SharedPeekPayload* value;

    SharedPeekSource() : value(NULL) {}

    const SharedPeekPayload* peek() const {
        return value;
    }

    void mark_dirty() {
        wave_dirty_hook()->mark_dirty();
    }
};

struct SharedPeekTop {
    SharedPeekPayload shared;
    SharedPeekSource a;
    SharedPeekSource b;

    SharedPeekTop() {
        shared.flag = false;
        shared.count = 100u;
        shared.delta = -7;
        a.value = &shared;
        b.value = &shared;
    }
};

namespace reflect {
template<> struct is_reflected<SharedPeekPayload> : std::true_type {};
template<> struct reflected_visitor<SharedPeekPayload> {
    template<class P, class V, class G>
    static void visit(const SharedPeekPayload* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("flag", std::addressof(obj->flag));
        on_ptr("count", std::addressof(obj->count));
        on_ptr("delta", std::addressof(obj->delta));
    }
};

template<> struct is_reflected<SharedPeekTop> : std::true_type {};
template<> struct reflected_visitor<SharedPeekTop> {
    template<class P, class V, class G>
    static void visit(const SharedPeekTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("a", std::addressof(obj->a));
        on_ptr("b", std::addressof(obj->b));
    }
};
}

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main(int argc, char** argv) {
    bool enable_dirty_peek = true;
    bool emit_only_on_change = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--no-dirty-peek") enable_dirty_peek = false;
        if (arg == "--dirty-peek") enable_dirty_peek = true;
        if (arg == "--emit-all") emit_only_on_change = false;
        if (arg == "--emit-only-on-change") emit_only_on_change = true;
    }

    SharedPeekTop top;

    std::string error;
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = "build_vs\\dirty_peek_alias_duplicate_repro.wvz4";
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 256;
    if (!recorder.open(cfg, error)) {
        return fail("recorder.open failed: " + error);
    }

    wave::ensure_dynamic_type_registered<SharedPeekPayload>();

    wave::BuildOptions opt;
    opt.emit_only_on_change = emit_only_on_change;
    opt.emit_track_decl_path = true;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_dirty_peek_groups = enable_dirty_peek;
    opt.enable_dirty_peek_memory_block_precheck = true;
    opt.enable_dirty_peek_memory_block_byte_map = true;
    opt.enable_parallel_sampling = false;
    opt.dump_leaf_distribution_after_topology = false;
    opt.debug_log_duplicate_sample_stack = true;
    opt.debug_log_path = "build_vs\\dirty_peek_alias_duplicate_stack.log";
    opt.debug_log_max_events = 10000;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) {
        return fail(std::string("cycle0_failed: ") + tap.last_error());
    }

    std::size_t owners = 0;
    std::size_t aliases = 0;
    std::size_t aliases_not_declaration_only = 0;
    const std::vector<wave::TrackDesc>& tracks = tracer.tracks();
    for (std::size_t i = 1; i < tracks.size(); ++i) {
        const wave::TrackDesc& t = tracks[i];
        if (t.id == 0) continue;
        if (t.storage_id == t.id) {
            ++owners;
        } else if (t.storage_id != 0) {
            ++aliases;
            if (!t.declaration_only) ++aliases_not_declaration_only;
        }
    }

    if (!recorder.close(error)) {
        return fail("recorder.close failed: " + error);
    }

    std::cout << "dirty_peek_alias_duplicate_repro_ok"
              << " dirty_peek=" << (enable_dirty_peek ? 1 : 0)
              << " emit_only_on_change=" << (emit_only_on_change ? 1 : 0)
              << " owners=" << owners
              << " aliases=" << aliases
              << " aliases_not_declaration_only=" << aliases_not_declaration_only
              << "\n";
    return 0;
}
