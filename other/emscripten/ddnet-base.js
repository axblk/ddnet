// Starting one of the programs compiled for the browser, with the files it is
// given and, where it has something to show, on a canvas.
//
// Two ways in, both of them on `Program`. `Program.open` puts one program on
// one canvas and hands back a handle to call into it - that is all an
// embedding page needs, and it claims no globals, so a page may have two of
// them or a program of its own beside them. `Program.openPage` is that plus
// the furniture our own pages share: a loading line, a console log, and the
// canvas filling the window. A package says the rest by subclassing the
// class: which program to start, where its script lies, and what it can be
// asked once it runs.
//
// A module. `import DDNetBase from "./ddnet-base.js"` for all of it, or
// `import { Program } from …` for one thing at a time; a page loads it with
// `<script type="module">`, which is what its own three pages do. A module is
// also the only form a package can honestly offer: the alternative would be a
// global claimed by a script, which is neither importable nor two things at
// once - and there is no way back from a module to a global that a plain
// `<script>` could wait for.
//
// Modules are strict by themselves, so nothing here says so.

const DDNetBase = (() => {
	// What this library answers with when it refuses or cannot do something.
	// The `code` is what a caller branches on: the sentence is for whoever
	// reads it and may be reworded, the code is part of the API and is not.
	class DDNetBaseError extends Error {
		constructor(code, message) {
			super(message);
			this.name = "DDNetBaseError";
			this.code = code;
		}
	}

	const fail = (code, message) => new DDNetBaseError(code, message);

	// This library's own version, which is not the game's: it says what the
	// API looks like, so it changes when the API does.
	const VERSION = "1.0.0";

	// What each way in takes, and of what shape. An option nobody reads is the
	// kind of mistake that otherwise turns up much later and in the words of
	// whatever went without it - a mistyped `fps` is first heard of from the
	// encoder, saying something about a frame rate of sixty.
	const START_OPTIONS = [
		"accept", "acceptLinks", "arguments", "canvas", "controls", "dataBase", "file", "fileArgument",
		"fileName", "homePath", "module", "needsWebGpu", "onExit", "onOutput", "onProgress",
		"onRenderProgress", "onVideo", "orientation", "paused", "persist", "programName", "scriptUrl",
		"signal", "speed", "startTime", "sweepVideoScratch", "urlParams", "videoSink", "zoom",
	];
	const PAGE_OPTIONS = START_OPTIONS.concat(["elements"]);
	// One table for every option any program here takes, this module's own and
	// the ones a package adds on top - `fps` is a number wherever it is said,
	// and one table is what makes every complaint about one read the same.
	const OPTION_SHAPES = {
		accept: "array", acceptLinks: "boolean", arguments: "array", audio: "boolean", canvas: "canvas",
		chat: "boolean", codec: "string", controls: "boolean", crf: "number", dataBase: "string",
		elements: "object", fileArgument: "string", fileName: "string", follow: "string", fps: "number",
		height: "number", homePath: "string", hud: "boolean", module: "function", moduleName: "string",
		name: "string", needsWebGpu: "boolean", onExit: "function", onOutput: "function",
		onProgress: "function", onRenderProgress: "function", onStart: "function", onVideo: "function",
		orientation: "string", output: "string", paused: "boolean", persist: "boolean", preset: "string",
		programName: "string", scriptUrl: "string", settings: "array",
		signal: "signal", speed: "number", startTime: "number", sweepVideoScratch: "boolean",
		urlParams: "array", width: "number", worker: "boolean", zoom: "boolean",
	};

	// What a shape is called when it is being asked for, so that a complaint
	// reads as a sentence rather than as a table lookup.
	const SHAPE_NAMES = {
		array: "an array", boolean: "true or false", canvas: "a canvas", function: "a function",
		number: "a number", object: "an object", signal: "an AbortSignal", string: "a string",
	};

	function hasShape(value, shape) {
		switch (shape) {
		case "array":
			return Array.isArray(value);
		case "canvas":
			// A canvas from another document is still a canvas, so what is
			// asked is whether it behaves like one rather than where it came
			// from.
			return typeof value === "object" && value !== null && typeof value.getContext === "function";
		case "number":
			return typeof value === "number" && isFinite(value);
		case "object":
			return typeof value === "object" && value !== null;
		case "signal":
			return typeof value === "object" && value !== null && "aborted" in value && typeof value.addEventListener === "function";
		default:
			return typeof value === shape;
		}
	}

	// What a class is called out in the open: the codebase writes
	// `CDemoPlayer` and a page imports `DemoPlayer`, and a complaint about an
	// option is for the page to read.
	const publicName = program => program.name.replace(/^C(?=[A-Z])/, "");

	function checkOptions(where, options, allowed) {
		if (typeof options !== "object" || options === null) {
			throw fail("BadOption", `${where} takes an object of options`);
		}
		for (const [name, value] of Object.entries(options)) {
			if (!allowed.includes(name)) {
				throw fail("BadOption", `${where} does not take '${name}'. It takes: ${allowed.join(", ")}.`);
			}
			// An option left out is an option left at its default, so only
			// what is actually there is looked at.
			const shape = OPTION_SHAPES[name];
			if (shape !== undefined && value !== undefined && value !== null && !hasShape(value, shape)) {
				throw fail("BadOption", `${where} wants ${SHAPE_NAMES[shape]} for '${name}'`);
			}
		}
	}

	const DEFAULT_HOME_PATH = "/home/web_user/.local/share/ddnet";
	// Where this module is, so that a worker can be given the same one. A
	// module knows this of itself, wherever it is running - which a script had
	// to be asked for while it ran, and could only answer on a page.
	const MODULE_URL = import.meta.url;
	// A file named in the URL is fetched into the same place a dropped file
	// goes. Anything larger than this is refused rather than filling the tab's
	// memory with whatever a link pointed at.
	const MAX_URL_FILE_BYTES = 256 * 1024 * 1024;

	function sanitizeFilename(filename) {
		return filename.replace(/[\/\\\?\|\*<>:"]/g, "_");
	}

	function parseAnsiColorRgb(line) {
		// Only handles RGB format because our logging system only uses that.
		const match = line.match(/^\x1b\[38;2;(\d+);(\d+);(\d+)m([\s\S]*?)\x1b\[0m$/);
		if (!match) {
			return { color: "black", message: line };
		}
		const [, r, g, b, message] = match;
		return { color: `rgb(${r}, ${g}, ${b})`, message: message };
	}

	// The value of a parameter, from the fragment first: a fragment never
	// reaches a server, so a link to somebody's demo stays between them and
	// their browser.
	function urlParameter(name) {
		const fragment = new URLSearchParams(location.hash.replace(/^#/, ""));
		if (fragment.has(name)) {
			return fragment.get(name);
		}
		const query = new URLSearchParams(location.search);
		return query.has(name) ? query.get(name) : null;
	}

	// The other direction: what the page's URL says about where it is, so that
	// whoever copies it out of the address bar copies what they are looking at.
	// Only the named parameters are touched and the rest of the fragment stays
	// as it was written - `URLSearchParams` would escape every slash and colon
	// of a demo's URL and turn a link somebody can read into one they cannot. A
	// parameter set to `null` is taken out.
	//
	// The entry is replaced rather than added: a viewer that moves would
	// otherwise fill the history with every second it played.
	function setUrlParameters(values) {
		const parts = location.hash.replace(/^#/, "").split("&")
			.filter(part => part !== "" && !(part.split("=")[0] in values));
		for (const [name, value] of Object.entries(values)) {
			if (value !== null && value !== undefined) {
				parts.push(`${name}=${value}`);
			}
		}
		const link = parts.length === 0 ? location.pathname + location.search : `#${parts.join("&")}`;
		if (link !== location.hash && !(location.hash === "" && parts.length === 0)) {
			history.replaceState(null, "", link);
		}
	}

	// The service worker keeps every file of the data directory it fetched,
	// which is safe because a URL there names the contents it carries. What it
	// must not keep is a file that the build no longer has, and the only thing
	// that knows which those are is the index - so once per load it is told to
	// go and compare. Once per page, however many instances are on it.
	var sweptDataCache = false;
	function sweepDataCache() {
		if (!sweptDataCache && navigator.serviceWorker && navigator.serviceWorker.controller) {
			sweptDataCache = true;
			navigator.serviceWorker.controller.postMessage({ type: "ddnet-data-sweep" });
		}
	}

	// What the browser has to be able to do before any of this is worth
	// starting, said in one sentence rather than found out over a page of the
	// program's own complaints.
	//
	// Threads are the one thing every program here needs, and a browser only
	// hands out the memory they share to a page that is cross-origin isolated.
	// Drawing without a window needs WebGPU, because that is the only backend
	// left once the window is gone.
	async function supportError(needsWebGpu) {
		if (typeof SharedArrayBuffer === "undefined" || self.crossOriginIsolated === false) {
			return fail("CrossOriginRefused",
				"This page is not cross-origin isolated, so the browser withholds the shared memory this needs. " +
				"The page has to send Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp, " +
				"or load coi-serviceworker.js before anything else.");
		}
		if (needsWebGpu) {
			if (!navigator.gpu) {
				return fail("NoWebGpu", "This browser has no WebGPU, which is what a render without a window draws with. A current Chrome or Firefox has it.");
			}
			// Having WebGPU and having something to draw with are two
			// different things: a browser started without a graphics device,
			// or one that blocklisted the one it found, answers with nothing
			// and only says so when it is asked.
			const adapter = await navigator.gpu.requestAdapter().catch(() => null);
			if (!adapter) {
				return fail("NoWebGpu", "This browser has WebGPU but no graphics adapter it is willing to use, so there is nothing to draw with.");
			}
		}
		return null;
	}

	// The same thing in one sentence, for a page that only wants to say so.
	async function supportProblem(needsWebGpu) {
		const error = await supportError(needsWebGpu);
		return error === null ? null : error.message;
	}

	// The encoders the browser may be asked for, one profile per family, the
	// same list and the same order as `Module.ddnetVideoFamilies` in
	// `src/engine/client/video_webcodecs.cpp` - the program picks the first
	// entry of a family it can actually configure, so a page only has to name
	// the family. A page offering a choice has to ask which of them work here,
	// because that differs between browsers and between machines.
	const VIDEO_CODEC_FAMILIES = [
		["H.264", ["avc1.640028", "avc1.4D0028", "avc1.42E01E"]],
		["H.265", ["hvc1.1.6.L120.B0"]],
		["AV1", ["av01.0.08M.08"]],
	];
	var videoCodecsProbe = null;

	async function videoCodecs() {
		if (videoCodecsProbe === null) {
			videoCodecsProbe = (async () => {
				if (typeof VideoEncoder === "undefined") {
					return [];
				}
				const supported = async candidates => {
					for (const codec of candidates) {
						try {
							const support = await VideoEncoder.isConfigSupported(
								{ codec: codec, width: 1280, height: 720, bitrate: 4000000, framerate: 60 });
							if (support.supported) {
								return codec;
							}
						} catch (error) {
							// An encoder this browser does not know at all.
						}
					}
					return null;
				};
				const found = await Promise.all(VIDEO_CODEC_FAMILIES.map(
					family => supported(family[1]).then(name => name === null ? null : { name, display: family[0] })));
				return found.filter(entry => entry !== null);
			})();
		}
		return await videoCodecsProbe;
	}

	// Controls over a picture belong to whoever is looking at it, not in front
	// of it: they show themselves when the pointer moves and step aside again
	// when it stops, the way a video player's do. What is being pointed at or
	// typed into stays, so a menu does not close itself under the hand that
	// opened it.
	function autoHide(elements, options) {
		const settings = Object.assign({ delay: 2500, picture: null, onHide: null, signal: undefined }, options || {});
		// What is put on the window and on the picture outlives the controls
		// themselves, so whoever takes those off the page says so here.
		const on = (target, type, listener, extra) =>
			target.addEventListener(type, listener, Object.assign({ signal: settings.signal }, extra || {}));
		const all = Array.isArray(elements) ? elements : [elements];
		var timer = null;
		var held = 0;
		var shown = false;
		const hide = () => {
			shown = false;
			for (const element of all) {
				element.classList.add("faded");
			}
			if (settings.onHide) {
				settings.onHide();
			}
		};
		const show = () => {
			shown = true;
			for (const element of all) {
				element.classList.remove("faded");
			}
			clearTimeout(timer);
			timer = held > 0 ? null : setTimeout(hide, settings.delay);
		};
		for (const element of all) {
			on(element, "pointerenter", () => { held++; show(); });
			on(element, "pointerleave", () => { held = Math.max(held - 1, 0); show(); });
			on(element, "focusin", () => { held++; show(); });
			on(element, "focusout", () => { held = Math.max(held - 1, 0); show(); });
		}
		// A press that went down on the picture, stayed where it was and was
		// let go of again is a tap, and a tap on the picture is how a video
		// player is told to show its controls or to get out of the way. The
		// same rule the viewers use for the bar they draw themselves, see
		// `CViewerControls::Render`.
		if (settings.picture !== null) {
			const TAP_DISTANCE = 16;
			const TAP_TIME = 400;
			var pressed = null;
			on(settings.picture, "pointerdown", event => {
				// Read before the press reaches the handler below that shows
				// everything again: what a tap does depends on what was there
				// when it started.
				pressed = { x: event.clientX, y: event.clientY, when: performance.now(), shown: shown };
			}, { capture: true });
			on(settings.picture, "pointerup", event => {
				if (pressed === null) {
					return;
				}
				const moved = Math.hypot(event.clientX - pressed.x, event.clientY - pressed.y);
				const tap = moved <= TAP_DISTANCE && performance.now() - pressed.when <= TAP_TIME;
				const wasShown = pressed.shown;
				pressed = null;
				if (tap && wasShown) {
					hide();
				} else if (tap) {
					show();
				}
			});
			on(settings.picture, "pointercancel", () => { pressed = null; });
		}
		for (const event of ["pointermove", "pointerdown", "keydown", "wheel"]) {
			on(window, event, show, { passive: true });
		}
		show();
		return { show, hide, shown: () => shown };
	}

	// A viewer that fills the window needs none of this: the window tells it
	// when it changes shape, and the picture follows. One that sits in a box of
	// a page's own has nothing to follow - nothing tells a window that a box
	// beside it was laid out differently - so the box is watched here and the
	// program is told its size.
	//
	// The other end of it is `DemoPlayerSetSize` in
	// `src/engine/client/demo_player_client.cpp` and `MapViewerSetSize` in
	// `src/game/map/standalone/map_viewer_main.cpp`.
	function followSize(element, controls, options) {
		const settings = Object.assign({ signal: undefined }, options || {});
		if (typeof ResizeObserver !== "function") {
			return { stop: () => {} };
		}
		var width = 0;
		var height = 0;
		const measure = () => {
			const box = element.getBoundingClientRect();
			const nextWidth = Math.round(box.width);
			const nextHeight = Math.round(box.height);
			// A box of no size is a box that is not being shown; the program
			// keeps the size it had rather than being told to draw nothing.
			if (nextWidth <= 0 || nextHeight <= 0 || (nextWidth === width && nextHeight === height)) {
				return;
			}
			width = nextWidth;
			height = nextHeight;
			controls.setSize(width, height);
		};
		const observer = new ResizeObserver(measure);
		observer.observe(element);
		const stop = () => observer.disconnect();
		if (settings.signal) {
			settings.signal.addEventListener("abort", stop, { once: true });
		}
		measure();
		return { stop: stop };
	}

	// The pictures on the buttons, as the browser draws pictures: one square
	// outline each, in whatever colour the button they sit on is written in.
	// They are named after `CViewerControls::EIcon` in
	// `src/engine/client/viewer_controls.h` and drawn to say the same thing,
	// so that a page and the program behind it do not offer the same button
	// with two different pictures on it.
	//
	// Here are the ones any page here draws. What only one viewer's buttons
	// need comes with that viewer's package and is put in through `addIcons`,
	// because a page that shows a map has no use for a picture of a volume
	// slider.
	const ICONS = {
		menu: '<rect x="3" y="5" width="18" height="2.6" rx="1.3"/><rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/><rect x="3" y="16.4" width="18" height="2.6" rx="1.3"/>',
		minus: '<rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/>',
		plus: '<rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/><rect x="10.7" y="3" width="2.6" height="18" rx="1.3"/>',
		fit: '<rect x="2.7" y="4.7" width="18.6" height="14.6" rx="2" fill="none" stroke="currentColor" stroke-width="2.4"/><rect x="6.6" y="8.6" width="10.8" height="6.8" opacity="0.55"/>',
		save: '<path d="M12 2.8V12" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/><path d="M8 8.8 12 13 16 8.8" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/><path d="M3.8 15.4v3.4a1.4 1.4 0 0 0 1.4 1.4h13.6a1.4 1.4 0 0 0 1.4-1.4v-3.4" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>',
		stop: '<rect x="4.5" y="4.5" width="15" height="15" rx="3"/>',
		fullscreen: '<path d="M3.4 9.6V3.4h6.2M20.6 9.6V3.4h-6.2M3.4 14.4v6.2h6.2M20.6 14.4v6.2h-6.2" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>',
	};

	// Pictures a package brings for its own buttons, put where every page can
	// ask for them. Said as the package is loaded, so that a page which
	// imported it can write `data-icon="play"` and mean it.
	function addIcons(pictures) {
		Object.assign(ICONS, pictures);
	}

	function icon(name) {
		const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
		svg.setAttribute("viewBox", "0 0 24 24");
		svg.setAttribute("aria-hidden", "true");
		svg.setAttribute("focusable", "false");
		svg.innerHTML = ICONS[name] || "";
		return svg;
	}

	// Every button that named a picture in the markup gets it, so that a page
	// says what is on its buttons where it says what its buttons are.
	function paintIcons(root) {
		for (const element of (root || document).querySelectorAll("[data-icon]")) {
			const wanted = element.dataset.icon;
			if (element.dataset.painted === wanted) {
				continue;
			}
			element.dataset.painted = wanted;
			const drawn = element.querySelector("svg");
			if (drawn !== null) {
				drawn.remove();
			}
			element.prepend(icon(wanted));
		}
	}

	// Filling the screen is the browser's to do and only out of a click of its
	// own. What goes full screen is the page and not the canvas: a canvas on
	// its own takes the controls off the screen with it, since they are beside
	// it and not in it.
	// Safari before 16.4 - which is every iPad that has not been updated since
	// 2023 - only has this under its own name, and an iPhone has it under no
	// name at all: there, only a video may fill the screen, and a button that
	// asks for it is a button that does nothing. So it is asked for under both
	// names, and where there is neither the button is taken away rather than
	// left sitting there dead.
	function fullscreenSupported() {
		return document.fullscreenEnabled === true || document.webkitFullscreenEnabled === true;
	}

	function isFullscreen() {
		return (document.fullscreenElement || document.webkitFullscreenElement) != null;
	}

	// What is being watched is wide and a phone is tall, so turning the phone
	// sideways is what makes the most of it - but that is a strong thing to do
	// to somebody else's device, and whoever holds it upright usually means to.
	// A device that is locked upright in its own settings would be turned
	// against that, and one that is not stays turned until it is given back.
	// So nothing is locked unless a page asks: `orientation: "landscape"`, or
	// anything else the Screen Orientation API takes. A browser that will not
	// do it says so and nothing else happens.
	function wantedOrientation(options) {
		const wanted = (options || {}).orientation;
		if (wanted === undefined || wanted === null || wanted === "" || wanted === "any") {
			return null;
		}
		return wanted;
	}

	// A lock outlives the full screen it was taken for unless somebody gives it
	// back, and leaving the full screen is not always a button here: Escape, the
	// browser's own way out and the viewer's own bar all end it. So the release
	// hangs off the change rather than off any of them.
	let orientationLocked = false;
	function releaseOrientation() {
		if (!orientationLocked) {
			return;
		}
		orientationLocked = false;
		if (screen.orientation && screen.orientation.unlock) {
			screen.orientation.unlock();
		}
	}

	function releaseOrientationWhenLeaving() {
		if (isFullscreen()) {
			return;
		}
		releaseOrientation();
		document.removeEventListener("fullscreenchange", releaseOrientationWhenLeaving);
		document.removeEventListener("webkitfullscreenchange", releaseOrientationWhenLeaving);
	}

	// Asked for by a button on the page, and by the viewers themselves for the
	// button they draw over the picture: what a browser will do about filling
	// the screen is the same either way, and it is knowledge the page has.
	function toggleFullscreen(options) {
		const settings = Object.assign({ element: document.documentElement }, options || {});
		if (!fullscreenSupported()) {
			return;
		}
		if (isFullscreen()) {
			releaseOrientation();
			(document.exitFullscreen || document.webkitExitFullscreen).call(document);
			return;
		}
		const orientation = wantedOrientation(settings);
		// A browser that says no says it in a promise nobody is waiting on,
		// which would otherwise be an unhandled rejection. It is said out loud
		// all the same: a button that does nothing is the hardest kind of
		// fault to look into, and the reason is in that rejection.
		const ask = settings.element.requestFullscreen || settings.element.webkitRequestFullscreen;
		// The older name returns nothing at all, so there is nothing to wait
		// on and nothing to be told; what follows is only for the newer one.
		Promise.resolve(ask.call(settings.element)).then(() => {
			// Only a page that fills the screen may ask for this, which is why
			// it is asked for here and nowhere else.
			if (orientation === null || !screen.orientation || !screen.orientation.lock) {
				return;
			}
			orientationLocked = true;
			document.addEventListener("fullscreenchange", releaseOrientationWhenLeaving);
			document.addEventListener("webkitfullscreenchange", releaseOrientationWhenLeaving);
			screen.orientation.lock(orientation).catch(() => { orientationLocked = false; });
		}).catch(error => console.warn("DDNetBase: this browser refused to fill the screen:", (error && error.message) || error));
	}

	function fullscreen(button, options) {
		if (!fullscreenSupported()) {
			button.hidden = true;
			return { supported: false };
		}
		const update = () => {
			const on = isFullscreen();
			const says = on ? "Leave full screen" : "Full screen";
			// A button with a picture on it says what it does in the words a
			// screen reader and a tooltip use; one with words on it says it in
			// those words. Writing over a picture would rub it out.
			if (button.dataset.icon === undefined) {
				button.textContent = says;
			}
			button.setAttribute("aria-label", says);
			button.setAttribute("aria-pressed", on ? "true" : "false");
			button.title = on ? `${says} (Escape)` : says;
		};
		const signal = (options || {}).signal;
		button.addEventListener("click", () => toggleFullscreen(options), { signal: signal });
		// On the document rather than on the button, so it also follows the key
		// that leaves full screen - and therefore worth taking off again when
		// the button goes.
		document.addEventListener("fullscreenchange", update, { signal: signal });
		document.addEventListener("webkitfullscreenchange", update, { signal: signal });
		update();
		return { supported: true };
	}

	// The sizes and frame rates a video is offered in, the same ones the client
	// offers in its own render window - see `gs_aaVideoResolutionPresets` and
	// `s_aFpsPresets` in `src/game/client/components/menus.cpp`. Anything else
	// is typed in, which is what `Custom` is for.
	const VIDEO_SIZE_PRESETS = [[1280, 720], [1920, 1080], [2560, 1440], [3840, 2160]];
	const VIDEO_FPS_PRESETS = [30, 50, 60, 120, 144, 240];

	function exportSettingsForm(container, options) {
		const settings = Object.assign({ canvas: null, audio: false }, options || {});
		const element = (tag, properties) => Object.assign(document.createElement(tag), properties || {});
		const labelled = (text, ...controls) => {
			const label = element("label");
			if (text !== null) {
				label.append(text + " ");
			}
			label.append(...controls);
			container.appendChild(label);
			return label;
		};
		const option = (value, text) => element("option", { value: String(value), textContent: text });

		const size = element("select");
		if (settings.canvas != null) {
			size.appendChild(option("canvas", "Canvas size"));
		}
		for (const [width, height] of VIDEO_SIZE_PRESETS) {
			size.appendChild(option(`${width}x${height}`, `${width} × ${height}`));
		}
		size.appendChild(option("custom", "Custom"));
		size.value = settings.canvas != null ? "canvas" : "1920x1080";
		labelled("Size", size);

		const width = element("input", { type: "number", min: 16, max: 8192, step: 2, value: 1920 });
		const height = element("input", { type: "number", min: 16, max: 8192, step: 2, value: 1080 });
		const customSize = labelled(null, width, element("span", { textContent: "×" }), height);

		const fps = element("select");
		for (const value of VIDEO_FPS_PRESETS) {
			fps.appendChild(option(value, String(value)));
		}
		fps.appendChild(option("custom", "Custom"));
		fps.value = "60";
		labelled("FPS", fps);
		const customFpsValue = element("input", { type: "number", min: 1, max: 1000, value: 60 });
		const customFps = labelled(null, customFpsValue);

		const crf = element("input", { type: "number", min: 0, max: 51, value: 23, title: "Lower is better, 0 is lossless" });
		labelled("Quality", crf);
		const codec = element("select");
		codec.appendChild(option("", "Default"));
		labelled("Codec", codec);
		// Which encoders are here is the browser's answer, not ours, so the
		// menu is what it says rather than a list that may name one it does
		// not have.
		videoCodecs().then(codecs => {
			for (const entry of codecs) {
				codec.appendChild(option(entry.name, entry.display));
			}
			applyCodec();
		});

		// The codec menu is filled in from what the browser answers, which is
		// after this returns. A codec asked for before then is remembered and
		// chosen once it is there - and left alone if it never turns up.
		var wantedCodec = null;
		const applyCodec = () => {
			if (wantedCodec !== null && [...codec.options].some(entry => entry.value === wantedCodec)) {
				codec.value = wantedCodec;
				wantedCodec = null;
			}
		};

		const audio = element("input", { type: "checkbox", checked: settings.audio === true });
		const hud = element("input", { type: "checkbox" });
		const chat = element("input", { type: "checkbox", checked: true });
		labelled(null, audio, element("span", { textContent: " Sound" }));
		labelled(null, hud, element("span", { textContent: " Interface" }));
		labelled(null, chat, element("span", { textContent: " Chat" }));

		// The fields that are typed into are only there when there is something
		// to type: a menu of sizes with two boxes beside it reads as though the
		// boxes were what the menu is about.
		const updateCustom = () => {
			customSize.hidden = size.value !== "custom";
			customFps.hidden = fps.value !== "custom";
		};
		size.addEventListener("change", updateCustom);
		fps.addEventListener("change", updateCustom);
		updateCustom();

		// Whoever wants to remember what was chosen - in a link, or for the
		// next visit - is told when it changes rather than having to ask.
		const changed = [];
		for (const control of [size, width, height, fps, customFpsValue, crf, codec, audio, hud, chat]) {
			control.addEventListener("change", () => {
				for (const listener of changed) {
					listener();
				}
			});
		}

		return {
			/** What was chosen, as `render` and `startExport` take it. */
			values() {
				var chosenWidth = parseInt(width.value, 10);
				var chosenHeight = parseInt(height.value, 10);
				if (size.value === "canvas") {
					chosenWidth = settings.canvas.width;
					chosenHeight = settings.canvas.height;
				} else if (size.value !== "custom") {
					const parts = size.value.split("x");
					chosenWidth = parseInt(parts[0], 10);
					chosenHeight = parseInt(parts[1], 10);
				}
				return {
					// An encoder takes whole macroblocks, and a canvas is
					// whatever size the window left it.
					width: chosenWidth & ~1,
					height: chosenHeight & ~1,
					fps: parseInt(fps.value === "custom" ? customFpsValue.value : fps.value, 10),
					crf: parseInt(crf.value, 10),
					codec: codec.value === "" ? null : codec.value,
					audio: audio.checked,
					hud: hud.checked,
					chat: chat.checked,
				};
			},

			/**
			 * Puts settings into the form, in the shape `values` answers. What
			 * is left out is left as it was, so a link that names only a frame
			 * rate changes only that.
			 */
			setValues(values) {
				const chosen = values || {};
				if (chosen.width > 0 && chosen.height > 0) {
					const preset = `${chosen.width}x${chosen.height}`;
					width.value = chosen.width;
					height.value = chosen.height;
					size.value = [...size.options].some(entry => entry.value === preset) ? preset : "custom";
				}
				if (chosen.fps > 0) {
					const preset = String(chosen.fps);
					customFpsValue.value = chosen.fps;
					fps.value = [...fps.options].some(entry => entry.value === preset) ? preset : "custom";
				}
				if (chosen.crf !== undefined && chosen.crf !== null) {
					crf.value = chosen.crf;
				}
				if (chosen.codec !== undefined && chosen.codec !== null) {
					wantedCodec = chosen.codec;
					applyCodec();
				}
				for (const [name, control] of [["audio", audio], ["hud", hud], ["chat", chat]]) {
					if (chosen[name] !== undefined && chosen[name] !== null) {
						control.checked = chosen[name] === true;
					}
				}
				updateCustom();
			},

			/** Called whenever any of it is changed. */
			onChange(listener) {
				changed.push(listener);
			},
		};
	}

	// A script from another origin, as a blob. Fetched rather than linked
	// because a blob belongs to this page: a worker may be made from it, and
	// `importScripts` takes it without asking the server for permission it
	// cannot give.
	async function fetchScript(url) {
		const response = await fetch(url.href, { mode: "cors" });
		if (!response.ok) {
			throw fail("FetchFailed", `${url.href} answered ${response.status} ${response.statusText}`);
		}
		return new Blob([await response.text()], { type: "text/javascript" });
	}

	// The program's script, as something a module can hold: it is a plain
	// script that names itself, and there is no way to run one of those from a
	// module or from a module worker. So it is read, given a line that hands
	// its name out, and imported as the module that makes of it. Nothing of the
	// script itself is changed; where its own files are it is told through
	// `scriptUrl` when it is started.
	//
	// Remembered by where it came from: a page with two viewers on it, or a
	// queue of renders, would otherwise translate the same script again for
	// each of them.
	const importedPrograms = new Map();

	async function importProgram(scriptUrl, moduleName) {
		const key = `${scriptUrl}|${moduleName}`;
		var pending = importedPrograms.get(key);
		if (pending === undefined) {
			pending = (async () => {
				const response = await fetch(scriptUrl);
				if (!response.ok) {
					throw fail("FetchFailed", `${scriptUrl} answered ${response.status} ${response.statusText}`);
				}
				const text = await response.text();
				const wrapped = URL.createObjectURL(new Blob(
					[`${text}\nexport default ${moduleName};`], { type: "text/javascript" }));
				try {
					return (await import(wrapped)).default;
				} finally {
					URL.revokeObjectURL(wrapped);
				}
			})();
			importedPrograms.set(key, pending);
		}
		return await pending;
	}

	// Where a video goes while it is being made. A fragmented MP4 is valid
	// after every fragment, so it is written to a file in the browser's own
	// private storage as it is encoded and only handed over when it is
	// finished: an export that takes minutes is then bounded by what the disk
	// holds rather than by what the tab can keep. Where there is no such
	// storage the export falls back to keeping the file in memory.
	const VIDEO_SCRATCH_DIRECTORY = "ddnet-video";
	async function videoScratchDirectory(create) {
		if (!navigator.storage || !navigator.storage.getDirectory) {
			return null;
		}
		const root = await navigator.storage.getDirectory();
		return await root.getDirectoryHandle(VIDEO_SCRATCH_DIRECTORY, { create: create });
	}

	async function videoScratchSink(info) {
		const directory = await videoScratchDirectory(true);
		if (directory == null) {
			return null;
		}
		const handle = await directory.getFileHandle(`${Date.now()}-${sanitizeFilename(info.fileName)}`, { create: true });
		return {
			stream: await handle.createWritable(),
			// The finished file, which is on disk rather than in memory: what
			// is handed over here is a handle to it, not its contents.
			done: async () => await handle.getFile(),
		};
	}

	// A video that was written but never taken is a video nobody wanted, so the
	// scratch files of earlier visits go at the start of this one. Once per
	// page, and before anything writes a new one - which is why a render in a
	// worker does not do this itself: every worker is a page as far as this
	// script is concerned, and the second render of a batch would sweep away
	// the video of the first one while the page was still offering it.
	var sweptVideoScratch = false;
	async function sweepVideoScratch() {
		if (sweptVideoScratch) {
			return;
		}
		sweptVideoScratch = true;
		try {
			const directory = await videoScratchDirectory(false);
			if (directory == null) {
				return;
			}
			for await (const name of directory.keys()) {
				await directory.removeEntry(name).catch(() => {});
			}
		} catch (error) {
			// No directory yet, or no permission to have one: nothing to sweep.
		}
	}

	// The browser's own shortcuts stay the browser's, whatever the program
	// makes of the keyboard. Two programs on one page both ask for this and
	// both get it, which changes nothing: stopping a key twice is stopping it.
	function installKeyGuard(signal) {
		document.addEventListener('keydown', e => {
			// Always use default browser actions for Ctrl+F5 (refresh), F11 (fullscreen), F12 (developer console).
			if ((e.ctrlKey && e.key === 'F5') || e.key === 'F11' || e.key == 'F12') {
				e.stopPropagation();
			}
		}, { capture: true, signal: signal });
	}

	/**
	 * One running program. What a page gets back from `start`.
	 */
	// Why a stop is a stop: an `AbortSignal` says so in its own words if it
	// was given a reason, and in ours if it was not.
	const abortError = signal => signal.reason !== undefined && signal.reason !== null
		? signal.reason
		: fail("Stopped", "This was stopped.");

	class CProgram extends EventTarget {
		/**
		 * What a program is, said once by the class rather than by every page
		 * that opens one: where its script lies, what the factory in it is
		 * called, what it answers to in a log, and which files it takes.
		 *
		 * A class that leaves them alone is opened with a factory handed in,
		 * which is what the full client does - it is loaded by the page and
		 * has no package of its own.
		 */
		static script = null;
		static base = MODULE_URL;
		static moduleName = null;
		static programName = null;
		static suffix = null;

		/** Where this program's script lies, worked out from the class. */
		static scriptUrl() {
			return this.script === null ? null : new URL(this.script, this.base).href;
		}

		/**
		 * Starts the program on a canvas and answers with it, running.
		 *
		 * Everything a page has to say about it is said here; what the class
		 * already knows - its script, its factory, its files - it fills in
		 * itself, and what the page says instead of that wins.
		 *
		 * @param options.module The factory the program's script defines, so
		 * `DDNetClient`, `DDNetDemoPlayer` or `DDNetMapViewer`. A class that
		 * knows where its script lies fetches it itself and needs none.
		 * @param options.canvas The canvas to draw on.
		 * @param options.dataBase Where the `data` directory is, if it is not
		 * next to the page.
		 * @param options.scriptUrl Where the program's script was loaded from,
		 * when that is another origin than this page.
		 * @param options.videoSink Where an exported video is written, see
		 * `setVideoSink`. Without it the video goes to a scratch file and is
		 * handed over when it is done.
		 * @param options.needsWebGpu Whether the program draws without a
		 * window, which only WebGPU does. Checked before it starts.
		 * @param options.accept The file suffixes this program takes.
		 * @param options.file A file to start on: bytes, a `File`, or the URL of
		 * one.
		 * @param options.urlParams Parameters of the page's URL that may name a
		 * file to open.
		 * @param options.controls `false` leaves off the bar of controls a
		 * viewer otherwise draws over what it shows, for a page that puts its
		 * own beside the canvas.
		 * @param options.signal An `AbortSignal`. Aborting it asks the program
		 * to stop; aborting it before the call starts nothing at all, and
		 * either way the promise ends with whatever the signal was aborted
		 * with.
		 * @param options.onOutput Called for every line the program writes.
		 * @param options.onProgress Called while the program is being fetched.
		 * @param options.onExit Called once the program has stopped.
		 *
		 * The program is also an `EventTarget`, which is the other way of
		 * hearing the same three things: `output` with
		 * `{detail: {message, kind}}`, `progress` with `{detail: {text}}` and
		 * `exit`. A listener and a callback can both be there; output nobody
		 * listens to and nobody was handed goes to the console.
		 *
		 * @returns a promise for the running program.
		 */
		static async open(options) {
			const settings = await this.settings(options);
			checkOptions(`${publicName(this)}.open`, settings, START_OPTIONS);
			return await new this(settings).run();
		}

		/**
		 * The same, plus the furniture our own pages share: the canvas
		 * filling the window, a line that says what is loading, a console log
		 * for what the program writes.
		 */
		static async openPage(options) {
			const settings = await this.settings(options);
			checkOptions(`${publicName(this)}.openPage`, settings, PAGE_OPTIONS);
			return await new this(pageOptions(settings)).run();
		}

		/**
		 * What was asked for, with what the class knows filled in - including
		 * the program's factory, which is fetched here where the class knows
		 * where it lies.
		 */
		static async settings(options) {
			const settings = Object.assign({}, options);
			if (this.script !== null) {
				settings.scriptUrl = settings.scriptUrl || this.scriptUrl();
				settings.programName = settings.programName || this.programName;
				if (this.suffix !== null && settings.accept === undefined) {
					settings.accept = [this.suffix];
				}
				if (settings.module === undefined) {
					settings.module = await importProgram(settings.scriptUrl, this.moduleName);
				}
			}
			return settings;
		}

		constructor(options) {
			super();
			// Which kinds of event somebody is listening for, so that output
			// nobody is listening to still reaches the console and output that
			// somebody is listening to does not reach it twice.
			this.listened = new Set();
			this.options = options;
			// A program that draws into a video file rather than onto the page
			// has no canvas, and nothing here may assume one.
			this.canvas = options.canvas || null;
			this.homePath = options.homePath || DEFAULT_HOME_PATH;
			this.accept = options.accept || [];
			this.acceptLinks = options.acceptLinks === true;
			this.module = null;
			this.exited = false;
			// Everything this hangs on the page hangs on one signal, and
			// `destroy` lets go of all of it at once. A page that lives as
			// long as its program never needs that; one that puts a viewer up
			// and takes it down again does.
			this.stopping = new AbortController();
			this.destroyed = false;
			this.video = null;
			this.pendingSink = null;
			this.finished = new Promise(resolve => {
				this.reportFinished = resolve;
			});
		}

		addEventListener(type, listener, options) {
			this.listened.add(type);
			super.addEventListener(type, listener, options);
		}

		// Both ways at once, on purpose: the callbacks were here first and the
		// three own pages use them, and an event is what a page embedding one
		// of these would rather have.
		say(type, detail) {
			this.dispatchEvent(new CustomEvent(type, { detail: detail }));
		}

		output(message, kind) {
			this.say("output", { message: message, kind: kind || {} });
			if (this.options.onOutput) {
				this.options.onOutput(message, kind || {});
			} else if (this.listened.has("output")) {
				// Somebody is listening, so the console would only say it
				// again.
			} else if (kind && kind.error) {
				console.error(message);
			} else {
				console.log(message);
			}
		}

		sayProgress(text) {
			this.say("progress", { text: text });
			if (this.options.onProgress) {
				this.options.onProgress(text);
			}
		}

		/**
		 * Calls one of the program's exported functions, the same arguments as
		 * emscripten's `ccall`. Answers `null` rather than throwing where the
		 * program is not running, which is most of the time a page is open.
		 */
		call(name, returnType, argTypes, args) {
			if (this.module == null || this.exited) {
				return null;
			}
			try {
				return this.module.ccall(name, returnType || null, argTypes || [], args || []);
			} catch (error) {
				return null;
			}
		}

		/**
		 * Puts bytes where the program looks for what the user brought along and
		 * hands the path to it, the same way a dropped file arrives.
		 *
		 * @returns the path it was written to.
		 */
		async loadBytes(name, data) {
			const path = this.filePath({ name: name });
			if (path == null) {
				throw fail("FileRefused", `${name} is not a kind of file this program takes`);
			}
			const filePath = await this.writeFile(path, name, data);
			this.call('EmscriptenCallbackDropFile', null, ['string'], [filePath]);
			return filePath;
		}

		async loadFile(file) {
			return await this.loadBytes(file.name, new Uint8Array(await file.arrayBuffer()));
		}

		/**
		 * Fetches a file and hands it over. Only http and https, and only from a
		 * server that allows it to be read from here, which is what a
		 * cross-origin request asks and answers.
		 */
		async loadUrl(url) {
			const filePath = await this.fetchUrlFile(url);
			this.call('EmscriptenCallbackDropFile', null, ['string'], [filePath]);
			return filePath;
		}

		/**
		 * Where the next video this program exports is written to, for a page
		 * that has somewhere better than the default: a `WritableStream`, or
		 * `{stream, done}` whose `done` answers with the finished file if there
		 * is still one to hand over.
		 *
		 * A page asking the user where to save has to ask while the click that
		 * started it is still the browser's idea of what the user is doing,
		 * which is why this is set before the export starts rather than
		 * answered when it does.
		 */
		setVideoSink(sink) {
			this.pendingSink = sink;
		}

		/** Asks the program to stop. `onExit` follows once it has. */
		quit() {
			this.call('EmscriptenCallbackQuit');
		}

		/**
		 * Lets go of the page. Every handler this put on it comes off, the
		 * sound stops, the error handler is given back, and the program is
		 * asked to quit if it is still running.
		 *
		 * A page that lives as long as its program never needs this - the page
		 * going is the program going. A page that takes a viewer off and puts
		 * another on, or a custom element being removed, does: without it the
		 * handlers on the window and on the document outlive what they were
		 * for.
		 *
		 * Asking for anything afterwards is answered the way a program that
		 * has stopped is answered, which is with nothing.
		 */
		destroy() {
			if (this.destroyed) {
				return;
			}
			this.destroyed = true;
			// The program first, while there is still something to ask: it is
			// what holds the canvas, the sound and the files. It stops its own
			// sound as it goes, and taking the sound out from under a program
			// that is still stopping leaves it reading something that is no
			// longer there - so only one that has already stopped leaves any
			// to stop here.
			if (this.exited) {
				this.stopAudio();
			} else {
				this.quit();
			}
			this.stopping.abort();
			this.restoreErrorHandler();
			// Whoever is waiting for the program is let go of: a program told
			// to stop from outside may never reach `onExit`.
			this.reportFinished();
		}

		// Everything below is how the two above are done, and how an instance
		// comes up in the first place.

		async writeFile(path, name, data) {
			const FS = this.module.FS;
			FS.mkdirTree(`${path}/upload`);
			const filePath = `${path}/upload/${sanitizeFilename(name)}`;
			FS.writeFile(filePath, data);
			return filePath;
		}

		// Where a file of this kind belongs below the home directory, or `null`
		// for a file this program does not take.
		filePath(file) {
			for (const suffix of this.accept) {
				if (file.name.endsWith(suffix)) {
					return `${this.homePath}/${suffix === ".demo" ? "demos" : "maps"}`;
				}
			}
			return null;
		}

		async fetchUrlFile(url) {
			const response = await fetch(url, { mode: "cors" });
			if (!response.ok) {
				throw fail("FetchFailed", `The server answered ${response.status} ${response.statusText}`);
			}
			const buffer = await response.arrayBuffer();
			if (buffer.byteLength > MAX_URL_FILE_BYTES) {
				throw fail("FileTooLarge", `The file is larger than ${MAX_URL_FILE_BYTES} bytes`);
			}
			const name = decodeURIComponent(new URL(url).pathname.split("/").pop() || "download");
			const path = this.filePath({ name: name }) || `${this.homePath}/${this.accept[0] === ".demo" ? "demos" : "maps"}`;
			return await this.writeFile(path, name, new Uint8Array(buffer));
		}

		droppedItemFromDataTransfer(dataTransfer) {
			var result = { file: null, path: null, link: null };
			if (!dataTransfer.items) {
				return result;
			}
			for (const item of dataTransfer.items) {
				if (item.kind == 'file') {
					const file = item.getAsFile();
					const path = this.filePath(file);
					if (path == null) {
						continue;
					}
					if (result.link != null) {
						alert("You cannot drop files and links at the same time.");
						break;
					}
					if (result.file != null) {
						alert("You cannot open multiple files at the same time. Only the first file will be opened.");
						break;
					}
					result.file = file;
					result.path = path;
				} else if (item.kind == 'string' && this.acceptLinks) {
					const string = dataTransfer.getData(item.type);
					if (string.startsWith('ddnet://')) {
						if (result.file != null) {
							alert("You cannot drop files and links at the same time.");
							break;
						}
						if (result.link != null && result.link != string) {
							alert("You cannot connect to multiple URLs at the same time. You will be connected to the first URL.");
							break;
						}
						result.link = string;
					}
				}
			}
			return result;
		}

		unsupportedDropMessage() {
			const kinds = this.accept.map(suffix => `${suffix} files`).join(" and ");
			return this.acceptLinks
				? `The items you dropped are not supported. You can drop ${kinds}, as well as ddnet:// links.`
				: `The items you dropped are not supported. You can drop ${kinds}.`;
		}

		installCanvasHandlers() {
			const instance = this;
			const canvas = this.canvas;
			if (canvas == null) {
				return;
			}
			const signal = this.stopping.signal;
			installKeyGuard(signal);
			canvas.addEventListener('contextmenu', e => e.preventDefault(), { signal: signal });
			canvas.addEventListener('webglcontextcreationerror', e => {
				instance.output(`Failed to create WebGL context: ${e.statusMessage || "Unknown error"}`, { error: true, bold: true });
			}, { signal: signal });
			canvas.addEventListener('webglcontextlost', e => {
				// The program cannot currently recover from GL context loss, because it
				// would require reloading all textures, framebuffers etc.
				instance.output(`The WebGL context was lost: ${e.statusMessage || "Unknown error"}`, { error: true, bold: true });
				instance.quit();
			}, { signal: signal });
			canvas.addEventListener('dragover', e => {
				e.preventDefault();
				e.dataTransfer.dropEffect = "none";
				if (!e.dataTransfer.items) {
					return;
				}
				for (const item of e.dataTransfer.items) {
					if (item.kind == 'file' || item.kind == 'string') {
						e.dataTransfer.dropEffect = "copy";
						return;
					}
				}
			}, { signal: signal });
			canvas.addEventListener('drop', async e => {
				e.preventDefault();
				const droppedItem = instance.droppedItemFromDataTransfer(e.dataTransfer);
				if (droppedItem.file != null) {
					await instance.loadFile(droppedItem.file);
				} else if (droppedItem.link != null) {
					instance.call('EmscriptenCallbackDropFile', null, ['string'], [droppedItem.link]);
				} else {
					alert(instance.unsupportedDropMessage());
				}
			}, { signal: signal });
		}

		// What the export asks when it starts, in order: what the page put
		// there for this one export, what it gave once for all of them, and the
		// scratch file otherwise.
		async videoSink(info) {
			if (this.pendingSink != null) {
				const sink = this.pendingSink;
				this.pendingSink = null;
				return sink;
			}
			if (this.options.videoSink) {
				return typeof this.options.videoSink === "function" ? await this.options.videoSink(info) : this.options.videoSink;
			}
			return await videoScratchSink(info);
		}

		// The audio keeps running after the runtime has stopped, and says so
		// once per buffer. Only this instance's, and only its own doing.
		stopAudio() {
			const SDL2 = this.module == null ? undefined : this.module['SDL2'];
			if (SDL2 === undefined) {
				return;
			}
			if (SDL2.audio !== undefined) {
				if (SDL2.audio.scriptProcessorNode !== undefined) {
					SDL2.audio.scriptProcessorNode.disconnect();
				}
				if (SDL2.audio.silenceTimer !== undefined) {
					clearInterval(SDL2.audio.silenceTimer);
				}
				SDL2.audio = undefined;
			}
			if (SDL2.audioContext !== undefined) {
				SDL2.audioContext.close();
				SDL2.audioContext = undefined;
			}
		}

		// An error out of the wasm leaves the runtime standing there rather than
		// exiting, which hangs a test run and skips `onExit`. Chained onto
		// whatever the page already had, since this is the page's own handler.
		installErrorHandler() {
			const instance = this;
			// `self`, not `window`: a program rendering in a worker has no
			// window, and this is the one thing here that would miss it.
			const previous = self.onerror;
			this.previousErrorHandler = previous;
			this.errorHandler = function(message, url, line, column, error) {
				instance.output(message, { error: true, bold: true, fatal: true });
				if (error && error.stack) {
					for (const line of error.stack.split("\n")) {
						if (line.length > 0) {
							instance.output(line, { error: true });
						}
					}
				}
				instance.stopAudio();
				// A runtime that has already aborted answers every call with an
				// exception, which `call` swallows, so there is nothing to ask
				// first. One that is past answering never reaches `onExit`
				// either, so whoever waits for the program is let go here.
				instance.call('EmscriptenCallbackQuitForce');
				instance.reportFinished();
				if (previous) {
					return previous.apply(this, arguments);
				}
			};
			self.onerror = this.errorHandler;
		}

		// Only if it is still ours: a page that put its own on afterwards has
		// chained onto this one, and taking ours away would take theirs with
		// it.
		restoreErrorHandler() {
			if (this.errorHandler !== undefined && self.onerror === this.errorHandler) {
				self.onerror = this.previousErrorHandler;
			}
		}

		async run() {
			const instance = this;
			const options = this.options;
			if (typeof options.module !== "function") {
				throw fail("BadOption", "DDNetBase needs the program's factory, for example `module: DDNetDemoPlayer`");
			}
			// A signal that is already aborted is a program that is not
			// started, and one aborted later is a program asked to stop. Both
			// come back as what the signal was aborted with.
			if (options.signal) {
				if (options.signal.aborted) {
					throw abortError(options.signal);
				}
				options.signal.addEventListener("abort", () => this.quit(), { once: true, signal: this.stopping.signal });
			}
			// Said once, and said here: what follows would say it a hundred
			// times, in the words of whatever failed first.
			const problem = await supportError(options.needsWebGpu === true);
			if (problem !== null) {
				this.output(problem.message, { error: true, bold: true, fatal: true });
				throw problem;
			}
			sweepDataCache();
			if (options.sweepVideoScratch !== false) {
				sweepVideoScratch();
			}
			this.installErrorHandler();

			const program = await this.program();
			var totalDependencies = 0;
			this.module = await options.module({
				websocket: {
					url: location.protocol === "https:" ? "wss://" : "ws://",
				},
				noInitialRun: true,
				canvas: this.canvas === null ? undefined : this.canvas,
				// A program from another origin is loaded from a blob, see
				// `foreignProgram`, and then only this says where its own files
				// are - the blob has no directory to look next to.
				mainScriptUrlOrBlob: program === null ? undefined : program.script,
				locateFile: program === null ? undefined : path => new URL(path, program.base).href,
				// What a browser will do about filling the screen, for the
				// controls a viewer draws itself. Handed to the program rather
				// than looked up by it: a module claims no global for anything
				// to look up. Read by `ViewerFullscreen`, see
				// `src/engine/client/viewer_fullscreen.cpp`.
				ddnetFullscreen: {
					supported: () => fullscreenSupported(),
					active: () => isFullscreen(),
					toggle: () => toggleFullscreen({ orientation: options.orientation }),
				},
				// Where a finished video goes. Without it the export offers the
				// file as a download, which is what somebody watching wants and
				// a page rendering by itself does not. Read by the WebCodecs
				// export, see `src/engine/client/video_webcodecs.cpp`.
				ddnetVideoOutput: options.onVideo,
				// Where a video is written while it is made, see `videoSink`.
				ddnetVideoSink: info => instance.videoSink(info),
				// How far a render has got, once a second while it runs.
				ddnetRenderProgress: status => {
					instance.say("renderprogress", status);
					if (options.onRenderProgress) {
						options.onRenderProgress(status);
					}
				},
				// Where `data` is, for a page that keeps it somewhere other than
				// next to itself. A program from another origin brings its own,
				// so that is where to look unless the page says otherwise. Read
				// by `webfs`, see `src/base/webfs.h`.
				ddnetDataBase: options.dataBase || (program === null ? undefined : new URL(".", program.base).href),
				// A viewer draws its own controls unless the page says it has
				// its own, and zooms what it shows unless the page wants the
				// wheel for itself. Said here rather than switched off once it
				// runs, so that neither is ever there to begin with - and the
				// same for where in the demo to start: a page can only ask for
				// that once it has been given a turn, and by then the demo has
				// played the seconds it took to get there.
				arguments: (options.controls === false ? ["--no-controls"] : [])
					.concat(options.zoom === false ? ["--no-zoom"] : [])
					.concat(options.startTime === undefined ? [] : ["--time", String(options.startTime)])
					.concat(options.speed === undefined ? [] : ["--speed", String(options.speed)])
					.concat(options.paused === true ? ["--paused"] : [])
					.concat(options.arguments || []),
				print: text => {
					const parsedLine = parseAnsiColorRgb(text);
					console.log(parsedLine.message);
					instance.output(parsedLine.message, { color: parsedLine.color });
				},
				printErr: text => {
					console.error(text);
					instance.output(text, { error: true });
				},
				onExit: () => instance.onExit(),
				monitorRunDependencies: left => {
					totalDependencies = Math.max(totalDependencies, left);
					const value = totalDependencies - left;
					instance.sayProgress(totalDependencies == 1 ? "Loading…" : `Loading… (${value}/${totalDependencies})`);
				},
			});

			this.installCanvasHandlers();
			await this.mountPersistentStorage();

			const args = this.module.arguments;
			const path = await this.initialFile();
			if (path != null) {
				if (options.fileArgument) {
					args.push(options.fileArgument);
				}
				args.push(path);
			}
			if (this.canvas != null) {
				this.canvas.style.display = "block";
			}
			this.module.callMain(args);
			// Now that it runs, for a program that wants to say what it is
			// doing without every page having to ask it.
			this.running();
			return this;
		}

		/**
		 * Called once the program runs. Nothing happens here; a program that
		 * says what it is doing - a demo player sending the events a
		 * `<video>` sends - starts doing it from here.
		 */
		running() {}

		// Where the program's own script is, when the page said. Emscripten
		// otherwise works it out from whatever script is running, which is right
		// on a page that loaded it and wrong everywhere else - in a worker, or
		// where the program comes from another origin.
		//
		// From another origin it is not enough to name it: the program brings
		// threads, and a thread's script has to come from the page's own origin,
		// so a worker made from a foreign URL is refused outright. The program
		// is fetched instead - which a cross-origin request may do where the
		// server permits it - and handed on as a blob, which belongs to whoever
		// made it.
		//
		// The rest of what this takes is the page's own doing and cannot be done
		// from here: a page that runs this has to be cross-origin isolated, so
		// `Cross-Origin-Opener-Policy: same-origin` and
		// `Cross-Origin-Embedder-Policy: require-corp` on the page itself, or
		// `coi-serviceworker.js` next to it; and under `require-corp` the
		// `<script>` that fetches the program needs `crossorigin`, because
		// without it the browser asks for it without CORS and refuses what comes
		// back.
		async program() {
			if (!this.options.scriptUrl) {
				return null;
			}
			const base = new URL(this.options.scriptUrl, location.href);
			if (base.origin === location.origin) {
				return { script: base.href, base: base };
			}
			return { script: await fetchScript(base), base: base };
		}

		// The file the program starts on, if it was given one: bytes the page
		// handed over, or a URL - the page's own or one of its parameters.
		async initialFile() {
			const file = this.options.file;
			if (file != null && typeof file !== "string") {
				const name = this.options.fileName || file.name || "upload";
				const bytes = file instanceof Uint8Array ? file : new Uint8Array(file instanceof ArrayBuffer ? file : await file.arrayBuffer());
				return await this.writeFile(this.filePath({ name: name }) || this.homePath, name, bytes);
			}
			const url = typeof file === "string" ? new URL(file, location.href).href : this.urlArgument();
			if (url == null) {
				return null;
			}
			this.output(`Downloading ${url}…`);
			try {
				return await this.fetchUrlFile(url);
			} catch (downloadError) {
				// A fetch that is refused says nothing about why: the browser
				// keeps that to itself and throws a bare TypeError. Almost
				// always it is the other server not allowing this page to read
				// it, so that is what is said - along with the way round it,
				// which is to bring the file along instead.
				const refused = downloadError instanceof TypeError;
				const sameOrigin = new URL(url, location.href).origin === location.origin;
				const reason = refused && !sameOrigin
					? `${new URL(url).origin} does not allow this page to read its files (no CORS headers)`
					: downloadError.message;
				this.output(`Failed to download ${url}: ${reason}. You can drop the file into this page instead.`,
					{ error: true, bold: true, fatal: true });
				return null;
			}
		}

		urlArgument() {
			for (const name of this.options.urlParams || []) {
				const value = urlParameter(name);
				if (value == null || value === "") {
					continue;
				}
				const url = new URL(value, location.href);
				if (url.protocol !== "http:" && url.protocol !== "https:") {
					this.output(`Refused to load ${url.protocol} URL`, { error: true });
					continue;
				}
				return url.href;
			}
			return null;
		}

		// Everything the user writes lives here and outlives the tab, which is
		// what IndexedDB is for. It has to be there before the program starts.
		async mountPersistentStorage() {
			const instance = this;
			const FS = this.module.FS;
			FS.mkdirTree(this.homePath);
			// A program that is only passing through leaves nothing behind: the
			// home directory is there for it to write into, and it goes with the
			// tab.
			if (this.options.persist === false) {
				return;
			}
			this.output("Synchronizing filesystem with IndexedDB…");
			FS.mount(this.module.IDBFS, {}, this.homePath);
			await new Promise(resolve => FS.syncfs(true, error => {
				if (error) {
					instance.output(`Failed to synchronize filesystem with IndexedDB: ${error}`, { error: true, bold: true });
				}
				resolve();
			}));
		}

		onExit() {
			this.exited = true;
			this.reportFinished();
			if (this.options.persist !== false) {
				this.syncPersistentStorage();
			}
			if (this.canvas != null) {
				// After the program quits, hide the canvas and reset the cursor, as
				// the canvas will be entirely black, also blocking the view of
				// whatever is behind it.
				this.canvas.style.display = "none";
				// Make sure to reset cursor because it sometimes does not become
				// visible.
				this.canvas.style.cursor = "default";
				// Also reset cursor of body because the cursor sometimes does not
				// become visible until being moved.
				document.body.style.cursor = "default";
			}
			this.output(`${this.options.programName || "The program"} closed.`, { bold: true });
			this.say("exit", {});
			if (this.options.onExit) {
				this.options.onExit();
			}
		}

		syncPersistentStorage() {
			if (this.module['ddnetSyncPersistentStorage'] !== undefined) {
				this.module['ddnetSyncPersistentStorage'](true);
			} else {
				this.module.FS.syncfs(error => {
					if (error) {
						this.output(`Failed to synchronize filesystem with IndexedDB: ${error}`, { error: true, bold: true });
					}
				});
			}
		}
	}

	// The furniture our own pages share, on top of `start`. Every piece of it
	// is optional, and a page that leaves one out is not showing it: a page
	// with no `output` keeps its log in the browser's console, where a log
	// belongs, and says only what went wrong - in its `error` line if it has
	// one. What is left then is the program on the canvas and nothing else,
	// which is what a viewer should look like while it starts.
	//
	// A page that does have an `output` collects the log there without showing
	// it. Starting is not something to watch somebody else do: what a page
	// shows while it starts is the program, and the log is what it shows once
	// there is no program left to show - when it could not start, when it
	// stopped, or when a page asks for it itself.
	function pageOptions(options) {
		const elements = options.elements;
		const showError = message => {
			// Something went wrong, so nothing is coming: a hint that says it is
			// still on its way would be the page's second answer to the same
			// question, and the wrong one.
			if (elements.loading) {
				elements.loading.hidden = true;
			}
			if (elements.error) {
				elements.error.textContent = message;
				elements.error.style.display = "block";
			}
		};
		const showLog = () => {
			if (elements.output) {
				elements.output.style.display = "flex";
			}
			if (elements.loading) {
				elements.loading.hidden = true;
			}
		};
		return Object.assign({}, options, {
			canvas: elements.canvas,
			onProgress: text => {
				if (elements.loading) {
					elements.loading.textContent = text;
				}
			},
			onOutput: (message, kind) => {
				if (!elements.output) {
					if (kind.error) {
						console.error(message);
						showError(message);
					} else {
						console.log(message);
					}
					return;
				}
				const span = document.createElement("span");
				span.textContent = message;
				span.className = "line";
				if (kind.error) {
					span.style.color = "red";
				} else if (kind.color) {
					span.style.borderColor = kind.color;
				}
				if (kind.bold) {
					span.style.fontWeight = "bold";
				}
				if (kind.fatal) {
					showLog();
				}
				elements.outputContent.appendChild(span);
				elements.outputContent.appendChild(document.createElement("br"));
				elements.outputContent.scrollTop = elements.outputContent.scrollHeight;
			},
			onExit: () => {
				showLog();
				if (elements.outputContent) {
					const restartButton = document.createElement("button");
					restartButton.textContent = "Reload page";
					restartButton.style.marginTop = "10px";
					restartButton.addEventListener('click', e => location.reload());
					elements.outputContent.appendChild(restartButton);
					elements.outputContent.scrollTop = elements.outputContent.scrollHeight;
				} else {
					// The canvas is gone with the program, so without this the
					// page would be empty and say nothing about why.
					showError("This has stopped. Reload the page to start it again.");
				}
				if (options.onExit) {
					options.onExit();
				}
			},
		});
	}

	// One line to put a viewer on somebody else's page:
	//
	//     <ddnet-demo src="https://…/x.demo" controls></ddnet-demo>
	//     <ddnet-map src="https://…/x.map" controls></ddnet-map>
	//
	// The picture lives in a shadow root, so nothing here shares a name with
	// anything on the page around it - no `#canvas` to collide with, and no
	// stylesheet of ours landing on their buttons. What may be styled from
	// outside is named: `::part(picture)` and `::part(message)`.
	//
	// The controls are the ones the viewer draws for itself. A page of our own
	// puts real buttons beside the canvas, which is better where the page is
	// ours to write; in somebody else's page the drawn bar is what travels.
	// How long the view attributes wait for the file they belong to. Abyss,
	// the largest map anybody has, takes a few seconds to unpack.
	const WAIT_FOR_FILE_MS = 60000;
	// How often the demo element asks what changed, to say it in the words a
	// `<video>` says them in. A browser fires `timeupdate` between four and
	// sixty times a second and says so in the standard; this is at the slow
	// end of that, which is as often as a page can draw a time readout with
	// anybody noticing.
	const VIDEO_EVENT_INTERVAL_MS = 200;
	// A demo stops on its last frame, and the last frame is not exactly the
	// end: how near the end counts as being at it.
	const END_OF_DEMO = 0.999;

	// A viewer is black until it has something to draw, and fetching a demo or
	// a map over a slow line takes long enough for that to look like a page
	// that is broken rather than one that is busy. So something says so until
	// there is a picture - and stops saying it whichever way the wait ends,
	// including the one where the file never turns up and the error takes over.
	function loadingHint(element, isThere, options) {
		const settings = Object.assign({ text: "Loading\u2026", until: WAIT_FOR_FILE_MS }, options || {});
		const signal = settings.signal;
		const deadline = Date.now() + settings.until;
		let stopped = false;
		const hide = () => {
			if (stopped) {
				return;
			}
			stopped = true;
			element.hidden = true;
		};
		if (settings.text !== null) {
			element.textContent = settings.text;
		}
		element.hidden = false;
		const look = () => {
			if (stopped || (signal !== undefined && signal !== null && signal.aborted)) {
				hide();
				return;
			}
			let there = false;
			try {
				there = isThere() === true;
			} catch (error) {
				there = true;
			}
			if (there || Date.now() >= deadline) {
				hide();
				return;
			}
			setTimeout(look, 100);
		};
		look();
		return { stop: hide };
	}

	const ELEMENT_STYLE = `
:host { display: block; position: relative; contain: content; background: #000; }
:host([hidden]) { display: none; }
canvas { display: block; width: 100%; height: 100%; background: #000; touch-action: none; }
/* Where a page puts a bar of its own: over the picture, and nowhere in the
   way of it. The overlay itself takes no pointer, so a tap that misses the
   bar is a tap on the picture; what the page put in it takes them as
   usual. */
.over { position: absolute; inset: 0; pointer-events: none; }
.over::slotted(*) { pointer-events: auto; }
.message {
	position: absolute;
	inset: 0;
	display: flex;
	align-items: center;
	justify-content: center;
	margin: 0;
	padding: 12px;
	background-color: rgba(0, 0, 0, 0.65);
	color: #f4f4f5;
	font: 13px/1.45 system-ui, sans-serif;
	text-align: center;
}
/* A rule of our own that sets display beats the browser's own rule for the
   hidden attribute, so without this the message would stay where it was put
   and the loading hint would never go away. */
.message[hidden] { display: none; }
`;

	// A worker has no DOM, and `HTMLElement` is not a name there at all - a
	// class cannot even be declared from one that does not exist. The render
	// worker imports this module for `start`, so the elements are declared from
	// a stand-in there and never made: defining one needs `customElements`,
	// which a worker has not got either.
	const ELEMENT_BASE = typeof HTMLElement === "undefined" ? class {} : HTMLElement;

	class CViewerElement extends ELEMENT_BASE {
		/**
		 * The bar of controls the package builds for this viewer, for
		 * `controls="html"`. A class taking `(viewer, options)` with a
		 * `destroy`; `@ddnet/demo-player` has one, `@ddnet/map-viewer` has
		 * one, and an element without one only ever draws the program's own.
		 */
		static viewerBar = null;

		constructor() {
			super();
			const root = this.attachShadow({ mode: "open" });
			const style = document.createElement("style");
			style.textContent = ELEMENT_STYLE;
			this.viewerCanvas = document.createElement("canvas");
			this.viewerCanvas.setAttribute("part", "picture");
			this.viewerMessage = document.createElement("p");
			this.viewerMessage.className = "message";
			this.viewerMessage.setAttribute("part", "message");
			this.viewerMessage.hidden = true;
			// A page that wants its own controls over the picture puts them in
			// the element and says which slot: `<div slot="controls">`. Below
			// the message, because a message is what to read when there is one.
			this.viewerSlot = document.createElement("slot");
			this.viewerSlot.name = "controls";
			this.viewerSlot.className = "over";
			root.append(style, this.viewerCanvas, this.viewerSlot, this.viewerMessage);
			this.viewerInstance = null;
			this.viewerControls = null;
			// The bar of real buttons, where the element was asked for one.
			this.viewerBarInstance = null;
			this.viewerStopping = null;
			// What says that something is on its way, for as long as it is, see
			// `loadingHint`.
			this.viewerHint = null;
			// Which file the view attributes are waiting for. A second file
			// asked for while the first is still on its way leaves the first
			// wait behind, and it has to know that it is no longer the one.
			this.viewerGeneration = 0;
			// Which attributes the program was started with, so that they are
			// not asked for a second time once it runs. See `startViewer`.
			this.viewerHandedOver = [];
			// A promise for the running program, so that a page can wait for it
			// and hear about anything that stopped it from starting.
			this.ready = null;
		}

		/** The controls of the viewer, once it runs, and `null` before that. */
		get controls() {
			return this.viewerControls;
		}

		/**
		 * The bar of buttons, where `controls="html"` asked for one, and
		 * `null` otherwise. Its parts carry `data-role`, so a page that wants
		 * at one of them asks this element rather than the document - which is
		 * what makes two viewers on one page possible.
		 */
		get bar() {
			return this.viewerBarInstance;
		}

		/**
		 * Which bar was asked for. `controls` is the one the program draws
		 * into the picture, `controls="html"` the one the package builds out
		 * of the browser's own buttons, and neither is the default.
		 */
		wantsProgramControls() {
			const asked = this.getAttribute("controls");
			return asked !== null && asked !== "html";
		}

		wantsHtmlControls() {
			return this.getAttribute("controls") === "html" && this.constructor.viewerBar !== null;
		}

		// Built once the program runs, taken away again when nobody asks for
		// it any more. The bar is the package's to fill; what this knows is
		// where it goes and what it steers.
		applyControlsKind() {
			const wanted = this.wantsHtmlControls();
			if (wanted === (this.viewerBarInstance !== null)) {
				return;
			}
			if (!wanted) {
				this.viewerBarInstance.destroy();
				this.viewerBarInstance = null;
				return;
			}
			this.viewerBarInstance = new this.constructor.viewerBar(this.viewerControls, {
				container: this,
				picture: this,
				slot: "controls",
				signal: this.viewerStopping.signal,
			});
		}

		/**
		 * What is drawn on, for a page that has something to say about it -
		 * how big a picture of it would be, or where a pointer is. It lives in
		 * the shadow root, and styling it from outside is `::part(picture)`.
		 */
		get picture() {
			return this.viewerCanvas;
		}

		/** What is being shown, as a file rather than as a name to fetch. */
		async load(file) {
			const instance = await this.ready;
			const loading = instance.loadFile(file);
			this.applyViewWhenLoaded();
			return await loading;
		}

		say(message) {
			// The hint writes in the same place, so it has had its turn once
			// there is something else to say - it must not take the message
			// away again when the file it was waiting for finally turns up.
			if (this.viewerHint !== null) {
				this.viewerHint.stop();
				this.viewerHint = null;
			}
			this.viewerMessage.textContent = message || "";
			this.viewerMessage.hidden = !message;
			// A page that put a viewer somewhere is not looking at it, so what
			// went wrong is said to it as well as written on the picture. The
			// same name a `<video>` says it under.
			if (message) {
				this.dispatchEvent(new CustomEvent("error", { detail: { message: message } }));
			}
		}

		connectedCallback() {
			// Moving an element within a page takes it out and puts it back,
			// and a program is too dear to throw away for that: this is only a
			// start if there is nothing running already.
			if (this.ready !== null) {
				return;
			}
			this.viewerStopping = new AbortController();
			// Said from here rather than from `startViewer`, because fetching
			// the program is itself most of the wait. An element with nothing
			// to show is not waiting for anything and says nothing.
			const source = this.getAttribute("src");
			if (source !== null && source !== "") {
				const kind = this.constructor.viewerKind;
				this.viewerHint = loadingHint(
					this.viewerMessage,
					() => this.viewerControls !== null && kind.loaded(this.viewerControls),
					{ signal: this.viewerStopping.signal });
			}
			this.ready = this.startViewer();
			this.ready.catch(error => {
				if (this.viewerStopping !== null && !this.viewerStopping.signal.aborted) {
					this.say((error && error.message) || String(error));
				}
			});
		}

		disconnectedCallback() {
			const instance = this.viewerInstance;
			const stopping = this.viewerStopping;
			if (this.viewerBarInstance !== null) {
				this.viewerBarInstance.destroy();
				this.viewerBarInstance = null;
			}
			this.viewerInstance = null;
			this.viewerControls = null;
			this.viewerStopping = null;
			this.ready = null;
			if (this.viewerHint !== null) {
				this.viewerHint.stop();
				this.viewerHint = null;
			}
			if (stopping !== null) {
				stopping.abort();
			}
			if (instance !== null) {
				instance.destroy();
			}
		}

		attributeChangedCallback(name, before, value) {
			if (before === value || this.viewerControls === null) {
				return;
			}
			this.applyAttribute(name, value);
		}

		async startViewer() {
			const kind = this.constructor.viewerKind;
			// What the program is started with rather than asked for
			// afterwards, and which attributes those were: asking again once it
			// runs would seek a demo that is already where it belongs, and a
			// seek asked for from outside while the program is still fetching
			// what it needs is one wait inside another.
			const handedOver = kind.startOptions === undefined ? {} : kind.startOptions(this);
			this.viewerHandedOver = Object.keys(handedOver).length === 0 ? [] : (kind.startAttributes || []);
			// Which program this is and where it lies is the class's to know
			// (`Program.open`). A page that keeps the programs somewhere else
			// than beside the module says so with `base`, and then the class
			// is asked for one that looks there instead.
			const program = this.constructor.viewerProgram;
			const moved = this.getAttribute("base");
			const opened = moved === null ? program : class extends program {
				static base = new URL(moved, location.href).href;
			};
			const source = this.getAttribute("src");
			const instance = await opened.open({
				canvas: this.viewerCanvas,
				// Drawn by the viewer itself unless this says otherwise, and
				// left off entirely where the page brings its own - either a
				// bar of the package's (`controls="html"`) or one the page
				// wrote itself.
				controls: this.wantsProgramControls(),
				// The demo viewer zooms what it shows when the wheel is turned
				// over it, which a page that scrolls around it does not want.
				// The map viewer is not asked: there, zooming is the point.
				zoom: kind.zoomable && this.hasAttribute("nozoom") ? false : undefined,
				// Filling the screen leaves a phone where it is unless this
				// says which way to turn it, see `wantedOrientation`.
				orientation: this.getAttribute("orientation") || undefined,
				// Where in the demo to start, said before it plays anything
				// rather than once the element gets a turn: by then the demo
				// would have played the seconds that took. The same attributes
				// are applied again afterwards, and again whenever one of them
				// changes, which is what makes a viewer that is already running
				// follow them.
				...handedOver,
				// A viewer on somebody else's page keeps nothing: it has no
				// settings to save and no recording to make, and an element
				// that quietly filled a visitor's IndexedDB would be a
				// surprise in somebody else's page.
				persist: false,
				file: source === null || source === "" ? undefined : source,
				dataBase: this.getAttribute("data") || undefined,
				signal: this.viewerStopping.signal,
				// A page that embeds a viewer did not ask for its address to be
				// read, and two viewers on one page could not both have it.
				urlParams: [],
				onOutput: (message, options) => {
					if (options.error) {
						this.say(message);
					}
				},
			});
			this.viewerInstance = instance;
			// A program answers for itself: what a viewer can be asked are
			// its own methods, not a second object made from it.
			this.viewerControls = instance;
			// Now there is something for a bar to steer.
			this.applyControlsKind();
			// Now that there is something to watch, for whoever watches it.
			this.viewerRunning();
			// The canvas is a box on somebody's page here, not the window, so
			// nothing would tell the program when it changes shape.
			followSize(this.viewerCanvas, this.viewerControls, { signal: this.viewerStopping.signal });
			// What the element was told to show while the program was still on
			// its way. A page that reads its own address and then says what it
			// found does exactly that, and until the program runs there is
			// nobody to say it to - so it is asked for again here rather than
			// remembered somewhere.
			const asked = this.getAttribute("src");
			if (asked !== source && asked !== null && asked !== "") {
				this.applyAttribute("src", asked);
				return instance;
			}
			this.applyViewWhenLoaded();
			return instance;
		}

		// What the element was asked for beyond the file itself, applied once
		// the file is there. A viewer fits the whole map, or puts a demo at its
		// beginning, at the moment it has one - which is after the element was
		// told what to show, so anything applied before that would be undone
		// again by it.
		applyViewWhenLoaded() {
			const kind = this.constructor.viewerKind;
			const signal = this.viewerStopping === null ? null : this.viewerStopping.signal;
			const generation = ++this.viewerGeneration;
			const until = Date.now() + WAIT_FOR_FILE_MS;
			const apply = () => {
				if (signal === null || signal.aborted || generation !== this.viewerGeneration || this.viewerControls === null) {
					return;
				}
				if (!kind.loaded(this.viewerControls)) {
					// A file that never arrives has said so through `onOutput`
					// by now, so the wait ends rather than going on for as long
					// as the page is open.
					if (Date.now() < until) {
						setTimeout(apply, 100);
					}
					return;
				}
				// Only the first time round: a file that replaces the first one
				// is nobody's to have positioned, so then everything the
				// element says is applied again.
				const handedOver = this.viewerHandedOver;
				this.viewerHandedOver = [];
				for (const name of this.constructor.observedAttributes) {
					if (name !== "src" && !handedOver.includes(name) && this.hasAttribute(name)) {
						this.applyAttribute(name, this.getAttribute(name));
					}
				}
			};
			apply();
		}

		/**
		 * Called once the program runs and its controls are there. Nothing
		 * happens here; a viewer that says what it is doing says it from here.
		 */
		viewerRunning() {}

		applyAttribute(name, value) {
			if (name === "src") {
				this.say("");
				if (value !== null && value !== "") {
					// No hint for a file that replaces another: a viewer that
					// already has one still says it has one while the next is on
					// its way, so there is nothing here to wait for that could
					// be told apart from being done. The first one is a wait,
					// and one that nobody has said anything about yet.
					const kind = this.constructor.viewerKind;
					if (this.viewerHint === null && !kind.loaded(this.viewerControls)) {
						this.viewerHint = loadingHint(
							this.viewerMessage,
							() => this.viewerControls !== null && kind.loaded(this.viewerControls),
							{ signal: this.viewerStopping.signal });
					}
					this.viewerInstance.loadUrl(value).catch(error => this.say((error && error.message) || String(error)));
					// The new file brings its own view with it, so what the
					// element asks for has to be put back on top of it again.
					this.applyViewWhenLoaded();
				}
				return;
			}
			if (name === "controls") {
				this.viewerControls.controls(this.wantsProgramControls());
				this.applyControlsKind();
				return;
			}
			this.constructor.viewerKind.apply(this.viewerControls, name, value, this);
		}
	}

	return {
		/**
		 * This library's own version, which says what its API looks like. Not
		 * the game's version: the two move for different reasons.
		 */
		version: VERSION,

		/**
		 * A program running on a page: the canvas, the files, what it writes,
		 * and the way to call into it.
		 *
		 * This is the base the packages beside this one are built on - a demo
		 * player is this class with the demo player's script, its factory and
		 * everything a demo can be asked. A page that has a program of its own
		 * opens this one directly and hands it the factory, which is what the
		 * full client does.
		 */
		Program: CProgram,

		/**
		 * What this library throws and rejects with. Every one of them carries
		 * a `code` beside its sentence, and the code is what to branch on:
		 *
		 * * `BadOption` - an option this does not take, or one of the wrong
		 *   shape.
		 * * `CrossOriginRefused` - the page is not cross-origin isolated, so
		 *   the browser withholds the shared memory every program here needs.
		 * * `NoWebGpu` - a render without a window was asked for and there is
		 *   no WebGPU, or no adapter the browser will use.
		 * * `FileRefused` - the file is not a kind the program takes.
		 * * `FileTooLarge` - a file named in a URL is bigger than this will
		 *   fetch into memory.
		 * * `FetchFailed` - something the program needed answered with an
		 *   error.
		 *
		 * A package throws this same class, so these four are
		 * `@ddnet/demo-renderer`'s:
		 *
		 * * `NoVideoEncoder` - this browser cannot encode video.
		 * * `RenderStopped` - a render was stopped on purpose.
		 * * `RenderFailed` - a render ended without a video.
		 * * `ZipTooLarge` - more than fits in one zip file was put into one.
		 */
		Error: DDNetBaseError,

		/**
		 * What this browser cannot do, in one sentence, or `null` when it can
		 * do all of it. A page can say so itself before it offers something
		 * that will not work. Answers with a promise, because asking a browser
		 * for a graphics adapter is a question it takes a moment over.
		 *
		 * @param needsWebGpu Whether what is offered draws without a window,
		 * which is the one thing that needs WebGPU.
		 */
		supportProblem(needsWebGpu) {
			return supportProblem(needsWebGpu === true);
		},

		/**
		 * The same question, answered with the error itself rather than with
		 * its sentence, or `null` where there is no problem. For a package
		 * that checks before it starts anything and then throws what it found,
		 * so that whoever catches it reads the same `code` as from a program
		 * that refused to start.
		 */
		supportError(needsWebGpu) {
			return supportError(needsWebGpu === true);
		},

		/**
		 * Makes a button the one that fills the screen with the page, and
		 * keeps what is written on it right. A browser that does not allow it
		 * leaves the button hidden.
		 *
		 * What goes full screen is the whole page rather than the canvas, so
		 * that the controls beside it are still there.
		 *
		 * @param button The button to wire up.
		 * @param options.element What to fill the screen with, the page
		 * itself otherwise.
		 * @param options.signal Takes the wiring off again, for a page that
		 * puts the button up and takes it down.
		 */
		fullscreen(button, options) {
			return fullscreen(button, options);
		},

		/**
		 * Keeps a viewer the size of the box it sits in.
		 *
		 * A viewer that fills the window follows it without being asked. One
		 * in a box of a page's own does not, because nothing tells a window
		 * that a box beside it changed shape: this watches the box and hands
		 * the size to the program.
		 *
		 * @param element The box to follow, usually the canvas itself.
		 * @param controls What `demoControls` or `mapControls` answered with.
		 * @param options.signal An `AbortSignal` that stops the watching.
		 * @returns `{stop}`, which also stops it.
		 */
		followSize(element, controls, options) {
			return followSize(element, controls, options);
		},

		/**
		 * Shows an element while a viewer has nothing to draw yet, and takes it
		 * away as soon as it has. `isThere` is asked every tenth of a second,
		 * and the wait gives up after `until` milliseconds - a file that never
		 * arrives has said so through the error by then.
		 */
		loadingHint(element, isThere, options) {
			return loadingHint(element, isThere, options);
		},

		/**
		 * The program's script as a module, which a plain `import` cannot do
		 * with it: it is a classic script that names itself. Answers the
		 * factory that names it, and remembers it, so asking twice for the same
		 * one costs nothing.
		 *
		 * @param scriptUrl Where the program's `.js` is.
		 * @param moduleName The name it gives itself, `DDNetDemoPlayer` and so
		 * on.
		 */
		importProgram(scriptUrl, moduleName) {
			return importProgram(scriptUrl, moduleName);
		},

		/**
		 * Where this module itself is. A worker sees no import map, so a
		 * package that starts one has to hand it the address of the base
		 * rather than its name.
		 */
		moduleUrl: MODULE_URL,

		/**
		 * A script from another origin, as a blob of this page's own. A page
		 * that is cross-origin isolated refuses a foreign script unless its
		 * server allows it by name; fetched and handed over as a blob it is
		 * this page's, and a worker may import it.
		 *
		 * @param url Where the script is.
		 * @return A promise of the script as a `Blob`. `URL.createObjectURL`
		 * makes it into something a worker can import, and whoever made one
		 * lets go of it again.
		 */
		fetchScript(url) {
			return fetchScript(new URL(url, self.location.href));
		},

		/**
		 * Complains about an option nobody reads, which is the kind of mistake
		 * that otherwise turns up much later and in the words of whatever went
		 * without it. For a package that has ways in of its own: this one
		 * knows the shape of every option any program here takes.
		 *
		 * @param where What to call the way in, `DemoPlayer.open` and so on.
		 * @param options What was said.
		 * @param allowed The names it takes.
		 */
		checkOptions(where, options, allowed) {
			return checkOptions(where, options, allowed);
		},

		/**
		 * What something stopped by a signal ends with: whatever the signal
		 * carries as its reason, or a `Stopped` of ours. So that a package
		 * that stops work of its own stops it in the same words a program
		 * does.
		 */
		abortError(signal) {
			return abortError(signal);
		},

		/**
		 * Throws away the videos of earlier visits that nobody took. Done once
		 * per page as a program starts, and only to be asked for by a package
		 * that writes a video without starting a program here - a render in a
		 * worker, where every worker is a page as far as this script is
		 * concerned and the second render of a batch would otherwise sweep
		 * away the video of the first.
		 */
		sweepVideoScratch() {
			return sweepVideoScratch();
		},

		/**
		 * The element a viewer package builds its own on: a canvas in a shadow
		 * root, a message over it, and a program started on it as soon as the
		 * element is in a page. What it shows and what it answers to is the
		 * `viewerKind` the package gives it.
		 */
		ViewerElement: CViewerElement,

		/**
		 * How long the element waits for a file before it gives up saying that
		 * it is on its way, in milliseconds. A package that waits for the same
		 * file waits as long.
		 */
		waitForFile: WAIT_FOR_FILE_MS,

		/**
		 * Pictures a package draws on its own buttons, added to the ones every
		 * page here can ask for by name. A package says this as it loads, and
		 * a page that imported it can then write `data-icon="play"` in its
		 * markup and get one.
		 *
		 * @param pictures `{name: "<svg contents>"}`, drawn in a 24 by 24 box
		 * and in `currentColor`.
		 */
		addIcons(pictures) {
			addIcons(pictures);
		},

		/**
		 * One of the pictures the viewers draw on their own buttons, as an
		 * `<svg>` element to put on a button of the page's own. The names are
		 * the ones `CViewerControls::EIcon` uses, in lower case: `menu`,
		 * `detail`, `entities`, `play`, `pause`, `restart`, `minus`, `plus`,
		 * `fit`, `save`, `save_all`, `stop`, `eye`, `freeview` and
		 * `fullscreen`.
		 */
		icon(name) {
			return icon(name);
		},

		/**
		 * Draws the picture every `data-icon` element under `root` asks for,
		 * so that markup can name what is on a button where it says what the
		 * button is. Calling it again only draws what has changed, which is
		 * how a button swaps its picture - `data-icon` is set and this is
		 * called.
		 *
		 * @param root Where to look, the whole document otherwise.
		 */
		paintIcons(root) {
			return paintIcons(root);
		},

		/**
		 * Whether this browser allows anything to fill the screen at all.
		 * Called from the viewers, for the button they draw themselves.
		 */
		fullscreenSupported() {
			return fullscreenSupported();
		},

		/** Whether something is filling the screen right now. */
		isFullscreen() {
			return isFullscreen();
		},

		/**
		 * Fills the screen with the page, or stops doing so. A browser only
		 * allows this out of something the user did, so it has to be called
		 * while what they did still counts - straight out of the click, or in
		 * the frame that reads it.
		 *
		 * @param options.element What to fill the screen with, the page
		 * itself otherwise.
		 */
		toggleFullscreen(options) {
			return toggleFullscreen(options);
		},

		/**
		 * Lets the controls over a picture fade out while nothing is happening
		 * and come back when something does, the way a video player's do. The
		 * elements are given the `faded` class, which is what the page's own
		 * stylesheet makes of it.
		 *
		 * @param elements The element, or the elements, that belong together.
		 * @param options.delay How long to wait before they go, in
		 * milliseconds.
		 * @param options.picture What they are drawn over, usually the canvas.
		 * A tap on it shows them or takes them away, which is what a tap on a
		 * video does everywhere.
		 * @param options.onHide Called whenever they go, so that a page can
		 * close what one of them had opened.
		 * @param options.signal Takes all of it off the page again, for a page
		 * that puts the controls up and takes them down.
		 */
		autoHide(elements, options) {
			return autoHide(elements, options);
		},

		/**
		 * Fills an element with the settings a video export takes - size,
		 * frame rate, quality, encoder, sound, interface and chat - and
		 * answers with `values()`, which reads them back in the form `render`
		 * and `startExport` take, `setValues()`, which puts them there, and
		 * `onChange()`, which says when any of them was changed.
		 *
		 * The sizes and frame rates on offer are the ones the client offers,
		 * with `Custom` for anything else.
		 *
		 * @param container The element the fields go into.
		 * @param options.canvas A canvas whose size is offered as well, and
		 * then chosen to begin with.
		 * @param options.audio Whether sound starts out switched on.
		 */
		exportSettingsForm(container, options) {
			return exportSettingsForm(container, options);
		},

		/**
		 * The video encoders this browser can be asked for, as
		 * `{name, display}`, most capable first and empty where it cannot
		 * encode at all. `name` is what `render` and `startExport` take as
		 * their `codec`; `display` is what to write in a menu.
		 *
		 * Asked once and remembered, because the answer cannot change while the
		 * page is open.
		 */
		videoCodecs() {
			return videoCodecs();
		},

		/**
		 * What the page's URL says about a parameter, from the fragment first:
		 * a fragment never reaches a server, so a link to somebody's demo stays
		 * between them and their browser.
		 */
		urlParameter(name) {
			return urlParameter(name);
		},

		/**
		 * Writes parameters into the page's fragment, so that the address bar
		 * says what is being looked at and a copied link brings somebody else
		 * to the same place. Everything the fragment already said that is not
		 * named here stays; a value of `null` takes its parameter out.
		 *
		 * Replaces the history entry rather than adding one, so the back button
		 * still leads to the page before this one.
		 */
		setUrlParameters(values) {
			setUrlParameters(values);
		},

	};
})();

// Both forms of the same thing: everything at once for whoever wants to write
// `DDNetBase.start(…)`, and one name at a time for whoever would rather
// import only what they use. The names are the object's own, so there is one
// list and not two.
export default DDNetBase;
export const {
	// tidy-alphabetical-start
	abortError, addIcons, autoHide, checkOptions, exportSettingsForm,
	fetchScript, followSize, fullscreen, fullscreenSupported, icon,
	importProgram, isFullscreen, loadingHint, moduleUrl, paintIcons, Program,
	setUrlParameters, supportError, supportProblem, sweepVideoScratch,
	toggleFullscreen, urlParameter, version, videoCodecs, ViewerElement,
	waitForFile,
	// tidy-alphabetical-end
} = DDNetBase;
// Not as `Error`: a name at the top of a module is a name for the whole of it,
// and this one is the browser's own further up - where the class above is
// declared from it.
export const DDNetBaseError = DDNetBase.Error;
