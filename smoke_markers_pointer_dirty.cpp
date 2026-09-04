#include "wave_runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>

template <typename T>
class vsiiIN {
public:
    virtual ~vsiiIN() {}
    virtual const T* peek() = 0;
};

template <typename T>
class vsipIN {
public:
    vsipIN() : iface_(NULL) {}
    void bind(vsiiIN<T>* iface) { iface_ = iface; }
    std::size_t size() const { return iface_ ? 1u : 0u; }
    const vsiiIN<T>* operator->() const { return iface_; }
    vsiiIN<T>* operator->() { return iface_; }

private:
    vsiiIN<T>* iface_;
};

struct PlainLeaf {
    std::uint32_t value;

    PlainLeaf() : value(0) {}
    explicit PlainLeaf(std::uint32_t v) : value(v) {}
};

struct DirectLeaf : wave::DirectReflectPointerTarget {
    std::uint32_t value;

    DirectLeaf() : value(0) {}
    explicit DirectLeaf(std::uint32_t v) : value(v) {}
};

struct MetricValue {
    std::uint32_t count;
};

struct MetricChannel : vsiiIN<MetricValue>,
                       wave::PeekTraceSourceFor<MetricChannel, MetricValue> {
    MetricValue value;

    MetricChannel() { value.count = 0; }

    const MetricValue* peek() override {
        return &value;
    }
};

struct ArraySlot {
    std::uint32_t count;
    std::uint8_t tag;

    ArraySlot() : count(0), tag(0) {}
};

class PrivateBox {
    WAVE_REFLECT_FRIEND

private:
    std::uint16_t secret_;

public:
    PrivateBox() : secret_(0) {}
    void set_secret(std::uint16_t v) { secret_ = v; }
};

struct Top {
    typedef unsigned char U01;

    PrivateBox friend_box;
    DirectLeaf raw_target;
    DirectLeaf unique_target_seed;
    DirectLeaf shared_target_seed;
    DirectLeaf wave_raw_target;
    DirectLeaf wave_raw_array_targets[2];
    DirectLeaf wave_shared_target_seed;
    PlainLeaf plain_target;
    DirectLeaf* direct_raw;
    std::unique_ptr<DirectLeaf> direct_unique;
    std::shared_ptr<DirectLeaf> direct_shared;
    PlainLeaf* plain_ptr;
    WAVE_PTR DirectLeaf* wave_raw;
    std::size_t wave_raw_array_count;
    WAVE_PTR_ARRAY(wave_raw_array_count) DirectLeaf* wave_raw_array;
    WAVE_PTR std::shared_ptr<DirectLeaf> wave_shared;
    MetricChannel peek;
    MetricChannel port_channel;
    vsipIN<MetricValue> port;
    wave::WaveValue<std::uint32_t> dirty_counter;
    wave::array<ArraySlot, 3> slots;
    U01 flag_bool;

    Top()
        : raw_target(10),
          unique_target_seed(20),
          shared_target_seed(30),
          wave_raw_target(40),
          wave_raw_array_targets{ DirectLeaf(70), DirectLeaf(80) },
          wave_shared_target_seed(50),
          plain_target(60),
          direct_raw(&raw_target),
          direct_unique(new DirectLeaf(unique_target_seed)),
          direct_shared(new DirectLeaf(shared_target_seed)),
          plain_ptr(&plain_target),
          wave_raw(&wave_raw_target),
          wave_raw_array_count(2),
          wave_raw_array(wave_raw_array_targets),
          wave_shared(std::shared_ptr<DirectLeaf>(new DirectLeaf(wave_shared_target_seed))),
          dirty_counter(0),
          flag_bool(0) {
        friend_box.set_secret(7);
        peek.value.count = 11;
        port_channel.value.count = 12;
        port.bind(&port_channel);
        slots[0].count = 1;
        slots[0].tag = 2;
        slots[1].count = 3;
        slots[1].tag = 4;
        slots[2].count = 5;
        slots[2].tag = 6;
    }
};

