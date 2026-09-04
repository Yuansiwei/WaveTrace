#pragma once

#include "reflect_macro.h"
#include <cstddef>

struct PchPointerTarget {
    int value = 0;
};

struct ConsumerBusinessType {
    std::size_t count = 0;
    WAVE_PTR PchPointerTarget* one = nullptr;
    WAVE_PTR_ARRAY(count) PchPointerTarget* many = nullptr;
    wave::array<int, 2> values{{1, 2}};
    wave::WaveU32 scalar = 3;
};

struct PeekBusinessType : wave::PeekTraceSourceFor<PeekBusinessType, int> {
    int sampled = 17;
    int* peek() { return &sampled; }
};

struct DynamicBusinessType : wave::DynamicTraceTargetFor<DynamicBusinessType> {
    int value = 23;
};
