# WVZ4 v4 bugfix check 2026-05-28

Based on `wvz4_v4_wavevalue_hash_20260528_1349`.

## Fixed

1. **Short-lived worker thread dirty loss**
   - Previous behavior: if a worker thread marked `WaveValue` / `wave::array` dirty and then exited before `Tracer::sample()`, its thread-local dirty buffer was destroyed and the sample missed those dirty groups.
   - New behavior: `ThreadTraceLocal::~ThreadTraceLocal()` transfers remaining dirty ids into the owning tracer's retired-dirty queues before unregistering TLS. The next `sample()` drains those queues.

2. **`WaveValue` / `wave::array` dirty required explicit worker attach**
   - Previous behavior: the static write callback only used `tls.owner`; a worker thread that never called `attach_current_thread_for_dirty_peek()` had `tls.owner == nullptr`, so dirty marks were silently ignored.
   - New behavior: active tracers are registered globally. If the current thread has no owner or the owner does not match the address, the callback resolves the owner through the prepared address lookup table and then uses the normal TLS dirty path. The hot path after resolution still uses `tls.owner`.

## Re-tested

- `wvz4_writer_monitor_main.cpp` compiles with and without `WVZ4_NO_ZSTD`.
- `WaveValue` address hash dirty smoke test passes.
- Short-lived worker threads without explicit attach preserve dirty updates after `join()`.
- `WaveTap` + WVZ4 output + stats log smoke test passes.
- ASAN/UBSAN smoke tests pass for the dirty and WaveTap cases.

## Still noted

- `wvz4_writer_typed.h` still has `-Wmisleading-indentation` warnings in some deserialize helpers. They are warning-level style issues, not observed runtime failures in the smoke tests.
- `TrackDecl::path` is empty by default unless `BuildOptions::emit_track_decl_path = true`; this is intentional for performance/WVZ4 layout mode but affects debug-only in-memory tests.
