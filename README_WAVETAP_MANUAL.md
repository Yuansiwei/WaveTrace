# SystemC-clocked WaveTap

中文完整接入、类型包装、性能配置、日志与 viewer 使用说明见：
[`docs/WaveTrace_使用手册_20260711.md`](docs/WaveTrace_使用手册_20260711.md)。

`wave::WaveTap` is now a clean non-owning wrapper for the workflow already used
by the GPU simulator:

```cpp
sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
PathStableWvz4Recorder recorder;
wave::Tracer tracer(recorder, opt);
wave::WaveTap tap("wave_tap", tracer, recorder, clk);
```

When SystemC is available, `WaveTap` derives from `sc_module` and automatically
samples on every falling edge of the registered `sc_clock`. It does not sample
from `start_of_simulation()`. WaveTap owns the internal cycle counter.
`SystemCStartSampler` no longer exists.

## Minimal usage

```cpp
#include <systemc>
#include "reflect_macro.h"   // business model headers
#include "wave_tap.h"        // simulation .cpp

struct Top {
    WAVE_REFLECT_FRIEND
    wave::WaveU32 state;
};

Top top{};
std::string error;
sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));

PathStableWvz4Recorder recorder;
PathStableWvz4Recorder::OpenConfig cfg;
cfg.clk_initial_value = true; // default: synthetic clk starts high
cfg.clk_period_ticks = 10;
cfg.clk_fall_offset_ticks = 5;

// clk_initial_value only controls the synthesized WVZ4 clock display phase.
// WaveTap is still triggered by the input SystemC clock's negedge_event().

// target_block_span is in writer ticks.  If emit_default_clk=true and
// clk_period_ticks=10, 10000 business cycles means 100000 writer ticks.
cfg.options.target_block_span = 10000 * cfg.clk_period_ticks;

if (!recorder.open(cfg, error)) {
    // handle error
}

wave::BuildOptions opt;
opt.enable_flat_leaf_fast_table = true;
opt.enable_flat_memory_block_precheck = true;
opt.enable_dirty_peek_groups = true;
opt.enable_dynamic_dirty_groups = true; // default: Dynamic targets report dirty
opt.enable_wave_value_dirty = true;
opt.enable_wave_value_address_hash = true;
opt.enable_wave_array_dirty = true;
opt.enable_parallel_sampling = true;

wave::Tracer tracer(recorder, opt);
tracer.add_root("top", &top);

wave::WaveTap tap("wave_tap", tracer, recorder, clk);
sc_core::sc_start();

recorder.close(error);
```

The integrated build reads `wavetrace_config.json` from the original
`WaveTracer` directory. `WaveTraceFileName` is the authoritative output name;
`WaveTraceStart`/`WaveTraceEnd` select the inclusive business-cycle window.
`WaveTrace=false` makes every automatic sample a successful no-op and lets
ReflectGen take its pre-AST placeholder-header path. Changing `WaveTrace` to
`false` only requires restarting the business process. Changing it back to
`true` requires rebuilding `cmodel` so the full reflection code is generated
and compiled.

## Ownership

`WaveTap` does not own the recorder or tracer.  It does not call
`recorder.open()` or `recorder.close()` for you.  This keeps ownership explicit
and avoids two competing WaveTap modes.

## Automatic sampling

`WaveTap` performs one frame on every clock falling edge, using its internal
monotonically increasing cycle counter:

```cpp
recorder.begin_cycle(cycle);
tracer.sample(cycle);
recorder.end_cycle(cycle, error);
++internal_cycle;
```

On the first falling-edge sample, if topology has not already been prepared, `WaveTap` lazily
expands the topology and opens the WVZ4 writer layout before the cycle frame
begins.  User code does not call `prepare_topology()`.

## Multi-thread note

The registered falling edge must be a stable business-cycle boundary. Processes
that also write traced state on the same falling edge need an explicit scheduling
contract; `WaveTap` is not a concurrent snapshot mechanism.

For persistent worker threads, calling:

```cpp
tap.attach_current_thread();
```

once in each worker is still allowed as a performance hint, but it is not
required.  The runtime has a no-explicit-attach fallback for `WaveValue`/
`wave::array`, and short-lived worker threads transfer pending dirty ids before
TLS destruction.

## WaveValue address lookup

`WaveValue<T>` remains size-preserving.  During the first lazy topology preparation the runtime
builds an open-addressing address -> dirty group hash table, so the normal write
hot path no longer performs a binary search.  The sorted-vector lookup remains a
fallback for tiny topologies and early dirty reports.
