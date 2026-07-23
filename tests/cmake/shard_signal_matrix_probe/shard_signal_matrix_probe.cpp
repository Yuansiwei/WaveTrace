#include "signal_matrix_input.h"
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
    std::vector<wave::TrackEvent> samples;
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
    void on_sample(const wave::TrackEvent& event) override {
        samples.push_back(event);
        ++sample_count;
    }
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

wave::TrackId track_id_for_path(const StrictSink& sink, const std::string& wanted) {
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& decl = sink.declarations[i];
        if (path_for_node(sink, decl.node_id) == wanted) return decl.track_id;
    }
    return static_cast<wave::TrackId>(-1);
}

bool require_u64_sample(const StrictSink& sink,
                        wave::TrackId track_id,
                        std::uint64_t expected) {
    for (std::vector<wave::TrackEvent>::const_reverse_iterator it = sink.samples.rbegin();
         it != sink.samples.rend(); ++it) {
        if (it->track_id != track_id) continue;
        if (!it->has_u64 || it->u64_value != expected) {
            std::cerr << "sample mismatch track=" << track_id
                      << " expected=" << expected
                      << " has_u64=" << it->has_u64
                      << " actual=" << it->u64_value << "\n";
            return false;
        }
        return true;
    }
    std::cerr << "missing sample for track=" << track_id << "\n";
    return false;
}

bool require_path_u64_sample(const StrictSink& sink,
                             const std::string& path,
                             std::uint64_t expected) {
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& decl = sink.declarations[i];
        if (path_for_node(sink, decl.node_id) != path) continue;
        const wave::TrackId storage_track = decl.storage_id != 0
            ? decl.storage_id
            : decl.track_id;
        for (std::vector<wave::TrackEvent>::const_reverse_iterator it = sink.samples.rbegin();
             it != sink.samples.rend(); ++it) {
            if (it->track_id != decl.track_id && it->track_id != storage_track) continue;
            if (it->has_u64 && it->u64_value == expected) return true;
            std::cerr << "path sample mismatch path=" << path
                      << " expected=" << expected
                      << " has_u64=" << it->has_u64
                      << " actual=" << it->u64_value << "\n";
            return false;
        }
        std::cerr << "missing path sample path=" << path << "\n";
        return false;
    }
    std::cerr << "missing declaration for sampled path=" << path << "\n";
    return false;
}

bool require_path_u64_storage_slice(const StrictSink& sink,
                                    const std::string& path,
                                    std::uint64_t expected,
                                    std::uint32_t expected_width,
                                    std::uint32_t expected_offset) {
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& decl = sink.declarations[i];
        if (path_for_node(sink, decl.node_id) != path) continue;
        if (decl.bit_width != expected_width || decl.bit_offset != expected_offset) {
            std::cerr << "storage slice metadata mismatch path=" << path
                      << " expected_width=" << expected_width
                      << " actual_width=" << decl.bit_width
                      << " expected_offset=" << expected_offset
                      << " actual_offset=" << decl.bit_offset << "\n";
            return false;
        }
        const wave::TrackId storage_track = decl.storage_id != 0
            ? decl.storage_id
            : decl.track_id;
        for (std::vector<wave::TrackEvent>::const_reverse_iterator it = sink.samples.rbegin();
             it != sink.samples.rend(); ++it) {
            if (it->track_id != storage_track) continue;
            if (!it->has_u64) {
                std::cerr << "storage slice has no u64 value path=" << path << "\n";
                return false;
            }
            const std::uint64_t mask = expected_width >= 64u
                ? ~std::uint64_t(0)
                : ((std::uint64_t(1) << expected_width) - 1u);
            const std::uint64_t actual = (it->u64_value >> expected_offset) & mask;
            if (actual == expected) return true;
            std::cerr << "storage slice mismatch path=" << path
                      << " expected=" << expected
                      << " actual=" << actual
                      << " storage=" << it->u64_value << "\n";
            return false;
        }
        std::cerr << "missing storage sample path=" << path << "\n";
        return false;
    }
    std::cerr << "missing declaration for storage slice path=" << path << "\n";
    return false;
}

bool require_path_bool_sample(const StrictSink& sink,
                              const std::string& path,
                              bool expected) {
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& decl = sink.declarations[i];
        if (path_for_node(sink, decl.node_id) != path) continue;
        for (std::vector<wave::TrackEvent>::const_reverse_iterator it = sink.samples.rbegin();
             it != sink.samples.rend(); ++it) {
            if (it->track_id != decl.track_id) continue;
            if (it->has_bool && it->bool_value == expected) return true;
            std::cerr << "bool sample mismatch path=" << path
                      << " expected=" << expected
                      << " has_bool=" << it->has_bool
                      << " actual=" << it->bool_value << "\n";
            return false;
        }
        std::cerr << "missing bool sample path=" << path << "\n";
        return false;
    }
    std::cerr << "missing declaration for bool path=" << path << "\n";
    return false;
}

bool require_path_u64_sample_since(const StrictSink& sink,
                                   const std::string& path,
                                   std::uint64_t expected,
                                   std::size_t sample_begin) {
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& decl = sink.declarations[i];
        if (path_for_node(sink, decl.node_id) != path) continue;
        const wave::TrackId storage_track = decl.storage_id != 0
            ? decl.storage_id
            : decl.track_id;
        for (std::size_t sample_index = sample_begin;
             sample_index < sink.samples.size(); ++sample_index) {
            const wave::TrackEvent& event = sink.samples[sample_index];
            if (event.track_id != decl.track_id && event.track_id != storage_track) continue;
            if (event.has_u64 && event.u64_value == expected) return true;
            std::cerr << "new path sample mismatch path=" << path
                      << " expected=" << expected
                      << " has_u64=" << event.has_u64
                      << " actual=" << event.u64_value << "\n";
            return false;
        }
        std::cerr << "missing new path sample path=" << path
                  << " begin=" << sample_begin << "\n";
        return false;
    }
    std::cerr << "missing declaration for newly sampled path=" << path << "\n";
    return false;
}

