#include "test.h"

#include <base/str.h>

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/demo.h>
#include <engine/shared/network.h>
#include <engine/shared/snapshot.h>
#include <engine/storage.h>

#include <game/client/components/envelope_state.h>
#include <game/client/game_state.h>
#include <game/client/game_view.h>
#include <game/client/session_context.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{
	using CClients = std::array<CGameState::CClientSnapshot, MAX_CLIENTS>;

	bool ApplySnapshot(CGameState &State, int Tick, const CSnapshot *pSnapshot, int Size)
	{
		if(!pSnapshot->IsValid(Size))
			return false;
		CClients aClients = {};
		for(int i = 0; i < pSnapshot->NumItems(); i++)
		{
			const CSnapshotItem *pItem = pSnapshot->GetItem(i);
			if(pItem->Id() < 0 || pItem->Id() >= MAX_CLIENTS)
				continue;
			CGameState::CClientSnapshot &Client = aClients[pItem->Id()];
			if(pItem->InternalType() == NETOBJTYPE_PLAYERINFO)
			{
				Client.m_Active = true;
				Client.m_HasPlayerInfo = true;
				Client.m_PlayerInfo = *reinterpret_cast<const CNetObj_PlayerInfo *>(pItem->Data());
			}
			else if(pItem->InternalType() == NETOBJTYPE_CHARACTER)
			{
				Client.m_Active = true;
				Client.m_HasCharacter = true;
				Client.m_Character = *reinterpret_cast<const CNetObj_Character *>(pItem->Data());
			}
		}
		State.ApplySnapshotData(Tick, aClients);
		return true;
	}

	class CDemoGameStateListener : public CDemoPlayer::IListener
	{
		CDemoPlayer &m_Player;
		CGameState &m_State;
		int m_NumSnapshots = 0;

	public:
		CDemoGameStateListener(CDemoPlayer &Player, CGameState &State) :
			m_Player(Player),
			m_State(State)
		{
		}

		void OnDemoPlayerSnapshot(void *pData, int Size) override
		{
			m_NumSnapshots++;
			const auto *pSnapshot = static_cast<const CSnapshot *>(pData);
			ApplySnapshot(m_State, m_Player.Info()->m_Info.m_CurrentTick, pSnapshot, Size);
		}

		void OnDemoPlayerMessage(void *pData, int Size) override {}

		int NumSnapshots() const { return m_NumSnapshots; }
	};
}

TEST(GameState, SnapshotDataFindsLocalClientAndClearsSpectatorInfo)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CTuningParams Tuning = CTuningParams::DEFAULT;
	Tuning.Set("gravity", 0.25f);
	State.ApplyTuning(Tuning);

	auto pClients = std::make_unique<CClients>();
	(*pClients)[5].m_HasPlayerInfo = true;
	(*pClients)[5].m_PlayerInfo.m_Local = 1;
	(*pClients)[5].m_PlayerInfo.m_Team = TEAM_SPECTATORS;
	State.ApplySnapshotData(80, *pClients);
	CNetObj_SpectatorInfo SpectatorInfo = {};
	SpectatorInfo.m_SpectatorId = 9;
	State.ApplySpectatorInfo(SpectatorInfo);
	CNetObj_SpectatorCount SpectatorCount = {};
	SpectatorCount.m_NumSpectators = 7;
	State.ApplySpectatorCount(SpectatorCount);

	EXPECT_EQ(State.SnapshotTick(), 80);
	EXPECT_EQ(State.LocalClientId(), 5);
	EXPECT_FLOAT_EQ(State.Tuning().m_Gravity, 0.25f);
	EXPECT_EQ(State.SpectatorInfo().m_SpectatorId, 9);
	EXPECT_EQ(State.SpectatorCount().m_NumSpectators, 7);

	State.ApplySnapshotData(81, {});
	EXPECT_EQ(State.LocalClientId(), -1);
	EXPECT_FALSE(State.HasSpectatorInfo());
	EXPECT_FALSE(State.HasSpectatorCount());
}

TEST(GameState, ResetClearsStateData)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	auto pClients = std::make_unique<CClients>();
	CGameState::CClientSnapshot &Client = (*pClients)[4];
	Client.m_Active = true;
	Client.m_HasPlayerInfo = true;
	Client.m_HasClientInfo = true;
	Client.m_HasCharacter = true;
	Client.m_HasPrevCharacter = true;
	Client.m_HasExtendedCharacter = true;
	Client.m_ExtendedCharacter.m_Flags = CHARACTERFLAG_SOLO;
	StrToInts(Client.m_ClientInfo.m_aName, std::size(Client.m_ClientInfo.m_aName), "name");
	State.SetTeam(4, 3);
	State.ApplySnapshotData(10, *pClients);
	State.ApplyEmoticon(4, 3, 11, 0.25f);
	CGameInfo GameInfo;
	GameInfo.m_Race = true;
	GameInfo.m_PredictDDRace = true;
	State.SetCoreGameInfo(GameInfo);
	State.UpdateRenderedClient(4, false, false, 0.5f, 0.0f);
	State.PredictionHistory(4).m_aSmoothLen[0] = 100;
	State.Input().m_MousePos = vec2(10.0f, 20.0f);
	State.m_Runtime.m_ServerMode = CGameState::SERVERMODE_MOD;
	State.m_DamageIndicators.Create(vec2(10.0f, 20.0f), vec2(1.0f, 0.0f), 4, 0.5f, 0.25f);
	State.m_EffectClock.Update(1000, 1000, 1.0f);
	State.m_SceneClock.Update(1000, 1000, 1.0f, 0.25f, 0.5f);
	CGameState::CParticle Particle;
	Particle.SetDefault();
	ASSERT_TRUE(State.m_Particles.Add(0, Particle));

	EXPECT_EQ(State.Teams().Team(4), 3);
	EXPECT_TRUE(State.Teams().GetSolo(4));
	EXPECT_TRUE(State.ClientIdentity(4).m_Active);
	EXPECT_EQ(State.ClientEmoticon(4).m_StartTick, 11);
	EXPECT_TRUE(State.m_GameWorld.m_WorldConfig.m_IsDDRace);
	EXPECT_TRUE(State.RenderedClient(4).m_Active);

	State.Reset();
	EXPECT_EQ(State.Teams().Team(4), TEAM_FLOCK);
	EXPECT_FALSE(State.Teams().GetSolo(4));
	EXPECT_FALSE(State.ClientIdentity(4).m_Active);
	EXPECT_EQ(State.ClientEmoticon(4).m_StartTick, -1);
	EXPECT_FALSE(State.CoreGameInfo().m_Race);
	EXPECT_FALSE(State.m_GameWorld.m_WorldConfig.m_IsDDRace);
	EXPECT_FALSE(State.RenderedClient(4).m_Active);
	EXPECT_EQ(State.PredictionHistory(4).m_aSmoothLen[0], 0);
	EXPECT_EQ(State.Input().m_MousePos, vec2(0.0f, 0.0f));
	EXPECT_EQ(State.m_Runtime.m_ServerMode, CGameState::SERVERMODE_PURE);
	EXPECT_EQ(State.m_DamageIndicators.NumItems(), 0);
	EXPECT_FALSE(State.m_EffectClock.m_Add5hz);
	EXPECT_FALSE(State.m_SceneClock.m_Initialized);
	EXPECT_EQ(State.m_Particles.NumParticles(), 0);
}

