#include "wave_runtime.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

typedef unsigned char U01;

static std::size_t g_top_visits = 0;
static std::size_t g_slot_visits = 0;
static std::size_t g_payload_visits = 0;

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

struct Top {
    wave::WavePtr<Slot*> slots;

    Top(Slot* data, std::size_t count) : slots(data) {
        slots.declareSize(count);
    }
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
        on_ptr("slots", std::addressof(obj->slots));
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
    opt.dump_leaf_distribution_after_topology = false;

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::uint64_t first_sample_ms = 0;
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
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    RunResult out;
    out.nodes = sink.node_declarations;
    out.tracks = sink.declarations;
    out.events = snapshot_events(sink.events);
    out.top_visits = g_top_visits;
    out.slot_visits = g_slot_visits;
    out.payload_visits = g_payload_visits;
    out.first_sample_ms = first_sample_ms;
    out.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

static CountRunResult run_count_case(std::size_t slot_count,
                                     std::uint32_t cycles,
                                     bool enable_topology_fast_path) {
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
    opt.dump_leaf_distribution_after_topology = false;

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::uint64_t first_sample_ms = 0;
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
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    CountRunResult out;
    out.nodes = sink.node_count;
    out.tracks = sink.track_count;
    out.events = sink.event_count;
    out.top_visits = g_top_visits;
    out.slot_visits = g_slot_visits;
    out.payload_visits = g_payload_visits;
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

int main(int argc, char** argv) {
    std::size_t slot_count = 20000;
    std::uint32_t cycles = 12;
    std::string mode = "compare";
    if (argc >= 2) slot_count = static_cast<std::size_t>(std::strtoull(argv[1], NULL, 10));
    if (argc >= 3) cycles = static_cast<std::uint32_t>(std::strtoul(argv[2], NULL, 10));
    if (argc >= 4) mode = argv[3];
    if (slot_count == 0) {
        std::cerr << "usage: smoke_reflect_topology_cache_stress [slot_count] [cycles] "
                     "[compare|baseline|optimized|baseline-count|optimized-count]\n";
        return 2;
    }

    if (mode == "baseline-count" || mode == "optimized-count") {
        const bool fast_path = (mode == "optimized-count");
        const CountRunResult result = run_count_case(slot_count, cycles, fast_path);
        std::cout << "reflect_topology_batch_stress " << mode
                  << " slots=" << slot_count
                  << " cycles=" << cycles
                  << " nodes=" << result.nodes
                  << " tracks=" << result.tracks
                  << " events=" << result.events
                  << " first_sample_ms=" << result.first_sample_ms
                  << " elapsed_ms=" << result.elapsed_ms
                  << " top_visits=" << result.top_visits
                  << " slot_visits=" << result.slot_visits
                  << " payload_visits=" << result.payload_visits
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
              << "\n";
    return 0;
}
