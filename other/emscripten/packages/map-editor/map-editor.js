/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * The DDNet map editor, as one module: a `.map` file goes in, the map is drawn
 * on a canvas, and everything that is not the map itself - the layer tree, the
 * properties, the history - is the page's, built out of what the program says
 * about the map.
 *
 * ```js
 * import { MapEditor, EditorPanels } from "@ddnet/map-editor";
 *
 * const editor = await MapEditor.open({ canvas, src: "https://…/a.map" });
 * new EditorPanels(editor, { container: document.querySelector("#panels") });
 * ```
 *
 * The program itself is WebAssembly and lives beside this file. What every
 * program of this family needs is `@ddnet/base`, the runtime this is built on.
 *
 * The other end of the calls below is the `MapEditor*` block in
 * `src/game/map/standalone/map_editor_main.cpp`. That is a C ABI, and it stops
 * here.
 */

import DDNetBase, { addIcons, Program } from "@ddnet/base";

// The pictures on the editor's own buttons. Named as the viewer names its
// own, so that a page which shows both says the same thing twice rather than
// two different things.
addIcons({
	detail: '<path d="M12 1.5 13.9 9.1 21.5 11 13.9 12.9 12 20.5 10.1 12.9 2.5 11 10.1 9.1Z"/>',
	entities: '<rect x="3" y="3" width="8" height="8" rx="1.6"/><rect x="13" y="3" width="8" height="8" rx="1.6"/><rect x="3" y="13" width="8" height="8" rx="1.6"/><rect x="13" y="13" width="8" height="8" rx="1.6"/>',
	play: '<path d="M7.5 3.8 20.5 12 7.5 20.2Z"/>',
	undo: '<path d="M4 11h10a5 5 0 0 1 0 10h-6M4 11l5-5M4 11l5 5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>',
	redo: '<path d="M20 11H10a5 5 0 0 0 0 10h6M20 11l-5-5M20 11l-5 5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>',
});

/** The program, and what its script calls the factory it defines. */
const PROGRAM = "ddnet-map-editor.js";
const MODULE_NAME = "DDNetMapEditor";
const PROGRAM_NAME = "Map editor";
const SUFFIX = ".map";

/** Where the program's script is, for a page that wants to fetch it early. */
export const programUrl = new URL(PROGRAM, import.meta.url).href;

// A tile is 32 world units across, in every map there is. The editor is
// written in those units and a page has no business knowing them, so the one
// place that turns the one into the other is here.
const MAP_TILE_SIZE = 32;

/**
 * A map editor: the base, told where its script lies, and the map said in the
 * words a map is written in - tiles, layers, groups, history entries.
 *
 * Every call takes the number of the map it is about, and leaving it out means
 * the one in front. That is not decoration: the program holds a list of maps,
 * so that a page may show tabs without starting a second program.
 */
class CMapEditor extends Program {
	static script = PROGRAM;
	static base = import.meta.url;
	static moduleName = MODULE_NAME;
	static programName = PROGRAM_NAME;
	static suffix = SUFFIX;

	// A call that takes numbers and answers nothing.
	setNumbers(name, values) {
		return this.call(name, null, values.map(() => "number"), values);
	}

	// A call that takes the number of a map and whatever else, and answers a
	// number or nothing.
	ask(name, returnType, values) {
		return this.call(name, returnType, (values || []).map(() => "number"), values || []);
	}

	// A call that answers a JSON text, parsed. `null` where the program is not
	// running or the map is not there.
	json(name, values) {
		const text = this.call(name, "string", (values || []).map(() => "number"), values || []);
		if (text === null || text === "null") {
			return null;
		}
		try {
			return JSON.parse(text);
		} catch (error) {
			return null;
		}
	}

	/** The number of the map that is in front, or -1 while none is open. */
	get map() {
		const active = this.call("MapEditorActive", "number");
		return active === null ? -1 : active;
	}

	/** The numbers of all the maps that are open, in the order they were. */
	get maps() {
		const count = this.call("MapEditorCount", "number") || 0;
		const ids = [];
		for (let index = 0; index < count; ++index) {
			ids.push(this.ask("MapEditorIdAt", "number", [index]));
		}
		return ids;
	}

