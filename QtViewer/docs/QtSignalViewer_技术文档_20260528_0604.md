# QtSignalViewer / WVZ4 波形系统技术文档

**版本包：** `20260528_0604`  
**代码基线：** `QtSignalViewer_stringless_logic_bugcheck2_20260527_2015.zip`  
**目标环境：** Visual Studio x64 + Qt5  
**核心范围：** 波形写入、WVZ4 v3 文件格式、波形解析、Qt 查看器、双文件对比、性能优化  

## 1. 系统总体作用

本波形系统用于在仿真过程中记录周期级信号数据，并在独立查看器中完成波形加载、浏览、搜索、选中、显示、跳转、区间选择和双文件差异对比。系统面向大规模信号数量和长时间仿真数据，重点解决传统波形文件在大树结构、大量信号、稀疏变化、按需查看和仿真异常中断场景下的性能与可恢复性问题。

整体目标可以概括为：**用稳定拓扑描述信号树，用分块压缩保存波形数据，用按需加载降低查看成本，用对比模式快速定位两个仿真结果之间的差异。**

## 2. 主要组成

| 模块 | 关键文件 | 作用 |
|---|---|---|
| WVZ4 写入器 | `wvz4_writer_typed.h` | 负责稳定拓扑、typed value、v3 signal chunk 写入和 WAL 支持。 |
| 稳定路径记录器 | `wave_path_wvz4_recorder.h` | 将反射系统中的节点和采样事件转换为 WVZ4 layout 与 cycle submission。 |
| WVZ4 解析器 | `WaveParser4.cpp/.h` | 读取 NAME/NODE/SIGT/WDAT/FOOT，支持 v1/v2/v3。 |
| 通用数据结构 | `WaveTypes.h` | 定义信号、样本、树节点、差异区间、rawBits 缓存等。 |
| 主窗口逻辑 | `MainWindow.cpp/.h` | 文件打开、树模型、搜索、按需加载、比较模式、按钮和工具栏。 |
| 波形绘制 | `WaveCanvas.cpp/.h` | 绘制波形、红色差异区、时间坐标、光标、拖拽时间范围。 |
| 选中信号区 | `ActiveSignalListWidget.cpp/.h` | 管理已选信号、当前值、分隔线拖动、复制和多选。 |
| 旧格式兼容 | `WaveParser.cpp/.h`, `WaveParser2.cpp/.h`, `WaveParser3.cpp/.h` | 兼容 JSON/WVZ/WVZ2/WVZ3。 |

## 3. 总体架构

系统分为四层：

1. **采集层**：业务仿真或反射系统输出节点声明、信号声明和每周期采样值。
2. **写入层**：将稳定拓扑和周期采样写入 WVZ4 文件，支持压缩、分块和 WAL。
3. **解析层**：读取文件头、layout section、footer index 和 WDAT tile，并按需返回样本。
4. **显示层**：Qt 查看器以树模型、选中信号列表和波形画布形式展示数据。

核心设计原则是：**拓扑先行、样本按需、字符串延迟、差异显式、绘制只看可见区域。**

## 4. WVZ4 v3 文件格式

### 4.1 文件头

WVZ4 v3 文件头为固定长度，包含以下关键信息：

- magic 和 version；
- header size；
- target block span；
- footer offset；
- signals_per_chunk；
- feature flags；
- 保留字段。

v3 写入器在 header 中写入 `signals_per_chunk` 和 signal chunking feature flag，用于让 reader 理解后续 WDAT tile 的组织方式。

### 4.2 Layout Section

layout section 描述静态信号树和信号定义：

| 原始 section | 压缩 section | 内容 |
|---|---|---|
| `NAME` | `NAMZ` | name_id 到名字字符串的映射。 |
| `NODE` | `NODZ` | node_id、parent_id、first_child、next_sibling 等树结构。 |
| `SIGT` | `SIGZ` | signal_id、node_id、value type、bit width、radix。 |

当压缩后有收益时，writer 会写 `NAMZ/NODZ/SIGZ`。parser 同时支持压缩和非压缩 layout。

### 4.3 NODE 树结构

WVZ4 的树不是通过 full path 字符串重建，而是由 `NODE` section 直接给出：

```text
node_id
parent_id
name_id
kind
first_child
next_sibling
```

