#pragma once

// Protect this public header from Windows-style min/max function-like macros.
// Some business environments define min/max before including project headers;
// those macros break std::min/std::max and std::numeric_limits<T>::max().
#if defined(min)
#pragma push_macro("min")
#undef min
#define WAVE_PATH_WVZ4_RECORDER_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define WAVE_PATH_WVZ4_RECORDER_RESTORE_MAX_MACRO_ 1
#endif

#if defined(new)
#undef new
#endif
#if defined(make_shared)
#undef make_shared
#endif
#if defined(make_unique)
#undef make_unique
#endif

#include "wave_runtime.h"
#include "wvz4_writer_typed.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Stable-topology WVZ4 recorder.
// - Node tree is collected from wave::NodeDecl generated during reflection expansion.
// - Signal ids are fixed at track declaration time. By default signal_id 1 is reserved for the synthetic clk; reflected tracks use signal_id == track_id + 1.
// - No dynamic add/remove, no Z/invalid samples, no string/custom fallback.
// - WVZ4 writer is opened lazily after topology is known; call open_writer_if_needed()
//   after tracer.prepare_topology() if you want layout I/O before cycle 0 sampling.
class PathStableWvz4Recorder : public wave::IWaveSink {
public:
    struct OpenConfig {
        std::string file_path = "wave.wvz4";
        wvz4::WriterOptions options{};
        bool async_writer = true;
        std::size_t async_writer_queue_limit = 256;
        std::size_t async_writer_queue_bytes_limit = 256u * 1024u * 1024u;

        // Crash/kill resistant helper mode. Enabled by default: the simulation
        // process sends layout/cycle frames to a separate writer process. The
        // helper owns the real WVZ4 Writer and finalizes the file if the parent
        // process exits, crashes, or is killed by VS Stop Debugging.
        //
        // Disable only for explicit scratch/perf runs where direct in-process
        // writing is acceptable.
        bool use_writer_process = true;
        std::string writer_process_exe_path;
        unsigned int writer_process_connect_timeout_ms = 10000;

        // WVZ4 v4 synthetic periodic clock. Enabled by default. The clock is
        // stored as CLKD(initial_value + period_ticks), not as ordinary WDAT
        // transitions. Business-cycle samples are still mapped to
        // cycle * clk_period_ticks so they align to clock edges.
        bool emit_default_clk = true;
        std::string default_clk_name = "clk";
        bool clk_initial_value = false;
        wvz4::u64 clk_period_ticks = 10;
        // In v4 this is interpreted as simple toggle period for CLKD. Keep the
        // old field name to preserve existing config source compatibility.
        wvz4::u64 clk_fall_offset_ticks = 5;
    };

    PathStableWvz4Recorder() = default;
    ~PathStableWvz4Recorder() { std::string ignored; close(ignored); }

    PathStableWvz4Recorder(const PathStableWvz4Recorder&) = delete;
    PathStableWvz4Recorder& operator=(const PathStableWvz4Recorder&) = delete;

    bool open(const OpenConfig& cfg, std::string& error) {
        error.clear();
        if (opened_) {
            error = "PathStableWvz4Recorder::open failed: already open";
            return false;
        }
        reset_session_state();
        cfg_ = cfg;
        opened_ = true;
        writer_opened_ = false;
        cycle_open_ = false;
        last_error_.clear();
        frame_work_reserved_capacity_ = 0;
        return true;
    }

    bool has_declared_topology() const noexcept {
        return !declared_node_ids_.empty();
    }

    bool has_declared_track_topology() const noexcept {
        return !declared_track_ids_.empty();
    }

    std::size_t declared_node_count() const noexcept {
        return declared_node_ids_.size();
    }

    std::size_t declared_track_count() const noexcept {
        return declared_track_ids_.size();
    }

    bool writer_is_open() const noexcept {
        return writer_opened_;
    }

    std::string debug_state_summary() const {
        std::ostringstream os;
        os << "opened=" << (opened_ ? 1 : 0)
           << " writer_opened=" << (writer_opened_ ? 1 : 0)
           << " cycle_open=" << (cycle_open_ ? 1 : 0)
           << " current_cycle=" << static_cast<unsigned long long>(current_cycle_)
           << " node_states=" << node_states_.size()
           << " track_states=" << track_states_.size()
           << " declared_nodes=" << declared_node_ids_.size()
           << " declared_tracks=" << declared_track_ids_.size()
           << " frame_slots=" << frame_slots_.size()
           << " pending_samples=" << current_sample_ids_.size()
           << " emit_default_clk=" << (cfg_.emit_default_clk ? 1 : 0)
           << " async=" << (cfg_.async_writer ? 1 : 0)
           << " writer_process=" << (cfg_.use_writer_process ? 1 : 0);
        if (!last_error_.empty()) os << " last_error='" << last_error_ << "'";
        return os.str();
    }

