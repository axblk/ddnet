#ifndef GAME_MAP_DOCUMENT_LAYER_H
#define GAME_MAP_DOCUMENT_LAYER_H

#include <game/map/document/shared_list.h>
#include <game/map/document/tile_store.h>
#include <game/mapitems.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <variant>

namespace map_document
{
	/**
	 * What kind of tiles a layer holds.
	 *
	 * Only `TILES` is a layer that is merely drawn. The rest are the physics
	 * layers, and each of them carries a second plane of tiles next to the one
	 * that is drawn: where a switch is, which tele it leads to, how hard a
	 * speedup pushes. The file format keeps them apart the same way, and so does
	 * the editor that exists today.
	 */
	enum class ETileLayerKind
	{
		TILES,
		GAME,
		FRONT,
		TELE,
		SPEEDUP,
		SWITCH,
		TUNE,
	};

	/**
	 * What every layer has, whatever it holds.
	 *
	 * The file format gives each kind of layer its own item with its own name in
	 * it, so this is not a layer that the others are a kind of - it is the two
	 * things the editor asks of a layer without caring what is in it.
	 */
	class CLayerProperties
	{
	public:
		std::string m_Name;
		/** Left out when "high detail" is off, which is the map's own choice. */
		bool m_Detail = false;
	};

	/**
	 * One tile layer, as a node that nobody writes to once it is shared.
	 *
	 * A version of the map is a version of every layer, so a layer has to be
	 * copyable for the price of its pointers: what a copy costs is the table of
	 * blocks, and the blocks themselves are shared until somebody writes to one
	 * of the two copies - see `CTileStore`. Changing a layer is therefore
	 * copying it, writing to the copy, and putting the copy in place of the old
	 * one; the old one stays valid for as long as a version of the map still
	 * names it, which is what makes undo a pointer swap.
	 *
	 * The fields are plain because a shared layer is held as `const` and a layer
	 * that is being changed belongs to whoever is changing it. Hiding them behind
	 * setters would suggest that a layer can be edited in place, which is exactly
	 * what must not happen.
	 */
	class CTileLayer : public CLayerProperties
	{
	public:
		/**
		 * The second plane of a physics layer, and nothing at all for a layer
		 * that is only drawn.
		 */
		using CExtraTiles = std::variant<
			std::monostate,
			CTileStore<CTeleTile>,
			CTileStore<CSpeedupTile>,
			CTileStore<CSwitchTile>,
			CTileStore<CTuneTile>>;

		CTileLayer() = default;

		CTileLayer(ETileLayerKind Kind, int Width, int Height) :
			m_Kind(Kind), m_Tiles(Width, Height)
		{
			switch(Kind)
			{
			case ETileLayerKind::TELE: m_ExtraTiles.emplace<CTileStore<CTeleTile>>(Width, Height); break;
			case ETileLayerKind::SPEEDUP: m_ExtraTiles.emplace<CTileStore<CSpeedupTile>>(Width, Height); break;
			case ETileLayerKind::SWITCH: m_ExtraTiles.emplace<CTileStore<CSwitchTile>>(Width, Height); break;
			case ETileLayerKind::TUNE: m_ExtraTiles.emplace<CTileStore<CTuneTile>>(Width, Height); break;
			case ETileLayerKind::TILES:
			case ETileLayerKind::GAME:
			case ETileLayerKind::FRONT: break;
			}
		}

		ETileLayerKind m_Kind = ETileLayerKind::TILES;

		// Only a layer that is drawn as an image has these; a physics layer takes
		// its picture from the entities of the game, not from the map.
		int m_Image = -1;
		CColor m_Color = {255, 255, 255, 255};
		int m_ColorEnvelope = -1;
		int m_ColorEnvelopeOffset = 0;

		// What the automapper does to this layer when it is asked to. Only a
		// layer that is drawn has one; a physics layer is not automapped.
		int m_AutomapperConfig = -1;
		int m_AutomapperSeed = 0;
		bool m_AutomapperAutomatic = false;

		CTileStore<CTile> m_Tiles;
		CExtraTiles m_ExtraTiles;

		int Width() const { return m_Tiles.Width(); }
		int Height() const { return m_Tiles.Height(); }

		/**
		 * Gives the layer another size, both of its planes at once.
		 *
		 * What is still on the layer stays where it is - the top left corner
		 * does not move - and what falls outside is gone. A physics layer
		 * carries its second plane along, because a tele number without the
		 * tile it belongs to is not a layer anybody could save.
		 */
		void Resize(int Width, int Height)
		{
			m_Tiles.Resize(Width, Height);
			std::visit([Width, Height](auto &Extra) {
				if constexpr(!std::is_same_v<std::decay_t<decltype(Extra)>, std::monostate>)
				{
					Extra.Resize(Width, Height);
				}
			},
				m_ExtraTiles);
		}