int verify_annotated_weak_lifetime() {
    const std::size_t count = 8;
    std::vector<std::shared_ptr<ShardSmallLeaf> > owners(count);
    std::vector<std::weak_ptr<ShardSmallLeaf> > observers(count);
    std::vector<ShardAnnotatedWeakOnly> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        owners[i].reset(new ShardSmallLeaf());
        observers[i] = owners[i];
        values[i].set(owners[i]);
    }
    ShardAnnotatedWeakArrayRoot root;
    root.count = values.size();
    root.values = values.data();
    StrictSink sink;
    {
        wave::BuildOptions options;
        options.enable_parallel_topology_expansion = true;
        options.topology_expansion_threads = 4u;
        options.parallel_topology_min_elements = 2u;
        options.parallel_topology_min_work_items_per_element = 1u;
        options.dump_leaf_distribution_after_topology = false;
        wave::Tracer tracer(sink, options);
        tracer.add_root("weak", &root);
        try {
            tracer.prepare_topology(0);
        } catch (const std::exception& ex) {
            std::cerr << "annotated weak topology preparation failed: " << ex.what();
            for (std::size_t i = 0; i < owners.size(); ++i) {
                std::cerr << " owner[" << i << "].dirty="
                          << static_cast<const void*>(&owners[i]->dirty);
            }
            std::cerr << "\n";
            return 21;
        }
        owners.clear();
        for (std::size_t i = 0; i < observers.size(); ++i) {
            if (observers[i].expired()) {
                std::cerr << "WAVE_PTR weak target was not retained at " << i
                          << " after parallel topology merge\n";
                return 10;
            }
        }
        if (sink.declarations.empty() || tracer.parallel_topology_batches() == 0) {
            std::cerr << "WAVE_PTR weak target was not retained and expanded\n";
            return 10;
        }
        tracer.sample(0);
    }
    for (std::size_t i = 0; i < observers.size(); ++i) {
        if (!observers[i].expired()) {
            std::cerr << "WAVE_PTR weak keepalive outlived its Tracer at " << i << "\n";
            return 11;
        }
    }
    return 0;
}

