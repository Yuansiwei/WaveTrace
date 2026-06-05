#pragma once

// Protect this public header from Windows-style min/max function-like macros.
// Some business environments define min/max before including project headers;
// those macros break std::min/std::max and std::numeric_limits<T>::max().
#if defined(min)
#pragma push_macro("min")
#undef min
#define REFLECT_RUNTIME_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define REFLECT_RUNTIME_RESTORE_MAX_MACRO_ 1
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

#include "reflect_macro.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <forward_list>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <ostream>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace reflect {

template <typename T>
struct reflected_visitor;

template <typename T>
struct is_reflected : std::false_type {};

namespace detail {

template <typename T, typename = void>
struct is_complete_type : std::false_type {};

template <typename T>
struct is_complete_type<T, decltype(void(sizeof(T)))> : std::true_type {};

} // namespace detail

struct VisitKey {
    const void* address;
    const void* type_tag;

    VisitKey() : address(NULL), type_tag(NULL) {}
    VisitKey(const void* a, const void* t) : address(a), type_tag(t) {}

    bool operator==(const VisitKey& other) const {
        return address == other.address && type_tag == other.type_tag;
    }
};

struct VisitKeyHash {
    std::size_t operator()(const VisitKey& key) const {
        const std::size_t h1 = std::hash<const void*>()(key.address);
        const std::size_t h2 = std::hash<const void*>()(key.type_tag);
        return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (h1 << 6) + (h1 >> 2));
    }
};

typedef std::unordered_set<VisitKey, VisitKeyHash> VisitSet;

inline void write_escaped(std::ostream& out, const char* s) {
    out << '"';
    if (s) {
        while (*s) {
            const char c = *s++;
            switch (c) {
            case '\\': out << "\\\\"; break;
            case '"':  out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:   out << c; break;
            }
        }
    }
    out << '"';
}

inline void write_escaped(std::ostream& out, const std::string& s) {
    write_escaped(out, s.c_str());
}

// Legacy no-op compatibility surface.  These names used to drive allocation
// tracking. They are intentionally inert in the clean no-allocation runtime.
inline void issue_credit() {}
inline bool try_consume_credit() { return false; }
inline void settle_credit_without_allocation() {}

template <typename T>
T* finalize_typed_allocation(T* p, const char*) { return p; }

struct NewHook {
    template <typename T>
    T* operator=(T* p) const { return finalize_typed_allocation(p, typeid(T).name()); }
};

inline NewHook& new_hook() {
    static NewHook h;
    return h;
}

