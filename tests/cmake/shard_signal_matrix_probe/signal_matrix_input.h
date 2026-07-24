#pragma once

#include "reflect_macro.h"
#include "business_complex_types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class ShardMatrixMode : std::uint16_t {
    Idle = 0,
    Busy = 1,
    Done = 7
};

struct ShardScalarBundle {
    bool boolean_value;
    std::int8_t i8_value;
    std::uint8_t u8_value;
    std::int16_t i16_value;
    std::uint16_t u16_value;
    std::int32_t i32_value;
    std::uint32_t u32_value;
    std::int64_t i64_value;
    std::uint64_t u64_value;
    float f32_value;
    double f64_value;
    long double long_double_value;
    ShardMatrixMode mode;
    volatile std::uint32_t volatile_value;
    wave::WaveValue<std::uint32_t> dirty_value;
};

struct ShardSmallLeaf {
    std::uint32_t value;
    bool valid;
    wave::WaveValue<std::uint16_t> dirty;
};

// Deliberately contains only inline, fixed-size state.  ReflectGen should mark
// this type relocation-safe even when its generated specialization lives in a
// different compile shard, allowing WAVE_PTR_ARRAY topology cloning.
struct ShardRelocationSafeLeaf {
    std::uint32_t value;
    std::array<std::uint16_t, 64> lanes;
    bool valid;
};

struct ShardDeepA {
    ShardSmallLeaf leaf;
    std::int64_t own;
};

struct ShardDeepB {
    ShardDeepA child;
    std::array<ShardSmallLeaf, 2> std_children;
    bool own;
};

struct ShardDeepC {
    ShardDeepB child;
    ShardSmallLeaf c_matrix[2][2];
    std::pair<ShardSmallLeaf, ShardSmallLeaf> pair_children;
    double own;
};

struct ShardBaseLeft {
    std::uint32_t base_left_value;
    ShardSmallLeaf base_left_leaf;
};

struct ShardBaseRight {
    std::uint64_t base_right_value;
    bool base_right_valid;
};

struct ShardDerivedLeaf : ShardBaseLeft, ShardBaseRight {
    ShardSmallLeaf derived_leaf;
    std::uint16_t derived_value;
};

struct ShardDirectLeaf : wave::DirectReflectPointerTarget {
    ShardSmallLeaf nested;
    std::uint64_t own;
};

struct ShardBitfieldBundle {
    std::uint32_t low : 3;
    std::uint32_t middle : 11;
    std::uint32_t high : 18;
    std::uint32_t ordinary;
};

class ShardPrivateNestedAnonymousUnion {
public:
    static const unsigned kPcWidth = 48;
    static const unsigned kCallDepthWidth = 16;
    typedef std::uint64_t RawDType;

    ShardPrivateNestedAnonymousUnion() : raw_(0), is_64bit_(false) {}
    void initialize(std::uint64_t raw, bool is_64bit) {
        raw_ = raw;
        is_64bit_ = is_64bit;
    }

private:
    union {
        struct {
            std::uint64_t pc_ : kPcWidth;
            std::uint64_t call_depth_ : kCallDepthWidth;
        };
        RawDType raw_;
    };
    // Match production placement: the access marker may appear after the
    // anonymous union and remains valid anywhere in class scope.
    WAVE_REFLECT_FRIEND;
    bool is_64bit_;
};

class ShardMultipleAnonymousUnions {
public:
    ShardMultipleAnonymousUnions()
        : first_raw_(0), separator_(0), second_raw_(0), tail_(false) {}

    void initialize(std::uint64_t first,
                    std::uint32_t separator,
                    std::uint64_t second,
                    bool tail) {
        first_raw_ = first;
        separator_ = separator;
        second_raw_ = second;
        tail_ = tail;
    }

private:
    union {
        struct {
            std::uint64_t first_low_ : 20;
            std::uint64_t first_high_ : 44;
        };
        std::uint64_t first_raw_;
    };
    std::uint32_t separator_;
    union {
        struct {
            std::uint64_t second_low_ : 36;
            std::uint64_t second_high_ : 28;
        };
        std::uint64_t second_raw_;
    };
    bool tail_;
    WAVE_REFLECT_FRIEND;
};

