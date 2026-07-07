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
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

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
typedef std::uint64_t NodeId;
typedef NodeId (*DynamicExpandFn)(Tracer&, const std::string&, NodeId, const void*);

template <typename T>
NodeId dynamic_expand_bridge(Tracer& tracer, const std::string& path, NodeId parent_id, const void* obj);

static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

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

template <typename PtrT> struct is_wave_ptr_storage : std::false_type {};

template <typename PtrT> struct wave_ptr_storage_traits {
    typedef void element_type;
};

template <typename T>
struct is_wave_ptr_storage<T*> : std::integral_constant<bool,
    !std::is_void<typename std::remove_cv<T>::type>::value &&
    !std::is_array<typename std::remove_cv<T>::type>::value
> {};

template <typename T>
struct wave_ptr_storage_traits<T*> {
    typedef T element_type;
    static element_type* get(T* p) noexcept { return p; }
    static void reset(T*& p) noexcept { p = NULL; }
    static void reset(T*& p, element_type* value) noexcept { p = value; }
};

template <typename T, typename D>
struct is_wave_ptr_storage<std::unique_ptr<T, D> > : std::integral_constant<bool,
    !std::is_void<typename std::remove_cv<T>::type>::value &&
    !std::is_array<T>::value
> {};

template <typename T, typename D>
struct wave_ptr_storage_traits<std::unique_ptr<T, D> > {
    typedef T element_type;
    static element_type* get(const std::unique_ptr<T, D>& p) noexcept { return p.get(); }
    static void reset(std::unique_ptr<T, D>& p) noexcept { p.reset(); }
};

template <typename T>
struct is_wave_ptr_storage<std::shared_ptr<T> > : std::integral_constant<bool,
    !std::is_void<typename std::remove_cv<T>::type>::value &&
    !std::is_array<T>::value
> {};

template <typename T>
struct wave_ptr_storage_traits<std::shared_ptr<T> > {
    typedef T element_type;
    static element_type* get(const std::shared_ptr<T>& p) noexcept { return p.get(); }
    static void reset(std::shared_ptr<T>& p) noexcept { p.reset(); }
};

template <typename T> struct is_wave_ptr : std::false_type {};

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

// WavePtr<PtrT> is a pointer-like wrapper whose tracing semantic is:
// expand the object(s) currently referenced by the wrapped pointer.  By default
// it expands a single object; business code may call declareSize(n) before
// tracing topology is prepared to expand ptr[0]..ptr[n-1].
//
// Supported PtrT forms:
//   T*
//   std::unique_ptr<T, D>
//   std::shared_ptr<T>
//
// The pointee T does not need to inherit DirectReflectPointerTarget.  The
// wrapper itself is the explicit opt-in marker.  Topology is still stable:
// changing the pointer or declared size after topology preparation does not
// rebuild the tree.
template <typename PtrT>
class WavePtr {
    static_assert(detail::is_wave_ptr_storage<PtrT>::value,
                  "wave::WavePtr<PtrT> requires PtrT to be T*, std::unique_ptr<T, D>, or std::shared_ptr<T> for a non-array object type");

public:
    typedef PtrT pointer_type;
    typedef typename detail::wave_ptr_storage_traits<PtrT>::element_type element_type;

    WavePtr() noexcept : ptr_(), declared_size_(1) {}
    WavePtr(std::nullptr_t) noexcept : ptr_(), declared_size_(1) {}
    WavePtr(const WavePtr& other) = default;
    WavePtr& operator=(const WavePtr& other) = default;
    WavePtr(WavePtr&& other) noexcept : ptr_(std::move(other.ptr_)), declared_size_(other.declared_size_) {}
    WavePtr& operator=(WavePtr&& other) noexcept {
        ptr_ = std::move(other.ptr_);
        declared_size_ = other.declared_size_;
        return *this;
    }

    template <typename U = PtrT, typename std::enable_if<std::is_pointer<U>::value, int>::type = 0>
    WavePtr(element_type* ptr) noexcept : ptr_(ptr), declared_size_(1) {}

    template <typename U = PtrT, typename std::enable_if<std::is_pointer<U>::value, int>::type = 0>
    WavePtr& operator=(element_type* ptr) noexcept {
        detail::wave_ptr_storage_traits<PtrT>::reset(ptr_, ptr);
        return *this;
    }

    template <typename U = PtrT, typename std::enable_if<
        !std::is_pointer<PtrT>::value &&
        !std::is_pointer<U>::value &&
        std::is_constructible<PtrT, const U&>::value, int>::type = 0>
    WavePtr(const U& ptr) : ptr_(ptr), declared_size_(1) {}

    template <typename U = PtrT, typename std::enable_if<
        !std::is_pointer<PtrT>::value &&
        !std::is_pointer<U>::value &&
        std::is_assignable<PtrT&, const U&>::value, int>::type = 0>
    WavePtr& operator=(const U& ptr) {
        ptr_ = ptr;
        return *this;
    }

