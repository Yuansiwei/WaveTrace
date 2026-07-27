# WavePerf 2026-07-27 改动交接

本文记录 2026-07-27 工作区中尚未包含在上一提交 `e60d2c3` 的改动。
目标读者是继续维护 WavePerf、WVZ4 Reader 和 Viewer 的 Agent。

开始修改前仍必须先读：

- `../AGENTS.md`
- `WavePerf_AGENT_GUIDE.md`
- `WavePerf.md`

本文是本次实现交接，不替代长期维护规范。

## 1. 本次改动总览

本次完成四组相关改动：

1. 新增 CBCtrl/CBData 专项性能分析器，接入统一 JSON、HTML、一级 finding
   和逐 QPPU 二级结论。
2. 修正 `InstIssueType::NotIssue` 的数值口径为 0。
3. 将 Predecode 指令特征列表改为分析器与测试生成器共享的单一数据源。
4. 为 `WaveSignalList = std::vector<WaveSignal>` 增加统一的 Qt `int` 边界转换，
   并让 WavePerf 工程真正使用 `/W4`。

JSON `schema_version` 从 7 升级为 8，新增顶层 `cb_ctrl`。

## 2. CBCtrl 分析器

### 2.1 代码入口

- `WavePerfCBCtrl.h/.cpp`
  - 快速候选谓词；
  - 正式路径识别；
  - FIFO 事件与 payload 提取；
  - 阶段关联、聚合、覆盖和瓶颈判定；
  - 独立自测。
- `WavePerf.cpp`
  - 在目录预筛选和正式选择中纳入 CBCtrl 字段；
  - 调用 `buildCBCtrlProfile()`；
  - 将结果写入根 JSON；
  - 调用 CBCtrl 自测。
- `WavePerfDiagnosis.cpp`
  - CBCtrl 全局 finding；
  - 逐 QPPU CBCtrl/CBData 二级归因；
  - signal selection 截断门禁。
- `WavePerfOutput.cpp`
  - 新增 CBCtrl HTML 页签。

### 2.2 识别的主链路

```text
BE 请求入口
  -> CBCtrl 寄存器读 uop
  -> EU 操作数返回
  -> CBData 指令下发
  -> CBData 写回
  -> Pending 清除
```

同时记录：

- Group 请求入口；
- CBData/L1 Credit 返回 token；
- MMA、DTF、Barrier、Group Core 等辅助下发；
- CBData instruction queue、RAM client、GR RAM、mFIFO 的 taken 活动。

`taken` 只表示资源在该周期被选择或返还，绝不能解释为可用 credit 余额。

### 2.3 FIFO 事件语义

CBCtrl 端口不能用统一上升沿计数：

- `m_num_read`、`m_num_written` 是每业务周期发生的事件数；
- 连续两个周期都保持 1，必须计为两次事件；
- 大于 1 时按该周期实际数量累计；
- 读事件使用事件发生前的 `m_ri` 和队首 payload；
- 写事件使用事件前的 `m_ri + m_num_readable` 定位队尾，并读取事件时刻
  payload；
- 分析窗口左边界缺少严格事件前状态时才允许 fallback，并分别累计
  `pointer_boundary_fallbacks` 和 `payload_boundary_fallbacks`。

事件遍历使用活跃游标优先队列，不把大周期范围展开成逐周期数组。

### 2.4 身份与关联

可用身份字段包括：

- PPU 路径；
- QPPU ID；
- SG ID；
- PC；
- CBCtrl/CBData client；
- mux；
- `bFirstUop`；
- Pending 清除的读写 ChkDep valid、index 和 dependency type。

关联按同一指令身份做 FIFO 顺序匹配，输出：

- 请求到首 uop：`arbitration_wait_*`；
- uop 到 EU 返回：`eu_read_latency_*`；
- 请求到首次写回：`first_writeback_*`；
- 请求到 Pending 清除：`pending_lifetime_*`。

没有 transaction ID，因此 JSON 明确声明
`latency_pairing_is_fifo_ordered=true`。分析窗口从在途指令中间开始时，未配对
事件会进入 warning，不能强行配对。

### 2.5 聚合维度

`cb_ctrl` 当前输出：

- `summary`：阶段事件总数、服务率、P50/P95/平均/最大延迟和未配对数；
- `stages`：通道数、事件数、event/channel-cycle、非空率和满率；
- `qppus`：请求、首 uop、服务率、需求份额、服务份额、公平性及延迟；
- `clients`：各读客户端 uop、EU 返回和返回延迟；
- `pc_hotspots`：按慢 Pending、慢仲裁、请求数排序的 Top 50；
- `pending_clear_detail`：读写 ChkDep 清除及 counter index 热点；
- `resource_taken`：CBData 资源活动；
- `coverage`：端点、事件、身份、first-uop flag 和边界覆盖；
- `bottleneck`：保守的阶段判断。

