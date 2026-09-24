/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "session_runtime.h"

#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/video.h>

#include <algorithm>
#include <cmath>

int64_t CSessionRuntime::DemoPlaybackTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).m_DemoPlayer.Video())
		return pVideo->Time();
#endif
	return time_get();
}

// TODO: OPT: do this a lot smarter!
int *CSessionRuntime::GetInput(CSessionId SessionId, int Tick) const
{
	int Best = -1;
	const CConnection &GameConnection = Connection(SessionId);
	for(int i = 0; i < 200; i++)
	{
		if(GameConnection.m_aInputs[i].m_Tick != -1 && GameConnection.m_aInputs[i].m_Tick <= Tick && (Best == -1 || GameConnection.m_aInputs[Best].m_Tick < GameConnection.m_aInputs[i].m_Tick))
			Best = i;
	}

	if(Best != -1)
		return (int *)GameConnection.m_aInputs[Best].m_aData;
	return nullptr;
}

int CSessionRuntime::GetPredictionTime(CSessionId SessionId)
{
	int64_t Now = time_get();
	return (int)((Connection(SessionId).m_PredictedTime.Get(Now) - Connection(SessionId).m_GameTime.Get(Now)) * 1000 / (float)time_freq());
}

int CSessionRuntime::GetPredictionTick(CSessionId SessionId)
{
	int PredictionTick = GetPredictionTime(SessionId) * GameTickSpeed() / 1000.0f;

	int PredictionMin = g_Config.m_ClAntiPingLimit * GameTickSpeed() / 1000.0f;

	if(g_Config.m_ClAntiPingLimit == 0)
	{
		float PredictionPercentage = 1 - g_Config.m_ClAntiPingPercent / 100.0f;
		PredictionMin = std::floor(PredictionTick * PredictionPercentage);
	}

	if(PredictionMin > PredictionTick - 1)
	{
		PredictionMin = PredictionTick - 1;
	}

	if(PredictionMin <= 0)
		return PredGameTick(SessionId);

	PredictionTick = PredGameTick(SessionId) - PredictionMin;

	if(PredictionTick < GameTick(SessionId) + 1)
	{
		PredictionTick = GameTick(SessionId) + 1;
	}
	return PredictionTick;
}

void CSessionRuntime::GetSmoothTick(CSessionId SessionId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount)
{
	int64_t GameTime = Connection(SessionId).m_GameTime.Get(Now);
	int64_t PredTime = Connection(SessionId).m_PredictedTime.Get(Now);
	int64_t SmoothTime = std::clamp(GameTime + (int64_t)(MixAmount * (PredTime - GameTime)), GameTime, PredTime);

	*pSmoothTick = (int)(SmoothTime * GameTickSpeed() / time_freq()) + 1;
	*pSmoothIntraTick = (SmoothTime - (*pSmoothTick - 1) * time_freq() / GameTickSpeed()) / (float)(time_freq() / GameTickSpeed());
}

ISessions::CSnapItem CSessionRuntime::SnapGetItem(CSessionId SessionId, int SnapId, int Index) const
{
	dbg_assert(SnapId >= 0 && SnapId < NUM_SNAPSHOT_TYPES, "invalid SnapId");
	const CSnapshot *pSnapshot = Connection(SessionId).m_apSnapshots[SnapId]->m_pAltSnap;
	const CSnapshotItem *pSnapshotItem = pSnapshot->GetItem(Index);
	CSnapItem Item;
	Item.m_Type = pSnapshot->GetItemType(Index);
	Item.m_Id = pSnapshotItem->Id();
	Item.m_pData = pSnapshotItem->Data();
	Item.m_DataSize = pSnapshot->GetItemSize(Index);
	return Item;
}

const void *CSessionRuntime::SnapFindItem(CSessionId SessionId, int SnapId, int Type, int Id) const
{
	if(!Connection(SessionId).m_apSnapshots[SnapId])
		return nullptr;

	return Connection(SessionId).m_apSnapshots[SnapId]->m_pAltSnap->FindItem(Type, Id);
}

int CSessionRuntime::SnapNumItems(CSessionId SessionId, int SnapId) const
{
	dbg_assert(SnapId >= 0 && SnapId < NUM_SNAPSHOT_TYPES, "invalid SnapId");
	if(!Connection(SessionId).m_apSnapshots[SnapId])
		return 0;
	return Connection(SessionId).m_apSnapshots[SnapId]->m_pAltSnap->NumItems();
}

void CSessionRuntime::SnapSetStaticsize(int ItemType, int Size)
{
	for(const std::unique_ptr<CSessionSource> &pSource : m_SessionManager.Sessions())
	{
		if(pSource->Type() == ESessionSourceType::NETWORK)
			static_cast<CNetworkSessionSource &>(*pSource).SnapshotDelta(false).SetStaticsize(ItemType, Size);
		else
			static_cast<CDemoSessionSource &>(*pSource).SnapshotDelta(false).SetStaticsize(ItemType, Size);
	}
}

