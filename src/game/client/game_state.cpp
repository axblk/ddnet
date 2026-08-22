#include "game_state.h"

#include "map_context.h"

#include <base/str.h>

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

void CGameState::CParticleSystemState::Reset()
{
	m_NumParticles = 0;
	m_FrictionFraction = 0.0f;
	m_LastRenderTime = 0;
	m_TimeInitialized = false;
	m_aFirstPart.fill(-1);
	if(m_vParticles.empty())
	{
		m_FirstFree = -1;
		return;
	}

	for(int i = 0; i < MAX_PARTICLES; i++)
	{
		m_vParticles[i].m_PrevPart = i - 1;
		m_vParticles[i].m_NextPart = i + 1;
	}
	m_vParticles[0].m_PrevPart = 0;
	m_vParticles[MAX_PARTICLES - 1].m_NextPart = -1;
	m_FirstFree = 0;
}

bool CGameState::CParticleSystemState::Add(int Group, const CParticle &Particle, float TimePassed)
{
	if(Group < 0 || Group >= NUM_GROUPS)
		return false;
	if(m_vParticles.empty())
	{
		m_vParticles.resize(MAX_PARTICLES);
		Reset();
	}
	if(m_FirstFree == -1)
		return false;

	const int Id = m_FirstFree;
	m_FirstFree = m_vParticles[Id].m_NextPart;
	if(m_FirstFree != -1)
		m_vParticles[m_FirstFree].m_PrevPart = -1;

	m_vParticles[Id] = Particle;
	m_vParticles[Id].m_PrevPart = -1;
	m_vParticles[Id].m_NextPart = m_aFirstPart[Group];
	if(m_aFirstPart[Group] != -1)
		m_vParticles[m_aFirstPart[Group]].m_PrevPart = Id;
	m_aFirstPart[Group] = Id;
	m_vParticles[Id].m_Life = TimePassed;
	m_NumParticles++;
	return true;
}

void CGameState::CEffectClockState::Reset()
{
	m_Add5hz = false;
	m_LastUpdate5hz = 0;
	m_Add50hz = false;
	m_LastUpdate50hz = 0;
	m_Add100hz = false;
	m_LastUpdate100hz = 0;
	m_SkidSoundTimer = 0;
}

void CGameState::CEffectClockState::Update(int64_t Now, int64_t TimeFrequency, float Speed)
{
	auto UpdateClock = [Now, TimeFrequency, Speed](bool &Add, int64_t &LastUpdate, int Frequency) {
		Add = (Now - LastUpdate) / static_cast<float>(TimeFrequency) * Speed > 1.0f / Frequency;
		if(Add)
			LastUpdate = Now;
	};
	UpdateClock(m_Add5hz, m_LastUpdate5hz, 5);
	UpdateClock(m_Add50hz, m_LastUpdate50hz, 50);
	UpdateClock(m_Add100hz, m_LastUpdate100hz, 100);
}

bool CGameState::CEffectClockState::TrySkidSound(int64_t Now, int64_t TimeFrequency)
{
	if(Now - m_SkidSoundTimer <= TimeFrequency / 10)
		return false;
	m_SkidSoundTimer = Now;
	return true;
}

void CGameState::CSceneClockState::Reset()
{
	m_AnimationTime = 0.0f;
	m_GameTickTime = 0.0f;
	m_PredIntraTick = 0.0f;
	m_LastUpdateTime = 0;
	m_Initialized = false;
}

void CGameState::CSceneClockState::Update(int64_t Now, int64_t TimeFrequency, float Speed, float GameTickTime, float PredIntraTick)
{
	if(!m_Initialized)
	{
		m_LastUpdateTime = Now;
		m_GameTickTime = GameTickTime;
		m_PredIntraTick = PredIntraTick;
		m_Initialized = true;
		return;
	}

	if(Now >= m_LastUpdateTime)
		m_AnimationTime += (Now - m_LastUpdateTime) / static_cast<float>(TimeFrequency) * Speed;
	m_LastUpdateTime = Now;
	if(Speed != 0.0f)
	{
		m_GameTickTime = GameTickTime;
		m_PredIntraTick = PredIntraTick;
	}
}

void CGameState::CDamageIndicatorState::Reset()
{
	m_NumItems = 0;
	m_LastUpdateTime = 0;
	m_TimeInitialized = false;
}

void CGameState::CDamageIndicatorState::Create(vec2 Pos, vec2 Dir, int OwnerClientId, float Alpha, float StartAngle)
{
	if(m_NumItems >= MAX_ITEMS)
		return;

	CItem &Item = m_aItems[m_NumItems++];
	Item.m_Pos = Pos;
	Item.m_Dir = -Dir;
	Item.m_RemainingLife = 0.75f;
	Item.m_StartAngle = StartAngle;
	Item.m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, Alpha);
	Item.m_OwnerClientId = OwnerClientId;
}

