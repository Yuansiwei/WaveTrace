#ifndef WVZ4_WRITER_TYPED_H_
#define WVZ4_WRITER_TYPED_H_

// Protect this public header from Windows-style min/max function-like macros.
// Some business environments define min/max before including project headers;
// those macros break std::min/std::max and std::numeric_limits<T>::max().
#if defined(min)
#pragma push_macro("min")
#undef min
#define WVZ4_WRITER_TYPED_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define WVZ4_WRITER_TYPED_RESTORE_MAX_MACRO_ 1
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef WVZ4_NO_ZSTD
#include <zstd.h>
#endif

namespace wvz4 {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8  = std::uint8_t;

static const u32 kFormatVersion = 12;
static const u8 kSignalFlagStorageOnly = 1u << 0;
static const u32 kMaxScalarBytes = 8;
static const u64 kLodBaseBucketCycles = 256;
static const u32 kLodLevelCount = 7;
static const u64 kLodMinRawTransitionsForTable = 4096;
static const u64 kLodMaxRecordsToSourceRatio = 5; // one LOD level may keep at most 20% of its source records.
static const u64 kLodTimeChunkBuckets = 4096;

// Raw WDAT payload encoding flags. The outer WDAT section and block index remain
// unchanged; these flags describe the uncompressed raw_payload inside each block.
static const u64 kWdatDeltaTimes        = 1ull << 0; // per-signal delta time stream
static const u64 kWdatFixedValueWidth   = 1ull << 1; // no per-transition byte_count
static const u64 kWdatSharedTimeTable   = 1ull << 2; // tile/block-level shared time table
static const u64 kWdatSignalChunkTile   = 1ull << 3; // WVZ4 v3: time block x signal chunk tile
static const u64 kWdatSparseSignalRecords = 1ull << 4; // WVZ4 v4: only active signals are listed in a tile
static const u64 kWdatPerRecordValueCodec = 1ull << 5; // WVZ4 v4: each non-empty signal record starts with ValueRecordCodec

// File-header feature flags. These are separate from per-WDAT-tile raw flags.
static const u64 kHeaderFeatureSignalChunking    = 1ull << 0;
static const u64 kHeaderFeatureClockDefinitions  = 1ull << 1;
static const u64 kHeaderFeatureSparseRecords     = 1ull << 2;
static const u64 kHeaderFeatureBoolToggleCodec   = 1ull << 3;
static const u64 kHeaderFeatureByteMaskCodec     = 1ull << 4;
static const u64 kHeaderFeatureNibbleMaskCodec   = 1ull << 5;
static const u64 kHeaderFeatureLodTables         = 1ull << 6;

enum class ValueRecordCodec : u8 {
    FullValues = 0,       // time + full fixed-width value for every transition
    BoolToggle = 1,       // bool only: first value + transition times, later values toggle
    ByteMask   = 2,       // first full value, then per-byte changed mask + changed bytes

    // WVZ4 v4 size-oriented record variants.  These keep the value payload
    // identical to the matching base codec but replace the per-transition time
    // stream with: first_rel + fixed stride.  Readers must support these codec
    // ids before enabling this writer option.
    FullValuesStride = 3,
    BoolToggleStride = 4,
    ByteMaskStride   = 5,

    // WVZ4 v4 compact changed-byte selector for 3/4-byte scalar values.
    // Two per-transition masks are packed into one byte: low nibble first,
    // high nibble second.  This avoids spending a full mask byte for int32,
    // uint32 and float records where only four selector bits are needed.
    NibbleMask       = 6,
    NibbleMaskStride = 7
};

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
    u32 signal_id = 0;     // logical signal id, fixed at registration/open time
    u32 storage_id = 0;    // physical WDAT stream id; 0 means storage_id == signal_id
    u32 node_id = 0;       // leaf node id in NodeTable
    ValueType type = ValueType::U64;
    u32 bit_width = 64;    // <= 64 in WVZ4 typed writer
    u32 bit_offset = 0;    // bit range within storage_id, least-significant bit is offset 0
    Radix radix = Radix::Auto;
    bool storage_only = false; // hidden physical stream; viewer exposes aliases that reference it
};

// WVZ4 v4 periodic clock descriptor.  A clock signal listed here is not stored
// as ordinary WDAT transitions.  Its value is generated as:
//   value(cycle) = initial_value toggled every period_ticks writer ticks.
struct ClockDefinition {
    u32 signal_id = 0;
    bool initial_value = false;
    u64 period_ticks = 1;
};

struct Layout {
    std::vector<NameRecord> names;
    std::vector<NodeRecord> nodes;
    std::vector<SignalDefinition> signals;
    std::vector<ClockDefinition> clocks;
};

struct WriterOptions {
    u64 target_block_span = 100000; // writer-cycle span; must be > 0
    Compression compression = Compression::Zstd;
    int zstd_level = 3;

    // Block pipeline: commit_block only builds raw block payload and queues it.
    // Compression workers encode blocks; file writer thread appends by block_id order.
    bool enable_block_pipeline = true;
    u32 block_pipeline_threads = 0;        // 0 = auto, roughly hardware_concurrency / 2
    std::size_t block_pipeline_queue_limit = 8; // bounded by default; 0 = unlimited

    // WDAT encoding. Keep both enabled by default: the writer builds candidate
    // encodings per block and stores the smaller one. Both encodings omit
    // per-transition byte_count because fixed value width is defined by SignalTable.
    bool enable_delta_time_encoding = true;
    bool enable_shared_time_table = true;

    // WVZ4 v2+ has an implicit all-zero value for every signal at cycle 0.
    // This avoids writing a huge first block when most signals initialize to 0.
    // Non-zero first values are still written explicitly.
    bool implicit_zero_initial_values = true;

    // WVZ4 v3: split each time block into independent signal chunks.  The chunk
    // for a signal is computed as (signal_id - 1) / signals_per_chunk.  Each tile
    // stores an offset table for all signals in that chunk so a reader can decode
    // one signal after decompressing only its tile.
    bool enable_signal_chunking = true;
    u32 signals_per_chunk = 1024;

    // WVZ4 v4 compression candidates.  The writer builds dense/sparse tile
    // candidates and per-signal value-codec candidates, then keeps the smallest
    // raw payload before optional outer Zstd compression.
    bool enable_sparse_signal_records = true;
    bool enable_bool_toggle_encoding = true;

    // Changed-byte value compression.  Width <= 2 deliberately uses full-value
    // records: the per-transition selector overhead is usually not worth it.
    // Width 3/4 uses NibbleMask/NibbleMaskStride, packing two masks per byte.
    // Width > 4 keeps the original ByteMask/ByteMaskStride codec.
    bool enable_value_byte_mask_encoding = true;

    // WVZ4 v4 size-oriented time optimization.  If all transition cycles in a
    // signal record have the same delta, the writer can store first_rel+stride
    // instead of one time varint per transition.  This changes the WDAT record
    // codec id, so the viewer/reader must support the Stride codec variants.
    bool enable_stride_time_record_encoding = true;

    // LOD tables speed up wide-range rendering but keep sampled transition
    // summaries in memory until FOOT is written. Disable for very large writer
    // stress runs when first-open/on-demand parser behavior is the target.
    bool enable_lod_tables = true;

    // Emit a sidecar text report at close. Empty path means <wvz4-file>.log.
    bool enable_stats_log = true;
    std::string stats_log_path;

    // Optional initial capacity hints. They only reduce allocation; semantics unchanged.
    std::size_t transition_reserve_per_signal = 0;
};

struct ScalarValue {
    std::array<u8, kMaxScalarBytes> bytes;
    u8 byte_count = 0;

    ScalarValue() { bytes.fill(0); }
};

struct CycleValueUpdate {
    // This id names the physical storage stream.  In WVZ4 v5-style layouts,
    // several logical SignalDefinition::signal_id entries may share it through
    // SignalDefinition::storage_id.
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

struct BacklogLogState {
    std::mutex mutex;
    bool initialized = false;
    bool disabled = false;
    std::size_t lines = 0;
    std::size_t max_lines = 512;
    std::string path;
    std::ofstream file;
};

inline BacklogLogState& backlog_log_state() {
    static BacklogLogState state;
    return state;
}

inline std::size_t parse_backlog_log_size_env(const char* name, std::size_t fallback_value) {
    const char* env = std::getenv(name);
    if (!env || !env[0]) return fallback_value;
    char* end = NULL;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (end == env || v == 0ull) return fallback_value;
    const unsigned long long max_size = static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)());
    return static_cast<std::size_t>(v > max_size ? max_size : v);
}

inline unsigned long long backlog_now_ms() {
    typedef std::chrono::steady_clock clock_t;
    static const clock_t::time_point start = clock_t::now();
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_t::now() - start).count());
}

inline void init_backlog_log_locked(BacklogLogState& state) {
    if (state.initialized) return;
    state.initialized = true;

    const char* disable_env = std::getenv("WVZ4_BACKLOG_LOG_DISABLE");
    state.disabled = (disable_env && disable_env[0] && std::strcmp(disable_env, "0") != 0);
    if (state.disabled) return;

    state.max_lines = parse_backlog_log_size_env("WVZ4_BACKLOG_LOG_MAX_LINES", 512u);

    const char* path_env = std::getenv("WVZ4_BACKLOG_LOG_FILE");
    state.path = (path_env && path_env[0]) ? path_env : "wvz4_writer_backlog.log";
    state.file.open(state.path.c_str(), std::ios::out | std::ios::trunc);
    if (!state.file) {
        state.disabled = true;
        return;
    }
    state.file << "WVZ4 writer backlog diagnostic log\n";
    state.file << "max_lines=" << state.max_lines << "\n";
    state.file << "set WVZ4_BACKLOG_LOG_DISABLE=1 to disable; set WVZ4_BACKLOG_LOG_FILE=<path> to change path\n";
    state.file.flush();
}

inline bool backlog_log_disabled() {
    BacklogLogState& state = backlog_log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    init_backlog_log_locked(state);
    return state.disabled || state.lines >= state.max_lines;
}

inline void backlog_log(const char* fmt, ...) {
    BacklogLogState& state = backlog_log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    init_backlog_log_locked(state);
    if (state.disabled || state.lines >= state.max_lines) return;

    char buffer[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    state.file << "[wvz4-backlog] ";
    if (n < 0) {
        state.file << "<format-error>";
    } else {
        state.file << buffer;
        if (static_cast<std::size_t>(n) >= sizeof(buffer)) state.file << " ...<truncated>";
    }
    state.file << "\n";
    ++state.lines;
    if (state.lines == state.max_lines) {
        state.file << "[wvz4-backlog] log line limit reached; further backlog diagnostics suppressed\n";
    }
    state.file.flush();
}

inline void append_u8(std::vector<u8>& out, u8 v) { out.push_back(v); }

inline std::size_t varuint_size(u64 v) {
    std::size_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        ++n;
    }
    return n;
}

inline void reserve_extra(std::vector<u8>& out, std::size_t extra) {
    const std::size_t need = out.size() + extra;
    if (need <= out.capacity()) return;
    std::size_t cap = out.capacity();
    if (cap < 1024u) cap = 1024u;
    while (cap < need && cap <= (std::numeric_limits<std::size_t>::max)() / 2u) cap *= 2u;
    if (cap < need) cap = need;
    out.reserve(cap);
}

inline void append_bytes(std::vector<u8>& out, const void* data, std::size_t size) {
    if (size == 0) return;
    reserve_extra(out, size);
    const std::size_t old = out.size();
    out.resize(old + size);
    std::memcpy(out.data() + old, data, size);
}

inline void append_vector_bytes(std::vector<u8>& out, const std::vector<u8>& src) {
    if (!src.empty()) append_bytes(out, src.data(), src.size());
}

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

