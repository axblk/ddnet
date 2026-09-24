/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "mapimages.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>

#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/map.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/client_data.h>

#include <game/client/gameclient.h>
#include <game/layers.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cstddef>
#include <vector>

CMapImages::CMapImages()
{
	std::fill(std::begin(m_aEntitiesIsLoaded), std::end(m_aEntitiesIsLoaded), false);
	m_SpeedupArrowIsLoaded = false;
	std::fill(std::begin(m_aTuneColorsIsLoaded), std::end(m_aTuneColorsIsLoaded), false);

	str_copy(m_aEntitiesPath, "editor/entities_clear");

	static_assert(std::size(gs_apModEntitiesNames) == MAP_IMAGE_MOD_TYPE_COUNT, "Mod name string count is not equal to mod type count");
}

CMapRenderImages::CMapRenderImages(CMapImages &Assets) :
	m_Assets(Assets)
{
}

void CMapImages::OnInit()
{
	m_TextureScale = g_Config.m_ClTextEntitiesSize;
	InitOverlayTextures();

	if(str_comp(g_Config.m_ClAssetsEntities, "default") == 0)
		str_copy(m_aEntitiesPath, "editor/entities_clear");
	else
	{
		str_format(m_aEntitiesPath, sizeof(m_aEntitiesPath), "assets/entities/%s", g_Config.m_ClAssetsEntities);
	}

	Console()->Chain("cl_text_entities_size", ConchainClTextEntitiesSize, this);
}

void CMapImages::OnUpdate()
{
	FinishEntitiesLoads();
}

void CMapImages::OnShutdown()
{
	m_vEntitiesLoads.clear();
	m_SpeedupArrowResource.Reset();
	for(int EntityVariant = 0; EntityVariant < MAP_IMAGE_MOD_TYPE_COUNT * 2; ++EntityVariant)
	{
		for(auto &Texture : m_aaEntitiesTextures[EntityVariant])
			Graphics()->UnloadTexture(&Texture);
		Graphics()->UnloadTexture(&m_aTuneColorMapTextures[EntityVariant]);
	}
	Graphics()->UnloadTexture(&m_SpeedupArrowTexture);
}

void CMapImages::FinishEntitiesLoads()
{
	for(auto It = m_vEntitiesLoads.begin(); It != m_vEntitiesLoads.end();)
	{
		if(std::any_of(It->m_vResources.begin(), It->m_vResources.end(), [](const CImageResource &Resource) { return !Resource.IsFinished(); }))
		{
			++It;
			continue;
		}
		bool Loaded = false;
		for(CImageResource &Resource : It->m_vResources)
		{
			if(!Resource.IsReady())
				continue;
			CImageInfo Image = Resource.TakeImage();
			Loaded = FinishEntitiesLoad(*It, Image, Resource.Path());
			Image.Free();
			if(Loaded)
				break;
		}
		if(!Loaded)
			log_error("mapimages", "Failed to load entities image for '%s'.", gs_apModEntitiesNames[It->m_ModType]);
		It = m_vEntitiesLoads.erase(It);
	}

	m_SpeedupArrowResource.FinishTexture(Graphics(), m_SpeedupArrowTexture, IGraphics::TEXLOAD_LAYERED | IGraphics::TEXLOAD_NO_2D_TEXTURE);
}

void CMapRenderImages::Update()
{
	bool ShowWarning = false;
	for(auto It = m_vImageLoads.begin(); It != m_vImageLoads.end();)
	{
		if(!It->m_Resource.IsFinished())
		{
			++It;
			continue;
		}
		if(It->m_Resource.IsReady())
		{
			CImageInfo Image = It->m_Resource.TakeImage();
			m_aTextures[It->m_Index] = Graphics()->LoadTextureRawMove(Image, It->m_LoadFlags, It->m_Resource.Path());
			ShowWarning = ShowWarning || !m_aTextures[It->m_Index].IsValid() || m_aTextures[It->m_Index].IsNullTexture();
		}
		else if(It->m_Resource.IsFailed())
		{
			log_error("mapimages", "Failed to load map image '%s'.", It->m_Resource.Path());
			ShowWarning = true;
		}
		It = m_vImageLoads.erase(It);
	}
	if(ShowWarning)
		Client()->AddWarning(SWarning(Localize("Some map images could not be loaded. Check the local console for details.")));
}

