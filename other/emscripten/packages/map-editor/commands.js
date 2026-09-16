/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * Everything the editor can be told to do, in one list.
 *
 * A command has a name, the group it belongs in, the keys that reach it, and
 * three questions it can answer about itself: whether it can be done now,
 * whether it is on, and what it does. The tool bar is built out of this list,
 * the keyboard is a lookup in it, the tooltips take their key from it, and the
 * palette and the menus will read the same list rather than growing their own.
 *
 * A new thing the editor can do is an entry here. It is not a new button.
 */

/**
 * What a key press is called, in one string: the modifiers in a fixed order,
 * then the key itself.
 *
 * A letter is named by its letter whatever shift does to it - `Shift+R`, not
 * `Shift+R` on one layout and `Shift+r` on another - and a key on the number
 * pad is told apart from the one on the row above, because the plan gives them
 * different work.
 */
export function keyName(event) {
	let key = event.key;
	if (key === undefined || key === null) {
		return "";
	}
	if (key.length === 1) {
		key = key.toUpperCase();
	}
	if (typeof event.code === "string" && event.code.startsWith("Numpad") && event.code !== "NumpadEnter") {
		key = event.code;
	}
	const parts = [];
	if (event.ctrlKey || event.metaKey) {
		parts.push("Ctrl");
	}
	if (event.shiftKey) {
		parts.push("Shift");
	}
	if (event.altKey) {
		parts.push("Alt");
	}
	// A modifier held on its own is not a press of anything.
	if (["Control", "Shift", "Alt", "Meta"].includes(event.key)) {
		return "";
	}
	parts.push(key);
	return parts.join("+");
}

/** The same name, written the way a tooltip says it. */
export function keyLabel(name) {
	return name
		.replace("NumpadAdd", "Num +")
		.replace("NumpadSubtract", "Num -")
		.replace("NumpadMultiply", "Num *")
		.replace("ArrowUp", "↑")
		.replace("ArrowDown", "↓")
		.replace("Delete", "Del")
		.replace("Escape", "Esc");
}

// How far apart the lines of the grid are when it is switched on, said here
// as well because a command may switch it on.
const GRID_SPACING = 10;

// What a zoom step does. The same factor the wheel uses.
const ZOOM_STEP = 1.25;

/** The three ways the line under the pointer can talk about a tile. */
export const TILE_INFO_WAYS = ["off", "dec", "hex"];

// Every command that is the same shape as every other: switch a way of
// looking at the map on and off.
function looking(id, label, icon, keys, read, write) {
	return {
		id: id,
		label: label,
		group: "View",
		icon: icon,
		bar: true,
		keys: keys,
		pressed: p => read(p),
		run: p => {
			write(p, !read(p));
			p.refreshBar();
		},
	};
}

// The ten places a brush can be put away in.
function slots() {
	const out = [];
	for (let slot = 0; slot < 10; slot++) {
		out.push({
			id: `brush.use${slot}`,
			label: `Brush ${slot}`,
			group: "Brush",
			keys: [String(slot)],
			palette: false,
			run: p => {
				p.editor.useBrush(slot);
				p.refreshTiles();
			},
		});
		out.push({
			id: `brush.store${slot}`,
			label: `Put the brush away as ${slot}`,
			group: "Brush",
			keys: [`Shift+${slot}`],
			palette: false,
			run: p => {
				p.editor.storeBrush(slot);
				p.refreshTiles();
			},
		});
	}
	return out;
}

// Which of the four ways the pointer draws, as four commands that are one
// choice - so each is "on" while it is the one chosen.
function tools() {
	return [
		["brush.paint", "Paint", "paint", "B"],
		["brush.grab", "Grab", "grab", "S"],
		["brush.fill", "Fill", "fill", "F"],
		["brush.erase", "Erase", "erase", "E"],
	].map(([id, label, mode, key]) => ({
		id: id,
		label: label,
		group: "Brush",
		icon: mode,
		bar: true,
		keys: [key],
		pressed: p => p.tool === mode,
		run: p => {
			p.tool = mode;
			p.refreshBar();
		},
	}));
}