inline void append_varuint_slow(std::vector<u8>& out, u64 v) {
    while (v >= 0x80u) {
        out.push_back(static_cast<u8>((v & 0x7fu) | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<u8>(v));
}

inline void append_varuint(std::vector<u8>& out, u64 v) {
    // Hot path: waveform cycle deltas, local signal deltas and record sizes are
    // usually encoded in one or two bytes.  Avoid the generic loop and repeated
    // vector capacity checks for those common cases.  The byte stream is exactly
    // the same unsigned LEB128 encoding as append_varuint_slow().
    if (v < 0x80u) {
        out.push_back(static_cast<u8>(v));
        return;
    }
    if (v < 0x4000u) {
        reserve_extra(out, 2u);
        const std::size_t old = out.size();
        out.resize(old + 2u);
        out[old] = static_cast<u8>((v & 0x7fu) | 0x80u);
        out[old + 1u] = static_cast<u8>(v >> 7);
        return;
    }
    append_varuint_slow(out, v);
}

inline void append_string(std::vector<u8>& out, const std::string& s) {
    append_varuint(out, static_cast<u64>(s.size()));
    if (!s.empty()) append_bytes(out, s.data(), s.size());
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
        output_path_ = path;
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
        if (options_.enable_signal_chunking && options_.signals_per_chunk == 0) {
            error = "WVZ4 WriterOptions.signals_per_chunk must be > 0 when signal chunking is enabled";
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
        ++writer_submit_count_;
        const i64 diag_span = static_cast<i64>(options_.target_block_span);
        const i64 diag_block_end = (current_block_start_ <= static_cast<u64>(std::numeric_limits<i64>::max() - diag_span))
            ? static_cast<i64>(current_block_start_) + diag_span
            : -1;
        if (writer_submit_count_ <= 4u ||
            (writer_submit_count_ & (writer_submit_count_ - 1u)) == 0u ||
            (diag_block_end >= 0 && submission.cycle >= diag_block_end)) {
            writer_diag_log_("writer-submit-enter", &submission, diag_block_end);
        }
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
            if (st.is_periodic_clock) {
                error = detail::make_error("update targets periodic clock signal_id stored by CLKD: ", upd.signal_id);
                return false;
            }
            if (upd.value.byte_count != st.byte_width_bytes) {
                error = detail::make_error("update byte width mismatch for signal_id: ", upd.signal_id);
                return false;
            }
            update_state_scratch_.push_back(&st);
        }

        if (diag_block_end >= 0 && submission.cycle >= diag_block_end) {
            writer_diag_log_("writer-submit-before-flush", &submission, diag_block_end);
        }
        if (!flush_until(submission.cycle, error)) return false;
        if (diag_block_end >= 0 && submission.cycle >= diag_block_end) {
            writer_diag_log_("writer-submit-after-flush", &submission, diag_block_end);
        }

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
            update_lod_tables(upd.signal_id, submission.cycle, upd.value);
            mark_current_block_shared_time(upd.signal_id, submission.cycle);
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
        if (ok && options_.enable_stats_log && !write_stats_log(local_error)) ok = false;

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
        bool is_periodic_clock = false;
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

    struct WdatByteBreakdown {
        // Raw WDAT semantic accounting.  These counters are gathered while the
        // chosen tile payload is being built; no shadow buffers or second-pass
        // compression are used, so the writer's performance profile stays the
        // same apart from a few integer additions.
        u64 tile_header_bytes = 0;           // block id, flags and non-semantic tile fields
        u64 tile_cycle_header_bytes = 0;     // tile start/end cycle fields
        u64 tile_signal_header_bytes = 0;    // chunk id, first signal id, signal count

        u64 cycle_delta_bytes = 0;           // per-record delta/absolute cycle varints
        u64 shared_time_table_bytes = 0;     // shared time table count + deltas
        u64 shared_time_index_bytes = 0;     // per-record indexes into the shared time table

        u64 dense_offset_table_bytes = 0;    // dense signal-offset table
        u64 sparse_active_signal_bytes = 0;  // sparse active-count + local signal deltas
        u64 sparse_record_size_bytes = 0;    // sparse per-record byte sizes
        u64 tile_index_bytes = 0;            // blob-size fields and similar local locators

        u64 record_header_bytes = 0;         // value codec + transition count
        u64 value_payload_bytes = 0;         // real data bytes, including first full value bytes
        u64 value_mask_bytes = 0;            // byte-mask selector bytes

        u64 cycle_id_bytes() const {
            return tile_cycle_header_bytes + cycle_delta_bytes + shared_time_table_bytes + shared_time_index_bytes;
        }

        u64 signal_locator_bytes() const {
            return tile_signal_header_bytes + dense_offset_table_bytes + sparse_active_signal_bytes;
        }

        u64 accounted_bytes() const {
            return tile_header_bytes + tile_cycle_header_bytes + tile_signal_header_bytes +
                   cycle_delta_bytes + shared_time_table_bytes + shared_time_index_bytes +
                   dense_offset_table_bytes + sparse_active_signal_bytes + sparse_record_size_bytes +
                   tile_index_bytes + record_header_bytes + value_payload_bytes + value_mask_bytes;
        }

        void add(const WdatByteBreakdown& o) {
            tile_header_bytes += o.tile_header_bytes;
            tile_cycle_header_bytes += o.tile_cycle_header_bytes;
            tile_signal_header_bytes += o.tile_signal_header_bytes;
            cycle_delta_bytes += o.cycle_delta_bytes;
            shared_time_table_bytes += o.shared_time_table_bytes;
            shared_time_index_bytes += o.shared_time_index_bytes;
            dense_offset_table_bytes += o.dense_offset_table_bytes;
            sparse_active_signal_bytes += o.sparse_active_signal_bytes;
            sparse_record_size_bytes += o.sparse_record_size_bytes;
            tile_index_bytes += o.tile_index_bytes;
            record_header_bytes += o.record_header_bytes;
            value_payload_bytes += o.value_payload_bytes;
            value_mask_bytes += o.value_mask_bytes;
        }
    };

    struct TileStats {
        u64 raw_flags = 0;
        u64 raw_payload_size = 0;
        u64 active_signal_count = 0;
        u64 tile_signal_count = 0;
        u64 full_value_records = 0;
        u64 bool_toggle_records = 0;
        u64 byte_mask_records = 0;
        u64 nibble_mask_records = 0;
        WdatByteBreakdown cost;
    };

    struct WriterStats {
        u64 file_header_bytes = 0;
        u64 total_file_bytes = 0;
        u64 section_bytes = 0;
        u64 layout_section_bytes = 0;
        u64 wdat_section_bytes = 0;
        u64 foot_section_bytes = 0;
        u64 other_section_bytes = 0;
        u64 wdat_raw_bytes = 0;
        u64 wdat_stored_payload_bytes = 0;
        u64 wdat_blocks = 0;
        u64 dense_tiles = 0;
        u64 sparse_tiles = 0;
        u64 delta_time_tiles = 0;
        u64 shared_time_tiles = 0;
        u64 absolute_time_tiles = 0;
        u64 active_signal_records = 0;
        u64 tile_signal_slots = 0;
        u64 full_value_records = 0;
        u64 bool_toggle_records = 0;
        u64 byte_mask_records = 0;
        u64 nibble_mask_records = 0;
        u64 clock_count = 0;
        u64 clock_section_payload_bytes = 0;
        WdatByteBreakdown wdat_cost;
        std::map<std::string, u64> section_bytes_by_tag;
    };

    struct BlockIndexRecord {
        u64 block_id = 0;       // physical tile/block write order id
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        u32 signal_chunk_id = 0;
        u32 first_signal_id = 0;
        u32 signal_count = 0;
        u64 file_offset = 0;
        u64 file_size = 0;
        u64 raw_size = 0;
        Compression compression = Compression::None;
    };

    struct LodChunkIndexRecord {
        u64 chunk_id = 0;
        u32 level_index = 0;
        u32 signal_chunk_id = 0;
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        u64 file_offset = 0;
        u64 file_size = 0;
        u64 raw_size = 0;
        Compression compression = Compression::None;
        u64 storage_count = 0;
        u64 record_count = 0;
    };

    struct LodValidRange {
        i64 start_cycle = 0;
        i64 end_cycle = 0;
    };

    struct LodLevelState {
        u64 min_cycle_delta = 0;
        bool disabled = false;
        bool has_pending_window = false;
        i64 pending_window_start = 0;
        Transition pending_transition;
        std::vector<Transition> transitions;
        bool has_open_valid_range = false;
        i64 open_valid_start = 0;
        i64 open_valid_end = 0;
        std::vector<LodValidRange> valid_ranges;
    };

    struct LodStorageState {
        bool valid = false;
        u8 byte_width = 0;
        ValueType value_type = ValueType::U64;
        u64 raw_transition_count = 0;
        std::vector<LodLevelState> levels;
    };

    struct LodSelectedLevel {
        std::size_t level = 0;
        u64 source_record_count = 0;
    };

    struct LodSelectedStorageLevel {
        u32 storage_id = 0;
        u64 source_record_count = 0;
    };

    struct LodStorageLevelRef {
        u32 storage_id = 0;
        const LodStorageState* storage = NULL;
        const LodLevelState* lod_level = NULL;
        std::vector<Transition> transitions;
        std::vector<LodValidRange> valid_ranges;
        std::size_t cursor = 0;
    };

    struct BlockJob {
        u64 block_id = 0;       // physical tile/block write order id
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        u32 signal_chunk_id = 0;
        u32 first_signal_id = 1;
        u32 signal_count = 0;
        std::vector<u8> raw_payload;
        TileStats stats;
    };

    struct EncodedBlock {
        u64 block_id = 0;
        i64 start_cycle = 0;
        i64 end_cycle = 0;
        u32 signal_chunk_id = 0;
        u32 first_signal_id = 0;
        u32 signal_count = 0;
        std::vector<u8> payload;
        u64 raw_size = 0;
        Compression compression = Compression::None;
        TileStats stats;
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

        // Low-volume runtime diagnostics for backlog/root-cause checks.
        u64 jobs_enqueued = 0;
        u64 jobs_started = 0;
        u64 jobs_finished = 0;
        u64 results_pushed = 0;
        u64 results_written = 0;
        u64 compression_workers_started = 0;
        bool file_writer_started = false;
        std::size_t next_jobs_log_size = 8;
        std::size_t next_results_log_size = 8;
        u32 diag_log_lines = 0;
    };

private:
    static u32 choose_pipeline_threads() {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) return 1;
        unsigned n = hw / 2;
        if (n == 0) n = 1;
        return static_cast<u32>(n);
    }

    static ScalarValue zero_scalar(u8 byte_width) {
        ScalarValue v;
        v.byte_count = byte_width;
        v.bytes.fill(0);
        return v;
    }

    void initialize_lod_bucket_cycles() {
        lod_bucket_cycles_.clear();
        lod_bucket_cycles_.reserve(kLodLevelCount);
        u64 bucket_cycles = kLodBaseBucketCycles;
        for (u32 i = 0; i < kLodLevelCount; ++i) {
            lod_bucket_cycles_.push_back(bucket_cycles);
            if (bucket_cycles <= (std::numeric_limits<u64>::max)() / kLodMaxRecordsToSourceRatio) {
                bucket_cycles *= kLodMaxRecordsToSourceRatio;
            }
        }
    }

    void initialize_lod_storage() {
        initialize_lod_bucket_cycles();
        lod_states_.clear();
        lod_states_.resize(signal_states_.size());
        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            const u32 storage_id = signal_order_[i];
            if (storage_id >= lod_states_.size()) continue;
            const SignalState& st = signal_states_[storage_id];
            if (!st.valid || st.is_periodic_clock) continue;
            LodStorageState& lod = lod_states_[storage_id];
            lod.valid = true;
            lod.byte_width = st.byte_width_bytes;
            lod.value_type = st.def.type;
        }
    }

    void initialize_lod_levels(LodStorageState& storage) {
        if (!storage.levels.empty()) return;
        storage.levels.resize(lod_bucket_cycles_.size());
        for (std::size_t level = 0; level < lod_bucket_cycles_.size(); ++level) {
            storage.levels[level].min_cycle_delta = lod_bucket_cycles_[level];
        }
    }

    static bool append_lod_transition_if_useful(LodLevelState& lod_level,
                                                const Transition& transition,
                                                u64 source_record_count) {
        const u64 max_useful_samples = source_record_count / kLodMaxRecordsToSourceRatio;
        if (max_useful_samples == 0) return false;
        if (!lod_level.transitions.empty() &&
            lod_level.transitions.back().cycle == transition.cycle) {
            lod_level.transitions.back() = transition;
            return true;
        }
        if (static_cast<u64>(lod_level.transitions.size() + 1) > max_useful_samples) return false;
        lod_level.transitions.push_back(transition);
        return true;
    }

    static i64 lod_sampling_window_start(i64 cycle, u64 span) {
        if (cycle <= 0 || span == 0) return 0;
        const u64 ucycle = static_cast<u64>(cycle);
        const u64 start = (ucycle / span) * span;
        const u64 imax = static_cast<u64>((std::numeric_limits<i64>::max)());
        return static_cast<i64>((std::min)(start, imax));
    }

    static i64 lod_sampling_window_end(i64 window_start, u64 span) {
        const u64 imax = static_cast<u64>((std::numeric_limits<i64>::max)());
        const u64 ustart = window_start <= 0 ? 0u : static_cast<u64>(window_start);
        if (span == 0 || span > imax - ustart) return (std::numeric_limits<i64>::max)();
        return static_cast<i64>(ustart + span);
    }

    static void close_lod_valid_range(LodLevelState& lod_level) {
        if (!lod_level.has_open_valid_range) return;
        if (lod_level.open_valid_end > lod_level.open_valid_start) {
            LodValidRange range;
            range.start_cycle = lod_level.open_valid_start;
            range.end_cycle = lod_level.open_valid_end;
            lod_level.valid_ranges.push_back(range);
        }
        lod_level.has_open_valid_range = false;
        lod_level.open_valid_start = 0;
        lod_level.open_valid_end = 0;
    }

    static void extend_lod_valid_range(LodLevelState& lod_level, i64 window_start) {
        const i64 window_end = lod_sampling_window_end(window_start, lod_level.min_cycle_delta);
        if (!lod_level.has_open_valid_range) {
            lod_level.has_open_valid_range = true;
            lod_level.open_valid_start = window_start;
            lod_level.open_valid_end = window_end;
            return;
        }
        if (window_start < lod_level.open_valid_start) lod_level.open_valid_start = window_start;
        if (window_end > lod_level.open_valid_end) lod_level.open_valid_end = window_end;
    }

    static u64 lod_level_materialized_count(const LodLevelState& lod_level) {
        u64 count = static_cast<u64>(lod_level.transitions.size());
        if (lod_level.has_pending_window) {
            if (lod_level.transitions.empty() ||
                lod_level.transitions.back().cycle != lod_level.pending_transition.cycle) {
                ++count;
            }
        }
        return count;
    }

    static std::vector<Transition> materialize_lod_level_transitions(const LodLevelState& lod_level) {
        std::vector<Transition> out = lod_level.transitions;
        if (lod_level.has_pending_window) {
            if (!out.empty() && out.back().cycle == lod_level.pending_transition.cycle) {
                out.back() = lod_level.pending_transition;
            } else {
                out.push_back(lod_level.pending_transition);
            }
        }
        return out;
    }

    static std::vector<LodValidRange> materialize_lod_level_valid_ranges(const LodLevelState& lod_level) {
        std::vector<LodValidRange> out = lod_level.valid_ranges;
        if (lod_level.has_open_valid_range && lod_level.open_valid_end > lod_level.open_valid_start) {
            LodValidRange range;
            range.start_cycle = lod_level.open_valid_start;
            range.end_cycle = lod_level.open_valid_end;
            out.push_back(range);
        }
        if (lod_level.has_pending_window) {
            const i64 start = lod_level.pending_window_start;
            const i64 end = (std::numeric_limits<i64>::max)();
            if (!out.empty() && out.back().end_cycle >= start) {
                out.back().end_cycle = end;
            } else {
                LodValidRange range;
                range.start_cycle = start;
                range.end_cycle = end;
                out.push_back(range);
            }
        }
        return out;
    }

    static void update_lod_level_window(LodLevelState& lod_level,
                                        const Transition& transition,
                                        u64 source_record_count) {
        const i64 window_start = lod_sampling_window_start(transition.cycle, lod_level.min_cycle_delta);
        if (!lod_level.has_pending_window) {
            lod_level.pending_window_start = window_start;
            lod_level.pending_transition = transition;
            lod_level.has_pending_window = true;
            return;
        }
        if (window_start == lod_level.pending_window_start) {
            lod_level.pending_transition = transition;
            return;
        }
        const bool appended = append_lod_transition_if_useful(lod_level, lod_level.pending_transition, source_record_count);
        if (appended) {
            extend_lod_valid_range(lod_level, lod_level.pending_window_start);
        } else {
            close_lod_valid_range(lod_level);
        }
        lod_level.pending_window_start = window_start;
        lod_level.pending_transition = transition;
    }

    void update_lod_tables(u32 storage_id, i64 cycle, const ScalarValue& new_value) {
        if (!options_.enable_lod_tables) return;
        if (cycle < 0 || storage_id >= lod_states_.size()) return;
        LodStorageState& storage = lod_states_[storage_id];
        if (!storage.valid || storage.byte_width == 0) return;
        ++storage.raw_transition_count;
        initialize_lod_levels(storage);
        Transition tr;
        tr.cycle = cycle;
        tr.value = new_value;
        tr.value.byte_count = storage.byte_width;
        u64 source_record_count = storage.raw_transition_count;
        for (std::size_t level = 0; level < storage.levels.size(); ++level) {
            LodLevelState& lod_level = storage.levels[level];
            const u64 min_delta = lod_level.min_cycle_delta;
            if (lod_level.disabled || min_delta == 0) continue;
            update_lod_level_window(lod_level, tr, source_record_count);
            const u64 materialized_count = lod_level_materialized_count(lod_level);
            if (!lod_level.disabled && materialized_count != 0) {
                source_record_count = materialized_count;
            }
        }
    }

    bool validate_and_prepare_layout(const Layout& layout, std::string& error) {
        layout_ = layout;
        if (layout_.names.empty()) { error = "WVZ4 layout requires a non-empty NameTable"; return false; }
        if (layout_.nodes.empty()) { error = "WVZ4 layout requires a non-empty NodeTable"; return false; }
        if (layout_.signals.empty()) { error = "WVZ4 layout requires a non-empty SignalTable"; return false; }

        u32 max_name_id = 0, max_node_id = 0, max_signal_id = 0;
        for (std::size_t i = 0; i < layout_.names.size(); ++i) max_name_id = (std::max)(max_name_id, layout_.names[i].name_id);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) max_node_id = (std::max)(max_node_id, layout_.nodes[i].node_id);
        for (std::size_t i = 0; i < layout_.signals.size(); ++i) {
            max_signal_id = (std::max)(max_signal_id, layout_.signals[i].signal_id);
            max_signal_id = (std::max)(max_signal_id, layout_.signals[i].storage_id != 0 ? layout_.signals[i].storage_id : layout_.signals[i].signal_id);
        }
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

        max_signal_id_ = max_signal_id;
        signal_states_.clear();
        signal_states_.resize(static_cast<std::size_t>(max_signal_id) + 1);
        signal_order_.clear();
        signal_order_.reserve(layout_.signals.size());
        std::vector<u8> seen_signals(max_signal_id + 1, 0);
        std::vector<u8> seen_storages(max_signal_id + 1, 0);
        for (std::size_t i = 0; i < layout_.signals.size(); ++i) {
            SignalDefinition s = layout_.signals[i];
            if (s.storage_id == 0) s.storage_id = s.signal_id;
            if (s.signal_id == 0) { error = "SignalDefinition.signal_id must be positive"; return false; }
            if (s.storage_id == 0 || s.storage_id > max_signal_id) { error = detail::make_error("SignalDefinition.storage_id invalid for signal_id: ", s.signal_id); return false; }
            if (seen_signals[s.signal_id]) { error = detail::make_error("duplicate signal_id: ", s.signal_id); return false; }
            if (s.storage_only) {
                if (s.node_id != 0) {
                    error = detail::make_error("storage-only SignalDefinition must not reference a node_id: ", s.signal_id); return false;
                }
                if (s.storage_id != s.signal_id) {
                    error = detail::make_error("storage-only SignalDefinition must use itself as storage_id: ", s.signal_id); return false;
                }
                if (s.bit_offset != 0) {
                    error = detail::make_error("storage-only SignalDefinition bit_offset must be zero for signal_id: ", s.signal_id); return false;
                }
            } else {
                if (s.node_id == 0 || s.node_id >= seen_nodes.size() || !seen_nodes[s.node_id]) {
                    error = detail::make_error("SignalDefinition references missing node_id: ", s.node_id); return false;
                }
                if (node_first_child[s.node_id] != 0) {
                    error = detail::make_error("SignalDefinition node_id must be a leaf node: ", s.node_id); return false;
                }
                if (node_kind[s.node_id] != NodeKind::SignalLeaf) {
                    error = detail::make_error("SignalDefinition node_id must reference a SignalLeaf node: ", s.node_id); return false;
                }
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
            if (s.bit_offset >= 64u || s.bit_offset + s.bit_width > 64u || s.bit_offset + s.bit_width < s.bit_width) {
                error = detail::make_error("SignalDefinition bit range is invalid for signal_id: ", s.signal_id); return false;
            }
            if (!seen_storages[s.storage_id]) {
                if (s.bit_offset != 0) {
                    error = detail::make_error("SignalDefinition bit_offset requires an existing physical storage signal for signal_id: ", s.signal_id); return false;
                }
                SignalState st;
                st.valid = true;
                st.def = s;
                st.def.signal_id = s.storage_id;
                st.def.storage_id = s.storage_id;
                st.byte_width_bytes = bytes;
                st.has_current = false;
                st.current.byte_count = bytes;
                st.current.bytes.fill(0);
                if (options_.transition_reserve_per_signal > 0) st.transitions.reserve(options_.transition_reserve_per_signal);
                signal_states_[s.storage_id] = st;
                signal_order_.push_back(s.storage_id);
                seen_storages[s.storage_id] = 1;
            } else {
                const SignalState& existing = signal_states_[s.storage_id];
                if (!existing.valid) {
                    error = detail::make_error("SignalDefinition.storage_id has incompatible logical aliases for signal_id: ", s.signal_id);
                    return false;
                }
                const u32 storage_bits = static_cast<u32>(existing.byte_width_bytes) * 8u;
                if (s.bit_offset + s.bit_width > storage_bits) {
                    error = detail::make_error("SignalDefinition bit range exceeds storage capacity for signal_id: ", s.signal_id);
                    return false;
                }
            }
            seen_signals[s.signal_id] = 1;
        }
        std::vector<u8> seen_clocks(max_signal_id + 1, 0);
        for (std::size_t i = 0; i < layout_.clocks.size(); ++i) {
            const ClockDefinition& c = layout_.clocks[i];
            if (c.signal_id == 0 || c.signal_id >= signal_states_.size() || !signal_states_[c.signal_id].valid) {
                error = detail::make_error("ClockDefinition references missing signal_id: ", c.signal_id);
                return false;
            }
            if (seen_clocks[c.signal_id]) {
                error = detail::make_error("duplicate ClockDefinition signal_id: ", c.signal_id);
                return false;
            }
            if (c.period_ticks == 0) {
                error = detail::make_error("ClockDefinition.period_ticks must be positive for signal_id: ", c.signal_id);
                return false;
            }
            SignalState& st = signal_states_[c.signal_id];
            if (st.def.type != ValueType::Bool || st.def.bit_width != 1u) {
                error = detail::make_error("ClockDefinition must reference a 1-bit Bool signal_id: ", c.signal_id);
                return false;
            }
            st.is_periodic_clock = true;
            st.has_current = true;
            st.current.byte_count = 1;
            st.current.bytes.fill(0);
            st.current.bytes[0] = c.initial_value ? 1u : 0u;
            seen_clocks[c.signal_id] = 1;
        }

        std::sort(signal_order_.begin(), signal_order_.end());

        // One shared-time vector per signal chunk for the current time block.
        // These vectors are maintained incrementally in submit_cycle(), so
        // commit_block() no longer needs to rescan all transitions and sort.
        const u32 chunk_count = options_.enable_signal_chunking
            ? ((max_signal_id_ + options_.signals_per_chunk - 1u) / options_.signals_per_chunk)
            : 1u;
        current_block_shared_times_by_chunk_.clear();
        current_block_shared_times_by_chunk_.resize(chunk_count == 0 ? 1u : chunk_count);
        if (options_.enable_lod_tables) {
            initialize_lod_storage();
        } else {
            initialize_lod_bucket_cycles();
            lod_states_.clear();
        }
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
        if (!detail::write_u64(out_, options_.enable_signal_chunking ? options_.signals_per_chunk : 0)) { error = "failed to write WVZ4 signals_per_chunk"; return false; }
        u64 feature_flags = 0;
        if (options_.enable_signal_chunking) feature_flags |= kHeaderFeatureSignalChunking;
        if (!layout_.clocks.empty()) feature_flags |= kHeaderFeatureClockDefinitions;
        if (options_.enable_sparse_signal_records) feature_flags |= kHeaderFeatureSparseRecords;
        if (options_.enable_bool_toggle_encoding) feature_flags |= kHeaderFeatureBoolToggleCodec;
        if (options_.enable_value_byte_mask_encoding) feature_flags |= kHeaderFeatureByteMaskCodec;
        if (options_.enable_value_byte_mask_encoding) feature_flags |= kHeaderFeatureNibbleMaskCodec;
        if (options_.enable_lod_tables) feature_flags |= kHeaderFeatureLodTables;
        if (!detail::write_u64(out_, feature_flags)) { error = "failed to write WVZ4 feature flags"; return false; }
        if (!detail::write_u64(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        if (!detail::write_u32(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        if (!detail::write_u32(out_, 0)) { error = "failed to write WVZ4 reserved"; return false; }
        stats_.file_header_bytes = 64;
        return true;
    }

    void record_section_stats(const char tag[4], u64 payload_size) {
        const std::string key(tag, tag + 4);
        const u64 section_bytes = 12u + payload_size;
        stats_.section_bytes += section_bytes;
        stats_.section_bytes_by_tag[key] += section_bytes;
        if (key == "WDAT") stats_.wdat_section_bytes += section_bytes;
        else if (key == "FOOT") stats_.foot_section_bytes += section_bytes;
        else if (key == "NAME" || key == "NAMZ" || key == "NODE" || key == "NODZ" ||
                 key == "SIGT" || key == "SIGZ" || key == "CLKD" || key == "CLKZ") {
            stats_.layout_section_bytes += section_bytes;
        } else {
            stats_.other_section_bytes += section_bytes;
        }
    }

    bool write_section(const char tag[4], const std::vector<u8>& payload, std::string& error) {
        if (!detail::write_all(out_, tag, 4)) { error = "failed to write section tag"; return false; }
        if (!detail::write_u64(out_, static_cast<u64>(payload.size()))) { error = "failed to write section length"; return false; }
        if (!detail::write_all(out_, payload.data(), payload.size())) { error = "failed to write section payload"; return false; }
        record_section_stats(tag, static_cast<u64>(payload.size()));
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
        detail::append_vector_bytes(zpayload, compressed);
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
            detail::append_varuint(payload, s.storage_id != 0 ? s.storage_id : s.signal_id);
            detail::append_varuint(payload, s.node_id);
            detail::append_u8(payload, static_cast<u8>(s.type));
            detail::append_varuint(payload, s.bit_width);
            detail::append_u8(payload, static_cast<u8>(s.radix));
            detail::append_varuint(payload, s.bit_offset);
            detail::append_u8(payload, s.storage_only ? kSignalFlagStorageOnly : 0u);
        }
        if (!write_layout_section("SIGT", "SIGZ", payload, error)) return false;

        payload.clear();
        detail::append_varuint(payload, static_cast<u64>(layout.clocks.size()));
        std::vector<ClockDefinition> clocks = layout.clocks;
        std::sort(clocks.begin(), clocks.end(), [](const ClockDefinition& a, const ClockDefinition& b) { return a.signal_id < b.signal_id; });
        for (std::size_t i = 0; i < clocks.size(); ++i) {
            const ClockDefinition& c = clocks[i];
            detail::append_varuint(payload, c.signal_id);
            detail::append_u8(payload, c.initial_value ? 1u : 0u);
            detail::append_varuint(payload, c.period_ticks);
        }
        stats_.clock_count = static_cast<u64>(clocks.size());
        stats_.clock_section_payload_bytes = static_cast<u64>(payload.size());
        if (!layout.clocks.empty()) {
            if (!write_layout_section("CLKD", "CLKZ", payload, error)) return false;
        }
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
                writer_diag_log_("writer-commit-begin", NULL, end_cycle);
                if (!commit_block(end_cycle, error)) return false;
                writer_diag_log_("writer-commit-end", NULL, end_cycle);
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
        if (!options_.enable_signal_chunking) {
            BlockJob job;
            job.block_id = next_block_id_++;
            job.start_cycle = static_cast<i64>(current_block_start_);
            job.end_cycle = end_cycle;
            job.signal_chunk_id = 0;
            job.first_signal_id = 1;
            job.signal_count = max_signal_id_;
            build_block_payload(job);
            writer_diag_log_("writer-commit-single-job-built", NULL, end_cycle, 1u, static_cast<u64>(job.raw_payload.size()));

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

        const u32 spc = options_.signals_per_chunk;
        const u32 chunk_count = (max_signal_id_ + spc - 1u) / spc;
        std::vector<BlockJob> jobs;
        jobs.reserve(chunk_count);
        for (u32 chunk = 0; chunk < chunk_count; ++chunk) {
            const u32 first_signal = chunk * spc + 1u;
            const u32 count = std::min<u32>(spc, max_signal_id_ - first_signal + 1u);
            if (!signal_chunk_has_content(first_signal, count)) continue;
            BlockJob job;
            job.block_id = next_block_id_++;
            job.start_cycle = static_cast<i64>(current_block_start_);
            job.end_cycle = end_cycle;
            job.signal_chunk_id = chunk;
            job.first_signal_id = first_signal;
            job.signal_count = count;
            build_block_payload(job);
            jobs.push_back(std::move(job));
        }

        {
            u64 total_raw_payload = 0;
            for (std::size_t i = 0; i < jobs.size(); ++i) total_raw_payload += static_cast<u64>(jobs[i].raw_payload.size());
            writer_diag_log_("writer-commit-jobs-built", NULL, end_cycle, jobs.size(), total_raw_payload);
        }

        for (std::size_t i = 0; i < jobs.size(); ++i) {
            bool committed = false;
            if (options_.enable_block_pipeline && pipeline_.running) {
                committed = enqueue_block_job(std::move(jobs[i]), error);
            } else {
                EncodedBlock encoded = encode_block_job(jobs[i]);
                if (!encoded.error.empty()) { error = encoded.error; return false; }
                committed = write_encoded_block(encoded, error);
            }
            if (!committed) return false;
        }
        clear_committed_transitions();
        return true;
    }

    void clear_committed_transitions() {
        for (std::size_t i = 0; i < signal_order_.size(); ++i) {
            SignalState& st = signal_states_[signal_order_[i]];
            st.transitions.clear();
        }
        clear_current_block_shared_times();
        have_pending_content_ = false;
    }

    u32 shared_time_chunk_id_for_signal(u32 signal_id) const {
        if (!options_.enable_signal_chunking) return 0u;
        return (signal_id - 1u) / options_.signals_per_chunk;
    }

    void ensure_current_block_shared_time_chunks() {
        if (!current_block_shared_times_by_chunk_.empty()) return;
        const u32 chunk_count = options_.enable_signal_chunking
            ? ((max_signal_id_ + options_.signals_per_chunk - 1u) / options_.signals_per_chunk)
            : 1u;
        current_block_shared_times_by_chunk_.resize(chunk_count == 0 ? 1u : chunk_count);
    }

    void clear_current_block_shared_times() {
        for (std::size_t i = 0; i < current_block_shared_times_by_chunk_.size(); ++i) {
            current_block_shared_times_by_chunk_[i].clear();
        }
    }

    void mark_current_block_shared_time(u32 signal_id, i64 cycle) {
        if (cycle < static_cast<i64>(current_block_start_)) return; // defensive; should never happen.
        ensure_current_block_shared_time_chunks();
        const u32 chunk_id = shared_time_chunk_id_for_signal(signal_id);
        if (chunk_id >= current_block_shared_times_by_chunk_.size()) {
            current_block_shared_times_by_chunk_.resize(static_cast<std::size_t>(chunk_id) + 1u);
        }
        const u64 rel = static_cast<u64>(cycle - static_cast<i64>(current_block_start_));
        std::vector<u64>& times = current_block_shared_times_by_chunk_[chunk_id];
        if (times.empty() || times.back() != rel) {
            // submit_cycle() is strictly monotonic, so this vector is naturally
            // sorted and unique for each chunk.
            times.push_back(rel);
        }
    }

    const std::vector<u64>* shared_times_for_job(const BlockJob& job) const {
        const u32 chunk_id = options_.enable_signal_chunking ? job.signal_chunk_id : 0u;
        if (chunk_id >= current_block_shared_times_by_chunk_.size()) return NULL;
        const std::vector<u64>& times = current_block_shared_times_by_chunk_[chunk_id];
        return times.empty() ? NULL : &times;
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
        // Common scalar widths are tiny.  Keep the hot path simple and avoid
        // iterator/range insert helpers; append_bytes() still handles unusual
        // widths safely.
        switch (fixed_width) {
        case 0:
            return;
        case 1:
            out.push_back(value.bytes[0]);
            return;
        case 2:
        case 4:
        case 8:
            detail::append_bytes(out, value.bytes.data(), fixed_width);
            return;
        default:
            detail::append_bytes(out, value.bytes.data(), fixed_width);
            return;
        }
    }

    static u64 appended_size_since(const std::vector<u8>& out, std::size_t before) {
        return static_cast<u64>(out.size() - before);
    }

    static void append_wdat_raw_header(std::vector<u8>& out, const BlockJob& job, u64 flags, WdatByteBreakdown* cost) {
        std::size_t before = out.size();
        detail::append_varuint(out, job.block_id);
        if (cost) cost->tile_header_bytes += appended_size_since(out, before);

        before = out.size();
        detail::append_i64(out, job.start_cycle);
        detail::append_i64(out, job.end_cycle);
        if (cost) cost->tile_cycle_header_bytes += appended_size_since(out, before);

        before = out.size();
        detail::append_varuint(out, flags | kWdatSignalChunkTile);
        if (cost) cost->tile_header_bytes += appended_size_since(out, before);

        before = out.size();
        detail::append_varuint(out, job.signal_chunk_id);
        detail::append_varuint(out, job.first_signal_id);
        detail::append_varuint(out, job.signal_count);
        if (cost) cost->tile_signal_header_bytes += appended_size_since(out, before);
    }

    bool signal_chunk_has_content(u32 first_signal_id, u32 signal_count) const {
        const u32 last = first_signal_id + signal_count;
        const u32 limit = std::min<u32>(last, static_cast<u32>(signal_states_.size()));
        for (u32 id = first_signal_id; id < limit; ++id) {
            if (signal_states_[id].valid && !signal_states_[id].is_periodic_clock && !signal_states_[id].transitions.empty()) return true;
        }
        return false;
    }

    static u64 find_shared_time_index_monotonic(const std::vector<u64>* shared_times,
                                                u64 rel,
                                                std::size_t& shared_pos) {
        assert(shared_times != NULL);
        if (shared_pos < shared_times->size() && (*shared_times)[shared_pos] <= rel) {
            while (shared_pos < shared_times->size() && (*shared_times)[shared_pos] < rel) {
                ++shared_pos;
            }
        } else {
            shared_pos = static_cast<std::size_t>(std::lower_bound(shared_times->begin(), shared_times->end(), rel) - shared_times->begin());
        }
        assert(shared_pos < shared_times->size());
        assert((*shared_times)[shared_pos] == rel);
        return static_cast<u64>(shared_pos);
    }

    static void append_record_time(std::vector<u8>& out,
                                   u64 rel,
                                   u64& prev_rel,
                                   bool use_shared_time,
                                   const std::vector<u64>* shared_times,
                                   std::size_t& shared_pos,
                                   WdatByteBreakdown* cost) {
        const std::size_t before = out.size();
        if (use_shared_time) {
            const u64 index = find_shared_time_index_monotonic(shared_times, rel, shared_pos);
            detail::append_varuint(out, index);
            if (cost) cost->shared_time_index_bytes += appended_size_since(out, before);
        } else {
            detail::append_varuint(out, rel - prev_rel);
            prev_rel = rel;
            if (cost) cost->cycle_delta_bytes += appended_size_since(out, before);
        }
    }

    static void append_record_stride_time(std::vector<u8>& out,
                                          u64 first_rel,
                                          u64 stride,
                                          WdatByteBreakdown* cost) {
        const std::size_t before = out.size();
        detail::append_varuint(out, first_rel);
        detail::append_varuint(out, stride);
        if (cost) cost->cycle_delta_bytes += appended_size_since(out, before);
    }

    static std::size_t record_stride_time_size(u64 first_rel, u64 stride) {
        return detail::varuint_size(first_rel) + detail::varuint_size(stride);
    }

    void append_signal_full_record(u32 signal_id,
                                   const BlockJob& job,
                                   bool use_shared_time,
                                   const std::vector<u64>* shared_times,
                                   std::vector<u8>& out,
                                   WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::FullValues));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        for (std::size_t t = 0; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            append_record_time(out, rel, prev_rel, use_shared_time, shared_times, shared_pos, cost);
            before = out.size();
            append_fixed_value_bytes(out, tr.value, st.byte_width_bytes);
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_signal_full_stride_record(u32 signal_id,
                                          const BlockJob& job,
                                          u64 stride,
                                          std::vector<u8>& out,
                                          WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::FullValuesStride));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (st.transitions.empty()) return;

        const u64 first_rel = static_cast<u64>(st.transitions[0].cycle - job.start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
        for (std::size_t t = 0; t < st.transitions.size(); ++t) {
            before = out.size();
            append_fixed_value_bytes(out, st.transitions[t].value, st.byte_width_bytes);
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    bool can_use_bool_toggle_record(const SignalState& st) const {
        if (!options_.enable_bool_toggle_encoding) return false;
        if (st.def.type != ValueType::Bool || st.byte_width_bytes != 1) return false;
        if (st.transitions.empty()) return false;
        for (std::size_t i = 0; i < st.transitions.size(); ++i) {
            if (st.transitions[i].value.bytes[0] > 1u) return false;
            if (i > 0 && st.transitions[i].value.bytes[0] == st.transitions[i - 1].value.bytes[0]) return false;
        }
        return true;
    }

    void append_signal_bool_toggle_record(u32 signal_id,
                                          const BlockJob& job,
                                          bool use_shared_time,
                                          const std::vector<u64>* shared_times,
                                          std::vector<u8>& out,
                                          WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::BoolToggle));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        before = out.size();
        detail::append_u8(out, st.transitions.empty() ? 0u : (st.transitions[0].value.bytes[0] ? 1u : 0u));
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        for (std::size_t t = 0; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            append_record_time(out, rel, prev_rel, use_shared_time, shared_times, shared_pos, cost);
        }
    }

    void append_signal_bool_toggle_stride_record(u32 signal_id,
                                                 const BlockJob& job,
                                                 u64 stride,
                                                 std::vector<u8>& out,
                                                 WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::BoolToggleStride));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        before = out.size();
        detail::append_u8(out, st.transitions.empty() ? 0u : (st.transitions[0].value.bytes[0] ? 1u : 0u));
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        if (st.transitions.empty()) return;

        const u64 first_rel = static_cast<u64>(st.transitions[0].cycle - job.start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
    }

    void append_signal_byte_mask_record(u32 signal_id,
                                        const BlockJob& job,
                                        bool use_shared_time,
                                        const std::vector<u64>* shared_times,
                                        std::vector<u8>& out,
                                        WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::ByteMask));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        if (st.transitions.empty()) return;

        const Transition& first = st.transitions[0];
        append_record_time(out, static_cast<u64>(first.cycle - job.start_cycle), prev_rel, use_shared_time, shared_times, shared_pos, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, st.byte_width_bytes);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            append_record_time(out, rel, prev_rel, use_shared_time, shared_times, shared_pos, cost);
            u8 mask = 0;
            for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                if (tr.value.bytes[b] != prev[b]) mask = static_cast<u8>(mask | static_cast<u8>(1u << b));
            }
            before = out.size();
            detail::append_u8(out, mask);
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);
            before = out.size();
            for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                if (mask & static_cast<u8>(1u << b)) out.push_back(tr.value.bytes[b]);
                prev[b] = tr.value.bytes[b];
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_signal_byte_mask_stride_record(u32 signal_id,
                                               const BlockJob& job,
                                               u64 stride,
                                               std::vector<u8>& out,
                                               WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::ByteMaskStride));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (st.transitions.empty()) return;

        const Transition& first = st.transitions[0];
        const u64 first_rel = static_cast<u64>(first.cycle - job.start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, st.byte_width_bytes);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            u8 mask = 0;
            for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                if (tr.value.bytes[b] != prev[b]) mask = static_cast<u8>(mask | static_cast<u8>(1u << b));
            }
            before = out.size();
            detail::append_u8(out, mask);
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);
            before = out.size();
            for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                if (mask & static_cast<u8>(1u << b)) out.push_back(tr.value.bytes[b]);
                prev[b] = tr.value.bytes[b];
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }


    static u8 changed_byte_mask_for_width(const ScalarValue& value,
                                          const std::array<u8, kMaxScalarBytes>& prev,
                                          u8 byte_width) {
        u8 mask = 0;
        for (u8 b = 0; b < byte_width; ++b) {
            if (value.bytes[b] != prev[b]) mask = static_cast<u8>(mask | static_cast<u8>(1u << b));
        }
        return mask;
    }

    static void append_changed_value_bytes_by_mask(const ScalarValue& value,
                                                   u8 byte_width,
                                                   u8 mask,
                                                   std::array<u8, kMaxScalarBytes>& prev,
                                                   std::vector<u8>& out) {
        for (u8 b = 0; b < byte_width; ++b) {
            if (mask & static_cast<u8>(1u << b)) out.push_back(value.bytes[b]);
            prev[b] = value.bytes[b];
        }
    }

    void append_signal_nibble_mask_record(u32 signal_id,
                                          const BlockJob& job,
                                          bool use_shared_time,
                                          const std::vector<u64>* shared_times,
                                          std::vector<u8>& out,
                                          WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::NibbleMask));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (st.transitions.empty()) return;

        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        const Transition& first = st.transitions[0];
        append_record_time(out, static_cast<u64>(first.cycle - job.start_cycle), prev_rel, use_shared_time, shared_times, shared_pos, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, st.byte_width_bytes);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < st.transitions.size(); t += 2u) {
            const Transition& tr0 = st.transitions[t];
            const u64 rel0 = static_cast<u64>(tr0.cycle - job.start_cycle);
            append_record_time(out, rel0, prev_rel, use_shared_time, shared_times, shared_pos, cost);
            const u8 mask0 = changed_byte_mask_for_width(tr0.value, prev, st.byte_width_bytes);

            u8 mask1 = 0;
            if (t + 1u < st.transitions.size()) {
                std::array<u8, kMaxScalarBytes> after0 = prev;
                for (u8 b = 0; b < st.byte_width_bytes; ++b) after0[b] = tr0.value.bytes[b];
                const Transition& tr1 = st.transitions[t + 1u];
                const u64 rel1 = static_cast<u64>(tr1.cycle - job.start_cycle);
                append_record_time(out, rel1, prev_rel, use_shared_time, shared_times, shared_pos, cost);
                mask1 = changed_byte_mask_for_width(tr1.value, after0, st.byte_width_bytes);
            }

            before = out.size();
            detail::append_u8(out, static_cast<u8>((mask0 & 0x0fu) | static_cast<u8>((mask1 & 0x0fu) << 4)));
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);

            before = out.size();
            append_changed_value_bytes_by_mask(tr0.value, st.byte_width_bytes, mask0, prev, out);
            if (t + 1u < st.transitions.size()) {
                const Transition& tr1 = st.transitions[t + 1u];
                append_changed_value_bytes_by_mask(tr1.value, st.byte_width_bytes, mask1, prev, out);
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_signal_nibble_mask_stride_record(u32 signal_id,
                                                 const BlockJob& job,
                                                 u64 stride,
                                                 std::vector<u8>& out,
                                                 WdatByteBreakdown* cost) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::NibbleMaskStride));
        detail::append_varuint(out, static_cast<u64>(st.transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (st.transitions.empty()) return;

        const Transition& first = st.transitions[0];
        const u64 first_rel = static_cast<u64>(first.cycle - job.start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, st.byte_width_bytes);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < st.transitions.size(); t += 2u) {
            const Transition& tr0 = st.transitions[t];
            const u8 mask0 = changed_byte_mask_for_width(tr0.value, prev, st.byte_width_bytes);

            u8 mask1 = 0;
            if (t + 1u < st.transitions.size()) {
                std::array<u8, kMaxScalarBytes> after0 = prev;
                for (u8 b = 0; b < st.byte_width_bytes; ++b) after0[b] = tr0.value.bytes[b];
                const Transition& tr1 = st.transitions[t + 1u];
                mask1 = changed_byte_mask_for_width(tr1.value, after0, st.byte_width_bytes);
            }

            before = out.size();
            detail::append_u8(out, static_cast<u8>((mask0 & 0x0fu) | static_cast<u8>((mask1 & 0x0fu) << 4)));
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);

            before = out.size();
            append_changed_value_bytes_by_mask(tr0.value, st.byte_width_bytes, mask0, prev, out);
            if (t + 1u < st.transitions.size()) {
                const Transition& tr1 = st.transitions[t + 1u];
                append_changed_value_bytes_by_mask(tr1.value, st.byte_width_bytes, mask1, prev, out);
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    static std::size_t record_time_encoded_size(u64 rel,
                                                u64& prev_rel,
                                                bool use_shared_time,
                                                const std::vector<u64>* shared_times,
                                                std::size_t& shared_pos) {
        if (use_shared_time) {
            const u64 index = find_shared_time_index_monotonic(shared_times, rel, shared_pos);
            return detail::varuint_size(index);
        }
        const std::size_t n = detail::varuint_size(rel - prev_rel);
        prev_rel = rel;
        return n;
    }

    std::size_t estimate_signal_full_record_size(u32 signal_id,
                                                 const BlockJob& job,
                                                 bool use_shared_time,
                                                 const std::vector<u64>* shared_times) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t n = 1u + detail::varuint_size(static_cast<u64>(st.transitions.size()));
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        for (std::size_t t = 0; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            n += record_time_encoded_size(rel, prev_rel, use_shared_time, shared_times, shared_pos);
            n += st.byte_width_bytes;
        }
        return n;
    }

    std::size_t estimate_signal_bool_toggle_record_size(u32 signal_id,
                                                        const BlockJob& job,
                                                        bool use_shared_time,
                                                        const std::vector<u64>* shared_times) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t n = 1u + detail::varuint_size(static_cast<u64>(st.transitions.size())) + 1u;
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        for (std::size_t t = 0; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            n += record_time_encoded_size(rel, prev_rel, use_shared_time, shared_times, shared_pos);
        }
        return n;
    }

    std::size_t estimate_signal_byte_mask_record_size(u32 signal_id,
                                                      const BlockJob& job,
                                                      bool use_shared_time,
                                                      const std::vector<u64>* shared_times) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t n = 1u + detail::varuint_size(static_cast<u64>(st.transitions.size()));
        if (st.transitions.empty()) return n;
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        const Transition& first = st.transitions[0];
        n += record_time_encoded_size(static_cast<u64>(first.cycle - job.start_cycle), prev_rel, use_shared_time, shared_times, shared_pos);
        n += st.byte_width_bytes;
        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            n += record_time_encoded_size(rel, prev_rel, use_shared_time, shared_times, shared_pos);
            n += 1u;
            for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                if (tr.value.bytes[b] != prev[b]) ++n;
                prev[b] = tr.value.bytes[b];
            }
        }
        return n;
    }


    std::size_t estimate_signal_nibble_mask_record_size(u32 signal_id,
                                                        const BlockJob& job,
                                                        bool use_shared_time,
                                                        const std::vector<u64>* shared_times) const {
        const SignalState& st = signal_states_[signal_id];
        std::size_t n = 1u + detail::varuint_size(static_cast<u64>(st.transitions.size()));
        if (st.transitions.empty()) return n;
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        const Transition& first = st.transitions[0];
        n += record_time_encoded_size(static_cast<u64>(first.cycle - job.start_cycle), prev_rel, use_shared_time, shared_times, shared_pos);
        n += st.byte_width_bytes;
        n += (st.transitions.size() - 1u + 1u) / 2u; // two <=4-bit masks per selector byte
        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < st.transitions.size(); ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            n += record_time_encoded_size(rel, prev_rel, use_shared_time, shared_times, shared_pos);
            for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                if (tr.value.bytes[b] != prev[b]) ++n;
                prev[b] = tr.value.bytes[b];
            }
        }
        return n;
    }

    struct SignalRecordChoice {
        ValueRecordCodec codec = ValueRecordCodec::FullValues;
        std::size_t encoded_size = 0;
        u64 stride = 0;
    };

    void append_value_full_record(const std::vector<Transition>& transitions,
                                  i64 start_cycle,
                                  u8 byte_width,
                                  bool use_shared_time,
                                  const std::vector<u64>* shared_times,
                                  std::vector<u8>& out,
                                  WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::FullValues));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        for (std::size_t t = 0; t < transitions.size(); ++t) {
            const Transition& tr = transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - start_cycle);
            append_record_time(out, rel, prev_rel, use_shared_time, shared_times, shared_pos, cost);
            before = out.size();
            append_fixed_value_bytes(out, tr.value, byte_width);
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_value_full_stride_record(const std::vector<Transition>& transitions,
                                         i64 start_cycle,
                                         u8 byte_width,
                                         u64 stride,
                                         std::vector<u8>& out,
                                         WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::FullValuesStride));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (transitions.empty()) return;

        const u64 first_rel = static_cast<u64>(transitions[0].cycle - start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
        for (std::size_t t = 0; t < transitions.size(); ++t) {
            before = out.size();
            append_fixed_value_bytes(out, transitions[t].value, byte_width);
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_value_bool_toggle_record(const std::vector<Transition>& transitions,
                                         i64 start_cycle,
                                         bool use_shared_time,
                                         const std::vector<u64>* shared_times,
                                         std::vector<u8>& out,
                                         WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::BoolToggle));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        before = out.size();
        detail::append_u8(out, transitions.empty() ? 0u : (transitions[0].value.bytes[0] ? 1u : 0u));
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        for (std::size_t t = 0; t < transitions.size(); ++t) {
            const Transition& tr = transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - start_cycle);
            append_record_time(out, rel, prev_rel, use_shared_time, shared_times, shared_pos, cost);
        }
    }

    void append_value_bool_toggle_stride_record(const std::vector<Transition>& transitions,
                                                i64 start_cycle,
                                                u64 stride,
                                                std::vector<u8>& out,
                                                WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::BoolToggleStride));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        before = out.size();
        detail::append_u8(out, transitions.empty() ? 0u : (transitions[0].value.bytes[0] ? 1u : 0u));
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        if (transitions.empty()) return;

        const u64 first_rel = static_cast<u64>(transitions[0].cycle - start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
    }

    void append_value_byte_mask_record(const std::vector<Transition>& transitions,
                                       i64 start_cycle,
                                       u8 byte_width,
                                       bool use_shared_time,
                                       const std::vector<u64>* shared_times,
                                       std::vector<u8>& out,
                                       WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::ByteMask));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        if (transitions.empty()) return;

        const Transition& first = transitions[0];
        append_record_time(out, static_cast<u64>(first.cycle - start_cycle), prev_rel, use_shared_time, shared_times, shared_pos, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, byte_width);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < transitions.size(); ++t) {
            const Transition& tr = transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - start_cycle);
            append_record_time(out, rel, prev_rel, use_shared_time, shared_times, shared_pos, cost);
            u8 mask = 0;
            for (u8 b = 0; b < byte_width; ++b) {
                if (tr.value.bytes[b] != prev[b]) mask = static_cast<u8>(mask | static_cast<u8>(1u << b));
            }
            before = out.size();
            detail::append_u8(out, mask);
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);
            before = out.size();
            for (u8 b = 0; b < byte_width; ++b) {
                if (mask & static_cast<u8>(1u << b)) out.push_back(tr.value.bytes[b]);
                prev[b] = tr.value.bytes[b];
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_value_byte_mask_stride_record(const std::vector<Transition>& transitions,
                                              i64 start_cycle,
                                              u8 byte_width,
                                              u64 stride,
                                              std::vector<u8>& out,
                                              WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::ByteMaskStride));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (transitions.empty()) return;

        const Transition& first = transitions[0];
        const u64 first_rel = static_cast<u64>(first.cycle - start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, byte_width);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < transitions.size(); ++t) {
            const Transition& tr = transitions[t];
            u8 mask = 0;
            for (u8 b = 0; b < byte_width; ++b) {
                if (tr.value.bytes[b] != prev[b]) mask = static_cast<u8>(mask | static_cast<u8>(1u << b));
            }
            before = out.size();
            detail::append_u8(out, mask);
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);
            before = out.size();
            for (u8 b = 0; b < byte_width; ++b) {
                if (mask & static_cast<u8>(1u << b)) out.push_back(tr.value.bytes[b]);
                prev[b] = tr.value.bytes[b];
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_value_nibble_mask_record(const std::vector<Transition>& transitions,
                                         i64 start_cycle,
                                         u8 byte_width,
                                         bool use_shared_time,
                                         const std::vector<u64>* shared_times,
                                         std::vector<u8>& out,
                                         WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::NibbleMask));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (transitions.empty()) return;

        u64 prev_rel = 0;
        std::size_t shared_pos = 0;
        const Transition& first = transitions[0];
        append_record_time(out, static_cast<u64>(first.cycle - start_cycle), prev_rel, use_shared_time, shared_times, shared_pos, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, byte_width);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < transitions.size(); t += 2u) {
            const Transition& tr0 = transitions[t];
            const u64 rel0 = static_cast<u64>(tr0.cycle - start_cycle);
            append_record_time(out, rel0, prev_rel, use_shared_time, shared_times, shared_pos, cost);
            const u8 mask0 = changed_byte_mask_for_width(tr0.value, prev, byte_width);

            u8 mask1 = 0;
            if (t + 1u < transitions.size()) {
                std::array<u8, kMaxScalarBytes> after0 = prev;
                for (u8 b = 0; b < byte_width; ++b) after0[b] = tr0.value.bytes[b];
                const Transition& tr1 = transitions[t + 1u];
                const u64 rel1 = static_cast<u64>(tr1.cycle - start_cycle);
                append_record_time(out, rel1, prev_rel, use_shared_time, shared_times, shared_pos, cost);
                mask1 = changed_byte_mask_for_width(tr1.value, after0, byte_width);
            }

            before = out.size();
            detail::append_u8(out, static_cast<u8>((mask0 & 0x0fu) | static_cast<u8>((mask1 & 0x0fu) << 4)));
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);

            before = out.size();
            append_changed_value_bytes_by_mask(tr0.value, byte_width, mask0, prev, out);
            if (t + 1u < transitions.size()) {
                const Transition& tr1 = transitions[t + 1u];
                append_changed_value_bytes_by_mask(tr1.value, byte_width, mask1, prev, out);
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    void append_value_nibble_mask_stride_record(const std::vector<Transition>& transitions,
                                                i64 start_cycle,
                                                u8 byte_width,
                                                u64 stride,
                                                std::vector<u8>& out,
                                                WdatByteBreakdown* cost) const {
        std::size_t before = out.size();
        detail::append_u8(out, static_cast<u8>(ValueRecordCodec::NibbleMaskStride));
        detail::append_varuint(out, static_cast<u64>(transitions.size()));
        if (cost) cost->record_header_bytes += appended_size_since(out, before);
        if (transitions.empty()) return;

        const Transition& first = transitions[0];
        const u64 first_rel = static_cast<u64>(first.cycle - start_cycle);
        append_record_stride_time(out, first_rel, stride, cost);
        before = out.size();
        append_fixed_value_bytes(out, first.value, byte_width);
        if (cost) cost->value_payload_bytes += appended_size_since(out, before);

        std::array<u8, kMaxScalarBytes> prev = first.value.bytes;
        for (std::size_t t = 1; t < transitions.size(); t += 2u) {
            const Transition& tr0 = transitions[t];
            const u8 mask0 = changed_byte_mask_for_width(tr0.value, prev, byte_width);

            u8 mask1 = 0;
            if (t + 1u < transitions.size()) {
                std::array<u8, kMaxScalarBytes> after0 = prev;
                for (u8 b = 0; b < byte_width; ++b) after0[b] = tr0.value.bytes[b];
                const Transition& tr1 = transitions[t + 1u];
                mask1 = changed_byte_mask_for_width(tr1.value, after0, byte_width);
            }

            before = out.size();
            detail::append_u8(out, static_cast<u8>((mask0 & 0x0fu) | static_cast<u8>((mask1 & 0x0fu) << 4)));
            if (cost) cost->value_mask_bytes += appended_size_since(out, before);

            before = out.size();
            append_changed_value_bytes_by_mask(tr0.value, byte_width, mask0, prev, out);
            if (t + 1u < transitions.size()) {
                const Transition& tr1 = transitions[t + 1u];
                append_changed_value_bytes_by_mask(tr1.value, byte_width, mask1, prev, out);
            }
            if (cost) cost->value_payload_bytes += appended_size_since(out, before);
        }
    }

    SignalRecordChoice choose_value_record_codec_and_size(const std::vector<Transition>& transitions,
                                                          i64 start_cycle,
                                                          u8 byte_width,
                                                          ValueType value_type,
                                                          bool use_shared_time,
                                                          const std::vector<u64>* shared_times) const {
        const std::size_t count = transitions.size();
        const std::size_t header_size = 1u + detail::varuint_size(static_cast<u64>(count));
        SignalRecordChoice choice;
        if (count == 0) {
            choice.codec = ValueRecordCodec::FullValues;
            choice.encoded_size = header_size;
            return choice;
        }

        std::size_t time_size = 0;
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;

        bool can_bool = options_.enable_bool_toggle_encoding &&
                        value_type == ValueType::Bool &&
                        byte_width == 1;

        bool can_nibble_mask = options_.enable_value_byte_mask_encoding &&
                               byte_width > 2 &&
                               byte_width <= 4 &&
                               count > 1;

        bool can_byte_mask = options_.enable_value_byte_mask_encoding &&
                             byte_width > 4 &&
                             count > 1;

        bool can_stride = options_.enable_stride_time_record_encoding &&
                          !use_shared_time &&
                          count >= 3u;
        u64 stride = 0;
        u64 prev_abs_rel = 0;

        const std::size_t full_value_bytes = count * static_cast<std::size_t>(byte_width);
        std::size_t byte_mask_value_bytes = can_byte_mask ? static_cast<std::size_t>(byte_width) : 0u;
        std::size_t nibble_mask_value_bytes = can_nibble_mask
            ? (static_cast<std::size_t>(byte_width) + (count - 1u + 1u) / 2u)
            : 0u;
        std::array<u8, kMaxScalarBytes> prev_value = transitions[0].value.bytes;
        std::array<u8, kMaxScalarBytes> prev_nibble_value = transitions[0].value.bytes;

        for (std::size_t t = 0; t < count; ++t) {
            const Transition& tr = transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - start_cycle);
            time_size += record_time_encoded_size(rel, prev_rel, use_shared_time, shared_times, shared_pos);

            if (can_stride) {
                if (t == 0) {
                    prev_abs_rel = rel;
                } else {
                    const u64 delta = rel - prev_abs_rel;
                    if (t == 1) {
                        stride = delta;
                    } else if (delta != stride) {
                        can_stride = false;
                    }
                    prev_abs_rel = rel;
                }
            }

            if (can_bool) {
                if (tr.value.bytes[0] > 1u || (t > 0 && tr.value.bytes[0] == transitions[t - 1].value.bytes[0])) {
                    can_bool = false;
                }
            }

            if (can_nibble_mask && t > 0) {
                for (u8 b = 0; b < byte_width; ++b) {
                    if (tr.value.bytes[b] != prev_nibble_value[b]) ++nibble_mask_value_bytes;
                    prev_nibble_value[b] = tr.value.bytes[b];
                }
                if (nibble_mask_value_bytes >= full_value_bytes) {
                    can_nibble_mask = false;
                }
            }

            if (can_byte_mask && t > 0) {
                byte_mask_value_bytes += 1u;
                for (u8 b = 0; b < byte_width; ++b) {
                    if (tr.value.bytes[b] != prev_value[b]) ++byte_mask_value_bytes;
                    prev_value[b] = tr.value.bytes[b];
                }
                if (byte_mask_value_bytes >= full_value_bytes) {
                    can_byte_mask = false;
                }
            }
        }

        const std::size_t full_size = header_size + time_size + full_value_bytes;
        choice.codec = ValueRecordCodec::FullValues;
        choice.encoded_size = full_size;

        if (can_bool) {
            const std::size_t bool_size = header_size + 1u + time_size;
            if (bool_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::BoolToggle;
                choice.encoded_size = bool_size;
            }
        }

        if (can_nibble_mask) {
            const std::size_t nibble_mask_size = header_size + time_size + nibble_mask_value_bytes;
            if (nibble_mask_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::NibbleMask;
                choice.encoded_size = nibble_mask_size;
            }
        }

        if (can_byte_mask) {
            const std::size_t byte_mask_size = header_size + time_size + byte_mask_value_bytes;
            if (byte_mask_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::ByteMask;
                choice.encoded_size = byte_mask_size;
            }
        }

        if (can_stride) {
            const u64 first_rel = static_cast<u64>(transitions[0].cycle - start_cycle);
            const std::size_t stride_time_size = record_stride_time_size(first_rel, stride);
            const std::size_t full_stride_size = header_size + stride_time_size + full_value_bytes;
            if (full_stride_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::FullValuesStride;
                choice.encoded_size = full_stride_size;
                choice.stride = stride;
            }
            if (can_bool) {
                const std::size_t bool_stride_size = header_size + 1u + stride_time_size;
                if (bool_stride_size < choice.encoded_size) {
                    choice.codec = ValueRecordCodec::BoolToggleStride;
                    choice.encoded_size = bool_stride_size;
                    choice.stride = stride;
                }
            }
            if (can_nibble_mask) {
                const std::size_t nibble_mask_stride_size = header_size + stride_time_size + nibble_mask_value_bytes;
                if (nibble_mask_stride_size < choice.encoded_size) {
                    choice.codec = ValueRecordCodec::NibbleMaskStride;
                    choice.encoded_size = nibble_mask_stride_size;
                    choice.stride = stride;
                }
            }
            if (can_byte_mask) {
                const std::size_t byte_mask_stride_size = header_size + stride_time_size + byte_mask_value_bytes;
                if (byte_mask_stride_size < choice.encoded_size) {
                    choice.codec = ValueRecordCodec::ByteMaskStride;
                    choice.encoded_size = byte_mask_stride_size;
                    choice.stride = stride;
                }
            }
        }
        return choice;
    }

    void append_value_best_record(const std::vector<Transition>& transitions,
                                  i64 start_cycle,
                                  u8 byte_width,
                                  ValueType value_type,
                                  bool use_shared_time,
                                  const std::vector<u64>* shared_times,
                                  std::vector<u8>& out) const {
        const SignalRecordChoice choice = choose_value_record_codec_and_size(transitions, start_cycle, byte_width,
                                                                             value_type, use_shared_time, shared_times);
        detail::reserve_extra(out, choice.encoded_size);
        switch (choice.codec) {
        case ValueRecordCodec::BoolToggle:
            append_value_bool_toggle_record(transitions, start_cycle, use_shared_time, shared_times, out, NULL);
            break;
        case ValueRecordCodec::BoolToggleStride:
            append_value_bool_toggle_stride_record(transitions, start_cycle, choice.stride, out, NULL);
            break;
        case ValueRecordCodec::ByteMask:
            append_value_byte_mask_record(transitions, start_cycle, byte_width, use_shared_time, shared_times, out, NULL);
            break;
        case ValueRecordCodec::ByteMaskStride:
            append_value_byte_mask_stride_record(transitions, start_cycle, byte_width, choice.stride, out, NULL);
            break;
        case ValueRecordCodec::NibbleMask:
            append_value_nibble_mask_record(transitions, start_cycle, byte_width, use_shared_time, shared_times, out, NULL);
            break;
        case ValueRecordCodec::NibbleMaskStride:
            append_value_nibble_mask_stride_record(transitions, start_cycle, byte_width, choice.stride, out, NULL);
            break;
        case ValueRecordCodec::FullValuesStride:
            append_value_full_stride_record(transitions, start_cycle, byte_width, choice.stride, out, NULL);
            break;
        case ValueRecordCodec::FullValues:
        default:
            append_value_full_record(transitions, start_cycle, byte_width, use_shared_time, shared_times, out, NULL);
            break;
        }
    }

    SignalRecordChoice choose_signal_record_codec_and_size(u32 signal_id,
                                                           const BlockJob& job,
                                                           bool use_shared_time,
                                                           const std::vector<u64>* shared_times) const {
        const SignalState& st = signal_states_[signal_id];
        const std::size_t count = st.transitions.size();
        const std::size_t header_size = 1u + detail::varuint_size(static_cast<u64>(count));
        SignalRecordChoice choice;
        if (count == 0) {
            choice.codec = ValueRecordCodec::FullValues;
            choice.encoded_size = header_size;
            return choice;
        }

        std::size_t time_size = 0;
        u64 prev_rel = 0;
        std::size_t shared_pos = 0;

        bool can_bool = options_.enable_bool_toggle_encoding &&
                        st.def.type == ValueType::Bool &&
                        st.byte_width_bytes == 1;

        bool can_nibble_mask = options_.enable_value_byte_mask_encoding &&
                               st.byte_width_bytes > 2 &&
                               st.byte_width_bytes <= 4 &&
                               count > 1;

        bool can_byte_mask = options_.enable_value_byte_mask_encoding &&
                             st.byte_width_bytes > 4 &&
                             count > 1;

        bool can_stride = options_.enable_stride_time_record_encoding &&
                          !use_shared_time &&
                          count >= 3u;
        u64 stride = 0;
        u64 prev_abs_rel = 0;

        const std::size_t full_value_bytes = count * static_cast<std::size_t>(st.byte_width_bytes);
        std::size_t byte_mask_value_bytes = can_byte_mask ? static_cast<std::size_t>(st.byte_width_bytes) : 0u;
        std::size_t nibble_mask_value_bytes = can_nibble_mask
            ? (static_cast<std::size_t>(st.byte_width_bytes) + (count - 1u + 1u) / 2u)
            : 0u;
        std::array<u8, kMaxScalarBytes> prev_value = st.transitions[0].value.bytes;
        std::array<u8, kMaxScalarBytes> prev_nibble_value = st.transitions[0].value.bytes;

        for (std::size_t t = 0; t < count; ++t) {
            const Transition& tr = st.transitions[t];
            const u64 rel = static_cast<u64>(tr.cycle - job.start_cycle);
            time_size += record_time_encoded_size(rel, prev_rel, use_shared_time, shared_times, shared_pos);

            if (can_stride) {
                if (t == 0) {
                    prev_abs_rel = rel;
                } else {
                    const u64 delta = rel - prev_abs_rel;
                    if (t == 1) {
                        stride = delta;
                    } else if (delta != stride) {
                        can_stride = false;
                    }
                    prev_abs_rel = rel;
                }
            }

            if (can_bool) {
                if (tr.value.bytes[0] > 1u || (t > 0 && tr.value.bytes[0] == st.transitions[t - 1].value.bytes[0])) {
                    can_bool = false;
                }
            }

            if (can_nibble_mask && t > 0) {
                for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                    if (tr.value.bytes[b] != prev_nibble_value[b]) ++nibble_mask_value_bytes;
                    prev_nibble_value[b] = tr.value.bytes[b];
                }
                // Nibble-mask has the same header and time stream as full-value.
                // Once its value stream is no smaller than full-value, it cannot
                // win; stop paying byte-compare cost for the remaining transitions.
                if (nibble_mask_value_bytes >= full_value_bytes) {
                    can_nibble_mask = false;
                }
            }

            if (can_byte_mask && t > 0) {
                byte_mask_value_bytes += 1u; // per-transition changed-byte mask
                for (u8 b = 0; b < st.byte_width_bytes; ++b) {
                    if (tr.value.bytes[b] != prev_value[b]) ++byte_mask_value_bytes;
                    prev_value[b] = tr.value.bytes[b];
                }
                // Byte-mask has the same header and time stream as full-value.
                // Once its value stream is no smaller than full-value, it cannot
                // win; stop paying byte-compare cost for the remaining transitions.
                if (byte_mask_value_bytes >= full_value_bytes) {
                    can_byte_mask = false;
                }
            }
        }

        const std::size_t full_size = header_size + time_size + full_value_bytes;
        choice.codec = ValueRecordCodec::FullValues;
        choice.encoded_size = full_size;

        if (can_bool) {
            const std::size_t bool_size = header_size + 1u + time_size;
            if (bool_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::BoolToggle;
                choice.encoded_size = bool_size;
            }
        }

        if (can_nibble_mask) {
            const std::size_t nibble_mask_size = header_size + time_size + nibble_mask_value_bytes;
            if (nibble_mask_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::NibbleMask;
                choice.encoded_size = nibble_mask_size;
            }
        }

        if (can_byte_mask) {
            const std::size_t byte_mask_size = header_size + time_size + byte_mask_value_bytes;
            if (byte_mask_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::ByteMask;
                choice.encoded_size = byte_mask_size;
            }
        }

        if (can_stride) {
            const u64 first_rel = static_cast<u64>(st.transitions[0].cycle - job.start_cycle);
            const std::size_t stride_time_size = record_stride_time_size(first_rel, stride);
            const std::size_t full_stride_size = header_size + stride_time_size + full_value_bytes;
            if (full_stride_size < choice.encoded_size) {
                choice.codec = ValueRecordCodec::FullValuesStride;
                choice.encoded_size = full_stride_size;
                choice.stride = stride;
            }
            if (can_bool) {
                const std::size_t bool_stride_size = header_size + 1u + stride_time_size;
                if (bool_stride_size < choice.encoded_size) {
                    choice.codec = ValueRecordCodec::BoolToggleStride;
                    choice.encoded_size = bool_stride_size;
                    choice.stride = stride;
                }
            }
            if (can_nibble_mask) {
                const std::size_t nibble_mask_stride_size = header_size + stride_time_size + nibble_mask_value_bytes;
                if (nibble_mask_stride_size < choice.encoded_size) {
                    choice.codec = ValueRecordCodec::NibbleMaskStride;
                    choice.encoded_size = nibble_mask_stride_size;
                    choice.stride = stride;
                }
            }
            if (can_byte_mask) {
                const std::size_t byte_mask_stride_size = header_size + stride_time_size + byte_mask_value_bytes;
                if (byte_mask_stride_size < choice.encoded_size) {
                    choice.codec = ValueRecordCodec::ByteMaskStride;
                    choice.encoded_size = byte_mask_stride_size;
                    choice.stride = stride;
                }
            }
        }
        return choice;
    }

    void append_signal_best_record(u32 signal_id,
                                   const BlockJob& job,
                                   bool use_shared_time,
                                   const std::vector<u64>* shared_times,
                                   std::vector<u8>& out,
                                   TileStats& stats) const {
        const SignalRecordChoice choice = choose_signal_record_codec_and_size(signal_id, job, use_shared_time, shared_times);

        detail::reserve_extra(out, choice.encoded_size);
        WdatByteBreakdown chosen_cost;
        switch (choice.codec) {
        case ValueRecordCodec::BoolToggle:
            append_signal_bool_toggle_record(signal_id, job, use_shared_time, shared_times, out, &chosen_cost);
            ++stats.bool_toggle_records;
            break;
        case ValueRecordCodec::BoolToggleStride:
            append_signal_bool_toggle_stride_record(signal_id, job, choice.stride, out, &chosen_cost);
            ++stats.bool_toggle_records;
            break;
        case ValueRecordCodec::ByteMask:
            append_signal_byte_mask_record(signal_id, job, use_shared_time, shared_times, out, &chosen_cost);
            ++stats.byte_mask_records;
            break;
        case ValueRecordCodec::ByteMaskStride:
            append_signal_byte_mask_stride_record(signal_id, job, choice.stride, out, &chosen_cost);
            ++stats.byte_mask_records;
            break;
        case ValueRecordCodec::NibbleMask:
            append_signal_nibble_mask_record(signal_id, job, use_shared_time, shared_times, out, &chosen_cost);
            ++stats.nibble_mask_records;
            break;
        case ValueRecordCodec::NibbleMaskStride:
            append_signal_nibble_mask_stride_record(signal_id, job, choice.stride, out, &chosen_cost);
            ++stats.nibble_mask_records;
            break;
        case ValueRecordCodec::FullValuesStride:
            append_signal_full_stride_record(signal_id, job, choice.stride, out, &chosen_cost);
            ++stats.full_value_records;
            break;
        case ValueRecordCodec::FullValues:
        default:
            append_signal_full_record(signal_id, job, use_shared_time, shared_times, out, &chosen_cost);
            ++stats.full_value_records;
            break;
        }
        stats.cost.add(chosen_cost);
    }

    void append_offset_table(std::vector<u8>& out, const std::vector<u64>& offsets, WdatByteBreakdown* cost) const {
        const std::size_t before = out.size();
        detail::append_varuint(out, static_cast<u64>(offsets.size()));
        u64 prev = 0;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            detail::append_varuint(out, offsets[i] - prev);
            prev = offsets[i];
        }
        if (cost) cost->dense_offset_table_bytes += appended_size_since(out, before);
    }

    void collect_shared_times(const BlockJob& job, std::vector<u64>& times) const {
        times.clear();
        for (u32 local = 0; local < job.signal_count; ++local) {
            const u32 signal_id = job.first_signal_id + local;
            if (signal_id >= signal_states_.size()) continue;
            const SignalState& st = signal_states_[signal_id];
            if (!st.valid || st.is_periodic_clock) continue;
            for (std::size_t t = 0; t < st.transitions.size(); ++t) {
                times.push_back(static_cast<u64>(st.transitions[t].cycle - job.start_cycle));
            }
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
    }

    void append_shared_time_table(std::vector<u8>& out, const std::vector<u64>& shared_times, WdatByteBreakdown* cost) const {
        const std::size_t before = out.size();
        detail::append_varuint(out, static_cast<u64>(shared_times.size()));
        u64 prev_time = 0;
        for (std::size_t i = 0; i < shared_times.size(); ++i) {
            detail::append_varuint(out, shared_times[i] - prev_time);
            prev_time = shared_times[i];
        }
        if (cost) cost->shared_time_table_bytes += appended_size_since(out, before);
    }

    struct ActiveRecordBuild {
        u32 local = 0;
        u64 record_size = 0;
    };

    static std::size_t estimate_wdat_raw_header_size(const BlockJob& job, u64 flags) {
        std::size_t n = 0;
        n += detail::varuint_size(job.block_id);
        n += 16u; // start/end i64 cycles
        n += detail::varuint_size(flags | kWdatSignalChunkTile);
        n += detail::varuint_size(job.signal_chunk_id);
        n += detail::varuint_size(job.first_signal_id);
        n += detail::varuint_size(job.signal_count);
        return n;
    }

    static std::size_t estimate_shared_time_table_size(const std::vector<u64>& shared_times) {
        std::size_t n = detail::varuint_size(static_cast<u64>(shared_times.size()));
        u64 prev_time = 0;
        for (std::size_t i = 0; i < shared_times.size(); ++i) {
            n += detail::varuint_size(shared_times[i] - prev_time);
            prev_time = shared_times[i];
        }
        return n;
    }

    std::size_t estimate_block_payload_dense_size(const BlockJob& job,
                                                  bool use_delta_time,
                                                  bool use_shared_time,
                                                  const std::vector<u64>* shared_times) const {
        u64 flags = kWdatFixedValueWidth | kWdatPerRecordValueCodec;
        if (use_delta_time) flags |= kWdatDeltaTimes;
        if (use_shared_time) flags |= kWdatSharedTimeTable;

        std::size_t total = estimate_wdat_raw_header_size(job, flags);
        if (use_shared_time) total += estimate_shared_time_table_size(*shared_times);

        std::size_t offset_table_bytes = detail::varuint_size(static_cast<u64>(static_cast<std::size_t>(job.signal_count) + 1u));
        u64 prev_offset = 0;
        u64 blob_size = 0;
        for (u32 local = 0; local < job.signal_count; ++local) {
            offset_table_bytes += detail::varuint_size(blob_size - prev_offset);
            prev_offset = blob_size;
            const u32 signal_id = job.first_signal_id + local;
            if (signal_id < signal_states_.size() && signal_states_[signal_id].valid &&
                !signal_states_[signal_id].is_periodic_clock &&
                !signal_states_[signal_id].transitions.empty()) {
                blob_size += static_cast<u64>(choose_signal_record_codec_and_size(signal_id, job, use_shared_time, shared_times).encoded_size);
            }
        }
        offset_table_bytes += detail::varuint_size(blob_size - prev_offset);
        total += offset_table_bytes;
        total += detail::varuint_size(blob_size);
        total += static_cast<std::size_t>(blob_size);
        return total;
    }

    std::size_t estimate_block_payload_sparse_size(const BlockJob& job,
                                                   bool use_delta_time,
                                                   bool use_shared_time,
                                                   const std::vector<u64>* shared_times) const {
        u64 flags = kWdatFixedValueWidth | kWdatSparseSignalRecords | kWdatPerRecordValueCodec;
        if (use_delta_time) flags |= kWdatDeltaTimes;
        if (use_shared_time) flags |= kWdatSharedTimeTable;

        std::size_t total = estimate_wdat_raw_header_size(job, flags);
        if (use_shared_time) total += estimate_shared_time_table_size(*shared_times);

        u64 active_count = 0;
        u64 prev_local = 0;
        u64 blob_size = 0;
        std::size_t active_table_bytes = 0;
        for (u32 local = 0; local < job.signal_count; ++local) {
            const u32 signal_id = job.first_signal_id + local;
            if (signal_id >= signal_states_.size()) continue;
            const SignalState& st = signal_states_[signal_id];
            if (!st.valid || st.is_periodic_clock || st.transitions.empty()) continue;
            const std::size_t record_size = choose_signal_record_codec_and_size(signal_id, job, use_shared_time, shared_times).encoded_size;
            active_table_bytes += detail::varuint_size(static_cast<u64>(local) - prev_local);
            active_table_bytes += detail::varuint_size(static_cast<u64>(record_size));
            prev_local = local;
            blob_size += static_cast<u64>(record_size);
            ++active_count;
        }
        total += detail::varuint_size(active_count);
        total += active_table_bytes;
        total += detail::varuint_size(blob_size);
        total += static_cast<std::size_t>(blob_size);
        return total;
    }

    void build_block_payload_dense(const BlockJob& job,
                                   bool use_delta_time,
                                   bool use_shared_time,
                                   const std::vector<u64>* shared_times,
                                   std::vector<u8>& out,
                                   TileStats& stats) const {
        std::vector<u8> blob;
        std::vector<u64> offsets;
        offsets.reserve(static_cast<std::size_t>(job.signal_count) + 1u);
        blob.reserve(1024);
        stats = TileStats();
        stats.tile_signal_count = job.signal_count;

        for (u32 local = 0; local < job.signal_count; ++local) {
            const u32 signal_id = job.first_signal_id + local;
            offsets.push_back(static_cast<u64>(blob.size()));
            if (signal_id < signal_states_.size() && signal_states_[signal_id].valid &&
                !signal_states_[signal_id].is_periodic_clock &&
                !signal_states_[signal_id].transitions.empty()) {
                ++stats.active_signal_count;
                append_signal_best_record(signal_id, job, use_shared_time, shared_times, blob, stats);
            }
        }
        offsets.push_back(static_cast<u64>(blob.size()));

        u64 flags = kWdatFixedValueWidth | kWdatPerRecordValueCodec;
        if (use_delta_time) flags |= kWdatDeltaTimes;
        if (use_shared_time) flags |= kWdatSharedTimeTable;

        out.clear();
        out.reserve(64u + offsets.size() * 5u + blob.size() + (use_shared_time ? shared_times->size() * 3u : 0u));
        append_wdat_raw_header(out, job, flags, &stats.cost);
        if (use_shared_time) append_shared_time_table(out, *shared_times, &stats.cost);
        append_offset_table(out, offsets, &stats.cost);
        {
            const std::size_t before = out.size();
            detail::append_varuint(out, static_cast<u64>(blob.size()));
            stats.cost.tile_index_bytes += appended_size_since(out, before);
        }
        detail::append_vector_bytes(out, blob);
        stats.raw_flags = flags | kWdatSignalChunkTile;
        stats.raw_payload_size = static_cast<u64>(out.size());
    }

    void build_block_payload_sparse(const BlockJob& job,
                                    bool use_delta_time,
                                    bool use_shared_time,
                                    const std::vector<u64>* shared_times,
                                    std::vector<u8>& out,
                                    TileStats& stats) const {
        std::vector<ActiveRecordBuild> active;
        active.reserve(std::min<u32>(job.signal_count, 1024u));
        std::vector<u8> blob;
        blob.reserve(4096);
        stats = TileStats();
        stats.tile_signal_count = job.signal_count;

        for (u32 local = 0; local < job.signal_count; ++local) {
            const u32 signal_id = job.first_signal_id + local;
            if (signal_id >= signal_states_.size()) continue;
            const SignalState& st = signal_states_[signal_id];
            if (!st.valid || st.is_periodic_clock || st.transitions.empty()) continue;
            const std::size_t before_record = blob.size();
            append_signal_best_record(signal_id, job, use_shared_time, shared_times, blob, stats);
            ActiveRecordBuild rec;
            rec.local = local;
            rec.record_size = static_cast<u64>(blob.size() - before_record);
            active.push_back(rec);
        }
        stats.active_signal_count = static_cast<u64>(active.size());

        u64 flags = kWdatFixedValueWidth | kWdatSparseSignalRecords | kWdatPerRecordValueCodec;
        if (use_delta_time) flags |= kWdatDeltaTimes;
        if (use_shared_time) flags |= kWdatSharedTimeTable;

        out.clear();
        out.reserve(64u + active.size() * 8u + blob.size() + (use_shared_time ? shared_times->size() * 3u : 0u));
        append_wdat_raw_header(out, job, flags, &stats.cost);
        if (use_shared_time) append_shared_time_table(out, *shared_times, &stats.cost);
        std::size_t before = out.size();
        detail::append_varuint(out, static_cast<u64>(active.size()));
        stats.cost.sparse_active_signal_bytes += appended_size_since(out, before);
        u64 prev_local = 0;
        for (std::size_t i = 0; i < active.size(); ++i) {
            before = out.size();
            detail::append_varuint(out, static_cast<u64>(active[i].local) - prev_local);
            stats.cost.sparse_active_signal_bytes += appended_size_since(out, before);
            before = out.size();
            detail::append_varuint(out, active[i].record_size);
            stats.cost.sparse_record_size_bytes += appended_size_since(out, before);
            prev_local = active[i].local;
        }
        before = out.size();
        detail::append_varuint(out, static_cast<u64>(blob.size()));
        stats.cost.tile_index_bytes += appended_size_since(out, before);
        detail::append_vector_bytes(out, blob);
        stats.raw_flags = flags | kWdatSignalChunkTile;
        stats.raw_payload_size = static_cast<u64>(out.size());
    }

    void build_block_payload_absolute_fixed(const BlockJob& job, std::vector<u8>& out, TileStats& stats) const {
        build_block_payload_dense(job, false, false, NULL, out, stats);
    }

    enum class PayloadBuildMode : u8 {
        DenseDefault = 0,
        SparseDefault = 1,
        DenseShared = 2,
        SparseShared = 3
    };

    void build_block_payload(BlockJob& job) const {
        const std::vector<u64>* shared_times = shared_times_for_job(job);
        const bool default_delta = options_.enable_delta_time_encoding;

        PayloadBuildMode best_mode = PayloadBuildMode::DenseDefault;
        std::size_t best_size = estimate_block_payload_dense_size(job, default_delta, false, NULL);

        if (options_.enable_sparse_signal_records) {
            const std::size_t cand_size = estimate_block_payload_sparse_size(job, default_delta, false, NULL);
            if (cand_size < best_size) {
                best_size = cand_size;
                best_mode = PayloadBuildMode::SparseDefault;
            }
        }

        if (options_.enable_shared_time_table && shared_times != NULL) {
            const std::size_t dense_shared_size = estimate_block_payload_dense_size(job, false, true, shared_times);
            if (dense_shared_size < best_size) {
                best_size = dense_shared_size;
                best_mode = PayloadBuildMode::DenseShared;
            }
            if (options_.enable_sparse_signal_records) {
                const std::size_t sparse_shared_size = estimate_block_payload_sparse_size(job, false, true, shared_times);
                if (sparse_shared_size < best_size) {
                    best_size = sparse_shared_size;
                    best_mode = PayloadBuildMode::SparseShared;
                }
            }
        }

        std::vector<u8> best;
        TileStats best_stats;
        best.reserve(best_size);
        switch (best_mode) {
        case PayloadBuildMode::SparseDefault:
            build_block_payload_sparse(job, default_delta, false, NULL, best, best_stats);
            break;
        case PayloadBuildMode::DenseShared:
            build_block_payload_dense(job, false, true, shared_times, best, best_stats);
            break;
        case PayloadBuildMode::SparseShared:
            build_block_payload_sparse(job, false, true, shared_times, best, best_stats);
            break;
        case PayloadBuildMode::DenseDefault:
        default:
            build_block_payload_dense(job, default_delta, false, NULL, best, best_stats);
            break;
        }

        job.raw_payload.swap(best);
        job.stats = best_stats;
    }

    EncodedBlock encode_block_job(const BlockJob& job) const {
        EncodedBlock e;
        e.block_id = job.block_id;
        e.start_cycle = job.start_cycle;
        e.end_cycle = job.end_cycle;
        e.signal_chunk_id = job.signal_chunk_id;
        e.first_signal_id = job.first_signal_id;
        e.signal_count = job.signal_count;
        e.raw_size = static_cast<u64>(job.raw_payload.size());
        e.compression = Compression::None;
        e.stats = job.stats;

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
        detail::append_varuint(payload, e.signal_chunk_id);
        detail::append_varuint(payload, e.first_signal_id);
        detail::append_varuint(payload, e.signal_count);
        detail::append_u8(payload, static_cast<u8>(e.compression));
        detail::append_varuint(payload, e.raw_size);
        detail::append_varuint(payload, static_cast<u64>(e.payload.size()));
        detail::append_vector_bytes(payload, e.payload);
        if (!write_section("WDAT", payload, error)) return false;
        const u64 end = static_cast<u64>(out_.tellp());

        BlockIndexRecord idx;
        idx.block_id = e.block_id;
        idx.start_cycle = e.start_cycle;
        idx.end_cycle = e.end_cycle;
        idx.signal_chunk_id = e.signal_chunk_id;
        idx.first_signal_id = e.first_signal_id;
        idx.signal_count = e.signal_count;
        idx.file_offset = offset;
        idx.file_size = end - offset;
        idx.raw_size = e.raw_size;
        idx.compression = e.compression;
        block_index_.push_back(idx);
        record_encoded_block_stats(e);
        return true;
    }

    void record_encoded_block_stats(const EncodedBlock& e) {
        ++stats_.wdat_blocks;
        stats_.wdat_raw_bytes += e.raw_size;
        stats_.wdat_stored_payload_bytes += static_cast<u64>(e.payload.size());
        if (e.stats.raw_flags & kWdatSparseSignalRecords) ++stats_.sparse_tiles;
        else ++stats_.dense_tiles;
        if (e.stats.raw_flags & kWdatSharedTimeTable) ++stats_.shared_time_tiles;
        else if (e.stats.raw_flags & kWdatDeltaTimes) ++stats_.delta_time_tiles;
        else ++stats_.absolute_time_tiles;
        stats_.active_signal_records += e.stats.active_signal_count;
        stats_.tile_signal_slots += e.stats.tile_signal_count;
        stats_.full_value_records += e.stats.full_value_records;
        stats_.bool_toggle_records += e.stats.bool_toggle_records;
        stats_.byte_mask_records += e.stats.byte_mask_records;
        stats_.nibble_mask_records += e.stats.nibble_mask_records;
        stats_.wdat_cost.add(e.stats.cost);
    }

    void writer_diag_log_(const char* tag, const CycleSubmission* submission = NULL, i64 block_end_cycle = -1, std::size_t extra_count = 0, u64 extra_bytes = 0) {
        if (writer_diag_log_lines_ >= 128u || detail::backlog_log_disabled()) return;
        ++writer_diag_log_lines_;
        const long long cycle = submission ? static_cast<long long>(submission->cycle) : static_cast<long long>(current_cycle_);
        const std::size_t updates = submission ? submission->updates.size() : 0;
        detail::backlog_log(
            "%s writer=%p opened=%d submit_count=%llu cycle=%lld updates=%zu current_cycle=%lld block_start=%llu block_end=%lld span=%llu pending=%d next_block=%llu max_signal=%u chunks=%zu bytes=%llu block_index=%zu pipeline=%d extra=%zu err=%s",
            tag,
            static_cast<void*>(this),
            opened_ ? 1 : 0,
            static_cast<unsigned long long>(writer_submit_count_),
            cycle,
            updates,
            static_cast<long long>(current_cycle_),
            static_cast<unsigned long long>(current_block_start_),
            static_cast<long long>(block_end_cycle),
            static_cast<unsigned long long>(options_.target_block_span),
            have_pending_content_ ? 1 : 0,
            static_cast<unsigned long long>(next_block_id_),
            max_signal_id_,
            extra_count,
            static_cast<unsigned long long>(extra_bytes),
            block_index_.size(),
            (options_.enable_block_pipeline && pipeline_.running) ? 1 : 0,
            extra_count,
            pipeline_.error.empty() ? "" : pipeline_.error.c_str());
    }

    void pipeline_diag_log_locked_(const char* tag, std::size_t pending_count = 0) {
        if (pipeline_.diag_log_lines >= 128u || detail::backlog_log_disabled()) return;
        ++pipeline_.diag_log_lines;
        detail::backlog_log(
            "%s writer=%p running=%d stop=%d compression_done=%d jobs=%zu results=%zu pending=%zu next_file_block=%llu enq=%llu started=%llu finished=%llu pushed=%llu written=%llu comp_workers_started=%llu comp_threads=%zu qlimit=%zu err=%s",
            tag,
            static_cast<void*>(this),
            pipeline_.running ? 1 : 0,
            pipeline_.stop_requested ? 1 : 0,
            pipeline_.compression_done ? 1 : 0,
            pipeline_.jobs.size(),
            pipeline_.results.size(),
            pending_count,
            static_cast<unsigned long long>(pipeline_.next_file_block_id),
            static_cast<unsigned long long>(pipeline_.jobs_enqueued),
            static_cast<unsigned long long>(pipeline_.jobs_started),
            static_cast<unsigned long long>(pipeline_.jobs_finished),
            static_cast<unsigned long long>(pipeline_.results_pushed),
            static_cast<unsigned long long>(pipeline_.results_written),
            static_cast<unsigned long long>(pipeline_.compression_workers_started),
            pipeline_.compression_threads.size(),
            options_.block_pipeline_queue_limit,
            pipeline_.error.empty() ? "" : pipeline_.error.c_str());
    }

    void maybe_log_pipeline_jobs_locked_(const char* tag) {
        if (pipeline_.diag_log_lines >= 128u || detail::backlog_log_disabled()) return;
        const std::size_t sz = pipeline_.jobs.size();
        if (sz >= pipeline_.next_jobs_log_size) {
            while (pipeline_.next_jobs_log_size <= sz && pipeline_.next_jobs_log_size < (std::numeric_limits<std::size_t>::max() / 2)) {
                pipeline_.next_jobs_log_size *= 2;
            }
            pipeline_diag_log_locked_(tag);
        }
    }

    void maybe_log_pipeline_results_locked_(const char* tag, std::size_t pending_count = 0) {
        if (pipeline_.diag_log_lines >= 128u || detail::backlog_log_disabled()) return;
        const std::size_t sz = pipeline_.results.size();
        if (sz >= pipeline_.next_results_log_size || pending_count >= pipeline_.next_results_log_size) {
            while ((pipeline_.next_results_log_size <= sz || pipeline_.next_results_log_size <= pending_count) &&
                   pipeline_.next_results_log_size < (std::numeric_limits<std::size_t>::max() / 2)) {
                pipeline_.next_results_log_size *= 2;
            }
            pipeline_diag_log_locked_(tag, pending_count);
        }
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
        {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            pipeline_diag_log_locked_("pipeline-start");
        }
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
        ++pipeline_.jobs_enqueued;
        maybe_log_pipeline_jobs_locked_("pipeline-job-enqueue");
        pipeline_.cv_jobs.notify_one();
        return true;
    }

    void compression_worker_loop() {
        {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            ++pipeline_.compression_workers_started;
            if (pipeline_.compression_workers_started <= 4u) pipeline_diag_log_locked_("pipeline-compression-worker-start");
        }
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
                ++pipeline_.jobs_started;
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
                ++pipeline_.jobs_finished;
                ++pipeline_.results_pushed;
                maybe_log_pipeline_results_locked_("pipeline-result-ready");
                pipeline_.cv_results.notify_one();
            }
        }
    }

    void file_writer_loop() {
        std::map<u64, EncodedBlock> pending;
        {
            std::lock_guard<std::mutex> lock(pipeline_.mutex);
            pipeline_.file_writer_started = true;
            pipeline_diag_log_locked_("pipeline-file-writer-start");
        }
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
            if (have) {
                pending.emplace(r.block_id, std::move(r));
                std::lock_guard<std::mutex> lock(pipeline_.mutex);
                maybe_log_pipeline_results_locked_("pipeline-file-pending", pending.size());
            }
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
                {
                    std::lock_guard<std::mutex> lock(pipeline_.mutex);
                    ++pipeline_.results_written;
                }
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
            {
                std::lock_guard<std::mutex> lock(pipeline_.mutex);
                ++pipeline_.results_written;
            }
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

    static double pct(u64 part, u64 total) {
        if (total == 0) return 0.0;
        return 100.0 * static_cast<double>(part) / static_cast<double>(total);
    }

    bool write_stats_log(std::string& error) {
        error.clear();
        const std::streampos pos = out_.tellp();
        if (pos >= std::streampos(0)) stats_.total_file_bytes = static_cast<u64>(pos);
        const std::string log_path = options_.stats_log_path.empty() ? (output_path_ + ".log") : options_.stats_log_path;
        std::ofstream log(log_path.c_str(), std::ios::out | std::ios::trunc);
        if (!log) {
            error = "failed to open WVZ4 stats log: " + log_path;
            return false;
        }
        log << "WVZ4 compression report\n";
        log << "file: " << output_path_ << "\n";
        log << "format_version: " << kFormatVersion << "\n";
        log << "total_file_bytes: " << stats_.total_file_bytes << "\n";
        log << "\n[sections]\n";
        log << std::fixed << std::setprecision(3);
        log << "FILE_HEADER bytes=" << stats_.file_header_bytes
            << " percent=" << pct(stats_.file_header_bytes, stats_.total_file_bytes) << "\n";
        for (std::map<std::string, u64>::const_iterator it = stats_.section_bytes_by_tag.begin();
             it != stats_.section_bytes_by_tag.end(); ++it) {
            log << it->first << " bytes=" << it->second
                << " percent=" << pct(it->second, stats_.total_file_bytes) << "\n";
        }
        log << "layout_sections bytes=" << stats_.layout_section_bytes
            << " percent=" << pct(stats_.layout_section_bytes, stats_.total_file_bytes) << "\n";
        log << "wdat_sections bytes=" << stats_.wdat_section_bytes
            << " percent=" << pct(stats_.wdat_section_bytes, stats_.total_file_bytes) << "\n";
        log << "foot_section bytes=" << stats_.foot_section_bytes
            << " percent=" << pct(stats_.foot_section_bytes, stats_.total_file_bytes) << "\n";

        log << "\n[wdat]\n";
        log << "blocks=" << stats_.wdat_blocks << "\n";
        log << "raw_payload_bytes=" << stats_.wdat_raw_bytes << "\n";
        log << "stored_payload_bytes=" << stats_.wdat_stored_payload_bytes << "\n";
        log << "outer_payload_compression_ratio="
            << (stats_.wdat_stored_payload_bytes == 0 ? 0.0 : static_cast<double>(stats_.wdat_raw_bytes) / static_cast<double>(stats_.wdat_stored_payload_bytes)) << "\n";
        log << "dense_tiles=" << stats_.dense_tiles
            << " percent=" << pct(stats_.dense_tiles, stats_.wdat_blocks) << "\n";
        log << "sparse_tiles=" << stats_.sparse_tiles
            << " percent=" << pct(stats_.sparse_tiles, stats_.wdat_blocks) << "\n";
        log << "delta_time_tiles=" << stats_.delta_time_tiles
            << " percent=" << pct(stats_.delta_time_tiles, stats_.wdat_blocks) << "\n";
        log << "shared_time_tiles=" << stats_.shared_time_tiles
            << " percent=" << pct(stats_.shared_time_tiles, stats_.wdat_blocks) << "\n";
        log << "absolute_time_tiles=" << stats_.absolute_time_tiles
            << " percent=" << pct(stats_.absolute_time_tiles, stats_.wdat_blocks) << "\n";
        log << "active_signal_records=" << stats_.active_signal_records << "\n";
        log << "tile_signal_slots=" << stats_.tile_signal_slots << "\n";
        log << "active_signal_slot_percent=" << pct(stats_.active_signal_records, stats_.tile_signal_slots) << "\n";

        const WdatByteBreakdown& c = stats_.wdat_cost;
        const u64 accounted = c.accounted_bytes();
        const u64 unaccounted = (stats_.wdat_raw_bytes > accounted) ? (stats_.wdat_raw_bytes - accounted) : 0;
        log << "\n[wdat_cost_breakdown_raw]\n";
        log << "cycle_id_bytes=" << c.cycle_id_bytes()
            << " percent=" << pct(c.cycle_id_bytes(), stats_.wdat_raw_bytes) << "\n";
        log << "signal_locator_bytes=" << c.signal_locator_bytes()
            << " percent=" << pct(c.signal_locator_bytes(), stats_.wdat_raw_bytes) << "\n";
        log << "value_payload_bytes=" << c.value_payload_bytes
            << " percent=" << pct(c.value_payload_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "value_mask_bytes=" << c.value_mask_bytes
            << " percent=" << pct(c.value_mask_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "record_header_bytes=" << c.record_header_bytes
            << " percent=" << pct(c.record_header_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "tile_header_bytes=" << c.tile_header_bytes
            << " percent=" << pct(c.tile_header_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "tile_index_bytes=" << (c.tile_index_bytes + c.sparse_record_size_bytes)
            << " percent=" << pct(c.tile_index_bytes + c.sparse_record_size_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "accounted_bytes=" << accounted
            << " percent=" << pct(accounted, stats_.wdat_raw_bytes) << "\n";
        log << "unaccounted_bytes=" << unaccounted
            << " percent=" << pct(unaccounted, stats_.wdat_raw_bytes) << "\n";

        log << "\n[wdat_cost_cycle_detail]\n";
        log << "tile_cycle_header_bytes=" << c.tile_cycle_header_bytes
            << " percent=" << pct(c.tile_cycle_header_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "cycle_delta_bytes=" << c.cycle_delta_bytes
            << " percent=" << pct(c.cycle_delta_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "shared_time_table_bytes=" << c.shared_time_table_bytes
            << " percent=" << pct(c.shared_time_table_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "shared_time_index_bytes=" << c.shared_time_index_bytes
            << " percent=" << pct(c.shared_time_index_bytes, stats_.wdat_raw_bytes) << "\n";

        log << "\n[wdat_cost_signal_detail]\n";
        log << "tile_signal_header_bytes=" << c.tile_signal_header_bytes
            << " percent=" << pct(c.tile_signal_header_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "dense_offset_table_bytes=" << c.dense_offset_table_bytes
            << " percent=" << pct(c.dense_offset_table_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "sparse_active_signal_bytes=" << c.sparse_active_signal_bytes
            << " percent=" << pct(c.sparse_active_signal_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "sparse_record_size_bytes=" << c.sparse_record_size_bytes
            << " percent=" << pct(c.sparse_record_size_bytes, stats_.wdat_raw_bytes) << "\n";

        log << "\n[wdat_cost_value_detail]\n";
        log << "value_payload_bytes=" << c.value_payload_bytes
            << " percent=" << pct(c.value_payload_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "value_mask_bytes=" << c.value_mask_bytes
            << " percent=" << pct(c.value_mask_bytes, stats_.wdat_raw_bytes) << "\n";
        log << "record_header_bytes=" << c.record_header_bytes
            << " percent=" << pct(c.record_header_bytes, stats_.wdat_raw_bytes) << "\n";

        log << "\n[wdat_cost_stored_est_by_raw_ratio]\n";
        log << "note=stored estimates are proportional to raw category bytes; zstd does not expose exact category attribution\n";
        log << "cycle_id_bytes_est=" << static_cast<u64>(static_cast<long double>(stats_.wdat_stored_payload_bytes) * c.cycle_id_bytes() / (stats_.wdat_raw_bytes ? stats_.wdat_raw_bytes : 1)) << "\n";
        log << "signal_locator_bytes_est=" << static_cast<u64>(static_cast<long double>(stats_.wdat_stored_payload_bytes) * c.signal_locator_bytes() / (stats_.wdat_raw_bytes ? stats_.wdat_raw_bytes : 1)) << "\n";
        log << "value_payload_bytes_est=" << static_cast<u64>(static_cast<long double>(stats_.wdat_stored_payload_bytes) * c.value_payload_bytes / (stats_.wdat_raw_bytes ? stats_.wdat_raw_bytes : 1)) << "\n";
        log << "value_mask_bytes_est=" << static_cast<u64>(static_cast<long double>(stats_.wdat_stored_payload_bytes) * c.value_mask_bytes / (stats_.wdat_raw_bytes ? stats_.wdat_raw_bytes : 1)) << "\n";

        const u64 value_records = stats_.full_value_records + stats_.bool_toggle_records + stats_.byte_mask_records + stats_.nibble_mask_records;
        log << "\n[value_codecs]\n";
        log << "full_value_records=" << stats_.full_value_records
            << " percent=" << pct(stats_.full_value_records, value_records) << "\n";
        log << "bool_toggle_records=" << stats_.bool_toggle_records
            << " percent=" << pct(stats_.bool_toggle_records, value_records) << "\n";
        log << "byte_mask_records=" << stats_.byte_mask_records
            << " percent=" << pct(stats_.byte_mask_records, value_records) << "\n";
        log << "nibble_mask_records=" << stats_.nibble_mask_records
            << " percent=" << pct(stats_.nibble_mask_records, value_records) << "\n";

        log << "\n[clock]\n";
        log << "clock_count=" << stats_.clock_count << "\n";
        log << "clock_section_payload_bytes=" << stats_.clock_section_payload_bytes << "\n";
        log << "clock_storage=CLKD(initial_value + period_ticks), no ordinary WDAT transitions\n";
        return static_cast<bool>(log);
    }

    std::vector<u8> build_lod_transition_payload(const std::vector<Transition>& transitions,
                                                 std::size_t begin,
                                                 std::size_t end,
                                                 const LodStorageState& storage,
                                                 u64& record_count) const {
        std::vector<u8> out;
        record_count = 0;
        if (begin >= end || begin >= transitions.size()) return out;
        end = (std::min)(end, transitions.size());
        std::vector<Transition> slice;
        slice.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) slice.push_back(transitions[i]);
        record_count = static_cast<u64>(slice.size());
        append_value_best_record(slice, 0, storage.byte_width,
                                 storage.value_type, false, NULL, out);
        return out;
    }

    static std::vector<LodSelectedLevel> select_lod_levels_to_store(const LodStorageState& storage) {
        std::vector<LodSelectedLevel> selected;
        if (storage.raw_transition_count < kLodMinRawTransitionsForTable) return selected;

        u64 previous_count = storage.raw_transition_count;
        for (std::size_t level = 0; level < storage.levels.size(); ++level) {
            const LodLevelState& lod_level = storage.levels[level];
            if (lod_level.disabled) continue;
            const u64 sampled_count = lod_level_materialized_count(lod_level);
            if (sampled_count == 0) continue;
            if (sampled_count <= previous_count / kLodMaxRecordsToSourceRatio) {
                LodSelectedLevel s;
                s.level = level;
                s.source_record_count = previous_count;
                selected.push_back(s);
                previous_count = sampled_count;
            }
        }
        return selected;
    }

    static u64 lod_time_chunk_span(u64 bucket_cycles) {
        if (bucket_cycles == 0) return kLodTimeChunkBuckets;
        if (bucket_cycles > (std::numeric_limits<u64>::max)() / kLodTimeChunkBuckets) {
            return (std::numeric_limits<u64>::max)();
        }
        return bucket_cycles * kLodTimeChunkBuckets;
    }

    static i64 lod_time_window_start(i64 cycle, u64 span) {
        if (cycle <= 0 || span == 0) return 0;
        const u64 ucycle = static_cast<u64>(cycle);
        const u64 start = (ucycle / span) * span;
        const u64 imax = static_cast<u64>((std::numeric_limits<i64>::max)());
        return static_cast<i64>((std::min)(start, imax));
    }

    static i64 lod_time_window_end(i64 start, u64 span) {
        const u64 imax = static_cast<u64>((std::numeric_limits<i64>::max)());
        const u64 ustart = start <= 0 ? 0u : static_cast<u64>(start);
        if (span == 0 || span > imax - ustart) return (std::numeric_limits<i64>::max)();
        return static_cast<i64>(ustart + span);
    }

    bool write_lodz_section(u32 level_index,
                            u32 signal_chunk_id,
                            i64 start_cycle,
                            i64 end_cycle,
                            const std::vector<u8>& raw_payload,
                            u64 storage_count,
                            u64 record_count,
                            std::string& error) {
        if (raw_payload.empty() || storage_count == 0 || record_count == 0) return true;

        Compression compression = Compression::None;
        std::vector<u8> encoded = raw_payload;
        if (options_.compression == Compression::Zstd) {
            std::vector<u8> compressed;
            if (!compress_payload_zstd(raw_payload, compressed, error)) return false;
            if (!compressed.empty() && compressed.size() < raw_payload.size()) {
                encoded.swap(compressed);
                compression = Compression::Zstd;
            }
        }

        const u64 chunk_id = static_cast<u64>(lod_chunk_index_.size());
        std::vector<u8> section_payload;
        section_payload.reserve(48 + encoded.size());
        detail::append_varuint(section_payload, chunk_id);
        detail::append_varuint(section_payload, level_index);
        detail::append_varuint(section_payload, signal_chunk_id);
        detail::append_i64(section_payload, start_cycle);
        detail::append_i64(section_payload, end_cycle);
        detail::append_u8(section_payload, static_cast<u8>(compression));
        detail::append_varuint(section_payload, static_cast<u64>(raw_payload.size()));
        detail::append_varuint(section_payload, static_cast<u64>(encoded.size()));
        detail::append_vector_bytes(section_payload, encoded);

        const u64 offset = static_cast<u64>(out_.tellp());
        if (!write_section("LODZ", section_payload, error)) return false;
        const u64 end = static_cast<u64>(out_.tellp());

        LodChunkIndexRecord idx;
        idx.chunk_id = chunk_id;
        idx.level_index = level_index;
        idx.signal_chunk_id = signal_chunk_id;
        idx.start_cycle = start_cycle;
        idx.end_cycle = end_cycle;
        idx.file_offset = offset;
        idx.file_size = end - offset;
        idx.raw_size = static_cast<u64>(raw_payload.size());
        idx.compression = compression;
        idx.storage_count = storage_count;
        idx.record_count = record_count;
        lod_chunk_index_.push_back(idx);
        return true;
    }

    bool write_lodz_chunks(std::string& error) {
        lod_chunk_index_.clear();
        if (!options_.enable_lod_tables || lod_bucket_cycles_.empty() || lod_states_.empty()) return true;

        std::vector<std::map<u32, std::vector<LodSelectedStorageLevel> > > selected_by_level_and_chunk(lod_bucket_cycles_.size());
        const u32 spc = (options_.enable_signal_chunking && options_.signals_per_chunk != 0)
            ? options_.signals_per_chunk
            : ((max_signal_id_ == 0) ? 1u : max_signal_id_);

        for (std::size_t sid = 0; sid < lod_states_.size(); ++sid) {
            const LodStorageState& storage = lod_states_[sid];
            if (!storage.valid || storage.levels.empty()) continue;
            const std::vector<LodSelectedLevel> selected_levels = select_lod_levels_to_store(storage);
            if (selected_levels.empty()) continue;
            const u32 storage_id = static_cast<u32>(sid);
            if (storage_id == 0) continue;
            const u32 signal_chunk_id = options_.enable_signal_chunking
                ? ((storage_id - 1u) / spc)
                : 0u;
            for (std::size_t i = 0; i < selected_levels.size(); ++i) {
                const std::size_t level = selected_levels[i].level;
                if (level < selected_by_level_and_chunk.size()) {
                    LodSelectedStorageLevel selected;
                    selected.storage_id = storage_id;
                    selected.source_record_count = selected_levels[i].source_record_count;
                    selected_by_level_and_chunk[level][signal_chunk_id].push_back(selected);
                }
            }
        }

        for (std::size_t level = 0; level < selected_by_level_and_chunk.size(); ++level) {
            const u64 span = lod_time_chunk_span(lod_bucket_cycles_[level]);
            std::map<u32, std::vector<LodSelectedStorageLevel> >& by_chunk = selected_by_level_and_chunk[level];
            for (std::map<u32, std::vector<LodSelectedStorageLevel> >::iterator it = by_chunk.begin(); it != by_chunk.end(); ++it) {
                std::vector<LodStorageLevelRef> refs;
                refs.reserve(it->second.size());
                for (std::size_t i = 0; i < it->second.size(); ++i) {
                    const LodSelectedStorageLevel& selected = it->second[i];
                    const u32 storage_id = selected.storage_id;
                    if (storage_id >= lod_states_.size()) continue;
                    const LodStorageState& storage = lod_states_[storage_id];
                    if (level >= storage.levels.size()) continue;
                    const LodLevelState& lod_level = storage.levels[level];
                    if (lod_level.disabled || lod_level.transitions.empty()) continue;
                    std::vector<Transition> materialized = materialize_lod_level_transitions(lod_level);
                    if (static_cast<u64>(materialized.size()) >
                        selected.source_record_count / kLodMaxRecordsToSourceRatio) {
                        materialized.clear();
                    }
                    if (materialized.empty()) continue;
                    LodStorageLevelRef ref;
                    ref.storage_id = storage_id;
                    ref.storage = &storage;
                    ref.lod_level = &lod_level;
                    ref.transitions.swap(materialized);
                    ref.valid_ranges = materialize_lod_level_valid_ranges(lod_level);
                    refs.push_back(ref);
                }

                while (true) {
                    bool have_next = false;
                    i64 window_start = (std::numeric_limits<i64>::max)();
                    for (std::size_t r = 0; r < refs.size(); ++r) {
                        const std::vector<Transition>& transitions = refs[r].transitions;
                        if (refs[r].cursor >= transitions.size()) continue;
                        const i64 candidate = lod_time_window_start(transitions[refs[r].cursor].cycle, span);
                        if (!have_next || candidate < window_start) {
                            have_next = true;
                            window_start = candidate;
                        }
                    }
                    if (!have_next) break;
                    const i64 window_end = lod_time_window_end(window_start, span);

                    std::vector<u8> records_payload;
                    u64 storage_count = 0;
                    u64 record_count = 0;
                    for (std::size_t r = 0; r < refs.size(); ++r) {
                        const std::vector<Transition>& transitions = refs[r].transitions;
                        if (refs[r].cursor >= transitions.size()) continue;
                        if (lod_time_window_start(transitions[refs[r].cursor].cycle, span) != window_start) continue;

                        const std::size_t begin = refs[r].cursor;
                        while (refs[r].cursor < transitions.size() &&
                               transitions[refs[r].cursor].cycle < window_end) {
                            ++refs[r].cursor;
                        }
                        u64 slice_count = 0;
                        const std::vector<u8> stream_payload =
                            build_lod_transition_payload(transitions, begin, refs[r].cursor, *refs[r].storage, slice_count);
                        if (slice_count == 0 || stream_payload.empty()) continue;

                        std::vector<LodValidRange> valid_ranges;
                        for (std::size_t vr = 0; vr < refs[r].valid_ranges.size(); ++vr) {
                            const LodValidRange& src = refs[r].valid_ranges[vr];
                            if (src.end_cycle <= window_start || src.start_cycle >= window_end) continue;
                            LodValidRange clipped;
                            clipped.start_cycle = src.start_cycle;
                            clipped.end_cycle = src.end_cycle;
                            if (clipped.end_cycle > clipped.start_cycle) valid_ranges.push_back(clipped);
                        }
                        if (valid_ranges.empty()) continue;

                        detail::append_varuint(records_payload, refs[r].storage_id);
                        detail::append_varuint(records_payload, refs[r].storage->byte_width);
                        detail::append_varuint(records_payload, static_cast<u64>(valid_ranges.size()));
                        for (std::size_t vr = 0; vr < valid_ranges.size(); ++vr) {
                            detail::append_i64(records_payload, valid_ranges[vr].start_cycle);
                            detail::append_i64(records_payload, valid_ranges[vr].end_cycle);
                        }
                        detail::append_varuint(records_payload, slice_count);
                        detail::append_varuint(records_payload, static_cast<u64>(stream_payload.size()));
                        detail::append_vector_bytes(records_payload, stream_payload);
                        ++storage_count;
                        record_count += slice_count;
                    }

                    if (storage_count == 0 || record_count == 0) continue;
                    std::vector<u8> raw_payload;
                    detail::append_varuint(raw_payload, storage_count);
                    detail::append_vector_bytes(raw_payload, records_payload);
                    if (!write_lodz_section(static_cast<u32>(level), it->first, window_start, window_end,
                                            raw_payload, storage_count, record_count, error)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    void append_lodz_index_to_footer(std::vector<u8>& payload) const {
        detail::append_varuint(payload, static_cast<u64>(lod_bucket_cycles_.size()));
        for (std::size_t i = 0; i < lod_bucket_cycles_.size(); ++i) {
            detail::append_varuint(payload, lod_bucket_cycles_[i]);
        }

        detail::append_varuint(payload, static_cast<u64>(lod_chunk_index_.size()));
        for (std::size_t i = 0; i < lod_chunk_index_.size(); ++i) {
            const LodChunkIndexRecord& r = lod_chunk_index_[i];
            detail::append_varuint(payload, r.chunk_id);
            detail::append_varuint(payload, r.level_index);
            detail::append_varuint(payload, r.signal_chunk_id);
            detail::append_i64(payload, r.start_cycle);
            detail::append_i64(payload, r.end_cycle);
            detail::append_varuint(payload, r.file_offset);
            detail::append_varuint(payload, r.file_size);
            detail::append_varuint(payload, r.raw_size);
            detail::append_u8(payload, static_cast<u8>(r.compression));
            detail::append_varuint(payload, r.storage_count);
            detail::append_varuint(payload, r.record_count);
        }
    }

    bool write_footer_and_patch_header(std::string& error) {
        if (!write_lodz_chunks(error)) return false;
        footer_offset_ = static_cast<u64>(out_.tellp());
        std::vector<u8> payload;
        detail::append_varuint(payload, static_cast<u64>(block_index_.size()));
        for (std::size_t i = 0; i < block_index_.size(); ++i) {
            const BlockIndexRecord& r = block_index_[i];
            detail::append_varuint(payload, r.block_id);
            detail::append_i64(payload, r.start_cycle);
            detail::append_i64(payload, r.end_cycle);
            detail::append_varuint(payload, r.signal_chunk_id);
            detail::append_varuint(payload, r.first_signal_id);
            detail::append_varuint(payload, r.signal_count);
            detail::append_varuint(payload, r.file_offset);
            detail::append_varuint(payload, r.file_size);
            detail::append_varuint(payload, r.raw_size);
            detail::append_u8(payload, static_cast<u8>(r.compression));
        }

        std::map<u32, std::vector<u64> > block_indexes_by_chunk;
        for (std::size_t i = 0; i < block_index_.size(); ++i) {
            block_indexes_by_chunk[block_index_[i].signal_chunk_id].push_back(static_cast<u64>(i));
        }
        detail::append_varuint(payload, static_cast<u64>(block_indexes_by_chunk.size()));
        for (std::map<u32, std::vector<u64> >::const_iterator it = block_indexes_by_chunk.begin();
             it != block_indexes_by_chunk.end(); ++it) {
            detail::append_varuint(payload, it->first);
            detail::append_varuint(payload, static_cast<u64>(it->second.size()));
            u64 prev = 0;
            for (std::size_t i = 0; i < it->second.size(); ++i) {
                detail::append_varuint(payload, it->second[i] - prev);
                prev = it->second[i];
            }
        }
        append_lodz_index_to_footer(payload);
        if (!write_section("FOOT", payload, error)) return false;

        out_.seekp(8 + 4 + 4 + 8, std::ios::beg); // magic + version + header_size + block_span
        if (!detail::write_u64(out_, footer_offset_)) { error = "failed to patch footer offset"; return false; }
        out_.seekp(0, std::ios::end);
        return true;
    }

    void reset_all() {
        opened_ = false;
        layout_ = Layout();
        output_path_.clear();
        signal_states_.clear();
        signal_order_.clear();
        lod_states_.clear();
        lod_bucket_cycles_.clear();
        block_index_.clear();
        lod_chunk_index_.clear();
        current_block_shared_times_by_chunk_.clear();
        max_signal_id_ = 0;
        have_pending_content_ = false;
        current_block_start_ = 0;
        current_cycle_ = 0;
        have_submitted_cycle_ = false;
        next_block_id_ = 0;
        footer_offset_ = 0;
        update_state_scratch_.clear();
        update_seen_stamp_.clear();
        update_seen_epoch_ = 1;
        stats_ = WriterStats();
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
        pipeline_.jobs_enqueued = 0;
        pipeline_.jobs_started = 0;
        pipeline_.jobs_finished = 0;
        pipeline_.results_pushed = 0;
        pipeline_.results_written = 0;
        pipeline_.compression_workers_started = 0;
        pipeline_.file_writer_started = false;
        pipeline_.next_jobs_log_size = 8;
        pipeline_.next_results_log_size = 8;
        pipeline_.diag_log_lines = 0;
    }

private:
    WriterOptions options_;
    Layout layout_;
    std::string output_path_;
    bool opened_ = false;
    std::ofstream out_;
    std::vector<SignalState> signal_states_; // indexed by signal_id
    std::vector<u32> signal_order_;          // ascending signal_id order
    std::vector<LodStorageState> lod_states_; // indexed by physical storage_id
    std::vector<u64> lod_bucket_cycles_;
    std::vector<BlockIndexRecord> block_index_;
    std::vector<LodChunkIndexRecord> lod_chunk_index_;
    std::vector<std::vector<u64> > current_block_shared_times_by_chunk_;
    u32 max_signal_id_ = 0;
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
    WriterStats stats_;
    u64 writer_submit_count_ = 0;
    u32 writer_diag_log_lines_ = 0;
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
              std::size_t queue_limit = 0,
              std::size_t queue_bytes_limit = 0) {
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
            queued_bytes_ = 0;
            stop_ = false;
            queue_limit_ = queue_limit;
            queue_bytes_limit_ = queue_bytes_limit;
            worker_started_ = false;
            enqueued_count_ = 0;
            dequeued_count_ = 0;
            submit_done_count_ = 0;
            worker_in_submit_ = false;
            active_submit_cycle_ = -1;
            active_submit_updates_ = 0;
            active_submit_approx_bytes_ = 0;
            active_submit_start_ms_ = 0;
            last_submit_cycle_ = -1;
            last_submit_updates_ = 0;
            last_submit_duration_ms_ = 0;
            next_queue_log_size_ = 64;
            diag_log_lines_ = 0;
        }
        if (!writer_.open(path, layout, options, error)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_ = true;
        }
        worker_ = std::thread([this]() { this->worker_loop(); });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            async_diag_log_locked_("async-open");
        }
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
            queued_bytes_ = 0;
            stop_ = false;
            worker_started_ = false;
        }
        if (!ok) error = local_error.empty() ? "WVZ4 AsyncWriter close failed" : local_error;
        return ok;
    }

private:
    void async_diag_log_locked_(const char* tag, const CycleSubmission* submission = NULL, std::size_t approx_bytes = 0) {
        if (diag_log_lines_ >= 128u || detail::backlog_log_disabled()) return;
        ++diag_log_lines_;
        const std::size_t updates = submission ? submission->updates.size() : 0;
        const long long cycle = submission ? static_cast<long long>(submission->cycle) : -1ll;
        const unsigned long long now_ms = detail::backlog_now_ms();
        const unsigned long long active_ms = worker_in_submit_ ? (now_ms - active_submit_start_ms_) : 0ull;
        detail::backlog_log(
            "%s async=%p opened=%d stop=%d worker_started=%d worker_joinable=%d q=%zu qbytes=%zu qlimit=%zu qblimit=%zu enq=%llu deq=%llu done=%llu in_submit=%d active_ms=%llu active_cycle=%lld active_updates=%zu active_approx=%zu last_cycle=%lld last_updates=%zu last_ms=%llu cycle=%lld updates=%zu approx=%zu err=%s",
            tag,
            static_cast<void*>(this),
            opened_ ? 1 : 0,
            stop_ ? 1 : 0,
            worker_started_ ? 1 : 0,
            worker_.joinable() ? 1 : 0,
            queue_.size(),
            queued_bytes_,
            queue_limit_,
            queue_bytes_limit_,
            static_cast<unsigned long long>(enqueued_count_),
            static_cast<unsigned long long>(dequeued_count_),
            static_cast<unsigned long long>(submit_done_count_),
            worker_in_submit_ ? 1 : 0,
            active_ms,
            static_cast<long long>(active_submit_cycle_),
            active_submit_updates_,
            active_submit_approx_bytes_,
            static_cast<long long>(last_submit_cycle_),
            last_submit_updates_,
            last_submit_duration_ms_,
            cycle,
            updates,
            approx_bytes,
            error_.empty() ? "" : error_.c_str());
    }

    void maybe_log_async_queue_locked_(const char* tag, const CycleSubmission* submission = NULL, std::size_t approx_bytes = 0) {
        if (diag_log_lines_ >= 128u || detail::backlog_log_disabled()) return;
        const std::size_t q = queue_.size();
        bool should = false;
        if (q >= next_queue_log_size_) {
            should = true;
            while (next_queue_log_size_ <= q && next_queue_log_size_ < (std::numeric_limits<std::size_t>::max() / 2)) {
                next_queue_log_size_ *= 2;
            }
        }
        if (!should && q > 0 && queue_limit_ > 0 && q >= queue_limit_) {
            if ((enqueued_count_ & 0xffffull) == 0ull) should = true;
        }
        if (!should && queue_bytes_limit_ > 0 && queued_bytes_ >= queue_bytes_limit_) {
            if ((enqueued_count_ & 0xffffull) == 0ull) should = true;
        }
        if (should) async_diag_log_locked_(tag, submission, approx_bytes);
    }

    static std::size_t estimate_submission_bytes(const CycleSubmission& submission) noexcept {
        return sizeof(CycleSubmission) + submission.updates.size() * sizeof(CycleValueUpdate);
    }

    bool enqueue(CycleSubmission&& submission, std::string& error) {
        error.clear();
        const std::size_t approx_bytes = estimate_submission_bytes(submission);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!opened_) { error = "WVZ4 AsyncWriter is not open"; return false; }
        if (stop_) { error = "WVZ4 AsyncWriter is stopping"; return false; }
        if (!error_.empty()) { error = error_; return false; }
        if (queue_limit_ > 0 || queue_bytes_limit_ > 0) {
            cv_space_.wait(lock, [this, approx_bytes]() {
                if (stop_ || !error_.empty()) return true;
                const bool count_ok = (queue_limit_ == 0) || (queue_.size() < queue_limit_);
                const bool bytes_ok = (queue_bytes_limit_ == 0) ||
                    (queued_bytes_ + approx_bytes <= queue_bytes_limit_) ||
                    queue_.empty(); // allow one oversized cycle submission to make progress
                return count_ok && bytes_ok;
            });
            if (!error_.empty()) { error = error_; return false; }
            if (stop_) { error = "WVZ4 AsyncWriter is stopping"; return false; }
        }
        QueueItem item;
        item.submission = std::move(submission);
        item.approx_bytes = approx_bytes;
        queued_bytes_ += approx_bytes;
        queue_.push_back(std::move(item));
        ++enqueued_count_;
        maybe_log_async_queue_locked_("async-enqueue", &queue_.back().submission, approx_bytes);
        cv_not_empty_.notify_one();
        return true;
    }

    void worker_loop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_started_ = true;
            async_diag_log_locked_("async-worker-start");
        }
        while (true) {
            QueueItem item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_not_empty_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stop_) return;
                    continue;
                }
                item = std::move(queue_.front());
                if (queued_bytes_ >= item.approx_bytes) queued_bytes_ -= item.approx_bytes;
                else queued_bytes_ = 0;
                queue_.pop_front();
                ++dequeued_count_;
                maybe_log_async_queue_locked_("async-dequeue", &item.submission, item.approx_bytes);
                cv_space_.notify_one();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                worker_in_submit_ = true;
                active_submit_cycle_ = item.submission.cycle;
                active_submit_updates_ = item.submission.updates.size();
                active_submit_approx_bytes_ = item.approx_bytes;
                active_submit_start_ms_ = detail::backlog_now_ms();
                if (queue_.size() >= next_queue_log_size_ / 2u || active_submit_updates_ > 100000u) {
                    async_diag_log_locked_("async-submit-begin", &item.submission, item.approx_bytes);
                }
            }
            std::string err;
            const bool ok = writer_.submit_cycle(item.submission, err);
            const unsigned long long done_ms = detail::backlog_now_ms();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const unsigned long long duration_ms = done_ms >= active_submit_start_ms_ ? (done_ms - active_submit_start_ms_) : 0ull;
                last_submit_cycle_ = active_submit_cycle_;
                last_submit_updates_ = active_submit_updates_;
                last_submit_duration_ms_ = duration_ms;
                ++submit_done_count_;
                worker_in_submit_ = false;
                active_submit_cycle_ = -1;
                active_submit_updates_ = 0;
                active_submit_approx_bytes_ = 0;
                active_submit_start_ms_ = 0;
                if (!ok || duration_ms >= slow_submit_log_ms_ || queue_.size() >= next_queue_log_size_ / 2u) {
                    async_diag_log_locked_(ok ? "async-submit-end" : "async-submit-error", &item.submission, item.approx_bytes);
                }
                if (!ok) {
                    if (error_.empty()) error_ = err;
                    stop_ = true;
                    queue_.clear();
                    queued_bytes_ = 0;
                    cv_space_.notify_all();
                    return;
                }
            }
        }
    }

private:
    struct QueueItem {
        CycleSubmission submission;
        std::size_t approx_bytes = 0;
    };

    Writer writer_;
    bool opened_ = false;
    bool stop_ = false;
    std::size_t queue_limit_ = 0;
    std::size_t queue_bytes_limit_ = 0;
    std::size_t queued_bytes_ = 0;
    bool worker_started_ = false;
    bool worker_in_submit_ = false;
    i64 active_submit_cycle_ = -1;
    std::size_t active_submit_updates_ = 0;
    std::size_t active_submit_approx_bytes_ = 0;
    unsigned long long active_submit_start_ms_ = 0;
    i64 last_submit_cycle_ = -1;
    std::size_t last_submit_updates_ = 0;
    unsigned long long last_submit_duration_ms_ = 0;
    u64 enqueued_count_ = 0;
    u64 dequeued_count_ = 0;
    u64 submit_done_count_ = 0;
    std::size_t next_queue_log_size_ = 64;
    u32 diag_log_lines_ = 0;
    static const unsigned long long slow_submit_log_ms_ = 1000ull;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_space_;
    std::deque<QueueItem> queue_;
    std::string error_;
};


// -----------------------------------------------------------------------------
// Writer helper process support.
//
// The parent process submits layout/cycle frames over a named pipe.  The helper
// process owns the real Writer and finalizes the WVZ4 directly when the parent
// closes normally, exits, crashes, or is killed.
// -----------------------------------------------------------------------------

namespace detail {
inline u32 fnv1a32(const std::vector<u8>& data) {
    u32 h = 2166136261u;
    for (std::size_t i = 0; i < data.size(); ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

inline void append_i32(std::vector<u8>& out, std::int32_t v) {
    append_u32(out, static_cast<u32>(v));
}

inline bool file_exists(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return static_cast<bool>(in);
}

#if defined(_WIN32)
inline std::string win32_error_message(const std::string& prefix, DWORD code = GetLastError()) {
    std::ostringstream os;
    os << prefix << " (GetLastError=" << static_cast<unsigned long>(code) << ")";
    return os.str();
}

inline std::string parent_dir(const std::string& path) {
    const std::size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return std::string();
    return path.substr(0, pos);
}

inline std::string join_path(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    const char last = dir[dir.size() - 1];
    if (last == '\\' || last == '/') return dir + leaf;
    return dir + "\\" + leaf;
}

inline std::string current_exe_dir() {
    char buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (n == 0 || n >= sizeof(buffer)) return std::string();
    return parent_dir(std::string(buffer, buffer + n));
}

inline std::string quote_process_arg(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '"') out.push_back('\\');
        out.push_back(arg[i]);
    }
    out.push_back('"');
    return out;
}

inline bool write_all_handle(HANDLE h, const void* data, std::size_t size, std::string& error) {
    const u8* p = static_cast<const u8*>(data);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(size, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, NULL)) {
            error = win32_error_message("failed to write WVZ4 IPC pipe");
            return false;
        }
        if (written == 0) {
            error = "failed to write WVZ4 IPC pipe: zero-byte write";
            return false;
        }
        p += written;
        size -= written;
    }
    return true;
}

inline bool read_all_handle(HANDLE h, void* data, std::size_t size, bool& disconnected, std::string& error) {
    disconnected = false;
    u8* p = static_cast<u8*>(data);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(size, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(h, p, chunk, &read, NULL)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF || code == ERROR_PIPE_NOT_CONNECTED) {
                disconnected = true;
                return false;
            }
            error = win32_error_message("failed to read WVZ4 IPC pipe", code);
            return false;
        }
        if (read == 0) {
            disconnected = true;
            return false;
        }
        p += read;
        size -= read;
    }
    return true;
}

inline bool wait_process_exited(HANDLE process, DWORD milliseconds) {
    return process && process != INVALID_HANDLE_VALUE &&
           WaitForSingleObject(process, milliseconds) == WAIT_OBJECT_0;
}
#endif

inline u32 load_u32_le(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

inline u64 load_u64_le(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<u64>(p[i]) << (8 * i));
    return v;
}

class PayloadReader {
public:
    PayloadReader(const std::vector<u8>& data) : data_(data) {}
    bool read_u8(u8& v) {
        if (pos_ >= data_.size()) return false;
        v = data_[pos_++]; return true;
    }
    bool read_varuint(u64& v) {
        v = 0;
        unsigned shift = 0;
        while (pos_ < data_.size() && shift <= 63) {
            u8 b = data_[pos_++];
            v |= (static_cast<u64>(b & 0x7fu) << shift);
            if ((b & 0x80u) == 0) return true;
            shift += 7;
        }
        return false;
    }
    bool read_i64(i64& v) {
        if (pos_ + 8 > data_.size()) return false;
        v = static_cast<i64>(load_u64_le(&data_[pos_]));
        pos_ += 8;
        return true;
    }
    bool read_string(std::string& s) {
        u64 n = 0;
        if (!read_varuint(n)) return false;
        if (n > data_.size() - pos_) return false;
        s.assign(reinterpret_cast<const char*>(&data_[pos_]), static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
        return true;
    }
    bool read_bytes(u8* dst, std::size_t n) {
        if (n > data_.size() - pos_) return false;
        if (n != 0) std::memcpy(dst, &data_[pos_], n);
        pos_ += n;
        return true;
    }
    bool at_end() const { return pos_ == data_.size(); }
private:
    const std::vector<u8>& data_;
    std::size_t pos_ = 0;
};

inline void serialize_options(std::vector<u8>& out, const WriterOptions& o) {
    append_varuint(out, o.target_block_span);
    append_u8(out, static_cast<u8>(o.compression));
    append_i64(out, static_cast<i64>(o.zstd_level));
    append_u8(out, o.enable_block_pipeline ? 1 : 0);
    append_varuint(out, o.block_pipeline_threads);
    append_varuint(out, static_cast<u64>(o.block_pipeline_queue_limit));
    append_u8(out, o.enable_delta_time_encoding ? 1 : 0);
    append_u8(out, o.enable_shared_time_table ? 1 : 0);
    append_u8(out, o.implicit_zero_initial_values ? 1 : 0);
    append_u8(out, o.enable_signal_chunking ? 1 : 0);
    append_varuint(out, o.signals_per_chunk);
    append_u8(out, o.enable_sparse_signal_records ? 1 : 0);
    append_u8(out, o.enable_bool_toggle_encoding ? 1 : 0);
    append_u8(out, o.enable_value_byte_mask_encoding ? 1 : 0);
    append_u8(out, o.enable_stride_time_record_encoding ? 1 : 0);
    append_u8(out, o.enable_lod_tables ? 1 : 0);
    append_u8(out, o.enable_stats_log ? 1 : 0);
    append_string(out, o.stats_log_path);
    append_varuint(out, static_cast<u64>(o.transition_reserve_per_signal));
}

inline bool deserialize_options(PayloadReader& r, WriterOptions& o) {
    u64 v = 0; u8 b = 0; i64 si = 0;
    if (!r.read_varuint(v)) return false; o.target_block_span = v;
    if (!r.read_u8(b)) return false; o.compression = static_cast<Compression>(b);
    if (!r.read_i64(si)) return false; o.zstd_level = static_cast<int>(si);
    if (!r.read_u8(b)) return false; o.enable_block_pipeline = (b != 0);
    if (!r.read_varuint(v)) return false; o.block_pipeline_threads = static_cast<u32>(v);
    if (!r.read_varuint(v)) return false; o.block_pipeline_queue_limit = static_cast<std::size_t>(v);
    if (!r.read_u8(b)) return false; o.enable_delta_time_encoding = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_shared_time_table = (b != 0);
    if (!r.read_u8(b)) return false; o.implicit_zero_initial_values = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_signal_chunking = (b != 0);
    if (!r.read_varuint(v)) return false; o.signals_per_chunk = static_cast<u32>(v);
    if (!r.read_u8(b)) return false; o.enable_sparse_signal_records = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_bool_toggle_encoding = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_value_byte_mask_encoding = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_stride_time_record_encoding = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_lod_tables = (b != 0);
    if (!r.read_u8(b)) return false; o.enable_stats_log = (b != 0);
    if (!r.read_string(o.stats_log_path)) return false;
    if (!r.read_varuint(v)) return false; o.transition_reserve_per_signal = static_cast<std::size_t>(v);
    return true;
}

inline void serialize_layout(std::vector<u8>& out, const Layout& l) {
    append_varuint(out, static_cast<u64>(l.names.size()));
    for (std::size_t i = 0; i < l.names.size(); ++i) {
        append_varuint(out, l.names[i].name_id);
        append_string(out, l.names[i].name);
    }
    append_varuint(out, static_cast<u64>(l.nodes.size()));
    for (std::size_t i = 0; i < l.nodes.size(); ++i) {
        const NodeRecord& n = l.nodes[i];
        append_varuint(out, n.node_id);
        append_varuint(out, n.parent_id);
        append_varuint(out, n.name_id);
        append_u8(out, static_cast<u8>(n.kind));
        append_varuint(out, n.first_child);
        append_varuint(out, n.next_sibling);
    }
    append_varuint(out, static_cast<u64>(l.signals.size()));
    for (std::size_t i = 0; i < l.signals.size(); ++i) {
        const SignalDefinition& s = l.signals[i];
        append_varuint(out, s.signal_id);
        append_varuint(out, s.storage_id != 0 ? s.storage_id : s.signal_id);
        append_varuint(out, s.node_id);
        append_u8(out, static_cast<u8>(s.type));
        append_varuint(out, s.bit_width);
        append_u8(out, static_cast<u8>(s.radix));
        append_varuint(out, s.bit_offset);
        append_u8(out, s.storage_only ? kSignalFlagStorageOnly : 0u);
    }
    append_varuint(out, static_cast<u64>(l.clocks.size()));
    for (std::size_t i = 0; i < l.clocks.size(); ++i) {
        const ClockDefinition& c = l.clocks[i];
        append_varuint(out, c.signal_id);
        append_u8(out, c.initial_value ? 1u : 0u);
        append_varuint(out, c.period_ticks);
    }
}

inline bool deserialize_layout(PayloadReader& r, Layout& l) {
    u64 n = 0;
    if (!r.read_varuint(n)) return false;
    l.names.clear(); l.names.reserve(static_cast<std::size_t>(n));
    for (u64 i = 0; i < n; ++i) {
        NameRecord rec; u64 tmp = 0;
        if (!r.read_varuint(tmp)) return false; rec.name_id = static_cast<u32>(tmp);
        if (!r.read_string(rec.name)) return false;
        l.names.push_back(rec);
    }
    if (!r.read_varuint(n)) return false;
    l.nodes.clear(); l.nodes.reserve(static_cast<std::size_t>(n));
    for (u64 i = 0; i < n; ++i) {
        NodeRecord rec; u64 tmp = 0; u8 b = 0;
        if (!r.read_varuint(tmp)) return false; rec.node_id = static_cast<u32>(tmp);
        if (!r.read_varuint(tmp)) return false; rec.parent_id = static_cast<u32>(tmp);
        if (!r.read_varuint(tmp)) return false; rec.name_id = static_cast<u32>(tmp);
        if (!r.read_u8(b)) return false; rec.kind = static_cast<NodeKind>(b);
        if (!r.read_varuint(tmp)) return false; rec.first_child = static_cast<u32>(tmp);
        if (!r.read_varuint(tmp)) return false; rec.next_sibling = static_cast<u32>(tmp);
        l.nodes.push_back(rec);
    }
    if (!r.read_varuint(n)) return false;
    l.signals.clear(); l.signals.reserve(static_cast<std::size_t>(n));
    for (u64 i = 0; i < n; ++i) {
        SignalDefinition rec; u64 tmp = 0; u8 b = 0;
        if (!r.read_varuint(tmp)) return false; rec.signal_id = static_cast<u32>(tmp);
        if (!r.read_varuint(tmp)) return false; rec.storage_id = static_cast<u32>(tmp);
        if (rec.storage_id == 0) rec.storage_id = rec.signal_id;
        if (!r.read_varuint(tmp)) return false; rec.node_id = static_cast<u32>(tmp);
        if (!r.read_u8(b)) return false; rec.type = static_cast<ValueType>(b);
        if (!r.read_varuint(tmp)) return false; rec.bit_width = static_cast<u32>(tmp);
        if (!r.read_u8(b)) return false; rec.radix = static_cast<Radix>(b);
        if (!r.read_varuint(tmp)) return false; rec.bit_offset = static_cast<u32>(tmp);
        if (!r.read_u8(b)) return false; rec.storage_only = (b & kSignalFlagStorageOnly) != 0;
        l.signals.push_back(rec);
    }
    if (!r.read_varuint(n)) return false;
    l.clocks.clear(); l.clocks.reserve(static_cast<std::size_t>(n));
    for (u64 i = 0; i < n; ++i) {
        ClockDefinition rec; u64 tmp = 0; u8 b = 0;
        if (!r.read_varuint(tmp)) return false; rec.signal_id = static_cast<u32>(tmp);
        if (!r.read_u8(b)) return false; rec.initial_value = (b != 0);
        if (!r.read_varuint(tmp)) return false; rec.period_ticks = tmp;
        l.clocks.push_back(rec);
    }
    return true;
}

inline void serialize_cycle(std::vector<u8>& out, const CycleSubmission& s) {
    append_i64(out, s.cycle);
    append_varuint(out, static_cast<u64>(s.updates.size()));
    for (std::size_t i = 0; i < s.updates.size(); ++i) {
        const CycleValueUpdate& u = s.updates[i];
        append_varuint(out, u.signal_id);
        append_u8(out, u.value.byte_count);
        out.insert(out.end(), u.value.bytes.begin(), u.value.bytes.begin() + u.value.byte_count);
    }
}

inline bool deserialize_cycle(PayloadReader& r, CycleSubmission& s) {
    if (!r.read_i64(s.cycle)) return false;
    u64 n = 0;
    if (!r.read_varuint(n)) return false;
    s.updates.clear(); s.updates.reserve(static_cast<std::size_t>(n));
    for (u64 i = 0; i < n; ++i) {
        CycleValueUpdate u; u64 sid = 0; u8 bc = 0;
        if (!r.read_varuint(sid)) return false; u.signal_id = static_cast<u32>(sid);
        if (!r.read_u8(bc)) return false;
        if (bc > kMaxScalarBytes) return false;
        u.value.byte_count = bc;
        u.value.bytes.fill(0);
        if (!r.read_bytes(u.value.bytes.data(), bc)) return false;
        s.updates.push_back(u);
    }
    return true;
}

static const u32 kIpcMagic = 0x34504957u;    // "WIP4" little-endian spelling.
static const u32 kIpcAckMagic = 0x34415057u; // "WPA4" little-endian spelling.
static const std::size_t kIpcHeaderSize = 28;
static const std::size_t kIpcAckHeaderSize = 24;
static const u64 kMaxIpcPayloadBytes = 512ull * 1024ull * 1024ull;
} // namespace detail

enum class WriterProcessFrameType : u32 {
    Layout   = 1,
    Cycle    = 2,
    Finalize = 3
};

class WriterProcessClient {
public:
    WriterProcessClient() {}
    ~WriterProcessClient() { std::string ignored; close(ignored); }

    WriterProcessClient(const WriterProcessClient&) = delete;
    WriterProcessClient& operator=(const WriterProcessClient&) = delete;

    bool open(const std::string& output_path,
              const Layout& layout,
              const WriterOptions& options,
              std::string& error,
              const std::string& helper_exe_path = std::string(),
              DWORD connect_timeout_ms = 10000) {
#if !defined(_WIN32)
        (void)output_path; (void)layout; (void)options; (void)helper_exe_path; (void)connect_timeout_ms;
        error = "WVZ4 writer helper process is only implemented on Windows";
        return false;
#else
        std::string close_error;
        close(close_error);
        error.clear();

        const std::string helper = resolve_helper_path(helper_exe_path);
        if (helper.empty()) {
            error = "failed to find wvz4_writer_monitor.exe for WVZ4 writer helper";
            return false;
        }

        pipe_name_ = make_pipe_name();
        if (!start_helper_process(helper, output_path, error)) {
            cleanup_handles(false);
            return false;
        }
        if (!connect_pipe(connect_timeout_ms, error)) {
            cleanup_handles(true);
            return false;
        }

        std::vector<u8> payload;
        detail::serialize_options(payload, options);
        detail::serialize_layout(payload, layout);
        if (!send_frame(WriterProcessFrameType::Layout, payload, error) ||
            !read_ack(seq_, error)) {
            cleanup_handles(true);
            return false;
        }
        opened_ = true;
        return true;
#endif
    }

    bool submit_cycle(const CycleSubmission& submission, std::string& error) {
#if !defined(_WIN32)
        (void)submission;
        error = "WVZ4 writer helper process is only implemented on Windows";
        return false;
#else
        error.clear();
        if (!opened_) { error = "WVZ4 WriterProcessClient is not open"; return false; }
        std::vector<u8> payload;
        detail::serialize_cycle(payload, submission);
        return send_frame(WriterProcessFrameType::Cycle, payload, error) &&
               read_ack(seq_, error);
#endif
    }

    bool close(std::string& error) {
        error.clear();
#if defined(_WIN32)
        if (!opened_) {
            cleanup_handles(false);
            return true;
        }

        bool ok = true;
        std::string local_error;
        std::vector<u8> payload;
        if (!send_frame(WriterProcessFrameType::Finalize, payload, local_error) ||
            !read_ack(seq_, local_error)) {
            ok = false;
        }
        cleanup_handles(true);
        opened_ = false;
        if (!ok) error = local_error.empty() ? "WVZ4 writer helper close failed" : local_error;
        return ok;
#else
        return true;
#endif
    }

private:
#if defined(_WIN32)
    static std::string make_pipe_name() {
        std::ostringstream os;
        os << "\\\\.\\pipe\\WaveTraceWVZ4Writer_"
           << static_cast<unsigned long>(GetCurrentProcessId()) << "_"
           << static_cast<unsigned long long>(GetTickCount64()) << "_"
           << reinterpret_cast<std::uintptr_t>(&os);
        return os.str();
    }

    static std::string resolve_helper_path(const std::string& explicit_path) {
        if (!explicit_path.empty() && detail::file_exists(explicit_path)) return explicit_path;

        std::vector<std::string> candidates;
        candidates.push_back("wvz4_writer_monitor.exe");
        candidates.push_back(detail::join_path("build_vs\\wvz4_writer_monitor\\Release", "wvz4_writer_monitor.exe"));
        const std::string exe_dir = detail::current_exe_dir();
        if (!exe_dir.empty()) {
            candidates.push_back(detail::join_path(exe_dir, "wvz4_writer_monitor.exe"));
            candidates.push_back(detail::join_path(detail::join_path(detail::parent_dir(detail::parent_dir(exe_dir)),
                                                                      "wvz4_writer_monitor\\Release"),
                                                  "wvz4_writer_monitor.exe"));
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (detail::file_exists(candidates[i])) return candidates[i];
        }
        return std::string();
    }

    bool start_helper_process(const std::string& helper,
                              const std::string& output_path,
                              std::string& error) {
        std::string cmd;
        cmd.reserve(helper.size() + output_path.size() + pipe_name_.size() + 128);
        cmd += detail::quote_process_arg(helper);
        cmd += " --writer-helper --pipe ";
        cmd += detail::quote_process_arg(pipe_name_);
        cmd += " --parent-pid ";
        cmd += std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
        cmd += " --out ";
        cmd += detail::quote_process_arg(output_path);

        std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back('\0');

        STARTUPINFOA si;
        std::memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        std::memset(&pi_, 0, sizeof(pi_));
        if (!CreateProcessA(helper.c_str(), mutable_cmd.data(), NULL, NULL, FALSE,
                            CREATE_NO_WINDOW, NULL, NULL, &si, &pi_)) {
            error = detail::win32_error_message("failed to start WVZ4 writer helper");
            return false;
        }
        return true;
    }

    bool connect_pipe(DWORD timeout_ms, std::string& error) {
        const ULONGLONG start = GetTickCount64();
        while (true) {
            pipe_ = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                NULL, OPEN_EXISTING, 0, NULL);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;

            const DWORD code = GetLastError();
            if (pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_OBJECT_0) {
                error = "WVZ4 writer helper exited before pipe connection";
                return false;
            }
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeout_ms) {
                error = "timed out connecting to WVZ4 writer helper pipe";
                return false;
            }
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PIPE_BUSY) {
                error = detail::win32_error_message("failed to connect WVZ4 writer helper pipe", code);
                return false;
            }
            Sleep(20);
        }
    }

    bool send_frame(WriterProcessFrameType type, const std::vector<u8>& payload, std::string& error) {
        if (pipe_ == INVALID_HANDLE_VALUE) { error = "WVZ4 writer helper pipe is not connected"; return false; }
        const u32 checksum = detail::fnv1a32(payload);
        std::vector<u8> header;
        header.reserve(detail::kIpcHeaderSize);
        detail::append_u32(header, detail::kIpcMagic);
        detail::append_u32(header, static_cast<u32>(type));
        detail::append_u64(header, ++seq_);
        detail::append_u64(header, static_cast<u64>(payload.size()));
        detail::append_u32(header, checksum);
        if (!detail::write_all_handle(pipe_, header.data(), header.size(), error)) return false;
        if (!payload.empty() && !detail::write_all_handle(pipe_, payload.data(), payload.size(), error)) return false;
        return true;
    }

    bool read_ack(u64 expected_seq, std::string& error) {
        u8 hdr[detail::kIpcAckHeaderSize];
        bool disconnected = false;
        if (!detail::read_all_handle(pipe_, hdr, sizeof(hdr), disconnected, error)) {
            if (disconnected) error = "WVZ4 writer helper pipe disconnected before ack";
            return false;
        }
        const u32 magic = detail::load_u32_le(hdr + 0);
        const u64 seq = detail::load_u64_le(hdr + 4);
        const u32 ok = detail::load_u32_le(hdr + 12);
        const u32 message_size = detail::load_u32_le(hdr + 16);
        const u32 checksum = detail::load_u32_le(hdr + 20);
        if (magic != detail::kIpcAckMagic || seq != expected_seq) {
            error = "WVZ4 writer helper returned an invalid ack";
            return false;
        }
        std::vector<u8> message(message_size);
        if (message_size > 0 &&
            !detail::read_all_handle(pipe_, message.data(), message.size(), disconnected, error)) {
            if (disconnected) error = "WVZ4 writer helper pipe disconnected during ack";
            return false;
        }
        if (detail::fnv1a32(message) != checksum) {
            error = "WVZ4 writer helper ack checksum mismatch";
            return false;
        }
        if (ok == 0) {
            error.assign(reinterpret_cast<const char*>(message.data()), message.size());
            if (error.empty()) error = "WVZ4 writer helper reported failure";
            return false;
        }
        return true;
    }

    void cleanup_handles(bool wait_for_process) {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        if (wait_for_process && pi_.hProcess) {
            WaitForSingleObject(pi_.hProcess, 5000);
        }
        if (pi_.hThread) {
            CloseHandle(pi_.hThread);
            pi_.hThread = NULL;
        }
        if (pi_.hProcess) {
            CloseHandle(pi_.hProcess);
            pi_.hProcess = NULL;
        }
    }

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi_ {};
#endif

    bool opened_ = false;
    u64 seq_ = 0;
    std::string pipe_name_;
};