QPPU 公平性定义：

```text
fairness = 该 QPPU 的首 uop 份额 / 该 QPPU 的请求份额
```

单个 QPPU 至少有 10 个请求才发布公平性。

### 2.6 当前瓶颈阈值

判定顺序从直接背压到相关性证据：

1. 请求入口满率至少 10%、请求至少 10、first-uop flag 完整、服务率低于
   90%：请求入口瓶颈。满率至少 30% 时为 critical。
2. 请求到首 uop 至少匹配 10 条、P95 至少 8 cycle、服务率低于 95%：
   CBCtrl 读仲裁/资源等待。
3. uop 到 EU 返回至少匹配 10 条、P95 至少 16 cycle：EU 操作数返回偏慢。
4. CBData 下发或写回至少 10 个事件且满率至少 10%：CBData 下发/写回瓶颈；
   满率至少 30% 时为 critical。
5. Pending 生命周期至少匹配 10 条、P95 至少 32 cycle：Pending 完成链偏慢。
6. first-uop flag 完整且最差 QPPU 公平性低于 0.50：QPPU 仲裁公平性风险。

只有 FIFO 满状态属于直接证据。延迟和公平性属于阶段相关性证据；源码和波形
没有暴露 CBCtrl 内部 arbiter 的逐请求拒绝原因时，不得继续猜成具体端口冲突。

### 2.7 覆盖门禁

以下规则是硬约束：

- `signal_selection_complete=false` 时，局部 `cb_ctrl` 数据可以保留；
- 不得生成全局 `cb_ctrl_bottleneck` finding；
- 不得用 CBCtrl 局部样本细化逐 QPPU 主瓶颈；
- `bFirstUop` 覆盖不完整时，不用服务率和公平性做确定性归因；
- 静态初值树返回 `static_snapshot`，不制造事件和瓶颈；
- 动态波形无 CBCtrl 事件返回 `no_activity`；
- 完全没有 CBCtrl 端点返回 `not_covered`，不显示为 0%。

普通数组和拍平数组的 QPPU 路径在诊断匹配时会移除 `[size=N]`，因此
`m_QPPUTOP.[0]` 与 `m_QPPUTOP[size=4].[0]` 可关联到同一逻辑实例。不要删除
这个归一化；CBCtrl 集中端口本身不能可靠推断目录采用哪种数组表示。

### 2.8 端点去重

波形可能同时暴露 CBCtrl/PPUSData 所有者路径和 QPPUTOP 根部镜像。当前规则：

- 同一 PPU、同一端点若存在 `.m_CBCtrl.` 或 `.m_PPUSData.` 所有者路径，
  丢弃 QPPUTOP/root 镜像；
- 不复制拍平 `[0]` 为其他实例；
- 覆盖数组保留最终采用的实际路径，便于审核。

静态拍平夹具最终识别 12 个唯一端点，未出现镜像重复。

## 3. 诊断和报告变化

### 3.1 一级 finding

当 CBCtrl 的 `bottleneck.stage != none`、状态为 `measured/partial` 且全局信号
选择完整时，生成 `cb_ctrl_bottleneck`。

直接 FIFO 满证据优先级高于一般相关性证据。最终主结论仍参与原有 findings
排序，不绕开 workload、调度器、资源和带宽判断。

### 3.2 逐 QPPU 二级结论

只有上游 FE/驻留/明确调度阻塞没有先解释发射空洞时，CBCtrl 才细化原来的
`backend_unknown`：

- 服务不足或仲裁偏慢 -> `module_key=cb_ctrl`；
- Pending 生命周期偏慢 -> `CBCtrl/CBData`；
- 发射仍高但服务份额明显失衡 -> warning 风险，不抢主瓶颈。

### 3.3 HTML

CBCtrl 页签包含：

- 总判断和 6 个 KPI；
- 阶段吞吐与满率；
- 每 QPPU 服务率和公平性；
- 每客户端返回延迟；
- Pending 清除 counter；
- 慢 PC Top 50；
- CBData taken 活动；
- 覆盖、边界 fallback 和因果边界。

HTML 只消费 `data.json` 同一模型，不重新计算指标。

## 4. NotIssue 与指令特征

`InstIssueType` 的正确口径是：

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

本次新增 `kInstIssueTypeNotIssue = 0`，修正 histogram 查询和 warning 文本，并
加入自测。不要恢复旧的 `NotIssue(1)`。

`WavePerfInstructionFeatures.def` 现在是 Predecode 特征的单一数据源，同时被：

- `WavePerfArchitecture.cpp` 的分析规格；
- `tests/waveperf_layout_modes/waveperf_layout_modes_writer.cpp` 的测试信号生成

