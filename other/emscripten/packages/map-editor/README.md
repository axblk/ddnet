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

## The whole thing in one element

A page that wants an editor rather than the pieces of one takes
`<ddnet-editor>`. It makes the program, the canvas, the shapes over it and the
panels, and lays them out in six areas around the map:

```html
<link rel="stylesheet" href="ddnet-editor.css">
<script type="module">import "@ddnet/map-editor";</script>

<ddnet-editor src="maps/ctf1.map" style="height: 100dvh">
	<div slot="header">Whatever the page wants above the map</div>
</ddnet-editor>
```

The areas are `header`, `toolbar`, `left`, `right`, `dock` and `status`; an
area nobody fills takes no room at all. What the editor puts in them:

| Area | What stands there |
|---|---|
| `toolbar` | Undo, redo, what is drawn, the grid, proof mode, saving. |
| `left` | The map's parts, one tab at a time: the layer tree, the pictures, the sounds, the map's own fields. |
| `right` | The inspector - what the selected thing is made of, and the tileset to paint with. The tile panel has two tabs of its own: the tileset, and the rules that paint by themselves. |
| `dock` | The strip along the bottom: envelopes, the history, the server settings, the rules file. It starts shut, because what is in it is looked at now and then and the map should not pay two hundred pixels for it the whole time. |
| `status` | One line: what is under the pointer, and what just happened. |

`box.panels.showTab("dock", "envelopes")` puts one of them in front and opens
the strip if it was shut. What the element fills in itself - the
map and the panels - is light DOM as well, so the same one stylesheet dresses
all of it and a page may reach any of it.

| Attribute | What it does |
|---|---|
| `src` | A map to open, and changing it opens another. |
| `urlparam` | The name of a parameter of the page's *own* address to take a map from - `urlparam="map"` reads `#map=…`. Left out, the element does not read the address at all, because two editors on one page could not both be what it is about. |
| `theme="light"` | The light set of colours. |
| `remember` | Keeps what is being edited in the browser's own storage, saves into it every minute, and asks before the tab goes with something unsaved in it. Without it the element keeps nothing - a page that quietly filled a visitor's storage would be a surprise. |
| `controls="none\|compact\|full"` | How much tool bar. Left out, the size of the box decides. |
| `readonly` | Nothing may be changed: no inspector, no brush, and the tool bar keeps only what is about looking. The pointer pans and zooms and paints nothing. |
| `targets="auto\|big\|small"` | How big the things one aims at are. Left out (or `auto`), the browser is asked: `pointer: coarse` gets the finger sizes. The other two are for the cases where that answer is wrong - a touch laptop with a mouse says `fine`, a tablet in desktop mode says `coarse`, and neither is what the hand on it is doing. |

`box.ready` is a promise for the running program, `box.editor` and
`box.panels` are it and its panels once there are any, and `box.part("tree")`
finds one of *this* editor's parts by name. That last one is what lets two
editors stand on one page: `data-role` names belong to the element, not to the
document.

The keyboard works the same way. An editor answers a key when the focus is
inside it; with the focus nowhere at all it answers only when it is the one
editor on the page, because with two there would be no way to say which was
meant.

### The shape it takes

The editor answers to the size of its **box**, not of the window: an editor in
an 800-pixel hole in a wide page is a narrow editor. `box.layout` says what it
decided and the element says the same thing in `data-` attributes, which is
what the stylesheet reads - the widths are written down once, in `BOX_WIDTHS`
in `map-editor.js`, because the same numbers also decide what the panels *do*.

| Box width | Left | Right | Tool bar |
|---|---|---|---|
| < 600 | sheet from the floor | sheet | six buttons |
| 600–899 | drawer | drawer | icons |
| 900–1199 | drawer | column | icons |
| 1200 and up | column | column | icons with the modes' names |

| Box height | Above the map | Status | Dock |
|---|---|---|---|
| < 600 | one row: the page's header goes, the tools stay | a chip in the corner of the map | over the map's foot |
| 600 and up | two rows | a line of its own | a strip beside the map |

A drawer is the same box of panels in the same place in the grid, laid *over*
the map rather than beside it; it starts shut, and a press on the map shuts an
open one without painting - the hand that reached past the drawer was reaching
for its edge, not for the tile behind it.

### With room to spare

