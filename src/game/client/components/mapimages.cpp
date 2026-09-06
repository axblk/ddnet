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
#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace
{
	std::atomic<int> gs_NextMapImageAssetOwner{1000};
}

CMapImages::CMapImages() :
	m_AssetOwnerId(gs_NextMapImageAssetOwner.fetch_add(1, std::memory_order_relaxed)),
	m_EntitiesAssetOwnerId(gs_NextMapImageAssetOwner.fetch_add(1, std::memory_order_relaxed))
{
	m_Count = 0;
	std::fill(std::begin(m_aEntitiesIsLoaded), std::end(m_aEntitiesIsLoaded), false);
	m_SpeedupArrowIsLoaded = false;
	m_TuneColorsIsLoaded = false;

	str_copy(m_aEntitiesPath, "editor/entities_clear");

	static_assert(std::size(gs_apModEntitiesNames) == MAP_IMAGE_MOD_TYPE_COUNT, "Mod name string count is not equal to mod type count");
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
	Update();
}

void CMapImages::OnShutdown()
{
	++m_AssetGeneration;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(m_EntitiesAssetOwnerId, m_AssetGeneration);
	m_vEntitiesLoads.clear();
	m_SpeedupArrowResource.Reset();
	for(auto &aEntitiesTextures : m_aaEntitiesTextures)
	{
		for(auto &Texture : aEntitiesTextures)
			Graphics()->UnloadTexture(&Texture);
	}
	Graphics()->UnloadTexture(&m_TuneColorMapTexture);
	Graphics()->UnloadTexture(&m_SpeedupArrowTexture);
	Unload();
}

void CMapImages::Update()
{
	FinishImageLoads();
	FinishEntitiesLoads();
}

void CMapImages::FinishImageLoads()
{
	bool ShowWarning = false;
	for(auto It = m_vImageLoads.begin(); It != m_vImageLoads.end();)
	{
		if(!It->m_Resource.IsFinished())
		{
			++It;
			continue;
		}
		if(It->m_Resource.IsReady(m_Generation))
		{
			CImageInfo Image = It->m_Resource.TakeImage();
			m_aTextures[It->m_Index] = Graphics()->LoadTextureRawMove(Image, It->m_LoadFlags, It->m_Resource.Path());
			ShowWarning = ShowWarning || !m_aTextures[It->m_Index].IsValid() || m_aTextures[It->m_Index].IsNullTexture();
		}
		else if(It->m_Resource.IsFailed(m_Generation))
		{
			log_error("mapimages", "Failed to load map image '%s'.", It->m_Resource.Path());
			ShowWarning = true;
		}
		It = m_vImageLoads.erase(It);
	}
	if(ShowWarning)
		Client()->AddWarning(SWarning(Localize("Some map images could not be loaded. Check the local console for details.")));
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
			if(!Resource.IsReady(m_AssetGeneration))
				continue;
			CImageInfo Image = Resource.TakeImage();
			if(FinishEntitiesLoad(*It, std::move(Image), Resource.Path()))
			{
				Loaded = true;
				break;
			}
		}
		if(!Loaded)
			log_error("mapimages", "Failed to load entities image for '%s'.", gs_apModEntitiesNames[It->m_ModType]);
		It = m_vEntitiesLoads.erase(It);
	}

	if(m_SpeedupArrowResource.IsFinished())
	{
		if(m_SpeedupArrowResource.IsReady(m_AssetGeneration))
		{
			CImageInfo Image = m_SpeedupArrowResource.TakeImage();
			const int TextureLoadFlag = Graphics()->TextureLoadFlags() | IGraphics::TEXLOAD_NO_2D_TEXTURE;
			IGraphics::CTextureHandle Texture = Graphics()->LoadTextureRawMove(Image, TextureLoadFlag, m_SpeedupArrowResource.Path());
			if(Texture.IsValid())
			{
				Graphics()->UnloadTexture(&m_SpeedupArrowTexture);
				m_SpeedupArrowTexture = Texture;
			}
			else
			{
				log_error("mapimages", "Failed to upload speedup arrow texture");
			}
		}
		else if(m_SpeedupArrowResource.IsFailed(m_AssetGeneration))
			log_error("mapimages", "Failed to load speedup arrow texture from '%s'", m_SpeedupArrowResource.Path());
		m_SpeedupArrowResource.Reset();
	}
}

void CMapImages::Unload()
{
	++m_Generation;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(m_AssetOwnerId, m_Generation);
	m_vImageLoads.clear();
	// unload all textures
	for(int i = 0; i < m_Count; i++)
	{
		Graphics()->UnloadTexture(&m_aTextures[i]);
	}
}

