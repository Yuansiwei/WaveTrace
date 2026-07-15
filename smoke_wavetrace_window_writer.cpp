#define WAVETRACE_CONFIG_PATH "build_vs/wavetrace_window_tests/runtime.json"
#include "wave_tap.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

struct WindowTop {
    std::uint32_t value = 0;
};

namespace reflect {
template<> struct is_reflected<WindowTop> : std::true_type {};
template<> struct reflected_visitor<WindowTop> {
    template<class P, class V, class G>
    static void visit(const WindowTop* object, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(object->value));
    }
};
}

int main(int argc, char** argv) {
    const int cycles = argc > 1 ? std::atoi(argv[1]) : 6;
    std::string error;
    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig cfg;
    cfg.file_path = "build_vs/legacy_name_must_not_exist.wvz4";
    cfg.emit_default_clk = true;
    cfg.clk_period_ticks = 10;
    cfg.options.compression = wvz4::Compression::None;
    cfg.options.enable_stats_log = false;
    cfg.options.target_block_span = 100;
    if (!recorder.open(cfg, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    WindowTop top;
    wave::BuildOptions options;
    options.emit_only_on_change = false;
    options.dump_leaf_distribution_after_topology = false;
    wave::Tracer tracer(recorder, options);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);
    for (int cycle = 0; cycle < cycles; ++cycle) {
        top.value = static_cast<std::uint32_t>(100 + cycle);
        if (!tap.sample_one_cycle()) {
            std::cerr << tap.last_error() << "\n";
            return 2;
        }
    }
    if (!recorder.close(error)) {
        std::cerr << error << "\n";
        return 3;
    }
    std::cout << "wavetrace_window_writer_ok cycles=" << cycles << "\n";
    return 0;
}
