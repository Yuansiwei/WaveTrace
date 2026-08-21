#pragma once

#ifndef WAVETRACE_REFLECT_MACRO_H_INCLUDED_
#define WAVETRACE_REFLECT_MACRO_H_INCLUDED_

// Wave tracing is currently a Windows-only runtime.  Every non-Windows build
// therefore selects the zero-standard-header API surface directly from the
// platform, without relying on target-specific CMake definitions.  The
// traditional include guard is intentional in addition to #pragma once: the
// package may still be reached through different physical/canonical paths.
#if !defined(_WIN32)

// Linux API-only compatibility surface. This header deliberately includes no
// C or C++ library headers, so placing it in a GCC/distcc PCH cannot preload
// libstdc++ declarations. It preserves business-code syntax and ordinary
// simulation behavior, but performs no waveform bookkeeping.

namespace reflect {
template <typename T>
inline const void* type_tag_of() noexcept {
    static const char tag = 0;
    return &tag;
}
} // namespace reflect

namespace wave {

typedef __SIZE_TYPE__ size_type;
typedef __PTRDIFF_TYPE__ difference_type;
typedef __UINT32_TYPE__ uint32_type;
typedef __UINT64_TYPE__ NodeId;

namespace compat_detail {

template <bool Enabled, typename T = void>
struct enable_if {};

template <typename T>
struct enable_if<true, T> { typedef T type; };

template <typename T>
struct is_lvalue_reference { static constexpr bool value = false; };

template <typename T>
struct is_lvalue_reference<T&> { static constexpr bool value = true; };

template <typename T>
struct remove_reference { typedef T type; };

template <typename T>
struct remove_reference<T&> { typedef T type; };

template <typename T>
struct remove_reference<T&&> { typedef T type; };

template <typename T>
inline typename remove_reference<T>::type&& move(T&& value) noexcept {
    return static_cast<typename remove_reference<T>::type&&>(value);
}

template <typename PointerT, typename ReferenceT, typename DifferenceT>
class reverse_iterator {
public:
    typedef PointerT pointer;
    typedef ReferenceT reference;
    typedef DifferenceT difference_type;

    reverse_iterator() noexcept : current_() {}
    explicit reverse_iterator(pointer current) noexcept : current_(current) {}

    template <typename OtherPointer, typename OtherReference>
    reverse_iterator(const reverse_iterator<OtherPointer, OtherReference, DifferenceT>& other) noexcept
        : current_(other.base()) {}

    pointer base() const noexcept { return current_; }
    reference operator*() const noexcept { pointer previous = current_; return *--previous; }
    pointer operator->() const noexcept { pointer previous = current_; return --previous; }
    reference operator[](difference_type offset) const noexcept { return *(*this + offset); }

    reverse_iterator& operator++() noexcept { --current_; return *this; }
    reverse_iterator operator++(int) noexcept { reverse_iterator old(*this); --current_; return old; }
    reverse_iterator& operator--() noexcept { ++current_; return *this; }
    reverse_iterator operator--(int) noexcept { reverse_iterator old(*this); ++current_; return old; }
    reverse_iterator& operator+=(difference_type offset) noexcept { current_ -= offset; return *this; }
    reverse_iterator& operator-=(difference_type offset) noexcept { current_ += offset; return *this; }

    reverse_iterator operator+(difference_type offset) const noexcept {
        reverse_iterator result(*this); result += offset; return result;
    }
    reverse_iterator operator-(difference_type offset) const noexcept {
        reverse_iterator result(*this); result -= offset; return result;
    }

private:
    pointer current_;
};

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline bool operator==(const reverse_iterator<LP, LR, D>& lhs,
                       const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return lhs.base() == rhs.base();
}

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline bool operator!=(const reverse_iterator<LP, LR, D>& lhs,
                       const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return !(lhs == rhs);
}

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline bool operator<(const reverse_iterator<LP, LR, D>& lhs,
                      const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return rhs.base() < lhs.base();
}

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline bool operator>(const reverse_iterator<LP, LR, D>& lhs,
                      const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return rhs < lhs;
}

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline bool operator<=(const reverse_iterator<LP, LR, D>& lhs,
                       const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return !(rhs < lhs);
}

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline bool operator>=(const reverse_iterator<LP, LR, D>& lhs,
                       const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return !(lhs < rhs);
}

template <typename LP, typename LR, typename RP, typename RR, typename D>
inline D operator-(const reverse_iterator<LP, LR, D>& lhs,
                   const reverse_iterator<RP, RR, D>& rhs) noexcept {
    return rhs.base() - lhs.base();
}

template <typename P, typename R, typename D>
inline reverse_iterator<P, R, D> operator+(D offset,
                                           const reverse_iterator<P, R, D>& value) noexcept {
    return value + offset;
}

} // namespace compat_detail

// API-only placeholders. Full builds use the concrete Tracer/std::string
// callbacks from reflect_macro.h; Linux compatibility builds never invoke an
// expander, but keeping these public names and virtual methods lets business
// interfaces compile unchanged without pulling a standard header into PCH.
typedef void (*DynamicExpandFn)();
typedef void (*DynamicExpandArrayFn)();

template <typename T> struct ReflectAccess;
struct ReflectFriendMarker {};

template <typename T>
struct GeneratedMemberNameTable {
    static constexpr bool generated = false;
    static const void* class_id() noexcept { return nullptr; }
    static const char* const* names() noexcept { return nullptr; }
    static size_type count() noexcept { return 0; }
};

struct WaveDirtyHook {
    typedef void (*MarkFn)(void*, uint32_type);

    void* tracer;
    uint32_type group_id;
    MarkFn mark_fn;

    WaveDirtyHook() noexcept : tracer(nullptr), group_id(~uint32_type(0)), mark_fn(nullptr) {}
    WaveDirtyHook(const WaveDirtyHook&) noexcept : tracer(nullptr), group_id(~uint32_type(0)), mark_fn(nullptr) {}
    WaveDirtyHook(WaveDirtyHook&&) noexcept : tracer(nullptr), group_id(~uint32_type(0)), mark_fn(nullptr) {}

    WaveDirtyHook& operator=(const WaveDirtyHook&) noexcept { clear(); return *this; }
    WaveDirtyHook& operator=(WaveDirtyHook&&) noexcept { clear(); return *this; }

    void clear() noexcept {
        tracer = nullptr;
        group_id = ~uint32_type(0);
        mark_fn = nullptr;
    }

    void bind(void* owner, uint32_type id, MarkFn fn) noexcept {
        tracer = owner;
        group_id = id;
        mark_fn = fn;
    }

    void mark_dirty() const noexcept {
        // Preserve harmless user-supplied hook behavior while WaveTrace itself
        // remains absent in Linux API-only builds.
        if (tracer && mark_fn && group_id != ~uint32_type(0)) mark_fn(tracer, group_id);
    }
};

struct DirectReflectPointerTarget {};

struct DynamicTraceTarget {
    virtual ~DynamicTraceTarget() {}
    virtual const void* wave_trace_target_ptr() const = 0;
    virtual const void* wave_trace_target_type_tag() const = 0;
    virtual uint32_type wave_trace_target_byte_width() const { return 0; }
    virtual WaveDirtyHook* wave_trace_dirty_hook() const { return nullptr; }
    virtual DynamicExpandFn wave_trace_dynamic_expander() const { return nullptr; }
};

template <typename T>
struct DynamicTraceTargetFor : DynamicTraceTarget {
protected:
    mutable WaveDirtyHook dynamic_trace_dirty_hook_;
public:
    const void* wave_trace_target_ptr() const override { return static_cast<const T*>(this); }
    const void* wave_trace_target_type_tag() const override { return reflect::type_tag_of<T>(); }
    uint32_type wave_trace_target_byte_width() const override { return static_cast<uint32_type>(sizeof(T)); }
    WaveDirtyHook* wave_trace_dirty_hook() const override { return wave_dirty_hook(); }
    WaveDirtyHook* wave_dirty_hook() const { return &dynamic_trace_dirty_hook_; }
};

struct PeekTraceSource {
    virtual ~PeekTraceSource() {}
    virtual const void* wave_trace_peek_ptr() const = 0;
    virtual const void* wave_trace_peek_type_tag() const = 0;
    virtual uint32_type wave_trace_peek_byte_width() const { return 0; }
    virtual WaveDirtyHook* wave_trace_peek_dirty_hook() const { return nullptr; }
    virtual DynamicExpandFn wave_trace_peek_dynamic_expander() const { return nullptr; }
};

template <typename DerivedT, typename ValueT>
struct PeekTraceSourceFor : PeekTraceSource {
    typedef ValueT wave_trace_peek_value_type;
protected:
    mutable WaveDirtyHook peek_trace_dirty_hook_;
public:
    const void* wave_trace_peek_ptr() const override {
        DerivedT* self = const_cast<DerivedT*>(static_cast<const DerivedT*>(this));
        return static_cast<const void*>(self->peek());
    }
    const void* wave_trace_peek_type_tag() const override { return reflect::type_tag_of<ValueT>(); }
    uint32_type wave_trace_peek_byte_width() const override { return static_cast<uint32_type>(sizeof(ValueT)); }
    DynamicExpandFn wave_trace_peek_dynamic_expander() const override { return nullptr; }
    WaveDirtyHook* wave_trace_peek_dirty_hook() const override { return wave_dirty_hook(); }
    WaveDirtyHook* wave_dirty_hook() const { return &peek_trace_dirty_hook_; }
};

template <typename T>
struct BoolStoragePtr { const T* ptr; };

template <typename T>
inline BoolStoragePtr<T> as_bool_storage_ptr(const T* ptr) noexcept {
    BoolStoragePtr<T> result = { ptr };
    return result;
}

template <typename T>
class WaveValue {
public:
    typedef T value_type;

