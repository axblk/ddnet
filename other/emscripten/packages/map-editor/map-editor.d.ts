/**
 * Types for `@ddnet/map-editor`. What the program answers is JSON, so what is
 * described here is the shape of that JSON - a page that reads a field which
 * is not in it gets `undefined` rather than a wrong number.
 */

/** Which map a call is about. Left out, it is the one in front. */
export type MapId = number | undefined;

export interface LayerBase {
	name: string;
	detail: boolean;
}

export interface TileLayer extends LayerBase {
	type: "tiles";
	kind: "tiles" | "game" | "front" | "tele" | "speedup" | "switch" | "tune";
	size: [number, number];
	image: number;
	color: [number, number, number, number];
	colorEnvelope: number;
	colorEnvelopeOffset: number;
	automapperConfig: number;
	automapperSeed: number;
	automapperAutomatic: boolean;
	/** Whether game tiles can be built from this layer's tiles. */
	construct: boolean;
}

export interface QuadLayer extends LayerBase {
	type: "quads";
	image: number;
	/** How many there are; the quads themselves are drawn, not listed. */
	quads: number;
}

export interface SoundLayer extends LayerBase {
	type: "sounds";
	sound: number;
	sources: number;
}

export type Layer = TileLayer | QuadLayer | SoundLayer;

export interface Group {
	name: string;
	offset: [number, number];
	parallax: [number, number];
	useClipping: boolean;
	clip: [number, number, number, number];
	layers: Layer[];
}

export interface Structure {
	info: { author: string; mapVersion: string; credits: string; license: string; settings: string[] };
	groups: Group[];
	envelopes: { name: string; channels: number; synchronized: boolean; points: number }[];
	images: { name: string; external: boolean; size: [number, number] }[];
	sounds: { name: string; external: boolean; bytes: number }[];
}

export interface Quad {
	/** Five points as ten numbers: four corners then the pivot, in world units. */
	points: number[];
	/** Four colours as sixteen numbers, each 0 to 255. */
	colors: number[];
	/** Four places in the picture as eight numbers; 1024 is the whole picture. */
	texcoords: number[];
	posEnv: number;
	posEnvOffset: number;
	colorEnv: number;
	colorEnvOffset: number;
}

/** Proof mode: a rectangle as left, top, right, bottom in world units. */
export type ProofRect = [number, number, number, number];

export interface Proof {
	menu: boolean;
	/** Where the camera stands, in the game layer's coordinates. */
	center: [number, number];
	/** Twenty-one shapes from square to 16:9; together they are one outline. */
	steps: ProofRect[];
	/** The two shapes a mapper is told to check, with their names. */
	named: { name: string; rect: ProofRect }[];
	/** Where a menu background can stand in this map; empty outside menu mode. */
	positions: { index: number; position: [number, number] }[];
}

/** What one map took from another. */
export interface AppendReport {
	name: string;
	groups: number;
	images: number;
	/** Pictures the map already had, byte for byte, so they were not added. */
	sharedImages: number;
	/** Pictures whose name was taken by a different picture. */
	renamedImages: number;
	sounds: number;
	envelopes: number;
	settings: number;
}

/** One thing a map may say to a server. */
export interface MapSetting {
	name: string;
	help: string;
	/** A variable takes one number and has a range; a command takes what its args say. */
	variable: boolean;
	default?: number;
	range?: [number, number];
	/** `i` a whole number, `f` a number, `s` a word, `r` the rest of the line. */
	args: { name: string; type: string; optional: boolean }[];
}

export interface SoundSource {
	/** Where it is, in world units. */
	position: [number, number];
	shape: "circle" | "rectangle";
	/** How far it carries, in world units; only a circle has one. */
	radius?: number;
	/** How wide and how tall, in world units; only a rectangle has one. */
	size?: [number, number];
	loop: boolean;
	pan: boolean;
	timeDelay: number;
	falloff: number;
	posEnv: number;
	posEnvOffset: number;
	soundEnv: number;
	soundEnvOffset: number;
}