    bool close(std::string& error) {
        error.clear();
        if (!opened_) return true;
        bool ok = true;
        std::string local_error;
        if (writer_opened_) {
            if (cfg_.use_writer_process) {
                if (!process_writer_.close(local_error)) ok = false;
            } else if (cfg_.async_writer) {
                if (!async_writer_.close(local_error)) ok = false;
            } else {
                if (!writer_.close(local_error)) ok = false;
            }
        }
        reset_session_state();
        if (!ok) error = local_error.empty() ? "WVZ4 recorder close failed" : local_error;
        return ok;
    }

    // Optional: after tracer.prepare_topology(), call this before cycle 0 to write
    // the WVZ4 layout outside the first sampled cycle.
    bool open_writer_if_needed(std::string& error) {
        error.clear();
        if (!opened_) { error = "WVZ4 recorder is not open"; return false; }
        if (writer_opened_) return true;
        // In v4, a clock-only file is a valid layout when the synthetic
        // periodic clock is enabled.  This matters for the lazy WaveTap path:
        // first sample may open the writer even if the reflected root is empty
        // or if the user intentionally wants only the default clock.
        if (node_states_.empty() && !cfg_.emit_default_clk) {
            error = "WVZ4 recorder cannot open writer before topology is declared; ";
            error += debug_state_summary();
            return false;
        }
        if (track_states_.empty() && !cfg_.emit_default_clk) {
            error = "WVZ4 recorder cannot open writer before track topology is declared; ";
            error += debug_state_summary();
            return false;
        }
        wvz4::Layout layout;
        if (!build_layout(layout, error)) return false;
        if (cfg_.use_writer_process) {
            if (!process_writer_.open(cfg_.file_path,
                                      layout,
                                      cfg_.options,
                                      error,
                                      cfg_.writer_process_exe_path,
                                      cfg_.writer_process_connect_timeout_ms)) return false;
        } else if (cfg_.async_writer) {
            if (!async_writer_.open(cfg_.file_path, layout, cfg_.options, error, cfg_.async_writer_queue_limit, cfg_.async_writer_queue_bytes_limit)) return false;
        } else {
            if (!writer_.open(cfg_.file_path, layout, cfg_.options, error)) return false;
        }
        writer_opened_ = true;
        ensure_frame_work_capacity_prepared();
        return true;
    }

    void begin_cycle(wave::Cycle cycle) {
        if (cycle_open_) {
            set_error("WVZ4 recorder begin_cycle failed: previous cycle is still open");
            return;
        }
        current_cycle_ = cycle;
        clear_frame_work();
        cycle_open_ = true;
        // last_error_ is intentionally sticky.  Topology/layout errors may be
        // raised by prepare_topology() before begin_cycle(); clearing them here
        // would hide invalid NodeTable/SignalTable construction.
        if (!opened_) {
            set_error("WVZ4 recorder begin_cycle failed: recorder is not open");
        }
        if (cycle > static_cast<wave::Cycle>(std::numeric_limits<wvz4::i64>::max())) {
            set_error("WVZ4 recorder begin_cycle failed: cycle exceeds int64 range");
        }
        ensure_frame_work_capacity_prepared();
    }

