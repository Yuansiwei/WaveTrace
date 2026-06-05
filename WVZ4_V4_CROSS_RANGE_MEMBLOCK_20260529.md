# WVZ4 v4 Cross-Range Dirty Memory Block Restore - 20260529

- Restored cross-range memory-block merging inside one dirty peek group.
- Restored cross-range memory-block merging inside one wave::array dirty group.
- Fallback leaves are still preserved for non-candidate leaves.
- Block formation still uses uintptr_t address arithmetic, max-gap, max-byte, byte-map, overlap fallback, and typed precise leaf sampling.
- No WaveTap runtime file logging was reintroduced.
