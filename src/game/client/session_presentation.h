#ifndef GAME_CLIENT_SESSION_PRESENTATION_H
#define GAME_CLIENT_SESSION_PRESENTATION_H

#include <engine/client/session.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>
#include <game/client/components/mapimages.h>
#include <game/client/components/maplayers.h>
#include <game/client/components/mapsounds.h>
#include <game/client/render.h>

#include <array>
#include <memory>
#include <vector>

class CGameSessionContext;
class CGameState;
class CGameView;
class CMapData;
class CPresentationContext;

enum class EPlayerSpeedChange
{
	NONE,
	INCREASE,
	DECREASE,
};

class CClientPresentation
{
public:
	bool m_Active = false;
	char m_aName[MAX_NAME_LENGTH] = {};
	char m_aClan[MAX_CLAN_LENGTH] = {};
	char m_aSkinName[MAX_SKIN_LENGTH] = {};
	int m_Team = 0;
	int m_Country = CountryCode::DEFAULT;
	bool m_Friend = false;
	bool m_DirectionLeft = false;
	bool m_DirectionJump = false;
	bool m_DirectionRight = false;
	std::array<int, 2> m_aSpeed = {};
	std::array<EPlayerSpeedChange, 2> m_aSpeedChange = {};
	CTeeRenderInfo m_BaseRenderInfo;
	CTeeRenderInfo m_RenderInfo;
};

// What a session shows of the clients of one of its game states.
class CStateClientPresentation
{
public:
	std::array<CClientPresentation, MAX_CLIENTS> m_aClients;
	std::array<std::shared_ptr<CManagedTeeRenderInfo>, MAX_CLIENTS> m_apSkinInfos;
	std::array<int, MAX_CLIENTS> m_aClientsByName;
	std::array<int, MAX_CLIENTS> m_aClientsByScore;
	std::array<int, MAX_CLIENTS> m_aClientsByDDTeamName;
	std::array<int, MAX_CLIENTS> m_aClientsByDDTeamScore;
	std::array<int, 2> m_aTeamSize = {};
	int m_SpectatorCount = 0;
	int m_LastZeroSpectatorCountTick = 0;
	int m_LastMovementSnapshotTick = -1;

	CStateClientPresentation();
};

/**
 * What draws a map: its textures and the layers built from them, which hold
 * the buffers and chunk caches on the graphics card. Sessions playing the same
 * map share one, like the panes of a split screen do; every view sets the
 * envelope time right before it draws.
 */
class CMapPresentation : public CComponentInterfaces
{
	std::shared_ptr<CMapData> m_pData;
	bool m_Sixup;

public:
	CMapRenderImages m_Images;
	CMapLayers m_LayersBackground{ERenderType::RENDERTYPE_BACKGROUND};
	CMapLayers m_LayersForeground{ERenderType::RENDERTYPE_FOREGROUND};
	CMapLayers m_LayersBackgroundForce{ERenderType::RENDERTYPE_BACKGROUND_FORCE};

	CMapPresentation(std::shared_ptr<CMapData> pData, bool Sixup, CMapImages &SharedMapImages);
	~CMapPresentation() override;
	void OnInterfacesInit(CGameClient *pClient) override;
	void Load();
	bool Draws(const CMapData *pData, bool Sixup) const { return m_pData.get() == pData && m_Sixup == Sixup; }
};

class CSessionPresentation : public CComponentInterfaces
{
	CSessionId m_SessionId;
	std::shared_ptr<CMapPresentation> m_pMap;
	CMapSounds m_MapSounds;
	std::array<CStateClientPresentation, NUM_DUMMIES> m_aStates;
	std::array<bool, MAX_CLIENTS> m_aChatIgnored = {};
	std::array<bool, MAX_CLIENTS> m_aEmoticonIgnored = {};
	bool GetClientSkinDescriptor(const CGameState &State, int ClientId, char *pSkinName, int SkinNameSize, CSkinDescriptor &SkinDescriptor) const;
	void ApplyClientColors(const CGameState &State, int ClientId, int Team, CTeeRenderInfo &RenderInfo) const;

public:
	explicit CSessionPresentation(CSessionId SessionId);
	~CSessionPresentation() override;

