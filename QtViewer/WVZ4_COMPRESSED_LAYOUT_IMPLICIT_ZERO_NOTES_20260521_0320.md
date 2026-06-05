# WVZ4 compressed-layout + implicit-zero viewer patch 20260521_0320

This viewer patch matches `wvz4_writer_typed_compress_bugfix2_20260521.h`.

## Format updates supported

1. Layout sections may now be compressed when beneficial:
   - `NAME` -> `NAMZ`
   - `NODE` -> `NODZ`
   - `SIGT` -> `SIGZ`

   A compressed layout payload is decoded as:

   ```text
   u8 compression
   varuint raw_size
   varuint compressed_size
   byte[compressed_size] compressed_payload
   ```

   The raw payload is then parsed by the same NAME/NODE/SIGT parsers.

2. WVZ4 v2 now has an implicit all-zero baseline at cycle 0 for every loaded signal.
   The parser prepends `time=0,value=0` for selected/loaded signals before reading WDAT.
   If WDAT contains an explicit `time=0` sample, the existing compacting append logic replaces
   the implicit zero at the same timestamp.

## Existing strict checks retained

- WVZ4 v2 WDAT flags are validated.
- WDAT raw/outer trailing bytes are rejected.
- NODE child/sibling chains are strictly validated.
- UI tree construction still trusts `first_child/next_sibling` and does not fall back to O(N^2) parent scans.
