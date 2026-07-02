# WaveTrace 全反射系统详解

本文给接手 WaveTrace 反射追踪系统的对话使用，重点说明系统原理、代码入口、关键数据流、性能优化点，以及当前已验证的测试范围。

## 1. 系统目标

WaveTrace 的反射系统不是简单把 C++ 对象打印成文本，而是把芯片架构仿真中的 C++/SystemC 业务对象自动展开成稳定的波形拓扑：

```text
业务对象
  -> 反射生成器识别类型和字段
  -> 运行时展开 Node/Track 拓扑
  -> 周期采样产生 TrackEvent
  -> WVZ4 writer 写成二进制波形文件
  -> Viewer 按树和信号读取显示
```

核心目标有四个：

1. 通用性：业务模型只做少量标记，不为每个类手写波形导出代码。
2. 正确性：bitfield、union、匿名 union、端口、peek 值源、指针目标、bool typedef 等业务语义都能追踪。
3. 性能：拓扑构建只做一次；采样阶段尽量走连续内存比较、dirty group、并行采样和 SIMD。
4. 文件效率：逻辑信号与物理 storage 分离，多个逻辑视图可以共享同一份底层数据。

## 2. 主要文件

`reflect_macro.h`

业务代码侧的轻量接入头。主要提供：

- `WAVE_REFLECT_FRIEND`：允许反射生成器访问 private/protected 成员。
- `wave::WaveValue<T>`：尺寸保持的可追踪标量封装，赋值时通知 tracer。
- `wave::array<T,N>`：尺寸保持的数组封装，支持元素级 dirty。
- `wave::WavePtr<PtrT>`：显式声明要展开的指针/数组目标。
- `wave::WaveDirtyHook`：业务对象主动通知“我变了”。
- `wave::DirectReflectPointerTarget`：允许指针目标被直接展开的显式基类。
- `wave::DynamicTraceTarget` / `wave::PeekTraceSource`：运行时类型对象和 peek 值源接入。
- `BoolStoragePtr<T>`、`UnionFieldTag`、`UnionFieldBase`：给生成代码和运行时传递特殊字段元信息。

`ReflectGen.cpp`

反射生成器。它基于 libclang 读取业务头文件、工程 include/macro 环境和 Visual Studio 工程配置，识别可反射类型、字段、基类、访问控制、bitfield、union、匿名 union、bool storage typedef 等信息，并输出自动反射头。

输出头中主要包含：

- `wave::ReflectAccess<T>`：访问 T 的字段，必要时访问 private/protected 字段。
- `reflect::is_reflected<T>`：标记 T 是可反射类型。
- `reflect::reflected_visitor<T>`：统一 visitor 入口。
- 动态类型注册函数：让 `DynamicTraceTarget` / `PeekTraceSource` 可以按运行时 type tag 找到展开函数。

`reflect_runtime.h`

偏通用的反射运行时基础层，提供 type tag、visitor 桥接、序列化辅助、递归访问保护等设施。

`wave_runtime.h`

波形追踪运行时核心。负责：

- `Tracer::add_root()` 延迟登记根对象。
- `prepare_topology()` 构建 Node/Track 拓扑。
- `expand_field()` / `expand_member_ptr()` 按类型分发展开。
- 创建 `NodeDecl` / `TrackDecl`。
- 周期采样、变化比较、dirty group、flat leaf fast table、连续内存块比较、并行 worker。
- bitfield/union/storage alias 的逻辑轨道和物理 storage 关系。

`wave_path_wvz4_recorder.h`

把 `wave::Tracer` 输出的拓扑和采样事件接到 WVZ4 writer。它维护 path 到 WVZ4 signal/storage 的映射，并负责 cycle submission。

`wvz4_writer_typed.h`

WVZ4 二进制文件 writer。它处理 layout、WDAT block、压缩、LOD、FOOT、helper 进程通信等。

## 3. 接入流程

### 3.1 业务类型标记

普通 public 结构体可以直接由生成器读取字段。需要追踪 private/protected 字段时，在类体内加入：

```cpp
WAVE_REFLECT_FRIEND
```

对于需要主动 dirty 的对象，可以使用：

```cpp
wave::WaveValue<uint32_t> counter;
wave::array<Slot, 16> slots;
wave::WaveDirtyHook* wave_dirty_hook();
```

对于指针目标，默认不会随便展开普通指针，避免追踪悬空指针或外部对象。要直接展开指针目标，目标类型需要继承：

```cpp
wave::DirectReflectPointerTarget
```

或者业务侧使用 `wave::WavePtr` 显式声明目标和数组长度。

### 3.2 运行 ReflectGen

常见方式：

```bat
ReflectGen input.hpp -o input_reflect_auto.h
```

工程模式：

```bat
ReflectGen --vcxproj model.vcxproj --configuration Release --platform x64
```

