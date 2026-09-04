# Sea-Eagle 编译系统详解

本文档解释 `vec1_cl_build` 相关的 Sea-Eagle 编译系统：入口在哪里、CMake
如何生成大量 Visual Studio 工程、不同配置如何切换、cmodel 和 WaveTrace
如何接入、增量生成如何判断，以及当前镜像能验证什么、不能验证什么。

文档以 `build_system_snapshots/sea_eagle/optimized` 为准。原始版本位于
`build_system_snapshots/sea_eagle/original`，可用于逐文件对照。

## 1. 先给结论

1. Windows 根入口是 `Sea-Eagle_build.bat`。
2. 真正的 CMake 根工程是 `SW/projects.se/driver/cuda/CMakeLists.txt`。
3. 根 CMake 通过 `include()` 和 `add_subdirectory()` 把 compiler、cmodel、
   HAL、driver、runtime、dummy、test 等目标接进同一张构建图，所以一个入口会
   生成很多 `.vcxproj` 和一个 `gpgpu.sln`。
4. Visual Studio 是多配置生成器。配置在构建时通过 `--config` 或 VS 工具栏
   选择，不能在配置阶段依赖一个固定的 `CMAKE_BUILD_TYPE`。
5. 优化版支持 `Debug`、`Release`、`RelWithDebInfo`、`MinSizeRel`、`Profile`。
6. `Release`、`RelWithDebInfo`、`Profile` 都可以生成可供 Windows 性能分析器
   使用的 PDB；`Profile` 继承 Release 优化，同时保留符号和帧指针。
7. MSVC 单工程编译使用 `/MP32`，命令行构建默认也使用 32 个并行任务。
8. 最终 SDK 输出仍然是扁平目录，不按配置再分一层：
   `SW/projects.se/build/sdk/bin` 和 `SW/projects.se/build/sdk/lib`。
9. `cmodel_reggen` 会出现在 solution 中，也会在构建 cmodel 时被调度，但内部
   时间戳脚本只在输入变新、输出缺失或 stamp 缺失时才真正执行 Python。
10. ReflectGen 使用 `add_custom_command(OUTPUT ...)`，目标头文件及依赖没有变化
    时不会重新反射。
11. 当前镜像没有任何 `p4`、`p4 sync`、Perforce checkout 或自动拉代码逻辑。
12. 该目录是构建文件的审计镜像，不含私有源码和全部被引用的 CMake 文件，
    不能脱离完整 Sea-Eagle 工作区独立 configure/build。

## 2. 总体结构

```text
Sea-Eagle_build.bat
  |
  |-- 建立 tools / arch / mathlib 三个目录 junction
  |-- cmake -S SW/projects.se/driver/cuda -B vec1_cl_build
  v
SW/projects.se/driver/cuda/CMakeLists.txt        CMake 根工程: gpgpu
  |
  |-- include(RegGen)
  |-- include(GCDefine)
  |-- include(CModel)
  |-- include(TestBench)                         可选
  |-- include(build_options)
  |
  |-- add_subdirectory(../../compiler/cuda)
  |-- add_subdirectory(hal)
  |-- add_subdirectory(driver)
  |-- add_subdirectory(runtime)
  |-- add_subdirectory(dummy)
  |-- add_subdirectory(test)                     BUILD_TESTS=ON 时
  |
  `-- import_emulator()
        |
        |-- add_subdirectory(${AQARCH}/cmodel)
        |     `-- 生成 cmodel、cmodel_reggen、ReflectGen 生成头等
        `-- 生成 libEmulator / Emulator，并链接 cmodel