union ShardVariant {
    std::uint32_t u32_value;
    float f32_value;
    ShardSmallLeaf leaf;

    ShardVariant() : u32_value(0) {}
};

union ShardDirectBitfieldUnion {
    std::uint32_t low5 : 5;
    std::uint32_t low13 : 13;
    std::uint32_t raw;

    ShardDirectBitfieldUnion() : raw(0) {}
};

class ShardNamedAnonymousUnionHolder {
public:
    ShardNamedAnonymousUnionHolder() : payload_{}, ordinary_(0) {}

    void initialize(std::uint32_t raw, std::uint32_t ordinary) {
        payload_.raw_ = raw;
        ordinary_ = ordinary;
    }

private:
    union {
        struct {
            std::uint32_t low_ : 7;
            std::uint32_t middle_ : 9;
            std::uint32_t high_ : 16;
        };
        std::uint32_t raw_;
    } payload_;
    std::uint32_t ordinary_;
    WAVE_REFLECT_FRIEND;
};

class ShardExoticBitfieldUnionHolder {
public:
    ShardExoticBitfieldUnionHolder()
        : signed_payload_{}, bool_payload_{}, enum_payload_{} {}

    void initialize(std::uint32_t signed_raw,
                    std::uint8_t bool_raw,
                    std::uint16_t enum_raw) {
        signed_payload_.raw_ = signed_raw;
        bool_payload_.raw_ = bool_raw;
        enum_payload_.raw_ = enum_raw;
    }

private:
    union {
        struct {
            std::int32_t signed_low_ : 5;
            std::uint32_t unsigned_middle_ : 11;
            std::int32_t signed_high_ : 16;
        };
        std::uint32_t raw_;
    } signed_payload_;
    union {
        struct {
            bool first_ : 1;
            bool second_ : 1;
        };
        std::uint8_t raw_;
    } bool_payload_;
    union {
        struct {
            ShardMatrixMode mode_ : 3;
            std::uint16_t tail_ : 13;
        };
        std::uint16_t raw_;
    } enum_payload_;
    WAVE_REFLECT_FRIEND;
};

template <std::size_t Tag>
class ShardTemplatePaddedUnion {
public:
    ShardTemplatePaddedUnion() : raw_(0) {}
    void initialize(std::uint64_t raw) { raw_ = raw; }

private:
    union {
        struct {
            std::uint32_t low_ : 4;
            std::uint32_t : 3;
            std::uint32_t middle_ : 5;
            std::uint32_t : 0;
            std::uint32_t high_ : 6;
        };
        std::uint64_t raw_;
    };
    std::uint32_t tag_ = static_cast<std::uint32_t>(Tag);
    WAVE_REFLECT_FRIEND;
};

union ShardMixedSizeUnion {
    std::uint8_t byte_value;
    std::uint16_t half_value;
    std::uint32_t word_value;
    std::uint64_t wide_value;
    std::uint8_t bytes[8];

    ShardMixedSizeUnion() : wide_value(0) {}
};

class ShardProtectedUnionBase {
protected:
    union {
        struct {
            std::uint16_t protected_low_ : 6;
            std::uint16_t protected_high_ : 10;
        };
        std::uint16_t protected_raw_;
    };
    WAVE_REFLECT_FRIEND;

public:
    ShardProtectedUnionBase() : protected_raw_(0) {}
    void initialize_base(std::uint16_t raw) { protected_raw_ = raw; }
};

class ShardProtectedUnionDerived : public ShardProtectedUnionBase {
private:
    ShardNamedAnonymousUnionHolder nested_;
    std::uint8_t tail_ = 0;
    WAVE_REFLECT_FRIEND;

public:
    void initialize(std::uint16_t base_raw,
                    std::uint32_t nested_raw,
                    std::uint8_t tail) {
        initialize_base(base_raw);
        nested_.initialize(nested_raw, 0x55AA55AAu);
        tail_ = tail;
    }
};

