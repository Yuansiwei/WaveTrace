#include "wave_runtime.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

typedef unsigned char U01;

static std::atomic<std::size_t> g_top_visits(0);
static std::atomic<std::size_t> g_slot_visits(0);
static std::atomic<std::size_t> g_payload_visits(0);

static void dump_process_memory_phase(const char* phase,
                                      const wave::Tracer& tracer,
                                      const char* report_path) {
    std::uint64_t private_bytes = 0;
    std::uint64_t working_set_bytes = 0;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters;
    std::memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        private_bytes = static_cast<std::uint64_t>(counters.PrivateUsage);
        working_set_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
#endif
    std::ofstream out(report_path, std::ios::out | std::ios::trunc);
    if (out) out << tracer.memory_usage_debug_report();
    std::cout << "phase=" << phase
              << " private_bytes=" << private_bytes
              << " working_set_bytes=" << working_set_bytes
              << " report=" << report_path << "\n";
}

union Payload {
    std::uint32_t u32;
    std::uint16_t lo16;

    Payload() : u32(0) {}
};

struct Slot {
    U01 active;
    std::uint32_t counter;
    std::uint16_t tag;
    Payload payload;

    Slot() : active(0), counter(0), tag(0), payload() {}
};

struct GuardedDirtySlot {
    wave::WaveValue<std::uint32_t> value;
    wave::array<std::uint32_t, 4> values;
};

struct GuardedDirtyTop {
    std::size_t slot_count;
    WAVE_PTR_ARRAY(slot_count) GuardedDirtySlot* slots;

    GuardedDirtyTop(GuardedDirtySlot* data, std::size_t count)
        : slot_count(count), slots(data) {}
};

struct GuardedDynamicValue : wave::DynamicTraceTargetFor<GuardedDynamicValue> {
    std::uint32_t value;

    GuardedDynamicValue() : value(0) {}

    void write(std::uint32_t next) {
        value = next;
        wave_dirty_hook()->mark_dirty();
    }
};

struct GuardedHookSlot {
    GuardedDynamicValue dynamic;
};

struct GuardedHookTop {
    std::size_t slot_count;
    WAVE_PTR_ARRAY(slot_count) GuardedHookSlot* slots;

    GuardedHookTop(GuardedHookSlot* data, std::size_t count)
        : slot_count(count), slots(data) {}
};

struct WideSlot {
    std::array<std::uint32_t, 64> values;

    WideSlot() : values() {}
};

struct WideTop {
    std::size_t slot_count;
    WAVE_PTR_ARRAY(slot_count) WideSlot* slots;

    WideTop(WideSlot* data, std::size_t count)
        : slot_count(count), slots(data) {}
};

struct RuntimeNamedFields {
    std::array<std::uint32_t, 4> values;

    RuntimeNamedFields() : values() {}
};

struct Top {
    std::size_t slot_count;
    WAVE_PTR_ARRAY(slot_count) Slot* slots;

    Top(Slot* data, std::size_t count)
        : slot_count(count), slots(data) {}
};

namespace reflect {

template<> struct is_reflected<Payload> : std::true_type {};
template<> struct reflected_visitor<Payload> {
    template<class P, class V, class G>
    static void visit(const Payload* obj, P&& on_ptr, V&&, G&&) {
        ++g_payload_visits;
        on_ptr("u32", std::addressof(obj->u32));
        on_ptr("lo16", std::addressof(obj->lo16));
    }
};

template<> struct is_reflected<Slot> : std::true_type {};
template<> struct reflected_visitor<Slot> {
    template<class P, class V, class G>
    static void visit(const Slot* obj, P&& on_ptr, V&&, G&&) {
        ++g_slot_visits;
        on_ptr("active", ::wave::as_bool_storage_ptr(std::addressof(obj->active)));
        on_ptr("counter", std::addressof(obj->counter));
        on_ptr("tag", std::addressof(obj->tag));
        ::wave::detail::invoke_ptr_visitor(
            on_ptr,
            "payload",
            std::addressof(obj->payload),
            ::wave::detail::UnionFieldTag(),
            sizeof(Payload),
            ::wave::detail::UnionFieldBase(std::addressof(obj->payload)));
    }
};

template<> struct is_reflected<Top> : std::true_type {};
template<> struct reflected_visitor<Top> {
    template<class P, class V, class G>
    static void visit(const Top* obj, P&& on_ptr, V&&, G&&) {
        ++g_top_visits;
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "slots", obj->slots, obj->slot_count);
    }
};

