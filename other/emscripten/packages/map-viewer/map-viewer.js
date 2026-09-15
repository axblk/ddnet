/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * The DDNet map viewer, as one module: a `.map` file goes in, the map is drawn
 * on a canvas, and what steers it is said in tiles - which is how a map is
 * written and how anybody talks about one.
 *
 * ```js
 * import { createMapViewer } from "@ddnet/map-viewer";
 *
 * const viewer = await createMapViewer({ canvas, src: "https://…/a.map" });
 * viewer.center(120, 64);
 * viewer.tilesAcross(90);
 * ```
 *
 * or, without writing any of that:
 *
 * ```html
 * <ddnet-map src="https://…/a.map" controls x="120" y="64" tiles="90"></ddnet-map>
 * ```
 *
 * The program itself is WebAssembly and lives beside this file; nothing here
 * has to be told where it is. What every program of this family needs is
 * `ddnet-loader`, the runtime this is built on.
 *
 * The other end of the calls below is the `MapViewer*` block in
 * `src/game/map/standalone/map_viewer_main.cpp`. That is a C ABI, and it stops
 * here.
 */

import DDNetLoader, { importProgram, page, start, ViewerElement } from "ddnet-loader";

/** The program, and what its script calls the factory it defines. */
const PROGRAM = "ddnet-map-viewer.js";
const MODULE_NAME = "DDNetMapViewer";
const PROGRAM_NAME = "Map viewer";
const SUFFIX = ".map";

/** Where the program's script is, for a page that wants to fetch it early. */
export const programUrl = new URL(PROGRAM, import.meta.url).href;

// A tile is 32 world units across, in every map there is. The viewer is
// written in those units and a page has no business knowing them, so the
// one place that turns the one into the other is here.
const MAP_TILE_SIZE = 32;

export function mapControls(instance) {
	const tiles = name => {
		const value = instance.call(name, "number");
		return value === null ? null : value / MAP_TILE_SIZE;
	};
	const setNumbers = (name, values) =>
		instance.call(name, null, values.map(() => "number"), values);
	return {
		loaded: () => instance.call("MapViewerMapLoaded", "number") === 1,
		/**
		 * How big to draw, in the units the page measures its boxes in.
		 * Only for a viewer that sits in a box of the page's own: one that
		 * fills the window follows it by itself. See `followSize`.
		 */
		setSize: (Width, Height) => setNumbers("MapViewerSetSize", [Math.round(Width), Math.round(Height)]),
		/** Fits the whole map on screen. */
		fit: () => instance.call("MapViewerFit"),
		/** How big the map is, in tiles. */
		size: () => {
			const width = tiles("MapViewerMapWidth");
			return width === null ? null : { width, height: tiles("MapViewerMapHeight") };
		},
		/** Where the view looks, in tiles, or looks there. */
		center: (X, Y) => {
			if (X === undefined) {
				const x = tiles("MapViewerCenterX");
				return x === null ? null : { x, y: tiles("MapViewerCenterY") };
			}
			return setNumbers("MapViewerSetCenter", [X * MAP_TILE_SIZE, Y * MAP_TILE_SIZE]);
		},
		/**
		 * How many tiles are across the screen, or zooms until that many
		 * are. It is the zoom said in a way that means the same in every
		 * window, which is what a link and a readout both need.
		 */
		tilesAcross: Tiles => {
			const visible = tiles("MapViewerVisibleWidth");
			if (Tiles === undefined) {
				return visible;
			}
			// What the view is set by is the zoom, and what that comes to in
			// tiles is the window's business - so it is asked what it shows
			// now and told the factor between that and what is wanted.
			if (visible > 0 && Tiles > 0) {
				setNumbers("MapViewerSetZoom", [instance.call("MapViewerZoom", "number") * Tiles / visible]);
			}
		},
		/** Zooms by a factor: 2 puts twice as much of the map on screen. */
		zoomBy: Factor => {
			const zoom = instance.call("MapViewerZoom", "number");
			if (zoom !== null) {
				setNumbers("MapViewerSetZoom", [zoom * Factor]);
			}
		},
		/**
		 * Whether the parts of the map that are only there to be looked at
		 * are drawn, or turns them on and off.
		 */
		highDetail: On => On === undefined
			? instance.call("MapViewerHighDetail", "number") === 1
			: instance.call("MapViewerSetHighDetail", null, ["number"], [On ? 1 : 0]),
		/**
		 * Whether what the tiles do is drawn over what they look like, or
		 * turns that on and off.
		 */
		entities: On => On === undefined
			? instance.call("MapViewerEntities", "number") === 1
			: instance.call("MapViewerSetEntities", null, ["number"], [On ? 1 : 0]),
		/**
		 * Whether the viewer draws its own bar of controls over the map,
		 * or switches it on and off, as in `demoControls`.
		 */
		controls: Show => Show === undefined
			? instance.call("MapViewerControls", "number") === 1
			: instance.call("MapViewerSetControls", null, ["number"], [Show ? 1 : 0]),
		/** Writes what is on screen, or the whole map, to a picture. */
		exportView: () => instance.call("MapViewerExportView"),
		exportFullMap: () => instance.call("MapViewerExportFullMap"),
		/** 0 while nothing is being written, 1 while it is, 2 when it failed. */
		exportState: () => instance.call("MapViewerExportState", "number"),
		/**
		 * How far a picture of the whole map has got, between 0 and 1. A
		 * picture of the view is one frame and is always 0.
		 */
		exportProgress: () => instance.call("MapViewerExportProgress", "number"),
	};
}

