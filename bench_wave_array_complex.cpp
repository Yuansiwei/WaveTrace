#include "wave_runtime.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace bench {

typedef std::chrono::steady_clock Clock;

struct Meta {
    std::uint16_t kind;
    std::uint16_t flags;
    std::uint32_t token;
};

struct Lane {
    std::uint64_t stamp;
    wave::array<std::uint32_t, 8> samples;
    Meta meta;
    std::uint8_t valid;
    std::uint8_t reserved[7];
};

struct Cell {
    std::uint32_t id;
    std::uint32_t status;
    wave::array<Lane, 4> lanes;
    wave::array<wave::array<std::int16_t, 8>, 3> matrix;
    std::uint64_t counter;
    std::uint32_t* skipped_pointer;
};

template <std::size_t N>
struct Top {
    std::uint64_t cycle_marker;
    wave::array<Cell, N> cells;
};

struct LargeScaleCell {
    std::uint32_t id;
    std::uint32_t status;
    wave::array<wave::array<std::uint16_t, 2>, 2> matrix;
    std::uint64_t counter;
    std::uint32_t* skipped_pointer;
};

template <std::size_t N>
struct LargeScaleTop {
    std::uint64_t cycle_marker;
    wave::array<LargeScaleCell, N> cells;
};

static_assert(std::is_standard_layout<Meta>::value, "Meta must remain standard-layout");
static_assert(std::is_standard_layout<Lane>::value, "Lane must remain standard-layout");
static_assert(std::is_standard_layout<Cell>::value, "Cell must remain standard-layout");
static_assert(std::is_trivially_destructible<Cell>::value, "Cell must remain trivially destructible");
static_assert(sizeof(wave::array<std::uint32_t, 8>) == sizeof(std::uint32_t) * 8,
              "wave::array must remain size-preserving");
static_assert(sizeof(LargeScaleCell) == 32, "40m benchmark memory budget changed");
static_assert(std::is_standard_layout<LargeScaleCell>::value,
              "LargeScaleCell must remain standard-layout");

struct Memory {
    std::uint64_t working_set;
    std::uint64_t private_bytes;
    std::uint64_t peak_working_set;
    Memory() : working_set(0), private_bytes(0), peak_working_set(0) {}
};

Memory process_memory() {
    Memory result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        result.working_set = static_cast<std::uint64_t>(counters.WorkingSetSize);
        result.private_bytes = static_cast<std::uint64_t>(counters.PrivateUsage);
        result.peak_working_set = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#endif
    return result;
}

std::int64_t delta(std::uint64_t after, std::uint64_t before) {
    if (after >= before) {
        const std::uint64_t value = after - before;
        return value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            ? (std::numeric_limits<std::int64_t>::max)()
            : static_cast<std::int64_t>(value);
    }
    const std::uint64_t value = before - after;
    return value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
        ? (std::numeric_limits<std::int64_t>::min)()
        : -static_cast<std::int64_t>(value);
}

struct Probe {
    std::string name;
    std::size_t offset;
    std::size_t size;
    const unsigned char* expected;
    std::vector<unsigned char> observed;
};

class CountingSink : public wave::IWaveSink {
public:
    std::size_t nodes = 0;
    std::size_t tracks = 0;
    std::size_t array_declarations = 0;
    std::uint64_t declared_bytes = 0;
    std::uint64_t outer_elements = 0;
    std::uint64_t element_stride = 0;
    std::vector<wave::ArraySchemaNodeDecl> schema;

    std::uint64_t patch_calls = 0;
    std::uint64_t patch_bytes = 0;
    std::uint32_t max_patch_bytes = 0;
    std::uint64_t patch_checksum = 1469598103934665603ull;
    std::vector<Probe> probes;

    void on_node_declared(const wave::NodeDecl&) override { ++nodes; }
    void on_track_declared(const wave::TrackDecl&) override { ++tracks; }
    void on_sample(const wave::TrackEvent&) override {}