template<> struct is_reflected<GuardedDirtySlot> : std::true_type {};
template<> struct reflected_visitor<GuardedDirtySlot> {
    template<class P, class V, class G>
    static void visit(const GuardedDirtySlot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(obj->value));
        on_ptr("values", std::addressof(obj->values));
    }
};

template<> struct is_reflected<GuardedDirtyTop> : std::true_type {};
template<> struct reflected_visitor<GuardedDirtyTop> {
    template<class P, class V, class G>
    static void visit(const GuardedDirtyTop* obj, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "slots", obj->slots, obj->slot_count);
    }
};

template<> struct is_reflected<GuardedDynamicValue> : std::true_type {};
template<> struct reflected_visitor<GuardedDynamicValue> {
    template<class P, class V, class G>
    static void visit(const GuardedDynamicValue* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(obj->value));
    }
};

template<> struct is_reflected<GuardedHookSlot> : std::true_type {};
template<> struct reflected_visitor<GuardedHookSlot> {
    template<class P, class V, class G>
    static void visit(const GuardedHookSlot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("dynamic", std::addressof(obj->dynamic));
    }
};

template<> struct is_reflected<GuardedHookTop> : std::true_type {};
template<> struct reflected_visitor<GuardedHookTop> {
    template<class P, class V, class G>
    static void visit(const GuardedHookTop* obj, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "slots", obj->slots, obj->slot_count);
    }
};

template<> struct is_reflected<WideSlot> : std::true_type {};
template<> struct reflected_visitor<WideSlot> {
    template<class P, class V, class G>
    static void visit(const WideSlot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("values", std::addressof(obj->values));
    }
};

template<> struct is_reflected<WideTop> : std::true_type {};
template<> struct reflected_visitor<WideTop> {
    template<class P, class V, class G>
    static void visit(const WideTop* obj, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "slots", obj->slots, obj->slot_count);
    }
};

template<> struct is_reflected<RuntimeNamedFields> : std::true_type {};
template<> struct reflected_visitor<RuntimeNamedFields> {
    template<class P, class V, class G>
    static void visit(const RuntimeNamedFields* obj, P&& on_ptr, V&&, G&&) {
        char name[3] = {'f', '0', '\0'};
        for (std::size_t i = 0; i < obj->values.size(); ++i) {
            name[1] = static_cast<char>('0' + i);
            on_ptr(name, std::addressof(obj->values[i]));
        }
    }
};

} // namespace reflect

struct EventSnapshot {
    wave::Cycle cycle;
    wave::TrackId track_id;
    wave::InvalidKind invalid;
    bool unchanged_keepalive;
    bool has_bool;
    bool bool_value;
    bool has_i64;
    std::int64_t i64_value;
    bool has_u64;
    std::uint64_t u64_value;
    bool has_f64;
    double f64_value;
    bool has_change_bits;
    std::uint64_t change_bits;
};

struct CountingWaveSink : public wave::IWaveSink {
    std::size_t node_count;
    std::size_t track_count;
    std::size_t event_count;

    CountingWaveSink() : node_count(0), track_count(0), event_count(0) {}

    virtual void on_node_declared(const wave::NodeDecl&) {
        ++node_count;
    }

    virtual void on_node_declared_fast(wave::NodeId,
                                       wave::NodeId,
                                       const std::string&,
                                       wave::NodeKind) {
        ++node_count;
    }

    virtual void on_track_declared(const wave::TrackDecl&) {
        ++track_count;
    }

    virtual void on_track_declared_fast(wave::TrackId,
                                        wave::TrackId,
                                        wave::NodeId,
                                        wave::ValueKind,
                                        std::uint32_t,
                                        std::uint32_t,
                                        bool,
                                        const std::string&) {
        ++track_count;
    }

    virtual void on_sample(const wave::TrackEvent&) {
        ++event_count;
    }
};

struct RunResult {
    std::vector<wave::NodeDecl> nodes;
    std::vector<wave::TrackDecl> tracks;
    std::vector<EventSnapshot> events;
    std::size_t top_visits;
    std::size_t slot_visits;
    std::size_t payload_visits;
    std::size_t parallel_topology_elements;
    std::size_t parallel_topology_batches;
    std::size_t parallel_topology_fallback_batches;
    std::uint64_t first_sample_ms;
    std::uint64_t elapsed_ms;
};

struct CountRunResult {
    std::size_t nodes;
    std::size_t tracks;
    std::size_t events;
    std::size_t top_visits;
    std::size_t slot_visits;
    std::size_t payload_visits;
    std::size_t parallel_topology_elements;
    std::size_t parallel_topology_batches;
    std::size_t parallel_topology_fallback_batches;
    std::uint64_t first_sample_ms;
    std::uint64_t elapsed_ms;
};