namespace wave {
template<> struct ReflectAccess<PrivateBox> {
    template<class P, class V, class G>
    static void visit(const PrivateBox* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("secret", std::addressof(obj->secret_));
    }
};
}

namespace reflect {
template<> struct is_reflected<PlainLeaf> : std::true_type {};
template<> struct reflected_visitor<PlainLeaf> {
    template<class P, class V, class G>
    static void visit(const PlainLeaf* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(obj->value));
    }
};

template<> struct is_reflected<DirectLeaf> : std::true_type {};
template<> struct reflected_visitor<DirectLeaf> {
    template<class P, class V, class G>
    static void visit(const DirectLeaf* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(obj->value));
    }
};

template<> struct is_reflected<MetricValue> : std::true_type {};
template<> struct reflected_visitor<MetricValue> {
    template<class P, class V, class G>
    static void visit(const MetricValue* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("count", std::addressof(obj->count));
    }
};

template<> struct is_reflected<ArraySlot> : std::true_type {};
template<> struct reflected_visitor<ArraySlot> {
    template<class P, class V, class G>
    static void visit(const ArraySlot* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("count", std::addressof(obj->count));
        on_ptr("tag", std::addressof(obj->tag));
    }
};

template<> struct is_reflected<PrivateBox> : std::true_type {};
template<> struct reflected_visitor<PrivateBox> {
    template<class P, class V, class G>
    static void visit(const PrivateBox* obj, P&& on_ptr, V&& on_value, G&& on_getter) {
        ::wave::ReflectAccess<PrivateBox>::visit(
            obj,
            std::forward<P>(on_ptr),
            std::forward<V>(on_value),
            std::forward<G>(on_getter));
    }
};

template<> struct is_reflected<Top> : std::true_type {};
template<> struct reflected_visitor<Top> {
    template<class P, class V, class G>
    static void visit(const Top* obj, P&& on_ptr, V&&, G&&) {
        on_ptr("friend_box", std::addressof(obj->friend_box));
        on_ptr("direct_raw", std::addressof(obj->direct_raw));
        on_ptr("direct_unique", std::addressof(obj->direct_unique));
        on_ptr("direct_shared", std::addressof(obj->direct_shared));
        on_ptr("plain_ptr", std::addressof(obj->plain_ptr));
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "wave_raw", obj->wave_raw, 1u);
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "wave_raw_array", obj->wave_raw_array, obj->wave_raw_array_count);
        ::wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "wave_shared", obj->wave_shared, 1u);
        on_ptr("peek", std::addressof(obj->peek));
        on_ptr("port", std::addressof(obj->port));
        on_ptr("dirty_counter", std::addressof(obj->dirty_counter));
        on_ptr("slots", std::addressof(obj->slots));
        on_ptr("flag_bool", ::wave::as_bool_storage_ptr(std::addressof(obj->flag_bool)));
    }
};
}

struct TraceIndex {
    std::map<std::string, wave::TrackId> track_by_path;
    std::map<wave::TrackId, std::string> path_by_track;
};

static TraceIndex build_index(const wave::InMemoryWaveSink& sink) {
    TraceIndex out;
    for (std::size_t i = 0; i < sink.declarations.size(); ++i) {
        const wave::TrackDecl& d = sink.declarations[i];
        out.track_by_path[d.path] = d.track_id;
        out.path_by_track[d.track_id] = d.path;
    }
    return out;
}

static bool has_decl(const TraceIndex& index, const std::string& path) {
    return index.track_by_path.find(path) != index.track_by_path.end();
}

static std::size_t event_count_for(const wave::InMemoryWaveSink& sink,
                                   const TraceIndex& index,
                                   const std::string& path) {
    std::map<std::string, wave::TrackId>::const_iterator it = index.track_by_path.find(path);
    if (it == index.track_by_path.end()) return 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        if (sink.events[i].track_id == it->second) ++count;
    }
    return count;
}

