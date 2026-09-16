#!/usr/bin/env python3
"""Turn a render trace into an answer.

A frame trace is opened with one question: some frames cost too much, which
component is it, and is it the CPU or the GPU. This report is built around that
question rather than around the file's contents - it leads with the tail, says
what the slow frames spend that the median frame does not, and puts a
component's two halves on one row.

Everything is aggregated here. The page carries the numbers it draws and not the
trace, so a full ring buffer is a report of tens of kilobytes rather than tens of
megabytes; and it loads nothing from anywhere, because the machine worth
profiling is not always a machine that is online.

    python scripts/trace_report.py trace.json [-o report.html]
    python scripts/trace_report.py after.json --against before.json
    python scripts/trace_report.py --self-test
"""

import argparse
import html
import json
import math
import pathlib
import statistics
import sys
import tempfile

FORMAT = "ddnet-render-trace"
VERSION = 2
STRIP_COLUMNS = 600
WORST_FRAMES = 15
# A frame is a hitch rather than part of the tail when it costs this many times
# the 99th percentile. The two are different findings - one is a stall worth
# finding the cause of, the other is a steady cost worth shaving - and a mean
# over a tail that contains a stall reports the stall and calls it the tail.
HITCH_FACTOR = 4
# Rows below this at their 99th percentile and in the tail are not shown: a
# table of forty zeros hides the four rows that are not.
NEGLIGIBLE_NS = 10_000
# Zones that bracket other zones. They are timed for the debug overlay, and
# adding them to the zones inside them counts the same work twice.
UMBRELLA_ZONES = {"world", "interface"}

# The counters worth correlating against frametime. The rest of the trace's
# fields are either memory levels, which drift rather than spike, or the timings
# this report already breaks down.
COUNTERS = [
	("draw_calls", "draw calls"),
	("triangles", "triangles"),
	("render_passes", "render passes"),
	("streamed_bytes", "streamed bytes"),
	("upload_bytes", "uploaded bytes"),
	("buffer_updates", "buffer updates"),
	("buffer_recreates", "buffer recreates"),
	("texture_creates", "texture creates"),
	("texture_updates", "texture updates"),
	("text_layout_calls", "text layouts"),
	("glyphs", "glyphs laid out"),
	("text_creates", "text container creates"),
	("text_soft_recreates", "text container recreates"),
]


def percentile(values_sorted, fraction):
	"""Nearest-rank, which is the only kind that never invents a value."""
	if not values_sorted:
		return 0
	index = min(len(values_sorted) - 1, max(0, math.ceil(fraction * len(values_sorted)) - 1))
	return values_sorted[index]


def self_times(events, num_frames):
	"""Exclusive time per name per frame.

	Scopes nest - the frame's own scope contains every component's - so the
	durations as recorded cannot be added up: a parent counts its children a
	second time. Subtracting each event from whichever event directly contains
	it leaves times that sum to the frame, which is what a breakdown has to do
	to be one.
	"""
	per_frame = [{} for _ in range(num_frames)]
	by_frame = {}
	for event in events:
		by_frame.setdefault(event["frame"], []).append(event)
	for frame, frame_events in by_frame.items():
		if not 0 <= frame < num_frames:
			continue
		frame_events.sort(key=lambda e: (e["start_ns"], -e["duration_ns"]))
		own = [e["duration_ns"] for e in frame_events]
		stack = []
		for index, event in enumerate(frame_events):
			start = event["start_ns"]
			while stack:
				top = frame_events[stack[-1]]
				if top["start_ns"] + top["duration_ns"] > start:
					break
				stack.pop()
			if stack:
				own[stack[-1]] -= event["duration_ns"]
			stack.append(index)
		out = per_frame[frame]
		for event, nanoseconds in zip(frame_events, own):
			out[event["name"]] = out.get(event["name"], 0) + max(0, nanoseconds)
	return per_frame


def load(path):
	trace = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
	if trace.get("format") != FORMAT:
		raise SystemExit(f"{path}: not a {FORMAT}")
	if trace.get("version") != VERSION:
		raise SystemExit(f"{path}: format version {trace.get('version')}, this reads {VERSION}")
	return trace