```

### `include()` 和 `add_subdirectory()` 的区别

- `include(X)`：在当前 CMake 作用域执行 `X.cmake`，通常用于加载函数、宏、公共
  选项和生成逻辑。它本身不一定创建 VS 工程。
- `add_subdirectory(dir)`：进入 `dir/CMakeLists.txt`，把其中创建的 target 加入
  当前构建图。每个 `add_library()`、`add_executable()` 或
  `add_custom_target()` 通常会对应一个 VS 工程。

因此，不能只看根 `CMakeLists.txt` 的行数来判断工程数量。根文件负责组织，真正
的 target 分散在被 include 的模块和各子目录中。

## 3. 两个 Windows 入口

### 3.1 根入口 `Sea-Eagle_build.bat`

位置：

```text
build_system_snapshots/sea_eagle/optimized/Sea-Eagle_build.bat
```

默认变量：

| 变量 | 默认值 | 含义 |
| --- | --- | --- |
| `WORKDIR` | `D:\Users\cn1842\workspace\cn1842_SeaEagle0` | Sea-Eagle 工作区根目录 |
| `CMAKE_EXE` | 固定的 CMake 3.28.1 路径 | 使用的 CMake 可执行文件 |
| `BUILD_CONFIG` | `Release` | `GENERATE_ONLY=0` 时构建的配置 |
| `BUILD_PARALLEL` | `32` | CMake build 并行度，同时传给 `/MP` 设置 |
| `GENERATE_ONLY` | `1` | `1` 只生成 solution，`0` 生成后继续编译 |

派生路径：

```text
AQROOT   = %WORKDIR%\SW\projects.se
AQARCH   = %AQROOT%\arch\XAQ2
AQTOOLS  = %WORKDIR%\TOOLS
BUILD_DIR= %WORKDIR%\vec1_cl_build
```

脚本首先重建三个目录 junction：

```text
SW/projects.se/tools                 -> TOOLS
SW/projects.se/arch                  -> HW/projects.se/arch
SW/projects.se/driver/cuda/mathlib   -> TOOLS/GcTest/mathlib
```

这里使用 `mklink /j`，目标均在本地工作区内。普通 cmd 双击即可执行，不要求
管理员权限或 Windows Developer Mode。删除旧路径之前会用
`fsutil reparsepoint query` 验证它确实是 reparse point；若那里是真实目录，脚本
会拒绝删除并报错，避免误删用户文件。

随后执行：

```bat
cmake -S "%AQROOT%\driver\cuda" -B "%WORKDIR%\vec1_cl_build" ^
  -G "Visual Studio 16 2019" -A x64 -T host=x64 ^
  -DGCDEFINE=cc10200L_0066 ^
  -DCMAKE_SYSTEM_VERSION=10.0.19041.0 ^
  -DCMAKE_CONFIGURATION_TYPES="Debug;Release;RelWithDebInfo;MinSizeRel;Profile" ^
  -DVEC1_MSVC_MP_COUNT=32 ^
  -DVEC1_RELEASE_DEBUG_INFO=ON
```

默认 `GENERATE_ONLY=1`，所以双击只做 CMake configure/generate，输出
`vec1_cl_build/gpgpu.sln`。CMake 配置阶段本身会打印大量检测、复制和 target
信息，这不代表已经调用 `cl.exe` 编译。

### 3.2 子入口 `build-gpgpu-solution.bat`

位置：

```text
SW/projects.se/build-gpgpu-solution.bat
```

它以脚本所在目录为基准：

```text
SOURCE_DIR = SW/projects.se/driver/cuda
BUILD_DIR  = SW/projects.se/gpgpu
```

与根入口的主要区别：

- CMake 必须能从 `PATH` 找到，而不是使用 `CMAKE_EXE`。
- 默认配置是 `Debug`。
- 生成目录是 `SW/projects.se/gpgpu`，不是工作区根下的 `vec1_cl_build`。
- 不负责创建 tools、arch、mathlib junction。

两个入口会创建不同 CMake cache 和中间目录，但最终 SDK 输出目录相同。日常应
固定使用一个入口，避免两个 build tree 交替覆盖同名最终产物。

### 3.3 环境和版本前提

完整 Windows 生成至少需要：

- CMake 3.28 或更新版本。根工程声明 `cmake_minimum_required(VERSION 3.28)`；
- Visual Studio 2019，安装 C++ x64 工具链；
- Windows SDK 10.0.19041.0，或同步修改 BAT 中的 SDK 版本参数；
- 本地工作区中的 `SW`、`HW`、`TOOLS` 以及 cmodel 私有源码；
- `TOOLS/GcTest/mathlib`、SystemC、CUDA headers、compiler/HAL/runtime 依赖；
- `AQROOT/tools/bin/python/python[.exe]` 及 cmodel feature database；
- 开启 WaveTrace 时所需的 ReflectGen、Clang resource、zstd 和 helper 文件；
- 能创建 junction 的本地 Windows 文件系统。UNC 网络共享不适合作为当前根脚本
  的直接目标。

两个 BAT 仍保留了历史工具链绝对路径：根入口引用 MSVC 14.29.30037 的 lib
目录，子入口把 14.29.30133 的 `bin/Hostx64/x64` 放入 `PATH`。真正选择编译器的
核心仍是 CMake 的 VS generator 和 `-T host=x64`，但机器未安装对应版本时，
硬编码路径会增加环境差异。长期维护建议通过 VS Developer Command Prompt 或
`vswhere` 定位工具链，而不是继续增加新的绝对版本路径。

## 4. 推荐使用方式

### 4.1 只生成 solution

在 cmd 中：

```bat
set WORKDIR=D:\Users\cn1842\workspace\cn1842_SeaEagle0
set CMAKE_EXE=D:\Users\cn1842\cmake-3.28.1-windows-x86_64\bin\cmake.exe
set GENERATE_ONLY=1
call Sea-Eagle_build.bat
```

然后打开：

```text
%WORKDIR%\vec1_cl_build\gpgpu.sln
```

### 4.2 命令行构建 Profile

```bat
set WORKDIR=D:\Users\cn1842\workspace\cn1842_SeaEagle0
set CMAKE_EXE=D:\Users\cn1842\cmake-3.28.1-windows-x86_64\bin\cmake.exe
set GENERATE_ONLY=0
set BUILD_CONFIG=Profile
set BUILD_PARALLEL=32
call Sea-Eagle_build.bat
```

已生成 solution 后，也可以直接执行：

```bat
cmake --build D:\Users\cn1842\workspace\cn1842_SeaEagle0\vec1_cl_build ^
  --config Profile --parallel 32
