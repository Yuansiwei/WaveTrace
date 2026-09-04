# WaveTrace 完整技术交接文档

日期：2026-06-10  
适用对象：接手工程师、技术负责人、专利/技术材料撰写人  
仓库根目录：`C:\Users\13611\Documents\WaveTrace`

本文不是专利交底书，而是给接手人快速理解整个系统用的工程交接文档。写法尽量按“系统为什么存在、怎么工作、代码在哪里、如何验证、哪些地方要小心”组织。

## 1. 系统一句话概括

WaveTrace 是一套面向芯片架构仿真和大规模业务仿真的波形追踪系统。它把普通 C++ 业务对象、SystemC/类 SystemC 端口、数组、位段、联合体、布尔封装、队列/通道对象等统一映射成稳定波形拓扑，再按周期记录变化，输出为 WVZ4 二进制波形文件，并由 Qt Viewer 按需加载、搜索、显示和对比。

传统波形系统更像“用户手工登记一批信号”；WaveTrace 的目标是“系统理解业务对象结构，自动形成可观察波形树，并且在百万级信号和十 GB 级波形文件下仍然能跑”。

## 2. 总体架构

核心链路如下：

```text
业务模型头文件
    |
    | 1. ReflectGen 解析 C++ 类型、字段、位段、联合体、访问权限
    v
自动生成反射访问代码
    |
    | 2. wave::Tracer 根据反射访问代码展开对象拓扑
    v
Node/Track 稳定拓扑
    |
    | 3. 每个业务 cycle 采样，比较前后值，只提交变化
    v
TrackEvent / CycleSubmission
    |
    | 4. PathStableWvz4Recorder 映射成 WVZ4 layout 和周期提交
    v
WVZ4 Writer / Writer Helper
    |
    | 5. 分块、压缩、LOD、FOOT 索引、最终文件
    v
.wvz4 文件
    |
    | 6. Qt Viewer 目录加载、按需解码、LOD 显示、搜索、对比
    v
交互式波形查看
```

这条链路里有三个独立但相互配合的系统：

1. 反射追踪系统：解决“怎么从业务对象自动得到波形信号”。
2. WVZ4 写入和压缩系统：解决“怎么把大规模变化样本写得小、写得快、能随机访问”。
3. Viewer 系统：解决“怎么打开十 GB 级文件、只加载需要的数据、远景用概要、近景用精确样本”。

## 3. 仓库目录说明

### 根目录核心文件

`ReflectGen.cpp`

反射代码生成器。基于 libclang 解析业务头文件和工程 include 环境，输出自动反射访问代码。它负责识别类、结构体、字段、基类、私有访问授权、位段、联合体、匿名联合体、布尔 typedef、SystemC/VSIP 风格端口等。

`reflect_macro.h`

业务代码侧最轻量的接入头文件。业务模型只需要声明可反射、使用尺寸保持的波形封装，通常包含这个头即可。关键内容包括：

- `WAVE_REFLECT_FRIEND`：允许反射生成器访问 private/protected 成员。
- `wave::WaveValue<T>`：尺寸保持的标量封装，写入时通知追踪系统。
- `wave::array<T,N>`：尺寸保持的数组封装，支持元素级 dirty。
- `wave::WaveDirtyHook`：业务对象主动标记“我变了”。
- `wave::DirectReflectPointerTarget`：允许指针目标被直接展开的显式标记。
- 布尔存储适配、联合体字段元信息等基础类型。

`reflect_runtime.h`

反射访问运行时。提供序列化、反射 visitor 桥接、类型标识、指针去重等基础设施。它偏通用反射层，不直接负责 WVZ4 文件。

`wave_runtime.h`

波形追踪运行时，文件很大，是整个追踪系统的核心。主要职责：

