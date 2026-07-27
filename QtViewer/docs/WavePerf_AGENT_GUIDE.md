# WavePerf Agent 维护指南

本文面向继续维护 WavePerf 的工程 Agent。目标不是重复用户手册，而是说明：

- 数据从 WVZ4 到结论的完整链路；
- 每项指标的真实分子、分母和覆盖条件；
- 普通数组、拍平数组和截断选择的差异；
- 哪些证据只能作为局部风险，哪些证据可以升级为一级瓶颈；
- 修改代码时最容易引入的静默错误；
- 完成修改前必须执行的回归。

用户侧命令和页面说明见 [`WavePerf.md`](WavePerf.md)。

---

## 1. 维护目标

WavePerf 是 WVZ4 v15 的离线 GPU 波形性能分析器。它不尝试恢复完整 C++ 对象，
而是从架构路径和关键字段中提取可验证事实，再递归形成：

1. 每个 issue slot 的发射事实；
2. 每个 SG 的驻留、队列、局部 Eligible 和阻塞事实；
3. 每个 QPPU 的局部诊断；
4. QPPU、PPU、DPPU、Cluster、GPU 的架构树聚合；
5. FIFO、Queue、Credit、Cache、L1/L2 和 Thread 指标；
6. 工作负载范围与一级、二级结论；
7. 同源的 `data.json` 与离线 `index.html`。

正确性优先级如下：

```text
覆盖是否可信
  > 原始计数是否正确
  > 局部归因是否成立
  > 全局结论是否成立
  > 页面是否好看
```

任何未知数据都必须显式保留。一个“部分覆盖”的正确答案，优于一个数值完整但
分母错误的答案。

---

## 2. 代码地图

### `WavePerf.cpp`

主程序与事实层：

- 命令行参数和业务周期范围；
- 性能候选信号预筛选；
- 正式信号选择和 `--max-signals` 截断检测；
- WVZ4 按需解码与进度条；
- 变化记录的区间积分；
- 事件语义积分；
- issue slot、指令类别、访存类别和 Thread mask 事实；
- FIFO/Queue/Credit 资源压力；
- 组装统一 JSON 模型；
- 调用各子分析器和自测。

### `WavePerfArchitecture.cpp/.h`

架构分类与递归聚合：

- 路径到 `key/category/helper/eventSemantics` 的正式分类；
- GPU、Cluster、DPPU、PPU、QPPU 和关键 class 的树；
- 普通数组与拍平数组的表示；
- issue、FIFO、Queue、Cache、EU、Credit 等模块聚合；
- 事件配对覆盖和 class 汇总；
- 分类器与架构聚合自测。

### `WavePerfScheduler.cpp/.h`

QPPU/SG 调度器：

- 识别 SG 表、指令队列、issue slot、依赖向量和功能单元 pending；
- 按变化边界积分 SG 状态；
- 计算 Active、Queue Ready、局部 Eligible 和实际 issued；
- 生成阻塞原因、PC 热点与时间线分桶；
- 发布每 SG、每 QPPU 和全局调度覆盖。

### `WavePerfBandwidth.cpp/.h`

内存数据通路：

- L1/L2 有效字节积分；
- lane、mask、握手和峰值覆盖；
- DLS-L1 请求/返回延迟；
- outstanding、P50/P95、覆盖和置信度；
- 带宽与延迟自测。

### `WavePerfCBCtrl.cpp/.h`

CBCtrl/CBData 数据通路：

- 识别 CBCtrl、PPUSData 和 CBData 的真实 FIFO 端点并消除层级镜像；
- 依据读写事件前后的指针语义提取 payload；
- 关联请求、首 uop、EU 返回、CBData 下发/写回和 Pending 清除；
- 按 QPPU、客户端、SG 与 PC 聚合服务率、延迟、公平性和 ChkDep 清除；
- 发布各阶段覆盖、匹配率和保守瓶颈结论；
- 连续事件、payload 边界和截断降级自测。

### `WavePerfDiagnosis.cpp/.h`

结论层：

- 每 QPPU 二级结论；
- 实际参与 QPPU 范围；
- workload regime；
- 全局 findings；
- 覆盖告警和优先级；
- 最终中文结论；
- 对抗性诊断自测。

### `WavePerfOutput.cpp/.h`

表现层：

- 离线 HTML；
- 总览、发射、调度、时间线、架构、资源、带宽和 PC 页面；
- 控制台摘要；
- 输出结构与诊断场景自测。

### `WaveParser4.cpp/.h`

WavePerf 只依赖 Viewer 的 WVZ4 v15 Reader。当前相关扩展是
`WaveParser4Reader::loadSignals()` 的进度回调。解析器仍是唯一解码事实来源，
不要在 WavePerf 中复制 WDAT 解码逻辑。

### `tests/waveperf_layout_modes/`

独立 WVZ4 测试生成器，生成：

- `normal`：显式数组元素；
- `flat`：`[size=N].[0]` 拍平代表元素；
- `latency`：低发射、低带宽、可配对 L1 延迟；
- `windowed`：文件首尾有空白、多个 QPPU 在不同时段发射，用于验证全局
  首末发射窗口。

---

## 3. 端到端数据流

