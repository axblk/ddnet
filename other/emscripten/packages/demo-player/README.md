# @ddnet/demo-player

Watch a DDNet demo in a browser.

```html
<ddnet-demo src="https://example.org/a.demo" controls></ddnet-demo>
<script type="module">import "@ddnet/demo-player";</script>
```

That is the whole of it for a page that wants a demo in a box. The element
answers to what a `<video>` answers to - `currentTime`, `duration`, `paused`,
`playbackRate`, `volume`, `muted`, `play()`, `pause()`, and the events
`loadedmetadata`, `play`, `pause`, `timeupdate`, `ratechange`, `volumechange`,
`ended` and `error` - so a page that already drives a video drives this.
`t`, `speed`, `paused` and `spec` say where in the demo to start and who to
watch, the same words a link out of the player uses. `t` and `end` together
mark a piece out: watching stops at its end, starting over goes back to its
beginning, and an export writes that piece and nothing else - which is what a
link to a moment in a demo is for.

For a page that draws its own controls:

```js
import { createDemoPlayer } from "@ddnet/demo-player";

const player = await createDemoPlayer({
	canvas: document.querySelector("canvas"),
	controls: false,                  // the page draws its own
	src: "https://example.org/a.demo",
});
player.currentTime = 30;
player.addEventListener("timeupdate", () => bar.value = player.currentTime);
// Everything the viewer can be asked, under one handle:
player.controls.spectateName("nameless tee");
```

`demoPlayerPage` is the same plus the furniture a page of nothing but a demo
player wants: the canvas filling the window, a line that says what is loading,
and a file named in the page's own address.

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
