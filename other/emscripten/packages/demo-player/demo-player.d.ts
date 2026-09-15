/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

import type { Callable, Instance, StartOptions, PageOptions, ViewerElement } from "ddnet-loader";

export interface DemoControls {
	setSize(width: number, height: number): void;
	length(): number | null;
	progress(): number | null;
	paused(): boolean;
	play(): void;
	pause(): void;
	seek(fraction: number): void;
	seekTime(seconds: number): void;
	restart(): void;
	/** The piece that is marked out, or `null` where nothing is. */
	clip(): { start: number; end: number } | null;
	/** Marks one out, or clears it with `clip(null)`. */
	clip(start: number | null, end?: number): void;
	speed(value?: number): number | null;
	exporting(): boolean;
	exportState(): number | null;
	exportProgress(): number | null;
	exportSecondsLeft(): number | null;
	exportError(): string;
	cancelExport(): void;
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
	/** How loud it is, between 0 and 1, the way `<video>` counts it. */
	volume(level?: number): number | void;
	/** Whether the sound is off; what it was before is remembered. */
	muted(mute?: boolean): boolean | void;
	recordedCameraAvailable(): boolean;
	recordedCamera(use?: boolean): boolean | void;
	controls(show?: boolean): boolean | void;
	startExport(options?: VideoSettings): boolean;
}

/** What a map viewer can be asked and told. Everything is in tiles. */

/**
 * `<ddnet-demo>`: `src`, `controls`, `nozoom`, `t`, `end`, `speed`, `paused`,
 * `spec`.
 *
 * Beyond those it answers to what a `<video>` answers to, so that a page that
 * can drive one of those can drive this: the properties below, and the events
 * `loadedmetadata`, `durationchange`, `play`, `pause`, `timeupdate`,
 * `ratechange`, `volumechange`, `ended` and `error`.
 *
 * What a `<video>` has and this has not is what a demo has no answer for:
 * `buffered`, `seekable`, `readyState`, `networkState`, `preload`, `poster`,
 * `loop` and the tracks.
 */
export declare class DemoElement extends ViewerElement {
	readonly controls: DemoControls | null;
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
	muted: boolean;
	/** What is being shown, the `src` attribute as a property. */
	src: string;
	play(): Promise<void>;
	pause(): void;
}

/**
 * A demo player a page made for itself rather than one it wrote into its HTML.
 * The same `<video>` surface as the element, and the same controls under it.
 */
export declare class DemoPlayer extends EventTarget {
	/** The running program, for what this does not offer itself. */
	readonly instance: Instance | null;
	/** Everything the viewer can be asked, the low-level way. */
	readonly controls: DemoControls | null;
	/** What is being shown, as the address it was named by. */
	readonly src: string;
	/** How long the demo is, in seconds, or `NaN` before there is one. */
	readonly duration: number;
	/** Where in it the viewer is, in seconds. Seeks when it is set. */
	currentTime: number;
	readonly paused: boolean;
	readonly ended: boolean;
	playbackRate: number;
	volume: number;
	muted: boolean;
	play(): Promise<void>;
	pause(): void;
	/** Shows a file rather than a name to fetch. */
	load(file: File): Promise<string>;
	/** Shows what is at this address. */
	loadUrl(url: string): Promise<string>;
	/** Stops the program and lets go of everything it held. */
	destroy(): void;
}

/** Where the program's script is, for a page that wants to fetch it early. */
export declare const programUrl: string;
/** What a demo player can be asked to do, bound to one of them. */
export declare function demoControls(instance: Callable): DemoControls;
/** Starts a demo player on a canvas. */
export declare function createDemoPlayer(options: Omit<StartOptions, "module"> & { module?: StartOptions["module"] }): Promise<DemoPlayer>;
/** The same, plus the furniture a page of nothing but a demo player wants. */
export declare function demoPlayerPage(options: Omit<PageOptions, "module"> & { module?: PageOptions["module"] }): Promise<Instance>;
/** Defines `<ddnet-demo>`, which this module does for itself as it loads. */
export declare function defineDemoElement(): void;

declare const DDNetDemoPlayer: {
	createDemoPlayer: typeof createDemoPlayer;
	demoPlayerPage: typeof demoPlayerPage;
	defineDemoElement: typeof defineDemoElement;
	demoControls: typeof demoControls;
	DemoElement: typeof DemoElement;
	DemoPlayer: typeof DemoPlayer;
	programUrl: typeof programUrl;
	loader: unknown;
};
export default DDNetDemoPlayer;
