#include "wave_tap.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct ArrayMemberCell {
    std::uint32_t id = 0;
    wave::array<std::uint16_t, 3> samples;
};

#pragma pack(push, 1)
struct CrossPageCell {
    std::uint16_t prefix = 0;
    std::uint64_t value = 0;
};
#pragma pack(pop)

static const std::size_t kCrossPageCount = 60000;

struct NestedWaveArrayTop {
    std::uint32_t scalar = 0;
    wave::array<wave::array<std::uint32_t, 2>, 2> matrix;
    wave::array<wave::array<wave::array<std::uint32_t, 2>, 2>, 2> cube;
    wave::array<ArrayMemberCell, 2> cells;
    wave::array<CrossPageCell, kCrossPageCount> cross_page;
};

struct ArrayOnlyTop {
    wave::array<std::uint64_t, 8193> values;
};

struct BoolStorageLeaf {
    std::uint8_t status = 0;
    std::uint32_t inline_value = 9u;
    const std::uint32_t* pointer;
    std::shared_ptr<std::uint32_t> smart_pointer;
    const std::uint32_t& reference;

    BoolStorageLeaf() : pointer(&inline_value), reference(inline_value) {}
};

struct BoolStorageArrayCell {
    BoolStorageLeaf direct;
    wave::array<BoolStorageLeaf, 2> children;
};

struct BoolStorageArrayTop {
    wave::array<wave::array<BoolStorageArrayCell, 2>, 2> cells;
};

struct UnreflectedPayload {
    std::uint32_t value = 0;
};

struct AllSkippedLeaf {
    std::uint32_t inline_value = 1u;
    std::uint32_t* raw_pointer;
    std::uint32_t* annotated_pointer;
    std::uint32_t& reference;
    std::unique_ptr<std::uint32_t> unique_pointer;
    std::shared_ptr<std::uint32_t> shared_pointer;
    std::weak_ptr<std::uint32_t> weak_pointer;
    std::vector<std::uint32_t> dynamic_values;
    std::string text;
    std::array<std::uint32_t, 2> std_array;
    UnreflectedPayload unreflected;

    AllSkippedLeaf()
        : raw_pointer(&inline_value),
          annotated_pointer(&inline_value),
          reference(inline_value),
          unique_pointer(new std::uint32_t(2u)),
          shared_pointer(new std::uint32_t(3u)),
          weak_pointer(shared_pointer),
          dynamic_values(3u, 4u),
          text("skip"),
          std_array{{5u, 6u}} {}
};

struct AllSkippedNested {
    AllSkippedLeaf direct;
    wave::array<AllSkippedLeaf, 2> children;
};

struct MixedSchemaLeaf {
    std::uint32_t visible = 7u;
    AllSkippedLeaf skipped_object;
    wave::array<AllSkippedLeaf, 2> skipped_array;
};

