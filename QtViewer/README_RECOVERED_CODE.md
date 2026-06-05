# QtSingalViewer recovered complete source package

This package is assembled from the last uploaded project/source files and the last drag-drop UI patch that had the multi-select drag/drop code.

## Included fixes

- Candidate signal tree defaults to collapsed (`collapseAll`).
- Candidate tree sorting is disabled so nodes are displayed in insertion/file order, not string order such as `A[100]` before `A[2]`.
- Search expands matching paths; clearing search collapses the tree again.
- Candidate -> active multi-select drag/drop is included.
- Active -> candidate drag/drop removal is included.
- Active list internal drag reorder is preserved.
- Qt5 compatibility: `QMouseEvent::position()` / `QWheelEvent::position()` are replaced by Qt5-compatible `pos()` usages.
- WVZ2/WVZ3 signal output order follows signal directory/file order instead of sorting `sampleMap.keys()`.
- `main.cpp` adds Qt5 high-DPI attributes before `QApplication`.
- Project file imports `QtLocal.props` early and uses `$(QtLocalRoot)` for Qt.

## How to use safely

1. Back up your current broken project folder.
2. Copy these files into the project root, replacing same-named files.
3. Double-click `qt5_oneclick.bat` if you need Qt5 local setup.
4. Reopen Visual Studio.
5. Build `Debug | x64`.

## Important

This package cannot include your external zstd library files if they were not uploaded. If your local project previously had `props/zstd_embed.props` and zstd headers/libs, keep that `props` folder in the project.
