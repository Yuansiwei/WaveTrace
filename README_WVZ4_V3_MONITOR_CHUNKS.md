# WVZ4 v3 monitor + signal chunking

This package keeps the existing WVZ4 stable-topology writer features and adds two optional features:

1. **Process-level writer helper finalization**. The simulation process sends committed layout/cycle frames to a helper process over a named pipe. The helper owns the real WVZ4 writer and finalizes the file after the simulation process exits, crashes, or is killed. Recovery is guaranteed only up to the last complete cycle frame received by the helper.
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


## PathStableWvz4Recorder helper mode

`PathStableWvz4Recorder` uses writer-helper mode by default so the waveform can
survive simulation process crash/kill. The main process sends layout/cycle
frames to `wvz4_writer_monitor.exe`; the helper process owns the real
`wvz4::Writer` and writes the final `.wvz4` directly.

```cpp
PathStableWvz4Recorder::OpenConfig cfg;
cfg.file_path = "out.wvz4";
cfg.use_writer_process = true; // default
cfg.writer_process_exe_path = "wvz4_writer_monitor.exe"; // optional; auto-located when empty
recorder.open(cfg, error);
```

On normal close, the parent sends a `Finalize` frame and waits for the helper
ack. If the parent process is killed, the helper observes the parent process
handle becoming signaled and/or the named pipe breaking, then closes the writer
and emits `FOOT/footer_offset`.

The helper command line is internal to `PathStableWvz4Recorder`, but can be
started manually for protocol debugging:

```bash
wvz4_writer_monitor.exe --writer-helper --pipe <name> --parent-pid <pid> --out out.wvz4
```

Do not open an in-progress direct-writer `.wvz4` after killing the process.  A
torn direct write may contain a `WDAT` section header whose recorded length is
larger than the remaining file bytes. New reader paths reject v3+ files whose
header has no finalized `FOOT/footer_offset`.

## Crash semantics

The helper can only recover data it fully received and acknowledged. It cannot
recover data still stored in the main process memory, thread-local buffers, the
current unfinished cycle, or a half-written pipe frame. If the helper process
itself is killed while writing the final `.wvz4`, the file may still be
unfinished; the viewer will reject it until a valid `FOOT/footer_offset` exists.

WVZ4 v3+ reader paths treat a missing `FOOT/footer_offset` as an unfinished
direct/helper-writer file and reject it by default.

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

## 2026-05-21 Fast leaf sampling update

This package adds a stable-topology scalar hot path in `wave_runtime.h`:

- `BuildOptions::enable_flat_leaf_fast_table` defaults to `true`.
- `BuildOptions::emit_only_on_change` now defaults to `true` for transition-style waveform output.
- After `prepare_topology()`, the tracer builds a flat scalar leaf table (`track_id`, address/reader, scalar kind, value kind, last bits).
- The per-cycle sampling path reads this flat table directly and avoids tree traversal, path construction, unordered maps, sets, and per-track `TrackDesc` lookup.
- If the track set contains a non-scalar track, the tracer falls back to the legacy sampling path rather than silently dropping samples.

For the cleanest hot path, pre-build topology before the simulation loop:

```cpp
tracer.add_root("top", &top);
tracer.prepare_topology(0);
recorder.open_writer_if_needed(error);

for (wave::Cycle c = 0; c != end; ++c) {
    recorder.begin_cycle(c);
    tracer.sample(c);
    recorder.end_cycle(c, error);
}
```

`wave_path_wvz4_recorder.h` also reuses per-cycle `CycleSubmission` buffers instead of constructing fresh local submission vectors every cycle.  It reserves frame/submission capacity as tracks are declared.  Strict no-allocation runs should still prefer `OpenConfig::async_writer = false`, because the current async writer API copies each `CycleSubmission` into its queue.

## 2026-05-22 update: dirty `peek()` group sampling

This version adds an opt-in optimization for value-source nodes whose value is obtained through `peek()` and whose content can only change through a corresponding `write()` path.

### Core idea

Repeated `peek()` pointers are grouped by:

```text
peek address + reflected/type tag + byte width -> dirty group id
```

Different waveform paths that point to the same `peek()` address still generate their own leaves/signals. They only share the same dirty group id. When any alias writes the underlying object, the dirty hook marks the whole group dirty; sampling then updates all leaves belonging to that group.

### Runtime structure

The implementation deliberately avoids a per-group `std::vector`. All ranges live in one global range pool:

```cpp
DirtyPeekGroup.first_range -> DirtyPeekRange.next_sibling -> ...
DirtyPeekRange.leaf_begin / leaf_count -> DirtyPeekLeaf[]
DirtyPeekLeaf.track_id -> TrackEvent
```

