#include "wave_runtime.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <psapi.h>
#endif

namespace {

typedef std::chrono::steady_clock BenchClock;

struct ScalarSpan {
    const std::uint32_t* values;
    std::size_t count;
    ScalarSpan() : values(NULL), count(0) {}
};

struct StaticObject {
    std::uint32_t value;
    StaticObject() : value(0) {}
};

struct DynamicObject : wave::DynamicTraceTargetFor<DynamicObject> {
    std::uint32_t value;
    DynamicObject() : value(0) {}
};

struct StaticObjectArray {
    const StaticObject* values;
    std::size_t count;
    StaticObjectArray() : values(NULL), count(0) {}
};

struct DynamicObjectArray {
    const DynamicObject* values;
    std::size_t count;
    DynamicObjectArray() : values(NULL), count(0) {}
};

struct StaticSlot {
    const StaticObject* value;
    StaticSlot() : value(NULL) {}
};

struct DynamicSlot {
    const wave::DynamicTraceTarget* value;
    DynamicSlot() : value(NULL) {}
};

struct StaticSlotArray {
    const StaticSlot* values;
    std::size_t count;
    StaticSlotArray() : values(NULL), count(0) {}
};

struct DynamicSlotArray {
    const DynamicSlot* values;
    std::size_t count;
    DynamicSlotArray() : values(NULL), count(0) {}
};

struct StaticBlock {
    ScalarSpan payload;
};

struct DynamicBlock : wave::DynamicTraceTargetFor<DynamicBlock> {
    ScalarSpan payload;
};

struct PeekSource : wave::PeekTraceSourceFor<PeekSource, ScalarSpan> {
    ScalarSpan payload;
    const ScalarSpan* peek() const noexcept { return &payload; }
};

struct DynamicPeekBlock : wave::DynamicTraceTargetFor<DynamicPeekBlock> {
    PeekSource source;
};

class CountingSink : public wave::IWaveSink {
public:
    std::size_t tracks;
    CountingSink() : tracks(0) {}
    void on_node_declared(const wave::NodeDecl&) override {}
    void on_track_declared(const wave::TrackDecl&) override { ++tracks; }
    void on_sample(const wave::TrackEvent&) override {}
};

struct ProcessMemory {
    std::uint64_t working_set;
    std::uint64_t private_bytes;
    std::uint64_t peak_working_set;
    ProcessMemory() : working_set(0), private_bytes(0), peak_working_set(0) {}
};

ProcessMemory process_memory() {
    ProcessMemory result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        result.working_set = static_cast<std::uint64_t>(counters.WorkingSetSize);
        result.private_bytes = static_cast<std::uint64_t>(counters.PrivateUsage);
        result.peak_working_set =
            static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#endif
    return result;
}

template <typename RootT>
int run_case(const char* kind,
             RootT* root,
             std::size_t expected_tracks,
             bool enable_dirty_peek_groups = true) {
    CountingSink sink;
    wave::BuildOptions options;
    options.enable_dirty_peek_groups = enable_dirty_peek_groups;
    options.enable_dynamic_dirty_groups = enable_dirty_peek_groups;
    options.enable_parallel_topology_expansion = false;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.debug_log_root_expand_stats = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    const ProcessMemory before = process_memory();
    const BenchClock::time_point begin = BenchClock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", root);
    tracer.prepare_topology(0);
    const BenchClock::time_point end = BenchClock::now();
    const ProcessMemory after = process_memory();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - begin).count();

