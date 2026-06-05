# WVZ4 v4 record-time / payload candidate optimization (20260604)

## Touched files

- `wvz4_writer_typed.h`: implementation changes.
- `WVZ4_V4_RECORD_TIME_PAYLOAD_OPT_20260604.md`: this note.

No changes were made to `wave_runtime.h`, `wave_tap.h`, `wave_path_wvz4_recorder.h`, `reflect_macro.h`, `reflect_runtime.h`, or `ReflectGen.cpp`.

## Changes

1. Added fast paths for unsigned LEB128 varuint writing:
   - 1-byte values use a direct push.
   - 2-byte values reserve once and write two bytes directly.
   - Larger values fall back to the original loop.
   - The byte stream remains identical.

2. Replaced per-transition shared-time `lower_bound` with a monotonic shared-time cursor inside a signal record.
   - Signal transitions are already sorted by cycle.
   - Shared-time indices therefore move monotonically.
   - Debug assertions check that the shared time exists.

3. Added combined per-signal record-size selection.
   - Full-value, bool-toggle, and byte-mask costs are estimated in one transition scan.
   - Time-size calculation is shared across codecs.
   - Byte-mask estimation early-rejects when its value payload cannot beat full-value.

4. Changed block payload candidate selection to estimate candidate sizes first and build only the selected payload.
   - Dense default, sparse default, dense shared-time, and sparse shared-time are no longer all fully materialized just to compare sizes.
   - The chosen payload is still built through the existing writer path, preserving statistics and format.

5. Kept WVZ4 format compatibility.
   - No new time codec was added.
   - No reader-side change is required.

## Validation performed

- Header include smoke compile with `-DWVZ4_NO_ZSTD` passed.
- `wvz4_writer_monitor_main.cpp` compile with `-DWVZ4_NO_ZSTD` passed.
- A deterministic no-zstd writer test produced byte-identical `.wvz4` output compared with `wvz4_v4_block_payload_opt_20260604_1815.zip`.
