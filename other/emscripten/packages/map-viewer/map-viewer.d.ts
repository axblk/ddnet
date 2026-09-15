/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

import type { Program, StartOptions, PageOptions, ViewerElement } from "@ddnet/base";

/**
 * A map viewer: the base, told where its script lies and what a map can be
 * asked. Everything is said in tiles.
 */
export declare class MapViewer extends Program {
	/** Starts a map viewer on a canvas and answers with it, running. */
	static open(options: StartOptions): Promise<MapViewer>;
	/** The same, plus the furniture a page of nothing but a viewer wants. */
	static openPage(options: PageOptions): Promise<MapViewer>;

	/** What is being shown, as the address it was named by. */
	readonly src: string;
	setSize(width: number, height: number): void;
	loaded(): boolean;
	fit(): void;
	size(): { width: number; height: number } | null;
	center(x?: number, y?: number): { x: number; y: number } | null | void;
	tilesAcross(tiles?: number): number | null;
	zoomBy(factor: number): void;
	highDetail(on?: boolean): boolean | void;
	entities(on?: boolean): boolean | void;
	exportView(): void;
	exportFullMap(): void;
	exportState(): number | null;
	exportProgress(): number | null;
	/** Whether the viewer draws its own bar over the map. */
	controls(show?: boolean): boolean | void;
}

/** `<ddnet-map>`: `src`, `controls`, `x`, `y`, `tiles`. */
export declare class MapElement extends ViewerElement {
	/** The viewer behind it, for everything the element does not offer. */
	readonly controls: MapViewer | null;
}

/** Where the program's script is, for a page that wants to fetch it early. */
export declare const programUrl: string;
/** Defines `<ddnet-map>`, which this module does for itself as it loads. */
export declare function defineMapElement(): void;

/**
 * The buttons in the corner for one map viewer: each wired to that viewer and
 * nothing else, so a page may have as many as it has maps. What they look like
 * comes from `@ddnet/base/viewer.css`; the parts carry `data-role`.
 *
 * `<ddnet-map controls="html">` is the same thing said in one word, and the
 * element then holds them as `element.bar`.
 */
export declare class MapControls {
	constructor(viewer: MapViewer, options?: {
		container?: Element | null;
		picture?: Element | null;
		slot?: string | null;
		signal?: AbortSignal;
	});
	/** The buttons themselves. */
	readonly element: HTMLElement;
	/** A part by the name it carries, `fit`, `entities`, `save-map` and so on. */
	part(role: string): HTMLElement | null;
	/** Takes them off the page again and stops asking the viewer anything. */
	destroy(): void;
}

declare const DDNetMapViewer: {
	MapViewer: typeof MapViewer;
	MapControls: typeof MapControls;
	MapElement: typeof MapElement;
	defineMapElement: typeof defineMapElement;
	programUrl: typeof programUrl;
	base: unknown;
};
export default DDNetMapViewer;