viewer 不再扫描 full path 来建树，也不再使用 `QTreeWidgetItem` 复制整棵树。当前实现使用 `QTreeView + QAbstractItemModel`，其中 `QModelIndex::internalId()` 直接对应 `node_id`。

### 4.4 Signal Table

`SIGT` 将 `signal_id` 绑定到 `SignalLeaf node_id`，并指定显示和解码所需的 type、bit width、radix。WVZ4 当前面向 64 bit 以内的 scalar value，不处理动态拓扑和 Z 状态。

### 4.5 WDAT v3 外层结构

v3 将波形数据拆成 **时间块 × 信号块** 的二维 tile。每个 WDAT section 的外层 payload 顺序为：

```text
block_id
start_cycle
end_cycle
signal_chunk_id
first_signal_id
signal_count
compression
raw_size
encoded_size
encoded_payload
```

这样 parser 在按需加载某个 signal 时，可以先判断该 signal 是否属于当前 tile。如果不属于，直接 seek 跳过 encoded payload，不读取、不解压。

### 4.6 WDAT v3 raw tile

解压后的 raw tile 结构为：

```text
block_id
start_cycle
end_cycle
flags | kWdatSignalChunkTile
signal_chunk_id
first_signal_id
signal_count
[shared time table]
offset_count = signal_count + 1
delta-coded offsets
records_blob_size
records_blob
```

每个 signal 的记录范围由 offset table 定位：

```text
local = signal_id - first_signal_id
record = records_blob[offset[local] ... offset[local + 1])
```

空区间表示该 signal 在当前 tile 中没有 transition。

### 4.7 FOOT v3

footer 保存 tile 索引，字段顺序为：

```text
block_id
start_cycle
end_cycle
signal_chunk_id
first_signal_id
signal_count
file_offset
file_size
raw_size
compression
```

parser 可以通过 header 中的 `footer_offset` 直接定位 FOOT，再用 FOOT 中的 tile index 随机读取相关 WDAT section。

## 5. Writer 与 Recorder 设计

### 5.1 typed writer

`wvz4_writer_typed.h` 是核心写入器，负责：

- 校验 layout；
- 写文件头和 layout section；
- 根据 `target_block_span` 切分时间块；
- 根据 `signals_per_chunk` 切分 signal chunk；
- 在每个 tile 内写 offset table 与 records blob；
- 在 delta-time 和 shared-time 编码之间选择更小结果；
- 对 layout 和 WDAT 进行 zstd 压缩；
- 关闭时写 FOOT 并回填 footer offset。

### 5.2 signal chunking

signal chunking 的目的不是减少总数据量，而是减少查看某个信号时需要读取和解压的无关数据。默认 `signals_per_chunk = 1024`。某个 signal 的 chunk 号为：

```text
signal_chunk_id = (signal_id - 1) / signals_per_chunk
```

这种设计比“一个 signal 一个压缩块”更平衡：footer 不会过大，压缩率不会严重下降，多信号加载也能复用同一个 chunk。

### 5.3 隐式 0 初值

WVZ4 v2+ 规定所有信号在 cycle 0 有隐式全 0 初值。writer 遇到第一次提交值为 0 的情况不会写 transition；parser 对已加载信号补充隐式 0 sample。如果文件中存在显式 cycle 0 非 0 sample，则显式值覆盖隐式值。

### 5.4 WAL/monitor finalization

WAL 方案允许仿真主进程写 spool 文件，由独立 monitor/finalizer 进程在仿真退出或被 kill 后重放已提交记录并生成最终 WVZ4。该能力只能恢复到最后一个 committed WAL record，不能恢复仍在主进程内存或未完成 cycle 中的数据。

## 6. Parser 设计

### 6.1 加载流程

`WaveParser4` 的主要加载步骤：

1. 读取并校验 WVZ4 header；
2. 解析 layout section：NAME/NODE/SIGT 或 NAMZ/NODZ/SIGZ；
3. 构造全量 signal definition 与 tree metadata；
4. 如果存在 FOOT，解析 tile index；
5. 根据加载模式选择：只加载部分 signal，或为比较模式加载全部 signal；
6. 对 WVZ4 v3 优先使用 FOOT 随机读取相关 WDAT tile；
7. 对样本执行 compact 和 implicit-zero 处理；
8. 构建 changeTimes 等派生缓存。

### 6.2 严格校验

parser 会校验：