- 构建 Node/Track 拓扑。
- 从反射 visitor 展开业务对象字段。
- 处理标量、数组、pair、字符串、指针、智能指针、端口、SystemC 信号、位段、联合体、布尔存储。
- 采样并生成 `TrackEvent`。
- 前值比较，避免未变化值输出。
- 支持 flat leaf 快速表、连续内存预比较、SIMD、dirty group、WaveValue 地址表、wave::array 元素级 dirty、并行采样。
- 支持 `WaveTraceLevel` 环境变量控制展开深度。

`wave_path_wvz4_recorder.h`

把 `wave::Tracer` 的拓扑和采样事件转换为 WVZ4 writer 的 layout 和 cycle submission。它是追踪运行时和 WVZ4 文件写入器之间的桥。

重要特性：

- 维护稳定拓扑到 WVZ4 `NAME/NODE/SIGT` 的映射。
- 支持逻辑 signal 与物理 storage stream 分离。
- 默认插入周期时钟描述。
- 支持异步写入。
- 默认启用 writer helper 进程，提高 crash/kill 下文件可恢复性。

`wvz4_writer_typed.h`

WVZ4 二进制 writer 的核心实现。它负责文件格式、分块、压缩、LOD、FOOT 索引、helper IPC 协议等。核心内容包括：

- WVZ4 layout：名字表、节点表、信号表、时钟表。
- WDAT 动态样本分块。
- 时间块 × 信号块二维切分。
- 前值去重和隐式初值。
- 局部时间编码、共享时间表、stride 时间。
- per-record value codec：布尔翻转、多字节变化掩码、nibble mask 等。
- Zstd 压缩。
- LOD / LODZ 概要数据。
- FOOT 文件尾索引。
- block pipeline：压缩线程和文件写线程分离。
- writer helper：named pipe、parent process 监控、finalize。

`wvz4_writer_monitor_main.cpp`

writer helper 进程入口。主仿真进程可以只负责把 layout/cycle frame 发给 helper；helper 独立拥有真正的 WVZ4 writer。如果主进程 crash、被 kill，或者 Visual Studio Stop Debugging 结束父进程，helper 可以把已经收到的完整周期 finalize 成可打开文件。

`wave_tap.h`

手动周期采样包装。业务模型自己推进周期，在每个稳定周期末调用 `sample_one_cycle()`。它内部执行 `begin_cycle -> tracer.sample -> end_cycle`。

`smoke_*.cpp`

烟测和功能测试入口，用于验证反射、布尔存储、storage alias、writer、LOD、大规模写入等局部能力。

### QtViewer 目录

`QtViewer/MainWindow.cpp` / `QtViewer/MainWindow.h`

Viewer 主窗口、信号树、活动信号列表、文件打开、按需加载、搜索、跳转、双文件对比等交互逻辑。

`QtViewer/WaveParser4.cpp` / `QtViewer/WaveParser4.h`

WVZ4 解析器。读取 header、layout、WDAT、FOOT、LODZ，支持按 signal/time 选择性加载，并支持只加载目录不加载样本。

`QtViewer/WaveCanvas.cpp` / `QtViewer/WaveCanvas.h`

波形绘制。负责根据当前时间窗口选择原始样本或 LOD 概要，绘制 bit/bus、差异区域、光标和 hover 状态。

`QtViewer/WaveTypes.h`

Viewer 内部波形数据结构。包括：

- `WaveSample`：单个样本。
- `WaveSignal`：信号定义、样本、LOD、diff region。
- `WaveLodBucket` / `WaveLodLevel`：LOD 概要。
- `WaveTreeInfo`：WVZ4 原生树结构。

`QtViewer/ActiveSignal*.cpp/h`

活动信号列表 UI 组件。

### props 目录

`props/llvm_local.props`

Visual Studio 工程使用本地 LLVM/libclang 的 include/lib 配置。

`props/zstd_embed.props`

把 Zstd 源码嵌入工程编译。

`props/wavetrace_app_common.props`

通用 x64 输出路径和 C++ 编译设置。

