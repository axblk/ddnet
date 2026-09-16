#include "explain.h"

#include "edit.h"

#include <game/explanations.h>
#include <game/mapitems.h>

#include <type_traits>
#include <variant>

namespace map_document
{
	const char *ExplainTile(ETileLayerKind Kind, int Index)
	{
		int Layer;
		switch(Kind)
		{
		case ETileLayerKind::GAME: Layer = LAYER_GAME; break;
		case ETileLayerKind::FRONT: Layer = LAYER_FRONT; break;
		case ETileLayerKind::TELE: Layer = LAYER_TELE; break;
		case ETileLayerKind::SPEEDUP: Layer = LAYER_SPEEDUP; break;
		case ETileLayerKind::SWITCH: Layer = LAYER_SWITCH; break;
		case ETileLayerKind::TUNE: Layer = LAYER_TUNE; break;
		case ETileLayerKind::TILES:
		default: return nullptr;
		}
		if(Index < 0 || Index > 255)
			return nullptr;
		// The entities sheet the editor draws with is DDNet's
		// (`map_view_support.h`), so the sentences are DDNet's too. A choice
		// of sheets would be a choice here as well.
		return CExplanations::Explain(CExplanations::EGametype::DDNET, Index, Layer);
	}

	int TileMeaning(const CTileLayer &Layer, int x, int y)
	{
		if(x < 0 || y < 0 || x >= Layer.Width() || y >= Layer.Height())
			return -1;
		if(DrawsOwnTiles(Layer.m_Kind))
			return Layer.m_Tiles.Get(x, y).m_Index;
		return std::visit([x, y](const auto &Store) -> int {
			using TStore = std::decay_t<decltype(Store)>;
			if constexpr(std::is_same_v<TStore, std::monostate>)
				return -1;
			else
				return Store.Get(x, y).m_Type;
		},
			Layer.m_ExtraTiles);
	}
} // namespace map_document
