/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

/**
 * The DDNet demo renderer, as one module: a demo goes in and an MP4 comes out.
 * Nothing is shown while it runs - the game is drawn into a surface without a
 * window, as fast as the machine manages, and every frame is handed to the
 * browser's own encoder.
 *
 * ```js
 * import { renderDemo } from "@ddnet/demo-renderer";
 *
 * const video = await renderDemo({
 *     demo: "https://…/a.demo",
 *     width: 1920, height: 1080, fps: 60,
 *     onRenderProgress: status => console.log(status.progress),
 * });
 * ```
 *
 * It needs WebGPU, because that is the one way a browser draws without a
 * window, and a `VideoEncoder`. Both are said to be missing before a demo is
 * fetched for nothing.
 *
 * By itself it runs in a worker of its own, so the page it was asked from goes
 * on drawing while a film is written. `worker: false` keeps it on the page's
 * own thread, which is worth it for nothing but debugging.
 *
 * The program itself is WebAssembly and lives beside this file; nothing here
 * has to be told where it is.
 */

import DDNetLoader, { importProgram, render } from "ddnet-loader";

/** The program, and what its script calls the factory it defines. */
const PROGRAM = "ddnet-demo-render.js";
const MODULE_NAME = "DDNetDemoRenderer";
const PROGRAM_NAME = "The demo renderer";

/** Where the program's script is, for a page that wants to fetch it early. */
export const programUrl = new URL(PROGRAM, import.meta.url).href;

/**
 * Writes a video of a demo.
 *
 * Everything `ddnet-loader`'s `render` takes may be said here as well - the
 * size, the frame rate, the codec, what to follow, where to write it. What
 * does not have to be said is which program to run and where it is: that is
 * what this module is.
 *
 * @returns a promise for the finished MP4 as a `Blob`, or for `null` where the
 * page said where to write it itself.
 */
export async function renderDemo(options) {
	const settings = Object.assign({}, options);
	const scriptUrl = settings.scriptUrl || programUrl;
	// A render in a worker fetches the program itself, out of the way of the
	// page; one on the page's own thread needs it here.
	const onPageThread = settings.worker === false || typeof Worker !== "function";
	const factory = settings.module || (onPageThread ? await importProgram(scriptUrl, MODULE_NAME) : undefined);
	// An option that is there but says nothing is still an option, and this
	// one is only there for a render that stays on the page's thread.
	if (factory !== undefined) {
		settings.module = factory;
	}
	return await render(Object.assign(settings, {
		scriptUrl: scriptUrl,
		moduleName: MODULE_NAME,
		programName: settings.programName || PROGRAM_NAME,
	}));
}

export default {
	renderDemo,
	programUrl,
	loader: DDNetLoader,
};
