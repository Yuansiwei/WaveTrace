#include "WaveFifoPerfOutput.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>

namespace wavefifo {
namespace {

bool writeFile(const QString& path, const QByteArray& content, QString& error) {
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
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WaveFifoPerf FIFO / Queue 满率报告</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--line:#d8dee5;--text:#17212b;--muted:#65717e;--blue:#1769aa;--amber:#a15c00;--red:#b42318}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.5 "Microsoft YaHei UI","Segoe UI",sans-serif}
header{padding:20px 28px;background:#15202b;color:#fff}h1{font-size:22px;margin:0 0 4px}header p{margin:0;color:#c9d4df;overflow-wrap:anywhere}
main{max-width:1500px;margin:auto;padding:22px 28px 40px}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(165px,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:7px;overflow:hidden;margin-bottom:16px}
.card{background:var(--card);padding:14px}.card b{display:block;font-size:23px;margin-top:4px}.card span,.muted{color:var(--muted)}section{background:#fff;border:1px solid var(--line);border-radius:7px;padding:17px;margin-bottom:16px}h2{font-size:17px;margin:0 0 12px}
.scroll{overflow:auto;max-height:680px;border:1px solid var(--line)}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{padding:8px 9px;text-align:left;vertical-align:top;border-bottom:1px solid #e6eaee}th{position:sticky;top:0;background:#f6f8fa}.path{font:12px Consolas,monospace;overflow-wrap:anywhere}.warn{color:var(--amber)}.bad{color:var(--red)}
.bar{height:8px;min-width:110px;background:#e4e9ed}.bar i{height:100%;display:block;background:var(--blue);max-width:100%}.pill{border:1px solid var(--line);border-radius:4px;padding:1px 6px;white-space:nowrap}.pill.warn{border-color:#d9b878}.toolbar{display:flex;justify-content:flex-end;align-items:center;gap:8px;margin:0 0 10px}.toolbar input,.toolbar select{font:inherit;padding:5px 8px;border:1px solid var(--line);border-radius:4px;background:#fff}.toolbar input{flex:1;min-width:180px;max-width:520px}
ul{margin:8px 0;padding-left:22px}@media(max-width:700px){header,main{padding-left:14px;padding-right:14px}section{padding:12px}}
</style></head><body><header><h1>WaveFifoPerf FIFO / Queue 满率报告</h1><p id="meta"></p></header><main>
<div class="cards" id="cards"></div>
<section><h2>FIFO / Queue 满率</h2><p class="muted">满：occupancy ≥ capacity。只有两者同时已知且 capacity &gt; 0 的区间进入分母；未知值不会按 0 处理。</p><div class="toolbar"><input id="pathFilter" type="search" placeholder="搜索 FIFO / Queue 路径" aria-label="搜索 FIFO / Queue 路径"><label for="sortMode">排序</label><select id="sortMode"><option value="full">按满率</option><option value="occupancy">按占用率</option></select></div><div class="scroll"><table><thead><tr><th>FIFO / Queue 路径</th><th>资源</th><th>满率</th><th>占用率</th><th>平均占用 / 容量</th></tr></thead><tbody id="rows"></tbody></table></div></section>
<section><h2>未确认候选</h2><div id="rejected"></div></section>
<section><h2>告警</h2><div id="warnings"></div></section>
</main><script>const DATA=)HTML";
    const QByteArray suffix = R"HTML(;
const esc=v=>String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const pct=v=>Number.isFinite(Number(v))?Number(v).toFixed(2)+'%':'-';
const num2=v=>Number.isFinite(Number(v))?Number(v).toLocaleString('zh-CN',{maximumFractionDigits:2}):'-';
const a=DATA.analysis||{},s=DATA.summary||{};
document.getElementById('meta').textContent=(DATA.input_file||'')+' · '+a.start_cycle+'–'+a.end_cycle+' cycle · '+a.ticks_per_cycle+' tick/cycle';
const cards=[['确认 FIFO',s.confirmed_fifo_count||0],['确认 Queue',s.confirmed_queue_count||0],['完整覆盖',s.complete_count||0],['部分覆盖',s.partial_count||0],['聚合满率',pct(s.aggregate_full_rate_percent)],['仅解码信号',s.decoded_signal_count||0]];
document.getElementById('cards').innerHTML=cards.map(x=>`<div class="card"><span>${esc(x[0])}</span><b>${esc(x[1])}</b></div>`).join('');
const sortedResources=mode=>{const query=document.getElementById('pathFilter').value.trim().toLocaleLowerCase();return[...(DATA.resources||[])].filter(x=>String(x.path||'').toLocaleLowerCase().includes(query)).sort((a,b)=>{const key=mode==='occupancy'?'occupancy_rate_percent':'full_rate_percent';const av=Number(a[key]),bv=Number(b[key]);const ak=Number.isFinite(av),bk=Number.isFinite(bv);if(ak!==bk)return ak?-1:1;if(ak&&av!==bv)return bv-av;return String(a.path||'').localeCompare(String(b.path||''),'zh-CN')})};
const renderRows=()=>{const mode=document.getElementById('sortMode').value;document.getElementById('rows').innerHTML=sortedResources(mode).map(x=>`<tr><td><div class="path">${esc(x.path)}</div>${x.representative_only?' <span class="pill">[0] 代表</span>':''}</td><td>${esc(x.resource_kind_label)}</td><td>${pct(x.full_rate_percent)}<div class="bar"><i style="width:${Math.max(0,Math.min(100,Number(x.full_rate_percent)||0))}%"></i></div></td><td>${pct(x.occupancy_rate_percent)}</td><td>${num2(x.average_occupancy)} / ${num2(x.average_capacity)}</td></tr>`).join('')||'<tr><td colspan="5">没有确认的 FIFO / Queue</td></tr>'};
document.getElementById('sortMode').addEventListener('change',renderRows);document.getElementById('pathFilter').addEventListener('input',renderRows);renderRows();
document.getElementById('rejected').innerHTML=(DATA.rejected_candidates||[]).map(x=>`<p><span class="path">${esc(x.path)}</span>：${esc(x.reason)}</p>`).join('')||'<p class="muted">无</p>';
document.getElementById('warnings').innerHTML=(DATA.warnings||[]).map(x=>`<p class="warn">${esc(x)}</p>`).join('')||'<p class="muted">无</p>';
</script></body></html>)HTML";
    return prefix + json + suffix;
}

}  // namespace

bool writeFifoPerformanceBundle(const QString& outputDirectory,
                                const QJsonObject& model,
                                QString& error) {
    QDir output(outputDirectory);
    if (!output.exists() && !QDir().mkpath(output.absolutePath())) {
        error = QStringLiteral("无法创建报告目录：%1").arg(output.absolutePath());
        return false;
    }
    const QByteArray json = QJsonDocument(model).toJson(QJsonDocument::Compact);
    if (!writeFile(output.filePath(QStringLiteral("data.json")), json, error))
        return false;
    return writeFile(output.filePath(QStringLiteral("index.html")),
                     htmlDocument(model), error);
}

}  // namespace wavefifo
