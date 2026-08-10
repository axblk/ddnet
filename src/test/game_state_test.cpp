#include "test.h"

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/demo.h>
#include <engine/shared/network.h>
#include <engine/shared/snapshot.h>
#include <engine/storage.h>

#include <game/client/game_state.h>
#include <game/client/game_view.h>
#include <game/client/map_context.h>

#include <gtest/gtest.h>

namespace
{
	class CTestRenderOutput : public CRenderOutput
	{
	public:
		struct CCharacterCall
		{
			int m_ClientId;
			vec2 m_Position;
			bool m_Local;
		};

		std::vector<CViewport> m_vViewports;
		std::vector<CCharacterCall> m_vCharacters;
		int m_EndedViews = 0;

		void BeginView(const CViewport &Viewport, vec2 CameraPosition, float Zoom) override
		{
			m_vViewports.push_back(Viewport);
		}

		void DrawCharacter(int ClientId, vec2 Position, bool Local) override
		{
			m_vCharacters.push_back({ClientId, Position, Local});
		}

		void EndView() override
		{
			m_EndedViews++;
		}
	};

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
			if(!pSnapshot->IsValid(Size))
				return;
			std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
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
			m_State.ApplySnapshotData(m_Player.Info()->m_Info.m_CurrentTick, pSnapshot->NumItems(), std::move(aClients));
		}

		void OnDemoPlayerMessage(void *pData, int Size) override {}

		int NumSnapshots() const { return m_NumSnapshots; }
	};
}

TEST(GameState, StreamMetadataAndTuningAreIndependent)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	CTuningParams PrimaryTuning = CTuningParams::DEFAULT;
	PrimaryTuning.Set("gravity", 0.25f);

	Primary.ApplySnapshotMetadata(100, 20, 3);
	Primary.ApplyTuning(PrimaryTuning);
	Primary.MarkPredicted(104);
	Additional.ApplySnapshotMetadata(200, 30, 7);
	Additional.MarkPredicted(205);

	EXPECT_EQ(Primary.LocalClientId(), 3);
	EXPECT_EQ(Primary.SnapshotTick(), 100);
	EXPECT_EQ(Primary.PredictionTick(), 104);
	EXPECT_EQ(Additional.LocalClientId(), 7);
	EXPECT_EQ(Additional.SnapshotTick(), 200);
	EXPECT_EQ(Additional.PredictionTick(), 205);
	EXPECT_NE(Primary.Id(), Additional.Id());
	EXPECT_FLOAT_EQ(Primary.Tuning().m_Gravity, 0.25f);
	EXPECT_FLOAT_EQ(Additional.Tuning().m_Gravity, CTuningParams::DEFAULT.m_Gravity);
}

TEST(GameState, DynamicManagerKeepsThreeStableStreams)
{
	CGameStateManager Manager;
	const CGameStateId FirstId = Manager.Create(CStreamId(10));
	const CGameStateId SecondId = Manager.Create(CStreamId(20));
	const CGameStateId ThirdId = Manager.Create(CStreamId(30));
	ASSERT_TRUE(FirstId.IsValid());
	ASSERT_TRUE(SecondId.IsValid());
	ASSERT_TRUE(ThirdId.IsValid());
	EXPECT_EQ(Manager.NumStates(), 3U);

	Manager.Find(FirstId)->ApplySnapshotMetadata(100, 1, 1);
	Manager.Find(SecondId)->ApplySnapshotMetadata(200, 2, 2);
	Manager.Find(ThirdId)->ApplySnapshotMetadata(300, 3, 3);
	CGameState *pThird = Manager.Find(ThirdId);
	EXPECT_TRUE(Manager.Destroy(SecondId));
	EXPECT_EQ(Manager.NumStates(), 2U);
	EXPECT_EQ(Manager.Find(ThirdId), pThird);
	EXPECT_EQ(Manager.FindByStream(CStreamId(10))->SnapshotTick(), 100);
	EXPECT_EQ(Manager.FindByStream(CStreamId(30))->SnapshotTick(), 300);
	EXPECT_EQ(Manager.FindByStream(CStreamId(20)), nullptr);
	EXPECT_FALSE(Manager.Create(CStreamId(10)).IsValid());
}

