#ifndef GAME_MAP_DOCUMENT_COMMAND_H
#define GAME_MAP_DOCUMENT_COMMAND_H

#include <string>

namespace map_document
{
	class CDocument;

	/**
	 * What the interface asks of the map, as JSON.
	 *
	 * Everything that changes the shape of a map - a group added, a layer
	 * moved, a parallax set - comes in through here as one object with an
	 * `op` in it. It is one entrance rather than one exported function per
	 * command because the list of commands is then in one place and can be
	 * held against the one the interface believes in; and because these are
	 * the cold path, a click rather than a movement. What happens while a
	 * pointer is being dragged goes through typed functions with numbers.
	 *
	 * Every command is a transaction of its own and therefore one entry in
	 * the history - unless one is already open, in which case it joins it.
	 * That is what a slider does: the interface opens a transaction when the
	 * dragging starts, sends a command per step, and closes it when the
	 * dragging stops. Each step is the preview; the entry is written once.
	 *
	 * The commands:
	 *
	 * | `op` | what it needs |
	 * |---|---|
	 * | `group.add` | - |
	 * | `group.delete` | `group` |
	 * | `group.move` | `group`, `to` |
	 * | `group.setProp` | `group`, `prop`, `value` |
	 * | `layer.add` | `group`, `type`, and for `tiles` a `kind`, `width`, `height` |
	 * | `layer.delete` | `group`, `layer` |
	 * | `layer.move` | `group`, `layer`, `toGroup`, `to` |
	 * | `layer.setProp` | `group`, `layer`, `prop`, `value` |
	 * | `envelope.add` | `name`, `channels` (1, 3 or 4) |
	 * | `envelope.delete` | `envelope` |
	 * | `envelope.setProp` | `envelope`, `prop`, `value` |
	 * | `envelope.point.add` | `envelope`, `time`, `values`, `curve` |
	 * | `envelope.point.delete` | `envelope`, `point` |
	 * | `envelope.point.set` | `envelope`, `point`, and what of `time`, `values`, `curve` is to change |
	 * | `history.undo` | - |
	 * | `history.redo` | - |
	 * | `history.jump` | `index` |
	 * | `tiles.read`, `tiles.write`, `tiles.fill`, `tiles.replace`, `tiles.find`, `tiles.stats` | see `tiles.h` |
	 *
	 * Anything may carry a `label`, which is what the history entry is
	 * called; every command has one it falls back on.
	 *
	 * An envelope point carries its time in whole milliseconds and its values
	 * in the map's own 22.10 fixed point, because that is what the file holds.
	 * What a value means is a question about the envelope's channels - a
	 * colour, a place, a volume - and belongs to whoever knows that.
	 *
	 * Anything may also carry a `merge`, which names what is being changed
	 * rather than what is being done: two changes carrying the same `merge`,
	 * close enough together, become one history entry. That is a number field
	 * being stepped with its arrows - ten steps are one change of one
	 * property, not ten things to undo. Without it nothing merges, which is
	 * what a brush stroke wants.
	 *
	 * @param Document The document to change.
	 * @param pJson The command, as a JSON object.
	 *
	 * @return A JSON object. `{"ok":true}` with whatever the command has to
	 * say - the index a new group was given, say - or `{"ok":false,"error":
	 * "..."}`, and then nothing was changed.
	 */
	std::string Apply(CDocument &Document, const char *pJson);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_COMMAND_H