```text
WVZ4 v15 directory
  -> 节点名快速预筛选
  -> 完整路径正式分类
  -> 选择性能信号
  -> 检查 signal_selection_complete
  -> loadSignals(全部 issue vld, 请求范围)
  -> 在覆盖完整时求全局首末发射并按业务周期对齐
  -> loadSignals(选择 ID, 实际分析范围)
  -> 每个信号按变化边界积分
  -> issue / scheduler / CBCtrl / resource / bandwidth / thread 事实
  -> architecture 递归聚合
  -> QPPU 二级结论
  -> 参与范围 workload_profile
  -> findings 与一级结论
  -> data.json
  -> 内嵌同一 JSON 的 index.html
```

不要让 HTML、控制台和 JSON 各自计算指标。`data.json` 是机器接口，也是 HTML
的数据源。表现层只负责选择显示内容，不应重新发明计数口径。

---

## 4. 时间与区间语义

### 4.1 半开区间

分析区间统一为：

```text
[startTick, endTick)
```

`--start-cycle` 和 `--end-cycle` 使用业务周期，结束周期不包含在内：

```text
startTick = startCycle * ticksPerCycle
endTick   = endCycle   * ticksPerCycle
```

默认 `ticksPerCycle=10`。不要把 WVZ4 内部 tick 当作业务 cycle。

### 4.2 左边界状态

普通区间积分需要知道 `startTick` 之前最后一个状态，并处理恰好位于
`startTick` 的变化。事件在 `endTick` 发生时不属于本区间。

### 4.3 不展开逐周期数组

主要分析器对变化记录的联合边界积分。这样复杂度取决于变化次数，不取决于业务
周期总数。只有明确需要在业务周期边界采样的流，例如 Thread 指令读取计数和
请求/返回握手，才按 `ticksPerCycle` 步进。

### 4.4 未对齐区间

若区间没有对齐业务周期边界，事件整数计数可能发生取整。报告必须保留警告，
不要静默把部分周期当完整周期。

---

## 5. Known、Unknown 与“没有跳变”

这是整个 Profiler 最重要的语义。

### 5.1 Unknown 不是 0

以下情况都必须视为 Unknown：

- 信号不存在；
- 分析区间起点之前没有可用状态；
- 样本为 Z/Absent；
- 数组只覆盖部分元素；
- issue slot 或 SG 必需字段缺失；
- `--max-signals` 截断了性能信号选择；
- occupancy 有值但 capacity 缺失；
- mask lane 不完整；
- 请求握手缺少 Taken/ready/store mask。

Unknown 不得进入完整分母，也不得用 0 填补。

### 5.2 稳定值仍然有效

`dynamic_signals == 0` 只表示所选范围没有发生值变化，不表示：

- 没有发射；
- 没有 SG 活动；
- 没有带宽；
- 没有 FIFO 压力；
- 只能生成静态快照。

稳定保持 `valid=1` 的信号必须覆盖整个区间并产生相应活跃周期。诊断不得再用
“是否发生跳变”作为性能计算开关。

### 5.3 可靠的无活动

只有所有观测 QPPU 的 SG 活动覆盖和 issue 覆盖都完整，并且 Active SG、
Queue Ready、实际发射均为 0 时，才允许发布 `no_activity`。

如果观测值为 0，但活动或 issue 覆盖不完整，应发布 `partial_coverage`，不能
说“没有 QPPU 参与”。

---

## 6. 信号选择与全局覆盖

### 6.1 两级分类

大波形可能有数百万信号。选择过程先使用树节点名做低成本候选筛选，再构造完整
路径并调用 `classifyArchitectureSignal()`。

新增性能字段时必须同时检查：

1. `fastPerformanceCandidate()` 是否能让该字段进入候选；
2. `classifyArchitectureSignal()` 是否给出正确 key/category；
3. helper 是否只参与下游联合分析而不重复成为普通 Counter；
4. EventSemantics 是否正确；
5. 分类器自测是否覆盖单数、复数和常见命名变体。

只改正式分类器而忘记快速预筛选，会造成小波形自测通过、大波形完全找不到信号。

### 6.2 `--max-signals`

达到上限时，只有后面确实还存在可分类的性能信号，才设置：

```json
"signal_selection_complete": false
```

不能仅因为目录还有普通信号就误报截断。

### 6.3 截断传播

`signal_selection_complete=false` 时：

- `workload_profile.regime = partial_selection`；
- `workload_profile.status = partial`；
- 全局 issue coverage 必须为 false；
- 一级结论必须是 `performance_coverage`；
- IPC、全局发射、Thread 总体效率等 KPI 显示“部分覆盖”；
- 禁止发布 QPPU 失衡、全局带宽、全局 Thread 效率或调度主瓶颈；
- 已完整观测的局部 QPPU 和资源仍可显示；
- 局部 FIFO/Queue/Credit 只能作为容量风险，不能升级为 critical 根因。

局部证据可看，不等于全局采样完整。

---

## 7. 事件信号语义

不要对所有事件使用上升沿。当前有四种语义：

### `None`

普通状态或数值，不生成事件数。

### `PerCycleValue`

每个业务周期的数值就是本周期事件条数：

```text
event_count = integral(value * ticks) / ticksPerCycle
```

用于：

- `m_num_read`
- `m_num_written`

如果数值 2 连续保持 3 个周期，事件数是 6，不是 1。

### `PerCycleMask`

每个业务周期的置位 bit 数就是事件数：

```text
event_count = integral(popcount(mask) * ticks) / ticksPerCycle
```

