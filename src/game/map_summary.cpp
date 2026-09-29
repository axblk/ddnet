#include "map_summary.h"

#include <engine/map.h>
#include <engine/shared/jsonwriter.h>

#include <game/mapitems.h>

namespace
{
	void WriteMapString(CJsonWriter &Writer, IMap &Map, const char *pName, int Index)
	{
		if(Index < 0)
			return;
		const char *pValue = Map.GetDataString(Index);
		if(pValue == nullptr || pValue[0] == '\0')
			return;
		Writer.WriteAttribute(pName);
		Writer.WriteStrValue(pValue);
	}

	void WriteCount(CJsonWriter &Writer, IMap &Map, const char *pName, int Type)
	{
		int Start, Num;
		Map.GetType(Type, &Start, &Num);
		Writer.WriteAttribute(pName);
		Writer.WriteIntValue(Num);
	}
} // namespace

void WriteMapSummary(CJsonWriter &Writer, IMap &Map)
{
	const int InfoIndex = Map.FindItemIndex(MAPITEMTYPE_INFO, 0);
	if(InfoIndex >= 0 && Map.GetItemSize(InfoIndex) >= (int)sizeof(CMapItemInfo))
	{
		const CMapItemInfo *pInfo = static_cast<const CMapItemInfo *>(Map.GetItem(InfoIndex));
		WriteMapString(Writer, Map, "author", pInfo->m_Author);
		WriteMapString(Writer, Map, "version", pInfo->m_MapVersion);
		WriteMapString(Writer, Map, "credits", pInfo->m_Credits);
		WriteMapString(Writer, Map, "license", pInfo->m_License);
		// The settings are commands one after the other, each ended by a zero.
		int NumSettings = 0;
		if(Map.GetItemSize(InfoIndex) >= (int)sizeof(CMapItemInfoSettings))
		{
			const int SettingsIndex = static_cast<const CMapItemInfoSettings *>(pInfo)->m_Settings;
			if(SettingsIndex >= 0)
			{
				const char *pSettings = static_cast<const char *>(Map.GetData(SettingsIndex));
				const int Size = Map.GetDataSize(SettingsIndex);
				for(int i = 0; pSettings != nullptr && i < Size; ++i)
				{
					if(pSettings[i] == '\0')
						++NumSettings;
				}
			}
		}
		Writer.WriteAttribute("settings");
		Writer.WriteIntValue(NumSettings);
	}

	int LayersStart, NumLayers;
	Map.GetType(MAPITEMTYPE_LAYER, &LayersStart, &NumLayers);
	for(int i = 0; i < NumLayers; ++i)
	{
		const CMapItemLayer *pLayer = static_cast<const CMapItemLayer *>(Map.GetItem(LayersStart + i));
		if(pLayer == nullptr || pLayer->m_Type != LAYERTYPE_TILES || Map.GetItemSize(LayersStart + i) < (int)sizeof(CMapItemLayerTilemap_v2))
			continue;
		const CMapItemLayerTilemap_v2 *pTilemap = reinterpret_cast<const CMapItemLayerTilemap_v2 *>(pLayer);
		if(pTilemap->m_Flags & TILESLAYERFLAG_GAME)
		{
			Writer.WriteAttribute("width");
			Writer.WriteIntValue(pTilemap->m_Width);
			Writer.WriteAttribute("height");
			Writer.WriteIntValue(pTilemap->m_Height);
			break;
		}
	}
	WriteCount(Writer, Map, "groups", MAPITEMTYPE_GROUP);
	WriteCount(Writer, Map, "layers", MAPITEMTYPE_LAYER);
	WriteCount(Writer, Map, "images", MAPITEMTYPE_IMAGE);
	WriteCount(Writer, Map, "sounds", MAPITEMTYPE_SOUND);
	WriteCount(Writer, Map, "envelopes", MAPITEMTYPE_ENVELOPE);
}
