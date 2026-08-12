#include "session_presentation.h"

#include <base/dbg.h>
#include <base/time.h>

#include <game/client/game_view.h>
#include <game/client/gameclient.h>
#include <game/client/session_context.h>

#include <algorithm>

class CStateClientPresentation
{
public:
	CGameStateId m_StateId;
	std::array<CClientPresentation, MAX_CLIENTS> m_aClients;
	std::array<std::shared_ptr<CManagedTeeRenderInfo>, MAX_CLIENTS> m_apSkinInfos;
	std::array<int, MAX_CLIENTS> m_aClientsByName;
	std::array<int, MAX_CLIENTS> m_aClientsByScore;
	std::array<int, MAX_CLIENTS> m_aClientsByDDTeamName;
	std::array<int, MAX_CLIENTS> m_aClientsByDDTeamScore;
	std::array<int, 2> m_aTeamSize = {};
	int m_SpectatorCount = 0;
	int m_LastZeroSpectatorCountTick = 0;

	explicit CStateClientPresentation(CGameStateId StateId) :
		m_StateId(StateId)
	{
		m_aClientsByName.fill(-1);
		m_aClientsByScore.fill(-1);
		m_aClientsByDDTeamName.fill(-1);
		m_aClientsByDDTeamScore.fill(-1);
	}
};

namespace
{
	CStateClientPresentation *FindClientPresentation(std::vector<std::unique_ptr<CStateClientPresentation>> &vpPresentations, CGameStateId StateId)
	{
		const auto It = std::find_if(vpPresentations.begin(), vpPresentations.end(), [StateId](const auto &pPresentation) { return pPresentation->m_StateId == StateId; });
		return It == vpPresentations.end() ? nullptr : It->get();
	}

	const CStateClientPresentation *FindClientPresentation(const std::vector<std::unique_ptr<CStateClientPresentation>> &vpPresentations, CGameStateId StateId)
	{
		const auto It = std::find_if(vpPresentations.begin(), vpPresentations.end(), [StateId](const auto &pPresentation) { return pPresentation->m_StateId == StateId; });
		return It == vpPresentations.end() ? nullptr : It->get();
	}
}

CSessionPresentation::CSessionPresentation(CSessionId SessionId, CMapImages &SharedMapImages) :
	m_SessionId(SessionId),
	m_MapImages(SharedMapImages)
{
}

void CSessionPresentation::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
	m_MapImages.OnInterfacesInit(pClient);
	m_MapLayersBackground.OnInterfacesInit(pClient);
	m_MapLayersForeground.OnInterfacesInit(pClient);
	m_MapLayersBackgroundForce.OnInterfacesInit(pClient);
	m_MapSounds.OnInterfacesInit(pClient);
}

void CSessionPresentation::Load(CGameSessionContext &Session)
{
	dbg_assert(Session.Id() == m_SessionId, "session presentation loaded for wrong session");
	Unload();

	CMapContext &MapContext = Session.MapContext();
	m_MapImages.Load(MapContext.Layers(), MapContext.Map(), Session.Protocol() == EGameProtocol::SIXUP);
	m_MapLayersBackground.Load(MapContext.Layers(), &m_MapImages);
	m_MapLayersForeground.Load(MapContext.Layers(), &m_MapImages);
	m_MapLayersBackgroundForce.Load(MapContext.Layers(), &m_MapImages);
	m_MapSounds.Load(MapContext.Map(), MapContext.Layers());
	m_Loaded = true;
}

void CSessionPresentation::Unload()
{
	m_vpClientPresentations.clear();
	m_aChatIgnored.fill(false);
	m_aEmoticonIgnored.fill(false);
	m_MapSounds.Unload();
	m_MapLayersBackground.Unload();
	m_MapLayersForeground.Unload();
	m_MapLayersBackgroundForce.Unload();
	m_MapImages.Unload();
	m_Loaded = false;
}