用于非累计的 cache hit/miss valid 或 mask。两个连续周期都为 1，代表两个事件，
不能因为中间没有下降沿就合并成一次。

### `CumulativeCounter`

累计计数器按所有正增量求和：

```text
event_count = sum(max(next - current, 0))
```

用于 `cache_hit_count`、`cache_hits_count`、`cache_miss_count`、
`cache_misses_count` 等明确命名。

计数器下降视为复位或不连续：

- 下降不计为事件；
- 记录 discontinuity；
- Cache 命中率覆盖不完整；
- 报告警告可能存在回绕低估。

### Cache 配对

命中率只在 hit 和 miss：

- 来源数量匹配；
- 资源父路径集合匹配；
- 没有 Unknown 周期；
- 没有累计 Counter discontinuity；

时发布。否则显示未覆盖，不输出假 0%。

---

## 8. Issue 与吞吐

### 8.1 发射利用率

每个 issue slot 的 `vld` 为 1 时累计一条指令：

```text
issued_instructions = sum(active issue slots over time)
issue_utilization   = issued_instructions / observed QPPU cycles
```

单发周期加 1，双发周期加 2。因此每 QPPU 发射利用率可以超过 100%，理论上限
由实际 slot 数决定，当前主架构为 200%。

另一个指标 `issue_slot_utilization_percent` 的分母是所有 slot capacity，范围
通常为 0% 到 100%。不要混淆这两个指标。

### 8.2 Issue Active

只要任一 slot 发射，本 QPPU 本周期为 issue active：

```text
issue_active_percent = cycles(any slot valid) / observed QPPU cycles
```

它衡量发射链是否持续工作，不区分单发和双发。

### 8.3 Main/Shadow

`issue_inst_[0]` 和 `[1]` 表示 Main/Shadow 位置。它们不是固定的普通指令槽和
Group 指令槽。类型必须读取当前发射记录的 `instIssueType`。

### 8.4 指令类型

当前功能类别为：

- Thread
- Group
- CB
- MMA

当前配套模型的 `InstIssueType` 固定映射为：

```text
0 NotIssue
1 Thread
2 ThreadEbb
3 Group
4 GroupEbb
5 CB
6 QppuMMA
7 QppuMMAEbb
8 Count
```

旧版本使用过从 2 开始的另一套编号，不能在当前模型上复用。类型编号、类别
映射、HTML 名称和 self-test 必须由同一张映射维护。

原始 `mainType` 与 `instIssueType` 直方图保留在 JSON。当前没有完整 opcode 到
助记符的解码，不得在报告中臆造具体 opcode。

类型字段缺失时，issue 活动仍可完整，但类型覆盖必须单独标为部分。

### 8.5 访存分类

Global/Local memory 依赖 `isGlobalMem` 与 `isLocalMem`。两者都覆盖时才认为该
slot 的访存分类完整。不能因 `isGlobalMem=0` 就直接推断为 Local。

### 8.6 全局与参与范围

根 `summary` 可以表示整个观测架构的总量。`workload_profile` 则先排除可靠确认
未参与的 QPPU，再计算参与范围内的每 QPPU 平均发射利用率。

对于只激活一个 QPPU 的 microbenchmark：

- 全局 issue utilization 可能被所有 QPPU 分母稀释；
- workload issue utilization 应反映这个参与 QPPU 的真实速率；
- 一级瓶颈使用 workload 范围；
- 架构总量仍保留在 summary。

---

## 9. SG 调度模型

### 9.1 Active

```text
sg_table_[SG].valid != 0
```

### 9.2 Queue Ready

```text
Active && instr_queue_[SG].m_count > 0
```

### 9.3 实际发射

任一完整 issue slot：

```text
issue_inst_[slot].vld &&
issue_inst_[slot].sgId == SG
```

### 9.4 局部 Eligible

Queue Ready，并且已观测的以下条件均未阻塞：

- Barrier；
- flow control；
- Sleep；
- SetMaxTemp；
- stall/delay counter；
- 7 组 dependency counter；
- 当前 Thread 功能单元 pending；
- issue slot 与 SG attribution 覆盖完整。

对于 FP32MinOrMax，可检查两个 FP32 单元是否都 pending；其他 Thread 指令使用
predecode 指定的执行单元。

### 9.5 Eligible 的边界

局部 Eligible 不覆盖所有操作数打包、写回冲突和最终仲裁。因此：

- `issued` 可以证明最终可发射；
- `eligible && !issued` 只能说明已知调度条件无法解释空洞；
- 这类空洞归入 BE/仲裁或未覆盖执行链，置信度应保守；
- 不得把局部 Eligible 写成“硬件本周期一定可以发射”。

### 9.6 阻塞原因

主要原因按明确状态记录，包括：

- barrier
- flow_control
- sleep
- set_max_temp
- stall_count
- dependency
- function_unit

缺少必要字段时状态为 Unknown。已观测到一个局部阻塞，但其他条件覆盖不完整时，
可以报告局部风险，不能说它解释了全部空洞。

### 9.7 PC 等待

Queue Head 等待定义为：

```text
Queue Ready && 本周期未发射该 SG
```

只有队列读指针、队首 PC、类型、SG 状态和 issue attribution 都可用时才累计。
不得从任意 queue entry、上一次 issue 或名字相似的 PC 猜测当前阻塞指令。
队首索引必须由该 SG 的 queue read pointer 解析，再读取同一 entry 的
`PC.pc_`、`preDecode.instIssueType` 和指令特征。

