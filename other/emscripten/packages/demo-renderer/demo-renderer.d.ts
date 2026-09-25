import type { OutputKind, VideoSettings, VideoSink } from "@ddnet/base";

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
	/** Console commands run before the render. */
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
 * Renders a demo into an MP4 without a window, with the demo player's program.
 * Answers the video, or `null` when it went to `videoSink`. Needs WebCodecs,
 * and draws with WebGPU or, where the browser has no adapter, WebGL 2.
 */
export declare function renderDemo(options: RenderOptions): Promise<File | Blob | null>;

/** One uncompressed zip of several videos, below 4 GiB. */
export declare function zip(entries: { name: string; blob: Blob }[]): Promise<Blob>;

/** Where the program's script is, for a page that preloads it. */
export declare const programUrl: string;