namespace reflect {
template<> struct is_reflected<ArrayMemberCell> : std::true_type {};
template<> struct reflected_visitor<ArrayMemberCell> {
    template<class P, class V, class G>
    static void visit(const ArrayMemberCell* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("id", std::addressof(obj->id));
        on_ptr("samples", std::addressof(obj->samples));
    }
};
template<> struct is_reflected<CrossPageCell> : std::true_type {};
template<> struct reflected_visitor<CrossPageCell> {
    template<class P, class V, class G>
    static void visit(const CrossPageCell* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("prefix", std::addressof(obj->prefix));
        on_ptr("value", std::addressof(obj->value));
    }
};
template<> struct is_reflected<NestedWaveArrayTop> : std::true_type {};
template<> struct reflected_visitor<NestedWaveArrayTop> {
    template<class P, class V, class G>
    static void visit(const NestedWaveArrayTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("scalar", std::addressof(obj->scalar));
        on_ptr("matrix", std::addressof(obj->matrix));
        on_ptr("cube", std::addressof(obj->cube));
        on_ptr("cells", std::addressof(obj->cells));
        on_ptr("cross_page", std::addressof(obj->cross_page));
    }
};
template<> struct is_reflected<ArrayOnlyTop> : std::true_type {};
template<> struct reflected_visitor<ArrayOnlyTop> {
    template<class P, class V, class G>
    static void visit(const ArrayOnlyTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("values", std::addressof(obj->values));
    }
};
template<> struct is_reflected<BoolStorageLeaf> : std::true_type {};
template<> struct reflected_visitor<BoolStorageLeaf> {
    template<class P, class V, class G>
    static void visit(const BoolStorageLeaf* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("status", wave::as_bool_storage_ptr(std::addressof(obj->status)));
        on_ptr("pointer", std::addressof(obj->pointer),
               wave::detail::PointerOrReferenceFieldTag());
        on_ptr("smart_pointer", std::addressof(obj->smart_pointer));
        on_ptr("reference", std::addressof(obj->reference),
               wave::detail::PointerOrReferenceFieldTag());
    }
};
template<> struct is_reflected<BoolStorageArrayCell> : std::true_type {};
template<> struct reflected_visitor<BoolStorageArrayCell> {
    template<class P, class V, class G>
    static void visit(const BoolStorageArrayCell* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("direct", std::addressof(obj->direct));
        on_ptr("children", std::addressof(obj->children));
    }
};
template<> struct is_reflected<BoolStorageArrayTop> : std::true_type {};
template<> struct reflected_visitor<BoolStorageArrayTop> {
    template<class P, class V, class G>
    static void visit(const BoolStorageArrayTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("cells", std::addressof(obj->cells));
    }
};
template<> struct is_reflected<AllSkippedLeaf> : std::true_type {};
template<> struct reflected_visitor<AllSkippedLeaf> {
    template<class P, class V, class G>
    static void visit(const AllSkippedLeaf* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("raw_pointer", std::addressof(obj->raw_pointer),
               wave::detail::PointerOrReferenceFieldTag());
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "annotated_pointer", obj->annotated_pointer, 1u,
            wave::detail::AnnotatedPointerMemberKey("AllSkippedLeaf", "annotated_pointer"));
        on_ptr("reference", std::addressof(obj->reference),
               wave::detail::PointerOrReferenceFieldTag());
        on_ptr("unique_pointer", std::addressof(obj->unique_pointer));
        on_ptr("shared_pointer", std::addressof(obj->shared_pointer));
        on_ptr("weak_pointer", std::addressof(obj->weak_pointer));
        on_ptr("dynamic_values", std::addressof(obj->dynamic_values));
        on_ptr("text", std::addressof(obj->text));
        on_ptr("std_array", std::addressof(obj->std_array));
        on_ptr("unreflected", std::addressof(obj->unreflected));
    }
};
template<> struct is_reflected<AllSkippedNested> : std::true_type {};
template<> struct reflected_visitor<AllSkippedNested> {
    template<class P, class V, class G>
    static void visit(const AllSkippedNested* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("direct", std::addressof(obj->direct));
        on_ptr("children", std::addressof(obj->children));
    }
};
template<> struct is_reflected<MixedSchemaLeaf> : std::true_type {};
template<> struct reflected_visitor<MixedSchemaLeaf> {
    template<class P, class V, class G>
    static void visit(const MixedSchemaLeaf* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("visible", std::addressof(obj->visible));
        on_ptr("skipped_object", std::addressof(obj->skipped_object));
        on_ptr("skipped_array", std::addressof(obj->skipped_array));
    }
};
}

class NestedWaveArrayPathRecorder : public PathStableWvz4Recorder {
public:
    std::set<std::string> declared_paths;
    std::vector<wave::ArrayBlockDecl> arrays;

    void on_array_block_declared(const wave::ArrayBlockDecl& decl) override {
        arrays.push_back(decl);
        PathStableWvz4Recorder::on_array_block_declared(decl);
    }

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        declared_paths.insert(path);
        PathStableWvz4Recorder::on_track_declared_fast(
            track_id,
            storage_id,
            node_id,
            kind,
            bit_width,
            bit_offset,
            storage_only,
            path);
    }
};

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