    bool end_cycle(wave::Cycle cycle, std::string& error) {
        error.clear();
        if (!opened_) { error = "WVZ4 recorder end_cycle failed: recorder is not open"; return false; }
        if (!cycle_open_) { error = "WVZ4 recorder end_cycle failed: begin_cycle was not called"; return false; }
        if (cycle != current_cycle_) {
            cycle_open_ = false;
            clear_frame_work();
            error = "WVZ4 recorder end_cycle failed: cycle mismatch";
            return false;
        }
        if (!last_error_.empty()) {
            error = last_error_;
            cycle_open_ = false;
            clear_frame_work();
            return false;
        }
        if (!open_writer_if_needed(error)) {
            cycle_open_ = false;
            clear_frame_work();
            return false;
        }

        wvz4::i64 writer_cycle = 0;
        if (!map_business_cycle_to_writer_cycle(cycle, writer_cycle, error)) {
            cycle_open_ = false;
            clear_frame_work();
            return false;
        }

        submission_work_.cycle = writer_cycle;
        submission_work_.updates.clear();
        for (std::size_t i = 0; i < current_sample_ids_.size(); ++i) {
            const wave::TrackId tid = current_sample_ids_[i];
            if (tid >= frame_slots_.size()) continue;
            const FrameSlot& slot = frame_slots_[static_cast<std::size_t>(tid)];
            if (!slot.has_sample) continue;
            submission_work_.updates.push_back(slot.update);
        }

        bool ok = true;
        if (!submission_work_.updates.empty()) {
            ok = submit_to_writer(submission_work_, error);
        }

        clear_frame_work();
        cycle_open_ = false;
        return ok;
    }

    void on_node_declared(const wave::NodeDecl& decl) override {
        if (writer_opened_) {
            set_error("WVZ4 recorder on_node_declared failed: topology changed after writer open");
            return;
        }
        if (decl.node_id == 0) { set_error("WVZ4 recorder on_node_declared failed: node_id is zero"); return; }
        if (decl.node_id > static_cast<wave::NodeId>(std::numeric_limits<wvz4::u32>::max())) {
            set_error("WVZ4 recorder on_node_declared failed: node_id exceeds uint32 range"); return;
        }
        if (decl.parent_id > static_cast<wave::NodeId>(std::numeric_limits<wvz4::u32>::max())) {
            set_error("WVZ4 recorder on_node_declared failed: parent_id exceeds uint32 range"); return;
        }
        if (decl.name.empty()) { set_error("WVZ4 recorder on_node_declared failed: empty node name"); return; }
        ensure_node_capacity(decl.node_id);
        NodeState& st = node_states_[static_cast<std::size_t>(decl.node_id)];
        if (st.declared) { set_error("WVZ4 recorder on_node_declared failed: duplicate node_id"); return; }
        st.declared = true;
        st.node_id = static_cast<wvz4::u32>(decl.node_id);
        st.parent_id = static_cast<wvz4::u32>(decl.parent_id);
        st.name = decl.name;
        st.kind = map_node_kind(decl.kind);
        declared_node_ids_.push_back(st.node_id);
    }

    void on_track_declared(const wave::TrackDecl& decl) override {
        if (writer_opened_) {
            set_error("WVZ4 recorder on_track_declared failed: topology changed after writer open");
            return;
        }
        if (decl.track_id == 0) { set_error("WVZ4 recorder on_track_declared failed: track_id is zero"); return; }
        const wave::TrackId max_track_id = cfg_.emit_default_clk
            ? static_cast<wave::TrackId>(std::numeric_limits<wvz4::u32>::max() - 1u)
            : static_cast<wave::TrackId>(std::numeric_limits<wvz4::u32>::max());
        if (decl.track_id > max_track_id) {
            set_error("WVZ4 recorder on_track_declared failed: track_id exceeds signal_id uint32 range"); return;
        }
        const wave::TrackId logical_storage_id = decl.storage_id != 0 ? decl.storage_id : decl.track_id;
        if (logical_storage_id == 0 || logical_storage_id > max_track_id) {
            set_error("WVZ4 recorder on_track_declared failed: storage_id exceeds signal_id uint32 range"); return;
        }
        if ((!decl.storage_only && decl.node_id == 0) ||
            decl.node_id > static_cast<wave::NodeId>(std::numeric_limits<wvz4::u32>::max())) {
            set_error("WVZ4 recorder on_track_declared failed: invalid node_id"); return;
        }
        ensure_track_capacity(decl.track_id);
        TrackState& st = track_states_[static_cast<std::size_t>(decl.track_id)];
        if (st.declared) { set_error("WVZ4 recorder on_track_declared failed: duplicate track_id"); return; }
        st.declared = true;
        st.track_id = static_cast<wvz4::u32>(decl.track_id);
        st.signal_id = cfg_.emit_default_clk
            ? static_cast<wvz4::u32>(decl.track_id + 1u)
            : static_cast<wvz4::u32>(decl.track_id);
        st.storage_signal_id = cfg_.emit_default_clk
            ? static_cast<wvz4::u32>(logical_storage_id + 1u)
            : static_cast<wvz4::u32>(logical_storage_id);
        st.storage_track_id = logical_storage_id;
        st.node_id = static_cast<wvz4::u32>(decl.node_id);
        st.kind = decl.kind;
        st.bit_width = decl.bit_width == 0 ? static_cast<wvz4::u32>(64) : decl.bit_width;
        st.bit_offset = decl.bit_offset;
        st.storage_only = decl.storage_only;
        st.value_type = map_value_type(decl.kind, st.bit_width);
        st.radix = default_radix(decl.kind);
        declared_track_ids_.push_back(st.track_id);
    }

