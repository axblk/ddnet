/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "skins.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <generated/client_data.h>

#include <game/client/gameclient.h>
#include <game/localization.h>

#include <algorithm>
#include <optional>

using namespace std::chrono_literals;

namespace
{
	constexpr int ASSET_OWNER_SKINS = 3;

	bool AllSkinTexturesValid(const CSkin::CSkinTextures &Textures)
	{
		if(!Textures.m_Body.IsValid() || !Textures.m_BodyOutline.IsValid() ||
			!Textures.m_Feet.IsValid() || !Textures.m_FeetOutline.IsValid() ||
			!Textures.m_Hands.IsValid() || !Textures.m_HandsOutline.IsValid())
		{
			return false;
		}
		return std::ranges::all_of(Textures.m_aEyes, [](const auto &Eye) { return Eye.IsValid(); });
	}
}

CSkins::CSkinContainer::CSkinContainer(CSkins *pSkins, const char *pName, EType Type, int StorageType) :
	m_pSkins(pSkins),
	m_Type(Type),
	m_StorageType(StorageType)
{
	str_copy(m_aName, pName);
	m_Vanilla = IsVanillaSkin(m_aName);
	m_Special = IsSpecialSkin(m_aName);
	m_AlwaysLoaded = m_Vanilla; // Vanilla skins are loaded immediately and not unloaded
}

CSkins::CSkinContainer::~CSkinContainer()
{
	if(m_LoadResource)
	{
		m_LoadResource.Abort();
	}
	if(m_pDownloadRequest)
	{
		m_pDownloadRequest->Abort();
	}
}

bool CSkins::CSkinContainer::operator<(const CSkinContainer &Other) const
{
	return str_comp(m_aName, Other.m_aName) < 0;
}

static constexpr std::chrono::nanoseconds MIN_REQUESTED_TIME_FOR_PENDING = 250ms;
static constexpr std::chrono::nanoseconds MAX_REQUESTED_TIME_FOR_PENDING = 500ms;
static constexpr std::chrono::nanoseconds MIN_UNLOAD_TIME_PENDING = 1s;
static constexpr std::chrono::nanoseconds MIN_UNLOAD_TIME_LOADED = 2s;
// Reading and decoding runs on the job pool, so the number that may be in
// flight is a question of how many decoded images we are willing to hold, not
// of how much work the client can take. Uploading them is what touches the main
// thread, and that is already limited by the time budget in UpdateFinishLoading.
static constexpr size_t MAX_CONCURRENT_SKIN_LOADS = 16;
static_assert(MIN_REQUESTED_TIME_FOR_PENDING < MAX_REQUESTED_TIME_FOR_PENDING);
static_assert(MIN_REQUESTED_TIME_FOR_PENDING < MIN_UNLOAD_TIME_PENDING, "Unloading pending skins must take longer than adding more pending skins");

void CSkins::CSkinContainer::RequestLoad()
{
	if(m_AlwaysLoaded)
	{
		return;
	}

	// Delay loading skins a bit after the load has been requested to avoid loading a lot of skins
	// when quickly scrolling through lists or if a player with a new skin quickly joins and leaves.
	if(m_State == EState::UNLOADED)
	{
		const std::chrono::nanoseconds Now = time_get_nanoseconds();
		if(!m_FirstLoadRequest.has_value() ||
			!m_LastLoadRequest.has_value() ||
			Now - m_LastLoadRequest.value() > MAX_REQUESTED_TIME_FOR_PENDING)
		{
			m_FirstLoadRequest = Now;
			m_LastLoadRequest = m_FirstLoadRequest;
		}
		else if(Now - m_FirstLoadRequest.value() > MIN_REQUESTED_TIME_FOR_PENDING)
		{
			m_State = EState::PENDING;
		}
	}
	else if(m_State == EState::PENDING ||
		m_State == EState::LOADING ||
		m_State == EState::LOADED)
	{
		m_LastLoadRequest = time_get_nanoseconds();
	}

	if(m_State == EState::PENDING ||
		m_State == EState::LOADED)
	{
		if(m_UsageEntryIterator.has_value())
		{
			m_pSkins->m_SkinsUsageList.erase(m_UsageEntryIterator.value());
		}
		m_pSkins->m_SkinsUsageList.emplace_front(Name());
		m_UsageEntryIterator = m_pSkins->m_SkinsUsageList.begin();
	}
}

CSkins::CSkinContainer::EState CSkins::CSkinContainer::DetermineInitialState() const
{
	if(m_AlwaysLoaded)
	{
		// Load immediately if it should always be loaded
		return EState::PENDING;
	}
	else if((g_Config.m_ClVanillaSkinsOnly && !m_Vanilla) ||
		(m_Type == EType::DOWNLOAD && !g_Config.m_ClDownloadSkins))
	{
		// Fail immediately if it shouldn't be loaded
		return EState::NOT_FOUND;
	}
	else
	{
		return EState::UNLOADED;
	}
}

void CSkins::CSkinContainer::SetState(EState State)
{
	m_State = State;

	if(m_State == EState::PENDING ||
		m_State == EState::LOADING ||
		m_State == EState::LOADED)
	{
		RequestLoad();
	}
	else
	{
		m_FirstLoadRequest = std::nullopt;
		m_LastLoadRequest = std::nullopt;
	}

	if(m_State != EState::PENDING &&
		m_State != EState::LOADED &&
		m_UsageEntryIterator.has_value())
	{
		m_pSkins->m_SkinsUsageList.erase(m_UsageEntryIterator.value());
		m_UsageEntryIterator = std::nullopt;
	}

	m_pSkins->m_SkinList.ForceRefresh();
}

bool CSkins::CSkinListEntry::operator<(const CSkins::CSkinListEntry &Other) const
{
	if(m_Favorite && !Other.m_Favorite)
	{
		return true;
	}
	if(!m_Favorite && Other.m_Favorite)
	{
		return false;
	}
	return str_comp(m_pSkinContainer->Name(), Other.m_pSkinContainer->Name()) < 0;
}

void CSkins::CSkinListEntry::RequestLoad()
{
	m_pSkinContainer->RequestLoad();
}

CSkins::CSkins() :
	m_PlaceholderSkin("dummy")
{
	m_PlaceholderSkin.m_OriginalSkin.Reset();
	m_PlaceholderSkin.m_ColorableSkin.Reset();
	m_PlaceholderSkin.m_BloodColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	m_PlaceholderSkin.m_Metrics.m_Body.m_Width = 64;
	m_PlaceholderSkin.m_Metrics.m_Body.m_Height = 64;
	m_PlaceholderSkin.m_Metrics.m_Body.m_OffsetX = 16;
	m_PlaceholderSkin.m_Metrics.m_Body.m_OffsetY = 16;
	m_PlaceholderSkin.m_Metrics.m_Body.m_MaxWidth = 96;
	m_PlaceholderSkin.m_Metrics.m_Body.m_MaxHeight = 96;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_Width = 32;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_Height = 16;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_OffsetX = 16;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_OffsetY = 8;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_MaxWidth = 64;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_MaxHeight = 32;
}