CB 指令还应读取同一 entry/slot 的 `preDecode.cb_inst_client`。当前
`CBCtrlInstClient` 映射为：

```text
0 IMGLDST       1 PSO          2 TEXTURE
3 FP64          4 LDST         5 FCU
6 TAC           7 UMMA         8 TotalNum
9 MBAlloc      10 DTF         11 BWGBarrier
12 NonCBInstrEbb
```

例如 AtomicAdd 类 CB memory 指令会由模型解码为 `CB / LDST`。客户端枚举只
描述指令将去往哪个后端，不等于该后端资源已经不足。资源耗尽必须另有同周期
credit、FIFO、inflight 或明确 stall 信号作为证据。

可用于报告阻塞指令类型的 Predecode 字段必须同时满足：

1. 配套模型源码明确显示字段如何赋值或消费；
2. 实际 WVZ4 的 queue entry 与 issue slot 指令对象中存在该字段；
3. 快速预筛选、正式分类和 Scheduler queue-entry 匹配均已接入；
4. normal、flat、latency 三种布局和 Scheduler self-test 均覆盖。

字段仅在波形中出现不代表语义已确认。`isFence` 已由模型维护者确认表示
Fence 指令，现纳入统一特征表。所有已确认字段均由
`instructionFeatureSpecs()` 统一维护，issue 特征与 queue-head 等待特征必须
共用这张表，避免两套口径漂移。

PC、粗类型和每个指令特征的覆盖必须分别报告。缺失特征是 Unknown，不是 false；
只有特征覆盖完整时才能说“该阻塞指令没有某标记”。多个特征可同时为真，占比
允许重叠。

`scheduler.summary.pc_wait_instruction_flags` 按置位等待周期从高到低列出
实际命中的特征；每个 QPPU 在同名字段中保存自己的分布。每项至少包含
`source_field`、`group`、`active_wait_cycles`、`wait_share_percent`、
`active_when_known_percent`、`coverage_percent` 和 `covered`。
`wait_share_percent` 的分母是全部 `Queue Ready && 未发射` 等待周期，
`active_when_known_percent` 的分母只包含该 flag 已知的等待周期。两者不能
混用；多个 flag 可同时置位，因此所有 `wait_share_percent` 相加可以超过
100%。

---

## 10. QPPU 参与范围

一个 QPPU 在以下任一事实成立时参与：

- Active SG > 0；
- Queue Ready > 0；
- 实际发射指令 > 0。

如果 SG 覆盖完整并且三者均为 0，可以标记为 inactive，不进入负载均衡分母。

如果 SG 覆盖缺失：

- 不得仅凭 issue=0 排除该 QPPU；
- 应保守纳入参与候选；
- 若 issue 也不完整，工作负载状态为 partial；
- 只有所有观测 QPPU 的活动与 issue 都完整，才允许发布全局 no_activity。

---

## 11. 每 QPPU 二级诊断

局部诊断的主要顺序是：

1. 可靠确认未参与；
2. issue 与 SG 均未覆盖；
3. issue 有空洞但 SG 未覆盖；
4. Active SG 容量不足；
5. Queue Ready/Active 过低，归因 FE；
6. Eligible 低且明确阻塞占比高，归因 QPPUCtrl；
7. 阻塞局部存在但覆盖不完整，仅标风险；
8. issue 覆盖不足，停止后端归因；
9. issue active 低并伴随同作用域资源满压；
10. Eligible 较高但 issue active 低，归因 BE/仲裁或执行链；
11. issue active 高但资源压力高，只标高压运行风险；
12. 否则为健康或未发现明确瓶颈。

注意：

- issue 未完整覆盖时，不在结论对象中发布推算的未发射周期；
- Active/Queue Ready 的局部结论可以独立于 issue 覆盖成立，但证据中必须说明
  issue 未覆盖；
- 资源压力只有在同一 QPPU 作用域且伴随发射不足时才可能升级；
- “模块瓶颈”必须包含原因和下一步，不只输出一个模块名。

---

## 12. Workload Regime

工作负载一级判型大致按以下优先级：

1. `partial_selection`：性能信号选择截断；
2. `partial_coverage`：无活动但无法证明，或参与 QPPU issue 覆盖不足；
3. `no_activity`：完整覆盖下确实没有执行；
4. `workload_fill`：Active SG 容量明显不足；
5. `frontend_supply`：SG 已驻留但 Queue Ready 不足；
6. `memory_bandwidth`：完整可用带宽达到阈值；
7. `memory_latency`：发射不足、访存/依赖证据和可靠延迟同时成立；
8. `dependency_latency`：明确依赖等待主导；
9. `resource_backpressure`：同作用域资源压力伴随发射不足；
10. `execution_latency`：Eligible 较高但 issue active 偏低；
11. `throughput`：issue active 高或每 QPPU 发射达到单发基线；
12. `scheduler_bound`：明确调度阻塞；
13. `mixed`：没有单一证据达到阈值。

阈值是启发式规则，不是硬件规范。修改阈值时必须同时检查：

- 单 QPPU microbenchmark；
- 多 QPPU 负载；
- 只有一个参与 QPPU 的场景；
- 完整与部分覆盖；
- normal 与 flat；
- latency 与 throughput 两个方向。

---

## 13. Findings 与优先级

