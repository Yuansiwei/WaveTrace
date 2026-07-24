#include "wave_runtime.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>

struct ArrayFirstLeaf {
    std::uint32_t value;

    ArrayFirstLeaf() : value(0) {}
    explicit ArrayFirstLeaf(std::uint32_t v) : value(v) {}
};

struct ArrayFirstTop {
    std::uint16_t c_values[3];
    std::array<std::uint32_t, 4> std_values;
    wave::array<ArrayFirstLeaf, 5> wave_values;
    std::uint8_t nested[2][3];
    ArrayFirstLeaf pointer_storage[6];
    std::size_t pointer_count;
    WAVE_PTR_ARRAY(pointer_count) ArrayFirstLeaf* pointer_values;

    ArrayFirstTop()
        : c_values{11, 12, 13},
          std_values{{21, 22, 23, 24}},
          nested{{31, 32, 33}, {34, 35, 36}},
          pointer_storage{
              ArrayFirstLeaf(41), ArrayFirstLeaf(42), ArrayFirstLeaf(43),
              ArrayFirstLeaf(44), ArrayFirstLeaf(45), ArrayFirstLeaf(46)},
          pointer_count(6),
          pointer_values(pointer_storage) {
        for (std::size_t i = 0; i < wave_values.size(); ++i) {
            wave_values[i].value = static_cast<std::uint32_t>(51 + i);
        }
    }
};

struct ArrayFirstStressTop {
    std::unique_ptr<ArrayFirstLeaf[]> storage;
    std::size_t count;
    WAVE_PTR_ARRAY(count) ArrayFirstLeaf* values;

    explicit ArrayFirstStressTop(std::size_t element_count)
        : storage(new ArrayFirstLeaf[element_count]),
          count(element_count),
          values(storage.get()) {
        if (count != 0u) values[0].value = 0x12345678u;
    }
};

struct ArrayFirstPointerStorageTop {
    static const std::size_t slot_count = 4u;
    static const std::size_t target_count = 3u;

    ArrayFirstLeaf raw_storage[slot_count][target_count];
    ArrayFirstLeaf std_storage[slot_count][target_count];
    ArrayFirstLeaf wave_storage[slot_count][target_count];
    ArrayFirstLeaf nested_storage[2][3][target_count];
    ArrayFirstLeaf* raw_slots[slot_count];
    std::array<ArrayFirstLeaf*, slot_count> std_slots;
    wave::array<ArrayFirstLeaf*, slot_count> wave_slots;
    ArrayFirstLeaf* nested_slots[2][3];
    std::shared_ptr<ArrayFirstLeaf> weak_owners[slot_count];
    std::weak_ptr<ArrayFirstLeaf> weak_slots[slot_count];

    ArrayFirstPointerStorageTop() {
        for (std::size_t i = 0; i < slot_count; ++i) {
            for (std::size_t j = 0; j < target_count; ++j) {
                raw_storage[i][j].value =
                    static_cast<std::uint32_t>(1000u + i * 10u + j);
                std_storage[i][j].value =
                    static_cast<std::uint32_t>(2000u + i * 10u + j);
                wave_storage[i][j].value =
                    static_cast<std::uint32_t>(3000u + i * 10u + j);
            }
            raw_slots[i] = raw_storage[i];
            std_slots[i] = std_storage[i];
            wave_slots[i] = wave_storage[i];
            weak_owners[i].reset(new ArrayFirstLeaf(
                static_cast<std::uint32_t>(5000u + i)));
            weak_slots[i] = weak_owners[i];
        }
        for (std::size_t i = 0; i < 2u; ++i) {
            for (std::size_t j = 0; j < 3u; ++j) {
                for (std::size_t k = 0; k < target_count; ++k) {
                    nested_storage[i][j][k].value =
                        static_cast<std::uint32_t>(4000u + i * 100u + j * 10u + k);
                }
                nested_slots[i][j] = nested_storage[i][j];
            }
        }
    }
};

struct ArrayFirstLargePointerStorageTop {
    static const std::size_t slot_count = 10000000u;

    std::unique_ptr<ArrayFirstLeaf[]> storage;
    ArrayFirstLeaf* slots[slot_count];

    ArrayFirstLargePointerStorageTop()
        : storage(new ArrayFirstLeaf[slot_count]) {
        for (std::size_t i = 0; i < slot_count; ++i) {
            storage[i].value = static_cast<std::uint32_t>(i);
            slots[i] = std::addressof(storage[i]);
        }
        storage[0].value = 0x87654321u;
    }
};

