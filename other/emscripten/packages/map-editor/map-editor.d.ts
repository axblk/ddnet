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
	save(id?: MapId): boolean;

	structure(id?: MapId): Structure | null;
	history(id?: MapId): History | null;
	apply(command: Command, id?: MapId): Answer;
	undo(id?: MapId): Answer;
	redo(id?: MapId): Answer;
	jump(index: number, id?: MapId): Answer;

	begin(label?: string, id?: MapId): void;
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

	highDetail(on?: boolean, id?: MapId): boolean | void;
	entities(value?: boolean | number, id?: MapId): number | void;
	animate(on?: boolean, id?: MapId): boolean | void;
	loading(): boolean;

	loadFile(file: File): Promise<string>;
	loadUrl(url: string | URL): Promise<string>;
	addEventListener(type: string, listener: (event: CustomEvent) => void, options?: AddEventListenerOptions): void;
}

export declare class EditorPanels {
	constructor(editor: MapEditor, options?: { container?: Element | null; signal?: AbortSignal });
	readonly element: HTMLElement;
	refresh(): void;
	destroy(): void;
}

export declare function steerEditor(
	editor: MapEditor,
	options?: { canvas?: HTMLCanvasElement | null; signal?: AbortSignal },
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