    WaveValue() noexcept : value_() {}
    WaveValue(const T& value) noexcept : value_(value) {}
    WaveValue(const WaveValue&) noexcept = default;
    WaveValue& operator=(const WaveValue&) noexcept = default;
    WaveValue& operator=(const T& value) noexcept { value_ = value; return *this; }
    operator T() const noexcept { return value_; }
    const T& read() const noexcept { return value_; }
    T& raw_unsafe_for_initialization_only() noexcept { return value_; }
    WaveValue& operator+=(const T& value) noexcept { value_ = static_cast<T>(value_ + value); return *this; }
    WaveValue& operator-=(const T& value) noexcept { value_ = static_cast<T>(value_ - value); return *this; }
    WaveValue& operator*=(const T& value) noexcept { value_ = static_cast<T>(value_ * value); return *this; }
    WaveValue& operator/=(const T& value) noexcept { value_ = static_cast<T>(value_ / value); return *this; }
    WaveValue& operator%=(const T& value) noexcept { value_ = static_cast<T>(value_ % value); return *this; }
    WaveValue& operator&=(const T& value) noexcept { value_ = static_cast<T>(value_ & value); return *this; }
    WaveValue& operator|=(const T& value) noexcept { value_ = static_cast<T>(value_ | value); return *this; }
    WaveValue& operator^=(const T& value) noexcept { value_ = static_cast<T>(value_ ^ value); return *this; }
    WaveValue& operator<<=(int bits) noexcept { value_ = static_cast<T>(value_ << bits); return *this; }
    WaveValue& operator>>=(int bits) noexcept { value_ = static_cast<T>(value_ >> bits); return *this; }
    WaveValue& operator++() noexcept { value_ = static_cast<T>(value_ + T(1)); return *this; }
    T operator++(int) noexcept { T old = value_; ++(*this); return old; }
    WaveValue& operator--() noexcept { value_ = static_cast<T>(value_ - T(1)); return *this; }
    T operator--(int) noexcept { T old = value_; --(*this); return old; }
private:
    T value_;
};

template <typename T> struct wave_value_underlying { typedef T type; };
template <typename T> struct wave_value_underlying<WaveValue<T> > { typedef T type; };

typedef WaveValue<bool> WaveBool;
typedef WaveValue<char> WaveChar;
typedef WaveValue<__INT8_TYPE__> WaveI8;
typedef WaveValue<__UINT8_TYPE__> WaveU8;
typedef WaveValue<__INT16_TYPE__> WaveI16;
typedef WaveValue<__UINT16_TYPE__> WaveU16;
typedef WaveValue<__INT32_TYPE__> WaveI32;
typedef WaveValue<__UINT32_TYPE__> WaveU32;
typedef WaveValue<__INT64_TYPE__> WaveI64;
typedef WaveValue<__UINT64_TYPE__> WaveU64;
typedef WaveValue<float> WaveFloat;
typedef WaveValue<double> WaveDouble;

template <typename T, size_type N>
struct array {
    typedef T value_type;
    typedef wave::size_type size_type;
    typedef wave::difference_type difference_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T* iterator;
    typedef const T* const_iterator;
    typedef compat_detail::reverse_iterator<iterator, reference, difference_type> reverse_iterator;
    typedef compat_detail::reverse_iterator<const_iterator, const_reference, difference_type> const_reverse_iterator;

    // The one-slot N==0 representation is only a compile-compatibility corner
    // case; ordinary N>0 arrays retain T[N] layout and alignment.
    T storage_[N == 0 ? 1 : N];

    array() = default;

    array(const T (&values)[N == 0 ? 1 : N]) {
        for (size_type i = 0; i < N; ++i) storage_[i] = values[i];
    }

    // Keep the API-only Linux surface source-compatible with the full
    // wave::array without including <array> (or any other standard header) in
    // the GCC/distcc PCH.  In particular, business code commonly passes a
    // std::array<T,N> to an interface taking const wave::array<T,N>&.  The
    // dependent operator[] expression makes this constructor participate only
    // for fixed/indexable array-like objects; std::array does not need to be
    // declared here and is complete by the time the conversion is instantiated.
    template <typename ArrayLike,
              typename ElementReference = decltype(
                  (*static_cast<const ArrayLike*>(nullptr))[size_type(0)])>
    array(const ArrayLike& other) {
        assign_from_array_like_(other);
    }

    template <typename ArrayLike,
              typename compat_detail::enable_if<
                  !compat_detail::is_lvalue_reference<ArrayLike>::value,
                  int>::type = 0,
              typename ElementReference = decltype(
                  (*static_cast<ArrayLike*>(nullptr))[size_type(0)])>
    array(ArrayLike&& other) {
        move_from_array_like_(other);
    }

    template <typename ArrayLike>
    auto operator=(const ArrayLike& other)
        -> decltype(other[size_type(0)], *this) {
        assign_from_array_like_(other);
        return *this;
    }


    template <typename ArrayLike,
              typename compat_detail::enable_if<
                  !compat_detail::is_lvalue_reference<ArrayLike>::value,
                  int>::type = 0>
    auto operator=(ArrayLike&& other)
        -> decltype(other[size_type(0)], *this) {
        move_from_array_like_(other);
        return *this;
    }

    T& operator[](size_type index) noexcept { return storage_[index]; }
    const T& operator[](size_type index) const noexcept { return storage_[index]; }
    T& at(size_type index) noexcept { return storage_[index]; }
    const T& at(size_type index) const noexcept { return storage_[index]; }
    T& front() noexcept { return storage_[0]; }
    const T& front() const noexcept { return storage_[0]; }
    T& back() noexcept { return storage_[N - 1]; }
    const T& back() const noexcept { return storage_[N - 1]; }
    const T& read(size_type index) const noexcept { return storage_[index]; }
    const array& read() const noexcept { return *this; }
    constexpr size_type size() const noexcept { return N; }
    constexpr size_type max_size() const noexcept { return N; }
    constexpr bool empty() const noexcept { return N == 0; }
    T* data() noexcept { return storage_; }
    const T* data() const noexcept { return storage_; }
    iterator begin() noexcept { return storage_; }
    const_iterator begin() const noexcept { return storage_; }
    const_iterator cbegin() const noexcept { return storage_; }
    iterator end() noexcept { return storage_ + N; }
    const_iterator end() const noexcept { return storage_ + N; }
    const_iterator cend() const noexcept { return storage_ + N; }
    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }
    void fill(const T& value) { for (size_type i = 0; i < N; ++i) storage_[i] = value; }
    void swap(array& other) noexcept {
        for (size_type i = 0; i < N; ++i) {
            T temporary = static_cast<T&&>(storage_[i]);
            storage_[i] = static_cast<T&&>(other.storage_[i]);
            other.storage_[i] = static_cast<T&&>(temporary);
        }
    }
    array* operator&() noexcept { return this; }
    const array* operator&() const noexcept { return this; }

    // The full implementation exposes its underlying std::array by const
    // reference.  The header-only Linux surface cannot name std::array without
    // importing a standard header into the PCH, so provide a checked value
    // conversion to any matching fixed/indexable target instead.  This keeps
    // calls taking const std::array<T,N>& source-compatible and avoids aliasing
    // one unrelated class layout as another.
    template <typename ArrayLike,
              typename ElementReference = decltype(
                  (*static_cast<ArrayLike*>(nullptr))[size_type(0)])>
    operator ArrayLike() const {
        ArrayLike result = {};
        copy_to_array_like_(result);
        return result;
    }

    operator T*() = delete;
    operator const T*() const = delete;

private:
    template <typename ArrayLike>
    void assign_from_array_like_(const ArrayLike& other) {
        // Reject dynamic containers and mismatched fixed arrays at compile
        // time.  This keeps the header independent of <type_traits>/<array>
        // without turning the convenience conversion into an unchecked read.
        static_assert(N == 0 || sizeof(ArrayLike) == sizeof(storage_),
                      "wave::array conversion requires matching fixed storage");
        static_assert(N == 0 || sizeof(other[size_type(0)]) == sizeof(T),
                      "wave::array conversion requires matching element width");
        for (size_type i = 0; i < N; ++i) storage_[i] = other[i];
    }


    template <typename ArrayLike>
    void move_from_array_like_(ArrayLike& other) {
        static_assert(N == 0 || sizeof(ArrayLike) == sizeof(storage_),
                      "wave::array move conversion requires matching fixed storage");
        static_assert(N == 0 || sizeof(other[size_type(0)]) == sizeof(T),
                      "wave::array move conversion requires matching element width");
        for (size_type i = 0; i < N; ++i) {
            storage_[i] = compat_detail::move(other[i]);
        }
    }

    template <typename ArrayLike>
    void copy_to_array_like_(ArrayLike& other) const {
        static_assert(N == 0 || sizeof(ArrayLike) == sizeof(storage_),
                      "wave::array target conversion requires matching fixed storage");
        static_assert(N == 0 || sizeof(other[size_type(0)]) == sizeof(T),
                      "wave::array target conversion requires matching element width");
        for (size_type i = 0; i < N; ++i) other[i] = storage_[i];
    }
};

template <typename T, size_type N>
inline bool operator==(const array<T, N>& lhs, const array<T, N>& rhs) {
    for (size_type i = 0; i < N; ++i) if (!(lhs[i] == rhs[i])) return false;
    return true;
}
template <typename T, size_type N>
inline bool operator!=(const array<T, N>& lhs, const array<T, N>& rhs) { return !(lhs == rhs); }

template <typename T, size_type N>
inline bool operator<(const array<T, N>& lhs, const array<T, N>& rhs) {
    for (size_type i = 0; i < N; ++i) {
        if (lhs[i] < rhs[i]) return true;
        if (rhs[i] < lhs[i]) return false;
    }
    return false;
}

template <typename T, size_type N>
inline bool operator>(const array<T, N>& lhs, const array<T, N>& rhs) { return rhs < lhs; }

template <typename T, size_type N>
inline bool operator<=(const array<T, N>& lhs, const array<T, N>& rhs) { return !(rhs < lhs); }

template <typename T, size_type N>
inline bool operator>=(const array<T, N>& lhs, const array<T, N>& rhs) { return !(lhs < rhs); }

template <typename T, size_type N>
inline void swap(array<T, N>& lhs, array<T, N>& rhs) noexcept { lhs.swap(rhs); }

template <size_type I, typename T, size_type N>
inline T& get(array<T, N>& value) noexcept { static_assert(I < N, "wave::array index out of range"); return value[I]; }
template <size_type I, typename T, size_type N>
inline const T& get(const array<T, N>& value) noexcept { static_assert(I < N, "wave::array index out of range"); return value[I]; }

template <size_type I, typename T, size_type N>
inline T&& get(array<T, N>&& value) noexcept {
    static_assert(I < N, "wave::array index out of range");
    return compat_detail::move(value[I]);
}

