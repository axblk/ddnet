# @ddnet/base

What the DDNet web packages share: starting an Emscripten build of a DDNet
program on a canvas, handing it files, and the element the viewers are made
of.

| Package | What it is |
|---|---|
| [`@ddnet/demo-player`](packages/demo-player) | `<ddnet-demo>`, a demo player that answers like a `<video>` |
| [`@ddnet/map-viewer`](packages/map-viewer) | `<ddnet-map>`, a map viewer that speaks in tiles |
| [`@ddnet/demo-renderer`](packages/demo-renderer) | `renderDemo`, a demo into an MP4 in a worker |

A program without a package of its own, such as the full client, is started
directly:

```js
import { Program } from "@ddnet/base";

const client = await Program.open({ module: DDNetClient, canvas });
client.addEventListener("output", event => console.log(event.detail.message));
```

`viewer.css` dresses the bars the packages build (`controls="html"`). The
element itself keeps its picture in a shadow root; `::part(picture)` and
`::part(message)` style it from outside.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`), because the
programs use threads. A page that cannot send headers loads
`coi-serviceworker.js` first. `supportError()` says what a browser lacks.

The types, and with them the documentation of the API, are in
`ddnet-base.d.ts`.

`demo.html`, `map.html`, `render.html` and `index.html` are the pages of the
web site built by the `web-site` target; `server.py` serves them locally.