批量模式可以从目录、目录列表、白名单头、root class 列表中收集目标类型。生成器会尽量复用工程原始 include 顺序、宏定义和强制 include，避免“单独解析头文件”和真实业务编译环境不一致。

### 3.3 运行时追踪

业务仿真中创建 sink/recorder/tracer，然后登记 root：

```cpp
tracer.add_root("top", &top);
```

`add_root()` 不马上展开对象。真实展开发生在 `sample()` 里的 `prepare_topology()`，这样可以避开 SystemC elaboration 阶段端口还没绑定、peek 还不能访问的问题。

采样时典型流程：

```text
sample(cycle)
  -> prepare_topology(cycle)
      -> refresh_root_watches()
      -> refresh_lazy_value_watches()
      -> export_node_declarations_once()
      -> 建 flat leaf / dirty memory block 表
  -> 收集 dirty 标记
  -> 采样普通 leaf 或 dirty leaf
  -> 输出变化事件
```

## 4. 反射展开分发

运行时展开以 `expand_field(path, parent_id, ptr)` 为入口。它按字段类型进入不同分支：

| 类型 | 处理方式 |
| --- | --- |
| 标量、枚举、bool | 建 leaf track，采样内存值 |
| `std::string` | 建 string-like track |
| C 数组、`std::array` | 建 aggregate node，逐元素展开 |
| `std::pair` | 展开 `first` / `second` |
| `wave::WaveValue<T>` | 建 leaf track，并建立 WaveValue dirty 地址映射 |
| `wave::array<T,N>` | 建 aggregate node，建立元素级 dirty group |
| `wave::WavePtr` | 按声明 size 展开目标对象或数组 |
| 普通指针/智能指针 | 只有目标类型显式继承 `DirectReflectPointerTarget` 才展开 |
| `PeekTraceSource` | 通过 peek 取稳定值地址，再展开值对象 |
| SystemC/VSIP 端口 | 延迟到安全时机，通过端口/peek 读到绑定对象 |
| bitfield | 生成 getter，运行时转成逻辑 alias track |
| union/匿名 union | 建立底层 storage track，成员作为 bit range 逻辑视图 |
| 黑名单 STL/SystemC 内部类型 | 跳过，避免展开第三方内部结构 |

## 5. Node、Track 和 Storage

反射展开产出两类声明。

`NodeDecl` 表示树结构：

```text
top
  orders
    metric_0002
```

`TrackDecl` 表示真正可采样的信号。普通信号通常是：

```text
track_id == storage_id
```

bitfield、union、匿名 union、多逻辑视图等场景下：

```text
多个逻辑 track_id -> 同一个 storage_id + 不同 bit_offset/bit_width
```

这样 WVZ4 文件只保存一份物理 storage 数据，Viewer 再按逻辑信号的位范围切片显示。

## 6. bitfield 机制

C++ bitfield 不能取地址，所以生成器不会把 bitfield 当普通字段指针传给运行时。它会生成 getter，并记录：

- 字段名
- bit width
- bit offset
- getter 函数

运行时优先选择覆盖该 bitfield 的底层字节范围作为物理 storage track。逻辑 bitfield track 只保存：

```text
storage_id + bit_offset + bit_width
```

如果无法安全选择底层 storage 范围，则退回 getter track。

## 7. union 和匿名 union

生成器识别 union 字段时，会给 visitor 额外传递：

- `UnionFieldTag`
- union storage bytes
- 可选的 union base 指针

运行时展开 union 成员时建立 `UnionAliasContext`。如果成员是标量 leaf，它不会为每个 union 成员重复保存物理数据，而是：

1. 找到 union 底层 storage 范围。
2. 建或复用 storage-only track。
3. 为每个成员建逻辑 alias track。

匿名 union 的字段也按同样逻辑处理。重点是逻辑字段名仍然可见，但底层数据不重复写。

## 8. BoolStorage

业务里常见 `typedef unsigned char U01`，物理上是一字节，语义上是一位 bool。生成器可以把这类字段转成：

```cpp
wave::as_bool_storage_ptr(&obj->flag)
```

运行时建出的 track 是 1-bit Bool，但采样地址仍然指向原始字节地址。这样既保持业务语义，又不破坏连续内存比较。

## 9. peek、动态类型和 sc_port

`PeekTraceSource` 适合端口、FIFO、channel 这类“值不直接在 wrapper 里，而是通过 peek 暴露”的对象。

流程是：

```text
端口或 channel
  -> wave_trace_peek_ptr()
  -> wave_trace_peek_type_tag()
  -> 找动态 expander
  -> 展开 peek 返回的值对象
```

动态 expander 来自：

- 生成反射头里的自动注册。
- 或手动调用 `wave::ensure_dynamic_type_registered<T>()`。

