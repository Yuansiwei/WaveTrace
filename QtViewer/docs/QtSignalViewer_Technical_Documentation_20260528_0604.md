# QtSignalViewer / WVZ4 Waveform System Technical Documentation

**Version package:** `20260528_0604`  
**Source baseline:** `QtSignalViewer_stringless_logic_bugcheck2_20260527_2015.zip`  
**Primary target:** Qt5 / Visual Studio x64 waveform viewer and WVZ4 stable-topology waveform format  

## 1. Overall Purpose

The waveform system is designed to record, store, load, compare, and visualize cycle-level simulation waveforms at large scale. It contains a typed waveform writer, a stable-topology recorder, a WVZ4 parser, and a Qt-based viewer. The system focuses on large signal counts, fast on-demand loading, readable signal hierarchy browsing, and practical debugging workflows.

The viewer supports normal single-file waveform inspection and two-file comparison. In comparison mode, only same-path signals with actual value differences are retained, and different time regions are highlighted on the waveform canvas.

## 2. Main Deliverables

| Area | Deliverable | Purpose |
|---|---|---|
| File format | WVZ4 v3 | Stable-topology waveform storage with signal chunk tiles. |
| Writer | `wvz4_writer_typed.h` | Typed writer, v3 chunked WDAT, optional WAL monitor support. |
| Recorder | `wave_path_wvz4_recorder.h` | Stable-path recorder for reflected simulation objects. |
| Parser | `WaveParser4.cpp/.h` | Reads WVZ4 metadata, WDAT tiles, compressed layouts, and footers. |
| Viewer | QtSignalViewer | Tree browsing, active signal list, waveform canvas, comparison workflow. |
| Compatibility | WVZ/WVZ2/WVZ3/WVZ4 readers | Keeps legacy waveform support while enabling WVZ4 v3. |

## 3. Architecture Overview

The system is separated into four layers:

1. **Recording layer**: converts simulation track declarations and samples into typed waveform submissions.
2. **Storage layer**: writes stable topology and time/signal-tiled waveform payloads into WVZ4.
3. **Loading layer**: parses metadata eagerly and waveform data selectively.
4. **Viewer layer**: presents the signal tree, active signal table, waveform canvas, search, and comparison features.

The current design intentionally separates topology metadata from waveform samples. Signal definitions and tree nodes are loaded as lightweight metadata, while sample data is loaded only when required by display, selection, or comparison workflows.

## 4. Source Tree and Key Files

| File | Role |
|---|---|
| `main.cpp` | Qt application entry point and startup setup. |
| `MainWindow.h/.cpp` | Main UI orchestration, file loading, search, compare, active-signal workflow. |
| `WaveTypes.h` | Shared waveform model: signals, samples, raw fields, diff regions, tree metadata. |
| `WaveCanvas.h/.cpp` | Waveform rendering, cursor, zoom, range selection, change/diff jumping. |
| `ActiveSignalListWidget.h/.cpp` | Active signal table, value display, drag/drop, width splitter, multi-select. |
| `WaveParser4.h/.cpp` | WVZ4 parser including compressed sections and v3 signal chunk tiles. |
| `WaveParser3.h/.cpp` | WVZ3 support. |
| `WaveParser2.h/.cpp` | WVZ2 support. |
| `WaveParser.h/.cpp` | Older JSON / WVZ compatibility. |
| `wvz4_writer_typed.h` | WVZ4 v3 typed writer and WAL monitor/finalizer support. |
| `wave_path_wvz4_recorder.h` | Stable-path WVZ4 recorder for reflected waveform capture. |
| `icons/compare.png` | Toolbar icon for two-file comparison. |

## 5. Core Runtime Data Model

### 5.1 WaveSample

`WaveSample` stores the sample timestamp and value state. The latest optimization avoids constructing string values in WVZ4 hot paths. WVZ4 samples are stored as raw fields first:

- `time`: sample cycle or tick.
- `rawBits`: raw little-endian scalar bits stored in a 64-bit field.
- `rawFieldsReady`: indicates that `rawBits` is authoritative.
- `isZ`: high-impedance marker for formats that support it.
- `isAbsent`: comparison-only state for file-end or missing-side difference regions.
- `value`: legacy text fallback for older formats.

### 5.2 WaveSignal

`WaveSignal` represents a single visualizable signal. Important fields include:

- `name`: leaf display name or compare-side leaf name.
- `fullName`: full path when available or computed.
- `width`: bit width.
- `radix`: default display radix.
- `samples`: compacted sample sequence.
- `diffRegions`: red highlight regions in comparison mode.
- `changeTimes`: cached transition times for fast navigation.
- `samplesLoaded`: WVZ4 on-demand loading state.

### 5.3 WaveTreeInfo

WVZ4 provides a stable signal tree through `NODE`. The viewer keeps this as data and exposes it through a model. The current tree view uses `QTreeView + QAbstractItemModel`, and `QModelIndex::internalId()` maps directly to the WVZ4 node id.

## 6. WVZ4 Format Evolution