template <size_type I, typename T, size_type N>
inline const T&& get(const array<T, N>&& value) noexcept {
    static_assert(I < N, "wave::array index out of range");
    return compat_detail::move(value[I]);
}

template <typename T> struct wave_array_traits;

template <typename T, size_type N>
struct wave_array_traits<array<T, N> > {
    typedef T element_type;
    static const size_type size = N;
};

} // namespace wave

#ifndef WAVE_REFLECT_FRIEND
#define WAVE_REFLECT_FRIEND
#endif
#ifndef WAVE_PTR
#define WAVE_PTR
#endif
#ifndef WAVE_PTR_ARRAY
#define WAVE_PTR_ARRAY(count)
#endif
#ifndef WAVE_NO_TRACE
#define WAVE_NO_TRACE
#endif
#ifndef WAVE_REFLECT_MARKED_FRIEND
#define WAVE_REFLECT_MARKED_FRIEND
#endif
#ifndef WAVE_TRACE_PRIVATE
#define WAVE_TRACE_PRIVATE
#endif

#else

// Lightweight public surface for business model headers.
// Include this header instead of wave_runtime.h when a business type only needs
// reflection opt-in macros, WaveValue<T>, wave::array<T,N>, or WaveDirtyHook.

#include "wavetrace_config.h"

#if defined(min)
#pragma push_macro("min")
#undef min
#define REFLECT_MACRO_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define REFLECT_MACRO_RESTORE_MAX_MACRO_ 1
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace reflect {

template <typename T>
inline const void* type_tag_of() {
    typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type RawT;
    static int tag_for_type = 0;
    (void)sizeof(RawT*);
    return &tag_for_type;
}

} // namespace reflect

namespace wave {

class Tracer;
template <typename T, std::size_t N> class array;
typedef std::uint64_t NodeId;
typedef NodeId (*DynamicExpandFn)(Tracer&, const std::string&, NodeId, const void*);
typedef bool (*DynamicExpandArrayFn)(Tracer&, NodeId, const void*, std::size_t);

template <typename T, typename RegistrationPolicy>
NodeId dynamic_expand_policy_bridge(Tracer& tracer, const std::string& path, NodeId parent_id, const void* obj);

template <typename T>
NodeId dynamic_expand_fallback_bridge(Tracer& tracer, const std::string& path, NodeId parent_id, const void* obj);

template <typename T, typename RegistrationPolicy>
bool dynamic_expand_array_policy_bridge(Tracer& tracer,
                                        NodeId parent_id,
                                        const void* objects,
                                        std::size_t count);

static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

template <typename T>
struct GeneratedMemberNameTable {
    static constexpr bool generated = false;
    static const void* class_id() noexcept { return NULL; }
    static const char* const* names() noexcept { return NULL; }
    static std::size_t count() noexcept { return 0; }
};

struct WaveDirtyHook {
    typedef void (*MarkFn)(void*, std::uint32_t);

    void* tracer;
    std::uint32_t group_id;
    MarkFn mark_fn;

    WaveDirtyHook() noexcept : tracer(NULL), group_id(kInvalidIndex), mark_fn(NULL) {}
    WaveDirtyHook(const WaveDirtyHook&) noexcept : tracer(NULL), group_id(kInvalidIndex), mark_fn(NULL) {}
    WaveDirtyHook(WaveDirtyHook&&) noexcept : tracer(NULL), group_id(kInvalidIndex), mark_fn(NULL) {}

    WaveDirtyHook& operator=(const WaveDirtyHook&) noexcept {
        clear();
        return *this;
    }

    WaveDirtyHook& operator=(WaveDirtyHook&&) noexcept {
        clear();
        return *this;
    }

    void clear() noexcept {
        tracer = NULL;
        group_id = kInvalidIndex;
        mark_fn = NULL;
    }

    void bind(void* t, std::uint32_t gid, MarkFn fn) noexcept {
        tracer = t;
        group_id = gid;
        mark_fn = fn;
    }

    void mark_dirty() const noexcept {
        if (tracer && mark_fn && group_id != kInvalidIndex) {
            mark_fn(tracer, group_id);
        }
    }
};

// Business opt-in marker: pointers/smart pointers to types derived from this
// marker may be expanded directly by wave_runtime.h.  No allocation tracking is
// performed, so the business model owns lifetime correctness.
struct DirectReflectPointerTarget {};

// Runtime-typed opt-in marker for interface pointers such as sc_port<IF>.
// A concrete object can inherit DynamicTraceTargetFor<ConcreteT>; wave_runtime.h
// can then recover the concrete target from an IF* through dynamic_cast and
// expand the registered ConcreteT reflection tree.
struct DynamicTraceTarget {
    virtual ~DynamicTraceTarget() {}
    virtual const void* wave_trace_target_ptr() const = 0;
    virtual const void* wave_trace_target_type_tag() const = 0;
    virtual std::uint32_t wave_trace_target_byte_width() const { return 0u; }
    virtual WaveDirtyHook* wave_trace_dirty_hook() const { return NULL; }
    virtual DynamicExpandFn wave_trace_dynamic_expander() const { return NULL; }
};

template <typename T>
struct DynamicTraceTargetFor : DynamicTraceTarget {
protected:
    mutable WaveDirtyHook dynamic_trace_dirty_hook_;

public:
    const void* wave_trace_target_ptr() const override {
        return static_cast<const T*>(this);
    }

    const void* wave_trace_target_type_tag() const override {
        return reflect::type_tag_of<T>();
    }

    std::uint32_t wave_trace_target_byte_width() const override {
        return static_cast<std::uint32_t>(sizeof(T));
    }

    WaveDirtyHook* wave_trace_dirty_hook() const override {
        return wave_dirty_hook();
    }

    WaveDirtyHook* wave_dirty_hook() const {
        return &dynamic_trace_dirty_hook_;
    }
};

// Runtime-typed opt-in marker for interface/channel objects whose sampled value
// is exposed through peek().  Business code should inherit this only on objects
// that are meant to expand through peek(); merely inheriting a project interface
// such as vsii* is intentionally not enough.
struct PeekTraceSource {
    virtual ~PeekTraceSource() {}
    virtual const void* wave_trace_peek_ptr() const = 0;
    virtual const void* wave_trace_peek_type_tag() const = 0;
    virtual std::uint32_t wave_trace_peek_byte_width() const { return 0u; }
    virtual WaveDirtyHook* wave_trace_peek_dirty_hook() const { return NULL; }
    virtual DynamicExpandFn wave_trace_peek_dynamic_expander() const { return NULL; }
};

template <typename DerivedT, typename ValueT>
struct PeekTraceSourceFor : PeekTraceSource {
    typedef ValueT wave_trace_peek_value_type;

protected:
    mutable WaveDirtyHook peek_trace_dirty_hook_;

public:
    const void* wave_trace_peek_ptr() const override {
        const DerivedT* self = static_cast<const DerivedT*>(this);
        const ValueT* value = const_cast<DerivedT*>(self)->peek();
        return static_cast<const void*>(value);
    }

    const void* wave_trace_peek_type_tag() const override {
        return reflect::type_tag_of<ValueT>();
    }

    std::uint32_t wave_trace_peek_byte_width() const override {
        return static_cast<std::uint32_t>(sizeof(ValueT));
    }

    DynamicExpandFn wave_trace_peek_dynamic_expander() const override {
        return NULL;
    }

    WaveDirtyHook* wave_trace_peek_dirty_hook() const override {
        return wave_dirty_hook();
    }

    WaveDirtyHook* wave_dirty_hook() const {
        return &peek_trace_dirty_hook_;
    }
};

// Generated reflection code places private/protected member access in this
// template.  Business classes that want private reflection should write:
//   WAVE_REFLECT_FRIEND
// inside the class body.
template <typename T>
struct ReflectAccess;

// AST-visible marker used by ReflectGen.  It is emitted as a class-scope type
// alias by WAVE_REFLECT_FRIEND, so it has no storage, no object-layout impact,
// no ODR-sensitive static data member, and it cannot be collected as an
// instance field.
struct ReflectFriendMarker {};

// Logical-bool storage marker used by generated reflection code for typedefs
// such as U01 that are physically stored in one byte but should be exported as
// a one-bit Bool signal.  The raw address is retained so runtime memory-block
// precheck can still compare the original storage bytes.
template <typename T>
struct BoolStoragePtr {
    const T* ptr;
};

template <typename T>
inline BoolStoragePtr<T> as_bool_storage_ptr(const T* p) noexcept {
    BoolStoragePtr<T> out = { p };
    return out;
}

namespace detail {

struct UnionFieldTag {};

struct GeneratedMemberId {
    GeneratedMemberId() : class_id(NULL), member_id(kInvalidIndex) {}
    GeneratedMemberId(const void* class_id_in, std::uint32_t member_id_in)
        : class_id(class_id_in), member_id(member_id_in) {}
    const void* class_id;
    std::uint32_t member_id;
};

// Runtime identity for a WAVE_PTR/WAVE_PTR_ARRAY member. Unlike
// GeneratedMemberId this key is also forwarded through fixed pointer-slot
// arrays, where every indexed child must share the owning member's JSON switch.
struct AnnotatedPointerMemberKey {
    AnnotatedPointerMemberKey() : class_name(NULL), member_name(NULL) {}
    AnnotatedPointerMemberKey(const char* class_name_in, const char* member_name_in)
        : class_name(class_name_in), member_name(member_name_in) {}
    bool valid() const noexcept {
        return class_name != NULL && class_name[0] != '\0' &&
               member_name != NULL && member_name[0] != '\0';
    }
    const char* class_name;
    const char* member_name;
};

// Pointer-storage arrays (for example `T* slots[N]`,
// `std::array<std::shared_ptr<T>, N>`, or nested combinations) are expanded by
// the generated visitor before Tracer sees an AnnotatedWavePtrView.  Wrap only
// the reflected pointer callback with the current limit policy: ordinary
// reflected objects pay no TLS/global-state cost, and nested/parallel tracers
// cannot leak policy into one another.
template <typename Visitor>
class AnnotatedPointerStorageArrayPolicyVisitor {
public:
    AnnotatedPointerStorageArrayPolicyVisitor(Visitor visitor, bool first_only)
        : visitor_(std::move(visitor)), first_only_(first_only) {}

