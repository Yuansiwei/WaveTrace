# WVZ4 compress bugfix 20260521

- Writer now preserves the first submitted value for every signal even when it equals the implicit zero state.
- Qt parser now rejects unknown WDAT v2 flags, invalid block time ranges, sample time overflow, and trailing bytes in raw/outer WDAT payloads.
