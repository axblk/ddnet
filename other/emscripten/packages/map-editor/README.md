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

## The brush

The left button paints, held shift it takes a piece of the layer into the
brush instead, held control it rubs out; the right and middle buttons move the
map. `X` and `Y` turn the brush over, `R` turns it a quarter, and the digits
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
