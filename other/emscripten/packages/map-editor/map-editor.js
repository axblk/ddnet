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

// The pictures on the editor's own buttons: one set of stroke icons, drawn
// alike - a 24-unit box, a 1.75-unit line with round ends - so that a button
// in the rail, one in the top bar and one beside a row look like they belong
// to the same editor. A shape is filled only where it stands for something
// that is filled. Named as the viewer names its own where both have one.
const STROKE = 'fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"';
function stroked(d, extra) {
	return `<path d="${d}" ${STROKE}${extra === undefined ? "" : ` ${extra}`}/>`;
}
addIcons({
	paint: stroked("M14.5 4.5l5 5L9 20H4v-5zM12 7l5 5"),
	grab: `<rect x="4" y="4" width="16" height="16" rx="1.5" ${STROKE} stroke-dasharray="3 2.6"/>`,
	fill: stroked("M4.5 11.5l7-7 7 7-7 7zM9 4.5v-2M20.5 14c0 1.6 1.5 3 1.5 4.5a1.5 1.5 0 0 1-3 0c0-1.5 1.5-2.9 1.5-4.5z"),
	erase: stroked("M8.5 20l-4.6-4.6a1.5 1.5 0 0 1 0-2.1l8.9-8.9a1.5 1.5 0 0 1 2.1 0l5.6 5.6a1.5 1.5 0 0 1 0 2.1L13 19.6M8.5 20H20M7 10l7 7"),
	pick: stroked("M17 3.5l3.5 3.5-2 2-3.5-3.5zM14.5 6l3.5 3.5-8.7 8.7a1 1 0 0 1-.5.3L5 19.5l1-3.8a1 1 0 0 1 .3-.5z"),
	move: stroked("M12 3v18M3 12h18M9 6l3-3 3 3M9 18l3 3 3-3M6 9l-3 3 3 3M18 9l3 3-3 3"),
	hand: stroked("M7.5 11V6.5a1.5 1.5 0 0 1 3 0V11M10.5 10.5V4.5a1.5 1.5 0 0 1 3 0v6M13.5 10.5V6a1.5 1.5 0 0 1 3 0v6.5M16.5 12.5V9a1.5 1.5 0 0 1 3 0v6a6 6 0 0 1-6 6h-1.6a6 6 0 0 1-5-2.7L4 14.7a1.6 1.6 0 0 1 2.6-1.9l.9 1.2V11"),
	undo: stroked("M4 10h10a5 5 0 0 1 0 10H9M8 6l-4 4 4 4"),
	redo: stroked("M20 10H10a5 5 0 0 0 0 10h5M16 6l4 4-4 4"),
	save: stroked("M12 3v11M8 10l4 4 4-4M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"),
	folder: stroked("M3 6.5a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"),
	add: stroked("M12 5v14M5 12h14"),
	minus: stroked("M5 12h14"),
	plus: stroked("M12 5v14M5 12h14"),
	close: stroked("M6 6l12 12M18 6L6 18"),
	// A bin: throwing something out of the map, which a cross - closing a
	// panel - must not be mistaken for.
	trash: stroked("M4 7h16M9.5 7V4.5h5V7M6.5 7l1 12.5a1 1 0 0 0 1 .9h7a1 1 0 0 0 1-.9l1-12.5M10 11v5.5M14 11v5.5"),
	fit: stroked("M4 9V5a1 1 0 0 1 1-1h4M20 9V5a1 1 0 0 0-1-1h-4M4 15v4a1 1 0 0 0 1 1h4M20 15v4a1 1 0 0 1-1 1h-4M9 9h6v6H9z"),
	expand: stroked("M14 4h6v6M20 4l-7 7M10 20H4v-6M4 20l7-7"),
	detail: stroked("M12 3l2.2 6.3L21 11l-6.8 1.7L12 19l-2.2-6.3L3 11l6.8-1.7z"),
	entities: `<rect x="4" y="4" width="7" height="7" rx="1.2" ${STROKE}/><rect x="13" y="4" width="7" height="7" rx="1.2" ${STROKE}/><rect x="4" y="13" width="7" height="7" rx="1.2" ${STROKE}/>${stroked("M16.5 13.5v6.5M13.3 16.8h6.4")}`,
	play: stroked("M7 4.5l12 7.5-12 7.5z"),
	grid: stroked("M9 4v16M15 4v16M4 9h16M4 15h16"),
	proof: `<rect x="3" y="5" width="18" height="14" rx="2" ${STROKE}/><rect x="7" y="8.5" width="10" height="7" rx="1" ${STROKE}/>`,
	info: `<circle cx="12" cy="12" r="8.5" ${STROKE}/>${stroked("M12 11v5.5")}<circle cx="12" cy="7.8" r="1.1" fill="currentColor"/>`,
	search: `<circle cx="10.5" cy="10.5" r="6" ${STROKE}/>${stroked("M15 15l4.5 4.5")}`,
	menu: stroked("M4 7h16M4 12h16M4 17h16"),
	more: '<circle cx="6" cy="12" r="1.6" fill="currentColor"/><circle cx="12" cy="12" r="1.6" fill="currentColor"/><circle cx="18" cy="12" r="1.6" fill="currentColor"/>',
	chevronRight: stroked("M9.5 6l6 6-6 6"),
	chevronDown: stroked("M6 9.5l6 6 6-6"),
	eye: `${stroked("M2.5 12s3.5-6.5 9.5-6.5 9.5 6.5 9.5 6.5-3.5 6.5-9.5 6.5S2.5 12 2.5 12z")}<circle cx="12" cy="12" r="2.6" ${STROKE}/>`,
	eyeOff: `${stroked("M4 4l16 16M10 5.8A9.7 9.7 0 0 1 12 5.5c6 0 9.5 6.5 9.5 6.5a16 16 0 0 1-3.4 4M14.7 15.6A9.7 9.7 0 0 1 12 18.5C6 18.5 2.5 12 2.5 12a16 16 0 0 1 4.2-4.6")}`,
	layerTiles: `<rect x="4" y="4" width="16" height="16" rx="1.5" ${STROKE}/>${stroked("M9.3 4v16M14.7 4v16M4 9.3h16M4 14.7h16")}`,
	layerGame: `<rect x="4" y="4" width="16" height="16" rx="1.5" ${STROKE}/><path d="M13.2 6.5L9 12.6h3.4L10.8 17.5 15 11.4h-3.4z" fill="currentColor"/>`,
	layerPhysics: `<rect x="4" y="4" width="16" height="16" rx="1.5" ${STROKE}/><circle cx="12" cy="12" r="3" fill="currentColor"/>`,
	layerQuads: stroked("M12 3.5L20.5 12 12 20.5 3.5 12z"),
	layerSounds: stroked("M4 9.5v5h3.5L12 19V5L7.5 9.5zM15.5 9a4 4 0 0 1 0 6M18.2 6.3a7.8 7.8 0 0 1 0 11.4"),
	group: stroked("M3 6.5a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"),
	image: `<rect x="3.5" y="5" width="17" height="14" rx="2" ${STROKE}/><circle cx="9" cy="10" r="1.8" ${STROKE}/>${stroked("M20 15.5l-4.5-4.5-7 7")}`,
	sound: stroked("M4 9.5v5h3.5L12 19V5L7.5 9.5zM15.5 9a4 4 0 0 1 0 6M18.2 6.3a7.8 7.8 0 0 1 0 11.4"),
	envelope: stroked("M3 16c3 0 3-8 6-8s3 8 6 8 3-8 6-8"),
	history: `<circle cx="12" cy="12" r="8.5" ${STROKE}/>${stroked("M12 7.5V12l3 2")}`,
	settings: stroked("M4 7h10M18 7h2M4 17h4M12 17h8M14 4.5v5M8 14.5v5"),
	rules: stroked("M6 3h8l5 5v13H6zM14 3v5h5M9 13h7M9 17h7"),
	properties: stroked("M8 6h12M8 12h12M8 18h12M4 6h.01M4 12h.01M4 18h.01"),
	flipX: stroked("M12 3v18M8 7L3 12l5 5M16 7l5 5-5 5"),
	flipY: stroked("M3 12h18M7 8l5-5 5 5M7 16l5 5 5-5"),
	rotate: stroked("M20 12a8 8 0 1 1-2.3-5.7M20 4v4.5h-4.5"),
	check: stroked("M5 12.5l4.5 4.5L19 7.5"),
	open: stroked("M3 6.5a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"),
	map: `<rect x="3.5" y="4.5" width="17" height="15" rx="2" ${STROKE}/>${stroked("M9 4.5v15M15 4.5v15")}`,
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
// The kinds of ghost the program draws, in the order it numbers them.
const GHOST_KINDS = [null, "stamp", "fill", "erase", "spot"];

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
	/** How many changes are open - see `begin`. */
	editingDepth = 0;

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
		++this.editingDepth;
		return this.call("MapEditorBegin", null, ["number", "string", "string"],
			[this.which(id), label || "Change", merge || ""]);
	}
	commit(id) {
		this.editingDepth = Math.max(0, this.editingDepth - 1);
		return this.ask("MapEditorCommit", null, [this.which(id)]);
	}
	abort(id) {
		this.editingDepth = Math.max(0, this.editingDepth - 1);
		return this.ask("MapEditorAbort", null, [this.which(id)]);
	}
	/**
	 * Whether a change is open right now - a stroke under way. What the map
	 * says in between is half made, and a page need not rebuild itself on
	 * every stamp of it.
	 */
	get editing() {
		return this.editingDepth > 0;
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
	/**
	 * Which tile of a group is under a point on the canvas. The plain view's
	 * tile and this one differ where the group has parallax, and a stroke in
	 * such a group has to land where the pointer is.
	 */
	groupTileAt(group, x, y, id) {
		const world = this.groupWorldAt(group, x, y, id);
		return world === null ? null : { x: Math.floor(world.x / MAP_TILE_SIZE), y: Math.floor(world.y / MAP_TILE_SIZE) };
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
	 * What the brush would do under the pointer, drawn before the button goes
	 * down: `"stamp"` draws the brush faintly with its corner on the tile,
	 * `"fill"` the brush repeated over the rectangle, `"erase"` the rectangle
	 * a rubber would clear, `"spot"` an outline around the rectangle alone.
	 * Called with nothing, or with `null`, it takes the ghost away.
	 *
	 * Like the mark it lies in the tiles of that group, and like the mark it
	 * is about looking: no version, no history entry.
	 */
	ghost(kind, group, x, y, width, height, id) {
		const number = GHOST_KINDS.indexOf(kind);
		return number <= 0
			? this.setNumbers("MapEditorGhost", [this.which(id), 0, 0, 0, 0, 0, 0])
			: this.setNumbers("MapEditorGhost", [this.which(id), number, group, x, y,
				width === undefined ? 1 : width, height === undefined ? 1 : height]);
	}

	/**
	 * The brush in hand, or the one in a slot, as a picture could be drawn
	 * of it: `{kind, image, color, width, height, tiles}`, with `tiles` the
	 * index and the flags of every tile in turn, row by row, as the map
	 * draws them. `null` where there is nothing in hand or in the slot.
	 */
	brush() {
		return this.json("MapEditorBrush");
	}
	storedBrush(slot) {
		return this.json("MapEditorStoredBrush", [slot]);
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
	{ role: "props-panel", area: "left", tab: "props", name: "Properties" },
	{ role: "images-panel", area: "left", tab: "images", name: "Images" },
	{ role: "audio-panel", area: "left", tab: "sounds", name: "Sounds" },
	{ role: "info-panel", area: "left", tab: "map", name: "Map" },
	// A list of steps is a column of text, and a column of text belongs in a
	// column: the history stands in the inspector, not in the strip below.
	{ role: "history-panel", area: "left", tab: "history", name: "History", status: 2 },
	// Every panel has a tab key and a name, because every panel can be
	// carried into an area that shows one panel at a time.
	{ role: "tree-panel", area: "right", tab: "layers", name: "Layers" },
	{ role: "group-panel", area: "right", tab: "group", name: "Group" },
	{ role: "tiles-panel", area: "right", tab: "tiles", name: "Tiles" },
	{ role: "quads-panel", area: "right", tab: "quads", name: "Quads" },
	{ role: "sounds-panel", area: "right", tab: "sources", name: "Sound sources" },
	{ role: "envelopes-panel", area: "dock", tab: "envelopes", name: "Envelopes", status: 1 },
	{ role: "settings-panel", area: "dock", tab: "settings", name: "Server settings", status: 3 },
	{ role: "rules-panel", area: "dock", tab: "rules", name: "Rules", status: 4 },
];

/** The two areas that show one panel at a time, and what they are called. */
const TABBED_AREAS = { left: "structure", dock: "dock" };

/** The name a shape of box is remembered under: its width class and its height class. */
function shapeName(shape) {
	return `${shape.size}/${shape.tallness}`;
}

/** The three areas a panel can be carried to, and what they are called to a person. */
const PANEL_AREAS = ["left", "right", "dock"];
const AREA_NAMES = { left: "Left column", right: "Right column", dock: "Below the map" };

/** The pictures physics layers can be drawn with, by the name the program knows. */
const ENTITIES_SHEETS = { ddnet: "DDNet", ddrace: "DDRace", race: "Race", fng: "FNG", vanilla: "Vanilla", "f-ddrace": "F-DDrace", blockworlds: "Blockworlds" };

/** What a settings file says it is, and which shape of file this editor writes and reads. */
const SETTINGS_KIND = "ddnet-editor-settings";
const SETTINGS_VERSION = 1;

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
	{ from: 2200, name: "huge", left: "column", right: "column", bar: "labels", inspector: "open" },
	{ from: 1800, name: "desk", left: "column", right: "column", bar: "labels", inspector: "open" },
	{ from: 1000, name: "wide", left: "column", right: "column", bar: "labels", inspector: "shut" },
	{ from: 700, name: "medium", left: "drawer", right: "drawer", bar: "icons", inspector: "shut" },
	{ from: 0, name: "small", left: "drawer", right: "drawer", bar: "few", inspector: "shut" },
];

/** And by height: what there is room for above and below the map. */
const BOX_HEIGHTS = [
	{ from: 1300, name: "high", head: "two", status: "line", dock: "strip" },
	{ from: 560, name: "tall", head: "two", status: "line", dock: "strip" },
	{ from: 0, name: "low", head: "one", status: "chip", dock: "overlay" },
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
<div class="editor-bar" data-role="bar"></div>
<div class="editor-rail" data-role="rail" role="toolbar" aria-label="Tools"></div>
<div class="editor-statusbits">
	<span class="editor-hover" data-role="hover"></span>
	<span class="editor-status" data-role="status" role="status"></span>
</div>
<div class="editor-columns">
	<section class="editor-panel editor-panel-tree" data-role="tree-panel">
		<header class="editor-panel-head">
			<h2>Layers</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="add-layer-menu" data-icon="add" title="Add a layer or group" aria-label="Add a layer or group" aria-haspopup="menu"></button>
				<button type="button" class="editor-icon-button" data-role="layer-menu" data-icon="more" title="More" aria-label="Options for the selected layer or group" aria-haspopup="menu"></button>
			</span>
		</header>
		<ul class="editor-tree" data-role="tree"></ul>
	</section>
	<section class="editor-panel editor-empty" data-role="group-panel" hidden>
		<p class="editor-empty-text" data-role="group-text">A group holds layers. Select a layer to paint.</p>
		<button type="button" class="editor-small editor-go" data-role="select-game">Select the game layer</button>
	</section>
	<section class="editor-panel editor-panel-tiles" data-role="tiles-panel" hidden>
		<header class="editor-panel-head">
			<h2 data-role="tiles-title">Tiles</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="tiles-zoom-out" data-icon="minus" title="Smaller tiles" aria-label="Smaller tiles"></button>
				<button type="button" class="editor-icon-button" data-role="tiles-zoom-in" data-icon="plus" title="Bigger tiles" aria-label="Bigger tiles"></button>
				<button type="button" class="editor-icon-button" data-role="tiles-big" data-command="picker.show" data-icon="expand" aria-pressed="false"></button>
			</span>
		</header>
		<div class="editor-numbers" data-role="numbers"></div>
		<div class="editor-tileset-scroll" data-role="tiles-body">
			<canvas class="editor-tileset" data-role="tileset" width="256" height="256"></canvas>
		</div>
		<div class="editor-brush-row">
			<button type="button" class="editor-icon-button" data-role="flip-x" data-command="brush.flipX" data-icon="flipX"></button>
			<button type="button" class="editor-icon-button" data-role="flip-y" data-command="brush.flipY" data-icon="flipY"></button>
			<button type="button" class="editor-icon-button" data-role="rotate" data-command="brush.rotate" data-icon="rotate"></button>
			<span class="editor-brush-size" data-role="brush-size"></span>
			<button type="button" class="editor-small" data-role="clear-brush" data-command="brush.clear">Clear</button>
		</div>
		<details class="editor-section" data-role="slots-section">
			<summary>Brush slots</summary>
			<div class="editor-slots" data-role="slots" role="group" aria-label="Brush slots"></div>
		</details>
		<details class="editor-section" data-role="automap-section" hidden>
			<summary>Automap</summary>
			<div class="editor-automap" data-role="automap">
				<select class="editor-small" data-role="automap-config" aria-label="Rules configuration"></select>
				<select class="editor-small" data-role="automap-reference" aria-label="Reference layer"></select>
				<button type="button" class="editor-small" data-role="automap-run" title="Run the rules over this layer">Run</button>
				<label class="editor-check" title="Run them after every stroke, as part of the same change"><input type="checkbox" data-role="automap-auto"> Auto</label>
			</div>
		</details>
		<details class="editor-section" data-role="type-section">
			<summary>Write text with tiles</summary>
			<div class="editor-type" data-role="type">
				<input type="text" data-role="type-text" placeholder="Text" aria-label="Text to write" title="Letters and digits become the tiles of a font tileset; the layer has to be drawn with one">
				<button type="button" class="editor-small" data-role="type-place" title="Write it where the view is looking">Write</button>
			</div>
		</details>
	</section>
	<section class="editor-panel" data-role="quads-panel" hidden>
		<header class="editor-panel-head">
			<h2>Quads</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="add-quad" data-command="quad.add" data-icon="add"></button>
				<button type="button" class="editor-icon-button" data-role="delete-quad" data-command="quad.delete" data-icon="trash"></button>
				<button type="button" class="editor-small" data-role="knife" data-command="quad.knife" aria-pressed="false">Knife</button>
			</span>
		</header>
		<ol class="editor-list" data-role="quad-list"></ol>
		<div class="editor-shape" data-role="shape">
			<button type="button" class="editor-small" data-role="shape-square" title="Make the quad a rectangle">Square</button>
			<button type="button" class="editor-small" data-role="shape-aspect" title="Give the quad the image's proportions">Aspect</button>
			<button type="button" class="editor-small" data-role="shape-centerPivot" title="Move the pivot to the middle">Pivot</button>
			<button type="button" class="editor-small" data-role="shape-align" title="Snap every corner to the grid">Align</button>
		</div>
		<div class="editor-props" data-role="quad-props"></div>
	</section>
	<section class="editor-panel" data-role="sounds-panel" hidden>
		<header class="editor-panel-head">
			<h2>Sound sources</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="add-source" data-command="source.add" data-icon="add"></button>
				<button type="button" class="editor-icon-button" data-role="delete-source" data-command="source.delete" data-icon="trash"></button>
			</span>
		</header>
		<ol class="editor-list" data-role="source-list"></ol>
		<div class="editor-props" data-role="source-props"></div>
	</section>
	<section class="editor-panel" data-role="props-panel">
		<header class="editor-panel-head"><h2 data-role="props-title">Properties</h2><span class="editor-here" data-role="here"></span></header>
		<div class="editor-props" data-role="props"></div>
		<div class="editor-construct" data-role="construct" hidden>
			<select class="editor-small" data-role="construct-tile" aria-label="Physics tile"></select>
			<button type="button" class="editor-small" data-role="construct-run" title="Put this physics tile under every tile this layer draws">Build</button>
		</div>
	</section>
	<section class="editor-panel" data-role="images-panel">
		<header class="editor-panel-head">
			<h2>Images</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="add-image" data-command="image.add" data-icon="add"></button>
				<button type="button" class="editor-icon-button" data-role="replace-image" data-command="image.replace" data-icon="rotate"></button>
				<button type="button" class="editor-small" data-role="unpack-image" data-command="image.unpack">Unpack</button>
				<button type="button" class="editor-icon-button" data-role="delete-image" data-command="image.delete" data-icon="trash"></button>
			</span>
		</header>
		<ol class="editor-list editor-images" data-role="image-list"></ol>
		<input type="file" accept="image/png,image/*" data-role="image-file" hidden>
		<details class="editor-section" data-role="art-section">
			<summary>Import an image as map</summary>
			<div class="editor-art" data-role="art">
				<button type="button" class="editor-small" data-role="tile-art" title="Turn an image into tiles, with a palette of its own colours">As tiles…</button>
				<button type="button" class="editor-small" data-role="quad-art" title="Turn an image into quads, one per pixel">As quads…</button>
				<label class="editor-art-field">px <input type="number" data-role="art-step" min="1" max="64" value="1" title="Pixels of the image per quad"></label>
				<label class="editor-art-field">size <input type="number" data-role="art-size" min="1" max="1024" value="64" title="Width of a quad on the map, in world units"></label>
				<label class="editor-art-field editor-check"><input type="checkbox" data-role="art-merge" checked title="Join runs of one colour into one quad"> merge</label>
				<label class="editor-art-field editor-check"><input type="checkbox" data-role="art-centralize" title="One pivot for all quads"> one pivot</label>
				<input type="file" accept="image/png,image/*" data-role="art-file" hidden>
			</div>
		</details>
	</section>
	<section class="editor-panel" data-role="audio-panel">
		<header class="editor-panel-head">
			<h2>Sounds</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="play-sound" data-command="sound.play" data-icon="play"></button>
				<button type="button" class="editor-icon-button" data-role="add-sound" data-command="sound.add" data-icon="add"></button>
				<button type="button" class="editor-icon-button" data-role="replace-sound" data-command="sound.replace" data-icon="rotate"></button>
				<button type="button" class="editor-small" data-role="unpack-sound" data-command="sound.unpack">Unpack</button>
				<button type="button" class="editor-icon-button" data-role="delete-sound" data-command="sound.delete" data-icon="trash"></button>
			</span>
		</header>
		<ol class="editor-list" data-role="sound-list"></ol>
		<input type="file" accept="audio/opus,audio/ogg,.opus" data-role="sound-file" hidden>
		<audio data-role="sound-player" hidden></audio>
	</section>
	<section class="editor-panel" data-role="info-panel">
		<header class="editor-panel-head">
			<h2>Map</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-small" data-role="append-map" data-command="file.append">Append…</button>
			</span>
		</header>
		<div class="editor-props" data-role="info-props"></div>
		<h3 class="editor-subhead">Open maps</h3>
		<ul class="editor-memory" data-role="memory"></ul>
		<input type="file" accept=".map" data-role="append-file" hidden>
	</section>
	<section class="editor-panel" data-role="envelopes-panel">
		<header class="editor-panel-head">
			<h2>Envelopes</h2>
			<span class="editor-panel-tools">
				<select class="editor-small" data-role="envelope-list" aria-label="Envelope"></select>
				<button type="button" class="editor-icon-button" data-role="add-envelope" data-command="envelope.add" data-icon="add"></button>
				<button type="button" class="editor-icon-button" data-role="delete-envelope" data-command="envelope.delete" data-icon="trash"></button>
			</span>
		</header>
		<svg class="editor-curve" data-role="curve" viewBox="0 0 100 100" preserveAspectRatio="none"></svg>
		<div class="editor-props" data-role="point-props"></div>
	</section>
	<section class="editor-panel" data-role="history-panel">
		<header class="editor-panel-head">
			<h2>History</h2>
			<span class="editor-panel-tools"><span class="editor-panel-note" data-role="history-bytes"></span></span>
		</header>
		<ol class="editor-list editor-history" data-role="history"></ol>
	</section>
	<section class="editor-panel" data-role="settings-panel">
		<header class="editor-panel-head">
			<h2>Server settings</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-icon-button" data-role="add-setting" data-command="setting.add" data-icon="add"></button>
				<button type="button" class="editor-icon-button" data-role="delete-setting" data-command="setting.delete" data-icon="trash"></button>
			</span>
		</header>
		<ol class="editor-settings" data-role="setting-list"></ol>
		<p class="editor-setting-said" data-role="setting-said" role="status"></p>
		<datalist data-role="setting-names"></datalist>
	</section>
	<section class="editor-panel" data-role="rules-panel" hidden>
		<header class="editor-panel-head">
			<h2>Rules</h2>
			<span class="editor-panel-tools">
				<button type="button" class="editor-small" data-role="rules-apply" data-command="rules.apply">Apply</button>
				<button type="button" class="editor-small" data-role="rules-revert" data-command="rules.revert">Reload</button>
				<button type="button" class="editor-small" data-role="rules-save" data-command="rules.save">Save file</button>
			</span>
		</header>
		<div class="editor-code">
			<pre class="editor-code-view" data-role="rules-view" aria-hidden="true"></pre>
			<textarea class="editor-code-text" data-role="rules-text" spellcheck="false" wrap="off" aria-label="Rules file"></textarea>
		</div>
		<p class="editor-code-status" data-role="rules-status"></p>
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
		{ prop: "image", label: "Image", kind: "ref", list: "images" },
		{ prop: "color", label: "Colour", kind: "color" },
		{ prop: "colorEnvelope", label: "Colour envelope", kind: "ref", list: "envelopes" },
		{ prop: "colorEnvelopeOffset", label: "Envelope offset", kind: "number" },
	],
	quads: [{ prop: "image", label: "Image", kind: "ref", list: "images" }],
	sounds: [{ prop: "sound", label: "Sound", kind: "ref", list: "sounds" }],
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

/** What a layer is called on the screen: its name, or what kind of layer it is. */
function layerName(layer) {
	if (layer.name) {
		return layer.name;
	}
	const kind = layer.type === "tiles" ? layer.kind : layer.type;
	return LAYER_KIND_NAMES[kind] || kind;
}

/** The picture beside a layer's name, by what kind of layer it is. */
function layerIcon(layer) {
	if (layer.type === "quads") {
		return "layerQuads";
	}
	if (layer.type === "sounds") {
		return "layerSounds";
	}
	if (layer.kind === "game") {
		return "layerGame";
	}
	return layer.kind === "tiles" || layer.kind === undefined ? "layerTiles" : "layerPhysics";
}

const LAYER_KIND_NAMES = {
	tiles: "Tiles", game: "Game", front: "Front", tele: "Tele", switch: "Switch", speedup: "Speedup", tune: "Tune",
	quads: "Quads", sounds: "Sounds",
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

// The flags a tile carries, as the map file writes them.
const TILE_XFLIP = 1;
const TILE_YFLIP = 2;
const TILE_ROTATE = 8;

/**
 * Draws a brush - the tiles in hand, or the ones in a slot - onto a canvas,
 * as large as fits, the way the map draws them: flipped first, then turned a
 * quarter clockwise, exactly as the renderer's texture coordinates say.
 *
 * @param canvas Where to draw; cleared first.
 * @param brush What `editor.brush()` answers, or null for nothing in hand.
 * @param picture The tileset the brush draws with, as anything `drawImage`
 * takes, or null where there is no picture: then every tile is a grey square,
 * which is all that can be said about it.
 * @param tint `[r, g, b]` to multiply in, or null.
 * @return Whether anything was drawn.
 */
function drawBrush(canvas, brush, picture, tint) {
	const paint = canvas.getContext("2d");
	paint.clearRect(0, 0, canvas.width, canvas.height);
	if (brush === null || brush === undefined || brush.width <= 0 || brush.height <= 0) {
		return false;
	}
	const cell = Math.max(1, Math.floor(Math.min(canvas.width / brush.width, canvas.height / brush.height)));
	const left = Math.floor((canvas.width - cell * brush.width) / 2);
	const top = Math.floor((canvas.height - cell * brush.height) / 2);
	const side = picture === null ? 0 : (picture.width || picture.naturalWidth || 1024) / TILESET_SIDE;
	paint.imageSmoothingEnabled = false;
	let drawn = false;
	for (let y = 0; y < brush.height; y++) {
		for (let x = 0; x < brush.width; x++) {
			const at = (y * brush.width + x) * 2;
			const index = brush.tiles[at];
			const flags = brush.tiles[at + 1];
			if (index === 0) {
				continue;
			}
			drawn = true;
			if (picture === null) {
				paint.fillStyle = "#8a8a99";
				paint.fillRect(left + x * cell, top + y * cell, cell - (cell > 3 ? 1 : 0), cell - (cell > 3 ? 1 : 0));
				continue;
			}
			paint.save();
			paint.translate(left + x * cell + cell / 2, top + y * cell + cell / 2);
			if ((flags & TILE_ROTATE) !== 0) {
				paint.rotate(Math.PI / 2);
			}
			paint.scale((flags & TILE_XFLIP) !== 0 ? -1 : 1, (flags & TILE_YFLIP) !== 0 ? -1 : 1);
			paint.drawImage(picture, (index % TILESET_SIDE) * side, Math.floor(index / TILESET_SIDE) * side, side, side,
				-cell / 2, -cell / 2, cell, cell);
			paint.restore();
		}
	}
	if (drawn && tint !== null && tint !== undefined && !(tint[0] === 255 && tint[1] === 255 && tint[2] === 255)) {
		// The colour goes over what was drawn and the drawing's own alpha is
		// put back, the way the tileset is tinted.
		const shape = document.createElement("canvas");
		shape.width = canvas.width;
		shape.height = canvas.height;
		shape.getContext("2d").drawImage(canvas, 0, 0);
		paint.save();
		paint.globalCompositeOperation = "multiply";
		paint.fillStyle = `rgb(${tint[0]}, ${tint[1]}, ${tint[2]})`;
		paint.fillRect(0, 0, canvas.width, canvas.height);
		paint.globalCompositeOperation = "destination-in";
		paint.drawImage(shape, 0, 0);
		paint.restore();
	}
	return drawn;
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
		// Whether a frame has been asked for to scroll the tree once it has a height.
		this.revealPending = false;
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
		// Once a pen has been seen a finger pans rather than paints, because
		// the hand holding the pen rests on the glass. Somebody who paints
		// with a finger and points with the pen turns it off.
		this.penHoldsPaper = true;
		this.unusedSaidAt = -Infinity;
		this.cannotSaidAt = -Infinity;
		// Which layer the brush was last filled for, and whether somebody
		// emptied it on purpose since - see `ensureBrush`.
		this.brushFor = null;
		this.brushCleared = false;
		// The line over the map for the first stroke, while it is shown.
		this.hint = null;
		this.hinted = false;
		// What a long press says, where a pointer would have hovered.
		this.tip = null;
		// Which shape the box is in, once something tells us. Without a box
		// there are no areas to reshape and the panels stand in one column.
		this.shape = null;
		this.readonly = false;
		this.askingLayer = false;
		// What the pointer code offers for letting go of its selection, once
		// the pointer is wired - see `letGo`.
		this.pointerLetGo = null;
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
		// Every picture a tileset, a swatch or a slot has asked for, by where
		// it came from: `null` while it is on its way. A brush in a slot may
		// draw with a picture no layer on the screen uses.
		this.pictures = new Map();
		// Whether the panels listen for keys on the whole page themselves.
		this.keys = settings.keys;
		// Where each panel stands, this editor's own copy of the table: a
		// layout somebody drags together is theirs and not every editor's.
		this.places = PANEL_PLACES.map(place => Object.assign({}, place));
		// Which areas somebody opened or shut, by the name of the shape the
		// box had then: what suits a wide window does not suit a narrow one.
		this.open = {};
		// While the shape is being applied, what opens and shuts is the
		// size's doing, not the person's, and is not remembered as theirs.
		this.applyingShape = false;
		// The panel being carried to another area, while one is.
		this.carrying = null;
		// Whether a click is the tail of a drag that just ended.
		this.carriedJust = false;
		this.keepLater = null;
		this.settingsInput = null;
		// The switches of how a map is looked at are the program's, one set
		// per map, and the entities picture is only there once the program
		// runs: what somebody wants of them is held here and given to every
		// map the first time it is in front (see `giveView`).
		this.viewWanted = {};
		this.viewGiven = new Set();
		// The areas the panels were spread into, or null while they all stand
		// in one column.
		this.areas = null;
		// The box the panels were spread into, if they were.
		this.box = null;
		// Which panel each area shows, for the two that show one at a time.
		this.tab = { left: "props", dock: "envelopes", tiles: "tiles" };
		// How big a tile is drawn in the tileset beside the map: null fits the
		// column, a number is pixels a tile and the tileset scrolls.
		this.tileZoom = null;
		// What stands over the map while there is no map.
		this.welcome = null;
		// Which of the four ways the pointer draws, while no modifier says
		// otherwise.
		this.tool = "paint";
		// Whether a `refresh` is already on its way - see `refreshSoon`.
		this.refreshQueued = false;
		// What each tile index means in the layer under the pointer, asked
		// once per index rather than on every move; and which tile the line
		// last spoke about, so that it is not written again for the same one.
		this.explained = new Map();
		this.hoverKey = null;
		// How much the line under the pointer says about a tile: "off", "dec"
		// or "hex".
		this.tileInfo = "hex";
		// The commands, and the keys that reach them.
		// A copy of each for these panels alone: the keys can be set, and two
		// editors on one page need not agree about them.
		this.commands = COMMANDS.map(command => Object.assign({}, command));
		this.keys_ = keyTable(this.commands);
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
				this.chooser.hidden = true;
				this.select({ group: what.group, layer: what.layer });
			}, { signal: this.stopping.signal });
			this.chooser.append(row);
		}
		// Beside the spot, and inside the map: measured once it is shown,
		// because a hidden list has no size.
		this.chooser.style.left = "0px";
		this.chooser.style.top = "0px";
		this.chooser.hidden = false;
		const size = this.chooser.getBoundingClientRect();
		const holder = this.chooser.parentElement.getBoundingClientRect();
		let left = event.clientX - holder.left;
		let top = event.clientY - holder.top;
		if (left + size.width > holder.width - 4) {
			left = event.clientX - holder.left - size.width;
		}
		if (top + size.height > holder.height - 4) {
			top = event.clientY - holder.top - size.height;
		}
		this.chooser.style.left = `${Math.max(4, Math.min(left, holder.width - size.width - 4))}px`;
		this.chooser.style.top = `${Math.max(4, Math.min(top, holder.height - size.height - 4))}px`;
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
	 * The strip of open maps, in the top bar after the menu: what one is
	 * editing and whether it is saved are the two things nobody may have to
	 * look for.
	 */
	buildMaps(box) {
		this.maps = document.createElement("div");
		this.maps.className = "editor-maps";
		this.maps.dataset.role = "maps";
		this.maps.setAttribute("role", "tablist");
		this.maps.setAttribute("aria-label", "Open maps");
		const bar = this.part("bar");
		const space = bar === null ? null : bar.querySelector(".editor-bar-space");
		if (space !== null) {
			bar.insertBefore(this.maps, space);
		} else {
			box.prepend(this.maps);
		}
		this.refreshMaps();
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
				tab.tabIndex = id === now ? 0 : -1;
				tab.querySelector('[data-role="map-dot"]').hidden = !dirty;
				tab.querySelector(".editor-map-name").textContent = name;
				tab.title = `${name}${dirty ? " - not saved" : ""}`;
				tab.setAttribute("aria-label", `${name}${dirty ? " (changed)" : ""}`);
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
			tab.tabIndex = id === now ? 0 : -1;
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
			tab.setAttribute("aria-label", `${name.textContent}${dirty ? " (changed)" : ""}`);
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
		add.dataset.icon = "add";
		const command = this.commands.find(which => which.id === "file.new");
		add.title = command === undefined ? "New map" : commandTitle(command, this.keysShown());
		add.setAttribute("aria-label", "New map");
		add.addEventListener("click", () => this.run("file.new"), { signal: this.stopping.signal });
		this.maps.append(add);
		DDNetBase.paintIcons(this.maps);
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
			this.selection = this.defaultSelection();
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
		this.viewGiven.delete(id);
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
		const place = panel === null ? undefined : this.placeOf(panel.dataset.role);
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
		// A button that is folded away is unfolded first.
		const folded = found.closest("details");
		if (folded !== null) {
			folded.open = true;
		}
		return found;
	}

	/**
	 * Takes the shape the box says it is in.
	 *
	 * A side that became a drawer is shut, because a drawer lies over the map
	 * and an editor that opened with its map covered would be an editor whose
	 * first act is in the way. The inspector opens where there is room for it
	 * and shuts where there is not; whoever opened or shut it by hand keeps
	 * that until the shape changes again.
	 */
	applyShape(shape) {
		const before = this.shape;
		this.shape = shape;
		this.readonly = shape.readonly;
		this.applyingShape = true;
		const big = this.finger() ? "yes" : "no";
		this.root.dataset.big = big;
		if (this.areas !== null) {
			for (const area of Object.values(this.areas)) {
				area.dataset.big = big;
			}
		}
		// The dock is open where there is room to stack, and shuts again when
		// the room goes: what the size opened, the size may close.
		if (shape.stack && (before === null || !before.stack)) {
			this.dockOpen = true;
		} else if (!shape.stack && before !== null && before.stack) {
			this.dockOpen = false;
		}
		for (const side of ["left", "right"]) {
			const drawer = shape[side] !== "column";
			const wanted = side === "left" ? (!drawer && shape.inspector === "open") : !drawer;
			if (before === null || drawer !== (before[side] !== "column")
				|| (side === "left" && shape.inspector !== before.inspector)) {
				this.drawer[side] = drawer;
				this.showArea(side, wanted && !shape.readonly);
			}
			if (shape[side] === "none" || (side === "left" && shape.readonly)) {
				this.showArea(side, false);
			}
		}
		// Then what the person had open in a box of this shape, if they
		// have had one before.
		this.applyOpen(shape);
		this.applyingShape = false;
		this.applyTabs();
		this.applyTilesTab();
		this.applyDragged();
		this.refreshBar();
		this.refreshTiles();
	}

	/**
	 * Opens and shuts the areas the way they were left in a box of this
	 * shape. Columns only: a drawer is never remembered open.
	 * @param {object} shape The shape the box has now.
	 */
	applyOpen(shape) {
		const kept = this.open[shapeName(shape)];
		if (kept === undefined || this.areas === null) {
			return;
		}
		for (const side of ["left", "right"]) {
			if (shape[side] === "column" && typeof kept[side] === "boolean" && !(side === "left" && shape.readonly)) {
				this.showArea(side, kept[side]);
			}
		}
		if (typeof kept.dock === "boolean") {
			this.dockOpen = kept.dock;
		}
	}

	/**
	 * Whether this shape of box shows that button of the bar or the rail.
	 *
	 * `none` shows none of them, `looking` only what does not change the map,
	 * `few` only what a small box has room for, `icons` everything but the
	 * view switches, and the wide shapes show all of them.
	 */
	barShows(command) {
		const how = this.shape === null ? "labels" : this.shape.bar;
		if (how === "none") {
			return false;
		}
		if (command.touch === true && !this.finger()) {
			return false;
		}
		if (command.whenDrawer !== undefined && (this.shape === null || this.shape[command.whenDrawer] === "column")) {
			return false;
		}
		if (how === "looking") {
			return command.safe === true;
		}
		if (how === "few") {
			return command.always === true;
		}
		// `icons` is the compact bar: what is done to the map, without the
		// switches of how it is looked at - except the one button that
		// holds those switches, which is what stands in for them here.
		if (how === "icons") {
			return command.toggle !== true || command.always === true;
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
	placeholder="Type a command" aria-label="Command" spellcheck="false">
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
		this.menu.setAttribute("aria-label", "Menu");
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

	/**
	 * One command as a row of a menu.
	 *
	 * @param command The command.
	 * @param stays Whether the menu stays open after the row is pressed: a
	 * menu of switches is one where somebody throws three in a row.
	 */
	menuRow(command, stays) {
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
			if (stays === true) {
				this.run(command.id);
				if (this.context !== null) {
					this.refreshRows(this.context);
				}
				return;
			}
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
		this.refreshRows(this.menu);
	}

	/** The same, for the rows of any menu: the main one, a context menu, the view menu. */
	refreshRows(container) {
		for (const row of container.querySelectorAll("[data-command]")) {
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
		const edge = 4;
		// The anchor is a button, or the rectangle a button had before the
		// list it stood in was rebuilt under it - a click that selects the
		// row remakes the row - or a spot.
		const at = anchor instanceof Element ? anchor.getBoundingClientRect()
			: anchor !== null && anchor !== undefined && typeof anchor.bottom === "number" ? anchor : null;
		const spot = at === null && anchor !== null && anchor !== undefined ? anchor : null;
		const settle = size => {
			let left = room.width - size.width - 8;
			let top = 8;
			if (at !== null) {
				// Under the button, hanging from its left edge; from its
				// right edge where that would run off the side; above it
				// where there is no room below - a menu that covered its own
				// button would hide the one thing that says where it came
				// from.
				left = at.left - room.left;
				if (left + size.width > room.width - edge) {
					left = at.right - room.left - size.width;
				}
				top = at.bottom - room.top + edge;
				if (top + size.height > room.height - edge) {
					const above = at.top - room.top - edge - size.height;
					top = above >= edge ? above : Math.max(edge, room.height - size.height - edge);
				}
			} else if (spot !== null) {
				// At the spot, to its lower right; to the other side where
				// that would run off - the way a context menu opens beside
				// the pointer.
				left = spot.x - room.left;
				top = spot.y - room.top;
				if (left + size.width > room.width - edge) {
					left = spot.x - room.left - size.width;
				}
				if (top + size.height > room.height - edge) {
					top = spot.y - room.top - size.height;
				}
			}
			what.style.left = `${Math.max(edge, Math.min(left, room.width - size.width - edge))}px`;
			what.style.top = `${Math.max(edge, Math.min(top, room.height - size.height - edge))}px`;
		};
		// Measured at the corner, not where it was last time: a box that
		// stood near the right edge wrapped its text to fit and was taller
		// there than it will be at its new place, and a box placed by the
		// wrong height hangs sixteen pixels off its button.
		what.style.left = "0px";
		what.style.top = "0px";
		const size = what.getBoundingClientRect();
		settle(size);
		// Near the right edge it may have wrapped again and grown: then once
		// more, by the size it has there.
		const now = what.getBoundingClientRect();
		if (now.width !== size.width || now.height !== size.height) {
			settle(now);
		}
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
		this.context.setAttribute("aria-label", `Options for this ${kind}`);
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

	/**
	 * What can be added to the map, under the plus of the layer list: every
	 * kind of layer, and a group.
	 */
	showAddMenu(anchor) {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		this.closeContext();
		this.showMenu(false);
		this.showPalette(false);
		this.context = document.createElement("div");
		this.context.className = "editor-menu editor-context";
		this.context.dataset.role = "context";
		this.context.setAttribute("role", "menu");
		this.context.setAttribute("aria-label", "Add a layer or group");
		for (const command of this.commands) {
			if (command.menu === "Layer/Add layer" || command.id === "layer.addGroup") {
				this.context.append(this.menuRow(command));
			}
		}
		this.context.addEventListener("keydown", event => {
			if (event.key === "Escape") {
				event.stopPropagation();
				event.preventDefault();
				this.closeContext();
			}
		}, { signal: this.stopping.signal });
		home.append(this.context);
		this.placeAt(this.context, anchor);
		const first = this.context.querySelector("button:not(:disabled)");
		if (first !== null) {
			first.focus();
		}
	}

	/**
	 * The switches of how the map is looked at - grid, entities, high detail,
	 * animation, proof, tile info - and the zooms, behind one button of the
	 * bar rather than six: the bar is for what is used all the time.
	 */
	showViewMenu(anchor) {
		const home = this.overlayHome();
		if (home === null) {
			return;
		}
		if (this.context !== null && this.context.dataset.menu === "view") {
			this.closeContext();
			this.refreshBar();
			return;
		}
		this.closeContext();
		this.showMenu(false);
		this.showPalette(false);
		this.context = document.createElement("div");
		this.context.className = "editor-menu editor-context";
		this.context.dataset.role = "context";
		this.context.dataset.menu = "view";
		this.context.setAttribute("role", "menu");
		this.context.setAttribute("aria-label", "View");
		for (const command of this.commands) {
			if (command.view === true) {
				this.context.append(this.menuRow(command, command.pressed !== undefined));
			}
		}
		this.refreshRows(this.context);
		this.context.addEventListener("keydown", event => {
			if (event.key === "Escape") {
				event.stopPropagation();
				event.preventDefault();
				this.closeContext();
				this.refreshBar();
			}
		}, { signal: this.stopping.signal });
		home.append(this.context);
		this.placeAt(this.context, anchor);
		this.refreshBar();
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
				more.title = "More";
				more.setAttribute("aria-label", "Options for this row");
				more.addEventListener("click", event => {
					event.stopPropagation();
					// Selecting the row rebuilds the list, and may scroll it:
					// the menu hangs from the button of the row that took
					// this one's place, not from one that is no longer there.
					const index = Array.prototype.indexOf.call(rows, row);
					row.click();
					this.showContext(kind === null ? row.dataset.role : kind, this.moreButtonAt(role, kind, index, more));
				}, { signal: this.stopping.signal });
				row.append(more);
			}
		}
	}

	closeContext() {
		if (this.context === null) {
			return;
		}
		const wasView = this.context.dataset.menu === "view";
		this.context.remove();
		this.context = null;
		if (wasView) {
			this.refreshBar();
		}
	}

	/**
	 * The menu button of the row at an index of a list, as the list is now:
	 * the given button while it is still in the list, else the one of the
	 * row that was built in its place.
	 * @param {string} role Which list.
	 * @param {string|null} kind What its rows are, null for the layer tree.
	 * @param {number} index Which row.
	 * @param {HTMLElement} known The button as it was when it was pressed.
	 */
	moreButtonAt(role, kind, index, known) {
		if (known.isConnected) {
			return known;
		}
		const list = this.part(role);
		if (list === null || index < 0) {
			return known;
		}
		const rows = kind === null
			? list.querySelectorAll('[data-role="layer"], [data-role="group"]')
			: list.querySelectorAll("li");
		const row = rows[index];
		const button = row === undefined ? null : row.querySelector('[data-role="more"]');
		return button === null ? known : button;
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
			if (this.context !== null && !inside(this.context) && !(this.context.dataset.menu === "view" && onButtonFor("view.menu"))) {
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
		this.dialog.setAttribute("aria-label", "Keyboard shortcuts");
		const form = document.createElement("form");
		form.className = "editor-dialog-body";
		const head = document.createElement("h2");
		head.className = "editor-dialog-head";
		head.textContent = "What the keys do";
		const hint = document.createElement("p");
		hint.className = "editor-dialog-note";
		hint.textContent = "Press a key to give it another one. Delete takes it away, Escape leaves it as it was.";
		form.append(head, hint);
		const sheet = document.createElement("div");
		sheet.className = "editor-keys";
		sheet.dataset.role = "keys";
		form.append(sheet);
		const fill = () => {
			sheet.textContent = "";
			const groups = new Map();
			for (const command of this.commands) {
				// A command that has a key, or had one: taking a key away must
				// not take the row away with it, or it could not be given back.
				const had = COMMANDS.find(which => which.id === command.id);
				if ((command.keys === undefined || command.keys.length === 0) && (had.keys === undefined || had.keys.length === 0)) {
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
					const button = document.createElement("button");
					button.type = "button";
					button.className = "editor-keys-key";
					button.dataset.role = "key";
					button.dataset.command = command.id;
					// Every key it answers to, not only the first: the tool bar
					// has room for one and a sheet has room for all of them.
					button.textContent = command.keys === undefined || command.keys.length === 0
						? "none" : command.keys.map(keyLabel).join(" or ");
					button.setAttribute("aria-label", `${command.label}: ${button.textContent}. Press to change.`);
					button.addEventListener("click", () => this.listenForKey(button, command, fill),
						{ signal: this.stopping.signal });
					key.append(button);
					list.append(what, key);
				}
				sheet.append(list);
			}
		};
		fill();
		const row = document.createElement("div");
		row.className = "editor-dialog-buttons";
		const back = document.createElement("button");
		back.type = "button";
		back.className = "editor-small";
		back.dataset.role = "keys-reset";
		back.textContent = "Every key as it was";
		back.addEventListener("click", () => {
			this.resetKeys();
			fill();
		}, { signal: this.stopping.signal });
		const go = document.createElement("button");
		go.type = "submit";
		go.className = "editor-small editor-dialog-go";
		go.dataset.role = "dialog-go";
		go.textContent = "Done";
		row.append(back, go);
		form.append(row);
		this.dialog.append(form);
		form.addEventListener("submit", event => {
			event.preventDefault();
			this.closeDialog();
		}, { signal: this.stopping.signal });
		home.append(this.dialog);
		go.focus();
	}

	/**
	 * Waits on one row of the sheet for the key that is to reach it.
	 *
	 * The press is taken before anything else hears it - otherwise the key
	 * being set would also do what it does now. Escape leaves the row as it
	 * was, Delete or Backspace takes its key away, and a modifier on its own
	 * is waited past.
	 */
	listenForKey(button, command, done) {
		button.textContent = "Press a key\u2026";
		button.setAttribute("aria-pressed", "true");
		const stop = new AbortController();
		const signal = AbortSignal.any([stop.signal, this.stopping.signal]);
		document.addEventListener("keydown", event => {
			const name = keyName(event);
			if (name === "") {
				return;
			}
			event.preventDefault();
			event.stopImmediatePropagation();
			stop.abort();
			if (name !== "Escape") {
				const taken = name === "Delete" || name === "Backspace" ? [] : [name];
				const moved = this.setKeys(command.id, taken);
				if (moved !== null) {
					this.say(`${name} now does "${command.label}" rather than "${moved.label}"`);
				}
			}
			done();
		}, { capture: true, signal: signal });
		// A press anywhere else gives up waiting.
		document.addEventListener("pointerdown", event => {
			if (event.target !== button) {
				stop.abort();
				done();
			}
		}, { capture: true, signal: signal });
	}

	/**
	 * Gives a command these keys and no others.
	 *
	 * A key reaches one command, so a key that is given here is taken from
	 * whichever had it; that one is handed back so that it can be said. What
	 * differs from the table is told to the element, which keeps it where the
	 * page asked it to keep things.
	 *
	 * @return The command that lost a key to this one, or `null`.
	 */
	setKeys(id, keys) {
		const command = this.commands.find(which => which.id === id);
		if (command === undefined) {
			return null;
		}
		let moved = null;
		for (const other of this.commands) {
			if (other === command || other.keys === undefined) {
				continue;
			}
			const kept = other.keys.filter(key => !keys.includes(key));
			if (kept.length !== other.keys.length) {
				other.keys = kept;
				moved = other;
			}
		}
		command.keys = keys.slice();
		this.keys_ = keyTable(this.commands);
		this.refreshBar();
		this.tellKeys();
		return moved;
	}

	/** Every key back to the table's. */
	resetKeys() {
		for (const command of this.commands) {
			const table = COMMANDS.find(which => which.id === command.id);
			command.keys = table.keys === undefined ? undefined : table.keys.slice();
		}
		this.keys_ = keyTable(this.commands);
		this.refreshBar();
		this.tellKeys();
	}

	/** What differs from the table: command by command, the keys it has now. */
	changedKeys() {
		const changed = {};
		for (const command of this.commands) {
			const table = COMMANDS.find(which => which.id === command.id);
			const now = JSON.stringify(command.keys || []);
			if (now !== JSON.stringify(table.keys || [])) {
				changed[command.id] = command.keys || [];
			}
		}
		return changed;
	}

	/** Puts keys back that were kept from another visit. */
	applyKeys(changed) {
		if (changed === null || typeof changed !== "object") {
			return;
		}
		for (const [id, keys] of Object.entries(changed)) {
			if (Array.isArray(keys) && keys.every(key => typeof key === "string")) {
				const command = this.commands.find(which => which.id === id);
				if (command !== undefined) {
					command.keys = keys.slice();
				}
			}
		}
		this.keys_ = keyTable(this.commands);
		this.refreshBar();
	}

	tellKeys() {
		const target = this.box !== null && this.box !== undefined ? this.box : this.root;
		if (target !== null) {
			target.dispatchEvent(new CustomEvent("editor-keys", { detail: { keys: this.changedKeys() } }));
		}
	}

	/** Which entities sheet, chosen from the ones there are. */
	askEntitiesImage() {
		const names = ENTITIES_SHEETS;
		this.askFor("Entities picture", [
			{ name: "what", kind: "note", label: "What physics layers are drawn with - the map is the same whichever it is." },
			{ name: "sheet", label: "Picture", kind: "pick", value: this.editor.entitiesImage(),
				choices: Object.entries(names).map(([value, label]) => ({ value, label })) },
		], answer => {
			this.editor.entitiesImage(answer.sheet);
			this.tilesetSource = undefined;
			this.refresh();
			this.keepSettings();
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
	 * The top bar and the rail, out of the list of commands. Nothing here
	 * knows what any of the buttons do; a button is a command that said it
	 * wanted one. The rail holds the tools and, under them, what is in the
	 * brush; the bar holds the menu, the open maps, and what is done to or
	 * seen of the map.
	 */
	buildBar() {
		const bar = this.root.querySelector('[data-role="bar"]');
		const rail = this.root.querySelector('[data-role="rail"]');
		const button = (command, className) => {
			const made = document.createElement("button");
			made.type = "button";
			made.className = className;
			made.dataset.role = commandRole(command);
			made.dataset.command = command.id;
			made.dataset.icon = command.icon;
			made.dataset.bar = "";
			made.title = commandTitle(command, this.keysShown());
			made.setAttribute("aria-label", command.label);
			if (command.pressed !== undefined) {
				made.setAttribute("aria-pressed", "false");
			}
			made.addEventListener("click", () => this.run(command.id), { signal: this.stopping.signal });
			return made;
		};
		// The menu first, then the maps that are open (put there once there is
		// a box to know them), then everything else, with a gap wherever the
		// group changes.
		const menu = this.commands.find(command => command.id === "menu.open");
		if (menu !== undefined) {
			bar.append(button(menu, "editor-button"));
		}
		const spacer = document.createElement("span");
		spacer.className = "editor-bar-space";
		bar.append(spacer);
		let group = null;
		for (const command of this.commands) {
			if (command.bar !== true || command === menu) {
				continue;
			}
			if (group !== null && command.group !== group) {
				const gap = document.createElement("span");
				gap.className = "editor-bar-gap";
				gap.dataset.after = group;
				bar.append(gap);
			}
			group = command.group;
			bar.append(button(command, "editor-button"));
		}
		for (const command of this.commands) {
			if (command.rail !== true) {
				continue;
			}
			const made = button(command, "editor-rail-button");
			made.setAttribute("role", "radio");
			const key = document.createElement("kbd");
			key.className = "editor-rail-key";
			key.setAttribute("aria-hidden", "true");
			made.append(key);
			rail.append(made);
		}
		// What is in hand, as a picture. Pressing it opens the big chooser: the
		// picture is small, and the thing one wants when looking at it is to
		// change it.
		const swatch = document.createElement("button");
		swatch.type = "button";
		swatch.className = "editor-swatch";
		swatch.dataset.role = "brush-swatch";
		swatch.title = "Brush";
		swatch.setAttribute("aria-label", "Brush");
		swatch.innerHTML = '<canvas class="editor-swatch-picture" data-role="brush-picture" width="32" height="32"></canvas><span class="editor-swatch-size" data-role="brush-swatch-size"></span>';
		swatch.addEventListener("click", () => this.run("picker.show"), { signal: this.stopping.signal });
		rail.append(swatch);
		const gap = document.createElement("span");
		gap.className = "editor-rail-space";
		rail.append(gap);
		// The inspector's switch, at the foot of the rail where Photopea keeps
		// its colours: the one panel that is not always there wants a button
		// that always is.
		const inspector = this.commands.find(command => command.id === "area.left");
		if (inspector !== undefined) {
			rail.append(button(Object.assign({}, inspector, { icon: "properties" }), "editor-rail-button editor-rail-foot"));
		}
	}

	/** The key a tool answers to, written on its button once a keyboard has been seen. */
	refreshRailKeys() {
		for (const key of this.parts("rail").flatMap(rail => [...rail.querySelectorAll(".editor-rail-key")])) {
			const command = this.commands.find(which => which.id === key.parentElement.dataset.command);
			key.textContent = command === undefined ? "" : this.keyText(command);
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
		// A switch of the view or a setting is somebody's own, and kept.
		if (command.group === "View" || command.group === "Settings") {
			this.keepSettings();
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
	 * Moves the panels out of the one column and into the areas of a box: the
	 * rail and the inspector to the left, the layers and the tileset to the
	 * right, the envelopes and the history below, the bar above and the
	 * status line at the bottom. Nothing about a panel changes; only where it
	 * stands.
	 */
	spread(areas, box) {
		// Taken before anything moves: `part` looks in the column, and the
		// column is about to be empty.
		const bar = this.part("bar");
		const rail = this.part("rail");
		const status = this.part("status");
		const hover = this.part("hover");
		const panels = new Map(this.places.map(place => [place.role, this.part(place.role)]));
		this.areas = areas;
		// What now holds every panel, for whoever asks the panels where they
		// are: with the panels spread over six areas there is no one node that
		// is "the panels" any more - the box is.
		this.box = box === undefined ? null : box;
		areas.toolbar.append(bar);
		if (areas.rail !== undefined) {
			areas.rail.append(rail);
		} else {
			areas.left.append(rail);
		}
		// The status line: what is under the pointer on the left, then what
		// just happened, then what is being worked in; on the right the four
		// strips below the map as switches, and the zoom.
		const line = document.createElement("div");
		line.className = "editor-statusline";
		line.innerHTML = '<span data-role="status-layer"></span><span data-role="status-brush"></span>';
		const hint = document.createElement("span");
		hint.className = "editor-tool-hint";
		hint.dataset.role = "tool-hint";
		const space = document.createElement("span");
		space.className = "editor-bar-space";
		const docks = document.createElement("div");
		docks.className = "editor-dock-tabs";
		docks.dataset.role = "dock-tabs";
		docks.setAttribute("role", "tablist");
		docks.setAttribute("aria-label", "Below the map");
		this.makeSwitches(docks);
		const zoom = document.createElement("span");
		zoom.dataset.role = "status-zoom";
		zoom.className = "editor-status-zoom";
		areas.status.append(hover, status, line, hint, space, docks, zoom);
		for (const area of ["left", "right", "dock"]) {
			const here = this.places.filter(place => place.area === area);
			if (area === "left") {
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
		this.wireHandles();
		DDNetBase.paintIcons(areas.status);
		DDNetBase.paintIcons(areas.dock);
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
			this.keepLayoutSoon();
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
					this.keepLayoutSoon();
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
			this.keepLayoutSoon();
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
			this.wireHandle(button, place.role);
			strip.append(button);
		}
		if (area === "left") {
			const shut = document.createElement("button");
			shut.type = "button";
			shut.className = "editor-icon-button editor-tabs-close";
			shut.dataset.role = "inspector-close";
			shut.dataset.icon = "close";
			shut.title = "Close";
			shut.setAttribute("aria-label", "Close the inspector");
			shut.addEventListener("click", () => this.showArea("left", false), { signal: this.stopping.signal });
			strip.append(shut);
			DDNetBase.paintIcons(strip);
		}
		return strip;
	}

	/**
	 * The parts of the tile panel that are only there when they have
	 * something to say: the rules that paint by themselves, for a layer
	 * whose picture has rules.
	 */
	applyTilesTab() {
		const there = this.automapThere === true;
		const section = this.part("automap-section");
		if (section !== null) {
			section.hidden = !there;
		}
	}

	/** Puts one of an area's panels in front, and opens the area if it was shut. */
	showTab(area, tab) {
		this.tab[area] = tab;
		if (area === "dock") {
			this.dockOpen = true;
		}
		this.applyTabs();
		this.keepLayoutSoon();
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
			const here = this.places.filter(place => place.area === area && place.tab !== undefined);
			const has = place => this.panelShown.get(place.role) !== false;
			if (!here.some(place => place.tab === this.tab[area] && has(place))) {
				const first = here.find(has);
				this.tab[area] = first === undefined ? null : first.tab;
			}
			// With room enough the envelopes stand beside whatever else the
			// strip shows, and the history under whatever the inspector
			// shows, instead of behind a tab.
			const roomy = this.shape !== null && this.shape.stack === true;
			const always = !roomy ? null : area === "dock" ? "envelopes" : area === "left" ? "history" : null;
			if (always !== null && this.tab[area] === always) {
				const next = here.find(place => place.tab !== always && has(place));
				this.tab[area] = next === undefined ? always : next.tab;
			}
			const buttons = this.parts(`${TABBED_AREAS[area]}-tab`);
			for (const place of here) {
				const panel = this.part(place.role);
				const shown = has(place) && (place.tab === this.tab[area] || place.tab === always);
				if (panel !== null) {
					panel.hidden = !shown;
					panel.classList.toggle("editor-panel-always", place.tab === always);
				}
				const button = buttons.find(one => one.dataset.tab === place.tab);
				if (button !== undefined) {
					button.hidden = area === "left" && place.tab === always;
					button.disabled = !has(place);
					const inFront = place.tab === this.tab[area] && (area !== "dock" || this.dockOpen);
					button.setAttribute("aria-selected", inFront ? "true" : "false");
					button.tabIndex = place.tab === this.tab[area] ? 0 : -1;
				}
			}
		}
		// A panel in an area without tabs is shown whenever it has something
		// to show - also one that was just carried there from behind a tab.
		for (const place of this.places) {
			if (TABBED_AREAS[place.area] === undefined) {
				const panel = this.part(place.role);
				if (panel !== null) {
					panel.hidden = this.panelShown.get(place.role) === false;
					panel.classList.remove("editor-panel-always");
				}
			}
		}
		const dock = this.areas.dock;
		if (dock !== undefined) {
			// A strip with no panel in it is no strip.
			const anything = this.places.some(place => place.area === "dock" && this.panelShown.get(place.role) !== false);
			dock.classList.toggle("editor-dock-shut", !this.dockOpen || !anything);
			dock.hidden = !this.dockOpen || !anything;
			this.applyDragged();
		}
		// The switches in the status line say which panel is being looked at,
		// wherever it stands - and are one stop for Tab: the one that is on,
		// or else the first.
		const switches = this.parts("dock-tab");
		let stop = null;
		for (const button of switches) {
			const place = this.places.find(where => where.tab === button.dataset.tab);
			const on = place !== undefined && this.panelInFront(place.role);
			button.setAttribute("aria-pressed", on ? "true" : "false");
			button.setAttribute("aria-selected", on ? "true" : "false");
			button.disabled = place === undefined || this.panelShown.get(place.role) === false;
			if (on && stop === null) {
				stop = button;
			}
		}
		for (const button of switches) {
			button.tabIndex = button === (stop === null ? switches[0] : stop) ? 0 : -1;
		}
		this.noteOpen();
	}

	/**
	 * Writes down which areas are open, under the name of the shape the box
	 * has - unless it is the shape itself that is opening and shutting them.
	 * A drawer is not written down: one that opened with the map covered
	 * would be an editor whose first act is in the way.
	 */
	noteOpen() {
		if (this.shape === null || this.applyingShape || this.areas === null) {
			return;
		}
		const name = shapeName(this.shape);
		const usual = this.defaultOpen(this.shape);
		const now = {};
		for (const side of ["left", "right"]) {
			if (this.shape[side] === "column") {
				now[side] = this.areaShown(side);
			}
		}
		now.dock = this.dockOpen;
		// Only what differs from what the shape would do by itself is
		// somebody's own; the rest is left to the shape, so that a better
		// default in a later editor is not overruled by a remembered one.
		const own = {};
		for (const [area, open] of Object.entries(now)) {
			if (open !== usual[area]) {
				own[area] = open;
			}
		}
		const was = this.open[name];
		const next = Object.keys(own).length === 0 ? undefined : own;
		if (JSON.stringify(was) !== JSON.stringify(next)) {
			if (next === undefined) {
				delete this.open[name];
			} else {
				this.open[name] = next;
			}
			this.keepLayoutSoon();
		}
	}

	/**
	 * What a shape opens by itself: the columns where they are columns, the
	 * inspector only where there is room for it, the strip where there is
	 * room to stack.
	 * @param {object} shape The shape of the box.
	 */
	defaultOpen(shape) {
		return {
			left: shape.left === "column" && shape.inspector === "open" && !shape.readonly,
			right: shape.right === "column" && !shape.readonly,
			dock: shape.stack === true,
		};
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
		const place = this.placeOf(role);
		if (place === undefined || TABBED_AREAS[place.area] === undefined) {
			return true;
		}
		return this.tab[place.area] === place.tab;
	}

	/** Where a panel stands, by the panel's name. */
	placeOf(role) {
		return this.places.find(where => where.role === role);
	}

	/**
	 * The switches of the status line, one per panel that has a number:
	 * each opens its panel where it stands and shuts it again, in the order
	 * the numbers say rather than the order the panels happen to have.
	 * @param {HTMLElement} docks The strip the switches go into; emptied first.
	 */
	makeSwitches(docks) {
		docks.textContent = "";
		for (const place of this.places.filter(where => where.status !== undefined).sort((one, other) => one.status - other.status)) {
			const button = document.createElement("button");
			button.type = "button";
			button.className = "editor-dock-tab";
			button.dataset.role = "dock-tab";
			button.dataset.tab = place.tab;
			button.dataset.command = `panel.${place.tab}`;
			button.setAttribute("role", "tab");
			button.textContent = place.name;
			button.title = `${place.name}; press again to close`;
			button.addEventListener("click", () => this.togglePanel(place.role), { signal: this.stopping.signal });
			docks.append(button);
		}
	}

	/**
	 * Carries a panel to another area, and puts it in front there.
	 *
	 * A panel that goes below the map gets a switch in the status line if it
	 * has none, because the strip has no tabs of its own: the switches are
	 * its tabs. One that leaves keeps its switch, which goes on opening it
	 * wherever it stands.
	 * @param {string} role The panel, by its name - `tiles-panel` and the like.
	 * @param {string} area Where to: `left`, `right` or `dock`.
	 * @returns {boolean} Whether there was such a panel and such an area.
	 */
	movePanel(role, area) {
		const place = this.placeOf(role);
		if (place === undefined || !PANEL_AREAS.includes(area) || this.areas === null) {
			return false;
		}
		if (place.area !== area) {
			// Last in its new area: what was just put somewhere stands at
			// the end of what was there.
			this.places.splice(this.places.indexOf(place), 1);
			this.places.push(place);
			place.area = area;
		}
		if (area === "dock" && place.status === undefined) {
			place.status = 1 + Math.max(0, ...this.places.map(where => where.status || 0));
		}
		this.placePanels();
		if (TABBED_AREAS[area] !== undefined) {
			this.showTab(area, place.tab);
		}
		this.showArea(area, true);
		this.keepLayoutSoon();
		return true;
	}

	/**
	 * Puts every panel where the table says, in the table's order, and
	 * remakes the tabs and the switches, which are made from the table.
	 */
	placePanels() {
		if (this.areas === null) {
			return;
		}
		for (const area of PANEL_AREAS) {
			const body = this.areas[area].querySelector(".editor-area-body");
			for (const place of this.places.filter(where => where.area === area)) {
				const panel = this.part(place.role);
				if (panel !== null && body !== null) {
					body.append(panel);
				}
			}
		}
		const strip = this.areas.left.querySelector('[data-role="structure-tabs"]');
		if (strip !== null) {
			strip.replaceWith(this.makeTabs("left", this.places.filter(where => where.area === "left")));
		}
		const docks = this.part("dock-tabs");
		if (docks !== null) {
			this.makeSwitches(docks);
		}
		this.wireHandles();
		this.applyTabs();
		this.refreshBar();
		this.refreshTiles();
	}

	/** Makes the head of every panel a handle it can be carried by. */
	wireHandles() {
		for (const place of this.places) {
			const panel = this.part(place.role);
			const head = panel === null ? null : panel.querySelector(".editor-panel-head");
			if (head !== null && head.dataset.carry === undefined) {
				this.wireHandle(head, place.role);
			}
		}
	}

	/**
	 * Makes a thing a handle: dragged eight pixels, it carries its panel,
	 * and the three areas offer themselves to drop it in. A shorter drag is
	 * a click, and does what a click on the thing did.
	 * @param {HTMLElement} handle The tab or the head of the panel.
	 * @param {string} role The panel it carries.
	 */
	wireHandle(handle, role) {
		handle.dataset.carry = role;
		handle.addEventListener("pointerdown", event => this.beginCarry(event, handle, role), { signal: this.stopping.signal });
		// The click that ends a drag is the drag's, not the tab's.
		handle.addEventListener("click", event => {
			if (this.carriedJust) {
				event.stopImmediatePropagation();
				event.preventDefault();
			}
		}, { capture: true, signal: this.stopping.signal });
	}

	beginCarry(event, handle, role) {
		if (event.button !== 0 || !event.isPrimary || this.readonly || this.carrying !== null) {
			return;
		}
		// A button in a head - the plus of the layer list - is a button.
		const button = event.target.closest("button");
		if (button !== null && button !== handle) {
			return;
		}
		const start = { x: event.clientX, y: event.clientY, id: event.pointerId };
		const stop = new AbortController();
		let carrying = false;
		// Captured from the start, or the first move beyond the handle would
		// go to whatever lies there and the handle would never hear of it.
		try {
			handle.setPointerCapture(start.id);
		} catch (error) {
			// A pointer that is already gone: there is nothing to carry with.
		}
		const move = moved => {
			if (!carrying) {
				if (Math.hypot(moved.clientX - start.x, moved.clientY - start.y) < 8) {
					return;
				}
				carrying = true;
				this.showDropZones(role);
			}
			this.moveCarry(moved.clientX, moved.clientY);
		};
		const end = ended => {
			stop.abort();
			try {
				handle.releasePointerCapture(start.id);
			} catch (error) {
				// It had already gone.
			}
			if (!carrying) {
				return;
			}
			const zone = ended.type === "pointerup" ? this.zoneAt(ended.clientX, ended.clientY) : null;
			this.hideDropZones();
			this.carriedJust = true;
			setTimeout(() => {
				this.carriedJust = false;
			}, 0);
			if (zone !== null) {
				this.movePanel(role, zone);
			}
		};
		handle.addEventListener("pointermove", move, { signal: stop.signal });
		handle.addEventListener("pointerup", end, { signal: stop.signal });
		handle.addEventListener("pointercancel", end, { signal: stop.signal });
		this.stopping.signal.addEventListener("abort", () => stop.abort(), { signal: stop.signal });
	}

	/**
	 * The three places a carried panel can be dropped, drawn over the
	 * editor: an area that is open is its own zone; one that is shut is a
	 * strip along its edge of the map.
	 * @param {string} role The panel being carried, for the label.
	 */
	showDropZones(role) {
		const home = this.overlayHome();
		if (home === null || this.areas === null) {
			return;
		}
		const room = home.getBoundingClientRect();
		const map = this.editor.canvas.getBoundingClientRect();
		const strip = 64;
		const zones = [];
		for (const area of PANEL_AREAS) {
			const box = this.areas[area];
			let at;
			if (box !== undefined && !box.hidden) {
				const k = box.getBoundingClientRect();
				at = { left: k.left, top: k.top, width: k.width, height: k.height };
			} else if (area === "left") {
				at = { left: map.left, top: map.top, width: strip, height: map.height };
			} else if (area === "right") {
				at = { left: map.right - strip, top: map.top, width: strip, height: map.height };
			} else {
				at = { left: map.left, top: map.bottom - strip, width: map.width, height: strip };
			}
			const zone = document.createElement("div");
			zone.className = "editor-drop";
			zone.dataset.role = "drop-zone";
			zone.dataset.area = area;
			zone.textContent = AREA_NAMES[area];
			zone.style.left = `${at.left - room.left}px`;
			zone.style.top = `${at.top - room.top}px`;
			zone.style.width = `${at.width}px`;
			zone.style.height = `${at.height}px`;
			home.append(zone);
			zones.push({ area: area, zone: zone, left: at.left, top: at.top, right: at.left + at.width, bottom: at.top + at.height });
		}
		const ghost = document.createElement("div");
		ghost.className = "editor-carry";
		ghost.dataset.role = "carry";
		const place = this.placeOf(role);
		ghost.textContent = place === undefined ? role : place.name;
		home.append(ghost);
		this.carrying = { role: role, zones: zones, ghost: ghost, room: room };
		if (this.box !== null && this.box !== undefined) {
			this.box.dataset.carrying = "yes";
		}
	}

	moveCarry(x, y) {
		if (this.carrying === null) {
			return;
		}
		const over = this.zoneAt(x, y);
		for (const zone of this.carrying.zones) {
			zone.zone.dataset.over = zone.area === over ? "yes" : "no";
		}
		this.carrying.ghost.style.left = `${x - this.carrying.room.left + 12}px`;
		this.carrying.ghost.style.top = `${y - this.carrying.room.top + 12}px`;
	}

	/** Which area's zone a point of the page lies in, or null. */
	zoneAt(x, y) {
		if (this.carrying === null) {
			return null;
		}
		const hit = this.carrying.zones.find(zone => x >= zone.left && x < zone.right && y >= zone.top && y < zone.bottom);
		return hit === undefined ? null : hit.area;
	}

	hideDropZones() {
		if (this.carrying === null) {
			return;
		}
		for (const zone of this.carrying.zones) {
			zone.zone.remove();
		}
		this.carrying.ghost.remove();
		this.carrying = null;
		if (this.box !== null && this.box !== undefined) {
			delete this.box.dataset.carrying;
		}
	}

	/**
	 * The same as carrying, for a keyboard or a finger that cannot drag:
	 * every panel with a choice of where it stands.
	 */
	askArrange() {
		const choices = PANEL_AREAS.map(area => ({ value: area, label: AREA_NAMES[area] }));
		this.askFor("Arrange panels", this.places.map(place => ({
			name: place.role, label: place.name, kind: "pick", value: place.area, choices: choices,
		})), answer => {
			for (const place of this.places.slice()) {
				if (answer[place.role] !== undefined && answer[place.role] !== place.area) {
					this.movePanel(place.role, answer[place.role]);
				}
			}
		}, { go: "Arrange" });
	}

	/** The layout as it is, the way it is kept and written out. */
	layoutState() {
		return {
			panels: this.places.map(place => ({ role: place.role, area: place.area })),
			tab: { left: this.tab.left, dock: this.tab.dock },
			sizes: Object.assign({}, this.dragged),
			open: JSON.parse(JSON.stringify(this.open)),
		};
	}

	/** The settings as they are: what the Settings menu and the View menu hold, and the keys. */
	settingsState() {
		// Without a map in front the program has no view to ask: what was
		// wanted stands in for it, and nothing is said where nothing is known.
		const open = this.editor.map >= 0;
		const view = open ? {
			entitiesImage: this.editor.entitiesImage(),
			grid: this.editor.grid(),
			entities: this.editor.entities(),
			highDetail: this.editor.highDetail(),
			animate: this.editor.animate(),
		} : this.viewWanted;
		return Object.assign({
			theme: this.scheme(),
			targets: this.targets(),
			brushColouring: this.brushColouring,
			penHoldsPaper: this.penHoldsPaper,
			allowUnused: this.editor.allowUnused(),
		}, JSON.parse(JSON.stringify(view)), {
			tileZoom: this.tileZoom,
			keys: this.changedKeys(),
		});
	}

	/**
	 * Gives a map the view somebody wants - grid, entities, detail, running
	 * envelopes, the entities picture - the first time it is in front.
	 * @param {number} map The map in front, -1 for none.
	 */
	giveView(map) {
		if (map < 0 || this.viewGiven.has(map)) {
			return;
		}
		this.viewGiven.add(map);
		const wanted = this.viewWanted;
		if (wanted.entitiesImage !== undefined && wanted.entitiesImage !== this.editor.entitiesImage()) {
			this.editor.entitiesImage(wanted.entitiesImage);
			this.tilesetSource = undefined;
		}
		for (const name of ["grid", "entities", "highDetail", "animate"]) {
			if (wanted[name] !== undefined) {
				this.editor[name](wanted[name], map);
			}
		}
	}

	/** Layout and settings together, as the file Export writes. */
	profile() {
		return { kind: SETTINGS_KIND, version: SETTINGS_VERSION, layout: this.layoutState(), settings: this.settingsState() };
	}

	/**
	 * Checks a file's worth of layout and settings before any of it is
	 * taken: a file with one bad value changes nothing. Keys the editor does
	 * not know are ignored, so a file from a later editor still opens.
	 * @param {unknown} data What the file held, parsed.
	 * @returns {{layout: object|null, settings: object|null}} What was checked, or throws with a reason.
	 */
	checkProfile(data) {
		if (data === null || typeof data !== "object" || Array.isArray(data)) {
			throw new Error("not a settings file");
		}
		if (data.version === undefined) {
			throw new Error("no version");
		}
		if (data.version !== SETTINGS_VERSION) {
			throw new Error(`version ${JSON.stringify(data.version)} is not ${SETTINGS_VERSION}`);
		}
		return {
			layout: data.layout === undefined ? null : this.checkLayout(data.layout),
			settings: data.settings === undefined ? null : this.checkSettings(data.settings),
		};
	}

	checkLayout(layout) {
		if (layout === null || typeof layout !== "object" || Array.isArray(layout)) {
			throw new Error("the layout is not an object");
		}
		const out = { panels: [], tab: {}, sizes: {}, open: {} };
		if (layout.panels !== undefined) {
			if (!Array.isArray(layout.panels)) {
				throw new Error("the panels are not a list");
			}
			const seen = new Set();
			for (const entry of layout.panels) {
				if (entry === null || typeof entry !== "object" || typeof entry.role !== "string" || typeof entry.area !== "string") {
					throw new Error("a panel without a name or an area");
				}
				if (!PANEL_PLACES.some(place => place.role === entry.role)) {
					throw new Error(`no panel is called ${entry.role}`);
				}
				if (!PANEL_AREAS.includes(entry.area)) {
					throw new Error(`no area is called ${entry.area}`);
				}
				if (seen.has(entry.role)) {
					throw new Error(`${entry.role} is listed twice`);
				}
				seen.add(entry.role);
				out.panels.push({ role: entry.role, area: entry.area });
			}
		}
		if (layout.tab !== undefined) {
			if (layout.tab === null || typeof layout.tab !== "object") {
				throw new Error("the tabs are not an object");
			}
			for (const area of Object.keys(TABBED_AREAS)) {
				if (layout.tab[area] !== undefined) {
					if (typeof layout.tab[area] !== "string") {
						throw new Error(`the ${area} tab is not a name`);
					}
					out.tab[area] = layout.tab[area];
				}
			}
		}
		if (layout.sizes !== undefined) {
			if (layout.sizes === null || typeof layout.sizes !== "object") {
				throw new Error("the sizes are not an object");
			}
			for (const area of PANEL_AREAS) {
				const size = layout.sizes[area];
				if (size === undefined) {
					continue;
				}
				if (size !== null && (typeof size !== "number" || !Number.isFinite(size))) {
					throw new Error(`the ${area} size is not a number`);
				}
				out.sizes[area] = size === null ? null : Math.max(DRAG_LIMITS[area].least, Math.min(DRAG_LIMITS[area].most, Math.round(size)));
			}
		}
		if (layout.open !== undefined) {
			if (layout.open === null || typeof layout.open !== "object") {
				throw new Error("what is open is not an object");
			}
			for (const [name, kept] of Object.entries(layout.open)) {
				if (kept === null || typeof kept !== "object") {
					throw new Error(`what is open at ${name} is not an object`);
				}
				const one = {};
				for (const area of PANEL_AREAS) {
					if (kept[area] !== undefined) {
						if (typeof kept[area] !== "boolean") {
							throw new Error(`whether the ${area} is open at ${name} is not yes or no`);
						}
						one[area] = kept[area];
					}
				}
				out.open[name] = one;
			}
		}
		return out;
	}

	checkSettings(settings) {
		if (settings === null || typeof settings !== "object" || Array.isArray(settings)) {
			throw new Error("the settings are not an object");
		}
		const out = {};
		const oneOf = (name, allowed) => {
			if (settings[name] !== undefined) {
				if (!allowed.includes(settings[name])) {
					throw new Error(`${name} is ${JSON.stringify(settings[name])}, not one of ${allowed.join(", ")}`);
				}
				out[name] = settings[name];
			}
		};
		const yesNo = name => {
			if (settings[name] !== undefined) {
				if (typeof settings[name] !== "boolean") {
					throw new Error(`${name} is not yes or no`);
				}
				out[name] = settings[name];
			}
		};
		const number = (name, least, most) => {
			if (settings[name] !== undefined) {
				const value = settings[name];
				if (typeof value !== "number" || !Number.isFinite(value) || value < least || value > most) {
					throw new Error(`${name} is not a number from ${least} to ${most}`);
				}
				out[name] = value;
			}
		};
		oneOf("theme", ["dark", "light"]);
		oneOf("targets", ["auto", "big", "small"]);
		yesNo("brushColouring");
		yesNo("penHoldsPaper");
		yesNo("allowUnused");
		yesNo("highDetail");
		yesNo("animate");
		oneOf("entitiesImage", Object.keys(ENTITIES_SHEETS));
		number("grid", 0, 1024);
		number("entities", 0, 100);
		oneOf("tileZoom", TILE_ZOOMS);
		if (settings.keys !== undefined) {
			if (settings.keys === null || typeof settings.keys !== "object" || Array.isArray(settings.keys)) {
				throw new Error("the keys are not an object");
			}
			for (const [id, keys] of Object.entries(settings.keys)) {
				if (!Array.isArray(keys) || !keys.every(key => typeof key === "string")) {
					throw new Error(`the keys of ${id} are not a list of names`);
				}
			}
			out.keys = settings.keys;
		}
		return out;
	}

	/**
	 * Takes a checked layout: every panel to its area in the file's order,
	 * then the tabs, the sizes and what is open.
	 * @param {object|null} layout What `checkLayout` returned.
	 */
	applyLayout(layout) {
		if (layout === null) {
			return;
		}
		if (layout.panels.length > 0) {
			const next = [];
			for (const entry of layout.panels) {
				const place = this.placeOf(entry.role);
				place.area = entry.area;
				if (entry.area === "dock" && place.status === undefined) {
					place.status = 1 + Math.max(0, ...this.places.map(where => where.status || 0));
				}
				next.push(place);
			}
			for (const place of this.places) {
				if (!next.includes(place)) {
					next.push(place);
				}
			}
			this.places = next;
		}
		Object.assign(this.tab, layout.tab);
		Object.assign(this.dragged, layout.sizes);
		Object.assign(this.open, layout.open);
		// Nothing is written down while the panels are put in place: the
		// areas still stand as the shape left them, and that is not
		// anybody's choice.
		this.applyingShape = true;
		this.placePanels();
		if (this.shape !== null) {
			this.applyOpen(this.shape);
		}
		this.applyingShape = false;
		this.applyTabs();
		this.applyDragged();
		this.refreshBar();
	}

	/**
	 * Takes checked settings, each through the same door the menu uses.
	 * @param {object|null} settings What `checkSettings` returned.
	 */
	applySettings(settings) {
		if (settings === null) {
			return;
		}
		if (settings.theme !== undefined) {
			this.scheme(settings.theme);
		}
		if (settings.targets !== undefined) {
			this.targets(settings.targets);
		}
		if (settings.brushColouring !== undefined) {
			this.brushColouring = settings.brushColouring;
		}
		if (settings.penHoldsPaper !== undefined) {
			this.penHoldsPaper = settings.penHoldsPaper;
		}
		if (settings.allowUnused !== undefined) {
			this.editor.allowUnused(settings.allowUnused);
		}
		for (const name of ["entitiesImage", "grid", "entities", "highDetail", "animate"]) {
			if (settings[name] !== undefined) {
				this.viewWanted[name] = settings[name];
			}
		}
		// The map in front takes it now; one that is not yet in front, when
		// it comes.
		this.viewGiven.clear();
		this.giveView(this.editor.map);
		if (settings.tileZoom !== undefined) {
			this.tileZoom = settings.tileZoom;
			if (this.remembers()) {
				try {
					localStorage.setItem(TILE_ZOOM_STORAGE, String(this.tileZoom));
				} catch (error) {
					// A browser that keeps nothing.
				}
			}
		}
		if (settings.keys !== undefined) {
			// The file's keys replace what was changed here, not add to it.
			for (const command of this.commands) {
				const table = COMMANDS.find(which => which.id === command.id);
				command.keys = (table.keys || []).slice();
			}
			this.applyKeys(settings.keys);
			this.tellKeys();
		}
		this.refresh();
		this.refreshOverlay();
	}

	/** Keeps the layout in the browser, if the element was asked to remember. */
	keepLayout() {
		if (this.keepLater !== null) {
			clearTimeout(this.keepLater);
			this.keepLater = null;
		}
		if (!this.remembers()) {
			return;
		}
		try {
			localStorage.setItem(LAYOUT_STORAGE, JSON.stringify(Object.assign({ version: SETTINGS_VERSION }, this.layoutState())));
		} catch (error) {
			// A browser that keeps nothing forgets the layout with the tab.
		}
	}

	/** The same, a moment later: the tabs are applied more often than they change. */
	keepLayoutSoon() {
		if (this.keepLater === null && this.remembers()) {
			this.keepLater = setTimeout(() => this.keepLayout(), 200);
		}
	}

	keepSettings() {
		const settings = this.settingsState();
		for (const name of ["entitiesImage", "grid", "entities", "highDetail", "animate"]) {
			if (settings[name] !== undefined && settings[name] !== null) {
				this.viewWanted[name] = settings[name];
			}
		}
		if (!this.remembers()) {
			return;
		}
		try {
			// The keys have a place of their own.
			delete settings.keys;
			localStorage.setItem(SETTINGS_STORAGE, JSON.stringify(Object.assign({ version: SETTINGS_VERSION }, settings)));
		} catch (error) {
			// A browser that keeps nothing.
		}
	}

	/**
	 * Takes back what was kept in the browser. Something kept that does not
	 * check - by an older editor, or by hand - is left alone and ignored.
	 */
	loadKept() {
		if (!this.remembers()) {
			return;
		}
		const read = key => {
			try {
				const text = localStorage.getItem(key);
				return text === null ? null : JSON.parse(text);
			} catch (error) {
				return null;
			}
		};
		const layout = read(LAYOUT_STORAGE);
		if (layout !== null) {
			try {
				this.applyLayout(this.checkProfile({ version: layout.version, layout: layout }).layout);
			} catch (error) {
				console.warn(`The kept layout was not taken: ${error.message}`);
			}
		}
		const settings = read(SETTINGS_STORAGE);
		if (settings !== null) {
			try {
				this.applySettings(this.checkProfile({ version: settings.version, settings: settings }).settings);
			} catch (error) {
				console.warn(`The kept settings were not taken: ${error.message}`);
			}
		}
	}

	/** Every panel back where it started, the sizes and what is open with it. */
	resetLayout() {
		this.places = PANEL_PLACES.map(place => Object.assign({}, place));
		this.dragged = { left: null, right: null, dock: null };
		this.open = {};
		this.tab.left = "props";
		this.tab.dock = "envelopes";
		this.applyingShape = true;
		this.placePanels();
		if (this.shape !== null && this.areas !== null) {
			const shape = this.shape;
			this.dockOpen = shape.stack === true;
			for (const side of ["left", "right"]) {
				const column = shape[side] === "column";
				const wanted = side === "left" ? (column && shape.inspector === "open") : column;
				this.showArea(side, wanted && !shape.readonly);
			}
		}
		this.applyingShape = false;
		this.applyTabs();
		this.applyDragged();
		this.refreshBar();
		if (this.remembers()) {
			try {
				localStorage.removeItem(LAYOUT_STORAGE);
			} catch (error) {
				// Nothing was kept.
			}
		}
		this.say("Layout reset");
	}

	/** Writes layout and settings to a file the browser hands out. */
	exportSettings() {
		const handout = document.createElement("a");
		const address = URL.createObjectURL(new Blob([JSON.stringify(this.profile(), null, "\t")], { type: "application/json" }));
		handout.href = address;
		handout.download = "ddnet-editor-settings.json";
		handout.click();
		setTimeout(() => URL.revokeObjectURL(address), 10000);
		this.say("Settings written to ddnet-editor-settings.json");
	}

	/** Asks for a settings file and takes it. */
	askImportSettings() {
		if (this.settingsInput === null) {
			this.settingsInput = document.createElement("input");
			this.settingsInput.type = "file";
			this.settingsInput.accept = ".json,application/json";
			this.settingsInput.hidden = true;
			this.settingsInput.dataset.role = "settings-file";
			this.settingsInput.addEventListener("change", () => {
				const file = this.settingsInput.files[0];
				this.settingsInput.value = "";
				if (file !== undefined && file !== null) {
					file.text().then(text => this.importSettings(text)).catch(error => this.say(`The file could not be read: ${error.message}`, "error"));
				}
			}, { signal: this.stopping.signal });
			this.root.append(this.settingsInput);
		}
		this.settingsInput.click();
	}

	/**
	 * Takes a settings file: checked whole before anything is applied, so
	 * a bad file changes nothing and says why.
	 * @param {string} text What the file held.
	 * @returns {boolean} Whether it was taken.
	 */
	importSettings(text) {
		let data;
		try {
			data = JSON.parse(text);
		} catch (error) {
			this.say("That is not a settings file: it is not JSON", "error");
			return false;
		}
		let checked;
		try {
			checked = this.checkProfile(data);
		} catch (error) {
			this.say(`The settings file was not taken: ${error.message}`, "error");
			return false;
		}
		this.applyLayout(checked.layout);
		this.applySettings(checked.settings);
		this.keepLayout();
		this.keepSettings();
		this.say("Layout and settings taken from the file");
		return true;
	}

	/**
	 * Whether a panel is the one being looked at: in front of its tabbed
	 * area and that area open, or simply shown in a stacked one.
	 */
	panelInFront(role) {
		const place = this.placeOf(role);
		if (place === undefined || this.areas === null) {
			return false;
		}
		if (place.area === "dock") {
			return this.dockOpen && this.tab.dock === place.tab;
		}
		if (place.tab !== undefined) {
			return this.areaShown(place.area) && this.tab[place.area] === place.tab;
		}
		const panel = this.part(role);
		return this.areaShown(place.area) && panel !== null && !panel.hidden;
	}

	/**
	 * Opens a panel where it stands, or shuts it if it was the one in front:
	 * what the switches in the status line do, and what the keys for the
	 * envelopes, the history, the settings and the rules do.
	 */
	togglePanel(role) {
		const place = this.placeOf(role);
		if (place === undefined || this.areas === null) {
			return;
		}
		if (this.panelInFront(role)) {
			if (place.area === "dock") {
				this.dockOpen = false;
				this.applyTabs();
			} else {
				this.showArea(place.area, false);
			}
			return;
		}
		if (place.area !== "dock") {
			this.showArea(place.area, true);
		}
		if (TABBED_AREAS[place.area] !== undefined) {
			this.showTab(place.area, place.tab);
		} else {
			const panel = this.part(role);
			if (panel !== null) {
				panel.scrollIntoView({ block: "nearest" });
			}
		}
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
		on("add-layer-menu", event => this.showAddMenu(event.currentTarget));
		on("layer-menu", event => {
			this.showContext(this.selection.layer < 0 ? "group" : "layer", event.currentTarget);
		});
		on("select-game", () => this.select(this.defaultSelection()));
		on("flip-x", () => this.run("brush.flipX"));
		on("flip-y", () => this.run("brush.flipY"));
		on("rotate", () => this.run("brush.rotate"));
		on("clear-brush", () => this.run("brush.clear"));
		on("tiles-big", () => this.run("picker.show"));
		on("tiles-zoom-in", () => this.zoomTiles(1));
		on("tiles-zoom-out", () => this.zoomTiles(-1));
		this.wireTileset();
		// The list of layers changes height after the fact - the tileset comes
		// when its picture has loaded and takes its share of the column - and
		// the row one is working on must not be pushed out of sight by that.
		const tree = this.part("tree");
		if (tree !== null && typeof ResizeObserver === "function") {
			const watcher = new ResizeObserver(() => this.revealSelected());
			watcher.observe(tree);
			signal.addEventListener("abort", () => watcher.disconnect());
		}
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
			this.collapsed.clear();
			this.selection = this.defaultSelection();
		} else if (type === "document" && this.editor.editing) {
			// A stroke under way: the map is half made and drawn by the
			// program itself. The marks over the canvas follow a dragged
			// quad; everything else waits for the stroke to end.
			this.refreshOverlay();
			return;
		}
		this.refreshSoon();
	}

	/**
	 * `refresh`, once, before the next frame - for the moments when several
	 * things say "changed" at once: the end of a stroke and the program's own
	 * word about it a frame later.
	 */
	refreshSoon() {
		if (this.refreshQueued) {
			return;
		}
		this.refreshQueued = true;
		requestAnimationFrame(() => {
			this.refreshQueued = false;
			if (!this.stopping.signal.aborted) {
				this.refresh();
			}
		});
	}

	/**
	 * Where the work starts on a map nobody has touched yet: the game layer,
	 * which every map has and every mapper knows; failing that the first tile
	 * layer, failing that the first layer there is.
	 *
	 * The native editor does the same (`SelectGameLayer` after
	 * `CreateDefault`). An editor that opens with nothing selected is an
	 * editor whose first stroke does nothing, and that is the one thing a
	 * first stroke must not do.
	 */
	defaultSelection() {
		const map = this.editor.structure();
		if (map === null || map.groups.length === 0) {
			return { group: 0, layer: -1 };
		}
		let first = null;
		let tiles = null;
		let game = null;
		map.groups.forEach((group, gi) => group.layers.forEach((layer, li) => {
			const where = { group: gi, layer: li };
			first = first || where;
			if (layer.type === "tiles") {
				tiles = tiles || where;
				if (layer.kind === "game") {
					game = game || where;
				}
			}
		}));
		return game || tiles || first || { group: 0, layer: -1 };
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
			// Escape is the way out of a field, and F6 the way on to the next
			// area; those two are all a field hands on.
			const name = keyName(event);
			const on = this.keys_.get(name);
			if (on !== undefined && (on.id === "focus.next" || on.id === "focus.previous")) {
				event.preventDefault();
				this.run(on.id);
				return;
			}
			if (event.key !== "Escape") {
				return;
			}
			target.blur();
		}
		// Space and Enter on a button press the button, and a list that is
		// open to the keyboard walks with the arrows - neither is a shortcut.
		// (The target may be the document itself when a key is sent by
		// script rather than struck, and the document has no attributes.)
		if (target && target.getAttribute && !event.ctrlKey && !event.altKey && !event.metaKey) {
			const own = target.tagName === "SELECT"
				? ["ArrowUp", "ArrowDown", "Home", "End", " ", "Enter"]
				: target.tagName === "BUTTON" || target.getAttribute("role") === "button" ? [" ", "Enter"] : [];
			if (own.includes(event.key)) {
				return;
			}
		}
		if (target && target.getAttribute && target.getAttribute("role") === "tab" && this.onTabKey(event)) {
			return;
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
		// A key held down repeats, and a switch that is thrown on every repeat
		// flickers: the tile chooser on Space opened and shut thirty times a
		// second. A switch answers to the press and to nothing after it; undo
		// is not a switch and goes on repeating.
		if (event.repeat && command.pressed !== undefined) {
			return;
		}
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

	/**
	 * Selects a group or a layer, and brings everything that depends on the
	 * selection up to date.
	 *
	 * The one way in: a row clicked in the tree, an arrow key, the eyedropper,
	 * the chooser under a right click and the button in the empty panel all
	 * come through here. There was a time when a click in the tree rebuilt the
	 * tree and the properties and nothing else, and the tileset, the brush,
	 * the tools and the hint went on talking about the layer before.
	 *
	 * @param where `{group, layer}`, with `layer` -1 for the group itself.
	 */
	select(where) {
		this.selection = { group: where.group, layer: where.layer === undefined ? -1 : where.layer };
		this.refreshSelection();
	}

	/**
	 * Everything that is about what is selected, made anew - and nothing
	 * about what is not: the pictures, the sounds, the envelopes and the
	 * history are the map's, and the map did not change.
	 */
	refreshSelection() {
		this.clampSelection();
		// A tool the new layer has no use for - the rubber on a quad layer -
		// hands over to the brush, rather than staying lit and doing nothing.
		const tool = this.commands.find(which => which.rail === true && which.pressed !== undefined && which.pressed(this));
		if (tool !== undefined && tool.enabled !== undefined && !tool.enabled(this)) {
			this.tool = "paint";
		}
		this.refreshTree();
		this.refreshProps();
		this.refreshTiles();
		this.refreshQuads();
		this.refreshSounds();
		this.refreshBar();
		this.refreshStatus();
		this.refreshOverlay();
		this.applyTabs();
		this.addMoreButtons();
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
			this.select(next);
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
		// A selection of the move tool goes too.
		this.letGo();
		const canvas = this.editor.canvas;
		if (canvas !== null && canvas !== undefined) {
			canvas.focus();
		}
	}

	/**
	 * Lets go of whatever the pointer code holds between two drags - the
	 * move tool's selection. Whoever wires the pointer says how.
	 */
	letGo() {
		if (this.pointerLetGo !== null && this.pointerLetGo !== undefined) {
			this.pointerLetGo();
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
		this.applyTabs();
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
		// A selection of the move tool is about tiles that are now elsewhere.
		this.letGo();
		const before = this.editor.history();
		const answer = this.change(work, true);
		const after = this.editor.history();
		if (before === null || after === null || before.current === after.current) {
			return answer;
		}
		const shown = after.current < before.current ? before.current : after.current;
		const where = this.snapshots.get(shown);
		if (where !== undefined) {
			this.select({ group: where.group, layer: where.layer });
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
			// A map seen for the first time starts on its game layer, whether
			// it came through `loaded` or was already open when the panels
			// were made.
			if (inFront >= 0 && !this.mapState.has(inFront) && this.selection.layer < 0) {
				this.selection = this.defaultSelection();
			}
			this.giveView(inFront);
		}
		const map = this.editor.structure();
		this.map = map;
		this.explained.clear();
		this.hoverKey = null;
		// A picture packed into the map may have been replaced by this change;
		// the fetched ones are files and stay.
		for (const source of [...this.pictures.keys()]) {
			if (source.startsWith("packed:")) {
				this.pictures.delete(source);
			}
		}
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
		this.refreshWelcome();
		// The line for the first stroke: there until something was drawn.
		const history = this.editor.history();
		if (history !== null && history.entries.length > 1) {
			this.hideHint();
		} else if (map !== null && !this.hinted) {
			this.showHint();
		}
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
		say("status-layer", layer === null ? groupName : `${groupName} \u203a ${layerName(layer)}`);
		const size = this.editor.brushSize();
		say("status-brush", size === null || size.width === 0 ? "" : `Brush ${size.width} \u00d7 ${size.height}`);
		const zoom = this.editor.zoom();
		const percent = zoom === null ? "" : `${Math.round(100 / zoom)} %`;
		say("status-zoom", percent);
		say("brush-swatch-size", size === null || size.width === 0 ? "" : `${size.width}\u00d7${size.height}`);
		if (this.zoomChip !== null && this.zoomChip !== undefined) {
			this.zoomChip.querySelector('[data-role="zoom-level"]').textContent = percent;
		}
		// What a drag does right now, said in a few words: the tool, and the
		// keys that make it another for one stroke.
		const command = this.commands.find(which => which.rail === true && which.pressed !== undefined && which.pressed(this));
		let hint = command === undefined ? "" : command.hint || command.label;
		if (this.map === null) {
			hint = "";
		} else if (layer === null) {
			hint = "Select a layer to paint";
		} else if (layer.type === "quads" && this.tool !== "hand" && this.tool !== "pick") {
			hint = `Drag a corner or the pivot of a quad${this.keysShown() ? " · Q adds one" : ""}`;
		} else if (layer.type === "sounds" && this.tool !== "hand" && this.tool !== "pick") {
			hint = "Drag a sound source to move it";
		} else if (layer.type === "tiles" && this.editor.brushEmpty() && this.tool === "paint") {
			hint = "No brush: drag to select tiles, or click a tile on the right";
		} else if (layer.type === "tiles" && this.tool === "paint" && this.keysShown() && !this.finger()) {
			hint += " \u00b7 Shift selects \u00b7 Alt fills \u00b7 Ctrl erases";
		}
		say("tool-hint", hint);
		const swatch = this.part("brush-swatch");
		if (swatch !== null) {
			swatch.classList.toggle("editor-swatch-empty", size === null || size.width === 0);
			swatch.title = size === null || size.width === 0 ? "No brush - click to choose tiles" : `Brush ${size.width} \u00d7 ${size.height} - click to choose tiles`;
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
	/** The bar's or the rail's button for a command, by the command's own name. */
	barButton(command) {
		return this.commandButtons().find(button => button.dataset.command === command.id && button.dataset.bar !== undefined) || null;
	}

	/**
	 * Every button that stands for a command, wherever it stands - in the bar,
	 * in the rail, in the status line, in a panel - says what its command
	 * says: whether it can be done, whether it is on, and what it is called.
	 */
	refreshBar() {
		for (const button of this.commandButtons()) {
			const command = this.commands.find(which => which.id === button.dataset.command);
			if (command === undefined) {
				continue;
			}
			if (button.dataset.bar !== undefined) {
				button.hidden = !this.barShows(command);
			}
			if (command.part === button.dataset.role) {
				// A panel's own button: whether it can be pressed is the
				// panel's to say, and the command asks the button. Answering
				// the button from the command would ask it about itself, and a
				// button once greyed - before the map was there - would stay
				// grey for good. Only a look-only editor greys it from here.
				const refused = this.readonly && command.safe !== true;
				if (refused) {
					button.disabled = true;
					button.dataset.refused = "";
				} else if (button.dataset.refused !== undefined) {
					button.disabled = false;
					delete button.dataset.refused;
				}
			} else {
				button.disabled = (command.enabled !== undefined && !command.enabled(this))
					|| (this.readonly && command.safe !== true);
			}
			// The name is written again rather than once at the start: whether
			// the shortcut belongs beside it is not known until the first key.
			if (button.dataset.role !== "brush-swatch") {
				button.title = commandTitle(command, this.keysShown());
				if (button.getAttribute("aria-label") === null || button.dataset.bar !== undefined) {
					button.setAttribute("aria-label", command.label);
				}
			}
			if (command.pressed !== undefined) {
				const on = command.pressed(this);
				button.setAttribute(button.getAttribute("role") === "radio" ? "aria-checked" : "aria-pressed", on ? "true" : "false");
				if (button.getAttribute("role") === "radio") {
					button.setAttribute("aria-pressed", on ? "true" : "false");
				}
			}
		}
		this.refreshRailKeys();
		// Proof mode has three states on one button, and which of the two
		// on-states it is in is not a thing `aria-pressed` can say.
		const proof = this.part("proof");
		if (proof !== null) {
			proof.dataset.proof = this.proof;
		}
	}

	/** Every button that carries a command's name, wherever the panels stand. */
	commandButtons() {
		const found = [...this.root.querySelectorAll("[data-command]")];
		if (this.areas !== null) {
			for (const area of Object.values(this.areas)) {
				found.push(...area.querySelectorAll("[data-command]"));
			}
		}
		if (this.box !== null && this.box !== undefined) {
			for (const one of this.box.querySelectorAll("[data-command]")) {
				if (!found.includes(one)) {
					found.push(one);
				}
			}
		}
		return found;
	}

	refreshTree() {
		const tree = this.part("tree");
		// The rows are made anew, so a keyboard that was in the tree would
		// otherwise find itself nowhere.
		const hadFocus = tree.contains(document.activeElement);
		if (!this.treeKeys) {
			this.treeKeys = true;
			tree.setAttribute("role", "tree");
			tree.setAttribute("aria-label", "Groups and layers");
			tree.addEventListener("keydown", event => this.onTreeKey(event), { signal: this.stopping.signal });
		}
		tree.textContent = "";
		if (this.map === null) {
			return;
		}
		this.map.groups.forEach((group, groupIndex) => {
			const item = document.createElement("li");
			item.className = "editor-group";
			item.setAttribute("role", "none");
			const head = document.createElement("div");
			head.className = "editor-row";
			head.dataset.role = "group";
			head.dataset.group = String(groupIndex);
			head.setAttribute("role", "treeitem");
			head.setAttribute("aria-level", "1");
			head.setAttribute("aria-expanded", this.collapsed.has(groupIndex) ? "false" : "true");
			head.setAttribute("aria-selected", this.selection.group === groupIndex && this.selection.layer < 0 ? "true" : "false");
			head.tabIndex = -1;
			const fold = document.createElement("button");
			fold.className = "editor-fold";
			// The row is the one stop for the keyboard; the arrows fold it.
			fold.tabIndex = -1;
			fold.setAttribute("aria-hidden", "true");
			fold.dataset.icon = this.collapsed.has(groupIndex) ? "chevronRight" : "chevronDown";
			fold.addEventListener("click", event => {
				event.stopPropagation();
				if (this.collapsed.has(groupIndex)) {
					this.collapsed.delete(groupIndex);
				} else {
					this.collapsed.add(groupIndex);
				}
				this.refreshTree();
			}, { signal: this.stopping.signal });
			const picture = DDNetBase.icon("group");
			picture.classList.add("editor-row-icon");
			const name = document.createElement("span");
			name.className = "editor-name";
			name.textContent = group.name || `Group ${groupIndex}`;
			head.append(fold, picture, name);
			if (this.selection.group === groupIndex && this.selection.layer < 0) {
				head.classList.add("editor-selected");
			}
			head.addEventListener("click", () => this.select({ group: groupIndex, layer: -1 }), { signal: this.stopping.signal });
			this.wireDragging(head, { group: groupIndex, layer: -1 });
			item.append(head);

			if (!this.collapsed.has(groupIndex)) {
				const list = document.createElement("ul");
				list.className = "editor-layers";
				list.setAttribute("role", "group");
				group.layers.forEach((layer, layerIndex) => {
					const row = document.createElement("li");
					row.className = "editor-row editor-layer";
					row.dataset.role = "layer";
					row.dataset.group = String(groupIndex);
					row.dataset.layer = String(layerIndex);
					row.setAttribute("role", "treeitem");
					row.setAttribute("aria-level", "2");
					row.setAttribute("aria-selected", this.selection.group === groupIndex && this.selection.layer === layerIndex ? "true" : "false");
					row.tabIndex = -1;
					const what = layer.type === "tiles" ? layer.kind : layer.type;
					// Hiding a layer is a thing about looking, so the eye is
					// not a property and writes no history entry.
					const shown = this.editor.visible(groupIndex, layerIndex);
					const eye = document.createElement("button");
					eye.className = "editor-eye";
					eye.dataset.role = "visible";
					eye.dataset.icon = shown ? "eye" : "eyeOff";
					eye.title = shown ? "Hide this layer" : "Show this layer";
					eye.setAttribute("aria-pressed", shown ? "true" : "false");
					// Space on the row does what the eye does.
					eye.tabIndex = -1;
					eye.addEventListener("click", event => {
						event.stopPropagation();
						this.editor.visible(groupIndex, layerIndex, !shown);
						this.refreshTree();
					}, { signal: this.stopping.signal });
					const picture = DDNetBase.icon(layerIcon(layer));
					picture.classList.add("editor-row-icon");
					const label = document.createElement("span");
					label.className = "editor-row-name";
					label.textContent = layerName(layer);
					label.title = `${layerName(layer)} (${what})`;
					if (layer.kind === "game") {
						row.classList.add("editor-layer-game");
					}
					row.append(picture, label);
					// The picture a layer draws with, in small print: three
					// layers all called "Tiles" are told apart by it, which is
					// what a thumbnail would be for, at the price of a word
					// instead of a picture too small to read.
					const image = typeof layer.image === "number" && layer.image >= 0 && this.map !== null
						&& this.map.images !== undefined && layer.image < this.map.images.length
						? this.map.images[layer.image] : null;
					if (image !== null) {
						const sub = document.createElement("span");
						sub.className = "editor-row-sub";
						sub.dataset.role = "layer-image";
						sub.textContent = image.name;
						sub.title = `Drawn with ${image.name}`;
						row.append(sub);
					}
					row.append(eye);
					if (!shown) {
						row.classList.add("editor-hidden-layer");
						row.setAttribute("aria-description", "hidden");
					}
					if (this.selection.group === groupIndex && this.selection.layer === layerIndex) {
						row.classList.add("editor-selected");
					}
					row.addEventListener("click", () => this.select({ group: groupIndex, layer: layerIndex }), { signal: this.stopping.signal });
					this.wireDragging(row, { group: groupIndex, layer: layerIndex });
					list.append(row);
				});
				item.append(list);
			}
			tree.append(item);
		});
		DDNetBase.paintIcons(tree);
		// One stop for Tab in the whole tree - the row that is selected, or
		// its group while the group is folded up - rather than one per layer.
		const rows = [...tree.querySelectorAll('[role="treeitem"]')];
		const stop = rows.find(row => row.getAttribute("aria-selected") === "true")
			|| rows.find(row => row.dataset.role === "group" && Number(row.dataset.group) === this.selection.group)
			|| rows[0];
		if (stop !== undefined) {
			stop.tabIndex = 0;
			if (hadFocus) {
				stop.focus();
			}
			// The row that is selected is the row one is working on; a list
			// that has scrolled it out of sight is a list that hides the one
			// thing it is for.
			if (stop.getAttribute("aria-selected") === "true") {
				this.revealSelected();
			}
		}
		this.addMoreButtons();
	}

	/**
	 * Scrolls the list of layers so that the selected row is in sight. Done
	 * by hand rather than with `scrollIntoView`: before the first layout
	 * there is nothing to scroll yet, so a list that has no height asks again
	 * after the next frame - and looks the row up again then, because every
	 * refresh makes the rows anew.
	 */
	revealSelected() {
		const tree = this.part("tree");
		if (tree === null) {
			return;
		}
		if (tree.clientHeight === 0) {
			if (this.revealPending !== true) {
				this.revealPending = true;
				requestAnimationFrame(() => {
					this.revealPending = false;
					this.revealSelected();
				});
			}
			return;
		}
		const row = tree.querySelector('[aria-selected="true"]');
		if (row === null) {
			return;
		}
		const at = row.getBoundingClientRect();
		const box = tree.getBoundingClientRect();
		if (at.bottom > box.bottom) {
			tree.scrollTop += at.bottom - box.bottom;
		} else if (at.top < box.top) {
			tree.scrollTop -= box.top - at.top;
		}
	}

	/**
	 * The tree as a keyboard walks it: up and down go from row to row and
	 * select, right opens a group and goes into it, left folds it or goes back
	 * up to it, Space is the eye (or the fold) and Enter goes to the name.
	 */
	onTreeKey(event) {
		const row = event.target.closest('[role="treeitem"]');
		if (row === null || event.ctrlKey || event.altKey || event.metaKey || event.shiftKey) {
			return;
		}
		const tree = this.part("tree");
		const rows = [...tree.querySelectorAll('[role="treeitem"]')];
		const at = rows.indexOf(row);
		const group = Number(row.dataset.group);
		const isGroup = row.dataset.role === "group";
		const choose = other => {
			if (other === undefined) {
				return;
			}
			this.select({ group: Number(other.dataset.group), layer: other.dataset.role === "group" ? -1 : Number(other.dataset.layer) });
			const now = tree.querySelector('[role="treeitem"][tabindex="0"]');
			if (now !== null) {
				now.focus();
			}
		};
		const fold = shut => {
			if (shut) {
				this.collapsed.add(group);
			} else {
				this.collapsed.delete(group);
			}
			this.refreshTree();
		};
		switch (event.key) {
		case "ArrowDown":
			choose(rows[at + 1]);
			break;
		case "ArrowUp":
			choose(rows[at - 1]);
			break;
		case "Home":
			choose(rows[0]);
			break;
		case "End":
			choose(rows[rows.length - 1]);
			break;
		case "ArrowRight":
			if (isGroup && this.collapsed.has(group)) {
				fold(false);
			} else if (isGroup) {
				choose(rows[at + 1] !== undefined && rows[at + 1].dataset.role === "layer" ? rows[at + 1] : undefined);
			}
			break;
		case "ArrowLeft":
			if (isGroup && !this.collapsed.has(group)) {
				fold(true);
			} else if (!isGroup) {
				choose(rows.find(other => other.dataset.role === "group" && Number(other.dataset.group) === group));
			}
			break;
		case " ":
			if (isGroup) {
				fold(!this.collapsed.has(group));
			} else {
				row.querySelector('[data-role="visible"]').click();
			}
			break;
		case "Enter":
		case "F2": {
			if (row.getAttribute("aria-selected") !== "true") {
				choose(row);
			}
			// The name is in the inspector, which may be shut: Enter on a
			// row is the keyboard's double click, and opens it at Properties.
			if (this.areas !== null) {
				this.showArea("left", true);
				this.showTab("left", "props");
			}
			const name = this.part("props").querySelector("input");
			if (name !== null) {
				name.focus();
				name.select();
			}
			break;
		}
		default:
			return;
		}
		event.preventDefault();
		event.stopPropagation();
	}

	/**
	 * A row of tabs as a keyboard walks it: one stop for Tab, the arrows go
	 * to the neighbour and show it, Home and End to the ends. Whether it was
	 * one of those keys.
	 */
	onTabKey(event) {
		if (event.ctrlKey || event.altKey || event.metaKey || event.shiftKey
			|| !["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
			return false;
		}
		const tabs = [...event.target.parentElement.children]
			.filter(one => one.getAttribute("role") === "tab" && !one.hidden && !one.disabled);
		const at = tabs.indexOf(event.target);
		const next = event.key === "Home" ? tabs[0]
			: event.key === "End" ? tabs[tabs.length - 1]
				: tabs[(at + (event.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length];
		event.preventDefault();
		if (next !== undefined && next !== event.target) {
			next.click();
			next.focus();
		}
		return true;
	}

	/**
	 * Where F6 goes: the areas in the order they are read - the strip of
	 * maps, the tool bar, the left, the map, the right and the dock - leaving
	 * out whatever is not shown. The focus lands on what that area has
	 * selected, or else on the first thing in it that takes the focus.
	 */
	focusArea(step) {
		const canvas = this.editor.canvas;
		const areas = this.areas === null
			? [this.element, canvas]
			: [this.areas.toolbar, this.areas.rail, this.areas.left, canvas, this.areas.right, this.areas.dock];
		const shown = areas.filter(area => area !== null && area !== undefined && !area.hidden && area.getClientRects().length > 0);
		if (shown.length === 0) {
			return false;
		}
		const now = document.activeElement;
		const from = shown.findIndex(area => area === now || (area !== canvas && area.contains(now)));
		const next = shown[from < 0 ? (step > 0 ? 0 : shown.length - 1) : (from + step + shown.length) % shown.length];
		if (next === canvas) {
			canvas.focus();
			return true;
		}
		const usable = one => !one.disabled && one.tabIndex >= 0 && one.getClientRects().length > 0 && one.closest("[hidden]") === null;
		// What is chosen in the area before whatever comes first in it: the
		// row of the tree, the tool in hand, the tab in front, then anything.
		for (const which of ['[role="treeitem"][tabindex="0"]', '[role="radio"][aria-checked="true"]', '[role="tab"][aria-selected="true"]', 'button, input, select, textarea, [tabindex="0"]']) {
			const one = [...next.querySelectorAll(which)].find(usable);
			if (one !== undefined) {
				one.focus();
				return true;
			}
		}
		return false;
	}

	refreshProps() {
		const props = this.part("props");
		props.textContent = "";
		this.part("construct").hidden = true;
		const nothing = this.map === null || this.map.groups.length === 0;
		// The place the tileset would be, while a group is selected: it says
		// what to do instead of showing nothing.
		this.showPanel("group-panel", !nothing && this.selection.layer < 0);
		if (nothing) {
			this.part("props-title").textContent = "Properties";
			return;
		}
		const where = this.selection;
		const group = this.map.groups[where.group];
		if (where.layer < 0) {
			const text = this.part("group-text");
			if (text !== null) {
				text.textContent = group.layers.length === 0
					? `${group.name || `Group ${where.group}`} is empty. Add a layer to paint in it.`
					: "A group holds layers. Select a layer to paint.";
			}
			this.part("props-title").textContent = `Group: ${group.name || where.group}`;
			for (const field of GROUP_PROPS) {
				props.append(this.field(group, field, value => ({
					op: "group.setProp", group: where.group, prop: field.prop, value: value,
				})));
			}
			return;
		}
		const layer = group.layers[where.layer];
		this.part("props-title").textContent = `Layer: ${layerName(layer)}`;
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
		} else if (description.kind === "ref") {
			// One of the map's images, envelopes or sounds, chosen by name:
			// the file keeps a number, and "-1" or "3" says nothing about
			// what is drawn or heard.
			const chooser = document.createElement("select");
			chooser.dataset.role = input.dataset.role;
			const things = this.map === null ? [] : (this.map[description.list] || []);
			const offer = (value, label) => {
				const option = document.createElement("option");
				option.value = String(value);
				option.textContent = label;
				chooser.append(option);
			};
			offer(-1, "None");
			things.forEach((thing, index) => {
				const called = thing.name || description.list.replace(/s$/, "");
				offer(index, description.list === "envelopes" ? `${index}: ${called} (${thing.channels})` : `${index}: ${called}`);
			});
			// A number that points past the list is still what the file
			// says, and shown as that rather than as something else.
			if (Number.isInteger(current) && (current < -1 || current >= things.length)) {
				offer(current, `${current}: missing`);
			}
			chooser.value = String(current);
			chooser.addEventListener("change", () => {
				send(Number.parseInt(chooser.value, 10));
				this.refresh();
			}, { signal: this.stopping.signal });
			row.append(name, chooser);
			return row;
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
			// The brush in the slot as a picture, and the digit that reaches
			// it small in the corner; an empty slot is the digit alone.
			button.innerHTML = '<canvas class="editor-slot-picture" data-role="slot-picture" width="28" height="28"></canvas><span class="editor-slot-key"></span>';
			button.lastElementChild.textContent = String(slot);
			button.title = `Brush ${slot} (${slot}, shift and ${slot} to put one here)`;
			button.setAttribute("aria-label", `Brush slot ${slot}`);
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

	/** Each slot as a picture of what is in it, or empty. */
	refreshSlots() {
		for (const button of this.parts("slot")) {
			const slot = Number(button.dataset.slot);
			const brush = this.editor.storedBrush(slot);
			const full = brush !== null;
			button.setAttribute("aria-pressed", full ? "true" : "false");
			button.classList.toggle("editor-slot-empty", !full);
			button.title = full
				? `Brush ${slot}: ${brush.width} × ${brush.height} (${slot} takes it, shift and ${slot} puts the brush here)`
				: `Brush slot ${slot} (shift and ${slot} to put the brush here)`;
			const canvas = button.querySelector('[data-role="slot-picture"]');
			if (canvas !== null) {
				const picture = full ? this.pictureFor(brush.kind, brush.image).picture : null;
				drawBrush(canvas, brush, picture, this.brushColouring && full ? brush.color : null);
			}
		}
	}

	wireTileset() {
		this.buildSlots();
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

	/**
	 * The picture a layer or a brush draws with, and where it came from.
	 *
	 * A physics kind draws out of the entities sheet; anything else out of
	 * the map's picture, fetched by the browser where it lies beside the map
	 * and asked of the program where it is packed into the file. Asked for
	 * once and kept; while it is on its way the answer is `null`, and the
	 * tiles are drawn again when it has come.
	 *
	 * @param kind The kind of tile layer: "tiles", "game", "tele" and so on.
	 * @param image Which of the map's pictures, or -1 for none.
	 * @return `{source, picture}`, both `null` where there is no picture.
	 */
	pictureFor(kind, image) {
		const physics = kind !== undefined && kind !== "tiles";
		const known = this.map === null || image < 0 || image >= this.map.images.length ? null : this.map.images[image];
		const source = physics
			? new URL(`editor/entities_clear/${this.editor.entitiesImage()}.png`, this.dataBase).href
			: known === null ? null : (known.external ? new URL(`mapres/${known.name}.png`, this.dataBase).href : `packed:${image}:${known.name}`);
		if (source === null) {
			return { source: null, picture: null };
		}
		let picture = this.pictures.get(source);
		if (picture === undefined) {
			if (source.startsWith("packed:")) {
				// `drawImage` takes no `ImageData`, so the pixels go through a
				// canvas once, here, rather than on every drawing.
				const data = this.editor.imageData(image);
				picture = null;
				if (data !== null) {
					picture = document.createElement("canvas");
					picture.width = data.width;
					picture.height = data.height;
					picture.getContext("2d").putImageData(data, 0, 0);
				}
				this.pictures.set(source, picture);
			} else {
				picture = null;
				this.pictures.set(source, null);
				const fetched = new Image();
				fetched.addEventListener("load", () => {
					if (this.stopping.signal.aborted) {
						return;
					}
					this.pictures.set(source, fetched);
					this.refreshTiles();
				}, { once: true });
				fetched.src = source;
			}
		}
		return { source: source, picture: picture };
	}

	/**
	 * Whether the rectangle marked in the tileset is what the brush holds. A
	 * brush grabbed off the map, or turned, is not - and a mark in the
	 * tileset that said otherwise would be a lie.
	 */
	pickedMatches(brush) {
		const picked = this.picked;
		if (picked === null || brush === null) {
			return picked === null && brush === null;
		}
		if (brush.width !== picked.width || brush.height !== picked.height) {
			return false;
		}
		for (let y = 0; y < brush.height; y++) {
			for (let x = 0; x < brush.width; x++) {
				const at = (y * brush.width + x) * 2;
				if (brush.tiles[at] !== (picked.y + y) * TILESET_SIDE + picked.x + x || brush.tiles[at + 1] !== 0) {
					return false;
				}
			}
		}
		return true;
	}

	refreshTiles() {
		const layer = this.selectedLayer();
		this.showPanel("tiles-panel", !(layer === null || layer.type !== "tiles"));
		// What is in hand is shown whatever is selected: a brush is carried
		// from layer to layer.
		if (!this.panelShown.get("tiles-panel")) {
			this.paintSwatch();
			this.refreshSlots();
			return;
		}
		this.ensureBrush();
		const brush = this.editor.brush();
		if (this.picked !== null && !this.pickedMatches(brush)) {
			this.picked = null;
		}
		const size = this.editor.brushSize();
		this.part("brush-size").textContent = size === null || size.width === 0 ? "No brush" : `${size.width} \u00d7 ${size.height}`;
		this.applyTileZoom();
		this.refreshNumbers(layer);
		this.refreshAutomap(layer);
		this.refreshRules(layer);

		// The picture the layer is drawn with. A layer with no picture at all
		// is left with a grid of numbers - the tiles are still there to be
		// picked, they just cannot be shown. A physics layer has no picture
		// of its own: it is drawn out of the entities sheet, so that is what
		// its tileset shows too.
		const image = layer.image >= 0 && layer.image < this.map.images.length ? this.map.images[layer.image] : null;
		const physics = layer.kind !== undefined && layer.kind !== "tiles";
		const found = this.pictureFor(layer.kind, layer.image);
		this.tilesetSource = found.source;
		this.tileset = found.picture;
		const title = this.part("tiles-title");
		if (title !== null) {
			title.textContent = physics ? "Entities" : image === null ? "Tiles (no image)" : image.name;
		}
		this.refreshSlots();
		this.paintTileset();
		this.paintPicker();
		this.paintSwatch();
	}

	/**
	 * How big a tile is in the tileset beside the map. Nothing set fits the
	 * column; a number is pixels a tile, and the box around it scrolls.
	 */
	applyTileZoom() {
		const canvas = this.part("tileset");
		if (canvas === null) {
			return;
		}
		const side = this.tileZoom === null ? 512 : this.tileZoom * TILESET_SIDE;
		if (canvas.width !== side) {
			canvas.width = side;
			canvas.height = side;
		}
		canvas.style.width = this.tileZoom === null ? "" : `${side}px`;
		canvas.style.height = this.tileZoom === null ? "" : `${side}px`;
		const out = this.part("tiles-zoom-out");
		const into = this.part("tiles-zoom-in");
		const steps = TILE_ZOOMS.indexOf(this.tileZoom);
		if (out !== null) {
			out.disabled = steps <= 0;
		}
		if (into !== null) {
			into.disabled = steps >= TILE_ZOOMS.length - 1;
		}
	}

	/** One step bigger or smaller, through the sizes a tile can be drawn at. */
	zoomTiles(step) {
		const at = TILE_ZOOMS.indexOf(this.tileZoom);
		const next = Math.max(0, Math.min(TILE_ZOOMS.length - 1, at + step));
		this.tileZoom = TILE_ZOOMS[next];
		this.applyTileZoom();
		this.paintTileset();
		try {
			if (this.remembers()) {
				localStorage.setItem(TILE_ZOOM_STORAGE, String(this.tileZoom));
			}
		} catch (error) {
			// Not kept; it is the size for this visit.
		}
	}

	/**
	 * What is in hand, drawn small in the rail: the brush's own tiles, as
	 * the program says they are drawn - so a brush grabbed off the map, one
	 * turned over, and one of tele tiles all look like what they will put
	 * down.
	 */
	paintSwatch() {
		const canvas = this.part("brush-picture");
		if (canvas === null) {
			return;
		}
		const brush = this.editor.brush();
		const picture = brush === null ? null : this.pictureFor(brush.kind, brush.image).picture;
		drawBrush(canvas, brush, picture, this.brushColouring && brush !== null ? brush.color : null);
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
					free.title = "Next number this layer does not use yet";
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
		if (this.tileset !== null) {
			// Without smoothing, because a tile is sixteen pixels and smoothing
			// it makes it somebody else's tile at the edges.
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

	/**
	 * A brush that is never empty by accident.
	 *
	 * A tile layer that is selected while nothing is in hand gets tile 1 -
	 * hookable on the game layer, the first tile of the picture elsewhere -
	 * the way LDtk's tile tool starts on tile 0. An empty brush grabs instead
	 * of painting, which mappers rely on and newcomers fall into; so it is
	 * only empty once somebody emptied it on purpose, and only until the next
	 * layer.
	 */
	ensureBrush() {
		const where = this.selection;
		const layer = this.selectedLayer();
		if (layer === null || layer.type !== "tiles" || this.readonly) {
			return;
		}
		const same = this.brushFor !== null && this.brushFor.group === where.group && this.brushFor.layer === where.layer;
		if (!same) {
			this.brushFor = { group: where.group, layer: where.layer };
			this.brushCleared = false;
		}
		if (this.brushCleared || !this.editor.brushEmpty()) {
			return;
		}
		this.picked = { x: 1, y: 0, width: 1, height: 1 };
		this.editor.pickTiles(where.group, where.layer, 1, 0, 1, 1);
	}

	/**
	 * Takes the tile under a spot on the map into the brush, and its layer
	 * into the selection: the eyedropper of a paint program, for a map.
	 *
	 * The layer nearest the front with something other than air there is the
	 * one meant. The tile is grabbed rather than picked out of the tileset so
	 * that a physics tile brings its numbers along.
	 */
	pickAt(spot) {
		if (this.map === null || this.readonly) {
			return false;
		}
		const canvas = this.editor.canvas;
		const box = canvas.getBoundingClientRect();
		const factor = (canvas.width || 1) / (box.width || 1);
		const found = this.layersAt((spot.x - box.left) * factor, (spot.y - box.top) * factor);
		if (found.length === 0) {
			this.say("Nothing but air here");
			return false;
		}
		// The layer being worked on first, where it has something here - the
		// way Tiled's and Ogmo's eyedroppers read the current layer - and the
		// one nearest the front otherwise.
		const where = this.selection;
		const what = found.find(one => one.group === where.group && one.layer === where.layer) || found[0];
		this.brushFor = { group: what.group, layer: what.layer };
		this.brushCleared = false;
		this.editor.grab(what.group, what.layer, what.tile.x, what.tile.y, 1, 1);
		this.picked = { x: what.index % TILESET_SIDE, y: Math.floor(what.index / TILESET_SIDE), width: 1, height: 1 };
		this.select({ group: what.group, layer: what.layer });
		this.say(`Picked tile ${what.index} from ${what.name}`);
		return true;
	}

	/**
	 * A stroke that could not paint says why, once, where it was tried.
	 *
	 * A group is selected, or a quad layer with nothing under the pointer: the
	 * map pans, which is right, and a newcomer sees nothing happen, which is
	 * not. The layer list is nudged as well, so that the eye goes where the
	 * fix is.
	 */
	cannotPaint(why) {
		const now = performance.now();
		if (now - this.cannotSaidAt < TOAST_MS) {
			return;
		}
		this.cannotSaidAt = now;
		const layer = this.selectedLayer();
		if (layer !== null && layer.type === "quads") {
			this.say("This is a quad layer: drag a corner or pivot, or select a tile layer to paint");
		} else if (layer !== null && layer.type === "sounds") {
			this.say("This is a sound layer: drag a source, or select a tile layer to paint");
		} else {
			this.say("Select a tile layer to paint - click one in Layers");
		}
		const panel = this.part("tree-panel");
		if (panel !== null) {
			panel.classList.remove("editor-nudge");
			// Taken off and put back on in two frames, so that a second nudge
			// runs the animation again rather than finding it already there.
			requestAnimationFrame(() => panel.classList.add("editor-nudge"));
			setTimeout(() => panel.classList.remove("editor-nudge"), 700);
		}
	}

	/**
	 * One line over the map for whoever has never used the editor: what a
	 * drag does. It goes with the first stroke, or with its cross, and a page
	 * that remembers things remembers that it went.
	 */
	showHint() {
		const home = this.floatHome;
		if (home === null || home === undefined || this.hint !== null || this.readonly) {
			return;
		}
		try {
			if (this.remembers() && localStorage.getItem(HINT_STORAGE) === "seen") {
				return;
			}
		} catch (error) {
			// A browser that keeps nothing shows the hint every time.
		}
		this.hint = document.createElement("div");
		this.hint.className = "editor-hint";
		this.hint.dataset.role = "hint";
		this.hint.setAttribute("role", "status");
		const text = document.createElement("span");
		text.dataset.role = "hint-text";
		text.textContent = this.finger()
			? "Drag on the map to paint · two fingers pan and zoom"
			: "Drag on the map to paint · right-drag pans · wheel zooms";
		const away = document.createElement("button");
		away.type = "button";
		away.className = "editor-hint-close";
		away.dataset.role = "hint-close";
		away.textContent = "×";
		away.setAttribute("aria-label", "Dismiss");
		away.addEventListener("click", () => this.hideHint(), { signal: this.stopping.signal });
		this.hint.append(text, away);
		home.append(this.hint);
	}

	/**
	 * What stands over the map while there is no map: how to get one. The
	 * canvas is black then, and a black box with a bar over it is a box that
	 * looks broken.
	 */
	refreshWelcome() {
		const home = this.floatHome;
		if (home === null || home === undefined) {
			return;
		}
		if (this.map !== null) {
			if (this.welcome !== null) {
				this.welcome.remove();
				this.welcome = null;
			}
			return;
		}
		if (this.welcome !== null) {
			return;
		}
		this.welcome = document.createElement("div");
		this.welcome.className = "editor-welcome";
		this.welcome.dataset.role = "welcome";
		const title = document.createElement("p");
		title.className = "editor-welcome-title";
		title.textContent = "No map is open";
		const text = document.createElement("p");
		text.className = "editor-welcome-text";
		text.textContent = "Open a map, drop one onto the editor, or start a new one.";
		const buttons = document.createElement("div");
		buttons.className = "editor-welcome-buttons";
		for (const [id, className] of [["file.open", "editor-small editor-go"], ["file.new", "editor-small"]]) {
			const command = this.commands.find(which => which.id === id);
			if (command === undefined) {
				continue;
			}
			const button = document.createElement("button");
			button.type = "button";
			button.className = className;
			button.dataset.command = id;
			button.textContent = command.label;
			button.addEventListener("click", () => this.run(id), { signal: this.stopping.signal });
			buttons.append(button);
		}
		this.welcome.append(title, text, buttons);
		home.append(this.welcome);
	}

	hideHint() {
		if (this.hint === null) {
			return;
		}
		this.hint.remove();
		this.hint = null;
		this.hinted = true;
		try {
			if (this.remembers()) {
				localStorage.setItem(HINT_STORAGE, "seen");
			}
		} catch (error) {
			// Nothing kept; the hint comes back next time, which is no harm.
		}
	}

	/** Whether the page asked the editor to keep things between visits. */
	remembers() {
		return this.box !== null && this.box !== undefined && typeof this.box.hasAttribute === "function"
			&& this.box.hasAttribute("remember");
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
		// The rows are new, and a new row has no menu button yet.
		this.addMoreButtons();
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
		// The rows are new, and a new row has no menu button yet.
		this.addMoreButtons();
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
		// Said once per tile, not once per pixel: a pointer crossing a tile
		// sends a dozen moves, and the words are the same for all of them.
		const where = this.selection;
		const key = tile === null ? "" : `${tile.x},${tile.y},${where.group},${where.layer},${this.tileInfo}`;
		if (key === this.hoverKey) {
			return;
		}
		this.hoverKey = key;
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
		const meaning = `${where.group},${where.layer},${index}`;
		let said = this.explained.get(meaning);
		if (said === undefined) {
			said = this.editor.explain(where.group, where.layer, index);
			this.explained.set(meaning, said);
		}
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
		// The rows are new, and a new row has no menu button yet.
		this.addMoreButtons();
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
		// The rows are new, and a new row has no menu button yet.
		this.addMoreButtons();
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
		// An empty panel says what it is for rather than standing blank.
		if (settings.length === 0) {
			const empty = document.createElement("li");
			empty.className = "editor-empty-text editor-settings-empty";
			empty.dataset.role = "settings-empty";
			empty.textContent = "No server settings. + adds a line the server runs when the map is loaded, such as sv_team 1.";
			list.append(empty);
		}
		// The rows are new, and a new row has no menu button yet.
		this.addMoreButtons();
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
		onPick: null, onCannotPaint: null, penHoldsPaper: null, paintable: null, signal: undefined,
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
	const target = () => {
		const where = settings.target === null ? null : settings.target();
		return where == null || where.layer < 0 ? null : where;
	};
	// The tile under the pointer in the group that is painted in, which for
	// a group with parallax is not the plain view's tile; without a layer to
	// paint in, the plain view's.
	const tileAt = event => {
		const at = atCanvas(event);
		const where = target();
		return where === null ? editor.tileAt(at.x, at.y) : editor.groupTileAt(where.group, at.x, at.y);
	};
	// Whether what is selected takes tiles at all - the panels know, and a
	// quad layer is asked for its quads only once a button goes down.
	const paintable = () => settings.paintable === null ? true : settings.paintable();
	// What the brush would do at the tile, shown before it does it. Said on
	// every move; the program only redraws when it differs from the last.
	let ghostShown = false;
	const ghost = (kind, where, box) => {
		if (kind === null || where === null || box === null) {
			if (ghostShown) {
				editor.ghost(null);
				ghostShown = false;
			}
			return;
		}
		ghostShown = true;
		editor.ghost(kind, where.group, box.x, box.y, box.width === undefined ? 1 : box.width, box.height === undefined ? 1 : box.height);
	};
	// The ghost for a pointer that is only hovering: the brush where the
	// tool would put it, or an outline where the tool takes a rectangle.
	const hoverGhost = (event, known) => {
		const where = target();
		if (where === null || !paintable() || (down.size > 0 && doing !== "paint" && doing !== null)) {
			ghost(null);
			return;
		}
		const tile = known === undefined ? tileAt(event) : known;
		if (tile === null) {
			ghost(null);
			return;
		}
		const tool = settings.mode === null ? "paint" : settings.mode();
		let kind = null;
		if (tool === "hand") {
			kind = null;
		} else if (tool === "move") {
			// Over the selection nothing: the mark says it all. Elsewhere the
			// tile under the pointer, where a new selection would begin.
			kind = moveBox !== null && insideBox(moveBox, tile) ? null : "spot";
		} else if (tool === "erase" || event.ctrlKey || event.metaKey) {
			kind = "erase";
		} else if (tool === "paint" && !event.altKey && !event.shiftKey && !editor.brushEmpty()) {
			kind = "stamp";
		} else {
			kind = "spot";
		}
		ghost(kind, where, tile);
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
	// Said only for what the program does not say itself. A stroke that was
	// committed is announced by the program a frame later, and a page told
	// twice would rebuild its panels twice.
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
	// The rectangle the move tool has selected, while it has one: it stays
	// marked after the button comes up, so that the next drag can pick it up.
	// And where its corner was when a drag began to carry it.
	let moveBox = null;
	let carried = null;
	const insideBox = (box, tile) => tile.x >= box.x && tile.y >= box.y && tile.x < box.x + box.width && tile.y < box.y + box.height;

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
		}
		doing = null;
		quadPoint = null;
		sourceDrag = null;
		editor.mark();
		ghost(null);
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
		if (sawPen && event.pointerType === "touch" && (settings.penHoldsPaper === null || settings.penHoldsPaper())) {
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
		// Whatever the hover showed is over; what the press does says anew.
		ghost(null);
		const where = target();
		const tool = settings.mode === null ? "paint" : settings.mode();
		// A selection of the move tool lasts while the tool does, on its layer.
		if (moveBox !== null && (tool !== "move" || where === null || where.group !== moveBox.group || where.layer !== moveBox.layer)) {
			moveBox = null;
			editor.mark();
		}
		// The hand only moves the map, whatever is under it.
		if (event.button === 0 && tool === "hand") {
			doing = "move";
			return;
		}
		// The eyedropper: what is here goes into the brush, and its layer
		// into the selection. A click, not a drag.
		if (event.button === 0 && tool === "pick") {
			if (settings.onPick !== null) {
				settings.onPick({ x: event.clientX, y: event.clientY });
			}
			doing = "tool";
			return;
		}
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
		// A quad or sound layer with nothing under the pointer: there is no
		// tile to paint, so the map pans - and says so.
		if (where !== null && (editor.quads(where.group, where.layer) !== null || editor.sources(where.group, where.layer) !== null)) {
			if (event.button === 0 && settings.onCannotPaint !== null) {
				settings.onCannotPaint("tool");
			}
			doing = "move";
			return;
		}
		const tile = tileAt(event);
		// The other end of a pen is a rubber, and it comes as button 5.
		const rubber = event.pointerType === "pen" && event.button === 5;
		if ((event.button !== 0 && !rubber) || where === null || tile === null) {
			// The left button with nothing to paint in pans, which is right,
			// and says so, which is what a newcomer needs: a group is
			// selected, or a quad layer with no handle under the pointer.
			if ((event.button === 0 || rubber) && settings.onCannotPaint !== null) {
				settings.onCannotPaint(where === null ? "layer" : "tool");
			}
			doing = "move";
			return;
		}
		from = tile;
		// A held modifier says what this one stroke is; without one it is
		// whatever the brush has been set to, which is painting until somebody
		// says otherwise.
		const asked = rubber ? "erase"
			: event.altKey ? "fill"
			: event.shiftKey ? "grab"
				: (event.ctrlKey || event.metaKey) ? "erase"
					: (settings.mode === null ? "paint" : settings.mode());
		// Nothing in hand draws nothing, so an empty brush grabs instead.
		// That is the rule the native editor has and mappers have in their
		// fingers: Escape empties the brush, and then dragging picks out a
		// rectangle.
		const chosen = asked === "paint" && editor.brushEmpty() ? "grab" : asked;
		// The move tool: a drag inside the selection carries it, a drag
		// anywhere else selects anew. The tiles go into the brush when they
		// are picked up, so the ghost that follows the pointer is what they
		// look like, and putting them down is a paint.
		if (chosen === "move") {
			if (moveBox !== null && moveBox.group === where.group && moveBox.layer === where.layer && insideBox(moveBox, tile)) {
				editor.grab(where.group, where.layer, moveBox.x, moveBox.y, moveBox.width, moveBox.height);
				changed();
				doing = "carry";
				carried = { x: moveBox.x, y: moveBox.y };
				ghost("stamp", where, moveBox);
				return;
			}
			moveBox = null;
			doing = "select";
			editor.mark(where.group, tile.x, tile.y, 1, 1);
			ghost("spot", where, tile);
			return;
		}
		if (chosen !== "paint") {
			doing = chosen;
		} else {
			doing = "paint";
			touched = null;
			touch(tile);
			editor.begin("Draw");
			// The panels are not rebuilt here or on any stamp that follows:
			// the map is drawn by the program the same frame, and the panels
			// catch up once when the stroke is over.
			editor.paint(where.group, where.layer, tile.x, tile.y);
			ghost("stamp", where, tile);
		}
		if (doing !== "paint") {
			// One tile is a rectangle too, and showing it from the first
			// moment says which gesture is under way.
			editor.mark(where.group, tile.x, tile.y, 1, 1);
			ghost(doing === "grab" ? "spot" : doing, where, tile);
		}
	}, { signal: signal });

	canvas.addEventListener("pointermove", event => {
		// Where the pointer is, said on every move whether or not anything is
		// being drawn with it: what is under the pointer is a question about
		// the pointer, not about the stroke.
		const under = tileAt(event);
		if (settings.onHover !== null) {
			settings.onHover(under);
		}
		if (doing === null) {
			hoverGhost(event, under);
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
			const tile = under;
			if (where !== null && tile !== null) {
				touch(tile);
				editor.paint(where.group, where.layer, tile.x, tile.y);
				ghost("stamp", where, tile);
			}
			return;
		}
		if (doing === "carry") {
			const where = target();
			const tile = under;
			if (where !== null && tile !== null && moveBox !== null) {
				const box = { x: carried.x + tile.x - from.x, y: carried.y + tile.y - from.y, width: moveBox.width, height: moveBox.height };
				editor.mark(where.group, box.x, box.y, box.width, box.height);
				ghost("stamp", where, box);
			}
			return;
		}
		// Grabbing, filling and rubbing out are about the rectangle the
		// pointer ends on, so while it is moving the rectangle is what there
		// is to show - and, inside it, what will happen to it.
		const where = target();
		const tile = under;
		if (where !== null && tile !== null) {
			const box = between(from, tile);
			editor.mark(where.group, box.x, box.y, box.width, box.height);
			ghost(doing === "grab" ? "spot" : doing, where, box);
		}
	}, { signal: signal });

	canvas.addEventListener("pointerleave", () => {
		if (settings.onHover !== null) {
			settings.onHover(null);
		}
		if (doing === null) {
			ghost(null);
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
		} else if (where !== null && tile !== null && doing === "select") {
			// The selection stays marked, for the next drag to pick up.
			moveBox = Object.assign({ group: where.group, layer: where.layer }, between(from, tile));
			doing = null;
			editor.mark(where.group, moveBox.x, moveBox.y, moveBox.width, moveBox.height);
			hoverGhost(event);
			capture(event.pointerId, false);
			return;
		} else if (where !== null && tile !== null && doing === "carry" && moveBox !== null) {
			const box = { x: carried.x + tile.x - from.x, y: carried.y + tile.y - from.y, width: moveBox.width, height: moveBox.height };
			if (box.x !== moveBox.x || box.y !== moveBox.y) {
				// Taken away and put down as one step: what was under the
				// pointer when it went down is what lands where it comes up.
				editor.begin("Move tiles");
				editor.erase(where.group, where.layer, moveBox.x, moveBox.y, moveBox.width, moveBox.height);
				editor.paint(where.group, where.layer, box.x, box.y);
				afterStroke(where, { x: Math.min(box.x, moveBox.x), y: Math.min(box.y, moveBox.y),
					width: Math.abs(box.x - moveBox.x) + box.width, height: Math.abs(box.y - moveBox.y) + box.height });
				editor.commit();
			}
			moveBox = Object.assign({ group: where.group, layer: where.layer }, box);
			doing = null;
			editor.mark(where.group, moveBox.x, moveBox.y, moveBox.width, moveBox.height);
			hoverGhost(event);
			capture(event.pointerId, false);
			return;
		} else if (where !== null && tile !== null && (doing === "grab" || doing === "erase" || doing === "fill")) {
			const box = between(from, tile);
			if (doing === "grab") {
				editor.grab(where.group, where.layer, box.x, box.y, box.width, box.height);
				// The one change here the program does not announce: the brush
				// is not part of the map.
				changed();
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
			}
		}
		doing = null;
		editor.mark();
		// The pointer is still there, so the ghost goes back to what a hover
		// shows - which after a grab is the brush that was just taken.
		hoverGhost(event);
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
		}
		if (doing === "paint") {
			// A pointer that was taken away mid-stroke leaves what it has
			// painted: throwing it out would be a surprise, and the one entry
			// it made is one undo away.
			editor.commit();
		}
		doing = null;
		editor.mark();
		ghost(null);
	}, { signal: signal });

	canvas.addEventListener("wheel", event => {
		event.preventDefault();
		const at = atCanvas(event);
		editor.zoomAt(at.x, at.y, event.deltaY > 0 ? WHEEL_ZOOM_STEP : 1 / WHEEL_ZOOM_STEP);
		moved();
	}, { signal: signal, passive: false });
	// Lets go of the move tool's selection, for whoever knows it no longer
	// holds: Escape, another tool, a step through the history.
	const letGo = () => {
		if (moveBox !== null) {
			moveBox = null;
			editor.mark();
		}
	};
	return { destroy: () => stopping.abort(), letGo: letGo };
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

// Where an element with `remember` keeps the keys somebody set, and whether
// the line over the map for the first stroke has been seen.
const KEYS_STORAGE = "ddnet-editor-keys";
// And where it keeps the layout somebody dragged together, and the settings.
const LAYOUT_STORAGE = "ddnet-editor-layout";
const SETTINGS_STORAGE = "ddnet-editor-settings";
const HINT_STORAGE = "ddnet-editor-hinted";
// Each element's description of its map needs a name no other element has.
let mapHelpCount = 0;
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
// The sizes a tile can be drawn at in the tileset beside the map: fitted to
// the column, then pixels a tile with the box scrolling.
const TILE_ZOOMS = [null, 32, 48, 64];
const TILE_ZOOM_STORAGE = "ddnet-editor-tile-zoom";

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
	background: var(--bg-0, #16161a);
	color: var(--text-1, #f1f1f4);
	font: var(--type-md, 400 13px/18px system-ui, sans-serif);
	overflow: hidden;
}

:host([hidden]) {
	display: none;
}

.box {
	display: grid;
	grid-template-columns: auto auto minmax(0, 1fr) auto;
	grid-template-rows: auto auto minmax(0, 1fr) auto auto;
	/* The dock lies under the map and the inspector only: the right column
	   keeps its whole height, so that opening the strip does not crush the
	   tileset. */
	grid-template-areas:
		"head head head head"
		"tools tools tools tools"
		"rail left map right"
		"rail dock dock right"
		"status status status status";
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
   "grid-area: map" instead of its own column. */
:host([data-left="drawer"]) .area.left,
:host([data-right="drawer"]) .area.right {
	grid-area: map;
	width: min(320px, 85%);
	z-index: 2;
}

:host([data-left="drawer"]) .area.left {
	justify-self: start;
}

:host([data-right="drawer"]) .area.right {
	justify-self: end;
}

/* Too short for the page's own row above the tools. */
:host([data-head="one"]) .area.head {
	display: none;
}

/* Too short for a line of its own: the status becomes a chip in the corner of
   the map, and the row it had collapses because the box left it. */
:host([data-status="chip"]) .area.status {
	grid-area: map;
	align-self: end;
	justify-self: start;
	max-width: 70%;
	z-index: 2;
	background: none;
	border-top: 0;
}

/* And the dock lies over the foot of the map instead of pushing it up. */
:host([data-dock="overlay"]) .area.dock {
	grid-area: map;
	align-self: end;
	height: 200px;
	z-index: 2;
}

/* Above the areas and over all of them, for what floats. The layer itself
   catches nothing - only what is put in it does. */
.over {
	position: absolute;
	inset: 0;
	z-index: 5;
	pointer-events: none;
}

/* An area nobody filled takes no room at all. */
.area.empty {
	display: none;
}

.head { grid-area: head; }
.tools { grid-area: tools; }
.rail { grid-area: rail; }
.left { grid-area: left; }
.map { grid-area: map; position: relative; }
.right { grid-area: right; }
.dock { grid-area: dock; }

/* The status line, and beside it whatever the page has to say: one row. */
.status {
	grid-area: status;
	display: flex;
	align-items: center;
	background: var(--bg-0, #16161a);
	border-top: 1px solid var(--line, #2f2f38);
}

.status ::slotted(:not(.editor-panels)) {
	flex: none;
	padding: 0 12px;
}

/* A short box has a chip instead of a line, and the chip is the editor's. */
:host([data-status="chip"]) .status ::slotted(:not(.editor-panels)) {
	display: none;
}
`;

const BOX_HTML = `
<div class="box" data-role="box">
	<div class="area head"><slot name="header"></slot></div>
	<div class="area tools"><slot name="toolbar"></slot></div>
	<div class="area rail"><slot name="rail"></slot></div>
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
		// The map takes the keyboard, so it has to be able to hold it. It is
		// an application to a screen reader, whose own keys would otherwise
		// never reach it; what it answers to, and the way out, is said in a
		// description beside it.
		this.editorCanvas.tabIndex = 0;
		this.editorCanvas.setAttribute("role", "application");
		this.editorCanvas.setAttribute("aria-label", "Map");
		this.mapHelp = document.createElement("p");
		this.mapHelp.hidden = true;
		this.mapHelp.id = `ddnet-editor-map-help-${++mapHelpCount}`;
		this.editorCanvas.setAttribute("aria-describedby", this.mapHelp.id);
		this.mapBox.append(this.editorCanvas, this.mapHelp);
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
		for (const [area, name] of [["toolbar", "toolbar"], ["rail", "railbox"], ["left", "left"], ["right", "right"], ["dock", "dock"], ["status", "statusbar"]]) {
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

	/**
	 * What the map says it answers to, for whoever cannot see it: the keys
	 * as they are now, which are not always the table's.
	 */
	describeMap() {
		const panels = this.editorPanels;
		if (panels === null) {
			return;
		}
		const key = id => {
			const command = panels.commands.find(one => one.id === id);
			return command === undefined || command.keys.length === 0 ? null : keyLabel(command.keys[0]);
		};
		const said = [
			["palette.open", "finds any command by its name"],
			["edit.undo", "undoes"],
			["layer.next", "goes to the layer below"],
			["focus.next", "goes to the next area"],
			["edit.escape", "comes back to the map"],
		].filter(([id]) => key(id) !== null).map(([id, what]) => `${key(id)} ${what}`);
		this.mapHelp.textContent = `The pointer paints with the brush and the mouse wheel zooms. ${said.join(", ")}.`;
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
			inspector: readonly ? "shut" : wide.inspector,
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
		this.dataset.inspector = now.inspector;
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
		const steering = steerWithPointer(instance, {
			canvas: this.editorCanvas,
			// An editor that is only to be looked at has no layer to paint in,
			// which is the one place that has to say so - the pointer does not
			// go through `run`, where everything else is refused.
			target: () => (panels.readonly ? null : panels.selection),
			mode: () => panels.tool,
			penHoldsPaper: () => panels.penHoldsPaper,
			paintable: () => {
				const layer = panels.selectedLayer();
				return layer !== null && layer.type === "tiles";
			},
			// Once per frame however many things changed in it: the program
			// says so too, a frame later, and the two land in one rebuild.
			onChange: () => panels.refreshSoon(),
			// Panning and zooming change nothing about the map, so the panels
			// are left alone - but what is drawn over the canvas is now over
			// the wrong place.
			onView: () => panels.refreshOverlay(),
			onHover: tile => panels.hoverAt(tile),
			// A tap that answers a question the panels asked.
			onAsk: spot => panels.answerHere(spot),
			// The eyedropper, and the stroke that had nothing to paint in.
			onPick: spot => panels.pickAt(spot),
			onCannotPaint: why => panels.cannotPaint(why),
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
		panels.pointerLetGo = steering.letGo;
		this.applyTheme();
		// Whatever has been changed goes into the browser's own storage every
		// minute - a safety net, not a place to keep a map.
		if (this.hasAttribute("remember")) {
			instance.autosave(60);
			// And the keys somebody set, which are theirs rather than a map's.
			try {
				panels.applyKeys(JSON.parse(localStorage.getItem(KEYS_STORAGE) || "null"));
				const zoom = localStorage.getItem(TILE_ZOOM_STORAGE);
				if (zoom !== null && TILE_ZOOMS.includes(zoom === "null" ? null : Number(zoom))) {
					panels.tileZoom = zoom === "null" ? null : Number(zoom);
					panels.refreshTiles();
				}
			} catch (error) {
				// Nothing kept, or something that is not keys: the table's.
			}
			// And the layout dragged together, and the settings - whatever
			// became of the keys.
			panels.loadKept();
			this.addEventListener("editor-keys", event => {
				try {
					if (Object.keys(event.detail.keys).length === 0) {
						localStorage.removeItem(KEYS_STORAGE);
					} else {
						localStorage.setItem(KEYS_STORAGE, JSON.stringify(event.detail.keys));
					}
				} catch (error) {
					// A browser that keeps nothing forgets the keys with the tab.
				}
			}, { signal: signal });
		}
		this.describeMap();
		this.addEventListener("editor-keys", () => this.describeMap(), { signal: signal });
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
