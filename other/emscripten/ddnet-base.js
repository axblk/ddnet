// Runs the DDNet programs built with Emscripten on a page. The API is
// documented in ddnet-base.d.ts.

export class DDNetBaseError extends Error {
	constructor(code, message) {
		super(message);
		this.name = "DDNetBaseError";
		this.code = code;
	}
}

const fail = (code, message) => new DDNetBaseError(code, message);

const DEFAULT_HOME_PATH = "/home/web_user/.local/share/ddnet";
// A file named by URL is fetched into memory, so its size is capped.
const MAX_URL_FILE_BYTES = 256 * 1024 * 1024;

export const moduleUrl = import.meta.url;

function sanitizeFilename(filename) {
	return filename.replace(/[\/\\\?\|\*<>:"]/g, "_");
}

function parseAnsiColorRgb(line) {
	// Our logger only writes RGB colours.
	const match = line.match(/^\x1b\[38;2;(\d+);(\d+);(\d+)m([\s\S]*?)\x1b\[0m$/);
	if (!match) {
		return { color: null, message: line };
	}
	const [, r, g, b, message] = match;
	return { color: `rgb(${r}, ${g}, ${b})`, message: message };
}

export const abortError = signal => signal.reason ?? fail("Stopped", "This was stopped.");

// The fragment is read first: it never reaches a server.
export function urlParameter(name) {
	const fragment = new URLSearchParams(location.hash.slice(1));
	if (fragment.has(name)) {
		return fragment.get(name);
	}
	return new URLSearchParams(location.search).get(name);
}

// Written by hand rather than with `URLSearchParams`, which would escape every
// slash of a URL in the fragment and make the link unreadable.
export function setUrlParameters(values) {
	const parts = location.hash.slice(1).split("&")
		.filter(part => part !== "" && !(part.split("=")[0] in values));
	for (const [name, value] of Object.entries(values)) {
		if (value !== null && value !== undefined) {
			parts.push(`${name}=${value}`);
		}
	}
	const hash = parts.length === 0 ? "" : `#${parts.join("&")}`;
	if (hash !== location.hash) {
		history.replaceState(history.state, "", hash === "" ? location.pathname + location.search : hash);
	}
}

// The service worker keeps fetched data files until the index no longer lists
// them; it is told to compare once per page load.
let sweptDataCache = false;
function sweepDataCache() {
	if (!sweptDataCache && navigator.serviceWorker?.controller) {
		sweptDataCache = true;
		navigator.serviceWorker.controller.postMessage({ type: "ddnet-data-sweep" });
	}
}

export async function supportError(needsWebGpu) {
	if (typeof SharedArrayBuffer === "undefined" || self.crossOriginIsolated === false) {
		return fail("CrossOriginRefused",
			"This page is not cross-origin isolated, so the browser withholds the shared memory this needs. " +
			"The page has to send Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp, " +
			"or load coi-serviceworker.js before anything else.");
	}
	if (needsWebGpu) {
		if (!navigator.gpu) {
			return fail("NoWebGpu", "This browser has no WebGPU, which rendering without a window needs.");
		}
		const adapter = await navigator.gpu.requestAdapter().catch(() => null);
		if (!adapter) {
			return fail("NoWebGpu", "This browser has WebGPU but no graphics adapter it is willing to use.");
		}
	}
	return null;
}

// The same families in the same order as `Module.ddnetVideoFamilies` in
// `src/engine/client/video_webcodecs.cpp`, which picks the first profile of a
// family the browser can configure.
const VIDEO_CODEC_FAMILIES = [
	["H.264", ["avc1.640028", "avc1.4D0028", "avc1.42E01E"]],
	["H.265", ["hvc1.1.6.L120.B0"]],
	["AV1", ["av01.0.08M.08"]],
];
let videoCodecsProbe = null;

function videoCodecs() {
	videoCodecsProbe ??= (async () => {
		if (typeof VideoEncoder === "undefined") {
			return [];
		}
		const supported = async candidates => {
			for (const codec of candidates) {
				const support = await VideoEncoder.isConfigSupported({ codec, width: 1280, height: 720, bitrate: 4000000, framerate: 60 }).catch(() => null);
				if (support?.supported) {
					return codec;
				}
			}
			return null;
		};
		const found = await Promise.all(VIDEO_CODEC_FAMILIES.map(async ([display, candidates]) => {
			const name = await supported(candidates);
			return name === null ? null : { name, display };
		}));
		return found.filter(entry => entry !== null);
	})();
	return videoCodecsProbe;
}

// Controls over a picture fade out while nothing happens and come back on any
// use, the way a video player's do. `hold` keeps them while it answers true -
// a menu that is open, a playback that is paused.
export function autoHide(elements, options = {}) {
	const { delay = 3000, picture = null, hold = () => false, onHide = null, signal } = options;
	const on = (target, type, listener, extra) => target.addEventListener(type, listener, { signal, ...extra });
	let timer = null;
	let shown = false;
	let hovered = 0;
	// Keyboard focus holds them, the focus a click leaves behind does not.
	const held = () => hovered > 0 || hold() || elements.some(element => element.querySelector(":focus-visible") !== null);
	const hide = () => {
		clearTimeout(timer);
		shown = false;
		for (const element of elements) {
			element.classList.add("faded");
		}
		onHide?.();
	};
	const schedule = () => {
		clearTimeout(timer);
		timer = setTimeout(() => (held() ? schedule() : hide()), delay);
	};
	const show = () => {
		shown = true;
		for (const element of elements) {
			element.classList.remove("faded");
		}
		schedule();
	};
	for (const element of elements) {
		on(element, "pointerenter", event => {
			if (event.pointerType === "mouse") {
				hovered++;
			}
		});
		on(element, "pointerleave", event => {
			if (event.pointerType === "mouse") {
				hovered = Math.max(hovered - 1, 0);
			}
		});
		on(element, "focusin", show);
	}
	if (picture !== null) {
		// A tap on the picture toggles the controls, as on a phone's video
		// player. A mouse only has to move.
		let pressed = null;
		on(picture, "pointerdown", event => {
			pressed = { x: event.clientX, y: event.clientY, when: performance.now(), shown, touch: event.pointerType !== "mouse" };
		}, { capture: true });
		on(picture, "pointerup", event => {
			if (pressed === null) {
				return;
			}
			const tap = pressed.touch && Math.hypot(event.clientX - pressed.x, event.clientY - pressed.y) <= 16 && performance.now() - pressed.when <= 400;
			const wasShown = pressed.shown;
			pressed = null;
			const onControls = elements.some(element => event.composedPath().includes(element));
			if (tap && wasShown && !onControls && !hold()) {
				hide();
			} else {
				show();
			}
		});
		on(picture, "pointercancel", () => { pressed = null; });
		for (const type of ["pointermove", "keydown", "wheel"]) {
			on(picture, type, event => {
				if (event.type !== "pointermove" || event.pointerType === "mouse") {
					show();
				}
			}, { passive: true });
		}
	}
	show();
	return { show, hide, shown: () => shown };
}

// A viewer in a box of the page's own is told the size of the box; one that
// fills the window follows the window by itself.
export function followSize(element, program, options = {}) {
	let width = 0;
	let height = 0;
	const measure = () => {
		const box = element.getBoundingClientRect();
		const nextWidth = Math.round(box.width);
		const nextHeight = Math.round(box.height);
		// A box that is not shown keeps the size it had.
		if (nextWidth <= 0 || nextHeight <= 0 || (nextWidth === width && nextHeight === height)) {
			return;
		}
		width = nextWidth;
		height = nextHeight;
		program.setSize(width, height);
	};
	const observer = new ResizeObserver(measure);
	observer.observe(element);
	const stop = () => observer.disconnect();
	options.signal?.addEventListener("abort", stop, { once: true });
	measure();
	return { stop };
}

// Named and drawn like `CViewerControls::EIcon` in
// `src/engine/client/viewer_controls.h`, so a button shows the same picture in
// the page and in the program. Packages add the ones only they use.
const ICONS = {
	menu: '<rect x="3" y="5" width="18" height="2.6" rx="1.3"/><rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/><rect x="3" y="16.4" width="18" height="2.6" rx="1.3"/>',
	minus: '<rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/>',
	plus: '<rect x="3" y="10.7" width="18" height="2.6" rx="1.3"/><rect x="10.7" y="3" width="2.6" height="18" rx="1.3"/>',
	fit: '<rect x="2.7" y="4.7" width="18.6" height="14.6" rx="2" fill="none" stroke="currentColor" stroke-width="2.4"/><rect x="6.6" y="8.6" width="10.8" height="6.8" opacity="0.55"/>',
	save: '<path d="M12 2.8V12" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/><path d="M8 8.8 12 13 16 8.8" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/><path d="M3.8 15.4v3.4a1.4 1.4 0 0 0 1.4 1.4h13.6a1.4 1.4 0 0 0 1.4-1.4v-3.4" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>',
	stop: '<rect x="4.5" y="4.5" width="15" height="15" rx="3"/>',
	fullscreen: '<path d="M3.4 9.6V3.4h6.2M20.6 9.6V3.4h-6.2M3.4 14.4v6.2h6.2M20.6 14.4v6.2h-6.2" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>',
};

export function addIcons(pictures) {
	Object.assign(ICONS, pictures);
}

export function icon(name) {
	const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
	svg.setAttribute("viewBox", "0 0 24 24");
	svg.setAttribute("aria-hidden", "true");
	svg.setAttribute("focusable", "false");
	svg.innerHTML = ICONS[name] || "";
	return svg;
}

export function paintIcons(root = document) {
	for (const element of root.querySelectorAll("[data-icon]")) {
		const wanted = element.dataset.icon;
		if (element.dataset.painted === wanted) {
			continue;
		}
		element.dataset.painted = wanted;
		element.querySelector("svg")?.remove();
		element.prepend(icon(wanted));
	}
}

// Safari before 16.4 only has the prefixed names, and an iPhone allows only a
// video to fill the screen.
const fullscreenSupported = () => document.fullscreenEnabled === true || document.webkitFullscreenEnabled === true;
const fullscreenElement = () => document.fullscreenElement ?? document.webkitFullscreenElement ?? null;

function toggleFullscreen(element = document.documentElement) {
	if (!fullscreenSupported()) {
		return;
	}
	if (fullscreenElement() !== null) {
		(document.exitFullscreen || document.webkitExitFullscreen).call(document);
		return;
	}
	// The prefixed variant returns nothing.
	const request = element.requestFullscreen || element.webkitRequestFullscreen;
	Promise.resolve(request.call(element)).catch(error => console.warn("DDNetBase: the browser refused to fill the screen:", error?.message ?? error));
}

export function fullscreen(button, options = {}) {
	const { element = document.documentElement, shortcut = null, signal } = options;
	if (!fullscreenSupported()) {
		button.hidden = true;
		return;
	}
	const update = () => {
		const active = fullscreenElement() !== null;
		const label = active ? "Exit full screen" : "Full screen";
		if (button.dataset.icon === undefined) {
			button.textContent = label;
		}
		button.setAttribute("aria-label", label);
		button.title = shortcut === null ? label : `${label} (${shortcut})`;
		if (shortcut !== null) {
			button.setAttribute("aria-keyshortcuts", shortcut);
		}
	};
	button.addEventListener("click", () => toggleFullscreen(element), { signal });
	for (const type of ["fullscreenchange", "webkitfullscreenchange"]) {
		document.addEventListener(type, update, { signal });
	}
	update();
}

// The same sizes and frame rates as `gs_aaVideoResolutionPresets` and
// `s_aFpsPresets` in `src/game/client/components/menus.cpp`.
const VIDEO_SIZE_PRESETS = [[1280, 720], [1920, 1080], [2560, 1440], [3840, 2160]];
const VIDEO_FPS_PRESETS = [30, 50, 60, 120, 144, 240];
// Constant rate factors, lower is better; 23 is the encoder's default.
const VIDEO_QUALITY_PRESETS = [[16, "Best"], [18, "High"], [23, "Normal"], [28, "Small"]];

export function exportSettingsForm(container, options = {}) {
	const { canvas = null, audio: audioDefault = true } = options;
	const element = (tag, properties) => Object.assign(document.createElement(tag), properties);
	const labelled = (text, ...controls) => {
		const label = element("label");
		if (text !== null) {
			label.append(`${text} `);
		}
		label.append(...controls);
		container.append(label);
		return label;
	};
	const option = (value, text) => element("option", { value: String(value), textContent: text });
	const preset = (select, value) => [...select.options].some(entry => entry.value === value) ? value : "custom";

	const size = element("select");
	if (canvas !== null) {
		size.append(option("canvas", "Canvas size"));
	}
	for (const [width, height] of VIDEO_SIZE_PRESETS) {
		size.append(option(`${width}x${height}`, `${width} × ${height}`));
	}
	size.append(option("custom", "Custom"));
	size.value = "1920x1080";
	labelled("Size", size);
	const width = element("input", { type: "number", min: 16, max: 8192, step: 2, value: 1920, ariaLabel: "Width" });
	const height = element("input", { type: "number", min: 16, max: 8192, step: 2, value: 1080, ariaLabel: "Height" });
	const customSize = labelled(null, width, element("span", { textContent: "×" }), height);

	const fps = element("select");
	fps.append(...VIDEO_FPS_PRESETS.map(value => option(value, String(value))), option("custom", "Custom"));
	fps.value = "60";
	labelled("FPS", fps);
	const customFpsValue = element("input", { type: "number", min: 1, max: 1000, value: 60, ariaLabel: "Frames per second" });
	const customFps = labelled(null, customFpsValue);

	const quality = element("select");
	quality.append(...VIDEO_QUALITY_PRESETS.map(([value, text]) => option(value, `${text} (${value})`)), option("custom", "Custom"));
	quality.value = "23";
	labelled("Quality", quality);
	const crf = element("input", { type: "number", min: 0, max: 51, value: 23, title: "Lower is better, 0 is lossless", ariaLabel: "Constant rate factor" });
	const customQuality = labelled(null, crf);

	const codec = element("select");
	codec.append(option("", "Default"));
	labelled("Codec", codec);
	// The menu is filled once the browser answered; a codec asked for before
	// that is chosen when it turns up.
	let wantedCodec = null;
	const applyCodec = () => {
		if (wantedCodec !== null && [...codec.options].some(entry => entry.value === wantedCodec)) {
			codec.value = wantedCodec;
			wantedCodec = null;
		}
	};
	videoCodecs().then(codecs => {
		codec.append(...codecs.map(entry => option(entry.name, entry.display)));
		applyCodec();
	});

	const audio = element("input", { type: "checkbox", checked: audioDefault });
	const hud = element("input", { type: "checkbox" });
	const chat = element("input", { type: "checkbox", checked: true });
	labelled(null, audio, " Sound");
	labelled(null, hud, " Interface");
	labelled(null, chat, " Chat");

	const updateCustom = () => {
		customSize.hidden = size.value !== "custom";
		customFps.hidden = fps.value !== "custom";
		customQuality.hidden = quality.value !== "custom";
	};
	for (const select of [size, fps, quality]) {
		select.addEventListener("change", updateCustom);
	}
	updateCustom();

	return {
		values() {
			let [chosenWidth, chosenHeight] = [parseInt(width.value, 10), parseInt(height.value, 10)];
			if (size.value === "canvas") {
				[chosenWidth, chosenHeight] = [canvas.width, canvas.height];
			} else if (size.value !== "custom") {
				[chosenWidth, chosenHeight] = size.value.split("x").map(part => parseInt(part, 10));
			}
			return {
				// Encoders take whole macroblocks.
				width: chosenWidth & ~1,
				height: chosenHeight & ~1,
				fps: parseInt(fps.value === "custom" ? customFpsValue.value : fps.value, 10),
				crf: parseInt(quality.value === "custom" ? crf.value : quality.value, 10),
				codec: codec.value === "" ? null : codec.value,
				audio: audio.checked,
				hud: hud.checked,
				chat: chat.checked,
			};
		},

		setValues(values) {
			if (values.width > 0 && values.height > 0) {
				width.value = values.width;
				height.value = values.height;
				size.value = preset(size, `${values.width}x${values.height}`);
			}
			if (values.fps > 0) {
				customFpsValue.value = values.fps;
				fps.value = preset(fps, String(values.fps));
			}
			if (values.crf != null) {
				crf.value = values.crf;
				quality.value = preset(quality, String(values.crf));
			}
			if (values.codec != null) {
				wantedCodec = values.codec;
				applyCodec();
			}
			for (const [name, control] of [["audio", audio], ["hud", hud], ["chat", chat]]) {
				if (values[name] != null) {
					control.checked = values[name] === true;
				}
			}
			updateCustom();
		},
	};
}

// Fetched rather than linked, so that a script from another origin becomes a
// blob of this page's own, which a worker may be made from.
export async function fetchScript(url) {
	const response = await fetch(new URL(url, location.href).href, { mode: "cors" });
	if (!response.ok) {
		throw fail("FetchFailed", `${response.url} answered ${response.status} ${response.statusText}`);
	}
	return new Blob([await response.text()], { type: "text/javascript" });
}

// The program's script is a classic script that defines one global factory, so
// it is imported as a module that exports that factory.
const importedPrograms = new Map();

export function importProgram(scriptUrl, moduleName) {
	const key = `${scriptUrl}|${moduleName}`;
	if (!importedPrograms.has(key)) {
		importedPrograms.set(key, (async () => {
			const response = await fetch(scriptUrl);
			if (!response.ok) {
				throw fail("FetchFailed", `${scriptUrl} answered ${response.status} ${response.statusText}`);
			}
			const wrapped = URL.createObjectURL(new Blob([`${await response.text()}\nexport default ${moduleName};`], { type: "text/javascript" }));
			try {
				return (await import(wrapped)).default;
			} finally {
				URL.revokeObjectURL(wrapped);
			}
		})());
	}
	return importedPrograms.get(key);
}

// A video is written to the origin private file system while it is encoded,
// so a long export is bounded by the disk rather than by the tab's memory.
const VIDEO_SCRATCH_DIRECTORY = "ddnet-video";

async function videoScratchDirectory(create) {
	if (!navigator.storage?.getDirectory) {
		return null;
	}
	const root = await navigator.storage.getDirectory();
	return await root.getDirectoryHandle(VIDEO_SCRATCH_DIRECTORY, { create });
}

async function videoScratchSink(info) {
	const directory = await videoScratchDirectory(true);
	if (directory === null) {
		return null;
	}
	const handle = await directory.getFileHandle(`${Date.now()}-${sanitizeFilename(info.fileName)}`, { create: true });
	return { stream: await handle.createWritable(), done: () => handle.getFile() };
}

// Scratch videos of earlier visits were never taken. Swept once per page, and
// by the page rather than by render workers: a second worker of a batch would
// otherwise delete the first one's video while it is still being offered.
let sweptVideoScratch = false;
export async function sweepVideoScratch() {
	if (sweptVideoScratch) {
		return;
	}
	sweptVideoScratch = true;
	try {
		const directory = await videoScratchDirectory(false);
		for await (const name of directory?.keys() ?? []) {
			await directory.removeEntry(name).catch(() => {});
		}
	} catch (error) {
		// Nothing to sweep.
	}
}

// Browser shortcuts stay the browser's, whatever the program does with keys,
// and so do the keys the program says it does not need.
function installKeyGuard(signal, passes) {
	document.addEventListener("keydown", event => {
		if ((event.ctrlKey && event.key === "F5") || event.key === "F11" || event.key === "F12" || passes(event)) {
			event.stopPropagation();
		}
	}, { capture: true, signal });
}

export class Program extends EventTarget {
	static script = null;
	static base = moduleUrl;
	static moduleName = null;
	static programName = null;
	static suffix = null;

	static scriptUrl() {
		return this.script === null ? null : new URL(this.script, this.base).href;
	}

	static open(options) {
		return new this(options).start();
	}

	constructor(options = {}) {
		super();
		this.options = options;
		this.canvas = options.canvas ?? null;
		this.homePath = options.homePath ?? DEFAULT_HOME_PATH;
		const suffix = this.constructor.suffix;
		this.accept = options.accept ?? (suffix === null ? [] : [suffix]);
		this.module = null;
		this.exited = false;
		// Everything this puts on the page hangs on this signal.
		this.stopping = new AbortController();
		this.destroyed = false;
		this.pendingSink = null;
		this.finished = new Promise(resolve => {
			this.reportFinished = resolve;
		});
	}

	output(message, kind = {}) {
		if (kind.error) {
			console.error(message);
		} else {
			console.log(message);
		}
		this.dispatchEvent(new CustomEvent("output", { detail: { message, kind } }));
	}

	call(name, returnType = null, argTypes = [], args = []) {
		if (this.module === null || this.exited) {
			return null;
		}
		try {
			return this.module.ccall(name, returnType, argTypes, args);
		} catch (error) {
			return null;
		}
	}

	async loadFile(file) {
		const path = this.filePath(file.name);
		if (path === null) {
			throw fail("FileRefused", `${file.name} is not a kind of file this program takes`);
		}
		this.dispatchEvent(new CustomEvent("loadstart", { detail: { url: null } }));
		const filePath = this.writeFile(path, file.name, new Uint8Array(await file.arrayBuffer()));
		this.call("EmscriptenCallbackDropFile", null, ["string"], [filePath]);
		return filePath;
	}

	async loadUrl(url) {
		this.dispatchEvent(new CustomEvent("loadstart", { detail: { url: String(url) } }));
		const filePath = await this.fetchUrlFile(url);
		this.call("EmscriptenCallbackDropFile", null, ["string"], [filePath]);
		return filePath;
	}

	// Set from the click that asks for an export, which is the only moment a
	// browser lets a page ask where to save.
	setVideoSink(sink) {
		this.pendingSink = sink;
	}

	quit() {
		this.call("EmscriptenCallbackQuit");
	}

	destroy() {
		if (this.destroyed) {
			return;
		}
		this.destroyed = true;
		// A program that is still running stops its own sound as it quits.
		if (this.exited) {
			this.stopAudio();
		} else {
			this.quit();
		}
		this.stopping.abort();
		this.restoreErrorHandler();
		this.reportFinished();
	}

	// Command line arguments beyond the file; subclasses add their own.
	programArguments() {
		return this.options.arguments ?? [];
	}

	// Called once the program runs.
	running() {}

	writeFile(path, name, data) {
		const FS = this.module.FS;
		FS.mkdirTree(`${path}/upload`);
		const filePath = `${path}/upload/${sanitizeFilename(name)}`;
		FS.writeFile(filePath, data);
		return filePath;
	}

	// Where a file of this name belongs below the home directory, or `null`.
	filePath(name) {
		const suffix = this.accept.find(entry => name.endsWith(entry));
		return suffix === undefined ? null : `${this.homePath}/${suffix === ".demo" ? "demos" : "maps"}`;
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
		const name = decodeURIComponent(new URL(url, location.href).pathname.split("/").pop() || "download");
		const path = this.filePath(name) ?? this.filePath(`file${this.accept[0]}`) ?? this.homePath;
		return this.writeFile(path, name, new Uint8Array(buffer));
	}

	droppedItem(dataTransfer) {
		const files = [...dataTransfer.files].filter(file => this.filePath(file.name) !== null);
		if (files.length > 0) {
			return { file: files[0] };
		}
		const link = this.options.acceptLinks ? (dataTransfer.getData("text/uri-list") || dataTransfer.getData("text/plain")).trim() : "";
		return link.startsWith("ddnet://") ? { link } : {};
	}

	// Keys that go to the page instead of the program, such as Tab to move
	// the focus on.
	passesKey(event) {
		return false;
	}

	installCanvasHandlers() {
		const canvas = this.canvas;
		if (canvas === null) {
			return;
		}
		const signal = this.stopping.signal;
		const on = (type, listener) => canvas.addEventListener(type, listener, { signal });
		installKeyGuard(signal, event => this.passesKey(event));
		on("contextmenu", event => event.preventDefault());
		on("webglcontextcreationerror", event => {
			this.output(`Failed to create WebGL context: ${event.statusMessage || "Unknown error"}`, { error: true, bold: true });
		});
		on("webglcontextlost", event => {
			// Recovering would mean reloading every texture and framebuffer.
			this.output(`The WebGL context was lost: ${event.statusMessage || "Unknown error"}`, { error: true, bold: true });
			this.quit();
		});
		on("dragover", event => {
			event.preventDefault();
			event.dataTransfer.dropEffect = "copy";
		});
		on("drop", async event => {
			event.preventDefault();
			const dropped = this.droppedItem(event.dataTransfer);
			if (dropped.file) {
				await this.loadFile(dropped.file).catch(error => this.output(error.message, { error: true }));
			} else if (dropped.link) {
				this.call("EmscriptenCallbackDropFile", null, ["string"], [dropped.link]);
			} else {
				const kinds = this.accept.map(suffix => `${suffix} files`).join(" and ");
				this.output(`Only ${kinds}${this.options.acceptLinks ? " and ddnet:// links" : ""} can be dropped here.`, { error: true });
			}
		});
	}

	async videoSink(info) {
		const sink = this.pendingSink ?? this.options.videoSink;
		this.pendingSink = null;
		return sink ?? await videoScratchSink(info);
	}

	// The audio outlives the runtime unless it is stopped here.
	stopAudio() {
		// The tools' own output, see `src/engine/client/web/web_platform.js`.
		this.module?.ddnetStopAudio?.();
		const SDL2 = this.module?.SDL2;
		if (SDL2 === undefined) {
			return;
		}
		if (SDL2.audio !== undefined) {
			SDL2.audio.scriptProcessorNode?.disconnect();
			clearInterval(SDL2.audio.silenceTimer);
			SDL2.audio = undefined;
		}
		SDL2.audioContext?.close();
		SDL2.audioContext = undefined;
	}

	// An error out of the wasm leaves the runtime standing without calling
	// `onExit`. `self` rather than `window`, for a program in a worker.
	installErrorHandler() {
		const program = this;
		const previous = self.onerror;
		this.previousErrorHandler = previous;
		this.errorHandler = function(message, url, line, column, error) {
			program.output(message, { error: true, bold: true, fatal: true });
			for (const stackLine of (error?.stack ?? "").split("\n").filter(Boolean)) {
				program.output(stackLine, { error: true });
			}
			program.stopAudio();
			program.call("EmscriptenCallbackQuitForce");
			program.reportFinished();
			return previous ? previous.apply(this, arguments) : undefined;
		};
		self.onerror = this.errorHandler;
	}

	restoreErrorHandler() {
		if (this.errorHandler !== undefined && self.onerror === this.errorHandler) {
			self.onerror = this.previousErrorHandler;
		}
	}

	async start() {
		const options = this.options;
		const scriptUrl = options.scriptUrl ?? this.constructor.scriptUrl();
		const factory = options.module ?? (scriptUrl === null ? null : await importProgram(scriptUrl, this.constructor.moduleName));
		if (typeof factory !== "function") {
			throw fail("BadOption", "The program's factory is missing, for example `module: DDNetClient`");
		}
		if (options.signal) {
			if (options.signal.aborted) {
				throw abortError(options.signal);
			}
			options.signal.addEventListener("abort", () => this.quit(), { once: true, signal: this.stopping.signal });
		}
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

		const program = await this.program(scriptUrl);
		let totalDependencies = 0;
		this.module = await factory({
			websocket: { url: location.protocol === "https:" ? "wss://" : "ws://" },
			noInitialRun: true,
			canvas: this.canvas ?? undefined,
			mainScriptUrlOrBlob: program?.script,
			locateFile: program === null ? undefined : path => new URL(path, program.base).href,
			// Read by `src/engine/client/web/web_platform.js`: without zoom
			// the wheel scrolls the page.
			ddnetWheel: options.zoom !== false,
			// Read by `src/engine/client/viewer_fullscreen.cpp`.
			ddnetFullscreen: {
				supported: fullscreenSupported,
				active: () => fullscreenElement() !== null,
				toggle: () => toggleFullscreen(options.fullscreenElement),
			},
			// Read by `src/engine/client/video_webcodecs.cpp`. A page that
			// cancels the `video` event keeps the file, else it is offered as a
			// download.
			ddnetVideoOutput: (file, name) => !this.dispatchEvent(new CustomEvent("video", { detail: { file, name }, cancelable: true })),
			ddnetVideoSink: info => this.videoSink(info),
			// Read by `src/engine/client/demo_render_client.cpp`.
			ddnetRenderProgress: status => this.dispatchEvent(new CustomEvent("renderprogress", { detail: status })),
			// Read by `src/base/webfs.cpp`. A program from another origin brings
			// its data along.
			ddnetDataBase: options.dataBase ?? (program === null ? undefined : new URL(".", program.base).href),
			arguments: this.programArguments(),
			print: text => {
				const line = parseAnsiColorRgb(text);
				this.output(line.message, { color: line.color });
			},
			printErr: text => this.output(text, { error: true }),
			onExit: () => this.onExit(),
			monitorRunDependencies: left => {
				totalDependencies = Math.max(totalDependencies, left);
				const text = totalDependencies === 1 ? "Loading…" : `Loading… (${totalDependencies - left}/${totalDependencies})`;
				this.dispatchEvent(new CustomEvent("progress", { detail: { text } }));
			},
		});

		this.installCanvasHandlers();
		await this.mountPersistentStorage();

		const args = this.module.arguments;
		const path = await this.initialFile();
		if (path !== null) {
			if (options.fileArgument) {
				args.push(options.fileArgument);
			}
			args.push(path);
		}
		if (this.canvas !== null) {
			this.canvas.style.display = "block";
		}
		this.module.callMain(args);
		this.running();
		return this;
	}

	// A program from another origin cannot start threads from its own URL, so
	// it is fetched and handed to Emscripten as a blob.
	async program(scriptUrl) {
		if (!scriptUrl) {
			return null;
		}
		const base = new URL(scriptUrl, location.href);
		return { script: base.origin === location.origin ? base.href : await fetchScript(base), base };
	}

	async initialFile() {
		const file = this.options.file;
		if (file == null) {
			return null;
		}
		if (typeof file !== "string") {
			const name = this.options.fileName ?? file.name ?? "upload";
			const bytes = file instanceof Uint8Array ? file : new Uint8Array(file instanceof ArrayBuffer ? file : await file.arrayBuffer());
			return this.writeFile(this.filePath(name) ?? this.homePath, name, bytes);
		}
		const url = new URL(file, location.href);
		if (url.protocol !== "http:" && url.protocol !== "https:" && url.protocol !== "blob:") {
			this.output(`Refused to load ${url.protocol} URL`, { error: true, fatal: true });
			return null;
		}
		this.output(`Downloading ${url.href}…`);
		try {
			return await this.fetchUrlFile(url.href);
		} catch (error) {
			// A refused fetch is a bare TypeError, and almost always a server
			// that does not allow this page to read it.
			const reason = error instanceof TypeError && url.origin !== location.origin
				? `${url.origin} does not allow this page to read its files (no CORS headers)`
				: error.message;
			this.output(`Failed to download ${url.href}: ${reason}. You can drop the file into this page instead.`, { error: true, bold: true, fatal: true });
			return null;
		}
	}

	async mountPersistentStorage() {
		const FS = this.module.FS;
		FS.mkdirTree(this.homePath);
		if (this.options.persist === false) {
			return;
		}
		this.output("Synchronizing filesystem with IndexedDB…");
		FS.mount(this.module.IDBFS, {}, this.homePath);
		await new Promise(resolve => FS.syncfs(true, error => {
			if (error) {
				this.output(`Failed to synchronize filesystem with IndexedDB: ${error}`, { error: true, bold: true });
			}
			resolve();
		}));
	}

	onExit() {
		this.exited = true;
		this.reportFinished();
		if (this.options.persist !== false) {
			this.module.ddnetSyncPersistentStorage?.(true);
		}
		if (this.canvas !== null) {
			// The canvas stays black otherwise, and the cursor may stay hidden.
			this.canvas.style.display = "none";
			this.canvas.style.cursor = "default";
			document.body.style.cursor = "default";
		}
		this.output(`${this.options.programName ?? this.constructor.programName ?? "The program"} closed.`, { bold: true });
		this.dispatchEvent(new CustomEvent("exit"));
	}
}

const ELEMENT_STYLE = `
:host { display: block; position: relative; contain: content; background: #000; }
:host([hidden]) { display: none; }
canvas { display: block; width: 100%; height: 100%; background: #000; touch-action: none; outline: none; }
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
	pointer-events: none;
}
.message[hidden] { display: none; }
`;

// A worker has no `HTMLElement`, and the render worker imports this module.
const ELEMENT_BASE = typeof HTMLElement === "undefined" ? class {} : HTMLElement;

export class ViewerElement extends ELEMENT_BASE {
	static program = null;
	static bar = null;
	// Page address parameter to attribute, for the `link` attribute.
	static linkParams = {};
	// Attributes that `startOptions` hands to the program when it starts.
	static startAttributes = [];
	static programEvents = [];
	// What the program fires once a file is shown.
	static loadEvent = "load";
	static observedAttributes = ["src", "controls"];

	constructor() {
		super();
		const root = this.attachShadow({ mode: "open", delegatesFocus: true });
		const style = document.createElement("style");
		style.textContent = ELEMENT_STYLE;
		this.canvasElement = Object.assign(document.createElement("canvas"), { tabIndex: 0 });
		this.canvasElement.setAttribute("part", "picture");
		this.canvasElement.setAttribute("aria-label", this.constructor.program?.programName ?? "Viewer");
		this.messageElement = Object.assign(document.createElement("p"), { className: "message", hidden: true });
		this.messageElement.setAttribute("part", "message");
		this.messageElement.setAttribute("role", "status");
		const slot = Object.assign(document.createElement("slot"), { name: "controls", className: "over" });
		root.append(style, this.canvasElement, slot, this.messageElement);
		this.programInstance = null;
		this.barInstance = null;
		this.stopping = null;
		// Whether a program was handed the canvas, which is then its for good.
		this.canvasTaken = false;
		// The attributes the program was started with, applied again only
		// once a file replaces the first one.
		this.startedWith = [];
		this.ready = null;
	}

	get program() {
		return this.programInstance;
	}

	get bar() {
		return this.barInstance;
	}

	get picture() {
		return this.canvasElement;
	}

	async load(file) {
		const program = await this.ready;
		this.forgetLink();
		return await program.loadFile(file);
	}

	// Opens the browser's file chooser; call it from a click.
	open() {
		const input = Object.assign(document.createElement("input"), { type: "file", accept: this.constructor.program.suffix });
		input.addEventListener("change", () => {
			if (input.files.length > 0) {
				this.load(input.files[0]).catch(error => this.say(error.message));
			}
		}, { once: true });
		input.click();
	}

	say(message) {
		this.messageElement.textContent = message;
		this.messageElement.hidden = !message;
	}

	fail(message) {
		this.say(message);
		this.dispatchEvent(new CustomEvent("error", { detail: { message } }));
	}

	connectedCallback() {
		// Moving the element within the page disconnects and reconnects it.
		if (this.ready !== null) {
			return;
		}
		this.stopping = new AbortController();
		if (this.hasAttribute("link")) {
			this.readLink();
		}
		if (this.getAttribute("src")) {
			this.say("Loading…");
		}
		this.ready = this.startProgram();
		this.ready.catch(error => {
			if (!this.stopping?.signal.aborted) {
				this.fail(error?.message ?? String(error));
			}
		});
	}

	disconnectedCallback() {
		const program = this.programInstance;
		this.barInstance?.destroy();
		this.barInstance = null;
		this.programInstance = null;
		this.stopping?.abort();
		this.stopping = null;
		this.ready = null;
		program?.destroy();
	}

	attributeChangedCallback(name, before, value) {
		if (before !== value && this.programInstance !== null) {
			this.applyAttribute(name, value);
		}
	}

	// Options for the program from the attributes, so that it starts where
	// they say rather than seeking there after its first frames.
	startOptions() {
		return {};
	}

	async startProgram() {
		const options = this.startOptions();
		this.startedWith = this.constructor.startAttributes.filter(name => this.hasAttribute(name));
		// `base` moves the programs away from the module.
		const moved = this.getAttribute("base");
		const Class = moved === null ? this.constructor.program : class extends this.constructor.program {
			static base = new URL(moved, location.href).href;
		};
		const source = this.getAttribute("src");
		const signal = this.stopping.signal;
		const program = new Class({
			...options,
			canvas: this.takeCanvas(),
			controls: this.wantsProgramControls(),
			fullscreenElement: this,
			// An embedded viewer leaves nothing in the visitor's storage.
			persist: false,
			file: source || undefined,
			dataBase: this.getAttribute("data") ?? undefined,
			signal,
		});
		// Only the line that ended the program is shown; the rest, such as its
		// stack, goes to the console.
		program.addEventListener("output", event => {
			if (event.detail.kind.fatal) {
				this.fail(event.detail.message);
			}
		}, { signal });
		program.addEventListener(this.constructor.loadEvent, () => this.loaded(), { signal });
		program.addEventListener("loadstart", event => {
			if (event.detail.url === null) {
				this.forgetLink();
			}
		}, { signal });
		for (const type of this.constructor.programEvents) {
			program.addEventListener(type, event => this.dispatchEvent(new CustomEvent(type, { detail: event.detail })), { signal });
		}
		if (this.hasAttribute("link")) {
			for (const type of this.linkEvents()) {
				program.addEventListener(type, () => this.writeLink(), { signal });
			}
		}
		await program.start();
		if (signal.aborted) {
			program.destroy();
			return program;
		}
		this.programInstance = program;
		this.applyControlsKind();
		followSize(this.canvasElement, program, { signal });
		// A `src` set while the program was starting.
		const asked = this.getAttribute("src");
		if (asked && asked !== source) {
			this.applyAttribute("src", asked);
		}
		return program;
	}

	// The program hands the canvas to the thread it draws on, after which
	// nothing else can draw on it, so a program started again, such as after
	// the element moved, gets a new one.
	takeCanvas() {
		if (this.canvasTaken) {
			const canvas = this.canvasElement.cloneNode(false);
			this.canvasElement.replaceWith(canvas);
			this.canvasElement = canvas;
		}
		this.canvasTaken = true;
		return this.canvasElement;
	}

	loaded() {
		this.say("");
		const skip = this.startedWith;
		this.startedWith = [];
		for (const name of this.constructor.observedAttributes) {
			if (name !== "src" && name !== "controls" && !skip.includes(name) && this.hasAttribute(name)) {
				this.applyAttribute(name, this.getAttribute(name));
			}
		}
	}

	barOptions() {
		return {};
	}

	// The programs draw no controls of their own in a browser; the page's
	// are the only ones, whatever `controls` names.
	wantsProgramControls() {
		return false;
	}

	applyControlsKind() {
		const wanted = this.hasAttribute("controls") && this.constructor.bar !== null;
		if (wanted === (this.barInstance !== null)) {
			return;
		}
		if (wanted) {
			this.barInstance = new this.constructor.bar(this.programInstance, { ...this.barOptions(), container: this, picture: this, slot: "controls", signal: this.stopping.signal });
		} else {
			this.barInstance.destroy();
			this.barInstance = null;
		}
	}

	applyAttribute(name, value) {
		if (name === "src") {
			if (value) {
				this.say("Loading…");
				this.programInstance.loadUrl(value).catch(error => this.fail(error.message));
			}
		} else if (name === "controls") {
			this.programInstance.controls(this.wantsProgramControls());
			this.applyControlsKind();
		}
	}

	// The page address mirrors what is shown, see the `link` attribute.
	readLink() {
		const fragment = new URLSearchParams(location.hash.slice(1));
		for (const [parameter, attribute] of Object.entries(this.constructor.linkParams)) {
			const value = fragment.get(parameter);
			if (value) {
				this.setAttribute(attribute, value);
			}
		}
	}

	linkEvents() {
		return [];
	}

	linkValues() {
		return {};
	}

	writeLink() {
		if (this.programInstance !== null) {
			setUrlParameters(this.linkValues());
		}
	}

	// A file the user opened is not the one the link was about.
	forgetLink() {
		if (!this.hasAttribute("link")) {
			return;
		}
		const values = {};
		for (const [parameter, attribute] of Object.entries(this.constructor.linkParams)) {
			this.removeAttribute(attribute);
			values[parameter] = null;
		}
		setUrlParameters(values);
	}
}
