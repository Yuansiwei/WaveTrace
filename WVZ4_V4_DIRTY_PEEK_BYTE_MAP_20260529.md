# WVZ4 v4 dirty-peek memory-block byte-map optimization

This change adds an optional byte-offset to dirty leaf reference map for dirty-peek memory blocks.

## Motivation

The previous dirty-peek memory-block path mapped changed byte ranges back to leaves with a range/binary-search fallback. For compact blocks this is unnecessary: the block is small, gaps are allowed, and leaf ranges are non-overlapping in the normal struct-field case.

## Behavior

For each eligible dirty-peek memory block, the tracer builds a table:

```text
block byte offset -> DirtyPeekMemoryLeafRef index
```

Gap bytes map to `kInvalidIndex`. When SIMD/scalar byte comparison finds a changed byte range, the runtime directly looks up the affected leaf ref for each changed byte, then uses the existing per-leaf epoch deduplication before precise typed sampling.

## Safety

- The byte map is used only as a changed-byte-to-leaf locator.
- Typed values are still read by the original leaf-specific sampler.
- Mixed scalar types in the same memory block remain supported.
- Up to 7-byte gaps remain supported; gap bytes map to `kInvalidIndex`.
- If overlapping leaves are detected, the byte map is not built and the existing range lookup fallback remains active.

## Options

```cpp
opt.enable_dirty_peek_memory_block_byte_map = true;
opt.dirty_peek_memory_block_byte_map_max_bytes = 4096;
opt.dirty_peek_memory_block_byte_map_max_overhead_per_leaf = 64;
```
