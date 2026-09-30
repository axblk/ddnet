/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// The DDNet demo player. The API is documented in demo-player.d.ts; the other
// end of the calls is the `DemoPlayer*` block in
// `src/engine/client/demo_player_client.cpp`.

import { addIcons, autoHide, demoInfoSections, exportSettingsForm, fillInfo, fullscreen, paintIcons, Program, ViewerElement } from "@ddnet/base";

addIcons({
	play: '<path d="M7.5 3.8 20.5 12 7.5 20.2Z"/>',
	pause: '<rect x="5.5" y="3.5" width="4.4" height="17" rx="2.2"/><rect x="14.1" y="3.5" width="4.4" height="17" rx="2.2"/>',
	restart: '<rect x="3.5" y="3.5" width="3.2" height="17" rx="1.6"/><path d="M20.5 3.8 20.5 20.2 8.4 12Z"/>',
	eye: '<path d="M12 4.6C5.6 4.6 1.8 12 1.8 12s3.8 7.4 10.2 7.4S22.2 12 22.2 12 18.4 4.6 12 4.6Z"/><circle cx="12" cy="12" r="2.7" fill="#000"/>',
	freeview: '<path d="M12 1.6 15.2 6.2H8.8ZM12 22.4 8.8 17.8h6.4ZM1.6 12 6.2 8.8v6.4ZM22.4 12 17.8 15.2V8.8Z"/><rect x="10.9" y="4.6" width="2.2" height="14.8" rx="1.1"/><rect x="4.6" y="10.9" width="14.8" height="2.2" rx="1.1"/>',
	zoom_reset: '<path d="M9.6 3.4v6.2H3.4M14.4 3.4v6.2h6.2M9.6 20.6v-6.2H3.4M14.4 20.6v-6.2h6.2" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>',
	volume: '<path d="M3 9.2h3.4L11.4 5v14L6.4 14.8H3Z"/><path d="M15.2 9.2a4 4 0 0 1 0 5.6M18 6.4a8 8 0 0 1 0 11.2" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/>',
	volume_off: '<path d="M3 9.2h3.4L11.4 5v14L6.4 14.8H3Z"/><path d="M15.4 9.6 20.6 14.8M20.6 9.6 15.4 14.8" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/>',
	clip_start: '<rect x="4.4" y="3.6" width="2.8" height="16.8" rx="1.4"/><path d="M9.4 7.2h9.4a1.4 1.4 0 0 1 1.4 1.4v6.8a1.4 1.4 0 0 1-1.4 1.4H9.4Z" opacity="0.55"/>',
	clip_end: '<rect x="16.8" y="3.6" width="2.8" height="16.8" rx="1.4"/><path d="M14.6 7.2H5.2a1.4 1.4 0 0 0-1.4 1.4v6.8a1.4 1.4 0 0 0 1.4 1.4h9.4Z" opacity="0.55"/>',
	clip_clear: '<rect x="4.4" y="3.6" width="2.8" height="16.8" rx="1.4"/><rect x="16.8" y="3.6" width="2.8" height="16.8" rx="1.4"/><path d="M9.6 9.6 14.4 14.4M14.4 9.6 9.6 14.4" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/>',
	settings: '<path d="M10.3 2.5h3.4l.5 2.6 1.9.8 2.2-1.5 2.4 2.4-1.5 2.2.8 1.9 2.6.5v3.4l-2.6.5-.8 1.9 1.5 2.2-2.4 2.4-2.2-1.5-1.9.8-.5 2.6h-3.4l-.5-2.6-1.9-.8-2.2 1.5-2.4-2.4 1.5-2.2-.8-1.9-2.6-.5v-3.4l2.6-.5.8-1.9-1.5-2.2 2.4-2.4 2.2 1.5 1.9-.8Z"/><circle cx="12" cy="12" r="3.2" fill="#000"/>',
	back: '<path d="M14.8 5 7.8 12l7 7" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/>',
	close: '<path d="M6 6 18 18M18 6 6 18" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>',
	info: '<circle cx="12" cy="12" r="9.6" fill="none" stroke="currentColor" stroke-width="2.2"/><rect x="10.8" y="10.4" width="2.4" height="7.2" rx="1.2"/><circle cx="12" cy="7.4" r="1.5"/>',
});

const PROGRAM = "ddnet-demo-player.js";

// How often the program is asked what changed; the fire rate of `timeupdate`
// in a browser is between 4 and 66 Hz.
const POLL_INTERVAL_MS = 200;
// A demo stops on its last frame, which is not quite at its length.
const END_OF_DEMO = 0.999;
// `CDemoClientBase::Spectating`: the player who recorded the demo, and the
// free view.
const SPEC_FOLLOW = -2;
const SPEC_FREEVIEW = -1;

export const programUrl = new URL(PROGRAM, import.meta.url).href;

// The version of a live stream's `index.json` the player reads, which is
// `CLiveRecorder::INDEX_VERSION` of the server.
const LIVE_INDEX_VERSION = 1;
// How often the index of a live stream is read.
const LIVE_POLL_MS = 1000;
// How far behind the end of a live demo still counts as live: seeking there
// stops two seconds before the end, `CDemoPlayer::SetPos`, and the stream
// arrives a second at a time.
const LIVE_EDGE_SECONDS = 4;
// How close to the beginning of what was read of a live stream the older part
// is read, a poll after the viewer went there.
const LIVE_START_SECONDS = 3;

const sleep = (milliseconds, signal) => new Promise(resolve => {
	const timer = setTimeout(resolve, milliseconds);
	signal.addEventListener("abort", () => {
		clearTimeout(timer);
		resolve();
	}, { once: true });
});

function concatBytes(parts) {
	const result = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
	let offset = 0;
	for (const part of parts) {
		result.set(part, offset);
		offset += part.length;
	}
	return result;
}

// The length of the chunk of a demo at `offset` if all of it is there, else
// 0, see `CDemoPlayer::ReadChunkHeader`.
function chunkLength(bytes, offset) {
	const left = bytes.length - offset;
	if (left < 1) {
		return 0;
	}
	const chunk = bytes[offset];
	if (chunk & 0x80) {
		// A tick marker: a tick delta in the byte, or a whole tick after it.
		const length = chunk & 0x20 ? 1 : 5;
		return left >= length ? length : 0;
	}
	let size = chunk & 0x1f;
	let header = 1;
	if (size === 30) {
		if (left < 2) {
			return 0;
		}
		size = bytes[offset + 1];
		header = 2;
	} else if (size === 31) {
		if (left < 3) {
			return 0;
		}
		size = bytes[offset + 1] | (bytes[offset + 2] << 8);
		header = 3;
	}
	return left >= header + size ? header + size : 0;
}

// How many bytes from the beginning are whole chunks.
function wholeChunks(bytes) {
	let offset = 0;
	for (let length = chunkLength(bytes, 0); length > 0; length = chunkLength(bytes, offset)) {
		offset += length;
	}
	return offset;
}