int verify_union_bitfield_opt_out() {
    ShardPrivateNestedAnonymousUnion value;
    value.initialize(0xA5123456789ABCDFull, true);

    StrictSink sink;
    wave::BuildOptions options;
    options.enable_union_fields = false;
    options.enable_bitfield_fields = false;
    options.enable_parallel_topology_expansion = false;
    wave::Tracer tracer(sink, options);
    tracer.add_root("disabled", &value);
    tracer.prepare_topology(0);

    std::set<std::string> paths;
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        paths.insert(path_for_node(sink, sink.declarations[i].node_id));
    }
    if (paths.find("disabled.is_64bit_") == paths.end()) {
        std::cerr << "union/bit-field opt-out suppressed an ordinary sibling\n";
        return 26;
    }
    if (paths.find("disabled.pc_") != paths.end() ||
        paths.find("disabled.call_depth_") != paths.end() ||
        paths.find("disabled.raw_") != paths.end()) {
        std::cerr << "explicit union/bit-field opt-out was ignored\n";
        return 27;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: shard_signal_matrix_probe <snapshot> <bulk-elements>"
                     " [expect-alias-b-disabled|expect-raw-array-disabled]\n";
        return 64;
    }

    const bool expect_alias_b_disabled =
        argc == 4 && std::string(argv[3]) == "expect-alias-b-disabled";
    const bool expect_raw_array_disabled =
        argc == 4 && std::string(argv[3]) == "expect-raw-array-disabled";
    if (argc == 4 && !expect_alias_b_disabled && !expect_raw_array_disabled) {
        std::cerr << "unknown expectation mode: " << argv[3] << "\n";
        return 64;
    }

    const std::size_t element_count = static_cast<std::size_t>(std::strtoull(argv[2], NULL, 10));
    if (element_count == 0) {
        std::cerr << "bulk element count must be non-zero\n";
        return 65;
    }
    const int weak_lifetime_result = verify_annotated_weak_lifetime();
    if (weak_lifetime_result != 0) return weak_lifetime_result;
    const int union_opt_out_result = verify_union_bitfield_opt_out();
    if (union_opt_out_result != 0) return union_opt_out_result;

    std::vector<ShardStressElement> bulk(element_count);
    std::vector<ShardSmallLeaf> annotated_array(3);
    ShardSmallLeaf alias_storage = {};
    ShardSmallLeaf annotated_raw_storage = {};
    ShardDirectLeaf raw_direct_storage = {};
    std::shared_ptr<ShardSmallLeaf> shared_wave_storage(new ShardSmallLeaf());
    std::shared_ptr<ShardSmallLeaf> template_shared_storage(new ShardSmallLeaf());
    std::shared_ptr<ShardSmallLeaf> template_weak_storage(new ShardSmallLeaf());
    ShardSmallLeaf template_raw_storage = {};
    ShardSmallLeaf nested_template_raw_storage = {};
    std::shared_ptr<ShardSmallLeaf> nested_template_shared_storage(new ShardSmallLeaf());
    std::shared_ptr<ShardSmallLeaf> nested_template_weak_storage(new ShardSmallLeaf());
    std::array<ShardTemplatePointerBox<ShardSmallLeaf>, 2> template_array;
    std::array<ShardSmallLeaf, 2> template_array_raw_storage = {};
    std::array<std::shared_ptr<ShardSmallLeaf>, 2> template_array_shared_storage;
    std::array<std::shared_ptr<ShardSmallLeaf>, 2> template_array_weak_storage;
    ShardSmallLeaf edge_array_storage[2] = {};
    ShardSmallLeaf edge_alias_storage = {};
    ShardSmallLeaf edge_const_raw_storage = {};
    std::shared_ptr<ShardSmallLeaf> edge_const_shared_owner(new ShardSmallLeaf());
    std::shared_ptr<const ShardSmallLeaf> edge_const_shared_storage =
        edge_const_shared_owner;
    ShardSmallLeaf frozen_original_storage = {};
    ShardSmallLeaf frozen_replacement_storage = {};
    ShardPointerCycleNode cycle_a;
    ShardPointerCycleNode cycle_b;
    std::array<ShardNullablePointerOnly, 3> first_empty_array = {};
    std::array<ShardNullablePointerOnly, 4> middle_empty_array = {};
    std::array<ShardSmallLeaf, 3> first_empty_array_leaves = {};
    std::array<ShardSmallLeaf, 4> middle_empty_array_leaves = {};
    ShardSmallLeaf dynamic_inline_leaf = {};
    ShardSmallLeaf dynamic_ptr_leaf = {};
    ShardSmallLeaf dynamic_next_leaf = {};
    ShardDynamicWaveTarget dynamic_ptr_target;
    ShardDynamicWaveTarget dynamic_next_target;
    ShardHiddenDynamicWaveTarget hidden_dynamic_target;
    ShardSmallLeaf hidden_dynamic_leaf = {};
    ShardTemplateDynamicWaveTarget<ShardSmallLeaf> template_dynamic_ptr_target;
    ShardTemplateDynamicWaveTarget<ShardSmallLeaf> template_dynamic_erased_target;
    ShardSmallLeaf template_dynamic_inline_payload = {};
    ShardSmallLeaf template_dynamic_ptr_payload = {};
    ShardSmallLeaf template_dynamic_erased_payload = {};
    std::array<ShardDynamicWaveTarget, 2> dynamic_array;
    std::array<ShardSmallLeaf, 2> dynamic_array_leaves = {};
    wave::array<ShardMatrixPeekSource, 3> wave_peek_array_root;
    wave::array<ShardDynamicWaveTarget, 3> wave_dynamic_array_root;
    std::array<ShardSmallLeaf, 3> wave_dynamic_array_leaves = {};
    wave::array<wave::array<ShardMatrixPeekSource, 2>, 2>
        nested_wave_peek_array_root;
    wave::array<wave::array<ShardDynamicWaveTarget, 2>, 2>
        nested_wave_dynamic_array_root;
    ShardSmallLeaf nested_wave_dynamic_array_leaves[2][2] = {};
    ShardSmallLeaf pointer_c_slots[2] = {};
    ShardSmallLeaf pointer_c_matrix[2][2] = {};
    std::array<ShardSmallLeaf, 2> pointer_std_slots = {};
    ShardSmallLeaf pointer_std_matrix[2][2] = {};
    std::array<ShardSmallLeaf, 2> pointer_wave_slots = {};
    ShardSmallLeaf pointer_wave_matrix[2][2] = {};
    ShardSmallLeaf pointer_mixed_c_std[2][2] = {};
    std::array<std::shared_ptr<ShardSmallLeaf>, 2> pointer_shared_owners;
    std::array<std::shared_ptr<ShardSmallLeaf>, 2> pointer_weak_owners;
    ShardSmallLeaf pointer_span_storage[2][2] = {};
    ShardSignalMatrixRoot root;
    root.bulk_count = element_count;
    root.bulk = bulk.data();
    root.alias_a = &alias_storage;
    root.alias_b = &alias_storage;
    root.owning_wave_ptr = std::unique_ptr<ShardSmallLeaf>(new ShardSmallLeaf());
    root.shared_wave_ptr = shared_wave_storage;
    root.raw_direct = &raw_direct_storage;
    root.unique_direct.reset(new ShardDirectLeaf());
    root.shared_direct.reset(new ShardDirectLeaf());
    root.weak_direct = root.shared_direct;
    root.annotated.initialize(
        &annotated_raw_storage,
        annotated_array.data(),
        annotated_array.size(),
        std::unique_ptr<ShardSmallLeaf>(new ShardSmallLeaf()),
        std::unique_ptr<ShardSmallLeaf[]>(new ShardSmallLeaf[annotated_array.size()]),
        std::shared_ptr<ShardSmallLeaf>(new ShardSmallLeaf()));
    std::shared_ptr<ShardSmallLeaf> annotated_weak_owner(new ShardSmallLeaf());
    root.annotated_weak.set(annotated_weak_owner);
    root.template_box.initialize(
        &template_raw_storage, template_shared_storage, template_weak_storage);
    root.template_container.nested.initialize(
        &nested_template_raw_storage,
        nested_template_shared_storage,
        nested_template_weak_storage);
    for (std::size_t i = 0; i < template_array.size(); ++i) {
        template_array_shared_storage[i].reset(new ShardSmallLeaf());
        template_array_weak_storage[i].reset(new ShardSmallLeaf());
        template_array[i].initialize(
            &template_array_raw_storage[i],
            template_array_shared_storage[i],
            template_array_weak_storage[i]);
    }
    root.template_array_count = template_array.size();
    root.template_array = template_array.data();
    root.pointer_edges.initialize(
        edge_array_storage,
        edge_array_storage,
        edge_array_storage,
        edge_array_storage,
        &edge_alias_storage,
        &edge_const_raw_storage,
        edge_const_shared_storage,
        &frozen_original_storage);
    {
        std::shared_ptr<ShardSmallLeaf> expiring_owner(new ShardSmallLeaf());
        root.pointer_edges.set_expiring_weak(expiring_owner);
    }
    cycle_a.value = 0xA1u;
    cycle_b.value = 0xB2u;
    cycle_a.next = &cycle_b;
    cycle_b.next = &cycle_a;
    root.cycle_entry = &cycle_a;
    first_empty_array[1].leaf = &first_empty_array_leaves[1];
    first_empty_array[2].leaf = &first_empty_array_leaves[2];
    root.first_empty_array_count = first_empty_array.size();
    root.first_empty_array = first_empty_array.data();
    middle_empty_array[0].leaf = &middle_empty_array_leaves[0];
    middle_empty_array[2].leaf = &middle_empty_array_leaves[2];
    middle_empty_array[3].leaf = &middle_empty_array_leaves[3];
    root.middle_empty_array_count = middle_empty_array.size();
    root.middle_empty_array = middle_empty_array.data();
    root.nested_anonymous_union.initialize(0xA5123456789ABCDFull, true);
    root.dynamic_inline.initialize(0xD101u, &dynamic_inline_leaf);
    dynamic_ptr_target.initialize(0xD201u, &dynamic_ptr_leaf, &dynamic_next_target);
    dynamic_next_target.initialize(0xD301u, &dynamic_next_leaf, &dynamic_ptr_target);
    root.dynamic_ptr = &dynamic_ptr_target;
    for (std::size_t i = 0; i < dynamic_array.size(); ++i) {
        dynamic_array[i].initialize(
            static_cast<std::uint32_t>(0xD400u + i),
            &dynamic_array_leaves[i]);
    }
    root.dynamic_array_count = dynamic_array.size();
    root.dynamic_array = dynamic_array.data();
    hidden_dynamic_target.initialize(0xDC01u, &hidden_dynamic_leaf);
    hidden_dynamic_leaf.value = 0xDC11u;
    root.hidden_dynamic = &hidden_dynamic_target;
    template_dynamic_inline_payload.value = 0xED11u;
    template_dynamic_ptr_payload.value = 0xED21u;
    template_dynamic_erased_payload.value = 0xED31u;
    root.template_dynamic_inline.initialize(0xED10u, template_dynamic_inline_payload);
    template_dynamic_ptr_target.initialize(0xED20u, template_dynamic_ptr_payload);
    template_dynamic_erased_target.initialize(0xED30u, template_dynamic_erased_payload);
    root.scoped_template_dynamic.initialize();
    root.business.initialize();
    root.template_dynamic_ptr = &template_dynamic_ptr_target;
    root.template_dynamic_erased = &template_dynamic_erased_target;
    for (std::size_t i = 0; i < wave_dynamic_array_root.size(); ++i) {
        wave_dynamic_array_root[i].initialize(
            static_cast<std::uint32_t>(0xDA00u + i),
            &wave_dynamic_array_leaves[i]);
        wave_dynamic_array_leaves[i].value =
            static_cast<std::uint32_t>(0xDB00u + i);
        wave_peek_array_root[i].value.child.child.leaf.value =
            static_cast<std::uint32_t>(0xCA00u + i);
    }
    for (std::size_t i = 0; i < nested_wave_dynamic_array_root.size(); ++i) {
        for (std::size_t j = 0; j < nested_wave_dynamic_array_root[i].size(); ++j) {
            nested_wave_dynamic_array_root[i][j].initialize(
                static_cast<std::uint32_t>(0xEA00u + i * 0x10u + j),
                &nested_wave_dynamic_array_leaves[i][j]);
            nested_wave_dynamic_array_leaves[i][j].value =
                static_cast<std::uint32_t>(0xEB00u + i * 0x10u + j);
            nested_wave_peek_array_root[i][j].value.child.child.leaf.value =
                static_cast<std::uint32_t>(0xFA00u + i * 0x10u + j);
        }
    }
    for (std::size_t i = 0; i < 2u; ++i) {
        root.pointer_slot_containers.c_slots[i] = &pointer_c_slots[i];
        root.pointer_slot_containers.std_slots[i] = &pointer_std_slots[i];
        root.pointer_slot_containers.wave_slots[i] = &pointer_wave_slots[i];
        root.pointer_slot_containers.unique_slots[i].reset(new ShardSmallLeaf());
        pointer_shared_owners[i].reset(new ShardSmallLeaf());
        pointer_weak_owners[i].reset(new ShardSmallLeaf());
        root.pointer_slot_containers.shared_slots[i] = pointer_shared_owners[i];
        root.pointer_slot_containers.weak_slots[i] = pointer_weak_owners[i];
        root.pointer_slot_containers.span_slots[i] = pointer_span_storage[i];
        for (std::size_t j = 0; j < 2u; ++j) {
            root.pointer_slot_containers.c_matrix[i][j] = &pointer_c_matrix[i][j];
            root.pointer_slot_containers.std_matrix[i][j] = &pointer_std_matrix[i][j];
            root.pointer_slot_containers.wave_matrix[i][j] = &pointer_wave_matrix[i][j];
            root.pointer_slot_containers.mixed_c_std[i][j] = &pointer_mixed_c_std[i][j];
        }
    }

    alias_storage.value = 0x1101u;
    annotated_raw_storage.value = 0x2202u;
    annotated_array[1].value = 0x3303u;
    root.owning_wave_ptr->value = 0x4404u;
    shared_wave_storage->value = 0x5505u;
    template_raw_storage.value = 0x6606u;
    template_shared_storage->value = 0x7707u;
    template_weak_storage->value = 0x8808u;
    nested_template_raw_storage.value = 0x9909u;
    template_array_raw_storage[1].value = 0xAA0Au;
    template_array_shared_storage[1]->value = 0xBB0Bu;
    template_array_weak_storage[1]->value = 0xCC0Cu;
    edge_array_storage[1].value = 0xDD0Du;
    edge_alias_storage.value = 0xEE0Eu;
    edge_const_raw_storage.value = 0xF00Fu;
    edge_const_shared_owner->value = 0xA55Au;
    dynamic_inline_leaf.value = 0xD111u;
    dynamic_ptr_leaf.value = 0xD211u;
    dynamic_next_leaf.value = 0xD311u;
    dynamic_array_leaves[1].value = 0xD411u;
    first_empty_array_leaves[1].value = 0xDAA1u;
    first_empty_array_leaves[2].value = 0xDAA2u;
    middle_empty_array_leaves[0].value = 0xDAB0u;
    middle_empty_array_leaves[2].value = 0xDAB2u;
    middle_empty_array_leaves[3].value = 0xDAB3u;
    pointer_c_slots[1].value = 0xE101u;
    pointer_c_matrix[1][0].value = 0xE210u;
    pointer_std_slots[1].value = 0xE301u;
    pointer_std_matrix[1][1].value = 0xE411u;
    pointer_wave_slots[1].value = 0xE501u;
    pointer_wave_matrix[1][1].value = 0xE511u;
    pointer_mixed_c_std[1][0].value = 0xE520u;
    root.pointer_slot_containers.unique_slots[1]->value = 0xE531u;
    pointer_shared_owners[1]->value = 0xE601u;
    pointer_weak_owners[1]->value = 0xE701u;
    pointer_span_storage[1][1].value = 0xE811u;

    ShardMatrixPeekSource peek_source;
    std::array<ShardDeepC, 2> std_array_root = {};
    std::pair<ShardDeepA, ShardSmallLeaf> pair_root = {};
    wave::array<ShardDeepA, 3> wave_array_root;
    ShardSmallLeaf c_array_root[2] = {};
    std::uint64_t scalar_root = 0;

    StrictSink sink;
    wave::BuildOptions options;
    // Union members and bit-fields must be visible with ordinary default
    // BuildOptions.  This matches production Tracer/WaveTap construction and
    // prevents tests from hiding a default-off regression.
    if (!options.enable_union_fields || !options.enable_bitfield_fields) {
        std::cerr << "ordinary BuildOptions unexpectedly suppresses union/bit-field signals"
                  << " union=" << options.enable_union_fields
                  << " bitfield=" << options.enable_bitfield_fields << "\n";
        return 25;
    }
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
    tracer.add_root("wave_peek_array_root", &wave_peek_array_root);
    tracer.add_root("wave_dynamic_array_root", &wave_dynamic_array_root);
    tracer.add_root("nested_wave_peek_array_root", &nested_wave_peek_array_root);
    tracer.add_root("nested_wave_dynamic_array_root", &nested_wave_dynamic_array_root);
    tracer.add_root("c_array_root", &c_array_root);
    tracer.add_root("scalar_root", &scalar_root);