template <std::size_t Tag>
class ShardTemplateUnionCell {
private:
    union {
        struct {
            std::uint64_t low_ : 17;
            std::uint64_t high_ : 47;
        };
        std::uint64_t raw_;
    };
    std::uint32_t tag_ = static_cast<std::uint32_t>(Tag);
    WAVE_REFLECT_FRIEND;

public:
    ShardTemplateUnionCell() : raw_(0) {}
    void initialize(std::uint64_t raw) { raw_ = raw; }
};

class ShardUnionDynamicTarget
    : public wave::DynamicTraceTargetFor<ShardUnionDynamicTarget> {
private:
    ShardNamedAnonymousUnionHolder value_;
    ShardDirectBitfieldUnion direct_;
    WAVE_REFLECT_FRIEND;

public:
    void initialize(std::uint32_t value, std::uint32_t direct) {
        value_.initialize(value, 0xD00Du);
        direct_.raw = direct;
    }
};

class ShardUnionPeekSource
    : public wave::PeekTraceSourceFor<
          ShardUnionPeekSource, ShardNamedAnonymousUnionHolder> {
private:
    ShardNamedAnonymousUnionHolder value_;
    WAVE_REFLECT_FRIEND;

public:
    void initialize(std::uint32_t raw) { value_.initialize(raw, 0xC00Cu); }
    ShardNamedAnonymousUnionHolder* peek() { return &value_; }
};

struct ShardContainerBundle {
    ShardSmallLeaf c_array[3];
    ShardSmallLeaf c_matrix[2][3];
    std::array<ShardSmallLeaf, 4> std_array;
    std::array<std::array<ShardSmallLeaf, 2>, 3> std_matrix;
    wave::array<ShardSmallLeaf, 5> wave_array;
    wave::array<wave::array<ShardSmallLeaf, 2>, 3> wave_matrix;
    std::pair<ShardSmallLeaf, ShardSmallLeaf> pair_value;
};

struct ShardStressElement {
    ShardScalarBundle scalars;
    ShardSmallLeaf direct;
    std::array<ShardSmallLeaf, 2> std_array;
    wave::array<ShardSmallLeaf, 2> wave_array;
    ShardSmallLeaf c_array[2];
    ShardDeepA deep;
};

class ShardAnnotatedPointerBundle {
    WAVE_REFLECT_FRIEND

public:
    void initialize(ShardSmallLeaf* one,
                    ShardSmallLeaf* array,
                    std::size_t count,
                    std::unique_ptr<ShardSmallLeaf> owned,
                    std::unique_ptr<ShardSmallLeaf[]> owned_array,
                    std::shared_ptr<ShardSmallLeaf> shared) {
        raw_one_ = one;
        raw_array_ = array;
        count_ = count;
        owned_ = std::move(owned);
        owned_array_ = std::move(owned_array);
        shared_ = std::move(shared);
    }

private:
    std::size_t count_ = 0;
    WAVE_PTR ShardSmallLeaf* raw_one_ = nullptr;
    WAVE_PTR_ARRAY(count_) ShardSmallLeaf* raw_array_ = nullptr;
    WAVE_PTR std::unique_ptr<ShardSmallLeaf> owned_;
    WAVE_PTR_ARRAY(count_) std::unique_ptr<ShardSmallLeaf[]> owned_array_;
    WAVE_PTR std::shared_ptr<ShardSmallLeaf> shared_;
};

class ShardAnnotatedWeakOnly {
    WAVE_REFLECT_FRIEND

public:
    void set(const std::shared_ptr<ShardSmallLeaf>& value) { weak_target_ = value; }
    bool expired() const { return weak_target_.expired(); }

private:
    WAVE_PTR std::weak_ptr<ShardSmallLeaf> weak_target_;
};

template <typename T>
class ShardTemplatePointerBox {
    WAVE_REFLECT_FRIEND

public:
    void initialize(T* raw,
                    const std::shared_ptr<T>& shared,
                    const std::shared_ptr<T>& weak_owner) {
        raw_ = raw;
        shared_ = shared;
        weak_ = weak_owner;
    }

private:
    WAVE_PTR T* raw_ = nullptr;
    WAVE_PTR std::shared_ptr<T> shared_;
    WAVE_PTR std::weak_ptr<T> weak_;
};

struct ShardTemplatePointerContainer {
    ShardTemplatePointerBox<ShardSmallLeaf> nested;
};

