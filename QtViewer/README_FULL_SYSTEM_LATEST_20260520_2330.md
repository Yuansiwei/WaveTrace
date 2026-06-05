# QtSingalViewer full latest system package 20260520_2330

This package is the latest full source bundle assembled from the recovered complete project plus all relevant later patches.

## Use this package as the new baseline

Do not mix with older patch zips. This bundle already includes the latest versions of:

- MainWindow.h / MainWindow.cpp
- WaveTypes.h
- WaveParser4.h / WaveParser4.cpp
- WaveParser3.h / WaveParser3.cpp
- WaveCanvas.h / WaveCanvas.cpp
- ActiveSignalListWidget.h / ActiveSignalListWidget.cpp
- main.cpp
- app.rc / app.manifest
- Qt project files

## WVZ4 support

WVZ4 viewer support is included.

Implemented:
- Reads WVZ4 64-byte header.
- Reads NAME / NODE / SIGT / WDAT / FOOT sections.
- Uses WVZ4 NODE table as the native tree source.
- Does not rebuild WVZ4 tree from full signal names.
- `WaveSignal.name` for WVZ4 stores only the leaf segment.
- Complete signal path is materialized only when needed:
  - active-list row display;
  - export;
  - search-path materialization;
  - display helper calls.
- WVZ4 supports scalar values up to 64 bits.
- WVZ4 has no Z/high-impedance state.
- WDAT decoding is signal_id-based.
- Supports None/Zstd compression, assuming zstd headers/libs are available in the build environment.

## Search behavior

Candidate tree search:
- Does not run on every keystroke.
- Runs only when Enter is pressed in the search box.
- Single segment query, e.g. `valid`:
  - searches tree node segment names;
  - case-insensitive contains match.
- Multi-segment query, e.g. `a.b`:
  - structural tree search;
  - finds a node named `a` with a direct child path segment named `b`;
  - matching can start at any tree level;
  - case-insensitive exact match per segment.
- Search does not call `expandAll()`.
- Search does not allocate full signal names for every signal.

## Tree/loading behavior

- WVZ4 uses file-native NODE first_child / next_sibling / parent structure.
- WVZ3 and old formats without `WaveFile.tree` still fall back to the custom SmallStr32/SmallVec32 logic tree builder.
- QTreeWidgetItem creation is lazy; the UI tree does not instantiate all leaves during open.
- The custom logic tree uses:
  - SmallStr32 for segment names;
  - SmallVec32 for split segment lists, path nameId lists, and child lists;
  - custom flat maps for string interning and child lookup.
- No exact Qt macro-risk tokens are used in the modified main files: `signals`, `slots`, `emit`, `foreach`.

## Qt5 compile fixes included

- No `QVector<QString>::join`.
- No `QString::SkipEmptyParts`.
- No deprecated `QString::split(..., QString::SkipEmptyParts)` usage in tree search.
- No `QMouseEvent::position()` for Qt5 mouse handling in WaveCanvas patch path.

## Project integration

`QtSingalViewer.vcxproj` and `.filters` have been patched to include:

- WaveParser4.h
- WaveParser4.cpp
- app.manifest

If Visual Studio still does not show these files, add them manually:
- Header Files: `WaveParser4.h`
- Source Files: `WaveParser4.cpp`

## Zstd note

WVZ4 parser includes `<zstd.h>` and uses ZSTD APIs for compressed WDAT blocks.
Make sure the project include/lib paths can see zstd and link against zstd.lib/zstd.dll, or add a project-level fallback if you want to compile without zstd.

## Files in this package

Core:
- ActiveSignalItemWidget.cpp
- ActiveSignalItemWidget.h
- ActiveSignalListWidget.cpp
- ActiveSignalListWidget.h
- MainWindow.cpp
- MainWindow.h
- WaveCanvas.cpp
- WaveCanvas.h
- WaveParser.cpp
- WaveParser.h
- WaveParser2.cpp
- WaveParser2.h
- WaveParser3.cpp
- WaveParser3.h
- WaveParser4.cpp
- WaveParser4.h
- WaveTypes.h
- WVZ2Format.h
- WVZ3Format.h
- main.cpp
- app.rc
- app.manifest
- resources.qrc
- QtSingalViewer.qrc
- QtSingalViewer.ui

Project:
- QtSingalViewer.sln
- QtSingalViewer.vcxproj
- QtSingalViewer.vcxproj.filters
- QtSingalViewer.vcxproj.user

Scripts:
- qt5_oneclick.bat
- qt5_oneclick.ps1

## Static checks performed while packaging

- WVZ4 parser files exist.
- Project file contains WaveParser4.cpp and WaveParser4.h.
- WaveTypes.h contains WaveTreeInfo and WaveFile::tree.
- WaveParser4.cpp does not contain buildSignalPath().
- MainWindow search is Enter-only.
- MainWindow search is structural tree search.
- MainWindow does not call m_tree->expandAll().
- MainWindow does not use QVector<QString>::join.
- MainWindow does not use QString::SkipEmptyParts.
- MainWindow materializes full signal names only when needed.
- MainWindow.cpp / MainWindow.h / WaveParser4.cpp / WaveParser4.h / WaveTypes.h contain no exact tokens:
  signals / slots / emit / foreach.
