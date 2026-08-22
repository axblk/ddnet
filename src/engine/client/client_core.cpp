/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "client_core.h"

#include <base/dbg.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/video.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <game/localization.h>

#include <algorithm>
#include <cmath>
#include <utility>

void FormatMapDownloadFilename(const char *pName, const std::optional<SHA256_DIGEST> &Sha256, int Crc, bool Temp, char *pBuffer, int BufferSize)
{
	char aSuffix[32];
	if(Temp)
	{
		IStorage::FormatTmpPath(aSuffix, sizeof(aSuffix), "");
	}
	else
	{
		str_copy(aSuffix, ".map");
	}

	if(Sha256.has_value())
	{
		char aSha256[SHA256_MAXSTRSIZE];
		sha256_str(Sha256.value(), aSha256, sizeof(aSha256));
		str_format(pBuffer, BufferSize, "downloadedmaps/%s_%s%s", pName, aSha256, aSuffix);
	}
	else
	{
		str_format(pBuffer, BufferSize, "downloadedmaps/%s_%08x%s", pName, Crc, aSuffix);
	}
}

IClient::CSnapItem CClientCore::SnapGetItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Index) const
{
	dbg_assert(SnapId >= 0 && SnapId < NUM_SNAPSHOT_TYPES, "invalid SnapId");
	const CSnapshot *pSnapshot = Connection(SessionId, StreamId).m_apSnapshots[SnapId]->m_pAltSnap;
	const CSnapshotItem *pSnapshotItem = pSnapshot->GetItem(Index);
	CSnapItem Item;
	Item.m_Type = pSnapshot->GetItemType(Index);
	Item.m_Id = pSnapshotItem->Id();
	Item.m_pData = pSnapshotItem->Data();
	Item.m_DataSize = pSnapshot->GetItemSize(Index);
	return Item;
}

const void *CClientCore::SnapFindItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Type, int Id) const
{
	if(!Connection(SessionId, StreamId).m_apSnapshots[SnapId])
		return nullptr;

	return Connection(SessionId, StreamId).m_apSnapshots[SnapId]->m_pAltSnap->FindItem(Type, Id);
}

int CClientCore::SnapNumItems(CSessionId SessionId, CStreamId StreamId, int SnapId) const
{
	dbg_assert(SnapId >= 0 && SnapId < NUM_SNAPSHOT_TYPES, "invalid SnapId");
	if(!Connection(SessionId, StreamId).m_apSnapshots[SnapId])
		return 0;
	return Connection(SessionId, StreamId).m_apSnapshots[SnapId]->m_pAltSnap->NumItems();
}

void CClientCore::SnapSetStaticsize(int ItemType, int Size)
{
	m_avSnapshotStaticSizes[false].emplace_back(ItemType, Size);
	for(CSessionId SessionId : m_SessionManager.SessionIds())
		SessionSource(SessionId).SnapshotDelta(false).SetStaticsize(ItemType, Size);
}

void CClientCore::SnapSetStaticsize7(int ItemType, int Size)
{
	m_avSnapshotStaticSizes[true].emplace_back(ItemType, Size);
	for(CSessionId SessionId : m_SessionManager.SessionIds())
		SessionSource(SessionId).SnapshotDelta(true).SetStaticsize(ItemType, Size);
}

int64_t CClientCore::DemoPlaybackTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).DemoPlayer().Video())
		return pVideo->Time();
#endif
	return time_get();
}

float CClientCore::DemoPlaybackLocalTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).DemoPlayer().Video())
		return pVideo->LocalTime();
#endif
	return LocalTime();
}

int CClientCore::GetPredictionTime(CSessionId SessionId, CStreamId StreamId)
{
	int64_t Now = time_get();
	return (int)((Connection(SessionId, StreamId).m_PredictedTime.Get(Now) - Connection(SessionId, StreamId).m_GameTime.Get(Now)) * 1000 / (float)time_freq());
}

int CClientCore::GetPredictionTick(CSessionId SessionId, CStreamId StreamId)
{
	int PredictionTick = GetPredictionTime(SessionId, StreamId) * GameTickSpeed() / 1000.0f;

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
		return PredGameTick(SessionId, StreamId);

	PredictionTick = PredGameTick(SessionId, StreamId) - PredictionMin;

	if(PredictionTick < GameTick(SessionId, StreamId) + 1)
	{
		PredictionTick = GameTick(SessionId, StreamId) + 1;
	}
	return PredictionTick;
}