// Every kind of layer there is, as one command each. The digits are the order
// the file keeps them in, and Ctrl+Shift+<digit> is free where Ctrl+<digit> is
// the browser's.
function layerKinds() {
	return [
		["Tiles", "tiles", null],
		["Quads", "quads", null],
		["Sounds", "sounds", null],
		["Front", "tiles", "front"],
		["Tele", "tiles", "tele"],
		["Switch", "tiles", "switch"],
		["Speedup", "tiles", "speedup"],
		["Tune", "tiles", "tune"],
	].map(([name, type, kind], index) => ({
		id: `layer.add${name}`,
		label: `Add a ${name.toLowerCase()} layer`,
		group: "Layer",
		keys: [`Ctrl+Shift+${index + 1}`],
		enabled: p => p.map !== null,
		run: p => p.change(() => {
			const command = { op: "layer.add", group: p.selection.group, type: type };
			if (kind !== null) {
				command.kind = kind;
			}
			return p.editor.apply(command);
		}),
	}));
}

// The four sides of the map's own parts, and which tab each is.
function structureTabs() {
	return [
		["Layers", "layers"],
		["Images", "images"],
		["Sounds", "sounds"],
		["Map", "map"],
	].map(([name, tab], index) => ({
		id: `structure.${tab}`,
		label: name,
		group: "Areas",
		keys: [`Ctrl+Alt+${index + 1}`],
		pressed: p => p.areaShown("left") && p.tab.left === tab,
		run: p => {
			p.showArea("left", true);
			p.showTab("left", tab);
		},
	}));
}

/**
 * The list. Order is the order of the tool bar, and of the palette until
 * somebody types into it.
 */
