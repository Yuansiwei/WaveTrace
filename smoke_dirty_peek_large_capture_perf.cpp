#include "wave_runtime.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

#if defined(_WIN32)
#include <psapi.h>
#endif

namespace {

typedef std::chrono::steady_clock BenchClock;

struct LargePeekPayload {
    const std::uint32_t* values = NULL;
    std::size_t count = 0;
};

struct LargePeekSource : wave::PeekTraceSourceFor<LargePeekSource, LargePeekPayload> {
    LargePeekPayload payload;
    const LargePeekPayload* peek() const noexcept { return &payload; }
};

struct Root {
    LargePeekSource source;
};

class CountingSink : public wave::IWaveSink {
public:
    std::size_t tracks = 0;
    void on_node_declared(const wave::NodeDecl&) override {}
    void on_track_declared(const wave::TrackDecl&) override { ++tracks; }
    void on_sample(const wave::TrackEvent&) override {}
};

double elapsed_ms(BenchClock::time_point begin, BenchClock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

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

} // namespace

namespace wave {

template<> struct GeneratedMemberNameTable<LargePeekPayload> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"values"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<Root> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"source"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

} // namespace wave

namespace reflect {

template<> struct is_reflected<LargePeekPayload> : std::true_type {};
template<> struct reflected_visitor<LargePeekPayload> {
    template<class P, class V, class G>
    static void visit(const LargePeekPayload* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<LargePeekPayload>::class_id();
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr,
            "values",
            object->values,
            object->count,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

template<> struct is_reflected<Root> : std::true_type {};
template<> struct reflected_visitor<Root> {
    template<class P, class V, class G>
    static void visit(const Root* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<Root>::class_id();
        wave::detail::invoke_ptr_visitor(
            on_ptr,
            "source",
            &object->source,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

} // namespace reflect

int main(int argc, char** argv) {
    std::size_t count = 10000u;
    if (argc > 1) count = static_cast<std::size_t>(std::strtoull(argv[1], NULL, 10));
    if (count == 0) return 64;
    const bool enable_dirty_peek = argc <= 2 || std::atoi(argv[2]) != 0;

    std::unique_ptr<std::uint32_t[]> values(new std::uint32_t[count]);
    for (std::size_t i = 0; i < count; ++i) values[i] = static_cast<std::uint32_t>(i);

    Root root;
    root.source.payload.values = values.get();
    root.source.payload.count = count;

    wave::DynamicTypeRegistration<LargePeekPayload> registration;
    (void)registration;

    CountingSink sink;
    wave::BuildOptions options;
    options.enable_dirty_peek_groups = enable_dirty_peek;
    options.enable_parallel_topology_expansion = false;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.debug_log_root_expand_stats = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    const BenchClock::time_point begin = BenchClock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);
    const BenchClock::time_point end = BenchClock::now();

    std::cout << "elements=" << count
              << " dirty_peek=" << (enable_dirty_peek ? 1 : 0)
              << " tracks=" << sink.tracks
              << " topology_ms=" << elapsed_ms(begin, end)
              << " peak_bytes=" << peak_working_set_bytes()
              << "\n";
    std::cout << tracer.memory_usage_debug_report();
    if (sink.tracks != count) {
        std::cerr << "track count mismatch expected=" << count
                  << " actual=" << sink.tracks << "\n";
        return 2;
    }
    return 0;
}