namespace reflect {

template <>
struct is_reflected<ArrayFirstLeaf> : std::true_type {};

template <>
struct reflected_visitor<ArrayFirstLeaf> {
    template <class P, class V, class G>
    static void visit(const ArrayFirstLeaf* object, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(object->value));
    }
};

template <>
struct is_reflected<ArrayFirstTop> : std::true_type {};

template <>
struct reflected_visitor<ArrayFirstTop> {
    template <class P, class V, class G>
    static void visit(const ArrayFirstTop* object, P&& on_ptr, V&&, G&&) {
        on_ptr("c_values", std::addressof(object->c_values));
        on_ptr("std_values", std::addressof(object->std_values));
        on_ptr("wave_values", std::addressof(object->wave_values));
        on_ptr("nested", std::addressof(object->nested));
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr,
            "pointer_values",
            object->pointer_values,
            object->pointer_count);
    }
};

template <>
struct is_reflected<ArrayFirstStressTop> : std::true_type {};

template <>
struct reflected_visitor<ArrayFirstStressTop> {
    template <class P, class V, class G>
    static void visit(const ArrayFirstStressTop* object, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr,
            "values",
            object->values,
            object->count);
    }
};

template <>
struct is_reflected<ArrayFirstPointerStorageTop> : std::true_type {};

template <>
struct reflected_visitor<ArrayFirstPointerStorageTop> {
    template <class P, class V, class G>
    static void visit(const ArrayFirstPointerStorageTop* object, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_annotated_ptr_array_storage_array_visitor(
            on_ptr, "raw_slots", object->raw_slots,
            ArrayFirstPointerStorageTop::target_count);
        ::wave::detail::invoke_annotated_ptr_array_storage_array_visitor(
            on_ptr, "std_slots", object->std_slots,
            ArrayFirstPointerStorageTop::target_count);
        ::wave::detail::invoke_annotated_ptr_array_storage_array_visitor(
            on_ptr, "wave_slots", object->wave_slots,
            ArrayFirstPointerStorageTop::target_count);
        ::wave::detail::invoke_annotated_ptr_array_storage_array_visitor(
            on_ptr, "nested_slots", object->nested_slots,
            ArrayFirstPointerStorageTop::target_count);
        ::wave::detail::invoke_annotated_weak_ptr_storage_array_visitor(
            on_ptr, "weak_slots", object->weak_slots);
    }
};

template <>
struct is_reflected<ArrayFirstLargePointerStorageTop> : std::true_type {};

template <>
struct reflected_visitor<ArrayFirstLargePointerStorageTop> {
    template <class P, class V, class G>
    static void visit(const ArrayFirstLargePointerStorageTop* object, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_annotated_ptr_storage_array_visitor(
            on_ptr, "slots", object->slots, 1u);
    }
};

} // namespace reflect

namespace {

struct TraceIndex {
    std::map<std::string, wave::TrackId> by_path;
};

TraceIndex build_index(const wave::InMemoryWaveSink& sink) {
    TraceIndex index;
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        index.by_path[sink.declarations[i].path] = sink.declarations[i].track_id;
    }
    return index;
}

bool has_path(const TraceIndex& index, const char* path) {
    return index.by_path.find(path) != index.by_path.end();
}

bool has_u64_event(const wave::InMemoryWaveSink& sink,
                   const TraceIndex& index,
                   const char* path,
                   wave::Cycle cycle,
                   std::uint64_t value) {
    const std::map<std::string, wave::TrackId>::const_iterator found =
        index.by_path.find(path);
    if (found == index.by_path.end()) return false;
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        const wave::TrackEvent& event = sink.events[i];
        if (event.track_id == found->second &&
            event.cycle == cycle &&
            event.has_u64 &&
            event.u64_value == value) {
            return true;
        }
    }
    return false;
}

int fail(const char* message, const wave::InMemoryWaveSink& sink) {
    std::cerr << message << "\n";
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        std::cerr << "  " << sink.declarations[i].path << "\n";
    }
    return 1;
}

