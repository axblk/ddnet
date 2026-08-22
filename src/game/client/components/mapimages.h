/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MAPIMAGES_H
#define GAME_CLIENT_COMPONENTS_MAPIMAGES_H

#include <engine/client/asset_loader.h>
#include <engine/console.h>
#include <engine/graphics.h>

#include <game/client/component.h>
#include <game/map/render_interfaces.h>
#include <game/mapitems.h>

enum EMapImageModType
{
	MAP_IMAGE_MOD_TYPE_DDNET = 0,
	MAP_IMAGE_MOD_TYPE_DDRACE,
	MAP_IMAGE_MOD_TYPE_RACE,
	MAP_IMAGE_MOD_TYPE_BLOCKWORLDS,
	MAP_IMAGE_MOD_TYPE_FNG,
	MAP_IMAGE_MOD_TYPE_VANILLA,
	MAP_IMAGE_MOD_TYPE_FDDRACE,

	MAP_IMAGE_MOD_TYPE_COUNT,
};

constexpr const char *const gs_apModEntitiesNames[] = {
	"ddnet",
	"ddrace",
	"race",
	"blockworlds",
	"fng",
	"vanilla",
	"f-ddrace",
};

constexpr int MapImageEntityVariant(EMapImageModType ModType, bool Masked)
{
	return static_cast<int>(ModType) * 2 + static_cast<int>(Masked);
}

static_assert(MapImageEntityVariant(MAP_IMAGE_MOD_TYPE_DDNET, false) != MapImageEntityVariant(MAP_IMAGE_MOD_TYPE_DDNET, true));
static_assert(MapImageEntityVariant(MAP_IMAGE_MOD_TYPE_FDDRACE, true) == MAP_IMAGE_MOD_TYPE_COUNT * 2 - 1);

class CGameInfo;

class CMapImages : public CComponent
{
	char m_aEntitiesPath[IO_MAX_PATH_LENGTH];

public:
	CMapImages();
	int Sizeof() const override { return sizeof(*this); }

	void OnInit() override;
	void OnUpdate() override;
	void OnShutdown() override;

	// DDRace
	IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType);
	IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType, EMapImageModType EntitiesModType, bool EntitiesAreMasked);
	IGraphics::CTextureHandle GetSpeedupArrow();
	IGraphics::CTextureHandle GetTuneColors();
	IGraphics::CTextureHandle GetTuneColors(EMapImageModType EntitiesModType, bool EntitiesAreMasked);

	IGraphics::CTextureHandle GetOverlayBottom();
	IGraphics::CTextureHandle GetOverlayTop();
	IGraphics::CTextureHandle GetOverlayCenter();

	void SetTextureScale(int Scale);
	int GetTextureScale() const;

	void ChangeEntitiesPath(const char *pPath);

private:
	bool m_aEntitiesIsLoaded[MAP_IMAGE_MOD_TYPE_COUNT * 2];
	bool m_SpeedupArrowIsLoaded;
	bool m_aTuneColorsIsLoaded[MAP_IMAGE_MOD_TYPE_COUNT * 2];
	IGraphics::CTextureHandle m_aaEntitiesTextures[MAP_IMAGE_MOD_TYPE_COUNT * 2][MAP_IMAGE_ENTITY_LAYER_TYPE_COUNT];
	IGraphics::CTextureHandle m_SpeedupArrowTexture;
	IGraphics::CTextureHandle m_aTuneColorMapTextures[MAP_IMAGE_MOD_TYPE_COUNT * 2];
	IGraphics::CTextureHandle m_OverlayBottomTexture;
	IGraphics::CTextureHandle m_OverlayTopTexture;
	IGraphics::CTextureHandle m_OverlayCenterTexture;
	int m_TextureScale;
	class CEntitiesLoad
	{
	public:
		int m_EntityVariant;
		EMapImageModType m_ModType;
		bool m_Masked;
		std::vector<CImageResource> m_vResources;
	};
	std::vector<CEntitiesLoad> m_vEntitiesLoads;
	CImageResource m_SpeedupArrowResource;
	uint64_t m_AssetGeneration = 0;

	static void ConchainClTextEntitiesSize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	bool FinishEntitiesLoad(CEntitiesLoad &Load, CImageInfo ImgInfo, const char *pPath);
	void InitOverlayTextures();
	IGraphics::CTextureHandle UploadEntityLayerText(int TextureSize, int MaxWidth, int YOffset);
	void UpdateEntityLayerText(CImageInfo &TextImage, int TextureSize, int MaxWidth, int YOffset, int NumbersPower, int MaxNumber = -1);
};

class CMapRenderImages : public CComponentInterfaces, public IMapImages
{
	CMapImages &m_Assets;
	CAssetLoader *m_pAssetLoader = nullptr;
	IGraphics::CTextureHandle m_aTextures[MAX_MAPIMAGES];
	int m_Count = 0;
	EMapImageModType m_EntitiesModType = MAP_IMAGE_MOD_TYPE_DDNET;
	bool m_EntitiesAreMasked = true;
	class CExternalImageLoad
	{
	public:
		int m_Index;
		int m_LoadFlags;
		CImageResource m_Resource;
	};
	std::vector<CExternalImageLoad> m_vExternalImageLoads;
	uint64_t m_Generation = 0;
	int m_AssetOwnerId;

public:
	explicit CMapRenderImages(CMapImages &Assets);

	void OnInterfacesInit(CGameClient *pClient) override;
	void Load(class CLayers *pLayers, class IMap *pMap, bool Sixup);
	void Unload();
	void Update();
	void SetGameInfo(const CGameInfo &GameInfo);

	IGraphics::CTextureHandle Get(int Index) const override { return m_aTextures[Index]; }
	int Num() const override { return m_Count; }
	IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType) override;
	IGraphics::CTextureHandle GetSpeedupArrow() override { return m_Assets.GetSpeedupArrow(); }
	IGraphics::CTextureHandle GetTuneColors() override;
	IGraphics::CTextureHandle GetOverlayBottom() override { return m_Assets.GetOverlayBottom(); }
	IGraphics::CTextureHandle GetOverlayTop() override { return m_Assets.GetOverlayTop(); }
	IGraphics::CTextureHandle GetOverlayCenter() override { return m_Assets.GetOverlayCenter(); }
};

#endif