static int verifyBoolStorageCompactArray() {
    BoolStorageArrayTop top;
    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    wave::Tracer tracer(sink, options);
    tracer.add_root("bool_storage_top", &top);
    tracer.prepare_topology();
    if (tracer.compact_array_block_count() != 0u ||
        !sink.array_block_declarations.empty()) {
        return fail("array with external-address members must not use a partial compact schema");
    }
    bool saw_nested_bool = false;
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        if (sink.declarations[i].path ==
            "bool_storage_top.cells.[1].[1].children.[1].status") {
            saw_nested_bool = sink.declarations[i].kind == wave::ValueKind::Bool &&
                sink.declarations[i].bit_width == 1u;
        }
    }
    if (!saw_nested_bool) {
        return fail("recursive wave::array fallback lost nested bool storage");
    }

    wave::array<const std::uint32_t*, 2> pointers;
    wave::InMemoryWaveSink pointer_sink;
    wave::Tracer pointer_tracer(pointer_sink);
    pointer_tracer.add_root("pointer_only", &pointers);
    pointer_tracer.prepare_topology();
    if (pointer_tracer.compact_array_block_count() != 0u ||
        !pointer_sink.array_block_declarations.empty()) {
        return fail("pointer-only wave::array was not skipped");
    }

    wave::array<AllSkippedNested, 4> all_skipped;
    wave::InMemoryWaveSink all_skipped_sink;
    wave::Tracer all_skipped_tracer(all_skipped_sink);
    all_skipped_tracer.add_root("all_skipped", &all_skipped);
    all_skipped_tracer.prepare_topology();
    if (all_skipped_tracer.compact_array_block_count() != 0u ||
        !all_skipped_sink.array_block_declarations.empty()) {
        return fail("all-skipped nested wave::array produced a compact declaration");
    }

    wave::array<MixedSchemaLeaf, 8> mixed;
    wave::InMemoryWaveSink mixed_sink;
    wave::BuildOptions mixed_options;
    mixed_options.emit_track_decl_path = true;
    wave::Tracer mixed_tracer(mixed_sink, mixed_options);
    mixed_tracer.add_root("mixed", &mixed);
    mixed_tracer.prepare_topology();
    if (mixed_tracer.compact_array_block_count() != 0u ||
        !mixed_sink.array_block_declarations.empty()) {
        return fail("mixed supported/unsupported wave::array used a partial compact schema");
    }
    bool saw_last_visible = false;
    for (std::size_t i = 0; i < mixed_sink.declarations.size(); ++i) {
        if (mixed_sink.declarations[i].path == "mixed.[7].visible") {
            saw_last_visible = true;
        }
    }
    if (!saw_last_visible) {
        return fail("recursive mixed wave::array fallback lost supported members");
    }
    return 0;
}

struct StressKey {
    int array_index = -1;
    std::size_t leaf_index = 0;

    bool operator<(const StressKey& rhs) const noexcept {
        return array_index != rhs.array_index
            ? array_index < rhs.array_index
            : leaf_index < rhs.leaf_index;
    }
};

struct StressEvent {
    StressKey key;
    std::uint64_t time = 0;
    std::uint64_t value = 0;
};

static std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static std::uint64_t nextRandom(std::uint64_t& state) noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dull;
}

static std::vector<StressKey> stressKeys() {
    std::vector<StressKey> keys;
    keys.push_back(StressKey{-1, 1});
    for (std::size_t i = 0; i < 4; ++i) keys.push_back(StressKey{0, i});
    for (std::size_t i = 0; i < 8; ++i) keys.push_back(StressKey{1, i});
    for (std::size_t i = 0; i < 8; ++i) keys.push_back(StressKey{2, i});

    std::set<std::size_t> cross_indices = {0, 1, 2, 6552, 6553, 6554, kCrossPageCount - 1};
    for (std::size_t i = 37; i < kCrossPageCount; i += 997) cross_indices.insert(i);
    for (std::size_t i : cross_indices) {
        keys.push_back(StressKey{3, i * 2});
        keys.push_back(StressKey{3, i * 2 + 1});
    }
    return keys;
}

