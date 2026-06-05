# WVZ4 v4 backlog diagnostic log (2026-05-29)

## Purpose

Adds low-volume stderr diagnostics for writer-side backlog investigation.  The log is intended for cases where
`AsyncWriter::queue_` grows unexpectedly and it is unclear whether the bottleneck is async consumption, block compression,
block ordering, or file writing.

## Files changed

- `wvz4_writer_typed.h`
  - Adds `[wvz4-backlog]` diagnostics to `AsyncWriter`.
  - Adds `[wvz4-backlog]` diagnostics to the block pipeline.
  - Adds bounded counters and threshold-based logging; no per-cycle spam.

No WaveTap runtime file logging was reintroduced.

## Log behavior

Output goes to `stderr` only.  It is automatically rate-limited:

- AsyncWriter logs at queue size thresholds: 64, 128, 256, 512, ...
- Pipeline logs at job/result/pending thresholds: 8, 16, 32, 64, ...
- Each subsystem has a hard cap of 128 diagnostic lines.

Disable all backlog diagnostics with:

```bat
set WVZ4_BACKLOG_LOG_DISABLE=1
```

## Key tags

- `async-open`: AsyncWriter opened and created its worker thread.
- `async-worker-start`: AsyncWriter worker thread actually entered `worker_loop()`.
- `async-enqueue`: async queue crossed a backlog threshold.
- `async-dequeue`: worker is consuming but queue is still high.
- `pipeline-start`: block pipeline started.
- `pipeline-compression-worker-start`: compression worker entered its loop.
- `pipeline-file-writer-start`: file writer thread entered its loop.
- `pipeline-job-enqueue`: compression job queue crossed a threshold.
- `pipeline-result-ready`: encoded result queue crossed a threshold.
- `pipeline-file-pending`: file writer pending map crossed a threshold.

## Interpretation

- `async-enqueue` appears but no `async-worker-start`: worker thread is not running or code/symbols are mismatched.
- `async-enqueue` grows while `async-dequeue` does not: async worker is not consuming.
- `pipeline-job-enqueue` grows: compression/encoding is behind.
- `pipeline-result-ready` grows: file writer is behind.
- `pipeline-file-pending` grows: block results are arriving out of order; an earlier block is slow or missing.