int run_full_mode() {
    ArrayFirstTop top;
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.enable_wave_array_dirty = true;

    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &top);
    tracer.sample(0);

    const TraceIndex index = build_index(sink);
    if (sink.declarations.size() != 24u) {
        return fail("full mode must retain all 24 scalar leaves", sink);
    }
    const char* required[] = {
        "top.c_values.[2]",
        "top.std_values.[3]",
        "top.wave_values.[4].value",
        "top.nested.[1].[2]",
        "top.pointer_values.[5].value"
    };
    for (std::size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!has_path(index, required[i])) {
            return fail("full mode path missing or unexpectedly renamed", sink);
        }
    }
    return 0;
}

int run_first_only_mode(bool enable_in_build_options) {
    ArrayFirstTop top;
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.enable_wave_array_dirty = true;
    options.trace_array_first_element_only = enable_in_build_options;

    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &top);
    tracer.sample(0);

    const TraceIndex index = build_index(sink);
    const char* required[] = {
        "top.c_values[size=3].[0]",
        "top.std_values[size=4].[0]",
        "top.wave_values[size=5].[0].value",
        "top.nested[size=2].[0][size=3].[0]",
        "top.pointer_values[size=6].[0].value"
    };
    if (sink.declarations.size() != sizeof(required) / sizeof(required[0])) {
        return fail("first-only mode must retain exactly one scalar leaf per array branch", sink);
    }
    for (std::size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!has_path(index, required[i])) {
            return fail("first-only path or logical size annotation missing", sink);
        }
    }
    if (has_path(index, "top.c_values[size=3].[1]") ||
        has_path(index, "top.wave_values[size=5].[4].value") ||
        has_path(index, "top.pointer_values[size=6].[5].value")) {
        return fail("first-only mode leaked a nonzero array element", sink);
    }
    if (!has_u64_event(sink, index, required[0], 0, 11) ||
        !has_u64_event(sink, index, required[1], 0, 21) ||
        !has_u64_event(sink, index, required[2], 0, 51) ||
        !has_u64_event(sink, index, required[3], 0, 31) ||
        !has_u64_event(sink, index, required[4], 0, 41)) {
        return fail("first-only initial value is not element zero", sink);
    }

    top.c_values[0] = 111;
    top.std_values[0] = 121;
    top.wave_values[0].value = 151;
    top.nested[0][0] = 131;
    top.pointer_storage[0].value = 141;
    // This write intentionally touches an untracked wave::array element.  It
    // must be harmless and must not create hidden topology or dirty work.
    top.wave_values[4].value = 199;
    tracer.sample(1);

    if (has_u64_event(sink, index, required[0], 1, 111) ||
        has_u64_event(sink, index, required[1], 1, 121) ||
        has_u64_event(sink, index, required[2], 1, 151) ||
        has_u64_event(sink, index, required[3], 1, 131) ||
        has_u64_event(sink, index, required[4], 1, 141)) {
        return fail("first-only mode recorded a value after cycle zero", sink);
    }
    return 0;
}

int run_large_pointer_array_mode() {
    const std::size_t logical_count = 10000000u;
    ArrayFirstStressTop top(logical_count);
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.trace_array_first_element_only = true;

    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("stress", &top);
    tracer.sample(0);
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    const TraceIndex index = build_index(sink);
    const char* path = "stress.values[size=10000000].[0].value";
    if (sink.declarations.size() != 1u || !has_path(index, path)) {
        return fail("10M logical array expanded more than element zero", sink);
    }
    if (!has_u64_event(sink, index, path, 0, 0x12345678u)) {
        return fail("10M logical array element zero value mismatch", sink);
    }
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "array_first_only_10m_topology_ms=" << elapsed_ms << "\n";
    return 0;
}

