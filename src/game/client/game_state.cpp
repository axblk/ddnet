#include "game_state.h"

#include "map_context.h"

#include <engine/client.h>

#include <generated/protocol.h>

#include <game/client/prediction/entities/character.h>

#include <algorithm>
#include <utility>

namespace
{
	void DigestBytes(uint64_t &Digest, const void *pData, size_t Size)
	{
		const auto *pBytes = static_cast<const unsigned char *>(pData);
		for(size_t i = 0; i < Size; i++)
		{
			Digest ^= pBytes[i];
			Digest *= 1099511628211ULL;
		}
	}

	template<typename T>
	void DigestValue(uint64_t &Digest, const T &Value)
	{
		DigestBytes(Digest, &Value, sizeof(Value));
	}
}

CGameState::CGameState(CGameStateId Id, CStreamId StreamId) :
	m_Id(Id),
	m_StreamId(StreamId)
{
	Reset();
}

void CGameState::Reset()
{
	m_LocalClientId = -1;
	m_SnapshotTick = 0;
	m_SnapshotItems = 0;
	m_PredictionTick = 0;
	m_aTuning.fill(CTuningParams::DEFAULT);
	m_aClients = {};
	m_aPredictedClients = {};
	m_vEntities.clear();
	m_HasGameInfo = false;
	m_GameInfo = {};
	m_HasSpectatorInfo = false;
	m_SpectatorInfo = {};
	m_Teams.Reset();
	m_GameWorld.Clear();
	m_GameWorld.m_WorldConfig = {};
	m_GameWorld.m_WorldConfig.m_InfiniteAmmo = true;
	m_PredictedWorld.CopyWorld(&m_GameWorld);
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
}

void CGameState::InitPrediction(CMapContext &MapContext)
{
	std::copy(MapContext.TuningList(), MapContext.TuningList() + TuneZone::NUM, m_aTuning.begin());
	m_GameWorld.Init(MapContext.Collision(), m_aTuning.data(), MapContext.MapBugs(), &MapContext.GameConfig());
	m_GameWorld.m_Core.InitSwitchers(MapContext.Collision()->m_HighestSwitchNumber);
	m_PredictionInitialized = true;
	RebuildGameWorld();
}