    void on_array_block_declared(const wave::ArrayBlockDecl& decl) override {
        ++array_declarations;
        declared_bytes += decl.total_bytes;
        outer_elements = decl.element_count;
        element_stride = decl.element_stride;
        schema = decl.schema;
    }

    void on_array_patch(const wave::ArrayPatchEvent& ev) override {
        ++patch_calls;
        patch_bytes += ev.byte_count;
        max_patch_bytes = (std::max)(max_patch_bytes, ev.byte_count);
        if (ev.data && ev.byte_count != 0) {
            patch_checksum ^= ev.data[0];
            patch_checksum *= 1099511628211ull;
            patch_checksum ^= ev.data[ev.byte_count - 1u];
            patch_checksum *= 1099511628211ull;
        }
        const std::size_t patch_begin = static_cast<std::size_t>(ev.byte_offset);
        const std::size_t patch_end = patch_begin + ev.byte_count;
        for (std::size_t i = 0; i < probes.size(); ++i) {
            Probe& probe = probes[i];
            const std::size_t probe_end = probe.offset + probe.size;
            const std::size_t copy_begin = (std::max)(patch_begin, probe.offset);
            const std::size_t copy_end = (std::min)(patch_end, probe_end);
            if (copy_begin >= copy_end || !ev.data) continue;
            std::memcpy(
                probe.observed.data() + (copy_begin - probe.offset),
                ev.data + (copy_begin - patch_begin),
                copy_end - copy_begin);
        }
    }

    void add_probe(const char* name,
                   const unsigned char* block_base,
                   const void* address,
                   std::size_t size) {
        Probe probe;
        probe.name = name;
        probe.offset = static_cast<const unsigned char*>(address) - block_base;
        probe.size = size;
        probe.expected = static_cast<const unsigned char*>(address);
        probe.observed.resize(size, 0u);
        probes.push_back(probe);
    }

    bool verify_probes(std::string& error) const {
        for (std::size_t i = 0; i < probes.size(); ++i) {
            const Probe& probe = probes[i];
            if (std::memcmp(probe.observed.data(), probe.expected, probe.size) == 0) continue;
            std::ostringstream out;
            out << "probe mismatch: " << probe.name << " offset=" << probe.offset
                << " bytes=" << probe.size;
            error = out.str();
            return false;
        }
        return true;
    }

    void reset_patch_stats() {
        patch_calls = 0;
        patch_bytes = 0;
        max_patch_bytes = 0;
        patch_checksum = 1469598103934665603ull;
    }
};

struct PhaseResult {
    std::string name;
    double elapsed_ms;
    std::size_t cycles;
    std::uint64_t logical_updates;
    Memory before;
    Memory after;
    std::uint64_t patch_calls;
    std::uint64_t patch_bytes;
    std::uint32_t max_patch_bytes;
    std::uint64_t checksum;
};

template <class Fn>
PhaseResult measure(const char* name,
                    std::size_t cycles,
                    std::uint64_t logical_updates,
                    CountingSink& sink,
                    Fn fn) {
    sink.reset_patch_stats();
    PhaseResult result;
    result.name = name;
    result.cycles = cycles;
    result.logical_updates = logical_updates;
    result.before = process_memory();
    const Clock::time_point begin = Clock::now();
    fn();
    const Clock::time_point end = Clock::now();
    result.after = process_memory();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    result.patch_calls = sink.patch_calls;
    result.patch_bytes = sink.patch_bytes;
    result.max_patch_bytes = sink.max_patch_bytes;
    result.checksum = sink.patch_checksum;
    return result;
}

std::uint64_t schema_leaf_instances(const CountingSink& sink) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < sink.schema.size(); ++i) {
        if (sink.schema[i].kind != wave::ArraySchemaNodeKind::Leaf) continue;
        std::uint64_t instances = sink.outer_elements;
        std::uint32_t parent = sink.schema[i].parent_schema_node_id;
        while (parent != 0) {
            if (parent > sink.schema.size()) return 0;
            const wave::ArraySchemaNodeDecl& ancestor = sink.schema[parent - 1u];
            if (ancestor.kind == wave::ArraySchemaNodeKind::Array) {
                instances *= ancestor.element_count;
            }
            parent = ancestor.parent_schema_node_id;
        }
        total += instances;
    }
    return total;
}