TEST(GameState, SnapshotWorldsAreIndependent)
{
	CMapContext MapContext;
	MapContext.Init();
	const std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_TRUE(MapContext.Map()->Load(pStorage.get(), "data/maps/ctf1.map", IStorage::TYPE_ALL));
	MapContext.Layers()->Init(MapContext.Map(), false, true);
	MapContext.Collision()->Init(MapContext.Layers());
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	Primary.InitPrediction(MapContext);
	Additional.InitPrediction(MapContext);

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aPrimaryClients = {};
	aPrimaryClients[3].m_Active = true;
	aPrimaryClients[3].m_HasPlayerInfo = true;
	aPrimaryClients[3].m_PlayerInfo.m_Local = 1;
	aPrimaryClients[3].m_HasCharacter = true;
	aPrimaryClients[3].m_Character.m_X = 320;
	aPrimaryClients[3].m_Character.m_Y = 640;

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aAdditionalClients = {};
	aAdditionalClients[7].m_Active = true;
	aAdditionalClients[7].m_HasPlayerInfo = true;
	aAdditionalClients[7].m_PlayerInfo.m_Local = 1;
	aAdditionalClients[7].m_HasCharacter = true;
	aAdditionalClients[7].m_Character.m_X = 960;
	aAdditionalClients[7].m_Character.m_Y = 1280;

	Primary.ApplySnapshotData(100, 2, std::move(aPrimaryClients));
	Additional.ApplySnapshotData(200, 2, std::move(aAdditionalClients));

	EXPECT_TRUE(Primary.HasGameWorldCharacter(3));
	EXPECT_FALSE(Primary.HasGameWorldCharacter(7));
	EXPECT_TRUE(Additional.HasGameWorldCharacter(7));
	EXPECT_FALSE(Additional.HasGameWorldCharacter(3));
	EXPECT_EQ(Primary.GameWorldCharacterCore(3).m_Pos, vec2(320.0f, 640.0f));
	EXPECT_EQ(Additional.GameWorldCharacterCore(7).m_Pos, vec2(960.0f, 1280.0f));
	EXPECT_EQ(Primary.GameWorld().GameTick(), 100);
	EXPECT_EQ(Additional.GameWorld().GameTick(), 200);
	const uint64_t PrimarySnapshotDigest = Primary.SnapshotDigest();
	const uint64_t AdditionalSnapshotDigest = Additional.SnapshotDigest();
	EXPECT_NE(PrimarySnapshotDigest, AdditionalSnapshotDigest);

	CNetObj_PlayerInput PrimaryInput = {};
	PrimaryInput.m_Direction = 1;
	PrimaryInput.m_TargetY = -1;
	CNetObj_PlayerInput AdditionalInput = {};
	AdditionalInput.m_Direction = -1;
	AdditionalInput.m_TargetY = -1;
	Primary.PredictTo(101, [&PrimaryInput](int) { return &PrimaryInput; });
	Additional.PredictTo(201, [&AdditionalInput](int) { return &AdditionalInput; });

	ASSERT_TRUE(Primary.PredictedClient(3).m_HasCurrent);
	ASSERT_TRUE(Additional.PredictedClient(7).m_HasCurrent);
	EXPECT_GT(Primary.PredictedClient(3).m_Current.m_Vel.x, 0.0f);
	EXPECT_LT(Additional.PredictedClient(7).m_Current.m_Vel.x, 0.0f);
	EXPECT_EQ(Primary.PredictionTick(), 101);
	EXPECT_EQ(Additional.PredictionTick(), 201);
	EXPECT_EQ(Primary.SnapshotDigest(), PrimarySnapshotDigest);
	EXPECT_EQ(Additional.SnapshotDigest(), AdditionalSnapshotDigest);
	EXPECT_NE(Primary.PredictionDigest(), Additional.PredictionDigest());
}

TEST(GameView, RenderingTwoViewsDoesNotAdvanceState)
{
	CGameState State(CGameStateId(7), CStreamId(1));
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	aClients[4].m_HasPlayerInfo = true;
	aClients[4].m_PlayerInfo.m_Local = 1;
	aClients[4].m_HasCharacter = true;
	aClients[4].m_Character.m_X = 100;
	aClients[4].m_Character.m_Y = 200;
	State.ApplySnapshotData(50, 2, std::move(aClients));

	CGameViewManager ViewManager;
	const CGameViewId LeftId = ViewManager.Create(State.Id());
	const CGameViewId RightId = ViewManager.Create(State.Id());
	ASSERT_TRUE(LeftId.IsValid());
	ASSERT_TRUE(RightId.IsValid());
	ASSERT_NE(LeftId, RightId);
	CGameView *pLeft = ViewManager.Find(LeftId);
	CGameView *pRight = ViewManager.Find(RightId);
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pRight, nullptr);
	pLeft->SetViewport({0, 0, 640, 720});
	pLeft->SetCameraPosition(vec2(100.0f, 200.0f));
	pRight->SetViewport({640, 0, 640, 720});
	pRight->SetCameraPosition(vec2(300.0f, 400.0f));
	pRight->SetZoom(2.0f);

	const uint64_t SnapshotDigest = State.SnapshotDigest();
	const uint64_t PredictionDigest = State.PredictionDigest();
	CTestRenderOutput Output;
	CGameStateRenderer Renderer;
	Renderer.Render({State, *pLeft}, Output);
	Renderer.Render({State, *pRight}, Output);

	ASSERT_EQ(Output.m_vViewports.size(), 2U);
	EXPECT_EQ(Output.m_vViewports[0], (CViewport{0, 0, 640, 720}));
	EXPECT_EQ(Output.m_vViewports[1], (CViewport{640, 0, 640, 720}));
	EXPECT_EQ(Output.m_vCharacters.size(), 2U);
	EXPECT_EQ(Output.m_EndedViews, 2);
	EXPECT_EQ(State.SnapshotDigest(), SnapshotDigest);
	EXPECT_EQ(State.PredictionDigest(), PredictionDigest);
	EXPECT_TRUE(ViewManager.Destroy(LeftId));
	EXPECT_EQ(ViewManager.NumViews(), 1U);
}