void CMapRenderImages::Unload()
{
	m_vImageLoads.clear();
	// unload all textures
	for(int i = 0; i < m_Count; i++)
	{
		Graphics()->UnloadTexture(&m_aTextures[i]);
	}
	m_Count = 0;
}

void CMapRenderImages::Load(class CLayers *pLayers, IMap *pMap, bool Sixup)
{
	Unload();

	int Start;
	pMap->GetType(MAPITEMTYPE_IMAGE, &Start, &m_Count);
	m_Count = std::clamp<int>(m_Count, 0, MAX_MAPIMAGES);

	unsigned char aTextureUsedByTileOrQuadLayerFlag[MAX_MAPIMAGES] = {0}; // 0: nothing, 1(as flag): tile layer, 2(as flag): quad layer
	for(int GroupIndex = 0; GroupIndex < pLayers->NumGroups(); GroupIndex++)
	{
		const CMapItemGroup *pGroup = pLayers->GetGroup(GroupIndex);
		if(!pGroup)
		{
			continue;
		}

		for(int LayerIndex = 0; LayerIndex < pGroup->m_NumLayers; LayerIndex++)
		{
			const CMapItemLayer *pLayer = pLayers->GetLayer(pGroup->m_StartLayer + LayerIndex);
			if(!pLayer)
			{
				continue;
			}

			if(pLayer->m_Type == LAYERTYPE_TILES)
			{
				const CMapItemLayerTilemap *pLayerTilemap = reinterpret_cast<const CMapItemLayerTilemap *>(pLayer);
				if(pLayerTilemap->m_Image >= 0 && pLayerTilemap->m_Image < m_Count)
				{
					aTextureUsedByTileOrQuadLayerFlag[pLayerTilemap->m_Image] |= 1;
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				const CMapItemLayerQuads *pLayerQuads = reinterpret_cast<const CMapItemLayerQuads *>(pLayer);
				if(pLayerQuads->m_Image >= 0 && pLayerQuads->m_Image < m_Count)
				{
					aTextureUsedByTileOrQuadLayerFlag[pLayerQuads->m_Image] |= 2;
				}
			}
		}
	}

	// load new textures
	bool ShowWarning = false;
	for(int i = 0; i < m_Count; i++)
	{
		if(aTextureUsedByTileOrQuadLayerFlag[i] == 0)
		{
			// skip loading unused images
			continue;
		}

		const int LoadFlag = (((aTextureUsedByTileOrQuadLayerFlag[i] & 1) != 0) ? IGraphics::TEXLOAD_LAYERED : 0) | (((aTextureUsedByTileOrQuadLayerFlag[i] & 2) != 0) ? 0 : IGraphics::TEXLOAD_NO_2D_TEXTURE);
		const CMapItemImage_v2 *pImg = static_cast<const CMapItemImage_v2 *>(pMap->GetItem(Start + i));

		const char *pName = pMap->GetDataString(pImg->m_ImageName);
		if(pName == nullptr || pName[0] == '\0')
		{
			if(pImg->m_External)
			{
				log_error("mapimages", "Failed to load map image %d: failed to load name.", i);
				ShowWarning = true;
				continue;
			}
			pName = "(error)";
		}

		if(pImg->m_Version > 1 && pImg->m_MustBe1 != 1)
		{
			log_error("mapimages", "Failed to load map image %d '%s': invalid map image type.", i, pName);
			ShowWarning = true;
			continue;
		}

		if(pImg->m_External)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			bool Translated = false;
			if(Sixup)
			{
				Translated =
					!str_comp(pName, "grass_doodads") ||
					!str_comp(pName, "grass_main") ||
					!str_comp(pName, "winter_main") ||
					!str_comp(pName, "generic_shadows") ||
					!str_comp(pName, "generic_unhookable") ||
					!str_comp(pName, "easter");
			}
			str_format(aPath, sizeof(aPath), "mapres/%s%s.png", pName, Translated ? "_0.7" : "");
			m_vImageLoads.push_back({i, LoadFlag, GameClient()->AssetLoader().LoadImageFile(Storage(), aPath, IStorage::TYPE_ALL)});
		}
		else
		{
			if(pImg->m_Width <= 0 || pImg->m_Height <= 0)
			{
				log_error("mapimages", "Failed to load map image %d '%s': invalid image dimensions.", i, pName);
				ShowWarning = true;
				continue;
			}

			const size_t DataSize = (size_t)pImg->m_Width * pImg->m_Height * CImageInfo::PixelSize(CImageInfo::FORMAT_RGBA);
			CDataFileRawData RawData;
			if(!pMap->GetRawData(pImg->m_ImageData, RawData) || RawData.UncompressedSize() < DataSize)
			{
				log_error("mapimages", "Failed to load map image %d: failed to load data.", i);
				ShowWarning = true;
				continue;
			}
			char aTexName[IO_MAX_PATH_LENGTH];
			str_format(aTexName, sizeof(aTexName), "embedded: %s", pName);
			m_vImageLoads.push_back({i, LoadFlag, GameClient()->AssetLoader().LoadImageRawData(std::move(RawData), pImg->m_Width, pImg->m_Height, CImageInfo::FORMAT_RGBA, aTexName)});
		}
		pMap->UnloadData(pImg->m_ImageName);
	}
	if(ShowWarning)
	{
		Client()->AddWarning(SWarning(Localize("Some map images could not be loaded. Check the local console for details.")));
	}
}

