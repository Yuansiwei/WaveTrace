# WaveTrace 使用手册

本文面向在 C++/SystemC 模型中接入 WaveTrace、生成 WVZ4 波形并使用 QtViewer 查看结果的开发者。内容以 2026-07-11 当前源码为准；旧设计文档里的 dirty group、逐逻辑叶子采样等内部描述，不应替代本文所述的当前接口和行为。

## 1. 系统组成

正常数据流：

```text
业务对象
  -> ReflectGen 生成类型访问代码
  -> wave::Tracer 构建稳定拓扑并采样
  -> wave::WaveTap 定义业务周期边界
  -> PathStableWvz4Recorder 整理物理 storage 更新
  -> wvz4_writer_monitor.exe 独立进程写 WVZ4
  -> QtViewer 打开和分析 WVZ4
```

| 文件 | 用途 |
| --- | --- |
| `reflect_macro.h` | 反射标记、`WaveValue`、`WavePtr`、`wave::array`、dirty/dynamic 接口 |
| `reflect_runtime.h` | 生成代码使用的反射运行时 |
| `wave_runtime.h` | 拓扑、物理 storage、采样与并行运行时 |
| `wave_tap.h` | 对外周期采样入口 |
| `wave_path_wvz4_recorder.h` | 稳定拓扑到 WVZ4 的记录器 |
| `wvz4_writer_typed.h` | WVZ4 布局、编码、压缩与 helper 客户端 |
| `tools/bin/wavetrace_reflectgen.exe` | 预编译反射生成器 |
| `tools/bin/wvz4_writer_monitor.exe` | 独立 WVZ4 写入进程 |
| `QtViewer/build/x64/Release/QtViewer.exe` | WVZ4 查看器 |

## 2. 最小接入

### 2.1 让类型可反射

公有字段可直接生成访问代码。类型包含要反射的 private/protected 字段时，在类内加入：

```cpp
#include "reflect_macro.h"

class GpuState {
    WAVE_REFLECT_FRIEND
public:
    std::uint32_t cycle = 0;
private:
    std::uint32_t status = 0;
};
```

`WAVE_REFLECT_FRIEND` 只提供生成代码所需访问权限和标记，不增加对象实例数据。

### 2.2 包含生成头

业务工程应把生成目录加入 include path，并在合适的翻译单元中包含：

```cpp
#include "project_reflect_auto.h"
```

典型生成位置：

```text
<build-dir>/WaveTracer/generated_reflect/project_reflect_auto.h
<build-dir>/WaveTracer/generated_reflect/root_class_closure_reflect_auto.h
<build-dir>/WaveTracer/generated_reflect/reflectgen.log
```

不要手工修改生成头。源头头文件、编译宏或 include path 变化后，让构建系统重新运行 ReflectGen。

### 2.3 创建 recorder、tracer 和 tap

```cpp
#include "wave_tap.h"
#include "project_reflect_auto.h"

int run_model(GpuState& gpu) {
    std::string error;

    PathStableWvz4Recorder recorder;
    PathStableWvz4Recorder::OpenConfig output;
    output.file_path = "gpu.wvz4";
    output.emit_default_clk = true;
    output.clk_period_ticks = 10;
    if (!recorder.open(output, error)) return 1;

    wave::BuildOptions options;
    wave::Tracer tracer(recorder, options);
    tracer.add_root("gpu", &gpu);
    wave::WaveTap tap(tracer, recorder);

    for (int i = 0; i != 100; ++i) {
        // 完成本周期业务计算，并等待所有写线程到达屏障。
        gpu.cycle = static_cast<std::uint32_t>(i);
        if (!tap.sample_one_cycle()) {
            error = tap.last_error();
            recorder.close(error);
            return 2;
        }
    }

    return recorder.close(error) ? 0 : 3;
}
```

生命周期必须是：

1. 构造并初始化业务对象。
2. `recorder.open()`。
3. 构造 `Tracer`，调用 `tracer.add_root()`。
4. 构造 `WaveTap`。
5. 每个稳定业务周期调用一次 `tap.sample_one_cycle()`。
6. 正常结束时调用 `recorder.close()`。

