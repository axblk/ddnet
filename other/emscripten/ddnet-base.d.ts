// What `ddnet-base.js` is, in types. Written by hand rather than generated:
// the library is one file of plain JavaScript, and a hand-written declaration
// says what it means as well as what it is - which is what somebody reading it
// in their editor wants.
//
// Kept beside the module, so that the two are changed together.

/** What this library refuses or cannot do, with a `code` to branch on. */
export declare class DDNetBaseError extends Error {
	/**
	 * What went wrong, as one word. Part of the API: the sentence beside it
	 * may be reworded, this may not.
	 *
	 * `BadOption`, `Unsupported`, `FileRefused`, `FileTooLarge`,
	 * `FetchFailed`, `Stopped`, `RenderStopped`, `RenderFailed`.
	 */
	readonly code: string;
}

/** A program's factory, as Emscripten writes it. */
export type ProgramFactory = (moduleArg?: Record<string, unknown>) => Promise<EmscriptenModule>;

/** Emscripten's own object. Only what this library and its pages touch. */
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
	fatal?: boolean;
	color?: string;
}

/** Where a video is written while it is made. */
export interface VideoSink {
	stream: WritableStream;
	/** The finished file, for a sink that has it in hand; nothing otherwise. */
	done?: () => Promise<File | Blob | null> | File | Blob | null;
}

export type VideoSinkSource =
	| VideoSink
	| WritableStream
	| ((info: { fileName: string; type: string; width: number; height: number; fps: number }) => Promise<VideoSink | WritableStream>);

/** What every way in takes. */
export interface StartOptions {
	/** The program's factory, for example `DDNetDemoPlayer`. */
	module: ProgramFactory;
	/** What to call it in what the page says. */
	programName?: string;
	canvas?: HTMLCanvasElement;
	/** Where the user's own files live, `/home/web_user/.local/share/ddnet` by default. */
	homePath?: string;
	/** The file endings this program takes, for example `[".demo"]`. */
	accept?: string[];
	/** Whether `ddnet://` links may be dropped on it as well. */
	acceptLinks?: boolean;
	/** URL parameters to look in for a file to start on, in order. */
	urlParams?: string[];
	/** A file to start on: bytes, a `File`, or a URL. */
	file?: File | Blob | ArrayBuffer | Uint8Array | string;
	fileName?: string;
	/** What to put in front of the file on the command line. */
	fileArgument?: string;
	/** Whether the viewer draws its own controls. A page with its own says `false`. */
	controls?: boolean;
	/**
	 * Whether the demo viewer zooms on the wheel, the zoom keys and a pinch.
	 * A page that scrolls around it says `false`; what it asks for through
	 * `zoom(factor)` works either way.
	 */
	zoom?: boolean;
	/**
	 * Which way round to turn a phone while the viewer's own button fills the
	 * screen. Nothing is turned unless this says so; `"landscape"` is what a
	 * page that wants the most of a wide picture asks for.
	 */
	orientation?: OrientationLockType | "any";
	/**
	 * Where in the demo to start, in seconds, how fast, and whether to stand
	 * still there. Handed to the demo player before it plays anything: a page
	 * that asks for the same thing afterwards only gets a turn once the viewer
	 * runs, and by then the demo has played the seconds that took.
	 */
	startTime?: number;
	speed?: number;
	paused?: boolean;
	/** Anything else for the command line. */
	arguments?: string[];
	/** Where the program's script is, for one that is not beside the page. */
	scriptUrl?: string;
	/** Where `data` is, for a page that keeps it somewhere of its own. */
	dataBase?: string;
	/** Whether what this draws needs WebGPU, which only a render does. */
	needsWebGpu?: boolean;
	/**
	 * Whether what the program writes is kept between visits, in IndexedDB
	 * under the home directory. On by default, which is what the full client
	 * wants; a viewer that writes nothing worth keeping says `false` and saves
	 * itself the mount and the read-back.
	 */
	persist?: boolean;
	/** Where the next video goes, without asking for it each time. */
	videoSink?: VideoSinkSource;
	/** Whether left-over scratch files are swept up at the start. */
	sweepVideoScratch?: boolean;
	/** Stops the program, and refuses to start one that is already stopped. */
	signal?: AbortSignal;
	onOutput?: (message: string, kind: OutputKind) => void;
	onProgress?: (text: string) => void;
	onExit?: () => void;
}

/** The page's own furniture, for `page`. */
export interface PageElements {
	canvas?: HTMLCanvasElement;
	error?: HTMLElement;
	loading?: HTMLElement;
	output?: HTMLElement;
	outputContent?: HTMLElement;
}

export interface PageOptions extends StartOptions {
	elements?: PageElements;
}

