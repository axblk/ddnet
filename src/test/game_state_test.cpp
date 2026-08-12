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
#include <game/client/map_context.h>
#include <game/client/session_context.h>

#include <gtest/gtest.h>

#include <string>

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
		struct CSpectatorCharacterCall
		{
			int m_ClientId;
			vec2 m_Position;
			bool m_OtherTeam;
		};

		std::vector<CViewport> m_vViewports;
		std::vector<CCharacterCall> m_vCharacters;
		std::vector<CSpectatorCharacterCall> m_vSpectatorCharacters;
		int m_EndedViews = 0;

		void BeginView(const CViewport &Viewport, vec2 CameraPosition, float Zoom) override
		{
			m_vViewports.push_back(Viewport);
		}

		void DrawCharacter(int ClientId, vec2 Position, bool Local) override
		{
			m_vCharacters.push_back({ClientId, Position, Local});
		}

		void DrawSpectatorCharacter(int ClientId, vec2 Position, bool OtherTeam) override
		{
			m_vSpectatorCharacters.push_back({ClientId, Position, OtherTeam});
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

TEST(GameState, TeamsAreIndependentAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	Primary.SetTeam(5, 3);
	Primary.SetDDrace16(true);
	Additional.SetTeam(5, 7);

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aPrimaryClients = {};
	aPrimaryClients[5].m_Active = true;
	aPrimaryClients[5].m_HasExtendedCharacter = true;
	aPrimaryClients[5].m_ExtendedCharacter.m_Flags = CHARACTERFLAG_SOLO;
	Primary.ApplySnapshotData(10, 1, std::move(aPrimaryClients));

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aAdditionalClients = {};
	aAdditionalClients[5].m_Active = true;
	aAdditionalClients[5].m_HasExtendedCharacter = true;
	Additional.ApplySnapshotData(20, 1, std::move(aAdditionalClients));

	EXPECT_EQ(Primary.Teams().Team(5), 3);
	EXPECT_TRUE(Primary.Teams().GetSolo(5));
	EXPECT_TRUE(Primary.Teams().m_IsDDRace16);
	EXPECT_EQ(Additional.Teams().Team(5), 7);
	EXPECT_FALSE(Additional.Teams().GetSolo(5));
	EXPECT_FALSE(Additional.Teams().m_IsDDRace16);

	Primary.Reset();
	EXPECT_EQ(Primary.Teams().Team(5), TEAM_FLOCK);
	EXPECT_FALSE(Primary.Teams().GetSolo(5));
	EXPECT_FALSE(Primary.Teams().m_IsDDRace16);
	EXPECT_EQ(Additional.Teams().Team(5), 7);
	EXPECT_FALSE(Additional.Teams().GetSolo(5));
}

TEST(GameState, RenderedClientsUseStateOwnedSnapshotHistory)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	auto pPrimaryClients = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	CGameState::CClientSnapshot &PrimaryClient = (*pPrimaryClients)[4];
	PrimaryClient.m_Active = true;
	PrimaryClient.m_HasPlayerInfo = true;
	PrimaryClient.m_HasPrevPlayerInfo = true;
	PrimaryClient.m_HasCharacter = true;
	PrimaryClient.m_HasPrevCharacter = true;
	PrimaryClient.m_HasExtendedCharacter = true;
	PrimaryClient.m_HasPrevExtendedCharacter = true;
	PrimaryClient.m_PrevCharacter.m_X = 100;
	PrimaryClient.m_PrevCharacter.m_Y = 200;
	PrimaryClient.m_Character.m_X = 300;
	PrimaryClient.m_Character.m_Y = 600;
	PrimaryClient.m_PrevExtendedTargetX = -10;
	PrimaryClient.m_ExtendedCharacter.m_TargetX = 30;

	auto pAdditionalClients = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	CGameState::CClientSnapshot &AdditionalClient = (*pAdditionalClients)[4];
	AdditionalClient.m_Active = true;
	AdditionalClient.m_HasPlayerInfo = true;
	AdditionalClient.m_HasPrevPlayerInfo = true;
	AdditionalClient.m_HasCharacter = true;
	AdditionalClient.m_HasPrevCharacter = true;
	AdditionalClient.m_PrevCharacter.m_X = -400;
	AdditionalClient.m_PrevCharacter.m_Y = -800;
	AdditionalClient.m_Character.m_X = 400;
	AdditionalClient.m_Character.m_Y = 800;

	Primary.ApplySnapshotData(10, 4, std::move(*pPrimaryClients));
	Additional.ApplySnapshotData(20, 4, std::move(*pAdditionalClients));
	Primary.UpdateRenderedClient(4, false, false, 0.25f, 0.0f);
	Additional.UpdateRenderedClient(4, false, false, 0.75f, 0.0f);

	EXPECT_TRUE(Primary.RenderedClient(4).m_Active);
	EXPECT_EQ(Primary.RenderedClient(4).m_Position, vec2(150.0f, 300.0f));
	EXPECT_EQ(Additional.RenderedClient(4).m_Position, vec2(200.0f, 400.0f));
	EXPECT_TRUE(Primary.Client(4).m_HasPrevExtendedCharacter);
	EXPECT_EQ(Primary.Client(4).m_PrevExtendedTargetX, -10);
	EXPECT_FALSE(Additional.Client(4).m_HasPrevExtendedCharacter);

	Additional.RenderedClient(4).m_IsPredictedLocal = true;
	Primary.PredictionHistory(4).m_aSmoothLen[0] = 100;
	Primary.PredictionHistory(4).m_aPredPos[10] = vec2(10.0f, 20.0f);
	Additional.PredictionHistory(4).m_aSmoothLen[0] = 200;
	Additional.PredictionHistory(4).m_aPredPos[10] = vec2(30.0f, 40.0f);
	Primary.Reset();
	EXPECT_FALSE(Primary.RenderedClient(4).m_Active);
	EXPECT_FALSE(Primary.Client(4).m_HasPrevExtendedCharacter);
	EXPECT_EQ(Primary.PredictionHistory(4).m_aSmoothLen[0], 0);
	EXPECT_EQ(Primary.PredictionHistory(4).m_aPredPos[10], vec2(0.0f, 0.0f));
	EXPECT_TRUE(Additional.RenderedClient(4).m_Active);
	EXPECT_TRUE(Additional.RenderedClient(4).m_IsPredictedLocal);
	EXPECT_EQ(Additional.PredictionHistory(4).m_aSmoothLen[0], 200);
	EXPECT_EQ(Additional.PredictionHistory(4).m_aPredPos[10], vec2(30.0f, 40.0f));

	auto pIncompleteClients = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	(*pIncompleteClients)[4].m_HasPlayerInfo = true;
	(*pIncompleteClients)[4].m_HasCharacter = true;
	(*pIncompleteClients)[4].m_HasPrevCharacter = true;
	Primary.ApplySnapshotData(30, 2, std::move(*pIncompleteClients));
	Primary.UpdateRenderedClient(4, false, false, 0.5f, 0.0f);
	EXPECT_TRUE(Primary.RenderedClient(4).m_Active);
}

TEST(GameState, ClientIdentityAndEmoticonsAreIndependentAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	auto pPrimaryClients = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	auto pAdditionalClients = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	CGameState::CClientSnapshot &PrimaryClient = (*pPrimaryClients)[4];
	PrimaryClient.m_Active = true;
	PrimaryClient.m_HasPlayerInfo = true;
	PrimaryClient.m_HasClientInfo = true;
	StrToInts(PrimaryClient.m_ClientInfo.m_aName, std::size(PrimaryClient.m_ClientInfo.m_aName), "primary");
	StrToInts(PrimaryClient.m_ClientInfo.m_aClan, std::size(PrimaryClient.m_ClientInfo.m_aClan), "one");
	CGameState::CClientSnapshot &AdditionalClient = (*pAdditionalClients)[4];
	AdditionalClient.m_Active = true;
	AdditionalClient.m_HasPlayerInfo = true;
	AdditionalClient.m_HasClientInfo = true;
	StrToInts(AdditionalClient.m_ClientInfo.m_aName, std::size(AdditionalClient.m_ClientInfo.m_aName), "additional");
	StrToInts(AdditionalClient.m_ClientInfo.m_aClan, std::size(AdditionalClient.m_ClientInfo.m_aClan), "two");
	Primary.ApplySnapshotData(10, 2, std::move(*pPrimaryClients));
	Additional.ApplySnapshotData(20, 2, std::move(*pAdditionalClients));
	Primary.ApplyEmoticon(4, 3, 11, 0.25f);
	Additional.ApplyEmoticon(4, 7, 22, 0.75f);

	char aName[MAX_NAME_LENGTH];
	EXPECT_TRUE(IntsToStr(Primary.ClientIdentity(4).m_ClientInfo.m_aName, std::size(Primary.ClientIdentity(4).m_ClientInfo.m_aName), aName, std::size(aName)));
	EXPECT_STREQ(aName, "primary");
	EXPECT_TRUE(IntsToStr(Additional.ClientIdentity(4).m_ClientInfo.m_aName, std::size(Additional.ClientIdentity(4).m_ClientInfo.m_aName), aName, std::size(aName)));
	EXPECT_STREQ(aName, "additional");
	EXPECT_EQ(Primary.ClientEmoticon(4).m_Emoticon, 3);
	EXPECT_EQ(Primary.ClientEmoticon(4).m_StartTick, 11);
	EXPECT_FLOAT_EQ(Primary.ClientEmoticon(4).m_StartFraction, 0.25f);
	EXPECT_EQ(Additional.ClientEmoticon(4).m_Emoticon, 7);
	EXPECT_NE(Primary.SnapshotDigest(), Additional.SnapshotDigest());

	auto pPrimaryWithoutClientInfo = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	(*pPrimaryWithoutClientInfo)[4].m_Active = true;
	(*pPrimaryWithoutClientInfo)[4].m_HasPlayerInfo = true;
	Primary.ApplySnapshotData(12, 1, std::move(*pPrimaryWithoutClientInfo));
	EXPECT_TRUE(Primary.ClientIdentity(4).m_Active);
	EXPECT_TRUE(IntsToStr(Primary.ClientIdentity(4).m_ClientInfo.m_aName, std::size(Primary.ClientIdentity(4).m_ClientInfo.m_aName), aName, std::size(aName)));
	EXPECT_STREQ(aName, "primary");
	EXPECT_EQ(Primary.ClientEmoticon(4).m_StartTick, 11);

	auto pEmptyClients = std::make_unique<std::array<CGameState::CClientSnapshot, MAX_CLIENTS>>();
	Primary.ApplySnapshotData(13, 0, std::move(*pEmptyClients));
	EXPECT_FALSE(Primary.ClientIdentity(4).m_Active);
	EXPECT_EQ(Primary.ClientEmoticon(4).m_StartTick, -1);
	EXPECT_TRUE(Additional.ClientIdentity(4).m_Active);
	EXPECT_EQ(Additional.ClientEmoticon(4).m_StartTick, 22);

	Additional.Reset();
	EXPECT_FALSE(Additional.ClientIdentity(4).m_Active);
	EXPECT_EQ(Additional.ClientEmoticon(4).m_StartTick, -1);
}

TEST(GameState, CoreGameInfoIsIndependentAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	const uint64_t PrimaryDigestBefore = Primary.SnapshotDigest();
	CGameInfo PrimaryInfo;
	PrimaryInfo.m_Race = true;
	PrimaryInfo.m_PredictDDRace = true;
	PrimaryInfo.m_PredictDDRaceTiles = true;
	PrimaryInfo.m_BugDDRaceInput = true;
	PrimaryInfo.m_NoWeakHookAndBounce = true;
	PrimaryInfo.m_HudDDRace = true;
	CGameInfo AdditionalInfo;
	AdditionalInfo.m_Pvp = true;
	AdditionalInfo.m_PredictFNG = true;
	AdditionalInfo.m_PredictVanilla = true;
	AdditionalInfo.m_PredictEvents = true;
	AdditionalInfo.m_AllowHookColl = true;
	Primary.SetCoreGameInfo(PrimaryInfo);
	Additional.SetCoreGameInfo(AdditionalInfo);

	EXPECT_NE(Primary.SnapshotDigest(), PrimaryDigestBefore);
	EXPECT_TRUE(Primary.CoreGameInfo().m_Race);
	EXPECT_TRUE(Primary.CoreGameInfo().m_PredictDDRace);
	EXPECT_TRUE(Primary.CoreGameInfo().m_HudDDRace);
	EXPECT_FALSE(Primary.CoreGameInfo().m_PredictFNG);
	EXPECT_TRUE(Additional.CoreGameInfo().m_Pvp);
	EXPECT_TRUE(Additional.CoreGameInfo().m_PredictFNG);
	EXPECT_TRUE(Additional.CoreGameInfo().m_AllowHookColl);
	EXPECT_FALSE(Additional.CoreGameInfo().m_Race);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_IsDDRace);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_PredictDDRace);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_PredictTiles);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_UseTuneZones);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_BugDDRaceInput);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_NoWeakHookAndBounce);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_IsVanilla);
	EXPECT_TRUE(Additional.GameWorld().m_WorldConfig.m_IsFNG);
	EXPECT_TRUE(Additional.GameWorld().m_WorldConfig.m_IsVanilla);
	EXPECT_TRUE(Additional.GameWorld().m_WorldConfig.m_PredictEvents);
	EXPECT_FALSE(Additional.GameWorld().m_WorldConfig.m_IsDDRace);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_InfiniteAmmo);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_IsSolo);
	EXPECT_EQ(Primary.GameWorld().m_WorldConfig.m_PredictFreeze, 0);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_PredictWeapons);

	Primary.Reset();
	EXPECT_FALSE(Primary.CoreGameInfo().m_Race);
	EXPECT_FALSE(Primary.CoreGameInfo().m_PredictDDRace);
	EXPECT_FALSE(Primary.CoreGameInfo().m_HudDDRace);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_IsDDRace);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_PredictTiles);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_UseTuneZones);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_InfiniteAmmo);
	EXPECT_TRUE(Additional.CoreGameInfo().m_Pvp);
	EXPECT_TRUE(Additional.CoreGameInfo().m_PredictFNG);
	EXPECT_TRUE(Additional.GameWorld().m_WorldConfig.m_IsFNG);
	EXPECT_TRUE(Additional.GameWorld().m_WorldConfig.m_IsVanilla);
}

TEST(GameState, ComponentRuntimeIsOwnedAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	Primary.Input().m_MousePos = vec2(10.0f, 20.0f);
	Primary.Input().m_InputData.m_Fire = 3;
	Primary.Input().m_aAmmoCount[WEAPON_LASER] = 7;
	Primary.Runtime().m_ServerMode = CGameState::SERVERMODE_MOD;
	Primary.Runtime().m_CheckInfo = 50;
	Primary.Runtime().m_PlayerRecord = 12.34f;
	Primary.Runtime().m_LegacyPredictedTick = 123;
	Primary.Runtime().m_LastRoundStartTick = 100;
	Primary.Runtime().m_LastRaceTick = 90;
	Primary.Runtime().m_aStrongHookLastUpdateTick[4] = 123;
	Primary.Runtime().m_CharOrder.GiveStrong(7);
	Primary.Runtime().m_aFlagDropTick[TEAM_RED] = 80;
	Primary.Runtime().m_aLastPredictedPosition[4] = vec2(30.0f, 40.0f);
	Primary.Runtime().m_aLastPredictedActive[4] = true;
	Primary.Runtime().m_GameOver = true;
	Primary.Runtime().m_ReceivedDDNetPlayer = true;
	Primary.Runtime().m_ReceivedDDNetPlayerFinishTimes = true;
	Additional.Input().m_MousePos = vec2(-5.0f, 6.0f);
	Additional.Runtime().m_ServerMode = CGameState::SERVERMODE_PUREMOD;
	Additional.Runtime().m_ShowOthers = 2;
	Additional.Runtime().m_LastRaceTick = 200;
	Additional.Runtime().m_aStrongHookLastUpdateTick[4] = 456;
	Additional.Runtime().m_CharOrder.GiveWeak(7);

	EXPECT_EQ(Primary.Input().m_MousePos, vec2(10.0f, 20.0f));
	EXPECT_EQ(Primary.Input().m_InputData.m_Fire, 3);
	EXPECT_EQ(Primary.Input().m_aAmmoCount[WEAPON_LASER], 7);
	EXPECT_EQ(Additional.Input().m_MousePos, vec2(-5.0f, 6.0f));
	EXPECT_EQ(Additional.Input().m_InputData.m_Fire, 0);
	EXPECT_EQ(Primary.Runtime().m_ServerMode, CGameState::SERVERMODE_MOD);
	EXPECT_EQ(Additional.Runtime().m_ServerMode, CGameState::SERVERMODE_PUREMOD);
	EXPECT_EQ(Additional.Runtime().m_ShowOthers, 2);
	EXPECT_EQ(Additional.Runtime().m_LastRaceTick, 200);
	EXPECT_EQ(Primary.Runtime().m_aStrongHookLastUpdateTick[4], 123);
	EXPECT_EQ(Primary.Runtime().m_CharOrder.m_Ids.front(), 7);
	EXPECT_EQ(Additional.Runtime().m_aStrongHookLastUpdateTick[4], 456);
	EXPECT_EQ(Additional.Runtime().m_CharOrder.m_Ids.back(), 7);
	EXPECT_FALSE(Additional.Runtime().m_GameOver);
	EXPECT_FALSE(Additional.Runtime().m_ReceivedDDNetPlayer);

	Primary.Reset();
	EXPECT_EQ(Primary.Input().m_MousePos, vec2(0.0f, 0.0f));
	EXPECT_EQ(Primary.Input().m_InputData.m_Fire, 0);
	EXPECT_EQ(Primary.Input().m_aAmmoCount[WEAPON_LASER], 0);
	EXPECT_EQ(Primary.Runtime().m_ServerMode, CGameState::SERVERMODE_PURE);
	EXPECT_EQ(Primary.Runtime().m_CheckInfo, -1);
	EXPECT_FLOAT_EQ(Primary.Runtime().m_PlayerRecord, -1.0f);
	EXPECT_EQ(Primary.Runtime().m_LegacyPredictedTick, -1);
	EXPECT_EQ(Primary.Runtime().m_LastRoundStartTick, -1);
	EXPECT_EQ(Primary.Runtime().m_LastRaceTick, -1);
	EXPECT_EQ(Primary.Runtime().m_aStrongHookLastUpdateTick[4], 0);
	EXPECT_EQ(Primary.Runtime().m_CharOrder.m_Ids.front(), 0);
	EXPECT_EQ(Primary.Runtime().m_CharOrder.m_Ids.back(), MAX_CLIENTS - 1);
	EXPECT_EQ(Primary.Runtime().m_aFlagDropTick[TEAM_RED], 0);
	EXPECT_EQ(Primary.Runtime().m_aLastPredictedPosition[4], vec2(0.0f, 0.0f));
	EXPECT_FALSE(Primary.Runtime().m_aLastPredictedActive[4]);
	EXPECT_FALSE(Primary.Runtime().m_GameOver);
	EXPECT_FALSE(Primary.Runtime().m_ReceivedDDNetPlayer);
	EXPECT_FALSE(Primary.Runtime().m_ReceivedDDNetPlayerFinishTimes);
	EXPECT_EQ(Additional.Input().m_MousePos, vec2(-5.0f, 6.0f));
	EXPECT_EQ(Additional.Runtime().m_ServerMode, CGameState::SERVERMODE_PUREMOD);
	EXPECT_EQ(Additional.Runtime().m_ShowOthers, 2);
	EXPECT_EQ(Additional.Runtime().m_LastRaceTick, 200);
	EXPECT_EQ(Additional.Runtime().m_aStrongHookLastUpdateTick[4], 456);
	EXPECT_EQ(Additional.Runtime().m_CharOrder.m_Ids.back(), 7);
}

TEST(GameState, Protocol7ClientsAreOwnedAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	CGameState::CProtocol7ClientState &PrimaryClient = Primary.Protocol7Client(5);
	CGameState::CProtocol7ClientState &AdditionalClient = Additional.Protocol7Client(5);
	PrimaryClient.m_Active = true;
	str_copy(PrimaryClient.m_aaSkinPartNames[protocol7::SKINPART_BODY], "primary");
	PrimaryClient.m_aUseCustomColors[protocol7::SKINPART_BODY] = 1;
	PrimaryClient.m_aSkinPartColors[protocol7::SKINPART_BODY] = 123;
	PrimaryClient.m_PlayerFlags = protocol7::PLAYERFLAG_BOT;
	AdditionalClient.m_Active = true;
	str_copy(AdditionalClient.m_aaSkinPartNames[protocol7::SKINPART_BODY], "additional");

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	aClients[5].m_Active = true;
	Primary.ApplySnapshotData(1, 1, aClients);
	EXPECT_STREQ(Primary.Protocol7Client(5).m_aaSkinPartNames[protocol7::SKINPART_BODY], "primary");
	EXPECT_EQ(Primary.Protocol7Client(5).m_PlayerFlags, protocol7::PLAYERFLAG_BOT);
	EXPECT_STREQ(Additional.Protocol7Client(5).m_aaSkinPartNames[protocol7::SKINPART_BODY], "additional");

	aClients[5] = {};
	Primary.ApplySnapshotData(2, 0, std::move(aClients));
	EXPECT_FALSE(Primary.Protocol7Client(5).m_Active);
	EXPECT_EQ(Primary.Protocol7Client(5).m_aaSkinPartNames[protocol7::SKINPART_BODY][0], '\0');
	EXPECT_EQ(Primary.Protocol7Client(5).m_PlayerFlags, 0);
	EXPECT_TRUE(Additional.Protocol7Client(5).m_Active);
	EXPECT_STREQ(Additional.Protocol7Client(5).m_aaSkinPartNames[protocol7::SKINPART_BODY], "additional");
}

TEST(GameState, RaceMessagesAreOwnedAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	CGameState::CRaceMessageState &PrimaryMessages = Primary.RaceMessages();
	CGameState::CRaceMessageState &AdditionalMessages = Additional.RaceMessages();
	PrimaryMessages.ApplyDDRaceTime(10000, 75, false, 50);
	PrimaryMessages.ApplyDDRaceTime(12345, -125, true, 100);
	AdditionalMessages.ApplyLegacyRecord(67890, 250, 200);

	EXPECT_FLOAT_EQ(PrimaryMessages.m_CheckpointDiff, 0.75f);
	EXPECT_EQ(PrimaryMessages.m_CheckpointReceivedTick, 50);
	EXPECT_FLOAT_EQ(PrimaryMessages.m_FinishDiff, -1.25f);
	EXPECT_EQ(PrimaryMessages.m_FinishReceivedTick, 100);
	EXPECT_TRUE(PrimaryMessages.m_ShowFinish);
	EXPECT_FALSE(AdditionalMessages.m_ShowFinish);

	Primary.Reset();
	EXPECT_EQ(Primary.RaceMessages().m_DDRaceTime, 0);
	EXPECT_FLOAT_EQ(Primary.RaceMessages().m_CheckpointDiff, 0.0f);
	EXPECT_EQ(Primary.RaceMessages().m_CheckpointReceivedTick, 0);
	EXPECT_FLOAT_EQ(Primary.RaceMessages().m_FinishDiff, 0.0f);
	EXPECT_EQ(Primary.RaceMessages().m_FinishReceivedTick, 0);
	EXPECT_FALSE(Primary.RaceMessages().m_ShowFinish);
	EXPECT_EQ(Additional.RaceMessages().m_DDRaceTime, 67890);
	EXPECT_FLOAT_EQ(Additional.RaceMessages().m_CheckpointDiff, 2.5f);
	EXPECT_EQ(Additional.RaceMessages().m_CheckpointReceivedTick, 200);
	EXPECT_FALSE(Additional.RaceMessages().m_ShowFinish);
}

TEST(GameState, ExtendedCharactersAreIndependentAndResetPerState)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aPrimaryClients = {};
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aAdditionalClients = {};
	CGameState::CClientSnapshot &PrimaryClient = aPrimaryClients[5];
	PrimaryClient.m_Active = true;
	PrimaryClient.m_HasExtendedCharacter = true;
	PrimaryClient.m_ExtendedCharacter.m_Flags = CHARACTERFLAG_SOLO | CHARACTERFLAG_SUPER;
	PrimaryClient.m_ExtendedCharacter.m_FreezeEnd = -1;
	CGameState::CClientSnapshot &AdditionalClient = aAdditionalClients[5];
	AdditionalClient.m_Active = true;
	AdditionalClient.m_HasExtendedCharacter = true;
	AdditionalClient.m_ExtendedCharacter.m_Flags = CHARACTERFLAG_JETPACK | CHARACTERFLAG_MOVEMENTS_DISABLED;
	AdditionalClient.m_ExtendedCharacter.m_FreezeEnd = 123;
	Primary.ApplySnapshotData(10, 1, std::move(aPrimaryClients));
	Additional.ApplySnapshotData(20, 1, std::move(aAdditionalClients));

	ASSERT_NE(Primary.ExtendedCharacter(5), nullptr);
	ASSERT_NE(Additional.ExtendedCharacter(5), nullptr);
	EXPECT_EQ(Primary.ExtendedCharacter(5)->m_Flags, CHARACTERFLAG_SOLO | CHARACTERFLAG_SUPER);
	EXPECT_EQ(Primary.ExtendedCharacter(5)->m_FreezeEnd, -1);
	EXPECT_EQ(Additional.ExtendedCharacter(5)->m_Flags, CHARACTERFLAG_JETPACK | CHARACTERFLAG_MOVEMENTS_DISABLED);
	EXPECT_EQ(Additional.ExtendedCharacter(5)->m_FreezeEnd, 123);
	EXPECT_EQ(Primary.ExtendedCharacter(-1), nullptr);
	EXPECT_EQ(Primary.ExtendedCharacter(MAX_CLIENTS), nullptr);

	Primary.Reset();
	EXPECT_EQ(Primary.ExtendedCharacter(5), nullptr);
	ASSERT_NE(Additional.ExtendedCharacter(5), nullptr);
	EXPECT_EQ(Additional.ExtendedCharacter(5)->m_FreezeEnd, 123);
}

