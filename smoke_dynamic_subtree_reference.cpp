#include "wave_tap.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Payload {
    std::uint32_t count;
    bool valid;
};

struct AliasDynamic : wave::DynamicTraceTarget {
    Payload* payload;
    mutable wave::WaveDirtyHook hook;

    explicit AliasDynamic(Payload* value) : payload(value) {}

    const void* wave_trace_target_ptr() const override { return payload; }
    const void* wave_trace_target_type_tag() const override {
        return reflect::type_tag_of<Payload>();
    }
    std::uint32_t wave_trace_target_byte_width() const override {
        return static_cast<std::uint32_t>(sizeof(Payload));
    }
    wave::WaveDirtyHook* wave_trace_dirty_hook() const override { return &hook; }
};

struct AliasPeek : wave::PeekTraceSource {
    Payload* payload;
    mutable wave::WaveDirtyHook hook;

    explicit AliasPeek(Payload* value) : payload(value) {}

    const void* wave_trace_peek_ptr() const override { return payload; }
    const void* wave_trace_peek_type_tag() const override {
        return reflect::type_tag_of<Payload>();
    }
    std::uint32_t wave_trace_peek_byte_width() const override {
        return static_cast<std::uint32_t>(sizeof(Payload));
    }
    wave::WaveDirtyHook* wave_trace_peek_dirty_hook() const override { return &hook; }
};

struct Root {
    AliasDynamic first;
    AliasDynamic second;
    AliasPeek peekFirst;
    AliasPeek peekSecond;

    Root(Payload* dynamicPayload, Payload* peekPayload)
        : first(dynamicPayload), second(dynamicPayload),
          peekFirst(peekPayload), peekSecond(peekPayload) {}
};

struct ParallelAliasDynamic : wave::DynamicTraceTarget {
    Payload* payload;
    wave::WaveDirtyHook* sharedHook;
    ParallelAliasDynamic() : payload(NULL), sharedHook(NULL) {}

    const void* wave_trace_target_ptr() const override { return payload; }
    const void* wave_trace_target_type_tag() const override {
        return reflect::type_tag_of<Payload>();
    }
    std::uint32_t wave_trace_target_byte_width() const override {
        return static_cast<std::uint32_t>(sizeof(Payload));
    }
    wave::WaveDirtyHook* wave_trace_dirty_hook() const override {
        return sharedHook;
    }
};

struct ParallelAliasSlot {
    ParallelAliasDynamic target;
};

struct ParallelAliasRoot {
    const ParallelAliasSlot* slots;
    std::size_t count;
    ParallelAliasRoot(const ParallelAliasSlot* values, std::size_t size)
        : slots(values), count(size) {}
};

} // namespace

namespace reflect {

template<> struct is_reflected<Payload> : std::true_type {};
template<> struct reflected_visitor<Payload> {
    template<class P, class V, class G>
    static void visit(const Payload* object, P&& on_ptr, V&&, G&&) {
        on_ptr("count", &object->count);
        on_ptr("valid", &object->valid);
    }
};

template<> struct is_reflected<Root> : std::true_type {};
template<> struct reflected_visitor<Root> {
    template<class P, class V, class G>
    static void visit(const Root* object, P&& on_ptr, V&&, G&&) {
        on_ptr("first", &object->first);
        on_ptr("second", &object->second);
        on_ptr("peekFirst", &object->peekFirst);
        on_ptr("peekSecond", &object->peekSecond);
    }
};

template<> struct is_reflected<ParallelAliasSlot> : std::true_type {};
template<> struct reflected_visitor<ParallelAliasSlot> {
    template<class P, class V, class G>
    static void visit(const ParallelAliasSlot* object, P&& on_ptr, V&&, G&&) {
        on_ptr("target", &object->target);
    }
};

template<> struct is_reflected<ParallelAliasRoot> : std::true_type {};
template<> struct reflected_visitor<ParallelAliasRoot> {
    template<class P, class V, class G>
    static void visit(const ParallelAliasRoot* object, P&& on_ptr, V&&, G&&) {
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "slots", object->slots, object->count);
    }
};

} // namespace reflect

class ReferenceRecorder : public PathStableWvz4Recorder {
public:
    std::size_t trackCount = 0;
    std::size_t referenceCount = 0;

    void on_track_declared_fast(wave::TrackId track_id,
                                wave::TrackId storage_id,
                                wave::NodeId node_id,
                                wave::ValueKind kind,
                                std::uint32_t bit_width,
                                std::uint32_t bit_offset,
                                bool storage_only,
                                const std::string& path) override {
        ++trackCount;
        PathStableWvz4Recorder::on_track_declared_fast(
            track_id, storage_id, node_id, kind, bit_width, bit_offset,
            storage_only, path);
    }

    void on_subtree_reference_declared(wave::NodeId node_id,
                                       wave::NodeId target_node_id) override {
        ++referenceCount;
        PathStableWvz4Recorder::on_subtree_reference_declared(
            node_id, target_node_id);
    }
};

