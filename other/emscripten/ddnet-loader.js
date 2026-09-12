// Starting one of the programs compiled for the browser, with the files it is
// given and, where it has something to show, on a canvas.
//
// Three ways in. `DDNetLoader.start` puts one instance on one canvas and hands
// back a handle to call into it - that is all an embedding page needs, and it
// claims no globals, so a page may have two of them or a program of its own
// beside them. `DDNetLoader.page` is that plus the furniture our own three
// pages share: a loading line, a console log, and the canvas filling the
// window. `DDNetLoader.render` is the same again with nothing on screen at all:
// a demo goes in, an MP4 comes out, and the page decides what to do with it.
// They differ in the program they start and in what they take, not in how any
// of it works.
//
// A module. `import DDNetLoader from "./ddnet-loader.js"` for all of it, or
// `import { start, page } from …` for one thing at a time; a page loads it with
// `<script type="module">`, which is what its own three pages do. A module is
// also the only form a package can honestly offer: the alternative would be a
// global claimed by a script, which is neither importable nor two things at
// once - and there is no way back from a module to a global that a plain
// `<script>` could wait for.
//
// Modules are strict by themselves, so nothing here says so.

const DDNetLoader = (() => {
	// What this library answers with when it refuses or cannot do something.
	// The `code` is what a caller branches on: the sentence is for whoever
	// reads it and may be reworded, the code is part of the API and is not.
	class DDNetLoaderError extends Error {
		constructor(code, message) {
			super(message);
			this.name = "DDNetLoaderError";
			this.code = code;
		}
	}

	const fail = (code, message) => new DDNetLoaderError(code, message);

	// This library's own version, which is not the game's: it says what the
	// API looks like, so it changes when the API does.
	const VERSION = "1.0.0";

	// What each way in takes, and of what shape. An option nobody reads is the
	// kind of mistake that otherwise turns up much later and in the words of
	// whatever went without it - a mistyped `fps` is first heard of from the
	// encoder, saying something about a frame rate of sixty.
	const START_OPTIONS = [
		"accept", "acceptLinks", "arguments", "canvas", "controls", "dataBase", "file", "fileArgument",
		"fileName", "homePath", "module", "needsWebGpu", "onExit", "onOutput", "onProgress", "persist",
		"programName", "scriptUrl", "signal", "sweepVideoScratch", "urlParams", "videoSink",
	];
	const PAGE_OPTIONS = START_OPTIONS.concat(["elements"]);
	// A render takes what the command line of the render tool takes, plus the
	// few things every program here takes. `sweepVideoScratch` and `arguments`
	// are in it because the worker a render runs in calls back in through the
	// same door, with those two already settled.
	const RENDER_OPTIONS = [
		"arguments", "audio", "chat", "codec", "crf", "dataBase", "demo", "follow", "fps", "height",
		"homePath", "hud", "module", "moduleName", "name", "onOutput", "onProgress", "onRenderProgress",
		"onStart", "output", "preset", "programName", "scriptUrl", "settings", "signal",
		"sweepVideoScratch", "videoSink", "width", "worker",
	];
	const OPTION_SHAPES = {
		accept: "array", acceptLinks: "boolean", arguments: "array", audio: "boolean", canvas: "canvas",
		chat: "boolean", codec: "string", controls: "boolean", crf: "number", dataBase: "string",
		elements: "object", fileArgument: "string", fileName: "string", follow: "string", fps: "number",
		height: "number", homePath: "string", hud: "boolean", module: "function", moduleName: "string",
		name: "string", needsWebGpu: "boolean", onExit: "function", onOutput: "function",
		onProgress: "function", onRenderProgress: "function", onStart: "function", output: "string",
		persist: "boolean", preset: "string", programName: "string", scriptUrl: "string", settings: "array",
		signal: "signal", sweepVideoScratch: "boolean", urlParams: "array", width: "number",
		worker: "boolean",
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

	function checkOptions(where, options, allowed) {
		if (typeof options !== "object" || options === null) {
			throw fail("BadOption", `DDNetLoader.${where} takes an object of options`);
		}
		for (const [name, value] of Object.entries(options)) {
			if (!allowed.includes(name)) {
				throw fail("BadOption", `DDNetLoader.${where} does not take '${name}'. It takes: ${allowed.join(", ")}.`);
			}
			// An option left out is an option left at its default, so only
			// what is actually there is looked at.
			const shape = OPTION_SHAPES[name];
			if (shape !== undefined && value !== undefined && value !== null && !hasShape(value, shape)) {
				throw fail("BadOption", `DDNetLoader.${where} wants ${SHAPE_NAMES[shape]} for '${name}'`);
			}
		}
	}

	const DEFAULT_HOME_PATH = "/home/web_user/.local/share/ddnet";
	// Where this module is, so that a worker can be given the same one. A
	// module knows this of itself, wherever it is running - which a script had
	// to be asked for while it ran, and could only answer on a page.
	const LOADER_URL = import.meta.url;
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
	// The other end of it is `DemoViewerSetSize` in
	// `src/engine/client/demo_viewer_client.cpp` and `MapViewerSetSize` in
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

	// The same pictures the viewers draw on their own buttons, as the browser
	// draws pictures: one square outline each, in whatever colour the button
	// they sit on is written in. They are named after `CViewerControls::EIcon`
	// in `src/engine/client/viewer_controls.h` and drawn to say the same
	// thing, so that a page and the program behind it do not offer the same
	// button with two different pictures on it.
	const ICONS = {
		menu: '<rect x="3" y="5" width="18" height="2.6" rx="1.3"/><rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/><rect x="3" y="16.4" width="18" height="2.6" rx="1.3"/>',
		detail: '<path d="M12 1.5 13.9 9.1 21.5 11 13.9 12.9 12 20.5 10.1 12.9 2.5 11 10.1 9.1Z"/>',
		entities: '<rect x="3" y="3" width="8" height="8" rx="1.6"/><rect x="13" y="3" width="8" height="8" rx="1.6"/><rect x="3" y="13" width="8" height="8" rx="1.6"/><rect x="13" y="13" width="8" height="8" rx="1.6"/>',
		play: '<path d="M7.5 3.8 20.5 12 7.5 20.2Z"/>',
		pause: '<rect x="5.5" y="3.5" width="4.4" height="17" rx="2.2"/><rect x="14.1" y="3.5" width="4.4" height="17" rx="2.2"/>',
		restart: '<rect x="3.5" y="3.5" width="3.2" height="17" rx="1.6"/><path d="M20.5 3.8 20.5 20.2 8.4 12Z"/>',
		minus: '<rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/>',
		plus: '<rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/><rect x="10.7" y="3" width="2.6" height="18" rx="1.3"/>',
		fit: '<rect x="2.7" y="4.7" width="18.6" height="14.6" rx="2" fill="none" stroke="currentColor" stroke-width="2.4"/><rect x="6.6" y="8.6" width="10.8" height="6.8" opacity="0.55"/>',
		save: '<path d="M12 2.8V12" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/><path d="M8 8.8 12 13 16 8.8" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/><path d="M3.8 15.4v3.4a1.4 1.4 0 0 0 1.4 1.4h13.6a1.4 1.4 0 0 0 1.4-1.4v-3.4" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>',
		save_all: '<path d="M7 2.8V9.6M4.2 7 7 10 9.8 7M1.6 13.2v3a1.4 1.4 0 0 0 1.4 1.4h8a1.4 1.4 0 0 0 1.4-1.4v-3M17 6.4V13.2M14.2 10.6 17 13.6 19.8 10.6M11.6 16.8v3a1.4 1.4 0 0 0 1.4 1.4h8a1.4 1.4 0 0 0 1.4-1.4v-3" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>',
		stop: '<rect x="4.5" y="4.5" width="15" height="15" rx="3"/>',
		eye: '<path d="M12 4.6C5.6 4.6 1.8 12 1.8 12s3.8 7.4 10.2 7.4S22.2 12 22.2 12 18.4 4.6 12 4.6Z"/><circle cx="12" cy="12" r="2.7" fill="#000"/>',
		freeview: '<path d="M12 1.6 15.2 6.2H8.8ZM12 22.4 8.8 17.8h6.4ZM1.6 12 6.2 8.8v6.4ZM22.4 12 17.8 15.2V8.8Z"/><rect x="10.9" y="4.6" width="2.2" height="14.8" rx="1.1"/><rect x="4.6" y="10.9" width="14.8" height="2.2" rx="1.1"/>',
		fullscreen: '<path d="M3.4 9.6V3.4h6.2M20.6 9.6V3.4h-6.2M3.4 14.4v6.2h6.2M20.6 14.4v6.2h-6.2" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>',
	};

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

	// Asked for by a button on the page, and by the viewers themselves for the
	// button they draw over the picture: what a browser will do about filling
	// the screen is the same either way, and it is knowledge the page has.
	function toggleFullscreen(options) {
		const settings = Object.assign({ element: document.documentElement }, options || {});
		if (!fullscreenSupported()) {
			return;
		}
		if (isFullscreen()) {
			(document.exitFullscreen || document.webkitExitFullscreen).call(document);
			return;
		}
		// A browser that says no says it in a promise nobody is waiting on,
		// which would otherwise be an unhandled rejection. It is said out loud
		// all the same: a button that does nothing is the hardest kind of
		// fault to look into, and the reason is in that rejection.
		const ask = settings.element.requestFullscreen || settings.element.webkitRequestFullscreen;
		// The older name returns nothing at all, so there is nothing to wait
		// on and nothing to be told; what follows is only for the newer one.
		Promise.resolve(ask.call(settings.element)).then(() => {
			// What is being watched is wide and a phone is tall. Only a page
			// that fills the screen may ask for this, which is why it is asked
			// for here and nowhere else; a browser that does not do it says so
			// and nothing else happens.
			if (screen.orientation && screen.orientation.lock) {
				screen.orientation.lock("landscape").catch(() => {});
			}
		}).catch(error => console.warn("DDNetLoader: this browser refused to fill the screen:", (error && error.message) || error));
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
			if (!on && screen.orientation && screen.orientation.unlock) {
				screen.orientation.unlock();
			}
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

	class Instance extends EventTarget {
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

		progress(text) {
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
				throw fail("BadOption", "DDNetLoader needs the program's factory, for example `module: DDNetDemoViewer`");
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
					toggle: () => toggleFullscreen(),
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
				// its own. Said here rather than switched off once it runs, so
				// that a bar the page does not want is never drawn at all.
				arguments: (options.controls === false ? ["--no-controls"] : []).concat(options.arguments || []),
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
					instance.progress(totalDependencies == 1 ? "Loading…" : `Loading… (${value}/${totalDependencies})`);
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
			return this;
		}

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
				elements.loading.style.display = "none";
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

	// A render is a worker's worth of work - every frame drawn, read back and
	// encoded - and none of it needs the page. Done in a worker, the page stays
	// answerable while it happens, which is the whole point of a render nobody
	// is watching. This is what that worker runs: it loads this script and the
	// program again, and renders with the same call the page would have made.
	const WORKER_BOOTSTRAP = `
self.onmessage = async event => {
	const request = event.data;
	try {
		const loader = await import(request.loaderUrl);
		// The program is a plain script - it names itself and expects to be
		// run as one - and a module worker has no way to run a plain script.
		// So it is read, given a line that hands its name out, and imported as
		// the module that makes of it. Nothing of the script itself is
		// changed, and what it needs to find its own files it is told through
		// \`scriptUrl\`.
		const text = await (await fetch(request.scriptUrl)).text();
		const wrapped = URL.createObjectURL(new Blob(
			[text + "\\nexport default " + request.moduleName + ";"], { type: "text/javascript" }));
		const factory = (await import(wrapped)).default;
		URL.revokeObjectURL(wrapped);
		const video = await loader.render(Object.assign({}, request.options, {
			module: factory,
			worker: false,
			videoSink: request.sink,
			onOutput: (message, kind) => self.postMessage({type: "output", message: message, kind: kind}),
			onRenderProgress: status => self.postMessage({type: "progress", status: status}),
		}));
		self.postMessage({type: "done", video: video});
	} catch (error) {
		self.postMessage({type: "failed", message: String((error && error.message) || error)});
	}
};
`;

	// Everything about a render that is data rather than a promise to call
	// back: what survives being sent to a worker.
	const WORKER_OPTIONS = [
		// tidy-alphabetical-start
		"arguments", "audio", "chat", "codec", "crf", "dataBase", "demo", "fps",
		"height", "homePath", "hud", "moduleName", "name", "output", "preset",
		"programName", "scriptUrl", "settings", "width",
		// tidy-alphabetical-end
	];

	// What a render hands to whoever wants to watch it: the same events the
	// page gets as callbacks, and the way to stop it.
	class RenderHandle extends EventTarget {
		constructor(stop) {
			super();
			this.stop = stop;
		}

		say(type, detail) {
			this.dispatchEvent(new CustomEvent(type, { detail: detail }));
		}

		quit() {
			this.stop();
		}
	}

	async function renderInWorker(options, loaderUrl) {
		// A worker inherits the page's isolation, so what the page cannot do
		// the worker cannot either - and it is said here, where the page is
		// listening, rather than from inside the worker.
		const problem = await supportError(true);
		if (problem !== null) {
			throw problem;
		}
		const program = new URL(options.scriptUrl, location.href);
		// Both scripts go in as blobs where they are not this page's own:
		// `importScripts` asks for a foreign script without CORS, which a page
		// that is cross-origin isolated then refuses.
		const [loaderScript, programScript] = await Promise.all([loaderUrl, program].map(async url =>
			url.origin === location.origin ? url.href : URL.createObjectURL(await fetchScript(url))));
		const request = { loaderUrl: loaderScript, scriptUrl: programScript, moduleName: options.moduleName || null, options: {}, sink: undefined };
		for (const key of WORKER_OPTIONS) {
			if (options[key] !== undefined) {
				request.options[key] = options[key];
			}
		}
		request.options.scriptUrl = program.href;
		// Swept here, where there is one of these per page, rather than in the
		// worker, where there is one per render.
		await sweepVideoScratch();
		request.options.sweepVideoScratch = false;
		// A destination the page picked can be handed over, if it is the kind
		// of stream that can be. Where it is not, the render stays here rather
		// than quietly writing somewhere else.
		const transfer = [];
		if (options.videoSink && typeof options.videoSink !== "function") {
			request.sink = options.videoSink;
			transfer.push(options.videoSink);
		}
		const bootstrap = URL.createObjectURL(new Blob([WORKER_BOOTSTRAP], { type: "text/javascript" }));
		const worker = new Worker(bootstrap, { type: "module" });
		URL.revokeObjectURL(bootstrap);
		// A worker that is stopped says nothing more, so whoever stopped it has
		// to be the one to answer for it: without this the render would be over
		// and the promise still waiting.
		var stopRender = () => worker.terminate();
		const handle = new RenderHandle(() => stopRender());
		const finished = new Promise((resolve, reject) => {
			stopRender = (reason) => {
				worker.terminate();
				reject(reason !== undefined ? reason : fail("RenderStopped", "The render was stopped."));
			};
			if (options.signal) {
				if (options.signal.aborted) {
					reject(abortError(options.signal));
					worker.terminate();
					return;
				}
				options.signal.addEventListener("abort", () => stopRender(abortError(options.signal)), { once: true });
			}
			worker.onmessage = event => {
				const message = event.data;
				if (message.type === "output") {
					handle.say("output", { message: message.message, kind: message.kind || {} });
					if (options.onOutput) {
						options.onOutput(message.message, message.kind || {});
					}
					return;
				}
				if (message.type === "progress") {
					handle.say("renderprogress", message.status);
					if (options.onRenderProgress) {
						options.onRenderProgress(message.status);
					}
					return;
				}
				worker.terminate();
				if (message.type === "done") {
					resolve(message.video);
				} else {
					reject(fail("RenderFailed", message.message));
				}
			};
			worker.onerror = event => {
				worker.terminate();
				reject(fail("RenderFailed", event.message || "the render worker stopped"));
			};
			worker.postMessage(request, transfer);
		});
		if (options.onStart) {
			options.onStart(handle);
		}
		return await finished;
	}

	// The viewers' own controls, as calls rather than as names to spell out:
	// a page steering one should not have to know that `ccall` exists, nor
	// which of the arguments are numbers. Every call answers `null` where the
	// program is not running, the same as `call` does.
	//
	// The other end of these is the `DemoViewer*` block in
	// `src/engine/client/demo_viewer_client.cpp` and the `MapViewer*` block in
	// `src/game/map/standalone/map_viewer_main.cpp`.
	function demoControls(instance) {
		const number = (name, argument) => argument === undefined
			? instance.call(name, "number")
			: instance.call(name, null, ["number"], [argument]);
		return {
			/**
			 * How big to draw, in the units the page measures its boxes in.
			 * Only for a viewer that sits in a box of the page's own: one that
			 * fills the window follows it by itself. See `followSize`.
			 */
			setSize: (Width, Height) => instance.call("DemoViewerSetSize", null, ["number", "number"], [Math.round(Width), Math.round(Height)]),
			/** Whether a demo is loaded and how long it is, in seconds. */
			length: () => number("DemoViewerLength"),
			/** How far it has played, between 0 and 1. */
			progress: () => number("DemoViewerProgress"),
			paused: () => number("DemoViewerPaused") === 1,
			pause: () => number("DemoViewerSetPaused", 1),
			play: () => number("DemoViewerSetPaused", 0),
			/** Jumps to a part of the demo, between 0 and 1. */
			seek: Fraction => number("DemoViewerSeekPercent", Fraction),
			/** Jumps to a time in the demo, in seconds. */
			seekTime: Seconds => number("DemoViewerSeekTime", Seconds),
			restart: () => instance.call("DemoViewerSeekStart"),
			/** The playback speed, or sets it: 1 is as it was played. */
			speed: Value => Value === undefined ? number("DemoViewerSpeed") : number("DemoViewerSetSpeed", Value),
			exporting: () => number("DemoViewerExporting") === 1,
			/**
			 * How an export is getting on: 0 before any was asked for, 1 while
			 * one is being written, 2 once one was handed over, 3 when it
			 * failed. What `startExport` answers cannot say, because in a
			 * browser it returns before the encoder has even been asked.
			 */
			exportState: () => number("DemoViewerExportState"),
			/**
			 * How far the video being written has got, between 0 and 1. Not
			 * where the demo on the canvas is: the export reads it through a
			 * way of its own, so both move at once and apart.
			 */
			exportProgress: () => number("DemoViewerExportProgress"),
			/**
			 * How much longer the export has to run, in seconds, or a negative
			 * number while there is no telling yet. Worked out from what is
			 * left of the demo and the rate frames are being written at, so it
			 * follows a machine that speeds up or slows down.
			 */
			exportSecondsLeft: () => number("DemoViewerExportSecondsLeft"),
			/**
			 * Why the export that was last asked for failed, or an empty
			 * string when none has. A page is the only place this can be
			 * said: there is no log for whoever is looking at the demo.
			 */
			exportError: () => instance.call("DemoViewerExportError", "string") || "",
			/** Throws away the export that is running, and its file with it. */
			cancelExport: () => instance.call("DemoViewerCancelExport"),
			/**
			 * Who the demo is watched over the shoulder of, or sets it: a
			 * client id, -1 for a camera of one's own that the pointer drags
			 * around, or -2 for whoever recorded the demo. A demo a server
			 * recorded has nobody who recorded it, so it starts at -1.
			 */
			spectating: Id => Id === undefined
				? number("DemoViewerSpectating")
				: number("DemoViewerSetSpectate", Id),
			/** Follows whoever is called this, once the demo has named them. */
			spectateName: Name => instance.call("DemoViewerSetSpectateName", null, ["string"], [Name || ""]),
			/** On to the next player there is, or the one before. */
			spectateStep: Direction => number("DemoViewerSpectateStep", Direction),
			/**
			 * The players the demo has named so far, as `{id, name}` objects.
			 * A demo names them a snapshot or two in, so a list built from this
			 * is worth building again while it plays.
			 */
			players: () => JSON.parse(instance.call("DemoViewerPlayers", "string") || "[]"),
			/**
			 * How much of the world is in the canvas, or multiplies it. The
			 * wheel over the canvas does the same thing.
			 */
			zoom: Factor => Factor === undefined
				? number("DemoViewerZoom")
				: number("DemoViewerZoomBy", Factor),
			/**
			 * Whether the viewer draws its own bar of controls over the demo,
			 * or switches it on and off. A page with controls of its own turns
			 * it off - better with `controls: false`, which leaves it off from
			 * the first frame rather than after it.
			 */
			controls: Show => Show === undefined
				? number("DemoViewerControls") === 1
				: instance.call("DemoViewerSetControls", null, ["number"], [Show ? 1 : 0]),
			/**
			 * Starts a video export, and says whether it started. The options
			 * are named as in `render`, because they are the same settings the
			 * render tool takes: `width`, `height`, `fps`, `crf`, `codec`,
			 * `audio`, `hud`, `chat`.
			 */
			startExport: options => {
				const settings = options || {};
				return instance.call("DemoViewerStartExport", "number",
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

	// A tile is 32 world units across, in every map there is. The viewer is
	// written in those units and a page has no business knowing them, so the
	// one place that turns the one into the other is here.
	const MAP_TILE_SIZE = 32;

	function mapControls(instance) {
		const tiles = name => {
			const value = instance.call(name, "number");
			return value === null ? null : value / MAP_TILE_SIZE;
		};
		const setNumbers = (name, values) =>
			instance.call(name, null, values.map(() => "number"), values);
		return {
			loaded: () => instance.call("MapViewerMapLoaded", "number") === 1,
			/**
			 * How big to draw, in the units the page measures its boxes in.
			 * Only for a viewer that sits in a box of the page's own: one that
			 * fills the window follows it by itself. See `followSize`.
			 */
			setSize: (Width, Height) => setNumbers("MapViewerSetSize", [Math.round(Width), Math.round(Height)]),
			/** Fits the whole map on screen. */
			fit: () => instance.call("MapViewerFit"),
			/** How big the map is, in tiles. */
			size: () => {
				const width = tiles("MapViewerMapWidth");
				return width === null ? null : { width, height: tiles("MapViewerMapHeight") };
			},
			/** Where the view looks, in tiles, or looks there. */
			center: (X, Y) => {
				if (X === undefined) {
					const x = tiles("MapViewerCenterX");
					return x === null ? null : { x, y: tiles("MapViewerCenterY") };
				}
				return setNumbers("MapViewerSetCenter", [X * MAP_TILE_SIZE, Y * MAP_TILE_SIZE]);
			},
			/**
			 * How many tiles are across the screen, or zooms until that many
			 * are. It is the zoom said in a way that means the same in every
			 * window, which is what a link and a readout both need.
			 */
			tilesAcross: Tiles => {
				const visible = tiles("MapViewerVisibleWidth");
				if (Tiles === undefined) {
					return visible;
				}
				// What the view is set by is the zoom, and what that comes to in
				// tiles is the window's business - so it is asked what it shows
				// now and told the factor between that and what is wanted.
				if (visible > 0 && Tiles > 0) {
					setNumbers("MapViewerSetZoom", [instance.call("MapViewerZoom", "number") * Tiles / visible]);
				}
			},
			/** Zooms by a factor: 2 puts twice as much of the map on screen. */
			zoomBy: Factor => {
				const zoom = instance.call("MapViewerZoom", "number");
				if (zoom !== null) {
					setNumbers("MapViewerSetZoom", [zoom * Factor]);
				}
			},
			/**
			 * Whether the parts of the map that are only there to be looked at
			 * are drawn, or turns them on and off.
			 */
			highDetail: On => On === undefined
				? instance.call("MapViewerHighDetail", "number") === 1
				: instance.call("MapViewerSetHighDetail", null, ["number"], [On ? 1 : 0]),
			/**
			 * Whether what the tiles do is drawn over what they look like, or
			 * turns that on and off.
			 */
			entities: On => On === undefined
				? instance.call("MapViewerEntities", "number") === 1
				: instance.call("MapViewerSetEntities", null, ["number"], [On ? 1 : 0]),
			/**
			 * Whether the viewer draws its own bar of controls over the map,
			 * or switches it on and off, as in `demoControls`.
			 */
			controls: Show => Show === undefined
				? instance.call("MapViewerControls", "number") === 1
				: instance.call("MapViewerSetControls", null, ["number"], [Show ? 1 : 0]),
			/** Writes what is on screen, or the whole map, to a picture. */
			exportView: () => instance.call("MapViewerExportView"),
			exportFullMap: () => instance.call("MapViewerExportFullMap"),
			/** 0 while nothing is being written, 1 while it is, 2 when it failed. */
			exportState: () => instance.call("MapViewerExportState", "number"),
		};
	}

	// A zip of files that are already in memory, so that a batch of them is one
	// thing to save rather than one prompt each. Written by hand because it is
	// short: stored, never deflated - a video is compressed already and
	// squeezing it again would only cost time - and that leaves headers, a
	// central directory and a checksum per file.
	const CRC_TABLE = (() => {
		const table = new Uint32Array(256);
		for (let i = 0; i < 256; ++i) {
			let value = i;
			for (let bit = 0; bit < 8; ++bit) {
				value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
			}
			table[i] = value >>> 0;
		}
		return table;
	})();

	function crc32(bytes) {
		let crc = 0xffffffff;
		for (let i = 0; i < bytes.length; ++i) {
			crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
		}
		return (crc ^ 0xffffffff) >>> 0;
	}

	async function zip(entries) {
		// A zip says its sizes and offsets in 32 bits. Past that it takes the
		// ZIP64 records, which is a second format to write and to get wrong for
		// something nobody should be downloading in one piece anyway.
		const total = entries.reduce((sum, entry) => sum + entry.blob.size, 0);
		if (total >= 0xffffffff || entries.some(entry => entry.blob.size >= 0xffffffff)) {
			throw fail("ZipTooLarge", "Too much to put into one zip file");
		}
		const encoder = new TextEncoder();
		const now = new Date();
		const time = ((now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1)) & 0xffff;
		const date = (((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate()) & 0xffff;
		const parts = [];
		const central = [];
		var offset = 0;
		for (const entry of entries) {
			const name = encoder.encode(entry.name);
			const bytes = new Uint8Array(await entry.blob.arrayBuffer());
			const crc = crc32(bytes);
			const local = new DataView(new ArrayBuffer(30));
			local.setUint32(0, 0x04034b50, true);
			local.setUint16(4, 20, true);
			local.setUint16(6, 0x0800, true); // The names are UTF-8.
			local.setUint16(8, 0, true); // Stored.
			local.setUint16(10, time, true);
			local.setUint16(12, date, true);
			local.setUint32(14, crc, true);
			local.setUint32(18, bytes.length, true);
			local.setUint32(22, bytes.length, true);
			local.setUint16(26, name.length, true);
			parts.push(local.buffer, name, bytes);
			const directory = new DataView(new ArrayBuffer(46));
			directory.setUint32(0, 0x02014b50, true);
			directory.setUint16(4, 20, true);
			directory.setUint16(6, 20, true);
			directory.setUint16(8, 0x0800, true);
			directory.setUint16(10, 0, true);
			directory.setUint16(12, time, true);
			directory.setUint16(14, date, true);
			directory.setUint32(16, crc, true);
			directory.setUint32(20, bytes.length, true);
			directory.setUint32(24, bytes.length, true);
			directory.setUint16(28, name.length, true);
			directory.setUint32(42, offset, true);
			central.push(directory.buffer, name);
			offset += 30 + name.length + bytes.length;
		}
		const directorySize = central.reduce((sum, part) => sum + part.byteLength, 0);
		const end = new DataView(new ArrayBuffer(22));
		end.setUint32(0, 0x06054b50, true);
		end.setUint16(8, entries.length, true);
		end.setUint16(10, entries.length, true);
		end.setUint32(12, directorySize, true);
		end.setUint32(16, offset, true);
		return new Blob(parts.concat(central, [end.buffer]), { type: "application/zip" });
	}

	// What the render tool is asked on a command line, from what the page
	// asked for here. It is the same program with the same arguments as the one
	// a terminal starts, so `ddnet-demo-render --help` documents these too.
	function renderOptions(options) {
		const args = ["--output", options.output || "video.mp4"];
		const flag = (name, argument) => {
			if (options[name] !== undefined && options[name] !== null) {
				args.push(argument, String(options[name]));
			}
		};
		flag("width", "--width");
		flag("height", "--height");
		flag("fps", "--fps");
		flag("codec", "--codec");
		flag("crf", "--crf");
		flag("preset", "--preset");
		if (options.audio === false) {
			args.push("--no-audio");
		}
		if (options.hud === true) {
			args.push("--hud");
		}
		if (options.chat === false) {
			args.push("--no-chat");
		}
		// Who to watch, for a demo a server recorded: it has nobody who
		// recorded it, so without this the camera stands still.
		if (options.follow !== undefined && options.follow !== null && options.follow !== "") {
			args.push("--follow", String(options.follow));
		}
		// Everything the client takes on its command line it takes here as
		// well, one console command per entry, so `cl_showfps 1` works.
		for (const setting of options.settings || []) {
			args.push(setting);
		}
		return Object.assign({}, options, {
			canvas: null,
			persist: false,
			needsWebGpu: true,
			accept: [".demo"],
			file: options.demo,
			fileName: options.name || "render.demo",
			fileArgument: "--render-demo",
			programName: options.programName || "The demo renderer",
			arguments: args.concat(options.arguments || []),
		});
	}

	return {
		/**
		 * This library's own version, which says what its API looks like. Not
		 * the game's version: the two move for different reasons.
		 */
		version: VERSION,

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
		 * * `NoVideoEncoder` - this browser cannot encode video.
		 * * `FileRefused` - the file is not a kind the program takes.
		 * * `FileTooLarge` - a file named in a URL is bigger than this will
		 *   fetch into memory.
		 * * `FetchFailed` - something the program needed answered with an
		 *   error.
		 * * `RenderStopped` - a render was stopped on purpose.
		 * * `RenderFailed` - a render ended without a video.
		 * * `ZipTooLarge` - more than fits in one zip file was put into one.
		 */
		Error: DDNetLoaderError,

		/**
		 * Starts a program on a canvas.
		 *
		 * @param options.module The factory the program's script defines, so
		 * `DDNetClient`, `DDNetDemoViewer` or `DDNetMapViewer`.
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
		 * The instance is also an `EventTarget`, which is the other way of
		 * hearing the same three things: `output` with
		 * `{detail: {message, kind}}`, `progress` with `{detail: {text}}` and
		 * `exit`. A listener and a callback can both be there; output nobody
		 * listens to and nobody was handed goes to the console.
		 *
		 * @returns a promise for the running instance.
		 */
		start(options) {
			checkOptions("start", options, START_OPTIONS);
			return new Instance(options).run();
		},

		/**
		 * What a demo viewer can be asked to do, bound to one of them. The page
		 * that hosts it needs nothing else to steer it - and neither does a URL
		 * that says where to start, which is `urlParameter` below.
		 */
		demoControls(instance) {
			return demoControls(instance);
		},

		/**
		 * The same for a map viewer: what it shows, in tiles, and everything
		 * that changes it. A page steers it with these, and so does a link -
		 * `#x=…&y=…&tiles=…` is only these calls made for somebody else.
		 */
		mapControls(instance) {
			return mapControls(instance);
		},

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

		/**
		 * Puts files that are in memory into one zip file, so that a batch of
		 * them is one thing to save. Nothing is compressed - what this is for
		 * is videos, which are compressed already.
		 *
		 * @param entries `{name, blob}` objects, in the order they should be in.
		 *
		 * @return A promise of the zip as a `Blob`. It rejects when the whole
		 * of it would not fit in the 32 bits a plain zip counts in.
		 */
		zip(entries) {
			return zip(entries);
		},

		/** `start` with the furniture our own pages share around it. */
		page(options) {
			checkOptions("page", options, PAGE_OPTIONS);
			return new Instance(pageOptions(options)).run();
		},

		/**
		 * Renders a demo into a video file, with nothing on screen.
		 *
		 * The render tool draws into render targets, so it needs no canvas and
		 * takes no part of the page: it reads a demo, encodes every frame of it
		 * with the browser's own video encoder, and the file it would otherwise
		 * offer as a download is handed back here instead.
		 *
		 * @param options.module The factory `ddnet-demo-render.js` defines, so
		 * `DDNetDemoRenderer`.
		 * @param options.demo The demo to render: bytes, a `File`, or the URL of
		 * one.
		 * @param options.name The demo's file name, when it comes as bytes.
		 * @param options.output The name the video carries, `video.mp4`
		 * otherwise.
		 * @param options.width Video width in pixels, even, `cl_video_width`
		 * otherwise; `height`, `fps`, `codec`, `crf` and `preset` the same way.
		 * @param options.audio `false` renders without a sound track.
		 * @param options.hud `true` shows the ingame interface, `options.chat`
		 * `false` hides the chat.
		 * @param options.settings Console commands, one per entry.
		 * @param options.dataBase Where the `data` directory is, if it is not
		 * next to the page.
		 * @param options.onOutput Called for every line the render writes.
		 * @param options.onRenderProgress Called once a second while the render
		 * runs, with `{progress, encodedFrames, submittedFrames,
		 * framesPerSecond}` - `progress` is the part of the demo that is done,
		 * between 0 and 1.
		 * @param options.onStart Called with the running instance, whose `quit`
		 * ends a render that is taking too long.
		 * @param options.scriptUrl Where `ddnet-demo-render.js` was loaded from.
		 * With it the render happens in a worker and leaves the page free;
		 * `worker: false` keeps it here.
		 * @param options.videoSink Where the video is written while it is made.
		 * A `WritableStream` can go to the worker with it; a function cannot,
		 * and is only asked here.
		 * @param options.signal An `AbortSignal`. Aborting it stops the render
		 * and ends the promise with whatever the signal was aborted with; a
		 * render that is stopped without one ends with `RenderStopped`.
		 *
		 * What `onStart` is handed is an `EventTarget` as well: `output` and
		 * `renderprogress` are the same two things the callbacks say, and
		 * `quit()` stops it.
		 *
		 * @returns a promise for the finished MP4 as a `Blob`.
		 */
		async render(options) {
			checkOptions("render", options, RENDER_OPTIONS);
			// Encoding is the browser's to do, and a browser without an
			// encoder is worth saying so before a demo is fetched and a
			// program started for nothing.
			if (typeof VideoEncoder === "undefined") {
				throw fail("NoVideoEncoder", "This browser cannot encode video: it has no VideoEncoder. Chrome, Edge and a current Firefox or Safari have one.");
			}
			// A worker needs to load the program itself, so it needs to be told
			// where it is; without that this is the only thread there is.
			if (options.worker !== false && typeof Worker === "function" && options.scriptUrl && LOADER_URL) {
				return await renderInWorker(options, new URL(LOADER_URL, location.href));
			}
			const instance = new Instance(Object.assign(renderOptions(options), {
				onVideo: file => {
					instance.video = file;
				},
			}));
			await instance.run();
			if (options.onStart) {
				options.onStart(instance);
			}
			await instance.finished;
			// A render that was called off is not a render that failed, and
			// whoever called it off is told so in their own words.
			if (options.signal && options.signal.aborted) {
				throw abortError(options.signal);
			}
			if (instance.video == null) {
				throw fail("RenderFailed", "The demo was not rendered into a video, see the output for what went wrong");
			}
			return instance.video;
		},
	};
})();

// Both forms of the same thing: everything at once for whoever wants to write
// `DDNetLoader.start(…)`, and one name at a time for whoever would rather
// import only what they use. The names are the object's own, so there is one
// list and not two.
export default DDNetLoader;
export const {
	// tidy-alphabetical-start
	autoHide, demoControls, exportSettingsForm, followSize, fullscreen,
	fullscreenSupported, icon, isFullscreen, mapControls, page, paintIcons,
	render, setUrlParameters, start, supportProblem, toggleFullscreen,
	urlParameter, version, videoCodecs, zip,
	// tidy-alphabetical-end
} = DDNetLoader;
// Not as `Error`: a name at the top of a module is a name for the whole of it,
// and this one is the browser's own further up - where the class above is
// declared from it.
export const DDNetLoaderError = DDNetLoader.Error;