static bool has_u64_event(const wave::InMemoryWaveSink& sink,
                          const TraceIndex& index,
                          const std::string& path,
                          wave::Cycle cycle,
                          std::uint64_t value) {
    std::map<std::string, wave::TrackId>::const_iterator it = index.track_by_path.find(path);
    if (it == index.track_by_path.end()) return false;
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        const wave::TrackEvent& ev = sink.events[i];
        if (ev.track_id == it->second && ev.cycle == cycle && ev.has_u64 && ev.u64_value == value) {
            return true;
        }
    }
    return false;
}

static bool has_bool_event(const wave::InMemoryWaveSink& sink,
                           const TraceIndex& index,
                           const std::string& path,
                           wave::Cycle cycle,
                           bool value) {
    std::map<std::string, wave::TrackId>::const_iterator it = index.track_by_path.find(path);
    if (it == index.track_by_path.end()) return false;
    for (std::size_t i = 0; i < sink.events.size(); ++i) {
        const wave::TrackEvent& ev = sink.events[i];
        if (ev.track_id == it->second && ev.cycle == cycle && ev.has_bool && ev.bool_value == value) {
            return true;
        }
    }
    return false;
}

static bool patch_u32_at(const wave::InMemoryWaveSink& sink,
                         wave::NodeId node_id,
                         wave::Cycle cycle,
                         std::uint64_t byte_offset,
                         std::uint32_t expected) {
    unsigned char bytes[sizeof(expected)] = {};
    for (std::size_t byte = 0; byte < sizeof(expected); ++byte) {
        const std::uint64_t target = byte_offset + byte;
        bool found = false;
        for (std::size_t i = sink.array_patches.size(); i != 0; --i) {
            const wave::InMemoryWaveSink::OwnedArrayPatch& patch = sink.array_patches[i - 1u];
            if (patch.node_id != node_id || patch.cycle > cycle || target < patch.byte_offset) continue;
            const std::uint64_t local = target - patch.byte_offset;
            if (local >= patch.data.size()) continue;
            bytes[byte] = patch.data[static_cast<std::size_t>(local)];
            found = true;
            break;
        }
        if (!found) return false;
    }
    std::uint32_t actual = 0;
    std::memcpy(&actual, bytes, sizeof(actual));
    return actual == expected;
}

static int fail(const char* msg) {
    std::cerr << msg << "\n";
    return 1;
}

