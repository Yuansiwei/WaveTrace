#pragma once

// Manual-cycle WaveTap wrapper for the clean external-recorder workflow.
//
// This wrapper intentionally does not own Tracer or PathStableWvz4Recorder.
// Your business program owns and opens/closes the recorder, owns the tracer,
// registers roots, and calls sample_one_cycle() once after each stable business
// cycle. WaveTap owns only the monotonically increasing business-cycle counter
// used by the recorder/tracer sampling sequence:
//
//   recorder.begin_cycle(cycle);
//   tracer.sample(cycle);
//   recorder.end_cycle(cycle, error);
//   ++cycle;
//
// Business code does NOT pass a cycle number and does NOT call
// prepare_topology(). The first sample_one_cycle() lazily freezes the topology,
// builds dirty lookup tables, opens the WVZ4 writer layout, and then records
// cycle 0.
//
// WaveTap has no SystemC dependency and does not derive from sc_module.

#if defined(min)
#pragma push_macro("min")
#undef min
#define WAVE_TAP_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define WAVE_TAP_RESTORE_MAX_MACRO_ 1
#endif

#include "wave_path_wvz4_recorder.h"

#include <sstream>
#include <string>

namespace wave {

class WaveTap {
public:
    WaveTap() = delete;
    WaveTap(const WaveTap&) = delete;
    WaveTap& operator=(const WaveTap&) = delete;

    WaveTap(Tracer& tracer, ::PathStableWvz4Recorder& recorder)
        : tracer_(&tracer), recorder_(&recorder) {}

    ~WaveTap() = default;

    // Root registration belongs to Tracer, not WaveTap.
    // Use:
    //     tracer.add_root("gpu", g_GPUTop);
    // before constructing/using WaveTap.  WaveTap intentionally has no add_root()
    // wrapper so there is only one public ownership path for topology.

    void set_attach_sample_thread(bool enabled) noexcept {
        attach_sample_thread_ = enabled;
    }

    bool attach_sample_thread() const noexcept {
        return attach_sample_thread_;
    }

    void attach_current_thread() {
        if (tracer_) tracer_->attach_current_thread_for_dirty_peek();
    }

    void detach_current_thread() noexcept {
        if (tracer_) tracer_->detach_current_thread_for_dirty_peek();
    }

    // Samples exactly one stable business cycle and then advances the internal
    // cycle counter. Worker threads must already be at a barrier/join point
    // before this call; this method is not a concurrent snapshot mechanism.
    //
    // This is the only public sampling API by design: business code should call
    // tap.sample_one_cycle() once per completed cycle and should not pass cycle
    // numbers by hand. On failure, the internal cycle counter is not advanced
    // so the caller can inspect last_error() and decide whether to retry/abort.
    bool sample_one_cycle() {
        std::string error;
        const bool ok = sample_one_cycle_impl_(error);
        if (!ok) last_error_ = error;
        return ok;
    }

    Cycle next_cycle() const noexcept { return next_cycle_; }

    bool is_topology_prepared() const noexcept { return topology_prepared_; }

    const std::string& last_error() const noexcept { return last_error_; }

    Tracer& tracer() noexcept { return *tracer_; }
    const Tracer& tracer() const noexcept { return *tracer_; }

    ::PathStableWvz4Recorder& recorder() noexcept { return *recorder_; }
    const ::PathStableWvz4Recorder& recorder() const noexcept { return *recorder_; }

private:
    bool sample_one_cycle_impl_(std::string& error) {
        error.clear();
        if (!tracer_ || !recorder_) {
            error = "WaveTap::sample_one_cycle failed: invalid tracer/recorder";
            return false;
        }

        const Cycle cycle = next_cycle_;
        // Lazy topology freeze. User code does not call prepare_topology().
        // This is intentionally done before begin_cycle() so topology/layout
        // declaration failures cannot leave a partially-open cycle frame.
        if (!ensure_topology_prepared_(cycle, error)) {
            return false;
        }

        if (attach_sample_thread_) {
            tracer_->attach_current_thread_for_dirty_peek();
        }

        recorder_->begin_cycle(cycle);
        tracer_->sample(cycle);
        if (!recorder_->end_cycle(cycle, error)) {
            return false;
        }

        ++next_cycle_;
        last_error_.clear();
        return true;
    }

