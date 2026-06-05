# WVZ4 v4 WaveValue Address Hash Update (2026-05-28)

This update replaces the WaveValue dirty hot-path address lookup with a freeze-time
open-addressing hash table.

## What changed

- Added `BuildOptions::enable_wave_value_address_hash`, default `true`.
- Added `BuildOptions::wave_value_address_hash_min_entries`, default `16`.
- During `Tracer::prepare_topology()`, the runtime now builds a flat
  open-addressing table for `WaveValue` address -> dirty group id.
- `WaveValue<T>::operator=()` still reports only its object address, so
  `sizeof(WaveValue<T>) == sizeof(T)` remains unchanged.
- The existing sorted vector binary search remains as a fallback for tiny
  topologies, disabled hash mode, and defensive early dirty reports before
  topology preparation.
- The old cross-object raw pointer `<` sort comparison was replaced by
  `std::less<const void*>`.

## Hot-path effect

Before:

```text
WaveValue assignment -> address -> binary search over dirty_wave_value_addr_table_
```

After topology preparation:

```text
WaveValue assignment -> address -> hash probe -> group_id -> TLS dirty id push
```

The hash table uses power-of-two capacity and load factor <= 0.5 to keep average
probe count low.  It is built outside the business write path.

## Compatibility

- Public `WaveValue<T>` object layout is unchanged.
- Existing business code does not need to change.
- For best hot-path performance, still call `prepare_topology()` before worker
  threads start modifying `WaveValue` fields, then call `attach_current_thread()`
  in each worker thread.
