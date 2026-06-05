# WVZ4 v4 Shared-Time Incremental + Stride-Time Record Optimization

## Scope

Implemented in this package:

- `wvz4_writer_typed.h`
  - Incremental per-chunk shared-time collection.
  - Removed the hot-path dependency on `collect_shared_times()` scanning all transitions and sorting.
  - Added size-oriented stride-time record codec variants.
  - Added `WriterOptions::enable_stride_time_record_encoding`.
  - Updated option serialization/deserialization.

Not changed in this package:

- No AsyncWriter queue protection changes.
- No queue-limit default forcing.
- No `wave_runtime.h` changes.
- No `wave_tap.h` changes.
- No `wave_path_wvz4_recorder.h` changes.

## Incremental shared-time collection

Before this change, shared-time candidates called `collect_shared_times()` during block build:

1. Scan all transitions in the signal chunk.
2. Push every transition cycle into a temporary vector.
3. Sort.
4. Unique.

This was expensive for large blocks and showed up in profiling as `collect_shared_times()` / `std::sort`.

Now, `Writer::submit_cycle()` records shared times incrementally only when a real transition is appended. Since submitted cycles are strictly increasing, each chunk's shared-time vector is naturally sorted and unique.

Shared times are kept per signal chunk, not globally, so shared-time table size remains chunk-local and does not include unrelated chunks.

## Stride-time record codec variants

Added new record codec ids:

- `FullValuesStride = 3`
- `BoolToggleStride = 4`
- `ByteMaskStride = 5`

These variants keep the value payload identical to the corresponding base codec, but replace the per-transition time stream with:

- `first_rel`
- `stride`

This reduces cycle/time bytes for signals whose transitions are evenly spaced.

The option is enabled by default:

```cpp
WriterOptions::enable_stride_time_record_encoding = true;
```

Readers/viewers must support these new codec ids before consuming files written with the option enabled. To keep old-reader compatibility, set:

```cpp
options.enable_stride_time_record_encoding = false;
```

## Validation performed

- `smoke_compile.cpp` compiled with `-std=c++14 -DWVZ4_NO_ZSTD`.
- `wvz4_writer_monitor_main.cpp` compiled with `-std=c++14 -DWVZ4_NO_ZSTD`.
- A deterministic writer smoke test completed and generated a WVZ4 file using stride codecs.
- With `enable_stride_time_record_encoding=false`, the new writer produced byte-identical output to the previous writer on the deterministic no-zstd smoke test.

