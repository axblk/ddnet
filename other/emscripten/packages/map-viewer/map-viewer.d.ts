import type { Program, ProgramOptions, ViewerElement } from "@ddnet/base";

/**
 * A map viewer. Everything is said in tiles. Events beyond the base's:
 * `load` once a map is shown, `viewchange` when the view moves.
 */
export declare class MapViewer extends Program {
	constructor(options?: ProgramOptions);
	static open(options?: ProgramOptions): Promise<MapViewer>;

	/** The URL of the map, `""` for a file. */
	readonly src: string;
	loaded(): boolean;
	setSize(width: number, height: number): void;
	/** Shows the whole map. */
	fit(): void;
	size(): { width: number; height: number } | null;
	/** The tile in the middle of the view; moves there with both. */
	center(): { x: number; y: number } | null;
	center(x: number, y: number): void;
	/** How many tiles fit across the view; zooms with a number. */
	tilesAcross(tiles?: number): number | null;
	zoomBy(factor: number): void;
	/** Each of these answers without an argument and sets with one. */
	highDetail(on?: boolean): boolean;
	entities(on?: boolean): boolean;
	/** Whether the program draws its own buttons. */
	controls(show?: boolean): boolean;
	/** Saves the view, or the whole map, as a PNG. */
	exportView(): void;
	exportFullMap(): void;
	/** 0 idle, 1 saving, 2 done, 3 failed. */
	exportState(): number | null;
	exportProgress(): number | null;
}

/** The buttons in the corner for one viewer, styled by `ddnet-viewer.css`. */
export declare class MapControls {
	constructor(viewer: MapViewer, options?: {
		container?: Element | null;
		picture?: Element | null;
		slot?: string | null;
		signal?: AbortSignal;
	});
	readonly element: HTMLElement;
	part(role: string): HTMLElement | null;
	destroy(): void;
}

/**
 * `<ddnet-map>`. Attributes beyond those of `ViewerElement`: `x`, `y` (the
 * tile in the middle), `tiles` (how many fit across). Fires the viewer's
 * events.
 */
export declare class MapElement extends ViewerElement {
	readonly program: MapViewer | null;
	readonly bar: MapControls | null;
}

/** Where the program's script is, for a page that preloads it. */
export declare const programUrl: string;
