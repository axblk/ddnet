/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * The DDNet demo renderer, as one module: a demo goes in and an MP4 comes out.
 * Nothing is shown while it runs - the game is drawn into a surface without a
 * window, as fast as the machine manages, and every frame is handed to the
 * browser's own encoder.
 *
 * ```js
 * import { renderDemo } from "@ddnet/demo-renderer";
 *
 * const video = await renderDemo({
 *     demo: "https://…/a.demo",
 *     width: 1920, height: 1080, fps: 60,
 *     onRenderProgress: status => console.log(status.progress),
 * });
 * ```
 *
 * It needs WebGPU, because that is the one way a browser draws without a
 * window, and a `VideoEncoder`. Both are said to be missing before a demo is
 * fetched for nothing.
 *
 * By itself it runs in a worker of its own, so the page it was asked from goes
 * on drawing while a film is written. `worker: false` keeps it on the page's
 * own thread, which is worth it for nothing but debugging.
 *
 * The program itself is WebAssembly and lives beside this file; nothing here
 * has to be told where it is. What every program of this family needs is
 * `@ddnet/base`, the runtime this is built on.
 */

import DDNetBase, {
	abortError, checkOptions, DDNetBaseError, fetchScript, moduleUrl, Program,
	supportError, sweepVideoScratch,
} from "@ddnet/base";

/** The program, and what its script calls the factory it defines. */
const PROGRAM = "ddnet-demo-render.js";
const MODULE_NAME = "DDNetDemoRenderer";
const PROGRAM_NAME = "The demo renderer";
const SUFFIX = ".demo";

/** Where the program's script is, for a page that wants to fetch it early. */
export const programUrl = new URL(PROGRAM, import.meta.url).href;

/** What `renderDemo` takes, so that a mistyped `fsp` is heard of here. */
const RENDER_OPTIONS = [
	// tidy-alphabetical-start
	"arguments", "audio", "chat", "codec", "crf", "dataBase", "demo", "follow",
	"fps", "height", "homePath", "hud", "module", "moduleName", "name",
	"onOutput", "onProgress", "onRenderProgress", "onStart", "output",
	"preset", "programName", "scriptUrl", "settings", "signal",
	"sweepVideoScratch", "videoSink", "width", "worker",
	// tidy-alphabetical-end
];

// A render is a worker's worth of work - every frame drawn, read back and
// encoded - and none of it needs the page. Done in a worker, the page stays
// answerable while it happens, which is the whole point of a render nobody is
// watching. This is what that worker runs.
//
// It imports the base by address rather than by name, and it has to: a worker
// sees no import map, so `@ddnet/base` means nothing in here. That is also why
// the worker does not import this module - what to ask the program is worked
// out on the page, where this module already is, and what arrives here is a
// program to start and the settings to start it with.
const WORKER_BOOTSTRAP = `
self.onmessage = async event => {
	const request = event.data;
	try {
		const base = await import(request.baseUrl);
		let video = null;
		const program = await base.Program.open(Object.assign({}, request.settings, {
			module: await base.importProgram(request.scriptUrl, request.moduleName),
			videoSink: request.sink,
			onVideo: file => { video = file; },
			onOutput: (message, kind) => self.postMessage({type: "output", message: message, kind: kind}),
			onRenderProgress: status => self.postMessage({type: "progress", status: status}),
		}));
		await program.finished;
		self.postMessage({type: "done", video: video});
	} catch (error) {
		self.postMessage({type: "failed", message: String((error && error.message) || error)});
	}
};
`;

// Everything about a render that is data rather than a promise to call back:
// what survives being sent to a worker.
const WORKER_SETTINGS = [
	// tidy-alphabetical-start
	"accept", "arguments", "dataBase", "file", "fileArgument", "fileName",
	"homePath", "needsWebGpu", "persist", "programName", "scriptUrl",
	"sweepVideoScratch",
	// tidy-alphabetical-end
];

/**
 * What a render hands to whoever wants to watch it: the same events the page
 * gets as callbacks, and the way to stop it. A render on the page's own thread
 * hands over the program itself, which answers to the same three words.
 */
class CRenderHandle extends EventTarget {
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

/**
 * The demo renderer: the base, told where its program lies and how a render is
 * asked for. Everything a page says about a video it says in the words the
 * render tool's command line uses, because they are the same words - what
 * `ddnet-demo-render --help` documents documents these too.
 */
class CDemoRenderer extends Program {
	static script = PROGRAM;
	static base = import.meta.url;
	static moduleName = MODULE_NAME;
	static programName = PROGRAM_NAME;
	static suffix = SUFFIX;