```

### 4.3 在 Visual Studio 中切配置

打开同一个 `gpgpu.sln`，从工具栏选择：

```text
Debug / Release / RelWithDebInfo / MinSizeRel / Profile
```

切换后执行 Build Solution。无需为每个配置重新运行根 BAT，也无需删除
`vec1_cl_build`。只有 CMake 输入、工具链、根路径或 generator 改变时才需要重新
configure。

## 5. Visual Studio 多配置原理

Visual Studio generator 是 multi-config generator：

- CMake configure 阶段同时生成多套配置。
- `CMAKE_CONFIGURATION_TYPES` 决定 solution 中有哪些配置。
- 真正使用哪套编译选项，在 VS 构建时或 `cmake --build --config X` 时决定。
- `CMAKE_BUILD_TYPE` 主要用于 Ninja、Unix Makefiles 等 single-config generator，
  对 Visual Studio 的当前构建配置不起决定作用。

原始系统的问题是 cmodel 中存在如下配置阶段判断：

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # 选择 Debug SystemC / cmonitor 目录
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  # 选择 Release 目录
endif()
```

同一套 VS solution 切换配置时，CMake configure 不会跟着重新选择这个分支，
于是 Debug 工程可能继续链接 Release 库，或 Profile 根本没有对应分支。优化版
改用 generator expression：

```cmake
"$<$<CONFIG:Debug>:.../Debug>"
"$<$<NOT:$<CONFIG:Debug>>:.../Release>"
```

Debug 宏也改成：

```cmake
add_compile_definitions("$<$<CONFIG:Debug>:DEBUG>")
add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG>")
```

这些表达式会在生成每个 `.vcxproj` 配置段时分别展开，所以同一 solution 可以
正确切换。

## 6. 五种配置

| 配置 | 优化 | 调试符号 | 典型用途 |
| --- | --- | --- | --- |
| `Debug` | 低或关闭 | 有 | 断点、变量观察、逻辑调试 |
| `Release` | Release 优化 | 优化版额外生成完整 PDB | 交付性能、性能分析 |
| `RelWithDebInfo` | CMake 的优化+调试信息配置 | 有 | 通用的优化调试 |
| `MinSizeRel` | 以尺寸为目标 | 未额外强制 Profile 符号 | 最小体积验证 |
| `Profile` | 继承 Release 编译/链接优化 | 有，并关闭 frame-pointer omission | CPU profiler、调用栈分析 |

### 6.1 Profile 的实际 MSVC 选项

根工程为 Profile 定义：

```text
C/C++:  Release flags + /Zi + /Oy-
EXE/DLL linker: Release flags + /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO
```

cmodel target 使用 `/Z7 /Oy-`，并向最终链接传播：

```text
/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO
```

`/Zi` 把编译调试信息写入编译 PDB；`/Z7` 把调试信息放在 `.obj` 中。两者在
最终 `/DEBUG:FULL` 链接后都能形成最终 PDB。`/Oy-` 保留帧指针，使 profiler
更可靠地还原调用栈。

Profile 不是 Debug。它仍基于 Release 优化，因此局部变量可能被合并、重排或
优化掉，逐语句断点体验不等同于 Debug。

