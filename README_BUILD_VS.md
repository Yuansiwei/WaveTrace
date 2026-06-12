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

Command-line build from a VS developer prompt:

```bat
msbuild WaveTrace.sln /m /p:Configuration=Release /p:Platform=x64
```
