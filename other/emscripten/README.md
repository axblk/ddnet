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

## The demo player bundle

A program that hands out the demo player itself, a server host that shows
its live streams for example, takes it from the `demo-player-bundle` target
of a browser build:

```sh
cmake --build <browser build directory> --target demo-player-bundle
```

It writes `demo-player-bundle/` in the build directory, replaced as a whole
each time, and nothing else of the site, about 15 MB (13 MB of it `data/`):

| Path | What it is |
|---|---|
| `VERSION` | `key=value` lines: `version` (what `git describe --tags` says), `revision` (16 hex digits), `release` (the DDNet version) and `live_stream` (the version of a live stream's `index.json` the player reads) |
| `ddnet-demo-player.js`, `ddnet-demo-player.wasm` | the program |
| `demo-player.js`, `ddnet-base.js` | `@ddnet/demo-player` and `@ddnet/base` |
| `ddnet-viewer.css`, `ddnet-page.css`, `DDNet.ico` | the look of the bar and of `demo.html` |
| `demo.html` | a page with just the player (`#demo=<url>` or `#live=<index.json>`) |
| `coi-serviceworker.js` | for a page that cannot send the isolation headers |
| `data/` | what the player reads of the data directory, `packages/demo-player/bundle-data.txt`, with an `index.txt` of just that; no maps, a demo carries its map |

A page that is not `demo.html` maps `@ddnet/base` and `@ddnet/demo-player` to
the two modules in its import map and links `ddnet-viewer.css`; the player
finds its program and `data/` beside `demo-player.js`. The revision is the
CMake variable `GIT_REVISION` if it is set, else what `git archive` filled into
`scripts/git_archive_revision.txt`, else what git says of the checkout
(`scripts/git_revision.py`). The target fails when the server writes another
version of `index.json` than the player reads.