void CGameState::ApplySnapshot(const IClient &Client, int Conn)
{
	const int NumItems = Client.SnapNumItems(Conn, IClient::SNAP_CURRENT);
	std::array<CClientSnapshot, MAX_CLIENTS> aClients = {};
	std::vector<CEntitySnapshot> vEntities;
	CNetObj_GameInfo GameInfo = {};
	bool HasGameInfo = false;
	CNetObj_SpectatorInfo SpectatorInfo = {};
	bool HasSpectatorInfo = false;
	std::vector<CEntitySnapshot> vEntityEx;
	for(int i = 0; i < NumItems; i++)
	{
		const IClient::CSnapItem Item = Client.SnapGetItem(Conn, IClient::SNAP_CURRENT, i);
		if(Item.m_Type == NETOBJTYPE_GAMEINFO)
		{
			HasGameInfo = true;
			GameInfo = *static_cast<const CNetObj_GameInfo *>(Item.m_pData);
		}
		else if(Item.m_Type == NETOBJTYPE_SPECTATORINFO)
		{
			HasSpectatorInfo = true;
			SpectatorInfo = *static_cast<const CNetObj_SpectatorInfo *>(Item.m_pData);
		}
		else if(Item.m_Type == NETOBJTYPE_ENTITYEX)
		{
			CEntitySnapshot Entity;
			Entity.m_Id = Item.m_Id;
			Entity.m_Type = Item.m_Type;
			Entity.m_HasEntityEx = true;
			Entity.m_EntityEx = *static_cast<const CNetObj_EntityEx *>(Item.m_pData);
			vEntityEx.push_back(std::move(Entity));
		}
		else if(Item.m_Type == NETOBJTYPE_PICKUP || Item.m_Type == NETOBJTYPE_DDNETPICKUP || Item.m_Type == NETOBJTYPE_LASER || Item.m_Type == NETOBJTYPE_DDNETLASER || Item.m_Type == NETOBJTYPE_PROJECTILE || Item.m_Type == NETOBJTYPE_DDRACEPROJECTILE || Item.m_Type == NETOBJTYPE_DDNETPROJECTILE)
		{
			CEntitySnapshot Entity;
			Entity.m_Id = Item.m_Id;
			Entity.m_Type = Item.m_Type;
			const auto *pData = static_cast<const unsigned char *>(Item.m_pData);
			Entity.m_vData.assign(pData, pData + Item.m_DataSize);
			vEntities.push_back(std::move(Entity));
		}
		else if(Item.m_Id >= 0 && Item.m_Id < MAX_CLIENTS)
		{
			CClientSnapshot &SnapshotClient = aClients[Item.m_Id];
			switch(Item.m_Type)
			{
			case NETOBJTYPE_PLAYERINFO:
				SnapshotClient.m_Active = true;
				SnapshotClient.m_HasPlayerInfo = true;
				SnapshotClient.m_PlayerInfo = *static_cast<const CNetObj_PlayerInfo *>(Item.m_pData);
				break;
			case NETOBJTYPE_CLIENTINFO:
				SnapshotClient.m_Active = true;
				SnapshotClient.m_HasClientInfo = true;
				SnapshotClient.m_ClientInfo = *static_cast<const CNetObj_ClientInfo *>(Item.m_pData);
				break;
			case NETOBJTYPE_CHARACTER:
				SnapshotClient.m_Active = true;
				SnapshotClient.m_HasCharacter = true;
				SnapshotClient.m_Character = *static_cast<const CNetObj_Character *>(Item.m_pData);
				if(const auto *pPrev = static_cast<const CNetObj_Character *>(Client.SnapFindItem(Conn, IClient::SNAP_PREV, NETOBJTYPE_CHARACTER, Item.m_Id)))
				{
					SnapshotClient.m_HasPrevCharacter = true;
					SnapshotClient.m_PrevCharacter = *pPrev;
				}
				break;
			case NETOBJTYPE_DDNETCHARACTER:
				SnapshotClient.m_HasExtendedCharacter = true;
				SnapshotClient.m_ExtendedCharacter = *static_cast<const CNetObj_DDNetCharacter *>(Item.m_pData);
				break;
			}
		}
	}
	for(CEntitySnapshot &Entity : vEntities)
	{
		const auto Found = std::find_if(vEntityEx.begin(), vEntityEx.end(), [&Entity](const CEntitySnapshot &EntityEx) { return EntityEx.m_Id == Entity.m_Id; });
		if(Found != vEntityEx.end())
		{
			Entity.m_HasEntityEx = true;
			Entity.m_EntityEx = Found->m_EntityEx;
		}
	}
	ApplySnapshotData(Client.GameTick(Conn), NumItems, std::move(aClients), HasGameInfo ? &GameInfo : nullptr, std::move(vEntities));
	if(HasSpectatorInfo)
		ApplySpectatorInfo(SpectatorInfo);
}

void CGameState::ApplySnapshotData(int Tick, int NumItems, std::array<CClientSnapshot, MAX_CLIENTS> aClients, const CNetObj_GameInfo *pGameInfo, std::vector<CEntitySnapshot> vEntities)
{
	m_aClients = std::move(aClients);
	m_vEntities = std::move(vEntities);
	m_HasGameInfo = pGameInfo != nullptr;
	m_GameInfo = pGameInfo ? *pGameInfo : CNetObj_GameInfo{};
	m_HasSpectatorInfo = false;
	m_SpectatorInfo = {};
	int LocalClientId = -1;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		if(m_aClients[ClientId].m_HasPlayerInfo && m_aClients[ClientId].m_PlayerInfo.m_Local)
			LocalClientId = ClientId;
	ApplySnapshotMetadata(Tick, NumItems, LocalClientId);
	RebuildGameWorld();
}

void CGameState::ApplySnapshotMetadata(int Tick, int NumItems, int LocalClientId)
{
	m_SnapshotTick = Tick;
	m_SnapshotItems = NumItems;
	m_LocalClientId = LocalClientId;
}

void CGameState::ApplyTuning(const CTuningParams &Tuning, int TuneZone)
{
	if(TuneZone >= 0 && TuneZone < TuneZone::NUM)
		m_aTuning[TuneZone] = Tuning;
}

void CGameState::SetTeam(int ClientId, int Team)
{
	if(ClientId >= 0 && ClientId < MAX_CLIENTS)
		m_Teams.Team(ClientId, Team);
}