    template <typename U = PtrT, typename std::enable_if<
        !std::is_pointer<U>::value &&
        std::is_constructible<PtrT, PtrT&&>::value, int>::type = 0>
    WavePtr(PtrT&& ptr) noexcept : ptr_(std::move(ptr)), declared_size_(1) {}

    template <typename U = PtrT, typename std::enable_if<
        !std::is_pointer<U>::value &&
        std::is_assignable<PtrT&, PtrT&&>::value, int>::type = 0>
    WavePtr& operator=(PtrT&& ptr) noexcept {
        ptr_ = std::move(ptr);
        return *this;
    }

    WavePtr& operator=(std::nullptr_t) noexcept {
        detail::wave_ptr_storage_traits<PtrT>::reset(ptr_);
        return *this;
    }

    element_type* get() const noexcept {
        return detail::wave_ptr_storage_traits<PtrT>::get(ptr_);
    }

    element_type& operator*() const noexcept { return *get(); }
    element_type* operator->() const noexcept { return get(); }
    element_type& operator[](std::size_t index) const noexcept { return get()[index]; }
    operator element_type*() const noexcept { return get(); }

    template <typename U = PtrT, typename std::enable_if<!std::is_pointer<U>::value, int>::type = 0>
    operator U&() noexcept { return ptr_; }

    template <typename U = PtrT, typename std::enable_if<!std::is_pointer<U>::value, int>::type = 0>
    operator const U&() const noexcept { return ptr_; }

    template <typename U = PtrT, typename std::enable_if<!std::is_pointer<U>::value, int>::type = 0>
    operator U&&() && noexcept { return std::move(ptr_); }

    explicit operator bool() const noexcept { return get() != NULL; }

    WavePtr& declareSize(std::size_t count) noexcept {
        declared_size_ = count;
        return *this;
    }

    std::size_t declared_size() const noexcept { return declared_size_; }
    std::size_t declaredSize() const noexcept { return declared_size_; }

    void reset() noexcept {
        detail::wave_ptr_storage_traits<PtrT>::reset(ptr_);
    }

    PtrT& storage() noexcept { return ptr_; }
    const PtrT& storage() const noexcept { return ptr_; }

    PtrT& raw_storage_unsafe_for_initialization_only() noexcept { return ptr_; }
    const PtrT& raw_storage_unsafe_for_initialization_only() const noexcept { return ptr_; }

private:
    PtrT ptr_;
    std::size_t declared_size_;
};

template <typename PtrT>
struct detail::is_wave_ptr<WavePtr<PtrT> > : std::true_type {};

template <typename WavePtrT>
struct wave_ptr_traits;

template <typename PtrT>
struct wave_ptr_traits<WavePtr<PtrT> > {
    typedef PtrT pointer_type;
    typedef typename detail::wave_ptr_storage_traits<PtrT>::element_type element_type;
    static std::size_t declared_size(const WavePtr<PtrT>& ptr) noexcept { return ptr.declared_size(); }
};

template <typename PtrT>
WavePtr<typename std::decay<PtrT>::type> make_wave_ptr(PtrT&& ptr) {
    return WavePtr<typename std::decay<PtrT>::type>(std::forward<PtrT>(ptr));
}

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
    iterator end() noexcept {
        notify_all_elements_dirty_();
        return storage_.data() + N;
    }
    const_iterator end() const noexcept { return storage_.data() + N; }
    const_iterator cend() const noexcept { return storage_.data() + N; }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    void fill(const T& value) {
        for (size_type i = 0; i < N; ++i) {
            (*this)[i] = value;
        }
    }

    void swap(array& other) noexcept(noexcept(std::swap(std::declval<T&>(), std::declval<T&>()))) {
        if (this == std::addressof(other)) return;
        using std::swap;
        for (size_type i = 0; i < N; ++i) {
            swap((*this)[i], other[i]);
        }
    }

    array* operator&() noexcept {
        notify_all_elements_dirty_();
        return this;
    }
    const array* operator&() const noexcept {
        notify_all_elements_dirty_();
        return this;
    }

    operator const std::array<T, N>&() const noexcept {
        return storage_;
    }
    operator T*() = delete;
    operator const T*() const = delete;

private:
    void notify_all_elements_dirty_() const noexcept {
        for (size_type i = 0; i < N; ++i) {
            notify_wave_array_index_access(
                i,
                static_cast<const void*>(std::addressof(storage_[i])),
                reflect::type_tag_of<T>(),
                sizeof(T));
        }
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

#if defined(REFLECT_MACRO_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef REFLECT_MACRO_RESTORE_MAX_MACRO_
#endif
#if defined(REFLECT_MACRO_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef REFLECT_MACRO_RESTORE_MIN_MACRO_
#endif