通过 X-macro 使用。新增或删除特征时先改 `.def`，不要再维护两份列表。

## 5. WaveSignalList 和编译配置

`WaveSignalList` 是 `std::vector<WaveSignal>`，Qt 容器和索引接口仍使用 `int`。
新增 `waveSignalCount()` 统一检查 `size_t -> int`：

- 超过 Qt 索引上限时立即 `qFatal`；
- Viewer、Reader、Cache Loader 中涉及 `QBitArray/QVector/QHash::reserve`
  和 signal index 的位置统一使用该函数；
- 不要重新散落无检查的 `static_cast<int>(signalList.size())`。

`WavePerf.vcxproj` 现在固定：

- `/W4`；
- angle-bracket 外部头文件 `/external:W0`；
- 工程自身警告仍完整保留。

`WaveParser4::parseFooterSection()` 的两个保留参数用 `Q_UNUSED` 明确接口状态。

## 6. 已完成验证

### 6.1 构建与自测

- Release x64：实际 `/W4`，0 warning，0 error，`self_test_ok`。
- Debug x64：实际 `/W4`，0 warning，0 error，`self_test_ok`。
- CBCtrl 自测覆盖连续两周期 `m_num_read=1`、读写 payload 边界、两条请求、
  平均仲裁 1 cycle、平均 Pending 2 cycle 和 ChkDep 清除。
- 诊断自测覆盖全局 finding、逐 QPPU 归因、normal/flat 路径归一化和截断门禁。

### 6.2 端到端布局回归

重新生成并分析 normal、flat、latency、windowed：

```text
normal:   issue utilization 92.5%
flat:     issue utilization 150.0%
latency:  workload regime memory_latency, L1 average latency 20 cycle
windowed: actual tick range [100, 800), duration 70 business cycles
```

架构事件基线：

```text
normal: FIFO read/write 8/16, Cache hit/miss 8/4
flat:   FIFO read/write 2/4, Cache hit/miss 2/1
```

以上测试波形没有 CBCtrl 端点，均保持 `cb_ctrl.status=not_covered`，原有 finding
方向未改变。

### 6.3 截断对抗

`--max-signals 40/80/120` 均满足：

- `coverage.signal_selection_complete=false`；
- workload 为 `partial_selection`；
- 第一 finding 为 `performance_coverage`；
- 无 critical；
- 无 `qppu_imbalance`、`memory_bandwidth`、`issue_underfill` 或
  `cb_ctrl_bottleneck`。

### 6.4 输出与规模

- 7 份报告的 `data.json` 和 `index.html` 无 U+FFFD；
- 7 份 HTML 的全部内嵌脚本均通过 JavaScript 解析；
- v15 完整 QPPU 结构波形选中 16,920 个相关信号，约 2.0 秒完成；
- 静态拍平 GPU 夹具选中 1,284 个相关信号，CBCtrl 端点 12 个、路径全唯一、
  `bottleneck=none`；
- 37.9 MB 的百万周期旧文件是 WVZ4 v13，按当前无兼容负担策略被拒绝；
- 通用 1 万信号压力文件没有 GPU 性能节点，按设计返回
  `no supported GPU performance signals`。

生成的 `.wvz4`、`.perf/` 和 `build/` 未纳入提交。

## 7. 已知边界

1. 当前只支持 WVZ4 v15。
2. CBCtrl 内部 arbiter 的 eligible/block reason 未出现在现有波形，不能报告
   更细的确定性仲裁原因。
3. 阶段延迟按指令身份和 FIFO 顺序关联，不等价于 transaction ID 精确配对。
4. 实际可用的完整 CBCtrl 大树波形是静态快照；动态事件算法主要由内存自测
   验证。拿到动态 CBCtrl v15 波形后，应优先补一份可提交的小夹具。
5. `CBData*Taken` 不是 credit 余额；真正 credit 满率仍要求“可用 credit=0”。
6. CBCtrl 阶段阈值是首版保守经验值。调整阈值必须同时更新本文、自测和
   normal/flat/latency/windowed/截断回归。

## 8. 后续修改清单

修改 CBCtrl 时至少同步检查：

1. `isCBCtrlDetailLeafName()` 快速候选；
2. `isCBCtrlDetailSignalPath()` 正式选择；
3. owner/mirror 去重；
4. 读写事件前后状态；
5. Unknown、partial 和 signal selection 门禁；
6. normal/flat QPPU 路径归一化；
7. JSON schema 和 HTML；
8. `cbCtrlProfilerSelfTest()` 与 diagnosis/output self-test；
9. Release/Debug `/W4`；
10. 四类布局报告和 40/80/120 截断对抗。