    std::cout << "kind=" << kind
              << " tracks=" << sink.tracks
              << " topology_ms=" << elapsed_ms
              << " working_set_before=" << before.working_set
              << " working_set_after=" << after.working_set
              << " working_set_delta="
              << static_cast<std::int64_t>(after.working_set - before.working_set)
              << " private_before=" << before.private_bytes
              << " private_after=" << after.private_bytes
              << " private_delta="
              << static_cast<std::int64_t>(after.private_bytes - before.private_bytes)
              << " peak_bytes=" << after.peak_working_set
              << " dynamic_array_attempts=" << tracer.dynamic_array_entry_attempts()
              << " dynamic_array_successes=" << tracer.dynamic_array_entry_successes()
              << "\n";
    std::cout << tracer.memory_usage_debug_report();
    {
        const std::string distribution_path =
            std::string("tmp_dynamic_distribution_") + kind + ".txt";
        FILE* fp = std::fopen(distribution_path.c_str(), "w");
        if (fp) {
            tracer.dump_leaf_distribution_by_depth(fp, 8u, false);
            std::fclose(fp);
        }
    }
    if (sink.tracks != expected_tracks) {
        std::cerr << "track count mismatch expected=" << expected_tracks
                  << " actual=" << sink.tracks << "\n";
        return 2;
    }
    return 0;
}

} // namespace

namespace wave {

#define DEFINE_ONE_MEMBER_TABLE(TypeName, MemberName)                           \
template<> struct GeneratedMemberNameTable<TypeName> {                         \
    static constexpr bool generated = true;                                    \
    static const void* class_id() noexcept { static int id; return &id; }       \
    static const char* const* names() noexcept {                                \
        static const char* const value[] = {MemberName};                        \
        return value;                                                           \
    }                                                                           \
    static std::size_t count() noexcept { return 1u; }                          \
}

DEFINE_ONE_MEMBER_TABLE(ScalarSpan, "values");
DEFINE_ONE_MEMBER_TABLE(StaticObject, "value");
DEFINE_ONE_MEMBER_TABLE(DynamicObject, "value");
DEFINE_ONE_MEMBER_TABLE(StaticObjectArray, "values");
DEFINE_ONE_MEMBER_TABLE(DynamicObjectArray, "values");
DEFINE_ONE_MEMBER_TABLE(StaticSlot, "value");
DEFINE_ONE_MEMBER_TABLE(DynamicSlot, "value");
DEFINE_ONE_MEMBER_TABLE(StaticSlotArray, "values");
DEFINE_ONE_MEMBER_TABLE(DynamicSlotArray, "values");
DEFINE_ONE_MEMBER_TABLE(StaticBlock, "payload");
DEFINE_ONE_MEMBER_TABLE(DynamicBlock, "payload");
DEFINE_ONE_MEMBER_TABLE(DynamicPeekBlock, "source");

#undef DEFINE_ONE_MEMBER_TABLE

} // namespace wave

