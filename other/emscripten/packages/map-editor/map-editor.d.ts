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
	| { op: "layer.setProp"; group: number; layer: number; prop: string; value: unknown; label?: string }
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
	constructor(editor: MapEditor, options?: { container?: Element | null; dataBase?: string | null; signal?: AbortSignal });
	readonly element: HTMLElement;
	/** Which group, and which layer of it, or `layer: -1` for the group. */
	selection: { group: number; layer: number };
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
		signal?: AbortSignal;
	},
): { destroy(): void };

export declare const programUrl: string;

declare const _default: {
	MapEditor: typeof MapEditor;
	EditorPanels: typeof EditorPanels;
	steerEditor: typeof steerEditor;
	programUrl: string;
	base: unknown;
};
export default _default;