    template <typename... Args>
    decltype(auto) operator()(Args&&... args) {
        return visitor_(std::forward<Args>(args)...);
    }

    std::size_t annotated_pointer_storage_array_trace_count(
        std::size_t logical_count) const noexcept {
        return first_only_ && logical_count != 0u ? 1u : logical_count;
    }

    std::string annotated_pointer_storage_array_child_name(
        const std::string& base,
        std::size_t index,
        std::size_t logical_count) const {
        if (!first_only_) {
            return base + "[" + std::to_string(index) + "]";
        }
        return base + "[size=" + std::to_string(logical_count) +
               "].[" + std::to_string(index) + "]";
    }

private:
    Visitor visitor_;
    bool first_only_;
};

template <typename Visitor>
AnnotatedPointerStorageArrayPolicyVisitor<
    typename std::decay<Visitor>::type>
make_annotated_pointer_storage_array_policy_visitor(
    bool first_only, Visitor&& visitor) {
    typedef typename std::decay<Visitor>::type CleanVisitor;
    return AnnotatedPointerStorageArrayPolicyVisitor<CleanVisitor>(
        std::forward<Visitor>(visitor), first_only);
}

template <typename Visitor>
auto annotated_pointer_storage_array_trace_count_impl(
    int, Visitor& visitor, std::size_t logical_count)
    -> decltype(visitor.annotated_pointer_storage_array_trace_count(logical_count)) {
    return visitor.annotated_pointer_storage_array_trace_count(logical_count);
}

template <typename Visitor>
std::size_t annotated_pointer_storage_array_trace_count_impl(
    long, Visitor&, std::size_t logical_count) noexcept {
    return logical_count;
}

template <typename Visitor>
std::size_t annotated_pointer_storage_array_trace_count(
    Visitor& visitor, std::size_t logical_count) {
    return annotated_pointer_storage_array_trace_count_impl(
        0, visitor, logical_count);
}

inline AnnotatedPointerMemberKey annotated_pointer_member_key_or_invalid() {
    return AnnotatedPointerMemberKey();
}

template <typename... Rest>
inline AnnotatedPointerMemberKey annotated_pointer_member_key_or_invalid(
    AnnotatedPointerMemberKey key, Rest&&...) {
    return key;
}

template <typename First, typename... Rest>
inline AnnotatedPointerMemberKey annotated_pointer_member_key_or_invalid(
    First&&, Rest&&... rest) {
    return annotated_pointer_member_key_or_invalid(std::forward<Rest>(rest)...);
}

struct UnionFieldBase {
    UnionFieldBase() : ptr(nullptr) {}
    explicit UnionFieldBase(const void* p) : ptr(p) {}
    const void* ptr;
};

template <typename Visitor, typename Ptr, typename... Meta>
auto invoke_ptr_visitor_impl(int, Visitor&& visitor, const char* name, Ptr ptr, Meta&&... meta)
    -> decltype(visitor(name, ptr, std::forward<Meta>(meta)...), void()) {
    visitor(name, ptr, std::forward<Meta>(meta)...);
}

template <typename Visitor, typename Ptr, typename... Meta>
void invoke_ptr_visitor_impl(long, Visitor&& visitor, const char* name, Ptr ptr, Meta&&...) {
    visitor(name, ptr);
}

template <typename Visitor, typename Ptr, typename... Meta>
void invoke_ptr_visitor(Visitor&& visitor, const char* name, Ptr ptr, Meta&&... meta) {
    invoke_ptr_visitor_impl(0,
                            std::forward<Visitor>(visitor),
                            name,
                            ptr,
                            std::forward<Meta>(meta)...);
}

template <typename Visitor, typename Getter, typename... Meta>
auto invoke_getter_visitor_impl(int, Visitor&& visitor, const char* name, Getter getter, Meta&&... meta)
    -> decltype(visitor(name, getter, std::forward<Meta>(meta)...), void()) {
    visitor(name, getter, std::forward<Meta>(meta)...);
}

template <typename Visitor, typename Getter, typename... Meta>
void invoke_getter_visitor_impl(long, Visitor&& visitor, const char* name, Getter getter, Meta&&...) {
    visitor(name, getter);
}

template <typename Visitor, typename Getter, typename... Meta>
void invoke_getter_visitor(Visitor&& visitor, const char* name, Getter getter, Meta&&... meta) {
    invoke_getter_visitor_impl(0,
                               std::forward<Visitor>(visitor),
                               name,
                               getter,
                               std::forward<Meta>(meta)...);
}

template <typename T>
struct is_bool_storage_ptr : std::false_type {};

template <typename T>
struct is_bool_storage_ptr< ::wave::BoolStoragePtr<T> > : std::true_type {};

template <typename T>
struct bool_storage_ptr_value_type;

template <typename T>
struct bool_storage_ptr_value_type< ::wave::BoolStoragePtr<T> > {
    typedef T type;
};


typedef void (*WaveValueNotifyFn)(const void*);
typedef void (*WaveArrayIndexNotifyFn)(std::size_t, const void*, const void*, std::size_t);
typedef bool (*WaveArrayBulkNotifyFn)(const void*, const void*, std::size_t, std::size_t);
typedef std::uint64_t (*WaveArrayBulkNotifyEpochFn)();

struct WaveArrayBulkNotifyKey {
    const void* first_element_address;
    const void* element_type_tag;
    std::size_t element_size;
    std::size_t element_count;

    WaveArrayBulkNotifyKey()
        : first_element_address(NULL), element_type_tag(NULL), element_size(0), element_count(0) {}

    WaveArrayBulkNotifyKey(const void* address,
                           const void* type_tag,
                           std::size_t size,
                           std::size_t count)
        : first_element_address(address),
          element_type_tag(type_tag),
          element_size(size),
          element_count(count) {}