	/** What a map is called. */
	name(id) {
		return this.call("MapEditorName", "string", ["number"], [this.which(id)]) || "";
	}

	/** Puts a map in front. */
	activate(id) {
		return this.ask("MapEditorSetActive", "number", [id]) === 1;
	}

	/** Closes a map and gives up everything it held. */
	close(id) {
		return this.ask("MapEditorClose", "number", [this.which(id)]) === 1;
	}

	/** An empty map with a game layer of that many tiles. */
	create(width, height, name) {
		return this.call("MapEditorCreate", "number", ["number", "number", "string"],
			[Math.round(width), Math.round(height), name || "untitled"]);
	}

	/** Writes the map out and offers it as a file. */
	save(id) {
		return this.ask("MapEditorSave", "number", [this.which(id)]) === 1;
	}

	/** What the map is made of: groups, layers, envelopes, images, sounds. */
	structure(id) {
		return this.json("MapEditorStructure", [this.which(id)]);
	}

	/** What was done to the map, and where in it the map stands. */
	history(id) {
		return this.json("MapEditorHistory", [this.which(id)]);
	}

	/**
	 * Asks the map for a change - see `map_document::Apply` for the commands.
	 *
	 * @param command The command, as an object with an `op` in it.
	 * @param id Which map, or the one in front.
	 *
	 * @returns what the program answered: `{ok: true, …}` or
	 * `{ok: false, error}`.
	 */
	apply(command, id) {
		const text = this.call("MapEditorApply", "string", ["number", "string"],
			[this.which(id), JSON.stringify(command)]);
		if (text === null) {
			return { ok: false, error: "The editor is not running" };
		}
		try {
			return JSON.parse(text);
		} catch (error) {
			return { ok: false, error: text };
		}
	}

	/** A step back or forward through the versions, or straight to one. */
	undo(id) {
		return this.apply({ op: "history.undo" }, id);
	}
	redo(id) {
		return this.apply({ op: "history.redo" }, id);
	}
	jump(index, id) {
		return this.apply({ op: "history.jump", index: index }, id);
	}

	/**
	 * Opens a change that many commands make together, so that a slider being
	 * dragged or a brush being drawn with is one entry in the history. Every
	 * command in between is already what is drawn.
	 */
	begin(label, id) {
		return this.call("MapEditorBegin", null, ["number", "string"], [this.which(id), label || "Change"]);
	}
	commit(id) {
		return this.ask("MapEditorCommit", null, [this.which(id)]);
	}
	abort(id) {
		return this.ask("MapEditorAbort", null, [this.which(id)]);
	}

	/** How big to draw, in the units the page measures its boxes in. */
	setSize(width, height) {
		return this.setNumbers("MapEditorSetSize", [Math.round(width), Math.round(height)]);
	}

	/** Where the view looks, in tiles, or looks there. */
	center(x, y, id) {
		const map = this.which(id);
		if (x === undefined) {
			const cx = this.ask("MapEditorCenterX", "number", [map]);
			return cx === null ? null : { x: cx / MAP_TILE_SIZE, y: this.ask("MapEditorCenterY", "number", [map]) / MAP_TILE_SIZE };
		}
		return this.setNumbers("MapEditorSetCenter", [map, x * MAP_TILE_SIZE, y * MAP_TILE_SIZE]);
	}

	/** The zoom, which is how much of a tile fits in a pixel. */
	zoom(value, id) {
		const map = this.which(id);
		return value === undefined
			? this.ask("MapEditorZoom", "number", [map])
			: this.setNumbers("MapEditorSetZoom", [map, value]);
	}

	/** Moves the map under the pointer by that many pixels. */
	moveByPixels(dx, dy, id) {
		return this.setNumbers("MapEditorMoveByPixels", [this.which(id), dx, dy]);
	}

