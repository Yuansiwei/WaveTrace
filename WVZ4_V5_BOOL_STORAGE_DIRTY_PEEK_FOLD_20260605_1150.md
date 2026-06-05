# WVZ4 v5 bool-storage + dirty-peek storage folding 收敛版

时间：2026-06-05 11:50

## 本版目标

本版合并前面确认的全部需求：

1. value mask 策略继续保持：`byte_width_bytes <= 2` 不走 mask；3/4 字节走 packed nibble mask；8 字节继续走 byte mask。
2. 业务 typedef `U01` 按字段声明处 typedef spelling 识别为 logical Bool，而不是按 canonical `unsigned char` 判断。
3. `U01` 不走 getter；生成器只标记 `as_bool_storage_ptr()`，runtime 保留 raw storage address，因此 flat/memory-block/dirty-peek memory-block 仍可以比较原始存储字节。
4. dirty peek 做真正意义的物理存储折叠：路径和 logical signal 保留，但相同 physical storage stream 只写一份 WDAT。
5. 允许破坏旧格式兼容性：`kFormatVersion` 提升到 5，`SIGT` 增加 `storage_id` 字段。

## 涉及文件

修改文件：

- `reflect_macro.h`
- `ReflectGen.cpp`
- `wave_runtime.h`
- `wave_path_wvz4_recorder.h`
- `wvz4_writer_typed.h`

新增验证文件：

- `smoke_bool_storage.cpp`
- `smoke_storage_alias_writer.cpp`
- `smoke_tap_bool_storage.cpp`

## U01 bool-storage 规则

`ReflectGen.cpp` 现在默认把字段声明类型拼写为 `U01` 的字段生成为：

```cpp
on_ptr("flag", ::wave::as_bool_storage_ptr(std::addressof(obj->flag)));
```

而不是：

```cpp
on_ptr("flag", std::addressof(obj->flag));
```

识别依据是 `f.typeName`，不是 `f.canonicalTypeName`。因此：

```cpp
typedef unsigned char U01;
typedef unsigned char U08;
typedef unsigned char BYTE;

struct Foo {
    U01  valid;  // logical Bool
    U08  data;   // normal U8
    BYTE raw;    // normal U8
};
```

只会把 `valid` 标成 bool-storage，不会把所有 `unsigned char` 别名误伤。

额外白名单可通过生成器参数增加：

```bash
--bool-storage-typedef BOOL8
--bool-storage-typedef ns::U01
```

## runtime 采样语义

新增：

```cpp
wave::BoolStoragePtr<T>
wave::as_bool_storage_ptr(const T*)
```

runtime 创建 Bool track 时保存：

- logical kind：`ValueKind::Bool`
- logical bit width：`1`
- scalar sample kind：`ScalarSampleKind::Bool`
- raw memory address：原始 `U01*`
- raw memory byte width：`sizeof(U01)`
- transform：`raw != 0`

因此采样时：

```cpp
bool value = (*raw_ptr != 0);
```

但 memory-block 预检查仍比较原始 byte。`1 -> 2` 会唤醒 block，但 logical bool 不变，不会 emit 波形变化。

## dirty peek storage folding

`TrackDecl` 新增：

```cpp
TrackId storage_id;
```

含义：

- `track_id`：logical signal id，viewer 显示路径用。
- `storage_id`：physical storage stream id，WDAT 写入用。

普通信号：

```text
track_id == storage_id
```

dirty peek alias 信号：

```text
多个 logical track_id -> 同一个 storage_id
```

runtime 在 dirty peek scalar leaf 建 track 时，按以下 key 做 storage folding：

```text
raw memory address
scalar_reader / transform
scalar_kind
value_kind
logical bit_width
raw memory byte_width
```

这避免了同一地址被不同语义解释时错误折叠，例如同一 raw byte 一条路径按 U8，另一条路径按 BoolStorage。

## WVZ4 v5 layout 变化

`wvz4_writer_typed.h`：

```cpp
static const u32 kFormatVersion = 5;
```

`SignalDefinition` 新增：

```cpp
u32 storage_id;
```

SIGT 序列化顺序从旧版：

```text
signal_id, node_id, type, bit_width, radix
```

变为新版：

```text
signal_id, storage_id, node_id, type, bit_width, radix
```

WDAT 的 update id 语义变为 physical storage stream id。旧 reader/viewer 必须同步修改，否则不能读新版文件。

## viewer 需要同步的读取逻辑

新版 viewer 应按：

```text
logical signal_id -> storage_id -> WDAT stream
```

加载波形。树、路径、搜索仍然按 logical signal/node 构建；数据流按 `storage_id` 复用。

多个 signal 指向同一个 `storage_id` 时，viewer 可以正常显示多条路径，但它们底层复用同一份 decoded waveform。

## 验证命令

已在当前环境通过：

```bash
g++ -std=c++14 -DWVZ4_NO_ZSTD -pthread smoke_compile.cpp -o /tmp/smoke_compile_fold

g++ -std=c++14 -DWVZ4_NO_ZSTD -pthread smoke_backlog.cpp -o /tmp/smoke_backlog_fold

g++ -std=c++14 -DWVZ4_NO_ZSTD -pthread wvz4_writer_monitor_main.cpp -o /tmp/wvz4_writer_monitor_fold

g++ -std=c++14 -I. -DWVZ4_NO_ZSTD -pthread smoke_bool_storage.cpp -o /tmp/smoke_bool_storage

g++ -std=c++14 -I. -DWVZ4_NO_ZSTD -pthread smoke_storage_alias_writer.cpp -o /tmp/smoke_storage_alias_writer

g++ -std=c++14 -I. -DWVZ4_NO_ZSTD -pthread smoke_tap_bool_storage.cpp -o /tmp/smoke_tap_bool_storage
```

说明：当前容器缺少 `clang-c/Index.h`，所以这里只能确认 `ReflectGen.cpp` 变更逻辑，不能在本容器直接编译生成器本体。业务环境有 libclang 时需要再编一次 `ReflectGen.cpp`。

## 注意事项

1. `U01` bool-storage 是有损语义：`0 -> false`，任何非 0 -> `true`。
2. 不要按 canonical `unsigned char` 做 bool 化，否则会误伤 `U08/BYTE/unsigned char`。
3. 旧版 WVZ4 reader/viewer 不能读取 v5 SIGT，需要同步支持 `storage_id`。
4. 当前只对 dirty peek 做真正 storage folding；普通 flat leaf 仍默认路径独立，除非后续也显式引入全局 storage folding。
