# ddnet-loader

Runs DDNet, its demo viewer and its map viewer in a browser: one canvas, one
call, and a handle to steer them with.

The programs themselves are what this repository builds with Emscripten —
`DDNet.js`, `ddnet-demo-viewer.js`, `ddnet-map-viewer.js`,
`ddnet-demo-render.js` and their `.wasm` files. This module is what puts one of
them on a page: it starts the program, gives it the file that was dropped or
named in the URL, keeps the browser's own rules (cross-origin isolation, a
download that has to come out of a click, a save dialog that has to be asked
for while the click is still the browser's idea of what the user is doing), and
hands back something a page can ask questions of.

## Three ways in

```js
import DDNetLoader from "ddnet-loader";

// One program on one canvas. Claims no globals: a page may have two of them,
// or one of these beside a program of its own.
const viewer = await DDNetLoader.start({
	module: DDNetDemoViewer,          // the program's factory
	canvas: document.querySelector("canvas"),
	accept: [".demo"],
	controls: false,                  // the page draws its own
	file: "https://example.org/a.demo",
});

const demo = DDNetLoader.demoControls(viewer);
demo.play();
demo.seek(0.5);

viewer.addEventListener("output", event => console.log(event.detail.message));
viewer.destroy();                     // lets go of the page again
```

`DDNetLoader.page` is the same plus the furniture our own pages share: a
loading line, a console log, and the canvas filling the window.
`DDNetLoader.render` is the same again with nothing on screen: a demo goes in,
an MP4 comes out, and it does the work in a worker of its own.

`demoControls` and `mapControls` are the two viewers' own controls, and the
rest — `autoHide`, `fullscreen`, `icon`, `paintIcons`, `exportSettingsForm` —
is what our pages are built out of, offered because a page building the same
interface would otherwise write it again.

## What a page has to bring

* **Cross-origin isolation.** The programs use threads and therefore
  `SharedArrayBuffer`, which needs `Cross-Origin-Opener-Policy: same-origin`
  and `Cross-Origin-Embedder-Policy: require-corp` on the page. A page that
  cannot set headers can load `coi-serviceworker.js` from this package
  instead, which puts them on with a service worker and reloads once.
* **The program's script.** `<script src="ddnet-demo-viewer.js">` names its
  factory, or `scriptUrl` says where to fetch one from — including from another
  origin, where the server allows it.

`DDNetLoader.supportProblem()` answers with what is missing, in a sentence, or
`null` when nothing is.

## Types

`ddnet-loader.d.ts` is written by hand beside the module and says what the
options and the handles are.

## Licence

Zlib, as the rest of DDNet. See `license.txt` in the root of the repository.
