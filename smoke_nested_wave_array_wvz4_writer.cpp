#include "wave_tap.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

struct NestedWaveArrayTop {
    wave::array<wave::array<std::uint32_t, 2>, 2> matrix;
    wave::array<wave::array<wave::array<std::uint32_t, 2>, 2>, 2> cube;
};

namespace reflect {
template<> struct is_reflected<NestedWaveArrayTop> : std::true_type {};
template<> struct reflected_visitor<NestedWaveArrayTop> {
    template<class P, class V, class G>
    static void visit(const NestedWaveArrayTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("matrix", std::addressof(obj->matrix));
        on_ptr("cube", std::addressof(obj->cube));
    }
};
}

class NestedWaveArrayPathRecorder : public PathStableWvz4Recorder {
public:
    std::set<std::string> declared_paths;

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

int main(int argc, char** argv) {
    const std::string out_path =
        (argc >= 2 && argv[1] && argv[1][0] != '\0')
            ? argv[1]
            : "build_vs\\nested_wave_array_wvz4_smoke.wvz4";

    NestedWaveArrayTop top;
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
    if (!tap.sample_one_cycle()) return fail("cycle1 sample failed: " + tap.last_error());

    if (!tap.sample_one_cycle()) return fail("cycle2 sample failed: " + tap.last_error());

    top.matrix[1].data()[1] = 211u;
    top.cube[1][0].data()[0] = 2100u;
    if (!tap.sample_one_cycle()) return fail("cycle3 sample failed: " + tap.last_error());

    if (!recorder.close(error)) {
        return fail("recorder.close failed: " + error);
    }

    if (recorder.declared_paths.find("top.matrix.[0].[0]") == recorder.declared_paths.end()) {
        return fail("missing declared path top.matrix.[0].[0]");
    }
    if (recorder.declared_paths.find("top.matrix.[0].[1]") == recorder.declared_paths.end()) {
        return fail("missing declared path top.matrix.[0].[1]");
    }
    if (recorder.declared_paths.find("top.matrix.[1].[0]") == recorder.declared_paths.end()) {
        return fail("missing declared path top.matrix.[1].[0]");
    }
    if (recorder.declared_paths.find("top.matrix.[1].[1]") == recorder.declared_paths.end()) {
        return fail("missing declared path top.matrix.[1].[1]");
    }
    if (recorder.declared_paths.find("top.cube.[0].[1].[1]") == recorder.declared_paths.end()) {
        return fail("missing declared path top.cube.[0].[1].[1]");
    }
    if (recorder.declared_paths.find("top.cube.[1].[0].[0]") == recorder.declared_paths.end()) {
        return fail("missing declared path top.cube.[1].[0].[0]");
    }

    std::cout << "nested_wave_array_wvz4_writer_ok file=" << out_path
              << " tracks=" << recorder.declared_paths.size()
              << " cycles=4\n";
    return 0;
}