### 6.2 Release 为什么也能看到符号

`VEC1_RELEASE_DEBUG_INFO=ON` 时：

- `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` 对 Debug、Release、RelWithDebInfo、
  Profile 使用 ProgramDatabase。
- Release、RelWithDebInfo、Profile 链接时使用 `/DEBUG:FULL`。
- 同时保留 `/OPT:REF` 和 `/OPT:ICF`，所以生成符号不等于关闭 Release 优化。

性能分析器要正确显示函数名，exe/dll 和 PDB 必须来自同一次链接。不要拿旧 PDB
配新 dll，也不要在切配置后只复制部分产物。

## 7. 工程生成链路

根 CMake 的主要阶段如下。

### 7.1 初始化平台和功能开关

默认关键选项：

| 选项 | 默认值 |
| --- | --- |
| `PLATFORM_TYPE` | `CMODEL` |
| `BUILD_CMODEL` | `ON`，会再受平台类型控制 |
| `BUILD_CMODEL_TEST_BENCH` | `OFF` |
| `BUILD_VCC` | `ON` |
| `BUILD_VRTC` | `ON` |
| `BUILD_VLINK` | `ON` |
| `BUILD_SPVCOMPILER` | `ON` |
| `BUILD_VFATBIN` | `ON` |
| `BUILD_TESTS` | `OFF` |
| `ENABLE_DUMMY_LIBS` | `ON` |
| `ENABLE_CCACHE` | `OFF` |
| `ENABLE_DISTCC` | `OFF` |
| `BUILD_CUDA` | `124` |

`PLATFORM_TYPE` 决定 cmodel、emulator 和 KMD 组合：

- `CMODEL`：构建 cmodel 和 Emulator，不构建 QEMU/KMD。
- `QEMU`：构建 cmodel 和 QEMU wrapper，并根据主机选择 Windows/Linux KMD。
- `HOST`：不构建 cmodel，只构建主机对应 KMD。

### 7.2 导入环境路径

根工程计算并校验：

```text
AQROOT  = driver/cuda/../..
AQARCH  = AQROOT/arch/XAQ2
AQTOOLS = AQROOT/tools
GCDEFINE
```

`check_and_set_var()` 会优先使用 CMake 变量，否则读取同名环境变量，然后转换成
CMake 风格路径并写入 cache。缺少必需变量会在 configure 阶段直接失败。

### 7.3 加载 CMake 模块

```cmake
include(RegGen)
include(config.cmake OPTIONAL)
include(GCDefine)
include(CModel)
include(TestBench)       # BUILD_CMODEL_TEST_BENCH=ON
include(build_options)
```

这些模块提供寄存器生成、gcDefine 生成、cmodel/emulator 导入、测试平台和 HAL
公共选项。

### 7.4 加入主要子工程

```cmake
add_subdirectory(../../compiler/cuda compiler)
add_subdirectory(hal)
add_subdirectory(driver)
add_subdirectory(runtime)
add_subdirectory(dummy)
add_subdirectory(test)   # BUILD_TESTS=ON 且目录存在
```

这就是 solution 中会出现 compiler、HAL、driver、runtime、dummy、工具和测试等
大量项目的直接原因。不是只有反射系统或 cmodel。

### 7.5 导入 cmodel 和 Emulator

`import_emulator()` 由 `CModel.cmake` 提供：

- 没有 `FIXED_ARCH_TYPE` 时，通过
  `add_subdirectory(${AQARCH}/cmodel ${CMAKE_BINARY_DIR}/cmodelLib)` 构建源码 cmodel。
- 设置 `FIXED_ARCH_TYPE` 时，导入预编译 cmodel/Emulator。
- `BUILD_EMULATOR=ON` 时创建 Windows 的 `libEmulator` 或 Linux 的 `Emulator`，
  并链接 `cmodel`。
- QEMU 模式且 wrapper 存在时，创建 `wrapcmodel`。

## 8. cmodel 子系统

cmodel 根文件：

```text
SW/projects.se/arch/XAQ2/cmodel/CMakeLists.txt
```

正常工作区中 `SW/projects.se/arch` 是指向 `HW/projects.se/arch` 的 junction，因此
SW 和 HW 路径实际指向同一份 cmodel。镜像为了保留抽取时看到的两个相对路径，
保存了两份同内容文件。

### 8.1 cmodel target

cmodel 以静态库创建：