	/**
	 * Zooms about a point on the canvas, which keeps whatever is under the
	 * pointer under the pointer. A factor above one shows more map.
	 */
	zoomAt(x, y, factor, id) {
		return this.setNumbers("MapEditorZoomAt", [this.which(id), x, y, factor]);
	}

	/** Puts the whole map on the screen. */
	fit(id) {
		return this.ask("MapEditorFit", null, [this.which(id)]);
	}

	/** How big the map is, in tiles. */
	size(id) {
		const map = this.which(id);
		const width = this.ask("MapEditorWorldWidth", "number", [map]);
		return width === null ? null : { width: width / MAP_TILE_SIZE, height: this.ask("MapEditorWorldHeight", "number", [map]) / MAP_TILE_SIZE };
	}

	/** Where a point on the canvas is on the map, in tiles and in whole ones. */
	worldAt(x, y, id) {
		const map = this.which(id);
		const wx = this.ask("MapEditorWorldX", "number", [map, x, y]);
		return wx === null ? null : { x: wx / MAP_TILE_SIZE, y: this.ask("MapEditorWorldY", "number", [map, x, y]) / MAP_TILE_SIZE };
	}
	tileAt(x, y, id) {
		const map = this.which(id);
		const tx = this.ask("MapEditorTileX", "number", [map, x, y]);
		return tx === null ? null : { x: tx, y: this.ask("MapEditorTileY", "number", [map, x, y]) };
	}

	/** Whether the parts that are only there to be looked at are drawn. */
	highDetail(on, id) {
		const map = this.which(id);
		return on === undefined
			? this.ask("MapEditorHighDetail", "number", [map]) === 1
			: this.setNumbers("MapEditorSetHighDetail", [map, on ? 1 : 0]);
	}

	/** How strongly what the tiles do is drawn over what they look like. */
	entities(value, id) {
		const map = this.which(id);
		return value === undefined
			? this.ask("MapEditorEntities", "number", [map])
			: this.setNumbers("MapEditorSetEntities", [map, value === true ? 100 : (value === false ? 0 : value)]);
	}

	/** Whether the envelopes run. An editor stands still unless asked not to. */
	animate(on, id) {
		const map = this.which(id);
		return on === undefined
			? this.ask("MapEditorAnimate", "number", [map]) === 1
			: this.setNumbers("MapEditorSetAnimate", [map, on ? 1 : 0]);
	}

	/** Whether the pictures of the map in front are all here yet. */
	loading() {
		return this.call("MapEditorLoading", "number") === 1;
	}

	// Which map a call is about: the one that was named, or the one in front.
	which(id) {
		return id === undefined || id === null ? this.map : id;
	}
}


// What a panel is made of. Written here rather than in the page that shows
// it, so that the page is a page and two editors on one would get two sets of
// panels that know nothing about each other. Everything that can be got at
// from outside carries `data-role`, which is also what the browser tests ask
// for.
const PANELS_HTML = `
<div class="editor-bar" data-role="bar">
	<button class="editor-button" data-role="undo" data-icon="undo" title="Undo (Ctrl+Z)" aria-label="Undo"></button>
	<button class="editor-button" data-role="redo" data-icon="redo" title="Redo (Ctrl+Y)" aria-label="Redo"></button>
	<button class="editor-button" data-role="fit" data-icon="fit" title="The whole map (Home)" aria-label="The whole map"></button>
	<button class="editor-button" data-role="detail" data-icon="detail" title="What is only there to look at" aria-pressed="true"></button>
	<button class="editor-button" data-role="entities" data-icon="entities" title="What the tiles do" aria-pressed="false"></button>
	<button class="editor-button" data-role="animate" data-icon="play" title="Let the envelopes run" aria-pressed="false"></button>
	<button class="editor-button" data-role="save" data-icon="save" title="Save the map" aria-label="Save the map"></button>
	<span class="editor-status" data-role="status" role="status"></span>
</div>
<div class="editor-columns">
	<section class="editor-panel" data-role="tree-panel">
		<header class="editor-panel-head">
			<h2>Layers</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="add-group" title="Add a group">+ group</button>
				<button class="editor-small" data-role="add-layer" title="Add a tile layer">+ layer</button>
				<button class="editor-small" data-role="add-quads" title="Add a quad layer">+ quads</button>
				<button class="editor-small" data-role="delete" title="Delete what is selected">-</button>
				<button class="editor-small" data-role="up" title="Move it up">&uarr;</button>
				<button class="editor-small" data-role="down" title="Move it down">&darr;</button>
			</span>
		</header>
		<ul class="editor-tree" data-role="tree"></ul>
	</section>
	<section class="editor-panel" data-role="props-panel">
		<header class="editor-panel-head"><h2 data-role="props-title">Properties</h2></header>
		<div class="editor-props" data-role="props"></div>
	</section>
	<section class="editor-panel" data-role="history-panel">
		<header class="editor-panel-head">
			<h2>History</h2>
			<span class="editor-panel-tools"><span data-role="history-bytes"></span></span>
		</header>
		<ol class="editor-history" data-role="history"></ol>
	</section>
</div>
`;