void print_phase(const PhaseResult& r) {
    const double per_cycle = r.cycles == 0 ? r.elapsed_ms : r.elapsed_ms / r.cycles;
    const double updates_per_second = r.elapsed_ms == 0.0
        ? 0.0
        : static_cast<double>(r.logical_updates) * 1000.0 / r.elapsed_ms;
    std::cout << std::fixed << std::setprecision(3)
              << "phase=" << r.name
              << " elapsed_ms=" << r.elapsed_ms
              << " ms_per_cycle=" << per_cycle
              << " cycles=" << r.cycles
              << " logical_updates=" << r.logical_updates
              << " updates_per_sec=" << updates_per_second
              << " ws_before=" << r.before.working_set
              << " ws_after=" << r.after.working_set
              << " ws_delta=" << delta(r.after.working_set, r.before.working_set)
              << " private_before=" << r.before.private_bytes
              << " private_after=" << r.after.private_bytes
              << " private_delta=" << delta(r.after.private_bytes, r.before.private_bytes)
              << " peak_ws=" << r.after.peak_working_set
              << " patch_calls=" << r.patch_calls
              << " patch_bytes=" << r.patch_bytes
              << " max_patch_bytes=" << r.max_patch_bytes
              << " checksum=" << r.checksum
              << "\n";
}

struct Settings {
    std::string size = "256k";
    std::size_t workers = 4;
    std::size_t unchanged_cycles = 20;
    std::size_t sparse_cycles = 20;
    std::size_t sparse_updates = 4096;
    std::size_t dense_cycles = 4;
    std::size_t dense_updates = 65536;
};

bool parse_size(const char* text, std::size_t& value) {
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (!end || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_args(int argc, char** argv, Settings& settings) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") return false;
        if (i + 1 >= argc) return false;
        const char* value = argv[++i];
        if (arg == "--size") {
            settings.size = value;
        } else if (arg == "--workers") {
            if (!parse_size(value, settings.workers)) return false;
        } else if (arg == "--unchanged-cycles") {
            if (!parse_size(value, settings.unchanged_cycles)) return false;
        } else if (arg == "--sparse-cycles") {
            if (!parse_size(value, settings.sparse_cycles)) return false;
        } else if (arg == "--sparse-updates") {
            if (!parse_size(value, settings.sparse_updates)) return false;
        } else if (arg == "--dense-cycles") {
            if (!parse_size(value, settings.dense_cycles)) return false;
        } else if (arg == "--dense-updates") {
            if (!parse_size(value, settings.dense_updates)) return false;
        } else {
            return false;
        }
    }
    return settings.size == "64k" || settings.size == "256k" ||
           settings.size == "1m" || settings.size == "40m";
}

class Barrier {
public:
    explicit Barrier(std::size_t participants)
        : participants_(participants), remaining_(participants), generation_(0) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::size_t generation = generation_;
        if (--remaining_ == 0) {
            remaining_ = participants_;
            ++generation_;
            condition_.notify_all();
            return;
        }
        condition_.wait(lock, [this, generation]() { return generation_ != generation; });
    }

private:
    const std::size_t participants_;
    std::size_t remaining_;
    std::size_t generation_;
    std::mutex mutex_;
    std::condition_variable condition_;
};

template <class UpdateFn>
void run_parallel_updates(wave::Tracer& tracer,
                          std::size_t workers,
                          std::size_t cycles,
                          std::size_t updates,
                          wave::Cycle& cycle,
                          UpdateFn update) {
    if (workers <= 1) {
        for (std::size_t c = 0; c < cycles; ++c) {
            for (std::size_t u = 0; u < updates; ++u) update(c, u);
            tracer.sample(cycle++);
        }
        return;
    }

    workers = (std::min)(workers, updates);
    Barrier start(workers + 1u);
    Barrier done(workers + 1u);
    Barrier finish(workers + 1u);
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.push_back(std::thread([&, worker]() {
            tracer.attach_current_thread_for_dirty_peek();
            for (std::size_t c = 0; c < cycles; ++c) {
                start.wait();
                for (std::size_t u = worker; u < updates; u += workers) update(c, u);
                done.wait();
            }
            finish.wait();
            tracer.detach_current_thread_for_dirty_peek();
        }));
    }
    for (std::size_t c = 0; c < cycles; ++c) {
        start.wait();
        done.wait();
        tracer.sample(cycle++);
    }
    finish.wait();
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();
}

