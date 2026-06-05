# WVZ4 viewer strict tree/perf bugfix 20260521_0310

## Fixed

1. `SignalLogicTree::buildFromWaveTree()` no longer reconstructs children from `parentId` lists or any full-table fallback. It trusts the WVZ4 `firstChild/nextSibling` table and builds in O(N).
2. `WaveParser4` now validates the WVZ4 `NODE` graph before exposing it to the UI:
   - each node references an existing name;
   - node kind is valid;
   - parent/first_child/next_sibling references are valid;
   - sibling links have the same parent;
   - every non-root child is reachable from its parent's `firstChild/nextSibling` chain;
   - sibling chains are acyclic;
   - each `SIGT.node_id` points to a `SignalLeaf` with no children.
3. WVZ4 `includeAllSignalDefinitions=true` keeps all `SIGT` entries in the signal list and maps every `signal_id` to its `signalIndex`; WDAT decoding still only loads selected sample streams.
4. Removed remaining direct Qt macro tokens in local widget headers/sources by using `Q_SIGNALS`, `Q_SLOTS`, and `Q_EMIT`.
5. Kept Qt5-compatible mouse access; no `QMouseEvent::position()` remains.

## Static checks

- No O(N^2) `parentId` fallback scan remains in `MainWindow::buildFromWaveTree()`.
- `WaveParser4` accepts WVZ4 v1/v2 and enforces v2 WDAT flags/trailing-byte checks.
- `LoadOptions::timeStart/timeEnd` are used for block-level and transition-level WDAT filtering.
