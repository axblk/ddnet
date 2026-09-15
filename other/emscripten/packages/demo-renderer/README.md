# @ddnet/demo-renderer

Turn a DDNet demo into an MP4, in a browser.

```js
import { renderDemo } from "@ddnet/demo-renderer";

const video = await renderDemo({
	demo: "https://example.org/a.demo",
	width: 1920, height: 1080, fps: 60,
	onRenderProgress: status => console.log(status.progress),
});
```

Nothing is shown while it runs: the game is drawn into a surface without a
window, as fast as the machine manages, and every frame is handed to the
browser's own encoder. It runs in a worker of its own, so the page that asked
for it goes on drawing while a film is written.

It needs WebGPU - the one way a browser draws without a window - and a
`VideoEncoder`. Both are said to be missing before a demo is fetched for
nothing.

A film can be longer than a tab can hold: `videoSink` takes a
`FileSystemWritableFileStream` from `showSaveFilePicker`, and then what is
encoded is written as it is encoded and nothing is kept in memory.

The program itself is WebAssembly and ships inside this package, beside the
module; nothing has to be told where it is. What every program of this family
needs - the canvas, the files, the full screen - is
[`ddnet-loader`](../..), the runtime this is built on, and a page may use that
directly as well.

A browser has to be cross-origin isolated to run any of this: these programs
use threads, and a browser only hands out shared memory to a page that sends
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Where the headers are not yours
to set, `ddnet-loader/coi-serviceworker.js` sets them from a service worker.

Not published anywhere yet: what is here is the package as it would be
published, so that a page inside this repository uses exactly what a page
outside it would.