#if defined(WAVETRACE_COMPILED_SHARDS)
    const wave::DynamicTypeOps dynamic_ops_before_topology =
        wave::find_dynamic_type_ops(reflect::type_tag_of<ShardDynamicWaveTarget>());
    const wave::DynamicTypeOps template_dynamic_ops_before_topology =
        wave::find_dynamic_type_ops(
            reflect::type_tag_of<ShardTemplateDynamicWaveTarget<ShardSmallLeaf> >());
    if (!dynamic_ops_before_topology.reflected ||
        !template_dynamic_ops_before_topology.reflected) {
        std::cerr << "dynamic registration incomplete before topology"
                  << " ordinary=" << dynamic_ops_before_topology.reflected
                  << " template=" << template_dynamic_ops_before_topology.reflected
                  << " ordinary_one=" << (dynamic_ops_before_topology.expand_one != NULL)
                  << " template_one=" << (template_dynamic_ops_before_topology.expand_one != NULL)
                  << "\n";
        return 18;
    }
#endif
    try {
        tracer.prepare_topology(0);
    } catch (const std::exception& ex) {
        std::cerr << "topology preparation failed: " << ex.what() << "\n";
        return 19;
    } catch (...) {
        std::cerr << "topology preparation failed with a non-standard exception\n";
        return 20;
    }
    annotated_weak_owner.reset();
    if (root.annotated_weak.expired()) {
        std::cerr << "WAVE_PTR weak target was not retained after topology expansion\n";
        return 9;
    }

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
    required_ok &= require_path(paths, "top.nested_anonymous_union.pc_");
    required_ok &= require_path(paths, "top.nested_anonymous_union.call_depth_");
    required_ok &= require_path(paths, "top.nested_anonymous_union.raw_");
    required_ok &= require_path(paths, "top.nested_anonymous_union.is_64bit_");
    required_ok &= require_path(paths, "top.bulk[0].scalars.u32_value");
    required_ok &= require_path(paths, "top.bulk[0].wave_array[1].dirty");
    required_ok &= require_path(paths, "top.bulk[" + std::to_string(element_count - 1u) + "].deep.own");
    required_ok &= require_path(paths, "top.alias_a.value");
    if (!expect_alias_b_disabled) {
        required_ok &= require_path(paths, "top.alias_b.value");
    } else {
        for (std::set<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
            if (*it == "top.alias_b" || it->compare(0, 12, "top.alias_b.") == 0) {
                std::cerr << "runtime-disabled pointer member was expanded: " << *it << "\n";
                required_ok = false;
                break;
            }
        }
    }
    required_ok &= require_path(paths, "top.owning_wave_ptr.value");
    required_ok &= require_path(paths, "top.shared_wave_ptr.dirty");
    required_ok &= require_path(paths, "top.raw_direct.nested.value");
    required_ok &= require_path(paths, "top.unique_direct.own");
    required_ok &= require_path(paths, "top.shared_direct.nested.valid");
    required_ok &= require_path(paths, "top.weak_direct.nested.dirty");
    required_ok &= require_path(paths, "top.annotated.raw_one_.value");
    if (!expect_raw_array_disabled) {
        required_ok &= require_path(paths, "top.annotated.raw_array_[2].dirty");
    } else {
        for (std::set<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
            if (*it == "top.annotated.raw_array_" ||
                it->compare(0, 25, "top.annotated.raw_array_[") == 0) {
                std::cerr << "runtime-disabled pointer array was expanded: " << *it << "\n";
                required_ok = false;
                break;
            }
        }
    }
    required_ok &= require_path(paths, "top.annotated.owned_.valid");
    required_ok &= require_path(paths, "top.annotated.owned_array_[1].dirty");
    required_ok &= require_path(paths, "top.annotated.shared_.value");
    required_ok &= require_path(paths, "top.annotated_weak.weak_target_.dirty");
    required_ok &= require_path(paths, "top.template_box.raw_.value");
    required_ok &= require_path(paths, "top.template_box.shared_.valid");
    required_ok &= require_path(paths, "top.template_box.weak_.dirty");
    required_ok &= require_path(paths, "top.template_container.nested.raw_.dirty");
    required_ok &= require_path(paths, "top.template_array[0].shared_.value");
    required_ok &= require_path(paths, "top.template_array[1].weak_.valid");
    required_ok &= require_path(paths, "top.pointer_edges.literal_pair_[1].dirty");
    required_ok &= require_path(paths, "top.pointer_edges.enum_pair_[1].valid");
    required_ok &= require_path(paths, "top.pointer_edges.alias_.value");
    required_ok &= require_path(paths, "top.pointer_edges.const_raw_.valid");
    required_ok &= require_path(paths, "top.pointer_edges.const_shared_.dirty");
    required_ok &= require_path(paths, "top.pointer_edges.freeze_target_.value");
    required_ok &= require_path(paths, "top.cycle_entry.value");
    required_ok &= require_path(paths, "top.cycle_entry.next.value");
    required_ok &= require_path(paths, "top.first_empty_array[1].leaf.value");
    required_ok &= require_path(paths, "top.first_empty_array[2].leaf.value");
    required_ok &= require_path(paths, "top.middle_empty_array[0].leaf.value");
    required_ok &= require_path(paths, "top.middle_empty_array[2].leaf.value");
    required_ok &= require_path(paths, "top.middle_empty_array[3].leaf.value");
    required_ok &= require_path(paths, "top.dynamic_inline.own_");
    required_ok &= require_path(paths, "top.dynamic_inline.leaf_.value");
    required_ok &= require_path(paths, "top.dynamic_ptr.own_");
    required_ok &= require_path(paths, "top.dynamic_ptr.leaf_.value");
    required_ok &= require_path(paths, "top.dynamic_ptr.next_.own_");
    required_ok &= require_path(paths, "top.dynamic_ptr.next_.leaf_.value");
    required_ok &= require_path(paths, "top.dynamic_array[1].own_");
    required_ok &= require_path(paths, "top.dynamic_array[1].leaf_.value");
    required_ok &= require_path(paths, "top.hidden_dynamic.value_");
    required_ok &= require_path(paths, "top.hidden_dynamic.leaf_.value");
    required_ok &= require_path(paths, "top.template_dynamic_inline.own_");
    required_ok &= require_path(paths, "top.template_dynamic_inline.payload_.value");
    required_ok &= require_path(paths, "top.template_dynamic_ptr.own_");
    required_ok &= require_path(paths, "top.template_dynamic_ptr.payload_.value");
    required_ok &= require_path(paths, "top.template_dynamic_erased.own_");
    required_ok &= require_path(paths, "top.template_dynamic_erased.payload_.value");
    required_ok &= require_path(paths, "top.scoped_template_dynamic.target_.own_");
    required_ok &= require_path(paths, "top.scoped_template_dynamic.target_.payload_[2].value");
    required_ok &= require_path(paths, "top.business.block.protected_local_cycles_");
    required_ok &= require_path(paths, "top.business.block.inline_lane_.sequence_");
    required_ok &= require_path(paths, "top.business.block.inline_lane_.payload_[2].word");
    required_ok &= require_path(paths, "top.business.block.lane_array_[1].payload_[2].word");
    required_ok &= require_path(paths, "top.business.block.alias_.payload_[1].word");
    required_ok &= require_path(paths, "top.business.block.span_[1].payload_[2].word");
    required_ok &= require_path(paths, "top.business.block.owned_.payload_[2].word");
    required_ok &= require_path(paths, "top.business.block.shared_.payload_[1].word");
    required_ok &= require_path(paths, "top.business.block.weak_.payload_[1].word");
    required_ok &= require_path(paths, "top.business.block.erased_.payload_[2].word");
    required_ok &= require_path(paths, "top.business.block.raw_flags_");
    required_ok &= require_path(paths, "top.business.peek_owner.source_[2].word");
    required_ok &= require_path(paths, "top.business.peek_owner.erased_[1].word");
    required_ok &= require_path(paths, "top.pointer_slot_containers.c_slots[1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.c_matrix[1][0].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.std_slots[1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.std_matrix[1][1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.wave_slots[1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.wave_matrix[1][1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.mixed_c_std[1][0].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.unique_slots[1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.shared_slots[1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.weak_slots[1].value");
    required_ok &= require_path(paths, "top.pointer_slot_containers.span_slots[1][1].value");
    required_ok &= require_path(paths, "peek.child.child.leaf.value");
    required_ok &= require_path(paths, "std_array_root[1].pair_children.second.valid");
    required_ok &= require_path(paths, "pair_root.first.leaf.dirty");
    required_ok &= require_path(paths, "wave_array_root[2].own");
    required_ok &= require_path(
        paths, "wave_peek_array_root[1].child.child.leaf.value");
    required_ok &= require_path(paths, "wave_dynamic_array_root[1].own_");
    required_ok &= require_path(
        paths, "wave_dynamic_array_root[1].leaf_.value");
    required_ok &= require_path(
        paths, "nested_wave_peek_array_root[1][0].child.child.leaf.value");
    required_ok &= require_path(
        paths, "nested_wave_dynamic_array_root[1][0].own_");
    required_ok &= require_path(
        paths, "nested_wave_dynamic_array_root[1][0].leaf_.value");
    required_ok &= require_path(paths, "c_array_root[1].value");
    required_ok &= require_path(paths, "scalar_root");
    if (!required_ok) return 3;

    for (std::set<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
        if (starts_with(*it, "top.ignored_vector") ||
            starts_with(*it, "top.ignored_string") ||
            starts_with(*it, "top.ignored_c_string") ||
            starts_with(*it, "top.null_pointer") ||
            starts_with(*it, "top.pointer_edges.negative_array_") ||
            starts_with(*it, "top.pointer_edges.zero_array_") ||
            starts_with(*it, "top.pointer_edges.expired_weak_") ||
            starts_with(*it, "top.pointer_edges.empty_unique_") ||
            starts_with(*it, "top.pointer_edges.empty_shared_") ||
            starts_with(*it, "top.first_empty_array[0]") ||
            starts_with(*it, "top.middle_empty_array[1]")) {
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

    const std::size_t declaration_count_before_freeze_mutation = sink.declarations.size();
    const wave::TrackId frozen_value_track = track_id_for_path(
        sink, "top.pointer_edges.freeze_target_.value");
    if (frozen_value_track == static_cast<wave::TrackId>(-1)) {
        std::cerr << "freeze target track id was not found\n";
        return 12;
    }
    frozen_original_storage.value = 0x12345678u;
    frozen_replacement_storage.value = 0xDEADBEEFu;
    root.pointer_edges.replace_freeze_target(&frozen_replacement_storage);

    tracer.sample(0);
    if (sink.sample_count == 0) {
        std::cerr << "topology exists but initial sampling emitted no values\n";
        return 8;
    }
    if (sink.declarations.size() != declaration_count_before_freeze_mutation) {
        std::cerr << "topology changed after annotated pointer replacement"
                  << " before=" << declaration_count_before_freeze_mutation
                  << " after=" << sink.declarations.size() << "\n";
        return 12;
    }
    if (!require_u64_sample(sink, frozen_value_track, 0x12345678u)) {
        std::cerr << "sampling followed the replacement pointer after topology freeze\n";
        return 13;
    }
    struct ExpectedPathValue {
        const char* path;
        std::uint64_t value;
    };
    const ExpectedPathValue expected_pointer_values[] = {
        {"top.alias_a.value", 0x1101u},
        {"top.annotated.raw_one_.value", 0x2202u},
        {"top.annotated.raw_array_[1].value", 0x3303u},
        {"top.owning_wave_ptr.value", 0x4404u},
        {"top.shared_wave_ptr.value", 0x5505u},
        {"top.template_box.raw_.value", 0x6606u},
        {"top.template_box.shared_.value", 0x7707u},
        {"top.template_box.weak_.value", 0x8808u},
        {"top.template_container.nested.raw_.value", 0x9909u},
        {"top.template_array[1].raw_.value", 0xAA0Au},
        {"top.template_array[1].shared_.value", 0xBB0Bu},
        {"top.template_array[1].weak_.value", 0xCC0Cu},
        {"top.pointer_edges.literal_pair_[1].value", 0xDD0Du},
        {"top.pointer_edges.enum_pair_[1].value", 0xDD0Du},
        {"top.pointer_edges.alias_.value", 0xEE0Eu},
        {"top.pointer_edges.const_raw_.value", 0xF00Fu},
        {"top.pointer_edges.const_shared_.value", 0xA55Au},
        {"top.cycle_entry.value", 0xA1u},
        {"top.cycle_entry.next.value", 0xB2u},
        {"top.dynamic_inline.own_", 0xD101u},
        {"top.dynamic_inline.leaf_.value", 0xD111u},
        {"top.dynamic_ptr.own_", 0xD201u},
        {"top.dynamic_ptr.leaf_.value", 0xD211u},
        {"top.dynamic_ptr.next_.own_", 0xD301u},
        {"top.dynamic_ptr.next_.leaf_.value", 0xD311u},
        {"top.dynamic_array[1].own_", 0xD401u},
        {"top.dynamic_array[1].leaf_.value", 0xD411u},
        {"top.nested_anonymous_union.raw_", 0xA5123456789ABCDFull},
        {"top.template_dynamic_inline.own_", 0xED10u},
        {"top.template_dynamic_inline.payload_.value", 0xED11u},
        {"top.template_dynamic_ptr.own_", 0xED20u},
        {"top.template_dynamic_ptr.payload_.value", 0xED21u},
        {"top.template_dynamic_erased.own_", 0xED30u},
        {"top.template_dynamic_erased.payload_.value", 0xED31u},
        {"top.scoped_template_dynamic.target_.own_", 0xEC10u},
        {"top.scoped_template_dynamic.target_.payload_[2].value", 0xEC22u},
        {"top.business.block.protected_local_cycles_", 0xB003u},
        {"top.business.block.inline_lane_.sequence_", 0xB100u},
        {"top.business.block.inline_lane_.payload_[2].word", 0xB112u},
        {"top.business.block.lane_array_[1].payload_[2].word", 0xB312u},
        {"top.business.block.alias_.payload_[1].word", 0xB111u},
        {"top.business.block.span_[1].payload_[2].word", 0xB312u},
        {"top.business.block.owned_.payload_[2].word", 0xB412u},
        {"top.business.block.shared_.payload_[1].word", 0xB511u},
        {"top.business.block.weak_.payload_[1].word", 0xB511u},
        {"top.business.block.erased_.payload_[2].word", 0xB512u},
        {"top.business.block.raw_flags_", 0xBu},
        {"top.business.peek_owner.source_[2].word", 0xB612u},
        {"top.business.peek_owner.erased_[1].word", 0xB611u},
        {"wave_peek_array_root[1].child.child.leaf.value", 0xCA01u},
        {"wave_dynamic_array_root[1].own_", 0xDA01u},
        {"wave_dynamic_array_root[1].leaf_.value", 0xDB01u},
        {"nested_wave_peek_array_root[1][0].child.child.leaf.value", 0xFA10u},
        {"nested_wave_dynamic_array_root[1][0].own_", 0xEA10u},
        {"nested_wave_dynamic_array_root[1][0].leaf_.value", 0xEB10u},
        {"top.pointer_slot_containers.c_slots[1].value", 0xE101u},
        {"top.pointer_slot_containers.c_matrix[1][0].value", 0xE210u},
        {"top.pointer_slot_containers.std_slots[1].value", 0xE301u},
        {"top.pointer_slot_containers.std_matrix[1][1].value", 0xE411u},
        {"top.pointer_slot_containers.wave_slots[1].value", 0xE501u},
        {"top.pointer_slot_containers.wave_matrix[1][1].value", 0xE511u},
        {"top.pointer_slot_containers.mixed_c_std[1][0].value", 0xE520u},
        {"top.pointer_slot_containers.unique_slots[1].value", 0xE531u},
        {"top.pointer_slot_containers.shared_slots[1].value", 0xE601u},
        {"top.pointer_slot_containers.weak_slots[1].value", 0xE701u},
        {"top.pointer_slot_containers.span_slots[1][1].value", 0xE811u}
    };
    for (std::size_t i = 0;
         i < sizeof(expected_pointer_values) / sizeof(expected_pointer_values[0]);
         ++i) {
        if (expect_raw_array_disabled &&
            std::string(expected_pointer_values[i].path) ==
                "top.annotated.raw_array_[1].value") {
            continue;
        }
        if (!require_path_u64_sample(
                sink, expected_pointer_values[i].path, expected_pointer_values[i].value)) {
            return 16;
        }
    }
    if (!require_path_u64_storage_slice(
            sink, "top.nested_anonymous_union.pc_", 0x3456789ABCDFull, 48u, 0u) ||
        !require_path_u64_storage_slice(
            sink, "top.nested_anonymous_union.call_depth_", 0xA512u, 16u, 48u) ||
        !require_path_bool_sample(
            sink, "top.nested_anonymous_union.is_64bit_", true)) {
        return 16;
    }

    const std::size_t samples_after_initial = sink.samples.size();
    frozen_replacement_storage.value = 0xCAFEBABEu;
    tracer.sample(1);
    for (std::size_t i = samples_after_initial; i < sink.samples.size(); ++i) {
        if (sink.samples[i].track_id == frozen_value_track) {
            std::cerr << "replacement-only mutation emitted a frozen pointer sample\n";
            return 14;
        }
    }
    frozen_original_storage.value = 0x87654321u;
    tracer.sample(2);
    if (!require_u64_sample(sink, frozen_value_track, 0x87654321u)) {
        std::cerr << "frozen pointer stopped observing its topology-time target\n";
        return 15;
    }

    // Exercise dirty lookup and sampling after topology freeze.  Only one
    // element in each wave::array shape is touched; the new value must not be
    // lost behind PeekTraceSourceFor or DynamicTraceTargetFor dispatch.
    const std::size_t specialized_samples_begin = sink.samples.size();
    wave_peek_array_root[1].value.child.child.leaf.value = 0xCA91u;
    wave_dynamic_array_root[1].initialize(0xDA91u, &wave_dynamic_array_leaves[1]);
    wave_dynamic_array_leaves[1].value = 0xDB91u;
    nested_wave_peek_array_root[1][0].value.child.child.leaf.value = 0xFA90u;
    nested_wave_dynamic_array_root[1][0].initialize(
        0xEA90u, &nested_wave_dynamic_array_leaves[1][0]);
    nested_wave_dynamic_array_leaves[1][0].value = 0xEB90u;
    tracer.sample(3);
    if (!require_path_u64_sample_since(
            sink, "wave_peek_array_root[1].child.child.leaf.value",
            0xCA91u, specialized_samples_begin) ||
        !require_path_u64_sample_since(
            sink, "wave_dynamic_array_root[1].own_",
            0xDA91u, specialized_samples_begin) ||
        !require_path_u64_sample_since(
            sink, "wave_dynamic_array_root[1].leaf_.value",
            0xDB91u, specialized_samples_begin) ||
        !require_path_u64_sample_since(
            sink, "nested_wave_peek_array_root[1][0].child.child.leaf.value",
            0xFA90u, specialized_samples_begin) ||
        !require_path_u64_sample_since(
            sink, "nested_wave_dynamic_array_root[1][0].own_",
            0xEA90u, specialized_samples_begin) ||
        !require_path_u64_sample_since(
            sink, "nested_wave_dynamic_array_root[1][0].leaf_.value",
            0xEB90u, specialized_samples_begin)) {
        return 17;
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
              << " wave_specialized_arrays=4"
              << "\n";
    return 0;
}