void CGameState::CDamageIndicatorState::Update(float DeltaTime)
{
	for(int i = 0; i < m_NumItems;)
	{
		m_aItems[i].m_RemainingLife -= DeltaTime;
		if(m_aItems[i].m_RemainingLife < 0.0f)
			m_aItems[i] = m_aItems[--m_NumItems];
		else
			i++;
	}
}

void CGameState::CDamageIndicatorState::Advance(int64_t Now, int64_t TimeFrequency, float Speed)
{
	if(!m_TimeInitialized)
	{
		m_LastUpdateTime = Now;
		m_TimeInitialized = true;
		return;
	}
	if(Now >= m_LastUpdateTime && TimeFrequency > 0)
		Update((Now - m_LastUpdateTime) / static_cast<float>(TimeFrequency) * Speed);
	m_LastUpdateTime = Now;
}

void CGameState::CInputState::Reset()
{
	m_MousePos = vec2(0.0f, 0.0f);
	m_MousePosOnAction = vec2(0.0f, 0.0f);
	m_TargetPos = vec2(0.0f, 0.0f);
	m_MouseInputType = EMouseInputType::ABSOLUTE;
	m_InputData = {};
	m_LastData = {};
	m_InputDirectionLeft = 0;
	m_InputDirectionRight = 0;
	m_ShowHookColl = 0;
	m_aAmmoCount.fill(0);
	m_LastSendTime = 0;
}

void CGameState::CInputState::ReleaseGameplay()
{
	m_LastData.m_Direction = 0;
	if((m_LastData.m_Fire & 1) != 0)
		m_LastData.m_Fire++;
	m_LastData.m_Fire &= INPUT_STATE_MASK;
	m_LastData.m_Jump = 0;
	m_LastData.m_Hook = 0;
	m_InputData = m_LastData;
	m_InputDirectionLeft = 0;
	m_InputDirectionRight = 0;
}

bool CGameState::CInputState::ApplyStrokedCommand(const char *pCommand, int Stroke, bool BlockGameplayPress)
{
	struct CCommand
	{
		const char *m_pName;
		int *m_pValue;
		bool m_Counter;
		int m_PressValue;
		bool m_Blockable;
	};
	const CCommand aCommands[] = {
		{"+left", &m_InputDirectionLeft, false, 1, true},
		{"+right", &m_InputDirectionRight, false, 1, true},
		{"+jump", &m_InputData.m_Jump, false, 1, true},
		{"+hook", &m_InputData.m_Hook, false, 1, true},
		{"+fire", &m_InputData.m_Fire, true, 1, true},
		{"+showhookcoll", &m_ShowHookColl, false, 1, true},
		{"+weapon1", &m_InputData.m_WantedWeapon, false, 1, false},
		{"+weapon2", &m_InputData.m_WantedWeapon, false, 2, false},
		{"+weapon3", &m_InputData.m_WantedWeapon, false, 3, false},
		{"+weapon4", &m_InputData.m_WantedWeapon, false, 4, false},
		{"+weapon5", &m_InputData.m_WantedWeapon, false, 5, false},
		{"+nextweapon", &m_InputData.m_NextWeapon, true, 1, true},
		{"+prevweapon", &m_InputData.m_PrevWeapon, true, 1, true},
	};
	const auto *const It = std::find_if(std::begin(aCommands), std::end(aCommands), [pCommand](const CCommand &Command) { return str_comp(Command.m_pName, pCommand) == 0; });
	if(It == std::end(aCommands))
		return false;
	if(BlockGameplayPress && Stroke && It->m_Blockable)
	{
		if(It->m_pValue == &m_InputData.m_NextWeapon || It->m_pValue == &m_InputData.m_PrevWeapon)
			m_InputData.m_WantedWeapon = 0;
		return true;
	}
	int &Value = *It->m_pValue;
	if(It->m_Counter)
	{
		if((Value & 1) != Stroke)
			Value++;
		Value &= INPUT_STATE_MASK;
		if(It->m_pValue == &m_InputData.m_NextWeapon || It->m_pValue == &m_InputData.m_PrevWeapon)
			m_InputData.m_WantedWeapon = 0;
	}
	else if(Stroke)
		Value = It->m_PressValue;
	else if(It->m_pValue != &m_InputData.m_WantedWeapon)
		Value = 0;
	return true;
}

void CGameState::CRaceMessageState::Reset()
{
	m_CheckpointDiff = 0.0f;
	m_FinishDiff = 0.0f;
	m_DDRaceTime = 0;
	m_FinishReceivedTick = 0;
	m_CheckpointReceivedTick = 0;
	m_ShowFinish = false;
}

