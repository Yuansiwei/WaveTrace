#include "wave_runtime.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

struct RollbackGoodPayload {
    std::uint32_t value;
};

struct RollbackEmptyPayload {};

struct RollbackGoodPeek
    : wave::PeekTraceSourceFor<RollbackGoodPeek, RollbackGoodPayload> {
    RollbackGoodPayload payload;

    const RollbackGoodPayload* peek() const {
        return &payload;
    }
};

struct RollbackEmptyPeek
    : wave::PeekTraceSourceFor<RollbackEmptyPeek, RollbackEmptyPayload> {
    RollbackEmptyPayload payload;

    const RollbackEmptyPayload* peek() const {
        return &payload;
    }
};

struct RollbackRoot {
    std::size_t count;
    RollbackGoodPeek* good;
    RollbackEmptyPeek* empty;

    RollbackRoot() : count(0), good(NULL), empty(NULL) {}
};

namespace reflect {

template<> struct is_reflected<RollbackGoodPayload> : std::true_type {};
template<> struct reflected_visitor<RollbackGoodPayload> {
    template<class P, class V, class G>
    static void visit(const RollbackGoodPayload* object, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(object->value));
    }
};

template<> struct is_reflected<RollbackEmptyPayload> : std::true_type {};
template<> struct reflected_visitor<RollbackEmptyPayload> {
    template<class P, class V, class G>
    static void visit(const RollbackEmptyPayload*, P&&, V&&, G&&) {}
};

template<> struct is_reflected<RollbackRoot> : std::true_type {};
template<> struct reflected_visitor<RollbackRoot> {
    template<class P, class V, class G>
    static void visit(const RollbackRoot* object, P&& on_ptr, V&&, G&&) {
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "good", object->good, object->count);
        wave::detail::invoke_annotated_ptr_visitor(
            on_ptr, "empty", object->empty, object->count);
    }
};

} // namespace reflect

class RollbackCountingSink : public wave::IWaveSink {
public:
    std::size_t tracks = 0;

    void on_track_declared(const wave::TrackDecl&) override {
        ++tracks;
    }

    void on_sample(const wave::TrackEvent&) override {}
};

int main(int argc, char** argv) {
    std::size_t count = 50000u;
    if (argc > 1) {
        count = static_cast<std::size_t>(std::strtoull(argv[1], NULL, 10));
    }
    if (count == 0) return 64;

    std::unique_ptr<RollbackGoodPeek[]> good(new RollbackGoodPeek[count]);
    std::unique_ptr<RollbackEmptyPeek[]> empty(new RollbackEmptyPeek[count]);
    for (std::size_t i = 0; i < count; ++i) {
        good[i].payload.value = static_cast<std::uint32_t>(i);
    }

    RollbackRoot root;
    root.count = count;
    root.good = good.get();
    root.empty = empty.get();

    wave::ensure_dynamic_type_registered<RollbackGoodPayload>();
    wave::ensure_dynamic_type_registered<RollbackEmptyPayload>();

    wave::BuildOptions options;
    options.enable_parallel_topology_expansion = false;
    options.enable_dirty_peek_groups = true;
    options.emit_track_decl_path = false;
    options.debug_log = false;
    options.debug_log_root_expand_stats = false;
    options.dump_leaf_distribution_after_topology = false;
    options.dump_memory_usage_after_topology = false;

    RollbackCountingSink sink;
    const std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();
    wave::Tracer tracer(sink, options);
    tracer.add_root("top", &root);
    tracer.prepare_topology(0);
    const std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();

    const std::uint64_t elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
    if (sink.tracks != count) {
        std::cerr << "incremental_rollback_track_mismatch expected=" << count
                  << " actual=" << sink.tracks << "\n";
        return 1;
    }

    std::cout << "incremental_topology_rollback_ok"
              << " valid_peeks=" << count
              << " empty_peeks=" << count
              << " tracks=" << sink.tracks
              << " topology_us=" << elapsed_us
              << "\n";
    return 0;
}
