#include <wave_path_wvz4_recorder.h>
#include <wave_tap.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::uint64_t file_size(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    return input ? static_cast<std::uint64_t>(input.tellg()) : 0u;
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
    return 0u;
}

static const std::size_t kFixedOuter = 64u;
static const std::size_t kFixedInner = 16384u;

struct FixedArrayTop {
    wave::array<wave::array<std::uint32_t, kFixedInner>, kFixedOuter> matrix;
};

struct WavePtrElement {
    std::uint32_t valid;
    std::uint32_t state;
    std::uint32_t address;
    std::uint32_t payload;
};

static const std::size_t kWavePtrArrayCount = 4u;
static const std::size_t kWavePtrElementsPerArray = 65536u;

struct WavePtrTop {
    wave::WavePtr<WavePtrElement*> bank0;
    wave::WavePtr<WavePtrElement*> bank1;
    wave::WavePtr<WavePtrElement*> bank2;
    wave::WavePtr<WavePtrElement*> bank3;

    explicit WavePtrTop(WavePtrElement* data)
        : bank0(data),
          bank1(data + kWavePtrElementsPerArray),
          bank2(data + 2u * kWavePtrElementsPerArray),
          bank3(data + 3u * kWavePtrElementsPerArray) {
        bank0.declareSize(kWavePtrElementsPerArray);
        bank1.declareSize(kWavePtrElementsPerArray);
        bank2.declareSize(kWavePtrElementsPerArray);
        bank3.declareSize(kWavePtrElementsPerArray);
    }
};

} // namespace

namespace wave {

template<> struct GeneratedMemberNameTable<FixedArrayTop> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept { static const char* const n[] = {"matrix"}; return n; }
    static std::size_t count() noexcept { return 1u; }
};

template<> struct GeneratedMemberNameTable<WavePtrElement> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const n[] = {"valid", "state", "address", "payload"};
        return n;
    }
    static std::size_t count() noexcept { return 4u; }
};

template<> struct GeneratedMemberNameTable<WavePtrTop> {
    static constexpr bool generated = true;
    static const void* class_id() noexcept { static int id; return &id; }
    static const char* const* names() noexcept {
        static const char* const n[] = {"bank0", "bank1", "bank2", "bank3"};
        return n;
    }
    static std::size_t count() noexcept { return 4u; }
};

} // namespace wave

namespace reflect {

template<> struct is_reflected<FixedArrayTop> : std::true_type {};
template<> struct reflected_visitor<FixedArrayTop> {
    template<class P, class V, class G>
    static void visit(const FixedArrayTop* obj, P&& on_ptr, V&&, G&&) {
        ::wave::detail::invoke_ptr_visitor(
            on_ptr, "matrix", std::addressof(obj->matrix),
            ::wave::detail::GeneratedMemberId(::wave::GeneratedMemberNameTable<FixedArrayTop>::class_id(), 0u));
    }
};

template<> struct is_reflected<WavePtrElement> : std::true_type {};
template<> struct reflected_visitor<WavePtrElement> {
    template<class P, class V, class G>
    static void visit(const WavePtrElement* obj, P&& on_ptr, V&&, G&&) {
        const void* id = ::wave::GeneratedMemberNameTable<WavePtrElement>::class_id();
        ::wave::detail::invoke_ptr_visitor(on_ptr, "valid", std::addressof(obj->valid), ::wave::detail::GeneratedMemberId(id, 0u));
        ::wave::detail::invoke_ptr_visitor(on_ptr, "state", std::addressof(obj->state), ::wave::detail::GeneratedMemberId(id, 1u));
        ::wave::detail::invoke_ptr_visitor(on_ptr, "address", std::addressof(obj->address), ::wave::detail::GeneratedMemberId(id, 2u));
        ::wave::detail::invoke_ptr_visitor(on_ptr, "payload", std::addressof(obj->payload), ::wave::detail::GeneratedMemberId(id, 3u));
    }
};

template<> struct is_reflected<WavePtrTop> : std::true_type {};
template<> struct reflected_visitor<WavePtrTop> {
    template<class P, class V, class G>
    static void visit(const WavePtrTop* obj, P&& on_ptr, V&&, G&&) {
        const void* id = ::wave::GeneratedMemberNameTable<WavePtrTop>::class_id();
        ::wave::detail::invoke_ptr_visitor(on_ptr, "bank0", std::addressof(obj->bank0), ::wave::detail::GeneratedMemberId(id, 0u));
        ::wave::detail::invoke_ptr_visitor(on_ptr, "bank1", std::addressof(obj->bank1), ::wave::detail::GeneratedMemberId(id, 1u));
        ::wave::detail::invoke_ptr_visitor(on_ptr, "bank2", std::addressof(obj->bank2), ::wave::detail::GeneratedMemberId(id, 2u));
        ::wave::detail::invoke_ptr_visitor(on_ptr, "bank3", std::addressof(obj->bank3), ::wave::detail::GeneratedMemberId(id, 3u));
    }
};

} // namespace reflect

