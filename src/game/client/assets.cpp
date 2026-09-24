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

void CGameClient::StartLoadingCoreImages()
{
	m_vStartupImageLoads.clear();
	m_CoreImagesPending = true;

	for(int ImageId = 0; ImageId < g_pData->m_NumImages; ++ImageId)
	{
		if(ImageId == IMAGE_GAME)
			LoadGameSkin(g_Config.m_ClAssetGame);
		else if(ImageId == IMAGE_EMOTICONS)
			LoadEmoticonsSkin(g_Config.m_ClAssetEmoticons);
		else if(ImageId == IMAGE_PARTICLES)
			LoadParticlesSkin(g_Config.m_ClAssetParticles);
		else if(ImageId == IMAGE_HUD)
			LoadHudSkin(g_Config.m_ClAssetHud);
		else if(ImageId == IMAGE_EXTRAS)
			LoadExtrasSkin(g_Config.m_ClAssetExtras);
		else if(g_pData->m_aImages[ImageId].m_pFilename[0] != '\0')
			m_vStartupImageLoads.push_back({ImageId, m_AssetLoader.LoadImageFile(Storage(), g_pData->m_aImages[ImageId].m_pFilename, IStorage::TYPE_ALL)});
	}
}

void CGameClient::FinishLoadingCoreImages()
{
	UpdateAssetPackLoads();
	for(auto It = m_vStartupImageLoads.begin(); It != m_vStartupImageLoads.end();)
	{
		IGraphics::CTextureHandle &Texture = g_pData->m_aImages[It->m_ImageId].m_Id;
		if(!It->m_Resource.FinishTexture(Graphics(), Texture))
		{
			++It;
			continue;
		}
		if(!Texture.IsValid())
			Texture = Graphics()->LoadTexture(g_pData->m_aImages[It->m_ImageId].m_pFilename, IStorage::TYPE_ALL); // null texture
		It = m_vStartupImageLoads.erase(It);
	}
	if(m_vStartupImageLoads.empty() && m_vAssetPackLoads.empty())
	{
		m_CoreImagesPending = false;
		m_Menus.FinishLoading();
	}
}

void CGameClient::TryFinishStartupAssets()
{
	if(!m_StartupAssetsPending || m_CoreImagesPending || !m_Sounds.StartupAssetsLoaded() || !m_Skins.StartupAssetsLoaded() || !m_Skins7.StartupAssetsLoaded() || !m_Menus.StartupAssetsLoaded() || !m_CountryFlags.StartupAssetsLoaded() || !m_Scoreboard.StartupAssetsLoaded())
		return;
	m_StartupAssetsPending = false;
	log_info("asset_loader", "Client startup assets complete: wall=%.2fms", (time_get() - m_StartupAssetsStart) * 1000.0 / time_freq());
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

	m_vAssetPackLoads.erase(
		std::remove_if(m_vAssetPackLoads.begin(), m_vAssetPackLoads.end(), [ImageId](const CAssetPackLoad &Load) { return Load.m_ImageId == ImageId; }),
		m_vAssetPackLoads.end());

	CAssetPackLoad Load;
	Load.m_ImageId = ImageId;
	Load.m_Name = pName;
	Load.m_AsDir = AsDir;
	const auto Submit = [&](const char *pPath) {
		Load.m_vResources.push_back(m_AssetLoader.LoadImageFile(Storage(), pPath, IStorage::TYPE_ALL));
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
			if(!Resource.IsReady())
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
	if(m_GameSkin.m_Loaded)
	{
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

		std::fill(std::begin(m_GameSkin.m_aSpriteWeaponCursors), std::end(m_GameSkin.m_aSpriteWeaponCursors), IGraphics::CTextureHandle());

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookChain);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookHead);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammer);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaser);

		std::fill(std::begin(m_GameSkin.m_aSpriteWeapons), std::end(m_GameSkin.m_aSpriteWeapons), IGraphics::CTextureHandle());

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

		std::fill(std::begin(m_GameSkin.m_aSpriteWeaponProjectiles), std::end(m_GameSkin.m_aSpriteWeaponProjectiles), IGraphics::CTextureHandle());

		for(int i = 0; i < 3; ++i)
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponGunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i]);
		}
		for(auto &aSpriteWeaponsMuzzles : m_GameSkin.m_aaSpriteWeaponsMuzzles)
		{
			std::fill(std::begin(aSpriteWeaponsMuzzles), std::end(aSpriteWeaponsMuzzles), IGraphics::CTextureHandle());
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

		std::fill(std::begin(m_GameSkin.m_aSpritePickupWeapons), std::end(m_GameSkin.m_aSpritePickupWeapons), IGraphics::CTextureHandle());
		std::fill(std::begin(m_GameSkin.m_aSpritePickupWeaponArmor), std::end(m_GameSkin.m_aSpritePickupWeaponArmor), IGraphics::CTextureHandle());

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
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_GAME, "game");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitGameSkin("default");
		else
			CommitGameSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
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
		if(!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]))
		{
			m_GameSkin.m_SpriteNinjaBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]);
			m_GameSkin.m_SpriteNinjaBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]);
			m_GameSkin.m_SpriteNinjaBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]);
			m_GameSkin.m_SpriteNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]);
		}

		m_GameSkin.m_Loaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitEmoticonsSkin(const char *pPath, bool AsDir)
{
	if(m_EmoticonsSkin.m_Loaded)
	{
		for(auto &SpriteEmoticon : m_EmoticonsSkin.m_aSpriteEmoticons)
			Graphics()->UnloadTexture(&SpriteEmoticon);

		m_EmoticonsSkin.m_Loaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_EMOTICONS, "emoticons");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitEmoticonsSkin("default");
		else
			CommitEmoticonsSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
	{
		for(int i = 0; i < 16; ++i)
			m_EmoticonsSkin.m_aSpriteEmoticons[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_OOP + i]);

		m_EmoticonsSkin.m_Loaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitParticlesSkin(const char *pPath, bool AsDir)
{
	if(m_ParticlesSkin.m_Loaded)
	{
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
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_PARTICLES, "particles");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitParticlesSkin("default");
		else
			CommitParticlesSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
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
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitHudSkin(const char *pPath, bool AsDir)
{
	if(m_HudSkin.m_Loaded)
	{
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
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_HUD, "hud");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitHudSkin("default");
		else
			CommitHudSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
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
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::CommitExtrasSkin(const char *pPath, bool AsDir)
{
	if(m_ExtrasSkin.m_Loaded)
	{
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSnowflake);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSparkle);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpritePulley);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteHectagon);

		std::fill(std::begin(m_ExtrasSkin.m_aSpriteParticles), std::end(m_ExtrasSkin.m_aSpriteParticles), IGraphics::CTextureHandle());

		m_ExtrasSkin.m_Loaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_EXTRAS, "extras");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			CommitExtrasSkin("default");
		else
			CommitExtrasSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
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
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}