def analyse(trace, label):
	frames = trace["frames"]
	if not frames:
		raise SystemExit(f"{label}: the trace holds no frames")
	names = trace["names"]
	zone_names = trace["gpu_zone_names"]
	name_zones = trace.get("name_zones", [len(zone_names)] * len(names))

	# Frames are keyed by the client's own counter; index them from zero so that
	# the events, which carry the same counter, line up with the array.
	first = frames[0]["frame"]
	events = [dict(e, frame=e["frame"] - first) for e in trace["events"]]
	own_times = self_times(events, len(frames))

	frametimes = [f["frametime_ns"] for f in frames]
	ordered = sorted(frametimes)
	# A frametime is the time since the previous frame began, so the work a frame
	# does is paid for in the frametime of the frame after it: on a real trace the
	# two correlate at 0.99 shifted by one and at 0.30 in step. Anything that
	# blames a component for a slow frame has to use the shifted cost, or it
	# blames the frame after the slow one, which did nothing. The last frame has
	# no successor and stands in with its own render time.
	cost = frametimes[1:] + [frames[-1]["render_wall_ns"]]
	cost_ordered = sorted(cost)
	p99 = percentile(cost_ordered, 0.99)
	hitch_limit = HITCH_FACTOR * p99
	hitches = [i for i, c in enumerate(cost) if c > hitch_limit]
	tail = [i for i, c in enumerate(cost) if p99 <= c <= hitch_limit]
	# Against the middle rather than the mean, and by medians rather than means:
	# an average the tail is part of moves with the tail, and one stall in two
	# hundred frames is most of a mean and none of a median.
	low, high = percentile(cost_ordered, 0.45), percentile(cost_ordered, 0.55)
	base = [i for i, c in enumerate(cost) if low <= c <= high] or list(range(len(frames)))

	def median_of(indices, series):
		return statistics.median(series[i] for i in indices) if indices else 0.0

	def mean_of(indices, series):
		return statistics.fmean(series[i] for i in indices) if indices else 0.0

	components = {}
	for index, name in enumerate(names):
		zone = name_zones[index] if index < len(name_zones) else len(zone_names)
		series = [own_times[f].get(index, 0) for f in range(len(frames))]
		if not any(series):
			continue
		components[name] = {
			"name": name,
			"zone": zone_names[zone] if zone < len(zone_names) else None,
			"p50": percentile(sorted(series), 0.50),
			"p99": percentile(sorted(series), 0.99),
			"tail": median_of(tail, series) - median_of(base, series),
		}

	zones = {}
	gpu_supported = any(f.get("gpu_supported") for f in frames)
	if gpu_supported:
		for index, zone_name in enumerate(zone_names):
			series = [f["gpu_zones_ns"][index] if index < len(f["gpu_zones_ns"]) else 0 for f in frames]
			if not any(series):
				continue
			zones[zone_name] = {
				"name": zone_name,
				"p50": percentile(sorted(series), 0.50),
				"p99": percentile(sorted(series), 0.99),
				"tail": median_of(tail, series) - median_of(base, series),
			}

	counters = []
	for key, title in COUNTERS:
		series = [f.get(key, 0) for f in frames]
		if len(set(series)) < 2:
			continue
		try:
			correlation = statistics.correlation(series, cost)
		except (statistics.StatisticsError, ValueError, ZeroDivisionError):
			continue
		counters.append({
			"title": title,
			"r": correlation,
			"base": mean_of(base, series),
			"tail": mean_of(tail, series),
		})
	counters.sort(key=lambda c: -abs(c["r"]))

	# One column per screen pixel rather than one point per frame, and the
	# column keeps its maximum, because the maximum is what the tail is.
	stride = max(1, math.ceil(len(frames) / STRIP_COLUMNS))
	strip = []
	for start in range(0, len(frames), stride):
		chunk = cost[start : start + stride]
		peak = max(chunk)
		at = start + chunk.index(peak)
		spent = sorted(((names[n], t) for n, t in own_times[at].items() if t > 0), key=lambda p: -p[1])[:3]
		seconds = (frames[at]["timestamp_ns"] - frames[0]["timestamp_ns"]) / 1e9
		what = ", ".join(f"{name} {t / 1e6:.2f} ms" for name, t in spent) or "no scope"
		strip.append([peak, statistics.median(chunk), f"frame {frames[at]['frame']} at {seconds:.1f} s: {what}"])

	# The hitches if there are any, since they are what a person will want to see
	# first; otherwise the slowest frames, which are then the tail.
	listed = sorted(hitches, key=lambda i: -cost[i]) if hitches else sorted(range(len(frames)), key=lambda i: -cost[i])
	worst = []
	for index in listed[:WORST_FRAMES]:
		frame = frames[index]
		parts = sorted(((names[n], t) for n, t in own_times[index].items() if t > 0), key=lambda p: -p[1])
		worst.append({
			"frame": frame["frame"],
			"at": (frame["timestamp_ns"] - frames[0]["timestamp_ns"]) / 1e9,
			"cost": cost[index],
			"render": frame["render_wall_ns"],
			"gpu": frame["gpu_time_ns"],
			"parts": parts[:8],
			# What no scope explains. A stall that lands here is not in any
			# component the tracer knows about, and saying so is worth more than
			# pointing at whichever component happened to be largest.
			"outside": max(0, cost[index] - sum(t for _, t in parts)),
			"index": index,
		})

	return {
		"label": label,
		"frames": len(frames),
		"wall": (frames[-1]["timestamp_ns"] - frames[0]["timestamp_ns"]) / 1e9,
		"dropped_frames": trace.get("dropped_frames", 0),
		"dropped_events": trace.get("dropped_events", 0),
		"mailbox_dropped": frames[-1]["frames_dropped"] - frames[0]["frames_dropped"],
		"mailbox_produced": frames[-1]["frames_produced"] - frames[0]["frames_produced"],
		"gpu_supported": gpu_supported,
		"percentiles": {name: percentile(ordered, f) for name, f in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99), ("max", 1.0))},
		"components": components,
		"zones": zones,
		"counters": counters,
		"strip": strip,
		"worst": worst,
		"tail_frames": len(tail),
		"tail_cost": median_of(tail, cost),
		"base_cost": median_of(base, cost),
		"hitches": len(hitches),
		"hitch_limit": hitch_limit,
		"strip_ceiling": hitch_limit,
	}


