/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * The DDNet map viewer, as one module: a `.map` file goes in, the map is drawn
 * on a canvas, and what steers it is said in tiles - which is how a map is
 * written and how anybody talks about one.
 *
 * ```js
 * import { MapViewer } from "@ddnet/map-viewer";
 *
 * const viewer = await MapViewer.open({ canvas, src: "https://…/a.map" });
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
 * `@ddnet/base`, the runtime this is built on.
 *
 * The other end of the calls below is the `MapViewer*` block in
 * `src/game/map/standalone/map_viewer_main.cpp`. That is a C ABI, and it stops
 * here.
 */

import DDNetBase, { addIcons, Program, ViewerElement } from "@ddnet/base";

// The pictures on a map viewer's own buttons, which no other page here has any
// use for: they are named and drawn as `CViewerControls::EIcon` names and
// draws them, so that the buttons this page puts up and the ones the program
// draws for itself say the same thing. Said as this module loads, so a page
// that imported it can write `data-icon="entities"` and get one.
addIcons({
	detail: '<path d="M12 1.5 13.9 9.1 21.5 11 13.9 12.9 12 20.5 10.1 12.9 2.5 11 10.1 9.1Z"/>',
	entities: '<rect x="3" y="3" width="8" height="8" rx="1.6"/><rect x="13" y="3" width="8" height="8" rx="1.6"/><rect x="3" y="13" width="8" height="8" rx="1.6"/><rect x="13" y="13" width="8" height="8" rx="1.6"/>',
	save_all: '<path d="M7 2.8V9.6M4.2 7 7 10 9.8 7M1.6 13.2v3a1.4 1.4 0 0 0 1.4 1.4h8a1.4 1.4 0 0 0 1.4-1.4v-3M17 6.4V13.2M14.2 10.6 17 13.6 19.8 10.6M11.6 16.8v3a1.4 1.4 0 0 0 1.4 1.4h8a1.4 1.4 0 0 0 1.4-1.4v-3" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>',
});

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

/**
 * A map viewer: the base, told where its script lies and what a map can be
 * asked. Everything is said in tiles - 32 world units each, which is what
 * every map is drawn in and what no page has any business knowing.
 *
 * The other end of the calls is the `MapViewer*` block in
 * `src/game/map/standalone/map_viewer_main.cpp`.
 */
class CMapViewer extends Program {
	static script = PROGRAM;
	static base = import.meta.url;
	static moduleName = MODULE_NAME;
	static programName = PROGRAM_NAME;
	static suffix = SUFFIX;

	constructor(options) {
		super(options);
		// What was asked for, so that `src` can say what is being shown.
		this.viewerSource = typeof options.file === "string" ? options.file : "";
	}

	/** What is being shown, as the address it was named by. */
	get src() {
		return this.viewerSource;
	}

	async loadFile(file) {
		this.viewerSource = "";
		return await super.loadFile(file);
	}

	async loadUrl(url) {
		this.viewerSource = String(url);
		return await super.loadUrl(url);
	}

	// A call that answers a number of world units, in tiles.
	tiles(name) {
		const value = this.call(name, "number");
		return value === null ? null : value / MAP_TILE_SIZE;
	}

	// A call that takes numbers and answers nothing.
	setNumbers(name, values) {
		return this.call(name, null, values.map(() => "number"), values);
	}

	// A call that answers a number. Answers `null` where the program is not
	// running, the same as `call` does.
	number(name, argument) {
		return argument === undefined
			? this.call(name, "number")
			: this.call(name, null, ["number"], [argument]);
	}

	loaded() {
		return this.call("MapViewerMapLoaded", "number") === 1;
	}
	/**
	 * How big to draw, in the units the page measures its boxes in.
	 * Only for a viewer that sits in a box of the page's own: one that
	 * fills the window follows it by itself. See `followSize`.
	 */
	setSize(Width, Height) {
		return this.setNumbers("MapViewerSetSize", [Math.round(Width), Math.round(Height)]);
	}
	/** Fits the whole map on screen. */
	fit() {
		return this.call("MapViewerFit");
	}
	/** How big the map is, in tiles. */
	size() {
		const width = this.tiles("MapViewerMapWidth");
		return width === null ? null : { width, height: this.tiles("MapViewerMapHeight") };
	}
	/** Where the view looks, in tiles, or looks there. */
	center(X, Y) {
		if (X === undefined) {
			const x = this.tiles("MapViewerCenterX");
			return x === null ? null : { x, y: this.tiles("MapViewerCenterY") };
		}
		return this.setNumbers("MapViewerSetCenter", [X * MAP_TILE_SIZE, Y * MAP_TILE_SIZE]);
	}
	/**
	 * How many tiles are across the screen, or zooms until that many
	 * are. It is the zoom said in a way that means the same in every
	 * window, which is what a link and a readout both need.
	 */
	tilesAcross(Tiles) {
		const visible = this.tiles("MapViewerVisibleWidth");
		if (Tiles === undefined) {
			return visible;
		}
		// What the view is set by is the zoom, and what that comes to in
		// tiles is the window's business - so it is asked what it shows
		// now and told the factor between that and what is wanted.
		if (visible > 0 && Tiles > 0) {
			this.setNumbers("MapViewerSetZoom", [this.call("MapViewerZoom", "number") * Tiles / visible]);
		}
	}
	/** Zooms by a factor: 2 puts twice as much of the map on screen. */
	zoomBy(Factor) {
		const zoom = this.call("MapViewerZoom", "number");
		if (zoom !== null) {
			this.setNumbers("MapViewerSetZoom", [zoom * Factor]);
		}
	}
	/**
	 * Whether the parts of the map that are only there to be looked at
	 * are drawn, or turns them on and off.
	 */
	highDetail(On) {
		return On === undefined
			? this.call("MapViewerHighDetail", "number") === 1
			: this.call("MapViewerSetHighDetail", null, ["number"], [On ? 1 : 0]);
	}
	/**
	 * Whether what the tiles do is drawn over what they look like, or
	 * turns that on and off.
	 */
	entities(On) {
		return On === undefined
			? this.call("MapViewerEntities", "number") === 1
			: this.call("MapViewerSetEntities", null, ["number"], [On ? 1 : 0]);
	}
	/**
	 * Whether the viewer draws its own bar of controls over the map,
	 * or switches it on and off, as in `demoControls`.
	 */
	controls(Show) {
		return Show === undefined
			? this.call("MapViewerControls", "number") === 1
			: this.call("MapViewerSetControls", null, ["number"], [Show ? 1 : 0]);
	}
	/** Writes what is on screen, or the whole map, to a picture. */
	exportView() {
		return this.call("MapViewerExportView");
	}
	exportFullMap() {
		return this.call("MapViewerExportFullMap");
	}
	/** 0 while nothing is being written, 1 while it is, 2 when it failed. */
	exportState() {
		return this.call("MapViewerExportState", "number");
	}
	/**
	 * How far a picture of the whole map has got, between 0 and 1. A
	 * picture of the view is one frame and is always 0.
	 */
	exportProgress() {
		return this.call("MapViewerExportProgress", "number");
	}}

// A map is looked at rather than played, so there is nothing to seek through
// and no reason for a bar along the bottom: what the view does sits in a
// corner out of the way of the map, and what is asked for once in a while sits
// behind the menu. The same buttons the viewer draws for itself, in the same
// order and with the same pictures - only as buttons the browser knows about,
// which is what gives them a tooltip and a tab order.
//
// Written here rather than in the page that shows it, so that a page with two
// maps on it gets two sets and neither knows about the other. What it is
// dressed in is `@ddnet/base/viewer.css`, which the page has to load.
const CORNER_HTML = `
<p class="viewer-readout viewer-saving" data-role="saving" role="status" hidden></p>
<div class="viewer-controls viewer-controls-corner" data-role="controls">
	<div class="viewer-cluster" data-role="cluster" hidden>
		<button class="viewer-button" data-role="fullscreen" data-icon="fullscreen" title="Full screen" aria-label="Full screen"></button>
		<button class="viewer-button" data-role="zoom-out" data-icon="minus" title="Zoom out" aria-label="Zoom out"></button>
		<button class="viewer-button" data-role="zoom-in" data-icon="plus" title="Zoom in" aria-label="Zoom in"></button>
		<button class="viewer-button" data-role="fit" data-icon="fit" title="The whole map (Home)" aria-label="The whole map"></button>
		<button class="viewer-button" data-role="menu-button" data-icon="menu" title="More" aria-label="More" aria-haspopup="true" aria-expanded="false"></button>
	</div>
	<div class="viewer-cluster viewer-cluster-column" data-role="menu" hidden>
		<button class="viewer-button" data-role="detail" data-icon="detail" title="What is only there to look at" aria-label="What is only there to look at" aria-pressed="false"></button>
		<button class="viewer-button" data-role="entities" data-icon="entities" title="What the tiles do" aria-label="What the tiles do" aria-pressed="false"></button>
		<button class="viewer-button" data-role="save-view" data-icon="save" title="Save what is on the screen (F2)" aria-label="Save what is on the screen"></button>
		<button class="viewer-button" data-role="save-map" data-icon="save_all" title="Save the whole map" aria-label="Save the whole map"></button>
	</div>