void CMapImages::OnMapLoadImpl(class CLayers *pLayers, IMap *pMap)
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

		const int LoadFlag = (((aTextureUsedByTileOrQuadLayerFlag[i] & 1) != 0) ? Graphics()->TextureLoadFlags() : 0) | (((aTextureUsedByTileOrQuadLayerFlag[i] & 2) != 0) ? 0 : (Graphics()->HasTextureArraysSupport() ? IGraphics::TEXLOAD_NO_2D_TEXTURE : 0));
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
			if(Client()->IsSixup())
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
			m_vImageLoads.push_back({i, LoadFlag, GameClient()->AssetLoader().LoadImageFile(Storage(), aPath, IStorage::TYPE_ALL, m_AssetOwnerId, m_Generation)});
		}
		else
		{
			if(pImg->m_Width <= 0 || pImg->m_Height <= 0)
			{
				log_error("mapimages", "Failed to load map image %d '%s': invalid image dimensions.", i, pName);
				ShowWarning = true;
				continue;
			}

			// The data of embedded images is uncompressed and uploaded later,
			// only the compressed bytes are copied from the map file here.
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
			m_vImageLoads.push_back({i, LoadFlag, GameClient()->AssetLoader().LoadImageRawData(std::move(RawData), pImg->m_Width, pImg->m_Height, CImageInfo::FORMAT_RGBA, aTexName, m_AssetOwnerId, m_Generation)});
		}
		pMap->UnloadData(pImg->m_ImageName);
	}
	if(ShowWarning)
	{
		Client()->AddWarning(SWarning(Localize("Some map images could not be loaded. Check the local console for details.")));
	}
}

void CMapImages::OnMapLoad()
{
	OnMapLoadImpl(GameClient()->Layers(), GameClient()->Map());
}