template <std::size_t N>
int run(const Settings& settings) {
    const Memory process_start = process_memory();
    const Clock::time_point init_begin = Clock::now();
    std::unique_ptr<Top<N> > top(new Top<N>());
    Cell* cells = top->cells.data();
    for (std::size_t i = 0; i < N; i += 4093u) {
        cells[i].id = static_cast<std::uint32_t>(i);
        cells[i].counter = static_cast<std::uint64_t>(i) * 17u;
        cells[i].skipped_pointer = &cells[i].id;
    }
    const Clock::time_point init_end = Clock::now();
    const Memory after_init = process_memory();
    PhaseResult init;
    init.name = "model_init";
    init.elapsed_ms = std::chrono::duration<double, std::milli>(init_end - init_begin).count();
    init.cycles = 0;
    init.logical_updates = N;
    init.before = process_start;
    init.after = after_init;
    init.patch_calls = 0;
    init.patch_bytes = 0;
    init.max_patch_bytes = 0;
    init.checksum = 0;
    print_phase(init);

    CountingSink sink;
    const unsigned char* block_base = reinterpret_cast<const unsigned char*>(cells);
    const std::size_t middle = N / 2u;
    const std::size_t last = N - 1u;
    sink.add_probe("first.id", block_base, &cells[0].id, sizeof(cells[0].id));
    sink.add_probe("first.status", block_base, &cells[0].status, sizeof(cells[0].status));
    sink.add_probe("first.counter", block_base, &cells[0].counter, sizeof(cells[0].counter));
    sink.add_probe("first.lane.sample", block_base,
                   &cells[0].lanes.read(0).samples.read(0), sizeof(std::uint32_t));
    sink.add_probe("first.matrix", block_base,
                   &cells[0].matrix.read(0).read(0), sizeof(std::int16_t));
    sink.add_probe("middle.status", block_base, &cells[middle].status, sizeof(cells[middle].status));
    sink.add_probe("middle.lane.sample", block_base,
                   &cells[middle].lanes.read(2).samples.read(5), sizeof(std::uint32_t));
    sink.add_probe("last.matrix", block_base,
                   &cells[last].matrix.read(2).read(7), sizeof(std::int16_t));
    sink.add_probe("last.counter", block_base, &cells[last].counter, sizeof(cells[last].counter));
    sink.add_probe("first.skipped_pointer", block_base,
                   &cells[0].skipped_pointer, sizeof(cells[0].skipped_pointer));

    wave::BuildOptions options;
    options.enable_wave_array_dirty = true;
    options.enable_wave_array_parallel_sampling = true;
    options.wave_array_parallel_threshold = 1;
    options.enable_wave_array_memory_block_precheck = true;
    options.enable_wave_array_memory_block_byte_map = true;
    options.enable_parallel_topology_expansion = true;
    options.enable_parallel_sampling = true;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    std::unique_ptr<wave::Tracer> tracer;
    PhaseResult topology = measure("topology_expand", 0, 0, sink, [&]() {
        tracer.reset(new wave::Tracer(sink, options));
        tracer->add_root("top", top.get());
        tracer->prepare_topology(0);
    });
    print_phase(topology);

    std::cout << "topology"
              << " elements=" << N
              << " sizeof_cell=" << sizeof(Cell)
              << " model_bytes=" << sizeof(Top<N>)
              << " compact_blocks=" << tracer->compact_array_block_count()
              << " declared_arrays=" << sink.array_declarations
              << " declared_bytes=" << sink.declared_bytes
              << " element_stride=" << sink.element_stride
              << " schema_nodes=" << sink.schema.size()
              << " virtual_leaf_instances=" << schema_leaf_instances(sink)
              << " tracer_nodes=" << tracer->nodes().size()
              << " tracer_tracks=" << tracer->tracks().size()
              << " tracer_objects=" << tracer->objects().size()
              << " workers=" << settings.workers
              << "\n";
    const std::uint64_t virtual_leaves = schema_leaf_instances(sink);
    bool pointer_in_schema = false;
    for (std::size_t i = 0; i < sink.schema.size(); ++i) {
        pointer_in_schema = pointer_in_schema || sink.schema[i].name == "skipped_pointer";
    }
    if (sink.array_declarations != 1 || sink.declared_bytes != sizeof(Cell) * N ||
        sink.element_stride != sizeof(Cell) || virtual_leaves != 79ull * N ||
        pointer_in_schema) {
        std::cerr << "unexpected compact topology: arrays=" << sink.array_declarations
                  << " bytes=" << sink.declared_bytes
                  << " stride=" << sink.element_stride
                  << " virtual_leaves=" << virtual_leaves
                  << " pointer_in_schema=" << pointer_in_schema << "\n";
        return 8;
    }

    wave::Cycle cycle = 0;
    PhaseResult initial = measure("initial_shadow_and_full_patch", 1, 0, sink, [&]() {
        tracer->sample(cycle++);
    });
    print_phase(initial);
    std::string error;
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after initial sample\n";
        return 2;
    }
    if (initial.patch_bytes != sizeof(Cell) * N) {
        std::cerr << "initial patch byte count mismatch: actual=" << initial.patch_bytes
                  << " expected=" << sizeof(Cell) * N << "\n";
        return 3;
    }

    PhaseResult unchanged = measure(
        "runtime_unchanged", settings.unchanged_cycles, 0, sink, [&]() {
            for (std::size_t c = 0; c < settings.unchanged_cycles; ++c) tracer->sample(cycle++);
        });
    print_phase(unchanged);
    if (unchanged.patch_bytes != 0) {
        std::cerr << "unchanged phase emitted " << unchanged.patch_bytes << " bytes\n";
        return 4;
    }

    const std::size_t sparse_updates = (std::min)(settings.sparse_updates, N);
    PhaseResult nested_sparse = measure(
        "runtime_nested_sparse", settings.sparse_cycles,
        sparse_updates * settings.sparse_cycles, sink, [&]() {
            run_parallel_updates(*tracer, settings.workers, settings.sparse_cycles,
                                 sparse_updates, cycle,
                                 [&](std::size_t c, std::size_t u) {
                    const std::size_t index = (u * 4093u + c * 131u) % N;
                    const std::size_t lane = (u + c) & 3u;
                    const std::size_t sample = (u * 3u + c) & 7u;
                    if ((u & 1u) == 0u) {
                        cells[index].matrix[(u + c) % 3u][sample] +=
                            static_cast<std::int16_t>(1u + (u & 7u));
                    } else {
                        cells[index].lanes[lane].samples[sample] +=
                            static_cast<std::uint32_t>(1u + (u & 7u));
                    }
                });
        });
    print_phase(nested_sparse);
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after nested sparse phase\n";
        return 5;
    }

    PhaseResult outer_sparse = measure(
        "runtime_outer_sparse", settings.sparse_cycles,
        sparse_updates * settings.sparse_cycles, sink, [&]() {
            run_parallel_updates(*tracer, settings.workers, settings.sparse_cycles,
                                 sparse_updates, cycle,
                                 [&](std::size_t c, std::size_t u) {
                    const std::size_t index = (u * 8191u + c * 257u) % N;
                    Cell& cell = top->cells[index];
                    cell.status ^= static_cast<std::uint32_t>(0x9e3779b9u + u + c);
                    ++cell.counter;
                });
        });
    print_phase(outer_sparse);
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after outer sparse phase\n";
        return 6;
    }

    const std::size_t dense_updates = (std::min)(settings.dense_updates, N);
    PhaseResult dense = measure(
        "runtime_dense_data", settings.dense_cycles,
        dense_updates * settings.dense_cycles, sink, [&]() {
            for (std::size_t c = 0; c < settings.dense_cycles; ++c) {
                Cell* mutable_cells = top->cells.data();
                const std::size_t start = (c * 65537u) % (N - dense_updates + 1u);
                for (std::size_t u = 0; u < dense_updates; ++u) {
                    Cell& cell = mutable_cells[start + u];
                    cell.id += 3u;
                    cell.counter ^= static_cast<std::uint64_t>(start + u + c);
                }
                tracer->sample(cycle++);
            }
        });
    print_phase(dense);
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after dense phase\n";
        return 7;
    }

    std::cout << "result=PASS size=" << settings.size
              << " elements=" << N
              << " workers=" << settings.workers
              << " final_cycle=" << cycle
              << "\n";
    return 0;
}