static bool same_node(const wave::NodeDecl& a, const wave::NodeDecl& b) {
    return a.node_id == b.node_id &&
           a.parent_id == b.parent_id &&
           a.name == b.name &&
           a.kind == b.kind;
}

static bool same_track(const wave::TrackDecl& a, const wave::TrackDecl& b) {
    return a.track_id == b.track_id &&
           a.storage_id == b.storage_id &&
           a.node_id == b.node_id &&
           a.path == b.path &&
           a.kind == b.kind &&
           a.bit_width == b.bit_width &&
           a.bit_offset == b.bit_offset &&
           a.storage_only == b.storage_only;
}

static bool same_event(const EventSnapshot& a, const EventSnapshot& b) {
    return a.cycle == b.cycle &&
           a.track_id == b.track_id &&
           a.invalid == b.invalid &&
           a.unchanged_keepalive == b.unchanged_keepalive &&
           a.has_bool == b.has_bool &&
           a.bool_value == b.bool_value &&
           a.has_i64 == b.has_i64 &&
           a.i64_value == b.i64_value &&
           a.has_u64 == b.has_u64 &&
           a.u64_value == b.u64_value &&
           a.has_f64 == b.has_f64 &&
           (!a.has_f64 || a.f64_value == b.f64_value) &&
           a.has_change_bits == b.has_change_bits &&
           a.change_bits == b.change_bits;
}

static void initialize_slots(std::vector<Slot>& slots) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].active = static_cast<U01>(i & 1u);
        slots[i].counter = static_cast<std::uint32_t>(i * 17u + 3u);
        slots[i].tag = static_cast<std::uint16_t>((i * 5u + 1u) & 0xFFFFu);
        slots[i].payload.u32 = static_cast<std::uint32_t>(0x9E370000u ^ static_cast<std::uint32_t>(i * 97u));
    }
}

static void update_slots(std::vector<Slot>& slots, std::uint32_t cycle) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].active = static_cast<U01>((i + cycle) & 1u);
        slots[i].counter += static_cast<std::uint32_t>((cycle * 13u) + (i & 7u) + 1u);
        slots[i].tag = static_cast<std::uint16_t>(slots[i].tag + static_cast<std::uint16_t>(cycle + (i % 11u)));
        slots[i].payload.u32 ^= static_cast<std::uint32_t>(0x01010101u + cycle + (i * 3u));
    }
}

static std::vector<EventSnapshot> snapshot_events(const std::vector<wave::TrackEvent>& events) {
    std::vector<EventSnapshot> out;
    out.reserve(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const wave::TrackEvent& e = events[i];
        EventSnapshot s;
        s.cycle = e.cycle;
        s.track_id = e.track_id;
        s.invalid = e.invalid;
        s.unchanged_keepalive = e.unchanged_keepalive;
        s.has_bool = e.has_bool;
        s.bool_value = e.bool_value;
        s.has_i64 = e.has_i64;
        s.i64_value = e.i64_value;
        s.has_u64 = e.has_u64;
        s.u64_value = e.u64_value;
        s.has_f64 = e.has_f64;
        s.f64_value = e.f64_value;
        s.has_change_bits = e.has_change_bits;
        s.change_bits = e.change_bits;
        out.push_back(s);
    }
    return out;
}

