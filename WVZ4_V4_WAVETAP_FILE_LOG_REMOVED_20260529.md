# WVZ4 v4 WaveTap file logging removed 20260529

This package removes WaveTap's standalone runtime file logging code entirely.

Changes are limited to the WaveTap wrapper:

- Removed environment-variable controlled WaveTap file logging.
- Removed runtime log path/enabled setters.
- Removed log stream state and file write calls.
- Kept `last_error()` diagnostics and topology state checks.
- Kept sampling, lazy topology, recorder open, dirty, compression, and writer logic unchanged.

WVZ4 writer statistics logging remains controlled by writer options.