bool CSkins::IsSpecialSkin(const char *pName)
{
	return str_utf8_comp_nocase_num(pName, "x_", 2) == 0;
}

bool CSkins::IsVanillaSkin(const char *pName)
{
	return std::any_of(std::begin(VANILLA_SKINS), std::end(VANILLA_SKINS), [pName](const char *pVanillaSkin) {
		return str_comp(pName, pVanillaSkin) == 0;
	});
}

class CSkinScanUser
{
public:
	CSkins *m_pThis;
	CSkins::TSkinLoadedCallback m_SkinLoadedCallback;
};

int CSkins::SkinScan(const char *pName, int IsDir, int StorageType, void *pUser)
{
	auto *pUserReal = static_cast<CSkinScanUser *>(pUser);
	CSkins *pSelf = pUserReal->m_pThis;

	if(IsDir)
	{
		return 0;
	}

	const char *pSuffix = str_endswith(pName, ".png");
	if(pSuffix == nullptr)
	{
		return 0;
	}

	char aSkinName[IO_MAX_PATH_LENGTH];
	str_truncate(aSkinName, sizeof(aSkinName), pName, pSuffix - pName);
	if(!CSkin::IsValidName(aSkinName))
	{
		log_error("skins", "Skin name is not valid: %s", aSkinName);
		log_error("skins", "%s", CSkin::m_aSkinNameRestrictions);
		return 0;
	}

	CSkinContainer SkinContainer(pSelf, aSkinName, CSkinContainer::EType::LOCAL, StorageType);
	auto &&pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
	pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
	pSelf->m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)});
	pUserReal->m_SkinLoadedCallback();
	return 0;
}

static void CheckMetrics(CSkin::CSkinMetricVariable &Metrics, const uint8_t *pImg, int ImgWidth, int ImgX, int ImgY, int CheckWidth, int CheckHeight)
{
	int MaxY = -1;
	int MinY = CheckHeight + 1;
	int MaxX = -1;
	int MinX = CheckWidth + 1;

	for(int y = 0; y < CheckHeight; y++)
	{
		for(int x = 0; x < CheckWidth; x++)
		{
			int OffsetAlpha = (y + ImgY) * ImgWidth + (x + ImgX) * 4 + 3;
			uint8_t AlphaValue = pImg[OffsetAlpha];
			if(AlphaValue > 0)
			{
				if(MaxY < y)
					MaxY = y;
				if(MinY > y)
					MinY = y;
				if(MaxX < x)
					MaxX = x;
				if(MinX > x)
					MinX = x;
			}
		}
	}

	Metrics.m_Width = std::clamp((MaxX - MinX) + 1, 1, CheckWidth);
	Metrics.m_Height = std::clamp((MaxY - MinY) + 1, 1, CheckHeight);
	Metrics.m_OffsetX = std::clamp(MinX, 0, CheckWidth - 1);
	Metrics.m_OffsetY = std::clamp(MinY, 0, CheckHeight - 1);
	Metrics.m_MaxWidth = CheckWidth;
	Metrics.m_MaxHeight = CheckHeight;
}

