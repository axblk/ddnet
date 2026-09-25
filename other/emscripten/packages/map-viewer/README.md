# @ddnet/map-viewer

Look at a DDNet map in a browser.

```html
<link rel="stylesheet" href="@ddnet/base/viewer.css">
<ddnet-map src="https://example.org/a.map" controls="html" x="120" y="64" tiles="90"></ddnet-map>
<script type="module">import "@ddnet/map-viewer";</script>
```

`x`, `y` and `tiles` say where to look and how much to show, in tiles.
`controls`, whatever its value, is the package's buttons; without it, a page
puts its own into the `controls` slot and steers `element.program`. The
program draws no buttons of its own in a browser.

`MapViewer` runs on a canvas of the page's own, `MapControls` are the buttons
for it. The canvas is the program's once it runs, see `@ddnet/base`: its size
goes through `setSize`. The types are in `map-viewer.d.ts`.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, or
`@ddnet/base/coi-serviceworker.js`), because the program uses threads.
