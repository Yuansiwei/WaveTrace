# WVZ4 v4 logic bugfix 20260529

Changes in this package:

1. Dirty peek memory-block builder now uses integer address arithmetic (`uintptr_t`) instead of subtracting unrelated C++ object pointers. This removes UB risk when sorting and grouping dirty peek leaves.
2. Dirty peek memory-block grouping is now performed per dirty range, not across all ranges in a group. This avoids merging leaves separated by nested/alias ranges.
3. Wave array memory-block grouping is now also performed per dirty range, preserving the "direct leaves only" boundary and avoiding merging across nested wave::array ranges.
4. Wave array byte-map lookup now falls back to range lookup if the byte map is inconsistent or unavailable instead of silently returning and losing events.

Unchanged:
- TrackEvent remains POD-like; no std::string on the event hot path.
- WaveTap runtime file logging remains removed.
- Existing recorder/writer/WVZ4 compression logic remains unchanged.