static EMapImageModType GetEntitiesModType(const CGameInfo &GameInfo)
{
	if(GameInfo.m_EntitiesFDDrace)
		return MAP_IMAGE_MOD_TYPE_FDDRACE;
	else if(GameInfo.m_EntitiesDDNet)
		return MAP_IMAGE_MOD_TYPE_DDNET;
	else if(GameInfo.m_EntitiesDDRace)
		return MAP_IMAGE_MOD_TYPE_DDRACE;
	else if(GameInfo.m_EntitiesRace)
		return MAP_IMAGE_MOD_TYPE_RACE;
	else if(GameInfo.m_EntitiesBW)
		return MAP_IMAGE_MOD_TYPE_BLOCKWORLDS;
	else if(GameInfo.m_EntitiesFNG)
		return MAP_IMAGE_MOD_TYPE_FNG;
	else if(GameInfo.m_EntitiesVanilla)
		return MAP_IMAGE_MOD_TYPE_VANILLA;
	else
		return MAP_IMAGE_MOD_TYPE_DDNET;
}

void CMapRenderImages::SetGameInfo(const CGameInfo &GameInfo)
{
	m_EntitiesModType = GetEntitiesModType(GameInfo);
	m_EntitiesAreMasked = !GameInfo.m_DontMaskEntities;
}

IGraphics::CTextureHandle CMapRenderImages::GetEntities(EMapImageEntityLayerType EntityLayerType)
{
	return m_Assets.GetEntities(EntityLayerType, m_EntitiesModType, m_EntitiesAreMasked);
}

IGraphics::CTextureHandle CMapRenderImages::GetTuneColors()
{
	return m_Assets.GetTuneColors(m_EntitiesModType, m_EntitiesAreMasked);
}

static bool IsValidTile(int LayerType, bool EntitiesAreMasked, EMapImageModType EntitiesModType, int TileIndex)
{
	if(TileIndex == TILE_AIR)
		return false;
	if(!EntitiesAreMasked)
		return true;

	if(EntitiesModType == MAP_IMAGE_MOD_TYPE_DDNET || EntitiesModType == MAP_IMAGE_MOD_TYPE_DDRACE)
	{
		if(EntitiesModType == MAP_IMAGE_MOD_TYPE_DDNET || TileIndex != TILE_SPEED_BOOST_OLD)
		{
			if(LayerType == MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH &&
				!IsValidGameTile(TileIndex) &&
				!IsValidFrontTile(TileIndex) &&
				!IsValidSpeedupTile(TileIndex) &&
				!IsValidTeleTile(TileIndex) &&
				!IsValidTuneTile(TileIndex))
			{
				return false;
			}
			else if(LayerType == MAP_IMAGE_ENTITY_LAYER_TYPE_SWITCH &&
				!IsValidSwitchTile(TileIndex))
			{
				return false;
			}
		}
	}
	else if(EntitiesModType == MAP_IMAGE_MOD_TYPE_RACE && IsCreditsTile(TileIndex))
	{
		return false;
	}
	else if(EntitiesModType == MAP_IMAGE_MOD_TYPE_FNG && IsCreditsTile(TileIndex))
	{
		return false;
	}
	else if(EntitiesModType == MAP_IMAGE_MOD_TYPE_VANILLA && IsCreditsTile(TileIndex))
	{
		return false;
	}
	return true;
}

