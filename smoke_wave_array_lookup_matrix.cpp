#include "wave_runtime.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

struct MatrixSlot {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;
    std::uint32_t d;
};

struct MatrixTop {
    wave::array<std::uint32_t, 8> scalars;
    wave::array<MatrixSlot, 8> slots;
    wave::array<wave::array<std::uint32_t, 4>, 3> nested;
};

namespace reflect {
template<> struct is_reflected<MatrixSlot> : std::true_type {};
template<> struct reflected_visitor<MatrixSlot> {
    template<class P, class V, class G>
    static void visit(const MatrixSlot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("a", std::addressof(obj->a));
        on_ptr("b", std::addressof(obj->b));
        on_ptr("c", std::addressof(obj->c));
        on_ptr("d", std::addressof(obj->d));
    }
};

template<> struct is_reflected<MatrixTop> : std::true_type {};
template<> struct reflected_visitor<MatrixTop> {
    template<class P, class V, class G>
    static void visit(const MatrixTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("scalars", std::addressof(obj->scalars));
        on_ptr("slots", std::addressof(obj->slots));
        on_ptr("nested", std::addressof(obj->nested));
    }
};
}

static int fail(const char* msg) {
    std::cerr << msg << "\n";
    return 1;
}

static void init_top(MatrixTop& top) {
    for (std::size_t i = 0; i < top.scalars.size(); ++i) {
        top.scalars[i] = static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < top.slots.size(); ++i) {
        top.slots[i].a = static_cast<std::uint32_t>(100 + i * 10 + 0);
        top.slots[i].b = static_cast<std::uint32_t>(100 + i * 10 + 1);
        top.slots[i].c = static_cast<std::uint32_t>(100 + i * 10 + 2);
        top.slots[i].d = static_cast<std::uint32_t>(100 + i * 10 + 3);
    }
    for (std::size_t r = 0; r < top.nested.size(); ++r) {
        for (std::size_t c = 0; c < top.nested[r].size(); ++c) {
            top.nested[r][c] = static_cast<std::uint32_t>(1000 + r * 10 + c);
        }
    }
}

static const wave::ArrayBlockDecl* find_block(const wave::InMemoryWaveSink& sink,
                                              std::uint64_t count,
                                              std::uint64_t stride) {
    for (std::size_t i = 0; i < sink.array_block_declarations.size(); ++i) {
        const wave::ArrayBlockDecl& decl = sink.array_block_declarations[i];
        if (decl.element_count == count && decl.element_stride == stride) return &decl;
    }
    return NULL;
}

static bool patch_value_at(const wave::InMemoryWaveSink& sink,
                           wave::NodeId node_id,
                           wave::Cycle cycle,
                           std::uint64_t byte_offset,
                           std::uint32_t expected) {
    unsigned char bytes[sizeof(expected)] = {};
    for (std::size_t byte = 0; byte < sizeof(expected); ++byte) {
        const std::uint64_t target = byte_offset + byte;
        bool found = false;
        for (std::size_t i = sink.array_patches.size(); i != 0; --i) {
            const wave::InMemoryWaveSink::OwnedArrayPatch& patch = sink.array_patches[i - 1u];
            if (patch.node_id != node_id || patch.cycle > cycle || target < patch.byte_offset) continue;
            const std::uint64_t local = target - patch.byte_offset;
            if (local >= patch.data.size()) continue;
            bytes[byte] = patch.data[static_cast<std::size_t>(local)];
            found = true;
            break;
        }
        if (!found) return false;
    }
    std::uint32_t actual = 0;
    std::memcpy(&actual, bytes, sizeof(actual));
    return actual == expected;
}

static void dump_patches(const wave::InMemoryWaveSink& sink) {
    for (std::size_t i = 0; i < sink.array_patches.size(); ++i) {
        const wave::InMemoryWaveSink::OwnedArrayPatch& patch = sink.array_patches[i];
        std::cerr << "patch[" << i << "] node=" << patch.node_id
                  << " cycle=" << patch.cycle
                  << " offset=" << patch.byte_offset
                  << " bytes=" << patch.data.size() << "\n";
    }
}

