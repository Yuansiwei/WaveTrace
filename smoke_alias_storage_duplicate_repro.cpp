#include "wave_tap.h"

#include <cstdint>
#include <iostream>
#include <string>

struct AliasBits {
    std::uint32_t raw;

    static std::uint32_t low_a(const AliasBits* p) {
        return p ? (p->raw & 0xffu) : 0u;
    }

    static std::uint32_t low_b(const AliasBits* p) {
        return p ? (p->raw & 0xffu) : 0u;
    }
};

struct AliasStorageTop {
    AliasBits single;
    wave::array<AliasBits, 4> arr;
};

namespace reflect {
template<> struct is_reflected<AliasBits> : std::true_type {};
template<> struct reflected_visitor<AliasBits> {
    template<class P, class V, class G>
    static void visit(const AliasBits* obj, P&&, V&&, G&& on_getter) {
        (void)obj;
        on_getter("low_a", &AliasBits::low_a, 8u, 0ll);
        on_getter("low_b", &AliasBits::low_b, 8u, 0ll);
    }
};

template<> struct is_reflected<AliasStorageTop> : std::true_type {};
template<> struct reflected_visitor<AliasStorageTop> {
    template<class P, class V, class G>
    static void visit(const AliasStorageTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("single", std::addressof(obj->single));
        on_ptr("arr", std::addressof(obj->arr));
    }
};
}

static AliasStorageTop g_top;

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main(int argc, char** argv) {
    bool emit_only_on_change = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--emit-only-on-change") emit_only_on_change = true;
        if (arg == "--emit-all") emit_only_on_change = false;
    }

    g_top.single.raw = 0x12345678u;
    for (std::size_t i = 0; i < g_top.arr.size(); ++i) {
        g_top.arr[i].raw = 0xa5000000u | static_cast<std::uint32_t>(i);
    }

    std::string error;
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = "build_vs\\alias_storage_duplicate_repro.wvz4";
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    if (!recorder.open(cfg, error)) {
        return fail("recorder.open failed: " + error);
    }

    wave::BuildOptions opt;
    opt.emit_only_on_change = emit_only_on_change;
    opt.emit_track_decl_path = true;
    opt.enable_bitfield_fields = true;
    opt.enable_union_fields = true;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_wave_array_memory_block_precheck = true;
    opt.enable_wave_array_memory_block_byte_map = true;
    opt.enable_parallel_sampling = false;
    opt.dump_leaf_distribution_after_topology = false;

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &g_top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) {
        return fail(std::string("cycle0_failed: ") + tap.last_error());
    }

    std::size_t owners = 0;
    std::size_t aliases = 0;
    std::size_t declaration_aliases = 0;
    const std::vector<wave::TrackDesc>& tracks = tracer.tracks();
    for (std::size_t i = 1; i < tracks.size(); ++i) {
        const wave::TrackDesc& t = tracks[i];
        if (t.id == 0) continue;
        if (t.storage_id == t.id) {
            ++owners;
        } else if (t.storage_id != 0) {
            ++aliases;
            if (t.declaration_only) ++declaration_aliases;
        }
    }

    if (!recorder.close(error)) {
        return fail("recorder.close failed: " + error);
    }

    std::cout << "alias_storage_duplicate_repro_ok"
              << " emit_only_on_change=" << (emit_only_on_change ? 1 : 0)
              << " owners=" << owners
              << " aliases=" << aliases
              << " declaration_aliases=" << declaration_aliases
              << "\n";
    return 0;
}
