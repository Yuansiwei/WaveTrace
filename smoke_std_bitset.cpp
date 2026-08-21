#include "wave_runtime.h"

#include <bitset>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct BitsetRoot {
    std::uint32_t before = 0;
    std::bitset<5> small;
    std::bitset<130> wide;
    std::uint32_t after = 0;
};

struct BitsetPayload {
    std::uint32_t before = 0;
    std::bitset<70> bits;
    std::uint32_t after = 0;
};

struct BitsetPeek : wave::PeekTraceSourceFor<BitsetPeek, BitsetPayload> {
    BitsetPayload payload;
    const BitsetPayload* peek() const noexcept { return &payload; }
    void mark_dirty() { wave_dirty_hook()->mark_dirty(); }
};

struct BitsetPeekRoot { BitsetPeek source; };
struct BitsetArrayRoot { wave::array<BitsetPayload, 2> bits; };

int fail(const char* message) {
    std::cerr << message << "\n";
    return 2;
}

const wave::TrackEvent* find_event(const wave::InMemoryWaveSink& sink,
                                   wave::TrackId trackId,
                                   wave::Cycle cycle) {
    for (const wave::TrackEvent& event : sink.events) {
        if (event.track_id == trackId && event.cycle == cycle) return &event;
    }
    return nullptr;
}

std::size_t event_count_at(const wave::InMemoryWaveSink& sink, wave::Cycle cycle) {
    std::size_t count = 0;
    for (const wave::TrackEvent& event : sink.events) {
        if (event.cycle == cycle) ++count;
    }
    return count;
}

bool validate_bitset_decl(const wave::InMemoryWaveSink& sink,
                          std::size_t index,
                          std::uint32_t bits,
                          std::uint32_t words) {
    if (index >= sink.bitset_declarations.size()) return false;
    const wave::BitsetDecl& decl = sink.bitset_declarations[index];
    if (decl.node_id == 0 || decl.first_storage_id == 0 ||
        decl.bit_count != bits || decl.word_count != words) return false;
    for (std::uint32_t word = 0; word < words; ++word) {
        const wave::TrackId id = decl.first_storage_id + word;
        if (id >= sink.declarations.size() + 1u) return false;
        const wave::TrackDecl& track = sink.declarations[std::size_t(id - 1u)];
        if (track.track_id != id || track.storage_id != id || track.node_id != 0 ||
            track.kind != wave::ValueKind::UnsignedInt || track.bit_width != 64u ||
            track.bit_offset != 0u || !track.storage_only) return false;
    }
    return true;
}

int run_flat_case(bool firstOnly) {
    BitsetRoot root;
    root.before = 11u;
    root.small.set(0).set(3);
    root.wide.set(0).set(64).set(129);
    root.after = 22u;

    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.emit_only_on_change = true;
    options.flat_memory_block_min_leaf_count = 1u;
    options.trace_array_first_element_only = firstOnly;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);
    tracer.sample(0);

    // before + small word + three wide words + after. No per-bit tracks.
    if (sink.declarations.size() != 6u || sink.bitset_declarations.size() != 2u ||
        !validate_bitset_decl(sink, 0, 5u, 1u) ||
        !validate_bitset_decl(sink, 1, 130u, 3u)) {
        return fail("std::bitset was not represented as canonical hidden U64 storage");
    }
    const wave::TrackId small = sink.bitset_declarations[0].first_storage_id;
    const wave::TrackId wide = sink.bitset_declarations[1].first_storage_id;
    const wave::TrackEvent* small0 = find_event(sink, small, 0);
    const wave::TrackEvent* wide0 = find_event(sink, wide, 0);
    const wave::TrackEvent* wide1 = find_event(sink, wide + 1u, 0);
    const wave::TrackEvent* wide2 = find_event(sink, wide + 2u, 0);
    if (!small0 || !small0->has_u64 || small0->u64_value != 0x9u ||
        !wide0 || wide0->u64_value != 0x1u ||
        !wide1 || wide1->u64_value != 0x1u ||
        !wide2 || wide2->u64_value != 0x2u) {
        return fail("std::bitset raw-native word values are incorrect");
    }
    if (firstOnly) return 0;

    sink.events.clear();
    root.wide.flip(64);
    root.wide.set(65);
    tracer.sample(1);
    const wave::TrackEvent* changedWord = find_event(sink, wide + 1u, 1);
    if (event_count_at(sink, 1) != 1u || !changedWord ||
        !changedWord->has_u64 || changedWord->u64_value != 0x2u) {
        return fail("std::bitset change did not stay in its owning word stream");
    }
    return 0;
}