### third_party 目录

本地第三方依赖，包括 Qt、LLVM、Zstd。交付包默认不打包大型 Qt/LLVM 目录，只保留源码和文档；接手人按文档准备依赖或从现有机器复制。

## 4. 构建方式

### 4.1 主工程

使用 Visual Studio 2019/2022，打开根目录：

```text
WaveTrace.sln
```

推荐构建：

```bat
msbuild WaveTrace.sln /m /p:Configuration=Release /p:Platform=x64
```

主工程包含：

- `ReflectGen`
- `wvz4_writer_monitor`
- 多个 `smoke*` 测试项目
- 1M cycle 业务压测项目

依赖：

- LLVM/libclang：`third_party\llvm\llvm-local`
- Zstd：`third_party\zstd-src\zstd-1.5.7`
- `libclang.dll` 运行时

### 4.2 Viewer 工程

打开：

```text
QtViewer\QtViewer.sln
```

命令行构建：

```bat
cd QtViewer
msbuild QtViewer.sln /m /p:Configuration=Release /p:Platform=x64
```

运行：

```bat
run_release.bat
```

QtViewer 不依赖 Qt VS Tools，工程会直接调用本地 Qt 的 `moc/uic/rcc`。

## 5. 反射生成器工作原理

### 5.1 输入和输出

ReflectGen 的常见用法：

```bat
ReflectGen <input_header> -o <output_header>
```

批量模式：

```bat
ReflectGen --batch-dir <dir> -o <output_dir>
ReflectGen --batch-dir-list <dirs.txt> -o <output_dir>
```

也支持读取 Visual Studio 工程配置：

```bat
ReflectGen --vcxproj cmodel.vcxproj
```

输出文件通常是 `*_reflect_auto.h`，里面生成 `wave::ReflectAccess<T>` 和 `reflect::reflected_visitor<T>` 的特化。

### 5.2 私有字段访问

业务类如果希望 private/protected 成员进入波形，需要在类体中写：

```cpp
WAVE_REFLECT_FRIEND
```

宏展开后会给反射访问模板授权。ReflectGen 优先用 libclang AST 中的 friend 声明识别授权，也支持 marker fallback。这样可以避免在业务代码中为了追踪而把所有字段改成 public。

### 5.3 位段

C++ 位段不能直接取地址。ReflectGen 会把位段识别为 getter 形式：记录字段名、位宽、位偏移，运行期再通过 getter 读取值。

运行时不把每个位段都当作独立物理存储保存，而是给覆盖该位段的底层字节范围建立物理存储轨道。逻辑位段信号通过 bit offset/bit width 引用这份物理存储。

工程意义：

- Viewer 能看到业务字段名。
- WVZ4 只保存底层数据一次。
- 多个位段共享同一 storage 时不会重复写文件。

### 5.4 联合体和匿名联合体

ReflectGen 会识别 union 字段、匿名 union 注入字段以及 union storage size。运行时展开 union 字段时，会建立 union alias context，把多个成员视为同一底层存储的不同逻辑视图。

工程意义：

- 业务上能看到 `u32/f32/lo/hi` 这类不同解释。
- 文件中只保存 raw storage 一份。
- 嵌套 union、wave::array 嵌套 union 也可以按同一逻辑处理。

### 5.5 布尔存储 typedef

业务中常见 `typedef unsigned char U01` 这种一字节物理存储、一位逻辑语义。ReflectGen 默认把 `U01` 作为 BoolStorage 处理。

效果：

- 文件和 Viewer 中显示为 1-bit Bool。
- 物理地址仍是原始字节地址，便于 memory block 预比较。

### 5.6 SystemC / VSIP 端口

ReflectGen 会过滤大量 STL/SystemC 内部结构，只保留能形成业务值源的端口或 wrapper。运行时对端口采用延迟读取，避免 elaboration 阶段未绑定端口被提前访问。