Past 2560 pixels wide *or* 1400 tall, nothing needs to hide behind a tab any
more. The columns go to 320 and 400, the layer tree and the pictures stand
above each other instead of behind each other, the inspector shows the tileset
*and* the rules it paints by, and the dock is open with the envelope curve
beside the history. At 3840x2160 the tree, the pictures, the properties, the
tileset, the automapper, the curve and the history are all there at once, and
the map is still 3120 pixels wide.

What does **not** grow is anything one reads or hits. Forty inches of 3840
pixels is a hundred and ten dots per inch - the same as twenty inches of 1920 -
so twelve-pixel text is already the right size on the glass; making it bigger
would only mean less map. What the room buys is the *number* of things that
can be open, not the size of any of them.

Three things do move, because on a monitor eighty centimetres wide the middle
is not where one is looking: the tile chooser opens under the pointer rather
than in the middle (fitted back inside the edges if it would hang over one),
the coordinate under the pointer is shown a second time at the top of the
inspector - the line at the bottom is for the eye, that one is for the hand -
and the notes over the map move from the top right corner to the bottom
middle, because that corner is the far end of the desk.

### What just happened, and how far away

What the editor says about what it just did is said twice: in the line along
the bottom, where one looks for it afterwards, and as a note over the map,
which one sees without looking. A note goes by itself after four seconds and
never more than four stand at once; something that went *wrong* stays, marked
down its edge, until it is dismissed - a mistake that vanished before it was
read is a mistake nobody knows about. If the four are full it is the oldest
plain note that gives way, never the error.

The corner of the map says what the zoom is and is three buttons: further
away, back into the picture, closer. Only the number shows for a pointer,
which has a wheel and is quicker with it; a finger gets the minus and the plus
at forty-four pixels each.

Between each column and the map there is a handle. Dragging it sets the width
(240 to 480 on the left, 288 to 560 on the right, 160 to 640 for the dock),
the arrow keys move it sixteen pixels at a time, and Home or a double press
gives it back to the stylesheet. A side that is a drawer has no edge to drag
and does not show one.

### One editor, several maps

The strip at the top holds every map that is open: its name, a dot while it is
not saved, and a cross. The program has always kept several; what it did not
have was a way to see them.

Coming back to a map brings back what the *page* knew about it as well - what
was picked, which tabs were open, which groups were folded up, which mode the
brush was in. The program keeps the maps and the history; the rest is the
page's, and it is what makes coming back feel like coming back rather than
like opening the map again. The Map tab says what each open map is costing in
memory, because a second map open is a second map's worth of history and
history is whole versions of a map.

### With a finger

One surface, not two: what changes is the *input*, read from
`(pointer: coarse)` and from each event's `pointerType`, and it changes sizes
and adds buttons - never what the editor can do.

| Gesture | What it does |
|---|---|
| One finger | the layer's tool: paint, grab, fill, rub out, drag a handle |
| Two fingers | pan and zoom about the middle of them; the angle is ignored |
| Two-finger tap / three-finger tap | back / forward |
| A press that stands still, with an empty brush | which layer is here? |
| A press that stands still, with a full brush | nothing - a finger may rest while it paints |
| A pen | paints always; once a pen has been seen a finger pans instead, because the hand holding the pen lies on the glass |

The second finger of a pan lands fifty to a hundred and fifty milliseconds
after the first, and by then the first has already put down a tile. Within
150 ms, and while the first finger has gone less than 8 pixels, the second one
says the first was never a stroke: it is thrown away - no tile, no history
entry - and the two of them are a pan. After that the stroke is settled and a
late finger is ignored, because a hand resting on the glass beside a drawing
one is not a gesture.

Three of the tool bar's buttons exist only for a finger, because on a desk
they are keys nobody can press without a keyboard: **nothing in hand** (which
is Escape, and an empty brush is what grabs), **the big tile chooser** (which
is holding space, and nothing can be held), and **which layer is here** (which
is Ctrl and the right button, and a finger has neither). Every row that has a
menu shows a `...` for it; at a desk that button waits for the pointer to come
near.

The tileset in the inspector is nineteen pixels a tile - a picture of what is
in hand, not a thing a finger can hit - so with a finger a touch on it opens
the big chooser, where a tile is forty-four pixels or more.