    void on_sample(const wave::TrackEvent& ev) override {
        if (!cycle_open_) {
            set_error("WVZ4 recorder on_sample failed: sample outside begin_cycle/end_cycle");
            return;
        }
        if (ev.invalid != wave::InvalidKind::None) {
            set_error("WVZ4 recorder on_sample failed: invalid/Z/unsupported events are not supported by WVZ4");
            return;
        }
        if (ev.track_id == 0 || ev.track_id >= track_states_.size()) {
            set_error("WVZ4 recorder on_sample failed: unknown track_id");
            return;
        }
        TrackState& st = track_states_[static_cast<std::size_t>(ev.track_id)];
        if (!st.declared) { set_error("WVZ4 recorder on_sample failed: undeclared track_id"); return; }
        const wave::TrackId frame_id = st.storage_track_id != 0 ? st.storage_track_id : ev.track_id;
        ensure_frame_capacity(frame_id);
        FrameSlot& slot = frame_slots_[static_cast<std::size_t>(frame_id)];
        if (slot.has_sample) { set_error("WVZ4 recorder on_sample failed: duplicate storage sample in one cycle"); return; }
        wvz4::CycleValueUpdate upd;
        if (!make_update(st, ev, upd)) return;
        slot.has_sample = true;
        slot.update = upd;
        current_sample_ids_.push_back(frame_id);
    }

private:
    struct NodeState {
        bool declared = false;
        wvz4::u32 node_id = 0;
        wvz4::u32 parent_id = 0;
        std::string name;
        wvz4::NodeKind kind = wvz4::NodeKind::Object;
    };

    struct TrackState {
        bool declared = false;
        wvz4::u32 track_id = 0;
        wvz4::u32 signal_id = 0;
        wvz4::u32 storage_signal_id = 0;
        wave::TrackId storage_track_id = 0;
        wvz4::u32 node_id = 0;
        wave::ValueKind kind = wave::ValueKind::Unknown;
        wvz4::ValueType value_type = wvz4::ValueType::U64;
        wvz4::u32 bit_width = 64;
        wvz4::u32 bit_offset = 0;
        wvz4::Radix radix = wvz4::Radix::Auto;
        bool storage_only = false;
    };

    struct FrameSlot {
        bool has_sample = false;
        wvz4::CycleValueUpdate update;
    };

    OpenConfig cfg_;
    bool opened_ = false;
    bool writer_opened_ = false;
    bool cycle_open_ = false;
    wave::Cycle current_cycle_ = 0;
    std::string last_error_;

    wvz4::Writer writer_;
    wvz4::AsyncWriter async_writer_;
    wvz4::WriterProcessClient process_writer_;

    std::vector<NodeState> node_states_;       // id-indexed, id 0 unused
    std::vector<TrackState> track_states_;     // track-id indexed
    std::vector<FrameSlot> frame_slots_;       // track-id indexed, touched slots cleared after each cycle
    std::vector<wvz4::u32> declared_node_ids_;
    std::vector<wvz4::u32> declared_track_ids_;
    std::vector<wave::TrackId> current_sample_ids_;
    wvz4::CycleSubmission submission_work_;
    wvz4::CycleSubmission clk_down_work_;
    std::size_t frame_work_reserved_capacity_ = 0;


    void reset_session_state() {
        opened_ = false;
        writer_opened_ = false;
        cycle_open_ = false;
        current_cycle_ = 0;
        last_error_.clear();
        clear_frame_work();
        node_states_.clear();
        track_states_.clear();
        frame_slots_.clear();
        declared_node_ids_.clear();
        declared_track_ids_.clear();
        current_sample_ids_.clear();
        submission_work_ = wvz4::CycleSubmission();
        clk_down_work_ = wvz4::CycleSubmission();
        frame_work_reserved_capacity_ = 0;
    }

    void set_error(const std::string& msg) {
        if (last_error_.empty()) last_error_ = msg;
    }

