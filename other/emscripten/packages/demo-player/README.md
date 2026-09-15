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

`controls="html"` puts the same bar there as real buttons - a tooltip each, a
tab order, a screen reader that can say what they are - instead of one painted
into the picture:

```html
<link rel="stylesheet" href="@ddnet/base/viewer.css">
<ddnet-demo src="https://example.org/a.demo" controls="html"></ddnet-demo>
```

The package builds it and wires it to that one player, so a page with three
demos on it gets three bars and none of them knows about the others;
`element.bar` is the one that belongs to an element, and `bar.part("play")`
one of its buttons. `demo.html` in this repository is exactly this line.
`DemoControls` is the same bar for a page that has a player of its own and
wants to place it itself.

A page that would rather draw the bar itself instead puts its own controls
into the element, in the `controls` slot, where they are laid over the
picture:

```html
<ddnet-demo src="https://example.org/a.demo">
	<div class="bar" slot="controls">…buttons of your own…</div>
</ddnet-demo>
```

`element.controls` is the player to steer from them.

For a page that brings its own canvas as well:

```js
import { DemoPlayer } from "@ddnet/demo-player";

const player = await DemoPlayer.open({
	canvas: document.querySelector("canvas"),
	controls: false,                  // the page draws its own
	src: "https://example.org/a.demo",
});
player.currentTime = 30;
player.addEventListener("timeupdate", () => bar.value = player.currentTime);
// Everything only a demo can be asked is on the player as well:
player.spectateName("nameless tee");
player.clip(4, 8);
```

`DemoPlayer.openPage` is the same plus the furniture a page of nothing but a
demo player wants: the canvas filling the window, a line that says what is
loading, and a file named in the page's own address.

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
