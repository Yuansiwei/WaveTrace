#include "wave_path_wvz4_recorder.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::uint32_t declaration_count = 1000000u;
    if (argc == 3 && std::string(argv[1]) == "--count") {
        char* end = NULL;
        const unsigned long long parsed = std::strtoull(argv[2], &end, 10);
        if (!end || *end != '\0' || parsed == 0 || parsed > 0xffffffffull) {
            std::cerr << "invalid --count\n";
            return 1;
        }
        declaration_count = static_cast<std::uint32_t>(parsed);
    } else if (argc != 1) {
        std::cerr << "usage: smoke_bitset_recorder_scale [--count N]\n";
        return 1;
    }
    PathStableWvz4Recorder recorder;
    recorder.on_bitset_declarations_begin_fast(declaration_count);
    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    for (std::uint32_t i = 1; i <= declaration_count; ++i) {
        recorder.on_bitset_declared_fast(i, i, 64u, 1u);
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    if (recorder.debug_state_summary().find("last_error=") != std::string::npos) {
        std::cerr << recorder.debug_state_summary() << "\n";
        return 2;
    }

    PathStableWvz4Recorder unordered;
    unordered.on_bitset_declared_fast(10u, 1u, 64u, 1u);
    unordered.on_bitset_declared_fast(2u, 2u, 64u, 1u);
    unordered.on_bitset_declared_fast(6u, 3u, 64u, 1u);
    unordered.on_bitset_declared_fast(2u, 4u, 64u, 1u);
    if (unordered.debug_state_summary().find("duplicate bitset node_id") == std::string::npos) {
        std::cerr << "out-of-order duplicate bitset node was not rejected\n";
        return 3;
    }

    std::cout << "bitset_recorder_scale_ok declarations=" << declaration_count
              << " elapsed_ms=" << elapsed_ms << "\n";
    return 0;
}
