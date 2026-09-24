/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// The DDNet demo renderer: a demo in, an MP4 out, drawn without a window in a
// worker of its own. The API is documented in demo-renderer.d.ts.

import { abortError, DDNetBaseError, fetchScript, moduleUrl, supportError, sweepVideoScratch } from "@ddnet/base";

const PROGRAM = "ddnet-demo-render.js";
const MODULE_NAME = "DDNetDemoRenderer";

export const programUrl = new URL(PROGRAM, import.meta.url).href;

// A worker sees no import map, so it gets the base by URL.
const WORKER_SCRIPT = `
self.onmessage = async event => {
	const { baseUrl, scriptUrl, moduleName, settings, sink } = event.data;
	try {
		const base = await import(baseUrl);
		let video = null;
		const program = new base.Program({ ...settings, videoSink: sink, module: await base.importProgram(scriptUrl, moduleName) });
		program.addEventListener("output", event => self.postMessage({ type: "output", ...event.detail }));
		program.addEventListener("renderprogress", event => self.postMessage({ type: "progress", status: event.detail }));
		program.addEventListener("video", event => {
			video = event.detail.file;
			event.preventDefault();
		});
		await program.start();
		await program.finished;
		self.postMessage({ type: "done", video });
	} catch (error) {
		self.postMessage({ type: "failed", message: String(error?.message ?? error) });
	}
};
`;

// The video settings are the client's own `cl_video_*` variables, which the
// command line of `ddnet-demo-render` takes as console commands.
function renderArguments(options) {
	const args = ["--output", options.output ?? "video.mp4"];
	if (options.follow != null && options.follow !== "") {
		args.push("--follow", String(options.follow));
	}
	const settings = {
		cl_video_width: options.width,
		cl_video_height: options.height,
		cl_video_recorder_fps: options.fps,
		cl_video_codec: options.codec,
		cl_video_crf: options.crf,
		cl_video_preset: options.preset,
		cl_video_sound_enable: options.audio === undefined ? undefined : Number(options.audio),
		cl_video_showhud: options.hud === undefined ? undefined : Number(options.hud),
		cl_video_showchat: options.chat === undefined ? undefined : Number(options.chat),
	};
	for (const [name, value] of Object.entries(settings)) {
		if (value != null) {
			args.push(`${name} "${String(value).replace(/["\\]/g, "")}"`);
		}
	}
	return args.concat(options.settings ?? []);
}

export async function renderDemo(options) {
	if (typeof VideoEncoder === "undefined") {
		throw new DDNetBaseError("NoVideoEncoder", "This browser cannot encode video: it has no VideoEncoder.");
	}
	const problem = await supportError(true);
	if (problem !== null) {
		throw problem;
	}
	const signal = options.signal;
	if (signal?.aborted) {
		throw abortError(signal);
	}
	const base = new URL(moduleUrl, location.href);
	const program = new URL(options.scriptUrl ?? programUrl, location.href);
	// A cross-origin isolated page refuses foreign scripts in a worker, so
	// they go in as blobs of its own.
	const [baseScript, programScript] = await Promise.all([base, program].map(async url =>
		url.origin === location.origin ? url.href : URL.createObjectURL(await fetchScript(url))));
	const demo = options.demo;
	const settings = {
		arguments: renderArguments(options),
		canvas: null,
		persist: false,
		needsWebGpu: true,
		accept: [".demo"],
		// The worker's own address is a blob, so a relative URL is resolved here.
		file: typeof demo === "string" ? new URL(demo, location.href).href : demo,
		fileName: options.name ?? "render.demo",
		fileArgument: "--render-demo",
		programName: "The demo renderer",
		dataBase: options.dataBase === undefined ? undefined : new URL(options.dataBase, location.href).href,
		// The worker's program fetches itself again by its real address.
		scriptUrl: program.href,
		// Swept here, once per page: a worker of a later render would delete
		// the video of an earlier one.
		sweepVideoScratch: false,
	};
	await sweepVideoScratch();

	const workerUrl = URL.createObjectURL(new Blob([WORKER_SCRIPT], { type: "text/javascript" }));
	const worker = new Worker(workerUrl, { type: "module" });
	URL.revokeObjectURL(workerUrl);
	const finished = new Promise((resolve, reject) => {
		signal?.addEventListener("abort", () => {
			worker.terminate();
			reject(abortError(signal));
		}, { once: true });
		worker.onmessage = event => {
			const message = event.data;
			if (message.type === "output") {
				options.onOutput?.(message.message, message.kind);
			} else if (message.type === "progress") {
				options.onProgress?.(message.status);
			} else {
				worker.terminate();
				if (message.type === "done" && (message.video !== null || options.videoSink)) {
					resolve(message.video);
				} else {
					reject(new DDNetBaseError("RenderFailed", message.message ?? "The demo was not rendered into a video, see the output for what went wrong."));
				}
			}
		};
		worker.onerror = event => {
			worker.terminate();
			reject(new DDNetBaseError("RenderFailed", event.message || "The render worker stopped."));
		};
		const sink = options.videoSink;
		const stream = sink?.stream ?? sink;
		worker.postMessage({ baseUrl: baseScript, scriptUrl: programScript, moduleName: MODULE_NAME, settings, sink }, stream ? [stream] : []);
	});
	const dropScripts = () => {
		for (const url of [baseScript, programScript]) {
			if (url.startsWith("blob:")) {
				URL.revokeObjectURL(url);
			}
		}
	};
	finished.then(dropScripts, dropScripts);
	return await finished;
}

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

// A stored (uncompressed) zip: videos are compressed already. Without ZIP64,
// so everything has to fit in 32 bits.
export async function zip(entries) {
	const total = entries.reduce((sum, entry) => sum + entry.blob.size, 0);
	if (total >= 0xffffffff) {
		throw new DDNetBaseError("ZipTooLarge", "Too much to put into one zip file");
	}
	const encoder = new TextEncoder();
	const now = new Date();
	const time = ((now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1)) & 0xffff;
	const date = (((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate()) & 0xffff;
	const parts = [];
	const central = [];
	let offset = 0;
	for (const entry of entries) {
		const name = encoder.encode(entry.name);
		const bytes = new Uint8Array(await entry.blob.arrayBuffer());
		const crc = crc32(bytes);
		// The fields both headers share, from "version needed" to the name length.
		const common = view => {
			view.setUint16(0, 20, true);
			view.setUint16(2, 0x0800, true); // UTF-8 names
			view.setUint16(4, 0, true); // stored
			view.setUint16(6, time, true);
			view.setUint16(8, date, true);
			view.setUint32(10, crc, true);
			view.setUint32(14, bytes.length, true);
			view.setUint32(18, bytes.length, true);
			view.setUint16(22, name.length, true);
		};
		const local = new DataView(new ArrayBuffer(30));
		local.setUint32(0, 0x04034b50, true);
		common(new DataView(local.buffer, 4));
		parts.push(local.buffer, name, bytes);
		const directory = new DataView(new ArrayBuffer(46));
		directory.setUint32(0, 0x02014b50, true);
		directory.setUint16(4, 20, true);
		common(new DataView(directory.buffer, 6));
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