- section size 不越界；
- NODE 引用的 name_id、parent_id、first_child、next_sibling 是否有效；
- child sibling chain 是否完整并且无环；
- SIGT 是否绑定到 SignalLeaf；
- bit width 与 ValueType 是否匹配；
- WDAT outer header 与 raw tile header 是否一致；
- FOOT 记录与实际 WDAT section 是否一致；
- raw payload 是否存在 trailing bytes。

### 6.3 按需加载优化

对于 WVZ4 v3，按需加载不再顺序扫描全文件 WDAT。当前策略为：

1. 读取 FOOT tile index；
2. 用目标 signal_id 判断相关 signal chunk；
3. 用时间窗口判断相关 time block；
4. 只 seek 到相关 WDAT section；
5. 只读取并解压相关 encoded payload；
6. 用 offset table 直接定位目标 signal record。

## 7. 样本数据结构与去字符串化优化

### 7.1 原始问题

旧实现中，WVZ4 解码每个 sample 时会构造 QString，例如十六进制字符串、补零字符串、大写转换等。在大文件比较或全量加载时，这会导致 `rawHexText`、`QString::number`、`QString::rightJustified`、`QString::toUpper` 成为热点。

### 7.2 当前方案

WVZ4 hot path 不再为每个 sample 构造文本值，而是写入：

```text
sample.rawBits
sample.rawFieldsReady = true
sample.value.clear()
```

只有在显示、复制、导出或旧格式 fallback 时才格式化文本。

### 7.3 兼容旧格式

旧格式可能只提供文本值，因此比较和显示逻辑必须区分：

- 双方都有 rawFieldsReady：比较 rawBits；
- 双方都没有 rawFieldsReady：比较原始 value 文本；
- 一方 raw、一方 text：保守认为不同，必要时再 hydrate。

## 8. Qt Viewer 架构

### 8.1 主窗口

`MainWindow` 负责：

- 文件打开与格式分派；
- WVZ4 按需加载；
- 比较两个波形文件；
- 构造树模型；
- 管理 active signal list；
- 与 `WaveCanvas` 进行 cursor、range、selection、jump 联动。

### 8.2 信号树模型

候选信号树使用 `QTreeView + QAbstractItemModel`，避免 `QTreeWidgetItem` 大量对象和 dummy child。模型直接引用内部 node array：

```text
WVZ4 node_id = SignalLogicTree::nodes[node_id] = QModelIndex::internalId()
```

搜索结果保留树结构，不再平铺。搜索支持模块名、信号名和位宽文本，例如 `[7:0]`。

### 8.3 选中信号区

选中信号区支持：

- Ctrl 多选；
- 当前值显示；
- 慢双击进入文本选择模式；
- Ctrl+C 复制信号名和值；
- 信号名和值之间的分界线拖动；
- 长信号名优先显示右侧部分；
- 只刷新可见行，避免大量 active 信号时卡顿。

### 8.4 波形画布

`WaveCanvas` 支持：

- 波形绘制；
- bus/bit 信号显示；
- 时间坐标和光标；
- Ctrl 多选；
- 在时间轴和波形区拖动选择时间范围；
- 跳转下一次/上一次变化；
- Ctrl 跳转比较红色差异区；
- 比较模式下红色 diff region 背景；
- bus 变化处先画横向遮盖线，再画两条斜线。

## 9. 双文件比较功能

### 9.1 入口

工具栏新增“比较两个波形文件”按钮和图标。用户一次选择两个 waveform 文件后，viewer 进入比较模式。

### 9.2 筛选逻辑

比较逻辑分两步：

1. 按完整路径匹配，只保留两边路径相同的信号；
2. 对同路径信号计算 value state，只保留任意 cycle 存在差异的信号。

冗余 sample 但值不变不会被误判为差异。

### 9.3 树形显示

比较结果不是两棵独立树，而是在共同路径下让 A/B 叶子相邻：

```text
top
  module
    A data[7:0]
    B data[7:0]
```

这样便于直接对比同一信号的两侧结果。

### 9.4 红色差异区

比较时会生成 `diffRegions`。画布绘制对应时间区间的红色背景。文件长度不同导致的尾部缺失也会被视为差异区。

## 10. 性能优化总结

