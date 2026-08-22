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
class CGameStateId;
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

class CStateClientPresentation;

class CSessionPresentation : public CComponentInterfaces
{
	CSessionId m_SessionId;
	CMapRenderImages m_MapImages;
	CMapLayers m_MapLayersBackground{ERenderType::RENDERTYPE_BACKGROUND};
	CMapLayers m_MapLayersForeground{ERenderType::RENDERTYPE_FOREGROUND};
	CMapLayers m_MapLayersBackgroundForce{ERenderType::RENDERTYPE_BACKGROUND_FORCE};
	CMapSounds m_MapSounds;
	std::vector<std::unique_ptr<CStateClientPresentation>> m_vpClientPresentations;
	std::array<bool, MAX_CLIENTS> m_aChatIgnored = {};
	std::array<bool, MAX_CLIENTS> m_aEmoticonIgnored = {};
	bool m_Loaded = false;
	bool GetClientSkinDescriptor(const CGameState &State, int ClientId, char *pSkinName, int SkinNameSize, CSkinDescriptor &SkinDescriptor) const;
	void ApplyClientColors(const CGameState &State, int ClientId, int Team, CTeeRenderInfo &RenderInfo) const;

public:
	CSessionPresentation(CSessionId SessionId, CMapImages &SharedMapImages);
	~CSessionPresentation() override;

	void OnInterfacesInit(CGameClient *pClient) override;
	void Load(CGameSessionContext &Session);
	void Unload();
	void PrepareRender(const CRenderContext &Context, bool UsePredictedTime);
	void UpdateMapSounds(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool UsePredictedTime);
	void UpdateClients(const CPresentationContext &Context);
	void RemoveState(CGameStateId StateId);
	std::shared_ptr<CManagedTeeRenderInfo> CreateClientTee(const CGameState &State, int ClientId) const;
	const CClientPresentation *Client(CGameStateId StateId, int ClientId) const;
	const std::array<int, MAX_CLIENTS> *ClientsByName(CGameStateId StateId) const;
	const std::array<int, MAX_CLIENTS> *ClientsByScore(CGameStateId StateId) const;
	const std::array<int, MAX_CLIENTS> *ClientsByDDTeamName(CGameStateId StateId) const;
	const std::array<int, MAX_CLIENTS> *ClientsByDDTeamScore(CGameStateId StateId) const;
	int TeamSize(CGameStateId StateId, int Team) const;
	bool GetSpectatorCount(CGameStateId StateId, int &Count, int &LastZeroTick) const;
	bool EmoticonIgnored(int ClientId) const { return m_aEmoticonIgnored[ClientId]; }
	void ToggleEmoticonIgnored(int ClientId) { m_aEmoticonIgnored[ClientId] = !m_aEmoticonIgnored[ClientId]; }
	bool ChatIgnored(int ClientId) const { return m_aChatIgnored[ClientId]; }
	void ToggleChatIgnored(int ClientId) { m_aChatIgnored[ClientId] = !m_aChatIgnored[ClientId]; }

	CSessionId SessionId() const { return m_SessionId; }
	bool IsLoaded() const { return m_Loaded; }
	CMapLayers &MapLayersBackground() { return m_MapLayersBackground; }
	CMapLayers &MapLayersForeground() { return m_MapLayersForeground; }
	CMapLayers &MapLayersBackgroundForce() { return m_MapLayersBackgroundForce; }
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
	CSessionPresentation *Find(CSessionId SessionId);
	const CSessionPresentation *Find(CSessionId SessionId) const;
	void SetAudible(CSessionId SessionId);
	void Unload(CSessionId SessionId);
	bool Destroy(CSessionId SessionId);
	void UnloadAll();
};

#endif // GAME_CLIENT_SESSION_PRESENTATION_H
