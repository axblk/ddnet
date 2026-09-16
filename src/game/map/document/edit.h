#ifndef GAME_MAP_DOCUMENT_EDIT_H
#define GAME_MAP_DOCUMENT_EDIT_H

#include <base/dbg.h>
#include <base/vmath.h>

#include <game/map/document/document.h>
#include <game/map/document/layer.h>

#include <cstddef>
#include <utility>
#include <vector>

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
	 * Turns every tile of the brush that does nothing in its kind of layer
	 * into air, and says how many there were.
	 *
	 * What the native editor does unless "allow unused" is on: a game, front,
	 * tele, speedup, switch or tune layer only takes the tiles the game reads
	 * there. A tile it does not read is a tile that is in the file for nobody.
	 * A layer that is only drawn has no such thing, and its brush is left as
	 * it is.
	 *
	 * @param Brush The brush to clean.
	 *
	 * @return How many tiles were turned into air.
	 */
	int DropUnusedTiles(CBrush &Brush);

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
	 * The lowest number from 1 to 255 that no tile of this layer uses yet.
	 *
	 * What "uses" means is the layer's business: a tele layer has two counts
	 * that do not share their numbers - the checkpoints and everything else -
	 * and some tiles of a switch layer carry no number at all.
	 *
	 * @param Layer The layer to look through.
	 * @param Checkpoint For a tele layer, whether to count the checkpoints
	 * rather than the rest. Means nothing to the other kinds.
	 *
	 * @return The number, or -1 where all 255 are taken.
	 */
	int NextFreeNumber(const CTileLayer &Layer, bool Checkpoint = false);

	/**
	 * Every place a number is used, as tiles, one per cluster.
	 *
	 * The interface walks this to show somebody where a tele number goes:
	 * one number is usually a handful of places, and a teleporter that is
	 * four tiles wide is one of them rather than four. Which is why tiles
	 * closer than ten to the one before are left out - the same rule the
	 * editor in the client uses, only worked out at once instead of
	 * remembering where it was.
	 *
	 * @param Layer The layer to look through.
	 * @param Number The number to look for; 0 is no number and finds nothing.
	 *
	 * @return The places, in the order the layer is read, or empty.
	 */
	std::vector<ivec2> NumberPlaces(const CTileLayer &Layer, int Number);

	/**
	 * Which physics tile a layer's tiles are to be turned into.
	 *
	 * The thirteen the editor in the client offers under "construct", in the
	 * order it offers them: a design layer says where the walls are and this
	 * writes the game tiles under them, which is how a map is built - the
	 * shape is drawn once and the physics follow it.
	 */
	enum class EGameTile
	{
		AIR,
		HOOKABLE,
		DEATH,
		UNHOOKABLE,
		HOOKTHROUGH,
		FREEZE,
		UNFREEZE,
		DEEP_FREEZE,
		DEEP_UNFREEZE,
		BLUE_CHECK_TELE,
		RED_CHECK_TELE,
		LIVE_FREEZE,
		LIVE_UNFREEZE,
	};

	/**
	 * Whether this layer's tiles can be turned into game tiles at all.
	 *
	 * They can when the layer lies over the game layer tile for tile: it has
	 * to be a drawn layer rather than a physics one, its group must not move
	 * with the camera, and the group's offset has to be whole tiles. A layer
	 * in a parallax group is somewhere else at every moment, so there is no
	 * answer to where its tiles are.
	 *
	 * @param Map The map the layer is in.
	 * @param Group Which group.
	 * @param Layer Which layer of it.
	 */
	bool CanConstructGameTiles(const CMapState &Map, size_t Group, size_t Layer);

	/**
	 * Writes a physics tile under every tile this layer holds.
	 *
	 * Air in the design layer is left alone - this puts tiles under what is
	 * drawn, it does not clear what is not. The game layer grows if the design
	 * layer reaches past it, and because the physics layers of a map are all
	 * one size, they all grow with it.
	 *
	 * The two checkpoints are the exception that is not one: they are tele
	 * tiles, so they go into the tele layer, with the number 1 - and if the
	 * map has no tele layer, it gets one.
	 *
	 * @param Doc The document being changed.
	 * @param Group Which group the design layer is in.
	 * @param Layer Which layer of it.
	 * @param Tile Which physics tile to write.
	 *
	 * @return How many tiles were written, which is 0 for a layer that is all
	 * air - and then the version is no version, the way every other change
	 * that changed nothing is none.
	 */
	int ConstructGameTiles(CDocument &Doc, size_t Group, size_t Layer, EGameTile Tile);

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