TEST(GameState, RenderedClientsInterpolateSnapshots)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	auto pClients = std::make_unique<CClients>();
	CGameState::CClientSnapshot &Client = (*pClients)[4];
	Client.m_Active = true;
	Client.m_HasPlayerInfo = true;
	Client.m_HasPrevPlayerInfo = true;
	Client.m_HasCharacter = true;
	Client.m_HasPrevCharacter = true;
	Client.m_PrevCharacter.m_X = 100;
	Client.m_PrevCharacter.m_Y = 200;
	Client.m_Character.m_X = 300;
	Client.m_Character.m_Y = 600;
	State.ApplySnapshotData(10, *pClients);
	State.UpdateRenderedClient(4, false, false, 0.25f, 0.0f);
	EXPECT_TRUE(State.RenderedClient(4).m_Active);
	EXPECT_EQ(State.RenderedClient(4).m_Position, vec2(150.0f, 300.0f));

	auto pIncompleteClients = std::make_unique<CClients>();
	(*pIncompleteClients)[4].m_HasPlayerInfo = true;
	(*pIncompleteClients)[4].m_HasCharacter = true;
	(*pIncompleteClients)[4].m_HasPrevCharacter = true;
	State.ApplySnapshotData(30, *pIncompleteClients);
	State.UpdateRenderedClient(4, false, false, 0.5f, 0.0f);
	EXPECT_TRUE(State.RenderedClient(4).m_Active);
}

TEST(GameState, DamageIndicatorsAdvanceAndExpire)
{
	CGameState::CDamageIndicatorState Indicators;
	Indicators.Create(vec2(10.0f, 20.0f), vec2(1.0f, 0.0f), 4, 0.5f, 0.25f);
	ASSERT_EQ(Indicators.NumItems(), 1);
	EXPECT_EQ(Indicators.Item(0).m_Dir, vec2(-1.0f, 0.0f));
	EXPECT_FLOAT_EQ(Indicators.Item(0).m_Color.a, 0.5f);
	EXPECT_EQ(Indicators.Item(0).m_OwnerClientId, 4);

	Indicators.Advance(1000, 1000, 1.0f);
	Indicators.Advance(1250, 1000, 1.0f);
	EXPECT_FLOAT_EQ(Indicators.Item(0).m_RemainingLife, 0.5f);
	Indicators.Advance(1500, 1000, 0.0f);
	EXPECT_FLOAT_EQ(Indicators.Item(0).m_RemainingLife, 0.5f);
	Indicators.Update(0.8f);
	EXPECT_EQ(Indicators.NumItems(), 0);
}

TEST(GameState, EffectClockThrottlesSkidSound)
{
	CGameState::CEffectClockState Clock;
	Clock.Update(5, 1000, 1.0f);
	EXPECT_FALSE(Clock.m_Add5hz);
	Clock.Update(1000, 1000, 1.0f);
	EXPECT_TRUE(Clock.m_Add5hz);
	EXPECT_TRUE(Clock.m_Add50hz);
	EXPECT_TRUE(Clock.m_Add100hz);
	EXPECT_FALSE(Clock.TrySkidSound(100, 1000));
	EXPECT_TRUE(Clock.TrySkidSound(101, 1000));
	EXPECT_FALSE(Clock.TrySkidSound(201, 1000));
	EXPECT_TRUE(Clock.TrySkidSound(202, 1000));
}

TEST(GameState, SceneClockHoldsWhilePaused)
{
	CGameState::CSceneClockState Clock;
	Clock.Update(1000, 1000, 1.0f, 0.25f, 0.5f);
	Clock.Update(1500, 1000, 1.0f, 0.5f, 0.75f);
	Clock.Update(1500, 1000, 1.0f, 0.5f, 0.75f);
	EXPECT_FLOAT_EQ(Clock.m_AnimationTime, 0.5f);
	EXPECT_FLOAT_EQ(Clock.m_GameTickTime, 0.5f);
	EXPECT_FLOAT_EQ(Clock.m_PredIntraTick, 0.75f);

	Clock.Update(2000, 1000, 0.0f, 0.9f, 0.9f);
	EXPECT_FLOAT_EQ(Clock.m_AnimationTime, 0.5f);
	EXPECT_FLOAT_EQ(Clock.m_GameTickTime, 0.5f);
	EXPECT_FLOAT_EQ(Clock.m_PredIntraTick, 0.75f);
}

TEST(GameState, EnvelopeTimeUsesFrozenStateTicks)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 50;
	State.ApplySnapshotData(101, {}, &GameInfo);

	CGameTickInfo Time;
	Time.m_PrevGameTick = 100;
	Time.m_GameTick = 101;
	Time.m_PredGameTick = 104;
	Time.m_IntraGameTick = 0.25f;
	Time.m_PredIntraGameTick = 0.5f;
	Time.m_GameTickSpeed = 50;
	EXPECT_EQ(CEnvelopeState::CalculateOnlineTime(State, Time, false).count(), 1005000000);
	EXPECT_EQ(CEnvelopeState::CalculateOnlineTime(State, Time, true).count(), 1070000000);

	Time.m_PrevGameTick = 200;
	Time.m_GameTick = 201;
	Time.m_IntraGameTick = 0.5f;
	Time.m_GameTickSpeed = 100;
	GameInfo.m_RoundStartTick = 0;
	State.ApplySnapshotData(201, {}, &GameInfo);
	EXPECT_EQ(CEnvelopeState::CalculateOnlineTime(State, Time, false).count(), 2005000000);

	State.Reset();
	EXPECT_EQ(CEnvelopeState::CalculateOnlineTime(State, Time, false).count(), 0);
}