int main() {
    Top top;

    wave::InMemoryWaveSink sink;
    wave::BuildOptions opt;
    opt.emit_track_decl_path = true;
    opt.enable_dirty_peek_groups = true;
    opt.enable_wave_value_dirty = true;
    opt.enable_wave_array_dirty = true;
    opt.enable_flat_leaf_fast_table = true;

    wave::ensure_dynamic_type_registered<MetricValue>();

    wave::Tracer tracer(sink, opt);
    tracer.add_root("top", &top);
    tracer.sample(0);

    const TraceIndex index = build_index(sink);
    const char* required_paths[] = {
        "top.friend_box.secret",
        "top.direct_raw.value",
        "top.direct_unique.value",
        "top.direct_shared.value",
        "top.wave_raw.value",
        "top.wave_raw_array.[0].value",
        "top.wave_raw_array.[1].value",
        "top.wave_shared.value",
        "top.peek.count",
        "top.port.count",
        "top.dirty_counter",
        "top.flag_bool"
    };
    for (std::size_t i = 0; i < sizeof(required_paths) / sizeof(required_paths[0]); ++i) {
        if (!has_decl(index, required_paths[i])) {
            std::cerr << "missing declaration: " << required_paths[i] << "\n";
            for (std::size_t j = 0; j < sink.declarations.size(); ++j) {
                std::cerr << "  decl: " << sink.declarations[j].path << "\n";
            }
            return 2;
        }
    }
    if (has_decl(index, "top.plain_ptr.value")) {
        return fail("unmarked plain pointer was expanded");
    }
    if (sink.array_block_declarations.size() != 1u ||
        sink.array_block_declarations[0].element_count != top.slots.size() ||
        sink.array_block_declarations[0].element_stride != sizeof(ArraySlot) ||
        sink.array_block_declarations[0].schema.size() != 3u) {
        return fail("wave::array compact declaration mismatch");
    }
    const wave::NodeId slots_node_id = sink.array_block_declarations[0].node_id;

    if (!has_u64_event(sink, index, "top.friend_box.secret", 0, 7)) return fail("private friend value missing");
    if (!has_u64_event(sink, index, "top.direct_raw.value", 0, 10)) return fail("direct raw pointer value missing");
    if (!has_u64_event(sink, index, "top.direct_unique.value", 0, 20)) return fail("direct unique_ptr value missing");
    if (!has_u64_event(sink, index, "top.direct_shared.value", 0, 30)) return fail("direct shared_ptr value missing");
    if (!has_u64_event(sink, index, "top.wave_raw.value", 0, 40)) return fail("annotated raw pointer value missing");
    if (!has_u64_event(sink, index, "top.wave_raw_array.[0].value", 0, 70)) return fail("annotated raw pointer array element 0 missing");
    if (!has_u64_event(sink, index, "top.wave_raw_array.[1].value", 0, 80)) return fail("annotated raw pointer array element 1 missing");
    if (!has_u64_event(sink, index, "top.wave_shared.value", 0, 50)) return fail("annotated shared pointer value missing");
    if (!has_u64_event(sink, index, "top.peek.count", 0, 11)) return fail("peek initial value missing");
    if (!has_u64_event(sink, index, "top.port.count", 0, 12)) return fail("port-derived peek initial value missing");

    const std::size_t event_count_after_initial = sink.events.size();
    tracer.sample(1);
    if (sink.events.size() != event_count_after_initial) {
        return fail("unchanged sample emitted extra events");
    }

    const std::size_t peek_count_before_unmarked = event_count_for(sink, index, "top.peek.count");
    const std::size_t port_count_before_unmarked = event_count_for(sink, index, "top.port.count");
    top.peek.value.count = 111;
    top.port_channel.value.count = 112;
    tracer.sample(2);
    if (event_count_for(sink, index, "top.peek.count") != peek_count_before_unmarked) {
        return fail("peek dirty source updated without dirty mark");
    }
    if (event_count_for(sink, index, "top.port.count") != port_count_before_unmarked) {
        return fail("port dirty source updated without dirty mark");
    }

    top.peek.wave_dirty_hook()->mark_dirty();
    top.port_channel.wave_dirty_hook()->mark_dirty();
    top.dirty_counter = 77;
    top.slots[2].count = 55;
    top.flag_bool = 3;
    top.direct_raw->value = 99;
    top.wave_raw_array_targets[1].value = 88;
    tracer.sample(3);

    if (!has_u64_event(sink, index, "top.peek.count", 3, 111)) return fail("dirty peek update missing");
    if (!has_u64_event(sink, index, "top.port.count", 3, 112)) return fail("dirty port update missing");
    if (!has_u64_event(sink, index, "top.dirty_counter", 3, 77)) return fail("WaveValue dirty update missing");
    if (!patch_u32_at(sink, slots_node_id, 3,
                      2u * sizeof(ArraySlot) + offsetof(ArraySlot, count), 55u)) {
        return fail("wave::array dirty patch missing");
    }
    if (!has_bool_event(sink, index, "top.flag_bool", 3, true)) return fail("bool storage update missing");
    if (!has_u64_event(sink, index, "top.direct_raw.value", 3, 99)) return fail("direct pointer update missing");
    if (!has_u64_event(sink, index, "top.wave_raw_array.[1].value", 3, 88)) return fail("annotated raw pointer array update missing");

    std::cout << "markers/pointer/dirty coverage passed: tracks="
              << sink.declarations.size()
              << " events=" << sink.events.size()
              << "\n";
    return 0;
}
