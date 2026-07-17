#define WAVETRACE_CONFIG_PATH "tests/wavetrace_config_disabled.json"
#include "wave_tap.h"

#include <cstdint>
#include <iostream>

struct DisabledTop {
    std::uint32_t value = 7;
};

namespace reflect {
template<> struct is_reflected<DisabledTop> : std::true_type {};
template<> struct reflected_visitor<DisabledTop> {
    template<class P, class V, class G>
    static void visit(const DisabledTop* object, P&& on_ptr, V&&, G&&) {
        on_ptr("value", std::addressof(object->value));
    }
};
}

int main() {
    DisabledTop top;
    PathStableWvz4Recorder recorder;
    wave::BuildOptions options;
    options.print_cycle_progress = true;
    options.print_cycle_progress_period = 1;
    wave::Tracer tracer(recorder, options);
    tracer.add_root("top", &top);
    wave::WaveTap tap(tracer, recorder);

    for (int i = 0; i != 4; ++i) {
        ++top.value;
        if (!tap.sample_one_cycle()) {
            std::cerr << tap.last_error() << "\n";
            return 1;
        }
    }
    if (tap.next_cycle() != 4 || tap.is_topology_prepared() || tracer.root_watch_count() != 0) {
        std::cerr << "WaveTrace=false did not remain a topology-free no-op\n";
        return 2;
    }
    std::cout << "wavetrace_disabled_ok cycles=" << tap.next_cycle() << "\n";
    return 0;
}