A side that is a drawer is pulled out of the edge it sleeps behind: a finger
that starts in the twenty pixels along that edge and travels forty inwards
opens it, and that press never paints. Twenty pixels is narrow on purpose -
a stroke begins with the finger on the map, not on its edge.

A `title` is a pointer's affordance: it appears because the mouse rested
there, and a finger never rests anywhere without pressing. So a long press on
a button says the same words the pointer would have been shown. Shortcuts are
left out of those words until a key has actually been struck - `Ctrl+Z` beside
a name is a hint on a laptop and noise on an iPad - and from the first
keydown on they are back, in the tooltips and in the palette both.

Which sizes are used is the browser's answer, and `targets` on the element
overrules it where that answer is wrong. The command is **Big targets**
(Ctrl+Alt+T), which goes round the three: as the browser says, on, off.

## One list of everything it can do

The tool bar is not a row of buttons somebody wrote out; it is what the list of
commands says wants one. The keyboard is a lookup in the same list, the
tooltips take their key from it, and the menu, the palette and the menu of a
layer are built out of it. A new thing the editor can do is an entry in
`commands.js`, not a new button:

```js
{
	id: "view.grid", label: "A grid on the tiles", group: "View",
	icon: "grid", bar: true, keys: ["G", "Ctrl+G"],
	pressed: p => p.editor.grid() > 0,
	run: p => { p.editor.grid(p.editor.grid() > 0 ? 0 : 10); p.refreshBar(); },
}
```

`panels.run("view.grid")` does one by name, and answers whether it could be
done at all - a command that says `enabled` is false is not done and its button
is grey. A hundred and twelve commands answer to ninety-two keys today.

Four more fields say where else a command shows up:

| Field | What it does |
|---|---|
| `bar` | a button on the tool bar, with `icon` |
| `menu` | a row in the menu; `Layer/Add a layer` is a row that opens onto more |
| `for` | the kind of thing whose own menu it belongs in - `"layer"`, `"image"`, … |
| `palette: false` | kept out of the palette; the ten brush slots are all there is |

`part` names the button in a panel that a command presses, for the commands
that are a button and nothing else. The tool bar holds only what the plan calls
often used - the four brush modes, undo and redo, saving, the six view
switches, the palette and the menu - and everything else is reached by name.

Six of the native editor's keys cannot be had in a browser - Chrome keeps them
whatever a page does - so they are said differently here:

| There | Here | Why |
|---|---|---|
| Ctrl+N (new map) | Ctrl+Alt+N | Ctrl+N opens a window |
| Ctrl+L (load) | Ctrl+O | Ctrl+L is the address bar |
| Ctrl+T (the physics numbers) | T | Ctrl+T opens a tab |
| Ctrl+Q (add a quad) | Q | Ctrl+Q quits, on Linux |
| Ctrl+W, Ctrl+F4 (close the map) | Ctrl+Alt+W | both close the tab |
| Ctrl+Shift+I (hex tile info) | Ctrl+I | Ctrl+Shift+I opens the developer tools |

## Everything by its name, and the menus

**Ctrl+P** opens the palette: every command there is, filtered by what is
typed. What is called exactly that comes first, then what starts with it, then
what has a word starting with it, then what merely holds it somewhere. Arrows
walk it, Enter takes one, Escape gives up. A command that cannot be done right
now is still listed, greyed: knowing that the editor can do a thing at all is
most of what a palette is for.

**Alt+M**, or the ☰ button, opens the menu - File, Edit, View, Layer, Tools,
Settings, Help, in that order, out of the same list.

**A right-click** on a row of the tree, of the pictures, of the sounds, of the
quads, of the sources or of the settings opens the menu of that thing: what can
be *done* to it, and nothing about what it *is* - properties live in the
inspector, and a property with two homes is a property that disagrees with
itself. A tile layer that lies over the game layer also gets the thirteen
physics tiles as a submenu, which is the native editor's
"Game tiles from this layer".

Two things ask before they happen, because they cannot be undone into shape:
**New map** wants a name and a size, and **Save as** wants a name. Everything
else the editor does happens and can be undone.

All of that floats in a layer of its own over the six areas, so that a menu
opened from the tree is not cut off by the edge of the column the tree stands
in.

### Files, and the tools that work on a whole map

