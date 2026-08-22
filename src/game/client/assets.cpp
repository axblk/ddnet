#include "gameclient.h"

#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/storage.h>

#include <generated/client_data.h>
#include <generated/client_data7.h>

#include <algorithm>

namespace
{
	constexpr int ASSET_OWNER_STARTUP_IMAGES = 2;
	constexpr int ASSET_OWNER_PACK_BASE = 100;
	template<size_t N>
	bool AllTexturesValid(const IGraphics::CTextureHandle (&aTextures)[N])
	{
		return std::all_of(std::begin(aTextures), std::end(aTextures), [](IGraphics::CTextureHandle Texture) { return Texture.IsValid(); });
	}
}

void CGameClient::StartLoadingCoreImages()
{
	m_StartupImageBatchStart = time_get();
	++m_AssetGeneration;
	m_AssetLoader.AbortOwnerBeforeGeneration(ASSET_OWNER_STARTUP_IMAGES, m_AssetGeneration);
	m_vStartupImageLoads.clear();
	m_DecodedAssetImages.clear();

	const auto SubmitImage = [this](int ImageId, bool IsAssetSheet, const char *pPath) {
		m_vStartupImageLoads.push_back({ImageId, IsAssetSheet, m_AssetLoader.LoadImageFile(Storage(), pPath, IStorage::TYPE_ALL, ASSET_OWNER_STARTUP_IMAGES, m_AssetGeneration)});
	};
	const auto SubmitAssetSheet = [&](int ImageId, const char *pAssetName, const char *pDirectory) {
		const char *pDefaultPath = g_pData->m_aImages[ImageId].m_pFilename;
		if(str_comp(pAssetName, "default") != 0)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			str_format(aPath, sizeof(aPath), "assets/%s/%s.png", pDirectory, pAssetName);
			SubmitImage(ImageId, true, aPath);
			str_format(aPath, sizeof(aPath), "assets/%s/%s/%s", pDirectory, pAssetName, pDefaultPath);
			SubmitImage(ImageId, true, aPath);
		}
		SubmitImage(ImageId, true, pDefaultPath);
	};

	for(int ImageId = 0; ImageId < g_pData->m_NumImages; ++ImageId)
	{
		switch(ImageId)
		{
		case IMAGE_GAME:
			SubmitAssetSheet(ImageId, g_Config.m_ClAssetGame, "game");
			break;
		case IMAGE_EMOTICONS:
			SubmitAssetSheet(ImageId, g_Config.m_ClAssetEmoticons, "emoticons");
			break;
		case IMAGE_PARTICLES:
			SubmitAssetSheet(ImageId, g_Config.m_ClAssetParticles, "particles");
			break;
		case IMAGE_HUD:
			SubmitAssetSheet(ImageId, g_Config.m_ClAssetHud, "hud");
			break;
		case IMAGE_EXTRAS:
			SubmitAssetSheet(ImageId, g_Config.m_ClAssetExtras, "extras");
			break;
		default:
			if(g_pData->m_aImages[ImageId].m_pFilename[0] != '\0')
				SubmitImage(ImageId, false, g_pData->m_aImages[ImageId].m_pFilename);
		}
	}
}

