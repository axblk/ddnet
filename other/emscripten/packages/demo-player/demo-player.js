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
 * canvas, the files, the full screen, the icons - is `ddnet-loader`, the
 * runtime this is built on and which a page may use directly as well.
 *
 * The other end of the calls below is the `DemoPlayer*` block in
 * `src/engine/client/demo_player_client.cpp`. That is a C ABI, and it stops
 * here: nothing outside this file spells a function name or says which of the
 * arguments are numbers.
 */

import DDNetLoader, { importProgram, page, start, ViewerElement } from "ddnet-loader";

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

/** Where the program's script is, for a page that wants to fetch it early. */
export const programUrl = new URL(PROGRAM, import.meta.url).href;

// The viewers' own controls, as calls rather than as names to spell out:
// a page steering one should not have to know that `ccall` exists, nor
// which of the arguments are numbers. Every call answers `null` where the
// program is not running, the same as `call` does.
//
// The other end of these is the `DemoPlayer*` block in
// `src/engine/client/demo_player_client.cpp` and the `MapViewer*` block in
// `src/game/map/standalone/map_viewer_main.cpp`.
export function demoControls(instance) {
	const number = (name, argument) => argument === undefined
		? instance.call(name, "number")
		: instance.call(name, null, ["number"], [argument]);
	return {
		/**
		 * How big to draw, in the units the page measures its boxes in.
		 * Only for a viewer that sits in a box of the page's own: one that
		 * fills the window follows it by itself. See `followSize`.
		 */
		setSize: (Width, Height) => instance.call("DemoPlayerSetSize", null, ["number", "number"], [Math.round(Width), Math.round(Height)]),
		/** Whether a demo is loaded and how long it is, in seconds. */
		length: () => number("DemoPlayerLength"),
		/** How far it has played, between 0 and 1. */
		progress: () => number("DemoPlayerProgress"),
		paused: () => number("DemoPlayerPaused") === 1,
		pause: () => number("DemoPlayerSetPaused", 1),
		play: () => number("DemoPlayerSetPaused", 0),
		/** Jumps to a part of the demo, between 0 and 1. */
		seek: Fraction => number("DemoPlayerSeekPercent", Fraction),
		/** Jumps to a time in the demo, in seconds from its beginning. */
		seekTime: Seconds => number("DemoPlayerSeekToTime", Seconds),
		restart: () => instance.call("DemoPlayerSeekStart"),
		/** The playback speed, or sets it: 1 is as it was played. */
		speed: Value => Value === undefined ? number("DemoPlayerSpeed") : number("DemoPlayerSetSpeed", Value),
		exporting: () => number("DemoPlayerExporting") === 1,
		/**
		 * How an export is getting on: 0 before any was asked for, 1 while
		 * one is being written, 2 once one was handed over, 3 when it
		 * failed. What `startExport` answers cannot say, because in a
		 * browser it returns before the encoder has even been asked.
		 */
		exportState: () => number("DemoPlayerExportState"),
		/**
		 * How far the video being written has got, between 0 and 1. Not
		 * where the demo on the canvas is: the export reads it through a
		 * way of its own, so both move at once and apart.
		 */
		exportProgress: () => number("DemoPlayerExportProgress"),
		/**
		 * How much longer the export has to run, in seconds, or a negative
		 * number while there is no telling yet. Worked out from what is
		 * left of the demo and the rate frames are being written at, so it
		 * follows a machine that speeds up or slows down.
		 */
		exportSecondsLeft: () => number("DemoPlayerExportSecondsLeft"),
		/**
		 * Why the export that was last asked for failed, or an empty
		 * string when none has. A page is the only place this can be
		 * said: there is no log for whoever is looking at the demo.
		 */
		exportError: () => instance.call("DemoPlayerExportError", "string") || "",
		/** Throws away the export that is running, and its file with it. */
		cancelExport: () => instance.call("DemoPlayerCancelExport"),
		/**
		 * Who the demo is watched over the shoulder of, or sets it: a
		 * client id, -1 for a camera of one's own that the pointer drags
		 * around, or -2 for whoever recorded the demo. A demo a server
		 * recorded has nobody who recorded it, so it starts at -1.
		 */
		spectating: Id => Id === undefined
			? number("DemoPlayerSpectating")
			: number("DemoPlayerSetSpectate", Id),
		/** Follows whoever is called this, once the demo has named them. */
		spectateName: Name => instance.call("DemoPlayerSetSpectateName", null, ["string"], [Name || ""]),
		/** On to the next player there is, or the one before. */
		spectateStep: Direction => number("DemoPlayerSpectateStep", Direction),
		/**
		 * The players the demo has named so far, as `{id, name}` objects.
		 * A demo names them a snapshot or two in, so a list built from this
		 * is worth building again while it plays.
		 */
		players: () => JSON.parse(instance.call("DemoPlayerPlayers", "string") || "[]"),
		/**
		 * How much of the world is in the canvas, or multiplies it. The
		 * wheel over the canvas does the same thing.
		 */
		zoom: Factor => Factor === undefined
			? number("DemoPlayerZoom")
			: number("DemoPlayerZoomBy", Factor),
		/**
		 * Puts the zoom back where the demo started, and says whether
		 * there is anything to put back. Whoever turned the wheel a few
		 * times has no way back of their own, so a page with controls of
		 * its own offers one - while `zoomChanged()` says there is
		 * something to undo, the way the viewer's own bar does it.
		 */
		resetZoom: () => instance.call("DemoPlayerResetZoom"),
		zoomChanged: () => number("DemoPlayerZoomChanged") !== 0,
		/**
		 * Whether the wheel over the canvas, the zoom keys and two fingers
		 * on the picture zoom the demo, or switches that off. A page that
		 * scrolls around the viewer wants the wheel for itself, and one
		 * that shows a demo at one size and no other wants none of it.
		 * `zoom(Factor)` still works either way - this is only about what
		 * the viewer does on its own. Better with `zoom: false` at the
		 * start, which leaves it off from the first frame.
		 */
		zoomEnabled: Enable => Enable === undefined
			? number("DemoPlayerZoomEnabled") === 1
			: instance.call("DemoPlayerSetZoomEnabled", null, ["number"], [Enable ? 1 : 0]),
		/**
		 * How loud it is, between 0 and 1, the way `<video>` counts it.
		 * Said without an argument it answers, said with one it sets.
		 */
		volume: Level => Level === undefined
			? number("DemoPlayerVolume")
			: instance.call("DemoPlayerSetVolume", null, ["number"], [Math.min(Math.max(Level, 0), 1)]),
		/**
		 * Whether the sound is off. What it was is remembered, so turning
		 * it back on lands where it was rather than at a guess.
		 */
		muted: Mute => Mute === undefined
			? number("DemoPlayerMuted") === 1
			: instance.call("DemoPlayerSetMuted", null, ["number"], [Mute ? 1 : 0]),
		/**
		 * Whether the demo's own view is available at all: a demo carries
		 * where the camera was and how much of the world it had in it, but
		 * only if it was recorded with one and only while somebody is
		 * being followed.
		 */
		recordedCameraAvailable: () => number("DemoPlayerRecordedCamera") !== 0,
		/**
		 * Whether the demo's own view is the one being shown, or switches
		 * back to it. Zooming leaves it behind, which is what whoever
		 * zoomed asked for; this is the way back.
		 */
		recordedCamera: Use => Use === undefined
			? number("DemoPlayerRecordedCamera") === 2
			: instance.call("DemoPlayerSetRecordedCamera", null, ["number"], [Use ? 1 : 0]),
		/**
		 * Whether the viewer draws its own bar of controls over the demo,
		 * or switches it on and off. A page with controls of its own turns
		 * it off - better with `controls: false`, which leaves it off from
		 * the first frame rather than after it.
		 */
		controls: Show => Show === undefined
			? number("DemoPlayerControls") === 1
			: instance.call("DemoPlayerSetControls", null, ["number"], [Show ? 1 : 0]),
		/**
		 * Starts a video export, and says whether it started. The options
		 * are named as in `render`, because they are the same settings the
		 * render tool takes: `width`, `height`, `fps`, `crf`, `codec`,
		 * `audio`, `hud`, `chat`.
		 */
		startExport: options => {
			const settings = options || {};
			return instance.call("DemoPlayerStartExport", "number",
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
		},
	};
}

// What a `<video>` answers to, written once and put on both of the things
// here that answer to it: the element a page writes in its HTML, and the
// player a page makes for itself. Both keep the controls in
// `this.viewerControls` and both are `EventTarget`s, which is all of this
// needs.
const VIDEO_API = {
	// ----- what a page already knows, because a `<video>` has it -----
	//
	// A demo in a page is a video to whoever put it there: it plays, it
	// pauses, it is somewhere in its length, and it has a volume. Said in
	// the words a `<video>` says them in, a page that can drive one of
	// those can drive this without being told anything new - and the code
	// that does it reads the same either way.
	//
	// What is not here is what a demo has no answer for: `buffered`,
	// `seekable`, `readyState`, `networkState`, `preload`, `poster`,
	// `loop` and the tracks. A demo is fetched whole before it is shown,
	// so most of those would be one value that never changes.

	/** How long the demo is, in seconds, or `NaN` before there is one. */
	get duration() {
		const length = this.viewerControls === null ? 0 : this.viewerControls.length();
		return length > 0 ? length : NaN;
	},

	/** Where in it the viewer is, in seconds. */
	get currentTime() {
		const controls = this.viewerControls;
		if (controls === null) {
			return 0;
		}
		const length = controls.length();
		return length > 0 ? controls.progress() * length : 0;
	},
	set currentTime(seconds) {
		const time = parseFloat(seconds);
		if (this.viewerControls !== null && isFinite(time)) {
			this.viewerControls.seekTime(Math.max(0, time));
		}
	},

	/** Whether it stands still. A demo that is not there yet does. */
	get paused() {
		return this.viewerControls === null ? true : this.viewerControls.paused();
	},

	/**
	 * Whether it has played to its end. A demo stops on its last frame
	 * rather than going back to the start, so this stays true until
	 * somebody seeks away from there.
	 */
	get ended() {
		const controls = this.viewerControls;
		return controls !== null && controls.length() > 0 && controls.progress() >= END_OF_DEMO;
	},

	/** How fast, as a multiple of the speed it was recorded at. */
	get playbackRate() {
		return this.viewerControls === null ? 1 : this.viewerControls.speed();
	},
	set playbackRate(rate) {
		const speed = parseFloat(rate);
		if (this.viewerControls !== null && isFinite(speed) && speed > 0) {
			this.viewerControls.speed(speed);
		}
	},

	/** How loud, between 0 and 1. */
	get volume() {
		return this.viewerControls === null ? 1 : this.viewerControls.volume();
	},
	set volume(level) {
		const value = parseFloat(level);
		if (this.viewerControls !== null && isFinite(value)) {
			this.viewerControls.volume(Math.min(Math.max(value, 0), 1));
		}
	},

	get muted() {
		return this.viewerControls === null ? false : this.viewerControls.muted();
	},
	set muted(mute) {
		if (this.viewerControls !== null) {
			this.viewerControls.muted(mute === true);
		}
	},

	/**
	 * Plays it, and answers once it does. A `<video>` answers a promise
	 * here because a browser may refuse to play sound nobody asked for;
	 * nothing refuses here, but a page that awaits it is right to.
	 */
	play() {
		if (this.viewerControls !== null) {
			this.viewerControls.play();
			return Promise.resolve();
		}
		return Promise.resolve(this.ready).then(() => {
			if (this.viewerControls !== null) {
				this.viewerControls.play();
			}
		});
	},

	pause() {
		if (this.viewerControls !== null) {
			this.viewerControls.pause();
		}
	},

	// What a page hears about, in the words and the order a `<video>` says
	// them in: the length once it is known, then whatever changes. The
	// program is asked rather than telling - it holds the main thread while
	// it draws - so the asking happens here, once, instead of in every page
	// that wants to know.
	viewerRunning() {
		const controls = this.viewerControls;
		const signal = this.viewerStopping === null ? null : this.viewerStopping.signal;
		// What was true last time round, to tell a change from a state.
		let known = null;
		const fire = (name, detail) => this.dispatchEvent(new CustomEvent(name, { detail: detail }));
		const look = () => {
			if (signal === null || signal.aborted || this.viewerControls !== controls) {
				return;
			}
			setTimeout(look, VIDEO_EVENT_INTERVAL_MS);
			const length = controls.length();
			if (!(length > 0)) {
				return;
			}
			const now = {
				duration: length,
				time: controls.progress() * length,
				paused: controls.paused(),
				rate: controls.speed(),
				volume: controls.volume(),
				muted: controls.muted(),
				ended: controls.progress() >= END_OF_DEMO,
			};
			if (known === null || known.duration !== now.duration) {
				// A demo that replaces another is a second one of these,
				// which is what a `<video>` does for a second file too.
				fire("loadedmetadata");
				fire("durationchange");
				// A demo starts playing by itself, which is what a video
				// that autoplays does - and a page hears that the same way,
				// as the `play` the next line sends.
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
			// Once, where it stopped, and again only after somebody has
			// taken it away from the end.
			if (now.ended && !known.ended) {
				fire("ended");
			}
			known = now;
		};
		look();
	}
};

// What a demo takes beyond `src`, spelled the way the address of the demo
// page spells it: a link somebody copied out of the viewer and an element
// somebody wrote by hand say the same things by the same names.
class CDemoElement extends ViewerElement {
	static observedAttributes = ["src", "controls", "nozoom", "t", "speed", "paused", "spec"];
	static viewerKind = {
		// Where the program is: beside this module, wherever this module
		// was installed to.
		base: import.meta.url,
		script: PROGRAM,
		moduleName: MODULE_NAME,
		programName: PROGRAM_NAME,
		suffix: ".demo",
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
		controls: instance => demoControls(instance),
		loaded: controls => controls.length() > 0,
		apply: (controls, name, value) => {
			const number = parseFloat(value);
			if (name === "nozoom") {
				// Turned off while it is there, the way `controls` is on
				// while it is there: an attribute nobody wrote is the
				// viewer as it comes.
				controls.zoomEnabled(value === null);
			} else if (name === "t" && isFinite(number)) {
				controls.seekTime(number);
			} else if (name === "speed" && isFinite(number)) {
				controls.speed(number);
			} else if (name === "paused") {
				if (value === null || value === "0" || value === "false") {
					controls.play();
				} else {
					controls.pause();
				}
			} else if (name === "spec" && value !== null && value !== "") {
				// A name as well as a number, as in a link: who somebody is
				// worth watching is easier to write down than which client
				// id they happen to have.
				if (String(parseInt(value, 10)) === value) {
					controls.spectating(parseInt(value, 10));
				} else {
					controls.spectateName(value);
				}
			}
		},
	};
}

Object.defineProperties(CDemoElement.prototype, Object.getOwnPropertyDescriptors(VIDEO_API));

/**
 * A demo player a page made for itself, rather than one it wrote into its
 * HTML. The same picture and the same calls; what it does not have is the
 * element's attributes, because a page that made this has its own variables.
 */
class CDemoPlayer extends EventTarget {
	constructor() {
		super();
		this.viewerInstance = null;
		this.viewerControls = null;
		this.viewerSource = "";
	}

	/** The running program, for what this does not offer itself. */
	get instance() {
		return this.viewerInstance;
	}

	/** Everything the viewer can be asked, the low-level way. */
	get controls() {
		return this.viewerControls;
	}

	/** What is being shown, as the address it was named by. */
	get src() {
		return this.viewerSource;
	}

	/** Shows a file rather than a name to fetch. */
	async load(file) {
		this.viewerSource = "";
		return await this.viewerInstance.loadFile(file);
	}

	/** Shows what is at this address. */
	async loadUrl(url) {
		this.viewerSource = String(url);
		return await this.viewerInstance.loadUrl(url);
	}

	/** Stops the program and lets go of everything it held. */
	destroy() {
		const instance = this.viewerInstance;
		this.viewerInstance = null;
		this.viewerControls = null;
		if (instance !== null) {
			instance.destroy();
		}
	}
}

Object.defineProperties(CDemoPlayer.prototype, Object.getOwnPropertyDescriptors(VIDEO_API));

/**
 * Starts a demo player on a canvas.
 *
 * Everything `ddnet-loader`'s `start` takes may be said here as well - the
 * canvas, the file, where the data is, whether the viewer draws its own
 * controls, a signal to stop it with. What does not have to be said is which
 * program to run and where it is: that is what this module is.
 *
 * @returns a promise for the player, once the program runs.
 */
export async function createDemoPlayer(options) {
	const settings = Object.assign({}, options);
	const scriptUrl = settings.scriptUrl || programUrl;
	const factory = settings.module || await importProgram(scriptUrl, MODULE_NAME);
	const player = new CDemoPlayer();
	const instance = await start(Object.assign(settings, {
		module: factory,
		scriptUrl: scriptUrl,
		programName: settings.programName || PROGRAM_NAME,
		accept: settings.accept || [SUFFIX],
	}));
	player.viewerInstance = instance;
	player.viewerControls = demoControls(instance);
	player.viewerSource = typeof settings.file === "string" ? settings.file : "";
	// Now that there is something to watch, for whoever watches it.
	player.viewerRunning();
	return player;
}

/**
 * The same as `createDemoPlayer`, plus the furniture a page of nothing but a
 * demo player wants: the canvas filling the window, a line that says what is
 * loading, a console log for what the program writes, and a file named in the
 * page's own address.
 *
 * What it answers with is the running program rather than a demo player
 * object - this is for a page that steers the viewer through `demoControls`
 * and draws its own controls, which is what our own pages do.
 *
 * @returns a promise for the running instance.
 */
export async function demoPlayerPage(options) {
	const settings = Object.assign({}, options);
	const scriptUrl = settings.scriptUrl || programUrl;
	const factory = settings.module || await importProgram(scriptUrl, MODULE_NAME);
	return await page(Object.assign(settings, {
		module: factory,
		scriptUrl: scriptUrl,
		programName: settings.programName || PROGRAM_NAME,
		accept: settings.accept || [SUFFIX],
	}));
}

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

/** The class behind the element, for whoever wants to extend it. */
export const DemoElement = CDemoElement;
/** The class behind `createDemoPlayer`, for the same reason. */
export const DemoPlayer = CDemoPlayer;

export default {
	createDemoPlayer,
	demoPlayerPage,
	defineDemoElement,
	demoControls,
	DemoElement,
	DemoPlayer,
	programUrl,
	// The runtime this is built on, so that a page that has this has the rest
	// of it too without a second import.
	loader: DDNetLoader,
};
