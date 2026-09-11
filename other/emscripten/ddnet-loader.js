// Starting one of the programs compiled for the browser, with the files it is
// given and, where it has something to show, on a canvas.
//
// Three ways in. `DDNetLoader.start` puts one instance on one canvas and hands
// back a handle to call into it - that is all an embedding page needs, and it
// claims no globals, so a page may have two of them or a program of its own
// beside them. `DDNetLoader.page` is that plus the furniture our own three
// pages share: a loading line, a console log, and the canvas filling the
// window. `DDNetLoader.render` is the same again with nothing on screen at all:
// a demo goes in, an MP4 comes out, and the page decides what to do with it.
// They differ in the program they start and in what they take, not in how any
// of it works.
//
// A plain script rather than a module, so that a page can use it with a plain
// `<script>` and so can we.

"use strict";

const DDNetLoader = (() => {
	const DEFAULT_HOME_PATH = "/home/web_user/.local/share/ddnet";
	// Where this script is, so that a worker can be given the same one. Read
	// while it is being run, which is the only time a script can say.
	const LOADER_URL = typeof document !== "undefined" && document.currentScript ? document.currentScript.src : null;
	// A file named in the URL is fetched into the same place a dropped file
	// goes. Anything larger than this is refused rather than filling the tab's
	// memory with whatever a link pointed at.
	const MAX_URL_FILE_BYTES = 256 * 1024 * 1024;

	function sanitizeFilename(filename) {
		return filename.replace(/[\/\\\?\|\*<>:"]/g, "_");
	}

	function parseAnsiColorRgb(line) {
		// Only handles RGB format because our logging system only uses that.
		const match = line.match(/^\x1b\[38;2;(\d+);(\d+);(\d+)m([\s\S]*?)\x1b\[0m$/);
		if (!match) {
			return { color: "black", message: line };
		}
		const [, r, g, b, message] = match;
		return { color: `rgb(${r}, ${g}, ${b})`, message: message };
	}

	// The value of a parameter, from the fragment first: a fragment never
	// reaches a server, so a link to somebody's demo stays between them and
	// their browser.
	function urlParameter(name) {
		const fragment = new URLSearchParams(location.hash.replace(/^#/, ""));
		if (fragment.has(name)) {
			return fragment.get(name);
		}
		const query = new URLSearchParams(location.search);
		return query.has(name) ? query.get(name) : null;
	}

	// The service worker keeps every file of the data directory it fetched,
	// which is safe because a URL there names the contents it carries. What it
	// must not keep is a file that the build no longer has, and the only thing
	// that knows which those are is the index - so once per load it is told to
	// go and compare. Once per page, however many instances are on it.
	var sweptDataCache = false;
	function sweepDataCache() {
		if (!sweptDataCache && navigator.serviceWorker && navigator.serviceWorker.controller) {
			sweptDataCache = true;
			navigator.serviceWorker.controller.postMessage({ type: "ddnet-data-sweep" });
		}
	}

	// A script from another origin, as a blob. Fetched rather than linked
	// because a blob belongs to this page: a worker may be made from it, and
	// `importScripts` takes it without asking the server for permission it
	// cannot give.
	async function fetchScript(url) {
		const response = await fetch(url.href, { mode: "cors" });
		if (!response.ok) {
			throw new Error(`${url.href} answered ${response.status} ${response.statusText}`);
		}
		return new Blob([await response.text()], { type: "text/javascript" });
	}

	// Where a video goes while it is being made. A fragmented MP4 is valid
	// after every fragment, so it is written to a file in the browser's own
	// private storage as it is encoded and only handed over when it is
	// finished: an export that takes minutes is then bounded by what the disk
	// holds rather than by what the tab can keep. Where there is no such
	// storage the export falls back to keeping the file in memory.
	const VIDEO_SCRATCH_DIRECTORY = "ddnet-video";
	async function videoScratchDirectory(create) {
		if (!navigator.storage || !navigator.storage.getDirectory) {
			return null;
		}
		const root = await navigator.storage.getDirectory();
		return await root.getDirectoryHandle(VIDEO_SCRATCH_DIRECTORY, { create: create });
	}

	async function videoScratchSink(info) {
		const directory = await videoScratchDirectory(true);
		if (directory == null) {
			return null;
		}
		const handle = await directory.getFileHandle(`${Date.now()}-${sanitizeFilename(info.fileName)}`, { create: true });
		return {
			stream: await handle.createWritable(),
			// The finished file, which is on disk rather than in memory: what
			// is handed over here is a handle to it, not its contents.
			done: async () => await handle.getFile(),
		};
	}

	// A video that was written but never taken is a video nobody wanted, so the
	// scratch files of earlier visits go at the start of this one. Once per
	// page, and before anything writes a new one.
	var sweptVideoScratch = false;
	async function sweepVideoScratch() {
		if (sweptVideoScratch) {
			return;
		}
		sweptVideoScratch = true;
		try {
			const directory = await videoScratchDirectory(false);
			if (directory == null) {
				return;
			}
			for await (const name of directory.keys()) {
				await directory.removeEntry(name).catch(() => {});
			}
		} catch (error) {
			// No directory yet, or no permission to have one: nothing to sweep.
		}
	}

	// The browser's own shortcuts stay the browser's, whatever the program
	// makes of the keyboard. Once per page as well.
	var installedKeyGuard = false;
	function installKeyGuard() {
		if (installedKeyGuard) {
			return;
		}
		installedKeyGuard = true;
		document.addEventListener('keydown', e => {
			// Always use default browser actions for Ctrl+F5 (refresh), F11 (fullscreen), F12 (developer console).
			if ((e.ctrlKey && e.key === 'F5') || e.key === 'F11' || e.key == 'F12') {
				e.stopPropagation();
			}
		}, true);
	}

	/**
	 * One running program. What a page gets back from `start`.
	 */
	class Instance {
		constructor(options) {
			this.options = options;
			// A program that draws into a video file rather than onto the page
			// has no canvas, and nothing here may assume one.
			this.canvas = options.canvas || null;
			this.homePath = options.homePath || DEFAULT_HOME_PATH;
			this.accept = options.accept || [];
			this.acceptLinks = options.acceptLinks === true;
			this.module = null;
			this.exited = false;
			this.video = null;
			this.pendingSink = null;
			this.finished = new Promise(resolve => {
				this.reportFinished = resolve;
			});
		}

		output(message, kind) {
			if (this.options.onOutput) {
				this.options.onOutput(message, kind || {});
			} else if (kind && kind.error) {
				console.error(message);
			} else {
				console.log(message);
			}
		}

		progress(text) {
			if (this.options.onProgress) {
				this.options.onProgress(text);
			}
		}

		/**
		 * Calls one of the program's exported functions, the same arguments as
		 * emscripten's `ccall`. Answers `null` rather than throwing where the
		 * program is not running, which is most of the time a page is open.
		 */
		call(name, returnType, argTypes, args) {
			if (this.module == null || this.exited) {
				return null;
			}
			try {
				return this.module.ccall(name, returnType || null, argTypes || [], args || []);
			} catch (error) {
				return null;
			}
		}

		/**
		 * Puts bytes where the program looks for what the user brought along and
		 * hands the path to it, the same way a dropped file arrives.
		 *
		 * @returns the path it was written to.
		 */
		async loadBytes(name, data) {
			const path = this.filePath({ name: name });
			if (path == null) {
				throw new Error(`${name} is not a kind of file this program takes`);
			}
			const filePath = await this.writeFile(path, name, data);
			this.call('EmscriptenCallbackDropFile', null, ['string'], [filePath]);
			return filePath;
		}

		async loadFile(file) {
			return await this.loadBytes(file.name, new Uint8Array(await file.arrayBuffer()));
		}

		/**
		 * Fetches a file and hands it over. Only http and https, and only from a
		 * server that allows it to be read from here, which is what a
		 * cross-origin request asks and answers.
		 */
		async loadUrl(url) {
			const filePath = await this.fetchUrlFile(url);
			this.call('EmscriptenCallbackDropFile', null, ['string'], [filePath]);
			return filePath;
		}

		/**
		 * Where the next video this program exports is written to, for a page
		 * that has somewhere better than the default: a `WritableStream`, or
		 * `{stream, done}` whose `done` answers with the finished file if there
		 * is still one to hand over.
		 *
		 * A page asking the user where to save has to ask while the click that
		 * started it is still the browser's idea of what the user is doing,
		 * which is why this is set before the export starts rather than
		 * answered when it does.
		 */
		setVideoSink(sink) {
			this.pendingSink = sink;
		}

		/** Asks the program to stop. `onExit` follows once it has. */
		quit() {
			this.call('EmscriptenCallbackQuit');
		}

		// Everything below is how the two above are done, and how an instance
		// comes up in the first place.

		async writeFile(path, name, data) {
			const FS = this.module.FS;
			FS.mkdirTree(`${path}/upload`);
			const filePath = `${path}/upload/${sanitizeFilename(name)}`;
			FS.writeFile(filePath, data);
			return filePath;
		}

		// Where a file of this kind belongs below the home directory, or `null`
		// for a file this program does not take.
		filePath(file) {
			for (const suffix of this.accept) {
				if (file.name.endsWith(suffix)) {
					return `${this.homePath}/${suffix === ".demo" ? "demos" : "maps"}`;
				}
			}
			return null;
		}

		async fetchUrlFile(url) {
			const response = await fetch(url, { mode: "cors" });
			if (!response.ok) {
				throw new Error(`The server answered ${response.status} ${response.statusText}`);
			}
			const buffer = await response.arrayBuffer();
			if (buffer.byteLength > MAX_URL_FILE_BYTES) {
				throw new Error(`The file is larger than ${MAX_URL_FILE_BYTES} bytes`);
			}
			const name = decodeURIComponent(new URL(url).pathname.split("/").pop() || "download");
			const path = this.filePath({ name: name }) || `${this.homePath}/${this.accept[0] === ".demo" ? "demos" : "maps"}`;
			return await this.writeFile(path, name, new Uint8Array(buffer));
		}

		droppedItemFromDataTransfer(dataTransfer) {
			var result = { file: null, path: null, link: null };
			if (!dataTransfer.items) {
				return result;
			}
			for (const item of dataTransfer.items) {
				if (item.kind == 'file') {
					const file = item.getAsFile();
					const path = this.filePath(file);
					if (path == null) {
						continue;
					}
					if (result.link != null) {
						alert("You cannot drop files and links at the same time.");
						break;
					}
					if (result.file != null) {
						alert("You cannot open multiple files at the same time. Only the first file will be opened.");
						break;
					}
					result.file = file;
					result.path = path;
				} else if (item.kind == 'string' && this.acceptLinks) {
					const string = dataTransfer.getData(item.type);
					if (string.startsWith('ddnet://')) {
						if (result.file != null) {
							alert("You cannot drop files and links at the same time.");
							break;
						}
						if (result.link != null && result.link != string) {
							alert("You cannot connect to multiple URLs at the same time. You will be connected to the first URL.");
							break;
						}
						result.link = string;
					}
				}
			}
			return result;
		}

		unsupportedDropMessage() {
			const kinds = this.accept.map(suffix => `${suffix} files`).join(" and ");
			return this.acceptLinks
				? `The items you dropped are not supported. You can drop ${kinds}, as well as ddnet:// links.`
				: `The items you dropped are not supported. You can drop ${kinds}.`;
		}

		installCanvasHandlers() {
			const instance = this;
			const canvas = this.canvas;
			if (canvas == null) {
				return;
			}
			installKeyGuard();
			canvas.addEventListener('contextmenu', e => e.preventDefault());
			canvas.addEventListener('webglcontextcreationerror', e => {
				instance.output(`Failed to create WebGL context: ${e.statusMessage || "Unknown error"}`, { error: true, bold: true });
			});
			canvas.addEventListener('webglcontextlost', e => {
				// The program cannot currently recover from GL context loss, because it
				// would require reloading all textures, framebuffers etc.
				instance.output(`The WebGL context was lost: ${e.statusMessage || "Unknown error"}`, { error: true, bold: true });
				instance.quit();
			});
			canvas.addEventListener('dragover', e => {
				e.preventDefault();
				e.dataTransfer.dropEffect = "none";
				if (!e.dataTransfer.items) {
					return;
				}
				for (const item of e.dataTransfer.items) {
					if (item.kind == 'file' || item.kind == 'string') {
						e.dataTransfer.dropEffect = "copy";
						return;
					}
				}
			});
			canvas.addEventListener('drop', async e => {
				e.preventDefault();
				const droppedItem = instance.droppedItemFromDataTransfer(e.dataTransfer);
				if (droppedItem.file != null) {
					await instance.loadFile(droppedItem.file);
				} else if (droppedItem.link != null) {
					instance.call('EmscriptenCallbackDropFile', null, ['string'], [droppedItem.link]);
				} else {
					alert(instance.unsupportedDropMessage());
				}
			});
		}

		// What the export asks when it starts, in order: what the page put
		// there for this one export, what it gave once for all of them, and the
		// scratch file otherwise.
		async videoSink(info) {
			if (this.pendingSink != null) {
				const sink = this.pendingSink;
				this.pendingSink = null;
				return sink;
			}
			if (this.options.videoSink) {
				return typeof this.options.videoSink === "function" ? await this.options.videoSink(info) : this.options.videoSink;
			}
			return await videoScratchSink(info);
		}

		// The audio keeps running after the runtime has stopped, and says so
		// once per buffer. Only this instance's, and only its own doing.
		stopAudio() {
			const SDL2 = this.module == null ? undefined : this.module['SDL2'];
			if (SDL2 === undefined) {
				return;
			}
			if (SDL2.audio !== undefined) {
				if (SDL2.audio.scriptProcessorNode !== undefined) {
					SDL2.audio.scriptProcessorNode.disconnect();
				}
				if (SDL2.audio.silenceTimer !== undefined) {
					clearInterval(SDL2.audio.silenceTimer);
				}
				SDL2.audio = undefined;
			}
			if (SDL2.audioContext !== undefined) {
				SDL2.audioContext.close();
				SDL2.audioContext = undefined;
			}
		}

		// An error out of the wasm leaves the runtime standing there rather than
		// exiting, which hangs a test run and skips `onExit`. Chained onto
		// whatever the page already had, since this is the page's own handler.
		installErrorHandler() {
			const instance = this;
			// `self`, not `window`: a program rendering in a worker has no
			// window, and this is the one thing here that would miss it.
			const previous = self.onerror;
			self.onerror = function(message, url, line, column, error) {
				instance.output(message, { error: true, bold: true });
				if (error && error.stack) {
					for (const line of error.stack.split("\n")) {
						if (line.length > 0) {
							instance.output(line, { error: true });
						}
					}
				}
				instance.stopAudio();
				// A runtime that has already aborted answers every call with an
				// exception, which `call` swallows, so there is nothing to ask
				// first. One that is past answering never reaches `onExit`
				// either, so whoever waits for the program is let go here.
				instance.call('EmscriptenCallbackQuitForce');
				instance.reportFinished();
				if (previous) {
					return previous.apply(this, arguments);
				}
			};
		}

		async run() {
			const instance = this;
			const options = this.options;
			if (typeof options.module !== "function") {
				throw new Error("DDNetLoader needs the program's factory, for example `module: DDNetDemoViewer`");
			}
			sweepDataCache();
			sweepVideoScratch();
			this.installErrorHandler();

			const program = await this.program();
			var totalDependencies = 0;
			this.module = await options.module({
				websocket: {
					url: location.protocol === "https:" ? "wss://" : "ws://",
				},
				noInitialRun: true,
				canvas: this.canvas === null ? undefined : this.canvas,
				// A program from another origin is loaded from a blob, see
				// `foreignProgram`, and then only this says where its own files
				// are - the blob has no directory to look next to.
				mainScriptUrlOrBlob: program === null ? undefined : program.script,
				locateFile: program === null ? undefined : path => new URL(path, program.base).href,
				// Where a finished video goes. Without it the export offers the
				// file as a download, which is what somebody watching wants and
				// a page rendering by itself does not. Read by the WebCodecs
				// export, see `src/engine/client/video_webcodecs.cpp`.
				ddnetVideoOutput: options.onVideo,
				// Where a video is written while it is made, see `videoSink`.
				ddnetVideoSink: info => instance.videoSink(info),
				// How far a render has got, once a second while it runs.
				ddnetRenderProgress: options.onRenderProgress,
				// Where `data` is, for a page that keeps it somewhere other than
				// next to itself. A program from another origin brings its own,
				// so that is where to look unless the page says otherwise. Read
				// by `webfs`, see `src/base/webfs.h`.
				ddnetDataBase: options.dataBase || (program === null ? undefined : new URL(".", program.base).href),
				arguments: (options.arguments || []).slice(),
				print: text => {
					const parsedLine = parseAnsiColorRgb(text);
					console.log(parsedLine.message);
					instance.output(parsedLine.message, { color: parsedLine.color });
				},
				printErr: text => {
					console.error(text);
					instance.output(text, { error: true });
				},
				onExit: () => instance.onExit(),
				monitorRunDependencies: left => {
					totalDependencies = Math.max(totalDependencies, left);
					const value = totalDependencies - left;
					instance.progress(totalDependencies == 1 ? "Loading…" : `Loading… (${value}/${totalDependencies})`);
				},
			});

			this.installCanvasHandlers();
			await this.mountPersistentStorage();

			const args = this.module.arguments;
			const path = await this.initialFile();
			if (path != null) {
				if (options.fileArgument) {
					args.push(options.fileArgument);
				}
				args.push(path);
			}
			if (this.canvas != null) {
				this.canvas.style.display = "block";
			}
			this.module.callMain(args);
			return this;
		}

		// Where the program's own script is, when the page said. Emscripten
		// otherwise works it out from whatever script is running, which is right
		// on a page that loaded it and wrong everywhere else - in a worker, or
		// where the program comes from another origin.
		//
		// From another origin it is not enough to name it: the program brings
		// threads, and a thread's script has to come from the page's own origin,
		// so a worker made from a foreign URL is refused outright. The program
		// is fetched instead - which a cross-origin request may do where the
		// server permits it - and handed on as a blob, which belongs to whoever
		// made it.
		//
		// The rest of what this takes is the page's own doing and cannot be done
		// from here: a page that runs this has to be cross-origin isolated, so
		// `Cross-Origin-Opener-Policy: same-origin` and
		// `Cross-Origin-Embedder-Policy: require-corp` on the page itself, or
		// `coi-serviceworker.js` next to it; and under `require-corp` the
		// `<script>` that fetches the program needs `crossorigin`, because
		// without it the browser asks for it without CORS and refuses what comes
		// back.
		async program() {
			if (!this.options.scriptUrl) {
				return null;
			}
			const base = new URL(this.options.scriptUrl, location.href);
			if (base.origin === location.origin) {
				return { script: base.href, base: base };
			}
			return { script: await fetchScript(base), base: base };
		}

		// The file the program starts on, if it was given one: bytes the page
		// handed over, or a URL - the page's own or one of its parameters.
		async initialFile() {
			const file = this.options.file;
			if (file != null && typeof file !== "string") {
				const name = this.options.fileName || file.name || "upload";
				const bytes = file instanceof Uint8Array ? file : new Uint8Array(file instanceof ArrayBuffer ? file : await file.arrayBuffer());
				return await this.writeFile(this.filePath({ name: name }) || this.homePath, name, bytes);
			}
			const url = typeof file === "string" ? new URL(file, location.href).href : this.urlArgument();
			if (url == null) {
				return null;
			}
			this.output(`Downloading ${url}…`);
			try {
				return await this.fetchUrlFile(url);
			} catch (downloadError) {
				this.output(`Failed to download ${url}: ${downloadError.message}`, { error: true, bold: true });
				this.output("A server has to allow this page to read its files. You can drop the file into this page instead.");
				return null;
			}
		}

		urlArgument() {
			for (const name of this.options.urlParams || []) {
				const value = urlParameter(name);
				if (value == null || value === "") {
					continue;
				}
				const url = new URL(value, location.href);
				if (url.protocol !== "http:" && url.protocol !== "https:") {
					this.output(`Refused to load ${url.protocol} URL`, { error: true });
					continue;
				}
				return url.href;
			}
			return null;
		}

		// Everything the user writes lives here and outlives the tab, which is
		// what IndexedDB is for. It has to be there before the program starts.
		async mountPersistentStorage() {
			const instance = this;
			const FS = this.module.FS;
			FS.mkdirTree(this.homePath);
			// A program that is only passing through leaves nothing behind: the
			// home directory is there for it to write into, and it goes with the
			// tab.
			if (this.options.persist === false) {
				return;
			}
			this.output("Synchronizing filesystem with IndexedDB…");
			FS.mount(this.module.IDBFS, {}, this.homePath);
			await new Promise(resolve => FS.syncfs(true, error => {
				if (error) {
					instance.output(`Failed to synchronize filesystem with IndexedDB: ${error}`, { error: true, bold: true });
				}
				resolve();
			}));
		}

		onExit() {
			this.exited = true;
			this.reportFinished();
			if (this.options.persist !== false) {
				this.syncPersistentStorage();
			}
			if (this.canvas != null) {
				// After the program quits, hide the canvas and reset the cursor, as
				// the canvas will be entirely black, also blocking the view of
				// whatever is behind it.
				this.canvas.style.display = "none";
				// Make sure to reset cursor because it sometimes does not become
				// visible.
				this.canvas.style.cursor = "default";
				// Also reset cursor of body because the cursor sometimes does not
				// become visible until being moved.
				document.body.style.cursor = "default";
			}
			this.output(`${this.options.programName || "The program"} closed.`, { bold: true });
			if (this.options.onExit) {
				this.options.onExit();
			}
		}

		syncPersistentStorage() {
			if (this.module['ddnetSyncPersistentStorage'] !== undefined) {
				this.module['ddnetSyncPersistentStorage'](true);
			} else {
				this.module.FS.syncfs(error => {
					if (error) {
						this.output(`Failed to synchronize filesystem with IndexedDB: ${error}`, { error: true, bold: true });
					}
				});
			}
		}
	}

	// The furniture our own pages share, on top of `start`: the line that says
	// what is happening until the first frame, and the console log that is
	// there when something goes wrong. Takes the same options plus `elements`.
	function pageOptions(options) {
		const elements = options.elements;
		return Object.assign({}, options, {
			canvas: elements.canvas,
			onProgress: text => {
				elements.loading.textContent = text;
			},
			onOutput: (message, kind) => {
				const span = document.createElement("span");
				span.textContent = message;
				span.className = "line";
				if (kind.error) {
					span.style.color = "red";
				} else if (kind.color) {
					span.style.borderColor = kind.color;
				}
				if (kind.bold) {
					span.style.fontWeight = "bold";
				}
				elements.loading.style.display = "none";
				elements.output.style.display = "flex";
				elements.outputContent.appendChild(span);
				elements.outputContent.appendChild(document.createElement("br"));
				elements.outputContent.scrollTop = elements.outputContent.scrollHeight;
			},
			onExit: () => {
				const restartButton = document.createElement("button");
				restartButton.textContent = "Reload page";
				restartButton.style.marginTop = "10px";
				restartButton.addEventListener('click', e => location.reload());
				elements.outputContent.appendChild(restartButton);
				elements.outputContent.scrollTop = elements.outputContent.scrollHeight;
				if (options.onExit) {
					options.onExit();
				}
			},
		});
	}

	// A render is a worker's worth of work - every frame drawn, read back and
	// encoded - and none of it needs the page. Done in a worker, the page stays
	// answerable while it happens, which is the whole point of a render nobody
	// is watching. This is what that worker runs: it loads this script and the
	// program again, and renders with the same call the page would have made.
	const WORKER_BOOTSTRAP = `
self.onmessage = async event => {
	const request = event.data;
	// The program says its name to whoever is asking, and a module loader is
	// asking, so there is no need to know the name here.
	let factory = null;
	self.define = (dependencies, provide) => { factory = provide(); };
	self.define.amd = true;
	try {
		importScripts(request.loaderUrl, request.scriptUrl);
		if (factory === null) {
			factory = self[request.moduleName];
		}
		const video = await DDNetLoader.render(Object.assign({}, request.options, {
			module: factory,
			worker: false,
			videoSink: request.sink,
			onOutput: (message, kind) => self.postMessage({type: "output", message: message, kind: kind}),
			onRenderProgress: status => self.postMessage({type: "progress", status: status}),
		}));
		self.postMessage({type: "done", video: video});
	} catch (error) {
		self.postMessage({type: "failed", message: String((error && error.message) || error)});
	}
};
`;

	// Everything about a render that is data rather than a promise to call
	// back: what survives being sent to a worker.
	const WORKER_OPTIONS = [
		// tidy-alphabetical-start
		"arguments", "audio", "chat", "codec", "crf", "dataBase", "demo", "fps",
		"height", "homePath", "hud", "moduleName", "name", "output", "preset",
		"programName", "scriptUrl", "settings", "width",
		// tidy-alphabetical-end
	];

	async function renderInWorker(options, loaderUrl) {
		const program = new URL(options.scriptUrl, location.href);
		// Both scripts go in as blobs where they are not this page's own:
		// `importScripts` asks for a foreign script without CORS, which a page
		// that is cross-origin isolated then refuses.
		const [loaderScript, programScript] = await Promise.all([loaderUrl, program].map(async url =>
			url.origin === location.origin ? url.href : URL.createObjectURL(await fetchScript(url))));
		const request = { loaderUrl: loaderScript, scriptUrl: programScript, moduleName: options.moduleName || null, options: {}, sink: undefined };
		for (const key of WORKER_OPTIONS) {
			if (options[key] !== undefined) {
				request.options[key] = options[key];
			}
		}
		request.options.scriptUrl = program.href;
		// A destination the page picked can be handed over, if it is the kind
		// of stream that can be. Where it is not, the render stays here rather
		// than quietly writing somewhere else.
		const transfer = [];
		if (options.videoSink && typeof options.videoSink !== "function") {
			request.sink = options.videoSink;
			transfer.push(options.videoSink);
		}
		const bootstrap = URL.createObjectURL(new Blob([WORKER_BOOTSTRAP], { type: "text/javascript" }));
		const worker = new Worker(bootstrap);
		URL.revokeObjectURL(bootstrap);
		if (options.onStart) {
			options.onStart({ quit: () => worker.terminate() });
		}
		return await new Promise((resolve, reject) => {
			worker.onmessage = event => {
				const message = event.data;
				if (message.type === "output") {
					if (options.onOutput) {
						options.onOutput(message.message, message.kind || {});
					}
					return;
				}
				if (message.type === "progress") {
					if (options.onRenderProgress) {
						options.onRenderProgress(message.status);
					}
					return;
				}
				worker.terminate();
				if (message.type === "done") {
					resolve(message.video);
				} else {
					reject(new Error(message.message));
				}
			};
			worker.onerror = event => {
				worker.terminate();
				reject(new Error(event.message || "the render worker stopped"));
			};
			worker.postMessage(request, transfer);
		});
	}

	// The viewers' own controls, as calls rather than as names to spell out:
	// a page steering one should not have to know that `ccall` exists, nor
	// which of the arguments are numbers. Every call answers `null` where the
	// program is not running, the same as `call` does.
	//
	// The other end of these is the `DemoViewer*` block in
	// `src/engine/client/demo_viewer_client.cpp` and the `MapViewer*` block in
	// `src/game/map/standalone/map_viewer_main.cpp`.
	function demoControls(instance) {
		const number = (name, argument) => argument === undefined
			? instance.call(name, "number")
			: instance.call(name, null, ["number"], [argument]);
		return {
			/** Whether a demo is loaded and how long it is, in seconds. */
			length: () => number("DemoViewerLength"),
			/** How far it has played, between 0 and 1. */
			progress: () => number("DemoViewerProgress"),
			paused: () => number("DemoViewerPaused") === 1,
			pause: () => number("DemoViewerSetPaused", 1),
			play: () => number("DemoViewerSetPaused", 0),
			/** Jumps to a part of the demo, between 0 and 1. */
			seek: Fraction => number("DemoViewerSeekPercent", Fraction),
			/** Jumps to a time in the demo, in seconds. */
			seekTime: Seconds => number("DemoViewerSeekTime", Seconds),
			restart: () => instance.call("DemoViewerSeekStart"),
			/** The playback speed, or sets it: 1 is as it was played. */
			speed: Value => Value === undefined ? number("DemoViewerSpeed") : number("DemoViewerSetSpeed", Value),
			exporting: () => number("DemoViewerExporting") === 1,
			/** Starts a video export, and says whether it started. */
			startExport: (Width, Height, Fps, Audio) => instance.call("DemoViewerStartExport", "number",
				["number", "number", "number", "number"], [Width, Height, Fps, Audio ? 1 : 0]) === 1,
		};
	}

	function mapControls(instance) {
		return {
			loaded: () => instance.call("MapViewerMapLoaded", "number") === 1,
			/** Fits the whole map on screen. */
			fit: () => instance.call("MapViewerFit"),
			/** Writes what is on screen, or the whole map, to a picture. */
			exportView: () => instance.call("MapViewerExportView"),
			exportFullMap: () => instance.call("MapViewerExportFullMap"),
			/** 0 while nothing is being written, 1 while it is, 2 when it failed. */
			exportState: () => instance.call("MapViewerExportState", "number"),
		};
	}

	// What the render tool is asked on a command line, from what the page
	// asked for here. It is the same program with the same arguments as the one
	// a terminal starts, so `ddnet-demo-render --help` documents these too.
	function renderOptions(options) {
		const args = ["--output", options.output || "video.mp4"];
		const flag = (name, argument) => {
			if (options[name] !== undefined && options[name] !== null) {
				args.push(argument, String(options[name]));
			}
		};
		flag("width", "--width");
		flag("height", "--height");
		flag("fps", "--fps");
		flag("codec", "--codec");
		flag("crf", "--crf");
		flag("preset", "--preset");
		if (options.audio === false) {
			args.push("--no-audio");
		}
		if (options.hud === true) {
			args.push("--hud");
		}
		if (options.chat === false) {
			args.push("--no-chat");
		}
		// Everything the client takes on its command line it takes here as
		// well, one console command per entry, so `cl_showfps 1` works.
		for (const setting of options.settings || []) {
			args.push(setting);
		}
		return Object.assign({}, options, {
			canvas: null,
			persist: false,
			accept: [".demo"],
			file: options.demo,
			fileName: options.name || "render.demo",
			fileArgument: "--render-demo",
			programName: options.programName || "The demo renderer",
			arguments: args.concat(options.arguments || []),
		});
	}

	return {
		/**
		 * Starts a program on a canvas.
		 *
		 * @param options.module The factory the program's script defines, so
		 * `DDNetClient`, `DDNetDemoViewer` or `DDNetMapViewer`.
		 * @param options.canvas The canvas to draw on.
		 * @param options.dataBase Where the `data` directory is, if it is not
		 * next to the page.
		 * @param options.scriptUrl Where the program's script was loaded from,
		 * when that is another origin than this page.
		 * @param options.videoSink Where an exported video is written, see
		 * `setVideoSink`. Without it the video goes to a scratch file and is
		 * handed over when it is done.
		 * @param options.accept The file suffixes this program takes.
		 * @param options.file A file to start on: bytes, a `File`, or the URL of
		 * one.
		 * @param options.urlParams Parameters of the page's URL that may name a
		 * file to open.
		 * @param options.onOutput Called for every line the program writes.
		 * @param options.onProgress Called while the program is being fetched.
		 * @param options.onExit Called once the program has stopped.
		 *
		 * @returns a promise for the running instance.
		 */
		start(options) {
			return new Instance(options).run();
		},

		/**
		 * What a demo viewer can be asked to do, bound to one of them. The page
		 * that hosts it needs nothing else to steer it - and neither does a URL
		 * that says where to start, which is `urlParameter` below.
		 */
		demoControls(instance) {
			return demoControls(instance);
		},

		/** The same for a map viewer. */
		mapControls(instance) {
			return mapControls(instance);
		},

		/**
		 * What the page's URL says about a parameter, from the fragment first:
		 * a fragment never reaches a server, so a link to somebody's demo stays
		 * between them and their browser.
		 */
		urlParameter(name) {
			return urlParameter(name);
		},

		/** `start` with the furniture our own pages share around it. */
		page(options) {
			return new Instance(pageOptions(options)).run();
		},

		/**
		 * Renders a demo into a video file, with nothing on screen.
		 *
		 * The render tool draws into render targets, so it needs no canvas and
		 * takes no part of the page: it reads a demo, encodes every frame of it
		 * with the browser's own video encoder, and the file it would otherwise
		 * offer as a download is handed back here instead.
		 *
		 * @param options.module The factory `ddnet-demo-render.js` defines, so
		 * `DDNetDemoRenderer`.
		 * @param options.demo The demo to render: bytes, a `File`, or the URL of
		 * one.
		 * @param options.name The demo's file name, when it comes as bytes.
		 * @param options.output The name the video carries, `video.mp4`
		 * otherwise.
		 * @param options.width Video width in pixels, even, `cl_video_width`
		 * otherwise; `height`, `fps`, `codec`, `crf` and `preset` the same way.
		 * @param options.audio `false` renders without a sound track.
		 * @param options.hud `true` shows the ingame interface, `options.chat`
		 * `false` hides the chat.
		 * @param options.settings Console commands, one per entry.
		 * @param options.dataBase Where the `data` directory is, if it is not
		 * next to the page.
		 * @param options.onOutput Called for every line the render writes.
		 * @param options.onRenderProgress Called once a second while the render
		 * runs, with `{progress, encodedFrames, submittedFrames,
		 * framesPerSecond}` - `progress` is the part of the demo that is done,
		 * between 0 and 1.
		 * @param options.onStart Called with the running instance, whose `quit`
		 * ends a render that is taking too long.
		 * @param options.scriptUrl Where `ddnet-demo-render.js` was loaded from.
		 * With it the render happens in a worker and leaves the page free;
		 * `worker: false` keeps it here.
		 * @param options.videoSink Where the video is written while it is made.
		 * A `WritableStream` can go to the worker with it; a function cannot,
		 * and is only asked here.
		 *
		 * @returns a promise for the finished MP4 as a `Blob`.
		 */
		async render(options) {
			// A worker needs to load the program itself, so it needs to be told
			// where it is; without that this is the only thread there is.
			if (options.worker !== false && typeof Worker === "function" && options.scriptUrl && LOADER_URL) {
				return await renderInWorker(options, new URL(LOADER_URL, location.href));
			}
			const instance = new Instance(Object.assign(renderOptions(options), {
				onVideo: file => {
					instance.video = file;
				},
			}));
			await instance.run();
			if (options.onStart) {
				options.onStart(instance);
			}
			await instance.finished;
			if (instance.video == null) {
				throw new Error("The demo was not rendered into a video, see the output for what went wrong");
			}
			return instance.video;
		},
	};
})();
