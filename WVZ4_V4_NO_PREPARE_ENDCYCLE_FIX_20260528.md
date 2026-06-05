# WVZ4 v4 WaveTap no-prepare/end_cycle fix (20260528)

This version keeps the user-facing workflow as:

```cpp
recorder.open(cfg, error);
wave::Tracer tracer(recorder, opt);
tracer.add_root("gpu", g_GPUTop);
wave::WaveTap tap(tracer, recorder);
tap.sample_one_cycle();
recorder.close(error);
```

User code does not call `prepare_topology()` and does not call
`open_writer_if_needed()`.

Changes:

- `WaveTap::sample_one_cycle()` still performs lazy topology preparation internally.
- The internal writer pre-open step is documented as a WaveTap responsibility, not a user responsibility.
- `PathStableWvz4Recorder` now allows a WVZ4 v4 clock-only layout when `emit_default_clk=true`.
  This fixes the inconsistency where the recorder had default-clock layout code but still rejected
  empty reflected topology before layout build.
- Added recorder state query helpers: `has_declared_topology()`, `has_declared_track_topology()`,
  `declared_node_count()`, `declared_track_count()`, and `writer_is_open()`.

Important note: if you expected reflected GPU signals but the file contains only `clk`, then the root
was not reflected. Check `tracer.add_root(...)`, generated reflection headers, and whether the root
pointer is non-null.
