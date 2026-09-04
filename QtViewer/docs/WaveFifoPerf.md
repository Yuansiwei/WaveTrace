# WaveFifoPerf 通用 FIFO / Queue 满率分析器

WaveFifoPerf 从 WavePerf 中抽取 FIFO/Queue 满率功能，只输出资源压力相关网页，不依赖
GPU、QPPU、PC、SG 或其他架构路径。工具扫描 WVZ4 目录的对象字段，确认对象后只
解码占用量与容量，不读取 FIFO 数据内容。

## 识别规则

同一对象必须包含 `m_size`，并且恰好包含下列一个占用字段：

- `m_num_readable`：FIFO
- `m_numAvail`：FIFO
- `m_count`：Queue

`m_num_read`、`m_num_written`、`m_ri`、`m_wi` 等字段不参与识别，也不会加载
其采样值。缺字段、重复字段或同时出现多个占用字段的对象进入“未确认候选”，工具
不会猜测。

拍平数组的 `[size=N].[0]` 只表示一个代表性观测，不乘以 `N`。

## 满率语义

区间内 `occupancy >= capacity` 视为满。只有 occupancy 与 capacity 同时已知且
`capacity > 0` 的 tick 进入分母。稳定且没有跳变的已知值继续按区间积分；未知值
不按 0 处理。

每个资源先取 occupancy/capacity 两路事件合并后的首、末事件，将用户指定分析
区间裁剪到这个事件窗口。末事件之后的稳定状态不再延长进分母；首末事件相同的
静态资源没有可计时区间，满率保持未知。

## 使用

```powershell
QtViewer\build\x64\Release\WaveFifoPerf.exe wave.wvz4
```

默认生成 `wave.fifo.perf/index.html` 与 `data.json`。常用选项：

```powershell
WaveFifoPerf.exe wave.wvz4 `
  --start-cycle 0 --end-cycle 100000 `
  --ticks-per-cycle 10 `
  --out fifo_report
```

默认在终端显示目录扫描与选定信号解码的百分比进度条；`--no-progress` 可关闭。

网页名称列只显示 FIFO/Queue 对象自身的完整路径，不显示 `m_size` 或占用信号的
子路径。排序下拉框可在满率和平均占用率之间切换；平均占用率定义为分析区间内
occupancy 加权积分除以 capacity 加权积分。
搜索框按 FIFO/Queue 对象的完整路径实时过滤，搜索与当前排序方式同时生效。

实现上先读取便宜的叶子名，只有命中候选字段才重建完整路径。满率积分直接用
occupancy/capacity 双游标扫描，不再生成两路采样时间的合并边界数组。

## 回归

```powershell
QtViewer\build\x64\Release\WaveFifoPerf.exe --self-test
tests\wavefifo_layout_modes\run.ps1
```

布局回归生成 normal 与 flat 两个 WVZ4，分别验证两个 FIFO、一条 Queue、
三种字段签名、30%/50%/60% 满率、两个显式拒绝候选、只解码 6 个采样信号，
以及网页内嵌 JSON。