void CGameState::CRaceMessageState::ApplyDDRaceTime(int Time, int Check, bool Finish, int Tick)
{
	m_DDRaceTime = Time;
	m_ShowFinish = Finish;
	if(Finish)
	{
		m_FinishDiff = Check / 100.0f;
		m_FinishReceivedTick = Tick;
	}
	else
	{
		m_CheckpointDiff = Check / 100.0f;
		m_CheckpointReceivedTick = Tick;
	}
}

void CGameState::CRaceMessageState::ApplyLegacyRecord(int Time, int Check, int Tick)
{
	m_DDRaceTime = Time;
	m_FinishReceivedTick = Tick;
	if(Check != 0)
	{
		m_CheckpointDiff = Check / 100.0f;
		m_CheckpointReceivedTick = Tick;
	}
}

void CGameState::CRuntimeState::Reset()
{
	m_ServerMode = SERVERMODE_PURE;
	m_LastNewPredictedTick = -1;
	m_CheckInfo = -1;
	m_LocalTuneZone = -1;
	m_ReceivedTuning = false;
	m_ExpectingTuningForZone = -1;
	m_ExpectingTuningSince = 0;
	m_CurrentTuning = CTuningParams::DEFAULT;
	m_NextChangeInfo = -1;
	m_DDRaceMsgSent = false;
	m_ShowOthers = -1;
	m_EnableSpectatorCount = -1;
	m_SwitchStateTeam = -1;
	m_PlayerRecord = -1.0f;
	m_LegacyPredictedTick = -1;
	m_LastRoundStartTick = -1;
	m_LastRaceTick = -1;
	m_aStrongHookLastUpdateTick.fill(0);
	m_CharOrder.Reset();
	m_aFlagDropTick.fill(0);
	m_LastFlagCarrierRed = -4;
	m_LastFlagCarrierBlue = -4;
	m_aLastPredictedPosition.fill(vec2(0.0f, 0.0f));
	m_aLastPredictedActive.fill(false);
	m_GameOver = false;
	m_GamePaused = false;
	m_ReceivedDDNetPlayer = false;
	m_ReceivedDDNetPlayerFinishTimes = false;
	m_ReceivedDDNetPlayerFinishTimesMillis = false;
	m_Input.Reset();
}

void CGameState::CProtocol7ClientState::Reset()
{
	m_Active = false;
	for(auto &aSkinPartName : m_aaSkinPartNames)
		aSkinPartName[0] = '\0';
	std::fill(std::begin(m_aUseCustomColors), std::end(m_aUseCustomColors), 0);
	std::fill(std::begin(m_aSkinPartColors), std::end(m_aSkinPartColors), 0);
	m_PlayerFlags = 0;
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
	m_vClientIdentities.assign(MAX_CLIENTS, {});
	m_vClientEmoticons.assign(MAX_CLIENTS, {});
	for(CClientEmoticonState &Emoticon : m_vClientEmoticons)
		Emoticon.Reset();
	m_vRenderedClients.assign(MAX_CLIENTS, {});
	m_vClientPredictionHistory.assign(MAX_CLIENTS, {});
	m_aPredictedClients = {};
	m_vSnappedCharacters.assign(MAX_CLIENTS, {});
	m_vEvolvedCharacters.assign(MAX_CLIENTS, {});
	for(CNetObj_Character &Character : m_vEvolvedCharacters)
		Character.m_Tick = -1;
	for(CProtocol7ClientState &Client : m_aProtocol7Clients)
		Client.Reset();
	m_vEntities.clear();
	m_HasGameInfo = false;
	m_GameInfo = {};
	m_HasSpectatorInfo = false;
	m_SpectatorInfo = {};
	m_HasSpectatorCount = false;
	m_SpectatorCount = {};
	m_CoreGameInfo = {};
	m_Teams.Reset();
	m_FullyPredicted = false;
	m_GameWorld.Clear();
	m_GameWorld.m_WorldConfig = {};
	m_GameWorld.m_WorldConfig.m_InfiniteAmmo = true;
	m_PredictedWorld.CopyWorld(&m_GameWorld);
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
	m_Runtime.Reset();
	m_EffectClock.Reset();
	m_SceneClock.Reset();
	m_Particles.Reset();
	m_DamageIndicators.Reset();
	m_RaceMessages.Reset();
}

void CGameState::InitPrediction(CMapContext &MapContext)
{
	std::copy(MapContext.TuningList(), MapContext.TuningList() + TuneZone::NUM, m_aTuning.begin());
	m_GameWorld.Init(MapContext.Collision(), m_aTuning.data(), MapContext.MapBugs(), &MapContext.GameConfig());
	m_GameWorld.m_Core.InitSwitchers(MapContext.Collision()->m_HighestSwitchNumber);
	m_GameWorld.UpdatePhysicsRules();
	m_PredictionInitialized = true;
	RebuildGameWorld();
}