TEST(GameState, ParticlePoolIsBounded)
{
	CGameState::CParticleSystemState Particles;
	CGameState::CParticle Particle;
	Particle.SetDefault();
	Particle.m_LifeSpan = 1.0f;
	EXPECT_EQ(Particle.m_OwnerClientId, -1);

	EXPECT_FALSE(Particles.Add(-1, Particle));
	for(int i = 0; i < CGameState::CParticleSystemState::MAX_PARTICLES; i++)
		ASSERT_TRUE(Particles.Add(0, Particle));
	EXPECT_FALSE(Particles.Add(0, Particle));
	EXPECT_EQ(Particles.NumParticles(), CGameState::CParticleSystemState::MAX_PARTICLES);
}

TEST(GameState, EntitySnapshotsKeepCurrentAndPreviousData)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	std::vector<CGameState::CEntitySnapshot> vEntities;
	auto AddEntity = [&vEntities](int Id, int Type, const auto &CurrentData, const auto &PrevData) {
		CGameState::CEntitySnapshot Entity;
		Entity.m_Id = Id;
		Entity.m_Type = Type;
		const auto *pCurrentData = reinterpret_cast<const unsigned char *>(&CurrentData);
		Entity.m_vData.assign(pCurrentData, pCurrentData + sizeof(CurrentData));
		const auto *pPrevData = reinterpret_cast<const unsigned char *>(&PrevData);
		Entity.m_vPrevData.assign(pPrevData, pPrevData + sizeof(PrevData));
		vEntities.push_back(std::move(Entity));
	};
	CNetObj_Pickup Pickup = {};
	Pickup.m_X = 100;
	CNetObj_Pickup PrevPickup = {};
	PrevPickup.m_X = 50;
	AddEntity(7, NETOBJTYPE_PICKUP, Pickup, PrevPickup);
	CNetObj_GameData GameData = {};
	GameData.m_FlagCarrierRed = 4;
	CNetObj_GameData PrevGameData = GameData;
	PrevGameData.m_FlagCarrierRed = FLAG_ATSTAND;
	AddEntity(0, NETOBJTYPE_GAMEDATA, GameData, PrevGameData);
	State.ApplySnapshotData(10, {}, nullptr, vEntities);

	ASSERT_EQ(State.Entities().size(), 2U);
	const CGameState::CEntitySnapshot &Stored = State.Entities().front();
	EXPECT_EQ(Stored.m_Id, 7);
	EXPECT_EQ(reinterpret_cast<const CNetObj_Pickup *>(Stored.m_vData.data())->m_X, 100);
	EXPECT_EQ(reinterpret_cast<const CNetObj_Pickup *>(Stored.m_vPrevData.data())->m_X, 50);
	ASSERT_NE(State.GameData(), nullptr);
	EXPECT_EQ(State.GameData()->m_FlagCarrierRed, 4);

	State.ApplySnapshotData(20, {});
	EXPECT_EQ(State.GameData(), nullptr);
	EXPECT_TRUE(State.Entities().empty());
}

TEST(GameState, ReleaseGameplayReleasesHeldInput)
{
	CGameState::CInputState Input;
	Input.m_LastData.m_Direction = 1;
	Input.m_LastData.m_Jump = 1;
	Input.m_LastData.m_Hook = 1;
	Input.m_LastData.m_Fire = 5;
	Input.m_LastData.m_NextWeapon = 7;
	Input.m_InputDirectionLeft = 1;
	Input.m_InputDirectionRight = 1;
	Input.ReleaseGameplay();
	EXPECT_EQ(Input.m_InputData.m_Direction, 0);
	EXPECT_EQ(Input.m_InputData.m_Jump, 0);
	EXPECT_EQ(Input.m_InputData.m_Hook, 0);
	EXPECT_EQ(Input.m_InputData.m_Fire, 6);
	EXPECT_EQ(Input.m_InputData.m_NextWeapon, 7);
	EXPECT_EQ(Input.m_InputDirectionLeft, 0);
	EXPECT_EQ(Input.m_InputDirectionRight, 0);
}

TEST(GameState, GeneratedDemoPlaysHeadlessly)
{
	static constexpr const char *pFilename = "client-game-state-headless.demo";
	const std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	const std::unique_ptr<IConsole> pConsole = CreateConsole(CFGFLAG_CLIENT);
	CNetBase::Init();
	const auto pSnapshotDelta = std::make_unique<CSnapshotDelta>();
	const auto pRecorder = std::make_unique<CDemoRecorder>(pSnapshotDelta.get(), true);
	unsigned char DummyMapData = 0;
	const SHA256_DIGEST MapSha256 = {};
	ASSERT_EQ(pRecorder->Start(pStorage.get(), pConsole.get(), pFilename, "test", "headless", MapSha256, 0, "client", 0, &DummyMapData, nullptr, nullptr), 0);

	auto RecordSnapshot = [&pRecorder](int Tick, int X) {
		CSnapshotBuilder Builder;
		Builder.Init();
		CNetObj_PlayerInfo PlayerInfo = {};
		PlayerInfo.m_Local = 1;
		PlayerInfo.m_ClientId = 2;
		CNetObj_Character Character = {};
		Character.m_X = X;
		Character.m_Y = 320;
		EXPECT_TRUE(Builder.NewItem(NETOBJTYPE_CHARACTER, 2, &Character, sizeof(Character)));
		EXPECT_TRUE(Builder.NewItem(NETOBJTYPE_PLAYERINFO, 2, &PlayerInfo, sizeof(PlayerInfo)));
		CSnapshotBuffer Buffer;
		const int Size = Builder.Finish(&Buffer);
		EXPECT_TRUE(Buffer.AsSnapshot()->IsValid(Size));
		pRecorder->RecordSnapshot(Tick, Buffer.AsSnapshot(), Size);
	};
	RecordSnapshot(10, 100);
	RecordSnapshot(300, 200);
	RecordSnapshot(600, 300);
	ASSERT_EQ(pRecorder->Stop(IDemoRecorder::EStopMode::KEEP_FILE), 0);

	const auto pPlaybackDelta = std::make_unique<CSnapshotDelta>();
	const auto pPlaybackDeltaSixup = std::make_unique<CSnapshotDelta>();
	const auto pPlayer = std::make_unique<CDemoPlayer>(pPlaybackDelta.get(), pPlaybackDeltaSixup.get(), false, [] {});
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CDemoGameStateListener Listener(*pPlayer, State);
	pPlayer->SetListener(&Listener);
	ASSERT_EQ(pPlayer->Load(pStorage.get(), pConsole.get(), pFilename, IStorage::TYPE_SAVE), 0);
	pPlayer->Play();
	ASSERT_TRUE(pPlayer->IsPlaying()) << pPlayer->ErrorMessage();
	ASSERT_TRUE(pPlayer->SetPos(300)) << pPlayer->ErrorMessage();
	ASSERT_GT(Listener.NumSnapshots(), 0);

	EXPECT_EQ(State.SnapshotTick(), 300);
	EXPECT_EQ(State.LocalClientId(), 2);
	EXPECT_TRUE(State.Client(2).m_HasCharacter);
	EXPECT_EQ(State.Client(2).m_Character.m_X, 200);
	EXPECT_EQ(State.Client(2).m_Character.m_Y, 320);
	pPlayer->Stop();
	EXPECT_TRUE(pStorage->RemoveFile(pFilename, IStorage::TYPE_SAVE));
}