`sample_one_cycle()` 是业务代码唯一应使用的周期采样入口。不要自行组合 `prepare_topology()`、`begin_cycle()`、`sample()` 和 `end_cycle()`，也不要手工传 cycle。

## 3. 周期与稳定拓扑

第一次 `sample_one_cycle()` 会延迟完成：

1. 展开所有 root，建立节点、逻辑信号和物理 storage。
2. 冻结稳定拓扑，建立 flat、dirty 和 memory-block 索引。
3. 把布局交给 writer helper。
4. 采样业务 cycle 0。

只有全部成功，`WaveTap` 才把内部 cycle 从 0 增加到 1。失败时读取 `tap.last_error()`，cycle 不会前进。

writer 打开后禁止改变拓扑。以下操作必须在第一次采样前完成：

- 添加全部 root。
- 给 `WavePtr` 设置目标指针和 `declareSize()`。
- 注册 dynamic/peek 目标类型。
- 完成决定对象结构和指针目标的初始化。

第一次采样后修改普通值是允许的；替换被追踪指针或改变声明长度，不会自动重建波形树。

`sample_one_cycle()` 不是并发快照。业务写线程必须先到达 barrier/join 点，采样完成后才能开始下一周期写入，否则会产生不一致值或数据竞争。

## 4. 类型、路径与物理 storage

常见可反射对象：

- C++ 算术标量和枚举。
- 普通 class/struct 及其继承层次。
- C 数组、`std::array`、`wave::array`。
- 当前生成器支持的标准容器和业务包装器。
- `WavePtr` 指向的单对象或连续对象数组。
- `WaveValue`、dirty peek 和 dynamic target。

路径由 root 名、成员名和数组索引组成：

```text
gpu.epoch
gpu.clusters.[0].state
gpu.slots.[7].counter
```

逻辑信号与物理 storage 分离。bitfield、union alias 等多个逻辑路径可以共享一个物理 storage；采样只提交一次物理值，viewer 根据 `storage_id`、位偏移和位宽还原逻辑信号。

默认不展开 union 和 bitfield 字段。需要时设置：

```cpp
options.enable_union_fields = true;
options.enable_bitfield_fields = true;
```

## 5. 指针策略

裸指针默认不递归展开，避免误追踪临时地址、外部所有权和动态对象图。需要追踪目标时使用显式 `WavePtr`，支持 `T*`、`std::unique_ptr<T,D>` 和 `std::shared_ptr<T>`：

```cpp
wave::WavePtr<MyNode*> node;
node = &owned_node;

wave::WavePtr<MyNode*> nodes;
nodes = buffer;
nodes.declareSize(count); // 首次采样前
```

`declareSize(n)` 表示展开 `ptr[0]` 到 `ptr[n-1]`。首次拓扑冻结后再替换指针不会重建拓扑。

## 6. `WaveValue<T>`

`WaveValue<T>` 用于写驱动标量，只在值真正改变时上报地址，且大小和对齐与 `T` 一致：

```cpp
wave::WaveValue<std::uint32_t> counter;
counter = 10;
counter += 1;
std::uint32_t value = counter.read();
```

构造/反序列化阶段需要绕过 dirty 通知时，可在首次追踪前使用：

```cpp
counter.raw_unsafe_for_initialization_only() = initial_value;
```

不要在正常仿真阶段通过这个引用修改值，否则运行时不会收到 dirty 通知。

## 7. `wave::array<T,N>`

`wave::array` 是保持尺寸的 `std::array` 包装器，对象内部不保存 tracer 指针、group id 或 block id。

```cpp
wave::array<Item, 256> items;
items[3].state = 1;       // 只标记第 3 个元素对应范围

const auto& citems = items;
auto x = citems[3].state; // const 访问，不报 dirty

Item* p = items.data();   // 返回可写入口前标记整个数组范围
p[7].state = 2;
```

接口规则：