void CGameState::EvolveCharacter(CNetObj_Character &Character, int Tick)
{
	CWorldCore TempWorld;
	CCharacterCore TempCore{};
	CTeamsCore TempTeams{};
	TempCore.Init(&TempWorld, m_GameWorld.Collision(), &TempTeams);
	TempCore.Read(&Character);
	TempCore.m_ActiveWeapon = Character.m_Weapon;
	while(Character.m_Tick < Tick)
	{
		Character.m_Tick++;
		TempCore.Tick(false);
		TempCore.Move();
		TempCore.Quantize();
	}
	TempCore.Write(&Character);
}

void CGameState::ApplySnapshot(const IClient &Client, CSessionId SessionId, CStreamId StreamId)
{
	const int NumItems = Client.SnapNumItems(SessionId, StreamId, IClient::SNAP_CURRENT);
	std::array<CClientSnapshot, MAX_CLIENTS> aClients = {};
	CNetObj_GameInfo GameInfo = {};
	bool HasGameInfo = false;
	CNetObj_SpectatorInfo SpectatorInfo = {};
	bool HasSpectatorInfo = false;
	CNetObj_SpectatorCount SpectatorCount = {};
	bool HasSpectatorCount = false;
	// The entities are written into buffers that outlive the snapshot, so the byte
	// vectors of every entity keep the capacity they were given for the previous
	// one instead of being allocated and freed again for each snapshot.
	const auto EntitySlot = [](std::vector<CEntitySnapshot> &vEntities, size_t Index) -> CEntitySnapshot & {
		if(Index == vEntities.size())
			vEntities.emplace_back();
		return vEntities[Index];
	};
	size_t NumEntities = 0;
	size_t NumEntityEx = 0;
	for(int i = 0; i < NumItems; i++)
	{
		const IClient::CSnapItem Item = Client.SnapGetItem(SessionId, StreamId, IClient::SNAP_CURRENT, i);
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
		else if(Item.m_Type == NETOBJTYPE_SPECTATORCOUNT)
		{
			HasSpectatorCount = true;
			SpectatorCount = *static_cast<const CNetObj_SpectatorCount *>(Item.m_pData);
		}
		else if(Item.m_Type == NETOBJTYPE_ENTITYEX)
		{
			CEntitySnapshot &EntityEx = EntitySlot(m_vEntityEx, NumEntityEx++);
			EntityEx.m_Id = Item.m_Id;
			EntityEx.m_Type = Item.m_Type;
			EntityEx.m_HasEntityEx = true;
			EntityEx.m_EntityEx = *static_cast<const CNetObj_EntityEx *>(Item.m_pData);
		}
		else if(Item.m_Type == NETOBJTYPE_GAMEDATA || Item.m_Type == NETOBJTYPE_FLAG || Item.m_Type == NETOBJTYPE_PICKUP || Item.m_Type == NETOBJTYPE_DDNETPICKUP || Item.m_Type == NETOBJTYPE_LASER || Item.m_Type == NETOBJTYPE_DDNETLASER || Item.m_Type == NETOBJTYPE_PROJECTILE || Item.m_Type == NETOBJTYPE_DDRACEPROJECTILE || Item.m_Type == NETOBJTYPE_DDNETPROJECTILE)
		{
			CEntitySnapshot &Entity = EntitySlot(m_vEntityBuffer, NumEntities++);
			Entity.m_Id = Item.m_Id;
			Entity.m_Type = Item.m_Type;
			Entity.m_HasEntityEx = false;
			Entity.m_EntityEx = {};
			const auto *pData = static_cast<const unsigned char *>(Item.m_pData);
			Entity.m_vData.assign(pData, pData + Item.m_DataSize);
			if(const auto *pPrevData = static_cast<const unsigned char *>(Client.SnapFindItem(SessionId, StreamId, IClient::SNAP_PREV, Item.m_Type, Item.m_Id)))
				Entity.m_vPrevData.assign(pPrevData, pPrevData + Item.m_DataSize);
			else
				Entity.m_vPrevData.clear();
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
				if(Client.SnapFindItem(SessionId, StreamId, IClient::SNAP_PREV, NETOBJTYPE_PLAYERINFO, Item.m_Id))
				{
					SnapshotClient.m_HasPrevPlayerInfo = true;
				}
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
				if(const auto *pPrev = static_cast<const CNetObj_Character *>(Client.SnapFindItem(SessionId, StreamId, IClient::SNAP_PREV, NETOBJTYPE_CHARACTER, Item.m_Id)))
				{
					SnapshotClient.m_HasPrevCharacter = true;
					SnapshotClient.m_PrevCharacter = *pPrev;
					const bool EvolvePrev = Client.PrevGameTick(SessionId, StreamId) - SnapshotClient.m_PrevCharacter.m_Tick <= 3 * Client.GameTickSpeed();
					const bool EvolveCur = Client.GameTick(SessionId, StreamId) - SnapshotClient.m_Character.m_Tick <= 3 * Client.GameTickSpeed();
					if(EvolveCur && m_vEvolvedCharacters[Item.m_Id].m_Tick == Client.PrevGameTick(SessionId, StreamId))
					{
						if(mem_comp(&SnapshotClient.m_PrevCharacter, &m_vSnappedCharacters[Item.m_Id], sizeof(CNetObj_Character)) == 0)
							SnapshotClient.m_PrevCharacter = m_vEvolvedCharacters[Item.m_Id];
						if(mem_comp(&SnapshotClient.m_Character, &m_vSnappedCharacters[Item.m_Id], sizeof(CNetObj_Character)) == 0)
							SnapshotClient.m_Character = m_vEvolvedCharacters[Item.m_Id];
					}
					if(m_PredictionInitialized && EvolvePrev && SnapshotClient.m_PrevCharacter.m_Tick)
						EvolveCharacter(SnapshotClient.m_PrevCharacter, Client.PrevGameTick(SessionId, StreamId));
					if(m_PredictionInitialized && EvolveCur && SnapshotClient.m_Character.m_Tick)
						EvolveCharacter(SnapshotClient.m_Character, Client.GameTick(SessionId, StreamId));
					m_vSnappedCharacters[Item.m_Id] = *static_cast<const CNetObj_Character *>(Item.m_pData);
					m_vEvolvedCharacters[Item.m_Id] = SnapshotClient.m_Character;
				}
				else
					m_vEvolvedCharacters[Item.m_Id].m_Tick = -1;
				break;
			case NETOBJTYPE_DDNETCHARACTER:
				SnapshotClient.m_HasExtendedCharacter = true;
				SnapshotClient.m_ExtendedCharacter = *static_cast<const CNetObj_DDNetCharacter *>(Item.m_pData);
				if(const auto *pPrev = static_cast<const CNetObj_DDNetCharacter *>(Client.SnapFindItem(SessionId, StreamId, IClient::SNAP_PREV, NETOBJTYPE_DDNETCHARACTER, Item.m_Id)))
				{
					SnapshotClient.m_HasPrevExtendedCharacter = true;
					SnapshotClient.m_PrevExtendedTargetX = pPrev->m_TargetX;
					SnapshotClient.m_PrevExtendedTargetY = pPrev->m_TargetY;
				}
				break;
			case NETOBJTYPE_DDNETPLAYER:
				SnapshotClient.m_HasDDNetPlayer = true;
				SnapshotClient.m_DDNetPlayer = *static_cast<const CNetObj_DDNetPlayer *>(Item.m_pData);
				break;
			case NETOBJTYPE_SPECCHAR:
				SnapshotClient.m_HasSpecChar = true;
				SnapshotClient.m_SpecChar = *static_cast<const CNetObj_SpecChar *>(Item.m_pData);
				break;
			}
		}
	}
	m_vEntityBuffer.resize(NumEntities);
	m_vEntityEx.resize(NumEntityEx);
	// Sorted once so that every entity can find its extension by a binary search
	// instead of scanning the whole list. The ids of one snapshot type are unique.
	std::sort(m_vEntityEx.begin(), m_vEntityEx.end(), [](const CEntitySnapshot &Left, const CEntitySnapshot &Right) { return Left.m_Id < Right.m_Id; });
	for(CEntitySnapshot &Entity : m_vEntityBuffer)
	{
		if(Entity.m_Type == NETOBJTYPE_GAMEDATA || Entity.m_Type == NETOBJTYPE_FLAG)
			continue;
		const auto Found = std::lower_bound(m_vEntityEx.begin(), m_vEntityEx.end(), Entity.m_Id, [](const CEntitySnapshot &EntityEx, int Id) { return EntityEx.m_Id < Id; });
		if(Found != m_vEntityEx.end() && Found->m_Id == Entity.m_Id)
		{
			Entity.m_HasEntityEx = true;
			Entity.m_EntityEx = Found->m_EntityEx;
		}
	}
	ApplySnapshotData(Client.GameTick(SessionId, StreamId), NumItems, aClients, HasGameInfo ? &GameInfo : nullptr, &m_vEntityBuffer);
	if(HasSpectatorInfo)
		ApplySpectatorInfo(SpectatorInfo);
	if(HasSpectatorCount)
		ApplySpectatorCount(SpectatorCount);
}

