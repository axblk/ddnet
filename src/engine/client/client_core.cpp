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
#include <cstdlib>
#include <memory>
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

float CClientCore::DemoPlaybackLocalTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).m_DemoPlayer.Video())
		return pVideo->LocalTime();
#endif
	return LocalTime();
}

IClient::EClientState CClientCore::State() const
{
	if(m_ExitState.has_value())
		return *m_ExitState;
	const CSessionId SessionId = FocusedSessionId();
	switch(SessionState(SessionId))
	{
	case ESessionState::CONNECTING:
		return IClient::STATE_CONNECTING;
	case ESessionState::LOADING_MAP:
		return IClient::STATE_LOADING;
	case ESessionState::READY:
		return SessionType(SessionId) == ESessionSourceType::DEMO ? IClient::STATE_DEMOPLAYBACK : IClient::STATE_ONLINE;
	case ESessionState::OFFLINE:
	case ESessionState::STOPPING:
	case ESessionState::ERROR:
		break;
	}
	return IClient::STATE_OFFLINE;
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
	CancelMapSearch(SessionId);
	CDemoSessionSource &Source = DemoSource(SessionId);
	CDemoPlayer &Player = Source.m_DemoPlayer;
	if(Player.Load(Storage(), pFilename, StorageType))
		return Player.ErrorMessage();

	Source.m_Sixup = Player.IsSixup();
	const CMapInfo *pMapInfo = Player.GetMapInfo();

	// setup current server info
	CServerInfo &DemoServerInfo = Source.m_ServerInfo;
	DemoServerInfo = {};
	str_copy(DemoServerInfo.m_aMap, pMapInfo->m_aName);
	DemoServerInfo.m_MapCrc = pMapInfo->m_Crc;
	DemoServerInfo.m_MapSize = pMapInfo->m_Size;

	// The map a demo carries is the one it was recorded on, so nothing is
	// looked for, read or fetched besides it.
	if(BeginMapLoad(SessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc))
		return nullptr;
	if(unsigned char *pMapData = Player.GetMapData(Storage()))
	{
		std::vector<uint8_t> vData(pMapData, pMapData + pMapInfo->m_Size);
		free(pMapData);
		char aPath[IO_MAX_PATH_LENGTH];
		FormatMapDownloadFilename(pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc, false, aPath, sizeof(aPath));
		if(LoadMapData(SessionId, pMapInfo->m_aName, aPath, std::move(vData), pMapInfo->m_Sha256, pMapInfo->m_Crc) == nullptr)
			return nullptr;
	}
	if(LoadMapSearch(SessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc) == EMapSearch::NOT_FOUND)
		return MapNotFoundError(pMapInfo->m_aName);
	return nullptr;
}

void CClientCore::SetState(EClientState State)
{
	if(m_ExitState.has_value())
		return;
	if(State == IClient::STATE_CONNECTING || State == IClient::STATE_ONLINE)
		m_SessionManager.SetFocused(m_NetworkSessionId);
	else if(State == IClient::STATE_DEMOPLAYBACK)
		m_SessionManager.SetFocused(m_DemoSessionId);
	SetFocusedState(State, true);
}

void CClientCore::SetFocusedState(EClientState State, bool ResetSession)
{
	if(State == IClient::STATE_QUITTING || State == IClient::STATE_RESTARTING)
	{
		m_ExitState = State;
		AnnounceState();
		return;
	}

	if(ResetSession && State != m_AnnouncedState && State < IClient::STATE_ONLINE)
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
	AnnounceState();
}

void CClientCore::AnnounceState()
{
	const EClientState State = this->State();
	if(State == m_AnnouncedState)
		return;
	const EClientState OldState = m_AnnouncedState;
	if(g_Config.m_Debug)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "state change. last=%d current=%d", OldState, State);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client", aBuf);
	}
	m_AnnouncedState = State;
	m_StateStartTime = time_get();
	GameClient()->OnStateChange(State, OldState);
	OnStateChanged(State, OldState);
}