    bool operator==(const WaveArrayBulkNotifyKey& other) const noexcept {
        return first_element_address == other.first_element_address &&
               element_type_tag == other.element_type_tag &&
               element_size == other.element_size &&
               element_count == other.element_count;
    }
};

struct WaveArrayBulkNotifyKeyHash {
    std::size_t operator()(const WaveArrayBulkNotifyKey& key) const noexcept {
        std::size_t h = std::hash<const void*>()(key.first_element_address);
        h ^= std::hash<const void*>()(key.element_type_tag) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::size_t>()(key.element_size) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::size_t>()(key.element_count) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct WaveArrayBulkNotifyDedupeTls {
    std::uint64_t epoch_token;
    std::unordered_set<WaveArrayBulkNotifyKey, WaveArrayBulkNotifyKeyHash> seen;

    WaveArrayBulkNotifyDedupeTls() : epoch_token(0) {}
};

inline bool wave_notifications_enabled() noexcept {
    static const bool enabled = []() noexcept {
        try {
            return ::wave::config::enabled();
        } catch (...) {
            return false;
        }
    }();
    return enabled;
}

inline WaveArrayBulkNotifyDedupeTls& wave_array_bulk_notify_dedupe_tls() {
    thread_local WaveArrayBulkNotifyDedupeTls tls;
    return tls;
}

#if defined(_WIN32)
inline FARPROC resolve_wave_runtime_export(const char* name) noexcept {
    if (!name || !name[0]) return NULL;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return NULL;

    MODULEENTRY32 entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    FARPROC proc = NULL;
    if (Module32First(snapshot, &entry)) {
        do {
            if (!entry.hModule) continue;
            proc = GetProcAddress(entry.hModule, name);
            if (proc) break;
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return proc;
}

inline bool should_retry_wave_runtime_export_resolve(unsigned miss_count) noexcept {
    return miss_count < 16u || (miss_count & (miss_count - 1u)) == 0u;
}

inline WaveValueNotifyFn exported_wave_value_notify_fn() noexcept {
    static WaveValueNotifyFn fn = NULL;
    static unsigned miss_count = 0;
    if (!fn && should_retry_wave_runtime_export_resolve(miss_count++)) {
        fn = reinterpret_cast<WaveValueNotifyFn>(resolve_wave_runtime_export("WaveTrace_wave_value_notify"));
    }
    return fn;
}

inline WaveArrayIndexNotifyFn exported_wave_array_index_notify_fn() noexcept {
    static WaveArrayIndexNotifyFn fn = NULL;
    static unsigned miss_count = 0;
    if (!fn && should_retry_wave_runtime_export_resolve(miss_count++)) {
        fn = reinterpret_cast<WaveArrayIndexNotifyFn>(resolve_wave_runtime_export("WaveTrace_wave_array_index_notify"));
    }
    return fn;
}

inline WaveArrayBulkNotifyFn exported_wave_array_bulk_notify_fn() noexcept {
    static WaveArrayBulkNotifyFn fn = NULL;
    static unsigned miss_count = 0;
    if (!fn && should_retry_wave_runtime_export_resolve(miss_count++)) {
        fn = reinterpret_cast<WaveArrayBulkNotifyFn>(resolve_wave_runtime_export("WaveTrace_wave_array_bulk_notify"));
    }
    return fn;
}

inline WaveArrayBulkNotifyEpochFn exported_wave_array_bulk_notify_epoch_fn() noexcept {
    static WaveArrayBulkNotifyEpochFn fn = NULL;
    static unsigned miss_count = 0;
    if (!fn && should_retry_wave_runtime_export_resolve(miss_count++)) {
        fn = reinterpret_cast<WaveArrayBulkNotifyEpochFn>(resolve_wave_runtime_export("WaveTrace_wave_array_bulk_notify_epoch"));
    }
    return fn;
}
#endif

inline WaveValueNotifyFn& wave_value_notify_slot() noexcept {
    static WaveValueNotifyFn fn = NULL;
    return fn;
}

inline WaveArrayIndexNotifyFn& wave_array_index_notify_slot() noexcept {
    static WaveArrayIndexNotifyFn fn = NULL;
    return fn;
}

inline WaveArrayBulkNotifyFn& wave_array_bulk_notify_slot() noexcept {
    static WaveArrayBulkNotifyFn fn = NULL;
    return fn;
}

inline WaveArrayBulkNotifyEpochFn& wave_array_bulk_notify_epoch_slot() noexcept {
    static WaveArrayBulkNotifyEpochFn fn = NULL;
    return fn;
}

inline void set_wave_value_notify_fn(WaveValueNotifyFn fn) noexcept {
    wave_value_notify_slot() = fn;
}

inline void set_wave_array_index_notify_fn(WaveArrayIndexNotifyFn fn) noexcept {
    wave_array_index_notify_slot() = fn;
}

inline void set_wave_array_bulk_notify_fn(WaveArrayBulkNotifyFn fn) noexcept {
    wave_array_bulk_notify_slot() = fn;
}

inline void set_wave_array_bulk_notify_epoch_fn(WaveArrayBulkNotifyEpochFn fn) noexcept {
    wave_array_bulk_notify_epoch_slot() = fn;
}

inline std::uint64_t current_wave_array_bulk_notify_epoch() noexcept {
    WaveArrayBulkNotifyEpochFn fn = wave_array_bulk_notify_epoch_slot();
    if (fn) return fn();
#if defined(_WIN32)
    fn = exported_wave_array_bulk_notify_epoch_fn();
    if (fn) return fn();
#endif
    return 0;
}

inline bool wave_array_bulk_notify_seen_this_epoch(const void* first_element_address,
                                                   const void* element_type_tag,
                                                   std::size_t element_size,
                                                   std::size_t element_count) noexcept {
    const std::uint64_t epoch = current_wave_array_bulk_notify_epoch();
    if (epoch == 0) return false;
    try {
        WaveArrayBulkNotifyDedupeTls& tls = wave_array_bulk_notify_dedupe_tls();
        if (tls.epoch_token != epoch) {
            tls.epoch_token = epoch;
            tls.seen.clear();
        }
        const WaveArrayBulkNotifyKey key(first_element_address, element_type_tag, element_size, element_count);
        return tls.seen.find(key) != tls.seen.end();
    } catch (...) {
        return false;
    }
}

inline void remember_wave_array_bulk_notify_this_epoch(const void* first_element_address,
                                                       const void* element_type_tag,
                                                       std::size_t element_size,
                                                       std::size_t element_count) noexcept {
    const std::uint64_t epoch = current_wave_array_bulk_notify_epoch();
    if (epoch == 0) return;
    try {
        WaveArrayBulkNotifyDedupeTls& tls = wave_array_bulk_notify_dedupe_tls();
        if (tls.epoch_token != epoch) {
            tls.epoch_token = epoch;
            tls.seen.clear();
        }
        tls.seen.insert(WaveArrayBulkNotifyKey(first_element_address, element_type_tag, element_size, element_count));
    } catch (...) {
    }
}

template <typename T>
struct is_wave_value_allowed : std::integral_constant<bool,
    (std::is_arithmetic<T>::value || std::is_enum<T>::value) &&
    !std::is_pointer<T>::value
> {};

template <typename T>
struct is_wave_value : std::false_type {};

template <typename T> struct is_wave_array : std::false_type {};

template <typename T> struct is_wave_ptr : std::false_type {};

} // namespace detail

inline void notify_wave_value_write_address(const void* address) noexcept {
    if (!detail::wave_notifications_enabled()) return;
    detail::WaveValueNotifyFn fn = detail::wave_value_notify_slot();
    if (fn) {
        fn(address);
        return;
    }
#if defined(_WIN32)
    fn = detail::exported_wave_value_notify_fn();
    if (fn) fn(address);
#endif
}

inline void notify_wave_array_index_access(std::size_t index,
                                           const void* element_address,
                                             const void* element_type_tag,
                                             std::size_t element_size) noexcept {
    if (!detail::wave_notifications_enabled()) return;
    detail::WaveArrayIndexNotifyFn fn = detail::wave_array_index_notify_slot();
    if (fn) {
        fn(index, element_address, element_type_tag, element_size);
        return;
    }
#if defined(_WIN32)
    fn = detail::exported_wave_array_index_notify_fn();
    if (fn) fn(index, element_address, element_type_tag, element_size);
#endif
}

inline bool notify_wave_array_all_elements_access(const void* first_element_address,
                                                   const void* element_type_tag,
                                                   std::size_t element_size,
                                                   std::size_t element_count) noexcept {
    if (!detail::wave_notifications_enabled()) return true;
    detail::WaveArrayBulkNotifyFn fn = detail::wave_array_bulk_notify_slot();
    if (fn) {
        if (detail::wave_array_bulk_notify_seen_this_epoch(first_element_address, element_type_tag, element_size, element_count)) return true;
        if (fn(first_element_address, element_type_tag, element_size, element_count)) {
            detail::remember_wave_array_bulk_notify_this_epoch(first_element_address, element_type_tag, element_size, element_count);
            return true;
        }
        return false;
    }
#if defined(_WIN32)
    fn = detail::exported_wave_array_bulk_notify_fn();
    if (fn) {
        if (detail::wave_array_bulk_notify_seen_this_epoch(first_element_address, element_type_tag, element_size, element_count)) return true;
        if (fn(first_element_address, element_type_tag, element_size, element_count)) {
            detail::remember_wave_array_bulk_notify_this_epoch(first_element_address, element_type_tag, element_size, element_count);
            return true;
        }
        return false;
    }
#endif
    return true;
}

inline void fail_wave_array_bulk_notify(const void* first_element_address,
                                        const void* element_type_tag,
                                        std::size_t element_size,
                                        std::size_t element_count,
                                        const char* element_type_name,
                                        const char* function_signature,
                                        const char* reason) noexcept {
    if (FILE* fp = std::fopen("wave_runtime_error.log", "ab")) {
        std::fprintf(fp,
                     "[wave] fatal wave::array bulk dirty notify failed in reflect header "
                     "reason=%s first=%p type_tag=%p element_size=%llu element_count=%llu "
                     "element_type=%s caller=%s\n",
                     reason ? reason : "unknown",
                     first_element_address,
                     element_type_tag,
                     static_cast<unsigned long long>(element_size),
                     static_cast<unsigned long long>(element_count),
                     element_type_name ? element_type_name : "(unknown)",
                     function_signature ? function_signature : "(unknown)");
        std::fclose(fp);
    }
    std::fprintf(stderr,
                 "[wave] fatal: wave::array bulk dirty notify failed "
                 "reason=%s first=%p type_tag=%p element_size=%llu element_count=%llu "
                 "element_type=%s caller=%s\n",
                 reason ? reason : "unknown",
                 first_element_address,
                 element_type_tag,
                 static_cast<unsigned long long>(element_size),
                 static_cast<unsigned long long>(element_count),
                 element_type_name ? element_type_name : "(unknown)",
                 function_signature ? function_signature : "(unknown)");
    std::fflush(stderr);
    std::abort();
}

// WaveValue<T> is a size-preserving scalar wrapper for write-driven waveform
// tracing.  It intentionally stores only the wrapped value; no tracer pointer,
// id, or metadata is kept in the object, so sizeof(WaveValue<T>) == sizeof(T).
// The runtime maps object addresses to dirty groups during topology expansion.
template <typename T>
class WaveValue {
    static_assert(detail::is_wave_value_allowed<T>::value,
                  "wave::WaveValue<T> only accepts arithmetic C++ scalar types and enum types");
public:
    typedef T value_type;

    WaveValue() noexcept : value_() {}
    WaveValue(const T& v) noexcept : value_(v) {}
    WaveValue(const WaveValue& other) noexcept : value_(other.value_) {}

    WaveValue& operator=(const WaveValue& other) noexcept { return assign_value(other.value_); }
    WaveValue& operator=(const T& v) noexcept { return assign_value(v); }

    operator T() const noexcept { return value_; }
    const T& read() const noexcept { return value_; }

    // Escape hatch for construction/deserialization code.  Mutating through this
    // reference bypasses dirty reporting and should not be used in normal model code.
    T& raw_unsafe_for_initialization_only() noexcept { return value_; }

    WaveValue& operator+=(const T& v) noexcept { return assign_value(static_cast<T>(value_ + v)); }
    WaveValue& operator-=(const T& v) noexcept { return assign_value(static_cast<T>(value_ - v)); }
    WaveValue& operator*=(const T& v) noexcept { return assign_value(static_cast<T>(value_ * v)); }
    WaveValue& operator/=(const T& v) noexcept { return assign_value(static_cast<T>(value_ / v)); }

    template <typename U = T>
    typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, WaveValue&>::type
    operator%=(const T& v) noexcept { return assign_value(static_cast<T>(value_ % v)); }

    template <typename U = T>
    typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, WaveValue&>::type
    operator&=(const T& v) noexcept { return assign_value(static_cast<T>(value_ & v)); }

    template <typename U = T>
    typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, WaveValue&>::type
    operator|=(const T& v) noexcept { return assign_value(static_cast<T>(value_ | v)); }

    template <typename U = T>
    typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, WaveValue&>::type
    operator^=(const T& v) noexcept { return assign_value(static_cast<T>(value_ ^ v)); }

    template <typename U = T>
    typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, WaveValue&>::type
    operator<<=(int n) noexcept { return assign_value(static_cast<T>(value_ << n)); }

    template <typename U = T>
    typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, WaveValue&>::type
    operator>>=(int n) noexcept { return assign_value(static_cast<T>(value_ >> n)); }

    WaveValue& operator++() noexcept { return assign_value(static_cast<T>(value_ + static_cast<T>(1))); }
    T operator++(int) noexcept { T old = value_; assign_value(static_cast<T>(value_ + static_cast<T>(1))); return old; }
    WaveValue& operator--() noexcept { return assign_value(static_cast<T>(value_ - static_cast<T>(1))); }
    T operator--(int) noexcept { T old = value_; assign_value(static_cast<T>(value_ - static_cast<T>(1))); return old; }

private:
    WaveValue& assign_value(const T& v) noexcept {
        if (!same_value(value_, v)) {
            value_ = v;
            notify_wave_value_write_address(static_cast<const void*>(this));
        }
        return *this;
    }

    static bool same_value(const T& a, const T& b) noexcept {
        return std::memcmp(static_cast<const void*>(&a), static_cast<const void*>(&b), sizeof(T)) == 0;
    }

private:
    T value_;
};

template <typename T>
struct detail::is_wave_value<WaveValue<T> > : std::true_type {};

template <typename T>
struct wave_value_underlying { typedef T type; };

template <typename T>
struct wave_value_underlying<WaveValue<T> > { typedef T type; };

#define WAVE_DEFINE_WAVEVALUE_ALIAS(alias_name, raw_type) \
    typedef ::wave::WaveValue<raw_type> alias_name

WAVE_DEFINE_WAVEVALUE_ALIAS(WaveBool, bool);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveChar, char);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveI8, std::int8_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveU8, std::uint8_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveI16, std::int16_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveU16, std::uint16_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveI32, std::int32_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveU32, std::uint32_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveI64, std::int64_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveU64, std::uint64_t);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveFloat, float);
WAVE_DEFINE_WAVEVALUE_ALIAS(WaveDouble, double);

#undef WAVE_DEFINE_WAVEVALUE_ALIAS

static_assert(sizeof(WaveValue<std::uint32_t>) == sizeof(std::uint32_t), "WaveValue size mismatch");
static_assert(alignof(WaveValue<std::uint32_t>) == alignof(std::uint32_t), "WaveValue align mismatch");

template <typename WavePtrT>
struct wave_ptr_traits;

namespace detail {

// Generated reflection uses these short-lived views to opt an ordinary raw or
// smart pointer into the pointer-target topology path. They never become
// business-object members, so WAVE_PTR annotations do not change object layout.
template <typename T>
class AnnotatedWavePtrView {
public:
    typedef T element_type;