int run_dirty_wave_array_case() {
    BitsetArrayRoot root;
    root.bits[0].bits.set(1);
    root.bits[1].bits.set(69);
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.enable_wave_array_dirty = true;
    options.enable_wave_array_memory_block_precheck = true;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);
    tracer.sample(0);
    if (sink.bitset_declarations.size() != 2u) {
        return fail("wave::array bitsets lost virtual-subtree metadata");
    }
    const wave::TrackId secondWord = sink.bitset_declarations[1].first_storage_id + 1u;
    sink.events.clear();
    root.bits[1].bits.flip(69);
    tracer.sample(1);
    const wave::TrackEvent* event = find_event(sink, secondWord, 1);
    if (event_count_at(sink, 1) != 1u || !event || event->u64_value != 0u) {
        return fail("dirty wave::array bitset did not emit one changed word");
    }
    return 0;
}

int run_dirty_peek_case() {
    wave::DynamicTypeRegistration<BitsetPayload> registration;
    BitsetPeekRoot root;
    root.source.payload.bits.set(69);
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.enable_dirty_peek_groups = true;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);
    tracer.sample(0);
    if (sink.bitset_declarations.size() != 1u) {
        return fail("dirty Peek bitset lost virtual-subtree metadata");
    }
    const wave::TrackId secondWord = sink.bitset_declarations[0].first_storage_id + 1u;
    sink.events.clear();
    root.source.payload.bits.flip(69);
    root.source.mark_dirty();
    tracer.sample(1);
    const wave::TrackEvent* event = find_event(sink, secondWord, 1);
    if (event_count_at(sink, 1) != 1u || !event || event->u64_value != 0u) {
        return fail("dirty Peek bitset did not emit one changed word");
    }
    return 0;
}

int run_leaf_report_case() {
    const char* path = "smoke_std_bitset_leaf_distribution.txt";
    std::remove(path);
    BitsetRoot root;
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.dump_leaf_distribution_after_topology = true;
    options.leaf_distribution_dump_path = path;
    options.dump_memory_usage_after_topology = false;
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);

    std::ifstream in(path, std::ios::in | std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    in.close();
    std::remove(path);
    if (text.find("bitset_raw_tracking=enabled") == std::string::npos ||
        text.find("host_endian=little") == std::string::npos ||
        text.find("fields_seen=2 fields_traced=2 fields_skipped_layout=0") == std::string::npos) {
        return fail("leaf distribution report lost std::bitset ABI status");
    }
    return 0;
}

} // namespace

namespace reflect {
template<> struct is_reflected<BitsetRoot> : std::true_type {};
template<> struct reflected_visitor<BitsetRoot> {
    template<class P, class V, class G>
    static void visit(const BitsetRoot* o, P&& p, V&&, G&&) {
        p("before", &o->before); p("small", &o->small);
        p("wide", &o->wide); p("after", &o->after);
    }
};
template<> struct is_reflected<BitsetPayload> : std::true_type {};
template<> struct reflected_visitor<BitsetPayload> {
    template<class P, class V, class G>
    static void visit(const BitsetPayload* o, P&& p, V&&, G&&) {
        p("before", &o->before); p("bits", &o->bits); p("after", &o->after);
    }
};
template<> struct is_reflected<BitsetPeekRoot> : std::true_type {};
template<> struct reflected_visitor<BitsetPeekRoot> {
    template<class P, class V, class G>
    static void visit(const BitsetPeekRoot* o, P&& p, V&&, G&&) { p("source", &o->source); }
};
template<> struct is_reflected<BitsetArrayRoot> : std::true_type {};
template<> struct reflected_visitor<BitsetArrayRoot> {
    template<class P, class V, class G>
    static void visit(const BitsetArrayRoot* o, P&& p, V&&, G&&) { p("bits", &o->bits); }
};
} // namespace reflect

int main() {
    if (int rc = run_flat_case(false)) return rc;
    if (int rc = run_flat_case(true)) return rc;
    if (int rc = run_dirty_wave_array_case()) return rc;
    if (int rc = run_dirty_peek_case()) return rc;
    if (int rc = run_leaf_report_case()) return rc;
    std::cout << "std_bitset_raw_native_ok\n";
    return 0;
}
