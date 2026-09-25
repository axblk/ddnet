// The page's side of the web tools' platform layer, see
// `src/engine/client/web/web_platform.h`. What is here needs the page: the
// canvas element and its events, and the audio output. Every function runs on
// the page's thread, whichever thread calls it.

addToLibrary({
	$DDNetWeb: {
		// Aborted when the input stops listening.
		inputStop: null,
		audio: null,
	},

	// Where the program's picture goes: the canvas the page handed over as
	// `Module.canvas`. It may sit in a shadow root, where `#canvas` finds
	// nothing, so it is registered under that name. The CSS size and the
	// device pixel ratio are what the first frame is drawn at.
	ddnet_web_canvas_prepare__proxy: 'sync',
	ddnet_web_canvas_prepare__deps: ['$specialHTMLTargets'],
	ddnet_web_canvas_prepare: (pWidth, pHeight, pScale) => {
		const canvas = Module.canvas;
		if (!canvas) {
			return 0;
		}
		specialHTMLTargets['#canvas'] = canvas;
		const box = canvas.getBoundingClientRect();
		HEAP32[pWidth >> 2] = Math.round(box.width);
		HEAP32[pHeight >> 2] = Math.round(box.height);
		HEAPF32[pScale >> 2] = globalThis.devicePixelRatio || 1;
		return 1;
	},

	// The canvas's events, handed to `WebInputPush` in
	// `src/engine/client/web/input_web.cpp`. Keys arrive as the USB usage
	// numbers `src/engine/keys.h` counts in, pointers in CSS pixels of the
	// canvas.
	ddnet_web_input_install__proxy: 'sync',
	ddnet_web_input_install__deps: ['$DDNetWeb', 'WebInputPush'],
	ddnet_web_input_install: () => {
		const canvas = Module.canvas;
		if (!canvas || DDNetWeb.inputStop) {
			return 0;
		}
		// Event.code to the key numbers, in the order of the USB usage table.
		const codes = new Map();
		const run = (first, names) => names.forEach((name, index) => codes.set(name, first + index));
		run(4, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('').map(letter => `Key${letter}`));
		run(30, ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0'].map(digit => `Digit${digit}`));
		run(40, ['Enter', 'Escape', 'Backspace', 'Tab', 'Space', 'Minus', 'Equal', 'BracketLeft', 'BracketRight', 'Backslash']);
		run(51, ['Semicolon', 'Quote', 'Backquote', 'Comma', 'Period', 'Slash', 'CapsLock']);
		run(58, ['F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12']);
		run(70, ['PrintScreen', 'ScrollLock', 'Pause', 'Insert', 'Home', 'PageUp', 'Delete', 'End', 'PageDown', 'ArrowRight', 'ArrowLeft', 'ArrowDown', 'ArrowUp']);
		run(83, ['NumLock', 'NumpadDivide', 'NumpadMultiply', 'NumpadSubtract', 'NumpadAdd', 'NumpadEnter']);
		run(89, ['Numpad1', 'Numpad2', 'Numpad3', 'Numpad4', 'Numpad5', 'Numpad6', 'Numpad7', 'Numpad8', 'Numpad9', 'Numpad0', 'NumpadDecimal']);
		run(224, ['ControlLeft', 'ShiftLeft', 'AltLeft', 'MetaLeft', 'ControlRight', 'ShiftRight', 'AltRight', 'MetaRight']);
		// The numbers of `EWebInputEvent`.
		const KEY_DOWN = 1, KEY_UP = 2, KEY_REPEAT = 3, POINTER_DOWN = 4, POINTER_MOVE = 5, POINTER_UP = 6, WHEEL = 7, VISIBLE = 8, RELEASE_ALL = 9;
		const stop = new AbortController();
		DDNetWeb.inputStop = stop;
		const signal = stop.signal;
		const push = (type, code, id, x, y) => _WebInputPush(type, code, id, x, y);
		const where = event => {
			const box = canvas.getBoundingClientRect();
			return [event.clientX - box.left, event.clientY - box.top];
		};
		// A touch is a finger, anything else the pointer; the id tells the
		// fingers apart.
		const pointer = (type, event) => {
			const [x, y] = where(event);
			const touch = event.pointerType === 'touch' ? 1 : 0;
			push(type, (event.button & 0xff) | (touch << 8) | ((event.isPrimary ? 1 : 0) << 9), event.pointerId, x, y);
		};
		canvas.addEventListener('keydown', event => {
			const code = codes.get(event.code);
			if (code === undefined) {
				return;
			}
			// The page's shortcuts stay the page's.
			if (event.ctrlKey || event.metaKey) {
				return;
			}
			event.preventDefault();
			push(event.repeat ? KEY_REPEAT : KEY_DOWN, code, 0, 0, 0);
		}, { signal });
		canvas.addEventListener('keyup', event => {
			const code = codes.get(event.code);
			if (code !== undefined) {
				push(KEY_UP, code, 0, 0, 0);
			}
		}, { signal });
		canvas.addEventListener('pointerdown', event => {
			// The pointer stays the canvas's while a button is held, so a drag
			// that leaves it still ends.
			try {
				canvas.setPointerCapture(event.pointerId);
			} catch (error) {}
			pointer(POINTER_DOWN, event);
		}, { signal });
		canvas.addEventListener('pointermove', event => pointer(POINTER_MOVE, event), { signal });
		for (const type of ['pointerup', 'pointercancel']) {
			canvas.addEventListener(type, event => pointer(POINTER_UP, event), { signal });
		}
		// Whether the wheel zooms or scrolls the page is the page's to say.
		canvas.addEventListener('wheel', event => {
			if (Module.ddnetWheel === false) {
				return;
			}
			event.preventDefault();
			const [x, y] = where(event);
			push(WHEEL, event.deltaY < 0 ? 1 : event.deltaY > 0 ? -1 : 0, 0, x, y);
		}, { signal, passive: false });
		// Keys held while the focus leaves are never let go of otherwise.
		canvas.addEventListener('blur', () => push(RELEASE_ALL, 0, 0, 0, 0), { signal });
		const visibility = () => push(VISIBLE, document.visibilityState === 'visible' ? 1 : 0, 0, 0, 0);
		document.addEventListener('visibilitychange', visibility, { signal });
		visibility();
		return 1;
	},

	ddnet_web_input_uninstall__proxy: 'sync',
	ddnet_web_input_uninstall__deps: ['$DDNetWeb'],
	ddnet_web_input_uninstall: () => {
		DDNetWeb.inputStop?.abort();
		DDNetWeb.inputStop = null;
	},

	// Opens the audio output: an AudioContext and a worklet that plays what the
	// mixer thread writes into the ring at `Ring` (see `SWebAudioRing` in
	// `src/engine/client/web/audio_web.h`). The worklet reads the ring straight
	// out of the program's memory. Its module is a blob of this page's own, so
	// that a program from another origin needs no second file. Answers the
	// sample rate the browser mixes at, or 0 without audio.
	ddnet_web_audio_open__proxy: 'sync',
	ddnet_web_audio_open__deps: ['$DDNetWeb', 'ddnet_web_audio_close'],
	ddnet_web_audio_open: (Rate, Ring, Capacity) => {
		if (typeof AudioContext === 'undefined' || typeof AudioWorkletNode === 'undefined') {
			return 0;
		}
		let context;
		try {
			context = new AudioContext({ sampleRate: Rate, latencyHint: 'interactive' });
		} catch (error) {
			try {
				context = new AudioContext({ latencyHint: 'interactive' });
			} catch (error) {
				return 0;
			}
		}
		const processor = `
class DDNetOutput extends AudioWorkletProcessor {
	constructor(options) {
		super();
		const { memory, ring, capacity } = options.processorOptions;
		this.header = new Int32Array(memory, ring, 4);
		this.samples = new Int16Array(memory, ring + 16, capacity * 2);
		this.mask = capacity - 1;
		this.running = true;
		this.port.onmessage = event => { this.running = event.data !== 'stop'; };
	}
	process(inputs, outputs) {
		const output = outputs[0];
		const left = output[0];
		const right = output[1] ?? left;
		const write = Atomics.load(this.header, 0);
		const read = Atomics.load(this.header, 1);
		const count = Math.min((write - read) >>> 0, left.length);
		for (let i = 0; i < count; ++i) {
			const index = ((read + i) & this.mask) * 2;
			left[i] = this.samples[index] / 32768;
			right[i] = this.samples[index + 1] / 32768;
		}
		Atomics.store(this.header, 1, (read + count) | 0);
		Atomics.notify(this.header, 1);
		return this.running;
	}
}
registerProcessor('ddnet-output', DDNetOutput);
`;
		const audio = { context, node: null, stopped: false, resume: null };
		DDNetWeb.audio = audio;
		const url = URL.createObjectURL(new Blob([processor], { type: 'text/javascript' }));
		context.audioWorklet.addModule(url).then(() => {
			URL.revokeObjectURL(url);
			if (audio.stopped) {
				return;
			}
			audio.node = new AudioWorkletNode(context, 'ddnet-output', {
				numberOfInputs: 0,
				numberOfOutputs: 1,
				outputChannelCount: [2],
				processorOptions: { memory: HEAP8.buffer, ring: Ring, capacity: Capacity },
			});
			audio.node.connect(context.destination);
		}, error => {
			URL.revokeObjectURL(url);
			console.error('DDNet: the audio worklet did not load:', error);
		});
		// A browser only lets a page make sound after the visitor did
		// something on it.
		const events = ['pointerdown', 'keydown', 'touchend'];
		audio.resume = () => {
			if (context.state === 'suspended') {
				context.resume().catch(() => {});
			}
			if (context.state === 'running') {
				events.forEach(type => document.removeEventListener(type, audio.resume, true));
			}
		};
		events.forEach(type => document.addEventListener(type, audio.resume, true));
		audio.resume();
		// The page stops the sound of a program that went away without
		// closing it.
		Module.ddnetStopAudio = () => _ddnet_web_audio_close();
		return context.sampleRate;
	},

	ddnet_web_audio_pause__proxy: 'sync',
	ddnet_web_audio_pause__deps: ['$DDNetWeb'],
	ddnet_web_audio_pause: (Paused) => {
		const audio = DDNetWeb.audio;
		if (!audio || audio.stopped) {
			return;
		}
		if (Paused) {
			audio.context.suspend().catch(() => {});
		} else {
			audio.resume();
		}
	},

	ddnet_web_audio_close__proxy: 'sync',
	ddnet_web_audio_close__deps: ['$DDNetWeb'],
	ddnet_web_audio_close: () => {
		const audio = DDNetWeb.audio;
		if (!audio) {
			return;
		}
		DDNetWeb.audio = null;
		audio.stopped = true;
		['pointerdown', 'keydown', 'touchend'].forEach(type => document.removeEventListener(type, audio.resume, true));
		if (audio.node) {
			audio.node.port.postMessage('stop');
			audio.node.disconnect();
		}
		audio.context.close().catch(() => {});
	},

	// The canvas a render thread was handed arrives in GL.offscreenCanvases
	// under its id; it is registered as `#canvas` as on the page's thread, so
	// that WebGPU and WebGL find it whatever the page called it.
	ddnet_web_render_thread_attach__deps: ['$GL', '$specialHTMLTargets'],
	ddnet_web_render_thread_attach: () => {
		const record = Object.values(GL.offscreenCanvases).find(Boolean);
		if (!record) {
			return 0;
		}
		specialHTMLTargets['#canvas'] = record.offscreenCanvas ?? record.canvas;
		return 1;
	},

	// The render thread paces itself on the page's frames: a dedicated worker
	// has animation frames of its own. A hidden page paints none, and would
	// not come back if nothing ran, so a timeout races the frame.
	ddnet_web_render_thread_at_frame__deps: ['WebRenderThreadRun', '$callUserCallback'],
	ddnet_web_render_thread_at_frame: (Visible, Task, User) => {
		let done = false;
		const run = () => {
			if (!done) {
				done = true;
				callUserCallback(() => _WebRenderThreadRun(Task, User));
			}
		};
		if (Visible && typeof requestAnimationFrame === 'function') {
			requestAnimationFrame(run);
		}
		setTimeout(run, 100);
	},
});