| 问题 | 优化方式 |
|---|---|
| 大树构建慢 | 不再 O(N²) 扫 parentId，直接使用 NODE child chain。 |
| QTreeWidget item 多 | 改为 QTreeView + QAbstractItemModel。 |
| 搜索后展开卡 | 不使用 expandAll，只展开命中祖先路径。 |
| 选中信号加载慢 | v3 使用 FOOT index 只读取相关 signal chunk tile。 |
| WDAT 重复解压 | 通过 signal chunk 减少不相关 tile 解压。 |
| 每 sample 字符串构造 | WVZ4 解析只保存 rawBits，显示时再格式化。 |
| QHash/QSet 热点 | dense signal_id 映射改用 QVector direct map。 |
| 跳转变化点慢 | 每个 signal 构建 changeTimes，二分查找。 |
| 红色 diff 绘制慢 | 使用二分定位可见 diff region。 |
| active value 刷新慢 | 只刷新可见行。 |

## 11. Qt5 兼容性处理

为了兼容用户当前 Qt5 环境，代码中注意：

- Qt5 下不编译 `QMouseEvent::position()`；
- 不使用 `globalPosition()`；
- 不依赖 `QDataStream::Qt_5_15`；
- 避免使用直接 `signals:`、`slots:`、`emit` token；
- 增加必要 Qt 头文件，避免依赖间接 include；
- 修改 header 后建议 clean rebuild，确保 moc 重新生成。

## 12. 使用流程

### 12.1 打开单个波形

1. 启动 QtSignalViewer；
2. 打开 WVZ/WVZ2/WVZ3/WVZ4 文件；
3. 在信号树中浏览或搜索；
4. 双击或拖拽加入选中信号区；
5. 使用时间缩放、光标、范围选择和跳转功能分析波形。

### 12.2 比较两个波形

1. 点击比较按钮；
2. 选择两个波形文件；
3. 等待比较完成；
4. 查看共同路径下相邻的 A/B 信号；
5. 观察红色差异区；
6. 按 Ctrl 使用变化跳转按钮只跳转红色差异区域。

## 13. 测试清单

建议本地验证以下场景：

- WVZ4 v3 正常打开；
- 压缩 layout：NAMZ/NODZ/SIGZ 正常解析；
- v3 FOOT index 能跳过无关 WDAT tile；
- 隐式 0 初值显示正确；
- 搜索 `[7:0]` 能命中位宽；
- 搜索出的模块可以展开并显示成员；
- 比较结果只保留有差异的同路径信号；
- 红色差异区间准确；
- Ctrl 多选与多信号跳转正确；
- active list 分隔线拖动正常；
- Qt5 clean rebuild 无编译错误。

## 14. 已知边界与约束

- WVZ4 当前面向稳定拓扑，不处理运行期动态增删信号；
- WVZ4 writer 当前不支持 Z/high-impedance；
- 单个 scalar value 限制在 64 bit 以内；
- monitor 只能恢复已提交 WAL record；
- 如果主线程 crash 但进程仍存活，目前不处理；
- 旧格式仍可能依赖文本 value，需要保留 fallback 逻辑。

## 15. 后续建议

1. 将 WVZ4 样本从 `QVector<WaveSample>` 进一步改为 `times[] + rawBits[]` 双数组；
2. 增加有界 tile cache，避免同一 signal chunk 反复读取；
3. 建立 WVZ4 v1/v2/v3 自动回归测试集；
4. 增加大规模 synthetic benchmark；
5. 将长时间比较任务移动到后台线程；
6. 增加比较结果导出报告；
7. 进一步减少旧格式路径中的字符串解析频率。

## 16. 包内容说明

本次交付包包含：

- `source/`：最新完整工程代码；
- `docs/QtSignalViewer_技术文档_20260528_0604.md`：中文 Markdown 技术文档；
- `docs/QtSignalViewer_技术文档_20260528_0604.docx`：中文 Word 技术文档；
- `docs/QtSignalViewer_Technical_Documentation_20260528_0604.md/docx`：英文版技术文档；
- `docs/PACKAGE_NOTES_20260528_0604.md`：打包说明与本地验证提示。

## 17. 本地构建建议

由于当前容器没有 Qt/Visual Studio 编译环境，代码已做静态检查和文档整理，但最终仍需要在本地执行：

```text
Clean Solution
Rebuild Solution
```

重点检查 Qt moc、资源文件、WVZ4 v3 文件打开、比较模式和大文件性能。