/**
 * Follows a live stream that a server writes (`live_start`): it reads the
 * stream's `index.json` every second, puts the init of a map and its
 * segments together into one demo file of the program and appends to it
 * what the segments grow by, with range requests. Any static web server
 * does. A viewer who joins at the live end gets the last minutes of the map
 * (`backlogSeconds`), and older ones when seeking back to where they
 * begin; at the live end, what goes back further than `maxBacklogSeconds`
 * is dropped.
 */
export class LiveFeed extends EventTarget {
	// How much of a map a viewer who joins at the live end gets at first;
	// seeking back to where that begins brings twice as much, and so on.
	static backlogSeconds = 120;
	// How much of a map is kept while it plays at its live end: more than this
	// is dropped by opening the map's newest part again, so that a stream that
	// runs for days does not fill the memory.
	static maxBacklogSeconds = 30 * 60;

	constructor(player, url) {
		super();
		this.player = player;
		this.url = new URL(url, location.href);
		this.stopping = new AbortController();
		this.queue = Promise.resolve();
		this.index = null;
		// What the demo file of the program is: the stream and the map
		// (epoch) it is of, and how far each segment of it was read. A chunk
		// that is not whole yet waits in `tail`.
		this.stream = null;
		this.epoch = null;
		this.path = null;
		this.read = new Map();
		this.next = -1;
		this.tail = new Uint8Array(0);
		// The first segment in the file, and how many seconds back from the
		// newest it was cut off (`null` for not at all).
		this.firstRead = -1;
		this.backlog = null;
		// Whether all of the map is in the file: the stream went on with
		// another map, or ended.
		this.complete = false;
		this.state = "loading";
		this.error = "";
	}

	get signal() {
		return this.stopping.signal;
	}

	// The markers of the map that plays, `{ tick, kind, label }`.
	get markers() {
		return (this.index?.markers ?? []).filter(marker => marker.epoch === this.epoch);
	}

	// Whether the stream has older segments of the map that plays than the
	// file.
	get earlier() {
		return (this.index?.segments ?? []).some(segment => segment.epoch === this.epoch && segment.n < this.firstRead);
	}

	// Whether a newer map than the one that plays has begun.
	get newerMap() {
		return (this.index?.segments ?? []).some(segment => segment.epoch > this.epoch);
	}

	stop() {
		this.stopping.abort();
	}

	// One thing at a time: a poll, going live, going to the next map.
	serial(action) {
		const run = this.queue.then(() => this.signal.aborted ? undefined : action());
		this.queue = run.catch(() => {});
		return run;
	}

	changed() {
		this.dispatchEvent(new Event("livechange"));
	}

	async run() {
		while (!this.signal.aborted) {
			try {
				await this.serial(() => this.poll());
				this.error = "";
			} catch (error) {
				if (this.signal.aborted) {
					return;
				}
				this.error = error?.message ?? String(error);
				this.player.output(`Live stream: ${this.error}`, { error: true });
				this.changed();
			}
			if (this.state === "ended") {
				return;
			}
			await sleep(LIVE_POLL_MS, this.signal);
		}
	}

	async fetchIndex() {
		const response = await fetch(this.url, { cache: "no-store", signal: this.signal });
		if (!response.ok) {
			throw new Error(`the index answered ${response.status} ${response.statusText}`);
		}
		return await response.json();
	}

	// The bytes of a file of the stream from `from` on; `null` if it is gone.
	// A server without ranges sends the whole file.
	async fetchBytes(file, from) {
		const headers = from > 0 ? { Range: `bytes=${from}-` } : {};
		const response = await fetch(new URL(file, this.url), { cache: "no-store", headers, signal: this.signal });
		if (response.status === 416) {
			return new Uint8Array(0);
		}
		if (response.status === 404) {
			return null;
		}
		if (!response.ok) {
			throw new Error(`${file} answered ${response.status} ${response.statusText}`);
		}
		const bytes = new Uint8Array(await response.arrayBuffer());
		return response.status === 206 ? bytes : bytes.subarray(from);
	}

	async poll() {
		const index = await this.fetchIndex();
		if (index.version !== LIVE_INDEX_VERSION) {
			throw new Error(`the stream is of version ${index.version}, this player reads version ${LIVE_INDEX_VERSION}`);
		}
		this.index = index;
		if (this.stream !== null && index.stream !== this.stream) {
			// The stream started over under the same name.
			this.epoch = null;
		}
		this.stream = index.stream;
		const newest = index.segments.at(-1);
		if (this.epoch === null) {
			if (newest === undefined) {
				this.state = index.state === "ended" ? "ended" : "waiting";
			} else {
				await this.open(newest.epoch, true, LiveFeed.backlogSeconds);
			}
			this.changed();
			return;
		}
		await this.append();
		// Who watched a map to its end watches the next one.
		const ticks = this.player.ticks();
		const tickSpeed = index.tick_speed ?? 50;
		if (this.complete && this.newerMap && ticks.current >= ticks.last - 1) {
			const next = index.segments.find(segment => segment.epoch > this.epoch).epoch;
			await this.open(next, false);
		} else if (this.earlier && ticks.first >= 0 && ticks.current <= ticks.first + LIVE_START_SECONDS * tickSpeed) {
			// The viewer went back to where the file begins.
			await this.open(this.epoch, false, 2 * (this.backlog ?? LiveFeed.backlogSeconds), ticks.current);
		} else if (this.atLiveEdge() && ticks.last - ticks.first > LiveFeed.maxBacklogSeconds * tickSpeed) {
			await this.open(this.epoch, true, LiveFeed.backlogSeconds);
		}
		this.changed();
	}

