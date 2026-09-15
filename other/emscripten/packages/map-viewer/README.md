# @ddnet/map-viewer

Look at a DDNet map in a browser.

```html
<ddnet-map src="https://example.org/a.map" controls x="120" y="64" tiles="90"></ddnet-map>
<script type="module">import "@ddnet/map-viewer";</script>
```

`x`, `y` and `tiles` are where to look and how much of the map to show, in
tiles - which is how a map is written and how anybody talks about one. They are
the same words a link out of the viewer uses.

For a page that draws its own controls:

```js
import { createMapViewer } from "@ddnet/map-viewer";

const viewer = await createMapViewer({
	canvas: document.querySelector("canvas"),
	controls: false,
	src: "https://example.org/a.map",
});
viewer.center(120, 64);
viewer.tilesAcross(90);
viewer.entities(true);
viewer.exportFullMap();               // a picture of the whole map, as a download
```

`mapViewerPage` is the same plus the furniture a page of nothing but a map
viewer wants.

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