static std::uint64_t stressValue(const NestedWaveArrayTop& top, const StressKey& key) {
    if (key.array_index < 0) return top.scalar;
    if (key.array_index == 0) {
        return top.matrix.read(key.leaf_index / 2).read(key.leaf_index % 2);
    }
    if (key.array_index == 1) {
        const std::size_t outer = key.leaf_index / 4;
        const std::size_t rem = key.leaf_index % 4;
        return top.cube.read(outer).read(rem / 2).read(rem % 2);
    }
    if (key.array_index == 2) {
        const ArrayMemberCell& cell = top.cells.read(key.leaf_index / 4);
        const std::size_t member = key.leaf_index % 4;
        return member == 0 ? cell.id : cell.samples.read(member - 1);
    }
    const CrossPageCell& cell = top.cross_page.read(key.leaf_index / 2);
    return (key.leaf_index % 2) == 0 ? cell.prefix : cell.value;
}

static void captureStressSnapshot(const NestedWaveArrayTop& top,
                                  std::uint64_t time,
                                  const std::vector<StressKey>& keys,
                                  std::map<StressKey, std::uint64_t>& previous,
                                  std::vector<StressEvent>& events) {
    for (const StressKey& key : keys) {
        const std::uint64_t value = stressValue(top, key);
        const auto found = previous.find(key);
        if (found == previous.end() || found->second != value) {
            events.push_back(StressEvent{key, time, value});
            previous[key] = value;
        }
    }
}

static bool writeStressOracle(const std::string& path,
                              std::uint64_t seed,
                              std::size_t cycles,
                              const std::vector<StressEvent>& events,
                              std::string& error) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out) {
        error = "cannot open stress oracle: " + path;
        return false;
    }
    out << "WAVE_ARRAY_STRESS_V1 " << seed << ' ' << cycles << ' ' << events.size() << '\n';
    for (const StressEvent& event : events) {
        out << event.key.array_index << ' ' << event.key.leaf_index << ' '
            << event.time << ' ' << event.value << '\n';
    }
    if (!out) {
        error = "cannot write stress oracle: " + path;
        return false;
    }
    return true;
}

static void initializeStressTop(NestedWaveArrayTop& top, std::uint64_t seed) {
    top.scalar = static_cast<std::uint32_t>(mix64(seed));
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            top.matrix[i][j] = static_cast<std::uint32_t>(mix64(seed + i * 11 + j));
            for (std::size_t k = 0; k < 2; ++k) {
                top.cube[i][j][k] = static_cast<std::uint32_t>(
                    mix64(seed + 100 + i * 17 + j * 5 + k));
            }
        }
        top.cells[i].id = static_cast<std::uint32_t>(mix64(seed + 200 + i));
        for (std::size_t j = 0; j < 3; ++j) {
            top.cells[i].samples[j] = static_cast<std::uint16_t>(mix64(seed + 300 + i * 7 + j));
        }
    }
    CrossPageCell* raw = top.cross_page.data();
    for (std::size_t i = 0; i < top.cross_page.size(); ++i) {
        raw[i].prefix = static_cast<std::uint16_t>(mix64(seed + 1000 + i));
        raw[i].value = mix64(seed + 10000 + i);
    }
}