void CGameClient::TryFinishLoadingCoreImages()
{
	m_AssetLoader.Update();
	for(const CStartupImageLoad &Load : m_vStartupImageLoads)
	{
		if(!Load.m_Resource.IsFinished())
			return;
	}

	std::chrono::nanoseconds TotalReadTime{};
	std::chrono::nanoseconds TotalDecodeTime{};
	const size_t NumJobs = m_vStartupImageLoads.size();
	size_t NumErrors = 0;
	for(CStartupImageLoad &Load : m_vStartupImageLoads)
	{
		dbg_assert(Load.m_Resource.IsReady(m_AssetGeneration) || Load.m_Resource.IsFailed(m_AssetGeneration), "Startup image resource must not be aborted or stale");
		if(Load.m_Resource.IsReady(m_AssetGeneration) || Load.m_Resource.IsFailed(m_AssetGeneration))
		{
			TotalReadTime += Load.m_Resource.ReadTime();
			TotalDecodeTime += Load.m_Resource.DecodeTime();
			NumErrors += Load.m_Resource.IsFailed(m_AssetGeneration);
		}
		else
		{
			++NumErrors;
		}
	}
	const std::chrono::nanoseconds CommitStart = time_get_nanoseconds();
	for(int ImageId = 0; ImageId < g_pData->m_NumImages; ++ImageId)
	{
		bool IsAssetSheet = false;
		for(CStartupImageLoad &Load : m_vStartupImageLoads)
		{
			if(Load.m_ImageId != ImageId)
				continue;
			IsAssetSheet = Load.m_IsAssetSheet;
			if(!Load.m_Resource.IsReady(m_AssetGeneration))
				continue;

			CImageInfo Image = Load.m_Resource.TakeImage();
			if(IsAssetSheet)
			{
				m_DecodedAssetImages.emplace(Load.m_Resource.Path(), std::move(Image));
			}
			else
			{
				IGraphics::CTextureHandle NewTexture = Graphics()->LoadTextureRawMove(Image, 0, Load.m_Resource.Path());
				if(NewTexture.IsValid())
				{
					IGraphics::CTextureHandle &Texture = g_pData->m_aImages[ImageId].m_Id;
					if(Texture.IsValid())
						Graphics()->UnloadTexture(&Texture);
					Texture = NewTexture;
				}
			}
		}

		if(IsAssetSheet)
		{
			switch(ImageId)
			{
			case IMAGE_GAME: CommitGameSkin(g_Config.m_ClAssetGame); break;
			case IMAGE_EMOTICONS: CommitEmoticonsSkin(g_Config.m_ClAssetEmoticons); break;
			case IMAGE_PARTICLES: CommitParticlesSkin(g_Config.m_ClAssetParticles); break;
			case IMAGE_HUD: CommitHudSkin(g_Config.m_ClAssetHud); break;
			case IMAGE_EXTRAS: CommitExtrasSkin(g_Config.m_ClAssetExtras); break;
			}
		}
		else if(g_pData->m_aImages[ImageId].m_pFilename[0] == '\0')
		{
			g_pData->m_aImages[ImageId].m_Id = IGraphics::CTextureHandle();
		}
		else if(!g_pData->m_aImages[ImageId].m_Id.IsValid())
		{
			// Preserve the established null-texture fallback on the rare async read or upload failure path.
			g_pData->m_aImages[ImageId].m_Id = Graphics()->LoadTexture(g_pData->m_aImages[ImageId].m_pFilename, IStorage::TYPE_ALL);
		}
	}
	m_DecodedAssetImages.clear();
	m_vStartupImageLoads.clear();
	const std::chrono::nanoseconds CommitTime = time_get_nanoseconds() - CommitStart;
	log_info("asset_loader", "Startup image batch: jobs=%" PRIzu " errors=%" PRIzu " wall=%.2fms read=%.2fms decode=%.2fms commit=%.2fms",
		NumJobs, NumErrors, (time_get() - m_StartupImageBatchStart) * 1000.0 / time_freq(),
		TotalReadTime.count() / 1000000.0, TotalDecodeTime.count() / 1000000.0, CommitTime.count() / 1000000.0);
}

void CGameClient::FinishClientStartup()
{
	dbg_assert(m_StartupStart != 0, "Client startup already finished");
	m_Menus.FinishLoading();
	log_info("asset_loader", "Client startup ready: wall=%.2fms", (time_get_nanoseconds().count() - m_StartupStart) / 1000000.0);
	m_StartupStart = 0;
}

void CGameClient::TryFinishStartupAssets()
{
	if(m_StartupAssetsStart == 0 || !m_vStartupImageLoads.empty() || !m_Sounds.StartupAssetsLoaded() || !m_Skins.StartupAssetsLoaded() || !m_Skins7.StartupAssetsLoaded() || !m_Menus.StartupAssetsLoaded() || !m_CountryFlags.StartupAssetsLoaded() || !m_Scoreboard.StartupAssetsLoaded())
		return;
	log_info("asset_loader", "Client startup assets complete: wall=%.2fms", (time_get_nanoseconds().count() - m_StartupAssetsStart) / 1000000.0);
	m_StartupAssetsStart = 0;
}

void CGameClient::StartLoadingAssetPack(int ImageId, const char *pName, bool AsDir)
{
	const char *pDirectory = nullptr;
	switch(ImageId)
	{
	case IMAGE_GAME: pDirectory = "game"; break;
	case IMAGE_EMOTICONS: pDirectory = "emoticons"; break;
	case IMAGE_PARTICLES: pDirectory = "particles"; break;
	case IMAGE_HUD: pDirectory = "hud"; break;
	case IMAGE_EXTRAS: pDirectory = "extras"; break;
	default:
		dbg_assert_failed("Invalid asset pack image ID: %d", ImageId);
		return;
	}

	const uint64_t Generation = ++m_AssetPackGeneration;
	const int OwnerId = ASSET_OWNER_PACK_BASE + ImageId;
	m_AssetLoader.AbortOwnerBeforeGeneration(OwnerId, Generation);
	m_vAssetPackLoads.erase(
		std::remove_if(m_vAssetPackLoads.begin(), m_vAssetPackLoads.end(), [ImageId](const CAssetPackLoad &Load) { return Load.m_ImageId == ImageId; }),
		m_vAssetPackLoads.end());

	CAssetPackLoad Load;
	Load.m_ImageId = ImageId;
	Load.m_Generation = Generation;
	Load.m_Name = pName;
	Load.m_AsDir = AsDir;
	const auto Submit = [&](const char *pPath) {
		Load.m_vResources.push_back(m_AssetLoader.LoadImageFile(Storage(), pPath, IStorage::TYPE_ALL, OwnerId, Generation));
	};
	if(str_comp(pName, "default") != 0)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		if(!AsDir)
		{
			str_format(aPath, sizeof(aPath), "assets/%s/%s.png", pDirectory, pName);
			Submit(aPath);
		}
		str_format(aPath, sizeof(aPath), "assets/%s/%s/%s", pDirectory, pName, g_pData->m_aImages[ImageId].m_pFilename);
		Submit(aPath);
	}
	Submit(g_pData->m_aImages[ImageId].m_pFilename);
	m_vAssetPackLoads.push_back(std::move(Load));
}

