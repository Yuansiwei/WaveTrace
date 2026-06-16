# WaveTrace 源码交付包说明

日期：2026-06-10

## 1. 交付包内容

交付包包含 WaveTrace 当前源码、Visual Studio 工程文件、Qt Viewer 源码、脚本、烟测入口和中文交接文档。

主要包含：

- 根目录 C++ 源码和头文件。
- `WaveTrace.sln` 及各 `.vcxproj` 工程。
- `props` 下的 Visual Studio 属性表。
- `QtViewer` 源码、工程、资源文件和脚本。
- `tools` 下辅助脚本。
- `docs` 下新增的中文交接文档。
- 现有 README / changelog / patch notes。

## 2. 故意不包含的内容

以下内容不属于源码交付包，故意排除：

- `.git`
- `.vs`
- `build_vs`
- `_deps`
- `_codex_extract_*`
- `_viewer_extract_*`
- `third_party/llvm`
- `third_party/zstd-src`
- `QtViewer/third_party/Qt`
- `QtViewer/third_party/zstd-src`
- `.obj/.pdb/.ilk/.idb/.tlog` 等编译产物
- `.exe/.dll/.lib/.exp` 等本机二进制产物
- `.wvz4/.vcd` 等生成波形文件
- `.docx` 和历史生成的专利文档
- `.zip/.7z` 等压缩包

原因：

1. Qt 和 LLVM 是本地 SDK 依赖，体积大，不适合放进源码包。
2. 编译产物和调试缓存会污染代码交付。
3. Zstd 是通用第三方压缩库，代码量会稀释交付包重点；源码中只保留调用关系和工程配置说明。
4. 生成波形文件可能非常大，而且不是理解源码所必需。
5. 专利 docx 是临时材料，不作为工程源码交付。

## 3. 接手后需要准备的依赖

如果接手人要直接编译，需要准备：

- Visual Studio 2019 或 2022，x64 C++ 工具链。
- LLVM/libclang，本仓库工程默认期望路径为 `third_party/llvm/llvm-local`。
- Qt 6.5.3 MSVC 2019 x64，Viewer 工程默认期望路径为 `QtViewer/third_party/Qt/6.5.3/msvc2019_64`。
- Zstd 1.5.x 源码或等价 zstd 开发包。当前工程属性表默认按 `third_party/zstd-src/zstd-1.5.7` 查找；接手人可自行放置该目录或改工程属性表。

如果不使用默认路径，需要修改：

- `props/llvm_local.props`
- `QtViewer/QtLocal.props`
- `props/zstd_embed.props`
- `QtViewer/props/zstd_embed.props`

## 4. 推荐阅读顺序

1. `docs/WaveTrace_完整技术交接文档_20260610.md`
2. `docs/WaveTrace_外部撰写材料说明_20260610.md`
3. `README_BUILD_VS.md`
4. `README_WAVETAP_MANUAL.md`
5. `README_WVZ4_V3_MONITOR_CHUNKS.md`
6. `QtViewer/README_BUILD_VIEWER.md`
7. 核心代码文件：
   - `ReflectGen.cpp`
   - `reflect_macro.h`
   - `reflect_runtime.h`
   - `wave_runtime.h`
   - `wave_path_wvz4_recorder.h`
   - `wvz4_writer_typed.h`
   - `wvz4_writer_monitor_main.cpp`
   - `QtViewer/WaveParser4.cpp`
   - `QtViewer/MainWindow.cpp`
   - `QtViewer/WaveCanvas.cpp`

## 5. 当前源码状态说明

本包按当前工作区内容打包，不强行回滚未提交改动。也就是说，包中包含当前已经修改但未提交的文件，以及新增的 1M 业务压测入口。

新增文档位于：

- `docs/WaveTrace_完整技术交接文档_20260610.md`
- `docs/WaveTrace_外部撰写材料说明_20260610.md`
- `docs/WaveTrace_交付包说明_20260610.md`
