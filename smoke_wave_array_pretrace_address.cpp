#include "wave_runtime.h"

#include <array>
#include <cstdio>
#include <iostream>

struct PretraceBigElement {
    unsigned char bytes[512u * 1024u];
};

struct PretraceModuleLike {
    wave::array<PretraceBigElement, 4> reset_array;

    PretraceModuleLike() {
        (void)&reset_array;
        PretraceBigElement* p = reset_array.data();
        p[0].bytes[0] = 42u;
        std::array<PretraceBigElement, 4>* std_other = new std::array<PretraceBigElement, 4>();
        reset_array = *std_other;
        reset_array = std::move(*std_other);
        delete std_other;
        wave::array<PretraceBigElement, 4>* wave_other = new wave::array<PretraceBigElement, 4>();
        reset_array = *wave_other;
        reset_array = std::move(*wave_other);
        delete wave_other;
    }
};

int main() {
    std::remove("wave_runtime_error.log");
    PretraceModuleLike* module = new PretraceModuleLike();
    delete module;
    if (FILE* fp = std::fopen("wave_runtime_error.log", "rb")) {
        std::fclose(fp);
        std::cerr << "unexpected wave_runtime_error.log during pre-trace wave::array address reset\n";
        return 1;
    }
    std::cout << "wave_array_pretrace_address_ok\n";
    return 0;
}