- 非 const `operator[]`/`at`/`front`/`back` 标记所选元素。
- const 访问以及 `read()` 不标记 dirty。
- 非 const `data()`、`begin()`、`rbegin()`、`operator&()` 标记整段，因为后续写入位置不可知。
- 赋值、`fill()`、`swap()` 标记整段。
- `end()`/`rend()` 本身不标记；获得可写起点的入口已经负责整段通知。
- 只提供到 `const std::array<T,N>&` 的隐式转换，不提供可写 `std::array&` 或裸指针隐式转换。

嵌套数组会延迟到最内层实际元素再通知：

```cpp
wave::array<wave::array<std::uint32_t, 8>, 4> table;
table[1][3] = 9; // 标记 [1][3]，不是整个 table[1]
```

该规则可递归到多层。局部变量、构造阶段临时对象和未被 active tracer 覆盖的 `wave::array` 可以正常使用，其通知会被识别为未追踪地址并忽略。真正处于追踪范围但缺少映射属于拓扑错误，会输出诊断并终止；系统不做逐元素兜底。

## 8. Dirty Peek 与 Dynamic Target

`WaveDirtyHook::mark_dirty()` 只上报“这个源发生过写入”。真正读取在周期边界执行，不会在 `mark_dirty()` 调用点立即采样。

默认调用 `sample_one_cycle()` 的线程会自动绑定 tracer；其他会调用 dirty hook 的业务写线程使用：

```cpp
tap.attach_current_thread();
// 本线程执行会调用 mark_dirty() 的业务写入。
tap.detach_current_thread();
```

线程池应在线程生命周期内 attach/detach，不要每次字段写入都绑定。

Peek source 示例：

```cpp
struct Channel : wave::PeekTraceSourceFor<Channel, Payload> {
    Payload value{};
    const Payload* peek() const noexcept { return &value; }

    void write(std::uint32_t v) {
        value.count = v;
        wave_dirty_hook()->mark_dirty();
    }
};
```

Peek 包装层在路径中透明。若成员名是 `peek`，路径可为 `top.peek.count`，不会人为增加 `.value` 节点。启用 hook 优化：

```cpp
options.enable_dirty_peek_groups = true;
```

未启用时保持 pull/poll 行为。

Dynamic target 示例：

```cpp
class DynamicChannel : public wave::DynamicTraceTargetFor<DynamicChannel> {
    // reflected members and write methods
};

wave::ensure_dynamic_type_registered<DynamicChannel>();
wave::ensure_dynamic_type_registered<Payload>();
```

注册必须在首次展开前完成。Dynamic/peek 最终映射到构建阶段记录的物理 block cursor/range；周期采样面向物理 storage，不递归扫描逻辑 alias。

## 9. 常用 `BuildOptions`

| 选项 | 默认 | 说明 |
| --- | ---: | --- |
| `emit_only_on_change` | `true` | 只提交变化值 |
| `enable_flat_leaf_fast_table` | `true` | 稳定拓扑 flat 快路径 |
| `enable_node_name_interning` | `true` | 重复局部节点名驻留 |
| `enable_parallel_topology_expansion` | `true` | 大型 `WavePtr<T[]>` 并行展开 |
| `topology_expansion_threads` | `16` | 拓扑展开线程数 |
| `enable_parallel_sampling` | `false` | 通用并行采样开关 |
| `sampling_threads` | `15` | 后台 worker 数；加 caller 共 16 线程 |
| `parallel_sampling_threshold` | `8192` | 通用并行门槛 |
| `parallel_flat_leaf_threshold` | `8192` | flat 并行门槛 |
| `enable_wave_array_dirty` | `true` | `wave::array` dirty 范围采样 |
| `enable_wave_value_dirty` | `true` | `WaveValue` 写驱动采样 |
| `enable_dirty_peek_groups` | `false` | dirty peek hook 优化，需显式开启 |
| `dump_leaf_distribution_after_topology` | `true` | 首次展开后输出叶子分布 |
| `dump_memory_usage_after_topology` | `false` | 输出拓扑内存报告 |

从默认配置开始。确认所有被采样 getter、SystemC 对象及业务包装器允许并发读取后，再开启：

