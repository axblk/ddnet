/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * The DDNet demo player, as one module: a demo goes in, a picture of the game
 * comes out on a canvas, and what steers it reads like a `<video>`.
 *
 * ```js
 * import { createDemoPlayer } from "@ddnet/demo-player";
 *
 * const player = await createDemoPlayer({ canvas, src: "https://…/a.demo" });
 * player.currentTime = 30;
 * player.addEventListener("timeupdate", () => bar.value = player.currentTime);
 * ```
 *
 * or, without writing any of that:
 *
 * ```html
 * <ddnet-demo src="https://…/a.demo" controls></ddnet-demo>
 * ```
 *
 * The program itself is WebAssembly and lives beside this file; nothing here
 * has to be told where it is. What every program of this family needs - the
 * canvas, the files, the full screen, the icons - is `@ddnet/base`, the
 * runtime this is built on and which a page may use directly as well.
 *
 * The other end of the calls below is the `DemoPlayer*` block in
 * `src/engine/client/demo_player_client.cpp`. That is a C ABI, and it stops
 * here: nothing outside this file spells a function name or says which of the
 * arguments are numbers.
 */

import DDNetBase, { addIcons, Program, ViewerElement } from "@ddnet/base";

// The pictures on a demo player's own buttons, which no other page here has
// any use for: they are named and drawn as `CViewerControls::EIcon` names and
// draws them, so that the bar this page puts up and the bar the program draws
// for itself say the same thing. Said as this module loads, so a page that
// imported it can write `data-icon="play"` and get one.
addIcons({
	play: '<path d="M7.5 3.8 20.5 12 7.5 20.2Z"/>',
	pause: '<rect x="5.5" y="3.5" width="4.4" height="17" rx="2.2"/><rect x="14.1" y="3.5" width="4.4" height="17" rx="2.2"/>',
	restart: '<rect x="3.5" y="3.5" width="3.2" height="17" rx="1.6"/><path d="M20.5 3.8 20.5 20.2 8.4 12Z"/>',
	eye: '<path d="M12 4.6C5.6 4.6 1.8 12 1.8 12s3.8 7.4 10.2 7.4S22.2 12 22.2 12 18.4 4.6 12 4.6Z"/><circle cx="12" cy="12" r="2.7" fill="#000"/>',
	freeview: '<path d="M12 1.6 15.2 6.2H8.8ZM12 22.4 8.8 17.8h6.4ZM1.6 12 6.2 8.8v6.4ZM22.4 12 17.8 15.2V8.8Z"/><rect x="10.9" y="4.6" width="2.2" height="14.8" rx="1.1"/><rect x="4.6" y="10.9" width="14.8" height="2.2" rx="1.1"/>',
	zoom_reset: '<path d="M9.6 3.4v6.2H3.4M14.4 3.4v6.2h6.2M9.6 20.6v-6.2H3.4M14.4 20.6v-6.2h6.2" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>',
	volume: '<path d="M3 9.2h3.4L11.4 5v14L6.4 14.8H3Z"/><path d="M15.2 9.2a4 4 0 0 1 0 5.6M18 6.4a8 8 0 0 1 0 11.2" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/>',
	volume_off: '<path d="M3 9.2h3.4L11.4 5v14L6.4 14.8H3Z"/><path d="M15.4 9.6 20.6 14.8M20.6 9.6 15.4 14.8" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/>',
	// Where a piece of a demo begins and where it ends: a bar with the
	// stretch that is kept beside it, drawn the way a cut is marked
	// everywhere - the bar on the side the piece starts or stops at.
	clip_start: '<rect x="4.4" y="3.6" width="2.8" height="16.8" rx="1.4"/><path d="M9.4 7.2h9.4a1.4 1.4 0 0 1 1.4 1.4v6.8a1.4 1.4 0 0 1-1.4 1.4H9.4Z" opacity="0.55"/>',
	clip_end: '<rect x="16.8" y="3.6" width="2.8" height="16.8" rx="1.4"/><path d="M14.6 7.2H5.2a1.4 1.4 0 0 0-1.4 1.4v6.8a1.4 1.4 0 0 0 1.4 1.4h9.4Z" opacity="0.55"/>',
	clip_clear: '<rect x="4.4" y="3.6" width="2.8" height="16.8" rx="1.4"/><rect x="16.8" y="3.6" width="2.8" height="16.8" rx="1.4"/><path d="M9.6 9.6 14.4 14.4M14.4 9.6 9.6 14.4" fill="none" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/>',
});

/** The program, and what its script calls the factory it defines. */
const PROGRAM = "ddnet-demo-player.js";
const MODULE_NAME = "DDNetDemoPlayer";
const PROGRAM_NAME = "Demo player";
const SUFFIX = ".demo";

// How often a player asks what changed, to say it in the words a `<video>`
// says them in. A browser fires `timeupdate` between four and sixty times a
// second and says so in the standard; this is at the slow end of that, which
// is as often as a page can draw a time readout with anybody noticing.
const VIDEO_EVENT_INTERVAL_MS = 200;
// A demo stops on its last frame, and the last frame is not exactly the end:
// how near the end counts as being at it.
const END_OF_DEMO = 0.999;

/**
 * A demo player: the base, told where its script lies and what a demo can be
 * asked.
 *
 * What it can be asked is what a `<video>` can be asked - `currentTime`,
 * `duration`, `paused`, `play()`, `pause()`, `volume`, `muted` - and what
 * only a demo can: who to watch, how far to zoom, which piece is marked out,
 * and what an export is doing.
 *
 * The other end of the calls is the `DemoPlayer*` block in
 * `src/engine/client/demo_player_client.cpp`.
 */