/** The settings a video export takes, named as the render tool names them. */
export interface VideoSettings {
	width?: number;
	height?: number;
	fps?: number;
	crf?: number;
	codec?: string;
	audio?: boolean;
	hud?: boolean;
	chat?: boolean;
}

/** How a render is getting on, once a second while it runs. */
export interface RenderProgress {
	frames?: number;
	seconds?: number;
	fraction?: number;
	[key: string]: unknown;
}

/**
 * One running program. Also an `EventTarget`: `output`, `exit`,
 * `renderprogress`.
 */
export declare class Program extends EventTarget {
	/**
	 * What a program is, said by the class rather than by every page that
	 * opens one. A class that leaves them alone is opened with a factory
	 * handed in, which is what the full client does.
	 */
	static script: string | null;
	static base: string;
	static moduleName: string | null;
	static programName: string | null;
	static suffix: string | null;
	/** Where this program's script lies, worked out from the class. */
	static scriptUrl(): string | null;
	/** Starts the program on a canvas and answers with it, running. */
	static open<T extends Program>(this: new (options: StartOptions) => T, options: StartOptions): Promise<T>;
	/** The same, plus the furniture our own pages share around it. */
	static openPage<T extends Program>(this: new (options: PageOptions) => T, options: PageOptions): Promise<T>;

	readonly canvas: HTMLCanvasElement | null;
	readonly module: EmscriptenModule | null;
	readonly exited: boolean;
	/** Settles when the program has stopped, however it stopped. */
	readonly finished: Promise<void>;
	/** Calls into the program. Answers `null` once there is nothing to call. */
	call(name: string, returnType?: string | null, argTypes?: string[], args?: unknown[]): any;
	/** Hands over bytes as though they had been dropped on the page. */
	loadBytes(name: string, data: Uint8Array): Promise<string>;
	loadFile(file: File): Promise<string>;
	/** Fetches a file and hands it over. Only http and https, and only with CORS. */
	loadUrl(url: string): Promise<string>;
	/** Where the next video this program exports is written to. */
	setVideoSink(sink: VideoSink | WritableStream): void;
	/** Asks the program to stop. */
	quit(): void;
	/** Lets go of the page: every handler comes off and the program is stopped. */
	destroy(): void;
}

/** What a demo player can be asked and told. */
export interface Callable {
	call(name: string, returnType?: string | null, argTypes?: string[], args?: unknown[]): any;
}

export interface AutoHideOptions {
	/** How long to wait before they go, in milliseconds. */
	delay?: number;
	/** What they are drawn over. A tap on it shows them or takes them away. */
	picture?: Element | null;
	/** Called whenever they go. */
	onHide?: (() => void) | null;
	/** Takes all of it off the page again. */
	signal?: AbortSignal;
}

export interface AutoHideHandle {
	show(): void;
	hide(): void;
	shown(): boolean;
}

export interface FullscreenOptions {
	/** What to fill the screen with, the page itself otherwise. */
	element?: Element;
	/**
	 * Which way round to turn a phone while the screen is filled. Nothing is
	 * turned unless this says so; anything the Screen Orientation API takes
	 * may be said, `"landscape"` being the one a wide picture wants.
	 */
	orientation?: OrientationLockType | "any";
	signal?: AbortSignal;
}

export interface ExportSettingsFormOptions {
	/** A canvas whose size is offered as well, and chosen to begin with. */
	canvas?: HTMLCanvasElement;
	/** Whether sound starts out switched on. */
	audio?: boolean;
}

export interface ExportSettingsForm {
	values(): VideoSettings;
	setValues(values: VideoSettings): void;
	onChange(listener: () => void): void;
}

/** A video codec this browser can encode with. */
export interface VideoCodec {
	name: string;
	display: string;
}

export declare const version: string;
export declare function supportProblem(needsWebGpu?: boolean): Promise<string | null>;
export declare function supportError(needsWebGpu?: boolean): Promise<DDNetBaseError | null>;
export declare function fullscreen(button: HTMLElement, options?: FullscreenOptions): { supported: boolean };
export declare function fullscreenSupported(): boolean;
export declare function isFullscreen(): boolean;
export declare function toggleFullscreen(options?: FullscreenOptions): void;
export declare function icon(name: string): SVGElement | null;
/**
 * Pictures a package draws on its own buttons, added to the ones every page
 * here can ask for by name. `{name: "<svg contents>"}`, drawn in a 24 by 24
 * box and in `currentColor`.
 */
export declare function addIcons(pictures: Record<string, string>): void;
export declare function paintIcons(root: ParentNode): void;
export declare function autoHide(elements: Element | Element[], options?: AutoHideOptions): AutoHideHandle;
export interface LoadingHintOptions {
	/** What it says, `"Loading…"` otherwise; `null` leaves the text alone. */
	text?: string | null;
	/** How long to wait before giving up on it, 60000 ms otherwise. */
	until?: number;
	signal?: AbortSignal;
}