void CClientCore::GetSmoothTick(CSessionId SessionId, CStreamId StreamId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount)
{
	int64_t GameTime = Connection(SessionId, StreamId).m_GameTime.Get(Now);
	int64_t PredTime = Connection(SessionId, StreamId).m_PredictedTime.Get(Now);
	int64_t SmoothTime = std::clamp(GameTime + (int64_t)(MixAmount * (PredTime - GameTime)), GameTime, PredTime);

	*pSmoothTick = (int)(SmoothTime * GameTickSpeed() / time_freq()) + 1;
	*pSmoothIntraTick = (SmoothTime - (*pSmoothTick - 1) * time_freq() / GameTickSpeed()) / (float)(time_freq() / GameTickSpeed());
}

void CClientCore::AddWarning(const SWarning &Warning)
{
	const std::unique_lock<std::mutex> Lock(m_WarningsMutex);
	m_vWarnings.emplace_back(Warning);
}

std::optional<SWarning> CClientCore::CurrentWarning()
{
	const std::unique_lock<std::mutex> Lock(m_WarningsMutex);
	if(m_vWarnings.empty())
	{
		return std::nullopt;
	}
	else
	{
		std::optional<SWarning> Result = std::make_optional(m_vWarnings[0]);
		m_vWarnings.erase(m_vWarnings.begin());
		return Result;
	}
}

int CClientCore::MaxLatencyTicks(CSessionId SessionId) const
{
	return GameTickSpeed() + (PredictionMargin(SessionId) * GameTickSpeed()) / 1000;
}

int CClientCore::PredictionMargin(CSessionId SessionId) const
{
	return SessionSource(SessionId).SyncWeaponInput() ? g_Config.m_ClPredictionMargin : 10;
}

void CClientCore::SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger)
{
	m_pFileLogger = pFileLogger;
	m_pStdoutLogger = pStdoutLogger;
}

int CClientCore::UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo)
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

void CClientCore::OnDemoPlayerSnapshot(CDemoPlayer &DemoPlayer, void *pData, int Size)
{
	const CSessionId SessionId = FindDemoSessionId(DemoPlayer);
	dbg_assert(SessionId.IsValid(), "missing demo player session");
	// update ticks, they could have changed
	const CDemoPlayer::CPlaybackInfo *pInfo = DemoPlayer.Info();
	CConnection &DemoConnection = Connection(SessionId, CONN_MAIN);
	DemoConnection.m_CurGameTick = pInfo->m_Info.m_CurrentTick;
	DemoConnection.m_PrevGameTick = pInfo->m_PreviousTick;

	// create a verified and unpacked snapshot
	CSnapshotBuffer AltSnapBuffer;
	int AltSnapSize;

	if(IsSixup(SessionId))
	{
		AltSnapSize = GameClient()->TranslateSnap(SessionId, &AltSnapBuffer, (CSnapshot *)pData, PrimaryStreamId(SessionId));
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

	GameClient()->OnNewSnapshot(SessionId, PrimaryStreamId(SessionId));
}

void CClientCore::OnDemoPlayerMessage(CDemoPlayer &DemoPlayer, void *pData, int Size)
{
	const CSessionId SessionId = FindDemoSessionId(DemoPlayer);
	dbg_assert(SessionId.IsValid(), "missing demo player session");
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
		GameClient()->OnMessage(SessionId, Msg, &Unpacker, PrimaryStreamId(SessionId));
}

CSessionId CClientCore::FindDemoSessionId(const CDemoPlayer &DemoPlayer) const
{
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::DEMO && &DemoSource(SessionId).DemoPlayer() == &DemoPlayer)
			return SessionId;
	}
	return {};
}

void CClientCore::UpdateDemoIntraTimers(CDemoPlayer &DemoPlayer)
{
	const CSessionId SessionId = FindDemoSessionId(DemoPlayer);
	dbg_assert(SessionId.IsValid(), "missing demo player session");
	// update timers
	const CDemoPlayer::CPlaybackInfo *pInfo = DemoPlayer.Info();
	CConnection &DemoConnection = Connection(SessionId, CONN_MAIN);
	DemoConnection.m_CurGameTick = pInfo->m_Info.m_CurrentTick;
	DemoConnection.m_PrevGameTick = pInfo->m_PreviousTick;
	DemoConnection.m_GameIntraTick = pInfo->m_IntraTick;
	DemoConnection.m_GameTickTime = pInfo->m_TickTime;
	DemoConnection.m_GameIntraTickSincePrev = pInfo->m_IntraTickSincePrev;
}

