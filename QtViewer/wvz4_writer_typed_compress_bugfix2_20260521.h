#ifndef WVZ4_WRITER_TYPED_H_
#define WVZ4_WRITER_TYPED_H_

#include <algorithm>
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef WVZ4_NO_ZSTD
#include <zstd.h>
#endif

namespace wvz4 {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8  = std::uint8_t;

static const u32 kFormatVersion = 2;
static const u32 kMaxScalarBytes = 8;

// Raw WDAT payload encoding flags. The outer WDAT section and block index remain
// unchanged; these flags describe the uncompressed raw_payload inside each block.
static const u64 kWdatDeltaTimes        = 1ull << 0; // per-signal delta time stream
static const u64 kWdatFixedValueWidth   = 1ull << 1; // no per-transition byte_count
static const u64 kWdatSharedTimeTable   = 1ull << 2; // block-level shared time table

// WVZ4 intentionally targets stable-topology, two-state, scalar waveform data.
// No dynamic signal add/remove, no Z/high-impedance state, no name->signal lookup.
enum class NodeKind : u8 {
    Root        = 1,
    Object      = 2,
    Field       = 3,
    ArrayElem   = 4,  // e.g. sm[0], warp[3] is stored directly as node name.
    Container   = 5,
    SignalLeaf  = 6
};

enum class ValueType : u8 {
    Bool = 1,
    I8   = 2,
    U8   = 3,
    I16  = 4,
    U16  = 5,
    I32  = 6,
    U32  = 7,
    I64  = 8,
    U64  = 9,
    F32  = 10,
    F64  = 11
};

enum class Radix : u8 {
    Bin    = 2,
    Dec    = 10,
    Hex    = 16,
    Float  = 32,
    Auto   = 255
};

enum class Compression : u8 {
    None = 0,
    Zstd = 1
};

struct NameRecord {
    u32 name_id = 0;       // positive, unique
    std::string name;      // segment name, e.g. "top", "gpu", "warp[3]"
};

struct NodeRecord {
    u32 node_id = 0;       // positive, unique
    u32 parent_id = 0;     // 0 only for root-level nodes
    u32 name_id = 0;
    NodeKind kind = NodeKind::Object;

    // Stored child table. first_child points to first child node; siblings form a chain.
    u32 first_child = 0;
    u32 next_sibling = 0;
};

struct SignalDefinition {
    u32 signal_id = 0;     // positive, fixed at registration/open time
    u32 node_id = 0;       // leaf node id in NodeTable
    ValueType type = ValueType::U64;
    u32 bit_width = 64;    // <= 64 in WVZ4 typed writer
    Radix radix = Radix::Auto;
};

struct Layout {
    std::vector<NameRecord> names;
    std::vector<NodeRecord> nodes;
    std::vector<SignalDefinition> signals;
};

struct WriterOptions {
    u64 target_block_span = 100000; // writer-cycle span; must be > 0
    Compression compression = Compression::Zstd;
    int zstd_level = 3;

    // Block pipeline: commit_block only builds raw block payload and queues it.
    // Compression workers encode blocks; file writer thread appends by block_id order.
    bool enable_block_pipeline = true;
    u32 block_pipeline_threads = 0;        // 0 = auto, roughly hardware_concurrency / 2
    std::size_t block_pipeline_queue_limit = 0; // 0 = unlimited; set to bound memory

    // WDAT encoding. Keep both enabled by default: the writer builds candidate
    // encodings per block and stores the smaller one. Both encodings omit
    // per-transition byte_count because fixed value width is defined by SignalTable.
    bool enable_delta_time_encoding = true;
    bool enable_shared_time_table = true;

    // WVZ4 v2 has an implicit all-zero value for every signal at cycle 0.
    // This avoids writing a huge first block when most signals initialize to 0.
    // Non-zero first values are still written explicitly.
    bool implicit_zero_initial_values = true;

    // Optional initial capacity hints. They only reduce allocation; semantics unchanged.
    std::size_t transition_reserve_per_signal = 0;
};

struct ScalarValue {
    std::array<u8, kMaxScalarBytes> bytes;
    u8 byte_count = 0;

    ScalarValue() { bytes.fill(0); }
};

struct CycleValueUpdate {
    u32 signal_id = 0;
    ScalarValue value;

    template <typename T>
    static CycleValueUpdate make(u32 signal_id, T value) {
        CycleValueUpdate u;
        u.signal_id = signal_id;
        set_value(u.value, value);
        return u;
    }

    static CycleValueUpdate make_bool(u32 signal_id, bool value) {
        return make<bool>(signal_id, value);
    }

    static CycleValueUpdate make_f32(u32 signal_id, float value) {
        return make<float>(signal_id, value);
    }

    static CycleValueUpdate make_f64(u32 signal_id, double value) {
        return make<double>(signal_id, value);
    }

    static CycleValueUpdate make_raw(u32 signal_id, const void* data, u8 byte_count) {
        CycleValueUpdate u;
        u.signal_id = signal_id;
        u.value.byte_count = byte_count;
        if (byte_count > 0 && data) {
            std::memcpy(u.value.bytes.data(), data, byte_count > kMaxScalarBytes ? kMaxScalarBytes : byte_count);
        }
        return u;
    }

private:
    template <typename T>
    static typename std::enable_if<std::is_enum<T>::value, void>::type
    set_value(ScalarValue& out, T value) {
        typedef typename std::underlying_type<T>::type U;
        set_value(out, static_cast<U>(value));
    }

    template <typename T>
    static typename std::enable_if<std::is_same<T, bool>::value, void>::type
    set_value(ScalarValue& out, T value) {
        out.byte_count = 1;
        out.bytes[0] = value ? 1u : 0u;
    }

    template <typename T>
    static typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, void>::type
    set_value(ScalarValue& out, T value) {
        typedef typename std::make_unsigned<T>::type U;
        U v = static_cast<U>(value);
        out.byte_count = static_cast<u8>(sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            out.bytes[i] = static_cast<u8>((static_cast<u64>(v) >> (8 * i)) & 0xffu);
        }
    }

    static void set_value(ScalarValue& out, float value) {
        static_assert(sizeof(float) == 4, "WVZ4 assumes 32-bit float");
        out.byte_count = 4;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        for (std::size_t i = 0; i < 4; ++i) out.bytes[i] = static_cast<u8>((bits >> (8 * i)) & 0xffu);
    }

