#include "wave_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#endif

namespace {

typedef std::chrono::steady_clock BenchClock;
bool g_dynamic_object_layout = false;
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
bool g_force_parallel_fragment_error = false;
bool g_force_parallel_storage_alias = false;
#endif

struct PeekPayload {
    std::uint32_t status = 0;
    std::uint32_t counter = 0;
    std::uint16_t lane = 0;
    bool ready = false;
};

#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
PeekPayload& forced_alias_payload() {
    static PeekPayload value;
    return value;
}
#endif

struct PeekSource : wave::PeekTraceSourceFor<PeekSource, PeekPayload> {
    PeekPayload value;
    PeekPayload* peek() { return &value; }
};

struct ComplexElement {
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
    std::size_t ordinal = 0;
#endif
    std::uint8_t opcode = 0;
    std::uint16_t tag = 0;
    std::uint32_t address = 0;
    std::uint64_t data = 0;
    bool valid = false;
    std::int32_t credit = 0;
    PeekSource peek;
};

struct ComplexRoot {
    std::size_t count = 0;
    ComplexElement* elements = NULL;
};

class CountingSink : public wave::IWaveSink {
public:
    std::size_t nodes = 0;
    std::size_t tracks = 0;
    std::size_t samples = 0;

    void on_node_declared(const wave::NodeDecl&) override { ++nodes; }
    void on_track_declared(const wave::TrackDecl&) override { ++tracks; }
    void on_sample(const wave::TrackEvent&) override { ++samples; }
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

template<> struct GeneratedMemberNameTable<PeekPayload> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"status", "counter", "lane", "ready"};
        return value;
    }
    static std::size_t count() noexcept { return 4u; }
};

template<> struct GeneratedMemberNameTable<ComplexElement> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {
            "opcode", "tag", "address", "data", "valid", "credit", "peek"
        };
        return value;
    }
    static std::size_t count() noexcept { return 7u; }
};

template<> struct GeneratedMemberNameTable<ComplexRoot> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const value[] = {"elements"};
        return value;
    }
    static std::size_t count() noexcept { return 1u; }
};

} // namespace wave

namespace reflect {

template<> struct is_reflected<PeekPayload> : std::true_type {};
template<> struct reflected_visitor<PeekPayload> {
    template<class P, class V, class G>
    static void visit(const PeekPayload* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<PeekPayload>::class_id();
        const std::uint32_t* status = &object->status;
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
        if (g_force_parallel_storage_alias) status = &forced_alias_payload().status;
#endif
        wave::detail::invoke_ptr_visitor(on_ptr, "status", status,
            wave::detail::GeneratedMemberId(id, 0u));
        wave::detail::invoke_ptr_visitor(on_ptr, "counter", &object->counter,
            wave::detail::GeneratedMemberId(id, 1u));
        wave::detail::invoke_ptr_visitor(on_ptr, "lane", &object->lane,
            wave::detail::GeneratedMemberId(id, 2u));
        wave::detail::invoke_ptr_visitor(on_ptr, "ready", &object->ready,
            wave::detail::GeneratedMemberId(id, 3u));
    }
};

template<> struct is_reflected<ComplexElement> : std::true_type {};
template<> struct reflected_visitor<ComplexElement> {
    template<class P, class V, class G>
    static void visit(const ComplexElement* object, P&& on_ptr, V&&, G&&) {
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
        if (g_force_parallel_fragment_error && object->ordinal == 700u) {
            throw std::runtime_error("forced parallel fragment failure");
        }
#endif
        const void* id = wave::GeneratedMemberNameTable<ComplexElement>::class_id();
        wave::detail::invoke_ptr_visitor(on_ptr, "opcode", &object->opcode,
            wave::detail::GeneratedMemberId(id, 0u));
        wave::detail::invoke_ptr_visitor(on_ptr, "tag", &object->tag,
            wave::detail::GeneratedMemberId(id, 1u));
        wave::detail::invoke_ptr_visitor(on_ptr, "address", &object->address,
            wave::detail::GeneratedMemberId(id, 2u));
        wave::detail::invoke_ptr_visitor(on_ptr, "data", &object->data,
            wave::detail::GeneratedMemberId(id, 3u));
        wave::detail::invoke_ptr_visitor(on_ptr, "valid", &object->valid,
            wave::detail::GeneratedMemberId(id, 4u));
        wave::detail::invoke_ptr_visitor(on_ptr, "credit", &object->credit,
            wave::detail::GeneratedMemberId(id, 5u));
        if (!g_dynamic_object_layout || ((object->address / 64u) & 1u) == 0u) {
            wave::detail::invoke_ptr_visitor(on_ptr, "peek", &object->peek,
                wave::detail::GeneratedMemberId(id, 6u));
        }
    }
};

template<> struct is_reflected<ComplexRoot> : std::true_type {};
template<> struct reflected_visitor<ComplexRoot> {
    template<class P, class V, class G>
    static void visit(const ComplexRoot* object, P&& on_ptr, V&&, G&&) {
        const void* id = wave::GeneratedMemberNameTable<ComplexRoot>::class_id();
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "elements", object->elements, object->count,
            wave::detail::GeneratedMemberId(id, 0u));
    }
};

} // namespace reflect

