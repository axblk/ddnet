// Types of `ddnet-base.js`, the part the DDNet web packages share. The
// documentation of the API is here and nowhere else.

/** What this library refuses or cannot do, with a `code` to branch on. */
export declare class DDNetBaseError extends Error {
	/**
	 * What went wrong, as one word that stays when the sentence changes:
	 * `BadOption`, `CrossOriginRefused`, `NoWebGpu`, `NoVideoEncoder`,
	 * `FileRefused`, `FileTooLarge`, `FetchFailed`, `RenderFailed`,
	 * `ZipTooLarge`, `Stopped`.
	 */
	readonly code: string;
	constructor(code: string, message: string);
}

/** A program's factory, as Emscripten writes it. */
export type ProgramFactory = (moduleArg?: Record<string, unknown>) => Promise<EmscriptenModule>;

/** Emscripten's own object, as far as this library touches it. */
export interface EmscriptenModule {
	FS: any;
	arguments: string[];
	callMain(args: string[]): void;
	ccall(name: string, returnType: string | null, argTypes: string[], args: unknown[]): any;
	[key: string]: any;
}

/** How a line of output was meant. */
export interface OutputKind {
	error?: boolean;
	bold?: boolean;
	/** The line that ended the program. */
	fatal?: boolean;
	color?: string;
}

/** Where a video is written while it is made. */
export interface VideoSink {
	stream: WritableStream;
	/** The finished file, for a sink that has it in hand. */
	done?: () => Promise<File | Blob | null> | File | Blob | null;
}

export interface ProgramOptions {
	/** The factory, for a class that does not name its own script. */
	module?: ProgramFactory;
	/** Where the program's script is, for one that is not beside the package. */
	scriptUrl?: string;
	/** Where `data` is, beside the script otherwise. */
	dataBase?: string;
	/** What the program is called in its output. */
	programName?: string;
	/**
	 * Where the program draws. A web tool hands it to the thread it draws on,
	 * after which the page cannot size it through `width` and `height` or draw
	 * on it: the size goes through the program's `setSize` (see `followSize`),
	 * and a canvas serves one program. It needs no id.
	 */
	canvas?: HTMLCanvasElement | null;
	/** What the program's fullscreen button fills, the page otherwise. */
	fullscreenElement?: Element;
	/** Where the user's files live, `/home/web_user/.local/share/ddnet` by default. */
	homePath?: string;
	/** The file endings this program takes, the class's own by default. */
	accept?: string[];
	/** Whether `ddnet://` links may be dropped on the canvas as well. */
	acceptLinks?: boolean;
	/** A file to start with: its URL or its content. */
	file?: string | File | Blob | ArrayBuffer | Uint8Array;
	fileName?: string;
	/** What goes in front of the file on the command line. */
	fileArgument?: string;
	/** More for the command line. */
	arguments?: string[];
	/** Whether the program draws its own controls, which the web tools never do. */
	controls?: boolean;
	/**
	 * Whether the program refuses to start without a WebGPU adapter. None of
	 * the packages' programs do: they draw with WebGL 2 without one.
	 */
	needsWebGpu?: boolean;
	/** Whether what the program writes is kept in IndexedDB. On by default. */
	persist?: boolean;
	/** Where videos go, a scratch file that is then offered otherwise. */
	videoSink?: VideoSink | WritableStream;
	/** Whether scratch files of earlier videos are deleted at the start. */
	sweepVideoScratch?: boolean;
	/** Stops the program. */
	signal?: AbortSignal;
}

/**
 * One program on one canvas. Events: `output` ({message, kind}), `progress`
 * ({text}), `loadstart` ({url}, `null` for a file), `renderprogress`,
 * `video` ({file, name}; cancel it to keep the file rather than have it
 * offered as a download), `exit`.
 */
export declare class Program extends EventTarget {
	/** The script, relative to `base`, and the factory's name in it. */
	static script: string | null;
	static base: string;
	static moduleName: string | null;
	static programName: string | null;
	/** The file ending it opens. */
	static suffix: string | null;
	static scriptUrl(): string | null;
	/** `new this(options).start()`. */
	static open<T extends Program>(this: new (options: ProgramOptions) => T, options?: ProgramOptions): Promise<T>;

