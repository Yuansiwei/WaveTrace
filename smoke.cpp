#include "wave_tap.h"
#include "wave_runtime.h"
#include "wave_path_wvz4_recorder.h"
#include <iostream>

struct PeekSmokeValue {
    unsigned char count;
};

struct PeekSmokeSource : wave::PeekTraceSourceFor<PeekSmokeSource, PeekSmokeValue> {
    PeekSmokeValue value;

    const PeekSmokeValue* peek() {
        return &value;
    }
};

struct DynamicSmokeTarget : wave::DynamicTraceTargetFor<DynamicSmokeTarget> {
    unsigned char count;
};

static void count_dirty_mark(void* ctx, std::uint32_t group_id) {
    if (group_id == 42) {
        ++(*static_cast<int*>(ctx));
    }
}

namespace reflect {
template<> struct is_reflected<PeekSmokeValue> : std::true_type {};
template<> struct reflected_visitor<PeekSmokeValue> {
    template<class P, class V, class G>
    static void visit(const PeekSmokeValue* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("count", std::addressof(obj->count));
    }
};

template<> struct is_reflected<DynamicSmokeTarget> : std::true_type {};
template<> struct reflected_visitor<DynamicSmokeTarget> {
    template<class P, class V, class G>
    static void visit(const DynamicSmokeTarget* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("count", std::addressof(obj->count));
    }
};
}

