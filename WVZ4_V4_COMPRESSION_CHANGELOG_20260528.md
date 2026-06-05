# WVZ4 v4 compression-first changelog (2026-05-28)

## Implemented

1. Bumped `kFormatVersion` from `3` to `4`.
2. Added periodic clock descriptors:
   - `ClockDefinition { signal_id, initial_value, period_ticks }`.
   - `Layout::clocks`.
   - New `CLKD/CLKZ` section emitted from layout.
   - Periodic clock signals are rejected if submitted as ordinary updates.
3. Changed `PathStableWvz4Recorder` default clock behavior:
   - Default clock signal id `1` is still created.
   - Clock is now stored as `CLKD(initial_value + period_ticks)`.
   - Clock high/low updates are no longer submitted into WDAT.
   - Business samples still map to `cycle * clk_period_ticks`.
4. Added sparse signal-record tile candidate:
   - Dense tile keeps full offset table.
   - Sparse tile stores only active `local_signal_delta + record_size` entries.
   - Writer chooses the smaller raw payload per tile.
5. Added per-record value codecs:
   - `FullValues`.
   - `BoolToggle`.
   - `ByteMask`.
   - Writer chooses the smallest record encoding per active signal.
6. Added stats log:
   - Default path: `<output>.wvz4.log` / `<output>.log` depending on output filename.
   - Includes section byte percentages, WDAT raw/stored bytes, dense/sparse tile ratio, time mode ratio, active signal slot ratio, value codec ratio, and clock descriptor count.
7. Fixed recorder reuse lifecycle:
   - `open()` rejects duplicate open calls.
   - `close()` resets topology/session state.
   - Same recorder object can be reused after close without duplicate track/node residue.
8. Hardened WAL replay:
   - New v4 options/layout serialization includes clock and compression options.
   - WAL payload size now has a 256 MiB sanity cap before allocation.
   - Replay stats log is written next to the final output path, not the temporary replay file.

## Compatibility note

This version intentionally breaks older WVZ4 v3 reader assumptions:

- WDAT records now include a per-record `ValueRecordCodec` byte.
- WDAT tiles may be sparse and no longer always contain a dense offset table.
- Clocks may appear only in `CLKD` and have no ordinary WDAT events.
- WAL layout/options payloads include new fields.

Update readers/viewers to parse `kFormatVersion == 4`, `CLKD`, `kWdatSparseSignalRecords`, and `kWdatPerRecordValueCodec`.

## Validation performed

Compiled and ran local smoke tests for:

- direct writer with `CLKD`, sparse tile, byte-mask record, and log output;
- `PathStableWvz4Recorder` default clock path;
- recorder close/open reuse;
- WAL write + replay;
- `WVZ4_NO_ZSTD` direct writer build;
- `wave_tap.h` inclusion under C++14;
- monitor source compilation.