对于 VSIP 风格端口，运行时优先使用 `peek()` 路径取得稳定指针；如果有 dirty hook，可以归入 dirty group。

## 6. 追踪运行时工作原理

### 6.1 拓扑与样本分离

`wave::Tracer` 先建立稳定拓扑：

- `NodeDecl`：层级节点，例如对象、字段、数组元素、容器。
- `TrackDecl`：叶子信号，例如某个标量字段、位段逻辑视图、端口值源。

后续采样只产出 `TrackEvent`，不重复解释对象结构。

这种分离很关键：百万级信号场景下，不能每个 cycle 都重新走树、拼路径、查类型、分配字符串。

### 6.2 Track 与 storage 的关系

逻辑 Track 是 Viewer 能看到的业务信号；storage 是实际采样和写入的物理数据流。

普通信号：

```text
track_id == storage_id
```

位段、联合体、多视图场景：

```text
多个 track_id -> 同一个 storage_id + 不同 bit range
```

这是后续 WVZ4 压缩和 Viewer 切片读取的基础。

### 6.3 WaveTraceLevel

环境变量：

```text
WaveTraceLevel=<正整数>
```

含义：限制信号路径中 `.` 的最大数量，从而控制自动展开深度。未设置或非法值表示不限制。

用途：

- 大模型首次接入时先低层级追踪，避免信号量失控。
- 定位问题时再提高层级。
- 不需要改业务代码即可控制追踪规模。

### 6.4 变化采样

运行时默认只输出变化，不输出未变化值。每个叶子信号维护上一有效值，采样时读取当前值并比较：

```text
当前值 == 上次值 -> 不产生事件
当前值 != 上次值 -> 产生 TrackEvent
```

这样 WVZ4 writer 收到的是按 cycle 聚合的真实变化集合。

### 6.5 Flat leaf 快速表

拓扑冻结后，Tracer 会把可快速采样的叶子整理成平坦表。采样时直接线性遍历平坦表，不再递归树结构。

优化目的：

- 减少指针跳转。
- 减少 map/set 查找。
- 减少路径字符串处理。
- 更容易并行切片。

### 6.6 连续内存预比较和 SIMD

对于连续内存区域，例如数组、结构体片段、wave::array 元素范围，运行时可以先比较整块内存 shadow。如果整块没变，就跳过内部所有叶子。

实现中可选择 scalar、SSE2、AVX2 或自动选择后端。原则是：

```text
块级判断没变 -> 子字段都不采样
块级判断变了 -> 再进入字段级精确采样
```

该优化对大数组、密集结构体、队列槽位特别重要。

### 6.7 主动 dirty 机制

被动轮询适合简单字段，但百万级信号下每周期全扫成本高。WaveTrace 支持几类主动变更提示：

1. `WaveDirtyHook`：业务对象在 write/push/pop/update 时显式调用 `mark_dirty()`。
2. `WaveValue<T>`：尺寸保持标量封装，赋值时通知地址。
3. `wave::array<T,N>`：尺寸保持数组封装，元素访问或写入时通知元素地址。
4. dirty peek group：端口或通道通过 `peek()` 暴露值，写路径标记同一底层值源 dirty。

这些机制共同目标是：让运行时优先检查“可能变化的地方”，而不是每 cycle 扫所有信号。

### 6.8 多线程采样

拓扑稳定后，叶子采样任务可以按连续区间分给多个 worker。Worker 只读业务对象和本地 shadow，并把变化写入线程本地 buffer。最后由主线程统一提交给 sink。

注意：采样必须发生在业务 cycle 稳定点。多线程业务仿真需要在采样前设置 barrier，确保所有业务写入都完成。

## 7. WVZ4 Writer 与文件格式

### 7.1 文件设计目标

WVZ4 的目标不是“把 VCD 用压缩库压一下”，而是从波形语义上降低冗余：

