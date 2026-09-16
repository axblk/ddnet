# @ddnet/map-editor

Edit a DDNet map in a browser. The map itself lives in WebAssembly - every
version of it, so that undo is a pointer swap rather than a list of things to
take back - and the panels around it are the page's, built out of what the
program says the map is made of.

```js
import { MapEditor, EditorPanels, steerEditor } from "@ddnet/map-editor";

const editor = await MapEditor.open({ canvas, src: "https://…/a.map" });
steerEditor(editor, { canvas });
new EditorPanels(editor, { container: document.querySelector("#panels") });
```

The panels want `@ddnet/base/editor.css`; without it they are still there and
still work, they are just unpainted.

## What it is made of

* **The map is asked, not told.** `editor.structure()` answers what the map is
  made of - groups, layers, envelopes, images - as plain objects. What is *in*
  a layer is not in there: a tile layer says how large it is, and its tiles are
  drawn by the program.
* **Every change is a command.** `editor.apply({op: "layer.setProp", …})`, and
  the answer says whether it was taken. A command that is refused leaves the
  map exactly where it was.
* **A change that is made over many commands is one entry.** `editor.begin()`,
  as many commands as the dragging takes, `editor.commit()`. Every step in
  between is already what is drawn - that is the preview - and the history gets
  one line.
* **Nothing polls.** The program says when a map changed, and the editor is an
  `EventTarget`: `document`, `loaded`, `saved`, `closed`, `error`.
* **Several maps, one editor.** Every call takes the number of the map it is
  about, and leaving it out means the one in front. Tabs are a page's business;
  a second program is not needed for them.
* **The keyboard is the page's.** The program never listens at the document.
  Not a key reaches it that the page did not hand it.

## Looking rather than changing

Some of what an editor does changes nothing about the map, and none of it
writes a history entry or reaches a file: `editor.highDetail()` leaves out what
is only there to be looked at, `editor.entities(100)` draws what the tiles do
instead of what they look like, `editor.animate()` lets the envelopes run,
`editor.visible(group, layer, false)` switches one layer off to look under it,
and `editor.grid(10)` puts lines every ten tiles. The grid follows the group
the game layer is in, so its lines sit on that group's tiles at every zoom, and
it leaves itself out when it would be closer together than a few pixels. The
panels put all of these on the bar, the eye beside each layer in the list, and
`G` on the keyboard for the grid.

## The panels

The layer list is the map's own order: a layer or a group can be dragged to
where it belongs, or moved a step at a time with the arrows, and either way it
is the same command and the same one history entry. The eye beside a layer
switches it off, the bar carries the switches above, and a map that has been
changed and not written out says so - the page asks before the tab is closed,
and every minute what has changed goes into the browser's own storage by
itself.

## The brush

The left button paints, held shift it takes a piece of the layer into the
brush instead, held alt it fills a rectangle with it, held control it rubs out;
the right and middle buttons move the map. What a rectangle gesture is about
is drawn on the map while the button is down. `X` and `Y` turn the brush over, `R` turns it a quarter, and the digits
are ten slots to put one away in (`Shift` and a digit stores).

A stroke is one change made of many stamps, which is why it costs what it
touched rather than what the map is: a hundred strokes of eight stamps each
take 72 ms on ctf1 and 104 ms on Tsunami, a map eight times its size, and both
add the same 1.83 MiB to the history. Taking all hundred back again is 23 ms
and 17 ms.

## What it is not, yet

Quads, envelopes, images, sounds and the automapper are looked at but not
changed; the numbers beside a tele or a switch tile cannot be set. The tileset
of an image that is packed into the map file is shown as a grid of numbers
rather than as a picture - the pixels are in the program, and nothing hands
them out yet.
