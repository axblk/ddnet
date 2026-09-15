/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

import type { Program, StartOptions, PageOptions, ViewerElement, VideoSettings } from "@ddnet/base";

/**
 * A demo player: the base, told where its script lies and what a demo can be
 * asked.
 *
 * What it can be asked is what a `<video>` can be asked, and what only a demo
 * can. It is also an `EventTarget`, and it sends what a `<video>` sends:
 * `loadedmetadata`, `durationchange`, `play`, `pause`, `timeupdate`,
 * `ratechange`, `volumechange` and `ended`.
 */
export declare class DemoPlayer extends Program {
	/** Starts a demo player on a canvas and answers with it, running. */
	static open(options: StartOptions): Promise<DemoPlayer>;
	/** The same, plus the furniture a page of nothing but a player wants. */
	static openPage(options: PageOptions): Promise<DemoPlayer>;

	// ----- what a `<video>` answers to -----

	/** How long the demo is, in seconds, or `NaN` before there is one. */
	readonly duration: number;
	/** Where in it the viewer is, in seconds. Seeks when it is set. */
	currentTime: number;
	/** Whether it stands still. A demo that is not there yet does. */
	readonly paused: boolean;
	/** Whether it has played to its end and stopped there. */
	readonly ended: boolean;
	/** How fast, as a multiple of the speed it was recorded at. */
	playbackRate: number;
	/** How loud, between 0 and 1. */
	volume: number;
	/** Whether the sound is off; what it was before is remembered. */
	muted: boolean;
	/** What is being shown, as the address it was named by. */
	readonly src: string;
	/** Plays it; at the end it starts over, the way a video does. */
	play(): Promise<void>;
	pause(): void;

	// ----- what only a demo answers to -----

	setSize(width: number, height: number): void;
	/** How far it has played, between 0 and 1. */
	progress(): number | null;
	/** Jumps to a part of the demo, between 0 and 1. */
	seek(fraction: number): void;
	/** Jumps to a time in the demo, in seconds from its beginning. */
	seekTime(seconds: number): void;
	restart(): void;
	/** The piece that is marked out, or `null` where nothing is. */
	clip(): { start: number; end: number } | null;
	/** Marks one out, or clears it with `clip(null)`. */
	clip(start: number | null, end?: number): void;
	exporting(): boolean;
	exportState(): number | null;
	exportProgress(): number | null;
	exportSecondsLeft(): number | null;
	exportError(): string;
	cancelExport(): void;
	startExport(options?: VideoSettings): boolean;
	spectating(id?: number): number | null;
	spectateName(name: string): void;
	spectateStep(direction: number): void;
	players(): { id: number; name: string }[];
	zoom(factor?: number): number | null;
	/** Puts the zoom back where the demo started. */
	resetZoom(): void;
	/** Whether there is anything for `resetZoom` to put back. */
	zoomChanged(): boolean;
	/** Whether the viewer zooms on the wheel, the zoom keys and a pinch. */
	zoomEnabled(enable?: boolean): boolean | void;
	recordedCameraAvailable(): boolean;
	recordedCamera(use?: boolean): boolean | void;
	/** Whether the viewer draws its own bar over the demo. */
	controls(show?: boolean): boolean | void;
}

/**
 * `<ddnet-demo>`: `src`, `controls`, `nozoom`, `t`, `end`, `speed`, `paused`,
 * `spec`.
 *
 * Beyond those it answers to what a `<video>` answers to, by handing the
 * question on to the player it holds - the properties below, and the same
 * events, plus `error`.
 *
 * What a `<video>` has and this has not is what a demo has no answer for:
 * `buffered`, `seekable`, `readyState`, `networkState`, `preload`, `poster`,
 * `loop` and the tracks.
 */
export declare class DemoElement extends ViewerElement {
	/** The player behind it, for everything the element does not offer. */
	readonly controls: DemoPlayer | null;
	readonly duration: number;
	currentTime: number;
	readonly paused: boolean;
	readonly ended: boolean;
	playbackRate: number;
	volume: number;
	muted: boolean;
	src: string;
	play(): Promise<void>;
	pause(): void;
}

/** Where the program's script is, for a page that wants to fetch it early. */
export declare const programUrl: string;
/** Defines `<ddnet-demo>`, which this module does for itself as it loads. */
export declare function defineDemoElement(): void;

/**
 * A bar of controls for one demo player: every button wired to that player and
 * nothing else, so a page may have as many as it has players. What it looks
 * like comes from `@ddnet/base/viewer.css`; the parts carry `data-role`.
 *
 * `<ddnet-demo controls="html">` is the same thing said in one word, and the
 * element then holds it as `element.bar`.
 */
export declare class DemoControls {
	constructor(player: DemoPlayer, options?: {
		container?: Element | null;
		picture?: Element | null;
		slot?: string | null;
		signal?: AbortSignal;
	});
	/** The bar itself. */
	readonly element: HTMLElement;
	/** A part of it by the name it carries, `play`, `seek`, `export` and so on. */
	part(role: string): HTMLElement | null;
	/** Takes it off the page again and stops asking the player anything. */
	destroy(): void;
}

declare const DDNetDemoPlayer: {
	DemoPlayer: typeof DemoPlayer;
	DemoControls: typeof DemoControls;
	DemoElement: typeof DemoElement;
	defineDemoElement: typeof defineDemoElement;
	programUrl: typeof programUrl;
	base: unknown;
};
export default DDNetDemoPlayer;