	// Puts the init of a map and what is left of its segments into a demo
	// file and has the program play it, at its live end, at `seekTick` or at
	// its beginning. At the live end or a tick, only the segments of the last
	// `backlog` seconds are read, if it is not `null`.
	async open(epoch, atEnd, backlog = null, seekTick = null) {
		const index = this.index;
		const init = index.epochs.find(entry => entry.epoch === epoch);
		let segments = index.segments.filter(segment => segment.epoch === epoch);
		// An ended stream is watched from its beginning.
		atEnd = atEnd && index.state !== "ended";
		if (!atEnd && seekTick === null) {
			backlog = null;
		}
		if (backlog !== null && segments.length > 0) {
			const from = segments.at(-1).end_tick - backlog * (index.tick_speed ?? 50);
			segments = segments.filter((segment, i) => i === segments.length - 1 || segment.end_tick > from);
		}
		const header = await this.fetchBytes(init.init, 0);
		if (header === null) {
			throw new Error(`${init.init} is gone`);
		}
		const read = new Map();
		const parts = [];
		for (const segment of segments) {
			const bytes = await this.fetchBytes(segment.file, 0);
			if (bytes === null) {
				// Deleted while this read the ones before: the stream is
				// read again from the next poll on.
				if (parts.length === 0) {
					continue;
				}
				throw new Error(`${segment.file} is gone`);
			}
			parts.push(bytes);
			read.set(segment.n, bytes.length);
		}
		const body = concatBytes(parts);
		const whole = wholeChunks(body);

		const FS = this.player.module.FS;
		const directory = `${this.player.homePath}/demos/live`;
		FS.mkdirTree(directory);
		const name = String(index.name).replace(/[^A-Za-z0-9._-]/g, "_");
		const path = `${directory}/${name}-${epoch}.demo`;
		FS.writeFile(path, concatBytes([header, body.subarray(0, whole)]));
		const previous = this.path;
		this.path = path;
		this.epoch = epoch;
		this.read = read;
		this.next = segments.at(-1)?.n ?? -1;
		this.firstRead = segments[0]?.n ?? -1;
		this.backlog = backlog;
		this.tail = body.slice(whole);
		this.complete = false;
		this.state = "live";

		const player = this.player;
		const paused = player.paused;
		const loads = player.number("DemoPlayerLoadCount") ?? 0;
		player.dispatchEvent(new CustomEvent("loadstart", { detail: { url: this.url.href } }));
		player.call("EmscriptenCallbackDropFile", null, ["string"], [path]);
		while (!((player.number("DemoPlayerLoadCount") ?? 0) > loads)) {
			if (this.signal.aborted || player.exited) {
				return;
			}
			await sleep(50, this.signal);
		}
		if (previous !== null && previous !== path) {
			try {
				FS.unlink(previous);
			} catch {
				// Already gone.
			}
		}
		player.setLive(true);
		if (seekTick !== null) {
			// Where the viewer was, in a file that begins earlier now: with
			// its first segment, as the ticks the program says may still be
			// those of the file before for a frame.
			player.currentTime = Math.max(0, seekTick - (segments[0]?.start_tick ?? seekTick)) / (index.tick_speed ?? 50);
			if (!paused) {
				player.play();
			}
		} else {
			if (atEnd) {
				player.seek(1);
			} else {
				player.restart();
			}
			player.play();
		}
		// The stream may have ended or gone on with another map meanwhile.
		this.finishWhenDone();
	}

	// Appends what the segments of the map grew by since the last poll.
	async append() {
		const index = this.index;
		const segments = index.segments.filter(segment => segment.epoch === this.epoch && segment.n >= this.next);
		if (this.next >= 0 && segments[0]?.n !== this.next) {
			// The limits deleted what was still to read: the rest cannot
			// follow what is in the file.
			this.epoch = null;
			throw new Error("the stream went on faster than it could be read");
		}
		const parts = [this.tail];
		for (const segment of segments) {
			const had = this.read.get(segment.n) ?? 0;
			if (segment.complete && had >= segment.bytes) {
				continue;
			}
			const bytes = await this.fetchBytes(segment.file, had);
			if (bytes === null) {
				this.epoch = null;
				throw new Error(`${segment.file} is gone`);
			}
			parts.push(bytes);
			this.read.set(segment.n, had + bytes.length);
			this.next = segment.n;
		}
		const body = concatBytes(parts);
		const whole = wholeChunks(body);
		if (whole > 0) {
			const FS = this.player.module.FS;
			const stream = FS.open(this.path, "a");
			FS.write(stream, body, 0, whole);
			FS.close(stream);
		}
		this.tail = body.slice(whole);
		this.finishWhenDone();
	}

	// The map is all there once its last segment is complete and read, and
	// the stream went on with another map or ended. The program then plays
	// it to its end like any demo.
	finishWhenDone() {
		const index = this.index;
		const last = index.segments.filter(segment => segment.epoch === this.epoch).at(-1);
		const done = last !== undefined && last.complete && (this.read.get(last.n) ?? 0) >= last.bytes && this.tail.length === 0 &&
			(index.state === "ended" || this.newerMap);
		if (done && !this.complete) {
			this.complete = true;
			this.player.setLive(false);
		}
		if (index.state === "ended" && (done || last === undefined)) {
			this.state = "ended";
		}
	}

	// Goes to the end of the newest map.
	goLive() {
		return this.serial(async () => {
			const newest = this.index?.segments.at(-1);
			if (newest !== undefined && newest.epoch !== this.epoch) {
				await this.open(newest.epoch, true, LiveFeed.backlogSeconds);
				this.changed();
			} else {
				this.player.seek(1);
				this.player.play();
			}
		});
	}

	// Whether the program plays at the end of the newest map.
	atLiveEdge() {
		const ticks = this.player.ticks();
		const tickSpeed = this.index?.tick_speed ?? 50;
		return this.state === "live" && !this.complete && !this.newerMap && ticks.live &&
			ticks.last - ticks.current <= LIVE_EDGE_SECONDS * tickSpeed;
	}
}

export class DemoPlayer extends Program {
	static script = PROGRAM;
	static base = import.meta.url;
	static moduleName = "DDNetDemoPlayer";
	static programName = "Demo player";
	static suffix = ".demo";

	constructor(options = {}) {
		super(options);
		this.source = typeof options.file === "string" ? options.file : "";
		this.wakeLock = null;
		this.live = null;
	}

	programArguments() {
		const options = this.options;
		const args = [];
		if (options.controls === false) {
			args.push("--no-controls");
		}
		if (options.zoom === false) {
			args.push("--no-zoom");
		}
		if (options.overlays === false) {
			args.push("--no-overlays");
		}
		if (options.startTime !== undefined) {
			args.push("--time", String(options.startTime));
		}
		if (options.speed !== undefined) {
			args.push("--speed", String(options.speed));
		}
		if (options.paused === true) {
			args.push("--paused");
		}
		return args.concat(super.programArguments());
	}

	// Tab shows the scoreboard, unless the page turned that off. Shift+Tab
	// always moves the focus back out.
	passesKey(event) {
		return event.key === "Tab" && (event.shiftKey || !this.overlays());
	}

	number(name, argument) {
		return argument === undefined ? this.call(name, "number") : this.call(name, null, ["number"], [argument]);
	}

	flag(name, setter, value) {
		return value === undefined ? this.number(name) === 1 : this.number(setter, value ? 1 : 0);
	}

	setSize(width, height) {
		this.call("DemoPlayerSetSize", null, ["number", "number"], [Math.round(width), Math.round(height)]);
	}

	progress() {
		return this.number("DemoPlayerProgress");
	}

	seek(fraction) {
		this.number("DemoPlayerSeekPercent", fraction);
	}

	restart() {
		this.call("DemoPlayerSeekStart");
	}

	clip(start, end) {
		if (start === undefined) {
			const clipStart = this.number("DemoPlayerClipStart");
			const clipEnd = this.number("DemoPlayerClipEnd");
			return clipStart === null || clipEnd === null || clipEnd <= clipStart ? null : { start: clipStart, end: clipEnd };
		}
		this.call("DemoPlayerSetClip", null, ["number", "number"], start === null ? [0, 0] : [start, end ?? 0]);
	}

	markClip(which) {
		this.number("DemoPlayerMarkClip", which === "start" ? 1 : 0);
	}

