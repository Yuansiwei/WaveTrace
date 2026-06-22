# WVZ4 v4 Memory Backpressure Fix 20260529

## Problem
Long runs could grow memory without bound when the sampling side was faster than writer/compression.

Note: this entry describes the earlier in-process async-writer path. Current
`PathStableWvz4Recorder` submissions always go through the anti-kill helper
process; the low-level `AsyncWriter` remains only as writer infrastructure.

Primary unbounded buffers at that time:
- The recorder-side async submission queue defaulted to unlimited.
- `wvz4::WriterOptions::block_pipeline_queue_limit` defaulted to `0` (unlimited).

## Changes
- The recorder-side async queue was capped at 256 cycle submissions.
- A 256 MiB approximate queued-byte cap was added.
- `AsyncWriter` now tracks queued approximate bytes and applies backpressure.
- `block_pipeline_queue_limit` default is now `8` block jobs.
- Oversized single cycle submissions are still allowed to make progress when the async queue is empty.

## Files changed
- `wave_path_wvz4_recorder.h`
- `wvz4_writer_typed.h`
