import type { DemoInfo, OutputKind, VideoSettings, VideoSink } from "@ddnet/base";

/** What a renderer is started with, the same for every demo it renders. */
export interface DemoRendererOptions {
	/** Where `ddnet-demo-player.js` is, beside this module by default. */
	scriptUrl?: string;
	/** Where `data` is, beside the script by default. */
	dataBase?: string;
	/** Console commands run once, before the first demo. */
	settings?: string[];
	/** Everything the program says, whichever demo it is at. */
	onOutput?: (message: string, kind: OutputKind) => void;
}

export interface RenderOptions extends VideoSettings {
	/** The demo: its URL or its content. */
	demo: string | File | Blob | ArrayBuffer | Uint8Array;
	/** The demo's file name, for content. */
	name?: string;
	/** The video's file name, `video.mp4` by default. */
	output?: string;
	/** Whom the video follows, by name; a server demo needs somebody. */
	follow?: string;
	/** The encoder preset. */
	preset?: string;
	/** Console commands run before the render, for this demo alone. */
	settings?: string[];
	/** Where the video is written; kept in memory and answered otherwise. */
	videoSink?: VideoSink | WritableStream;
	/** Where `ddnet-demo-player.js` is, beside this module by default. */
	scriptUrl?: string;
	/** Where `data` is, beside the script by default. */
	dataBase?: string;
	signal?: AbortSignal;
	onOutput?: (message: string, kind: OutputKind) => void;
	onProgress?: (status: { progress: number; encodedFrames: number; submittedFrames: number; framesPerSecond: number }) => void;
}

/**
 * A program that renders demos into MP4s without a window, one after the
 * other: it starts with the first job, and every render after that begins
 * without loading the game again. Each demo is rendered as if it were the
 * only one - the settings of one do not reach the next. Needs WebCodecs, and
 * draws with WebGPU or, where the browser has no adapter, WebGL 2.
 */
export declare class DemoRenderer {
	constructor(options?: DemoRendererOptions);
	/** Whether the program runs. */
	readonly started: boolean;
	/** Starts the program ahead of the first job, which does it otherwise. */
	start(): Promise<unknown>;
	/**
	 * Renders a demo once the jobs before it are done. Answers the video, or
	 * `null` when it went to `videoSink`. `signal` cancels it, running or
	 * waiting, and throws away what was written.
	 */
	render(options: Omit<RenderOptions, "scriptUrl" | "dataBase">): Promise<File | Blob | null>;
	/** What a demo is: its header, its map, and the players at its beginning. */
	info(demo: RenderOptions["demo"], options?: { name?: string; signal?: AbortSignal }): Promise<DemoInfo>;
	/** Quits the program; what runs is stopped and what waits fails. */
	close(): void;
}

/**
 * Renders one demo with a renderer of its own, see `DemoRenderer.render`.
 * Answers the video, or `null` when it went to `videoSink`.
 */
export declare function renderDemo(options: RenderOptions): Promise<File | Blob | null>;

/**
 * What a demo's header says, read without the program, or `null` for what is
 * no demo. The length is in whole seconds, as milliseconds; of the markers
 * only how many there are.
 */
export declare function readDemoHeader(demo: Blob | ArrayBuffer | Uint8Array): Promise<(Omit<DemoInfo, "markers" | "players"> & { markerCount: number }) | null>;

/** One uncompressed zip of several videos, below 4 GiB. */
export declare function zip(entries: { name: string; blob: Blob }[]): Promise<Blob>;

/** Where the program's script is, for a page that preloads it. */
export declare const programUrl: string;