findings 分为 critical、warning 和 info，并带 `priority_score`。主结论是排序后的
第一项。

覆盖告警必须压过从局部前缀推导出的风险。尤其注意：

- `partial_selection` 的 `performance_coverage` 必须为主结论；
- 截断时不得出现 critical 局部证据；
- `scheduler_coverage` 不应与 `partial_selection` 重复；
- QPPU 失衡必须至少有两个完整、参与且 issue 可比的 QPPU；
- 带宽、Thread 总体效率和失衡必须受全局 signal selection coverage 保护；
- FIFO/Queue/Credit 高压但没有发射下降，只能是容量风险；
- 高带宽可以独立成为通路风险，但要有完整 lane/mask/峰值覆盖。

---

## 14. 普通数组与拍平数组

### 14.1 普通数组

```text
m_QPPUTOP.[0]
m_QPPUTOP.[1]
...
```

每个显式元素都是独立观测，逐个诊断并向上聚合。

### 14.2 拍平数组

```text
m_QPPUTOP[size=4].[0]
```

只观测 `[0]`，逻辑大小为 4。架构树可记录 represented instances，但性能数据
仍是 `[0]` 的代表性投影：

- 不复制四份事件；
- 不把代表值乘四伪装成实测总量；
- 不生成未观测 QPPU 的局部结论；
- 页面明确标记 array-first-representative。

### 14.3 路径规则

正则通常需要同时接受：

```text
array_.[0]
array_[size=N].[0]
```

修改路径匹配时必须用 normal 和 flat 两个生成波形回归。只测试一个布局是不够的。

---

## 15. FIFO、Queue 与 Credit

### 15.1 满率

资源满率必须在 occupancy 与 capacity 的联合变化边界上积分：

```text
full = occupancy >= capacity
full_rate = full known ticks / capacity-known ticks
```

capacity 为 0 或任一值 Unknown 时，该区间不进入完整分母。

### 15.2 FIFO 与 Queue 分开

路径包含 queue 的 `m_count/m_numAvail/m_num_readable` 归为 Queue occupancy；
普通 `m_numAvail/m_num_readable` 归为 FIFO occupancy。不要把两者合并。

### 15.3 同资源多个 occupancy 字段

同一资源可能同时暴露 `m_numAvail` 与 `m_num_readable`。资源压力表只能选择一个
occupancy 事实贡献满窗口，架构聚合也只能附加一次满周期。否则：

- full resource 数会翻倍；
- full cycles 会翻倍；
- 上层聚合虽然百分比可能看似不变，但证据量错误。

### 15.4 Top 50

只列出实际有满周期的实例，按满率排序，最多 50 个。没有 50 个时不补零。

### 15.5 Credit

已确认语义的 Credit Counter 表示剩余可用 credit：

```text
exhausted = available_credit == 0
```

当前只纳入：

- `mma_ldMb_credit_cnt_`
- `mma_stMb_credit_cnt_`
- `fe_dicache_credit_`

名字中含 credit 的脉冲、握手或累计请求数不能自动当作可用 Credit。

---

## 16. Thread 有效率

采样时机是 QPPUEU 实际读取 Thread 指令的业务周期。对当前 SG 读取：

- valid mask；
- active mask；
- execute mask。

指标：

```text
valid occupancy = valid lanes / lane capacity
active efficiency = active lanes / valid lanes
thread efficiency = execute lanes / valid lanes
```

硬约束：

- active 必须是 valid 的子集；
- execute 必须是 valid 的子集；
- lane 数、每 lane 值和 Thread 读取流都必须覆盖；
- 任一子集矛盾时标记 `thread_masks_inconsistent`；
- 不得输出大于 100% 的 Thread 效率来掩盖 mask 错配；
- 部分覆盖时每 SG 可保留局部条目，但总体效率不发布；
- signal selection 截断时总体 Thread KPI 显示部分覆盖。

---

## 17. L1/L2 带宽

### 17.1 有效字节

只统计握手成立且 mask 有效的字节，不使用 valid 周期直接冒充满带宽。

当前主要口径：

- L2 Read：parent valid、sector valid、sector mask；
- L2 Write：write valid、write mask；
- mask bit 到字节的换算必须已知；
- 声明 lane 必须全部覆盖。

### 17.2 峰值

当前部分峰值是 architecture target，而不是实现源码核实值：

- L1 read target：128 B/cycle；
- L1 write target：64 B/cycle；
- L2 read target：64 B/cycle；
- L2 write target：32 B/cycle。

`implementation_peak_verified=false` 时，报告必须保留 peak basis/note。不要把目标
值描述成已验证的 RTL 峰值。

### 17.3 利用率可用条件

只有以下条件同时成立才发布利用率：

- 数据边界与握手已确认；
- lane 布局完整；
- mask 值完整；
- mask bit 字节宽度已知；
- 峰值口径存在。

否则发布 unavailable/partial，并说明 reason。若 lane 布局不完整但已观测
lane 的值覆盖完整，可以发布“已观测 lane 的 B/cycle”，状态必须为
`partial`，且不得发布全接口利用率。

---

## 18. DLS-L1 延迟

当前延迟配对按同通道顺序：

```text
request = request.valid && Taken && 非 Store
response = response.valid && response.ready
latency = response_cycle - oldest_unmatched_request_cycle
```

Store 通过 store smask 排除。store mask 缺失时覆盖不完整。