SystemC/VSIP 风格端口不会在 `add_root()` 里立即解引用。运行时在拓扑准备阶段处理端口，保证绑定完成后再访问。对于已稳定返回地址的 peek 值源，系统会把它展开成普通 leaf track，后续采样不必每 cycle 调 peek。

## 10. dirty 机制

全量扫描百万级信号很贵，所以系统支持主动 dirty：

1. `WaveValue<T>`：赋值时通知地址。
2. `wave::array<T,N>`：元素访问/写入时通知元素地址。
3. `WaveDirtyHook`：业务对象主动调用 `mark_dirty()`。
4. dirty peek group：端口或 channel 暴露的值源可按底层地址分组。

运行时会把 dirty group 映射到 leaf 范围。采样时如果一个 dirty-safe group 没被标记，就可以跳过该组的叶子。对于连续内存区域，还会先做 memory block shadow 比较，整块没变就跳过内部 leaf。

## 11. 大规模重复对象的拓扑构建优化

当前主路径已经撤回“同类型反射拓扑缓存”。也就是说，反射类型仍按原有 visitor 逐实例展开，不再保存字段偏移模板并重放。这样路径更直观，也避免复杂字段、动态指针、union/bitfield 组合下的缓存条件判断。

现在保留的是两类更低风险的优化：

### 11.1 节点局部名池

`NodeDesc` 仍然只保存 `name_id`，但 `name_id` 指向共享的局部名表：

```text
node.name_id -> node_names_[id]
```

对于大量同类型对象：

```text
slots[0].active
slots[1].active
slots[2].active
...
```

字段名 `active` 只保存一次。`counter`、`tag`、`payload`、`u32` 等重复字段名同理。数组下标如 `[0]`、`[1]` 本身每个都不同，仍按独立名字保存，避免 hash 表对一次性名字做无意义查找。

该优化只影响名字存储，不改变：

- NodeId
- TrackId
- 父子关系
- NodeDecl/TrackDecl 内容
- 采样地址
- storage alias
- dirty/peek 分组

### 11.2 WavePtr 大数组批量预留

`WavePtr<T*>` 展开大数组前会按元素数量预留拓扑容器容量：

```text
nodes_
tracks_
track_runtime_
all_track_ids_
parallel_track_ids_
objects_
object_id_by_key_
bitfield_storage_by_key_
bitfield_storage_created_keys_
```

这不是改变创建顺序的“批量填表”，而是先做容量准备，再沿用原来的逐元素展开逻辑。因此输出 id 顺序和行为保持不变，但能减少 `vector` 扩容、hash rehash 和内存搬移。

### 11.3 性能效果边界

该优化减少的是拓扑构建期成本：

- 少保存重复字段名。
- 少做大数组展开过程中的容器增长。
- 少做 bitfield/raw storage map 的 rehash。

它不会减少这些必要工作：

- 每个实例仍要创建自己的 Node/Track。
- 每个实例仍有自己的采样地址和 dirty group。
- 反射 visitor 仍按实例执行。

典型收益场景是：

- 大数组里有大量相同结构体元素。
- FIFO slot、cache line、queue entry、订单/事务结构等重复业务单元。
- bitfield/union 字段较多，storage alias map 压力较大的结构。

### 11.4 验证

`smoke_bool_storage.cpp` 已加入专门验收：

- `Top` 中包含 `Slot slots[8]`。
- `reflected_visitor<Slot>::visit()` 内部计数。
- 第一次 sample 后要求 `Slot` visitor 调用 8 次，证明同类型拓扑缓存已经撤回。
- 同时验证 `top.slots.[3].count` 的声明和后续采样事件正确。

`smoke_reflect_topology_cache_stress.cpp` 现在作为大规模一致性压测使用：同一批重复 `Slot` 对象分别关闭/开启“节点名池 + WavePtr 批量预留”，逐条比较 `NodeDecl`、`TrackDecl` 和 `TrackEvent`，要求完全一致。

## 12. WaveTraceLevel

环境变量：

```text
WaveTraceLevel=<正整数>
```

含义是限制信号路径中 `.` 的最大数量。未设置或非法值表示不限制。

用途：

- 大模型接入早期降低追踪量。
- 避免意外把过深对象树全展开。
- 在不改业务代码的情况下控制波形规模。

注意：名字池和批量预留不绕过 WaveTraceLevel。是否展开仍由原始路径和原有展开流程决定。

## 13. 与 WVZ4 writer 的关系

反射系统只负责产生拓扑和变化事件。WVZ4 writer 负责文件格式和压缩。

连接点是：

```text
Tracer
  -> NodeDecl / TrackDecl / TrackEvent
  -> wave_path_wvz4_recorder
  -> wvz4::Writer
```

反射系统里的 storage alias 会直接影响 WVZ4 layout：

