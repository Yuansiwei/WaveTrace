# WVZ4 v4 WaveTap sample_one_cycle cleanup - 20260528

This revision makes `wave::WaveTap` expose only the no-argument sampling API
needed by the GPU simulator integration:

```cpp
wave::WaveTap tap(tracer, recorder);

// after each stable business cycle / barrier
tap.sample_one_cycle();
```

## Semantics

- `WaveTap` owns an internal monotonically increasing business-cycle counter.
- The first `sample_one_cycle()` records cycle 0.
- A successful call advances the internal counter by one.
- A failed call does not advance the counter; read `tap.last_error()`.
- User code does not pass a cycle number and does not call `prepare_topology()`.
- Topology is still lazily prepared before the first cycle frame begins.
- `WaveTap` still does not own or close `PathStableWvz4Recorder`.

## Removed from the public sampling surface

- `sample(Cycle cycle, std::string& error)`
- `sample(Cycle cycle)`

This avoids accidental double-counting, skipped cycles, or unit confusion between
business cycles and writer ticks.