置信度规则：

- 完整覆盖、无未配对请求/返回、maximum outstanding <= 1：高置信；
- 多 outstanding 且没有 transaction ID：低置信；
- 任一握手、store mask 或区间状态缺失：低置信或不可用。

诊断只使用高可靠延迟。多 outstanding 的 FIFO 顺序统计可展示，但不能直接升级
成 L1 延迟根因。未来若加入 request/response transaction ID，应优先按 ID 配对，
并保留顺序模式作为无 ID 的保守回退。

请求和响应同周期时延迟可以为 0。outstanding 同时加减时，不应制造负值。

---

## 19. 架构树聚合

树以实际路径为基础，聚合时必须避免：

- 同一 Counter 在父子节点重复加入同一节点；
- 拍平代表实例乘逻辑大小；
- 同一资源的多个 occupancy 字段重复贡献满窗口；
- hit/miss 来源不配对却发布命中率；
- issue slot 缺失却发布完整利用率。

节点中以下概念不同：

- observed instances：实际波形元素；
- represented instances：拍平元数据表示的逻辑元素；
- counters：实际选中的 Counter；
- active signal cycles：每信号活跃周期总和；
- issue utilization：每 QPPU 周期的发射条数；
- issue slot utilization：slot capacity 利用率；
- FIFO/Queue full rate：资源周期加权满率。

不要把 signal-cycles 当模块 busy cycles。多个信号同周期有效时，signal-cycles
可以大于业务周期。

---

## 20. JSON 模型

当前 `schema_version` 为 8。主要顶层字段：

- `file`
- `analysis`
- `coverage`
- `summary`
- `scheduler`
- `cb_ctrl`
- `sg_thread_efficiency`
- `memory_bandwidth`
- `resource_pressure`
- `architecture`
- `qppu_conclusions`
- `workload_profile`
- `findings`
- `issue_main_type_cycles`
- `issue_type_cycles`
- `warnings`
- `conclusion`

重要覆盖字段：

```text
coverage.signal_selection_complete
summary.issue_activity_coverage_complete
summary.issue_type_signals_covered
summary.memory_issue_signals_covered
scheduler.summary.activity_coverage_complete
scheduler.summary.eligibility_coverage_complete
cb_ctrl.coverage.first_uop_flag_coverage_percent
workload_profile.selection_coverage_complete
workload_profile.issue_coverage_complete
workload_profile.activity_coverage_complete
workload_profile.eligibility_coverage_complete
```

新增指标时：

1. 数值与覆盖字段一起设计；
2. unavailable 与合法 0 分开；
3. 大整数事件数用字符串，避免 JavaScript 精度丢失；
4. HTML 只能在覆盖成立时显示百分比；
5. 控制台摘要必须与 HTML 使用同一覆盖；
6. 更新 schema 说明与自测。

当前不承担旧 Profiler 输出兼容负担，但不要无目的改名。若重构模型，应一次性同步
JSON、HTML、自测和本文。

---

## 21. 报告输出

`index.html` 内嵌完整 JSON，可直接双击打开。它不能依赖：

- 本地 HTTP 服务；
- fetch 本地 `data.json`；
- 外部 CDN；
- 浏览器扩展。

输出验证至少包括：

- `data.json` 可解析；
- UTF-8 中没有 U+FFFD；
- 所有内嵌 `<script>` 可由 JavaScript 解析；
- 截断 KPI 显示部分覆盖；
- 未覆盖不显示为 0%；
- 大于 100% 的发射利用率不被进度条破坏；
- Thread/FIFO/Queue 表的状态与 JSON 一致。

---

## 22. 已知限制

1. 只支持 WVZ4 v15，不保留 v13 兼容。
2. 当前识别指令功能类别与原始枚举，不解码完整 opcode 助记符。
3. 局部 Eligible 缺少部分操作数、写回和最终仲裁事实。
4. DLS-L1 多 outstanding 没有 transaction ID 时只能低置信顺序配对。
5. L1/L2 部分峰值是架构目标，不是实现核实值。
6. 拍平波形只代表 `[0]`，不能恢复每个实例的真实负载分布。
7. FIFO/Queue 高满率与发射空洞目前主要按同作用域关联，最终因果仍应在时间线上
   检查重合。
8. 数百万信号文件的主要耗时可能是候选选择和路径构造，而不是 WDAT 解码。
9. 没有 GPU 性能字段的 WVZ4 返回退出码 5，这是正常拒绝，不是解析失败。

---

## 23. 常见错误模式

### 错误：用上升沿统计事件

后果：连续两个周期的 hit/miss 或 FIFO read/write 被合并。

正确做法：按 PerCycleValue、PerCycleMask 或 CumulativeCounter 分类积分。

### 错误：`dynamic_signals == 0` 就停止性能分析

后果：稳定 valid=1 的吞吐被报告成静态快照或无活动。

正确做法：按已知状态覆盖区间积分，跳变数只作描述。

### 错误：截断后的 QPPU 前缀当完整 GPU

后果：`--max-signals=40/80/120` 分别看到 1/2/3 个 QPPU，却发布全局失衡和瓶颈。

正确做法：全链路传播 `signal_selection_complete=false`。

### 错误：零值等于可靠空闲

后果：缺失 issue/SG 信号的模型被误报 no_activity。

正确做法：无活动结论要求所有观测 QPPU 的活动与 issue 覆盖完整。