export declare function loadingHint(element: HTMLElement, isThere: () => boolean, options?: LoadingHintOptions): { stop(): void };
export declare function followSize(element: Element, controls: { setSize(width: number, height: number): void }, options?: { signal?: AbortSignal }): { stop(): void };
export declare function exportSettingsForm(container: HTMLElement, options?: ExportSettingsFormOptions): ExportSettingsForm;
export declare function videoCodecs(): Promise<VideoCodec[]>;
export declare function urlParameter(name: string): string | null;
export declare function setUrlParameters(values: Record<string, string | null>): void;
/**
 * How long a viewer element waits for a file before it gives up saying that it
 * is on its way, in milliseconds.
 */
export declare const waitForFile: number;

/**
 * What a viewer package builds its element on: a canvas in a shadow root, a
 * message over it, and a program started on it as soon as the element is in a
 * page. `@ddnet/demo-player` makes `<ddnet-demo>` out of this and
 * `@ddnet/map-viewer` makes `<ddnet-map>`.
 *
 * The picture lives in a shadow root, so nothing here shares a name with the
 * page around it. What may be styled from outside is named: `::part(picture)`
 * and `::part(message)`.
 */
export declare class ViewerElement extends HTMLElement {
	/** The running program, or what stopped it from starting. */
	readonly ready: Promise<Program> | null;
	/**
	 * The viewer's own controls, and `null` before it runs. What they are is
	 * the package's to say: `DemoControls` for `<ddnet-demo>`, `MapControls`
	 * for `<ddnet-map>`.
	 */
	readonly controls: unknown;
	/**
	 * What is drawn on. It lives in the shadow root, and styling it from
	 * outside is `::part(picture)`.
	 */
	readonly picture: HTMLCanvasElement;
	/**
	 * The bar of real buttons the package builds, where `controls="html"`
	 * asked for one, and `null` otherwise. Its parts carry `data-role`, so a
	 * page asks this element for one of them rather than the document - which
	 * is what lets a page hold more than one viewer.
	 */
	readonly bar: { readonly element: HTMLElement; part(role: string): HTMLElement | null; destroy(): void } | null;
	/** Shows a file rather than a name to fetch. */
	load(file: File): Promise<string>;
	/** Puts a line over the picture, or takes it away again with `""`. */
	say(message: string): void;
}

/**
 * The program's script as a module, which a plain `import` cannot do with it:
 * it is a classic script that names itself. Answers the factory that names it,
 * and remembers it, so asking twice for the same one costs nothing.
 */
export declare function importProgram(scriptUrl: string, moduleName: string): Promise<ProgramFactory>;

/**
 * What a package needs of the base beyond starting a program: where this
 * module is, because a worker sees no import map and has to be handed the
 * address; a foreign script as a blob of this page's own, which is the only
 * kind a cross-origin isolated page lets a worker import; the same complaint
 * about a misspelt option that every way in here makes; the error a signal
 * stops something with; and the sweep of the videos nobody took.
 */
export declare const moduleUrl: string;
export declare function fetchScript(url: string | URL): Promise<Blob>;
export declare function checkOptions(where: string, options: object, allowed: string[]): void;
export declare function abortError(signal: AbortSignal): unknown;
export declare function sweepVideoScratch(): Promise<void>;

/** All of it at once, for `import DDNetBase from "@ddnet/base"`. */
declare const DDNetBase: {
	version: typeof version;
	Error: typeof DDNetBaseError;
	Program: typeof Program;
	supportProblem: typeof supportProblem;
	supportError: typeof supportError;
	fullscreen: typeof fullscreen;
	fullscreenSupported: typeof fullscreenSupported;
	isFullscreen: typeof isFullscreen;
	toggleFullscreen: typeof toggleFullscreen;
	icon: typeof icon;
	addIcons: typeof addIcons;
	paintIcons: typeof paintIcons;
	autoHide: typeof autoHide;
	followSize: typeof followSize;
	loadingHint: typeof loadingHint;
	exportSettingsForm: typeof exportSettingsForm;
	videoCodecs: typeof videoCodecs;
	urlParameter: typeof urlParameter;
	setUrlParameters: typeof setUrlParameters;
	ViewerElement: typeof ViewerElement;
	waitForFile: typeof waitForFile;
	importProgram: typeof importProgram;
	moduleUrl: typeof moduleUrl;
	fetchScript: typeof fetchScript;
	checkOptions: typeof checkOptions;
	abortError: typeof abortError;
	sweepVideoScratch: typeof sweepVideoScratch;
};
export default DDNetBase;