- 拓扑和动态样本分离。
- 默认初值隐式表达。
- 每个信号只写变化。
- 时间用局部编码。
- 值用固定宽二进制。
- 信号归属由块内结构隐含。
- 按时间和信号二维分块。
- 再用 Zstd 做通用压缩。

### 7.2 Layout

WVZ4 layout 主要包含：

- 名字表：局部名字字符串。
- 节点表：父子关系、first_child/next_sibling。
- 信号表：signal id、storage id、node id、类型、位宽、位偏移、显示进制、是否 storage-only。
- 时钟表：周期性时钟可以用初值和周期描述，不必写成普通 WDAT 变化。

### 7.3 动态样本 WDAT

WDAT 按时间块提交。每个 block 覆盖一段 cycle 范围。启用 signal chunking 时，再按 signal id 分块：

```text
time block × signal chunk -> WDAT tile
```

tile 内部包含：

- block id
- start/end cycle
- flags
- signal chunk id
- first signal id
- signal count
- 可选共享时间表
- offset 表
- records blob

某个信号的 record 通过 offset 表定位。空 record 表示该信号在这个 tile 内没有变化。

### 7.4 时间压缩

Writer 会在多种时间表达之间选择：

- 普通相对时间。
- 单信号 delta time。
- tile 内共享时间表。
- stride time。

选择原则是 payload 更小且可无损恢复。

### 7.5 值压缩

Writer 支持多种 record codec：

- 完整固定宽值。
- 布尔翻转序列。
- 多字节值的 changed-byte mask。
- 3/4 字节值的 nibble mask。
- stride variants。

这些 codec 的共同点是：先利用信号类型、位宽和变化规律减少 raw payload，再交给 Zstd。

### 7.6 Sparse / Dense tile

信号块里可能只有少数信号变化，也可能大部分信号都变化。Writer 可以根据密度选择：

- sparse record：只列有变化信号。
- dense record：对整个 chunk 建 offset。

目的：避免稀疏场景下 offset 表太大，也避免稠密场景下 sparse 列表太重。

### 7.7 LOD / LODZ

LOD 是给 Viewer 远距离显示用的概要。Writer 在写原始 transition 的同时维护多级概要。

关键策略：

- 基础 bucket cycles 默认从较小粒度开始。
- 每一级相对于来源记录最多保留约 20%。
- 每个 bucket 记录最后值、最小值、最大值、变化次数、状态集合。
- 对于远景显示，Viewer 可以直接用 LOD，避免加载全部原始样本。
- LODZ 将概要按 level、signal chunk 和时间 chunk 切分，避免所有 LOD 堆在 FOOT 里导致文件尾过大。

特别注意“尾锚点”：概要 bucket 必须记录窗口内最后一次变化，而不是只记录第一处变化。因为后续长时间稳定区的值由最后一次变化决定。

### 7.8 FOOT

FOOT 是文件尾索引。Reader/Viewer 通过 FOOT 知道：

- WDAT block 在哪里。
- block 覆盖哪些时间和信号范围。
- block 压缩前后大小。
- signal chunk 索引。
- LOD level 和 LODZ chunk 索引。

WVZ4 v3+ 对 finalized FOOT/footer_offset 有强依赖。未 finalize 的直接写文件不应该被当作完整文件打开。

### 7.9 Block pipeline

Writer 的 block pipeline 把工作拆成：

```text
主线程提交 cycle
    -> commit block 构造 raw payload
    -> compression workers 压缩
    -> file writer thread 按 block_id 顺序落盘
```

这样可以减少主仿真线程等待压缩和磁盘写入的时间。

### 7.10 Helper 进程抗 crash/kill

直接在主仿真进程里写文件的问题：如果进程被 kill，文件可能停在半个 section 或没有 FOOT。

helper 模式：