void CGameState::ApplySnapshotData(int Tick, int NumItems, const std::array<CClientSnapshot, MAX_CLIENTS> &aClients, const CNetObj_GameInfo *pGameInfo, std::vector<CEntitySnapshot> *pEntities)
{
	m_aClients = aClients;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CClientSnapshot &SnapshotClient = m_aClients[ClientId];
		if(!SnapshotClient.m_HasPlayerInfo)
		{
			m_vClientIdentities[ClientId].Reset();
			m_vClientEmoticons[ClientId].Reset();
		}
		else if(SnapshotClient.m_HasClientInfo)
		{
			m_vClientIdentities[ClientId].m_Active = true;
			m_vClientIdentities[ClientId].m_ClientInfo = SnapshotClient.m_ClientInfo;
		}
	}
	// Swapped, not moved: the caller gets the entities of the previous snapshot
	// back and reuses their storage for the next one.
	if(pEntities != nullptr)
		m_vEntities.swap(*pEntities);
	else
		m_vEntities.clear();
	m_HasGameInfo = pGameInfo != nullptr;
	m_GameInfo = pGameInfo ? *pGameInfo : CNetObj_GameInfo{};
	m_HasSpectatorInfo = false;
	m_SpectatorInfo = {};
	m_HasSpectatorCount = false;
	m_SpectatorCount = {};
	int LocalClientId = -1;
	bool HasUnsetDDNetFinishTimes = false;
	bool HasTrueMillisecondFinishTimes = false;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!m_aClients[ClientId].m_Active)
			m_aProtocol7Clients[ClientId].Reset();
		if(m_aClients[ClientId].m_HasPlayerInfo && m_aClients[ClientId].m_PlayerInfo.m_Local)
			LocalClientId = ClientId;
		if(m_aClients[ClientId].m_HasDDNetPlayer)
		{
			m_Runtime.m_ReceivedDDNetPlayer = true;
			if(m_aClients[ClientId].m_DDNetPlayer.m_FinishTimeSeconds == FinishTime::UNSET)
				HasUnsetDDNetFinishTimes = true;
			else if(m_aClients[ClientId].m_DDNetPlayer.m_FinishTimeMillis % 10 != 0)
				HasTrueMillisecondFinishTimes = true;
		}
	}
	m_Runtime.m_ReceivedDDNetPlayerFinishTimes = m_Runtime.m_ReceivedDDNetPlayer && !HasUnsetDDNetFinishTimes;
	m_Runtime.m_ReceivedDDNetPlayerFinishTimesMillis = m_Runtime.m_ReceivedDDNetPlayer && HasTrueMillisecondFinishTimes;
	ApplySnapshotMetadata(Tick, NumItems, LocalClientId);
	RebuildGameWorld();
}

