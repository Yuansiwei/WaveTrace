#define WAVETRACE_CONFIG_PATH "build_vs/wavetrace_config_edge_tests/runtime.json"
#include "wavetrace_config.h"

#include <iostream>
#include <limits>

int main() {
    const wave::config::RuntimeConfig& cfg = wave::config::runtime_config();
    if (!cfg.valid) {
        std::cout << "invalid error=" << cfg.error << "\n";
        return 2;
    }
    std::cout << "valid"
              << " loaded=" << (cfg.loaded ? 1 : 0)
              << " authoritative=" << (cfg.path_is_authoritative ? 1 : 0)
              << " enabled=" << (cfg.wave_trace ? 1 : 0)
              << " file=" << cfg.wave_trace_file_name
              << " start=" << cfg.wave_trace_start
              << " end=" << cfg.wave_trace_end
              << " level_enabled=" << (cfg.wave_trace_level_enabled ? 1 : 0)
              << " level=" << cfg.wave_trace_level
              << " array_first_only=" << (cfg.wave_trace_array_first_only ? 1 : 0)
              << " stats=" << (cfg.dirty_array_stats ? 1 : 0)
              << " marks=" << (cfg.dirty_array_marks ? 1 : 0)
              << " memory=" << (cfg.memory_usage ? 1 : 0)
              << "\n";
    return 0;
}