static RunResult run_case(std::size_t slot_count, std::uint32_t cycles, bool enable_topology_fast_path) {
    std::vector<Slot> slots(slot_count);
    initialize_slots(slots);
    Top top(slots.empty() ? static_cast<Slot*>(NULL) : &slots[0], slots.size());

    g_top_visits = 0;
    g_slot_visits = 0;
    g_payload_visits = 0;

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_union_fields = true;
    opt.enable_bitfield_fields = true;
    opt.enable_node_name_interning = enable_topology_fast_path;
    opt.enable_wave_ptr_array_batch_reserve = enable_topology_fast_path;
    opt.enable_flat_memory_block_precheck = enable_topology_fast_path;
    opt.enable_parallel_topology_expansion = enable_topology_fast_path &&
        std::getenv("WAVE_STRESS_DISABLE_PARALLEL_TOPOLOGY") == NULL;
    opt.parallel_topology_min_work_items_per_element = 0;
    opt.dump_leaf_distribution_after_topology = false;

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::uint64_t first_sample_ms = 0;
    std::size_t parallel_topology_elements = 0;
    std::size_t parallel_topology_batches = 0;
    std::size_t parallel_topology_fallback_batches = 0;
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("top", &top);
        const std::chrono::steady_clock::time_point first0 = std::chrono::steady_clock::now();
        tracer.sample(0);
        const std::chrono::steady_clock::time_point first1 = std::chrono::steady_clock::now();
        first_sample_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(first1 - first0).count());
        for (std::uint32_t c = 1; c <= cycles; ++c) {
            update_slots(slots, c);
            tracer.sample(c);
        }
        parallel_topology_elements = tracer.parallel_topology_expanded_elements();
        parallel_topology_batches = tracer.parallel_topology_batches();
        parallel_topology_fallback_batches = tracer.parallel_topology_fallback_batches();
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    RunResult out;
    out.nodes = sink.node_declarations;
    out.tracks = sink.declarations;
    out.events = snapshot_events(sink.events);
    out.top_visits = g_top_visits.load(std::memory_order_relaxed);
    out.slot_visits = g_slot_visits.load(std::memory_order_relaxed);
    out.payload_visits = g_payload_visits.load(std::memory_order_relaxed);
    out.parallel_topology_elements = parallel_topology_elements;
    out.parallel_topology_batches = parallel_topology_batches;
    out.parallel_topology_fallback_batches = parallel_topology_fallback_batches;
    out.first_sample_ms = first_sample_ms;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

static CountRunResult run_count_case(std::size_t slot_count,
                                     std::uint32_t cycles,
                                     bool enable_topology_fast_path,
                                     bool enable_parallel_sampling,
                                     std::size_t sampling_threads,
                                     bool dump_phase_memory = false) {
    std::vector<Slot> slots(slot_count);
    initialize_slots(slots);
    Top top(slots.empty() ? static_cast<Slot*>(NULL) : &slots[0], slots.size());

    g_top_visits = 0;
    g_slot_visits = 0;
    g_payload_visits = 0;

    CountingWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_union_fields = true;
    opt.enable_bitfield_fields = true;
    opt.enable_node_name_interning = enable_topology_fast_path;
    opt.enable_wave_ptr_array_batch_reserve = enable_topology_fast_path;
    opt.enable_flat_memory_block_precheck = enable_topology_fast_path;
    opt.enable_parallel_topology_expansion = enable_topology_fast_path &&
        std::getenv("WAVE_STRESS_DISABLE_PARALLEL_TOPOLOGY") == NULL;
    opt.parallel_topology_min_work_items_per_element = 0;
    opt.enable_parallel_sampling = enable_parallel_sampling;
    opt.sampling_threads = sampling_threads;
    opt.dump_leaf_distribution_after_topology = false;
    opt.trim_parallel_event_buffers_after_first_sample =
        std::getenv("WAVE_STRESS_DISABLE_EVENT_TRIM") == NULL;

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::uint64_t first_sample_ms = 0;
    std::size_t parallel_topology_elements = 0;
    std::size_t parallel_topology_batches = 0;
    std::size_t parallel_topology_fallback_batches = 0;
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("top", &top);
        if (dump_phase_memory) {
            tracer.prepare_topology(0);
            dump_process_memory_phase("topology", tracer, "phase_topology_memory.txt");
        }
        const std::chrono::steady_clock::time_point first0 = std::chrono::steady_clock::now();
        tracer.sample(0);
        const std::chrono::steady_clock::time_point first1 = std::chrono::steady_clock::now();
        if (dump_phase_memory) {
            dump_process_memory_phase("sample0", tracer, "phase_sample0_memory.txt");
        }
        first_sample_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(first1 - first0).count());
        for (std::uint32_t c = 1; c <= cycles; ++c) {
            if (!dump_phase_memory) update_slots(slots, c);
            tracer.sample(c);
            if (dump_phase_memory && c == 1u) {
                dump_process_memory_phase("sample1_sparse", tracer, "phase_sample1_sparse_memory.txt");
            }
        }
        parallel_topology_elements = tracer.parallel_topology_expanded_elements();
        parallel_topology_batches = tracer.parallel_topology_batches();
        parallel_topology_fallback_batches = tracer.parallel_topology_fallback_batches();
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    CountRunResult out;
    out.nodes = sink.node_count;
    out.tracks = sink.track_count;
    out.events = sink.event_count;
    out.top_visits = g_top_visits.load(std::memory_order_relaxed);
    out.slot_visits = g_slot_visits.load(std::memory_order_relaxed);
    out.payload_visits = g_payload_visits.load(std::memory_order_relaxed);
    out.parallel_topology_elements = parallel_topology_elements;
    out.parallel_topology_batches = parallel_topology_batches;
    out.parallel_topology_fallback_batches = parallel_topology_fallback_batches;
    out.first_sample_ms = first_sample_ms;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