	/**
	 * Writes a video of a demo.
	 *
	 * @param options.demo The demo to render: bytes, a `File`, or the URL of
	 * one.
	 * @param options.name The demo's file name, when it comes as bytes.
	 * @param options.output The name the video carries, `video.mp4` otherwise.
	 * @param options.width Video width in pixels, even, `cl_video_width`
	 * otherwise; `height`, `fps`, `codec`, `crf` and `preset` the same way.
	 * @param options.audio `false` renders without a sound track.
	 * @param options.hud `true` shows the ingame interface, `options.chat`
	 * `false` hides the chat.
	 * @param options.follow Whose shoulder to watch over, for a demo a server
	 * recorded: it has nobody who recorded it, so without this the camera
	 * stands still.
	 * @param options.settings Console commands, one per entry.
	 * @param options.dataBase Where the `data` directory is, if it is not next
	 * to the page.
	 * @param options.onOutput Called for every line the render writes.
	 * @param options.onRenderProgress Called once a second while the render
	 * runs, with `{progress, encodedFrames, submittedFrames, framesPerSecond}`
	 * - `progress` is the part of the demo that is done, between 0 and 1.
	 * @param options.onStart Called with the running render, whose `quit` ends
	 * one that is taking too long.
	 * @param options.worker `false` renders on the page's own thread, which is
	 * worth it for nothing but debugging.
	 * @param options.scriptUrl Where `ddnet-demo-render.js` is, if it is not
	 * beside this module.
	 * @param options.videoSink Where the video is written while it is made. A
	 * `WritableStream` can go to the worker with it; a function cannot, and is
	 * only asked on the page's own thread.
	 * @param options.signal An `AbortSignal`. Aborting it stops the render and
	 * ends the promise with whatever the signal was aborted with; a render
	 * that is stopped without one ends with `RenderStopped`.
	 *
	 * What `onStart` is handed is an `EventTarget` as well: `output` and
	 * `renderprogress` are the same two things the callbacks say, and `quit()`
	 * stops it.
	 *
	 * @returns a promise for the finished MP4 as a `Blob`, or for `null` where
	 * the page said where to write it itself.
	 */
	static async render(options) {
		checkOptions("renderDemo", options, RENDER_OPTIONS);
		// Encoding is the browser's to do, and a browser without an encoder is
		// worth saying so before a demo is fetched and a program started for
		// nothing.
		if (typeof VideoEncoder === "undefined") {
			throw new DDNetBaseError("NoVideoEncoder", "This browser cannot encode video: it has no VideoEncoder. Chrome, Edge and a current Firefox or Safari have one.");
		}
		if (options.worker !== false && typeof Worker === "function") {
			return await this.renderInWorker(options);
		}
		let video = null;
		const program = await this.open(Object.assign(this.programSettings(options), {
			onVideo: file => {
				video = file;
			},
		}));
		if (options.onStart) {
			options.onStart(program);
		}
		await program.finished;
		// A render that was called off is not a render that failed, and
		// whoever called it off is told so in their own words.
		if (options.signal && options.signal.aborted) {
			throw abortError(options.signal);
		}
		if (video == null) {
			throw new DDNetBaseError("RenderFailed", "The demo was not rendered into a video, see the output for what went wrong");
		}
		return video;
	}

	/**
	 * What the program is started with, from what the page asked for: the same
	 * arguments the render tool is given on a command line, and the few things
	 * every program here takes.
	 */
	static programSettings(options) {
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
		return {
			arguments: args.concat(options.arguments || []),
			// Nothing is shown, so there is nothing to show it on and nothing
			// to keep afterwards - and drawing without a window is the one
			// thing that needs WebGPU.
			canvas: null,
			persist: false,
			needsWebGpu: true,
			accept: [SUFFIX],
			file: options.demo,
			fileName: options.name || "render.demo",
			fileArgument: "--render-demo",
			programName: options.programName || PROGRAM_NAME,
			dataBase: options.dataBase,
			homePath: options.homePath,
			module: options.module,
			scriptUrl: options.scriptUrl,
			signal: options.signal,
			sweepVideoScratch: options.sweepVideoScratch,
			videoSink: options.videoSink,
			onOutput: options.onOutput,
			onProgress: options.onProgress,
			onRenderProgress: options.onRenderProgress,
		};
	}