void CGameState::ApplyEmoticon(int ClientId, int Emoticon, int Tick, float StartFraction)
{
	if(!in_range(ClientId, MAX_CLIENTS - 1))
		return;
	CClientEmoticonState &State = m_vClientEmoticons[ClientId];
	State.m_Emoticon = Emoticon;
	State.m_StartTick = Tick;
	State.m_StartFraction = StartFraction;
}

void CGameState::ApplySnapshotMetadata(int Tick, int NumItems, int LocalClientId)
{
	m_SnapshotTick = Tick;
	m_SnapshotItems = NumItems;
	m_LocalClientId = LocalClientId;
}

void CGameState::ApplyTuning(const CTuningParams &Tuning, int TuneZone)
{
	m_Runtime.m_CurrentTuning = Tuning;
	if(TuneZone >= 0 && TuneZone < TuneZone::NUM)
		m_aTuning[TuneZone] = Tuning;
	UpdateWorldConfigFromSnapshot();
}

const CNetObj_DDNetCharacter *CGameState::ExtendedCharacter(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || !m_aClients[ClientId].m_HasExtendedCharacter)
		return nullptr;
	return &m_aClients[ClientId].m_ExtendedCharacter;
}

const CNetObj_GameData *CGameState::GameData() const
{
	const auto It = std::find_if(m_vEntities.begin(), m_vEntities.end(), [](const CEntitySnapshot &Entity) { return Entity.m_Type == NETOBJTYPE_GAMEDATA && Entity.m_vData.size() >= sizeof(CNetObj_GameData); });
	return It == m_vEntities.end() ? nullptr : reinterpret_cast<const CNetObj_GameData *>(It->m_vData.data());
}

void CGameState::SetTeam(int ClientId, int Team)
{
	if(ClientId >= 0 && ClientId < MAX_CLIENTS)
		m_Teams.Team(ClientId, Team);
}

void CGameState::SetCoreGameInfo(const CGameInfo &GameInfo)
{
	m_CoreGameInfo = GameInfo;
	m_Teams.m_IsDDRace64 = !GameInfo.m_Supports128Teams;
	m_GameWorld.m_WorldConfig.m_IsVanilla = GameInfo.m_PredictVanilla;
	m_GameWorld.m_WorldConfig.m_IsDDRace = GameInfo.m_PredictDDRace;
	m_GameWorld.m_WorldConfig.m_IsFNG = GameInfo.m_PredictFNG;
	m_GameWorld.m_WorldConfig.m_PredictDDRace = GameInfo.m_PredictDDRace;
	m_GameWorld.m_WorldConfig.m_PredictTiles = GameInfo.m_PredictDDRace && GameInfo.m_PredictDDRaceTiles;
	m_GameWorld.m_WorldConfig.m_UseTuneZones = GameInfo.m_PredictDDRaceTiles;
	m_GameWorld.m_WorldConfig.m_BugDDRaceInput = GameInfo.m_BugDDRaceInput;
	m_GameWorld.m_WorldConfig.m_NoWeakHookAndBounce = GameInfo.m_NoWeakHookAndBounce;
	m_GameWorld.m_WorldConfig.m_PredictEvents = GameInfo.m_PredictEvents;
	m_GameWorld.UpdatePhysicsRules();
}