static bool compare_results(const RunResult& a, const RunResult& b) {
    if (a.nodes.size() != b.nodes.size()) {
        std::cerr << "node count mismatch " << a.nodes.size() << " vs " << b.nodes.size() << "\n";
        return false;
    }
    if (a.tracks.size() != b.tracks.size()) {
        std::cerr << "track count mismatch " << a.tracks.size() << " vs " << b.tracks.size() << "\n";
        return false;
    }
    if (a.events.size() != b.events.size()) {
        std::cerr << "event count mismatch " << a.events.size() << " vs " << b.events.size() << "\n";
        return false;
    }
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        if (!same_node(a.nodes[i], b.nodes[i])) {
            std::cerr << "node mismatch at " << i << "\n";
            return false;
        }
    }
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        if (!same_track(a.tracks[i], b.tracks[i])) {
            std::cerr << "track mismatch at " << i
                      << " path " << a.tracks[i].path
                      << " vs " << b.tracks[i].path << "\n";
            return false;
        }
    }
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        if (!same_event(a.events[i], b.events[i])) {
            std::cerr << "event mismatch at " << i
                      << " cycle " << a.events[i].cycle
                      << " track " << a.events[i].track_id << "\n";
            return false;
        }
    }
    return true;
}

static RunResult run_guarded_dirty_parallel_case(bool enable_parallel) {
    const std::size_t slot_count = 64;
    std::vector<GuardedDirtySlot> slots(slot_count);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].value.raw_unsafe_for_initialization_only() = static_cast<std::uint32_t>(i);
        for (std::size_t j = 0; j < slots[i].values.size(); ++j) {
            slots[i].values[j] = static_cast<std::uint32_t>(i * 10u + j);
        }
    }
    GuardedDirtyTop top(&slots[0], slots.size());

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_parallel_topology_expansion = enable_parallel;
    opt.topology_expansion_threads = 4;
    opt.parallel_topology_min_elements = 2;
    opt.parallel_topology_min_work_items_per_element = 0;
    opt.parallel_topology_batch_elements = slot_count;
    opt.dump_leaf_distribution_after_topology = false;

    RunResult out;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("top", &top);
        tracer.sample(0);
        slots[7].value = 7007u;
        slots[7].values[2] = 702u;
        tracer.sample(1);
        out.parallel_topology_elements = tracer.parallel_topology_expanded_elements();
        out.parallel_topology_batches = tracer.parallel_topology_batches();
        out.parallel_topology_fallback_batches = tracer.parallel_topology_fallback_batches();
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    out.nodes = sink.node_declarations;
    out.tracks = sink.declarations;
    out.events = snapshot_events(sink.events);
    out.top_visits = 0;
    out.slot_visits = 0;
    out.payload_visits = 0;
    out.first_sample_ms = 0;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

static bool verify_guarded_dirty_parallel_merge() {
    const RunResult serial = run_guarded_dirty_parallel_case(false);
    const RunResult guarded = run_guarded_dirty_parallel_case(true);
    if (!compare_results(serial, guarded)) {
        std::cerr << "parallel dirty-source merge changed topology or samples\n";
        std::cerr << "serial events=" << serial.events.size()
                  << " parallel events=" << guarded.events.size() << "\n";
        const std::size_t serial_begin = serial.events.size() > 8 ? serial.events.size() - 8 : 0;
        const std::size_t parallel_begin = guarded.events.size() > 8 ? guarded.events.size() - 8 : 0;
        for (std::size_t i = serial_begin; i < serial.events.size(); ++i) {
            std::cerr << "serial tail i=" << i << " cycle=" << serial.events[i].cycle
                      << " track=" << serial.events[i].track_id << "\n";
        }
        for (std::size_t i = parallel_begin; i < guarded.events.size(); ++i) {
            std::cerr << "parallel tail i=" << i << " cycle=" << guarded.events[i].cycle
                      << " track=" << guarded.events[i].track_id << "\n";
        }
        return false;
    }
    if (guarded.parallel_topology_elements != 63 ||
        guarded.parallel_topology_batches != 1 ||
        guarded.parallel_topology_fallback_batches != 0) {
        std::cerr << "guarded dirty-source parallel counters are wrong: elements="
                  << guarded.parallel_topology_elements
                  << " batches=" << guarded.parallel_topology_batches
                  << " fallbacks=" << guarded.parallel_topology_fallback_batches << "\n";
        return false;
    }
    return true;
}

