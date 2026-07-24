#include "signal_matrix_input.h"
#include "project_reflect_auto.h"

#include <iostream>
#include <memory>
#include <set>
#include <string>

namespace {

bool starts_with(const std::string& value, const char* prefix) {
    const std::string text(prefix);
    return value.compare(0, text.size(), text) == 0;
}

int fail(const char* message, const wave::InMemoryWaveSink& sink) {
    std::cerr << message << "\n";
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        std::cerr << "  " << sink.declarations[i].path << "\n";
    }
    return 1;
}

} // namespace

int main() {
    ShardPointerSlotContainers value;
    ShardSmallLeaf raw[32] = {};
    std::shared_ptr<ShardSmallLeaf> shared[4];
    std::size_t raw_cursor = 0u;

    for (std::size_t i = 0; i < 2u; ++i) {
        value.c_slots[i] = &raw[raw_cursor++];
        value.std_slots[i] = &raw[raw_cursor++];
        value.wave_slots[i] = &raw[raw_cursor++];
        value.unique_slots[i].reset(new ShardSmallLeaf());
        shared[i].reset(new ShardSmallLeaf());
        value.shared_slots[i] = shared[i];
        value.weak_slots[i] = shared[i];
        value.span_slots[i] = &raw[raw_cursor];
        raw_cursor += 2u;
        for (std::size_t j = 0; j < 2u; ++j) {
            value.c_matrix[i][j] = &raw[raw_cursor++];
            value.std_matrix[i][j] = &raw[raw_cursor++];
            value.wave_matrix[i][j] = &raw[raw_cursor++];
            value.mixed_c_std[i][j] = &raw[raw_cursor++];
        }
    }

    wave::InMemoryWaveSink sink;
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.trace_array_first_element_only = true;
    wave::Tracer tracer(sink, options);
    tracer.add_root("generated", &value);
    tracer.sample(0);

    std::set<std::string> paths;
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        paths.insert(sink.declarations[i].path);
    }
    const char* required[] = {
        "generated.c_slots[size=2].[0].value",
        "generated.c_matrix[size=2].[0][size=2].[0].value",
        "generated.std_slots[size=2].[0].value",
        "generated.std_matrix[size=2].[0][size=2].[0].value",
        "generated.wave_slots[size=2].[0].value",
        "generated.wave_matrix[size=2].[0][size=2].[0].value",
        "generated.mixed_c_std[size=2].[0][size=2].[0].value",
        "generated.unique_slots[size=2].[0].value",
        "generated.shared_slots[size=2].[0].value",
        "generated.weak_slots[size=2].[0].value",
        "generated.span_slots[size=2].[0][size=2].[0].value"
    };
    for (std::size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (paths.find(required[i]) == paths.end()) {
            return fail("generated pointer-storage element-zero path missing", sink);
        }
    }
    for (std::set<std::string>::const_iterator it = paths.begin();
         it != paths.end(); ++it) {
        if (it->find("].[1]") != std::string::npos) {
            return fail("generated pointer-storage array leaked a nonzero slot", sink);
        }
    }
    const std::size_t expected_tracks =
        (sizeof(required) / sizeof(required[0])) * 3u;
    if (sink.declarations.size() != expected_tracks) {
        return fail("generated pointer-storage array produced unexpected leaves", sink);
    }
    raw[0].value = 0x12345678u;
    tracer.sample(11);
    const std::string updated_path =
        "generated.c_slots[size=2].[0].value";
    bool saw_cycle_11 = false;
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        const wave::TrackEvent& event = sink.events[i];
        if (event.cycle != 11 || !event.has_u64 ||
            event.u64_value != 0x12345678u) {
            continue;
        }
        for (std::size_t j = 0; j < sink.declarations.size(); ++j) {
            if (sink.declarations[j].track_id == event.track_id &&
                sink.declarations[j].path == updated_path) {
                saw_cycle_11 = true;
                break;
            }
        }
    }
    if (saw_cycle_11) {
        return fail("generated pointer-storage first-only mode recorded after cycle zero", sink);
    }
    std::cout << "generated_pointer_storage_first_only_ok tracks="
              << sink.declarations.size() << "\n";
    return 0;
}
