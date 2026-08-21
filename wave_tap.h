#pragma once

// Business-cycle WaveTap wrapper for the clean external-recorder workflow.
//
// This wrapper intentionally does not own Tracer or PathStableWvz4Recorder.
// Your business program owns and opens/closes the recorder, owns the tracer,
// and registers roots.  In a SystemC build WaveTap is an sc_module: its
// constructor registers a supplied sc_clock and automatically samples on every
// falling edge.  Without SystemC the manual sample_one_cycle() API remains
// available.  WaveTap owns only the monotonically increasing business-cycle
// counter used by the recorder/tracer sampling sequence:
//
//   recorder.begin_cycle(cycle);
//   tracer.sample(cycle);
//   recorder.end_cycle(cycle, error);
//   ++cycle;
//
// Business code does NOT pass a cycle number and does NOT call
// prepare_topology(). The first falling-edge/manual sample lazily freezes the
// topology, builds dirty lookup tables, opens the WVZ4 writer layout, and then
// records cycle 0.
//
// WaveTap also samples once in start_of_simulation(). The falling-edge process
// uses dont_initialize(), so the initial callback and the first clock edge are
// two explicit, non-overlapping samples.

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

#if defined(__has_include)
#if __has_include(<systemc.h>)
#include <systemc.h>
#define WAVE_TAP_HAS_SYSTEMC_ 1
#endif
#endif

#if defined(WAVE_TAP_HAS_SYSTEMC_)
#include <iostream>
#endif
#include <cstdint>
#include <exception>
#include <limits>
#include <sstream>
#include <string>

namespace wave {

#if defined(WAVE_TAP_HAS_SYSTEMC_)
class WaveTap : public sc_core::sc_module {
#else
class WaveTap {
#endif
public:
#if defined(WAVE_TAP_HAS_SYSTEMC_)
    SC_HAS_PROCESS(WaveTap);
#endif

    WaveTap() = delete;
    WaveTap(const WaveTap&) = delete;
    WaveTap& operator=(const WaveTap&) = delete;

#if defined(WAVE_TAP_HAS_SYSTEMC_)
    WaveTap(sc_core::sc_module_name name,
            Tracer& tracer,
            ::PathStableWvz4Recorder& recorder,
            sc_core::sc_clock& clock)
        : sc_core::sc_module(name),
          tracer_(&tracer),
          recorder_(&recorder) {
        if (tracer_->trace_cycle_zero_only()) {
            recorder_->disable_lod_tables_for_cycle_zero_snapshot();
        }
        if (automatic_sampling_required_()) {
            SC_METHOD(sample_on_clock_falling_edge_);
            sensitive << clock.negedge_event();
            dont_initialize();
        }
    }
#else
    WaveTap(Tracer& tracer, ::PathStableWvz4Recorder& recorder)
        : tracer_(&tracer), recorder_(&recorder) {
        if (tracer_->trace_cycle_zero_only()) {
            recorder_->disable_lod_tables_for_cycle_zero_snapshot();
        }
    }
#endif

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
    // cycle counter. SystemC builds call this automatically before simulation
    // and on each registered clock falling edge. Worker threads must already
    // be at a barrier/join point before a sample; this method is not a
    // concurrent snapshot mechanism.
    //
    // The public method remains available for non-SystemC/manual integrations
    // and tests. Callers do not pass cycle numbers. On failure, the internal
    // cycle counter is not advanced so last_error() identifies the failed
    // automatic or manual sample.
    bool sample_one_cycle() noexcept {
        if (fatal_error_) return false;
        try {
            std::string error;
            const bool ok = sample_one_cycle_impl_(error);
            if (!ok) last_error_ = error;
            return ok;
        } catch (const std::exception& ex) {
            latch_fatal_exception_(ex.what());
            return false;
        } catch (...) {
            latch_fatal_exception_("non-standard exception");
            return false;
        }
    }

#if defined(WAVE_TAP_HAS_SYSTEMC_)
    void start_of_simulation() noexcept override {
        if (!automatic_sampling_required_()) return;
        run_automatic_sample_("start_of_simulation");
    }
#endif

    Cycle next_cycle() const noexcept { return next_cycle_; }