void CGameState::RebuildGameWorld()
{
	if(!m_PredictionInitialized)
		return;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aClients[ClientId].m_HasExtendedCharacter)
			m_Teams.SetSolo(ClientId, (m_aClients[ClientId].m_ExtendedCharacter.m_Flags & CHARACTERFLAG_SOLO) != 0);
	}

	m_GameWorld.m_GameTick = m_SnapshotTick;
	m_GameWorld.NetObjBegin(m_Teams, m_LocalClientId);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CClientSnapshot &SnapshotClient = m_aClients[ClientId];
		if(!SnapshotClient.m_HasCharacter)
			continue;
		const int GameTeam = SnapshotClient.m_HasPlayerInfo ? SnapshotClient.m_PlayerInfo.m_Team : ClientId;
		m_GameWorld.NetCharAdd(ClientId, &SnapshotClient.m_Character,
			SnapshotClient.m_HasExtendedCharacter ? &SnapshotClient.m_ExtendedCharacter : nullptr,
			GameTeam, ClientId == m_LocalClientId);
	}
	for(const CEntitySnapshot &Entity : m_vEntities)
		m_GameWorld.NetObjAdd(Entity.m_Id, Entity.m_Type, Entity.m_vData.data(), Entity.m_HasEntityEx ? &Entity.m_EntityEx : nullptr);
	m_GameWorld.NetObjEnd();
}

void CGameState::Predict(const IClient &Client, int Conn)
{
	PredictTo(Client.PredGameTick(Conn), [&Client, Conn](int Tick) {
		return reinterpret_cast<const CNetObj_PlayerInput *>(Client.GetInput(Conn, Tick));
	});
}

void CGameState::PredictTo(int TargetTick, const std::function<const CNetObj_PlayerInput *(int)> &InputAt)
{
	m_PredictionTick = TargetTick;
	m_aPredictedClients = {};
	if(!m_PredictionInitialized || m_LocalClientId < 0 || m_LocalClientId >= MAX_CLIENTS || !m_aClients[m_LocalClientId].m_HasCharacter)
		return;
	if(m_HasGameInfo && (m_GameInfo.m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return;

	m_PredictedWorld.CopyWorld(&m_GameWorld);
	CCharacter *pLocalCharacter = m_PredictedWorld.GetCharacterById(m_LocalClientId);
	if(!pLocalCharacter)
		return;
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);

	for(int Tick = m_SnapshotTick + 1; Tick <= TargetTick; Tick++)
	{
		const CNetObj_PlayerInput *pInput = InputAt(Tick);
		if(pInput)
			pLocalCharacter->OnDirectInput(pInput);
		m_PredictedWorld.m_GameTick = Tick;
		if(pInput)
			pLocalCharacter->OnPredictedInput(pInput);
		if(Tick == TargetTick)
			m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
		m_PredictedWorld.Tick();
	}

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(CCharacter *pCharacter = m_PrevPredictedWorld.GetCharacterById(ClientId))
		{
			m_aPredictedClients[ClientId].m_HasPrev = true;
			m_aPredictedClients[ClientId].m_Prev = pCharacter->GetCore();
		}
		if(CCharacter *pCharacter = m_PredictedWorld.GetCharacterById(ClientId))
		{
			m_aPredictedClients[ClientId].m_HasCurrent = true;
			m_aPredictedClients[ClientId].m_Current = pCharacter->GetCore();
		}
	}
}

bool CGameState::HasGameWorldCharacter(int ClientId) const
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS && m_GameWorld.GetCharacterById(ClientId) != nullptr;
}

CCharacterCore CGameState::GameWorldCharacterCore(int ClientId) const
{
	const CCharacter *pCharacter = ClientId >= 0 && ClientId < MAX_CLIENTS ? m_GameWorld.GetCharacterById(ClientId) : nullptr;
	return pCharacter ? pCharacter->GetCore() : CCharacterCore{};
}