using ShardLeafPointerAlias = ShardSmallLeaf*;

enum class ShardArrayExtent : int {
    Empty = 0,
    Pair = 2
};

class ShardPointerEdgeCases {
    WAVE_REFLECT_FRIEND

public:
    void initialize(ShardSmallLeaf* negative,
                    ShardSmallLeaf* zero,
                    ShardSmallLeaf* literal_pair,
                    ShardSmallLeaf* enum_pair,
                    ShardLeafPointerAlias alias,
                    const ShardSmallLeaf* const_raw,
                    const std::shared_ptr<const ShardSmallLeaf>& const_shared,
                    ShardSmallLeaf* freeze_target) {
        negative_count_ = -7;
        zero_count_ = 0;
        enum_count_ = ShardArrayExtent::Pair;
        negative_array_ = negative;
        zero_array_ = zero;
        literal_pair_ = literal_pair;
        enum_pair_ = enum_pair;
        alias_ = alias;
        const_raw_ = const_raw;
        const_shared_ = const_shared;
        freeze_target_ = freeze_target;
    }

    void set_expiring_weak(const std::shared_ptr<ShardSmallLeaf>& owner) {
        expired_weak_ = owner;
    }

    void replace_freeze_target(ShardSmallLeaf* target) {
        freeze_target_ = target;
    }

private:
    int negative_count_ = -1;
    std::size_t zero_count_ = 0;
    ShardArrayExtent enum_count_ = ShardArrayExtent::Empty;
    WAVE_PTR_ARRAY(negative_count_) ShardSmallLeaf* negative_array_ = nullptr;
    WAVE_PTR_ARRAY(zero_count_) ShardSmallLeaf* zero_array_ = nullptr;
    WAVE_PTR_ARRAY(2) ShardSmallLeaf* literal_pair_ = nullptr;
    WAVE_PTR_ARRAY(enum_count_) ShardSmallLeaf* enum_pair_ = nullptr;
    WAVE_PTR ShardLeafPointerAlias alias_ = nullptr;
    WAVE_PTR const ShardSmallLeaf* const_raw_ = nullptr;
    WAVE_PTR std::shared_ptr<const ShardSmallLeaf> const_shared_;
    WAVE_PTR std::weak_ptr<ShardSmallLeaf> expired_weak_;
    WAVE_PTR std::unique_ptr<ShardSmallLeaf> empty_unique_;
    WAVE_PTR std::shared_ptr<ShardSmallLeaf> empty_shared_;
    WAVE_PTR ShardSmallLeaf* freeze_target_ = nullptr;
};

struct ShardPointerCycleNode {
    std::uint32_t value = 0;
    WAVE_PTR ShardPointerCycleNode* next = nullptr;
};

// A pointer-only reflected element may have no visible topology when its
// target is null. Pointer arrays must still inspect later elements instead of
// treating an empty first/middle element as a reflection failure.
struct ShardNullablePointerOnly {
    WAVE_PTR ShardSmallLeaf* leaf = nullptr;
};

class ShardDynamicWaveTarget
    : public wave::DynamicTraceTargetFor<ShardDynamicWaveTarget> {
    WAVE_REFLECT_FRIEND

public:
    void initialize(std::uint32_t own,
                    ShardSmallLeaf* leaf,
                    ShardDynamicWaveTarget* next = nullptr) {
        own_ = own;
        leaf_ = leaf;
        next_ = next;
    }

    void set_next(ShardDynamicWaveTarget* next) { next_ = next; }

private:
    std::uint32_t own_ = 0;
    WAVE_PTR ShardSmallLeaf* leaf_ = nullptr;
    WAVE_PTR ShardDynamicWaveTarget* next_ = nullptr;
};

// This concrete runtime type is deliberately reachable only through the
// DynamicTraceTarget interface.  Root-closure generation must retain and
// register it even though no field exposes the concrete static type.
class ShardHiddenDynamicWaveTarget
    : public wave::DynamicTraceTargetFor<ShardHiddenDynamicWaveTarget> {
    WAVE_REFLECT_FRIEND

public:
    void initialize(std::uint32_t value, ShardSmallLeaf* leaf) {
        value_ = value;
        leaf_ = leaf;
    }

private:
    std::uint32_t value_ = 0;
    WAVE_PTR ShardSmallLeaf* leaf_ = nullptr;
};

