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