void CGameState::UpdateWorldConfigFromSnapshot()
{
	if(m_LocalClientId < 0 || m_LocalClientId >= MAX_CLIENTS || !m_aClients[m_LocalClientId].m_HasCharacter)
		return;
	const CClientSnapshot &LocalClient = m_aClients[m_LocalClientId];
	if(LocalClient.m_Character.m_AmmoCount > 0 && LocalClient.m_Character.m_Weapon != WEAPON_NINJA)
		m_GameWorld.m_WorldConfig.m_InfiniteAmmo = false;
	m_GameWorld.m_WorldConfig.m_IsSolo = !LocalClient.m_HasExtendedCharacter && !m_Runtime.m_CurrentTuning.m_PlayerCollision && !m_Runtime.m_CurrentTuning.m_PlayerHooking;
}

void CGameState::RebuildGameWorld()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aClients[ClientId].m_HasExtendedCharacter)
			m_Teams.SetSolo(ClientId, (m_aClients[ClientId].m_ExtendedCharacter.m_Flags & CHARACTERFLAG_SOLO) != 0);
	}

	// The full prediction advances this world itself and reconciles it with the
	// snapshot, so rebuilding it here would drop what it computed.
	if(!m_PredictionInitialized || m_FullyPredicted)
		return;
	UpdateWorldConfigFromSnapshot();

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

void CGameState::Predict(const IClient &Client, CSessionId SessionId, CStreamId StreamId)
{
	PredictTo(Client.PredGameTick(SessionId, StreamId), [&Client, SessionId, StreamId](int Tick) {
		return reinterpret_cast<const CNetObj_PlayerInput *>(Client.GetInput(SessionId, StreamId, Tick));
	});
}

