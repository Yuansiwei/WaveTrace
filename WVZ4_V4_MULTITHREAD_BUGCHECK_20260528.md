# WVZ4 v4 Multithread Dirty Bugcheck (2026-05-28)

This pass focused on the dirty runtime under business-code multithreading.

## Fixed

1. **TLS owner switching could drop pending dirty ids**

   `ThreadTraceLocal` has a single `owner` pointer.  If the same worker thread
   touched objects belonging to Tracer A and then Tracer B before sampling, the
   previous implementation re-attached the TLS to B and reset the TLS counters,
   dropping A's pending dirty groups.

   Fix: before a TLS owner switch, pending dirty ids are transferred to the old
   owner's retired-dirty queue via `preserve_tls_dirty_before_owner_switch_()`.

2. **Explicit detach could drop pending dirty ids**

   If a worker called `detach_current_thread_for_dirty_peek()` after dirtying
   values but before `sample()`, the pending TLS dirty ids were cleared.

   Fix: `detach_current_thread_for_dirty_peek()` now adopts pending dirty ids
   into the tracer's retired-dirty queue before detaching.

## Verified

- Persistent worker with explicit attach: pass.
- Short-lived worker without explicit attach, `join()` before `sample()`: pass.
- Multiple short-lived workers dirtying different fields: pass.
- Same worker touches two different Tracers before sampling: pass.
- Explicit detach after dirty, before sample: pass.
- ThreadSanitizer pass for distinct-field multithread dirty test.
- ThreadSanitizer pass for two-Tracer owner-switch test.
- `wvz4_writer_monitor_main.cpp` compiles with `WVZ4_NO_ZSTD`.

## Required usage model

Sampling still requires a cycle barrier.  Business threads may mark dirty in
parallel, but `Tracer::sample()` must run after those writes are complete.  The
runtime does not make ordinary business fields atomic; writing the same
`WaveValue<T>` concurrently from multiple threads remains a business-side data
race unless the model provides its own synchronization.