IGraphics::CTextureHandle CMapImages::GetEntities(EMapImageEntityLayerType EntityLayerType)
{
	const bool EntitiesAreMasked = !GameClient()->FocusedGameInfo().m_DontMaskEntities;
	const EMapImageModType EntitiesModType = GetEntitiesModType(GameClient()->FocusedGameInfo());
	return GetEntities(EntityLayerType, EntitiesModType, EntitiesAreMasked);
}

IGraphics::CTextureHandle CMapImages::GetEntities(EMapImageEntityLayerType EntityLayerType, EMapImageModType EntitiesModType, bool EntitiesAreMasked)
{
	const int EntityVariant = MapImageEntityVariant(EntitiesModType, EntitiesAreMasked);
	if(!m_aEntitiesIsLoaded[EntityVariant])
	{
		m_aEntitiesIsLoaded[EntityVariant] = true;
		CEntitiesLoad Load{EntityVariant, EntitiesModType, EntitiesAreMasked, {}};
		const auto Submit = [&](const char *pPath) {
			Load.m_vResources.push_back(GameClient()->AssetLoader().LoadImageFile(Storage(), pPath, IStorage::TYPE_ALL));
		};
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "%s/%s.png", m_aEntitiesPath, gs_apModEntitiesNames[EntitiesModType]);
		Submit(aPath);
		// try as single ddnet replacement
		if(EntitiesModType == MAP_IMAGE_MOD_TYPE_DDNET)
		{
			str_format(aPath, sizeof(aPath), "%s.png", m_aEntitiesPath);
			Submit(aPath);
		}
		// try default
		if(str_comp(m_aEntitiesPath, "editor/entities_clear") != 0)
		{
			str_format(aPath, sizeof(aPath), "editor/entities_clear/%s.png", gs_apModEntitiesNames[EntitiesModType]);
			Submit(aPath);
		}
		m_vEntitiesLoads.push_back(std::move(Load));
	}

	return m_aaEntitiesTextures[EntityVariant][EntityLayerType];
}

bool CMapImages::FinishEntitiesLoad(const CEntitiesLoad &Load, CImageInfo &ImgInfo, const char *pPath)
{
	if(ImgInfo.m_Format != CImageInfo::FORMAT_RGBA || ImgInfo.m_Width < 16 || ImgInfo.m_Height < 16 || ImgInfo.m_Width % 16 != 0 || ImgInfo.m_Height % 16 != 0)
	{
		log_error("mapimages", "Invalid entities image '%s'.", pPath);
		return false;
	}

	const int TextureLoadFlag = IGraphics::TEXLOAD_LAYERED | IGraphics::TEXLOAD_NO_2D_TEXTURE;

	CImageInfo BuildImageInfo;
	BuildImageInfo.m_Width = ImgInfo.m_Width;
	BuildImageInfo.m_Height = ImgInfo.m_Height;
	BuildImageInfo.m_Format = ImgInfo.m_Format;
	BuildImageInfo.Allocate(); // allocate already transparent image

	// convert tune tile to gray
	const size_t CopyWidth = ImgInfo.m_Width / 16;
	const size_t CopyHeight = ImgInfo.m_Height / 16;
	const size_t TuneTileX = static_cast<size_t>(TILE_TUNE % 16) * CopyWidth;
	const size_t TuneTileY = static_cast<size_t>(TILE_TUNE / 16) * CopyHeight;

	ConvertToGrayscaleRect(ImgInfo, TuneTileX, TuneTileY, CopyWidth, CopyHeight);

	// build game layer
	for(int LayerType = 0; LayerType < MAP_IMAGE_ENTITY_LAYER_TYPE_COUNT; ++LayerType)
	{
		// set everything transparent
		mem_zero(BuildImageInfo.m_pData, BuildImageInfo.DataSize());

		for(int i = 0; i < 256; ++i)
		{
			int TileIndex = i;
			if(IsValidTile(LayerType, Load.m_Masked, Load.m_ModType, TileIndex))
			{
				if(LayerType == MAP_IMAGE_ENTITY_LAYER_TYPE_SWITCH && TileIndex == TILE_SWITCHTIMEDOPEN)
				{
					TileIndex = 8;
				}

				const size_t OffsetX = (size_t)(TileIndex % 16) * CopyWidth;
				const size_t OffsetY = (size_t)(TileIndex / 16) * CopyHeight;
				BuildImageInfo.CopyRectFrom(ImgInfo, OffsetX, OffsetY, CopyWidth, CopyHeight, OffsetX, OffsetY);
			}
		}

		Graphics()->UnloadTexture(&m_aaEntitiesTextures[Load.m_EntityVariant][LayerType]);
		m_aaEntitiesTextures[Load.m_EntityVariant][LayerType] = Graphics()->LoadTextureRaw(BuildImageInfo, TextureLoadFlag, pPath);
	}

	BuildImageInfo.Free();

	// build tune map from the tune tile
	CImageInfo TuneMapInfo;
	TuneMapInfo.m_Width = ImgInfo.m_Width;
	TuneMapInfo.m_Height = ImgInfo.m_Height;
	TuneMapInfo.m_Format = ImgInfo.m_Format;
	TuneMapInfo.AllocateFillZero();

	for(int TileIndex = 1; TileIndex < 256; ++TileIndex)
	{
		size_t StartX = CopyWidth * (TileIndex % 16);
		size_t StartY = CopyHeight * (TileIndex / 16);
		TuneMapInfo.CopyRectFrom(ImgInfo, TuneTileX, TuneTileY, CopyWidth, CopyHeight, StartX, StartY);
		float Hue = std::fmod((TileIndex - 1) * normalized_golden_angle, 1.0f);
		ColorizeWithHueRect(TuneMapInfo, Hue, 0.75f, StartX, StartY, CopyWidth, CopyHeight);
	}
	Graphics()->UnloadTexture(&m_aTuneColorMapTextures[Load.m_EntityVariant]);
	m_aTuneColorMapTextures[Load.m_EntityVariant] = Graphics()->LoadTextureRawMove(TuneMapInfo, TextureLoadFlag);
	m_aTuneColorsIsLoaded[Load.m_EntityVariant] = true;
	return true;
}