export interface Envelope {
	name: string;
	channels: number;
	synchronized: boolean;
	points: {
		/** Whole milliseconds. */
		time: number;
		curve: number;
		/** One per channel, in the map's 22.10 fixed point. */
		values: number[];
		/** Tangents, two numbers per channel. */
		in: number[];
		out: number[];
	}[];
}

export interface History {
	current: number;
	canUndo: boolean;
	canRedo: boolean;
	bytes: number;
	maxBytes: number;
	/** Whether a change is half made right now - a slider being dragged. */
	editing: boolean;
	entries: { label: string; timeNanos: number }[];
}

/** What a command is. The `op` decides which of the rest it wants. */
export type Command =
	| { op: "group.add"; name?: string; label?: string }
	| { op: "group.delete"; group: number; label?: string }
	| { op: "group.move"; group: number; to: number; label?: string }
	| { op: "group.setProp"; group: number; prop: string; value: unknown; label?: string }
	| { op: "layer.add"; group: number; type?: "tiles" | "quads" | "sounds"; kind?: string; width?: number; height?: number; name?: string; label?: string }
	| { op: "layer.delete"; group: number; layer: number; label?: string }
	| { op: "layer.move"; group: number; layer: number; toGroup: number; to: number; label?: string }
	| { op: "layer.resize"; group: number; layer: number; width?: number; height?: number; label?: string }
	| { op: "layer.type"; group: number; layer: number; x: number; y: number; text: string; label?: string }
	| { op: "layer.constructGameTiles"; group: number; layer: number; tile: string; label?: string }
	| { op: "quad.setTexcoord"; group: number; layer: number; quad: number; corner: number; u: number; v: number; label?: string }
	| { op: "quad.carve"; group: number; layer: number; quad: number; points: number[]; label?: string }
	| { op: "quad.shape"; group: number; layer: number; quad: number; shape: "square" | "aspect" | "centerPivot" | "align"; grid?: number; label?: string }
	| { op: "source.add"; group: number; layer: number; x: number; y: number; radius?: number; label?: string }
	| { op: "source.delete"; group: number; layer: number; source: number; label?: string }
	| { op: "source.setPoint"; group: number; layer: number; source: number; x: number; y: number; label?: string }
	| { op: "source.setProp"; group: number; layer: number; source: number; prop: string; value: unknown; label?: string }
	| { op: "layer.setProp"; group: number; layer: number; prop: string; value: unknown; label?: string }
	| { op: "quad.add"; group: number; layer: number; x: number; y: number; width?: number; height?: number; label?: string }
	| { op: "quad.delete"; group: number; layer: number; quad: number; label?: string }
	| { op: "quad.setPoint"; group: number; layer: number; quad: number; point: number; x: number; y: number; label?: string }
	| { op: "quad.setColor"; group: number; layer: number; quad: number; corner: number; value: number[]; label?: string }
	| { op: "quad.setProp"; group: number; layer: number; quad: number; prop: string; value: number; label?: string }
	| { op: "info.setProp"; prop: "author" | "mapVersion" | "credits" | "license"; value: string; label?: string }
	| { op: "info.settings.add"; value: string; label?: string }
	| { op: "info.settings.set"; line: number; value: string; label?: string }
	| { op: "info.settings.delete"; line: number; label?: string }
	| { op: "image.add"; name: string; width?: number; height?: number; label?: string }
	| { op: "image.delete"; image: number; label?: string }
	| { op: "image.setProp"; image: number; prop: "name" | "external"; value: string | boolean; label?: string }
	| { op: "sound.add"; name: string; label?: string }
	| { op: "sound.delete"; sound: number; label?: string }
	| { op: "sound.setProp"; sound: number; prop: "name" | "external"; value: string | boolean; label?: string }
	| { op: "envelope.add"; name?: string; channels?: number; label?: string }
	| { op: "envelope.delete"; envelope: number; label?: string }
	| { op: "envelope.setProp"; envelope: number; prop: string; value: unknown; label?: string }
	| { op: "envelope.point.add"; envelope: number; time: number; values: number[]; curve?: number; label?: string }
	| { op: "envelope.point.delete"; envelope: number; point: number; label?: string }
	| { op: "envelope.point.set"; envelope: number; point: number; time?: number; values?: number[]; curve?: number; label?: string }
	| { op: "history.undo" }
	| { op: "history.redo" }
	| { op: "history.jump"; index: number };