TEST(GameState, ExtendedPlayersAndSpectatorCharactersAreIndependentAndResetPerState)
{
	const auto pPrimary = std::make_unique<CGameState>(CGameStateId(1), CStreamId(1));
	const auto pAdditional = std::make_unique<CGameState>(CGameStateId(2), CStreamId(2));
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aPrimaryClients = {};
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aAdditionalClients = {};
	CGameState::CClientSnapshot &PrimaryClient = aPrimaryClients[5];
	PrimaryClient.m_HasDDNetPlayer = true;
	PrimaryClient.m_DDNetPlayer.m_AuthLevel = AUTHED_ADMIN;
	PrimaryClient.m_DDNetPlayer.m_Flags = EXPLAYERFLAG_AFK | EXPLAYERFLAG_PAUSED;
	PrimaryClient.m_DDNetPlayer.m_FinishTimeSeconds = 12;
	PrimaryClient.m_DDNetPlayer.m_FinishTimeMillis = 123;
	PrimaryClient.m_HasSpecChar = true;
	PrimaryClient.m_SpecChar.m_X = 100;
	PrimaryClient.m_SpecChar.m_Y = 200;
	CGameState::CClientSnapshot &AdditionalClient = aAdditionalClients[5];
	AdditionalClient.m_HasDDNetPlayer = true;
	AdditionalClient.m_DDNetPlayer.m_AuthLevel = AUTHED_MOD;
	AdditionalClient.m_DDNetPlayer.m_Flags = EXPLAYERFLAG_SPEC;
	AdditionalClient.m_DDNetPlayer.m_FinishTimeSeconds = FinishTime::UNSET;
	AdditionalClient.m_HasSpecChar = true;
	AdditionalClient.m_SpecChar.m_X = 300;
	AdditionalClient.m_SpecChar.m_Y = 400;
	pPrimary->ApplySnapshotData(10, 2, std::move(aPrimaryClients));
	pAdditional->ApplySnapshotData(10, 2, std::move(aAdditionalClients));

	EXPECT_EQ(pPrimary->Client(5).m_DDNetPlayer.m_AuthLevel, AUTHED_ADMIN);
	EXPECT_EQ(pPrimary->Client(5).m_DDNetPlayer.m_Flags, EXPLAYERFLAG_AFK | EXPLAYERFLAG_PAUSED);
	EXPECT_EQ(pPrimary->Client(5).m_SpecChar.m_X, 100);
	EXPECT_EQ(pAdditional->Client(5).m_DDNetPlayer.m_AuthLevel, AUTHED_MOD);
	EXPECT_EQ(pAdditional->Client(5).m_DDNetPlayer.m_Flags, EXPLAYERFLAG_SPEC);
	EXPECT_EQ(pAdditional->Client(5).m_SpecChar.m_X, 300);
	EXPECT_NE(pPrimary->SnapshotDigest(), pAdditional->SnapshotDigest());
	EXPECT_TRUE(pPrimary->Runtime().m_ReceivedDDNetPlayer);
	EXPECT_TRUE(pPrimary->Runtime().m_ReceivedDDNetPlayerFinishTimes);
	EXPECT_TRUE(pPrimary->Runtime().m_ReceivedDDNetPlayerFinishTimesMillis);
	EXPECT_TRUE(pAdditional->Runtime().m_ReceivedDDNetPlayer);
	EXPECT_FALSE(pAdditional->Runtime().m_ReceivedDDNetPlayerFinishTimes);
	EXPECT_FALSE(pAdditional->Runtime().m_ReceivedDDNetPlayerFinishTimesMillis);

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aEmptyClients = {};
	pPrimary->ApplySnapshotData(30, 0, std::move(aEmptyClients));
	EXPECT_FALSE(pPrimary->Client(5).m_HasDDNetPlayer);
	EXPECT_FALSE(pPrimary->Client(5).m_HasSpecChar);
	EXPECT_TRUE(pPrimary->Runtime().m_ReceivedDDNetPlayer);
	EXPECT_TRUE(pPrimary->Runtime().m_ReceivedDDNetPlayerFinishTimes);
	EXPECT_FALSE(pPrimary->Runtime().m_ReceivedDDNetPlayerFinishTimesMillis);
	EXPECT_TRUE(pAdditional->Client(5).m_HasDDNetPlayer);
	EXPECT_TRUE(pAdditional->Client(5).m_HasSpecChar);

	pAdditional->Reset();
	EXPECT_FALSE(pAdditional->Client(5).m_HasDDNetPlayer);
	EXPECT_FALSE(pAdditional->Client(5).m_HasSpecChar);
	EXPECT_TRUE(pPrimary->Runtime().m_ReceivedDDNetPlayer);

	pPrimary->Reset();
	EXPECT_FALSE(pPrimary->Runtime().m_ReceivedDDNetPlayer);
}

TEST(GameState, DamageIndicatorsAdvanceIndependently)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	Primary.DamageIndicators().Create(vec2(10.0f, 20.0f), vec2(1.0f, 0.0f), 4, 0.5f, 0.25f);
	Additional.DamageIndicators().Create(vec2(30.0f, 40.0f), vec2(0.0f, 1.0f), 9, 0.75f, 0.5f);

	ASSERT_EQ(Primary.DamageIndicators().NumItems(), 1);
	ASSERT_EQ(Additional.DamageIndicators().NumItems(), 1);
	EXPECT_EQ(Primary.DamageIndicators().Item(0).m_Pos, vec2(10.0f, 20.0f));
	EXPECT_EQ(Primary.DamageIndicators().Item(0).m_Dir, vec2(-1.0f, 0.0f));
	EXPECT_FLOAT_EQ(Primary.DamageIndicators().Item(0).m_Color.a, 0.5f);
	EXPECT_EQ(Primary.DamageIndicators().Item(0).m_OwnerClientId, 4);
	EXPECT_EQ(Additional.DamageIndicators().Item(0).m_OwnerClientId, 9);
	Primary.DamageIndicators().Advance(1000, 1000, 1.0f);
	Primary.DamageIndicators().Advance(1250, 1000, 1.0f);
	EXPECT_FLOAT_EQ(Primary.DamageIndicators().Item(0).m_RemainingLife, 0.5f);
	Primary.DamageIndicators().Advance(1500, 1000, 0.0f);
	EXPECT_FLOAT_EQ(Primary.DamageIndicators().Item(0).m_RemainingLife, 0.5f);
	Primary.DamageIndicators().Update(0.8f);
	EXPECT_EQ(Primary.DamageIndicators().NumItems(), 0);
	EXPECT_EQ(Additional.DamageIndicators().NumItems(), 1);

	Additional.Reset();
	EXPECT_EQ(Additional.DamageIndicators().NumItems(), 0);
}

TEST(GameState, EffectClocksAdvanceAndResetIndependently)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));

	Primary.EffectClock().Update(1000, 1000, 1.0f);
	Additional.EffectClock().Update(5, 1000, 1.0f);
	EXPECT_TRUE(Primary.EffectClock().m_Add5hz);
	EXPECT_TRUE(Primary.EffectClock().m_Add50hz);
	EXPECT_TRUE(Primary.EffectClock().m_Add100hz);
	EXPECT_FALSE(Additional.EffectClock().m_Add5hz);
	EXPECT_FALSE(Additional.EffectClock().m_Add50hz);
	EXPECT_FALSE(Additional.EffectClock().m_Add100hz);
	EXPECT_FALSE(Additional.EffectClock().TrySkidSound(100, 1000));
	EXPECT_TRUE(Additional.EffectClock().TrySkidSound(101, 1000));
	EXPECT_FALSE(Additional.EffectClock().TrySkidSound(201, 1000));
	EXPECT_TRUE(Additional.EffectClock().TrySkidSound(202, 1000));

	Primary.Reset();
	EXPECT_FALSE(Primary.EffectClock().m_Add5hz);
	EXPECT_EQ(Primary.EffectClock().m_LastUpdate5hz, 0);
	EXPECT_EQ(Primary.EffectClock().m_SkidSoundTimer, 0);
	EXPECT_EQ(Additional.EffectClock().m_SkidSoundTimer, 202);
}

TEST(GameState, SceneClocksAdvancePauseAndResetIndependently)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));

	Primary.SceneClock().Update(1000, 1000, 1.0f, 0.25f, 0.5f);
	Primary.SceneClock().Update(1500, 1000, 1.0f, 0.5f, 0.75f);
	Primary.SceneClock().Update(1500, 1000, 1.0f, 0.5f, 0.75f);
	Additional.SceneClock().Update(2000, 1000, 2.0f, 0.1f, 0.2f);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_AnimationTime, 0.5f);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_GameTickTime, 0.5f);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_PredIntraTick, 0.75f);
	EXPECT_FLOAT_EQ(Additional.SceneClock().m_AnimationTime, 0.0f);

	Primary.SceneClock().Update(2000, 1000, 0.0f, 0.9f, 0.9f);
	Additional.SceneClock().Update(2500, 1000, 2.0f, 0.3f, 0.4f);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_AnimationTime, 0.5f);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_GameTickTime, 0.5f);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_PredIntraTick, 0.75f);
	EXPECT_FLOAT_EQ(Additional.SceneClock().m_AnimationTime, 1.0f);

	Primary.Reset();
	EXPECT_FALSE(Primary.SceneClock().m_Initialized);
	EXPECT_FLOAT_EQ(Primary.SceneClock().m_AnimationTime, 0.0f);
	EXPECT_FLOAT_EQ(Additional.SceneClock().m_AnimationTime, 1.0f);
}

TEST(GameState, EnvelopeTimeUsesFrozenStateTicks)
{
	CGameState State(CGameStateId(1), CStreamId(1));
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 50;
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	State.ApplySnapshotData(101, 1, std::move(aClients), &GameInfo);

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
	aClients = {};
	State.ApplySnapshotData(201, 1, std::move(aClients), &GameInfo);
	EXPECT_EQ(CEnvelopeState::CalculateOnlineTime(State, Time, false).count(), 2005000000);

	State.Reset();
	EXPECT_EQ(CEnvelopeState::CalculateOnlineTime(State, Time, false).count(), 0);
}

TEST(GameState, ParticlePoolsAreLazyBoundedAndResetIndependently)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	CGameState::CParticle Particle;
	Particle.SetDefault();
	Particle.m_LifeSpan = 1.0f;
	EXPECT_EQ(Particle.m_OwnerClientId, -1);

	EXPECT_FALSE(Primary.Particles().Add(-1, Particle));
	EXPECT_TRUE(Additional.Particles().Add(0, Particle));
	for(int i = 0; i < CGameState::CParticleSystemState::MAX_PARTICLES; i++)
		ASSERT_TRUE(Primary.Particles().Add(0, Particle));
	EXPECT_FALSE(Primary.Particles().Add(0, Particle));
	EXPECT_EQ(Primary.Particles().NumParticles(), CGameState::CParticleSystemState::MAX_PARTICLES);
	EXPECT_EQ(Additional.Particles().NumParticles(), 1);

	Primary.Reset();
	EXPECT_EQ(Primary.Particles().NumParticles(), 0);
	EXPECT_EQ(Additional.Particles().NumParticles(), 1);
	EXPECT_TRUE(Primary.Particles().Add(0, Particle));
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

TEST(GameState, SessionsOwnDifferentMapsProtocolsAndStates)
{
	CGameSessionContextManager Contexts;
	CGameSessionContext *pNetwork = Contexts.Create(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1), CStreamId(2)});
	CGameSessionContext *pDemo = Contexts.Create(CSessionId(2), "Sunny Side Up", EGameProtocol::SIXUP, {CStreamId(3)});
	ASSERT_NE(pNetwork, nullptr);
	ASSERT_NE(pDemo, nullptr);
	EXPECT_EQ(Contexts.Create(CSessionId(1), "duplicate", EGameProtocol::SIX, {}), nullptr);

	CConfig Base{};
	Base.m_SvHit = 1;
	pNetwork->MapContext().GameConfig().Reset(Base);
	pDemo->MapContext().GameConfig().Reset(Base);
	pNetwork->MapContext().GameConfig().ExecuteLine("sv_hit 0");
	pNetwork->SetServerCapAnyPlayerFlag(true);
	pNetwork->GameStates().FindByStream(CStreamId(1))->ApplySnapshotMetadata(100, 2, 4);
	pDemo->GameStates().FindByStream(CStreamId(3))->ApplySnapshotMetadata(500, 7, 8);
	pNetwork->Broadcast().Apply("network broadcast", 100, 50);
	pDemo->Broadcast().Apply("demo broadcast", 500, 50);
	pNetwork->Motd().Apply("network\\nmotd");
	pDemo->Motd().Apply("demo motd");

	EXPECT_STREQ(pNetwork->MapName(), "Kobra 4");
	EXPECT_STREQ(pDemo->MapName(), "Sunny Side Up");
	EXPECT_EQ(pNetwork->Protocol(), EGameProtocol::SIX);
	EXPECT_EQ(pDemo->Protocol(), EGameProtocol::SIXUP);
	EXPECT_TRUE(pNetwork->ServerCapAnyPlayerFlag());
	EXPECT_FALSE(pDemo->ServerCapAnyPlayerFlag());
	EXPECT_EQ(pNetwork->MapContext().GameConfig().m_SvHit, 0);
	EXPECT_EQ(pDemo->MapContext().GameConfig().m_SvHit, 1);
	EXPECT_EQ(pNetwork->GameStates().FindByStream(CStreamId(1))->SnapshotTick(), 100);
	EXPECT_EQ(pDemo->GameStates().FindByStream(CStreamId(3))->SnapshotTick(), 500);
	EXPECT_EQ(pNetwork->GameStates().NumStates(), 2U);
	EXPECT_EQ(pDemo->GameStates().NumStates(), 1U);
	EXPECT_STREQ(pNetwork->Broadcast().Text(), "network broadcast");
	EXPECT_EQ(pNetwork->Broadcast().ExpireTick(), 600);
	EXPECT_TRUE(pNetwork->Broadcast().IsActiveAt(599));
	EXPECT_FALSE(pNetwork->Broadcast().IsActiveAt(600));
	EXPECT_EQ(pNetwork->Broadcast().Revision(), 1U);
	EXPECT_STREQ(pDemo->Broadcast().Text(), "demo broadcast");
	EXPECT_EQ(pDemo->Broadcast().ExpireTick(), 1000);
	pNetwork->Broadcast().Reset();
	EXPECT_STREQ(pNetwork->Broadcast().Text(), "");
	EXPECT_EQ(pNetwork->Broadcast().ExpireTick(), 0);
	EXPECT_EQ(pNetwork->Broadcast().Revision(), 2U);
	EXPECT_STREQ(pDemo->Broadcast().Text(), "demo broadcast");
	EXPECT_EQ(pDemo->Broadcast().Revision(), 1U);
	const std::string LongBroadcast(1100, 'x');
	pNetwork->Broadcast().Apply(LongBroadcast.c_str(), 200, 50);
	EXPECT_EQ(str_length(pNetwork->Broadcast().Text()), CSessionBroadcastState::MAX_TEXT_LENGTH);
	EXPECT_EQ(pNetwork->Broadcast().Revision(), 3U);
	EXPECT_STREQ(pNetwork->Motd().Text(), "network\nmotd");
	EXPECT_EQ(pNetwork->Motd().Revision(), 1U);
	EXPECT_STREQ(pDemo->Motd().Text(), "demo motd");
	pNetwork->Motd().Reset();
	EXPECT_STREQ(pNetwork->Motd().Text(), "");
	EXPECT_EQ(pNetwork->Motd().Revision(), 2U);
	EXPECT_STREQ(pDemo->Motd().Text(), "demo motd");
	const std::string LongMotd(1000, 'x');
	pNetwork->Motd().Apply(LongMotd.c_str());
	EXPECT_EQ(str_length(pNetwork->Motd().Text()), CSessionMotdState::MAX_TEXT_LENGTH);
	EXPECT_EQ(pNetwork->Motd().Revision(), 3U);
}