class CDemoPlayer extends Program {
	static script = PROGRAM;
	static base = import.meta.url;
	static moduleName = MODULE_NAME;
	static programName = PROGRAM_NAME;
	static suffix = SUFFIX;

	constructor(options) {
		super(options);
		// What was asked for, so that `src` can say what is being shown.
		this.viewerSource = typeof options.file === "string" ? options.file : "";
	}

	// A call that answers a number, which most of them do. Answers `null`
	// where the program is not running, the same as `call` does.
	number(name, argument) {
		return argument === undefined
			? this.call(name, "number")
			: this.call(name, null, ["number"], [argument]);
	}

	/**
	 * How big to draw, in the units the page measures its boxes in.
	 * Only for a viewer that sits in a box of the page's own: one that
	 * fills the window follows it by itself. See `followSize`.
	 */
	setSize(Width, Height) {
		return this.call("DemoPlayerSetSize", null, ["number", "number"], [Math.round(Width), Math.round(Height)]);
	}
	/** How far it has played, between 0 and 1. */
	progress() {
		return this.number("DemoPlayerProgress");
	}
	/** Jumps to a part of the demo, between 0 and 1. */
	seek(Fraction) {
		return this.number("DemoPlayerSeekPercent", Fraction);
	}
	/** Jumps to a time in the demo, in seconds from its beginning. */
	seekTime(Seconds) {
		return this.number("DemoPlayerSeekToTime", Seconds);
	}
	restart() {
		return this.call("DemoPlayerSeekStart");
	}
	/**
	 * The piece of the demo that is marked out, as `{start, end}` in
	 * seconds, or `null` where nothing is. Playback stops at its end,
	 * starting over goes back to its beginning, and an export writes it
	 * and nothing else.
	 *
	 * Set it with `clip(start, end)`, and clear it with `clip(0, 0)` or
	 * `clip(null)`.
	 */
	clip(Start, End) {
		if (Start === undefined) {
			const end = this.number("DemoPlayerClipEnd");
			const start = this.number("DemoPlayerClipStart");
			return end === null || start === null || end <= start ? null : { start: start, end: end };
		}
		return this.call("DemoPlayerSetClip", null, ["number", "number"],
			Start === null ? [0, 0] : [Start, End === undefined || End === null ? 0 : End]);
	}
	exporting() {
		return this.number("DemoPlayerExporting") === 1;
	}
	/**
	 * How an export is getting on: 0 before any was asked for, 1 while
	 * one is being written, 2 once one was handed over, 3 when it
	 * failed. What `startExport` answers cannot say, because in a
	 * browser it returns before the encoder has even been asked.
	 */
	exportState() {
		return this.number("DemoPlayerExportState");
	}
	/**
	 * How far the video being written has got, between 0 and 1. Not
	 * where the demo on the canvas is: the export reads it through a
	 * way of its own, so both move at once and apart.
	 */
	exportProgress() {
		return this.number("DemoPlayerExportProgress");
	}
	/**
	 * How much longer the export has to run, in seconds, or a negative
	 * number while there is no telling yet. Worked out from what is
	 * left of the demo and the rate frames are being written at, so it
	 * follows a machine that speeds up or slows down.
	 */
	exportSecondsLeft() {
		return this.number("DemoPlayerExportSecondsLeft");
	}
	/**
	 * Why the export that was last asked for failed, or an empty
	 * string when none has. A page is the only place this can be
	 * said: there is no log for whoever is looking at the demo.
	 */
	exportError() {
		return this.call("DemoPlayerExportError", "string") || "";
	}
	/** Throws away the export that is running, and its file with it. */
	cancelExport() {
		return this.call("DemoPlayerCancelExport");
	}
	/**
	 * Who the demo is watched over the shoulder of, or sets it: a
	 * client id, -1 for a camera of one's own that the pointer drags
	 * around, or -2 for whoever recorded the demo. A demo a server
	 * recorded has nobody who recorded it, so it starts at -1.
	 */
	spectating(Id) {
		return Id === undefined
			? this.number("DemoPlayerSpectating")
			: this.number("DemoPlayerSetSpectate", Id);
	}
	/** Follows whoever is called this, once the demo has named them. */
	spectateName(Name) {
		return this.call("DemoPlayerSetSpectateName", null, ["string"], [Name || ""]);
	}
	/** On to the next player there is, or the one before. */
	spectateStep(Direction) {
		return this.number("DemoPlayerSpectateStep", Direction);
	}
	/**
	 * The players the demo has named so far, as `{id, name}` objects.
	 * A demo names them a snapshot or two in, so a list built from this
	 * is worth building again while it plays.
	 */
	players() {
		return JSON.parse(this.call("DemoPlayerPlayers", "string") || "[]");
	}
	/**
	 * How much of the world is in the canvas, or multiplies it. The
	 * wheel over the canvas does the same thing.
	 */
	zoom(Factor) {
		return Factor === undefined
			? this.number("DemoPlayerZoom")
			: this.number("DemoPlayerZoomBy", Factor);
	}
	/**
	 * Puts the zoom back where the demo started, and says whether
	 * there is anything to put back. Whoever turned the wheel a few
	 * times has no way back of their own, so a page with controls of
	 * its own offers one - while `zoomChanged()` says there is
	 * something to undo, the way the viewer's own bar does it.
	 */
	resetZoom() {
		return this.call("DemoPlayerResetZoom");
	}
	zoomChanged() {
		return this.number("DemoPlayerZoomChanged") !== 0;
	}
	/**
	 * Whether the wheel over the canvas, the zoom keys and two fingers
	 * on the picture zoom the demo, or switches that off. A page that
	 * scrolls around the viewer wants the wheel for itself, and one
	 * that shows a demo at one size and no other wants none of it.
	 * `zoom(Factor)` still works either way - this is only about what
	 * the viewer does on its own. Better with `zoom: false` at the
	 * start, which leaves it off from the first frame.
	 */
	zoomEnabled(Enable) {
		return Enable === undefined
			? this.number("DemoPlayerZoomEnabled") === 1
			: this.call("DemoPlayerSetZoomEnabled", null, ["number"], [Enable ? 1 : 0]);
	}
	/**
	 * Whether the demo's own view is available at all: a demo carries
	 * where the camera was and how much of the world it had in it, but
	 * only if it was recorded with one and only while somebody is
	 * being followed.
	 */
	recordedCameraAvailable() {
		return this.number("DemoPlayerRecordedCamera") !== 0;
	}
	/**
	 * Whether the demo's own view is the one being shown, or switches
	 * back to it. Zooming leaves it behind, which is what whoever
	 * zoomed asked for; this is the way back.
	 */
	recordedCamera(Use) {
		return Use === undefined
			? this.number("DemoPlayerRecordedCamera") === 2
			: this.call("DemoPlayerSetRecordedCamera", null, ["number"], [Use ? 1 : 0]);
	}
	/**
	 * Whether the viewer draws its own bar of controls over the demo,
	 * or switches it on and off. A page with controls of its own turns
	 * it off - better with `controls: false`, which leaves it off from
	 * the first frame rather than after it.
	 */
	controls(Show) {
		return Show === undefined
			? this.number("DemoPlayerControls") === 1
			: this.call("DemoPlayerSetControls", null, ["number"], [Show ? 1 : 0]);
	}
	/**
	 * Starts a video export, and says whether it started. The options
	 * are named as in `render`, because they are the same settings the
	 * render tool takes: `width`, `height`, `fps`, `crf`, `codec`,
	 * `audio`, `hud`, `chat`.
	 */
	startExport(options) {
		const settings = options || {};
		return this.call("DemoPlayerStartExport", "number",
			["number", "number", "number", "number", "number", "string", "number", "number"],
			[
				settings.width || 0,
				settings.height || 0,
				settings.fps || 60,
				settings.audio ? 1 : 0,
				settings.crf === undefined || settings.crf === null ? 18 : settings.crf,
				settings.codec || "",
				settings.hud ? 1 : 0,
				settings.chat === false ? 0 : 1,
			]) === 1;
	}