static void mutateStressCycle(NestedWaveArrayTop& top,
                              std::uint64_t seed,
                              std::size_t cycle) {
    if ((cycle % 13) == 0) return;
    std::uint64_t random = mix64(seed ^ (cycle * 0xd1342543de82ef95ull));
    CrossPageCell* raw = (cycle % 3) == 0 ? top.cross_page.data() : nullptr;

    for (std::size_t op = 0; op < 48; ++op) {
        const std::uint64_t word = nextRandom(random);
        const std::size_t choice = static_cast<std::size_t>(word % 10);
        const std::uint64_t value = mix64(word ^ cycle ^ (op << 8));
        if (choice < 6) {
            const std::size_t index = static_cast<std::size_t>(nextRandom(random) % kCrossPageCount);
            CrossPageCell& cell = raw ? raw[index] : top.cross_page[index];
            if ((word & 1u) != 0) cell.value = value;
            else cell.prefix = static_cast<std::uint16_t>(value);
        } else if (choice == 6) {
            const std::size_t row = static_cast<std::size_t>((word >> 8) & 1u);
            const std::size_t col = static_cast<std::size_t>((word >> 9) & 1u);
            top.matrix[row][col] = static_cast<std::uint32_t>(value);
        } else if (choice == 7) {
            const std::size_t i = static_cast<std::size_t>((word >> 8) & 1u);
            const std::size_t j = static_cast<std::size_t>((word >> 9) & 1u);
            const std::size_t k = static_cast<std::size_t>((word >> 10) & 1u);
            top.cube[i][j][k] = static_cast<std::uint32_t>(value);
        } else if (choice == 8) {
            top.cells[(word >> 8) & 1u].id = static_cast<std::uint32_t>(value);
        } else {
            top.cells[(word >> 8) & 1u].samples[(word >> 9) % 3] =
                static_cast<std::uint16_t>(value);
        }
    }

    const std::size_t repeated = static_cast<std::size_t>(nextRandom(random) % kCrossPageCount);
    top.cross_page[repeated].value = mix64(random ^ 0x1111u);
    top.cross_page[repeated].value = mix64(random ^ 0x2222u);

    if ((cycle % 7) == 0) top.scalar = static_cast<std::uint32_t>(nextRandom(random));
    if ((cycle % 17) == 0) {
        const std::size_t start = static_cast<std::size_t>(nextRandom(random) % (kCrossPageCount - 768));
        CrossPageCell* dense = top.cross_page.data();
        for (std::size_t i = 0; i < 768; ++i) {
            dense[start + i].prefix = static_cast<std::uint16_t>(mix64(seed + cycle + i));
            dense[start + i].value = mix64(seed ^ (cycle << 32) ^ i);
        }
    }
    if ((cycle % 19) == 0) top.cells[0].samples.swap(top.cells[1].samples);
    if ((cycle % 23) == 0) {
        const std::size_t row = static_cast<std::size_t>(nextRandom(random) & 1u);
        std::array<std::uint32_t, 2> replacement = {{
            static_cast<std::uint32_t>(nextRandom(random)),
            static_cast<std::uint32_t>(nextRandom(random))}};
        top.matrix[row] = replacement;
    }
    if ((cycle % 29) == 0) {
        top.cells[nextRandom(random) & 1u].samples.fill(static_cast<std::uint16_t>(nextRandom(random)));
    }
    if ((cycle % 31) == 0) {
        static const std::size_t boundary = 6553;
        CrossPageCell replacement;
        replacement.prefix = static_cast<std::uint16_t>(nextRandom(random));
        replacement.value = nextRandom(random);
        top.cross_page[boundary] = replacement;
    }
}

static wave::BuildOptions stressBuildOptions() {
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
    return opt;
}

static int runStress(const std::string& out_path,
                     std::uint64_t seed,
                     std::size_t cycles) {
    if (cycles == 0 || cycles > 10000) return fail("stress cycles must be in [1,10000]");
    std::unique_ptr<NestedWaveArrayTop> top(new NestedWaveArrayTop());
    initializeStressTop(*top, seed);

    NestedWaveArrayPathRecorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = (seed & 1u) != 0 ? wvz4::Compression::Zstd : wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 97;

    std::string error;
    if (!recorder.open(cfg, error)) return fail("stress recorder.open failed: " + error);
    wave::Tracer tracer(recorder, stressBuildOptions());
    tracer.add_root("top", top.get());
    wave::WaveTap tap(tracer, recorder);

    const std::vector<StressKey> keys = stressKeys();
    std::map<StressKey, std::uint64_t> previous;
    std::vector<StressEvent> events;
    events.reserve(keys.size() * (cycles / 3 + 2));
    if (!tap.sample_one_cycle()) return fail("stress cycle0 failed: " + tap.last_error());
    captureStressSnapshot(*top, 0, keys, previous, events);
    for (std::size_t cycle = 1; cycle <= cycles; ++cycle) {
        mutateStressCycle(*top, seed, cycle);
        if (!tap.sample_one_cycle()) {
            return fail("stress cycle " + std::to_string(cycle) + " failed: " + tap.last_error());
        }
        captureStressSnapshot(*top, cycle * 10, keys, previous, events);
    }
    if (!recorder.close(error)) return fail("stress recorder.close failed: " + error);
    if (recorder.declared_paths.size() != 1u || recorder.arrays.size() != 4u) {
        return fail("stress compact registration count mismatch");
    }
    if (!writeStressOracle(out_path + ".oracle", seed, cycles, events, error)) return fail(error);
    std::cout << "nested_wave_array_stress_writer_ok file=" << out_path
              << " seed=" << seed
              << " cycles=" << cycles
              << " oracle_events=" << events.size()
              << " compression=" << (((seed & 1u) != 0) ? "zstd" : "none") << "\n";
    return 0;
}