TEST(GameState, SessionMapMetadataIsIndependentAndResettable)
{
	CGameSessionContext Network(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1)});
	CGameSessionContext Demo(CSessionId(2), "Sunny Side Up", EGameProtocol::SIXUP, {CStreamId(2)});
	CSessionMapMetadataState &NetworkMetadata = Network.MapMetadata();
	CSessionMapMetadataState &DemoMetadata = Demo.MapMetadata();

	EXPECT_EQ(NetworkMetadata.BestTimeSeconds(), FinishTime::UNSET);
	EXPECT_EQ(NetworkMetadata.BestTimeMillis(), 0);
	EXPECT_STREQ(NetworkMetadata.Description(), "");
	NetworkMetadata.ApplyRecordBestTime(0);
	NetworkMetadata.ApplyRecordBestTime(-1);
	EXPECT_EQ(NetworkMetadata.BestTimeSeconds(), FinishTime::UNSET);

	NetworkMetadata.ApplyRecordBestTime(12345);
	NetworkMetadata.SetDescription("network description");
	DemoMetadata.ApplyBestTime(FinishTime::NOT_FINISHED_MILLIS, 999);
	DemoMetadata.SetDescription("demo description");
	EXPECT_EQ(NetworkMetadata.BestTimeSeconds(), 123);
	EXPECT_EQ(NetworkMetadata.BestTimeMillis(), 450);
	EXPECT_STREQ(NetworkMetadata.Description(), "network description");
	EXPECT_EQ(DemoMetadata.BestTimeSeconds(), FinishTime::NOT_FINISHED_MILLIS);
	EXPECT_EQ(DemoMetadata.BestTimeMillis(), 999);
	EXPECT_STREQ(DemoMetadata.Description(), "demo description");

	NetworkMetadata.ApplyRecordBestTime(0);
	NetworkMetadata.ApplyRecordBestTime(-1);
	EXPECT_EQ(NetworkMetadata.BestTimeSeconds(), 123);
	EXPECT_EQ(NetworkMetadata.BestTimeMillis(), 450);

	std::string LongDescription(CSessionMapMetadataState::MAX_DESCRIPTION_LENGTH - 1, 'd');
	LongDescription += "\xC3\xA4";
	NetworkMetadata.SetDescription(LongDescription.c_str());
	EXPECT_EQ(str_length(NetworkMetadata.Description()), CSessionMapMetadataState::MAX_DESCRIPTION_LENGTH - 1);
	EXPECT_EQ(NetworkMetadata.Description()[CSessionMapMetadataState::MAX_DESCRIPTION_LENGTH - 2], 'd');

	NetworkMetadata.Reset();
	EXPECT_EQ(NetworkMetadata.BestTimeSeconds(), FinishTime::UNSET);
	EXPECT_EQ(NetworkMetadata.BestTimeMillis(), 0);
	EXPECT_STREQ(NetworkMetadata.Description(), "");
	EXPECT_EQ(DemoMetadata.BestTimeSeconds(), FinishTime::NOT_FINISHED_MILLIS);
	EXPECT_STREQ(DemoMetadata.Description(), "demo description");
}

TEST(GameState, SessionInfoMessagesAreBoundedAndIndependent)
{
	CGameSessionContext Network(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1), CStreamId(2)});
	CGameSessionContext Demo(CSessionId(2), "Sunny Side Up", EGameProtocol::SIXUP, {CStreamId(3)});
	for(int Tick = 10; Tick <= 60; Tick += 10)
	{
		CSessionInfoMessageState::CMessage Message;
		Message.m_Tick = Tick;
		str_format(Message.m_aVictimName, sizeof(Message.m_aVictimName), "network-%d", Tick);
		Network.InfoMessages().Add(std::move(Message));
	}
	CSessionInfoMessageState::CMessage DemoMessage;
	DemoMessage.m_Type = CSessionInfoMessageState::EType::FINISH;
	DemoMessage.m_Tick = 500;
	str_copy(DemoMessage.m_aVictimName, "demo");
	Demo.InfoMessages().Add(std::move(DemoMessage));

	ASSERT_EQ(Network.InfoMessages().Count(), CSessionInfoMessageState::MAX_MESSAGES);
	for(int Index = 0; Index < Network.InfoMessages().Count(); ++Index)
	{
		const CSessionInfoMessageState::CMessage &Message = Network.InfoMessages().Message(Index);
		EXPECT_EQ(Message.m_Tick, 20 + Index * 10);
		EXPECT_EQ(Message.m_Id, static_cast<uint64_t>(Index + 2));
	}
	ASSERT_EQ(Demo.InfoMessages().Count(), 1);
	EXPECT_EQ(Demo.InfoMessages().Message(0).m_Tick, 500);
	EXPECT_STREQ(Demo.InfoMessages().Message(0).m_aVictimName, "demo");

	Network.InfoMessages().Reset();
	EXPECT_EQ(Network.InfoMessages().Count(), 0);
	ASSERT_EQ(Demo.InfoMessages().Count(), 1);
	EXPECT_EQ(Demo.InfoMessages().Message(0).m_Type, CSessionInfoMessageState::EType::FINISH);
}

TEST(GameState, SessionChatIsBoundedAndIndependent)
{
	CGameSessionContext Network(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1), CStreamId(2)});
	CGameSessionContext Demo(CSessionId(2), "Sunny Side Up", EGameProtocol::SIXUP, {CStreamId(3)});

	for(int Index = 0; Index <= CSessionChatState::MAX_LINES; ++Index)
	{
		CSessionChatState::CLine Line;
		Line.m_Time = 100 + Index;
		Line.m_ClientId = Index % MAX_CLIENTS;
		str_format(Line.m_aText, sizeof(Line.m_aText), "network-%d", Index);
		Network.Chat().Add(std::move(Line));
	}
	CSessionChatState::CLine Repeated;
	Repeated.m_Time = 999;
	Repeated.m_ClientId = CSessionChatState::MAX_LINES % MAX_CLIENTS;
	str_format(Repeated.m_aText, sizeof(Repeated.m_aText), "network-%d", CSessionChatState::MAX_LINES);
	const CSessionChatState::CLine &StoredRepeated = Network.Chat().Add(std::move(Repeated));

	ASSERT_EQ(Network.Chat().Count(), CSessionChatState::MAX_LINES);
	EXPECT_EQ(Network.Chat().Line(0).m_Id, 2);
	EXPECT_STREQ(Network.Chat().Line(0).m_aText, "network-1");
	EXPECT_EQ(StoredRepeated.m_Id, CSessionChatState::MAX_LINES + 1);
	EXPECT_EQ(StoredRepeated.m_Revision, 2);
	EXPECT_EQ(StoredRepeated.m_TimesRepeated, 1);
	EXPECT_EQ(StoredRepeated.m_Time, 999);

	CSessionChatState::CLine DemoLine;
	str_copy(DemoLine.m_aText, "network-64");
	Demo.Chat().Add(std::move(DemoLine));
	Demo.Chat().BeginCommandInfo();
	Demo.Chat().RegisterCommand("save", "?r[code]", "Save the team");
	Demo.Chat().RegisterCommand("load", "r[code]", "Load the team");
	Demo.Chat().RegisterCommand("save", "", "duplicate");
	const auto &Commands = Demo.Chat().SortedCommands();
	ASSERT_EQ(Commands.size(), 2);
	EXPECT_EQ(Commands[0].m_Name, "load");
	EXPECT_EQ(Commands[1].m_Name, "save");
	Demo.Chat().UnregisterCommand("load");
	EXPECT_EQ(Demo.Chat().Commands().size(), 1);
	EXPECT_EQ(Demo.Chat().Line(0).m_TimesRepeated, 0);

	EXPECT_TRUE(Network.Chat().Enqueue(CStreamId(1), 0, "first"));
	EXPECT_TRUE(Network.Chat().Enqueue(CStreamId(2), 1, "second"));
	EXPECT_TRUE(Network.Chat().Enqueue(CStreamId(1), 0, "third"));
	EXPECT_FALSE(Network.Chat().Enqueue(CStreamId(2), 1, "overflow"));
	EXPECT_EQ(Network.Chat().Pending().m_StreamId, CStreamId(1));
	EXPECT_EQ(Network.Chat().Pending().m_Text, "first");
	Network.Chat().PopPending();
	EXPECT_EQ(Network.Chat().Pending().m_StreamId, CStreamId(2));
	EXPECT_EQ(Network.Chat().Pending().m_Text, "second");
	EXPECT_EQ(Demo.Chat().PendingCount(), 0);
	Network.Chat().SetLastSend(777);
	EXPECT_EQ(Network.Chat().LastSend(), 777);
	EXPECT_EQ(Demo.Chat().LastSend(), 0);

	Network.Chat().Reset();
	EXPECT_EQ(Network.Chat().Count(), 0);
	EXPECT_EQ(Network.Chat().PendingCount(), 0);
	EXPECT_EQ(Network.Chat().LastSend(), 0);
	ASSERT_EQ(Demo.Chat().Count(), 1);
	EXPECT_STREQ(Demo.Chat().Line(0).m_aText, "network-64");
	EXPECT_EQ(Demo.Chat().Commands().size(), 1);
}

TEST(GameState, SessionStatsAreIndependentAndSurviveStateReset)
{
	CGameSessionContext Network(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1), CStreamId(2)});
	CGameSessionContext Demo(CSessionId(2), "Sunny Side Up", EGameProtocol::SIXUP, {CStreamId(3)});
	CSessionClientStats &NetworkStats = Network.Stats().Client(4);
	CSessionClientStats &DemoStats = Demo.Stats().Client(4);

	EXPECT_FALSE(NetworkStats.IsActive());
	EXPECT_EQ(NetworkStats.m_Frags, 0);
	EXPECT_EQ(NetworkStats.m_aFragsWith[WEAPON_LASER], 0);
	NetworkStats.JoinGame(100);
	NetworkStats.m_Frags = 3;
	NetworkStats.m_aFragsWith[WEAPON_LASER] = 2;
	NetworkStats.m_FlagCaptures = 1;
	NetworkStats.JoinSpec(160);
	EXPECT_FALSE(NetworkStats.IsActive());
	NetworkStats.JoinGame(200);
	EXPECT_TRUE(NetworkStats.IsActive());
	EXPECT_EQ(NetworkStats.GetIngameTicks(230), 90);
	EXPECT_EQ(NetworkStats.GetFPM(230, 50), 100.0f);
	DemoStats.JoinGame(500);
	DemoStats.m_Frags = 7;
	DemoStats.m_FlagGrabs = 2;
	Network.GameStates().FindByStream(CStreamId(1))->Reset();
	EXPECT_EQ(NetworkStats.m_Frags, 3);
	EXPECT_EQ(NetworkStats.m_FlagCaptures, 1);
	EXPECT_EQ(DemoStats.m_Frags, 7);
	EXPECT_EQ(DemoStats.m_FlagGrabs, 2);

	Network.Stats().Reset();
	EXPECT_FALSE(NetworkStats.IsActive());
	EXPECT_EQ(NetworkStats.m_Frags, 0);
	EXPECT_EQ(NetworkStats.m_aFragsWith[WEAPON_LASER], 0);
	EXPECT_EQ(NetworkStats.m_FlagCaptures, 0);
	EXPECT_TRUE(DemoStats.IsActive());
	EXPECT_EQ(DemoStats.m_Frags, 7);
	EXPECT_EQ(DemoStats.m_FlagGrabs, 2);
}