int main(int argc, char** argv) {
    Payload payload = {7u, true};
    Payload peekPayload = {70u, false};
    Root root(&payload, &peekPayload);

    wave::ensure_dynamic_type_registered<Payload>();
    wave::BuildOptions options;
    options.emit_track_decl_path = true;
    options.enable_dynamic_dirty_groups = true;

    {
        wave::InMemoryWaveSink sink;
        wave::Tracer tracer(sink, options);
        tracer.add_root("top", &root);
        tracer.prepare_topology(0);
        tracer.sample(0);

        if (root.first.hook.tracer != &tracer ||
            root.second.hook.tracer != &tracer ||
            root.first.hook.group_id != root.second.hook.group_id ||
            root.peekFirst.hook.group_id != root.peekSecond.hook.group_id ||
            root.first.hook.group_id == root.peekFirst.hook.group_id) {
            std::cerr << "hooks did not retain the expected object-level group ids\n";
            return 1;
        }
        if (sink.declarations.size() != 4u) {
            std::cerr << "expected two canonical two-leaf signal tables, got "
                      << sink.declarations.size() << " tracks\n";
            return 1;
        }
        if (sink.subtree_references.size() != 2u) {
            std::cerr << "expected Dynamic and Peek subtree references, got "
                      << sink.subtree_references.size() << "\n";
            return 1;
        }
        for (std::size_t i = 0; i < sink.subtree_references.size(); ++i) {
            const std::pair<wave::NodeId, wave::NodeId>& ref =
                sink.subtree_references[i];
            if (ref.first == 0 || ref.second == 0 || ref.second >= ref.first) {
                std::cerr << "invalid subtree reference ids\n";
                return 1;
            }
        }
        for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
            if (sink.declarations[i].storage_id != sink.declarations[i].track_id) {
                std::cerr << "ordinary canonical leaf unexpectedly aliases storage\n";
                return 1;
            }
        }

        payload.count = 8u;
        root.second.hook.mark_dirty();
        peekPayload.count = 80u;
        root.peekSecond.hook.mark_dirty();
        tracer.sample(1);
    }

    {
        Payload badPayload = {1u, true};
        Payload badPeekPayload = {2u, false};
        Root badRoot(&badPayload, &badPeekPayload);
        wave::InMemoryWaveSink sink;
        wave::Tracer tracer(sink, options);
        badRoot.second.hook.tracer = &tracer;
        badRoot.second.hook.group_id = 123456u;
        bool rejected = false;
        try {
            tracer.add_root("bad", &badRoot);
            tracer.prepare_topology(0);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        badRoot.second.hook.clear();
        if (!rejected) {
            std::cerr << "corrupt hook group id was silently accepted\n";
            return 1;
        }
    }

    {
        Payload sharedPayload = {100u, true};
        wave::WaveDirtyHook sharedHook;
        std::vector<ParallelAliasSlot> slots(64u);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            slots[i].target.payload = &sharedPayload;
            slots[i].target.sharedHook = &sharedHook;
        }
        ParallelAliasRoot aliasRoot(&slots[0], slots.size());
        wave::BuildOptions parallelOptions = options;
        parallelOptions.emit_track_decl_path = false;
        parallelOptions.enable_parallel_topology_expansion = true;
        parallelOptions.topology_expansion_threads = 8u;
        parallelOptions.parallel_topology_min_elements = 2u;
        parallelOptions.parallel_topology_min_work_items_per_element = 0u;
        parallelOptions.parallel_topology_batch_elements = slots.size();

        wave::InMemoryWaveSink sink;
        wave::Tracer tracer(sink, parallelOptions);
        tracer.add_root("parallelAlias", &aliasRoot);
        bool aliasRejected = false;
        try {
            tracer.prepare_topology(0);
        } catch (const std::logic_error& error) {
            aliasRejected =
                std::string(error.what()).find("alias conflict") !=
                std::string::npos;
        }
        if (!aliasRejected || sharedHook.tracer != &tracer) {
            std::cerr << "parallel repeated Dynamic alias was hidden or changed hook owner"
                      << " rejected=" << (aliasRejected ? 1 : 0)
                      << " hook_bound=" << (sharedHook.tracer == &tracer ? 1 : 0)
                      << "\n";
            return 1;
        }
    }

    const std::string outputPath =
        argc > 1 ? argv[1] : "build_vs\\dynamic_subtree_reference.wvz4";
    ReferenceRecorder recorder;
    PathStableWvz4Recorder::OpenConfig config;
    config.file_path = outputPath;
    config.emit_default_clk = false;
    config.options.compression = wvz4::Compression::None;
    config.options.enable_stats_log = false;
    std::string error;
    if (!recorder.open(config, error)) {
        std::cerr << "recorder open failed: " << error << "\n";
        return 1;
    }
    wave::Tracer fileTracer(recorder, options);
    fileTracer.add_root("top", &root);
    wave::WaveTap tap(fileTracer, recorder);
    if (!tap.sample_one_cycle()) {
        std::cerr << "cycle 0 failed: " << tap.last_error() << "\n";
        return 1;
    }
    payload.count = 9u;
    root.first.hook.mark_dirty();
    peekPayload.count = 90u;
    root.peekFirst.hook.mark_dirty();
    if (!tap.sample_one_cycle()) {
        std::cerr << "cycle 1 failed: " << tap.last_error() << "\n";
        return 1;
    }
    if (!recorder.close(error)) {
        std::cerr << "recorder close failed: " << error << "\n";
        return 1;
    }
    if (recorder.trackCount != 4u || recorder.referenceCount != 2u) {
        std::cerr << "WVZ4 recorder did not preserve canonical/reference counts\n";
        return 1;
    }

    std::cout << "dynamic_subtree_reference_ok file=" << outputPath
              << " tracks=" << recorder.trackCount
              << " references=" << recorder.referenceCount << "\n";
    return 0;
}
