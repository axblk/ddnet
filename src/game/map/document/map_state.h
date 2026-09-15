#ifndef GAME_MAP_DOCUMENT_MAP_STATE_H
#define GAME_MAP_DOCUMENT_MAP_STATE_H

#include <base/dbg.h>

#include <game/map/document/assets.h>
#include <game/map/document/layer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace map_document
{
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

		std::vector<std::shared_ptr<const CLayer>> m_vpLayers;

		/**
		 * Whether two groups are the same group. Layers that are the same
		 * node are not looked into, so this is cheap between two versions of
		 * one group and dear between two groups that were built separately.
		 */
		bool operator==(const CGroup &Other) const
		{
			if(m_Name != Other.m_Name ||
				m_OffsetX != Other.m_OffsetX || m_OffsetY != Other.m_OffsetY ||
				m_ParallaxX != Other.m_ParallaxX || m_ParallaxY != Other.m_ParallaxY ||
				m_UseClipping != Other.m_UseClipping ||
				m_ClipX != Other.m_ClipX || m_ClipY != Other.m_ClipY ||
				m_ClipW != Other.m_ClipW || m_ClipH != Other.m_ClipH ||
				m_vpLayers.size() != Other.m_vpLayers.size())
				return false;
			for(size_t i = 0; i < m_vpLayers.size(); ++i)
			{
				if(m_vpLayers[i] == Other.m_vpLayers[i])
					continue;
				if(*m_vpLayers[i] != *Other.m_vpLayers[i])
					return false;
			}
			return true;
		}
		bool operator!=(const CGroup &Other) const { return !(*this == Other); }

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
					Total += sizeof(CLayer) + LayerBytesOnce(*pLayer, Seen);
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
	 * CMapState Next = Current;                           // pointers only
	 * CTileLayer Changed = *Next.TileLayer(Group, Layer);  // pointers only
	 * Changed.m_Tiles.Set(x, y, Tile);                     // one block, apart
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

		/**
		 * What the groups point at by number, and what the map says about
		 * itself. They are versioned with everything else, so an undo takes back
		 * a renamed envelope as readily as a painted tile.
		 */
		std::vector<std::shared_ptr<const CEnvelope>> m_vpEnvelopes;
		std::vector<std::shared_ptr<const CImage>> m_vpImages;
		std::vector<std::shared_ptr<const CSound>> m_vpSounds;
		CMapInfo m_Info;

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

		const CLayer *Layer(size_t Group, size_t Layer) const
		{
			const CGroup *pGroup = this->Group(Group);
			dbg_assert(Layer < pGroup->m_vpLayers.size(), "Layer out of range");
			return pGroup->m_vpLayers[Layer].get();
		}

		/**
		 * The same layer, for a caller that already knows it holds tiles - the
		 * game layer of a map, say, or the layer a tile tool is working on.
		 */
		const CTileLayer *TileLayer(size_t Group, size_t Layer) const
		{
			const CLayer *pLayer = this->Layer(Group, Layer);
			dbg_assert(std::holds_alternative<CTileLayer>(*pLayer), "Layer %d of group %d holds no tiles", (int)Layer, (int)Group);
			return &std::get<CTileLayer>(*pLayer);
		}

		/**
		 * Puts a changed layer in place of the one that was there. Every other
		 * layer of that group, and every other group, comes along unchanged - as
		 * the same node, not as a copy of it.
		 */
		void ReplaceLayer(size_t GroupIndex, size_t LayerIndex, CLayer Changed)
		{
			CGroup ChangedGroup = *Group(GroupIndex);
			dbg_assert(LayerIndex < ChangedGroup.m_vpLayers.size(), "Layer out of range");
			ChangedGroup.m_vpLayers[LayerIndex] = std::make_shared<const CLayer>(std::move(Changed));
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

		size_t NumEnvelopes() const { return m_vpEnvelopes.size(); }
		size_t NumImages() const { return m_vpImages.size(); }
		size_t NumSounds() const { return m_vpSounds.size(); }

		const CEnvelope *Envelope(size_t Index) const
		{
			dbg_assert(Index < m_vpEnvelopes.size(), "Envelope out of range");
			return m_vpEnvelopes[Index].get();
		}

		const CImage *Image(size_t Index) const
		{
			dbg_assert(Index < m_vpImages.size(), "Image out of range");
			return m_vpImages[Index].get();
		}

		const CSound *Sound(size_t Index) const
		{
			dbg_assert(Index < m_vpSounds.size(), "Sound out of range");
			return m_vpSounds[Index].get();
		}

		/**
		 * Puts a changed envelope, image or sound in place of the one that was
		 * there - everything else in the list comes along as the same node.
		 */
		void ReplaceEnvelope(size_t Index, CEnvelope Changed)
		{
			dbg_assert(Index < m_vpEnvelopes.size(), "Envelope out of range");
			m_vpEnvelopes[Index] = std::make_shared<const CEnvelope>(std::move(Changed));
		}

		void ReplaceImage(size_t Index, CImage Changed)
		{
			dbg_assert(Index < m_vpImages.size(), "Image out of range");
			m_vpImages[Index] = std::make_shared<const CImage>(std::move(Changed));
		}

		void ReplaceSound(size_t Index, CSound Changed)
		{
			dbg_assert(Index < m_vpSounds.size(), "Sound out of range");
			m_vpSounds[Index] = std::make_shared<const CSound>(std::move(Changed));
		}

		void AddEnvelope(CEnvelope Envelope)
		{
			m_vpEnvelopes.push_back(std::make_shared<const CEnvelope>(std::move(Envelope)));
		}

		void AddImage(CImage Image)
		{
			m_vpImages.push_back(std::make_shared<const CImage>(std::move(Image)));
		}

		void AddSound(CSound Sound)
		{
			m_vpSounds.push_back(std::make_shared<const CSound>(std::move(Sound)));
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
			uint64_t Total = m_Info.BytesOnce(Seen);
			for(const auto &pGroup : m_vpGroups)
			{
				if(Seen.insert(pGroup.get()).second)
					Total += sizeof(CGroup) + pGroup->BytesOnce(Seen);
			}
			for(const auto &pEnvelope : m_vpEnvelopes)
			{
				if(Seen.insert(pEnvelope.get()).second)
					Total += sizeof(CEnvelope) + pEnvelope->BytesOnce(Seen);
			}
			for(const auto &pImage : m_vpImages)
			{
				if(Seen.insert(pImage.get()).second)
					Total += sizeof(CImage) + pImage->BytesOnce(Seen);
			}
			for(const auto &pSound : m_vpSounds)
			{
				if(Seen.insert(pSound.get()).second)
					Total += sizeof(CSound) + pSound->BytesOnce(Seen);
			}
			return Total;
		}
	};
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_MAP_STATE_H
