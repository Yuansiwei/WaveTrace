#include <array>
#include <cstddef>
#include <memory>

#if defined(__clang__)
#define WAVE_PTR __attribute__((annotate("wavetrace.ptr")))
#define WAVE_PTR_ARRAY(count) __attribute__((annotate("wavetrace.ptr_array:" #count)))
#else
#define WAVE_PTR
#define WAVE_PTR_ARRAY(count)
#endif

struct PayloadA {
    int value;
};

struct PayloadB {
    int value;
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
    alpha::Box<PayloadA> first;
    alpha::Box<PayloadB> second;
    beta::Box third;
    std::array<PayloadA, 2> array_payloads;
    std::shared_ptr<PayloadA> shared_payload;
    std::unique_ptr<PayloadB> unique_payload;
    WAVE_PTR PayloadA* direct_ptr;
    WAVE_PTR AliasPtr alias_ptr;
    std::size_t annotated_count;
    WAVE_PTR PayloadA* annotated_ptr;
    WAVE_PTR_ARRAY(annotated_count) PayloadA* annotated_array;
    WAVE_PTR std::shared_ptr<PayloadA> annotated_shared;
    WAVE_PTR std::weak_ptr<PayloadB> annotated_weak;
};