    void ensure_node_capacity(wave::NodeId id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (node_states_.size() <= index) node_states_.resize(index + 1);
    }

    void ensure_track_capacity(wave::TrackId id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (track_states_.size() <= index) track_states_.resize(index + 1);
        if (frame_slots_.size() <= index) frame_slots_.resize(index + 1);
    }

    void ensure_frame_capacity(wave::TrackId id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (frame_slots_.size() <= index) frame_slots_.resize(index + 1);
    }

    void ensure_frame_work_capacity_prepared() {
        const std::size_t sample_capacity = frame_slots_.size();
        const std::size_t update_capacity = sample_capacity + (cfg_.emit_default_clk ? 1u : 0u);

        // This must not run once per declared track. Exact reserve(track_count) on every
        // track declaration causes O(N^2) realloc/copy when large topologies are exported.
        // It is called at cycle entry / writer open and is a no-op after capacity is enough.
        if (frame_work_reserved_capacity_ >= sample_capacity &&
            current_sample_ids_.capacity() >= sample_capacity &&
            submission_work_.updates.capacity() >= update_capacity &&
            (!cfg_.emit_default_clk || clk_down_work_.updates.capacity() >= 1u)) {
            return;
        }

        if (current_sample_ids_.capacity() < sample_capacity) {
            current_sample_ids_.reserve(sample_capacity);
        }
        if (submission_work_.updates.capacity() < update_capacity) {
            submission_work_.updates.reserve(update_capacity);
        }
        if (cfg_.emit_default_clk && clk_down_work_.updates.capacity() < 1u) {
            clk_down_work_.updates.reserve(1u);
        }
        frame_work_reserved_capacity_ = sample_capacity;
    }

    void clear_frame_work() {
        for (std::size_t i = 0; i < current_sample_ids_.size(); ++i) {
            const std::size_t index = static_cast<std::size_t>(current_sample_ids_[i]);
            if (index < frame_slots_.size()) frame_slots_[index].has_sample = false;
        }
        current_sample_ids_.clear();
    }

    bool submit_to_writer(const wvz4::CycleSubmission& submission, std::string& error) {
        if (cfg_.use_writer_process) {
            return process_writer_.submit_cycle(submission, error);
        }
        if (cfg_.async_writer) {
            wvz4::CycleSubmission copy = submission;
            return async_writer_.submit_cycle(std::move(copy), error);
        }
        return writer_.submit_cycle(submission, error);
    }

    bool map_business_cycle_to_writer_cycle(wave::Cycle cycle, wvz4::i64& out, std::string& error) const {
        if (!cfg_.emit_default_clk) {
            if (cycle > static_cast<wave::Cycle>(std::numeric_limits<wvz4::i64>::max())) {
                error = "WVZ4 recorder end_cycle failed: cycle exceeds int64 range";
                return false;
            }
            out = static_cast<wvz4::i64>(cycle);
            return true;
        }
        if (cfg_.clk_period_ticks == 0) {
            error = "WVZ4 recorder config error: clk_period_ticks must be positive";
            return false;
        }
        if (cfg_.clk_fall_offset_ticks == 0) {
            error = "WVZ4 recorder config error: clk_fall_offset_ticks/toggle period must be positive";
            return false;
        }
        const wvz4::u64 max_i64 = static_cast<wvz4::u64>(std::numeric_limits<wvz4::i64>::max());
        if (cycle > static_cast<wave::Cycle>(max_i64 / cfg_.clk_period_ticks)) {
            error = "WVZ4 recorder end_cycle failed: scaled clk cycle exceeds int64 range";
            return false;
        }
        out = static_cast<wvz4::i64>(static_cast<wvz4::u64>(cycle) * cfg_.clk_period_ticks);
        return true;
    }

    bool make_clk_fall_cycle(wvz4::i64 base_cycle, wvz4::i64& out, std::string& error) const {
        const wvz4::u64 max_i64 = static_cast<wvz4::u64>(std::numeric_limits<wvz4::i64>::max());
        const wvz4::u64 base = static_cast<wvz4::u64>(base_cycle);
        if (base > max_i64 - cfg_.clk_fall_offset_ticks) {
            error = "WVZ4 recorder end_cycle failed: clk fall cycle exceeds int64 range";
            return false;
        }
        out = static_cast<wvz4::i64>(base + cfg_.clk_fall_offset_ticks);
        return true;
    }

    static wvz4::CycleValueUpdate make_clk_update(bool high) {
        return wvz4::CycleValueUpdate::make_bool(1u, high);
    }

