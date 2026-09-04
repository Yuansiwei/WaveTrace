#include "wave_runtime.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct NestedLeaf {
    std::uint32_t value;
};

struct InnerPeek : wave::PeekTraceSourceFor<InnerPeek, NestedLeaf> {
    NestedLeaf* value;
    InnerPeek() : value(NULL) {}
    const NestedLeaf* peek() const noexcept { return value; }
};

struct OuterPayload {
    std::uint32_t before;
    InnerPeek inner;
};

struct OuterPeek : wave::PeekTraceSourceFor<OuterPeek, OuterPayload> {
    OuterPayload payload;
    const OuterPayload* peek() const noexcept { return &payload; }
    void mark_dirty() { wave_dirty_hook()->mark_dirty(); }
};

struct Root {
    OuterPeek outer;
};

class EventSink : public wave::IWaveSink {
public:
    std::vector<wave::TrackEvent> events;
    std::vector<std::string> paths;
    std::vector<wave::TrackDecl> declarations;

    void on_node_declared(const wave::NodeDecl&) override {}
    void on_track_declared(const wave::TrackDecl& decl) override {
        if (paths.size() <= decl.track_id) paths.resize(
            static_cast<std::size_t>(decl.track_id) + 1u);
        if (declarations.size() <= decl.track_id) declarations.resize(
            static_cast<std::size_t>(decl.track_id) + 1u);
        paths[static_cast<std::size_t>(decl.track_id)] = decl.path;
        declarations[static_cast<std::size_t>(decl.track_id)] = decl;
    }
    void on_sample(const wave::TrackEvent& event) override {
        events.push_back(event);
    }
};

} // namespace

namespace reflect {

template<> struct is_reflected<NestedLeaf> : std::true_type {};
template<> struct reflected_visitor<NestedLeaf> {
    template<class P, class V, class G>
    static void visit(const NestedLeaf* object, P&& on_ptr, V&&, G&&) {
        on_ptr("value", &object->value);
    }
};

template<> struct is_reflected<OuterPayload> : std::true_type {};
template<> struct reflected_visitor<OuterPayload> {
    template<class P, class V, class G>
    static void visit(const OuterPayload* object, P&& on_ptr, V&&, G&&) {
        on_ptr("before", &object->before);
        on_ptr("inner", &object->inner);
        // This aliases storage created by the nested InnerPeek capture, but it
        // belongs to the outer capture and must remain independently dirty.
        on_ptr("alias_after_inner", &object->inner.value->value);
    }
};

template<> struct is_reflected<Root> : std::true_type {};
template<> struct reflected_visitor<Root> {
    template<class P, class V, class G>
    static void visit(const Root* object, P&& on_ptr, V&&, G&&) {
        on_ptr("outer", &object->outer);
    }
};

} // namespace reflect

int main() {
    NestedLeaf leaf = {1u};
    Root root;
    root.outer.payload.before = 7u;
    root.outer.payload.inner.value = &leaf;

    wave::DynamicTypeRegistration<NestedLeaf> nested_registration;
    wave::DynamicTypeRegistration<OuterPayload> outer_registration;
    (void)nested_registration;
    (void)outer_registration;

    EventSink sink;
    wave::BuildOptions options;
    options.enable_dirty_peek_groups = true;
    options.emit_only_on_change = true;
    options.emit_track_decl_path = true;
    options.enable_parallel_sampling = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);
    tracer.sample(0);

    wave::TrackId inner_track = 0;
    wave::TrackId alias_track = 0;
    for (std::size_t i = 1; i < sink.paths.size(); ++i) {
        if (sink.paths[i] == "top.outer.inner.value") inner_track = i;
        if (sink.paths[i] == "top.outer.alias_after_inner") alias_track = i;
    }
    if (inner_track == 0 || alias_track == 0 ||
        alias_track >= sink.declarations.size() ||
        sink.declarations[alias_track].storage_id != alias_track ||
        sink.declarations[inner_track].storage_id != inner_track) {
        std::cerr << "nested capture unexpectedly deduplicated descendant leaves\n";
        return 2;
    }

    sink.events.clear();
    leaf.value = 2u;
    root.outer.mark_dirty();
    tracer.sample(1);

    bool saw_outer_leaf = false;
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        const wave::TrackId track_id = sink.events[i].track_id;
        if (track_id == alias_track) saw_outer_leaf = true;
    }

    if (!saw_outer_leaf) {
        std::cerr << "nested capture lost the independently owned outer leaf\n";
        return 2;
    }
    std::cout << "dirty_peek_nested_capture_ok events=" << sink.events.size() << "\n";
    return 0;
}
