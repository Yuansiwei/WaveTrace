# WVZ4 v4 block payload optimization - 20260604

## Scope

This version targets the profiler hotspot in `wvz4::Writer::build_block_payload()` and its per-signal record builders.

## Changed files

- `wvz4_writer_typed.h`
  - Reworked hot byte append helpers to use `resize + memcpy` instead of `std::vector::insert` for bulk bytes.
  - Added geometric `reserve_extra()` for byte payload vectors to avoid repeated exact-size reallocations.
  - Added exact encoded-size estimators for FullValues, BoolToggle, and ByteMask per-signal records.
  - Changed `append_signal_best_record()` from "build all candidate records into temporary vectors, then choose" to "estimate all candidate sizes, then build only the selected record once".
  - Changed sparse tile building to append all active records directly into one `blob`, while storing only `{local, record_size}` metadata per active signal.
  - Added reserve hints for dense/sparse tile final payloads.
  - Replaced WDAT encoded-block payload append with `append_vector_bytes()`.
  - Fixed a duplicated `sparse_record_size_bytes` stats accumulation line.

No changes were made to:

- `wave_runtime.h`
- `wave_tap.h`
- `wave_path_wvz4_recorder.h`
- `reflect_macro.h`
- `reflect_runtime.h`
- `ReflectGen.cpp`

## Expected impact

This specifically reduces the costs visible as:

- `wvz4::Writer::build_block_payload`
- `wvz4::Writer::build_block_payload_sparse`
- `wvz4::Writer::build_block_payload_dense`
- `wvz4::Writer::append_signal_best_record`
- `wvz4::Writer::append_signal_byte_mask_record`
- `std::vector<unsigned char>::_Insert_range`
- `std::vector<unsigned char>::_Emplace_reallocate`

The async queue / backpressure behavior is not changed in this package.

## Validation

- Header include smoke compile passed with `-std=c++14 -DWVZ4_NO_ZSTD`.
- `wvz4_writer_monitor_main.cpp` compile passed with `-std=c++14 -DWVZ4_NO_ZSTD`.
- A small deterministic no-zstd writer test produced byte-identical `.wvz4` output before and after this optimization.
