# WVZ4 v4 topology logic fix 2026-05-28

Fixes a lazy-topology state mismatch where `Tracer::prepare_topology()` could have `nodes_exported_ == true` after an empty/no-root prepare, then later `add_root()` + expansion could create tracks but skip `NodeDecl` export. That left `PathStableWvz4Recorder` with tracks or samples but zero declared nodes, and `end_cycle()`/`open_writer_if_needed()` reported `WVZ4 recorder cannot open writer before topology is declared`.

Changes:
- `Tracer::add_root()` reopens node-declaration export by setting `nodes_exported_ = false`.
- `export_node_declarations_once()` no longer marks an empty pre-root prepare as permanently exported.
- `WaveTap` now detects the impossible state: tracer has tracks but recorder has zero declared nodes, and prints root/node/track/export diagnostics.
- Added public diagnostic getters on `Tracer`: `root_watch_count()`, `expanded_root_watch_count()`, and `node_declarations_exported()`.