int main(){
    PeekSmokeSource source;
    source.value.count = 1;

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
    wave::ensure_dynamic_type_registered<PeekSmokeValue>();
    wave::Tracer tracer(sink, opt);
    tracer.add_root("peek", &source);
    tracer.sample(0);

    source.value.count = 9;
    tracer.sample(1);

    bool saw_count_decl = false;
    for (const auto& decl : sink.declarations) {
        if (decl.path == "peek.count" &&
            decl.kind == wave::ValueKind::UnsignedInt &&
            decl.bit_width == 8) {
            saw_count_decl = true;
        }
    }
    if (!saw_count_decl) {
        std::cerr << "missing peek.count declaration\n";
        std::cerr << "nodes=" << tracer.nodes().size()
                  << " tracks=" << tracer.tracks().size()
                  << " roots=" << tracer.root_watch_count()
                  << " expanded_roots=" << tracer.expanded_root_watch_count()
                  << "\n";
        for (const auto& decl : sink.declarations) {
            std::cerr << "declared path=" << decl.path << "\n";
        }
        return 1;
    }

    bool saw_updated_value = false;
    for (const auto& ev : sink.events) {
        if (ev.has_u64 && ev.u64_value == 9) {
            saw_updated_value = true;
        }
    }
    if (!saw_updated_value) {
        std::cerr << "missing peek updated value\n";
        return 2;
    }

    {
        DynamicSmokeTarget flat_dynamic;
        flat_dynamic.count = 3;
        wave::InMemoryWaveSink dynamic_sink;
        wave::BuildOptions dynamic_opt;
        dynamic_opt.emit_track_decl_path = true;
        dynamic_opt.enable_dirty_peek_groups = true;
        // Explicit compatibility opt-out: runtime type recovery remains
        // available when a legacy Dynamic target still needs flat polling.
        dynamic_opt.enable_dynamic_dirty_groups = false;
        wave::Tracer dynamic_tracer(dynamic_sink, dynamic_opt);
        dynamic_tracer.add_root("dynamic_flat", &flat_dynamic);
        dynamic_tracer.sample(0);

        bool flat_track_is_not_dirty_grouped = false;
        for (const auto& track : dynamic_tracer.tracks()) {
            if (track.scalar_kind != wave::ScalarSampleKind::None &&
                track.dirty_peek_group_id == wave::kInvalidIndex) {
                flat_track_is_not_dirty_grouped = true;
            }
        }
        if (!flat_track_is_not_dirty_grouped) {
            std::cerr << "explicit-flat dynamic target unexpectedly created dirty group\n";
            return 3;
        }

        flat_dynamic.count = 9;
        // The explicit-flat Dynamic hook is deliberately unbound. Calling it
        // remains harmless; ordinary polling must still observe the update.
        flat_dynamic.wave_dirty_hook()->mark_dirty();
        dynamic_tracer.sample(1);
        bool saw_flat_dynamic_update = false;
        for (const auto& ev : dynamic_sink.events) {
            if (ev.has_u64 && ev.u64_value == 9) {
                saw_flat_dynamic_update = true;
            }
        }
        if (!saw_flat_dynamic_update) {
            std::cerr << "explicit-flat dynamic target missed updated value\n";
            return 4;
        }
    }

    {
        DynamicSmokeTarget dirty_dynamic;
        dirty_dynamic.count = 5;
        wave::InMemoryWaveSink dynamic_sink;
        wave::BuildOptions dynamic_opt;
        dynamic_opt.emit_track_decl_path = true;
        wave::Tracer dynamic_tracer(dynamic_sink, dynamic_opt);
        dynamic_tracer.add_root("dynamic_dirty", &dirty_dynamic);
        dynamic_tracer.attach_current_thread_for_dirty_peek();
        dynamic_tracer.sample(0);

        bool dirty_track_is_grouped = false;
        for (const auto& track : dynamic_tracer.tracks()) {
            if (track.scalar_kind != wave::ScalarSampleKind::None &&
                track.dirty_peek_group_id != wave::kInvalidIndex) {
                dirty_track_is_grouped = true;
            }
        }
        if (!dirty_track_is_grouped) {
            std::cerr << "default Dynamic target did not create dirty group\n";
            return 5;
        }

        dirty_dynamic.count = 11;
        // Dynamic is active-report only: changing storage without marking the
        // hook must not be rescued by the flat polling path.
        dynamic_tracer.sample(1);
        bool saw_unmarked_dynamic_update = false;
        for (const auto& ev : dynamic_sink.events) {
            if (ev.has_u64 && ev.u64_value == 11) {
                saw_unmarked_dynamic_update = true;
            }
        }
        if (saw_unmarked_dynamic_update) {
            std::cerr << "default dirty Dynamic target was still flat-polled\n";
            return 6;
        }

        dirty_dynamic.wave_dirty_hook()->mark_dirty();
        dynamic_tracer.sample(2);
        bool saw_dirty_dynamic_update = false;
        for (const auto& ev : dynamic_sink.events) {
            if (ev.has_u64 && ev.u64_value == 11) {
                saw_dirty_dynamic_update = true;
            }
        }
        if (!saw_dirty_dynamic_update) {
            std::cerr << "default dirty Dynamic target missed marked update\n";
            return 7;
        }
    }

    PeekSmokeSource peek_hook_source;
    if (!peek_hook_source.wave_trace_peek_dirty_hook() ||
        peek_hook_source.wave_trace_peek_dirty_hook() != peek_hook_source.wave_dirty_hook()) {
        std::cerr << "missing peek dirty hook\n";
        return 8;
    }

    int peek_dirty_count = 0;
    peek_hook_source.wave_dirty_hook()->bind(&peek_dirty_count, 42, &count_dirty_mark);
    peek_hook_source.wave_dirty_hook()->mark_dirty();
    if (peek_dirty_count != 1) {
        std::cerr << "peek dirty hook did not fire\n";
        return 9;
    }
    PeekSmokeSource copied_peek_source = peek_hook_source;
    copied_peek_source.wave_dirty_hook()->mark_dirty();
    if (peek_dirty_count != 1) {
        std::cerr << "copied peek dirty hook kept binding\n";
        return 10;
    }

    DynamicSmokeTarget dynamic_target;
    dynamic_target.count = 3;
    if (!dynamic_target.wave_trace_dirty_hook() ||
        dynamic_target.wave_trace_dirty_hook() != dynamic_target.wave_dirty_hook()) {
        std::cerr << "missing dynamic dirty hook\n";
        return 11;
    }

    int dirty_count = 0;
    dynamic_target.wave_dirty_hook()->bind(&dirty_count, 42, &count_dirty_mark);
    dynamic_target.wave_dirty_hook()->mark_dirty();
    if (dirty_count != 1) {
        std::cerr << "dynamic dirty hook did not fire\n";
        return 12;
    }
    DynamicSmokeTarget copied_target = dynamic_target;
    copied_target.wave_dirty_hook()->mark_dirty();
    if (dirty_count != 1) {
        std::cerr << "copied dynamic dirty hook kept binding\n";
        return 13;
    }

    return 0;
}