export type Answer = { ok: true; [key: string]: unknown } | { ok: false; error: string };

export declare class MapEditor {
	static open(options: Record<string, unknown>): Promise<MapEditor>;
	static openPage(options: Record<string, unknown>): Promise<MapEditor>;

	readonly canvas: HTMLCanvasElement | null;
	/** The number of the map in front, or -1 while none is open. */
	readonly map: number;
	readonly maps: number[];

	name(id?: MapId): string;
	activate(id: number): boolean;
	close(id?: MapId): boolean;
	create(width: number, height: number, name?: string): number | null;
	save(id?: MapId, options?: { handout?: boolean }): boolean;
	/** Whether the map has been changed since it was last written out. */
	dirty(id?: MapId): boolean;
	/** Writes every changed map into the browser's own storage every so many seconds. */
	autosave(seconds: number): void;

	structure(id?: MapId): Structure | null;
	/** The pixels of a picture packed into the map file, or null for one beside it. */
	imageData(index: number, id?: MapId): ImageData | null;
	/** The quads of one layer, points in world units; null for a layer without them. */
	quads(group: number, layer: number, id?: MapId): Quad[] | null;
	/** Puts handles on one quad's corners, or takes them away when called with nothing. */
	showQuad(group?: number, layer?: number, quad?: number, id?: MapId): void;
	/** Where a canvas point is in one group's coordinates, in world units. */
	groupWorldAt(group: number, x: number, y: number, id?: MapId): { x: number; y: number } | null;
	/** What tile stands in one place of a layer, or -1 where there is none. */
	tileIndex(group: number, layer: number, x: number, y: number, id?: MapId): number;
	/** Keeps a `.rules` file under a name; answers how many configurations it holds. */
	loadRules(name: string, text: string): number;
	/** The sound sources of one layer, or null for a layer that holds none. */
	sources(group: number, layer: number, id?: number): SoundSource[] | null;
	/** What a player would see from where the view is looking, in world units. */
	proof(menu?: boolean, id?: MapId): Proof | null;
	/** What a tile of a physics layer does, or "" where there is nothing to say. */
	explain(group: number, layer: number, index: number, id?: MapId): string;
	/** Everything a map may say to a server, asked once and kept. */
	settingsHelp(): MapSetting[];
	/** What is wrong with each settings line, and where each repeats an earlier one. */
	settingProblems(id?: MapId): { problem: string; repeats: number }[];
	/** The names of settings that begin with what has been typed. */
	settingNames(prefix: string): string[];
	/** What is wrong with one settings line, or "" where nothing is. */
	checkSetting(line: string): string;
	/** Puts a second map's groups, assets and settings into this one; one history entry. */
	appendFile(file: File, id?: MapId): Promise<AppendReport | null>;

	/** Where a place in one group's coordinates is on the canvas, in pixels. */
	groupPixelAt(group: number, x: number, y: number, id?: number): { x: number; y: number } | null;

	/** Which lines of a rules file were passed over, counting from one. */
	ruleProblems(name: string): number[];