bool CSkins::LoadSkinData(const char *pName, CImageInfo &Info, CSkinLoadData &Data, bool LogErrors)
{
	if(Info.m_Format != CImageInfo::FORMAT_RGBA)
	{
		if(LogErrors)
			log_error("skins", "Skin format is not RGBA: %s", pName);
		return false;
	}
	const int DivX = g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx;
	const int DivY = g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy;
	const bool WidthBroken = Info.m_Width == 0 || Info.m_Width % DivX != 0;
	const bool HeightBroken = Info.m_Height == 0 || Info.m_Height % DivY != 0;
	if(WidthBroken || HeightBroken)
	{
		if(Info.m_Width == 0 || Info.m_Height == 0)
		{
			if(LogErrors)
				log_error("skins", "Skin has invalid size (w=%" PRIzu ", h=%" PRIzu "): %s", Info.m_Width, Info.m_Height, pName);
			return false;
		}
		int NewWidth;
		int NewHeight;
		if(WidthBroken)
		{
			NewWidth = std::max(HighestBit(Info.m_Width), DivX);
			NewHeight = (NewWidth / DivX) * DivY;
		}
		else
		{
			NewHeight = std::max(HighestBit(Info.m_Height), DivY);
			NewWidth = (NewHeight / DivY) * DivX;
		}
		const size_t NewDataSize = static_cast<size_t>(NewWidth) * NewHeight * Info.PixelSize();
		if(NewWidth > static_cast<int>(CImageLoader::MAX_IMAGE_DIMENSION) ||
			NewHeight > static_cast<int>(CImageLoader::MAX_IMAGE_DIMENSION) ||
			NewDataSize > CImageLoader::MAX_IMAGE_DATA_SIZE)
		{
			if(LogErrors)
				log_error("skins", "Resized skin would be too large (w=%d, h=%d, size=%" PRIzu "): %s", NewWidth, NewHeight, NewDataSize, pName);
			return false;
		}
		Data.m_OriginalWidth = Info.m_Width;
		Data.m_OriginalHeight = Info.m_Height;
		Data.m_ResizedWidth = NewWidth;
		Data.m_ResizedHeight = NewHeight;
		ResizeImage(Info, NewWidth, NewHeight);
	}
	const size_t BodyWidth = g_pData->m_aSprites[SPRITE_TEE_BODY].m_W * (Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx);
	const size_t BodyHeight = g_pData->m_aSprites[SPRITE_TEE_BODY].m_H * (Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy);
	if(BodyWidth > Info.m_Width || BodyHeight > Info.m_Height)
	{
		if(LogErrors)
			log_error("skins", "Skin size unsupported (w=%" PRIzu ", h=%" PRIzu "): %s", Info.m_Width, Info.m_Height, pName);
		return false;
	}

	int FeetGridPixelsWidth = Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_FOOT].m_pSet->m_Gridx;
	int FeetGridPixelsHeight = Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_FOOT].m_pSet->m_Gridy;
	int FeetWidth = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_W * FeetGridPixelsWidth;
	int FeetHeight = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_H * FeetGridPixelsHeight;
	int FeetOffsetX = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_X * FeetGridPixelsWidth;
	int FeetOffsetY = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_Y * FeetGridPixelsHeight;

	int FeetOutlineGridPixelsWidth = Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_pSet->m_Gridx;
	int FeetOutlineGridPixelsHeight = Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_pSet->m_Gridy;
	int FeetOutlineWidth = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_W * FeetOutlineGridPixelsWidth;
	int FeetOutlineHeight = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_H * FeetOutlineGridPixelsHeight;
	int FeetOutlineOffsetX = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_X * FeetOutlineGridPixelsWidth;
	int FeetOutlineOffsetY = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_Y * FeetOutlineGridPixelsHeight;

	int BodyOutlineGridPixelsWidth = Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_pSet->m_Gridx;
	int BodyOutlineGridPixelsHeight = Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_pSet->m_Gridy;
	int BodyOutlineWidth = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_W * BodyOutlineGridPixelsWidth;
	int BodyOutlineHeight = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_H * BodyOutlineGridPixelsHeight;
	int BodyOutlineOffsetX = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_X * BodyOutlineGridPixelsWidth;
	int BodyOutlineOffsetY = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_Y * BodyOutlineGridPixelsHeight;

	const size_t PixelStep = Info.PixelSize();
	const size_t Pitch = Info.m_Width * PixelStep;

	// dig out blood color
	{
		int64_t aColors[3] = {0};
		for(size_t y = 0; y < BodyHeight; y++)
		{
			for(size_t x = 0; x < BodyWidth; x++)
			{
				const size_t Offset = y * Pitch + x * PixelStep;
				if(Info.m_pData[Offset + 3] > 128)
				{
					for(size_t c = 0; c < 3; c++)
					{
						aColors[c] += Info.m_pData[Offset + c];
					}
				}
			}
		}
		const vec3 NormalizedColor = normalize(vec3(aColors[0], aColors[1], aColors[2]));
		Data.m_BloodColor = ColorRGBA(NormalizedColor.x, NormalizedColor.y, NormalizedColor.z);
	}

	CheckMetrics(Data.m_Metrics.m_Body, Info.m_pData, Pitch, 0, 0, BodyWidth, BodyHeight);
	CheckMetrics(Data.m_Metrics.m_Body, Info.m_pData, Pitch, BodyOutlineOffsetX, BodyOutlineOffsetY, BodyOutlineWidth, BodyOutlineHeight);
	CheckMetrics(Data.m_Metrics.m_Feet, Info.m_pData, Pitch, FeetOffsetX, FeetOffsetY, FeetWidth, FeetHeight);
	CheckMetrics(Data.m_Metrics.m_Feet, Info.m_pData, Pitch, FeetOutlineOffsetX, FeetOutlineOffsetY, FeetOutlineWidth, FeetOutlineHeight);

	Data.m_InfoGrayscale = Info.DeepCopy();
	ConvertToGrayscale(Data.m_InfoGrayscale);

	int aFreq[256] = {0};
	uint8_t OrgWeight = 1;
	uint8_t NewWeight = 192;

	// find most common non-zero frequency
	for(size_t y = 0; y < BodyHeight; y++)
	{
		for(size_t x = 0; x < BodyWidth; x++)
		{
			const size_t Offset = y * Pitch + x * PixelStep;
			if(Data.m_InfoGrayscale.m_pData[Offset + 3] > 128)
			{
				aFreq[Data.m_InfoGrayscale.m_pData[Offset]]++;
			}
		}
	}

	for(int i = 1; i < 256; i++)
	{
		if(aFreq[OrgWeight] < aFreq[i])
		{
			OrgWeight = i;
		}
	}

	// reorder
	for(size_t y = 0; y < BodyHeight; y++)
	{
		for(size_t x = 0; x < BodyWidth; x++)
		{
			const size_t Offset = y * Pitch + x * PixelStep;
			uint8_t v = Data.m_InfoGrayscale.m_pData[Offset];
			if(v <= OrgWeight)
			{
				v = (uint8_t)((v / (float)OrgWeight) * NewWeight);
			}
			else
			{
				v = (uint8_t)(((v - OrgWeight) / (float)(255 - OrgWeight)) * (255 - NewWeight) + NewWeight);
			}
			Data.m_InfoGrayscale.m_pData[Offset] = v;
			Data.m_InfoGrayscale.m_pData[Offset + 1] = v;
			Data.m_InfoGrayscale.m_pData[Offset + 2] = v;
		}
	}

	return true;
}

bool CSkins::LoadSkinFinish(CSkinContainer *pSkinContainer, const CSkinLoadData &Data)
{
	CSkin Skin{pSkinContainer->Name()};

	// people load the jankiest skins, so we ignore empty sprites
	Skin.m_OriginalSkin.m_Body = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY]);
	Skin.m_OriginalSkin.m_BodyOutline = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE]);
	Skin.m_OriginalSkin.m_Feet = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT]);
	Skin.m_OriginalSkin.m_FeetOutline = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE]);
	Skin.m_OriginalSkin.m_Hands = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND]);
	Skin.m_OriginalSkin.m_HandsOutline = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND_OUTLINE]);
	for(size_t i = 0; i < std::size(Skin.m_OriginalSkin.m_aEyes); ++i)
	{
		Skin.m_OriginalSkin.m_aEyes[i] = Graphics()->LoadSpriteTexture(Data.m_Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_EYE_NORMAL + i]);
	}

	Skin.m_ColorableSkin.m_Body = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY]);
	Skin.m_ColorableSkin.m_BodyOutline = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE]);
	Skin.m_ColorableSkin.m_Feet = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT]);
	Skin.m_ColorableSkin.m_FeetOutline = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE]);
	Skin.m_ColorableSkin.m_Hands = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND]);
	Skin.m_ColorableSkin.m_HandsOutline = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND_OUTLINE]);
	for(size_t i = 0; i < std::size(Skin.m_ColorableSkin.m_aEyes); ++i)
	{
		Skin.m_ColorableSkin.m_aEyes[i] = Graphics()->LoadSpriteTexture(Data.m_InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_EYE_NORMAL + i]);
	}

	Skin.m_Metrics = Data.m_Metrics;
	Skin.m_BloodColor = Data.m_BloodColor;
	if(!AllSkinTexturesValid(Skin.m_OriginalSkin) || !AllSkinTexturesValid(Skin.m_ColorableSkin))
	{
		log_error("skins", "Failed to upload required textures of skin '%s'", Skin.GetName());
		Skin.m_OriginalSkin.Unload(Graphics());
		Skin.m_ColorableSkin.Unload(Graphics());
		return false;
	}

	if(g_Config.m_Debug)
	{
		log_trace("skins", "Loaded skin '%s'", Skin.GetName());
	}

	auto SkinIt = m_Skins.find(pSkinContainer->Name());
	dbg_assert(SkinIt != m_Skins.end(), "LoadSkinFinish on skin '%s' which is not in m_Skins", pSkinContainer->Name());
	if(SkinIt->second->m_pSkin)
	{
		SkinIt->second->m_pSkin->m_OriginalSkin.Unload(Graphics());
		SkinIt->second->m_pSkin->m_ColorableSkin.Unload(Graphics());
	}
	SkinIt->second->m_pSkin = std::make_unique<CSkin>(std::move(Skin));
	return true;
}

