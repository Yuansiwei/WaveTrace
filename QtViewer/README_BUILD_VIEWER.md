# QtViewer VS Build

This is the simplified Visual Studio entry for the recovered Qt waveform viewer.

Open:

```text
QtViewer.sln
```

Build from a Visual Studio developer command prompt:

```bat
msbuild QtViewer.sln /m /p:Configuration=Release /p:Platform=x64
```

Run the Release build with Qt DLL/plugin paths configured:

```bat
run_release.bat
```

Local dependencies are copied into the project directory:

- `third_party/Qt/6.5.3/msvc2019_64`
- `third_party/zstd-src/zstd-1.5.7`
- `QtLocal.props`
- `props/zstd_embed.props`

This project does not require Qt VS Tools. `QtViewer.vcxproj` calls the local
Qt `moc`, `uic`, and `rcc` tools directly before C++ compilation.

WVZ4 parser compatibility has been updated for the WaveTrace WVZ4 v5 writer:

- v5 `SIGT` with `storage_id`
- logical signal to physical storage stream aliasing
- sparse WDAT tiles
- per-record value codecs, including bool toggle, byte mask, nibble mask, and stride variants
- `CLKD/CLKZ` periodic clock sections

The parser smoke source is `smoke_wvz4_parser.cpp`. It is shown in the VS
project as a non-build file because it has its own console `main`.
