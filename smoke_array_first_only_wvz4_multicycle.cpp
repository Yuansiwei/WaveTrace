#define main wavetrace_array_first_only_memory_smoke_main
#include "smoke_array_first_only.cpp"
#undef main

#include "wave_tap.h"

#include <map>
#include <string>
#include <vector>

namespace {

class CapturingArrayFirstRecorder : public PathStableWvz4Recorder {
public:
    std::map<wave::TrackId, std::string> paths;
    std::vector<wave::TrackEvent> samples;

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        PathStableWvz4Recorder::on_track_declared_fast(
            track_id,
            storage_id,
            node_id,
            kind,
            bit_width,
            bit_offset,
            storage_only,
            path);
        paths[track_id] = path;
    }

    void on_sample(const wave::TrackEvent& event) override {
        samples.push_back(event);
        PathStableWvz4Recorder::on_sample(event);
    }
};

bool has_recorded_value(const CapturingArrayFirstRecorder& recorder,
                        const char* path,
                        wave::Cycle cycle,
                        std::uint64_t value) {
    for (std::size_t i = 0; i < recorder.samples.size(); ++i) {
        const wave::TrackEvent& event = recorder.samples[i];
        const std::map<wave::TrackId, std::string>::const_iterator found =
            recorder.paths.find(event.track_id);
        if (found != recorder.paths.end() &&
            found->second == path &&
            event.cycle == cycle &&
            event.has_u64 &&
            event.u64_value == value) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    ArrayFirstTop top;
    CapturingArrayFirstRecorder recorder;
    PathStableWvz4Recorder::OpenConfig config;
    config.file_path = "tmp/array_first_only_multicycle.wvz4";
    config.options.enable_stats_log = true;

    std::string error;
    if (!recorder.open(config, error)) {
        std::cerr << error << "\n";
        return 2;
    }

    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.trace_array_first_element_only = true;
    wave::Tracer tracer(recorder, options);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    if (!tap.sample_one_cycle()) {
        std::cerr << tap.last_error() << "\n";
        return 3;
    }
    top.c_values[0] = 111u;
    top.pointer_storage[0].value = 141u;
    if (!tap.sample_one_cycle()) {
        std::cerr << tap.last_error() << "\n";
        return 4;
    }
    if (!recorder.close(error)) {
        std::cerr << error << "\n";
        return 5;
    }

    if (!has_recorded_value(recorder, "top.c_values[size=3].[0]", 0, 11u) ||
        !has_recorded_value(recorder, "top.std_values[size=4].[0]", 0, 21u) ||
        !has_recorded_value(recorder, "top.wave_values[size=5].[0].value", 0, 51u) ||
        !has_recorded_value(recorder, "top.nested[size=2].[0][size=3].[0]", 0, 31u) ||
        !has_recorded_value(recorder, "top.pointer_values[size=6].[0].value", 0, 41u)) {
        std::cerr << "WVZ4 recorder lost a cycle-zero first-element value\n";
        return 6;
    }
    if (has_recorded_value(
            recorder, "top.c_values[size=3].[0]", 1, 111u) ||
        has_recorded_value(
            recorder, "top.pointer_values[size=6].[0].value", 1, 141u)) {
        std::cerr << "WVZ4 recorder received a first-only sample after cycle zero\n";
        return 7;
    }
    for (std::size_t i = 0; i < recorder.samples.size(); ++i) {
        if (recorder.samples[i].cycle != 0) {
            std::cerr << "WVZ4 recorder received an unexpected nonzero cycle\n";
            return 8;
        }
    }
    std::cout << "array_first_only_wvz4_cycle0_only_ok\n";
    return 0;
}
