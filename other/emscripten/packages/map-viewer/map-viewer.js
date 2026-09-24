/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// The DDNet map viewer. The API is documented in map-viewer.d.ts; the other
// end of the calls is the `MapViewer*` block in
// `src/game/map/standalone/map_viewer_main.cpp`.

import { addIcons, autoHide, fullscreen, paintIcons, Program, ViewerElement } from "@ddnet/base";

addIcons({
	detail: '<path d="M12 1.5 13.9 9.1 21.5 11 13.9 12.9 12 20.5 10.1 12.9 2.5 11 10.1 9.1Z"/>',
	entities: '<rect x="3" y="3" width="8" height="8" rx="1.6"/><rect x="13" y="3" width="8" height="8" rx="1.6"/><rect x="3" y="13" width="8" height="8" rx="1.6"/><rect x="13" y="13" width="8" height="8" rx="1.6"/>',
	save_all: '<path d="M7 2.8V9.6M4.2 7 7 10 9.8 7M1.6 13.2v3a1.4 1.4 0 0 0 1.4 1.4h8a1.4 1.4 0 0 0 1.4-1.4v-3M17 6.4V13.2M14.2 10.6 17 13.6 19.8 10.6M11.6 16.8v3a1.4 1.4 0 0 0 1.4 1.4h8a1.4 1.4 0 0 0 1.4-1.4v-3" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>',
});

const PROGRAM = "ddnet-map-viewer.js";
// World units per tile, in every map.
const TILE_SIZE = 32;
const POLL_INTERVAL_MS = 200;

export const programUrl = new URL(PROGRAM, import.meta.url).href;

export class MapViewer extends Program {
	static script = PROGRAM;
	static base = import.meta.url;
	static moduleName = "DDNetMapViewer";
	static programName = "Map viewer";
	static suffix = ".map";

	constructor(options = {}) {
		super(options);
		this.source = typeof options.file === "string" ? options.file : "";
	}

	programArguments() {
		return (this.options.controls === false ? ["--no-controls"] : []).concat(super.programArguments());
	}

	// The map viewer has nothing on Tab, so it moves the focus.
	passesKey(event) {
		return event.key === "Tab";
	}

	get src() {
		return this.source;
	}

	async loadFile(file) {
		this.source = "";
		return await super.loadFile(file);
	}

	async loadUrl(url) {
		this.source = String(url);
		return await super.loadUrl(url);
	}

	tiles(name) {
		const value = this.call(name, "number");
		return value === null ? null : value / TILE_SIZE;
	}

	numbers(name, values) {
		this.call(name, null, values.map(() => "number"), values);
	}

	flag(name, setter, value) {
		return value === undefined ? this.call(name, "number") === 1 : this.numbers(setter, [value ? 1 : 0]);
	}

	loaded() {
		return this.call("MapViewerLoadCount", "number") > 0;
	}

	setSize(width, height) {
		this.numbers("MapViewerSetSize", [Math.round(width), Math.round(height)]);
	}

	fit() {
		this.call("MapViewerFit");
	}

	size() {
		const width = this.tiles("MapViewerMapWidth");
		return width === null ? null : { width, height: this.tiles("MapViewerMapHeight") };
	}

	center(x, y) {
		if (x === undefined) {
			const centerX = this.tiles("MapViewerCenterX");
			return centerX === null ? null : { x: centerX, y: this.tiles("MapViewerCenterY") };
		}
		this.numbers("MapViewerSetCenter", [x * TILE_SIZE, y * TILE_SIZE]);
	}

	// The zoom as the number of tiles across the view, which means the same
	// in every window.
	tilesAcross(tiles) {
		const visible = this.tiles("MapViewerVisibleWidth");
		if (tiles === undefined) {
			return visible;
		}
		if (visible > 0 && tiles > 0) {
			this.numbers("MapViewerSetZoom", [this.call("MapViewerZoom", "number") * tiles / visible]);
		}
	}

	zoomBy(factor) {
		const zoom = this.call("MapViewerZoom", "number");
		if (zoom !== null) {
			this.numbers("MapViewerSetZoom", [zoom * factor]);
		}
	}

	highDetail(on) {
		return this.flag("MapViewerHighDetail", "MapViewerSetHighDetail", on);
	}