bool CSessionPresentation::GetClientSkinDescriptor(const CGameState &State, int ClientId, char *pSkinName, int SkinNameSize, CSkinDescriptor &SkinDescriptor) const
{
	const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
	const CGameState::CProtocol7ClientState &Protocol7Client = State.Protocol7Client(ClientId);
	if(Identity.m_Active && !Protocol7Client.m_Active)
	{
		if(!IntsToStr(Identity.m_ClientInfo.m_aSkin, std::size(Identity.m_ClientInfo.m_aSkin), pSkinName, SkinNameSize))
			str_copy(pSkinName, "default", SkinNameSize);
		if(!CSkin::IsValidName(pSkinName) || (!State.CoreGameInfo().m_AllowXSkins && CSkins::IsSpecialSkin(pSkinName)))
			str_copy(pSkinName, "default", SkinNameSize);
		SkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SIX;
		str_copy(SkinDescriptor.m_aSkinName, pSkinName);
	}
	else if(Protocol7Client.m_Active)
	{
		SkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SEVEN;
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; ++Part)
			str_copy(SkinDescriptor.m_Sixup.m_aaSkinPartNames[Part], Protocol7Client.m_aaSkinPartNames[Part]);
		SkinDescriptor.m_Sixup.m_XmasHat = time_season() == ETimeSeason::XMAS;
		SkinDescriptor.m_Sixup.m_BotDecoration = (Protocol7Client.m_PlayerFlags & protocol7::PLAYERFLAG_BOT) != 0;
	}
	return SkinDescriptor.m_Flags != 0;
}

void CSessionPresentation::ApplyClientColors(const CGameState &State, int ClientId, int Team, CTeeRenderInfo &RenderInfo) const
{
	const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
	const CGameState::CProtocol7ClientState &Protocol7Client = State.Protocol7Client(ClientId);
	if(Identity.m_Active && !Protocol7Client.m_Active)
	{
		const CNetObj_ClientInfo &Info = Identity.m_ClientInfo;
		RenderInfo.ApplyColors(Info.m_UseCustomColor, Info.m_ColorBody, Info.m_ColorFeet);
	}
	else if(Protocol7Client.m_Active)
	{
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; ++Part)
			GameClient()->m_Skins7.ApplyColorTo(RenderInfo.m_Sixup, Protocol7Client.m_aUseCustomColors[Part], Protocol7Client.m_aSkinPartColors[Part], Part);
	}
	RenderInfo.m_Size = 64.0f;
	RenderInfo.m_TeeRenderFlags = 0;

	const bool IsTeamPlay = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	if(!IsTeamPlay)
		return;
	RenderInfo.m_CustomColoredSkin = true;
	std::fill(std::begin(RenderInfo.m_Sixup.m_aUseCustomColors), std::end(RenderInfo.m_Sixup.m_aUseCustomColors), true);
	if(Team >= TEAM_RED && Team <= TEAM_BLUE)
	{
		const int aTeamColors[2] = {65461, 10223541};
		const ColorRGBA TeamColor = color_cast<ColorRGBA>(ColorHSLA(aTeamColors[Team]));
		RenderInfo.m_ColorBody = TeamColor;
		RenderInfo.m_ColorFeet = TeamColor;
		CTeeRenderInfo::CSixup &Sixup = RenderInfo.m_Sixup;
		const ColorRGBA aTeamColorsSixup[2] = {ColorRGBA(0.753f, 0.318f, 0.318f, 1.0f), ColorRGBA(0.318f, 0.471f, 0.753f, 1.0f)};
		const ColorRGBA aMarkingColorsSixup[2] = {ColorRGBA(0.824f, 0.345f, 0.345f, 1.0f), ColorRGBA(0.345f, 0.514f, 0.824f, 1.0f)};
		const float MarkingAlpha = Sixup.m_aColors[protocol7::SKINPART_MARKING].a;
		for(ColorRGBA &Color : Sixup.m_aColors)
			Color = aTeamColorsSixup[Team];
		if(MarkingAlpha > 0.1f)
			Sixup.m_aColors[protocol7::SKINPART_MARKING] = aMarkingColorsSixup[Team];
	}
	else
	{
		const ColorRGBA TeamColor = color_cast<ColorRGBA>(ColorHSLA(12829350));
		RenderInfo.m_ColorBody = TeamColor;
		RenderInfo.m_ColorFeet = TeamColor;
		for(ColorRGBA &Color : RenderInfo.m_Sixup.m_aColors)
			Color = TeamColor;
	}
}

std::shared_ptr<CManagedTeeRenderInfo> CSessionPresentation::CreateClientTee(const CGameState &State, int ClientId) const
{
	if(!in_range(ClientId, MAX_CLIENTS - 1) || !State.Client(ClientId).m_HasPlayerInfo)
		return nullptr;
	char aSkinName[MAX_SKIN_LENGTH] = {};
	CSkinDescriptor SkinDescriptor;
	if(!GetClientSkinDescriptor(State, ClientId, aSkinName, sizeof(aSkinName), SkinDescriptor))
		return nullptr;
	auto pTee = GameClient()->CreateManagedTeeRenderInfo(CTeeRenderInfo(), SkinDescriptor);
	ApplyClientColors(State, ClientId, State.Client(ClientId).m_PlayerInfo.m_Team, pTee->TeeRenderInfo());
	return pTee;
}