**Open from this browser** lists what lies in the browser's own storage -
what autosave wrote and what every Save writes on its way to the downloads -
with the size of each, because a name alone says nothing about which of two
maps it is. The list comes from the program: the files are in its file
system, and a page cannot look into that. **Save a copy** writes the map
under another name and leaves the map one is working on as it was: its
name, and the dot that says it has unsaved changes. Closing a map that has
such changes asks first, in the editor's own dialogue rather than
`confirm()`, which would stop the page - and the map being drawn - dead.

**A border round the layer** stamps what is in hand along the four edges of
the selected tile layer, stepping by the brush's size, as one entry in the
history. **Take out unused envelopes** asks the program which envelopes
nothing is bound to - a layer, a quad or a sound source - and takes all of
them out in one step; the bindings above them come down with them.

**What the keys do** (Ctrl+/) is every shortcut on one sheet, grouped the way
the palette groups them, and every key a command answers to rather than only
the first.

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

And the file itself can be written. The `Rules` panel holds it as text with
its words coloured - configurations, the words that begin a line, the words
that stand inside one, and the numbers - and `apply` hands the text as it
stands back to the program. What the program could not use it says by line
number, and those lines are underlined where they stand: a rules file is read
as far as it is understood and the rest is passed over, which is what lets a
file from a newer editor still automap, but somebody writing one wants to be
told. `revert` fetches the file again and `save` writes the text out so that
it can be put where the game looks for it. A rules file is not part of the
map, so none of this is in the history and none of it makes the map unsaved.

A textarea cannot colour its own words, so what is coloured is a `<pre>`
behind it holding the same ones and the textarea above is transparent but for
its caret. The two therefore have to agree about every measurement that moves
text - font, padding, line height, wrapping - and the scrolling of the one is
copied onto the other.

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

## What a tile is, and what it does

The bar says what is under the pointer: where it is, which tile stands there
in decimal and in hex, and what that tile does. Hex because the entities sheet
is sixteen wide, so `0x23` names row 2 and column 3 in one go; decimal because
that is how mappers talk about tiles.

The sentence is the one the editor in the client shows. Those sentences moved
out of `game/editor/` into `game/` for this - they were always only a table
about `game/mapitems.h`, and somebody who learned what a tile does in one
editor should not be told something else in the other.

Which number a tile *is* turns out to be a question the file format answers
rather than the layer: the three kinds that draw their own tiles keep it where
it is drawn from, and a physics layer draws nothing, so its plane of tiles is
air and the meaning sits beside it in the extra plane.
`map_document::TileMeaning` is the one place that knows which, so
`editor.tileIndex` on a tele layer gives 26 rather than the 0 it draws.

## Proof mode

The one question an editor cannot answer by looking: is this still on screen
for everybody? A map is edited at whatever zoom and in whatever window somebody
happens to have; it is played on a screen whose shape nobody chose, at a zoom
the game decides. Proof mode draws that screen over the map - twenty-one
shapes from square to 16:9, which together are the outline of everything
anybody could see, with 4:3 and 16:10 named and drawn on top because those are
the two a mapper is told to check.

The arithmetic is the game's own (`CalcViewSize`), so the rectangle here and
the view a client ends up with are the same rectangle rather than two guesses
about it. The document answers it (`map_document::ProofScreen`, `ProofJson`)
and the page draws it in the same SVG the sound sources use, because it is
lines over a picture and that is what an overlay is for.

It stays in the middle of the screen and the map moves under it, the same as
in the editor in the client: the question is what a player standing *here*
would see, and where here is, is where the view is looking. Pressing the
button - or `P` - once gives a game, twice gives a menu background at the
menu's own 0.7 zoom, three times turns it off. In menu mode the places this
map names for a menu background come with it: a map says so with time
checkpoint tiles in its game layer, tile 35 being the first place. Places the
map does not name are not shown, because where those stand is the client's
business and the document does not know the client.

## Sound sources

A sound layer is not seen, so the shapes its sources are heard within are
drawn by the page rather than by the program: an SVG over the canvas, which is
what an SVG is for - a few shapes that stand over a picture and are told where
to stand. Where that is comes from the program (`editor.groupPixelAt`), the
other way round from what a pointer asks, so the shapes and the map cannot
drift apart. The SVG lets every click through: taking hold of a source is the
canvas pointer's job, the same as a quad's corner.