export const COMMANDS = [
	...tools(),
	{
		id: "edit.undo", label: "Undo", group: "Edit", icon: "undo", bar: true,
		keys: ["Ctrl+Z"],
		enabled: p => {
			const history = p.editor.history();
			return history !== null && history.canUndo;
		},
		run: p => p.stepHistory(() => p.editor.undo()),
	},
	{
		id: "edit.redo", label: "Redo", group: "Edit", icon: "redo", bar: true,
		keys: ["Ctrl+Y", "Ctrl+Shift+Z"],
		enabled: p => {
			const history = p.editor.history();
			return history !== null && history.canRedo;
		},
		run: p => p.stepHistory(() => p.editor.redo()),
	},
	{
		id: "file.open", label: "Open a map…", group: "File", icon: "folder", bar: true,
		keys: ["Ctrl+O"],
		run: p => p.openMap(),
	},
	{
		id: "file.new", label: "New map", group: "File",
		// Ctrl+N belongs to the browser and cannot be taken from it.
		keys: ["Ctrl+Alt+N"],
		run: p => {
			p.editor.create(100, 50, "untitled");
			p.refresh();
		},
	},
	{
		id: "file.save", label: "Save", group: "File", icon: "save", bar: true,
		keys: ["Ctrl+S"],
		enabled: p => p.map !== null,
		run: p => p.editor.save(),
	},
	{
		id: "file.append", label: "Append a map…", group: "File",
		keys: ["Ctrl+Shift+A"],
		enabled: p => p.map !== null,
		run: p => p.part("append-file").click(),
	},
	{
		id: "view.fit", label: "The whole map", group: "View", icon: "fit", bar: true,
		keys: ["Home"],
		run: p => p.editor.fit(),
	},
	{
		id: "view.zoomIn", label: "Closer", group: "View",
		keys: ["NumpadAdd", "+"],
		run: p => p.editor.zoom(p.editor.zoom() / ZOOM_STEP),
	},
	{
		id: "view.zoomOut", label: "Further away", group: "View",
		keys: ["NumpadSubtract", "-"],
		run: p => p.editor.zoom(p.editor.zoom() * ZOOM_STEP),
	},
	{
		id: "view.zoomReset", label: "Back to one to one", group: "View",
		keys: ["NumpadMultiply"],
		run: p => p.editor.zoom(1),
	},
	looking("view.detail", "What is only there to look at", "detail", ["Ctrl+H"],
		p => p.editor.highDetail(), (p, on) => p.editor.highDetail(on)),
	looking("view.entities", "What the tiles do", "entities", ["Ctrl+Alt+E"],
		p => p.editor.entities() > 0, (p, on) => p.editor.entities(on ? 100 : 0)),
	looking("view.animate", "Let the envelopes run", "play", ["Ctrl+M"],
		p => p.editor.animate(), (p, on) => p.editor.animate(on)),
	looking("view.grid", "A grid on the tiles", "grid", ["G", "Ctrl+G"],
		p => p.editor.grid() > 0, (p, on) => p.editor.grid(on ? GRID_SPACING : 0)),
	{
		id: "view.proof", label: "What a player would see", group: "View", icon: "proof", bar: true,
		keys: ["P"],
		pressed: p => p.proof !== "off",
		run: p => {
			p.proof = p.proof === "off" ? "game" : (p.proof === "game" ? "menu" : "off");
			p.refreshBar();
			p.refreshOverlay();
		},
	},
	{
		id: "view.tileInfo", label: "What the tile under the pointer is", group: "View",
		keys: ["Ctrl+I"],
		pressed: p => p.tileInfo !== "off",
		run: p => {
			const next = TILE_INFO_WAYS.indexOf(p.tileInfo) + 1;
			p.tileInfo = TILE_INFO_WAYS[next % TILE_INFO_WAYS.length];
			p.say(`Tile info: ${p.tileInfo}`);
		},
	},
	{
		id: "brush.flipX", label: "Turn the brush over sideways", group: "Brush",
		keys: ["X", "N"],
		run: p => {
			p.editor.flipBrushX();
			p.refreshTiles();
		},
	},
	{
		id: "brush.flipY", label: "Turn the brush over", group: "Brush",
		keys: ["Y", "M"],
		run: p => {
			p.editor.flipBrushY();
			p.refreshTiles();
		},
	},
	{
		id: "brush.rotate", label: "A quarter turn", group: "Brush",
		keys: ["R"],
		run: p => {
			p.editor.rotateBrush();
			p.refreshTiles();
		},
	},
	{
		id: "brush.rotateBack", label: "A quarter turn the other way", group: "Brush",
		keys: ["Shift+R"],
		run: p => {
			// Three quarters one way is a quarter the other, and the program
			// only turns one way.
			p.editor.rotateBrush();
			p.editor.rotateBrush();
			p.editor.rotateBrush();
			p.refreshTiles();
		},
	},
	{
		id: "layer.addGroup", label: "Add a group", group: "Layer",
		keys: ["Ctrl+Shift+G"],
		enabled: p => p.map !== null,
		run: p => p.change(() => p.editor.apply({ op: "group.add", name: "group" })),
	},
	...layerKinds(),
	{
		id: "layer.next", label: "The layer below", group: "Layer",
		keys: ["ArrowDown"],
		run: p => p.stepSelection(1),
	},
	{
		id: "layer.previous", label: "The layer above", group: "Layer",
		keys: ["ArrowUp"],
		run: p => p.stepSelection(-1),
	},
	{
		id: "layer.up", label: "Move it up", group: "Layer",
		keys: ["Ctrl+ArrowUp"],
		run: p => p.moveSelected(-1),
	},
	{
		id: "layer.down", label: "Move it down", group: "Layer",
		keys: ["Ctrl+ArrowDown"],
		run: p => p.moveSelected(1),
	},
	{
		id: "layer.hide", label: "Draw it, or do not", group: "Layer",
		keys: ["V"],
		enabled: p => p.selection.layer >= 0,
		run: p => {
			const where = p.selection;
			p.editor.visible(where.group, where.layer, !p.editor.visible(where.group, where.layer));
			p.refresh();
		},
	},
	{
		id: "layer.delete", label: "Take the layer away", group: "Layer",
		keys: ["Ctrl+Delete"],
		enabled: p => p.map !== null,
		run: p => p.deleteSelected(),
	},
	{
		id: "edit.delete", label: "Take away what is picked", group: "Edit",
		keys: ["Delete"],
		run: p => p.deletePicked(),
	},
	{
		id: "quad.add", label: "Add a quad", group: "Quads",
		keys: ["Q"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "quads";
		},
		run: p => p.part("add-quad").click(),
	},
	{
		id: "quad.knife", label: "Cut a piece out of a quad", group: "Quads",
		keys: ["K"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "quads" && p.quad >= 0;
		},
		pressed: p => p.carving !== null,
		run: p => p.knife(),
	},
	{
		id: "tiles.numbers", label: "Into the numbers of the physics tile", group: "Brush",
		keys: ["T"],
		enabled: p => p.parts("number-number").length > 0 || p.parts("number-force").length > 0,
		run: p => {
			const field = p.part("number-number") || p.part("number-force");
			if (field !== null) {
				field.focus();
				field.select();
			}
		},
	},
	{
		id: "tiles.nextFree", label: "The next unused number", group: "Brush",
		keys: ["Ctrl+F"],
		enabled: p => p.part("next-free") !== null && !p.part("next-free").disabled,
		run: p => p.part("next-free").click(),
	},
	{
		id: "dock.envelopes", label: "Envelopes", group: "Areas",
		keys: ["Ctrl+E"],
		pressed: p => p.dockOpen && p.tab.dock === "envelopes",
		run: p => p.showTab("dock", "envelopes"),
	},
	{
		id: "dock.history", label: "History", group: "Areas",
		keys: ["Ctrl+Shift+H"],
		pressed: p => p.dockOpen && p.tab.dock === "history",
		run: p => p.showTab("dock", "history"),
	},
	{
		id: "dock.settings", label: "Server settings", group: "Areas",
		keys: ["Ctrl+Shift+E"],
		pressed: p => p.dockOpen && p.tab.dock === "settings",
		run: p => p.showTab("dock", "settings"),
	},
	{
		id: "dock.rules", label: "The rules file", group: "Areas",
		keys: ["Ctrl+Shift+R"],
		pressed: p => p.dockOpen && p.tab.dock === "rules",
		run: p => p.showTab("dock", "rules"),
	},
	{
		id: "area.left", label: "The map's parts", group: "Areas",
		keys: ["["],
		pressed: p => p.areaShown("left"),
		run: p => p.showArea("left", !p.areaShown("left")),
	},
	{
		id: "area.right", label: "The inspector", group: "Areas",
		keys: ["]"],
		pressed: p => p.areaShown("right"),
		run: p => p.showArea("right", !p.areaShown("right")),
	},
	{
		id: "area.mapOnly", label: "Nothing but the map", group: "Areas",
		keys: ["Tab"],
		// Only from the map itself: everywhere else Tab is how somebody walks
		// through the buttons, and taking that away would be worse than the
		// command is worth.
		where: "map",
		pressed: p => !p.areaShown("left") && !p.areaShown("right"),
		run: p => {
			const away = p.areaShown("left") || p.areaShown("right");
			for (const area of ["left", "right"]) {
				p.showArea(area, !away);
			}
			p.dockOpen = away ? false : p.dockOpen;
			p.applyTabs();
		},
	},
	{
		id: "image.add", label: "Add a picture\u2026", group: "Layer",
		keys: ["Ctrl+Shift+I"],
		enabled: p => p.map !== null,
		run: p => p.part("image-file").click(),
	},
	{
		id: "sound.add", label: "Add a sound\u2026", group: "Layer",
		keys: ["Ctrl+Shift+U"],
		enabled: p => p.map !== null,
		run: p => p.part("sound-file").click(),
	},
	{
		id: "source.add", label: "Add a sound source", group: "Quads",
		keys: ["Ctrl+Shift+S"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "sounds";
		},
		run: p => p.part("add-source").click(),
	},
	{
		id: "file.close", label: "Close the map", group: "File",
		// Ctrl+W and Ctrl+F4 belong to the browser.
		keys: ["Ctrl+Alt+W"],
		enabled: p => p.map !== null,
		run: p => {
			p.editor.close();
			p.refresh();
		},
	},
	...structureTabs(),
	{
		id: "help.wiki", label: "How mapping works (the wiki)", group: "Help",
		keys: ["F1"],
		run: () => window.open("https://wiki.ddnet.org/wiki/Mapping", "_blank", "noopener"),
	},
	{
		id: "edit.escape", label: "Back to the map", group: "Edit",
		keys: ["Escape"],
		run: p => p.escape(),
	},
	...slots(),
];

/**
 * What a command's button is called in the markup. The names are the ones the
 * buttons carried before there was a list, so that whoever looks for `undo`
 * still finds it.
 */
export function commandRole(command) {
	return command.role === undefined ? command.id.split(".")[1] : command.role;
}

/** What its tooltip says: what it does, and the key that does it. */
export function commandTitle(command) {
	const keys = command.keys || [];
	return keys.length === 0 ? command.label : `${command.label} (${keyLabel(keys[0])})`;
}

/** The commands by the keys that reach them. */
export function keyTable(commands) {
	const table = new Map();
	for (const command of commands) {
		for (const key of command.keys || []) {
			if (!table.has(key)) {
				table.set(key, command);
			}
		}
	}
	return table;
}

/** How many keys the table answers to, for whoever counts them. */
export function keyCount(commands) {
	return keyTable(commands).size;
}