TEST(GameState, SessionStatsFollowTheirSnapshotLifecycle)
{
	CGameSessionContext Session(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1)});
	CGameState &State = *Session.GameStates().FindByStream(CStreamId(1));
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 10;
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	aClients[5].m_HasPlayerInfo = true;
	aClients[5].m_PlayerInfo.m_ClientId = 5;
	aClients[5].m_PlayerInfo.m_Team = TEAM_RED;
	State.ApplySnapshotData(100, 2, aClients, &GameInfo);
	Session.Stats().UpdateSnapshot(State, 100);
	CSessionClientStats &Stats = Session.Stats().Client(5);
	EXPECT_TRUE(Stats.IsActive());
	Stats.m_Frags = 2;

	aClients[5].m_PlayerInfo.m_Team = TEAM_SPECTATORS;
	State.ApplySnapshotData(150, 2, aClients, &GameInfo);
	Session.Stats().UpdateSnapshot(State, 150);
	EXPECT_FALSE(Stats.IsActive());
	aClients[5].m_PlayerInfo.m_Team = TEAM_RED;
	State.ApplySnapshotData(160, 2, aClients, &GameInfo);
	Session.Stats().UpdateSnapshot(State, 160);
	EXPECT_EQ(Stats.GetIngameTicks(160), 50);

	GameInfo.m_RoundStartTick = 20;
	State.ApplySnapshotData(200, 2, std::move(aClients), &GameInfo);
	Session.Stats().UpdateSnapshot(State, 200);
	EXPECT_FALSE(Stats.IsActive());
	EXPECT_EQ(Stats.m_Frags, 0);
}

TEST(GameState, SessionVotesAreIndependentAndPreserveOptionsAcrossVotes)
{
	CGameSessionContext Network(CSessionId(1), "Kobra 4", EGameProtocol::SIX, {CStreamId(1)});
	CGameSessionContext Demo(CSessionId(2), "Sunny Side Up", EGameProtocol::SIXUP, {CStreamId(2)});
	CSessionVoteState &NetworkVote = Network.Vote();
	CSessionVoteState &DemoVote = Demo.Vote();

	NetworkVote.AddOption("alpha");
	NetworkVote.AddOption("duplicate");
	NetworkVote.AddOption("duplicate");
	DemoVote.AddOption("beta");
	NetworkVote.RemoveOption("duplicate");
	ASSERT_EQ(NetworkVote.NumOptions(), 2);
	ASSERT_NE(NetworkVote.Option(0), nullptr);
	ASSERT_NE(NetworkVote.Option(1), nullptr);
	ASSERT_NE(DemoVote.Option(0), nullptr);
	EXPECT_STREQ(NetworkVote.Option(0)->c_str(), "alpha");
	EXPECT_STREQ(NetworkVote.Option(1)->c_str(), "duplicate");
	EXPECT_EQ(NetworkVote.Option(-1), nullptr);
	EXPECT_EQ(NetworkVote.Option(2), nullptr);
	EXPECT_STREQ(DemoVote.Option(0)->c_str(), "beta");

	const std::string LongDescription(VOTE_DESC_LENGTH + 10, 'd');
	const std::string LongReason(VOTE_REASON_LENGTH + 10, 'r');
	EXPECT_TRUE(NetworkVote.ApplyVoteSet(10, LongDescription.c_str(), LongReason.c_str(), 1000, 100));
	EXPECT_TRUE(NetworkVote.IsVoting());
	EXPECT_EQ(NetworkVote.OpenTime(), 1000);
	EXPECT_EQ(NetworkVote.CloseTime(), 2000);
	EXPECT_EQ(NetworkVote.SecondsLeft(1000, 100), 10);
	EXPECT_EQ(NetworkVote.SecondsLeft(2000, 100), 0);
	EXPECT_EQ(NetworkVote.SecondsLeft(2099, 100), 0);
	EXPECT_EQ(NetworkVote.SecondsLeft(2100, 100), -1);
	EXPECT_EQ(str_length(NetworkVote.Description()), VOTE_DESC_LENGTH - 1);
	EXPECT_EQ(str_length(NetworkVote.Reason()), VOTE_REASON_LENGTH - 1);
	EXPECT_TRUE(DemoVote.ApplyVoteSet(20, "demo vote", "demo reason", 1000, 100));
	NetworkVote.Expire(2100, 100);
	EXPECT_FALSE(NetworkVote.IsVoting());
	EXPECT_TRUE(DemoVote.IsVoting());
	std::string Utf8Description(VOTE_DESC_LENGTH - 2, 'd');
	Utf8Description += "\xC3\xA4";
	std::string Utf8Reason(VOTE_REASON_LENGTH - 2, 'r');
	Utf8Reason += "\xC3\xA4";
	EXPECT_TRUE(NetworkVote.ApplyVoteSet(10, Utf8Description.c_str(), Utf8Reason.c_str(), 1000, 100));
	EXPECT_EQ(str_length(NetworkVote.Description()), VOTE_DESC_LENGTH - 2);
	EXPECT_EQ(str_length(NetworkVote.Reason()), VOTE_REASON_LENGTH - 2);
	NetworkVote.ApplyStatus(3, 2, 4, 9);
	NetworkVote.SetVoted(-1);
	NetworkVote.SetReceivingOptions(true);
	EXPECT_EQ(NetworkVote.Yes(), 3);
	EXPECT_EQ(NetworkVote.No(), 2);
	EXPECT_EQ(NetworkVote.Pass(), 4);
	EXPECT_EQ(NetworkVote.Total(), 9);
	EXPECT_EQ(NetworkVote.Voted(), -1);
	EXPECT_TRUE(NetworkVote.IsReceivingOptions());

	EXPECT_FALSE(NetworkVote.ApplyVoteSet(0, "", "", 3000, 100));
	EXPECT_FALSE(NetworkVote.IsVoting());
	EXPECT_EQ(NetworkVote.NumOptions(), 2);
	EXPECT_FALSE(NetworkVote.IsReceivingOptions());
	EXPECT_EQ(DemoVote.NumOptions(), 1);

	NetworkVote.Reset();
	EXPECT_EQ(NetworkVote.NumOptions(), 0);
	EXPECT_EQ(DemoVote.NumOptions(), 1);
	NetworkVote.AddOption(Utf8Description.c_str());
	ASSERT_NE(NetworkVote.Option(0), nullptr);
	EXPECT_EQ(NetworkVote.Option(0)->size(), VOTE_DESC_LENGTH - 2);
	NetworkVote.ClearOptions();
	for(int i = 0; i < MAX_VOTE_OPTIONS + 1; ++i)
		NetworkVote.AddOption("bounded");
	EXPECT_EQ(NetworkVote.NumOptions(), MAX_VOTE_OPTIONS);
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
	CGameInfo PrimaryInfo;
	PrimaryInfo.m_PredictDDRace = true;
	PrimaryInfo.m_PredictDDRaceTiles = true;
	PrimaryInfo.m_BugDDRaceInput = true;
	CGameInfo AdditionalInfo;
	AdditionalInfo.m_PredictFNG = true;
	AdditionalInfo.m_PredictVanilla = true;
	AdditionalInfo.m_NoWeakHookAndBounce = true;
	Primary.SetCoreGameInfo(PrimaryInfo);
	Additional.SetCoreGameInfo(AdditionalInfo);
	CTuningParams SoloTuning = CTuningParams::DEFAULT;
	SoloTuning.m_PlayerCollision = 0;
	SoloTuning.m_PlayerHooking = 0;
	Primary.ApplyTuning(SoloTuning);

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aPrimaryClients = {};
	aPrimaryClients[3].m_Active = true;
	aPrimaryClients[3].m_HasPlayerInfo = true;
	aPrimaryClients[3].m_PlayerInfo.m_Local = 1;
	aPrimaryClients[3].m_HasCharacter = true;
	aPrimaryClients[3].m_Character.m_X = 320;
	aPrimaryClients[3].m_Character.m_Y = 640;
	aPrimaryClients[3].m_Character.m_Weapon = WEAPON_GUN;
	aPrimaryClients[3].m_Character.m_AmmoCount = 10;

	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aAdditionalClients = {};
	aAdditionalClients[7].m_Active = true;
	aAdditionalClients[7].m_HasPlayerInfo = true;
	aAdditionalClients[7].m_PlayerInfo.m_Local = 1;
	aAdditionalClients[7].m_HasCharacter = true;
	aAdditionalClients[7].m_Character.m_X = 960;
	aAdditionalClients[7].m_Character.m_Y = 1280;
	aAdditionalClients[7].m_HasExtendedCharacter = true;

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
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_InfiniteAmmo);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_IsSolo);
	EXPECT_TRUE(Additional.GameWorld().m_WorldConfig.m_InfiniteAmmo);
	EXPECT_FALSE(Additional.GameWorld().m_WorldConfig.m_IsSolo);
	CTuningParams TeamTuning = CTuningParams::DEFAULT;
	Primary.ApplyTuning(TeamTuning);
	EXPECT_FALSE(Primary.GameWorld().m_WorldConfig.m_IsSolo);
	Primary.ApplyTuning(SoloTuning);
	EXPECT_TRUE(Primary.GameWorld().m_WorldConfig.m_IsSolo);
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
	EXPECT_EQ(Primary.PredictionHistory(3).m_aPredTick[100], 100);
	EXPECT_EQ(Primary.PredictionHistory(3).m_aPredTick[101], 101);
	EXPECT_EQ(Additional.PredictionHistory(7).m_aPredTick[0], 200);
	EXPECT_EQ(Additional.PredictionHistory(7).m_aPredTick[1], 201);
	EXPECT_NE(Primary.PredictionHistory(3).m_aPredPos[101], Additional.PredictionHistory(7).m_aPredPos[1]);
	EXPECT_TRUE(Primary.PredictedWorld().m_WorldConfig.m_IsDDRace);
	EXPECT_TRUE(Primary.PredictedWorld().m_WorldConfig.m_PredictTiles);
	EXPECT_TRUE(Primary.PredictedWorld().m_WorldConfig.m_BugDDRaceInput);
	EXPECT_FALSE(Primary.PredictedWorld().m_WorldConfig.m_InfiniteAmmo);
	EXPECT_TRUE(Primary.PredictedWorld().m_WorldConfig.m_IsSolo);
	EXPECT_TRUE(Primary.PrevPredictedWorld().m_WorldConfig.m_IsDDRace);
	EXPECT_TRUE(Additional.PredictedWorld().m_WorldConfig.m_IsFNG);
	EXPECT_TRUE(Additional.PredictedWorld().m_WorldConfig.m_IsVanilla);
	EXPECT_TRUE(Additional.PredictedWorld().m_WorldConfig.m_NoWeakHookAndBounce);
	EXPECT_TRUE(Additional.PrevPredictedWorld().m_WorldConfig.m_IsFNG);
	EXPECT_EQ(Primary.SnapshotDigest(), PrimarySnapshotDigest);
	EXPECT_EQ(Additional.SnapshotDigest(), AdditionalSnapshotDigest);
	EXPECT_NE(Primary.PredictionDigest(), Additional.PredictionDigest());
}

TEST(GameState, EntitySnapshotsOwnCurrentAndPreviousData)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	CNetObj_Pickup Current = {};
	Current.m_X = 100;
	Current.m_Y = 200;
	CNetObj_Pickup Prev = {};
	Prev.m_X = 50;
	Prev.m_Y = 150;
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
	AddEntity(7, NETOBJTYPE_PICKUP, Current, Prev);
	CNetObj_Flag CurrentFlag = {};
	CurrentFlag.m_X = 300;
	CurrentFlag.m_Team = TEAM_RED;
	CNetObj_Flag PrevFlag = CurrentFlag;
	PrevFlag.m_X = 250;
	AddEntity(0, NETOBJTYPE_FLAG, CurrentFlag, PrevFlag);
	CNetObj_GameData CurrentGameData = {};
	CurrentGameData.m_FlagCarrierRed = 4;
	CNetObj_GameData PrevGameData = CurrentGameData;
	PrevGameData.m_FlagCarrierRed = FLAG_ATSTAND;
	AddEntity(0, NETOBJTYPE_GAMEDATA, CurrentGameData, PrevGameData);
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	Primary.ApplySnapshotData(10, 3, aClients, nullptr, std::move(vEntities));
	Additional.ApplySnapshotData(20, 0, std::move(aClients));

	ASSERT_EQ(Primary.Entities().size(), 3U);
	const CGameState::CEntitySnapshot &Stored = Primary.Entities().front();
	EXPECT_EQ(Stored.m_Id, 7);
	EXPECT_EQ(reinterpret_cast<const CNetObj_Pickup *>(Stored.m_vData.data())->m_X, 100);
	EXPECT_EQ(reinterpret_cast<const CNetObj_Pickup *>(Stored.m_vPrevData.data())->m_X, 50);
	const auto Flag = std::find_if(Primary.Entities().begin(), Primary.Entities().end(), [](const CGameState::CEntitySnapshot &StoredEntity) { return StoredEntity.m_Type == NETOBJTYPE_FLAG; });
	const auto GameData = std::find_if(Primary.Entities().begin(), Primary.Entities().end(), [](const CGameState::CEntitySnapshot &StoredEntity) { return StoredEntity.m_Type == NETOBJTYPE_GAMEDATA; });
	ASSERT_NE(Flag, Primary.Entities().end());
	ASSERT_NE(GameData, Primary.Entities().end());
	EXPECT_EQ(reinterpret_cast<const CNetObj_Flag *>(Flag->m_vData.data())->m_X, 300);
	EXPECT_EQ(reinterpret_cast<const CNetObj_Flag *>(Flag->m_vPrevData.data())->m_X, 250);
	EXPECT_EQ(reinterpret_cast<const CNetObj_GameData *>(GameData->m_vData.data())->m_FlagCarrierRed, 4);
	EXPECT_EQ(reinterpret_cast<const CNetObj_GameData *>(GameData->m_vPrevData.data())->m_FlagCarrierRed, FLAG_ATSTAND);
	ASSERT_NE(Primary.GameData(), nullptr);
	EXPECT_EQ(Primary.GameData()->m_FlagCarrierRed, 4);
	EXPECT_EQ(Additional.GameData(), nullptr);
	EXPECT_TRUE(Additional.Entities().empty());
	EXPECT_NE(Primary.SnapshotDigest(), Additional.SnapshotDigest());
	Primary.Reset();
	EXPECT_TRUE(Primary.Entities().empty());
}