void CSessionPresentation::UpdateClients(const CPresentationContext &Context)
{
	dbg_assert(Context.m_Session.Id() == m_SessionId, "presentation context does not match session presentation");
	CStateClientPresentation *pStatePresentation = FindClientPresentation(m_vpClientPresentations, Context.m_State.Id());
	if(pStatePresentation == nullptr)
	{
		auto pNewPresentation = std::make_unique<CStateClientPresentation>(Context.m_State.Id());
		pStatePresentation = pNewPresentation.get();
		m_vpClientPresentations.push_back(std::move(pNewPresentation));
	}

	const CGameState &State = Context.m_State;
	int SpectatorCount = 0;
	if(Context.m_Session.Protocol() == EGameProtocol::SIXUP)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			const bool SessionLocal = std::any_of(Context.m_Session.GameStates().States().begin(), Context.m_Session.GameStates().States().end(), [ClientId](const auto &pState) { return pState->LocalClientId() == ClientId; });
			if(!SessionLocal && (State.Protocol7Client(ClientId).m_PlayerFlags & protocol7::PLAYERFLAG_WATCHING) != 0)
				++SpectatorCount;
		}
	}
	else if(State.HasSpectatorCount())
	{
		SpectatorCount = State.SpectatorCount().m_NumSpectators;
	}
	pStatePresentation->m_SpectatorCount = SpectatorCount;
	if(SpectatorCount == 0)
		pStatePresentation->m_LastZeroSpectatorCountTick = Context.m_Time.m_GameTick;

	const bool IsTeamPlay = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	pStatePresentation->m_aTeamSize.fill(0);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
		const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
		CClientPresentation &Client = pStatePresentation->m_aClients[ClientId];
		std::shared_ptr<CManagedTeeRenderInfo> &pSkinInfo = pStatePresentation->m_apSkinInfos[ClientId];
		if(!SnapshotClient.m_HasPlayerInfo)
		{
			Client = {};
			pSkinInfo.reset();
			continue;
		}

		Client.m_Active = true;
		Client.m_Team = SnapshotClient.m_PlayerInfo.m_Team;
		if(Client.m_Team >= TEAM_RED && Client.m_Team <= TEAM_BLUE)
			++pStatePresentation->m_aTeamSize[Client.m_Team];
		if(Identity.m_Active)
		{
			const CNetObj_ClientInfo &Info = Identity.m_ClientInfo;
			if(!IntsToStr(Info.m_aName, std::size(Info.m_aName), Client.m_aName, std::size(Client.m_aName)))
				str_copy(Client.m_aName, "nameless tee");
			IntsToStr(Info.m_aClan, std::size(Info.m_aClan), Client.m_aClan, std::size(Client.m_aClan));
			IntsToStr(Info.m_aSkin, std::size(Info.m_aSkin), Client.m_aSkinName, std::size(Client.m_aSkinName));
			if(!CSkin::IsValidName(Client.m_aSkinName) || (!State.CoreGameInfo().m_AllowXSkins && CSkins::IsSpecialSkin(Client.m_aSkinName)))
				str_copy(Client.m_aSkinName, "default");
			Client.m_Country = in_range(Info.m_Country, CountryCode::MINIMUM, CountryCode::MAXIMUM) ? Info.m_Country : CountryCode::DEFAULT;
		}
		else
		{
			Client.m_aName[0] = '\0';
			Client.m_aClan[0] = '\0';
			str_copy(Client.m_aSkinName, "default");
			Client.m_Country = CountryCode::DEFAULT;
		}

		bool SessionLocal = false;
		for(const auto &pSessionState : Context.m_Session.GameStates().States())
			SessionLocal |= pSessionState->LocalClientId() == ClientId;
		Client.m_Friend = !SessionLocal && Identity.m_Active && GameClient()->Friends()->IsFriend(Client.m_aName, Client.m_aClan, true);

		const CGameState *pInputState = nullptr;
		if(!Context.m_Time.m_IsDemoPlayback)
		{
			if(State.LocalClientId() == ClientId)
				pInputState = &State;
			else
			{
				for(const auto &pSessionState : Context.m_Session.GameStates().States())
				{
					if(pSessionState->LocalClientId() == ClientId)
					{
						pInputState = pSessionState.get();
						break;
					}
				}
			}
		}
		if(pInputState != nullptr)
		{
			const CNetObj_PlayerInput &Input = pInputState->Input().m_InputData;
			Client.m_DirectionLeft = Input.m_Direction == -1;
			Client.m_DirectionJump = Input.m_Jump == 1;
			Client.m_DirectionRight = Input.m_Direction == 1;
		}
		else
		{
			const CNetObj_Character &Character = SnapshotClient.m_Character;
			Client.m_DirectionLeft = SnapshotClient.m_HasCharacter && Character.m_Direction == -1;
			Client.m_DirectionJump = SnapshotClient.m_HasCharacter && (Character.m_Jumped & 1) != 0;
			Client.m_DirectionRight = SnapshotClient.m_HasCharacter && Character.m_Direction == 1;
		}

		CSkinDescriptor SkinDescriptor;
		if(!GetClientSkinDescriptor(State, ClientId, Client.m_aSkinName, sizeof(Client.m_aSkinName), SkinDescriptor))
		{
			Client.m_BaseRenderInfo.Reset();
			Client.m_RenderInfo.Reset();
			pSkinInfo.reset();
			continue;
		}
		if(pSkinInfo == nullptr)
		{
			pSkinInfo = GameClient()->CreateManagedTeeRenderInfo(CTeeRenderInfo(), SkinDescriptor);
		}
		else if(pSkinInfo->SkinDescriptor() != SkinDescriptor)
		{
			pSkinInfo = GameClient()->CreateManagedTeeRenderInfo(CTeeRenderInfo(), SkinDescriptor);
		}

		Client.m_RenderInfo = pSkinInfo->TeeRenderInfo();
		ApplyClientColors(State, ClientId, Client.m_Team, Client.m_RenderInfo);
		Client.m_BaseRenderInfo = Client.m_RenderInfo;

		bool Frozen = false;
		if(SessionLocal)
		{
			const CCharacterCore &Predicted = State.PredictedClient(ClientId).m_Current;
			if(Predicted.m_FreezeEnd != 0)
				Client.m_RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
			if(Predicted.m_LiveFrozen)
				Client.m_RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
			if(Predicted.m_Invincible)
				Client.m_RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_SPARKLE;
			Frozen = Predicted.m_FreezeEnd != 0;
		}
		else
		{
			const CNetObj_DDNetCharacter *pExtended = State.ExtendedCharacter(ClientId);
			if(pExtended != nullptr && pExtended->m_FreezeEnd != 0)
				Client.m_RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
			if(pExtended != nullptr && (pExtended->m_Flags & CHARACTERFLAG_MOVEMENTS_DISABLED) != 0)
				Client.m_RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
			if(pExtended != nullptr && (pExtended->m_Flags & CHARACTERFLAG_INVINCIBLE) != 0)
				Client.m_RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_SPARKLE;
			Frozen = pExtended != nullptr && pExtended->m_FreezeEnd != 0;
		}
		if((State.RenderedClient(ClientId).m_Cur.m_Weapon == WEAPON_NINJA || (Frozen && !State.CoreGameInfo().m_NoSkinChangeForFrozen)) && g_Config.m_ClShowNinja)
		{
			Client.m_RenderInfo.m_Sixup.Reset();
			Client.m_RenderInfo.ApplySkin(GameClient()->m_Players.NinjaTeeRenderInfo()->TeeRenderInfo());
			Client.m_RenderInfo.m_CustomColoredSkin = IsTeamPlay;
			if(!IsTeamPlay)
			{
				Client.m_RenderInfo.m_ColorBody = ColorRGBA(1, 1, 1);
				Client.m_RenderInfo.m_ColorFeet = ColorRGBA(1, 1, 1);
			}
		}
	}
	pStatePresentation->m_aClientsByName.fill(-1);
	int NumClients = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(State.Client(ClientId).m_HasPlayerInfo)
			pStatePresentation->m_aClientsByName[NumClients++] = ClientId;
	}
	std::stable_sort(pStatePresentation->m_aClientsByName.begin(), pStatePresentation->m_aClientsByName.begin() + NumClients, [pStatePresentation](int ClientId1, int ClientId2) {
		return str_comp_nocase(pStatePresentation->m_aClients[ClientId1].m_aName, pStatePresentation->m_aClients[ClientId2].m_aName) < 0;
	});

	pStatePresentation->m_aClientsByScore = pStatePresentation->m_aClientsByName;
	const bool ReceivedFinishTimes = State.Runtime().m_ReceivedDDNetPlayerFinishTimes;
	const bool Race7 = Context.m_Session.Protocol() == EGameProtocol::SIXUP && State.HasGameInfo() && (State.GameInfo().m_GameFlags & protocol7::GAMEFLAG_RACE) != 0;
	const auto ScoreComparator = CGameClient::GetScoreComparator(State.CoreGameInfo().m_TimeScore, ReceivedFinishTimes, Race7);
	std::stable_sort(pStatePresentation->m_aClientsByScore.begin(), pStatePresentation->m_aClientsByScore.begin() + NumClients, [&State, ScoreComparator, ReceivedFinishTimes](int ClientId1, int ClientId2) {
		const CGameState::CClientSnapshot &Client1 = State.Client(ClientId1);
		const CGameState::CClientSnapshot &Client2 = State.Client(ClientId2);
		if(ReceivedFinishTimes)
		{
			return ScoreComparator(
				Client1.m_HasDDNetPlayer ? Client1.m_DDNetPlayer.m_FinishTimeSeconds : FinishTime::UNSET,
				Client2.m_HasDDNetPlayer ? Client2.m_DDNetPlayer.m_FinishTimeSeconds : FinishTime::UNSET,
				Client1.m_HasDDNetPlayer ? Client1.m_DDNetPlayer.m_FinishTimeMillis : 0,
				Client2.m_HasDDNetPlayer ? Client2.m_DDNetPlayer.m_FinishTimeMillis : 0);
		}
		return ScoreComparator(Client1.m_PlayerInfo.m_Score, Client2.m_PlayerInfo.m_Score, 0, 0);
	});

	pStatePresentation->m_aClientsByDDTeamScore.fill(-1);
	int DDTeamIndex = 0;
	for(int Team = TEAM_FLOCK; Team < NUM_DDRACE_TEAMS; ++Team)
	{
		for(int Index = 0; Index < NumClients; ++Index)
		{
			const int ClientId = pStatePresentation->m_aClientsByScore[Index];
			if(State.Teams().Team(ClientId) == Team)
				pStatePresentation->m_aClientsByDDTeamScore[DDTeamIndex++] = ClientId;
		}
	}
	pStatePresentation->m_aClientsByDDTeamName.fill(-1);
	DDTeamIndex = 0;
	for(int Team = TEAM_FLOCK; Team < NUM_DDRACE_TEAMS; ++Team)
	{
		for(int Index = 0; Index < NumClients; ++Index)
		{
			const int ClientId = pStatePresentation->m_aClientsByName[Index];
			if(State.Teams().Team(ClientId) == Team)
				pStatePresentation->m_aClientsByDDTeamName[DDTeamIndex++] = ClientId;
		}
	}

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const bool ActiveInSession = std::any_of(Context.m_Session.GameStates().States().begin(), Context.m_Session.GameStates().States().end(), [ClientId](const auto &pState) { return pState->Client(ClientId).m_HasPlayerInfo; });
		if(!ActiveInSession)
			m_aChatIgnored[ClientId] = false;
		if(!ActiveInSession)
			m_aEmoticonIgnored[ClientId] = false;
	}
}

