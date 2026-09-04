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

struct PeekSource : wave::PeekTraceSourceFor<PeekSource, ScalarSpan> {
    ScalarSpan payload;
    const ScalarSpan* peek() const noexcept { return &payload; }
};

struct PeekRoot {
    PeekSource source;
};

struct NestedAliasPayload {
    PeekSource inner;
    const std::uint32_t* aliases;
    std::size_t count;
    NestedAliasPayload() : aliases(NULL), count(0) {}
};

struct NestedAliasSource
    : wave::PeekTraceSourceFor<NestedAliasSource, NestedAliasPayload> {
    NestedAliasPayload payload;
    const NestedAliasPayload* peek() const noexcept { return &payload; }
};

struct NestedAliasRoot {
    NestedAliasSource source;
};

struct PlainRoot {
    ScalarSpan payload;
};

struct WaveValueSpan {
    const wave::WaveValue<std::uint32_t>* values;
    std::size_t count;
    WaveValueSpan() : values(NULL), count(0) {}
};

struct WaveValueRoot {
    WaveValueSpan payload;
};

typedef wave::array<std::uint32_t, 1000000u> MillionWaveArray;
typedef std::array<std::uint32_t, 1000000u> MillionStdArray;

class CountingSink : public wave::IWaveSink {
public:
    std::size_t tracks;
    CountingSink() : tracks(0) {}
    void on_node_declared(const wave::NodeDecl&) override {}
    void on_track_declared(const wave::TrackDecl&) override { ++tracks; }
    void on_sample(const wave::TrackEvent&) override {}
};

std::uint64_t peak_working_set_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#endif
    return 0;
}

template <typename RootT>
int run_case(const char* kind, RootT* root, std::size_t expected_tracks) {
    CountingSink sink;
    wave::BuildOptions options;
    options.enable_dirty_peek_groups = true;
    options.enable_parallel_topology_expansion = false;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.debug_log_root_expand_stats = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    const BenchClock::time_point begin = BenchClock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", root);
    tracer.prepare_topology(0);
    const BenchClock::time_point end = BenchClock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - begin).count();

    std::cout << "kind=" << kind
              << " tracks=" << sink.tracks
              << " topology_ms=" << elapsed_ms
              << " peak_bytes=" << peak_working_set_bytes()
              << "\n";
    std::cout << tracer.memory_usage_debug_report();
    if (sink.tracks != expected_tracks) {
        std::cerr << "track count mismatch expected=" << expected_tracks
                  << " actual=" << sink.tracks << "\n";
        return 2;
    }
    return 0;
}

} // namespace

namespace wave {

template<> struct GeneratedMemberNameTable<ScalarSpan> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"values"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<PeekRoot> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"source"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<PlainRoot> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"payload"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<NestedAliasPayload> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"inner", "aliases"};
        return value;
    }
    static std::size_t count() noexcept { return 2u; }
};

template<> struct GeneratedMemberNameTable<NestedAliasRoot> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"source"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<WaveValueSpan> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"values"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<WaveValueRoot> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"payload"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

} // namespace wave

namespace reflect {

template<> struct is_reflected<ScalarSpan> : std::true_type {};
template<> struct reflected_visitor<ScalarSpan> {
    template<class P, class V, class G>
    static void visit(const ScalarSpan* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<ScalarSpan>::class_id();
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "values", object->values, object->count,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<PeekRoot> : std::true_type {};
template<> struct reflected_visitor<PeekRoot> {
    template<class P, class V, class G>
    static void visit(const PeekRoot* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<PeekRoot>::class_id();
        wave::detail::invoke_ptr_visitor(
            on_ptr, "source", &object->source,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<PlainRoot> : std::true_type {};
template<> struct reflected_visitor<PlainRoot> {
    template<class P, class V, class G>
    static void visit(const PlainRoot* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<PlainRoot>::class_id();
        wave::detail::invoke_ptr_visitor(
            on_ptr, "payload", &object->payload,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<NestedAliasPayload> : std::true_type {};
template<> struct reflected_visitor<NestedAliasPayload> {
    template<class P, class V, class G>
    static void visit(const NestedAliasPayload* object, P&& on_ptr, V&&, G&&) {
        const void* id =
            wave::GeneratedMemberNameTable<NestedAliasPayload>::class_id();
        wave::detail::invoke_ptr_visitor(
            on_ptr, "inner", &object->inner,
            wave::detail::GeneratedMemberId(id, 0u));
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "aliases", object->aliases, object->count,
            wave::detail::GeneratedMemberId(id, 1u));
    }
};

template<> struct is_reflected<NestedAliasRoot> : std::true_type {};
template<> struct reflected_visitor<NestedAliasRoot> {
    template<class P, class V, class G>
    static void visit(const NestedAliasRoot* object, P&& on_ptr, V&&, G&&) {
        const void* id =
            wave::GeneratedMemberNameTable<NestedAliasRoot>::class_id();
        wave::detail::invoke_ptr_visitor(
            on_ptr, "source", &object->source,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<WaveValueSpan> : std::true_type {};
template<> struct reflected_visitor<WaveValueSpan> {
    template<class P, class V, class G>
    static void visit(const WaveValueSpan* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<WaveValueSpan>::class_id();
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "values", object->values, object->count,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<WaveValueRoot> : std::true_type {};
template<> struct reflected_visitor<WaveValueRoot> {
    template<class P, class V, class G>
    static void visit(const WaveValueRoot* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<WaveValueRoot>::class_id();
        wave::detail::invoke_ptr_visitor(
            on_ptr, "payload", &object->payload,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

} // namespace reflect

int main(int argc, char** argv) {
    const std::string kind = argc > 1 ? argv[1] : "plain";
    const std::size_t count =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], NULL, 10))
                 : 100000u;
    if (count == 0 || count > 1000000u) return 64;

    if (kind == "plain" || kind == "peek" || kind == "nested_alias") {
        std::unique_ptr<std::uint32_t[]> values(new std::uint32_t[count]);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = static_cast<std::uint32_t>(i);
        }
        if (kind == "plain") {
            PlainRoot root;
            root.payload.values = values.get();
            root.payload.count = count;
            return run_case("plain", &root, count);
        }
        wave::DynamicTypeRegistration<ScalarSpan> registration;
        (void)registration;
        if (kind == "peek") {
            PeekRoot root;
            root.source.payload.values = values.get();
            root.source.payload.count = count;
            return run_case("peek", &root, count);
        }
        NestedAliasRoot root;
        root.source.payload.inner.payload.values = values.get();
        root.source.payload.inner.payload.count = count;
        root.source.payload.aliases = values.get();
        root.source.payload.count = count;
        wave::DynamicTypeRegistration<NestedAliasPayload> alias_registration;
        (void)alias_registration;
        return run_case("nested_alias", &root, count * 2u);
    }

    if (kind == "wave_value") {
        std::unique_ptr<wave::WaveValue<std::uint32_t>[]> values(
            new wave::WaveValue<std::uint32_t>[count]);
        WaveValueRoot root;
        root.payload.values = values.get();
        root.payload.count = count;
        return run_case("wave_value", &root, count);
    }

    if (kind == "wave_array") {
        if (count != 1000000u) return 64;
        std::unique_ptr<MillionWaveArray> values(new MillionWaveArray());
        return run_case("wave_array", values.get(), count);
    }

    if (kind == "std_array") {
        if (count != 1000000u) return 64;
        std::unique_ptr<MillionStdArray> values(new MillionStdArray());
        return run_case("std_array", values.get(), count);
    }

    return 64;
}
