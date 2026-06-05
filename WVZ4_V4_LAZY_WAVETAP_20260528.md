# WVZ4 v4 lazy WaveTap cleanup - 20260528

This version keeps the clean external-recorder WaveTap workflow, but removes
manual topology preparation from the public business-side API.

## Intended usage

```cpp
PathStableWvz4Recorder recorder;
recorder.open(cfg, error);

wave::Tracer tracer(recorder, opt);
tracer.add_root("gpu", g_GPUTop);

wave::WaveTap tap(tracer, recorder);

for (...) {
    // business cycle
    // barrier if business code is multithreaded
    tap.sample(cycle, error);  // lazy topology prepare happens here once
}

recorder.close(error);
```

## Behavior

- User code no longer calls `tap.prepare_topology()`.
- The first `tap.sample(cycle, error)` lazily calls `tracer.prepare_topology(cycle)`.
- The first `sample()` also calls `recorder.open_writer_if_needed(error)` before opening the cycle frame.
- Root registration is done only through `Tracer::add_root()`; `WaveTap` intentionally has no `add_root()` wrapper.
- `WaveTap` still does not own or close the recorder.
- The business-thread rule is unchanged: worker writes must finish before `sample()`.

## Why

The target integration code already owns `PathStableWvz4Recorder` and `Tracer`.
The desired call surface is therefore only:

```cpp
wave::WaveTap tap(tracer, recorder);
tap.sample(cycle, error);
```

This avoids requiring business code to remember an explicit topology preparation
step while keeping topology/layout errors outside a partially-open cycle frame.
