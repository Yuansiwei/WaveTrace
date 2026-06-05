# WVZ4 v3 monitor + signal chunking

This package keeps the existing WVZ4 stable-topology writer features and adds two optional features:

1. **Process-level WAL/monitor finalization**. The simulation process may write committed layout/cycle records to a spool file. A separate monitor/finalizer process can replay committed records and produce a finalized WVZ4 after the simulation process exits or is killed. Recovery is guaranteed only up to the last committed WAL record.
2. **Two-dimensional WDAT tiling**. Wave data is split by time block and by signal chunk. The signal chunk is computed by `signal_chunk_id = (signal_id - 1) / signals_per_chunk`. Each decompressed WDAT tile stores an offset table for all signals in that chunk, so a reader can jump to a signal's local record after decompressing only that tile.

## Writer options

```cpp
wvz4::WriterOptions opt;
opt.target_block_span = 100000;
opt.enable_signal_chunking = true;
opt.signals_per_chunk = 1024;
opt.enable_delta_time_encoding = true;
opt.enable_shared_time_table = true;
opt.implicit_zero_initial_values = true;
opt.enable_block_pipeline = true;
opt.block_pipeline_threads = 4;
```

`enable_signal_chunking` is on by default. Set it to `false` to use one tile per time block.

## Direct writer path

Existing usage still works:

```cpp
wvz4::Writer writer;
writer.open("out.wvz4", layout, opt, error);
writer.submit_cycle(submission, error);
writer.close(error);
```

`wvz4::AsyncWriter` also still works.

## WAL / monitor path

Main simulation process:

```cpp
wvz4::WalWriter wal;
wal.open("out.wvz4.spool", layout, opt, error);

// per cycle
wal.submit_cycle(submission, error);  // writes commit marker after payload

// normal finish
wal.request_finalize(error);
wal.close();
```

Monitor/finalizer process:

```cpp
std::string error;
wvz4::WalMonitor::replay_committed_spool_to_file(
    "out.wvz4.spool", "out.wvz4", error);
```

A minimal command-line finalizer is included:

```bash
wvz4_writer_monitor.exe out.wvz4.spool out.wvz4
```

## Crash semantics

The monitor can only recover data committed to the spool file. It cannot recover data still stored in the main process memory, thread-local buffers, or an unfinished cycle. A partially written WAL record is ignored because its commit marker is not set or its checksum fails.

This version intentionally does **not** handle the case where the main thread crashes but the process stays alive.

## WDAT v3 tile payload summary

Each WDAT tile contains:

```text
block_id
start_cycle
end_cycle
flags | kWdatSignalChunkTile
signal_chunk_id
first_signal_id
signal_count
[shared time table if enabled]
offset_count = signal_count + 1
delta-coded offsets into records_blob
records_blob_size
records_blob
```

For a signal:

```text
local = signal_id - first_signal_id
record = records_blob[offset[local] ... offset[local + 1])
```

An empty range means no transitions for that signal in this time block/signal chunk.


## 2026-05-21 simplified friend detection build

This build deliberately simplifies ReflectGen friend detection for performance:

- normal path: use libclang `FriendDecl` under the record;
- fallback path: scan only the current record token range for exact marker macros such as `WAVE_REFLECT_FRIEND`;
- removed from the hot path: TranslationUnit-wide macro-expansion containment scan, repeated source-path canonicalization, and per-record raw source file fallback;
- `--expanded-friend-scan` is now opt-in. It is disabled by default to avoid temporary `.ii` preprocessing files and expensive expanded-TU parsing.

If a rare macro-generated class still fails to expose private fields, rerun ReflectGen with `--expanded-friend-scan` for that case only.

## 2026-05-21 friend marker update

`WAVE_REFLECT_FRIEND` now expands to both the original `friend ::wave::ReflectAccess<T>` declaration and a private static constexpr marker member:

```cpp
using wave_reflect_friend_marker_do_not_use = ::wave::ReflectFriendMarker;
```

ReflectGen first checks the real AST `FriendDecl`, then checks for this marker member as a direct AST child of the record. This supports macro-wrapped class headers such as:

```cpp
MY_CLASS_MACRO(Foo)
{
    WAVE_REFLECT_FRIEND
private:
    int a;
};
```

The default friend path does not open source files, scan raw file text, build a whole-TU macro expansion index, or generate temporary `.ii` files. The heavy preprocessed fallback remains opt-in through `--expanded-friend-scan` only.