static RunResult run_guarded_hook_parallel_case(bool enable_parallel, bool& hook_bound) {
    const std::size_t slot_count = 64;
    std::vector<GuardedHookSlot> slots(slot_count);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].dynamic.value = static_cast<std::uint32_t>(i);
    }
    GuardedHookTop top(&slots[0], slots.size());

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_parallel_topology_expansion = enable_parallel;
    opt.topology_expansion_threads = 4;
    opt.parallel_topology_min_elements = 2;
    opt.parallel_topology_min_work_items_per_element = 0;
    opt.parallel_topology_batch_elements = slot_count;
    opt.enable_dirty_peek_groups = true;
    opt.enable_dynamic_dirty_groups = true;
    opt.dump_leaf_distribution_after_topology = false;

    RunResult out;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("top", &top);
        tracer.sample(0);
        tracer.attach_current_thread_for_dirty_peek();
        const wave::WaveDirtyHook* hook = slots[7].dynamic.wave_dirty_hook();
        hook_bound = hook && hook->tracer == static_cast<void*>(std::addressof(tracer)) &&
                     hook->mark_fn != NULL;
        slots[7].dynamic.write(7007u);
        tracer.sample(1);
        out.parallel_topology_elements = tracer.parallel_topology_expanded_elements();
        out.parallel_topology_batches = tracer.parallel_topology_batches();
        out.parallel_topology_fallback_batches = tracer.parallel_topology_fallback_batches();
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    out.nodes = sink.node_declarations;
    out.tracks = sink.declarations;
    out.events = snapshot_events(sink.events);
    out.top_visits = 0;
    out.slot_visits = 0;
    out.payload_visits = 0;
    out.first_sample_ms = 0;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

static bool verify_guarded_hook_parallel_merge() {
    bool serial_hook_bound = false;
    bool guarded_hook_bound = false;
    const RunResult serial = run_guarded_hook_parallel_case(false, serial_hook_bound);
    const RunResult guarded = run_guarded_hook_parallel_case(true, guarded_hook_bound);
    if (!serial_hook_bound || !guarded_hook_bound) {
        std::cerr << "dynamic dirty hook was not bound after parallel merge: serial="
                  << (serial_hook_bound ? 1 : 0)
                  << " guarded=" << (guarded_hook_bound ? 1 : 0)
                  << " serial_tracks=" << serial.tracks.size()
                  << " guarded_tracks=" << guarded.tracks.size()
                  << " guarded_fallbacks=" << guarded.parallel_topology_fallback_batches << "\n";
        return false;
    }
    if (!compare_results(serial, guarded)) {
        std::cerr << "parallel dynamic-hook merge changed topology or samples: elements="
                  << guarded.parallel_topology_elements
                  << " batches=" << guarded.parallel_topology_batches
                  << " fallbacks=" << guarded.parallel_topology_fallback_batches
                  << " serial_events=" << serial.events.size()
                  << " guarded_events=" << guarded.events.size() << "\n";
        return false;
    }
    if (guarded.parallel_topology_elements != 63 ||
        guarded.parallel_topology_batches != 1 ||
        guarded.parallel_topology_fallback_batches != 0) {
        std::cerr << "guarded dynamic-hook parallel counters are wrong\n";
        return false;
    }
    return true;
}

static RunResult run_wide_parallel_case(bool enable_parallel) {
    const std::size_t slot_count = 2048;
    std::vector<WideSlot> slots(slot_count);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        for (std::size_t j = 0; j < slots[i].values.size(); ++j) {
            slots[i].values[j] = static_cast<std::uint32_t>(i * 131u + j);
        }
    }
    WideTop top(&slots[0], slots.size());

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_parallel_topology_expansion = enable_parallel;
    opt.topology_expansion_threads = 8;
    opt.parallel_topology_min_elements = 2;
    opt.parallel_topology_batch_elements = slot_count;
    opt.dump_leaf_distribution_after_topology = false;

    RunResult out;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("top", &top);
        tracer.sample(0);
        for (std::size_t i = 0; i < slots.size(); i += 97u) {
            slots[i].values[(i / 97u) % slots[i].values.size()] ^= 0x5a5a5a5au;
        }
        tracer.sample(1);
        out.parallel_topology_elements = tracer.parallel_topology_expanded_elements();
        out.parallel_topology_batches = tracer.parallel_topology_batches();
        out.parallel_topology_fallback_batches = tracer.parallel_topology_fallback_batches();
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    out.nodes = sink.node_declarations;
    out.tracks = sink.declarations;
    out.events = snapshot_events(sink.events);
    out.top_visits = 0;
    out.slot_visits = 0;
    out.payload_visits = 0;
    out.first_sample_ms = 0;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