def rows(report, against):
	"""The component table: a GPU zone and the CPU scopes that draw into it.

	Several components share a zone, so a zone is a heading with its own GPU
	numbers and the components are the CPU half underneath it. Everything the
	GPU never timed - the update-only components, and every component at all
	when the driver has no timer - falls into one group at the end rather than
	being given a zone it does not have.
	"""
	grouped = {}
	for component in report["components"].values():
		grouped.setdefault(component["zone"], []).append(component)

	def delta(now, before, key):
		if before is None:
			return None
		return now[key] - before[key]

	def visible(item):
		return item["p99"] >= NEGLIGIBLE_NS or abs(item["tail"]) >= NEGLIGIBLE_NS

	out = []
	hidden = 0
	for zone_name, members in grouped.items():
		zone = report["zones"].get(zone_name)
		shown = [c for c in members if visible(c)]
		hidden += len(members) - len(shown)
		if not shown and not (zone and visible(zone)):
			continue
		shown.sort(key=lambda c: (-c["tail"], -c["p99"]))
		was = (against or {}).get("zones", {}).get(zone_name) if zone else None
		out.append({
			"name": zone_name or "not drawn, or not timed by the driver",
			"gpu": zone,
			"gpu_delta": delta(zone, was, "p99") if zone and was else None,
			# The zone's GPU time and its components' CPU time are on different
			# clocks, so they are never added; a group sorts by whichever half of
			# it the tail leans on harder.
			"order": max(zone["tail"] if zone else 0, sum(c["tail"] for c in shown)),
			"members": [{"component": c, "delta": delta(c, (against or {}).get("components", {}).get(c["name"]), "p99")} for c in shown],
		})
	out.sort(key=lambda g: -g["order"])
	return out, hidden


