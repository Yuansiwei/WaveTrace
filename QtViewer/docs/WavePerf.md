# WavePerf 波形性能分析器

WavePerf 使用 Viewer 的 WVZ4 v17 解析器，只加载 GPU 性能分析需要的信号。
同一份区间积分结果同时驱动诊断、HTML 页面和 JSON，不按报表重复扫描波形。

维护实现、增加指标或修改诊断规则前，请先阅读
[`WavePerf_AGENT_GUIDE.md`](WavePerf_AGENT_GUIDE.md)。该文档定义覆盖传播、
事件语义、普通/拍平布局回归和提交前检查。

## 使用

```powershell
QtViewer\build\x64\Release\WavePerf.exe wave.wvz4
```

默认生成：

```text
wave.perf/
  index.html
  data.json
```

`index.html` 内嵌本次分析数据，可以直接双击打开，不依赖服务端或浏览器本地
文件读取权限。`data.json` 是唯一机器接口。

常用参数：

```powershell
WavePerf.exe wave.wvz4 `
  --out result.perf `
  --start-cycle 1000 `
  --end-cycle 2000 `
  --ticks-per-cycle 10 `
  --timeline-bins 200
```

- `--start-cycle/--end-cycle` 使用业务周期，结束周期不包含在范围内；它们是
  自动收缩前的外层裁剪边界。
- `--ticks-per-cycle` 指定业务周期到 WVZ4 tick 的换算，默认 10。
- `--timeline-bins` 控制时间线分桶数，默认 160，范围 1..1000。
- `--no-progress` 关闭 WDAT 解码进度条。
- `--max-samples/--max-signals` 是大文件保护上限。
- `--dump-predecode-fields` 列出波形内实际存在的 Predecode 字段和示例路径，
  用于核对版本差异；列出字段不等于分析器已经采用其语义。

旧 `profile/Markdown/CSV/逐信号 JSON` 接口不再保留。

## 默认分析范围

WavePerf 默认只分析全局第一条实际发射的指令到最后一条实际发射的指令：

1. 第一遍只解码所有已覆盖的 `issue_inst_[slot].vld`。
2. 在 `--start-cycle/--end-cycle` 指定的外层范围内，寻找任意 QPPU、任意
   Main/Shadow 槽第一次和最后一次有效发射。
3. 起点向下对齐到第一条发射所在的业务周期，终点向上对齐到最后一条发射
   所在业务周期的末尾。
4. 第二遍只解码这个有效区间内的全部性能信号，所有利用率、等待时间、时间线
   和热点统一使用该区间。

因此仿真启动和结束阶段没有指令发射的空白周期不会稀释利用率。显式
`--start-cycle/--end-cycle` 仍然有效，但只负责先裁掉不希望分析的外层区间。

只有在发射槽选择完整时才会自动收缩。若范围内完全没有指令发射，或发射槽
覆盖不完整，WavePerf 会保留请求范围并给出覆盖告警，不会猜测首末发射位置。
`data.json` 的 `analysis` 中同时保留：

- `requested_start_tick/requested_end_tick`：用户请求或文件给出的外层范围；
- `start_tick/end_tick`：实际分析范围；
- `range_basis`：`global_issue_window` 或 `requested_range_fallback`；
- `first_issue_tick/last_issue_tick_exclusive`：实际观测到的首末发射边界。

## 分析流程

1. 自动识别 QPPU、PPU、DPPU、Cluster、GPU、FIFO、Queue 和 L1/L2 信号。
2. 先只读取全局发射有效位，确定可靠的首末发射窗口。
3. 对有效窗口内的变化记录做区间积分，不把波形展开为逐周期数组。
4. 建立每个 QPPU、每个 SG 的调度状态和时间线分桶。
5. 由同一组事实生成发射、线程、缓冲、带宽和架构树指标。
6. 先推断实际参与执行的 QPPU，再统一判定驻留、前端供给、调度依赖、
   执行延迟、资源背压、访存延迟、带宽和持续吞吐。

报告页面分为：

- 总览：一级主瓶颈、逐 QPPU 二级归因、核心利用率和补充证据。
- 发射 / Thread：指令类型、Main/Shadow、双发组合、每 SG Thread 有效率和
  QPPU EU 指标；同时显示源码已确认的 Predecode 指令特征。
- 调度器：每个 QPPU/SG 的 Active、Queue Ready、Eligible 和阻塞原因。
- CBCtrl：请求、寄存器读 uop、EU 操作数返回、CBData 下发/写回和 Pending
  清除六段链路；按 QPPU、客户端、SG 和 PC 展示服务率、延迟与公平性。
- 时间线：分阶段查看吞吐和调度状态。
- 架构树：QPPU 到 GPU 的递归聚合利用率。
- 资源压力：实际 FIFO、Queue、Credit Counter 压力 Top 50，以及模块聚合的
  Stall、Pending 和缓存命中证据；未覆盖不显示为 0%。
- L1/L2：有效字节带宽、峰值口径、DLS-L1 请求返回延迟和覆盖状态。
- PC 热点：按发射有效时间排序的 PC，以及 Queue Head PC 的等待时间和主要
  阻塞状态。

## 关键口径

### 发射利用率

只要发出一条指令就累计一次，双发射同周期累计两次，再除以
`QPPU 数量 × 业务周期`。因此单 QPPU 发射利用率允许超过 100%，上限 200%。

`issue_inst_[0]` 和 `[1]` 是 Main/Shadow 发射顺序，不是固定的普通指令槽与
Group 指令槽。指令功能类型按 `instIssueType` 归类为 Thread、Group、CB、
MMA。

当前模型的 `InstIssueType` 数值口径为：

```text
0 NotIssue
1 Thread
2 Thread EBB
3 Group
4 Group EBB
5 CB
6 QPPU MMA
7 QPPU MMA EBB
8 Count
```

该表不能沿用旧模型中从 2 开始的类型编号，否则会把 CB 等类型整体错判。

### SG 调度状态

- Active：`sg_table_[SG].valid` 有效。
- Queue Ready：Active 且 `instr_queue_[SG].m_count > 0`。
- 实际发射：任一 `issue_inst_[slot].vld` 且 `sgId` 指向该 SG。
- 局部 Eligible：Queue Ready，且已观测的 Barrier、流控、Sleep、
  SetMaxTemp、延迟计数和 7 组依赖计数均未阻塞。

局部 Eligible 不会假设未出现在波形中的操作数打包、写回端口和功能单元仲裁
已经通过。缺少任一必要信号时，相关周期进入 Unknown，报告标记为部分覆盖。

`inflight_mem_cnt_[SG]` 表示在途访存压力，不等同于阻塞原因；只有明确的等待
或资源上限证据才会归因到瓶颈。

### QPPU 负载失衡

每个 QPPU 分别按发射指令数除以业务周期计算发射利用率。最高与最低 QPPU
差距较大时，不直接把差异归因于任务分发。分析器先对低利用率 QPPU 依次检查
Active SG、Queue Ready、局部 Eligible、实际发射活跃周期和本地资源压力，
再将一级结论归类为任务分发、FE、QPPUCtrl、EU/BE、CB 等模块。

完全没有 Active SG、Queue Ready 或实际发射的 QPPU 标记为“未参与”，不进入
负载均衡和参与范围百分比的分母。若缺少 SG 状态，分析器保守纳入该 QPPU，
不会仅凭零发射把它排除。

二级结论保留每个 QPPU 的瓶颈模块、原因、未发射周期、证据和置信度。FIFO、
Queue 或 Credit 压力若没有伴随发射下降，只标记为容量风险，不直接认定为
吞吐瓶颈。

### PC 等待归因

发射热点使用 `issue_inst_[slot].PC` 和 `instIssueType`。等待热点只在同时覆盖
队列读指针、队首条目的 PC/类型、SG 队列状态和实际发射时生成：

```text
Queue Head 等待 = Queue Ready 且本周期未发射该 SG
```

每个等待周期记录已观测到的主要阻塞原因，并从当前队列读指针定位真正的队首
条目，报告该条目的 PC、`instIssueType` 和源码已确认的 Predecode 类型标记，
例如 `isGlobalMem`、`isLDMB`、`isBarrier`。若局部 Eligible 但仍未发射，则
保留为 `eligible`，表示瓶颈位于波形未覆盖的仲裁、操作数或执行资源，而不会
猜测成某个具体原因。

对于 CB 指令，同时读取同一条 queue/issue 指令对象的
`preDecode.cb_inst_client`，按当前模型的 `CBCtrlInstClient` 枚举显示目标
客户端，例如 `4 = LDST`。这只能证明该指令需要哪个 CB 客户端；只有相应
credit、FIFO 或 inflight 上限也被波形直接覆盖时，才能进一步断言该资源已经
耗尽。

PC、粗类型和每个 Predecode 类型标记分别计算覆盖率。字段不存在时报告部分
覆盖，不把缺失字段当作 false。当前只采用已在配套模型源码中确认赋值/消费
语义、且确实位于 queue/issue 指令对象上的字段。`isFence` 已由模型维护者
确认表示 Fence 指令，因此与 `isBarrier` 等字段一样参与 issue 特征统计和
Queue Head 阻塞指令类型展示；缺失时仍按 Unknown 处理，不当作 false。

总览中的“阻塞指令 flag 分布”以全部 `Queue Ready && 未发射` 的 SG 等待
周期为分母，统计队首指令各 Predecode flag 置位时覆盖了多少等待周期；逐
QPPU 同样单独统计。报告同时给出全部等待占比、flag 已知区间内的置位占比和
覆盖率。flag 不是互斥分类，一条指令可以同时具有多个标记，所以各类占比之和
可以超过 100%；覆盖不足的区间不会按未置位处理。

### CBCtrl 数据通路

CBCtrl 页面按 FIFO 的真实读写事件复原以下链路：

```text
BE 请求 -> CBCtrl 读 uop -> EU 操作数返回
        -> CBData 指令下发 -> CBData 写回
        -> Pending 清除
