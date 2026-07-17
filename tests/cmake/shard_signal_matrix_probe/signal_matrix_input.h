#pragma once

#include "reflect_macro.h"

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

union ShardVariant {
    std::uint32_t u32_value;
    float f32_value;
    ShardSmallLeaf leaf;

    ShardVariant() : u32_value(0) {}
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

struct ShardSignalMatrixRoot {
    ShardScalarBundle scalars;
    ShardContainerBundle containers;
    ShardDeepC deep;
    ShardDerivedLeaf derived;
    ShardBitfieldBundle bitfields;
    ShardVariant variant;
    wave::WavePtr<ShardStressElement*> bulk;
    wave::WavePtr<ShardSmallLeaf*> alias_a;
    wave::WavePtr<ShardSmallLeaf*> alias_b;
    wave::WavePtr<ShardSmallLeaf*> null_pointer;
    wave::WavePtr<std::unique_ptr<ShardSmallLeaf> > owning_wave_ptr;
    wave::WavePtr<std::shared_ptr<ShardSmallLeaf> > shared_wave_ptr;
    ShardDirectLeaf* raw_direct;
    std::unique_ptr<ShardDirectLeaf> unique_direct;
    std::shared_ptr<ShardDirectLeaf> shared_direct;
    std::weak_ptr<ShardDirectLeaf> weak_direct;

    // Deliberately unsupported containers must remain absent in every mode.
    std::vector<ShardSmallLeaf> ignored_vector;
    std::string ignored_string;
    const char* ignored_c_string;

    ShardSignalMatrixRoot() : raw_direct(NULL), ignored_c_string("ignored") {}
};