void CSkins::LoadSkinDirect(const char *pName)
{
	if(m_Skins.contains(pName))
	{
		return;
	}
	CSkinContainer SkinContainer(this, pName, CSkinContainer::EType::LOCAL, IStorage::TYPE_ALL);
	auto &&pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
	pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
	const auto &[SkinIt, _] = m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)});

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "skins/%s.png", pName);
	CSkinLoadData DefaultSkinData;
	SkinIt->second->SetState(CSkinContainer::EState::LOADING);
	if(!Graphics()->LoadPng(DefaultSkinData.m_Info, aPath, SkinIt->second->StorageType()))
	{
		log_error("skins", "Failed to load PNG of skin '%s' from '%s'", pName, aPath);
		SkinIt->second->SetState(CSkinContainer::EState::ERROR);
	}
	else
	{
		const bool DataLoaded = LoadSkinData(pName, DefaultSkinData.m_Info, DefaultSkinData, true);
		if(DefaultSkinData.m_ResizedWidth != 0)
			log_warn("skins", "Resizing skin '%s' from %" PRIzu "x%" PRIzu " to %" PRIzu "x%" PRIzu " because its size is not divisible by %dx%d", pName, DefaultSkinData.m_OriginalWidth, DefaultSkinData.m_OriginalHeight, DefaultSkinData.m_ResizedWidth, DefaultSkinData.m_ResizedHeight, g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy);
		if(DataLoaded && LoadSkinFinish(SkinIt->second.get(), DefaultSkinData))
			SkinIt->second->SetState(CSkinContainer::EState::LOADED);
		else
			SkinIt->second->SetState(CSkinContainer::EState::ERROR);
	}
	DefaultSkinData.m_Info.Free();
	DefaultSkinData.m_InfoGrayscale.Free();
}

void CSkins::StartSkinDecode(CSkinContainer *pSkinContainer, const char *pPath, int StorageType, ESkinDecodeSource Source)
{
	auto pData = std::make_shared<CSkinLoadData>();
	const std::string Name = pSkinContainer->Name();
	pSkinContainer->m_LoadResource = GameClient()->AssetLoader().LoadImageFile(Storage(), pPath, StorageType, ASSET_OWNER_SKINS, m_Generation, [pData, Name](CImageInfo &Info) {
		return LoadSkinData(Name.c_str(), Info, *pData, false);
	});
	pSkinContainer->m_pLoadData = std::move(pData);
	pSkinContainer->m_DecodeSource = Source;
}

void CSkins::StartSkinDecode(CSkinContainer *pSkinContainer, std::vector<uint8_t> vData, const char *pContextName, ESkinDecodeSource Source)
{
	auto pData = std::make_shared<CSkinLoadData>();
	const std::string Name = pSkinContainer->Name();
	pSkinContainer->m_LoadResource = GameClient()->AssetLoader().LoadImageData(std::move(vData), pContextName, ASSET_OWNER_SKINS, m_Generation, [pData, Name](CImageInfo &Info) {
		return LoadSkinData(Name.c_str(), Info, *pData, false);
	});
	pSkinContainer->m_pLoadData = std::move(pData);
	pSkinContainer->m_DecodeSource = Source;
}

void CSkins::StartLocalSkinLoad(CSkinContainer *pSkinContainer)
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "skins/%s.png", pSkinContainer->Name());
	StartSkinDecode(pSkinContainer, aPath, pSkinContainer->StorageType(), ESkinDecodeSource::LOCAL);
}

void CSkins::StartDownload(CSkinContainer *pSkinContainer, bool Force)
{
	const char *pBaseUrl = g_Config.m_ClDownloadCommunitySkins != 0 ? g_Config.m_ClSkinCommunityDownloadUrl : g_Config.m_ClSkinDownloadUrl;

	char aEscapedName[256];
	str_url_encode(aEscapedName, pSkinContainer->Name());

	char aUrl[IO_MAX_PATH_LENGTH];
	str_format(aUrl, sizeof(aUrl), "%s%s.png", pBaseUrl, aEscapedName);

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "downloadedskins/%s.png", pSkinContainer->Name());

	const CTimeout Timeout{10000, 0, 8192, 10};
	constexpr size_t MaxResponseSize = 10 * 1024 * 1024;

	if(Http() == nullptr)
	{
		// A program without HTTP has nowhere to download from, so a skin is
		// whatever is on disk: what an earlier download cached, or nothing.
		pSkinContainer->m_DownloadNotFound = !Storage()->FileExists(aPath, IStorage::TYPE_SAVE);
		pSkinContainer->m_DecodeSource = ESkinDecodeSource::NONE;
		if(!pSkinContainer->m_DownloadNotFound)
			StartSkinDecode(pSkinContainer, aPath, IStorage::TYPE_SAVE, ESkinDecodeSource::DOWNLOAD_CACHE);
		return;
	}
	std::shared_ptr<IHttpRequest> pRequest = Http()->CreateGetBoth(aUrl, Storage(), aPath, IStorage::TYPE_SAVE);
	pRequest->Timeout(Timeout);
	pRequest->MaxResponseSize(MaxResponseSize);
	pRequest->ValidateBeforeOverwrite(true);
	pRequest->SkipByFileTime(!Force);
	pRequest->LogProgress(HTTPLOG::NONE);
	pRequest->FailOnErrorStatus(false);
	pSkinContainer->m_pDownloadRequest = pRequest;
	pSkinContainer->m_DecodeSource = ESkinDecodeSource::NONE;
	Http()->Run(std::move(pRequest));
}

void CSkins::StartDownloadedSkinLoad(CSkinContainer *pSkinContainer)
{
	pSkinContainer->m_DownloadRetried = false;
	pSkinContainer->m_DownloadNotFound = false;
	StartDownload(pSkinContainer, false);
}

void CSkins::ResetSkinLoad(CSkinContainer *pSkinContainer)
{
	pSkinContainer->m_LoadResource.Reset();
	pSkinContainer->m_pLoadData = nullptr;
	pSkinContainer->m_pDownloadRequest = nullptr;
	pSkinContainer->m_DecodeSource = ESkinDecodeSource::NONE;
}

void CSkins::OnConsoleInit()
{
	ConfigManager()->RegisterCallback(CSkins::ConfigSaveCallback, this);
	Console()->Register("add_favorite_skin", "s[skin_name]", CFGFLAG_CLIENT, ConAddFavoriteSkin, this, "Add a skin as a favorite");
	Console()->Register("remove_favorite_skin", "s[skin_name]", CFGFLAG_CLIENT, ConRemFavoriteSkin, this, "Remove a skin from the favorites");

	Console()->Chain("player_skin", ConchainRefreshSkinList, this);
	Console()->Chain("dummy_skin", ConchainRefreshSkinList, this);
}