    static wvz4::NodeKind map_node_kind(wave::NodeKind kind) {
        switch (kind) {
        case wave::NodeKind::Leaf: return wvz4::NodeKind::SignalLeaf;
        case wave::NodeKind::FixedIndexedContainer: return wvz4::NodeKind::Container;
        case wave::NodeKind::PointerLink: return wvz4::NodeKind::Field;
        case wave::NodeKind::ValueSource: return wvz4::NodeKind::Object;
        case wave::NodeKind::Aggregate: return wvz4::NodeKind::Object;
        case wave::NodeKind::Unsupported: return wvz4::NodeKind::Object;
        default: return wvz4::NodeKind::Object;
        }
    }

    static wvz4::Radix default_radix(wave::ValueKind kind) {
        switch (kind) {
        case wave::ValueKind::Bool: return wvz4::Radix::Bin;
        case wave::ValueKind::SignedInt: return wvz4::Radix::Dec;
        case wave::ValueKind::Float64: return wvz4::Radix::Float;
        case wave::ValueKind::UnsignedInt:
        case wave::ValueKind::Enum:
        default: return wvz4::Radix::Hex;
        }
    }

    static wvz4::ValueType map_value_type(wave::ValueKind kind, wvz4::u32 bit_width) {
        if (kind == wave::ValueKind::Bool) return wvz4::ValueType::Bool;
        if (kind == wave::ValueKind::Float64) return bit_width <= 32 ? wvz4::ValueType::F32 : wvz4::ValueType::F64;
        const bool signed_type = (kind == wave::ValueKind::SignedInt);
        if (bit_width <= 8) return signed_type ? wvz4::ValueType::I8 : wvz4::ValueType::U8;
        if (bit_width <= 16) return signed_type ? wvz4::ValueType::I16 : wvz4::ValueType::U16;
        if (bit_width <= 32) return signed_type ? wvz4::ValueType::I32 : wvz4::ValueType::U32;
        return signed_type ? wvz4::ValueType::I64 : wvz4::ValueType::U64;
    }

    static wvz4::u8 byte_width_for_type(wvz4::ValueType type) {
        switch (type) {
        case wvz4::ValueType::Bool: return 1;
        case wvz4::ValueType::I8:
        case wvz4::ValueType::U8: return 1;
        case wvz4::ValueType::I16:
        case wvz4::ValueType::U16: return 2;
        case wvz4::ValueType::I32:
        case wvz4::ValueType::U32:
        case wvz4::ValueType::F32: return 4;
        case wvz4::ValueType::I64:
        case wvz4::ValueType::U64:
        case wvz4::ValueType::F64: return 8;
        default: return 8;
        }
    }

    static void write_u64_le(wvz4::ScalarValue& out, std::uint64_t v, wvz4::u8 nbytes) {
        out.byte_count = nbytes;
        out.bytes.fill(0);
        for (wvz4::u8 i = 0; i < nbytes && i < wvz4::kMaxScalarBytes; ++i) {
            out.bytes[i] = static_cast<wvz4::u8>((v >> (8u * i)) & 0xffu);
        }
    }

