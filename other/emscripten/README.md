# @ddnet/base

What the DDNet web packages share: starting an Emscripten build of a DDNet
program on a canvas, handing it files, and the element the viewers are made
of.

| Package | What it is |
|---|---|
| [`@ddnet/demo-player`](packages/demo-player) | `<ddnet-demo>`, a demo player that answers like a `<video>` |
| [`@ddnet/map-viewer`](packages/map-viewer) | `<ddnet-map>`, a map viewer that speaks in tiles |
| [`@ddnet/demo-renderer`](packages/demo-renderer) | `renderDemo`, a demo into an MP4 |

A program without a package of its own, such as the full client, is started
directly:

```js
import { Program } from "@ddnet/base";

const client = await Program.open({ module: DDNetClient, canvas });
client.addEventListener("output", event => console.log(event.detail.message));
```

`viewer.css` dresses the bars the packages build (`controls`). The element
itself keeps its picture in a shadow root; `::part(picture)` and
`::part(message)` style it from outside.

The demo player, the map viewer and the renderer run in workers (the full
client does not yet): the program on a thread of its own, and its drawing,
with WebGPU or with WebGL 2 where the browser has no WebGPU adapter, on a
thread the canvas is handed to (`transferControlToOffscreen`). The page's
thread only passes events on and is never made to wait. Once a program has
the canvas, the page can neither size it through its `width` and `height`
nor draw on it: the size goes through the program (`setSize`, or
`followSize`, which the elements use), and a canvas serves one program. The
canvas needs no id. The sound goes to an AudioWorklet whose module is made
from a blob, so that nothing but the program is served for it; a page with a
Content Security Policy has to allow `blob:` worklets.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`), because the
programs use threads and shared memory. A page that cannot send headers loads
`coi-serviceworker.js` first. `supportError()` says what a browser lacks.

The types, and with them the documentation of the API, are in
`ddnet-base.d.ts`.

`demo.html`, `map.html`, `render.html` and `index.html` are the pages of the
web site built by the `web-site` target; `server.py` serves them locally.