		/**
		 * Whether two layers are the same layer, everything about them
		 * included. Two versions that share their blocks answer this without
		 * looking into them - see `CTileStore::operator==`.
		 */
		bool operator==(const CTileLayer &Other) const
		{
			return m_Kind == Other.m_Kind &&
			       m_Name == Other.m_Name &&
			       m_Detail == Other.m_Detail &&
			       m_Image == Other.m_Image &&
			       m_Color.r == Other.m_Color.r && m_Color.g == Other.m_Color.g &&
			       m_Color.b == Other.m_Color.b && m_Color.a == Other.m_Color.a &&
			       m_ColorEnvelope == Other.m_ColorEnvelope &&
			       m_ColorEnvelopeOffset == Other.m_ColorEnvelopeOffset &&
			       m_AutomapperConfig == Other.m_AutomapperConfig &&
			       m_AutomapperSeed == Other.m_AutomapperSeed &&
			       m_AutomapperAutomatic == Other.m_AutomapperAutomatic &&
			       m_Tiles == Other.m_Tiles &&
			       m_ExtraTiles == Other.m_ExtraTiles;
		}
		bool operator!=(const CTileLayer &Other) const { return !(*this == Other); }

		/** What this layer holds, both planes of it. */
		uint64_t Bytes() const
		{
			std::unordered_set<const void *> Seen;
			return BytesOnce(Seen);
		}

		/** The same, counting nothing twice - see `CTileStore::BytesOnce`. */
		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const
		{
			uint64_t Total = m_Tiles.BytesOnce(Seen);
			std::visit([&Total, &Seen](const auto &Extra) {
				if constexpr(!std::is_same_v<std::decay_t<decltype(Extra)>, std::monostate>)
				{
					Total += Extra.BytesOnce(Seen);
				}
			},
				m_ExtraTiles);
			return Total;
		}
	};

	/**
	 * One quad layer, as a node that nobody writes to once it is shared.
	 *
	 * A quad is a few dozen bytes and a layer of them is a list, so a version
	 * that moves one corner copies that list and shares the rest of the map -
	 * there is nothing here to split into blocks the way tiles are.
	 */
	class CQuadLayer : public CLayerProperties
	{
	public:
		/** The image the quads are drawn with, or -1 for plain colour. */
		int m_Image = -1;

		CSharedList<CQuad> m_Quads;

		bool operator==(const CQuadLayer &Other) const
		{
			if(m_Name != Other.m_Name || m_Detail != Other.m_Detail || m_Image != Other.m_Image)
				return false;
			if(m_Quads.Id() == Other.m_Quads.Id())
				return true;
			if(m_Quads.Size() != Other.m_Quads.Size())
				return false;
			return m_Quads.Empty() ||
			       std::memcmp(m_Quads.All().data(), Other.m_Quads.All().data(), m_Quads.Size() * sizeof(CQuad)) == 0;
		}
		bool operator!=(const CQuadLayer &Other) const { return !(*this == Other); }

		uint64_t Bytes() const
		{
			std::unordered_set<const void *> Seen;
			return BytesOnce(Seen);
		}

		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const { return m_Quads.BytesOnce(Seen); }
	};

	/**
	 * One sound layer, as a node that nobody writes to once it is shared.
	 *
	 * Same shape as a quad layer: a list of places where a sound is heard, and
	 * how far it carries.
	 */
	class CSoundLayer : public CLayerProperties
	{
	public:
		/** The sound these sources play, or -1 for a layer that plays none. */
		int m_Sound = -1;

		CSharedList<CSoundSource> m_Sources;

		bool operator==(const CSoundLayer &Other) const
		{
			if(m_Name != Other.m_Name || m_Detail != Other.m_Detail || m_Sound != Other.m_Sound)
				return false;
			if(m_Sources.Id() == Other.m_Sources.Id())
				return true;
			if(m_Sources.Size() != Other.m_Sources.Size())
				return false;
			return m_Sources.Empty() ||
			       std::memcmp(m_Sources.All().data(), Other.m_Sources.All().data(), m_Sources.Size() * sizeof(CSoundSource)) == 0;
		}
		bool operator!=(const CSoundLayer &Other) const { return !(*this == Other); }

		uint64_t Bytes() const
		{
			std::unordered_set<const void *> Seen;
			return BytesOnce(Seen);
		}

		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const { return m_Sources.BytesOnce(Seen); }
	};

	/**
	 * A layer of a map, of whatever kind.
	 *
	 * The three kinds have almost nothing in common - a tile layer is a grid, a
	 * quad layer a list, a sound layer a list of something else - so they are
	 * held as what they are rather than behind a shared interface that would only
	 * ever be asked which one it is.
	 */
	using CLayer = std::variant<CTileLayer, CQuadLayer, CSoundLayer>;

	/** The name and the detail flag of a layer, whatever kind it is. */
	inline const CLayerProperties &LayerProperties(const CLayer &Layer)
	{
		return std::visit([](const auto &Kind) -> const CLayerProperties & { return Kind; }, Layer);
	}

	/** What a layer holds, counting nothing that is already in `Seen`. */
	inline uint64_t LayerBytesOnce(const CLayer &Layer, std::unordered_set<const void *> &Seen)
	{
		return std::visit([&Seen](const auto &Kind) { return Kind.BytesOnce(Seen); }, Layer);
	}

	/** What a layer holds, as if it were the only one holding it. */
	inline uint64_t LayerBytes(const CLayer &Layer)
	{
		std::unordered_set<const void *> Seen;
		return LayerBytesOnce(Layer, Seen);
	}
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_LAYER_H