	/** What the configurations of a rules file that was loaded are called. */
	ruleConfigs(name: string): string[];
	/** Runs one configuration over a layer, or over a piece of it, as one history entry. */
	automap(group: number, layer: number, rules: string, config: number, options?: {
		seed?: number; reference?: number; x?: number; y?: number; width?: number; height?: number; id?: MapId;
	}): boolean;
	/** Puts a picture with its pixels into the map; answers which picture it became, or -1. */
	addImage(name: string, pixels: ImageData, id?: MapId): number;
	/** A picture as tile layers drawn with palettes of its own colours; answers the group, or -1. */
	addTileArt(name: string, pixels: ImageData, id?: MapId): number;
	/** How many colours a picture holds, not counting what is not opaque. */
	artColors(pixels: ImageData): number;
	/** A picture as a group of quads, one per pixel or per run of one colour. */
	addQuadArt(name: string, pixels: ImageData, options?: {
		pixelStep?: number; quadSize?: number; centralize?: boolean; merge?: boolean;
	}, id?: MapId): number;
	/** Puts other pixels into a picture the map has, keeping the layers drawn with it. */
	setImagePixels(index: number, pixels: ImageData, id?: MapId): boolean;
	/** Puts a sound with its bytes into the map; answers which sound it became, or -1. */
	addSound(name: string, bytes: Uint8Array | ArrayBuffer, id?: MapId): number;
	/** Puts other bytes into a sound the map has, keeping the layers that play it. */
	setSoundData(index: number, bytes: Uint8Array | ArrayBuffer, id?: MapId): boolean;
	/** The bytes of a sound packed into the map file, or null for one beside it. */
	soundData(index: number, id?: MapId): Uint8Array | null;
	/** The lowest number a physics layer is not using yet, or -1 when all are taken. */
	nextFreeNumber(group: number, layer: number, checkpoint?: boolean, id?: MapId): number;
	/** Moves the view to the `which`-th place a number is used; answers how many there are. */
	gotoNumber(group: number, layer: number, number: number, which: number, id?: MapId): number;
	/** Whether the tiles in hand are tele checkpoints, which count their numbers apart. */
	brushCheckpoint(): boolean;
	/** The points of one envelope; times in ms, values in 22.10 fixed point. */
	envelope(index: number, id?: MapId): Envelope | null;
	history(id?: MapId): History | null;
	apply(command: Command, id?: MapId): Answer;
	undo(id?: MapId): Answer;
	redo(id?: MapId): Answer;
	jump(index: number, id?: MapId): Answer;

	begin(label?: string, id?: MapId, merge?: string): void;
	commit(id?: MapId): void;
	abort(id?: MapId): void;

	setSize(width: number, height: number): void;
	center(id?: MapId): { x: number; y: number } | null;
	center(x: number, y: number, id?: MapId): void;
	zoom(value?: number, id?: MapId): number | void;
	moveByPixels(dx: number, dy: number, id?: MapId): void;
	zoomAt(x: number, y: number, factor: number, id?: MapId): void;
	fit(id?: MapId): void;
	size(id?: MapId): { width: number; height: number } | null;
	worldAt(x: number, y: number, id?: MapId): { x: number; y: number } | null;
	tileAt(x: number, y: number, id?: MapId): { x: number; y: number } | null;

	pickTiles(group: number, layer: number, x: number, y: number, width?: number, height?: number, id?: MapId): boolean;
	grab(group: number, layer: number, x: number, y: number, width: number, height: number, id?: MapId): boolean;
	paint(group: number, layer: number, x: number, y: number, id?: MapId): boolean;
	fill(group: number, layer: number, x: number, y: number, width: number, height: number, id?: MapId): boolean;
	erase(group: number, layer: number, x: number, y: number, width: number, height: number, id?: MapId): boolean;
	flipBrushX(): void;
	flipBrushY(): void;
	rotateBrush(): void;
	storeBrush(slot: number): boolean;
	useBrush(slot: number): boolean;
	brushSize(): { width: number; height: number } | null;
	/** What goes beside a physics tile the brush puts down. */
	numbers(): { number: number; delay: number; force: number; maxSpeed: number; angle: number };
	numbers(values: { number?: number; delay?: number; force?: number; maxSpeed?: number; angle?: number }): void;