IGraphics::CTextureHandle CMapImages::GetSpeedupArrow()
{
	if(!m_SpeedupArrowIsLoaded)
	{
		m_SpeedupArrowIsLoaded = true;
		m_SpeedupArrowResource = GameClient()->AssetLoader().LoadImageFile(Storage(), "editor/speed_arrow_array.png", IStorage::TYPE_ALL);
	}
	return m_SpeedupArrowTexture;
}

IGraphics::CTextureHandle CMapImages::GetTuneColors()
{
	const CGameInfo &GameInfo = GameClient()->FocusedGameInfo();
	return GetTuneColors(GetEntitiesModType(GameInfo), !GameInfo.m_DontMaskEntities);
}

IGraphics::CTextureHandle CMapImages::GetTuneColors(EMapImageModType EntitiesModType, bool EntitiesAreMasked)
{
	const int EntityVariant = MapImageEntityVariant(EntitiesModType, EntitiesAreMasked);
	// loading the entities also loads the tune map
	if(!m_aTuneColorsIsLoaded[EntityVariant])
		GetEntities(EMapImageEntityLayerType::MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH, EntitiesModType, EntitiesAreMasked);
	return m_aTuneColorMapTextures[EntityVariant];
}

IGraphics::CTextureHandle CMapImages::GetOverlayBottom()
{
	return m_OverlayBottomTexture;
}

IGraphics::CTextureHandle CMapImages::GetOverlayTop()
{
	return m_OverlayTopTexture;
}

IGraphics::CTextureHandle CMapImages::GetOverlayCenter()
{
	return m_OverlayCenterTexture;
}

void CMapImages::ChangeEntitiesPath(const char *pPath)
{
	m_vEntitiesLoads.clear();
	if(m_SpeedupArrowResource)
	{
		m_SpeedupArrowResource.Reset();
		m_SpeedupArrowIsLoaded = false;
	}
	if(str_comp(pPath, "default") == 0)
		str_copy(m_aEntitiesPath, "editor/entities_clear");
	else
	{
		str_format(m_aEntitiesPath, sizeof(m_aEntitiesPath), "assets/entities/%s", pPath);
	}

	// The old textures are replaced once the new ones are loaded
	std::fill(std::begin(m_aEntitiesIsLoaded), std::end(m_aEntitiesIsLoaded), false);
	std::fill(std::begin(m_aTuneColorsIsLoaded), std::end(m_aTuneColorsIsLoaded), false);
}