### 错误：局部风险压过覆盖告警

后果：截断报告中的一个 FIFO 满压成为一级 critical。

正确做法：截断时局部证据最多作为 warning，主结论必须是覆盖不足。

### 错误：同资源多个 occupancy 重复附加满窗口

后果：满资源数和满周期翻倍。

正确做法：每个资源只选择一个 occupancy Counter 贡献 full window。

### 错误：slot 位置当指令类型

后果：Main 被写成普通指令、Shadow 被写成 Group。

正确做法：位置和 `instIssueType` 独立统计。

### 错误：QPPU 利用率用全部 GPU 分母

后果：单 QPPU microbenchmark 被四个 QPPU 分母稀释。

正确做法：保留全局 summary，同时用参与 QPPU 形成 workload_profile。

### 错误：FIFO 平均占用低就认为没压力

后果：大 FIFO 的短时满压被平均值隐藏。

正确做法：主要排序使用 full rate，平均占用只作补充。

### 错误：没有未发射就说资源不是风险

后果：吞吐 microbenchmark 中接近耗尽的 Queue/Credit 完全消失。

正确做法：保留容量风险，但不升级为当前根因。

---

## 24. 自测结构

`WavePerf.exe --self-test` 覆盖：

- 布尔与数值区间积分；
- 部分 issue slot 覆盖；
- 连续 Cache 事件脉冲；
- FIFO 每周期多事件；
- 累计 Counter 与复位；
- FIFO full window；
- issue 双发与分类；
- normal/flat 架构聚合；
- Cache/FIFO 事件配对；
- Scheduler 状态与 PC；
- L1/L2 字节和延迟；
- CBCtrl 连续 FIFO 事件、payload 边界、请求/uop/返回/Pending 关联；
- CBCtrl 全局 finding、逐 QPPU 归因及 signal selection 截断门禁；
- QPPU 局部模块诊断；
- 单 QPPU microbenchmark 范围；
- 稳定活动信号；
- Unknown 零活动与可靠 no_activity；
- 部分调度覆盖；
- signal selection 截断；
- Thread mask 子集矛盾；
- HTML 和控制台输出。

修改算法时，优先把反例加入对应模块的 self-test。不要只在外部脚本验证后留下
不可复现的修复。

---

## 25. 构建与回归命令

### 25.1 Release

```powershell
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe'

& $msbuild QtViewer\WavePerf.vcxproj `
  /m /p:Configuration=Release /p:Platform=x64 /p:WarningLevel=Level4

QtViewer\build\x64\Release\WavePerf.exe --self-test
```

### 25.2 Debug

```powershell
& $msbuild QtViewer\WavePerf.vcxproj `
  /m /p:Configuration=Debug /p:Platform=x64 /p:WarningLevel=Level4

QtViewer\build\x64\Debug\WavePerf.exe --self-test
```

### 25.3 布局生成器

```powershell
& $msbuild tests\waveperf_layout_modes\waveperf_layout_modes_writer.vcxproj `
  /m /p:Configuration=Release /p:Platform=x64

$writer = 'tests\waveperf_layout_modes\build\waveperf_layout_modes_writer.exe'

& $writer normal  build_vs\waveperf_normal.wvz4
& $writer flat    build_vs\waveperf_flat.wvz4
& $writer latency build_vs\waveperf_latency.wvz4
& $writer windowed build_vs\waveperf_windowed.wvz4
```

### 25.4 生成报告

```powershell
$perf = 'QtViewer\build\x64\Release\WavePerf.exe'

& $perf build_vs\waveperf_normal.wvz4 `
  --out build_vs\waveperf_normal.perf --no-progress

& $perf build_vs\waveperf_flat.wvz4 `
  --out build_vs\waveperf_flat.perf --no-progress

& $perf build_vs\waveperf_latency.wvz4 `
  --out build_vs\waveperf_latency.perf --no-progress

& $perf build_vs\waveperf_windowed.wvz4 `
  --out build_vs\waveperf_windowed.perf --no-progress
```

### 25.5 截断对抗

```powershell
foreach ($limit in 40, 80, 120) {
  & $perf build_vs\waveperf_normal.wvz4 `
    --out "build_vs\waveperf_normal_max_$limit.perf" `
    --max-signals $limit --no-progress
}
```

每份截断报告必须满足：

```text
coverage.signal_selection_complete == false
workload_profile.status == partial
workload_profile.regime == partial_selection
findings[0].key == performance_coverage
summary.issue_activity_coverage_complete == false
不存在 critical finding
不存在 qppu_imbalance / memory_bandwidth / issue_underfill / cb_ctrl_bottleneck
```

### 25.6 全局首末发射范围

```powershell
& $perf build_vs\waveperf_windowed.wvz4 `
  --out build_vs\waveperf_windowed.perf --no-progress
```

`windowed` 文件请求范围为 tick `[0,1000)`，实际发射分布在多个 QPPU 上。
应得到：

```text
analysis.requested_start_tick == "0"
analysis.requested_end_tick == "1000"
analysis.first_issue_tick == "100"
analysis.last_issue_tick_exclusive == "800"
analysis.start_tick == "100"
analysis.end_tick == "800"
analysis.duration_cycles == 70
analysis.range_basis == "global_issue_window"
analysis.global_issue_window_coverage_complete == true
```

再验证显式外层裁剪：

