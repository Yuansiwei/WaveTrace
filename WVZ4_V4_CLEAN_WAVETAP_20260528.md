# WVZ4 v4 clean WaveTap update - 20260528

This package replaces the mixed owning/non-owning WaveTap wrapper with a single
clean external-recorder workflow:

```cpp
PathStableWvz4Recorder recorder;
wave::Tracer tracer(recorder, opt);
wave::WaveTap tap(tracer, recorder);
```

`WaveTap` no longer owns or opens/closes `PathStableWvz4Recorder`.  The business
program remains responsible for:

1. `recorder.open(cfg, error)` before constructing/using WaveTap.
2. Registering roots before first topology preparation or sample.
3. `recorder.close(error)` after the final sample.

`WaveTap::sample(cycle, error)` now always performs the recorder frame sequence:

```cpp
recorder.begin_cycle(cycle);
tracer.sample(cycle);
recorder.end_cycle(cycle, error);
```

On the first sample it also calls `prepare_topology(cycle, error, true)`, which
expands the topology and opens the WVZ4 writer layout before the cycle begins.
This catches missing-root/layout errors before an open frame and builds WaveValue
address hashes before the first business sampling cycle.

Worker threads still need a barrier before `sample()`.  `WaveTap` is a
cycle-boundary sampler, not a concurrent snapshot mechanism.
