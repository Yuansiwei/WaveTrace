# WVZ4 v4 Memory Backpressure Fix 20260529

## Problem
Long runs could grow memory without bound when the sampling side was faster than writer/compression.

Primary unbounded buffers:
- `PathStableWvz4Recorder::OpenConfig::async_writer_queue_limit` defaulted to `0` (unlimited).
- `wvz4::WriterOptions::block_pipeline_queue_limit` defaulted to `0` (unlimited).

## Changes
- `async_writer_queue_limit` default is now `256` cycle submissions.
- Added `async_writer_queue_bytes_limit`, default `256 MiB`, as an approximate byte cap.
- `AsyncWriter` now tracks queued approximate bytes and applies backpressure.
- `block_pipeline_queue_limit` default is now `8` block jobs.
- Oversized single cycle submissions are still allowed to make progress when the async queue is empty.

## Files changed
- `wave_path_wvz4_recorder.h`
- `wvz4_writer_typed.h`