| Version | Main Features |
|---|---|
| v1 | Initial WVZ4 metadata and WDAT block support. |
| v2 | Fixed-width values, delta/shared-time encoding, compressed layout sections, implicit zero baseline. |
| v3 | Time block x signal chunk WDAT tiling, footer tile index, header feature fields. |

The current package targets WVZ4 v3 while retaining parser compatibility for earlier WVZ4 variants and legacy WVZ versions.

## 7. WVZ4 v3 File Structure

### 7.1 File Header

The WVZ4 header is fixed-size and includes:

- Magic and version.
- Header size.
- Target block span.
- Footer offset.
- Signals-per-chunk when signal chunking is enabled.
- Feature flags.

### 7.2 Layout Sections

WVZ4 layout can be written raw or compressed:

| Raw Section | Compressed Section | Content |
|---|---|---|
| `NAME` | `NAMZ` | Name table. |
| `NODE` | `NODZ` | Stable tree nodes. |
| `SIGT` | `SIGZ` | Signal definitions. |

Compressed layout payloads store compression type, raw size, compressed size, and compressed bytes. The parser supports both raw and compressed variants.

### 7.3 WDAT v3 Outer Tile Header

Each WDAT section represents one time-block and signal-chunk tile. The v3 outer WDAT payload begins with:

```text
block_id
start_cycle
end_cycle
signal_chunk_id
first_signal_id
signal_count
compression
raw_size
encoded_size
encoded_payload
```

This allows the parser to skip unrelated tiles before reading or decompressing their payload.

### 7.4 WDAT v3 Raw Tile Payload

After decompression, the raw tile contains:

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
delta-coded offsets
records_blob_size
records_blob
```

For any signal in the tile:

```text
local = signal_id - first_signal_id
record_start = offsets[local]
record_end   = offsets[local + 1]
```

An empty range means that signal has no transition in the tile.

### 7.5 FOOT v3

The footer stores a tile index:

```text
block_id
start_cycle
end_cycle
signal_chunk_id
first_signal_id
signal_count
file_offset
file_size
raw_size
compression
```

The viewer can use the footer to jump directly to relevant WDAT tiles instead of scanning the whole file.

## 8. Writer and Recorder Design

### 8.1 Typed Writer

The writer is header-only and validates layout before writing. It supports:

- Stable topology.
- Typed scalar values up to 64 bits.
- Fixed value widths derived from `SignalDefinition`.
- Delta-time and shared-time candidate encoding.
- Signal chunking by `(signal_id - 1) / signals_per_chunk`.
- Optional zstd compression.
- Optional block pipeline compression/writing.
- WAL monitor/finalizer path.

### 8.2 Signal Chunking

Signal chunking avoids decompressing unrelated signals. Each chunk usually contains a fixed number of signal ids, defaulting to 1024. The writer emits only chunks that contain transitions in the current time block.

### 8.3 Implicit Zero Baseline

WVZ4 v2 and v3 define an implicit all-zero value for every signal at cycle 0. The writer omits explicit first-zero transitions. The parser supplies an implicit zero sample for loaded signals, while explicit non-zero cycle-0 samples override it.

### 8.4 WAL / Monitor Finalization

The WAL path allows a separate monitor to replay committed spool records into a finalized WVZ4 file. Recovery is guaranteed only up to the last committed WAL record. Unfinished or checksum-invalid WAL records are ignored.

## 9. Parser and Loading Workflow

### 9.1 Metadata Loading

On open, metadata sections are parsed to build:

- Name table.
- Node table.
- Signal table.
- Tree metadata.
- Footer tile index if present.

The parser validates node references, child-sibling chains, signal leaf bindings, value types, bit widths, and radix values.

### 9.2 On-Demand Loading

For WVZ4, sample data is loaded only for selected signals. With v3 tile indexing, the parser can use FOOT records to read only relevant WDAT tiles. It skips unrelated signal chunks without reading compressed payloads.

### 9.3 Hot-Path String Reduction

WVZ4 decoding no longer constructs value strings for each sample. It stores `rawBits` and formats values only when display, export, or text access requires it. This removes heavy `QString` construction from the parsing path.

## 10. Viewer Tree Model

The signal tree uses `QTreeView + QAbstractItemModel`. The model directly wraps the internal tree data. It avoids duplicating tree nodes as `QTreeWidgetItem` objects.

Key properties:

- `internalId()` is the stable node id.
- Search preserves tree structure.
- Search can match node name and bit-width text such as `[7:0]`.
- Comparison mode places A/B leaves under the same common path.
- The compare toolbar button uses `icons/compare.png` with fallback rendering.

## 11. Search Behavior

Search filters the tree while preserving hierarchy. Visible nodes include:

- Direct matches.
- Ancestors of matches.
- Descendants when a matching module should expose its subtree.

This prevents flat search results and keeps the user oriented in the original hierarchy.

## 12. Active Signal List

The active signal list manages selected signals and current values. It supports:

- Multi-selection.
- Ctrl+C copying of selected signal/value text.
- Slow-double-click text editing/selection behavior.
- Right-biased display of long names.
- Draggable split line between name and value columns.
- Visible-row-only value refresh to avoid large-list overhead.

## 13. WaveCanvas Rendering

The canvas handles waveform rows, cursor, scaling, time range selection, and rendering. Current notable behaviors:

- Ctrl multi-select in waveform area.
- Dragging time range on both time ruler and waveform area.
- Jump-to-change across selected signals.
- Ctrl jump-to-diff-region in comparison mode.
- Red background for differing comparison intervals.
- Bus transition rendering with horizontal mask lines and slanted transition lines.
- High-lighted rows avoid black mask lines that would overwrite highlight color.

## 14. Two-File Comparison Workflow

The comparison button asks the user to select two waveform files. The system loads both files fully for comparison, maps signals by full path, and retains only common-path signals with actual value differences.

The resulting tree is not two independent trees. It is a shared hierarchy with adjacent leaves:

```text
top
  module
    A data[7:0]
    B data[7:0]