	highDetail(on?: boolean, id?: MapId): boolean | void;
	entities(value?: boolean | number, id?: MapId): number | void;
	animate(on?: boolean, id?: MapId): boolean | void;
	/** Whether a layer is drawn. Changes nothing about the map itself. */
	visible(group: number, layer: number, on?: boolean, id?: MapId): boolean | void;
	/** Marks a rectangle of tiles, or takes the mark away when called with nothing. */
	mark(group?: number, x?: number, y?: number, width?: number, height?: number, id?: MapId): void;
	/** How many tiles apart the lines of the grid are, 0 for no grid. */
	grid(spacing?: number | boolean, id?: MapId): number | void;
	loading(): boolean;

	loadFile(file: File): Promise<string>;
	loadUrl(url: string | URL): Promise<string>;
	addEventListener(type: string, listener: (event: CustomEvent) => void, options?: AddEventListenerOptions): void;
}

export declare class EditorPanels {
	constructor(editor: MapEditor, options?: {
		container?: Element | null;
		dataBase?: string | null;
		/** Whether the panels listen for keys on the whole page themselves. */
		keys?: boolean;
		signal?: AbortSignal;
	});
	/** What holds the panels: the one column, or the box they were spread into. */
	readonly element: HTMLElement;
	/** Which group, and which layer of it, or `layer: -1` for the group. */
	selection: { group: number; layer: number };
	/** Which panel each area that shows one at a time has in front. */
	readonly tab: { left: string; dock: string; tiles: string };
	/** One of the panels' parts by the name it carries, wherever it stands. */
	part(role: string): Element | null;
	/** Every part of that name. */
	parts(role: string): Element[];
	/** Puts one of an area's panels in front, opening the area if it was shut. */
	showTab(area: "left" | "dock", tab: string): void;
	refresh(): void;
	destroy(): void;
}

export declare function steerEditor(
	editor: MapEditor,
	options?: {
		canvas?: HTMLCanvasElement | null;
		/** Which layer the pointer paints in - the panels know. */
		target?: (() => { group: number; layer: number } | null) | null;
		onChange?: (() => void) | null;
		/**
		 * Called with a click in the painted layer's own coordinates before
		 * anything else is done with it. Answering `true` takes the click.
		 */
		onClickInGroup?: ((world: { x: number; y: number }, where: { group: number; layer: number }) => boolean) | null;
		/** Called with the tile under the pointer on every move, or null once it has left. */
		onHover?: ((tile: { x: number; y: number } | null) => void) | null;
		/** Called when only the view moved - panned or zoomed. */
		onView?: (() => void) | null;
		/** Called while a stroke's change is still open. */
		afterStroke?: ((where: { group: number; layer: number }, box: { x: number; y: number; width: number; height: number }) => void) | null;
		signal?: AbortSignal;
	},
): { destroy(): void };

/**
 * `<ddnet-editor>` - the whole editor as one element, laid out in six areas
 * a page may fill with `slot="header"`, `"toolbar"`, `"left"`, `"right"`,
 * `"dock"` and `"status"`.
 *
 * Attributes: `src`, `urlparam`, `theme="light"`, `remember`.
 */
export declare class EditorElement extends HTMLElement {
	/** The running program, or null until it is. */
	readonly editor: MapEditor | null;
	/** The panels beside the map, or null until they are there. */
	readonly panels: EditorPanels | null;
	/** The canvas the map is drawn on; there before the program is. */
	readonly canvas: HTMLCanvasElement;
	/** Waits for the program, and says what stopped it if it did not start. */
	readonly ready: Promise<MapEditor> | null;
	/** One of this editor's parts by the name it carries in `data-role`. */
	part(role: string): Element | null;
}

export declare const programUrl: string;

declare const _default: {
	MapEditor: typeof MapEditor;
	EditorPanels: typeof EditorPanels;
	EditorElement: typeof EditorElement;
	steerEditor: typeof steerEditor;
	programUrl: string;
	base: unknown;
};
export default _default;
