# WVZ4 v4 Logic Bugfix 2 - 20260529

- Rechecked cross-range dirty memory block logic for dirty peek and wave::array paths.
- Added per-leaf memory sampling epoch guards in addition to per-leaf-ref guards.
  This prevents duplicate events if the same dirty leaf is reachable through multiple
  refs/blocks/fallback paths due to aliasing, duplicate ranges, or future overlapping
  layouts.
- Added shadow-buffer bounds guards before dirty peek / wave::array memory-block sampling.
- Added defensive overflow guards before growing dirty peek / wave::array memory-block
  shadow buffers.
- Kept cross-range memory-block merging enabled inside one dirty group.
- Kept byte-map fast path and range-lookup fallback.
- No WaveTap runtime file logging was reintroduced.
