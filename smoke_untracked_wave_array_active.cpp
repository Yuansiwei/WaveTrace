#include "wave_tap.h"

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>

struct UntrackedActiveSlot {
    std::uint32_t a;
    std::uint32_t b;
};

struct UntrackedActiveTop {
    wave::array<UntrackedActiveSlot, 4> slots;
};

namespace reflect {
template<> struct is_reflected<UntrackedActiveSlot> : std::true_type {};
template<> struct reflected_visitor<UntrackedActiveSlot> {
    template<class P, class V, class G>
    static void visit(const UntrackedActiveSlot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("a", std::addressof(obj->a));
        on_ptr("b", std::addressof(obj->b));
    }
};

template<> struct is_reflected<UntrackedActiveTop> : std::true_type {};
template<> struct reflected_visitor<UntrackedActiveTop> {
    template<class P, class V, class G>
    static void visit(const UntrackedActiveTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("slots", std::addressof(obj->slots));
    }
};
}

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main() {
    std::remove("wave_runtime_error.log");

    UntrackedActiveTop top;
    for (std::size_t i = 0; i < top.slots.size(); ++i) {
        top.slots[i].a = static_cast<std::uint32_t>(i);
        top.slots[i].b = static_cast<std::uint32_t>(i + 100);
    }

    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = "build_vs\\untracked_wave_array_active.wvz4";
    cfg.emit_default_clk = false;
    cfg.options.compression = wvz4::Compression::None;

    std::string error;
    if (!recorder.open(cfg, error)) return fail("open failed: " + error);

    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_wave_array_memory_block_precheck = true;
    opt.enable_wave_array_memory_block_byte_map = true;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) return fail("cycle0 failed: " + tap.last_error());

    wave::array<std::uint32_t, 256>* scratch = new wave::array<std::uint32_t, 256>();
    std::uint32_t* raw = scratch->data();
    raw[0] = 123u;
    scratch->fill(7u);
    delete scratch;

    top.slots[1].a = 456u;
    if (!tap.sample_one_cycle()) return fail("cycle1 failed: " + tap.last_error());
    if (!recorder.close(error)) return fail("close failed: " + error);

    if (FILE* fp = std::fopen("wave_runtime_error.log", "rb")) {
        std::fclose(fp);
        return fail("unexpected wave_runtime_error.log for untracked active wave::array");
    }

    std::cout << "untracked_wave_array_active_ok\n";
    return 0;
}
