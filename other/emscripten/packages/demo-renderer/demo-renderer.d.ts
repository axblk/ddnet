/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

import type {
	OutputKind, Program, ProgramFactory, RenderProgress, VideoSettings,
	VideoSinkSource,
} from "@ddnet/base";

/** Where the program's script is, for a page that wants to fetch it early. */
export declare const programUrl: string;

/** What a render takes: the words the render tool's command line uses. */
export interface RenderOptions extends VideoSettings {
	/** The demo to render: bytes, a `File`, or a URL. */
	demo: File | Blob | ArrayBuffer | Uint8Array | string;
	/** What to call the demo, when it comes as bytes. */
	name?: string;
	/** What the video is called, `video.mp4` otherwise. */
	output?: string;
	/** Whose shoulder to watch over, by name. */
	follow?: string;
	preset?: string;
	/** Console commands, one per entry. */
	settings?: string[];
	/** Whether it runs in a worker of its own. On by default. */
	worker?: boolean;
	/** The program's factory, for a page that loaded the script itself. */
	module?: ProgramFactory;
	/** The name that factory goes by, `DDNetDemoRenderer` otherwise. */
	moduleName?: string;
	/** Where `ddnet-demo-render.js` is, if it is not beside this module. */
	scriptUrl?: string;
	homePath?: string;
	programName?: string;
	dataBase?: string;
	arguments?: string[];
	videoSink?: VideoSinkSource;
	sweepVideoScratch?: boolean;
	signal?: AbortSignal;
	onStart?: (handle: RenderHandle | Program) => void;
	onOutput?: (message: string, kind: OutputKind) => void;
	onProgress?: (text: string) => void;
	onRenderProgress?: (status: RenderProgress) => void;
}

/**
 * A render that is running: the same two things the callbacks say, said as
 * events, and the way to stop it. A render on the page's own thread hands over
 * the program itself, which answers to the same words.
 */
export declare class RenderHandle extends EventTarget {
	quit(): void;
}

/**
 * The demo renderer: a program of the same family as the viewers, with a way
 * in of its own. `DemoRenderer.render` is what `renderDemo` calls.
 */
export declare class DemoRenderer extends Program {
	static render(options: RenderOptions): Promise<File | Blob | null>;
}

/** Writes a video of a demo. */
export declare function renderDemo(options: RenderOptions): Promise<File | Blob | null>;

/**
 * Puts videos that are in memory into one zip file, so that a batch of them is
 * one thing to save. Nothing is compressed - a video is compressed already.
 */
export declare function zip(entries: { name: string; blob: Blob }[]): Promise<Blob>;

declare const DDNetDemoRenderer: {
	DemoRenderer: typeof DemoRenderer;
	renderDemo: typeof renderDemo;
	zip: typeof zip;
	programUrl: typeof programUrl;
	base: unknown;
};
export default DDNetDemoRenderer;