Thus a group can have multiple ranges without per-group heap allocations.

### Thread-local dirty collection

Each thread that touches the tracing TLS automatically registers the address of its `ThreadTraceLocal`. Dirty ids are stored in the TLS object. Sampling scans all registered TLS entries, collects only entries whose `owner == this`, and clears their dirty count. Registry mutation is protected by a mutex. The hot `write()` path itself does not use a mutex, atomics, hash/map/set, or allocation.

A barrier is still required before sampling: business writes must finish before `Tracer::sample()` reads the TLS dirty buffers.

### Required usage

Enable the feature explicitly:

```cpp
wave::BuildOptions opt;
opt.enable_dirty_peek_groups = true;
opt.enable_flat_leaf_fast_table = true;
opt.enable_parallel_sampling = true; // optional, enables parallel dirty group sampling too
```

Each business thread that may call `write()` must attach after topology has been prepared/frozen:

```cpp
tracer.prepare_topology(0);
tracer.attach_current_thread_for_dirty_peek(); // call once in every business writer thread
```

A value-source object can expose a hook like this:

```cpp
class MyChannel {
public:
    const Value* peek() const noexcept { return &value_; }

    void write(const Value& v) {
        if (same_value(value_, v)) return;
        value_ = v;
        dirty_hook_.mark_dirty();
    }

    wave::WaveDirtyHook* wave_dirty_hook() noexcept { return &dirty_hook_; }

private:
    Value value_;
    wave::WaveDirtyHook dirty_hook_;
};
```

The tracer automatically binds the hook to the dirty group when the `peek()` source is expanded. If no hook is visible, or `enable_dirty_peek_groups` is false, the node remains on the normal poll path.

### Dynamic dirty group task split

After collecting dirty group ids, this version can split dirty-group sampling per cycle by group count or by group leaf weight. It uses pre-existing worker threads and preallocated vectors where possible. No hash/map/set/sort is used in the per-cycle dirty group split.


## 2026-05-28 update: WVZ4 v4 compression-first format

This package intentionally bumps the WVZ4 writer format version to `4`.  Backward compatibility with older v3 readers is not preserved, because this update prioritizes file size and cleaner compression semantics.

### Periodic clock descriptor: `CLKD`

Synthetic clocks are no longer emitted as ordinary high/low WDAT updates.  A periodic clock is stored in a dedicated `CLKD` layout section:

```text
clock_count
  signal_id
  initial_value
  period_ticks
```

The generated value toggles every `period_ticks` writer ticks.  `PathStableWvz4Recorder::OpenConfig::emit_default_clk` still creates signal id `1` named `clk` by default, but now adds a `ClockDefinition` to `Layout::clocks` instead of submitting clock transitions.  Business samples are still mapped to `cycle * clk_period_ticks`, so existing waveforms remain aligned to the synthetic clock edge grid.

### Sparse signal-record tiles

WDAT tiles now choose between dense and sparse layouts per tile.  Dense tiles keep the old all-signal offset table.  Sparse tiles write only active signals:

```text
active_count
  local_signal_delta
  record_size
records_blob
```

The sparse path is selected only when its raw payload is smaller.  This is especially useful when `signals_per_chunk` is large and only a few signals change inside a time block.

### Per-record value codecs

Each non-empty signal record now starts with a `ValueRecordCodec` byte:

```cpp
enum class ValueRecordCodec : uint8_t {
    FullValues = 0,
    BoolToggle = 1,
    ByteMask   = 2
};
```

The writer builds candidates per signal record and keeps the smallest:

- `FullValues`: time + fixed-width value for every transition.
- `BoolToggle`: bool-only, stores first value and transition times; later values toggle.
- `ByteMask`: stores first full value, then changed-byte mask plus changed bytes.

This is selected before optional outer Zstd compression, so the Zstd layer sees a smaller and lower-entropy raw payload.

### Compression statistics log

`WriterOptions::enable_stats_log` is enabled by default.  Closing a writer emits a sidecar text report at:

```text
<wvz4-output>.log
```

or at `WriterOptions::stats_log_path` if explicitly set.  The report includes:

- file/section byte percentages (`NAME/NODE/SIGT/CLKD/WDAT/FOOT`);
- WDAT raw bytes vs stored payload bytes;
- dense vs sparse tile counts;
- delta-time vs shared-time tile counts;
- active signal slot ratio;
- per-record value codec usage;
- clock descriptor count.

For helper-process writing, the stats log is written next to the final output file.

### Recorder lifecycle cleanup

`PathStableWvz4Recorder::open()` now rejects duplicate open calls and resets all topology/session state before a new session.  This fixes the previous duplicate-track failure when reusing the same recorder object for a second output file.