// Which properties a thing has, what they are called on the screen, and what
// sort of value each takes. One table rather than a form per kind: the names
// are the ones `map_document::Apply` knows, so a property that is added there
// is added here and nowhere else.
const GROUP_PROPS = [
	{ prop: "name", label: "Name", kind: "text" },
	{ prop: "parallaxX", label: "Parallax X", kind: "number" },
	{ prop: "parallaxY", label: "Parallax Y", kind: "number" },
	{ prop: "offsetX", label: "Offset X", kind: "number" },
	{ prop: "offsetY", label: "Offset Y", kind: "number" },
	{ prop: "useClipping", label: "Clipping", kind: "boolean" },
	{ prop: "clipX", label: "Clip X", kind: "number" },
	{ prop: "clipY", label: "Clip Y", kind: "number" },
	{ prop: "clipW", label: "Clip W", kind: "number" },
	{ prop: "clipH", label: "Clip H", kind: "number" },
];

const LAYER_PROPS = {
	common: [
		{ prop: "name", label: "Name", kind: "text" },
		{ prop: "detail", label: "Detail", kind: "boolean" },
	],
	tiles: [
		{ prop: "image", label: "Image", kind: "number" },
		{ prop: "color", label: "Colour", kind: "color" },
		{ prop: "colorEnvelope", label: "Colour envelope", kind: "number" },
		{ prop: "colorEnvelopeOffset", label: "Envelope offset", kind: "number" },
	],
	quads: [{ prop: "image", label: "Image", kind: "number" }],
	sounds: [{ prop: "sound", label: "Sound", kind: "number" }],
};

// How the value of a group or layer is found in what the program said about
// it: a clip is one array of four, an offset one of two, everything else is
// itself.
const PACKED = {
	offsetX: ["offset", 0], offsetY: ["offset", 1],
	parallaxX: ["parallax", 0], parallaxY: ["parallax", 1],
	clipX: ["clip", 0], clipY: ["clip", 1], clipW: ["clip", 2], clipH: ["clip", 3],
};

function propertyValue(thing, prop) {
	const packed = PACKED[prop];
	return packed === undefined ? thing[prop] : thing[packed[0]][packed[1]];
}

// A colour as the browser writes one, and back. The map keeps four channels
// of its own, and the alpha is not something an `<input type=color>` has.
function hexOf(color) {
	return "#" + color.slice(0, 3).map(channel => channel.toString(16).padStart(2, "0")).join("");
}

function channelsOf(hex) {
	return [1, 3, 5].map(at => parseInt(hex.slice(at, at + 2), 16));
}

/**
 * The panels of one editor: what the map is made of, what the selected thing
 * is, and what was done to it. Each wired to that editor and nothing else.
 *
 * What it looks like comes from `@ddnet/base/editor.css`. The panels are
 * rebuilt whenever the program says the map changed, which is the only way
 * they hear about it - nothing here polls.
 */