static bool error_log_exists() {
    FILE* fp = std::fopen("wave_runtime_error.log", "rb");
    if (!fp) return false;
    std::fclose(fp);
    return true;
}

int main() {
    std::remove("wave_runtime_error.log");

    // No active tracer mapping yet: these must be ignored without a log.
    wave::array<std::uint32_t, 4> pretrace;
    pretrace.fill(1u);
    pretrace.data()[2] = 7u;
    (void)&pretrace;
    if (error_log_exists()) return fail("pretrace wave::array produced error log");

    MatrixTop top;
    init_top(top);

    wave::InMemoryWaveSink sink;
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

    wave::Tracer tracer(sink, opt);
    tracer.add_root("top", &top);
    tracer.sample(0);
    const wave::ArrayBlockDecl* scalars = find_block(sink, 8u, sizeof(std::uint32_t));
    const wave::ArrayBlockDecl* slots = find_block(sink, 8u, sizeof(MatrixSlot));
    const wave::ArrayBlockDecl* nested = find_block(sink, 3u, sizeof(wave::array<std::uint32_t, 4>));
    if (!sink.declarations.empty() || sink.array_block_declarations.size() != 3u ||
        !scalars || !slots || !nested) {
        return fail("compact lookup matrix declaration mismatch");
    }

    // Same-thread slice lookup through operator[].
    top.scalars[3] = 303u;
    top.slots[2].c = 202u;
    tracer.sample(1);
    if (!patch_value_at(sink, scalars->node_id, 1, 3u * sizeof(std::uint32_t), 303u)) {
        dump_patches(sink);
        return fail("same-thread scalar slice missing");
    }
    if (!patch_value_at(sink, slots->node_id, 1, 2u * sizeof(MatrixSlot) + offsetof(MatrixSlot, c), 202u)) return fail("same-thread struct slice missing");

    // Same-thread registered bulk lookups: data(), fill(), std::array assignment, operator&().
    std::uint32_t* scalar_data = top.scalars.data();
    scalar_data[5] = 505u;
    top.nested[1].fill(900u);
    std::array<std::uint32_t, 8> replacement = {{11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u}};
    top.scalars = replacement;
    (void)&top.slots;
    tracer.sample(2);
    if (!patch_value_at(sink, scalars->node_id, 2, 5u * sizeof(std::uint32_t), 16u)) return fail("same-thread bulk assignment missing");
    if (!patch_value_at(sink, nested->node_id, 2, sizeof(wave::array<std::uint32_t, 4>) + 2u * sizeof(std::uint32_t), 900u)) return fail("same-thread nested bulk fill missing");

    // Cross-thread no-TLS path: active-tracer explicit bulk/slice maps must resolve owner without overlap scanning.
    std::thread worker([&top]() {
        top.scalars[6] = 606u;
        top.slots[4].b = 404u;
        MatrixSlot* slots = top.slots.data();
        slots[6].d = 604u;
        top.nested[2].data()[3] = 2303u;
    });
    worker.join();
    tracer.sample(3);
    if (!patch_value_at(sink, scalars->node_id, 3, 6u * sizeof(std::uint32_t), 606u)) return fail("cross-thread scalar slice missing");
    if (!patch_value_at(sink, slots->node_id, 3, 4u * sizeof(MatrixSlot) + offsetof(MatrixSlot, b), 404u)) return fail("cross-thread struct slice missing");
    if (!patch_value_at(sink, slots->node_id, 3, 6u * sizeof(MatrixSlot) + offsetof(MatrixSlot, d), 604u)) return fail("cross-thread bulk data missing");
    if (!patch_value_at(sink, nested->node_id, 3, 2u * sizeof(wave::array<std::uint32_t, 4>) + 3u * sizeof(std::uint32_t), 2303u)) return fail("cross-thread nested bulk data missing");

    if (error_log_exists()) return fail("wave_runtime_error.log was produced on registered lookup matrix");
    std::cout << "wave_array_lookup_matrix_ok arrays=" << sink.array_block_declarations.size()
              << " patches=" << sink.array_patches.size() << "\n";
    return 0;
}