	constructor(options?: ProgramOptions);
	readonly options: ProgramOptions;
	readonly canvas: HTMLCanvasElement | null;
	readonly module: EmscriptenModule | null;
	readonly exited: boolean;
	/** Settles once the program has stopped, however it stopped. */
	readonly finished: Promise<void>;
	/** Loads the program and runs it. */
	start(): Promise<this>;
	/** Calls an exported function. `null` once there is nothing to call. */
	call(name: string, returnType?: string | null, argTypes?: string[], args?: unknown[]): any;
	loadFile(file: File): Promise<string>;
	/** Fetches a file and opens it; the server has to allow CORS. */
	loadUrl(url: string): Promise<string>;
	/** Where the next video goes; set from the click that asks for it. */
	setVideoSink(sink: VideoSink | WritableStream): void;
	quit(): void;
	/** Stops the program and takes everything it put on the page off again. */
	destroy(): void;
	/** Keys that go to the page rather than to the program. */
	passesKey(event: KeyboardEvent): boolean;
}

/**
 * What the viewer elements are made of: a canvas in a shadow root
 * (`::part(picture)`), a message over it (`::part(message)`), and a program
 * started on it once the element is in a page. Controls for it go into the
 * `controls` slot.
 *
 * Attributes: `src`, `controls` (the package's own bar, whatever the value;
 * the programs draw none of their own), `link` (keeps the page address in step
 * with what is shown), `base` (where the program's script is), `data` (where
 * its data is). Events: those of the package, and `error` ({message}).
 */
export declare class ViewerElement extends HTMLElement {
	/** The running program, or why it did not start. */
	readonly ready: Promise<Program> | null;
	/** The program, once it runs. */
	readonly program: Program | null;
	/** The package's bar, where `controls="html"` asked for it. */
	readonly bar: { readonly element: HTMLElement; part(role: string): HTMLElement | null; destroy(): void } | null;
	readonly picture: HTMLCanvasElement;
	load(file: File): Promise<string>;
	/** Opens the browser's file chooser. Call it from a click. */
	open(): void;
	/** Shows a line over the picture, `""` takes it away. */
	say(message: string): void;
}

export interface AutoHideOptions {
	/** Milliseconds without use before they go. 3000 by default. */
	delay?: number;
	/** What they lie over: use of it shows them, a tap toggles them. */
	picture?: Element | null;
	/** Keeps them while it answers true, such as while paused. */
	hold?: () => boolean;
	onHide?: (() => void) | null;
	signal?: AbortSignal;
}

/** Controls over a picture that fade out while nothing happens, as a video player's do. */
export declare function autoHide(elements: Element[], options?: AutoHideOptions): { show(): void; hide(): void; shown(): boolean };
/**
 * Tells the program the size of the box its canvas is in, which is how a
 * canvas the program was handed is sized.
 */
export declare function followSize(element: Element, program: { setSize(width: number, height: number): void }, options?: { signal?: AbortSignal }): { stop(): void };
/** Whether the device is steered with a finger: a phone or a tablet, not a laptop with a touch screen. */
export declare function touchDevice(): boolean;
/**
 * On a touch device, fills the screen with `element` (the page by default) and
 * turns it to its long side on the first tap, and on the next one after the
 * user has left again. Does nothing elsewhere, or where the browser cannot.
 */
export declare function fillTouchScreen(options?: { element?: Element; signal?: AbortSignal }): void;
/** Makes `button` fill the screen with `element`, or hides it where that is not possible. */
export declare function fullscreen(button: HTMLElement, options?: { element?: Element; shortcut?: string | null; signal?: AbortSignal }): void;