uint64_t CGameState::SnapshotDigest() const
{
	uint64_t Digest = 14695981039346656037ULL;
	DigestValue(Digest, m_SnapshotTick);
	DigestValue(Digest, m_SnapshotItems);
	DigestValue(Digest, m_LocalClientId);
	DigestValue(Digest, m_HasGameInfo);
	if(m_HasGameInfo)
		DigestValue(Digest, m_GameInfo);
	DigestValue(Digest, m_HasSpectatorInfo);
	if(m_HasSpectatorInfo)
		DigestValue(Digest, m_SpectatorInfo);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CClientSnapshot &SnapshotClient = m_aClients[ClientId];
		DigestValue(Digest, SnapshotClient.m_Active);
		DigestValue(Digest, SnapshotClient.m_HasPlayerInfo);
		DigestValue(Digest, SnapshotClient.m_HasClientInfo);
		DigestValue(Digest, SnapshotClient.m_HasCharacter);
		DigestValue(Digest, SnapshotClient.m_HasPrevCharacter);
		DigestValue(Digest, SnapshotClient.m_HasExtendedCharacter);
		if(SnapshotClient.m_HasPlayerInfo)
			DigestValue(Digest, SnapshotClient.m_PlayerInfo);
		if(SnapshotClient.m_HasClientInfo)
			DigestValue(Digest, SnapshotClient.m_ClientInfo);
		if(SnapshotClient.m_HasCharacter)
			DigestValue(Digest, SnapshotClient.m_Character);
		if(SnapshotClient.m_HasPrevCharacter)
			DigestValue(Digest, SnapshotClient.m_PrevCharacter);
		if(SnapshotClient.m_HasExtendedCharacter)
			DigestValue(Digest, SnapshotClient.m_ExtendedCharacter);
		DigestValue(Digest, m_Teams.Team(ClientId));
		DigestValue(Digest, m_Teams.GetSolo(ClientId));
	}
	for(const CEntitySnapshot &Entity : m_vEntities)
	{
		DigestValue(Digest, Entity.m_Id);
		DigestValue(Digest, Entity.m_Type);
		DigestBytes(Digest, Entity.m_vData.data(), Entity.m_vData.size());
		DigestValue(Digest, Entity.m_HasEntityEx);
		if(Entity.m_HasEntityEx)
			DigestValue(Digest, Entity.m_EntityEx);
	}
	return Digest;
}

uint64_t CGameState::PredictionDigest() const
{
	uint64_t Digest = 14695981039346656037ULL;
	DigestValue(Digest, m_PredictionTick);
	for(const CPredictedClient &PredictedClient : m_aPredictedClients)
	{
		DigestValue(Digest, PredictedClient.m_HasPrev);
		DigestValue(Digest, PredictedClient.m_HasCurrent);
		if(PredictedClient.m_HasPrev)
		{
			CNetObj_CharacterCore Core = {};
			PredictedClient.m_Prev.Write(&Core);
			DigestValue(Digest, Core);
			DigestValue(Digest, PredictedClient.m_Prev.m_ActiveWeapon);
		}
		if(PredictedClient.m_HasCurrent)
		{
			CNetObj_CharacterCore Core = {};
			PredictedClient.m_Current.Write(&Core);
			DigestValue(Digest, Core);
			DigestValue(Digest, PredictedClient.m_Current.m_ActiveWeapon);
		}
	}
	return Digest;
}

CGameStateId CGameStateManager::Create(CStreamId StreamId)
{
	if(!StreamId.IsValid() || FindByStream(StreamId))
		return {};
	const CGameStateId Id(m_NextId++);
	m_vpStates.push_back(std::make_unique<CGameState>(Id, StreamId));
	return Id;
}

CGameState *CGameStateManager::Find(CGameStateId Id)
{
	const auto Found = std::find_if(m_vpStates.begin(), m_vpStates.end(), [Id](const auto &pState) { return pState->Id() == Id; });
	return Found == m_vpStates.end() ? nullptr : Found->get();
}

const CGameState *CGameStateManager::Find(CGameStateId Id) const
{
	const auto Found = std::find_if(m_vpStates.begin(), m_vpStates.end(), [Id](const auto &pState) { return pState->Id() == Id; });
	return Found == m_vpStates.end() ? nullptr : Found->get();
}

CGameState *CGameStateManager::FindByStream(CStreamId StreamId)
{
	const auto Found = std::find_if(m_vpStates.begin(), m_vpStates.end(), [StreamId](const auto &pState) { return pState->StreamId() == StreamId; });
	return Found == m_vpStates.end() ? nullptr : Found->get();
}

const CGameState *CGameStateManager::FindByStream(CStreamId StreamId) const
{
	const auto Found = std::find_if(m_vpStates.begin(), m_vpStates.end(), [StreamId](const auto &pState) { return pState->StreamId() == StreamId; });
	return Found == m_vpStates.end() ? nullptr : Found->get();
}

bool CGameStateManager::Destroy(CGameStateId Id)
{
	const auto Found = std::find_if(m_vpStates.begin(), m_vpStates.end(), [Id](const auto &pState) { return pState->Id() == Id; });
	if(Found == m_vpStates.end())
		return false;
	m_vpStates.erase(Found);
	return true;
}