</div>
`;

// How often the buttons ask the viewer what to show, in milliseconds. The
// viewer holds the main thread for as long as it draws and only lets go
// between frames, so asking is both simpler and cheaper than being called
// back.
const CORNER_INTERVAL_MS = 200;

// A button is pressed once where a wheel is turned several notches, so it
// takes a bigger step - the same one the viewer's own button takes.
const BUTTON_ZOOM_STEP = 1.4;

/**
 * The buttons in the corner for one map viewer: each wired to that viewer and
 * nothing else, so a page may have as many as it has maps.
 *
 * ```js
 * const viewer = await MapViewer.open({ canvas, src: "…/a.map" });
 * const buttons = new MapControls(viewer, { container: document.body });
 * ```
 *
 * or, on an element, `<ddnet-map controls="html">`.
 *
 * What it looks like comes from `@ddnet/base/viewer.css`. The parts carry
 * `data-role`, so a page that wants at one of them asks its own set for it
 * rather than the document.
 */
class CMapControls {
	/**
	 * @param viewer The viewer to steer.
	 * @param options.container Where the buttons go.
	 * @param options.picture What a tap on it brings them back, and what fills
	 * the screen.
	 * @param options.slot Which slot to sit in, `controls` inside an element.
	 * @param options.signal Takes them off again, the same as `destroy`.
	 */
	constructor(viewer, options) {
		const settings = Object.assign({ container: null, picture: null, slot: null, signal: undefined }, options || {});
		this.viewer = viewer;
		this.stopping = new AbortController();
		if (settings.signal) {
			settings.signal.addEventListener("abort", () => this.destroy(), { once: true });
		}
		this.picture = settings.picture || viewer.canvas;
		this.root = document.createElement("div");
		this.root.className = "viewer-corner";
		if (settings.slot !== null) {
			this.root.setAttribute("slot", settings.slot);
		}
		this.root.innerHTML = CORNER_HTML;
		DDNetBase.paintIcons(this.root);
		if (settings.container !== null) {
			settings.container.append(this.root);
		}
		this.wire();
		this.timer = setInterval(() => this.update(), CORNER_INTERVAL_MS);
		this.update();
	}

	/** The buttons themselves, for a page that wants to put them elsewhere. */
	get element() {
		return this.root;
	}

	/** A part by the name it carries, for a page that wants at it. */
	part(role) {
		return this.root.querySelector(`[data-role="${role}"]`);
	}

	/** Takes them off the page again and stops asking the viewer anything. */
	destroy() {
		if (this.timer !== null) {
			clearInterval(this.timer);
			this.timer = null;
		}
		this.stopping.abort();
		this.root.remove();
	}

	openMenu(open) {
		this.part("menu").hidden = !open;
		this.part("menu-button").setAttribute("aria-expanded", open ? "true" : "false");
	}

	wire() {
		const viewer = this.viewer;
		const signal = this.stopping.signal;
		const on = (target, type, listener) => target.addEventListener(type, listener, { signal: signal });
		const part = role => this.part(role);

		DDNetBase.fullscreen(part("fullscreen"), { element: this.picture, signal: signal });
		// The buttons and the menu are one thing to whoever uses them, so they
		// step aside together while nothing is happening - and a tap on the map
		// takes them away or brings them back. The menu closes with them,
		// because a menu over a map somebody wanted to see is worse than no
		// menu.
		DDNetBase.autoHide([part("cluster"), part("menu")], {
			picture: this.picture,
			onHide: () => this.openMenu(false),
			signal: signal,
		});

		on(part("menu-button"), "click", () => this.openMenu(part("menu").hidden));
		// A click anywhere else closes it: a menu that has to be dismissed with
		// the button that opened it is one people leave open over the map they
		// wanted to see.
		on(document, "pointerdown", event => {
			if (!part("controls").contains(event.target)) {
				this.openMenu(false);
			}
		});
		on(document, "keydown", event => {
			if (event.key === "Escape") {
				this.openMenu(false);
			}
		});

		on(part("zoom-out"), "click", () => viewer.zoomBy(BUTTON_ZOOM_STEP));
		on(part("zoom-in"), "click", () => viewer.zoomBy(1 / BUTTON_ZOOM_STEP));
		on(part("fit"), "click", () => viewer.fit());
		on(part("detail"), "click", () => viewer.highDetail(!viewer.highDetail()));
		on(part("entities"), "click", () => viewer.entities(!viewer.entities()));
		// Writing a picture is what the browser hands over as a download, and
		// it only does that for what came out of a click. The click this is,
		// which is why it is asked for here and not out of something the page
		// works out for itself later.
		on(part("save-view"), "click", () => {
			viewer.exportView();
			this.openMenu(false);
		});
		on(part("save-map"), "click", () => {
			viewer.exportFullMap();
			this.openMenu(false);
		});
	}

	// What the buttons show, asked of the viewer rather than remembered from
	// the clicks: the window answers to its own keys and to the pointer as
	// well.
	update() {
		const viewer = this.viewer;
		const part = role => this.part(role);
		if (!viewer.loaded()) {
			part("cluster").hidden = true;
			this.openMenu(false);
			return;
		}
		part("cluster").hidden = false;
		part("detail").setAttribute("aria-pressed", viewer.highDetail() ? "true" : "false");
		part("entities").setAttribute("aria-pressed", viewer.entities() ? "true" : "false");
		// A picture is drawn in pieces between frames, and asking for a second
		// one while the first is being written would cut into it.
		const busy = viewer.exportState() === 1;
		part("save-view").disabled = busy;
		part("save-map").disabled = busy;
		// A picture of the whole map is drawn a piece at a time over many
		// frames, and for a large map that is long enough that somebody would
		// otherwise wonder whether the button did anything.
		const progress = busy ? viewer.exportProgress() : 0;
		const saving = part("saving");
		saving.hidden = !busy || !(progress > 0);
		if (!saving.hidden) {
			saving.textContent = `Saving the whole map… ${Math.round(progress * 100)}%`;
		}
	}
}

class CMapElement extends ViewerElement {
	static observedAttributes = ["src", "controls", "x", "y", "tiles"];
	static viewerProgram = CMapViewer;
	static viewerBar = CMapControls;
	static viewerKind = {
		loaded: viewer => viewer.loaded(),
		apply: (viewer, name, value, element) => {
			const number = parseFloat(value);
			if (name === "tiles" && isFinite(number) && number > 0) {
				viewer.tilesAcross(number);
			} else if (name === "x" || name === "y") {
				// Both or neither: half a place to look is no place to
				// look, and the two arrive as two separate changes.
				const x = parseFloat(element.getAttribute("x"));
				const y = parseFloat(element.getAttribute("y"));
				if (isFinite(x) && isFinite(y)) {
					viewer.center(x, y);
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

/**
 * A map viewer: `MapViewer.open({canvas, src})` for one on a canvas the page
 * keeps, `MapViewer.openPage({elements})` for a page that is nothing else.
 */
export const MapViewer = CMapViewer;
/**
 * The buttons in the corner for one viewer, for a page that places them
 * itself. `<ddnet-map controls="html">` is the same thing said in one word.
 */
export const MapControls = CMapControls;
/** The class behind the element, for whoever wants to extend it. */
export const MapElement = CMapElement;

export default {
	MapViewer,
	MapControls,
	MapElement,
	defineMapElement,
	programUrl,
	// The base this is built on, so that a page that has this has the rest of
	// it too without a second import.
	base: DDNetBase,
};
