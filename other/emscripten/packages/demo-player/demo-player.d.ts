import type { DemoInfo, Program, ProgramOptions, ViewerElement, VideoSettings } from "@ddnet/base";

export interface DemoPlayerOptions extends ProgramOptions {
	/** Whether the wheel, the zoom keys and a pinch zoom. */
	zoom?: boolean;
	/** Whether Tab and `=` show the scoreboard and the statistics. */
	overlays?: boolean;
	/** Where to start, in seconds, how fast, and whether paused. */
	startTime?: number;
	speed?: number;
	paused?: boolean;
}

export interface ExportOptions extends VideoSettings {
	/**
	 * Whom the video follows: a player id, -1 for the free view, -2 (the
	 * default) for whoever recorded the demo. A server demo was recorded by
	 * nobody and needs one of the first two.
	 */
	follow?: number;
}

/** A marker of a live stream, in the ticks of its demo. */
export interface LiveMarker {
	epoch: number;
	tick: number;
	/** `match_end` where a match ended, `manual` from `live_marker`. */
	kind: "match_end" | "manual";
	label?: string;
}

/**
 * Follows a live stream a server writes with `live_start`: it reads its
 * `index.json` every second and appends what the segments grew by to the
 * demo that plays, with range requests. Any static web server does.
 * `livechange` says that something of it changed. Joining at the live end
 * reads the last two minutes of the map, seeking back to where they begin
 * reads older ones, and at the live end more than half an hour is dropped.
 */
export declare class LiveFeed extends EventTarget {
	/** Seconds of the map read when joining at the live end, 120. */
	static backlogSeconds: number;
	/** Seconds of the map kept while playing at the live end, 1800. */
	static maxBacklogSeconds: number;
	/** The URL of the `index.json`. */
	readonly url: URL;
	/** The index as last read. */
	readonly index: object | null;
	/** The map of the stream that plays, counted from 0. */
	readonly epoch: number | null;
	/** `loading`, `waiting` (for the first segment), `live` or `ended`. */
	readonly state: string;
	/** What went wrong the last time, `""` if nothing. */
	readonly error: string;
	/** The markers of the map that plays. */
	readonly markers: LiveMarker[];
	/** Whether a newer map began than the one that plays. */
	readonly newerMap: boolean;
	/** Whether the stream has older parts of the map that plays than were read. */
	readonly earlier: boolean;
	/** Goes to the end of the newest map. */
	goLive(): Promise<void>;
	/** Whether it plays at the end of the newest map. */
	atLiveEdge(): boolean;
	stop(): void;
}

/**
 * A demo player. It answers what a `<video>` answers and fires the same
 * events: `loadedmetadata`, `durationchange`, `play`, `pause`, `timeupdate`,
 * `ratechange`, `volumechange`, `ended`. `viewchange` says that the marked
 * piece or whom it watches changed, `livechange` that a live stream did.
 */
export declare class DemoPlayer extends Program {
	constructor(options?: DemoPlayerOptions);
	static open(options?: DemoPlayerOptions): Promise<DemoPlayer>;

	/** In seconds, `NaN` before a demo is loaded. */
	readonly duration: number;
	currentTime: number;
	readonly paused: boolean;
	readonly ended: boolean;
	playbackRate: number;
	/** Between 0 and 1. */
	volume: number;
	muted: boolean;
	/** The URL of the demo, `""` for a file. */
	readonly src: string;
	/** Starts over at the end, as a video does; a live stream goes on at its end. */
	play(): Promise<void>;
	pause(): void;

	/** The live stream that plays, see `watchLive`. */
	readonly live: LiveFeed | null;
	/** Plays the live stream whose `index.json` is at `url`. */
	watchLive(url: string | URL): LiveFeed;
	stopLive(): void;
	/** To the end of the live stream. */
	goLive(): Promise<void> | undefined;
	/** Where the demo stands in ticks, and whether its file still grows. */
	ticks(): { first: number; current: number; last: number; live: boolean };

	/** The canvas's size in CSS pixels, which the page cannot set on the canvas itself. */
	setSize(width: number, height: number): void;
	/** How far it has played, between 0 and 1. */
	progress(): number | null;
	/** Jumps to a fraction of the demo. */
	seek(fraction: number): void;
	restart(): void;
	/** The marked piece in seconds, or `null`. */
	clip(): { start: number; end: number } | null;
	/** Marks a piece, or clears it with `clip(null)`. */
	clip(start: number | null, end?: number): void;
	/** Marks where the piece starts or ends at the current time. */
	markClip(which: "start" | "end"): void;

	/** Starts a video of the marked piece or the whole demo. */
	startExport(options?: ExportOptions): boolean;
	/** 0 idle, 1 exporting, 2 done, 3 failed. */
	exportState(): number | null;
	/** Between 0 and 1. */
	exportProgress(): number | null;
	exportSecondsLeft(): number | null;
	exportError(): string;
	cancelExport(): void;

	/** Whom the view follows (see `ExportOptions.follow`); sets it with an id. */
	spectating(id?: number): number | null;
	spectateName(name: string): void;
	/** The next (1) or previous (-1) player. */
	spectateStep(direction: number): void;
	/** Whether the demo was recorded by a server, so nobody's own view is in it. */
	serverDemo(): boolean;
	players(): { id: number; name: string }[];
	/** What the demo is: its header, its markers, its map and its players so far. */
	info(): DemoInfo;

	/** The zoom; multiplies it by `factor`. */
	zoom(factor?: number): number | null;
	resetZoom(): void;
	zoomChanged(): boolean;
	/** Each of these answers without an argument and sets with one. */
	zoomEnabled(enable?: boolean): boolean;
	overlays(enable?: boolean): boolean;
	/** The key presses shown on the players. Off by default. */
	keyPresses(show?: boolean): boolean;
	/** The map's high-detail layers. On by default. */
	highDetail(on?: boolean): boolean;
	recordedCameraAvailable(): boolean;
	/** Whether the view zooms as the recorded one did. */
	recordedCamera(use?: boolean): boolean;
	/**
	 * Whether the program is asked to draw its own bar, which it does not in a
	 * browser.
	 */
	controls(show?: boolean): boolean;
}

/**
 * The bar for one player, built on the page. It is styled by
 * `ddnet-viewer.css`; its parts carry `data-role`.
 */
export declare class DemoControls {
	constructor(player: DemoPlayer, options?: {
		container?: Element | null;
		/** What the bar lies over, the canvas by default. */
		picture?: Element | null;
		slot?: string | null;
		/** Whether the menu offers the key presses and high detail. On by default. */
		settings?: boolean;
		signal?: AbortSignal;
	});
	readonly element: HTMLElement;
	part(role: string): HTMLElement | null;
	destroy(): void;
}

/**
 * `<ddnet-demo>`. Attributes beyond those of `ViewerElement`: `live` (the
 * `index.json` of a live stream, in place of `src`), `nozoom`, `nooverlays`
 * (Tab shows no scoreboard and moves the focus on instead), `nosettings`
 * (the menu offers no display settings), `t`, `end` (a marked piece),
 * `speed`, `paused`, `spec` (a player id or name). It answers what a
 * `<video>` answers and fires the player's events.
 */
export declare class DemoElement extends ViewerElement {
	readonly program: DemoPlayer | null;
	readonly bar: DemoControls | null;
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

/** Where the program's script is, for a page that preloads it. */
export declare const programUrl: string;