```

- 请求到首条 uop：CBCtrl 读仲裁等待；
- uop 到 EU 返回：操作数读取延迟；
- 请求到首次写回：首写回延迟；
- 请求到 Pending 清除：完整 Pending 生命周期；
- 请求数与首 uop 数：请求服务率；
- 各 QPPU 服务份额相对请求份额：公平性。

FIFO `m_num_read/m_num_write` 是每周期事件数；连续两个周期都为 1 会计为两次
事件，不能只数上升沿。读事件使用读指针移动前的队首 payload，写事件使用写入
前的队尾位置和事件时刻 payload。只有 `bFirstUop` 覆盖完整时，服务率和公平性
才能用于确定性归因。

`CBData*Taken` 仅报告各目的端被选中的次数，不把它解释成 credit 余额。没有
内部仲裁阻塞枚举时，长等待只能归因到 CBCtrl 仲裁区间，不能虚构更细的具体
阻塞原因。`signal_selection_complete=false` 时仍可显示局部观测，但不会发布
全局 CBCtrl 瓶颈或逐 QPPU 确定性 CBCtrl 归因。

### FIFO 与 Queue

满率通过占用和容量的联合变化边界积分：

```text
full_rate = occupancy >= capacity 的周期 / 容量可判定周期
```

FIFO 和 Queue 分开统计。模块压力使用满率，不使用平均占用率，避免大容量
Model FIFO 的低平均占用掩盖短时背压。

资源压力页面分别列出实际 FIFO 和 Queue 实例。只有联合覆盖 occupancy 与
capacity，并且分析区间内至少出现过一个满周期的实例才进入 Top 50；不足
50 个时不补零压力条目。

Credit Counter 表示剩余可用 credit。当前只纳入已确认语义的
`mma_ldMb_credit_cnt_`、`mma_stMb_credit_cnt_` 和 `fe_dicache_credit_`：

```text
credit_exhausted_rate = available_credit == 0 的周期 / 已知周期
```

Credit Top 50 同样只列出实际出现过耗尽周期的 Counter。名字中碰巧含有
`credit` 的握手脉冲不会被当成 Credit Counter。

### Thread 有效率

在 QPPUEU 实际读取 Thread 指令时，对应 SG 的 execute mask 除以 valid mask。
按 SG 独立汇总；mask 覆盖不足时显示未覆盖，不写成 0%。

### L1/L2 带宽

带宽只统计已确认的握手和有效字节 mask。只有声明 lane 全部覆盖、mask 位宽
可映射到字节、并且峰值口径已知时才输出利用率。
拍平波形只覆盖部分 lane 时仍显示这些已观测 lane 的 B/cycle，但明确标为
`partial`，不发布全接口利用率。

- L1LSTX-L2 读：按返回 `vld + sector.vld + sector.mask` 统计有效字节。
- L1LSTX-L2 写：按写数据 `vld + wmask` 统计有效字节。
- DLS-L1 延迟：同端口非 Store 请求的 `valid && Taken` 与返回
  `valid && ready` 按顺序配对；单 outstanding 为高置信，多 outstanding
  因缺少 transaction ID 标为低置信。
- 拍平波形只计算实际存在的 `[0]` 和已覆盖 lane，不复制成完整 GPU 实测值。

只有发射明显不足且访存指令占主导，或 L1 返回延迟与明确依赖等待同时出现时，
才判为访存延迟受限。带宽接近峰值则优先判为带宽受限；仅看见 L1 流量不会
自动成为瓶颈结论。

## 正常与拍平波形

正常数组：

```text
m_QPPUTOP.[0]
m_QPPUTOP.[1]
...
```

每个元素单独分析，再向 PPU、DPPU、Cluster 和 GPU 递归聚合。

拍平数组：

```text
m_QPPUTOP[size=4].[0]
```

只分析实际存在的 `[0]`，并记录逻辑数组大小和代表性属性。代表实例不会冒充
其他实例的实测数据。正常与拍平布局共用同一分析器。

大规模波形先用节点名做语义预筛选，再只为候选信号构造完整路径。该筛选与
正式分类器使用同一组性能字段，不改变调度覆盖口径。`data.json` 使用紧凑
JSON，HTML 仍内嵌完整分析数据并可直接打开。

## 构建与自测

```powershell
msbuild QtViewer\WavePerf.vcxproj /m /p:Configuration=Release /p:Platform=x64
QtViewer\build\x64\Release\WavePerf.exe --self-test
```