    static void set_value(ScalarValue& out, double value) {
        static_assert(sizeof(double) == 8, "WVZ4 assumes 64-bit double");
        out.byte_count = 8;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        for (std::size_t i = 0; i < 8; ++i) out.bytes[i] = static_cast<u8>((bits >> (8 * i)) & 0xffu);
    }
};

struct CycleSubmission {
    i64 cycle = 0;
    std::vector<CycleValueUpdate> updates;
};

namespace detail {

inline void append_u8(std::vector<u8>& out, u8 v) { out.push_back(v); }

inline void append_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xffu));
    out.push_back(static_cast<u8>((v >> 8) & 0xffu));
    out.push_back(static_cast<u8>((v >> 16) & 0xffu));
    out.push_back(static_cast<u8>((v >> 24) & 0xffu));
}

inline void append_u64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

inline void append_i64(std::vector<u8>& out, i64 v) {
    append_u64(out, static_cast<u64>(v));
}

inline void append_varuint(std::vector<u8>& out, u64 v) {
    while (v >= 0x80u) {
        out.push_back(static_cast<u8>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<u8>(v));
}

inline void append_string(std::vector<u8>& out, const std::string& s) {
    append_varuint(out, static_cast<u64>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

inline bool write_all(std::ofstream& out, const void* data, std::size_t size) {
    if (size == 0) return true;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

inline bool write_u32(std::ofstream& out, u32 v) {
    u8 b[4] = {
        static_cast<u8>(v & 0xffu),
        static_cast<u8>((v >> 8) & 0xffu),
        static_cast<u8>((v >> 16) & 0xffu),
        static_cast<u8>((v >> 24) & 0xffu)
    };
    return write_all(out, b, 4);
}

inline bool write_u64(std::ofstream& out, u64 v) {
    u8 b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<u8>((v >> (8 * i)) & 0xffu);
    return write_all(out, b, 8);
}

inline bool value_type_byte_width(ValueType t, u8& bytes) {
    switch (t) {
    case ValueType::Bool: bytes = 1; return true;
    case ValueType::I8:   bytes = 1; return true;
    case ValueType::U8:   bytes = 1; return true;
    case ValueType::I16:  bytes = 2; return true;
    case ValueType::U16:  bytes = 2; return true;
    case ValueType::I32:  bytes = 4; return true;
    case ValueType::U32:  bytes = 4; return true;
    case ValueType::I64:  bytes = 8; return true;
    case ValueType::U64:  bytes = 8; return true;
    case ValueType::F32:  bytes = 4; return true;
    case ValueType::F64:  bytes = 8; return true;
    default: return false;
    }
}

inline bool is_valid_node_kind(NodeKind k) {
    switch (k) {
    case NodeKind::Root:
    case NodeKind::Object:
    case NodeKind::Field:
    case NodeKind::ArrayElem:
    case NodeKind::Container:
    case NodeKind::SignalLeaf:
        return true;
    default:
        return false;
    }
}

inline bool is_valid_radix(Radix r) {
    switch (r) {
    case Radix::Bin:
    case Radix::Dec:
    case Radix::Hex:
    case Radix::Float:
    case Radix::Auto:
        return true;
    default:
        return false;
    }
}

inline bool is_valid_compression(Compression c) {
    return c == Compression::None || c == Compression::Zstd;
}

inline bool scalar_equal(const ScalarValue& a, const ScalarValue& b) {
    if (a.byte_count != b.byte_count) return false;
    return std::memcmp(a.bytes.data(), b.bytes.data(), a.byte_count) == 0;
}

inline bool scalar_is_zero(const ScalarValue& v, u8 fixed_width) {
    if (v.byte_count != fixed_width) return false;
    for (u8 i = 0; i < fixed_width; ++i) {
        if (v.bytes[i] != 0) return false;
    }
    return true;
}

inline std::string make_error(const char* prefix, u64 id) {
    return std::string(prefix) + std::to_string(id);
}

} // namespace detail

class Writer {
public:
    Writer() {}
    ~Writer() { std::string ignored; close(ignored); }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool open(const std::string& path, const Layout& layout, const WriterOptions& options, std::string& error) {
        std::string close_error;
        if (!close(close_error)) {
            error = close_error.empty() ? "failed to close previous WVZ4 writer before reopen" : close_error;
            return false;
        }
        error.clear();
        options_ = options;
        if (options_.target_block_span == 0) {
            error = "WVZ4 WriterOptions.target_block_span must be > 0";
            return false;
        }
        if (options_.target_block_span > static_cast<u64>(std::numeric_limits<i64>::max())) {
            error = "WVZ4 WriterOptions.target_block_span exceeds int64 cycle range";
            return false;
        }
        if (!detail::is_valid_compression(options_.compression)) {
            error = "invalid WVZ4 compression option";
            return false;
        }
        if (!validate_and_prepare_layout(layout, error)) return false;

        out_.open(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!out_) {
            error = "failed to open output file: " + path;
            reset_all();
            return false;
        }
        opened_ = true;
        current_block_start_ = 0;
        current_cycle_ = 0;
        next_block_id_ = 0;
        footer_offset_ = 0;

        if (!write_file_header(error)) { if (out_.is_open()) out_.close(); reset_all(); return false; }
        if (!write_layout_sections(layout_, error)) { if (out_.is_open()) out_.close(); reset_all(); return false; }

        if (options_.enable_block_pipeline) {
            start_block_pipeline();
        }
        return true;
    }

    bool submit_cycle(const CycleSubmission& submission, std::string& error) {
        error.clear();
        if (!opened_) { error = "WVZ4 writer is not open"; return false; }
        if (submission.cycle < 0) { error = "cycle must be non-negative"; return false; }
        if (submission.cycle == std::numeric_limits<i64>::max()) { error = "cycle is too large to close safely"; return false; }
        if (have_submitted_cycle_ && submission.cycle <= current_cycle_) {
            error = "cycle must be strictly greater than previous submitted cycle";
            return false;
        }

        // Validate the whole submission before mutating writer state. This avoids
        // partially applying early updates if a later update is malformed.
        update_state_scratch_.clear();
        if (update_state_scratch_.capacity() < submission.updates.size()) {
            update_state_scratch_.reserve(submission.updates.size());
        }
        ensure_update_seen_capacity();
        begin_update_seen_epoch();
        for (std::size_t i = 0; i < submission.updates.size(); ++i) {
            const CycleValueUpdate& upd = submission.updates[i];
            if (upd.signal_id == 0 || upd.signal_id >= signal_states_.size()) {
                error = detail::make_error("invalid signal_id in update: ", upd.signal_id);
                return false;
            }
            if (mark_update_seen(upd.signal_id)) {
                error = detail::make_error("duplicate signal_id in one CycleSubmission: ", upd.signal_id);
                return false;
            }
            SignalState& st = signal_states_[upd.signal_id];
            if (!st.valid) {
                error = detail::make_error("update targets undefined signal_id: ", upd.signal_id);
                return false;
            }
            if (upd.value.byte_count != st.byte_width_bytes) {
                error = detail::make_error("update byte width mismatch for signal_id: ", upd.signal_id);
                return false;
            }
            update_state_scratch_.push_back(&st);
        }

        if (!flush_until(submission.cycle, error)) return false;

        for (std::size_t i = 0; i < submission.updates.size(); ++i) {
            const CycleValueUpdate& upd = submission.updates[i];
            SignalState& st = *update_state_scratch_[i];
            if (!st.has_current && options_.implicit_zero_initial_values &&
                detail::scalar_is_zero(upd.value, st.byte_width_bytes)) {
                st.current = upd.value;
                st.has_current = true;
                continue;
            }
            if (st.has_current && detail::scalar_equal(upd.value, st.current)) {
                continue;
            }
            Transition tr;
            tr.cycle = submission.cycle;
            tr.value = upd.value;
            st.transitions.push_back(tr);
            st.current = upd.value;
            st.has_current = true;
            have_pending_content_ = true;
        }
        current_cycle_ = submission.cycle;
        have_submitted_cycle_ = true;
        return true;
    }

    bool close(std::string& error) {
        error.clear();
        if (!opened_) return true;

        bool ok = true;
        std::string local_error;
        if (have_pending_content_) {
            if (!commit_block(current_cycle_ + 1, local_error)) ok = false;
        }
        if (!stop_block_pipeline(local_error)) ok = false;
        if (ok && !write_footer_and_patch_header(local_error)) ok = false;

        if (out_.is_open()) out_.close();
        reset_all();
        if (!ok) error = local_error.empty() ? "WVZ4 writer close failed" : local_error;
        return ok;
    }

private:
    struct Transition {
        i64 cycle = 0;
        ScalarValue value;
    };

    struct SignalState {
        bool valid = false;
        SignalDefinition def;
        u8 byte_width_bytes = 0;

        // current is only meaningful after the first submitted value for this
        // signal.  Do not treat the implicit zero-initialized storage value as a
        // real waveform value; otherwise an initial zero sample would be dropped
        // and the viewer would have no explicit value at the first sampled cycle.
        bool has_current = false;
        ScalarValue current;
        std::vector<Transition> transitions;
    };

    struct BlockIndexRecord {
        u64 block_id = 0;
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        u64 file_offset = 0;
        u64 file_size = 0;
        u64 raw_size = 0;
        Compression compression = Compression::None;
    };

    struct BlockJob {
        u64 block_id = 0;
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        std::vector<u8> raw_payload;
    };

    struct EncodedBlock {
        u64 block_id = 0;
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        std::vector<u8> payload;
        u64 raw_size = 0;
        Compression compression = Compression::None;
        std::string error;
    };

    struct PipelineState {
        bool running = false;
        bool stop_requested = false;
        bool compression_done = false;
        std::mutex mutex;
        std::condition_variable cv_jobs;
        std::condition_variable cv_results;
        std::condition_variable cv_space;
        std::deque<BlockJob> jobs;
        std::deque<EncodedBlock> results;
        std::vector<std::thread> compression_threads;
        std::thread file_thread;
        std::string error;
        u64 next_file_block_id = 0;
    };

private:
    static u32 choose_pipeline_threads() {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) return 1;
        unsigned n = hw / 2;
        if (n == 0) n = 1;
        return static_cast<u32>(n);
    }

    bool validate_and_prepare_layout(const Layout& layout, std::string& error) {
        layout_ = layout;
        if (layout_.names.empty()) { error = "WVZ4 layout requires a non-empty NameTable"; return false; }
        if (layout_.nodes.empty()) { error = "WVZ4 layout requires a non-empty NodeTable"; return false; }
        if (layout_.signals.empty()) { error = "WVZ4 layout requires a non-empty SignalTable"; return false; }

        u32 max_name_id = 0, max_node_id = 0, max_signal_id = 0;
        for (std::size_t i = 0; i < layout_.names.size(); ++i) max_name_id = std::max(max_name_id, layout_.names[i].name_id);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) max_node_id = std::max(max_node_id, layout_.nodes[i].node_id);
        for (std::size_t i = 0; i < layout_.signals.size(); ++i) max_signal_id = std::max(max_signal_id, layout_.signals[i].signal_id);
        if (max_name_id == 0 || max_node_id == 0 || max_signal_id == 0) {
            error = "WVZ4 ids must be positive";
            return false;
        }

        std::vector<u8> seen_names(max_name_id + 1, 0);
        for (std::size_t i = 0; i < layout_.names.size(); ++i) {
            const NameRecord& r = layout_.names[i];
            if (r.name_id == 0) { error = "NameRecord.name_id must be positive"; return false; }
            if (r.name.empty()) { error = detail::make_error("NameRecord.name must not be empty for name_id: ", r.name_id); return false; }
            if (seen_names[r.name_id]) { error = detail::make_error("duplicate name_id: ", r.name_id); return false; }
            seen_names[r.name_id] = 1;
        }

        std::vector<u8> seen_nodes(max_node_id + 1, 0);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const NodeRecord& r = layout_.nodes[i];
            if (r.node_id == 0) { error = "NodeRecord.node_id must be positive"; return false; }
            if (r.name_id == 0 || r.name_id >= seen_names.size() || !seen_names[r.name_id]) {
                error = detail::make_error("NodeRecord references missing name_id: ", r.name_id); return false;
            }
            if (!detail::is_valid_node_kind(r.kind)) { error = detail::make_error("invalid NodeKind for node_id: ", r.node_id); return false; }
            if (seen_nodes[r.node_id]) { error = detail::make_error("duplicate node_id: ", r.node_id); return false; }
            seen_nodes[r.node_id] = 1;
        }
        std::vector<u32> node_parent(max_node_id + 1, 0);
        std::vector<u32> node_first_child(max_node_id + 1, 0);
        std::vector<u32> node_next_sibling(max_node_id + 1, 0);
        std::vector<NodeKind> node_kind(max_node_id + 1, NodeKind::Object);
        std::vector<u32> child_count(max_node_id + 1, 0);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const NodeRecord& r = layout_.nodes[i];
            node_parent[r.node_id] = r.parent_id;
            node_first_child[r.node_id] = r.first_child;
            node_next_sibling[r.node_id] = r.next_sibling;
            node_kind[r.node_id] = r.kind;
            if (r.parent_id != 0 && r.parent_id < child_count.size()) ++child_count[r.parent_id];
            if (r.parent_id != 0 && (r.parent_id >= seen_nodes.size() || !seen_nodes[r.parent_id])) {
                error = detail::make_error("NodeRecord references missing parent_id: ", r.parent_id); return false;
            }
            if (r.first_child != 0 && (r.first_child >= seen_nodes.size() || !seen_nodes[r.first_child])) {
                error = detail::make_error("NodeRecord references missing first_child: ", r.first_child); return false;
            }
            if (r.next_sibling != 0 && (r.next_sibling >= seen_nodes.size() || !seen_nodes[r.next_sibling])) {
                error = detail::make_error("NodeRecord references missing next_sibling: ", r.next_sibling); return false;
            }
        }
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const NodeRecord& r = layout_.nodes[i];
            if (r.next_sibling != 0 && node_parent[r.next_sibling] != r.parent_id) {
                error = detail::make_error("next_sibling has different parent_id for node_id: ", r.node_id); return false;
            }
        }
        // Validate the stored child table: parent.first_child and child.next_sibling must
        // enumerate exactly the children whose parent_id equals that parent, without cycles.
        std::vector<u32> visit_stamp(max_node_id + 1, 0);
        u32 stamp = 1;
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const NodeRecord& parent = layout_.nodes[i];
            u32 count = 0;
            for (u32 child = parent.first_child; child != 0; child = node_next_sibling[child]) {
                if (child >= seen_nodes.size() || !seen_nodes[child]) {
                    error = detail::make_error("child chain references missing node_id: ", child); return false;
                }
                if (node_parent[child] != parent.node_id) {
                    error = detail::make_error("child chain node has wrong parent_id: ", child); return false;
                }
                if (visit_stamp[child] == stamp) {
                    error = detail::make_error("cycle in child sibling chain under node_id: ", parent.node_id); return false;
                }
                visit_stamp[child] = stamp;
                ++count;
            }
            if (count != child_count[parent.node_id]) {
                error = detail::make_error("child chain does not enumerate all children for node_id: ", parent.node_id); return false;
            }
            ++stamp;
            if (stamp == 0) { std::fill(visit_stamp.begin(), visit_stamp.end(), 0); stamp = 1; }
        }

        signal_states_.clear();
        signal_states_.resize(static_cast<std::size_t>(max_signal_id) + 1);
        signal_order_.clear();
        signal_order_.reserve(layout_.signals.size());
        std::vector<u8> seen_signals(max_signal_id + 1, 0);
        for (std::size_t i = 0; i < layout_.signals.size(); ++i) {
            const SignalDefinition& s = layout_.signals[i];
            if (s.signal_id == 0) { error = "SignalDefinition.signal_id must be positive"; return false; }
            if (seen_signals[s.signal_id]) { error = detail::make_error("duplicate signal_id: ", s.signal_id); return false; }
            if (s.node_id == 0 || s.node_id >= seen_nodes.size() || !seen_nodes[s.node_id]) {
                error = detail::make_error("SignalDefinition references missing node_id: ", s.node_id); return false;
            }
            if (node_first_child[s.node_id] != 0) {
                error = detail::make_error("SignalDefinition node_id must be a leaf node: ", s.node_id); return false;
            }
            if (node_kind[s.node_id] != NodeKind::SignalLeaf) {
                error = detail::make_error("SignalDefinition node_id must reference a SignalLeaf node: ", s.node_id); return false;
            }
            if (!detail::is_valid_radix(s.radix)) {
                error = detail::make_error("invalid Radix for signal_id: ", s.signal_id); return false;
            }
            if (s.bit_width == 0 || s.bit_width > 64) {
                error = detail::make_error("WVZ4 signal bit_width must be 1..64 for signal_id: ", s.signal_id); return false;
            }
            u8 bytes = 0;
            if (!detail::value_type_byte_width(s.type, bytes)) {
                error = detail::make_error("invalid ValueType for signal_id: ", s.signal_id); return false;
            }
            if (s.bit_width > static_cast<u32>(bytes) * 8u) {
                error = detail::make_error("signal bit_width exceeds ValueType capacity for signal_id: ", s.signal_id); return false;
            }
            SignalState st;
            st.valid = true;
            st.def = s;
            st.byte_width_bytes = bytes;
            st.has_current = false;
            st.current.byte_count = bytes;
            st.current.bytes.fill(0);
            if (options_.transition_reserve_per_signal > 0) st.transitions.reserve(options_.transition_reserve_per_signal);
            signal_states_[s.signal_id] = st;
            signal_order_.push_back(s.signal_id);
            seen_signals[s.signal_id] = 1;
        }
        std::sort(signal_order_.begin(), signal_order_.end());
        return true;
    }

    bool write_file_header(std::string& error) {
        // Fixed 64-byte header. footer_offset is patched at close.
        const char magic[8] = {'W','V','Z','4','\r','\n',0,0};
        if (!detail::write_all(out_, magic, 8)) { error = "failed to write WVZ4 magic"; return false; }
        if (!detail::write_u32(out_, kFormatVersion)) { error = "failed to write WVZ4 version"; return false; }
        if (!detail::write_u32(out_, 64)) { error = "failed to write WVZ4 header size"; return false; }
        if (!detail::write_u64(out_, options_.target_block_span)) { error = "failed to write WVZ4 block span"; return false; }
        if (!detail::write_u64(out_, 0)) { error = "failed to write WVZ4 footer placeholder"; return false; }
        if (!detail::write_u64(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        if (!detail::write_u64(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        if (!detail::write_u64(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        if (!detail::write_u32(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        if (!detail::write_u32(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        return true;
    }

    bool write_section(const char tag[4], const std::vector<u8>& payload, std::string& error) {
        if (!detail::write_all(out_, tag, 4)) { error = "failed to write section tag"; return false; }
        if (!detail::write_u64(out_, static_cast<u64>(payload.size()))) { error = "failed to write section length"; return false; }
        if (!detail::write_all(out_, payload.data(), payload.size())) { error = "failed to write section payload"; return false; }
        return true;
    }

    bool compress_payload_zstd(const std::vector<u8>& raw, std::vector<u8>& compressed, std::string& error) const {
        compressed.clear();
        if (raw.empty()) return true;
#ifndef WVZ4_NO_ZSTD
        const std::size_t bound = ZSTD_compressBound(raw.size());
        compressed.resize(bound);
        const std::size_t written = ZSTD_compress(compressed.data(), bound,
                                                  raw.data(), raw.size(),
                                                  options_.zstd_level);
        if (ZSTD_isError(written)) {
            error = std::string("ZSTD_compress layout failed: ") + ZSTD_getErrorName(written);
            compressed.clear();
            return false;
        }
        compressed.resize(written);
        return true;
#else
        (void)raw;
        error = "WVZ4 built with WVZ4_NO_ZSTD but layout zstd compression was requested";
        return false;
#endif
    }

    bool write_layout_section(const char raw_tag[4], const char zstd_tag[4],
                              const std::vector<u8>& raw_payload, std::string& error) {
        if (options_.compression != Compression::Zstd || raw_payload.empty()) {
            return write_section(raw_tag, raw_payload, error);
        }
        std::vector<u8> compressed;
        if (!compress_payload_zstd(raw_payload, compressed, error)) return false;
        std::vector<u8> zpayload;
        zpayload.reserve(1 + 10 + 10 + compressed.size());
        detail::append_u8(zpayload, static_cast<u8>(Compression::Zstd));
        detail::append_varuint(zpayload, static_cast<u64>(raw_payload.size()));
        detail::append_varuint(zpayload, static_cast<u64>(compressed.size()));
        zpayload.insert(zpayload.end(), compressed.begin(), compressed.end());
        if (zpayload.size() < raw_payload.size()) {
            return write_section(zstd_tag, zpayload, error);
        }
        return write_section(raw_tag, raw_payload, error);
    }

    bool write_layout_sections(const Layout& layout, std::string& error) {
        std::vector<u8> payload;
        payload.reserve(layout.names.size() * 16);
        detail::append_varuint(payload, static_cast<u64>(layout.names.size()));
        std::vector<NameRecord> names = layout.names;
        std::sort(names.begin(), names.end(), [](const NameRecord& a, const NameRecord& b) { return a.name_id < b.name_id; });
        for (std::size_t i = 0; i < names.size(); ++i) {
            detail::append_varuint(payload, names[i].name_id);
            detail::append_string(payload, names[i].name);
        }
        if (!write_layout_section("NAME", "NAMZ", payload, error)) return false;

        payload.clear();
        detail::append_varuint(payload, static_cast<u64>(layout.nodes.size()));
        std::vector<NodeRecord> nodes = layout.nodes;
        std::sort(nodes.begin(), nodes.end(), [](const NodeRecord& a, const NodeRecord& b) { return a.node_id < b.node_id; });
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const NodeRecord& n = nodes[i];
            detail::append_varuint(payload, n.node_id);
            detail::append_varuint(payload, n.parent_id);
            detail::append_varuint(payload, n.name_id);
            detail::append_u8(payload, static_cast<u8>(n.kind));
            detail::append_varuint(payload, n.first_child);
            detail::append_varuint(payload, n.next_sibling);
        }
        if (!write_layout_section("NODE", "NODZ", payload, error)) return false;

        payload.clear();
        detail::append_varuint(payload, static_cast<u64>(layout.signals.size()));
        std::vector<SignalDefinition> sigs = layout.signals;
        std::sort(sigs.begin(), sigs.end(), [](const SignalDefinition& a, const SignalDefinition& b) { return a.signal_id < b.signal_id; });
        for (std::size_t i = 0; i < sigs.size(); ++i) {
            const SignalDefinition& s = sigs[i];
            detail::append_varuint(payload, s.signal_id);
            detail::append_varuint(payload, s.node_id);
            detail::append_u8(payload, static_cast<u8>(s.type));
            detail::append_varuint(payload, s.bit_width);
            detail::append_u8(payload, static_cast<u8>(s.radix));
        }
        if (!write_layout_section("SIGT", "SIGZ", payload, error)) return false;
        return true;
    }

    bool flush_until(i64 cycle, std::string& error) {
        const i64 span = static_cast<i64>(options_.target_block_span);
        while (true) {
            if (current_block_start_ > static_cast<u64>(std::numeric_limits<i64>::max())) {
                error = "current block start exceeds int64 cycle range";
                return false;
            }
            const i64 start = static_cast<i64>(current_block_start_);
            if (start > std::numeric_limits<i64>::max() - span) {
                // The next block boundary is beyond representable cycle values.
                // Since submit_cycle rejects INT64_MAX, no submitted cycle can cross it.
                return true;
            }
            const i64 end_cycle = start + span;
            if (cycle < end_cycle) return true;

            if (have_pending_content_) {
                // Commit exactly the block that currently owns the pending transitions.
                // submit_cycle() calls flush_until() before applying the current cycle,
                // so pending transitions are strictly before end_cycle here.
                if (!commit_block(end_cycle, error)) return false;
                current_block_start_ = static_cast<u64>(end_cycle);
                continue;
            }

            // No pending transitions: skip all empty blocks in O(1). The old loop
            // advanced one target_block_span at a time, which could hang on a large
            // cycle jump with no waveform changes.
            const u64 span_u = options_.target_block_span;
            const u64 target = (static_cast<u64>(cycle) / span_u) * span_u;
            if (target < current_block_start_) return true; // defensive; cycle is monotonic.
            current_block_start_ = target;
            return true;
        }
    }

    bool commit_block(i64 end_cycle, std::string& error) {
        BlockJob job;
        job.block_id = next_block_id_++;
        job.start_cycle = static_cast<i64>(current_block_start_);
        job.end_cycle = end_cycle;
        build_block_payload(job, job.raw_payload);

        bool committed = false;
        if (options_.enable_block_pipeline && pipeline_.running) {
            committed = enqueue_block_job(std::move(job), error);
        } else {
            EncodedBlock encoded = encode_block_job(job);
            if (!encoded.error.empty()) { error = encoded.error; return false; }
            committed = write_encoded_block(encoded, error);
        }
        if (!committed) return false;

        clear_committed_transitions();
        return true;
    }

    void clear_committed_transitions() {
        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            SignalState& st = signal_states_[signal_order_[i]];
            st.transitions.clear();
        }
        have_pending_content_ = false;
    }

    void ensure_update_seen_capacity() {
        if (update_seen_stamp_.size() < signal_states_.size()) {
            update_seen_stamp_.resize(signal_states_.size(), 0);
        }
    }

    void begin_update_seen_epoch() {
        ++update_seen_epoch_;
        if (update_seen_epoch_ == 0) {
            std::fill(update_seen_stamp_.begin(), update_seen_stamp_.end(), 0);
            update_seen_epoch_ = 1;
        }
    }

    bool mark_update_seen(u32 signal_id) {
        if (update_seen_stamp_[signal_id] == update_seen_epoch_) return true;
        update_seen_stamp_[signal_id] = update_seen_epoch_;
        return false;
    }

    static void append_fixed_value_bytes(std::vector<u8>& out, const ScalarValue& value, u8 fixed_width) {
        out.insert(out.end(), value.bytes.begin(), value.bytes.begin() + fixed_width);
    }

    static void append_wdat_raw_header(std::vector<u8>& out, const BlockJob& job, u64 flags) {
        detail::append_varuint(out, job.block_id);
        detail::append_i64(out, job.start_cycle);
        detail::append_i64(out, job.end_cycle);
        detail::append_varuint(out, flags);
    }

    u64 count_nonempty_signal_records() const {
        u64 record_count = 0;
        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            const SignalState& st = signal_states_[signal_order_[i]];
            if (!st.transitions.empty()) ++record_count;
        }
        return record_count;
    }

    void build_block_payload_delta(const BlockJob& job, std::vector<u8>& out) const {
        out.clear();
        append_wdat_raw_header(out, job, kWdatFixedValueWidth | kWdatDeltaTimes);
        detail::append_varuint(out, count_nonempty_signal_records());

        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            const u32 signal_id = signal_order_[i];
            const SignalState& st = signal_states_[signal_id];
            if (st.transitions.empty()) continue;

            detail::append_varuint(out, signal_id);
            detail::append_varuint(out, static_cast<u64>(st.transitions.size()));

            u64 prev_rel = 0;
            for (std::size_t t = 0; t < st.transitions.size(); ++t) {
                const Transition& tr = st.transitions[t];
                const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
                detail::append_varuint(out, rel - prev_rel);
                prev_rel = rel;
                append_fixed_value_bytes(out, tr.value, st.byte_width_bytes);
            }
        }
    }

    void collect_shared_times(const BlockJob& job, std::vector<u64>& times) const {
        times.clear();
        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            const SignalState& st = signal_states_[signal_order_[i]];
            for (std::size_t t = 0; t < st.transitions.size(); ++t) {
                const Transition& tr = st.transitions[t];
                times.push_back(static_cast<u64>(tr.cycle - job.start_cycle));
            }
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
    }

    void build_block_payload_shared_time(const BlockJob& job, std::vector<u8>& out) const {
        std::vector<u64> shared_times;
        collect_shared_times(job, shared_times);

        out.clear();
        append_wdat_raw_header(out, job, kWdatFixedValueWidth | kWdatSharedTimeTable);

        // Shared table is delta-coded. Each signal transition then references a
        // table index instead of repeating its absolute/delta time.
        detail::append_varuint(out, static_cast<u64>(shared_times.size()));
        u64 prev = 0;
        for (std::size_t i = 0; i < shared_times.size(); ++i) {
            detail::append_varuint(out, shared_times[i] - prev);
            prev = shared_times[i];
        }

        detail::append_varuint(out, count_nonempty_signal_records());
        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            const u32 signal_id = signal_order_[i];
            const SignalState& st = signal_states_[signal_id];
            if (st.transitions.empty()) continue;

            detail::append_varuint(out, signal_id);
            detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
            for (std::size_t t = 0; t < st.transitions.size(); ++t) {
                const Transition& tr = st.transitions[t];
                const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
                std::vector<u64>::const_iterator it = std::lower_bound(shared_times.begin(), shared_times.end(), rel);
                const u64 idx = static_cast<u64>(it - shared_times.begin());
                detail::append_varuint(out, idx);
                append_fixed_value_bytes(out, tr.value, st.byte_width_bytes);
            }
        }
    }

    void build_block_payload_absolute_fixed(const BlockJob& job, std::vector<u8>& out) const {
        out.clear();
        append_wdat_raw_header(out, job, kWdatFixedValueWidth);
        detail::append_varuint(out, count_nonempty_signal_records());

        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            const u32 signal_id = signal_order_[i];
            const SignalState& st = signal_states_[signal_id];
            if (st.transitions.empty()) continue;

            detail::append_varuint(out, signal_id);
            detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
            for (std::size_t t = 0; t < st.transitions.size(); ++t) {
                const Transition& tr = st.transitions[t];
                const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
                detail::append_varuint(out, rel);
                append_fixed_value_bytes(out, tr.value, st.byte_width_bytes);
            }
        }
    }

    void build_block_payload(const BlockJob& job, std::vector<u8>& out) {
        if (options_.enable_delta_time_encoding) {
            build_block_payload_delta(job, out);
        } else {
            build_block_payload_absolute_fixed(job, out);
        }

        if (options_.enable_shared_time_table) {
            std::vector<u8> shared_candidate;
            build_block_payload_shared_time(job, shared_candidate);
            if (shared_candidate.size() < out.size()) out.swap(shared_candidate);
        }
    }

    EncodedBlock encode_block_job(const BlockJob& job) const {
        EncodedBlock e;
        e.block_id = job.block_id;
        e.start_cycle = job.start_cycle;
        e.end_cycle = job.end_cycle;
        e.raw_size = static_cast<u64>(job.raw_payload.size());
        e.compression = Compression::None;

        if (options_.compression == Compression::Zstd) {
#ifndef WVZ4_NO_ZSTD
            const std::size_t bound = ZSTD_compressBound(job.raw_payload.size());
            e.payload.resize(bound);
            std::size_t written = ZSTD_compress(e.payload.data(), bound,
                                                job.raw_payload.data(), job.raw_payload.size(),
                                                options_.zstd_level);
            if (ZSTD_isError(written)) {
                e.error = std::string("ZSTD_compress failed: ") + ZSTD_getErrorName(written);
                return e;
            }
            e.payload.resize(written);
            e.compression = Compression::Zstd;
#else
            e.error = "WVZ4 built with WVZ4_NO_ZSTD but zstd compression was requested";
            return e;
#endif
        } else {
            e.payload = job.raw_payload;
            e.compression = Compression::None;
        }
        return e;
    }

    bool write_encoded_block(const EncodedBlock& e, std::string& error) {
        if (!out_) { error = "output stream is invalid before block write"; return false; }
        const u64 offset = static_cast<u64>(out_.tellp());
        std::vector<u8> payload;
        payload.reserve(32 + e.payload.size());
        detail::append_varuint(payload, e.block_id);
        detail::append_i64(payload, e.start_cycle);
        detail::append_i64(payload, e.end_cycle);
        detail::append_u8(payload, static_cast<u8>(e.compression));
        detail::append_varuint(payload, e.raw_size);
        detail::append_varuint(payload, static_cast<u64>(e.payload.size()));
        payload.insert(payload.end(), e.payload.begin(), e.payload.end());
        if (!write_section("WDAT", payload, error)) return false;
        const u64 end = static_cast<u64>(out_.tellp());

        BlockIndexRecord idx;
        idx.block_id = e.block_id;
        idx.start_cycle = e.start_cycle;
        idx.end_cycle = e.end_cycle;
        idx.file_offset = offset;
        idx.file_size = end - offset;
        idx.raw_size = e.raw_size;
        idx.compression = e.compression;
        block_index_.push_back(idx);
        return true;
    }

    void start_block_pipeline() {
        pipeline_.running = true;
        pipeline_.stop_requested = false;
        pipeline_.compression_done = false;
        pipeline_.next_file_block_id = next_block_id_;
        const u32 n = options_.block_pipeline_threads == 0 ? choose_pipeline_threads() : options_.block_pipeline_threads;
        for (u32 i = 0; i < n; ++i) {
            pipeline_.compression_threads.push_back(std::thread([this]() { this->compression_worker_loop(); }));
        }
        pipeline_.file_thread = std::thread([this]() { this->file_writer_loop(); });
    }

    bool enqueue_block_job(BlockJob&& job, std::string& error) {
        std::unique_lock<std::mutex> lock(pipeline_.mutex);
        if (!pipeline_.error.empty()) { error = pipeline_.error; return false; }
        if (options_.block_pipeline_queue_limit > 0) {
            pipeline_.cv_space.wait(lock, [this]() {
                return pipeline_.jobs.size() < options_.block_pipeline_queue_limit || !pipeline_.error.empty();
            });
            if (!pipeline_.error.empty()) { error = pipeline_.error; return false; }
        }
        pipeline_.jobs.push_back(std::move(job));
        pipeline_.cv_jobs.notify_one();
        return true;
    }

    void compression_worker_loop() {
        while (true) {
            BlockJob job;
            {
                std::unique_lock<std::mutex> lock(pipeline_.mutex);
                pipeline_.cv_jobs.wait(lock, [this]() {
                    return pipeline_.stop_requested || !pipeline_.jobs.empty();
                });
                if (pipeline_.jobs.empty()) {
                    if (pipeline_.stop_requested) return;
                    continue;
                }
                job = std::move(pipeline_.jobs.front());
                pipeline_.jobs.pop_front();
                pipeline_.cv_space.notify_one();
            }
            EncodedBlock result = encode_block_job(job);
            {
                std::lock_guard<std::mutex> lock(pipeline_.mutex);
                if (!result.error.empty()) {
                    if (pipeline_.error.empty()) pipeline_.error = result.error;
                    pipeline_.cv_results.notify_all();
                    pipeline_.cv_space.notify_all();
                    continue;
                }
                pipeline_.results.push_back(std::move(result));
                pipeline_.cv_results.notify_one();
            }
        }
    }

    void file_writer_loop() {
        std::map<u64, EncodedBlock> pending;
        while (true) {
            EncodedBlock r;
            bool have = false;
            {
                std::unique_lock<std::mutex> lock(pipeline_.mutex);
                pipeline_.cv_results.wait(lock, [this]() {
                    return pipeline_.compression_done || !pipeline_.results.empty() || !pipeline_.error.empty();
                });
                if (!pipeline_.results.empty()) {
                    r = std::move(pipeline_.results.front());
                    pipeline_.results.pop_front();
                    have = true;
                } else if (!pipeline_.error.empty()) {
                    break;
                } else if (pipeline_.compression_done) {
                    break;
                }
            }
            if (have) pending.emplace(r.block_id, std::move(r));
            while (true) {
                std::map<u64, EncodedBlock>::iterator it = pending.find(pipeline_.next_file_block_id);
                if (it == pending.end()) break;
                std::string err;
                if (!write_encoded_block(it->second, err)) {
                    std::lock_guard<std::mutex> lock(pipeline_.mutex);
                    if (pipeline_.error.empty()) pipeline_.error = err;
                    pipeline_.cv_jobs.notify_all();
                    pipeline_.cv_results.notify_all();
                    pipeline_.cv_space.notify_all();
                    return;
                }
                pending.erase(it);
                ++pipeline_.next_file_block_id;
            }
        }
        // Drain any final contiguous results.
        while (true) {
            std::map<u64, EncodedBlock>::iterator it = pending.find(pipeline_.next_file_block_id);
            if (it == pending.end()) break;
            std::string err;
            if (!write_encoded_block(it->second, err)) {
                std::lock_guard<std::mutex> lock(pipeline_.mutex);
                if (pipeline_.error.empty()) pipeline_.error = err;
                return;
            }
            pending.erase(it);
            ++pipeline_.next_file_block_id;
        }
        if (!pending.empty()) {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            if (pipeline_.error.empty()) pipeline_.error = "block pipeline stopped with non-contiguous pending blocks";
        }
    }

    bool stop_block_pipeline(std::string& error) {
        if (!pipeline_.running) return true;
        {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            pipeline_.stop_requested = true;
            pipeline_.cv_jobs.notify_all();
            pipeline_.cv_results.notify_all();
            pipeline_.cv_space.notify_all();
        }
        for (std::size_t i = 0; i < pipeline_.compression_threads.size(); ++i) {
            if (pipeline_.compression_threads[i].joinable()) pipeline_.compression_threads[i].join();
        }
        {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            pipeline_.compression_done = true;
            pipeline_.cv_results.notify_all();
        }
        if (pipeline_.file_thread.joinable()) pipeline_.file_thread.join();

        bool ok = true;
        {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            if (!pipeline_.error.empty()) { error = pipeline_.error; ok = false; }
        }
        reset_pipeline_storage();
        return ok;
    }

    bool write_footer_and_patch_header(std::string& error) {
        footer_offset_ = static_cast<u64>(out_.tellp());
        std::vector<u8> payload;
        detail::append_varuint(payload, static_cast<u64>(block_index_.size()));
        for (std::size_t i = 0; i < block_index_.size(); ++i) {
            const BlockIndexRecord& r = block_index_[i];
            detail::append_varuint(payload, r.block_id);
            detail::append_i64(payload, r.start_cycle);
            detail::append_i64(payload, r.end_cycle);
            detail::append_varuint(payload, r.file_offset);
            detail::append_varuint(payload, r.file_size);
            detail::append_varuint(payload, r.raw_size);
            detail::append_u8(payload, static_cast<u8>(r.compression));
        }
        if (!write_section("FOOT", payload, error)) return false;

        out_.seekp(8 + 4 + 4 + 8, std::ios::beg); // magic + version + header_size + block_span
        if (!detail::write_u64(out_, footer_offset_)) { error = "failed to patch footer offset"; return false; }
        out_.seekp(0, std::ios::end);
        return true;
    }

    void reset_all() {
        opened_ = false;
        layout_ = Layout();
        signal_states_.clear();
        signal_order_.clear();
        block_index_.clear();
        have_pending_content_ = false;
        current_block_start_ = 0;
        current_cycle_ = 0;
        have_submitted_cycle_ = false;
        next_block_id_ = 0;
        footer_offset_ = 0;
        update_state_scratch_.clear();
        update_seen_stamp_.clear();
        update_seen_epoch_ = 1;
        reset_pipeline_storage();
    }

    void reset_pipeline_storage() {
        pipeline_.running = false;
        pipeline_.stop_requested = false;
        pipeline_.compression_done = false;
        pipeline_.jobs.clear();
        pipeline_.results.clear();
        pipeline_.compression_threads.clear();
        pipeline_.error.clear();
        pipeline_.next_file_block_id = 0;
    }

private:
    WriterOptions options_;
    Layout layout_;
    bool opened_ = false;
    std::ofstream out_;
    std::vector<SignalState> signal_states_; // indexed by signal_id
    std::vector<u32> signal_order_;          // ascending signal_id order
    std::vector<BlockIndexRecord> block_index_;
    bool have_pending_content_ = false;
    u64 current_block_start_ = 0;
    i64 current_cycle_ = 0;
    bool have_submitted_cycle_ = false;
    u64 next_block_id_ = 0;
    u64 footer_offset_ = 0;
    PipelineState pipeline_;
    std::vector<SignalState*> update_state_scratch_;
    std::vector<u32> update_seen_stamp_;
    u32 update_seen_epoch_ = 1;
};

// Optional cycle-submission queue. This moves Writer::submit_cycle onto a single
// background thread. Writer itself is still accessed by exactly one thread.
class AsyncWriter {
public:
    AsyncWriter() {}
    ~AsyncWriter() { std::string ignored; close(ignored); }

    AsyncWriter(const AsyncWriter&) = delete;
    AsyncWriter& operator=(const AsyncWriter&) = delete;

    bool open(const std::string& path,
              const Layout& layout,
              const WriterOptions& options,
              std::string& error,
              std::size_t queue_limit = 0) {
        std::string close_error;
        if (!close(close_error)) {
            error = close_error.empty() ? "failed to close previous WVZ4 AsyncWriter before reopen" : close_error;
            return false;
        }
        error.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_.clear();
            queue_.clear();
            stop_ = false;
            queue_limit_ = queue_limit;
        }
        if (!writer_.open(path, layout, options, error)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_ = true;
        }
        worker_ = std::thread([this]() { this->worker_loop(); });
        return true;
    }

    bool submit_cycle(const CycleSubmission& submission, std::string& error) {
        return enqueue(CycleSubmission(submission), error);
    }

    bool submit_cycle(CycleSubmission&& submission, std::string& error) {
        return enqueue(std::move(submission), error);
    }

    bool close(std::string& error) {
        error.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!opened_) return true;
            stop_ = true;
            cv_not_empty_.notify_all();
            cv_space_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        bool ok = true;
        std::string local_error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_.empty()) { local_error = error_; ok = false; }
        }
        if (!writer_.close(local_error)) ok = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_ = false;
            queue_.clear();
            stop_ = false;
        }
        if (!ok) error = local_error.empty() ? "WVZ4 AsyncWriter close failed" : local_error;
        return ok;
    }

