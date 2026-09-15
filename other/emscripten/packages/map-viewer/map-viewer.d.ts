/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

import type { Callable, Instance, StartOptions, PageOptions, ViewerElement } from "ddnet-loader";

export interface MapControls {
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
	controls(show?: boolean): boolean | void;
}

/** Anything that can be called into, which is an `Instance` or a stand-in. */

/** `<ddnet-map>`: `src`, `controls`, `x`, `y`, `tiles`. */
export declare class MapElement extends ViewerElement {
	readonly controls: MapControls | null;
}

/**
 * A map viewer a page made for itself rather than one it wrote into its HTML.
 * Everything the viewer can be asked is on it by name as well as under
 * `controls`.
 */
export declare class MapViewer extends EventTarget {
	readonly instance: Instance | null;
	readonly controls: MapControls | null;
	readonly src: string;
	load(file: File): Promise<string>;
	loadUrl(url: string): Promise<string>;
	destroy(): void;
}
export declare interface MapViewer extends MapControls {}

/** Where the program's script is, for a page that wants to fetch it early. */
export declare const programUrl: string;
/** What a map viewer can be asked to do, bound to one of them. */
export declare function mapControls(instance: Callable): MapControls;
/** Starts a map viewer on a canvas. */
export declare function createMapViewer(options: Omit<StartOptions, "module"> & { module?: StartOptions["module"] }): Promise<MapViewer>;
/** The same, plus the furniture a page of nothing but a map viewer wants. */
export declare function mapViewerPage(options: Omit<PageOptions, "module"> & { module?: PageOptions["module"] }): Promise<Instance>;
/** Defines `<ddnet-map>`, which this module does for itself as it loads. */
export declare function defineMapElement(): void;

declare const DDNetMapViewer: {
	createMapViewer: typeof createMapViewer;
	mapViewerPage: typeof mapViewerPage;
	defineMapElement: typeof defineMapElement;
	mapControls: typeof mapControls;
	MapElement: typeof MapElement;
	MapViewer: typeof MapViewer;
	programUrl: typeof programUrl;
	loader: unknown;
};
export default DDNetMapViewer;