	exportState() {
		return this.number("DemoPlayerExportState");
	}

	exportProgress() {
		return this.number("DemoPlayerExportProgress");
	}

	exportSecondsLeft() {
		return this.number("DemoPlayerExportSecondsLeft");
	}

	exportError() {
		return this.call("DemoPlayerExportError", "string") ?? "";
	}

	cancelExport() {
		this.call("DemoPlayerCancelExport");
	}

	spectating(id) {
		return id === undefined ? this.number("DemoPlayerSpectating") : this.number("DemoPlayerSetSpectate", id);
	}

	spectateName(name) {
		this.call("DemoPlayerSetSpectateName", null, ["string"], [name ?? ""]);
	}

	spectateStep(direction) {
		this.number("DemoPlayerSpectateStep", direction);
	}

	serverDemo() {
		return this.number("DemoPlayerServerDemo") === 1;
	}

	players() {
		return JSON.parse(this.call("DemoPlayerPlayers", "string") || "[]");
	}

	info() {
		return JSON.parse(this.call("DemoPlayerInfo", "string") || "{}");
	}

	zoom(factor) {
		return factor === undefined ? this.number("DemoPlayerZoom") : this.number("DemoPlayerZoomBy", factor);
	}

	resetZoom() {
		this.call("DemoPlayerResetZoom");
	}

	zoomChanged() {
		return this.number("DemoPlayerZoomChanged") === 1;
	}

	zoomEnabled(enable) {
		// Without zoom the wheel scrolls the page, see
		// `src/engine/client/web/web_platform.js`.
		if (enable !== undefined && this.module) {
			this.module.ddnetWheel = enable;
		}
		return this.flag("DemoPlayerZoomEnabled", "DemoPlayerSetZoomEnabled", enable);
	}

	overlays(enable) {
		return this.flag("DemoPlayerOverlays", "DemoPlayerSetOverlays", enable);
	}

	keyPresses(show) {
		return this.flag("DemoPlayerKeyPresses", "DemoPlayerSetKeyPresses", show);
	}

	highDetail(on) {
		return this.flag("DemoPlayerHighDetail", "DemoPlayerSetHighDetail", on);
	}

	recordedCameraAvailable() {
		return this.number("DemoPlayerRecordedCamera") > 0;
	}

	recordedCamera(use) {
		return use === undefined ? this.number("DemoPlayerRecordedCamera") === 2 : this.number("DemoPlayerSetRecordedCamera", use ? 1 : 0);
	}

	controls(show) {
		return this.flag("DemoPlayerControls", "DemoPlayerSetControls", show);
	}

	// `follow` is who the video watches: a client id, or -1 for the free view.
	// Only a server demo needs it, a client demo follows its recorder.
	startExport(options = {}) {
		return this.call("DemoPlayerStartExport", "number",
			["number", "number", "number", "number", "number", "string", "number", "number", "number", "number", "number", "number"],
			[
				options.width ?? 0,
				options.height ?? 0,
				options.fps ?? 60,
				options.audio === false ? 0 : 1,
				options.crf ?? 23,
				options.codec ?? "",
				options.hud ? 1 : 0,
				options.chat === false ? 0 : 1,
				options.follow ?? SPEC_FOLLOW,
				options.highDetail === false ? 0 : 1,
				// Everybody's, as `cl_video_show_direction 2`: in a demo that
				// includes whoever recorded it.
				options.keyPresses ? 2 : 0,
				options.nameplates === false ? 0 : 1,
			]) === 1;
	}

	get duration() {
		const length = this.number("DemoPlayerLength");
		return length > 0 ? length : NaN;
	}

	get currentTime() {
		const length = this.number("DemoPlayerLength");
		return length > 0 ? this.progress() * length : 0;
	}

	set currentTime(seconds) {
		const time = parseFloat(seconds);
		if (isFinite(time)) {
			this.number("DemoPlayerSeekToTime", Math.max(0, time));
		}
	}

	get paused() {
		return this.number("DemoPlayerPaused") !== 0;
	}

	get ended() {
		return this.number("DemoPlayerLength") > 0 && this.progress() >= END_OF_DEMO;
	}

	get playbackRate() {
		return this.number("DemoPlayerSpeed") ?? 1;
	}

	set playbackRate(rate) {
		const speed = parseFloat(rate);
		if (isFinite(speed) && speed > 0) {
			this.number("DemoPlayerSetSpeed", speed);
		}
	}

	get volume() {
		return this.number("DemoPlayerVolume") ?? 1;
	}

	set volume(level) {
		const value = parseFloat(level);
		if (isFinite(value)) {
			this.number("DemoPlayerSetVolume", Math.min(Math.max(value, 0), 1));
		}
	}

	get muted() {
		return this.number("DemoPlayerMuted") === 1;
	}

	set muted(mute) {
		this.number("DemoPlayerSetMuted", mute ? 1 : 0);
	}

	get src() {
		return this.source;
	}

	// Plays the live stream whose `index.json` is at `url`, see `LiveFeed`.
	watchLive(url) {
		this.stopLive();
		this.source = "";
		this.live = new LiveFeed(this, url);
		this.live.addEventListener("livechange", () => this.dispatchEvent(new Event("livechange")));
		this.live.run();
		this.dispatchEvent(new Event("livechange"));
		return this.live;
	}

	stopLive() {
		if (this.live !== null) {
			this.live.stop();
			this.live = null;
			this.setLive(false);
			this.dispatchEvent(new Event("livechange"));
		}
	}

	// Whether the demo file still grows, see `CDemoPlayer::SetLive`.
	setLive(on) {
		this.number("DemoPlayerSetLive", on ? 1 : 0);
	}

	// Where the demo stands in ticks.
	ticks() {
		return JSON.parse(this.call("DemoPlayerTicks", "string") || "null") ?? { first: -1, current: -1, last: -1, live: false };
	}

	// To the end of a live stream.
	goLive() {
		return this.live?.goLive();
	}

	destroy() {
		this.live?.stop();
		super.destroy();
	}

	// At the end, playing starts over.
	play() {
		this.number("DemoPlayerSetPaused", 0);
		return Promise.resolve();
	}

	pause() {
		this.number("DemoPlayerSetPaused", 1);
	}

	async loadFile(file) {
		this.stopLive();
		this.source = "";
		return await super.loadFile(file);
	}

	async loadUrl(url) {
		this.stopLive();
		this.source = String(url);
		return await super.loadUrl(url);
	}