/** The settings of a video export, named after the render tool's. */
export interface VideoSettings {
	width?: number;
	height?: number;
	fps?: number;
	/** Constant rate factor, lower is better. */
	crf?: number;
	codec?: string | null;
	audio?: boolean;
	hud?: boolean;
	chat?: boolean;
	/** The map's detail layers, as `gfx_high_detail`. On by default. */
	highDetail?: boolean;
	/** The players' names over them, as `cl_nameplates`. On by default. */
	nameplates?: boolean;
	/** Every player's key presses over them. Off by default. */
	keyPresses?: boolean;
}

/**
 * Fields for the video settings in `container`. It fires `change`. With
 * `presets`, a choice of whole sets comes first and the single settings are
 * details under it.
 */
export declare function exportSettingsForm(container: HTMLElement, options?: { canvas?: HTMLCanvasElement | null; audio?: boolean; presets?: boolean }): { values(): VideoSettings; setValues(values: VideoSettings): void };

/** What `DemoPlayer.info` and `DemoRenderer.info` answer. */
export interface DemoInfo {
	file?: string;
	/** Bytes. */
	size?: number;
	version?: number;
	netversion?: string;
	/** `client` or `server`: who recorded it. */
	type?: string;
	/** When it was recorded, as its header says. */
	date?: string;
	/** Milliseconds. */
	length?: number;
	/** Milliseconds from the beginning. */
	markers?: number[];
	map?: MapInfo & { name?: string };
	/** The players it named so far, or at its beginning. */
	players?: { id: number; name: string }[];
}

/** What a map says about itself, and its file. */
export interface MapInfo {
	file?: string;
	/** Bytes. */
	size?: number;
	crc?: string;
	sha256?: string;
	author?: string;
	version?: string;
	credits?: string;
	license?: string;
	/** Server settings the map brings. */
	settings?: number;
	/** Of the game layer, in tiles. */
	width?: number;
	height?: number;
	groups?: number;
	layers?: number;
	images?: number;
	sounds?: number;
	envelopes?: number;
}

export declare function formatBytes(bytes: number): string;
export declare function formatDuration(milliseconds: number): string;
/** The sections `fillInfo` shows for a demo. */
export declare function demoInfoSections(info: DemoInfo): [string, [string | null, unknown][]][];
/** The sections `fillInfo` shows for a map. */
export declare function mapInfoSections(info: MapInfo): [string, [string | null, unknown][]][];
/** Fills a `<dl>` with headed sections of names and values, leaving out what has no value. */
export declare function fillInfo(list: HTMLElement, sections: [string, [string | null, unknown][]][]): void;

/** Adds pictures by name: `{name: "<svg contents>"}` in a 24 by 24 box, in `currentColor`. */
export declare function addIcons(pictures: Record<string, string>): void;
export declare function icon(name: string): SVGSVGElement;
/** Draws the picture named by `data-icon` into every element below `root`. */
export declare function paintIcons(root?: ParentNode): void;

/** Why this browser cannot run the programs, or `null`. */
export declare function supportError(needsWebGpu?: boolean): Promise<DDNetBaseError | null>;
/** A parameter of the page address, after `#` or `?`. */
export declare function urlParameter(name: string): string | null;
/**
 * The connect link that the page address asks for with `#connect=<link>` (the
 * rest of the address) or `?connect=<link>`, for the arguments of the client,
 * or `null`.
 */
export declare function connectLink(address?: { hash: string; search: string }): string | null;
/** Sets parameters after `#` without a new history entry; `null` removes one. */
export declare function setUrlParameters(values: Record<string, string | number | null>): void;

/** Where this module is; a worker has no import map and is told. */
export declare const moduleUrl: string;
/** A script of another origin as a blob, which a cross-origin isolated page lets a worker import. */
export declare function fetchScript(url: string | URL): Promise<Blob>;
/** The factory in a program's script. */
export declare function importProgram(scriptUrl: string, moduleName: string): Promise<ProgramFactory>;
/** The error a signal stops something with. */
export declare function abortError(signal: AbortSignal): unknown;
/** Deletes the scratch files of videos nobody took. */
export declare function sweepVideoScratch(): Promise<void>;