```cmake
add_library(cmodel STATIC ${ALL_SOURCES} ${ALL_HEADERS} ${RT_CONFIG_FILES})
```

MSVC 下输出名改为 `arch.cmodel`，使用 `inc/vsiPrecomp.h` 作为预编译头，并添加
`/vmg /Zm300 /bigobj /MP32` 等选项。

cmodel 自身继续加入 blt、CacheSubsystem、CFI、cluster、common、CWD、dma、
DPPU、dump、FA、float、GenSource、HostInterface、isa、memory、PPU、QPPU、
shader、SysWrapper、texture、toplevel、cmonitor、mathlib 等目录。完整工作区中的
这些子目录共同形成 cmodel 的源码和头文件集合。

### 8.2 配置相关依赖库

Windows MSVC 下：

- Debug 使用 SystemC Debug 和 cmonitor Debug 目录。
- Release、RelWithDebInfo、MinSizeRel、Profile 使用 Release 目录。
- 公共链接库为 `SystemC`、`vsiMathlib`、`cmonitor_protocol.lib`。

cmonitor 路径先转为绝对 CMake 路径，再写入
`INTERFACE_LINK_DIRECTORIES`，避免 CMake 报 imported/interface target 含相对链接
目录。

### 8.3 WaveTrace 接入

`ENABLE_WAVETRACE` 默认 `ON`。CMake 按顺序寻找 WaveTrace：

1. `WAVETRACE_ROOT` cache 变量。
2. `ENV{WAVETRACE_ROOT}`。
3. cmodel 同级或相邻的 `WaveTracer` / `WaveTrace` 目录。

开启时必须存在：

```text
reflect_macro.h
third_party/zstd/include/zstd.h
tools/bin/wavetrace_reflectgen.exe
tools/lib/clang/22/include/stddef.h
tools/lib/zstd_release.lib             Windows
tools/lib/zstd_debug.lib               Windows
```

WaveTrace 头和生成目录会加入 cmodel include path，`reflect_macro.h` 被强制包含。
Windows 下导入 `WaveTrace::zstd`，Profile、RelWithDebInfo、MinSizeRel 都映射到
Release zstd 库，Debug 使用 Debug zstd 库。

`wavetrace_writer_helper.cmake` 负责让 cmodel 运行时能找到 writer helper。

## 9. 增量生成

### 9.1 cmodel_reggen

solution 中会看到 `cmodel_reggen`。它是 `add_custom_target()`，所以构建 cmodel
时该项目会被调度。调度不等于每次执行 Python。

真正执行的是生成到 build tree 的：

```text
run_cmodel_reggen_if_needed.cmake
```

判断输入：

```text
tools/bin/gcDefineGen.py
AQARCH/.aqtree
AQARCH/cmodel/CModelFeatureDB.csv
AQARCH/cmodel/inc/AQ.h
REG_DIR 下已有文件
```

判断输出：

```text
AQARCH/cmodel/inc/gcDefines.h
AQARCH/cmodel/isa/src/isa_instructions.h
```

触发条件：

- `cmodel_reggen.stamp` 不存在；
- 任一声明输出不存在；
- 任一已记录输入比 stamp 新。

满足条件时运行 `gcDefineGen.py`，处理 `isa_instructions.h` 的只读属性，并更新
stamp。否则只输出 `cmodel register generator is up to date`。

### 9.2 gcDefineGen.py 为什么不会无效改写

生成头包含当前时间。若直接比较文本，时间每次不同会导致每次重写，从而触发
整个 cmodel 重编译。优化版比较前把：

```text
// Chip --ARCH-- generated at ... CST
```

归一化成固定 `<timestamp>`。除时间外内容相同则保留旧文件和旧 mtime；内容
真正变化时，先写 `.tmp`，再用 `os.replace()` 原子替换。

### 9.3 ReflectGen

ReflectGen 的主输出和副产物：

```text
vec1_cl_build/WaveTracer/generated_reflect/project_reflect_auto.h
vec1_cl_build/WaveTracer/generated_reflect/root_class_closure_reflect_auto.h
```

主输出由 `add_custom_command(OUTPUT ...)` 管理，依赖包括：

- `wavetrace_reflectgen.exe`；
- 自动发现或显式指定的入口头列表；
- `ALL_HEADERS`；
- cmodel reggen stamp；
- `wavetrace_reflect_inputs.txt`。