1. 主进程创建 writer helper。
2. 两者通过 Windows named pipe 传 layout/cycle/finalize frame。
3. helper 拥有真正的 WVZ4 writer。
4. helper 监控父进程 handle 和 pipe 状态。
5. 父进程正常关闭时发送 finalize。
6. 父进程 crash/kill 时，helper 将已完整接收并确认的 cycle finalize。

保证边界：

- 能恢复到 helper 已完整收到的最后一个周期。
- 不能恢复还在主进程内存里的未发送数据。
- 不能恢复半个 pipe frame。
- 如果 helper 自己也被 kill，仍可能生成未完成文件。

## 8. Viewer 工作原理

### 8.1 打开文件

`WaveParser4::LoadOptions` 支持目录优先：

- `includeAllSignalDefinitions = true`：加载所有信号定义。
- `autoLoadFirstSignalCount = 0`：只加载目录，不加载原始样本。
- `autoLoadFirstSignalLodCount`：可预加载前几行 LOD。
- `signalIds`：只加载指定信号。
- `timeStart/timeEnd`：只加载指定时间窗口。
- `maxDecodedSamples`：防止一次解码爆内存。
- `allowUnfinalized = false`：默认拒绝未 finalize WVZ4。

Viewer 打开大文件时优先加载目录和索引，不全量解码 WDAT。

### 8.2 按需加载

MainWindow 维护：

- 当前波形文件路径。
- 是否支持按需加载。
- signal id 到 signal index 的映射。
- 活动信号列表。

当用户把信号加入 active list 或缩放/平移到需要精确样本时，`ensureSignalSamplesLoaded()` 重新调用 parser，只加载对应 signal/time 的样本。

### 8.3 LOD 绘制

WaveCanvas 根据当前可见时间跨度和绘图区宽度选择：

```text
周期/像素很大 -> 使用 LOD
周期/像素较小 -> 使用原始样本
```

LOD bucket 用于判断：

- 该区间是否稳定。
- 是否出现高/低/Z/absent。
- bus 是否需要画成活动区间。
- 显示最后值、最大最小值或状态集合。

放大后再回到原始 transition，保证精确性。

### 8.4 数值搜索

快捷键 `Ctrl+F` 打开数值搜索。当前实现针对选中或高亮的 active signals：

1. 解析输入目标值。
2. 按每个信号位宽裁剪目标值。
3. 确保目标信号样本已经加载。
4. 遍历样本，统计命中。
5. 生成按信号分组的结果树。
6. 搜索完成后自动跳到第一个命中。
7. 也支持上一个/下一个命中导航。

搜索时对不同 radix 的显示不敏感，本质比较 raw bits。

### 8.5 双文件对比

Viewer 支持加载两个波形后做信号配对和差异区间计算。差异结果写入 `WaveDiffRegion`，Canvas 用 overlay 区间显示。

对比的正确方向是：

- 先用拓扑/路径匹配信号。
- 再按数值状态比较。
- 不依赖文本显示字符串。

### 8.6 Storage alias 显示

如果 WVZ4 中多个逻辑 signal 共享同一 storage id，Parser 会先解码物理 storage 样本，再根据每个逻辑 signal 的 bit offset/width 切片。

这保证 bitfield/union/anonymous union 在 Viewer 中可见，同时不会重复加载底层数据。

## 9. 大规模压测入口

`smoke_business_1m_writer.cpp` 是业务型压测入口，可以生成大量 signal 和大量 cycle：

```bat
smoke_business_1m_writer.exe out.wvz4 1000000 1000000 1 --lod
```

参数含义：

```text
out.wvz4              输出文件
1000000              business cycles
1000000              signal count
1                    每个 cycle 旋转更新的信号数
--lod / --no-lod     是否生成 LOD
--helper             走 helper 进程
--helper-exe path    指定 helper exe
--progress N         每 N cycle 打印进度
```

默认 writer options：

- block span：8192
- signals per chunk：256
- Zstd level：3
- block pipeline queue limit：16
- LOD enabled