    AnnotatedWavePtrView(T* ptr,
                         std::size_t count,
                         bool array_semantics = false) noexcept
        : ptr_(ptr), count_(count), array_semantics_(array_semantics) {}

    T* get() const noexcept { return ptr_; }
    std::size_t declared_size() const noexcept { return count_; }
    bool array_semantics() const noexcept { return array_semantics_; }

private:
    T* ptr_;
    std::size_t count_;
    bool array_semantics_;
};

template <typename T>
class AnnotatedWeakPtrView {
public:
    typedef T element_type;

    AnnotatedWeakPtrView(std::shared_ptr<T> locked,
                         std::size_t count,
                         bool array_semantics = false) noexcept
        : locked_(std::move(locked)),
          count_(count),
          array_semantics_(array_semantics) {}

    T* get() const noexcept { return locked_.get(); }
    std::size_t declared_size() const noexcept { return count_; }
    bool array_semantics() const noexcept { return array_semantics_; }
    std::shared_ptr<const void> keepalive() const noexcept {
        return std::shared_ptr<const void>(locked_);
    }

private:
    std::shared_ptr<T> locked_;
    std::size_t count_;
    bool array_semantics_;
};

template <typename T>
struct is_wave_ptr<AnnotatedWavePtrView<T> > : std::true_type {};

template <typename T>
struct is_wave_ptr<AnnotatedWeakPtrView<T> > : std::true_type {};

template <typename StorageT>
struct annotated_pointer_storage_traits;

template <typename T>
struct annotated_pointer_storage_traits<T*> {
    typedef T element_type;
    static T* get(T* ptr) noexcept { return ptr; }
};

template <typename T, typename Deleter>
struct annotated_pointer_storage_traits<std::unique_ptr<T, Deleter> > {
    typedef typename std::unique_ptr<T, Deleter>::element_type element_type;
    static element_type* get(const std::unique_ptr<T, Deleter>& ptr) noexcept {
        return ptr.get();
    }
};

template <typename T>
struct annotated_pointer_storage_traits<std::shared_ptr<T> > {
    typedef typename std::shared_ptr<T>::element_type element_type;
    static element_type* get(const std::shared_ptr<T>& ptr) noexcept { return ptr.get(); }
};

template <typename T>
struct annotated_weak_pointer_storage_traits;

template <typename T>
struct annotated_weak_pointer_storage_traits<std::weak_ptr<T> > {
    typedef T element_type;
    static std::shared_ptr<T> lock(const std::weak_ptr<T>& ptr) noexcept {
        return ptr.lock();
    }
};

template <typename StorageT>
struct annotated_pointer_storage_array_traits {
    typedef typename annotated_pointer_storage_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count = 1u;
};

template <typename StorageT, std::size_t N>
struct annotated_pointer_storage_array_traits<StorageT[N]> {
    typedef typename annotated_pointer_storage_array_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count =
        N * annotated_pointer_storage_array_traits<StorageT>::slot_count;
};

template <typename StorageT, std::size_t N>
struct annotated_pointer_storage_array_traits<std::array<StorageT, N> > {
    typedef typename annotated_pointer_storage_array_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count =
        N * annotated_pointer_storage_array_traits<StorageT>::slot_count;
};

template <typename StorageT, std::size_t N>
struct annotated_pointer_storage_array_traits< ::wave::array<StorageT, N> > {
    typedef typename annotated_pointer_storage_array_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count =
        N * annotated_pointer_storage_array_traits<StorageT>::slot_count;
};

template <typename StorageT>
struct annotated_weak_pointer_storage_array_traits {
    typedef typename annotated_weak_pointer_storage_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count = 1u;
};

template <typename StorageT, std::size_t N>
struct annotated_weak_pointer_storage_array_traits<StorageT[N]> {
    typedef typename annotated_weak_pointer_storage_array_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count =
        N * annotated_weak_pointer_storage_array_traits<StorageT>::slot_count;
};

template <typename StorageT, std::size_t N>
struct annotated_weak_pointer_storage_array_traits<std::array<StorageT, N> > {
    typedef typename annotated_weak_pointer_storage_array_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count =
        N * annotated_weak_pointer_storage_array_traits<StorageT>::slot_count;
};

template <typename StorageT, std::size_t N>
struct annotated_weak_pointer_storage_array_traits< ::wave::array<StorageT, N> > {
    typedef typename annotated_weak_pointer_storage_array_traits<StorageT>::element_type element_type;
    static constexpr std::size_t slot_count =
        N * annotated_weak_pointer_storage_array_traits<StorageT>::slot_count;
};

template <typename StorageT>
struct is_annotated_pointer_storage_array : std::false_type {};

template <typename StorageT, std::size_t N>
struct is_annotated_pointer_storage_array<StorageT[N]> : std::true_type {};

template <typename StorageT, std::size_t N>
struct is_annotated_pointer_storage_array<std::array<StorageT, N> > : std::true_type {};

template <typename StorageT, std::size_t N>
struct is_annotated_pointer_storage_array< ::wave::array<StorageT, N> > : std::true_type {};

template <typename CountT>
std::size_t normalize_annotated_pointer_count_impl(CountT count, std::true_type) noexcept {
    return count < 0 ? 0u : static_cast<std::size_t>(count);
}

template <typename CountT>
std::size_t normalize_annotated_pointer_count_impl(CountT count, std::false_type) noexcept {
    return static_cast<std::size_t>(count);
}

template <typename CountT>
std::size_t normalize_annotated_pointer_count_clean(CountT count, std::false_type) noexcept {
    return normalize_annotated_pointer_count_impl(
        count,
        std::integral_constant<bool, std::is_signed<CountT>::value>());
}

template <typename CountT>
std::size_t normalize_annotated_pointer_count_clean(CountT count, std::true_type) noexcept {
    typedef typename std::underlying_type<CountT>::type Underlying;
    return normalize_annotated_pointer_count_impl(
        static_cast<Underlying>(count),
        std::integral_constant<bool, std::is_signed<Underlying>::value>());
}

template <typename CountT>
std::size_t normalize_annotated_pointer_count(CountT count) noexcept {
    typedef typename std::remove_cv<typename std::remove_reference<CountT>::type>::type CleanCount;
    static_assert(std::is_integral<CleanCount>::value || std::is_enum<CleanCount>::value,
                  "WAVE_PTR_ARRAY length must be an integral or enum value");
    return normalize_annotated_pointer_count_clean(
        static_cast<CleanCount>(count),
        std::integral_constant<bool, std::is_enum<CleanCount>::value>());
}

template <typename Visitor, typename StorageT, typename CountT, typename... Meta>
void invoke_annotated_ptr_visitor_impl(Visitor&& visitor,
                                       const char* name,
                                       const StorageT& storage,
                                       CountT count,
                                       bool array_semantics,
                                       Meta&&... meta) {
    typedef typename std::remove_cv<typename std::remove_reference<StorageT>::type>::type CleanStorage;
    typedef annotated_pointer_storage_traits<CleanStorage> StorageTraits;
    typedef typename StorageTraits::element_type Element;
    AnnotatedWavePtrView<Element> view(
        StorageTraits::get(storage),
        normalize_annotated_pointer_count(count),
        array_semantics);
    invoke_ptr_visitor(std::forward<Visitor>(visitor),
                       name,
                       std::addressof(view),
                       std::forward<Meta>(meta)...);
}

template <typename Visitor, typename StorageT, typename CountT, typename... Meta>
void invoke_annotated_ptr_visitor(Visitor&& visitor,
                                  const char* name,
                                  const StorageT& storage,
                                  CountT count,
                                  Meta&&... meta) {
    invoke_annotated_ptr_visitor_impl(
        std::forward<Visitor>(visitor),
        name,
        storage,
        count,
        false,
        std::forward<Meta>(meta)...);
}

template <typename Visitor, typename StorageT, typename CountT, typename... Meta>
void invoke_annotated_ptr_array_visitor(Visitor&& visitor,
                                        const char* name,
                                        const StorageT& storage,
                                        CountT count,
                                        Meta&&... meta) {
    invoke_annotated_ptr_visitor_impl(
        std::forward<Visitor>(visitor),
        name,
        storage,
        count,
        true,
        std::forward<Meta>(meta)...);
}

template <typename Visitor, typename StorageT, typename... Meta>
void invoke_annotated_weak_ptr_visitor(Visitor&& visitor,
                                       const char* name,
                                       const StorageT& storage,
                                       Meta&&... meta) {
    typedef typename std::remove_cv<typename std::remove_reference<StorageT>::type>::type CleanStorage;
    typedef annotated_weak_pointer_storage_traits<CleanStorage> StorageTraits;
    typedef typename StorageTraits::element_type Element;
    AnnotatedWeakPtrView<Element> view(StorageTraits::lock(storage), 1u, false);
    invoke_ptr_visitor(std::forward<Visitor>(visitor),
                       name,
                       std::addressof(view),
                       std::forward<Meta>(meta)...);
}

inline std::string annotated_pointer_storage_array_child_name(const std::string& base,
                                                              std::size_t index) {
    return base + "[" + std::to_string(index) + "]";
}

template <typename Visitor>
auto annotated_pointer_storage_array_child_name_impl(
    int,
    Visitor& visitor,
    const std::string& base,
    std::size_t index,
    std::size_t logical_count)
    -> decltype(visitor.annotated_pointer_storage_array_child_name(
        base, index, logical_count)) {
    return visitor.annotated_pointer_storage_array_child_name(
        base, index, logical_count);
}

template <typename Visitor>
std::string annotated_pointer_storage_array_child_name_impl(
    long,
    Visitor&,
    const std::string& base,
    std::size_t index,
    std::size_t) {
    return annotated_pointer_storage_array_child_name(base, index);
}

template <typename Visitor>
std::string annotated_pointer_storage_array_child_name(
    Visitor& visitor,
    const std::string& base,
    std::size_t index,
    std::size_t logical_count) {
    return annotated_pointer_storage_array_child_name_impl(
        0, visitor, base, index, logical_count);
}

template <typename StorageT>
struct AnnotatedPointerStorageArrayVisitor {
    template <typename Visitor, typename CountT>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const StorageT& storage,
                      CountT count,
                      bool array_semantics,
                      AnnotatedPointerMemberKey member_key) {
        // Do not forward GeneratedMemberId here: it names the array field, not
        // an individual pointer slot. The indexed name is consumed immediately
        // by Tracer and remains unambiguous for nested C arrays.
        invoke_annotated_ptr_visitor_impl(
            visitor, name.c_str(), storage, count, array_semantics, member_key);
    }
};

template <typename StorageT, std::size_t N>
struct AnnotatedPointerStorageArrayVisitor<StorageT[N]> {
    template <typename Visitor, typename CountT>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const StorageT (&storage)[N],
                      CountT count,
                      bool array_semantics,
                      AnnotatedPointerMemberKey member_key) {
        const std::size_t traced_count =
            annotated_pointer_storage_array_trace_count(visitor, N);
        for (std::size_t i = 0; i < traced_count; ++i) {
            AnnotatedPointerStorageArrayVisitor<StorageT>::visit(
                visitor,
                annotated_pointer_storage_array_child_name(visitor, name, i, N),
                storage[i],
                count,
                array_semantics,
                member_key);
        }
    }
};

template <typename StorageT, std::size_t N>
struct AnnotatedPointerStorageArrayVisitor<std::array<StorageT, N> > {
    template <typename Visitor, typename CountT>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const std::array<StorageT, N>& storage,
                      CountT count,
                      bool array_semantics,
                      AnnotatedPointerMemberKey member_key) {
        const std::size_t traced_count =
            annotated_pointer_storage_array_trace_count(visitor, N);
        for (std::size_t i = 0; i < traced_count; ++i) {
            AnnotatedPointerStorageArrayVisitor<StorageT>::visit(
                visitor,
                annotated_pointer_storage_array_child_name(visitor, name, i, N),
                storage[i],
                count,
                array_semantics,
                member_key);
        }
    }
};