template <typename Top>
bool emit_pressure_samples(Top&,
                           wave::Tracer&,
                           PathStableWvz4Recorder&,
                           std::string&) {
    return false;
}

template <>
bool emit_pressure_samples<FixedArrayTop>(FixedArrayTop& top,
                                          wave::Tracer& tracer,
                                          PathStableWvz4Recorder& recorder,
                                          std::string& error) {
    wave::WaveTap tap(tracer, recorder);
    if (!tap.sample_one_cycle()) {
        error = tap.last_error();
        return false;
    }

    top.matrix[0][0] = 0x11111111u;
    top.matrix[31][8192] = 0x31181920u;
    if (!tap.sample_one_cycle()) {
        error = tap.last_error();
        return false;
    }

    top.matrix[0][16383] = 0x00016383u;
    top.matrix[63][16383] = 0x6316383u;
    if (!tap.sample_one_cycle()) {
        error = tap.last_error();
        return false;
    }

    top.matrix[0][0] = 0x33333333u;
    top.matrix[1][0] = 0x10000000u;
    if (!tap.sample_one_cycle()) {
        error = tap.last_error();
        return false;
    }
    return true;
}

template <typename Top>
int run_benchmark(const char* mode,
                  const std::string& output_path,
                  Top& top,
                  bool capture_samples) {
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig config;
    config.file_path = output_path;
    config.emit_default_clk = false;
    config.options.compression = wvz4::Compression::Zstd;
    config.options.zstd_level = 3;
    config.options.enable_block_pipeline = false;
    config.options.enable_lod_tables = false;
    config.options.enable_stats_log = true;
    config.options.stats_log_path = output_path + ".log";
    config.writer_process_connect_timeout_ms = 120000;

    std::string error;
    const Clock::time_point total_begin = Clock::now();
    if (!recorder.open(config, error)) {
        std::cerr << "recorder.open failed: " << error << "\n";
        return 3;
    }

    wave::BuildOptions options;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.debug_log_root_expand_stats = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;
    options.enable_parallel_topology_expansion = false;

    const Clock::time_point topology_begin = Clock::now();
    wave::Tracer tracer(recorder, options);
    tracer.add_root("top", &top);
    tracer.prepare_topology();
    const Clock::time_point topology_end = Clock::now();
    const std::size_t nodes = recorder.declared_node_count();
    const std::size_t signals = recorder.declared_track_count();
    const std::string recorder_state = recorder.debug_state_summary();

    if (capture_samples && !emit_pressure_samples(top, tracer, recorder, error)) {
        std::cerr << "sample pressure failed for " << mode << ": " << error << "\n";
        return 6;
    }

    const Clock::time_point writer_begin = Clock::now();
    if (!recorder.open_writer_if_needed(error)) {
        std::cerr << "open_writer_if_needed failed: " << error << " state=" << recorder_state << "\n";
        return 4;
    }
    const Clock::time_point writer_end = Clock::now();
    const Clock::time_point close_begin = Clock::now();
    if (!recorder.close(error)) {
        std::cerr << "recorder.close failed: " << error << "\n";
        return 5;
    }
    const Clock::time_point close_end = Clock::now();

    std::cout << "mode=" << mode
              << " nodes=" << nodes
              << " signals=" << signals
              << " cycles=" << (capture_samples ? 4 : 0)
              << " topology_ms=" << elapsed_ms(topology_begin, topology_end)
              << " writer_open_ms=" << elapsed_ms(writer_begin, writer_end)
              << " close_ms=" << elapsed_ms(close_begin, close_end)
              << " total_ms=" << elapsed_ms(total_begin, close_end)
              << " parent_peak_bytes=" << peak_working_set_bytes()
              << " file_bytes=" << file_size(output_path)
              << " state={" << recorder_state << "}"
              << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: array_name_e2e_bench <fixed|waveptr> <output.wvz4> [samples]\n";
        return 2;
    }
    const std::string mode = argv[1];
    const std::string output_path = argv[2];
    const bool capture_samples = argc == 4 && std::string(argv[3]) == "samples";
    if (mode == "fixed") {
        std::unique_ptr<FixedArrayTop> top(new FixedArrayTop());
        return run_benchmark("fixed", output_path, *top, capture_samples);
    }
    if (mode == "waveptr") {
        std::vector<WavePtrElement> elements(kWavePtrArrayCount * kWavePtrElementsPerArray);
        WavePtrTop top(elements.data());
        return run_benchmark("waveptr", output_path, top, capture_samples);
    }
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
}