class CEditorPanels {
	/**
	 * @param editor The editor to show.
	 * @param options.container Where the panels go.
	 * @param options.signal Takes them off again, the same as `destroy`.
	 */
	constructor(editor, options) {
		const settings = Object.assign({ container: null, signal: undefined }, options || {});
		this.editor = editor;
		this.stopping = new AbortController();
		if (settings.signal) {
			settings.signal.addEventListener("abort", () => this.destroy(), { once: true });
		}
		// What is selected: a group, or a layer in one. Held here rather than
		// in the program, because what is selected is a thing about the
		// panels and the program has no panels.
		this.selection = { group: 0, layer: -1 };
		this.collapsed = new Set();
		this.root = document.createElement("div");
		this.root.className = "editor-panels";
		this.root.innerHTML = PANELS_HTML;
		DDNetBase.paintIcons(this.root);
		if (settings.container !== null) {
			settings.container.append(this.root);
		}
		this.wire();
		this.refresh();
	}

	/** The panels themselves, for a page that wants to put them elsewhere. */
	get element() {
		return this.root;
	}

	part(role) {
		return this.root.querySelector(`[data-role="${role}"]`);
	}

	destroy() {
		this.stopping.abort();
		this.root.remove();
	}

	wire() {
		const signal = this.stopping.signal;
		const on = (role, handler) => this.part(role).addEventListener("click", handler, { signal: signal });
		on("undo", () => this.change(() => this.editor.undo()));
		on("redo", () => this.change(() => this.editor.redo()));
		on("fit", () => this.editor.fit());
		on("save", () => this.editor.save());
		on("detail", () => {
			this.editor.highDetail(!this.editor.highDetail());
			this.refreshBar();
		});
		on("entities", () => {
			this.editor.entities(this.editor.entities() > 0 ? 0 : 100);
			this.refreshBar();
		});
		on("animate", () => {
			this.editor.animate(!this.editor.animate());
			this.refreshBar();
		});
		on("add-group", () => this.change(() => this.editor.apply({ op: "group.add", name: "group" })));
		on("add-layer", () => this.change(() => this.editor.apply({ op: "layer.add", group: this.selection.group, type: "tiles" })));
		on("add-quads", () => this.change(() => this.editor.apply({ op: "layer.add", group: this.selection.group, type: "quads" })));
		on("delete", () => this.deleteSelected());
		on("up", () => this.moveSelected(-1));
		on("down", () => this.moveSelected(1));

		// The program says when the map changed; nothing here asks it in a
		// loop the way the viewer's buttons do, because an editor calls.
		for (const type of ["document", "loaded", "closed", "saved", "error"]) {
			this.editor.addEventListener(type, event => this.onProgram(type, event.detail), { signal: signal });
		}
		document.addEventListener("keydown", event => this.onKey(event), { signal: signal });
	}

	onProgram(type, detail) {
		if (type === "saved") {
			this.say("Saved");
		} else if (type === "error") {
			this.say(`Failed: ${detail && detail.what ? detail.what : "something"}`);
		} else if (type === "loaded") {
			this.selection = { group: 0, layer: -1 };
			this.collapsed.clear();
		}
		this.refresh();
	}