```cpp
options.enable_parallel_sampling = true;
options.sampling_threads = 15; // 15 worker + 当前采样线程
```

`sampling_threads=0` 表示自动使用 `hardware_concurrency()-1` 个 worker。应以真实模型和 profiler 结果选线程数。

拓扑很大时可开启：

```cpp
options.dump_memory_usage_after_topology = true;
options.memory_usage_dump_path = "wave_memory_usage.txt";
```

生产运行不建议开启逐事件 debug log、逐 track path 或每周期进度输出。

## 10. WVZ4 输出

### 10.1 周期和合成时钟

默认配置：

```cpp
output.emit_default_clk = true;
output.default_clk_name = "clk";
output.clk_initial_value = false;
output.clk_period_ticks = 10;
output.clk_fall_offset_ticks = 5;
```

业务 cycle `n` 映射到 writer 时间 `n * clk_period_ticks`。关闭 `emit_default_clk` 只隐藏合成 clk 信号，不改变 writer 时间单位。

### 10.2 压缩和流水线

主要默认值：

```cpp
output.options.compression = wvz4::Compression::Zstd;
output.options.zstd_level = 3;
output.options.enable_block_pipeline = true;
output.options.block_pipeline_threads = 0; // 自动，约硬件线程一半
output.options.block_pipeline_queue_limit = 8;
output.options.target_block_span = 100000;
output.options.enable_signal_chunking = true;
output.options.signals_per_chunk = 32;
output.options.enable_lod_tables = true;
```

`block_pipeline_queue_limit=0` 表示无限队列，可能显著增加内存。大量信号初始化时，默认 `implicit_zero_initial_values=true` 会省略 cycle 0 的全零值。

### 10.3 helper 进程

`PathStableWvz4Recorder` 固定使用 `tools/bin/wvz4_writer_monitor.exe`，发布包必须保留 helper 及依赖的相对布局。

helper 只保证已完整接收的 cycle frame 可恢复。业务进程或 helper 在当前帧传输中被强杀时，未完整提交的最后一帧不保证保留。正常路径必须调用 `recorder.close()`，使 writer 写出尾部索引和最终标记。

## 11. SystemC

包含 `systemc.h` 时，`wave_tap.h` 提供 `SystemCStartSampler`，用于在 `start_of_simulation()` 采一次初始稳定状态。一般仿真循环仍应在明确的周期完成点调用 `sample_one_cycle()`。

若调度中有多个写线程或 process，仍需确保采样时本周期所有相关更新已完成。不要把 `WaveTap` 当作异步观察器。

## 12. 使用 QtViewer

启动 viewer：

```powershell
QtViewer\run_release.bat
```

直接打开文件：

```powershell
QtViewer\run_release.bat gpu.wvz4
```

自动验证文件可打开：

```powershell
QtViewer\build\x64\Release\QtViewer.exe --open-and-exit gpu.wvz4 500
```

返回码 0 表示成功；500 是等待 UI/按需解析稳定的毫秒数。

writer 和 viewer 必须来自同一版发布包。出现“不支持 WVZ4 version/codec”时，优先检查实际加载的 viewer exe 是否为旧版本。

## 13. 日志与诊断

| 文件 | 产生条件 | 用途 |
| --- | --- | --- |
| `reflectgen.log` | ReflectGen 运行 | 输入头、clang 参数、解析和生成错误 |
| `wave_leaf_distribution.txt` | 默认首次拓扑展开 | flat、dirty peek、wave array 叶子分布 |
| `wave_runtime_debug.log` | `debug_log=true` | 运行时拓扑/dirty 诊断 |
| `wave_runtime_error.log` | fatal 映射、重复 storage 等 | 高信号错误和相关路径 |
| `<file>.writer.log` | helper writer 会话 | 初始化、布局传输和阶段耗时 |
| `<file>.log` | `enable_stats_log=true` | close 后的压缩、块、信号统计 |
| `wvz4_writer_backlog.log` | 默认低频诊断 | writer 队列积压 |
| `wave_memory_usage.txt` | 显式开启 | 拓扑容量和内存估算 |