int run_40m(const Settings& settings) {
    static const std::size_t N = 40000000u;
    typedef LargeScaleTop<N> Root;

    const Memory process_start = process_memory();
    const Clock::time_point init_begin = Clock::now();
    std::unique_ptr<Root> top(new Root());
    LargeScaleCell* cells = top->cells.data();
    for (std::size_t i = 0; i < N; i += 65537u) {
        cells[i].id = static_cast<std::uint32_t>(i);
        cells[i].counter = static_cast<std::uint64_t>(i) * 17u;
        cells[i].skipped_pointer = &cells[i].id;
    }
    const Clock::time_point init_end = Clock::now();
    PhaseResult init;
    init.name = "model_init";
    init.elapsed_ms = std::chrono::duration<double, std::milli>(init_end - init_begin).count();
    init.cycles = 0;
    init.logical_updates = N;
    init.before = process_start;
    init.after = process_memory();
    init.patch_calls = 0;
    init.patch_bytes = 0;
    init.max_patch_bytes = 0;
    init.checksum = 0;
    print_phase(init);

    CountingSink sink;
    const unsigned char* block_base = reinterpret_cast<const unsigned char*>(cells);
    const std::size_t middle = N / 2u;
    const std::size_t last = N - 1u;
    sink.add_probe("first.id", block_base, &cells[0].id, sizeof(cells[0].id));
    sink.add_probe("first.status", block_base, &cells[0].status, sizeof(cells[0].status));
    sink.add_probe("first.matrix", block_base,
                   &cells[0].matrix.read(0).read(0), sizeof(std::uint16_t));
    sink.add_probe("first.counter", block_base, &cells[0].counter, sizeof(cells[0].counter));
    sink.add_probe("middle.matrix", block_base,
                   &cells[middle].matrix.read(1).read(1), sizeof(std::uint16_t));
    sink.add_probe("last.counter", block_base, &cells[last].counter, sizeof(cells[last].counter));

    wave::BuildOptions options;
    options.enable_wave_array_dirty = true;
    options.enable_wave_array_parallel_sampling = true;
    options.wave_array_parallel_threshold = 1;
    options.enable_wave_array_memory_block_precheck = true;
    options.enable_wave_array_memory_block_byte_map = true;
    options.enable_parallel_topology_expansion = true;
    options.enable_parallel_sampling = true;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    std::unique_ptr<wave::Tracer> tracer;
    PhaseResult topology = measure("topology_expand", 0, 0, sink, [&]() {
        tracer.reset(new wave::Tracer(sink, options));
        tracer->add_root("top", top.get());
        tracer->prepare_topology(0);
    });
    print_phase(topology);

    const std::uint64_t virtual_leaves = schema_leaf_instances(sink);
    std::cout << "topology"
              << " elements=" << N
              << " sizeof_cell=" << sizeof(LargeScaleCell)
              << " model_bytes=" << sizeof(Root)
              << " compact_blocks=" << tracer->compact_array_block_count()
              << " declared_arrays=" << sink.array_declarations
              << " declared_bytes=" << sink.declared_bytes
              << " element_stride=" << sink.element_stride
              << " schema_nodes=" << sink.schema.size()
              << " virtual_leaf_instances=" << virtual_leaves
              << " tracer_nodes=" << tracer->nodes().size()
              << " tracer_tracks=" << tracer->tracks().size()
              << " tracer_objects=" << tracer->objects().size()
              << " workers=" << settings.workers
              << "\n";
    bool pointer_in_schema = false;
    for (std::size_t i = 0; i < sink.schema.size(); ++i) {
        pointer_in_schema = pointer_in_schema || sink.schema[i].name == "skipped_pointer";
    }
    if (sink.array_declarations != 1 ||
        sink.declared_bytes != sizeof(LargeScaleCell) * N ||
        sink.element_stride != sizeof(LargeScaleCell) ||
        virtual_leaves != 7ull * N || pointer_in_schema) {
        std::cerr << "unexpected 40m compact topology: arrays=" << sink.array_declarations
                  << " bytes=" << sink.declared_bytes
                  << " stride=" << sink.element_stride
                  << " virtual_leaves=" << virtual_leaves
                  << " pointer_in_schema=" << pointer_in_schema << "\n";
        return 20;
    }

    wave::Cycle cycle = 0;
    PhaseResult initial = measure("initial_shadow_and_full_patch", 1, 0, sink, [&]() {
        tracer->sample(cycle++);
    });
    print_phase(initial);
    std::string error;
    if (!sink.verify_probes(error) || initial.patch_bytes != sizeof(LargeScaleCell) * N) {
        std::cerr << (error.empty() ? "40m initial patch byte count mismatch" : error) << "\n";
        return 21;
    }

    PhaseResult unchanged = measure(
        "runtime_unchanged", settings.unchanged_cycles, 0, sink, [&]() {
            for (std::size_t c = 0; c < settings.unchanged_cycles; ++c) tracer->sample(cycle++);
        });
    print_phase(unchanged);
    if (unchanged.patch_bytes != 0) {
        std::cerr << "40m unchanged phase emitted " << unchanged.patch_bytes << " bytes\n";
        return 22;
    }

    const std::size_t sparse_updates = (std::min)(settings.sparse_updates, N);
    PhaseResult nested_sparse = measure(
        "runtime_nested_sparse", settings.sparse_cycles,
        sparse_updates * settings.sparse_cycles, sink, [&]() {
            run_parallel_updates(*tracer, settings.workers, settings.sparse_cycles,
                                 sparse_updates, cycle,
                                 [&](std::size_t c, std::size_t u) {
                    const std::size_t index = (u * 1048583u + c * 65537u) % N;
                    cells[index].matrix[(u + c) & 1u][(u * 3u + c) & 1u] +=
                        static_cast<std::uint16_t>(1u + (u & 7u));
                });
        });
    print_phase(nested_sparse);
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after 40m nested sparse phase\n";
        return 23;
    }

    PhaseResult outer_sparse = measure(
        "runtime_outer_sparse", settings.sparse_cycles,
        sparse_updates * settings.sparse_cycles, sink, [&]() {
            run_parallel_updates(*tracer, settings.workers, settings.sparse_cycles,
                                 sparse_updates, cycle,
                                 [&](std::size_t c, std::size_t u) {
                    const std::size_t index = (u * 2097169u + c * 131071u) % N;
                    LargeScaleCell& cell = top->cells[index];
                    cell.status ^= static_cast<std::uint32_t>(0x9e3779b9u + u + c);
                    ++cell.counter;
                });
        });
    print_phase(outer_sparse);
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after 40m outer sparse phase\n";
        return 24;
    }

    const std::size_t dense_updates = (std::min)(settings.dense_updates, N);
    PhaseResult dense = measure(
        "runtime_dense_data", settings.dense_cycles,
        dense_updates * settings.dense_cycles, sink, [&]() {
            for (std::size_t c = 0; c < settings.dense_cycles; ++c) {
                LargeScaleCell* mutable_cells = top->cells.data();
                const std::size_t start = (c * 1048583u) % (N - dense_updates + 1u);
                for (std::size_t u = 0; u < dense_updates; ++u) {
                    LargeScaleCell& cell = mutable_cells[start + u];
                    cell.id += 3u;
                    cell.counter ^= static_cast<std::uint64_t>(start + u + c);
                }
                tracer->sample(cycle++);
            }
        });
    print_phase(dense);
    if (!sink.verify_probes(error)) {
        std::cerr << error << " after 40m dense phase\n";
        return 25;
    }

    std::cout << "result=PASS size=40m elements=" << N
              << " workers=" << settings.workers
              << " final_cycle=" << cycle
              << "\n";
    return 0;
}

} // namespace bench