class CMapElement extends ViewerElement {
	static observedAttributes = ["src", "controls", "x", "y", "tiles"];
	static viewerKind = {
		// Where the program is: beside this module, wherever this module
		// was installed to.
		base: import.meta.url,
		script: PROGRAM,
		moduleName: MODULE_NAME,
		programName: PROGRAM_NAME,
		suffix: ".map",
		controls: instance => mapControls(instance),
		loaded: controls => controls.loaded(),
		apply: (controls, name, value, element) => {
			const number = parseFloat(value);
			if (name === "tiles" && isFinite(number) && number > 0) {
				controls.tilesAcross(number);
			} else if (name === "x" || name === "y") {
				// Both or neither: half a place to look is no place to
				// look, and the two arrive as two separate changes.
				const x = parseFloat(element.getAttribute("x"));
				const y = parseFloat(element.getAttribute("y"));
				if (isFinite(x) && isFinite(y)) {
					controls.center(x, y);
				}
			}
		},
	};
}

/**
 * A map viewer a page made for itself, rather than one it wrote into its HTML.
 * Everything the viewer can be asked is on it by name - `center`,
 * `tilesAcross`, `entities` and the rest - because a map viewer has no
 * `<video>` to take after and the calls are the interface.
 */
class CMapViewer extends EventTarget {
	constructor() {
		super();
		this.viewerInstance = null;
		this.viewerControls = null;
		this.viewerSource = "";
	}

	/** The running program, for what this does not offer itself. */
	get instance() {
		return this.viewerInstance;
	}

	/** Everything the viewer can be asked, as one object. */
	get controls() {
		return this.viewerControls;
	}

	/** What is being shown, as the address it was named by. */
	get src() {
		return this.viewerSource;
	}

	/** Shows a file rather than a name to fetch. */
	async load(file) {
		this.viewerSource = "";
		return await this.viewerInstance.loadFile(file);
	}

	/** Shows what is at this address. */
	async loadUrl(url) {
		this.viewerSource = String(url);
		return await this.viewerInstance.loadUrl(url);
	}

	/** Stops the program and lets go of everything it held. */
	destroy() {
		const instance = this.viewerInstance;
		this.viewerInstance = null;
		this.viewerControls = null;
		if (instance !== null) {
			instance.destroy();
		}
	}
}

/**
 * Starts a map viewer on a canvas.
 *
 * Everything `ddnet-loader`'s `start` takes may be said here as well. What
 * does not have to be said is which program to run and where it is: that is
 * what this module is.
 *
 * @returns a promise for the viewer, once the program runs.
 */
export async function createMapViewer(options) {
	const settings = Object.assign({}, options);
	const scriptUrl = settings.scriptUrl || programUrl;
	const factory = settings.module || await importProgram(scriptUrl, MODULE_NAME);
	const viewer = new CMapViewer();
	const instance = await start(Object.assign(settings, {
		module: factory,
		scriptUrl: scriptUrl,
		programName: settings.programName || PROGRAM_NAME,
		accept: settings.accept || [SUFFIX],
	}));
	viewer.viewerInstance = instance;
	viewer.viewerControls = mapControls(instance);
	viewer.viewerSource = typeof settings.file === "string" ? settings.file : "";
	// What the viewer can be asked, asked of the viewer: a page that made one
	// of these should not have to go through a second object for the calls
	// that are the whole point of it.
	for (const [name, call] of Object.entries(viewer.viewerControls)) {
		if (viewer[name] === undefined) {
			viewer[name] = call;
		}
	}
	return viewer;
}

/**
 * The same as `createMapViewer`, plus the furniture a page of nothing but a
 * map viewer wants: the canvas filling the window, a line that says what is
 * loading, a console log for what the program writes, and a file named in the
 * page's own address.
 *
 * What it answers with is the running program rather than a map viewer
 * object - this is for a page that steers the viewer through `mapControls`
 * and draws its own controls, which is what our own pages do.
 *
 * @returns a promise for the running instance.
 */
export async function mapViewerPage(options) {
	const settings = Object.assign({}, options);
	const scriptUrl = settings.scriptUrl || programUrl;
	const factory = settings.module || await importProgram(scriptUrl, MODULE_NAME);
	return await page(Object.assign(settings, {
		module: factory,
		scriptUrl: scriptUrl,
		programName: settings.programName || PROGRAM_NAME,
		accept: settings.accept || [SUFFIX],
	}));
}

/**
 * Defines `<ddnet-map>`, which this module does for itself as it loads. Here
 * for a page that takes it off and wants it back, and harmless twice.
 */
export function defineMapElement() {
	if (typeof customElements === "undefined") {
		return;
	}
	if (customElements.get("ddnet-map") === undefined) {
		customElements.define("ddnet-map", CMapElement);
	}
}

// Defined as this module is loaded, because the point of an element is that
// putting one in the page is all there is to it.
defineMapElement();

/** The class behind the element, for whoever wants to extend it. */
export const MapElement = CMapElement;
/** The class behind `createMapViewer`, for the same reason. */
export const MapViewer = CMapViewer;

export default {
	createMapViewer,
	mapViewerPage,
	defineMapElement,
	mapControls,
	MapElement,
	MapViewer,
	programUrl,
	loader: DDNetLoader,
};