void CGameClient::UpdateAssetPackLoads()
{
	for(auto It = m_vAssetPackLoads.begin(); It != m_vAssetPackLoads.end();)
	{
		if(std::any_of(It->m_vResources.begin(), It->m_vResources.end(), [](const CImageResource &Resource) { return !Resource.IsFinished(); }))
		{
			++It;
			continue;
		}

		m_DecodedAssetImages.clear();
		for(CImageResource &Resource : It->m_vResources)
		{
			if(!Resource.IsReady(It->m_Generation))
				continue;
			m_DecodedAssetImages.emplace(Resource.Path(), Resource.TakeImage());
		}
		switch(It->m_ImageId)
		{
		case IMAGE_GAME: CommitGameSkin(It->m_Name.c_str(), It->m_AsDir); break;
		case IMAGE_EMOTICONS: CommitEmoticonsSkin(It->m_Name.c_str(), It->m_AsDir); break;
		case IMAGE_PARTICLES: CommitParticlesSkin(It->m_Name.c_str(), It->m_AsDir); break;
		case IMAGE_HUD: CommitHudSkin(It->m_Name.c_str(), It->m_AsDir); break;
		case IMAGE_EXTRAS: CommitExtrasSkin(It->m_Name.c_str(), It->m_AsDir); break;
		}
		m_DecodedAssetImages.clear();
		It = m_vAssetPackLoads.erase(It);
	}
}

void CGameClient::LoadGameSkin(const char *pPath, bool AsDir)
{
	StartLoadingAssetPack(IMAGE_GAME, pPath, AsDir);
}

void CGameClient::LoadEmoticonsSkin(const char *pPath, bool AsDir)
{
	StartLoadingAssetPack(IMAGE_EMOTICONS, pPath, AsDir);
}

void CGameClient::LoadParticlesSkin(const char *pPath, bool AsDir)
{
	StartLoadingAssetPack(IMAGE_PARTICLES, pPath, AsDir);
}

void CGameClient::LoadHudSkin(const char *pPath, bool AsDir)
{
	StartLoadingAssetPack(IMAGE_HUD, pPath, AsDir);
}

void CGameClient::LoadExtrasSkin(const char *pPath, bool AsDir)
{
	StartLoadingAssetPack(IMAGE_EXTRAS, pPath, AsDir);
}

CGameClient::CImageAsset CGameClient::LoadAssetFromPath(const char *pPath, bool AsDir, int AssetId, const char *pDirectory)
{
	CImageAsset LoadedAsset;
	LoadedAsset.m_IsDefault = str_comp(pPath, "default") == 0;
	if(LoadedAsset.m_IsDefault)
	{
		str_copy(LoadedAsset.m_aPath, g_pData->m_aImages[AssetId].m_pFilename);
	}
	else if(AsDir)
	{
		str_format(LoadedAsset.m_aPath, sizeof(LoadedAsset.m_aPath), "assets/%s/%s/%s", pDirectory, pPath, g_pData->m_aImages[AssetId].m_pFilename);
	}
	else
	{
		str_format(LoadedAsset.m_aPath, sizeof(LoadedAsset.m_aPath), "assets/%s/%s.png", pDirectory, pPath);
	}

	auto It = m_DecodedAssetImages.find(LoadedAsset.m_aPath);
	if(It != m_DecodedAssetImages.end())
		LoadedAsset.m_ImageInfo = std::move(It->second);

	if(!LoadedAsset.m_IsDefault && LoadedAsset.IsLoaded())
	{
		CImageInfo ImgDefaultInfo;
		auto DefaultIt = m_DecodedAssetImages.find(g_pData->m_aImages[AssetId].m_pFilename);
		if(DefaultIt != m_DecodedAssetImages.end())
			ImgDefaultInfo = std::move(DefaultIt->second);
		if(ImgDefaultInfo.m_pData != nullptr)
			LoadedAsset.m_FallbackImageInfo = std::move(ImgDefaultInfo);
	}
	return LoadedAsset;
}

