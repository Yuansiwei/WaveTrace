#include "WavePerfOutput.h"

#include "WavePerfDiagnosis.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>

namespace waveperf {
namespace {

bool writeFile(const QString& path,
               const QByteArray& content,
               QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("无法写入 %1").arg(path);
        return false;
    }
    if (file.write(content) != content.size()) {
        error = QStringLiteral("写入不完整：%1").arg(path);
        return false;
    }
    return true;
}

QByteArray htmlDocument(const QJsonObject& model) {
    QByteArray json = QJsonDocument(model).toJson(QJsonDocument::Compact);
    json.replace("</script", "<\\/script");
    const QByteArray prefix = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WavePerf 性能报告</title>
<style>
:root{color-scheme:light;--bg:#f4f6f8;--surface:#fff;--line:#d8dee5;--text:#18212b;--muted:#66717e;--blue:#1769aa;--green:#18794e;--amber:#a15c00;--red:#b42318}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.5 "Microsoft YaHei UI","Segoe UI",sans-serif;letter-spacing:0}
header{background:#15202b;color:#fff;padding:18px 28px 15px}h1{font-size:22px;margin:0 0 4px;font-weight:650}header p{margin:0;color:#c9d4df;overflow-wrap:anywhere}
nav{display:flex;gap:4px;padding:0 28px;background:#15202b;border-top:1px solid #34414e;overflow:auto}
nav button{border:0;border-bottom:3px solid transparent;background:transparent;color:#c9d4df;padding:11px 14px;white-space:nowrap;cursor:pointer;font:inherit}
nav button.active{color:#fff;border-bottom-color:#52a9e8}main{max-width:1500px;min-width:0;margin:0 auto;padding:22px 28px 40px}
.view{display:none;min-width:0}.view.active{display:block}.section{min-width:0;background:var(--surface);border:1px solid var(--line);border-radius:6px;margin:0 0 16px;padding:18px}
h2{font-size:17px;margin:0 0 13px}h3{font-size:14px;margin:18px 0 8px}.lead{font-size:17px;margin:0}.muted{color:var(--muted)}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:6px;overflow:hidden;margin-bottom:16px}
.kpi{background:#fff;padding:14px 15px;min-height:82px}.kpi b{display:block;font-size:23px;font-weight:650;margin-top:5px}.kpi span{color:var(--muted)}
.finding{border-left:4px solid var(--blue);padding:10px 13px;margin:9px 0;background:#f7fafc}.finding.warning{border-color:var(--amber)}.finding.critical{border-color:var(--red)}.finding strong{display:block;margin-bottom:2px}.finding p{margin:3px 0}.next{color:var(--muted)}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}th,td{text-align:left;padding:8px 9px;border-bottom:1px solid #e5e9ed;vertical-align:top}th{background:#f6f8fa;color:#46515d;font-weight:600;position:sticky;top:0}tbody tr:hover{background:#f8fbfd}.scroll{overflow:auto;max-height:620px;border:1px solid var(--line)}
.bar{height:8px;background:#e5e9ed;min-width:100px}.bar i{display:block;height:100%;background:var(--blue);max-width:100%}.bar.warn i{background:var(--amber)}.bar.bad i{background:var(--red)}
canvas{display:block;width:100%;height:270px;border:1px solid var(--line);background:#fff}
.legend{display:flex;gap:17px;flex-wrap:wrap;margin:10px 0;color:var(--muted)}.legend i{display:inline-block;width:12px;height:3px;margin:0 5px 3px 0}
details{border-bottom:1px solid var(--line);padding:8px 0}summary{cursor:pointer;font-weight:600}.path{font-family:Consolas,monospace;font-size:12px;color:var(--muted);overflow-wrap:anywhere}
.pill{display:inline-block;padding:1px 6px;border:1px solid var(--line);border-radius:4px;font-size:12px;color:var(--muted)}
 .pill.bottleneck{border-color:#e6a6a0;color:var(--red)}.pill.risk{border-color:#d9b878;color:var(--amber)}.pill.healthy{border-color:#91c8ad;color:var(--green)}.pill.inactive{color:var(--muted);background:#f1f3f5}
@media(max-width:700px){header,nav{padding-left:14px;padding-right:14px}main{padding:14px}.section{padding:13px}.kpis{grid-template-columns:repeat(2,minmax(0,1fr))}.kpi b{font-size:19px}th,td{padding:7px 6px}}
</style>
</head>
<body>
<header><h1>WavePerf 性能报告</h1><p id="meta"></p></header>
<nav id="tabs">
<button data-view="overview" class="active">总览</button>
<button data-view="issue">发射 / Thread</button>
<button data-view="scheduler">调度器</button>
<button data-view="timeline">时间线</button>
<button data-view="architecture">架构树</button>
<button data-view="resources">资源压力</button>
<button data-view="memory">L1 / L2</button>
<button data-view="hotspots">PC 热点</button>
</nav>
<main>
<section id="overview" class="view active"></section>
<section id="issue" class="view"></section>
<section id="scheduler" class="view"></section>
<section id="timeline" class="view"></section>
<section id="architecture" class="view"></section>
<section id="resources" class="view"></section>
<section id="memory" class="view"></section>
<section id="hotspots" class="view"></section>
</main>
<script>const DATA=)HTML";
    const QByteArray suffix = R"HTML(;
const $=id=>document.getElementById(id);
const esc=v=>String(v??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const num=(v,d=1)=>Number.isFinite(Number(v))?Number(v).toLocaleString("zh-CN",{maximumFractionDigits:d}):"-";
const pct=v=>num(v,1)+"%";
const bar=(v,kind="")=>`<div class="bar ${kind}"><i style="width:${Math.max(0,Math.min(100,Number(v)||0))}%"></i></div>`;
const table=(headers,rows)=>`<div class="scroll"><table><thead><tr>${headers.map(x=>`<th>${x}</th>`).join("")}</tr></thead><tbody>${rows.join("")}</tbody></table></div>`;
document.querySelectorAll("#tabs button").forEach(button=>button.onclick=()=>{
  document.querySelectorAll("#tabs button,.view").forEach(x=>x.classList.remove("active"));
  button.classList.add("active");$(button.dataset.view).classList.add("active");
  if(button.dataset.view==="timeline")drawTimeline();
});
 const file=DATA.file||{},analysis=DATA.analysis||{},sum=DATA.summary||{},sched=DATA.scheduler||{},ss=sched.summary||{},wp=DATA.workload_profile||{};
 const schedCovered=sched.status==="measured"||sched.status==="partial";
 const scoped=["measured","partial"].includes(wp.status)&&Number(wp.participating_qppus)>0;
 const selectionCovered=wp.selection_coverage_complete!==false;
 const issueCovered=selectionCovered&&(scoped?wp.issue_coverage_complete===true:sum.issue_activity_coverage_complete===true);
 const activityCovered=selectionCovered&&(scoped?wp.activity_coverage_complete===true:ss.activity_coverage_complete===true);
 const eligibilityCovered=selectionCovered&&(scoped?wp.eligibility_coverage_complete===true:ss.eligibility_coverage_complete===true);
$("meta").textContent=`${file.path||""} · ${num(analysis.duration_cycles,2)} 业务周期 · ${num(file.signals,0)} 个信号`;
const findings=DATA.findings||[];
const primary=findings[0]||{};
const primaryHtml=primary.title?`<p class="lead"><strong>${esc(primary.title)}</strong></p><p>${esc(primary.conclusion)}</p><p><b>依据：</b>${esc(primary.evidence)}</p><p class="next"><b>动作：</b>${esc(primary.next_step)}</p>`:`<p class="lead">${esc(DATA.conclusion||"没有可用结论")}</p>`;
const secondary=findings.slice(1);
const secondaryHtml=secondary.map(f=>`<div class="finding ${esc(f.severity)}"><strong>${esc(f.title)}</strong><p><b>依据：</b>${esc(f.evidence)}</p><p class="next"><b>动作：</b>${esc(f.next_step)}</p></div>`).join("");
const qppuConclusions=DATA.qppu_conclusions||[];
const qppuConclusionByPath=new Map(qppuConclusions.map(q=>[String(q.path||""),q]));
 const stateName=s=>({bottleneck:"瓶颈",risk:"风险",healthy:"正常",uncovered:"未覆盖",inactive:"未参与"}[s]||s||"-");
 const qppuConclusionRows=qppuConclusions.map(q=>{const issue=q.issue_coverage_complete===true;return `<tr><td>QPPU ${num(q.qppu_index,0)}<br><span class="path">${esc(q.path)}</span></td><td><span class="pill ${esc(q.state)}">${esc(stateName(q.state))}</span></td><td>${esc(q.module)}</td><td><b>${esc(q.title)}</b></td><td>${esc(q.reason)}</td><td>${issue?num(q.issue_idle_cycles,2):"部分覆盖"}</td><td>${issue?pct(q.issue_active_percent):"部分覆盖"}</td><td>${issue?pct(q.issue_utilization_percent):"部分覆盖"}</td><td>${esc(q.confidence)}</td></tr>`});
 const kpis=[
   ["发射利用率",issueCovered?pct(scoped?wp.issue_utilization_percent:sum.issue_utilization_percent):"部分覆盖"],
   ["IPC",issueCovered?num(sum.ipc_observed,3):"部分覆盖"],
  ["Active SG",activityCovered?pct(scoped?wp.active_percent:ss.active_percent):schedCovered?"部分覆盖":"未覆盖"],
  ["Queue Ready",activityCovered?pct(scoped?wp.queue_ready_percent:ss.queue_ready_percent):schedCovered?"部分覆盖":"未覆盖"],
  ["局部 Eligible",eligibilityCovered?pct(scoped?wp.eligible_percent:ss.eligible_percent):schedCovered?"部分覆盖":"未覆盖"],
  ["Thread 有效率",!selectionCovered?"部分覆盖":DATA.sg_thread_efficiency?.overall_thread_efficiency_percent==null?"未覆盖":pct(DATA.sg_thread_efficiency.overall_thread_efficiency_percent)]
 ];
 const scopeNote=!selectionCovered?"性能信号选择达到上限；局部实例数据可查看，但全局 KPI 和瓶颈判型均按部分覆盖处理。":(wp.status==="static_snapshot"?"静态快照只保留边界状态，不计算动态参与范围。":`参与 ${num(wp.participating_qppus,0)} / 观测 ${num(wp.observed_qppus,0)} 个 QPPU；未参与实例不进入负载均衡分母。`);
 const workloadHtml=wp.title?`<div class="section"><h2>工作负载判型 <span class="pill">${esc(wp.confidence)}置信</span></h2><p class="lead"><strong>${esc(wp.scope_name)} · ${esc(wp.title)}</strong></p><p>${esc(wp.conclusion)}</p><p><b>证据：</b>${esc(wp.evidence)}</p><p class="muted">${esc(scopeNote)}</p></div>`:"";
 $("overview").innerHTML=`<div class="section"><h2>一级结论</h2>${primaryHtml}</div>${workloadHtml}<div class="kpis">${kpis.map(k=>`<div class="kpi"><span>${k[0]}</span><b>${k[1]}</b></div>`).join("")}</div>${qppuConclusionRows.length?`<div class="section"><h2>二级结论：逐 QPPU</h2>${table(["实例","状态","瓶颈模块","判断","原因","未发射周期","发射活跃","发射利用率","置信度"],qppuConclusionRows)}</div>`:""}${secondary.length?`<div class="section"><h2>全局补充证据</h2>${secondaryHtml}</div>`:""}<div class="section"><h2>覆盖</h2><p>信号选择 ${selectionCovered?"完整":"截断"} · QPPU ${num(sched.qppu_count,0)} · SG ${num(ss.fully_covered_shader_groups,0)}/${num(ss.shader_groups,0)} · 发生跳变信号 ${num(analysis.dynamic_signals,0)}</p>${sched.eligibility_definition?`<details><summary>Eligible 判据</summary><p class="muted">${esc(sched.eligibility_definition)}</p></details>`:""}</div>`;
function flattenArchitecture(nodes,depth=0,out=[]){(nodes||[]).forEach(n=>{out.push({n,depth});flattenArchitecture(n.children,depth+1,out)});return out}
const allArch=flattenArchitecture(DATA.architecture?.roots||[]);
const className=k=>({thread:"Thread",group:"Group",cb:"CB",mma:"MMA"}[k]||k||"Unknown");
const issueClassRows=(sum.issue_classes||[]).map(c=>`<tr><td>${esc(className(c.key))}</td><td>${num(c.issued_cycles,2)}</td><td>${c.covered?pct(c.qppu_cycle_rate_percent):"部分覆盖"}</td><td>${c.covered?pct(c.instruction_share_percent)+bar(c.instruction_share_percent):"部分覆盖"}</td><td>${c.covered?"完整":"部分"}</td></tr>`);
 const issueSlotRows=(sum.issue_slots||[]).map(s=>`<tr><td>${num(s.index,0)}</td><td>${num(s.issued_cycles,2)}</td><td>${num(s.observed_cycles,2)}</td><td>${s.covered?pct(s.utilization_percent)+bar(s.utilization_percent):"部分覆盖"}</td><td>${s.covered?"是":"否"}</td></tr>`);
const pairRows=(sum.dual_issue_pairs||[]).map(p=>`<tr><td>${esc(className(p.main_class))}</td><td>${esc(className(p.shadow_class))}</td><td>${num(p.cycles,2)}</td><td>${p.covered?pct(p.dual_issue_share_percent)+bar(p.dual_issue_share_percent):"部分覆盖"}</td></tr>`);
const threadEntries=[...(DATA.sg_thread_efficiency?.entries||[])].sort((a,b)=>(Number(a.thread_efficiency_percent)||101)-(Number(b.thread_efficiency_percent)||101));
const threadRows=threadEntries.map(t=>`<tr><td>QPPU ${num(t.qppu_index,0)}<br><span class="path">${esc(t.qppu_path)}</span></td><td>${num(t.sg_index,0)}</td><td>${esc(t.thread_instructions)}</td><td>${t.valid_thread_occupancy_percent==null?"未覆盖":pct(t.valid_thread_occupancy_percent)}</td><td>${t.active_thread_efficiency_percent==null?"未覆盖":pct(t.active_thread_efficiency_percent)}</td><td>${t.thread_efficiency_percent==null?"未覆盖":pct(t.thread_efficiency_percent)+bar(t.thread_efficiency_percent,t.thread_efficiency_percent<70?"warn":"")}</td></tr>`);
const euRows=allArch.filter(x=>x.n.aggregate?.eu).map(({n})=>{const e=n.aggregate.eu||{},run=e.execution||{},input=e.instruction_input||{},phase=e.phase1_request||{};return `<tr><td>${esc(n.class)}</td><td class="path">${esc(n.path)}</td><td>${run.utilization_percent==null?"未覆盖":pct(run.utilization_percent)}</td><td>${esc(run.instructions_received??"-")}</td><td>${esc(run.instructions_executed??"-")}</td><td>${input.nonempty_rate_percent==null?"未覆盖":pct(input.nonempty_rate_percent)}</td><td>${input.full_rate_percent==null?"未覆盖":pct(input.full_rate_percent)}</td><td>${phase.pending_rate_percent==null?"未覆盖":pct(phase.pending_rate_percent)}</td></tr>`});
$("issue").innerHTML=`<div class="section"><h2>发射类型</h2>${table(["类型","发射指令周期","QPPU 周期率","指令占比","覆盖"],issueClassRows)}</div><div class="section"><h2>Main / Shadow 位置</h2>${table(["位置","发射周期","观测周期","占用率","覆盖"],issueSlotRows)}</div><div class="section"><h2>双发组合</h2>${table(["Main 类型","Shadow 类型","周期","双发占比"],pairRows.length?pairRows:["<tr><td colspan=4>没有观测到已分类双发组合</td></tr>"])}</div><div class="section"><h2>每 SG Thread 有效率</h2>${table(["QPPU","SG","Thread 指令","Valid 占用","Active / Valid","Execute / Valid"],threadRows.length?threadRows:["<tr><td colspan=6>Thread mask 或 Thread 指令流未覆盖</td></tr>"])}</div><div class="section"><h2>QPPU EU</h2>${table(["模块","路径","执行利用率","接收","执行","输入非空率","输入满率","Phase1 Pending"],euRows.length?euRows:["<tr><td colspan=8>EU 输入/执行信号未覆盖</td></tr>"])}</div>`;
const reasonRows=(ss.block_reasons||[]).map(r=>`<tr><td>${esc(r.name)}</td><td>${num(r.cycles,2)}</td><td>${eligibilityCovered?pct(r.queue_ready_percent)+bar(r.queue_ready_percent,r.queue_ready_percent>=30?"bad":r.queue_ready_percent>=10?"warn":""):"部分覆盖"}</td></tr>`);
 const qppuRows=(sched.qppus||[]).map(q=>{const d=qppuConclusionByPath.get(String(q.path||""))||{},issue=q.issue_coverage_complete===true,activity=q.activity_coverage_complete===true,eligibility=q.eligibility_coverage_complete===true,issueUtil=issue?pct(q.issue_utilization_percent)+bar(q.issue_utilization_percent,q.issue_utilization_percent<50?"warn":""):"部分覆盖";return `<tr><td>QPPU ${num(q.index,0)}</td><td>${esc(d.module||"未覆盖")}<br><span class="muted">${esc(d.title||"")}</span></td><td>${num(q.issued_instructions_estimate,2)}</td><td>${issueUtil}</td><td>${issue?pct(q.issue_active_percent):"部分覆盖"}</td><td>${issue?num(q.issue_idle_cycles,2):"部分覆盖"}</td><td>${activity?num(q.active_sg_cycles,2):"部分覆盖"}</td><td>${activity?num(q.queue_ready_sg_cycles,2):"部分覆盖"}</td><td>${eligibility?num(q.eligible_sg_cycles,2):"部分覆盖"}</td><td>${eligibility?pct(q.eligible_percent):"部分覆盖"}</td><td class="path">${esc(q.path)}</td></tr>`});
const sgRows=[];(sched.qppus||[]).forEach(q=>(q.shader_groups||[]).forEach(s=>sgRows.push(`<tr><td>QPPU ${num(q.index,0)}<br><span class="path">${esc(q.path)}</span></td><td>${num(s.index,0)}</td><td>${s.activity_coverage_complete?num(s.active_cycles,2):"部分覆盖"}</td><td>${s.activity_coverage_complete?pct(s.queue_ready_percent):"部分覆盖"}</td><td>${s.eligibility_coverage_complete?pct(s.eligible_percent):"部分覆盖"}</td><td>${num(s.issued_cycles,2)}</td><td>${s.eligibility_coverage_complete?"完整":"部分"}</td></tr>`)));
$("scheduler").innerHTML=`<div class="section"><h2>调度器总览</h2>${table(["阻塞原因","SG-cycle","Queue Ready 占比"],reasonRows.length?reasonRows:["<tr><td colspan=3>没有观测到已分类阻塞</td></tr>"])}</div><div class="section"><h2>每个 QPPU</h2>${table(["实例","二级结论","发射指令","发射利用率","发射活跃","未发射周期","Active SG-cycle","Queue Ready SG-cycle","Eligible SG-cycle","Eligible 率","路径"],qppuRows)}</div><div class="section"><h2>每个 SG</h2>${table(["QPPU","SG","Active 周期","Queue Ready","Eligible","发射周期","覆盖"],sgRows)}</div>`;
)HTML";
    const QByteArray suffix2 = R"HTML(
let timelineDrawn=false;
function drawTimeline(){
 if(timelineDrawn)return;timelineDrawn=true;
 const data=sched.timeline||[];$("timeline").innerHTML=`<div class="section"><h2>调度吞吐时间线</h2><canvas id="chart" width="1400" height="270"></canvas><div class="legend"><span><i style="background:#1769aa"></i>Active SG</span><span><i style="background:#18794e"></i>Queue Ready SG</span><span><i style="background:#a15c00"></i>Eligible SG</span><span><i style="background:#b42318"></i>Issued SG</span></div><p class="muted">纵轴为每个时间桶内的平均 SG 数。用于找阶段性退化，不把桶内样本展开成逐周期数据。</p></div>`;
 if(!data.length)return;
 const c=$("chart"),ctx=c.getContext("2d"),w=c.width,h=c.height,p=36;
 const keys=[["active_sg_cycles","#1769aa"],["queue_ready_sg_cycles","#18794e"],["eligible_sg_cycles","#a15c00"],["issued_sg_cycles","#b42318"]];
 const value=(x,key)=>(Number(x[key])||0)/Math.max(1e-12,Number(x.end_cycle)-Number(x.start_cycle));
 const max=Math.max(1,...data.flatMap(x=>keys.map(k=>value(x,k[0]))));
 ctx.strokeStyle="#d8dee5";ctx.beginPath();ctx.moveTo(p,10);ctx.lineTo(p,h-p);ctx.lineTo(w-8,h-p);ctx.stroke();
 keys.forEach(([key,color])=>{ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();data.forEach((x,i)=>{const px=p+(w-p-10)*(i/Math.max(1,data.length-1));const py=h-p-(h-p-18)*(value(x,key)/max);i?ctx.lineTo(px,py):ctx.moveTo(px,py)});ctx.stroke()});
 ctx.fillStyle="#66717e";ctx.font="12px Segoe UI";ctx.fillText(num(max,1),2,16);ctx.fillText(num(data[0].start_cycle,1),p,h-10);ctx.fillText(num(data[data.length-1].end_cycle,1),w-90,h-10);
}
 const archRows=allArch.map(({n,depth})=>{const a=n.aggregate||{},fifo=Number(a.fifo_full_resources)>0?pct(a.fifo_full_rate_percent):"未覆盖",queue=Number(a.queue_full_resources)>0?pct(a.queue_full_rate_percent):"未覆盖",issue=Number(a.issue_contexts)>0?(a.issue_coverage_complete?pct(a.issue_utilization_percent):"部分覆盖"):"未覆盖";return `<tr><td style="padding-left:${8+depth*18}px">${esc(n.class||n.class_key)}</td><td class="path">${esc(n.path)}</td><td>${num(n.represented_instances,0)}</td><td>${issue}</td><td>${fifo}</td><td>${queue}</td><td>${num(a.stall_signal_cycles,2)}</td></tr>`});
$("architecture").innerHTML=`<div class="section"><h2>递归架构利用率</h2><p class="muted">父节点为子树聚合值；展开的数组按报告中的 represented instances 解释。</p>${table(["模块","路径","代表实例","发射利用率","FIFO 满率","Queue 满率","Stall 信号周期"],archRows)}</div>`;
const pressure=n=>Math.max(Number(n.aggregate?.fifo_full_rate_percent)||0,Number(n.aggregate?.queue_full_rate_percent)||0);
const resourceEntries=[...allArch].map(({n})=>({n,a:n.aggregate||{}})).filter(({a})=>Number(a.fifo_full_rate_percent)>0||Number(a.queue_full_rate_percent)>0||Number(a.stall_signal_cycles)>0||Number(a.pending_signal_cycles)>0||Number(a.cache_hit_events)>0||Number(a.cache_miss_events)>0).sort((x,y)=>pressure(y.n)-pressure(x.n)||Number(y.a.stall_signal_cycles)-Number(x.a.stall_signal_cycles)||Number(y.a.pending_signal_cycles)-Number(x.a.pending_signal_cycles));
const resourceRows=resourceEntries.map(({n,a})=>{const fifo=Number(a.fifo_full_resources)>0?pct(a.fifo_full_rate_percent)+bar(a.fifo_full_rate_percent,a.fifo_full_rate_percent>=30?"bad":""):"未覆盖",queue=Number(a.queue_full_resources)>0?pct(a.queue_full_rate_percent)+bar(a.queue_full_rate_percent,a.queue_full_rate_percent>=30?"bad":""):"未覆盖",cache=a.cache_rate_coverage_complete&&Number(a.cache_hit_events)+Number(a.cache_miss_events)>0?pct(a.cache_hit_rate_percent):"未覆盖";return `<tr><td>${esc(n.class)}</td><td class="path">${esc(n.path)}</td><td>${fifo}</td><td>${queue}</td><td>${num(a.stall_signal_cycles,2)}</td><td>${num(a.pending_signal_cycles,2)}</td><td>${cache}</td></tr>`});
const resourcePressure=DATA.resource_pressure||{};
function fullResourceTop(title,section){section=section||{};const rows=(section.top||[]).map((r,i)=>`<tr><td>${i+1}</td><td class="path">${esc(r.path)}</td><td>${pct(r.full_rate_percent)}${bar(r.full_rate_percent,r.full_rate_percent>=30?"bad":"")}</td><td>${num(r.full_cycles,2)}</td><td>${num(r.observed_cycles,2)}</td><td>${num(r.average_occupancy,2)}</td><td>${num(r.average_capacity,2)}</td></tr>`),covered=Number(section.covered_resources)||0,incomplete=Number(section.incomplete_resources)||0,pressured=Number(section.pressured_resources)||0,message=covered===0?`未完整覆盖实际 ${title}`:`完整覆盖 ${covered} 个，未观测到满状态`;return `<div class="section"><h2>${title} 满率 Top 50 <span class="pill">${pressured} 命中 / ${covered} 完整 / ${incomplete} 不完整</span></h2>${table(["排名","实例路径","满率","满周期","覆盖周期","平均占用","平均容量"],rows.length?rows:[`<tr><td colspan=7>${message}</td></tr>`])}</div>`}
function creditResourceTop(section){section=section||{};const rows=(section.top||[]).map((r,i)=>`<tr><td>${i+1}</td><td class="path">${esc(r.path)}</td><td>${pct(r.exhausted_rate_percent)}${bar(r.exhausted_rate_percent,r.exhausted_rate_percent>=30?"bad":"")}</td><td>${num(r.exhausted_cycles,2)}</td><td>${num(r.observed_cycles,2)}</td><td>${num(r.average_available,2)}</td><td>${esc(r.minimum_available)}</td><td>${esc(r.maximum_available)}</td></tr>`),covered=Number(section.covered_resources)||0,incomplete=Number(section.incomplete_resources)||0,pressured=Number(section.pressured_resources)||0,message=covered===0?"未完整覆盖已确认语义的 Credit Counter":`完整覆盖 ${covered} 个，未观测到 Credit 耗尽`;return `<div class="section"><h2>Credit 耗尽率 Top 50 <span class="pill">${pressured} 命中 / ${covered} 完整 / ${incomplete} 不完整</span></h2>${table(["排名","Counter 路径","耗尽率","耗尽周期","覆盖周期","平均可用","最小可用","最大可用"],rows.length?rows:[`<tr><td colspan=8>${message}</td></tr>`])}</div>`}
$("resources").innerHTML=`${fullResourceTop("FIFO",resourcePressure.fifo)}${fullResourceTop("Queue",resourcePressure.queue)}${creditResourceTop(resourcePressure.credit)}<div class="section"><h2>模块聚合压力</h2>${table(["模块","路径","FIFO 满率","Queue 满率","Stall 周期","Pending 周期","Cache 命中率"],resourceRows.length?resourceRows:["<tr><td colspan=7>没有可判定的资源满率或背压证据</td></tr>"])}</div>`;
function direction(label,d){d=d||{};return `<tr><td>${label}</td><td>${esc(d.status||"unavailable")}</td><td>${d.bytes_per_cycle==null?"-":num(d.bytes_per_cycle,3)}</td><td>${d.peak_bytes_per_cycle==null?"-":num(d.peak_bytes_per_cycle,3)}</td><td>${d.utilization_percent==null?"未覆盖":pct(d.utilization_percent)+bar(d.utilization_percent,d.utilization_percent>=80?"bad":"")}</td><td>${esc(d.basis||d.reason||"")}</td></tr>`}
const l1=DATA.memory_bandwidth?.l1||{},l2=DATA.memory_bandwidth?.l2||{};
const lat=l1.latency||{},latencyRows=lat.available?[`<tr><td>${num(lat.request_count,0)}</td><td>${num(lat.matched_transactions,0)}</td><td>${num(lat.average_cycles,2)}</td><td>${num(lat.p50_cycles,2)}</td><td>${num(lat.p95_cycles,2)}</td><td>${num(lat.maximum_cycles,2)}</td><td>${num(lat.average_outstanding,2)}</td><td>${num(lat.maximum_outstanding,0)}</td><td>${lat.coverage_complete?"完整":"部分"}</td><td>${esc(lat.confidence)}</td></tr>`]:[`<tr><td colspan=10>${esc(lat.reason||"请求/返回握手未覆盖")}</td></tr>`];
$("memory").innerHTML=`<div class="section"><h2>L1 / L2 有效带宽</h2>${table(["通路","状态","B/cycle","峰值 B/cycle","利用率","口径"],[direction("L1 读",l1.read),direction("L1 写",l1.write),direction("L2 读",l2.read),direction("L2 写",l2.write)])}<p class="muted">只有通道与 mask 覆盖足以建立峰值口径时才显示利用率，缺失数据不会按 0% 处理。</p></div><div class="section"><h2>DLS - L1 请求返回延迟</h2>${table(["请求","已配对","平均周期","P50","P95","最大","平均并发","最大并发","覆盖","置信度"],latencyRows)}<p class="muted">${esc(lat.basis||"")}</p></div>`;
 const issueType=v=>v==="unknown"||v==null?"未知":({2:"Thread",3:"Thread",4:"Group",5:"Group",6:"CB",7:"CB",8:"MMA",9:"MMA"}[Number(v)]||`类型 ${v}`);
const hotRows=(sched.pc_hotspots||[]).map((h,i)=>`<tr><td>${i+1}</td><td>${esc(h.pc)}</td><td>${esc(issueType(h.issue_type))}</td><td>${num(h.issued_instructions_estimate,2)}</td><td>${pct(h.share_percent)}${bar(h.share_percent)}</td><td>${num(h.queue_wait_cycles,2)}</td><td class="path">${esc(h.qppu_path)}</td></tr>`);
const waitRows=(sched.pc_wait_hotspots||[]).map((h,i)=>`<tr><td>${i+1}</td><td>${esc(h.pc)}</td><td>${esc(issueType(h.issue_type))}</td><td>${num(h.wait_cycles,2)}</td><td>${pct(h.share_percent)}${bar(h.share_percent,h.share_percent>=30?"warn":"")}</td><td>${esc(h.dominant_reason)}</td><td>${num(h.dominant_reason_cycles,2)}</td><td class="path">${esc(h.qppu_path)}</td></tr>`);
const issuePcCoverage=`发射 PC 覆盖 ${pct(ss.pc_issue_coverage_percent)}；PC 指令类型覆盖 ${pct(ss.pc_issue_type_coverage_percent)}`;
const waitCoverage=Number(ss.pc_wait_cycles)>0?`Queue Head PC 覆盖 ${pct(ss.pc_wait_coverage_percent)}（${num(ss.pc_wait_covered_cycles,2)} / ${num(ss.pc_wait_cycles,2)} SG-cycle）`:"分析区间没有 Queue Ready 且未发射的 SG 周期";
$("hotspots").innerHTML=`<div class="section"><h2>PC 发射热点</h2><p class="muted">${esc(issuePcCoverage)}</p>${table(["排名","PC","类型","估算指令数","发射占比","队首等待周期","QPPU"],hotRows.length?hotRows:["<tr><td colspan=7>波形没有覆盖发射 PC</td></tr>"])}</div><div class="section"><h2>Queue Head PC 等待热点</h2><p class="muted">${esc(waitCoverage)}</p>${table(["排名","PC","类型","等待周期","覆盖等待占比","主要状态","该状态周期","QPPU"],waitRows.length?waitRows:["<tr><td colspan=8>队列读指针或队首 PC 未覆盖，未进行猜测归因</td></tr>"])}</div>`;
</script>
</body>
</html>
)HTML";
    return prefix + json + suffix + suffix2;
}

}  // namespace

bool writePerformanceBundle(const QString& outputDirectory,
                            QJsonObject& model,
                            QString& error) {
    QDir output(outputDirectory);
    if (!output.exists() && !QDir().mkpath(output.absolutePath())) {
        error = QStringLiteral("无法创建报告目录：%1")
                    .arg(output.absolutePath());
        return false;
    }

    const QJsonArray qppuConclusions =
        buildQppuConclusions(model);
    model.insert(QStringLiteral("qppu_conclusions"),
                 qppuConclusions);
    model.insert(
        QStringLiteral("workload_profile"),
        buildWorkloadProfile(model, qppuConclusions));
    const QJsonArray findings = buildPerformanceFindings(model);
    model.insert(QStringLiteral("findings"), findings);
    model.insert(QStringLiteral("conclusion"),
                 buildPerformanceConclusion(model, findings));

    const QByteArray json =
        QJsonDocument(model).toJson(QJsonDocument::Compact);
    if (!writeFile(output.filePath(QStringLiteral("data.json")),
                   json, error)) {
        return false;
    }
    return writeFile(output.filePath(QStringLiteral("index.html")),
                     htmlDocument(model), error);
}

QString buildPerformanceConsoleSummary(const QJsonObject& model,
                                       const QString& outputDirectory) {
    const QJsonObject summary =
        model.value(QStringLiteral("summary")).toObject();
    const QJsonObject scheduler =
        model.value(QStringLiteral("scheduler")).toObject()
            .value(QStringLiteral("summary")).toObject();
    const QJsonObject workload =
        model.value(QStringLiteral("workload_profile")).toObject();
    const bool selectionCovered =
        !workload.contains(
            QStringLiteral("selection_coverage_complete")) ||
        workload.value(
            QStringLiteral("selection_coverage_complete")).toBool();
    const bool workloadCovered =
        selectionCovered &&
        (workload.value(QStringLiteral("status")).toString() ==
             QStringLiteral("measured") ||
         workload.value(QStringLiteral("status")).toString() ==
             QStringLiteral("partial")) &&
        workload.value(
            QStringLiteral("participating_qppus")).toInt() > 0;
    const QString schedulerStatus =
        model.value(QStringLiteral("scheduler")).toObject()
            .value(QStringLiteral("status")).toString();
    const bool schedulerCovered =
        schedulerStatus == QStringLiteral("measured") ||
        schedulerStatus == QStringLiteral("partial");
    QString text;
    QTextStream out(&text);
    out << QStringLiteral("结论：")
        << model.value(QStringLiteral("conclusion")).toString()
        << '\n';
    if (workload.value(QStringLiteral("status")).toString() ==
        QStringLiteral("static_snapshot")) {
        out << QStringLiteral(
                   "发射利用率：静态快照，不作吞吐判断\n");
        out << QStringLiteral("报告：")
            << QDir(outputDirectory).absoluteFilePath(
                   QStringLiteral("index.html"))
            << '\n';
        out << QStringLiteral("数据：")
            << QDir(outputDirectory).absoluteFilePath(
                   QStringLiteral("data.json"))
            << '\n';
        return text;
    }
    out << (selectionCovered
                ? QStringLiteral("发射利用率：")
                : QStringLiteral("发射利用率（部分覆盖，仅已观测值）："))
        << QString::number(
               workloadCovered
                   ? workload.value(
                         QStringLiteral(
                             "issue_utilization_percent")).toDouble()
                   : summary.value(
                         QStringLiteral(
                             "issue_utilization_percent")).toDouble(),
               'f', 1)
        << QStringLiteral("%，参与范围：")
        << workload.value(
               QStringLiteral("participating_qppus")).toInt()
        << QLatin1Char('/')
        << workload.value(
               QStringLiteral("observed_qppus")).toInt()
        << QStringLiteral(" QPPU，调度器：");
    if (schedulerCovered) {
        out << (selectionCovered
                    ? QStringLiteral("Active SG ")
                    : QStringLiteral(
                          "Active SG（部分覆盖，仅已观测值） "))
            << QString::number(
                   workloadCovered
                       ? workload.value(
                             QStringLiteral(
                                 "active_percent")).toDouble()
                       : scheduler.value(
                             QStringLiteral(
                                 "active_percent")).toDouble(),
                   'f', 1)
            << (schedulerStatus == QStringLiteral("measured") &&
                        selectionCovered
                    ? QStringLiteral("%，局部 Eligible ")
                    : QStringLiteral("%，局部 Eligible（部分覆盖）"))
            << QString::number(
                   workloadCovered
                       ? workload.value(
                             QStringLiteral(
                                 "eligible_percent")).toDouble()
                       : scheduler.value(
                             QStringLiteral(
                                 "eligible_percent")).toDouble(),
                   'f', 1)
            << QStringLiteral("%\n");
    } else {
        out << QStringLiteral("未覆盖 SG 状态\n");
    }
    out << QStringLiteral("报告：")
        << QDir(outputDirectory).absoluteFilePath(
               QStringLiteral("index.html"))
        << '\n';
    out << QStringLiteral("数据：")
        << QDir(outputDirectory).absoluteFilePath(
               QStringLiteral("data.json"))
        << '\n';
    return text;
}

bool performanceOutputSelfTest(QString& error) {
    QJsonObject model;
    QJsonObject analysis;
    analysis.insert(QStringLiteral("dynamic_signals"), 1);
    analysis.insert(QStringLiteral("duration_cycles"), 100.0);
    model.insert(QStringLiteral("analysis"), analysis);
    QJsonObject summary;
    summary.insert(QStringLiteral("issue_utilization_percent"), 80.0);
    model.insert(QStringLiteral("summary"), summary);
    QJsonObject schedulerSummary;
    schedulerSummary.insert(QStringLiteral("active_percent"), 90.0);
    schedulerSummary.insert(QStringLiteral("queue_ready_percent"), 90.0);
    schedulerSummary.insert(QStringLiteral("eligible_percent"), 20.0);
    QJsonObject scheduler;
    scheduler.insert(QStringLiteral("status"), QStringLiteral("measured"));
    scheduler.insert(QStringLiteral("summary"), schedulerSummary);
    QJsonArray qppus;
    QJsonObject idleQppu;
    idleQppu.insert(QStringLiteral("index"), 0);
    idleQppu.insert(
        QStringLiteral("path"),
        QStringLiteral(
            "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0].m_QPPUTOP.[0]"));
    idleQppu.insert(QStringLiteral("issue_utilization_percent"), 0.0);
    idleQppu.insert(QStringLiteral("issue_active_percent"), 0.0);
    idleQppu.insert(QStringLiteral("issue_active_cycles"), 0.0);
    idleQppu.insert(QStringLiteral("issue_idle_cycles"), 100.0);
    idleQppu.insert(QStringLiteral("issue_observed_cycles"), 100.0);
    idleQppu.insert(QStringLiteral("issue_coverage_complete"), true);
    idleQppu.insert(QStringLiteral("activity_coverage_complete"), true);
    idleQppu.insert(QStringLiteral("eligibility_coverage_complete"), true);
    idleQppu.insert(QStringLiteral("active_sg_cycles"), 100.0);
    idleQppu.insert(QStringLiteral("queue_ready_sg_cycles"), 100.0);
    idleQppu.insert(QStringLiteral("eligible_sg_cycles"), 100.0);
    idleQppu.insert(QStringLiteral("eligible_percent"), 100.0);
    idleQppu.insert(QStringLiteral("shader_groups"),
                    QJsonArray{QJsonObject()});
    idleQppu.insert(QStringLiteral("block_reasons"), QJsonArray());
    qppus.push_back(idleQppu);
    QJsonObject busyQppu;
    busyQppu.insert(QStringLiteral("index"), 1);
    busyQppu.insert(
        QStringLiteral("path"),
        QStringLiteral(
            "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0].m_QPPUTOP.[1]"));
    busyQppu.insert(QStringLiteral("issue_utilization_percent"), 200.0);
    busyQppu.insert(QStringLiteral("issue_active_percent"), 100.0);
    busyQppu.insert(QStringLiteral("issue_active_cycles"), 100.0);
    busyQppu.insert(QStringLiteral("issue_idle_cycles"), 0.0);
    busyQppu.insert(QStringLiteral("issue_observed_cycles"), 100.0);
    busyQppu.insert(QStringLiteral("issue_coverage_complete"), true);
    busyQppu.insert(QStringLiteral("activity_coverage_complete"), true);
    busyQppu.insert(QStringLiteral("eligibility_coverage_complete"), true);
    busyQppu.insert(QStringLiteral("active_sg_cycles"), 100.0);
    busyQppu.insert(QStringLiteral("queue_ready_sg_cycles"), 100.0);
    busyQppu.insert(QStringLiteral("eligible_sg_cycles"), 100.0);
    busyQppu.insert(QStringLiteral("eligible_percent"), 100.0);
    busyQppu.insert(QStringLiteral("shader_groups"),
                    QJsonArray{QJsonObject()});
    busyQppu.insert(QStringLiteral("block_reasons"), QJsonArray());
    qppus.push_back(busyQppu);
    QJsonObject feQppu;
    feQppu.insert(QStringLiteral("index"), 2);
    feQppu.insert(
        QStringLiteral("path"),
        QStringLiteral(
            "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0].m_QPPUTOP.[2]"));
    feQppu.insert(QStringLiteral("issue_utilization_percent"), 20.0);
    feQppu.insert(QStringLiteral("issue_active_percent"), 20.0);
    feQppu.insert(QStringLiteral("issue_idle_cycles"), 80.0);
    feQppu.insert(QStringLiteral("issue_observed_cycles"), 100.0);
    feQppu.insert(QStringLiteral("issue_coverage_complete"), true);
    feQppu.insert(QStringLiteral("activity_coverage_complete"), true);
    feQppu.insert(QStringLiteral("eligibility_coverage_complete"), true);
    feQppu.insert(QStringLiteral("active_sg_cycles"), 100.0);
    feQppu.insert(QStringLiteral("queue_ready_sg_cycles"), 20.0);
    feQppu.insert(QStringLiteral("eligible_sg_cycles"), 20.0);
    feQppu.insert(QStringLiteral("eligible_percent"), 100.0);
    feQppu.insert(QStringLiteral("shader_groups"),
                  QJsonArray{QJsonObject()});
    feQppu.insert(QStringLiteral("block_reasons"), QJsonArray());
    qppus.push_back(feQppu);
    QJsonObject ctrlQppu;
    ctrlQppu.insert(QStringLiteral("index"), 3);
    ctrlQppu.insert(
        QStringLiteral("path"),
        QStringLiteral(
            "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0].m_QPPUTOP.[3]"));
    ctrlQppu.insert(QStringLiteral("issue_utilization_percent"), 20.0);
    ctrlQppu.insert(QStringLiteral("issue_active_percent"), 20.0);
    ctrlQppu.insert(QStringLiteral("issue_idle_cycles"), 80.0);
    ctrlQppu.insert(QStringLiteral("issue_observed_cycles"), 100.0);
    ctrlQppu.insert(QStringLiteral("issue_coverage_complete"), true);
    ctrlQppu.insert(QStringLiteral("activity_coverage_complete"), true);
    ctrlQppu.insert(QStringLiteral("eligibility_coverage_complete"), true);
    ctrlQppu.insert(QStringLiteral("active_sg_cycles"), 100.0);
    ctrlQppu.insert(QStringLiteral("queue_ready_sg_cycles"), 100.0);
    ctrlQppu.insert(QStringLiteral("eligible_sg_cycles"), 20.0);
    ctrlQppu.insert(QStringLiteral("eligible_percent"), 20.0);
    ctrlQppu.insert(QStringLiteral("shader_groups"),
                    QJsonArray{QJsonObject()});
    QJsonObject dependencyBlock;
    dependencyBlock.insert(QStringLiteral("name"),
                           QStringLiteral("数据依赖"));
    dependencyBlock.insert(QStringLiteral("cycles"), 60.0);
    dependencyBlock.insert(QStringLiteral("queue_ready_percent"), 60.0);
    ctrlQppu.insert(QStringLiteral("block_reasons"),
                    QJsonArray{dependencyBlock});
    qppus.push_back(ctrlQppu);
    scheduler.insert(QStringLiteral("qppus"), qppus);
    model.insert(QStringLiteral("scheduler"), scheduler);
    QJsonObject fifoEntry;
    fifoEntry.insert(QStringLiteral("path"),
                     QStringLiteral(
                         "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                         "m_QPPUTOP.[0].m_QPPUEU.test_fifo"));
    fifoEntry.insert(QStringLiteral("full_rate_percent"), 60.0);
    fifoEntry.insert(QStringLiteral("full_cycles"), 60.0);
    QJsonObject fifoPressure;
    fifoPressure.insert(QStringLiteral("top"), QJsonArray{fifoEntry});
    QJsonObject creditEntry;
    creditEntry.insert(QStringLiteral("path"),
                       QStringLiteral(
                           "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                           "m_CBCtrl.test_credit"));
    creditEntry.insert(QStringLiteral("exhausted_rate_percent"), 40.0);
    creditEntry.insert(QStringLiteral("exhausted_cycles"), 40.0);
    QJsonObject creditPressure;
    creditPressure.insert(QStringLiteral("top"), QJsonArray{creditEntry});
    QJsonObject resourcePressure;
    resourcePressure.insert(QStringLiteral("fifo"), fifoPressure);
    resourcePressure.insert(QStringLiteral("credit"), creditPressure);
    model.insert(QStringLiteral("resource_pressure"), resourcePressure);
    const QJsonArray qppuConclusions =
        buildQppuConclusions(model);
    model.insert(QStringLiteral("qppu_conclusions"),
                 qppuConclusions);
    const QJsonObject workloadProfile =
        buildWorkloadProfile(model, qppuConclusions);
    model.insert(QStringLiteral("workload_profile"),
                 workloadProfile);
    const QJsonArray findings = buildPerformanceFindings(model);
    if (findings.isEmpty() ||
        findings.first().toObject()
                .value(QStringLiteral("key")).toString() !=
            QStringLiteral("qppu_imbalance") ||
        !buildPerformanceConclusion(model, findings).contains(
            QStringLiteral("EU/BE"))) {
        error =
            QStringLiteral(
                "finding priority or QPPU imbalance mismatch: first=%1, "
                "conclusion=%2")
                .arg(findings.isEmpty()
                         ? QStringLiteral("<empty>")
                         : findings.first().toObject()
                               .value(
                                   QStringLiteral("key")).toString())
                .arg(buildPerformanceConclusion(model, findings));
        return false;
    }
    if (qppuConclusions.size() != 4 ||
        qppuConclusions.first().toObject()
                .value(QStringLiteral("module_key")).toString() !=
            QStringLiteral("qppu_eu") ||
        qppuConclusions.first().toObject()
                .value(QStringLiteral("issue_idle_cycles")).toDouble() !=
            100.0) {
        error = QStringLiteral("per-QPPU bottleneck attribution mismatch");
        return false;
    }
    QSet<QString> diagnosedModules;
    for (const QJsonValue& value : qppuConclusions) {
        diagnosedModules.insert(
            value.toObject()
                .value(QStringLiteral("module_key")).toString());
    }
    if (!diagnosedModules.contains(QStringLiteral("qppu_eu")) ||
        !diagnosedModules.contains(QStringLiteral("fe")) ||
        !diagnosedModules.contains(QStringLiteral("qppu_ctrl"))) {
        error = QStringLiteral("QPPU stage diagnosis coverage mismatch");
        return false;
    }
    bool foundFifoPath = false;
    bool foundCreditPath = false;
    bool foundCreditModule = false;
    for (const QJsonValue& value : findings) {
        const QJsonObject object = value.toObject();
        const QString key = object.value(QStringLiteral("key")).toString();
        const QString evidence =
            object.value(QStringLiteral("evidence")).toString();
        if (key == QStringLiteral("buffer_pressure") &&
            evidence.contains(QStringLiteral("m_QPPUEU.test_fifo"))) {
            foundFifoPath = true;
        }
        if (key == QStringLiteral("credit_exhaustion") &&
            evidence.contains(QStringLiteral("m_CBCtrl.test_credit"))) {
            foundCreditPath = true;
            foundCreditModule =
                object.value(QStringLiteral("title")).toString()
                    .contains(QStringLiteral("CB"));
        }
    }
    if (!foundFifoPath || !foundCreditPath ||
        !foundCreditModule) {
        error = QStringLiteral("resource finding path mismatch");
        return false;
    }

    QJsonObject duplicateIndexModel;
    duplicateIndexModel.insert(QStringLiteral("analysis"), analysis);
    duplicateIndexModel.insert(QStringLiteral("summary"), summary);
    QJsonObject duplicateScheduler;
    duplicateScheduler.insert(QStringLiteral("status"),
                              QStringLiteral("measured"));
    duplicateScheduler.insert(QStringLiteral("summary"),
                              schedulerSummary);
    QJsonObject duplicateIdle;
    duplicateIdle.insert(QStringLiteral("index"), 0);
    duplicateIdle.insert(
        QStringLiteral("path"),
        QStringLiteral(
            "gpu.m_ppu.[0].m_QPPUTOP.[0]"));
    duplicateIdle.insert(QStringLiteral("issue_utilization_percent"), 0.0);
    duplicateIdle.insert(QStringLiteral("issue_active_percent"), 0.0);
    duplicateIdle.insert(QStringLiteral("issue_active_cycles"), 0.0);
    duplicateIdle.insert(QStringLiteral("issue_idle_cycles"), 100.0);
    duplicateIdle.insert(QStringLiteral("issue_observed_cycles"), 100.0);
    duplicateIdle.insert(QStringLiteral("issue_coverage_complete"), true);
    duplicateIdle.insert(QStringLiteral("issued_instructions_estimate"), 0.0);
    duplicateIdle.insert(QStringLiteral("active_sg_cycles"), 0.0);
    duplicateIdle.insert(QStringLiteral("queue_ready_sg_cycles"), 0.0);
    duplicateIdle.insert(QStringLiteral("eligible_sg_cycles"), 0.0);
    duplicateIdle.insert(QStringLiteral("eligible_percent"), 0.0);
    duplicateIdle.insert(QStringLiteral("activity_coverage_complete"), true);
    duplicateIdle.insert(QStringLiteral("eligibility_coverage_complete"), true);
    duplicateIdle.insert(
        QStringLiteral("shader_groups"),
        QJsonArray{QJsonObject{
            {QStringLiteral("activity_coverage_complete"), true},
            {QStringLiteral("eligibility_coverage_complete"), true}}});
    duplicateIdle.insert(QStringLiteral("block_reasons"), QJsonArray());
    QJsonObject duplicateBusy = duplicateIdle;
    duplicateBusy.insert(
        QStringLiteral("path"),
        QStringLiteral(
            "gpu.m_ppu.[1].m_QPPUTOP.[0]"));
    duplicateBusy.insert(QStringLiteral("issue_utilization_percent"), 200.0);
    duplicateBusy.insert(QStringLiteral("issue_active_percent"), 100.0);
    duplicateBusy.insert(QStringLiteral("issue_active_cycles"), 100.0);
    duplicateBusy.insert(QStringLiteral("issue_idle_cycles"), 0.0);
    duplicateBusy.insert(QStringLiteral("issued_instructions_estimate"), 200.0);
    duplicateBusy.insert(QStringLiteral("active_sg_cycles"), 100.0);
    duplicateBusy.insert(QStringLiteral("queue_ready_sg_cycles"), 100.0);
    duplicateBusy.insert(QStringLiteral("eligible_sg_cycles"), 100.0);
    duplicateBusy.insert(QStringLiteral("eligible_percent"), 100.0);
    duplicateScheduler.insert(
        QStringLiteral("qppus"),
        QJsonArray{duplicateIdle, duplicateBusy});
    duplicateIndexModel.insert(QStringLiteral("scheduler"),
                               duplicateScheduler);
    const QJsonArray duplicateConclusions =
        buildQppuConclusions(duplicateIndexModel);
    const QJsonObject duplicateWorkload =
        buildWorkloadProfile(duplicateIndexModel,
                             duplicateConclusions);
    const QJsonArray duplicatePaths =
        duplicateWorkload.value(
            QStringLiteral("participant_paths")).toArray();
    if (duplicateConclusions.size() != 2 ||
        duplicateWorkload.value(
            QStringLiteral("participating_qppus")).toInt() != 1 ||
        duplicateWorkload.value(
            QStringLiteral("inactive_qppus")).toInt() != 1 ||
        duplicatePaths.size() != 1 ||
        duplicatePaths.first().toString() !=
            QStringLiteral("gpu.m_ppu.[1].m_QPPUTOP.[0]")) {
        error = QStringLiteral(
            "duplicate local QPPU indexes were not isolated by path");
        return false;
    }

    QJsonObject partialCoverageModel = duplicateIndexModel;
    QJsonObject partialScheduler = duplicateScheduler;
    QJsonObject partialQppu = duplicateBusy;
    partialQppu.insert(QStringLiteral("issue_utilization_percent"), 0.0);
    partialQppu.insert(QStringLiteral("issue_active_percent"), 0.0);
    partialQppu.insert(QStringLiteral("issue_active_cycles"), 0.0);
    partialQppu.insert(QStringLiteral("issue_idle_cycles"), 100.0);
    partialQppu.insert(QStringLiteral("issued_instructions_estimate"), 0.0);
    partialQppu.insert(QStringLiteral("eligible_sg_cycles"), 0.0);
    partialQppu.insert(QStringLiteral("eligible_percent"), 0.0);
    partialQppu.insert(QStringLiteral("eligibility_coverage_complete"), false);
    QJsonObject partialBlock;
    partialBlock.insert(QStringLiteral("key"),
                        QStringLiteral("stall_count"));
    partialBlock.insert(QStringLiteral("name"),
                        QStringLiteral("指令延迟计数"));
    partialBlock.insert(QStringLiteral("cycles"), 20.0);
    partialBlock.insert(QStringLiteral("queue_ready_percent"), 20.0);
    partialQppu.insert(QStringLiteral("block_reasons"),
                       QJsonArray{partialBlock});
    partialScheduler.insert(QStringLiteral("status"),
                            QStringLiteral("partial"));
    partialScheduler.insert(QStringLiteral("qppus"),
                            QJsonArray{partialQppu});
    partialCoverageModel.insert(QStringLiteral("scheduler"),
                                partialScheduler);
    const QJsonArray partialConclusions =
        buildQppuConclusions(partialCoverageModel);
    if (partialConclusions.size() != 1 ||
        partialConclusions.first().toObject()
                .value(QStringLiteral("state")).toString() !=
            QStringLiteral("risk") ||
        partialConclusions.first().toObject()
                .value(QStringLiteral("confidence_score")).toInt() >= 80) {
        error = QStringLiteral(
            "partial blocker coverage was promoted to a root cause");
        return false;
    }

    auto microModel =
        [](double issueUtilization,
           double issueActive,
           double queueReady,
           double eligible,
           bool dependency,
           bool l1Latency) {
            QJsonObject micro;
            QJsonObject microAnalysis;
            microAnalysis.insert(
                QStringLiteral("dynamic_signals"), 1);
            microAnalysis.insert(
                QStringLiteral("duration_cycles"), 100.0);
            micro.insert(QStringLiteral("analysis"),
                         microAnalysis);

            QJsonObject microSummary;
            microSummary.insert(
                QStringLiteral("issue_utilization_percent"),
                issueUtilization / 4.0);
            microSummary.insert(
                QStringLiteral("issued_instructions_estimate"),
                issueUtilization);
            microSummary.insert(
                QStringLiteral("global_memory_issue_estimate"),
                l1Latency ? issueUtilization : 0.0);
            microSummary.insert(
                QStringLiteral("local_memory_issue_estimate"), 0.0);
            QJsonObject threadClass;
            threadClass.insert(QStringLiteral("key"),
                               QStringLiteral("thread"));
            threadClass.insert(
                QStringLiteral("instruction_share_percent"),
                100.0);
            microSummary.insert(
                QStringLiteral("issue_classes"),
                QJsonArray{threadClass});
            micro.insert(QStringLiteral("summary"),
                         microSummary);

            QJsonObject microScheduler;
            microScheduler.insert(
                QStringLiteral("status"),
                QStringLiteral("measured"));
            QJsonObject microSchedulerSummary;
            microSchedulerSummary.insert(
                QStringLiteral("active_percent"), 25.0);
            microSchedulerSummary.insert(
                QStringLiteral("queue_ready_percent"),
                queueReady / 4.0);
            microSchedulerSummary.insert(
                QStringLiteral("eligible_percent"),
                eligible);
            microScheduler.insert(
                QStringLiteral("summary"),
                microSchedulerSummary);
            QJsonArray microQppus;
            for (int index = 0; index < 4; ++index) {
                const bool active = index == 0;
                QJsonObject qppu;
                qppu.insert(QStringLiteral("index"), index);
                qppu.insert(
                    QStringLiteral("path"),
                    QStringLiteral(
                        "gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                        "m_QPPUTOP.[%1]")
                        .arg(index));
                qppu.insert(
                    QStringLiteral(
                        "issued_instructions_estimate"),
                    active ? issueUtilization : 0.0);
                qppu.insert(
                    QStringLiteral(
                        "issue_utilization_percent"),
                    active ? issueUtilization : 0.0);
                qppu.insert(
                    QStringLiteral("issue_active_percent"),
                    active ? issueActive : 0.0);
                qppu.insert(
                    QStringLiteral("issue_active_cycles"),
                    active ? issueActive : 0.0);
                qppu.insert(
                    QStringLiteral("issue_idle_cycles"),
                    active ? 100.0 - issueActive : 100.0);
                qppu.insert(
                    QStringLiteral("issue_observed_cycles"),
                    100.0);
                qppu.insert(
                    QStringLiteral("issue_coverage_complete"),
                    true);
                qppu.insert(
                    QStringLiteral("activity_coverage_complete"),
                    true);
                qppu.insert(
                    QStringLiteral("eligibility_coverage_complete"),
                    true);
                qppu.insert(
                    QStringLiteral("active_sg_cycles"),
                    active ? 100.0 : 0.0);
                qppu.insert(
                    QStringLiteral("queue_ready_sg_cycles"),
                    active ? queueReady : 0.0);
                qppu.insert(
                    QStringLiteral("eligible_sg_cycles"),
                    active ? queueReady * eligible / 100.0
                           : 0.0);
                qppu.insert(
                    QStringLiteral("eligible_percent"),
                    active ? eligible : 0.0);
                qppu.insert(
                    QStringLiteral("shader_groups"),
                    QJsonArray{QJsonObject()});
                QJsonArray blocks;
                if (active && dependency) {
                    QJsonObject block;
                    block.insert(
                        QStringLiteral("key"),
                        QStringLiteral("operand_not_ready"));
                    block.insert(
                        QStringLiteral("name"),
                        QStringLiteral("数据依赖"));
                    block.insert(
                        QStringLiteral("cycles"), 60.0);
                    block.insert(
                        QStringLiteral(
                            "queue_ready_percent"),
                        queueReady > 0.0
                            ? 6000.0 / queueReady
                            : 0.0);
                    blocks.push_back(block);
                }
                qppu.insert(QStringLiteral("block_reasons"),
                            blocks);
                microQppus.push_back(qppu);
            }
            microScheduler.insert(QStringLiteral("qppus"),
                                  microQppus);
            micro.insert(QStringLiteral("scheduler"),
                         microScheduler);

            if (l1Latency) {
                QJsonObject latency;
                latency.insert(QStringLiteral("available"), true);
                latency.insert(
                    QStringLiteral("status"),
                    QStringLiteral("measured"));
                latency.insert(
                    QStringLiteral("average_cycles"), 24.0);
                latency.insert(
                    QStringLiteral("p95_cycles"), 26.0);
                latency.insert(
                    QStringLiteral("maximum_cycles"), 28.0);
                latency.insert(
                    QStringLiteral("maximum_outstanding"), 1);
                latency.insert(
                    QStringLiteral("confidence_score"), 95);
                latency.insert(
                    QStringLiteral("coverage_complete"), true);
                QJsonObject l1;
                l1.insert(QStringLiteral("latency"), latency);
                QJsonObject memoryBandwidth;
                memoryBandwidth.insert(QStringLiteral("l1"),
                                       l1);
                micro.insert(
                    QStringLiteral("memory_bandwidth"),
                    memoryBandwidth);
            }

            const QJsonArray conclusions =
                buildQppuConclusions(micro);
            micro.insert(
                QStringLiteral("qppu_conclusions"),
                conclusions);
            micro.insert(
                QStringLiteral("workload_profile"),
                buildWorkloadProfile(micro, conclusions));
            micro.insert(
                QStringLiteral("findings"),
                buildPerformanceFindings(micro));
            return micro;
        };

    const QJsonObject memoryLatency =
        microModel(20.0, 20.0, 100.0, 20.0, true, true);
    const QJsonObject dependencyLatency =
        microModel(20.0, 20.0, 100.0, 20.0, true, false);
    const QJsonObject frontend =
        microModel(20.0, 20.0, 20.0, 100.0, false, false);
    const QJsonObject throughput =
        microModel(200.0, 100.0, 100.0, 100.0, false, false);
    QJsonObject stableThroughput = throughput;
    QJsonObject stableAnalysis =
        stableThroughput.value(
            QStringLiteral("analysis")).toObject();
    stableAnalysis.insert(QStringLiteral("dynamic_signals"), 0);
    stableThroughput.insert(QStringLiteral("analysis"),
                            stableAnalysis);
    const QJsonObject stableProfile =
        buildWorkloadProfile(stableThroughput, QJsonArray());
    const auto profileRegime =
        [](const QJsonObject& object) {
            return object.value(
                QStringLiteral("workload_profile")).toObject()
                .value(QStringLiteral("regime")).toString();
        };
    const QJsonObject memoryScope =
        memoryLatency.value(
            QStringLiteral("workload_profile")).toObject();
    bool hasFalseImbalance = false;
    for (const QJsonValue& value :
         memoryLatency.value(
             QStringLiteral("findings")).toArray()) {
        if (value.toObject()
                .value(QStringLiteral("key")).toString() ==
            QStringLiteral("qppu_imbalance")) {
            hasFalseImbalance = true;
        }
    }
    if (memoryScope.value(
            QStringLiteral("participating_qppus")).toInt() != 1 ||
        memoryScope.value(
            QStringLiteral("inactive_qppus")).toInt() != 3 ||
        hasFalseImbalance ||
        profileRegime(memoryLatency) !=
            QStringLiteral("memory_latency") ||
        profileRegime(dependencyLatency) !=
            QStringLiteral("dependency_latency") ||
        profileRegime(frontend) !=
            QStringLiteral("frontend_supply") ||
        profileRegime(throughput) !=
            QStringLiteral("throughput") ||
        stableProfile.value(
            QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        stableProfile.value(
            QStringLiteral("regime")).toString() !=
            QStringLiteral("throughput")) {
        error =
            QStringLiteral(
                "microbenchmark classification mismatch: "
                "scope=%1/%2, memory=%3, dependency=%4, "
                "frontend=%5, throughput=%6, imbalance=%7")
                .arg(memoryScope.value(
                         QStringLiteral(
                             "participating_qppus")).toInt())
                .arg(memoryScope.value(
                         QStringLiteral(
                             "inactive_qppus")).toInt())
                .arg(profileRegime(memoryLatency))
                .arg(profileRegime(dependencyLatency))
                .arg(profileRegime(frontend))
                .arg(profileRegime(throughput))
                .arg(hasFalseImbalance);
        return false;
    }

    const QByteArray html = htmlDocument(model);
    if (!html.contains("WavePerf") || !html.contains("const DATA=") ||
        !html.contains("\"scheduler\"")) {
        error = QStringLiteral("HTML report embedding mismatch");
        return false;
    }
    return true;
}

}  // namespace waveperf