namespace reflect {

#define DEFINE_SCALAR_VISITOR(TypeName, MemberName)                             \
template<> struct is_reflected<TypeName> : std::true_type {};                  \
template<> struct reflected_visitor<TypeName> {                                \
    template<class P, class V, class G>                                        \
    static void visit(const TypeName* object, P&& on_ptr, V&&, G&&) {          \
        const void* id = wave::GeneratedMemberNameTable<TypeName>::class_id(); \
        wave::detail::invoke_ptr_visitor(                                      \
            on_ptr, #MemberName, &object->MemberName,                          \
            wave::detail::GeneratedMemberId(id, 0u));                          \
    }                                                                           \
}

#define DEFINE_ARRAY_VISITOR(TypeName)                                         \
template<> struct is_reflected<TypeName> : std::true_type {};                  \
template<> struct reflected_visitor<TypeName> {                                \
    template<class P, class V, class G>                                        \
    static void visit(const TypeName* object, P&& on_ptr, V&&, G&&) {          \
        const void* id = wave::GeneratedMemberNameTable<TypeName>::class_id(); \
        wave::detail::invoke_annotated_ptr_visitor(                            \
            on_ptr, "values", object->values, object->count,                   \
            wave::detail::GeneratedMemberId(id, 0u));                          \
    }                                                                           \
}

DEFINE_ARRAY_VISITOR(ScalarSpan);
DEFINE_SCALAR_VISITOR(StaticObject, value);
DEFINE_SCALAR_VISITOR(DynamicObject, value);
DEFINE_ARRAY_VISITOR(StaticObjectArray);
DEFINE_ARRAY_VISITOR(DynamicObjectArray);
DEFINE_ARRAY_VISITOR(StaticSlotArray);
DEFINE_ARRAY_VISITOR(DynamicSlotArray);
DEFINE_SCALAR_VISITOR(StaticBlock, payload);
DEFINE_SCALAR_VISITOR(DynamicBlock, payload);
DEFINE_SCALAR_VISITOR(DynamicPeekBlock, source);

#undef DEFINE_ARRAY_VISITOR
#undef DEFINE_SCALAR_VISITOR

template<> struct is_reflected<StaticSlot> : std::true_type {};
template<> struct reflected_visitor<StaticSlot> {
    template<class P, class V, class G>
    static void visit(const StaticSlot* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<StaticSlot>::class_id();
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "value", object->value, 1u,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<DynamicSlot> : std::true_type {};
template<> struct reflected_visitor<DynamicSlot> {
    template<class P, class V, class G>
    static void visit(const DynamicSlot* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<DynamicSlot>::class_id();
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "value", object->value, 1u,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

} // namespace reflect

int main(int argc, char** argv) {
    const std::string kind = argc > 1 ? argv[1] : "dynamic_slots";
    const std::size_t count =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], NULL, 10))
                 : 100000u;
    if (count == 0 || count > 10000000u) return 64;

    if (kind == "static_objects") {
        std::unique_ptr<StaticObject[]> values(new StaticObject[count]);
        StaticObjectArray root;
        root.values = values.get();
        root.count = count;
        return run_case("static_objects", &root, count);
    }

    if (kind == "dynamic_objects") {
        std::unique_ptr<DynamicObject[]> values(new DynamicObject[count]);
        wave::DynamicTypeRegistration<DynamicObject> registration;
        (void)registration;
        DynamicObjectArray root;
        root.values = values.get();
        root.count = count;
        return run_case("dynamic_objects", &root, count);
    }

    if (kind == "static_slots") {
        std::unique_ptr<StaticObject[]> objects(new StaticObject[count]);
        std::unique_ptr<StaticSlot[]> slots(new StaticSlot[count]);
        for (std::size_t i = 0; i < count; ++i) slots[i].value = &objects[i];
        StaticSlotArray root;
        root.values = slots.get();
        root.count = count;
        return run_case("static_slots", &root, count);
    }

    if (kind == "dynamic_slots" || kind == "dynamic_slots_nodirty") {
        std::unique_ptr<DynamicObject[]> objects(new DynamicObject[count]);
        std::unique_ptr<DynamicSlot[]> slots(new DynamicSlot[count]);
        for (std::size_t i = 0; i < count; ++i) slots[i].value = &objects[i];
        wave::DynamicTypeRegistration<DynamicObject> registration;
        (void)registration;
        DynamicSlotArray root;
        root.values = slots.get();
        root.count = count;
        const bool dirty = kind == "dynamic_slots";
        return run_case(kind.c_str(), &root, count, dirty);
    }

    std::unique_ptr<std::uint32_t[]> values(new std::uint32_t[count]);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::uint32_t>(i);
    }

    if (kind == "static_block") {
        StaticBlock root;
        root.payload.values = values.get();
        root.payload.count = count;
        return run_case("static_block", &root, count);
    }

    if (kind == "dynamic_block" || kind == "dynamic_block_nodirty") {
        DynamicBlock root;
        root.payload.values = values.get();
        root.payload.count = count;
        wave::DynamicTypeRegistration<DynamicBlock> registration;
        (void)registration;
        const bool dirty = kind == "dynamic_block";
        return run_case(kind.c_str(), &root, count, dirty);
    }

    if (kind == "dynamic_peek") {
        DynamicPeekBlock root;
        root.source.payload.values = values.get();
        root.source.payload.count = count;
        wave::DynamicTypeRegistration<DynamicPeekBlock> dynamic_registration;
        wave::DynamicTypeRegistration<ScalarSpan> scalar_registration;
        (void)dynamic_registration;
        (void)scalar_registration;
        return run_case("dynamic_peek", &root, count);
    }

    return 64;
}