TEST(GameView, RenderingTwoViewsDoesNotAdvanceState)
{
	const CSessionId SessionId(3);
	CGameSessionContext Session(SessionId, "render-test", EGameProtocol::SIX, {CStreamId(1)});
	CGameState *pState = Session.GameStates().FindByStream(CStreamId(1));
	ASSERT_NE(pState, nullptr);
	CGameState &State = *pState;
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	aClients[4].m_HasPlayerInfo = true;
	aClients[4].m_PlayerInfo.m_Local = 1;
	aClients[4].m_HasCharacter = true;
	aClients[4].m_Character.m_X = 100;
	aClients[4].m_Character.m_Y = 200;
	aClients[5].m_HasSpecChar = true;
	aClients[5].m_SpecChar.m_X = 300;
	aClients[5].m_SpecChar.m_Y = 400;
	State.SetTeam(4, 1);
	State.SetTeam(5, 2);
	State.ApplySnapshotData(50, 3, std::move(aClients));

	CGameViewManager ViewManager;
	const CGameViewId LeftId = ViewManager.Create(SessionId, State.Id());
	const CGameViewId RightId = ViewManager.Create(SessionId, State.Id());
	ASSERT_TRUE(LeftId.IsValid());
	ASSERT_TRUE(RightId.IsValid());
	ASSERT_NE(LeftId, RightId);
	CGameView *pLeft = ViewManager.Find(LeftId);
	CGameView *pRight = ViewManager.Find(RightId);
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pRight, nullptr);
	EXPECT_TRUE(pLeft->MatchesTarget(SessionId, State.Id()));
	EXPECT_TRUE(pLeft->MatchesBinding(LeftId, SessionId, State.Id()));
	EXPECT_FALSE(pLeft->MatchesBinding(RightId, SessionId, State.Id()));
	EXPECT_FALSE(pLeft->MatchesTarget(CSessionId(99), State.Id()));
	EXPECT_FALSE(pLeft->MatchesTarget(SessionId, CGameStateId(99)));
	pLeft->Camera().m_LastInputPosition = vec2(10.0f, 20.0f);
	pLeft->Camera().m_DynamicCameraOffset = vec2(30.0f, 40.0f);
	pRight->Camera().m_LastInputPosition = vec2(50.0f, 60.0f);
	pRight->Camera().m_DynamicCameraOffset = vec2(70.0f, 80.0f);
	pLeft->Camera().m_CameraSmoothing = true;
	pLeft->Camera().m_AutoSpecCamera = false;
	pLeft->Camera().m_GotoTeleOffset = 5;
	EXPECT_EQ(pLeft->Camera().m_LastInputPosition, vec2(10.0f, 20.0f));
	EXPECT_EQ(pLeft->Camera().m_DynamicCameraOffset, vec2(30.0f, 40.0f));
	EXPECT_EQ(pRight->Camera().m_LastInputPosition, vec2(50.0f, 60.0f));
	EXPECT_EQ(pRight->Camera().m_DynamicCameraOffset, vec2(70.0f, 80.0f));
	EXPECT_TRUE(pLeft->Camera().m_CameraSmoothing);
	EXPECT_FALSE(pLeft->Camera().m_AutoSpecCamera);
	EXPECT_EQ(pLeft->Camera().m_GotoTeleOffset, 5);
	EXPECT_FALSE(pRight->Camera().m_CameraSmoothing);
	EXPECT_TRUE(pRight->Camera().m_AutoSpecCamera);
	EXPECT_EQ(pRight->Camera().m_GotoTeleOffset, 0);
	pLeft->SetViewport({0, 0, 640, 720});
	pLeft->SetCameraPosition(vec2(100.0f, 200.0f));
	pRight->SetViewport({640, 0, 640, 720});
	pRight->SetCameraPosition(vec2(300.0f, 400.0f));
	pRight->SetZoom(2.0f);
	pRight->SetSpectator(true, SPEC_FREEVIEW);
	State.EffectClock().Update(1000, 1000, 1.0f);
	EXPECT_TRUE(State.EffectClock().TrySkidSound(1001, 1000));
	State.SceneClock().Update(1000, 1000, 1.0f, 0.25f, 0.5f);
	State.SceneClock().Update(1500, 1000, 1.0f, 0.5f, 0.75f);
	CGameState::CParticle Particle;
	Particle.SetDefault();
	Particle.m_LifeSpan = 1.0f;
	ASSERT_TRUE(State.Particles().Add(0, Particle));
	State.DamageIndicators().Create(vec2(10.0f, 20.0f), vec2(1.0f, 0.0f), 4, 0.5f, 0.25f);

	const uint64_t SnapshotDigest = State.SnapshotDigest();
	const uint64_t PredictionDigest = State.PredictionDigest();
	const CGameState::CEffectClockState EffectClock = State.EffectClock();
	const CGameState::CSceneClockState SceneClock = State.SceneClock();
	const int NumParticles = State.Particles().NumParticles();
	const int NumDamageIndicators = State.DamageIndicators().NumItems();
	const CGameState::CDamageIndicatorState::CItem DamageIndicator = State.DamageIndicators().Item(0);
	CTestRenderOutput Output;
	CGameStateRenderer Renderer;
	CGameTickInfo LeftTime;
	LeftTime.m_GameTick = 50;
	LeftTime.m_IntraGameTick = 0.25f;
	LeftTime.m_GameTickSpeed = 50;
	CGameTickInfo RightTime;
	RightTime.m_GameTick = 51;
	RightTime.m_IntraGameTick = 0.75f;
	RightTime.m_GameTickSpeed = 50;
	const CRenderContext LeftContext(Session, State, *pLeft, LeftTime, CVisibleWorldRect(vec2(0.0f, 0.0f), vec2(100.0f, 100.0f)));
	const CRenderContext RightContext(Session, State, *pRight, RightTime, CVisibleWorldRect(vec2(200.0f, 200.0f), vec2(300.0f, 300.0f)));
	EXPECT_EQ(&LeftContext.m_Session, &Session);
	EXPECT_EQ(&LeftContext.m_State, &State);
	EXPECT_EQ(&LeftContext.m_View, pLeft);
	EXPECT_EQ(LeftContext.m_Time.m_GameTick, 50);
	EXPECT_FLOAT_EQ(LeftContext.m_Time.m_IntraGameTick, 0.25f);
	EXPECT_TRUE(LeftContext.m_VisibleWorldRect.Inside(vec2(50.0f, 50.0f), vec2(0.0f, 0.0f)));
	EXPECT_FLOAT_EQ(LeftContext.AspectRatio(16.0f / 9.0f), 640.0f / 720.0f);
	EXPECT_EQ(&RightContext.m_View, pRight);
	EXPECT_EQ(RightContext.m_Time.m_GameTick, 51);
	EXPECT_FLOAT_EQ(RightContext.m_Time.m_IntraGameTick, 0.75f);
	EXPECT_FLOAT_EQ(LeftContext.AlphaForOwner(5, 0.25f), 0.25f);
	EXPECT_FLOAT_EQ(RightContext.AlphaForOwner(5, 0.25f), 1.0f);
	EXPECT_FLOAT_EQ(LeftContext.AlphaForOwner(-1, 0.25f), 1.0f);
	Renderer.Render(LeftContext, Output);
	Renderer.Render(RightContext, Output);

	ASSERT_EQ(Output.m_vViewports.size(), 2U);
	EXPECT_EQ(Output.m_vViewports[0], (CViewport{0, 0, 640, 720}));
	EXPECT_EQ(Output.m_vViewports[1], (CViewport{640, 0, 640, 720}));
	EXPECT_EQ(Output.m_vCharacters.size(), 2U);
	ASSERT_EQ(Output.m_vSpectatorCharacters.size(), 2U);
	EXPECT_EQ(Output.m_vSpectatorCharacters[0].m_ClientId, 5);
	EXPECT_EQ(Output.m_vSpectatorCharacters[0].m_Position, vec2(300.0f, 400.0f));
	EXPECT_TRUE(Output.m_vSpectatorCharacters[0].m_OtherTeam);
	EXPECT_EQ(Output.m_vSpectatorCharacters[1].m_ClientId, 5);
	EXPECT_EQ(Output.m_vSpectatorCharacters[1].m_Position, vec2(300.0f, 400.0f));
	EXPECT_FALSE(Output.m_vSpectatorCharacters[1].m_OtherTeam);
	EXPECT_EQ(Output.m_EndedViews, 2);
	EXPECT_EQ(State.SnapshotDigest(), SnapshotDigest);
	EXPECT_EQ(State.PredictionDigest(), PredictionDigest);
	EXPECT_EQ(State.EffectClock().m_Add5hz, EffectClock.m_Add5hz);
	EXPECT_EQ(State.EffectClock().m_LastUpdate5hz, EffectClock.m_LastUpdate5hz);
	EXPECT_EQ(State.EffectClock().m_Add50hz, EffectClock.m_Add50hz);
	EXPECT_EQ(State.EffectClock().m_LastUpdate50hz, EffectClock.m_LastUpdate50hz);
	EXPECT_EQ(State.EffectClock().m_Add100hz, EffectClock.m_Add100hz);
	EXPECT_EQ(State.EffectClock().m_LastUpdate100hz, EffectClock.m_LastUpdate100hz);
	EXPECT_EQ(State.EffectClock().m_SkidSoundTimer, EffectClock.m_SkidSoundTimer);
	EXPECT_FLOAT_EQ(State.SceneClock().m_AnimationTime, SceneClock.m_AnimationTime);
	EXPECT_FLOAT_EQ(State.SceneClock().m_GameTickTime, SceneClock.m_GameTickTime);
	EXPECT_FLOAT_EQ(State.SceneClock().m_PredIntraTick, SceneClock.m_PredIntraTick);
	EXPECT_EQ(State.SceneClock().m_LastUpdateTime, SceneClock.m_LastUpdateTime);
	EXPECT_EQ(State.SceneClock().m_Initialized, SceneClock.m_Initialized);
	EXPECT_EQ(State.Particles().NumParticles(), NumParticles);
	ASSERT_EQ(State.DamageIndicators().NumItems(), NumDamageIndicators);
	EXPECT_EQ(State.DamageIndicators().Item(0).m_Pos, DamageIndicator.m_Pos);
	EXPECT_EQ(State.DamageIndicators().Item(0).m_Dir, DamageIndicator.m_Dir);
	EXPECT_FLOAT_EQ(State.DamageIndicators().Item(0).m_RemainingLife, DamageIndicator.m_RemainingLife);
	EXPECT_FLOAT_EQ(State.DamageIndicators().Item(0).m_StartAngle, DamageIndicator.m_StartAngle);
	EXPECT_EQ(State.DamageIndicators().Item(0).m_Color, DamageIndicator.m_Color);
	EXPECT_TRUE(ViewManager.Destroy(LeftId));
	EXPECT_EQ(ViewManager.NumViews(), 1U);
}

TEST(GameView, PresentationContextCombinesVisibleWorldRectsWithoutView)
{
	CGameSessionContext Session(CSessionId(4), "presentation-test", EGameProtocol::SIX, {CStreamId(1)});
	CGameState *pState = Session.GameStates().FindByStream(CStreamId(1));
	ASSERT_NE(pState, nullptr);
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	aClients[1].m_HasPlayerInfo = true;
	aClients[1].m_PlayerInfo.m_Local = 1;
	pState->SetTeam(1, 1);
	pState->SetTeam(2, 2);
	pState->ApplySnapshotData(50, 1, std::move(aClients));

	const std::array aVisibleWorldRects = {
		CVisibleWorldRect(vec2(0.0f, 0.0f), vec2(100.0f, 100.0f)),
		CVisibleWorldRect(vec2(200.0f, 200.0f), vec2(300.0f, 300.0f)),
	};
	CGameTickInfo Time;
	Time.m_GameTick = 50;
	Time.m_PredictionTick = 52;
	Time.m_GameTickSpeed = 50;
	CPresentationContext Context(Session, *pState, Time, aVisibleWorldRects, EPresentationPlayback::PLAYING, EPresentationAudio::MUTED);

	EXPECT_EQ(&Context.m_Session, &Session);
	EXPECT_EQ(&Context.m_State, pState);
	EXPECT_EQ(Context.m_Time.m_PredictionTick, 52);
	EXPECT_EQ(Context.m_vVisibleWorldRects.size(), 2U);
	EXPECT_EQ(Context.m_Playback, EPresentationPlayback::PLAYING);
	EXPECT_EQ(Context.m_Audio, EPresentationAudio::MUTED);
	EXPECT_TRUE(Context.IsVisible(vec2(50.0f, 50.0f), vec2(0.0f, 0.0f)));
	EXPECT_TRUE(Context.IsVisible(vec2(250.0f, 250.0f), vec2(0.0f, 0.0f)));
	EXPECT_FALSE(Context.IsVisible(vec2(150.0f, 150.0f), vec2(0.0f, 0.0f)));
	EXPECT_TRUE(Context.IsVisible(vec2(150.0f, 150.0f), vec2(50.0f, 50.0f)));
	EXPECT_FALSE(Context.IsOtherTeamFromLocalPlayer(1));
	EXPECT_TRUE(Context.IsOtherTeamFromLocalPlayer(2));
}

