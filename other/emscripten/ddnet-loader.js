// Everything three pages have in common when they start a program compiled for
// the browser: the module, the console, the persistent storage, the launch
// button, and the ways a file gets in - dropped, picked or named in the URL.
//
// A plain script rather than a module, because the loader emscripten emits is
// `<script async>` and has to find `Module` on the global object.

"use strict";

const DDNetLoader = (() => {
	const DEFAULT_HOME_PATH = "/home/web_user/.local/share/ddnet";
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

	class Loader {
		constructor(options) {
			this.options = options;
			this.homePath = options.homePath || DEFAULT_HOME_PATH;
			this.accept = options.accept || [];
			this.acceptLinks = options.acceptLinks === true;
			this.urlParams = options.urlParams || [];
			this.elements = options.elements;
			this.loadOnLaunch = { file: null, path: null, link: null, url: null };
			this.totalDependencies = 0;
			this.createModule();
			this.installErrorHandler();
			this.installCanvasHandlers();
			this.installDropHandlers();
			this.installLaunchButton();
			this.readUrl();
			this.sweepDataCache();
		}

		// The service worker keeps every file of the data directory it fetched,
		// which is safe because a URL there names the contents it carries. What
		// it must not keep is a file that this build no longer has, and the only
		// thing that knows which those are is the index - so once per load it is
		// told to go and compare.
		sweepDataCache() {
			if (navigator.serviceWorker && navigator.serviceWorker.controller) {
				navigator.serviceWorker.controller.postMessage({ type: "ddnet-data-sweep" });
			}
		}

		appendOutput(message, bold, textColor, borderColor) {
			const outputContent = this.elements.outputContent;
			const span = document.createElement("span");
			span.textContent = message;
			span.className = "line";
			if (textColor) {
				span.style.color = textColor;
			}
			if (borderColor) {
				span.style.borderColor = borderColor;
			}
			if (bold) {
				span.style.fontWeight = "bold";
			}
			outputContent.appendChild(span);
			outputContent.appendChild(document.createElement("br"));
			outputContent.scrollTop = outputContent.scrollHeight;
		}

		setStatus(text) {
			if (this.elements.dropTargetStatus) {
				this.elements.dropTargetStatus.textContent = text;
			}
		}

		createModule() {
			const loader = this;
			const elements = this.elements;
			window.Module = {
				websocket: {
					url: location.protocol === "https:" ? "wss://" : "ws://",
				},
				noInitialRun: true,
				arguments: (this.options.arguments || []).slice(),
				print: function(text) {
					const parsedLine = parseAnsiColorRgb(text);
					console.log(parsedLine.message);
					loader.appendOutput(parsedLine.message, false, undefined, parsedLine.color);
				},
				printErr: function(text) {
					console.error(text);
					loader.appendOutput(text, false, "red");
				},
				onRuntimeInitialized: function() {
					const launchButton = elements.launchButton;
					const oldTransition = launchButton.style.transition;
					launchButton.style.transition = "none";
					launchButton.disabled = false;
					elements.launchButtonLabel.textContent = loader.options.launchLabel || "Start";
					elements.launchButtonProgress.style.display = "none";
					launchButton.offsetHeight; // Force reflow to avoid transition
					launchButton.style.transition = oldTransition;
					// For testing when compiled with emrun support, allow launching automatically by specifying URL parameter.
					if (typeof emrun_register_handlers === "function" && document.location.hash === "#__emrun_autostart__") {
						launchButton.click();
					}
					if (loader.options.autoLaunch && loader.loadOnLaunch.url != null) {
						launchButton.click();
					}
				},
				onExit: function() {
					if (Module['ddnetSyncPersistentStorage'] !== undefined) {
						Module['ddnetSyncPersistentStorage'](true);
					} else {
						FS.syncfs(error => {
							if (error) {
								loader.appendOutput(`Failed to synchronize filesystem with IndexedDB: ${error}`, true, "red");
							}
						});
					}
					// After the program quits, hide the canvas and reset the cursor, as the canvas
					// will be entirely black, also blocking the view of the console output.
					Module['canvas'].style.display = "none";
					// Make sure to reset cursor because it sometimes does not become visible.
					Module['canvas'].style.cursor = "default";
					// Also reset cursor of body because the cursor sometimes does not become
					// visible until being moved.
					document.body.style.cursor = "default";
					loader.appendOutput(`${loader.options.programName || "The program"} closed. Reload the page to restart.`, true);
					const restartButton = document.createElement("button");
					restartButton.textContent = "Reload page";
					restartButton.style.marginTop = "10px";
					restartButton.addEventListener('click', e => location.reload());
					elements.outputContent.appendChild(restartButton);
					elements.outputContent.scrollTop = elements.outputContent.scrollHeight;
				},
				totalDependencies: 0,
				monitorRunDependencies(left) {
					this.totalDependencies = Math.max(this.totalDependencies, left);
					const value = this.totalDependencies - left;
					elements.launchButtonLabel.textContent = this.totalDependencies == 1 ? "Loading…" : `Loading… (${value}/${this.totalDependencies})`;
					elements.launchButtonProgress.style.width = `${value * 100.0 / this.totalDependencies}%`;
				},
				canvas: elements.canvas
			};
		}

		installErrorHandler() {
			const loader = this;
			window.onerror = function(message, url, line, column, error) {
				loader.appendOutput(message, true, "red");
				if (error && error.stack) {
					for (const line of error.stack.split("\n")) {
						if (line.length > 0) {
							loader.appendOutput(line, false, "red");
						}
					}
				}
				// Force stop audio processing because the program does not do so when force quit on
				// assertion errors and segmentation faults. This would otherwise cause many error
				// messages to be emitted when audio processing continues after the runtime exited.
				var SDL2 = Module['SDL2'];
				if (SDL2 !== undefined) {
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
				// Force quit on errors, because the runtime does not exit otherwise, which
				// would cause tests using `emrun` to time out and prevent `onExit` from being invoked.
				if (!ABORT) {
					Module.ccall('EmscriptenCallbackQuitForce', null, [], []);
				}
			};
		}

		installCanvasHandlers() {
			const loader = this;
			document.addEventListener('keydown', e => {
				// Always use default browser actions for Ctrl+F5 (refresh), F11 (fullscreen), F12 (developer console).
				if ((e.ctrlKey && e.key === 'F5') || e.key === 'F11' || e.key == 'F12') {
					e.stopPropagation();
				}
			}, true);
			const canvas = this.elements.canvas;
			canvas.addEventListener('contextmenu', e => e.preventDefault());
			canvas.addEventListener('webglcontextcreationerror', e => {
				loader.appendOutput(`Failed to create WebGL context: ${e.statusMessage || "Unknown error"}`, true, "red");
			});
			canvas.addEventListener('webglcontextlost', e => {
				// The program cannot currently recover from GL context loss, because it
				// would require reloading all textures, framebuffers etc.
				loader.appendOutput(`The WebGL context was lost: ${e.statusMessage || "Unknown error"}`, true, "red");
				Module.ccall('EmscriptenCallbackQuit', null, [], []);
			});
		}

		// Where a file of this kind belongs below the home directory, or `null`
		// for a file this page does not take.
		dropFilePath(file) {
			for (const suffix of this.accept) {
				if (file.name.endsWith(suffix)) {
					return `${this.homePath}/${suffix === ".demo" ? "demos" : "maps"}`;
				}
			}
			return null;
		}

		droppedItemFromDataTransfer(dataTransfer) {
			var result = { file: null, path: null, link: null };
			if (!dataTransfer.items) {
				return result;
			}
			for (const item of dataTransfer.items) {
				if (item.kind == 'file') {
					const file = item.getAsFile();
					const path = this.dropFilePath(file);
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

		installDropHandlers() {
			const loader = this;
			const canvas = this.elements.canvas;
			const dropTarget = this.elements.dropTarget;
			if (dropTarget) {
				document.addEventListener('dragover', e => {
					if (e.target != dropTarget && e.target != canvas) {
						// Prevent items from being dropped outside of the designated drop target and canvas.
						e.preventDefault();
						e.dataTransfer.dropEffect = "none";
					}
				}, true);
				document.addEventListener('drop', e => {
					if (e.target != dropTarget && e.target != canvas) {
						e.preventDefault();
					}
				}, true);
				dropTarget.addEventListener('dragover', e => {
					e.preventDefault();
					e.dataTransfer.dropEffect = "none";
					if (!e.dataTransfer.items) {
						return;
					}
					for (const item of e.dataTransfer.items) {
						if (item.kind == 'file' || item.kind == 'string') {
							e.dataTransfer.dropEffect = "copy";
							dropTarget.setAttribute("drop-active", true);
							return;
						}
					}
				});
				dropTarget.addEventListener('dragleave', e => {
					dropTarget.removeAttribute("drop-active");
				});
				dropTarget.addEventListener('drop', async e => {
					e.preventDefault();
					const droppedItem = loader.droppedItemFromDataTransfer(e.dataTransfer);
					if (droppedItem.file != null) {
						loader.loadOnLaunch = { file: droppedItem.file, path: droppedItem.path, link: null, url: null };
						loader.setStatus(`Selected file: ${droppedItem.file.name}`);
					} else if (droppedItem.link != null) {
						loader.loadOnLaunch = { file: null, path: null, link: droppedItem.link, url: null };
						loader.setStatus(`Selected link: ${droppedItem.link}`);
					} else {
						alert(loader.unsupportedDropMessage());
					}
					dropTarget.removeAttribute("drop-active");
				});

				const fileInput = this.elements.dropTargetFileInput;
				if (fileInput) {
					dropTarget.addEventListener('click', e => fileInput.click());
					fileInput.addEventListener('change', e => {
						if (e.target.files.length == 0) {
							return;
						}
						const path = loader.dropFilePath(e.target.files[0]);
						if (path == null) {
							alert(`The file you selected is not supported. You can select ${loader.accept.join(" and ")} files.`);
							return;
						}
						loader.loadOnLaunch = { file: e.target.files[0], path: path, link: null, url: null };
						loader.setStatus(`Selected file: ${loader.loadOnLaunch.file.name}`);
					});
				}
			}

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
				const droppedItem = loader.droppedItemFromDataTransfer(e.dataTransfer);
				if (droppedItem.file != null) {
					const filePath = await loader.writeFile(droppedItem.path, droppedItem.file.name, new Uint8Array(await droppedItem.file.arrayBuffer()));
					Module.ccall('EmscriptenCallbackDropFile', null, ['string'], [filePath]);
				} else if (droppedItem.link != null) {
					Module.ccall('EmscriptenCallbackDropFile', null, ['string'], [droppedItem.link]);
				} else {
					alert(loader.unsupportedDropMessage());
				}
			});
		}

		// Puts bytes where the program looks for what the user brought along.
		async writeFile(path, name, data) {
			FS.mkdirTree(`${path}/upload`);
			const filePath = `${path}/upload/${sanitizeFilename(name)}`;
			FS.writeFile(filePath, data);
			return filePath;
		}

		// A file named in the URL. Only http and https, and only from a server
		// that allows it to be read from here, which is what a cross-origin
		// request asks and answers.
		readUrl() {
			for (const name of this.urlParams) {
				const value = urlParameter(name);
				if (value == null || value === "") {
					continue;
				}
				const url = new URL(value, location.href);
				if (url.protocol !== "http:" && url.protocol !== "https:") {
					this.setStatus(`Refused to load ${url.protocol} URL`);
					continue;
				}
				this.loadOnLaunch = { file: null, path: null, link: null, url: url.href };
				this.setStatus(`Selected link: ${url.href}`);
				return;
			}
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
			const path = this.dropFilePath({ name: name }) || `${this.homePath}/${this.accept[0] === ".demo" ? "demos" : "maps"}`;
			return await this.writeFile(path, name, new Uint8Array(buffer));
		}

		installLaunchButton() {
			const loader = this;
			this.elements.launchButton.addEventListener('click', async e => {
				if (loader.elements.controls) {
					loader.elements.controls.style.display = "none";
				}
				loader.elements.output.style.display = "flex";
				loader.appendOutput("Synchronizing filesystem with IndexedDB…");
				FS.mkdirTree(loader.homePath);
				FS.mount(IDBFS, {}, loader.homePath);
				FS.syncfs(true, async error => {
					if (error) {
						loader.appendOutput(`Failed to synchronize filesystem with IndexedDB: ${error}`, true, "red");
						return;
					}
					Module['canvas'].style.display = "block";
					var args = Module.arguments;
					if (loader.loadOnLaunch.file != null) {
						const data = new Uint8Array(await loader.loadOnLaunch.file.arrayBuffer());
						args.push(await loader.writeFile(loader.loadOnLaunch.path, loader.loadOnLaunch.file.name, data));
					} else if (loader.loadOnLaunch.link != null) {
						args.push(loader.loadOnLaunch.link);
					} else if (loader.loadOnLaunch.url != null) {
						loader.appendOutput(`Downloading ${loader.loadOnLaunch.url}…`);
						try {
							args.push(await loader.fetchUrlFile(loader.loadOnLaunch.url));
						} catch (downloadError) {
							loader.appendOutput(`Failed to download ${loader.loadOnLaunch.url}: ${downloadError.message}`, true, "red");
							loader.appendOutput("A server has to allow this page to read its files. You can drop the file into this page instead.", false);
							return;
						}
					}
					Module.callMain(args);
				});
			});
		}
	}

	return {
		create(options) {
			return new Loader(options);
		}
	};
})();