TEST(SessionState, GameStatesFollowConnections)
{
	const auto pNetwork = std::make_unique<CGameSessionContext>(CSessionId(1), NUM_DUMMIES);
	const auto pDemo = std::make_unique<CGameSessionContext>(CSessionId(2), 1);
	EXPECT_EQ(pNetwork->GameStates().size(), 2U);
	EXPECT_EQ(pNetwork->GameState(1).m_Conn, 1);
	EXPECT_EQ(pDemo->GameStates().size(), 1U);
	EXPECT_EQ(pDemo->GameState(0).m_Conn, 0);

	pNetwork->m_Stats.Client(4).m_Frags = 3;
	pNetwork->GameState(0).Reset();
	EXPECT_EQ(pNetwork->m_Stats.Client(4).m_Frags, 3);
}

TEST(SessionState, MapMetadataIgnoresMissingRecordsAndTruncates)
{
	CSessionMapMetadataState Metadata;
	EXPECT_EQ(Metadata.BestTimeSeconds(), FinishTime::UNSET);
	Metadata.ApplyRecordBestTime(12345);
	Metadata.ApplyRecordBestTime(0);
	Metadata.ApplyRecordBestTime(-1);
	EXPECT_EQ(Metadata.BestTimeSeconds(), 123);
	EXPECT_EQ(Metadata.BestTimeMillis(), 450);

	std::string LongDescription(CSessionMapMetadataState::MAX_DESCRIPTION_LENGTH - 1, 'd');
	LongDescription += "\xC3\xA4";
	Metadata.SetDescription(LongDescription.c_str());
	EXPECT_EQ(str_length(Metadata.Description()), CSessionMapMetadataState::MAX_DESCRIPTION_LENGTH - 1);

	Metadata.Reset();
	EXPECT_EQ(Metadata.BestTimeSeconds(), FinishTime::UNSET);
	EXPECT_STREQ(Metadata.Description(), "");
}

TEST(SessionState, InfoMessagesKeepTheNewest)
{
	CSessionInfoMessageState InfoMessages;
	for(int Tick = 10; Tick <= 60; Tick += 10)
	{
		CSessionInfoMessageState::CMessage Message;
		Message.m_Tick = Tick;
		InfoMessages.Add(Message);
	}
	ASSERT_EQ(InfoMessages.Count(), CSessionInfoMessageState::MAX_MESSAGES);
	for(int Index = 0; Index < InfoMessages.Count(); ++Index)
	{
		EXPECT_EQ(InfoMessages.Message(Index).m_Tick, 20 + Index * 10);
		EXPECT_EQ(InfoMessages.Message(Index).m_Id, static_cast<uint64_t>(Index + 2));
	}
	InfoMessages.Reset();
	EXPECT_EQ(InfoMessages.Count(), 0);
}

TEST(SessionState, ChatKeepsTheNewestAndQueuesMessages)
{
	const auto pChat = std::make_unique<CSessionChatState>();
	CSessionChatState &Chat = *pChat;
	for(int Index = 0; Index <= CSessionChatState::MAX_LINES; ++Index)
	{
		CSessionChatState::CLine Line;
		Line.m_Time = 100 + Index;
		Line.m_ClientId = Index % MAX_CLIENTS;
		str_format(Line.m_aText, sizeof(Line.m_aText), "line-%d", Index);
		Chat.Add(Line);
	}
	CSessionChatState::CLine Repeated;
	Repeated.m_Time = 999;
	Repeated.m_ClientId = CSessionChatState::MAX_LINES % MAX_CLIENTS;
	str_format(Repeated.m_aText, sizeof(Repeated.m_aText), "line-%d", CSessionChatState::MAX_LINES);
	const CSessionChatState::CLine &StoredRepeated = Chat.Add(Repeated);

	ASSERT_EQ(Chat.Count(), CSessionChatState::MAX_LINES);
	EXPECT_EQ(Chat.Line(0).m_Id, 2);
	EXPECT_STREQ(Chat.Line(0).m_aText, "line-1");
	EXPECT_EQ(StoredRepeated.m_Id, CSessionChatState::MAX_LINES + 1);
	EXPECT_EQ(StoredRepeated.m_Revision, 2);
	EXPECT_EQ(StoredRepeated.m_TimesRepeated, 1);
	EXPECT_EQ(StoredRepeated.m_Time, 999);

	Chat.BeginCommandInfo();
	Chat.RegisterCommand("save", "?r[code]", "Save the team");
	Chat.RegisterCommand("load", "r[code]", "Load the team");
	Chat.RegisterCommand("save", "", "duplicate");
	const auto &Commands = Chat.SortedCommands();
	ASSERT_EQ(Commands.size(), 2);
	EXPECT_EQ(Commands[0].m_Name, "load");
	EXPECT_EQ(Commands[1].m_Name, "save");
	Chat.UnregisterCommand("load");
	EXPECT_EQ(Chat.Commands().size(), 1);

	EXPECT_TRUE(Chat.Enqueue(0, 0, "first"));
	EXPECT_TRUE(Chat.Enqueue(1, 1, "second"));
	EXPECT_TRUE(Chat.Enqueue(0, 0, "third"));
	EXPECT_FALSE(Chat.Enqueue(1, 1, "overflow"));
	EXPECT_EQ(Chat.Pending().m_Conn, 0);
	EXPECT_EQ(Chat.Pending().m_Text, "first");
	Chat.PopPending();
	EXPECT_EQ(Chat.Pending().m_Conn, 1);
	EXPECT_EQ(Chat.Pending().m_Text, "second");

	Chat.Reset();
	EXPECT_EQ(Chat.Count(), 0);
	EXPECT_EQ(Chat.PendingCount(), 0);
	EXPECT_TRUE(Chat.Commands().empty());
}

