#!/usr/bin/env python3
from pathlib import Path
import argparse
import html
import json
import tempfile

TEMPLATE = r"""<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<script src="https://cdn.plot.ly/plotly-3.1.0.min.js"></script>
<style>
:root{color-scheme:dark;--bg:#0b1020;--panel:#131a2b;--text:#e8edf8;--muted:#93a1ba;--line:#27324a;--accent:#70a5ff;--good:#55d6a0;--warn:#ffbd69}
[data-theme=light]{color-scheme:light;--bg:#f4f6fa;--panel:#fff;--text:#172033;--muted:#61708a;--line:#dce2ec;--accent:#2563eb;--good:#07855f;--warn:#a95d00}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 Inter,ui-sans-serif,system-ui,sans-serif}header{position:sticky;top:0;z-index:2;display:flex;align-items:center;gap:16px;padding:14px 24px;background:color-mix(in srgb,var(--bg) 88%,transparent);backdrop-filter:blur(14px);border-bottom:1px solid var(--line)}h1{font-size:18px;margin:0}button,select,input{background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:7px 10px}button{cursor:pointer;margin-left:auto}main{max-width:1680px;margin:auto;padding:20px}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px}.card,.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;box-shadow:0 8px 30px #0001}.card{padding:14px}.card strong{display:block;font-size:22px;margin-top:4px}.muted{color:var(--muted)}.warning{color:var(--warn)}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;margin-top:14px}.panel{padding:14px;min-width:0}.wide{grid-column:1/-1}h2{font-size:15px;margin:0 0 10px}.plot{height:350px}.controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:8px}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}th,td{text-align:right;padding:7px;border-bottom:1px solid var(--line)}th:first-child,td:first-child{text-align:left}th{position:sticky;top:0;background:var(--panel);color:var(--muted)}.table-wrap{max-height:430px;overflow:auto}@media(max-width:900px){.grid{grid-template-columns:1fr}.plot{height:300px}header{padding:12px}main{padding:12px}}
</style>
</head>
<body>
<header><h1>__TITLE__</h1><span class="muted">DDNet render trace</span><button id="theme">Light mode</button></header>
<main>
<div id="warning"></div><div class="cards" id="cards"></div>
<div class="grid">
  <section class="panel wide"><h2>Frame timing</h2><div id="timing" class="plot"></div></section>
  <section class="panel"><h2>Render counters</h2><div class="controls"><label>Metric <select id="counter"></select></label></div><div id="counters" class="plot"></div></section>
  <section class="panel"><h2>CPU zones over time</h2><div id="zones" class="plot"></div></section>
  <section class="panel wide"><h2>GPU components over time</h2><div id="gpu-zones" class="plot"></div></section>
  <section class="panel wide"><h2>CPU event timeline</h2><div class="controls"><label>Zone <select id="event-filter"><option value="-1">All zones</option></select></label><label>First frame <input id="first-frame" type="number" min="0"></label><label>Last frame <input id="last-frame" type="number" min="0"></label><button id="apply" style="margin-left:0">Apply</button><span class="muted" id="event-note"></span></div><div id="events" class="plot"></div></section>
  <section class="panel"><h2>CPU zone statistics</h2><div class="table-wrap"><table id="zone-table"></table></div></section>
  <section class="panel"><h2>GPU component statistics</h2><div class="table-wrap"><table id="gpu-zone-table"></table></div></section>
  <section class="panel"><h2>Frame metric statistics</h2><div class="table-wrap"><table id="metric-table"></table></div></section>
</div>
</main>
<script>const TRACE=__TRACE_DATA__;</script>
<script>
const frames=TRACE.frames||[], events=TRACE.events||[], names=TRACE.names||[],gpuZoneNames=TRACE.gpu_zone_names||['world','interface'],seenGpu=new Set(),gpuFrames=frames.filter(f=>f.gpu_supported&&f.gpu_time_ns>0&&!seenGpu.has(f.gpu_sample)&&seenGpu.add(f.gpu_sample)),gpuWorldFrames=gpuFrames.filter(f=>(f.gpu_zone_mask&1)!==0),gpuInterfaceFrames=gpuFrames.filter(f=>(f.gpu_zone_mask&2)!==0),gpuZoneValue=(f,i)=>f.gpu_zones_ns?.[i]??(i===0?f.gpu_world_ns||0:i===1?f.gpu_interface_ns||0:0);
const $=id=>document.getElementById(id), nsms=v=>v/1e6, pct=(a,p)=>{if(!a.length)return 0;const b=[...a].sort((x,y)=>x-y),i=(b.length-1)*p,l=Math.floor(i),h=Math.ceil(i);return b[l]+(b[h]-b[l])*(i-l)}, fmt=(v,d=2)=>Number(v).toLocaleString(undefined,{maximumFractionDigits:d}),esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const theme=()=>document.documentElement.dataset.theme, layout=(title,y)=>({title:{text:title,font:{size:13}},paper_bgcolor:'transparent',plot_bgcolor:'transparent',font:{color:getComputedStyle(document.body).getPropertyValue('--text')},margin:{l:60,r:20,t:36,b:45},xaxis:{title:'Seconds',gridcolor:getComputedStyle(document.body).getPropertyValue('--line')},yaxis:{title:y,gridcolor:getComputedStyle(document.body).getPropertyValue('--line')},legend:{orientation:'h'}});
function draw(){const x=frames.map(f=>f.timestamp_ns/1e9);Plotly.react('timing',[{x,y:frames.map(f=>nsms(f.frametime_ns)),name:'Frame time'},{x,y:frames.map(f=>nsms(f.render_wall_ns)),name:'Render wall'},{x:gpuFrames.map(f=>f.timestamp_ns/1e9),y:gpuFrames.map(f=>nsms(f.gpu_time_ns)),name:'Native GPU'},{x:gpuWorldFrames.map(f=>f.timestamp_ns/1e9),y:gpuWorldFrames.map(f=>nsms(f.gpu_world_ns)),name:'GPU world'},{x:gpuInterfaceFrames.map(f=>f.timestamp_ns/1e9),y:gpuInterfaceFrames.map(f=>nsms(f.gpu_interface_ns)),name:'GPU UI'}],layout('', 'Milliseconds'),{responsive:true,displaylogo:false});drawCounter();drawZones();drawGpuZones();drawEvents()}
const duration=frames.length?(frames.at(-1).timestamp_ns-frames[0].timestamp_ns)/1e9:0;
const mailboxDelta=k=>frames.length?Math.max(0,(frames.at(-1)[k]||0)-(frames[0][k]||0)):0;for(let i=0;i<frames.length;i++)for(const k of ['frames_produced','frames_rendered','frames_dropped'])frames[i][k+'_delta']=i?Math.max(0,(frames[i][k]||0)-(frames[i-1][k]||0)):0;
const cards=[['Frames',fmt(frames.length,0)],['Duration',fmt(duration)+' s'],['Frontend FPS',fmt(1e9/pct(frames.map(f=>f.frametime_ns).filter(Boolean),.5),0)],['Render p95',fmt(nsms(pct(frames.map(f=>f.render_wall_ns),.95)))+' ms'],['GPU p95',gpuFrames.length?fmt(nsms(pct(gpuFrames.map(f=>f.gpu_time_ns),.95)))+' ms':'Unavailable'],['GPU samples',fmt(gpuFrames.length,0)],['Mailbox P/R/D',`${fmt(mailboxDelta('frames_produced'),0)} / ${fmt(mailboxDelta('frames_rendered'),0)} / ${fmt(mailboxDelta('frames_dropped'),0)}`],['CPU events',fmt(events.length,0)]];
$('cards').innerHTML=cards.map(([a,b])=>`<div class="card"><span class="muted">${a}</span><strong>${b}</strong></div>`).join('');
if(TRACE.dropped_frames||TRACE.dropped_events)$('warning').innerHTML=`<p class="warning">Ring capacity exceeded: ${fmt(TRACE.dropped_frames,0)} frames and ${fmt(TRACE.dropped_events,0)} CPU events overwritten.</p>`;
const frameMetrics=['commands','resource_commands','draw_commands','draw_calls','triangles','instances','render_passes','buffer_creates','buffer_recreates','buffer_updates','texture_creates','texture_updates','upload_bytes','streamed_bytes','text_layout_ns','text_layout_calls','glyphs','text_creates','text_soft_recreates','text_deletes','text_renders','text_upload_bytes','frames_produced_delta','frames_rendered_delta','frames_dropped_delta','texture_memory','buffer_memory','streamed_memory','staging_memory'];
$('counter').innerHTML=frameMetrics.map(k=>`<option>${k}</option>`).join('');$('counter').value='draw_calls';$('counter').onchange=drawCounter;
function drawCounter(){const k=$('counter').value,x=frames.map(f=>f.timestamp_ns/1e9);Plotly.react('counters',[{x,y:frames.map(f=>f[k]||0),name:k,line:{color:'#70a5ff'}}],layout('',k),{responsive:true,displaylogo:false})}
const grouped=names.map(()=>new Map()),durations=names.map(()=>[]);for(const e of events){const m=grouped[e.name];m.set(e.frame,(m.get(e.frame)||0)+e.duration_ns);durations[e.name].push(e.duration_ns)}
const zoneStats=names.map((name,i)=>{const d=frames.map(f=>grouped[i].get(f.frame)||0);return{name,i,calls:durations[i].length,total:d.reduce((a,b)=>a+b,0),p50:pct(d,.5),p95:pct(d,.95),p99:pct(d,.99)}}).sort((a,b)=>b.total-a.total);
function drawZones(){const top=zoneStats.slice(0,8),x=frames.map(f=>f.timestamp_ns/1e9);Plotly.react('zones',top.map(z=>({x,y:frames.map(f=>nsms(grouped[z.i].get(f.frame)||0)),name:z.name})),layout('', 'Inclusive CPU ms/frame'),{responsive:true,displaylogo:false})}
$('zone-table').innerHTML='<thead><tr><th>Zone</th><th>Calls</th><th>Total ms</th><th>p50 µs/frame</th><th>p95 µs/frame</th><th>p99 µs/frame</th></tr></thead><tbody>'+zoneStats.map(z=>`<tr><td>${esc(z.name)}</td><td>${fmt(z.calls,0)}</td><td>${fmt(nsms(z.total))}</td><td>${fmt(z.p50/1e3)}</td><td>${fmt(z.p95/1e3)}</td><td>${fmt(z.p99/1e3)}</td></tr>`).join('')+'</tbody>';
const gpuZoneStats=gpuZoneNames.map((name,i)=>{const samples=gpuFrames.filter(f=>(f.gpu_zone_mask&(1<<i))!==0),values=samples.map(f=>gpuZoneValue(f,i));return{name,i,samples,values,total:values.reduce((a,b)=>a+b,0),p50:pct(values,.5),p95:pct(values,.95),p99:pct(values,.99)}}).filter(z=>z.i>1&&z.values.length).sort((a,b)=>b.total-a.total);
function drawGpuZones(){Plotly.react('gpu-zones',gpuZoneStats.slice(0,8).map(z=>({x:z.samples.map(f=>f.timestamp_ns/1e9),y:z.values.map(nsms),name:z.name})),layout('', 'GPU ms/frame'),{responsive:true,displaylogo:false})}
$('gpu-zone-table').innerHTML='<thead><tr><th>Component</th><th>Samples</th><th>p50 µs</th><th>p95 µs</th><th>p99 µs</th></tr></thead><tbody>'+gpuZoneStats.map(z=>`<tr><td>${esc(z.name)}</td><td>${fmt(z.values.length,0)}</td><td>${fmt(z.p50/1e3)}</td><td>${fmt(z.p95/1e3)}</td><td>${fmt(z.p99/1e3)}</td></tr>`).join('')+'</tbody>';
const stats=[['frametime_ns',1e6,'ms'],['render_wall_ns',1e6,'ms'],['gpu_time_ns',1e6,'ms'],['gpu_world_ns',1e6,'ms'],['gpu_interface_ns',1e6,'ms'],...frameMetrics.map(k=>[k,1,''])];
$('metric-table').innerHTML='<thead><tr><th>Metric</th><th>p50</th><th>p95</th><th>p99</th><th>Max</th></tr></thead><tbody>'+stats.map(([k,s,u])=>{const source=k==='gpu_time_ns'?gpuFrames:k==='gpu_world_ns'?gpuWorldFrames:k==='gpu_interface_ns'?gpuInterfaceFrames:frames,a=source.map(f=>f[k]||0);return `<tr><td>${k} ${u}</td><td>${fmt(pct(a,.5)/s)}</td><td>${fmt(pct(a,.95)/s)}</td><td>${fmt(pct(a,.99)/s)}</td><td>${fmt(a.reduce((m,v)=>Math.max(m,v),0)/s)}</td></tr>`}).join('')+'</tbody>';
$('event-filter').innerHTML+=[...names].map((n,i)=>`<option value="${i}">${esc(n)}</option>`).join('');if(frames.length){$('first-frame').value=frames[0].frame;$('last-frame').value=frames.at(-1).frame}$('apply').onclick=drawEvents;
function drawEvents(){const wanted=+$('event-filter').value,first=+$('first-frame').value,last=+$('last-frame').value,max=25000;let a=events.filter(e=>(wanted<0||e.name===wanted)&&e.frame>=first&&e.frame<=last);const stride=Math.max(1,Math.ceil(a.length/max));if(stride>1)a=a.filter((_,i)=>i%stride===0);$('event-note').textContent=stride>1?`Showing every ${stride}th event (${fmt(a.length,0)} points)`:`${fmt(a.length,0)} events`;Plotly.react('events',[{x:a.map(e=>e.start_ns/1e9),y:a.map(e=>names[e.name]),mode:'markers',type:'scattergl',marker:{size:6,color:a.map(e=>nsms(e.duration_ns)),colorscale:'Viridis',showscale:true,colorbar:{title:'ms'}},text:a.map(e=>`frame ${e.frame}<br>${fmt(e.duration_ns/1e3)} µs`),hovertemplate:'%{y}<br>%{x:.6f} s<br>%{text}<extra></extra>'}],layout('', 'Zone'),{responsive:true,displaylogo:false})}
$('theme').onclick=()=>{document.documentElement.dataset.theme=theme()==='dark'?'light':'dark';$('theme').textContent=theme()==='dark'?'Light mode':'Dark mode';localStorage.setItem('ddnet-trace-theme',theme());draw()};document.documentElement.dataset.theme=localStorage.getItem('ddnet-trace-theme')||'dark';$('theme').textContent=theme()==='dark'?'Light mode':'Dark mode';draw();
</script>
</body></html>"""