```

Differences are computed by advancing the value state over the union of sample times. Red regions represent time intervals where the two value states differ, including cases where one file ends earlier.

## 15. Performance Strategy

### 15.1 Current Optimizations

| Hotspot | Optimization |
|---|---|
| Metadata parsing during on-demand load | Skip NAME/NODE validation when only samples are needed. |
| Signal id mapping | Use dense vector maps instead of `QHash<int,int>` in hot WVZ4 paths. |
| WVZ4 sample decode | Store rawBits instead of per-sample QString. |
| Change navigation | Use prebuilt `changeTimes` with binary search. |
| Diff rendering | Binary-search visible diff regions. |
| Active value refresh | Refresh visible rows only. |
| Tree display | Use model-view data access instead of item duplication. |
| WDAT loading | Use v3 footer tile index to avoid unrelated chunks. |

### 15.2 Remaining Cost Centers

The expected remaining heavy costs are zstd decompression, vector pushes for samples, Qt painting, and comparison state transitions. If profiling still shows bottlenecks, the next major refactor is to store samples as separate `times[]` and `rawBits[]` arrays instead of `QVector<WaveSample>`.

## 16. Build and Compatibility Notes

The project targets Visual Studio x64 and Qt5. Compatibility considerations:

- Avoid Qt6-only mouse APIs in Qt5 builds.
- Avoid `QDataStream::Qt_5_15` for older Qt5 environments.
- Avoid direct `signals`, `slots`, and `emit` tokens where Qt macro conflicts matter.
- Clean rebuild is recommended after signal declarations or model/delegate changes.
- `app.rc` no longer depends on a missing `app.ico`; icon fallback is handled in code.

## 17. Typical Usage

### 17.1 Open Single Waveform

1. Start QtSignalViewer.
2. Open a supported waveform file.
3. Search or browse the signal tree.
4. Add signals by double-click or drag/drop.
5. Use waveform cursor, zoom, range selection, and next/previous change navigation.

### 17.2 Compare Two Waveforms

1. Click the compare button.
2. Select two waveform files.
3. The viewer keeps only common-path signals with value differences.
4. A/B leaves appear adjacent under the same path.
5. Red intervals show exact differing time ranges.
6. Ctrl + next/previous change jumps to red regions.

## 18. Validation Checklist

Before delivery or regression testing, verify:

- WVZ4 v3 files open correctly.
- NAMZ/NODZ/SIGZ compressed layouts parse correctly.
- FOOT-indexed loading reaches only relevant signal chunks.
- Same-path comparison removes identical signals.
- Different intervals are highlighted red.
- Search preserves tree hierarchy and supports `[N:0]` bit-width queries.
- Active list value copying and slow-double-click selection work.
- Waveform Ctrl multi-select and range dragging work.
- Qt5 build has no `position()`, `globalPosition()`, or `Qt_5_15` compile failures.

## 19. Known Constraints

- WVZ4 is designed for stable topology and scalar values up to 64 bits.
- WVZ4 does not support Z/high-impedance states in the current writer path.
- The monitor can recover only committed WAL records.
- A main-thread crash that leaves the process alive is intentionally not handled.
- Some legacy formats still rely on textual value storage and fallback hydration.

## 20. Recommended Future Work

1. Convert `QVector<WaveSample>` into structure-of-arrays storage for WVZ4 raw samples.
2. Add bounded decompressed-tile cache for repeated signal selection in the same chunk.
3. Add automated regression tests for WVZ4 v1/v2/v3 files.
4. Add synthetic benchmark files for large tree, large sample count, and sparse signals.
5. Add background worker loading for long compare operations.
6. Add optional export of comparison reports.

## 21. Packaging Contents

This package contains:

- `source/`: latest complete source code package.
- `docs/QtSignalViewer_Technical_Documentation_20260528_0604.md`: Markdown technical document.
- `docs/QtSignalViewer_Technical_Documentation_20260528_0604.docx`: Word technical document.
- `docs/PACKAGE_NOTES_20260528_0604.md`: quick package notes and build reminder.

## 22. Delivery Notes

This documentation is generated from the current conversation baseline and latest source package. The code has been statically inspected in the container, but the container does not provide a Qt/Visual Studio build environment. Perform a local clean rebuild in Visual Studio before treating the package as release-ready.