template <typename T, typename... Args>
std::shared_ptr<T> make_shared(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T> >::type
make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T>
typename std::enable_if<std::is_array<T>::value && std::extent<T>::value == 0, std::unique_ptr<T> >::type
make_unique(std::size_t n) {
    typedef typename std::remove_extent<T>::type E;
    return std::unique_ptr<T>(new E[n]());
}

template <typename T, typename... Args>
typename std::enable_if<std::extent<T>::value != 0, void>::type
make_unique(Args&&...) = delete;

template <typename T>
void serialize_object(std::ostream& out, const T* obj, VisitSet& visited);

template <typename T>
void serialize_value(std::ostream& out, const T& value, VisitSet& visited);

template <typename T, typename Enable = void>
struct Serializer {
    static void write(std::ostream&, const T&, VisitSet&) {
        static_assert(is_reflected<T>::value,
                      "Type is not serializable: no serialize_value overload and not marked as reflected.");
    }
};

template <typename T>
struct Serializer<T, typename std::enable_if<is_reflected<T>::value>::type> {
    static void write(std::ostream& out, const T& value, VisitSet& visited) {
        serialize_object(out, &value, visited);
    }
};

template <typename T>
struct Serializer<T, typename std::enable_if<std::is_same<T, bool>::value>::type> {
    static void write(std::ostream& out, const T& value, VisitSet&) {
        out << (value ? "true" : "false");
    }
};

template <typename T>
struct Serializer<T, typename std::enable_if<
    std::is_integral<T>::value && !std::is_same<T, bool>::value
>::type> {
    static void write(std::ostream& out, const T& value, VisitSet&) {
        out << value;
    }
};

template <typename T>
struct Serializer<T, typename std::enable_if<std::is_floating_point<T>::value>::type> {
    static void write(std::ostream& out, const T& value, VisitSet&) {
        out << value;
    }
};

template <typename T>
struct Serializer<T, typename std::enable_if<std::is_enum<T>::value>::type> {
    static void write(std::ostream& out, const T& value, VisitSet&) {
        out << static_cast<typename std::underlying_type<T>::type>(value);
    }
};

inline void serialize_value(std::ostream& out, const std::string& value, VisitSet&) {
    write_escaped(out, value);
}

inline void serialize_value(std::ostream& out, const char* value, VisitSet&) {
    if (value) write_escaped(out, value);
    else out << "null";
}

inline void serialize_value(std::ostream& out, char* const& value, VisitSet&) {
    if (value) write_escaped(out, value);
    else out << "null";
}

template <typename T>
void serialize_value(std::ostream& out, T* const& ptr, VisitSet& visited) {
    if (!ptr) {
        out << "null";
        return;
    }
    typedef typename std::remove_cv<T>::type CleanT;
    if (std::is_base_of< ::wave::DirectReflectPointerTarget, CleanT>::value && is_reflected<CleanT>::value) {
        serialize_object(out, ptr, visited);
    } else {
        out << "\"<untracked_ptr>\"";
    }
}

template <typename T, typename Deleter>
typename std::enable_if<!std::is_array<T>::value, void>::type
serialize_value(std::ostream& out, const std::unique_ptr<T, Deleter>& ptr, VisitSet& visited) {
    T* raw = ptr.get();
    serialize_value(out, raw, visited);
}

template <typename T, typename Deleter>
typename std::enable_if<std::is_array<T>::value, void>::type
serialize_value(std::ostream& out, const std::unique_ptr<T, Deleter>&, VisitSet&) {
    out << "\"<untracked_array_ptr>\"";
}

template <typename T>
void serialize_value(std::ostream& out, const std::shared_ptr<T>& ptr, VisitSet& visited) {
    T* raw = ptr.get();
    serialize_value(out, raw, visited);
}

template <typename T>
void serialize_value(std::ostream& out, const std::weak_ptr<T>& ptr, VisitSet& visited) {
    std::shared_ptr<T> locked = ptr.lock();
    T* raw = locked.get();
    serialize_value(out, raw, visited);
}

template <typename T, std::size_t N>
void serialize_value(std::ostream& out, const std::array<T, N>& value, VisitSet& visited) {
    out << "[";
    for (std::size_t i = 0; i < N; ++i) {
        if (i) out << ", ";
        serialize_value(out, value[i], visited);
    }
    out << "]";
}

template <typename T>
void serialize_value(std::ostream& out, const std::vector<T>& value, VisitSet& visited) {
    out << "[";
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i) out << ", ";
        serialize_value(out, value[i], visited);
    }
    out << "]";
}

template <typename A, typename B>
void serialize_value(std::ostream& out, const std::pair<A, B>& value, VisitSet& visited) {
    out << "{";
    write_escaped(out, "first");
    out << ": ";
    serialize_value(out, value.first, visited);
    out << ", ";
    write_escaped(out, "second");
    out << ": ";
    serialize_value(out, value.second, visited);
    out << "}";
}

template <typename T>
void serialize_value(std::ostream& out, const T& value, VisitSet& visited) {
    Serializer<T>::write(out, value, visited);
}

template <typename T>
void serialize_object(std::ostream& out, const T* obj, VisitSet& visited) {
    if (!obj) {
        out << "null";
        return;
    }

    typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type CleanT;
    const VisitKey key(static_cast<const void*>(obj), type_tag_of<CleanT>());
    if (visited.find(key) != visited.end()) {
        out << "\"<recursive_ref>\"";
        return;
    }
    visited.insert(key);

    out << "{";
    bool first = true;
    auto emit_key = [&](const char* name) {
        if (!first) out << ", ";
        first = false;
        write_escaped(out, name);
        out << ": ";
    };

    reflected_visitor<CleanT>::visit(
        obj,
        [&](const char* name, auto field_ptr) {
            emit_key(name);
            serialize_value(out, *field_ptr, visited);
        },
        [&](const char* name, const auto& value) {
            emit_key(name);
            serialize_value(out, value, visited);
        },
        [&](const char* name, auto getter) {
            emit_key(name);
            serialize_value(out, getter(obj), visited);
        });

    out << "}";
    visited.erase(key);
}

template <typename T>
std::string to_json(const T& obj) {
    std::ostringstream oss;
    VisitSet visited;
    serialize_value(oss, obj, visited);
    return oss.str();
}

} // namespace reflect

// Global operator new/delete hooks are intentionally absent in the clean
// no-allocation-tracking runtime.

#ifndef NEW
#define NEW new
#endif
#ifndef MAKE_SHARED
#define MAKE_SHARED std::make_shared
#endif
#ifndef MAKE_UNIQUE
#define MAKE_UNIQUE std::make_unique
#endif
#if defined(REFLECT_RUNTIME_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef REFLECT_RUNTIME_RESTORE_MAX_MACRO_
#endif
#if defined(REFLECT_RUNTIME_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef REFLECT_RUNTIME_RESTORE_MIN_MACRO_
#endif