int main(int argc, char** argv) {
    std::size_t element_count = 1000000u;
    std::size_t thread_count = 16u;
    std::size_t batch_elements = 131072u;
    bool sample_once = false;
    bool multithread_sample = false;
    if (argc > 1) element_count = static_cast<std::size_t>(std::strtoull(argv[1], NULL, 10));
    if (argc > 2) thread_count = static_cast<std::size_t>(std::strtoull(argv[2], NULL, 10));
    if (argc > 3) batch_elements = static_cast<std::size_t>(std::strtoull(argv[3], NULL, 10));
    if (argc > 4) {
        const std::string mode(argv[4]);
        sample_once = mode == "sample";
        multithread_sample = mode == "multithread";
        g_dynamic_object_layout = mode == "dynamic_layout";
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
        g_force_parallel_fragment_error = mode == "force_error";
        g_force_parallel_storage_alias = mode == "force_alias";
#endif
    }
    if (element_count == 0 || thread_count == 0 || batch_elements == 0) return 64;

    std::unique_ptr<ComplexElement[]> elements(new ComplexElement[element_count]);
    for (std::size_t i = 0; i < element_count; ++i) {
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
        elements[i].ordinal = i;
#endif
        elements[i].opcode = static_cast<std::uint8_t>(i);
        elements[i].tag = static_cast<std::uint16_t>(i * 3u);
        elements[i].address = static_cast<std::uint32_t>(i * 64u);
        elements[i].data = static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
        elements[i].valid = (i & 1u) != 0u;
        elements[i].credit = static_cast<std::int32_t>(i & 0x7ffu) - 1024;
        elements[i].peek.value.status = static_cast<std::uint32_t>(i ^ 0x55aa55aau);
        elements[i].peek.value.counter = static_cast<std::uint32_t>(i * 7u);
        elements[i].peek.value.lane = static_cast<std::uint16_t>(i & 31u);
        elements[i].peek.value.ready = (i % 3u) == 0u;
    }

    ComplexRoot root;
    root.count = element_count;
    root.elements = elements.get();

    // ReflectGen emits the same registration for every runtime-erased peek
    // value type.  Keep the benchmark faithful to generated reflection.
    wave::DynamicTypeRegistration<PeekPayload> peek_payload_registration;
    (void)peek_payload_registration;

    CountingSink sink;
    wave::BuildOptions options;
    options.enable_dirty_peek_groups = true;
    options.enable_parallel_topology_expansion = true;
    options.topology_expansion_threads = thread_count;
    options.parallel_topology_min_elements = 2u;
    options.parallel_topology_min_work_items_per_element = 1u;
    options.parallel_topology_batch_elements = batch_elements;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.debug_log_root_expand_stats = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    const BenchClock::time_point begin = BenchClock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
    try {
        tracer.prepare_topology(0);
    }
    catch (const std::exception& error) {
        if (g_force_parallel_fragment_error &&
            std::string(error.what()) == "forced parallel fragment failure") {
            std::cout << "parallel_error_propagated=1 message=" << error.what() << "\n";
            return 0;
        }
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
        if (g_force_parallel_fragment_error || g_force_parallel_storage_alias) {
            std::cerr << "unexpected_parallel_error=" << error.what() << "\n";
            return 7;
        }
#endif
        throw;
    }
#else
    tracer.prepare_topology(0);
#endif
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
    if (g_force_parallel_fragment_error) {
        std::cerr << "forced parallel fragment failure was hidden\n";
        return 5;
    }
#endif
    const BenchClock::time_point end = BenchClock::now();
    if (sample_once) {
        tracer.sample(0);
    } else if (multithread_sample) {
        tracer.sample(0);
        sink.samples = 0;

        std::atomic<std::size_t> ready_threads(0);
        std::atomic<bool> release_threads(false);
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            workers.push_back(std::thread([&, worker]() {
                if ((worker & 1u) == 0u) {
                    tracer.attach_current_thread_for_dirty_peek();
                }
                const std::size_t begin_index =
                    element_count * worker / thread_count;
                const std::size_t end_index =
                    element_count * (worker + 1u) / thread_count;
                for (std::size_t i = begin_index; i < end_index; ++i) {
                    elements[i].peek.value.status += 1u;
                    elements[i].peek.wave_dirty_hook()->mark_dirty();
                }
                ready_threads.fetch_add(1u, std::memory_order_release);
                while (!release_threads.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }));
        }
        while (ready_threads.load(std::memory_order_acquire) != thread_count) {
            std::this_thread::yield();
        }
        tracer.sample(1);
        release_threads.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
        if (sink.samples != element_count) {
            std::cerr << "live worker dirty sample count mismatch expected="
                      << element_count << " actual=" << sink.samples << "\n";
            return 8;
        }

        workers.clear();
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            workers.push_back(std::thread([&, worker]() {
                const std::size_t begin_index =
                    element_count * worker / thread_count;
                const std::size_t end_index =
                    element_count * (worker + 1u) / thread_count;
                for (std::size_t i = begin_index; i < end_index; ++i) {
                    elements[i].peek.value.counter += 1u;
                    elements[i].peek.wave_dirty_hook()->mark_dirty();
                }
            }));
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
        tracer.sample(2);

        elements[0].peek.value.lane += 1u;
        workers.clear();
        ready_threads.store(0u, std::memory_order_relaxed);
        release_threads.store(false, std::memory_order_relaxed);
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            workers.push_back(std::thread([&, worker]() {
                if ((worker & 1u) == 0u) {
                    tracer.attach_current_thread_for_dirty_peek();
                }
                for (std::size_t repeat = 0; repeat < 4096u; ++repeat) {
                    elements[0].peek.wave_dirty_hook()->mark_dirty();
                }
                ready_threads.fetch_add(1u, std::memory_order_release);
                while (!release_threads.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }));
        }
        while (ready_threads.load(std::memory_order_acquire) != thread_count) {
            std::this_thread::yield();
        }
        tracer.sample(3);
        release_threads.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
    }

    const std::size_t peek_element_count = g_dynamic_object_layout
        ? (element_count + 1u) / 2u
        : element_count;
    const std::size_t expected_tracks =
        element_count * 6u + peek_element_count * 4u;
    std::cout << "elements=" << element_count
              << " expected_tracks=" << expected_tracks
              << " tracks=" << sink.tracks
              << " nodes=" << sink.nodes
              << " topology_ms=" << elapsed_ms(begin, end)
              << " parallel_batches=" << tracer.parallel_topology_batches()
              << " parallel_elements=" << tracer.parallel_topology_expanded_elements()
              << " fallback_batches=" << tracer.parallel_topology_fallback_batches()
              << " peak_bytes=" << peak_working_set_bytes()
              << " samples=" << sink.samples
              << " summary={" << tracer.topology_debug_summary(1u) << "}"
              << "\n";
#if defined(WAVE_RUNTIME_BENCH_FAULT_INJECTION)
    if (g_force_parallel_storage_alias) {
        std::cout << "parallel_leaf_alias_allowed=1"
                  << " fallback_batches=" << tracer.parallel_topology_fallback_batches()
                  << "\n";
    }
#endif
    if (sink.tracks != expected_tracks) {
        std::cerr << "track count mismatch\n";
        return 2;
    }
    if (sample_once && sink.samples != expected_tracks) {
        std::cerr << "sample count mismatch\n";
        return 4;
    }
    if (multithread_sample && sink.samples != element_count * 2u + 1u) {
        std::cerr << "multithread dirty sample count mismatch expected="
                  << element_count * 2u + 1u << " actual=" << sink.samples << "\n";
        return 9;
    }
    if (tracer.parallel_topology_batches() == 0u ||
        tracer.parallel_topology_expanded_elements() + 1u != element_count ||
        tracer.parallel_topology_fallback_batches() != 0u) {
        std::cerr << "parallel expansion invariant mismatch\n";
        return 3;
    }
    return 0;
}
