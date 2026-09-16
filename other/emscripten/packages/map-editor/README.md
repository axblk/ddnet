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
* **And a run of changes of the same thing is one entry too.** A number field
  stepped ten times with its arrows is ten changes, each with its own
  `begin`/`commit`, but one thing to undo: they carry the same `merge` key and
  fold into one entry while they keep coming. Nothing merges that does not say
  so, so two brush strokes stay two.
* **Going back takes the panels along.** The entry remembers what was selected
  when it was made, so an undo puts the layer that was worked in back in front
  instead of leaving it to be looked for.
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

A tile layer's properties hold its width and its height. They are the one pair
of fields that is only handed over when the field is left rather than at every
keystroke: on the way from 8 to 150 the number is 1 and then 15, and for a size
that would mean the tiles outside are gone before the number is finished. The
physics layers of a map are all the size of its game layer - that is the size
the game plays - so resizing one of them resizes all of them, and a layer that
is only drawn is resized by itself.

Under the properties of such a layer stand the thirteen construct operations:
pick a physics tile and every tile the layer draws gets one under it. That is
how a map is built - the shape is drawn once and the physics follow it. The row
is only there for a layer the operation means anything for, and whether it does
is the program's answer (`layer.construct`), not the page's: it depends on the
group lying over the game layer tile for tile, and a group that moves with the
camera is somewhere else at every moment. The two checkpoints are tele tiles,
so they go into the tele layer - and a map without one gets one.

## The automapper

A rules file belongs to a picture, not to a layer: a layer drawn with
`grass_main` is automapped by `grass_main.rules`. So the panel follows the
picture - it fetches the file once, hands the text to the program, and offers
the configurations the program found in it. A picture with no rules beside it
costs one 404 and then the row is simply not there.

What the rules say and what they do to a layer is the same grammar and the
same sum as in the editor in the client, ported rather than reinvented, so
that a map automapped in the browser comes out as it would have come out
natively. The first run of a configuration may be filtered by one kind of
physics tile, which is how one file draws freeze and hookable out of the same
game layer; the rest read the layer they are writing into.

A layer can also be told to do it by itself: then every stroke runs the rules
over what it drew, while the stroke's change is still open, so drawing and
what it led to are one thing to undo. Only the rectangle the stroke was over
is run, with the margin the rules need - which is why automapping while
drawing costs what was touched rather than what the map is.

A run over a rectangle gives the same tiles as a run over everything, because
the rectangle is worked out with a margin as wide as the rules reach and only
the rectangle itself is written back. And the rules that only fire sometimes
fire off a hash of the place rather than a die, so the same seed twice is the
same map - which is why running the same configuration again writes no history
entry at all: it changed nothing.

## Pictures

A picture is read in by the browser, not by the program: a browser reads PNGs,
and what comes out of a canvas is already the RGBA the map keeps. The pixels
then cross over as bytes rather than as a command - a picture of a thousand by
a thousand is four megabytes, and four megabytes of JSON is a text nobody
should have to write or read - which is why `editor.addImage(name, pixels)`
and `editor.setImagePixels(index, pixels)` are calls of their own and the rest
of what a picture has (`image.add` for one that lies beside the map,
`image.delete`, `image.setProp`) goes through the ordinary commands.

Replacing a picture keeps every layer that is drawn with it: the tiles stay
where they are and the picture under them changes. Taking one away does the
opposite and takes it off them - a layer names a picture by its place, so the
ones after it come down one and a layer that was drawn with the one that is
gone is drawn with none. Taking the pixels back out ("out") leaves the name,
which is a picture that lies beside the map again.

## Envelopes

The envelope panel is an SVG, not a canvas, and that is the point: a few dozen
points that are dragged one at a time is exactly what an SVG is for - the
browser hit-tests them and hands over a pointer, and nothing has to be drawn
twice to find out what was clicked. A click where there is no point makes one,
a point is dragged where it belongs, and a drag is one history entry however
far it travelled. A point dragged past its neighbour changes places with it,
because the points of an envelope are in time order and the sum that reads them
counts on that - the document sees to it and says where the point ended up.

Times are whole milliseconds and values are the map's own 22.10 fixed point:
whole numbers out through `editor.envelope(index)` and whole numbers back in,
so a map that is read and written again comes back byte for byte. What a value
means is a question about the channels - four are a colour, three a place and a
turn, one a volume - and that is the panel's business, not the document's.