private:
    bool enqueue(CycleSubmission&& submission, std::string& error) {
        error.clear();
        std::unique_lock<std::mutex> lock(mutex_);
        if (!opened_) { error = "WVZ4 AsyncWriter is not open"; return false; }
        if (stop_) { error = "WVZ4 AsyncWriter is stopping"; return false; }
        if (!error_.empty()) { error = error_; return false; }
        if (queue_limit_ > 0) {
            cv_space_.wait(lock, [this]() { return queue_.size() < queue_limit_ || stop_ || !error_.empty(); });
            if (!error_.empty()) { error = error_; return false; }
            if (stop_) { error = "WVZ4 AsyncWriter is stopping"; return false; }
        }
        queue_.push_back(std::move(submission));
        cv_not_empty_.notify_one();
        return true;
    }

    void worker_loop() {
        while (true) {
            CycleSubmission s;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_not_empty_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stop_) return;
                    continue;
                }
                s = std::move(queue_.front());
                queue_.pop_front();
                cv_space_.notify_one();
            }
            std::string err;
            if (!writer_.submit_cycle(s, err)) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (error_.empty()) error_ = err;
                stop_ = true;
                queue_.clear();
                cv_space_.notify_all();
                return;
            }
        }
    }

private:
    Writer writer_;
    bool opened_ = false;
    bool stop_ = false;
    std::size_t queue_limit_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_space_;
    std::deque<CycleSubmission> queue_;
    std::string error_;
};

} // namespace wvz4

#endif // WVZ4_WRITER_TYPED_H_