void CSkins::OnInit()
{
	RefreshEventSkins();

	// load skins
	Refresh([this]() {
		GameClient()->m_Menus.RenderLoading(Localize("Loading DDNet Client"), Localize("Loading skin files"), 0);
	});
	GameClient()->CollectManagedTeeRenderInfos([this](const char *pSkinName) {
		GameClient()->OnSkinUpdate(pSkinName);
	});
}

void CSkins::OnShutdown()
{
	m_Generation++;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(ASSET_OWNER_SKINS, m_Generation);
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_LoadResource)
		{
			pSkinContainer->m_LoadResource.Abort();
		}
		if(pSkinContainer->m_pDownloadRequest)
		{
			pSkinContainer->m_pDownloadRequest->Abort();
		}
	}
	m_Skins.clear();
}

void CSkins::OnUpdate()
{
	// Only update skins periodically to reduce FPS impact
	const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
	const std::chrono::nanoseconds MaxTime = std::chrono::milliseconds(std::clamp(round_to_int(Client()->RenderFrameTime() * 50000.0f), 25, 500));
	// Startup is the exception: two skins per pass every half second is six
	// seconds of cold start for the eighteen vanilla skins alone, and the frame
	// rate this pacing protects is the one of a client that is not running yet.
	if(m_ContainerUpdateTime.has_value() && StartTime - m_ContainerUpdateTime.value() < MaxTime &&
		!GameClient()->StartupAssetsPending())
	{
		return;
	}
	m_ContainerUpdateTime = StartTime;

	// Update loaded state of managed skins which are not retrieved with the FindOrNullptr function
	GameClient()->CollectManagedTeeRenderInfos([&](const char *pSkinName) {
		// This will update the loaded state of the container
		dbg_assert(FindContainerOrNullptr(pSkinName) != nullptr, "No skin container found for managed tee render info: %s", pSkinName);
	});
	// Keep player and dummy skin loaded
	FindContainerOrNullptr(g_Config.m_ClPlayerSkin);
	FindContainerOrNullptr(g_Config.m_ClDummySkin);

	CSkinLoadingStats Stats = LoadingStats();
	UpdateUnloadSkins(Stats);
	UpdateStartLoading(Stats);
	UpdateFinishLoading(Stats, StartTime, MaxTime);
}

void CSkins::UpdateUnloadSkins(CSkinLoadingStats &Stats)
{
	if(Stats.m_NumPending + Stats.m_NumLoaded + Stats.m_NumLoading <= (size_t)g_Config.m_ClSkinsLoadedMax)
	{
		return;
	}

	const std::chrono::nanoseconds UnloadStart = time_get_nanoseconds();
	size_t NumToUnload = std::min(Stats.m_NumPending + Stats.m_NumLoaded + Stats.m_NumLoading - (size_t)g_Config.m_ClSkinsLoadedMax, (size_t)16);
	const size_t MaxSkipped = m_SkinsUsageList.size() / 8;
	size_t NumSkipped = 0;
	for(auto It = m_SkinsUsageList.rbegin(); It != m_SkinsUsageList.rend() && NumToUnload != 0 && NumSkipped < MaxSkipped; ++It)
	{
		auto SkinIt = m_Skins.find(*It);
		dbg_assert(SkinIt != m_Skins.end(), "m_SkinsUsageList contains skin not in m_Skins");
		auto &pSkinContainer = SkinIt->second;
		dbg_assert(!pSkinContainer->m_AlwaysLoaded, "m_SkinsUsageList contains skins with m_AlwaysLoaded");
		if(pSkinContainer->m_State != CSkinContainer::EState::PENDING &&
			pSkinContainer->m_State != CSkinContainer::EState::LOADED)
		{
			dbg_assert(pSkinContainer->m_State == CSkinContainer::EState::LOADING, "m_SkinsUsageList contains skin which is not PENDING, LOADING or LOADED");
			NumSkipped++;
			continue;
		}
		const std::chrono::nanoseconds TimeUnused = UnloadStart - pSkinContainer->m_LastLoadRequest.value();
		if(TimeUnused < (pSkinContainer->m_State == CSkinContainer::EState::LOADED ? MIN_UNLOAD_TIME_LOADED : MIN_UNLOAD_TIME_PENDING))
		{
			NumSkipped++;
			continue;
		}
		if(pSkinContainer->m_pSkin)
		{
			pSkinContainer->m_pSkin->m_OriginalSkin.Unload(Graphics());
			pSkinContainer->m_pSkin->m_ColorableSkin.Unload(Graphics());
			pSkinContainer->m_pSkin = nullptr;
		}
		if(pSkinContainer->m_State == CSkinContainer::EState::LOADED)
			Stats.m_NumLoaded--;
		else
			Stats.m_NumPending--;
		Stats.m_NumUnloaded++;
		pSkinContainer->SetState(CSkinContainer::EState::UNLOADED);
		NumToUnload--;
	}
}

void CSkins::UpdateStartLoading(CSkinLoadingStats &Stats)
{
	dbg_assert(Stats.m_NumLoading <= MAX_CONCURRENT_SKIN_LOADS, "Too many concurrent skin loads");
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(Stats.m_NumPending == 0 ||
			Stats.m_NumLoading >= MAX_CONCURRENT_SKIN_LOADS ||
			Stats.m_NumLoading + Stats.m_NumLoaded >= (size_t)g_Config.m_ClSkinsLoadedMax)
		{
			break;
		}
		if(pSkinContainer->m_State != CSkinContainer::EState::PENDING)
		{
			continue;
		}
		switch(pSkinContainer->Type())
		{
		case CSkinContainer::EType::LOCAL:
			StartLocalSkinLoad(pSkinContainer.get());
			break;
		case CSkinContainer::EType::DOWNLOAD:
			StartDownloadedSkinLoad(pSkinContainer.get());
			break;
		default:
			dbg_assert_failed("pSkinContainer->Type() invalid");
		}
		pSkinContainer->SetState(CSkinContainer::EState::LOADING);
		Stats.m_NumPending--;
		Stats.m_NumLoading++;
	}
	dbg_assert(Stats.m_NumLoading <= MAX_CONCURRENT_SKIN_LOADS, "Too many concurrent skin loads");
}

