#ifndef GAME_MAP_DOCUMENT_REPORT_H
#define GAME_MAP_DOCUMENT_REPORT_H

#include <base/vmath.h>

#include <cstddef>
#include <string>

namespace map_document
{
	class CDocument;
	class CMapState;

	/**
	 * What the map is made of, as JSON, for the shadow copy the interface
	 * keeps of it.
	 *
	 * The interface draws its panels out of this and nothing else: a layer
	 * tree, the properties of whatever is selected, the list of images. What
	 * is *in* a layer is not here - a tile layer says how large it is, a quad
	 * layer how many quads it has, and the tiles and the quads themselves are
	 * asked for one at a time or drawn by the renderer. A map with four
	 * million tiles would otherwise be a JSON text of forty megabytes after
	 * every stroke.
	 *
	 * Envelope points are left out for the same reason - a long envelope has
	 * hundreds of them and nearly nothing asks for them - and are asked for
	 * one envelope at a time with `EnvelopeJson`.
	 *
	 * @param Map The version to describe.
	 *
	 * @return The JSON text, which is an object.
	 */
	std::string StructureJson(const CMapState &Map);

	/**
	 * The points of one envelope, for the panel that draws it.
	 *
	 * They go out as whole numbers and come back as whole numbers, because
	 * that is what they are: a time is whole milliseconds and a value is the
	 * map's own 22.10 fixed point. Nothing is turned into a fraction on the
	 * way, so nothing is lost on the way back - which is what an editor that
	 * writes the file again needs.
	 *
	 * What a value means is a question about the channels: four are a colour,
	 * three a place and a turn, one a volume. The panel knows that; the
	 * document holds what the file holds.
	 *
	 * @param Map The version to read.
	 * @param Index Which envelope of it.
	 *
	 * @return The JSON text, which is an object, or `null` where there is no
	 * such envelope.
	 */
	std::string EnvelopeJson(const CMapState &Map, size_t Index);

	/**
	 * The quads of one layer, for the panel that lists them and the pointer
	 * that drags their corners.
	 *
	 * Left out of `StructureJson` for the same reason the tiles are: a quad
	 * layer says how many it has, and this says what they are when somebody
	 * is working in one.
	 *
	 * Points come out in **world units** rather than the 22.10 fixed point
	 * the file keeps, because that is the only number a page can do anything
	 * with: it turns a click into a world place and back. What that costs is
	 * the tenth of a unit below the point, which is a thousandth of a tile -
	 * and a quad that is not touched is not written again, so nothing drifts
	 * from being looked at.
	 *
	 * @param Map The version to read.
	 * @param Group Which group.
	 * @param Layer Which layer of it, which has to be a quad layer.
	 *
	 * @return The JSON text, which is an array, or `null` for a layer that
	 * holds no quads.
	 */
	std::string QuadsJson(const CMapState &Map, size_t Group, size_t Layer);

	/**
	 * The sound sources of one sound layer, for the panel and the overlay
	 * that drags them.
	 *
	 * The same rule as the quads: a place comes out in **world units** and a
	 * size with it, because a page turns a click into a place and back. What
	 * a source is heard within is a rectangle or a circle, and which of the
	 * two it says in a word rather than in a number, because the number means
	 * nothing without the file format beside it.
	 *
	 * @param Map The version to read.
	 * @param Group Which group.
	 * @param Layer Which layer of it, which has to be a sound layer.
	 *
	 * @return The JSON text, which is an array, or `null` for a layer that
	 * holds no sounds.
	 */
	std::string SoundSourcesJson(const CMapState &Map, size_t Group, size_t Layer);

	/**
	 * Proof mode: the rectangles a player's screen would cover around a place.
	 *
	 * Twenty-one shapes from square to 16:9 give the outline of everything
	 * anybody could see; two of them are named, because 4:3 and 16:10 are the
	 * ones a mapper is told to check. In menu mode the zoom is the menu's
	 * 0.7 and the places the map names for a menu background come with it.
	 *
	 * The page draws all of it, because it is lines over a picture and that
	 * is what an overlay is for; what it gets is world units, the same as the
	 * sound sources.
	 *
	 * @param Map The version to read.
	 * @param Center Where the camera stands, in the game layer's coordinates.
	 * @param Menu Whether to answer for a menu background rather than a game.
	 *
	 * @return The JSON text, which is an object.
	 */
	std::string ProofJson(const CMapState &Map, vec2 Center, bool Menu);

	/**
	 * Everything a map may say to a server, with what it means and what it
	 * takes - see `map_document::KnownSettings`.
	 *
	 * The whole table at once, because it does not change while the editor
	 * runs and a list of a few dozen is cheaper to hand over once than to ask
	 * for at every keystroke.
	 *
	 * @return The JSON text, which is an array.
	 */
	std::string SettingsHelpJson();

	/**
	 * What is wrong with each line of a map's settings, and where each one
	 * repeats an earlier one.
	 *
	 * @param Map The version to read.
	 *
	 * @return The JSON text, which is an array with one entry per line.
	 */
	std::string SettingProblemsJson(const CMapState &Map);

	/**
	 * The history as the history panel shows it: what was done, when, where
	 * in it the map stands, and what it all costs.
	 *
	 * @param Document The document whose history to describe.
	 *
	 * @return The JSON text, which is an object.
	 */
	std::string HistoryJson(const CDocument &Document);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_REPORT_H