void CSessionRuntime::SnapSetStaticsize7(int ItemType, int Size)
{
	for(const std::unique_ptr<CSessionSource> &pSource : m_SessionManager.Sessions())
	{
		if(pSource->Type() == ESessionSourceType::NETWORK)
			static_cast<CNetworkSessionSource &>(*pSource).SnapshotDelta(true).SetStaticsize(ItemType, Size);
		else
			static_cast<CDemoSessionSource &>(*pSource).SnapshotDelta(true).SetStaticsize(ItemType, Size);
	}
}

int CSessionRuntime::UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo)
{
	CUnpacker Unpacker;
	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObjHandler *pNetObjHandler = GameClient()->GetNetObjHandler();

	int Num = pFrom->NumItems();
	for(int Index = 0; Index < Num; Index++)
	{
		const CSnapshotItem *pFromItem = pFrom->GetItem(Index);
		const int FromItemSize = pFrom->GetItemSize(Index);
		const int ItemType = pFrom->GetItemType(Index);
		const void *pData = pFromItem->Data();
		Unpacker.Reset(pData, FromItemSize);

		if(ItemType <= 0)
		{
			// Don't add extended item type descriptions, they get
			// added implicitly (== 0).
			//
			// Don't add items of unknown item types either (< 0).
			continue;
		}

		void *pSecuredData = pNetObjHandler->SecureUnpackObj(ItemType, &Unpacker);
		if(!pSecuredData)
		{
			if(g_Config.m_Debug && ItemType != UUID_UNKNOWN)
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "dropped weird object '%s' (%d), failed on '%s'", pNetObjHandler->GetObjName(ItemType), ItemType, pNetObjHandler->FailedObjOn());
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);
			}
			continue;
		}
		const int ItemSize = pNetObjHandler->GetUnpackedObjSize(ItemType);

		if(!Builder.NewItem(ItemType, pFromItem->Id(), pSecuredData, ItemSize))
		{
			return -4;
		}
	}

	return Builder.Finish(pTo);
}

void CSessionRuntime::OnDemoSnapshot(CSessionId SessionId, void *pData, int Size)
{
	CDemoSessionSource &Source = DemoSource(SessionId);
	CConnection &DemoConnection = Source.m_Connection;
	// update ticks, they could have changed
	UpdateDemoIntraTimers(SessionId);

	// create a verified and unpacked snapshot
	CSnapshotBuffer AltSnapBuffer;
	int AltSnapSize;

	if(Source.m_Sixup)
	{
		AltSnapSize = GameClient()->TranslateSnap(SessionId, &AltSnapBuffer, (CSnapshot *)pData);
		if(AltSnapSize < 0)
		{
			dbg_msg("sixup", "failed to translate snapshot. error=%d", AltSnapSize);
			return;
		}
	}
	else
	{
		AltSnapSize = UnpackAndValidateSnapshot((CSnapshot *)pData, &AltSnapBuffer);
		if(AltSnapSize < 0)
		{
			dbg_msg("client", "unpack snapshot and validate failed. error=%d", AltSnapSize);
			return;
		}
	}

	// handle snapshots after validation
	std::swap(DemoConnection.m_apSnapshots[SNAP_PREV], DemoConnection.m_apSnapshots[SNAP_CURRENT]);
	mem_copy(DemoConnection.m_apSnapshots[SNAP_CURRENT]->m_pSnap, pData, Size);
	mem_copy(DemoConnection.m_apSnapshots[SNAP_CURRENT]->m_pAltSnap, &AltSnapBuffer, AltSnapSize);

	GameClient()->OnNewSnapshot(SessionId);
}

void CSessionRuntime::OnDemoMessage(CSessionId SessionId, void *pData, int Size)
{
	CUnpacker Unpacker;
	Unpacker.Reset(pData, Size);
	CMsgPacker Packer(NETMSG_EX, true);

	// unpack msgid and system flag
	int Msg;
	bool Sys;
	CUuid Uuid;

	int Result = UnpackMessageId(&Msg, &Sys, &Uuid, &Unpacker, &Packer);
	if(Result == UNPACKMESSAGE_ERROR)
	{
		return;
	}

	if(!Sys)
		GameClient()->OnMessage(SessionId, Msg, &Unpacker);
}

void CSessionRuntime::UpdateDemoIntraTimers(CSessionId SessionId)
{
	// update timers
	CDemoSessionSource &Source = DemoSource(SessionId);
	const CDemoPlayer::CPlaybackInfo *pInfo = Source.m_DemoPlayer.Info();
	CConnection &DemoConnection = Source.m_Connection;
	DemoConnection.m_CurGameTick = pInfo->m_Info.m_CurrentTick;
	DemoConnection.m_PrevGameTick = pInfo->m_PreviousTick;
	DemoConnection.m_GameIntraTick = pInfo->m_IntraTick;
	DemoConnection.m_GameTickTime = pInfo->m_TickTime;
	DemoConnection.m_GameIntraTickSincePrev = pInfo->m_IntraTickSincePrev;
}

bool CSessionRuntime::UpdateDemoPlayer(CSessionId SessionId)
{
	CDemoPlayer &Player = DemoSource(SessionId).m_DemoPlayer;
	if(!Player.IsPlaying())
		return false;
#if defined(CONF_VIDEORECORDER)
	if(Player.Video())
		Player.Video()->NextVideoFrame();
#endif
	Player.Update();
	UpdateDemoIntraTimers(SessionId);
	return true;
}