void CSkins::UpdateFinishLoading(CSkinLoadingStats &Stats, std::chrono::nanoseconds StartTime, std::chrono::nanoseconds MaxTime)
{
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(Stats.m_NumLoading == 0)
		{
			break;
		}
		if(pSkinContainer->m_State != CSkinContainer::EState::LOADING)
		{
			continue;
		}

		if(!pSkinContainer->m_LoadResource && pSkinContainer->m_pDownloadRequest)
		{
			if(!pSkinContainer->m_pDownloadRequest->Done())
			{
				continue;
			}

			if(pSkinContainer->m_pDownloadRequest->State() == EHttpState::DONE && pSkinContainer->m_pDownloadRequest->StatusCode() < 400)
			{
				if(pSkinContainer->m_pDownloadRequest->StatusCode() == 304)
				{
					char aPath[IO_MAX_PATH_LENGTH];
					str_format(aPath, sizeof(aPath), "downloadedskins/%s.png", pSkinContainer->Name());
					StartSkinDecode(pSkinContainer.get(), aPath, IStorage::TYPE_SAVE, ESkinDecodeSource::DOWNLOAD_CACHE);
				}
				else
				{
					unsigned char *pResult;
					size_t ResultSize;
					pSkinContainer->m_pDownloadRequest->Result(&pResult, &ResultSize);
					std::vector<uint8_t> vData;
					if(ResultSize > 0)
						vData.assign(pResult, pResult + ResultSize);
					char aContextName[IO_MAX_PATH_LENGTH];
					str_format(aContextName, sizeof(aContextName), "downloaded skin '%s'", pSkinContainer->Name());
					StartSkinDecode(pSkinContainer.get(), std::move(vData), aContextName, ESkinDecodeSource::DOWNLOAD_RESPONSE);
				}
			}
			else
			{
				pSkinContainer->m_DownloadNotFound = pSkinContainer->m_pDownloadRequest->State() == EHttpState::DONE && pSkinContainer->m_pDownloadRequest->StatusCode() == 404;
				pSkinContainer->m_pDownloadRequest = nullptr;
				char aPath[IO_MAX_PATH_LENGTH];
				str_format(aPath, sizeof(aPath), "downloadedskins/%s.png", pSkinContainer->Name());
				// A skin the database does not have was never cached either, so
				// decoding the missing file only costs a job and reports a load
				// failure for a file that was never there.
				if(!Storage()->FileExists(aPath, IStorage::TYPE_SAVE))
				{
					FinishSkinLoad(Stats, pSkinContainer.get(), false);
					continue;
				}
				StartSkinDecode(pSkinContainer.get(), aPath, IStorage::TYPE_SAVE, ESkinDecodeSource::DOWNLOAD_CACHE);
			}
			continue;
		}

		if(!pSkinContainer->m_LoadResource)
		{
			// Neither a file being read nor a download to wait for: there is
			// nothing left that could still make this skin appear.
			FinishSkinLoad(Stats, pSkinContainer.get(), false);
			continue;
		}
		if(!pSkinContainer->m_LoadResource.IsFinished())
		{
			continue;
		}

		if(pSkinContainer->m_pLoadData->m_ResizedWidth != 0)
			log_warn("skins", "Resizing skin '%s' from %" PRIzu "x%" PRIzu " to %" PRIzu "x%" PRIzu " because its size is not divisible by %dx%d", pSkinContainer->Name(), pSkinContainer->m_pLoadData->m_OriginalWidth, pSkinContainer->m_pLoadData->m_OriginalHeight, pSkinContainer->m_pLoadData->m_ResizedWidth, pSkinContainer->m_pLoadData->m_ResizedHeight, g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy);

		const bool Stale = pSkinContainer->m_LoadResource.IsStale(m_Generation);
		if(!Stale && pSkinContainer->m_LoadResource.IsFailed(m_Generation))
			log_error("skins", "Failed to load skin '%s' from '%s'", pSkinContainer->Name(), pSkinContainer->m_LoadResource.Path());
		const bool DecodeSuccess = pSkinContainer->m_LoadResource.IsReady(m_Generation);
		bool PublishSuccess = false;
		if(DecodeSuccess)
		{
			pSkinContainer->m_pLoadData->m_Info = pSkinContainer->m_LoadResource.TakeImage();
			PublishSuccess = LoadSkinFinish(pSkinContainer.get(), *pSkinContainer->m_pLoadData);
		}
		if(PublishSuccess)
		{
			pSkinContainer->SetState(CSkinContainer::EState::LOADED);
			if(pSkinContainer->m_pDownloadRequest)
			{
				pSkinContainer->m_pDownloadRequest->OnValidation(true);
			}
			GameClient()->OnSkinUpdate(pSkinContainer->Name());
			ResetSkinLoad(pSkinContainer.get());
			Stats.m_NumLoading--;
			Stats.m_NumLoaded++;
			if(time_get_nanoseconds() - StartTime >= MaxTime)
			{
				// Avoid using too much frame time for loading skins
				break;
			}
			continue;
		}

		if(!Stale && pSkinContainer->m_DecodeSource == ESkinDecodeSource::DOWNLOAD_RESPONSE)
		{
			dbg_assert(pSkinContainer->m_pDownloadRequest != nullptr, "Downloaded skin response missing request");
			pSkinContainer->m_pDownloadRequest->OnValidation(false);
			pSkinContainer->m_pDownloadRequest = nullptr;
			pSkinContainer->m_LoadResource.Reset();
			pSkinContainer->m_pLoadData = nullptr;
			char aPath[IO_MAX_PATH_LENGTH];
			str_format(aPath, sizeof(aPath), "downloadedskins/%s.png", pSkinContainer->Name());
			StartSkinDecode(pSkinContainer.get(), aPath, IStorage::TYPE_SAVE, ESkinDecodeSource::DOWNLOAD_CACHE);
			continue;
		}

		if(!Stale && pSkinContainer->m_DecodeSource == ESkinDecodeSource::DOWNLOAD_CACHE &&
			pSkinContainer->m_pDownloadRequest && pSkinContainer->m_pDownloadRequest->StatusCode() == 304 &&
			!pSkinContainer->m_DownloadRetried)
		{
			pSkinContainer->m_pDownloadRequest->OnValidation(false);
			pSkinContainer->m_pDownloadRequest = nullptr;
			pSkinContainer->m_LoadResource.Reset();
			pSkinContainer->m_pLoadData = nullptr;
			pSkinContainer->m_DownloadRetried = true;
			StartDownload(pSkinContainer.get(), true);
			continue;
		}

		FinishSkinLoad(Stats, pSkinContainer.get(), Stale);
	}
}

void CSkins::FinishSkinLoad(CSkinLoadingStats &Stats, CSkinContainer *pSkinContainer, bool Stale)
{
	Stats.m_NumLoading--;
	const bool NotFound = !Stale && pSkinContainer->m_DownloadNotFound;
	ResetSkinLoad(pSkinContainer);
	if(pSkinContainer->m_pSkin)
	{
		pSkinContainer->SetState(CSkinContainer::EState::LOADED);
		Stats.m_NumLoaded++;
	}
	else if(NotFound)
	{
		pSkinContainer->SetState(CSkinContainer::EState::NOT_FOUND);
		Stats.m_NumNotFound++;
	}
	else
	{
		pSkinContainer->SetState(Stale ? CSkinContainer::EState::UNLOADED : CSkinContainer::EState::ERROR);
		if(Stale)
			Stats.m_NumUnloaded++;
		else
			Stats.m_NumError++;
	}
}