- 普通信号：一个逻辑 signal 一个 storage。
- bitfield/union：多个逻辑 signal 指向同一个 storage。
- Viewer 按 `storage_id + bit_offset + bit_width` 还原逻辑值。

## 14. 抗 crash / kill 写文件路径

抗 crash/kill 主要在 writer/helper 层，不在反射展开层。

默认思路：

```text
主仿真进程
  -> 每 cycle 把变化提交给 helper
  -> helper 独立拥有 WVZ4 writer
  -> helper 监控父进程 handle 和 pipe
  -> 父进程正常退出或被 kill 时，helper finalize 已完整收到的 cycle
```

反射系统与该机制的关系是：

- 反射产生稳定 layout 和 cycle event。
- recorder 把它们转成 helper frame。
- helper 只保证已收到的完整 frame 可落盘。

## 15. 已验证测试

本次改动后已在本机执行：

```bat
msbuild smoke_compile.vcxproj /p:Configuration=Release /p:Platform=x64 /m
msbuild smoke_markers_pointer_dirty.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_markers_pointer_dirty\Release\smoke_markers_pointer_dirty.exe
msbuild smoke_bool_storage.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_bool_storage\Release\smoke_bool_storage.exe
msbuild smoke_tap_bool_storage.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_tap_bool_storage\Release\smoke_tap_bool_storage.exe
msbuild smoke_storage_alias_writer.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_storage_alias_writer\Release\smoke_storage_alias_writer.exe
msbuild smoke_bitfield_track_writer.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_bitfield_track_writer\Release\smoke_bitfield_track_writer.exe %TEMP%\bitfield_track_reflect_check.wvz4
msbuild smoke_reflect_topology_cache_stress.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_reflect_topology_cache_stress\Release\smoke_reflect_topology_cache_stress.exe 100000 1
msbuild smoke_systemc_wvz4.vcxproj /p:Configuration=Release /p:Platform=x64 /m
build_vs\smoke_systemc_wvz4\Release\smoke_systemc_wvz4.exe
```

关键结果：

- `smoke_markers_pointer_dirty`：通过，覆盖 private 反射、DirectReflectPointerTarget、WavePtr、peek、VSIP/SystemC 风格端口、WaveValue、wave::array、BoolStorage。
- `smoke_bool_storage`：通过，且验证 `Slot` 类型 visitor 调用 8 次，证明同类型拓扑缓存已撤回；同时覆盖 bool storage、C 数组嵌套反射和更新事件。
- `smoke_reflect_topology_cache_stress`：通过。它用同一批重复 `Slot` 对象分别关闭/开启节点名池和 WavePtr 批量预留，逐条比较 `NodeDecl`、`TrackDecl` 和 `TrackEvent`，要求完全一致；同时输出 baseline/optimized 耗时。
- `smoke_systemc_wvz4`：通过，输出 `systemc_wvz4_ok cycles=32`。
- storage alias 和 bitfield writer smoke 均能生成文件。

构建日志中部分项目仍有历史链接警告，例如 `LNK4098` 和 `LNK4286`，与本次反射拓扑构建优化无关。

## 16. 接手时重点看哪里

如果要继续优化反射展开：

1. 先看 `wave_runtime.h` 中 `expand_reflected_field_direct()` 和 `expand_field()` 的反射分支。
2. 再看 `node_name_id_for_path_()`、`intern_node_name_()`、`reserve_wave_ptr_array_expansion_()`。
3. 确认任何新优化都不改变 NodeId/TrackId 顺序和声明内容。
4. 新增类型支持后跑 `smoke_markers_pointer_dirty`、`smoke_bool_storage` 和大规模一致性压测。

如果要改生成器：

1. 看 `ReflectGen.cpp` 中输出 `ReflectAccess<T>::visit()` 的代码段。
2. 注意 pointer 字段最好传“指针存储地址”，这样运行时能读取每个实例的当前指针值。
3. bitfield 仍应走 getter，不要试图对 bitfield 取地址。
4. union 字段必须继续传 storage bytes 和 union base。

如果要查采样性能：

1. 看 `ensure_flat_leaf_fast_table()` 和 flat memory block 相关逻辑。
2. 看 dirty peek / dirty wave value / dirty wave array 的 group、range、leaf 表。
3. 节点名池和 WavePtr 批量预留只影响建拓扑，不直接影响每 cycle 采样热路径。

## 17. 当前边界

- 当前没有同类型反射拓扑缓存；反射 visitor 仍按实例执行。
- 节点名池只复用局部名字，不复用 Node/Track。
- 动态指针目标、运行时绑定端口、peek 值源仍按每个实例的当前地址处理。
- WavePtr 批量预留只预留容量，不改变拓扑创建顺序。
- WaveTraceLevel、union 开关、bitfield 开关仍在最终展开时生效。
- 改动不要求重新生成已有反射头；运行时层即可生效。