    bool make_update(const TrackState& st, const wave::TrackEvent& ev, wvz4::CycleValueUpdate& out) {
        out.signal_id = st.storage_signal_id != 0 ? st.storage_signal_id : st.signal_id;
        const wvz4::u8 nbytes = byte_width_for_type(st.value_type);
        switch (st.value_type) {
        case wvz4::ValueType::Bool: {
            const bool v = ev.has_bool ? ev.bool_value : (ev.has_u64 ? ev.u64_value != 0 : false);
            out.value.byte_count = 1;
            out.value.bytes.fill(0);
            out.value.bytes[0] = v ? 1u : 0u;
            return true;
        }
        case wvz4::ValueType::F32: {
            if (ev.has_u64) {
                write_u64_le(out.value, ev.u64_value, 4);
                return true;
            }
            const float f = static_cast<float>(ev.has_f64 ? ev.f64_value : 0.0);
            out = wvz4::CycleValueUpdate::make_f32(st.storage_signal_id != 0 ? st.storage_signal_id : st.signal_id, f);
            return true;
        }
        case wvz4::ValueType::F64: {
            if (ev.has_u64) {
                write_u64_le(out.value, ev.u64_value, 8);
                return true;
            }
            const double d = ev.has_f64 ? ev.f64_value : 0.0;
            out = wvz4::CycleValueUpdate::make_f64(st.storage_signal_id != 0 ? st.storage_signal_id : st.signal_id, d);
            return true;
        }
        case wvz4::ValueType::I8:
        case wvz4::ValueType::I16:
        case wvz4::ValueType::I32:
        case wvz4::ValueType::I64: {
            if (!ev.has_i64 && !ev.has_u64) { set_error("WVZ4 recorder make_update failed: missing integer payload"); return false; }
            const std::uint64_t bits = ev.has_i64 ? static_cast<std::uint64_t>(ev.i64_value) : ev.u64_value;
            write_u64_le(out.value, bits, nbytes);
            return true;
        }
        case wvz4::ValueType::U8:
        case wvz4::ValueType::U16:
        case wvz4::ValueType::U32:
        case wvz4::ValueType::U64: {
            if (!ev.has_u64 && !ev.has_i64) { set_error("WVZ4 recorder make_update failed: missing unsigned payload"); return false; }
            const std::uint64_t bits = ev.has_u64 ? ev.u64_value : static_cast<std::uint64_t>(ev.i64_value);
            write_u64_le(out.value, bits, nbytes);
            return true;
        }
        default:
            set_error("WVZ4 recorder make_update failed: unsupported value type");
            return false;
        }
    }

