# WVZ4 v4 wave::array dirty memory-block precheck (2026-05-29)

This version extends the dirty memory-block optimization from dirty peek groups to `wave::array<T,N>` dirty element groups.

## What changed

- Added `wave::array` dirty-element memory-block precheck.
- For each dirty array element group, direct scalar leaves under that element are grouped into compact byte ranges.
- Small gaps are allowed; mixed scalar types are allowed.
- A byte-offset to leaf-ref map is built for compact, non-overlapping blocks, avoiding binary search in the changed-byte mapping path.
- The block is only a changed-byte precheck. Event generation still uses the original typed per-leaf sampler.
- Nested `wave::array` elements get their own dirty groups while being expanded; the outer element group naturally contains only direct, non-intervening-array leaves.

## New BuildOptions

```cpp
opt.enable_wave_array_memory_block_precheck = true;
opt.wave_array_memory_block_max_gap = 7;
opt.wave_array_memory_block_min_leaf_count = 8;
opt.wave_array_memory_block_max_bytes = 4096;
opt.enable_wave_array_memory_block_simd_mask = true;
opt.wave_array_memory_block_simd_min_bytes = 64;
opt.enable_wave_array_memory_block_byte_map = true;
opt.wave_array_memory_block_byte_map_max_bytes = 4096;
opt.wave_array_memory_block_byte_map_max_overhead_per_leaf = 64;
```

## Safety notes

- Gap bytes map to `kInvalidIndex` and never produce events.
- Mixed scalar types are only compared as bytes; typed values are read by the original leaf reader.
- Overlapping leaves do not use the byte map and fall back to range lookup.
- Non-memory candidates, volatile/custom reader leaves, and unsuitable blocks fall back to the original per-leaf path.