输出存在且所有依赖时间戳不更新时，ReflectGen 不会执行。修改目标 `.h`、更换
ReflectGen exe、修改 reggen 结果或删除生成头时才会重新生成。

入口头可以通过 `WAVETRACE_REFLECT_ENTRY_HEADER` 明确指定；为空时，CMake 在
`ALL_HEADERS` 中寻找提及 `WAVETRACE_REFLECT_ROOT_CLASS` 的头。默认根类型是
`cmoTopLevel`。

正常成功构建默认不打印完整 ReflectGen 输出。需要诊断时可设置：

```text
-DWAVETRACE_REFLECT_VERBOSE=ON
-DWAVETRACE_VERBOSE_BUILD=ON
```

## 10. 增量编译与配置切换

### 不需要重跑根 BAT

- 只修改 `.cpp` 或现有 `.h`；
- 只在 Debug/Release/Profile 之间切换；
- 只重新构建某个现有 target；
- gcDefine/ReflectGen 输入变化，已有依赖可以自动检测。

### 应重新运行根 BAT 或 CMake configure

- 新增或删除 `add_subdirectory()`；
- 修改任意 `CMakeLists.txt` 或 `.cmake`；
- 修改 generator、platform、toolset、Windows SDK；
- 改 `WORKDIR`、`AQROOT`、`AQARCH`、`AQTOOLS`；
- 改 `BUILD_CUDA`、`PLATFORM_TYPE` 或 target 开关；
- 修改作为 cmodel 文件清单来源的旧 `.vcxproj`；
- 新增需要被 glob/configure 发现的输入文件。

CMake 生成的 VS 工程含 `ZERO_CHECK`，很多 CMake 输入改变时会自动重新 configure，
但涉及路径、generator 或新增目录时，主动重跑根 BAT 更容易确认错误。

### 应清理 build tree

- 从不同 VS generator/toolset 迁移；
- CMake cache 中保存了错误的绝对路径；
- junction 指向了另一个工作区；
- 同一 build tree 被不同源码根复用；
- CMake 明确报告 cache/source directory 不匹配。

普通配置切换不需要删除 `vec1_cl_build`。

## 11. 并行编译

优化版有两层并行：

1. `cmake --build ... --parallel 32`：允许 MSBuild 并行调度多个项目。
2. `/MP32`：单个 C/C++ 项目内部允许 `cl.exe` 并行编译多个源文件。

两者不是简单相乘成 1024 个持续线程，但大型 solution 可能出现 CPU、内存和
磁盘过度竞争。32 核/线程机器可先用默认值；若内存不足、系统换页或链接阶段
明显抖动，可把 `BUILD_PARALLEL` 和 `VEC1_MSVC_MP_COUNT` 降到 16 或 8。

`ENABLE_CCACHE` 和 `ENABLE_DISTCC` 默认关闭。MSVC ccache 路径会复制 launcher
为 build tree 中的 `cl.exe`，属于另一套优化，不应在没有独立验证时与 `/MP32`
同时盲目开启。

## 12. 输出目录

默认：

```text
COMPUTE_SDK_DIR = SW/projects.se/build/sdk
```

Windows 所有配置统一输出：

```text
archive/static libraries -> build/sdk/lib
DLL/EXE                  -> build/sdk/bin
PDB                      -> build/sdk/bin
```

所有 `CMAKE_*_OUTPUT_DIRECTORY_<CONFIG>` 也被设置到同一组目录，因此不会生成：

```text
build/sdk/bin/Debug
build/sdk/bin/Release
```

这种扁平布局减少了对现有部署和测试脚本的影响，但意味着不同配置会覆盖同名
最终文件。切换配置后应构建完整依赖链，并确认 PDB 与二进制时间一致。不要同时
从两个 build tree 向同一个 SDK 目录构建不同配置。

中间 `.obj`、项目级 PDB 和 CMake 内部文件仍按配置保存在 build tree 中，所以
Debug/Release 的增量对象不会互相复用。

## 13. Windows 与 Linux 边界

### 仅 Windows 生效

- 两个 `.bat` 入口；
- junction 创建；
- Visual Studio 2019 generator、x64、`host=x64`；
- `/MP32`、`/Zi`、`/Z7`、`/Oy-`、`/DEBUG:FULL`；
- SystemC/cmonitor 的 MSVC Debug/Release 路径；
- Windows zstd `.lib` 导入；
- `cmodel_reggen` 当前的 MSVC 分支。

### 也会影响其他平台的部分

