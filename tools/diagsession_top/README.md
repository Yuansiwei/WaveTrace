# DiagSessionTop

`DiagSessionTop` reads Visual Studio CPU sampling data without opening the Visual Studio UI.
It accepts a `.diagsession` archive or a raw `.etl`, converts ETL to ETLX through TraceEvent,
resolves available native symbols, and prints self/inclusive sample counts and sampled stacks.

## Build

Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\diagsession_top\build.ps1
```

The build uses the C# compiler and TraceEvent assemblies shipped with Visual Studio 2019.
It does not require a separate .NET SDK.

## Usage

```powershell
build_vs\diagsession_top\Release\DiagSessionTop.exe `
  report.diagsession `
  --process smoke_reflect_topology_cache_stress `
  --top 100 `
  --csv build_vs\topology_hotspots.csv `
  --symbol-path "C:\symbols;C:\Users\13611\Documents\WaveTrace\build_vs"
```

Raw ETL is also accepted:

```powershell
build_vs\diagsession_top\Release\DiagSessionTop.exe trace.etl --process 1234
```

For native names, build with PDB files and include their directories in `--symbol-path` or
`_NT_SYMBOL_PATH`. The tool removes temporary extraction and ETLX files unless
`--keep-expanded` is specified.

`.diagsession` is treated as its documented ZIP container. Visual Studio's private report/UI
database is not parsed; CPU samples and stacks come from the standard ETL payload.