	/** The same render, in a worker, so that the page stays answerable. */
	static async renderInWorker(options) {
		// A worker inherits the page's isolation, so what the page cannot do
		// the worker cannot either - and it is said here, where the page is
		// listening, rather than from inside the worker.
		const problem = await supportError(true);
		if (problem !== null) {
			throw problem;
		}
		const base = new URL(moduleUrl, location.href);
		const program = new URL(options.scriptUrl || this.scriptUrl(), location.href);
		// Both scripts go in as blobs where they are not this page's own: a
		// page that is cross-origin isolated refuses a foreign script unless
		// its server allows it by name, and a blob of our own is nobody's
		// foreign script.
		const [baseScript, programScript] = await Promise.all([base, program].map(async url =>
			url.origin === location.origin ? url.href : URL.createObjectURL(await fetchScript(url))));
		const request = {
			baseUrl: baseScript,
			scriptUrl: programScript,
			moduleName: this.moduleName,
			settings: {},
			sink: undefined,
		};
		const settings = this.programSettings(options);
		for (const key of WORKER_SETTINGS) {
			if (settings[key] !== undefined) {
				request.settings[key] = settings[key];
			}
		}
		// The worker fetches the program itself, so what it is told is where
		// the program really is rather than the blob it imported.
		request.settings.scriptUrl = program.href;
		// A worker made from a blob has a blob for an address, and nothing can
		// be resolved against one of those. What a relative address is relative
		// to is the page, so it is made absolute while the page is still the one
		// asking.
		if (typeof settings.file === "string") {
			request.settings.file = new URL(settings.file, location.href).href;
		}
		// A blob outlives the worker that was made to import it unless somebody
		// lets go of it, and a page that renders one demo after another would
		// keep every script it ever handed over. The worker has imported both
		// by the time the render is over, however it ended.
		const dropScripts = () => {
			for (const url of [baseScript, programScript]) {
				if (url.startsWith("blob:")) {
					URL.revokeObjectURL(url);
				}
			}
		};
		// Swept on the page, where there is one of these per page, rather than
		// in the worker, where there is one per render: the second render of a
		// batch would otherwise sweep away the video of the first one while
		// the page was still offering it.
		await sweepVideoScratch();
		request.settings.sweepVideoScratch = false;
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
		const handle = new CRenderHandle(() => stopRender());
		const finished = new Promise((resolve, reject) => {
			stopRender = (reason) => {
				worker.terminate();
				reject(reason !== undefined ? reason : new DDNetBaseError("RenderStopped", "The render was stopped."));
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
					reject(new DDNetBaseError("RenderFailed", message.message));
				}
			};
			worker.onerror = event => {
				worker.terminate();
				reject(new DDNetBaseError("RenderFailed", event.message || "the render worker stopped"));
			};
			worker.postMessage(request, transfer);
		});
		finished.then(dropScripts, dropScripts);
		if (options.onStart) {
			options.onStart(handle);
		}
		return await finished;
	}
}

// A zip of files that are already in memory, so that a batch of them is one
// thing to save rather than one prompt each. Written by hand because it is
// short: stored, never deflated - a video is compressed already and squeezing
// it again would only cost time - and that leaves headers, a central directory
// and a checksum per file.
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

/**
 * Puts files that are in memory into one zip file, so that a batch of videos
 * is one thing to save rather than one prompt each. Nothing is compressed -
 * what this is for is videos, which are compressed already.
 *
 * @param entries `{name, blob}` objects, in the order they should be in.
 *
 * @return A promise of the zip as a `Blob`. It rejects with `ZipTooLarge` when
 * the whole of it would not fit in the 32 bits a plain zip counts in.
 */
export async function zip(entries) {
	// A zip says its sizes and offsets in 32 bits. Past that it takes the
	// ZIP64 records, which is a second format to write and to get wrong for
	// something nobody should be downloading in one piece anyway.
	const total = entries.reduce((sum, entry) => sum + entry.blob.size, 0);
	if (total >= 0xffffffff || entries.some(entry => entry.blob.size >= 0xffffffff)) {
		throw new DDNetBaseError("ZipTooLarge", "Too much to put into one zip file");
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

/**
 * Writes a video of a demo. The same as `DemoRenderer.render`, which is where
 * it is written down, under the name a page that renders one demo wants to
 * read.
 */
export function renderDemo(options) {
	return CDemoRenderer.render(options);
}

/** The renderer itself, for a page that would rather say it that way. */
export const DemoRenderer = CDemoRenderer;

export default {
	DemoRenderer,
	renderDemo,
	zip,
	programUrl,
	// The base this is built on, so that a page that has this has the rest of
	// it too without a second import.
	base: DDNetBase,
};
