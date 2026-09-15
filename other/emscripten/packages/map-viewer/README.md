# @ddnet/map-viewer

Look at a DDNet map in a browser.

```html
<ddnet-map src="https://example.org/a.map" controls x="120" y="64" tiles="90"></ddnet-map>
<script type="module">import "@ddnet/map-viewer";</script>
```

`x`, `y` and `tiles` are where to look and how much of the map to show, in
tiles - which is how a map is written and how anybody talks about one. They are
the same words a link out of the viewer uses.

`controls="html"` puts the buttons in the corner there as real buttons instead
of ones painted into the picture:

```html
<link rel="stylesheet" href="@ddnet/base/viewer.css">
<ddnet-map src="https://example.org/a.map" controls="html"></ddnet-map>
```

The package builds them and wires them to that one viewer, so a page with two
maps on it gets two sets and neither knows about the other; `element.bar` is
the set that belongs to an element. `map.html` in this repository is exactly
this line. `MapControls` is the same set for a page that has a viewer of its
own and wants to place them itself.

A page that would rather draw the buttons itself instead puts its own into the
element, in the `controls` slot, where they are laid over the map:

```html
<ddnet-map src="https://example.org/a.map" x="120" y="64" tiles="90">
	<div class="corner" slot="controls">…buttons of your own…</div>
</ddnet-map>
```

`element.controls` is the viewer to steer from them.

For a page that brings its own canvas as well:

```js
import { MapViewer } from "@ddnet/map-viewer";

const viewer = await MapViewer.open({
	canvas: document.querySelector("canvas"),
	controls: false,
	src: "https://example.org/a.map",
});
viewer.center(120, 64);
viewer.tilesAcross(90);
viewer.entities(true);
viewer.exportFullMap();               // a picture of the whole map, as a download
```

`MapViewer.openPage` is the same plus the furniture a page of nothing but a map
viewer wants.

The program itself is WebAssembly and ships inside this package, beside the
module; nothing has to be told where it is. What every program of this family
needs - the canvas, the files, the full screen - is
[`@ddnet/base`](../..), the runtime this is built on, and a page may use that
directly as well.

A browser has to be cross-origin isolated to run any of this: these programs
use threads, and a browser only hands out shared memory to a page that sends
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Where the headers are not yours
to set, `@ddnet/base/coi-serviceworker.js` sets them from a service worker.

Not published anywhere yet: what is here is the package as it would be
published, so that a page inside this repository uses exactly what a page
outside it would.
