# WVZ4 v4 cost-breakdown log update - 20260528

This update adds raw WDAT semantic byte accounting without changing the file format or the payload encoding selected by the writer.

## Added log sections

- `[wdat_cost_breakdown_raw]`
  - `cycle_id_bytes`: cycle/time identification cost, including tile cycle range, per-record delta times, shared-time table bytes, and shared-time indexes.
  - `signal_locator_bytes`: signal identification/location cost, including chunk signal header, dense offset table, and sparse active-signal ids.
  - `value_payload_bytes`: actual value bytes.
  - `value_mask_bytes`: byte-mask selector bytes used by byte-mask value codec.
  - `record_header_bytes`: value codec tag and transition-count fields.
  - `tile_header_bytes`: block id / flags and non-semantic tile fields.
  - `tile_index_bytes`: blob-size and sparse record-size locator fields.
  - `accounted_bytes` / `unaccounted_bytes`: sanity check against `raw_payload_bytes`.

- `[wdat_cost_cycle_detail]`
  - `tile_cycle_header_bytes`
  - `cycle_delta_bytes`
  - `shared_time_table_bytes`
  - `shared_time_index_bytes`

- `[wdat_cost_signal_detail]`
  - `tile_signal_header_bytes`
  - `dense_offset_table_bytes`
  - `sparse_active_signal_bytes`
  - `sparse_record_size_bytes`

- `[wdat_cost_value_detail]`
  - `value_payload_bytes`
  - `value_mask_bytes`
  - `record_header_bytes`

- `[wdat_cost_stored_est_by_raw_ratio]`
  - Proportional estimates for compressed/stored bytes. These are diagnostic estimates only, because Zstd does not expose exact byte ownership by semantic category.

## Performance note

The accounting is collected while building the already-selected candidate payload. It only measures `out.size()` deltas around existing append operations and accumulates integer counters. It does not allocate shadow streams and does not run extra compression passes.

## Validation performed

- `wvz4_writer_monitor_main.cpp` compiles with and without `WVZ4_NO_ZSTD`.
- `wave_tap.h` compiles with `WVZ4_NO_ZSTD` under C++14.
- A direct writer smoke test generated a `.wvz4.log` with `accounted_bytes == raw_payload_bytes` and `unaccounted_bytes == 0`.