	void OnInterfacesInit(CGameClient *pClient) override;
	void Load(CGameSessionContext &Session, std::shared_ptr<CMapPresentation> pMap);
	void Unload();
	bool UpdateMapImages() { return m_pMap->m_Images.Update(); }
	void PrepareRender(const CRenderContext &Context, bool UsePredictedTime);
	/**
	 * Plays the map's sounds as heard from a view, and leaves out those of
	 * detail layers where the view draws no detail.
	 */
	void UpdateMapSounds(const CGameState &State, const CGameTickInfo &Time, const CGameView &View, bool UsePredictedTime, bool Offline = false);
	void UpdateClients(const CPresentationContext &Context);
	std::shared_ptr<CManagedTeeRenderInfo> CreateClientTee(const CGameState &State, int ClientId) const;
	const CClientPresentation *Client(int Seat, int ClientId) const;
	const std::array<int, MAX_CLIENTS> *ClientsByName(int Seat) const { return &m_aStates[Seat].m_aClientsByName; }
	const std::array<int, MAX_CLIENTS> *ClientsByScore(int Seat) const { return &m_aStates[Seat].m_aClientsByScore; }
	const std::array<int, MAX_CLIENTS> *ClientsByDDTeamName(int Seat) const { return &m_aStates[Seat].m_aClientsByDDTeamName; }
	const std::array<int, MAX_CLIENTS> *ClientsByDDTeamScore(int Seat) const { return &m_aStates[Seat].m_aClientsByDDTeamScore; }
	int TeamSize(int Seat, int Team) const;
	bool GetSpectatorCount(int Seat, int &Count, int &LastZeroTick) const;
	bool EmoticonIgnored(int ClientId) const { return m_aEmoticonIgnored[ClientId]; }
	void ToggleEmoticonIgnored(int ClientId) { m_aEmoticonIgnored[ClientId] = !m_aEmoticonIgnored[ClientId]; }
	bool ChatIgnored(int ClientId) const { return m_aChatIgnored[ClientId]; }
	void ToggleChatIgnored(int ClientId) { m_aChatIgnored[ClientId] = !m_aChatIgnored[ClientId]; }

	CSessionId Id() const { return m_SessionId; }
	bool IsLoaded() const { return m_pMap != nullptr; }
	const std::shared_ptr<CMapPresentation> &MapPresentation() const { return m_pMap; }
	// None before a map is loaded, when the world is drawn without one.
	CMapLayers *MapLayersBackground() { return m_pMap ? &m_pMap->m_LayersBackground : nullptr; }
	CMapLayers *MapLayersForeground() { return m_pMap ? &m_pMap->m_LayersForeground : nullptr; }
	CMapLayers *MapLayersBackgroundForce() { return m_pMap ? &m_pMap->m_LayersBackgroundForce : nullptr; }
	CMapSounds &MapSounds() { return m_MapSounds; }
};

class CSessionPresentationManager
{
	CMapImages &m_SharedMapImages;
	CGameClient *m_pGameClient = nullptr;
	std::vector<std::unique_ptr<CSessionPresentation>> m_vpPresentations;
	CSessionId m_AudibleSessionId;

public:
	explicit CSessionPresentationManager(CMapImages &SharedMapImages);

	void OnInterfacesInit(CGameClient *pClient);
	CSessionPresentation *Create(CSessionId SessionId);
	/**
	 * The presentation of a map, shared with every session that already
	 * draws it.
	 */
	std::shared_ptr<CMapPresentation> MapPresentation(const std::shared_ptr<CMapData> &pData, bool Sixup);
	CSessionPresentation *Find(CSessionId SessionId) const { return FindSessionEntry(m_vpPresentations, SessionId); }
	void SetAudible(CSessionId SessionId);
	// Stops the map sound voices of every session, for when sound is switched off.
	void StopMapSounds();
	void Unload(CSessionId SessionId);
	void UnloadAll();
};

#endif // GAME_CLIENT_SESSION_PRESENTATION_H