TEST(SessionState, StatsFollowTheSnapshotLifecycle)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CSessionStatsState SessionStats;
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 10;
	auto pClients = std::make_unique<CClients>();
	(*pClients)[5].m_HasPlayerInfo = true;
	(*pClients)[5].m_PlayerInfo.m_ClientId = 5;
	(*pClients)[5].m_PlayerInfo.m_Team = TEAM_RED;
	State.ApplySnapshotData(100, *pClients, &GameInfo);
	SessionStats.UpdateSnapshot(State, 100);
	CSessionClientStats &Stats = SessionStats.Client(5);
	EXPECT_TRUE(Stats.IsActive());
	Stats.m_Frags = 2;

	(*pClients)[5].m_PlayerInfo.m_Team = TEAM_SPECTATORS;
	State.ApplySnapshotData(150, *pClients, &GameInfo);
	SessionStats.UpdateSnapshot(State, 150);
	EXPECT_FALSE(Stats.IsActive());
	(*pClients)[5].m_PlayerInfo.m_Team = TEAM_RED;
	State.ApplySnapshotData(160, *pClients, &GameInfo);
	SessionStats.UpdateSnapshot(State, 160);
	EXPECT_EQ(Stats.GetIngameTicks(160), 50);
	EXPECT_EQ(Stats.GetFPM(160, 50), 120.0f);

	GameInfo.m_RoundStartTick = 20;
	State.ApplySnapshotData(200, *pClients, &GameInfo);
	SessionStats.UpdateSnapshot(State, 200);
	EXPECT_FALSE(Stats.IsActive());
	EXPECT_EQ(Stats.m_Frags, 0);
}

TEST(SessionState, VotesKeepOptionsAcrossVotes)
{
	CSessionVoteState Vote;
	Vote.AddOption("alpha");
	Vote.AddOption("duplicate");
	Vote.AddOption("duplicate");
	Vote.RemoveOption("duplicate");
	ASSERT_EQ(Vote.NumOptions(), 2);
	EXPECT_STREQ(Vote.Option(0)->c_str(), "alpha");
	EXPECT_STREQ(Vote.Option(1)->c_str(), "duplicate");
	EXPECT_EQ(Vote.Option(-1), nullptr);
	EXPECT_EQ(Vote.Option(2), nullptr);

	const std::string LongDescription(VOTE_DESC_LENGTH + 10, 'd');
	const std::string LongReason(VOTE_REASON_LENGTH + 10, 'r');
	EXPECT_TRUE(Vote.ApplyVoteSet(10, LongDescription.c_str(), LongReason.c_str(), 1000, 100));
	EXPECT_EQ(Vote.CloseTime(), 2000);
	EXPECT_EQ(Vote.SecondsLeft(2099, 100), 0);
	EXPECT_EQ(Vote.SecondsLeft(2100, 100), -1);
	EXPECT_EQ(str_length(Vote.Description()), VOTE_DESC_LENGTH - 1);
	EXPECT_EQ(str_length(Vote.Reason()), VOTE_REASON_LENGTH - 1);
	Vote.Expire(2100, 100);
	EXPECT_FALSE(Vote.IsVoting());

	std::string Utf8Description(VOTE_DESC_LENGTH - 2, 'd');
	Utf8Description += "\xC3\xA4";
	EXPECT_TRUE(Vote.ApplyVoteSet(10, Utf8Description.c_str(), "", 1000, 100));
	EXPECT_EQ(str_length(Vote.Description()), VOTE_DESC_LENGTH - 2);
	Vote.SetReceivingOptions(true);
	EXPECT_FALSE(Vote.ApplyVoteSet(0, "", "", 3000, 100));
	EXPECT_FALSE(Vote.IsVoting());
	EXPECT_FALSE(Vote.IsReceivingOptions());
	EXPECT_EQ(Vote.NumOptions(), 2);

	Vote.Reset();
	EXPECT_EQ(Vote.NumOptions(), 0);
	for(int i = 0; i < MAX_VOTE_OPTIONS + 1; ++i)
		Vote.AddOption("bounded");
	EXPECT_EQ(Vote.NumOptions(), MAX_VOTE_OPTIONS);
}

TEST(GameView, PresentationContextCombinesVisibleWorldRects)
{
	const auto pSession = std::make_unique<CGameSessionContext>(CSessionId(4), 1);
	CGameState &State = pSession->GameState(0);
	auto pClients = std::make_unique<CClients>();
	(*pClients)[1].m_HasPlayerInfo = true;
	(*pClients)[1].m_PlayerInfo.m_Local = 1;
	State.SetTeam(1, 1);
	State.SetTeam(2, 2);
	State.ApplySnapshotData(50, *pClients);

	const std::array aVisibleWorldRects = {
		CVisibleWorldRect(vec2(0.0f, 0.0f), vec2(100.0f, 100.0f)),
		CVisibleWorldRect(vec2(200.0f, 200.0f), vec2(300.0f, 300.0f)),
	};
	CPresentationContext Context(*pSession, State, CGameTickInfo(), aVisibleWorldRects, EPresentationPlayback::PLAYING, EPresentationAudio::MUTED);
	EXPECT_TRUE(Context.IsVisible(vec2(50.0f, 50.0f), vec2(0.0f, 0.0f)));
	EXPECT_TRUE(Context.IsVisible(vec2(250.0f, 250.0f), vec2(0.0f, 0.0f)));
	EXPECT_FALSE(Context.IsVisible(vec2(150.0f, 150.0f), vec2(0.0f, 0.0f)));
	EXPECT_TRUE(Context.IsVisible(vec2(150.0f, 150.0f), vec2(50.0f, 50.0f)));
	EXPECT_FALSE(Context.IsOtherTeamFromLocalPlayer(1));
	EXPECT_TRUE(Context.IsOtherTeamFromLocalPlayer(2));
}

