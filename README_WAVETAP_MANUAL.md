# Manual-cycle WaveTap

`wave::WaveTap` is now a clean non-owning wrapper for the workflow already used
by the GPU simulator:

```cpp
PathStableWvz4Recorder recorder;
wave::Tracer tracer(recorder, opt);
wave::WaveTap tap(tracer, recorder);
```

It has no SystemC dependency and does not derive from `sc_module`.  Business code
owns the simulation schedule and calls `tap.sample_one_cycle()` only after the
current business cycle is stable. WaveTap owns the internal cycle counter.

## Minimal usage

```cpp
#include "reflect_macro.h"   // business model headers
#include "wave_tap.h"        // simulation .cpp

struct Top {
    WAVE_REFLECT_FRIEND
    wave::WaveU32 state;
};

Top top{};
std::string error;

PathStableWvz4Recorder recorder;
PathStableWvz4Recorder::OpenConfig cfg;
cfg.file_path = "out.wvz4";
cfg.async_writer = true;
cfg.clk_period_ticks = 10;
cfg.clk_fall_offset_ticks = 5;

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
opt.enable_wave_value_dirty = true;
opt.enable_wave_value_address_hash = true;
opt.enable_wave_array_dirty = true;
opt.enable_parallel_sampling = true;

wave::Tracer tracer(recorder, opt);
tracer.add_root("top", &top);

wave::WaveTap tap(tracer, recorder);

for (wave::Cycle cycle = 0; cycle < max_cycle; ++cycle) {
    run_one_cycle(cycle);

    // In multi-threaded simulations, all business writers must be finished here.
    // Put your barrier before sample_one_cycle().
    if (!tap.sample_one_cycle()) {
        error = tap.last_error();
        // handle error
        break;
    }
}

recorder.close(error);
```

## Ownership

`WaveTap` does not own the recorder or tracer.  It does not call
`recorder.open()` or `recorder.close()` for you.  This keeps ownership explicit
and avoids two competing WaveTap modes.

## What `sample_one_cycle()` does

`tap.sample_one_cycle()` performs exactly one stable cycle frame using WaveTap's internal monotonically increasing cycle counter:

```cpp
recorder.begin_cycle(cycle);
tracer.sample(cycle);
recorder.end_cycle(cycle, error);
++internal_cycle;
```

On the first `sample_one_cycle()`, if topology has not already been prepared, `WaveTap` lazily
expands the topology and opens the WVZ4 writer layout before the cycle frame
begins.  User code does not call `prepare_topology()`.

## Multi-thread note

Sampling must happen after a business-thread barrier.  `WaveTap` is a
cycle-boundary sampler, not a concurrent snapshot mechanism.

For persistent worker threads, calling:

```cpp
tap.attach_current_thread();
```

once in each worker is still allowed as a performance hint, but it is not
required.  The runtime has a no-explicit-attach fallback for `WaveValue`/
`wave::array`, and short-lived worker threads transfer pending dirty ids before
TLS destruction.  The barrier before `sample_one_cycle()` is still required.

## WaveValue address lookup

`WaveValue<T>` remains size-preserving.  During the first lazy topology preparation the runtime
builds an open-addressing address -> dirty group hash table, so the normal write
hot path no longer performs a binary search.  The sorted-vector lookup remains a
fallback for tiny topologies and early dirty reports.
