#include "wave_path_wvz4_recorder.h"

#include <cstdint>
#include <iostream>
#include <string>

struct ArrayNameTableTop {
    wave::array<std::uint32_t, 4> short_array;
    wave::array<std::uint32_t, 7> long_array;
};

namespace reflect {
template<> struct is_reflected<ArrayNameTableTop> : std::true_type {};
template<> struct reflected_visitor<ArrayNameTableTop> {
    template<class P, class V, class G>
    static void visit(const ArrayNameTableTop* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("short_array", std::addressof(obj->short_array));
        on_ptr("long_array", std::addressof(obj->long_array));
    }
};
}

class ArrayNameCountingRecorder : public PathStableWvz4Recorder {
public:
    std::size_t runtime_name_calls = 0;
    std::size_t runtime_name_table_entries = 0;
    std::size_t array_index_calls = 0;
    std::uint32_t max_array_index = 0;

    void on_node_name_table_entry_fast(std::uint32_t runtime_name_id,
                                       const std::string& name) override {
        ++runtime_name_table_entries;
        PathStableWvz4Recorder::on_node_name_table_entry_fast(runtime_name_id, name);
    }

    void on_node_declared_name_ref_fast(wave::NodeId node_id,
                                        wave::NodeId parent_id,
                                        std::uint32_t runtime_name_id,
                                        wave::NodeKind kind) override {
        ++runtime_name_calls;
        PathStableWvz4Recorder::on_node_declared_name_ref_fast(
            node_id, parent_id, runtime_name_id, kind);
    }

    void on_node_declared_array_index_fast(wave::NodeId node_id,
                                           wave::NodeId parent_id,
                                           std::uint32_t array_index,
                                           wave::NodeKind kind) override {
        ++array_index_calls;
        if (array_index > max_array_index) max_array_index = array_index;
        PathStableWvz4Recorder::on_node_declared_array_index_fast(
            node_id, parent_id, array_index, kind);
    }
};

int main(int argc, char** argv) {
    const std::string output_path = argc >= 2
        ? std::string(argv[1])
        : std::string("tests\\build\\array_name_table\\array_name_table.wvz4");
    ArrayNameTableTop top;
    ArrayNameCountingRecorder recorder;
    PathStableWvz4Recorder::OpenConfig config;
    config.file_path = output_path;
    config.emit_default_clk = false;
    config.options.compression = wvz4::Compression::None;
    config.options.enable_stats_log = false;
    config.options.enable_lod_tables = false;
    std::string error;
    if (!recorder.open(config, error)) {
        std::cerr << "recorder.open failed: " << error << "\n";
        return 5;
    }
    wave::BuildOptions options;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    wave::Tracer tracer(recorder, options);
    tracer.add_root("top", &top);
    tracer.prepare_topology();

    if (recorder.array_index_calls != 11u || recorder.max_array_index != 6u) {
        std::cerr << "unexpected array callback count/max: "
                  << recorder.array_index_calls << "/" << recorder.max_array_index << "\n";
        return 1;
    }
    if (recorder.runtime_name_calls != 3u || recorder.runtime_name_table_entries != 3u) {
        std::cerr << "unexpected runtime name callback/table count: "
                  << recorder.runtime_name_calls << "/"
                  << recorder.runtime_name_table_entries << "\n";
        return 2;
    }
    if (recorder.declared_node_count() != 14u || recorder.declared_track_count() != 11u) {
        std::cerr << "unexpected topology counts: nodes=" << recorder.declared_node_count()
                  << " tracks=" << recorder.declared_track_count() << "\n";
        return 3;
    }
    const std::string summary = recorder.debug_state_summary();
    if (summary.find("interned_names=3") == std::string::npos ||
        summary.find("numeric_array_nodes=11") == std::string::npos) {
        std::cerr << "unexpected recorder name tables: " << summary << "\n";
        return 4;
    }
    if (!recorder.open_writer_if_needed(error)) {
        std::cerr << "open_writer_if_needed failed: " << error << "\n";
        return 6;
    }
    if (!recorder.close(error)) {
        std::cerr << "recorder.close failed: " << error << "\n";
        return 7;
    }
    std::cout << "array_name_table_ok file=" << output_path << " " << summary << "\n";
    return 0;
}
