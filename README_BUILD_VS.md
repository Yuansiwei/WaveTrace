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
  `$(IntDir)\WaveTracer\generated_reflect`, and adds that directory to the
  include path. It does not add a `ProjectReference`.
- `props\wvz4_writer_helper_reference.props`: checks that the prebuilt writer
  helper exists. It does not add a `ProjectReference`.

## CMake Integration

When `ENABLE_WAVETRACE=ON`, the CMake integration uses the prebuilt tools from
`tools\bin`. It does not call `add_executable()` for ReflectGen or the writer
helper, and the zstd dependency is an `IMPORTED STATIC` library.

Generated reflection files are written to:

```text
<business-build-dir>\WaveTracer\generated_reflect\project_reflect_auto.h
<business-build-dir>\WaveTracer\generated_reflect\root_class_closure_reflect_auto.h
```

The same directory also contains `reflectgen.log` and
`wavetrace_reflect_targets.txt`.

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
