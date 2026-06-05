# WVZ4 v4 WaveTap API cleanup 2026-05-28

This package removes `wave::WaveTap::add_root()`. Root ownership is now unambiguous:

```cpp
wave::Tracer tracer(recorder, opt);
tracer.add_root("gpu", g_GPUTop);

wave::WaveTap tap(tracer, recorder);
tap.sample_one_cycle();
```

`WaveTap` is only the cycle sampler wrapper. It does not own topology and does not forward root registration. This avoids two public root-registration paths and prevents late-layout mutation confusion.

Also tightened the lazy-topology state machine: `topology_prepared_` is set only after lazy prepare checks pass and `recorder.open_writer_if_needed()` succeeds. A failed/empty lazy expansion is retried on the next `sample_one_cycle()` instead of being cached as prepared.