void CSkins::RefreshEventSkins()
{
	m_aEventSkinPrefix[0] = '\0';

	if(g_Config.m_Events)
	{
		if(time_season() == ETimeSeason::XMAS)
		{
			str_copy(m_aEventSkinPrefix, "santa");
		}
	}
}

void CSkins::Refresh(TSkinLoadedCallback &&SkinLoadedCallback)
{
	m_Generation++;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(ASSET_OWNER_SKINS, m_Generation);
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_LoadResource)
		{
			pSkinContainer->m_LoadResource.Abort();
		}
		if(pSkinContainer->m_pDownloadRequest)
		{
			pSkinContainer->m_pDownloadRequest->Abort();
		}
	}
	auto OldSkins = std::move(m_Skins);
	m_Skins.clear();
	m_SkinsUsageList.clear();

	LoadSkinDirect("default");
	SkinLoadedCallback();

	CSkinScanUser SkinScanUser;
	SkinScanUser.m_pThis = this;
	SkinScanUser.m_SkinLoadedCallback = SkinLoadedCallback;
	Storage()->ListDirectory(IStorage::TYPE_ALL, "skins", SkinScan, &SkinScanUser);

	for(auto &[_, pOldSkinContainer] : OldSkins)
	{
		if(pOldSkinContainer->Type() != CSkinContainer::EType::DOWNLOAD || !pOldSkinContainer->m_pSkin || m_Skins.contains(pOldSkinContainer->Name()))
		{
			continue;
		}
		CSkinContainer SkinContainer(this, pOldSkinContainer->Name(), CSkinContainer::EType::DOWNLOAD, IStorage::TYPE_SAVE);
		auto pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
		pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
		m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)});
		SkinLoadedCallback();
	}

	for(auto &[Name, pSkinContainer] : m_Skins)
	{
		auto OldSkinIt = OldSkins.find(Name);
		if(OldSkinIt == OldSkins.end() || !OldSkinIt->second->m_pSkin)
		{
			continue;
		}
		if(pSkinContainer->m_pSkin || pSkinContainer->m_State == CSkinContainer::EState::NOT_FOUND)
		{
			OldSkinIt->second->m_pSkin->m_OriginalSkin.Unload(Graphics());
			OldSkinIt->second->m_pSkin->m_ColorableSkin.Unload(Graphics());
			OldSkinIt->second->m_pSkin = nullptr;
			continue;
		}

		pSkinContainer->m_pSkin = std::move(OldSkinIt->second->m_pSkin);
		pSkinContainer->SetState(pSkinContainer->m_State == CSkinContainer::EState::ERROR ? CSkinContainer::EState::LOADED : CSkinContainer::EState::PENDING);
	}

	for(auto &[_, pOldSkinContainer] : OldSkins)
	{
		if(!pOldSkinContainer->m_pSkin)
		{
			continue;
		}
		pOldSkinContainer->m_pSkin->m_OriginalSkin.Unload(Graphics());
		pOldSkinContainer->m_pSkin->m_ColorableSkin.Unload(Graphics());
	}
}

CSkins::CSkinLoadingStats CSkins::LoadingStats() const
{
	CSkinLoadingStats Stats;
	for(const auto &[_, pSkinContainer] : m_Skins)
	{
		switch(pSkinContainer->m_State)
		{
		case CSkinContainer::EState::UNLOADED:
			Stats.m_NumUnloaded++;
			break;
		case CSkinContainer::EState::PENDING:
			Stats.m_NumPending++;
			break;
		case CSkinContainer::EState::LOADING:
			Stats.m_NumLoading++;
			break;
		case CSkinContainer::EState::LOADED:
			Stats.m_NumLoaded++;
			break;
		case CSkinContainer::EState::ERROR:
			Stats.m_NumError++;
			break;
		case CSkinContainer::EState::NOT_FOUND:
			Stats.m_NumNotFound++;
			break;
		}
	}
	return Stats;
}

bool CSkins::StartupAssetsLoaded() const
{
	return std::none_of(m_Skins.begin(), m_Skins.end(), [](const auto &Entry) {
		const CSkinContainer &Container = *Entry.second;
		return Container.m_AlwaysLoaded && (Container.m_State == CSkinContainer::EState::PENDING || Container.m_State == CSkinContainer::EState::LOADING);
	});
}

CSkins::CSkinList &CSkins::SkinList()
{
	if(!m_SkinList.m_NeedsUpdate)
	{
		return m_SkinList;
	}

	m_SkinList.m_vSkins.clear();
	m_SkinList.m_UnfilteredCount = 0;

	// Ensure all favorite skins are present as skin containers so they are included in the next loop.
	for(const auto &FavoriteSkin : m_Favorites)
	{
		FindContainerOrNullptr(FavoriteSkin.c_str());
	}

	m_SkinList.m_vSkins.reserve(m_Skins.size());
	for(const auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->IsSpecial())
		{
			continue;
		}

		const bool SelectedMain = str_comp(pSkinContainer->Name(), g_Config.m_ClPlayerSkin) == 0;
		const bool SelectedDummy = str_comp(pSkinContainer->Name(), g_Config.m_ClDummySkin) == 0;
		const bool Favorite = IsFavorite(pSkinContainer->Name());

		// Don't include skins in the list that couldn't be found in the database except the current player
		// and dummy skins to avoid showing a lot of not-found entries while the user is typing a skin name.
		if(pSkinContainer->m_State == CSkinContainer::EState::NOT_FOUND &&
			!pSkinContainer->IsSpecial() &&
			!SelectedMain &&
			!SelectedDummy &&
			!Favorite)
		{
			continue;
		}
		m_SkinList.m_UnfilteredCount++;

		std::optional<std::pair<int, int>> NameMatch;
		if(g_Config.m_ClSkinFilterString[0] != '\0')
		{
			const char *pNameMatchEnd;
			const char *pNameMatchStart = str_utf8_find_nocase(pSkinContainer->Name(), g_Config.m_ClSkinFilterString, &pNameMatchEnd);
			if(pNameMatchStart == nullptr)
			{
				continue;
			}
			NameMatch = std::make_pair<int, int>(pNameMatchStart - pSkinContainer->Name(), pNameMatchEnd - pNameMatchStart);
		}
		m_SkinList.m_vSkins.emplace_back(pSkinContainer.get(), Favorite, SelectedMain, SelectedDummy, NameMatch);
	}

	std::sort(m_SkinList.m_vSkins.begin(), m_SkinList.m_vSkins.end());
	m_SkinList.m_NeedsUpdate = false;
	return m_SkinList;
}