Where a source is, is dragged; what it is, is fields. A source is heard within
a circle or within a rectangle, and which of the two decides which fields it
has - a circle has a radius and no sides. Changing from one to the other
brings a size along rather than keeping whatever stood in the same place in
the file's union of the two.

Panning and zooming do not change the map, so the panels are left alone; what
they do change is where an overlay belongs, which is why steering the canvas
says `onView` as well as `onChange`. Rebuilding every panel on every pixel of
a drag would be work nobody asked for.

## Sound files

A sound in a map is an Opus file and the program never looks into it: it takes
the bytes, keeps them, and hands them back. There is no decoder in it and
there does not need to be one, because the one thing that has to play a map's
sound is a browser, and every browser decodes Opus. `editor.addSound(name,
bytes)` puts one in, `editor.soundData(index)` takes the bytes out again
unchanged, and the panel wraps those bytes in a `Blob` for an `<audio>`
element. A sound that lies beside the map is played from `mapres/<name>.opus`
instead, the same place a picture beside the map comes from.

The bytes go the way a picture's pixels go - through their own C entrance,
not through JSON - while the structure around them goes through `apply`:
`sound.add` names one that lies beside the map, `sound.setProp` renames it or
takes its bytes out, and `sound.delete` takes it away. Taking one away takes
it off the layers that played it, the same arithmetic as a picture or an
envelope: what pointed past it comes down one, what pointed at it points at
nothing. Going the other way - beside the map back into it - is not something
a command can do, because the bytes are not in the command.

## Typing with tiles

A font tileset is a tileset like any other; what makes it a font is that `A` is
at tile 1 and `1` is at tile 54, which is a convention of the sheets people
draw rather than anything the file format knows. Letters and digits become
those tiles, a space becomes nothing, and a newline goes down a row and back to
the column it started in - so a block of text stays a block, and a line that
reaches the right-hand edge wraps the same way. Anything else is passed over: a
font tileset has 26 letters and ten digits, and refusing a comma would be
refusing the sentence it stands in.

The editor in the client does this a keystroke at a time in a mode of its own.
A page has text fields, so here it is a text and one history entry - which is
also the only version that can be undone in one go. It is written where the
view is looking, because that is where somebody is when they decide to write
something.

## A picture, turned into map

Two ways of doing it, and which one is wanted is a question about the picture
rather than about the map, so both are offered and neither is the default.

**As tiles**, the picture becomes its own tileset: every colour in it gets a
tile of that colour on a 16-by-16 sheet, and the layer is those tiles. Tile 0
is nothing, which is what a pixel that is not opaque becomes. A picture of more
than 255 colours needs more than one sheet and gets a layer for each; together
they are the picture, one layer over the next, and no pixel is drawn twice. The
colours are sorted rather than taken as found, so the same picture always gives
the same palette and a map made twice is the same map.

**As quads**, each pixel becomes a quad of one colour. A run of one colour
becomes *one* quad - grow right as far as the colour holds, then down as far as
whole rows of it hold - which makes a flat picture cheap and leaves a
photograph exactly as dear as it was. The group clips to what was drawn, so a
picture put on a map stays where it was put. `pixelStep` reads every second or
fourth pixel, `quadSize` says how big one is on the map, and `centralize` puts
every pivot in the same place, which is what an envelope wants: one envelope
then turns the whole picture rather than every pixel on the spot.

Both read pixels rather than a file, the same as `addImage`: a browser decodes
a PNG and the map already keeps RGBA, so a decoder in the program would be a
second one. Both ask before doing something expensive - `artColors` says how
many palettes a picture would need, and a quad count above a few thousand is
put to the user first, because a quad per pixel of a photograph is not
something anybody means to ask for.

## Appending a map

Another map's groups go into this one: everything it draws, the pictures,
sounds and envelopes it draws with, and the lines it asks of a server. Not its
game layer - physics belongs to the map being worked on, and two game layers
is not a map.

Everything a layer names, it names by its place, so every place in the map
coming in is read again against where it ends up. Pictures are the awkward
one. A picture with the same name *and* the same bytes is the same picture and
is not brought over twice; one whose name is taken by a different picture is
renamed to `name (1)` rather than dropped, because dropping it would change
what the map looks like. A settings line already there is already there.

