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
the menu. `controls`, whatever its value, is the package's bar; without it, a
page puts its own into the `controls` slot and steers `element.program`. The
program draws no bar of its own in a browser.

A live stream that a server writes (`live_start`, `sv_live_dir`) plays with
`live` in place of `src`, the address of its `index.json`:

```html
<ddnet-demo live="https://example.org/live/main/index.json" controls="html"></ddnet-demo>
```

It is read every second and what it grew by is fetched with range requests,
so any static web server that answers them will do. A viewer who joins gets
the last two minutes of the map; seeking back to where they begin fetches
older ones, and at the live end more than half an hour is dropped again. The bar then has a button
to the live end and the markers of the stream (the ends of matches, and
`live_marker`) on its seek bar; a new map goes on in a demo of its own.

`DemoPlayer` runs on a canvas of the page's own, `DemoControls` is the bar for
it. The canvas is the program's once it runs, see `@ddnet/base`: its size goes
through `setSize`. The types are in `demo-player.d.ts`.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, or
`@ddnet/base/coi-serviceworker.js`), because the program uses threads.
The player's files need no headers of their own; on another origin than the
page, they need CORS (see `@ddnet/base`).