TEST(GameView, RetargetingResetsWhatBelongsToTheOldTarget)
{
	CGameView View;
	View.SetTarget(CSessionId(1), 0);
	View.m_EmoticonSelector.m_Active = true;
	View.m_EmoticonSelector.m_SelectedEmote = 3;
	View.m_SpectatorSelector.m_Active = true;
	View.m_SpectatorSelector.m_PendingSpectatorId = 8;
	View.m_SpectatorCursor.m_Available = true;
	View.m_SpectatorCursor.m_CursorOwnerId = 5;
	View.m_MultiView.m_Active = true;
	View.m_MultiView.m_aSelected[7] = true;

	View.SetTarget(CSessionId(1), 0);
	EXPECT_TRUE(View.m_SpectatorCursor.IsAvailable());
	EXPECT_TRUE(View.m_SpectatorSelector.m_Active);

	View.SetTarget(CSessionId(1), 1);
	EXPECT_EQ(View.Binding(), (CViewBinding{&View, CSessionId(1), 1}));
	EXPECT_TRUE(View.m_EmoticonSelector.m_Active);
	EXPECT_FALSE(View.m_SpectatorSelector.m_Active);
	EXPECT_EQ(View.m_SpectatorSelector.m_PendingSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_FALSE(View.m_SpectatorCursor.IsAvailable());
	EXPECT_EQ(View.m_SpectatorCursor.m_CursorOwnerId, -1);
	EXPECT_TRUE(View.m_MultiView.m_Active);

	View.SetTarget(CSessionId(2), 1);
	EXPECT_FALSE(View.m_MultiView.m_Active);
	EXPECT_FALSE(View.m_MultiView.m_aSelected[7]);
}

TEST(GameView, SpectatorSelection)
{
	CGameView::CSpectatorSelectorState Selector;
	std::array<int, MAX_CLIENTS> aClients;
	aClients.fill(-1);
	for(int Index = 0; Index < 9; ++Index)
		aClients[Index] = Index + 10;

	Selector.m_SelectorMouse = vec2(-250.0f, -250.0f);
	Selector.UpdateSelection(300.0f, 60.0f, 8, aClients, 9, true);
	EXPECT_EQ(Selector.m_SelectedSpectatorId, SPEC_FREEVIEW);
	Selector.m_SelectorMouse = vec2(0.0f, -250.0f);
	Selector.UpdateSelection(300.0f, 60.0f, 8, aClients, 9, true);
	EXPECT_EQ(Selector.m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::MULTI_VIEW);
	Selector.m_SelectorMouse = vec2(220.0f, -250.0f);
	Selector.UpdateSelection(300.0f, 60.0f, 8, aClients, 9, true);
	EXPECT_EQ(Selector.m_SelectedSpectatorId, SPEC_FOLLOW);
	Selector.UpdateSelection(300.0f, 60.0f, 8, aClients, 9, false);
	EXPECT_EQ(Selector.m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	Selector.m_SelectorMouse = vec2(-260.0f, -190.0f);
	Selector.UpdateSelection(300.0f, 60.0f, 8, aClients, 9, false);
	EXPECT_EQ(Selector.m_SelectedSpectatorId, 10);
	Selector.m_SelectorMouse = vec2(30.0f, -190.0f);
	Selector.UpdateSelection(300.0f, 60.0f, 8, aClients, 9, false);
	EXPECT_EQ(Selector.m_SelectedSpectatorId, 18);
}

TEST(GameView, EmoticonSelection)
{
	CGameView::CEmoticonSelectorState Selector;
	Selector.m_SelectorMouse = vec2(200.0f, 0.0f);
	Selector.UpdateSelection(16, 6, true);
	EXPECT_FLOAT_EQ(length(Selector.m_SelectorMouse), 170.0f);
	EXPECT_EQ(Selector.m_SelectedEmote, 0);
	EXPECT_EQ(Selector.m_SelectedEyeEmote, -1);

	Selector.m_SelectorMouse = vec2(70.0f, 0.0f);
	Selector.UpdateSelection(16, 6, true);
	EXPECT_EQ(Selector.m_SelectedEmote, -1);
	EXPECT_EQ(Selector.m_SelectedEyeEmote, 0);

	Selector.UpdateSelection(16, 6, false);
	EXPECT_EQ(Selector.m_SelectedEmote, -1);
	EXPECT_EQ(Selector.m_SelectedEyeEmote, -1);
}

TEST(GameView, MotdVisibilityIsRevisionBound)
{
	CGameView::CMotdPresentationState Motd;
	Motd.Show(CSessionId(1), 4, 100);
	EXPECT_TRUE(Motd.IsActive(CSessionId(1), 4, 99));
	EXPECT_FALSE(Motd.IsActive(CSessionId(1), 4, 100));
	EXPECT_FALSE(Motd.IsActive(CSessionId(1), 5, 99));
	EXPECT_FALSE(Motd.IsActive(CSessionId(2), 4, 99));
	Motd.Dismiss();
	EXPECT_FALSE(Motd.IsActive(CSessionId(1), 4, 99));
}

namespace
{
	// the local player, client 5, joins a round of deathmatch that started at tick 100
	std::unique_ptr<CClients> LocalPlayerSnapshot()
	{
		auto pClients = std::make_unique<CClients>();
		CGameState::CClientSnapshot &Local = (*pClients)[5];
		Local.m_Active = true;
		Local.m_HasPlayerInfo = true;
		Local.m_PlayerInfo.m_ClientId = 5;
		Local.m_PlayerInfo.m_Local = 1;
		Local.m_PlayerInfo.m_Team = TEAM_RED;
		Local.m_PlayerInfo.m_Score = 10;
		Local.m_HasClientInfo = true;
		StrToInts(Local.m_ClientInfo.m_aName, std::size(Local.m_ClientInfo.m_aName), "local");
		StrToInts(Local.m_ClientInfo.m_aClan, std::size(Local.m_ClientInfo.m_aClan), "clan");
		return pClients;
	}

	CObservedMatchMetadata ObservedMetadata(EMatchTermination Termination = EMatchTermination::COMPLETED)
	{
		CObservedMatchMetadata Metadata;
		Metadata.m_OriginId = "127.0.0.1:8303";
		Metadata.m_ModeId = "dm";
		Metadata.m_MapName = "dm1";
		Metadata.m_MapSha256 = sha256("map", 3);
		Metadata.m_EndTimeUtc = 2000000;
		Metadata.m_TickRate = 50;
		Metadata.m_Termination = Termination;
		return Metadata;
	}

	CMatchReport ServerReport(int RoundStartTick)
	{
		CMatchReport Report;
		Report.m_MatchId = CalculateUuid("server-report");
		Report.m_ModeId = "vanilla.dm";
		Report.m_MapName = "dm1";
		Report.m_MapSha256 = sha256("map", 3);
		Report.m_StartTimeUtc = 100;
		Report.m_EndTimeUtc = 110;
		Report.m_DurationTicks = 500;
		Report.m_TickRate = 50;
		Report.m_RoundStartTick = RoundStartTick;
		Report.m_vParticipants.push_back({7, std::nullopt, "Tee", "", 0, std::nullopt});
		Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, 7, 1, EMatchOutcome::WIN});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 7, "kills", 3, EMatchMetricAggregation::SUM});
		return Report;
	}

	std::string PackedReport(const CMatchReport &Report)
	{
		std::string Packed;
		std::string Error;
		EXPECT_TRUE(MatchReportToPacked(Report, Packed, &Error)) << Error;
		return Packed;
	}
}