class WriterProcessServer {
public:
    static bool run(const std::string& pipe_name,
                    const std::string& output_path,
                    unsigned long parent_pid,
                    std::string& error) {
#if !defined(_WIN32)
        (void)pipe_name; (void)output_path; (void)parent_pid;
        error = "WVZ4 writer helper process is only implemented on Windows";
        return false;
#else
        error.clear();
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent_pid));
        HANDLE pipe = CreateNamedPipeA(pipe_name.c_str(),
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                                       1,
                                       1u << 20,
                                       1u << 20,
                                       0,
                                       NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (parent) CloseHandle(parent);
            error = detail::win32_error_message("failed to create WVZ4 writer helper pipe");
            return false;
        }

        bool connected = false;
        while (!connected) {
            if (ConnectNamedPipe(pipe, NULL)) {
                connected = true;
                break;
            }
            const DWORD code = GetLastError();
            if (code == ERROR_PIPE_CONNECTED) {
                connected = true;
                break;
            }
            if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
                CloseHandle(pipe);
                CloseHandle(parent);
                return true;
            }
            if (code != ERROR_PIPE_LISTENING && code != ERROR_NO_DATA) {
                CloseHandle(pipe);
                if (parent) CloseHandle(parent);
                error = detail::win32_error_message("failed while waiting for WVZ4 writer helper pipe client", code);
                return false;
            }
            Sleep(20);
        }

        DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
        SetNamedPipeHandleState(pipe, &mode, NULL, NULL);

        Writer writer;
        bool have_writer = false;
        bool result = true;
        while (true) {
            WriterProcessFrameType type = WriterProcessFrameType::Finalize;
            u64 seq = 0;
            std::vector<u8> payload;
            bool disconnected = false;
            if (!read_frame(pipe, type, seq, payload, disconnected, error)) {
                if (disconnected) {
                    if (have_writer) {
                        std::string close_error;
                        if (!writer.close(close_error)) {
                            error = close_error;
                            result = false;
                        }
                    }
                    break;
                }
                result = false;
                break;
            }

            std::string op_error;
            bool ok = handle_frame(type, payload, output_path, writer, have_writer, op_error);
            if (!send_ack(pipe, seq, ok, op_error, error)) {
                result = false;
                break;
            }
            if (!ok) {
                result = false;
                break;
            }
            if (type == WriterProcessFrameType::Finalize) {
                break;
            }
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (parent) CloseHandle(parent);
        return result;
#endif
    }