    bool is_topology_prepared() const noexcept { return topology_prepared_; }

    bool has_fatal_error() const noexcept { return fatal_error_; }

    const std::string& last_error() const noexcept { return last_error_; }

    Tracer& tracer() noexcept { return *tracer_; }
    const Tracer& tracer() const noexcept { return *tracer_; }

    ::PathStableWvz4Recorder& recorder() noexcept { return *recorder_; }
    const ::PathStableWvz4Recorder& recorder() const noexcept { return *recorder_; }

private:
#if defined(WAVE_TAP_HAS_SYSTEMC_)
    static bool automatic_sampling_required_() {
        const ::wave::config::RuntimeConfig& config =
            ::wave::config::runtime_config();
        // Malformed configuration must still reach sample_one_cycle() so its
        // existing diagnostic is reported. Only an explicit, valid disabled
        // state suppresses automatic SystemC sampling entirely.
        return !config.valid || config.wave_trace;
    }

    void sample_on_clock_falling_edge_() noexcept {
        run_automatic_sample_("falling-edge");
    }

    void run_automatic_sample_(const char* source) noexcept {
        if (sample_one_cycle()) {
            automatic_error_reported_ = false;
            return;
        }
        report_automatic_failure_once_(source);
    }

    void report_automatic_failure_once_(const char* source) noexcept {
        if (automatic_error_reported_) return;
        automatic_error_reported_ = true;
        try {
            std::cerr << "[wave] " << (source ? source : "automatic")
                      << " sample failed: "
                      << (last_error_.empty() ? "unknown WaveTrace failure" : last_error_)
                      << "\n";
        } catch (...) {
            // The SystemC callback is an absolute exception boundary. Even a
            // diagnostic stream configured to throw must not escape into the
            // simulation kernel.
        }
    }
#endif

    void latch_fatal_exception_(const char* message) noexcept {
        fatal_error_ = true;
        try {
            std::ostringstream os;
            os << "WaveTap fatal sampling exception at cycle "
               << static_cast<unsigned long long>(next_cycle_)
               << ": " << (message ? message : "unknown exception");
            last_error_ = os.str();
        } catch (...) {
            // Preserve the fatal latch even if diagnostics cannot allocate.
            try {
                last_error_ = "WaveTap fatal sampling exception";
            } catch (...) {
                last_error_.clear();
            }
        }
    }

    bool sample_one_cycle_impl_(std::string& error) {
        error.clear();
        if (!tracer_ || !recorder_) {
            error = "WaveTap::sample_one_cycle failed: invalid tracer/recorder";
            return false;
        }

        const Cycle cycle = next_cycle_;
        const ::wave::config::RuntimeConfig& config = ::wave::config::runtime_config();
        if (!config.valid) {
            error = "WaveTap::sample_one_cycle failed: invalid WaveTrace config '";
            error += config.path;
            error += "': ";
            error += config.error;
            return false;
        }
        // Manual callers may still invoke sample_one_cycle() while tracing is
        // disabled. Treat that as a quiet successful no-op.
        if (!config.wave_trace) return finish_skipped_cycle_(cycle, false);

        const std::uint64_t cycle_value = static_cast<std::uint64_t>(cycle);
        const bool outside_capture_window =
            cycle_value < config.wave_trace_start ||
            cycle_value > config.wave_trace_end ||
            (tracer_->trace_cycle_zero_only() && cycle != 0);
        if (outside_capture_window) return finish_skipped_cycle_(cycle, true);

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

    bool finish_skipped_cycle_(Cycle cycle, bool show_progress) {
        if (show_progress) tracer_->maybe_print_cycle_progress(cycle, false);
        if (next_cycle_ != (std::numeric_limits<Cycle>::max)()) ++next_cycle_;
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
        if (tracer_->root_watch_count() != 0u && tracer_->tracks().size() <= 1u &&
            tracer_->compact_array_block_count() == 0u) {
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
    bool fatal_error_ = false;
#if defined(WAVE_TAP_HAS_SYSTEMC_)
    bool automatic_error_reported_ = false;
#endif
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

#if defined(WAVE_TAP_HAS_SYSTEMC_)
#undef WAVE_TAP_HAS_SYSTEMC_
#endif