static int runArrayOnly(const std::string& out_path) {
    ArrayOnlyTop top;
    std::uint64_t* raw = top.values.data();
    for (std::size_t i = 0; i < top.values.size(); ++i) raw[i] = mix64(0x12340000u + i);

    NestedWaveArrayPathRecorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = wvz4::Compression::Zstd;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 31;

    std::string error;
    if (!recorder.open(cfg, error)) return fail("array-only recorder.open failed: " + error);
    wave::Tracer tracer(recorder, stressBuildOptions());
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);
    if (!tap.sample_one_cycle()) return fail("array-only cycle0 failed: " + tap.last_error());
    top.values[0] = 0x1111222233334444ull;
    if (!tap.sample_one_cycle()) return fail("array-only cycle1 failed: " + tap.last_error());
    raw = top.values.data();
    raw[8191] = 0x5555666677778888ull;
    raw[8192] = 0x9999aaaabbbbccccull;
    if (!tap.sample_one_cycle()) return fail("array-only cycle2 failed: " + tap.last_error());
    top.values.fill(0x55aa55aa55aa55aaull);
    if (!tap.sample_one_cycle()) return fail("array-only cycle3 failed: " + tap.last_error());
    if (!recorder.close(error)) return fail("array-only recorder.close failed: " + error);
    if (!recorder.declared_paths.empty() || recorder.arrays.size() != 1u ||
        recorder.arrays[0].element_count != 8193u || recorder.arrays[0].element_stride != 8u) {
        return fail("array-only compact registration mismatch");
    }
    std::cout << "nested_wave_array_array_only_writer_ok file=" << out_path
              << " arrays=1 tracks=0 cycles=4\n";
    return 0;
}