def ms(nanoseconds):
	return f"{nanoseconds / 1000000.0:.2f}"


def signed_ms(nanoseconds):
	return f"{nanoseconds / 1000000.0:+.2f}"


def render(report, against):
	e = html.escape
	p = report["percentiles"]
	out = []
	add = out.append

	add(f"<h1>{e(report['label'])}</h1>")
	subtitle = f"{report['frames']} frames over {report['wall']:.1f} s"
	if against:
		subtitle += f" &middot; compared against <b>{e(against['label'])}</b>"
	add(f"<p class=sub>{subtitle}</p>")

	for warning in warnings(report):
		add(f"<p class=warn>{e(warning)}</p>")

	# The verdict. Percentiles first because the tail is the complaint, then what
	# the tail is made of, which is the part the numbers alone never say.
	add("<div class=cards>")
	for name, title in (("p50", "median"), ("p95", "p95"), ("p99", "p99"), ("max", "worst")):
		compare = ""
		if against:
			compare = f"<small>{signed_ms(p[name] - against['percentiles'][name])} ms</small>"
		add(f"<div class=card><b>{ms(p[name])} ms</b><span>{title} frame</span>{compare}</div>")
	add("</div>")

	add(verdict(report))

	add("<h2>Every frame</h2>")
	add(
		f"<p class=sub>Each column is a slice of the trace and stands for that slice&rsquo;s worst frame; the line is the slice&rsquo;s median. Columns above {ms(report['strip_ceiling'])} ms are hitches and are clipped, in red, so that one stall does not flatten the rest. Click a column for the frame behind it.</p>"
	)
	add("<canvas id=strip height=160></canvas><div id=picked class=picked></div>")

	add("<h2>Where the time goes</h2>")
	add(
		"<p class=sub>Grouped by GPU zone, with the CPU scopes that draw into it underneath. CPU times are exclusive, so a column adds up to the frame. <b>Tail</b> is what a typical frame of the slowest 1&nbsp;% spends on this that a median frame does not, hitches left out &mdash; the column to sort your afternoon by. A zone row is GPU time and the rows under it are CPU time; the two are never added.</p>"
	)
	add(table(report, against))

	if report["counters"]:
		add("<h2>What else moved with the frametime</h2>")
		add("<p class=sub>Pearson correlation across every frame, and what the tail does differently. A counter near &plusmn;1 rises and falls with the frametime; near 0 it does not, whatever its own graph looks like.</p>")
		add(counters(report))

	add("<h2>Hitches</h2>" if report["hitches"] else "<h2>The slowest frames</h2>")
	if report["hitches"]:
		add(f"<p class=sub>Frames that cost more than {HITCH_FACTOR}&times; the 99th percentile. Each row is the frame that did the work, which is the one before the long frametime.</p>")
	add(worst(report))
	return "\n".join(out)


def warnings(report):
	out = []
	if report["dropped_frames"]:
		out.append(f"The frame ring overflowed: {report['dropped_frames']} frames were overwritten and are not in this report. Trace for less time.")
	if report["dropped_events"]:
		out.append(f"The event ring overflowed: {report['dropped_events']} scopes were overwritten, so the breakdown below is missing part of what it measures.")
	if report["mailbox_dropped"]:
		produced = report["mailbox_produced"] or report["frames"]
		out.append(f"{report['mailbox_dropped']} of {produced} frames were drawn by the game thread and replaced before the render thread took them. The game thread is outrunning the renderer, so these frametimes describe the game thread, not what reached the screen.")
	if not report["gpu_supported"]:
		out.append("The driver reported no GPU timer, so every GPU column here is empty. The CPU side is still measured.")
	return out


