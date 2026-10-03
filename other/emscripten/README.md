# @ddnet/base

What the DDNet web packages share: starting an Emscripten build of a DDNet
program on a canvas, handing it files, and the element the viewers are made
of.

| Package | What it is |
|---|---|
| [`@ddnet/demo-player`](packages/demo-player) | `<ddnet-demo>`, a demo player that answers like a `<video>` |
| [`@ddnet/map-viewer`](packages/map-viewer) | `<ddnet-map>`, a map viewer that speaks in tiles |
| [`@ddnet/demo-renderer`](packages/demo-renderer) | `DemoRenderer` and `renderDemo`, demos into MP4s |

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

A program's script is fetched once; its threads start from a blob of it, and
its `.wasm` and data are fetched too. So none of these files needs a header of
its own on the page's origin, not even `Cross-Origin-Embedder-Policy`. A page
on another origin needs them served with CORS (`Access-Control-Allow-Origin`).
`coi-serviceworker.js` isolates a page only once its service worker is active,
which takes a reload the first time.

The files of the data directory are served beside the page, below `data/`
(or where `Module.ddnetDataBase` says), and fetched as they are needed, never
all at once. The full client first fetches `data/index.txt`, which lists every
file with its size and hash, and the digests of every map, so that it can list
the directory, join a server whose map is there without downloading it, and
name each file by its hash in its address, which lets the files be cached for
good. The demo player, the map viewer and the renderer fetch no index: they ask
for `data/<path>` by name, and a 404 says a file is not there. Only text files
(`.cfg`, `.json`, `.rules`, `.txt`) are read as they are opened, which waits
for their request; everything else is fetched without waiting (see
`src/base/web_data.h`).

`demo.html`, `map.html`, `render.html` and `index.html` are the pages of the
web site built by the `web-site` target; `server.py` serves them locally.

`index.html#connect=<link>` starts the client and joins a server right away.
The link is what the server browser copies, for example
`ddnet+wt://203.0.113.5:8303#cert-sha256=…`, and it is the rest of the page
address, so it comes last. `?connect=` works as well, with the link escaped
(`%23` for its `#`). An address without the scheme of a link, such as
`wss://example.org:8304`, is joined as it is.