template <typename PayloadT>
class ShardTemplateDynamicWaveTarget
    : public wave::DynamicTraceTargetFor<ShardTemplateDynamicWaveTarget<PayloadT> > {
    WAVE_REFLECT_FRIEND

public:
    void initialize(std::uint32_t own, const PayloadT& payload) {
        own_ = own;
        payload_ = payload;
    }

private:
    std::uint32_t own_ = 0;
    PayloadT payload_ = {};
};

// Regression coverage for type spellings that are valid only in their
// declaration context. The old registration emitter copied PrivatePayload and
// kElementCount into a namespace-scope function, where neither name is valid.
template <typename PayloadT, std::size_t ElementCount>
class ShardScopedTemplateDynamicWaveTarget
    : public wave::DynamicTraceTargetFor<
          ShardScopedTemplateDynamicWaveTarget<PayloadT, ElementCount> > {
    WAVE_REFLECT_FRIEND

public:
    void initialize(std::uint32_t own, std::uint32_t base) {
        own_ = own;
        for (std::size_t i = 0; i < ElementCount; ++i) {
            payload_[i].value = base + static_cast<std::uint32_t>(i);
        }
    }

private:
    std::uint32_t own_ = 0;
    std::array<PayloadT, ElementCount> payload_ = {};
};

class ShardScopedTemplateOwner {
    WAVE_REFLECT_FRIEND

private:
    struct PrivatePayload {
        std::uint32_t value = 0;
    };
    static const std::size_t kElementCount = 3u;
    ShardScopedTemplateDynamicWaveTarget<PrivatePayload, kElementCount> target_;

public:
    void initialize() { target_.initialize(0xEC10u, 0xEC20u); }
};

struct ShardAnnotatedWeakArrayRoot {
    std::size_t count = 0;
    WAVE_PTR_ARRAY(count) ShardAnnotatedWeakOnly* values = nullptr;
};

struct ShardPointerSlotContainers {
    WAVE_PTR ShardSmallLeaf* c_slots[2] = {};
    WAVE_PTR ShardSmallLeaf* c_matrix[2][2] = {};
    WAVE_PTR std::array<ShardSmallLeaf*, 2> std_slots = {};
    WAVE_PTR std::array<std::array<ShardSmallLeaf*, 2>, 2> std_matrix = {};
    WAVE_PTR wave::array<ShardSmallLeaf*, 2> wave_slots;
    WAVE_PTR wave::array<wave::array<ShardSmallLeaf*, 2>, 2> wave_matrix;
    WAVE_PTR std::array<ShardSmallLeaf*, 2> mixed_c_std[2];
    WAVE_PTR std::array<std::unique_ptr<ShardSmallLeaf>, 2> unique_slots;
    WAVE_PTR std::array<std::shared_ptr<ShardSmallLeaf>, 2> shared_slots;
    WAVE_PTR std::array<std::weak_ptr<ShardSmallLeaf>, 2> weak_slots;
    WAVE_PTR_ARRAY(2) std::array<ShardSmallLeaf*, 2> span_slots = {};
};