namespace reflect {

template<> struct is_reflected<bench::Meta> : std::true_type {};
template<> struct reflected_visitor<bench::Meta> {
    template<class P, class V, class G>
    static void visit(const bench::Meta* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("kind", std::addressof(obj->kind));
        on_ptr("flags", std::addressof(obj->flags));
        on_ptr("token", std::addressof(obj->token));
    }
};

template<> struct is_reflected<bench::Lane> : std::true_type {};
template<> struct reflected_visitor<bench::Lane> {
    template<class P, class V, class G>
    static void visit(const bench::Lane* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("stamp", std::addressof(obj->stamp));
        on_ptr("samples", std::addressof(obj->samples));
        on_ptr("meta", std::addressof(obj->meta));
        on_ptr("valid", std::addressof(obj->valid));
    }
};

template<> struct is_reflected<bench::Cell> : std::true_type {};
template<> struct reflected_visitor<bench::Cell> {
    template<class P, class V, class G>
    static void visit(const bench::Cell* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("id", std::addressof(obj->id));
        on_ptr("status", std::addressof(obj->status));
        on_ptr("lanes", std::addressof(obj->lanes));
        on_ptr("matrix", std::addressof(obj->matrix));
        on_ptr("counter", std::addressof(obj->counter));
        on_ptr("skipped_pointer", std::addressof(obj->skipped_pointer),
               wave::detail::PointerOrReferenceFieldTag());
    }
};

template<std::size_t N> struct is_reflected<bench::Top<N> > : std::true_type {};
template<std::size_t N> struct reflected_visitor<bench::Top<N> > {
    template<class P, class V, class G>
    static void visit(const bench::Top<N>* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("cycle_marker", std::addressof(obj->cycle_marker));
        on_ptr("cells", std::addressof(obj->cells));
    }
};

template<> struct is_reflected<bench::LargeScaleCell> : std::true_type {};
template<> struct reflected_visitor<bench::LargeScaleCell> {
    template<class P, class V, class G>
    static void visit(const bench::LargeScaleCell* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("id", std::addressof(obj->id));
        on_ptr("status", std::addressof(obj->status));
        on_ptr("matrix", std::addressof(obj->matrix));
        on_ptr("counter", std::addressof(obj->counter));
        on_ptr("skipped_pointer", std::addressof(obj->skipped_pointer),
               wave::detail::PointerOrReferenceFieldTag());
    }
};

template<std::size_t N> struct is_reflected<bench::LargeScaleTop<N> > : std::true_type {};
template<std::size_t N> struct reflected_visitor<bench::LargeScaleTop<N> > {
    template<class P, class V, class G>
    static void visit(const bench::LargeScaleTop<N>* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("cycle_marker", std::addressof(obj->cycle_marker));
        on_ptr("cells", std::addressof(obj->cells));
    }
};

} // namespace reflect

int main(int argc, char** argv) {
    bench::Settings settings;
    if (!bench::parse_args(argc, argv, settings)) {
        std::cerr << "usage: bench_wave_array_complex [--size 64k|256k|1m|40m] "
                     "[--workers N] "
                     "[--unchanged-cycles N] [--sparse-cycles N] "
                     "[--sparse-updates N] [--dense-cycles N] [--dense-updates N]\n";
        return 1;
    }
    if (settings.size == "64k") return bench::run<65536>(settings);
    if (settings.size == "256k") return bench::run<262144>(settings);
    if (settings.size == "1m") return bench::run<1048576>(settings);
    return bench::run_40m(settings);
}