void CClientCore::FocusSession(CSessionId SessionId)
{
	if(!m_SessionManager.SetFocused(SessionId))
		return;
	AnnounceState();
	GameClient()->OnSessionFocused(SessionId);
}

bool CClientCore::BeginMapLoad(CSessionId SessionId, const char *pName, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	if(SessionSource(SessionId).State() != ESessionState::LOADING_MAP || GameClient()->Map(SessionId)->IsLoaded())
		GameClient()->OnSessionClosed(SessionId);
	if(FocusedSessionId() == SessionId)
	{
		SetFocusedState(IClient::STATE_LOADING, false);
		SetLoadingStateDetail(IClient::ELoadingStateDetail::LOADING_MAP);
		if((bool)m_LoadingCallback)
			m_LoadingCallback(IClient::ELoadingCallbackDetail::MAP);
	}
	else
	{
		SessionSource(SessionId).SetState(ESessionState::LOADING_MAP);
	}

	// Unload the current map and reset all snapshots before loading a new map,
	// because the snapshots are only valid for the old map.
	SessionSource(SessionId).m_Connection.ResetSnapshots();
	GameClient()->InvalidateSnapshot(SessionId);
	if(GameClient()->ShareLoadedMap(SessionId, pName, WantedSha256, WantedCrc))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "shared loaded map '%s'", pName);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);
		return true;
	}
	return false;
}

const char *CClientCore::LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	if(BeginMapLoad(SessionId, pName, WantedSha256, WantedCrc))
		return nullptr;
	return LoadMapFile(SessionId, pName, pFilename, WantedSha256, WantedCrc);
}

const char *CClientCore::LoadMapFile(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	static char s_aErrorMsg[128];
	if(!GameClient()->Map(SessionId)->Load(pName, Storage(), pFilename, IStorage::TYPE_ALL))
	{
		str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "map '%s' not found", pFilename);
		return s_aErrorMsg;
	}
	return CheckLoadedMap(SessionId, pFilename, WantedSha256, WantedCrc);
}

const char *CClientCore::LoadMapData(CSessionId SessionId, const char *pName, const char *pFilename, std::vector<uint8_t> &&vData, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	static char s_aErrorMsg[128];
	if(!GameClient()->Map(SessionId)->LoadFromMemory(pName, std::move(vData), pFilename))
	{
		str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "map '%s' could not be read", pFilename);
		return s_aErrorMsg;
	}
	return CheckLoadedMap(SessionId, pFilename, WantedSha256, WantedCrc);
}

const char *CClientCore::CheckLoadedMap(CSessionId SessionId, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	static char s_aErrorMsg[128];
	IMap *pMap = GameClient()->Map(SessionId);
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

const char *CClientCore::MapNotFoundError(const char *pMapName)
{
	static char s_aErrorMsg[256];
	str_format(s_aErrorMsg, sizeof(s_aErrorMsg), "Could not find map '%s'", pMapName);
	return s_aErrorMsg;
}

CClientCore::EMapSearch CClientCore::LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc)
{
	CancelMapSearch(SessionId);
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

	if(BeginMapLoad(SessionId, pMapName, WantedSha256, WantedCrc))
		return EMapSearch::LOADED;
	CMapSearch Search;
	Search.m_SessionId = SessionId;
	Search.m_Name = pMapName;
	Search.m_WantedSha256 = WantedSha256;
	Search.m_WantedCrc = WantedCrc;
	const EMapSearch Result = ContinueMapSearch(Search);
	if(Result == EMapSearch::FETCHING)
		m_vMapSearches.push_back(std::move(Search));
	return Result;
}