	// ----- what a page already knows, because a `<video>` has it -----
	//
	// A demo in a page is a video to whoever put it there: it plays, it
	// pauses, it is somewhere in its length, and it has a volume. Said in the
	// words a `<video>` says them in, a page that can drive one of those can
	// drive this without being told anything new - and the code that does it
	// reads the same either way.
	//
	// What is not here is what a demo has no answer for: `buffered`,
	// `seekable`, `readyState`, `networkState`, `preload`, `poster`, `loop`
	// and the tracks. A demo is fetched whole before it is shown, so most of
	// those would be one value that never changes.

	/** How long the demo is, in seconds, or `NaN` before there is one. */
	get duration() {
		const length = this.number("DemoPlayerLength");
		return length > 0 ? length : NaN;
	}

	/** Where in it the viewer is, in seconds. */
	get currentTime() {
		const length = this.number("DemoPlayerLength");
		return length > 0 ? this.number("DemoPlayerProgress") * length : 0;
	}
	set currentTime(seconds) {
		const time = parseFloat(seconds);
		if (isFinite(time)) {
			this.seekTime(Math.max(0, time));
		}
	}

	/** Whether it stands still. A demo that is not there yet does. */
	get paused() {
		const paused = this.number("DemoPlayerPaused");
		return paused === null || paused === 1;
	}

	/**
	 * Whether it has played to its end. A demo stops on its last frame rather
	 * than going back to the start, so this stays true until somebody seeks
	 * away from there - or presses play, which starts it over.
	 */
	get ended() {
		const length = this.number("DemoPlayerLength");
		return length > 0 && this.number("DemoPlayerProgress") >= END_OF_DEMO;
	}

	/** How fast, as a multiple of the speed it was recorded at. */
	get playbackRate() {
		const speed = this.number("DemoPlayerSpeed");
		return speed === null ? 1 : speed;
	}
	set playbackRate(rate) {
		const speed = parseFloat(rate);
		if (isFinite(speed) && speed > 0) {
			this.number("DemoPlayerSetSpeed", speed);
		}
	}

	/** How loud, between 0 and 1. */
	get volume() {
		const level = this.number("DemoPlayerVolume");
		return level === null ? 1 : level;
	}
	set volume(level) {
		const value = parseFloat(level);
		if (isFinite(value)) {
			this.call("DemoPlayerSetVolume", null, ["number"], [Math.min(Math.max(value, 0), 1)]);
		}
	}

	/**
	 * Whether the sound is off. What it was is remembered, so turning it back
	 * on lands where it was rather than at a guess.
	 */
	get muted() {
		return this.number("DemoPlayerMuted") === 1;
	}
	set muted(mute) {
		this.call("DemoPlayerSetMuted", null, ["number"], [mute === true ? 1 : 0]);
	}

	/** What is being shown, as the address it was named by. */
	get src() {
		return this.viewerSource;
	}

	/**
	 * Plays it, and answers once it does. A `<video>` answers a promise here
	 * because a browser may refuse to play sound nobody asked for; nothing
	 * refuses here, but a page that awaits it is right to.
	 *
	 * At the end it starts over, the way a video does. Where a piece is marked
	 * out, that is the piece's beginning.
	 */
	play() {
		this.number("DemoPlayerSetPaused", 0);
		return Promise.resolve();
	}

	pause() {
		this.number("DemoPlayerSetPaused", 1);
	}

	/** Shows a file rather than a name to fetch. */
	async loadFile(file) {
		this.viewerSource = "";
		return await super.loadFile(file);
	}