	// The program cannot call out while it draws, so what changed is asked for
	// here and said in the events a `<video>` fires.
	running() {
		const signal = this.stopping.signal;
		let known = null;
		// A demo that replaces another counts as loaded once the program has
		// opened it, not while it still shows the old one.
		let waitFor = 1;
		const fire = type => this.dispatchEvent(new Event(type));
		this.addEventListener("loadstart", () => {
			known = null;
			waitFor = (this.number("DemoPlayerLoadCount") ?? 0) + 1;
		}, { signal });
		const poll = () => {
			if (signal.aborted) {
				return;
			}
			setTimeout(poll, POLL_INTERVAL_MS);
			const loads = this.number("DemoPlayerLoadCount");
			if (!(loads >= waitFor)) {
				return;
			}
			const clip = this.clip();
			const now = {
				loads,
				duration: this.duration,
				time: this.currentTime,
				paused: this.paused,
				rate: this.playbackRate,
				volume: this.volume,
				muted: this.muted,
				ended: this.ended,
				view: `${clip?.start}-${clip?.end}-${this.spectating()}`,
			};
			if (known === null || known.loads !== now.loads) {
				fire("loadedmetadata");
				fire("durationchange");
				// A demo plays by itself, which is announced as a `play`.
				known = { ...now, paused: true, ended: false, view: null };
			}
			if (known.paused !== now.paused) {
				fire(now.paused ? "pause" : "play");
			}
			if (known.time !== now.time) {
				fire("timeupdate");
			}
			if (known.rate !== now.rate) {
				fire("ratechange");
			}
			if (known.volume !== now.volume || known.muted !== now.muted) {
				fire("volumechange");
			}
			if (now.ended && !known.ended) {
				fire("ended");
			}
			if (known.view !== now.view) {
				fire("viewchange");
			}
			known = now;
		};
		poll();
		this.keepScreenOn(signal);
	}

	// A phone would dim and lock while somebody watches.
	keepScreenOn(signal) {
		if (!navigator.wakeLock) {
			return;
		}
		const update = async () => {
			const wanted = !this.paused && !this.ended && document.visibilityState === "visible" && !signal.aborted;
			if (wanted && this.wakeLock === null) {
				this.wakeLock = "pending";
				this.wakeLock = await navigator.wakeLock.request("screen").catch(() => null);
				this.wakeLock?.addEventListener("release", () => { this.wakeLock = null; }, { once: true });
				if (signal.aborted) {
					this.wakeLock?.release();
				}
			} else if (!wanted && this.wakeLock !== null && this.wakeLock !== "pending") {
				this.wakeLock.release();
				this.wakeLock = null;
			}
		};
		for (const type of ["play", "pause", "ended"]) {
			this.addEventListener(type, update, { signal });
		}
		document.addEventListener("visibilitychange", update, { signal });
		signal.addEventListener("abort", update, { once: true });
	}
}

function formatTime(seconds) {
	const total = Math.max(0, Math.round(seconds));
	const hours = Math.floor(total / 3600);
	const minutes = Math.floor(total / 60) % 60;
	const rest = String(total % 60).padStart(2, "0");
	return hours > 0 ? `${hours}:${String(minutes).padStart(2, "0")}:${rest}` : `${minutes}:${rest}`;
}

const SPEEDS = [0.25, 0.5, 0.75, 1, 1.5, 2, 4, 8];

const BAR_HTML = `
<p class="viewer-status" data-role="export-status" role="status" hidden>
	<span data-role="export-text"></span>
	<button class="viewer-button viewer-button-small" data-role="export-stop" data-icon="close" title="Stop the export" aria-label="Stop the export" hidden></button>
</p>
<div class="viewer-popup viewer-menu" data-role="menu" role="menu" aria-label="Settings" hidden></div>
<div class="viewer-popup viewer-panel" data-role="export-panel" role="dialog" aria-label="Export video" hidden>
	<div class="viewer-panel-title">Export video</div>
	<label class="viewer-follow" data-role="follow-row" hidden>Follow
		<select data-role="follow" required></select>
	</label>
	<div class="viewer-form" data-role="export-settings"></div>
	<p class="viewer-panel-message" data-role="export-message" role="alert" hidden></p>
	<div class="viewer-panel-actions">
		<button class="viewer-text-button viewer-text-button-quiet" data-role="export-cancel">Cancel</button>
		<button class="viewer-text-button" data-role="export-start">Export</button>
	</div>
</div>
<div class="viewer-popup viewer-panel" data-role="info-panel" role="dialog" aria-label="About this demo" hidden>
	<div class="viewer-panel-title">About this demo</div>
	<dl class="viewer-info" data-role="info"></dl>
	<div class="viewer-panel-actions">
		<button class="viewer-text-button viewer-text-button-quiet" data-role="info-close">Close</button>
	</div>
</div>
<div class="viewer-bar" data-role="bar" hidden>
	<div class="viewer-seek-track">
		<input class="viewer-seek" data-role="seek" type="range" min="0" max="1000" value="0" step="1" aria-label="Seek">
		<div class="viewer-markers" data-role="markers" aria-hidden="true"></div>
	</div>
	<div class="viewer-row">
		<button class="viewer-button" data-role="play" data-icon="pause"></button>
		<div class="viewer-volume">
			<button class="viewer-button" data-role="mute" data-icon="volume"></button>
			<input class="viewer-volume-slider" data-role="volume" type="range" min="0" max="100" value="100" step="1" aria-label="Volume">
		</div>
		<span class="viewer-readout" data-role="time">0:00 / 0:00</span>
		<span class="viewer-readout viewer-readout-clip" data-role="clip-time" hidden></span>
		<button class="viewer-live" data-role="live" hidden>Live</button>
		<span class="viewer-spacer"></span>
		<button class="viewer-button" data-role="settings" data-icon="settings" title="Settings" aria-label="Settings" aria-haspopup="menu" aria-expanded="false"></button>
		<button class="viewer-button" data-role="fullscreen" data-icon="fullscreen"></button>
	</div>
</div>
`;

const BAR_INTERVAL_MS = 200;

export class DemoControls {
	constructor(player, options = {}) {
		const { container = null, picture = null, slot = null, signal, settings = true } = options;
		this.player = player;
		this.stopping = new AbortController();
		signal?.addEventListener("abort", () => this.destroy(), { once: true });
		this.picture = picture ?? player.canvas;
		this.allowSettings = settings;
		this.root = Object.assign(document.createElement("div"), { className: "viewer-controls" });
		if (slot !== null) {
			this.root.slot = slot;
		}
		this.root.innerHTML = BAR_HTML;
		paintIcons(this.root);
		container?.append(this.root);
		// While the slider is dragged the demo stands still.
		this.seeking = false;
		this.pausedBeforeSeeking = false;
		this.submenu = null;
		this.wire();
		this.timer = setInterval(() => this.update(), BAR_INTERVAL_MS);
		this.update();
	}

	get element() {
		return this.root;
	}

	part(role) {
		return this.root.querySelector(`[data-role="${role}"]`);
	}

	destroy() {
		clearInterval(this.timer);
		this.stopping.abort();
		this.root.remove();
	}

	menuOpen() {
		return !this.part("menu").hidden;
	}

	panelOpen() {
		return !this.part("export-panel").hidden || !this.part("info-panel").hidden;
	}

