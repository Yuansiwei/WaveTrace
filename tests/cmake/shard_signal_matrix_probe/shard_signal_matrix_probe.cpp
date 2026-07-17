#include "project_reflect_auto.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ShardMatrixPeekSource : wave::PeekTraceSourceFor<ShardMatrixPeekSource, ShardDeepC> {
    ShardDeepC value = {};
    ShardDeepC* peek() { return &value; }
};

struct StrictSink : wave::IWaveSink {
    std::vector<wave::TrackDecl> declarations;
    std::vector<wave::NodeDecl> nodes;
    std::size_t node_count = 0;
    std::size_t sample_count = 0;

    StrictSink() : nodes(1) {}

    void on_node_declared(const wave::NodeDecl& decl) override {
        if (nodes.size() <= decl.node_id) nodes.resize(decl.node_id + 1u);
        nodes[decl.node_id] = decl;
        ++node_count;
    }
    void on_track_declared(const wave::TrackDecl& decl) override {
        declarations.push_back(decl);
    }
    void on_sample(const wave::TrackEvent&) override { ++sample_count; }
};

std::string path_for_node(const StrictSink& sink, wave::NodeId node_id) {
    std::vector<std::string> parts;
    std::size_t guard = 0;
    while (node_id != 0 && node_id < sink.nodes.size() && guard < sink.nodes.size()) {
        const wave::NodeDecl& node = sink.nodes[node_id];
        parts.push_back(node.name);
        if (node.parent_id == 0 || node.parent_id == node_id) break;
        node_id = node.parent_id;
        ++guard;
    }
    std::reverse(parts.begin(), parts.end());
    std::string path;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        if (path.empty() || parts[i][0] == '[') {
            path += parts[i];
        } else {
            path.push_back('.');
            path += parts[i];
        }
    }
    return path;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

std::string canonical_track(const wave::TrackDecl& decl, const std::string& path) {
    std::ostringstream os;
    os << path
       << "|kind=" << static_cast<unsigned>(decl.kind)
       << "|bits=" << decl.bit_width
       << "|offset=" << decl.bit_offset
       << "|storage_only=" << (decl.storage_only ? 1 : 0)
       << "|storage_id=" << decl.storage_id;
    return os.str();
}

bool require_path(const std::set<std::string>& paths, const std::string& path) {
    if (paths.find(path) != paths.end()) return true;
    std::cerr << "missing required signal path: " << path << "\n";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: shard_signal_matrix_probe <snapshot> <bulk-elements>\n";
        return 64;
    }

    const std::size_t element_count = static_cast<std::size_t>(std::strtoull(argv[2], NULL, 10));
    if (element_count == 0) {
        std::cerr << "bulk element count must be non-zero\n";
        return 65;
    }

    std::vector<ShardStressElement> bulk(element_count);
    ShardSmallLeaf alias_storage = {};
    ShardDirectLeaf raw_direct_storage = {};
    std::shared_ptr<ShardSmallLeaf> shared_wave_storage(new ShardSmallLeaf());
    ShardSignalMatrixRoot root;
    root.bulk = bulk.data();
    root.bulk.declareSize(element_count);
    root.alias_a = &alias_storage;
    root.alias_a.declareSize(1);
    root.alias_b = &alias_storage;
    root.alias_b.declareSize(1);
    root.owning_wave_ptr = std::unique_ptr<ShardSmallLeaf>(new ShardSmallLeaf());
    root.shared_wave_ptr = shared_wave_storage;
    root.raw_direct = &raw_direct_storage;
    root.unique_direct.reset(new ShardDirectLeaf());
    root.shared_direct.reset(new ShardDirectLeaf());
    root.weak_direct = root.shared_direct;

    ShardMatrixPeekSource peek_source;
    std::array<ShardDeepC, 2> std_array_root = {};
    std::pair<ShardDeepA, ShardSmallLeaf> pair_root = {};
    wave::array<ShardDeepA, 3> wave_array_root;
    ShardSmallLeaf c_array_root[2] = {};
    std::uint64_t scalar_root = 0;

    StrictSink sink;
    wave::BuildOptions options;
    options.enable_bitfield_fields = true;
    options.enable_union_fields = true;
    options.enable_parallel_topology_expansion = true;
    options.topology_expansion_threads = 8u;
    options.parallel_topology_min_elements = 2u;
    options.parallel_topology_min_work_items_per_element = 1u;
    options.dump_leaf_distribution_after_topology = false;

    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.add_root("peek", &peek_source);
    tracer.add_root("std_array_root", &std_array_root);
    tracer.add_root("pair_root", &pair_root);
    tracer.add_root("wave_array_root", &wave_array_root);
    tracer.add_root("c_array_root", &c_array_root);
    tracer.add_root("scalar_root", &scalar_root);
    tracer.prepare_topology(0);

    const wave::DynamicTypeOps stress_ops =
        wave::find_dynamic_type_ops(reflect::type_tag_of<ShardStressElement>());
#if defined(WAVETRACE_COMPILED_SHARDS)
    if (!stress_ops.reflected || !stress_ops.expand_one || !stress_ops.expand_array) {
        std::cerr << "ShardStressElement dynamic operations are incomplete"
                  << " reflected=" << stress_ops.reflected
                  << " one=" << (stress_ops.expand_one != NULL)
                  << " array=" << (stress_ops.expand_array != NULL) << "\n";
        return 1;
    }