TEST(GameView, SchedulerUpdatesEachStateBeforeRenderingExplicitOutputs)
{
	CGameSessionContext Network(CSessionId(10), "network", EGameProtocol::SIX, {CStreamId(1), CStreamId(2)});
	CGameSessionContext Demo(CSessionId(20), "demo", EGameProtocol::SIX, {CStreamId(1)});
	CGameState *pMainState = Network.GameStates().FindByStream(CStreamId(1));
	CGameState *pDummyState = Network.GameStates().FindByStream(CStreamId(2));
	CGameState *pDemoState = Demo.GameStates().FindByStream(CStreamId(1));
	ASSERT_NE(pMainState, nullptr);
	ASSERT_NE(pDummyState, nullptr);
	ASSERT_NE(pDemoState, nullptr);

	CGameViewManager ViewManager;
	CGameView *pMainLeft = ViewManager.Find(ViewManager.Create(Network.Id(), pMainState->Id()));
	CGameView *pDemoView = ViewManager.Find(ViewManager.Create(Demo.Id(), pDemoState->Id()));
	CGameView *pDummyView = ViewManager.Find(ViewManager.Create(Network.Id(), pDummyState->Id()));
	CGameView *pMainRight = ViewManager.Find(ViewManager.Create(Network.Id(), pMainState->Id()));
	ASSERT_NE(pMainLeft, nullptr);
	ASSERT_NE(pDemoView, nullptr);
	ASSERT_NE(pDummyView, nullptr);
	ASSERT_NE(pMainRight, nullptr);
	pMainLeft->SetViewport({0, 0, 640, 720});
	pDemoView->SetViewport({0, 0, 320, 180});
	pDummyView->SetViewport({320, 0, 320, 180});
	pMainRight->SetViewport({640, 0, 640, 720});
	pMainRight->SetCameraPosition(vec2(123.0f, 456.0f));

	CGameTickInfo MainTime;
	MainTime.m_GameTick = 50;
	MainTime.m_GameTickSpeed = 50;
	MainTime.m_PresentationTime = 1000;
	MainTime.m_PresentationTimeFrequency = 1000;
	MainTime.m_AnimationPlaybackSpeed = 1.0f;
	MainTime.m_IsGameActive = true;
	CGameTickInfo DummyTime;
	DummyTime.m_GameTick = 60;
	DummyTime.m_GameTickSpeed = 50;
	DummyTime.m_PresentationTime = 2000;
	DummyTime.m_PresentationTimeFrequency = 1000;
	DummyTime.m_AnimationPlaybackSpeed = 1.0f;
	DummyTime.m_IsGameActive = true;
	CGameTickInfo DemoTime;
	DemoTime.m_GameTick = 70;
	DemoTime.m_GameTickSpeed = 50;
	DemoTime.m_PresentationTime = 3000;
	DemoTime.m_PresentationTimeFrequency = 1000;
	DemoTime.m_AnimationPlaybackSpeed = 0.0f;
	DemoTime.m_IsGameActive = true;
	DemoTime.m_IsDemoPlayback = true;

	CTestRenderOutput MainLeftOutput;
	CTestRenderOutput DemoOffscreenOutput;
	CTestRenderOutput DummyOutput;
	CTestRenderOutput MainRightOutput;
	const std::array aRequests = {
		CGameRenderRequest(Network, *pMainState, *pMainLeft, MainTime, CVisibleWorldRect(vec2(0.0f, 0.0f), vec2(100.0f, 100.0f)), EPresentationPlayback::PLAYING, EPresentationAudio::MUTED, MainLeftOutput),
		CGameRenderRequest(Demo, *pDemoState, *pDemoView, DemoTime, CVisibleWorldRect(vec2(400.0f, 400.0f), vec2(500.0f, 500.0f)), EPresentationPlayback::PAUSED, EPresentationAudio::MUTED, DemoOffscreenOutput),
		CGameRenderRequest(Network, *pDummyState, *pDummyView, DummyTime, CVisibleWorldRect(vec2(-200.0f, -200.0f), vec2(-100.0f, -100.0f)), EPresentationPlayback::PLAYING, EPresentationAudio::MUTED, DummyOutput),
		CGameRenderRequest(Network, *pMainState, *pMainRight, MainTime, CVisibleWorldRect(vec2(200.0f, 200.0f), vec2(300.0f, 300.0f)), EPresentationPlayback::PLAYING, EPresentationAudio::AUDIBLE, MainRightOutput),
	};
	const CGameRenderRequest *pAudibleRequest = FindAudibleRenderRequest(aRequests);
	ASSERT_EQ(pAudibleRequest, &aRequests[3]);
	EXPECT_EQ(pAudibleRequest->m_View.CameraPosition(), vec2(123.0f, 456.0f));

	std::vector<const CGameState *> vpUpdatedStates;
	std::vector<const CGameView *> vpRenderedViews;
	std::vector<CRenderOutput *> vpRenderOutputs;
	std::vector<CVisibleWorldRect> vRenderedWorldRects;
	CGameStateRenderer Renderer;
	CGameRenderScheduler Scheduler;
	Scheduler.Run(
		aRequests,
		[&](const CPresentationContext &Context) {
			EXPECT_TRUE(vpRenderedViews.empty());
			vpUpdatedStates.push_back(&Context.m_State);
			if(&Context.m_State == pMainState)
			{
				EXPECT_EQ(Context.m_Time.m_PresentationTime, 1000);
				EXPECT_EQ(Context.m_Time.m_PresentationTimeFrequency, 1000);
				EXPECT_FLOAT_EQ(Context.m_Time.m_AnimationPlaybackSpeed, 1.0f);
				EXPECT_TRUE(Context.m_Time.m_IsGameActive);
				EXPECT_EQ(Context.m_vVisibleWorldRects.size(), 2U);
				EXPECT_TRUE(Context.IsVisible(vec2(50.0f, 50.0f), vec2(0.0f, 0.0f)));
				EXPECT_TRUE(Context.IsVisible(vec2(250.0f, 250.0f), vec2(0.0f, 0.0f)));
				EXPECT_FALSE(Context.IsVisible(vec2(150.0f, 150.0f), vec2(0.0f, 0.0f)));
				EXPECT_EQ(Context.m_Audio, EPresentationAudio::AUDIBLE);
			}
			else if(&Context.m_State == pDemoState)
			{
				EXPECT_EQ(Context.m_Time.m_PresentationTime, 3000);
				EXPECT_FLOAT_EQ(Context.m_Time.m_AnimationPlaybackSpeed, 0.0f);
				EXPECT_TRUE(Context.m_Time.m_IsDemoPlayback);
			}
		},
		[&](const CRenderContext &Context, CRenderOutput &Output) {
			EXPECT_EQ(vpUpdatedStates.size(), 3U);
			EXPECT_EQ(Context.m_OutputCacheKey, Output.PresentationCacheKey());
			EXPECT_FALSE(Context.m_IsVideoOutput);
			vpRenderedViews.push_back(&Context.m_View);
			vpRenderOutputs.push_back(&Output);
			vRenderedWorldRects.push_back(Context.m_VisibleWorldRect);
			Renderer.Render(Context, Output);
		});

	ASSERT_EQ(vpUpdatedStates.size(), 3U);
	EXPECT_EQ(std::count(vpUpdatedStates.begin(), vpUpdatedStates.end(), pMainState), 1);
	EXPECT_EQ(std::count(vpUpdatedStates.begin(), vpUpdatedStates.end(), pDummyState), 1);
	EXPECT_EQ(std::count(vpUpdatedStates.begin(), vpUpdatedStates.end(), pDemoState), 1);
	ASSERT_EQ(vpRenderedViews.size(), aRequests.size());
	EXPECT_EQ(vpRenderedViews[0], pMainLeft);
	EXPECT_EQ(vpRenderedViews[1], pDemoView);
	EXPECT_EQ(vpRenderedViews[2], pDummyView);
	EXPECT_EQ(vpRenderedViews[3], pMainRight);
	EXPECT_EQ(vpRenderOutputs[0], &MainLeftOutput);
	EXPECT_EQ(vpRenderOutputs[1], &DemoOffscreenOutput);
	EXPECT_EQ(vpRenderOutputs[2], &DummyOutput);
	EXPECT_EQ(vpRenderOutputs[3], &MainRightOutput);
	EXPECT_EQ(vRenderedWorldRects[0].m_TopLeft, vec2(0.0f, 0.0f));
	EXPECT_EQ(vRenderedWorldRects[1].m_TopLeft, vec2(400.0f, 400.0f));
	EXPECT_EQ(vRenderedWorldRects[2].m_TopLeft, vec2(-200.0f, -200.0f));
	EXPECT_EQ(vRenderedWorldRects[3].m_TopLeft, vec2(200.0f, 200.0f));
	EXPECT_EQ(MainLeftOutput.m_EndedViews, 1);
	EXPECT_EQ(DemoOffscreenOutput.m_EndedViews, 1);
	EXPECT_EQ(DummyOutput.m_EndedViews, 1);
	EXPECT_EQ(MainRightOutput.m_EndedViews, 1);
}

TEST(GameView, EqualStateIdsInDifferentSessionsRemainDistinct)
{
	CGameSessionContext Network(CSessionId(10), "network", EGameProtocol::SIX, {CStreamId(1)});
	CGameSessionContext Demo(CSessionId(20), "demo", EGameProtocol::SIXUP, {CStreamId(1)});
	const CGameState *pNetworkState = Network.GameStates().FindByStream(CStreamId(1));
	const CGameState *pDemoState = Demo.GameStates().FindByStream(CStreamId(1));
	ASSERT_NE(pNetworkState, nullptr);
	ASSERT_NE(pDemoState, nullptr);
	ASSERT_EQ(pNetworkState->Id(), pDemoState->Id());
	CGameViewManager ViewManager;
	const CGameViewId NetworkViewId = ViewManager.Create(Network.Id(), pNetworkState->Id());
	const CGameViewId DemoViewId = ViewManager.Create(Demo.Id(), pDemoState->Id());
	ASSERT_TRUE(NetworkViewId.IsValid());
	ASSERT_TRUE(DemoViewId.IsValid());
	const CGameView *pNetworkView = ViewManager.Find(NetworkViewId);
	const CGameView *pDemoView = ViewManager.Find(DemoViewId);
	ASSERT_NE(pNetworkView, nullptr);
	ASSERT_NE(pDemoView, nullptr);
	EXPECT_EQ(pNetworkView->SessionId(), Network.Id());
	EXPECT_EQ(pNetworkView->StateId(), pNetworkState->Id());
	EXPECT_EQ(pDemoView->SessionId(), Demo.Id());
	EXPECT_EQ(pDemoView->StateId(), pDemoState->Id());
	CGameTickInfo NetworkTime;
	NetworkTime.m_GameTick = 100;
	NetworkTime.m_GameTickSpeed = 50;
	CGameTickInfo DemoTime;
	DemoTime.m_GameTick = 200;
	DemoTime.m_GameTickSpeed = 50;
	const CRenderContext NetworkContext(Network, *pNetworkState, *pNetworkView, NetworkTime, CVisibleWorldRect(vec2(0.0f, 0.0f), vec2(100.0f, 100.0f)));
	const CRenderContext DemoContext(Demo, *pDemoState, *pDemoView, DemoTime, CVisibleWorldRect(vec2(200.0f, 200.0f), vec2(300.0f, 300.0f)));
	EXPECT_EQ(&NetworkContext.m_Session, &Network);
	EXPECT_EQ(&DemoContext.m_Session, &Demo);
	EXPECT_EQ(NetworkContext.m_Time.m_GameTick, 100);
	EXPECT_EQ(DemoContext.m_Time.m_GameTick, 200);
}

