#ifndef GAME_CLIENT_COMPONENTS_BACKGROUND_H
#define GAME_CLIENT_COMPONENTS_BACKGROUND_H

#include <engine/client/asset_loader.h>
#include <engine/map.h>

#include <game/client/components/maplayers.h>

#include <cstdint>
#include <memory>
#include <string>

class CLayers;
class CMapRenderImages;

// Special value to use background of current map
#define CURRENT_MAP "%current%"

class CBackground : public CMapLayers
{
protected:
	IMap *m_pMap;
	bool m_Loaded;
	bool m_UseCurrentMap;
	char m_aMapName[MAX_MAP_LENGTH];

	std::unique_ptr<IMap> m_pBackgroundMap;
	CLayers *m_pBackgroundLayers;
	CMapRenderImages *m_pBackgroundImages;

	// The map that is loaded, see `StartMapLoad`.
	CTypedAssetResource<CFileAssetJob> m_MapResource;
	std::string m_LoadingMapName;
	/**
	 * Loads a map through the asset loader, without waiting for it: in a
	 * browser it is fetched. `FinishMapLoad` takes it once it is here.
	 *
	 * @param pName The name of the map.
	 * @param pPath Its file.
	 */
	void StartMapLoad(const char *pName, const char *pPath);
	enum class EMapLoad
	{
		PENDING,
		LOADED,
		FAILED,
	};
	/**
	 * Loads the map that `StartMapLoad` asked for once it is here, with its
	 * layers and images.
	 */
	EMapLoad FinishMapLoad();

public:
	CBackground(ERenderType MapType = ERenderType::RENDERTYPE_BACKGROUND_FORCE, bool OnlineOnly = true);
	~CBackground() override;
	int Sizeof() const override { return sizeof(*this); }

	void OnInterfacesInit(CGameClient *pClient) override;
	void OnInit() override;
	void OnMapLoad() override;
	void OnRender(const CRenderContext &Context) override;
	void OnShutdown() override;
	void OnUpdate() override;
	void OnCollectCriticalAssets(CFirstFrameGate::EScene Scene, CSessionId SessionId, CCriticalAssets &Pending) const override;

	void LoadBackground();
	bool UsesCurrentMap() const { return m_UseCurrentMap; }
	const char *MapName() const { return m_aMapName; }
};

#endif
