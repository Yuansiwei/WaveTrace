# WVZ4 v2 viewer patch notes 20260521_0309

This viewer package is based on `QtSignalViewer_perf_fix_20260521_0245.zip` and is updated to match `wvz4_writer_typed_compress_bugfix_20260521.h`.

## Parser compatibility

- Accepts WVZ4 format versions 1 and 2.
- WVZ4 v2 raw WDAT decoding now supports:
  - `kWdatFixedValueWidth`
  - `kWdatDeltaTimes`
  - `kWdatSharedTimeTable`
- WVZ4 v2 no longer expects per-transition `byte_count`; value width is taken from `SIGT.ValueType`, matching the writer.
- Keeps version-1 decoding path for older WVZ4 test files.

## Validation and corruption detection

- Rejects unknown WVZ4 v2 WDAT flags.
- Rejects invalid block time ranges.
- Rejects raw/header block metadata mismatch.
- Rejects sample relative-time overflow or out-of-block timestamps.
- Rejects trailing bytes in outer WDAT payload and raw WDAT payload.
- Rejects malformed varuint overflow in the 10th byte.
- Adds trailing-byte checks for NAME/NODE/SIGT/FOOT sections.
- Validates SIGT `ValueType`, `Radix`, and `bit_width <= type capacity`.

## Performance notes

- Preserves the previous O(N) tree-building fix from `QtSignalViewer_perf_fix_20260521_0245`.
- WDAT block-level time-window filtering is now wired through `WaveParser4::LoadOptions::timeStart/timeEnd`; out-of-window blocks skip decompression.
- Fixed-width v2 decoding skips unselected signal values using a precomputed `signal_id -> byte_width` map.

## Build note

- `app.rc` no longer hard-requires missing `app.ico`; the ICON line is commented out while keeping the manifest resource.

Update 20260521_0320: later writer `wvz4_writer_typed_compress_bugfix2_20260521.h` adds NAMZ/NODZ/SIGZ compressed layout sections and WVZ4 v2 implicit zero baseline support. See `WVZ4_COMPRESSED_LAYOUT_IMPLICIT_ZERO_NOTES_20260521_0320.md`.
