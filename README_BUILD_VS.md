# WaveTrace Release Package Build Notes

构建完成后的 WaveTrace 接入与运行说明见：
[`docs/WaveTrace_使用手册_20260711.md`](docs/WaveTrace_使用手册_20260711.md)。

The formal package is intended to be copied into a business tree and consumed as
prebuilt tooling plus source headers. It does not require business CMake or
Visual Studio projects to build WaveTrace tools.

The zip top-level directory is `WaveTracer`.

Prebuilt tools and libraries:

- `tools\bin\wavetrace_reflectgen.exe`
- `tools\bin\wvz4_writer_monitor.exe`
- `tools\bin\libclang.dll` and its runtime DLLs
- `tools\lib\zstd_release.lib`
- `tools\lib\zstd_debug.lib`
- `tools\lib\clang\22`
- `third_party\zstd\include`

Property sheets:

- `props\zstd_embed.props`: adds the minimal zstd include path and links the
  prebuilt zstd static library. It does not compile zstd sources.
- `props\wavetrace_reflectgen_reference.props`: runs the prebuilt ReflectGen
  executable before C++ compilation, writes generated reflection headers under
  `$(WaveTraceRoot)\generated_reflect`, and adds that directory to the
  include path. It does not add a `ProjectReference`.
- `props\wvz4_writer_helper_reference.props`: checks that the prebuilt writer
  helper exists. It does not add a `ProjectReference`.

## CMake Integration

When `ENABLE_WAVETRACE=ON`, the CMake integration uses the prebuilt tools from
`tools\bin`. It does not call `add_executable()` for ReflectGen or the writer
helper, and the zstd dependency is an `IMPORTED STATIC` library.

Generated reflection files are written to:

```text
<original-WaveTracer-dir>\generated_reflect\project_reflect_auto.h
<original-WaveTracer-dir>\generated_reflect\root_class_closure_reflect_auto.h
```

The same directory also contains `reflectgen.log` and
`wavetrace_reflect_targets.txt`.

### Unified reflection and runtime configuration

The user-owned configuration is kept in the original package directory, not in
the build tree:

```text
<original-WaveTracer-dir>\wavetrace_config.json
```

`WaveTrace=false` is read before libclang/AST collection. ReflectGen then skips
reflection and emits small `project_reflect_auto.h` and
`root_class_closure_reflect_auto.h` placeholders so existing includes continue
to compile. `WaveTraceFileName` controls the WVZ4 output. `WaveTraceStart` and
`WaveTraceEnd` are inclusive business-cycle bounds; an empty start means zero
and an empty end means no configured limit. The generated file and Viewer use
the configured absolute start and the actual recorded end.

The same JSON also replaces the former WaveTrace runtime environment variables:
`WaveTraceLevel`, `WaveTraceDirtyArrayStats`, `WaveTraceDirtyArrayMarks`, and
`WaveTraceMemoryUsage`.

### Per-member `WavePtr` reflection switches

Each entry is keyed by the declaring class and member name. A `wave::WavePtr`
member is excluded only when its matching entry has `"reflect": false`.
Missing entries and entries set to `true` are reflected. ReflectGen discovers
fields while collecting the AST, preserves existing `false` values, adds new
members as `true`, and atomically updates the JSON after successful generation.
It does not generate and then prune disabled fields.

Class-template instances share one entry. For example, `Box<int>::ptr` and
`Box<float>::ptr` are both controlled by `Box::ptr`.

Run the focused configuration cases with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\test_reflectgen_waveptr_config.ps1
```

The test output is isolated under
`build_vs\reflectgen_waveptr_config_tests`.

For a final executable target in the same CMake project, call:

```cmake
include("${WAVETRACE_ROOT}/cmake/wavetrace_writer_helper.cmake")
wavetrace_target_needs_writer_helper(my_sim)
```

For executable targets, the function copies `wvz4_writer_monitor.exe` beside the
simulator after build, matching the runtime helper lookup path.

## Formal Package

Use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_wavetrace_release.ps1
```

The package script uses a whitelist. It deliberately excludes smoke/test
projects, SystemC, the full zstd source tree, and the full LLVM tree.
