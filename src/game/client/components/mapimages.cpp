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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace
{
	// The numbers are drawn into an image of 16 by 16 tiles
	constexpr int NUMBERS_TILE_SIZE = 64;
	constexpr int NUMBERS_IMAGE_SIZE = 16 * NUMBERS_TILE_SIZE;
	constexpr const char *DIGITS = "0123456789";
} // namespace

/**
 * Draws the numbers 1 to 255 into the images that the numbers over the tele,
 * speedup and switch tiles are drawn from (see `IMapImages::GetOverlayCenter`).
 * The text renderer rasterizes the ten digits beforehand on the main thread,
 * once per font size, and the numbers are put together from them here: a
 * digit goes to the top of the number without its bearing, one pixel after
 * the one before it, and is cut off at the edge of the number's tile.
 */
class CEntityNumbersJob final : public CAssetJob
{
public:
	class CDigits
	{
	public:
		// Of `DIGITS`
		std::vector<std::optional<CCharacterBitmap>> m_vBitmaps;
		// What `ITextRender::CalculateTextWidth` says for each
		int m_aWidths[10] = {};
	};

	// The numbers with the same count of digits in one of the images
	class CNumbers
	{
	public:
		// Below 1 for none
		int m_FontSize = 0;
		int m_OffsetY = 0;
		const CDigits *m_pDigits = nullptr;
	};

	class CImage
	{
	public:
		// With one, two and three digits
		CNumbers m_aNumbers[3];
		CImageInfo m_Image;
	};

	CEntityNumbersJob() :
		CAssetJob("entity numbers")
	{
	}

	/**
	 * Prepares an image on the main thread.
	 *
	 * @param TextRender Rasterizes the digits.
	 * @param Image The image.
	 * @param MaxFontSize The size of the numbers.
	 * @param OffsetY Where in a tile the numbers are, from its top.
	 */
	void Prepare(ITextRender &TextRender, CImage &Image, int MaxFontSize, int OffsetY)
	{
		for(int Power = 0; Power < 3; ++Power)
		{
			char aFirst[4];
			str_format(aFirst, sizeof(aFirst), "%d", (int)std::pow(10, Power));
			const int FittingFontSize = TextRender.AdjustFontSize(aFirst, Power + 1, MaxFontSize, NUMBERS_TILE_SIZE);
			CNumbers &Numbers = Image.m_aNumbers[Power];
			Numbers.m_FontSize = FittingFontSize * 0.92f; // should be smoothed enough to fit any digits combination
			Numbers.m_OffsetY = OffsetY + (MaxFontSize - Numbers.m_FontSize) / 2;
			if(Numbers.m_FontSize < 1)
				continue;
			const auto [It, Inserted] = m_Digits.try_emplace(Numbers.m_FontSize);
			Numbers.m_pDigits = &It->second;
			if(!Inserted)
				continue;
			TextRender.RasterizeCharacters(DIGITS, Numbers.m_FontSize, It->second.m_vBitmaps);
			for(int Digit = 0; Digit < 10; ++Digit)
				It->second.m_aWidths[Digit] = TextRender.CalculateTextWidth(&DIGITS[Digit], 1, 0, Numbers.m_FontSize);
		}
	}

	CImage m_aImages[CMapImages::NUM_OVERLAYS];

protected:
	bool Process() override
	{
		for(CImage &Image : m_aImages)
		{
			Image.m_Image.m_Width = NUMBERS_IMAGE_SIZE;
			Image.m_Image.m_Height = NUMBERS_IMAGE_SIZE;
			Image.m_Image.m_Format = CImageInfo::FORMAT_RGBA;
			Image.m_Image.AllocateFillZero();
			for(int Power = 0; Power < 3; ++Power)
			{
				const CNumbers &Numbers = Image.m_aNumbers[Power];
				if(Numbers.m_FontSize < 1)
					continue;
				const int First = std::pow(10, Power);
				const int Last = std::min(First * 10 - 1, 255);
				for(int Number = First; Number <= Last; ++Number)
					DrawNumber(Image.m_Image, Numbers, Number, Power + 1);
			}
		}
		return true;
	}

private:
	std::map<int, CDigits> m_Digits;