TEST(GameView, SelectorStatesAreIndependentAndStaleSpectatorIntentIsCancelled)
{
	CGameViewManager ViewManager;
	const CGameViewId LeftId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	const CGameViewId RightId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	CGameView *pLeft = ViewManager.Find(LeftId);
	CGameView *pRight = ViewManager.Find(RightId);
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pRight, nullptr);

	EXPECT_FALSE(pLeft->EmoticonSelector().m_Active);
	EXPECT_EQ(pLeft->EmoticonSelector().m_SelectedEmote, -1);
	EXPECT_FALSE(pLeft->SpectatorSelector().m_Active);
	EXPECT_EQ(pLeft->SpectatorSelector().m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_EQ(pLeft->SpectatorSelector().m_MultiViewActivateTime, 0.0f);

	pLeft->EmoticonSelector().m_Active = true;
	pLeft->EmoticonSelector().m_SelectorMouse = vec2(10.0f, 20.0f);
	pLeft->EmoticonSelector().m_SelectedEmote = 3;
	pLeft->EmoticonSelector().m_TouchPressedOutside = true;
	pLeft->EmoticonSelector().m_OriginSessionId = CSessionId(1);
	pLeft->EmoticonSelector().m_OriginConnection = 1;
	pLeft->SpectatorSelector().m_Active = true;
	pLeft->SpectatorSelector().m_SelectorMouse = vec2(30.0f, 40.0f);
	pLeft->SpectatorSelector().m_SelectedSpectatorId = 7;
	pLeft->SpectatorSelector().m_MultiViewActivateTime = 12.5f;
	pLeft->SpectatorSelector().m_OriginSessionId = CSessionId(1);
	pLeft->SpectatorSelector().m_OriginStateId = CGameStateId(1);
	pLeft->SpectatorSelector().m_PendingSpectatorId = 8;

	EXPECT_FALSE(pRight->EmoticonSelector().m_Active);
	EXPECT_EQ(pRight->EmoticonSelector().m_SelectorMouse, vec2(0.0f, 0.0f));
	EXPECT_EQ(pRight->EmoticonSelector().m_SelectedEmote, -1);
	EXPECT_FALSE(pRight->EmoticonSelector().m_TouchPressedOutside);
	EXPECT_FALSE(pRight->SpectatorSelector().m_Active);
	EXPECT_EQ(pRight->SpectatorSelector().m_SelectorMouse, vec2(0.0f, 0.0f));
	EXPECT_EQ(pRight->SpectatorSelector().m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_EQ(pRight->SpectatorSelector().m_MultiViewActivateTime, 0.0f);

	pLeft->SetTarget(CSessionId(2), CGameStateId(2));
	EXPECT_FALSE(pLeft->MatchesTarget(CSessionId(1), CGameStateId(1)));
	EXPECT_TRUE(pLeft->MatchesTarget(CSessionId(2), CGameStateId(2)));
	EXPECT_TRUE(pLeft->EmoticonSelector().m_Active);
	EXPECT_EQ(pLeft->EmoticonSelector().m_SelectedEmote, 3);
	EXPECT_EQ(pLeft->EmoticonSelector().m_OriginSessionId, CSessionId(1));
	EXPECT_EQ(pLeft->EmoticonSelector().m_OriginConnection, 1);
	EXPECT_FALSE(pLeft->SpectatorSelector().m_Active);
	EXPECT_EQ(pLeft->SpectatorSelector().m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_EQ(pLeft->SpectatorSelector().m_MultiViewActivateTime, 0.0f);
	EXPECT_FALSE(pLeft->SpectatorSelector().m_OriginSessionId.IsValid());
	EXPECT_EQ(pLeft->SpectatorSelector().m_PendingSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);

	pLeft->EmoticonSelector().Reset();
	pLeft->SpectatorSelector().Reset();
	EXPECT_FALSE(pLeft->EmoticonSelector().m_Active);
	EXPECT_EQ(pLeft->EmoticonSelector().m_SelectedEmote, -1);
	EXPECT_FALSE(pLeft->SpectatorSelector().m_Active);
	EXPECT_EQ(pLeft->SpectatorSelector().m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_EQ(pLeft->SpectatorSelector().m_MultiViewActivateTime, 0.0f);
}

TEST(GameView, SpectatorSelectionUsesViewOwnedLayoutAndDemoMode)
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

	CGameViewManager ViewManager;
	CGameView *pView = ViewManager.Find(ViewManager.Create(CSessionId(1), CGameStateId(1)));
	ASSERT_NE(pView, nullptr);
	pView->SetSpectator(true, 3);
	EXPECT_EQ(pView->SpectatorMode(), 3);
	pView->SetSpectatorMode(SPEC_FOLLOW);
	EXPECT_EQ(pView->SpectatorMode(), SPEC_FOLLOW);
}

TEST(GameView, EmoticonSelectionUsesTheViewOwnedPointer)
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

TEST(GameView, SpectatorCursorsAreIndependentAndResetWhenRetargeted)
{
	CGameViewManager ViewManager;
	const CGameViewId LeftId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	const CGameViewId RightId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	CGameView *pLeft = ViewManager.Find(LeftId);
	CGameView *pRight = ViewManager.Find(RightId);
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pRight, nullptr);

	EXPECT_FALSE(pLeft->SpectatorCursor().IsAvailable());
	EXPECT_EQ(pLeft->SpectatorCursor().m_CursorOwnerId, -1);
	EXPECT_EQ(pLeft->SpectatorCursor().m_NumSamples, 0);
	pLeft->SpectatorCursor().m_CursorOwnerId = 5;
	pLeft->SpectatorCursor().m_aTargetSamplesTime[0] = 100.0;
	pLeft->SpectatorCursor().m_aTargetSamplesData[0] = vec2(10.0f, 20.0f);
	pLeft->SpectatorCursor().m_NumSamples = 1;
	pLeft->SpectatorCursor().m_Available = true;
	pLeft->SpectatorCursor().m_Weapon = 3;
	pLeft->SpectatorCursor().m_Target = vec2(30.0f, 40.0f);
	pLeft->SpectatorCursor().m_WorldTarget = vec2(50.0f, 60.0f);
	pLeft->SpectatorCursor().m_Position = vec2(70.0f, 80.0f);

	EXPECT_FALSE(pRight->SpectatorCursor().IsAvailable());
	EXPECT_EQ(pRight->SpectatorCursor().m_CursorOwnerId, -1);
	EXPECT_EQ(pRight->SpectatorCursor().m_NumSamples, 0);
	EXPECT_EQ(pRight->SpectatorCursor().Target(), vec2(0.0f, 0.0f));
	pLeft->SetTarget(CSessionId(1), CGameStateId(1));
	EXPECT_TRUE(pLeft->SpectatorCursor().IsAvailable());
	EXPECT_EQ(pLeft->SpectatorCursor().m_NumSamples, 1);
	pLeft->SetTarget(CSessionId(1), CGameStateId(2));
	EXPECT_FALSE(pLeft->SpectatorCursor().IsAvailable());
	EXPECT_EQ(pLeft->SpectatorCursor().m_CursorOwnerId, -1);
	EXPECT_EQ(pLeft->SpectatorCursor().m_NumSamples, 0);
	EXPECT_EQ(pLeft->SpectatorCursor().Target(), vec2(0.0f, 0.0f));

	pLeft->SpectatorCursor().m_Available = true;
	pLeft->SpectatorCursor().m_CursorOwnerId = 7;
	pLeft->SetTarget(CSessionId(2), CGameStateId(2));
	EXPECT_FALSE(pLeft->SpectatorCursor().IsAvailable());
	EXPECT_EQ(pLeft->SpectatorCursor().m_CursorOwnerId, -1);
	pLeft->SpectatorCursor().m_Available = true;
	pLeft->SpectatorCursor().m_CursorOwnerId = 9;
	pLeft->SpectatorCursor().Reset();
	EXPECT_FALSE(pLeft->SpectatorCursor().IsAvailable());
	EXPECT_EQ(pLeft->SpectatorCursor().m_CursorOwnerId, -1);
	EXPECT_EQ(pLeft->SpectatorCursor().m_NumSamples, 0);
	EXPECT_EQ(pLeft->SpectatorCursor().Target(), vec2(0.0f, 0.0f));
}

TEST(GameView, MultiViewStatesAreIndependentAndReset)
{
	CGameViewManager ViewManager;
	const CGameViewId LeftId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	const CGameViewId RightId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	CGameView *pLeft = ViewManager.Find(LeftId);
	CGameView *pRight = ViewManager.Find(RightId);
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pRight, nullptr);

	CGameView::CMultiViewState &Left = pLeft->MultiView();
	CGameView::CMultiViewState &Right = pRight->MultiView();
	Left.m_Team = 3;
	Left.m_PersonalZoom = 1.5f;
	Left.m_ShowHud = true;
	Left.m_Active = true;
	Left.m_aSelected[7] = true;
	Left.m_Solo = true;
	Left.m_IsInit = true;
	Left.m_Teleported = true;
	Left.m_aVanish[7] = true;
	Left.m_OldPos = vec2(10.0f, 20.0f);
	Left.m_OldPersonalZoom = 2;
	Left.m_SecondChance = 3.0f;
	Left.m_OldCameraDistance = 4.0f;
	Left.m_aLastFreeze[7] = 5.0f;

	Right.m_Team = 9;
	Right.m_aSelected[8] = true;
	Right.m_OldPos = vec2(-10.0f, -20.0f);
	EXPECT_FALSE(Right.m_Active);
	EXPECT_FALSE(Right.m_aSelected[7]);
	EXPECT_FALSE(Right.m_aVanish[7]);
	EXPECT_FLOAT_EQ(Right.m_aLastFreeze[7], 0.0f);

	Left.Reset();
	EXPECT_EQ(Left.m_Team, 0);
	EXPECT_FLOAT_EQ(Left.m_PersonalZoom, 0.0f);
	EXPECT_FALSE(Left.m_ShowHud);
	EXPECT_FALSE(Left.m_Active);
	EXPECT_FALSE(Left.m_aSelected[7]);
	EXPECT_FALSE(Left.m_Solo);
	EXPECT_FALSE(Left.m_IsInit);
	EXPECT_FALSE(Left.m_Teleported);
	EXPECT_FALSE(Left.m_aVanish[7]);
	EXPECT_EQ(Left.m_OldPos, vec2(0.0f, 0.0f));
	EXPECT_EQ(Left.m_OldPersonalZoom, 0);
	EXPECT_FLOAT_EQ(Left.m_SecondChance, 0.0f);
	EXPECT_FLOAT_EQ(Left.m_OldCameraDistance, 0.0f);
	EXPECT_FLOAT_EQ(Left.m_aLastFreeze[7], 0.0f);
	EXPECT_EQ(Right.m_Team, 9);
	EXPECT_TRUE(Right.m_aSelected[8]);
	EXPECT_EQ(Right.m_OldPos, vec2(-10.0f, -20.0f));
}

TEST(GameView, MotdVisibilityIsViewLocalAndRevisionBound)
{
	CGameViewManager ViewManager;
	const CGameViewId LeftId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	const CGameViewId RightId = ViewManager.Create(CSessionId(1), CGameStateId(1));
	CGameView *pLeft = ViewManager.Find(LeftId);
	CGameView *pRight = ViewManager.Find(RightId);
	ASSERT_NE(pLeft, nullptr);
	ASSERT_NE(pRight, nullptr);

	pLeft->Motd().Show(CSessionId(1), 4, 100);
	EXPECT_TRUE(pLeft->Motd().IsActive(CSessionId(1), 4, 99));
	EXPECT_FALSE(pLeft->Motd().IsActive(CSessionId(1), 4, 100));
	EXPECT_FALSE(pRight->Motd().IsActive(CSessionId(1), 4, 99));
	EXPECT_FALSE(pLeft->Motd().IsActive(CSessionId(1), 5, 99));

	pLeft->SetTarget(CSessionId(2), CGameStateId(2));
	EXPECT_FALSE(pLeft->Motd().IsActive(CSessionId(2), 4, 99));
	pLeft->SetTarget(CSessionId(1), CGameStateId(1));
	EXPECT_TRUE(pLeft->Motd().IsActive(CSessionId(1), 4, 99));
	pLeft->Motd().Dismiss();
	EXPECT_FALSE(pLeft->Motd().IsActive(CSessionId(1), 4, 99));
	pLeft->Motd().Show(CSessionId(1), 5, 200);
	EXPECT_TRUE(pLeft->Motd().IsActive(CSessionId(1), 5, 199));
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

	const uint64_t BeforeSpectatorCount = State.SnapshotDigest();
	CNetObj_SpectatorCount SpectatorCount = {};
	SpectatorCount.m_NumSpectators = 7;
	State.ApplySpectatorCount(SpectatorCount);
	EXPECT_TRUE(State.HasSpectatorCount());
	EXPECT_EQ(State.SpectatorCount().m_NumSpectators, 7);
	const uint64_t DigestWithSpectatorCount = State.SnapshotDigest();
	EXPECT_NE(DigestWithSpectatorCount, BeforeSpectatorCount);

	State.Reset();
	EXPECT_FALSE(State.HasSpectatorCount());
	EXPECT_NE(State.SnapshotDigest(), DigestWithSpectatorCount);
}

TEST(GameState, StrokedInputCommandsRemainStateLocal)
{
	CGameState First(CGameStateId(1), CStreamId(1));
	CGameState Second(CGameStateId(2), CStreamId(2));

	EXPECT_TRUE(First.Input().ApplyStrokedCommand("+fire", 1, false));
	EXPECT_EQ(First.Input().m_InputData.m_Fire & 1, 1);
	EXPECT_EQ(Second.Input().m_InputData.m_Fire, 0);
	EXPECT_TRUE(First.Input().ApplyStrokedCommand("+fire", 0, true));
	EXPECT_EQ(First.Input().m_InputData.m_Fire & 1, 0);

	EXPECT_TRUE(First.Input().ApplyStrokedCommand("+hook", 1, true));
	EXPECT_EQ(First.Input().m_InputData.m_Hook, 0);
	EXPECT_TRUE(First.Input().ApplyStrokedCommand("+weapon3", 1, true));
	EXPECT_EQ(First.Input().m_InputData.m_WantedWeapon, 3);
	EXPECT_TRUE(First.Input().ApplyStrokedCommand("+nextweapon", 1, false));
	EXPECT_EQ(First.Input().m_InputData.m_WantedWeapon, 0);
	EXPECT_FALSE(First.Input().ApplyStrokedCommand("+not-a-game-input", 1, false));
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
