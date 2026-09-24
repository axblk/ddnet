/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// The DDNet demo player. The API is documented in demo-player.d.ts; the other
// end of the calls is the `DemoPlayer*` block in
// `src/engine/client/demo_player_client.cpp`.

import { addIcons, autoHide, exportSettingsForm, fullscreen, paintIcons, Program, ViewerElement } from "@ddnet/base";

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
			["number", "number", "number", "number", "number", "string", "number", "number", "number"],
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

	// At the end, playing starts over.
	play() {
		this.number("DemoPlayerSetPaused", 0);
		return Promise.resolve();
	}

	pause() {
		this.number("DemoPlayerSetPaused", 1);
	}

	async loadFile(file) {
		this.source = "";
		return await super.loadFile(file);
	}

	async loadUrl(url) {
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
<div class="viewer-bar" data-role="bar" hidden>
	<input class="viewer-seek" data-role="seek" type="range" min="0" max="1000" value="0" step="1" aria-label="Seek">
	<div class="viewer-row">
		<button class="viewer-button" data-role="play" data-icon="pause"></button>
		<div class="viewer-volume">
			<button class="viewer-button" data-role="mute" data-icon="volume"></button>
			<input class="viewer-volume-slider" data-role="volume" type="range" min="0" max="100" value="100" step="1" aria-label="Volume">
		</div>
		<span class="viewer-readout" data-role="time">0:00 / 0:00</span>
		<span class="viewer-readout viewer-readout-clip" data-role="clip-time" hidden></span>
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
		return !this.part("export-panel").hidden;
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
		const panel = this.part("export-panel");
		panel.hidden = false;
		this.say("");
		this.fillFollow();
		this.autoHide.show();
		panel.querySelector("select, button")?.focus();
	}

	closePanel() {
		this.part("export-panel").hidden = true;
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
		on(part("export-stop"), "click", () => player.cancelExport());
		on(part("export-start"), "click", () => this.startExport());
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
	static observedAttributes = ["src", "controls", "nozoom", "nooverlays", "t", "end", "speed", "paused", "spec"];
	static startAttributes = ["t", "speed", "paused"];
	static linkParams = { demo: "src", t: "t", end: "end", speed: "speed", paused: "paused", spec: "spec" };
	static programEvents = ["loadedmetadata", "durationchange", "play", "pause", "timeupdate", "ratechange", "volumechange", "ended", "viewchange"];
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
