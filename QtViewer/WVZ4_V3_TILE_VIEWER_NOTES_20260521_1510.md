# WVZ4 v3 tile viewer adaptation 20260521_1510

This viewer build adapts `WaveParser4.cpp` to the WVZ4 v3 time-block + signal-chunk tile format.

## Supported WVZ4 versions

- v1 legacy WDAT block payload
- v2 fixed-width WDAT block payload, compressed layout sections, implicit zero baseline
- v3 signal-chunk WDAT tile payload

## v3 WDAT tile support

The parser accepts `kWdatSignalChunkTile` in the raw WDAT flags and decodes:

```text
block_id
start_cycle
end_cycle
flags | kWdatSignalChunkTile
signal_chunk_id
first_signal_id
signal_count
[shared time table if kWdatSharedTimeTable]
offset_count = signal_count + 1
delta-coded offsets into records_blob
records_blob_size
records_blob
```

Each selected signal is read by:

```text
local = signal_id - first_signal_id
record = records_blob[offset[local] ... offset[local + 1])
```

The local record is decoded as:

```text
transition_count
(time_code, fixed_width_value_bytes) * transition_count
```

Time code semantics reuse v2:

- `kWdatSharedTimeTable`: time code is shared-time index
- `kWdatDeltaTimes`: time code is per-signal delta
- neither flag: time code is absolute relative time inside block

## Compatibility behavior

For transitional v3 files without `kWdatSignalChunkTile`, the parser falls back to the existing v2 raw block decoder. WVZ4 v3 also receives the same implicit cycle-0 zero baseline as v2.

## Current limitation

This patch decodes v3 tiles as they appear in WDAT sections. It does not yet use a footer-level signal-chunk reverse index, because the uploaded README only specifies the tile payload summary and not a new FOOT reverse-index layout.