const CClientPresentation *CSessionPresentation::Client(CGameStateId StateId, int ClientId) const
{
	if(!in_range(ClientId, MAX_CLIENTS - 1))
		return nullptr;
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	return pPresentation == nullptr ? nullptr : &pPresentation->m_aClients[ClientId];
}

bool CSessionPresentation::GetSpectatorCount(CGameStateId StateId, int &Count, int &LastZeroTick) const
{
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	if(pPresentation == nullptr)
		return false;
	Count = pPresentation->m_SpectatorCount;
	LastZeroTick = pPresentation->m_LastZeroSpectatorCountTick;
	return true;
}

const std::array<int, MAX_CLIENTS> *CSessionPresentation::ClientsByName(CGameStateId StateId) const
{
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	return pPresentation == nullptr ? nullptr : &pPresentation->m_aClientsByName;
}

const std::array<int, MAX_CLIENTS> *CSessionPresentation::ClientsByScore(CGameStateId StateId) const
{
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	return pPresentation == nullptr ? nullptr : &pPresentation->m_aClientsByScore;
}

const std::array<int, MAX_CLIENTS> *CSessionPresentation::ClientsByDDTeamScore(CGameStateId StateId) const
{
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	return pPresentation == nullptr ? nullptr : &pPresentation->m_aClientsByDDTeamScore;
}