	// `keyboard` says whether the menu is used from the keyboard, which then
	// gets focus in it; a pointer leaves the keyboard with the picture.
	openMenu(open, submenu = null, keyboard = false) {
		const menu = this.part("menu");
		const wasOpen = this.menuOpen();
		menu.hidden = !open;
		this.part("settings").setAttribute("aria-expanded", String(open));
		if (open) {
			this.closePanel();
			this.submenu = submenu;
			this.fillMenu();
			this.autoHide.show();
			if (keyboard) {
				(menu.querySelector("[aria-checked=true]") ?? menu.querySelector("button"))?.focus();
			}
		} else if (wasOpen && menu.contains(document.activeElement)) {
			this.part("settings").focus();
		}
	}

	openPanel() {
		this.openMenu(false);
		this.part("info-panel").hidden = true;
		const panel = this.part("export-panel");
		panel.hidden = false;
		this.say("");
		this.fillFollow();
		this.autoHide.show();
		panel.querySelector("select, button")?.focus();
	}

	closePanel() {
		this.part("export-panel").hidden = true;
		this.part("info-panel").hidden = true;
	}

	openInfo() {
		this.openMenu(false);
		this.part("info-panel").hidden = false;
		this.updateInfo();
		this.autoHide.show();
		this.part("info-close").focus();
	}

	// The players come as the demo names them, so the panel is filled again
	// while it is open.
	updateInfo() {
		const text = this.player.call("DemoPlayerInfo", "string") || "{}";
		if (text !== this.infoText) {
			this.infoText = text;
			fillInfo(this.part("info"), demoInfoSections(JSON.parse(text)));
		}
	}

	say(message) {
		const line = this.part("export-message");
		line.textContent = message;
		line.hidden = message === "";
	}

	// Who a video of a server demo watches has to be chosen: a server demo
	// has nobody who recorded it.
	fillFollow() {
		const player = this.player;
		const row = this.part("follow-row");
		const select = this.part("follow");
		row.hidden = !player.serverDemo();
		if (row.hidden) {
			return;
		}
		const chosen = select.value;
		const option = (value, text) => Object.assign(document.createElement("option"), { value: String(value), textContent: text });
		select.replaceChildren(
			option("", "Choose a player…"),
			...player.players().map(one => option(one.id, one.name)),
			option(SPEC_FREEVIEW, "Free view"));
		const watching = player.spectating();
		select.value = chosen !== "" ? chosen : watching >= 0 ? String(watching) : "";
		if (select.selectedIndex < 0) {
			select.value = "";
		}
	}

	wire() {
		const player = this.player;
		const signal = this.stopping.signal;
		const on = (target, type, listener, extra) => target.addEventListener(type, listener, { signal, ...extra });
		const part = role => this.part(role);

		this.exportForm = exportSettingsForm(part("export-settings"), { canvas: player.canvas });
		fullscreen(part("fullscreen"), { element: this.picture, shortcut: "F", signal });
		this.autoHide = autoHide([part("bar")], {
			picture: this.picture,
			hold: () => player.paused || this.menuOpen() || this.panelOpen() || this.seeking,
			onHide: () => this.openMenu(false),
			signal,
		});
		// A click on a button leaves the keyboard with the picture, where the
		// shortcuts are; tabbing still reaches every button.
		on(this.root, "mousedown", event => {
			if (event.target.closest("button")) {
				event.preventDefault();
			}
		});
		on(document, "pointerdown", event => {
			if (this.menuOpen() && !event.composedPath().some(node => node === part("menu") || node === part("settings"))) {
				this.openMenu(false);
			}
		});
		// Escape also reaches here from the picture, which keeps the focus
		// after a click on a button.
		const keys = event => {
			if (event.key === "Escape" && (this.menuOpen() || this.panelOpen())) {
				event.stopPropagation();
				const inside = this.root.contains(document.activeElement);
				this.openMenu(false);
				this.closePanel();
				if (inside) {
					part("settings").focus();
				}
			} else if (this.menuOpen() && (event.key === "ArrowDown" || event.key === "ArrowUp")) {
				// Arrow keys walk the menu, as in any menu.
				event.preventDefault();
				const items = [...part("menu").querySelectorAll("button")];
				const index = items.indexOf(document.activeElement);
				items[(index + (event.key === "ArrowDown" ? 1 : items.length - 1)) % items.length]?.focus();
			}
		};
		on(this.picture, "keydown", keys);
		if (!this.picture.contains(this.root)) {
			on(this.root, "keydown", keys);
		}

		on(part("play"), "click", () => (player.paused ? player.play() : player.pause()));
		on(part("mute"), "click", () => { player.muted = !player.muted; });
		on(part("volume"), "input", () => {
			const level = part("volume").valueAsNumber / 100;
			player.muted = level === 0;
			if (level > 0) {
				player.volume = level;
			}
		});
		// The demo follows the slider while it is dragged.
		on(part("seek"), "input", () => {
			if (!this.seeking) {
				this.seeking = true;
				this.pausedBeforeSeeking = player.paused;
				player.pause();
			}
			player.seek(part("seek").valueAsNumber / 1000);
		});
		on(part("seek"), "change", () => {
			player.seek(part("seek").valueAsNumber / 1000);
			this.seeking = false;
			if (!this.pausedBeforeSeeking) {
				player.play();
			}
		});
		// A click from the keyboard has no pointer position.
		on(part("settings"), "click", event => this.openMenu(!this.menuOpen(), null, event.detail === 0));
		on(part("export-cancel"), "click", () => this.closePanel());
		on(part("info-close"), "click", () => this.closePanel());
		on(part("export-stop"), "click", () => player.cancelExport());
		on(part("export-start"), "click", () => this.startExport());
		on(part("live"), "click", () => player.goLive());
		this.noEncoder = typeof VideoEncoder === "undefined";
	}

	async startExport() {
		const player = this.player;
		const follow = this.part("follow");
		const serverDemo = !this.part("follow-row").hidden;
		if (serverDemo && follow.value === "") {
			this.say("Choose who the video follows.");
			follow.focus();
			return;
		}
		// Asking where to save is only allowed out of this click.
		if (window.showSaveFilePicker) {
			try {
				const handle = await window.showSaveFilePicker({
					suggestedName: "video.mp4",
					types: [{ description: "MP4 video", accept: { "video/mp4": [".mp4"] } }],
				});
				player.setVideoSink({ stream: await handle.createWritable(), done: () => null });
			} catch (error) {
				if (error?.name !== "AbortError") {
					this.say(`The file could not be opened: ${error?.message ?? error}`);
				}
				return;
			}
		}
		const settings = { ...this.exportForm.values(), follow: serverDemo ? parseInt(follow.value, 10) : undefined };
		if (!player.startExport(settings)) {
			this.say("The export could not be started.");
			return;
		}
		this.closePanel();
	}

