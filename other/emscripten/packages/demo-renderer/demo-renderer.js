/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// The DDNet demo renderer: a demo in, an MP4 out, drawn without a window. The
// program is the demo player's, which renders without a page when it is given
// `--render-queue`: it waits for the demos this module hands it, see
// `DemoRenderer*` in `src/engine/client/demo_render_client.cpp`. The API is
// documented in demo-renderer.d.ts.

import { abortError, DDNetBaseError, importProgram, Program, supportError, sweepVideoScratch } from "@ddnet/base";

const PROGRAM = "ddnet-demo-player.js";
const MODULE_NAME = "DDNetDemoPlayer";

export const programUrl = new URL(PROGRAM, import.meta.url).href;

// The video settings are the client's own `cl_video_*` variables, which the
// renderer takes as console commands.
function renderCommands(options) {
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
		gfx_high_detail: options.highDetail === undefined ? undefined : Number(options.highDetail),
		cl_nameplates: options.nameplates === undefined ? undefined : Number(options.nameplates),
		cl_nameplates_own: options.nameplates === undefined ? undefined : Number(options.nameplates),
		// Everybody's, including whoever recorded the demo.
		cl_video_show_direction: options.keyPresses === undefined ? undefined : options.keyPresses ? 2 : 0,
	};
	const commands = [];
	for (const [name, value] of Object.entries(settings)) {
		if (value != null) {
			commands.push(`${name} "${String(value).replace(/["\\]/g, "")}"`);
		}
	}
	// A line is one command; what a page passes is not split any further.
	return commands.concat((options.settings ?? []).map(command => String(command).replace(/[\r\n]/g, " ")));
}

// How long a finished render may take to hand over its video.
const VIDEO_HANDOVER_MS = 30000;

// One program that renders demo after demo: it starts once, and every render
// after the first begins without loading the game again. Renders and
// questions wait for each other in the order they were asked.
export class DemoRenderer {
	constructor(options = {}) {
		this.options = options;
		this.program = null;
		this.starting = null;
		this.closed = false;
		this.nextId = 1;
		// The job the program works on; the program does one at a time.
		this.current = null;
		this.queue = Promise.resolve();
	}

	get started() {
		return this.program !== null;
	}

	start() {
		this.starting ??= this.startProgram();
		return this.starting;
	}

	async startProgram() {
		if (typeof VideoEncoder === "undefined") {
			throw new DDNetBaseError("NoVideoEncoder", "This browser cannot encode video: it has no VideoEncoder.");
		}
		// WebGPU where the browser has an adapter, WebGL 2 where it does not.
		const problem = await supportError();
		if (problem !== null) {
			throw problem;
		}
		const options = this.options;
		const scriptUrl = new URL(options.scriptUrl ?? programUrl, location.href).href;
		await sweepVideoScratch();
		// The program runs on this page's thread, which it never makes wait, and
		// not in a worker of its own: its threads would then be workers a worker
		// starts, and Firefox leaves a worker that starts one hanging for good
		// now and then while something watches workers (the developer tools,
		// WebDriver BiDi).
		const program = new Program({
			module: await importProgram(scriptUrl, MODULE_NAME),
			scriptUrl,
			arguments: ["--render-queue"].concat(options.settings ?? []),
			canvas: null,
			persist: false,
			accept: [".demo"],
			programName: "The demo renderer",
			dataBase: options.dataBase === undefined ? undefined : new URL(options.dataBase, location.href).href,
		});
		program.addEventListener("output", event => {
			options.onOutput?.(event.detail.message, event.detail.kind);
			this.current?.onOutput?.(event.detail.message, event.detail.kind);
		});
		program.addEventListener("renderprogress", event => this.current?.onProgress?.(event.detail));
		program.addEventListener("video", event => {
			const job = this.current;
			if (job !== null) {
				event.preventDefault();
				job.video = event.detail.file;
				job.videoArrived();
			}
		});
		program.addEventListener("renderdone", event => {
			if (this.current?.id === event.detail.id) {
				this.current.finish(event.detail);
			}
		});
		program.finished.then(() => {
			this.program = null;
			this.current?.finish({ ok: false, error: "The renderer stopped." });
		});
		if (this.closed) {
			throw new DDNetBaseError("RendererClosed", "The renderer was closed.");
		}
		await program.start();
		this.program = program;
		return program;
	}

	// Jobs run one after the other; a failed one does not hold up the next.
	enqueue(work) {
		const run = this.queue.then(work);
		this.queue = run.catch(() => {});
		return run;
	}

	render(options) {
		return this.enqueue(() => this.runJob(options, true));
	}

	info(demo, options = {}) {
		return this.enqueue(() => this.runJob({ ...options, demo }, false));
	}

	close() {
		this.closed = true;
		this.program?.destroy();
		this.program = null;
	}