int main(int argc, char** argv) {
    static_assert(sizeof(CrossPageCell) == 10, "cross-page test requires packed layout");
    static_assert(offsetof(CrossPageCell, value) == 2, "cross-page leaf offset changed");
    static const std::size_t kCrossPageIndex = 6553;
    const int bool_storage_result = verifyBoolStorageCompactArray();
    if (bool_storage_result != 0) return bool_storage_result;
    const std::string out_path =
        (argc >= 2 && argv[1] && argv[1][0] != '\0')
            ? argv[1]
            : "build_vs\\nested_wave_array_wvz4_smoke.wvz4";
    if (argc >= 3 && std::string(argv[2]) == "--array-only") return runArrayOnly(out_path);
    if (argc >= 3 && std::string(argv[2]) == "--stress") {
        if (argc < 5) return fail("usage: writer <out.wvz4> --stress <seed> <cycles>");
        const std::uint64_t seed = static_cast<std::uint64_t>(std::strtoull(argv[3], nullptr, 0));
        const std::size_t cycles = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 0));
        return runStress(out_path, seed, cycles);
    }

    NestedWaveArrayTop top;
    top.scalar = 5u;
    top.matrix[0][0] = 0u;
    top.matrix[0][1] = 1u;
    top.matrix[1][0] = 10u;
    top.matrix[1][1] = 11u;
    top.cube[0][0][0] = 0u;
    top.cube[0][0][1] = 1u;
    top.cube[0][1][0] = 10u;
    top.cube[0][1][1] = 11u;
    top.cube[1][0][0] = 100u;
    top.cube[1][0][1] = 101u;
    top.cube[1][1][0] = 110u;
    top.cube[1][1][1] = 111u;
    top.cells[0].id = 7u;
    top.cells[0].samples[0] = 70u;
    top.cells[0].samples[1] = 71u;
    top.cells[0].samples[2] = 72u;
    top.cells[1].id = 8u;
    top.cells[1].samples[0] = 80u;
    top.cells[1].samples[1] = 81u;
    top.cells[1].samples[2] = 82u;
    top.cross_page[kCrossPageIndex].prefix = 0x1234u;
    top.cross_page[kCrossPageIndex].value = 0x0102030405060708ull;

    std::string error;
    NestedWaveArrayPathRecorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = out_path;
    cfg.emit_default_clk = false;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 256;

    if (!recorder.open(cfg, error)) {
        return fail("recorder.open failed: " + error);
    }

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

    wave::Tracer tracer(recorder, opt);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) return fail("cycle0 sample failed: " + tap.last_error());

    top.matrix[0][0] = 100u;
    top.cube[0][1][1] = 1111u;
    top.cells[1].samples[2] = 182u;
    if (!tap.sample_one_cycle()) return fail("cycle1 sample failed: " + tap.last_error());

    top.cross_page[kCrossPageIndex].value = 0x8877665544332211ull;
    if (!tap.sample_one_cycle()) return fail("cycle2 sample failed: " + tap.last_error());

    top.matrix[1].data()[1] = 211u;
    top.cube[1][0].data()[0] = 2100u;
    top.cells[0].id = 17u;
    top.scalar = 9u;
    if (!tap.sample_one_cycle()) return fail("cycle3 sample failed: " + tap.last_error());

    if (!recorder.close(error)) {
        return fail("recorder.close failed: " + error);
    }

    if (recorder.declared_paths.size() != 1u ||
        recorder.declared_paths.count("top.scalar") != 1u) {
        return fail("compact arrays declared scalar tracks outside top.scalar");
    }
    if (recorder.arrays.size() != 4u) return fail("expected exactly four compact array blocks");
    if (recorder.arrays[0].element_count != 2u || recorder.arrays[0].element_stride != 8u ||
        recorder.arrays[0].total_bytes != 16u || recorder.arrays[0].schema.size() != 2u) {
        return fail("matrix compact array metadata mismatch");
    }
    if (recorder.arrays[1].element_count != 2u || recorder.arrays[1].element_stride != 16u ||
        recorder.arrays[1].total_bytes != 32u || recorder.arrays[1].schema.size() != 3u) {
        return fail("cube compact array metadata mismatch");
    }
    if (recorder.arrays[2].element_count != 2u ||
        recorder.arrays[2].element_stride != sizeof(ArrayMemberCell) ||
        recorder.arrays[2].total_bytes != sizeof(top.cells) ||
        recorder.arrays[2].schema.size() != 4u) {
        return fail("member-array compact metadata mismatch");
    }
    if (recorder.arrays[3].element_count != kCrossPageCount ||
        recorder.arrays[3].element_stride != sizeof(CrossPageCell) ||
        recorder.arrays[3].total_bytes != sizeof(top.cross_page) ||
        recorder.arrays[3].schema.size() != 3u) {
        return fail("cross-page compact metadata mismatch: count=" +
                    std::to_string(recorder.arrays[3].element_count) +
                    " stride=" + std::to_string(recorder.arrays[3].element_stride) +
                    " bytes=" + std::to_string(recorder.arrays[3].total_bytes) +
                    " schema=" + std::to_string(recorder.arrays[3].schema.size()));
    }

    std::cout << "nested_wave_array_wvz4_writer_ok file=" << out_path
              << " tracks=" << recorder.declared_paths.size()
              << " arrays=" << recorder.arrays.size()
              << " cycles=4\n";
    return 0;
}
