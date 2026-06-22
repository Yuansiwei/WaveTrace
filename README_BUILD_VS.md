# Visual Studio build

Open `WaveTrace.sln` in Visual Studio 2019/2022 and build `Release|x64`.

Projects:

- `ReflectGen`: builds the current `ReflectGen.cpp` and uses local `third_party\llvm\llvm-local`.
- `wvz4_writer_monitor`: builds the helper writer process used by `PathStableWvz4Recorder`.
- `smoke*`: small compile/runtime checks for the reflection-waveform runtime and WVZ4 writer.

Dependency property sheets:

- `props\llvm_local.props`: adds `third_party\llvm\llvm-local\include`, links `libclang.lib`, and copies `libclang.dll`.
- `props\zstd_embed.props`: embeds zstd sources from `third_party\zstd-src\zstd-1.5.7`.
- `props\wavetrace_app_common.props`: shared x64 output paths and C++ settings.
- `props\wavetrace_reflectgen_reference.props`: adds an MSBuild `ProjectReference` to `ReflectGen` for apps whose build invokes the reflection generator.
- `props\wvz4_writer_helper_reference.props`: adds an MSBuild `ProjectReference` to `wvz4_writer_monitor` for apps that use helper-process WVZ4 writing. This is an incremental build dependency only; it does not link the helper into the app.

For a business executable that invokes ReflectGen and uses `PathStableWvz4Recorder`
or `wvz4::WriterProcessClient`, import both dependency sheets after
`props\wavetrace_app_common.props`:

```xml
<Import Project="props\wavetrace_reflectgen_reference.props"
        Condition="Exists('props\wavetrace_reflectgen_reference.props')" />
<Import Project="props\wvz4_writer_helper_reference.props"
        Condition="Exists('props\wvz4_writer_helper_reference.props')" />
```

MSBuild/Visual Studio will then build `ReflectGen.exe` and
`wvz4_writer_monitor.exe` before the app when needed, and skip them when their
inputs are already up to date. Avoid post-build `msbuild` commands for these
tools; they bypass normal project dependency scheduling.

Command-line build from a VS developer prompt:

```bat
msbuild WaveTrace.sln /m /p:Configuration=Release /p:Platform=x64
```

## CMake tool integration

The CMake integration builds ReflectGen from the current source through
`cmake/wavetrace_reflectgen.cmake`; reflection custom commands depend on that
target, so changing `ReflectGen.cpp` rebuilds the generator before regenerating
headers.

The helper process uses `cmake/wavetrace_writer_helper.cmake`. When
`ENABLE_WAVETRACE=ON`, the cmodel templates call:

```cmake
include("${WAVETRACE_ROOT}/cmake/wavetrace_writer_helper.cmake")
wavetrace_target_needs_writer_helper(cmodel)
```

This adds a normal CMake target dependency on `wavetrace_writer_monitor`, so
Ninja/MSBuild builds the helper before `cmodel` only when needed. Because
`cmodel` is a static library, CMake cannot know the final simulator executable
directory. For a final executable target in the same CMake project, call the
same function on that executable too:

```cmake
add_executable(my_sim ...)
target_link_libraries(my_sim PRIVATE cmodel)
wavetrace_target_needs_writer_helper(my_sim)
```

For executable targets, the function also copies `wvz4_writer_monitor.exe`
beside the simulator after build, which matches the runtime helper lookup path.