这个测试更接近“百万 cycle + 百万 signal + 不稀疏/可控变化密度”的性能压测。

## 10. 重要测试文件说明

`smoke_bool_storage.cpp`

验证 `U01` 等 typedef 是否按 Bool 信号处理。

`smoke_tap_bool_storage.cpp`

验证 WaveTap 路径下布尔存储追踪。

`smoke_storage_alias_writer.cpp`

验证 bitfield/union/storage alias 写入关系。

`smoke_business_1m_writer.cpp`

验证 WVZ4 writer 在大规模业务 cycle 和大量 signal 下的吞吐、文件大小、LOD 开销。

`QtViewer/smoke_wvz4_parser.cpp`

Viewer parser 的独立 smoke 入口，项目中作为非构建文件保留。

## 11. 典型接入方式

业务头文件：

```cpp
#include "reflect_macro.h"

struct Top {
    WAVE_REFLECT_FRIEND

private:
    wave::WaveU32 state;
    wave::array<unsigned, 16> counters;
};
```

仿真侧：

```cpp
#include <systemc>
#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"

Top top;
std::string error;

PathStableWvz4Recorder recorder;
PathStableWvz4Recorder::OpenConfig cfg;
cfg.file_path = "out.wvz4";
recorder.open(cfg, error);

wave::BuildOptions opt;
opt.enable_flat_leaf_fast_table = true;
opt.enable_flat_memory_block_precheck = true;
opt.enable_dirty_peek_groups = true;
opt.enable_wave_value_dirty = true;
opt.enable_wave_array_dirty = true;
opt.enable_parallel_sampling = true;

wave::Tracer tracer(recorder, opt);
tracer.add_root("top", &top);

sc_core::sc_clock clk("clk", sc_core::sc_time(1, sc_core::SC_NS));
wave::WaveTap tap("wave_tap", tracer, recorder, clk);
sc_core::sc_start();

recorder.close(error);
```

## 12. 已知边界和注意事项

1. 当前系统主要面向稳定拓扑。动态增加/删除信号不是 WVZ4 的主设计目标。
2. WVZ4 typed writer 主要面向 64 位以内标量。更宽向量需要扩展 value representation。
3. helper 进程抗 crash/kill 主要是 Windows 路径，依赖 named pipe 和 process handle。
4. helper 只能保存已完整收到并确认的 cycle。
5. Viewer 默认拒绝没有 finalized footer 的 WVZ4。
6. LOD 用于远景显示，不应作为精确取值的最终依据。精确搜索/导出/对比应回到原始 transition。
7. 多线程业务模型必须在采样前做 barrier。
8. `WaveValue<T>` 和 `wave::array<T,N>` 是尺寸保持封装，但业务代码不能绕过正常写路径长期修改 raw storage，否则 dirty 提示会失效。
9. `WaveTraceLevel` 只在运行时首次读取后缓存，改变环境变量通常需要重启进程。
10. 当前仓库含有较多历史 patch 文档和临时构建产物，交付包会排除 `.obj/.vs/build_vs/_deps` 等非源码内容。

## 13. 后续优化方向

可以继续推进的方向：

1. 把 WVZ4 格式文档从代码中独立出来，形成正式 spec。
2. 给 writer/helper IPC 增加独立协议测试。
3. 给 Viewer 的按需加载增加异步任务队列，避免 UI thread 等待大块解压。
4. 给 LOD 增加更多针对 bus 的摘要，例如哈希、状态集合压缩、边沿密度。
5. 为宽总线或结构化 payload 扩展超过 64 位的 typed value。
6. 给 FIFO/队列建立统一 traits 或基类接入方案。
7. 将 ReflectGen 的工程集成做成更稳定的批处理入口，减少人工命令参数。
8. 增加跨平台 helper 实现。
9. 增加端到端 benchmark 报告自动生成。
10. 把专利材料和工程实现解耦，用术语表把内部名映射成正式技术名。