TEST(SessionState, ObservedMatchIsFinalizedOnce)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CSessionStatsState Stats;
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 100;
	auto pClients = LocalPlayerSnapshot();
	State.ApplySnapshotData(150, *pClients, &GameInfo);
	EXPECT_FALSE(Stats.UpdateSnapshot(State, 150));
	Stats.Client(5).m_Frags = 3;
	EXPECT_FALSE(Stats.FinalizeObservedMatch(ObservedMetadata(), State, 150));

	GameInfo.m_GameStateFlags = GAMESTATEFLAG_GAMEOVER;
	State.ApplySnapshotData(200, *pClients, &GameInfo);
	EXPECT_TRUE(Stats.UpdateSnapshot(State, 200));
	ASSERT_TRUE(Stats.FinalizeObservedMatch(ObservedMetadata(), State, 200));
	EXPECT_FALSE(Stats.FinalizeObservedMatch(ObservedMetadata(), State, 200));
	ASSERT_TRUE(Stats.LatestMatch().has_value());
	const CStoredMatch &Stored = *Stats.LatestMatch();
	EXPECT_EQ(Stored.m_Source, EMatchReportSource::CLIENT_OBSERVED);
	EXPECT_EQ(Stored.m_Completeness, EMatchCompleteness::PARTIAL_SINCE_JOIN);
	EXPECT_EQ(Stored.m_OriginId, "127.0.0.1:8303");
	ASSERT_EQ(Stored.m_Report.m_vParticipants.size(), 1u);
	EXPECT_EQ(Stored.m_Report.m_vParticipants[0].m_DisplayName, "local");
	EXPECT_EQ(Stored.m_Report.m_vParticipants[0].m_JoinedTick, 50);
	EXPECT_EQ(Stored.m_Report.Metric(EMatchSubjectKind::PARTICIPANT, 0, "kills"), 3);
	EXPECT_EQ(Stored.m_Report.Metric(EMatchSubjectKind::PARTICIPANT, 0, "score"), 10);
	// nothing happened with a flag, so there is no flag metric
	EXPECT_FALSE(Stored.m_Report.Metric(EMatchSubjectKind::PARTICIPANT, 0, "flag_captures").has_value());
	ASSERT_NE(Stored.m_Report.Standing(EMatchSubjectKind::PARTICIPANT, 0), nullptr);
	EXPECT_EQ(Stored.m_Report.Standing(EMatchSubjectKind::PARTICIPANT, 0)->m_Outcome, EMatchOutcome::WIN);
	std::string Error;
	EXPECT_TRUE(MatchReportValidate(Stored.m_Report, &Error)) << Error;
}

TEST(SessionState, ObservedMatchAbortedWithoutGameOver)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CSessionStatsState Stats;
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 100;
	auto pClients = LocalPlayerSnapshot();
	State.ApplySnapshotData(150, *pClients, &GameInfo);
	Stats.UpdateSnapshot(State, 150);

	ASSERT_TRUE(Stats.FinalizeObservedMatch(ObservedMetadata(EMatchTermination::ABORTED), State, 175));
	ASSERT_TRUE(Stats.LatestMatch().has_value());
	EXPECT_EQ(Stats.LatestMatch()->m_Completeness, EMatchCompleteness::ABORTED);
	EXPECT_EQ(Stats.LatestMatch()->m_Report.m_Termination, EMatchTermination::ABORTED);
	ASSERT_EQ(Stats.LatestMatch()->m_Report.m_vStandings.size(), 1u);
	EXPECT_EQ(Stats.LatestMatch()->m_Report.m_vStandings[0].m_Outcome, EMatchOutcome::DNF);
}

TEST(SessionState, ServerReportReplacesTheObservedRound)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CSessionStatsState Stats;
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 100;
	auto pClients = LocalPlayerSnapshot();
	State.ApplySnapshotData(150, *pClients, &GameInfo);
	Stats.UpdateSnapshot(State, 150);
	GameInfo.m_GameStateFlags = GAMESTATEFLAG_GAMEOVER;
	State.ApplySnapshotData(200, *pClients, &GameInfo);
	Stats.UpdateSnapshot(State, 200);
	ASSERT_TRUE(Stats.FinalizeObservedMatch(ObservedMetadata(), State, 200));

	// the report of the round arrives after the next one started
	GameInfo.m_GameStateFlags = 0;
	GameInfo.m_RoundStartTick = 300;
	State.ApplySnapshotData(300, *pClients, &GameInfo);
	Stats.UpdateSnapshot(State, 300);
	EXPECT_FALSE(Stats.LatestMatch().has_value());

	CStoredMatch Server;
	Server.m_OriginId = "127.0.0.1:8303";
	Server.m_Source = EMatchReportSource::SERVER_REPORT;
	Server.m_Report = ServerReport(100);
	const CStoredMatch *pObserved = Stats.ObservedMatchReplacedBy(Server);
	ASSERT_NE(pObserved, nullptr);
	EXPECT_EQ(pObserved->m_Source, EMatchReportSource::CLIENT_OBSERVED);
	Server.m_Report.m_RoundStartTick = 101;
	EXPECT_EQ(Stats.ObservedMatchReplacedBy(Server), nullptr);
	Server.m_Report.m_RoundStartTick = 100;
	Server.m_OriginId = "127.0.0.1:8304";
	EXPECT_EQ(Stats.ObservedMatchReplacedBy(Server), nullptr);
	Stats.ClearPreviousObservedMatch();
	Server.m_OriginId = "127.0.0.1:8303";
	EXPECT_EQ(Stats.ObservedMatchReplacedBy(Server), nullptr);
}

