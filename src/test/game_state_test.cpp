#include "test.h"

#include <base/str.h>

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/demo.h>
#include <engine/shared/network.h>
#include <engine/shared/snapshot.h>
#include <engine/storage.h>

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
	Primary.DamageIndicators().Create(vec2(10.0f, 20.0f), vec2(1.0f, 0.0f), 0.5f, 0.25f);
	Additional.DamageIndicators().Create(vec2(30.0f, 40.0f), vec2(0.0f, 1.0f), 0.75f, 0.5f);

	ASSERT_EQ(Primary.DamageIndicators().NumItems(), 1);
	ASSERT_EQ(Additional.DamageIndicators().NumItems(), 1);
	EXPECT_EQ(Primary.DamageIndicators().Item(0).m_Pos, vec2(10.0f, 20.0f));
	EXPECT_EQ(Primary.DamageIndicators().Item(0).m_Dir, vec2(-1.0f, 0.0f));
	EXPECT_FLOAT_EQ(Primary.DamageIndicators().Item(0).m_Color.a, 0.5f);
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

TEST(GameState, ParticlePoolsAreLazyBoundedAndResetIndependently)
{
	CGameState Primary(CGameStateId(1), CStreamId(1));
	CGameState Additional(CGameStateId(2), CStreamId(2));
	CGameState::CParticle Particle;
	Particle.SetDefault();
	Particle.m_LifeSpan = 1.0f;

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
	State.DamageIndicators().Create(vec2(10.0f, 20.0f), vec2(1.0f, 0.0f), 0.5f, 0.25f);

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
	const CRenderContext LeftContext(Session, State, *pLeft, LeftTime);
	const CRenderContext RightContext(Session, State, *pRight, RightTime);
	EXPECT_EQ(&LeftContext.m_Session, &Session);
	EXPECT_EQ(&LeftContext.m_State, &State);
	EXPECT_EQ(&LeftContext.m_View, pLeft);
	EXPECT_EQ(LeftContext.m_Time.m_GameTick, 50);
	EXPECT_FLOAT_EQ(LeftContext.m_Time.m_IntraGameTick, 0.25f);
	EXPECT_EQ(&RightContext.m_View, pRight);
	EXPECT_EQ(RightContext.m_Time.m_GameTick, 51);
	EXPECT_FLOAT_EQ(RightContext.m_Time.m_IntraGameTick, 0.75f);
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
	const CRenderContext NetworkContext(Network, *pNetworkState, *pNetworkView, NetworkTime);
	const CRenderContext DemoContext(Demo, *pDemoState, *pDemoView, DemoTime);
	EXPECT_EQ(&NetworkContext.m_Session, &Network);
	EXPECT_EQ(&DemoContext.m_Session, &Demo);
	EXPECT_EQ(NetworkContext.m_Time.m_GameTick, 100);
	EXPECT_EQ(DemoContext.m_Time.m_GameTick, 200);
}

TEST(GameView, SelectorStatesAreIndependentAndSurviveRetargeting)
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
	pLeft->SpectatorSelector().m_Active = true;
	pLeft->SpectatorSelector().m_SelectorMouse = vec2(30.0f, 40.0f);
	pLeft->SpectatorSelector().m_SelectedSpectatorId = 7;
	pLeft->SpectatorSelector().m_MultiViewActivateTime = 12.5f;

	EXPECT_FALSE(pRight->EmoticonSelector().m_Active);
	EXPECT_EQ(pRight->EmoticonSelector().m_SelectorMouse, vec2(0.0f, 0.0f));
	EXPECT_EQ(pRight->EmoticonSelector().m_SelectedEmote, -1);
	EXPECT_FALSE(pRight->EmoticonSelector().m_TouchPressedOutside);
	EXPECT_FALSE(pRight->SpectatorSelector().m_Active);
	EXPECT_EQ(pRight->SpectatorSelector().m_SelectorMouse, vec2(0.0f, 0.0f));
	EXPECT_EQ(pRight->SpectatorSelector().m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_EQ(pRight->SpectatorSelector().m_MultiViewActivateTime, 0.0f);

	pLeft->SetTarget(CSessionId(2), CGameStateId(2));
	EXPECT_TRUE(pLeft->EmoticonSelector().m_Active);
	EXPECT_EQ(pLeft->EmoticonSelector().m_SelectedEmote, 3);
	EXPECT_TRUE(pLeft->SpectatorSelector().m_Active);
	EXPECT_EQ(pLeft->SpectatorSelector().m_SelectedSpectatorId, 7);
	EXPECT_EQ(pLeft->SpectatorSelector().m_MultiViewActivateTime, 12.5f);

	pLeft->EmoticonSelector().Reset();
	pLeft->SpectatorSelector().Reset();
	EXPECT_FALSE(pLeft->EmoticonSelector().m_Active);
	EXPECT_EQ(pLeft->EmoticonSelector().m_SelectedEmote, -1);
	EXPECT_FALSE(pLeft->SpectatorSelector().m_Active);
	EXPECT_EQ(pLeft->SpectatorSelector().m_SelectedSpectatorId, CGameView::CSpectatorSelectorState::NO_SELECTION);
	EXPECT_EQ(pLeft->SpectatorSelector().m_MultiViewActivateTime, 0.0f);
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