	// The keyboard belongs to whoever has the focus: a name being typed into
	// a field is not an undo, whatever letters are in it.
	onKey(event) {
		const target = event.target;
		if (target && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.isContentEditable)) {
			return;
		}
		if (event.ctrlKey || event.metaKey) {
			if (event.key === "z" && !event.shiftKey) {
				this.change(() => this.editor.undo());
			} else if (event.key === "y" || (event.key === "z" && event.shiftKey)) {
				this.change(() => this.editor.redo());
			} else {
				return;
			}
			event.preventDefault();
		}
	}

	say(text) {
		this.part("status").textContent = text || "";
	}

	// Does something that changes the map and then shows what came of it. The
	// program says so as well, and saying it twice costs a redraw of three
	// panels - but a command that changed nothing sends no event, and a panel
	// that then showed the old selection would be lying.
	change(work) {
		const answer = work();
		if (answer && answer.ok === false) {
			this.say(answer.error || "Refused");
		}
		this.refresh();
		return answer;
	}

	deleteSelected() {
		const where = this.selection;
		if (where.layer >= 0) {
			this.change(() => this.editor.apply({ op: "layer.delete", group: where.group, layer: where.layer }));
			this.selection = { group: where.group, layer: -1 };
		} else {
			this.change(() => this.editor.apply({ op: "group.delete", group: where.group }));
			this.selection = { group: 0, layer: -1 };
		}
		this.refresh();
	}

	moveSelected(by) {
		const where = this.selection;
		const map = this.editor.structure();
		if (map === null) {
			return;
		}
		if (where.layer >= 0) {
			const to = where.layer + by;
			const count = map.groups[where.group].layers.length;
			if (to < 0 || to >= count) {
				return;
			}
			const answer = this.change(() => this.editor.apply({
				op: "layer.move", group: where.group, layer: where.layer, toGroup: where.group, to: to,
			}));
			if (answer && answer.ok) {
				this.selection = { group: answer.group, layer: answer.layer };
			}
		} else {
			const to = where.group + by;
			if (to < 0 || to >= map.groups.length) {
				return;
			}
			const answer = this.change(() => this.editor.apply({ op: "group.move", group: where.group, to: to }));
			if (answer && answer.ok) {
				this.selection = { group: answer.group, layer: -1 };
			}
		}
		this.refresh();
	}

	/** Builds all three panels again out of what the program says now. */
	refresh() {
		const map = this.editor.structure();
		this.map = map;
		this.clampSelection();
		this.refreshBar();
		this.refreshTree();
		this.refreshProps();
		this.refreshHistory();
	}

	clampSelection() {
		if (this.map === null || this.map.groups.length === 0) {
			this.selection = { group: 0, layer: -1 };
			return;
		}
		const group = Math.min(this.selection.group, this.map.groups.length - 1);
		const layers = this.map.groups[group].layers.length;
		this.selection = { group: group, layer: Math.min(this.selection.layer, layers - 1) };
	}

	refreshBar() {
		const history = this.editor.history();
		const set = (role, on) => this.part(role).setAttribute("aria-pressed", on ? "true" : "false");
		this.part("undo").disabled = !(history && history.canUndo);
		this.part("redo").disabled = !(history && history.canRedo);
		set("detail", this.editor.highDetail());
		set("entities", this.editor.entities() > 0);
		set("animate", this.editor.animate());
	}

	refreshTree() {
		const tree = this.part("tree");
		tree.textContent = "";
		if (this.map === null) {
			return;
		}
		this.map.groups.forEach((group, groupIndex) => {
			const item = document.createElement("li");
			item.className = "editor-group";
			const head = document.createElement("div");
			head.className = "editor-row";
			head.dataset.role = "group";
			head.dataset.group = String(groupIndex);
			const fold = document.createElement("button");
			fold.className = "editor-fold";
			fold.textContent = this.collapsed.has(groupIndex) ? "▸" : "▾";
			fold.addEventListener("click", event => {
				event.stopPropagation();
				if (this.collapsed.has(groupIndex)) {
					this.collapsed.delete(groupIndex);
				} else {
					this.collapsed.add(groupIndex);
				}
				this.refreshTree();
			}, { signal: this.stopping.signal });
			const name = document.createElement("span");
			name.className = "editor-name";
			name.textContent = group.name || `Group ${groupIndex}`;
			head.append(fold, name);
			if (this.selection.group === groupIndex && this.selection.layer < 0) {
				head.classList.add("editor-selected");
			}
			head.addEventListener("click", () => {
				this.selection = { group: groupIndex, layer: -1 };
				this.refreshTree();
				this.refreshProps();
			}, { signal: this.stopping.signal });
			item.append(head);

			if (!this.collapsed.has(groupIndex)) {
				const list = document.createElement("ul");
				list.className = "editor-layers";
				group.layers.forEach((layer, layerIndex) => {
					const row = document.createElement("li");
					row.className = "editor-row editor-layer";
					row.dataset.role = "layer";
					row.dataset.group = String(groupIndex);
					row.dataset.layer = String(layerIndex);
					const what = layer.type === "tiles" ? layer.kind : layer.type;
					row.textContent = `${layer.name || what} (${what})`;
					if (this.selection.group === groupIndex && this.selection.layer === layerIndex) {
						row.classList.add("editor-selected");
					}
					row.addEventListener("click", () => {
						this.selection = { group: groupIndex, layer: layerIndex };
						this.refreshTree();
						this.refreshProps();
					}, { signal: this.stopping.signal });
					list.append(row);
				});
				item.append(list);
			}
			tree.append(item);
		});
	}

	refreshProps() {
		const props = this.part("props");
		props.textContent = "";
		if (this.map === null || this.map.groups.length === 0) {
			this.part("props-title").textContent = "Properties";
			return;
		}
		const where = this.selection;
		const group = this.map.groups[where.group];
		if (where.layer < 0) {
			this.part("props-title").textContent = `Group: ${group.name || where.group}`;
			for (const field of GROUP_PROPS) {
				props.append(this.field(group, field, value => ({
					op: "group.setProp", group: where.group, prop: field.prop, value: value,
				})));
			}
			return;
		}
		const layer = group.layers[where.layer];
		this.part("props-title").textContent = `Layer: ${layer.name || layer.type}`;
		const fields = LAYER_PROPS.common.concat(LAYER_PROPS[layer.type] || []);
		for (const field of fields) {
			props.append(this.field(layer, field, value => ({
				op: "layer.setProp", group: where.group, layer: where.layer, prop: field.prop, value: value,
			})));
		}
		if (layer.type === "tiles") {
			props.append(this.readout("Size", `${layer.size[0]} x ${layer.size[1]}`));
		} else if (layer.type === "quads") {
			props.append(this.readout("Quads", String(layer.quads)));
		}
	}

	readout(label, text) {
		const row = document.createElement("label");
		row.className = "editor-prop";
		const name = document.createElement("span");
		name.textContent = label;
		const value = document.createElement("span");
		value.className = "editor-readout";
		value.textContent = text;
		row.append(name, value);
		return row;
	}

	/**
	 * One property as a form field. A field that is dragged - a number with
	 * arrows, a colour being picked - opens a change on the first move and
	 * closes it when it is let go, so that the map follows every step and the
	 * history gets one entry.
	 */
	field(thing, description, command) {
		const row = document.createElement("label");
		row.className = "editor-prop";
		const name = document.createElement("span");
		name.textContent = description.label;
		const input = document.createElement("input");
		input.dataset.role = `prop-${description.prop}`;
		const current = propertyValue(thing, description.prop);
		let editing = false;
		const send = value => {
			const answer = this.editor.apply(command(value));
			if (answer && answer.ok === false) {
				this.say(answer.error || "Refused");
			}
		};
		const open = () => {
			if (!editing) {
				editing = true;
				this.editor.begin(description.label);
			}
		};
		const close = () => {
			if (editing) {
				editing = false;
				this.editor.commit();
				this.refresh();
			}
		};

		if (description.kind === "boolean") {
			input.type = "checkbox";
			input.checked = current === true;
			input.addEventListener("change", () => {
				send(input.checked);
				this.refresh();
			}, { signal: this.stopping.signal });
		} else if (description.kind === "color") {
			input.type = "color";
			input.value = hexOf(current);
			input.addEventListener("input", () => {
				open();
				send(channelsOf(input.value).concat([current[3]]));
			}, { signal: this.stopping.signal });
			input.addEventListener("change", close, { signal: this.stopping.signal });
			input.addEventListener("blur", close, { signal: this.stopping.signal });
		} else if (description.kind === "number") {
			input.type = "number";
			input.value = String(current);
			input.addEventListener("input", () => {
				const value = Number.parseInt(input.value, 10);
				if (Number.isFinite(value)) {
					open();
					send(value);
				}
			}, { signal: this.stopping.signal });
			input.addEventListener("change", close, { signal: this.stopping.signal });
			input.addEventListener("blur", close, { signal: this.stopping.signal });
		} else {
			input.type = "text";
			input.value = String(current);
			input.addEventListener("change", () => {
				send(input.value);
				this.refresh();
			}, { signal: this.stopping.signal });
		}
		row.append(name, input);
		return row;
	}

	refreshHistory() {
		const list = this.part("history");
		list.textContent = "";
		const history = this.editor.history();
		if (history === null) {
			this.part("history-bytes").textContent = "";
			return;
		}
		this.part("history-bytes").textContent = `${Math.round(history.bytes / (1024 * 1024))} MiB`;
		history.entries.forEach((entry, index) => {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "history-entry";
			row.dataset.index = String(index);
			row.textContent = entry.label;
			if (index === history.current) {
				row.classList.add("editor-selected");
			} else if (index > history.current) {
				row.classList.add("editor-undone");
			}
			row.addEventListener("click", () => this.change(() => this.editor.jump(index)), { signal: this.stopping.signal });
			list.append(row);
		});
	}
}