static bool verify_wide_parallel_topology() {
    const RunResult serial = run_wide_parallel_case(false);
    const RunResult parallel = run_wide_parallel_case(true);
    if (!compare_results(serial, parallel)) {
        std::cerr << "wide reflected array parallel expansion changed topology or samples\n";
        return false;
    }
    if (parallel.parallel_topology_elements != 2047u ||
        parallel.parallel_topology_batches != 1u ||
        parallel.parallel_topology_fallback_batches != 0u) {
        std::cerr << "wide reflected array did not use the expected parallel path\n";
        return false;
    }
    return true;
}

static bool verify_runtime_name_buffer_cache() {
    RuntimeNamedFields value;
    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.dump_leaf_distribution_after_topology = false;
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("runtime_names", &value);
        tracer.sample(0);
    }

    bool seen[4] = {false, false, false, false};
    std::size_t leaf_count = 0;
    for (std::size_t i = 0; i < sink.node_declarations.size(); ++i) {
        const wave::NodeDecl& node = sink.node_declarations[i];
        if (node.kind != wave::NodeKind::Leaf) continue;
        ++leaf_count;
        if (node.name.size() != 2u || node.name[0] != 'f' ||
            node.name[1] < '0' || node.name[1] > '3') {
            std::cerr << "runtime name buffer produced bad node name: " << node.name << "\n";
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(node.name[1] - '0');
        if (seen[index]) {
            std::cerr << "runtime name buffer collapsed duplicate node name: " << node.name << "\n";
            return false;
        }
        seen[index] = true;
    }
    if (leaf_count != 4u || !seen[0] || !seen[1] || !seen[2] || !seen[3]) {
        std::cerr << "runtime name buffer node set incomplete\n";
        return false;
    }
    return true;
}