Taking an envelope away takes it off everything that was bound to it: a layer
or a quad names an envelope by its place, so the bindings above it come down
one and a binding to the one that is gone becomes no binding at all. A layer
that was bound to nothing is left as the node it is, which is why this costs
the layers that used the envelope rather than the map.

## Quads

A quad is four corners and a pivot, and all five are dragged on the map rather
than typed into a field: the panel lists the quads by where their pivot sits,
picking one puts handles on it, and the program draws those handles because a
quad lies in its group's coordinates - parallax and all - and only the program
knows where that is on the screen. Dragging a corner moves that corner;
dragging the pivot carries all five, which is how a quad is moved without
changing its shape. Either way the whole drag is one history entry.

What is not a point is a field: a colour on each of the four corners with its
alpha beside it, and which envelopes move and colour the quad. A binding names
an envelope by its place, so one the map does not have is refused rather than
written - a map that reads back differently than it was written is not a saved
map.

The points go out in world units (`editor.quads(group, layer)`), because that
is the only number a page can do anything with - it turns a click into a place
and back - and the pointer asks the program where a click lands in the group
(`editor.groupWorldAt(group, x, y)`) rather than working it out itself, so
what is drawn and what is caught cannot drift apart. A new quad appears in the
middle of the view of *its group*, not of the plain view: in a group with no
parallax at all those are nowhere near each other.

## What the map says about itself

Author, version, credits, licence, and the lines a server runs when it loads
the map. All of it goes through the same commands as everything else, which is
the one thing here worth saying: the editor in the client changes a map's own
description without an undo entry, and that is not copied - `info.setProp` and
`info.settings.add/set/delete` make versions like any other change.

A setting is one line. A line with a break in it would come back as two lines
and then the map would not be the map that was written, so it is refused.
There is no checking of what the line *says*: that would need the console's
own list of commands, which is the server's business and not the document's.

## The brush

The left button paints, held shift it takes a piece of the layer into the
brush instead, held alt it fills a rectangle with it, held control it rubs out;
the right and middle buttons move the map. What a rectangle gesture is about
is drawn on the map while the button is down. `X` and `Y` turn the brush over, `R` turns it a quarter, and the digits
are ten slots to put one away in (`Shift` and a digit stores).

The picture of the tiles comes from wherever the map keeps it. A picture that
lies beside the map is a PNG and the browser reads PNGs, so the page fetches it
and the program is not asked; a picture packed into the map file is already
unpacked in the program, and the page asks for the pixels
(`editor.imageData(index)`) and draws them itself. Either way the program never
draws a tileset - it draws maps.

In a tele, switch, speedup or tune layer the tileset comes with the numbers
that go beside a tile: which tele the tile sends to, which switch it belongs to
and how long it waits, how hard and which way a speedup pushes. They belong to
the brush rather than to a tile - a number is chosen and then tiles are put
down with it - and grabbing a piece of a layer brings back the numbers that
were on it, so carrying a piece of a map somewhere else carries them along.
What is air keeps none of them.

Beside the number field are the two things somebody does with a number: take
one that is free, and go and look at where this one already is. Which number
is free is a question about the layer and the program answers it - a tele
layer counts its checkpoints apart from its teleporters, and which of the two
is about to be put down is read off the brush. Going there moves the view to
the next place that number is used and says which of how many it is; tiles
closer together than ten count as one place, so a teleporter four tiles wide
is somewhere to go rather than four. Which place was last looked at is the
panel's to remember, because it is the panel that knows the button was pressed
twice.

A stroke is one change made of many stamps, which is why it costs what it
touched rather than what the map is: a hundred strokes of eight stamps each
take 72 ms on ctf1 and 104 ms on Tsunami, a map eight times its size, and both
add the same 1.83 MiB to the history. Taking all hundred back again is 23 ms
and 17 ms.

## What it is not, yet

Sounds are looked at but not changed, a quad's picture coordinates are read
but not yet edited, and a rules file can be run but not written - there is no
editor for the rules themselves. A layer with no picture at all is shown as a grid of numbers - the tiles are still there to
be picked, they just cannot be shown.