int run_pointer_storage_array_mode(bool first_only,
                                   bool enable_in_build_options = true) {
    ArrayFirstPointerStorageTop top;
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.trace_array_first_element_only =
        first_only && enable_in_build_options;

    wave::Tracer tracer(sink, options);
    tracer.add_root("pointer_storage", &top);
    tracer.sample(0);

    const TraceIndex index = build_index(sink);
    if (!first_only) {
        if (sink.declarations.size() != 58u) {
            return fail("full mode changed pointer-storage array topology", sink);
        }
        const char* full_required[] = {
            "pointer_storage.raw_slots[3].[2].value",
            "pointer_storage.std_slots[3].[2].value",
            "pointer_storage.wave_slots[3].[2].value",
            "pointer_storage.nested_slots[1][2].[2].value",
            "pointer_storage.weak_slots[3].value"
        };
        for (std::size_t i = 0;
             i < sizeof(full_required) / sizeof(full_required[0]); ++i) {
            if (!has_path(index, full_required[i])) {
                return fail("full mode pointer-storage array path missing", sink);
            }
        }
        return 0;
    }

    const char* required[] = {
        "pointer_storage.raw_slots[size=4].[0][size=3].[0].value",
        "pointer_storage.std_slots[size=4].[0][size=3].[0].value",
        "pointer_storage.wave_slots[size=4].[0][size=3].[0].value",
        "pointer_storage.nested_slots[size=2].[0][size=3].[0][size=3].[0].value",
        "pointer_storage.weak_slots[size=4].[0].value"
    };
    if (sink.declarations.size() != sizeof(required) / sizeof(required[0])) {
        return fail("first-only mode leaked nonzero pointer-storage array slots", sink);
    }
    for (std::size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!has_path(index, required[i])) {
            return fail("pointer-storage array element zero is missing", sink);
        }
    }
    if (!has_u64_event(sink, index, required[0], 0, 1000u) ||
        !has_u64_event(sink, index, required[1], 0, 2000u) ||
        !has_u64_event(sink, index, required[2], 0, 3000u) ||
        !has_u64_event(sink, index, required[3], 0, 4000u) ||
        !has_u64_event(sink, index, required[4], 0, 5000u)) {
        return fail("pointer-storage array element-zero value mismatch", sink);
    }
    top.raw_storage[0][0].value = 1100u;
    top.std_storage[0][0].value = 2100u;
    top.wave_storage[0][0].value = 3100u;
    top.nested_storage[0][0][0].value = 4100u;
    top.weak_owners[0]->value = 5100u;
    // Nonzero slots remain intentionally untracked.
    top.raw_storage[1][0].value = 1199u;
    top.std_storage[1][0].value = 2199u;
    tracer.sample(7);
    if (has_u64_event(sink, index, required[0], 7, 1100u) ||
        has_u64_event(sink, index, required[1], 7, 2100u) ||
        has_u64_event(sink, index, required[2], 7, 3100u) ||
        has_u64_event(sink, index, required[3], 7, 4100u) ||
        has_u64_event(sink, index, required[4], 7, 5100u)) {
        return fail("pointer-storage first-only mode recorded after cycle zero", sink);
    }
    return 0;
}

int run_large_pointer_storage_array_mode() {
    std::unique_ptr<ArrayFirstLargePointerStorageTop> top(
        new ArrayFirstLargePointerStorageTop());
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.trace_array_first_element_only = true;

    const std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("large_pointer_storage", top.get());
    tracer.sample(0);
    const std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();

    const TraceIndex index = build_index(sink);
    const char* path =
        "large_pointer_storage.slots[size=10000000].[0].value";
    if (sink.declarations.size() != 1u || !has_path(index, path)) {
        return fail("10M pointer-storage slots were not pruned before visiting", sink);
    }
    if (!has_u64_event(sink, index, path, 0, 0x87654321u)) {
        return fail("10M pointer-storage slot zero value mismatch", sink);
    }
    top->storage[0].value = 0x12345678u;
    top->storage[1].value = 0xDEADBEEFu;
    tracer.sample(9);
    if (has_u64_event(sink, index, path, 9, 0x12345678u)) {
        return fail("10M pointer-storage first-only mode recorded after cycle zero", sink);
    }
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "array_first_only_10m_pointer_storage_topology_ms="
              << elapsed_ms << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--config") {
        if (!wave::config::runtime_config().wave_trace_array_first_only) {
            std::cerr << "WaveTraceArrayFirstOnly was not loaded from JSON\n";
            return 4;
        }
        if (run_first_only_mode(false) != 0) return 5;
        if (run_pointer_storage_array_mode(true, false) != 0) return 9;
        std::cout << "array_first_only_json_ok\n";
        return 0;
    }
    if (run_full_mode() != 0) return 1;
    if (run_first_only_mode(true) != 0) return 2;
    if (run_large_pointer_array_mode() != 0) return 3;
    if (run_pointer_storage_array_mode(false) != 0) return 6;
    if (run_pointer_storage_array_mode(true) != 0) return 7;
    if (run_large_pointer_storage_array_mode() != 0) return 8;
    std::cout << "array_first_only_ok\n";
    return 0;
}
