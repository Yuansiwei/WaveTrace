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

    PeekSmokeSource peek_hook_source;
    if (!peek_hook_source.wave_trace_peek_dirty_hook() ||
        peek_hook_source.wave_trace_peek_dirty_hook() != peek_hook_source.wave_dirty_hook()) {
        std::cerr << "missing peek dirty hook\n";
        return 3;
    }

    int peek_dirty_count = 0;
    peek_hook_source.wave_dirty_hook()->bind(&peek_dirty_count, 42, &count_dirty_mark);
    peek_hook_source.wave_dirty_hook()->mark_dirty();
    if (peek_dirty_count != 1) {
        std::cerr << "peek dirty hook did not fire\n";
        return 4;
    }
    PeekSmokeSource copied_peek_source = peek_hook_source;
    copied_peek_source.wave_dirty_hook()->mark_dirty();
    if (peek_dirty_count != 1) {
        std::cerr << "copied peek dirty hook kept binding\n";
        return 5;
    }

    DynamicSmokeTarget dynamic_target;
    dynamic_target.count = 3;
    if (!dynamic_target.wave_trace_dirty_hook() ||
        dynamic_target.wave_trace_dirty_hook() != dynamic_target.wave_dirty_hook()) {
        std::cerr << "missing dynamic dirty hook\n";
        return 6;
    }

    int dirty_count = 0;
    dynamic_target.wave_dirty_hook()->bind(&dirty_count, 42, &count_dirty_mark);
    dynamic_target.wave_dirty_hook()->mark_dirty();
    if (dirty_count != 1) {
        std::cerr << "dynamic dirty hook did not fire\n";
        return 7;
    }
    DynamicSmokeTarget copied_target = dynamic_target;
    copied_target.wave_dirty_hook()->mark_dirty();
    if (dirty_count != 1) {
        std::cerr << "copied dynamic dirty hook kept binding\n";
        return 8;
    }

    return 0;
}
