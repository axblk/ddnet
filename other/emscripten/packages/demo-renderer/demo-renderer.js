/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// The DDNet demo renderer: a demo in, an MP4 out, drawn without a window. The
// program is the demo player's, which renders without a page when it is given
// `--render-demo`. The API is documented in demo-renderer.d.ts.

import { abortError, DDNetBaseError, importProgram, Program, supportError, sweepVideoScratch } from "@ddnet/base";

const PROGRAM = "ddnet-demo-player.js";
const MODULE_NAME = "DDNetDemoPlayer";

export const programUrl = new URL(PROGRAM, import.meta.url).href;

// The video settings are the client's own `cl_video_*` variables, which the
// command line of the render takes as console commands.
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
	// WebGPU where the browser has an adapter, WebGL 2 where it does not.
	const problem = await supportError();
	if (problem !== null) {
		throw problem;
	}
	const signal = options.signal;
	if (signal?.aborted) {
		throw abortError(signal);
	}
	const scriptUrl = new URL(options.scriptUrl ?? programUrl, location.href).href;
	const demo = options.demo;
	await sweepVideoScratch();
	// The program runs on this page's thread, which it never makes wait, and
	// not in a worker of its own: its threads would then be workers a worker
	// starts, and Firefox leaves a worker that starts one hanging for good
	// now and then while something watches workers (the developer tools,
	// WebDriver BiDi).
	const program = new Program({
		module: await importProgram(scriptUrl, MODULE_NAME),
		scriptUrl,
		arguments: renderArguments(options),
		canvas: null,
		persist: false,
		accept: [".demo"],
		file: typeof demo === "string" ? new URL(demo, location.href).href : demo,
		fileName: options.name ?? "render.demo",
		fileArgument: "--render-demo",
		programName: "The demo renderer",
		dataBase: options.dataBase === undefined ? undefined : new URL(options.dataBase, location.href).href,
		videoSink: options.videoSink,
		// Quits the program.
		signal,
	});
	let video = null;
	program.addEventListener("output", event => options.onOutput?.(event.detail.message, event.detail.kind));
	program.addEventListener("renderprogress", event => options.onProgress?.(event.detail));
	program.addEventListener("video", event => {
		video = event.detail.file;
		event.preventDefault();
	});
	const started = program.start();
	const aborted = new Promise((resolve, reject) => {
		signal?.addEventListener("abort", () => reject(abortError(signal)), { once: true });
	});
	// An abort after the end is nobody's business.
	aborted.catch(() => {});
	try {
		await Promise.race([started.then(() => program.finished), aborted]);
	} catch (error) {
		if (signal?.aborted) {
			// A program that was still starting is quit once it runs.
			started.then(() => program.destroy(), () => {});
			throw abortError(signal);
		}
		throw error instanceof DDNetBaseError ? error : new DDNetBaseError("RenderFailed", String(error?.message ?? error));
	}
	program.destroy();
	if (video === null && !options.videoSink) {
		throw new DDNetBaseError("RenderFailed", "The demo was not rendered into a video, see the output for what went wrong.");
	}
	return video;
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