All of it is one history entry, however much came over, and what came over is
said in the status line as counts - appending moves numbers about everywhere
at once, and a count is the only honest summary of that.

## Quads

A quad is four corners and a pivot, and all five are dragged on the map rather
than typed into a field: the panel lists the quads by where their pivot sits,
picking one puts handles on it, and the program draws those handles because a
quad lies in its group's coordinates - parallax and all - and only the program
knows where that is on the screen. Dragging a corner moves that corner;
dragging the pivot carries all five, which is how a quad is moved without
changing its shape. Either way the whole drag is one history entry.

What is not a point is a field: a colour on each of the four corners with its
alpha beside it, where that corner sits in the picture, and which envelopes
move and colour the quad. A binding names an envelope by its place, so one the
map does not have is refused rather than written - a map that reads back
differently than it was written is not a saved map.

The two picture fields per corner are in the numbers the map file holds, which
are also the numbers the editor in the client shows: 1024 is the whole picture
across, so 0 and 1024 are its two edges and 3072 is three pictures along. A
fraction would have been friendlier to read and would have thrown away what a
quad that repeats its picture forty times holds.

Above the fields stand the four ways a quad is put in order rather than
dragged into it: **square** makes it the rectangle its corners span, **aspect**
keeps its width and takes its height from the proportions of the picture,
**pivot** puts the pivot in the middle, and **align** moves every corner - and
the pivot with them - onto the nearest tile. A quad dragged by four corners is
almost never the rectangle somebody meant. The editor in the client leaves the
first corner alone when it aligns; that is a slip rather than a rule, and it is
not copied.

The points go out in world units (`editor.quads(group, layer)`), because that
is the only number a page can do anything with - it turns a click into a place
and back - and the pointer asks the program where a click lands in the group
(`editor.groupWorldAt(group, x, y)`) rather than working it out itself, so
what is drawn and what is caught cannot drift apart. A new quad appears in the
middle of the view of *its group*, not of the plain view: in a group with no
parallax at all those are nowhere near each other.

The knife cuts a piece out of a quad. Four clicks inside it make a new quad of
those four places; the one that was cut from is left alone, which is what the
editor in the client does too - a knife here adds rather than divides.

What the piece keeps is what it was cut from. Each of the four places is
written as a mixture of three of the old quad's corners, in the proportion of
the three triangles the place makes with them, and the colour and the place in
the picture come out of that same mixture - so a piece cut out of a wall still
shows the part of the wall it sits over. A quarter of the way in on the map is
a quarter of the way into the picture.

The four places come in as a ring, because that is how somebody clicks them,
and the file keeps corners as two rows; a ring that folds over itself is
unfolded rather than refused. A ring clicked the other way round gives the same
piece: which corner is which differs, and it does not matter, because what each
corner shows comes from where it sits.

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

The lines a server runs when it loads the map are checked while they are being
typed. Nothing checks them today until a server refuses to start, which is the
wrong moment to find out that a variable is spelled wrong.

What a server would accept is not written out again here: it is every config
variable carrying `CFGFLAG_GAME`, taken from `config_variables.h` itself so
that the two cannot fall out of step, plus the six commands a map may use,
which are not variables and so are not in that file. Twenty-nine things in
all, each with the sentence the config already carries - which is what the
line under the list says once a name is settled.

Three things are said about a line: a name a server has never heard of, an
argument that is the wrong shape or outside its range, and a line that says
the same thing as an earlier one. The third is the interesting one, because
"the same thing" is not "the same text": `sv_deepfly 0` twice is a mistake,
and `tune_zone 1 ...` beside `tune_zone 2 ...` is not, so each command says
how many of its arguments tell two of them apart.

None of it refuses anything. A line on its way to being right is wrong for
most of the time it is being written, and an editor that would not let that
happen is an editor nobody can type in - so it is marked and explained, and
that is all. The names that begin with what stands there go into a `datalist`,
which is the one piece of completion a page gets for free and the one that
already behaves the way everybody expects.

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

Nothing of the plan is missing any more. What is not here is what the plan
says is not here: editing together, testing a map in the browser, and a new
file format. A layer with no picture at all is shown as a grid of numbers - the
tiles are still there to be picked, they just cannot be shown.