```powershell
& $perf build_vs\waveperf_windowed.wvz4 `
  --out build_vs\waveperf_windowed_0_60.perf `
  --start-cycle 0 --end-cycle 60 --no-progress
```

应得到请求范围 tick `[0,600)`、实际范围 `[100,600)`。如果显式范围内完全
没有发射，必须保留显式范围并设置
`range_basis=requested_range_fallback`，不能生成空分析区间。

自动收缩必须满足以下不变量：

- 合并所有已选 QPPU 和所有 Main/Shadow 发射槽，不按单实例裁剪；
- 首条发射所在业务周期完整保留；
- 最后一条发射所在业务周期完整保留；
- 显式 start/end 是不可越过的外层边界；
- 发射槽覆盖不完整、信号选择截断或范围内没有发射时禁止自动收缩；
- 诊断、分母、时间线、PC 热点、内存延迟和 HTML/JSON 必须共用实际范围。

---

## 26. 基准回归预期

测试生成器当前预期方向：

### normal

- 4 个显式 QPPU；
- 发射利用率约 92.5%；
- QPPU 之间存在明显差异；
- 一级结论可体现低利用率 QPPU 的内部执行链限制；
- L2 带宽远离峰值；
- 事件总数：FIFO read/write 8/16，Cache hit/miss 8/4。

### flat

- 只观测代表 QPPU `[0]`；
- 全局首末发射窗口为 80 个业务周期，发射利用率约 150%；
- workload 为 throughput；
- 资源压力可以是容量风险；
- 事件总数：FIFO read/write 2/4，Cache hit/miss 2/1。

### latency

- 只有一个 QPPU 实际参与；
- 在最后一个业务周期额外保留一条发射，使请求/返回延迟证据位于全局首末
  发射窗口内；
- 参与范围 issue utilization 约 2%；
- 全局四 QPPU summary 约 0.5%；
- L1 延迟高置信且带宽不高；
- workload 为 memory_latency；
- 事件总数：FIFO read/write 8/16，Cache hit/miss 8/4。

### windowed

- 请求范围 tick `[0,1000)`；
- QPPU 0 在 tick `[100,300)` 发射，QPPU 1 在 `[500,800)` 发射；
- 实际分析范围为 `[100,800)`，共 70 个业务周期；
- 文件首尾空白不进入利用率分母；
- 显式外层裁剪与无发射回退均保持稳定。

数值变化时先判断测试生成器是否有意修改，不要直接把断言改成新结果。

---

## 27. 大波形检查

大波形至少记录：

- `file.signals`
- `analysis.matched_signals`
- `analysis.reported_counters`
- `analysis.decoded_samples`
- `analysis.open_ms`
- `analysis.selection_ms`
- `analysis.issue_window_scan_ms`
- `analysis.decode_ms`
- `analysis.analysis_ms`
- `coverage.signal_selection_complete`

当前 229 万信号、约 4764 个性能候选的恢复波形，Release 端到端约 1.1 秒，
主要耗时在信号选择而非解码。该数据只作数量级参考，不要写成固定性能承诺。

优化大波形时优先保持以下不变量：

- 快速筛选不能漏掉正式分类信号；
- 不为所有信号构造完整路径；
- 不解码未选择信号；
- 不把周期范围展开为逐周期数组；
- 优化前后 JSON 事实与覆盖一致。

---

## 28. 提交前检查表

### 代码

- [ ] 新信号同时进入快速筛选和正式分类。
- [ ] EventSemantics 与硬件字段语义一致。
- [ ] Unknown 没有被当成 0。
- [ ] 稳定值能覆盖整个区间。
- [ ] 普通和拍平路径都匹配。
- [ ] issue、activity、eligibility、selection 覆盖分别传播。
- [ ] 局部风险不会越级成为全局根因。
- [ ] 没有重复资源聚合。
- [ ] 大整数不会以 JavaScript Number 丢精度。

### 测试

- [ ] Release Level4 构建通过。
- [ ] Debug Level4 构建通过。
- [ ] 两个配置的 `--self-test` 通过。
- [ ] normal/flat/latency/windowed 均能生成报告。
- [ ] windowed 全局首末发射范围通过。
- [ ] 显式外层裁剪、无发射回退和覆盖不完整回退通过。
- [ ] 40/80/120 截断对抗通过。
- [ ] 事件总数符合预期。
- [ ] JSON 可解析且无 U+FFFD。
- [ ] HTML 内嵌脚本可解析。
- [ ] 至少一个大波形通过。

### Git

- [ ] 不提交 `build/`、`build_vs/`、`.wvz4`、`.perf/`、`.exe`、`.pdb`。
- [ ] 文档与当前 schema、阈值和命令一致。
- [ ] 提交中包含需要的 solution/project 接入。
- [ ] 推送前确认分支与远端。

---

## 29. 维护原则总结

当证据不够时，WavePerf 应该明确告诉使用者“缺什么”，而不是给出一个看似完整
的百分比。维护时始终问四个问题：

1. 这个数的分子是什么？
2. 这个数的分母是否完整？
3. 这个证据只属于局部实例，还是能代表整个 GPU？
4. 这个结论是直接观测、保守推断，还是尚未覆盖？

只要这四个问题在 JSON、HTML、控制台和自测中保持一致，Profiler 才不会在波形
规模、布局或信号稳定性变化后悄悄给出错误结论。
