#include "wave_path_wvz4_recorder.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static bool file_contains(const char* path, const char* needle) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

static int fail(const std::string& msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main() {
    std::remove("wave_runtime_error.log");

    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = "build_vs\\recorder_duplicate_path_log.wvz4";
    cfg.emit_default_clk = false;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;

    std::string error;
    if (!recorder.open(cfg, error)) return fail("open failed: " + error);

    recorder.on_node_declared_fast(1, 0, "top", wave::NodeKind::Aggregate);
    recorder.on_node_declared_fast(2, 1, "child", wave::NodeKind::Aggregate);
    recorder.on_node_declared_fast(3, 2, "value", wave::NodeKind::Leaf);
    recorder.on_node_declared_fast(4, 2, "alias", wave::NodeKind::Leaf);
    recorder.on_track_declared_fast(1, 1, 3, wave::ValueKind::UnsignedInt, 32, 0, false, "top.child.value");
    recorder.on_track_declared_fast(2, 1, 4, wave::ValueKind::UnsignedInt, 32, 0, false, "top.child.alias");

    if (!recorder.open_writer_if_needed(error)) return fail("open_writer_if_needed failed: " + error);
    recorder.begin_cycle(0);

    wave::TrackEvent first;
    first.track_id = 1;
    first.cycle = 0;
    first.has_u64 = true;
    first.u64_value = 11;
    first.has_change_bits = true;
    first.change_bits = 11;
    recorder.on_sample(first);

    wave::TrackEvent second = first;
    second.track_id = 2;
    second.u64_value = 22;
    second.change_bits = 22;
    recorder.on_sample(second);

    if (recorder.end_cycle(0, error)) {
        return fail("duplicate was not detected");
    }
    if (error.find("duplicate storage sample") == std::string::npos) {
        return fail("unexpected end_cycle error: " + error);
    }
    if (!file_contains("wave_runtime_error.log", "path=top.child.value")) {
        return fail("missing first path in wave_runtime_error.log");
    }
    if (!file_contains("wave_runtime_error.log", "path=top.child.alias")) {
        return fail("missing alias path in wave_runtime_error.log");
    }

    std::cout << "recorder_duplicate_path_log_ok\n";
    return 0;
}