	// The menu is a list of rows; a row with a value opens its own list in
	// place, the way a video player's settings menu does.
	fillMenu() {
		const player = this.player;
		const menu = this.part("menu");
		const signal = this.stopping.signal;
		const row = (label, action, extra = {}) => {
			const item = Object.assign(document.createElement("button"), { className: "viewer-menu-item" });
			item.setAttribute("role", extra.checked === undefined ? "menuitem" : "menuitemradio");
			if (extra.checked !== undefined) {
				item.setAttribute("aria-checked", String(extra.checked));
			}
			if (extra.icon) {
				item.dataset.icon = extra.icon;
			}
			item.append(Object.assign(document.createElement("span"), { textContent: label }));
			if (extra.value !== undefined) {
				item.append(Object.assign(document.createElement("span"), { className: "viewer-menu-value", textContent: extra.value }));
				item.setAttribute("aria-haspopup", "menu");
			}
			if (extra.shortcut) {
				item.setAttribute("aria-keyshortcuts", extra.shortcut);
				item.title = `${label} (${extra.shortcut})`;
			}
			item.addEventListener("click", event => action(event.detail === 0), { signal });
			return item;
		};
		const toggle = (label, on, set) => {
			const item = row(label, fromKeyboard => { set(!on); this.openMenu(true, null, fromKeyboard); }, { value: on ? "On" : "Off" });
			item.setAttribute("role", "menuitemcheckbox");
			item.setAttribute("aria-checked", String(on));
			item.removeAttribute("aria-haspopup");
			return item;
		};
		const back = title => row(title, fromKeyboard => this.openMenu(true, null, fromKeyboard), { icon: "back" });
		const items = [];
		const spectating = player.spectating();
		const players = player.players();
		const watchName = spectating === SPEC_FREEVIEW ? "Free view" : spectating === SPEC_FOLLOW ? "Recorder" : players.find(one => one.id === spectating)?.name ?? "";
		if (this.submenu === "speed") {
			items.push(back("Speed"));
			const rate = player.playbackRate;
			for (const speed of SPEEDS) {
				items.push(row(speed === 1 ? "Normal" : `${speed}×`, fromKeyboard => { player.playbackRate = speed; this.openMenu(true, null, fromKeyboard); }, { checked: Math.abs(rate - speed) < 0.01 }));
			}
		} else if (this.submenu === "watch") {
			items.push(back("Watch"));
			const choices = player.serverDemo()
				? players.map(one => [one.id, one.name, "eye"])
				: [[SPEC_FOLLOW, "Recorder", "eye"]];
			choices.push([SPEC_FREEVIEW, "Free view", "freeview"]);
			for (const [id, name, icon] of choices) {
				items.push(row(name, fromKeyboard => { player.spectating(id); this.openMenu(true, null, fromKeyboard); }, { checked: id === spectating, icon }));
			}
		} else {
			items.push(row("Speed", fromKeyboard => this.openMenu(true, "speed", fromKeyboard), { value: player.playbackRate === 1 ? "Normal" : `${+player.playbackRate.toFixed(2)}×` }));
			items.push(row("Watch", fromKeyboard => this.openMenu(true, "watch", fromKeyboard), { value: watchName, shortcut: "N" }));
			if (player.recordedCameraAvailable()) {
				items.push(toggle("Demo camera", player.recordedCamera(), on => player.recordedCamera(on)));
			}
			if (player.zoomChanged()) {
				items.push(row("Reset zoom", () => { player.resetZoom(); this.openMenu(false); }, { icon: "zoom_reset" }));
			}
			items.push(row("Start the piece here", () => { player.markClip("start"); this.openMenu(false); }, { icon: "clip_start", shortcut: "I" }));
			items.push(row("End the piece here", () => { player.markClip("end"); this.openMenu(false); }, { icon: "clip_end", shortcut: "O" }));
			if (player.clip() !== null) {
				items.push(row("Whole demo again", () => { player.clip(null); this.openMenu(false); }, { icon: "clip_clear" }));
			}
			if (this.allowSettings) {
				items.push(toggle("Key presses", player.keyPresses(), on => player.keyPresses(on)));
				items.push(toggle("High detail", player.highDetail(), on => player.highDetail(on)));
			}
			if (!this.noEncoder) {
				items.push(row("Export video…", () => this.openPanel(), { icon: "save" }));
			}
			items.push(row("About this demo", () => this.openInfo(), { icon: "info" }));
		}
		menu.replaceChildren(...items);
		paintIcons(menu);
	}

	// Asked of the player every time: its own keys and the pointer change it
	// as well.
	update() {
		const player = this.player;
		const part = role => this.part(role);
		const length = player.duration;
		part("bar").hidden = !(length > 0);
		this.updateExport();
		if (!part("info-panel").hidden) {
			this.updateInfo();
		}
		if (!(length > 0)) {
			return;
		}
		const time = player.currentTime;
		const seek = part("seek");
		if (!this.seeking) {
			seek.value = Math.round(time / length * 1000);
		}
		seek.setAttribute("aria-valuetext", `${formatTime(time)} of ${formatTime(length)}`);
		seek.style.setProperty("--viewer-progress", `${seek.value / 10}%`);
		const clip = player.clip();
		seek.style.setProperty("--viewer-clip-start", `${clip === null ? 0 : clip.start / length * 100}%`);
		seek.style.setProperty("--viewer-clip-end", `${clip === null ? 0 : clip.end / length * 100}%`);
		seek.classList.toggle("viewer-seek-clipped", clip !== null);
		const clipTime = part("clip-time");
		clipTime.hidden = clip === null;
		clipTime.textContent = clip === null ? "" : `${formatTime(clip.start)}–${formatTime(clip.end)}`;
		part("time").textContent = `${formatTime(time)} / ${formatTime(length)}`;
		this.updateLive();

		const paused = player.paused;
		const play = part("play");
		play.dataset.icon = paused ? "play" : "pause";
		play.title = paused ? "Play (K)" : "Pause (K)";
		play.setAttribute("aria-label", paused ? "Play" : "Pause");
		// Muting leaves the slider where it was, which is what unmuting
		// returns to.
		const muted = player.muted;
		const volume = part("volume");
		if (document.activeElement !== volume) {
			volume.value = Math.round((muted ? 0 : player.volume) * 100);
		}
		volume.style.setProperty("--viewer-progress", `${volume.value}%`);
		const mute = part("mute");
		mute.dataset.icon = muted || volume.valueAsNumber === 0 ? "volume_off" : "volume";
		mute.title = muted ? "Unmute (M)" : "Mute (M)";
		mute.setAttribute("aria-label", muted ? "Unmute" : "Mute");
		paintIcons(part("bar"));
	}