template <typename StorageT, std::size_t N>
struct AnnotatedPointerStorageArrayVisitor< ::wave::array<StorageT, N> > {
    template <typename Visitor, typename CountT>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const ::wave::array<StorageT, N>& storage,
                      CountT count,
                      bool array_semantics,
                      AnnotatedPointerMemberKey member_key) {
        const std::size_t traced_count =
            annotated_pointer_storage_array_trace_count(visitor, N);
        for (std::size_t i = 0; i < traced_count; ++i) {
            AnnotatedPointerStorageArrayVisitor<StorageT>::visit(
                visitor,
                annotated_pointer_storage_array_child_name(visitor, name, i, N),
                storage[i],
                count,
                array_semantics,
                member_key);
        }
    }
};

template <typename StorageT>
struct AnnotatedWeakPointerStorageArrayVisitor {
    template <typename Visitor>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const StorageT& storage,
                      AnnotatedPointerMemberKey member_key) {
        invoke_annotated_weak_ptr_visitor(visitor, name.c_str(), storage, member_key);
    }
};

template <typename StorageT, std::size_t N>
struct AnnotatedWeakPointerStorageArrayVisitor<StorageT[N]> {
    template <typename Visitor>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const StorageT (&storage)[N],
                      AnnotatedPointerMemberKey member_key) {
        const std::size_t traced_count =
            annotated_pointer_storage_array_trace_count(visitor, N);
        for (std::size_t i = 0; i < traced_count; ++i) {
            AnnotatedWeakPointerStorageArrayVisitor<StorageT>::visit(
                visitor,
                annotated_pointer_storage_array_child_name(visitor, name, i, N),
                storage[i],
                member_key);
        }
    }
};

template <typename StorageT, std::size_t N>
struct AnnotatedWeakPointerStorageArrayVisitor<std::array<StorageT, N> > {
    template <typename Visitor>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const std::array<StorageT, N>& storage,
                      AnnotatedPointerMemberKey member_key) {
        const std::size_t traced_count =
            annotated_pointer_storage_array_trace_count(visitor, N);
        for (std::size_t i = 0; i < traced_count; ++i) {
            AnnotatedWeakPointerStorageArrayVisitor<StorageT>::visit(
                visitor,
                annotated_pointer_storage_array_child_name(visitor, name, i, N),
                storage[i],
                member_key);
        }
    }
};

template <typename StorageT, std::size_t N>
struct AnnotatedWeakPointerStorageArrayVisitor< ::wave::array<StorageT, N> > {
    template <typename Visitor>
    static void visit(Visitor& visitor,
                      const std::string& name,
                      const ::wave::array<StorageT, N>& storage,
                      AnnotatedPointerMemberKey member_key) {
        const std::size_t traced_count =
            annotated_pointer_storage_array_trace_count(visitor, N);
        for (std::size_t i = 0; i < traced_count; ++i) {
            AnnotatedWeakPointerStorageArrayVisitor<StorageT>::visit(
                visitor,
                annotated_pointer_storage_array_child_name(visitor, name, i, N),
                storage[i],
                member_key);
        }
    }
};

template <typename Visitor, typename StorageT, typename CountT, typename... Meta>
void invoke_annotated_ptr_storage_array_visitor_impl(Visitor&& visitor,
                                                     const char* name,
                                                     const StorageT& storage,
                                                     CountT count,
                                                     bool array_semantics,
                                                     Meta&&... meta) {
    typedef typename std::remove_cv<typename std::remove_reference<StorageT>::type>::type CleanStorage;
    static_assert(is_annotated_pointer_storage_array<CleanStorage>::value,
                  "annotated pointer storage array helper requires a supported fixed array");
    AnnotatedPointerStorageArrayVisitor<CleanStorage>::visit(
        visitor,
        std::string(name ? name : ""),
        storage,
        count,
        array_semantics,
        annotated_pointer_member_key_or_invalid(std::forward<Meta>(meta)...));
}

// Keep the original helper as the scalar-target form so already-generated
// reflection headers remain source-compatible after a WaveTrace update.
template <typename Visitor, typename StorageT, typename CountT, typename... Meta>
void invoke_annotated_ptr_storage_array_visitor(Visitor&& visitor,
                                                const char* name,
                                                const StorageT& storage,
                                                CountT count,
                                                Meta&&... meta) {
    invoke_annotated_ptr_storage_array_visitor_impl(
        std::forward<Visitor>(visitor),
        name,
        storage,
        count,
        false,
        std::forward<Meta>(meta)...);
}

template <typename Visitor, typename StorageT, typename CountT, typename... Meta>
void invoke_annotated_ptr_array_storage_array_visitor(Visitor&& visitor,
                                                      const char* name,
                                                      const StorageT& storage,
                                                      CountT count,
                                                      Meta&&... meta) {
    invoke_annotated_ptr_storage_array_visitor_impl(
        std::forward<Visitor>(visitor),
        name,
        storage,
        count,
        true,
        std::forward<Meta>(meta)...);
}

template <typename Visitor, typename StorageT, typename... Meta>
void invoke_annotated_weak_ptr_storage_array_visitor(Visitor&& visitor,
                                                     const char* name,
                                                     const StorageT& storage,
                                                     Meta&&... meta) {
    typedef typename std::remove_cv<typename std::remove_reference<StorageT>::type>::type CleanStorage;
    static_assert(is_annotated_pointer_storage_array<CleanStorage>::value,
                  "annotated weak pointer storage array helper requires a supported fixed array");
    AnnotatedWeakPointerStorageArrayVisitor<CleanStorage>::visit(
        visitor,
        std::string(name ? name : ""),
        storage,
        annotated_pointer_member_key_or_invalid(std::forward<Meta>(meta)...));
}

} // namespace detail

template <typename T>
struct wave_ptr_traits<detail::AnnotatedWavePtrView<T> > {
    typedef T* pointer_type;
    typedef T element_type;
    static std::size_t declared_size(const detail::AnnotatedWavePtrView<T>& ptr) noexcept {
        return ptr.declared_size();
    }
    static bool array_semantics(const detail::AnnotatedWavePtrView<T>& ptr) noexcept {
        return ptr.array_semantics();
    }
    static std::shared_ptr<const void> keepalive(const detail::AnnotatedWavePtrView<T>&) noexcept {
        return std::shared_ptr<const void>();
    }
};

template <typename T>
struct wave_ptr_traits<detail::AnnotatedWeakPtrView<T> > {
    typedef std::weak_ptr<T> pointer_type;
    typedef T element_type;
    static std::size_t declared_size(const detail::AnnotatedWeakPtrView<T>& ptr) noexcept {
        return ptr.declared_size();
    }
    static bool array_semantics(const detail::AnnotatedWeakPtrView<T>& ptr) noexcept {
        return ptr.array_semantics();
    }
    static std::shared_ptr<const void> keepalive(const detail::AnnotatedWeakPtrView<T>& ptr) noexcept {
        return ptr.keepalive();
    }
};

template <typename T, std::size_t N> class array;

template <typename T> struct wave_array_traits;