def verdict(report):
	"""The paragraphs a person came for: the stalls, then the steady tail."""
	out = []
	e = html.escape
	if report["hitches"]:
		first = report["worst"][0]
		cause = ", ".join(f"<b>{e(name)}</b> {ms(t)} ms" for name, t in first["parts"][:2]) or "no scope this trace measures"
		many = f"{report['hitches']} hitches" if report["hitches"] > 1 else "One hitch"
		out.append(f"<p class=verdict><b>{many}.</b> The worst, {first['at']:.1f}&nbsp;s in, cost <b>{ms(first['cost'])} ms</b>, and that frame spent it on {cause}. A hitch is a stall rather than a steady cost: look for what that component did once, not for what it does every frame.</p>")
		blind = [h for h in report["worst"] if h["outside"] * 2 > h["cost"]]
		if blind:
			out.append(
				f"<p class=verdict><b>{len(blind)} of these are mostly outside every scope</b> &mdash; the worst leaves {ms(max(h['outside'] for h in blind))} ms that no component accounts for. The tracer does not cover where those stalls happen, so no component in the table below is their cause; the next scope to add is the one that would contain them.</p>"
			)

	excess = report["tail_cost"] - report["base_cost"]
	lead = "Apart from those" if report["hitches"] else "Otherwise"
	if excess <= NEGLIGIBLE_NS:
		out.append(f"<p class=verdict>{lead}, the frametime has no tail worth the name: the slowest percentile costs what the median does.</p>")
		return "".join(out)

	def movers(items):
		return [m for m in sorted(items, key=lambda m: -m[1]) if m[1] >= NEGLIGIBLE_NS][:3]

	cpu = movers([(c["name"], c["tail"]) for c in report["components"].values()])
	gpu = movers([(z["name"], z["tail"]) for z in report["zones"].values() if z["name"] not in UMBRELLA_ZONES])
	listed = []
	if cpu:
		listed.append("on the CPU to " + ", ".join(f"<b>{e(n)}</b> {signed_ms(t)} ms" for n, t in cpu))
	if gpu:
		listed.append("on the GPU to " + ", ".join(f"<b>{e(n)}</b> {signed_ms(t)} ms" for n, t in gpu))
	spent = "; ".join(listed) if listed else "to nothing any scope measures &mdash; it is spent outside all of them"
	out.append(f"<p class=verdict>{lead}, a typical frame of the slowest 1&nbsp;% costs <b>{ms(report['tail_cost'])} ms</b> against <b>{ms(report['base_cost'])} ms</b> for a median one. The difference goes {spent}.</p>")
	return "".join(out)


def table(report, against):
	groups, hidden = rows(report, against)
	head = "<th>component</th><th>CPU p50</th><th>CPU p99</th><th>GPU p50</th><th>GPU p99</th><th>tail</th>"
	out = ["<table class=grid><thead><tr>" + head + ("<th>&Delta; p99</th>" if against else "") + "</tr></thead><tbody>"]

	def tail_cell(value):
		return f"<td class='{'hot' if value >= NEGLIGIBLE_NS else ''}'>{signed_ms(value)}</td>"

	for group in groups:
		gpu = group["gpu"]
		out.append(f"<tr class=zone><td>{html.escape(group['name'])}</td><td></td><td></td>")
		out.append(f"<td>{ms(gpu['p50']) if gpu else ''}</td><td>{ms(gpu['p99']) if gpu else ''}</td>")
		out.append(tail_cell(gpu["tail"]) if gpu else "<td></td>")
		if against:
			out.append(f"<td>{signed_ms(group['gpu_delta']) if group['gpu_delta'] is not None else ''}</td>")
		out.append("</tr>")
		for member in group["members"]:
			c = member["component"]
			out.append(f"<tr><td class=indent>{html.escape(c['name'])}</td><td>{ms(c['p50'])}</td><td>{ms(c['p99'])}</td><td></td><td></td>")
			out.append(tail_cell(c["tail"]))
			if against:
				d = member["delta"]
				out.append(f"<td class='{'hot' if d and d > 0 else ''}'>{signed_ms(d) if d is not None else 'new'}</td>")
			out.append("</tr>")
	note = f" {hidden} components that never reach {NEGLIGIBLE_NS // 1000}&nbsp;&micro;s are left out." if hidden else ""
	out.append(f"</tbody></table><p class=sub>All times in milliseconds.{note}</p>")
	return "".join(out)