TEST(GameState, HeadlessSpectatorSnapshotHasNoDesktopDependencies)
{
	CGameState State(CGameStateId(1), CStreamId(1));
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	aClients[5].m_HasPlayerInfo = true;
	aClients[5].m_PlayerInfo.m_Local = 1;
	aClients[5].m_PlayerInfo.m_Team = TEAM_SPECTATORS;
	State.ApplySnapshotData(80, 2, std::move(aClients));
	const uint64_t BeforeSpectatorInfo = State.SnapshotDigest();
	CNetObj_SpectatorInfo SpectatorInfo = {};
	SpectatorInfo.m_SpectatorId = 9;
	SpectatorInfo.m_X = 123;
	SpectatorInfo.m_Y = 456;
	State.ApplySpectatorInfo(SpectatorInfo);

	EXPECT_TRUE(State.HasSpectatorInfo());
	EXPECT_EQ(State.LocalClientId(), 5);
	EXPECT_EQ(State.Client(5).m_PlayerInfo.m_Team, TEAM_SPECTATORS);
	EXPECT_EQ(State.SpectatorInfo().m_SpectatorId, 9);
	EXPECT_NE(State.SnapshotDigest(), BeforeSpectatorInfo);
}

TEST(GameState, GeneratedDemoPlaysToKnownDigestHeadlessly)
{
	static constexpr const char *pFilename = "client-game-state-headless.demo";
	const std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	const std::unique_ptr<IConsole> pConsole = CreateConsole(CFGFLAG_CLIENT);
	CNetBase::Init();
	const auto pSnapshotDelta = std::make_unique<CSnapshotDelta>();
	const auto pRecorder = std::make_unique<CDemoRecorder>(pSnapshotDelta.get(), true);
	unsigned char DummyMapData = 0;
	const SHA256_DIGEST MapSha256 = {};
	ASSERT_EQ(pRecorder->Start(pStorage.get(), pConsole.get(), pFilename, "test", "headless", MapSha256, 0, "client", 0, &DummyMapData, nullptr, nullptr, nullptr), 0);

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
	CGameState State(CGameStateId(1), CStreamId(1));
	CDemoGameStateListener Listener(*pPlayer, State);
	pPlayer->SetListener(&Listener);
	ASSERT_EQ(pPlayer->Load(pStorage.get(), pConsole.get(), pFilename, IStorage::TYPE_SAVE), 0);
	pPlayer->Play();
	ASSERT_TRUE(pPlayer->IsPlaying()) << pPlayer->ErrorMessage();
	ASSERT_TRUE(pPlayer->SetPos(300)) << pPlayer->ErrorMessage();
	ASSERT_GT(Listener.NumSnapshots(), 0);

	EXPECT_EQ(State.SnapshotTick(), 300);
	EXPECT_EQ(State.LocalClientId(), 2);
	EXPECT_EQ(State.Client(2).m_Character.m_X, 200);
	CGameState Expected(CGameStateId(2), CStreamId(1));
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aExpectedClients = {};
	aExpectedClients[2].m_Active = true;
	aExpectedClients[2].m_HasPlayerInfo = true;
	aExpectedClients[2].m_PlayerInfo.m_Local = 1;
	aExpectedClients[2].m_PlayerInfo.m_ClientId = 2;
	aExpectedClients[2].m_HasCharacter = true;
	aExpectedClients[2].m_Character.m_X = 200;
	aExpectedClients[2].m_Character.m_Y = 320;
	Expected.ApplySnapshotData(300, 2, std::move(aExpectedClients));
	EXPECT_EQ(State.SnapshotDigest(), Expected.SnapshotDigest());
	pPlayer->Stop();
	EXPECT_TRUE(pStorage->RemoveFile(pFilename, IStorage::TYPE_SAVE));
}