void CClientCore::UpdateDemoSession(CSessionId SessionId)
{
	CDemoSessionSource &Source = DemoSource(SessionId);
	CDemoPlayer &Player = Source.DemoPlayer();
	if(Source.State() == ESessionState::READY)
	{
		if(Player.IsPlaying())
		{
#if defined(CONF_VIDEORECORDER)
			if(Player.Video())
				Player.Video()->NextVideoFrame();
#endif

			Player.Update();

			// update timers
			const CDemoPlayer::CPlaybackInfo *pInfo = Player.Info();
			CConnection &DemoConnection = Connection(SessionId, CONN_MAIN);
			DemoConnection.m_CurGameTick = pInfo->m_Info.m_CurrentTick;
			DemoConnection.m_PrevGameTick = pInfo->m_PreviousTick;
			DemoConnection.m_GameIntraTick = pInfo->m_IntraTick;
			DemoConnection.m_GameTickTime = pInfo->m_TickTime;
		}
		else
		{
			// Disconnect when demo playback stopped, either due to playback error
			// or because the end of the demo was reached when rendering it.
			m_SessionManager.Close(SessionId, Player.ErrorMessage());
			if(Player.ErrorMessage()[0] != '\0' && m_SessionManager.FocusedId() == SessionId)
			{
				SWarning Warning(Localize("Error playing demo"), Player.ErrorMessage());
				Warning.m_AutoHide = false;
				AddWarning(Warning);
			}
		}
	}
}

bool CClientCore::IsOnline() const
{
	const CGameSession *pSession = m_SessionManager.Focused();
	return pSession && pSession->Source().Type() == ESessionSourceType::NETWORK && pSession->State() == ESessionState::READY;
}

bool CClientCore::IsDemoPlayback() const
{
	const CGameSession *pSession = m_SessionManager.Focused();
	return pSession && pSession->Source().Type() == ESessionSourceType::DEMO && pSession->State() == ESessionState::READY;
}

void CClientCore::SetState(EClientState State)
{
	if(m_State == IClient::STATE_QUITTING || m_State == IClient::STATE_RESTARTING)
		return;
	FocusSessionForState(State);
	SetFocusedState(State, true);
}

void CClientCore::SetFocusedState(EClientState State, bool ResetSession)
{
	const bool StateChanged = m_State != State;

	if(StateChanged && g_Config.m_Debug)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "state change. last=%d current=%d", m_State, State);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client", aBuf);
	}

	const EClientState OldState = m_State;
	if(StateChanged && ResetSession && State < IClient::STATE_ONLINE)
		GameClient()->OnSessionClosed(m_SessionManager.FocusedId());

	CGameSession *pFocusedSession = m_SessionManager.Focused();
	if(pFocusedSession)
	{
		switch(State)
		{
		case IClient::STATE_OFFLINE:
			pFocusedSession->Source().SetState(ESessionState::OFFLINE);
			break;
		case IClient::STATE_CONNECTING:
			pFocusedSession->Source().SetState(ESessionState::CONNECTING);
			break;
		case IClient::STATE_LOADING:
			pFocusedSession->Source().SetState(ESessionState::LOADING_MAP);
			break;
		case IClient::STATE_ONLINE:
		case IClient::STATE_DEMOPLAYBACK:
			pFocusedSession->Source().SetState(ESessionState::READY);
			break;
		case IClient::STATE_QUITTING:
		case IClient::STATE_RESTARTING:
			break;
		}
	}
	if(!StateChanged)
		return;
	m_State = State;

	m_StateStartTime = time_get();
	GameClient()->OnStateChange(m_State, OldState);

	OnStateChanged(m_State, OldState);
}

void CClientCore::FocusSession(CSessionId SessionId)
{
	if(!m_SessionManager.SetFocused(SessionId))
		return;
	const CGameSession *pSession = m_SessionManager.Find(SessionId);
	dbg_assert(pSession != nullptr, "missing focused session");
	EClientState State = IClient::STATE_OFFLINE;
	switch(pSession->State())
	{
	case ESessionState::CONNECTING:
		State = IClient::STATE_CONNECTING;
		break;
	case ESessionState::LOADING_MAP:
		State = IClient::STATE_LOADING;
		break;
	case ESessionState::READY:
		State = pSession->Source().Type() == ESessionSourceType::DEMO ? IClient::STATE_DEMOPLAYBACK : IClient::STATE_ONLINE;
		break;
	case ESessionState::OFFLINE:
	case ESessionState::STOPPING:
	case ESessionState::ERROR:
		break;
	}
	SetFocusedState(State, false);
	GameClient()->OnSessionFocused(SessionId);
}

