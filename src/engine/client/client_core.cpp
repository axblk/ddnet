/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "client_core.h"

#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/video.h>
#include <engine/storage.h>

#include <game/localization.h>

#include <algorithm>
#include <cmath>

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

float CClientCore::DemoPlaybackLocalTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).m_DemoPlayer.Video())
		return pVideo->LocalTime();
#endif
	return LocalTime();
}

bool CClientCore::IsOnline() const
{
	return m_NetworkSessionId.IsValid() && FocusedSessionId() == m_NetworkSessionId && SessionState(m_NetworkSessionId) == ESessionState::READY;
}

bool CClientCore::IsDemoPlayback() const
{
	return m_DemoSessionId.IsValid() && FocusedSessionId() == m_DemoSessionId && SessionState(m_DemoSessionId) == ESessionState::READY;
}

const char *CClientCore::LoadDemo(CSessionId SessionId, const char *pFilename, int StorageType)
{
	CDemoSessionSource &Source = DemoSource(SessionId);
	CDemoPlayer &Player = Source.m_DemoPlayer;
	if(Player.Load(Storage(), m_pConsole, pFilename, StorageType))
		return Player.ErrorMessage();

	Source.m_Sixup = Player.IsSixup();

	// load map
	const CMapInfo *pMapInfo = Player.GetMapInfo();
	const char *pError = LoadMapSearch(SessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc);
	if(pError)
	{
		if(!Player.ExtractMap(Storage()))
			return pError;
		pError = LoadMapSearch(SessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc);
		if(pError)
			return pError;
	}

	// setup current server info
	CServerInfo &DemoServerInfo = Source.m_ServerInfo;
	DemoServerInfo = {};
	str_copy(DemoServerInfo.m_aMap, pMapInfo->m_aName);
	DemoServerInfo.m_MapCrc = pMapInfo->m_Crc;
	DemoServerInfo.m_MapSize = pMapInfo->m_Size;
	return nullptr;
}

void CClientCore::SetState(EClientState State)
{
	if(m_State == IClient::STATE_QUITTING || m_State == IClient::STATE_RESTARTING)
		return;
	if(State == IClient::STATE_CONNECTING || State == IClient::STATE_ONLINE)
		m_SessionManager.SetFocused(m_NetworkSessionId);
	else if(State == IClient::STATE_DEMOPLAYBACK)
		m_SessionManager.SetFocused(m_DemoSessionId);
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

	CSessionSource &FocusedSession = *m_SessionManager.Focused();
	switch(State)
	{
	case IClient::STATE_OFFLINE:
		FocusedSession.SetState(ESessionState::OFFLINE);
		break;
	case IClient::STATE_CONNECTING:
		FocusedSession.SetState(ESessionState::CONNECTING);
		break;
	case IClient::STATE_LOADING:
		FocusedSession.SetState(ESessionState::LOADING_MAP);
		break;
	case IClient::STATE_ONLINE:
	case IClient::STATE_DEMOPLAYBACK:
		FocusedSession.SetState(ESessionState::READY);
		break;
	case IClient::STATE_QUITTING:
	case IClient::STATE_RESTARTING:
		break;
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
	const CSessionSource &Session = SessionSource(SessionId);
	EClientState State = IClient::STATE_OFFLINE;
	switch(Session.State())
	{
	case ESessionState::CONNECTING:
		State = IClient::STATE_CONNECTING;
		break;
	case ESessionState::LOADING_MAP:
		State = IClient::STATE_LOADING;
		break;
	case ESessionState::READY:
		State = Session.Type() == ESessionSourceType::DEMO ? IClient::STATE_DEMOPLAYBACK : IClient::STATE_ONLINE;
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
	if(FocusedSessionId() == SessionId)
	{
		SetFocusedState(IClient::STATE_LOADING, false);
		SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_LOADING_MAP);
		if((bool)m_LoadingCallback)
			m_LoadingCallback(IClient::LOADING_CALLBACK_DETAIL_MAP);
	}
	else
	{
		SessionSource(SessionId).SetState(ESessionState::LOADING_MAP);
	}

	// Unload the current map and reset all snapshots before loading a new map,
	// because the snapshots are only valid for the old map.
	IMap *pMap = GameClient()->Map(SessionId);
	CSessionSourceBase &Source = SessionSource(SessionId);
	if(Source.Type() == ESessionSourceType::NETWORK)
	{
		for(CConnection &Connection : static_cast<CNetworkSessionSource &>(Source).m_aConnections)
			Connection.ResetSnapshots();
	}
	else
		static_cast<CDemoSessionSource &>(Source).m_Connection.ResetSnapshots();
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

void CClientCore::SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger)
{
	m_pFileLogger = pFileLogger;
	m_pStdoutLogger = pStdoutLogger;
}