static CountRunResult run_wide_topology_count(std::size_t slot_count,
                                              bool enable_parallel,
                                              std::size_t threads) {
    std::vector<WideSlot> slots(slot_count);
    WideTop top(slots.empty() ? static_cast<WideSlot*>(NULL) : &slots[0], slots.size());
    CountingWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = false;
    opt.enable_parallel_topology_expansion = enable_parallel;
    opt.topology_expansion_threads = threads;
    opt.dump_leaf_distribution_after_topology = false;

    CountRunResult out;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    {
        wave::Tracer tracer(sink, opt);
        tracer.add_root("top", &top);
        tracer.prepare_topology(0);
        out.parallel_topology_elements = tracer.parallel_topology_expanded_elements();
        out.parallel_topology_batches = tracer.parallel_topology_batches();
        out.parallel_topology_fallback_batches = tracer.parallel_topology_fallback_batches();
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    out.nodes = sink.node_count;
    out.tracks = sink.track_count;
    out.events = sink.event_count;
    out.top_visits = 0;
    out.slot_visits = 0;
    out.payload_visits = 0;
    out.first_sample_ms = 0;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

int main(int argc, char** argv) {
    std::size_t slot_count = 20000;
    std::uint32_t cycles = 12;
    std::string mode = "compare";
    if (argc >= 2) slot_count = static_cast<std::size_t>(std::strtoull(argv[1], NULL, 10));
    if (argc >= 3) cycles = static_cast<std::uint32_t>(std::strtoul(argv[2], NULL, 10));
    if (argc >= 4) mode = argv[3];
    if (slot_count == 0) {
        std::cerr << "usage: smoke_reflect_topology_cache_stress [slot_count] [cycles] "
                     "[compare|baseline|optimized|baseline-count|optimized-count|optimized-parallel-count|phase-count|wide-topology-count] [threads]\n";
        return 2;
    }

    if (mode == "wide-topology-count") {
        const std::size_t threads = argc >= 5
            ? static_cast<std::size_t>(std::strtoull(argv[4], NULL, 10))
            : static_cast<std::size_t>(16);
        const bool parallel = std::getenv("WAVE_STRESS_DISABLE_PARALLEL_TOPOLOGY") == NULL;
        const CountRunResult result = run_wide_topology_count(slot_count, parallel, threads);
        std::cout << "reflect_topology_wide_count"
                  << " slots=" << slot_count
                  << " threads=" << threads
                  << " parallel=" << (parallel ? 1 : 0)
                  << " nodes=" << result.nodes
                  << " tracks=" << result.tracks
                  << " elapsed_ms=" << result.elapsed_ms
                  << " parallel_topology_elements=" << result.parallel_topology_elements
                  << " parallel_topology_batches=" << result.parallel_topology_batches
                  << " parallel_topology_fallback_batches=" << result.parallel_topology_fallback_batches
                  << "\n";
        return 0;
    }

    if (mode == "baseline-count" || mode == "optimized-count" || mode == "optimized-parallel-count" || mode == "phase-count") {
        const bool fast_path = (mode != "baseline-count");
        const bool parallel_sampling = (mode == "optimized-parallel-count" || mode == "phase-count");
        const bool dump_phase_memory = (mode == "phase-count");
        const std::size_t sampling_threads = argc >= 5
            ? static_cast<std::size_t>(std::strtoull(argv[4], NULL, 10))
            : static_cast<std::size_t>(0);
        const CountRunResult result = run_count_case(slot_count, cycles, fast_path, parallel_sampling, sampling_threads, dump_phase_memory);
        std::cout << "reflect_topology_batch_stress " << mode
                  << " slots=" << slot_count
                  << " cycles=" << cycles
                  << " sampling_threads=" << sampling_threads
                  << " nodes=" << result.nodes
                  << " tracks=" << result.tracks
                  << " events=" << result.events
                  << " first_sample_ms=" << result.first_sample_ms
                  << " elapsed_ms=" << result.elapsed_ms
                  << " top_visits=" << result.top_visits
                  << " slot_visits=" << result.slot_visits
                  << " payload_visits=" << result.payload_visits
                  << " parallel_topology_elements=" << result.parallel_topology_elements
                  << " parallel_topology_batches=" << result.parallel_topology_batches
                  << " parallel_topology_fallback_batches=" << result.parallel_topology_fallback_batches
                  << "\n";
        return 0;
    }

    if (mode == "baseline" || mode == "optimized") {
        const bool fast_path = (mode == "optimized");
        const RunResult result = run_case(slot_count, cycles, fast_path);
        std::cout << "reflect_topology_batch_stress " << mode
                  << " slots=" << slot_count
                  << " cycles=" << cycles
                  << " nodes=" << result.nodes.size()
                  << " tracks=" << result.tracks.size()
                  << " events=" << result.events.size()
                  << " first_sample_ms=" << result.first_sample_ms
                  << " elapsed_ms=" << result.elapsed_ms
                  << " top_visits=" << result.top_visits
                  << " slot_visits=" << result.slot_visits
                  << " payload_visits=" << result.payload_visits
                  << " parallel_topology_elements=" << result.parallel_topology_elements
                  << " parallel_topology_batches=" << result.parallel_topology_batches
                  << " parallel_topology_fallback_batches=" << result.parallel_topology_fallback_batches
                  << "\n";
        return 0;
    }

    if (mode != "compare") {
        std::cerr << "unknown mode: " << mode << "\n";
        return 2;
    }

    const RunResult baseline = run_case(slot_count, cycles, false);
    const RunResult optimized = run_case(slot_count, cycles, true);
    if (!compare_results(baseline, optimized)) return 1;
    if (!verify_guarded_dirty_parallel_merge()) return 1;
    if (!verify_guarded_hook_parallel_merge()) return 1;
    if (!verify_wide_parallel_topology()) return 1;
    if (!verify_runtime_name_buffer_cache()) return 1;

    const double speedup = optimized.elapsed_ms == 0
        ? 0.0
        : static_cast<double>(baseline.elapsed_ms) / static_cast<double>(optimized.elapsed_ms);

    std::cout << "reflect_topology_batch_stress ok"
              << " slots=" << slot_count
              << " cycles=" << cycles
              << " nodes=" << optimized.nodes.size()
              << " tracks=" << optimized.tracks.size()
              << " events=" << optimized.events.size()
              << " baseline_first_sample_ms=" << baseline.first_sample_ms
              << " optimized_first_sample_ms=" << optimized.first_sample_ms
              << " baseline_ms=" << baseline.elapsed_ms
              << " optimized_ms=" << optimized.elapsed_ms
              << " speedup=" << speedup
              << " baseline_slot_visits=" << baseline.slot_visits
              << " optimized_slot_visits=" << optimized.slot_visits
              << " baseline_payload_visits=" << baseline.payload_visits
              << " optimized_payload_visits=" << optimized.payload_visits
              << " parallel_topology_elements=" << optimized.parallel_topology_elements
              << " parallel_topology_batches=" << optimized.parallel_topology_batches
              << " parallel_topology_fallback_batches=" << optimized.parallel_topology_fallback_batches
              << "\n";
    return 0;
}
