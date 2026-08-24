#include "wave_runtime.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <cstring>
#include <string>

struct DirtyArrayBlockSlot {
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

struct DirtyArrayBlockTop {
    wave::array<DirtyArrayBlockSlot, 128> slots;
};

namespace reflect {
template<> struct is_reflected<DirtyArrayBlockSlot> : std::true_type {};
template<> struct reflected_visitor<DirtyArrayBlockSlot> {
    template<class P, class V, class G>
    static void visit(const DirtyArrayBlockSlot* obj, P&& on_ptr, V&&, G&&) {
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

template<> struct is_reflected<DirtyArrayBlockTop> : std::true_type {};
template<> struct reflected_visitor<DirtyArrayBlockTop> {
    template<class P, class V, class G>
    static void visit(const DirtyArrayBlockTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("slots", std::addressof(obj->slots));
    }
};
}

static void init_top(DirtyArrayBlockTop& top) {
    for (std::size_t i = 0; i < top.slots.size(); ++i) {
        DirtyArrayBlockSlot& s = top.slots[i];
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

static bool patch_value_at(const wave::InMemoryWaveSink& sink,
                           wave::Cycle cycle,
                           std::uint64_t byte_offset,
                           std::uint32_t expected) {
    for (std::size_t i = sink.array_patches.size(); i != 0; --i) {
        const wave::InMemoryWaveSink::OwnedArrayPatch& patch = sink.array_patches[i - 1u];
        if (patch.cycle > cycle || byte_offset < patch.byte_offset) continue;
        const std::uint64_t local = byte_offset - patch.byte_offset;
        if (local + sizeof(expected) > patch.data.size()) continue;
        std::uint32_t actual = 0;
        std::memcpy(&actual, patch.data.data() + static_cast<std::size_t>(local), sizeof(actual));
        return actual == expected;
    }
    return false;
}

static int fail(const char* msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main(int argc, char** argv) {
    const unsigned requested_threads = (argc > 1 && argv[1]) ? static_cast<unsigned>(std::strtoul(argv[1], NULL, 10)) : 4u;
    DirtyArrayBlockTop top;
    init_top(top);

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
    opt.enable_flat_leaf_fast_table = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_wave_array_memory_block_precheck = true;
    opt.enable_parallel_sampling = true;
    opt.enable_wave_array_parallel_sampling = true;
    opt.wave_array_parallel_threshold = 1;
    opt.sampling_threads = requested_threads;
    opt.debug_log_dirty_wave_array_stats = true;

    wave::Tracer tracer(sink, opt);
    tracer.add_root("top", &top);
    tracer.sample(0);

    if (!sink.declarations.empty() || sink.array_block_declarations.size() != 1u) {
        return fail("wave::array did not use one compact block");
    }
    const wave::ArrayBlockDecl& decl = sink.array_block_declarations[0];
    if (decl.element_count != top.slots.size() ||
        decl.element_stride != sizeof(DirtyArrayBlockSlot) ||
        decl.schema.size() != 17u) {
        return fail("compact array declaration shape mismatch");
    }
    const std::uint64_t f3_offset = 7u * sizeof(DirtyArrayBlockSlot) + offsetof(DirtyArrayBlockSlot, f3);
    const std::uint64_t f4_offset = 9u * sizeof(DirtyArrayBlockSlot) + offsetof(DirtyArrayBlockSlot, f4);
    if (!patch_value_at(sink, 0, f3_offset, 7003u) ||
        !patch_value_at(sink, 0, f4_offset, 9004u)) {
        return fail("initial compact array patch missing");
    }

    const std::size_t after_initial = sink.array_patches.size();
    top.slots[7].f3 = 0x12345678u;
    tracer.sample(1);
    const std::size_t after_one_dirty = sink.array_patches.size();
    if (after_one_dirty <= after_initial) return fail("single element dirty emitted no patch");
    if (!patch_value_at(sink, 1, f3_offset, 0x12345678u)) return fail("single element dirty value missing");

    tracer.sample(2);
    if (sink.array_patches.size() != after_one_dirty) return fail("no-dirty cycle emitted compact array patches");

    DirtyArrayBlockSlot* raw = top.slots.data();
    raw[9].f4 = 0x87654321u;
    (void)top.slots.data();
    tracer.sample(3);
    if (!patch_value_at(sink, 3, f4_offset, 0x87654321u)) return fail("bulk data dirty value missing");

    std::cout << "dirty array block smoke passed: threads=" << requested_threads
              << " arrays=" << sink.array_block_declarations.size()
              << " patches=" << sink.array_patches.size() << "\n";
    return 0;
}
