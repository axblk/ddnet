#ifndef GAME_MAP_DOCUMENT_EDIT_H
#define GAME_MAP_DOCUMENT_EDIT_H

#include <base/dbg.h>

#include <game/map/document/document.h>
#include <game/map/document/layer.h>

#include <cstddef>
#include <utility>

namespace map_document
{
	/**
	 * What a brush is: a tile layer that is not in a map.
	 *
	 * A selection from the tileset, a piece grabbed out of a layer and the
	 * single tile of a pencil are all the same thing - a small layer that is
	 * stamped into a large one. Making it a layer rather than a type of its
	 * own means a brush holds what a layer holds, the physics planes
	 * included, and that the tool that grabbed it is the tool that stamps it.
	 */
	using CBrush = CTileLayer;

	/**
	 * Whether a layer of this kind draws the tiles it holds in `m_Tiles`.
	 *
	 * The three that do are the ones whose tiles are in the file the way they
	 * are drawn. The physics layers keep a plane of air there and say what
	 * they mean in their second plane, so painting one writes there - see
	 * `DocumentLayerSource` for the other half of the same rule.
	 */
	inline bool DrawsOwnTiles(ETileLayerKind Kind)
	{
		return Kind == ETileLayerKind::TILES || Kind == ETileLayerKind::GAME || Kind == ETileLayerKind::FRONT;
	}

	/**
	 * Whether a brush can be stamped into a layer of this kind at all.
	 *
	 * The three plain kinds take each other's tiles, because all three hold
	 * the same thing. A physics layer takes only a brush of its own kind:
	 * what a tile index means in a tele layer is a tele type with a number
	 * beside it, and inventing that number is the tool's business rather
	 * than the document's.
	 */
	bool CanStamp(ETileLayerKind Layer, ETileLayerKind Brush);

	/**
	 * Takes a rectangle out of a layer as a brush, clipped to the layer.
	 *
	 * The brush comes out with the kind of the layer and both of its planes,
	 * so grabbing a piece of a switch layer and stamping it back puts the
	 * same numbers and delays back.
	 */
	CBrush GrabTiles(const CTileLayer &Layer, int x, int y, int Width, int Height);

	/**
	 * Stamps the brush once, with its top left corner at `x`, `y`. Whatever
	 * falls outside the layer is left off.
	 */
	void StampTiles(CTileLayer &Layer, int x, int y, const CBrush &Brush);

	/**
	 * Repeats the brush over a rectangle, starting from its top left corner -
	 * what the editor calls filling a selection.
	 */
	void FillTiles(CTileLayer &Layer, int x, int y, int Width, int Height, const CBrush &Brush);

	/** Puts air back in a rectangle, in whichever plane the layer draws. */
	void EraseTiles(CTileLayer &Layer, int x, int y, int Width, int Height);

	/**
	 * Mirrors a brush, left to right and top to bottom.
	 *
	 * The tiles change places and so do their flags: a tile that is drawn
	 * turned keeps being drawn turned the other way, and a speedup that
	 * pushes to the right pushes to the left. A tile that cannot be turned at
	 * all - an entity in a game, front or switch layer - loses its flags
	 * rather than being drawn as something it is not, which is what the
	 * editor does today.
	 */
	void FlipBrushX(CBrush &Brush);
	void FlipBrushY(CBrush &Brush);

	/**
	 * Turns a brush a quarter turn clockwise, which swaps its width and its
	 * height.
	 */
	void RotateBrush(CBrush &Brush);

	/**
	 * What goes beside a physics tile: which of them it means.
	 *
	 * A tile index in a tele layer says what the tile *does* - send, check,
	 * a start, an exit - and this says to which target. The same for a
	 * switch's group and how long it waits, and for how hard and which way a
	 * speedup pushes. It is one set for a whole brush rather than one per
	 * tile, because that is how somebody places them: a number is chosen and
	 * then tiles are put down with it.
	 *
	 * Which of these mean anything depends on the kind of layer, and the
	 * ones that do not are left where they are.
	 */
	class CBrushNumbers
	{
	public:
		/** A tele's target, a switch's group, a tune zone. 0 to 255. */
		int m_Number = 0;
		/** How long a switch waits, in seconds. 0 to 255. */
		int m_Delay = 0;
		/** How hard a speedup pushes, and how fast it may get. 0 to 255. */
		int m_Force = 0;
		int m_MaxSpeed = 0;
		/** Which way a speedup pushes, in degrees. */
		int m_Angle = 0;
	};

	/**
	 * Writes those numbers onto every tile of the brush that is not air.
	 *
	 * Air keeps none of them: a number on a tile that does nothing would be
	 * written into the file and read back as a tile that does nothing with a
	 * number. A brush of a kind that has no numbers is left alone.
	 *
	 * @param Brush The brush to write on.
	 * @param Numbers What to write.
	 */
	void SetBrushNumbers(CBrush &Brush, const CBrushNumbers &Numbers);

	/**
	 * The numbers the brush is carrying, read off its first tile that is not
	 * air - for an interface that has just grabbed a piece of a layer and
	 * wants to show what came with it.
	 *
	 * @param Brush The brush to read.
	 *
	 * @return What it carries, or all zeroes where it carries nothing.
	 */
	CBrushNumbers BrushNumbers(const CBrush &Brush);

	/**
	 * Changes one tile layer of the version being made.
	 *
	 * This is the three steps of `CMapState` as one, for the case that is
	 * nearly all of them: the layer is copied - pointers only - handed to the
	 * caller to write to, and put back in place of the one that was there.
	 * Only inside a transaction, because there is no other way to change a
	 * map.
	 */
	template<typename FChange>
	void EditTileLayer(CDocument &Doc, size_t Group, size_t Layer, FChange &&Change)
	{
		CTileLayer Changed = *Doc.Edit().TileLayer(Group, Layer);
		Change(Changed);
		Doc.Edit().ReplaceLayer(Group, Layer, std::move(Changed));
	}

	/**
	 * One stamp of a stroke.
	 *
	 * A stroke is one transaction and many of these: the tool opens it when
	 * the button goes down, paints on every move, and closes it when the
	 * button comes up - one entry in the history, however many tiles it
	 * touched.
	 */
	void PaintTiles(CDocument &Doc, size_t Group, size_t Layer, int x, int y, const CBrush &Brush);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_EDIT_H