	entities(on) {
		return this.flag("MapViewerEntities", "MapViewerSetEntities", on);
	}

	controls(show) {
		return this.flag("MapViewerControls", "MapViewerSetControls", show);
	}

	exportView() {
		this.call("MapViewerExportView");
	}

	exportFullMap() {
		this.call("MapViewerExportFullMap");
	}

	exportState() {
		return this.call("MapViewerExportState", "number");
	}

	exportProgress() {
		return this.call("MapViewerExportProgress", "number");
	}

	// Turns what the program shows into `load` and `viewchange` events.
	running() {
		const signal = this.stopping.signal;
		let known = null;
		let waitFor = 1;
		this.addEventListener("loadstart", () => {
			known = null;
			waitFor = (this.call("MapViewerLoadCount", "number") ?? 0) + 1;
		}, { signal });
		const poll = () => {
			if (signal.aborted) {
				return;
			}
			setTimeout(poll, POLL_INTERVAL_MS);
			const loads = this.call("MapViewerLoadCount", "number");
			if (!(loads >= waitFor)) {
				return;
			}
			const center = this.center();
			const view = `${center?.x}:${center?.y}:${this.tilesAcross()}`;
			if (known === null || known.loads !== loads) {
				this.dispatchEvent(new Event("load"));
			} else if (known.view !== view) {
				this.dispatchEvent(new Event("viewchange"));
			}
			known = { loads, view };
		};
		poll();
	}
}

// The same icons as the program's own corner, as buttons of the browser.
const CORNER_HTML = `
<p class="viewer-status" data-role="saving" role="status" hidden></p>
<div class="viewer-controls-corner" data-role="controls">
	<div class="viewer-cluster" data-role="cluster" hidden>
		<button class="viewer-button" data-role="fullscreen" data-icon="fullscreen"></button>
		<button class="viewer-button" data-role="zoom-out" data-icon="minus" title="Zoom out" aria-label="Zoom out"></button>
		<button class="viewer-button" data-role="zoom-in" data-icon="plus" title="Zoom in" aria-label="Zoom in"></button>
		<button class="viewer-button" data-role="fit" data-icon="fit" title="The whole map (Home)" aria-label="The whole map" aria-keyshortcuts="Home"></button>
		<button class="viewer-button" data-role="menu-button" data-icon="menu" title="More" aria-label="More" aria-haspopup="true" aria-expanded="false"></button>
	</div>
	<div class="viewer-cluster viewer-cluster-column" data-role="menu" hidden>
		<button class="viewer-button" data-role="detail" data-icon="detail" title="High detail" aria-label="High detail" aria-pressed="false"></button>
		<button class="viewer-button" data-role="entities" data-icon="entities" title="Entities" aria-label="Entities" aria-pressed="false"></button>
		<button class="viewer-button" data-role="save-view" data-icon="save" title="Save the view as a picture (F2)" aria-label="Save the view as a picture" aria-keyshortcuts="F2"></button>
		<button class="viewer-button" data-role="save-map" data-icon="save_all" title="Save the whole map as a picture" aria-label="Save the whole map as a picture"></button>
	</div>
</div>
`;

const CORNER_INTERVAL_MS = 200;
// One press zooms as far as the program's own button.
const BUTTON_ZOOM_STEP = 1.4;

export class MapControls {
	constructor(viewer, options = {}) {
		const { container = null, picture = null, slot = null, signal } = options;
		this.viewer = viewer;
		this.stopping = new AbortController();
		signal?.addEventListener("abort", () => this.destroy(), { once: true });
		this.picture = picture ?? viewer.canvas;
		this.root = Object.assign(document.createElement("div"), { className: "viewer-controls" });
		if (slot !== null) {
			this.root.slot = slot;
		}
		this.root.innerHTML = CORNER_HTML;
		paintIcons(this.root);
		container?.append(this.root);
		this.wire();
		this.timer = setInterval(() => this.update(), CORNER_INTERVAL_MS);
		this.update();
	}

	get element() {
		return this.root;
	}

	part(role) {
		return this.root.querySelector(`[data-role="${role}"]`);
	}

	destroy() {
		clearInterval(this.timer);
		this.stopping.abort();
		this.root.remove();
	}