def counters(report):
	out = ["<table class=grid><thead><tr><th>counter</th><th>r</th>" + "<th>median frame</th><th>slowest 1&nbsp;%</th></tr></thead><tbody>"]
	for counter in report["counters"]:
		strong = "hot" if abs(counter["r"]) >= 0.5 else ""
		out.append(f"<tr><td>{html.escape(counter['title'])}</td><td class='{strong}'>{counter['r']:+.2f}</td><td>{counter['base']:,.0f}</td><td>{counter['tail']:,.0f}</td></tr>")
	out.append("</tbody></table>")
	return "".join(out)


def worst(report):
	out = ["<table class=grid><thead><tr><th>at</th><th>frame</th><th>cost</th><th>outside scopes</th><th>GPU</th><th>spent on</th></tr></thead><tbody>"]
	for frame in report["worst"]:
		spent = ", ".join(f"{html.escape(name)} {ms(t)}" for name, t in frame["parts"]) or "&mdash;"
		blind = "hot" if frame["outside"] * 2 > frame["cost"] else ""
		out.append(f"<tr><td>{frame['at']:.1f}&nbsp;s</td><td>{frame['frame']}</td><td class=hot>{ms(frame['cost'])}</td><td class='{blind}'>{ms(frame['outside'])}</td><td>{ms(frame['gpu'])}</td><td class=spent>{spent}</td></tr>")
	out.append("</tbody></table><p class=sub>All times in milliseconds. Cost is the frametime this frame caused, which the client records against the frame after it. Outside scopes is the part of it no component accounts for, marked when it is most of the frame.</p>")
	return "".join(out)


TEMPLATE = """<!doctype html>
<html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<style>
:root{color-scheme:light dark;--bg:#fbfbfa;--fg:#1a1a1a;--dim:#6a6a6a;--line:#e0e0dd;
--hot:#b3261e;--zone:#f2f2ef;--accent:#2b6cb0}
@media(prefers-color-scheme:dark){:root{--bg:#16181a;--fg:#e8e8e6;--dim:#9a9a98;
--line:#2c2f33;--hot:#ff8a80;--zone:#1e2124;--accent:#7fb3e8}}
*{box-sizing:border-box}
body{margin:0;padding:24px 20px 64px;background:var(--bg);color:var(--fg);
font:14px/1.5 -apple-system,Segoe UI,system-ui,sans-serif;max-width:1040px;margin-inline:auto}
h1{font-size:20px;margin:0 0 2px}h2{font-size:15px;margin:32px 0 6px;letter-spacing:.02em}
.sub{color:var(--dim);margin:0 0 12px}
.warn{border-left:3px solid var(--hot);padding:8px 12px;margin:8px 0;background:var(--zone)}
.verdict{border-left:3px solid var(--accent);padding:10px 12px;margin:14px 0;background:var(--zone)}
.cards{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}
.card{flex:1 1 120px;border:1px solid var(--line);border-radius:6px;padding:10px 12px}
.card b{display:block;font-size:19px}
.card span{color:var(--dim);font-size:12px}
.card small{display:block;color:var(--dim)}
canvas{width:100%;border:1px solid var(--line);border-radius:6px;display:block}
.picked{color:var(--dim);min-height:2.6em;padding-top:8px}
table.grid{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
.grid th{text-align:right;font-weight:600;color:var(--dim);border-bottom:1px solid var(--line);
padding:6px 8px;font-size:12px}
.grid th:first-child,.grid td:first-child{text-align:left}
.grid td{text-align:right;padding:4px 8px;border-bottom:1px solid var(--line)}
tr.zone{background:var(--zone)}tr.zone td{font-weight:600}
td.indent{padding-left:24px;color:var(--dim)}
td.hot{color:var(--hot)}
td.spent{text-align:left;color:var(--dim);font-size:12px}
@media(max-width:640px){body{padding:16px 12px 48px}.grid{font-size:12px}}
</style></head><body>
__BODY__
<script>
const STRIP = __STRIP__, CEILING = __CEILING__ || 1;
const canvas = document.getElementById("strip"), picked = document.getElementById("picked");
function draw(){
	const ratio = window.devicePixelRatio || 1;
	const width = canvas.clientWidth, height = 160;
	canvas.width = width * ratio; canvas.height = height * ratio;
	const ctx = canvas.getContext("2d"); ctx.scale(ratio, ratio);
	const style = getComputedStyle(document.documentElement);
	const step = width / STRIP.length, usable = height - 20;
	const accent = style.getPropertyValue("--accent").trim(), hot = style.getPropertyValue("--hot").trim();
	ctx.clearRect(0, 0, width, height);
	STRIP.forEach((column, i) => {
		const clipped = column[0] > CEILING;
		const bar = Math.max(1, Math.min(column[0], CEILING) / CEILING * usable);
		ctx.fillStyle = clipped ? hot : accent;
		ctx.globalAlpha = clipped ? 1 : 0.85;
		ctx.fillRect(i * step, height - bar, Math.max(1, step - 0.5), bar);
	});
	ctx.globalAlpha = 1;
	ctx.strokeStyle = style.getPropertyValue("--fg").trim(); ctx.lineWidth = 1;
	ctx.beginPath();
	STRIP.forEach((column, i) => {
		const y = height - Math.max(1, Math.min(column[1], CEILING) / CEILING * usable);
		i ? ctx.lineTo(i * step + step / 2, y) : ctx.moveTo(step / 2, y);
	});
	ctx.stroke();
	ctx.fillStyle = style.getPropertyValue("--dim").trim(); ctx.font = "11px sans-serif";
	ctx.fillText((CEILING / 1e6).toFixed(2) + " ms", 6, 14);
}
canvas.addEventListener("click", event => {
	const box = canvas.getBoundingClientRect();
	const index = Math.min(STRIP.length - 1,
		Math.floor((event.clientX - box.left) / box.width * STRIP.length));
	const column = STRIP[index];
	picked.textContent = "Worst " + (column[0] / 1e6).toFixed(2) + " ms, median "
		+ (column[1] / 1e6).toFixed(2) + " ms \u2014 " + column[2];
});
draw();
addEventListener("resize", draw);
// A canvas keeps the colours it was drawn in, so a change of theme has to draw it again.
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", draw);
</script></body></html>
"""