writer 初始化诊断可设置：

```cpp
output.options.diagnostic_log_path = "gpu.wvz4.writer.log";
```

backlog 环境变量：

```bat
set WVZ4_BACKLOG_LOG_DISABLE=1
set WVZ4_BACKLOG_LOG_FILE=D:\logs\wvz4_writer_backlog.log
set WVZ4_BACKLOG_LOG_MAX_LINES=1024
```

## 14. 常见故障

### `lazy topology produced no reflected tracks`

检查是否调用 `add_root()`、root 是否存活、是否包含当前生成头、`reflectgen.log` 是否生成该类型，以及 private 类型是否使用 `WAVE_REFLECT_FRIEND`。

### `topology changed after writer open`

首次采样后又增加 root 或声明新信号。把全部拓扑初始化移到 cycle 0 之前。

### `duplicate storage sample in one cycle`

同一物理 storage 在同一 cycle 被提交两次。查看 `wave_runtime_error.log` 中 first/second track、storage id 和路径。不要在 recorder 层静默去重，应修复采样调度或 alias owner 识别。

### `wave::array bulk dirty notify failed`

当前实现不做逐元素兜底。日志会区分未追踪局部变量和应当追踪但映射缺失的地址。若是追踪对象，检查首次展开时数组地址、类型、元素大小/数量是否与通知一致。

### helper 连接或启动失败

检查 helper 是否存在、exe/helper/zstd 是否来自同一发布包、安全软件是否阻止子进程或 IPC，以及连接超时是否适合当前机器。

### viewer 无法打开

先运行 `--open-and-exit`，再核对 viewer 时间戳、实际路径和 writer 版本。正常结束前未执行 `recorder.close()` 的文件可能缺少完整 FOOT/索引。

## 15. 性能建议

1. 使用 Release 或带优化和 PDB 的 Profile/RelWithDebInfo 测性能，不用 Debug 判断热点。
2. 保持 `emit_only_on_change=true`。
3. 大数组优先用元素写入口；只有确实会任意写整段时才调用非 const `data()/begin()`。
4. 确认线程安全后再开启并行采样，并用 1、4、8、16、32 总线程实测。
5. 默认保留 memory-block SIMD/byte-map；不要拆成大量过小任务。
6. 大型 `WavePtr<T[]>` 默认可并行展开；真实跨元素物理 alias 会拒绝并行片段并串行重建，保持一个 storage 只有一个 owner。
7. 内存阶跃优先检查 cycle 0 worker event buffer、LOD 表和 writer pipeline 队列。
8. 大规模写入关注 `<file>.writer.log` 的布局传输、server open、首帧、压缩和队列等待分解。

## 16. 发布前检查

- ReflectGen 输出随目标头时间戳增量更新。
- root、`WavePtr::declareSize()`、动态类型注册都在 cycle 0 前完成。
- 所有写线程在采样点有明确屏障。
- dirty 写线程已 attach。
- cycle 0、无变化 cycle、单字段变化、整数组入口、nested array、peek、dynamic 都有测试。
- parser 验证路径、cycle 和值，而不只是 writer 返回 0。
- writer/viewer 使用同一发布包。
- 正常结束调用 `recorder.close()`。
- 生产配置关闭高频 debug log。

代表性闭环样例：

- `smoke_complex_class_wvz4_writer.cpp`
- `smoke_dirty_array_wvz4_writer.cpp`
- `smoke_nested_wave_array_wvz4_writer.cpp`
- `smoke_dirty_peek_dynamic_wvz4_writer.cpp`
- `QtViewer/smoke_wvz4_complex_class_parser.cpp`
- `QtViewer/smoke_wvz4_dirty_array_parser.cpp`
- `QtViewer/smoke_wvz4_nested_wave_array_parser.cpp`
- `QtViewer/smoke_wvz4_dirty_peek_dynamic_parser.cpp`

这些样例同时验证 writer、WVZ4 和 reader 端的值/路径/cycle，可作为业务接入后的回归基线。