private:
#if defined(_WIN32)
    static bool read_frame(HANDLE pipe,
                           WriterProcessFrameType& type,
                           u64& seq,
                           std::vector<u8>& payload,
                           bool& disconnected,
                           std::string& error) {
        u8 hdr[detail::kIpcHeaderSize];
        if (!detail::read_all_handle(pipe, hdr, sizeof(hdr), disconnected, error)) return false;
        const u32 magic = detail::load_u32_le(hdr + 0);
        const u32 type_u = detail::load_u32_le(hdr + 4);
        seq = detail::load_u64_le(hdr + 8);
        const u64 size = detail::load_u64_le(hdr + 16);
        const u32 checksum = detail::load_u32_le(hdr + 24);
        if (magic != detail::kIpcMagic) {
            error = "WVZ4 writer helper received invalid IPC magic";
            return false;
        }
        if (size > detail::kMaxIpcPayloadBytes ||
            size > static_cast<u64>((std::numeric_limits<std::size_t>::max)())) {
            error = "WVZ4 writer helper IPC payload is too large";
            return false;
        }
        payload.assign(static_cast<std::size_t>(size), 0);
        if (size > 0 && !detail::read_all_handle(pipe, payload.data(), payload.size(), disconnected, error)) {
            return false;
        }
        if (detail::fnv1a32(payload) != checksum) {
            error = "WVZ4 writer helper IPC payload checksum mismatch";
            return false;
        }
        type = static_cast<WriterProcessFrameType>(type_u);
        return true;
    }

    static bool handle_frame(WriterProcessFrameType type,
                             const std::vector<u8>& payload,
                             const std::string& output_path,
                             Writer& writer,
                             bool& have_writer,
                             std::string& error) {
        error.clear();
        detail::PayloadReader r(payload);
        if (type == WriterProcessFrameType::Layout) {
            if (have_writer) { error = "WVZ4 writer helper received duplicate layout"; return false; }
            WriterOptions opt;
            Layout layout;
            if (!detail::deserialize_options(r, opt) ||
                !detail::deserialize_layout(r, layout) ||
                !r.at_end()) {
                error = "WVZ4 writer helper received corrupt layout payload";
                return false;
            }
            if (opt.enable_stats_log && opt.stats_log_path.empty()) opt.stats_log_path = output_path + ".log";
            if (!writer.open(output_path, layout, opt, error)) return false;
            have_writer = true;
            return true;
        }
        if (type == WriterProcessFrameType::Cycle) {
            if (!have_writer) { error = "WVZ4 writer helper received cycle before layout"; return false; }
            CycleSubmission s;
            if (!detail::deserialize_cycle(r, s) || !r.at_end()) {
                error = "WVZ4 writer helper received corrupt cycle payload";
                return false;
            }
            return writer.submit_cycle(s, error);
        }
        if (type == WriterProcessFrameType::Finalize) {
            if (!have_writer) return true;
            if (!writer.close(error)) return false;
            have_writer = false;
            return true;
        }
        error = "WVZ4 writer helper received unknown IPC frame type";
        return false;
    }

    static bool send_ack(HANDLE pipe, u64 seq, bool ok, const std::string& message, std::string& error) {
        std::vector<u8> msg;
        if (!message.empty()) {
            msg.assign(message.begin(), message.end());
        }
        std::vector<u8> header;
        header.reserve(detail::kIpcAckHeaderSize);
        detail::append_u32(header, detail::kIpcAckMagic);
        detail::append_u64(header, seq);
        detail::append_u32(header, ok ? 1u : 0u);
        detail::append_u32(header, static_cast<u32>(msg.size()));
        detail::append_u32(header, detail::fnv1a32(msg));
        if (!detail::write_all_handle(pipe, header.data(), header.size(), error)) return false;
        if (!msg.empty() && !detail::write_all_handle(pipe, msg.data(), msg.size(), error)) return false;
        return true;
    }
#endif
};

} // namespace wvz4

#if defined(WVZ4_WRITER_TYPED_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef WVZ4_WRITER_TYPED_RESTORE_MAX_MACRO_
#endif
#if defined(WVZ4_WRITER_TYPED_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef WVZ4_WRITER_TYPED_RESTORE_MIN_MACRO_
#endif

#endif // WVZ4_WRITER_TYPED_H_
