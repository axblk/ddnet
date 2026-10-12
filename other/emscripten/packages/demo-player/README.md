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
so any static web server that answers them will do. A stream is a row of
parts: a new map goes on in a demo of its own, and so does the next visit to
a server the stream rested on while it was empty. A viewer who joins watches
the newest part at its live end, an ended stream plays from its first part
on, and the menu of the bar has the other parts.

The seek bar is the whole part that plays, with the markers of the stream on
it (the ends of matches, and `live_marker`), and a button goes to the live
end. Only a piece of the part is fetched: the last two minutes for a viewer
who joins, and as much around where the viewer seeks. What follows is
fetched as the playback gets near it, and what lies more than half an hour
behind is dropped again.

`DemoPlayer` runs on a canvas of the page's own, `DemoControls` is the bar for
it. The canvas is the program's once it runs, see `@ddnet/base`: its size goes
through `setSize`. The types are in `demo-player.d.ts`.

The page has to be cross-origin isolated (`Cross-Origin-Opener-Policy:
same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, or
`@ddnet/base/coi-serviceworker.js`), because the program uses threads.
The player's files need no headers of their own; on another origin than the
page, they need CORS (see `@ddnet/base`).
