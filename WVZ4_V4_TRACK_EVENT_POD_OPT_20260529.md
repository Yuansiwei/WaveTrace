# WVZ4 v4 TrackEvent POD Hot-Path Optimization 20260529

This build removes owning `std::string` fields from the hot-path `wave::TrackEvent` sample object.

## Reason
Visual Studio CPU profiling showed the sampling hot path dominated by:

- `std::vector<wave::TrackEvent>::emplace_back`
- `std::vector<wave::TrackEvent>::clear` / `_Destroy`
- `std::basic_string::~basic_string`
- `wave::Tracer::fill_flat_leaf_event`

The recorder/WVZ4 writer consumes typed payload fields (`has_u64`, `u64_value`, `has_bool`, etc.) and does not need per-sample owning path/value strings. Owning strings in every temporary sample event made worker event buffers expensive to construct and clear.

## Change
`wave::TrackEvent` is now POD-like:

- `std::string path` -> `const char* path`
- `std::string value` -> `const char* value`
- All fields have default member initializers.

The pointers are optional debug pointers. In normal WVZ4 output they are not populated in the hot path.

## Preserved

- `IWaveSink::on_sample(const TrackEvent&)` API remains.
- `PathStableWvz4Recorder` logic is unchanged.
- `WaveTap` lazy `sample_one_cycle()` logic is unchanged.
- Dirty, multithread, clock, WDAT compression, and stats-log logic are unchanged.

## Note
If external debug code previously expected `TrackEvent::path` or `TrackEvent::value` to be `std::string`, update it to treat them as nullable C-string pointers. WVZ4 production output should not rely on these fields.
