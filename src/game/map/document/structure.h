#ifndef GAME_MAP_DOCUMENT_STRUCTURE_H
#define GAME_MAP_DOCUMENT_STRUCTURE_H

#include <game/map/document/document.h>
#include <game/map/document/layer.h>
#include <game/map/document/map_state.h>

#include <cstddef>
#include <optional>
#include <utility>

namespace map_document
{
	/**
	 * Which layer, of all of them: a group and a place in it.
	 *
	 * Layers are named by where they are rather than by a number of their own,
	 * because that is what the file holds and what a panel shows. It also means
	 * that moving a layer changes its name, which is why everything that moves
	 * one says where it ended up.
	 */
	class CLayerAddress
	{
	public:
		size_t m_Group = 0;
		size_t m_Layer = 0;

		bool operator==(const CLayerAddress &Other) const { return m_Group == Other.m_Group && m_Layer == Other.m_Layer; }
		bool operator!=(const CLayerAddress &Other) const { return !(*this == Other); }
	};

	/** Whether this is a layer the game reads rather than one that is drawn. */
	bool IsPhysicsLayer(const CLayer &Layer);

	/**
	 * Where the game layer is, if the map has one.
	 *
	 * A map is played by its game layer, and the group it is in is the one the
	 * other physics layers belong to - that is the rule the editor keeps and
	 * the one a tool needs when it is asked for a tele layer where there is
	 * none yet.
	 */
	std::optional<CLayerAddress> FindGameLayer(const CMapState &Map);

	/**
	 * Adds a group at the end, and says where it went.
	 *
	 * Like everything here, only inside a transaction - the document has no
	 * other way to change.
	 */
	size_t AddGroup(CDocument &Doc, CGroup Group);

	/** Takes a group out, and every layer in it with it. */
	void DeleteGroup(CDocument &Doc, size_t Group);

	/**
	 * Puts a group somewhere else in the order, which is the order it is drawn
	 * in.
	 *
	 * @param Doc The document being changed.
	 * @param From Which group is being moved.
	 * @param To Where it goes, counted in the list it leaves behind - so
	 * moving the first group to the end is `NumGroups() - 1`, not
	 * `NumGroups()`.
	 *
	 * @return Where it ended up.
	 */
	size_t MoveGroup(CDocument &Doc, size_t From, size_t To);

	/**
	 * Gives a tile layer another size.
	 *
	 * What is still on the layer stays where it is and what falls outside is
	 * gone; the price is the blocks the new edge cuts through rather than the
	 * blocks the layer holds - see `CTileStore::Resize`.
	 *
	 * The physics layers of a map are all the size of its game layer, because
	 * that is the size the game plays: a tele layer that is wider than the
	 * game layer has tiles nobody can stand on, and one that is narrower is a
	 * map whose right-hand edge teleports nobody. So resizing any of them
	 * resizes all of them, and a plain drawn layer is resized by itself.
	 *
	 * @param Doc The document being changed.
	 * @param Layer Which layer, which has to be a tile layer.
	 * @param Width How wide it is to be, at least one tile.
	 * @param Height How tall.
	 */
	void ResizeLayer(CDocument &Doc, const CLayerAddress &Layer, int Width, int Height);

	/** Adds a layer at the end of a group, and says where it went. */
	CLayerAddress AddLayer(CDocument &Doc, size_t Group, CLayer Layer);

	/** Takes a layer out of its group. */
	void DeleteLayer(CDocument &Doc, const CLayerAddress &Layer);

	/**
	 * Moves a layer, inside its group or into another one.
	 *
	 * @param Doc The document being changed.
	 * @param From Which layer is being moved.
	 * @param To Where it goes, counted in the map it leaves behind: the layer
	 * is taken out first, and the place is a place in what is left. A layer
	 * dragged to the end of its own group is therefore the last index of that
	 * group, one less than it has layers.
	 *
	 * @return Where it ended up.
	 */
	CLayerAddress MoveLayer(CDocument &Doc, const CLayerAddress &From, const CLayerAddress &To);

	/**
	 * A square quad of that size around that place, in world units - what
	 * "add a quad" means before anybody has dragged a corner of it.
	 *
	 * Its four corners are the order the file keeps them in: top left, top
	 * right, bottom left, bottom right, and the fifth point is the pivot it
	 * turns about. It is drawn white and takes the whole of its picture.
	 *
	 * @param CenterX Where the middle of it goes, in world units.
	 * @param CenterY The same, downwards.
	 * @param Width How wide it is, in world units.
	 * @param Height How tall it is.
	 *
	 * @return The quad, which is not in any layer yet.
	 */
	CQuad MakeQuad(int CenterX, int CenterY, int Width, int Height);

	/**
	 * Puts a quad at the end of a quad layer, and says which one it became.
	 *
	 * @param Doc The document being changed.
	 * @param Layer Which layer, which has to be a quad layer.
	 * @param Quad The quad to put in.
	 *
	 * @return Which quad of that layer it is.
	 */
	size_t AddQuad(CDocument &Doc, const CLayerAddress &Layer, const CQuad &Quad);