void CGameState::PredictTo(int TargetTick, const std::function<const CNetObj_PlayerInput *(int)> &InputAt)
{
	m_PredictionTick = TargetTick;
	// Only the flags are cleared. Assigning a fresh array would run a
	// CCharacterCore constructor for every client twice over, and each of those
	// builds a std::set, so a frame would start with a hundred allocations that
	// nothing reads: the cores below are written before anyone looks at them.
	for(CPredictedClient &PredictedClient : m_aPredictedClients)
	{
		PredictedClient.m_HasPrev = false;
		PredictedClient.m_HasCurrent = false;
	}
	if(m_FullyPredicted)
		return;
	if(!m_PredictionInitialized || m_LocalClientId < 0 || m_LocalClientId >= MAX_CLIENTS || !m_aClients[m_LocalClientId].m_HasCharacter)
		return;
	if(m_HasGameInfo && (m_GameInfo.m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return;

	m_PredictedWorld.CopyWorld(&m_GameWorld);
	CCharacter *pLocalCharacter = m_PredictedWorld.GetCharacterById(m_LocalClientId);
	if(!pLocalCharacter)
		return;
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
	auto RecordPredictionHistory = [this](int Tick) {
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(const CCharacter *pCharacter = m_PredictedWorld.GetCharacterById(ClientId))
			{
				m_vClientPredictionHistory[ClientId].m_aPredPos[Tick % 200] = pCharacter->Core()->m_Pos;
				m_vClientPredictionHistory[ClientId].m_aPredTick[Tick % 200] = Tick;
			}
		}
	};
	RecordPredictionHistory(m_SnapshotTick);

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
		RecordPredictionHistory(Tick);
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

void CGameState::UpdateRenderedClient(int ClientId, bool UsePredicted, bool PredictedLocal, float IntraGameTick, float PredIntraGameTick)
{
	CRenderedClient &RenderedClient = m_vRenderedClients[ClientId];
	RenderedClient = {};
	const CClientSnapshot &SnapshotClient = m_aClients[ClientId];
	if(!SnapshotClient.m_HasCharacter || !SnapshotClient.m_HasPrevCharacter)
		return;

	RenderedClient.m_Active = true;
	RenderedClient.m_Prev = SnapshotClient.m_PrevCharacter;
	RenderedClient.m_Cur = SnapshotClient.m_Character;
	float Intra = IntraGameTick;
	const CPredictedClient &PredictedClient = m_aPredictedClients[ClientId];
	if(UsePredicted && PredictedClient.m_HasPrev && PredictedClient.m_HasCurrent)
	{
		PredictedClient.m_Prev.Write(&RenderedClient.m_Prev);
		PredictedClient.m_Current.Write(&RenderedClient.m_Cur);
		RenderedClient.m_IsPredicted = true;
		RenderedClient.m_IsPredictedLocal = PredictedLocal;
		Intra = PredIntraGameTick;
	}
	RenderedClient.m_Position = mix(
		vec2(RenderedClient.m_Prev.m_X, RenderedClient.m_Prev.m_Y),
		vec2(RenderedClient.m_Cur.m_X, RenderedClient.m_Cur.m_Y),
		Intra);
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

bool CGameState::IsOtherTeamFromLocalPlayer(int ClientId) const
{
	if(!in_range(ClientId, MAX_CLIENTS - 1) || !in_range(m_LocalClientId, MAX_CLIENTS - 1))
		return false;
	const CNetObj_DDNetCharacter *pLocalExtended = ExtendedCharacter(m_LocalClientId);
	const CNetObj_DDNetCharacter *pClientExtended = ExtendedCharacter(ClientId);
	const bool LocalSolo = pLocalExtended != nullptr && (pLocalExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
	const bool ClientSolo = pClientExtended != nullptr && (pClientExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
	if((LocalSolo || ClientSolo) && ClientId != m_LocalClientId)
		return true;
	if(m_Teams.Team(ClientId) == TEAM_SUPER || m_Teams.Team(m_LocalClientId) == TEAM_SUPER)
		return false;
	return m_Teams.Team(ClientId) != m_Teams.Team(m_LocalClientId);
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
	DigestValue(Digest, m_CoreGameInfo);
	DigestValue(Digest, m_HasSpectatorInfo);
	if(m_HasSpectatorInfo)
		DigestValue(Digest, m_SpectatorInfo);
	DigestValue(Digest, m_HasSpectatorCount);
	if(m_HasSpectatorCount)
		DigestValue(Digest, m_SpectatorCount);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CClientSnapshot &SnapshotClient = m_aClients[ClientId];
		DigestValue(Digest, SnapshotClient.m_Active);
		DigestValue(Digest, SnapshotClient.m_HasPlayerInfo);
		DigestValue(Digest, SnapshotClient.m_HasPrevPlayerInfo);
		DigestValue(Digest, SnapshotClient.m_HasClientInfo);
		DigestValue(Digest, SnapshotClient.m_HasCharacter);
		DigestValue(Digest, SnapshotClient.m_HasPrevCharacter);
		DigestValue(Digest, SnapshotClient.m_HasExtendedCharacter);
		DigestValue(Digest, SnapshotClient.m_HasPrevExtendedCharacter);
		DigestValue(Digest, SnapshotClient.m_HasDDNetPlayer);
		DigestValue(Digest, SnapshotClient.m_HasSpecChar);
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
		if(SnapshotClient.m_HasPrevExtendedCharacter)
		{
			DigestValue(Digest, SnapshotClient.m_PrevExtendedTargetX);
			DigestValue(Digest, SnapshotClient.m_PrevExtendedTargetY);
		}
		if(SnapshotClient.m_HasDDNetPlayer)
			DigestValue(Digest, SnapshotClient.m_DDNetPlayer);
		if(SnapshotClient.m_HasSpecChar)
			DigestValue(Digest, SnapshotClient.m_SpecChar);
		const CClientEmoticonState &Emoticon = m_vClientEmoticons[ClientId];
		DigestValue(Digest, Emoticon.m_Emoticon);
		DigestValue(Digest, Emoticon.m_StartFraction);
		DigestValue(Digest, Emoticon.m_StartTick);
		const CClientIdentityState &Identity = m_vClientIdentities[ClientId];
		DigestValue(Digest, Identity.m_Active);
		if(Identity.m_Active)
			DigestValue(Digest, Identity.m_ClientInfo);
		const CProtocol7ClientState &Protocol7Client = m_aProtocol7Clients[ClientId];
		if(Protocol7Client.m_Active)
		{
			DigestValue(Digest, Protocol7Client.m_Active);
			for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
			{
				DigestBytes(Digest, Protocol7Client.m_aaSkinPartNames[Part], str_length(Protocol7Client.m_aaSkinPartNames[Part]));
				DigestValue(Digest, Protocol7Client.m_aUseCustomColors[Part]);
				DigestValue(Digest, Protocol7Client.m_aSkinPartColors[Part]);
			}
			DigestValue(Digest, Protocol7Client.m_PlayerFlags);
		}
		DigestValue(Digest, m_Teams.Team(ClientId));
		DigestValue(Digest, m_Teams.GetSolo(ClientId));
	}
	for(const CEntitySnapshot &Entity : m_vEntities)
	{
		DigestValue(Digest, Entity.m_Id);
		DigestValue(Digest, Entity.m_Type);
		DigestBytes(Digest, Entity.m_vData.data(), Entity.m_vData.size());
		DigestBytes(Digest, Entity.m_vPrevData.data(), Entity.m_vPrevData.size());
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