    bool ensure_topology_prepared_(Cycle cycle, std::string& error) {
        if (!tracer_ || !recorder_) {
            error = "WaveTap::sample_one_cycle failed: invalid tracer/recorder";
            return false;
        }

        // topology_prepared_ means the topology is frozen AND the recorder writer
        // has been successfully pre-opened.  It must not mean merely "we once
        // called Tracer::prepare_topology()"; failed/empty lazy expansion must be
        // retried on the next sample_one_cycle() instead of locking the tap into
        // a false-ready state.
        if (!topology_prepared_) {
            tracer_->prepare_topology(cycle);
        }

        // If the user registered at least one root, opening a clock-only file is
        // almost always a bug: either reflection did not instantiate, the root
        // pointer/type is wrong, or the first lazy expansion was empty. Fail here
        // before the recorder opens an irreversible layout.
        if (tracer_->root_watch_count() != 0u && tracer_->tracks().size() <= 1u) {
            error = "WaveTap lazy topology produced no reflected tracks after add_root; ";
            error += state_summary_();
            return false;
        }

        if (tracer_->root_watch_count() != 0u && tracer_->expanded_root_watch_count() == 0u) {
            error = "WaveTap lazy topology has registered roots but none expanded successfully; ";
            error += state_summary_();
            return false;
        }

        if (attach_sample_thread_) {
            tracer_->attach_current_thread_for_dirty_peek();
        }

        if (!writer_preopened_) {
            // If tracks exist but no NodeDecl reached the recorder, this is a
            // topology export bug, not an empty-root situation.  Fail here with
            // high-signal diagnostics instead of letting end_cycle() report a
            // misleading generic "topology is not declared" message.
            if (tracer_->tracks().size() > 1u && recorder_->declared_node_count() == 0u) {
                error = "WaveTap topology export mismatch before writer open: tracer has tracks but recorder has zero declared nodes";
                error += "; root_watches=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->root_watch_count()));
                error += " expanded_roots=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->expanded_root_watch_count()));
                error += " tracer_nodes=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->nodes().size()));
                error += " tracer_tracks=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->tracks().size()));
                error += " recorder_nodes=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(recorder_->declared_node_count()));
                error += " recorder_tracks=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(recorder_->declared_track_count()));
                error += " nodes_exported=";
                error += tracer_->node_declarations_exported() ? "1" : "0";
                error += " writer_preopened=0";
                return false;
            }

            // Not a user call: WaveTap pre-opens the writer after lazy topology
            // preparation and before begin_cycle().  PathStableWvz4Recorder also
            // keeps a defensive open in end_cycle() for callers that bypass
            // WaveTap, but the normal sample_one_cycle() path should not rely
            // on that fallback.
            if (!recorder_->open_writer_if_needed(error)) {
                if (!error.empty()) {
                    error += "; ";
                }
                error += "WaveTap lazy topology state: root_watches=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->root_watch_count()));
                error += " expanded_roots=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->expanded_root_watch_count()));
                error += " tracer_nodes=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->nodes().size()));
                error += " tracer_tracks=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(tracer_->tracks().size()));
                error += " recorder_nodes=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(recorder_->declared_node_count()));
                error += " recorder_tracks=";
                error += detail::to_string_unsigned(static_cast<std::uint64_t>(recorder_->declared_track_count()));
                error += " nodes_exported=";
                error += tracer_->node_declarations_exported() ? "1" : "0";
                error += " writer_preopened=0";
                error += "; ";
                error += state_summary_();
                return false;
            }
            writer_preopened_ = true;
            topology_prepared_ = true;
        } else {
            // Defensive invariant: any path that has a preopened writer is also
            // considered topology-prepared from WaveTap's point of view.
            topology_prepared_ = true;
        }
        return true;
    }

private:
    std::string state_summary_() const {
        std::ostringstream os;
        os << "tap_next_cycle=" << static_cast<unsigned long long>(next_cycle_)
           << " topology_prepared=" << (topology_prepared_ ? 1 : 0)
           << " writer_preopened=" << (writer_preopened_ ? 1 : 0);
        if (tracer_) {
            os << " tracer{" << tracer_->topology_debug_summary(6) << "}";
        } else {
            os << " tracer=null";
        }
        if (recorder_) {
            os << " recorder{" << recorder_->debug_state_summary() << "}";
        } else {
            os << " recorder=null";
        }
        return os.str();
    }

private:
    Tracer* tracer_ = nullptr;
    ::PathStableWvz4Recorder* recorder_ = nullptr;
    Cycle next_cycle_ = 0;
    bool topology_prepared_ = false;
    bool writer_preopened_ = false;
    bool attach_sample_thread_ = true;
    std::string last_error_;
};

} // namespace wave

#if defined(WAVE_TAP_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef WAVE_TAP_RESTORE_MAX_MACRO_
#endif
#if defined(WAVE_TAP_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef WAVE_TAP_RESTORE_MIN_MACRO_
#endif