TEST(SessionState, LiveStatsArePersistedOnlyForTheRunningRound)
{
	const auto pState = std::make_unique<CGameState>();
	CGameState &State = *pState;
	CSessionStatsState Stats;
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 100;
	auto pClients = LocalPlayerSnapshot();
	State.ApplySnapshotData(150, *pClients, &GameInfo);
	Stats.UpdateSnapshot(State, 150);

	CStoredMatch Live;
	Live.m_Source = EMatchReportSource::SERVER_SNAPSHOT;
	Live.m_Report = ServerReport(100);
	Stats.SetLiveStats(Live, false);
	EXPECT_TRUE(Stats.LiveStats().has_value());
	EXPECT_EQ(Stats.LiveStatsToPersist(), nullptr);
	Stats.SetLiveStats(Live, true);
	EXPECT_NE(Stats.LiveStatsToPersist(), nullptr);

	// the final report says everything the live statistics said
	CStoredMatch Final = Live;
	Final.m_Source = EMatchReportSource::SERVER_REPORT;
	ASSERT_TRUE(Stats.IsCurrentServerMatch(Final.m_Report));
	Stats.SetLatestServerMatch(Final);
	EXPECT_FALSE(Stats.LiveStats().has_value());
	EXPECT_EQ(Stats.LiveStatsToPersist(), nullptr);
	EXPECT_FALSE(Stats.IsCurrentServerMatch(Final.m_Report));

	Stats.SetLiveStats(Live, true);
	GameInfo.m_RoundStartTick = 300;
	State.ApplySnapshotData(300, *pClients, &GameInfo);
	Stats.UpdateSnapshot(State, 300);
	EXPECT_FALSE(Stats.LiveStats().has_value());
}

TEST(MatchReportAssembler, AssemblesChunksInOrder)
{
	const CMatchReport Report = ServerReport(100);
	const std::string Payload = PackedReport(Report);
	const int Split = Payload.size() / 2;
	CMatchReportAssembler Assembler;
	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, true, 7, Payload.size()));
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Split));
	EXPECT_FALSE(Assembler.IsComplete());
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 1, Payload.data() + Split, Payload.size() - Split));
	EXPECT_TRUE(Assembler.IsComplete());
	EXPECT_FALSE(Assembler.IsLive());
	EXPECT_TRUE(Assembler.PersistOnDisconnect());
	CStoredMatch Match;
	std::string Error;
	ASSERT_TRUE(Assembler.Finish(Match, &Error)) << Error;
	EXPECT_FALSE(Assembler.IsComplete());
	EXPECT_EQ(Match.m_Source, EMatchReportSource::SERVER_REPORT);
	EXPECT_EQ(Match.m_Completeness, EMatchCompleteness::COMPLETE);
	EXPECT_EQ(Match.m_LocalParticipantId, 7);
	EXPECT_EQ(Match.m_Report.m_MatchId, Report.m_MatchId);
	EXPECT_EQ(Match.m_Report.Metric(EMatchSubjectKind::PARTICIPANT, 7, "kills"), 3);

	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, true, false, 7, Payload.size()));
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Payload.size()));
	ASSERT_TRUE(Assembler.Finish(Match, &Error)) << Error;
	EXPECT_EQ(Match.m_Source, EMatchReportSource::SERVER_SNAPSHOT);
	EXPECT_EQ(Match.m_Completeness, EMatchCompleteness::ABORTED);

	CMatchReport Restarted = Report;
	Restarted.m_Termination = EMatchTermination::ADMIN_ENDED;
	const std::string RestartedPayload = PackedReport(Restarted);
	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 7, RestartedPayload.size()));
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 0, RestartedPayload.data(), RestartedPayload.size()));
	ASSERT_TRUE(Assembler.Finish(Match, &Error)) << Error;
	EXPECT_EQ(Match.m_Source, EMatchReportSource::SERVER_REPORT);
	EXPECT_EQ(Match.m_Completeness, EMatchCompleteness::ABORTED);
}

TEST(MatchReportAssembler, RejectsWhatDoesNotFitTheAnnouncement)
{
	const CMatchReport Report = ServerReport(100);
	const std::string Payload = PackedReport(Report);
	const int Size = Payload.size();
	CMatchReportAssembler Assembler;
	CStoredMatch Match;
	std::string Error;

	EXPECT_FALSE(Assembler.Start(Report.m_MatchId, false, false, 7, MatchReportLimits::MAX_PAYLOAD_SIZE + 1));
	EXPECT_FALSE(Assembler.Start(Report.m_MatchId, false, false, 7, 0));
	EXPECT_FALSE(Assembler.Start(UUID_ZEROED, false, false, 7, Size));
	EXPECT_FALSE(Assembler.Start(Report.m_MatchId, false, false, -1, Size));
	EXPECT_FALSE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Size));

	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 7, Size));
	EXPECT_FALSE(Assembler.AddChunk(Report.m_MatchId, 1, Payload.data(), Size));
	EXPECT_FALSE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Size));

	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 7, Size - 1));
	EXPECT_FALSE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Size));

	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 7, Size));
	EXPECT_FALSE(Assembler.AddChunk(CalculateUuid("other"), 0, Payload.data(), Size));

	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 7, Size));
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Size / 2));
	EXPECT_FALSE(Assembler.Finish(Match, &Error));

	// the report names somebody else, or another match
	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 8, Size));
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 0, Payload.data(), Size));
	EXPECT_FALSE(Assembler.Finish(Match, &Error));
	ASSERT_TRUE(Assembler.Start(CalculateUuid("other"), false, false, 7, Size));
	ASSERT_TRUE(Assembler.AddChunk(CalculateUuid("other"), 0, Payload.data(), Size));
	EXPECT_FALSE(Assembler.Finish(Match, &Error));

	const std::string Garbage(Size, 'x');
	ASSERT_TRUE(Assembler.Start(Report.m_MatchId, false, false, 7, Size));
	ASSERT_TRUE(Assembler.AddChunk(Report.m_MatchId, 0, Garbage.data(), Size));
	EXPECT_FALSE(Assembler.Finish(Match, &Error));
}