    bool build_layout(wvz4::Layout& layout, std::string& error) {
        error.clear();
        if (!last_error_.empty()) { error = last_error_; return false; }
        // WVZ4 v4 supports a clock-only layout when emit_default_clk is enabled.
        // Without the synthetic clock, a layout with no reflected nodes/tracks
        // is still invalid.
        if (declared_node_ids_.empty() && !cfg_.emit_default_clk) { error = "WVZ4 layout requires at least one node"; return false; }
        if (declared_track_ids_.empty() && !cfg_.emit_default_clk) { error = "WVZ4 layout requires at least one signal"; return false; }

        std::vector<wvz4::u32> node_ids = declared_node_ids_;
        std::sort(node_ids.begin(), node_ids.end());
        node_ids.erase(std::unique(node_ids.begin(), node_ids.end()), node_ids.end());
        std::vector<wvz4::u32> track_ids = declared_track_ids_;
        std::sort(track_ids.begin(), track_ids.end());
        track_ids.erase(std::unique(track_ids.begin(), track_ids.end()), track_ids.end());

        std::unordered_map<std::string, wvz4::u32> name_to_id;
        name_to_id.reserve(node_ids.size());
        std::vector<std::vector<wvz4::u32> > children(node_states_.size());
        for (std::size_t i = 0; i < node_ids.size(); ++i) {
            const wvz4::u32 nid = node_ids[i];
            if (nid >= node_states_.size() || !node_states_[nid].declared) { error = "WVZ4 layout build found missing node"; return false; }
            const NodeState& ns = node_states_[nid];
            if (ns.parent_id != 0) {
                if (ns.parent_id >= node_states_.size() || !node_states_[ns.parent_id].declared) {
                    error = "WVZ4 layout build found node with missing parent; "; error += debug_state_summary(); return false;
                }
                children[ns.parent_id].push_back(nid);
            }
            if (name_to_id.find(ns.name) == name_to_id.end()) {
                const wvz4::u32 name_id = static_cast<wvz4::u32>(name_to_id.size() + 1u);
                name_to_id[ns.name] = name_id;
                wvz4::NameRecord nr;
                nr.name_id = name_id;
                nr.name = ns.name;
                layout.names.push_back(nr);
            }
        }
        if (cfg_.emit_default_clk) {
            if (cfg_.default_clk_name.empty()) { error = "WVZ4 layout default clk name must not be empty"; return false; }
            if (name_to_id.find(cfg_.default_clk_name) == name_to_id.end()) {
                const wvz4::u32 name_id = static_cast<wvz4::u32>(name_to_id.size() + 1u);
                name_to_id[cfg_.default_clk_name] = name_id;
                wvz4::NameRecord nr;
                nr.name_id = name_id;
                nr.name = cfg_.default_clk_name;
                layout.names.push_back(nr);
            }
        }
        std::sort(layout.names.begin(), layout.names.end(), [](const wvz4::NameRecord& a, const wvz4::NameRecord& b) {
            return a.name_id < b.name_id;
        });

        wvz4::u32 default_clk_node_id = 0;
        if (cfg_.emit_default_clk) {
            if (!node_ids.empty() && node_ids.back() == std::numeric_limits<wvz4::u32>::max()) {
                error = "WVZ4 layout cannot allocate default clk node_id: node_id range exhausted";
                return false;
            }
            default_clk_node_id = node_ids.empty() ? 1u : (node_ids.back() + 1u);
        }

        layout.nodes.reserve(node_ids.size() + (cfg_.emit_default_clk ? 1u : 0u));
        if (cfg_.emit_default_clk) {
            wvz4::NodeRecord clk_node;
            clk_node.node_id = default_clk_node_id;
            clk_node.parent_id = 0;
            clk_node.name_id = name_to_id[cfg_.default_clk_name];
            clk_node.kind = wvz4::NodeKind::SignalLeaf;
            clk_node.first_child = 0;
            clk_node.next_sibling = 0;
            layout.nodes.push_back(clk_node);
        }
        for (std::size_t i = 0; i < node_ids.size(); ++i) {
            const wvz4::u32 nid = node_ids[i];
            const NodeState& ns = node_states_[nid];
            wvz4::NodeRecord rec;
            rec.node_id = ns.node_id;
            rec.parent_id = ns.parent_id;
            rec.name_id = name_to_id[ns.name];
            rec.kind = ns.kind;
            if (!children[nid].empty()) rec.first_child = children[nid][0];
            rec.next_sibling = 0;
            if (ns.parent_id != 0) {
                const std::vector<wvz4::u32>& siblings = children[ns.parent_id];
                for (std::size_t k = 0; k + 1 < siblings.size(); ++k) {
                    if (siblings[k] == nid) { rec.next_sibling = siblings[k + 1]; break; }
                }
            }
            layout.nodes.push_back(rec);
        }

        layout.signals.reserve(track_ids.size() + (cfg_.emit_default_clk ? 1u : 0u));
        if (cfg_.emit_default_clk) {
            wvz4::SignalDefinition clk_sig;
            clk_sig.signal_id = 1u;
            clk_sig.node_id = default_clk_node_id;
            clk_sig.type = wvz4::ValueType::Bool;
            clk_sig.bit_width = 1u;
            clk_sig.radix = wvz4::Radix::Bin;
            clk_sig.storage_id = clk_sig.signal_id;
            layout.signals.push_back(clk_sig);
            wvz4::ClockDefinition clk_desc;
            clk_desc.signal_id = clk_sig.signal_id;
            clk_desc.initial_value = cfg_.clk_initial_value;
            clk_desc.period_ticks = cfg_.clk_fall_offset_ticks == 0 ? 1u : cfg_.clk_fall_offset_ticks;
            layout.clocks.push_back(clk_desc);
        }
        for (std::size_t i = 0; i < track_ids.size(); ++i) {
            const wvz4::u32 tid = track_ids[i];
            if (tid >= track_states_.size() || !track_states_[tid].declared) { error = "WVZ4 layout build found missing track"; return false; }
            const TrackState& ts = track_states_[tid];
            if (ts.storage_only) {
                if (ts.node_id != 0) {
                    error = "WVZ4 layout build found storage-only signal with node"; return false;
                }
            } else {
                if (ts.node_id >= node_states_.size() || !node_states_[ts.node_id].declared) {
                    error = "WVZ4 layout build found signal with missing node; "; error += debug_state_summary(); return false;
                }
                if (node_states_[ts.node_id].kind != wvz4::NodeKind::SignalLeaf) {
                    error = "WVZ4 layout build found signal whose node is not SignalLeaf"; return false;
                }
            }
            wvz4::SignalDefinition sig;
            sig.signal_id = ts.signal_id;
            sig.storage_id = ts.storage_signal_id != 0 ? ts.storage_signal_id : ts.signal_id;
            sig.node_id = ts.node_id;
            sig.type = ts.value_type;
            sig.bit_width = ts.bit_width;
            sig.bit_offset = ts.bit_offset;
            sig.radix = ts.radix;
            sig.storage_only = ts.storage_only;
            layout.signals.push_back(sig);
        }
        return true;
    }
};
#if defined(WAVE_PATH_WVZ4_RECORDER_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef WAVE_PATH_WVZ4_RECORDER_RESTORE_MAX_MACRO_
#endif
#if defined(WAVE_PATH_WVZ4_RECORDER_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef WAVE_PATH_WVZ4_RECORDER_RESTORE_MIN_MACRO_
#endif