	openMenu(open) {
		this.part("menu").hidden = !open;
		this.part("menu-button").setAttribute("aria-expanded", String(open));
	}

	wire() {
		const viewer = this.viewer;
		const signal = this.stopping.signal;
		const on = (target, type, listener) => target.addEventListener(type, listener, { signal });
		const part = role => this.part(role);

		fullscreen(part("fullscreen"), { element: this.picture, signal });
		autoHide([part("cluster"), part("menu")], {
			picture: this.picture,
			hold: () => !part("menu").hidden,
			onHide: () => this.openMenu(false),
			signal,
		});
		on(this.root, "mousedown", event => {
			if (event.target.closest("button")) {
				event.preventDefault();
			}
		});
		on(part("menu-button"), "click", () => this.openMenu(part("menu").hidden));
		on(document, "pointerdown", event => {
			if (!event.composedPath().includes(part("controls"))) {
				this.openMenu(false);
			}
		});
		// Escape also reaches here from the picture, which keeps the focus
		// after a click on a button.
		const keys = event => {
			if (event.key === "Escape" && !part("menu").hidden) {
				event.stopPropagation();
				const inside = this.root.contains(document.activeElement);
				this.openMenu(false);
				if (inside) {
					part("menu-button").focus();
				}
			}
		};
		on(this.picture, "keydown", keys);
		if (!this.picture.contains(this.root)) {
			on(this.root, "keydown", keys);
		}
		on(part("zoom-out"), "click", () => viewer.zoomBy(BUTTON_ZOOM_STEP));
		on(part("zoom-in"), "click", () => viewer.zoomBy(1 / BUTTON_ZOOM_STEP));
		on(part("fit"), "click", () => viewer.fit());
		on(part("detail"), "click", () => viewer.highDetail(!viewer.highDetail()));
		on(part("entities"), "click", () => viewer.entities(!viewer.entities()));
		// A download has to come out of a click.
		on(part("save-view"), "click", () => {
			viewer.exportView();
			this.openMenu(false);
		});
		on(part("save-map"), "click", () => {
			viewer.exportFullMap();
			this.openMenu(false);
		});
	}

	update() {
		const viewer = this.viewer;
		const part = role => this.part(role);
		if (!viewer.loaded()) {
			part("cluster").hidden = true;
			this.openMenu(false);
			return;
		}
		part("cluster").hidden = false;
		part("detail").setAttribute("aria-pressed", String(viewer.highDetail()));
		part("entities").setAttribute("aria-pressed", String(viewer.entities()));
		const busy = viewer.exportState() === 1;
		part("save-view").disabled = busy;
		part("save-map").disabled = busy;
		const progress = busy ? viewer.exportProgress() : 0;
		const saving = part("saving");
		saving.hidden = !(progress > 0);
		saving.textContent = saving.hidden ? "" : `Saving the whole map… ${Math.round(progress * 100)}%`;
	}
}

export class MapElement extends ViewerElement {
	static program = MapViewer;
	static bar = MapControls;
	static observedAttributes = ["src", "controls", "x", "y", "tiles"];
	static linkParams = { map: "src", x: "x", y: "y", tiles: "tiles" };
	static programEvents = ["load", "viewchange"];

	applyAttribute(name, value) {
		const viewer = this.program;
		const number = parseFloat(value);
		if (name === "tiles" && number > 0) {
			viewer.tilesAcross(number);
		} else if (name === "x" || name === "y") {
			// Both or neither.
			const x = parseFloat(this.getAttribute("x"));
			const y = parseFloat(this.getAttribute("y"));
			if (isFinite(x) && isFinite(y)) {
				viewer.center(x, y);
			}
		} else {
			super.applyAttribute(name, value);
		}
	}

	linkEvents() {
		return ["load", "viewchange"];
	}

	linkValues() {
		const center = this.program.center();
		const tiles = this.program.tilesAcross();
		if (center === null || !(tiles > 0)) {
			return {};
		}
		return { x: center.x.toFixed(1), y: center.y.toFixed(1), tiles: tiles.toFixed(1) };
	}
}

if (typeof customElements !== "undefined" && customElements.get("ddnet-map") === undefined) {
	customElements.define("ddnet-map", MapElement);
}
