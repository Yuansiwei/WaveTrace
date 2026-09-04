#include <array>
#include <bitset>
#include <cstddef>
#include <memory>

#define WAVE_REFLECT_MARKED_FRIEND \
    template <typename WaveReflectAccessT> friend struct ::wave::ReflectAccess;

#if defined(__clang__)
#define WAVE_PTR __attribute__((annotate("wavetrace.ptr")))
#define WAVE_PTR_ARRAY(count) __attribute__((annotate("wavetrace.ptr_array:" #count)))
#define WAVE_NO_TRACE __attribute__((annotate("wavetrace.no_trace")))
#define WAVE_TRACE_PRIVATE \
    WAVE_REFLECT_MARKED_FRIEND \
    __attribute__((annotate("wavetrace.private_trace")))
#else
#define WAVE_PTR
#define WAVE_PTR_ARRAY(count)
#define WAVE_NO_TRACE
#define WAVE_TRACE_PRIVATE WAVE_REFLECT_MARKED_FRIEND
#endif

namespace wave { template <typename T> struct ReflectAccess; struct ReflectFriendMarker; }

struct PayloadA {
    int value;
};

struct PayloadB {
    int value;
};

struct BitsetOnly {
    std::bitset<130> flags;
};

struct IgnoredOnly {
    int hidden_value;
};

class SelectivePrivate {
private:
    WAVE_TRACE_PRIVATE int traced_value;
    WAVE_TRACE_PRIVATE int second_traced_value;
    int untraced_private_value;
};

namespace alpha {
template <typename T>
struct Box {
    WAVE_PTR T* template_ptr;
    WAVE_PTR T* enabled_ptr;
    int ordinary;
};
}

namespace beta {
struct Box {
    WAVE_PTR int* other_ptr;
};
}

using AliasPtr = PayloadB*;

struct Root {
    WAVE_NO_TRACE IgnoredOnly ignored_object;
    WAVE_NO_TRACE WAVE_PTR PayloadA* ignored_ptr;
    SelectivePrivate selective_private;
    alpha::Box<PayloadA> first;
    alpha::Box<PayloadB> second;
    beta::Box third;
    std::array<PayloadA, 2> array_payloads;
    std::bitset<130> flags;
    std::shared_ptr<PayloadA> shared_payload;
    std::unique_ptr<PayloadB> unique_payload;
    PayloadA* ordinary_raw_ptr;
    PayloadA& ordinary_reference;
    WAVE_PTR PayloadA* direct_ptr;
    WAVE_PTR AliasPtr alias_ptr;
    std::size_t annotated_count;
    WAVE_PTR PayloadA* annotated_ptr;
    WAVE_PTR_ARRAY(annotated_count) PayloadA* annotated_array;
    WAVE_PTR std::shared_ptr<PayloadA> annotated_shared;
    WAVE_PTR std::weak_ptr<PayloadB> annotated_weak;
};