CClientCore::EMapSearch CClientCore::ContinueMapSearch(CMapSearch &Search)
{
	const char *pName = Search.m_Name.c_str();
	while(true)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		switch(Search.m_NextStep++)
		{
		case 0:
			// the normal maps folder
			str_format(aPath, sizeof(aPath), "maps/%s.map", pName);
			break;
		case 1:
			// the downloaded maps
			FormatMapDownloadFilename(pName, Search.m_WantedSha256, Search.m_WantedCrc, false, aPath, sizeof(aPath));
			break;
		case 2:
			// backward compatibility with old names
			if(!Search.m_WantedSha256.has_value())
				continue;
			FormatMapDownloadFilename(pName, std::nullopt, Search.m_WantedCrc, false, aPath, sizeof(aPath));
			break;
		case 3:
		{
			// the folders below the maps folder
			char aFilename[IO_MAX_PATH_LENGTH];
			str_format(aFilename, sizeof(aFilename), "%s.map", pName);
			if(!Storage()->FindFile(aFilename, "maps", IStorage::TYPE_ALL, aPath, sizeof(aPath)))
				continue;
			break;
		}
		default:
			return EMapSearch::NOT_FOUND;
		}

		// A map that is fetched rather than read is not waited for. When the
		// storage knows what it is, it is only fetched if it is the one.
		char aUrl[IO_MAX_PATH_LENGTH];
		if(Storage()->FetchUrl(aPath, IStorage::TYPE_ALL, aUrl, sizeof(aUrl)))
		{
			SHA256_DIGEST Sha256;
			unsigned Crc;
			if(Storage()->FetchedFileDigests(aPath, IStorage::TYPE_ALL, &Sha256, &Crc) &&
				(Search.m_WantedSha256.has_value() ? Sha256 != Search.m_WantedSha256.value() : Crc != Search.m_WantedCrc))
			{
				log_debug("client", "'%s' is not the map wanted", aPath);
				continue;
			}
			log_info("client", "fetching map '%s'", aPath);
			Search.m_Path = aPath;
			Search.m_Resource = GameClient()->AssetLoader().Load(std::make_shared<CFileAssetJob>(Storage(), aPath, IStorage::TYPE_ALL), EAssetPriority::URGENT);
			return EMapSearch::FETCHING;
		}
		if(LoadMapFile(Search.m_SessionId, pName, aPath, Search.m_WantedSha256, Search.m_WantedCrc) == nullptr)
			return EMapSearch::LOADED;
	}
}

void CClientCore::UpdateMapSearches()
{
	if(m_vMapSearches.empty())
		return;
	std::vector<std::pair<CSessionId, bool>> vDone;
	for(auto It = m_vMapSearches.begin(); It != m_vMapSearches.end();)
	{
		CMapSearch &Search = *It;
		if(!Search.m_Resource.IsFinished())
		{
			++It;
			continue;
		}
		EMapSearch Result;
		if(Search.m_Resource.IsReady())
		{
			std::vector<uint8_t> vData = Search.m_Resource.Result().TakeBytes();
			Search.m_Resource.Reset();
			Result = LoadMapData(Search.m_SessionId, Search.m_Name.c_str(), Search.m_Path.c_str(), std::move(vData), Search.m_WantedSha256, Search.m_WantedCrc) == nullptr ? EMapSearch::LOADED : ContinueMapSearch(Search);
		}
		else
		{
			log_error("client", "map '%s' could not be fetched", Search.m_Path.c_str());
			Search.m_Resource.Reset();
			Result = ContinueMapSearch(Search);
		}
		if(Result == EMapSearch::FETCHING)
		{
			++It;
			continue;
		}
		vDone.emplace_back(Search.m_SessionId, Result == EMapSearch::LOADED);
		It = m_vMapSearches.erase(It);
	}
	// Told only now, because what they do may start another search.
	for(const auto &[SessionId, Loaded] : vDone)
		OnMapSearchDone(SessionId, Loaded);
}

void CClientCore::CancelMapSearch(CSessionId SessionId)
{
	m_vMapSearches.erase(std::remove_if(m_vMapSearches.begin(), m_vMapSearches.end(), [SessionId](const CMapSearch &Search) { return Search.m_SessionId == SessionId; }), m_vMapSearches.end());
}

bool CClientCore::IsMapSearchPending(CSessionId SessionId) const
{
	return std::any_of(m_vMapSearches.begin(), m_vMapSearches.end(), [SessionId](const CMapSearch &Search) { return Search.m_SessionId == SessionId; });
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
