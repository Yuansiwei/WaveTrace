# WVZ4 v4 backlog stage log enhancement 20260604

Purpose: add low-volume file diagnostics to identify why AsyncWriter backlog grows.

Changed file:
- `wvz4_writer_typed.h`

Added diagnostics:
- AsyncWriter now reports whether worker is inside `Writer::submit_cycle()`:
  - `in_submit`
  - `active_ms`
  - `active_cycle`
  - `active_updates`
  - `active_approx`
  - `last_cycle`
  - `last_updates`
  - `last_ms`
  - `done`
- AsyncWriter logs `async-submit-begin`, `async-submit-end`, and `async-submit-error` when the queue is already backlogged, a submission is large, or a submission is slow.
- Writer now logs stage markers:
  - `writer-submit-enter`
  - `writer-submit-before-flush`
  - `writer-commit-begin`
  - `writer-commit-single-job-built`
  - `writer-commit-jobs-built`
  - `writer-commit-end`
  - `writer-submit-after-flush`

How to read:
- `async-enqueue ... in_submit=1 active_ms=... active_cycle=...` means the worker is alive but stuck/slow inside `Writer::submit_cycle()` for that cycle.
- Last `writer-*` tag tells the exact writer stage reached before slowdown.
- `writer-commit-begin` without `writer-commit-end` points to block commit/build/encode/enqueue.
- `writer-commit-jobs-built` followed by pipeline backlog points to compression/writer pipeline.
- No `writer-*` after `async-submit-begin` suggests the worker is not entering `Writer::submit_cycle()` or symbols/build are mismatched.

Log remains bounded by the existing global limit and environment variables:
- `WVZ4_BACKLOG_LOG_FILE`
- `WVZ4_BACKLOG_LOG_MAX_LINES`
- `WVZ4_BACKLOG_LOG_DISABLE`