- 根 CMake 对 multi-config/single-config 的判断；
- Debug 宏的 `$<CONFIG:Debug>` 写法；
- cmodel 路径归一化；
- ReflectGen 依赖列表和增量规则；
- `WAVETRACE_VERBOSE_BUILD` 默认关闭。

Linux 常用 Ninja/Makefiles 是 single-config，应显式传：

```bash
cmake -S ... -B ... -DCMAKE_BUILD_TYPE=Release
```

当前自定义 Profile flags 只在 MSVC 分支定义。Linux 即使接受
`-DCMAKE_BUILD_TYPE=Profile`，也不保证自动得到 Release 优化和调试符号。Linux
性能分析建议继续使用 Release/RelWithDebInfo，或在 Linux 工具链中单独定义
`CMAKE_C_FLAGS_PROFILE`、`CMAKE_CXX_FLAGS_PROFILE` 和 linker flags。

镜像未修改 `driver/cuda/script/build_cmodel.sh`，Linux 原有入口不会因为 BAT 改动
而改变。

## 14. 原始版到优化版的变化

优化镜像相对原始镜像只有以下路径不同：

```text
Sea-Eagle_build.bat
SW/projects.se/build-gpgpu-solution.bat
SW/projects.se/driver/cuda/CMakeLists.txt
HW/projects.se/arch/XAQ2/cmodel/CMakeLists.txt
SW/projects.se/arch/XAQ2/cmodel/CMakeLists.txt
SW/projects.se/tools/bin/gcDefineGen.py                  新增
```

主要行为变化：

- 根 BAT 从写死 Release configure 改为多配置生成和可选构建。
- 根 BAT 默认只生成 solution，失败时保留窗口和退出码。
- 根 BAT 使用普通权限 junction，并拒绝删除真实目录。
- 增加 Debug/Release/RelWithDebInfo/MinSizeRel/Profile。
- 配置判断从 `CMAKE_BUILD_TYPE` 改为 generator expressions。
- Release/Profile 生成 profiler 可用 PDB。
- `/MP` 明确为 `/MP32`，命令行 build 也支持 32 并行。
- 所有 MSVC 配置仍输出到同一 SDK bin/lib。
- 修复 Profile 对 zstd、SystemC、cmonitor 的配置映射。
- cmodel reggen 从每次无条件执行改为 stamp+输入/输出时间戳判断。
- gcDefine 语义内容不变时不改写头文件。
- ReflectGen 改为明确 `OUTPUT/BYPRODUCTS/DEPENDS` 的增量生成。
- ReflectGen 和普通 CMake 成功日志默认收敛，需要时可显式开启详细日志。

## 15. 当前镜像的完整性边界

镜像包含 131 个筛选后的构建文件：CMake、Makefile、`.mk`、BAT、PowerShell、
shell、VS `.props/.targets` 和新增的 `gcDefineGen.py`。不包含源码、生成物和
third-party build files，也按要求排除了 WaveTrace `integration/CMakeLists.txt`。

它不是完整源码树。根 CMake 明确引用但镜像中没有保存的例子包括：

```text
driver/cuda/cmake/reggen/RegGen.cmake
driver/cuda/cmake/cmodel/GCDefine.cmake
driver/cuda/driver/CMakeLists.txt
driver/cuda/dummy/CMakeLists.txt
driver/cuda/test/CMakeLists.txt
```

此外，cmodel 根文件加入的多数生产子目录 CMakeLists 也不在这份筛选镜像中。
这些文件必须由完整 Sea-Eagle 私有工作区提供。镜像适合：

- 审计入口和配置逻辑；
- 对照原版/优化版；
- 将改动覆盖回完整工作区；
- 做静态检查和 BAT/CMake 控制流模拟。

镜像不适合直接当成独立工程 configure/build。

Android.mk、QNX `.mk`、Makefile 和 shell 脚本是其他平台的独立入口，不会被
Windows `vec1_cl_build/gpgpu.sln` 自动执行。它们被保留是为了构建文件镜像完整，
不代表 Windows 根 CMake 会遍历所有文件类型。

## 16. 常见问题定位

### 双击 BAT 一闪而过

优化版结尾有 `pause`，失败也会保留窗口。若仍闪退，优先确认实际替换的是优化版
且文件为 CRLF。也可以从 cmd 执行并保留完整输出。

### `Refusing to remove non-link path`

目标位置是一个真实目录，不是 junction。脚本不会替你删除。先人工确认内容和
来源，再备份/移动该目录，不能把安全检查改成无条件 `rmdir /s /q`。