void CMapImages::ConchainClTextEntitiesSize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CMapImages *pThis = static_cast<CMapImages *>(pUserData);
		pThis->SetTextureScale(g_Config.m_ClTextEntitiesSize);
	}
}

void CMapImages::SetTextureScale(int Scale)
{
	if(m_TextureScale == Scale)
		return;

	m_TextureScale = Scale;

	if(Graphics() && m_OverlayCenterTexture.IsValid()) // check if component was initialized
	{
		// reinitialize component
		Graphics()->UnloadTexture(&m_OverlayBottomTexture);
		Graphics()->UnloadTexture(&m_OverlayTopTexture);
		Graphics()->UnloadTexture(&m_OverlayCenterTexture);

		InitOverlayTextures();
	}
}

int CMapImages::GetTextureScale() const
{
	return m_TextureScale;
}

IGraphics::CTextureHandle CMapImages::UploadEntityLayerText(int TextureSize, int MaxWidth, int YOffset)
{
	CImageInfo TextImage;
	TextImage.m_Width = 1024;
	TextImage.m_Height = 1024;
	TextImage.m_Format = CImageInfo::FORMAT_RGBA;
	TextImage.AllocateFillZero();

	UpdateEntityLayerText(TextImage, TextureSize, MaxWidth, YOffset, 0);
	UpdateEntityLayerText(TextImage, TextureSize, MaxWidth, YOffset, 1);
	UpdateEntityLayerText(TextImage, TextureSize, MaxWidth, YOffset, 2, 255);

	const int TextureLoadFlag = IGraphics::TEXLOAD_LAYERED | IGraphics::TEXLOAD_NO_2D_TEXTURE;
	return Graphics()->LoadTextureRawMove(TextImage, TextureLoadFlag);
}

void CMapImages::UpdateEntityLayerText(CImageInfo &TextImage, int TextureSize, int MaxWidth, int YOffset, int NumbersPower, int MaxNumber)
{
	char aBuf[4];
	int DigitsCount = NumbersPower + 1;

	int CurrentNumber = std::pow(10, NumbersPower);

	if(MaxNumber == -1)
		MaxNumber = CurrentNumber * 10 - 1;

	str_format(aBuf, sizeof(aBuf), "%d", CurrentNumber);

	int CurrentNumberSuitableFontSize = TextRender()->AdjustFontSize(aBuf, DigitsCount, TextureSize, MaxWidth);
	int UniversalSuitableFontSize = CurrentNumberSuitableFontSize * 0.92f; // should be smoothed enough to fit any digits combination

	YOffset += ((TextureSize - UniversalSuitableFontSize) / 2);

	for(; CurrentNumber <= MaxNumber; ++CurrentNumber)
	{
		str_format(aBuf, sizeof(aBuf), "%d", CurrentNumber);

		float x = (CurrentNumber % 16) * 64;
		float y = (CurrentNumber / 16) * 64;

		int ApproximateTextWidth = TextRender()->CalculateTextWidth(aBuf, DigitsCount, 0, UniversalSuitableFontSize);
		int XOffSet = (MaxWidth - std::clamp(ApproximateTextWidth, 0, MaxWidth)) / 2;

		TextRender()->UploadEntityLayerText(TextImage, (TextImage.m_Width / 16) - XOffSet, (TextImage.m_Height / 16) - YOffset, aBuf, DigitsCount, x + XOffSet, y + YOffset, UniversalSuitableFontSize);
	}
}

void CMapImages::InitOverlayTextures()
{
	int TextureSize = 64 * m_TextureScale / 100;
	TextureSize = std::clamp(TextureSize, 2, 64);
	int TextureToVerticalCenterOffset = (64 - TextureSize) / 2 + TextureSize * 0.1f; // should be used to move texture to the center of 64 pixels area

	if(!m_OverlayBottomTexture.IsValid())
	{
		m_OverlayBottomTexture = UploadEntityLayerText(TextureSize / 2, 64, 32 + TextureToVerticalCenterOffset / 2);
	}

	if(!m_OverlayTopTexture.IsValid())
	{
		m_OverlayTopTexture = UploadEntityLayerText(TextureSize / 2, 64, TextureToVerticalCenterOffset / 2);
	}

	if(!m_OverlayCenterTexture.IsValid())
	{
		m_OverlayCenterTexture = UploadEntityLayerText(TextureSize, 64, TextureToVerticalCenterOffset);
	}
}