// wave::array<T,N> is a size-preserving wrapper around std::array<T,N>.
// It deliberately exposes mutation only through non-const operator[] so the
// tracer can mark the accessed element dirty.  The object stores no tracer id
// or metadata, therefore sizeof(wave::array<T,N>) == sizeof(std::array<T,N>).
template <typename T, std::size_t N>
class array {
public:
    typedef T value_type;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T* iterator;
    typedef const T* const_iterator;
    typedef std::reverse_iterator<iterator> reverse_iterator;
    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

    array() = default;
    array(const array& other) = default;
    array(array&& other) noexcept(std::is_nothrow_move_constructible<std::array<T, N> >::value) = default;
    array(const std::array<T, N>& rhs) : storage_(rhs) {}
    array(std::array<T, N>&& rhs) noexcept(std::is_nothrow_move_constructible<std::array<T, N> >::value)
        : storage_(std::move(rhs)) {}

    array& operator=(const array& rhs) {
        if (this == std::addressof(rhs)) return *this;
        notify_all_elements_dirty_();
        storage_ = rhs.storage_;
        return *this;
    }

    array& operator=(const std::array<T, N>& rhs) {
        notify_all_elements_dirty_();
        storage_ = rhs;
        return *this;
    }

    array& operator=(array&& rhs) noexcept(noexcept(std::declval<std::array<T, N>&>() = std::declval<std::array<T, N>&&>())) {
        if (this == std::addressof(rhs)) return *this;
        notify_all_elements_dirty_();
        storage_ = std::move(rhs.storage_);
        return *this;
    }

    array& operator=(std::array<T, N>&& rhs) noexcept(noexcept(std::declval<std::array<T, N>&>() = std::declval<std::array<T, N>&&>())) {
        notify_all_elements_dirty_();
        storage_ = std::move(rhs);
        return *this;
    }

    template <typename U = T>
    typename std::enable_if<!detail::is_wave_array<U>::value, U&>::type
    operator[](size_type i) noexcept {
        notify_wave_array_index_access(
            i,
            static_cast<const void*>(std::addressof(storage_[i])),
            reflect::type_tag_of<U>(),
            sizeof(U));
        return storage_[i];
    }

    template <typename U = T>
    typename std::enable_if<detail::is_wave_array<U>::value, U&>::type
    operator[](size_type i) noexcept {
        return storage_[i];
    }

    const T& operator[](size_type i) const noexcept { return storage_[i]; }
    T& at(size_type i) {
        if (i >= N) throw std::out_of_range("wave::array::at");
        return (*this)[i];
    }
    const T& at(size_type i) const {
        if (i >= N) throw std::out_of_range("wave::array::at");
        return storage_[i];
    }
    T& front() noexcept { return (*this)[0]; }
    const T& front() const noexcept { return storage_[0]; }
    T& back() noexcept { return (*this)[N - 1]; }
    const T& back() const noexcept { return storage_[N - 1]; }
    const T& read(size_type i) const noexcept { return storage_[i]; }
    const std::array<T, N>& read() const noexcept { return storage_; }

    constexpr size_type size() const noexcept { return N; }
    constexpr size_type max_size() const noexcept { return N; }
    constexpr bool empty() const noexcept { return N == 0; }

    T* data() noexcept {
        notify_all_elements_dirty_();
        return storage_.data();
    }
    const T* data() const noexcept { return storage_.data(); }
    iterator begin() noexcept {
        notify_all_elements_dirty_();
        return storage_.data();
    }
    const_iterator begin() const noexcept { return storage_.data(); }
    const_iterator cbegin() const noexcept { return storage_.data(); }
    iterator end() noexcept { return storage_.data() + N; }
    const_iterator end() const noexcept { return storage_.data() + N; }
    const_iterator cend() const noexcept { return storage_.data() + N; }

    reverse_iterator rbegin() noexcept {
        notify_all_elements_dirty_();
        return reverse_iterator(storage_.data() + N);
    }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    reverse_iterator rend() noexcept { return reverse_iterator(storage_.data()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    void fill(const T& value) {
        notify_all_elements_dirty_();
        storage_.fill(value);
    }

    void swap(array& other) noexcept(noexcept(std::swap(std::declval<T&>(), std::declval<T&>()))) {
        if (this == std::addressof(other)) return;
        notify_all_elements_dirty_();
        other.notify_all_elements_dirty_();
        storage_.swap(other.storage_);
    }

    array* operator&() noexcept {
        notify_all_elements_dirty_();
        return this;
    }
    const array* operator&() const noexcept { return this; }

    operator const std::array<T, N>&() const noexcept {
        return storage_;
    }
    operator T*() = delete;
    operator const T*() const = delete;

private:
    void notify_all_elements_dirty_() const noexcept {
        if (N == 0) return;
        if (notify_wave_array_all_elements_access(
                static_cast<const void*>(storage_.data()),
                reflect::type_tag_of<T>(),
                sizeof(T),
                N)) {
            return;
        }
        fail_wave_array_bulk_notify(
            static_cast<const void*>(storage_.data()),
            reflect::type_tag_of<T>(),
            sizeof(T),
            N,
            typeid(T).name(),
#if defined(_MSC_VER)
            __FUNCSIG__,
#elif defined(__GNUC__) || defined(__clang__)
            __PRETTY_FUNCTION__,
#else
            __func__,
#endif
            "notify callback unavailable or runtime mapping failed");
    }

    std::array<T, N> storage_;
};

template <typename T, std::size_t N>
inline bool operator==(const array<T, N>& lhs, const array<T, N>& rhs) {
    return lhs.read() == rhs.read();
}

template <typename T, std::size_t N>
inline bool operator!=(const array<T, N>& lhs, const array<T, N>& rhs) {
    return !(lhs == rhs);
}

template <typename T, std::size_t N>
inline bool operator<(const array<T, N>& lhs, const array<T, N>& rhs) {
    return lhs.read() < rhs.read();
}

template <typename T, std::size_t N>
inline bool operator>(const array<T, N>& lhs, const array<T, N>& rhs) {
    return rhs < lhs;
}

template <typename T, std::size_t N>
inline bool operator<=(const array<T, N>& lhs, const array<T, N>& rhs) {
    return !(rhs < lhs);
}

template <typename T, std::size_t N>
inline bool operator>=(const array<T, N>& lhs, const array<T, N>& rhs) {
    return !(lhs < rhs);
}

template <typename T, std::size_t N>
inline void swap(array<T, N>& lhs, array<T, N>& rhs) noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
}

template <std::size_t I, typename T, std::size_t N>
inline T& get(array<T, N>& a) noexcept {
    static_assert(I < N, "wave::array get index out of range");
    return a[I];
}

template <std::size_t I, typename T, std::size_t N>
inline const T& get(const array<T, N>& a) noexcept {
    static_assert(I < N, "wave::array get index out of range");
    return a[I];
}

template <std::size_t I, typename T, std::size_t N>
inline T&& get(array<T, N>&& a) noexcept {
    static_assert(I < N, "wave::array get index out of range");
    return std::move(a[I]);
}

template <std::size_t I, typename T, std::size_t N>
inline const T&& get(const array<T, N>&& a) noexcept {
    static_assert(I < N, "wave::array get index out of range");
    return std::move(a[I]);
}

template <typename T, std::size_t N>
struct detail::is_wave_array<array<T, N> > : std::true_type {};

template <typename T, std::size_t N>
struct wave_array_traits<array<T, N> > {
    typedef T element_type;
    static const std::size_t size = N;
};

static_assert(sizeof(array<std::uint32_t, 4>) == sizeof(std::array<std::uint32_t, 4>), "wave::array size mismatch");
static_assert(alignof(array<std::uint32_t, 4>) == alignof(std::array<std::uint32_t, 4>), "wave::array align mismatch");

} // namespace wave

namespace std {

template <typename T, std::size_t N>
struct tuple_size< ::wave::array<T, N> > : integral_constant<std::size_t, N> {};

template <std::size_t I, typename T, std::size_t N>
struct tuple_element<I, ::wave::array<T, N> > {
    static_assert(I < N, "wave::array tuple_element index out of range");
    typedef T type;
};

} // namespace std

#ifndef WAVE_REFLECT_FRIEND
#define WAVE_REFLECT_FRIEND \
    template <typename WaveReflectAccessT> friend struct ::wave::ReflectAccess; \
    using wave_reflect_friend_marker_do_not_use = ::wave::ReflectFriendMarker;
#endif

// Kept for source compatibility. New code only needs WAVE_TRACE_PRIVATE on
// each private/protected member that should be reflected.
#ifndef WAVE_REFLECT_MARKED_FRIEND
#define WAVE_REFLECT_MARKED_FRIEND \
    template <typename WaveReflectAccessT> friend struct ::wave::ReflectAccess;
#endif

// Keyword-like reflection annotations. Clang retains the payload for ReflectGen;
// MSVC/GCC see an ordinary declaration with no ABI change.
#if defined(__clang__)
#define WAVE_DETAIL_POINTER_ANNOTATE_(payload) __attribute__((annotate(payload)))
#else
#define WAVE_DETAIL_POINTER_ANNOTATE_(payload)
#endif

#ifndef WAVE_PTR
#define WAVE_PTR WAVE_DETAIL_POINTER_ANNOTATE_("wavetrace.ptr")
#endif

#ifndef WAVE_PTR_ARRAY
#define WAVE_PTR_ARRAY(count) WAVE_DETAIL_POINTER_ANNOTATE_("wavetrace.ptr_array:" #count)
#endif

// Omit the marked data member and its complete subtree from reflection.
#ifndef WAVE_NO_TRACE
#define WAVE_NO_TRACE WAVE_DETAIL_POINTER_ANNOTATE_("wavetrace.no_trace")
#endif

// Opt one private/protected member into reflection and grant ReflectGen the
// required class access. No separate class-level macro is needed.
#ifndef WAVE_TRACE_PRIVATE
#define WAVE_TRACE_PRIVATE \
    WAVE_REFLECT_MARKED_FRIEND \
    WAVE_DETAIL_POINTER_ANNOTATE_("wavetrace.private_trace")
#endif

#if defined(REFLECT_MACRO_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef REFLECT_MACRO_RESTORE_MAX_MACRO_
#endif
#if defined(REFLECT_MACRO_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef REFLECT_MACRO_RESTORE_MIN_MACRO_
#endif

#endif // !_WIN32

#endif // WAVETRACE_REFLECT_MACRO_H_INCLUDED_