def generate(trace, label, against=None, against_label=None):
	report = analyse(trace, label)
	other = analyse(against, against_label) if against is not None else None
	# The ceiling is the hitch limit, but never below the tallest column that is
	# not a hitch, so a trace without any still fills its own height.
	steady = [c[0] for c in report["strip"] if c[0] <= report["strip_ceiling"]]
	ceiling = max(steady) if steady else report["strip_ceiling"]
	return TEMPLATE.replace("__TITLE__", html.escape(label)).replace("__BODY__", render(report, other)).replace("__STRIP__", json.dumps(report["strip"])).replace("__CEILING__", json.dumps(ceiling))


def synthetic(stall_at=None):
	"""A trace whose tail is one component's fault, recorded the way the client
	records it: a frame's work is paid for in the frametime of the frame after.

	With a stall, one frame additionally spends a long time in the players
	component, which has to come out as a hitch and stay out of the tail.
	"""
	names = ["client/game", "world/players", "interface/hud"]
	frames, events, work, now = [], [], [], 0
	for i in range(400):
		slow = i % 50 == 0
		hud = 9_000_000 if slow else 400_000
		players = 600_000 + (400_000_000 if i == stall_at else 0)
		work.append(hud + players)
		events += [
			{"frame": i, "start_ns": 0, "duration_ns": hud + players, "name": 0},
			{"frame": i, "start_ns": 0, "duration_ns": players, "name": 1},
			{"frame": i, "start_ns": players, "duration_ns": hud, "name": 2},
		]
		# The time since the previous frame began, which is the previous frame's work.
		frametime = 2_000_000 + (work[i - 1] if i else work[0])
		now += frametime
		frames.append({
			"frame": i,
			"timestamp_ns": now,
			"frametime_ns": frametime,
			"render_wall_ns": hud + players,
			"gpu_time_ns": 1_000_000,
			"gpu_zones_ns": [0, 700_000 if slow else 100_000],
			"gpu_supported": True,
			"frames_produced": i,
			"frames_dropped": 0,
			"draw_calls": 900 if slow else 100,
			"triangles": 5,
			"streamed_bytes": 10,
		})
	return {"format": FORMAT, "version": VERSION, "dropped_frames": 0, "dropped_events": 0, "names": names, "name_zones": [2, 0, 1], "gpu_zone_names": ["world", "hud", "none"], "frames": frames, "events": events}


