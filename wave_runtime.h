#pragma once

// Protect this public header from Windows-style min/max function-like macros.
// Some business environments define min/max before including project headers;
// those macros break std::min/std::max and std::numeric_limits<T>::max().
#if defined(min)
#pragma push_macro("min")
#undef min
#define WAVE_RUNTIME_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define WAVE_RUNTIME_RESTORE_MAX_MACRO_ 1
#endif

#ifndef WAVE_RUNTIME_AVAILABLE
#define WAVE_RUNTIME_AVAILABLE 1
#endif

#define WAVE_RUNTIME_NO_ALLOC_TRACKING 1
#define WAVE_RUNTIME_POINTER_WATCH_DISABLED 1

#if defined(new)
#undef new
#endif
#if defined(make_shared)
#undef make_shared
#endif
#if defined(make_unique)
#undef make_unique
#endif


#include "reflect_runtime.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__AVX2__) || defined(_M_AVX2) || defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <immintrin.h>
#endif
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#  include <intrin.h>
#endif
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
#  include <cpuid.h>
#endif

#if defined(WAVE_ENABLE_SYSTEMC)
#  if __has_include(<systemc>)
#    include <systemc>
#    define WAVE_HAS_SYSTEMC 1
#  elif __has_include(<systemc.h>)
#    include <systemc.h>
#    define WAVE_HAS_SYSTEMC 1
#  else
#    define WAVE_HAS_SYSTEMC 0
#  endif
#elif defined(__has_include)
#  if __has_include(<systemc>)
#    include <systemc>
#    define WAVE_HAS_SYSTEMC 1
#  elif __has_include(<systemc.h>)
#    include <systemc.h>
#    define WAVE_HAS_SYSTEMC 1
#  else
#    define WAVE_HAS_SYSTEMC 0
#  endif
#else
#  define WAVE_HAS_SYSTEMC 0
#endif

// Optional custom VSIP-style port forward declarations.
// If your project defines vsipIN/vsipOUT/vsipINOUT in a namespace or with a
// different template signature, define WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS
// before including this header and add your own trait specializations in
// wave::detail.
#ifndef WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS
#  if WAVE_HAS_SYSTEMC
// Project VSIP-style ports use the same shape as sc_port wrappers:
//   vsipIN<T, N, POL>, vsipOUT<T, N, POL>, vsipINOUT<T, N, POL>.
// Do not add default template arguments here, to avoid conflicts with the
// project's real definitions.
template <typename T, int N, sc_core::sc_port_policy POL> class vsipIN;
template <typename T, int N, sc_core::sc_port_policy POL> class vsipOUT;
template <typename T, int N, sc_core::sc_port_policy POL> class vsipINOUT;
// Project VSIP-style interfaces. These are only forward declarations.
template <typename T> class vsiiIN;
template <typename T> class vsiiOUT;
template <typename T> class vsiiINOUT;
#  else
// Fallback declarations for non-SystemC builds. Projects that use a different
// signature can define WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS and specialize the
// traits below themselves.
template <typename T> class vsipIN;
template <typename T> class vsipOUT;
template <typename T> class vsipINOUT;
template <typename T> class vsiiIN;
template <typename T> class vsiiOUT;
template <typename T> class vsiiINOUT;
#  endif
#endif

namespace wave {

using Cycle = std::uint64_t;
using NodeId = std::uint64_t;
using TrackId = std::uint64_t;
using ObjectId = std::uint64_t;
using WatchId = std::uint64_t;

enum class NodeKind {
    Aggregate,
    Leaf,
    PointerLink,
    FixedIndexedContainer,
    ValueSource,
    Unsupported
};

enum class ValueKind {
    Bool,
    SignedInt,
    UnsignedInt,
    Float64,
    StringLike,
    Enum,
    PointerAddress,
    Unsupported,
    Unknown
};

enum class InvalidKind {
    None,
    Z,
    Null,
    Unsupported,
    RecursiveCut
};


enum class FlatMemoryBlockSimdBackend {
    Auto,
    Scalar,
    SSE2,
    AVX2
};

struct ObjectKey {
    const void* address;
    const void* type_tag;

    ObjectKey() : address(NULL), type_tag(NULL) {}
    ObjectKey(const void* a, const void* t) : address(a), type_tag(t) {}

    bool operator==(const ObjectKey& other) const {
        return address == other.address && type_tag == other.type_tag;
    }
};

struct ObjectKeyHash {
    std::size_t operator()(const ObjectKey& key) const {
        const std::size_t h1 = std::hash<const void*>()(key.address);
        const std::size_t h2 = std::hash<const void*>()(key.type_tag);
        return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (h1 << 6) + (h1 >> 2));
    }
};

struct TrackEvent {
    // Hot-path sample event.  Keep this POD-like: no std::string, no owning
    // allocations.  Worker buffers may contain millions of TrackEvent objects
    // per run; owning strings made vector::emplace_back()/clear() dominate CPU.
    Cycle cycle = 0;
    TrackId track_id = 0;
    const char* path = NULL;   // optional debug pointer, owned by Tracer intern tables when set
    const char* value = NULL;  // optional legacy debug string; normally NULL for typed WVZ4 output
    InvalidKind invalid = InvalidKind::None;
    // True means this event is a keep-alive for recorder active-set semantics.
    // The value is unchanged from the last emitted concrete sample, so a
    // recorder may keep the signal active without writing a new value update.
    bool unchanged_keepalive = false;

    // Optional typed payload for binary/typed writers.
    bool has_bool = false;
    bool bool_value = false;
    bool has_i64 = false;
    std::int64_t i64_value = 0;
    bool has_u64 = false;
    std::uint64_t u64_value = 0;
    bool has_f64 = false;
    double f64_value = 0.0;

    // Compact comparison key for typed samples.  When this is true, the tracer
    // can detect unchanged values without formatting/copying legacy strings.
    bool has_change_bits = false;
    std::uint64_t change_bits = 0;
};

struct NodeDecl {
    NodeId node_id = 0;
    NodeId parent_id = 0;
    std::string name;       // one logical segment, e.g. "top", "sm[0]", "warp[3]"
    NodeKind kind = NodeKind::Aggregate;
};

struct TrackDecl {
    TrackId track_id = 0;      // logical signal id / viewer path id
    TrackId storage_id = 0;    // physical storage stream id; 0 means same as track_id
    NodeId node_id = 0;
    std::string path;       // debug/backward-compatible full path; WVZ4 recorder does not store it in file layout.
    ValueKind kind = ValueKind::Unknown;
    std::uint32_t bit_width = 64;
};

class IWaveSink {
public:
    virtual ~IWaveSink() {}
    virtual void on_node_declared(const NodeDecl&) {}
    virtual void on_track_declared(const TrackDecl& decl) = 0;
    virtual void on_sample(const TrackEvent& ev) = 0;
};

class InMemoryWaveSink : public IWaveSink {
public:
    std::vector<NodeDecl> node_declarations;
    std::vector<TrackDecl> declarations;
    std::vector<TrackEvent> events;

    virtual void on_node_declared(const NodeDecl& decl) {
        node_declarations.push_back(decl);
    }

    virtual void on_track_declared(const TrackDecl& decl) {
        declarations.push_back(decl);
    }

    virtual void on_sample(const TrackEvent& ev) {
        events.push_back(ev);
    }
};

struct BuildOptions {
    // Default to transition-style sampling.  This avoids sending unchanged
    // scalar leaves through the recorder/writer on every cycle.  Set false only
    // when a debugging sink really needs a keep-alive event for every track.
    bool emit_only_on_change = true;
    bool emit_pointer_address_track = false;

    // Stable-topology hot path: after prepare_topology() has created all scalar
    // tracks, build a flat leaf table once and sample that POD table instead of
    // walking track ids through TrackDesc/TrackRuntimeState indirection.  If a
    // non-scalar track is ever present, the tracer safely falls back to the
    // legacy path rather than dropping samples.
    bool enable_flat_leaf_fast_table = true;


    // Dirty-peek optimization.  When enabled, value sources that expose a
    // WaveDirtyHook can be removed from the per-cycle polling table.  write()
    // marks a peek address group dirty; sample() then samples only dirty groups.
    // The default is disabled to preserve legacy pull-sampling behavior until
    // business threads call attach_current_thread_for_dirty_peek().
    bool enable_dirty_peek_groups = false;
    bool enable_dirty_peek_parallel_sampling = true;
    std::size_t dirty_peek_parallel_threshold = 64;
    bool dirty_peek_balance_by_leaf_count = true;

    // WaveValue<T> dirty-scalar optimization.  WaveValue<T> is a size-preserving
    // scalar wrapper that reports its own address when modified.  The tracer
    // maps those addresses to dirty scalar groups during topology construction.
    bool enable_wave_value_dirty = true;
    // Build a freeze-time open-addressing lookup table for WaveValue address ->
    // dirty group id.  This keeps WaveValue<T> size-preserving while avoiding a
    // binary search in the assignment hot path.  The sorted vector is retained as
    // a fallback/debug path and for tiny topologies below the threshold.
    bool enable_wave_value_address_hash = true;
    std::size_t wave_value_address_hash_min_entries = 16;

    // wave::array<T,N> dirty-element optimization.  wave::array is a size-preserving
    // wrapper around std::array.  Its non-const operator[] reports the accessed
    // element address; the tracer samples only that element subtree at cycle end.
    bool enable_wave_array_dirty = true;

    // wave::array dirty-element memory-block precheck.  For each dirty array
    // element group, direct scalar leaves under that element are grouped into
    // compact byte ranges before precise leaf sampling.  Nested wave::array
    // elements get their own dirty groups while they are expanded, so the outer
    // element group naturally contains only direct, non-intervening-array leaves.
    // Gaps up to max_gap and mixed scalar types are allowed; the block is used
    // only for changed-byte precheck and never for typed event generation.
    bool enable_wave_array_memory_block_precheck = true;
    std::size_t wave_array_memory_block_max_gap = 7;
    std::size_t wave_array_memory_block_min_leaf_count = 8;
    std::size_t wave_array_memory_block_max_bytes = 4096;
    bool enable_wave_array_memory_block_simd_mask = true;
    std::size_t wave_array_memory_block_simd_min_bytes = 64;
    bool enable_wave_array_memory_block_byte_map = true;
    std::size_t wave_array_memory_block_byte_map_max_bytes = 4096;
    std::size_t wave_array_memory_block_byte_map_max_overhead_per_leaf = 64;


    // Print progress on the same terminal line using '\r'.  This is intentionally
    // independent of debug_log so long runs can show coarse progress without
    // producing large log files.  Set print_cycle_progress=false to disable.
    // Off by default.  Large simulations should not print progress unless the
    // caller explicitly enables it.
    bool print_cycle_progress = false;
    Cycle print_cycle_progress_period = 10;

    bool emit_unsupported_marker = true;
    bool emit_recursive_cut_marker = true;

    // Diagnostic logging for runtime tree construction and invalidation.
    // Keep disabled in long production runs. Enable temporarily when checking why
    // signals become Z or why roots/lazy ports/pointers are not expanded.
    bool debug_log = false;
    bool debug_log_track_samples = false;
    // Track full paths are expensive for very large topologies.  WVZ4 uses
    // node_id + local node names for layout, so the full track path is kept
    // disabled by default.  Enable only for debugging sinks that consume
    // TrackDecl::path / TrackEvent::path.
    bool emit_track_decl_path = false;

    // One-shot topology diagnostic.  When enabled, the tracer dumps leaf
    // distribution after topology expansion is complete: flat poll leaves and
    // dirty-safe peek leaves are reported separately, grouped by node depth.
    bool dump_leaf_distribution_after_topology = false;
    std::string leaf_distribution_dump_path = "wave_leaf_distribution.txt";
    std::uint32_t leaf_distribution_top_n = 50;
    bool leaf_distribution_dirty_safe_only = true;

    std::string debug_log_path = "wave_runtime_debug.log";
    std::size_t debug_log_max_events = 200000;

    // Performance knobs for the first sample-time root expansion. 0 means no
    // explicit reserve. Use these when a root expands to tens/hundreds of
    // thousands of nodes/tracks to avoid repeated unordered_map/vector rehashes.
    std::size_t root_expand_reserve_nodes = 0;
    std::size_t root_expand_reserve_tracks = 0;
    std::size_t root_expand_reserve_objects = 0;
    std::size_t root_expand_reserve_lazy_watches = 0;

    // Optional reserve-only hints for id-indexed topology storage. 0 keeps
    // std::vector's normal growth policy. Do not use resize-based chunk growth
    // here; NodeDesc/TrackDesc tables can be very large.
    std::size_t id_slot_node_growth_chunk = 0;
    std::size_t id_slot_track_growth_chunk = 0;
    std::size_t id_slot_object_growth_chunk = 0;
    std::size_t id_slot_watch_growth_chunk = 0;

    // Emits one begin/end stats line around root expansion. Does not print per-node logs.
    bool debug_log_root_expand_stats = false;

    // Parallel sampling: all declared tracks are assigned to worker slices when
    // enabled. This intentionally includes custom/string/SystemC/VSIP/value-source
    // samplers; use only when these reads are known to be safe in your simulation.
    bool enable_parallel_sampling = false;
    std::size_t sampling_threads = 0;          // 0 = hardware_concurrency() - 1, minimum 1
    std::size_t parallel_sampling_threshold = 8192;
    // Flat-leaf fast path has its own parallel gate because it bypasses the
    // legacy sample_parallel() path. Set to 1 to force parallel flat-leaf
    // sampling whenever at least one leaf exists.
    std::size_t parallel_flat_leaf_threshold = 8192;
    std::size_t parallel_worker_event_reserve = 4096;

    // Diagnostic: log per-thread flat-leaf sampling load and time.  This is
    // intended for checking load imbalance; keep it disabled in production.
    bool log_parallel_flat_leaf_load = false;
    Cycle parallel_flat_leaf_load_log_period = 100;
    std::string parallel_flat_leaf_load_log_path = "wave_parallel_flat_leaf_load.log";

    // Flat memory-block precheck.  After the flat leaf table is built, adjacent
    // leaves with nearby addresses are merged into byte ranges.  This mode no
    // longer requires the leaves to have the same parent node; address
    // monotonicity, max-gap, and max-block-size are still enforced.  Each cycle compares the block shadow first; if
    // the bytes are unchanged, all leaves in that block are skipped.  If bytes
    // changed, only leaves whose byte ranges overlap the changed bytes are
    // sampled precisely.  This is conservative: leaves that cannot be safely
    // grouped remain in the normal scalar flat path.
    bool enable_flat_memory_block_precheck = true;
    std::size_t flat_memory_block_max_gap = 7;
    std::size_t flat_memory_block_min_leaf_count = 8;
    std::size_t flat_memory_block_max_bytes = 4096;
    bool log_flat_memory_block_summary = false;
    // Memory-block changed-byte detection.  This path does not call memcmp();
    // it scans current/shadow bytes once, uses SIMD masks when available,
    // updates the shadow in the same pass, then precisely samples only leaves
    // whose byte ranges overlap changed bytes.
    bool enable_flat_memory_block_simd_mask = true;
    std::size_t flat_memory_block_simd_min_bytes = 64;
    // Auto chooses the best compiled-in backend that the current CPU/OS supports.
    // On MSVC, AVX2 code is only compiled when _M_AVX2 is defined, so /arch:AVX2
    // is required for the AVX2 backend; otherwise Auto falls back to SSE2/scalar.
    FlatMemoryBlockSimdBackend flat_memory_block_simd_backend = FlatMemoryBlockSimdBackend::Auto;

    // Dirty-peek memory-block precheck.  For each dirty peek group, scalar
    // leaves whose sample_ctx addresses are near each other are grouped into
    // byte ranges before precise leaf sampling.  The grouping deliberately
    // allows small gaps and mixed scalar types: the memory block is used only
    // as a conservative changed-byte precheck, while event generation still
    // reads each leaf through its normal typed path.
    bool enable_dirty_peek_memory_block_precheck = true;
    std::size_t dirty_peek_memory_block_max_gap = 7;
    std::size_t dirty_peek_memory_block_min_leaf_count = 8;
    std::size_t dirty_peek_memory_block_max_bytes = 4096;
    bool enable_dirty_peek_memory_block_simd_mask = true;
    std::size_t dirty_peek_memory_block_simd_min_bytes = 64;
    // Fast changed-byte to leaf mapping for dirty-peek memory blocks.  When a
    // block is compact and non-overlapping, build a byte-offset -> leaf-ref
    // table so SIMD/scalar changed ranges can map directly to leaves without
    // per-range binary search.  Gap bytes map to kInvalidIndex.
    bool enable_dirty_peek_memory_block_byte_map = true;
    std::size_t dirty_peek_memory_block_byte_map_max_bytes = 4096;
    std::size_t dirty_peek_memory_block_byte_map_max_overhead_per_leaf = 64;

};

namespace detail {

struct WaveCpuFeatures {
    bool sse2 = false;
    bool avx = false;
    bool avx2 = false;
    bool os_avx = false;
};

inline bool wave_cpuid_(std::uint32_t leaf,
                        std::uint32_t subleaf,
                        std::uint32_t& eax,
                        std::uint32_t& ebx,
                        std::uint32_t& ecx,
                        std::uint32_t& edx) noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    int regs[4] = {0, 0, 0, 0};
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    eax = static_cast<std::uint32_t>(regs[0]);
    ebx = static_cast<std::uint32_t>(regs[1]);
    ecx = static_cast<std::uint32_t>(regs[2]);
    edx = static_cast<std::uint32_t>(regs[3]);
    return true;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
    return true;
#else
    (void)leaf; (void)subleaf;
    eax = ebx = ecx = edx = 0;
    return false;
#endif
}

inline std::uint64_t wave_xgetbv_(std::uint32_t index) noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    return static_cast<std::uint64_t>(_xgetbv(index));
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    std::uint32_t eax = 0;
    std::uint32_t edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return (static_cast<std::uint64_t>(edx) << 32) | eax;
#else
    (void)index;
    return 0;
#endif
}

inline WaveCpuFeatures detect_wave_cpu_features_() noexcept {
    WaveCpuFeatures f;
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!wave_cpuid_(0, 0, eax, ebx, ecx, edx)) return f;
    const std::uint32_t max_leaf = eax;

    if (max_leaf >= 1 && wave_cpuid_(1, 0, eax, ebx, ecx, edx)) {
        f.sse2 = (edx & (1u << 26)) != 0;
        const bool osxsave = (ecx & (1u << 27)) != 0;
        const bool avx_bit = (ecx & (1u << 28)) != 0;
        if (osxsave && avx_bit) {
            const std::uint64_t xcr0 = wave_xgetbv_(0);
            f.os_avx = (xcr0 & 0x6u) == 0x6u;
            f.avx = f.os_avx;
        }
    }

    if (max_leaf >= 7 && f.avx && wave_cpuid_(7, 0, eax, ebx, ecx, edx)) {
        f.avx2 = (ebx & (1u << 5)) != 0;
    }

#  if defined(_M_X64) || defined(__x86_64__)
    // x86-64 ABI requires SSE2, even if CPUID was unavailable or filtered.
    f.sse2 = true;
#  endif
#endif
    return f;
}

inline const WaveCpuFeatures& wave_cpu_features_() noexcept {
    static const WaveCpuFeatures f = detect_wave_cpu_features_();
    return f;
}

inline const char* flat_memory_simd_backend_name(FlatMemoryBlockSimdBackend b) noexcept {
    switch (b) {
    case FlatMemoryBlockSimdBackend::Auto: return "auto";
    case FlatMemoryBlockSimdBackend::Scalar: return "scalar";
    case FlatMemoryBlockSimdBackend::SSE2: return "sse2";
    case FlatMemoryBlockSimdBackend::AVX2: return "avx2";
    }
    return "unknown";
}

inline std::string compiled_flat_memory_simd_backends() {
    std::string out = "scalar";
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    out += ",sse2";
#endif
#if defined(__AVX2__) || defined(_M_AVX2)
    out += ",avx2";
#endif
    return out;
}

inline std::string runtime_flat_memory_simd_features() {
    const WaveCpuFeatures& f = wave_cpu_features_();
    std::ostringstream os;
    os << "sse2=" << (f.sse2 ? 1 : 0)
       << ",avx=" << (f.avx ? 1 : 0)
       << ",os_avx=" << (f.os_avx ? 1 : 0)
       << ",avx2=" << (f.avx2 ? 1 : 0);
    return os.str();
}

inline std::string compose_child_path(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    std::string out;
    out.reserve(parent.size() + child.size() + 1);
    out.append(parent);
    if (!child.empty() && child[0] == '[') {
        out.append(child);
    } else {
        out.push_back('.');
        out.append(child);
    }
    return out;
}

inline std::string compose_index_path(const std::string& parent, std::size_t index) {
    const std::string idx = std::to_string(index);
    std::string out;
    out.reserve(parent.size() + idx.size() + 2);
    out.append(parent);
    out.push_back('[');
    out.append(idx);
    out.push_back(']');
    return out;
}

inline std::string compose_index_child_path(const std::string& parent, std::size_t index) {
    // Used when an array-like object already has its own aggregate node.  The
    // synthetic child local name should be "[i]" under that node rather than
    // repeating "array_name[i]".  The extra dot makes path_last_segment() return
    // "[i]" while leaving the parent path intact for debug output.
    const std::string idx = std::to_string(index);
    std::string out;
    out.reserve(parent.size() + idx.size() + 3);
    out.append(parent);
    out.append(".[");
    out.append(idx);
    out.push_back(']');
    return out;
}

inline std::string path_last_segment(const std::string& path) {
    if (path.empty()) return std::string("root");
    const std::size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) return path;
    if (pos + 1 >= path.size()) return std::string("unnamed");
    return path.substr(pos + 1);
}


// Convert arbitrary object pointers (including pointers to volatile objects) to
// an address identity.  This is used only for bookkeeping/logging/comparison,
// never for dereferencing through the returned void pointer.
template <typename P>
inline const void* pointer_address(P* p) {
    return const_cast<const void*>(static_cast<const volatile void*>(p));
}

inline const void* pointer_address(const void* p) {
    return p;
}

inline const void* pointer_address(void* p) {
    return p;
}

inline const void* pointer_address(std::nullptr_t) {
    return NULL;
}

inline std::string pointer_to_string(const void* p) {
    if (!p) return "null";
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << reinterpret_cast<std::uintptr_t>(p);
    return oss.str();
}

inline std::string to_string_unsigned(std::uint64_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

inline std::string scalar_to_string(const bool& value) {
    return value ? "true" : "false";
}

inline std::string scalar_to_string(const std::string& value) {
    return value;
}

inline std::string scalar_to_string(const char* value) {
    return value ? std::string(value) : std::string("null");
}

inline std::string scalar_to_string(char* value) {
    return value ? std::string(value) : std::string("null");
}

template <typename T>
inline typename std::enable_if<
    std::is_integral<T>::value && std::is_signed<T>::value && !std::is_same<T, bool>::value,
    std::string>::type
scalar_to_string(const T& value) {
    std::ostringstream oss;
    oss << static_cast<long long>(value);
    return oss.str();
}

template <typename T>
inline typename std::enable_if<
    std::is_integral<T>::value && std::is_unsigned<T>::value && !std::is_same<T, bool>::value,
    std::string>::type
scalar_to_string(const T& value) {
    std::ostringstream oss;
    oss << static_cast<unsigned long long>(value);
    return oss.str();
}

template <typename T>
inline typename std::enable_if<std::is_floating_point<T>::value, std::string>::type
scalar_to_string(const T& value) {
    std::ostringstream oss;
    oss << static_cast<long double>(value);
    return oss.str();
}

template <typename T>
inline typename std::enable_if<std::is_enum<T>::value, std::string>::type
scalar_to_string(const T& value) {
    typedef typename std::underlying_type<T>::type Underlying;
    return scalar_to_string(static_cast<Underlying>(value));
}

template <typename T>
inline typename std::enable_if<
    !std::is_integral<T>::value &&
    !std::is_floating_point<T>::value &&
    !std::is_enum<T>::value,
    std::string>::type
scalar_to_string(const T&) {
    return std::string("<unprintable>");
}


inline void assign_typed_value(TrackEvent& ev, const bool& value) {
    ev.has_bool = true;
    ev.bool_value = value;
    ev.has_change_bits = true;
    ev.change_bits = value ? 1u : 0u;
}

template <typename T>
inline typename std::enable_if<
    std::is_integral<T>::value && std::is_signed<T>::value && !std::is_same<T, bool>::value,
    void>::type
assign_typed_value(TrackEvent& ev, const T& value) {
    ev.has_i64 = true;
    ev.i64_value = static_cast<std::int64_t>(value);
    ev.has_u64 = true;
    ev.u64_value = static_cast<std::uint64_t>(ev.i64_value);
    ev.has_change_bits = true;
    ev.change_bits = ev.u64_value;
}

template <typename T>
inline typename std::enable_if<
    std::is_integral<T>::value && std::is_unsigned<T>::value,
    void>::type
assign_typed_value(TrackEvent& ev, const T& value) {
    ev.has_u64 = true;
    ev.u64_value = static_cast<std::uint64_t>(value);
    ev.has_change_bits = true;
    ev.change_bits = ev.u64_value;
}

template <typename T>
inline typename std::enable_if<std::is_enum<T>::value, void>::type
assign_typed_value(TrackEvent& ev, const T& value) {
    typedef typename std::underlying_type<T>::type Underlying;
    assign_typed_value(ev, static_cast<Underlying>(value));
}

template <typename T>
inline typename std::enable_if<std::is_floating_point<T>::value, void>::type
assign_typed_value(TrackEvent& ev, const T& value) {
    ev.has_f64 = true;
    ev.f64_value = static_cast<double>(value);
    ev.has_change_bits = true;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &ev.f64_value, sizeof(bits));
    ev.change_bits = bits;
}

inline void assign_typed_value(TrackEvent&, const std::string&) {}
inline void assign_typed_value(TrackEvent&, const char*) {}

template <typename T>
struct remove_cvref {
    typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type type;
};

// Optional compile-time type dump. Define WAVE_DEBUG_EXPAND_MEMBER_TYPE_DUMP
// to force the compiler to print the exact reflected member pointer type P
// and cleaned FieldT at expand_member_ptr() instantiation sites.
template <typename T>
struct debug_type_dump;

// C++14 priority tag dispatch helper.  Higher N wins; lower N is used only
// when the higher-priority overload is removed by SFINAE.
template <int N>
struct priority_tag : priority_tag<N - 1> {};

template <>
struct priority_tag<0> {};

template <typename T, typename = void>
struct is_complete_type : std::false_type {};

template <typename T>
struct is_complete_type<T, decltype(void(sizeof(T)))> : std::true_type {};

// Pointee marker detection.  The marker itself is declared in reflect_runtime.h
// as wave::DirectReflectPointerTarget so business code can opt in by inheriting
// from it.  We only evaluate std::is_base_of for complete non-void types; for
// incomplete pointee types the safe answer is false.
template <typename T, bool Complete = is_complete_type<typename remove_cvref<T>::type>::value>
struct is_direct_reflect_pointer_target_impl : std::false_type {};

template <typename T>
struct is_direct_reflect_pointer_target_impl<T, true> : std::integral_constant<bool,
    !std::is_void<typename remove_cvref<T>::type>::value &&
    std::is_base_of< ::wave::DirectReflectPointerTarget, typename remove_cvref<T>::type>::value> {};

template <typename T>
struct is_direct_reflect_pointer_target : is_direct_reflect_pointer_target_impl<T> {};

template <typename T>
struct is_std_string : std::false_type {};

template <>
struct is_std_string<std::string> : std::true_type {};

template <typename T>
struct is_std_array : std::false_type {};

template <typename T, std::size_t N>
struct is_std_array<std::array<T, N> > : std::true_type {};

template <typename T>
struct is_std_pair : std::false_type {};

template <typename A, typename B>
struct is_std_pair<std::pair<A, B> > : std::true_type {};

template <typename T>
struct is_unique_ptr_scalar : std::false_type {};

template <typename T, typename D>
struct is_unique_ptr_scalar<std::unique_ptr<T, D> > : std::integral_constant<bool, !std::is_array<T>::value> {};

template <typename T>
struct is_unique_ptr_array : std::false_type {};

template <typename T, typename D>
struct is_unique_ptr_array<std::unique_ptr<T, D> > : std::integral_constant<bool, std::is_array<T>::value> {};

template <typename T>
struct unique_ptr_scalar_target;

template <typename T, typename D>
struct unique_ptr_scalar_target<std::unique_ptr<T, D> > { typedef T type; };

template <typename T>
struct unique_ptr_array_element;

template <typename T, typename D>
struct unique_ptr_array_element<std::unique_ptr<T, D> > {
    typedef typename std::remove_extent<T>::type type;
};

template <typename T>
struct is_shared_ptr : std::false_type {};

template <typename T>
struct is_shared_ptr<std::shared_ptr<T> > : std::true_type {};

template <typename T>
struct shared_ptr_target;

template <typename T>
struct shared_ptr_target<std::shared_ptr<T> > { typedef T type; };

template <typename T>
struct is_weak_ptr : std::false_type {};

template <typename T>
struct is_weak_ptr<std::weak_ptr<T> > : std::true_type {};

template <typename T>
struct weak_ptr_target;

template <typename T>
struct weak_ptr_target<std::weak_ptr<T> > { typedef T type; };

// VSIP-style ports/interfaces are treated as peek value sources.
//
// There are two supported cases:
//   1. Exact project port wrappers:
//        vsipIN<T,N,POL>, vsipOUT<T,N,POL>, vsipINOUT<T,N,POL>
//      These are sc_port-like objects.  Their sampled value is obtained through
//      the bound IF, i.e. port->peek().  Tracer receives const Port* and calls
//      read_vsip_peek(...), which uses operator->().
//
//   2. Project channel/interface classes whose inheritance chain contains one
//      of the IF templates:
//        vsiiIN<T>, vsiiOUT<T>, vsiiINOUT<T>
//      These are treated as value sources too, but their sampled value is
//      obtained by calling peek() through the vsii* base template, not by
//      guessing a non-template interface and not by checking vsip* inheritance.
//      This is important because vsiiIN/OUT/INOUT are templates.
//
// Exact vsip* ports use port->peek(); classes derived from vsii* use .peek()
// through the appropriate vsii<T> base.  Classes merely derived from vsip* are
// not treated as direct-dot peek wrappers.
template <typename T>
struct type_identity { typedef T type; };

template <typename T>
struct is_vsip_in : std::false_type {};

template <typename T>
struct is_vsip_out : std::false_type {};

template <typename T>
struct is_vsip_inout : std::false_type {};

#ifndef WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS
// Inheritance-based value-source detection is intentionally tied to the
// interface templates, not the port templates.  vsiiIN/OUT/INOUT are templates:
//   vsiiIN<T>, vsiiOUT<T>, vsiiINOUT<T>
// The overloads below recover T when CleanT is derived from one of them.
template <typename V>
type_identity<V> infer_vsii_in_base(const ::vsiiIN<V>*);

template <typename V>
type_identity<V> infer_vsii_out_base(const ::vsiiOUT<V>*);

template <typename V>
type_identity<V> infer_vsii_inout_base(const ::vsiiINOUT<V>*);
#endif

type_identity<void> infer_vsii_in_base(...);
type_identity<void> infer_vsii_out_base(...);
type_identity<void> infer_vsii_inout_base(...);

template <typename T>
struct inferred_vsip_value_type {
    typedef typename remove_cvref<T>::type CleanT;
    typedef typename decltype(infer_vsii_inout_base(static_cast<const CleanT*>(NULL)))::type InOutValue;
    typedef typename decltype(infer_vsii_in_base(static_cast<const CleanT*>(NULL)))::type InValue;
    typedef typename decltype(infer_vsii_out_base(static_cast<const CleanT*>(NULL)))::type OutValue;

    typedef typename std::conditional<
        !std::is_void<InOutValue>::value,
        InOutValue,
        typename std::conditional<
            !std::is_void<InValue>::value,
            InValue,
            OutValue
        >::type
    >::type type;
};

template <typename T>
struct vsip_port_value_type {
    typedef typename inferred_vsip_value_type<T>::type type;
};

#ifndef WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS
#  if WAVE_HAS_SYSTEMC
template <typename T, int N, sc_core::sc_port_policy POL>
struct is_vsip_in< ::vsipIN<T, N, POL> > : std::true_type {};

template <typename T, int N, sc_core::sc_port_policy POL>
struct is_vsip_out< ::vsipOUT<T, N, POL> > : std::true_type {};

template <typename T, int N, sc_core::sc_port_policy POL>
struct is_vsip_inout< ::vsipINOUT<T, N, POL> > : std::true_type {};

template <typename T, int N, sc_core::sc_port_policy POL>
struct vsip_port_value_type< ::vsipIN<T, N, POL> > { typedef T type; };

template <typename T, int N, sc_core::sc_port_policy POL>
struct vsip_port_value_type< ::vsipOUT<T, N, POL> > { typedef T type; };

template <typename T, int N, sc_core::sc_port_policy POL>
struct vsip_port_value_type< ::vsipINOUT<T, N, POL> > { typedef T type; };
#  else
template <typename T>
struct is_vsip_in< ::vsipIN<T> > : std::true_type {};

template <typename T>
struct is_vsip_out< ::vsipOUT<T> > : std::true_type {};

template <typename T>
struct is_vsip_inout< ::vsipINOUT<T> > : std::true_type {};

template <typename T>
struct vsip_port_value_type< ::vsipIN<T> > { typedef T type; };

template <typename T>
struct vsip_port_value_type< ::vsipOUT<T> > { typedef T type; };

template <typename T>
struct vsip_port_value_type< ::vsipINOUT<T> > { typedef T type; };
#  endif
#endif

template <typename T>
struct is_vsip_exact_port : std::integral_constant<bool,
    is_vsip_in<typename remove_cvref<T>::type>::value ||
    is_vsip_out<typename remove_cvref<T>::type>::value ||
    is_vsip_inout<typename remove_cvref<T>::type>::value> {};

template <typename T>
struct is_vsip_peek_port : std::integral_constant<bool,
    !std::is_void<typename vsip_port_value_type<typename remove_cvref<T>::type>::type>::value> {};

// Backward-compatible internal alias name: older code paths used
// is_vsip_read_port. The behavior is now peek()-based.
template <typename T>
struct is_vsip_read_port : is_vsip_peek_port<T> {};

// STL blacklist (except string/array/pair and smart pointers)
template <typename T>
struct is_blacklisted_stl : std::false_type {};

template <typename T, typename A>
struct is_blacklisted_stl<std::vector<T, A> > : std::true_type {};

template <typename T, typename A>
struct is_blacklisted_stl<std::deque<T, A> > : std::true_type {};

template <typename T, typename A>
struct is_blacklisted_stl<std::list<T, A> > : std::true_type {};

template <typename T, typename A>
struct is_blacklisted_stl<std::forward_list<T, A> > : std::true_type {};

template <typename K, typename C, typename A>
struct is_blacklisted_stl<std::set<K, C, A> > : std::true_type {};

template <typename K, typename C, typename A>
struct is_blacklisted_stl<std::multiset<K, C, A> > : std::true_type {};

template <typename K, typename H, typename E, typename A>
struct is_blacklisted_stl<std::unordered_set<K, H, E, A> > : std::true_type {};

template <typename K, typename H, typename E, typename A>
struct is_blacklisted_stl<std::unordered_multiset<K, H, E, A> > : std::true_type {};

template <typename K, typename V, typename C, typename A>
struct is_blacklisted_stl<std::map<K, V, C, A> > : std::true_type {};

template <typename K, typename V, typename C, typename A>
struct is_blacklisted_stl<std::multimap<K, V, C, A> > : std::true_type {};

template <typename K, typename V, typename H, typename E, typename A>
struct is_blacklisted_stl<std::unordered_map<K, V, H, E, A> > : std::true_type {};

template <typename K, typename V, typename H, typename E, typename A>
struct is_blacklisted_stl<std::unordered_multimap<K, V, H, E, A> > : std::true_type {};

template <typename T, typename C>
struct is_blacklisted_stl<std::queue<T, C> > : std::true_type {};

template <typename T, typename C>
struct is_blacklisted_stl<std::stack<T, C> > : std::true_type {};

template <typename T, typename C, typename Comp>
struct is_blacklisted_stl<std::priority_queue<T, C, Comp> > : std::true_type {};

#if WAVE_HAS_SYSTEMC

template <typename T>
struct is_sc_in : std::false_type {};

template <typename T>
struct is_sc_in<sc_core::sc_in<T> > : std::true_type {};

template <typename T>
struct is_sc_out : std::false_type {};

template <typename T>
struct is_sc_out<sc_core::sc_out<T> > : std::true_type {};

template <typename T>
struct is_sc_inout : std::false_type {};

template <typename T>
struct is_sc_inout<sc_core::sc_inout<T> > : std::true_type {};

template <typename T>
struct sc_port_value_type;

template <typename T>
struct sc_port_value_type<sc_core::sc_in<T> > { typedef T type; };

template <typename T>
struct sc_port_value_type<sc_core::sc_out<T> > { typedef T type; };

template <typename T>
struct sc_port_value_type<sc_core::sc_inout<T> > { typedef T type; };

template <typename T>
struct is_sc_signal : std::false_type {};

template <typename T, sc_core::sc_writer_policy POL>
struct is_sc_signal<sc_core::sc_signal<T, POL> > : std::true_type {};

template <typename T>
struct is_sc_buffer : std::false_type {};

template <typename T, sc_core::sc_writer_policy POL>
struct is_sc_buffer<sc_core::sc_buffer<T, POL> > : std::true_type {};

template <typename T>
struct sc_signal_value_type;

template <typename T, sc_core::sc_writer_policy POL>
struct sc_signal_value_type<sc_core::sc_signal<T, POL> > { typedef T type; };

template <typename T, sc_core::sc_writer_policy POL>
struct sc_signal_value_type<sc_core::sc_buffer<T, POL> > { typedef T type; };

template <typename T>
struct is_sc_vector : std::false_type {};

template <typename T>
struct is_sc_vector<sc_core::sc_vector<T> > : std::true_type {};

template <typename T>
struct is_sc_clock : std::false_type {};

template <>
struct is_sc_clock<sc_core::sc_clock> : std::true_type {};

constexpr bool wave_cstr_contains(const char* haystack, const char* needle)
{
    if (!needle || !*needle) return true;
    if (!haystack || !*haystack) return false;

    for (const char* h = haystack; *h; ++h)
    {
        const char* h2 = h;
        const char* n2 = needle;
        while (*h2 && *n2 && *h2 == *n2)
        {
            ++h2;
            ++n2;
        }
        if (!*n2) return true;
    }
    return false;
}

template <typename T>
struct sc_namespace_blacklisted_eval
{
    static constexpr bool compute()
    {
#if defined(_MSC_VER)
        return wave_cstr_contains(__FUNCSIG__, "sc_core::");
#else
        return wave_cstr_contains(__PRETTY_FUNCTION__, "sc_core::");
#endif
    }
};

template <typename T>
struct is_sc_namespace_blacklisted : std::integral_constant<bool, sc_namespace_blacklisted_eval<T>::compute()> {};


#else

template <typename T> struct is_sc_in : std::false_type {};
template <typename T> struct is_sc_out : std::false_type {};
template <typename T> struct is_sc_inout : std::false_type {};
template <typename T> struct is_sc_signal : std::false_type {};
template <typename T> struct is_sc_buffer : std::false_type {};
template <typename T> struct is_sc_vector : std::false_type {};
template <typename T> struct is_sc_clock : std::false_type {};
template <typename T> struct is_sc_namespace_blacklisted : std::false_type {};

template <typename T> struct sc_port_value_type { typedef void type; };
template <typename T> struct sc_signal_value_type { typedef void type; };

#endif

template <typename T>
struct is_leaf_scalar : std::integral_constant<bool,
    std::is_arithmetic<T>::value ||
    std::is_enum<T>::value ||
    is_std_string<T>::value
> {};

template <typename P>
struct is_volatile_leaf_member_ptr : std::false_type {};

template <typename RawT>
struct is_volatile_leaf_member_ptr<RawT*> : std::integral_constant<bool,
    std::is_volatile<RawT>::value &&
    is_leaf_scalar<typename remove_cvref<RawT>::type>::value &&
    !is_std_string<typename remove_cvref<RawT>::type>::value
> {};


template <typename P>
struct is_c_string_member_ptr : std::false_type {};

template <typename RawT>
struct is_c_string_member_ptr<RawT*> : std::integral_constant<bool,
    std::is_same<typename remove_cvref<RawT>::type, char*>::value ||
    std::is_same<typename remove_cvref<RawT>::type, const char*>::value> {};

template <typename T>
inline ValueKind classify_value_kind() {
    if (std::is_same<T, bool>::value) return ValueKind::Bool;
    if (std::is_integral<T>::value && std::is_signed<T>::value && !std::is_same<T, bool>::value) return ValueKind::SignedInt;
    if (std::is_integral<T>::value && std::is_unsigned<T>::value) return ValueKind::UnsignedInt;
    if (std::is_floating_point<T>::value) return ValueKind::Float64;
    if (std::is_enum<T>::value) return ValueKind::Enum;
    if (is_std_string<T>::value) return ValueKind::StringLike;
    return ValueKind::Unknown;
}


template <typename T, typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
auto call_reflected_visit_impl(const T* ptr, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&& on_getter, int)
    -> decltype(reflect::reflected_visitor<T>::visit(ptr, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value), std::forward<GetterVisitor>(on_getter)), void())
{
    reflect::reflected_visitor<T>::visit(ptr, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value), std::forward<GetterVisitor>(on_getter));
}

template <typename T, typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
void call_reflected_visit_impl(const T* ptr, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&&, long)
{
    reflect::reflected_visitor<T>::visit(ptr, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value));
}

template <typename T, typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>
void call_reflected_visit(const T* ptr, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&& on_getter)
{
    call_reflected_visit_impl<T>(ptr, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value), std::forward<GetterVisitor>(on_getter), 0);
}

} // namespace detail

class Tracer;



struct ThreadTraceLocal;

class WaveTlsRegistry {
public:
    static WaveTlsRegistry& instance() {
        static WaveTlsRegistry* registry = new WaveTlsRegistry();
        return *registry;
    }

    void register_tls(ThreadTraceLocal* tls) noexcept {
        if (!tls) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (std::find(slots_.begin(), slots_.end(), tls) == slots_.end()) {
            slots_.push_back(tls);
        }
    }

    void unregister_tls(ThreadTraceLocal* tls) noexcept {
        if (!tls) return;
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<ThreadTraceLocal*>::iterator it = std::find(slots_.begin(), slots_.end(), tls);
        if (it != slots_.end()) slots_.erase(it);
    }

    template <typename Fn>
    void for_each_tls_locked(Fn fn) {
        std::lock_guard<std::mutex> lock(mu_);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            ThreadTraceLocal* tls = slots_[i];
            if (tls) fn(tls);
        }
    }

private:
    WaveTlsRegistry() {}
    WaveTlsRegistry(const WaveTlsRegistry&) = delete;
    WaveTlsRegistry& operator=(const WaveTlsRegistry&) = delete;

    std::mutex mu_;
    std::vector<ThreadTraceLocal*> slots_;
};

struct ThreadTraceLocal {
    ThreadTraceLocal() noexcept {
        WaveTlsRegistry::instance().register_tls(this);
    }

    ~ThreadTraceLocal() noexcept;


    Tracer* owner = NULL;

    // Dirty peek groups.
    std::vector<std::uint32_t> dirty_ids;      // stores DirtyPeekGroup ids
    std::vector<std::uint32_t> local_epoch;    // indexed by DirtyPeekGroup id
    std::uint32_t dirty_count = 0;

    // Dirty WaveValue scalar groups.
    std::vector<std::uint32_t> wave_value_dirty_ids;
    std::vector<std::uint32_t> wave_value_local_epoch;
    std::uint32_t wave_value_dirty_count = 0;

    // Dirty wave::array element groups.
    std::vector<std::uint32_t> wave_array_dirty_ids;
    std::vector<std::uint32_t> wave_array_local_epoch;
    std::uint32_t wave_array_dirty_count = 0;

    void ensure_capacity(std::size_t dirty_group_count) {
        if (dirty_ids.size() < dirty_group_count) dirty_ids.resize(dirty_group_count);
        if (local_epoch.size() < dirty_group_count) local_epoch.resize(dirty_group_count, 0);
    }

    void ensure_wave_value_capacity(std::size_t dirty_group_count) {
        if (wave_value_dirty_ids.size() < dirty_group_count) wave_value_dirty_ids.resize(dirty_group_count);
        if (wave_value_local_epoch.size() < dirty_group_count) wave_value_local_epoch.resize(dirty_group_count, 0);
    }

    void attach(Tracer* tracer, std::size_t dirty_group_count, std::uint32_t epoch) {
        owner = tracer;
        ensure_capacity(dirty_group_count);

        // attach() is not a per-cycle hot path.  Reset the TLS epoch cache every
        // time it is attached so stale values from an earlier Tracer instance, or
        // from a previously destroyed Tracer that reused the same stack address,
        // cannot suppress the first dirty mark of the new attachment.
        std::fill(local_epoch.begin(), local_epoch.end(), 0);

        dirty_count = 0;
        (void)epoch;
    }

    void attach_wave_values(Tracer* tracer, std::size_t dirty_group_count, std::uint32_t epoch) {
        owner = tracer;
        ensure_wave_value_capacity(dirty_group_count);
        std::fill(wave_value_local_epoch.begin(), wave_value_local_epoch.end(), 0);
        wave_value_dirty_count = 0;
        (void)epoch;
    }

    void ensure_wave_array_capacity(std::size_t dirty_group_count) {
        if (wave_array_dirty_ids.size() < dirty_group_count) wave_array_dirty_ids.resize(dirty_group_count);
        if (wave_array_local_epoch.size() < dirty_group_count) wave_array_local_epoch.resize(dirty_group_count, 0);
    }

    void attach_wave_arrays(Tracer* tracer, std::size_t dirty_group_count, std::uint32_t epoch) {
        owner = tracer;
        ensure_wave_array_capacity(dirty_group_count);
        std::fill(wave_array_local_epoch.begin(), wave_array_local_epoch.end(), 0);
        wave_array_dirty_count = 0;
        (void)epoch;
    }

    void detach(Tracer* tracer) noexcept {
        if (owner == tracer) {
            owner = NULL;
            dirty_count = 0;
            wave_value_dirty_count = 0;
            wave_array_dirty_count = 0;
        }
    }
};

inline ThreadTraceLocal& current_thread_trace_local() {
    thread_local ThreadTraceLocal tls;
    return tls;
}

typedef NodeId (*DynamicExpandFn)(Tracer&, const std::string&, NodeId, const void*);

inline std::unordered_map<const void*, DynamicExpandFn>& dynamic_expand_registry() {
    static std::unordered_map<const void*, DynamicExpandFn>* registry = new std::unordered_map<const void*, DynamicExpandFn>();
    return *registry;
}

inline void register_dynamic_expander(const void* type_tag, DynamicExpandFn fn) {
    if (!type_tag || !fn) return;
    dynamic_expand_registry()[type_tag] = fn;
}

inline DynamicExpandFn find_dynamic_expander(const void* type_tag) {
    std::unordered_map<const void*, DynamicExpandFn>& registry = dynamic_expand_registry();
    std::unordered_map<const void*, DynamicExpandFn>::const_iterator it = registry.find(type_tag);
    return it == registry.end() ? static_cast<DynamicExpandFn>(NULL) : it->second;
}

template <typename T>
NodeId dynamic_expand_bridge(Tracer& tracer, const std::string& path, NodeId parent_id, const void* obj);

template <typename T>
struct DynamicTypeRegistration {
    DynamicTypeRegistration() {
        register_dynamic_expander(reflect::type_tag_of<T>(), &dynamic_expand_bridge<T>);
    }
};



namespace detail {

template <typename T>
class has_wave_dirty_hook {
    template <typename U>
    static auto test(int) -> decltype(std::declval<U*>()->wave_dirty_hook(), std::true_type());
    template <typename>
    static std::false_type test(...);
public:
    static const bool value = decltype(test<typename std::remove_const<T>::type>(0))::value;
};

template <typename T>
inline typename std::enable_if<has_wave_dirty_hook<T>::value, WaveDirtyHook*>::type
maybe_wave_dirty_hook(const T* obj) {
    return obj ? const_cast<typename std::remove_const<T>::type*>(obj)->wave_dirty_hook() : static_cast<WaveDirtyHook*>(NULL);
}

template <typename T>
inline typename std::enable_if<!has_wave_dirty_hook<T>::value, WaveDirtyHook*>::type
maybe_wave_dirty_hook(const T*) {
    return static_cast<WaveDirtyHook*>(NULL);
}

} // namespace detail

struct TrackDesc;

enum class TrackThreadClass {
    ParallelSafe,
    MainThreadOnly
};

enum class ScalarSampleKind : unsigned char {
    None = 0,
    Bool,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    FLongDouble
};

struct ScalarSnapshot {
    ScalarSampleKind sample_kind;
    std::uint64_t bits;
    bool bool_value;
    std::int64_t i64_value;
    std::uint64_t u64_value;
    double f64_value;

    ScalarSnapshot()
        : sample_kind(ScalarSampleKind::None), bits(0), bool_value(false),
          i64_value(0), u64_value(0), f64_value(0.0) {}
};

typedef bool (*ScalarReadFn)(const void*, ScalarSnapshot&);

struct TrackDesc {
    TrackId id;
    NodeId node_id;
    TrackId next_in_node;
    std::uint32_t path_id;
    ValueKind kind;
    const void* sample_ctx;
    ScalarReadFn scalar_reader;
    ScalarSampleKind scalar_kind;
    std::uint32_t bit_width;
    TrackThreadClass thread_class;
    TrackId storage_id;
    const void* memory_ctx;
    std::uint32_t memory_byte_width;
    std::uint32_t dirty_peek_group_id;
    std::uint32_t dirty_wave_value_group_id;
    std::uint32_t dirty_wave_array_group_id;

    TrackDesc() : id(0), node_id(0), next_in_node(0), path_id(kInvalidIndex),
                  kind(ValueKind::Unknown), sample_ctx(NULL), scalar_reader(NULL),
                  scalar_kind(ScalarSampleKind::None), bit_width(0),
                  thread_class(TrackThreadClass::MainThreadOnly), storage_id(0),
                  memory_ctx(NULL), memory_byte_width(0),
                  dirty_peek_group_id(kInvalidIndex),
                  dirty_wave_value_group_id(kInvalidIndex),
                  dirty_wave_array_group_id(kInvalidIndex) {}
};

struct TrackRuntimeState {
    bool alive;
    bool has_last_event;
    InvalidKind last_invalid;
    bool last_has_change_bits;
    std::uint64_t last_change_bits;
    std::uint32_t last_value_id;
    // Per-sample dirty-peek physical-storage de-dup stamp.  This is separate
    // from logical track liveness: several live logical tracks may alias one
    // physical storage stream.
    std::uint32_t dirty_peek_storage_sample_epoch;

    TrackRuntimeState() : alive(true), has_last_event(false), last_invalid(InvalidKind::None),
                          last_has_change_bits(false), last_change_bits(0),
                          last_value_id(kInvalidIndex),
                          dirty_peek_storage_sample_epoch(0) {}
};

struct NodeDesc {
    NodeId id;
    NodeId parent_id;
    NodeId first_child;
    NodeId next_sibling;
    TrackId first_track;
    ObjectId object_id;
    std::uint32_t name_id;
    std::uint32_t debug_path_id;
    NodeKind kind;
    bool alive;

    NodeDesc() : id(0), parent_id(0), first_child(0), next_sibling(0), first_track(0),
                 object_id(0), name_id(kInvalidIndex), debug_path_id(kInvalidIndex),
                 kind(NodeKind::Aggregate), alive(true) {}
};

struct ObjectEntry {
    ObjectId id;
    ObjectKey key;
    bool alive;
    std::uint32_t debug_name_id;
    ObjectEntry() : id(0), alive(true), debug_name_id(kInvalidIndex) {}
};


// Lazy value watch for non-scalar port-like objects.
// add_root()/tree construction must not call sc_port::operator->(), read(), or
// project-specific peek(), because SystemC binding may not have completed yet.
// This watch defers the first dereference until sample(), and rebuilds the
// subtree if the returned value address changes.
struct LazyValueWatch {
    WatchId id;
    NodeId parent_id;
    NodeId target_subtree_root_id;
    bool alive;
    const void* last_value_address;
    std::string path;
    std::function<const void*()> read_value_address;
    std::function<NodeId(const void*)> expand_value;

    LazyValueWatch()
        : id(0), parent_id(0), target_subtree_root_id(0), alive(true),
          last_value_address(NULL) {}
};

// RootWatch makes add_root() side-effect-free. add_root() only records a root
// request; the actual reflection tree is built at the beginning of sample().
// This avoids touching SystemC ports/channels during elaboration-time setup.
struct RootWatch {
    WatchId id;
    NodeId root_node_id;
    bool expanded;
    bool alive;
    std::string path;
    const void* address;
    ObjectKey key;
    std::function<NodeId()> expand_root;
    std::string root_type_name;
    bool root_is_reflected;
    bool root_is_leaf_scalar;
    bool root_is_wave_value;
    bool root_is_wave_array;
    bool root_is_pointer_or_smart;
    bool root_is_blacklisted_stl;

    RootWatch()
        : id(0), root_node_id(0), expanded(false), alive(true),
          address(NULL), key(), root_is_reflected(false), root_is_leaf_scalar(false),
          root_is_wave_value(false), root_is_wave_array(false), root_is_pointer_or_smart(false),
          root_is_blacklisted_stl(false) {}
};

struct ExpandingGuard {
    std::vector<ObjectKey>& active_stack;
    ObjectKey key;
    bool engaged;

    ExpandingGuard(std::vector<ObjectKey>& s, const ObjectKey& k)
        : active_stack(s), key(k), engaged(true) {
        active_stack.push_back(key);
    }

    ~ExpandingGuard() {
        if (!engaged) return;
        for (std::vector<ObjectKey>::reverse_iterator it = active_stack.rbegin();
             it != active_stack.rend(); ++it) {
            if (*it == key) {
                active_stack.erase((it + 1).base());
                break;
            }
        }
    }
};

class Tracer {
public:
    explicit Tracer(IWaveSink& sink, const BuildOptions& options = BuildOptions())
        : sink_(sink), options_(options), debug_log_event_count_(0),
          next_node_id_(1), next_track_id_(1), next_object_id_(1), next_watch_id_(1),
          nodes_exported_(false), nodes_(1), tracks_(1), track_runtime_(1), objects_(1), lazy_value_watches_(1), root_watches_(1),
          flat_leaf_fast_table_valid_(false), flat_leaf_fast_table_complete_(false) {
        ::wave::detail::set_wave_value_notify_fn(&Tracer::wave_value_notify_bridge_);
        ::wave::detail::set_wave_array_index_notify_fn(&Tracer::wave_array_index_notify_bridge_);
        register_active_tracer_(this);
        open_debug_log();
        open_parallel_flat_leaf_load_log_();
        if (options_.debug_log) debug_log_msg(std::string("construct tracer"));
    }

    ~Tracer() {
        unregister_active_tracer_(this);
        stop_parallel_workers();
        detach_all_tls_for_this_tracer_();
        clear_bound_dirty_hooks();
    }

    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;

    template <typename T>
    NodeId add_root(const std::string& name, T* root) {
        // add_root() must not recursively expand the object immediately. In a
        // SystemC model, ports may be syntactically bound but not yet safe to
        // dereference before sc_start()/initialization. Store a lazy root and
        // build it at the beginning of sample().
        if (!root) {
            if (options_.debug_log) debug_log_msg(std::string("add_root skip null path=") + name);
            return 0;
        }

        typedef typename detail::remove_cvref<T>::type CleanT;
        const void* address = detail::pointer_address(root);
        if (!address) {
            if (options_.debug_log) debug_log_msg(std::string("add_root skip null-address path=") + name);
            return 0;
        }

        if (root_paths_.find(name) != root_paths_.end()) {
            if (options_.debug_log) debug_log_msg(std::string("add_root skip duplicate-path path=") + name);
            return 0;
        }
        if (root_addresses_.find(address) != root_addresses_.end()) {
            if (options_.debug_log) debug_log_msg(std::string("add_root skip duplicate-address path=") + name +
                          " addr=" + detail::pointer_to_string(address));
            return 0;
        }

        RootWatch watch;
        watch.id = next_watch_id_++;
        if (options_.debug_log || options_.debug_log_root_expand_stats) watch.path = name;
        // Always keep enough root type metadata for WaveTap runtime diagnostics.
        // This is intentionally cheap and helps distinguish "root pointer exists"
        // from "the static root type is not reflected / unsupported in this TU".
        watch.path = name;
        watch.root_type_name = typeid(CleanT).name();
        watch.root_is_reflected = reflect::is_reflected<CleanT>::value;
        watch.root_is_leaf_scalar = detail::is_leaf_scalar<CleanT>::value;
        watch.root_is_wave_value = detail::is_wave_value<CleanT>::value;
        watch.root_is_wave_array = detail::is_wave_array<CleanT>::value;
        watch.root_is_pointer_or_smart = std::is_pointer<CleanT>::value ||
                                        detail::is_unique_ptr_scalar<CleanT>::value ||
                                        detail::is_unique_ptr_array<CleanT>::value ||
                                        detail::is_shared_ptr<CleanT>::value ||
                                        detail::is_weak_ptr<CleanT>::value;
        watch.root_is_blacklisted_stl = detail::is_blacklisted_stl<CleanT>::value;
        watch.address = address;
        watch.key = ObjectKey(address, reflect::type_tag_of<CleanT>());
        watch.expand_root = [this, name, root]() -> NodeId {
            typedef typename detail::remove_cvref<T>::type RootT;
            const ObjectKey key(detail::pointer_address(root), reflect::type_tag_of<RootT>());

            // No allocation tracking / pointer folding: root de-dup is handled
            // by add_root() path/address checks. Expand this root once.
            return expand_field(name, 0, static_cast<const RootT*>(root));
        };

        root_paths_.insert(name);
        root_addresses_.insert(address);
        leaf_distribution_dumped_ = false;
        // A previous lazy prepare may have exported an empty/no-root topology.
        // Adding a root must reopen the node-declaration export gate so the
        // next prepare_topology() can actually emit NodeDecl records.  WaveTap
        // rejects late add_root after topology freeze; direct Tracer users that
        // add roots after writer open will still be rejected by the recorder.
        nodes_exported_ = false;
        ensure_id_slot_reserved(root_watches_, watch.id, options_.id_slot_watch_growth_chunk) = watch;
        if (options_.debug_log) debug_log_msg(std::string("add_root registered id=") + detail::to_string_unsigned(watch.id) +
                      " path=" + name + " addr=" + detail::pointer_to_string(address));
        return 0;
    }

    void maybe_print_cycle_progress(Cycle cycle) const noexcept {
        if (!options_.print_cycle_progress) return;
        if (options_.print_cycle_progress_period == 0) return;
        if ((cycle % options_.print_cycle_progress_period) != 0) return;
        std::fprintf(stderr, "\r[wave] cycle=%llu", static_cast<unsigned long long>(cycle));
        std::fflush(stderr);
    }

    void sample(Cycle cycle) {
        maybe_print_cycle_progress(cycle);
        if (options_.debug_log) debug_log_msg(std::string("sample begin cycle=") + detail::to_string_unsigned(cycle));
        prepare_topology(cycle);

        if (options_.enable_dirty_peek_groups) {
            collect_dirty_peek_groups_from_tls();
        }
        if (options_.enable_wave_value_dirty) {
            collect_dirty_wave_value_groups_from_tls();
        }
        if (options_.enable_wave_array_dirty) {
            collect_dirty_wave_array_groups_from_tls();
        }

        if (options_.enable_flat_leaf_fast_table && ensure_flat_leaf_fast_table()) {
            sample_flat_leaf_fast(cycle);
        } else if (should_use_parallel_sampling()) {
            sample_parallel(cycle);
        } else {
            sample_serial(cycle);
        }

        if (options_.enable_dirty_peek_groups) {
            sample_dirty_peek_groups(cycle);
        }
        if (options_.enable_wave_value_dirty) {
            sample_dirty_wave_value_groups(cycle);
        }
        if (options_.enable_wave_array_dirty) {
            sample_dirty_wave_array_groups(cycle);
        }
        if (options_.enable_dirty_peek_groups || options_.enable_wave_value_dirty || options_.enable_wave_array_dirty) {
            ++dirty_epoch_;
            if (dirty_epoch_ == 0) {
                dirty_epoch_ = 1;
                reset_dirty_epochs_after_wrap();
            }
        }

        if (options_.debug_log) debug_log_msg(std::string("sample end cycle=") + detail::to_string_unsigned(cycle));
    }

    // Optional pre-elaboration for WVZ4/stable-topology mode.  This builds the
    // node tree and declares tracks before the first sampled cycle, allowing the
    // WVZ4 recorder to open the writer/layout before cycle-0 value updates.
    void prepare_topology(Cycle cycle = 0) {
        refresh_root_watches(cycle);
        // Allocation tracking and per-cycle pointer validity/rebuild checks are intentionally disabled.
        // Pointer fields are expanded only once, and only when their pointee type opts in via
        // ::wave::DirectReflectPointerTarget.
        refresh_lazy_value_watches(cycle);
        export_node_declarations_once();
        if (options_.enable_wave_value_dirty) {
            ensure_dirty_wave_value_addr_table_sorted();
            ensure_dirty_wave_value_addr_hash_table();
            // Make single-thread use work without an explicit attach call.
            // Do not clear the TLS dirty buffer if this thread is already attached,
            // because sample() calls prepare_topology() every cycle.
            try_ensure_wave_value_tls_capacity_for_current_thread();
        }
        if (options_.enable_wave_array_dirty) {
            ensure_dirty_wave_array_addr_table_sorted();
            try_ensure_wave_array_tls_capacity_for_current_thread();
        }
        if (options_.enable_flat_leaf_fast_table) {
            ensure_flat_leaf_fast_table();
        }
        maybe_dump_leaf_distribution_after_topology();
    }

    const std::vector<NodeDesc>& nodes() const { return nodes_; }
    const std::vector<TrackDesc>& tracks() const { return tracks_; }
    const std::vector<ObjectEntry>& objects() const { return objects_; }

    std::size_t root_watch_count() const noexcept {
        return root_watches_.empty() ? 0u : (root_watches_.size() - 1u);
    }

    std::size_t expanded_root_watch_count() const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 1; i < root_watches_.size(); ++i) {
            const RootWatch& w = root_watches_[i];
            if (w.id != 0 && w.alive && w.expanded) ++n;
        }
        return n;
    }

    bool node_declarations_exported() const noexcept { return nodes_exported_; }

    std::string topology_debug_summary(std::size_t root_limit = 8) const {
        std::ostringstream os;
        os << "root_watches=" << root_watch_count()
           << " expanded_roots=" << expanded_root_watch_count()
           << " nodes_size=" << nodes_.size()
           << " tracks_size=" << tracks_.size()
           << " objects_size=" << objects_.size()
           << " lazy_watches_size=" << lazy_value_watches_.size()
           << " nodes_exported=" << (nodes_exported_ ? 1 : 0)
           << " flat_leaf_valid=" << (flat_leaf_fast_table_valid_ ? 1 : 0)
           << " flat_leaf_count=" << flat_leaf_fast_table_.size()
           << " dirty_peek_groups=" << dirty_peek_groups_.size()
           << " dirty_wave_value_groups=" << dirty_wave_value_groups_.size()
           << " dirty_wave_array_groups=" << dirty_wave_array_groups_.size();
        os << " roots=[";
        std::size_t shown = 0;
        for (std::size_t i = 1; i < root_watches_.size() && shown < root_limit; ++i) {
            const RootWatch& w = root_watches_[i];
            if (w.id == 0) continue;
            if (shown != 0) os << "; ";
            os << "id=" << w.id
               << " alive=" << (w.alive ? 1 : 0)
               << " expanded=" << (w.expanded ? 1 : 0)
               << " root_node=" << w.root_node_id
               << " addr=" << detail::pointer_to_string(w.address);
            if (!w.path.empty()) os << " path=" << w.path;
            if (!w.root_type_name.empty()) os << " type=" << w.root_type_name;
            os << " reflected=" << (w.root_is_reflected ? 1 : 0)
               << " leaf=" << (w.root_is_leaf_scalar ? 1 : 0)
               << " wave_value=" << (w.root_is_wave_value ? 1 : 0)
               << " wave_array=" << (w.root_is_wave_array ? 1 : 0)
               << " ptr=" << (w.root_is_pointer_or_smart ? 1 : 0)
               << " blacklisted_stl=" << (w.root_is_blacklisted_stl ? 1 : 0);
            ++shown;
        }
        if (root_watch_count() > shown) os << "; ...";
        os << "]";
        return os.str();
    }

    void dump_leaf_distribution_by_depth(FILE* fp,
                                         std::uint32_t top_n = 50,
                                         bool dirty_safe_only = true) const {
        dump_leaf_distribution_by_depth_impl_(fp, top_n, dirty_safe_only);
    }

    static void wave_dirty_hook_mark_bridge_(void* tracer, std::uint32_t group_id) noexcept {
        if (tracer) {
            static_cast<Tracer*>(tracer)->mark_dirty_peek_group(group_id);
        }
    }

    static void wave_value_notify_bridge_(const void* address) noexcept {
        if (!address) return;
        ThreadTraceLocal& tls = current_thread_trace_local();
        if (tls.owner && tls.owner->try_mark_dirty_wave_value_address(address)) return;

        // If the business thread did not explicitly attach, resolve the owner
        // once through the active-tracer address table, then attach this TLS.
        // Normal hot path after the first write still uses tls.owner directly.
        Tracer* owner = resolve_wave_value_owner_for_address_(address);
        if (owner) owner->try_mark_dirty_wave_value_address(address);
    }

    static void wave_array_index_notify_bridge_(std::size_t index, const void* element_address, const void* element_type_tag, std::size_t element_size) noexcept {
        ThreadTraceLocal& tls = current_thread_trace_local();
        if (tls.owner && tls.owner->try_mark_dirty_wave_array_element_address(index, element_address, element_type_tag, element_size)) return;

        Tracer* owner = resolve_wave_array_owner_for_address_(element_address, element_type_tag, element_size);
        if (owner) owner->try_mark_dirty_wave_array_element_address(index, element_address, element_type_tag, element_size);
    }

    void attach_current_thread_for_dirty_peek() {
        if (options_.enable_dirty_peek_groups) ensure_dirty_tls_capacity_for_current_thread();
        if (options_.enable_wave_value_dirty) ensure_wave_value_tls_capacity_for_current_thread();
        if (options_.enable_wave_array_dirty) ensure_wave_array_tls_capacity_for_current_thread();
    }

    void detach_current_thread_for_dirty_peek() noexcept {
        ThreadTraceLocal& tls = current_thread_trace_local();
        if (tls.owner == this) {
            adopt_tls_dirty_on_thread_exit(tls);
        }
        tls.detach(this);
    }

    // Called from ThreadTraceLocal destructor.  This preserves dirty marks from
    // short-lived worker threads that exit before the cycle-level sample() call.
    void adopt_tls_dirty_on_thread_exit(ThreadTraceLocal& tls) noexcept {
        if (tls.owner != this) return;
        try {
            std::lock_guard<std::mutex> lock(retired_dirty_mu_);
            const std::uint32_t dirty_count = std::min<std::uint32_t>(tls.dirty_count, static_cast<std::uint32_t>(tls.dirty_ids.size()));
            for (std::uint32_t i = 0; i < dirty_count; ++i) retired_dirty_group_ids_.push_back(tls.dirty_ids[i]);

            const std::uint32_t wave_value_count = std::min<std::uint32_t>(tls.wave_value_dirty_count, static_cast<std::uint32_t>(tls.wave_value_dirty_ids.size()));
            for (std::uint32_t i = 0; i < wave_value_count; ++i) retired_dirty_wave_value_group_ids_.push_back(tls.wave_value_dirty_ids[i]);

            const std::uint32_t wave_array_count = std::min<std::uint32_t>(tls.wave_array_dirty_count, static_cast<std::uint32_t>(tls.wave_array_dirty_ids.size()));
            for (std::uint32_t i = 0; i < wave_array_count; ++i) retired_dirty_wave_array_group_ids_.push_back(tls.wave_array_dirty_ids[i]);

            tls.dirty_count = 0;
            tls.wave_value_dirty_count = 0;
            tls.wave_array_dirty_count = 0;
        } catch (...) {}
    }

    static void preserve_tls_dirty_before_owner_switch_(ThreadTraceLocal& tls, Tracer* new_owner) noexcept {
        if (tls.owner && tls.owner != new_owner) {
            tls.owner->adopt_tls_dirty_on_thread_exit(tls);
        }
    }

    void mark_dirty_peek_group(std::uint32_t group_id) noexcept {
        if (!options_.enable_dirty_peek_groups) return;
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return;
        if (!dirty_peek_groups_[group_id].dirty_safe) return;

        ThreadTraceLocal& tls = current_thread_trace_local();
        if (tls.owner != this || group_id >= tls.local_epoch.size() || group_id >= tls.dirty_ids.size()) {
            // Correctness fallback: if the business thread forgot to call
            // attach_current_thread_for_dirty_peek(), or if new dirty groups were
            // created after it attached, grow this thread's TLS buffers once here.
            // This is not the intended hot path; normal users should still attach
            // business threads after topology preparation to avoid first-write
            // allocation.
            if (!try_ensure_dirty_tls_capacity_for_current_thread()) return;
        }
        if (group_id >= tls.local_epoch.size() || group_id >= tls.dirty_ids.size()) return;
        if (tls.local_epoch[group_id] == dirty_epoch_) return;
        tls.local_epoch[group_id] = dirty_epoch_;
        if (tls.dirty_count < tls.dirty_ids.size()) {
            tls.dirty_ids[tls.dirty_count++] = group_id;
        }
    }


    void attach_current_thread_for_wave_values() {
        if (!options_.enable_wave_value_dirty) return;
        ensure_wave_value_tls_capacity_for_current_thread();
    }

    bool try_mark_dirty_wave_value_address(const void* address) noexcept {
        if (!options_.enable_wave_value_dirty || !address) return false;
        const std::uint32_t group_id = lookup_dirty_wave_value_group_by_address(address);
        if (group_id == kInvalidIndex || group_id >= dirty_wave_value_groups_.size()) return false;
        mark_dirty_wave_value_group(group_id);
        return true;
    }

    void mark_dirty_wave_value_address(const void* address) noexcept {
        (void)try_mark_dirty_wave_value_address(address);
    }

    bool try_mark_dirty_wave_array_element_address(std::size_t index, const void* element_address, const void* element_type_tag, std::size_t element_size) noexcept {
        (void)index;
        if (!options_.enable_wave_array_dirty || !element_address || !element_type_tag || element_size == 0) return false;
        const std::uint32_t group_id = lookup_dirty_wave_array_group_by_address(element_address, element_type_tag, static_cast<std::uint32_t>(element_size));
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) return false;
        mark_dirty_wave_array_group(group_id);
        return true;
    }

    void mark_dirty_wave_array_element_address(std::size_t index, const void* element_address, const void* element_type_tag, std::size_t element_size) noexcept {
        (void)try_mark_dirty_wave_array_element_address(index, element_address, element_type_tag, element_size);
    }

private:
    static std::mutex& active_tracers_mutex_() {
        static std::mutex* mu = new std::mutex();
        return *mu;
    }

    static std::vector<Tracer*>& active_tracers_() {
        static std::vector<Tracer*>* tracers = new std::vector<Tracer*>();
        return *tracers;
    }

    static void register_active_tracer_(Tracer* tracer) noexcept {
        if (!tracer) return;
        try {
            std::lock_guard<std::mutex> lock(active_tracers_mutex_());
            std::vector<Tracer*>& tracers = active_tracers_();
            if (std::find(tracers.begin(), tracers.end(), tracer) == tracers.end()) {
                tracers.push_back(tracer);
            }
        } catch (...) {}
    }

    static void unregister_active_tracer_(Tracer* tracer) noexcept {
        if (!tracer) return;
        try {
            std::lock_guard<std::mutex> lock(active_tracers_mutex_());
            std::vector<Tracer*>& tracers = active_tracers_();
            tracers.erase(std::remove(tracers.begin(), tracers.end(), tracer), tracers.end());
        } catch (...) {}
    }

    static Tracer* resolve_wave_value_owner_for_address_(const void* address) noexcept {
        if (!address) return NULL;
        try {
            std::lock_guard<std::mutex> lock(active_tracers_mutex_());
            std::vector<Tracer*>& tracers = active_tracers_();
            for (std::size_t i = 0; i < tracers.size(); ++i) {
                Tracer* tracer = tracers[i];
                if (!tracer || !tracer->options_.enable_wave_value_dirty) continue;
                if (tracer->lookup_dirty_wave_value_group_by_address(address) != kInvalidIndex) return tracer;
            }
        } catch (...) {}
        return NULL;
    }

    static Tracer* resolve_wave_array_owner_for_address_(const void* element_address, const void* element_type_tag, std::size_t element_size) noexcept {
        if (!element_address || !element_type_tag || element_size == 0) return NULL;
        try {
            std::lock_guard<std::mutex> lock(active_tracers_mutex_());
            std::vector<Tracer*>& tracers = active_tracers_();
            for (std::size_t i = 0; i < tracers.size(); ++i) {
                Tracer* tracer = tracers[i];
                if (!tracer || !tracer->options_.enable_wave_array_dirty) continue;
                if (tracer->lookup_dirty_wave_array_group_by_address(element_address, element_type_tag, static_cast<std::uint32_t>(element_size)) != kInvalidIndex) return tracer;
            }
        } catch (...) {}
        return NULL;
    }

    struct ScalarGetterStorageBase {
        virtual ~ScalarGetterStorageBase() {}
    };

    template <typename Obj, typename T>
    struct ScalarGetterStorage : ScalarGetterStorageBase {
        const Obj* obj;
        T (*getter)(const Obj*);

        ScalarGetterStorage(const Obj* o, T (*g)(const Obj*)) : obj(o), getter(g) {}
    };

    IWaveSink& sink_;
    BuildOptions options_;
    std::ofstream debug_log_stream_;
    std::ofstream parallel_flat_leaf_load_log_stream_;
    std::size_t debug_log_event_count_;

    NodeId next_node_id_;
    TrackId next_track_id_;
    ObjectId next_object_id_;
    WatchId next_watch_id_;

    // Id-indexed storage. Id 0 is unused, so NodeId/TrackId/WatchId can be used
    // as a direct vector index. This removes per-cycle unordered_map iteration and
    // lookup overhead after the reflection tree has been built.
    std::vector<NodeDesc> nodes_;
    std::deque<std::string> node_names_;
    std::deque<std::string> node_debug_paths_;
    std::vector<TrackDesc> tracks_;
    std::deque<std::string> track_paths_;
    std::vector<TrackRuntimeState> track_runtime_;
    std::deque<std::string> track_runtime_last_values_;
    std::vector<std::unique_ptr<ScalarGetterStorageBase> > scalar_getter_storage_;
    std::vector<ObjectEntry> objects_;
    std::deque<std::string> object_debug_names_;
    std::vector<LazyValueWatch> lazy_value_watches_;
    std::vector<RootWatch> root_watches_;

    // Address/type lookup is still hash-based because object identity is not an
    // integer id before insertion. It is used only while expanding the tree.
    std::unordered_map<ObjectKey, ObjectId, ObjectKeyHash> object_id_by_key_;
    std::vector<ObjectKey> object_created_keys_;
    bool nodes_exported_;
    std::unordered_set<std::string> root_paths_;
    std::unordered_set<const void*> root_addresses_;

    enum class WorkerJobKind { TrackSlice, DirtyGroupSlice, FlatLeafSlice, FlatMemoryMixedSlice };

    struct SampleWorker;
    std::vector<std::unique_ptr<SampleWorker> > parallel_workers_;
    std::vector<TrackId> all_track_ids_;
    std::vector<TrackId> parallel_track_ids_;
    std::vector<TrackId> main_thread_track_ids_;

    struct FlatLeafFast {
        TrackId track_id;
        TrackId storage_id;
        const void* sample_ctx;
        const void* memory_ctx;
        std::uint32_t memory_byte_width;
        ScalarReadFn scalar_reader;
        ScalarSampleKind scalar_kind;
        ValueKind value_kind;
        bool has_last;
        std::uint64_t last_bits;

        FlatLeafFast()
            : track_id(0), storage_id(0), sample_ctx(NULL), memory_ctx(NULL), memory_byte_width(0),
              scalar_reader(NULL), scalar_kind(ScalarSampleKind::None), value_kind(ValueKind::Unknown),
              has_last(false), last_bits(0) {}
    };

    std::vector<FlatLeafFast> flat_leaf_fast_table_;
    bool flat_leaf_fast_table_valid_ = false;
    bool flat_leaf_fast_table_complete_ = false;

    struct FlatMemoryBlock {
        const unsigned char* base;
        std::size_t byte_count;
        std::size_t shadow_offset;
        std::uint32_t first_leaf_ref;
        std::uint32_t leaf_count;
        bool needs_initial_scan;

        FlatMemoryBlock()
            : base(NULL), byte_count(0), shadow_offset(0), first_leaf_ref(0),
              leaf_count(0), needs_initial_scan(false) {}
    };

    struct FlatMemoryLeafRef {
        std::uint32_t flat_leaf_index;
        std::uint32_t offset;
        std::uint32_t byte_count;
        std::uint32_t last_sample_epoch;

        FlatMemoryLeafRef()
            : flat_leaf_index(0), offset(0), byte_count(0), last_sample_epoch(0) {}
        FlatMemoryLeafRef(std::uint32_t i, std::uint32_t o, std::uint32_t b)
            : flat_leaf_index(i), offset(o), byte_count(b), last_sample_epoch(0) {}
    };

    std::vector<FlatMemoryBlock> flat_memory_blocks_;
    std::vector<FlatMemoryLeafRef> flat_memory_leaf_refs_;
    std::vector<std::uint32_t> flat_memory_scalar_leaf_indices_;
    // Prefix sum over the mixed memory-block sampling items.  Items are laid
    // out as [all blocks][all scalar remainder leaves].  The weight is leaf
    // count, not item count, so worker slicing can balance large blocks better
    // and reserve event buffers in O(1).
    std::vector<std::uint64_t> flat_memory_mixed_leaf_prefix_;
    std::vector<unsigned char> flat_memory_shadow_bytes_;
    std::vector<unsigned char> flat_memory_leaf_covered_;
    bool flat_memory_blocks_valid_ = false;
    bool flat_memory_blocks_complete_ = false;
    bool flat_memory_block_summary_logged_ = false;
    std::uint32_t flat_memory_sample_epoch_ = 1;
    bool leaf_distribution_dumped_ = false;

    struct DirtyPeekGroupKey {
        const void* address;
        const void* type_tag;
        std::uint32_t byte_width;

        DirtyPeekGroupKey() : address(NULL), type_tag(NULL), byte_width(0) {}
        DirtyPeekGroupKey(const void* a, const void* t, std::uint32_t w)
            : address(a), type_tag(t), byte_width(w) {}

        bool operator==(const DirtyPeekGroupKey& other) const {
            return address == other.address && type_tag == other.type_tag && byte_width == other.byte_width;
        }
    };

    struct DirtyPeekGroupKeyHash {
        std::size_t operator()(const DirtyPeekGroupKey& key) const {
            std::size_t h = std::hash<const void*>()(key.address);
            h ^= std::hash<const void*>()(key.type_tag) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint32_t>()(key.byte_width) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct DirtyPeekGroup {
        DirtyPeekGroupKey key;
        std::uint32_t first_range;
        std::uint32_t total_leaf_count;
        std::uint32_t range_count;
        std::uint32_t hooked_range_count;
        std::uint32_t safe_alias_range_count;
        std::uint32_t queued_epoch;
        std::uint32_t memory_block_begin;
        std::uint32_t memory_block_count;
        std::uint32_t memory_scalar_begin;
        std::uint32_t memory_scalar_count;
        bool dirty_safe;

        DirtyPeekGroup()
            : first_range(kInvalidIndex), total_leaf_count(0), range_count(0), hooked_range_count(0),
              safe_alias_range_count(0), queued_epoch(0), memory_block_begin(0), memory_block_count(0),
              memory_scalar_begin(0), memory_scalar_count(0), dirty_safe(false) {}
    };

    struct DirtyPeekRange {
        std::uint32_t group_id;
        std::uint32_t leaf_begin;
        std::uint32_t leaf_count;
        std::uint32_t next_sibling;
        bool has_hook;
        bool safe_alias_without_own_hook;

        DirtyPeekRange()
            : group_id(kInvalidIndex), leaf_begin(0), leaf_count(0), next_sibling(kInvalidIndex),
              has_hook(false), safe_alias_without_own_hook(false) {}
    };

    struct DirtyPeekLeaf {
        TrackId track_id;
        TrackId storage_id;
        const void* sample_ctx;
        const void* memory_ctx;
        std::uint32_t memory_byte_width;
        ScalarReadFn scalar_reader;
        ScalarSampleKind scalar_kind;
        ValueKind value_kind;
        bool has_last;
        std::uint64_t last_bits;
        std::uint32_t memory_last_sample_epoch;

        DirtyPeekLeaf()
            : track_id(0), storage_id(0), sample_ctx(NULL), memory_ctx(NULL), memory_byte_width(0),
              scalar_reader(NULL), scalar_kind(ScalarSampleKind::None),
              value_kind(ValueKind::Unknown), has_last(false), last_bits(0), memory_last_sample_epoch(0) {}
    };

    struct DirtyPeekMemoryBlock {
        const unsigned char* base;
        std::size_t byte_count;
        std::size_t shadow_offset;
        std::uint32_t first_leaf_ref;
        std::uint32_t leaf_count;
        std::uint32_t byte_map_begin;
        std::uint32_t byte_map_count;
        bool has_byte_map;
        bool needs_initial_scan;

        DirtyPeekMemoryBlock()
            : base(NULL), byte_count(0), shadow_offset(0), first_leaf_ref(0),
              leaf_count(0), byte_map_begin(0), byte_map_count(0),
              has_byte_map(false), needs_initial_scan(false) {}
    };

    struct DirtyPeekMemoryLeafRef {
        std::uint32_t dirty_leaf_index;
        std::uint32_t offset;
        std::uint32_t byte_count;
        std::uint32_t last_sample_epoch;

        DirtyPeekMemoryLeafRef()
            : dirty_leaf_index(0), offset(0), byte_count(0), last_sample_epoch(0) {}
        DirtyPeekMemoryLeafRef(std::uint32_t i, std::uint32_t o, std::uint32_t b)
            : dirty_leaf_index(i), offset(o), byte_count(b), last_sample_epoch(0) {}
    };

    struct DirtyPeekStorageKey {
        const void* memory_address;
        ScalarReadFn scalar_reader;
        ScalarSampleKind scalar_kind;
        ValueKind value_kind;
        std::uint32_t bit_width;
        std::uint32_t memory_byte_width;

        DirtyPeekStorageKey()
            : memory_address(NULL), scalar_reader(NULL), scalar_kind(ScalarSampleKind::None),
              value_kind(ValueKind::Unknown), bit_width(0), memory_byte_width(0) {}

        DirtyPeekStorageKey(const void* addr, ScalarReadFn reader, ScalarSampleKind sk,
                            ValueKind vk, std::uint32_t bits, std::uint32_t mem_bytes)
            : memory_address(addr), scalar_reader(reader), scalar_kind(sk),
              value_kind(vk), bit_width(bits), memory_byte_width(mem_bytes) {}

        bool operator==(const DirtyPeekStorageKey& other) const {
            return memory_address == other.memory_address &&
                   scalar_reader == other.scalar_reader &&
                   scalar_kind == other.scalar_kind &&
                   value_kind == other.value_kind &&
                   bit_width == other.bit_width &&
                   memory_byte_width == other.memory_byte_width;
        }
    };

    struct DirtyPeekStorageKeyHash {
        std::size_t operator()(const DirtyPeekStorageKey& key) const {
            std::size_t h = std::hash<const void*>()(key.memory_address);
            h ^= std::hash<const void*>()(reinterpret_cast<const void*>(key.scalar_reader)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(static_cast<int>(key.scalar_kind)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(static_cast<int>(key.value_kind)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint32_t>()(key.bit_width) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint32_t>()(key.memory_byte_width) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::unordered_map<DirtyPeekGroupKey, std::uint32_t, DirtyPeekGroupKeyHash> dirty_peek_group_by_key_;
    std::unordered_map<DirtyPeekStorageKey, TrackId, DirtyPeekStorageKeyHash> dirty_peek_storage_by_key_;
    std::vector<DirtyPeekGroup> dirty_peek_groups_;
    std::vector<DirtyPeekRange> dirty_peek_ranges_;
    std::vector<DirtyPeekLeaf> dirty_peek_leaves_;
    std::vector<DirtyPeekMemoryBlock> dirty_peek_memory_blocks_;
    std::vector<DirtyPeekMemoryLeafRef> dirty_peek_memory_leaf_refs_;
    std::vector<std::uint32_t> dirty_peek_memory_byte_to_leaf_refs_;
    std::vector<std::uint32_t> dirty_peek_memory_scalar_leaf_indices_;
    std::vector<unsigned char> dirty_peek_memory_shadow_bytes_;
    bool dirty_peek_memory_blocks_valid_ = false;
    std::uint32_t dirty_peek_storage_sample_epoch_ = 1;
    bool dirty_peek_memory_blocks_complete_ = false;
    std::uint32_t dirty_peek_memory_sample_epoch_ = 1;
    std::vector<WaveDirtyHook*> bound_dirty_hooks_;
    std::vector<std::uint32_t> global_dirty_group_ids_;
    std::vector<std::uint32_t> retired_dirty_group_ids_;
    mutable std::mutex retired_dirty_mu_;
    std::vector<std::uint32_t> dirty_worker_plan_begin_;
    std::vector<std::uint32_t> dirty_worker_plan_end_;
    std::uint32_t global_dirty_group_count_ = 0;
    std::uint32_t dirty_epoch_ = 1;
    bool dirty_peek_initial_sample_done_ = false;
    std::uint32_t active_dirty_peek_group_id_ = kInvalidIndex;
    std::uint32_t active_dirty_peek_range_id_ = kInvalidIndex;


    struct DirtyWaveValueGroupKey {
        const void* address;
        const void* type_tag;
        std::uint32_t byte_width;

        DirtyWaveValueGroupKey() : address(NULL), type_tag(NULL), byte_width(0) {}
        DirtyWaveValueGroupKey(const void* a, const void* t, std::uint32_t w)
            : address(a), type_tag(t), byte_width(w) {}

        bool operator==(const DirtyWaveValueGroupKey& other) const {
            return address == other.address && type_tag == other.type_tag && byte_width == other.byte_width;
        }
    };

    struct DirtyWaveValueGroupKeyHash {
        std::size_t operator()(const DirtyWaveValueGroupKey& key) const {
            std::size_t h = std::hash<const void*>()(key.address);
            h ^= std::hash<const void*>()(key.type_tag) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint32_t>()(key.byte_width) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct DirtyWaveValueGroup {
        DirtyWaveValueGroupKey key;
        std::uint32_t first_range;
        std::uint32_t total_leaf_count;
        std::uint32_t queued_epoch;

        DirtyWaveValueGroup()
            : first_range(kInvalidIndex), total_leaf_count(0), queued_epoch(0) {}
    };

    struct DirtyWaveValueRange {
        std::uint32_t group_id;
        std::uint32_t leaf_begin;
        std::uint32_t leaf_count;
        std::uint32_t next_sibling;

        DirtyWaveValueRange()
            : group_id(kInvalidIndex), leaf_begin(0), leaf_count(0), next_sibling(kInvalidIndex) {}
    };

    struct DirtyWaveValueLeaf {
        TrackId track_id;
        TrackId storage_id;
        const void* sample_ctx;
        const void* memory_ctx;
        std::uint32_t memory_byte_width;
        ScalarReadFn scalar_reader;
        ScalarSampleKind scalar_kind;
        ValueKind value_kind;
        bool has_last;
        std::uint64_t last_bits;

        DirtyWaveValueLeaf()
            : track_id(0), storage_id(0), sample_ctx(NULL), memory_ctx(NULL), memory_byte_width(0),
              scalar_reader(NULL), scalar_kind(ScalarSampleKind::None),
              value_kind(ValueKind::Unknown), has_last(false), last_bits(0) {}
    };

    struct DirtyWaveValueAddressEntry {
        const void* address;
        std::uint32_t group_id;
        std::uint32_t byte_width;
        DirtyWaveValueAddressEntry() : address(NULL), group_id(kInvalidIndex), byte_width(0) {}
        DirtyWaveValueAddressEntry(const void* a, std::uint32_t g, std::uint32_t w)
            : address(a), group_id(g), byte_width(w) {}
    };

    struct DirtyWaveValueAddressHashSlot {
        const void* address;
        std::uint32_t group_id;
        std::uint32_t byte_width;
        DirtyWaveValueAddressHashSlot() : address(NULL), group_id(kInvalidIndex), byte_width(0) {}
    };

    std::unordered_map<DirtyWaveValueGroupKey, std::uint32_t, DirtyWaveValueGroupKeyHash> dirty_wave_value_group_by_key_;
    std::vector<DirtyWaveValueGroup> dirty_wave_value_groups_;
    std::vector<DirtyWaveValueRange> dirty_wave_value_ranges_;
    std::vector<DirtyWaveValueLeaf> dirty_wave_value_leaves_;
    std::vector<DirtyWaveValueAddressEntry> dirty_wave_value_addr_table_;
    bool dirty_wave_value_addr_table_sorted_ = false;
    std::vector<DirtyWaveValueAddressHashSlot> dirty_wave_value_addr_hash_table_;
    bool dirty_wave_value_addr_hash_built_ = false;
    std::size_t dirty_wave_value_addr_hash_mask_ = 0;
    std::vector<std::uint32_t> global_dirty_wave_value_group_ids_;
    std::vector<std::uint32_t> retired_dirty_wave_value_group_ids_;
    std::uint32_t global_dirty_wave_value_group_count_ = 0;
    bool dirty_wave_value_initial_sample_done_ = false;
    std::uint32_t active_dirty_wave_value_group_id_ = kInvalidIndex;
    std::uint32_t active_dirty_wave_value_range_id_ = kInvalidIndex;

    struct DirtyWaveArrayGroupKey {
        const void* address;
        const void* type_tag;
        std::uint32_t byte_width;
        DirtyWaveArrayGroupKey() : address(NULL), type_tag(NULL), byte_width(0) {}
        DirtyWaveArrayGroupKey(const void* a, const void* t, std::uint32_t w) : address(a), type_tag(t), byte_width(w) {}
        bool operator==(const DirtyWaveArrayGroupKey& other) const {
            return address == other.address && type_tag == other.type_tag && byte_width == other.byte_width;
        }
    };

    struct DirtyWaveArrayGroupKeyHash {
        std::size_t operator()(const DirtyWaveArrayGroupKey& key) const {
            std::size_t h = std::hash<const void*>()(key.address);
            h ^= std::hash<const void*>()(key.type_tag) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint32_t>()(key.byte_width) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct DirtyWaveArrayGroup {
        DirtyWaveArrayGroupKey key;
        std::uint32_t element_index;
        std::uint32_t first_range;
        std::uint32_t total_leaf_count;
        std::uint32_t queued_epoch;
        std::uint32_t memory_block_begin;
        std::uint32_t memory_block_count;
        std::uint32_t memory_scalar_begin;
        std::uint32_t memory_scalar_count;
        DirtyWaveArrayGroup()
            : element_index(0), first_range(kInvalidIndex), total_leaf_count(0), queued_epoch(0),
              memory_block_begin(0), memory_block_count(0), memory_scalar_begin(0), memory_scalar_count(0) {}
    };

    struct DirtyWaveArrayRange {
        std::uint32_t group_id;
        std::uint32_t leaf_begin;
        std::uint32_t leaf_count;
        std::uint32_t next_sibling;
        DirtyWaveArrayRange() : group_id(kInvalidIndex), leaf_begin(0), leaf_count(0), next_sibling(kInvalidIndex) {}
    };

    struct DirtyWaveArrayLeaf {
        TrackId track_id;
        TrackId storage_id;
        const void* sample_ctx;
        const void* memory_ctx;
        std::uint32_t memory_byte_width;
        ScalarReadFn scalar_reader;
        ScalarSampleKind scalar_kind;
        ValueKind value_kind;
        bool has_last;
        std::uint64_t last_bits;
        std::uint32_t memory_last_sample_epoch;
        DirtyWaveArrayLeaf() : track_id(0), storage_id(0), sample_ctx(NULL), memory_ctx(NULL), memory_byte_width(0),
                               scalar_reader(NULL), scalar_kind(ScalarSampleKind::None),
                               value_kind(ValueKind::Unknown), has_last(false), last_bits(0), memory_last_sample_epoch(0) {}
    };

    struct DirtyWaveArrayMemoryBlock {
        const unsigned char* base;
        std::size_t byte_count;
        std::size_t shadow_offset;
        std::uint32_t first_leaf_ref;
        std::uint32_t leaf_count;
        std::uint32_t byte_map_begin;
        std::uint32_t byte_map_count;
        bool has_byte_map;
        bool needs_initial_scan;

        DirtyWaveArrayMemoryBlock()
            : base(NULL), byte_count(0), shadow_offset(0), first_leaf_ref(0),
              leaf_count(0), byte_map_begin(0), byte_map_count(0),
              has_byte_map(false), needs_initial_scan(false) {}
    };

    struct DirtyWaveArrayMemoryLeafRef {
        std::uint32_t dirty_leaf_index;
        std::uint32_t offset;
        std::uint32_t byte_count;
        std::uint32_t last_sample_epoch;

        DirtyWaveArrayMemoryLeafRef()
            : dirty_leaf_index(0), offset(0), byte_count(0), last_sample_epoch(0) {}
        DirtyWaveArrayMemoryLeafRef(std::uint32_t i, std::uint32_t o, std::uint32_t b)
            : dirty_leaf_index(i), offset(o), byte_count(b), last_sample_epoch(0) {}
    };

    struct DirtyWaveArrayAddressEntry {
        const void* address;
        const void* type_tag;
        std::uint32_t byte_width;
        std::uint32_t group_id;
        DirtyWaveArrayAddressEntry() : address(NULL), type_tag(NULL), byte_width(0), group_id(kInvalidIndex) {}
        DirtyWaveArrayAddressEntry(const void* a, const void* t, std::uint32_t w, std::uint32_t g)
            : address(a), type_tag(t), byte_width(w), group_id(g) {}
    };

    std::unordered_map<DirtyWaveArrayGroupKey, std::uint32_t, DirtyWaveArrayGroupKeyHash> dirty_wave_array_group_by_key_;
    std::vector<DirtyWaveArrayGroup> dirty_wave_array_groups_;
    std::vector<DirtyWaveArrayRange> dirty_wave_array_ranges_;
    std::vector<DirtyWaveArrayLeaf> dirty_wave_array_leaves_;
    std::vector<DirtyWaveArrayAddressEntry> dirty_wave_array_addr_table_;
    bool dirty_wave_array_addr_table_sorted_ = false;
    std::vector<DirtyWaveArrayMemoryBlock> dirty_wave_array_memory_blocks_;
    std::vector<DirtyWaveArrayMemoryLeafRef> dirty_wave_array_memory_leaf_refs_;
    std::vector<std::uint32_t> dirty_wave_array_memory_byte_to_leaf_refs_;
    std::vector<std::uint32_t> dirty_wave_array_memory_scalar_leaf_indices_;
    std::vector<unsigned char> dirty_wave_array_memory_shadow_bytes_;
    bool dirty_wave_array_memory_blocks_valid_ = false;
    bool dirty_wave_array_memory_blocks_complete_ = false;
    std::uint32_t dirty_wave_array_memory_sample_epoch_ = 1;
    std::vector<std::uint32_t> global_dirty_wave_array_group_ids_;
    std::vector<std::uint32_t> retired_dirty_wave_array_group_ids_;
    std::uint32_t global_dirty_wave_array_group_count_ = 0;
    bool dirty_wave_array_initial_sample_done_ = false;
    std::uint32_t active_dirty_wave_array_group_id_ = kInvalidIndex;
    std::uint32_t active_dirty_wave_array_range_id_ = kInvalidIndex;

    std::vector<TrackEvent> caller_parallel_events_;

    // Expansion stack is normally shallow; vector avoids hashing on every nested
    // reflected-object entry while still detecting recursion.
    std::vector<ObjectKey> expanding_;

private:
    enum class SampleFilter {
        Any,
        ParallelSafeOnly,
        MainThreadOnlyOnly
    };

    struct SampleWorker {
        explicit SampleWorker(Tracer* owner_in)
            : owner(owner_in), stop(false), has_job(false), done(true), job_kind(WorkerJobKind::TrackSlice), cycle(0), begin_pos(0), end_pos(0), events(NULL), debug_scanned(0), debug_changed(0), debug_time_us(0) {
            thread = std::thread(&SampleWorker::thread_main, this);
        }

        ~SampleWorker() {}

        Tracer* owner;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable cv;
        bool stop;
        bool has_job;
        bool done;
        WorkerJobKind job_kind;
        Cycle cycle;
        std::size_t begin_pos;
        std::size_t end_pos;
        std::vector<TrackEvent>* events;
        std::vector<TrackEvent> event_buffer;
        std::size_t debug_scanned;
        std::size_t debug_changed;
        std::uint64_t debug_time_us;

        void thread_main() {
            if (owner && owner->options_.parallel_worker_event_reserve != 0) {
                event_buffer.reserve(owner->options_.parallel_worker_event_reserve);
            }

            for (;;) {
                Cycle local_cycle = 0;
                WorkerJobKind local_job_kind = WorkerJobKind::TrackSlice;
                std::size_t local_begin = 0;
                std::size_t local_end = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this]() { return stop || has_job; });
                    if (stop) return;
                    local_cycle = cycle;
                    local_job_kind = job_kind;
                    local_begin = begin_pos;
                    local_end = end_pos;
                    has_job = false;
                }

                event_buffer.clear();
                const std::chrono::high_resolution_clock::time_point debug_t0 = std::chrono::high_resolution_clock::now();
                if (owner) {
                    if (local_job_kind == WorkerJobKind::DirtyGroupSlice) {
                        owner->sample_dirty_group_id_slice_to_buffer(local_cycle, local_begin, local_end, event_buffer);
                    } else if (local_job_kind == WorkerJobKind::FlatLeafSlice) {
                        owner->sample_flat_leaf_slice_to_buffer(local_cycle, local_begin, local_end, event_buffer);
                    } else if (local_job_kind == WorkerJobKind::FlatMemoryMixedSlice) {
                        owner->sample_flat_memory_mixed_slice_to_buffer(local_cycle, local_begin, local_end, event_buffer);
                    } else {
                        owner->sample_track_id_slice_to_buffer(local_cycle, local_begin, local_end, event_buffer);
                    }
                }
                const std::chrono::high_resolution_clock::time_point debug_t1 = std::chrono::high_resolution_clock::now();

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    events = &event_buffer;
                    if (owner && local_job_kind == WorkerJobKind::FlatMemoryMixedSlice) {
                        debug_scanned = owner->flat_memory_mixed_slice_event_capacity(local_begin, local_end);
                    } else {
                        debug_scanned = (local_end > local_begin) ? (local_end - local_begin) : 0;
                    }
                    debug_changed = event_buffer.size();
                    debug_time_us = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(debug_t1 - debug_t0).count());
                    done = true;
                }
                cv.notify_one();
            }
        }
    };

    bool should_use_parallel_sampling() const {
        if (!options_.enable_parallel_sampling) return false;
        if (options_.debug_log || options_.debug_log_track_samples || options_.debug_log_root_expand_stats) return false;
        if (parallel_track_ids_.size() <= options_.parallel_sampling_threshold) return false;
        return desired_parallel_worker_count() > 0;
    }

    std::size_t desired_parallel_worker_count() const {
        std::size_t requested = options_.sampling_threads;
        if (requested == 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            requested = hw > 1 ? static_cast<std::size_t>(hw - 1) : 1;
        }
        const std::size_t track_count = parallel_track_ids_.size();
        if (track_count == 0) return 0;
        if (requested > track_count) requested = track_count;
        return requested;
    }

    void ensure_parallel_workers(std::size_t count) {
        if (parallel_workers_.size() == count) return;
        stop_parallel_workers();
        parallel_workers_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            parallel_workers_.push_back(std::unique_ptr<SampleWorker>(new SampleWorker(this)));
        }
    }

    void stop_parallel_workers() {
        for (std::size_t i = 0; i < parallel_workers_.size(); ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            if (!worker) continue;
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->stop = true;
                worker->has_job = false;
            }
            worker->cv.notify_one();
        }
        for (std::size_t i = 0; i < parallel_workers_.size(); ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            if (worker && worker->thread.joinable()) worker->thread.join();
        }
        parallel_workers_.clear();
    }

    static ScalarSampleKind signed_scalar_kind_for_size(std::size_t size) {
        switch (size) {
        case 1: return ScalarSampleKind::I8;
        case 2: return ScalarSampleKind::I16;
        case 4: return ScalarSampleKind::I32;
        case 8: return ScalarSampleKind::I64;
        default: return ScalarSampleKind::None;
        }
    }

    static ScalarSampleKind unsigned_scalar_kind_for_size(std::size_t size) {
        switch (size) {
        case 1: return ScalarSampleKind::U8;
        case 2: return ScalarSampleKind::U16;
        case 4: return ScalarSampleKind::U32;
        case 8: return ScalarSampleKind::U64;
        default: return ScalarSampleKind::None;
        }
    }

    template <typename T, bool IsEnum>
    struct scalar_storage_type_impl { typedef T type; };

    template <typename T>
    struct scalar_storage_type_impl<T, true> { typedef typename std::underlying_type<T>::type type; };

    template <typename T>
    static ScalarSampleKind scalar_kind_for_type() {
        typedef typename detail::remove_cvref<T>::type CleanT;
        typedef typename scalar_storage_type_impl<CleanT, std::is_enum<CleanT>::value>::type StorageT;
        if (std::is_same<StorageT, bool>::value) return ScalarSampleKind::Bool;
        if (std::is_floating_point<StorageT>::value) {
            return sizeof(StorageT) == sizeof(float)
                ? ScalarSampleKind::F32
                : (sizeof(StorageT) == sizeof(double)
                    ? ScalarSampleKind::F64
                    : ScalarSampleKind::FLongDouble);
        }
        if (std::is_integral<StorageT>::value) {
            return std::is_signed<StorageT>::value
                ? signed_scalar_kind_for_size(sizeof(StorageT))
                : unsigned_scalar_kind_for_size(sizeof(StorageT));
        }
        return ScalarSampleKind::None;
    }

    template <typename T>
    static bool fill_scalar_snapshot_from_value(const T& raw_value, ScalarSnapshot& sample) {
        typedef typename detail::remove_cvref<T>::type CleanT;
        typedef typename scalar_storage_type_impl<CleanT, std::is_enum<CleanT>::value>::type StorageT;
        const ScalarSampleKind kind = scalar_kind_for_type<CleanT>();
        if (kind == ScalarSampleKind::None) return false;
        const StorageT value = static_cast<StorageT>(raw_value);
        sample.sample_kind = kind;
        switch (kind) {
        case ScalarSampleKind::Bool: {
            const bool v = static_cast<bool>(value);
            sample.bool_value = v;
            sample.u64_value = v ? 1u : 0u;
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I8: {
            const std::int8_t v = static_cast<std::int8_t>(value);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U8: {
            const std::uint8_t v = static_cast<std::uint8_t>(value);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I16: {
            const std::int16_t v = static_cast<std::int16_t>(value);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U16: {
            const std::uint16_t v = static_cast<std::uint16_t>(value);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I32: {
            const std::int32_t v = static_cast<std::int32_t>(value);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U32: {
            const std::uint32_t v = static_cast<std::uint32_t>(value);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I64: {
            const std::int64_t v = static_cast<std::int64_t>(value);
            sample.i64_value = v;
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U64: {
            const std::uint64_t v = static_cast<std::uint64_t>(value);
            sample.u64_value = v;
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::F32: {
            const float v = static_cast<float>(value);
            sample.f64_value = static_cast<double>(v);
            std::uint32_t bits32 = 0;
            std::memcpy(&bits32, &v, sizeof(bits32));
            sample.bits = static_cast<std::uint64_t>(bits32);
            sample.u64_value = static_cast<std::uint64_t>(bits32);
            return true;
        }
        case ScalarSampleKind::F64: {
            const double v = static_cast<double>(value);
            sample.f64_value = v;
            std::uint64_t bits = 0;
            std::memcpy(&bits, &sample.f64_value, sizeof(bits));
            sample.bits = bits;
            return true;
        }
        case ScalarSampleKind::FLongDouble: {
            const long double v = static_cast<long double>(value);
            sample.f64_value = static_cast<double>(v);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &sample.f64_value, sizeof(bits));
            sample.bits = bits;
            return true;
        }
        case ScalarSampleKind::None:
        default:
            return false;
        }
    }

    template <typename Obj, typename T>
    static bool read_scalar_getter_storage(const void* storage_ptr, ScalarSnapshot& sample) {
        const ScalarGetterStorage<Obj, T>* storage = static_cast<const ScalarGetterStorage<Obj, T>*>(storage_ptr);
        if (!storage || !storage->obj || !storage->getter) return false;
        const T value = storage->getter(storage->obj);
        return fill_scalar_snapshot_from_value<T>(value, sample);
    }

    static bool read_scalar_snapshot(const TrackDesc& track, ScalarSnapshot& sample) {
        if (track.scalar_reader) return track.scalar_reader(track.sample_ctx, sample);
        const void* p = track.sample_ctx;
        if (!p) return false;
        switch (track.scalar_kind) {
        case ScalarSampleKind::Bool: {
            const bool v = *static_cast<const bool*>(p);
            sample.bool_value = v;
            sample.u64_value = v ? 1u : 0u;
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I8: {
            const std::int8_t v = *static_cast<const std::int8_t*>(p);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U8: {
            const std::uint8_t v = *static_cast<const std::uint8_t*>(p);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I16: {
            const std::int16_t v = *static_cast<const std::int16_t*>(p);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U16: {
            const std::uint16_t v = *static_cast<const std::uint16_t*>(p);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I32: {
            const std::int32_t v = *static_cast<const std::int32_t*>(p);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U32: {
            const std::uint32_t v = *static_cast<const std::uint32_t*>(p);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I64: {
            const std::int64_t v = *static_cast<const std::int64_t*>(p);
            sample.i64_value = v;
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U64: {
            const std::uint64_t v = *static_cast<const std::uint64_t*>(p);
            sample.u64_value = v;
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::F32: {
            const float v = *static_cast<const float*>(p);
            sample.f64_value = static_cast<double>(v);
            std::uint32_t bits32 = 0;
            std::memcpy(&bits32, &v, sizeof(bits32));
            sample.bits = static_cast<std::uint64_t>(bits32);
            sample.u64_value = static_cast<std::uint64_t>(bits32);
            return true;
        }
        case ScalarSampleKind::F64: {
            const double v = *static_cast<const double*>(p);
            sample.f64_value = v;
            std::uint64_t bits = 0;
            std::memcpy(&bits, &sample.f64_value, sizeof(bits));
            sample.bits = bits;
            return true;
        }
        case ScalarSampleKind::FLongDouble: {
            const long double v = *static_cast<const long double*>(p);
            sample.f64_value = static_cast<double>(v);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &sample.f64_value, sizeof(bits));
            sample.bits = bits;
            return true;
        }
        case ScalarSampleKind::None:
        default:
            return false;
        }
    }

    static std::uint32_t scalar_bit_width(ScalarSampleKind kind) {
        switch (kind) {
        case ScalarSampleKind::Bool: return 1;
        case ScalarSampleKind::I8:
        case ScalarSampleKind::U8: return 8;
        case ScalarSampleKind::I16:
        case ScalarSampleKind::U16: return 16;
        case ScalarSampleKind::I32:
        case ScalarSampleKind::U32:
        case ScalarSampleKind::F32: return 32;
        case ScalarSampleKind::I64:
        case ScalarSampleKind::U64:
        case ScalarSampleKind::F64:
        case ScalarSampleKind::FLongDouble: return 64;
        case ScalarSampleKind::None:
        default: return 64;
        }
    }

    static void fill_scalar_event(const TrackDesc& track, const ScalarSnapshot& sample, Cycle cycle, TrackEvent& ev) {
        ev.cycle = cycle;
        ev.track_id = track.id;
        ev.invalid = InvalidKind::None;
        ev.has_change_bits = true;
        ev.change_bits = sample.bits;
        switch (track.kind) {
        case ValueKind::Bool:
            ev.has_bool = true;
            ev.bool_value = sample.bool_value;
            break;
        case ValueKind::SignedInt:
            ev.has_i64 = true;
            ev.i64_value = sample.i64_value;
            ev.has_u64 = true;
            ev.u64_value = sample.u64_value;
            break;
        case ValueKind::UnsignedInt:
        case ValueKind::Enum:
        case ValueKind::PointerAddress:
            ev.has_u64 = true;
            ev.u64_value = sample.u64_value;
            break;
        case ValueKind::Float64:
            ev.has_f64 = true;
            ev.f64_value = sample.f64_value;
            ev.has_u64 = true;
            ev.u64_value = sample.bits;
            break;
        default:
            ev.has_u64 = true;
            ev.u64_value = sample.u64_value;
            break;
        }
    }

    bool is_dirty_peek_track_poll_suppressed(const TrackDesc& track) const {
        if (!options_.enable_dirty_peek_groups) return false;
        if (track.dirty_peek_group_id == kInvalidIndex) return false;
        if (track.dirty_peek_group_id >= dirty_peek_groups_.size()) return false;
        return dirty_peek_groups_[track.dirty_peek_group_id].dirty_safe;
    }

    bool is_dirty_wave_value_track_poll_suppressed(const TrackDesc& track) const {
        if (!options_.enable_wave_value_dirty) return false;
        if (track.dirty_wave_value_group_id == kInvalidIndex) return false;
        return track.dirty_wave_value_group_id < dirty_wave_value_groups_.size();
    }

    bool is_dirty_wave_array_track_poll_suppressed(const TrackDesc& track) const {
        if (!options_.enable_wave_array_dirty) return false;
        if (track.dirty_wave_array_group_id == kInvalidIndex) return false;
        return track.dirty_wave_array_group_id < dirty_wave_array_groups_.size();
    }

    bool sample_scalar_changed_by_id(TrackId track_id, ScalarSnapshot& sample, const TrackDesc** track_out) {
        if (track_out) *track_out = NULL;
        if (track_id == 0 || track_id >= tracks_.size() || track_id >= track_runtime_.size()) return false;
        TrackDesc& track = tracks_[track_id];
        TrackRuntimeState& runtime = track_runtime_[track_id];
        if (track.id == 0 || !runtime.alive || track.scalar_kind == ScalarSampleKind::None) return false;
        if (is_dirty_peek_track_poll_suppressed(track)) return false;
        if (is_dirty_wave_value_track_poll_suppressed(track)) return false;
        if (is_dirty_wave_array_track_poll_suppressed(track)) return false;
        if (!read_scalar_snapshot(track, sample)) return false;

        if (options_.emit_only_on_change && runtime.has_last_event &&
            runtime.last_invalid == InvalidKind::None && runtime.last_has_change_bits &&
            runtime.last_change_bits == sample.bits) {
            return false;
        }

        runtime.has_last_event = true;
        runtime.last_invalid = InvalidKind::None;
        runtime.last_has_change_bits = true;
        runtime.last_change_bits = sample.bits;
        if (track_out) *track_out = &track;
        return true;
    }

    void invalidate_flat_leaf_fast_table() {
        flat_leaf_fast_table_valid_ = false;
        flat_leaf_fast_table_complete_ = false;
        invalidate_flat_memory_blocks();
    }

    void invalidate_flat_memory_blocks() noexcept {
        // Hot topology-build path: finish_track_create() calls this once per
        // track.  Keep invalidation O(1).  The vectors are cleared/rebuilt
        // lazily in ensure_flat_memory_blocks(); clearing them here turns large
        // topology expansion or late track insertion into repeated O(existing
        // block state) work.
        flat_memory_blocks_valid_ = false;
        flat_memory_blocks_complete_ = false;
        flat_memory_block_summary_logged_ = false;
    }

    bool ensure_flat_leaf_fast_table() {
        if (!options_.enable_flat_leaf_fast_table) return false;
        if (flat_leaf_fast_table_valid_) return flat_leaf_fast_table_complete_;

        flat_leaf_fast_table_.clear();
        flat_leaf_fast_table_.reserve(parallel_track_ids_.size());
        bool complete = true;

        for (std::size_t i = 0; i < parallel_track_ids_.size(); ++i) {
            const TrackId tid = parallel_track_ids_[i];
            if (tid == 0 || tid >= tracks_.size() || tid >= track_runtime_.size()) {
                complete = false;
                continue;
            }
            const TrackDesc& track = tracks_[static_cast<std::size_t>(tid)];
            const TrackRuntimeState& runtime = track_runtime_[static_cast<std::size_t>(tid)];
            if (track.id == 0 || track.scalar_kind == ScalarSampleKind::None) {
                complete = false;
                continue;
            }
            if (is_dirty_peek_track_poll_suppressed(track)) {
                continue;
            }
            if (is_dirty_wave_value_track_poll_suppressed(track)) {
                continue;
            }
            if (is_dirty_wave_array_track_poll_suppressed(track)) {
                continue;
            }

            FlatLeafFast leaf;
            leaf.track_id = tid;
            leaf.storage_id = track.storage_id != 0 ? track.storage_id : tid;
            leaf.sample_ctx = track.sample_ctx;
            leaf.memory_ctx = track.memory_ctx;
            leaf.memory_byte_width = track.memory_byte_width;
            leaf.scalar_reader = track.scalar_reader;
            leaf.scalar_kind = track.scalar_kind;
            leaf.value_kind = track.kind;
            leaf.has_last = runtime.has_last_event &&
                            runtime.last_invalid == InvalidKind::None &&
                            runtime.last_has_change_bits;
            leaf.last_bits = leaf.has_last ? runtime.last_change_bits : 0;
            flat_leaf_fast_table_.push_back(leaf);
        }

        // Dirty-safe peek tracks are intentionally absent from the poll table.
        // Other non-scalar tracks still force fallback.
        flat_leaf_fast_table_complete_ = complete;
        flat_leaf_fast_table_valid_ = true;

        if (options_.debug_log) {
            debug_log_msg(std::string("flat_leaf_fast_table build tracks=") + detail::to_string_unsigned(parallel_track_ids_.size()) +
                          " leaves=" + detail::to_string_unsigned(flat_leaf_fast_table_.size()) +
                          " complete=" + (complete ? "true" : "false"));
        }
        return complete;
    }

    static std::size_t scalar_sample_kind_byte_width(ScalarSampleKind kind) {
        switch (kind) {
        case ScalarSampleKind::Bool: return sizeof(bool);
        case ScalarSampleKind::I8:
        case ScalarSampleKind::U8: return 1;
        case ScalarSampleKind::I16:
        case ScalarSampleKind::U16: return 2;
        case ScalarSampleKind::I32:
        case ScalarSampleKind::U32:
        case ScalarSampleKind::F32: return 4;
        case ScalarSampleKind::I64:
        case ScalarSampleKind::U64:
        case ScalarSampleKind::F64: return 8;
        case ScalarSampleKind::FLongDouble: return sizeof(long double);
        case ScalarSampleKind::None:
        default: return 0;
        }
    }

    static bool read_flat_leaf_snapshot(const FlatLeafFast& leaf, ScalarSnapshot& sample) {
        if (leaf.scalar_reader) return leaf.scalar_reader(leaf.sample_ctx, sample);
        const void* p = leaf.sample_ctx;
        if (!p) return false;
        switch (leaf.scalar_kind) {
        case ScalarSampleKind::Bool: {
            const bool v = *static_cast<const bool*>(p);
            sample.bool_value = v;
            sample.u64_value = v ? 1u : 0u;
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I8: {
            const std::int8_t v = *static_cast<const std::int8_t*>(p);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U8: {
            const std::uint8_t v = *static_cast<const std::uint8_t*>(p);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I16: {
            const std::int16_t v = *static_cast<const std::int16_t*>(p);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U16: {
            const std::uint16_t v = *static_cast<const std::uint16_t*>(p);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I32: {
            const std::int32_t v = *static_cast<const std::int32_t*>(p);
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.u64_value = static_cast<std::uint64_t>(sample.i64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U32: {
            const std::uint32_t v = *static_cast<const std::uint32_t*>(p);
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::I64: {
            const std::int64_t v = *static_cast<const std::int64_t*>(p);
            sample.i64_value = v;
            sample.u64_value = static_cast<std::uint64_t>(v);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::U64: {
            const std::uint64_t v = *static_cast<const std::uint64_t*>(p);
            sample.u64_value = v;
            sample.i64_value = static_cast<std::int64_t>(v);
            sample.bits = sample.u64_value;
            return true;
        }
        case ScalarSampleKind::F32: {
            const float v = *static_cast<const float*>(p);
            sample.f64_value = static_cast<double>(v);
            std::uint32_t bits32 = 0;
            std::memcpy(&bits32, &v, sizeof(bits32));
            sample.bits = static_cast<std::uint64_t>(bits32);
            sample.u64_value = static_cast<std::uint64_t>(bits32);
            return true;
        }
        case ScalarSampleKind::F64: {
            const double v = *static_cast<const double*>(p);
            sample.f64_value = v;
            std::uint64_t bits = 0;
            std::memcpy(&bits, &sample.f64_value, sizeof(bits));
            sample.bits = bits;
            return true;
        }
        case ScalarSampleKind::FLongDouble: {
            const long double v = *static_cast<const long double*>(p);
            sample.f64_value = static_cast<double>(v);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &sample.f64_value, sizeof(bits));
            sample.bits = bits;
            return true;
        }
        case ScalarSampleKind::None:
        default:
            return false;
        }
    }

    static void fill_flat_leaf_event(const FlatLeafFast& leaf, const ScalarSnapshot& sample, Cycle cycle, TrackEvent& ev) {
        ev = TrackEvent();
        ev.cycle = cycle;
        ev.track_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        ev.invalid = InvalidKind::None;
        ev.has_change_bits = true;
        ev.change_bits = sample.bits;
        switch (leaf.value_kind) {
        case ValueKind::Bool:
            ev.has_bool = true;
            ev.bool_value = sample.bool_value;
            break;
        case ValueKind::SignedInt:
            ev.has_i64 = true;
            ev.i64_value = sample.i64_value;
            ev.has_u64 = true;
            ev.u64_value = sample.u64_value;
            break;
        case ValueKind::UnsignedInt:
        case ValueKind::Enum:
        case ValueKind::PointerAddress:
            ev.has_u64 = true;
            ev.u64_value = sample.u64_value;
            break;
        case ValueKind::Float64:
            ev.has_f64 = true;
            ev.f64_value = sample.f64_value;
            ev.has_u64 = true;
            ev.u64_value = sample.bits;
            break;
        default:
            ev.has_u64 = true;
            ev.u64_value = sample.u64_value;
            break;
        }
    }

    bool sample_one_flat_leaf_changed(FlatLeafFast& leaf, ScalarSnapshot& sample) {
        const TrackId state_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        if (state_id == 0 || state_id >= track_runtime_.size()) return false;
        TrackRuntimeState& runtime = track_runtime_[static_cast<std::size_t>(state_id)];
        if (!runtime.alive) return false;

        if (!read_flat_leaf_snapshot(leaf, sample)) return false;
        if (options_.emit_only_on_change && leaf.has_last && leaf.last_bits == sample.bits) {
            return false;
        }

        leaf.has_last = true;
        leaf.last_bits = sample.bits;
        runtime.has_last_event = true;
        runtime.last_invalid = InvalidKind::None;
        runtime.last_has_change_bits = true;
        runtime.last_change_bits = sample.bits;
        return true;
    }


    bool flat_leaf_memory_candidate_(const FlatLeafFast& leaf,
                                     const unsigned char*& begin,
                                     std::size_t& byte_count) const noexcept {
        begin = NULL;
        byte_count = 0;
        const void* memory = leaf.memory_ctx ? leaf.memory_ctx : leaf.sample_ctx;
        if (memory == NULL) return false;
        byte_count = leaf.memory_byte_width != 0
            ? static_cast<std::size_t>(leaf.memory_byte_width)
            : scalar_sample_kind_byte_width(leaf.scalar_kind);
        if (byte_count == 0) return false;
        if (leaf.scalar_reader != NULL && leaf.memory_ctx == NULL) return false;
        begin = static_cast<const unsigned char*>(memory);
        return true;
    }

    bool ensure_flat_memory_blocks() {
        if (!options_.enable_flat_memory_block_precheck) return false;
        if (!ensure_flat_leaf_fast_table()) return false;
        if (flat_memory_blocks_valid_) return flat_memory_blocks_complete_;

        flat_memory_blocks_.clear();
        flat_memory_leaf_refs_.clear();
        flat_memory_scalar_leaf_indices_.clear();
        flat_memory_mixed_leaf_prefix_.clear();
        flat_memory_shadow_bytes_.clear();
        flat_memory_leaf_covered_.assign(flat_leaf_fast_table_.size(), static_cast<unsigned char>(0));

        struct CandidateRef {
            std::uint32_t leaf_index;
            const unsigned char* begin;
            const unsigned char* end;
            std::uintptr_t begin_addr;
            std::uintptr_t end_addr;
            std::size_t byte_count;
            bool needs_initial;
        };

        std::vector<CandidateRef> current_refs;
        current_refs.reserve(64);
        const unsigned char* current_base = NULL;
        const unsigned char* current_end = NULL;
        std::uintptr_t current_base_addr = 0;
        std::uintptr_t current_end_addr = 0;
        bool current_needs_initial = false;

        const std::size_t min_leaf_count = options_.flat_memory_block_min_leaf_count;
        const std::size_t max_gap = options_.flat_memory_block_max_gap;
        const std::size_t max_bytes = options_.flat_memory_block_max_bytes == 0 ? static_cast<std::size_t>(4096) : options_.flat_memory_block_max_bytes;

        const auto flush_current = [&]() {
            if (current_refs.empty()) return;
            const std::size_t block_bytes = static_cast<std::size_t>(current_end_addr - current_base_addr);
            if (current_refs.size() >= min_leaf_count && block_bytes != 0 && block_bytes <= max_bytes) {
                FlatMemoryBlock block;
                block.base = current_base;
                block.byte_count = block_bytes;
                block.shadow_offset = flat_memory_shadow_bytes_.size();
                block.first_leaf_ref = static_cast<std::uint32_t>(flat_memory_leaf_refs_.size());
                block.leaf_count = static_cast<std::uint32_t>(current_refs.size());
                // A newly built memory block must run one precise leaf pass before
                // relying on its byte shadow. Otherwise, if blocks are built after
                // leaf.last_bits already holds a previous-cycle value, initializing
                // the shadow from current memory could hide a change in the build
                // cycle. The precise pass still respects emit_only_on_change.
                block.needs_initial_scan = true;

                const std::size_t old_size = flat_memory_shadow_bytes_.size();
                flat_memory_shadow_bytes_.resize(old_size + block_bytes);
                std::memcpy(&flat_memory_shadow_bytes_[old_size], current_base, block_bytes);

                for (std::size_t j = 0; j < current_refs.size(); ++j) {
                    const CandidateRef& ref = current_refs[j];
                    const std::size_t off = static_cast<std::size_t>(ref.begin_addr - current_base_addr);
                    flat_memory_leaf_refs_.push_back(FlatMemoryLeafRef(ref.leaf_index,
                        static_cast<std::uint32_t>(off),
                        static_cast<std::uint32_t>(ref.byte_count)));
                    if (ref.leaf_index < flat_memory_leaf_covered_.size()) {
                        flat_memory_leaf_covered_[ref.leaf_index] = static_cast<unsigned char>(1);
                    }
                }
                flat_memory_blocks_.push_back(block);
            } else {
                for (std::size_t j = 0; j < current_refs.size(); ++j) {
                    flat_memory_scalar_leaf_indices_.push_back(current_refs[j].leaf_index);
                }
            }
            current_refs.clear();
            current_base = NULL;
            current_end = NULL;
            current_base_addr = 0;
            current_end_addr = 0;
            current_needs_initial = false;
        };

        for (std::size_t i = 0; i < flat_leaf_fast_table_.size(); ++i) {
            const FlatLeafFast& leaf = flat_leaf_fast_table_[i];
            const unsigned char* begin = NULL;
            std::size_t byte_count = 0;
            if (!flat_leaf_memory_candidate_(leaf, begin, byte_count)) {
                flush_current();
                flat_memory_scalar_leaf_indices_.push_back(static_cast<std::uint32_t>(i));
                continue;
            }
            const std::uintptr_t begin_addr = reinterpret_cast<std::uintptr_t>(begin);
            if (byte_count > static_cast<std::size_t>(std::numeric_limits<std::uintptr_t>::max() - begin_addr)) {
                flush_current();
                flat_memory_scalar_leaf_indices_.push_back(static_cast<std::uint32_t>(i));
                continue;
            }
            const std::uintptr_t end_addr = begin_addr + byte_count;
            const unsigned char* end = begin + byte_count;

            bool append_to_current = false;
            if (!current_refs.empty() && begin_addr >= current_end_addr) {
                const std::size_t gap = static_cast<std::size_t>(begin_addr - current_end_addr);
                const std::size_t new_bytes = static_cast<std::size_t>(end_addr - current_base_addr);
                if (gap <= max_gap && new_bytes <= max_bytes) {
                    append_to_current = true;
                }
            }

            if (!append_to_current) {
                flush_current();
                current_base = begin;
                current_end = end;
                current_base_addr = begin_addr;
                current_end_addr = end_addr;
            } else {
                current_end = end;
                current_end_addr = end_addr;
            }

            CandidateRef ref;
            ref.leaf_index = static_cast<std::uint32_t>(i);
            ref.begin = begin;
            ref.end = end;
            ref.begin_addr = begin_addr;
            ref.end_addr = end_addr;
            ref.byte_count = byte_count;
            ref.needs_initial = !leaf.has_last;
            current_needs_initial = current_needs_initial || ref.needs_initial;
            current_refs.push_back(ref);
        }
        flush_current();

        rebuild_flat_memory_mixed_leaf_prefix_();

        flat_memory_blocks_complete_ = true;
        flat_memory_blocks_valid_ = true;

        if (options_.log_flat_memory_block_summary && !flat_memory_block_summary_logged_) {
            std::ofstream os("wave_flat_memory_blocks.log", std::ios::out | std::ios::trunc);
            if (os) {
                os << "flat_leaves=" << static_cast<unsigned long long>(flat_leaf_fast_table_.size())
                   << ",blocks=" << static_cast<unsigned long long>(flat_memory_blocks_.size())
                   << ",block_leaf_refs=" << static_cast<unsigned long long>(flat_memory_leaf_refs_.size())
                   << ",scalar_remainder=" << static_cast<unsigned long long>(flat_memory_scalar_leaf_indices_.size())
                   << ",shadow_bytes=" << static_cast<unsigned long long>(flat_memory_shadow_bytes_.size())
                   << ",requested_simd=" << detail::flat_memory_simd_backend_name(options_.flat_memory_block_simd_backend)
                   << ",selected_simd=" << detail::flat_memory_simd_backend_name(resolve_flat_memory_block_simd_backend_())
                   << ",compiled_simd=" << detail::compiled_flat_memory_simd_backends()
                   << ",cpu_features=" << detail::runtime_flat_memory_simd_features()
                   << '\n';
                std::vector<std::size_t> order(flat_memory_blocks_.size());
                for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
                    const FlatMemoryBlock& ba = flat_memory_blocks_[a];
                    const FlatMemoryBlock& bb = flat_memory_blocks_[b];
                    if (ba.byte_count != bb.byte_count) return ba.byte_count > bb.byte_count;
                    if (ba.leaf_count != bb.leaf_count) return ba.leaf_count > bb.leaf_count;
                    return a < b;
                });
                const std::size_t limit = order.size() < 100 ? order.size() : static_cast<std::size_t>(100);
                os << "top100_by_byte_count\n";
                os << "rank,block_id,leaf_count,byte_count,base,needs_initial\n";
                for (std::size_t rank = 0; rank < limit; ++rank) {
                    const std::size_t block_id = order[rank];
                    const FlatMemoryBlock& b = flat_memory_blocks_[block_id];
                    os << static_cast<unsigned long long>(rank + 1)
                       << ',' << static_cast<unsigned long long>(block_id)
                       << ',' << static_cast<unsigned long long>(b.leaf_count)
                       << ',' << static_cast<unsigned long long>(b.byte_count)
                       << ',' << detail::pointer_to_string(reinterpret_cast<const void*>(b.base))
                       << ',' << (b.needs_initial_scan ? 1 : 0) << '\n';
                }
            }
            flat_memory_block_summary_logged_ = true;
        }

        return true;
    }

    void sample_flat_memory_leaf_ref_precise_(Cycle cycle,
                                              FlatMemoryLeafRef& ref,
                                              std::vector<TrackEvent>& out) {
        if (ref.last_sample_epoch == flat_memory_sample_epoch_) return;
        ref.last_sample_epoch = flat_memory_sample_epoch_;
        if (ref.flat_leaf_index >= flat_leaf_fast_table_.size()) return;

        ScalarSnapshot sample;
        FlatLeafFast& leaf = flat_leaf_fast_table_[ref.flat_leaf_index];
        if (sample_one_flat_leaf_changed(leaf, sample)) {
            out.emplace_back();
            fill_flat_leaf_event(leaf, sample, cycle, out.back());
        }
    }

    void sample_flat_memory_changed_byte_range_(Cycle cycle,
                                                FlatMemoryBlock& block,
                                                std::size_t changed_begin,
                                                std::size_t changed_end,
                                                std::vector<TrackEvent>& out) {
        if (changed_begin >= changed_end) return;
        if (changed_begin >= block.byte_count) return;
        if (changed_end > block.byte_count) changed_end = block.byte_count;

        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        if (begin_ref >= flat_memory_leaf_refs_.size()) return;
        const std::uint32_t safe_end_ref =
            end_ref < flat_memory_leaf_refs_.size()
                ? end_ref
                : static_cast<std::uint32_t>(flat_memory_leaf_refs_.size());
        if (begin_ref >= safe_end_ref) return;

        // Leaf refs inside a block are emitted in monotonically increasing byte
        // offsets.  Avoid scanning the whole block for every changed byte run:
        // jump to the first leaf whose end is after changed_begin, then stop
        // once leaf_begin reaches changed_end.
        std::uint32_t lo = begin_ref;
        std::uint32_t hi = safe_end_ref;
        while (lo < hi) {
            const std::uint32_t mid = lo + static_cast<std::uint32_t>((hi - lo) / 2u);
            const FlatMemoryLeafRef& ref = flat_memory_leaf_refs_[mid];
            const std::size_t leaf_end = static_cast<std::size_t>(ref.offset) +
                                         static_cast<std::size_t>(ref.byte_count);
            if (leaf_end <= changed_begin) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }

        for (std::uint32_t r = lo; r < safe_end_ref; ++r) {
            FlatMemoryLeafRef& ref = flat_memory_leaf_refs_[r];
            const std::size_t leaf_begin = static_cast<std::size_t>(ref.offset);
            if (leaf_begin >= changed_end) break;
            const std::size_t leaf_end = leaf_begin + static_cast<std::size_t>(ref.byte_count);
            if (leaf_begin < changed_end && changed_begin < leaf_end) {
                sample_flat_memory_leaf_ref_precise_(cycle, ref, out);
            }
        }
    }

    void sample_flat_memory_changed_mask32_(Cycle cycle,
                                            FlatMemoryBlock& block,
                                            std::size_t base_off,
                                            std::uint32_t diff_mask,
                                            std::vector<TrackEvent>& out) {
        std::uint32_t pos = 0;
        while (pos < 32) {
            while (pos < 32 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) == 0)) {
                ++pos;
            }
            if (pos >= 32) break;
            const std::uint32_t run_begin = pos;
            while (pos < 32 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) != 0)) {
                ++pos;
            }
            const std::size_t begin = base_off + static_cast<std::size_t>(run_begin);
            const std::size_t end = base_off + static_cast<std::size_t>(pos);
            sample_flat_memory_changed_byte_range_(cycle, block, begin, end, out);
        }
    }

    void sample_flat_memory_changed_mask16_(Cycle cycle,
                                            FlatMemoryBlock& block,
                                            std::size_t base_off,
                                            std::uint32_t diff_mask,
                                            std::vector<TrackEvent>& out) {
        std::uint32_t pos = 0;
        while (pos < 16) {
            while (pos < 16 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) == 0)) {
                ++pos;
            }
            if (pos >= 16) break;
            const std::uint32_t run_begin = pos;
            while (pos < 16 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) != 0)) {
                ++pos;
            }
            const std::size_t begin = base_off + static_cast<std::size_t>(run_begin);
            const std::size_t end = base_off + static_cast<std::size_t>(pos);
            sample_flat_memory_changed_byte_range_(cycle, block, begin, end, out);
        }
    }

    void sample_flat_memory_block_scalar_mask_to_buffer_(Cycle cycle,
                                                         FlatMemoryBlock& block,
                                                         unsigned char* shadow,
                                                         std::vector<TrackEvent>& out,
                                                         std::size_t start_off = 0) {
        std::size_t off = start_off;
        while (off < block.byte_count) {
            if (block.base[off] == shadow[off]) {
                ++off;
                continue;
            }

            const std::size_t run_begin = off;
            do {
                shadow[off] = block.base[off];
                ++off;
            } while (off < block.byte_count && block.base[off] != shadow[off]);

            sample_flat_memory_changed_byte_range_(cycle, block, run_begin, off, out);
        }
    }

    FlatMemoryBlockSimdBackend resolve_flat_memory_block_simd_backend_() const noexcept {
        if (!options_.enable_flat_memory_block_simd_mask) {
            return FlatMemoryBlockSimdBackend::Scalar;
        }

        const detail::WaveCpuFeatures& cpu = detail::wave_cpu_features_();
        const FlatMemoryBlockSimdBackend requested = options_.flat_memory_block_simd_backend;

        if (requested == FlatMemoryBlockSimdBackend::Scalar) {
            return FlatMemoryBlockSimdBackend::Scalar;
        }

        if (requested == FlatMemoryBlockSimdBackend::AVX2) {
#if defined(__AVX2__) || defined(_M_AVX2)
            return cpu.avx2 ? FlatMemoryBlockSimdBackend::AVX2 : FlatMemoryBlockSimdBackend::Scalar;
#else
            return FlatMemoryBlockSimdBackend::Scalar;
#endif
        }

        if (requested == FlatMemoryBlockSimdBackend::SSE2) {
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
            return cpu.sse2 ? FlatMemoryBlockSimdBackend::SSE2 : FlatMemoryBlockSimdBackend::Scalar;
#else
            return FlatMemoryBlockSimdBackend::Scalar;
#endif
        }

        // Auto: choose the best backend that is both compiled into this binary
        // and supported by the current CPU/OS.
#if defined(__AVX2__) || defined(_M_AVX2)
        if (cpu.avx2) {
            return FlatMemoryBlockSimdBackend::AVX2;
        }
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        if (cpu.sse2) {
            return FlatMemoryBlockSimdBackend::SSE2;
        }
#endif
        return FlatMemoryBlockSimdBackend::Scalar;
    }

    void sample_flat_memory_block_simd_mask_to_buffer_(Cycle cycle,
                                                       FlatMemoryBlock& block,
                                                       unsigned char* shadow,
                                                       std::vector<TrackEvent>& out) {
        std::size_t off = 0;
        const FlatMemoryBlockSimdBackend backend =
            (block.byte_count >= options_.flat_memory_block_simd_min_bytes)
                ? resolve_flat_memory_block_simd_backend_()
                : FlatMemoryBlockSimdBackend::Scalar;

#if defined(__AVX2__) || defined(_M_AVX2)
        if (backend == FlatMemoryBlockSimdBackend::AVX2) {
            for (; off + 32 <= block.byte_count; off += 32) {
                const __m256i cur = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block.base + off));
                const __m256i old = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shadow + off));
                const __m256i eq = _mm256_cmpeq_epi8(cur, old);
                const std::uint32_t eq_mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(eq));
                const std::uint32_t diff_mask = (~eq_mask) & static_cast<std::uint32_t>(0xFFFFFFFFu);
                if (diff_mask == 0) continue;

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(shadow + off), cur);
                sample_flat_memory_changed_mask32_(cycle, block, off, diff_mask, out);
            }
        }
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        if (backend == FlatMemoryBlockSimdBackend::SSE2) {
            for (; off + 16 <= block.byte_count; off += 16) {
                const __m128i cur = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block.base + off));
                const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadow + off));
                const __m128i eq = _mm_cmpeq_epi8(cur, old);
                const std::uint32_t eq_mask = static_cast<std::uint32_t>(_mm_movemask_epi8(eq));
                const std::uint32_t diff_mask = (~eq_mask) & static_cast<std::uint32_t>(0xFFFFu);
                if (diff_mask == 0) continue;

                _mm_storeu_si128(reinterpret_cast<__m128i*>(shadow + off), cur);
                sample_flat_memory_changed_mask16_(cycle, block, off, diff_mask, out);
            }
        }
#endif

        sample_flat_memory_block_scalar_mask_to_buffer_(cycle, block, shadow, out, off);
    }

    void sample_flat_memory_block_initial_to_buffer_(Cycle cycle,
                                                    FlatMemoryBlock& block,
                                                    unsigned char* shadow,
                                                    std::vector<TrackEvent>& out) {
        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        for (std::uint32_t r = begin_ref; r < end_ref && r < flat_memory_leaf_refs_.size(); ++r) {
            sample_flat_memory_leaf_ref_precise_(cycle, flat_memory_leaf_refs_[r], out);
        }
        std::memcpy(shadow, block.base, block.byte_count);
        block.needs_initial_scan = false;
    }

    void sample_flat_memory_block_to_buffer(Cycle cycle, FlatMemoryBlock& block, std::vector<TrackEvent>& out) {
        if (block.base == NULL || block.byte_count == 0) return;
        unsigned char* shadow = flat_memory_shadow_bytes_.empty() ? NULL : &flat_memory_shadow_bytes_[block.shadow_offset];
        if (!shadow) return;

        if (block.needs_initial_scan) {
            sample_flat_memory_block_initial_to_buffer_(cycle, block, shadow, out);
            return;
        }

        // No memcmp here: current/shadow bytes are compared once, using SIMD
        // byte masks when available, and the shadow is updated in the same pass.
        sample_flat_memory_block_simd_mask_to_buffer_(cycle, block, shadow, out);
    }

    void rebuild_flat_memory_mixed_leaf_prefix_() {
        const std::size_t block_count = flat_memory_blocks_.size();
        const std::size_t scalar_count = flat_memory_scalar_leaf_indices_.size();
        flat_memory_mixed_leaf_prefix_.clear();
        flat_memory_mixed_leaf_prefix_.reserve(block_count + scalar_count + 1u);
        flat_memory_mixed_leaf_prefix_.push_back(0u);
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < block_count; ++i) {
            acc += static_cast<std::uint64_t>(flat_memory_blocks_[i].leaf_count);
            flat_memory_mixed_leaf_prefix_.push_back(acc);
        }
        for (std::size_t i = 0; i < scalar_count; ++i) {
            ++acc;
            flat_memory_mixed_leaf_prefix_.push_back(acc);
        }
    }

    std::size_t flat_memory_mixed_item_count_() const noexcept {
        return flat_memory_blocks_.size() + flat_memory_scalar_leaf_indices_.size();
    }

    std::uint64_t flat_memory_mixed_leaf_weight_total_() const noexcept {
        return flat_memory_mixed_leaf_prefix_.empty()
            ? static_cast<std::uint64_t>(flat_memory_mixed_item_count_())
            : flat_memory_mixed_leaf_prefix_.back();
    }

    std::size_t flat_memory_mixed_pos_for_leaf_weight_(std::uint64_t weight) const {
        const std::size_t total = flat_memory_mixed_item_count_();
        if (total == 0) return 0;
        if (flat_memory_mixed_leaf_prefix_.size() != total + 1u) {
            return weight >= static_cast<std::uint64_t>(total)
                ? total
                : static_cast<std::size_t>(weight);
        }
        const std::uint64_t max_weight = flat_memory_mixed_leaf_prefix_.back();
        if (weight == 0) return 0;
        if (weight >= max_weight) return total;
        std::vector<std::uint64_t>::const_iterator it =
            std::lower_bound(flat_memory_mixed_leaf_prefix_.begin(),
                             flat_memory_mixed_leaf_prefix_.end(),
                             weight);
        std::size_t pos = static_cast<std::size_t>(it - flat_memory_mixed_leaf_prefix_.begin());
        if (pos > total) pos = total;
        return pos;
    }

    std::size_t flat_memory_mixed_slice_event_capacity(std::size_t begin_pos, std::size_t end_pos) const {
        const std::size_t total = flat_memory_mixed_item_count_();
        if (begin_pos > total) begin_pos = total;
        if (end_pos > total) end_pos = total;
        if (end_pos <= begin_pos) return 0;
        if (flat_memory_mixed_leaf_prefix_.size() == total + 1u) {
            return static_cast<std::size_t>(flat_memory_mixed_leaf_prefix_[end_pos] -
                                            flat_memory_mixed_leaf_prefix_[begin_pos]);
        }
        std::size_t cap = 0;
        const std::size_t block_count = flat_memory_blocks_.size();
        for (std::size_t pos = begin_pos; pos < end_pos; ++pos) {
            if (pos < block_count) {
                cap += flat_memory_blocks_[pos].leaf_count;
            } else {
                ++cap;
            }
        }
        return cap;
    }

    void sample_flat_memory_mixed_slice_to_buffer(Cycle cycle, std::size_t begin_pos, std::size_t end_pos, std::vector<TrackEvent>& out) {
        const std::size_t block_count = flat_memory_blocks_.size();
        const std::size_t scalar_count = flat_memory_scalar_leaf_indices_.size();
        const std::size_t total = block_count + scalar_count;
        if (begin_pos > total) begin_pos = total;
        if (end_pos > total) end_pos = total;
        for (std::size_t pos = begin_pos; pos < end_pos; ++pos) {
            if (pos < block_count) {
                sample_flat_memory_block_to_buffer(cycle, flat_memory_blocks_[pos], out);
            } else {
                const std::size_t scalar_pos = pos - block_count;
                if (scalar_pos >= flat_memory_scalar_leaf_indices_.size()) continue;
                const std::uint32_t leaf_index = flat_memory_scalar_leaf_indices_[scalar_pos];
                if (leaf_index >= flat_leaf_fast_table_.size()) continue;
                ScalarSnapshot sample;
                FlatLeafFast& leaf = flat_leaf_fast_table_[leaf_index];
                if (sample_one_flat_leaf_changed(leaf, sample)) {
                    out.emplace_back();
                    fill_flat_leaf_event(leaf, sample, cycle, out.back());
                }
            }
        }
    }

    void sample_flat_memory_mixed_slice_to_sink(Cycle cycle, std::size_t begin_pos, std::size_t end_pos) {
        caller_parallel_events_.clear();
        sample_flat_memory_mixed_slice_to_buffer(cycle, begin_pos, end_pos, caller_parallel_events_);
        for (std::size_t i = 0; i < caller_parallel_events_.size(); ++i) {
            sink_.on_sample(caller_parallel_events_[i]);
        }
    }

    void begin_flat_memory_sample_epoch_() {
        ++flat_memory_sample_epoch_;
        if (flat_memory_sample_epoch_ == 0) {
            flat_memory_sample_epoch_ = 1;
            for (std::size_t i = 0; i < flat_memory_leaf_refs_.size(); ++i) {
                flat_memory_leaf_refs_[i].last_sample_epoch = 0;
            }
        }
    }

    void sample_flat_memory_blocks_fast(Cycle cycle) {
        if (!ensure_flat_memory_blocks()) {
            sample_flat_leaf_slice_to_sink(cycle, 0, flat_leaf_fast_table_.size());
            return;
        }
        begin_flat_memory_sample_epoch_();
        const std::size_t total = flat_memory_mixed_item_count_();
        if (total == 0) return;
        const std::uint64_t total_leaf_weight = flat_memory_mixed_leaf_weight_total_();

        if (!should_use_parallel_flat_leaf_sampling()) {
            sample_flat_memory_mixed_slice_to_sink(cycle, 0, total);
            return;
        }

        const std::size_t worker_count = desired_worker_count_for_items(total);
        if (worker_count == 0) {
            sample_flat_memory_mixed_slice_to_sink(cycle, 0, total);
            return;
        }
        ensure_parallel_workers(worker_count);

        const bool log_load = should_log_parallel_flat_leaf_load_(cycle);
        const std::size_t participants = worker_count + 1;

        std::size_t cursor = 0;
        for (std::size_t i = 0; i < worker_count; ++i) {
            const std::uint64_t end_weight =
                (total_leaf_weight * static_cast<std::uint64_t>(i + 1u)) /
                static_cast<std::uint64_t>(participants);
            const std::size_t begin_pos = cursor;
            std::size_t end_pos = flat_memory_mixed_pos_for_leaf_weight_(end_weight);
            if (end_pos < begin_pos) end_pos = begin_pos;
            cursor = end_pos;

            SampleWorker* worker = parallel_workers_[i].get();
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                reserve_worker_event_capacity(worker, flat_memory_mixed_slice_event_capacity(begin_pos, end_pos));
                worker->cycle = cycle;
                worker->job_kind = WorkerJobKind::FlatMemoryMixedSlice;
                worker->begin_pos = begin_pos;
                worker->end_pos = end_pos;
                worker->events = NULL;
                worker->debug_scanned = 0;
                worker->debug_changed = 0;
                worker->debug_time_us = 0;
                worker->done = false;
                worker->has_job = true;
            }
            worker->cv.notify_one();
        }

        const std::size_t caller_begin = cursor;
        const std::size_t caller_span = total - cursor;
        const std::size_t caller_event_capacity = flat_memory_mixed_slice_event_capacity(caller_begin, total);
        const std::size_t caller_scanned_leaf_count = caller_event_capacity;
        if (caller_parallel_events_.capacity() < caller_event_capacity) {
            caller_parallel_events_.reserve(caller_event_capacity);
        }
        caller_parallel_events_.clear();
        const std::chrono::high_resolution_clock::time_point caller_t0 = std::chrono::high_resolution_clock::now();
        sample_flat_memory_mixed_slice_to_buffer(cycle, caller_begin, total, caller_parallel_events_);
        const std::chrono::high_resolution_clock::time_point caller_t1 = std::chrono::high_resolution_clock::now();
        const std::uint64_t caller_time_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(caller_t1 - caller_t0).count());
        const std::size_t caller_changed = caller_parallel_events_.size();

        struct LocalWorkerStat {
            std::size_t begin_pos;
            std::size_t end_pos;
            std::size_t scanned;
            std::size_t changed;
            std::uint64_t time_us;
        };
        std::vector<LocalWorkerStat> worker_stats;
        if (log_load) worker_stats.reserve(worker_count);

        std::size_t total_events = caller_changed;
        const std::chrono::high_resolution_clock::time_point flush_t0 = std::chrono::high_resolution_clock::now();
        for (std::size_t i = 0; i < worker_count; ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            std::vector<TrackEvent>* events = NULL;
            LocalWorkerStat stat = {0, 0, 0, 0, 0};
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->cv.wait(lock, [worker]() { return worker->done; });
                events = worker->events;
                stat.begin_pos = worker->begin_pos;
                stat.end_pos = worker->end_pos;
                stat.scanned = worker->debug_scanned;
                stat.changed = worker->debug_changed;
                stat.time_us = worker->debug_time_us;
            }
            if (log_load) worker_stats.push_back(stat);
            if (!events) continue;
            total_events += events->size();
            for (std::size_t j = 0; j < events->size(); ++j) {
                sink_.on_sample((*events)[j]);
            }
        }
        for (std::size_t j = 0; j < caller_parallel_events_.size(); ++j) {
            sink_.on_sample(caller_parallel_events_[j]);
        }
        const std::chrono::high_resolution_clock::time_point flush_t1 = std::chrono::high_resolution_clock::now();
        const std::uint64_t flush_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(flush_t1 - flush_t0).count());

        if (log_load) {
            std::ostream& os = parallel_flat_leaf_load_output_stream_();
            os << "cycle=" << static_cast<unsigned long long>(cycle)
               << ",mode=memory_block"
               << ",total_items=" << static_cast<unsigned long long>(total)
               << ",blocks=" << static_cast<unsigned long long>(flat_memory_blocks_.size())
               << ",scalar_remainder=" << static_cast<unsigned long long>(flat_memory_scalar_leaf_indices_.size())
               << ",worker_count=" << static_cast<unsigned long long>(worker_count)
               << ",participants=" << static_cast<unsigned long long>(participants)
               << ",total_events=" << static_cast<unsigned long long>(total_events)
               << ",flush_us=" << static_cast<unsigned long long>(flush_us) << '\n';
            for (std::size_t i = 0; i < worker_stats.size(); ++i) {
                const LocalWorkerStat& st = worker_stats[i];
                const double events_per_1000 = st.scanned ? (1000.0 * static_cast<double>(st.changed) / static_cast<double>(st.scanned)) : 0.0;
                const double items_per_us = st.time_us ? (static_cast<double>(st.scanned) / static_cast<double>(st.time_us)) : 0.0;
                os << "worker," << static_cast<unsigned long long>(i)
                   << ',' << static_cast<unsigned long long>(st.begin_pos)
                   << ',' << static_cast<unsigned long long>(st.end_pos)
                   << ',' << static_cast<unsigned long long>(st.scanned)
                   << ',' << static_cast<unsigned long long>(st.changed)
                   << ',' << static_cast<unsigned long long>(st.time_us)
                   << ',' << events_per_1000
                   << ',' << items_per_us << '\n';
            }
            const double caller_events_per_1000 = caller_scanned_leaf_count ? (1000.0 * static_cast<double>(caller_changed) / static_cast<double>(caller_scanned_leaf_count)) : 0.0;
            const double caller_items_per_us = caller_time_us ? (static_cast<double>(caller_scanned_leaf_count) / static_cast<double>(caller_time_us)) : 0.0;
            os << "caller," << static_cast<unsigned long long>(worker_count)
               << ',' << static_cast<unsigned long long>(caller_begin)
               << ',' << static_cast<unsigned long long>(total)
               << ',' << static_cast<unsigned long long>(caller_scanned_leaf_count)
               << ',' << static_cast<unsigned long long>(caller_changed)
               << ',' << static_cast<unsigned long long>(caller_time_us)
               << ',' << caller_events_per_1000
               << ',' << caller_items_per_us << '\n';
            os << std::flush;
        }
    }

    void sample_flat_leaf_slice_to_buffer(Cycle cycle, std::size_t begin_pos, std::size_t end_pos, std::vector<TrackEvent>& out) {
        const std::size_t count = flat_leaf_fast_table_.size();
        if (begin_pos > count) begin_pos = count;
        if (end_pos > count) end_pos = count;
        for (std::size_t i = begin_pos; i < end_pos; ++i) {
            ScalarSnapshot sample;
            FlatLeafFast& leaf = flat_leaf_fast_table_[i];
            if (sample_one_flat_leaf_changed(leaf, sample)) {
                out.emplace_back();
                fill_flat_leaf_event(leaf, sample, cycle, out.back());
            }
        }
    }

    void sample_flat_leaf_slice_to_sink(Cycle cycle, std::size_t begin_pos, std::size_t end_pos) {
        const std::size_t count = flat_leaf_fast_table_.size();
        if (begin_pos > count) begin_pos = count;
        if (end_pos > count) end_pos = count;
        for (std::size_t i = begin_pos; i < end_pos; ++i) {
            ScalarSnapshot sample;
            FlatLeafFast& leaf = flat_leaf_fast_table_[i];
            if (sample_one_flat_leaf_changed(leaf, sample)) {
                TrackEvent ev;
                fill_flat_leaf_event(leaf, sample, cycle, ev);
                sink_.on_sample(ev);
            }
        }
    }

    bool should_use_parallel_flat_leaf_sampling() const {
        if (!options_.enable_parallel_sampling) return false;
        if (options_.debug_log || options_.debug_log_track_samples || options_.debug_log_root_expand_stats) return false;
        if (flat_leaf_fast_table_.size() <= options_.parallel_flat_leaf_threshold) return false;
        return desired_worker_count_for_items(flat_leaf_fast_table_.size()) > 0;
    }

    void reserve_worker_event_capacity(SampleWorker* worker, std::size_t capacity) {
        if (!worker) return;
        if (worker->event_buffer.capacity() < capacity) {
            worker->event_buffer.reserve(capacity);
        }
    }

    void sample_flat_leaf_fast(Cycle cycle) {
        const std::size_t total = flat_leaf_fast_table_.size();
        if (total == 0) return;

        if (options_.enable_flat_memory_block_precheck) {
            sample_flat_memory_blocks_fast(cycle);
            return;
        }

        if (!should_use_parallel_flat_leaf_sampling()) {
            sample_flat_leaf_slice_to_sink(cycle, 0, total);
            return;
        }

        const std::size_t worker_count = desired_worker_count_for_items(total);
        if (worker_count == 0) {
            sample_flat_leaf_slice_to_sink(cycle, 0, total);
            return;
        }
        ensure_parallel_workers(worker_count);

        const bool log_load = should_log_parallel_flat_leaf_load_(cycle);
        const std::size_t participants = worker_count + 1;
        const std::size_t base = total / participants;
        const std::size_t extra = total % participants;

        std::size_t cursor = 0;
        for (std::size_t i = 0; i < worker_count; ++i) {
            const std::size_t span = base + (i < extra ? static_cast<std::size_t>(1) : static_cast<std::size_t>(0));
            const std::size_t begin_pos = cursor;
            const std::size_t end_pos = begin_pos + span;
            cursor = end_pos;

            SampleWorker* worker = parallel_workers_[i].get();
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                reserve_worker_event_capacity(worker, span);
                worker->cycle = cycle;
                worker->job_kind = WorkerJobKind::FlatLeafSlice;
                worker->begin_pos = begin_pos;
                worker->end_pos = end_pos;
                worker->events = NULL;
                worker->debug_scanned = 0;
                worker->debug_changed = 0;
                worker->debug_time_us = 0;
                worker->done = false;
                worker->has_job = true;
            }
            worker->cv.notify_one();
        }

        const std::size_t caller_begin = cursor;
        const std::size_t caller_span = total - cursor;
        if (caller_parallel_events_.capacity() < caller_span) {
            caller_parallel_events_.reserve(caller_span);
        }
        caller_parallel_events_.clear();
        const std::chrono::high_resolution_clock::time_point caller_t0 = std::chrono::high_resolution_clock::now();
        sample_flat_leaf_slice_to_buffer(cycle, caller_begin, total, caller_parallel_events_);
        const std::chrono::high_resolution_clock::time_point caller_t1 = std::chrono::high_resolution_clock::now();
        const std::uint64_t caller_time_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(caller_t1 - caller_t0).count());
        const std::size_t caller_changed = caller_parallel_events_.size();

        struct LocalWorkerStat {
            std::size_t begin_pos;
            std::size_t end_pos;
            std::size_t scanned;
            std::size_t changed;
            std::uint64_t time_us;
            std::vector<TrackEvent>* events;
        };

        std::vector<LocalWorkerStat> worker_stats;
        if (log_load) worker_stats.reserve(worker_count);

        std::size_t total_events = caller_changed;
        const std::chrono::high_resolution_clock::time_point flush_t0 = std::chrono::high_resolution_clock::now();

        // Submit only from the caller thread. Workers only read/compare disjoint
        // leaf ranges and write their own private buffers, so recorder/sink state
        // remains single-threaded and no sink-side locking is required.
        for (std::size_t i = 0; i < worker_count; ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            std::vector<TrackEvent>* events = NULL;
            LocalWorkerStat stat;
            stat.begin_pos = 0;
            stat.end_pos = 0;
            stat.scanned = 0;
            stat.changed = 0;
            stat.time_us = 0;
            stat.events = NULL;
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->cv.wait(lock, [worker]() { return worker->done; });
                events = worker->events;
                stat.begin_pos = worker->begin_pos;
                stat.end_pos = worker->end_pos;
                stat.scanned = worker->debug_scanned;
                stat.changed = worker->debug_changed;
                stat.time_us = worker->debug_time_us;
                stat.events = events;
            }
            if (log_load) worker_stats.push_back(stat);
            if (!events) continue;
            total_events += events->size();
            for (std::size_t j = 0; j < events->size(); ++j) {
                sink_.on_sample((*events)[j]);
            }
        }

        for (std::size_t j = 0; j < caller_parallel_events_.size(); ++j) {
            sink_.on_sample(caller_parallel_events_[j]);
        }

        const std::chrono::high_resolution_clock::time_point flush_t1 = std::chrono::high_resolution_clock::now();
        const std::uint64_t flush_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(flush_t1 - flush_t0).count());

        if (log_load) {
            std::ostream& os = parallel_flat_leaf_load_output_stream_();
            os << "cycle=" << static_cast<unsigned long long>(cycle)
               << ",total_flat_leaves=" << static_cast<unsigned long long>(total)
               << ",worker_count=" << static_cast<unsigned long long>(worker_count)
               << ",participants=" << static_cast<unsigned long long>(participants)
               << ",total_events=" << static_cast<unsigned long long>(total_events)
               << ",flush_us=" << static_cast<unsigned long long>(flush_us) << '\n';

            for (std::size_t i = 0; i < worker_stats.size(); ++i) {
                const LocalWorkerStat& st = worker_stats[i];
                const double events_per_1000 = st.scanned ? (1000.0 * static_cast<double>(st.changed) / static_cast<double>(st.scanned)) : 0.0;
                const double leaves_per_us = st.time_us ? (static_cast<double>(st.scanned) / static_cast<double>(st.time_us)) : 0.0;
                os << "worker," << static_cast<unsigned long long>(i)
                   << ',' << static_cast<unsigned long long>(st.begin_pos)
                   << ',' << static_cast<unsigned long long>(st.end_pos)
                   << ',' << static_cast<unsigned long long>(st.scanned)
                   << ',' << static_cast<unsigned long long>(st.changed)
                   << ',' << static_cast<unsigned long long>(st.time_us)
                   << ',' << events_per_1000
                   << ',' << leaves_per_us << '\n';
            }

            const double caller_events_per_1000 = caller_span ? (1000.0 * static_cast<double>(caller_changed) / static_cast<double>(caller_span)) : 0.0;
            const double caller_leaves_per_us = caller_time_us ? (static_cast<double>(caller_span) / static_cast<double>(caller_time_us)) : 0.0;
            os << "caller," << static_cast<unsigned long long>(worker_count)
               << ',' << static_cast<unsigned long long>(caller_begin)
               << ',' << static_cast<unsigned long long>(total)
               << ',' << static_cast<unsigned long long>(caller_span)
               << ',' << static_cast<unsigned long long>(caller_changed)
               << ',' << static_cast<unsigned long long>(caller_time_us)
               << ',' << caller_events_per_1000
               << ',' << caller_leaves_per_us << '\n';
            os << std::flush;
        }
    }

    void sample_track_id_slice_to_buffer(Cycle cycle, std::size_t begin_pos, std::size_t end_pos, std::vector<TrackEvent>& out) {
        const std::size_t count = parallel_track_ids_.size();
        if (begin_pos > count) begin_pos = count;
        if (end_pos > count) end_pos = count;
        for (std::size_t pos = begin_pos; pos < end_pos; ++pos) {
            const TrackId tid = parallel_track_ids_[pos];
            ScalarSnapshot sample;
            const TrackDesc* track = NULL;
            if (sample_scalar_changed_by_id(tid, sample, &track) && track) {
                out.emplace_back();
                fill_scalar_event(*track, sample, cycle, out.back());
            }
        }
    }

    void sample_track_ids_to_sink(Cycle cycle, const std::vector<TrackId>& ids, bool) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            ScalarSnapshot sample;
            const TrackDesc* track = NULL;
            if (sample_scalar_changed_by_id(ids[i], sample, &track) && track) {
                TrackEvent ev;
                fill_scalar_event(*track, sample, cycle, ev);
                sink_.on_sample(ev);
            }
        }
    }

    void ensure_dirty_tls_capacity_for_current_thread() {
        ThreadTraceLocal& tls = current_thread_trace_local();
        preserve_tls_dirty_before_owner_switch_(tls, this);
        tls.attach(this, dirty_peek_groups_.size(), dirty_epoch_);
    }

    bool try_ensure_dirty_tls_capacity_for_current_thread() noexcept {
        try {
            ThreadTraceLocal& tls = current_thread_trace_local();
            if (tls.owner != this) {
                preserve_tls_dirty_before_owner_switch_(tls, this);
                tls.attach(this, dirty_peek_groups_.size(), dirty_epoch_);
            } else {
                tls.ensure_capacity(dirty_peek_groups_.size());
            }
            return true;
        } catch (...) {
            return false;
        }
    }


    void ensure_wave_value_tls_capacity_for_current_thread() {
        ThreadTraceLocal& tls = current_thread_trace_local();
        preserve_tls_dirty_before_owner_switch_(tls, this);
        tls.attach_wave_values(this, dirty_wave_value_groups_.size(), dirty_epoch_);
    }

    bool try_ensure_wave_value_tls_capacity_for_current_thread() noexcept {
        try {
            ThreadTraceLocal& tls = current_thread_trace_local();
            if (tls.owner != this) {
                preserve_tls_dirty_before_owner_switch_(tls, this);
                tls.attach_wave_values(this, dirty_wave_value_groups_.size(), dirty_epoch_);
            } else {
                tls.ensure_wave_value_capacity(dirty_wave_value_groups_.size());
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void ensure_wave_array_tls_capacity_for_current_thread() {
        ThreadTraceLocal& tls = current_thread_trace_local();
        preserve_tls_dirty_before_owner_switch_(tls, this);
        tls.attach_wave_arrays(this, dirty_wave_array_groups_.size(), dirty_epoch_);
    }

    bool try_ensure_wave_array_tls_capacity_for_current_thread() noexcept {
        try {
            ThreadTraceLocal& tls = current_thread_trace_local();
            if (tls.owner != this) {
                preserve_tls_dirty_before_owner_switch_(tls, this);
                tls.attach_wave_arrays(this, dirty_wave_array_groups_.size(), dirty_epoch_);
            } else {
                tls.ensure_wave_array_capacity(dirty_wave_array_groups_.size());
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void mark_dirty_wave_array_group(std::uint32_t group_id) noexcept {
        if (!options_.enable_wave_array_dirty) return;
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) return;
        ThreadTraceLocal& tls = current_thread_trace_local();
        if (tls.owner != this || group_id >= tls.wave_array_local_epoch.size() || group_id >= tls.wave_array_dirty_ids.size()) {
            if (!try_ensure_wave_array_tls_capacity_for_current_thread()) return;
        }
        if (group_id >= tls.wave_array_local_epoch.size() || group_id >= tls.wave_array_dirty_ids.size()) return;
        if (tls.wave_array_local_epoch[group_id] == dirty_epoch_) return;
        tls.wave_array_local_epoch[group_id] = dirty_epoch_;
        if (tls.wave_array_dirty_count < tls.wave_array_dirty_ids.size()) {
            tls.wave_array_dirty_ids[tls.wave_array_dirty_count++] = group_id;
        }
    }

    void mark_dirty_wave_value_group(std::uint32_t group_id) noexcept {
        if (!options_.enable_wave_value_dirty) return;
        if (group_id == kInvalidIndex || group_id >= dirty_wave_value_groups_.size()) return;
        ThreadTraceLocal& tls = current_thread_trace_local();
        if (tls.owner != this || group_id >= tls.wave_value_local_epoch.size() || group_id >= tls.wave_value_dirty_ids.size()) {
            if (!try_ensure_wave_value_tls_capacity_for_current_thread()) return;
        }
        if (group_id >= tls.wave_value_local_epoch.size() || group_id >= tls.wave_value_dirty_ids.size()) return;
        if (tls.wave_value_local_epoch[group_id] == dirty_epoch_) return;
        tls.wave_value_local_epoch[group_id] = dirty_epoch_;
        if (tls.wave_value_dirty_count < tls.wave_value_dirty_ids.size()) {
            tls.wave_value_dirty_ids[tls.wave_value_dirty_count++] = group_id;
        }
    }

    void reset_dirty_epochs_after_wrap() {
        for (std::size_t i = 0; i < dirty_peek_groups_.size(); ++i) {
            dirty_peek_groups_[i].queued_epoch = 0;
        }
        for (std::size_t i = 0; i < dirty_wave_value_groups_.size(); ++i) {
            dirty_wave_value_groups_[i].queued_epoch = 0;
        }
        for (std::size_t i = 0; i < dirty_wave_array_groups_.size(); ++i) {
            dirty_wave_array_groups_[i].queued_epoch = 0;
        }
        for (std::size_t i = 0; i < flat_memory_leaf_refs_.size(); ++i) {
            flat_memory_leaf_refs_[i].last_sample_epoch = 0;
        }
        WaveTlsRegistry::instance().for_each_tls_locked([&](ThreadTraceLocal* tls) {
            if (tls && tls->owner == this) {
                std::fill(tls->local_epoch.begin(), tls->local_epoch.end(), 0);
                std::fill(tls->wave_value_local_epoch.begin(), tls->wave_value_local_epoch.end(), 0);
                tls->dirty_count = 0;
                tls->wave_value_dirty_count = 0;
                std::fill(tls->wave_array_local_epoch.begin(), tls->wave_array_local_epoch.end(), 0);
                tls->wave_array_dirty_count = 0;
            }
        });
    }

    void drain_retired_dirty_peek_groups_() {
        std::lock_guard<std::mutex> lock(retired_dirty_mu_);
        for (std::size_t i = 0; i < retired_dirty_group_ids_.size(); ++i) {
            const std::uint32_t group_id = retired_dirty_group_ids_[i];
            if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) continue;
            DirtyPeekGroup& group = dirty_peek_groups_[group_id];
            if (!group.dirty_safe) continue;
            if (group.queued_epoch == dirty_epoch_) continue;
            group.queued_epoch = dirty_epoch_;
            if (global_dirty_group_count_ < global_dirty_group_ids_.size()) {
                global_dirty_group_ids_[global_dirty_group_count_++] = group_id;
            }
        }
        retired_dirty_group_ids_.clear();
    }

    void invalidate_dirty_peek_memory_blocks() noexcept {
        // Hot topology-build path: this may be called once per reflected leaf.
        // Keep it O(1).  The actual vectors and per-group block ranges are
        // cleared/rebuilt lazily in ensure_dirty_peek_memory_blocks().
        dirty_peek_memory_blocks_valid_ = false;
        dirty_peek_memory_blocks_complete_ = false;
    }

    void invalidate_dirty_wave_array_memory_blocks() noexcept {
        // Hot topology-build path: wave::array may create one dirty leaf per
        // scalar element/member.  Do not clear vectors or walk all groups here;
        // doing so makes expansion O(groups * leaves).  The actual rebuild is
        // performed once, lazily, by ensure_dirty_wave_array_memory_blocks().
        dirty_wave_array_memory_blocks_valid_ = false;
        dirty_wave_array_memory_blocks_complete_ = false;
    }

    void collect_dirty_peek_groups_from_tls() {
        global_dirty_group_count_ = 0;
        if (global_dirty_group_ids_.size() < dirty_peek_groups_.size()) {
            global_dirty_group_ids_.resize(dirty_peek_groups_.size());
        }

        WaveTlsRegistry::instance().for_each_tls_locked([&](ThreadTraceLocal* tls) {
            if (!tls || tls->owner != this || tls->dirty_count == 0) return;
            const std::uint32_t count = tls->dirty_count;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t group_id = tls->dirty_ids[i];
                if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) continue;
                DirtyPeekGroup& group = dirty_peek_groups_[group_id];
                if (!group.dirty_safe) continue;
                if (group.queued_epoch == dirty_epoch_) continue;
                group.queued_epoch = dirty_epoch_;
                if (global_dirty_group_count_ < global_dirty_group_ids_.size()) {
                    global_dirty_group_ids_[global_dirty_group_count_++] = group_id;
                }
            }
            tls->dirty_count = 0;
        });
        drain_retired_dirty_peek_groups_();
    }

    void append_initial_dirty_peek_groups() {
        if (dirty_peek_initial_sample_done_) return;
        if (global_dirty_group_ids_.size() < dirty_peek_groups_.size()) {
            global_dirty_group_ids_.resize(dirty_peek_groups_.size());
        }
        for (std::uint32_t group_id = 0; group_id < dirty_peek_groups_.size(); ++group_id) {
            DirtyPeekGroup& group = dirty_peek_groups_[group_id];
            if (!group.dirty_safe) continue;
            if (group.queued_epoch == dirty_epoch_) continue;
            group.queued_epoch = dirty_epoch_;
            if (global_dirty_group_count_ < global_dirty_group_ids_.size()) {
                global_dirty_group_ids_[global_dirty_group_count_++] = group_id;
            }
        }
        dirty_peek_initial_sample_done_ = true;
    }


    void invalidate_dirty_wave_value_addr_lookup() noexcept {
        dirty_wave_value_addr_table_sorted_ = false;
        dirty_wave_value_addr_hash_built_ = false;
        dirty_wave_value_addr_hash_mask_ = 0;
    }

    static std::size_t next_power_of_two_size_(std::size_t n) noexcept {
        if (n <= 1) return 1;
        --n;
        for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
            n |= (n >> shift);
        }
        return n + 1;
    }

    static std::size_t hash_wave_value_address_(const void* address) noexcept {
        std::uintptr_t x = reinterpret_cast<std::uintptr_t>(address);
        // Pointer values are usually aligned, so mix before masking.  This is a
        // small splitmix-style finalizer with no allocation and no dependency on
        // std::hash implementation quality.
        x ^= (x >> 33);
        x *= static_cast<std::uintptr_t>(0xff51afd7ed558ccdull);
        x ^= (x >> 33);
        x *= static_cast<std::uintptr_t>(0xc4ceb9fe1a85ec53ull);
        x ^= (x >> 33);
        return static_cast<std::size_t>(x);
    }

    void ensure_dirty_wave_value_addr_table_sorted() const {
        Tracer* self = const_cast<Tracer*>(this);
        if (self->dirty_wave_value_addr_table_sorted_) return;
        std::less<const void*> less;
        std::sort(self->dirty_wave_value_addr_table_.begin(), self->dirty_wave_value_addr_table_.end(),
                  [less](const DirtyWaveValueAddressEntry& a, const DirtyWaveValueAddressEntry& b) {
                      return less(a.address, b.address);
                  });
        self->dirty_wave_value_addr_table_sorted_ = true;
    }

    void ensure_dirty_wave_value_addr_hash_table() const {
        Tracer* self = const_cast<Tracer*>(this);
        if (!self->options_.enable_wave_value_address_hash ||
            self->dirty_wave_value_addr_table_.size() < self->options_.wave_value_address_hash_min_entries) {
            self->dirty_wave_value_addr_hash_table_.clear();
            self->dirty_wave_value_addr_hash_built_ = false;
            self->dirty_wave_value_addr_hash_mask_ = 0;
            return;
        }
        if (self->dirty_wave_value_addr_hash_built_) return;

        // Keep load factor <= 0.5 so the write hot path usually resolves in one
        // or two probes.  The table is built during topology preparation, not in
        // WaveValue::operator=().
        std::size_t slot_count = next_power_of_two_size_(self->dirty_wave_value_addr_table_.size() * 2u);
        if (slot_count < 16u) slot_count = 16u;
        self->dirty_wave_value_addr_hash_table_.assign(slot_count, DirtyWaveValueAddressHashSlot());
        self->dirty_wave_value_addr_hash_mask_ = slot_count - 1u;

        for (std::size_t i = 0; i < self->dirty_wave_value_addr_table_.size(); ++i) {
            const DirtyWaveValueAddressEntry& entry = self->dirty_wave_value_addr_table_[i];
            if (!entry.address || entry.group_id == kInvalidIndex) continue;
            std::size_t slot = hash_wave_value_address_(entry.address) & self->dirty_wave_value_addr_hash_mask_;
            for (;;) {
                DirtyWaveValueAddressHashSlot& hs = self->dirty_wave_value_addr_hash_table_[slot];
                if (!hs.address) {
                    hs.address = entry.address;
                    hs.group_id = entry.group_id;
                    hs.byte_width = entry.byte_width;
                    break;
                }
                if (hs.address == entry.address) {
                    // Duplicate addresses should not normally occur for WaveValue
                    // groups.  Preserve the first mapping to match sorted-vector
                    // first-hit behavior as closely as practical.
                    break;
                }
                slot = (slot + 1u) & self->dirty_wave_value_addr_hash_mask_;
            }
        }
        self->dirty_wave_value_addr_hash_built_ = true;
    }

    std::uint32_t lookup_dirty_wave_value_group_by_address_hash_(const void* address) const noexcept {
        if (!dirty_wave_value_addr_hash_built_ || dirty_wave_value_addr_hash_table_.empty() || dirty_wave_value_addr_hash_mask_ == 0) {
            return kInvalidIndex;
        }
        std::size_t slot = hash_wave_value_address_(address) & dirty_wave_value_addr_hash_mask_;
        for (;;) {
            const DirtyWaveValueAddressHashSlot& hs = dirty_wave_value_addr_hash_table_[slot];
            if (!hs.address) return kInvalidIndex;
            if (hs.address == address) return hs.group_id;
            slot = (slot + 1u) & dirty_wave_value_addr_hash_mask_;
        }
    }

    std::uint32_t lookup_dirty_wave_value_group_by_address_binary_(const void* address) const noexcept {
        std::size_t lo = 0;
        std::size_t hi = dirty_wave_value_addr_table_.size();
        std::less<const void*> less;
        while (lo < hi) {
            const std::size_t mid = lo + ((hi - lo) >> 1);
            if (less(dirty_wave_value_addr_table_[mid].address, address)) lo = mid + 1;
            else hi = mid;
        }
        if (lo < dirty_wave_value_addr_table_.size() && dirty_wave_value_addr_table_[lo].address == address) {
            return dirty_wave_value_addr_table_[lo].group_id;
        }
        return kInvalidIndex;
    }

    std::uint32_t lookup_dirty_wave_value_group_by_address(const void* address) const noexcept {
        if (!address || dirty_wave_value_addr_table_.empty()) return kInvalidIndex;
        const std::uint32_t hash_group_id = lookup_dirty_wave_value_group_by_address_hash_(address);
        if (hash_group_id != kInvalidIndex) return hash_group_id;
        // Fallback for tiny topologies below the hash threshold, and for defensive
        // early dirty calls before prepare_topology() has built the hash table.
        ensure_dirty_wave_value_addr_table_sorted();
        return lookup_dirty_wave_value_group_by_address_binary_(address);
    }

    void queue_dirty_wave_value_group_once(std::uint32_t group_id) noexcept {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_value_groups_.size()) return;
        DirtyWaveValueGroup& group = dirty_wave_value_groups_[group_id];
        if (group.queued_epoch == dirty_epoch_) return;
        group.queued_epoch = dirty_epoch_;
        if (global_dirty_wave_value_group_ids_.size() < dirty_wave_value_groups_.size()) {
            try { global_dirty_wave_value_group_ids_.resize(dirty_wave_value_groups_.size()); }
            catch (...) { return; }
        }
        if (global_dirty_wave_value_group_count_ < global_dirty_wave_value_group_ids_.size()) {
            global_dirty_wave_value_group_ids_[global_dirty_wave_value_group_count_++] = group_id;
        }
    }

    static bool address_range_overlaps_(const void* a, std::size_t asz, const void* b, std::size_t bsz) noexcept {
        if (!a || !b || asz == 0 || bsz == 0) return false;
        const std::uintptr_t ab = reinterpret_cast<std::uintptr_t>(a);
        const std::uintptr_t bb = reinterpret_cast<std::uintptr_t>(b);
        const std::uintptr_t ae = ab + asz;
        const std::uintptr_t be = bb + bsz;
        return ab < be && bb < ae;
    }

    void drain_retired_dirty_wave_value_groups_() {
        std::lock_guard<std::mutex> lock(retired_dirty_mu_);
        for (std::size_t i = 0; i < retired_dirty_wave_value_group_ids_.size(); ++i) {
            queue_dirty_wave_value_group_once(retired_dirty_wave_value_group_ids_[i]);
        }
        retired_dirty_wave_value_group_ids_.clear();
    }

    void collect_dirty_wave_value_groups_from_tls() {
        global_dirty_wave_value_group_count_ = 0;
        if (global_dirty_wave_value_group_ids_.size() < dirty_wave_value_groups_.size()) {
            global_dirty_wave_value_group_ids_.resize(dirty_wave_value_groups_.size());
        }
        ensure_dirty_wave_value_addr_table_sorted();

        WaveTlsRegistry::instance().for_each_tls_locked([&](ThreadTraceLocal* tls) {
            if (!tls || tls->owner != this) return;

            const std::uint32_t count = tls->wave_value_dirty_count;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t group_id = tls->wave_value_dirty_ids[i];
                queue_dirty_wave_value_group_once(group_id);
            }
            tls->wave_value_dirty_count = 0;
        });
        drain_retired_dirty_wave_value_groups_();
    }

    void append_initial_dirty_wave_value_groups() {
        if (dirty_wave_value_initial_sample_done_) return;
        if (global_dirty_wave_value_group_ids_.size() < dirty_wave_value_groups_.size()) {
            global_dirty_wave_value_group_ids_.resize(dirty_wave_value_groups_.size());
        }
        for (std::uint32_t group_id = 0; group_id < dirty_wave_value_groups_.size(); ++group_id) {
            queue_dirty_wave_value_group_once(group_id);
        }
        dirty_wave_value_initial_sample_done_ = true;
    }

    static bool dirty_wave_array_address_entry_less_(const DirtyWaveArrayAddressEntry& a,
                                                     const DirtyWaveArrayAddressEntry& b) noexcept {
        std::less<const void*> less_ptr;
        if (a.address != b.address) return less_ptr(a.address, b.address);
        if (a.type_tag != b.type_tag) return less_ptr(a.type_tag, b.type_tag);
        return a.byte_width < b.byte_width;
    }

    void ensure_dirty_wave_array_addr_table_sorted() const {
        Tracer* self = const_cast<Tracer*>(this);
        if (self->dirty_wave_array_addr_table_sorted_) return;
        std::sort(self->dirty_wave_array_addr_table_.begin(), self->dirty_wave_array_addr_table_.end(), dirty_wave_array_address_entry_less_);
        self->dirty_wave_array_addr_table_sorted_ = true;
    }

    std::uint32_t lookup_dirty_wave_array_group_by_address(const void* address, const void* type_tag, std::uint32_t byte_width) const noexcept {
        if (!address || !type_tag || dirty_wave_array_addr_table_.empty()) return kInvalidIndex;
        ensure_dirty_wave_array_addr_table_sorted();
        DirtyWaveArrayAddressEntry key(address, type_tag, byte_width, kInvalidIndex);
        std::size_t lo = 0;
        std::size_t hi = dirty_wave_array_addr_table_.size();
        while (lo < hi) {
            const std::size_t mid = lo + ((hi - lo) >> 1);
            if (dirty_wave_array_address_entry_less_(dirty_wave_array_addr_table_[mid], key)) lo = mid + 1;
            else hi = mid;
        }
        if (lo < dirty_wave_array_addr_table_.size()) {
            const DirtyWaveArrayAddressEntry& e = dirty_wave_array_addr_table_[lo];
            if (e.address == address && e.type_tag == type_tag && e.byte_width == byte_width) return e.group_id;
        }
        return kInvalidIndex;
    }

    void queue_dirty_wave_array_group_once(std::uint32_t group_id) noexcept {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) return;
        DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
        if (group.queued_epoch == dirty_epoch_) return;
        group.queued_epoch = dirty_epoch_;
        if (global_dirty_wave_array_group_ids_.size() < dirty_wave_array_groups_.size()) {
            try { global_dirty_wave_array_group_ids_.resize(dirty_wave_array_groups_.size()); }
            catch (...) { return; }
        }
        if (global_dirty_wave_array_group_count_ < global_dirty_wave_array_group_ids_.size()) {
            global_dirty_wave_array_group_ids_[global_dirty_wave_array_group_count_++] = group_id;
        }
    }

    void drain_retired_dirty_wave_array_groups_() {
        std::lock_guard<std::mutex> lock(retired_dirty_mu_);
        for (std::size_t i = 0; i < retired_dirty_wave_array_group_ids_.size(); ++i) {
            queue_dirty_wave_array_group_once(retired_dirty_wave_array_group_ids_[i]);
        }
        retired_dirty_wave_array_group_ids_.clear();
    }

    void collect_dirty_wave_array_groups_from_tls() {
        global_dirty_wave_array_group_count_ = 0;
        if (global_dirty_wave_array_group_ids_.size() < dirty_wave_array_groups_.size()) {
            global_dirty_wave_array_group_ids_.resize(dirty_wave_array_groups_.size());
        }
        ensure_dirty_wave_array_addr_table_sorted();

        WaveTlsRegistry::instance().for_each_tls_locked([&](ThreadTraceLocal* tls) {
            if (!tls || tls->owner != this) return;
            const std::uint32_t count = tls->wave_array_dirty_count;
            for (std::uint32_t i = 0; i < count; ++i) {
                queue_dirty_wave_array_group_once(tls->wave_array_dirty_ids[i]);
            }
            tls->wave_array_dirty_count = 0;
        });
        drain_retired_dirty_wave_array_groups_();
    }

    void append_initial_dirty_wave_array_groups() {
        if (dirty_wave_array_initial_sample_done_) return;
        if (global_dirty_wave_array_group_ids_.size() < dirty_wave_array_groups_.size()) {
            global_dirty_wave_array_group_ids_.resize(dirty_wave_array_groups_.size());
        }
        for (std::uint32_t group_id = 0; group_id < dirty_wave_array_groups_.size(); ++group_id) {
            queue_dirty_wave_array_group_once(group_id);
        }
        dirty_wave_array_initial_sample_done_ = true;
    }

    bool dirty_peek_leaf_memory_candidate_(const DirtyPeekLeaf& leaf,
                                           const unsigned char*& begin,
                                           std::size_t& byte_count) const noexcept {
        begin = NULL;
        byte_count = 0;
        const void* memory = leaf.memory_ctx ? leaf.memory_ctx : leaf.sample_ctx;
        if (memory == NULL) return false;
        byte_count = leaf.memory_byte_width != 0
            ? static_cast<std::size_t>(leaf.memory_byte_width)
            : scalar_sample_kind_byte_width(leaf.scalar_kind);
        if (byte_count == 0) return false;
        if (leaf.scalar_reader != NULL && leaf.memory_ctx == NULL) return false;
        begin = static_cast<const unsigned char*>(memory);
        return true;
    }

    bool build_dirty_peek_memory_block_byte_map_(DirtyPeekMemoryBlock& block) {
        block.has_byte_map = false;
        block.byte_map_begin = 0;
        block.byte_map_count = 0;
        if (!options_.enable_dirty_peek_memory_block_byte_map) return false;
        if (block.byte_count == 0) return false;
        const std::size_t byte_map_max_bytes = options_.dirty_peek_memory_block_byte_map_max_bytes == 0
            ? static_cast<std::size_t>(4096)
            : options_.dirty_peek_memory_block_byte_map_max_bytes;
        if (block.byte_count > byte_map_max_bytes) return false;
        const std::size_t overhead_per_leaf = options_.dirty_peek_memory_block_byte_map_max_overhead_per_leaf == 0
            ? static_cast<std::size_t>(64)
            : options_.dirty_peek_memory_block_byte_map_max_overhead_per_leaf;
        const std::size_t leaf_count = static_cast<std::size_t>(block.leaf_count);
        if (leaf_count == 0) return false;
        if (overhead_per_leaf != 0 && block.byte_count > leaf_count * overhead_per_leaf) return false;

        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        if (begin_ref >= dirty_peek_memory_leaf_refs_.size()) return false;
        const std::uint32_t safe_end_ref =
            end_ref < dirty_peek_memory_leaf_refs_.size()
                ? end_ref
                : static_cast<std::uint32_t>(dirty_peek_memory_leaf_refs_.size());
        if (begin_ref >= safe_end_ref) return false;

        std::vector<std::uint32_t> local_map;
        try {
            local_map.assign(block.byte_count, kInvalidIndex);
        } catch (...) {
            return false;
        }

        for (std::uint32_t ref_i = begin_ref; ref_i < safe_end_ref; ++ref_i) {
            const DirtyPeekMemoryLeafRef& ref = dirty_peek_memory_leaf_refs_[ref_i];
            const std::size_t off = static_cast<std::size_t>(ref.offset);
            const std::size_t count = static_cast<std::size_t>(ref.byte_count);
            if (count == 0 || off >= block.byte_count || count > block.byte_count - off) return false;
            for (std::size_t b = 0; b < count; ++b) {
                std::uint32_t& slot = local_map[off + b];
                if (slot != kInvalidIndex) {
                    // Overlapping leaves can happen with unions/aliases.  A one-to-one
                    // byte map would be incorrect, so keep the safe range-lookup path.
                    return false;
                }
                slot = ref_i;
            }
        }

        const std::size_t map_begin = dirty_peek_memory_byte_to_leaf_refs_.size();
        try {
            dirty_peek_memory_byte_to_leaf_refs_.insert(
                dirty_peek_memory_byte_to_leaf_refs_.end(), local_map.begin(), local_map.end());
        } catch (...) {
            return false;
        }
        if (map_begin > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            local_map.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            dirty_peek_memory_byte_to_leaf_refs_.resize(map_begin);
            return false;
        }
        block.byte_map_begin = static_cast<std::uint32_t>(map_begin);
        block.byte_map_count = static_cast<std::uint32_t>(local_map.size());
        block.has_byte_map = true;
        return true;
    }

    bool ensure_dirty_peek_memory_blocks() {
        if (!options_.enable_dirty_peek_memory_block_precheck) return false;
        if (dirty_peek_memory_blocks_valid_) return dirty_peek_memory_blocks_complete_;

        dirty_peek_memory_blocks_.clear();
        dirty_peek_memory_leaf_refs_.clear();
        dirty_peek_memory_byte_to_leaf_refs_.clear();
        dirty_peek_memory_scalar_leaf_indices_.clear();
        dirty_peek_memory_shadow_bytes_.clear();

        for (std::size_t group_index = 0; group_index < dirty_peek_groups_.size(); ++group_index) {
            DirtyPeekGroup& g = dirty_peek_groups_[group_index];
            g.memory_block_begin = static_cast<std::uint32_t>(dirty_peek_memory_blocks_.size());
            g.memory_block_count = 0;
            g.memory_scalar_begin = static_cast<std::uint32_t>(dirty_peek_memory_scalar_leaf_indices_.size());
            g.memory_scalar_count = 0;
        }

        struct CandidateRef {
            std::uint32_t leaf_index;
            const unsigned char* begin;
            const unsigned char* end;
            std::uintptr_t begin_addr;
            std::uintptr_t end_addr;
            std::size_t byte_count;
        };

        const std::size_t min_leaf_count = options_.dirty_peek_memory_block_min_leaf_count == 0
            ? static_cast<std::size_t>(1)
            : options_.dirty_peek_memory_block_min_leaf_count;
        const std::size_t max_gap = options_.dirty_peek_memory_block_max_gap;
        const std::size_t max_bytes = options_.dirty_peek_memory_block_max_bytes == 0
            ? static_cast<std::size_t>(4096)
            : options_.dirty_peek_memory_block_max_bytes;

        std::vector<CandidateRef> candidates;
        std::vector<CandidateRef> current_refs;
        candidates.reserve(128);
        current_refs.reserve(64);

        for (std::uint32_t group_id = 0; group_id < dirty_peek_groups_.size(); ++group_id) {
            DirtyPeekGroup& group = dirty_peek_groups_[group_id];
            group.memory_block_begin = static_cast<std::uint32_t>(dirty_peek_memory_blocks_.size());
            group.memory_scalar_begin = static_cast<std::uint32_t>(dirty_peek_memory_scalar_leaf_indices_.size());

            const auto flush_current = [&](const unsigned char*& current_base,
                                           const unsigned char*& current_end,
                                           std::uintptr_t& current_base_addr,
                                           std::uintptr_t& current_end_addr) {
                if (current_refs.empty()) return;
                const std::size_t block_bytes = static_cast<std::size_t>(current_end_addr - current_base_addr);
                if (current_refs.size() >= min_leaf_count && block_bytes != 0 && block_bytes <= max_bytes) {
                    DirtyPeekMemoryBlock block;
                    block.base = current_base;
                    block.byte_count = block_bytes;
                    block.shadow_offset = dirty_peek_memory_shadow_bytes_.size();
                    block.first_leaf_ref = static_cast<std::uint32_t>(dirty_peek_memory_leaf_refs_.size());
                    block.leaf_count = static_cast<std::uint32_t>(current_refs.size());
                    block.needs_initial_scan = true;
                    if (block_bytes > static_cast<std::size_t>(std::numeric_limits<std::size_t>::max()) - dirty_peek_memory_shadow_bytes_.size()) {
                        for (std::size_t j = 0; j < current_refs.size(); ++j) {
                            dirty_peek_memory_scalar_leaf_indices_.push_back(current_refs[j].leaf_index);
                        }
                        current_refs.clear();
                        current_base = NULL;
                        current_end = NULL;
                        current_base_addr = 0;
                        current_end_addr = 0;
                        return;
                    }
                    try {
                        dirty_peek_memory_shadow_bytes_.resize(dirty_peek_memory_shadow_bytes_.size() + block_bytes);
                    } catch (...) {
                        for (std::size_t j = 0; j < current_refs.size(); ++j) {
                            dirty_peek_memory_scalar_leaf_indices_.push_back(current_refs[j].leaf_index);
                        }
                        current_refs.clear();
                        current_base = NULL;
                        current_end = NULL;
                        current_base_addr = 0;
                        current_end_addr = 0;
                        return;
                    }
                    for (std::size_t j = 0; j < current_refs.size(); ++j) {
                        const CandidateRef& ref = current_refs[j];
                        const std::size_t off = static_cast<std::size_t>(ref.begin_addr - current_base_addr);
                        dirty_peek_memory_leaf_refs_.push_back(DirtyPeekMemoryLeafRef(ref.leaf_index,
                            static_cast<std::uint32_t>(off),
                            static_cast<std::uint32_t>(ref.byte_count)));
                    }
                    build_dirty_peek_memory_block_byte_map_(block);
                    dirty_peek_memory_blocks_.push_back(block);
                } else {
                    for (std::size_t j = 0; j < current_refs.size(); ++j) {
                        dirty_peek_memory_scalar_leaf_indices_.push_back(current_refs[j].leaf_index);
                    }
                }
                current_refs.clear();
                current_base = NULL;
                current_end = NULL;
                current_base_addr = 0;
                current_end_addr = 0;
            };

            candidates.clear();
            current_refs.clear();

            // Merge memory-block candidates across all ranges of the same dirty group.
            // Ranges are still respected for fallback leaf collection, but block formation
            // is based on the actual address layout of all candidate leaves in the group.
            for (std::uint32_t range_id = group.first_range;
                 range_id != kInvalidIndex && range_id < dirty_peek_ranges_.size();
                 range_id = dirty_peek_ranges_[range_id].next_sibling) {
                const DirtyPeekRange& range = dirty_peek_ranges_[range_id];
                const std::uint32_t begin_leaf = range.leaf_begin;
                const std::uint32_t end_leaf = begin_leaf + range.leaf_count;
                for (std::uint32_t leaf_id = begin_leaf; leaf_id < end_leaf && leaf_id < dirty_peek_leaves_.size(); ++leaf_id) {
                    const DirtyPeekLeaf& leaf = dirty_peek_leaves_[leaf_id];
                    const unsigned char* begin = NULL;
                    std::size_t byte_count = 0;
                    if (!dirty_peek_leaf_memory_candidate_(leaf, begin, byte_count)) {
                        dirty_peek_memory_scalar_leaf_indices_.push_back(leaf_id);
                        continue;
                    }
                    CandidateRef ref;
                    ref.leaf_index = leaf_id;
                    ref.begin = begin;
                    ref.end = begin + byte_count;
                    ref.begin_addr = reinterpret_cast<std::uintptr_t>(begin);
                    ref.end_addr = ref.begin_addr + byte_count;
                    ref.byte_count = byte_count;
                    candidates.push_back(ref);
                }
            }

            std::sort(candidates.begin(), candidates.end(), [](const CandidateRef& a, const CandidateRef& b) {
                if (a.begin_addr != b.begin_addr) return a.begin_addr < b.begin_addr;
                if (a.byte_count != b.byte_count) return a.byte_count < b.byte_count;
                return a.leaf_index < b.leaf_index;
            });

            const unsigned char* current_base = NULL;
            const unsigned char* current_end = NULL;
            std::uintptr_t current_base_addr = 0;
            std::uintptr_t current_end_addr = 0;

            for (std::size_t i = 0; i < candidates.size(); ++i) {
                const CandidateRef& ref = candidates[i];
                if (current_refs.empty()) {
                    current_base = ref.begin;
                    current_end = ref.end;
                    current_base_addr = ref.begin_addr;
                    current_end_addr = ref.end_addr;
                    current_refs.push_back(ref);
                    continue;
                }

                bool can_merge = false;
                if (ref.begin_addr >= current_end_addr) {
                    const std::size_t gap = static_cast<std::size_t>(ref.begin_addr - current_end_addr);
                    const std::size_t new_bytes = static_cast<std::size_t>(ref.end_addr - current_base_addr);
                    can_merge = (gap <= max_gap && new_bytes <= max_bytes);
                }
                if (!can_merge) {
                    flush_current(current_base, current_end, current_base_addr, current_end_addr);
                    current_base = ref.begin;
                    current_end = ref.end;
                    current_base_addr = ref.begin_addr;
                    current_end_addr = ref.end_addr;
                    current_refs.push_back(ref);
                    continue;
                }
                if (ref.end_addr > current_end_addr) {
                    current_end = ref.end;
                    current_end_addr = ref.end_addr;
                }
                current_refs.push_back(ref);
            }
            flush_current(current_base, current_end, current_base_addr, current_end_addr);

            group.memory_block_count = static_cast<std::uint32_t>(dirty_peek_memory_blocks_.size()) - group.memory_block_begin;
            group.memory_scalar_count = static_cast<std::uint32_t>(dirty_peek_memory_scalar_leaf_indices_.size()) - group.memory_scalar_begin;
        }

        dirty_peek_memory_blocks_complete_ = true;
        dirty_peek_memory_blocks_valid_ = true;
        return dirty_peek_memory_blocks_complete_;
    }

    bool read_dirty_peek_leaf_snapshot(const DirtyPeekLeaf& leaf, ScalarSnapshot& sample) const {
        if (leaf.scalar_reader) return leaf.scalar_reader(leaf.sample_ctx, sample);
        FlatLeafFast tmp;
        tmp.track_id = leaf.track_id;
        tmp.storage_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        tmp.sample_ctx = leaf.sample_ctx;
        tmp.memory_ctx = leaf.memory_ctx;
        tmp.memory_byte_width = leaf.memory_byte_width;
        tmp.scalar_reader = leaf.scalar_reader;
        tmp.scalar_kind = leaf.scalar_kind;
        tmp.value_kind = leaf.value_kind;
        return read_flat_leaf_snapshot(tmp, sample);
    }

    void fill_dirty_peek_leaf_event(const DirtyPeekLeaf& leaf, const ScalarSnapshot& sample, Cycle cycle, TrackEvent& ev) const {
        FlatLeafFast tmp;
        tmp.track_id = leaf.track_id;
        tmp.storage_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        tmp.sample_ctx = leaf.sample_ctx;
        tmp.memory_ctx = leaf.memory_ctx;
        tmp.memory_byte_width = leaf.memory_byte_width;
        tmp.scalar_reader = leaf.scalar_reader;
        tmp.scalar_kind = leaf.scalar_kind;
        tmp.value_kind = leaf.value_kind;
        tmp.has_last = leaf.has_last;
        tmp.last_bits = leaf.last_bits;
        fill_flat_leaf_event(tmp, sample, cycle, ev);
    }

    void begin_dirty_peek_storage_sample_epoch_() {
        ++dirty_peek_storage_sample_epoch_;
        if (dirty_peek_storage_sample_epoch_ == 0) {
            dirty_peek_storage_sample_epoch_ = 1;
            for (std::size_t i = 0; i < track_runtime_.size(); ++i) {
                track_runtime_[i].dirty_peek_storage_sample_epoch = 0;
            }
        }
    }

    bool sample_one_dirty_peek_leaf_changed(DirtyPeekLeaf& leaf, ScalarSnapshot& sample) {
        if (leaf.track_id == 0 || leaf.track_id >= track_runtime_.size()) return false;
        TrackRuntimeState& logical_runtime = track_runtime_[static_cast<std::size_t>(leaf.track_id)];
        if (!logical_runtime.alive) return false;

        const TrackId state_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        if (state_id == 0 || state_id >= track_runtime_.size()) return false;
        TrackRuntimeState& storage_runtime = track_runtime_[static_cast<std::size_t>(state_id)];
        if (storage_runtime.dirty_peek_storage_sample_epoch == dirty_peek_storage_sample_epoch_) {
            return false;
        }

        if (!read_dirty_peek_leaf_snapshot(leaf, sample)) return false;
        storage_runtime.dirty_peek_storage_sample_epoch = dirty_peek_storage_sample_epoch_;

        if (options_.emit_only_on_change && storage_runtime.has_last_event &&
            storage_runtime.last_invalid == InvalidKind::None && storage_runtime.last_has_change_bits &&
            storage_runtime.last_change_bits == sample.bits) {
            leaf.has_last = true;
            leaf.last_bits = sample.bits;
            return false;
        }
        if (options_.emit_only_on_change && leaf.has_last && leaf.last_bits == sample.bits) return false;
        leaf.has_last = true;
        leaf.last_bits = sample.bits;
        storage_runtime.has_last_event = true;
        storage_runtime.last_invalid = InvalidKind::None;
        storage_runtime.last_has_change_bits = true;
        storage_runtime.last_change_bits = sample.bits;
        return true;
    }

    void sample_dirty_peek_memory_leaf_ref_precise_(Cycle cycle,
                                                   DirtyPeekMemoryLeafRef& ref,
                                                   std::vector<TrackEvent>& out) {
        if (ref.dirty_leaf_index >= dirty_peek_leaves_.size()) return;
        if (ref.last_sample_epoch == dirty_peek_memory_sample_epoch_) return;
        ref.last_sample_epoch = dirty_peek_memory_sample_epoch_;
        DirtyPeekLeaf& leaf = dirty_peek_leaves_[ref.dirty_leaf_index];
        if (leaf.memory_last_sample_epoch == dirty_peek_memory_sample_epoch_) return;
        leaf.memory_last_sample_epoch = dirty_peek_memory_sample_epoch_;
        ScalarSnapshot sample;
        if (!sample_one_dirty_peek_leaf_changed(leaf, sample)) return;
        out.emplace_back();
        fill_dirty_peek_leaf_event(leaf, sample, cycle, out.back());
    }

    void sample_dirty_peek_memory_changed_byte_range_(Cycle cycle,
                                                      DirtyPeekMemoryBlock& block,
                                                      std::size_t changed_begin,
                                                      std::size_t changed_end,
                                                      std::vector<TrackEvent>& out) {
        if (changed_end <= changed_begin) return;
        if (changed_begin >= block.byte_count) return;
        if (changed_end > block.byte_count) changed_end = block.byte_count;

        if (block.has_byte_map && block.byte_map_count == block.byte_count) {
            const std::size_t map_begin = static_cast<std::size_t>(block.byte_map_begin);
            if (map_begin <= dirty_peek_memory_byte_to_leaf_refs_.size() &&
                block.byte_count <= dirty_peek_memory_byte_to_leaf_refs_.size() - map_begin) {
                for (std::size_t off = changed_begin; off < changed_end; ++off) {
                    const std::uint32_t ref_i = dirty_peek_memory_byte_to_leaf_refs_[map_begin + off];
                    if (ref_i == kInvalidIndex || ref_i >= dirty_peek_memory_leaf_refs_.size()) continue;
                    sample_dirty_peek_memory_leaf_ref_precise_(cycle, dirty_peek_memory_leaf_refs_[ref_i], out);
                }
                return;
            }
        }

        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        if (begin_ref >= dirty_peek_memory_leaf_refs_.size()) return;
        const std::uint32_t safe_end_ref =
            end_ref < dirty_peek_memory_leaf_refs_.size()
                ? end_ref
                : static_cast<std::uint32_t>(dirty_peek_memory_leaf_refs_.size());
        if (begin_ref >= safe_end_ref) return;

        std::uint32_t lo = begin_ref;
        std::uint32_t hi = safe_end_ref;
        while (lo < hi) {
            const std::uint32_t mid = lo + static_cast<std::uint32_t>((hi - lo) / 2u);
            const DirtyPeekMemoryLeafRef& ref = dirty_peek_memory_leaf_refs_[mid];
            const std::size_t leaf_end = static_cast<std::size_t>(ref.offset) +
                                         static_cast<std::size_t>(ref.byte_count);
            if (leaf_end <= changed_begin) lo = mid + 1u;
            else hi = mid;
        }

        for (std::uint32_t r = lo; r < safe_end_ref; ++r) {
            DirtyPeekMemoryLeafRef& ref = dirty_peek_memory_leaf_refs_[r];
            const std::size_t leaf_begin = static_cast<std::size_t>(ref.offset);
            if (leaf_begin >= changed_end) break;
            const std::size_t leaf_end = leaf_begin + static_cast<std::size_t>(ref.byte_count);
            if (leaf_begin < changed_end && changed_begin < leaf_end) {
                sample_dirty_peek_memory_leaf_ref_precise_(cycle, ref, out);
            }
        }
    }

    void sample_dirty_peek_memory_changed_mask32_(Cycle cycle,
                                                  DirtyPeekMemoryBlock& block,
                                                  std::size_t base_off,
                                                  std::uint32_t diff_mask,
                                                  std::vector<TrackEvent>& out) {
        std::uint32_t pos = 0;
        while (pos < 32) {
            while (pos < 32 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) == 0)) ++pos;
            if (pos >= 32) break;
            const std::uint32_t run_begin = pos;
            while (pos < 32 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) != 0)) ++pos;
            sample_dirty_peek_memory_changed_byte_range_(cycle, block,
                base_off + static_cast<std::size_t>(run_begin),
                base_off + static_cast<std::size_t>(pos), out);
        }
    }

    void sample_dirty_peek_memory_changed_mask16_(Cycle cycle,
                                                  DirtyPeekMemoryBlock& block,
                                                  std::size_t base_off,
                                                  std::uint32_t diff_mask,
                                                  std::vector<TrackEvent>& out) {
        std::uint32_t pos = 0;
        while (pos < 16) {
            while (pos < 16 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) == 0)) ++pos;
            if (pos >= 16) break;
            const std::uint32_t run_begin = pos;
            while (pos < 16 && ((diff_mask & (static_cast<std::uint32_t>(1) << pos)) != 0)) ++pos;
            sample_dirty_peek_memory_changed_byte_range_(cycle, block,
                base_off + static_cast<std::size_t>(run_begin),
                base_off + static_cast<std::size_t>(pos), out);
        }
    }

    void sample_dirty_peek_memory_block_scalar_mask_to_buffer_(Cycle cycle,
                                                               DirtyPeekMemoryBlock& block,
                                                               unsigned char* shadow,
                                                               std::vector<TrackEvent>& out,
                                                               std::size_t start_off = 0) {
        std::size_t off = start_off;
        while (off < block.byte_count) {
            if (block.base[off] == shadow[off]) {
                ++off;
                continue;
            }
            const std::size_t run_begin = off;
            do {
                shadow[off] = block.base[off];
                ++off;
            } while (off < block.byte_count && block.base[off] != shadow[off]);
            sample_dirty_peek_memory_changed_byte_range_(cycle, block, run_begin, off, out);
        }
    }

    void sample_dirty_peek_memory_block_simd_mask_to_buffer_(Cycle cycle,
                                                             DirtyPeekMemoryBlock& block,
                                                             unsigned char* shadow,
                                                             std::vector<TrackEvent>& out) {
        std::size_t off = 0;
        const FlatMemoryBlockSimdBackend backend =
            (options_.enable_dirty_peek_memory_block_simd_mask &&
             block.byte_count >= options_.dirty_peek_memory_block_simd_min_bytes)
                ? resolve_flat_memory_block_simd_backend_()
                : FlatMemoryBlockSimdBackend::Scalar;

#if defined(__AVX2__) || defined(_M_AVX2)
        if (backend == FlatMemoryBlockSimdBackend::AVX2) {
            for (; off + 32 <= block.byte_count; off += 32) {
                const __m256i cur = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block.base + off));
                const __m256i old = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shadow + off));
                const __m256i eq = _mm256_cmpeq_epi8(cur, old);
                const std::uint32_t eq_mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(eq));
                const std::uint32_t diff_mask = (~eq_mask) & static_cast<std::uint32_t>(0xFFFFFFFFu);
                if (diff_mask == 0) continue;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(shadow + off), cur);
                sample_dirty_peek_memory_changed_mask32_(cycle, block, off, diff_mask, out);
            }
        }
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        if (backend == FlatMemoryBlockSimdBackend::SSE2) {
            for (; off + 16 <= block.byte_count; off += 16) {
                const __m128i cur = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block.base + off));
                const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadow + off));
                const __m128i eq = _mm_cmpeq_epi8(cur, old);
                const std::uint32_t eq_mask = static_cast<std::uint32_t>(_mm_movemask_epi8(eq));
                const std::uint32_t diff_mask = (~eq_mask) & static_cast<std::uint32_t>(0xFFFFu);
                if (diff_mask == 0) continue;
                _mm_storeu_si128(reinterpret_cast<__m128i*>(shadow + off), cur);
                sample_dirty_peek_memory_changed_mask16_(cycle, block, off, diff_mask, out);
            }
        }
#endif

        sample_dirty_peek_memory_block_scalar_mask_to_buffer_(cycle, block, shadow, out, off);
    }

    void sample_dirty_peek_memory_block_initial_to_buffer_(Cycle cycle,
                                                           DirtyPeekMemoryBlock& block,
                                                           unsigned char* shadow,
                                                           std::vector<TrackEvent>& out) {
        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        for (std::uint32_t r = begin_ref; r < end_ref && r < dirty_peek_memory_leaf_refs_.size(); ++r) {
            sample_dirty_peek_memory_leaf_ref_precise_(cycle, dirty_peek_memory_leaf_refs_[r], out);
        }
        std::memcpy(shadow, block.base, block.byte_count);
        block.needs_initial_scan = false;
    }

    void sample_dirty_peek_memory_block_to_buffer_(Cycle cycle,
                                                   DirtyPeekMemoryBlock& block,
                                                   std::vector<TrackEvent>& out) {
        if (block.base == NULL || block.byte_count == 0) return;
        if (block.shadow_offset > dirty_peek_memory_shadow_bytes_.size() ||
            block.byte_count > dirty_peek_memory_shadow_bytes_.size() - block.shadow_offset) return;
        unsigned char* shadow = dirty_peek_memory_shadow_bytes_.empty()
            ? NULL
            : &dirty_peek_memory_shadow_bytes_[block.shadow_offset];
        if (!shadow) return;
        if (block.needs_initial_scan) {
            sample_dirty_peek_memory_block_initial_to_buffer_(cycle, block, shadow, out);
            return;
        }
        sample_dirty_peek_memory_block_simd_mask_to_buffer_(cycle, block, shadow, out);
    }

    void sample_dirty_peek_group_to_buffer(std::uint32_t group_id, Cycle cycle, std::vector<TrackEvent>& out) {
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return;
        DirtyPeekGroup& group = dirty_peek_groups_[group_id];

        if (dirty_peek_memory_blocks_valid_ && options_.enable_dirty_peek_memory_block_precheck) {
            const std::uint32_t block_begin = group.memory_block_begin;
            const std::uint32_t block_end = block_begin + group.memory_block_count;
            for (std::uint32_t b = block_begin; b < block_end && b < dirty_peek_memory_blocks_.size(); ++b) {
                sample_dirty_peek_memory_block_to_buffer_(cycle, dirty_peek_memory_blocks_[b], out);
            }

            const std::uint32_t scalar_begin = group.memory_scalar_begin;
            const std::uint32_t scalar_end = scalar_begin + group.memory_scalar_count;
            for (std::uint32_t s = scalar_begin; s < scalar_end && s < dirty_peek_memory_scalar_leaf_indices_.size(); ++s) {
                const std::uint32_t leaf_id = dirty_peek_memory_scalar_leaf_indices_[s];
                if (leaf_id >= dirty_peek_leaves_.size()) continue;
                ScalarSnapshot sample;
                DirtyPeekLeaf& leaf = dirty_peek_leaves_[leaf_id];
                if (!sample_one_dirty_peek_leaf_changed(leaf, sample)) continue;
                out.emplace_back();
                fill_dirty_peek_leaf_event(leaf, sample, cycle, out.back());
            }
            return;
        }

        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex && range_id < dirty_peek_ranges_.size();
             range_id = dirty_peek_ranges_[range_id].next_sibling) {
            DirtyPeekRange& range = dirty_peek_ranges_[range_id];
            const std::uint32_t begin = range.leaf_begin;
            const std::uint32_t end = begin + range.leaf_count;
            for (std::uint32_t i = begin; i < end && i < dirty_peek_leaves_.size(); ++i) {
                ScalarSnapshot sample;
                DirtyPeekLeaf& leaf = dirty_peek_leaves_[i];
                if (!sample_one_dirty_peek_leaf_changed(leaf, sample)) continue;
                out.emplace_back();
                fill_dirty_peek_leaf_event(leaf, sample, cycle, out.back());
            }
        }
    }

    bool read_dirty_wave_value_leaf_snapshot(const DirtyWaveValueLeaf& leaf, ScalarSnapshot& sample) const {
        if (leaf.scalar_reader) return leaf.scalar_reader(leaf.sample_ctx, sample);
        FlatLeafFast tmp;
        tmp.track_id = leaf.track_id;
        tmp.storage_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        tmp.sample_ctx = leaf.sample_ctx;
        tmp.memory_ctx = leaf.memory_ctx;
        tmp.memory_byte_width = leaf.memory_byte_width;
        tmp.scalar_reader = leaf.scalar_reader;
        tmp.scalar_kind = leaf.scalar_kind;
        tmp.value_kind = leaf.value_kind;
        return read_flat_leaf_snapshot(tmp, sample);
    }

    void fill_dirty_wave_value_leaf_event(const DirtyWaveValueLeaf& leaf, const ScalarSnapshot& sample, Cycle cycle, TrackEvent& ev) const {
        FlatLeafFast tmp;
        tmp.track_id = leaf.track_id;
        tmp.storage_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        tmp.sample_ctx = leaf.sample_ctx;
        tmp.memory_ctx = leaf.memory_ctx;
        tmp.memory_byte_width = leaf.memory_byte_width;
        tmp.scalar_reader = leaf.scalar_reader;
        tmp.scalar_kind = leaf.scalar_kind;
        tmp.value_kind = leaf.value_kind;
        tmp.has_last = leaf.has_last;
        tmp.last_bits = leaf.last_bits;
        fill_flat_leaf_event(tmp, sample, cycle, ev);
    }

    bool sample_one_dirty_wave_value_leaf_changed(DirtyWaveValueLeaf& leaf, ScalarSnapshot& sample) {
        const TrackId state_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        if (state_id == 0 || state_id >= track_runtime_.size()) return false;
        TrackRuntimeState& runtime = track_runtime_[static_cast<std::size_t>(state_id)];
        if (!runtime.alive) return false;
        if (!read_dirty_wave_value_leaf_snapshot(leaf, sample)) return false;
        if (options_.emit_only_on_change && runtime.has_last_event &&
            runtime.last_invalid == InvalidKind::None && runtime.last_has_change_bits &&
            runtime.last_change_bits == sample.bits) {
            leaf.has_last = true;
            leaf.last_bits = sample.bits;
            return false;
        }
        if (options_.emit_only_on_change && leaf.has_last && leaf.last_bits == sample.bits) return false;
        leaf.has_last = true;
        leaf.last_bits = sample.bits;
        runtime.has_last_event = true;
        runtime.last_invalid = InvalidKind::None;
        runtime.last_has_change_bits = true;
        runtime.last_change_bits = sample.bits;
        return true;
    }

    void sample_dirty_wave_value_group_to_buffer(std::uint32_t group_id, Cycle cycle, std::vector<TrackEvent>& out) {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_value_groups_.size()) return;
        DirtyWaveValueGroup& group = dirty_wave_value_groups_[group_id];
        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex && range_id < dirty_wave_value_ranges_.size();
             range_id = dirty_wave_value_ranges_[range_id].next_sibling) {
            DirtyWaveValueRange& range = dirty_wave_value_ranges_[range_id];
            const std::uint32_t begin = range.leaf_begin;
            const std::uint32_t end = begin + range.leaf_count;
            for (std::uint32_t i = begin; i < end && i < dirty_wave_value_leaves_.size(); ++i) {
                ScalarSnapshot sample;
                DirtyWaveValueLeaf& leaf = dirty_wave_value_leaves_[i];
                if (!sample_one_dirty_wave_value_leaf_changed(leaf, sample)) continue;
                out.emplace_back();
                fill_dirty_wave_value_leaf_event(leaf, sample, cycle, out.back());
            }
        }
    }

    void sample_dirty_wave_value_groups(Cycle cycle) {
        append_initial_dirty_wave_value_groups();
        if (global_dirty_wave_value_group_count_ == 0) return;
        const std::size_t expected = dirty_wave_value_leaves_.size();
        if (caller_parallel_events_.capacity() < expected) caller_parallel_events_.reserve(expected);
        caller_parallel_events_.clear();
        for (std::uint32_t i = 0; i < global_dirty_wave_value_group_count_; ++i) {
            sample_dirty_wave_value_group_to_buffer(global_dirty_wave_value_group_ids_[i], cycle, caller_parallel_events_);
        }
        for (std::size_t i = 0; i < caller_parallel_events_.size(); ++i) {
            sink_.on_sample(caller_parallel_events_[i]);
        }
        global_dirty_wave_value_group_count_ = 0;
    }

    bool read_dirty_wave_array_leaf_snapshot(const DirtyWaveArrayLeaf& leaf, ScalarSnapshot& sample) const {
        if (leaf.scalar_reader) return leaf.scalar_reader(leaf.sample_ctx, sample);
        FlatLeafFast tmp;
        tmp.track_id = leaf.track_id;
        tmp.storage_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        tmp.sample_ctx = leaf.sample_ctx;
        tmp.memory_ctx = leaf.memory_ctx;
        tmp.memory_byte_width = leaf.memory_byte_width;
        tmp.scalar_reader = leaf.scalar_reader;
        tmp.scalar_kind = leaf.scalar_kind;
        tmp.value_kind = leaf.value_kind;
        return read_flat_leaf_snapshot(tmp, sample);
    }

    void fill_dirty_wave_array_leaf_event(const DirtyWaveArrayLeaf& leaf, const ScalarSnapshot& sample, Cycle cycle, TrackEvent& ev) const {
        FlatLeafFast tmp;
        tmp.track_id = leaf.track_id;
        tmp.storage_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        tmp.sample_ctx = leaf.sample_ctx;
        tmp.memory_ctx = leaf.memory_ctx;
        tmp.memory_byte_width = leaf.memory_byte_width;
        tmp.scalar_reader = leaf.scalar_reader;
        tmp.scalar_kind = leaf.scalar_kind;
        tmp.value_kind = leaf.value_kind;
        tmp.has_last = leaf.has_last;
        tmp.last_bits = leaf.last_bits;
        fill_flat_leaf_event(tmp, sample, cycle, ev);
    }

    bool sample_one_dirty_wave_array_leaf_changed(DirtyWaveArrayLeaf& leaf, ScalarSnapshot& sample) {
        const TrackId state_id = leaf.storage_id != 0 ? leaf.storage_id : leaf.track_id;
        if (state_id == 0 || state_id >= track_runtime_.size()) return false;
        TrackRuntimeState& runtime = track_runtime_[static_cast<std::size_t>(state_id)];
        if (!runtime.alive) return false;
        if (!read_dirty_wave_array_leaf_snapshot(leaf, sample)) return false;

        // Cross-path de-duplication: the same track may also be reported by
        // WaveValue or another dirty mechanism in this cycle.  Runtime state is
        // the authoritative per-track last value; if it is already current, do
        // not emit a duplicate event.
        if (options_.emit_only_on_change && runtime.has_last_event &&
            runtime.last_invalid == InvalidKind::None && runtime.last_has_change_bits &&
            runtime.last_change_bits == sample.bits) {
            leaf.has_last = true;
            leaf.last_bits = sample.bits;
            return false;
        }

        if (options_.emit_only_on_change && leaf.has_last && leaf.last_bits == sample.bits) return false;
        leaf.has_last = true;
        leaf.last_bits = sample.bits;
        runtime.has_last_event = true;
        runtime.last_invalid = InvalidKind::None;
        runtime.last_has_change_bits = true;
        runtime.last_change_bits = sample.bits;
        return true;
    }

    bool dirty_wave_array_leaf_memory_candidate_(const DirtyWaveArrayLeaf& leaf,
                                                 const unsigned char*& begin,
                                                 std::size_t& byte_count) const noexcept {
        begin = NULL;
        byte_count = 0;
        if (leaf.sample_ctx == NULL) return false;
        // Volatile/custom getter leaves must keep their typed reader path; raw
        // byte block compare would bypass the intended access semantics.
        if (leaf.scalar_reader != NULL) return false;
        byte_count = scalar_sample_kind_byte_width(leaf.scalar_kind);
        if (byte_count == 0) return false;
        begin = static_cast<const unsigned char*>(leaf.sample_ctx);
        return true;
    }

    bool build_dirty_wave_array_memory_block_byte_map_(DirtyWaveArrayMemoryBlock& block) {
        block.has_byte_map = false;
        block.byte_map_begin = 0;
        block.byte_map_count = 0;
        if (!options_.enable_wave_array_memory_block_byte_map) return false;
        if (block.byte_count == 0) return false;
        const std::size_t byte_map_max_bytes = options_.wave_array_memory_block_byte_map_max_bytes == 0
            ? static_cast<std::size_t>(4096)
            : options_.wave_array_memory_block_byte_map_max_bytes;
        if (block.byte_count > byte_map_max_bytes) return false;
        const std::size_t overhead_per_leaf = options_.wave_array_memory_block_byte_map_max_overhead_per_leaf == 0
            ? static_cast<std::size_t>(64)
            : options_.wave_array_memory_block_byte_map_max_overhead_per_leaf;
        const std::size_t leaf_count = static_cast<std::size_t>(block.leaf_count);
        if (leaf_count == 0) return false;
        if (overhead_per_leaf != 0 && block.byte_count > leaf_count * overhead_per_leaf) return false;

        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        if (begin_ref >= dirty_wave_array_memory_leaf_refs_.size()) return false;
        const std::uint32_t safe_end_ref =
            end_ref < dirty_wave_array_memory_leaf_refs_.size()
                ? end_ref
                : static_cast<std::uint32_t>(dirty_wave_array_memory_leaf_refs_.size());
        if (begin_ref >= safe_end_ref) return false;

        std::vector<std::uint32_t> local_map;
        try {
            local_map.assign(block.byte_count, kInvalidIndex);
        } catch (...) {
            return false;
        }

        for (std::uint32_t ref_i = begin_ref; ref_i < safe_end_ref; ++ref_i) {
            const DirtyWaveArrayMemoryLeafRef& ref = dirty_wave_array_memory_leaf_refs_[ref_i];
            const std::size_t off = static_cast<std::size_t>(ref.offset);
            const std::size_t count = static_cast<std::size_t>(ref.byte_count);
            if (count == 0 || off >= block.byte_count || count > block.byte_count - off) return false;
            for (std::size_t b = 0; b < count; ++b) {
                std::uint32_t& slot = local_map[off + b];
                if (slot != kInvalidIndex) {
                    // Overlapping leaves can happen with unions/aliases.  A one-to-one
                    // byte map would be incorrect, so keep the safe range-lookup path.
                    return false;
                }
                slot = ref_i;
            }
        }

        const std::size_t map_begin = dirty_wave_array_memory_byte_to_leaf_refs_.size();
        try {
            dirty_wave_array_memory_byte_to_leaf_refs_.insert(
                dirty_wave_array_memory_byte_to_leaf_refs_.end(), local_map.begin(), local_map.end());
        } catch (...) {
            return false;
        }
        if (map_begin > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            local_map.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            dirty_wave_array_memory_byte_to_leaf_refs_.resize(map_begin);
            return false;
        }
        block.byte_map_begin = static_cast<std::uint32_t>(map_begin);
        block.byte_map_count = static_cast<std::uint32_t>(local_map.size());
        block.has_byte_map = true;
        return true;
    }

    bool ensure_dirty_wave_array_memory_blocks() {
        if (!options_.enable_wave_array_memory_block_precheck) return false;
        if (dirty_wave_array_memory_blocks_valid_) return dirty_wave_array_memory_blocks_complete_;

        dirty_wave_array_memory_blocks_.clear();
        dirty_wave_array_memory_leaf_refs_.clear();
        dirty_wave_array_memory_byte_to_leaf_refs_.clear();
        dirty_wave_array_memory_scalar_leaf_indices_.clear();
        dirty_wave_array_memory_shadow_bytes_.clear();

        for (std::size_t group_index = 0; group_index < dirty_wave_array_groups_.size(); ++group_index) {
            DirtyWaveArrayGroup& g = dirty_wave_array_groups_[group_index];
            g.memory_block_begin = static_cast<std::uint32_t>(dirty_wave_array_memory_blocks_.size());
            g.memory_block_count = 0;
            g.memory_scalar_begin = static_cast<std::uint32_t>(dirty_wave_array_memory_scalar_leaf_indices_.size());
            g.memory_scalar_count = 0;
        }

        struct CandidateRef {
            std::uint32_t leaf_index;
            const unsigned char* begin;
            const unsigned char* end;
            std::uintptr_t begin_addr;
            std::uintptr_t end_addr;
            std::size_t byte_count;
        };

        const std::size_t min_leaf_count = options_.wave_array_memory_block_min_leaf_count == 0
            ? static_cast<std::size_t>(1)
            : options_.wave_array_memory_block_min_leaf_count;
        const std::size_t max_gap = options_.wave_array_memory_block_max_gap;
        const std::size_t max_bytes = options_.wave_array_memory_block_max_bytes == 0
            ? static_cast<std::size_t>(4096)
            : options_.wave_array_memory_block_max_bytes;

        std::vector<CandidateRef> candidates;
        std::vector<CandidateRef> current_refs;
        candidates.reserve(128);
        current_refs.reserve(64);

        for (std::uint32_t group_id = 0; group_id < dirty_wave_array_groups_.size(); ++group_id) {
            DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
            group.memory_block_begin = static_cast<std::uint32_t>(dirty_wave_array_memory_blocks_.size());
            group.memory_scalar_begin = static_cast<std::uint32_t>(dirty_wave_array_memory_scalar_leaf_indices_.size());

            const auto flush_current = [&](const unsigned char*& current_base,
                                           const unsigned char*& current_end,
                                           std::uintptr_t& current_base_addr,
                                           std::uintptr_t& current_end_addr) {
                if (current_refs.empty()) return;
                const std::size_t block_bytes = static_cast<std::size_t>(current_end_addr - current_base_addr);
                if (current_refs.size() >= min_leaf_count && block_bytes != 0 && block_bytes <= max_bytes) {
                    DirtyWaveArrayMemoryBlock block;
                    block.base = current_base;
                    block.byte_count = block_bytes;
                    block.shadow_offset = dirty_wave_array_memory_shadow_bytes_.size();
                    block.first_leaf_ref = static_cast<std::uint32_t>(dirty_wave_array_memory_leaf_refs_.size());
                    block.leaf_count = static_cast<std::uint32_t>(current_refs.size());
                    block.needs_initial_scan = true;
                    if (block_bytes > static_cast<std::size_t>(std::numeric_limits<std::size_t>::max()) - dirty_wave_array_memory_shadow_bytes_.size()) {
                        for (std::size_t j = 0; j < current_refs.size(); ++j) {
                            dirty_wave_array_memory_scalar_leaf_indices_.push_back(current_refs[j].leaf_index);
                        }
                        current_refs.clear();
                        current_base = NULL;
                        current_end = NULL;
                        current_base_addr = 0;
                        current_end_addr = 0;
                        return;
                    }
                    try {
                        dirty_wave_array_memory_shadow_bytes_.resize(dirty_wave_array_memory_shadow_bytes_.size() + block_bytes);
                    } catch (...) {
                        for (std::size_t i = 0; i < current_refs.size(); ++i) {
                            dirty_wave_array_memory_scalar_leaf_indices_.push_back(current_refs[i].leaf_index);
                        }
                        current_refs.clear();
                        current_base = NULL;
                        current_end = NULL;
                        current_base_addr = 0;
                        current_end_addr = 0;
                        return;
                    }
                    for (std::size_t i = 0; i < current_refs.size(); ++i) {
                        const CandidateRef& ref = current_refs[i];
                        const std::size_t off = static_cast<std::size_t>(ref.begin_addr - current_base_addr);
                        dirty_wave_array_memory_leaf_refs_.push_back(DirtyWaveArrayMemoryLeafRef(ref.leaf_index,
                                                                                                 static_cast<std::uint32_t>(off),
                                                                                                 static_cast<std::uint32_t>(ref.byte_count)));
                    }
                    build_dirty_wave_array_memory_block_byte_map_(block);
                    dirty_wave_array_memory_blocks_.push_back(block);
                } else {
                    for (std::size_t i = 0; i < current_refs.size(); ++i) {
                        dirty_wave_array_memory_scalar_leaf_indices_.push_back(current_refs[i].leaf_index);
                    }
                }
                current_refs.clear();
                current_base = NULL;
                current_end = NULL;
                current_base_addr = 0;
                current_end_addr = 0;
            };

            candidates.clear();
            current_refs.clear();

            // Merge memory-block candidates across all ranges of the same wave::array
            // dirty group.  Nested/non-candidate leaves remain on the scalar fallback path.
            for (std::uint32_t range_id = group.first_range;
                 range_id != kInvalidIndex && range_id < dirty_wave_array_ranges_.size();
                 range_id = dirty_wave_array_ranges_[range_id].next_sibling) {
                const DirtyWaveArrayRange& range = dirty_wave_array_ranges_[range_id];
                const std::uint32_t begin_leaf = range.leaf_begin;
                const std::uint32_t end_leaf = begin_leaf + range.leaf_count;
                for (std::uint32_t leaf_id = begin_leaf; leaf_id < end_leaf && leaf_id < dirty_wave_array_leaves_.size(); ++leaf_id) {
                    const DirtyWaveArrayLeaf& leaf = dirty_wave_array_leaves_[leaf_id];
                    const unsigned char* begin = NULL;
                    std::size_t byte_count = 0;
                    if (!dirty_wave_array_leaf_memory_candidate_(leaf, begin, byte_count)) {
                        dirty_wave_array_memory_scalar_leaf_indices_.push_back(leaf_id);
                        continue;
                    }
                    CandidateRef ref;
                    ref.leaf_index = leaf_id;
                    ref.begin = begin;
                    ref.end = begin + byte_count;
                    ref.begin_addr = reinterpret_cast<std::uintptr_t>(begin);
                    ref.end_addr = ref.begin_addr + byte_count;
                    ref.byte_count = byte_count;
                    candidates.push_back(ref);
                }
            }

            std::sort(candidates.begin(), candidates.end(), [](const CandidateRef& a, const CandidateRef& b) {
                if (a.begin_addr != b.begin_addr) return a.begin_addr < b.begin_addr;
                if (a.byte_count != b.byte_count) return a.byte_count < b.byte_count;
                return a.leaf_index < b.leaf_index;
            });

            const unsigned char* current_base = NULL;
            const unsigned char* current_end = NULL;
            std::uintptr_t current_base_addr = 0;
            std::uintptr_t current_end_addr = 0;

            for (std::size_t i = 0; i < candidates.size(); ++i) {
                const CandidateRef& ref = candidates[i];
                if (current_refs.empty()) {
                    current_base = ref.begin;
                    current_end = ref.end;
                    current_base_addr = ref.begin_addr;
                    current_end_addr = ref.end_addr;
                    current_refs.push_back(ref);
                    continue;
                }

                bool can_merge = false;
                if (ref.begin_addr >= current_end_addr) {
                    const std::size_t gap = static_cast<std::size_t>(ref.begin_addr - current_end_addr);
                    const std::size_t new_bytes = static_cast<std::size_t>(ref.end_addr - current_base_addr);
                    can_merge = (gap <= max_gap && new_bytes <= max_bytes);
                }
                if (!can_merge) {
                    flush_current(current_base, current_end, current_base_addr, current_end_addr);
                    current_base = ref.begin;
                    current_end = ref.end;
                    current_base_addr = ref.begin_addr;
                    current_end_addr = ref.end_addr;
                    current_refs.push_back(ref);
                    continue;
                }
                if (ref.end_addr > current_end_addr) {
                    current_end = ref.end;
                    current_end_addr = ref.end_addr;
                }
                current_refs.push_back(ref);
            }
            flush_current(current_base, current_end, current_base_addr, current_end_addr);

            group.memory_block_count = static_cast<std::uint32_t>(dirty_wave_array_memory_blocks_.size()) - group.memory_block_begin;
            group.memory_scalar_count = static_cast<std::uint32_t>(dirty_wave_array_memory_scalar_leaf_indices_.size()) - group.memory_scalar_begin;
        }

        dirty_wave_array_memory_blocks_complete_ = true;
        dirty_wave_array_memory_blocks_valid_ = true;
        return dirty_wave_array_memory_blocks_complete_;
    }

    void sample_dirty_wave_array_memory_leaf_ref_precise_(Cycle cycle,
                                                          DirtyWaveArrayMemoryLeafRef& ref,
                                                          std::vector<TrackEvent>& out) {
        if (ref.last_sample_epoch == dirty_wave_array_memory_sample_epoch_) return;
        ref.last_sample_epoch = dirty_wave_array_memory_sample_epoch_;
        if (ref.dirty_leaf_index >= dirty_wave_array_leaves_.size()) return;
        DirtyWaveArrayLeaf& leaf = dirty_wave_array_leaves_[ref.dirty_leaf_index];
        if (leaf.memory_last_sample_epoch == dirty_wave_array_memory_sample_epoch_) return;
        leaf.memory_last_sample_epoch = dirty_wave_array_memory_sample_epoch_;
        ScalarSnapshot sample;
        if (!sample_one_dirty_wave_array_leaf_changed(leaf, sample)) return;
        out.emplace_back();
        fill_dirty_wave_array_leaf_event(leaf, sample, cycle, out.back());
    }

    bool sample_dirty_wave_array_memory_changed_byte_range_by_map_(Cycle cycle,
                                                                    DirtyWaveArrayMemoryBlock& block,
                                                                    std::size_t begin,
                                                                    std::size_t end,
                                                                    std::vector<TrackEvent>& out) {
        if (!block.has_byte_map || block.byte_map_count < block.byte_count) return false;
        if (begin > block.byte_count) begin = block.byte_count;
        if (end > block.byte_count) end = block.byte_count;
        const std::size_t map_begin = static_cast<std::size_t>(block.byte_map_begin);
        if (map_begin > dirty_wave_array_memory_byte_to_leaf_refs_.size() ||
            block.byte_count > dirty_wave_array_memory_byte_to_leaf_refs_.size() - map_begin) {
            return false;
        }
        for (std::size_t off = begin; off < end; ++off) {
            const std::uint32_t ref_index = dirty_wave_array_memory_byte_to_leaf_refs_[map_begin + off];
            if (ref_index == kInvalidIndex || ref_index >= dirty_wave_array_memory_leaf_refs_.size()) continue;
            sample_dirty_wave_array_memory_leaf_ref_precise_(cycle, dirty_wave_array_memory_leaf_refs_[ref_index], out);
        }
        return true;
    }

    void sample_dirty_wave_array_memory_changed_byte_range_(Cycle cycle,
                                                            DirtyWaveArrayMemoryBlock& block,
                                                            std::size_t begin,
                                                            std::size_t end,
                                                            std::vector<TrackEvent>& out) {
        if (begin >= end) return;
        if (block.has_byte_map &&
            sample_dirty_wave_array_memory_changed_byte_range_by_map_(cycle, block, begin, end, out)) {
            return;
        }

        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        std::uint32_t lo = begin_ref;
        std::uint32_t hi = end_ref < dirty_wave_array_memory_leaf_refs_.size()
            ? end_ref
            : static_cast<std::uint32_t>(dirty_wave_array_memory_leaf_refs_.size());
        while (lo < hi) {
            const std::uint32_t mid = lo + ((hi - lo) >> 1);
            const DirtyWaveArrayMemoryLeafRef& ref = dirty_wave_array_memory_leaf_refs_[mid];
            const std::size_t ref_end = static_cast<std::size_t>(ref.offset) + static_cast<std::size_t>(ref.byte_count);
            if (ref_end <= begin) lo = mid + 1;
            else hi = mid;
        }
        for (std::uint32_t r = lo; r < end_ref && r < dirty_wave_array_memory_leaf_refs_.size(); ++r) {
            DirtyWaveArrayMemoryLeafRef& ref = dirty_wave_array_memory_leaf_refs_[r];
            const std::size_t ref_begin = static_cast<std::size_t>(ref.offset);
            if (ref_begin >= end) break;
            const std::size_t ref_end = ref_begin + static_cast<std::size_t>(ref.byte_count);
            if (ref_begin < end && begin < ref_end) {
                sample_dirty_wave_array_memory_leaf_ref_precise_(cycle, ref, out);
            }
        }
    }

    void sample_dirty_wave_array_memory_changed_mask16_(Cycle cycle,
                                                        DirtyWaveArrayMemoryBlock& block,
                                                        std::size_t base_off,
                                                        std::uint32_t mask,
                                                        std::vector<TrackEvent>& out) {
        while (mask != 0) {
            unsigned bit = 0; while (bit < 32u && ((mask >> bit) & 1u) == 0u) ++bit;
            std::uint32_t run = mask >> bit;
            unsigned len = 0;
            while ((run & 1u) != 0u && bit + len < 16u) { ++len; run >>= 1u; }
            sample_dirty_wave_array_memory_changed_byte_range_(cycle, block, base_off + bit, base_off + bit + len, out);
            const std::uint32_t clear_mask = (len >= 32u) ? 0xFFFFFFFFu : (((1u << len) - 1u) << bit);
            mask &= ~clear_mask;
        }
    }

    void sample_dirty_wave_array_memory_changed_mask32_(Cycle cycle,
                                                        DirtyWaveArrayMemoryBlock& block,
                                                        std::size_t base_off,
                                                        std::uint32_t mask,
                                                        std::vector<TrackEvent>& out) {
        while (mask != 0) {
            unsigned bit = 0; while (bit < 32u && ((mask >> bit) & 1u) == 0u) ++bit;
            std::uint32_t run = mask >> bit;
            unsigned len = 0;
            while ((run & 1u) != 0u && bit + len < 32u) { ++len; run >>= 1u; }
            sample_dirty_wave_array_memory_changed_byte_range_(cycle, block, base_off + bit, base_off + bit + len, out);
            const std::uint32_t clear_mask = (len >= 32u) ? 0xFFFFFFFFu : (((1u << len) - 1u) << bit);
            mask &= ~clear_mask;
        }
    }

    void sample_dirty_wave_array_memory_block_scalar_mask_to_buffer_(Cycle cycle,
                                                                     DirtyWaveArrayMemoryBlock& block,
                                                                     unsigned char* shadow,
                                                                     std::vector<TrackEvent>& out,
                                                                     std::size_t start_off) {
        std::size_t run_begin = static_cast<std::size_t>(-1);
        for (std::size_t off = start_off; off < block.byte_count; ++off) {
            const unsigned char cur = block.base[off];
            if (cur != shadow[off]) {
                shadow[off] = cur;
                if (run_begin == static_cast<std::size_t>(-1)) run_begin = off;
            } else if (run_begin != static_cast<std::size_t>(-1)) {
                sample_dirty_wave_array_memory_changed_byte_range_(cycle, block, run_begin, off, out);
                run_begin = static_cast<std::size_t>(-1);
            }
        }
        if (run_begin != static_cast<std::size_t>(-1)) {
            sample_dirty_wave_array_memory_changed_byte_range_(cycle, block, run_begin, block.byte_count, out);
        }
    }

    void sample_dirty_wave_array_memory_block_simd_mask_to_buffer_(Cycle cycle,
                                                                   DirtyWaveArrayMemoryBlock& block,
                                                                   unsigned char* shadow,
                                                                   std::vector<TrackEvent>& out) {
        std::size_t off = 0;
        const FlatMemoryBlockSimdBackend backend =
            (options_.enable_wave_array_memory_block_simd_mask &&
             block.byte_count >= options_.wave_array_memory_block_simd_min_bytes)
                ? resolve_flat_memory_block_simd_backend_()
                : FlatMemoryBlockSimdBackend::Scalar;

#if defined(__AVX2__) || defined(_M_AVX2)
        if (backend == FlatMemoryBlockSimdBackend::AVX2) {
            for (; off + 32 <= block.byte_count; off += 32) {
                const __m256i cur = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block.base + off));
                const __m256i old = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shadow + off));
                const __m256i eq = _mm256_cmpeq_epi8(cur, old);
                const std::uint32_t eq_mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(eq));
                const std::uint32_t diff_mask = (~eq_mask) & static_cast<std::uint32_t>(0xFFFFFFFFu);
                if (diff_mask == 0) continue;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(shadow + off), cur);
                sample_dirty_wave_array_memory_changed_mask32_(cycle, block, off, diff_mask, out);
            }
        }
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        if (backend == FlatMemoryBlockSimdBackend::SSE2) {
            for (; off + 16 <= block.byte_count; off += 16) {
                const __m128i cur = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block.base + off));
                const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadow + off));
                const __m128i eq = _mm_cmpeq_epi8(cur, old);
                const std::uint32_t eq_mask = static_cast<std::uint32_t>(_mm_movemask_epi8(eq));
                const std::uint32_t diff_mask = (~eq_mask) & static_cast<std::uint32_t>(0xFFFFu);
                if (diff_mask == 0) continue;
                _mm_storeu_si128(reinterpret_cast<__m128i*>(shadow + off), cur);
                sample_dirty_wave_array_memory_changed_mask16_(cycle, block, off, diff_mask, out);
            }
        }
#endif

        sample_dirty_wave_array_memory_block_scalar_mask_to_buffer_(cycle, block, shadow, out, off);
    }

    void sample_dirty_wave_array_memory_block_initial_to_buffer_(Cycle cycle,
                                                                 DirtyWaveArrayMemoryBlock& block,
                                                                 unsigned char* shadow,
                                                                 std::vector<TrackEvent>& out) {
        const std::uint32_t begin_ref = block.first_leaf_ref;
        const std::uint32_t end_ref = begin_ref + block.leaf_count;
        for (std::uint32_t r = begin_ref; r < end_ref && r < dirty_wave_array_memory_leaf_refs_.size(); ++r) {
            sample_dirty_wave_array_memory_leaf_ref_precise_(cycle, dirty_wave_array_memory_leaf_refs_[r], out);
        }
        std::memcpy(shadow, block.base, block.byte_count);
        block.needs_initial_scan = false;
    }

    void sample_dirty_wave_array_memory_block_to_buffer_(Cycle cycle,
                                                         DirtyWaveArrayMemoryBlock& block,
                                                         std::vector<TrackEvent>& out) {
        if (block.base == NULL || block.byte_count == 0) return;
        if (block.shadow_offset > dirty_wave_array_memory_shadow_bytes_.size() ||
            block.byte_count > dirty_wave_array_memory_shadow_bytes_.size() - block.shadow_offset) return;
        unsigned char* shadow = dirty_wave_array_memory_shadow_bytes_.empty()
            ? NULL
            : &dirty_wave_array_memory_shadow_bytes_[block.shadow_offset];
        if (!shadow) return;
        if (block.needs_initial_scan) {
            sample_dirty_wave_array_memory_block_initial_to_buffer_(cycle, block, shadow, out);
            return;
        }
        sample_dirty_wave_array_memory_block_simd_mask_to_buffer_(cycle, block, shadow, out);
    }

    void sample_dirty_wave_array_group_to_buffer(std::uint32_t group_id, Cycle cycle, std::vector<TrackEvent>& out) {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) return;
        DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
        if (dirty_wave_array_memory_blocks_valid_ && options_.enable_wave_array_memory_block_precheck) {
            const std::uint32_t block_begin = group.memory_block_begin;
            const std::uint32_t block_end = block_begin + group.memory_block_count;
            for (std::uint32_t b = block_begin; b < block_end && b < dirty_wave_array_memory_blocks_.size(); ++b) {
                sample_dirty_wave_array_memory_block_to_buffer_(cycle, dirty_wave_array_memory_blocks_[b], out);
            }

            const std::uint32_t scalar_begin = group.memory_scalar_begin;
            const std::uint32_t scalar_end = scalar_begin + group.memory_scalar_count;
            for (std::uint32_t s = scalar_begin; s < scalar_end && s < dirty_wave_array_memory_scalar_leaf_indices_.size(); ++s) {
                const std::uint32_t leaf_id = dirty_wave_array_memory_scalar_leaf_indices_[s];
                if (leaf_id >= dirty_wave_array_leaves_.size()) continue;
                ScalarSnapshot sample;
                DirtyWaveArrayLeaf& leaf = dirty_wave_array_leaves_[leaf_id];
                if (!sample_one_dirty_wave_array_leaf_changed(leaf, sample)) continue;
                out.emplace_back();
                fill_dirty_wave_array_leaf_event(leaf, sample, cycle, out.back());
            }
            return;
        }

        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex && range_id < dirty_wave_array_ranges_.size();
             range_id = dirty_wave_array_ranges_[range_id].next_sibling) {
            DirtyWaveArrayRange& range = dirty_wave_array_ranges_[range_id];
            const std::uint32_t begin = range.leaf_begin;
            const std::uint32_t end = begin + range.leaf_count;
            for (std::uint32_t i = begin; i < end && i < dirty_wave_array_leaves_.size(); ++i) {
                ScalarSnapshot sample;
                DirtyWaveArrayLeaf& leaf = dirty_wave_array_leaves_[i];
                if (!sample_one_dirty_wave_array_leaf_changed(leaf, sample)) continue;
                out.emplace_back();
                fill_dirty_wave_array_leaf_event(leaf, sample, cycle, out.back());
            }
        }
    }

    std::size_t dirty_wave_array_leaf_count_for_group(std::uint32_t group_id) const noexcept {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) return 0;
        const DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
        std::size_t total = 0;
        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex && range_id < dirty_wave_array_ranges_.size();
             range_id = dirty_wave_array_ranges_[range_id].next_sibling) {
            total += dirty_wave_array_ranges_[range_id].leaf_count;
        }
        return total;
    }

    std::size_t dirty_wave_array_leaf_count_for_current_groups() const noexcept {
        std::size_t total = 0;
        for (std::uint32_t i = 0; i < global_dirty_wave_array_group_count_; ++i) {
            total += dirty_wave_array_leaf_count_for_group(global_dirty_wave_array_group_ids_[i]);
        }
        return total;
    }

    void sample_dirty_wave_array_groups(Cycle cycle) {
        append_initial_dirty_wave_array_groups();
        if (global_dirty_wave_array_group_count_ == 0) return;
        if (options_.enable_wave_array_memory_block_precheck) {
            ensure_dirty_wave_array_memory_blocks();
            ++dirty_wave_array_memory_sample_epoch_;
            if (dirty_wave_array_memory_sample_epoch_ == 0) {
                dirty_wave_array_memory_sample_epoch_ = 1;
                for (std::size_t i = 0; i < dirty_wave_array_memory_leaf_refs_.size(); ++i) {
                    dirty_wave_array_memory_leaf_refs_[i].last_sample_epoch = 0;
                }
                for (std::size_t i = 0; i < dirty_wave_array_leaves_.size(); ++i) {
                    dirty_wave_array_leaves_[i].memory_last_sample_epoch = 0;
                }
            }
        }
        const std::size_t expected = dirty_wave_array_leaf_count_for_current_groups();
        if (caller_parallel_events_.capacity() < expected) caller_parallel_events_.reserve(expected);
        caller_parallel_events_.clear();
        for (std::uint32_t i = 0; i < global_dirty_wave_array_group_count_; ++i) {
            sample_dirty_wave_array_group_to_buffer(global_dirty_wave_array_group_ids_[i], cycle, caller_parallel_events_);
        }
        for (std::size_t i = 0; i < caller_parallel_events_.size(); ++i) {
            sink_.on_sample(caller_parallel_events_[i]);
        }
        global_dirty_wave_array_group_count_ = 0;
    }

    std::size_t dirty_leaf_count_for_group(std::uint32_t group_id) const noexcept {
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return 0;
        const DirtyPeekGroup& group = dirty_peek_groups_[group_id];
        std::size_t total = 0;
        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex && range_id < dirty_peek_ranges_.size();
             range_id = dirty_peek_ranges_[range_id].next_sibling) {
            total += dirty_peek_ranges_[range_id].leaf_count;
        }
        return total;
    }

    std::size_t dirty_leaf_count_for_group_id_slice(std::size_t begin_pos, std::size_t end_pos) const noexcept {
        const std::size_t count = static_cast<std::size_t>(global_dirty_group_count_);
        if (begin_pos > count) begin_pos = count;
        if (end_pos > count) end_pos = count;
        std::size_t total = 0;
        for (std::size_t pos = begin_pos; pos < end_pos; ++pos) {
            total += dirty_leaf_count_for_group(global_dirty_group_ids_[pos]);
        }
        return total;
    }

    void sample_dirty_group_id_slice_to_buffer(Cycle cycle, std::size_t begin_pos, std::size_t end_pos, std::vector<TrackEvent>& out) {
        const std::size_t count = static_cast<std::size_t>(global_dirty_group_count_);
        if (begin_pos > count) begin_pos = count;
        if (end_pos > count) end_pos = count;
        for (std::size_t pos = begin_pos; pos < end_pos; ++pos) {
            sample_dirty_peek_group_to_buffer(global_dirty_group_ids_[pos], cycle, out);
        }
    }

    void sample_dirty_group_id_slice_to_sink(Cycle cycle, std::size_t begin_pos, std::size_t end_pos) {
        const std::size_t expected = dirty_leaf_count_for_group_id_slice(begin_pos, end_pos);
        if (caller_parallel_events_.capacity() < expected) {
            caller_parallel_events_.reserve(expected);
        }
        caller_parallel_events_.clear();
        sample_dirty_group_id_slice_to_buffer(cycle, begin_pos, end_pos, caller_parallel_events_);
        for (std::size_t i = 0; i < caller_parallel_events_.size(); ++i) {
            sink_.on_sample(caller_parallel_events_[i]);
        }
    }

    std::size_t desired_worker_count_for_items(std::size_t item_count) const {
        std::size_t requested = options_.sampling_threads;
        if (requested == 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            requested = hw > 1 ? static_cast<std::size_t>(hw - 1) : 1;
        }
        if (item_count == 0) return 0;
        if (requested > item_count) requested = item_count;
        return requested;
    }

    bool should_use_dirty_peek_parallel_sampling() const {
        if (!options_.enable_parallel_sampling) return false;
        if (!options_.enable_dirty_peek_parallel_sampling) return false;
        if (options_.debug_log || options_.debug_log_track_samples || options_.debug_log_root_expand_stats) return false;
        if (global_dirty_group_count_ <= options_.dirty_peek_parallel_threshold) return false;
        return desired_worker_count_for_items(global_dirty_group_count_) > 0;
    }

    void build_dirty_worker_plans_by_count(std::size_t participants) {
        if (dirty_worker_plan_begin_.size() < participants) dirty_worker_plan_begin_.resize(participants);
        if (dirty_worker_plan_end_.size() < participants) dirty_worker_plan_end_.resize(participants);
        const std::size_t n = static_cast<std::size_t>(global_dirty_group_count_);
        for (std::size_t i = 0; i < participants; ++i) {
            dirty_worker_plan_begin_[i] = static_cast<std::uint32_t>((n * i) / participants);
            dirty_worker_plan_end_[i] = static_cast<std::uint32_t>((n * (i + 1)) / participants);
        }
    }

    void build_dirty_worker_plans_by_leaf_weight(std::size_t participants) {
        if (dirty_worker_plan_begin_.size() < participants) dirty_worker_plan_begin_.resize(participants);
        if (dirty_worker_plan_end_.size() < participants) dirty_worker_plan_end_.resize(participants);
        const std::size_t n = static_cast<std::size_t>(global_dirty_group_count_);
        std::uint64_t total_weight = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t gid = global_dirty_group_ids_[i];
            if (gid < dirty_peek_groups_.size()) total_weight += dirty_peek_groups_[gid].total_leaf_count;
        }
        if (total_weight == 0) {
            build_dirty_worker_plans_by_count(participants);
            return;
        }
        std::size_t cursor = 0;
        std::uint64_t acc = 0;
        for (std::size_t w = 0; w < participants; ++w) {
            const std::size_t begin = cursor;
            const std::uint64_t target = (total_weight * static_cast<std::uint64_t>(w + 1)) / participants;
            while (cursor < n && acc < target) {
                const std::uint32_t gid = global_dirty_group_ids_[cursor];
                if (gid < dirty_peek_groups_.size()) acc += dirty_peek_groups_[gid].total_leaf_count;
                ++cursor;
            }
            dirty_worker_plan_begin_[w] = static_cast<std::uint32_t>(begin);
            dirty_worker_plan_end_[w] = static_cast<std::uint32_t>(cursor);
        }
    }

    void sample_dirty_peek_groups(Cycle cycle) {
        append_initial_dirty_peek_groups();
        if (global_dirty_group_count_ == 0) return;

        begin_dirty_peek_storage_sample_epoch_();

        if (options_.enable_dirty_peek_memory_block_precheck) {
            ensure_dirty_peek_memory_blocks();
            ++dirty_peek_memory_sample_epoch_;
            if (dirty_peek_memory_sample_epoch_ == 0) {
                dirty_peek_memory_sample_epoch_ = 1;
                for (std::size_t i = 0; i < dirty_peek_memory_leaf_refs_.size(); ++i) {
                    dirty_peek_memory_leaf_refs_[i].last_sample_epoch = 0;
                }
                for (std::size_t i = 0; i < dirty_peek_leaves_.size(); ++i) {
                    dirty_peek_leaves_[i].memory_last_sample_epoch = 0;
                }
            }
        }

        if (!should_use_dirty_peek_parallel_sampling()) {
            sample_dirty_group_id_slice_to_sink(cycle, 0, global_dirty_group_count_);
            global_dirty_group_count_ = 0;
            return;
        }

        const std::size_t worker_count = desired_worker_count_for_items(global_dirty_group_count_);
        if (worker_count == 0) {
            sample_dirty_group_id_slice_to_sink(cycle, 0, global_dirty_group_count_);
            global_dirty_group_count_ = 0;
            return;
        }
        ensure_parallel_workers(worker_count);

        const std::size_t participants = worker_count + 1;
        if (options_.dirty_peek_balance_by_leaf_count) build_dirty_worker_plans_by_leaf_weight(participants);
        else build_dirty_worker_plans_by_count(participants);

        for (std::size_t i = 0; i < worker_count; ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            reserve_worker_event_capacity(worker, dirty_leaf_count_for_group_id_slice(dirty_worker_plan_begin_[i], dirty_worker_plan_end_[i]));
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->cycle = cycle;
                worker->job_kind = WorkerJobKind::DirtyGroupSlice;
                worker->begin_pos = dirty_worker_plan_begin_[i];
                worker->end_pos = dirty_worker_plan_end_[i];
                worker->events = NULL;
                worker->done = false;
                worker->has_job = true;
            }
            worker->cv.notify_one();
        }

        {
            const std::size_t expected = dirty_leaf_count_for_group_id_slice(dirty_worker_plan_begin_[worker_count],
                                                                             dirty_worker_plan_end_[worker_count]);
            if (caller_parallel_events_.capacity() < expected) {
                caller_parallel_events_.reserve(expected);
            }
        }
        caller_parallel_events_.clear();
        sample_dirty_group_id_slice_to_buffer(cycle,
                                              dirty_worker_plan_begin_[worker_count],
                                              dirty_worker_plan_end_[worker_count],
                                              caller_parallel_events_);

        for (std::size_t i = 0; i < worker_count; ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            std::vector<TrackEvent>* events = NULL;
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->cv.wait(lock, [worker]() { return worker->done; });
                events = worker->events;
            }
            if (!events) continue;
            for (std::size_t j = 0; j < events->size(); ++j) sink_.on_sample((*events)[j]);
        }
        for (std::size_t j = 0; j < caller_parallel_events_.size(); ++j) sink_.on_sample(caller_parallel_events_[j]);
        global_dirty_group_count_ = 0;
    }

    void sample_serial(Cycle cycle) {
        sample_track_ids_to_sink(cycle, parallel_track_ids_, true);
    }

    void sample_parallel(Cycle cycle) {
        const std::size_t worker_count = desired_parallel_worker_count();
        if (worker_count == 0) {
            sample_serial(cycle);
            return;
        }
        ensure_parallel_workers(worker_count);

        const std::size_t total = parallel_track_ids_.size();
        const std::size_t participants = worker_count + 1; // workers + caller thread
        const std::size_t base = total / participants;
        const std::size_t extra = total % participants;

        std::size_t cursor = 0;
        for (std::size_t i = 0; i < worker_count; ++i) {
            const std::size_t span = base + (i < extra ? static_cast<std::size_t>(1) : static_cast<std::size_t>(0));
            const std::size_t begin_pos = cursor;
            const std::size_t end_pos = begin_pos + span;
            cursor = end_pos;

            SampleWorker* worker = parallel_workers_[i].get();
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->cycle = cycle;
                worker->job_kind = WorkerJobKind::TrackSlice;
                worker->begin_pos = begin_pos;
                worker->end_pos = end_pos;
                worker->events = NULL;
                worker->done = false;
                worker->has_job = true;
            }
            worker->cv.notify_one();
        }

        // Caller thread processes the final contiguous slice.  Fixed equal slices
        // keep event order stable enough for deterministic merging while avoiding
        // per-track/chunk atomic scheduling overhead.
        caller_parallel_events_.clear();
        sample_track_id_slice_to_buffer(cycle, cursor, total, caller_parallel_events_);

        // Preserve deterministic submission order: worker slices are emitted first
        // in ascending track-id order, then the caller slice.
        for (std::size_t i = 0; i < worker_count; ++i) {
            SampleWorker* worker = parallel_workers_[i].get();
            std::vector<TrackEvent>* events = NULL;
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->cv.wait(lock, [worker]() { return worker->done; });
                events = worker->events;
            }
            if (!events) continue;
            for (std::size_t j = 0; j < events->size(); ++j) {
                sink_.on_sample((*events)[j]);
            }
        }

        for (std::size_t j = 0; j < caller_parallel_events_.size(); ++j) {
            sink_.on_sample(caller_parallel_events_[j]);
        }
    }

    static const char* node_kind_name(NodeKind kind) {
        switch (kind) {
        case NodeKind::Aggregate: return "Aggregate";
        case NodeKind::Leaf: return "Leaf";
        case NodeKind::PointerLink: return "PointerLink";
        case NodeKind::FixedIndexedContainer: return "FixedIndexedContainer";
        case NodeKind::ValueSource: return "ValueSource";
        case NodeKind::Unsupported: return "Unsupported";
        default: return "UnknownNodeKind";
        }
    }

    static const char* value_kind_name(ValueKind kind) {
        switch (kind) {
        case ValueKind::Bool: return "Bool";
        case ValueKind::SignedInt: return "SignedInt";
        case ValueKind::UnsignedInt: return "UnsignedInt";
        case ValueKind::Float64: return "Float64";
        case ValueKind::StringLike: return "StringLike";
        case ValueKind::Enum: return "Enum";
        case ValueKind::PointerAddress: return "PointerAddress";
        case ValueKind::Unsupported: return "Unsupported";
        case ValueKind::Unknown: return "Unknown";
        default: return "UnknownValueKind";
        }
    }

    static const char* invalid_kind_name(InvalidKind invalid) {
        switch (invalid) {
        case InvalidKind::None: return "None";
        case InvalidKind::Z: return "Z";
        case InvalidKind::Null: return "Null";
        case InvalidKind::Unsupported: return "Unsupported";
        case InvalidKind::RecursiveCut: return "RecursiveCut";
        default: return "UnknownInvalid";
        }
    }

    bool same_event_payload(const TrackRuntimeState& state, const TrackEvent& ev) const {
        if (state.last_has_change_bits || ev.has_change_bits) {
            return state.last_has_change_bits && ev.has_change_bits &&
                   state.last_change_bits == ev.change_bits;
        }
        return runtime_last_value_ref(state) == (ev.value ? std::string(ev.value) : std::string());
    }

    void cache_last_event(TrackRuntimeState& state, const TrackEvent& ev) {
        state.has_last_event = true;
        state.last_invalid = ev.invalid;
        state.last_has_change_bits = ev.has_change_bits;
        state.last_change_bits = ev.change_bits;
        if (ev.has_change_bits) {
            clear_runtime_last_value(state);
        } else {
            set_runtime_last_value(state, ev.value ? std::string(ev.value) : std::string());
        }
    }

    static std::string event_value_for_debug(const TrackEvent& ev) {
        if (ev.value && ev.value[0] != '\0') return std::string(ev.value);
        if (ev.has_bool) return ev.bool_value ? "true" : "false";
        if (ev.has_i64) return detail::scalar_to_string(ev.i64_value);
        if (ev.has_u64) return detail::to_string_unsigned(ev.u64_value);
        if (ev.has_f64) return detail::scalar_to_string(ev.f64_value);
        return std::string();
    }

    void open_debug_log() {
        if (!options_.debug_log) return;
        if (!options_.debug_log_path.empty()) {
            debug_log_stream_.open(options_.debug_log_path.c_str(), std::ios::out | std::ios::trunc);
        }
    }

    void debug_log_msg(const std::string& msg) {
        if (!options_.debug_log && !options_.debug_log_root_expand_stats) return;
        if (options_.debug_log_max_events != 0 && debug_log_event_count_ >= options_.debug_log_max_events) return;
        ++debug_log_event_count_;
        std::ostream& os = debug_log_stream_.is_open() ? static_cast<std::ostream&>(debug_log_stream_) : std::cerr;
        os << "[wave] " << msg << std::endl;
    }

    void open_parallel_flat_leaf_load_log_() {
        if (!options_.log_parallel_flat_leaf_load) return;
        if (options_.parallel_flat_leaf_load_log_path.empty()) return;
        parallel_flat_leaf_load_log_stream_.open(
            options_.parallel_flat_leaf_load_log_path.c_str(), std::ios::out | std::ios::trunc);
        if (parallel_flat_leaf_load_log_stream_.is_open()) {
            parallel_flat_leaf_load_log_stream_
                << "# cycle,total_flat_leaves,worker_count,participants,total_events,flush_us\n"
                << "# role,id,begin,end,scanned,events,time_us,events_per_1000_leaves,leaves_per_us\n";
        }
    }

    bool should_log_parallel_flat_leaf_load_(Cycle cycle) const {
        if (!options_.log_parallel_flat_leaf_load) return false;
        if (options_.parallel_flat_leaf_load_log_period == 0) return false;
        return (cycle % options_.parallel_flat_leaf_load_log_period) == 0;
    }

    std::ostream& parallel_flat_leaf_load_output_stream_() {
        if (parallel_flat_leaf_load_log_stream_.is_open()) {
            return parallel_flat_leaf_load_log_stream_;
        }
        return std::cerr;
    }


    void debug_log_unsupported_raw(const std::string& path,
                                   NodeId parent_id,
                                   const std::string& reason,
                                   const std::string& type_name,
                                   const std::string& marker_suffix = std::string(),
                                   const void* address = NULL) {
        if (!options_.debug_log) return;
        std::string msg = std::string("unsupported reason=") + reason +
                          " path=" + path +
                          " parent=" + detail::to_string_unsigned(parent_id);
        if (!type_name.empty()) msg += " type=" + type_name;
        if (!marker_suffix.empty()) msg += " marker=" + marker_suffix;
        if (address) msg += " addr=" + detail::pointer_to_string(address);
        debug_log_msg(msg);
    }

    template <typename T>
    void debug_log_unsupported_type(const std::string& path,
                                    NodeId parent_id,
                                    const std::string& reason,
                                    const std::string& marker_suffix = std::string(),
                                    const void* address = NULL) {
        debug_log_unsupported_raw(path, parent_id, reason, typeid(T).name(), marker_suffix, address);
    }

    template <typename T, typename IdT>
    static T* id_lookup(std::vector<T>& items, IdT id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index == 0 || index >= items.size()) return NULL;
        return &items[index];
    }

    template <typename T, typename IdT>
    static const T* id_lookup_const(const std::vector<T>& items, IdT id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index == 0 || index >= items.size()) return NULL;
        return &items[index];
    }

    template <typename T>
    static void reserve_id_slot_append_capacity(std::vector<T>& items,
                                                std::size_t index,
                                                std::size_t growth_chunk) {
        if (growth_chunk == 0) return;
        if (index < items.capacity()) return;

        std::size_t new_capacity = items.capacity();
        if (new_capacity == 0) new_capacity = growth_chunk;
        while (new_capacity <= index) {
            const std::size_t doubled = new_capacity * 2;
            const std::size_t chunked = new_capacity + growth_chunk;
            new_capacity = doubled > chunked ? doubled : chunked;
        }
        items.reserve(new_capacity);
    }

    // Id values are monotonic and dense in this runtime. The hot path should be
    // exactly append-one-slot. The gap path is only a defensive fallback for
    // rollback/diagnostic scenarios and should not appear in normal profiles.
    template <typename T, typename IdT>
    static T& ensure_id_slot(std::vector<T>& items, IdT id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index == items.size()) {
            items.emplace_back();
            return items[index];
        }
        if (index < items.size()) {
            return items[index];
        }
        items.resize(index + 1);
        return items[index];
    }

    template <typename T, typename IdT>
    static T& ensure_id_slot_reserved(std::vector<T>& items, IdT id, std::size_t growth_chunk) {
        const std::size_t index = static_cast<std::size_t>(id);
        reserve_id_slot_append_capacity(items, index, growth_chunk);
        return ensure_id_slot(items, id);
    }

    NodeDesc* find_node(NodeId id) { return id_lookup(nodes_, id); }
    const NodeDesc* find_node(NodeId id) const { return id_lookup_const(nodes_, id); }
    TrackDesc* find_track(TrackId id) { return id_lookup(tracks_, id); }
    const TrackDesc* find_track(TrackId id) const { return id_lookup_const(tracks_, id); }
    TrackRuntimeState* find_track_runtime(TrackId id) { return id_lookup(track_runtime_, id); }
    const TrackRuntimeState* find_track_runtime(TrackId id) const { return id_lookup_const(track_runtime_, id); }
    LazyValueWatch* find_lazy_watch(WatchId id) { return id_lookup(lazy_value_watches_, id); }

    static bool contains_expanding_key(const std::vector<ObjectKey>& stack, const ObjectKey& key) {
        for (std::size_t i = 0; i < stack.size(); ++i) {
            if (stack[i] == key) return true;
        }
        return false;
    }

    struct TopologyCheckpoint {
        NodeId next_node_id;
        TrackId next_track_id;
        ObjectId next_object_id;
        WatchId next_watch_id;
        std::size_t nodes_size;
        std::size_t node_names_size;
        std::size_t node_debug_paths_size;
        std::size_t tracks_size;
        std::size_t track_paths_size;
        std::size_t track_runtime_size;
        std::size_t track_runtime_last_values_size;
        std::size_t scalar_getter_storage_size;
        std::size_t objects_size;
        std::size_t object_debug_names_size;
        std::size_t object_created_keys_size;
        std::size_t lazy_value_watches_size;
        std::size_t all_track_ids_size;
        std::size_t parallel_track_ids_size;
        std::size_t main_thread_track_ids_size;
        std::size_t dirty_peek_groups_size;
        std::size_t dirty_peek_ranges_size;
        std::size_t dirty_peek_leaves_size;
        std::size_t dirty_peek_storage_by_key_size;
        std::size_t dirty_wave_value_groups_size;
        std::size_t dirty_wave_value_ranges_size;
        std::size_t dirty_wave_value_leaves_size;
        std::size_t dirty_wave_value_addr_table_size;
        std::size_t dirty_wave_array_groups_size;
        std::size_t dirty_wave_array_ranges_size;
        std::size_t dirty_wave_array_leaves_size;
        std::size_t dirty_wave_array_addr_table_size;
        std::size_t bound_dirty_hooks_size;
        NodeId parent_id;
        NodeId parent_first_child;
    };

    TopologyCheckpoint make_topology_checkpoint(NodeId parent_id) const {
        TopologyCheckpoint cp;
        cp.next_node_id = next_node_id_;
        cp.next_track_id = next_track_id_;
        cp.next_object_id = next_object_id_;
        cp.next_watch_id = next_watch_id_;
        cp.nodes_size = nodes_.size();
        cp.node_names_size = node_names_.size();
        cp.node_debug_paths_size = node_debug_paths_.size();
        cp.tracks_size = tracks_.size();
        cp.track_paths_size = track_paths_.size();
        cp.track_runtime_size = track_runtime_.size();
        cp.track_runtime_last_values_size = track_runtime_last_values_.size();
        cp.scalar_getter_storage_size = scalar_getter_storage_.size();
        cp.objects_size = objects_.size();
        cp.object_debug_names_size = object_debug_names_.size();
        cp.object_created_keys_size = object_created_keys_.size();
        cp.lazy_value_watches_size = lazy_value_watches_.size();
        cp.all_track_ids_size = all_track_ids_.size();
        cp.parallel_track_ids_size = parallel_track_ids_.size();
        cp.main_thread_track_ids_size = main_thread_track_ids_.size();
        cp.dirty_peek_groups_size = dirty_peek_groups_.size();
        cp.dirty_peek_ranges_size = dirty_peek_ranges_.size();
        cp.dirty_peek_leaves_size = dirty_peek_leaves_.size();
        cp.dirty_peek_storage_by_key_size = dirty_peek_storage_by_key_.size();
        cp.dirty_wave_value_groups_size = dirty_wave_value_groups_.size();
        cp.dirty_wave_value_ranges_size = dirty_wave_value_ranges_.size();
        cp.dirty_wave_value_leaves_size = dirty_wave_value_leaves_.size();
        cp.dirty_wave_value_addr_table_size = dirty_wave_value_addr_table_.size();
        cp.dirty_wave_array_groups_size = dirty_wave_array_groups_.size();
        cp.dirty_wave_array_ranges_size = dirty_wave_array_ranges_.size();
        cp.dirty_wave_array_leaves_size = dirty_wave_array_leaves_.size();
        cp.dirty_wave_array_addr_table_size = dirty_wave_array_addr_table_.size();
        cp.bound_dirty_hooks_size = bound_dirty_hooks_.size();
        cp.parent_id = parent_id;
        const NodeDesc* parent = parent_id != 0 && parent_id < nodes_.size() ? &nodes_[static_cast<std::size_t>(parent_id)] : NULL;
        cp.parent_first_child = parent ? parent->first_child : 0;
        return cp;
    }

    void rollback_topology_to(const TopologyCheckpoint& cp) {
        if (cp.parent_id != 0 && cp.parent_id < nodes_.size()) {
            nodes_[static_cast<std::size_t>(cp.parent_id)].first_child = cp.parent_first_child;
        }
        while (object_created_keys_.size() > cp.object_created_keys_size) {
            object_id_by_key_.erase(object_created_keys_.back());
            object_created_keys_.pop_back();
        }
        nodes_.resize(cp.nodes_size);
        node_names_.resize(cp.node_names_size);
        node_debug_paths_.resize(cp.node_debug_paths_size);
        tracks_.resize(cp.tracks_size);
        track_paths_.resize(cp.track_paths_size);
        track_runtime_.resize(cp.track_runtime_size);
        track_runtime_last_values_.resize(cp.track_runtime_last_values_size);
        scalar_getter_storage_.resize(cp.scalar_getter_storage_size);
        objects_.resize(cp.objects_size);
        object_debug_names_.resize(cp.object_debug_names_size);
        lazy_value_watches_.resize(cp.lazy_value_watches_size);
        all_track_ids_.resize(cp.all_track_ids_size);
        parallel_track_ids_.resize(cp.parallel_track_ids_size);
        main_thread_track_ids_.resize(cp.main_thread_track_ids_size);
        while (bound_dirty_hooks_.size() > cp.bound_dirty_hooks_size) {
            WaveDirtyHook* hook = bound_dirty_hooks_.back();
            if (hook && hook->tracer == this) hook->clear();
            bound_dirty_hooks_.pop_back();
        }
        const bool dirty_peek_changed =
            dirty_peek_groups_.size() != cp.dirty_peek_groups_size ||
            dirty_peek_ranges_.size() != cp.dirty_peek_ranges_size ||
            dirty_peek_leaves_.size() != cp.dirty_peek_leaves_size;
        while (dirty_peek_groups_.size() > cp.dirty_peek_groups_size) {
            dirty_peek_group_by_key_.erase(dirty_peek_groups_.back().key);
            dirty_peek_groups_.pop_back();
        }
        dirty_peek_ranges_.resize(cp.dirty_peek_ranges_size);
        dirty_peek_leaves_.resize(cp.dirty_peek_leaves_size);
        if (dirty_peek_changed) {
            rebuild_dirty_peek_group_range_links();
        }

        const bool dirty_wave_value_changed =
            dirty_wave_value_groups_.size() != cp.dirty_wave_value_groups_size ||
            dirty_wave_value_ranges_.size() != cp.dirty_wave_value_ranges_size ||
            dirty_wave_value_leaves_.size() != cp.dirty_wave_value_leaves_size ||
            dirty_wave_value_addr_table_.size() != cp.dirty_wave_value_addr_table_size;
        while (dirty_wave_value_groups_.size() > cp.dirty_wave_value_groups_size) {
            dirty_wave_value_group_by_key_.erase(dirty_wave_value_groups_.back().key);
            dirty_wave_value_groups_.pop_back();
        }
        dirty_wave_value_ranges_.resize(cp.dirty_wave_value_ranges_size);
        dirty_wave_value_leaves_.resize(cp.dirty_wave_value_leaves_size);
        dirty_wave_value_addr_table_.resize(cp.dirty_wave_value_addr_table_size);
        if (dirty_wave_value_changed) {
            invalidate_dirty_wave_value_addr_lookup();
            rebuild_dirty_wave_value_group_range_links();
        }
        global_dirty_group_ids_.resize(dirty_peek_groups_.size());
        global_dirty_wave_value_group_ids_.resize(dirty_wave_value_groups_.size());

        const bool dirty_wave_array_changed =
            dirty_wave_array_groups_.size() != cp.dirty_wave_array_groups_size ||
            dirty_wave_array_ranges_.size() != cp.dirty_wave_array_ranges_size ||
            dirty_wave_array_leaves_.size() != cp.dirty_wave_array_leaves_size ||
            dirty_wave_array_addr_table_.size() != cp.dirty_wave_array_addr_table_size;
        while (dirty_wave_array_groups_.size() > cp.dirty_wave_array_groups_size) {
            dirty_wave_array_group_by_key_.erase(dirty_wave_array_groups_.back().key);
            dirty_wave_array_groups_.pop_back();
        }
        dirty_wave_array_ranges_.resize(cp.dirty_wave_array_ranges_size);
        dirty_wave_array_leaves_.resize(cp.dirty_wave_array_leaves_size);
        dirty_wave_array_addr_table_.resize(cp.dirty_wave_array_addr_table_size);
        if (dirty_wave_array_changed) {
            dirty_wave_array_addr_table_sorted_ = false;
            rebuild_dirty_wave_array_group_range_links();
        }
        global_dirty_wave_array_group_ids_.resize(dirty_wave_array_groups_.size());
        if (dirty_peek_storage_by_key_.size() != cp.dirty_peek_storage_by_key_size ||
            tracks_.size() != cp.tracks_size || dirty_peek_changed) {
            rebuild_dirty_peek_storage_map_();
        }
        next_node_id_ = cp.next_node_id;
        next_track_id_ = cp.next_track_id;
        next_object_id_ = cp.next_object_id;
        next_watch_id_ = cp.next_watch_id;
    }



    struct LeafDistributionItem {
        NodeId node_id;
        std::uint32_t depth;
        std::uint64_t leaf_count;

        LeafDistributionItem() : node_id(0), depth(0), leaf_count(0) {}
    };

    NodeId track_parent_node_for_dump_(TrackId track_id) const noexcept {
        if (track_id == 0 || static_cast<std::size_t>(track_id) >= tracks_.size()) {
            return 0;
        }
        const TrackDesc& track = tracks_[static_cast<std::size_t>(track_id)];
        return track.node_id;
    }

    std::uint32_t node_depth_for_dump_(NodeId node_id) const noexcept {
        std::uint32_t depth = 0;
        std::size_t guard = 0;
        while (node_id != 0 &&
               static_cast<std::size_t>(node_id) < nodes_.size() &&
               guard < nodes_.size()) {
            const NodeDesc& node = nodes_[static_cast<std::size_t>(node_id)];
            const NodeId parent = node.parent_id;
            if (parent == 0 || parent == node_id) {
                return depth;
            }
            node_id = parent;
            ++depth;
            ++guard;
        }
        return depth;
    }

    std::string node_full_path_for_dump_(NodeId node_id) const {
        std::vector<std::string> parts;
        std::size_t guard = 0;
        while (node_id != 0 &&
               static_cast<std::size_t>(node_id) < nodes_.size() &&
               guard < nodes_.size()) {
            const NodeDesc& node = nodes_[static_cast<std::size_t>(node_id)];
            if (!node.alive) {
                break;
            }
            parts.push_back(node_name_ref(node));
            const NodeId parent = node.parent_id;
            if (parent == 0 || parent == node_id) {
                break;
            }
            node_id = parent;
            ++guard;
        }

        std::reverse(parts.begin(), parts.end());
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const std::string& part = parts[i];
            if (part.empty()) {
                continue;
            }
            if (out.empty()) {
                out += part;
            } else if (!part.empty() && part[0] == '[') {
                out += part;
            } else {
                out += ".";
                out += part;
            }
        }
        return out.empty() ? std::string("?") : out;
    }

    void accumulate_leaf_to_ancestors_for_dump_(NodeId node_id,
                                                std::vector<std::uint64_t>& counts) const noexcept {
        std::size_t guard = 0;
        while (node_id != 0 &&
               static_cast<std::size_t>(node_id) < nodes_.size() &&
               static_cast<std::size_t>(node_id) < counts.size() &&
               guard < nodes_.size()) {
            const NodeDesc& node = nodes_[static_cast<std::size_t>(node_id)];
            if (!node.alive) {
                break;
            }
            ++counts[static_cast<std::size_t>(node_id)];
            const NodeId parent = node.parent_id;
            if (parent == 0 || parent == node_id) {
                break;
            }
            node_id = parent;
            ++guard;
        }
    }

    std::vector<std::uint64_t> collect_flat_leaf_distribution_counts_() const {
        std::vector<std::uint64_t> counts(nodes_.size(), 0);
        for (std::size_t i = 0; i < flat_leaf_fast_table_.size(); ++i) {
            const FlatLeafFast& leaf = flat_leaf_fast_table_[i];
            const NodeId parent_node = track_parent_node_for_dump_(leaf.track_id);
            accumulate_leaf_to_ancestors_for_dump_(parent_node, counts);
        }
        return counts;
    }

    std::vector<std::uint64_t> collect_dirty_leaf_distribution_counts_(bool dirty_safe_only) const {
        std::vector<std::uint64_t> counts(nodes_.size(), 0);
        for (std::uint32_t group_id = 0;
             group_id < dirty_peek_groups_.size();
             ++group_id) {
            const DirtyPeekGroup& group = dirty_peek_groups_[group_id];
            if (dirty_safe_only && !group.dirty_safe) {
                continue;
            }

            std::size_t range_guard = 0;
            for (std::uint32_t range_id = group.first_range;
                 range_id != kInvalidIndex &&
                 range_id < dirty_peek_ranges_.size() &&
                 range_guard < dirty_peek_ranges_.size();
                 range_id = dirty_peek_ranges_[range_id].next_sibling) {
                const DirtyPeekRange& range = dirty_peek_ranges_[range_id];
                const std::uint32_t begin = range.leaf_begin;
                const std::uint32_t end = begin + range.leaf_count;
                for (std::uint32_t leaf_id = begin;
                     leaf_id < end && leaf_id < dirty_peek_leaves_.size();
                     ++leaf_id) {
                    const DirtyPeekLeaf& leaf = dirty_peek_leaves_[leaf_id];
                    const NodeId parent_node = track_parent_node_for_dump_(leaf.track_id);
                    accumulate_leaf_to_ancestors_for_dump_(parent_node, counts);
                }
                ++range_guard;
            }
        }
        return counts;
    }

    std::vector<std::uint64_t> collect_dirty_wave_array_leaf_distribution_counts_() const {
        std::vector<std::uint64_t> counts(nodes_.size(), 0);
        for (std::uint32_t group_id = 0;
             group_id < dirty_wave_array_groups_.size();
             ++group_id) {
            const DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];

            std::size_t range_guard = 0;
            for (std::uint32_t range_id = group.first_range;
                 range_id != kInvalidIndex &&
                 range_id < dirty_wave_array_ranges_.size() &&
                 range_guard < dirty_wave_array_ranges_.size();
                 range_id = dirty_wave_array_ranges_[range_id].next_sibling) {
                const DirtyWaveArrayRange& range = dirty_wave_array_ranges_[range_id];
                const std::uint32_t begin = range.leaf_begin;
                const std::uint32_t end = begin + range.leaf_count;
                for (std::uint32_t leaf_id = begin;
                     leaf_id < end && leaf_id < dirty_wave_array_leaves_.size();
                     ++leaf_id) {
                    const DirtyWaveArrayLeaf& leaf = dirty_wave_array_leaves_[leaf_id];
                    const NodeId parent_node = track_parent_node_for_dump_(leaf.track_id);
                    accumulate_leaf_to_ancestors_for_dump_(parent_node, counts);
                }
                ++range_guard;
            }
        }
        return counts;
    }

    std::uint32_t dirty_wave_array_group_range_count_for_dump_(std::uint32_t group_id) const {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) {
            return 0;
        }
        const DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
        std::uint32_t count = 0;
        std::size_t range_guard = 0;
        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex &&
             range_id < dirty_wave_array_ranges_.size() &&
             range_guard < dirty_wave_array_ranges_.size();
             range_id = dirty_wave_array_ranges_[range_id].next_sibling) {
            ++count;
            ++range_guard;
        }
        return count;
    }

    TrackId dirty_wave_array_group_first_track_for_dump_(std::uint32_t group_id) const {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) {
            return 0;
        }
        const DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
        std::size_t range_guard = 0;
        for (std::uint32_t range_id = group.first_range;
             range_id != kInvalidIndex &&
             range_id < dirty_wave_array_ranges_.size() &&
             range_guard < dirty_wave_array_ranges_.size();
             range_id = dirty_wave_array_ranges_[range_id].next_sibling) {
            const DirtyWaveArrayRange& range = dirty_wave_array_ranges_[range_id];
            if (range.leaf_count > 0 && range.leaf_begin < dirty_wave_array_leaves_.size()) {
                return dirty_wave_array_leaves_[range.leaf_begin].track_id;
            }
            ++range_guard;
        }
        return 0;
    }

    std::string dirty_wave_array_group_summary_for_dump_(std::uint32_t group_id) const {
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) {
            return std::string("wave_array_gid=invalid");
        }
        const DirtyWaveArrayGroup& g = dirty_wave_array_groups_[group_id];
        std::string out = std::string("wave_array_gid=") + detail::to_string_unsigned(group_id);
        out += " index=" + detail::to_string_unsigned(g.element_index);
        out += " ranges=" + detail::to_string_unsigned(dirty_wave_array_group_range_count_for_dump_(group_id));
        out += " leaves=" + detail::to_string_unsigned(g.total_leaf_count);
        out += " addr=" + detail::pointer_to_string(g.key.address);
        out += " type=" + detail::pointer_to_string(g.key.type_tag);
        out += " bytes=" + detail::to_string_unsigned(g.key.byte_width);
        return out;
    }


    std::string dirty_peek_group_summary_for_dump_(std::uint32_t group_id) const {
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) {
            return std::string("gid=invalid");
        }
        const DirtyPeekGroup& g = dirty_peek_groups_[group_id];
        std::string out = std::string("gid=") + detail::to_string_unsigned(group_id);
        out += " dirty_safe=";
        out += g.dirty_safe ? "true" : "false";
        out += " ranges=" + detail::to_string_unsigned(g.range_count);
        out += " hooked=" + detail::to_string_unsigned(g.hooked_range_count);
        out += " alias_safe=" + detail::to_string_unsigned(g.safe_alias_range_count);
        out += " leaves=" + detail::to_string_unsigned(g.total_leaf_count);
        out += " addr=" + detail::pointer_to_string(g.key.address);
        out += " type=" + detail::pointer_to_string(g.key.type_tag);
        out += " bytes=" + detail::to_string_unsigned(g.key.byte_width);
        return out;
    }

    std::string track_path_for_dump_(TrackId track_id) const {
        if (track_id == 0 || static_cast<std::size_t>(track_id) >= tracks_.size()) {
            return std::string("?");
        }
        const TrackDesc& track = tracks_[static_cast<std::size_t>(track_id)];
        return node_full_path_for_dump_(track.node_id);
    }

    std::string track_poll_classification_extra_for_dump_(const TrackDesc& track) const {
        std::string extra;
        if (track.dirty_peek_group_id != kInvalidIndex) {
            extra += dirty_peek_group_summary_for_dump_(track.dirty_peek_group_id);
        }
        if (track.dirty_wave_value_group_id != kInvalidIndex) {
            if (!extra.empty()) extra += " ";
            extra += "wave_value_gid=" + detail::to_string_unsigned(track.dirty_wave_value_group_id);
        }
        if (track.dirty_wave_array_group_id != kInvalidIndex) {
            if (!extra.empty()) extra += " ";
            extra += dirty_wave_array_group_summary_for_dump_(track.dirty_wave_array_group_id);
        }
        return extra;
    }

    void dump_dirty_wave_array_group_summary_(FILE* fp, std::uint32_t top_n) const {
        if (!fp) {
            return;
        }
        if (top_n == 0) {
            top_n = 50;
        }

        struct WaveArrayGroupDumpItem {
            std::uint32_t group_id;
            std::uint32_t leaf_count;
            std::uint32_t range_count;
            TrackId first_track;
        };

        std::vector<WaveArrayGroupDumpItem> items;
        items.reserve(dirty_wave_array_groups_.size());
        for (std::uint32_t group_id = 0; group_id < dirty_wave_array_groups_.size(); ++group_id) {
            const DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
            if (group.total_leaf_count == 0) {
                continue;
            }
            WaveArrayGroupDumpItem item;
            item.group_id = group_id;
            item.leaf_count = group.total_leaf_count;
            item.range_count = dirty_wave_array_group_range_count_for_dump_(group_id);
            item.first_track = dirty_wave_array_group_first_track_for_dump_(group_id);
            items.push_back(item);
        }

        std::sort(items.begin(), items.end(),
                  [](const WaveArrayGroupDumpItem& a, const WaveArrayGroupDumpItem& b) {
                      if (a.leaf_count != b.leaf_count) {
                          return a.leaf_count > b.leaf_count;
                      }
                      return a.group_id < b.group_id;
                  });

        const std::size_t limit = std::min<std::size_t>(items.size(), static_cast<std::size_t>(top_n));
        std::fprintf(fp, "\n========== DIRTY_WAVE_ARRAY_GROUPS_FIRST_%u ==========" "\n",
                     static_cast<unsigned>(top_n));
        std::fprintf(fp,
                     "note: each group corresponds to one wave::array element address; leaves are sampled only when that element is reported dirty.\n");
        std::fprintf(fp, "total_groups=%u groups_with_leaves=%u shown=%u total_ranges=%u total_leaves=%u\n",
                     static_cast<unsigned>(dirty_wave_array_groups_.size()),
                     static_cast<unsigned>(items.size()),
                     static_cast<unsigned>(limit),
                     static_cast<unsigned>(dirty_wave_array_ranges_.size()),
                     static_cast<unsigned>(dirty_wave_array_leaves_.size()));
        std::fprintf(fp, "rank,leaf_count,range_count,group_id,index,first_track,path,addr,type,bytes\n");

        for (std::size_t i = 0; i < limit; ++i) {
            const WaveArrayGroupDumpItem& item = items[i];
            const DirtyWaveArrayGroup& group = dirty_wave_array_groups_[item.group_id];
            const std::string path = item.first_track != 0 ? track_path_for_dump_(item.first_track) : std::string("?");
            std::fprintf(fp,
                         "%u,%u,%u,%u,%u,%u,%s,%s,%s,%u\n",
                         static_cast<unsigned>(i + 1),
                         static_cast<unsigned>(item.leaf_count),
                         static_cast<unsigned>(item.range_count),
                         static_cast<unsigned>(item.group_id),
                         static_cast<unsigned>(group.element_index),
                         static_cast<unsigned>(item.first_track),
                         path.c_str(),
                         detail::pointer_to_string(group.key.address).c_str(),
                         detail::pointer_to_string(group.key.type_tag).c_str(),
                         static_cast<unsigned>(group.key.byte_width));
        }
        std::fflush(fp);
    }

    void dump_track_poll_classification_(FILE* fp, std::uint32_t top_n) const {
        if (!fp) {
            return;
        }
        if (top_n == 0) {
            top_n = 50;
        }

        enum DumpClass {
            FlatNormal = 0,
            FlatPeekNoHook = 1,
            FlatPeekMixedHook = 2,
            FlatPeekOtherFallback = 3,
            FlatPeekDirtySafeBug = 4,
            FlatWaveValueBug = 5,
            FlatWaveArrayBug = 6,
            SuppressedDirtyPeekSafe = 7,
            SuppressedWaveValue = 8,
            SuppressedWaveArray = 9,
            DumpClassCount = 10
        };

        const char* names[DumpClassCount] = {
            "FLAT_POLL_NORMAL_LEAVES",
            "FLAT_POLL_PEEK_FALLBACK_NO_HOOK",
            "FLAT_POLL_PEEK_FALLBACK_MIXED_HOOK",
            "FLAT_POLL_PEEK_FALLBACK_OTHER",
            "BUG_FLAT_POLL_PEEK_DIRTY_SAFE",
            "BUG_FLAT_POLL_WAVEVALUE_DIRTY",
            "BUG_FLAT_POLL_WAVEARRAY_DIRTY",
            "SUPPRESSED_DIRTY_PEEK_SAFE",
            "SUPPRESSED_WAVEVALUE_DIRTY",
            "SUPPRESSED_WAVEARRAY_DIRTY"
        };

        std::uint64_t counts[DumpClassCount] = {};
        std::vector<std::string> examples[DumpClassCount];

        std::vector<unsigned char> track_in_flat;
        track_in_flat.resize(tracks_.size(), 0);

        for (std::size_t i = 0; i < flat_leaf_fast_table_.size(); ++i) {
            const FlatLeafFast& leaf = flat_leaf_fast_table_[i];
            if (leaf.track_id == 0 || static_cast<std::size_t>(leaf.track_id) >= tracks_.size()) {
                continue;
            }
            track_in_flat[static_cast<std::size_t>(leaf.track_id)] = 1;
            const TrackDesc& track = tracks_[static_cast<std::size_t>(leaf.track_id)];

            DumpClass cls = FlatNormal;
            if (track.dirty_peek_group_id != kInvalidIndex &&
                track.dirty_peek_group_id < dirty_peek_groups_.size()) {
                const DirtyPeekGroup& g = dirty_peek_groups_[track.dirty_peek_group_id];
                if (g.dirty_safe) {
                    cls = FlatPeekDirtySafeBug;
                } else if (g.hooked_range_count == 0) {
                    cls = FlatPeekNoHook;
                } else if (g.hooked_range_count < g.range_count) {
                    cls = FlatPeekMixedHook;
                } else {
                    cls = FlatPeekOtherFallback;
                }
            } else if (track.dirty_wave_value_group_id != kInvalidIndex &&
                       track.dirty_wave_value_group_id < dirty_wave_value_groups_.size()) {
                cls = FlatWaveValueBug;
            } else if (track.dirty_wave_array_group_id != kInvalidIndex &&
                       track.dirty_wave_array_group_id < dirty_wave_array_groups_.size()) {
                cls = FlatWaveArrayBug;
            }

            ++counts[cls];
            if (examples[cls].size() < static_cast<std::size_t>(top_n)) {
                std::string line = std::string("track=") + detail::to_string_unsigned(track.id);
                line += " node=" + detail::to_string_unsigned(track.node_id);
                line += " path=" + track_path_for_dump_(track.id);
                const std::string extra = track_poll_classification_extra_for_dump_(track);
                if (!extra.empty()) {
                    line += " ";
                    line += extra;
                }
                examples[cls].push_back(line);
            }
        }

        for (TrackId tid = 1; static_cast<std::size_t>(tid) < tracks_.size(); ++tid) {
            const TrackDesc& track = tracks_[static_cast<std::size_t>(tid)];
            if (track.id == 0 || track.scalar_kind == ScalarSampleKind::None) {
                continue;
            }

            DumpClass cls = DumpClassCount;
            if (is_dirty_peek_track_poll_suppressed(track)) {
                cls = SuppressedDirtyPeekSafe;
            } else if (is_dirty_wave_value_track_poll_suppressed(track)) {
                cls = SuppressedWaveValue;
            } else if (is_dirty_wave_array_track_poll_suppressed(track)) {
                cls = SuppressedWaveArray;
            } else {
                continue;
            }

            ++counts[cls];
            if (examples[cls].size() < static_cast<std::size_t>(top_n)) {
                std::string line = std::string("track=") + detail::to_string_unsigned(track.id);
                line += " node=" + detail::to_string_unsigned(track.node_id);
                line += " in_flat=";
                line += (static_cast<std::size_t>(tid) < track_in_flat.size() && track_in_flat[static_cast<std::size_t>(tid)]) ? "true" : "false";
                line += " path=" + track_path_for_dump_(track.id);
                const std::string extra = track_poll_classification_extra_for_dump_(track);
                if (!extra.empty()) {
                    line += " ";
                    line += extra;
                }
                examples[cls].push_back(line);
            }
        }

        std::fprintf(fp, "\n========== TRACK_POLL_CLASSIFICATION_FIRST_%u ==========" "\n",
                     static_cast<unsigned>(top_n));
        std::fprintf(fp,
                     "note: FLAT_POLL_PEEK_FALLBACK_* means peek leaves are still in the per-cycle poll table because the dirty group is not dirty-safe. BUG_* sections should normally be empty.\n");

        for (int cls = 0; cls < DumpClassCount; ++cls) {
            std::fprintf(fp, "\n[%s] total=%llu shown=%u\n",
                         names[cls],
                         static_cast<unsigned long long>(counts[cls]),
                         static_cast<unsigned>(examples[cls].size()));
            for (std::size_t i = 0; i < examples[cls].size(); ++i) {
                std::fprintf(fp, "%u,%s\n",
                             static_cast<unsigned>(i + 1),
                             examples[cls][i].c_str());
            }
        }
        std::fflush(fp);
    }

    void dump_leaf_distribution_part_(FILE* fp,
                                      const char* title,
                                      const std::vector<std::uint64_t>& counts,
                                      std::uint32_t top_n) const {
        if (!fp) {
            return;
        }

        std::uint32_t max_depth = 0;
        for (NodeId node_id = 1;
             static_cast<std::size_t>(node_id) < nodes_.size() &&
             static_cast<std::size_t>(node_id) < counts.size();
             ++node_id) {
            if (counts[static_cast<std::size_t>(node_id)] == 0) {
                continue;
            }
            const std::uint32_t depth = node_depth_for_dump_(node_id);
            if (depth > max_depth) {
                max_depth = depth;
            }
        }

        std::vector<std::vector<LeafDistributionItem> > levels(max_depth + 1);
        std::uint64_t root_sum = 0;
        std::uint32_t active_node_count = 0;

        for (NodeId node_id = 1;
             static_cast<std::size_t>(node_id) < nodes_.size() &&
             static_cast<std::size_t>(node_id) < counts.size();
             ++node_id) {
            const std::uint64_t count = counts[static_cast<std::size_t>(node_id)];
            if (count == 0) {
                continue;
            }
            const std::uint32_t depth = node_depth_for_dump_(node_id);
            LeafDistributionItem item;
            item.node_id = node_id;
            item.depth = depth;
            item.leaf_count = count;
            if (depth >= levels.size()) {
                levels.resize(depth + 1);
            }
            levels[depth].push_back(item);
            ++active_node_count;
            if (nodes_[static_cast<std::size_t>(node_id)].parent_id == 0) {
                root_sum += count;
            }
        }

        std::fprintf(fp, "\n========== %s ==========" "\n", title ? title : "UNKNOWN");
        std::fprintf(fp,
                     "total_leaf_count_at_roots=%llu active_nodes=%u max_depth=%u\n",
                     static_cast<unsigned long long>(root_sum),
                     static_cast<unsigned>(active_node_count),
                     static_cast<unsigned>(max_depth));

        for (std::uint32_t depth = 0; depth < levels.size(); ++depth) {
            std::vector<LeafDistributionItem>& items = levels[depth];
            if (items.empty()) {
                continue;
            }
            std::sort(items.begin(), items.end(),
                      [](const LeafDistributionItem& a, const LeafDistributionItem& b) {
                          if (a.leaf_count != b.leaf_count) {
                              return a.leaf_count > b.leaf_count;
                          }
                          return a.node_id < b.node_id;
                      });

            const std::uint32_t limit = static_cast<std::uint32_t>(
                std::min<std::size_t>(items.size(), static_cast<std::size_t>(top_n)));
            std::fprintf(fp,
                         "\n[depth=%u] node_count=%u top=%u\n",
                         static_cast<unsigned>(depth),
                         static_cast<unsigned>(items.size()),
                         static_cast<unsigned>(limit));
            std::fprintf(fp, "rank,leaf_count,node_id,path\n");

            for (std::uint32_t i = 0; i < limit; ++i) {
                const LeafDistributionItem& item = items[i];
                const std::string path = node_full_path_for_dump_(item.node_id);
                std::fprintf(fp,
                             "%u,%llu,%u,%s\n",
                             static_cast<unsigned>(i + 1),
                             static_cast<unsigned long long>(item.leaf_count),
                             static_cast<unsigned>(item.node_id),
                             path.c_str());
            }
        }
        std::fflush(fp);
    }

    void dump_leaf_distribution_by_depth_impl_(FILE* fp,
                                               std::uint32_t top_n,
                                               bool dirty_safe_only) const {
        if (!fp) {
            return;
        }
        if (top_n == 0) {
            top_n = 50;
        }

        std::fprintf(fp, "Reflection wave leaf distribution\n");
        std::fprintf(fp,
                     "nodes=%u tracks=%u flat_poll_leaves=%u dirty_peek_groups=%u dirty_peek_ranges=%u dirty_peek_leaves=%u dirty_wave_array_groups=%u dirty_wave_array_ranges=%u dirty_wave_array_leaves=%u\n",
                     static_cast<unsigned>(nodes_.size() > 0 ? nodes_.size() - 1 : 0),
                     static_cast<unsigned>(tracks_.size() > 0 ? tracks_.size() - 1 : 0),
                     static_cast<unsigned>(flat_leaf_fast_table_.size()),
                     static_cast<unsigned>(dirty_peek_groups_.size()),
                     static_cast<unsigned>(dirty_peek_ranges_.size()),
                     static_cast<unsigned>(dirty_peek_leaves_.size()),
                     static_cast<unsigned>(dirty_wave_array_groups_.size()),
                     static_cast<unsigned>(dirty_wave_array_ranges_.size()),
                     static_cast<unsigned>(dirty_wave_array_leaves_.size()));
        std::fprintf(fp,
                     "dirty_section=%s\n",
                     dirty_safe_only ? "dirty-safe active-report leaves only" : "all dirty-peek registered leaves");

        dump_leaf_distribution_part_(fp,
                                     "FLAT_POLL_LEAVES",
                                     collect_flat_leaf_distribution_counts_(),
                                     top_n);
        dump_leaf_distribution_part_(fp,
                                     dirty_safe_only ? "DIRTY_PEEK_LEAVES_DIRTY_SAFE_ONLY" : "DIRTY_PEEK_LEAVES_ALL_GROUPS",
                                     collect_dirty_leaf_distribution_counts_(dirty_safe_only),
                                     top_n);
        dump_leaf_distribution_part_(fp,
                                     "DIRTY_WAVE_ARRAY_LEAVES",
                                     collect_dirty_wave_array_leaf_distribution_counts_(),
                                     top_n);
        dump_dirty_wave_array_group_summary_(fp, top_n);
        dump_track_poll_classification_(fp, top_n);
    }

    void maybe_dump_leaf_distribution_after_topology() {
        if (leaf_distribution_dumped_) {
            return;
        }
        if (!options_.dump_leaf_distribution_after_topology) {
            return;
        }
        // Only dump after topology declarations have been exported and the flat
        // table has had a chance to suppress dirty-safe peek leaves.
        if (!nodes_exported_) {
            return;
        }
        if (options_.enable_flat_leaf_fast_table && !flat_leaf_fast_table_valid_) {
            return;
        }

        leaf_distribution_dumped_ = true;
        FILE* fp = NULL;
        if (!options_.leaf_distribution_dump_path.empty()) {
            fp = std::fopen(options_.leaf_distribution_dump_path.c_str(), "w");
        }
        if (!fp) {
            std::fprintf(stderr,
                         "[wave] failed to open leaf distribution dump path='%s'\n",
                         options_.leaf_distribution_dump_path.c_str());
            return;
        }
        dump_leaf_distribution_by_depth_impl_(fp,
                                              options_.leaf_distribution_top_n,
                                              options_.leaf_distribution_dirty_safe_only);
        std::fclose(fp);
        std::fprintf(stderr,
                     "[wave] leaf distribution dumped to %s\n",
                     options_.leaf_distribution_dump_path.c_str());
    }

    static const std::string& empty_node_string() {
        static const std::string empty;
        return empty;
    }

    const std::string& node_name_ref(const NodeDesc& node) const {
        const std::uint32_t id = node.name_id;
        if (id == kInvalidIndex || static_cast<std::size_t>(id) >= node_names_.size()) {
            return empty_node_string();
        }
        return node_names_[static_cast<std::size_t>(id)];
    }

    const std::string& node_debug_path_ref(const NodeDesc& node) const {
        const std::uint32_t id = node.debug_path_id;
        if (id == kInvalidIndex || static_cast<std::size_t>(id) >= node_debug_paths_.size()) {
            return empty_node_string();
        }
        return node_debug_paths_[static_cast<std::size_t>(id)];
    }

    const std::string& track_path_ref(const TrackDesc& track) const {
        const std::uint32_t id = track.path_id;
        if (id == kInvalidIndex || static_cast<std::size_t>(id) >= track_paths_.size()) {
            return empty_node_string();
        }
        return track_paths_[static_cast<std::size_t>(id)];
    }

    const std::string& runtime_last_value_ref(const TrackRuntimeState& state) const {
        const std::uint32_t id = state.last_value_id;
        if (id == kInvalidIndex || static_cast<std::size_t>(id) >= track_runtime_last_values_.size()) {
            return empty_node_string();
        }
        return track_runtime_last_values_[static_cast<std::size_t>(id)];
    }

    void clear_runtime_last_value(TrackRuntimeState& state) {
        state.last_value_id = kInvalidIndex;
    }

    void set_runtime_last_value(TrackRuntimeState& state, const std::string& value) {
        if (state.last_value_id == kInvalidIndex ||
            static_cast<std::size_t>(state.last_value_id) >= track_runtime_last_values_.size()) {
            state.last_value_id = static_cast<std::uint32_t>(track_runtime_last_values_.size());
            track_runtime_last_values_.push_back(value);
        } else {
            track_runtime_last_values_[static_cast<std::size_t>(state.last_value_id)] = value;
        }
    }

    bool node_has_signal_subtree(NodeId node_id) const {
        const NodeDesc* node = find_node(node_id);
        return node && node->alive && (node->first_track != 0 || node->first_child != 0);
    }

    NodeId keep_node_or_rollback(const TopologyCheckpoint& cp, NodeId node_id) {
        if (node_has_signal_subtree(node_id)) return node_id;
        if (options_.debug_log) {
            debug_log_msg(std::string("topology rollback empty node=") + detail::to_string_unsigned(node_id));
        }
        rollback_topology_to(cp);
        return 0;
    }

    void export_node_declarations_once() {
        if (nodes_exported_) return;
        std::size_t exported_count = 0;
        for (std::size_t i = 1; i < nodes_.size(); ++i) {
            const NodeDesc& node = nodes_[i];
            if (node.id == 0 || !node.alive) continue;
            if (!node_has_signal_subtree(node.id) && node.kind != NodeKind::Leaf) continue;
            NodeDecl decl;
            decl.node_id = node.id;
            decl.parent_id = node.parent_id;
            decl.name = node_name_ref(node);
            decl.kind = node.kind;
            sink_.on_node_declared(decl);
            ++exported_count;
        }

        // Do not permanently mark an empty pre-root prepare as exported.  That
        // state caused the exact failure where tracks/nodes were later built but
        // NodeDecl export was skipped, leaving the recorder with no topology at
        // open_writer_if_needed()/end_cycle().
        bool has_unexpanded_root = false;
        for (std::size_t i = 1; i < root_watches_.size(); ++i) {
            const RootWatch& w = root_watches_[i];
            if (w.id != 0 && w.alive && !w.expanded) {
                has_unexpanded_root = true;
                break;
            }
        }
        if (exported_count != 0 || !has_unexpanded_root) {
            nodes_exported_ = true;
        }
    }

    NodeId create_node(NodeId parent_id, const std::string& path, NodeKind kind, ObjectId object_id) {
        const NodeId id = next_node_id_++;
        NodeDesc& node = ensure_id_slot_reserved(nodes_, id, options_.id_slot_node_growth_chunk);
        node = NodeDesc();
        node.id = id;
        node.parent_id = parent_id;
        node.object_id = object_id;
        node.name_id = static_cast<std::uint32_t>(node_names_.size());
        node_names_.push_back(detail::path_last_segment(path));
        if (options_.debug_log) {
            node.debug_path_id = static_cast<std::uint32_t>(node_debug_paths_.size());
            node_debug_paths_.push_back(path);
        }
        node.kind = kind;
        node.alive = true;

        if (parent_id != 0) {
            NodeDesc* parent = find_node(parent_id);
            if (parent) {
                node.next_sibling = parent->first_child;
                parent->first_child = id;
            }
        }

        if (options_.debug_log) debug_log_msg(std::string("create_node id=") + detail::to_string_unsigned(id) +
                      " parent=" + detail::to_string_unsigned(parent_id) +
                      " kind=" + node_kind_name(kind) +
                      " object=" + detail::to_string_unsigned(object_id) +
                      " name=" + node_name_ref(node) +
                      " path=" + node_debug_path_ref(node));
        return id;
    }

    ObjectId ensure_object(const ObjectKey& key, const std::string& debug_name) {
        if (!key.address) return 0;
        ObjectId id = 0;
        std::unordered_map<ObjectKey, ObjectId, ObjectKeyHash>::iterator it = object_id_by_key_.find(key);
        if (it != object_id_by_key_.end()) {
            id = it->second;
            ObjectEntry* existing = id_lookup(objects_, id);
            if (existing && existing->alive) return existing->id;
        } else {
            id = next_object_id_++;
            object_id_by_key_[key] = id;
            object_created_keys_.push_back(key);
        }

        ObjectEntry& entry = ensure_id_slot_reserved(objects_, id, options_.id_slot_object_growth_chunk);
        entry.id = id;
        entry.key = key;
        entry.alive = true;
        if (options_.debug_log) {
            entry.debug_name_id = static_cast<std::uint32_t>(object_debug_names_.size());
            object_debug_names_.push_back(debug_name);
        } else {
            entry.debug_name_id = kInvalidIndex;
        }
        if (options_.debug_log) debug_log_msg(std::string("ensure_object id=") + detail::to_string_unsigned(entry.id) +
                      " addr=" + detail::pointer_to_string(key.address) +
                      " name=" + debug_name);
        return entry.id;
    }


    bool is_live_node(NodeId node_id) const {
        const NodeDesc* node = find_node(node_id);
        return node && node->alive;
    }








    void rebuild_dirty_peek_storage_map_() {
        dirty_peek_storage_by_key_.clear();
        dirty_peek_storage_by_key_.reserve(tracks_.size() > 1 ? tracks_.size() - 1 : 0);
        for (TrackId tid = 1; static_cast<std::size_t>(tid) < tracks_.size(); ++tid) {
            TrackDesc& track = tracks_[static_cast<std::size_t>(tid)];
            if (track.id == 0 || track.dirty_peek_group_id == kInvalidIndex || track.scalar_kind == ScalarSampleKind::None) continue;
            const void* memory = track.memory_ctx ? track.memory_ctx : track.sample_ctx;
            const std::uint32_t mem_bytes = track.memory_byte_width != 0
                ? track.memory_byte_width
                : static_cast<std::uint32_t>(scalar_sample_kind_byte_width(track.scalar_kind));
            if (memory == NULL || mem_bytes == 0) continue;
            const TrackId sid = track.storage_id != 0 ? track.storage_id : track.id;
            DirtyPeekStorageKey key(memory, track.scalar_reader, track.scalar_kind,
                                    track.kind, track.bit_width, mem_bytes);
            typename std::unordered_map<DirtyPeekStorageKey, TrackId, DirtyPeekStorageKeyHash>::const_iterator it =
                dirty_peek_storage_by_key_.find(key);
            if (it == dirty_peek_storage_by_key_.end() || sid < it->second) {
                dirty_peek_storage_by_key_[key] = sid;
            }
        }
    }

    void assign_track_storage_id_(TrackDesc& track) {
        if (track.id == 0) return;
        if (track.storage_id != 0) return;
        if (track.dirty_peek_group_id != kInvalidIndex && track.scalar_kind != ScalarSampleKind::None) {
            const void* memory = track.memory_ctx ? track.memory_ctx : track.sample_ctx;
            const std::uint32_t mem_bytes = track.memory_byte_width != 0
                ? track.memory_byte_width
                : static_cast<std::uint32_t>(scalar_sample_kind_byte_width(track.scalar_kind));
            if (memory != NULL && mem_bytes != 0) {
                DirtyPeekStorageKey key(memory, track.scalar_reader, track.scalar_kind,
                                        track.kind, track.bit_width, mem_bytes);
                typename std::unordered_map<DirtyPeekStorageKey, TrackId, DirtyPeekStorageKeyHash>::const_iterator it =
                    dirty_peek_storage_by_key_.find(key);
                if (it != dirty_peek_storage_by_key_.end() && it->second != 0) {
                    track.storage_id = it->second;
                    return;
                }
                dirty_peek_storage_by_key_[key] = track.id;
            }
        }
        track.storage_id = track.id;
    }

    TrackId finish_track_create(NodeId node_id,
                                const std::string& path,
                                ValueKind kind,
                                TrackDesc& track) {
        NodeDesc* node = find_node(node_id);
        if (node) {
            track.next_in_node = node->first_track;
            node->first_track = track.id;
        }

        assign_track_storage_id_(track);

        TrackDecl decl;
        decl.track_id = track.id;
        decl.storage_id = track.storage_id != 0 ? track.storage_id : track.id;
        decl.node_id = node_id;
        if (options_.emit_track_decl_path) {
            decl.path = path;
        }
        decl.kind = kind;
        decl.bit_width = track.bit_width != 0 ? track.bit_width : scalar_bit_width(track.scalar_kind);
        sink_.on_track_declared(decl);

        all_track_ids_.push_back(track.id);
        // High-performance mode: every track is scheduled through the parallel
        // track-id list. Scalar tracks take the scalar fast path; custom/string/
        // SystemC/VSIP/value-source tracks are sampled by worker threads as custom
        // samplers. main_thread_track_ids_ is kept only for source compatibility.
        parallel_track_ids_.push_back(track.id);
        invalidate_flat_leaf_fast_table();

        if (options_.debug_log || options_.emit_track_decl_path) {
            track.path_id = static_cast<std::uint32_t>(track_paths_.size());
            track_paths_.push_back(path);
        } else {
            track.path_id = kInvalidIndex;
        }
        if (options_.debug_log) {
            debug_log_msg(std::string("create_track id=") + detail::to_string_unsigned(track.id) +
                          " node=" + detail::to_string_unsigned(node_id) +
                          " kind=" + value_kind_name(kind) +
                          " path=" + path);
        }
        return track.id;
    }

    std::uint32_t get_or_create_dirty_peek_group(const void* address,
                                                const void* type_tag,
                                                std::uint32_t byte_width) {
        if (!address || !type_tag || !options_.enable_dirty_peek_groups) return kInvalidIndex;
        const DirtyPeekGroupKey key(address, type_tag, byte_width);
        std::unordered_map<DirtyPeekGroupKey, std::uint32_t, DirtyPeekGroupKeyHash>::const_iterator it = dirty_peek_group_by_key_.find(key);
        if (it != dirty_peek_group_by_key_.end()) return it->second;

        const std::uint32_t group_id = static_cast<std::uint32_t>(dirty_peek_groups_.size());
        DirtyPeekGroup group;
        group.key = key;
        dirty_peek_groups_.push_back(group);
        dirty_peek_group_by_key_[key] = group_id;
        dirty_peek_initial_sample_done_ = false;
        invalidate_dirty_peek_memory_blocks();
        global_dirty_group_ids_.resize(dirty_peek_groups_.size());
        return group_id;
    }

    void clear_bound_dirty_hooks() noexcept {
        for (std::size_t i = 0; i < bound_dirty_hooks_.size(); ++i) {
            WaveDirtyHook* hook = bound_dirty_hooks_[i];
            if (hook && hook->tracer == this) hook->clear();
        }
        bound_dirty_hooks_.clear();
    }

    void detach_all_tls_for_this_tracer_() noexcept {
        WaveTlsRegistry::instance().for_each_tls_locked([&](ThreadTraceLocal* tls) {
            if (tls) {
                tls->detach(this);
            }
        });
    }

    void bind_dirty_hook_to_group(WaveDirtyHook* hook, std::uint32_t group_id) {
        if (!hook || group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return;
        if (hook->tracer == this && hook->group_id == group_id) {
            return;
        }
        bound_dirty_hooks_.push_back(hook);
        hook->bind(this, group_id, &Tracer::wave_dirty_hook_mark_bridge_);
        dirty_peek_initial_sample_done_ = false;
    }

    void refresh_dirty_peek_group_safety(DirtyPeekGroup& group) noexcept {
        // A peek group is dirty-safe when at least one alias has a real write hook
        // and every recorded range is either hook-owned or a safe wrapper alias
        // of that hook-owned value source.  This avoids the overly conservative
        // channel+port case where sc_vector/vsip ports observe the same channel
        // address but the hook is exposed only by the channel/interface object.
        group.dirty_safe = (group.range_count != 0 &&
                            group.hooked_range_count != 0 &&
                            group.range_count == group.hooked_range_count + group.safe_alias_range_count);
    }

    void mark_dirty_peek_ranges_hooked(std::uint32_t group_id,
                                       std::uint32_t first_range_id,
                                       std::uint32_t end_range_id) {
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return;
        DirtyPeekGroup& group = dirty_peek_groups_[group_id];
        if (end_range_id > dirty_peek_ranges_.size()) end_range_id = static_cast<std::uint32_t>(dirty_peek_ranges_.size());
        for (std::uint32_t range_id = first_range_id; range_id < end_range_id; ++range_id) {
            DirtyPeekRange& range = dirty_peek_ranges_[range_id];
            if (range.group_id != group_id) continue;
            if (!range.has_hook) {
                range.has_hook = true;
                ++group.hooked_range_count;
            }
        }
        refresh_dirty_peek_group_safety(group);
        dirty_peek_initial_sample_done_ = false;
    }

    void mark_dirty_peek_ranges_safe_alias(std::uint32_t group_id,
                                           std::uint32_t first_range_id,
                                           std::uint32_t end_range_id) {
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return;
        DirtyPeekGroup& group = dirty_peek_groups_[group_id];
        if (end_range_id > dirty_peek_ranges_.size()) {
            end_range_id = static_cast<std::uint32_t>(dirty_peek_ranges_.size());
        }
        for (std::uint32_t range_id = first_range_id; range_id < end_range_id; ++range_id) {
            DirtyPeekRange& range = dirty_peek_ranges_[range_id];
            if (range.group_id != group_id) continue;
            if (!range.has_hook && !range.safe_alias_without_own_hook) {
                range.safe_alias_without_own_hook = true;
                ++group.safe_alias_range_count;
            }
        }
        refresh_dirty_peek_group_safety(group);
        dirty_peek_initial_sample_done_ = false;
    }

    void rebuild_dirty_peek_group_range_links() {
        for (std::size_t i = 0; i < dirty_peek_groups_.size(); ++i) {
            dirty_peek_groups_[i].first_range = kInvalidIndex;
            dirty_peek_groups_[i].total_leaf_count = 0;
            dirty_peek_groups_[i].range_count = 0;
            dirty_peek_groups_[i].hooked_range_count = 0;
            dirty_peek_groups_[i].safe_alias_range_count = 0;
            dirty_peek_groups_[i].dirty_safe = false;
        }
        for (std::uint32_t range_id = 0; range_id < dirty_peek_ranges_.size(); ++range_id) {
            DirtyPeekRange& range = dirty_peek_ranges_[range_id];
            const std::uint32_t group_id = range.group_id;
            if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) {
                range.next_sibling = kInvalidIndex;
                continue;
            }
            range.next_sibling = dirty_peek_groups_[group_id].first_range;
            dirty_peek_groups_[group_id].first_range = range_id;
            dirty_peek_groups_[group_id].total_leaf_count += range.leaf_count;
            ++dirty_peek_groups_[group_id].range_count;
            if (range.has_hook) {
                ++dirty_peek_groups_[group_id].hooked_range_count;
            } else if (range.safe_alias_without_own_hook) {
                ++dirty_peek_groups_[group_id].safe_alias_range_count;
            }
            refresh_dirty_peek_group_safety(dirty_peek_groups_[group_id]);
        }
        invalidate_dirty_peek_memory_blocks();
    }

    void rebuild_dirty_wave_value_group_range_links() {
        for (std::size_t i = 0; i < dirty_wave_value_groups_.size(); ++i) {
            dirty_wave_value_groups_[i].first_range = kInvalidIndex;
            dirty_wave_value_groups_[i].total_leaf_count = 0;
        }
        for (std::uint32_t range_id = 0; range_id < dirty_wave_value_ranges_.size(); ++range_id) {
            DirtyWaveValueRange& range = dirty_wave_value_ranges_[range_id];
            const std::uint32_t group_id = range.group_id;
            if (group_id == kInvalidIndex || group_id >= dirty_wave_value_groups_.size()) {
                range.next_sibling = kInvalidIndex;
                continue;
            }
            range.next_sibling = dirty_wave_value_groups_[group_id].first_range;
            dirty_wave_value_groups_[group_id].first_range = range_id;
            dirty_wave_value_groups_[group_id].total_leaf_count += range.leaf_count;
        }
        invalidate_dirty_wave_value_addr_lookup();
    }

    void record_dirty_peek_leaf_for_track(TrackId track_id) {
        if (!options_.enable_dirty_peek_groups) return;
        if (track_id == 0 || track_id >= tracks_.size()) return;
        TrackDesc& track = tracks_[track_id];
        const std::uint32_t group_id = track.dirty_peek_group_id;
        if (group_id == kInvalidIndex || group_id >= dirty_peek_groups_.size()) return;
        if (track.scalar_kind == ScalarSampleKind::None) return;

        DirtyPeekLeaf leaf;
        leaf.track_id = track.id;
        leaf.storage_id = track.storage_id != 0 ? track.storage_id : track.id;
        leaf.sample_ctx = track.sample_ctx;
        leaf.memory_ctx = track.memory_ctx;
        leaf.memory_byte_width = track.memory_byte_width;
        leaf.scalar_reader = track.scalar_reader;
        leaf.scalar_kind = track.scalar_kind;
        leaf.value_kind = track.kind;
        leaf.has_last = false;
        leaf.last_bits = 0;

        const std::uint32_t leaf_index = static_cast<std::uint32_t>(dirty_peek_leaves_.size());
        dirty_peek_leaves_.push_back(leaf);
        invalidate_dirty_peek_memory_blocks();

        DirtyPeekGroup& group = dirty_peek_groups_[group_id];
        if (active_dirty_peek_range_id_ != kInvalidIndex &&
            active_dirty_peek_range_id_ < dirty_peek_ranges_.size()) {
            DirtyPeekRange& active_range = dirty_peek_ranges_[active_dirty_peek_range_id_];
            if (active_range.group_id == group_id &&
                active_range.leaf_begin + active_range.leaf_count == leaf_index) {
                ++active_range.leaf_count;
                ++group.total_leaf_count;
                return;
            }
        }

        DirtyPeekRange range;
        range.group_id = group_id;
        range.leaf_begin = leaf_index;
        range.leaf_count = 1;
        range.next_sibling = group.first_range;

        const std::uint32_t range_id = static_cast<std::uint32_t>(dirty_peek_ranges_.size());
        dirty_peek_ranges_.push_back(range);
        group.first_range = range_id;
        active_dirty_peek_range_id_ = range_id;
        ++group.total_leaf_count;
        ++group.range_count;
        refresh_dirty_peek_group_safety(group);
    }

    std::uint32_t get_or_create_dirty_wave_value_group(const void* address,
                                                       const void* type_tag,
                                                       std::uint32_t byte_width) {
        if (!options_.enable_wave_value_dirty || !address || !type_tag || byte_width == 0) return kInvalidIndex;
        const DirtyWaveValueGroupKey key(address, type_tag, byte_width);
        std::unordered_map<DirtyWaveValueGroupKey, std::uint32_t, DirtyWaveValueGroupKeyHash>::const_iterator it =
            dirty_wave_value_group_by_key_.find(key);
        if (it != dirty_wave_value_group_by_key_.end()) return it->second;

        DirtyWaveValueGroup group;
        group.key = key;
        const std::uint32_t group_id = static_cast<std::uint32_t>(dirty_wave_value_groups_.size());
        dirty_wave_value_groups_.push_back(group);
        dirty_wave_value_group_by_key_.insert(std::make_pair(key, group_id));
        dirty_wave_value_addr_table_.push_back(DirtyWaveValueAddressEntry(address, group_id, byte_width));
        invalidate_dirty_wave_value_addr_lookup();
        dirty_wave_value_initial_sample_done_ = false;
        return group_id;
    }

    void record_dirty_wave_value_leaf_for_track(TrackId track_id) {
        if (!options_.enable_wave_value_dirty) return;
        if (track_id == 0 || track_id >= tracks_.size()) return;
        TrackDesc& track = tracks_[track_id];
        const std::uint32_t group_id = track.dirty_wave_value_group_id;
        if (group_id == kInvalidIndex || group_id >= dirty_wave_value_groups_.size()) return;
        if (track.scalar_kind == ScalarSampleKind::None) return;

        DirtyWaveValueLeaf leaf;
        leaf.track_id = track.id;
        leaf.storage_id = track.storage_id != 0 ? track.storage_id : track.id;
        leaf.sample_ctx = track.sample_ctx;
        leaf.memory_ctx = track.memory_ctx;
        leaf.memory_byte_width = track.memory_byte_width;
        leaf.scalar_reader = track.scalar_reader;
        leaf.scalar_kind = track.scalar_kind;
        leaf.value_kind = track.kind;
        leaf.has_last = false;
        leaf.last_bits = 0;

        const std::uint32_t leaf_index = static_cast<std::uint32_t>(dirty_wave_value_leaves_.size());
        dirty_wave_value_leaves_.push_back(leaf);

        DirtyWaveValueGroup& group = dirty_wave_value_groups_[group_id];
        if (active_dirty_wave_value_range_id_ != kInvalidIndex &&
            active_dirty_wave_value_range_id_ < dirty_wave_value_ranges_.size()) {
            DirtyWaveValueRange& active_range = dirty_wave_value_ranges_[active_dirty_wave_value_range_id_];
            if (active_range.group_id == group_id &&
                active_range.leaf_begin + active_range.leaf_count == leaf_index) {
                ++active_range.leaf_count;
                ++group.total_leaf_count;
                return;
            }
        }

        DirtyWaveValueRange range;
        range.group_id = group_id;
        range.leaf_begin = leaf_index;
        range.leaf_count = 1;
        range.next_sibling = group.first_range;

        const std::uint32_t range_id = static_cast<std::uint32_t>(dirty_wave_value_ranges_.size());
        dirty_wave_value_ranges_.push_back(range);
        group.first_range = range_id;
        active_dirty_wave_value_range_id_ = range_id;
        ++group.total_leaf_count;
    }

    void rebuild_dirty_wave_array_group_range_links() {
        for (std::size_t i = 0; i < dirty_wave_array_groups_.size(); ++i) {
            dirty_wave_array_groups_[i].first_range = kInvalidIndex;
            dirty_wave_array_groups_[i].total_leaf_count = 0;
        }
        for (std::uint32_t range_id = 0; range_id < dirty_wave_array_ranges_.size(); ++range_id) {
            DirtyWaveArrayRange& range = dirty_wave_array_ranges_[range_id];
            const std::uint32_t group_id = range.group_id;
            if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) {
                range.next_sibling = kInvalidIndex;
                continue;
            }
            range.next_sibling = dirty_wave_array_groups_[group_id].first_range;
            dirty_wave_array_groups_[group_id].first_range = range_id;
            dirty_wave_array_groups_[group_id].total_leaf_count += range.leaf_count;
        }
        dirty_wave_array_addr_table_sorted_ = false;
        invalidate_dirty_wave_array_memory_blocks();
    }

    std::uint32_t get_or_create_dirty_wave_array_group(const void* address,
                                                       const void* type_tag,
                                                       std::uint32_t byte_width,
                                                       std::uint32_t element_index) {
        if (!options_.enable_wave_array_dirty || !address || !type_tag || byte_width == 0) return kInvalidIndex;
        const DirtyWaveArrayGroupKey key(address, type_tag, byte_width);
        std::unordered_map<DirtyWaveArrayGroupKey, std::uint32_t, DirtyWaveArrayGroupKeyHash>::const_iterator it =
            dirty_wave_array_group_by_key_.find(key);
        if (it != dirty_wave_array_group_by_key_.end()) return it->second;

        DirtyWaveArrayGroup group;
        group.key = key;
        group.element_index = element_index;
        const std::uint32_t group_id = static_cast<std::uint32_t>(dirty_wave_array_groups_.size());
        dirty_wave_array_groups_.push_back(group);
        dirty_wave_array_group_by_key_.insert(std::make_pair(key, group_id));
        dirty_wave_array_addr_table_.push_back(DirtyWaveArrayAddressEntry(address, type_tag, byte_width, group_id));
        dirty_wave_array_addr_table_sorted_ = false;
        dirty_wave_array_initial_sample_done_ = false;
        invalidate_dirty_wave_array_memory_blocks();
        return group_id;
    }

    void record_dirty_wave_array_leaf_for_track(TrackId track_id) {
        if (!options_.enable_wave_array_dirty) return;
        if (track_id == 0 || track_id >= tracks_.size()) return;
        TrackDesc& track = tracks_[track_id];
        const std::uint32_t group_id = track.dirty_wave_array_group_id;
        if (group_id == kInvalidIndex || group_id >= dirty_wave_array_groups_.size()) return;
        if (track.scalar_kind == ScalarSampleKind::None) return;

        DirtyWaveArrayLeaf leaf;
        leaf.track_id = track.id;
        leaf.storage_id = track.storage_id != 0 ? track.storage_id : track.id;
        leaf.sample_ctx = track.sample_ctx;
        leaf.memory_ctx = track.memory_ctx;
        leaf.memory_byte_width = track.memory_byte_width;
        leaf.scalar_reader = track.scalar_reader;
        leaf.scalar_kind = track.scalar_kind;
        leaf.value_kind = track.kind;
        leaf.has_last = false;
        leaf.last_bits = 0;

        const std::uint32_t leaf_index = static_cast<std::uint32_t>(dirty_wave_array_leaves_.size());
        dirty_wave_array_leaves_.push_back(leaf);
        invalidate_dirty_wave_array_memory_blocks();

        DirtyWaveArrayGroup& group = dirty_wave_array_groups_[group_id];
        if (active_dirty_wave_array_range_id_ != kInvalidIndex &&
            active_dirty_wave_array_range_id_ < dirty_wave_array_ranges_.size()) {
            DirtyWaveArrayRange& active_range = dirty_wave_array_ranges_[active_dirty_wave_array_range_id_];
            if (active_range.group_id == group_id &&
                active_range.leaf_begin + active_range.leaf_count == leaf_index) {
                ++active_range.leaf_count;
                ++group.total_leaf_count;
                return;
            }
        }

        DirtyWaveArrayRange range;
        range.group_id = group_id;
        range.leaf_begin = leaf_index;
        range.leaf_count = 1;
        range.next_sibling = group.first_range;

        const std::uint32_t range_id = static_cast<std::uint32_t>(dirty_wave_array_ranges_.size());
        dirty_wave_array_ranges_.push_back(range);
        group.first_range = range_id;
        active_dirty_wave_array_range_id_ = range_id;
        ++group.total_leaf_count;
    }

    template <typename T>
    TrackId create_wave_value_track(NodeId node_id,
                                    const std::string& path,
                                    const WaveValue<T>* wrapper) {
        if (!wrapper) return 0;
        const ScalarSampleKind sk = scalar_kind_for_type<T>();
        if (sk == ScalarSampleKind::None) return 0;
        const std::uint32_t group_id = get_or_create_dirty_wave_value_group(
            static_cast<const void*>(wrapper),
            reflect::type_tag_of<WaveValue<T> >(),
            static_cast<std::uint32_t>(sizeof(WaveValue<T>)));
        if (group_id == kInvalidIndex) {
            return create_scalar_ptr_track<T>(node_id, path, detail::classify_value_kind<T>(), std::addressof(wrapper->read()));
        }

        const TrackId id = next_track_id_++;
        TrackDesc& track = ensure_id_slot_reserved(tracks_, id, options_.id_slot_track_growth_chunk);
        track = TrackDesc();
        track.id = id;
        track.node_id = node_id;
        track.kind = detail::classify_value_kind<T>();
        TrackRuntimeState& runtime = ensure_id_slot_reserved(track_runtime_, id, options_.id_slot_track_growth_chunk);
        runtime = TrackRuntimeState();
        track.sample_ctx = static_cast<const void*>(std::addressof(wrapper->read()));
        track.memory_ctx = track.sample_ctx;
        track.memory_byte_width = static_cast<std::uint32_t>(sizeof(T));
        track.scalar_kind = sk;
        track.bit_width = scalar_bit_width(sk);
        track.thread_class = TrackThreadClass::ParallelSafe;
        track.dirty_wave_value_group_id = group_id;
        if (active_dirty_wave_array_group_id_ != kInvalidIndex) {
            track.dirty_wave_array_group_id = active_dirty_wave_array_group_id_;
        }

        const TrackId created = finish_track_create(node_id, path, track.kind, track);
        record_dirty_wave_value_leaf_for_track(created);
        record_dirty_wave_array_leaf_for_track(created);
        return created;
    }

    template <typename T>
    static bool read_bool_storage_ptr_storage(const void* ptr, ScalarSnapshot& sample) {
        typedef typename detail::remove_cvref<T>::type RawT;
        const RawT* p = static_cast<const RawT*>(ptr);
        if (!p) return false;
        const bool v = (*p != static_cast<RawT>(0));
        sample.sample_kind = ScalarSampleKind::Bool;
        sample.bool_value = v;
        sample.u64_value = v ? 1u : 0u;
        sample.i64_value = static_cast<std::int64_t>(sample.u64_value);
        sample.bits = sample.u64_value;
        return true;
    }

    template <typename T>
    TrackId create_bool_storage_ptr_track(NodeId node_id,
                                          const std::string& path,
                                          ::wave::BoolStoragePtr<T> ptr) {
        typedef typename detail::remove_cvref<T>::type RawT;
        if (ptr.ptr == NULL) return 0;
        const TrackId id = next_track_id_++;
        TrackDesc& track = ensure_id_slot_reserved(tracks_, id, options_.id_slot_track_growth_chunk);
        track = TrackDesc();
        track.id = id;
        track.node_id = node_id;
        track.kind = ValueKind::Bool;
        TrackRuntimeState& runtime = ensure_id_slot_reserved(track_runtime_, id, options_.id_slot_track_growth_chunk);
        runtime = TrackRuntimeState();
        track.sample_ctx = static_cast<const void*>(ptr.ptr);
        track.memory_ctx = static_cast<const void*>(ptr.ptr);
        track.memory_byte_width = static_cast<std::uint32_t>(sizeof(RawT));
        track.scalar_reader = &read_bool_storage_ptr_storage<RawT>;
        track.scalar_kind = ScalarSampleKind::Bool;
        track.bit_width = 1u;
        track.thread_class = TrackThreadClass::ParallelSafe;
        if (active_dirty_peek_group_id_ != kInvalidIndex) {
            track.dirty_peek_group_id = active_dirty_peek_group_id_;
        }
        if (active_dirty_wave_array_group_id_ != kInvalidIndex) {
            track.dirty_wave_array_group_id = active_dirty_wave_array_group_id_;
        }
        const TrackId created = finish_track_create(node_id, path, ValueKind::Bool, track);
        record_dirty_peek_leaf_for_track(created);
        record_dirty_wave_array_leaf_for_track(created);
        return created;
    }

    template <typename T>
    TrackId create_scalar_ptr_track(NodeId node_id,
                                    const std::string& path,
                                    ValueKind kind,
                                    const T* ptr) {
        const ScalarSampleKind sk = scalar_kind_for_type<T>();
        if (sk == ScalarSampleKind::None || ptr == NULL) {
            return 0;
        }
        const TrackId id = next_track_id_++;
        TrackDesc& track = ensure_id_slot_reserved(tracks_, id, options_.id_slot_track_growth_chunk);
        track = TrackDesc();
        track.id = id;
        track.node_id = node_id;
        track.kind = kind;
        TrackRuntimeState& runtime = ensure_id_slot_reserved(track_runtime_, id, options_.id_slot_track_growth_chunk);
        runtime = TrackRuntimeState();
        track.sample_ctx = static_cast<const void*>(ptr);
        track.memory_ctx = static_cast<const void*>(ptr);
        track.memory_byte_width = static_cast<std::uint32_t>(sizeof(T));
        track.scalar_kind = sk;
        track.bit_width = scalar_bit_width(sk);
        track.thread_class = TrackThreadClass::ParallelSafe;
        if (active_dirty_peek_group_id_ != kInvalidIndex) {
            track.dirty_peek_group_id = active_dirty_peek_group_id_;
        }
        if (active_dirty_wave_array_group_id_ != kInvalidIndex) {
            track.dirty_wave_array_group_id = active_dirty_wave_array_group_id_;
        }
        const TrackId created = finish_track_create(node_id, path, kind, track);
        record_dirty_peek_leaf_for_track(created);
        record_dirty_wave_array_leaf_for_track(created);
        return created;
    }

    template <typename T>
    static bool read_scalar_volatile_ptr_storage(const void* ptr, ScalarSnapshot& sample) {
        const volatile T* vp = static_cast<const volatile T*>(ptr);
        if (!vp) return false;
        const T value = static_cast<T>(*vp);
        return fill_scalar_snapshot_from_value<T>(value, sample);
    }

    template <typename T>
    TrackId create_scalar_volatile_ptr_track(NodeId node_id,
                                             const std::string& path,
                                             ValueKind kind,
                                             const volatile T* ptr) {
        const ScalarSampleKind sk = scalar_kind_for_type<T>();
        if (sk == ScalarSampleKind::None || ptr == NULL) {
            return 0;
        }
        const TrackId id = next_track_id_++;
        TrackDesc& track = ensure_id_slot_reserved(tracks_, id, options_.id_slot_track_growth_chunk);
        track = TrackDesc();
        track.id = id;
        track.node_id = node_id;
        track.kind = kind;
        TrackRuntimeState& runtime = ensure_id_slot_reserved(track_runtime_, id, options_.id_slot_track_growth_chunk);
        runtime = TrackRuntimeState();
        // sample_ctx is reinterpreted by scalar_reader as a volatile pointer.
        track.sample_ctx = const_cast<const void*>(static_cast<const volatile void*>(ptr));
        track.scalar_reader = &read_scalar_volatile_ptr_storage<T>;
        track.scalar_kind = sk;
        track.bit_width = scalar_bit_width(sk);
        track.thread_class = TrackThreadClass::ParallelSafe;
        if (active_dirty_peek_group_id_ != kInvalidIndex) {
            track.dirty_peek_group_id = active_dirty_peek_group_id_;
        }
        if (active_dirty_wave_array_group_id_ != kInvalidIndex) {
            track.dirty_wave_array_group_id = active_dirty_wave_array_group_id_;
        }
        const TrackId created = finish_track_create(node_id, path, kind, track);
        record_dirty_peek_leaf_for_track(created);
        record_dirty_wave_array_leaf_for_track(created);
        return created;
    }

    void emit_immediate_invalid(Cycle cycle, TrackId track_id, InvalidKind invalid) {
        const TrackDesc* track = find_track(track_id);
        if (!track || track->id == 0) return;
        TrackEvent ev;
        ev.cycle = cycle;
        ev.track_id = track_id;
        ev.path = track_path_ref(*track).c_str();
        ev.value = NULL;
        ev.invalid = invalid;
        if (options_.debug_log) debug_log_msg(std::string("emit_invalid cycle=") + detail::to_string_unsigned(cycle) +
                      " track=" + detail::to_string_unsigned(track_id) +
                      " invalid=" + invalid_kind_name(invalid) +
                      " path=" + track_path_ref(*track));
        sink_.on_sample(ev);
    }

    void invalidate_node_recursive(NodeId node_id, Cycle cycle, InvalidKind invalid) {
        NodeDesc* node_ptr = find_node(node_id);
        if (!node_ptr) return;
        NodeDesc& node = *node_ptr;
        if (!node.alive) return;
        node.alive = false;
        if (options_.debug_log) debug_log_msg(std::string("invalidate_node cycle=") + detail::to_string_unsigned(cycle) +
                      " node=" + detail::to_string_unsigned(node_id) +
                      " invalid=" + invalid_kind_name(invalid) +
                      " path=" + node_debug_path_ref(node));

        for (TrackId track_id = node.first_track; track_id != 0; ) {
            TrackDesc* track = find_track(track_id);
            const TrackId next_track = track ? track->next_in_node : 0;
            TrackRuntimeState* runtime = track ? find_track_runtime(track->id) : NULL;
            if (track && runtime && runtime->alive) {
                runtime->alive = false;
                emit_immediate_invalid(cycle, track->id, invalid);
            }
            track_id = next_track;
        }
        for (NodeId child_id = node.first_child; child_id != 0; ) {
            NodeDesc* child = find_node(child_id);
            const NodeId next_child = child ? child->next_sibling : 0;
            invalidate_node_recursive(child_id, cycle, invalid);
            child_id = next_child;
        }
    }




    void reserve_for_root_expansion() {
        if (options_.root_expand_reserve_nodes != 0) {
            nodes_.reserve(nodes_.size() + options_.root_expand_reserve_nodes);
        }
        if (options_.root_expand_reserve_tracks != 0) {
            tracks_.reserve(tracks_.size() + options_.root_expand_reserve_tracks);
            track_runtime_.reserve(track_runtime_.size() + options_.root_expand_reserve_tracks);
            scalar_getter_storage_.reserve(scalar_getter_storage_.size() + options_.root_expand_reserve_tracks);
        }
        if (options_.root_expand_reserve_objects != 0) {
            objects_.reserve(objects_.size() + options_.root_expand_reserve_objects);
            object_id_by_key_.reserve(object_id_by_key_.size() + options_.root_expand_reserve_objects);
        }
        if (options_.root_expand_reserve_lazy_watches != 0) {
            lazy_value_watches_.reserve(lazy_value_watches_.size() + options_.root_expand_reserve_lazy_watches);
        }
    }

    void refresh_root_watches(Cycle cycle) {
        const std::size_t watch_count = root_watches_.size();
        for (std::size_t i = 1; i < watch_count; ++i) {
            RootWatch& watch = root_watches_[i];
            if (watch.id == 0) continue;
            if (!watch.alive || watch.expanded) continue;
            if (options_.debug_log) debug_log_msg(std::string("root expand begin cycle=") + detail::to_string_unsigned(cycle) +
                          " id=" + detail::to_string_unsigned(watch.id) +
                          " path=" + watch.path +
                          " addr=" + detail::pointer_to_string(watch.address));

            const std::size_t nodes_before = nodes_.size();
            const std::size_t tracks_before = tracks_.size();
            const std::size_t objects_before = objects_.size();
            const std::size_t lazy_watches_before = lazy_value_watches_.size();

            reserve_for_root_expansion();
            const NodeId node_id = watch.expand_root ? watch.expand_root() : 0;
            watch.root_node_id = node_id;
            // Do not mark an empty/failed root expansion as permanently expanded.
            // Otherwise a first lazy sample taken before the model is ready, or a
            // transient reflection failure, can freeze an empty topology and later
            // open a clock-only writer even though add_root() was called.
            watch.expanded = (node_id != 0);

            if (options_.debug_log_root_expand_stats || options_.debug_log) {
                debug_log_msg(std::string("root expand stats cycle=") + detail::to_string_unsigned(cycle) +
                              " id=" + detail::to_string_unsigned(watch.id) +
                              " path=" + watch.path +
                              " nodes_added=" + detail::to_string_unsigned(nodes_.size() - nodes_before) +
                              " tracks_added=" + detail::to_string_unsigned(tracks_.size() - tracks_before) +
                              " objects_added=" + detail::to_string_unsigned(objects_.size() - objects_before) +
                              " lazy_watches_added=" + detail::to_string_unsigned(lazy_value_watches_.size() - lazy_watches_before));
            }
            if (options_.debug_log) debug_log_msg(std::string("root expand end cycle=") + detail::to_string_unsigned(cycle) +
                          " id=" + detail::to_string_unsigned(watch.id) +
                          " path=" + watch.path +
                          " node=" + detail::to_string_unsigned(node_id));
        }
    }

    void refresh_lazy_value_watches(Cycle cycle) {
        for (std::size_t i = 1; i < lazy_value_watches_.size(); ++i) {
            LazyValueWatch& watch = lazy_value_watches_[i];
            if (watch.id == 0) continue;
            if (!watch.alive) continue;

            if (watch.parent_id != 0) {
                const NodeDesc* parent = find_node(watch.parent_id);
                if (!parent || !parent->alive) {
                    if (watch.target_subtree_root_id != 0) {
                        invalidate_node_recursive(watch.target_subtree_root_id, cycle, InvalidKind::Z);
                        watch.target_subtree_root_id = 0;
                    }
                    if (options_.debug_log) debug_log_msg(std::string("lazy parent-dead cycle=") + detail::to_string_unsigned(cycle) +
                                  " watch=" + detail::to_string_unsigned(watch.id) +
                                  " path=" + watch.path);
                    watch.alive = false;
                    continue;
                }
            }

            const void* current = watch.read_value_address ? watch.read_value_address() : static_cast<const void*>(NULL);
            if (current == watch.last_value_address) {
                continue;
            }
            if (options_.debug_log) debug_log_msg(std::string("lazy change cycle=") + detail::to_string_unsigned(cycle) +
                          " watch=" + detail::to_string_unsigned(watch.id) +
                          " path=" + watch.path +
                          " current=" + detail::pointer_to_string(current) +
                          " last=" + detail::pointer_to_string(watch.last_value_address));

            if (watch.target_subtree_root_id != 0) {
                invalidate_node_recursive(watch.target_subtree_root_id, cycle, InvalidKind::Z);
                watch.target_subtree_root_id = 0;
            }
            watch.last_value_address = current;
            if (!current) {
                if (options_.debug_log) debug_log_msg(std::string("lazy null cycle=") + detail::to_string_unsigned(cycle) +
                              " watch=" + detail::to_string_unsigned(watch.id) +
                              " path=" + watch.path);
                continue;
            }

            std::function<NodeId(const void*)> expander = watch.expand_value;
            if (options_.debug_log) debug_log_msg(std::string("lazy expand cycle=") + detail::to_string_unsigned(cycle) +
                          " watch=" + detail::to_string_unsigned(watch.id) +
                          " path=" + watch.path +
                          " current=" + detail::pointer_to_string(current));
            const NodeId root = expander ? expander(current) : 0;

            LazyValueWatch* current_watch = find_lazy_watch(static_cast<WatchId>(i));
            if (current_watch && current_watch->alive && current_watch->last_value_address == current) {
                current_watch->target_subtree_root_id = root;
                // High-performance stable-source mode: read()/peek() sources are
                // only used to obtain the stable target address once.  After the
                // pointed object has been expanded, its scalar leaves are sampled
                // through ordinary parallel scalar tracks.  Do not keep calling
                // read()/peek() on the main thread every cycle.
                current_watch->alive = false;
                current_watch->read_value_address = std::function<const void*()>();
                current_watch->expand_value = std::function<NodeId(const void*)>();
            }
        }
    }

    template <typename V, typename Reader>
    NodeId add_lazy_value_object(const std::string& path,
                                 NodeId parent_id,
                                 Reader reader,
                                 WaveDirtyHook* dirty_hook = NULL,
                                 bool safe_alias_without_own_hook = false) {
        // WVZ4 stable-topology mode: read()/peek() sources are required to return
        // a stable address.  Resolve the address immediately during topology
        // preparation and recursively expand the pointed value; do not leave a
        // per-cycle lazy watch or an empty placeholder node behind.
        const V* p = reader();
        if (!p) {
            if (options_.debug_log) debug_log_msg(std::string("stable value source resolved null path=") + path);
            return 0;
        }

        const std::uint32_t group_id = get_or_create_dirty_peek_group(
            detail::pointer_address(p),
            reflect::type_tag_of<typename detail::remove_cvref<V>::type>(),
            static_cast<std::uint32_t>(sizeof(typename detail::remove_cvref<V>::type)));

        const std::uint32_t previous_group = active_dirty_peek_group_id_;
        const std::uint32_t previous_range = active_dirty_peek_range_id_;
        const std::uint32_t first_new_range = static_cast<std::uint32_t>(dirty_peek_ranges_.size());
        if (group_id != kInvalidIndex) {
            active_dirty_peek_group_id_ = group_id;
            active_dirty_peek_range_id_ = kInvalidIndex;
        }
        const NodeId result = expand_field(path, parent_id, static_cast<const V*>(p));
        const std::uint32_t end_new_range = static_cast<std::uint32_t>(dirty_peek_ranges_.size());
        active_dirty_peek_group_id_ = previous_group;
        active_dirty_peek_range_id_ = previous_range;

        // Bind the hook only after the peek value actually produced a signal
        // subtree.  Dirty-only polling suppression is enabled only when every
        // recorded range in the address group came from a hook-capable source.
        // This keeps mixed hook/non-hook aliases conservative and avoids missed
        // updates.
        if (result != 0 && first_new_range != end_new_range) {
            bind_dirty_hook_to_group(dirty_hook, group_id);
            if (dirty_hook) {
                mark_dirty_peek_ranges_hooked(group_id, first_new_range, end_new_range);
            } else if (safe_alias_without_own_hook) {
                mark_dirty_peek_ranges_safe_alias(group_id, first_new_range, end_new_range);
            }
        }
        return result;
    }

public:
    template <typename T>
    NodeId expand_registered_dynamic(const std::string& path, NodeId parent_id, const T* ptr) {
        return expand_field<T>(path, parent_id, ptr);
    }

private:
    template <typename Pointee>
    typename std::enable_if<!std::is_void<typename std::remove_cv<Pointee>::type>::value, NodeId>::type
    expand_pointer_pointee_dispatch(const std::string& path, NodeId parent_id, const void* target) {
        typedef typename std::remove_cv<Pointee>::type CleanPointee;
        if (!target) {
            debug_log_unsupported_type<CleanPointee>(path, parent_id, "pointer target null in pointee dispatch", "", target);
            return 0;
        }
#ifdef WAVE_DEBUG_EXPAND_POINTER_POINTEE_TYPE_DUMP
        detail::debug_type_dump<Pointee> __wave_debug_pointer_pointee;
        detail::debug_type_dump<CleanPointee> __wave_debug_pointer_clean_pointee;
        (void)__wave_debug_pointer_pointee;
        (void)__wave_debug_pointer_clean_pointee;
#endif
        if (options_.debug_log) debug_log_msg(std::string("pointer pointee dispatch path=") + path);
        return this->template expand_member_clean_dispatch<CleanPointee>(
            path,
            parent_id,
            static_cast<const CleanPointee*>(target));
    }

    template <typename Pointee>
    typename std::enable_if<std::is_void<typename std::remove_cv<Pointee>::type>::value, NodeId>::type
    expand_pointer_pointee_dispatch(const std::string&, NodeId, const void*) {
        return 0;
    }

    template <typename T>
    NodeId add_wave_value_signal(const std::string& path, NodeId parent_id, const WaveValue<T>* ptr) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Leaf, 0);
        if (options_.debug_log) debug_log_msg(std::string("wave_value leaf add path=") + path +
                      " wrapper=" + detail::pointer_to_string(detail::pointer_address(ptr)) +
                      " value_addr=" + detail::pointer_to_string(detail::pointer_address(std::addressof(ptr->read()))));
        const TrackId tid = create_wave_value_track<T>(node_id, path, ptr);
        if (tid == 0) {
            rollback_topology_to(cp);
            return 0;
        }
        return node_id;
    }

    template <typename T>
    NodeId add_bool_storage_signal(const std::string& path, NodeId parent_id, ::wave::BoolStoragePtr<T> ptr) {
        if (!ptr.ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Leaf, 0);
        if (options_.debug_log) debug_log_msg(std::string("bool_storage leaf add path=") + path +
                      " ptr=" + detail::pointer_to_string(detail::pointer_address(ptr.ptr)));
        const TrackId tid = create_bool_storage_ptr_track<T>(node_id, path, ptr);
        if (tid == 0) {
            rollback_topology_to(cp);
            return 0;
        }
        return node_id;
    }

    template <typename T>
    typename std::enable_if<!detail::is_std_string<T>::value, NodeId>::type
    add_leaf_signal(const std::string& path, NodeId parent_id, const T* ptr) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Leaf, 0);
        if (options_.debug_log) debug_log_msg(std::string("leaf add path=") + path +
                      " ptr=" + detail::pointer_to_string(detail::pointer_address(ptr)));
        const TrackId tid = create_scalar_ptr_track(
            node_id,
            path,
            detail::classify_value_kind<T>(),
            ptr);
        if (tid == 0) {
            rollback_topology_to(cp);
            return 0;
        }
        return node_id;
    }

    template <typename T>
    typename std::enable_if<detail::is_std_string<T>::value, NodeId>::type
    add_leaf_signal(const std::string& path, NodeId parent_id, const T*) {
        if (options_.debug_log) debug_log_msg(std::string("string leaf skipped path=") + path);
        return 0;
    }

    template <typename RawT>
    NodeId add_volatile_leaf_signal(const std::string& path, NodeId parent_id, const RawT* ptr) {
        if (!ptr) return 0;
        typedef typename detail::remove_cvref<RawT>::type CleanT;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Leaf, 0);
        if (options_.debug_log) debug_log_msg(std::string("volatile_leaf add path=") + path +
                      " ptr=" + detail::pointer_to_string(detail::pointer_address(ptr)));
        const TrackId tid = create_scalar_volatile_ptr_track<CleanT>(
            node_id,
            path,
            detail::classify_value_kind<CleanT>(),
            ptr);
        if (tid == 0) {
            rollback_topology_to(cp);
            return 0;
        }
        return node_id;
    }

    template <typename Obj, typename T>
    TrackId create_scalar_getter_track(NodeId node_id,
                                       const std::string& path,
                                       ValueKind kind,
                                       const Obj* obj,
                                       T (*getter)(const Obj*),
                                       std::uint32_t explicit_bit_width = 0) {
        const ScalarSampleKind scalar_kind = scalar_kind_for_type<T>();
        if (scalar_kind == ScalarSampleKind::None) {
            return 0;
        }

        const TrackId id = next_track_id_++;
        TrackDesc& track = ensure_id_slot_reserved(tracks_, id, options_.id_slot_track_growth_chunk);
        track = TrackDesc();
        track.id = id;
        track.node_id = node_id;
        track.kind = kind;
        TrackRuntimeState& runtime = ensure_id_slot_reserved(track_runtime_, id, options_.id_slot_track_growth_chunk);
        runtime = TrackRuntimeState();

        std::unique_ptr<ScalarGetterStorageBase> storage(
            new ScalarGetterStorage<Obj, T>(obj, getter));
        track.sample_ctx = static_cast<const void*>(storage.get());
        track.scalar_reader = &read_scalar_getter_storage<Obj, T>;
        track.scalar_kind = scalar_kind;
        track.bit_width = explicit_bit_width != 0 ? explicit_bit_width : scalar_bit_width(scalar_kind);
        track.thread_class = TrackThreadClass::ParallelSafe;
        if (active_dirty_peek_group_id_ != kInvalidIndex) {
            track.dirty_peek_group_id = active_dirty_peek_group_id_;
        }
        if (active_dirty_wave_array_group_id_ != kInvalidIndex) {
            track.dirty_wave_array_group_id = active_dirty_wave_array_group_id_;
        }
        scalar_getter_storage_.push_back(std::move(storage));

        const TrackId created = finish_track_create(node_id, path, kind, track);
        record_dirty_peek_leaf_for_track(created);
        record_dirty_wave_array_leaf_for_track(created);
        return created;
    }

    static std::uint32_t getter_bit_width_or_zero() { return 0; }

    template <typename First, typename... Rest>
    static std::uint32_t getter_bit_width_or_zero(First first, Rest...) {
        const long long v = static_cast<long long>(first);
        if (v <= 0) return 0;
        if (static_cast<unsigned long long>(v) > static_cast<unsigned long long>(std::numeric_limits<std::uint32_t>::max())) return 0;
        return static_cast<std::uint32_t>(v);
    }

    template <typename Obj, typename T>
    typename std::enable_if<detail::is_leaf_scalar<T>::value && !detail::is_std_string<T>::value, NodeId>::type
    add_getter_signal(const std::string& path, NodeId parent_id, const Obj* obj, T (*getter)(const Obj*), std::uint32_t explicit_bit_width = 0) {
        if (!obj || !getter) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Leaf, 0);
        const TrackId tid = create_scalar_getter_track<Obj, T>(
            node_id,
            path,
            detail::classify_value_kind<T>(),
            obj,
            getter,
            explicit_bit_width);
        if (tid == 0) {
            rollback_topology_to(cp);
            return 0;
        }
        return node_id;
    }

    template <typename Obj, typename T>
    typename std::enable_if<!detail::is_leaf_scalar<T>::value || detail::is_std_string<T>::value, NodeId>::type
    add_getter_signal(const std::string& path, NodeId parent_id, const Obj*, T (*)(const Obj*), std::uint32_t = 0) {
        if (options_.debug_log) debug_log_msg(std::string("non-scalar/string getter skipped path=") + path);
        return 0;
    }

    NodeId add_marker(const std::string& path, NodeId parent_id, InvalidKind invalid, const std::string& suffix) {
        if (options_.debug_log) debug_log_msg(std::string("marker skipped path=") + path +
                      " suffix=" + suffix +
                      " invalid=" + invalid_kind_name(invalid) +
                      " parent=" + detail::to_string_unsigned(parent_id));
        (void)suffix;
        return 0;
    }

    template <typename T>
    typename std::enable_if<detail::is_leaf_scalar<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        return add_leaf_signal<T>(path, parent_id, ptr);
    }

    NodeId expand_field(const std::string& path, NodeId parent_id, const char* const*) {
        if (options_.debug_log) debug_log_msg(std::string("c-string leaf skipped path=") + path);
        return 0;
    }

    NodeId expand_field(const std::string& path, NodeId parent_id, char* const*) {
        if (options_.debug_log) debug_log_msg(std::string("c-string leaf skipped path=") + path);
        return 0;
    }

    template <typename ArrT>
    typename std::enable_if<detail::is_wave_array<ArrT>::value, NodeId>::type
    expand_wave_array_field_impl(const std::string& path, NodeId parent_id, const ArrT* ptr) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        typedef typename wave_array_traits<ArrT>::element_type ElemT;
        const std::uint32_t prev_group = active_dirty_wave_array_group_id_;
        const std::uint32_t prev_range = active_dirty_wave_array_range_id_;

        for (std::size_t i = 0; i < wave_array_traits<ArrT>::size; ++i) {
            const ElemT* elem = std::addressof((*ptr)[i]);
            if (options_.enable_wave_array_dirty) {
                const std::uint32_t group_id = get_or_create_dirty_wave_array_group(
                    static_cast<const void*>(elem),
                    reflect::type_tag_of<ElemT>(),
                    static_cast<std::uint32_t>(sizeof(ElemT)),
                    static_cast<std::uint32_t>(i));
                active_dirty_wave_array_group_id_ = group_id;
                active_dirty_wave_array_range_id_ = kInvalidIndex;
            } else {
                active_dirty_wave_array_group_id_ = kInvalidIndex;
                active_dirty_wave_array_range_id_ = kInvalidIndex;
            }
            expand_member_clean_dispatch<ElemT>(
                detail::compose_index_child_path(path, i),
                node_id,
                elem);
        }

        active_dirty_wave_array_group_id_ = prev_group;
        active_dirty_wave_array_range_id_ = prev_range;
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename T>
    typename std::enable_if<detail::is_wave_array<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        return expand_wave_array_field_impl<T>(path, parent_id, ptr);
    }

    template <typename T, std::size_t N>
    NodeId expand_field(const std::string& path, NodeId parent_id, const T (*ptr)[N]) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        for (std::size_t i = 0; i < N; ++i) {
            expand_field(detail::compose_index_child_path(path, i), node_id, &((*ptr)[i]));
        }
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename T>
    typename std::enable_if<detail::is_std_array<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        for (std::size_t i = 0; i < ptr->size(); ++i) {
            typedef typename detail::remove_cvref<decltype((*ptr)[i])>::type ElementT;
            expand_field(detail::compose_index_child_path(path, i), node_id, &((*ptr)[i]));
        }
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename T>
    typename std::enable_if<detail::is_std_pair<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        expand_field(detail::compose_child_path(path, "first"), node_id, &ptr->first);
        expand_field(detail::compose_child_path(path, "second"), node_id, &ptr->second);
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename T>
    typename std::enable_if<detail::is_blacklisted_stl<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T*) {
        debug_log_unsupported_type<T>(path, parent_id, "blacklisted STL/container type", "$unsupported");
        return 0;
    }

#ifndef WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS
#  if WAVE_HAS_SYSTEMC
    template <typename V, int N, sc_core::sc_port_policy POL>
    const V* read_vsip_peek(const ::vsipIN<V, N, POL>* port) const {
        if (!port || port->size() == 0) return NULL;
        const ::vsiiIN<V>* iface = (*port).operator->();
        return iface ? const_cast< ::vsiiIN<V>* >(iface)->peek() : static_cast<const V*>(NULL);
    }

    template <typename V, int N, sc_core::sc_port_policy POL>
    const V* read_vsip_peek(const ::vsipOUT<V, N, POL>* port) const {
        if (!port || port->size() == 0) return NULL;
        const ::vsiiOUT<V>* iface = (*port).operator->();
        return iface ? const_cast< ::vsiiOUT<V>* >(iface)->peek() : static_cast<const V*>(NULL);
    }

    template <typename V, int N, sc_core::sc_port_policy POL>
    const V* read_vsip_peek(const ::vsipINOUT<V, N, POL>* port) const {
        if (!port || port->size() == 0) return NULL;
        const ::vsiiINOUT<V>* iface = (*port).operator->();
        if (!iface) return NULL;
        ::vsiiIN<V>* in_iface = const_cast< ::vsiiIN<V>* >(static_cast<const ::vsiiIN<V>* >(iface));
        return in_iface ? in_iface->peek() : static_cast<const V*>(NULL);
    }
#  else
    template <typename V>
    const V* read_vsip_peek(const ::vsipIN<V>* port) const {
        if (!port || port->size() == 0) return NULL;
        const ::vsiiIN<V>* iface = (*port).operator->();
        return iface ? const_cast< ::vsiiIN<V>* >(iface)->peek() : static_cast<const V*>(NULL);
    }

    template <typename V>
    const V* read_vsip_peek(const ::vsipOUT<V>* port) const {
        if (!port || port->size() == 0) return NULL;
        const ::vsiiOUT<V>* iface = (*port).operator->();
        return iface ? const_cast< ::vsiiOUT<V>* >(iface)->peek() : static_cast<const V*>(NULL);
    }

    template <typename V>
    const V* read_vsip_peek(const ::vsipINOUT<V>* port) const {
        if (!port || port->size() == 0) return NULL;
        const ::vsiiINOUT<V>* iface = (*port).operator->();
        if (!iface) return NULL;
        ::vsiiIN<V>* in_iface = const_cast< ::vsiiIN<V>* >(static_cast<const ::vsiiIN<V>* >(iface));
        return in_iface ? in_iface->peek() : static_cast<const V*>(NULL);
    }
#  endif
#endif

    template <typename Port, typename V>
    typename std::enable_if<detail::is_vsip_exact_port<Port>::value, const V*>::type
    read_vsip_value(const Port* port) const {
        return read_vsip_peek(port);
    }

    template <typename Obj, typename V>
    typename std::enable_if<std::is_base_of< ::vsiiINOUT<V>, typename detail::remove_cvref<Obj>::type>::value, const V*>::type
    read_vsii_object_peek(const Obj* obj) const {
        if (!obj) return static_cast<const V*>(NULL);
        typedef typename detail::remove_cvref<Obj>::type CleanObj;
        const ::vsiiINOUT<V>* inout_iface = static_cast<const ::vsiiINOUT<V>*>(static_cast<const CleanObj*>(obj));
        const ::vsiiIN<V>* in_iface = static_cast<const ::vsiiIN<V>*>(inout_iface);
        return const_cast< ::vsiiIN<V>* >(in_iface)->peek();
    }

    template <typename Obj, typename V>
    typename std::enable_if<!std::is_base_of< ::vsiiINOUT<V>, typename detail::remove_cvref<Obj>::type>::value &&
                            std::is_base_of< ::vsiiIN<V>, typename detail::remove_cvref<Obj>::type>::value, const V*>::type
    read_vsii_object_peek(const Obj* obj) const {
        if (!obj) return static_cast<const V*>(NULL);
        typedef typename detail::remove_cvref<Obj>::type CleanObj;
        const ::vsiiIN<V>* in_iface = static_cast<const ::vsiiIN<V>*>(static_cast<const CleanObj*>(obj));
        return const_cast< ::vsiiIN<V>* >(in_iface)->peek();
    }

    template <typename Obj, typename V>
    typename std::enable_if<!std::is_base_of< ::vsiiINOUT<V>, typename detail::remove_cvref<Obj>::type>::value &&
                            !std::is_base_of< ::vsiiIN<V>, typename detail::remove_cvref<Obj>::type>::value &&
                            std::is_base_of< ::vsiiOUT<V>, typename detail::remove_cvref<Obj>::type>::value, const V*>::type
    read_vsii_object_peek(const Obj* obj) const {
        if (!obj) return static_cast<const V*>(NULL);
        typedef typename detail::remove_cvref<Obj>::type CleanObj;
        const ::vsiiOUT<V>* out_iface = static_cast<const ::vsiiOUT<V>*>(static_cast<const CleanObj*>(obj));
        return const_cast< ::vsiiOUT<V>* >(out_iface)->peek();
    }

    template <typename Port, typename V>
    typename std::enable_if<!detail::is_vsip_exact_port<Port>::value && detail::is_vsip_read_port<Port>::value, const V*>::type
    read_vsip_value(const Port* port) const {
        // IF-derived channel/wrapper classes are detected through the template
        // bases vsiiIN<V>, vsiiOUT<V>, or vsiiINOUT<V>.  Call peek() through the
        // selected vsii<T> base.  This avoids ambiguous .peek() lookup on
        // vsiiINOUT-derived classes, where both vsiiIN<T> and vsiiOUT<T> may
        // declare peek().
        return read_vsii_object_peek<Port, V>(port);
    }


#ifndef WAVE_DISABLE_VSIP_PORT_FORWARD_DECLS
#  if WAVE_HAS_SYSTEMC
    template <typename V, int N, sc_core::sc_port_policy POL>
    WaveDirtyHook* read_vsip_exact_dirty_hook(const ::vsipIN<V, N, POL>* port) const {
        if (!port || port->size() == 0) return static_cast<WaveDirtyHook*>(NULL);
        auto iface = (*port).operator->();
        return detail::maybe_wave_dirty_hook(iface);
    }

    template <typename V, int N, sc_core::sc_port_policy POL>
    WaveDirtyHook* read_vsip_exact_dirty_hook(const ::vsipOUT<V, N, POL>* port) const {
        if (!port || port->size() == 0) return static_cast<WaveDirtyHook*>(NULL);
        auto iface = (*port).operator->();
        return detail::maybe_wave_dirty_hook(iface);
    }

    template <typename V, int N, sc_core::sc_port_policy POL>
    WaveDirtyHook* read_vsip_exact_dirty_hook(const ::vsipINOUT<V, N, POL>* port) const {
        if (!port || port->size() == 0) return static_cast<WaveDirtyHook*>(NULL);
        auto iface = (*port).operator->();
        return detail::maybe_wave_dirty_hook(iface);
    }
#  else
    template <typename V>
    WaveDirtyHook* read_vsip_exact_dirty_hook(const ::vsipIN<V>* port) const {
        if (!port || port->size() == 0) return static_cast<WaveDirtyHook*>(NULL);
        auto iface = (*port).operator->();
        return detail::maybe_wave_dirty_hook(iface);
    }

    template <typename V>
    WaveDirtyHook* read_vsip_exact_dirty_hook(const ::vsipOUT<V>* port) const {
        if (!port || port->size() == 0) return static_cast<WaveDirtyHook*>(NULL);
        auto iface = (*port).operator->();
        return detail::maybe_wave_dirty_hook(iface);
    }

    template <typename V>
    WaveDirtyHook* read_vsip_exact_dirty_hook(const ::vsipINOUT<V>* port) const {
        if (!port || port->size() == 0) return static_cast<WaveDirtyHook*>(NULL);
        auto iface = (*port).operator->();
        return detail::maybe_wave_dirty_hook(iface);
    }
#  endif
#endif

    template <typename Port, typename V>
    typename std::enable_if<detail::is_vsip_exact_port<Port>::value, WaveDirtyHook*>::type
    read_vsip_dirty_hook(const Port* port) const {
        // Exact vsip* port wrappers must fetch the dirty hook from the bound
        // interface/channel via operator->(), matching the peek() source.  Calling
        // maybe_wave_dirty_hook(port) would only inspect the wrapper object and can
        // miss a hook provided by the channel interface.
        return read_vsip_exact_dirty_hook(port);
    }

    template <typename Obj, typename V>
    typename std::enable_if<!detail::is_vsip_exact_port<Obj>::value && detail::is_vsip_read_port<Obj>::value, WaveDirtyHook*>::type
    read_vsip_dirty_hook(const Obj* obj) const {
        // vsii-derived objects are real value-source objects, so access the hook
        // directly through the object itself.
        return detail::maybe_wave_dirty_hook(obj);
    }

    template <typename T>
    typename std::enable_if<detail::is_vsip_read_port<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        typedef typename detail::vsip_port_value_type<T>::type V;
        // High-performance source handling: vsip/vsii peek() already returns a
        // stable pointer.  Defer the first peek until sampling time so ports may
        // finish binding, then expand the returned object by pointer.
        // If V is scalar this creates a scalar_ptr_track; if V is reflected or a
        // container it recursively expands its members until scalar leaves.
        return add_lazy_value_object<V>(path, parent_id, [this, ptr]() -> const V* {
            return this->read_vsip_value<T, V>(ptr);
        }, this->template read_vsip_dirty_hook<T, V>(ptr),
           detail::is_vsip_exact_port<T>::value);
    }

#if WAVE_HAS_SYSTEMC
    template <typename T>
    typename std::enable_if<detail::is_sc_vector<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        if (!ptr) return 0;
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        typedef typename detail::remove_cvref<decltype((*ptr)[0])>::type ElementT;
        for (std::size_t i = 0; i < ptr->size(); ++i) {
            expand_field(detail::compose_index_child_path(path, i), node_id, &((*ptr)[i]));
        }
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename T>
    typename std::enable_if<
        (detail::is_sc_in<T>::value || detail::is_sc_out<T>::value || detail::is_sc_inout<T>::value),
        NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        typedef typename detail::sc_port_value_type<T>::type V;
        // SystemC port read() returns a stable reference in this high-performance
        // mode.  Defer the first read until sampling time, then take the
        // address of that reference.  This is deliberately different from
        // vsip/vsii peek(), which already returns a pointer.
        return add_lazy_value_object<V>(path, parent_id, [ptr]() -> const V* {
            if (!ptr || ptr->size() == 0) return static_cast<const V*>(NULL);
            const V& ref = ptr->read();
            return std::addressof(ref);
        }, detail::maybe_wave_dirty_hook(ptr), true);
    }

    template <typename T>
    typename std::enable_if<detail::is_sc_signal<T>::value || detail::is_sc_buffer<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        typedef typename detail::sc_signal_value_type<T>::type V;
        // sc_signal/sc_buffer read() returns a stable reference in this high-
        // performance mode.  Take the address of that reference; scalar V
        // becomes a scalar_ptr_track, and reflected/container V is recursively
        // expanded.
        const V& ref = ptr->read();
        return expand_field(path, parent_id, std::addressof(ref));
    }

    template <typename T>
    typename std::enable_if<detail::is_sc_clock<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        const bool& ref = ptr->read();
        return expand_field(path, parent_id, std::addressof(ref));
    }
#endif

    template <typename T>
    typename std::enable_if<
        std::is_pointer<T>::value &&
        !std::is_same<typename detail::remove_cvref<T>::type, const char*>::value &&
        !std::is_same<typename detail::remove_cvref<T>::type, char*>::value,
        NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* field_ptr) {
        typedef typename std::remove_pointer<T>::type Pointee;
        const void* target = field_ptr ? detail::pointer_address(*field_ptr) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename T>
    typename std::enable_if<detail::is_unique_ptr_scalar<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* field_ptr) {
        typedef typename detail::unique_ptr_scalar_target<T>::type Pointee;
        const void* target = field_ptr ? detail::pointer_address(field_ptr->get()) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename T>
    typename std::enable_if<detail::is_unique_ptr_array<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* field_ptr) {
        typedef typename detail::unique_ptr_array_element<T>::type Elem;
        const void* target = field_ptr ? detail::pointer_address(field_ptr->get()) : static_cast<const void*>(NULL);
        if (options_.debug_log) {
            debug_log_msg(std::string("pointer array skipped path=") + path +
                          " reason=pointer-array-tracking-disabled" +
                          " elem=" + typeid(Elem).name() +
                          " addr=" + detail::pointer_to_string(target));
        }
        return 0;
    }

    template <typename T>
    typename std::enable_if<detail::is_shared_ptr<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* field_ptr) {
        typedef typename detail::shared_ptr_target<T>::type Pointee;
        const void* target = field_ptr ? detail::pointer_address(field_ptr->get()) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename T>
    typename std::enable_if<detail::is_weak_ptr<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* field_ptr) {
        typedef typename detail::weak_ptr_target<T>::type Pointee;
        std::shared_ptr<Pointee> sp;
        if (field_ptr) sp = field_ptr->lock();
        const void* target = sp ? detail::pointer_address(sp.get()) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename P>
    typename std::enable_if<detail::is_bool_storage_ptr<typename detail::remove_cvref<P>::type>::value, NodeId>::type
    expand_member_ptr(const std::string& path, NodeId parent_id, P field_ptr) {
        return add_bool_storage_signal(path, parent_id, field_ptr);
    }

    template <typename P>
    typename std::enable_if<detail::is_c_string_member_ptr<P>::value, NodeId>::type
    expand_member_ptr(const std::string& path, NodeId parent_id, P field_ptr) {
        return expand_field(path, parent_id, field_ptr);
    }

    template <typename P>
    typename std::enable_if<detail::is_volatile_leaf_member_ptr<P>::value, NodeId>::type
    expand_member_ptr(const std::string& path, NodeId parent_id, P field_ptr) {
        typedef typename std::remove_pointer<P>::type RawFieldT;
        return add_volatile_leaf_signal<RawFieldT>(path, parent_id, field_ptr);
    }

    template <typename FieldT>
    NodeId expand_reflected_field_direct(const std::string& path, NodeId parent_id, const FieldT* ptr) {
        if (!ptr) return 0;
        const ObjectKey key(detail::pointer_address(ptr), reflect::type_tag_of<FieldT>());
        if (contains_expanding_key(expanding_, key)) {
            debug_log_unsupported_type<FieldT>(path, parent_id, "recursive expansion cut", "$recursive_cut", ptr);
            return 0;
        }

        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const ObjectId object_id = ensure_object(key, typeid(FieldT).name());
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, object_id);
        ExpandingGuard expanding_guard(expanding_, key);
        detail::call_reflected_visit<FieldT>(
            ptr,
            [this, &path, node_id](const char* name, auto child_field_ptr) {
                expand_member_ptr(detail::compose_child_path(path, name), node_id, child_field_ptr);
            },
            [this, &path, node_id](const char* name, const auto&) {
                const std::string bit_path = detail::compose_child_path(path, name);
                debug_log_unsupported_raw(bit_path, node_id, "bitfield/getter value not directly addressable", std::string(), "$bitfield_getter_missing");
            },
            [this, &path, node_id, ptr](const char* name, auto getter, auto... meta) {
                typedef typename detail::remove_cvref<decltype(getter(ptr))>::type GetterValueT;
                const std::string bit_path = detail::compose_child_path(path, name);
                add_getter_signal<FieldT, GetterValueT>(bit_path, node_id, ptr, getter, getter_bit_width_or_zero(meta...));
            });
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 13>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=wave_array path=") + path);
        return expand_wave_array_field_impl<FieldT>(path, parent_id, clean_ptr);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 12>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=wave_value path=") + path);
        typedef typename wave_value_underlying<FieldT>::type UnderlyingT;
        return add_wave_value_signal<UnderlyingT>(path, parent_id, clean_ptr);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 11>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=c_string path=") + path);
        return expand_field(path, parent_id, clean_ptr);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 10>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=vsip path=") + path);
        typedef typename detail::vsip_port_value_type<FieldT>::type V;
        return expand_member_vsip_dispatch<FieldT, V>(path, parent_id, clean_ptr,
                                                      typename detail::is_leaf_scalar<V>::type());
    }

    template <typename FieldT, typename V, typename IsScalar>
    NodeId expand_member_vsip_dispatch(const std::string& path,
                                       NodeId parent_id,
                                       const FieldT* clean_ptr,
                                       IsScalar) {
        (void)sizeof(IsScalar);
        // Member vsip/vsii ports use the same stable-pointer path as root
        // vsip/vsii ports.  peek() returns const V*, and the pointed V object is
        // recursively expanded; scalar V becomes a scalar_ptr_track.
        return add_lazy_value_object<V>(path, parent_id, [this, clean_ptr]() -> const V* {
            return this->template read_vsip_value<FieldT, V>(clean_ptr);
        }, this->template read_vsip_dirty_hook<FieldT, V>(clean_ptr),
           detail::is_vsip_exact_port<FieldT>::value);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 9>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=leaf path=") + path);
        return add_leaf_signal<FieldT>(path, parent_id, clean_ptr);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 8>) {
        if (!clean_ptr) return 0;
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=std_array path=") + path);
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        for (std::size_t i = 0; i < clean_ptr->size(); ++i) {
            typedef typename detail::remove_cvref<decltype((*clean_ptr)[i])>::type ElemT;
            expand_member_clean_dispatch<ElemT>(
                detail::compose_index_child_path(path, i),
                node_id,
                static_cast<const ElemT*>(&((*clean_ptr)[i])));
        }
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 7>) {
        if (!clean_ptr) return 0;
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=std_pair path=") + path);
        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, 0);
        typedef typename detail::remove_cvref<decltype(clean_ptr->first)>::type FirstT;
        typedef typename detail::remove_cvref<decltype(clean_ptr->second)>::type SecondT;
        expand_member_clean_dispatch<FirstT>(
            detail::compose_child_path(path, "first"),
            node_id,
            static_cast<const FirstT*>(&clean_ptr->first));
        expand_member_clean_dispatch<SecondT>(
            detail::compose_child_path(path, "second"),
            node_id,
            static_cast<const SecondT*>(&clean_ptr->second));
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT*,
                                                 std::integral_constant<int, 6>) {
        debug_log_unsupported_type<FieldT>(path, parent_id, "blacklisted STL/container type", "$unsupported");
        return 0;
    }

    template <typename Pointee>
    typename std::enable_if<detail::is_direct_reflect_pointer_target<Pointee>::value, NodeId>::type
    expand_direct_marked_pointer_once(const std::string& path, NodeId parent_id, const void* target) {
        typedef typename detail::remove_cvref<Pointee>::type CleanPointee;
        if (!target) {
            if (options_.debug_log) debug_log_msg(std::string("pointer direct marker null path=") + path);
            return 0;
        }
        if (options_.debug_log) {
            debug_log_msg(std::string("pointer direct marker expand-once path=") + path +
                          " type=" + typeid(CleanPointee).name() +
                          " addr=" + detail::pointer_to_string(target));
        }
        return expand_member_clean_dispatch<CleanPointee>(
            path,
            parent_id,
            static_cast<const CleanPointee*>(target));
    }

    template <typename Pointee>
    typename std::enable_if<!detail::is_direct_reflect_pointer_target<Pointee>::value, NodeId>::type
    expand_direct_marked_pointer_once(const std::string& path, NodeId parent_id, const void* target) {
        typedef typename detail::remove_cvref<Pointee>::type CleanPointee;
        if (options_.debug_log) {
            debug_log_msg(std::string("pointer skipped path=") + path +
                          " reason=pointee-not-DirectReflectPointerTarget" +
                          " type=" + typeid(CleanPointee).name() +
                          " addr=" + detail::pointer_to_string(target));
        }
        return 0;
    }

    template <typename FieldT>
    typename std::enable_if<
        std::is_pointer<FieldT>::value &&
        !std::is_same<typename detail::remove_cvref<FieldT>::type, const char*>::value &&
        !std::is_same<typename detail::remove_cvref<FieldT>::type, char*>::value,
        NodeId>::type
    expand_pointer_or_smart_member_direct(const std::string& path, NodeId parent_id, const FieldT* field_ptr) {
        typedef typename std::remove_pointer<FieldT>::type Pointee;
        const void* target = field_ptr ? detail::pointer_address(*field_ptr) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename FieldT>
    typename std::enable_if<detail::is_unique_ptr_scalar<FieldT>::value, NodeId>::type
    expand_pointer_or_smart_member_direct(const std::string& path, NodeId parent_id, const FieldT* field_ptr) {
        typedef typename detail::unique_ptr_scalar_target<FieldT>::type Pointee;
        const void* target = field_ptr ? detail::pointer_address(field_ptr->get()) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename FieldT>
    typename std::enable_if<detail::is_unique_ptr_array<FieldT>::value, NodeId>::type
    expand_pointer_or_smart_member_direct(const std::string& path, NodeId parent_id, const FieldT* field_ptr) {
        typedef typename detail::unique_ptr_array_element<FieldT>::type Elem;
        const void* target = field_ptr ? detail::pointer_address(field_ptr->get()) : static_cast<const void*>(NULL);
        if (options_.debug_log) {
            debug_log_msg(std::string("pointer array skipped path=") + path +
                          " reason=pointer-array-tracking-disabled" +
                          " elem=" + typeid(Elem).name() +
                          " addr=" + detail::pointer_to_string(target));
        }
        return 0;
    }

    template <typename FieldT>
    typename std::enable_if<detail::is_shared_ptr<FieldT>::value, NodeId>::type
    expand_pointer_or_smart_member_direct(const std::string& path, NodeId parent_id, const FieldT* field_ptr) {
        typedef typename detail::shared_ptr_target<FieldT>::type Pointee;
        const void* target = field_ptr ? detail::pointer_address(field_ptr->get()) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename FieldT>
    typename std::enable_if<detail::is_weak_ptr<FieldT>::value, NodeId>::type
    expand_pointer_or_smart_member_direct(const std::string& path, NodeId parent_id, const FieldT* field_ptr) {
        typedef typename detail::weak_ptr_target<FieldT>::type Pointee;
        std::shared_ptr<Pointee> sp;
        if (field_ptr) sp = field_ptr->lock();
        const void* target = sp ? detail::pointer_address(sp.get()) : static_cast<const void*>(NULL);
        return expand_direct_marked_pointer_once<Pointee>(path, parent_id, target);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 5>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=pointer_or_smartptr direct-marker-only path=") + path);
        return expand_pointer_or_smart_member_direct<FieldT>(path, parent_id, clean_ptr);
    }

#if WAVE_HAS_SYSTEMC
    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 4>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=systemc_whitelist path=") + path);
        return this->template expand_field<FieldT>(path, parent_id, clean_ptr);
    }
#endif

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT*,
                                                 std::integral_constant<int, 3>) {
        debug_log_unsupported_type<FieldT>(path, parent_id, "sc_core namespace blacklisted type", "$sc_core_blacklisted");
        return 0;
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT* clean_ptr,
                                                 std::integral_constant<int, 2>) {
        if (options_.debug_log) debug_log_msg(std::string("member dispatch branch=reflected path=") + path);
        return expand_reflected_field_direct<FieldT>(path, parent_id, clean_ptr);
    }

    template <typename FieldT>
    NodeId expand_member_clean_dispatch_selected(const std::string& path,
                                                 NodeId parent_id,
                                                 const FieldT*,
                                                 std::integral_constant<int, 0>) {
        debug_log_unsupported_type<FieldT>(path, parent_id, "member dispatch: no matching branch", "$type_unsupported");
        return 0;
    }

    template <typename FieldT>
    struct member_dispatch_category {
        enum {
            is_wave_array = detail::is_wave_array<FieldT>::value,
            is_wave_value = detail::is_wave_value<FieldT>::value,
            is_c_string = std::is_same<typename detail::remove_cvref<FieldT>::type, char*>::value ||
                          std::is_same<typename detail::remove_cvref<FieldT>::type, const char*>::value,
            is_pointer_or_smart = std::is_pointer<FieldT>::value ||
                                  detail::is_unique_ptr_scalar<FieldT>::value ||
                                  detail::is_unique_ptr_array<FieldT>::value ||
                                  detail::is_shared_ptr<FieldT>::value ||
                                  detail::is_weak_ptr<FieldT>::value,
#if WAVE_HAS_SYSTEMC
            is_systemc_whitelist = detail::is_sc_vector<FieldT>::value ||
                                   detail::is_sc_in<FieldT>::value ||
                                   detail::is_sc_out<FieldT>::value ||
                                   detail::is_sc_inout<FieldT>::value ||
                                   detail::is_sc_signal<FieldT>::value ||
                                   detail::is_sc_buffer<FieldT>::value ||
                                   detail::is_sc_clock<FieldT>::value,
#else
            is_systemc_whitelist = false,
#endif
            value = is_wave_array ? 13 :
                    is_wave_value ? 12 :
                    is_c_string ? 11 :
                    detail::is_vsip_read_port<FieldT>::value ? 10 :
                    detail::is_leaf_scalar<FieldT>::value ? 9 :
                    detail::is_std_array<FieldT>::value ? 8 :
                    detail::is_std_pair<FieldT>::value ? 7 :
                    detail::is_blacklisted_stl<FieldT>::value ? 6 :
                    is_pointer_or_smart ? 5 :
                    is_systemc_whitelist ? 4 :
                    detail::is_sc_namespace_blacklisted<FieldT>::value ? 3 :
                    reflect::is_reflected<FieldT>::value ? 2 :
                    0
        };
    };

    template <typename FieldT>
    NodeId expand_member_clean_dispatch(const std::string& path, NodeId parent_id, const FieldT* clean_ptr) {
        /*
         * C++14 deterministic dispatch.  This deliberately avoids calling an
         * overload set with inherited priority_tag conversions, because MSVC can
         * still report ambiguous overloads for project wrapper types.
         */
#ifdef WAVE_DEBUG_EXPAND_MEMBER_DISPATCH
        if (options_.debug_log) debug_log_msg(std::string("member dispatch category=") + detail::to_string_signed(member_dispatch_category<FieldT>::value) +
                      " path=" + path + " type=" + typeid(FieldT).name());
#endif
        return expand_member_clean_dispatch_selected<FieldT>(
            path,
            parent_id,
            clean_ptr,
            std::integral_constant<int, member_dispatch_category<FieldT>::value>());
    }

    template <typename P>
    typename std::enable_if<!detail::is_bool_storage_ptr<typename detail::remove_cvref<P>::type>::value &&
                            !detail::is_volatile_leaf_member_ptr<P>::value &&
                            !detail::is_c_string_member_ptr<P>::value &&
                            !std::is_array<typename detail::remove_cvref<typename std::remove_pointer<P>::type>::type>::value &&
                            !std::is_volatile<typename std::remove_pointer<P>::type>::value, NodeId>::type
    expand_member_ptr(const std::string& path, NodeId parent_id, P field_ptr) {
        typedef typename detail::remove_cvref<typename std::remove_pointer<P>::type>::type FieldT;
#ifdef WAVE_DEBUG_EXPAND_MEMBER_TYPE_DUMP
        detail::debug_type_dump<P> __wave_debug_expand_member_P;
        detail::debug_type_dump<FieldT> __wave_debug_expand_member_FieldT;
        (void)__wave_debug_expand_member_P;
        (void)__wave_debug_expand_member_FieldT;
#endif
        return expand_member_clean_dispatch<FieldT>(
            path,
            parent_id,
            static_cast<const FieldT*>(field_ptr));
    }

    template <typename P>
    typename std::enable_if<!detail::is_bool_storage_ptr<typename detail::remove_cvref<P>::type>::value &&
                            !detail::is_volatile_leaf_member_ptr<P>::value &&
                            !detail::is_c_string_member_ptr<P>::value &&
                            std::is_volatile<typename std::remove_pointer<P>::type>::value, NodeId>::type
    expand_member_ptr(const std::string& path, NodeId parent_id, P field_ptr) {
        typedef typename detail::remove_cvref<typename std::remove_pointer<P>::type>::type FieldT;
        debug_log_unsupported_type<FieldT>(
            path,
            parent_id,
            "volatile non-leaf member is not expanded",
            "$volatile_non_leaf",
            detail::pointer_address(field_ptr));
        return 0;
    }

    template <typename U, std::size_t N>
    NodeId expand_member_ptr(const std::string& path, NodeId parent_id, const U (*field_ptr)[N]) {
        return expand_field(path, parent_id, field_ptr);
    }

    template <typename T>
    typename std::enable_if<
        detail::is_sc_namespace_blacklisted<T>::value &&
        !detail::is_std_array<T>::value &&
        !detail::is_std_pair<T>::value &&
        !detail::is_blacklisted_stl<T>::value &&
        !std::is_pointer<T>::value &&
        !detail::is_unique_ptr_scalar<T>::value &&
        !detail::is_unique_ptr_array<T>::value &&
        !detail::is_shared_ptr<T>::value &&
        !detail::is_weak_ptr<T>::value &&
        !detail::is_vsip_read_port<T>::value &&
        !detail::is_sc_in<T>::value &&
        !detail::is_sc_out<T>::value &&
        !detail::is_sc_inout<T>::value &&
        !detail::is_sc_signal<T>::value &&
        !detail::is_sc_buffer<T>::value &&
        !detail::is_sc_vector<T>::value &&
        !detail::is_sc_clock<T>::value,
        NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T*) {
        debug_log_unsupported_type<T>(path, parent_id, "sc_core namespace blacklisted type", "$sc_core_blacklisted");
        return 0;
    }

    template <typename T>
    typename std::enable_if<reflect::is_reflected<T>::value && !detail::is_vsip_read_port<T>::value && !detail::is_sc_namespace_blacklisted<T>::value, NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T* ptr) {
        if (!ptr) return 0;
        const ObjectKey key(detail::pointer_address(ptr), reflect::type_tag_of<T>());
        if (contains_expanding_key(expanding_, key)) {
            debug_log_unsupported_type<T>(path, parent_id, "recursive expansion cut", "$recursive_cut", ptr);
            return 0;
        }

        TopologyCheckpoint cp = make_topology_checkpoint(parent_id);
        const ObjectId object_id = ensure_object(key, typeid(T).name());
        const NodeId node_id = create_node(parent_id, path, NodeKind::Aggregate, object_id);
        ExpandingGuard expanding_guard(expanding_, key);
        detail::call_reflected_visit<T>(
            ptr,
            [this, &path, node_id](const char* name, auto field_ptr) {
                expand_member_ptr(detail::compose_child_path(path, name), node_id, field_ptr);
            },
            [this, &path, node_id](const char* name, const auto&) {
                const std::string bit_path = detail::compose_child_path(path, name);
                debug_log_unsupported_raw(bit_path, node_id, "bitfield/getter value not directly addressable", std::string(), "$bitfield_getter_missing");
            },
            [this, &path, node_id, ptr](const char* name, auto getter, auto... meta) {
                typedef typename detail::remove_cvref<decltype(getter(ptr))>::type GetterValueT;
                const std::string bit_path = detail::compose_child_path(path, name);
                add_getter_signal<T, GetterValueT>(bit_path, node_id, ptr, getter, getter_bit_width_or_zero(meta...));
            });
        return keep_node_or_rollback(cp, node_id);
    }

    template <typename T>
    typename std::enable_if<!reflect::is_reflected<T>::value &&
                            !detail::is_leaf_scalar<T>::value &&
                            !detail::is_std_array<T>::value &&
                            !detail::is_wave_array<T>::value &&
                            !detail::is_std_pair<T>::value &&
                            !detail::is_blacklisted_stl<T>::value &&
                            !std::is_pointer<T>::value &&
                            !detail::is_unique_ptr_scalar<T>::value &&
                            !detail::is_unique_ptr_array<T>::value &&
                            !detail::is_shared_ptr<T>::value &&
                            !detail::is_weak_ptr<T>::value &&
                            !detail::is_vsip_read_port<T>::value &&
                            !detail::is_sc_in<T>::value &&
                            !detail::is_sc_out<T>::value &&
                            !detail::is_sc_inout<T>::value &&
                            !detail::is_sc_signal<T>::value &&
                            !detail::is_sc_buffer<T>::value &&
                            !detail::is_sc_vector<T>::value &&
                            !detail::is_sc_clock<T>::value &&
                            !detail::is_sc_namespace_blacklisted<T>::value,
                            NodeId>::type
    expand_field(const std::string& path, NodeId parent_id, const T*) {
        debug_log_unsupported_type<T>(path, parent_id, "no matching expand_field branch: not reflected, not scalar, not array/pair/string/pointer/smart_ptr/vsip/sc", "$type_unsupported");
        return 0;
    }
};

inline ThreadTraceLocal::~ThreadTraceLocal() noexcept {
    if (owner) {
        owner->adopt_tls_dirty_on_thread_exit(*this);
    }
    WaveTlsRegistry::instance().unregister_tls(this);
}

template <typename T>
NodeId dynamic_expand_bridge(Tracer& tracer, const std::string& path, NodeId parent_id, const void* obj) {
    return tracer.template expand_registered_dynamic<T>(path, parent_id, static_cast<const T*>(obj));
}





} // namespace wave

#if defined(WAVE_RUNTIME_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef WAVE_RUNTIME_RESTORE_MAX_MACRO_
#endif
#if defined(WAVE_RUNTIME_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef WAVE_RUNTIME_RESTORE_MIN_MACRO_
#endif