void CGameClient::CommitGameSkin(const char *pPath, bool AsDir)
{
	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_GAME, "game");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitGameSkin("default");
		else
			CommitGameSkin(pPath, true);
		return;
	}
	if(!LoadedAsset.IsLoaded() || !Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridy, true) || !Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
		return;

	CGameSkin OldSkin = m_GameSkin;
	m_GameSkin = {};
	const auto UnloadCurrentSkin = [this]() {
		if(!m_GameSkin.m_Loaded)
			return;

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHealthFull);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHealthEmpty);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteArmorFull);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteArmorEmpty);

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammerCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGunCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgunCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenadeCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinjaCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaserCursor);

		for(auto &SpriteWeaponCursor : m_GameSkin.m_aSpriteWeaponCursors)
		{
			SpriteWeaponCursor = IGraphics::CTextureHandle();
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookChain);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookHead);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammer);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaser);

		for(auto &SpriteWeapon : m_GameSkin.m_aSpriteWeapons)
		{
			SpriteWeapon = IGraphics::CTextureHandle();
		}

		for(auto &SpriteParticle : m_GameSkin.m_aSpriteParticles)
		{
			Graphics()->UnloadTexture(&SpriteParticle);
		}

		for(auto &SpriteStar : m_GameSkin.m_aSpriteStars)
		{
			Graphics()->UnloadTexture(&SpriteStar);
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGunProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgunProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenadeProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammerProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinjaProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaserProjectile);

		for(auto &SpriteWeaponProjectile : m_GameSkin.m_aSpriteWeaponProjectiles)
		{
			SpriteWeaponProjectile = IGraphics::CTextureHandle();
		}

		for(int i = 0; i < 3; ++i)
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponGunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i]);

			for(auto &SpriteWeaponsMuzzle : m_GameSkin.m_aaSpriteWeaponsMuzzles)
			{
				SpriteWeaponsMuzzle[i] = IGraphics::CTextureHandle();
			}
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupHealth);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupFreeze);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorLaser);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupLaser);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupHammer);

		for(auto &SpritePickupWeapon : m_GameSkin.m_aSpritePickupWeapons)
		{
			SpritePickupWeapon = IGraphics::CTextureHandle();
		}

		for(auto &SpritePickupWeaponArmor : m_GameSkin.m_aSpritePickupWeaponArmor)
		{
			SpritePickupWeaponArmor = IGraphics::CTextureHandle();
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteFlagBlue);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteFlagRed);

		if(m_GameSkin.IsSixup())
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarFullLeft);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarFull);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarEmpty);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarEmptyRight);
		}

		m_GameSkin.m_Loaded = false;
	};

	const bool HasNinjaBar =
		!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]) ||
		!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]) ||
		!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]) ||
		!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]);
	{
		m_GameSkin.m_SpriteHealthFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HEALTH_FULL]);
		m_GameSkin.m_SpriteHealthEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HEALTH_EMPTY]);
		m_GameSkin.m_SpriteArmorFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_ARMOR_FULL]);
		m_GameSkin.m_SpriteArmorEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_ARMOR_EMPTY]);

		m_GameSkin.m_SpriteWeaponHammerCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_HAMMER_CURSOR]);
		m_GameSkin.m_SpriteWeaponGunCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_CURSOR]);
		m_GameSkin.m_SpriteWeaponShotgunCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_CURSOR]);
		m_GameSkin.m_SpriteWeaponGrenadeCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_CURSOR]);
		m_GameSkin.m_SpriteWeaponNinjaCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_CURSOR]);
		m_GameSkin.m_SpriteWeaponLaserCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_CURSOR]);

		m_GameSkin.m_aSpriteWeaponCursors[0] = m_GameSkin.m_SpriteWeaponHammerCursor;
		m_GameSkin.m_aSpriteWeaponCursors[1] = m_GameSkin.m_SpriteWeaponGunCursor;
		m_GameSkin.m_aSpriteWeaponCursors[2] = m_GameSkin.m_SpriteWeaponShotgunCursor;
		m_GameSkin.m_aSpriteWeaponCursors[3] = m_GameSkin.m_SpriteWeaponGrenadeCursor;
		m_GameSkin.m_aSpriteWeaponCursors[4] = m_GameSkin.m_SpriteWeaponLaserCursor;
		m_GameSkin.m_aSpriteWeaponCursors[5] = m_GameSkin.m_SpriteWeaponNinjaCursor;

		// weapons and hook
		m_GameSkin.m_SpriteHookChain = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HOOK_CHAIN]);
		m_GameSkin.m_SpriteHookHead = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HOOK_HEAD]);
		m_GameSkin.m_SpriteWeaponHammer = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_HAMMER_BODY]);
		m_GameSkin.m_SpriteWeaponGun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_BODY]);
		m_GameSkin.m_SpriteWeaponShotgun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_BODY]);
		m_GameSkin.m_SpriteWeaponGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_BODY]);
		m_GameSkin.m_SpriteWeaponNinja = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_BODY]);
		m_GameSkin.m_SpriteWeaponLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_BODY]);

		m_GameSkin.m_aSpriteWeapons[0] = m_GameSkin.m_SpriteWeaponHammer;
		m_GameSkin.m_aSpriteWeapons[1] = m_GameSkin.m_SpriteWeaponGun;
		m_GameSkin.m_aSpriteWeapons[2] = m_GameSkin.m_SpriteWeaponShotgun;
		m_GameSkin.m_aSpriteWeapons[3] = m_GameSkin.m_SpriteWeaponGrenade;
		m_GameSkin.m_aSpriteWeapons[4] = m_GameSkin.m_SpriteWeaponLaser;
		m_GameSkin.m_aSpriteWeapons[5] = m_GameSkin.m_SpriteWeaponNinja;

		// particles
		for(int i = 0; i < 9; ++i)
		{
			m_GameSkin.m_aSpriteParticles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART1 + i]);
		}

		// stars
		for(int i = 0; i < 3; ++i)
		{
			m_GameSkin.m_aSpriteStars[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_STAR1 + i]);
		}

		// projectiles
		m_GameSkin.m_SpriteWeaponGunProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_PROJ]);
		m_GameSkin.m_SpriteWeaponShotgunProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_PROJ]);
		m_GameSkin.m_SpriteWeaponGrenadeProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_PROJ]);

		// these weapons have no projectiles
		m_GameSkin.m_SpriteWeaponHammerProjectile = IGraphics::CTextureHandle();
		m_GameSkin.m_SpriteWeaponNinjaProjectile = IGraphics::CTextureHandle();

		m_GameSkin.m_SpriteWeaponLaserProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_PROJ]);

		m_GameSkin.m_aSpriteWeaponProjectiles[0] = m_GameSkin.m_SpriteWeaponHammerProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[1] = m_GameSkin.m_SpriteWeaponGunProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[2] = m_GameSkin.m_SpriteWeaponShotgunProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[3] = m_GameSkin.m_SpriteWeaponGrenadeProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[4] = m_GameSkin.m_SpriteWeaponLaserProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[5] = m_GameSkin.m_SpriteWeaponNinjaProjectile;

		// muzzles
		for(int i = 0; i < 3; ++i)
		{
			m_GameSkin.m_aSpriteWeaponGunMuzzles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_MUZZLE1 + i]);
			m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_MUZZLE1 + i]);
			m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_MUZZLE1 + i]);

			m_GameSkin.m_aaSpriteWeaponsMuzzles[1][i] = m_GameSkin.m_aSpriteWeaponGunMuzzles[i];
			m_GameSkin.m_aaSpriteWeaponsMuzzles[2][i] = m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i];
			m_GameSkin.m_aaSpriteWeaponsMuzzles[5][i] = m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i];
		}

		// pickups
		m_GameSkin.m_SpritePickupHealth = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_HEALTH]);
		m_GameSkin.m_SpritePickupFreeze = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_FREEZE]);
		m_GameSkin.m_SpritePickupArmor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR]);
		m_GameSkin.m_SpritePickupHammer = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_HAMMER]);
		m_GameSkin.m_SpritePickupGun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_GUN]);
		m_GameSkin.m_SpritePickupShotgun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_SHOTGUN]);
		m_GameSkin.m_SpritePickupGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_GRENADE]);
		m_GameSkin.m_SpritePickupLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_LASER]);
		m_GameSkin.m_SpritePickupNinja = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_NINJA]);
		m_GameSkin.m_SpritePickupArmorShotgun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_SHOTGUN]);
		m_GameSkin.m_SpritePickupArmorGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_GRENADE]);
		m_GameSkin.m_SpritePickupArmorNinja = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_NINJA]);
		m_GameSkin.m_SpritePickupArmorLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_LASER]);

		m_GameSkin.m_aSpritePickupWeapons[0] = m_GameSkin.m_SpritePickupHammer;
		m_GameSkin.m_aSpritePickupWeapons[1] = m_GameSkin.m_SpritePickupGun;
		m_GameSkin.m_aSpritePickupWeapons[2] = m_GameSkin.m_SpritePickupShotgun;
		m_GameSkin.m_aSpritePickupWeapons[3] = m_GameSkin.m_SpritePickupGrenade;
		m_GameSkin.m_aSpritePickupWeapons[4] = m_GameSkin.m_SpritePickupLaser;
		m_GameSkin.m_aSpritePickupWeapons[5] = m_GameSkin.m_SpritePickupNinja;

		m_GameSkin.m_aSpritePickupWeaponArmor[0] = m_GameSkin.m_SpritePickupArmorShotgun;
		m_GameSkin.m_aSpritePickupWeaponArmor[1] = m_GameSkin.m_SpritePickupArmorGrenade;
		m_GameSkin.m_aSpritePickupWeaponArmor[2] = m_GameSkin.m_SpritePickupArmorNinja;
		m_GameSkin.m_aSpritePickupWeaponArmor[3] = m_GameSkin.m_SpritePickupArmorLaser;

		// flags
		m_GameSkin.m_SpriteFlagBlue = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_FLAG_BLUE]);
		m_GameSkin.m_SpriteFlagRed = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_FLAG_RED]);

		// ninja bar (0.7)
		if(HasNinjaBar)
		{
			m_GameSkin.m_SpriteNinjaBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]);
			m_GameSkin.m_SpriteNinjaBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]);
			m_GameSkin.m_SpriteNinjaBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]);
			m_GameSkin.m_SpriteNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]);
		}

		m_GameSkin.m_Loaded = true;
	}
	const IGraphics::CTextureHandle aRequiredTextures[] = {
		m_GameSkin.m_SpriteHealthFull,
		m_GameSkin.m_SpriteHealthEmpty,
		m_GameSkin.m_SpriteArmorFull,
		m_GameSkin.m_SpriteArmorEmpty,
		m_GameSkin.m_SpriteHookChain,
		m_GameSkin.m_SpriteHookHead,
		m_GameSkin.m_SpriteWeaponGunProjectile,
		m_GameSkin.m_SpriteWeaponShotgunProjectile,
		m_GameSkin.m_SpriteWeaponGrenadeProjectile,
		m_GameSkin.m_SpriteWeaponLaserProjectile,
		m_GameSkin.m_SpritePickupHealth,
		m_GameSkin.m_SpritePickupArmor,
		m_GameSkin.m_SpriteFlagBlue,
		m_GameSkin.m_SpriteFlagRed,
	};
	const bool NinjaBarValid = !HasNinjaBar ||
				   (m_GameSkin.m_SpriteNinjaBarFullLeft.IsValid() && m_GameSkin.m_SpriteNinjaBarFull.IsValid() &&
					   m_GameSkin.m_SpriteNinjaBarEmpty.IsValid() && m_GameSkin.m_SpriteNinjaBarEmptyRight.IsValid());
	if(!AllTexturesValid(aRequiredTextures) ||
		!AllTexturesValid(m_GameSkin.m_aSpriteWeaponCursors) ||
		!AllTexturesValid(m_GameSkin.m_aSpriteWeapons) ||
		!AllTexturesValid(m_GameSkin.m_aSpriteParticles) ||
		!AllTexturesValid(m_GameSkin.m_aSpriteStars) ||
		!AllTexturesValid(m_GameSkin.m_aSpriteWeaponGunMuzzles) ||
		!AllTexturesValid(m_GameSkin.m_aSpriteWeaponShotgunMuzzles) ||
		!AllTexturesValid(m_GameSkin.m_aaSpriteWeaponNinjaMuzzles) ||
		!AllTexturesValid(m_GameSkin.m_aSpritePickupWeapons) ||
		!AllTexturesValid(m_GameSkin.m_aSpritePickupWeaponArmor) ||
		!NinjaBarValid)
	{
		log_error("asset_loader", "Failed to upload all game skin textures from '%s'", LoadedAsset.m_aPath);
		UnloadCurrentSkin();
		m_GameSkin = OldSkin;
		return;
	}
	CGameSkin NewSkin = m_GameSkin;
	m_GameSkin = OldSkin;
	UnloadCurrentSkin();
	m_GameSkin = NewSkin;
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitEmoticonsSkin(const char *pPath, bool AsDir)
{
	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_EMOTICONS, "emoticons");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitEmoticonsSkin("default");
		else
			CommitEmoticonsSkin(pPath, true);
		return;
	}
	if(!LoadedAsset.IsLoaded() || !Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridy, true) || !Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
		return;

	CEmoticonsSkin OldSkin = m_EmoticonsSkin;
	m_EmoticonsSkin = {};
	const auto UnloadCurrentSkin = [this]() {
		if(!m_EmoticonsSkin.m_Loaded)
			return;

		for(auto &SpriteEmoticon : m_EmoticonsSkin.m_aSpriteEmoticons)
			Graphics()->UnloadTexture(&SpriteEmoticon);

		m_EmoticonsSkin.m_Loaded = false;
	};

	{
		for(int i = 0; i < 16; ++i)
			m_EmoticonsSkin.m_aSpriteEmoticons[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_OOP + i]);

		m_EmoticonsSkin.m_Loaded = true;
	}
	if(!AllTexturesValid(m_EmoticonsSkin.m_aSpriteEmoticons))
	{
		log_error("asset_loader", "Failed to upload all emoticon skin textures from '%s'", LoadedAsset.m_aPath);
		UnloadCurrentSkin();
		m_EmoticonsSkin = OldSkin;
		return;
	}
	CEmoticonsSkin NewSkin = m_EmoticonsSkin;
	m_EmoticonsSkin = OldSkin;
	UnloadCurrentSkin();
	m_EmoticonsSkin = NewSkin;
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitParticlesSkin(const char *pPath, bool AsDir)
{
	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_PARTICLES, "particles");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitParticlesSkin("default");
		else
			CommitParticlesSkin(pPath, true);
		return;
	}
	if(!LoadedAsset.IsLoaded() || !Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridy, true) || !Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
		return;

	CParticlesSkin OldSkin = m_ParticlesSkin;
	m_ParticlesSkin = {};
	const auto UnloadCurrentSkin = [this]() {
		if(!m_ParticlesSkin.m_Loaded)
			return;

		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleSlice);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleBall);
		for(auto &SpriteParticleSplat : m_ParticlesSkin.m_aSpriteParticleSplat)
			Graphics()->UnloadTexture(&SpriteParticleSplat);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleSmoke);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleShell);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleExpl);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleAirJump);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleHit);

		std::fill(std::begin(m_ParticlesSkin.m_aSpriteParticles), std::end(m_ParticlesSkin.m_aSpriteParticles), IGraphics::CTextureHandle());

		m_ParticlesSkin.m_Loaded = false;
	};

	{
		m_ParticlesSkin.m_SpriteParticleSlice = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SLICE]);
		m_ParticlesSkin.m_SpriteParticleBall = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_BALL]);
		for(int i = 0; i < 3; ++i)
			m_ParticlesSkin.m_aSpriteParticleSplat[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SPLAT01 + i]);
		m_ParticlesSkin.m_SpriteParticleSmoke = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SMOKE]);
		m_ParticlesSkin.m_SpriteParticleShell = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SHELL]);
		m_ParticlesSkin.m_SpriteParticleExpl = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_EXPL01]);
		m_ParticlesSkin.m_SpriteParticleAirJump = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_AIRJUMP]);
		m_ParticlesSkin.m_SpriteParticleHit = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_HIT01]);

		m_ParticlesSkin.m_aSpriteParticles[0] = m_ParticlesSkin.m_SpriteParticleSlice;
		m_ParticlesSkin.m_aSpriteParticles[1] = m_ParticlesSkin.m_SpriteParticleBall;
		for(int i = 0; i < 3; ++i)
			m_ParticlesSkin.m_aSpriteParticles[2 + i] = m_ParticlesSkin.m_aSpriteParticleSplat[i];
		m_ParticlesSkin.m_aSpriteParticles[5] = m_ParticlesSkin.m_SpriteParticleSmoke;
		m_ParticlesSkin.m_aSpriteParticles[6] = m_ParticlesSkin.m_SpriteParticleShell;
		m_ParticlesSkin.m_aSpriteParticles[7] = m_ParticlesSkin.m_SpriteParticleExpl;
		m_ParticlesSkin.m_aSpriteParticles[8] = m_ParticlesSkin.m_SpriteParticleAirJump;
		m_ParticlesSkin.m_aSpriteParticles[9] = m_ParticlesSkin.m_SpriteParticleHit;

		m_ParticlesSkin.m_Loaded = true;
	}
	if(!AllTexturesValid(m_ParticlesSkin.m_aSpriteParticles))
	{
		log_error("asset_loader", "Failed to upload all particle skin textures from '%s'", LoadedAsset.m_aPath);
		UnloadCurrentSkin();
		m_ParticlesSkin = OldSkin;
		return;
	}
	CParticlesSkin NewSkin = m_ParticlesSkin;
	m_ParticlesSkin = OldSkin;
	UnloadCurrentSkin();
	m_ParticlesSkin = NewSkin;
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitHudSkin(const char *pPath, bool AsDir)
{
	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_HUD, "hud");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitHudSkin("default");
		else
			CommitHudSkin(pPath, true);
		return;
	}
	if(!LoadedAsset.IsLoaded() || !Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridy, true) || !Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
		return;

	CHudSkin OldSkin = m_HudSkin;
	m_HudSkin = {};
	const auto UnloadCurrentSkin = [this]() {
		if(!m_HudSkin.m_Loaded)
			return;

		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudAirjump);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudAirjumpEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudSolo);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudCollisionDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudEndlessJump);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudEndlessHook);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudJetpack);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarFullLeft);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarFull);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarEmptyRight);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarFullLeft);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarFull);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudHookHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudHammerHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudShotgunHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudGrenadeHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLaserHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudGunHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDeepFrozen);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLiveFrozen);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportGrenade);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportGun);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportLaser);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudPracticeMode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLockMode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeam0Mode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDummyHammer);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDummyCopy);
		m_HudSkin.m_Loaded = false;
	};

	{
		m_HudSkin.m_SpriteHudAirjump = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_AIRJUMP]);
		m_HudSkin.m_SpriteHudAirjumpEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_AIRJUMP_EMPTY]);
		m_HudSkin.m_SpriteHudSolo = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_SOLO]);
		m_HudSkin.m_SpriteHudCollisionDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_COLLISION_DISABLED]);
		m_HudSkin.m_SpriteHudEndlessJump = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_ENDLESS_JUMP]);
		m_HudSkin.m_SpriteHudEndlessHook = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_ENDLESS_HOOK]);
		m_HudSkin.m_SpriteHudJetpack = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_JETPACK]);
		m_HudSkin.m_SpriteHudFreezeBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_FULL_LEFT]);
		m_HudSkin.m_SpriteHudFreezeBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_FULL]);
		m_HudSkin.m_SpriteHudFreezeBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_EMPTY]);
		m_HudSkin.m_SpriteHudFreezeBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_EMPTY_RIGHT]);
		m_HudSkin.m_SpriteHudNinjaBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_FULL_LEFT]);
		m_HudSkin.m_SpriteHudNinjaBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_FULL]);
		m_HudSkin.m_SpriteHudNinjaBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_EMPTY]);
		m_HudSkin.m_SpriteHudNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_EMPTY_RIGHT]);
		m_HudSkin.m_SpriteHudHookHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_HOOK_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudHammerHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_HAMMER_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudShotgunHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_SHOTGUN_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudGrenadeHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_GRENADE_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudLaserHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LASER_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudGunHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_GUN_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudDeepFrozen = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DEEP_FROZEN]);
		m_HudSkin.m_SpriteHudLiveFrozen = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LIVE_FROZEN]);
		m_HudSkin.m_SpriteHudTeleportGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_GRENADE]);
		m_HudSkin.m_SpriteHudTeleportGun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_GUN]);
		m_HudSkin.m_SpriteHudTeleportLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_LASER]);
		m_HudSkin.m_SpriteHudPracticeMode = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_PRACTICE_MODE]);
		m_HudSkin.m_SpriteHudLockMode = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LOCK_MODE]);
		m_HudSkin.m_SpriteHudTeam0Mode = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TEAM0_MODE]);
		m_HudSkin.m_SpriteHudDummyHammer = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DUMMY_HAMMER]);
		m_HudSkin.m_SpriteHudDummyCopy = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DUMMY_COPY]);

		m_HudSkin.m_Loaded = true;
	}
	const IGraphics::CTextureHandle aRequiredTextures[] = {
		m_HudSkin.m_SpriteHudAirjump,
		m_HudSkin.m_SpriteHudAirjumpEmpty,
		m_HudSkin.m_SpriteHudSolo,
		m_HudSkin.m_SpriteHudCollisionDisabled,
		m_HudSkin.m_SpriteHudEndlessJump,
		m_HudSkin.m_SpriteHudEndlessHook,
		m_HudSkin.m_SpriteHudJetpack,
		m_HudSkin.m_SpriteHudFreezeBarFullLeft,
		m_HudSkin.m_SpriteHudFreezeBarFull,
		m_HudSkin.m_SpriteHudFreezeBarEmpty,
		m_HudSkin.m_SpriteHudFreezeBarEmptyRight,
		m_HudSkin.m_SpriteHudNinjaBarFullLeft,
		m_HudSkin.m_SpriteHudNinjaBarFull,
		m_HudSkin.m_SpriteHudNinjaBarEmpty,
		m_HudSkin.m_SpriteHudNinjaBarEmptyRight,
		m_HudSkin.m_SpriteHudHookHitDisabled,
		m_HudSkin.m_SpriteHudHammerHitDisabled,
		m_HudSkin.m_SpriteHudShotgunHitDisabled,
		m_HudSkin.m_SpriteHudGrenadeHitDisabled,
		m_HudSkin.m_SpriteHudLaserHitDisabled,
		m_HudSkin.m_SpriteHudGunHitDisabled,
		m_HudSkin.m_SpriteHudDeepFrozen,
		m_HudSkin.m_SpriteHudLiveFrozen,
		m_HudSkin.m_SpriteHudTeleportGrenade,
		m_HudSkin.m_SpriteHudTeleportGun,
		m_HudSkin.m_SpriteHudTeleportLaser,
		m_HudSkin.m_SpriteHudPracticeMode,
		m_HudSkin.m_SpriteHudLockMode,
		m_HudSkin.m_SpriteHudTeam0Mode,
		m_HudSkin.m_SpriteHudDummyHammer,
		m_HudSkin.m_SpriteHudDummyCopy,
	};
	if(!AllTexturesValid(aRequiredTextures))
	{
		log_error("asset_loader", "Failed to upload all HUD skin textures from '%s'", LoadedAsset.m_aPath);
		UnloadCurrentSkin();
		m_HudSkin = OldSkin;
		return;
	}
	CHudSkin NewSkin = m_HudSkin;
	m_HudSkin = OldSkin;
	UnloadCurrentSkin();
	m_HudSkin = NewSkin;
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitExtrasSkin(const char *pPath, bool AsDir)
{
	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_EXTRAS, "extras");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitExtrasSkin("default");
		else
			CommitExtrasSkin(pPath, true);
		return;
	}
	if(!LoadedAsset.IsLoaded() || !Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridy, true) || !Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
		return;

	CExtrasSkin OldSkin = m_ExtrasSkin;
	m_ExtrasSkin = {};
	const auto UnloadCurrentSkin = [this]() {
		if(!m_ExtrasSkin.m_Loaded)
			return;

		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSnowflake);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSparkle);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpritePulley);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteHectagon);

		std::fill(std::begin(m_ExtrasSkin.m_aSpriteParticles), std::end(m_ExtrasSkin.m_aSpriteParticles), IGraphics::CTextureHandle());

		m_ExtrasSkin.m_Loaded = false;
	};

	{
		m_ExtrasSkin.m_SpriteParticleSnowflake = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE]);
		m_ExtrasSkin.m_SpriteParticleSparkle = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SPARKLE]);
		m_ExtrasSkin.m_SpritePulley = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_PULLEY]);
		m_ExtrasSkin.m_SpriteHectagon = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_HECTAGON]);

		m_ExtrasSkin.m_aSpriteParticles[0] = m_ExtrasSkin.m_SpriteParticleSnowflake;
		m_ExtrasSkin.m_aSpriteParticles[1] = m_ExtrasSkin.m_SpriteParticleSparkle;
		m_ExtrasSkin.m_aSpriteParticles[2] = m_ExtrasSkin.m_SpritePulley;
		m_ExtrasSkin.m_aSpriteParticles[3] = m_ExtrasSkin.m_SpriteHectagon;

		m_ExtrasSkin.m_Loaded = true;
	}
	if(!AllTexturesValid(m_ExtrasSkin.m_aSpriteParticles))
	{
		log_error("asset_loader", "Failed to upload all extras skin textures from '%s'", LoadedAsset.m_aPath);
		UnloadCurrentSkin();
		m_ExtrasSkin = OldSkin;
		return;
	}
	CExtrasSkin NewSkin = m_ExtrasSkin;
	m_ExtrasSkin = OldSkin;
	UnloadCurrentSkin();
	m_ExtrasSkin = NewSkin;
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}