	/** Shows what is at this address. */
	async loadUrl(url) {
		this.viewerSource = String(url);
		return await super.loadUrl(url);
	}

	// What a page hears about, in the words and the order a `<video>` says
	// them in: the length once it is known, then whatever changes. The
	// program is asked rather than telling - it holds the main thread while it
	// draws - so the asking happens here, once, instead of in every page that
	// wants to know.
	running() {
		const signal = this.stopping.signal;
		// What was true last time round, to tell a change from a state.
		let known = null;
		const fire = (name, detail) => this.dispatchEvent(new CustomEvent(name, { detail: detail }));
		const look = () => {
			if (signal.aborted) {
				return;
			}
			setTimeout(look, VIDEO_EVENT_INTERVAL_MS);
			const length = this.number("DemoPlayerLength");
			if (!(length > 0)) {
				return;
			}
			const now = {
				duration: length,
				time: this.number("DemoPlayerProgress") * length,
				paused: this.paused,
				rate: this.playbackRate,
				volume: this.volume,
				muted: this.muted,
				ended: this.ended,
			};
			if (known === null || known.duration !== now.duration) {
				// A demo that replaces another is a second one of these, which
				// is what a `<video>` does for a second file too.
				fire("loadedmetadata");
				fire("durationchange");
				// A demo starts playing by itself, which is what a video that
				// autoplays does - and a page hears that the same way, as the
				// `play` the next line sends.
				known = Object.assign({}, now, { paused: true, ended: false });
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
			// Once, where it stopped, and again only after somebody has taken
			// it away from the end.
			if (now.ended && !known.ended) {
				fire("ended");
			}
			known = now;
		};
		look();
	}
}

/** Where the program's script is, for a page that wants to fetch it early. */
export const programUrl = new URL(PROGRAM, import.meta.url).href;

// What a demo takes beyond `src`, spelled the way the address of the demo
// page spells it: a link somebody copied out of the viewer and an element
// somebody wrote by hand say the same things by the same names.

// The piece an element marks out runs from where it starts watching to where
// `end` says, which is the same pair of words a link out of the player uses.
// Without an `end` there is no piece.
function markClip(player, element) {
	const start = parseFloat(element.getAttribute("t"));
	const end = parseFloat(element.getAttribute("end"));
	if (isFinite(end) && end > 0) {
		player.clip(isFinite(start) && start > 0 ? start : 0, end);
	} else {
		player.clip(null);
	}
}

// The bar a video player has, over the demo rather than beside it, out of the
// browser's own buttons: the same things the viewer draws for itself when it
// is a program on somebody's desktop, in the same order, with the pictures
// from the same set. In a browser they are real buttons - a tooltip each, a
// tab order, a screen reader that can say what they are - and that is worth
// more here than a bar the program paints into the picture.
//
// Written here rather than in the page that shows it, so that a page with two
// demos on it gets two bars and neither knows about the other. What it is
// dressed in is `@ddnet/base/viewer.css`, which the page has to load; nothing
// here brings a stylesheet of its own.
//
// The parts are named with `data-role` rather than with an `id`, because an
// `id` is a page's to give out and there is only one of each.
const BAR_HTML = `
<div class="viewer-panel" data-role="export-panel" hidden>
	<div class="viewer-panel-title">Export video</div>
	<div class="viewer-form" data-role="export-settings"></div>
	<p class="viewer-panel-message" data-role="export-message" role="status" hidden></p>
	<div class="viewer-panel-actions">
		<button class="viewer-text-button" data-role="export-start">Start</button>
	</div>
</div>
<div class="viewer-menu" data-role="spectate-menu" hidden></div>
<div class="viewer-bar" data-role="bar" hidden>
	<input class="viewer-seek" data-role="seek" type="range" min="0" max="1000" value="0" step="1" aria-label="Seek">
	<div class="viewer-row">
		<button class="viewer-button" data-role="play" data-icon="pause" title="Pause (Space)" aria-label="Pause"></button>
		<button class="viewer-button" data-role="restart" data-icon="restart" title="Back to the start (Home)" aria-label="Back to the start"></button>
		<div class="viewer-volume">
			<button class="viewer-button" data-role="mute" data-icon="volume" title="Mute (M)" aria-label="Mute" aria-pressed="false"></button>
			<input class="viewer-volume-slider" data-role="volume" type="range" min="0" max="100" value="100" step="1" aria-label="Volume">
		</div>
		<span class="viewer-readout" data-role="time">0:00 / 0:00</span>
		<span class="viewer-spacer"></span>
		<button class="viewer-button viewer-optional" data-role="slower" data-icon="minus" title="Slower (Down arrow)" aria-label="Slower"></button>
		<span class="viewer-readout viewer-readout-speed viewer-optional" data-role="speed">1.00&times;</span>
		<button class="viewer-button viewer-optional" data-role="faster" data-icon="plus" title="Faster (Up arrow)" aria-label="Faster"></button>
		<button class="viewer-button viewer-optional" data-role="clip-start" data-icon="clip_start" title="Start the piece here (I)" aria-label="Start the piece here"></button>
		<button class="viewer-button viewer-optional" data-role="clip-end" data-icon="clip_end" title="End the piece here (O)" aria-label="End the piece here"></button>
		<button class="viewer-button viewer-optional" data-role="clip-clear" data-icon="clip_clear" title="The whole demo again" aria-label="The whole demo again" hidden></button>
		<span class="viewer-readout viewer-optional" data-role="clip-time" hidden></span>
		<button class="viewer-button" data-role="zoom-reset" data-icon="zoom_reset" title="Back to the size it started at" aria-label="Back to the size it started at" hidden></button>
		<button class="viewer-button" data-role="camera" data-icon="fit" title="The view the demo brought" aria-label="The view the demo brought" aria-pressed="false" hidden></button>
		<button class="viewer-button" data-role="spectate" data-icon="eye" title="Who to watch (N, P, F)" aria-label="Who to watch" aria-haspopup="true" aria-expanded="false"></button>
		<span class="viewer-readout" data-role="export-status" role="status" hidden></span>
		<button class="viewer-button" data-role="export" data-icon="save" title="Export video" aria-label="Export video" aria-haspopup="true" aria-expanded="false"></button>
		<button class="viewer-button" data-role="fullscreen" data-icon="fullscreen" title="Full screen" aria-label="Full screen"></button>
	</div>
</div>
`;

// How often the bar asks the player what to show. The viewer holds the main
// thread for as long as it draws and only lets go between frames, so asking is
// both simpler and cheaper than being called back.
const BAR_INTERVAL_MS = 200;

// Which bar the keys belong to. Two players on one page both hear the
// keyboard; `I` and `O` are the page's own keys, and page keys have to belong
// to one of them. The one last pointed at is the one meant, and until
// something has been pointed at it is the one that was made first.
let keyBar = null;

function formatTime(seconds) {
	const total = Math.max(0, Math.round(seconds));
	return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

/**
 * A bar of controls for one demo player: every button wired to that player and
 * nothing else, so a page may have as many as it has players.
 *
 * ```js
 * const player = await DemoPlayer.open({ canvas, src: "…/a.demo" });
 * const bar = new DemoControls(player, { container: document.body });
 * ```
 *
 * or, on an element, `<ddnet-demo controls="html">`, which is the same thing
 * said once.
 *
 * What it looks like comes from `@ddnet/base/viewer.css`; the page loads that
 * and may restyle anything in it. The parts carry `data-role`, so a page that
 * wants at one of them asks its own bar for it rather than the document.
 */
class CDemoControls {
	/**
	 * @param player The player to steer.
	 * @param options.container Where the bar goes. The element itself for a
	 * `<ddnet-demo>`, any box of the page's own otherwise.
	 * @param options.picture What a tap on it shows the bar again, and what
	 * fills the screen: the element for a `<ddnet-demo>`, the canvas
	 * otherwise.
	 * @param options.slot Which slot to sit in, `controls` inside an element.
	 * @param options.signal Takes the bar off again, the same as `destroy`.
	 */
	constructor(player, options) {
		const settings = Object.assign({ container: null, picture: null, slot: null, signal: undefined }, options || {});
		this.player = player;
		this.stopping = new AbortController();
		if (settings.signal) {
			settings.signal.addEventListener("abort", () => this.destroy(), { once: true });
		}
		this.picture = settings.picture || player.canvas;
		this.root = document.createElement("div");
		this.root.className = "viewer-controls viewer-controls-bottom";
		if (settings.slot !== null) {
			this.root.setAttribute("slot", settings.slot);
		}
		this.root.innerHTML = BAR_HTML;
		DDNetBase.paintIcons(this.root);
		if (settings.container !== null) {
			settings.container.append(this.root);
		}
		// While the slider is being dragged the demo stands still, and whoever
		// is dragging it is the one who says what it shows.
		this.seeking = false;
		this.pausedBeforeSeeking = false;
		// What the list of players looked like when it was last built, so that
		// it is only built again when it has changed - rebuilding it under
		// somebody reading it would take it away from them.
		this.playerList = "";
		if (keyBar === null) {
			keyBar = this;
		}
		this.wire();
		this.timer = setInterval(() => this.update(), BAR_INTERVAL_MS);
		this.update();
	}

	/** The bar itself, for a page that wants to put it somewhere else. */
	get element() {
		return this.root;
	}

	/** A part of the bar by the name it carries, for a page that wants at it. */
	part(role) {
		return this.root.querySelector(`[data-role="${role}"]`);
	}

	/** Takes the bar off the page again and stops asking the player anything. */
	destroy() {
		if (this.timer !== null) {
			clearInterval(this.timer);
			this.timer = null;
		}
		this.stopping.abort();
		this.root.remove();
		if (keyBar === this) {
			keyBar = null;
		}
	}

	// One panel at a time, and a click anywhere else closes it: two open panels
	// over a picture leave nothing of the picture, and a menu that has to be
	// dismissed with the button that opened it is one people leave open.
	openPanel(wanted) {
		for (const [button, panel] of [[this.part("spectate"), this.part("spectate-menu")], [this.part("export"), this.part("export-panel")]]) {
			const open = panel === wanted && panel.hidden;
			panel.hidden = !open;
			button.setAttribute("aria-expanded", open ? "true" : "false");
		}
	}

	// What went wrong, in the panel it went wrong in. An empty string takes the
	// line away again.
	say(message) {
		const line = this.part("export-message");
		line.textContent = message;
		line.hidden = message === "";
	}

	// Marking out a piece: where it is now, as the one end or the other. A
	// piece that would be empty or backwards is no piece, so a mark that lands
	// on the wrong side of the other one takes the other one with it.
	markClip(which) {
		const player = this.player;
		const length = player.duration;
		if (!(length > 0)) {
			return;
		}
		const now = player.progress() * length;
		const clip = player.clip();
		if (which === "start") {
			player.clip(now, clip !== null && clip.end > now ? clip.end : length);
		} else {
			player.clip(clip !== null && clip.start < now ? clip.start : 0, now);
		}
	}

	wire() {
		const player = this.player;
		const signal = this.stopping.signal;
		const on = (target, type, listener, extra) =>
			target.addEventListener(type, listener, Object.assign({ signal: signal }, extra || {}));
		const part = role => this.part(role);

		this.exportForm = DDNetBase.exportSettingsForm(part("export-settings"), { canvas: player.canvas });
		DDNetBase.fullscreen(part("fullscreen"), { element: this.picture, signal: signal });
		// The bar and what it opens are one thing to whoever uses them, so they
		// step aside together while nothing is happening - and a tap on the
		// demo takes them away or brings them back, the way a tap on a video
		// does everywhere. What they had opened goes with them.
		DDNetBase.autoHide([part("bar"), part("spectate-menu"), part("export-panel")], {
			picture: this.picture,
			onHide: () => this.openPanel(null),
			signal: signal,
		});
		// Encoding a video is the browser's to do, and not every browser does
		// it. Better said here than by an export that starts and then cannot
		// finish.
		if (typeof VideoEncoder === "undefined") {
			part("export").disabled = true;
			part("export").title = "This browser has no video encoder";
		}

		on(document, "pointerdown", event => {
			// Whoever was last pointed at owns the page's own keys, and a
			// press that lands anywhere else closes what this one had open.
			if (this.root.contains(event.target) || (this.picture !== null && this.picture.contains(event.target))) {
				keyBar = this;
			}
			if (!this.root.contains(event.target)) {
				this.openPanel(null);
			}
		});
		on(document, "keydown", event => {
			if (event.key === "Escape") {
				this.openPanel(null);
			}
		});
		// The keys a video editor uses for a piece, and the only two the page
		// has of its own: the viewer answers to the rest itself.
		on(document, "keydown", event => {
			if (event.ctrlKey || event.altKey || event.metaKey || event.target !== document.body || keyBar !== this) {
				return;
			}
			if (event.key === "i" || event.key === "I") {
				this.markClip("start");
			} else if (event.key === "o" || event.key === "O") {
				this.markClip("end");
			}
		});

		on(part("play"), "click", () => {
			if (player.paused) {
				player.play();
			} else {
				player.pause();
			}
		});
		on(part("restart"), "click", () => player.restart());
		// Dragging the slider says how loud, and a slider dragged to nothing is
		// the same thing as muting - so that what the button then says is what
		// the slider shows.
		on(part("mute"), "click", () => { player.muted = !player.muted; });
		on(part("volume"), "input", () => {
			const slider = part("volume");
			const level = slider.value / slider.max;
			player.muted = level === 0;
			if (level > 0) {
				player.volume = level;
			}
		});
		on(part("slower"), "click", () => { player.playbackRate = player.playbackRate / 1.25; });
		on(part("faster"), "click", () => { player.playbackRate = player.playbackRate * 1.25; });

		// The demo follows the bar while it is dragged, not once it is let go
		// of: what somebody is looking for is what they can see, and a bar that
		// only jumps at the end is one they have to guess with. While it is
		// being dragged the demo stands still, because one that goes on playing
		// runs out from under whoever is looking for a place in it; it goes on
		// again afterwards if it was going on before.
		on(part("seek"), "input", () => {
			const slider = part("seek");
			if (!this.seeking) {
				this.seeking = true;
				this.pausedBeforeSeeking = player.paused;
				player.pause();
			}
			player.seek(slider.value / slider.max);
		});
		on(part("seek"), "change", () => {
			const slider = part("seek");
			player.seek(slider.value / slider.max);
			this.seeking = false;
			if (!this.pausedBeforeSeeking) {
				player.play();
			}
		});

		on(part("clip-start"), "click", () => this.markClip("start"));
		on(part("clip-end"), "click", () => this.markClip("end"));
		on(part("clip-clear"), "click", () => player.clip(null));
		on(part("zoom-reset"), "click", () => player.resetZoom());
		on(part("camera"), "click", () => player.recordedCamera(!player.recordedCamera()));
		on(part("spectate"), "click", () => this.openPanel(part("spectate-menu")));
		// The export settings are asked for before the export runs, the same
		// ones the render tool takes on its command line. While one is being
		// written the button stops it instead.
		on(part("export"), "click", () => {
			if (player.exportState() === 1) {
				player.cancelExport();
				return;
			}
			this.openPanel(part("export-panel"));
		});
		on(part("export-start"), "click", async () => {
			// The browser only hands over a download that came out of a click,
			// so the export has to start from this handler and not from a
			// question the page asks itself later. Asking where to save is the
			// same: it has to happen while this click is still what the browser
			// thinks the user is doing. What is written there is written while
			// the demo renders, so nothing is kept in memory and a video the
			// size of a film is no different from a short one.
			if (window.showSaveFilePicker) {
				try {
					const handle = await window.showSaveFilePicker({
						suggestedName: "video.mp4",
						types: [{ description: "MP4 video", accept: { "video/mp4": [".mp4"] } }],
					});
					player.setVideoSink({ stream: await handle.createWritable(), done: () => null });
				} catch (error) {
					// A dismissed dialog is somebody changing their mind and
					// nothing to report. Anything else is a file that cannot be
					// written, which has to be said: a button that does nothing
					// at all is the worst of it.
					if (error && error.name === "AbortError") {
						return;
					}
					this.say(`The file could not be opened: ${(error && error.message) || error}`);
					return;
				}
			}
			this.say("");
			if (!player.startExport(this.exportForm.values())) {
				this.say("The export could not be started.");
				return;
			}
			this.openPanel(null);
		});
	}

	// Who there is to watch, which is the list the viewer opens behind the same
	// button. A demo names its players a snapshot or two in and people come and
	// go while it plays, so the list is built again whenever it has changed.
	updateSpectate() {
		const player = this.player;
		const menu = this.part("spectate-menu");
		const players = player.players() || [];
		const watching = player.spectating();
		const signature = JSON.stringify(players);
		if (signature !== this.playerList) {
			this.playerList = signature;
			// A demo a client recorded is a demo of whoever recorded it, so
			// watching over their shoulder is what it offers besides a camera
			// of one's own - and nothing to pick from, which is what the viewer
			// answers with an empty list. A demo a server recorded has nobody
			// whose demo it is, and offers everybody who happened to be there.
			const options = players.length === 0 ? []
				: [{ id: -1, name: "Free view", icon: "freeview" }]
					.concat(players.map(one => ({ id: one.id, name: one.name, icon: "eye" })));
			// A demo a client recorded has nobody to pick from, and a button
			// that opens nothing is a button in the way. The keys still step
			// through whoever is there.
			this.part("spectate").hidden = options.length === 0;
			if (options.length === 0 && !menu.hidden) {
				this.openPanel(null);
			}
			menu.replaceChildren(...options.map(option => {
				const item = document.createElement("button");
				item.className = "viewer-menu-item";
				item.dataset.icon = option.icon;
				item.dataset.id = option.id;
				item.appendChild(Object.assign(document.createElement("span"), { textContent: option.name }));
				item.addEventListener("click", () => {
					player.spectating(option.id);
					this.openPanel(null);
				}, { signal: this.stopping.signal });
				return item;
			}));
			DDNetBase.paintIcons(menu);
		}
		for (const item of menu.children) {
			item.setAttribute("aria-checked", item.dataset.id === String(watching) ? "true" : "false");
		}
	}

	// What the bar shows, asked of the player rather than remembered from the
	// clicks: the viewer answers to its own keys and to the pointer as well,
	// and a bar that only knew what it was told would drift away from it.
	update() {
		const player = this.player;
		const part = role => this.part(role);
		const bar = part("bar");
		const length = player.duration;
		if (!(length > 0)) {
			bar.hidden = true;
			return;
		}
		bar.hidden = false;
		const progress = player.progress();
		const seek = part("seek");
		if (!this.seeking) {
			seek.value = Math.round(progress * seek.max);
		}
		// The filled part of the bar is where the demo is: the browser draws a
		// slider, and a strip of time is not one.
		seek.style.setProperty("--viewer-progress", `${(seek.value / seek.max * 100).toFixed(2)}%`);
		// What is marked out, drawn along the bar so that the piece can be seen
		// rather than only read.
		const clip = player.clip();
		seek.style.setProperty("--viewer-clip-start", `${clip === null ? 0 : (clip.start / length * 100).toFixed(2)}%`);
		seek.style.setProperty("--viewer-clip-end", `${clip === null ? 0 : (clip.end / length * 100).toFixed(2)}%`);
		seek.classList.toggle("viewer-seek-clipped", clip !== null);
		part("clip-clear").hidden = clip === null;
		const clipTime = part("clip-time");
		clipTime.hidden = clip === null;
		if (clip !== null) {
			clipTime.textContent = `${formatTime(clip.start)}–${formatTime(clip.end)}`;
		}
		part("time").textContent = `${formatTime(progress * length)} / ${formatTime(length)}`;
		part("speed").textContent = `${player.playbackRate.toFixed(2)}×`;
		const paused = player.paused;
		const play = part("play");
		play.dataset.icon = paused ? "play" : "pause";
		play.title = paused ? "Play (Space)" : "Pause (Space)";
		play.setAttribute("aria-label", paused ? "Play" : "Pause");
		// Muting leaves the slider where it was rather than at nothing, because
		// what it was is what unmuting comes back to - the same as a video
		// player. While it is being dragged the slider is whoever is dragging
		// it.
		const muted = player.muted;
		const level = muted ? 0 : player.volume;
		const volume = part("volume");
		if (document.activeElement !== volume) {
			volume.value = Math.round(level * volume.max);
		}
		volume.style.setProperty("--viewer-progress", `${(volume.value / volume.max * 100).toFixed(2)}%`);
		const mute = part("mute");
		mute.dataset.icon = muted || level === 0 ? "volume_off" : "volume";
		mute.title = muted ? "Unmute (M)" : "Mute (M)";
		mute.setAttribute("aria-label", muted ? "Unmute" : "Mute");
		mute.setAttribute("aria-pressed", muted ? "true" : "false");
		this.updateSpectate();
		// The wheel over the canvas zooms, and there is no notch counted
		// anywhere to turn back by. The button is only there once there is
		// something for it to undo.
		part("zoom-reset").hidden = !player.zoomChanged();
		// A demo carries the view of whoever recorded it, and zooming leaves
		// that view behind. The button is only there while there is one to go
		// back to, and says whether it is the one being followed.
		const recorded = player.recordedCameraAvailable();
		const camera = part("camera");
		camera.hidden = !recorded;
		if (recorded) {
			const following = player.recordedCamera();
			camera.classList.toggle("viewer-on", following);
			camera.setAttribute("aria-pressed", following ? "true" : "false");
			camera.title = following ? "Following the view the demo brought" : "Back to the view the demo brought";
			camera.setAttribute("aria-label", camera.title);
		}
		// What the export is doing is asked for rather than remembered from the
		// click: starting one in a browser returns long before the browser has
		// an encoder, so the click itself knows nothing worth showing. While
		// one runs the button stops it and says how far it has got.
		const state = player.exportState();
		const running = state === 1;
		const exportButton = part("export");
		exportButton.dataset.icon = running ? "stop" : "save";
		exportButton.classList.toggle("viewer-on", running);
		exportButton.title = running ? "Stop the export"
			: state === 2 ? "The video was handed over"
			: state === 3 ? "The export failed"
			: clip !== null ? "Export the marked piece" : "Export video";
		exportButton.setAttribute("aria-label", exportButton.title);
		// An export that failed says why, where it was asked for.
		if (state === 3) {
			const reason = player.exportError();
			if (reason !== "" && this.part("export-message").textContent !== reason) {
				this.say(reason);
			}
		}
		// How far it has got and how much longer it has, beside the button that
		// stops it - which is where the viewer's own bar writes it too. A time
		// only once there is one: a rate takes a second or two to mean anything.
		const status = part("export-status");
		status.hidden = !running;
		if (running) {
			const left = player.exportSecondsLeft();
			const eta = left === null || left < 0 ? "" : ` · ${formatTime(left)} left`;
			status.textContent = `${Math.round(player.exportProgress() * 100)}%${eta}`;
		}
		DDNetBase.paintIcons(bar);
	}
}

class CDemoElement extends ViewerElement {
	static observedAttributes = ["src", "controls", "nozoom", "t", "end", "speed", "paused", "spec"];
	static viewerProgram = CDemoPlayer;
	static viewerBar = CDemoControls;
	static viewerKind = {
		zoomable: true,
		startAttributes: ["t", "speed", "paused"],
		startOptions: element => {
			const time = parseFloat(element.getAttribute("t"));
			const speed = parseFloat(element.getAttribute("speed"));
			const paused = element.getAttribute("paused");
			return {
				startTime: isFinite(time) && time > 0 ? time : undefined,
				speed: isFinite(speed) && speed > 0 ? speed : undefined,
				paused: paused !== null && paused !== "0" && paused !== "false",
			};
		},
		loaded: player => player.duration > 0,
		apply: (player, name, value, element) => {
			const number = parseFloat(value);
			if (name === "nozoom") {
				// Turned off while it is there, the way `controls` is on
				// while it is there: an attribute nobody wrote is the
				// viewer as it comes.
				player.zoomEnabled(value === null);
			} else if (name === "t" && isFinite(number)) {
				player.currentTime = number;
				// Where watching starts is also where a marked piece
				// starts, so moving one moves the other.
				markClip(player, element);
			} else if (name === "end") {
				markClip(player, element);
			} else if (name === "speed" && isFinite(number)) {
				player.playbackRate = number;
			} else if (name === "paused") {
				if (value === null || value === "0" || value === "false") {
					player.play();
				} else {
					player.pause();
				}
			} else if (name === "spec" && value !== null && value !== "") {
				// A name as well as a number, as in a link: who somebody is
				// worth watching is easier to write down than which client
				// id they happen to have.
				if (String(parseInt(value, 10)) === value) {
					player.spectating(parseInt(value, 10));
				} else {
					player.spectateName(value);
				}
			}
		},
	};
}

// The element answers to what a `<video>` answers to by handing the question
// on to the program it holds - which is the demo player, and which answers
// all of it already. An element without one yet answers what a viewer with
// nothing in it would.
const VIDEO_PROPERTIES = ["duration", "currentTime", "paused", "ended", "playbackRate", "volume", "muted", "src"];
const VIDEO_EVENTS = ["loadedmetadata", "durationchange", "play", "pause", "timeupdate", "ratechange", "volumechange", "ended"];
const VIDEO_EMPTY = { duration: NaN, currentTime: 0, paused: true, ended: false, playbackRate: 1, volume: 1, muted: false, src: "" };

for (const name of VIDEO_PROPERTIES) {
	Object.defineProperty(CDemoElement.prototype, name, {
		get() {
			return this.viewerInstance === null ? VIDEO_EMPTY[name] : this.viewerInstance[name];
		},
		set(value) {
			if (this.viewerInstance !== null) {
				this.viewerInstance[name] = value;
			}
		},
		configurable: true,
	});
}

Object.assign(CDemoElement.prototype, {
	/** Plays it, once there is something to play. */
	play() {
		if (this.viewerInstance !== null) {
			return this.viewerInstance.play();
		}
		// An element told to play before its program runs is a page that put
		// one in and pressed play; the program plays as soon as it is there.
		return Promise.resolve(this.ready).then(() => {
			if (this.viewerInstance !== null) {
				this.viewerInstance.play();
			}
		});
	},

	pause() {
		if (this.viewerInstance !== null) {
			this.viewerInstance.pause();
		}
	},

	// The program says what it is doing; the element says it again, so that a
	// page listens where it wrote the element rather than to something it has
	// to ask for first.
	viewerRunning() {
		const instance = this.viewerInstance;
		for (const name of VIDEO_EVENTS) {
			instance.addEventListener(name, event => {
				this.dispatchEvent(new CustomEvent(name, { detail: event.detail }));
			});
		}
	},
});

/**
 * Defines `<ddnet-demo>`, which this module does for itself as it loads. Here
 * for a page that takes it off and wants it back, and harmless twice.
 */
export function defineDemoElement() {
	if (typeof customElements === "undefined") {
		return;
	}
	if (customElements.get("ddnet-demo") === undefined) {
		customElements.define("ddnet-demo", CDemoElement);
	}
}

// Defined as this module is loaded, because the point of an element is that
// putting one in the page is all there is to it.
defineDemoElement();

/**
 * A demo player: `DemoPlayer.open({canvas, src})` for one on a canvas the
 * page keeps, `DemoPlayer.openPage({elements})` for a page that is nothing
 * else. Both answer with the player, running.
 */
export const DemoPlayer = CDemoPlayer;
/**
 * The bar of controls for one player, out of the browser's own buttons, for a
 * page that places it itself. `<ddnet-demo controls="html">` is the same thing
 * said in one word.
 */
export const DemoControls = CDemoControls;
/** The class behind the element, for whoever wants to extend it. */
export const DemoElement = CDemoElement;

export default {
	DemoPlayer,
	DemoControls,
	DemoElement,
	defineDemoElement,
	programUrl,
	// The base this is built on, so that a page that has this has the rest of
	// it too without a second import.
	base: DDNetBase,
};