struct ShardSignalMatrixRoot {
    ShardScalarBundle scalars;
    ShardContainerBundle containers;
    ShardDeepC deep;
    ShardDerivedLeaf derived;
    ShardBitfieldBundle bitfields;
    ShardPrivateNestedAnonymousUnion nested_anonymous_union;
    ShardMultipleAnonymousUnions multiple_anonymous_unions;
    ShardVariant variant;
    ShardDirectBitfieldUnion direct_bitfield_union;
    ShardNamedAnonymousUnionHolder named_anonymous_union;
    ShardExoticBitfieldUnionHolder exotic_bitfield_unions;
    ShardTemplatePaddedUnion<11> padded_template_union;
    ShardMixedSizeUnion mixed_size_union;
    ShardMixedSizeUnion mixed_union_c_array[3];
    std::array<ShardMixedSizeUnion, 3> mixed_union_std_array;
    WAVE_PTR ShardMixedSizeUnion* mixed_union_ptr = nullptr;
    ShardProtectedUnionBase protected_union_base;
    ShardProtectedUnionDerived protected_union;
    ShardTemplateUnionCell<3> template_union;
    ShardNamedAnonymousUnionHolder union_c_array[2];
    std::array<ShardNamedAnonymousUnionHolder, 2> union_std_array;
    wave::array<ShardNamedAnonymousUnionHolder, 2> union_wave_array;
    wave::array<wave::array<ShardTemplateUnionCell<7>, 2>, 2>
        union_wave_matrix;
    ShardUnionDynamicTarget union_dynamic_inline;
    WAVE_PTR ShardUnionDynamicTarget* union_dynamic_ptr = nullptr;
    ShardUnionPeekSource union_peek_inline;
    WAVE_PTR wave::PeekTraceSource* union_peek_erased = nullptr;
    std::size_t bulk_count = 0;
    WAVE_PTR_ARRAY(bulk_count) ShardStressElement* bulk = nullptr;
    std::size_t relocation_safe_count = 0;
    WAVE_PTR_ARRAY(relocation_safe_count) ShardRelocationSafeLeaf* relocation_safe = nullptr;
    WAVE_PTR ShardSmallLeaf* alias_a = nullptr;
    WAVE_PTR ShardSmallLeaf* alias_b = nullptr;
    WAVE_PTR ShardSmallLeaf* null_pointer = nullptr;
    WAVE_PTR std::unique_ptr<ShardSmallLeaf> owning_wave_ptr;
    WAVE_PTR std::shared_ptr<ShardSmallLeaf> shared_wave_ptr;
    ShardDirectLeaf* raw_direct;
    std::unique_ptr<ShardDirectLeaf> unique_direct;
    std::shared_ptr<ShardDirectLeaf> shared_direct;
    std::weak_ptr<ShardDirectLeaf> weak_direct;
    ShardAnnotatedPointerBundle annotated;
    ShardAnnotatedWeakOnly annotated_weak;
    ShardAnnotatedWeakArrayRoot annotated_weak_array;
    ShardTemplatePointerBox<ShardSmallLeaf> template_box;
    ShardTemplatePointerContainer template_container;
    std::size_t template_array_count = 0;
    WAVE_PTR_ARRAY(template_array_count)
    ShardTemplatePointerBox<ShardSmallLeaf>* template_array = nullptr;
    ShardPointerEdgeCases pointer_edges;
    WAVE_PTR ShardPointerCycleNode* cycle_entry = nullptr;
    std::size_t first_empty_array_count = 0;
    WAVE_PTR_ARRAY(first_empty_array_count)
    ShardNullablePointerOnly* first_empty_array = nullptr;
    std::size_t middle_empty_array_count = 0;
    WAVE_PTR_ARRAY(middle_empty_array_count)
    ShardNullablePointerOnly* middle_empty_array = nullptr;
    ShardDynamicWaveTarget dynamic_inline;
    WAVE_PTR ShardDynamicWaveTarget* dynamic_ptr = nullptr;
    std::size_t dynamic_array_count = 0;
    WAVE_PTR_ARRAY(dynamic_array_count) ShardDynamicWaveTarget* dynamic_array = nullptr;
    WAVE_PTR wave::DynamicTraceTarget* hidden_dynamic = nullptr;
    ShardTemplateDynamicWaveTarget<ShardSmallLeaf> template_dynamic_inline;
    WAVE_PTR ShardTemplateDynamicWaveTarget<ShardSmallLeaf>* template_dynamic_ptr = nullptr;
    WAVE_PTR wave::DynamicTraceTarget* template_dynamic_erased = nullptr;
    ShardScopedTemplateOwner scoped_template_dynamic;
    ShardPointerSlotContainers pointer_slot_containers;
    business_sim::model::ComplexBusinessRoot business;

    // Deliberately unsupported containers must remain absent in every mode.
    std::vector<ShardSmallLeaf> ignored_vector;
    std::string ignored_string;
    const char* ignored_c_string;

    ShardSignalMatrixRoot() : raw_direct(NULL), ignored_c_string("ignored") {}
};
