# WVZ4 v4 WaveTap lazy writer-open retry fix (2026-05-28)

Fixes a lazy WaveTap state-machine bug where `topology_prepared_ == true`
only meant that `Tracer::prepare_topology()` had been attempted.  If the
subsequent `PathStableWvz4Recorder::open_writer_if_needed()` failed,
`writer_preopened_` remained false; however, a later `sample_one_cycle()`
returned early from `ensure_topology_prepared_()` because `topology_prepared_`
was already true.  The call then reached `end_cycle()`, which triggered the
recorder's defensive writer-open path and reported a misleading late error such
as:

    WVZ4 recorder cannot open writer before topology is declared

The fix makes `ensure_topology_prepared_()` always retry `open_writer_if_needed()`
when `writer_preopened_ == false`, even if topology has already been prepared.
It also appends `tracer_nodes` and `tracer_tracks` counts to the lazy-open error
message to make empty-root / missing-reflection cases easier to diagnose.
