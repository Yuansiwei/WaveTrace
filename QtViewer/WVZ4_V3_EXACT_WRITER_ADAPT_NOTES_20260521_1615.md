# WVZ4 v3 exact writer viewer adaptation 20260521_1615

Adapted WaveParser4.cpp to the uploaded wvz4_writer_typed.h exact v3 layout.

Key fixes:

1. WDAT outer payload for v3 now reads:
   block_id, start_cycle, end_cycle, signal_chunk_id, first_signal_id, signal_count,
   compression, raw_size, encoded_size, encoded_payload.

2. Raw v3 tile header is validated against the outer WDAT chunk header.

3. FOOT v3 block index now reads:
   block_id, start_cycle, end_cycle, signal_chunk_id, first_signal_id, signal_count,
   file_offset, file_size, raw_size, compression.

4. On-demand signal loading can skip non-overlapping signal chunks before decompression.

5. v1/v2 WDAT and FOOT parsing paths remain compatible.
