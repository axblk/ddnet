# @ddnet/demo-player

Watch a DDNet demo in a browser.

```html
<link rel="stylesheet" href="@ddnet/base/viewer.css">
<ddnet-demo src="https://example.org/a.demo" controls="html"></ddnet-demo>
<script type="module">import "@ddnet/demo-player";</script>
```

The element answers what a `<video>` answers (`currentTime`, `duration`,
`paused`, `play()`, the same events) and takes `t`, `end`, `speed`, `paused`
and `spec` to say where to start and whom to watch. `nooverlays` keeps Tab
from showing the scoreboard, `nosettings` keeps the display settings out of
the menu. `controls="html"` is the package's bar, `controls` the one drawn by
the program; without either, a page puts its own into the `controls` slot and
steers `element.program`.

`DemoPlayer` runs on a canvas of the page's own, `DemoControls` is the bar for
it. The types are in `demo-player.d.ts`.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, or
`@ddnet/base/coi-serviceworker.js`), because the program uses threads.