const CSkin *CSkins::Find(const char *pName)
{
	const auto *pSkin = FindOrNullptr(pName);
	if(pSkin == nullptr)
	{
		pSkin = FindOrNullptr("default");
	}
	if(pSkin == nullptr)
	{
		pSkin = &m_PlaceholderSkin;
	}
	return pSkin;
}

const CSkins::CSkinContainer *CSkins::FindContainerOrNullptr(const char *pName)
{
	const char *pSkinPrefix = SkinPrefix();
	if(pSkinPrefix[0] != '\0')
	{
		char aNameWithPrefix[2 * MAX_SKIN_LENGTH + 2]; // Larger than skin name length to allow IsValidName to check if it's too long
		str_format(aNameWithPrefix, sizeof(aNameWithPrefix), "%s_%s", pSkinPrefix, pName);
		// If we find something, use it, otherwise fall back to normal skins.
		const CSkinContainer *pSkinContainer = FindContainerImpl(aNameWithPrefix);
		if(pSkinContainer != nullptr && pSkinContainer->Skin())
		{
			return pSkinContainer;
		}
	}
	return FindContainerImpl(pName);
}

const CSkins::CSkinContainer *CSkins::FindContainerImpl(const char *pName)
{
	if(!CSkin::IsValidName(pName))
	{
		return nullptr;
	}

	auto ExistingSkin = m_Skins.find(pName);
	if(ExistingSkin == m_Skins.end())
	{
		CSkinContainer SkinContainer(this, pName, CSkinContainer::EType::DOWNLOAD, IStorage::TYPE_SAVE);
		auto &&pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
		pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
		ExistingSkin = m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)}).first;
	}
	ExistingSkin->second->RequestLoad();
	return ExistingSkin->second.get();
}

const CSkin *CSkins::FindOrNullptr(const char *pName)
{
	const CSkinContainer *pSkinContainer = FindContainerOrNullptr(pName);
	if(pSkinContainer == nullptr || !pSkinContainer->m_pSkin)
	{
		return nullptr;
	}
	return pSkinContainer->m_pSkin.get();
}

void CSkins::AddFavorite(const char *pName)
{
	if(!CSkin::IsValidName(pName))
	{
		log_error("skins", "Favorite skin name '%s' is not valid", pName);
		log_error("skins", "%s", CSkin::m_aSkinNameRestrictions);
		return;
	}

	const auto &[_, Inserted] = m_Favorites.emplace(pName);
	if(Inserted)
	{
		m_SkinList.ForceRefresh();
	}
}

void CSkins::RemoveFavorite(const char *pName)
{
	const auto FavoriteIt = m_Favorites.find(pName);
	if(FavoriteIt != m_Favorites.end())
	{
		m_Favorites.erase(FavoriteIt);
		m_SkinList.ForceRefresh();
	}
}

bool CSkins::IsFavorite(const char *pName) const
{
	return m_Favorites.contains(pName);
}

void CSkins::RandomizeSkin(int Dummy)
{
	static const float s_aSchemes[] = {1.0f / 2.0f, 1.0f / 3.0f, 1.0f / -3.0f, 1.0f / 12.0f, 1.0f / -12.0f}; // complementary, triadic, analogous
	const bool UseCustomColor = Dummy ? g_Config.m_ClDummyUseCustomColor : g_Config.m_ClPlayerUseCustomColor;
	if(UseCustomColor)
	{
		float GoalSat = random_float(0.3f, 1.0f);
		float MaxBodyLht = 1.0f - GoalSat * GoalSat; // max allowed lightness before we start losing saturation

		ColorHSLA Body;
		Body.h = random_float();
		Body.l = random_float(0.0f, MaxBodyLht);
		Body.s = std::clamp(GoalSat * GoalSat / (1.0f - Body.l), 0.0f, 1.0f);

		ColorHSLA Feet;
		Feet.h = std::fmod(Body.h + s_aSchemes[rand() % std::size(s_aSchemes)], 1.0f);
		Feet.l = random_float();
		Feet.s = std::clamp(GoalSat * GoalSat / (1.0f - Feet.l), 0.0f, 1.0f);

		unsigned *pColorBody = Dummy ? &g_Config.m_ClDummyColorBody : &g_Config.m_ClPlayerColorBody;
		unsigned *pColorFeet = Dummy ? &g_Config.m_ClDummyColorFeet : &g_Config.m_ClPlayerColorFeet;

		*pColorBody = Body.Pack(false);
		*pColorFeet = Feet.Pack(false);
	}

	std::vector<const CSkinContainer *> vpConsideredSkins;
	for(const auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_State == CSkinContainer::EState::ERROR ||
			pSkinContainer->m_State == CSkinContainer::EState::NOT_FOUND ||
			pSkinContainer->IsSpecial())
		{
			continue;
		}
		vpConsideredSkins.push_back(pSkinContainer.get());
	}
	const char *pRandomSkin;
	if(vpConsideredSkins.empty())
	{
		pRandomSkin = "default";
	}
	else
	{
		pRandomSkin = vpConsideredSkins[rand() % vpConsideredSkins.size()]->Name();
	}

	char *pSkinName = Dummy ? g_Config.m_ClDummySkin : g_Config.m_ClPlayerSkin;
	const size_t SkinNameSize = Dummy ? sizeof(g_Config.m_ClDummySkin) : sizeof(g_Config.m_ClPlayerSkin);
	str_copy(pSkinName, pRandomSkin, SkinNameSize);
	m_SkinList.ForceRefresh();
}

const char *CSkins::SkinPrefix() const
{
	if(g_Config.m_ClVanillaSkinsOnly)
	{
		return "";
	}
	if(m_aEventSkinPrefix[0] != '\0')
	{
		return m_aEventSkinPrefix;
	}
	return g_Config.m_ClSkinPrefix;
}

void CSkins::ConAddFavoriteSkin(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CSkins *>(pUserData);
	pSelf->AddFavorite(pResult->GetString(0));
}

void CSkins::ConRemFavoriteSkin(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CSkins *>(pUserData);
	pSelf->RemoveFavorite(pResult->GetString(0));
}

void CSkins::ConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData)
{
	auto *pSelf = static_cast<CSkins *>(pUserData);
	pSelf->OnConfigSave(pConfigManager);
}

void CSkins::OnConfigSave(IConfigManager *pConfigManager)
{
	for(const auto &Favorite : m_Favorites)
	{
		char aBuffer[32 + MAX_SKIN_LENGTH];
		str_format(aBuffer, sizeof(aBuffer), "add_favorite_skin \"%s\"", Favorite.c_str());
		pConfigManager->WriteLine(aBuffer);
	}
}

void CSkins::ConchainRefreshSkinList(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CSkins *pThis = static_cast<CSkins *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		pThis->m_SkinList.ForceRefresh();
	}
}