void CMapImages::LoadBackground(class CLayers *pLayers, class IMap *pMap)
{
	OnMapLoadImpl(pLayers, pMap);
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
	const bool EntitiesAreMasked = !GameClient()->m_GameInfo.m_DontMaskEntities;
	const EMapImageModType EntitiesModType = GetEntitiesModType(GameClient()->m_GameInfo);
	const int EntityVariant = MapImageEntityVariant(EntitiesModType, EntitiesAreMasked);

	if(!m_aEntitiesIsLoaded[EntityVariant])
	{
		m_aEntitiesIsLoaded[EntityVariant] = true;
		CEntitiesLoad Load{EntityVariant, EntitiesModType, EntitiesAreMasked, {}};
		// The candidates are loaded together and the first one that decodes
		// wins. With the default entities path the last candidate repeats the
		// first, so decoding the same file twice is skipped.
		std::vector<std::string> vSubmittedPaths;
		const auto Submit = [&](const char *pPath) {
			if(std::any_of(vSubmittedPaths.begin(), vSubmittedPaths.end(), [pPath](const std::string &Submitted) { return Submitted == pPath; }))
				return;
			vSubmittedPaths.emplace_back(pPath);
			Load.m_vResources.push_back(GameClient()->AssetLoader().LoadImageFile(Storage(), pPath, IStorage::TYPE_ALL, m_EntitiesAssetOwnerId, m_AssetGeneration));
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
		str_format(aPath, sizeof(aPath), "editor/entities_clear/%s.png", gs_apModEntitiesNames[EntitiesModType]);
		Submit(aPath);
		m_vEntitiesLoads.push_back(std::move(Load));
	}

	return m_aaEntitiesTextures[EntityVariant][EntityLayerType];
}

bool CMapImages::FinishEntitiesLoad(CEntitiesLoad &Load, CImageInfo ImgInfo, const char *pPath)
{
	if(ImgInfo.m_Format != CImageInfo::FORMAT_RGBA || ImgInfo.m_Width < 16 || ImgInfo.m_Height < 16 || ImgInfo.m_Width % 16 != 0 || ImgInfo.m_Height % 16 != 0)
	{
		log_error("mapimages", "Invalid entities image '%s'.", pPath);
		return false;
	}

	const int TextureLoadFlag = Graphics()->HasTextureArraysSupport() ? Graphics()->TextureLoadFlags() | IGraphics::TEXLOAD_NO_2D_TEXTURE : 0;
	CImageInfo BuildImageInfo;
	BuildImageInfo.m_Width = ImgInfo.m_Width;
	BuildImageInfo.m_Height = ImgInfo.m_Height;
	BuildImageInfo.m_Format = ImgInfo.m_Format;
	if(!BuildImageInfo.TryAllocate())
	{
		log_error("mapimages", "Failed to allocate entities image '%s'.", pPath);
		return false;
	}
	IGraphics::CTextureHandle aNewTextures[MAP_IMAGE_ENTITY_LAYER_TYPE_COUNT];
	IGraphics::CTextureHandle NewTuneColorMapTexture;
	const auto UnloadNewTextures = [&]() {
		for(auto &Texture : aNewTextures)
			Graphics()->UnloadTexture(&Texture);
		Graphics()->UnloadTexture(&NewTuneColorMapTexture);
	};

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
		for(int TileIndex = 0; TileIndex < 256; ++TileIndex)
		{
			int SourceTileIndex = TileIndex;
			if(!IsValidTile(LayerType, Load.m_Masked, Load.m_ModType, SourceTileIndex))
				continue;
			if(LayerType == MAP_IMAGE_ENTITY_LAYER_TYPE_SWITCH && SourceTileIndex == TILE_SWITCHTIMEDOPEN)
				SourceTileIndex = 8;
			const size_t OffsetX = static_cast<size_t>(SourceTileIndex % 16) * CopyWidth;
			const size_t OffsetY = static_cast<size_t>(SourceTileIndex / 16) * CopyHeight;
			BuildImageInfo.CopyRectFrom(ImgInfo, OffsetX, OffsetY, CopyWidth, CopyHeight, OffsetX, OffsetY);
		}
		aNewTextures[LayerType] = Graphics()->LoadTextureRaw(BuildImageInfo, TextureLoadFlag, pPath);
		if(!aNewTextures[LayerType].IsValid())
		{
			UnloadNewTextures();
			log_error("mapimages", "Failed to upload entities image '%s'.", pPath);
			return false;
		}
	}

	// build tune map from the tune tile
	if(Graphics()->HasTextureArraysSupport())
	{
		CImageInfo TuneMapInfo;
		TuneMapInfo.m_Width = ImgInfo.m_Width;
		TuneMapInfo.m_Height = ImgInfo.m_Height;
		TuneMapInfo.m_Format = ImgInfo.m_Format;
		if(!TuneMapInfo.TryAllocate())
		{
			UnloadNewTextures();
			log_error("mapimages", "Failed to allocate tune color image '%s'.", pPath);
			return false;
		}
		mem_zero(TuneMapInfo.m_pData, TuneMapInfo.DataSize());
		for(int TileIndex = 1; TileIndex < 256; ++TileIndex)
		{
			const size_t StartX = CopyWidth * (TileIndex % 16);
			const size_t StartY = CopyHeight * (TileIndex / 16);
			TuneMapInfo.CopyRectFrom(ImgInfo, TuneTileX, TuneTileY, CopyWidth, CopyHeight, StartX, StartY);
			const float Hue = std::fmod((TileIndex - 1) * normalized_golden_angle, 1.0f);
			ColorizeWithHueRect(TuneMapInfo, Hue, 0.75f, StartX, StartY, CopyWidth, CopyHeight);
		}
		NewTuneColorMapTexture = Graphics()->LoadTextureRawMove(TuneMapInfo, TextureLoadFlag);
		if(!NewTuneColorMapTexture.IsValid())
		{
			UnloadNewTextures();
			log_error("mapimages", "Failed to upload tune color image '%s'.", pPath);
			return false;
		}
	}

	// commit, replacing the textures a frame may still be using
	for(int LayerType = 0; LayerType < MAP_IMAGE_ENTITY_LAYER_TYPE_COUNT; ++LayerType)
	{
		Graphics()->UnloadTexture(&m_aaEntitiesTextures[Load.m_EntityVariant][LayerType]);
		m_aaEntitiesTextures[Load.m_EntityVariant][LayerType] = aNewTextures[LayerType];
	}
	if(Graphics()->HasTextureArraysSupport())
	{
		Graphics()->UnloadTexture(&m_TuneColorMapTexture);
		m_TuneColorMapTexture = NewTuneColorMapTexture;
		m_TuneColorsIsLoaded = true;
	}
	return true;
}

IGraphics::CTextureHandle CMapImages::GetSpeedupArrow()
{
	if(!m_SpeedupArrowIsLoaded)
	{
		m_SpeedupArrowIsLoaded = true;
		m_SpeedupArrowResource = GameClient()->AssetLoader().LoadImageFile(Storage(), "editor/speed_arrow_array.png", IStorage::TYPE_ALL, m_EntitiesAssetOwnerId, m_AssetGeneration);
	}
	return m_SpeedupArrowTexture;
}

IGraphics::CTextureHandle CMapImages::GetTuneColors()
{
	if(Graphics()->HasTextureArraysSupport())
	{
		if(!m_TuneColorsIsLoaded)
		{
			// load entities, this also loads the tune map
			GetEntities(EMapImageEntityLayerType::MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH);
		}
		return m_TuneColorMapTexture;
	}
	else
	{
		return GetEntities(MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH);
	}
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
	++m_AssetGeneration;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(m_EntitiesAssetOwnerId, m_AssetGeneration);
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

	// The old textures stay until the replacements have loaded, so a frame
	// that is already using them keeps drawing.
	std::fill(std::begin(m_aEntitiesIsLoaded), std::end(m_aEntitiesIsLoaded), false);
	m_TuneColorsIsLoaded = false;
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

	const int TextureLoadFlag = Graphics()->TextureLoadFlags() | IGraphics::TEXLOAD_NO_2D_TEXTURE;
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