	// A live stream: where its end is, and its markers on the seek bar.
	updateLive() {
		const live = this.player.live;
		const button = this.part("live");
		const markers = this.part("markers");
		button.hidden = live === null;
		if (live === null) {
			markers.replaceChildren();
			this.markerKey = "";
			return;
		}
		const ended = live.state === "ended";
		const edge = !ended && live.atLiveEdge();
		button.textContent = ended ? "Ended" : "Live";
		button.disabled = ended;
		button.dataset.edge = String(edge);
		button.title = ended ? "The stream has ended" : edge ? "Live" : "Go to the live end";
		button.setAttribute("aria-label", button.title);

		const ticks = this.player.ticks();
		const span = ticks.last - ticks.first;
		const shown = span > 0 ? live.markers.filter(marker => marker.tick >= ticks.first && marker.tick <= ticks.last) : [];
		const key = `${ticks.first}-${ticks.last}-${shown.map(marker => marker.tick).join(",")}`;
		if (key === this.markerKey) {
			return;
		}
		this.markerKey = key;
		markers.replaceChildren(...shown.map(marker => {
			const mark = Object.assign(document.createElement("span"), { className: "viewer-marker" });
			mark.dataset.kind = marker.kind;
			mark.dataset.tick = String(marker.tick);
			mark.style.left = `${(marker.tick - ticks.first) / span * 100}%`;
			mark.title = marker.kind === "match_end" ? "End of the match" : marker.label || "Marker";
			return mark;
		}));
	}

	updateExport() {
		const player = this.player;
		const state = player.exportState();
		const status = this.part("export-status");
		const text = this.part("export-text");
		const stop = this.part("export-stop");
		if (state === 1) {
			const left = player.exportSecondsLeft();
			text.textContent = `Exporting ${Math.round(player.exportProgress() * 100)}%${left === null || left < 0 ? "" : ` · ${formatTime(left)} left`}`;
			stop.hidden = false;
			status.hidden = false;
			this.lastState = state;
			return;
		}
		stop.hidden = true;
		// A cancelled export goes back to nothing, and has nothing to say.
		if (state === 0 && this.lastState === 1) {
			clearTimeout(this.statusTimer);
			status.hidden = true;
		}
		if (state !== this.lastState && (state === 2 || state === 3)) {
			text.textContent = state === 2 ? "The video was saved." : `The export failed: ${player.exportError() || "unknown reason"}`;
			status.hidden = false;
			clearTimeout(this.statusTimer);
			this.statusTimer = setTimeout(() => { status.hidden = true; }, state === 2 ? 4000 : 10000);
		}
		this.lastState = state;
	}
}

export class DemoElement extends ViewerElement {
	static program = DemoPlayer;
	static bar = DemoControls;
	static observedAttributes = ["src", "live", "controls", "nozoom", "nooverlays", "t", "end", "speed", "paused", "spec"];
	static startAttributes = ["t", "speed", "paused"];
	static linkParams = { demo: "src", live: "live", t: "t", end: "end", speed: "speed", paused: "paused", spec: "spec" };
	static programEvents = ["loadedmetadata", "durationchange", "play", "pause", "timeupdate", "ratechange", "volumechange", "ended", "viewchange", "livechange"];
	static loadEvent = "loadedmetadata";

	startOptions() {
		const time = parseFloat(this.getAttribute("t"));
		const speed = parseFloat(this.getAttribute("speed"));
		return {
			zoom: this.hasAttribute("nozoom") ? false : undefined,
			overlays: this.hasAttribute("nooverlays") ? false : undefined,
			startTime: time > 0 ? time : undefined,
			speed: speed > 0 ? speed : undefined,
			paused: isOn(this.getAttribute("paused")),
		};
	}

	barOptions() {
		return { settings: !this.hasAttribute("nosettings") };
	}

	// `live` is the `index.json` of a live stream, which plays in place of
	// `src`.
	async startProgram() {
		const live = this.getAttribute("live");
		if (live && !this.getAttribute("src")) {
			this.say("Waiting for the stream…");
		}
		const program = await super.startProgram();
		if (live && !this.getAttribute("src") && this.programInstance === program) {
			program.watchLive(live);
		}
		return program;
	}

	applyAttribute(name, value) {
		const player = this.program;
		const number = parseFloat(value);
		if (name === "nozoom") {
			player.zoomEnabled(value === null);
		} else if (name === "nooverlays") {
			player.overlays(value === null);
		} else if (name === "t" && isFinite(number)) {
			player.currentTime = number;
			this.applyClip();
		} else if (name === "end") {
			this.applyClip();
		} else if (name === "speed" && number > 0) {
			player.playbackRate = number;
		} else if (name === "paused") {
			if (isOn(value)) {
				player.pause();
			} else {
				player.play();
			}
		} else if (name === "live") {
			// Asked again with every demo the stream loads.
			if (!value) {
				player.stopLive();
			} else if (player.live?.url.href !== new URL(value, location.href).href) {
				this.say("Waiting for the stream…");
				player.watchLive(value);
			}
		} else if (name === "spec" && value) {
			if (String(parseInt(value, 10)) === value) {
				player.spectating(parseInt(value, 10));
			} else {
				player.spectateName(value);
			}
		} else {
			super.applyAttribute(name, value);
		}
	}

	// `t` and `end` mark a piece, the same words a link out of the player uses.
	applyClip() {
		const start = parseFloat(this.getAttribute("t"));
		const end = parseFloat(this.getAttribute("end"));
		this.program.clip(end > 0 ? (start > 0 ? start : 0) : null, end);
	}

	linkEvents() {
		return ["timeupdate", "pause", "play", "ratechange", "viewchange"];
	}

	// A marked piece is what a copied link is about, so the link says where
	// that piece is rather than where the demo stands.
	linkValues() {
		const player = this.program;
		const clip = player.clip();
		const spectating = player.spectating();
		// A time in a live stream is of a file that begins where the page
		// began reading it.
		if (player.live !== null) {
			return {
				end: null,
				t: null,
				speed: Math.abs(player.playbackRate - 1) < 0.005 ? null : player.playbackRate.toFixed(2),
				paused: null,
				spec: spectating >= 0 ? spectating : null,
			};
		}
		return {
			end: clip === null ? null : Math.round(clip.end),
			t: Math.round(clip === null ? player.currentTime : clip.start) || null,
			speed: Math.abs(player.playbackRate - 1) < 0.005 ? null : player.playbackRate.toFixed(2),
			paused: player.paused ? 1 : null,
			spec: spectating >= 0 ? spectating : null,
		};
	}
}

const isOn = value => value !== null && value !== "0" && value !== "false";

// The element answers what a `<video>` answers by asking its player.
const VIDEO_DEFAULTS = { duration: NaN, currentTime: 0, paused: true, ended: false, playbackRate: 1, volume: 1, muted: false, src: "" };
for (const name of Object.keys(VIDEO_DEFAULTS)) {
	Object.defineProperty(DemoElement.prototype, name, {
		get() {
			return this.program === null ? VIDEO_DEFAULTS[name] : this.program[name];
		},
		set(value) {
			if (name === "src") {
				this.setAttribute("src", value);
			} else if (this.program !== null) {
				this.program[name] = value;
			}
		},
		configurable: true,
	});
}

DemoElement.prototype.play = async function() {
	const player = await this.ready;
	return player?.play();
};

DemoElement.prototype.pause = function() {
	this.program?.pause();
};

if (typeof customElements !== "undefined" && customElements.get("ddnet-demo") === undefined) {
	customElements.define("ddnet-demo", DemoElement);
}