def generate(input_path: Path, output_path: Path) -> None:
	trace = json.loads(input_path.read_text(encoding="utf-8"))
	if trace.get("format") != "ddnet-render-trace" or trace.get("version") != 1:
		raise ValueError("unsupported render trace format")
	title = f"Render trace — {input_path.name}"
	data = json.dumps(trace, separators=(",", ":")).replace("</", "<\\/").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
	output_path.write_text(TEMPLATE.replace("__TITLE__", html.escape(title)).replace("__TRACE_DATA__", data), encoding="utf-8")


def self_test() -> None:
	trace = {
		"format": "ddnet-render-trace",
		"version": 1,
		"dropped_frames": 0,
		"dropped_events": 0,
		"names": ["client/game"],
		"frames": [{"frame": 1, "timestamp_ns": 0, "frametime_ns": 8_000_000, "render_wall_ns": 500_000, "gpu_time_ns": 250_000, "gpu_supported": True}],
		"events": [{"frame": 1, "start_ns": 10, "duration_ns": 100, "name": 0}],
	}
	with tempfile.TemporaryDirectory() as directory:
		source = Path(directory) / "trace.json"
		report = Path(directory) / "report.html"
		source.write_text(json.dumps(trace), encoding="utf-8")
		generate(source, report)
		output = report.read_text(encoding="utf-8")
		assert "DDNet render trace" in output and '"client/game"' in output and "Plotly.react" in output


def main() -> None:
	parser = argparse.ArgumentParser(description="Generate an interactive HTML report from a DDNet render trace")
	parser.add_argument("trace", nargs="?", type=Path)
	parser.add_argument("-o", "--output", type=Path)
	parser.add_argument("--self-test", action="store_true")
	args = parser.parse_args()
	if args.self_test:
		self_test()
		return
	if args.trace is None:
		parser.error("trace is required unless --self-test is used")
	output = args.output or args.trace.with_suffix(".html")
	generate(args.trace, output)
	print(output)


if __name__ == "__main__":
	main()
