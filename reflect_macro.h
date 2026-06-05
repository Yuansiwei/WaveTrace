#pragma once

// Lightweight public surface for business model headers.
// Include this header instead of wave_runtime.h when a business type only needs
// reflection opt-in macros, WaveValue<T>, wave::array<T,N>, or WaveDirtyHook.

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
#include <cstring>
#include <memory>
#include <type_traits>

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

static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

// Business opt-in marker: pointers/smart pointers to types derived from this
// marker may be expanded directly by wave_runtime.h.  No allocation tracking is
// performed, so the business model owns lifetime correctness.
struct DirectReflectPointerTarget {};

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

inline WaveValueNotifyFn& wave_value_notify_slot() noexcept {
    static WaveValueNotifyFn fn = NULL;
    return fn;
}

inline WaveArrayIndexNotifyFn& wave_array_index_notify_slot() noexcept {
    static WaveArrayIndexNotifyFn fn = NULL;
    return fn;
}

inline void set_wave_value_notify_fn(WaveValueNotifyFn fn) noexcept {
    wave_value_notify_slot() = fn;
}

inline void set_wave_array_index_notify_fn(WaveArrayIndexNotifyFn fn) noexcept {
    wave_array_index_notify_slot() = fn;
}

template <typename T>
struct is_wave_value_allowed : std::integral_constant<bool,
    (std::is_arithmetic<T>::value || std::is_enum<T>::value) &&
    !std::is_pointer<T>::value
> {};

template <typename T>
struct is_wave_value : std::false_type {};

template <typename T> struct is_wave_array : std::false_type {};

} // namespace detail

inline void notify_wave_value_write_address(const void* address) noexcept {
    detail::WaveValueNotifyFn fn = detail::wave_value_notify_slot();
    if (fn) fn(address);
}

inline void notify_wave_array_index_access(std::size_t index,
                                           const void* element_address,
                                           const void* element_type_tag,
                                           std::size_t element_size) noexcept {
    detail::WaveArrayIndexNotifyFn fn = detail::wave_array_index_notify_slot();
    if (fn) fn(index, element_address, element_type_tag, element_size);
}

struct WaveDirtyHook {
    typedef void (*MarkFn)(void*, std::uint32_t);

    void* tracer;
    std::uint32_t group_id;
    MarkFn mark_fn;

    WaveDirtyHook() noexcept : tracer(NULL), group_id(kInvalidIndex), mark_fn(NULL) {}

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
    typedef const T* const_iterator;

    array() = default;
    array(const array& other) = default;
    array(const std::array<T, N>& rhs) : storage_(rhs) {}

    array& operator=(const array& rhs) {
        if (this == std::addressof(rhs)) return *this;
        for (size_type i = 0; i < N; ++i) {
            (*this)[i] = rhs.storage_[i];
        }
        return *this;
    }

    array& operator=(const std::array<T, N>& rhs) {
        for (size_type i = 0; i < N; ++i) {
            (*this)[i] = rhs[i];
        }
        return *this;
    }

    T& operator[](size_type i) noexcept {
        notify_wave_array_index_access(
            i,
            static_cast<const void*>(std::addressof(storage_[i])),
            reflect::type_tag_of<T>(),
            sizeof(T));
        return storage_[i];
    }

    const T& operator[](size_type i) const noexcept { return storage_[i]; }
    const T& read(size_type i) const noexcept { return storage_[i]; }
    const std::array<T, N>& read() const noexcept { return storage_; }

    constexpr size_type size() const noexcept { return N; }
    bool empty() const noexcept { return N == 0; }

    const T* data() const noexcept { return storage_.data(); }
    const_iterator begin() const noexcept { return storage_.data(); }
    const_iterator end() const noexcept { return storage_.data() + N; }

    T* data() noexcept = delete;
    T* begin() noexcept = delete;
    T* end() noexcept = delete;
    array* operator&() = delete;
    const array* operator&() const = delete;

    operator std::array<T, N>&() = delete;
    operator const std::array<T, N>&() const = delete;
    operator T*() = delete;
    operator const T*() const = delete;

private:
    std::array<T, N> storage_;
};

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

#ifndef WAVE_REFLECT_FRIEND
#define WAVE_REFLECT_FRIEND \
    template <typename T> friend struct ::wave::ReflectAccess; \
    using wave_reflect_friend_marker_do_not_use = ::wave::ReflectFriendMarker;
#endif

#if defined(REFLECT_MACRO_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef REFLECT_MACRO_RESTORE_MAX_MACRO_
#endif
#if defined(REFLECT_MACRO_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef REFLECT_MACRO_RESTORE_MIN_MACRO_
#endif