#endif

    std::set<std::string> paths;
    std::vector<std::string> canonical;
    canonical.reserve(sink.declarations.size());
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& decl = sink.declarations[i];
        const std::string path = path_for_node(sink, decl.node_id);
        if (path.empty()) {
            if (decl.storage_only && decl.node_id == 0) {
                canonical.push_back(canonical_track(
                    decl, "@storage[" + std::to_string(decl.storage_id) + "]"));
                continue;
            }
            std::cerr << "track has no reconstructable path: track_id=" << decl.track_id
                      << " node_id=" << decl.node_id
                      << " storage_only=" << decl.storage_only << "\n";
            return 2;
        }
        if (!paths.insert(path).second) {
            std::cerr << "duplicate logical signal path: " << path << "\n";
            return 2;
        }
        canonical.push_back(canonical_track(decl, path));
    }

    bool required_ok = true;
    required_ok &= require_path(paths, "top.scalars.boolean_value");
    required_ok &= require_path(paths, "top.scalars.dirty_value");
    required_ok &= require_path(paths, "top.containers.c_array[2].dirty");
    required_ok &= require_path(paths, "top.containers.c_matrix[1][2].value");
    required_ok &= require_path(paths, "top.containers.std_array[3].valid");
    required_ok &= require_path(paths, "top.containers.std_matrix[2][1].dirty");
    required_ok &= require_path(paths, "top.containers.wave_array[4].value");
    required_ok &= require_path(paths, "top.containers.wave_matrix[2][1].valid");
    required_ok &= require_path(paths, "top.containers.pair_value.second.dirty");
    required_ok &= require_path(paths, "top.deep.child.child.leaf.value");
    required_ok &= require_path(paths, "top.derived.derived_leaf.value");
    required_ok &= require_path(paths, "top.bitfields.ordinary");
    required_ok &= require_path(paths, "top.bulk[0].scalars.u32_value");
    required_ok &= require_path(paths, "top.bulk[0].wave_array[1].dirty");
    required_ok &= require_path(paths, "top.bulk[" + std::to_string(element_count - 1u) + "].deep.own");
    required_ok &= require_path(paths, "top.alias_a.value");
    required_ok &= require_path(paths, "top.alias_b.value");
    required_ok &= require_path(paths, "top.owning_wave_ptr.value");
    required_ok &= require_path(paths, "top.shared_wave_ptr.dirty");
    required_ok &= require_path(paths, "top.raw_direct.nested.value");
    required_ok &= require_path(paths, "top.unique_direct.own");
    required_ok &= require_path(paths, "top.shared_direct.nested.valid");
    required_ok &= require_path(paths, "top.weak_direct.nested.dirty");
    required_ok &= require_path(paths, "peek.child.child.leaf.value");
    required_ok &= require_path(paths, "std_array_root[1].pair_children.second.valid");
    required_ok &= require_path(paths, "pair_root.first.leaf.dirty");
    required_ok &= require_path(paths, "wave_array_root[2].own");
    required_ok &= require_path(paths, "c_array_root[1].value");
    required_ok &= require_path(paths, "scalar_root");
    if (!required_ok) return 3;

    for (std::set<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
        if (starts_with(*it, "top.ignored_vector") ||
            starts_with(*it, "top.ignored_string") ||
            starts_with(*it, "top.ignored_c_string") ||
            starts_with(*it, "top.null_pointer")) {
            std::cerr << "unsupported/null field unexpectedly produced a signal: " << *it << "\n";
            return 4;
        }
    }

    // ShardStressElement has 40 leaves.  This assertion is independent of the
    // unsharded baseline and catches a common-mode expansion regression.
    const std::size_t expected_bulk_tracks = element_count * 40u;
    std::size_t actual_bulk_tracks = 0;
    for (std::set<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
        if (starts_with(*it, "top.bulk[")) ++actual_bulk_tracks;
    }
    if (actual_bulk_tracks != expected_bulk_tracks) {
        std::cerr << "bulk signal count mismatch expected=" << expected_bulk_tracks
                  << " actual=" << actual_bulk_tracks << "\n";
        return 5;
    }

    std::sort(canonical.begin(), canonical.end());
    std::ofstream snapshot(argv[1], std::ios::out | std::ios::binary | std::ios::trunc);
    if (!snapshot) {
        std::cerr << "cannot open snapshot: " << argv[1] << "\n";
        return 6;
    }
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        snapshot << canonical[i] << '\n';
    }
    snapshot.close();
    if (!snapshot) {
        std::cerr << "cannot finish snapshot: " << argv[1] << "\n";
        return 7;
    }

    tracer.sample(0);
    if (sink.sample_count == 0) {
        std::cerr << "topology exists but initial sampling emitted no values\n";
        return 8;
    }

    std::cout << "signal_matrix_ok"
              << " elements=" << element_count
              << " tracks=" << sink.declarations.size()
              << " nodes=" << sink.node_count
              << " samples=" << sink.sample_count
              << " bulk_tracks=" << actual_bulk_tracks
              << " parallel_batches=" << tracer.parallel_topology_batches()
              << " parallel_elements=" << tracer.parallel_topology_expanded_elements()
              << " cloned_elements=" << tracer.direct_topology_cloned_elements()
              << " entry_attempts=" << tracer.dynamic_array_entry_attempts()
              << " entry_successes=" << tracer.dynamic_array_entry_successes()
              << "\n";
    return 0;
}