### `mklink /j` 失败

确认三个目标目录存在且是本地目录。junction 不适合 UNC 网络目标。若工作区在
网络共享上，需要重新评估链接类型和权限，不要直接取消检查。

### 配置列表里没有 Profile

可能打开了旧 build tree 或旧 `.sln`。重新运行优化版 BAT，并确认 configure
命令含完整 `CMAKE_CONFIGURATION_TYPES`。必要时删除错误的 CMake cache 后重新
生成。

### 切配置后 LNK1181 找不到库

检查报错库属于 Debug 还是 Release，确认 cmodel CMake 使用的是
`$<CONFIG:Debug>` / `$<NOT:$<CONFIG:Debug>>` 版本。旧版 configure-time
`CMAKE_BUILD_TYPE` 分支会造成这种问题。也检查依赖项目是否被当前 solution 配置
勾选构建。

### Release/Profile 看不到符号

检查：

1. `VEC1_RELEASE_DEBUG_INFO=ON`；
2. 链接命令含 `/DEBUG:FULL`；
3. `build/sdk/bin` 中存在同一时间生成的 PDB；
4. VS Modules 窗口加载的是当前 dll 和当前 PDB；
5. 没有从另一 build tree 覆盖同名 dll。

### 每次 Build 都看到 cmodel_reggen

custom target 被调度是正常的。查看输出应是：

```text
cmodel register generator is up to date
```

只有出现 `Running cmodel register generator` 才真正调用 Python。若每次都运行，
检查 stamp、两个声明输出是否存在，以及哪个输入 mtime 持续变化。

### 每次 Build 都运行 ReflectGen

检查以下文件的时间戳：

```text
project_reflect_auto.h
root_class_closure_reflect_auto.h
wavetrace_reflect_targets.txt
wavetrace_reflect_inputs.txt
全部 ALL_HEADERS
cmodel_reggen.stamp
wavetrace_reflectgen.exe
```

如果每次都重新 configure，CMake 会重写部分输入清单，从而合理触发 ReflectGen。
普通 Build 不应无故重跑。

### 修改 CMakeLists 后 solution 没有新工程

`add_subdirectory()` 是 configure 阶段行为。重新运行根 BAT 或显式执行
`cmake -S ... -B ...`，仅在 VS 中 Build 旧 solution 不一定足够。

### 输出很多日志是不是已经编译

看是否出现 `cl.exe`、`Building Custom Rule`、`.obj`、link 等编译信息。只看到
compiler detection、option summary、Configuring/Generating，说明只是生成工程。

## 17. 已验证内容

当前仓库对优化版做过以下验证：

- 镜像文件数：原始 130，优化 131；
- 原版和优化版只有文档列出的 6 个路径存在差异；
- 未混入源码、对象、库、exe、PDB、CMake cache、build tree、third-party 或
  integration CMake；
- 根 BAT generate-only；
- 根 BAT Profile + 32 并行 build 控制流；
- 子 BAT generate-only；
- 子 BAT Profile + 32 并行 build 控制流；
- 普通权限创建并识别三个 junction；
- gcDefine 首次创建、内容不变保持 mtime、内容变化时重写；
- WaveTrace solution 的 Release/Debug 全量重编译及增量对象时间戳验证；
- WaveTrace writer/reader 的百万信号闭环验证。

由于镜像不含完整私有源码和外部 SDK，Sea-Eagle 的真实完整 configure、编译、
链接和运行仍需在目标工作区中执行。

## 18. 维护规则

1. 修改 live Sea-Eagle cmodel CMake 时，确认 `SW/projects.se/arch` junction 实际
   指向的 HW 文件，不要把它误当成两套独立源码。
2. 更新镜像时，原始树保持只读历史，改动只覆盖到 optimized 树。
3. 不向镜像加入 `.sln`、`.vcxproj` 生成物、CMake cache、源码或二进制。
4. 不把 WaveTrace `integration/CMakeLists.txt` 放回 Sea-Eagle 根编译镜像。
5. 新增配置时同时检查：配置列表、编译 flags、link flags、SystemC/cmonitor 路径、
   zstd imported location 和输出目录。
6. 新增生成步骤时必须声明输出、输入依赖和缺失输出行为，避免用无条件 PRE_BUILD
   改写公共头。
7. 不在 Build 中加入 P4 拉取逻辑。源码同步和本地编译应保持两个独立动作。