def self_test():
	trace = synthetic()
	report = analyse(trace, "synthetic")

	# Exclusive time: the parent scope covers both children, so its own share is
	# nothing and the two children keep their full durations.
	own = self_times(trace["events"], len(trace["frames"]))
	assert own[1] == {0: 0, 1: 600_000, 2: 400_000}, own[1]

	# The tail is the hud's doing, and the report has to say so rather than blame
	# the frame scope that contains it. This only holds if the frametime is read
	# one frame late: read in step, the slow frames are the ones after the hud's.
	movers = sorted(report["components"].values(), key=lambda c: -c["tail"])
	assert movers[0]["name"] == "interface/hud", [c["name"] for c in movers]
	assert movers[0]["tail"] > 8_000_000, movers[0]["tail"]
	# And the scope that merely contains them gets no row: it spends nothing of
	# its own, which is the point of measuring exclusively.
	assert "client/game" not in report["components"]
	assert report["hitches"] == 0

	# A component is joined to the zone it draws into, not to one that happens to
	# share its name.
	assert report["components"]["interface/hud"]["zone"] == "hud"
	assert report["components"]["world/players"]["zone"] == "world"

	# Draw calls move with the cost here and triangles do not; a report that
	# cannot tell those apart is the one this replaced.
	by_title = {c["title"]: c["r"] for c in report["counters"]}
	assert by_title["draw calls"] > 0.9, by_title
	assert "triangles" not in by_title

	# A stall is a hitch, blamed on the frame that stalled rather than on the one
	# whose frametime shows it, and it leaves the tail alone: a mean would have
	# charged 400 ms across a handful of tail frames to the players component.
	stalled = analyse(synthetic(stall_at=123), "stalled")
	assert stalled["hitches"] == 1, stalled["hitches"]
	assert stalled["worst"][0]["frame"] == 123, stalled["worst"][0]
	assert stalled["worst"][0]["parts"][0][0] == "world/players", stalled["worst"][0]["parts"]
	# That stall is inside a scope, so it must not be reported as one the tracer
	# cannot see; only the two milliseconds of unscoped frame overhead are left.
	assert stalled["worst"][0]["outside"] < 3_000_000, stalled["worst"][0]["outside"]
	assert stalled["components"]["world/players"]["tail"] < 1_000_000, stalled["components"]["world/players"]
	assert stalled["components"]["interface/hud"]["tail"] > 8_000_000

	# And it all comes out as one file that needs nothing from anywhere.
	page = generate(synthetic(stall_at=123), "synthetic")
	assert "interface/hud" in page and "One hitch" in page
	assert "http://" not in page and "https://" not in page, "the report must not fetch anything"
	with tempfile.TemporaryDirectory() as directory:
		out = pathlib.Path(directory) / "report.html"
		out.write_text(page, encoding="utf-8")
		assert out.stat().st_size < 200_000, out.stat().st_size
	print("self-test passed")


def main():
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument("trace", nargs="?", help="a trace written by render_trace_stop")
	parser.add_argument("-o", "--output", help="where to write the report (default: next to the trace)")
	parser.add_argument("--against", help="a second trace to compare this one against")
	parser.add_argument("--self-test", action="store_true", help="check the analysis and exit")
	args = parser.parse_args()

	if args.self_test:
		self_test()
		return 0
	if not args.trace:
		parser.error("a trace is required")

	trace = load(args.trace)
	against = load(args.against) if args.against else None
	page = generate(trace, pathlib.Path(args.trace).name, against, pathlib.Path(args.against).name if args.against else None)
	output = pathlib.Path(args.output) if args.output else pathlib.Path(args.trace).with_suffix(".html")
	output.write_text(page, encoding="utf-8")
	print(f"{output} ({output.stat().st_size // 1024} KiB)")
	return 0


if __name__ == "__main__":
	sys.exit(main())
