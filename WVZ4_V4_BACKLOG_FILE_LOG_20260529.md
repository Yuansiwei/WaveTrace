# WVZ4 v4 Writer Backlog File Log

## Purpose

Adds a low-volume writer backlog diagnostic log file for memory growth analysis.
The log is intended to distinguish:

- AsyncWriter queue buildup
- block compression/encoding backlog
- encoded-result/file-writer backlog
- ordered-write pending backlog
- worker startup/state issues

## Files changed

- `wvz4_writer_typed.h`
  - Backlog diagnostics now write to a bounded file instead of `stderr`.
  - Default path: `wvz4_writer_backlog.log`.
  - Environment variables:
    - `WVZ4_BACKLOG_LOG_DISABLE=1` disables the backlog log.
    - `WVZ4_BACKLOG_LOG_FILE=<path>` changes the output path.
    - `WVZ4_BACKLOG_LOG_MAX_LINES=<N>` changes the global line limit, default `512`.
  - Existing per-object throttles remain in place: async queue logs at 64/128/256/... and pipeline queues at 8/16/32/...
  - Fixed a duplicate `std::lock_guard` on `pipeline_.mutex` in the file-writer error path.

## Notes

The log is not per-cycle. It is threshold-triggered and globally line-limited to avoid log explosions.
