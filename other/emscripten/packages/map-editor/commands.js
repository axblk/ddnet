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
	if (key === " ") {
		key = "Space";
	} else if (key.length === 1) {
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
		menu: "View",
		icon: icon,
		// In the bar's view menu, behind one button, rather than six buttons
		// in the bar: the bar is for what is used all the time.
		view: true,
		toggle: true,
		safe: true,
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
			label: `Brush slot ${slot}`,
			group: "Brush",
			// In the menu but not in the palette: ten rows of "Brush 3" would
			// drown the palette, and without a keyboard the menu is the way -
			// the strip of slots under the tileset is the quick way.
			menu: "Tools/Brush slots",
			keys: [String(slot)],
			palette: false,
			run: p => {
				p.editor.useBrush(slot);
				p.refreshTiles();
			},
		});
		out.push({
			id: `brush.store${slot}`,
			label: `Store brush in slot ${slot}`,
			group: "Brush",
			menu: "Tools/Store brush",
			keys: [`Shift+${slot}`],
			palette: false,
			run: p => {
				p.editor.storeBrush(slot);
				p.slotsUsed.add(slot);
				p.refreshTiles();
			},
		});
	}
	return out;
}

// The six tools, as six commands that are one choice - so each is "on" while
// it is the one chosen. The keys are the ones Tiled and Aseprite use.
function tools() {
	return [
		["brush.paint", "Brush", "paint", "B", "Drag to paint with the brush"],
		["brush.grab", "Select", "grab", "S", "Drag a rectangle to take tiles into the brush"],
		["brush.fill", "Fill", "fill", "F", "Drag a rectangle to fill it with the brush"],
		["brush.erase", "Eraser", "erase", "E", "Drag a rectangle to clear it"],
		["tool.pick", "Pick", "pick", "I", "Click a tile to take it, and its layer, into the brush"],
		["tool.hand", "Hand", "hand", "H", "Drag to pan"],
	].map(([id, label, mode, key, hint]) => ({
		id: id,
		label: label,
		hint: hint,
		group: "Tools",
		icon: mode,
		rail: true,
		// The hand only looks; the rest change the map, and an editor that is
		// only to be looked at has no use for a choice between them.
		safe: mode === "hand",
		always: ["paint", "grab", "erase", "hand"].includes(mode),
		keys: [key],
		// Filling and rubbing out are things done to tiles; on a quad or a
		// sound layer there are none, and the button says so by going grey.
		enabled: p => {
			if (mode !== "fill" && mode !== "erase") {
				return true;
			}
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "tiles";
		},
		pressed: p => p.tool === mode,
		run: p => {
			p.tool = mode;
			p.refreshBar();
			p.refreshStatus();
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
		label: `Add ${name.toLowerCase()} layer`,
		group: "Layer",
		menu: "Layer/Add layer",
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
		["Properties", "props"],
		["Images", "images"],
		["Sounds", "sounds"],
		["Map", "map"],
	].map(([name, tab], index) => ({
		id: `structure.${tab}`,
		label: name,
		group: "Panels",
		menu: "View/Inspector",
		safe: true,
		keys: [`Ctrl+Alt+${index + 1}`],
		pressed: p => p.areaShown("left") && p.tab.left === tab,
		run: p => {
			p.showArea("left", true);
			p.showTab("left", tab);
		},
	}));
}

// Everything that used to be a button in a panel and nowhere else.
//
// The button stays where it is - with the panel open it is the shortest way
// there is - but the command is what the palette, the menus and the keyboard
// see. Otherwise a thing the editor can do would be a thing only a visible
// button can do, and half of what the editor can do would be unfindable.
function panelButton(id, label, group, role, extra) {
	return Object.assign({
		id: id,
		label: label,
		group: group,
		part: role,
		enabled: p => {
			const button = p.part(role);
			return p.map !== null && button !== null && !button.disabled && !button.closest("[hidden]");
		},
		run: p => p.reveal(role).click(),
	}, extra || {});
}

// The buttons of the panels, in the order their panels stand in.
function panelButtons() {
	return [
		panelButton("art.tiles", "Import image as tiles…", "Tools", "tile-art", { menu: "Tools" }),
		panelButton("art.quads", "Import image as quads…", "Tools", "quad-art", { menu: "Tools" }),
		panelButton("tiles.write", "Write text with tiles", "Tools", "type-place", { menu: "Tools" }),
		panelButton("tiles.automap", "Run automapper", "Tools", "automap-run", { menu: "Tools" }),
		panelButton("layer.construct", "Build physics tiles from layer", "Tools", "construct-run",
			{ menu: "Tools" }),
		panelButton("image.replace", "Replace image…", "Layer", "replace-image", { for: "image" }),
		panelButton("image.unpack", "Unpack image", "Layer", "unpack-image", { for: "image" }),
		panelButton("image.delete", "Delete image", "Layer", "delete-image", { for: "image" }),
		panelButton("sound.play", "Play sound", "Layer", "play-sound", { for: "sound" }),
		panelButton("sound.replace", "Replace sound…", "Layer", "replace-sound", { for: "sound" }),
		panelButton("sound.unpack", "Unpack sound", "Layer", "unpack-sound", { for: "sound" }),
		panelButton("sound.delete", "Delete sound", "Layer", "delete-sound", { for: "sound" }),
		panelButton("quad.delete", "Delete quad", "Quads", "delete-quad", { for: "quad" }),
		panelButton("quad.square", "Make square", "Quads", "shape-square", { for: "quad" }),
		panelButton("quad.aspect", "Match image aspect", "Quads", "shape-aspect", { for: "quad" }),
		panelButton("quad.pivot", "Center pivot", "Quads", "shape-centerPivot", { for: "quad" }),
		panelButton("quad.align", "Align to grid", "Quads", "shape-align", { for: "quad" }),
		panelButton("source.delete", "Delete sound source", "Quads", "delete-source", { for: "source" }),
		panelButton("envelope.add", "Add envelope", "Areas", "add-envelope"),
		panelButton("envelope.delete", "Delete envelope", "Areas", "delete-envelope", { for: "envelope" }),
		panelButton("setting.add", "Add server setting", "Areas", "add-setting"),
		panelButton("setting.delete", "Delete server setting", "Areas", "delete-setting", { for: "setting" }),
		panelButton("rules.apply", "Apply rules", "Areas", "rules-apply"),
		panelButton("rules.revert", "Reload rules", "Areas", "rules-revert"),
		panelButton("rules.save", "Save rules file", "Areas", "rules-save"),
	];
}

/**
 * The list. Order is the order of the tool bar, and of the palette until
 * somebody types into it.
 */
export const COMMANDS = [
	...tools(),
	{
		id: "edit.undo", label: "Undo", group: "Edit", always: true, menu: "Edit", icon: "undo", bar: true,
		keys: ["Ctrl+Z"],
		enabled: p => {
			const history = p.editor.history();
			return history !== null && history.canUndo;
		},
		run: p => p.stepHistory(() => p.editor.undo()),
	},
	{
		id: "edit.redo", label: "Redo", group: "Edit", always: true, menu: "Edit", icon: "redo", bar: true,
		keys: ["Ctrl+Y", "Ctrl+Shift+Z"],
		enabled: p => {
			const history = p.editor.history();
			return history !== null && history.canRedo;
		},
		run: p => p.stepHistory(() => p.editor.redo()),
	},
	{
		// The one button in the bar for everything about looking: the six
		// switches and the zooms stand behind it.
		id: "view.menu", label: "View", group: "View", safe: true, always: true, toggle: true,
		role: "view-menu", icon: "eye", bar: true, palette: false,
		pressed: p => p.context !== null && p.context.dataset.menu === "view",
		run: p => p.showViewMenu(p.part("view-menu")),
	},
	{
		id: "file.open", label: "Open map…", group: "File", icon: "folder", menu: "File",
		keys: ["Ctrl+O"],
		run: p => p.openMap(),
	},
	{
		id: "file.new", label: "New map…", group: "File", icon: "add", menu: "File",
		// Ctrl+N belongs to the browser and cannot be taken from it.
		keys: ["Ctrl+Alt+N"],
		run: p => p.askNewMap(),
	},
	{
		id: "file.save", label: "Save", group: "File", safe: true, icon: "save", bar: true, menu: "File",
		keys: ["Ctrl+S"],
		enabled: p => p.map !== null,
		run: p => p.editor.save(),
	},
	{
		// The maps in the browser's storage: what autosave wrote, and what
		// "Save" always writes on its way out to the downloads.
		id: "file.openSaved", label: "Open from browser storage…", group: "File", menu: "File",
		keys: ["Ctrl+Alt+O"],
		run: p => p.askOpenSaved(),
	},
	{
		id: "file.saveCopy", label: "Save a copy…", group: "File", menu: "File",
		enabled: p => p.map !== null,
		run: p => p.askSaveCopy(),
	},
	{
		id: "file.picture", label: "Export as image", group: "File", safe: true, menu: "File",
		keys: ["Ctrl+Shift+E"],
		enabled: p => p.map !== null && p.editor.pictureState() !== 1,
		run: p => p.exportPicture(),
	},
	{
		id: "file.saveAs", label: "Save as…", group: "File", menu: "File",
		keys: ["Ctrl+Shift+S"],
		enabled: p => p.map !== null,
		run: p => p.askSaveAs(),
	},
	{
		id: "file.append", label: "Append map…", group: "File", menu: "File",
		keys: ["Ctrl+Shift+A"],
		enabled: p => p.map !== null,
		run: p => p.part("append-file").click(),
	},
	{
		id: "view.fit", label: "Zoom to fit", group: "View", safe: true, menu: "View", icon: "fit", view: true,
		keys: ["Home"],
		run: p => p.editor.fit(),
	},
	{
		id: "view.zoomIn", label: "Zoom in", group: "View", safe: true, menu: "View",
		keys: ["NumpadAdd", "+"],
		run: p => p.editor.zoom(p.editor.zoom() / ZOOM_STEP),
	},
	{
		id: "view.zoomOut", label: "Zoom out", group: "View", safe: true, menu: "View",
		keys: ["NumpadSubtract", "-"],
		run: p => p.editor.zoom(p.editor.zoom() * ZOOM_STEP),
	},
	{
		id: "view.zoomReset", label: "Zoom to 100 %", group: "View", safe: true, menu: "View", view: true,
		keys: ["NumpadMultiply"],
		run: p => p.editor.zoom(1),
	},
	looking("view.detail", "High detail", "detail", ["Ctrl+H"],
		p => p.editor.highDetail(), (p, on) => p.editor.highDetail(on)),
	looking("view.entities", "Show entities", "entities", ["Ctrl+Alt+E"],
		p => p.editor.entities() > 0, (p, on) => p.editor.entities(on ? 100 : 0)),
	looking("view.animate", "Play animations", "play", ["Ctrl+M"],
		p => p.editor.animate(), (p, on) => p.editor.animate(on)),
	Object.assign(looking("view.grid", "Show grid", "grid", ["G", "Ctrl+G"],
		p => p.editor.grid() > 0, (p, on) => p.editor.grid(on ? GRID_SPACING : 0)), { always: true }),
	{
		id: "view.proof", label: "Proof mode", group: "View", safe: true, menu: "View", icon: "proof", view: true, toggle: true,
		keys: ["P"],
		pressed: p => p.proof !== "off",
		run: p => {
			p.proof = p.proof === "off" ? "game" : (p.proof === "game" ? "menu" : "off");
			p.refreshBar();
			p.refreshOverlay();
		},
	},
	{
		id: "view.tileInfo", label: "Tile info", group: "View", safe: true, menu: "View",
		icon: "info", view: true, toggle: true,
		keys: ["Ctrl+I"],
		pressed: p => p.tileInfo !== "off",
		run: p => {
			const next = TILE_INFO_WAYS.indexOf(p.tileInfo) + 1;
			p.tileInfo = TILE_INFO_WAYS[next % TILE_INFO_WAYS.length];
			p.say(`Tile info: ${p.tileInfo}`);
		},
	},
	{
		id: "brush.flipX", label: "Flip brush horizontally", group: "Brush", menu: "Tools",
		keys: ["X", "N"],
		run: p => {
			p.editor.flipBrushX();
			p.refreshTiles();
		},
	},
	{
		id: "brush.flipY", label: "Flip brush vertically", group: "Brush", menu: "Tools",
		keys: ["Y", "M"],
		run: p => {
			p.editor.flipBrushY();
			p.refreshTiles();
		},
	},
	{
		id: "brush.rotate", label: "Rotate brush", group: "Brush", menu: "Tools",
		keys: ["R"],
		run: p => {
			p.editor.rotateBrush();
			p.refreshTiles();
		},
	},
	{
		id: "brush.rotateBack", label: "Rotate brush back", group: "Brush", menu: "Tools",
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
		id: "layer.addGroup", label: "Add group", group: "Layer", menu: "Layer",
		keys: ["Ctrl+Shift+G"],
		enabled: p => p.map !== null,
		run: p => p.change(() => p.editor.apply({ op: "group.add", name: "group" })),
	},
	...layerKinds(),
	{
		id: "layer.next", label: "Next layer", group: "Layer", safe: true,
		keys: ["ArrowDown"],
		run: p => p.stepSelection(1),
	},
	{
		id: "layer.previous", label: "Previous layer", group: "Layer", safe: true,
		keys: ["ArrowUp"],
		run: p => p.stepSelection(-1),
	},
	{
		id: "layer.up", label: "Move up", group: "Layer", menu: "Layer", for: ["layer", "group"],
		keys: ["Ctrl+ArrowUp"],
		run: p => p.moveSelected(-1),
	},
	{
		id: "layer.down", label: "Move down", group: "Layer", menu: "Layer", for: ["layer", "group"],
		keys: ["Ctrl+ArrowDown"],
		run: p => p.moveSelected(1),
	},
	{
		id: "layer.hide", label: "Show or hide layer", group: "Layer", safe: true, menu: "Layer", for: "layer",
		keys: ["V"],
		enabled: p => p.selection.layer >= 0,
		run: p => {
			const where = p.selection;
			p.editor.visible(where.group, where.layer, !p.editor.visible(where.group, where.layer));
			p.refresh();
		},
	},
	{
		id: "layer.delete", label: "Delete layer or group", group: "Layer", menu: "Layer", for: ["layer", "group"],
		keys: ["Ctrl+Delete"],
		enabled: p => p.map !== null,
		run: p => p.deleteSelected(),
	},
	{
		id: "edit.delete", label: "Delete selected", group: "Edit", menu: "Edit",
		keys: ["Delete"],
		run: p => p.deletePicked(),
	},
	{
		id: "quad.add", label: "Add quad", group: "Quads", menu: "Layer",
		keys: ["Q"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "quads";
		},
		run: p => p.part("add-quad").click(),
	},
	{
		id: "quad.knife", label: "Quad knife", group: "Quads", menu: "Tools",
		keys: ["K"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "quads" && p.quad >= 0;
		},
		pressed: p => p.carving !== null,
		run: p => p.knife(),
	},
	{
		id: "tiles.numbers", label: "Edit tile numbers", group: "Brush", menu: "Tools",
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
		id: "tiles.border", label: "Add border", group: "Brush", menu: "Tools",
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "tiles" && !p.editor.brushEmpty();
		},
		run: p => p.makeBorder(),
	},
	{
		id: "envelope.deleteUnused", label: "Delete unused envelopes", group: "Envelopes", menu: "Tools",
		enabled: p => p.map !== null && p.map.envelopes !== undefined && p.map.envelopes.length > 0,
		run: p => p.deleteUnusedEnvelopes(),
	},
	{
		id: "tiles.nextFree", label: "Next free number", group: "Brush", menu: "Tools",
		keys: ["Ctrl+F"],
		enabled: p => p.part("next-free") !== null && !p.part("next-free").disabled,
		run: p => p.part("next-free").click(),
	},
	// The four panels that are looked at now and then, wherever each of them
	// stands: the key and the switch in the status line open it there and
	// shut it again.
	...[
		["envelopes", "Envelopes", "Ctrl+E", "envelopes-panel"],
		["history", "History", "Ctrl+Shift+H", "history-panel"],
		["settings", "Server settings", "Ctrl+Shift+E", "settings-panel"],
		["rules", "Rules", "Ctrl+Shift+R", "rules-panel"],
	].map(([tab, label, key, role]) => ({
		id: `panel.${tab}`, label: label, group: "Panels", safe: true, menu: "View/Panels",
		keys: [key],
		pressed: p => p.panelInFront(role),
		run: p => p.togglePanel(role),
	})),
	{
		id: "area.left", label: "Inspector", group: "Panels", safe: true, menu: "View", icon: "properties",
		keys: ["["],
		pressed: p => p.areaShown("left"),
		run: p => p.showArea("left", !p.areaShown("left")),
	},
	{
		id: "area.right", label: "Layers and tiles", group: "Panels", safe: true, menu: "View",
		// A button in the bar only while the column is a drawer: a column
		// that is always there needs no button, a drawer that is not has
		// nothing else to open it with.
		icon: "layerTiles", bar: true, whenDrawer: "right",
		keys: ["]"],
		pressed: p => p.areaShown("right"),
		run: p => p.showArea("right", !p.areaShown("right")),
	},
	{
		id: "area.mapOnly", label: "Map only", group: "Panels", safe: true, menu: "View",
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
		id: "image.add", label: "Add image…", group: "Layer", menu: "Layer",
		keys: ["Ctrl+Shift+I"],
		enabled: p => p.map !== null,
		run: p => p.part("image-file").click(),
	},
	{
		id: "sound.add", label: "Add sound…", group: "Layer", menu: "Layer",
		keys: ["Ctrl+Shift+U"],
		enabled: p => p.map !== null,
		run: p => p.part("sound-file").click(),
	},
	{
		id: "source.add", label: "Add sound source", group: "Quads", menu: "Layer",
		keys: ["Ctrl+Shift+S"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "sounds";
		},
		run: p => p.part("add-source").click(),
	},
	{
		id: "file.close", label: "Close map", group: "File", menu: "File",
		// Ctrl+W and Ctrl+F4 belong to the browser.
		keys: ["Ctrl+Alt+W"],
		enabled: p => p.map !== null,
		run: p => {
			// Through the panels rather than straight into the program: with
			// more than one map open this is the tab's cross, and a map with
			// changes in it asks before it goes.
			if (p.editor.maps.length > 1) {
				p.closeMap(p.editor.map);
				return;
			}
			p.editor.close();
			p.refresh();
		},
	},
	...structureTabs(),
	{
		id: "picker.show", label: "Tile picker", group: "Brush", safe: true, menu: "Tools",
		// A key held on a desk; a button in the tile panel for a hand that
		// holds nothing.
		icon: "expand", role: "tiles-big",
		keys: ["Space"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "tiles";
		},
		pressed: p => p.picker !== null && !p.picker.hidden,
		run: p => p.showPicker(p.picker === null || p.picker.hidden),
	},
	{
		id: "picker.pin", label: "Pin tile picker", group: "Brush", safe: true,
		keys: ["Ctrl+Space"],
		enabled: p => {
			const layer = p.selectedLayer();
			return layer !== null && layer.type === "tiles";
		},
		pressed: p => p.pickerPinned,
		run: p => {
			p.pickerPinned = !p.pickerPinned;
			p.showPicker(p.pickerPinned || (p.picker !== null && !p.picker.hidden));
			p.pickerPinned = p.picker !== null && !p.picker.hidden ? p.pickerPinned : false;
		},
	},
	{
		id: "brush.clear", label: "Clear brush", group: "Brush", menu: "Tools",
		// A button in the tile panel. An empty brush is what grabs, so this
		// is also the way to a rectangle.
		icon: "close", role: "clear-brush",
		enabled: p => p.map !== null,
		run: p => {
			p.editor.clearBrush();
			// On purpose, so the next refresh does not fill it again.
			p.brushCleared = true;
			p.picked = null;
			p.refreshTiles();
			p.refreshStatus();
			p.say("No brush - drag to select tiles, or click a tile on the right");
		},
	},
	{
		id: "layer.here", label: "Pick layer here", group: "Layer", menu: "Layer",
		// Ctrl and the right button; the Pick tool is the same thing as a
		// tool, so this has no button of its own.
		icon: "pick", safe: true, role: "layer-here",
		enabled: p => p.map !== null,
		pressed: p => p.askingLayer,
		run: p => p.askHere(),
	},
	{
		id: "palette.open", label: "Command palette", group: "Help", safe: true, always: true,
		// Its own name for its button: `commandRole` would call it "open",
		// and so would the menu's, and two buttons cannot share one name.
		role: "palette",
		icon: "search", bar: true, menu: "Help", palette: false,
		keys: ["Ctrl+P"],
		pressed: p => p.palette !== null && !p.palette.hidden,
		run: p => p.showPalette(p.palette === null || p.palette.hidden),
	},
	{
		id: "menu.open", label: "Menu", group: "Help", safe: true, always: true,
		role: "menu",
		icon: "menu", bar: true, palette: false,
		keys: ["Alt+M"],
		pressed: p => p.menu !== null && !p.menu.hidden,
		run: p => p.showMenu(p.menu === null || p.menu.hidden),
	},
	{
		id: "view.scheme", label: "Light theme", group: "View", safe: true, menu: "Settings",
		keys: ["Ctrl+Alt+L"],
		pressed: p => p.scheme() === "light",
		run: p => p.scheme(p.scheme() === "light" ? "dark" : "light"),
	},
	{
		// Three states rather than a switch: the query is right nearly always,
		// and the two answers are for the two known cases where it lies - a
		// touch laptop with a mouse, and a tablet in desktop mode.
		id: "view.targets", label: "Big targets", group: "View", safe: true, menu: "Settings",
		keys: ["Ctrl+Alt+T"],
		pressed: p => p.finger(),
		run: p => {
			const next = { auto: "big", big: "small", small: "auto" }[p.targets()];
			p.targets(next);
			p.say(next === "auto" ? "Big targets: as the browser says"
				: next === "big" ? "Big targets: on" : "Big targets: off");
		},
	},
	{
		id: "settings.entities", label: "Entities image…", group: "Settings", safe: true, menu: "Settings",
		run: p => p.askEntitiesImage(),
	},
	{
		id: "settings.brushColouring", label: "Tint tileset with layer colour", group: "Settings", safe: true, menu: "Settings",
		pressed: p => p.brushColouring,
		run: p => {
			p.brushColouring = !p.brushColouring;
			p.refresh();
		},
	},
	{
		id: "settings.penHoldsPaper", label: "Pen mode: a finger pans", group: "Settings", safe: true, menu: "Settings",
		pressed: p => p.penHoldsPaper,
		run: p => {
			p.penHoldsPaper = !p.penHoldsPaper;
			p.refresh();
		},
	},
	{
		// Ctrl+U, as in the native editor.
		id: "settings.allowUnused", label: "Allow unused tiles", group: "Settings", menu: "Settings",
		keys: ["Ctrl+U"],
		pressed: p => p.editor.allowUnused(),
		run: p => {
			const now = p.editor.allowUnused(!p.editor.allowUnused());
			p.say(now ? "Unused tiles may be put down" : "Unused tiles go down as air");
			p.refreshBar();
		},
	},
	{
		// Not configurable - the keys are the table's, and the table is one
		// place. What this is, is the sheet one looks at to find out what the
		// keys are, which is what one actually wants from a shortcut dialogue.
		id: "help.keys", label: "Keyboard shortcuts", group: "Help", safe: true, menu: "Settings",
		keys: ["Ctrl+/"],
		run: p => p.showKeys(),
	},
	{
		id: "help.wiki", label: "Mapping wiki", group: "Help", safe: true, menu: "Help",
		keys: ["F1"],
		run: () => window.open("https://wiki.ddnet.org/wiki/Mapping", "_blank", "noopener"),
	},
	{
		// The browser's own convention for walking from one part of a page
		// to the next.
		id: "focus.next", label: "Next area", group: "View", safe: true,
		keys: ["F6"],
		run: p => p.focusArea(1),
	},
	{
		id: "focus.previous", label: "Previous area", group: "View", safe: true,
		keys: ["Shift+F6"],
		run: p => p.focusArea(-1),
	},
	{
		id: "edit.escape", label: "Focus the map", group: "Edit", safe: true,
		keys: ["Escape"],
		run: p => p.escape(),
	},
	...slots(),
	...panelButtons(),
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
export function commandTitle(command, withKeys = true) {
	const keys = command.keys || [];
	return keys.length === 0 || !withKeys ? command.label : `${command.label} (${keyLabel(keys[0])})`;
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
