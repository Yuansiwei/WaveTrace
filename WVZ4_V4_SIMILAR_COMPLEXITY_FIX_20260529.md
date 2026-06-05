# WVZ4 v4 similar expansion complexity fixes (2026-05-29)

- Made flat memory-block invalidation O(1). `finish_track_create()` calls flat table invalidation once per reflected track, so it must not clear block vectors on every leaf/track creation. The real clear/rebuild now stays in `ensure_flat_memory_blocks()`.
- Removed cross-object pointer subtraction/comparison from flat memory-block formation by using `std::uintptr_t` address arithmetic, matching the dirty peek / wave::array memory-block builders.
- Optimized topology rollback: dirty group/range rebuilds now run only when the corresponding dirty structures actually changed after the checkpoint. This avoids repeated O(groups + ranges) rebuilds when rolling back empty/unsupported nodes that did not create dirty leaves.
- Existing cross-range dirty peek / wave::array memory blocks, byte maps, per-leaf epoch deduplication, and TrackEvent POD optimization remain unchanged.