const std::array<int, MAX_CLIENTS> *CSessionPresentation::ClientsByDDTeamName(CGameStateId StateId) const
{
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	return pPresentation == nullptr ? nullptr : &pPresentation->m_aClientsByDDTeamName;
}

int CSessionPresentation::TeamSize(CGameStateId StateId, int Team) const
{
	if(Team < TEAM_RED || Team > TEAM_BLUE)
		return 0;
	const CStateClientPresentation *pPresentation = FindClientPresentation(m_vpClientPresentations, StateId);
	return pPresentation == nullptr ? 0 : pPresentation->m_aTeamSize[Team];
}

void CSessionPresentation::PrepareRender(const CRenderContext &Context, bool UsePredictedTime)
{
	dbg_assert(Context.m_Session.Id() == m_SessionId, "render context does not match session presentation");
	dbg_assert(m_Loaded, "session presentation must be loaded before rendering");
	m_MapImages.SetGameInfo(Context.m_State.CoreGameInfo());
	m_MapLayersBackground.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
	m_MapLayersForeground.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
	m_MapLayersBackgroundForce.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
}

void CSessionPresentation::UpdateMapSounds(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool UsePredictedTime)
{
	dbg_assert(m_Loaded, "session presentation must be loaded before updating map sounds");
	m_MapLayersBackground.EnvEvaluator().SetOnlineTime(State, Time, UsePredictedTime);
	m_MapSounds.Update(State, Time, ListenerPosition, Time.m_IsDemoPlaybackPaused, m_MapLayersBackground.EnvEvaluator());
}

