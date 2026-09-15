# ddnet-loader

The runtime the DDNet programs run on in a browser: one canvas, one call, and a
handle to steer them with.

The programs themselves are what this repository builds with Emscripten —
`DDNet.js`, `ddnet-demo-player.js`, `ddnet-map-viewer.js`,
`ddnet-demo-render.js` and their `.wasm` files. This module is what puts one of
them on a page: it starts the program, gives it the file that was dropped or
named in the URL, keeps the browser's own rules (cross-origin isolation, a
download that has to come out of a click, a save dialog that has to be asked
for while the click is still the browser's idea of what the user is doing), and
hands back something a page can ask questions of.

## One package per program

A page that wants a demo player wants a demo player, not a runtime and a
program to point it at. That is what the three packages beside this one are,
each holding its own program and the module that steers it:

| Package | What it is | What it gives |
|---|---|---|
| [`@ddnet/demo-player`](packages/demo-player) | Watch a demo | `createDemoPlayer`, `<ddnet-demo>`, a `<video>`-shaped interface |
| [`@ddnet/map-viewer`](packages/map-viewer) | Look at a map | `createMapViewer`, `<ddnet-map>`, everything said in tiles |
| [`@ddnet/demo-renderer`](packages/demo-renderer) | Turn a demo into an MP4 | `renderDemo`, in a worker of its own |

```js
import { createDemoPlayer } from "@ddnet/demo-player";

const player = await createDemoPlayer({ canvas, src: "https://…/a.demo" });
player.currentTime = 30;
```

Each of them finds its own program beside itself and needs to be told nothing
about where it is. This package is what they are built on, and a page that
wants what is under them — a program of its own, the full screen, the icons,
the stylesheets — uses it directly.

## Three ways in

```js
import DDNetLoader from "ddnet-loader";

// One program on one canvas. Claims no globals: a page may have two of them,
// or one of these beside a program of its own.
const viewer = await DDNetLoader.start({
	module: DDNetDemoPlayer,          // the program's factory
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

`autoHide`, `fullscreen`, `icon`, `paintIcons`, `exportSettingsForm` and
`loadingHint` are what our pages are built out of, offered because a page
building the same interface would otherwise write it again. What a particular
viewer can be asked — `demoControls`, `mapControls` — belongs to its package.

The stylesheets that dress them come with the package and are imported by
name: `ddnet-loader/viewer.css` for the bar, the menus and the panels a viewer
page puts over the picture, `ddnet-loader/page.css` for a page that is nothing
but a viewer, and `ddnet-loader/render.css` for one that shows a render and
what it is doing. The element brings its own and needs none of them.

## One line

For a page that only wants a viewer in a box, the element is the whole of it.
It comes with the package it belongs to and is defined by importing it - one
`import "@ddnet/demo-player"` and `<ddnet-demo>` works:

```html
<script type="module" src="ddnet-loader.js"></script>

<ddnet-demo src="https://example.org/a.demo" controls></ddnet-demo>
<ddnet-map src="https://example.org/a.map" controls x="120" y="64" tiles="90"></ddnet-map>
```

The picture lives in a shadow root, so nothing in it shares a name with the
page around it — no `#canvas` to collide with, and no stylesheet of ours
landing on somebody else's buttons. What may be styled from outside is named:

```css
ddnet-map { width: 800px; height: 450px; }
ddnet-map::part(picture) { image-rendering: pixelated; }
ddnet-map::part(message) { font-family: monospace; }
```

`controls` is the bar the viewer draws for itself; without it the element shows
nothing but the picture, and the page steers it through `element.controls`,
which is the same `demoControls`/`mapControls` handle as above. `nozoom` takes
the wheel, the zoom keys and the pinch off a demo, for a page that scrolls
around the viewer or wants the demo at one size and no other; the page can
still zoom it itself through `element.controls.zoom(factor)`. `orientation`
says which way round to turn a phone while the viewer's own button fills the
screen — `landscape` for the most of a wide picture. Without it the device is
left where whoever is holding it put it. The attributes
beyond `src` are the ones the viewer pages spell in their address — `t`,
`speed`, `paused`, `spec` for a demo, `x`, `y`, `tiles` for a map — so a link
somebody copied out of a viewer and an element somebody wrote by hand say the
same things by the same names. Changing one later moves the running viewer;
changing `src` shows another file.

`<ddnet-demo>` answers to what a `<video>` answers to, because a demo in a page
is a video to whoever put it there:

```js
const demo = document.querySelector("ddnet-demo");
demo.addEventListener("loadedmetadata", () => console.log(demo.duration));
demo.addEventListener("timeupdate", () => bar.value = demo.currentTime / demo.duration);
demo.currentTime = 30;
demo.playbackRate = 2;
demo.muted = true;
await demo.play();
```

`duration`, `currentTime`, `paused`, `ended`, `playbackRate`, `volume`,
`muted`, `src`, `play()` and `pause()`, with `loadedmetadata`,
`durationchange`, `play`, `pause`, `timeupdate`, `ratechange`, `volumechange`,
`ended` and `error` as events. A page that already drives a video drives this
without being told anything new. What is missing is what a demo has no answer
for: `buffered`, `seekable`, `readyState`, `networkState`, `preload`,
`poster`, `loop` and the tracks.

`element.ready` is the running program, so a page that wants to do more can
wait for it, and taking the element out of the page stops the program and lets
go of everything it held.

## What a page has to bring

* **Cross-origin isolation.** The programs use threads and therefore
  `SharedArrayBuffer`, which needs `Cross-Origin-Opener-Policy: same-origin`
  and `Cross-Origin-Embedder-Policy: require-corp` on the page. A page that
  cannot set headers can load `coi-serviceworker.js` from this package
  instead, which puts them on with a service worker and reloads once.
* **The program's script.** `<script src="ddnet-demo-player.js">` names its
  factory, or `scriptUrl` says where to fetch one from — including from another
  origin, where the server allows it.

`DDNetLoader.supportProblem()` answers with what is missing, in a sentence, or
`null` when nothing is.

## Types

`ddnet-loader.d.ts` is written by hand beside the module and says what the
options and the handles are.

## Licence

Zlib, as the rest of DDNet. See `license.txt` in the root of the repository.