/**
 * Pointer and wheel on the canvas: dragging moves the map, the wheel zooms
 * about whatever is under it. That is all the editor does with a pointer so
 * far; a brush is the next thing that goes here.
 *
 * @param editor The editor to steer.
 * @param options.canvas What to listen on, or the editor's own canvas.
 * @param options.signal Stops listening again.
 */
function steerWithPointer(editor, options) {
	const settings = Object.assign({ canvas: null, signal: undefined }, options || {});
	const canvas = settings.canvas || editor.canvas;
	const stopping = new AbortController();
	if (settings.signal) {
		settings.signal.addEventListener("abort", () => stopping.abort(), { once: true });
	}
	const signal = stopping.signal;
	let dragging = 0;
	let last = { x: 0, y: 0 };
	// The canvas is measured in the units the page lays out in and drawn in
	// the pixels the screen has; a pointer that ignored the difference would
	// move the map at half speed on a screen with more of the latter.
	const scale = () => (canvas.width || 1) / (canvas.getBoundingClientRect().width || 1);
	canvas.addEventListener("pointerdown", event => {
		dragging = event.pointerId;
		last = { x: event.clientX, y: event.clientY };
		canvas.setPointerCapture(event.pointerId);
	}, { signal: signal });
	canvas.addEventListener("pointermove", event => {
		if (dragging !== event.pointerId) {
			return;
		}
		const factor = scale();
		editor.moveByPixels(-(event.clientX - last.x) * factor, -(event.clientY - last.y) * factor);
		last = { x: event.clientX, y: event.clientY };
	}, { signal: signal });
	const release = event => {
		if (dragging === event.pointerId) {
			dragging = 0;
			canvas.releasePointerCapture(event.pointerId);
		}
	};
	canvas.addEventListener("pointerup", release, { signal: signal });
	canvas.addEventListener("pointercancel", release, { signal: signal });
	canvas.addEventListener("wheel", event => {
		event.preventDefault();
		const box = canvas.getBoundingClientRect();
		const factor = scale();
		editor.zoomAt((event.clientX - box.left) * factor, (event.clientY - box.top) * factor,
			event.deltaY > 0 ? WHEEL_ZOOM_STEP : 1 / WHEEL_ZOOM_STEP);
	}, { signal: signal, passive: false });
	return { destroy: () => stopping.abort() };
}

// What one notch of the wheel does, the same step the map viewer takes.
const WHEEL_ZOOM_STEP = 1.1;

/**
 * A map editor: `MapEditor.open({canvas, src})` for one on a canvas the page
 * keeps, `MapEditor.openPage({elements})` for a page that is nothing else.
 */
export const MapEditor = CMapEditor;
/** The panels for one editor, for a page that places them itself. */
export const EditorPanels = CEditorPanels;
/** Dragging and the wheel on the canvas. */
export const steerEditor = steerWithPointer;

export default {
	MapEditor,
	EditorPanels,
	steerEditor,
	programUrl,
	// The base this is built on, so that a page that has this has the rest of
	// it too without a second import.
	base: DDNetBase,
};
