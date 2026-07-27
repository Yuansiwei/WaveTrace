# QtViewer Agent Instructions

## WavePerf

修改 `WavePerf*`、`WaveParser4Reader::loadSignals()` 或性能报告格式之前，必须先读：

- `docs/WavePerf_AGENT_GUIDE.md`：实现、指标、覆盖、诊断和回归的维护规范。
- `docs/WavePerf.md`：面向使用者的命令行和指标说明。
- `docs/WavePerf_CHANGES_20260727.md`：CBCtrl、NotIssue、指令特征单一数据源及
  本轮回归结果的交接记录。

维护 WavePerf 时必须遵守以下硬约束：

1. Unknown 不是 0；缺信号、未知区间和信号选择截断必须继续传播为部分覆盖。
2. 没有跳变不等于没有活动。稳定保持为 1 的 valid、mask 或计数值仍要按区间积分。
3. `issue_inst_[0]/[1]` 是 Main/Shadow 位置，不是固定的 Thread/Group 类型槽。
4. 发射利用率按实际发射条数累计，双发周期加 2，允许超过 100%。
5. 事件信号按已确认语义积分，不得统一改成上升沿计数。
6. 拍平数组的 `[0]` 是代表性观测，不得复制成其他元素的实测结果。
7. `signal_selection_complete=false` 时，不得发布全局吞吐、失衡、带宽或主瓶颈结论。
8. FIFO/Queue 满率必须同时覆盖 occupancy 与 capacity；Credit 满率使用可用 Credit 为 0。
9. 局部 Eligible 不是最终可发射证明；实际 issue 才能证明最终发射成功。
10. 新增分类规则时同时更新快速候选筛选、正式分类、自测和布局回归。

提交前至少运行：

```powershell
msbuild WavePerf.vcxproj /m /p:Configuration=Release /p:Platform=x64
build\x64\Release\WavePerf.exe --self-test
msbuild WavePerf.vcxproj /m /p:Configuration=Debug /p:Platform=x64
build\x64\Debug\WavePerf.exe --self-test
```

涉及数组路径、事件语义、覆盖率或诊断规则时，还必须生成 normal、flat、latency
三类测试波形，并检查 `data.json` 与 `index.html`。不要提交 `build/`、`.wvz4`
或生成的 `.perf/` 目录。