CSessionPresentationManager::CSessionPresentationManager(CMapImages &SharedMapImages) :
	m_SharedMapImages(SharedMapImages)
{
}

void CSessionPresentationManager::OnInterfacesInit(CGameClient *pClient)
{
	m_pGameClient = pClient;
	for(auto &pPresentation : m_vpPresentations)
		pPresentation->OnInterfacesInit(pClient);
}

CSessionPresentation *CSessionPresentationManager::Create(CSessionId SessionId)
{
	if(!SessionId.IsValid() || Find(SessionId) != nullptr)
		return nullptr;

	auto pPresentation = std::make_unique<CSessionPresentation>(SessionId, m_SharedMapImages);
	if(m_pGameClient != nullptr)
		pPresentation->OnInterfacesInit(m_pGameClient);
	m_vpPresentations.push_back(std::move(pPresentation));
	return m_vpPresentations.back().get();
}

CSessionPresentation *CSessionPresentationManager::Find(CSessionId SessionId)
{
	const auto It = std::find_if(m_vpPresentations.begin(), m_vpPresentations.end(), [SessionId](const auto &pPresentation) { return pPresentation->SessionId() == SessionId; });
	return It == m_vpPresentations.end() ? nullptr : It->get();
}

const CSessionPresentation *CSessionPresentationManager::Find(CSessionId SessionId) const
{
	const auto It = std::find_if(m_vpPresentations.begin(), m_vpPresentations.end(), [SessionId](const auto &pPresentation) { return pPresentation->SessionId() == SessionId; });
	return It == m_vpPresentations.end() ? nullptr : It->get();
}

void CSessionPresentationManager::SetAudible(CSessionId SessionId)
{
	if(m_AudibleSessionId == SessionId)
		return;
	CSessionPresentation *pPrevious = Find(m_AudibleSessionId);
	if(pPrevious != nullptr)
		pPrevious->MapSounds().SetAudible(false);
	m_AudibleSessionId = SessionId;
	CSessionPresentation *pCurrent = Find(m_AudibleSessionId);
	if(pCurrent != nullptr)
		pCurrent->MapSounds().SetAudible(true);
}

void CSessionPresentationManager::Unload(CSessionId SessionId)
{
	CSessionPresentation *pPresentation = Find(SessionId);
	if(pPresentation == nullptr)
		return;
	if(m_AudibleSessionId == SessionId)
		m_AudibleSessionId = CSessionId();
	pPresentation->Unload();
}

void CSessionPresentationManager::UnloadAll()
{
	m_AudibleSessionId = CSessionId();
	for(auto &pPresentation : m_vpPresentations)
		pPresentation->Unload();
}