	/** Takes one quad out of a quad layer. */
	void DeleteQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad);

	/**
	 * Puts a changed quad back in place of the one that was there - which is
	 * what dragging a corner, a colour or a pivot comes down to.
	 *
	 * @param Doc The document being changed.
	 * @param Layer Which layer.
	 * @param Quad Which quad of it.
	 * @param Changed What the quad is to be.
	 */
	void SetQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad, const CQuad &Changed);

	/**
	 * The four ways a quad is put in order rather than dragged into it.
	 *
	 * All four are what the editor in the client offers beside a quad, and
	 * all four are worth having because a quad dragged by four corners is
	 * almost never the rectangle somebody meant.
	 */
	enum class EQuadShape
	{
		/** The rectangle its corners span - top, left, bottom, right. */
		SQUARE,
		/** As wide as it is, and as tall as its picture's proportions ask. */
		ASPECT,
		/** The pivot into the middle of the corners. */
		CENTER_PIVOT,
		/** Every corner onto the nearest crossing of a grid. */
		ALIGN,
	};

	/**
	 * Puts one quad into shape.
	 *
	 * @param Doc The document being changed.
	 * @param Layer Which layer, which has to be a quad layer.
	 * @param Quad Which quad of it.
	 * @param Shape Which of the four.
	 * @param Grid How far apart the crossings are for `ALIGN`, in world
	 * units; a tile is thirty-two. Means nothing to the other three.
	 *
	 * @return Whether it could be done, which is false only for `ASPECT` on a
	 * layer that is drawn with no picture - there are no proportions to ask.
	 */
	bool ShapeQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad, EQuadShape Shape, int Grid = 32);

	/**
	 * Adds a picture at the end, and says where it went.
	 *
	 * @param Doc The document being changed.
	 * @param Image The picture to add.
	 *
	 * @return Which picture of the map it became.
	 */
	size_t AddImage(CDocument &Doc, CImage Image);

	/**
	 * Takes a picture out of the map, and takes it off every layer that was
	 * drawn with it.
	 *
	 * A layer names a picture by its place, the same way a quad names an
	 * envelope, so the same rebinding is needed: what pointed past the one
	 * that is gone comes down one, and a layer that was drawn with it is
	 * drawn with none. A layer that used a different picture is left as the
	 * node it is, so this costs the layers that used this one rather than the
	 * map.
	 *
	 * @param Doc The document being changed.
	 * @param Image Which picture to take out.
	 */
	void DeleteImage(CDocument &Doc, size_t Image);

	/**
	 * Puts another picture in the place of one, keeping every layer that is
	 * drawn with it.
	 *
	 * Which is what replacing a picture is for: the tiles stay where they
	 * are and the picture under them changes.
	 *
	 * @param Doc The document being changed.
	 * @param Index Which picture of the map.
	 * @param Changed What it is to be.
	 */
	void SetImage(CDocument &Doc, size_t Index, CImage Changed);

	/**
	 * Adds an envelope at the end, and says where it went.
	 *
	 * @param Doc The document being changed.
	 * @param Envelope The envelope to add.
	 *
	 * @return Which envelope it became.
	 */
	size_t AddEnvelope(CDocument &Doc, CEnvelope Envelope);

	/**
	 * Takes an envelope out of the map, and takes it off whatever was bound
	 * to it.
	 *
	 * A layer or a quad names an envelope by its place, so the ones after it
	 * move up - which means every binding above it has to come down by one,
	 * and a binding to the one being taken away becomes no binding at all.
	 * Leaving that to the caller would mean a map whose colours come from the
	 * wrong envelope, which is worse than one that is slower to save.
	 *
	 * @param Doc The document being changed.
	 * @param Envelope Which envelope to take out.
	 */
	void DeleteEnvelope(CDocument &Doc, size_t Envelope);

	/**
	 * Puts a point into an envelope, in the place its time gives it.
	 *
	 * The points of an envelope are in time order and the sum that reads them
	 * counts on it, so where a point goes is not the caller's to choose: it
	 * goes where its time puts it, after any point at the same time.
	 *
	 * @param Doc The document being changed.
	 * @param Envelope Which envelope.
	 * @param Point The point to put in.
	 *
	 * @return Which point of that envelope it became.
	 */
	size_t AddEnvelopePoint(CDocument &Doc, size_t Envelope, const CEnvPoint_runtime &Point);

	/** Takes one point out of an envelope. */
	void DeleteEnvelopePoint(CDocument &Doc, size_t Envelope, size_t Point);

	/**
	 * Changes one point, and puts it back in time order if it moved.
	 *
	 * @param Doc The document being changed.
	 * @param Envelope Which envelope.
	 * @param Point Which point of it.
	 * @param Changed What the point is to be.
	 *
	 * @return Where that point is now, which is somewhere else if its time
	 * moved it past one of its neighbours.
	 */
	size_t SetEnvelopePoint(CDocument &Doc, size_t Envelope, size_t Point, const CEnvPoint_runtime &Changed);

	/**
	 * Changes the properties of a group - its name, its parallax, its offset,
	 * its clipping - or the order of its layers.
	 *
	 * The three steps of `CMapState` as one, the way `EditTileLayer` does them
	 * for a layer.
	 */
	template<typename FChange>
	void EditGroup(CDocument &Doc, size_t Group, FChange &&Change)
	{
		CGroup Changed = *Doc.Edit().Group(Group);
		Change(Changed);
		Doc.Edit().ReplaceGroup(Group, std::move(Changed));
	}

	/**
	 * Changes a layer of whatever kind - what all of them have is a name and
	 * the detail flag, and a caller that knows more than that looks into the
	 * variant itself.
	 */
	template<typename FChange>
	void EditLayer(CDocument &Doc, const CLayerAddress &Address, FChange &&Change)
	{
		CLayer Changed = *Doc.Edit().Layer(Address.m_Group, Address.m_Layer);
		Change(Changed);
		Doc.Edit().ReplaceLayer(Address.m_Group, Address.m_Layer, std::move(Changed));
	}
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_STRUCTURE_H