	static void DrawNumber(CImageInfo &Image, const CNumbers &Numbers, int Number, int NumDigits)
	{
		char aNumber[4];
		str_format(aNumber, sizeof(aNumber), "%d", Number);

		int TextWidth = 0;
		for(int i = 0; i < NumDigits; ++i)
			TextWidth += Numbers.m_pDigits->m_aWidths[aNumber[i] - '0'];
		const int OffsetX = (NUMBERS_TILE_SIZE - std::clamp(TextWidth, 0, NUMBERS_TILE_SIZE)) / 2;

		const float X = (Number % 16) * NUMBERS_TILE_SIZE + OffsetX;
		const float Y = (Number / 16) * NUMBERS_TILE_SIZE + Numbers.m_OffsetY;
		const int TileRestWidth = NUMBERS_TILE_SIZE - OffsetX;
		const int TileRestHeight = NUMBERS_TILE_SIZE - Numbers.m_OffsetY;
		const size_t PixelSize = Image.PixelSize();
		int DigitX = 0;
		for(int i = 0; i < NumDigits; ++i)
		{
			const std::optional<CCharacterBitmap> &Digit = Numbers.m_pDigits->m_vBitmaps[aNumber[i] - '0'];
			if(!Digit)
				continue;
			for(int OffY = 0; OffY < Digit->m_Height; ++OffY)
			{
				for(int OffX = 0; OffX < Digit->m_Width; ++OffX)
				{
					const int ImageX = std::clamp(X + OffX + DigitX, X, (X + TileRestWidth) - 1);
					const int ImageY = std::clamp(Y + OffY, Y, (Y + TileRestHeight) - 1);
					uint8_t *pPixel = &Image.m_pData[ImageY * (Image.m_Width * PixelSize) + ImageX * PixelSize];
					for(size_t Channel = 0; Channel < PixelSize - 1; ++Channel)
						pPixel[Channel] = 255;
					pPixel[PixelSize - 1] = Digit->m_vCoverage[OffY * Digit->m_Width + OffX];
				}
			}
			DigitX += Digit->m_Width + 1;
		}
	}
};

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
	FinishOverlayTextures();
}

void CMapImages::OnMapLoad()
{
	// Made ahead of the first frame of a game that draws them
	const CLayers *pLayers = Layers();
	if(g_Config.m_ClOverlayEntities && g_Config.m_ClTextEntities && pLayers != nullptr &&
		(pLayers->TeleLayer() != nullptr || pLayers->SpeedupLayer() != nullptr || pLayers->SwitchLayer() != nullptr))
		RequestOverlayTextures();
}