	// The demo goes into the program's files for as long as the job runs.
	async writeDemo(program, demo, name) {
		if (typeof demo === "string") {
			const url = new URL(demo, location.href).href;
			return program.fetchUrlFile(url);
		}
		const bytes = demo instanceof Uint8Array ? demo : new Uint8Array(demo instanceof ArrayBuffer ? demo : await demo.arrayBuffer());
		return program.writeFile(program.filePath(name) ?? program.homePath, name, bytes);
	}

	async runJob(options, render) {
		const signal = options.signal;
		if (signal?.aborted) {
			throw abortError(signal);
		}
		if (this.closed) {
			throw new DDNetBaseError("RendererClosed", "The renderer was closed.");
		}
		const program = this.program ?? await this.start();
		if (signal?.aborted) {
			throw abortError(signal);
		}
		const demo = options.demo;
		const name = options.name ?? (typeof demo === "string" ? decodeURIComponent(new URL(demo, location.href).pathname.split("/").pop() || "") : demo?.name) ?? "render.demo";
		const path = await this.writeDemo(program, demo, name.endsWith(".demo") ? name : `${name}.demo`);
		const id = this.nextId++;
		const job = { id, onProgress: options.onProgress, onOutput: options.onOutput, video: null };
		const done = new Promise(resolve => {
			job.finish = resolve;
		});
		const videoArrived = new Promise(resolve => {
			job.videoArrived = resolve;
		});
		const cancel = () => program.call("DemoRendererCancel", null, ["number"], [id]);
		signal?.addEventListener("abort", cancel, { once: true });
		this.current = job;
		let result;
		try {
			if (render) {
				// The sink is asked for once the video starts, which is after
				// the job was handed over.
				program.setVideoSink(options.videoSink ?? null);
				const follow = options.follow == null ? "" : String(options.follow);
				program.call("DemoRendererRender", null, ["number", "string", "string", "string", "string"],
					[id, path, options.output ?? "video.mp4", follow, renderCommands(options).join("\n")]);
			} else {
				program.call("DemoRendererInfo", null, ["number", "string"], [id, path]);
			}
			result = await done;
			// The video is handed over as the file is closed, which may come
			// just after the job said it was done.
			if (render && result.ok && !options.videoSink && job.video === null) {
				await Promise.race([videoArrived, new Promise(resolve => setTimeout(resolve, VIDEO_HANDOVER_MS))]);
			}
		} finally {
			signal?.removeEventListener("abort", cancel);
			if (this.current === job) {
				this.current = null;
			}
			program.setVideoSink(null);
			try {
				program.module?.FS.unlink(path);
			} catch (error) {
				// The program is gone, and its files with it.
			}
		}
		if (!result.ok) {
			if (signal?.aborted) {
				throw abortError(signal);
			}
			throw new DDNetBaseError("RenderFailed", result.error || "The demo was not rendered, see the output for what went wrong.");
		}
		if (!render) {
			return JSON.parse(result.info || "{}");
		}
		if (job.video === null && !options.videoSink) {
			throw new DDNetBaseError("RenderFailed", "The demo was not rendered into a video, see the output for what went wrong.");
		}
		return job.video;
	}
}

// What a demo's header says, read by the page without the program: enough to
// list a demo before anything was rendered. See `CDemoHeader` in
// `src/engine/demo.h`; the numbers are big-endian.
const DEMO_HEADER_SIZE = 176;
export async function readDemoHeader(demo) {
	const blob = demo instanceof Blob ? demo : new Blob([demo]);
	const bytes = new Uint8Array(await blob.slice(0, DEMO_HEADER_SIZE + 4).arrayBuffer());
	if (bytes.length < DEMO_HEADER_SIZE) {
		return null;
	}
	const decoder = new TextDecoder();
	const text = (start, length) => {
		const field = bytes.subarray(start, start + length);
		const end = field.indexOf(0);
		return decoder.decode(end < 0 ? field : field.subarray(0, end));
	};
	if (text(0, 7) !== "TWDEMO") {
		return null;
	}
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const version = bytes[7];
	return {
		size: blob.size,
		version,
		netversion: text(8, 64),
		type: text(144, 8),
		date: text(156, 20),
		// Whole seconds in the header.
		length: view.getInt32(152) * 1000,
		map: { name: text(72, 64), size: view.getUint32(136), crc: view.getUint32(140).toString(16).padStart(8, "0") },
		// Only how many: where they are counts from a tick the header does
		// not have.
		markerCount: version >= 4 && bytes.length >= DEMO_HEADER_SIZE + 4 ? Math.max(0, view.getInt32(DEMO_HEADER_SIZE)) : 0,
	};
}

// One demo with a renderer of its own, which is gone afterwards.
export async function renderDemo(options) {
	const renderer = new DemoRenderer({ scriptUrl: options.scriptUrl, dataBase: options.dataBase });
	try {
		return await renderer.render(options);
	} finally {
		renderer.close();
	}
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
