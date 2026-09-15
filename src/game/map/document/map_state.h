#ifndef GAME_MAP_DOCUMENT_MAP_STATE_H
#define GAME_MAP_DOCUMENT_MAP_STATE_H

#include <base/dbg.h>

#include <game/map/document/layer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * A group of layers, as a node that nobody writes to once it is shared.
 *
 * Same rule as `CTileLayer`: a copy copies the list of layers, which is a
 * list of pointers, and the layers themselves are shared until one of them is
 * replaced.
 */
class CGroup
{
public:
	std::string m_Name;

	int m_OffsetX = 0;
	int m_OffsetY = 0;
	int m_ParallaxX = 100;
	int m_ParallaxY = 100;

	bool m_UseClipping = false;
	int m_ClipX = 0;
	int m_ClipY = 0;
	int m_ClipW = 0;
	int m_ClipH = 0;

	std::vector<std::shared_ptr<const CTileLayer>> m_vpLayers;

	/** What this group holds, its layers with it. */
	uint64_t Bytes() const
	{
		std::unordered_set<const void *> Seen;
		return BytesOnce(Seen);
	}

	/** The same, counting nothing twice - see `CTileStore::BytesOnce`. */
	uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const
	{
		uint64_t Total = 0;
		for(const auto &pLayer : m_vpLayers)
		{
			if(Seen.insert(pLayer.get()).second)
				Total += sizeof(CTileLayer) + pLayer->BytesOnce(Seen);
		}
		return Total;
	}
};

/**
 * One version of the map: everything the editor would write to a file, with
 * nothing in it that says how it is drawn or looked at.
 *
 * Copying a state copies the list of groups, so a version costs a handful of
 * pointers plus whatever the change itself made new. Changing something is
 * always the same three steps, and they are meant to be read as one:
 *
 * ```
 * CMapState Next = Current;                       // pointers only
 * CTileLayer Changed = *Next.Layer(Group, Layer); // pointers only
 * Changed.m_Tiles.Set(x, y, Tile);                // one block, taken apart
 * Next.ReplaceLayer(Group, Layer, std::move(Changed));
 * ```
 *
 * `Current` is untouched by all of it, which is the whole point: the history
 * is a list of states, and undo hands back the one before.
 */
class CMapState
{
public:
	std::vector<std::shared_ptr<const CGroup>> m_vpGroups;

	size_t NumGroups() const { return m_vpGroups.size(); }

	size_t NumLayers(size_t Group) const
	{
		dbg_assert(Group < m_vpGroups.size(), "Group out of range");
		return m_vpGroups[Group]->m_vpLayers.size();
	}

	const CGroup *Group(size_t Group) const
	{
		dbg_assert(Group < m_vpGroups.size(), "Group out of range");
		return m_vpGroups[Group].get();
	}

	const CTileLayer *Layer(size_t Group, size_t Layer) const
	{
		const CGroup *pGroup = this->Group(Group);
		dbg_assert(Layer < pGroup->m_vpLayers.size(), "Layer out of range");
		return pGroup->m_vpLayers[Layer].get();
	}

	/**
	 * Puts a changed layer in place of the one that was there. Every other
	 * layer of that group, and every other group, comes along unchanged - as
	 * the same node, not as a copy of it.
	 */
	void ReplaceLayer(size_t GroupIndex, size_t LayerIndex, CTileLayer Changed)
	{
		CGroup ChangedGroup = *Group(GroupIndex);
		dbg_assert(LayerIndex < ChangedGroup.m_vpLayers.size(), "Layer out of range");
		ChangedGroup.m_vpLayers[LayerIndex] = std::make_shared<const CTileLayer>(std::move(Changed));
		m_vpGroups[GroupIndex] = std::make_shared<const CGroup>(std::move(ChangedGroup));
	}

	/**
	 * Puts a changed group in place of the one that was there, for the
	 * properties of the group itself and for adding, removing or reordering
	 * its layers.
	 */
	void ReplaceGroup(size_t GroupIndex, CGroup Changed)
	{
		dbg_assert(GroupIndex < m_vpGroups.size(), "Group out of range");
		m_vpGroups[GroupIndex] = std::make_shared<const CGroup>(std::move(Changed));
	}

	void AddGroup(CGroup Group)
	{
		m_vpGroups.push_back(std::make_shared<const CGroup>(std::move(Group)));
	}

	/**
	 * What this version holds, as if it were the only one - two versions that
	 * share all but one block both answer with the whole map.
	 *
	 * What a history of versions costs together is `CHistory::Bytes`, which
	 * walks them with one `Seen` between them.
	 */
	uint64_t Bytes() const
	{
		std::unordered_set<const void *> Seen;
		return BytesOnce(Seen);
	}

	/** The same, counting nothing twice - see `CTileStore::BytesOnce`. */
	uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const
	{
		uint64_t Total = 0;
		for(const auto &pGroup : m_vpGroups)
		{
			if(Seen.insert(pGroup.get()).second)
				Total += sizeof(CGroup) + pGroup->BytesOnce(Seen);
		}
		return Total;
	}
};

#endif // GAME_MAP_DOCUMENT_MAP_STATE_H
