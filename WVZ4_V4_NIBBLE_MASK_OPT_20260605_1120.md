# WVZ4 v4 value mask optimization - 20260605_1120

## Changed files

- `wvz4_writer_typed.h`

## Motivation

The previous `ByteMask` codec spent one full selector byte per non-first transition. That is reasonable for 8-byte values, but wasteful for narrow values:

- `bool`, `uint8`, `int8`: a mask byte can cost as much as or more than the value itself.
- `uint16`, `int16`: one selector byte only protects two payload bytes and often does not win after record/time overhead.
- `int32`, `uint32`, `float`: only four selector bits are needed, but the old codec still wrote eight selector bits.

## New policy

### 1. Width <= 2 never uses changed-byte mask

For `byte_width_bytes <= 2`, the writer no longer considers `ByteMask`/mask-style value records. Candidate selection falls back to:

- `BoolToggle` / `BoolToggleStride` for valid bool toggles;
- `FullValues` / `FullValuesStride` for 8-bit and 16-bit scalar values.

This removes the known mask overhead for `int8`, `uint8`, `int16`, and `uint16`.

### 2. Width 3/4 uses packed nibble mask

Added two new value record codecs:

```cpp
NibbleMask       = 6,
NibbleMaskStride = 7
```

For 3-byte and 4-byte scalar records, the writer packs two changed-byte masks into one selector byte:

```text
selector byte = low_nibble(mask for transition t) | high_nibble(mask for transition t + 1)
```

For ordinary records, the layout is:

```text
codec, transition_count,
first_time, first_full_value,
for each pair of following transitions:
    time_0, [time_1 if present], packed_nibble_mask, changed_bytes_0, changed_bytes_1
```

For stride records, the layout is:

```text
codec, transition_count,
first_rel, stride,
first_full_value,
for each pair of following transitions:
    packed_nibble_mask, changed_bytes_0, changed_bytes_1
```

This halves selector overhead for `int32`, `uint32`, and `float` records compared with the old byte-mask codec.

### 3. Width > 4 keeps ByteMask

For `int64`, `uint64`, and `double`, the original byte-mask codec remains available because all eight selector bits can be useful.

## Codec selection behavior

The writer still estimates exact raw record size and only selects a mask codec when it is smaller than full-value encoding.

Current eligibility:

```text
BoolToggle:    bool only, width == 1, valid toggle sequence
NibbleMask:    2 < byte_width_bytes <= 4, count > 1
ByteMask:      byte_width_bytes > 4, count > 1
Stride forms:  same value codec eligibility, plus regular transition-cycle stride
```

## Stats

The stats log now prints a separate counter:

```text
nibble_mask_records=<N>
```

`value_mask_bytes` includes both byte-mask selector bytes and packed nibble selector bytes.

## Compatibility note

Files using `NibbleMask` or `NibbleMaskStride` require reader/viewer support for codec IDs 6 and 7. To force old-reader-compatible output, disable mask codecs through:

```cpp
WriterOptions options;
options.enable_value_byte_mask_encoding = false;
```

That disables both byte-mask and nibble-mask value records.

## Validation performed

Compiled successfully with:

```bash
g++ -std=c++14 -DWVZ4_NO_ZSTD -pthread smoke_compile.cpp -o /tmp/smoke_compile_mask
g++ -std=c++14 -DWVZ4_NO_ZSTD -pthread smoke_backlog.cpp -o /tmp/smoke_backlog_mask
g++ -std=c++14 -DWVZ4_NO_ZSTD -pthread wvz4_writer_monitor_main.cpp -o /tmp/wvz4_writer_monitor_mask
```

A targeted writer test with `bool`, `uint8`, `uint16`, `uint32`, and `uint64` produced:

```text
full_value_records=2
bool_toggle_records=1
byte_mask_records=1
nibble_mask_records=1
```

This verifies:

- `uint8` and `uint16` no longer choose mask records;
- `uint32` chooses the new packed nibble-mask record;
- `uint64` still chooses byte-mask when profitable;
- `bool` continues to choose bool-toggle when valid.