void CMapImages::OnCollectCriticalAssets(CFirstFrameGate::EScene Scene, CSessionId SessionId, CCriticalAssets &Pending) const
{
	// Made because the game draws them, see OnMapLoad
	if(Scene == CFirstFrameGate::EScene::GAME && m_OverlayResource)
		Pending.Add(Localize("entity numbers"));
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
	m_OverlayResource.Reset();
	for(auto &Texture : m_aOverlayTextures)
		Graphics()->UnloadTexture(&Texture);
	m_OverlayScale = 0;
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

bool CMapRenderImages::Update()
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
	return !m_vImageLoads.empty();
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

	// A map that Teeworlds 0.7 wrote means the 0.7 pictures of the images 0.7
	// drew again, and so does every map a 0.7 server sends
	const bool Teeworlds07Pictures = Sixup || IsTeeworlds07Map(pMap);

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

		const CImageInfo::EImageFormat Format = MapImageFormat(pImg);
		if(Format == CImageInfo::FORMAT_UNDEFINED)
		{
			log_error("mapimages", "Failed to load map image %d '%s': invalid map image type.", i, pName);
			ShowWarning = true;
			continue;
		}

		if(pImg->m_External)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			const bool Translated = Teeworlds07Pictures && IsMapImageRedrawnFor07(pName);
			str_format(aPath, sizeof(aPath), "mapres/%s%s.png", pName, Translated ? "_0.7" : "");
			m_vImageLoads.push_back({i, LoadFlag, GameClient()->AssetLoader().LoadImageFile(Storage(), aPath, IStorage::TYPE_ALL, {}, EAssetPriority::URGENT)});
		}
		else
		{
			if(pImg->m_Width <= 0 || pImg->m_Height <= 0)
			{
				log_error("mapimages", "Failed to load map image %d '%s': invalid image dimensions.", i, pName);
				ShowWarning = true;
				continue;
			}

			const size_t DataSize = (size_t)pImg->m_Width * pImg->m_Height * CImageInfo::PixelSize(Format);
			CDataFileRawData RawData;
			if(!pMap->GetRawData(pImg->m_ImageData, RawData) || RawData.UncompressedSize() < DataSize)
			{
				log_error("mapimages", "Failed to load map image %d: failed to load data.", i);
				ShowWarning = true;
				continue;
			}
			char aTexName[IO_MAX_PATH_LENGTH];
			str_format(aTexName, sizeof(aTexName), "embedded: %s", pName);
			// Teeworlds 0.7 maps can embed RGB pictures. Textures are RGBA, and
			// the job that uncompresses the picture converts it as well.
			std::function<bool(CImageInfo &)> Postprocess;
			if(Format != CImageInfo::FORMAT_RGBA)
				Postprocess = [](CImageInfo &Image) {
					ConvertToRgba(Image);
					return true;
				};
			m_vImageLoads.push_back({i, LoadFlag, GameClient()->AssetLoader().LoadImageRawData(std::move(RawData), pImg->m_Width, pImg->m_Height, Format, aTexName, std::move(Postprocess), EAssetPriority::URGENT)});
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
	RequestOverlayTextures();
	return m_aOverlayTextures[OVERLAY_BOTTOM];
}

IGraphics::CTextureHandle CMapImages::GetOverlayTop()
{
	RequestOverlayTextures();
	return m_aOverlayTextures[OVERLAY_TOP];
}

IGraphics::CTextureHandle CMapImages::GetOverlayCenter()
{
	RequestOverlayTextures();
	return m_aOverlayTextures[OVERLAY_CENTER];
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
	// The textures are made again when they are next drawn
	m_TextureScale = Scale;
}

int CMapImages::GetTextureScale() const
{
	return m_TextureScale;
}

void CMapImages::RequestOverlayTextures()
{
	if(m_OverlayScale == m_TextureScale)
		return;
	m_OverlayScale = m_TextureScale;

	const int TextureSize = std::clamp(NUMBERS_TILE_SIZE * m_TextureScale / 100, 2, NUMBERS_TILE_SIZE);
	const int CenterOffset = (NUMBERS_TILE_SIZE - TextureSize) / 2 + TextureSize * 0.1f; // moves the numbers to the middle of the tile
	auto pJob = std::make_shared<CEntityNumbersJob>();
	pJob->Prepare(*TextRender(), pJob->m_aImages[OVERLAY_BOTTOM], TextureSize / 2, NUMBERS_TILE_SIZE / 2 + CenterOffset / 2);
	pJob->Prepare(*TextRender(), pJob->m_aImages[OVERLAY_TOP], TextureSize / 2, CenterOffset / 2);
	pJob->Prepare(*TextRender(), pJob->m_aImages[OVERLAY_CENTER], TextureSize, CenterOffset);
	// What is drawn next waits for it
	m_OverlayResource = GameClient()->AssetLoader().Load(std::move(pJob), EAssetPriority::URGENT);
}

void CMapImages::FinishOverlayTextures()
{
	if(!m_OverlayResource.IsFinished())
		return;
	if(m_OverlayResource.IsReady())
	{
		for(int Overlay = 0; Overlay < NUM_OVERLAYS; ++Overlay)
		{
			Graphics()->UnloadTexture(&m_aOverlayTextures[Overlay]);
			m_aOverlayTextures[Overlay] = Graphics()->LoadTextureRawMove(m_OverlayResource.Result().m_aImages[Overlay].m_Image, IGraphics::TEXLOAD_LAYERED | IGraphics::TEXLOAD_NO_2D_TEXTURE, m_OverlayResource.Path());
		}
	}
	else
	{
		log_error("mapimages", "Failed to make the entity numbers.");
	}
	m_OverlayResource.Reset();
}
