#include "project_reflect_auto.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <utility>
#include <vector>

struct ProbeSink : wave::IWaveSink {
    bool accepts_node_name_table_fast() const override { return true; }
    void on_track_declared(const wave::TrackDecl&) override {}
    void on_sample(const wave::TrackEvent&) override {}
};

struct ProbePeekSource : wave::PeekTraceSourceFor<ProbePeekSource, Dependency06> {
    Dependency06 value = {};
    Dependency06* peek() { return &value; }
};

int main() {
    const std::size_t element_count = 9000u;
    std::vector<Dependency00> storage(element_count);
    wave::detail::AnnotatedWavePtrView<Dependency00> reflected_array(
        storage.data(), element_count);

    Dependency02 direct_root = {};
    std::array<Dependency03, 2> std_array = {};
    std::pair<Dependency04, Dependency05> std_pair = {};
    ProbePeekSource peek_source;

    ProbeSink sink;
    wave::BuildOptions options;
    options.enable_parallel_topology_expansion = true;
    options.topology_expansion_threads = 4u;
    options.parallel_topology_min_elements = 2u;
    options.parallel_topology_min_work_items_per_element = 1u;

    wave::Tracer tracer(sink, options);
    const wave::DynamicTypeOps dependency_ops =
        wave::find_dynamic_type_ops(reflect::type_tag_of<Dependency00>());
    if (!dependency_ops.reflected || !dependency_ops.expand_one || !dependency_ops.expand_array) {
        std::cerr << "compiled-shard type operations were not registered"
                  << " reflected=" << dependency_ops.reflected
                  << " one=" << (dependency_ops.expand_one != NULL)
                  << " array=" << (dependency_ops.expand_array != NULL) << "\n";
        return 1;
    }
    tracer.add_root("reflected_array", &reflected_array);
    tracer.add_root("direct_root", &direct_root);
    tracer.add_root("std_array", &std_array);
    tracer.add_root("std_pair", &std_pair);
    tracer.add_root("peek_source", &peek_source);
    tracer.prepare_topology(0);

    const std::size_t expected_physical_tracks = element_count + 1u + 2u + 2u + 1u;
    const std::size_t actual_physical_tracks = tracer.tracks().size() > 0u
        ? tracer.tracks().size() - 1u
        : 0u;
    if (actual_physical_tracks != expected_physical_tracks) {
        std::cerr << "compiled-shard expansion lost reflected tracks: expected="
                  << expected_physical_tracks << " actual=" << actual_physical_tracks << "\n";
        return 2;
    }
    if ((tracer.parallel_topology_batches() == 0u ||
         tracer.parallel_topology_expanded_elements() == 0u) &&
        tracer.direct_topology_cloned_elements() == 0u) {
        std::cerr << "compiled-shard pointer array did not use the registered batch entry"
                  << " fallback_batches=" << tracer.parallel_topology_fallback_batches()
                  << " entry_attempts=" << tracer.dynamic_array_entry_attempts()
                  << " entry_successes=" << tracer.dynamic_array_entry_successes()
                  << " tracks=" << actual_physical_tracks << "\n";
        return 3;
    }

    std::cout << "compiled_shard_runtime_ok tracks=" << actual_physical_tracks
              << " parallel_batches=" << tracer.parallel_topology_batches()
              << " parallel_elements=" << tracer.parallel_topology_expanded_elements()
              << " cloned_elements=" << tracer.direct_topology_cloned_elements()
              << " entry_attempts=" << tracer.dynamic_array_entry_attempts()
              << " entry_successes=" << tracer.dynamic_array_entry_successes()
              << "\n";
    return 0;
}
