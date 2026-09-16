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

import DDNetBase, { addIcons, followSize, Program } from "@ddnet/base";
import { COMMANDS, commandRole, commandTitle, keyLabel, keyName, keyTable } from "./commands.js";

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
	paint: '<path d="M4 16.5 15.2 5.3a2.4 2.4 0 0 1 3.4 3.4L7.5 19.9 3 21Z" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>',
	grab: '<path d="M6 3.5v9M6 9.5 4.2 13a6 6 0 0 0 5.3 8.5H14a6 6 0 0 0 6-6V9M20 9V7M16.5 9V6.5M13 9V6" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
	fill: '<path d="M11 2.5 3.5 10a1.6 1.6 0 0 0 0 2.3l6.2 6.2a1.6 1.6 0 0 0 2.3 0l7.5-7.5Z" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/><path d="M20.5 15c1.4 2 2 3.2 2 4a2 2 0 1 1-4 0c0-.8.6-2 2-4Z"/>',
	erase: '<path d="M8.5 20.5 3 15a1.6 1.6 0 0 1 0-2.3l9.2-9.2a1.6 1.6 0 0 1 2.3 0l6.5 6.5a1.6 1.6 0 0 1 0 2.3l-8.2 8.2ZM8 8l8 8" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>',
	folder: '<path d="M2.5 6.5a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2Z" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>',
	add: '<path d="M12 4.5v15M4.5 12h15" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>',
	search: '<circle cx="10.5" cy="10.5" r="6" fill="none" stroke="currentColor" stroke-width="2"/><path d="M15 15l4.5 4.5" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/>',
	menu: '<path d="M4 7h16M4 12h16M4 17h16" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/>',
	info: '<circle cx="12" cy="12" r="8.5" fill="none" stroke="currentColor" stroke-width="2"/><path d="M12 11v5.5" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/><circle cx="12" cy="7.8" r="1.2" fill="currentColor"/>',
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

	/**
	 * Calls a map something else.
	 *
	 * The name is the name of the file the map is written to, so renaming and
	 * then saving is what "save as" is.
	 */
	rename(id, name) {
		return this.call("MapEditorRename", "number", ["number", "string"], [this.which(id), name]) === 1;
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
	 * Turns a picture into a group of tile layers drawn with palettes made
	 * out of its own colours. Answers which group it became, or -1.
	 *
	 * A picture of more than 255 colours needs more than one palette and gets
	 * a layer for each; `artColors` says how many there are, so a page can
	 * warn before it asks.
	 */
	addTileArt(name, pixels, id) {
		return this.withPixels(pixels, (address, width, height) =>
			this.call("MapEditorTileArt", "number", ["number", "string", "number", "number", "number"],
				[this.which(id), name, width, height, address]));
	}

	/** How many colours a picture holds, not counting what is not opaque. */
	artColors(pixels) {
		return this.withPixels(pixels, (address, width, height) =>
			this.ask("MapEditorArtColors", "number", [width, height, address])) || 0;
	}

	/**
	 * Turns a picture into a group with one quad per pixel - or per run of
	 * pixels of one colour. Answers which group it became, or -1.
	 */
	addQuadArt(name, pixels, options, id) {
		const settings = Object.assign({ pixelStep: 1, quadSize: 64, centralize: false, merge: true }, options || {});
		return this.withPixels(pixels, (address, width, height) =>
			this.call("MapEditorQuadArt", "number",
				["number", "string", "number", "number", "number", "number", "number", "number", "number"],
				[this.which(id), name, width, height, address, settings.pixelStep, settings.quadSize,
					settings.centralize ? 1 : 0, settings.merge ? 1 : 0]));
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

	/**
	 * The maps that lie in the browser's own storage, newest name first.
	 *
	 * The program says it, not the page: the files are in the program's own
	 * file system, and a page cannot look into it.
	 */
	saved() {
		const text = this.call("MapEditorSaved", "string", [], []);
		try {
			return text === null ? [] : JSON.parse(text);
		} catch (error) {
			return [];
		}
	}

	/**
	 * Asks for a picture of the whole map, drawn a band at a time over the
	 * frames that follow and handed out as a PNG when it is done.
	 */
	picture(id) {
		return this.ask("MapEditorPicture", "number", [this.which(id)]) === 1;
	}
	/** 0 never asked, 1 being drawn, 2 handed over, 3 failed. */
	pictureState() {
		return this.call("MapEditorPictureState", "number") || 0;
	}
	/** How far the picture has got, from 0 to 1. */
	pictureProgress() {
		return this.call("MapEditorPictureProgress", "number") || 0;
	}

	/**
	 * Which entities sheet physics layers are drawn out of - `ddnet`, `race`,
	 * `fng`, `vanilla` and the rest of `data/editor/entities_clear/`. Called
	 * with a name it sets it, for every map; a name that is not one of them
	 * changes nothing.
	 */
	entitiesImage(name) {
		if (name !== undefined) {
			this.call("MapEditorSetEntitiesImage", "number", ["string"], [name]);
		}
		return this.call("MapEditorEntitiesImage", "string") || "ddnet";
	}

	/**
	 * Whether a tile that does nothing in a physics layer may be put there.
	 * Called with a value it sets it.
	 */
	allowUnused(on) {
		if (on !== undefined) {
			this.ask("MapEditorSetAllowUnused", null, [on ? 1 : 0]);
		}
		return this.call("MapEditorAllowUnused", "number") === 1;
	}

	/** Opens one of them by name, and puts it in front. */
	openSaved(name) {
		return this.call("MapEditorOpenSaved", "number", ["string"], [name || ""]);
	}

	/**
	 * Writes the map out under another name without becoming that map.
	 *
	 * The map one is working on keeps its name and its place in the history,
	 * so the dot that says "not saved" stays where it was. Saving *as* is a
	 * rename and then a save, which is a different thing.
	 */
	saveCopy(id, name, options) {
		const handout = !(options && options.handout === false);
		return this.call("MapEditorSaveCopy", "number", ["number", "string", "number"],
			[this.which(id), name || "", handout ? 1 : 0]) === 1;
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

	/** Whether there is nothing in hand. An empty brush is what grabs. */
	brushEmpty() {
		const size = this.brushSize();
		return size === null || size.width === 0 || size.height === 0;
	}

	/** Puts the brush down. */
	clearBrush() {
		this.call("MapEditorClearBrush", null);
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
/**
 * Where each panel goes when the editor has areas to put it in, and under
 * which tab it stands there.
 *
 * The panels themselves know nothing of this: they are built as one lot and
 * then moved. A page that places them itself gets them all in one column, the
 * way it always did.
 */
const PANEL_PLACES = [
	{ role: "tree-panel", area: "left", tab: "layers", name: "Layers" },
	{ role: "images-panel", area: "left", tab: "images", name: "Images" },
	{ role: "audio-panel", area: "left", tab: "sounds", name: "Sounds" },
	{ role: "info-panel", area: "left", tab: "map", name: "Map" },
	{ role: "props-panel", area: "right" },
	{ role: "tiles-panel", area: "right" },
	{ role: "quads-panel", area: "right" },
	{ role: "sounds-panel", area: "right" },
	{ role: "envelopes-panel", area: "dock", tab: "envelopes", name: "Envelopes" },
	{ role: "history-panel", area: "dock", tab: "history", name: "History" },
	{ role: "settings-panel", area: "dock", tab: "settings", name: "Settings" },
	{ role: "rules-panel", area: "dock", tab: "rules", name: "Rules" },
];

/** The two areas that show one panel at a time, and what they are called. */
const TABBED_AREAS = { left: "structure", dock: "dock" };

// Two editors on a page are two of everything, and a row of a list can only
// be pointed at by an id that is the page's alone.
let overCount = 0;

/**
 * Which shape the editor takes at which size of its box, widest first.
 *
 * One table rather than a handful of container queries, because the same
 * numbers decide two things: what the stylesheet draws, and what the panels
 * do - a drawer is not a column that moved, it also shuts when somebody
 * touches the map, and a tool bar that shows six of its buttons has to be
 * told which six. Two copies of six numbers would drift apart; the stylesheet
 * answers to the attribute this sets.
 *
 * The widths are of the *box*, not of the window: an editor in an 800-pixel
 * hole in somebody's page is a narrow editor on a wide screen.
 */
// Which lists have a menu of their own, and what each of their rows is. One
// list, because two things read it: what a right-click opens, and where the
// "..." buttons go.
const CONTEXT_LISTS = [
	["tree", null],
	["image-list", "image"],
	["sound-list", "sound"],
	["quad-list", "quad"],
	["source-list", "source"],
	["setting-list", "setting"],
];

const BOX_WIDTHS = [
	{ from: 2560, name: "huge", left: "column", right: "column", bar: "labels" },
	{ from: 1600, name: "desk", left: "column", right: "column", bar: "labels" },
	{ from: 1200, name: "wide", left: "column", right: "column", bar: "labels" },
	{ from: 900, name: "medium", left: "drawer", right: "column", bar: "icons" },
	{ from: 600, name: "small", left: "drawer", right: "drawer", bar: "icons" },
	{ from: 0, name: "phone", left: "sheet", right: "sheet", bar: "few" },
];

/** And by height: what there is room for above and below the map. */
const BOX_HEIGHTS = [
	{ from: 1400, name: "high", head: "two", status: "line", dock: "strip" },
	{ from: 600, name: "tall", head: "two", status: "line", dock: "strip" },
	{ from: 480, name: "low", head: "one", status: "chip", dock: "overlay" },
	{ from: 0, name: "short", head: "one", status: "chip", dock: "overlay" },
];

// The order the menu puts its headings in: what a map is, then what was just
// done to it, then what one is looking at, then the things one reaches for.
const MENU_ORDER = ["File", "Edit", "View", "Layer", "Tools", "Settings", "Help"];

// How long a note about what happened stays, and how many may stand at once.
const TOAST_MS = 4000;
const TOAST_MOST = 4;

// How far a column or the dock may be dragged, in pixels. The lower end is
// what the panel inside needs; the upper end is where it starts eating the
// map, which is the thing one is actually working on.
const DRAG_LIMITS = {
	left: { least: 240, most: 480 },
	right: { least: 288, most: 560 },
	dock: { least: 160, most: 640 },
};

const PANELS_HTML = `
<div class="editor-bar" data-role="bar">
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
		<div class="editor-art" data-role="art">
			<button class="editor-small" data-role="tile-art" title="A picture as tiles, with a palette made of its own colours">picture as tiles&hellip;</button>
			<button class="editor-small" data-role="quad-art" title="A picture as quads, one per pixel">as quads&hellip;</button>
			<label class="editor-art-field">px <input type="number" data-role="art-step" min="1" max="64" value="1" title="How many pixels of the picture one quad stands for"></label>
			<label class="editor-art-field">size <input type="number" data-role="art-size" min="1" max="1024" value="64" title="How wide a quad is on the map, in world units"></label>
			<label class="editor-art-field"><input type="checkbox" data-role="art-merge" checked title="A run of one colour becomes one quad"> merge</label>
			<label class="editor-art-field"><input type="checkbox" data-role="art-centralize" title="Every quad turns about the same place"> one pivot</label>
			<input type="file" accept="image/png,image/*" data-role="art-file" hidden>
		</div>
	</section>
	<section class="editor-panel" data-role="props-panel">
		<header class="editor-panel-head"><h2 data-role="props-title">Properties</h2><span class="editor-here" data-role="here"></span></header>
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
		<div class="editor-tabs editor-subtabs" role="tablist">
			<button type="button" class="editor-tab" data-role="tiles-tab" data-tab="tiles" role="tab" aria-selected="true">Tiles</button>
			<button type="button" class="editor-tab" data-role="tiles-tab" data-tab="automap" role="tab" aria-selected="false">Automap</button>
		</div>
		<div data-role="tiles-body">
			<canvas class="editor-tileset" data-role="tileset" width="256" height="256"></canvas>
			<div class="editor-numbers" data-role="numbers"></div>
			<div class="editor-slots" data-role="slots" role="group" aria-label="Where a brush is put away"></div>
			<div class="editor-type" data-role="type">
				<input type="text" data-role="type-text" placeholder="Type with the tiles&hellip;" title="Letters and digits become the tiles of a font tileset; the layer has to be drawn with one">
				<button class="editor-small" data-role="type-place" title="Write it where the view is looking">write</button>
			</div>
		</div>
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
				<button class="editor-small" data-role="knife" title="Cut a piece out of the quad that is picked: four clicks inside it" aria-pressed="false">knife</button>
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
			</span>
		</header>
		<div class="editor-props" data-role="info-props"></div>
		<ul class="editor-memory" data-role="memory"></ul>
		<input type="file" accept=".map" data-role="append-file" hidden>
	</section>
	<section class="editor-panel" data-role="settings-panel">
		<header class="editor-panel-head">
			<h2>Server settings</h2>
			<span class="editor-panel-tools">
				<button class="editor-small" data-role="add-setting" title="A line the server runs when it loads the map">+ setting</button>
				<button class="editor-small" data-role="delete-setting" title="Take this line away">-</button>
			</span>
		</header>
		<ol class="editor-settings" data-role="setting-list"></ol>
		<p class="editor-setting-said" data-role="setting-said" role="status"></p>
		<datalist data-role="setting-names"></datalist>
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
		const settings = Object.assign({ container: null, dataBase: null, keys: true, signal: undefined }, options || {});
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
		// The big tile chooser over the map, and whether it stays open when
		// the key that opened it is let go of.
		this.picker = null;
		this.zoomChip = null;
		this.floatHome = null;
		// Whether a keyboard has been seen. A desk has one until proven
		// otherwise; a finger has to show one first.
		this.sawKey = false;
		// The native editor's two settings about tiles: the tileset shown in
		// the layer's colour (on, as there), and whether a tile that does
		// nothing in a physics layer may be put down (off, as there).
		this.brushColouring = true;
		this.unusedSaidAt = -Infinity;
		// What a long press says, where a pointer would have hovered.
		this.tip = null;
		// Which shape the box is in, once something tells us. Without a box
		// there are no areas to reshape and the panels stand in one column.
		this.shape = null;
		this.readonly = false;
		this.askingLayer = false;
		// Where the pointer last was over the map. On a big screen the tile
		// chooser opens there rather than in the middle of a monitor that is
		// eighty centimetres wide.
		this.pointerAt = null;
		// The strip of open maps, and what each of them looked like when it
		// was last in front. The program keeps the maps; what somebody had
		// picked, which tab was open and which groups were folded up are the
		// page's, and they are what makes coming back to a map feel like
		// coming back rather than like opening it again.
		this.maps = null;
		this.mapState = new Map();
		this.lastMap = null;
		this.toasts = null;
		// What somebody dragged the columns and the dock to, in pixels, or
		// nothing where nobody has dragged them.
		this.dragged = { left: null, right: null, dock: null };
		this.drawer = { left: false, right: false };
		// What floats over the whole box, and where it floats in.
		this.over = null;
		this.overId = `ed${++overCount}`;
		this.palette = null;
		this.paletteAt = 0;
		this.paletteRows = [];
		this.menu = null;
		this.context = null;
		this.dialog = null;
		this.pickerPinned = false;
		// The list of layers under a spot on the map.
		this.chooser = null;
		// Which of the ten places a brush has been put away in are full. The
		// program does not say, so what was put away here is remembered here.
		this.slotsUsed = new Set();
		// Whether what a player would see is drawn, and at which zoom:
		// "off", "game" or "menu".
		this.proof = "off";
		// The places clicked with the knife so far, or null when it is not
		// out. Four of them make a cut.
		this.carving = null;
		// The picture of the tiles, what it was fetched from, and the
		// rectangle that was taken out of it.
		this.dataBase = settings.dataBase || new URL("data/", location.href).href;
		this.tileset = null;
		this.tilesetSource = null;
		this.picked = null;
		// Whether the panels listen for keys on the whole page themselves.
		this.keys = settings.keys;
		// The areas the panels were spread into, or null while they all stand
		// in one column.
		this.areas = null;
		// The box the panels were spread into, if they were.
		this.box = null;
		// Which panel each area shows, for the two that show one at a time.
		this.tab = { left: "layers", dock: "envelopes", tiles: "tiles" };
		// Which of the four ways the pointer draws, while no modifier says
		// otherwise.
		this.tool = "paint";
		// How much the line under the pointer says about a tile: "off", "dec"
		// or "hex".
		this.tileInfo = "hex";
		// The commands, and the keys that reach them.
		this.commands = COMMANDS;
		this.keys_ = keyTable(COMMANDS);
		// Whether the strip at the bottom is open. It starts closed: what is
		// in it - envelopes, the history, the server settings, the rules - is
		// looked at now and then, and the map should not pay two hundred
		// pixels for it the whole time.
		this.dockOpen = false;
		// Whether each panel has anything to show at all - a different
		// question from whether its tab is the one in front.
		this.panelShown = new Map();
		this.root = document.createElement("div");
		this.root.className = "editor-panels";
		this.root.innerHTML = PANELS_HTML;
		this.buildBar();
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
		// The big tile chooser, over the map rather than beside it: a tileset
		// in a 320-pixel column is seventeen pixels a tile, and seventeen
		// pixels is not a tile anybody can tell from its neighbour.
		this.picker = document.createElement("div");
		this.picker.className = "editor-picker";
		this.picker.dataset.role = "picker";
		this.picker.hidden = true;
		this.picker.innerHTML = '<canvas class="editor-picker-tiles" data-role="picker-tiles"></canvas>'
			+ '<p class="editor-picker-name" data-role="picker-name"></p>';
		// Which layer a spot on the map belongs to, when more than one does.
		// What the zoom is, where the hand is. The status line says it too, at
		// the other end of the screen; on a wide one those are not the same
		// place, and this is the one a finger can reach.
		this.zoomChip = document.createElement("div");
		this.zoomChip.className = "editor-zoom";
		this.zoomChip.dataset.role = "zoom-chip";
		this.zoomChip.innerHTML = '<button type="button" class="editor-zoom-step" data-role="zoom-out" aria-label="Further away">\u2212</button>'
			+ '<button type="button" class="editor-zoom-level" data-role="zoom-level"></button>'
			+ '<button type="button" class="editor-zoom-step" data-role="zoom-in" aria-label="Closer">+</button>';
		this.chooser = document.createElement("ul");
		this.chooser.className = "editor-choose";
		this.chooser.dataset.role = "layer-choose";
		this.chooser.hidden = true;
		// Where the things that float over the map live. It is not the
		// element-wide over-layer: a note in the top right corner of that
		// would sit on the right column and cover what it is telling about.
		this.floatHome = parent;
		parent.append(this.overlay, this.picker, this.chooser, this.zoomChip);
		this.wirePick(this.picker.querySelector('[data-role="picker-tiles"]'));
		this.wireChooser();
		this.wireZoomChip();
	}

	/**
	 * Picking a rectangle out of a tileset, on whichever canvas shows one.
	 * The small one beside the map and the big one over it are the same
	 * gesture and the same answer.
	 */
	wirePick(canvas) {
		let from = null;
		// Nineteen pixels a tile is a picture of what is in hand, not a thing
		// a finger can hit. A touch on it opens the big one, where the tiles
		// are forty-four.
		canvas.addEventListener("pointerdown", event => {
			if (canvas.dataset.role !== "tileset" || !this.finger()) {
				return;
			}
			event.preventDefault();
			event.stopPropagation();
			this.run("picker.show");
		}, { capture: true, signal: this.stopping.signal });
		// A refused capture must not take the pick with it.
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
				// A pick taken from the big one is what it was opened for.
				if (this.picker !== null && canvas === this.picker.firstElementChild && !this.pickerPinned) {
					this.showPicker(false);
				}
			}
		};
		canvas.addEventListener("pointerup", release, { signal: this.stopping.signal });
		canvas.addEventListener("pointercancel", () => { from = null; }, { signal: this.stopping.signal });
	}

	/**
	 * Which layer a spot on the map belongs to. Ctrl and the right button ask
	 * it: every tile layer that has something other than air at that spot is
	 * one line, nearest the front first, and picking one selects it.
	 */
	wireChooser() {
		const canvas = this.editor.canvas;
		canvas.addEventListener("contextmenu", event => {
			if (!event.ctrlKey && !event.metaKey) {
				return;
			}
			event.preventDefault();
			this.showChooser(event);
		}, { signal: this.stopping.signal });
		// Anywhere else shuts it again.
		document.addEventListener("pointerdown", event => {
			if (this.chooser !== null && !this.chooser.hidden && !this.chooser.contains(event.target)) {
				this.chooser.hidden = true;
			}
		}, { signal: this.stopping.signal });
	}

	/** The three buttons of the zoom chip, all of them commands. */
	wireZoomChip() {
		const on = (role, id) => {
			const button = this.zoomChip.querySelector(`[data-role="${role}"]`);
			button.title = commandTitle(this.commands.find(which => which.id === id), this.keysShown());
			button.addEventListener("click", () => this.run(id), { signal: this.stopping.signal });
		};
		on("zoom-out", "view.zoomOut");
		on("zoom-in", "view.zoomIn");
		// The number is a button too: it says what the zoom is, and pressing it
		// asks for the one zoom nobody has to think about.
		on("zoom-level", "view.fit");
	}

	/** What lies under a spot on the map, as a list to pick from. */
	layersAt(x, y) {
		const found = [];
		if (this.map === null) {
			return found;
		}
		this.map.groups.forEach((group, gi) => {
			group.layers.forEach((layer, li) => {
				if (layer.type !== "tiles") {
					return;
				}
				const world = this.editor.groupWorldAt(gi, x, y);
				if (world === null) {
					return;
				}
				const tile = { x: Math.floor(world.x / MAP_TILE_SIZE), y: Math.floor(world.y / MAP_TILE_SIZE) };
				const index = this.editor.tileIndex(gi, li, tile.x, tile.y);
				if (index > 0) {
					found.push({ group: gi, layer: li, name: layer.name || layer.kind, index: index, tile: tile });
				}
			});
		});
		// Nearest the front first: the last group is drawn over the others.
		return found.reverse();
	}

	showChooser(event) {
		const box = this.editor.canvas.getBoundingClientRect();
		const factor = (this.editor.canvas.width || 1) / (box.width || 1);
		const found = this.layersAt((event.clientX - box.left) * factor, (event.clientY - box.top) * factor);
		this.chooser.textContent = "";
		if (found.length === 0) {
			const empty = document.createElement("li");
			empty.className = "editor-choose-empty";
			empty.textContent = "Nothing but air here";
			this.chooser.append(empty);
		}
		for (const what of found) {
			const row = document.createElement("li");
			row.className = "editor-row";
			row.dataset.role = "layer-choice";
			row.dataset.group = String(what.group);
			row.dataset.layer = String(what.layer);
			row.textContent = `${what.name} \u00b7 ${what.index}`;
			row.addEventListener("click", () => {
				this.selection = { group: what.group, layer: what.layer };
				this.chooser.hidden = true;
				this.refresh();
			}, { signal: this.stopping.signal });
			this.chooser.append(row);
		}
		this.chooser.style.left = `${event.clientX - box.left}px`;
		this.chooser.style.top = `${event.clientY - box.top}px`;
		this.chooser.hidden = false;
	}

	/** Opens or shuts the big tile chooser. */
	showPicker(on) {
		const layer = this.selectedLayer();
		const wanted = on === true && layer !== null && layer.type === "tiles";
		this.picker.hidden = !wanted;
		if (!wanted) {
			this.pickerPinned = false;
			return;
		}
		const canvas = this.picker.querySelector('[data-role="picker-tiles"]');
		// A finger needs a bigger tile than an eye does, and the whole box
		// rather than the map: a tileset sized to fit a short map would be
		// nineteen pixels a tile again. Wide enough is what counts, because
		// the chooser may scroll downwards and a finger scrolls it.
		const finger = this.finger();
		const home = finger ? this.overlayHome() : null;
		if (home !== null && this.picker.parentElement !== home) {
			home.append(this.picker);
		}
		const box = (home === null ? this.editor.canvas : home).getBoundingClientRect();
		// As big as there is room for, and never smaller than the thirty-two
		// pixels a tile needs to be told apart - forty-four for a finger.
		const least = finger ? 44 : 32;
		// And a ceiling, or a forty-inch monitor would show a tileset seventeen
		// hundred pixels across: past a point a bigger tile says nothing more
		// about which tile it is, and the chooser only gets harder to take in
		// at a glance and harder to fit beside the pointer.
		const most = finger ? 88 : 64;
		const room = finger ? box.width : Math.min(box.width, box.height);
		const fits = Math.floor((room - 32) / TILESET_SIDE) * TILESET_SIDE;
		const side = Math.min(TILESET_SIDE * most, Math.max(TILESET_SIDE * least, fits));
		canvas.width = side;
		canvas.height = side;
		canvas.style.width = `${side}px`;
		canvas.style.height = `${side}px`;
		const image = layer.image >= 0 && layer.image < this.map.images.length ? this.map.images[layer.image] : null;
		this.picker.querySelector('[data-role="picker-name"]').textContent = image === null ? "No picture - the numbers are the tiles" : image.name;
		this.placePicker(side);
		this.paintPicker();
	}

	/**
	 * Where the chooser opens.
	 *
	 * In the middle of what holds it, until the screen is big enough that the
	 * middle is somewhere else entirely: on a forty-inch monitor the middle is
	 * forty centimetres from the hand, and a chooser that opens there is a
	 * chooser one has to go and find. Then it opens where the pointer is, and
	 * is pushed back inside the edges rather than hanging over them.
	 */
	placePicker(side) {
		const roomy = this.shape !== null && this.shape.stack === true;
		const at = this.pointerAt;
		if (!roomy || at === null) {
			this.picker.style.left = "";
			this.picker.style.top = "";
			this.picker.style.transform = "";
			return;
		}
		const home = this.picker.parentElement;
		const room = home.getBoundingClientRect();
		// Measured rather than guessed: the box is the tiles plus its padding,
		// its border and the line of text under it.
		const box = this.picker.getBoundingClientRect();
		const width = Math.max(box.width, side);
		const height = Math.max(box.height, side);
		const left = Math.max(8, Math.min(at.x - room.left - width / 2, room.width - width - 8));
		const top = Math.max(8, Math.min(at.y - room.top - height / 2, room.height - height - 8));
		this.picker.style.left = `${left}px`;
		this.picker.style.top = `${top}px`;
		this.picker.style.transform = "none";
	}

	paintPicker() {
		if (this.picker === null || this.picker.hidden) {
			return;
		}
		this.paintTileset(this.picker.querySelector('[data-role="picker-tiles"]'));
	}

	/**
	 * The strip of open maps.
	 *
	 * Light DOM in the element's header slot, like everything else the editor
	 * draws, so that the one stylesheet dresses it; where there is no room for
	 * a header row it moves down into the tool bar, which is the one row there
	 * always is.
	 */
	buildMaps(box) {
		this.maps = document.createElement("div");
		this.maps.className = "editor-maps";
		this.maps.dataset.role = "maps";
		this.maps.setAttribute("role", "tablist");
		this.maps.setAttribute("aria-label", "The maps that are open");
		this.maps.slot = "header";
		// Before whatever the page put in the header: what the map is called
		// and whether it is saved are the two things nobody may have to look
		// for, so they go first.
		box.prepend(this.maps);
		this.refreshMaps();
	}

	/** Puts the strip where this shape of box has room for it. */
	placeMaps() {
		if (this.maps === null) {
			return;
		}
		const inBar = this.shape !== null && this.shape.head === "one";
		const bar = this.part("bar");
		if (inBar && bar !== null && this.maps.parentElement !== bar) {
			this.maps.removeAttribute("slot");
			bar.prepend(this.maps);
		} else if (!inBar && this.box !== null && this.box !== undefined && this.maps.parentElement !== this.box) {
			this.maps.slot = "header";
			this.box.prepend(this.maps);
		}
	}

	refreshMaps() {
		if (this.maps === null) {
			return;
		}
		const open = this.editor.maps;
		const now = this.editor.map;
		// Only rebuilt when the maps themselves changed. A strip built anew on
		// every switch would throw away the button that was just clicked, and
		// with it the focus of whoever clicked it with a keyboard.
		const there = [...this.maps.querySelectorAll('[data-role="map-tab"]')];
		if (there.length === open.length && there.every((tab, at) => Number(tab.dataset.map) === open[at])) {
			for (const tab of there) {
				const id = Number(tab.dataset.map);
				const dirty = this.editor.dirty(id);
				const name = this.editor.name(id) || "untitled";
				tab.setAttribute("aria-selected", id === now ? "true" : "false");
				tab.querySelector('[data-role="map-dot"]').hidden = !dirty;
				tab.querySelector(".editor-map-name").textContent = name;
				tab.title = `${name}${dirty ? " - not saved" : ""}`;
			}
			return;
		}
		this.maps.textContent = "";
		for (const id of open) {
			const tab = document.createElement("button");
			tab.type = "button";
			tab.className = "editor-map-tab";
			tab.dataset.role = "map-tab";
			tab.dataset.map = String(id);
			tab.setAttribute("role", "tab");
			tab.setAttribute("aria-selected", id === now ? "true" : "false");
			const dirty = this.editor.dirty(id);
			const dot = document.createElement("span");
			dot.className = "editor-map-dot";
			dot.dataset.role = "map-dot";
			dot.hidden = !dirty;
			dot.textContent = "\u25cf";
			const name = document.createElement("span");
			name.className = "editor-map-name";
			name.textContent = this.editor.name(id) || "untitled";
			tab.append(dot, name);
			// The dot is for the eye; the title is for whoever is not reading
			// with their eyes.
			tab.title = `${name.textContent}${dirty ? " - not saved" : ""}`;
			tab.addEventListener("click", () => this.showMap(id), { signal: this.stopping.signal });
			if (open.length > 1) {
				const shut = document.createElement("span");
				shut.className = "editor-map-close";
				shut.dataset.role = "map-close";
				shut.textContent = "\u00d7";
				shut.setAttribute("role", "button");
				shut.setAttribute("aria-label", `Close ${name.textContent}`);
				shut.addEventListener("click", event => {
					event.stopPropagation();
					this.closeMap(id);
				}, { signal: this.stopping.signal });
				tab.append(shut);
			}
			this.maps.append(tab);
		}
		const add = document.createElement("button");
		add.type = "button";
		add.className = "editor-map-add";
		add.dataset.role = "map-add";
		add.textContent = "+";
		const command = this.commands.find(which => which.id === "file.new");
		add.title = command === undefined ? "New map" : commandTitle(command, this.keysShown());
		add.setAttribute("aria-label", "New map");
		add.addEventListener("click", () => this.run("file.new"), { signal: this.stopping.signal });
		this.maps.append(add);
		this.placeMaps();
	}

	/**
	 * What the page remembers about a map while another one is in front.
	 *
	 * The program keeps the maps. What was picked, which tab was open and
	 * which groups were folded up are the page's, and they are what makes
	 * coming back to a map feel like coming back rather than like opening it.
	 */
	rememberMap(which) {
		const now = which === undefined ? this.editor.map : which;
		if (now < 0) {
			return;
		}
		this.mapState.set(now, {
			selection: { group: this.selection.group, layer: this.selection.layer },
			collapsed: new Set(this.collapsed),
			tab: Object.assign({}, this.tab),
			dockOpen: this.dockOpen,
			tool: this.tool,
		});
	}

	/** Puts a map in front, with everything about it as it was left. */
	showMap(id) {
		if (id === this.editor.map) {
			return true;
		}
		this.rememberMap();
		if (!this.editor.activate(id)) {
			return false;
		}
		// Said before the state is put back, so that the refresh that follows
		// does not put the restored state away under the old map's name.
		this.lastMap = id;
		const was = this.mapState.get(id);
		if (was === undefined) {
			this.selection = { group: 0, layer: 0 };
			this.collapsed = new Set();
		} else {
			this.selection = { group: was.selection.group, layer: was.selection.layer };
			this.collapsed = new Set(was.collapsed);
			this.tab = Object.assign({}, was.tab);
			this.dockOpen = was.dockOpen;
			this.tool = was.tool;
		}
		this.refresh();
		this.refreshMaps();
		return true;
	}

	/** Closes one, and puts another in front if that was the one in front. */
	closeMap(id, asked) {
		const open = this.editor.maps;
		if (open.length < 2) {
			return false;
		}
		// What was changed and not written out is gone the moment the map is:
		// the history goes with it. So it is asked first, once.
		if (asked !== true && this.editor.dirty(id)) {
			this.askYesNo("Close the map", `${this.editor.name(id) || "This map"} has changes that were never saved.`,
				"Close it anyway", () => this.closeMap(id, true));
			return false;
		}
		if (id === this.editor.map) {
			this.showMap(open.find(which => which !== id));
		}
		this.mapState.delete(id);
		this.editor.close(id);
		this.refresh();
		this.refreshMaps();
		return true;
	}

	/**
	 * Where a thing that floats over everything goes.
	 *
	 * In a box that is a layer of its own above the six areas, so that a menu
	 * opened from a row of the tree is not cut off by the edge of the column
	 * the tree stands in. Without a box it is whatever holds the canvas,
	 * which is then the only thing there is.
	 */
	overlayHome() {
		const box = this.box;
		if (box !== null && box !== undefined && box.shadowRoot !== null && box.shadowRoot !== undefined) {
			if (this.over === null) {
				this.over = document.createElement("div");
				this.over.className = "editor-over";
				this.over.slot = "over";
				box.append(this.over);
			}
			return this.over;
		}
		const canvas = this.editor.canvas;
		return canvas === null || canvas === undefined ? null : canvas.parentElement;
	}

	/**
	 * Brings whatever carries that name into view and hands it back.
	 *
	 * A command that is a button in a panel has to open the panel first, or
	 * it would press a button nobody can see and the page would change
	 * somewhere the eye is not. Which tab a panel is in is not written down
	 * twice: the panel says where it hangs, and `PANEL_PLACES` says which tab
	 * that is.
	 */
	reveal(role) {
		const found = this.part(role);
		if (found === null) {
			return null;
		}
		const panel = found.closest("[data-role$=\"-panel\"]");
		const place = panel === null ? undefined : PANEL_PLACES.find(which => which.role === panel.dataset.role);
		if (place !== undefined) {
			// The dock opens by being told which tab it shows; the two
			// columns are shown or not shown.
			if (place.area === "left" || place.area === "right") {
				this.showArea(place.area, true);
			}
			if (place.tab !== undefined && place.tab !== null) {
				this.showTab(place.area, place.tab);
			}
		}
		// The tiles panel has two tabs of its own, and some of the buttons
		// live in the second one.
		if (found.closest('[data-role="automap"]') !== null) {
			this.tab.tiles = "automap";
			this.applyTilesTab();
		} else if (found.closest('[data-role="tiles-body"]') !== null) {
			this.tab.tiles = "tiles";
			this.applyTilesTab();
		}
		return found;
	}

	/**
	 * Takes the shape the box says it is in.
	 *
	 * A side that became a drawer is shut, because a drawer lies over the map
	 * and an editor that opened with its map covered would be an editor whose
	 * first act is in the way. A side that became a column again is opened,
	 * because a column takes room of its own and an empty one is a stripe of
	 * nothing.
	 */
	applyShape(shape) {
		const before = this.shape;
		this.shape = shape;
		this.readonly = shape.readonly;
		// The panels say for themselves whether they are drawn for a finger,
		// so that the stylesheet has one answer to read whether they stand in
		// the element or on a page of their own. Every area is told as well as
		// the root: each of them carries `editor-panels` too, and one that was
		// not told would go on declaring the sizes the query asked for.
		const big = this.finger() ? "yes" : "no";
		this.root.dataset.big = big;
		if (this.areas !== null) {
			for (const area of Object.values(this.areas)) {
				area.dataset.big = big;
			}
		}
		// The dock is shut to begin with, because what is in it is looked at
		// now and then; with room enough it is open, because then it costs
		// nothing. Said once, when the box first says there is room, so that
		// somebody who shut it keeps it shut.
		if (shape.stack && (before === null || !before.stack)) {
			this.dockOpen = true;
		}
		for (const side of ["left", "right"]) {
			const drawer = shape[side] !== "column";
			if (before === null || drawer !== (before[side] !== "column")) {
				this.drawer[side] = drawer;
				this.showArea(side, !drawer);
			}
			if (shape[side] === "none") {
				this.showArea(side, false);
			}
		}
		this.applyTabs();
		this.applyTilesTab();
		this.applyDragged();
		this.placeMaps();
		this.refreshBar();
	}

	/**
	 * Whether this shape of box shows that button.
	 *
	 * `none` shows none of them, `looking` only what does not change the map,
	 * `few` only what the plan calls the six a phone has room for, and the two
	 * wide shapes show all of them - with the modes' names written out only
	 * where there is room for the words.
	 */
	barShows(command) {
		const how = this.shape === null ? "labels" : this.shape.bar;
		if (how === "none") {
			return false;
		}
		// Three of the buttons are keys on a desk and have to be buttons for a
		// finger: there is no Escape to empty the brush with, nothing to hold
		// to keep the tile chooser open, and no Ctrl to hold while pressing
		// the right button that a finger also does not have.
		if (command.touch === true && !this.finger()) {
			return false;
		}
		if (how === "looking") {
			return command.safe === true;
		}
		if (how === "few") {
			return command.always === true;
		}
		return true;
	}

	/**
	 * A press on the map shuts an open drawer, and does not go on to the map.
	 *
	 * A click that puts something away does not also paint: the hand that
	 * reached past the drawer was reaching for the drawer's edge, not for the
	 * tile behind it.
	 */
	wireDrawers() {
		const canvas = this.editor.canvas;
		if (canvas === null || canvas === undefined) {
			return;
		}
		canvas.addEventListener("pointermove", event => {
			this.pointerAt = { x: event.clientX, y: event.clientY };
		}, { signal: this.stopping.signal });
		canvas.addEventListener("pointerdown", event => {
			this.pointerAt = { x: event.clientX, y: event.clientY };
			const open = ["left", "right"].filter(side => this.drawer[side] && this.areaShown(side));
			if (open.length === 0) {
				// A finger that starts at the very edge is reaching for the
				// drawer that lives there, not painting: strokes begin with
				// the finger on the map.
				this.edgeSwipe(event, canvas);
				return;
			}
			for (const side of open) {
				this.showArea(side, false);
			}
			event.stopPropagation();
			event.preventDefault();
		}, { capture: true, signal: this.stopping.signal });
	}

	/**
	 * What a button is called, for a hand that cannot hover.
	 *
	 * A `title` is a pointer's affordance: it appears because the mouse rested
	 * there, and a finger never rests anywhere without pressing. So at a
	 * coarse pointer a long press says the same thing in the same words - the
	 * ones the button already carries, so that there is one text and not two.
	 */
	wireTips() {
		// On the document rather than on the box: the areas are the element's
		// light DOM and the box is not known yet when the panels are wired, so
		// whose press this is gets asked when it happens rather than now.
		const root = document;
		const mine = target => {
			if (target === null || target === undefined || target.nodeType !== 1) {
				return false;
			}
			return (this.box !== null && this.box !== undefined && this.box.contains(target))
				|| (this.root !== null && this.root.contains(target));
		};
		let waiting = null;
		let from = null;
		const drop = () => {
			if (waiting !== null) {
				clearTimeout(waiting);
				waiting = null;
			}
		};
		const hide = () => {
			drop();
			if (this.tip !== null) {
				this.tip.hidden = true;
			}
		};
		root.addEventListener("pointerdown", event => {
			hide();
			if (event.pointerType !== "touch" && event.pointerType !== "pen") {
				return;
			}
			// The map has its own long press - it asks which layer is there -
			// and two answers to one press is one too many.
			const what = !mine(event.target) || event.target.closest === undefined ? null
				: event.target.closest("button[title], [role=\"separator\"][aria-label]");
			if (what === null || what === this.editor.canvas || !mine(what)) {
				return;
			}
			from = { x: event.clientX, y: event.clientY };
			waiting = setTimeout(() => this.showTip(what), LONG_PRESS_MS);
		}, { capture: true, signal: this.stopping.signal });
		root.addEventListener("pointermove", event => {
			if (waiting === null || from === null) {
				return;
			}
			if (Math.hypot(event.clientX - from.x, event.clientY - from.y) > LONG_PRESS_PIXELS) {
				drop();
			}
		}, { capture: true, signal: this.stopping.signal });
		for (const gone of ["pointerup", "pointercancel"]) {
			root.addEventListener(gone, drop, { capture: true, signal: this.stopping.signal });
		}
		// It goes away at the next touch anywhere, like a tooltip does when the
		// pointer leaves.
		root.addEventListener("click", hide, { capture: true, signal: this.stopping.signal });
	}

	/** Shows what that thing is called, beside it. */
	showTip(what) {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		if (this.tip === null) {
			this.tip = document.createElement("div");
			this.tip.className = "editor-tip";
			this.tip.dataset.role = "tip";
			this.tip.setAttribute("role", "tooltip");
			home.append(this.tip);
		}
		this.tip.textContent = what.title || what.getAttribute("aria-label") || "";
		this.tip.hidden = this.tip.textContent === "";
		if (!this.tip.hidden) {
			this.placeAt(this.tip, what);
		}
	}

	/**
	 * The drawer that is pulled in from the edge it sleeps behind.
	 *
	 * Only with a finger, and only from a strip twenty pixels wide: with a
	 * pointer there is a button for it, and a wider strip would swallow every
	 * stroke that starts near the edge of the map. The drawer follows nothing
	 * while the finger travels - it opens once the finger has gone far enough
	 * inwards that no tap could be meant.
	 */
	edgeSwipe(event, canvas) {
		if (!this.finger()) {
			return;
		}
		const box = canvas.getBoundingClientRect();
		const side = event.clientX - box.left <= EDGE_SWIPE_ZONE ? "left"
			: box.right - event.clientX <= EDGE_SWIPE_ZONE ? "right" : null;
		if (side === null || !this.drawer[side]) {
			return;
		}
		// The stroke that would otherwise have started here never does.
		event.stopPropagation();
		event.preventDefault();
		const from = event.clientX;
		const inwards = side === "left" ? 1 : -1;
		const move = moved => {
			if (moved.pointerId !== event.pointerId) {
				return;
			}
			if ((moved.clientX - from) * inwards >= EDGE_SWIPE_REACH) {
				done();
				this.showArea(side, true);
			}
		};
		const done = () => {
			canvas.removeEventListener("pointermove", move, true);
			canvas.removeEventListener("pointerup", done, true);
			canvas.removeEventListener("pointercancel", done, true);
		};
		canvas.addEventListener("pointermove", move, true);
		canvas.addEventListener("pointerup", done, true);
		canvas.addEventListener("pointercancel", done, true);
	}

	/**
	 * Makes the next tap on the map a question about the layer there.
	 *
	 * What Ctrl and the right button do on a desk. A finger has neither, and
	 * the long press is taken by the layer chooser only while the brush is
	 * empty - with a full brush a finger is allowed to stand still - so with a
	 * full brush this button is the way.
	 */
	askHere() {
		this.askingLayer = !this.askingLayer;
		this.say(this.askingLayer ? "Touch the map: which layer is there?" : "");
		this.refreshBar();
	}

	/** An answer to that question, or no answer because none was asked. */
	answerHere(spot) {
		if (!this.askingLayer) {
			return false;
		}
		this.askingLayer = false;
		this.say("");
		this.refreshBar();
		// After this press has finished being handled: the chooser shuts
		// itself on a press anywhere but in it, and the press that asked for
		// it is a press anywhere but in it.
		setTimeout(() => this.showChooser({ clientX: spot.x, clientY: spot.y }), 0);
		return true;
	}

	/**
	 * Whether targets are drawn big enough for a finger.
	 *
	 * "auto" asks the browser, which is right nearly always; "big" and
	 * "small" are for when it is not. It lives on the element beside `theme`,
	 * for the same reason: whether it is remembered between visits is the
	 * page's business, not the editor's.
	 */
	targets(next) {
		const box = this.box;
		const read = () => {
			const said = box !== null && box !== undefined
				? box.getAttribute("targets") : this.root.dataset.targets;
			return said === "big" || said === "small" ? said : "auto";
		};
		if (next === undefined) {
			return read();
		}
		if (box !== null && box !== undefined) {
			box.setAttribute("targets", next);
		} else {
			this.root.dataset.targets = next;
			this.root.dataset.big = this.finger() ? "yes" : "no";
		}
		this.refreshBar();
		return read();
	}

	/** Whether this hand is a finger, once the setting has had its say. */
	finger() {
		const said = this.targets();
		return said === "big" ? true
			: said === "small" ? false : matchMedia("(pointer: coarse)").matches;
	}

	/** Which of the two schemes the editor is drawn in. */
	scheme(next) {
		const box = this.box;
		const read = () => {
			const said = box !== null && box !== undefined ? box.getAttribute("theme") : this.root.dataset.theme;
			return said === "light" ? "light" : "dark";
		};
		if (next === undefined) {
			return read();
		}
		if (box !== null && box !== undefined) {
			box.setAttribute("theme", next);
		} else {
			this.root.dataset.theme = next;
		}
		this.refreshBar();
		return read();
	}

	/**
	 * Every command that a palette would show, with what it was asked about
	 * in front.
	 *
	 * The order is: what is called exactly that, then what starts with what
	 * was typed, then what has a word starting with it, then what merely
	 * holds it somewhere, then what only the group is called. Within a rank
	 * the order is the list's own, which is the order of the tool bar - so an
	 * empty question answers with the things one does most.
	 *
	 * The exact rank is there because one name can be the beginning of
	 * another: typing all of "Turn the brush over" would otherwise answer
	 * with "Turn the brush over sideways", which stands earlier in the list.
	 */
	findCommands(question) {
		const asked = question.trim().toLowerCase();
		const out = [];
		this.commands.forEach((command, index) => {
			if (command.palette === false) {
				return;
			}
			const label = command.label.toLowerCase();
			const group = command.group.toLowerCase();
			if (asked === "") {
				out.push({ command: command, rank: 0, index: index });
				return;
			}
			const at = label.indexOf(asked);
			let rank = -1;
			if (label === asked) {
				rank = 0;
			} else if (at === 0) {
				rank = 1;
			} else if (at > 0 && !/[a-z0-9]/.test(label[at - 1])) {
				rank = 2;
			} else if (at > 0) {
				rank = 3;
			} else if (group.includes(asked) || command.id.toLowerCase().includes(asked)) {
				rank = 4;
			}
			if (rank >= 0) {
				out.push({ command: command, rank: rank, index: index });
			}
		});
		out.sort((one, other) => one.rank - other.rank || one.index - other.index);
		return out.map(found => found.command);
	}

	/**
	 * The palette: every command there is, by its name.
	 *
	 * The rows are built once and only shown or hidden afterwards. A hundred
	 * rows built again on every keystroke would be work for nothing, and the
	 * rows never change - only which of them are the answer does.
	 */
	buildPalette() {
		const home = this.overlayHome();
		if (home === null) {
			return false;
		}
		this.palette = document.createElement("div");
		this.palette.className = "editor-palette";
		this.palette.dataset.role = "palette";
		this.palette.hidden = true;
		this.palette.setAttribute("role", "dialog");
		this.palette.setAttribute("aria-label", "Everything the editor can do");
		const listId = `${this.overId}-palette`;
		this.palette.innerHTML = `<input class="editor-palette-find" data-role="palette-find" type="text"
	role="combobox" aria-expanded="true" aria-controls="${listId}" aria-autocomplete="list"
	placeholder="What should happen?" aria-label="What should happen?" spellcheck="false">
<ul class="editor-palette-list" data-role="palette-list" role="listbox" id="${listId}"
	aria-label="Everything the editor can do"></ul>
<p class="editor-palette-none" data-role="palette-none" hidden>Nothing is called that.</p>`;
		const list = this.palette.querySelector('[data-role="palette-list"]');
		this.paletteRows = this.commands.filter(command => command.palette !== false).map((command, index) => {
			const row = document.createElement("li");
			row.className = "editor-palette-row";
			row.id = `${listId}-${index}`;
			row.dataset.command = command.id;
			row.setAttribute("role", "option");
			row.setAttribute("aria-selected", "false");
			const what = document.createElement("span");
			what.className = "editor-palette-what";
			what.textContent = command.label;
			const where = document.createElement("span");
			where.className = "editor-palette-where";
			where.textContent = command.group;
			const key = document.createElement("kbd");
			key.className = "editor-palette-key";
			key.textContent = this.keyText(command);
			row.append(what, where, key);
			// The pointer is not allowed to take the focus off the field: the
			// field is what the keyboard is talking to, and a click that
			// blurred it would close the palette before the click arrived.
			row.addEventListener("mousedown", event => event.preventDefault(), { signal: this.stopping.signal });
			row.addEventListener("click", () => this.runFromPalette(command), { signal: this.stopping.signal });
			list.append(row);
			return { command: command, row: row };
		});
		const find = this.palette.querySelector('[data-role="palette-find"]');
		find.addEventListener("input", () => this.refreshPalette(), { signal: this.stopping.signal });
		this.palette.addEventListener("keydown", event => this.onPaletteKey(event), { signal: this.stopping.signal });
		home.append(this.palette);
		return true;
	}

	/** Arrows walk the answer, Enter takes one, Escape gives up. */
	onPaletteKey(event) {
		const shown = this.paletteRows.filter(row => !row.row.hidden);
		if (event.key === "Escape") {
			event.stopPropagation();
			event.preventDefault();
			this.showPalette(false);
			return;
		}
		if (event.key === "Enter") {
			event.stopPropagation();
			event.preventDefault();
			if (shown.length > 0) {
				this.runFromPalette(shown[Math.min(this.paletteAt, shown.length - 1)].command);
			}
			return;
		}
		const step = event.key === "ArrowDown" ? 1 : (event.key === "ArrowUp" ? -1 : 0);
		if (step === 0) {
			// Anything else is typing, and typing is the field's business -
			// but not the editor's, or `G` would turn the grid on.
			event.stopPropagation();
			return;
		}
		event.stopPropagation();
		event.preventDefault();
		if (shown.length === 0) {
			return;
		}
		this.paletteAt = (this.paletteAt + step + shown.length) % shown.length;
		this.markPalette(shown);
	}

	/** Writes the shortcuts into rows that were built before the first key. */
	refreshKeys(rows) {
		for (const row of rows) {
			const key = row.row.querySelector("kbd");
			if (key !== null) {
				key.textContent = this.keyText(row.command);
			}
		}
	}

	/** Which row is the one Enter would take. */
	markPalette(shown) {
		const find = this.palette.querySelector('[data-role="palette-find"]');
		for (const row of this.paletteRows) {
			row.row.classList.remove("editor-palette-at");
			row.row.setAttribute("aria-selected", "false");
		}
		if (shown.length === 0) {
			find.removeAttribute("aria-activedescendant");
			return;
		}
		const at = shown[Math.min(this.paletteAt, shown.length - 1)].row;
		at.classList.add("editor-palette-at");
		at.setAttribute("aria-selected", "true");
		find.setAttribute("aria-activedescendant", at.id);
		at.scrollIntoView({ block: "nearest" });
	}

	/** Shows the rows that answer what was typed, and grays what cannot be done. */
	refreshPalette() {
		const question = this.palette.querySelector('[data-role="palette-find"]').value;
		const answer = new Set(this.findCommands(question).map(command => command.id));
		const order = this.findCommands(question);
		const list = this.palette.querySelector('[data-role="palette-list"]');
		this.refreshKeys(this.paletteRows);
		for (const found of order) {
			const row = this.paletteRows.find(which => which.command === found);
			list.append(row.row);
		}
		let shown = [];
		for (const row of this.paletteRows) {
			const wanted = answer.has(row.command.id);
			row.row.hidden = !wanted;
			const can = row.command.enabled === undefined || row.command.enabled(this);
			row.row.classList.toggle("editor-palette-cannot", !can);
			row.row.setAttribute("aria-disabled", can ? "false" : "true");
			if (wanted) {
				shown.push(row);
			}
		}
		shown = this.paletteRows.filter(row => !row.row.hidden);
		this.palette.querySelector('[data-role="palette-none"]').hidden = shown.length > 0;
		this.paletteAt = 0;
		this.markPalette(shown);
	}

	/** Takes a row: the palette goes away first, so that what it does is seen. */
	runFromPalette(command) {
		this.showPalette(false);
		this.run(command.id);
	}

	showPalette(on) {
		if (this.palette === null && on === true && !this.buildPalette()) {
			return;
		}
		if (this.palette === null) {
			return;
		}
		this.showMenu(false);
		this.closeContext();
		this.palette.hidden = on !== true;
		if (on !== true) {
			this.refreshBar();
			const canvas = this.editor.canvas;
			if (canvas !== null && canvas !== undefined) {
				canvas.focus();
			}
			return;
		}
		const find = this.palette.querySelector('[data-role="palette-find"]');
		find.value = "";
		this.refreshPalette();
		find.focus();
		this.refreshBar();
	}

	/**
	 * The menu, out of the same list.
	 *
	 * A command says which heading it hangs under, and a heading with a slash
	 * in it is a row that opens: `Layer/Add a layer` puts the eight kinds of
	 * layer behind one row rather than eight rows in the way of everything
	 * else.
	 */
	buildMenu() {
		const home = this.overlayHome();
		if (home === null) {
			return false;
		}
		this.menu = document.createElement("div");
		this.menu.className = "editor-menu";
		this.menu.dataset.role = "menu";
		this.menu.hidden = true;
		this.menu.setAttribute("role", "menu");
		this.menu.setAttribute("aria-label", "The menu");
		const heads = new Map();
		for (const command of this.commands) {
			if (command.menu === undefined) {
				continue;
			}
			const [head, under] = command.menu.split("/");
			if (!heads.has(head)) {
				heads.set(head, { plain: [], under: new Map() });
			}
			const at = heads.get(head);
			if (under === undefined) {
				at.plain.push(command);
			} else {
				if (!at.under.has(under)) {
					at.under.set(under, []);
				}
				at.under.get(under).push(command);
			}
		}
		const named = [...heads.keys()].sort((one, other) => {
			const a = MENU_ORDER.indexOf(one);
			const b = MENU_ORDER.indexOf(other);
			return (a < 0 ? MENU_ORDER.length : a) - (b < 0 ? MENU_ORDER.length : b);
		});
		for (const head of named) {
			const section = document.createElement("section");
			section.className = "editor-menu-part";
			const title = document.createElement("h3");
			title.className = "editor-menu-head";
			title.textContent = head;
			section.append(title);
			for (const command of heads.get(head).plain) {
				section.append(this.menuRow(command));
			}
			for (const [under, commands] of heads.get(head).under) {
				section.append(this.menuFold(under, commands));
			}
			this.menu.append(section);
		}
		this.menu.addEventListener("keydown", event => {
			if (event.key === "Escape") {
				event.stopPropagation();
				event.preventDefault();
				this.showMenu(false);
			}
		}, { signal: this.stopping.signal });
		home.append(this.menu);
		return true;
	}

	/** One command as a row of a menu. */
	menuRow(command) {
		const row = document.createElement("button");
		row.type = "button";
		row.className = "editor-menu-row";
		row.dataset.command = command.id;
		row.setAttribute("role", "menuitem");
		const what = document.createElement("span");
		what.textContent = command.label;
		const key = document.createElement("kbd");
		key.className = "editor-menu-key";
		key.textContent = this.keyText(command);
		row.append(what, key);
		row.addEventListener("click", () => {
			this.showMenu(false);
			this.closeContext();
			this.run(command.id);
		}, { signal: this.stopping.signal });
		return row;
	}

	/** A row that opens onto more rows. */
	menuFold(label, commands) {
		const holder = document.createElement("div");
		holder.className = "editor-menu-fold";
		const open = document.createElement("button");
		open.type = "button";
		open.className = "editor-menu-row editor-menu-more";
		open.setAttribute("aria-expanded", "false");
		open.innerHTML = `<span></span><span class="editor-menu-arrow" aria-hidden="true">▸</span>`;
		open.firstElementChild.textContent = label;
		const under = document.createElement("div");
		under.className = "editor-menu-under";
		under.hidden = true;
		for (const command of commands) {
			under.append(this.menuRow(command));
		}
		open.addEventListener("click", () => {
			const shown = under.hidden;
			under.hidden = !shown;
			open.setAttribute("aria-expanded", shown ? "true" : "false");
			if (shown) {
				this.refreshMenu();
			}
		}, { signal: this.stopping.signal });
		holder.append(open, under);
		return holder;
	}

	/** What can be done right now, and what is on. */
	refreshMenu() {
		if (this.menu === null) {
			return;
		}
		for (const row of this.menu.querySelectorAll("[data-command]")) {
			const command = this.commands.find(which => which.id === row.dataset.command);
			if (command === undefined) {
				continue;
			}
			const key = row.querySelector("kbd");
			if (key !== null) {
				key.textContent = this.keyText(command);
			}
			row.disabled = command.enabled !== undefined && !command.enabled(this);
			if (command.pressed !== undefined) {
				row.setAttribute("aria-checked", command.pressed(this) ? "true" : "false");
				row.setAttribute("role", "menuitemcheckbox");
			}
		}
	}

	showMenu(on) {
		if (this.menu === null && on === true && !this.buildMenu()) {
			return;
		}
		if (this.menu === null) {
			return;
		}
		if (on === true) {
			this.showPalette(false);
			this.closeContext();
			this.refreshMenu();
		}
		this.menu.hidden = on !== true;
		if (on === true) {
			this.placeAt(this.menu, this.part(commandRole(this.commands.find(which => which.id === "menu.open"))));
			const first = this.menu.querySelector("button:not(:disabled)");
			if (first !== null) {
				first.focus();
			}
		}
		this.refreshBar();
	}

	/**
	 * Puts a thing that floats under the button it belongs to, or at a spot,
	 * and keeps it inside the editor.
	 *
	 * Measured after it is shown, because a hidden box has no size and a menu
	 * placed by the size it does not have yet would hang off the edge.
	 */
	placeAt(what, anchor) {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		const room = home.getBoundingClientRect();
		const size = what.getBoundingClientRect();
		let left = room.width - size.width - 8;
		let top = 8;
		if (anchor instanceof Element) {
			const at = anchor.getBoundingClientRect();
			left = at.right - room.left - size.width;
			top = at.bottom - room.top + 4;
		} else if (anchor !== null && anchor !== undefined) {
			left = anchor.x - room.left;
			top = anchor.y - room.top;
		}
		what.style.left = `${Math.max(4, Math.min(left, room.width - size.width - 4))}px`;
		what.style.top = `${Math.max(4, Math.min(top, room.height - size.height - 4))}px`;
	}

	/**
	 * The menu of a thing: what can be done to the layer, the picture, the
	 * quad that was clicked, and nothing about what it is - that is the
	 * inspector's, and a property with two homes is a property that disagrees
	 * with itself.
	 */
	showContext(kind, at) {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		this.closeContext();
		this.showMenu(false);
		this.showPalette(false);
		const commands = this.commands.filter(command => command.for !== undefined
			&& [].concat(command.for).includes(kind)
			&& (command.enabled === undefined || command.enabled(this)));
		const layer = kind === "layer" ? this.selectedLayer() : null;
		const physics = layer !== null && layer.construct === true;
		if (commands.length === 0 && !physics) {
			return;
		}
		this.context = document.createElement("div");
		this.context.className = "editor-menu editor-context";
		this.context.dataset.role = "context";
		this.context.setAttribute("role", "menu");
		this.context.setAttribute("aria-label", `What can be done with this ${kind}`);
		for (const command of commands) {
			this.context.append(this.menuRow(command));
		}
		if (physics) {
			this.context.append(this.gameTilesFold());
		}
		this.context.addEventListener("keydown", event => {
			if (event.key === "Escape") {
				event.stopPropagation();
				event.preventDefault();
				this.closeContext();
			}
		}, { signal: this.stopping.signal });
		home.append(this.context);
		this.placeAt(this.context, at);
		const first = this.context.querySelector("button:not(:disabled)");
		if (first !== null) {
			first.focus();
		}
	}

	/** The thirteen physics tiles, behind one row. */
	gameTilesFold() {
		const holder = document.createElement("div");
		holder.className = "editor-menu-fold";
		const open = document.createElement("button");
		open.type = "button";
		open.className = "editor-menu-row editor-menu-more";
		open.dataset.role = "game-tiles";
		open.setAttribute("aria-expanded", "false");
		open.innerHTML = `<span>Physics tiles from this layer</span><span class="editor-menu-arrow" aria-hidden="true">▸</span>`;
		const under = document.createElement("div");
		under.className = "editor-menu-under";
		under.hidden = true;
		for (const [name, label] of GAME_TILES) {
			const row = document.createElement("button");
			row.type = "button";
			row.className = "editor-menu-row";
			row.dataset.tile = name;
			row.setAttribute("role", "menuitem");
			row.textContent = label;
			row.addEventListener("click", () => {
				const where = this.selection;
				this.closeContext();
				this.change(() => this.editor.apply({
					op: "layer.constructGameTiles", group: where.group, layer: where.layer, tile: name,
				}));
			}, { signal: this.stopping.signal });
			under.append(row);
		}
		open.addEventListener("click", () => {
			under.hidden = !under.hidden;
			open.setAttribute("aria-expanded", under.hidden ? "false" : "true");
		}, { signal: this.stopping.signal });
		holder.append(open, under);
		return holder;
	}

	/**
	 * A "..." on every row that has a menu of its own.
	 *
	 * The long press opens the same menu, but nobody finds a long press, and a
	 * finger has no right button. Added after the lists have been built rather
	 * than inside each of the six that build them, so that a new list is a
	 * list with a menu without anybody having to remember.
	 */
	addMoreButtons() {
		for (const [role, kind] of CONTEXT_LISTS) {
			const list = this.part(role);
			if (list === null) {
				continue;
			}
			const rows = kind === null
				? list.querySelectorAll('[data-role="layer"], [data-role="group"]')
				: list.querySelectorAll("li");
			for (const row of rows) {
				if (row.querySelector('[data-role="more"]') !== null) {
					continue;
				}
				const more = document.createElement("button");
				more.type = "button";
				more.className = "editor-more";
				more.dataset.role = "more";
				more.textContent = "\u22ef";
				more.title = "What can be done with this";
				more.setAttribute("aria-label", "What can be done with this");
				more.addEventListener("click", event => {
					event.stopPropagation();
					row.click();
					this.showContext(kind === null ? row.dataset.role : kind, more);
				}, { signal: this.stopping.signal });
				row.append(more);
			}
		}
	}

	closeContext() {
		if (this.context === null) {
			return;
		}
		this.context.remove();
		this.context = null;
	}

	/**
	 * A press beside a thing that floats puts it away.
	 *
	 * The button that opened it is left out, or a press on it would close the
	 * menu and the click that follows would open it again - and a button that
	 * does nothing when pressed twice is a button nobody trusts. The dialogue
	 * is left out too: it was asked a question and wants an answer.
	 */
	wireClickAway() {
		document.addEventListener("pointerdown", event => {
			if (this.dialog !== null) {
				return;
			}
			const path = event.composedPath();
			const inside = what => what !== null && what !== undefined && path.includes(what);
			const onButtonFor = id => path.some(node => node instanceof Element
				&& node.dataset !== undefined && node.dataset.command === id);
			if (this.context !== null && !inside(this.context)) {
				this.closeContext();
			}
			if (this.menu !== null && !this.menu.hidden && !inside(this.menu) && !onButtonFor("menu.open")) {
				this.showMenu(false);
			}
			if (this.palette !== null && !this.palette.hidden && !inside(this.palette) && !onButtonFor("palette.open")) {
				this.showPalette(false);
			}
		}, { capture: true, signal: this.stopping.signal });
	}

	/**
	 * A right-click on a row of a list opens the menu of what that row is.
	 *
	 * The row is picked first, because a menu that acted on something other
	 * than what was clicked would be a menu nobody could trust; picking is
	 * what a plain click does, so a plain click is what it is told to do.
	 */
	wireContextMenus() {
		for (const [role, kind] of CONTEXT_LISTS) {
			const list = this.part(role);
			if (list === null) {
				continue;
			}
			list.addEventListener("contextmenu", event => {
				// Ctrl and the right button together is the layer chooser's,
				// and that one is about the map, not about a list.
				const row = kind === null
					? event.target.closest('[data-role="layer"], [data-role="group"]')
					: event.target.closest("li");
				if (row === null) {
					return;
				}
				event.preventDefault();
				row.click();
				const what = kind === null ? row.dataset.role : kind;
				this.showContext(what, { x: event.clientX, y: event.clientY });
			}, { signal: this.stopping.signal });
		}
	}

	/**
	 * A dialogue: the few things that need an answer before they can happen.
	 *
	 * It is one box with a heading, some fields and two buttons, built from a
	 * list of what to ask, because a new map and a new name are the same
	 * shape and only differ in what is asked.
	 */
	askFor(title, fields, done, options) {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		this.closeDialog();
		this.dialog = document.createElement("div");
		this.dialog.className = "editor-dialog";
		this.dialog.dataset.role = "dialog";
		this.dialog.setAttribute("role", "dialog");
		this.dialog.setAttribute("aria-modal", "true");
		this.dialog.setAttribute("aria-label", title);
		const form = document.createElement("form");
		form.className = "editor-dialog-body";
		const head = document.createElement("h2");
		head.className = "editor-dialog-head";
		head.textContent = title;
		form.append(head);
		const inputs = new Map();
		for (const field of fields) {
			// A line that only says something has nothing to type into and no
			// name to hand back - a question needs saying before it is asked.
			if (field.kind === "note") {
				const note = document.createElement("p");
				note.className = "editor-dialog-note";
				note.dataset.role = `dialog-${field.name}`;
				note.textContent = field.label;
				form.append(note);
				continue;
			}
			const label = document.createElement("label");
			label.className = "editor-dialog-field";
			const name = document.createElement("span");
			name.textContent = field.label;
			let input;
			if (field.kind === "pick") {
				input = document.createElement("select");
				for (const choice of field.choices) {
					const one = document.createElement("option");
					one.value = String(choice.value);
					one.textContent = choice.label;
					input.append(one);
				}
			} else {
				input = document.createElement("input");
				input.type = field.kind === "number" ? "number" : "text";
				if (field.kind === "number") {
					input.min = String(field.min);
					input.max = String(field.max);
				}
			}
			input.dataset.role = `dialog-${field.name}`;
			input.value = String(field.value);
			label.append(name, input);
			form.append(label);
			inputs.set(field.name, input);
		}
		const row = document.createElement("div");
		row.className = "editor-dialog-buttons";
		const cancel = document.createElement("button");
		cancel.type = "button";
		cancel.className = "editor-small";
		cancel.dataset.role = "dialog-cancel";
		cancel.textContent = "Never mind";
		const go = document.createElement("button");
		go.type = "submit";
		go.className = "editor-small editor-dialog-go";
		go.dataset.role = "dialog-go";
		go.textContent = options && options.go ? options.go : title;
		row.append(cancel, go);
		form.append(row);
		this.dialog.append(form);
		cancel.addEventListener("click", () => this.closeDialog(), { signal: this.stopping.signal });
		form.addEventListener("submit", event => {
			event.preventDefault();
			const answer = {};
			for (const [name, input] of inputs) {
				answer[name] = input.type === "number" ? Number(input.value) : input.value;
			}
			this.closeDialog();
			done(answer);
		}, { signal: this.stopping.signal });
		this.dialog.addEventListener("keydown", event => {
			event.stopPropagation();
			if (event.key === "Escape") {
				event.preventDefault();
				this.closeDialog();
			}
		}, { signal: this.stopping.signal });
		home.append(this.dialog);
		const first = this.dialog.querySelector("input");
		if (first !== null) {
			first.focus();
			first.select();
		}
	}

	closeDialog() {
		if (this.dialog === null) {
			return;
		}
		this.dialog.remove();
		this.dialog = null;
		const canvas = this.editor.canvas;
		if (canvas !== null && canvas !== undefined) {
			canvas.focus();
		}
	}

	/** How big, and called what. */
	askNewMap() {
		this.askFor("New map", [
			{ name: "name", label: "Name", kind: "text", value: "untitled" },
			{ name: "width", label: "Tiles across", kind: "number", value: 100, min: 2, max: 1000 },
			{ name: "height", label: "Tiles down", kind: "number", value: 50, min: 2, max: 1000 },
		], answer => {
			this.editor.create(Math.max(2, answer.width), Math.max(2, answer.height), answer.name || "untitled");
			this.refresh();
		});
	}

	/**
	 * A picture of the whole map, handed out as a PNG.
	 *
	 * A large map is hundreds of pieces and takes seconds; the plan's rule for
	 * anything longer than a second is that it says so over the map while it
	 * runs. So a note stands and counts, and goes when the picture is done -
	 * which the program then says itself.
	 */
	exportPicture() {
		if (!this.editor.picture()) {
			this.say("A picture is already being made", "error");
			return false;
		}
		const started = performance.now();
		let note = null;
		const watch = setInterval(() => {
			const state = this.editor.pictureState();
			if (state !== 1) {
				clearInterval(watch);
				if (note !== null) {
					note.remove();
				}
				return;
			}
			if (performance.now() - started < 1000) {
				return;
			}
			const percent = Math.round(this.editor.pictureProgress() * 100);
			if (note === null) {
				note = this.tell("Drawing the picture", "progress");
			}
			if (note !== null) {
				note.firstElementChild.textContent = `Drawing the picture \u2026 ${percent} %`;
			}
		}, 250);
		this.stopping.signal.addEventListener("abort", () => clearInterval(watch), { once: true });
		return true;
	}

	/**
	 * The outer ring of the layer, drawn with what is in hand.
	 *
	 * One entry in the history for the whole ring: the brush is stamped
	 * along the four edges inside one transaction, and each stamp joins it.
	 * A brush bigger than one tile steps by its own size, so the ring is the
	 * brush's width thick and nothing is stamped twice.
	 */
	makeBorder() {
		const layer = this.selectedLayer();
		if (layer === null || layer.type !== "tiles" || layer.size === undefined) {
			this.say("Pick a tile layer to put a border round", "error");
			return false;
		}
		const brush = this.editor.brushSize();
		if (brush === null || brush.width === 0 || brush.height === 0) {
			this.say("Nothing in hand to draw the border with", "error");
			return false;
		}
		const [width, height] = layer.size;
		const where = this.selection;
		const spots = new Set();
		const stamp = (x, y) => spots.add(`${Math.max(0, Math.min(width - brush.width, x))},${Math.max(0, Math.min(height - brush.height, y))}`);
		for (let x = 0; x < width; x += brush.width) {
			stamp(x, 0);
			stamp(x, height - brush.height);
		}
		for (let y = 0; y < height; y += brush.height) {
			stamp(0, y);
			stamp(width - brush.width, y);
		}
		this.change(() => {
			this.editor.begin("Border");
			for (const spot of spots) {
				const [x, y] = spot.split(",").map(Number);
				this.editor.paint(where.group, where.layer, x, y);
			}
			this.editor.commit();
		});
		this.say(`A border round ${layer.name || "the layer"}`);
		return true;
	}

	/** Takes out every envelope that nothing is bound to. */
	deleteUnusedEnvelopes() {
		const answer = this.change(() => this.editor.apply({ op: "envelope.deleteUnused" }));
		if (answer && answer.ok) {
			const count = answer.envelopes || 0;
			this.say(`${count} ${count === 1 ? "envelope" : "envelopes"} taken out`);
		}
		return answer;
	}

	/**
	 * Every key the editor answers to, on one sheet.
	 *
	 * Grouped the way the palette groups them, because that is the order they
	 * are in everywhere else. It is a dialogue and not a panel because it is
	 * looked at once and then closed - and because a sheet that covered the
	 * map while one worked would be the wrong shape of help.
	 */
	showKeys() {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		this.closeDialog();
		this.dialog = document.createElement("div");
		this.dialog.className = "editor-dialog editor-dialog-wide";
		this.dialog.dataset.role = "dialog";
		this.dialog.setAttribute("role", "dialog");
		this.dialog.setAttribute("aria-modal", "true");
		this.dialog.setAttribute("aria-label", "What the keys do");
		const form = document.createElement("form");
		form.className = "editor-dialog-body";
		const head = document.createElement("h2");
		head.className = "editor-dialog-head";
		head.textContent = "What the keys do";
		form.append(head);
		const sheet = document.createElement("div");
		sheet.className = "editor-keys";
		sheet.dataset.role = "keys";
		const groups = new Map();
		for (const command of this.commands) {
			if (command.keys === undefined || command.keys.length === 0) {
				continue;
			}
			if (!groups.has(command.group)) {
				groups.set(command.group, []);
			}
			groups.get(command.group).push(command);
		}
		for (const [group, commands] of groups) {
			const where = document.createElement("h3");
			where.className = "editor-keys-group";
			where.textContent = group;
			sheet.append(where);
			const list = document.createElement("dl");
			list.className = "editor-keys-list";
			for (const command of commands) {
				const what = document.createElement("dt");
				what.textContent = command.label;
				const key = document.createElement("dd");
				// Every key it answers to, not only the first: the tool bar
				// has room for one and a sheet has room for all of them.
				key.textContent = command.keys.map(keyLabel).join(" or ");
				list.append(what, key);
			}
			sheet.append(list);
		}
		form.append(sheet);
		const row = document.createElement("div");
		row.className = "editor-dialog-buttons";
		const go = document.createElement("button");
		go.type = "submit";
		go.className = "editor-small editor-dialog-go";
		go.dataset.role = "dialog-go";
		go.textContent = "Done";
		row.append(go);
		form.append(row);
		this.dialog.append(form);
		form.addEventListener("submit", event => {
			event.preventDefault();
			this.closeDialog();
		}, { signal: this.stopping.signal });
		home.append(this.dialog);
		go.focus();
	}

	/** Which entities sheet, chosen from the ones there are. */
	askEntitiesImage() {
		const names = { ddnet: "DDNet", ddrace: "DDRace", race: "Race", fng: "FNG", vanilla: "Vanilla", "f-ddrace": "F-DDrace", blockworlds: "Blockworlds" };
		this.askFor("Entities picture", [
			{ name: "what", kind: "note", label: "What physics layers are drawn with - the map is the same whichever it is." },
			{ name: "sheet", label: "Picture", kind: "pick", value: this.editor.entitiesImage(),
				choices: Object.entries(names).map(([value, label]) => ({ value, label })) },
		], answer => {
			this.editor.entitiesImage(answer.sheet);
			this.tilesetSource = undefined;
			this.refresh();
			this.say(`Physics layers drawn with ${names[answer.sheet] || answer.sheet}`);
		}, { go: "Use it" });
	}

	/**
	 * Asks a question whose answer is yes or no.
	 *
	 * The browser has `confirm()`, and it stops the whole page dead while it
	 * is up - which in a program that keeps drawing means the map stops with
	 * it. This one is the editor's own dialogue, in the editor's own colours,
	 * and the frame goes on.
	 */
	askYesNo(title, text, yes, done) {
		this.askFor(title, [{ name: "what", label: text, kind: "note" }],
			() => done(), { go: yes });
	}

	/** One of the maps that lie in this browser's storage. */
	askOpenSaved() {
		const saved = this.editor.saved();
		if (saved.length === 0) {
			this.say("Nothing has been saved in this browser yet", "error");
			return;
		}
		const size = bytes => bytes < 1024 ? `${bytes} B`
			: bytes < 1024 * 1024 ? `${Math.round(bytes / 1024)} KiB`
				: `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
		this.askFor("Open from this browser", [{
			name: "name", label: "Map", kind: "pick", value: saved[0].name,
			choices: saved.map(one => ({ value: one.name, label: `${one.name} (${size(one.size)})` })),
		}], answer => {
			if (this.editor.openSaved(answer.name) < 0) {
				this.say("That map could not be read", "error");
				return;
			}
			this.refresh();
			this.refreshMaps();
		}, { go: "Open" });
	}

	/** Another file with the same map in it; this map stays this map. */
	askSaveCopy() {
		this.askFor("Save a copy", [
			{ name: "name", label: "Called", kind: "text", value: `${this.editor.name() || "untitled"} copy` },
		], answer => {
			const name = (answer.name || "").trim();
			if (name === "") {
				this.say("A copy needs a name", "error");
				return;
			}
			this.say(this.editor.saveCopy(undefined, name) ? `Copy saved as ${name}` : "That copy was refused",
				this.editor.dirty() ? "note" : "note");
			this.refreshMaps();
		}, { go: "Save a copy" });
	}

	/** Called what, from now on - the name is the name of the file. */
	askSaveAs() {
		this.askFor("Save as", [
			{ name: "name", label: "Name", kind: "text", value: this.editor.name() },
		], answer => {
			const name = (answer.name || "").trim();
			if (name === "") {
				return;
			}
			this.editor.rename(undefined, name);
			this.editor.save();
			this.refresh();
		});
	}

	/**
	 * What holds the panels: the one column while they stand in one, and the
	 * box they were spread into once they are.
	 */
	get element() {
		return this.box === null || this.box === undefined ? this.root : this.box;
	}

	part(role) {
		const which = `[data-role="${role}"]`;
		const here = this.root.querySelector(which);
		if (here !== null || this.areas === null) {
			return here;
		}
		for (const area of Object.values(this.areas)) {
			const found = area.querySelector(which);
			if (found !== null) {
				return found;
			}
		}
		// And last the box itself: the strip of open maps hangs in the header,
		// which is the page's area rather than one of the six.
		return this.box === null || this.box === undefined ? null : this.box.querySelector(which);
	}

	/**
	 * The tool bar, out of the list of commands. Nothing here knows what any
	 * of the buttons do; a button is a command that said it wanted one.
	 */
	buildBar() {
		const bar = this.root.querySelector('[data-role="bar"]');
		const status = bar.firstElementChild;
		for (const command of this.commands) {
			if (command.bar !== true) {
				continue;
			}
			const button = document.createElement("button");
			button.type = "button";
			button.className = "editor-button";
			button.dataset.role = commandRole(command);
			button.dataset.command = command.id;
			button.dataset.icon = command.icon;
			button.title = commandTitle(command, this.keysShown());
			button.setAttribute("aria-label", command.label);
			if (command.pressed !== undefined) {
				button.setAttribute("aria-pressed", "false");
			}
			// The name beside the icon, for the shapes of box that have room
			// for words. Only the modes carry one: they are the four that are
			// a choice rather than an action, and a choice wants a name.
			if (command.text === true) {
				const name = document.createElement("span");
				name.className = "editor-button-text";
				name.textContent = command.label;
				button.append(name);
			}
			button.addEventListener("click", () => this.run(command.id), { signal: this.stopping.signal });
			bar.insertBefore(button, status);
		}
	}

	/** Does one of the commands, by name, if it can be done at all. */
	run(id) {
		const command = this.commands.find(which => which.id === id);
		if (command === undefined || (command.enabled !== undefined && !command.enabled(this))) {
			return false;
		}
		// One place says no, rather than a hundred commands each remembering
		// to ask: an editor that is only to be looked at does the things that
		// are about looking and none of the rest.
		if (this.readonly && command.safe !== true) {
			return false;
		}
		command.run(this);
		// A command that moves the view moves nothing else: the program draws
		// the next frame by itself, but the marks over the map and the line at
		// the bottom are drawn here and would keep saying the old numbers.
		if (command.group === "View") {
			this.refreshOverlay();
		}
		return true;
	}

	/** Every part of that name, wherever the panels stand. */
	parts(role) {
		const which = `[data-role="${role}"]`;
		const found = [...this.root.querySelectorAll(which)];
		if (this.areas !== null) {
			for (const area of Object.values(this.areas)) {
				found.push(...area.querySelectorAll(which));
			}
		}
		if (found.length === 0 && this.box !== null && this.box !== undefined) {
			found.push(...this.box.querySelectorAll(which));
		}
		return found;
	}

	/**
	 * Moves the panels out of the one column and into the areas of a box -
	 * the tree and the pictures to the left, the inspector to the right, the
	 * envelopes and the history below, the bar above and the status line at
	 * the bottom. Nothing about a panel changes; only where it stands.
	 */
	spread(areas, box) {
		// Taken before anything moves: `part` looks in the column, and the
		// column is about to be empty.
		const bar = this.part("bar");
		const status = this.part("status");
		const hover = this.part("hover");
		const panels = new Map(PANEL_PLACES.map(place => [place.role, this.part(place.role)]));
		this.areas = areas;
		// What now holds every panel, for whoever asks the panels where they
		// are: with the panels spread over six areas there is no one node that
		// is "the panels" any more - the box is.
		this.box = box === undefined ? null : box;
		areas.toolbar.append(bar);
		// The status line: what is under the pointer on the left, what is being
		// worked in next to it, and what just happened on the right.
		const line = document.createElement("div");
		line.className = "editor-statusline";
		line.innerHTML = '<span data-role="status-layer"></span><span data-role="status-brush"></span><span data-role="status-zoom"></span>';
		areas.status.append(hover, line, status);
		for (const area of ["left", "right", "dock"]) {
			const here = PANEL_PLACES.filter(place => place.area === area);
			if (TABBED_AREAS[area] !== undefined) {
				areas[area].append(this.makeTabs(area, here));
			}
			const body = document.createElement("div");
			body.className = "editor-area-body";
			for (const place of here) {
				const panel = panels.get(place.role);
				if (panel !== null && panel !== undefined) {
					body.append(panel);
				}
			}
			areas[area].append(body);
		}
		for (const area of ["left", "right", "dock"]) {
			areas[area].append(this.makeGrip(area));
		}
		this.applyTabs();
		if (this.box !== null) {
			this.buildMaps(this.box);
		}
	}

	/**
	 * The handle between a column and the map, and above the dock.
	 *
	 * A column has a width that suits most maps; some maps and some people want
	 * another one, and the plan says so. It is a separator rather than a
	 * decoration, so the arrow keys move it as well - a handle only a pointer
	 * can reach is a handle half the people cannot use.
	 */
	makeGrip(area) {
		const limits = DRAG_LIMITS[area];
		const sideways = area !== "dock";
		const grip = document.createElement("div");
		grip.className = `editor-grip editor-grip-${area}`;
		grip.dataset.role = `grip-${area}`;
		grip.tabIndex = 0;
		grip.setAttribute("role", "separator");
		grip.setAttribute("aria-orientation", sideways ? "vertical" : "horizontal");
		grip.setAttribute("aria-label", `How wide the ${area} is`);
		const now = () => {
			const box = this.areas[area].getBoundingClientRect();
			return Math.round(sideways ? box.width : box.height);
		};
		const put = size => {
			const want = Math.max(limits.least, Math.min(limits.most, Math.round(size)));
			this.dragged[area] = want;
			this.applyDragged();
			grip.setAttribute("aria-valuenow", String(want));
			grip.setAttribute("aria-valuemin", String(limits.least));
			grip.setAttribute("aria-valuemax", String(limits.most));
		};
		let from = null;
		grip.addEventListener("pointerdown", event => {
			from = { at: sideways ? event.clientX : event.clientY, was: now() };
			grip.setPointerCapture(event.pointerId);
			event.preventDefault();
		}, { signal: this.stopping.signal });
		grip.addEventListener("pointermove", event => {
			if (from === null) {
				return;
			}
			// Which way is bigger depends on which edge the handle is on: the
			// right column grows leftwards and the dock grows upwards.
			const went = (sideways ? event.clientX : event.clientY) - from.at;
			put(from.was + (area === "left" ? went : -went));
		}, { signal: this.stopping.signal });
		const letGo = event => {
			from = null;
			try {
				grip.releasePointerCapture(event.pointerId);
			} catch (error) {
				// It had already gone; there is nothing to let go of.
			}
		};
		grip.addEventListener("pointerup", letGo, { signal: this.stopping.signal });
		grip.addEventListener("pointercancel", letGo, { signal: this.stopping.signal });
		grip.addEventListener("keydown", event => {
			const step = { ArrowLeft: -16, ArrowRight: 16, ArrowUp: -16, ArrowDown: 16 }[event.key];
			if (step === undefined) {
				if (event.key === "Home") {
					event.preventDefault();
					event.stopPropagation();
					this.dragged[area] = null;
					this.applyDragged();
				}
				return;
			}
			event.preventDefault();
			event.stopPropagation();
			put(now() + (area === "left" ? step : -step));
		}, { signal: this.stopping.signal });
		// Two presses put it back where it was: the same as Home, for a pointer.
		grip.addEventListener("dblclick", () => {
			this.dragged[area] = null;
			this.applyDragged();
		}, { signal: this.stopping.signal });
		return grip;
	}

	/**
	 * Puts the dragged sizes on, where this shape of box has a size to put
	 * them on at all.
	 *
	 * A drawer lies over the map at a width of its own and a shut dock is its
	 * names and nothing else; in both the stylesheet is right and a number
	 * somebody dragged a while ago is not.
	 */
	applyDragged() {
		if (this.areas === null) {
			return;
		}
		for (const area of ["left", "right", "dock"]) {
			const box = this.areas[area];
			if (box === undefined) {
				continue;
			}
			const column = area === "dock"
				? this.dockOpen
				: this.shape === null || this.shape[area] === "column";
			const size = this.dragged[area];
			if (!column || size === null) {
				box.style.width = "";
				box.style.height = "";
				continue;
			}
			if (area === "dock") {
				box.style.height = `${size}px`;
			} else {
				box.style.width = `${size}px`;
			}
		}
	}

	// The strip of names above an area that shows one panel at a time.
	makeTabs(area, places) {
		const strip = document.createElement("div");
		strip.className = "editor-tabs";
		strip.dataset.role = `${TABBED_AREAS[area]}-tabs`;
		strip.setAttribute("role", "tablist");
		for (const place of places) {
			if (place.tab === undefined) {
				continue;
			}
			const button = document.createElement("button");
			button.type = "button";
			button.className = "editor-tab";
			button.dataset.role = `${TABBED_AREAS[area]}-tab`;
			button.dataset.tab = place.tab;
			button.setAttribute("role", "tab");
			button.textContent = place.name;
			button.addEventListener("click", () => this.showTab(area, place.tab), { signal: this.stopping.signal });
			strip.append(button);
		}
		if (area === "dock") {
			const toggle = document.createElement("button");
			toggle.type = "button";
			toggle.className = "editor-tab editor-dock-toggle";
			toggle.dataset.role = "dock-toggle";
			toggle.title = "Open or close the strip at the bottom";
			toggle.addEventListener("click", () => {
				this.dockOpen = !this.dockOpen;
				this.applyTabs();
			}, { signal: this.stopping.signal });
			strip.append(toggle);
		}
		return strip;
	}

	/**
	 * The two sides of the tile panel: the tileset one paints with, and the
	 * rules that paint by themselves. They are one panel with two tabs rather
	 * than two panels, because both are about the same layer and only one of
	 * them is wanted at a time.
	 */
	applyTilesTab() {
		const there = this.automapThere === true;
		if (!there && this.tab.tiles === "automap") {
			this.tab.tiles = "tiles";
		}
		// The same with the inspector's own two: with room they stand above
		// each other, and the strip that chose between them goes away.
		const roomy = this.shape !== null && this.shape.stack === true;
		const body = this.part("tiles-body");
		if (body !== null) {
			body.hidden = !roomy && this.tab.tiles !== "tiles";
		}
		const automap = this.part("automap");
		if (automap !== null) {
			automap.hidden = !there || (!roomy && this.tab.tiles !== "automap");
		}
		for (const button of this.parts("tiles-tab")) {
			button.hidden = roomy;
			button.disabled = button.dataset.tab === "automap" && !there;
			button.setAttribute("aria-selected", button.dataset.tab === this.tab.tiles ? "true" : "false");
		}
	}

	/** Puts one of an area's panels in front, and opens the area if it was shut. */
	showTab(area, tab) {
		this.tab[area] = tab;
		if (area === "dock") {
			this.dockOpen = true;
		}
		this.applyTabs();
	}

	/**
	 * Which panel each tabbed area shows. A panel with nothing to show - no
	 * map, a layer whose picture has no rules - makes its tab grey rather than
	 * taking it away, so that the strip does not change shape underfoot; and
	 * if the one in front is that panel, the area falls back to the first that
	 * has something.
	 */
	applyTabs() {
		if (this.areas === null) {
			return;
		}
		for (const area of Object.keys(TABBED_AREAS)) {
			const here = PANEL_PLACES.filter(place => place.area === area && place.tab !== undefined);
			const has = place => this.panelShown.get(place.role) !== false;
			if (!here.some(place => place.tab === this.tab[area] && has(place))) {
				const first = here.find(has);
				this.tab[area] = first === undefined ? null : first.tab;
			}
			// With room enough, the map's parts stand above each other instead
			// of behind each other: the layer tree is always there, and one
			// of the other three is under it. A tab that is always in front
			// is not a tab, so it leaves the strip.
			const roomy = this.shape !== null && this.shape.stack === true;
			const always = !roomy ? null
				: area === "left" ? "layers"
					: area === "dock" ? "envelopes" : null;
			if (always !== null && this.tab[area] === always) {
				const next = here.find(place => place.tab !== always && has(place));
				this.tab[area] = next === undefined ? always : next.tab;
			}
			for (const place of here) {
				const panel = this.part(place.role);
				const shown = has(place) && (place.tab === this.tab[area] || place.tab === always);
				if (panel !== null) {
					panel.hidden = !shown;
					panel.classList.toggle("editor-panel-always", place.tab === always);
				}
				const button = this.areas[area].querySelector(`[data-tab="${place.tab}"]`);
				if (button !== null) {
					button.hidden = place.tab === always;
					button.disabled = !has(place);
					button.setAttribute("aria-selected", place.tab === this.tab[area] ? "true" : "false");
				}
			}
		}
		const dock = this.areas.dock;
		if (dock !== undefined) {
			dock.classList.toggle("editor-dock-shut", !this.dockOpen);
			this.applyDragged();
			const toggle = dock.querySelector('[data-role="dock-toggle"]');
			if (toggle !== null) {
				toggle.setAttribute("aria-expanded", this.dockOpen ? "true" : "false");
				toggle.textContent = this.dockOpen ? "\u25be" : "\u25b4";
			}
		}
	}

	/**
	 * Whether a panel has anything to show. Said here rather than by setting
	 * `hidden` straight away, because in a tabbed area the tab has a say too.
	 */
	showPanel(role, show) {
		this.panelShown.set(role, show);
		const panel = this.part(role);
		if (panel !== null) {
			panel.hidden = !show || !this.tabInFront(role);
		}
	}

	// Whether the tab a panel stands under is the one in front. A panel in an
	// area without tabs is always in front.
	tabInFront(role) {
		if (this.areas === null) {
			return true;
		}
		const place = PANEL_PLACES.find(where => where.role === role);
		if (place === undefined || place.tab === undefined) {
			return true;
		}
		return this.tab[place.area] === place.tab;
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
		for (const floating of [this.picker, this.chooser, this.zoomChip, this.over]) {
			if (floating !== null && floating !== undefined) {
				floating.remove();
			}
		}
		this.picker = null;
		this.chooser = null;
		this.over = null;
		this.palette = null;
		this.menu = null;
		this.context = null;
		this.dialog = null;
		this.root.remove();
	}

	wire() {
		const signal = this.stopping.signal;
		const on = (role, handler) => this.part(role).addEventListener("click", handler, { signal: signal });
		this.wireArt();
		this.wireType();
		on("add-group", () => this.run("layer.addGroup"));
		on("add-layer", () => this.run("layer.addTiles"));
		on("add-quads", () => this.run("layer.addQuads"));
		on("flip-x", () => this.run("brush.flipX"));
		on("flip-y", () => this.run("brush.flipY"));
		on("rotate", () => this.run("brush.rotate"));
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
		this.wireContextMenus();
		this.wireClickAway();
		this.wireDrawers();
		this.wireTips();
		on("delete", () => this.run("layer.delete"));
		on("up", () => this.run("layer.up"));
		on("down", () => this.run("layer.down"));

		// The program says when the map changed; nothing here asks it in a
		// loop the way the viewer's buttons do, because an editor calls.
		for (const type of ["document", "loaded", "closed", "saved", "copied", "picture", "unused", "error"]) {
			this.editor.addEventListener(type, event => this.onProgram(type, event.detail), { signal: signal });
		}
		// Whoever put the panels on the page may hand out the keyboard
		// themselves - `<ddnet-editor>` does, so that two editors on one page
		// do not both answer to the same key.
		if (this.keys) {
			document.addEventListener("keydown", event => this.onKey(event), { signal: signal });
			document.addEventListener("keyup", event => this.onKeyUp(event), { signal: signal });
		}
	}

	onProgram(type, detail) {
		if (type === "saved") {
			this.say("Saved");
		} else if (type === "unused") {
			// Once every few seconds at most: a stroke across a hundred tiles
			// is one thing to say, not a hundred.
			const now = performance.now();
			if (now - this.unusedSaidAt > TOAST_MS) {
				this.unusedSaidAt = now;
				const count = detail && detail.tiles ? detail.tiles : 0;
				this.say(`${count === 1 ? "A tile does" : "Some tiles do"} nothing in this layer and went down as air - Settings, "Allow unused tiles"`);
			}
			return;
		} else if (type === "picture") {
			this.say("The picture is in your downloads");
		} else if (type === "error") {
			this.say(`Failed: ${detail && detail.what ? detail.what : "something"}`, "error");
		} else if (type === "loaded") {
			this.selection = { group: 0, layer: -1 };
			this.collapsed.clear();
		}
		this.refresh();
	}

	// The keyboard belongs to whoever has the focus: a name being typed into
	// a field is not an undo, whatever letters are in it.
	onKey(event) {
		// Whoever struck it has a keyboard, and from now on the tooltips and
		// the palette are allowed to name the shortcuts. Before that they are
		// noise on a tablet: a key nobody can press is not a hint.
		this.sawKey = true;
		const target = event.target;
		if (target && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.isContentEditable)) {
			// Escape is the way out of a field, and the only key a field
			// hands on.
			if (event.key !== "Escape") {
				return;
			}
			target.blur();
		}
		const command = this.keys_.get(keyName(event));
		if (command === undefined) {
			return;
		}
		// A command that only belongs to the map leaves the key alone
		// everywhere else - Tab walks through the buttons there.
		if (command.where === "map" && event.target !== this.editor.canvas) {
			return;
		}
		// A command that cannot be done now still takes the key: a disabled
		// Ctrl+S must not reach the browser's own save dialogue.
		event.preventDefault();
		this.run(command.id);
	}

	/**
	 * The one key that means something while it is held rather than when it
	 * is struck: the big tile chooser is open for as long as the space bar is
	 * down, unless somebody pinned it with Ctrl and space.
	 */
	onKeyUp(event) {
		if (keyName(event) !== "Space" || this.pickerPinned) {
			return;
		}
		const target = event.target;
		if (target && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.isContentEditable)) {
			return;
		}
		this.showPicker(false);
	}

	/**
	 * Opens the file dialogue for a map. The element around the panels keeps
	 * the input, because a page may have put the panels somewhere without one.
	 */
	openMap() {
		if (this.box !== null && this.box !== undefined && typeof this.box.openFile === "function") {
			this.box.openFile();
			return;
		}
		this.say("This page opens maps its own way", "error");
	}

	/** The layer before or after the one that is selected, over all groups. */
	stepSelection(step) {
		if (this.map === null) {
			return;
		}
		const all = [];
		this.map.groups.forEach((group, index) => {
			all.push({ group: index, layer: -1 });
			group.layers.forEach((layer, which) => all.push({ group: index, layer: which }));
		});
		const now = all.findIndex(where => where.group === this.selection.group && where.layer === this.selection.layer);
		const next = all[Math.min(all.length - 1, Math.max(0, (now < 0 ? 0 : now) + step))];
		if (next !== undefined) {
			this.selection = next;
			this.refresh();
		}
	}

	/**
	 * Takes away whatever is picked - a quad, a sound source, a line of the
	 * server settings. The layer itself has a key of its own, because losing a
	 * layer to a stray Delete is a bad afternoon.
	 */
	deletePicked() {
		const layer = this.selectedLayer();
		const kind = layer === null ? null : layer.type;
		if (kind === "quads" && this.quad >= 0) {
			this.part("delete-quad").click();
		} else if (kind === "sounds" && this.source >= 0) {
			this.part("delete-source").click();
		} else if (this.setting >= 0 && this.tab.dock === "settings") {
			this.part("delete-setting").click();
		} else {
			this.say("Nothing picked - Ctrl+Delete takes the layer");
		}
	}

	/**
	 * One step back out of whatever is open: a sub-mode first, then the
	 * focus, which always ends up on the map.
	 */
	escape() {
		if (this.dialog !== null) {
			this.closeDialog();
			return;
		}
		if (this.context !== null) {
			this.closeContext();
			return;
		}
		if (this.menu !== null && !this.menu.hidden) {
			this.showMenu(false);
			return;
		}
		if (this.palette !== null && !this.palette.hidden) {
			this.showPalette(false);
			return;
		}
		if (this.carving !== null) {
			this.knife();
			return;
		}
		const canvas = this.editor.canvas;
		if (canvas !== null && canvas !== undefined) {
			canvas.focus();
		}
	}

	/** Whether one of the areas beside the map is shown. */
	areaShown(area) {
		return this.areas !== null && !this.areas[area].hidden;
	}

	showArea(area, on) {
		if (this.areas === null) {
			return;
		}
		this.areas[area].hidden = !on;
		this.refreshBar();
	}


	/**
	 * A word about what just happened, over the map.
	 *
	 * The status line is where one looks for it afterwards; a note over the
	 * map is what one sees without looking, and the two say the same thing.
	 * A note goes away by itself after four seconds - except one about
	 * something that went wrong, which stays until it is dismissed, because a
	 * mistake that vanished before it was read is a mistake nobody knows about.
	 */
	tell(text, kind) {
		const home = this.floatHome === null || this.floatHome === undefined
			? this.overlayHome() : this.floatHome;
		if (home === null || !text) {
			return null;
		}
		if (this.toasts === null) {
			this.toasts = document.createElement("div");
			this.toasts.className = "editor-toasts";
			this.toasts.dataset.role = "toasts";
			home.append(this.toasts);
		}
		const note = document.createElement("div");
		note.className = "editor-toast";
		note.dataset.role = "toast";
		note.dataset.kind = kind === "error" || kind === "progress" ? kind : "note";
		// What went wrong interrupts; what merely happened does not.
		note.setAttribute("role", kind === "error" ? "alert" : "status");
		const what = document.createElement("span");
		what.textContent = text;
		note.append(what);
		if (kind === "error") {
			const away = document.createElement("button");
			away.type = "button";
			away.className = "editor-toast-close";
			away.dataset.role = "toast-close";
			away.textContent = "\u00d7";
			away.setAttribute("aria-label", "Dismiss");
			away.addEventListener("click", () => note.remove(), { signal: this.stopping.signal });
			note.append(away);
		} else if (kind !== "progress") {
			// Work that is still going on says so until it is over; whoever
			// started it takes the note away.
			setTimeout(() => note.remove(), TOAST_MS);
		}
		this.toasts.append(note);
		// Never more than a handful: a stack that grew without end would cover
		// the map it is telling about.
		while (this.toasts.children.length > TOAST_MOST) {
			// A note would have gone by itself in a moment anyway; something
			// that went wrong is still waiting to be read.
			const oldest = this.toasts.querySelector('[data-kind="note"]')
				|| this.toasts.firstElementChild;
			oldest.remove();
		}
		return note;
	}

	say(text, kind) {
		this.part("status").textContent = text || "";
		this.tell(text, kind);
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
			this.say(answer.error || "Refused", "error");
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
		// A map can come to the front without anybody going through `showMap`:
		// making one and opening one both do it. At this moment what the page
		// knows is still about the map that *was* in front, so this is where
		// it is put away - a moment later `clampSelection` will have moved the
		// selection to fit the new map and it would be gone.
		const inFront = this.editor.map;
		if (this.lastMap !== inFront) {
			if (this.lastMap !== null && this.lastMap >= 0 && this.editor.maps.includes(this.lastMap)) {
				this.rememberMap(this.lastMap);
			}
			this.lastMap = inFront;
		}
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
		this.refreshStatus();
		// Which panel each tabbed area shows can only be answered once every
		// panel has said whether it has anything to show.
		this.applyTabs();
		// And the rows exist now, so they can be given their menus.
		this.addMoreButtons();
		this.refreshMaps();
	}

	/**
	 * What each open map is costing, in the panel that is about the map.
	 *
	 * Every map keeps its own history, and a history keeps whole versions of
	 * the map - so a second map open is a second map's worth of memory, and
	 * somebody who opened four should be able to see that without guessing.
	 */
	refreshMemory() {
		const list = this.part("memory");
		if (list === null) {
			return;
		}
		list.textContent = "";
		const now = this.editor.map;
		for (const id of this.editor.maps) {
			const history = this.editor.history(id);
			const row = document.createElement("li");
			row.className = "editor-memory-row";
			row.dataset.role = "memory-row";
			row.dataset.map = String(id);
			if (id === now) {
				row.classList.add("editor-selected");
			}
			const name = document.createElement("span");
			name.textContent = this.editor.name(id) || "untitled";
			const size = document.createElement("span");
			size.className = "editor-memory-size";
			size.dataset.role = "memory-size";
			size.textContent = history === null ? "" : `${(history.bytes / (1024 * 1024)).toFixed(1)} MiB`;
			row.append(name, size);
			if (history !== null) {
				row.title = `${history.entries.length} steps, of ${(history.maxBytes / (1024 * 1024)).toFixed(0)} MiB allowed`;
			}
			list.append(row);
		}
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

	/** What the line along the bottom says about where the work is. */
	refreshStatus() {
		const say = (role, text) => {
			const part = this.part(role);
			if (part !== null) {
				part.textContent = text;
			}
		};
		const layer = this.selectedLayer();
		const where = this.selection;
		const group = this.map === null || where.group >= this.map.groups.length ? null : this.map.groups[where.group];
		const groupName = group === null ? "" : (group.name || `Group ${where.group}`);
		say("status-layer", layer === null ? groupName : `${groupName} \u203a ${layer.name || layer.kind}`);
		const size = this.editor.brushSize();
		say("status-brush", size === null || size.width === 0 ? "" : `Brush ${size.width} \u00d7 ${size.height}`);
		const zoom = this.editor.zoom();
		const percent = zoom === null ? "" : `${Math.round(100 / zoom)} %`;
		say("status-zoom", percent);
		if (this.zoomChip !== null && this.zoomChip !== undefined) {
			this.zoomChip.querySelector('[data-role="zoom-level"]').textContent = percent;
		}
	}

	/**
	 * Whether a shortcut belongs beside a name.
	 *
	 * With a pointer, yes: there is a keyboard beside it. With a finger, only
	 * once one has been used - an iPad shows `Ctrl+Z` to nobody.
	 */
	keysShown() {
		return this.sawKey || !this.finger();
	}

	/** The shortcut of a command as it should be written here, if at all. */
	keyText(command) {
		return !this.keysShown() || command.keys === undefined || command.keys.length === 0
			? "" : keyLabel(command.keys[0]);
	}

	/** The tool bar's button for a command, by the command's own name. */
	barButton(command) {
		const bar = this.part("bar");
		return bar === null ? null : bar.querySelector(`[data-command="${command.id}"]`);
	}

	refreshBar() {
		for (const command of this.commands) {
			if (command.bar !== true) {
				continue;
			}
			// By the command's own name, not by the role its button carries:
			// two commands can end up with the same role - `palette.open` and
			// `menu.open` are both "open" - and then one of them would be
			// refreshed twice and the other never.
			const button = this.barButton(command);
			if (button === null) {
				continue;
			}
			button.hidden = !this.barShows(command);
			button.disabled = (command.enabled !== undefined && !command.enabled(this))
				|| (this.readonly && command.safe !== true);
			// The name is written again rather than once at the start: whether
			// the shortcut belongs beside it is not known until the first key.
			button.title = commandTitle(command, this.keysShown());
			if (command.pressed !== undefined) {
				button.setAttribute("aria-pressed", command.pressed(this) ? "true" : "false");
			}
		}
		// Proof mode has three states on one button, and which of the two
		// on-states it is in is not a thing `aria-pressed` can say.
		const proof = this.part("proof");
		if (proof !== null) {
			proof.dataset.proof = this.proof;
		}
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
		this.addMoreButtons();
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
				this.say(answer.error || "Refused", "error");
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
	/**
	 * The ten places a brush can be put away in. A digit fetches one, shift
	 * and a digit puts the brush there - the same as the keys, said where
	 * somebody can see that there are ten of them.
	 */
	buildSlots() {
		const strip = this.part("slots");
		if (strip === null || strip.childElementCount > 0) {
			return;
		}
		for (let slot = 0; slot < 10; slot++) {
			const button = document.createElement("button");
			button.type = "button";
			button.className = "editor-slot";
			button.dataset.role = "slot";
			button.dataset.slot = String(slot);
			button.textContent = String(slot);
			button.title = `Brush ${slot} (${slot}, shift and ${slot} to put one here)`;
			button.addEventListener("click", event => {
				if (event.shiftKey) {
					this.editor.storeBrush(slot);
					this.slotsUsed.add(slot);
				} else {
					this.editor.useBrush(slot);
				}
				this.refreshTiles();
			}, { signal: this.stopping.signal });
			strip.append(button);
		}
	}

	refreshSlots() {
		for (const button of this.parts("slot")) {
			button.setAttribute("aria-pressed", this.slotsUsed.has(Number(button.dataset.slot)) ? "true" : "false");
		}
	}

	wireTileset() {
		this.buildSlots();
		// The two tabs over the tileset: what is painted with, and what paints
		// by itself.
		for (const button of this.parts("tiles-tab")) {
			button.addEventListener("click", () => {
				this.tab.tiles = button.dataset.tab;
				this.applyTilesTab();
			}, { signal: this.stopping.signal });
		}
		this.wirePick(this.part("tileset"));
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
		const layer = this.selectedLayer();
		this.showPanel("tiles-panel", !(layer === null || layer.type !== "tiles"));
		if (!this.panelShown.get("tiles-panel")) {
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
		// A physics layer has no picture of its own: it is drawn out of the
		// entities sheet, so that is what its tileset shows too.
		const physics = layer.kind !== undefined && layer.kind !== "tiles";
		const source = physics
			? new URL(`editor/entities_clear/${this.editor.entitiesImage()}.png`, this.dataBase).href
			: image === null ? null : (image.external ? new URL(`mapres/${image.name}.png`, this.dataBase).href : `packed:${layer.image}:${image.name}`);
		if (source !== this.tilesetSource) {
			this.tilesetSource = source;
			this.tileset = null;
			if (!physics && image !== null && !image.external) {
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
		this.refreshSlots();
		this.paintTileset();
		this.paintPicker();
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
			this.say("Every number is taken", "error");
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
		const name = this.rulesNameFor(layer);
		this.automapThere = !(name === null || this.rules.get(name) === null);
		this.applyTilesTab();
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
		// A rules file that holds no configuration is a rules file with
		// nothing to run, so the tab for it goes grey.
		this.automapThere = configs.length > 0;
		this.applyTilesTab();
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

	/**
	 * The tileset in the colour of the layer it is drawn into.
	 *
	 * What "brush colouring" is in the native editor: a layer that tints its
	 * tiles blue shows a blue tileset, so that what is picked looks like what
	 * will appear. The colour is multiplied in and the picture's own alpha is
	 * put back, because a tint that filled the transparent parts would be a
	 * coloured square and not a tileset. Opaque either way - how see-through a
	 * layer is says nothing about which tile is which.
	 */
	tintTileset(paint, picture, canvas) {
		const layer = this.selectedLayer();
		if (!this.brushColouring || layer === null || !Array.isArray(layer.color)) {
			return;
		}
		const [r, g, b] = layer.color;
		if (r === 255 && g === 255 && b === 255) {
			return;
		}
		paint.save();
		paint.globalCompositeOperation = "multiply";
		paint.fillStyle = `rgb(${r}, ${g}, ${b})`;
		paint.fillRect(0, 0, canvas.width, canvas.height);
		paint.globalCompositeOperation = "destination-in";
		paint.drawImage(picture, 0, 0, canvas.width, canvas.height);
		paint.restore();
	}

	paintTileset(into) {
		const canvas = into === undefined ? this.part("tileset") : into;
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
			this.tintTileset(paint, packed, canvas);
		} else if (this.tileset !== null) {
			paint.imageSmoothingEnabled = false;
			paint.drawImage(this.tileset, 0, 0, canvas.width, canvas.height);
			this.tintTileset(paint, this.tileset, canvas);
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
		const layer = this.selectedLayer();
		this.showPanel("quads-panel", !(layer === null || layer.type !== "quads"));
		if (!this.panelShown.get("quads-panel")) {
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
		const layer = this.selectedLayer();
		this.showPanel("sounds-panel", !(layer === null || layer.type !== "sounds"));
		if (!this.panelShown.get("sounds-panel")) {
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
		// Panning and zooming change nothing about the map, but they do change
		// what the line at the bottom says about the view.
		this.refreshStatus();
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
		const drawn = this.paintProof(overlay) + this.paintCarve(overlay) + this.paintSources(overlay, known);
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
		// The same coordinate at the top of the inspector, where the hand is.
		// On a wide screen the line at the bottom is eighty centimetres from
		// what the hand is doing; the stylesheet shows it only there.
		const here = this.part("here");
		if (here !== null) {
			here.textContent = tile === null ? "" : `${tile.x}, ${tile.y}`;
		}
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
		if (this.tileInfo === "off") {
			readout.textContent = `${tile.x}, ${tile.y}`;
			readout.title = "";
			return;
		}
		const hex = index.toString(16).toUpperCase().padStart(2, "0");
		const number = this.tileInfo === "hex" ? `${index} (0x${hex})` : String(index);
		const said = this.editor.explain(where.group, where.layer, index);
		readout.textContent = `${tile.x}, ${tile.y} · ${number}${said === "" ? "" : ` · ${said}`}`;
		readout.title = said;
	}

	/**
	 * Where the knife has been clicked so far, so that a cut half made can be
	 * seen while it is being made.
	 */
	paintCarve(overlay) {
		if (this.carving === null || this.carving.length === 0) {
			return 0;
		}
		const where = this.selection;
		const spot = (x, y) => this.editor.groupPixelAt(where.group, x, y);
		const places = [];
		for (let at = 0; at < this.carving.length; at += 2) {
			const point = spot(this.carving[at], this.carving[at + 1]);
			if (point !== null) {
				places.push(point);
			}
		}
		if (places.length === 0) {
			return 0;
		}
		const line = document.createElementNS(SVG_NAMESPACE, "path");
		line.setAttribute("d", places.map((p, i) => `${i === 0 ? "M" : "L"}${p.x} ${p.y}`).join(""));
		line.setAttribute("class", "editor-carve");
		line.dataset.role = "carve-line";
		overlay.append(line);
		places.forEach((place, index) => {
			const dot = document.createElementNS(SVG_NAMESPACE, "circle");
			dot.setAttribute("cx", String(place.x));
			dot.setAttribute("cy", String(place.y));
			dot.setAttribute("r", "4");
			dot.setAttribute("class", "editor-carve-dot");
			dot.dataset.role = "carve-dot";
			dot.dataset.point = String(index);
			overlay.append(dot);
		});
		return places.length + 1;
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
		const name = this.rulesNameFor(layer);
		this.showPanel("rules-panel", !(name === null || this.rules.get(name) === null));
		if (!this.panelShown.get("rules-panel") || this.rules.get(name) === undefined) {
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
	/**
	 * A picture turned into map: as tiles with a palette of its own colours,
	 * or as quads, one per pixel.
	 *
	 * The browser decodes the file, the same as it does for a picture the map
	 * is drawn with. Which of the two is wanted is a question about the
	 * picture rather than about the map, so both are offered and neither is
	 * the default.
	 */
	wireArt() {
		const signal = this.stopping.signal;
		const file = this.part("art-file");
		let asQuads = false;
		this.part("tile-art").addEventListener("click", () => {
			asQuads = false;
			file.value = "";
			file.click();
		}, { signal: signal });
		this.part("quad-art").addEventListener("click", () => {
			asQuads = true;
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
				this.say("That picture could not be read", "error");
				return;
			}
			const name = chosen.name.replace(/\.[^.]*$/, "");
			if (asQuads) {
				const options = {
					pixelStep: Math.max(1, Number(this.part("art-step").value) || 1),
					quadSize: Math.max(1, Number(this.part("art-size").value) || 64),
					centralize: this.part("art-centralize").checked,
					merge: this.part("art-merge").checked,
				};
				const across = Math.ceil(pixels.width / options.pixelStep);
				const down = Math.ceil(pixels.height / options.pixelStep);
				// Said before it happens, because a quad per pixel of a
				// photograph is a number nobody means to ask for.
				if (across * down > ART_QUAD_WARNING && !confirm(
					`${name} would be up to ${across * down} quads. Go on?`)) {
					return;
				}
				const group = this.change(() => this.editor.addQuadArt(name, pixels, options));
				this.say(group >= 0 ? `${name} as quads` : "That picture was refused",
					group >= 0 ? "note" : "error");
			} else {
				const colors = this.editor.artColors(pixels);
				const sheets = Math.max(1, Math.ceil(colors / (ART_PALETTE_SIZE - 1)));
				if (colors === 0) {
					this.say("Nothing in that picture is opaque", "error");
					return;
				}
				if (sheets > 1 && !confirm(
					`${name} holds ${colors} colours, which needs ${sheets} palettes and ${sheets} layers. Go on?`)) {
					return;
				}
				const group = this.change(() => this.editor.addTileArt(name, pixels));
				this.say(group >= 0
					? `${name} as tiles: ${colors} ${colors === 1 ? "colour" : "colours"}${sheets > 1 ? ` in ${sheets} layers` : ""}`
					: "That picture was refused", group >= 0 ? "note" : "error");
			}
			this.refresh();
		}, { signal: signal });
	}

	/**
	 * Typing with tiles.
	 *
	 * A font tileset is a tileset like any other; what makes it a font is
	 * that `A` is at 1 and `1` is at 54, which is a convention of the sheets
	 * people draw rather than anything the file knows. The editor in the
	 * client types a keystroke at a time in a mode of its own; a page has
	 * text fields, so here it is a text and one history entry - which is also
	 * the only version that can be undone in one go.
	 *
	 * Where it goes is the middle of the view, because that is where somebody
	 * is looking when they decide to write something.
	 */
	wireType() {
		const signal = this.stopping.signal;
		const write = () => {
			const field = this.part("type-text");
			const text = field.value;
			if (text === "" || this.map === null) {
				return;
			}
			const where = this.selection;
			const at = this.editor.tileAt(this.editor.canvas.width / 2, this.editor.canvas.height / 2);
			if (at === null) {
				return;
			}
			const answer = this.change(() => this.editor.apply({
				op: "layer.type", group: where.group, layer: where.layer, x: at.x, y: at.y, text: text,
			}));
			if (answer && answer.ok) {
				this.say(`${answer.tiles} ${answer.tiles === 1 ? "tile" : "tiles"} at ${at.x}, ${at.y}`);
				field.value = "";
			} else {
				this.say(answer && answer.error ? answer.error : "That text was refused", "error");
			}
			this.refresh();
		};
		this.part("type-place").addEventListener("click", write, { signal: signal });
		this.part("type-text").addEventListener("keydown", event => {
			if (event.key === "Enter") {
				event.preventDefault();
				write();
			}
		}, { signal: signal });
	}

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
		this.part("knife").addEventListener("click", () => this.knife(this.carving === null),
			{ signal: this.stopping.signal });
	}

	/**
	 * The knife: out or away.
	 *
	 * Out, it waits for four clicks inside the quad that is picked and makes
	 * a piece of it into a quad of its own. Away, whatever was clicked so far
	 * is forgotten - a cut half made is not a cut.
	 */
	knife(out) {
		this.carving = out ? [] : null;
		this.part("knife").setAttribute("aria-pressed", out ? "true" : "false");
		this.refreshOverlay();
	}

	/**
	 * A click on the map while the knife is out.
	 *
	 * @param world Where it was, in the quad layer's own coordinates.
	 *
	 * @return Whether the knife took it, so the canvas knows to do nothing
	 * else with the click.
	 */
	carveAt(world) {
		if (this.carving === null || this.quad < 0) {
			return false;
		}
		this.carving.push(Math.round(world.x), Math.round(world.y));
		if (this.carving.length < 8) {
			this.refreshOverlay();
			return true;
		}
		const where = this.selection;
		const points = this.carving;
		this.carving = [];
		const answer = this.change(() => this.editor.apply({
			op: "quad.carve", group: where.group, layer: where.layer, quad: this.quad, points: points,
		}));
		if (answer && answer.ok) {
			this.quad = answer.quad;
		} else {
			this.say(answer && answer.error ? answer.error : "That cut was refused", "error");
		}
		this.refresh();
		return true;
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
				this.say("That is not a picture this browser can read", "error");
				return;
			}
			// The name without its suffix, which is what a map calls a
			// picture - the file is `grass_main.png`, the picture is
			// `grass_main`.
			const name = chosen.name.replace(/\.[^.]*$/, "");
			const put = (into, called) => this.change(() => {
				if (into >= 0) {
					this.image = into;
					return { ok: this.editor.setImagePixels(into, pixels) === true };
				}
				const index = this.editor.addImage(called, pixels);
				if (index >= 0) {
					this.image = index;
				}
				return { ok: index >= 0, error: "The picture was refused" };
			});
			if (replacing >= 0) {
				put(replacing, name);
				return;
			}
			// A map finds its pictures by name, so two of one name are one
			// too many: the second would never be the one a layer gets. Which
			// of the two things somebody meant is theirs to say.
			const images = this.map === null ? [] : this.map.images;
			const same = images.findIndex(image => image.name === name);
			if (same < 0) {
				put(-1, name);
				return;
			}
			let free = 2;
			while (images.some(image => image.name === `${name} ${free}`)) {
				++free;
			}
			this.askFor("The map has that picture", [
				{ name: "what", kind: "note", label: `There is already a picture called ${name}${images[same].external ? ", beside the map" : ", in the map"}.` },
				{
					name: "how", label: "Use the file", kind: "pick", value: "replace", choices: [
						{ value: "replace", label: `for ${name}` },
						{ value: "beside", label: `as ${name} ${free}` },
					],
				},
			], answer => put(answer.how === "replace" ? same : -1, answer.how === "replace" ? name : `${name} ${free}`),
			{ go: "Use it" });
		}, { signal: signal });
		this.part("unpack-image").addEventListener("click", async () => {
			if (this.image < 0 || this.map === null) {
				return;
			}
			const which = this.image;
			const image = this.map.images[which];
			const unpack = () => {
				this.change(() => this.editor.apply({ op: "image.setProp", image: which, prop: "external", value: true }));
				this.refresh();
			};
			// A picture beside the map is looked for among the game's own, by
			// name. One the game does not have is a layer everybody else sees
			// as nothing - which is worth one question before it happens.
			let known = true;
			try {
				const answer = await fetch(new URL(`mapres/${image.name}.png`, this.dataBase).href, { method: "HEAD" });
				known = answer.ok;
			} catch (error) {
				// Nobody to ask is not a no: unpacking stays possible offline.
			}
			if (known) {
				unpack();
				return;
			}
			this.askYesNo("Take the picture out of the map",
				`The game has no picture called ${image.name}. Out of the map, every layer that uses it shows nothing to anybody without the file.`,
				"Take it out anyway", unpack);
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
		this.showPanel("images-panel", this.map !== null);
		if (!this.panelShown.get("images-panel")) {
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
				this.say("That sound has no bytes to play", "error");
				return;
			}
			this.playing = URL.createObjectURL(new Blob([bytes], { type: "audio/ogg" }));
			audio.src = this.playing;
		}
		audio.play().catch(() => this.say("This browser would not play that", "error"));
	}

	refreshAudio() {
		this.showPanel("audio-panel", this.map !== null);
		if (!this.panelShown.get("audio-panel")) {
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
				this.say("That map could not be read", "error");
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
		this.showPanel("info-panel", this.map !== null);
		if (!this.panelShown.get("info-panel")) {
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

		this.refreshMemory();

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
		const count = this.envelopeCount();
		this.showPanel("envelopes-panel", this.map !== null);
		if (!this.panelShown.get("envelopes-panel")) {
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
	const settings = Object.assign({
		canvas: null, target: null, mode: null, onChange: null, onView: null, onHover: null,
		onClickInGroup: null, afterStroke: null, onLongPress: null, onFingerTap: null, onAsk: null,
		signal: undefined,
	}, options || {});
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
	// Every pointer that is down on the canvas right now. A mouse has one; a
	// hand has as many as it has fingers on the glass, and what the hand means
	// depends on how many of them there are.
	const down = new Map();
	// When the first of them landed and where, so that a second one arriving
	// straight after can be told from one arriving later.
	let firstAt = 0;
	let firstAtSpot = { x: 0, y: 0 };
	// How many were down at once before they all came up, so that a tap can
	// be counted after the fingers have gone.
	let mostFingers = 0;
	// Two fingers panning and zooming, or null while they are not.
	let gesture = null;
	// How far they went, kept apart from the gesture itself because the
	// gesture ends when the second finger lifts and the question "was that a
	// tap" is only asked when the last one does.
	let gestureMoved = -1;
	// The press that is waiting to become a question about this place.
	let pressing = null;
	// Whether a pen has been seen. A hand that holds a pen rests on the glass,
	// so once one has been seen a finger stops painting and pans instead.
	let sawPen = false;
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
		const wide = event.pointerType === "touch" ? HANDLE_REACH_FINGER : HANDLE_REACH_PIXELS;
		const step = editor.groupWorldAt(where.group, spot.x + wide, spot.y);
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
		const wide = event.pointerType === "touch" ? HANDLE_REACH_FINGER : HANDLE_REACH_PIXELS;
		const step = editor.groupWorldAt(where.group, spot.x + wide, spot.y);
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

	/**
	 * Throws away what the first finger had begun.
	 *
	 * Only what is not settled yet is thrown away: a stroke that has lasted
	 * long enough, or gone far enough, was meant, and a finger that lands on
	 * the glass beside it is a hand resting, not a gesture.
	 */
	const undoTheStart = () => {
		if (doing === null) {
			return;
		}
		if (doing === "paint" || doing === "quad" || doing === "source") {
			editor.abort();
			changed();
		}
		doing = null;
		quadPoint = null;
		sourceDrag = null;
		editor.mark();
	};

	/**
	 * Whether the stroke has gone on long enough, or far enough, to be meant.
	 *
	 * How far is about the *first* finger: how far it has travelled since it
	 * landed. Where the second one comes down says nothing - two fingers of
	 * one hand land a hundred pixels apart, which is not movement.
	 */
	const settled = event => {
		const now = event.timeStamp || Date.now();
		const first = down.get(pointer);
		const went = first === undefined ? 0
			: Math.hypot(first.x - firstAtSpot.x, first.y - firstAtSpot.y);
		return now - firstAt > SECOND_FINGER_MS || went > SECOND_FINGER_PIXELS;
	};

	/** Where two fingers are, as one place and one distance apart. */
	const twoFingers = () => {
		const spots = [...down.values()].slice(0, 2);
		if (spots.length < 2) {
			return null;
		}
		return {
			x: (spots[0].x + spots[1].x) / 2,
			y: (spots[0].y + spots[1].y) / 2,
			apart: Math.max(1, Math.hypot(spots[0].x - spots[1].x, spots[0].y - spots[1].y)),
		};
	};

	const stopPressing = () => {
		if (pressing !== null) {
			clearTimeout(pressing.timer);
			pressing = null;
		}
	};

	canvas.addEventListener("contextmenu", event => event.preventDefault(), { signal: signal });
	canvas.addEventListener("pointerdown", event => {
		down.set(event.pointerId, { x: event.clientX, y: event.clientY, at: event.timeStamp || Date.now() });
		mostFingers = Math.max(mostFingers, down.size);
		if (event.pointerType === "pen") {
			sawPen = true;
		}
		if (down.size === 1) {
			firstAt = event.timeStamp || Date.now();
			firstAtSpot = { x: event.clientX, y: event.clientY };
			// Somebody asked a question with a button and this tap is the
			// answer: it says where, and it does nothing else.
			if (settings.onAsk !== null && settings.onAsk({ x: event.clientX, y: event.clientY }) === true) {
				return;
			}
			// A press that stands still is a question about the place; it is
			// what Ctrl and the right button are on a desk, and a finger has
			// neither.
			if (event.pointerType === "touch" && settings.onLongPress !== null) {
				const where = { x: event.clientX, y: event.clientY };
				pressing = { timer: setTimeout(() => {
					pressing = null;
					if (settings.onLongPress(where) === true) {
						undoTheStart();
					}
				}, LONG_PRESS_MS) };
			}
		}
		if (down.size === 2) {
			stopPressing();
			if (!settled(event)) {
				undoTheStart();
			}
			const two = twoFingers();
			if (two !== null && doing === null) {
				gesture = { x: two.x, y: two.y, apart: two.apart };
				gestureMoved = 0;
			}
			return;
		}
		if (down.size > 2) {
			stopPressing();
			return;
		}
		// A finger on a tablet where a pen has been seen holds the paper; the
		// pen is what draws.
		if (sawPen && event.pointerType === "touch") {
			pointer = event.pointerId;
			last = { x: event.clientX, y: event.clientY };
			capture(pointer, true);
			doing = "move";
			return;
		}
		if (doing !== null) {
			return;
		}
		pointer = event.pointerId;
		last = { x: event.clientX, y: event.clientY };
		capture(pointer, true);
		const where = target();
		// A tool that takes clicks takes this one and nothing else happens
		// with it - the knife is the one there is so far.
		if (event.button === 0 && where !== null && settings.onClickInGroup !== null) {
			const spot = atCanvas(event);
			const world = editor.groupWorldAt(where.group, spot.x, spot.y);
			if (world !== null && settings.onClickInGroup(world, where) === true) {
				doing = "tool";
				return;
			}
		}
		if (event.button === 0 && where !== null && (takeQuadPoint(event, where) || takeSource(event, where))) {
			return;
		}
		const tile = tileAt(event);
		if (event.button !== 0 || where === null || tile === null) {
			doing = "move";
			return;
		}
		from = tile;
		// A held modifier says what this one stroke is; without one it is
		// whatever the brush has been set to, which is painting until somebody
		// says otherwise.
		const asked = event.altKey ? "fill"
			: event.shiftKey ? "grab"
				: (event.ctrlKey || event.metaKey) ? "erase"
					: (settings.mode === null ? "paint" : settings.mode());
		// Nothing in hand draws nothing, so an empty brush grabs instead.
		// That is the rule the native editor has and mappers have in their
		// fingers: Escape empties the brush, and then dragging picks out a
		// rectangle.
		const chosen = asked === "paint" && editor.brushEmpty() ? "grab" : asked;
		if (chosen !== "paint") {
			doing = chosen;
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
		const held = down.get(event.pointerId);
		if (held !== undefined) {
			held.x = event.clientX;
			held.y = event.clientY;
		}
		if (pressing !== null && down.size === 1) {
			const away = Math.hypot(event.clientX - firstAtSpot.x, event.clientY - firstAtSpot.y);
			if (away > LONG_PRESS_PIXELS) {
				stopPressing();
			}
		}
		// Two fingers move the map and nothing in it: the middle of them is
		// what pans, how far apart they are is what zooms, and what angle they
		// stand at is nothing at all.
		if (gesture !== null && down.size >= 2) {
			const two = twoFingers();
			if (two === null) {
				return;
			}
			const factor = scale();
			gestureMoved += Math.hypot(two.x - gesture.x, two.y - gesture.y) + Math.abs(two.apart - gesture.apart);
			editor.moveByPixels(-(two.x - gesture.x) * factor, -(two.y - gesture.y) * factor);
			const box = canvas.getBoundingClientRect();
			editor.zoomAt((two.x - box.left) * factor, (two.y - box.top) * factor, gesture.apart / two.apart);
			gesture = { x: two.x, y: two.y, apart: two.apart };
			moved();
			return;
		}
		if (doing === null || pointer !== event.pointerId) {
			return;
		}
		if (doing === "tool") {
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

	/**
	 * A finger leaving: what it leaves behind, and what all of them together
	 * turn out to have meant.
	 *
	 * Counted when the last of them goes, because two fingers that went down
	 * and came up again without going anywhere are a tap, and a tap is only a
	 * tap once it is over.
	 */
	const fingerUp = event => {
		down.delete(event.pointerId);
		stopPressing();
		if (down.size >= 2) {
			return false;
		}
		const was = gesture;
		if (down.size < 2) {
			gesture = null;
		}
		if (down.size > 0) {
			return was !== null;
		}
		const fingers = mostFingers;
		const went = gestureMoved;
		const lasted = (event.timeStamp || Date.now()) - firstAt;
		mostFingers = 0;
		gestureMoved = -1;
		if (fingers >= 2 && went >= 0 && went < FINGER_TAP_PIXELS && lasted < FINGER_TAP_MS) {
			if (settings.onFingerTap !== null) {
				settings.onFingerTap(fingers);
			}
			return true;
		}
		return was !== null || went >= 0;
	};

	const release = event => {
		if (fingerUp(event)) {
			capture(event.pointerId, false);
			return;
		}
		if (doing === null || pointer !== event.pointerId) {
			return;
		}
		if (doing === "tool") {
			// A tool's click did its work when it went down; letting go of it
			// is nothing.
			doing = null;
			capture(event.pointerId, false);
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
		down.delete(event.pointerId);
		stopPressing();
		if (down.size < 2) {
			gesture = null;
		}
		if (down.size === 0) {
			mostFingers = 0;
			gestureMoved = -1;
		}
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
// is taken hold of, in pixels of the canvas. A finger is fatter than a mouse
// and cannot see what it covers, so it is allowed to be further off.
const HANDLE_REACH_PIXELS = 10;
const HANDLE_REACH_FINGER = 22;

// The second finger of a pan lands fifty to a hundred and fifty milliseconds
// after the first, and by then the first has already put down a tile. Within
// this window, and within this many pixels, the second finger says the first
// was never a stroke: it is thrown out, and the two of them are a pan.
//
// After it, a stroke is settled and a late finger is ignored - a hand resting
// on the glass while the other draws is not a gesture.
// How wide the strip along the edge is that a drawer is pulled out of, and
// how far inwards a finger has to travel before it counts as a pull rather
// than a tap. Twenty pixels is narrow enough that a stroke which starts near
// the edge of the map still starts on the map.
const EDGE_SWIPE_ZONE = 20;
const EDGE_SWIPE_REACH = 40;

const SECOND_FINGER_MS = 150;
const SECOND_FINGER_PIXELS = 8;

// Two or three fingers down and up again without going anywhere: back, and
// forward. Procreate's, and the only undo a tablet without a keyboard has.
const FINGER_TAP_MS = 250;
const FINGER_TAP_PIXELS = 12;

// A finger that stands still this long, this near where it landed, is asking
// about the place rather than drawing on it.
const LONG_PRESS_MS = 500;
const LONG_PRESS_PIXELS = 8;

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
/** How many colours one palette picture holds, one of them being none. */
const ART_PALETTE_SIZE = 256;

/** Above this many quads, a picture is asked about before it becomes one. */
const ART_QUAD_WARNING = 5000;

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

// ---------------------------------------------------------------------------
// The box.

/**
 * What the frame around the map is dressed in. This is the only style the
 * package keeps in the program: everything it holds is a grid of six areas,
 * and everything *in* those areas is the page's own light DOM, dressed by
 * `ddnet-editor.css` the way it always was. The colours below are asked for
 * as variables and inherit through the shadow boundary from that same file.
 */
const BOX_STYLE = `
:host {
	display: block;
	position: relative;
	/* The areas answer to the width of the box, not of the window: an editor
	   in an 800-pixel hole in somebody's page is a narrow editor even on a
	   wide screen. */
	container-type: inline-size;
	container-name: editor;
	background: var(--bg-0, #121216);
	color: var(--text-1, #ececf1);
	font: var(--type-sm, 400 12px/16px system-ui, sans-serif);
	overflow: hidden;
}

:host([hidden]) {
	display: none;
}

.box {
	display: grid;
	grid-template-columns: auto minmax(0, 1fr) auto;
	grid-template-rows: auto auto minmax(0, 1fr) auto auto;
	grid-template-areas:
		"head head head"
		"tools tools tools"
		"left map right"
		"dock dock dock"
		"status status status";
	width: 100%;
	height: 100%;
	min-width: 0;
	min-height: 0;
}

.area {
	min-width: 0;
	min-height: 0;
	overflow: hidden;
}

/* A side that is a drawer keeps its place in the grid but is laid over the map
   rather than beside it: the same box, the same panels, the same names - only
   "grid-area: map" instead of its own column, which takes no arithmetic and
   cannot be off by the height of a tool bar. A side that is shut holds a box
   that is switched off, so it takes no width and lies over nothing. */
:host([data-left="drawer"]) .area.left,
:host([data-right="drawer"]) .area.right {
	grid-area: map;
	width: min(288px, 80%);
	z-index: 2;
}

:host([data-left="drawer"]) .area.left {
	justify-self: start;
}

:host([data-right="drawer"]) .area.right {
	justify-self: end;
}

/* Narrower than a drawer is worth: from the floor, half the height, because
   288 pixels of drawer on a 390-pixel phone leave 102 pixels of map. */
:host([data-left="sheet"]) .area.left,
:host([data-right="sheet"]) .area.right {
	grid-area: map;
	align-self: end;
	width: 100%;
	height: 50%;
	z-index: 2;
}

/* Too short for two rows above the map: the page's own header is the one that
   goes. Ours carries the tools, and the tools are the editor. */
:host([data-head="one"]) .area.head {
	display: none;
}

/* Too short for a line of its own: the status becomes a chip in the corner of
   the map, and the row it had collapses because the box left it. */
:host([data-status="chip"]) .area.status {
	grid-area: map;
	align-self: end;
	justify-self: start;
	max-width: 60%;
	z-index: 2;
}

/* And the dock lies over the foot of the map instead of pushing it up. */
:host([data-dock="overlay"]) .area.dock {
	grid-area: map;
	align-self: end;
	height: 200px;
	z-index: 2;
}

/* Above the six areas and over all of them: a menu opened from a row of the
   tree would otherwise be cut off by the edge of the column the tree stands
   in, and the column is the narrowest thing on the screen. The layer itself
   catches nothing - only what is put in it does, or the map under it would
   stop hearing the pointer. */
.over {
	position: absolute;
	inset: 0;
	z-index: 5;
	pointer-events: none;
}

/* An area nobody filled takes no room at all - not a line, not a gap. The
   class is set from a slotchange, because a slot with nothing in it is still
   a box as far as the grid is concerned. */
.area.empty {
	display: none;
}

.head { grid-area: head; }
.tools { grid-area: tools; }
.left { grid-area: left; }
.map { grid-area: map; position: relative; }
.right { grid-area: right; }
.dock { grid-area: dock; }
.status { grid-area: status; }
`;

const BOX_HTML = `
<div class="box" data-role="box">
	<div class="area head"><slot name="header"></slot></div>
	<div class="area tools"><slot name="toolbar"></slot></div>
	<div class="area left"><slot name="left"></slot></div>
	<div class="area map"><slot name="map"></slot></div>
	<div class="area right"><slot name="right"></slot></div>
	<div class="area dock"><slot name="dock"></slot></div>
	<div class="area status"><slot name="status"></slot></div>
</div>
<div class="over"><slot name="over"></slot></div>
`;

/**
 * Every editor element that is on the page right now.
 *
 * The keyboard is the reason: a key pressed while nothing at all has the
 * focus belongs to the editor when there is one editor, and to nobody when
 * there are two - there would be no way to say which.
 */
const boxes = new Set();

/**
 * Who has the focus, through however many shadow roots. `document.activeElement`
 * stops at the first one and names the element that holds it, not what is
 * inside.
 */
function deepActive() {
	let element = document.activeElement;
	while (element !== null && element.shadowRoot !== null && element.shadowRoot.activeElement !== null) {
		element = element.shadowRoot.activeElement;
	}
	return element;
}

// A worker has no `HTMLElement`, and a class cannot be declared from a name
// that is not there. Nothing in a worker makes one, so the stand-in is never
// used for anything.
const ELEMENT_BASE = typeof HTMLElement === "undefined" ? class {} : HTMLElement;

/**
 * `<ddnet-editor>` - the whole editor as one element.
 *
 * ```html
 * <ddnet-editor src="maps/ctf1.map" style="height: 100dvh"></ddnet-editor>
 * ```
 *
 * It makes the program, the canvas, the shapes over it and the panels, and
 * lays them out in six areas: `header`, `toolbar`, `left`, `right`, `dock`
 * and `status`. A page may put its own things into any of them with
 * `slot="header"` and the like; what the element fills in itself is light DOM
 * as well, so `ddnet-editor.css` dresses it and a page may reach it.
 *
 * Attributes: `src` a map to open, `urlparam` a parameter of the page's own
 * address to take one from, `theme="light"`, `remember` to let it keep what
 * is being edited in the browser's storage between visits.
 *
 * The element takes no part in the page's keyboard unless the focus is inside
 * it - which is what lets two of them stand on one page.
 */
class CEditorElement extends ELEMENT_BASE {
	static observedAttributes = ["src", "theme", "controls", "readonly", "targets"];

	constructor() {
		super();
		const root = this.attachShadow({ mode: "open" });
		root.innerHTML = `<style>${BOX_STYLE}</style>${BOX_HTML}`;
		// What the element puts into its own light DOM: the map, and the
		// panels beside it. A page that wants them somewhere else moves them;
		// a page that wants none of them is Step 7's business.
		this.mapBox = document.createElement("div");
		this.mapBox.slot = "map";
		this.mapBox.className = "editor-map";
		this.editorCanvas = document.createElement("canvas");
		this.editorCanvas.className = "editor-canvas";
		this.editorCanvas.dataset.role = "map";
		// The map takes the keyboard, so it has to be able to hold it.
		this.editorCanvas.tabIndex = 0;
		this.mapBox.append(this.editorCanvas);
		// The way a map is chosen from the disc. The element keeps it, because
		// opening one is a command of the editor's and not of whatever page
		// happens to hold it.
		this.fileInput = document.createElement("input");
		this.fileInput.type = "file";
		this.fileInput.accept = ".map";
		this.fileInput.hidden = true;
		this.fileInput.dataset.role = "open-file";
		// One box per area the panels are spread into. Each carries
		// `editor-panels` as well, because that is the class the stylesheet
		// dresses everything inside a panel by.
		this.areaBoxes = {};
		// `status` would collide with the status line's own class, so the box
		// around it is called something else.
		for (const [area, name] of [["toolbar", "toolbar"], ["left", "left"], ["right", "right"], ["dock", "dock"], ["status", "statusbar"]]) {
			const box = document.createElement("div");
			box.slot = area;
			box.className = `editor-panels editor-${name}`;
			box.dataset.role = `area-${area}`;
			this.areaBoxes[area] = box;
		}
		this.editorInstance = null;
		this.editorPanels = null;
		this.stopping = null;
		/** A promise for the running editor, for a page that waits for one. */
		this.ready = null;
	}

	/** The program, once it runs, and `null` before that. */
	get editor() {
		return this.editorInstance;
	}

	/** The panels beside the map, once they are there. */
	get panels() {
		return this.editorPanels;
	}

	/** The canvas the map is drawn on. It is there before the program is. */
	get canvas() {
		return this.editorCanvas;
	}

	/**
	 * What shape the editor is in, and what decided it.
	 *
	 * Everything in here is worked out from the size of the box and from what
	 * the page asked for, and nothing else is remembered - so a page that asks
	 * gets the answer for the box as it stands, not for the box as it was when
	 * something last changed.
	 */
	get layout() {
		const box = this.getBoundingClientRect();
		const last = BOX_WIDTHS[BOX_WIDTHS.length - 1];
		const wide = BOX_WIDTHS.find(step => box.width >= step.from) || last;
		const tall = BOX_HEIGHTS.find(step => box.height >= step.from) || BOX_HEIGHTS[BOX_HEIGHTS.length - 1];
		const readonly = this.hasAttribute("readonly");
		// What the page asked for beats what the size would have chosen - a
		// page that says `controls="none"` wants its own buttons, whatever
		// room there is for ours.
		const said = this.getAttribute("controls");
		const bar = readonly ? "looking"
			: said === "none" ? "none"
				: said === "compact" ? "icons"
					: said === "full" ? "labels" : wide.bar;
		// Room enough to stop hiding things behind tabs. Either measurement
		// on its own is enough: a 3840-wide screen has the width for two
		// panels beside the map, and a 2160-tall one has the height for two
		// above each other. At 3840x2160 both are true.
		const roomy = !readonly && (wide.name === "huge" || tall.name === "high");
		return {
			width: Math.round(box.width),
			height: Math.round(box.height),
			size: wide.name,
			tallness: tall.name,
			left: wide.left,
			right: readonly ? "none" : wide.right,
			bar: bar,
			head: tall.head,
			status: tall.status,
			dock: tall.dock,
			// Two panels above each other rather than one behind a tab.
			stack: roomy,
			readonly: readonly,
		};
	}

	/**
	 * Says the shape out loud, on the element itself.
	 *
	 * The stylesheet reads these rather than asking the box its width a second
	 * time: the numbers are in `BOX_WIDTHS`, and a container query would be a
	 * second copy of them.
	 */
	applyLayout() {
		const now = this.layout;
		this.dataset.size = now.size;
		this.dataset.tall = now.tallness;
		this.dataset.left = now.left;
		this.dataset.right = now.right;
		this.dataset.bar = now.bar;
		this.dataset.head = now.head;
		this.dataset.status = now.status;
		this.dataset.dock = now.dock;
		this.dataset.stack = now.stack ? "yes" : "no";
		// Whether the editor is drawn for a finger. The query answers it in
		// almost every case; the attribute is for the cases where it lies -
		// a touch laptop with a mouse says `fine`, a tablet in desktop mode
		// says `coarse`, and neither is what the hand on it is doing.
		this.dataset.big = this.editorPanels !== null && this.editorPanels.finger() ? "yes" : "no";
		this.dataset.readonly = now.readonly ? "yes" : "no";
		if (this.editorPanels !== null) {
			this.editorPanels.applyShape(now);
		}
	}

	/**
	 * Keeps the field somebody is typing in above the keyboard.
	 *
	 * An on-screen keyboard does not make the window smaller; it covers the
	 * bottom of it, and the only thing that says so is `visualViewport`. What
	 * the editor does about it is the least that helps: the field that has the
	 * focus is scrolled into view, and the box says that a keyboard is up for
	 * whoever wants to draw differently for it.
	 */
	watchKeyboard() {
		const port = window.visualViewport;
		if (port === null || port === undefined) {
			return;
		}
		const look = () => {
			const covered = window.innerHeight - port.height;
			this.dataset.keyboard = covered > 120 ? "yes" : "no";
			if (covered <= 120) {
				return;
			}
			const focused = document.activeElement;
			if (focused !== null && focused !== this && this.contains(focused)) {
				focused.scrollIntoView({ block: "nearest" });
			}
		};
		port.addEventListener("resize", look, { signal: this.stopping.signal });
		look();
	}

	// The box can change size without the window doing anything at all - a
	// page that folds a sidebar away makes the editor wider - so the box is
	// what is watched.
	watchSize() {
		this.applyLayout();
		const watcher = new ResizeObserver(() => this.applyLayout());
		watcher.observe(this);
		this.stopping.signal.addEventListener("abort", () => watcher.disconnect(), { once: true });
		// A mouse plugged into a tablet changes the answer without changing
		// the size, and the box would go on saying what it said before.
		matchMedia("(pointer: coarse)").addEventListener("change", () => this.applyLayout(),
			{ signal: this.stopping.signal });
	}

	/**
	 * One of the element's parts by the name it carries. Its own first, the
	 * frame's after - `data-role` names are this element's, not the page's,
	 * which is what lets two editors stand on one page and both be asked.
	 */
	part(role) {
		const which = `[data-role="${role}"]`;
		return this.querySelector(which) || this.shadowRoot.querySelector(which);
	}

	/** Asks for a map file, and opens whatever comes back. */
	openFile() {
		this.fileInput.click();
	}

	connectedCallback() {
		// Moving an element within a page takes it out and puts it back, and a
		// program is too dear to throw away for that.
		if (this.ready !== null) {
			return;
		}
		boxes.add(this);
		this.stopping = new AbortController();
		if (!this.contains(this.mapBox)) {
			this.append(this.mapBox, this.fileInput, ...Object.values(this.areaBoxes));
		}
		this.watchAreas();
		this.watchSize();
		this.watchKeyboard();
		this.ready = this.start();
	}

	disconnectedCallback() {
		boxes.delete(this);
		const stopping = this.stopping;
		const instance = this.editorInstance;
		this.editorInstance = null;
		this.editorPanels = null;
		this.stopping = null;
		this.ready = null;
		if (stopping !== null) {
			stopping.abort();
		}
		if (instance !== null) {
			instance.destroy();
		}
	}

	attributeChangedCallback(name, was, now) {
		if (was === now) {
			return;
		}
		if (name === "theme") {
			this.applyTheme();
		} else if (name === "controls" || name === "readonly" || name === "targets") {
			if (this.stopping !== null) {
				this.applyLayout();
			}
		} else if (name === "src" && this.editorInstance !== null && now !== null && now !== "") {
			this.editorInstance.loadUrl(now);
		}
	}

	// An area with nothing in it is not an empty strip at the top of the box;
	// it is not there.
	watchAreas() {
		const signal = this.stopping.signal;
		for (const slot of this.shadowRoot.querySelectorAll("slot")) {
			const area = slot.parentElement;
			const look = () => area.classList.toggle("empty", slot.assignedNodes({ flatten: true }).length === 0);
			slot.addEventListener("slotchange", look, { signal: signal });
			look();
		}
	}

	// Which colours the panels use. The element is the one that knows which of
	// them are its own, so it says so on each of them rather than leaving the
	// page to find them.
	applyTheme() {
		const theme = this.getAttribute("theme");
		for (const part of [this.mapBox, ...this.querySelectorAll(".editor-panels, .editor-overlay")]) {
			if (theme === null || theme === "") {
				delete part.dataset.theme;
			} else {
				part.dataset.theme = theme;
			}
		}
		if (theme === null || theme === "") {
			delete this.dataset.theme;
		} else {
			this.dataset.theme = theme;
		}
	}

	async start() {
		const signal = this.stopping.signal;
		const source = this.getAttribute("src");
		// A parameter of the page's own address is read only where the page
		// said to read one: two editors on a page cannot both be the one the
		// address is about.
		const parameter = this.getAttribute("urlparam");
		let instance = null;
		try {
			instance = await CMapEditor.open({
				canvas: this.editorCanvas,
				file: source === null || source === "" ? undefined : source,
				urlParams: parameter === null || parameter === "" ? [] : [parameter],
				// An element in somebody else's page keeps nothing unless it
				// was asked to: a page that quietly filled a visitor's storage
				// would be a surprise.
				persist: this.hasAttribute("remember"),
				signal: signal,
				onOutput: (text, kind) => {
					if (kind.error) {
						console.error(text);
					}
				},
			});
		} catch (error) {
			this.dispatchEvent(new CustomEvent("editor-failed", { detail: { error: error } }));
			throw error;
		}
		if (signal.aborted) {
			instance.destroy();
			return null;
		}
		this.editorInstance = instance;
		// The canvas is a box on a page here, not the window, so nothing else
		// would tell the program when it changes shape.
		followSize(this.mapBox, instance, { signal: signal });
		const panels = new CEditorPanels(instance, {
			// The keyboard is handed out by the element, below.
			keys: false,
			signal: signal,
		});
		// The tree to the left, the inspector to the right, the envelopes and
		// the history below, the bar above, the status line at the bottom.
		panels.spread(this.areaBoxes, this);
		this.editorPanels = panels;
		// The box already knows its shape - it worked it out before there was
		// anything to shape - so the panels are told once, now that they exist.
		this.applyLayout();
		steerWithPointer(instance, {
			canvas: this.editorCanvas,
			// An editor that is only to be looked at has no layer to paint in,
			// which is the one place that has to say so - the pointer does not
			// go through `run`, where everything else is refused.
			target: () => (panels.readonly ? null : panels.selection),
			mode: () => panels.tool,
			onChange: () => panels.refresh(),
			// Panning and zooming change nothing about the map, so the panels
			// are left alone - but what is drawn over the canvas is now over
			// the wrong place.
			onView: () => panels.refreshOverlay(),
			onHover: tile => panels.hoverAt(tile),
			// A tap that answers a question the panels asked.
			onAsk: spot => panels.answerHere(spot),
			// A finger that stands still over an empty brush asks which layer
			// is there; over a full one it is allowed to stand still.
			onLongPress: spot => {
				if (!instance.brushEmpty()) {
					return false;
				}
				panels.showChooser({ clientX: spot.x, clientY: spot.y });
				return true;
			},
			// Two fingers back, three forward. Procreate's, and the only undo
			// a tablet without a keyboard has.
			onFingerTap: fingers => panels.run(fingers >= 3 ? "edit.redo" : "edit.undo"),
			// A tool that takes clicks - the knife so far - takes this one and
			// the canvas does nothing else with it.
			onClickInGroup: world => panels.carveAt(world),
			// A layer that automaps itself does it while the stroke's change is
			// still open, so that drawing and what it led to are one thing to
			// undo.
			afterStroke: (where, box) => panels.automapAfterStroke(where, box),
			signal: signal,
		});
		this.applyTheme();
		// Whatever has been changed goes into the browser's own storage every
		// minute - a safety net, not a place to keep a map.
		if (this.hasAttribute("remember")) {
			instance.autosave(60);
		}
		document.addEventListener("keydown", event => {
			if (this.hears()) {
				panels.onKey(event);
			}
		}, { signal: signal });
		document.addEventListener("keyup", event => {
			if (this.hears()) {
				panels.onKeyUp(event);
			}
		}, { signal: signal });
		// A map let go of over the box is a map to open. The box is the whole
		// element, so a file dropped on the panels counts as much as one
		// dropped on the map.
		this.fileInput.addEventListener("change", () => {
			const file = this.fileInput.files[0];
			// The same file twice is still a choice; without this the second
			// time would go unnoticed.
			this.fileInput.value = "";
			if (file !== undefined && file !== null) {
				instance.loadFile(file).catch(error => console.error(error));
			}
		}, { signal: signal });
		this.addEventListener("dragover", event => event.preventDefault(), { signal: signal });
		this.addEventListener("drop", event => {
			const file = event.dataTransfer === null ? null : event.dataTransfer.files[0];
			if (file === undefined || file === null) {
				return;
			}
			event.preventDefault();
			instance.loadFile(file).catch(error => console.error(error));
		}, { signal: signal });
		// A page that keeps what is being edited asks before the tab goes with
		// something in it that is not written out even there yet. A page that
		// keeps nothing has nothing to lose that it did not know about.
		if (this.hasAttribute("remember")) {
			window.addEventListener("beforeunload", event => {
				if (instance.maps.some(id => instance.dirty(id))) {
					event.preventDefault();
					// What browsers before the standard wanted, and some still do.
					event.returnValue = "";
				}
			}, { signal: signal });
		}
		this.dispatchEvent(new CustomEvent("editor-ready", { detail: { editor: instance } }));
		return instance;
	}

	// Whether a key pressed now was meant for this editor. Inside it, always;
	// with the focus nowhere at all, only when it is the one editor on the
	// page - with two there would be no way to say which was meant.
	hears() {
		const active = deepActive();
		if (active !== null && (this.contains(active) || this.shadowRoot.contains(active))) {
			return true;
		}
		const nowhere = active === null || active === document.body || active === document.documentElement;
		return nowhere && boxes.size === 1;
	}
}

if (typeof customElements !== "undefined" && customElements.get("ddnet-editor") === undefined) {
	customElements.define("ddnet-editor", CEditorElement);
}

/**
 * A map editor: `MapEditor.open({canvas, src})` for one on a canvas the page
 * keeps, `MapEditor.openPage({elements})` for a page that is nothing else.
 */
export const MapEditor = CMapEditor;
/** The panels for one editor, for a page that places them itself. */
export const EditorPanels = CEditorPanels;
/** Dragging and the wheel on the canvas. */
export const steerEditor = steerWithPointer;
/** `<ddnet-editor>`, the whole thing as one element. */
export const EditorElement = CEditorElement;

export default {
	MapEditor,
	EditorPanels,
	EditorElement,
	steerEditor,
	programUrl,
	// The base this is built on, so that a page that has this has the rest of
	// it too without a second import.
	base: DDNetBase,
};
