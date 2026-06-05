# WVZ4 v4 expand complexity fix 2026-05-29

## Problem

During topology expansion, `record_dirty_wave_array_leaf_for_track()` and `record_dirty_peek_leaf_for_track()` call the memory-block invalidation helper once per reflected leaf.  The previous invalidation helper cleared memory-block vectors and walked every dirty group to reset cached block ranges.

For large `wave::array` objects this made expansion approximately O(groups * leaves), and Visual Studio profiling showed the hot path stuck in `invalidate_dirty_wave_array_memory_blocks()` while expanding `unsigned char[128]` leaves.

## Fix

`invalidate_dirty_peek_memory_blocks()` and `invalidate_dirty_wave_array_memory_blocks()` are now O(1): they only mark the memory-block cache as invalid.

The expensive cleanup/rebuild already happens lazily in:

- `ensure_dirty_peek_memory_blocks()`
- `ensure_dirty_wave_array_memory_blocks()`

These functions clear the old vectors and reset each group's memory block ranges once before rebuilding.

## Behavior

No waveform semantics are changed.  Memory block cache invalidation is deferred until the first dirty sampling pass that needs the cache.

