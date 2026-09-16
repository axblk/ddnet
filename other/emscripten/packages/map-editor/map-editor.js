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
	grid: '<path d="M9 3v18M15 3v18M3 9h18M3 15h18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
	proof: '<rect x="1.8" y="5" width="20.4" height="14" rx="2" fill="none" stroke="currentColor" stroke-width="2"/><rect x="6.2" y="8" width="11.6" height="8" fill="none" stroke="currentColor" stroke-width="1.6" stroke-dasharray="2.4 1.8"/>',
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

// How many tiles apart the lines of the grid are when it is switched on. Ten,
// because that is what somebody counting tiles counts in.
const GRID_SPACING = 10;

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

	/** Where in its history each map stood when it was last written out. */
	savedAt = new Map();
	/** What `autosave` set going, 0 while nothing is. */
	autosaveTimer = 0;
	// The table of what a server would accept, once it has been asked for.
	settingsKnown = null;

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

	/**
	 * Writes the map out.
	 *
	 * It always goes into the browser's own storage, where it survives the
	 * tab; `{handout: false}` stops it from also going out to wherever the
	 * user keeps their files, which is what an autosave wants.
	 */
	save(id, options) {
		const map = this.which(id);
		const handout = !(options && options.handout === false);
		if (this.ask("MapEditorSave", "number", [map, handout ? 1 : 0]) !== 1) {
			return false;
		}
		const history = this.history(map);
		if (history !== null) {
			this.savedAt.set(map, history.current);
		}
		return true;
	}

	/**
	 * Whether the map has been changed since it was last written out.
	 *
	 * It is the place in the history that is compared, not the bytes: undoing
	 * back to where the map was saved makes it unchanged again, which is what
	 * somebody who undid their way back would say themselves.
	 */
	dirty(id) {
		const map = this.which(id);
		const history = this.history(map);
		return history !== null && history.current !== (this.savedAt.get(map) || 0);
	}

	/**
	 * Writes every map that has been changed into the browser's own storage
	 * every so many seconds, and nowhere else - a page that dropped a file
	 * into the downloads every minute is a page nobody leaves open. Called
	 * with 0 it stops.
	 */
	autosave(seconds) {
		if (this.autosaveTimer !== 0) {
			clearInterval(this.autosaveTimer);
			this.autosaveTimer = 0;
		}
		if (!seconds) {
			return;
		}
		this.autosaveTimer = setInterval(() => {
			for (const id of this.maps) {
				if (this.dirty(id)) {
					this.save(id, { handout: false });
				}
			}
		}, seconds * 1000);
		this.addEventListener("exit", () => this.autosave(0), { once: true });
	}

	/**
	 * The pixels of a picture that is packed into the map file, as an
	 * `ImageData` the page can draw - `null` for a picture that lies beside
	 * the map, which the browser fetches itself.
	 *
	 * The bytes are copied out of the program on the spot, because what the
	 * program hands over is where they lie rather than a copy, and where they
	 * lie stops being true the moment the map changes.
	 */
	imageData(index, id) {
		const map = this.which(id);
		const width = this.ask("MapEditorImageWidth", "number", [map, index]);
		const height = this.ask("MapEditorImageHeight", "number", [map, index]);
		const address = this.ask("MapEditorImagePixels", "number", [map, index]);
		if (!width || !height || !address) {
			return null;
		}
		const heap = this.module == null ? undefined : this.module.HEAPU8;
		if (heap === undefined) {
			return null;
		}
		return new ImageData(new Uint8ClampedArray(heap.subarray(address, address + width * height * 4)), width, height);
	}

	/**
	 * What tile stands in one place of a layer, or -1 where there is none.
	 *
	 * One at a time, the way the quads and the envelope points are asked
	 * for: a map of four million tiles is not a thing to hand out after
	 * every stroke.
	 */
	tileIndex(group, layer, x, y, id) {
		return this.ask("MapEditorTileIndex", "number", [this.which(id), group, layer, x, y]);
	}

	/**
	 * Keeps a `.rules` file under a name, parsed, and says how many
	 * configurations it holds.
	 *
	 * The file is not read by the program: the page fetches it, because a
	 * page fetches things. The name is the one the map calls the picture,
	 * because that is how a layer finds its rules.
	 */
	loadRules(name, text) {
		return this.call("MapEditorLoadRules", "number", ["string", "string"], [name, text]) || 0;
	}

	/**
	 * Which lines of a rules file were passed over, counting from one.
	 *
	 * A rules file is read as far as it is understood, so a line the grammar
	 * has no word for stops nothing - but somebody writing one wants to be
	 * told which line it was.
	 */
	ruleProblems(name) {
		const answer = this.call("MapEditorRuleProblems", "string", ["string"], [name]);
		try {
			return JSON.parse(answer || "[]");
		} catch (error) {
			return [];
		}
	}

	/** What the configurations of a rules file that was loaded are called. */
	ruleConfigs(name) {
		const count = this.call("MapEditorNumRuleConfigs", "number", ["string"], [name]) || 0;
		const names = [];
		for (let index = 0; index < count; ++index) {
			names.push(this.call("MapEditorRuleConfigName", "string", ["string", "number"], [name, index]) || "");
		}
		return names;
	}

	/**
	 * Runs one configuration of a rules file over a layer, or over a piece of
	 * one. One call is one history entry.
	 *
	 * @param options.seed 0 for one that is made up, which means the answer
	 * is not the same twice.
	 * @param options.reference Which physics tile the first run is filtered
	 * by, -1 for none.
	 * @param options.x The rectangle, in tiles; left out it is the whole
	 * layer.
	 */
	automap(group, layer, rules, config, options) {
		const settings = Object.assign({ seed: 0, reference: -1, x: 0, y: 0, width: -1, height: -1, id: undefined }, options || {});
		return this.call("MapEditorAutomap", "number",
			["number", "number", "number", "string", "number", "number", "number", "number", "number", "number", "number"],
			[this.which(settings.id), group, layer, rules, config, settings.seed, settings.reference,
				settings.x, settings.y, settings.width, settings.height]) === 1;
	}

	/**
	 * Puts a picture into the map with its pixels, and says which picture of
	 * the map it became - or -1 where it was refused.
	 *
	 * The pixels do not go through a command, because they are bytes: a
	 * picture of a thousand by a thousand is four megabytes, and four
	 * megabytes of JSON is a text nobody should have to write or read. The
	 * page decodes the PNG - browsers do that - and hands over what came out.
	 *
	 * @param name What to call it in the map.
	 * @param pixels An `ImageData`, which is what a canvas gives back.
	 */
	addImage(name, pixels, id) {
		return this.withPixels(pixels, (address, width, height) =>
			this.call("MapEditorAddImage", "number", ["number", "string", "number", "number", "number"],
				[this.which(id), name, width, height, address]));
	}

	/**
	 * Puts other pixels into a picture the map already has, keeping every
	 * layer that is drawn with it - which is what replacing a picture is for.
	 */
	setImagePixels(index, pixels, id) {
		return this.withPixels(pixels, (address, width, height) =>
			this.ask("MapEditorSetImagePixels", "number", [this.which(id), index, width, height, address]) === 1);
	}

	// Hands the pixels to the program and takes the room back again. The
	// program copies what it keeps, so the room is only needed for the call.
	withPixels(pixels, work) {
		const module = this.module;
		if (module == null || pixels == null || !pixels.width || !pixels.height) {
			return null;
		}
		const bytes = pixels.width * pixels.height * 4;
		const address = module._malloc(bytes);
		if (!address) {
			return null;
		}
		try {
			module.HEAPU8.set(new Uint8Array(pixels.data.buffer, pixels.data.byteOffset, bytes), address);
			return work(address, pixels.width, pixels.height);
		} finally {
			module._free(address);
		}
	}

	/**
	 * Reads a sound into the map as bytes, and says which one it became.
	 *
	 * The bytes are an Opus file - what is in them is nobody's business here;
	 * the map keeps them and the browser is the one that can play them.
	 */
	addSound(name, bytes, id) {
		return this.withBytes(bytes, (address, size) =>
			this.call("MapEditorAddSound", "number", ["number", "string", "number", "number"],
				[this.which(id), name, size, address]));
	}

	/** Puts other bytes into a sound the map already has. */
	setSoundData(index, bytes, id) {
		return this.withBytes(bytes, (address, size) =>
			this.ask("MapEditorSetSoundData", "number", [this.which(id), index, size, address]) === 1);
	}

	/**
	 * The bytes of a sound the map holds, as a `Uint8Array`, or null where it
	 * holds none - a sound that lies beside the map has none here. What comes
	 * out goes back in through `setSoundData` unchanged.
	 *
	 * A copy, because the program's own bytes are only good until the map
	 * changes and whoever plays a sound holds it for longer than that.
	 */
	soundData(index, id) {
		const module = this.module;
		const map = this.which(id);
		const size = this.ask("MapEditorSoundSize", "number", [map, index]);
		if (module == null || size === null || size <= 0) {
			return null;
		}
		const address = this.ask("MapEditorSoundData", "number", [map, index]);
		if (!address) {
			return null;
		}
		return module.HEAPU8.slice(address, address + size);
	}

	// Hands a block of bytes to the program and takes the room back again.
	// The program copies what it keeps, the same as with the pixels.
	withBytes(bytes, work) {
		const module = this.module;
		const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || 0);
		if (module == null || view.length === 0) {
			return null;
		}
		const address = module._malloc(view.length);
		if (!address) {
			return null;
		}
		try {
			module.HEAPU8.set(view, address);
			return work(address, view.length);
		} finally {
			module._free(address);
		}
	}

	/** What the map is made of: groups, layers, envelopes, images, sounds. */
	structure(id) {
		return this.json("MapEditorStructure", [this.which(id)]);
	}

	/**
	 * The quads of one layer: their five points in world units, their four
	 * corner colours, and which envelopes move and colour them.
	 *
	 * `null` for a layer that holds no quads, which is how a caller finds out
	 * what sort of layer it is looking at.
	 */
	quads(group, layer, id) {
		return this.json("MapEditorQuads", [this.which(id), group, layer]);
	}

	/**
	 * Puts handles on the corners of one quad, or takes them away when called
	 * with nothing. Drawn by the program because a quad lies in its group's
	 * coordinates, parallax and all.
	 */
	showQuad(group, layer, quad, id) {
		return group === undefined || group === null
			? this.setNumbers("MapEditorShowQuad", [this.which(id), 0, 0, 0, 0])
			: this.setNumbers("MapEditorShowQuad", [this.which(id), group, layer, quad, 1]);
	}

	/**
	 * Where a point on the canvas is in the coordinates of one group, in
	 * world units.
	 *
	 * Not the same as `worldAt`, which answers for the plain view: a group
	 * with parallax shows a different piece of the world, and a quad in it
	 * lives in that piece.
	 */
	groupWorldAt(group, x, y, id) {
		const map = this.which(id);
		const wx = this.ask("MapEditorGroupWorldX", "number", [map, group, x, y]);
		return wx === null ? null : { x: wx, y: this.ask("MapEditorGroupWorldY", "number", [map, group, x, y]) };
	}

	/**
	 * The other way round: where a place in one group's coordinates is on the
	 * canvas, in pixels from its top left.
	 *
	 * What an overlay drawn in the page has to ask. Working it out here
	 * instead would mean keeping a second copy of the sum the renderer draws
	 * with, and the two would drift apart the first time one of them changed.
	 */
	groupPixelAt(group, x, y, id) {
		const map = this.which(id);
		const px = this.ask("MapEditorGroupPixelX", "number", [map, group, x, y]);
		return px === null ? null : { x: px, y: this.ask("MapEditorGroupPixelY", "number", [map, group, x, y]) };
	}

	/**
	 * The sound sources of one layer, or null for a layer that holds none.
	 *
	 * Places and sizes in world units, the same as the quads, because that is
	 * the number a page can turn a click into and back.
	 */
	sources(group, layer, id) {
		return this.json("MapEditorSoundSources", [this.which(id), group, layer]);
	}

	/**
	 * What a player would see from where the view is looking, in the game
	 * layer's world units.
	 *
	 * Asked afresh whenever the view moves, because that is what it is about:
	 * the rectangle belongs to the place, not to the moment.
	 *
	 * @param menu Whether to answer for a menu background rather than a game.
	 */
	proof(menu, id) {
		return this.json("MapEditorProof", [this.which(id), menu ? 1 : 0]);
	}

	/**
	 * What a tile of a physics layer does, in a sentence, or "" where there
	 * is nothing to say.
	 *
	 * The same sentences the editor in the client shows: somebody who learned
	 * what a tile does in one editor should not be told something else in the
	 * other.
	 */
	explain(group, layer, index, id) {
		return this.ask("MapEditorExplain", "string", [this.which(id), group, layer, index]) || "";
	}

	/**
	 * Everything a map may say to a server, with what it means and what it
	 * takes.
	 *
	 * Asked once and kept: the list does not change while the editor runs.
	 */
	settingsHelp() {
		if (this.settingsKnown === null) {
			this.settingsKnown = this.json("MapEditorSettingsHelp", []) || [];
		}
		return this.settingsKnown;
	}

	/** What is wrong with each settings line, and where each repeats an earlier one. */
	settingProblems(id) {
		return this.json("MapEditorSettingProblems", [this.which(id)]) || [];
	}

	/**
	 * Puts a second map into the one that is open: everything it draws, and
	 * the pictures, sounds and envelopes it draws with, and the lines it asks
	 * of a server. Not its game layer - physics belongs to the map being
	 * worked on, and two game layers is not a map.
	 *
	 * One history entry, however much comes over. Answers what came over, or
	 * null where the file could not be read.
	 */
	async appendFile(file, id) {
		const where = this.filePath({ name: file.name });
		if (where === null) {
			return null;
		}
		const path = await this.writeFile(where, file.name, new Uint8Array(await file.arrayBuffer()));
		// The file is where the page put it rather than anywhere the program
		// would look for it, so it is named absolutely (`TYPE_ABSOLUTE`).
		const text = this.call("MapEditorAppend", "string", ["number", "string", "number"],
			[this.which(id), path, STORAGE_ABSOLUTE]);
		try {
			return text === null || text === "null" ? null : JSON.parse(text);
		} catch (error) {
			return null;
		}
	}

	/**
	 * What is wrong with one settings line, or "" where nothing is.
	 *
	 * Asked about a line that is not in the map yet, which is the whole point:
	 * saying so after it has been put in is saying so too late.
	 */
	checkSetting(line) {
		return this.call("MapEditorCheckSetting", "string", ["string"], [line || ""]) || "";
	}

	/** The names of settings that begin with what has been typed. */
	settingNames(prefix) {
		const text = this.call("MapEditorSettingNames", "string", ["string"], [prefix || ""]);
		try {
			return text === null ? [] : JSON.parse(text);
		} catch (error) {
			return [];
		}
	}

	/**
	 * The lowest number no tile of a physics layer is using yet, or -1 where
	 * all 255 are taken.
	 *
	 * The checkpoints of a tele layer keep their own count, which is why
	 * there is a flag for them.
	 */
	nextFreeNumber(group, layer, checkpoint, id) {
		return this.ask("MapEditorNextFreeNumber", "number", [this.which(id), group, layer, checkpoint ? 1 : 0]);
	}

	/**
	 * Moves the view to where a number is used, and answers how many such
	 * places there are - zero when the view did not move.
	 *
	 * Which of them is the page's to count: a button that is pressed twice
	 * goes to the next one, and the program is not the one that knows it was
	 * pressed twice. Tiles closer together than ten are one place, so a
	 * teleporter four tiles wide is somewhere to go rather than four.
	 */
	gotoNumber(group, layer, number, which, id) {
		return this.ask("MapEditorGotoNumber", "number", [this.which(id), group, layer, number, which]);
	}

	/**
	 * The points of one envelope.
	 *
	 * Not in `structure()`, which says only how many there are: a long
	 * envelope has hundreds of points and nearly nothing wants them. Times
	 * are whole milliseconds and values the map's own 22.10 fixed point -
	 * whole numbers out and whole numbers back in, so nothing is lost on the
	 * way. What a value means is a question about the channels.
	 */
	envelope(index, id) {
		return this.json("MapEditorEnvelope", [this.which(id), index]);
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
	 *
	 * `merge` names what is being changed rather than what is being done:
	 * two changes carrying the same one, close enough together, become one
	 * entry. That is a number field stepped with its arrows, where each step
	 * is its own change but ten of them are one thing to undo. Leave it out
	 * and the change stands alone, which is what a brush stroke wants.
	 */
	begin(label, id, merge) {
		return this.call("MapEditorBegin", null, ["number", "string", "string"],
			[this.which(id), label || "Change", merge || ""]);
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

	/**
	 * Whether a layer is drawn, so that somebody can look under it.
	 *
	 * This changes nothing about the map: no version, no history entry,
	 * nothing that is saved. It names the layer by where it is, so moving a
	 * layer leaves what is hidden where it was.
	 */
	visible(group, layer, on, id) {
		const map = this.which(id);
		return on === undefined
			? this.ask("MapEditorLayerVisible", "number", [map, group, layer]) === 1
			: this.setNumbers("MapEditorSetLayerVisible", [map, group, layer, on ? 1 : 0]);
	}

	/**
	 * Marks a rectangle of tiles, which is what a gesture about an area shows
	 * while it is being made: taking a piece of a layer into the brush,
	 * filling it, rubbing it out. Called with nothing it takes the mark away.
	 *
	 * The rectangle is drawn in the tiles of that group, so it sits on them
	 * at every zoom, and it belongs to looking rather than to the map: no
	 * version, no history entry, nothing that is saved.
	 */
	mark(group, x, y, width, height, id) {
		return group === undefined || group === null
			? this.setNumbers("MapEditorMark", [this.which(id), 0, 0, 0, 0, 0])
			: this.setNumbers("MapEditorMark", [this.which(id), group, x, y, width, height]);
	}

	/**
	 * How many tiles apart the lines of the grid are, or 0 for no grid.
	 *
	 * The grid follows the group the game layer is in, so its lines sit on
	 * that group's tiles at every zoom, and it is left out when its lines
	 * would be closer together than a few pixels.
	 */
	grid(spacing, id) {
		const map = this.which(id);
		return spacing === undefined
			? this.ask("MapEditorGrid", "number", [map])
			: this.setNumbers("MapEditorSetGrid", [map, spacing === true ? 1 : (spacing === false ? 0 : spacing)]);
	}

	/**
	 * The brush: what is in hand and what putting it down does.
	 *
	 * A stroke is one change made of many stamps: open it with `begin` when
	 * the button goes down, call `paint` on every move, close it with
	 * `commit` when the button comes up. The history gets one entry, however
	 * many tiles were touched, and every step in between is already drawn.
	 *
	 * Everything below answers `false` where the brush may not go in that
	 * layer at all - a tele brush in a game layer, say.
	 */
	pickTiles(group, layer, x, y, width, height, id) {
		return this.ask("MapEditorPickTiles", "number",
			[this.which(id), group, layer, x, y, width === undefined ? 1 : width, height === undefined ? 1 : height]) === 1;
	}
	grab(group, layer, x, y, width, height, id) {
		return this.ask("MapEditorGrab", "number", [this.which(id), group, layer, x, y, width, height]) === 1;
	}
	paint(group, layer, x, y, id) {
		return this.ask("MapEditorPaint", "number", [this.which(id), group, layer, x, y]) === 1;
	}
	fill(group, layer, x, y, width, height, id) {
		return this.ask("MapEditorFill", "number", [this.which(id), group, layer, x, y, width, height]) === 1;
	}
	erase(group, layer, x, y, width, height, id) {
		return this.ask("MapEditorErase", "number", [this.which(id), group, layer, x, y, width, height]) === 1;
	}
	flipBrushX() {
		return this.call("MapEditorFlipBrushX");
	}
	flipBrushY() {
		return this.call("MapEditorFlipBrushY");
	}
	rotateBrush() {
		return this.call("MapEditorRotateBrush");
	}
	/** Ten slots to put a brush in and take it out of again. */
	storeBrush(slot) {
		return this.ask("MapEditorStoreBrush", "number", [slot]) === 1;
	}
	useBrush(slot) {
		return this.ask("MapEditorUseBrush", "number", [slot]) === 1;
	}
	/** How big the brush is, in tiles. */
	/**
	 * What goes beside a physics tile the brush puts down: which tele, which
	 * switch and how long it waits, how hard and which way a speedup pushes.
	 *
	 * Called with nothing it answers what the brush is carrying, which is
	 * what a grab or a stored brush brought back with it. Called with an
	 * object it sets them and writes them onto the brush in hand.
	 */
	numbers(values) {
		if (values === undefined) {
			return {
				number: this.call("MapEditorNumber", "number") || 0,
				delay: this.call("MapEditorDelay", "number") || 0,
				force: this.call("MapEditorForce", "number") || 0,
				maxSpeed: this.call("MapEditorMaxSpeed", "number") || 0,
				angle: this.call("MapEditorAngle", "number") || 0,
			};
		}
		const now = this.numbers();
		const pick = name => (values[name] === undefined ? now[name] : Math.round(values[name]));
		return this.setNumbers("MapEditorSetNumbers",
			[pick("number"), pick("delay"), pick("force"), pick("maxSpeed"), pick("angle")]);
	}

	brushSize() {
		const width = this.call("MapEditorBrushWidth", "number");
		return width === null ? null : { width: width, height: this.call("MapEditorBrushHeight", "number") };
	}

	/**
	 * Whether the tiles in hand are tele checkpoints, which count their
	 * numbers apart from the teleporters. What a tile index means is the
	 * program's to know.
	 */
	brushCheckpoint() {
		return this.call("MapEditorBrushCheckpoint", "number") === 1;
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
	<button class="editor-button" data-role="grid" data-icon="grid" title="A grid on the tiles (G)" aria-pressed="false"></button>
	<button class="editor-button" data-role="proof" data-icon="proof" title="What a player would see (P); again for a menu background" aria-pressed="false"></button>
	<button class="editor-button" data-role="save" data-icon="save" title="Save the map" aria-label="Save the map"></button>
	<span class="editor-status" data-role="status" role="status"></span>
	<span class="editor-hover" data-role="hover"></span>
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
		<div class="editor-construct" data-role="construct" hidden>
			<select class="editor-small" data-role="construct-tile"></select>
			<button class="editor-small" data-role="construct-run" title="Put this physics tile under every tile this layer draws">construct</button>
		</div>
	</section>
	<section class="editor-panel" data-role="tiles-panel" hidden>
		<header class="editor-panel-head">
			<h2>Tiles</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="flip-x" title="Turn the brush over sideways (X)">&harr;</button>
				<button class="editor-small" data-role="flip-y" title="Turn the brush over (Y)">&updownarrow;</button>
				<button class="editor-small" data-role="rotate" title="A quarter turn (R)">&#8635;</button>
				<span data-role="brush-size"></span>
			</span>
		</header>
		<canvas class="editor-tileset" data-role="tileset" width="256" height="256"></canvas>
		<div class="editor-numbers" data-role="numbers"></div>
		<div class="editor-automap" data-role="automap" hidden>
			<select class="editor-small" data-role="automap-config"></select>
			<select class="editor-small" data-role="automap-reference"></select>
			<button class="editor-small" data-role="automap-run" title="Put the tiles the rules ask for into this layer">automap</button>
			<label class="editor-small" title="Run them over every stroke, as part of the same change"><input type="checkbox" data-role="automap-auto"> auto</label>
		</div>
	</section>
	<section class="editor-panel" data-role="audio-panel">
		<header class="editor-panel-head">
			<h2>Sounds</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="play-sound" title="Play it, through the browser">&#9654;</button>
				<button class="editor-small" data-role="add-sound" title="Read an Opus file into the map">+</button>
				<button class="editor-small" data-role="replace-sound" title="Other bytes for this sound">&#8635;</button>
				<button class="editor-small" data-role="unpack-sound" title="Take the bytes out and name the file instead">out</button>
				<button class="editor-small" data-role="delete-sound" title="Take this sound out of the map">-</button>
			</span>
		</header>
		<ol class="editor-images" data-role="sound-list"></ol>
		<input type="file" accept="audio/opus,audio/ogg,.opus" data-role="sound-file" hidden>
		<audio data-role="sound-player" hidden></audio>
	</section>
	<section class="editor-panel" data-role="sounds-panel" hidden>
		<header class="editor-panel-head">
			<h2>Sound sources</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="add-source" title="A source in the middle of the view">+</button>
				<button class="editor-small" data-role="delete-source" title="Delete the source that is picked">-</button>
			</span>
		</header>
		<ol class="editor-quads" data-role="source-list"></ol>
		<div class="editor-props" data-role="source-props"></div>
	</section>
	<section class="editor-panel" data-role="rules-panel" hidden>
		<header class="editor-panel-head">
			<h2>Rules</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="rules-apply" title="Read the text as it stands now">apply</button>
				<button class="editor-small" data-role="rules-revert" title="Fetch the file again as it lies beside the game">revert</button>
				<button class="editor-small" data-role="rules-save" title="Write the text out as a file">save</button>
			</span>
		</header>
		<div class="editor-code">
			<pre class="editor-code-view" data-role="rules-view" aria-hidden="true"></pre>
			<textarea class="editor-code-text" data-role="rules-text" spellcheck="false" wrap="off"></textarea>
		</div>
		<p class="editor-code-status" data-role="rules-status"></p>
	</section>
	<section class="editor-panel" data-role="quads-panel" hidden>
		<header class="editor-panel-head">
			<h2>Quads</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="add-quad" title="A quad in the middle of the view">+</button>
				<button class="editor-small" data-role="delete-quad" title="Delete the quad that is picked">-</button>
			</span>
		</header>
		<ol class="editor-quads" data-role="quad-list"></ol>
		<div class="editor-shape" data-role="shape">
			<button class="editor-small" data-role="shape-square" title="The rectangle the corners span">square</button>
			<button class="editor-small" data-role="shape-aspect" title="As tall as the picture's proportions ask">aspect</button>
			<button class="editor-small" data-role="shape-centerPivot" title="The pivot into the middle">pivot</button>
			<button class="editor-small" data-role="shape-align" title="Every corner onto the nearest tile">align</button>
		</div>
		<div class="editor-props" data-role="quad-props"></div>
	</section>
	<section class="editor-panel" data-role="images-panel">
		<header class="editor-panel-head">
			<h2>Images</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="add-image" title="Read a PNG into the map">+</button>
				<button class="editor-small" data-role="replace-image" title="Other pixels for this picture">&#8635;</button>
				<button class="editor-small" data-role="unpack-image" title="Take the pixels out and name the file instead">out</button>
				<button class="editor-small" data-role="delete-image" title="Take this picture out of the map">-</button>
			</span>
		</header>
		<ol class="editor-images" data-role="image-list"></ol>
		<input type="file" accept="image/png,image/*" data-role="image-file" hidden>
	</section>
	<section class="editor-panel" data-role="envelopes-panel">
		<header class="editor-panel-head">
			<h2>Envelopes</h2>
			<span class="editor-panel-tools">
				<select class="editor-small" data-role="envelope-list"></select>
				<button class="editor-small" data-role="add-envelope" title="Add a colour envelope">+</button>
				<button class="editor-small" data-role="delete-envelope" title="Delete this envelope">-</button>
			</span>
		</header>
		<svg class="editor-curve" data-role="curve" viewBox="0 0 100 100" preserveAspectRatio="none"></svg>
		<div class="editor-props" data-role="point-props"></div>
	</section>
	<section class="editor-panel" data-role="info-panel">
		<header class="editor-panel-head">
			<h2>Map</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="append-map" title="Put another map's groups into this one">append&hellip;</button>
				<button class="editor-small" data-role="add-setting" title="A line the server runs when it loads the map">+ setting</button>
				<button class="editor-small" data-role="delete-setting" title="Take this line away">-</button>
			</span>
		</header>
		<div class="editor-props" data-role="info-props"></div>
		<ol class="editor-settings" data-role="setting-list"></ol>
		<p class="editor-setting-said" data-role="setting-said" role="status"></p>
		<datalist data-role="setting-names"></datalist>
		<input type="file" accept=".map" data-role="append-file" hidden>
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

// What a map says about itself. The lines a server runs are a list and are
// not here; everything else is a word.
const MAP_INFO_PROPS = [
	{ prop: "author", label: "Author", kind: "text" },
	{ prop: "mapVersion", label: "Version", kind: "text" },
	{ prop: "credits", label: "Credits", kind: "text" },
	{ prop: "license", label: "Licence", kind: "text" },
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

// The grammar of a `.rules` file, as far as colouring it needs to know: the
// words that begin a line, and the words that stand inside one. Taken from
// the parser rather than from memory - a word the parser does not know is a
// word this must not paint as if it did.
const RULES_KEYWORDS = ["NewRun", "Index", "Pos", "Random", "Modulo", "NoDefaultRule", "NoLayerCopy"];
const RULES_WORDS = ["EMPTY", "FULL", "INDEX", "NOTINDEX", "NONE", "OR", "XFLIP", "YFLIP", "ROTATE"];

function escaped(text) {
	return text.replace(/[&<>]/g, one => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[one]);
}

/**
 * A rules file, painted.
 *
 * A textarea cannot colour its own text, so what is coloured is a `<pre>`
 * behind it holding the same words; the textarea above it is transparent
 * except for its caret. That is why the two have to be laid out to the pixel
 * and why the scrolling of the one is copied onto the other.
 *
 * @param text The whole file.
 * @param problems The line numbers the program passed over, counting from
 * one; those lines are painted as the mistakes they are.
 */
function paintedRules(text, problems) {
	const bad = new Set(problems);
	return text.split("\n").map((line, index) => {
		const number = index + 1;
		const trimmed = line.trimStart();
		let body;
		if (trimmed.startsWith("#")) {
			body = `<span class="rules-comment">${escaped(line)}</span>`;
		} else if (trimmed.startsWith("[")) {
			body = `<span class="rules-config">${escaped(line)}</span>`;
		} else {
			// Word by word, so that the spaces between them stay where they
			// are - a colouring that moves the text is worse than none.
			body = escaped(line).replace(/[^\s]+/g, word => {
				if (RULES_KEYWORDS.includes(word)) {
					return `<span class="rules-keyword">${word}</span>`;
				}
				if (RULES_WORDS.includes(word)) {
					return `<span class="rules-word">${word}</span>`;
				}
				if (/^-?\d+(\.\d+)?%?$/.test(word)) {
					return `<span class="rules-number">${word}</span>`;
				}
				return word;
			});
		}
		return bad.has(number) ? `<span class="rules-problem">${body}</span>` : body;
	}).join("\n");
}

// The thirteen physics tiles a layer's tiles can be turned into, in the order
// the editor in the client offers them. The names are the command's; what
// stands beside them is what a person reads.
const GAME_TILES = [
	["hookable", "Hookable"],
	["unhookable", "Unhookable"],
	["hookthrough", "Hookthrough"],
	["death", "Death"],
	["freeze", "Freeze"],
	["unfreeze", "Unfreeze"],
	["deepFreeze", "Deep freeze"],
	["deepUnfreeze", "Deep unfreeze"],
	["liveFreeze", "Live freeze"],
	["liveUnfreeze", "Live unfreeze"],
	["blueCheckTele", "Blue check tele"],
	["redCheckTele", "Red check tele"],
	["air", "Air"],
];

// How large a tile layer is. Not a property like the others: it goes through
// `layer.resize`, because a physics layer is not resized on its own.
const SIZE_PROPS = [
	{ prop: "width", label: "Width", kind: "count" },
	{ prop: "height", label: "Height", kind: "count" },
];

// How the value of a group or layer is found in what the program said about
// it: a clip is one array of four, an offset one of two, everything else is
// itself.
const PACKED = {
	offsetX: ["offset", 0], offsetY: ["offset", 1],
	parallaxX: ["parallax", 0], parallaxY: ["parallax", 1],
	clipX: ["clip", 0], clipY: ["clip", 1], clipW: ["clip", 2], clipH: ["clip", 3],
	width: ["size", 0], height: ["size", 1],
};

function propertyValue(thing, prop) {
	const packed = PACKED[prop];
	return packed === undefined ? thing[prop] : thing[packed[0]][packed[1]];
}

/**
 * The pixels of a picture file, decoded by the browser.
 *
 * A browser reads PNGs and what comes out of a canvas is already the RGBA the
 * map keeps, so the program is never asked to decode anything - which is also
 * why a picture that is 4096 wide is refused here rather than there: that is
 * what a map file can hold.
 *
 * @return An `ImageData`, or null where the browser could not read it.
 */
async function pixelsOf(file) {
	try {
		const picture = await createImageBitmap(file);
		const canvas = document.createElement("canvas");
		canvas.width = picture.width;
		canvas.height = picture.height;
		const paint = canvas.getContext("2d", { willReadFrequently: true });
		paint.drawImage(picture, 0, 0);
		picture.close();
		return paint.getImageData(0, 0, canvas.width, canvas.height);
	} catch (error) {
		return null;
	}
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
		const settings = Object.assign({ container: null, dataBase: null, signal: undefined }, options || {});
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
		// What is being dragged in the layer list, while something is.
		this.dragging = null;
		// Where the panels stood when each history entry was made, so that
		// stepping back through them takes the panels along.
		this.snapshots = new Map();
		// Which envelope is being drawn, and which of its points is picked.
		this.envelope = 0;
		this.point = -1;
		// Which quad of the selected layer has handles on it.
		this.quad = -1;
		// Which picture of the map is picked in the image panel.
		this.image = -1;
		// Which line of the server settings is picked.
		this.setting = -1;
		// The rules files that were fetched, by the name of the picture they
		// belong to. `null` means there are none for that picture - asked
		// once and then remembered, so a layer without rules costs one 404.
		this.rules = new Map();
		// The text of those files, by the same name. Kept apart from the
		// parsed ones because what is typed into the box is not what the
		// program holds until somebody says so.
		this.ruleText = new Map();
		// Which of the places a number is used at was looked at last, so that
		// pressing the button again goes to the next one.
		this.gotoAt = 0;
		// Which sound source is picked, or -1 for none.
		this.source = -1;
		// Which sound of the map is picked, and the address of the one that
		// is playing, so that it can be given back again.
		this.sound = -1;
		this.playing = null;
		// The shapes drawn over the canvas, made once the canvas has a parent
		// to hang them in.
		this.overlay = null;
		// Whether what a player would see is drawn, and at which zoom:
		// "off", "game" or "menu".
		this.proof = "off";
		// The picture of the tiles, what it was fetched from, and the
		// rectangle that was taken out of it.
		this.dataBase = settings.dataBase || new URL("data/", location.href).href;
		this.tileset = null;
		this.tilesetSource = null;
		this.picked = null;
		this.root = document.createElement("div");
		this.root.className = "editor-panels";
		this.root.innerHTML = PANELS_HTML;
		DDNetBase.paintIcons(this.root);
		if (settings.container !== null) {
			settings.container.append(this.root);
		}
		this.makeOverlay();
		this.wire();
		this.refresh();
	}

	/**
	 * The SVG that shapes are drawn on, over the canvas.
	 *
	 * It goes where the canvas is rather than where the panels are, because
	 * that is what it lies over; a page that puts the canvas somewhere with
	 * no parent to hang it in gets no overlay and loses nothing else.
	 */
	makeOverlay() {
		const canvas = this.editor.canvas;
		const parent = canvas === null || canvas === undefined ? null : canvas.parentElement;
		if (parent === null) {
			return;
		}
		this.overlay = document.createElementNS(SVG_NAMESPACE, "svg");
		this.overlay.setAttribute("class", "editor-overlay");
		this.overlay.dataset.role = "overlay";
		this.overlay.hidden = true;
		parent.append(this.overlay);
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
		if (this.playing !== null) {
			URL.revokeObjectURL(this.playing);
			this.playing = null;
		}
		if (this.overlay !== null) {
			this.overlay.remove();
			this.overlay = null;
		}
		this.root.remove();
	}

	wire() {
		const signal = this.stopping.signal;
		const on = (role, handler) => this.part(role).addEventListener("click", handler, { signal: signal });
		on("undo", () => this.stepHistory(() => this.editor.undo()));
		on("redo", () => this.stepHistory(() => this.editor.redo()));
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
		on("grid", () => {
			this.editor.grid(this.editor.grid() > 0 ? 0 : GRID_SPACING);
			this.refreshBar();
		});
		// Off, then a game, then a menu background, then off again: three
		// states on one button, because the two on-states are the same
		// question asked at two zooms.
		on("proof", () => {
			this.proof = this.proof === "off" ? "game" : (this.proof === "game" ? "menu" : "off");
			this.refreshBar();
			this.refreshOverlay();
		});
		on("add-group", () => this.change(() => this.editor.apply({ op: "group.add", name: "group" })));
		on("add-layer", () => this.change(() => this.editor.apply({ op: "layer.add", group: this.selection.group, type: "tiles" })));
		on("add-quads", () => this.change(() => this.editor.apply({ op: "layer.add", group: this.selection.group, type: "quads" })));
		on("flip-x", () => { this.editor.flipBrushX(); this.refreshTiles(); });
		on("flip-y", () => { this.editor.flipBrushY(); this.refreshTiles(); });
		on("rotate", () => { this.editor.rotateBrush(); this.refreshTiles(); });
		this.wireTileset();
		this.wireAutomap();
		this.wireConstruct();
		this.wireShape();
		this.wireRules();
		this.wireSounds();
		this.wireAudio();
		this.wireEnvelopes();
		this.wireQuads();
		this.wireImages();
		this.wireInfo();
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
		// The brush, which is what an editor's keyboard is mostly for: turn it
		// over, turn it round, and ten slots to put one away in.
		if (!event.ctrlKey && !event.metaKey && !event.altKey) {
			const key = event.key.toLowerCase();
			if (key === "x" || key === "y" || key === "r") {
				if (key === "x") {
					this.editor.flipBrushX();
				} else if (key === "y") {
					this.editor.flipBrushY();
				} else {
					this.editor.rotateBrush();
				}
				this.refreshTiles();
				event.preventDefault();
				return;
			}
			if (key === "g") {
				this.editor.grid(this.editor.grid() > 0 ? 0 : GRID_SPACING);
				this.refreshBar();
				event.preventDefault();
				return;
			}
			if (key === "p") {
				this.proof = this.proof === "off" ? "game" : (this.proof === "game" ? "menu" : "off");
				this.refreshBar();
				this.refreshOverlay();
				event.preventDefault();
				return;
			}
			if (key >= "0" && key <= "9") {
				const slot = Number.parseInt(key, 10);
				if (event.shiftKey) {
					this.editor.storeBrush(slot);
				} else {
					this.editor.useBrush(slot);
				}
				this.refreshTiles();
				event.preventDefault();
				return;
			}
		}
		if (event.ctrlKey || event.metaKey) {
			if (event.key === "z" && !event.shiftKey) {
				this.stepHistory(() => this.editor.undo());
			} else if (event.key === "y" || (event.key === "z" && event.shiftKey)) {
				this.stepHistory(() => this.editor.redo());
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
	change(work, stepping) {
		// Where the work was being done, so that going back to this version
		// later puts the panels where they were. An undo that leaves the
		// wrong layer selected is an undo that has to be looked for.
		const before = this.editor.history();
		const where = { group: this.selection.group, layer: this.selection.layer };
		const answer = work();
		if (answer && answer.ok === false) {
			this.say(answer.error || "Refused");
		}
		const after = this.editor.history();
		// Only a change that wrote an entry leaves a snapshot. Stepping
		// through the history moves the same mark about and must not
		// overwrite what is already written there, and a change that was
		// folded into the entry before it keeps that entry's mark.
		if (!stepping && before !== null && after !== null && after.current > before.current) {
			this.snapshots.set(after.current, where);
		}
		this.refresh();
		return answer;
	}

	/**
	 * Steps through the history and takes the panels along.
	 *
	 * What is put back is what was selected when the entry being *left* was
	 * made - going back over a change shows where that change was, not where
	 * the one before it was.
	 *
	 * @param work What moves the history.
	 */
	stepHistory(work) {
		const before = this.editor.history();
		const answer = this.change(work, true);
		const after = this.editor.history();
		if (before === null || after === null || before.current === after.current) {
			return answer;
		}
		const shown = after.current < before.current ? before.current : after.current;
		const where = this.snapshots.get(shown);
		if (where !== undefined) {
			this.selection = { group: where.group, layer: where.layer };
			this.clampSelection();
			this.refreshTree();
			this.refreshProps();
			this.refreshTiles();
		}
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

	/**
	 * Makes one row of the tree something that can be picked up and something
	 * that can be dropped on.
	 *
	 * Dropping is the same move the arrows make, so it goes through the same
	 * command and gets the same one history entry: a layer dropped on a layer
	 * goes to that place, a layer dropped on a group head goes to the end of
	 * that group, and a group dropped on a group head goes to that place. The
	 * arrows stay, because a drag is not something everybody can do.
	 */
	wireDragging(row, what) {
		const signal = this.stopping.signal;
		row.draggable = true;
		row.addEventListener("dragstart", event => {
			this.dragging = what;
			event.dataTransfer.effectAllowed = "move";
			// Something has to be carried or Firefox starts no drag at all.
			event.dataTransfer.setData("text/plain", "");
			event.stopPropagation();
		}, { signal: signal });
		row.addEventListener("dragend", () => {
			this.dragging = null;
			row.classList.remove("editor-drop");
		}, { signal: signal });
		row.addEventListener("dragover", event => {
			if (this.dropWould(what) === null) {
				return;
			}
			event.preventDefault();
			event.dataTransfer.dropEffect = "move";
			row.classList.add("editor-drop");
		}, { signal: signal });
		row.addEventListener("dragleave", () => row.classList.remove("editor-drop"), { signal: signal });
		row.addEventListener("drop", event => {
			const command = this.dropWould(what);
			row.classList.remove("editor-drop");
			this.dragging = null;
			if (command === null) {
				return;
			}
			// The page beneath may be waiting for a dropped map file; this is
			// not one.
			event.preventDefault();
			event.stopPropagation();
			const answer = this.change(() => this.editor.apply(command));
			if (answer && answer.ok) {
				this.selection = { group: answer.group, layer: answer.layer === undefined ? -1 : answer.layer };
			}
			this.refresh();
		}, { signal: signal });
	}

	/**
	 * What dropping what is being dragged on that row would do, or nothing.
	 *
	 * Where something goes is counted in the list it leaves behind - see
	 * `MoveGroup` - and that is exactly what makes the number here the place
	 * that was dropped on: what is dragged lands where the row it was dropped
	 * on is now, whichever way it came from.
	 */
	dropWould(onto) {
		const held = this.dragging;
		if (held == null || this.map === null) {
			return null;
		}
		if (held.layer < 0) {
			// A group only goes where a group goes, and not onto itself.
			if (onto.layer >= 0 || onto.group === held.group) {
				return null;
			}
			return { op: "group.move", group: held.group, to: onto.group };
		}
		const same = onto.group === held.group;
		let to;
		if (onto.layer < 0) {
			// The head of a group is the end of it: dropped there, a layer is
			// drawn last of that group's layers. One less when it is already
			// in that group, because then it is counted without itself.
			to = this.map.groups[onto.group].layers.length - (same ? 1 : 0);
			if (same && held.layer === to) {
				return null;
			}
		} else {
			if (same && onto.layer === held.layer) {
				return null;
			}
			to = onto.layer;
		}
		return { op: "layer.move", group: held.group, layer: held.layer, toGroup: onto.group, to: to };
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
		this.refreshTiles();
		this.refreshQuads();
		this.refreshSounds();
		this.refreshImages();
		this.refreshAudio();
		this.refreshEnvelopes();
		this.refreshInfo();
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
		set("grid", this.editor.grid() > 0);
		const proof = this.part("proof");
		proof.setAttribute("aria-pressed", this.proof === "off" ? "false" : "true");
		proof.dataset.proof = this.proof;
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
			this.wireDragging(head, { group: groupIndex, layer: -1 });
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
					// Hiding a layer is a thing about looking, so the eye is
					// not a property and writes no history entry.
					const shown = this.editor.visible(groupIndex, layerIndex);
					const eye = document.createElement("button");
					eye.className = "editor-eye";
					eye.dataset.role = "visible";
					eye.textContent = shown ? "\u25c9" : "\u25cb";
					eye.title = shown ? "Hide this layer" : "Show this layer";
					eye.setAttribute("aria-pressed", shown ? "true" : "false");
					eye.addEventListener("click", event => {
						event.stopPropagation();
						this.editor.visible(groupIndex, layerIndex, !shown);
						this.refreshTree();
					}, { signal: this.stopping.signal });
					const label = document.createElement("span");
					label.textContent = `${layer.name || what} (${what})`;
					row.append(eye, label);
					if (!shown) {
						row.classList.add("editor-hidden-layer");
					}
					if (this.selection.group === groupIndex && this.selection.layer === layerIndex) {
						row.classList.add("editor-selected");
					}
					row.addEventListener("click", () => {
						this.selection = { group: groupIndex, layer: layerIndex };
						this.refreshTree();
						this.refreshProps();
					}, { signal: this.stopping.signal });
					this.wireDragging(row, { group: groupIndex, layer: layerIndex });
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
		this.part("construct").hidden = true;
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
		this.part("construct").hidden = layer.construct !== true;
		if (layer.type === "tiles") {
			for (const size of SIZE_PROPS) {
				props.append(this.field(layer, size, value => ({
					op: "layer.resize", group: where.group, layer: where.layer,
					[size.prop]: value, label: "Resize layer",
				})));
			}
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
		// What is being changed, for the history to fold a run of steps into
		// one entry: this property, of this layer, of this group. Two
		// different layers' names are two different things.
		const what = JSON.stringify(command(null));
		const open = () => {
			if (!editing) {
				editing = true;
				this.editor.begin(description.label, undefined, what);
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
		} else if (description.kind === "choice") {
			// A word out of a short list, which is a `<select>` rather than a
			// field: the list is what the command will take, so a word that
			// would be refused cannot be typed in the first place.
			const chooser = document.createElement("select");
			chooser.dataset.role = input.dataset.role;
			for (const [value, label] of description.options) {
				const option = document.createElement("option");
				option.value = value;
				option.textContent = label;
				chooser.append(option);
			}
			chooser.value = String(current);
			chooser.addEventListener("change", () => {
				send(chooser.value);
				this.refresh();
			}, { signal: this.stopping.signal });
			row.append(name, chooser);
			return row;
		} else if (description.kind === "count") {
			// A number that is only handed over when the field is left. Every
			// step on the way would be a change of its own, and a layer typed
			// from 8 to 150 would be resized to 1 and then to 15 first - which
			// for a size means the tiles outside are gone before the number is
			// finished.
			input.type = "number";
			input.min = "1";
			input.value = String(current);
			input.addEventListener("change", () => {
				const value = Number.parseInt(input.value, 10);
				if (Number.isFinite(value)) {
					send(value);
				}
				this.refresh();
			}, { signal: this.stopping.signal });
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

	// The picture of a layer's tiles, and the rectangle somebody dragged over
	// it. The picture is the page's doing rather than the program's: it is a
	// PNG that the map names, the browser reads PNGs, and a tileset drawn on
	// a canvas costs the program nothing.
	wireTileset() {
		const canvas = this.part("tileset");
		let from = null;
		// A refused capture must not take the pick with it; see the canvas
		// pointer, which holds on the same way.
		const capture = (pointerId, hold) => {
			try {
				if (hold) {
					canvas.setPointerCapture(pointerId);
				} else {
					canvas.releasePointerCapture(pointerId);
				}
			} catch (error) {
				// The pick still works; it just stops at the edge.
			}
		};
		const at = event => {
			const box = canvas.getBoundingClientRect();
			return {
				x: Math.min(TILESET_SIDE - 1, Math.max(0, Math.floor((event.clientX - box.left) / box.width * TILESET_SIDE))),
				y: Math.min(TILESET_SIDE - 1, Math.max(0, Math.floor((event.clientY - box.top) / box.height * TILESET_SIDE))),
			};
		};
		canvas.addEventListener("pointerdown", event => {
			from = at(event);
			capture(event.pointerId, true);
			this.pick(from, from);
		}, { signal: this.stopping.signal });
		canvas.addEventListener("pointermove", event => {
			if (from !== null) {
				this.pick(from, at(event));
			}
		}, { signal: this.stopping.signal });
		const release = event => {
			if (from !== null) {
				this.pick(from, at(event));
				from = null;
				capture(event.pointerId, false);
			}
		};
		canvas.addEventListener("pointerup", release, { signal: this.stopping.signal });
		canvas.addEventListener("pointercancel", () => { from = null; }, { signal: this.stopping.signal });
	}

	// Takes the rectangle between two tiles of the tileset into the brush.
	pick(one, other) {
		const where = this.selection;
		if (this.map === null || where.layer < 0) {
			return;
		}
		this.picked = {
			x: Math.min(one.x, other.x),
			y: Math.min(one.y, other.y),
			width: Math.abs(other.x - one.x) + 1,
			height: Math.abs(other.y - one.y) + 1,
		};
		this.editor.pickTiles(where.group, where.layer, this.picked.x, this.picked.y, this.picked.width, this.picked.height);
		this.refreshTiles();
	}

	refreshTiles() {
		const panel = this.part("tiles-panel");
		const layer = this.selectedLayer();
		panel.hidden = layer === null || layer.type !== "tiles";
		if (panel.hidden) {
			return;
		}
		const size = this.editor.brushSize();
		this.part("brush-size").textContent = size === null ? "" : `${size.width} x ${size.height}`;
		this.refreshNumbers(layer);
		this.refreshAutomap(layer);
		this.refreshRules(layer);

		// The picture the layer is drawn with, where the map names one that
		// lies beside it. A layer whose picture is inside the map file, or
		// which has none at all, gets a grid of numbers: the tiles are still
		// there to be picked, they just cannot be shown.
		const image = layer.image >= 0 && layer.image < this.map.images.length ? this.map.images[layer.image] : null;
		// A picture that lies beside the map is fetched by the browser; one
		// that is packed into the map file is already unpacked in the program
		// and is asked for. Only a layer with no picture at all is left with
		// a grid of numbers - the tiles are still there to be picked, they
		// just cannot be shown.
		const source = image === null ? null : (image.external ? new URL(`mapres/${image.name}.png`, this.dataBase).href : `packed:${layer.image}:${image.name}`);
		if (source !== this.tilesetSource) {
			this.tilesetSource = source;
			this.tileset = null;
			if (image !== null && !image.external) {
				this.tileset = this.editor.imageData(layer.image);
			} else if (source !== null) {
				const picture = new Image();
				picture.addEventListener("load", () => {
					if (this.tilesetSource === source) {
						this.tileset = picture;
						this.refreshTiles();
					}
				}, { once: true });
				picture.src = source;
			}
		}
		this.paintTileset();
	}

	/**
	 * The numbers that go beside a physics tile, as fields under the tileset.
	 *
	 * Changing one changes the brush and nothing else: no version, no history
	 * entry. What is put down afterwards carries it.
	 *
	 * @param layer The layer that is selected.
	 */
	refreshNumbers(layer) {
		const box = this.part("numbers");
		const fields = TILE_NUMBERS[layer.kind] || [];
		// Only rebuilt when the layer wants other fields than are there, so
		// that a number being typed is not taken away mid-word.
		const wanted = fields.map(field => field.key).join(",");
		if (box.dataset.fields !== wanted) {
			box.dataset.fields = wanted;
			box.textContent = "";
			for (const field of fields) {
				const row = document.createElement("label");
				row.className = "editor-prop";
				const name = document.createElement("span");
				name.textContent = field.label;
				const input = document.createElement("input");
				input.type = "number";
				input.min = "0";
				input.max = String(field.max);
				input.dataset.role = `number-${field.key}`;
				input.addEventListener("input", () => {
					const value = Number.parseInt(input.value, 10);
					if (Number.isFinite(value)) {
						this.editor.numbers({ [field.key]: value });
					}
				}, { signal: this.stopping.signal });
				row.append(name, input);
				if (field.key === "number") {
					// Beside the number, the two things somebody does with
					// one: take one that is free, and go and look at where
					// this one already is.
					const free = document.createElement("button");
					free.className = "editor-small";
					free.dataset.role = "next-free";
					free.textContent = "free";
					free.title = "The lowest number this layer is not using";
					row.append(free);
					const goto_ = document.createElement("button");
					goto_.className = "editor-small";
					goto_.dataset.role = "goto-number";
					goto_.textContent = "go";
					goto_.title = "Look at where this number is used; again for the next one";
					row.append(goto_);
					free.addEventListener("click", () => this.takeFreeNumber(input), { signal: this.stopping.signal });
					goto_.addEventListener("click", () => this.lookAtNumber(), { signal: this.stopping.signal });
				}
				box.append(row);
			}
		}
		if (fields.length === 0) {
			return;
		}
		// What the brush carries, which after a grab is what was picked up.
		const carried = this.editor.numbers();
		for (const field of fields) {
			const input = box.querySelector(`[data-role="number-${field.key}"]`);
			if (input !== null && input !== document.activeElement) {
				input.value = String(carried[field.key]);
			}
		}
	}

	/** Puts the lowest free number of this layer into the brush. */
	takeFreeNumber(input) {
		const where = this.selection;
		const layer = this.selectedLayer();
		if (layer === null) {
			return;
		}
		// A tele layer counts its checkpoints apart from the rest, and which
		// of the two is being put down is a question about the brush.
		const checkpoint = layer.kind === "tele" && this.editor.brushCheckpoint();
		const free = this.editor.nextFreeNumber(where.group, where.layer, checkpoint);
		if (free < 0) {
			this.say("Every number is taken");
			return;
		}
		this.editor.numbers({ number: free });
		input.value = String(free);
	}

	/**
	 * Moves the view to where the brush's number is already used, and to the
	 * next such place when pressed again.
	 */
	lookAtNumber() {
		const where = this.selection;
		const number = this.editor.numbers().number;
		const places = this.editor.gotoNumber(where.group, where.layer, number, this.gotoAt);
		if (places === 0) {
			this.gotoAt = 0;
			this.say(`Nothing uses ${number}`);
			return;
		}
		this.say(`${(this.gotoAt % places) + 1} of ${places}`);
		this.gotoAt = (this.gotoAt + 1) % places;
	}

	/**
	 * The automapper: which rules to run over this layer and which of their
	 * configurations.
	 *
	 * The rules file belongs to the picture, not to the layer - a layer drawn
	 * with `grass_main` is automapped by `grass_main.rules` - so what is
	 * offered follows the picture. The page fetches the file, because a page
	 * fetches things; the program parses it and runs it.
	 */
	wireAutomap() {
		this.part("automap-run").addEventListener("click", () => {
			const layer = this.selectedLayer();
			const name = this.rulesNameFor(layer);
			if (layer === null || name === null) {
				return;
			}
			const config = Number.parseInt(this.part("automap-config").value, 10);
			const reference = Number.parseInt(this.part("automap-reference").value, 10);
			if (!Number.isFinite(config)) {
				return;
			}
			const where = this.selection;
			this.change(() => ({
				ok: this.editor.automap(where.group, where.layer, name, config,
					{ seed: layer.automapperSeed || 0, reference: reference }),
				error: "The rules would not run",
			}));
		}, { signal: this.stopping.signal });
		this.part("automap-auto").addEventListener("change", () => {
			const where = this.selection;
			this.change(() => this.editor.apply({
				op: "layer.setProp", group: where.group, layer: where.layer,
				prop: "automapperAutomatic", value: this.part("automap-auto").checked,
			}));
		}, { signal: this.stopping.signal });
	}

	/**
	 * The thirteen construct operations: a physics tile under every tile the
	 * layer draws.
	 *
	 * Whether a layer can be built from at all is the program's answer
	 * (`layer.construct` in the structure), not the page's - it depends on
	 * where the group lies over the game layer, and the page does not know
	 * that rule.
	 */
	wireConstruct() {
		const choice = this.part("construct-tile");
		for (const [name, label] of GAME_TILES) {
			const option = document.createElement("option");
			option.value = name;
			option.textContent = label;
			choice.append(option);
		}
		this.part("construct-run").addEventListener("click", () => {
			const where = this.selection;
			this.change(() => this.editor.apply({
				op: "layer.constructGameTiles", group: where.group, layer: where.layer,
				tile: choice.value,
			}));
		}, { signal: this.stopping.signal });
	}

	/**
	 * Runs the layer's own rules over what a stroke just drew, where the
	 * layer was told to do that by itself.
	 *
	 * Called while the stroke's change is still open, so drawing and what it
	 * led to are one entry: one undo takes both back. Only the rectangle the
	 * stroke was over is run, with the margin the rules need - which is why a
	 * stroke on a large map costs what it touched.
	 */
	automapAfterStroke(where, box) {
		const layer = this.selectedLayer();
		const name = this.rulesNameFor(layer);
		if (layer === null || name === null || !layer.automapperAutomatic || layer.automapperConfig < 0) {
			return;
		}
		if (this.rules.get(name) !== name) {
			return;
		}
		this.editor.automap(where.group, where.layer, name, layer.automapperConfig, {
			seed: layer.automapperSeed || 0, reference: -1,
			x: box.x, y: box.y, width: box.width, height: box.height,
		});
	}

	// Which rules file a layer is automapped by: the one named after its
	// picture, and nothing at all for a layer that has no picture.
	rulesNameFor(layer) {
		if (layer === null || layer.type !== "tiles" || layer.image < 0 || this.map === null) {
			return null;
		}
		const image = this.map.images[layer.image];
		return image === undefined ? null : image.name;
	}

	refreshAutomap(layer) {
		const box = this.part("automap");
		const name = this.rulesNameFor(layer);
		box.hidden = name === null || this.rules.get(name) === null;
		if (name === null) {
			return;
		}
		if (!this.rules.has(name)) {
			// Asked for once. What comes back goes to the program, which
			// parses it; what does not come back is remembered as nothing, so
			// a picture without rules is not fetched again.
			this.rules.set(name, undefined);
			fetch(new URL(`editor/automap/${name}.rules`, this.dataBase).href)
				.then(answer => (answer.ok ? answer.text() : null))
				.then(text => {
					this.rules.set(name, text === null || this.editor.loadRules(name, text) === 0 ? null : name);
					if (text !== null) {
						this.ruleText.set(name, text);
						this.part("rules-text").dataset.rules = "";
					}
					this.refreshTiles();
				})
				.catch(() => {
					this.rules.set(name, null);
				});
			return;
		}
		if (this.rules.get(name) === undefined) {
			return;
		}
		const configs = this.editor.ruleConfigs(name);
		box.hidden = configs.length === 0;
		const chooser = this.part("automap-config");
		if (chooser.dataset.rules !== name) {
			chooser.dataset.rules = name;
			chooser.textContent = "";
			configs.forEach((title, index) => {
				const option = document.createElement("option");
				option.value = String(index);
				option.textContent = title;
				chooser.append(option);
			});
			chooser.value = String(Math.max(0, layer.automapperConfig));
			chooser.addEventListener("change", () => {
				const where = this.selection;
				this.change(() => this.editor.apply({
					op: "layer.setProp", group: where.group, layer: where.layer,
					prop: "automapperConfig", value: Number.parseInt(chooser.value, 10),
				}));
			}, { signal: this.stopping.signal });
		} else if (document.activeElement !== chooser) {
			chooser.value = String(Math.max(0, layer.automapperConfig));
		}
		const reference = this.part("automap-reference");
		if (reference.childElementCount === 0) {
			// The first run may read the game layer instead of this one, and
			// then only one kind of physics tile of it.
			["Off"].concat(AUTOMAP_REFERENCES).forEach((title, index) => {
				const option = document.createElement("option");
				option.value = String(index - 1);
				option.textContent = title;
				reference.append(option);
			});
			reference.value = "-1";
		}
		this.part("automap-auto").checked = layer.automapperAutomatic === true;
	}

	paintTileset() {
		const canvas = this.part("tileset");
		const paint = canvas.getContext("2d");
		const side = canvas.width / TILESET_SIDE;
		paint.clearRect(0, 0, canvas.width, canvas.height);
		if (this.tileset instanceof ImageData) {
			// `putImageData` ignores the size of the target, so the pixels go
			// through a canvas of their own to be drawn at the size of this
			// one - and without smoothing, because a tile is sixteen pixels
			// and smoothing it makes it somebody else's tile at the edges.
			const packed = document.createElement("canvas");
			packed.width = this.tileset.width;
			packed.height = this.tileset.height;
			packed.getContext("2d").putImageData(this.tileset, 0, 0);
			paint.imageSmoothingEnabled = false;
			paint.drawImage(packed, 0, 0, canvas.width, canvas.height);
		} else if (this.tileset !== null) {
			paint.imageSmoothingEnabled = false;
			paint.drawImage(this.tileset, 0, 0, canvas.width, canvas.height);
		} else {
			paint.fillStyle = "#1a1a1e";
			paint.fillRect(0, 0, canvas.width, canvas.height);
			paint.fillStyle = "#6a6a76";
			paint.font = `${Math.round(side * 0.5)}px system-ui, sans-serif`;
			paint.textAlign = "center";
			paint.textBaseline = "middle";
			for (let index = 0; index < TILESET_SIDE * TILESET_SIDE; ++index) {
				paint.fillText(String(index), (index % TILESET_SIDE + 0.5) * side, (Math.floor(index / TILESET_SIDE) + 0.5) * side);
			}
		}
		paint.strokeStyle = "#ffffff30";
		paint.lineWidth = 1;
		for (let line = 1; line < TILESET_SIDE; ++line) {
			paint.beginPath();
			paint.moveTo(line * side, 0);
			paint.lineTo(line * side, canvas.height);
			paint.moveTo(0, line * side);
			paint.lineTo(canvas.width, line * side);
			paint.stroke();
		}
		if (this.picked !== null) {
			paint.strokeStyle = "#ffcc44";
			paint.lineWidth = 2;
			paint.strokeRect(this.picked.x * side + 1, this.picked.y * side + 1,
				this.picked.width * side - 2, this.picked.height * side - 2);
		}
	}

	/** The layer that is selected, or `null` when a group is. */
	selectedLayer() {
		if (this.map === null || this.map.groups.length === 0 || this.selection.layer < 0) {
			return null;
		}
		return this.map.groups[this.selection.group].layers[this.selection.layer] || null;
	}

	/**
	 * The envelope panel: which envelope, its curve, and the point that is
	 * picked out of it.
	 *
	 * The curve is an SVG rather than a canvas because that is what an SVG is
	 * for - a few dozen points that are dragged one at a time, each of them a
	 * thing the browser can hit-test and give a pointer to. This is the place
	 * the plan meant when it said web technology is most clearly ahead of the
	 * painted editor.
	 */
	wireEnvelopes() {
		const signal = this.stopping.signal;
		this.part("envelope-list").addEventListener("change", event => {
			this.envelope = Number.parseInt(event.target.value, 10);
			this.point = -1;
			this.refreshEnvelopes();
		}, { signal: signal });
		this.part("add-envelope").addEventListener("click", () => {
			const answer = this.change(() => this.editor.apply({ op: "envelope.add", name: "envelope" }));
			if (answer && answer.ok) {
				this.envelope = answer.envelope;
				this.point = -1;
				this.refreshEnvelopes();
			}
		}, { signal: signal });
		this.part("delete-envelope").addEventListener("click", () => {
			if (this.envelopeCount() === 0) {
				return;
			}
			this.change(() => this.editor.apply({ op: "envelope.delete", envelope: this.envelope }));
			this.envelope = Math.max(0, Math.min(this.envelope, this.envelopeCount() - 1));
			this.point = -1;
			this.refreshEnvelopes();
		}, { signal: signal });
		this.wireCurve();
	}

	envelopeCount() {
		return this.map === null ? 0 : this.map.envelopes.length;
	}

	/**
	 * The rectangle of the envelope that the curve is drawn in: all of its
	 * time, and enough of its values to show them.
	 *
	 * Worked out from the points rather than fixed, because a colour envelope
	 * runs 0 to 1024 and a position envelope runs wherever the map goes, and
	 * a drawing that fits one would be a flat line for the other.
	 */
	curveBounds(envelope) {
		let last = 1000;
		let low = 0;
		let high = ENVELOPE_ONE;
		for (const point of envelope.points) {
			last = Math.max(last, point.time);
			for (const value of point.values) {
				low = Math.min(low, value);
				high = Math.max(high, value);
			}
		}
		// A little air above and below, or a point at the very top is drawn
		// half outside the box.
		const air = Math.max(1, (high - low) * 0.08);
		return { time: last, low: low - air, high: high + air };
	}

	/**
	 * The quads of the layer that is selected, as a list to pick from.
	 *
	 * Picking one puts handles on its corners - drawn by the program, because
	 * a quad lies in its group's coordinates and the parallax sum belongs
	 * where the drawing is. Dragging them is the pointer's business, not the
	 * panel's.
	 */
	wireQuads() {
		const signal = this.stopping.signal;
		this.part("add-quad").addEventListener("click", () => {
			const where = this.selection;
			const canvas = this.editor.canvas;
			// In the middle of the view - but asked of the group, not of the
			// plain view: a quad lives in its group's coordinates, and in a
			// group with no parallax at all the middle of the world is
			// nowhere near the middle of the screen.
			const middle = this.editor.groupWorldAt(where.group, canvas.width / 2, canvas.height / 2);
			if (middle === null) {
				return;
			}
			const answer = this.change(() => this.editor.apply({
				op: "quad.add", group: where.group, layer: where.layer,
				x: Math.round(middle.x), y: Math.round(middle.y),
			}));
			if (answer && answer.ok) {
				this.quad = answer.quad;
			}
			this.refresh();
		}, { signal: signal });
		this.part("delete-quad").addEventListener("click", () => {
			if (this.quad < 0) {
				return;
			}
			const where = this.selection;
			this.change(() => this.editor.apply({ op: "quad.delete", group: where.group, layer: where.layer, quad: this.quad }));
			this.quad = -1;
			this.refresh();
		}, { signal: signal });
	}

	refreshQuads() {
		const panel = this.part("quads-panel");
		const layer = this.selectedLayer();
		panel.hidden = layer === null || layer.type !== "quads";
		if (panel.hidden) {
			this.quad = -1;
			this.editor.showQuad();
			return;
		}
		const where = this.selection;
		const quads = this.editor.quads(where.group, where.layer) || [];
		if (this.quad >= quads.length) {
			this.quad = -1;
		}
		this.part("delete-quad").disabled = this.quad < 0;
		// Shaping is something done to a quad, so it is there when one is
		// picked; only the proportions also need a picture on the layer.
		this.part("shape").hidden = this.quad < 0;
		this.part("shape-aspect").disabled = layer.image < 0;
		const list = this.part("quad-list");
		list.textContent = "";
		quads.forEach((quad, index) => {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "quad";
			row.dataset.quad = String(index);
			// Where it is rather than what it is called, because a quad has
			// no name - the place of its pivot is what tells two apart.
			row.textContent = `${index}: ${Math.round(quad.points[8] / MAP_TILE_SIZE)}, ${Math.round(quad.points[9] / MAP_TILE_SIZE)}`;
			if (index === this.quad) {
				row.classList.add("editor-selected");
			}
			row.addEventListener("click", () => {
				this.quad = index;
				this.refreshQuads();
			}, { signal: this.stopping.signal });
			list.append(row);
		});
		if (this.quad < 0) {
			this.editor.showQuad();
		} else {
			this.editor.showQuad(where.group, where.layer, this.quad);
		}
		this.refreshQuadProps(quads[this.quad]);
	}

	/**
	 * The sound sources of a sound layer: where they are and what they are.
	 *
	 * Where they are is dragged on the map like a quad's points; what they
	 * are - the shape, how far they carry, what they are bound to - are
	 * fields. What is heard is not shown here at all: the program has no
	 * sound in it, and the page has the file.
	 */
	wireSounds() {
		const signal = this.stopping.signal;
		this.part("add-source").addEventListener("click", () => {
			const where = this.selection;
			const canvas = this.editor.canvas;
			// In the middle of the view of its own group, the same as a quad.
			const middle = this.editor.groupWorldAt(where.group, canvas.width / 2, canvas.height / 2);
			if (middle === null) {
				return;
			}
			const answer = this.change(() => this.editor.apply({
				op: "source.add", group: where.group, layer: where.layer,
				x: Math.round(middle.x), y: Math.round(middle.y),
			}));
			if (answer && answer.ok) {
				this.source = answer.source;
			}
			this.refresh();
		}, { signal: signal });
		this.part("delete-source").addEventListener("click", () => {
			if (this.source < 0) {
				return;
			}
			const where = this.selection;
			this.change(() => this.editor.apply({ op: "source.delete", group: where.group, layer: where.layer, source: this.source }));
			this.source = -1;
			this.refresh();
		}, { signal: signal });
	}

	refreshSounds() {
		const panel = this.part("sounds-panel");
		const layer = this.selectedLayer();
		panel.hidden = layer === null || layer.type !== "sounds";
		if (panel.hidden) {
			this.source = -1;
			this.refreshOverlay();
			return;
		}
		const where = this.selection;
		const sources = this.editor.sources(where.group, where.layer) || [];
		if (this.source >= sources.length) {
			this.source = -1;
		}
		this.part("delete-source").disabled = this.source < 0;
		const list = this.part("source-list");
		list.textContent = "";
		sources.forEach((source, index) => {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "source";
			row.dataset.source = String(index);
			// Where it is, in tiles, the same way a quad is listed: a source
			// has no name either.
			row.textContent = `${index}: ${Math.round(source.position[0] / MAP_TILE_SIZE)}, ${Math.round(source.position[1] / MAP_TILE_SIZE)}`;
			if (index === this.source) {
				row.classList.add("editor-selected");
			}
			row.addEventListener("click", () => {
				this.source = index;
				this.refreshSounds();
			}, { signal: this.stopping.signal });
			list.append(row);
		});
		this.refreshSourceProps(sources[this.source]);
		this.refreshOverlay(sources);
	}

	refreshSourceProps(source) {
		const props = this.part("source-props");
		props.textContent = "";
		if (source === undefined) {
			return;
		}
		const where = this.selection;
		const index = this.source;
		const thing = Object.assign({}, source, {
			radius: source.radius === undefined ? 0 : source.radius,
			width: source.size === undefined ? 0 : source.size[0],
			height: source.size === undefined ? 0 : source.size[1],
		});
		const set = prop => value => ({
			op: "source.setProp", group: where.group, layer: where.layer, source: index,
			prop: prop, value: value,
		});
		// Which fields a source has depends on what it is heard within: a
		// circle has a radius and no sides, a rectangle the other way round.
		const fields = SOURCE_PROPS.concat(source.shape === "circle" ? SOURCE_CIRCLE : SOURCE_RECTANGLE);
		for (const field of fields) {
			props.append(this.field(thing, field, set(field.prop)));
		}
	}

	/**
	 * The shapes of the sound sources, drawn over the canvas as an SVG.
	 *
	 * The program does not draw them: a sound layer is not seen, and what is
	 * drawn here is a working aid rather than part of the map. An SVG is what
	 * this is for - a few shapes that stand over a picture and are told where
	 * to stand. Where that is comes from the program (`groupPixelAt`), not
	 * from a sum repeated here, or the shapes and the map would drift apart
	 * the first time one of the two changed.
	 */
	refreshOverlay(known) {
		const overlay = this.overlay;
		if (overlay === null) {
			return;
		}
		const canvas = this.editor.canvas;
		if (canvas === null || canvas === undefined) {
			return;
		}
		overlay.textContent = "";
		// The canvas is drawn in the pixels the screen has and laid out in
		// the units the page uses; the SVG is laid out, so it is told about
		// the drawn ones and scales itself.
		overlay.setAttribute("viewBox", `0 0 ${canvas.width} ${canvas.height}`);
		const drawn = this.paintProof(overlay) + this.paintSources(overlay, known);
		overlay.hidden = drawn === 0;
	}

	/**
	 * What a player would see, drawn over the map.
	 *
	 * Twenty-one rectangles are one outline: the widest screen and the
	 * tallest one are the two ends of it, and everything in between is a
	 * shape somebody's window actually has. The two named ones are drawn on
	 * top, because they are the two a mapper is told to check.
	 *
	 * It is all in the game layer's coordinates - proof mode asks what a
	 * *player* sees, and a player sees the game.
	 */
	paintProof(overlay) {
		if (this.proof === "off" || this.map === null) {
			return 0;
		}
		const group = this.map.groups.findIndex(g => g.layers.some(l => l.kind === "game"));
		if (group < 0) {
			return 0;
		}
		const proof = this.editor.proof(this.proof === "menu");
		if (proof === null) {
			return 0;
		}
		const spot = (x, y) => this.editor.groupPixelAt(group, x, y);
		const corners = rect => {
			const a = spot(rect[0], rect[1]);
			const b = spot(rect[2], rect[3]);
			return a === null || b === null ? null : [a, b];
		};
		const box = (rect, kind, name) => {
			const both = corners(rect);
			if (both === null) {
				return 0;
			}
			const shape = document.createElementNS(SVG_NAMESPACE, "rect");
			shape.setAttribute("x", String(both[0].x));
			shape.setAttribute("y", String(both[0].y));
			shape.setAttribute("width", String(Math.max(1, both[1].x - both[0].x)));
			shape.setAttribute("height", String(Math.max(1, both[1].y - both[0].y)));
			shape.setAttribute("class", `editor-proof editor-proof-${kind}`);
			shape.dataset.role = "proof-rect";
			shape.dataset.proof = kind;
			if (name !== undefined) {
				shape.dataset.name = name;
			}
			overlay.append(shape);
			return 1;
		};

		let drawn = 0;
		// The two ends of the outline first, then the shapes in between as one
		// path, so that the middle is a hint and the ends are the answer.
		const steps = proof.steps;
		drawn += box(steps[0], "step");
		drawn += box(steps[steps.length - 1], "step");
		const path = document.createElementNS(SVG_NAMESPACE, "path");
		const along = (pick, from, to) => {
			const points = [];
			for (let i = from; i !== to; i += from < to ? 1 : -1) {
				const both = corners(steps[i]);
				if (both !== null) {
					points.push(pick(both));
				}
			}
			return points;
		};
		// Each corner walks its own way from the tall screen to the wide one:
		// four lines, which together are the edge of everything anybody sees.
		const line = points => points.map((p, i) => `${i === 0 ? "M" : "L"}${p.x} ${p.y}`).join("");
		const last = steps.length;
		path.setAttribute("d", [
			line(along(b => ({ x: b[0].x, y: b[0].y }), 0, last)),
			line(along(b => ({ x: b[1].x, y: b[0].y }), 0, last)),
			line(along(b => ({ x: b[0].x, y: b[1].y }), 0, last)),
			line(along(b => ({ x: b[1].x, y: b[1].y }), 0, last)),
		].join(""));
		path.setAttribute("class", "editor-proof editor-proof-outline");
		path.dataset.role = "proof-outline";
		overlay.append(path);
		drawn += 1;

		proof.named.forEach((named, index) => {
			drawn += box(named.rect, index === 0 ? "first" : "second", named.name);
		});

		// Where the camera stands, and - behind a menu - the other places it
		// could stand in this map, each moved as if it were the one standing.
		const here = spot(proof.center[0], proof.center[1]);
		if (here !== null) {
			const dot = document.createElementNS(SVG_NAMESPACE, "circle");
			dot.setAttribute("cx", String(here.x));
			dot.setAttribute("cy", String(here.y));
			dot.setAttribute("r", "6");
			dot.setAttribute("class", "editor-proof-tee");
			dot.dataset.role = "proof-tee";
			overlay.append(dot);
			drawn += 1;
		}
		proof.positions.forEach(position => {
			const at = spot(position.position[0], position.position[1]);
			if (at === null) {
				return;
			}
			const mark = document.createElementNS(SVG_NAMESPACE, "circle");
			mark.setAttribute("cx", String(at.x));
			mark.setAttribute("cy", String(at.y));
			mark.setAttribute("r", "6");
			mark.setAttribute("class", "editor-proof-position");
			mark.dataset.role = "proof-position";
			mark.dataset.index = String(position.index);
			overlay.append(mark);
			drawn += 1;
		});
		return drawn;
	}

	/**
	 * What is under the pointer, said in the bar.
	 *
	 * Three things, and the third is the one that matters: where, which tile,
	 * and what that tile does. The number comes in both bases because a
	 * mapper reads tiles in decimal and the entities sheet is sixteen wide,
	 * so hex says which row and column in one go - `0x23` is row 2, column 3.
	 *
	 * @param tile Where the pointer is, in tiles, or null for gone.
	 */
	hoverAt(tile) {
		const readout = this.part("hover");
		if (tile === null || this.map === null) {
			readout.textContent = "";
			return;
		}
		const where = this.selection;
		const layer = this.selectedLayer();
		if (layer === null || layer.type !== "tiles") {
			readout.textContent = `${tile.x}, ${tile.y}`;
			return;
		}
		const index = this.editor.tileIndex(where.group, where.layer, tile.x, tile.y);
		if (index < 0) {
			readout.textContent = `${tile.x}, ${tile.y}`;
			return;
		}
		const hex = index.toString(16).toUpperCase().padStart(2, "0");
		const said = this.editor.explain(where.group, where.layer, index);
		readout.textContent = `${tile.x}, ${tile.y} · ${index} (0x${hex})${said === "" ? "" : ` · ${said}`}`;
		readout.title = said;
	}

	/** The shapes a sound layer's sources are heard within. */
	paintSources(overlay, known) {
		const layer = this.selectedLayer();
		const where = this.selection;
		const sources = layer !== null && layer.type === "sounds"
			? (known || this.editor.sources(where.group, where.layer) || [])
			: [];
		const spot = (x, y) => this.editor.groupPixelAt(where.group, x, y);
		sources.forEach((source, index) => {
			const at = spot(source.position[0], source.position[1]);
			if (at === null) {
				return;
			}
			const shape = document.createElementNS(SVG_NAMESPACE, source.shape === "circle" ? "circle" : "rect");
			if (source.shape === "circle") {
				// A radius is a distance, so it is measured rather than
				// converted: how far the point that far away landed.
				const edge = spot(source.position[0] + source.radius, source.position[1]);
				shape.setAttribute("cx", String(at.x));
				shape.setAttribute("cy", String(at.y));
				shape.setAttribute("r", String(Math.max(1, edge === null ? 1 : Math.abs(edge.x - at.x))));
			} else {
				const corner = spot(source.position[0] - source.size[0] / 2, source.position[1] - source.size[1] / 2);
				const other = spot(source.position[0] + source.size[0] / 2, source.position[1] + source.size[1] / 2);
				shape.setAttribute("x", String(corner === null ? at.x : corner.x));
				shape.setAttribute("y", String(corner === null ? at.y : corner.y));
				shape.setAttribute("width", String(Math.max(1, other === null || corner === null ? 1 : other.x - corner.x)));
				shape.setAttribute("height", String(Math.max(1, other === null || corner === null ? 1 : other.y - corner.y)));
			}
			shape.setAttribute("class", index === this.source ? "editor-source editor-source-picked" : "editor-source");
			shape.dataset.role = "source-shape";
			shape.dataset.source = String(index);
			overlay.append(shape);

			const dot = document.createElementNS(SVG_NAMESPACE, "circle");
			dot.setAttribute("cx", String(at.x));
			dot.setAttribute("cy", String(at.y));
			dot.setAttribute("r", "5");
			dot.setAttribute("class", index === this.source ? "editor-source-dot editor-source-picked" : "editor-source-dot");
			dot.dataset.role = "source-dot";
			dot.dataset.source = String(index);
			overlay.append(dot);
		});
		return sources.length;
	}

	/**
	 * The colours and the envelope bindings of the quad that is picked.
	 *
	 * Written as fields rather than dragged on the map, because that is what
	 * they are: a colour is picked and a binding is a number. The points are
	 * the other way round and are not here at all - they are dragged.
	 */
	refreshQuadProps(quad) {
		const props = this.part("quad-props");
		props.textContent = "";
		if (quad === undefined) {
			return;
		}
		const where = this.selection;
		const index = this.quad;
		// The four channels the map keeps, so that a colour picker - which
		// has no alpha - can put the one it does not know back untouched.
		const thing = { posEnv: quad.posEnv, posEnvOffset: quad.posEnvOffset, colorEnv: quad.colorEnv, colorEnvOffset: quad.colorEnvOffset };
		QUAD_CORNERS.forEach((name, corner) => {
			const color = quad.colors.slice(corner * 4, corner * 4 + 4);
			thing[`corner${corner}`] = color;
			thing[`alpha${corner}`] = color[3];
			const set = value => ({ op: "quad.setColor", group: where.group, layer: where.layer, quad: index, corner: corner, value: value });
			props.append(this.field(thing, { prop: `corner${corner}`, label: `${name} colour`, kind: "color" }, set));
			props.append(this.field(thing, { prop: `alpha${corner}`, label: `${name} alpha`, kind: "number" },
				value => set(value === null ? null : color.slice(0, 3).concat([Math.min(255, Math.max(0, value))]))));
			// Where the corner sits in the picture. 1024 is the whole picture
			// across, which is the number the file holds and the one the
			// editor in the client shows, so a map made in either comes out
			// with the same numbers in it.
			const texture = quad.texcoords.slice(corner * 2, corner * 2 + 2);
			thing[`texU${corner}`] = texture[0];
			thing[`texV${corner}`] = texture[1];
			const place = (u, v) => ({
				op: "quad.setTexcoord", group: where.group, layer: where.layer, quad: index,
				corner: corner, u: u, v: v,
			});
			props.append(this.field(thing, { prop: `texU${corner}`, label: `${name} tex U`, kind: "number" },
				value => place(value, texture[1])));
			props.append(this.field(thing, { prop: `texV${corner}`, label: `${name} tex V`, kind: "number" },
				value => place(texture[0], value)));
		});
		for (const description of QUAD_PROPS) {
			props.append(this.field(thing, description,
				value => ({ op: "quad.setProp", group: where.group, layer: where.layer, quad: index, prop: description.prop, value: value })));
		}
	}

	/**
	 * The rules file of the picture a layer is drawn with, as text.
	 *
	 * A rules file does not belong to the map - it lies beside the game,
	 * named after the picture - so what is edited here is the copy the
	 * program holds. `apply` reads the text as it stands and says which lines
	 * it could not use, `revert` fetches the file again, and `save` writes it
	 * out so that it can be put where the game looks for it. Nothing here
	 * touches the map, so nothing here is in the history.
	 */
	wireRules() {
		const text = this.part("rules-text");
		const view = this.part("rules-view");
		// The painted copy is behind the text and has to be scrolled with it,
		// or the two drift apart the moment the file is longer than the box.
		text.addEventListener("scroll", () => {
			view.scrollTop = text.scrollTop;
			view.scrollLeft = text.scrollLeft;
		}, { signal: this.stopping.signal });
		text.addEventListener("input", () => this.paintRules(), { signal: this.stopping.signal });

		this.part("rules-apply").addEventListener("click", () => {
			const name = this.rulesNameFor(this.selectedLayer());
			if (name === null) {
				return;
			}
			this.ruleText.set(name, text.value);
			const configs = this.editor.loadRules(name, text.value);
			this.rules.set(name, configs === 0 ? null : name);
			// The configurations may be other ones now, so the automapper's
			// list has to be built again rather than kept.
			this.part("automap-config").dataset.rules = "";
			this.refresh();
		}, { signal: this.stopping.signal });

		this.part("rules-revert").addEventListener("click", () => {
			const name = this.rulesNameFor(this.selectedLayer());
			if (name === null) {
				return;
			}
			this.ruleText.delete(name);
			this.rules.delete(name);
			this.part("automap-config").dataset.rules = "";
			this.refresh();
		}, { signal: this.stopping.signal });

		this.part("rules-save").addEventListener("click", () => {
			const name = this.rulesNameFor(this.selectedLayer());
			if (name === null) {
				return;
			}
			const handout = document.createElement("a");
			const address = URL.createObjectURL(new Blob([text.value], { type: "text/plain" }));
			handout.href = address;
			handout.download = `${name}.rules`;
			handout.click();
			// Given back once the browser has had it; keeping it would keep
			// the whole file alive for as long as the page is open.
			setTimeout(() => URL.revokeObjectURL(address), 10000);
		}, { signal: this.stopping.signal });
	}

	/** Paints the text as it stands, and says what the program made of it. */
	paintRules() {
		const name = this.rulesNameFor(this.selectedLayer());
		const text = this.part("rules-text");
		const problems = name === null ? [] : this.editor.ruleProblems(name);
		// Only the lines that were passed over the last time it was read, so
		// a line being typed is not painted as a mistake before it is done.
		const stale = name === null || this.ruleText.get(name) !== text.value;
		this.part("rules-view").innerHTML = paintedRules(text.value, stale ? [] : problems);
		const configs = name === null ? [] : this.editor.ruleConfigs(name);
		const said = [`${configs.length} ${configs.length === 1 ? "configuration" : "configurations"}`];
		if (stale) {
			said.push("not read yet");
		} else if (problems.length > 0) {
			said.push(`${problems.length === 1 ? "line" : "lines"} ${problems.join(", ")} not understood`);
		}
		this.part("rules-status").textContent = said.join(" \u00b7 ");
	}

	refreshRules(layer) {
		const panel = this.part("rules-panel");
		const name = this.rulesNameFor(layer);
		panel.hidden = name === null || this.rules.get(name) === null;
		if (panel.hidden || this.rules.get(name) === undefined) {
			return;
		}
		const text = this.part("rules-text");
		// The text belongs to the picture, so switching between two layers
		// drawn with the same one keeps whatever was typed into it.
		if (text.dataset.rules !== name) {
			text.dataset.rules = name;
			text.value = this.ruleText.get(name) || "";
		}
		this.paintRules();
	}

	/**
	 * The four ways a quad is put in order rather than dragged into it.
	 *
	 * A quad dragged by four corners is almost never the rectangle somebody
	 * meant, so there are buttons for the rectangle, for the proportions of
	 * the picture, for the pivot in the middle, and for the grid.
	 */
	wireShape() {
		for (const shape of ["square", "aspect", "centerPivot", "align"]) {
			this.part(`shape-${shape}`).addEventListener("click", () => {
				const where = this.selection;
				this.change(() => this.editor.apply({
					op: "quad.shape", group: where.group, layer: where.layer,
					quad: this.quad, shape: shape,
				}));
			}, { signal: this.stopping.signal });
		}
	}

	/**
	 * The pictures of the map: reading one in, swapping its pixels, taking
	 * the pixels back out, and taking it away.
	 *
	 * The PNG is decoded by the browser rather than by the program - a
	 * browser reads PNGs, and what comes out of a canvas is already the RGBA
	 * the map keeps. What crosses over is the bytes, not a JSON text of them.
	 */
	wireImages() {
		const signal = this.stopping.signal;
		const file = this.part("image-file");
		// What the file, once chosen, is for: a new picture or another one's
		// pixels. Held here because the dialogue answers later.
		let replacing = -1;
		this.part("add-image").addEventListener("click", () => {
			replacing = -1;
			file.value = "";
			file.click();
		}, { signal: signal });
		this.part("replace-image").addEventListener("click", () => {
			if (this.image < 0) {
				return;
			}
			replacing = this.image;
			file.value = "";
			file.click();
		}, { signal: signal });
		file.addEventListener("change", async () => {
			const chosen = file.files && file.files[0];
			if (!chosen) {
				return;
			}
			const pixels = await pixelsOf(chosen);
			if (pixels === null) {
				this.say("That is not a picture this browser can read");
				return;
			}
			// The name without its suffix, which is what a map calls a
			// picture - the file is `grass_main.png`, the picture is
			// `grass_main`.
			const name = chosen.name.replace(/\.[^.]*$/, "");
			this.change(() => {
				if (replacing >= 0) {
					return { ok: this.editor.setImagePixels(replacing, pixels) === true };
				}
				const index = this.editor.addImage(name, pixels);
				if (index >= 0) {
					this.image = index;
				}
				return { ok: index >= 0, error: "The picture was refused" };
			});
		}, { signal: signal });
		this.part("unpack-image").addEventListener("click", () => {
			if (this.image < 0) {
				return;
			}
			this.change(() => this.editor.apply({ op: "image.setProp", image: this.image, prop: "external", value: true }));
			this.refresh();
		}, { signal: signal });
		this.part("delete-image").addEventListener("click", () => {
			if (this.image < 0) {
				return;
			}
			this.change(() => this.editor.apply({ op: "image.delete", image: this.image }));
			this.image = -1;
			this.refresh();
		}, { signal: signal });
	}

	refreshImages() {
		const panel = this.part("images-panel");
		panel.hidden = this.map === null;
		if (panel.hidden) {
			return;
		}
		const images = this.map.images || [];
		if (this.image >= images.length) {
			this.image = -1;
		}
		for (const role of ["replace-image", "unpack-image", "delete-image"]) {
			this.part(role).disabled = this.image < 0;
		}
		if (this.image >= 0) {
			this.part("unpack-image").disabled = images[this.image].external;
		}
		const list = this.part("image-list");
		list.textContent = "";
		images.forEach((image, index) => {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "image";
			row.dataset.image = String(index);
			// Where its pixels are is the thing worth saying about a picture:
			// one beside the map has to be fetched, one in it does not.
			row.textContent = `${image.name} ${image.size[0]}x${image.size[1]}${image.external ? " (beside)" : ""}`;
			if (index === this.image) {
				row.classList.add("editor-selected");
			}
			row.addEventListener("click", () => {
				this.image = index;
				this.refreshImages();
			}, { signal: this.stopping.signal });
			list.append(row);
		});
	}

	/**
	 * The sounds of the map: reading one in, swapping its bytes, taking the
	 * bytes back out, taking it away - and hearing it.
	 *
	 * The bytes are an Opus file and nothing here looks into them. The
	 * program has no sound in it and does not need any: a browser decodes
	 * Opus, so the one place that can play a map's sound is the page.
	 */
	wireAudio() {
		const signal = this.stopping.signal;
		const file = this.part("sound-file");
		let replacing = -1;
		this.part("add-sound").addEventListener("click", () => {
			replacing = -1;
			file.value = "";
			file.click();
		}, { signal: signal });
		this.part("replace-sound").addEventListener("click", () => {
			if (this.sound < 0) {
				return;
			}
			replacing = this.sound;
			file.value = "";
			file.click();
		}, { signal: signal });
		file.addEventListener("change", async () => {
			const chosen = file.files && file.files[0];
			if (!chosen) {
				return;
			}
			const bytes = new Uint8Array(await chosen.arrayBuffer());
			// The name without its suffix, the same as a picture: the file is
			// `wind.opus`, the sound is `wind`.
			const name = chosen.name.replace(/\.[^.]*$/, "");
			this.change(() => {
				if (replacing >= 0) {
					return { ok: this.editor.setSoundData(replacing, bytes) === true, error: "The sound was refused" };
				}
				const index = this.editor.addSound(name, bytes);
				if (index >= 0) {
					this.sound = index;
				}
				return { ok: index >= 0, error: "The sound was refused" };
			});
			this.refresh();
		}, { signal: signal });
		this.part("unpack-sound").addEventListener("click", () => {
			if (this.sound < 0) {
				return;
			}
			this.change(() => this.editor.apply({ op: "sound.setProp", sound: this.sound, prop: "external", value: true }));
			this.refresh();
		}, { signal: signal });
		this.part("delete-sound").addEventListener("click", () => {
			if (this.sound < 0) {
				return;
			}
			this.change(() => this.editor.apply({ op: "sound.delete", sound: this.sound }));
			this.sound = -1;
			this.refresh();
		}, { signal: signal });
		this.part("play-sound").addEventListener("click", () => this.playSound(), { signal: signal });
	}

	/**
	 * Plays the sound that is picked, through the browser.
	 *
	 * A sound that lies beside the map is fetched the way its picture would
	 * be; one that is in the map is handed over as the bytes it is. Either
	 * way the browser decodes it - the program never has to know what Opus
	 * is, which is the whole reason the bytes are kept as bytes.
	 */
	playSound() {
		if (this.sound < 0 || this.map === null) {
			return;
		}
		const sound = this.map.sounds[this.sound];
		if (sound === undefined) {
			return;
		}
		const audio = this.part("sound-player") || new Audio();
		if (this.playing !== null) {
			URL.revokeObjectURL(this.playing);
			this.playing = null;
		}
		if (sound.external) {
			audio.src = new URL(`mapres/${sound.name}.opus`, this.dataBase).href;
		} else {
			const bytes = this.editor.soundData(this.sound);
			if (bytes === null) {
				this.say("That sound has no bytes to play");
				return;
			}
			this.playing = URL.createObjectURL(new Blob([bytes], { type: "audio/ogg" }));
			audio.src = this.playing;
		}
		audio.play().catch(() => this.say("This browser would not play that"));
	}

	refreshAudio() {
		const panel = this.part("audio-panel");
		panel.hidden = this.map === null;
		if (panel.hidden) {
			return;
		}
		const sounds = this.map.sounds || [];
		if (this.sound >= sounds.length) {
			this.sound = -1;
		}
		for (const role of ["play-sound", "replace-sound", "unpack-sound", "delete-sound"]) {
			this.part(role).disabled = this.sound < 0;
		}
		if (this.sound >= 0) {
			this.part("unpack-sound").disabled = sounds[this.sound].external;
		}
		const list = this.part("sound-list");
		list.textContent = "";
		sounds.forEach((sound, index) => {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "sound";
			row.dataset.sound = String(index);
			// Where its bytes are, the same thing worth saying about a
			// picture: one beside the map has to be fetched, one in it does
			// not.
			row.textContent = `${sound.name}${sound.external ? " (beside)" : ` ${sound.bytes < 1024 ? `${sound.bytes} B` : `${Math.round(sound.bytes / 1024)} KiB`}`}`;
			if (index === this.sound) {
				row.classList.add("editor-selected");
			}
			row.addEventListener("click", () => {
				this.sound = index;
				this.refreshAudio();
			}, { signal: this.stopping.signal });
			list.append(row);
		});
	}

	/**
	 * What the map says about itself, and the lines a server runs when it
	 * loads it.
	 *
	 * A change here is a version like any other - the editor in the client
	 * changes the map's own description without an undo entry, and that is
	 * the one thing about it worth not copying.
	 */
	wireInfo() {
		const signal = this.stopping.signal;
		const appending = this.part("append-file");
		this.part("append-map").addEventListener("click", () => {
			appending.value = "";
			appending.click();
		}, { signal: signal });
		appending.addEventListener("change", async () => {
			const chosen = appending.files && appending.files[0];
			if (!chosen) {
				return;
			}
			const came = await this.editor.appendFile(chosen);
			if (came === null) {
				this.say("That map could not be read");
				return;
			}
			// Said rather than shown somewhere: appending moves numbers about
			// everywhere at once, and a count is the only honest summary.
			const parts = [`${came.groups} ${came.groups === 1 ? "group" : "groups"}`];
			if (came.images > 0) {
				parts.push(`${came.images} ${came.images === 1 ? "picture" : "pictures"}`);
			}
			if (came.sharedImages > 0) {
				parts.push(`${came.sharedImages} already there`);
			}
			if (came.renamedImages > 0) {
				parts.push(`${came.renamedImages} renamed`);
			}
			if (came.sounds > 0) {
				parts.push(`${came.sounds} ${came.sounds === 1 ? "sound" : "sounds"}`);
			}
			if (came.envelopes > 0) {
				parts.push(`${came.envelopes} ${came.envelopes === 1 ? "envelope" : "envelopes"}`);
			}
			if (came.settings > 0) {
				parts.push(`${came.settings} ${came.settings === 1 ? "setting" : "settings"}`);
			}
			this.say(`From ${came.name}: ${parts.join(", ")}`);
			this.refresh();
		}, { signal: signal });
		this.part("add-setting").addEventListener("click", () => {
			const answer = this.change(() => this.editor.apply({ op: "info.settings.add", value: "sv_setting 0" }));
			if (answer && answer.ok) {
				this.setting = answer.line;
			}
			this.refresh();
		}, { signal: signal });
		this.part("delete-setting").addEventListener("click", () => {
			if (this.setting < 0) {
				return;
			}
			this.change(() => this.editor.apply({ op: "info.settings.delete", line: this.setting }));
			this.setting = -1;
			this.refresh();
		}, { signal: signal });
	}

	refreshInfo() {
		const panel = this.part("info-panel");
		panel.hidden = this.map === null;
		if (panel.hidden) {
			return;
		}
		const info = this.map.info;
		const props = this.part("info-props");
		// Only rebuilt when a field is not being typed in, so that a name
		// being written is not taken away mid-word.
		if (!props.contains(document.activeElement)) {
			props.textContent = "";
			for (const description of MAP_INFO_PROPS) {
				props.append(this.field(info, description,
					value => ({ op: "info.setProp", prop: description.prop, value: value })));
			}
		}

		const settings = info.settings || [];
		if (this.setting >= settings.length) {
			this.setting = -1;
		}
		this.part("delete-setting").disabled = this.setting < 0;
		const list = this.part("setting-list");
		if (list.contains(document.activeElement)) {
			return;
		}
		list.textContent = "";
		const problems = this.editor.settingProblems();
		settings.forEach((line, index) => {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "setting";
			const input = document.createElement("input");
			input.type = "text";
			input.dataset.role = "setting-line";
			input.dataset.line = String(index);
			input.value = line;
			input.addEventListener("focus", () => {
				this.setting = index;
				this.part("delete-setting").disabled = false;
				for (const other of list.querySelectorAll(".editor-row")) {
					other.classList.remove("editor-selected");
				}
				row.classList.add("editor-selected");
			}, { signal: this.stopping.signal });
			input.addEventListener("change", () => {
				this.change(() => this.editor.apply({ op: "info.settings.set", line: index, value: input.value }));
			}, { signal: this.stopping.signal });
			// While it is being typed, nothing is changed and nothing is
			// undone - only said. A line half written is wrong on the way to
			// being right, and saying so at every keystroke is the point.
			input.addEventListener("input", () => this.sayAboutSetting(input), { signal: this.stopping.signal });
			input.addEventListener("focus", () => this.sayAboutSetting(input), { signal: this.stopping.signal });
			if (index === this.setting) {
				row.classList.add("editor-selected");
			}
			const problem = problems[index] || { problem: "", repeats: -1 };
			if (problem.problem !== "") {
				row.classList.add("editor-setting-wrong");
				input.title = problem.problem;
			} else if (problem.repeats >= 0) {
				row.classList.add("editor-setting-repeat");
				input.title = `the same as line ${problem.repeats + 1}`;
			}
			row.append(input);
			list.append(row);
		});
	}

	/**
	 * What is wrong with the line being typed, and what could be meant.
	 *
	 * Said rather than refused: a line on its way to being right is wrong for
	 * most of the time it is being written, and an editor that would not let
	 * that happen would be an editor nobody could type in. The names that
	 * begin with what stands there go into the list the browser offers, which
	 * is the one piece of completion a page gets for free and the one that
	 * already works the way everybody expects.
	 */
	sayAboutSetting(input) {
		const said = this.part("setting-said");
		const text = input.value;
		const first = text.split(" ")[0];
		const names = this.part("setting-names");
		// Only while a name is still being written: once there is a space,
		// the name is settled and offering more of them is in the way.
		const offers = text.includes(" ") ? [] : this.editor.settingNames(first);
		names.textContent = "";
		for (const name of offers) {
			const option = document.createElement("option");
			option.value = name;
			names.append(option);
		}
		// A list is named rather than nested, so the name has to be one of a
		// kind - two editors on one page would otherwise offer each other's.
		if (names.id === "") {
			names.id = `editor-settings-${++panelCount}`;
		}
		input.setAttribute("list", names.id);

		const problem = this.editor.checkSetting(text);
		const known = this.editor.settingsHelp().find(setting => setting.name === first.toLowerCase());
		said.textContent = problem !== "" ? problem : (known === undefined ? "" : known.help);
		said.classList.toggle("editor-setting-said-wrong", problem !== "");
	}

	refreshEnvelopes() {
		const panel = this.part("envelopes-panel");
		const count = this.envelopeCount();
		panel.hidden = this.map === null;
		if (panel.hidden) {
			return;
		}
		this.envelope = count === 0 ? 0 : Math.min(this.envelope, count - 1);
		const list = this.part("envelope-list");
		list.textContent = "";
		this.map.envelopes.forEach((envelope, index) => {
			const option = document.createElement("option");
			option.value = String(index);
			option.textContent = `${index}: ${envelope.name || "envelope"} (${envelope.channels})`;
			option.selected = index === this.envelope;
			list.append(option);
		});
		this.part("delete-envelope").disabled = count === 0;
		this.paintCurve();
		this.refreshPoint();
	}

	/** The envelope being drawn, with its points, or `null`. */
	shownEnvelope() {
		return this.envelopeCount() === 0 ? null : this.editor.envelope(this.envelope);
	}

	paintCurve() {
		const svg = this.part("curve");
		svg.textContent = "";
		const envelope = this.shownEnvelope();
		if (envelope === null) {
			return;
		}
		const bounds = this.curveBounds(envelope);
		svg.dataset.time = String(bounds.time);
		svg.dataset.low = String(bounds.low);
		svg.dataset.high = String(bounds.high);
		const make = name => document.createElementNS(SVG_NAMESPACE, name);
		const x = time => (bounds.time === 0 ? 0 : (time / bounds.time) * 100);
		// Upside down, because a value that grows should go up and an SVG
		// counts downwards.
		const y = value => 100 - ((value - bounds.low) / (bounds.high - bounds.low)) * 100;

		// The line at zero, so that a value which turns negative is visible
		// as such rather than as a line that happens to be lower.
		if (bounds.low < 0 && bounds.high > 0) {
			const zero = make("line");
			zero.setAttribute("x1", "0");
			zero.setAttribute("x2", "100");
			zero.setAttribute("y1", String(y(0)));
			zero.setAttribute("y2", String(y(0)));
			zero.setAttribute("class", "editor-curve-zero");
			svg.append(zero);
		}

		const channels = ENVELOPE_CHANNELS[envelope.channels] || [];
		channels.forEach((channel, index) => {
			if (envelope.points.length > 0) {
				const line = make("polyline");
				line.setAttribute("points", envelope.points.map(p => `${x(p.time)},${y(p.values[index])}`).join(" "));
				line.setAttribute("fill", "none");
				line.setAttribute("stroke", channel.colour);
				line.setAttribute("vector-effect", "non-scaling-stroke");
				line.setAttribute("stroke-width", "1.5");
				svg.append(line);
			}
			envelope.points.forEach((point, at) => {
				const dot = make("circle");
				dot.setAttribute("cx", String(x(point.time)));
				dot.setAttribute("cy", String(y(point.values[index])));
				// Not scaled with the box, which is stretched to fill the
				// panel: a circle in it would be an egg.
				dot.setAttribute("r", "1.6");
				dot.setAttribute("fill", channel.colour);
				dot.setAttribute("class", at === this.point ? "editor-curve-point editor-curve-picked" : "editor-curve-point");
				dot.dataset.point = String(at);
				dot.dataset.channel = String(index);
				svg.append(dot);
			});
		});
	}

	/**
	 * Dragging a point, and clicking where there is none to make one.
	 *
	 * A drag is one transaction from the button going down to it coming up,
	 * so the history gets one entry however far the point travelled - and
	 * every step of the way is already drawn, which is the preview.
	 */
	wireCurve() {
		const svg = this.part("curve");
		const signal = this.stopping.signal;
		let dragging = -1;
		let channel = 0;

		// Holding on to the pointer, or letting go of it. Either may be
		// refused - a pointer that has already gone is not there to be caught
		// - and neither is worth giving up a drag over.
		const capture = (pointerId, hold) => {
			try {
				if (hold) {
					svg.setPointerCapture(pointerId);
				} else {
					svg.releasePointerCapture(pointerId);
				}
			} catch (error) {
				// The drag still works; it just stops when the pointer leaves.
			}
		};

		// Where a pointer is, in the envelope's own numbers.
		const at = event => {
			const box = svg.getBoundingClientRect();
			const time = Number.parseFloat(svg.dataset.time) || 1000;
			const low = Number.parseFloat(svg.dataset.low) || 0;
			const high = Number.parseFloat(svg.dataset.high) || ENVELOPE_ONE;
			const across = box.width === 0 ? 0 : (event.clientX - box.left) / box.width;
			const down = box.height === 0 ? 0 : (event.clientY - box.top) / box.height;
			return {
				time: Math.max(0, Math.round(across * time)),
				value: Math.round(low + (1 - down) * (high - low)),
			};
		};

		svg.addEventListener("pointerdown", event => {
			const envelope = this.shownEnvelope();
			if (envelope === null) {
				return;
			}
			const picked = event.target.dataset && event.target.dataset.point;
			if (picked === undefined) {
				// Nowhere in particular: a new point there, on every channel
				// at once so that the envelope keeps its shape.
				const where = at(event);
				const values = new Array(envelope.channels).fill(where.value);
				const answer = this.change(() => this.editor.apply({
					op: "envelope.point.add", envelope: this.envelope, time: where.time, values: values,
				}));
				if (answer && answer.ok) {
					this.point = answer.point;
					this.refreshEnvelopes();
				}
				return;
			}
			dragging = Number.parseInt(picked, 10);
			channel = Number.parseInt(event.target.dataset.channel, 10);
			this.point = dragging;
			// The change is opened before the pointer is caught, and catching
			// it is allowed to fail: a browser refuses for a pointer that is
			// no longer there, and a drag without a transaction would write
			// an entry per step.
			this.editor.begin("Move point");
			capture(event.pointerId, true);
			this.refreshEnvelopes();
		}, { signal: signal });

		svg.addEventListener("pointermove", event => {
			if (dragging < 0) {
				return;
			}
			const envelope = this.shownEnvelope();
			if (envelope === null || dragging >= envelope.points.length) {
				return;
			}
			const where = at(event);
			// Only the channel whose dot was taken hold of moves; the others
			// stay where they are, which is what somebody dragging a red dot
			// means by it.
			const values = envelope.points[dragging].values.slice();
			values[channel] = where.value;
			const answer = this.editor.apply({
				op: "envelope.point.set", envelope: this.envelope, point: dragging, time: where.time, values: values,
			});
			if (answer && answer.ok) {
				dragging = answer.point;
				this.point = answer.point;
			}
			this.paintCurve();
			this.refreshPoint();
		}, { signal: signal });

		const release = event => {
			if (dragging < 0) {
				return;
			}
			dragging = -1;
			this.editor.commit();
			capture(event.pointerId, false);
			this.refresh();
		};
		svg.addEventListener("pointerup", release, { signal: signal });
		svg.addEventListener("pointercancel", release, { signal: signal });
	}

	/** The point that is picked, as fields: its time, its values, its curve. */
	refreshPoint() {
		const box = this.part("point-props");
		box.textContent = "";
		const envelope = this.shownEnvelope();
		if (envelope === null || this.point < 0 || this.point >= envelope.points.length) {
			return;
		}
		const point = envelope.points[this.point];
		const send = command => this.change(() => this.editor.apply(command));

		const time = document.createElement("label");
		time.className = "editor-prop";
		const timeName = document.createElement("span");
		timeName.textContent = "Time (ms)";
		const timeInput = document.createElement("input");
		timeInput.type = "number";
		timeInput.dataset.role = "point-time";
		timeInput.value = String(point.time);
		timeInput.addEventListener("change", () => {
			const value = Number.parseInt(timeInput.value, 10);
			if (Number.isFinite(value)) {
				const answer = send({ op: "envelope.point.set", envelope: this.envelope, point: this.point, time: value });
				if (answer && answer.ok) {
					this.point = answer.point;
				}
				this.refreshEnvelopes();
			}
		}, { signal: this.stopping.signal });
		time.append(timeName, timeInput);
		box.append(time);

		const channels = ENVELOPE_CHANNELS[envelope.channels] || [];
		channels.forEach((channel, index) => {
			const row = document.createElement("label");
			row.className = "editor-prop";
			const name = document.createElement("span");
			name.textContent = channel.name;
			const input = document.createElement("input");
			input.type = "number";
			input.dataset.role = `point-value-${index}`;
			input.value = String(point.values[index]);
			input.addEventListener("change", () => {
				const value = Number.parseInt(input.value, 10);
				if (!Number.isFinite(value)) {
					return;
				}
				const values = point.values.slice();
				values[index] = value;
				send({ op: "envelope.point.set", envelope: this.envelope, point: this.point, values: values });
				this.refreshEnvelopes();
			}, { signal: this.stopping.signal });
			row.append(name, input);
			box.append(row);
		});

		const curve = document.createElement("label");
		curve.className = "editor-prop";
		const curveName = document.createElement("span");
		curveName.textContent = "Curve";
		const select = document.createElement("select");
		select.dataset.role = "point-curve";
		CURVES.forEach((label, index) => {
			const option = document.createElement("option");
			option.value = String(index);
			option.textContent = label;
			option.selected = index === point.curve;
			select.append(option);
		});
		select.addEventListener("change", () => {
			send({ op: "envelope.point.set", envelope: this.envelope, point: this.point, curve: Number.parseInt(select.value, 10) });
			this.refreshEnvelopes();
		}, { signal: this.stopping.signal });
		curve.append(curveName, select);
		box.append(curve);

		const remove = document.createElement("button");
		remove.className = "editor-small";
		remove.dataset.role = "delete-point";
		remove.textContent = "Delete point";
		remove.addEventListener("click", () => {
			send({ op: "envelope.point.delete", envelope: this.envelope, point: this.point });
			this.point = -1;
			this.refreshEnvelopes();
		}, { signal: this.stopping.signal });
		box.append(remove);
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
			row.addEventListener("click", () => this.stepHistory(() => this.editor.jump(index)), { signal: this.stopping.signal });
			list.append(row);
		});
	}
}

/**
 * Pointer and wheel on the canvas.
 *
 * What each does is what the editor in the client does, because somebody who
 * knows one should not have to learn the other: the left button paints with
 * the brush, held shift it takes a piece of the layer into the brush instead,
 * held control it rubs out. The right button and the middle button move the
 * map, and so does the left one while there is no layer to paint in. The
 * wheel zooms about whatever is under the pointer.
 *
 * A stroke is one change: it opens when the button goes down and closes when
 * it comes up, so the history gets one entry however far the pointer
 * travelled.
 *
 * @param editor The editor to steer.
 * @param options.canvas What to listen on, or the editor's own canvas.
 * @param options.target A function answering which layer to paint in, as
 * `{group, layer}` - the panels know, and this is how they say so. Without
 * one the pointer only moves the map.
 * @param options.onChange Called after anything was painted, for a page that
 * wants to show it.
 * @param options.onView Called when only the view moved - panned or zoomed.
 * Kept apart from `onChange` because rebuilding the panels on every pixel of
 * a drag would be work nobody asked for; what a moved view changes is what is
 * drawn over the canvas.
 * @param options.afterStroke Called while a stroke's change is still open,
 * with the layer and the rectangle it went over.
 * @param options.signal Stops listening again.
 */
function steerWithPointer(editor, options) {
	const settings = Object.assign({ canvas: null, target: null, onChange: null, onView: null, onHover: null, afterStroke: null, signal: undefined }, options || {});
	const canvas = settings.canvas || editor.canvas;
	const stopping = new AbortController();
	if (settings.signal) {
		settings.signal.addEventListener("abort", () => stopping.abort(), { once: true });
	}
	const signal = stopping.signal;
	// Which pointer is doing something, and what it is doing. One at a time:
	// a second finger on a map being painted is a mistake, not a tool.
	let doing = null;
	let pointer = 0;
	let last = { x: 0, y: 0 };
	let from = { x: 0, y: 0 };
	// The tiles a stroke has been over, so that whatever wants to look at
	// what was drawn - the automapper - is told a rectangle rather than the
	// whole layer.
	let touched = null;
	const touch = tile => {
		if (touched === null) {
			touched = { x: tile.x, y: tile.y, toX: tile.x, toY: tile.y };
			return;
		}
		touched.x = Math.min(touched.x, tile.x);
		touched.y = Math.min(touched.y, tile.y);
		touched.toX = Math.max(touched.toX, tile.x);
		touched.toY = Math.max(touched.toY, tile.y);
	};
	// Said while the stroke's change is still open, so that what it leads to
	// is part of the same history entry.
	const afterStroke = (where, box) => {
		if (settings.afterStroke !== null && where !== null && box !== null) {
			settings.afterStroke(where, box);
		}
	};

	// The canvas is measured in the units the page lays out in and drawn in
	// the pixels the screen has; a pointer that ignored the difference would
	// move the map at half the speed of the hand.
	const scale = () => (canvas.width || 1) / (canvas.getBoundingClientRect().width || 1);
	const atCanvas = event => {
		const box = canvas.getBoundingClientRect();
		const factor = scale();
		return { x: (event.clientX - box.left) * factor, y: (event.clientY - box.top) * factor };
	};
	const tileAt = event => {
		const at = atCanvas(event);
		return editor.tileAt(at.x, at.y);
	};
	const target = () => {
		const where = settings.target === null ? null : settings.target();
		return where == null || where.layer < 0 ? null : where;
	};
	// Holding on to the pointer, or letting go of it. Either may be refused -
	// a pointer that has already gone is not there to be caught - and a drag
	// that gave up over it would be a stroke that never started.
	const capture = (pointerId, hold) => {
		try {
			if (hold) {
				canvas.setPointerCapture(pointerId);
			} else {
				canvas.releasePointerCapture(pointerId);
			}
		} catch (error) {
			// The stroke still works; it just stops when the pointer leaves.
		}
	};
	const changed = () => {
		if (settings.onChange !== null) {
			settings.onChange();
		}
	};
	// Only the view moved. Kept apart from `onChange` because that one
	// rebuilds the panels, and rebuilding them on every pixel of a drag would
	// be work nobody asked for - what a moved view changes is what is drawn
	// over the canvas, and nothing else.
	const moved = () => {
		if (settings.onView !== null) {
			settings.onView();
		}
	};
	// The rectangle two corners make, as a place and a size. Either corner
	// may be the one that was there first.
	const between = (one, other) => ({
		x: Math.min(one.x, other.x),
		y: Math.min(one.y, other.y),
		width: Math.abs(other.x - one.x) + 1,
		height: Math.abs(other.y - one.y) + 1,
	});

	// Which quad point is being dragged, while one is. A quad layer is found
	// out by asking for its quads: a layer that holds none answers nothing.
	let quadPoint = null;

	/**
	 * Takes hold of a quad point under the pointer, if there is one.
	 *
	 * The points are in the coordinates of the group the layer is in, which
	 * for a group with parallax is not where the plain view says - so the
	 * pointer is asked for in those coordinates too, and the two are compared
	 * where they both mean the same thing.
	 */
	// Which sound source is being dragged, while one is.
	let sourceDrag = null;

	/**
	 * Takes hold of a sound source under the pointer, if there is one.
	 *
	 * The same sum as the quad points, for the same reason: a source lives in
	 * its group's coordinates, so the pointer is asked for in those too.
	 */
	const takeSource = (event, where) => {
		const sources = editor.sources(where.group, where.layer);
		if (sources === null) {
			return false;
		}
		const spot = atCanvas(event);
		const world = editor.groupWorldAt(where.group, spot.x, spot.y);
		if (world === null) {
			return false;
		}
		const step = editor.groupWorldAt(where.group, spot.x + HANDLE_REACH_PIXELS, spot.y);
		const reach = step === null ? 32 : Math.abs(step.x - world.x);

		let best = null;
		sources.forEach((source, index) => {
			const away = Math.hypot(source.position[0] - world.x, source.position[1] - world.y);
			if (away <= reach && (best === null || away < best.away)) {
				best = { source: index, away: away };
			}
		});
		if (best === null) {
			return false;
		}
		doing = "source";
		sourceDrag = { group: where.group, layer: where.layer, source: best.source };
		editor.begin("Move sound source");
		changed();
		return true;
	};

	const takeQuadPoint = (event, where) => {
		const quads = editor.quads(where.group, where.layer);
		if (quads === null) {
			return false;
		}
		const spot = atCanvas(event);
		const world = editor.groupWorldAt(where.group, spot.x, spot.y);
		if (world === null) {
			return false;
		}
		// How near counts, in world units: a handful of pixels, turned into
		// world units by what a pixel is worth right now.
		const step = editor.groupWorldAt(where.group, spot.x + HANDLE_REACH_PIXELS, spot.y);
		const reach = step === null ? 32 : Math.abs(step.x - world.x);

		let best = null;
		quads.forEach((quad, index) => {
			for (let point = 0; point < 5; ++point) {
				const dx = quad.points[point * 2] - world.x;
				const dy = quad.points[point * 2 + 1] - world.y;
				const away = Math.hypot(dx, dy);
				if (away <= reach && (best === null || away < best.away)) {
					best = { quad: index, point: point, away: away };
				}
			}
		});
		if (best === null) {
			return false;
		}
		doing = "quad";
		quadPoint = { group: where.group, layer: where.layer, quad: best.quad, point: best.point };
		editor.showQuad(where.group, where.layer, best.quad);
		editor.begin(best.point === 4 ? "Move quad" : "Move corner");
		changed();
		return true;
	};

	canvas.addEventListener("contextmenu", event => event.preventDefault(), { signal: signal });
	canvas.addEventListener("pointerdown", event => {
		if (doing !== null) {
			return;
		}
		pointer = event.pointerId;
		last = { x: event.clientX, y: event.clientY };
		capture(pointer, true);
		const where = target();
		if (event.button === 0 && where !== null && (takeQuadPoint(event, where) || takeSource(event, where))) {
			return;
		}
		const tile = tileAt(event);
		if (event.button !== 0 || where === null || tile === null) {
			doing = "move";
			return;
		}
		from = tile;
		if (event.altKey) {
			doing = "fill";
		} else if (event.shiftKey) {
			doing = "grab";
		} else if (event.ctrlKey || event.metaKey) {
			doing = "erase";
		} else {
			doing = "paint";
			touched = null;
			touch(tile);
			editor.begin("Draw");
			editor.paint(where.group, where.layer, tile.x, tile.y);
			changed();
		}
		if (doing !== "paint") {
			// One tile is a rectangle too, and showing it from the first
			// moment says which gesture is under way.
			editor.mark(where.group, tile.x, tile.y, 1, 1);
		}
	}, { signal: signal });

	canvas.addEventListener("pointermove", event => {
		// Where the pointer is, said on every move whether or not anything is
		// being drawn with it: what is under the pointer is a question about
		// the pointer, not about the stroke.
		if (settings.onHover !== null) {
			settings.onHover(tileAt(event));
		}
		if (doing === null || pointer !== event.pointerId) {
			return;
		}
		if (doing === "move") {
			const factor = scale();
			editor.moveByPixels(-(event.clientX - last.x) * factor, -(event.clientY - last.y) * factor);
			last = { x: event.clientX, y: event.clientY };
			// The view moved, so anything drawn over the canvas is now over
			// the wrong place. The program redraws itself; an overlay in the
			// page has to be told.
			moved();
			return;
		}
		if (doing === "quad") {
			const spot = atCanvas(event);
			const world = editor.groupWorldAt(quadPoint.group, spot.x, spot.y);
			if (world !== null) {
				editor.apply({
					op: "quad.setPoint", group: quadPoint.group, layer: quadPoint.layer,
					quad: quadPoint.quad, point: quadPoint.point,
					x: Math.round(world.x), y: Math.round(world.y),
				});
			}
			return;
		}
		if (doing === "source") {
			const spot = atCanvas(event);
			const world = editor.groupWorldAt(sourceDrag.group, spot.x, spot.y);
			if (world !== null) {
				editor.apply({
					op: "source.setPoint", group: sourceDrag.group, layer: sourceDrag.layer,
					source: sourceDrag.source, x: Math.round(world.x), y: Math.round(world.y),
				});
			}
			return;
		}
		if (doing === "paint") {
			const where = target();
			const tile = tileAt(event);
			if (where !== null && tile !== null) {
				touch(tile);
				editor.paint(where.group, where.layer, tile.x, tile.y);
				changed();
			}
			return;
		}
		// Grabbing, filling and rubbing out are about the rectangle the
		// pointer ends on, so while it is moving the rectangle is all there
		// is to show.
		const where = target();
		const tile = tileAt(event);
		if (where !== null && tile !== null) {
			const box = between(from, tile);
			editor.mark(where.group, box.x, box.y, box.width, box.height);
		}
	}, { signal: signal });

	canvas.addEventListener("pointerleave", () => {
		if (settings.onHover !== null) {
			settings.onHover(null);
		}
	}, { signal: signal });

	const release = event => {
		if (doing === null || pointer !== event.pointerId) {
			return;
		}
		if (doing === "quad" || doing === "source") {
			doing = null;
			quadPoint = null;
			sourceDrag = null;
			editor.commit();
			changed();
			capture(event.pointerId, false);
			return;
		}
		const where = target();
		const tile = tileAt(event);
		if (doing === "paint") {
			// The brush is stamped with its corner on the tile, so what it
			// covered reaches that much further than where the pointer went.
			const brush = editor.brushSize() || { width: 1, height: 1 };
			afterStroke(where, touched === null ? null : {
				x: touched.x, y: touched.y,
				width: touched.toX - touched.x + brush.width,
				height: touched.toY - touched.y + brush.height,
			});
			editor.commit();
			changed();
		} else if (where !== null && tile !== null && (doing === "grab" || doing === "erase" || doing === "fill")) {
			const box = between(from, tile);
			if (doing === "grab") {
				editor.grab(where.group, where.layer, box.x, box.y, box.width, box.height);
			} else if (doing === "fill" || doing === "erase") {
				// Opened here as well, so that what follows the stroke - the
				// automapper - lands in the same history entry. The call
				// inside opens one of its own, and one inside another is
				// still one entry.
				editor.begin(doing === "fill" ? "Fill" : "Erase");
				if (doing === "fill") {
					// The brush is laid out over the rectangle again and
					// again, so a fill of one tile and a fill of a pattern
					// are the same gesture.
					editor.fill(where.group, where.layer, box.x, box.y, box.width, box.height);
				} else {
					editor.erase(where.group, where.layer, box.x, box.y, box.width, box.height);
				}
				afterStroke(where, box);
				editor.commit();
				changed();
			}
		}
		doing = null;
		editor.mark();
		capture(event.pointerId, false);
	};
	canvas.addEventListener("pointerup", release, { signal: signal });
	canvas.addEventListener("pointercancel", event => {
		if (doing === "quad" || doing === "source") {
			editor.commit();
			changed();
		}
		if (doing === "paint") {
			// A pointer that was taken away mid-stroke leaves what it has
			// painted: throwing it out would be a surprise, and the one entry
			// it made is one undo away.
			editor.commit();
			changed();
		}
		doing = null;
		editor.mark();
	}, { signal: signal });

	canvas.addEventListener("wheel", event => {
		event.preventDefault();
		const at = atCanvas(event);
		editor.zoomAt(at.x, at.y, event.deltaY > 0 ? WHEEL_ZOOM_STEP : 1 / WHEEL_ZOOM_STEP);
		moved();
	}, { signal: signal, passive: false });
	return { destroy: () => stopping.abort() };
}

// How near a pointer has to come to a quad's handle for it to be the one that
// is taken hold of, in pixels of the canvas.
const HANDLE_REACH_PIXELS = 10;

// What one notch of the wheel does, the same step the map viewer takes.
const WHEEL_ZOOM_STEP = 1.1;

// What the channels of an envelope are called and what colour each is drawn
// in. Which of them an envelope has is its channel count: four are a colour,
// three a place and a turn, one a volume.
// What a quad has beside its points: a colour on each corner, and which
// envelopes move and colour it. The corners are named the way the file orders
// them - top left, top right, bottom left, bottom right.
// What the first run of a configuration may be filtered by, in the order the
// program counts them. "Off" is not one of them and is not in the list.
const AUTOMAP_REFERENCES = ["Game Layer", "Hookable", "Death", "Unhookable", "Freeze",
	"Unfreeze", "Deep Freeze", "Deep Unfreeze", "Live Freeze", "Live Unfreeze"];

// What SVG elements are made in. The envelope panel says it in place; here it
// is a name because the overlay makes one of these per source per frame.
/** `IStorage::TYPE_ABSOLUTE`: a path as it stands, not one to look up. */
const STORAGE_ABSOLUTE = -2;

// How many panels this page has made, so that a list one of them names is
// not a list another one finds.
let panelCount = 0;

const SVG_NAMESPACE = "http://www.w3.org/2000/svg";

// What a sound source has beside where it is. Which of the two shapes it is
// decides whether it has a radius or two sides; the rest are the same either
// way. Distances are world units, the same as everywhere a page is given one.
const SOURCE_PROPS = [
	{ prop: "shape", label: "Heard within", kind: "choice", options: [["circle", "Circle"], ["rectangle", "Rectangle"]] },
	{ prop: "loop", label: "Loop", kind: "boolean" },
	{ prop: "pan", label: "Panning", kind: "boolean" },
	{ prop: "timeDelay", label: "Delay (s)", kind: "number" },
	{ prop: "falloff", label: "Falloff", kind: "number" },
	{ prop: "posEnv", label: "Position envelope", kind: "number" },
	{ prop: "posEnvOffset", label: "Position offset", kind: "number" },
	{ prop: "soundEnv", label: "Sound envelope", kind: "number" },
	{ prop: "soundEnvOffset", label: "Sound offset", kind: "number" },
];
const SOURCE_CIRCLE = [{ prop: "radius", label: "Radius", kind: "number" }];
const SOURCE_RECTANGLE = [
	{ prop: "width", label: "Width", kind: "number" },
	{ prop: "height", label: "Height", kind: "number" },
];

const QUAD_CORNERS = ["Top left", "Top right", "Bottom left", "Bottom right"];
const QUAD_PROPS = [
	{ prop: "posEnv", label: "Position envelope", kind: "number" },
	{ prop: "posEnvOffset", label: "Position offset", kind: "number" },
	{ prop: "colorEnv", label: "Colour envelope", kind: "number" },
	{ prop: "colorEnvOffset", label: "Colour offset", kind: "number" },
];

const ENVELOPE_CHANNELS = {
	1: [{ name: "Volume", colour: "#e8b84a" }],
	3: [{ name: "X", colour: "#e8615a" }, { name: "Y", colour: "#5ad07a" }, { name: "Rotation", colour: "#5a9ce8" }],
	4: [
		{ name: "Red", colour: "#e8615a" },
		{ name: "Green", colour: "#5ad07a" },
		{ name: "Blue", colour: "#5a9ce8" },
		{ name: "Alpha", colour: "#cccccc" },
	],
};

// What the curve between two points does. The numbers are the map's own.
const CURVES = ["Step", "Linear", "Slow", "Fast", "Smooth", "Bezier"];

// One unit of an envelope value, as the file counts: 22.10 fixed point, so a
// colour channel runs 0 to 1024 and a place is in world units times this.
const ENVELOPE_ONE = 1024;

// What goes beside a physics tile, and which kinds of layer take which.
// A tile index says what the tile does; these say to which of them - and a
// layer that has none of them shows none.
const TILE_NUMBERS = {
	tele: [{ key: "number", label: "Number", max: 255 }],
	switch: [{ key: "number", label: "Number", max: 255 }, { key: "delay", label: "Delay", max: 255 }],
	speedup: [
		{ key: "force", label: "Force", max: 255 },
		{ key: "maxSpeed", label: "Max speed", max: 255 },
		{ key: "angle", label: "Angle", max: 359 },
	],
	tune: [{ key: "number", label: "Zone", max: 255 }],
};

// A tileset is sixteen by sixteen, and the index of a tile is its place in
// that square. Every map there is says it this way.
const TILESET_SIDE = 16;

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