const char *CClientCore::LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	static char s_aErrorMsg[128];

	if(SessionSource(SessionId).State() != ESessionState::LOADING_MAP || GameClient()->Map(SessionId)->IsLoaded())
		GameClient()->OnSessionClosed(SessionId);
	if(m_SessionManager.FocusedId() == SessionId)
		SetFocusedState(IClient::STATE_LOADING, false);
	else
		SessionSource(SessionId).SetState(ESessionState::LOADING_MAP);
	if(m_SessionManager.FocusedId() == SessionId)
	{
		SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_LOADING_MAP);
		if((bool)m_LoadingCallback)
			m_LoadingCallback(IClient::LOADING_CALLBACK_DETAIL_MAP);
	}

	OnMapLoadStarted(SessionId);

	// Unload the current map and reset all snapshots before loading a new map,
	// because the snapshots are only valid for the old map.
	IMap *pMap = GameClient()->Map(SessionId);
	for(const CStreamId StreamId : SessionSource(SessionId).StreamIds())
		Connection(SessionId, StreamId).ResetSnapshots();
	GameClient()->InvalidateSnapshot(SessionId);

	if(!pMap->Load(pName, Storage(), pFilename, IStorage::TYPE_ALL))
	{
		str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "map '%s' not found", pFilename);
		return s_aErrorMsg;
	}

	if(WantedSha256.has_value() && pMap->Sha256() != WantedSha256.value())
	{
		char aWanted[SHA256_MAXSTRSIZE];
		char aGot[SHA256_MAXSTRSIZE];
		sha256_str(WantedSha256.value(), aWanted, sizeof(aWanted));
		sha256_str(pMap->Sha256(), aGot, sizeof(aWanted));
		str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "map differs from the server. %s != %s", aGot, aWanted);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", s_aErrorMsg);
		pMap->Unload();
		return s_aErrorMsg;
	}

	// Only check CRC if we don't have the secure SHA256.
	if(!WantedSha256.has_value() && pMap->Crc() != WantedCrc)
	{
		str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "map differs from the server. %08x != %08x", pMap->Crc(), WantedCrc);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", s_aErrorMsg);
		pMap->Unload();
		return s_aErrorMsg;
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "loaded map '%s'", pFilename);
	m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);

	return nullptr;
}

const char *CClientCore::LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc)
{
	char aBuf[512];
	char aWanted[SHA256_MAXSTRSIZE + 16];
	aWanted[0] = 0;
	if(WantedSha256.has_value())
	{
		char aWantedSha256[SHA256_MAXSTRSIZE];
		sha256_str(WantedSha256.value(), aWantedSha256, sizeof(aWantedSha256));
		str_format(aWanted, sizeof(aWanted), "sha256=%s ", aWantedSha256);
	}
	str_format(aBuf, sizeof(aBuf), "loading map, map=%s wanted %scrc=%08x", pMapName, aWanted, WantedCrc);
	m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);

	// try the normal maps folder
	str_format(aBuf, sizeof(aBuf), "maps/%s.map", pMapName);
	const char *pError = LoadMap(SessionId, pMapName, aBuf, WantedSha256, WantedCrc);
	if(!pError)
		return nullptr;

	// try the downloaded maps
	FormatMapDownloadFilename(pMapName, WantedSha256, WantedCrc, false, aBuf, sizeof(aBuf));
	pError = LoadMap(SessionId, pMapName, aBuf, WantedSha256, WantedCrc);
	if(!pError)
		return nullptr;

	// backward compatibility with old names
	if(WantedSha256.has_value())
	{
		FormatMapDownloadFilename(pMapName, std::nullopt, WantedCrc, false, aBuf, sizeof(aBuf));
		pError = LoadMap(SessionId, pMapName, aBuf, WantedSha256, WantedCrc);
		if(!pError)
			return nullptr;
	}

	// search for the map within subfolders
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "%s.map", pMapName);
	if(Storage()->FindFile(aFilename, "maps", IStorage::TYPE_ALL, aBuf, sizeof(aBuf)))
	{
		pError = LoadMap(SessionId, pMapName, aBuf, WantedSha256, WantedCrc);
		if(!pError)
			return nullptr;
	}

	static char s_aErrorMsg[256];
	str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "Could not find map '%s'", pMapName);
	return s_aErrorMsg;
}
