/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "server.h"

#include "databases/connection.h"
#include "databases/connection_pool.h"
#include "register.h"

#include <base/bytes.h>
#include <base/fs.h>
#include <base/io.h>
#include <base/logger.h>
#include <base/secure.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/http.h>
#include <engine/map.h>
#include <engine/server.h>
#include <engine/server/authmanager.h>
#include <engine/shared/compression.h>
#include <engine/shared/config.h>
#include <engine/shared/console.h>
#include <engine/shared/demo.h>
#include <engine/shared/econ.h>
#include <engine/shared/fifo.h>
#include <engine/shared/filecollection.h>
#include <engine/shared/game_wire.h>
#include <engine/shared/host_lookup.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/linereader.h>
#include <engine/shared/masterserver.h>
#include <engine/shared/netban.h>
#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol7.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/rust_version.h>
#include <engine/shared/serverinfo.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/websockets.h>
#include <engine/storage.h>

#include <game/version.h>

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

#if defined(CONF_PLATFORM_ANDROID)
extern std::vector<std::string> FetchAndroidServerCommandQueue();
#endif

void CServerBan::InitServerBan(IConsole *pConsole, IStorage *pStorage, CServer *pServer)
{
	CNetBan::Init(pConsole, pStorage);

	m_pServer = pServer;

	Console()->Register("ban", "s[ip|id] ?i[minutes] r[reason]", CFGFLAG_SERVER | CFGFLAG_STORE, ConBanExt, this, "Ban player with ip/client id for x minutes for any reason");
	Console()->Register("ban_region", "s[region] s[ip|id] ?i[minutes] r[reason]", CFGFLAG_SERVER | CFGFLAG_STORE, ConBanRegion, this, "Ban player in a region");
	Console()->Register("ban_region_range", "s[region] s[first ip] s[last ip] ?i[minutes] r[reason]", CFGFLAG_SERVER | CFGFLAG_STORE, ConBanRegionRange, this, "Ban range in a region");
}

template<class T>
int CServerBan::BanExt(T *pBanPool, const typename T::CDataType *pData, int Seconds, const char *pReason, bool VerbatimReason)
{
	// validate address
	if(Server()->m_RconClientId >= 0 && Server()->m_RconClientId < MAX_CLIENTS &&
		Server()->m_aClients[Server()->m_RconClientId].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(NetMatch(pData, Server()->ClientAddr(Server()->m_RconClientId)))
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (you can't ban yourself)");
			return -1;
		}

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(i == Server()->m_RconClientId || Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(Server()->GetAuthedState(i) >= Server()->m_RconAuthLevel && NetMatch(pData, Server()->ClientAddr(i)))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (command denied)");
				return -1;
			}
		}
	}
	else if(Server()->m_RconClientId == IServer::RCON_CID_VOTE)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(Server()->IsRconAuthed(i) && NetMatch(pData, Server()->ClientAddr(i)))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (command denied)");
				return -1;
			}
		}
	}

	int Result = Ban(pBanPool, pData, Seconds, pReason, VerbatimReason);
	if(Result != 0)
		return Result;

	// drop banned clients
	typename T::CDataType Data = *pData;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
			continue;

		if(NetMatch(&Data, Server()->ClientAddr(i)))
		{
			CNetHash NetHash(&Data);
			char aBuf[256];
			MakeBanInfo(pBanPool->Find(&Data, &NetHash), aBuf, sizeof(aBuf), MSGTYPE_PLAYER);
			Server()->DropClient(i, aBuf);
		}
	}

	return Result;
}

int CServerBan::BanAddr(const NETADDR *pAddr, int Seconds, const char *pReason, bool VerbatimReason)
{
	return BanExt(&m_BanAddrPool, pAddr, Seconds, pReason, VerbatimReason);
}

int CServerBan::BanRange(const CNetRange *pRange, int Seconds, const char *pReason)
{
	if(pRange->IsValid())
		return BanExt(&m_BanRangePool, pRange, Seconds, pReason, true);

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban failed (invalid range)");
	return -1;
}

void CServerBan::ConBanExt(IConsole::IResult *pResult, void *pUser)
{
	CServerBan *pThis = static_cast<CServerBan *>(pUser);

	const char *pStr = pResult->GetString(0);
	int Minutes = pResult->NumArguments() > 1 ? std::clamp(pResult->GetInteger(1), 0, 525600) : 10;
	const char *pReason = pResult->NumArguments() > 2 ? pResult->GetString(2) : "Follow the server rules. Type /rules into the chat.";

	if(str_isallnum(pStr))
	{
		int ClientId = str_toint(pStr);
		if(ClientId < 0 || ClientId >= MAX_CLIENTS || pThis->Server()->m_aClients[ClientId].m_State == CServer::CClient::STATE_EMPTY)
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (invalid client id)");
		else
			pThis->BanAddr(pThis->Server()->ClientAddr(ClientId), Minutes * 60, pReason, false);
	}
	else
	{
		NETADDR Addr;
		if(net_addr_from_str(&Addr, pStr) == 0)
			pThis->BanAddr(&Addr, Minutes * 60, pReason, false);
		else
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (invalid network address)");
	}
}

void CServerBan::ConBanRegion(IConsole::IResult *pResult, void *pUser)
{
	const char *pRegion = pResult->GetString(0);
	if(str_comp_nocase(pRegion, g_Config.m_SvRegionName))
		return;

	pResult->RemoveArgument(0);
	ConBanExt(pResult, pUser);
}

void CServerBan::ConBanRegionRange(IConsole::IResult *pResult, void *pUser)
{
	CServerBan *pServerBan = static_cast<CServerBan *>(pUser);

	const char *pRegion = pResult->GetString(0);
	if(str_comp_nocase(pRegion, g_Config.m_SvRegionName))
		return;

	pResult->RemoveArgument(0);
	ConBanRange(pResult, static_cast<CNetBan *>(pServerBan));
}

// Not thread-safe!
class CRconClientLogger : public ILogger
{
	CServer *m_pServer;
	int m_ClientId;

public:
	CRconClientLogger(CServer *pServer, int ClientId) :
		m_pServer(pServer),
		m_ClientId(ClientId)
	{
	}
	void Log(const CLogMessage *pMessage) override;
};

void CRconClientLogger::Log(const CLogMessage *pMessage)
{
	if(m_Filter.Filters(pMessage))
	{
		return;
	}
	m_pServer->SendRconLogLine(m_ClientId, pMessage);
}

void CServer::CClient::Reset()
{
	// reset input
	for(auto &Input : m_aInputs)
	{
		Input.m_GameTick = -1;
		Input.m_ReceiveTime = 0;
	}
	m_CurrentInput = 0;
	mem_zero(&m_LastPreInput, sizeof(m_LastPreInput));
	mem_zero(&m_LatestInput, sizeof(m_LatestInput));

	m_Snapshots.PurgeAll();
	m_LastAckedSnapshot = -1;
	m_LastInputTick = -1;
	m_SnapRate = CClient::SNAPRATE_INIT;
	m_Score = -1;
	m_NextMapChunk = 0;
	m_NumMapChunks = 0;
	m_PreInputsTick = -1;
	m_NumPreInputs = 0;
	m_Flags = 0;
	m_RedirectDropTime = 0;

	std::fill(std::begin(m_aIdMap), std::end(m_aIdMap), -1);
	std::fill(std::begin(m_aReverseIdMap), std::end(m_aReverseIdMap), -1);
	m_QuicResumeSessionId = 0;
	m_aQuicResumeToken.fill(0);
	m_QuicResumeDeadline = 0;
	m_QuicResumeArmed = false;
	m_QuicDetached = false;
	m_QuicDropPending = false;
}

CServer::CServer()
{
	m_pConfig = &g_Config;
	for(int i = 0; i < MAX_CLIENTS; i++)
		m_aDemoRecorder[i] = CDemoRecorder(&m_SnapshotDelta, true);
	m_aDemoRecorder[RECORDER_MANUAL] = CDemoRecorder(&m_SnapshotDelta, false);
	m_aDemoRecorder[RECORDER_AUTO] = CDemoRecorder(&m_SnapshotDelta, false);

	m_pGameServer = nullptr;

	m_CurrentGameTick = MIN_TICK;
	m_RunServer = UNINITIALIZED;

	m_aShutdownReason[0] = 0;

	for(int i = 0; i < NUM_MAP_TYPES; i++)
	{
		m_apCurrentMapData[i] = nullptr;
		m_aCurrentMapSize[i] = 0;
	}

	m_MapReload = false;
	m_SameMapReload = false;
	m_ReloadedWhenEmpty = false;
	m_aMapDownloadUrl[0] = '\0';

	m_RconClientId = IServer::RCON_CID_SERV;
	m_RconAuthLevel = AUTHED_ADMIN;

	m_ServerInfoFirstRequest = 0;
	m_ServerInfoNumRequests = 0;

#ifdef CONF_FAMILY_UNIX
	m_ConnLoggingSocketCreated = false;
#endif

	m_pConnectionPool = new CDbConnectionPool();
	m_pRegister = nullptr;

	m_aErrorShutdownReason[0] = 0;

	Init();
}

CServer::~CServer()
{
	for(auto &pCurrentMapData : m_apCurrentMapData)
	{
		free(pCurrentMapData);
	}

	if(m_RunServer != UNINITIALIZED)
	{
		for(auto &Client : m_aClients)
		{
			free(Client.m_pPersistentData);
		}
	}
	free(m_pPersistentData);

	delete m_pRegister;
	delete m_pConnectionPool;
}

const char *CServer::DnsblStateStr(EDnsblState State)
{
	switch(State)
	{
	case EDnsblState::NONE:
		return "n/a";
	case EDnsblState::PENDING:
		return "pending";
	case EDnsblState::BLACKLISTED:
		return "black";
	case EDnsblState::WHITELISTED:
		return "white";
	}

	dbg_assert_failed("Invalid dnsbl State: %d", static_cast<int>(State));
}

IConsole::EAccessLevel CServer::ConsoleAccessLevel(int ClientId) const
{
	int AuthLevel = GetAuthedState(ClientId);
	switch(AuthLevel)
	{
	case AUTHED_ADMIN:
		return IConsole::EAccessLevel::ADMIN;
	case AUTHED_MOD:
		return IConsole::EAccessLevel::MODERATOR;
	case AUTHED_HELPER:
		return IConsole::EAccessLevel::HELPER;
	};

	dbg_assert_failed("Invalid AuthLevel: %d", AuthLevel);
}

bool CServer::IsClientNameAvailable(int ClientId, const char *pNameRequest)
{
	// check for empty names
	if(!pNameRequest[0])
		return false;

	// check for names starting with /, as they can be abused to make people
	// write chat commands
	if(pNameRequest[0] == '/')
		return false;

	// make sure that two clients don't have the same name
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i != ClientId && m_aClients[i].m_State >= CClient::STATE_READY)
		{
			if(str_utf8_comp_confusable(pNameRequest, m_aClients[i].m_aName) == 0)
				return false;
		}
	}

	return true;
}

bool CServer::SetClientNameImpl(int ClientId, const char *pNameRequest, bool Set)
{
	dbg_assert(0 <= ClientId && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	if(m_aClients[ClientId].m_State < CClient::STATE_READY)
		return false;

	const CNameBan *pBanned = m_NameBans.IsBanned(pNameRequest);
	if(pBanned)
	{
		if(m_aClients[ClientId].m_State == CClient::STATE_READY && Set)
		{
			char aBuf[256];
			if(pBanned->m_aReason[0])
			{
				str_format(aBuf, sizeof(aBuf), "Kicked (your name is banned: %s)", pBanned->m_aReason);
			}
			else
			{
				str_copy(aBuf, "Kicked (your name is banned)");
			}
			Kick(ClientId, aBuf);
		}
		return false;
	}

	// trim the name
	char aTrimmedName[MAX_NAME_LENGTH];
	str_copy(aTrimmedName, str_utf8_skip_whitespaces(pNameRequest));
	str_utf8_trim_right(aTrimmedName);

	char aNameTry[MAX_NAME_LENGTH];
	str_copy(aNameTry, aTrimmedName);

	if(!IsClientNameAvailable(ClientId, aNameTry))
	{
		// auto rename
		for(int i = 1;; i++)
		{
			str_format(aNameTry, sizeof(aNameTry), "(%d)%s", i, aTrimmedName);
			if(IsClientNameAvailable(ClientId, aNameTry))
				break;
		}
	}

	bool Changed = str_comp(m_aClients[ClientId].m_aName, aNameTry) != 0;

	if(Set && Changed)
	{
		// set the client name
		str_copy(m_aClients[ClientId].m_aName, aNameTry);
		GameServer()->TeehistorianRecordPlayerName(ClientId, m_aClients[ClientId].m_aName);
		GameServer()->OnClientInfoChange(ClientId);
	}

	return Changed;
}

bool CServer::SetClientClanImpl(int ClientId, const char *pClanRequest, bool Set)
{
	dbg_assert(0 <= ClientId && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	if(m_aClients[ClientId].m_State < CClient::STATE_READY)
		return false;

	const CNameBan *pBanned = m_NameBans.IsBanned(pClanRequest);
	if(pBanned)
	{
		if(m_aClients[ClientId].m_State == CClient::STATE_READY && Set)
		{
			char aBuf[256];
			if(pBanned->m_aReason[0])
			{
				str_format(aBuf, sizeof(aBuf), "Kicked (your clan is banned: %s)", pBanned->m_aReason);
			}
			else
			{
				str_copy(aBuf, "Kicked (your clan is banned)");
			}
			Kick(ClientId, aBuf);
		}
		return false;
	}

	// trim the clan
	char aTrimmedClan[MAX_CLAN_LENGTH];
	str_copy(aTrimmedClan, str_utf8_skip_whitespaces(pClanRequest));
	str_utf8_trim_right(aTrimmedClan);

	bool Changed = str_comp(m_aClients[ClientId].m_aClan, aTrimmedClan) != 0;

	if(Set && Changed)
	{
		// set the client clan
		str_copy(m_aClients[ClientId].m_aClan, aTrimmedClan);
		GameServer()->OnClientInfoChange(ClientId);
	}

	return Changed;
}

bool CServer::WouldClientNameChange(int ClientId, const char *pNameRequest)
{
	return SetClientNameImpl(ClientId, pNameRequest, false);
}

bool CServer::WouldClientClanChange(int ClientId, const char *pClanRequest)
{
	return SetClientClanImpl(ClientId, pClanRequest, false);
}

void CServer::SetClientName(int ClientId, const char *pName)
{
	SetClientNameImpl(ClientId, pName, true);
}

void CServer::SetClientClan(int ClientId, const char *pClan)
{
	SetClientClanImpl(ClientId, pClan, true);
}

void CServer::SetClientCountry(int ClientId, int Country)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State < CClient::STATE_READY)
		return;

	if(m_aClients[ClientId].m_Country == Country)
		return;

	m_aClients[ClientId].m_Country = Country;
	GameServer()->OnClientInfoChange(ClientId);
}

void CServer::SetClientScore(int ClientId, std::optional<int> Score)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State < CClient::STATE_READY)
		return;

	if(m_aClients[ClientId].m_Score != Score)
		ExpireServerInfo();

	m_aClients[ClientId].m_Score = Score;
}

void CServer::SetClientFlags(int ClientId, int Flags)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State < CClient::STATE_READY)
		return;

	m_aClients[ClientId].m_Flags = Flags;
}

void CServer::Kick(int ClientId, const char *pReason)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State == CClient::STATE_EMPTY)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "invalid client id to kick");
		return;
	}
	else if(m_RconClientId == ClientId)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "you can't kick yourself");
		return;
	}
	else if(GetAuthedState(ClientId) > m_RconAuthLevel)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "kick command denied");
		return;
	}

	DropClient(ClientId, pReason);
}

void CServer::DropClient(int ClientId, const char *pReason)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State == CClient::STATE_EMPTY)
		return;
	if(!m_aClients[ClientId].m_Quic)
	{
		m_NetServer.Drop(ClientId, pReason);
		return;
	}
	m_QuicTransport.Close(m_aClients[ClientId].m_QuicSession, pReason);
	DelClientCallback(ClientId, pReason, this);
}

void CServer::Ban(int ClientId, int Seconds, const char *pReason, bool VerbatimReason)
{
	m_NetServer.NetBan()->BanAddr(ClientAddr(ClientId), Seconds, pReason, VerbatimReason);
}

void CServer::ReconnectClient(int ClientId)
{
	dbg_assert(0 <= ClientId && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(m_aClients[ClientId].m_State != CClient::STATE_EMPTY, "Client slot empty: %d", ClientId);

	if(GetClientVersion(ClientId) < VERSION_DDNET_RECONNECT)
	{
		RedirectClient(ClientId, m_NetServer.Address().port);
		return;
	}
	log_info("server", "telling client to reconnect, cid=%d", ClientId);

	CMsgPacker Msg(NETMSG_RECONNECT, true);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);

	if(m_aClients[ClientId].m_State >= CClient::STATE_READY)
	{
		GameServer()->OnClientDrop(ClientId, "reconnect");
	}

	m_aClients[ClientId].m_RedirectDropTime = time_get() + time_freq() * 10;
	m_aClients[ClientId].m_State = CClient::STATE_REDIRECTED;
}

void CServer::RedirectClient(int ClientId, int Port)
{
	dbg_assert(0 <= ClientId && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(m_aClients[ClientId].m_State != CClient::STATE_EMPTY, "Client slot empty: %d", ClientId);

	bool SupportsRedirect = GetClientVersion(ClientId) >= VERSION_DDNET_REDIRECT;

	log_info("server", "redirecting client, cid=%d port=%d supported=%d", ClientId, Port, SupportsRedirect);

	if(!SupportsRedirect)
	{
		char aBuf[128];
		bool SamePort = Port == this->Port();
		str_format(aBuf, sizeof(aBuf), "Redirect unsupported: please connect to port %d", Port);
		Kick(ClientId, SamePort ? "Redirect unsupported: please reconnect" : aBuf);
		return;
	}

	CMsgPacker Msg(NETMSG_REDIRECT, true);
	Msg.AddInt(Port);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);

	if(m_aClients[ClientId].m_State >= CClient::STATE_READY)
	{
		GameServer()->OnClientDrop(ClientId, "redirect");
	}

	m_aClients[ClientId].m_RedirectDropTime = time_get() + time_freq() * 10;
	m_aClients[ClientId].m_State = CClient::STATE_REDIRECTED;
}

int64_t CServer::TickStartTime(int Tick)
{
	return m_GameStartTime + (time_freq() * Tick) / TickSpeed();
}

int CServer::Init()
{
	for(auto &Client : m_aClients)
	{
		Client.m_State = CClient::STATE_EMPTY;
		Client.m_aName[0] = 0;
		Client.m_aClan[0] = 0;
		Client.m_Country = CountryCode::DEFAULT;
		Client.m_Snapshots.Init();
		Client.m_Traffic = 0;
		Client.m_TrafficSince = 0;
		Client.m_ShowIps = false;
		Client.m_DebugDummy = false;
		Client.m_Quic = false;
		Client.m_WebTransport = false;
		Client.m_QuicSession = CQuicSessionId();
		Client.m_QuicResumeSessionId = 0;
		Client.m_aQuicResumeToken.fill(0);
		Client.m_QuicResumeDeadline = 0;
		Client.m_QuicResumeArmed = false;
		Client.m_QuicDetached = false;
		Client.m_QuicDropPending = false;
		Client.m_AuthKey = -1;
		Client.m_Latency = 0;
		Client.m_Sixup = false;
		Client.m_RedirectDropTime = 0;
	}

	m_CurrentGameTick = MIN_TICK;

	m_AnnouncementLastLine = -1;
	std::fill(std::begin(m_aPrevStates), std::end(m_aPrevStates), 0);

	return 0;
}

bool CServer::StrHideIps(const char *pInput, char *pOutputWithIps, size_t OutputWithIpsSize, char *pOutputWithoutIps, size_t OutputWithoutIpsSize)
{
	const char *pStart = str_find(pInput, "<{");
	const char *pEnd = pStart == nullptr ? nullptr : str_find(pStart + 2, "}>");
	pOutputWithIps[0] = '\0';
	pOutputWithoutIps[0] = '\0';

	if(pStart == nullptr || pEnd == nullptr)
	{
		str_copy(pOutputWithIps, pInput, OutputWithIpsSize);
		str_copy(pOutputWithoutIps, pInput, OutputWithoutIpsSize);
		return false;
	}

	str_append(pOutputWithIps, pInput, std::min((size_t)(pStart - pInput + 1), OutputWithIpsSize));
	str_append(pOutputWithIps, pStart + 2, std::min((size_t)(pEnd - pInput - 1), OutputWithIpsSize));
	str_append(pOutputWithIps, pEnd + 2, OutputWithIpsSize);

	str_append(pOutputWithoutIps, pInput, std::min((size_t)(pStart - pInput + 1), OutputWithoutIpsSize));
	str_append(pOutputWithoutIps, "XXX", OutputWithoutIpsSize);
	str_append(pOutputWithoutIps, pEnd + 2, OutputWithoutIpsSize);
	return true;
}

void CServer::SendLogLine(const CLogMessage *pMessage)
{
	if(pMessage->m_Level <= IConsole::ToLogLevelFilter(g_Config.m_ConsoleOutputLevel))
	{
		SendRconLogLine(-1, pMessage);
	}
	if(pMessage->m_Level <= IConsole::ToLogLevelFilter(g_Config.m_EcOutputLevel))
	{
		m_Econ.Send(-1, pMessage->m_aLine);
	}
}

void CServer::SetRconCid(int ClientId)
{
	m_RconClientId = ClientId;
}

int CServer::GetAuthedState(int ClientId) const
{
	if(ClientId == IConsole::CLIENT_ID_UNSPECIFIED)
		return AUTHED_ADMIN;
	if(ClientId == IConsole::CLIENT_ID_GAME)
		return AUTHED_ADMIN;
	if(ClientId == IConsole::CLIENT_ID_NO_GAME)
		return AUTHED_ADMIN;
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(m_aClients[ClientId].m_State != CServer::CClient::STATE_EMPTY, "Client slot %d is empty", ClientId);
	return m_AuthManager.KeyLevel(m_aClients[ClientId].m_AuthKey);
}

bool CServer::IsRconAuthed(int ClientId) const
{
	return GetAuthedState(ClientId) != AUTHED_NO;
}

bool CServer::IsRconAuthedAdmin(int ClientId) const
{
	return GetAuthedState(ClientId) == AUTHED_ADMIN;
}

const char *CServer::GetAuthName(int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(m_aClients[ClientId].m_State != CServer::CClient::STATE_EMPTY, "Client slot %d is empty", ClientId);
	int Key = m_aClients[ClientId].m_AuthKey;
	dbg_assert(Key != -1, "Client not authed");
	return m_AuthManager.KeyIdent(Key);
}

bool CServer::HasAuthHidden(int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	return m_aClients[ClientId].m_AuthHidden;
}

bool CServer::GetClientInfo(int ClientId, CClientInfo *pInfo) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(pInfo != nullptr, "pInfo cannot be null");

	if(m_aClients[ClientId].m_State == CClient::STATE_INGAME)
	{
		pInfo->m_pName = m_aClients[ClientId].m_aName;
		pInfo->m_Latency = m_aClients[ClientId].m_Latency;
		pInfo->m_GotDDNetVersion = m_aClients[ClientId].m_DDNetVersionSettled;
		pInfo->m_DDNetVersion = m_aClients[ClientId].m_DDNetVersion >= 0 ? m_aClients[ClientId].m_DDNetVersion : VERSION_VANILLA;
		if(m_aClients[ClientId].m_GotDDNetVersionPacket)
		{
			pInfo->m_pConnectionId = &m_aClients[ClientId].m_ConnectionId;
			pInfo->m_pDDNetVersionStr = m_aClients[ClientId].m_aDDNetVersionStr;
		}
		else
		{
			pInfo->m_pConnectionId = nullptr;
			pInfo->m_pDDNetVersionStr = nullptr;
		}
		return true;
	}
	return false;
}

void CServer::SetClientDDNetVersion(int ClientId, int DDNetVersion)
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);

	if(m_aClients[ClientId].m_State == CClient::STATE_INGAME)
	{
		m_aClients[ClientId].m_DDNetVersion = DDNetVersion;
		m_aClients[ClientId].m_DDNetVersionSettled = true;
	}
}

const NETADDR *CServer::ClientAddr(int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(m_aClients[ClientId].m_State != CServer::CClient::STATE_EMPTY, "Client slot %d is empty", ClientId);
	if(m_aClients[ClientId].m_DebugDummy)
	{
		return &m_aClients[ClientId].m_DebugDummyAddr;
	}
	if(m_aClients[ClientId].m_Quic)
		return &m_aClients[ClientId].m_QuicAddr;
	return m_NetServer.ClientAddr(ClientId);
}

const std::array<char, NETADDR_MAXSTRSIZE> &CServer::ClientAddrStringImpl(int ClientId, bool IncludePort) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	dbg_assert(m_aClients[ClientId].m_State != CServer::CClient::STATE_EMPTY, "Client slot %d is empty", ClientId);
	if(m_aClients[ClientId].m_DebugDummy)
	{
		return IncludePort ? m_aClients[ClientId].m_aDebugDummyAddrString : m_aClients[ClientId].m_aDebugDummyAddrStringNoPort;
	}
	if(m_aClients[ClientId].m_Quic)
		return IncludePort ? m_aClients[ClientId].m_aQuicAddrString : m_aClients[ClientId].m_aQuicAddrStringNoPort;
	return m_NetServer.ClientAddrString(ClientId, IncludePort);
}

const char *CServer::ClientTransportName(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return "unknown";
	if(!m_aClients[ClientId].m_Quic)
		return "udp";
	return m_aClients[ClientId].m_WebTransport ? "webtransport" : "quic";
}

const char *CServer::ClientName(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State == CServer::CClient::STATE_EMPTY)
		return "(invalid)";
	if(m_aClients[ClientId].m_State == CServer::CClient::STATE_INGAME || m_aClients[ClientId].m_State == CServer::CClient::STATE_REDIRECTED)
		return m_aClients[ClientId].m_aName;
	else
		return "(connecting)";
}

const char *CServer::ClientClan(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State == CServer::CClient::STATE_EMPTY)
		return "";
	if(m_aClients[ClientId].m_State == CServer::CClient::STATE_INGAME)
		return m_aClients[ClientId].m_aClan;
	else
		return "";
}

int CServer::ClientCountry(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State == CServer::CClient::STATE_EMPTY)
		return -1;
	if(m_aClients[ClientId].m_State == CServer::CClient::STATE_INGAME)
		return m_aClients[ClientId].m_Country;
	else
		return -1;
}

bool CServer::ClientSlotEmpty(int ClientId) const
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS && m_aClients[ClientId].m_State == CServer::CClient::STATE_EMPTY;
}

bool CServer::ClientIngame(int ClientId) const
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS && m_aClients[ClientId].m_State == CServer::CClient::STATE_INGAME;
}

int CServer::Port() const
{
	return m_NetServer.Address().port;
}

int CServer::MaxClients() const
{
	return m_RunServer == UNINITIALIZED ? 0 : m_NetServer.MaxClients();
}

int CServer::ClientCount() const
{
	int ClientCount = 0;
	for(const auto &Client : m_aClients)
	{
		if(Client.m_State != CClient::STATE_EMPTY)
		{
			ClientCount++;
		}
	}

	return ClientCount;
}

int CServer::DistinctClientCount() const
{
	const NETADDR *apAddresses[MAX_CLIENTS];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		// connecting clients with spoofed ips can clog slots without being ingame
		apAddresses[i] = ClientIngame(i) ? ClientAddr(i) : nullptr;
	}

	int ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(apAddresses[i] == nullptr)
		{
			continue;
		}
		ClientCount++;
		for(int j = 0; j < i; j++)
		{
			if(apAddresses[j] != nullptr && !net_addr_comp_noport(apAddresses[i], apAddresses[j]))
			{
				ClientCount--;
				break;
			}
		}
	}
	return ClientCount;
}

int CServer::GetClientVersion(int ClientId) const
{
	// Assume latest client version for server demos
	if(ClientId == SERVER_DEMO_CLIENT)
		return DDNET_VERSION_NUMBER;

	CClientInfo Info;
	if(GetClientInfo(ClientId, &Info))
		return Info.m_DDNetVersion;
	return VERSION_NONE;
}

static inline bool RepackMsg(const CMsgPacker *pMsg, CPacker &Packer, bool Sixup)
{
	int MsgId = pMsg->m_MsgId;
	Packer.Reset();

	if(Sixup && !pMsg->m_NoTranslate)
	{
		if(pMsg->m_System)
		{
			if(MsgId >= OFFSET_UUID)
				;
			else if(MsgId >= NETMSG_MAP_CHANGE && MsgId <= NETMSG_MAP_DATA)
				;
			else if(MsgId >= NETMSG_CON_READY && MsgId <= NETMSG_INPUTTIMING)
				MsgId += 1;
			else if(MsgId == NETMSG_RCON_LINE)
				MsgId = protocol7::NETMSG_RCON_LINE;
			else if(MsgId >= NETMSG_PING && MsgId <= NETMSG_PING_REPLY)
				MsgId += 4;
			else if(MsgId >= NETMSG_RCON_CMD_ADD && MsgId <= NETMSG_RCON_CMD_REM)
				MsgId -= 11;
			else
			{
				log_error("net", "DROP send sys %d", MsgId);
				return false;
			}
		}
		else
		{
			if(MsgId >= 0 && MsgId < OFFSET_UUID)
				MsgId = Msg_SixToSeven(MsgId);

			if(MsgId < 0)
				return false;
		}
	}

	if(MsgId < OFFSET_UUID)
	{
		Packer.AddInt((MsgId << 1) | (pMsg->m_System ? 1 : 0));
	}
	else
	{
		Packer.AddInt(pMsg->m_System ? 1 : 0); // NETMSG_EX, NETMSGTYPE_EX
		g_UuidManager.PackUuid(MsgId, &Packer);
	}
	Packer.AddRaw(pMsg->Data(), pMsg->Size());

	return true;
}

int CServer::SendMsg(CMsgPacker *pMsg, int Flags, int ClientId)
{
	CNetChunk Packet = {};
	if(Flags & MSGFLAG_VITAL)
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	if(Flags & MSGFLAG_FLUSH)
		Packet.m_Flags |= NETSENDFLAG_FLUSH;
	const bool Vital = (Flags & MSGFLAG_VITAL) != 0;

	if(ClientId < 0)
	{
		CPacker Pack6, Pack7;
		if(!RepackMsg(pMsg, Pack6, false))
			return -1;
		if(!RepackMsg(pMsg, Pack7, true))
			return -1;

		// write message to demo recorders
		if(!(Flags & MSGFLAG_NORECORD))
		{
			for(auto &Recorder : m_aDemoRecorder)
				if(Recorder.IsRecording())
					Recorder.RecordMessage(Pack6.Data(), Pack6.Size());
		}

		if(!(Flags & MSGFLAG_NOSEND))
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(m_aClients[i].m_State == CClient::STATE_INGAME)
				{
					CPacker *pPack = m_aClients[i].m_Sixup ? &Pack7 : &Pack6;
					Packet.m_pData = pPack->Data();
					Packet.m_DataSize = pPack->Size();
					Packet.m_ClientId = i;
					if(Antibot()->OnEngineServerMessage(i, Packet.m_pData, Packet.m_DataSize, Flags))
					{
						continue;
					}
					if(m_aClients[i].m_Quic)
					{
						if(m_aClients[i].m_QuicDetached || m_aClients[i].m_QuicDropPending)
							continue;
						if(!m_QuicTransport.Send(m_aClients[i].m_QuicSession, Packet.m_pData, Packet.m_DataSize, Vital) && Vital)
							m_aClients[i].m_QuicDropPending = true;
					}
					else
						m_NetServer.Send(&Packet);
				}
			}
		}
	}
	else
	{
		CPacker Pack;
		if(!RepackMsg(pMsg, Pack, m_aClients[ClientId].m_Sixup))
			return -1;

		Packet.m_ClientId = ClientId;
		Packet.m_pData = Pack.Data();
		Packet.m_DataSize = Pack.Size();

		if(Antibot()->OnEngineServerMessage(ClientId, Packet.m_pData, Packet.m_DataSize, Flags))
		{
			return 0;
		}

		// write message to demo recorders
		if(!(Flags & MSGFLAG_NORECORD))
		{
			if(m_aDemoRecorder[ClientId].IsRecording())
				m_aDemoRecorder[ClientId].RecordMessage(Pack.Data(), Pack.Size());
			if(m_aDemoRecorder[RECORDER_MANUAL].IsRecording())
				m_aDemoRecorder[RECORDER_MANUAL].RecordMessage(Pack.Data(), Pack.Size());
			if(m_aDemoRecorder[RECORDER_AUTO].IsRecording())
				m_aDemoRecorder[RECORDER_AUTO].RecordMessage(Pack.Data(), Pack.Size());
		}

		if(!(Flags & MSGFLAG_NOSEND))
		{
			if(m_aClients[ClientId].m_Quic)
			{
				if(m_aClients[ClientId].m_QuicDetached || m_aClients[ClientId].m_QuicDropPending)
					return 0;
				if(!m_QuicTransport.Send(m_aClients[ClientId].m_QuicSession, Packet.m_pData, Packet.m_DataSize, Vital) && Vital)
					m_aClients[ClientId].m_QuicDropPending = true;
			}
			else
				m_NetServer.Send(&Packet);
		}
	}

	return 0;
}

void CServer::SendMsgRaw(int ClientId, const void *pData, int Size, int Flags)
{
	if(m_aClients[ClientId].m_Quic)
	{
		if(m_aClients[ClientId].m_QuicDetached || m_aClients[ClientId].m_QuicDropPending)
			return;
		const bool Vital = (Flags & MSGFLAG_VITAL) != 0;
		if(!m_QuicTransport.Send(m_aClients[ClientId].m_QuicSession, pData, Size, Vital) && Vital)
			m_aClients[ClientId].m_QuicDropPending = true;
		return;
	}
	CNetChunk Packet;
	mem_zero(&Packet, sizeof(CNetChunk));
	Packet.m_ClientId = ClientId;
	Packet.m_pData = pData;
	Packet.m_DataSize = Size;
	Packet.m_Flags = 0;
	if(Flags & MSGFLAG_VITAL)
	{
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	}
	if(Flags & MSGFLAG_FLUSH)
	{
		Packet.m_Flags |= NETSENDFLAG_FLUSH;
	}
	m_NetServer.Send(&Packet);
}

void CServer::DoSnapshot()
{
	bool IsGlobalSnap = Config()->m_SvHighBandwidth || (m_CurrentGameTick % 2) == 0;

	if(m_aDemoRecorder[RECORDER_MANUAL].IsRecording() || m_aDemoRecorder[RECORDER_AUTO].IsRecording())
	{
		// create snapshot for demo recording
		CSnapshotBuffer Data;

		// build snap and possibly add some messages
		m_SnapshotBuilder.Init();
		GameServer()->OnSnap(-1, IsGlobalSnap, true);
		int SnapshotSize = m_SnapshotBuilder.Finish(&Data);

		// write snapshot
		if(m_aDemoRecorder[RECORDER_MANUAL].IsRecording())
			m_aDemoRecorder[RECORDER_MANUAL].RecordSnapshot(Tick(), Data.AsSnapshot(), SnapshotSize);
		if(m_aDemoRecorder[RECORDER_AUTO].IsRecording())
			m_aDemoRecorder[RECORDER_AUTO].RecordSnapshot(Tick(), Data.AsSnapshot(), SnapshotSize);
	}

	// create snapshots for all clients
	for(int i = 0; i < MaxClients(); i++)
	{
		// client must be ingame to receive snapshots
		if(m_aClients[i].m_State != CClient::STATE_INGAME || m_aClients[i].m_QuicDetached)
			continue;

		// this client is trying to recover, don't spam snapshots
		if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_RECOVER && (Tick() % TickSpeed()) != 0)
			continue;

		// this client is trying to recover, don't spam snapshots
		if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_INIT && (Tick() % 10) != 0)
			continue;

		// only allow clients with forced high bandwidth on spectate to receive snapshots on non-global ticks
		if(!IsGlobalSnap && !(m_aClients[i].m_ForceHighBandwidthOnSpectate && GameServer()->IsClientHighBandwidth(i)))
			continue;

		{
			m_SnapshotBuilder.Init(m_aClients[i].m_Sixup);

			// only snap events on global ticks
			GameServer()->OnSnap(i, IsGlobalSnap, m_aDemoRecorder[i].IsRecording());

			// finish snapshot
			CSnapshotBuffer Data;
			int SnapshotSize = m_SnapshotBuilder.Finish(&Data);

			if(m_aDemoRecorder[i].IsRecording())
			{
				// write snapshot
				m_aDemoRecorder[i].RecordSnapshot(Tick(), Data.AsSnapshot(), SnapshotSize);
			}

			int Crc = Data.AsSnapshot()->Crc();

			// Remove old snapshots. Only the last acked snapshot
			// is still needed as delta base, keep at most 3
			// seconds worth for clients that aren't acking.
			//
			// This also works for the sentinel value -1 of
			// `m_LastAckedSnapshot` (before the first ack):
			// the max then falls back to the 3 second cap.
			m_aClients[i].m_Snapshots.PurgeUntil(std::max(m_CurrentGameTick - TickSpeed() * 3, m_aClients[i].m_LastAckedSnapshot));

			// save the snapshot
			m_aClients[i].m_Snapshots.Add(m_CurrentGameTick, time_get(), SnapshotSize, Data.AsSnapshot(), 0, nullptr);

			// find snapshot that we can perform delta against
			int DeltaTick = -1;
			const CSnapshot *pDeltashot = CSnapshot::EmptySnapshot();
			{
				int DeltashotSize;
				if(m_aClients[i].m_LastAckedSnapshot >= MIN_TICK)
				{
					DeltashotSize = m_aClients[i].m_Snapshots.Get(m_aClients[i].m_LastAckedSnapshot, nullptr, &pDeltashot, nullptr);
				}
				else
				{
					DeltashotSize = -1;
				}
				if(DeltashotSize >= 0)
				{
					DeltaTick = m_aClients[i].m_LastAckedSnapshot;
				}
				else
				{
					// no acked package found, force client to recover rate
					if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_FULL)
						m_aClients[i].m_SnapRate = CClient::SNAPRATE_RECOVER;
				}
			}

			// create delta
			CSnapshotDelta *const pSnapshotDelta = IsSixup(i) ? &m_SnapshotDeltaSixup : &m_SnapshotDelta;
			char aDeltaData[CSnapshot::MAX_SIZE];
			int DeltaSize = pSnapshotDelta->CreateDelta(pDeltashot, Data.AsSnapshot(), aDeltaData);

			if(DeltaSize)
			{
				// compress it
				const int MaxSize = MAX_SNAPSHOT_PACKSIZE;

				char aCompData[CSnapshot::MAX_SIZE];
				SnapshotSize = CVariableInt::Compress(aDeltaData, DeltaSize, aCompData, sizeof(aCompData));
				int NumPackets = (SnapshotSize + MaxSize - 1) / MaxSize;

				for(int n = 0, Left = SnapshotSize; Left > 0; n++)
				{
					int Chunk = Left < MaxSize ? Left : MaxSize;
					Left -= Chunk;

					if(NumPackets == 1)
					{
						CMsgPacker Msg(NETMSG_SNAPSINGLE, true);
						Msg.AddInt(m_CurrentGameTick);
						Msg.AddInt(m_CurrentGameTick - DeltaTick);
						Msg.AddInt(Crc);
						Msg.AddInt(Chunk);
						Msg.AddRaw(&aCompData[n * MaxSize], Chunk);
						SendMsg(&Msg, MSGFLAG_FLUSH, i);
					}
					else
					{
						CMsgPacker Msg(NETMSG_SNAP, true);
						Msg.AddInt(m_CurrentGameTick);
						Msg.AddInt(m_CurrentGameTick - DeltaTick);
						Msg.AddInt(NumPackets);
						Msg.AddInt(n);
						Msg.AddInt(Crc);
						Msg.AddInt(Chunk);
						Msg.AddRaw(&aCompData[n * MaxSize], Chunk);
						SendMsg(&Msg, MSGFLAG_FLUSH, i);
					}
				}
			}
			else
			{
				CMsgPacker Msg(NETMSG_SNAPEMPTY, true);
				Msg.AddInt(m_CurrentGameTick);
				Msg.AddInt(m_CurrentGameTick - DeltaTick);
				SendMsg(&Msg, MSGFLAG_FLUSH, i);
			}
		}
	}

	if(IsGlobalSnap)
	{
		GameServer()->OnPostGlobalSnap();
	}
}

int CServer::ClientRejoinCallback(int ClientId, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	pThis->m_aClients[ClientId].m_AuthKey = -1;
	pThis->m_aClients[ClientId].m_pRconCmdToSend = nullptr;
	pThis->m_aClients[ClientId].m_MaplistEntryToSend = CClient::MAPLIST_UNINITIALIZED;
	pThis->m_aClients[ClientId].m_DDNetVersion = VERSION_NONE;
	pThis->m_aClients[ClientId].m_GotDDNetVersionPacket = false;
	pThis->m_aClients[ClientId].m_DDNetVersionSettled = false;

	pThis->m_aClients[ClientId].Reset();

	pThis->GameServer()->TeehistorianRecordPlayerRejoin(ClientId);
	pThis->Antibot()->OnEngineClientDrop(ClientId, "rejoin");
	pThis->Antibot()->OnEngineClientJoin(ClientId);

	pThis->SendMap(ClientId);

	return 0;
}

int CServer::NewClientNoAuthCallback(int ClientId, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	pThis->m_aClients[ClientId].m_DnsblState = EDnsblState::NONE;

	pThis->m_aClients[ClientId].m_State = CClient::STATE_CONNECTING;
	pThis->m_aClients[ClientId].m_aName[0] = 0;
	pThis->m_aClients[ClientId].m_aClan[0] = 0;
	pThis->m_aClients[ClientId].m_Country = CountryCode::DEFAULT;
	pThis->m_aClients[ClientId].m_AuthKey = -1;
	pThis->m_aClients[ClientId].m_AuthTries = 0;
	pThis->m_aClients[ClientId].m_AuthHidden = false;
	pThis->m_aClients[ClientId].m_pRconCmdToSend = nullptr;
	pThis->m_aClients[ClientId].m_MaplistEntryToSend = CClient::MAPLIST_UNINITIALIZED;
	pThis->m_aClients[ClientId].m_ShowIps = false;
	pThis->m_aClients[ClientId].m_DebugDummy = false;
	pThis->m_aClients[ClientId].m_Quic = false;
	pThis->m_aClients[ClientId].m_WebTransport = false;
	pThis->m_aClients[ClientId].m_QuicSession = CQuicSessionId();
	pThis->m_aClients[ClientId].m_ForceHighBandwidthOnSpectate = false;
	pThis->m_aClients[ClientId].m_DDNetVersion = VERSION_NONE;
	pThis->m_aClients[ClientId].m_GotDDNetVersionPacket = false;
	pThis->m_aClients[ClientId].m_DDNetVersionSettled = false;
	pThis->m_aClients[ClientId].Reset();

	pThis->GameServer()->TeehistorianRecordPlayerJoin(ClientId, false);
	pThis->Antibot()->OnEngineClientJoin(ClientId);

	pThis->SendCapabilities(ClientId);
	pThis->SendMap(ClientId);
#if defined(CONF_FAMILY_UNIX)
	pThis->SendConnLoggingCommand(OPEN_SESSION, pThis->ClientAddr(ClientId));
#endif
	return 0;
}

int CServer::NewClientCallback(int ClientId, void *pUser, bool Sixup)
{
	CServer *pThis = (CServer *)pUser;
	pThis->m_aClients[ClientId].m_State = CClient::STATE_PREAUTH;
	pThis->m_aClients[ClientId].m_DnsblState = EDnsblState::NONE;
	pThis->m_aClients[ClientId].m_aName[0] = 0;
	pThis->m_aClients[ClientId].m_aClan[0] = 0;
	pThis->m_aClients[ClientId].m_Country = CountryCode::DEFAULT;
	pThis->m_aClients[ClientId].m_AuthKey = -1;
	pThis->m_aClients[ClientId].m_AuthTries = 0;
	pThis->m_aClients[ClientId].m_AuthHidden = false;
	pThis->m_aClients[ClientId].m_pRconCmdToSend = nullptr;
	pThis->m_aClients[ClientId].m_MaplistEntryToSend = CClient::MAPLIST_UNINITIALIZED;
	pThis->m_aClients[ClientId].m_Traffic = 0;
	pThis->m_aClients[ClientId].m_TrafficSince = 0;
	pThis->m_aClients[ClientId].m_ShowIps = false;
	pThis->m_aClients[ClientId].m_DebugDummy = false;
	pThis->m_aClients[ClientId].m_ForceHighBandwidthOnSpectate = false;
	pThis->m_aClients[ClientId].m_DDNetVersion = VERSION_NONE;
	pThis->m_aClients[ClientId].m_GotDDNetVersionPacket = false;
	pThis->m_aClients[ClientId].m_DDNetVersionSettled = false;
	pThis->m_aClients[ClientId].Reset();
	pThis->m_aClients[ClientId].m_Sixup = Sixup;

	pThis->GameServer()->TeehistorianRecordPlayerJoin(ClientId, Sixup);
	pThis->Antibot()->OnEngineClientJoin(ClientId);

#if defined(CONF_FAMILY_UNIX)
	pThis->SendConnLoggingCommand(OPEN_SESSION, pThis->ClientAddr(ClientId));
#endif
	return 0;
}

void CServer::InitDnsbl(int ClientId)
{
	NETADDR Addr = *ClientAddr(ClientId);

	//TODO: support ipv6
	if(Addr.type != NETTYPE_IPV4)
		return;

	// build dnsbl host lookup
	char aBuf[256];
	if(Config()->m_SvDnsblKey[0] == '\0')
	{
		// without key
		str_format(aBuf, sizeof(aBuf), "%d.%d.%d.%d.%s", Addr.ip[3], Addr.ip[2], Addr.ip[1], Addr.ip[0], Config()->m_SvDnsblHost);
	}
	else
	{
		// with key
		str_format(aBuf, sizeof(aBuf), "%s.%d.%d.%d.%d.%s", Config()->m_SvDnsblKey, Addr.ip[3], Addr.ip[2], Addr.ip[1], Addr.ip[0], Config()->m_SvDnsblHost);
	}

	m_aClients[ClientId].m_pDnsblLookup = std::make_shared<CHostLookup>(aBuf, NETTYPE_IPV4);
	Engine()->AddJob(m_aClients[ClientId].m_pDnsblLookup);
	m_aClients[ClientId].m_DnsblState = EDnsblState::PENDING;
}

#ifdef CONF_FAMILY_UNIX
void CServer::SendConnLoggingCommand(CONN_LOGGING_CMD Cmd, const NETADDR *pAddr)
{
	if(!Config()->m_SvConnLoggingServer[0] || !m_ConnLoggingSocketCreated)
		return;

	// pack the data and send it
	unsigned char aData[23] = {0};
	aData[0] = Cmd;
	mem_copy(&aData[1], &pAddr->type, 4);
	mem_copy(&aData[5], pAddr->ip, 16);
	mem_copy(&aData[21], &pAddr->port, 2);

	net_unix_send(m_ConnLoggingSocket, &m_ConnLoggingDestAddr, aData, sizeof(aData));
}
#endif

int CServer::DelClientCallback(int ClientId, const char *pReason, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	const bool ExternalSlot = pThis->m_aClients[ClientId].m_Quic || pThis->m_aClients[ClientId].m_DebugDummy;
	if(pThis->m_aClients[ClientId].m_Quic)
		pThis->m_aClients[ClientId].m_QuicDetached = true;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "client dropped. cid=%d addr=<{%s}> transport=%s reason='%s'", ClientId, pThis->ClientAddrString(ClientId, true), pThis->ClientTransportName(ClientId), pReason);
	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);

#if defined(CONF_FAMILY_UNIX)
	// Make copy of address because the client slot will be empty at the end of the function
	const NETADDR Addr = *pThis->ClientAddr(ClientId);
#endif

	// notify the mod about the drop
	if(pThis->m_aClients[ClientId].m_State >= CClient::STATE_READY)
		pThis->GameServer()->OnClientDrop(ClientId, pReason);

	pThis->m_aClients[ClientId].m_State = CClient::STATE_EMPTY;
	pThis->m_aClients[ClientId].m_aName[0] = 0;
	pThis->m_aClients[ClientId].m_aClan[0] = 0;
	pThis->m_aClients[ClientId].m_Country = CountryCode::DEFAULT;
	pThis->m_aClients[ClientId].m_AuthKey = -1;
	pThis->m_aClients[ClientId].m_AuthTries = 0;
	pThis->m_aClients[ClientId].m_AuthHidden = false;
	pThis->m_aClients[ClientId].m_pRconCmdToSend = nullptr;
	pThis->m_aClients[ClientId].m_MaplistEntryToSend = CClient::MAPLIST_UNINITIALIZED;
	pThis->m_aClients[ClientId].m_Traffic = 0;
	pThis->m_aClients[ClientId].m_TrafficSince = 0;
	pThis->m_aClients[ClientId].m_ShowIps = false;
	pThis->m_aClients[ClientId].m_DebugDummy = false;
	pThis->m_aClients[ClientId].m_ForceHighBandwidthOnSpectate = false;
	pThis->m_aClients[ClientId].m_Quic = false;
	pThis->m_aClients[ClientId].m_WebTransport = false;
	pThis->m_aClients[ClientId].m_QuicSession = CQuicSessionId();
	pThis->m_aClients[ClientId].m_QuicResumeSessionId = 0;
	pThis->m_aClients[ClientId].m_aQuicResumeToken.fill(0);
	pThis->m_aClients[ClientId].m_QuicResumeDeadline = 0;
	pThis->m_aClients[ClientId].m_QuicResumeArmed = false;
	pThis->m_aClients[ClientId].m_QuicDetached = false;
	pThis->m_aClients[ClientId].m_QuicDropPending = false;
	pThis->m_aPrevStates[ClientId] = CClient::STATE_EMPTY;
	pThis->m_aClients[ClientId].m_Snapshots.PurgeAll();
	pThis->m_aClients[ClientId].m_Sixup = false;
	pThis->m_aClients[ClientId].m_RedirectDropTime = 0;
	pThis->m_aClients[ClientId].m_HasPersistentData = false;

	pThis->GameServer()->TeehistorianRecordPlayerDrop(ClientId, pReason);
	pThis->Antibot()->OnEngineClientDrop(ClientId, pReason);
	if(ExternalSlot)
		pThis->m_NetServer.SetExternalSlot(ClientId, nullptr);
#if defined(CONF_FAMILY_UNIX)
	pThis->SendConnLoggingCommand(CLOSE_SESSION, &Addr);
#endif
	return 0;
}

void CServer::SendRconType(int ClientId, bool UsernameReq)
{
	CMsgPacker Msg(NETMSG_RCONTYPE, true);
	Msg.AddInt(UsernameReq);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendCapabilities(int ClientId)
{
	CMsgPacker Msg(NETMSG_CAPABILITIES, true);
	Msg.AddInt(SERVERCAP_CURVERSION); // version
	Msg.AddInt(SERVERCAPFLAG_DDNET | SERVERCAPFLAG_CHATTIMEOUTCODE | SERVERCAPFLAG_ANYPLAYERFLAG | SERVERCAPFLAG_PINGEX | SERVERCAPFLAG_ALLOWDUMMY | SERVERCAPFLAG_SYNCWEAPONINPUT); // flags
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendMap(int ClientId)
{
	int MapType = IsSixup(ClientId) ? MAP_TYPE_SIXUP : MAP_TYPE_SIX;
	{
		CMsgPacker Msg(NETMSG_MAP_DETAILS, true);
		Msg.AddString(GameServer()->Map()->BaseName(), 0);
		Msg.AddRaw(&m_aCurrentMapSha256[MapType].data, sizeof(m_aCurrentMapSha256[MapType].data));
		Msg.AddInt(m_aCurrentMapCrc[MapType]);
		Msg.AddInt(m_aCurrentMapSize[MapType]);
		if(m_aMapDownloadUrl[0])
		{
			Msg.AddString(m_aMapDownloadUrl, 0);
		}
		else
		{
			Msg.AddString("", 0);
		}
		SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
	}
	{
		CMsgPacker Msg(NETMSG_MAP_CHANGE, true);
		Msg.AddString(GameServer()->Map()->BaseName(), 0);
		Msg.AddInt(m_aCurrentMapCrc[MapType]);
		Msg.AddInt(m_aCurrentMapSize[MapType]);
		if(MapType == MAP_TYPE_SIXUP)
		{
			Msg.AddInt(Config()->m_SvMapWindow);
			Msg.AddInt(NET_MAX_CHUNK_SIZE - 128);
			Msg.AddRaw(m_aCurrentMapSha256[MapType].data, sizeof(m_aCurrentMapSha256[MapType].data));
		}
		SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);
	}

	m_aClients[ClientId].m_NextMapChunk = 0;
	m_aClients[ClientId].m_NumMapChunks = 0;
}

void CServer::SendMapData(int ClientId, int Chunk)
{
	int MapType = IsSixup(ClientId) ? MAP_TYPE_SIXUP : MAP_TYPE_SIX;
	unsigned int ChunkSize = NET_MAX_CHUNK_SIZE - 128;
	unsigned int Offset = Chunk * ChunkSize;
	int Last = 0;

	// drop faulty map data requests
	if(Chunk < 0 || Offset > m_aCurrentMapSize[MapType])
		return;

	// a client can ask for the same chunk any number of times
	CClient &Client = m_aClients[ClientId];
	const unsigned int NumChunks = (m_aCurrentMapSize[MapType] + ChunkSize - 1) / ChunkSize;
	if(Client.m_NumMapChunks >= (int)(2 * NumChunks))
		return;
	Client.m_NumMapChunks++;

	if(Offset + ChunkSize >= m_aCurrentMapSize[MapType])
	{
		ChunkSize = m_aCurrentMapSize[MapType] - Offset;
		Last = 1;
	}

	CMsgPacker Msg(NETMSG_MAP_DATA, true);
	if(MapType == MAP_TYPE_SIX)
	{
		Msg.AddInt(Last);
		Msg.AddInt(m_aCurrentMapCrc[MAP_TYPE_SIX]);
		Msg.AddInt(Chunk);
		Msg.AddInt(ChunkSize);
	}
	Msg.AddRaw(&m_apCurrentMapData[MapType][Offset], ChunkSize);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);

	if(Config()->m_Debug)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "sending chunk %d with size %d", Chunk, ChunkSize);
		Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
	}
}

bool CServer::UpdateQuicMaps()
{
	if(!m_QuicTransport.IsRunning())
		return true;
	for(uint32_t MapType = MAP_TYPE_SIX; MapType <= MAP_TYPE_SIXUP; ++MapType)
	{
		if(MapType == MAP_TYPE_SIXUP && !Config()->m_SvSixup)
			continue;
		if(!m_QuicTransport.SetMap(
			   MapType,
			   GameServer()->Map()->BaseName(),
			   m_aCurrentMapCrc[MapType],
			   m_aCurrentMapSha256[MapType].data,
			   m_apCurrentMapData[MapType],
			   m_aCurrentMapSize[MapType]))
			return false;
	}
	return true;
}

void CServer::SendMapReload(int ClientId)
{
	CMsgPacker Msg(NETMSG_MAP_RELOAD, true);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);
}

void CServer::SendConnectionReady(int ClientId)
{
	CMsgPacker Msg(NETMSG_CON_READY, true);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);
}

void CServer::SendRconLine(int ClientId, const char *pLine)
{
	CMsgPacker Msg(NETMSG_RCON_LINE, true);
	Msg.AddString(pLine, 512);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendRconLogLine(int ClientId, const CLogMessage *pMessage)
{
	char aLine[sizeof(CLogMessage().m_aLine)];
	char aLineWithoutIps[sizeof(CLogMessage().m_aLine)];
	StrHideIps(pMessage->m_aLine, aLine, sizeof(aLine), aLineWithoutIps, sizeof(aLineWithoutIps));

	if(ClientId == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_aClients[i].m_State != CClient::STATE_EMPTY && IsRconAuthedAdmin(i))
				SendRconLine(i, m_aClients[i].m_ShowIps ? aLine : aLineWithoutIps);
		}
	}
	else
	{
		if(m_aClients[ClientId].m_State != CClient::STATE_EMPTY)
			SendRconLine(ClientId, m_aClients[ClientId].m_ShowIps ? aLine : aLineWithoutIps);
	}
}

void CServer::SendRconCmdAdd(const IConsole::ICommandInfo *pCommandInfo, int ClientId)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_ADD, true);
	Msg.AddString(pCommandInfo->Name(), IConsole::TEMPCMD_NAME_LENGTH);
	Msg.AddString(pCommandInfo->Help(), IConsole::TEMPCMD_HELP_LENGTH);
	Msg.AddString(pCommandInfo->Params(), IConsole::TEMPCMD_PARAMS_LENGTH);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendRconCmdRem(const IConsole::ICommandInfo *pCommandInfo, int ClientId)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_REM, true);
	Msg.AddString(pCommandInfo->Name(), IConsole::TEMPCMD_NAME_LENGTH);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendRconCmdGroupStart(int ClientId)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_GROUP_START, true);
	Msg.AddInt(NumRconCommands(ClientId));
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendRconCmdGroupEnd(int ClientId)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_GROUP_END, true);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

int CServer::NumRconCommands(int ClientId)
{
	int Num = 0;
	for(const IConsole::ICommandInfo *pCmd = Console()->FirstCommandInfo(ClientId, CFGFLAG_SERVER);
		pCmd; pCmd = Console()->NextCommandInfo(pCmd, ClientId, CFGFLAG_SERVER))
	{
		Num++;
	}
	return Num;
}

void CServer::UpdateClientRconCommands(int ClientId)
{
	CClient &Client = m_aClients[ClientId];
	if(Client.m_State != CClient::STATE_INGAME ||
		!IsRconAuthed(ClientId) ||
		Client.m_pRconCmdToSend == nullptr)
	{
		return;
	}

	for(int i = 0; i < MAX_RCONCMD_SEND && Client.m_pRconCmdToSend; ++i)
	{
		SendRconCmdAdd(Client.m_pRconCmdToSend, ClientId);
		Client.m_pRconCmdToSend = Console()->NextCommandInfo(Client.m_pRconCmdToSend, ClientId, CFGFLAG_SERVER);
		if(Client.m_pRconCmdToSend == nullptr)
		{
			SendRconCmdGroupEnd(ClientId);
		}
	}
}

CServer::CMaplistEntry::CMaplistEntry(const char *pName)
{
	str_copy(m_aName, pName);
}

bool CServer::CMaplistEntry::operator<(const CMaplistEntry &Other) const
{
	return str_comp_filenames(m_aName, Other.m_aName) < 0;
}

void CServer::SendMaplistGroupStart(int ClientId)
{
	CMsgPacker Msg(NETMSG_MAPLIST_GROUP_START, true);
	Msg.AddInt(m_vMaplistEntries.size());
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::SendMaplistGroupEnd(int ClientId)
{
	CMsgPacker Msg(NETMSG_MAPLIST_GROUP_END, true);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
}

void CServer::UpdateClientMaplistEntries(int ClientId)
{
	CClient &Client = m_aClients[ClientId];
	if(Client.m_State != CClient::STATE_INGAME ||
		!IsRconAuthed(ClientId) ||
		Client.m_Sixup ||
		Client.m_pRconCmdToSend != nullptr || // wait for command sending
		Client.m_MaplistEntryToSend == CClient::MAPLIST_DISABLED ||
		Client.m_MaplistEntryToSend == CClient::MAPLIST_DONE)
	{
		return;
	}

	if(Client.m_MaplistEntryToSend == CClient::MAPLIST_UNINITIALIZED)
	{
		static const char *const MAP_COMMANDS[] = {"sv_map", "change_map"};
		const IConsole::EAccessLevel AccessLevel = ConsoleAccessLevel(ClientId);
		const bool MapCommandAllowed = std::any_of(std::begin(MAP_COMMANDS), std::end(MAP_COMMANDS), [&](const char *pMapCommand) {
			const IConsole::ICommandInfo *pInfo = Console()->GetCommandInfo(pMapCommand, CFGFLAG_SERVER, false);
			dbg_assert(pInfo != nullptr, "Map command not found");
			return AccessLevel <= pInfo->GetAccessLevel();
		});
		if(MapCommandAllowed)
		{
			Client.m_MaplistEntryToSend = 0;
			SendMaplistGroupStart(ClientId);
		}
		else
		{
			Client.m_MaplistEntryToSend = CClient::MAPLIST_DISABLED;
			return;
		}
	}

	if((size_t)Client.m_MaplistEntryToSend < m_vMaplistEntries.size())
	{
		CMsgPacker Msg(NETMSG_MAPLIST_ADD, true);
		int Limit = NET_MAX_CHUNK_SIZE - 128;
		while((size_t)Client.m_MaplistEntryToSend < m_vMaplistEntries.size())
		{
			// Space for null termination not included in Limit
			const int SizeBefore = Msg.Size();
			Msg.AddString(m_vMaplistEntries[Client.m_MaplistEntryToSend].m_aName, Limit - 1, false);
			if(Msg.Error())
			{
				break;
			}
			Limit -= Msg.Size() - SizeBefore;
			if(Limit <= 1)
			{
				break;
			}
			++Client.m_MaplistEntryToSend;
		}
		SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
	}

	if((size_t)Client.m_MaplistEntryToSend >= m_vMaplistEntries.size())
	{
		SendMaplistGroupEnd(ClientId);
		Client.m_MaplistEntryToSend = CClient::MAPLIST_DONE;
	}
}

static inline int MsgFromSixup(int Msg, bool System)
{
	if(System)
	{
		if(Msg == NETMSG_INFO)
			;
		else if(Msg >= 14 && Msg <= 15)
			Msg += 11;
		else if(Msg >= 18 && Msg <= 28)
			Msg = NETMSG_READY + Msg - 18;
		else if(Msg < OFFSET_UUID)
			return -1;
	}

	return Msg;
}

bool CServer::CheckReservedSlotAuth(int ClientId, const char *pPassword)
{
	if(Config()->m_SvReservedSlotsPass[0] && !str_comp(Config()->m_SvReservedSlotsPass, pPassword))
	{
		log_info("server", "ClientId=%d joining reserved slot with reserved slots password", ClientId);
		return true;
	}

	// "^([^:]*):(.*)$"
	if(Config()->m_SvReservedSlotsAuthLevel != 4)
	{
		char aName[sizeof(Config()->m_Password)];
		const char *pInnerPassword = str_next_token(pPassword, ":", aName, sizeof(aName));
		if(!pInnerPassword)
		{
			return false;
		}
		int Slot = m_AuthManager.FindKey(aName);
		if(m_AuthManager.CheckKey(Slot, pInnerPassword + 1) && m_AuthManager.KeyLevel(Slot) >= Config()->m_SvReservedSlotsAuthLevel)
		{
			log_info("server", "ClientId=%d joining reserved slot with key='%s'", ClientId, m_AuthManager.KeyIdent(Slot));
			return true;
		}
	}

	return false;
}

bool CServer::TakePreInputBudget(int ClientId)
{
	// nothing stops a client from sending more than one input per tick
	CClient &Client = m_aClients[ClientId];
	if(Client.m_PreInputsTick != Tick())
	{
		Client.m_PreInputsTick = Tick();
		Client.m_NumPreInputs = 0;
	}
	if(Config()->m_SvMaxPreInputsPerTick != 0 &&
		Client.m_NumPreInputs >= Config()->m_SvMaxPreInputsPerTick)
	{
		return false;
	}
	Client.m_NumPreInputs++;
	return true;
}

void CServer::ProcessClientPacket(CNetChunk *pPacket)
{
	int ClientId = pPacket->m_ClientId;
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || m_aClients[ClientId].m_State == CClient::STATE_EMPTY)
		return;
	CUnpacker Unpacker;
	Unpacker.Reset(pPacket->m_pData, pPacket->m_DataSize);
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

	if(m_aClients[ClientId].m_Sixup && (Msg = MsgFromSixup(Msg, Sys)) < 0)
	{
		return;
	}

	if(Config()->m_SvNetlimit && Msg != NETMSG_REQUEST_MAP_DATA)
	{
		int64_t Now = time_get();
		int64_t Diff = Now - m_aClients[ClientId].m_TrafficSince;
		double Alpha = Config()->m_SvNetlimitAlpha / 100.0;
		double Limit = (double)(Config()->m_SvNetlimit * 1024) / time_freq();

		if(m_aClients[ClientId].m_Traffic > Limit)
		{
			m_NetServer.NetBan()->BanAddr(&pPacket->m_Address, 600, "Stressing network", false);
			return;
		}
		if(Diff > 100)
		{
			m_aClients[ClientId].m_Traffic = (Alpha * ((double)pPacket->m_DataSize / Diff)) + (1.0 - Alpha) * m_aClients[ClientId].m_Traffic;
			m_aClients[ClientId].m_TrafficSince = Now;
		}
	}

	if(Result == UNPACKMESSAGE_ANSWER)
	{
		SendMsg(&Packer, MSGFLAG_VITAL, ClientId);
	}

	{
		bool VitalFlag = (pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0;
		bool NonVitalMsg = Sys && (Msg == NETMSG_INPUT || Msg == NETMSG_PING || Msg == NETMSG_PINGEX);
		if(!VitalFlag && !NonVitalMsg)
		{
			if(g_Config.m_Debug)
			{
				log_debug(
					"server",
					"strange message ClientId=%d msg=%d data_size=%d (missing vital flag)",
					ClientId,
					Msg,
					pPacket->m_DataSize);
			}
			return;
		}
	}

	if(Sys)
	{
		// system message
		if(Msg == NETMSG_CLIENTVER)
		{
			CUuid *pConnectionId = (CUuid *)Unpacker.GetRaw(sizeof(*pConnectionId));
			int DDNetVersion = Unpacker.GetInt();
			const char *pDDNetVersionStr = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(Unpacker.Error())
				return;

			OnNetMsgClientVer(ClientId, pConnectionId, DDNetVersion, pDDNetVersionStr);
		}
		else if(Msg == NETMSG_INFO)
		{
			const char *pVersion = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(Unpacker.Error())
				return;
			const char *pPassword = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(Unpacker.Error())
				pPassword = nullptr;

			OnNetMsgInfo(ClientId, pVersion, pPassword);
		}
		else if(Msg == NETMSG_REQUEST_MAP_DATA)
		{
			if(m_aClients[ClientId].m_State < CClient::STATE_CONNECTING)
				return;
			if(m_aClients[ClientId].m_Quic)
			{
				if(!m_aClients[ClientId].m_Sixup)
				{
					const int Chunk = Unpacker.GetInt();
					if(Unpacker.Error() || Chunk != 0)
						return;
				}
				if(m_aClients[ClientId].m_NextMapChunk != 0)
					return;
				const uint32_t MapType = m_aClients[ClientId].m_Sixup ? MAP_TYPE_SIXUP : MAP_TYPE_SIX;
				if(!m_QuicTransport.SendMap(m_aClients[ClientId].m_QuicSession, MapType))
				{
					m_QuicTransport.Close(m_aClients[ClientId].m_QuicSession, "map stream queue full");
					return;
				}
				m_aClients[ClientId].m_NextMapChunk = -1;
				return;
			}

			if(m_aClients[ClientId].m_Sixup)
			{
				for(int i = 0; i < Config()->m_SvMapWindow; i++)
				{
					SendMapData(ClientId, m_aClients[ClientId].m_NextMapChunk++);
				}
				return;
			}

			int Chunk = Unpacker.GetInt();
			if(Unpacker.Error())
			{
				return;
			}
			if(Chunk != m_aClients[ClientId].m_NextMapChunk || !Config()->m_SvFastDownload)
			{
				SendMapData(ClientId, Chunk);
				return;
			}

			if(Chunk == 0)
			{
				for(int i = 0; i < Config()->m_SvMapWindow; i++)
				{
					SendMapData(ClientId, i);
				}
			}
			SendMapData(ClientId, Config()->m_SvMapWindow + m_aClients[ClientId].m_NextMapChunk);
			m_aClients[ClientId].m_NextMapChunk++;
		}
		else if(Msg == NETMSG_READY)
		{
			OnNetMsgReady(ClientId);
		}
		else if(Msg == NETMSG_ENTERGAME)
		{
			OnNetMsgEnterGame(ClientId);
		}
		else if(Msg == NETMSG_INPUT)
		{
			if(pPacket->m_Flags & NET_CHUNKFLAG_VITAL)
			{
				return;
			}
			if(m_aClients[ClientId].m_State != CClient::STATE_INGAME)
			{
				return;
			}

			const int LastAckedSnapshot = Unpacker.GetInt();
			if(Unpacker.Error() ||
				LastAckedSnapshot < -1 ||
				LastAckedSnapshot > Tick())
			{
				return;
			}

			int IntendedTick = Unpacker.GetInt();
			if(Unpacker.Error() ||
				IntendedTick < MIN_TICK ||
				IntendedTick > MAX_TICK)
			{
				return;
			}

			const int Size = Unpacker.GetInt();
			if(Unpacker.Error() ||
				Size % (int)sizeof(int32_t) != 0 ||
				Size / (int)sizeof(int32_t) < MIN_INPUT_SIZE ||
				Size / (int)sizeof(int32_t) > MAX_INPUT_SIZE)
			{
				return;
			}

			const int PreviousAckedSnapshot = m_aClients[ClientId].m_LastAckedSnapshot;
			m_aClients[ClientId].m_LastAckedSnapshot = LastAckedSnapshot;
			if(m_aClients[ClientId].m_LastAckedSnapshot >= MIN_TICK)
			{
				m_aClients[ClientId].m_SnapRate = CClient::SNAPRATE_FULL;

				int64_t TagTime;
				if(m_aClients[ClientId].m_Snapshots.Get(m_aClients[ClientId].m_LastAckedSnapshot, &TagTime, nullptr, nullptr) >= 0)
				{
					const int64_t Age = time_get() - TagTime;
					m_aClients[ClientId].m_Latency = (int)((Age * 1000) / time_freq());
					if(Config()->m_SvTestingCommands && LastAckedSnapshot > PreviousAckedSnapshot && m_vBaselineSnapshotAckAgeMicroseconds.size() < 10000)
						m_vBaselineSnapshotAckAgeMicroseconds.push_back(Age * 1000000 / time_freq());
				}
			}

			// add message to report the input timing
			// skip packets that are old
			if(IntendedTick > m_aClients[ClientId].m_LastInputTick)
			{
				const int TimeLeft = (TickStartTime(IntendedTick) - time_get()) / (time_freq() / 1000);

				CMsgPacker Msgp(NETMSG_INPUTTIMING, true);
				Msgp.AddInt(IntendedTick);
				Msgp.AddInt(TimeLeft);
				SendMsg(&Msgp, 0, ClientId);
			}
			m_aClients[ClientId].m_LastInputTick = IntendedTick;

			IntendedTick = std::max(IntendedTick, Tick() + 1);

			CClient::CInput *pInput = &m_aClients[ClientId].m_aInputs[m_aClients[ClientId].m_CurrentInput];
			pInput->m_GameTick = IntendedTick;
			for(int i = 0; i < Size / (int)sizeof(int32_t); i++)
			{
				pInput->m_aData[i] = Unpacker.GetInt();
			}
			if(Unpacker.Error())
			{
				return;
			}
			pInput->m_ReceiveTime = time_get();

			if(g_Config.m_SvPreInput &&
				IntendedTick <= Tick() + 4 * TickSpeed() + 1 &&
				TakePreInputBudget(ClientId))
			{
				// send preinputs of ClientId to valid clients
				bool aPreInputClients[MAX_CLIENTS] = {};
				GameServer()->PreInputClients(ClientId, aPreInputClients);

				CNetMsg_Sv_PreInput PreInput = {};
				mem_zero(&PreInput, sizeof(PreInput));
				CNetObj_PlayerInput *pInputData = (CNetObj_PlayerInput *)&pInput->m_aData;

				PreInput.m_Direction = pInputData->m_Direction;
				PreInput.m_Jump = pInputData->m_Jump;
				PreInput.m_Fire = pInputData->m_Fire;
				PreInput.m_Hook = pInputData->m_Hook;
				PreInput.m_WantedWeapon = pInputData->m_WantedWeapon;
				PreInput.m_NextWeapon = pInputData->m_NextWeapon;
				PreInput.m_PrevWeapon = pInputData->m_PrevWeapon;

				if(mem_comp(&m_aClients[ClientId].m_LastPreInput, &PreInput, sizeof(CNetMsg_Sv_PreInput)) != 0)
				{
					m_aClients[ClientId].m_LastPreInput = PreInput;

					PreInput.m_Owner = ClientId;
					PreInput.m_IntendedTick = IntendedTick;

					// target angle isn't updated all the time to save bandwidth
					PreInput.m_TargetX = pInputData->m_TargetX;
					PreInput.m_TargetY = pInputData->m_TargetY;

					for(int Id = 0; Id < MAX_CLIENTS; Id++)
					{
						if(!aPreInputClients[Id])
							continue;
						if(m_aClients[Id].m_SnapRate != CClient::SNAPRATE_FULL)
							continue;

						if(!Translate(PreInput.m_Owner, Id))
							continue;

						SendPackMsg(&PreInput, MSGFLAG_FLUSH | MSGFLAG_NORECORD, Id);
						// Reset for others after translating and sending
						PreInput.m_Owner = ClientId;
					}
				}
			}

			GameServer()->OnClientPrepareInput(ClientId, pInput->m_aData);
			mem_copy(m_aClients[ClientId].m_LatestInput.m_aData, pInput->m_aData, sizeof(m_aClients[ClientId].m_LatestInput.m_aData));

			m_aClients[ClientId].m_CurrentInput++;
			m_aClients[ClientId].m_CurrentInput %= 200;

			// call the mod with the fresh input data
			GameServer()->OnClientDirectInput(ClientId, m_aClients[ClientId].m_LatestInput.m_aData);
		}
		else if(Msg == NETMSG_RCON_CMD)
		{
			const char *pCmd = Unpacker.GetString();
			if(Unpacker.Error())
				return;

			OnNetMsgRconCmd(ClientId, pCmd);
		}
		else if(Msg == NETMSG_RCON_AUTH)
		{
			const char *pName = "";
			if(!IsSixup(ClientId))
				pName = Unpacker.GetString(CUnpacker::SANITIZE_CC); // login name, now used
			const char *pPw = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			bool SendRconCmds = true;
			if(!IsSixup(ClientId))
				SendRconCmds = Unpacker.GetInt() != 0;
			if(Unpacker.Error())
				return;

			OnNetMsgRconAuth(ClientId, pName, pPw, SendRconCmds);
		}
		else if(Msg == NETMSG_PING)
		{
			CMsgPacker Msgp(NETMSG_PING_REPLY, true);
			SendMsg(&Msgp, ((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) ? MSGFLAG_VITAL : 0) | MSGFLAG_FLUSH, ClientId);
		}
		else if(Msg == NETMSG_PINGEX)
		{
			CUuid *pId = (CUuid *)Unpacker.GetRaw(sizeof(*pId));
			if(Unpacker.Error())
			{
				return;
			}
			CMsgPacker Msgp(NETMSG_PONGEX, true);
			Msgp.AddRaw(pId, sizeof(*pId));
			SendMsg(&Msgp, ((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) ? MSGFLAG_VITAL : 0) | MSGFLAG_FLUSH, ClientId);
		}
		else
		{
			if(Config()->m_Debug)
			{
				constexpr int MaxDumpedDataSize = 32;
				char aBuf[MaxDumpedDataSize * 3 + 1];
				str_hex(aBuf, sizeof(aBuf), pPacket->m_pData, std::min(pPacket->m_DataSize, MaxDumpedDataSize));

				char aBufMsg[256];
				str_format(aBufMsg, sizeof(aBufMsg), "strange message ClientId=%d msg=%d data_size=%d", ClientId, Msg, pPacket->m_DataSize);
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBufMsg);
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
			}
		}
	}
	else if(m_aClients[ClientId].m_State >= CClient::STATE_READY)
	{
		// game message
		GameServer()->OnMessage(Msg, &Unpacker, ClientId);
	}
}

void CServer::OnNetMsgClientVer(int ClientId, CUuid *pConnectionId, int DDNetVersion, const char *pDDNetVersionStr)
{
	if(m_aClients[ClientId].m_State != CClient::STATE_PREAUTH)
		return;
	if(DDNetVersion < 0)
		return;

	m_aClients[ClientId].m_ConnectionId = *pConnectionId;
	m_aClients[ClientId].m_DDNetVersion = DDNetVersion;
	str_copy(m_aClients[ClientId].m_aDDNetVersionStr, pDDNetVersionStr);
	m_aClients[ClientId].m_DDNetVersionSettled = true;
	m_aClients[ClientId].m_GotDDNetVersionPacket = true;
	m_aClients[ClientId].m_State = CClient::STATE_AUTH;
}

void CServer::OnNetMsgInfo(int ClientId, const char *pVersion, const char *pPasswordOrNullptr)
{
	if((m_aClients[ClientId].m_State != CClient::STATE_PREAUTH && m_aClients[ClientId].m_State != CClient::STATE_AUTH))
		return;

	if(str_comp(pVersion, GameServer()->NetVersion()) != 0 && str_comp(pVersion, "0.7 802f1be60a05665f") != 0)
	{
		// wrong version
		char aReason[256];
		str_format(aReason, sizeof(aReason), "Wrong version. Server is running '%s' and client '%s'", GameServer()->NetVersion(), pVersion);
		DropClient(ClientId, aReason);
		return;
	}

	const char *pPassword = pPasswordOrNullptr;
	if(!pPassword)
		return;

	if(Config()->m_Password[0] != 0 && str_comp(Config()->m_Password, pPassword) != 0)
	{
		// wrong password
		DropClient(ClientId, "Wrong password");
		return;
	}

	int NumConnectedClients = 0;
	for(int i = 0; i < MaxClients(); ++i)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			NumConnectedClients++;
		}
	}

	// reserved slot
	if(NumConnectedClients > MaxClients() - Config()->m_SvReservedSlots && !CheckReservedSlotAuth(ClientId, pPassword))
	{
		DropClient(ClientId, "This server is full");
		return;
	}

	m_aClients[ClientId].m_State = CClient::STATE_CONNECTING;
	SendRconType(ClientId, m_AuthManager.NumNonDefaultKeys() > 0);
	SendCapabilities(ClientId);
	SendMap(ClientId);
}

void CServer::OnNetMsgReady(int ClientId)
{
	if(m_aClients[ClientId].m_State == CClient::STATE_CONNECTING)
	{
		log_debug(
			"server",
			"player is ready. ClientId=%d addr=<{%s}> transport=%s secure=%s",
			ClientId,
			ClientAddrString(ClientId, true),
			ClientTransportName(ClientId),
			m_NetServer.HasSecurityToken(ClientId) ? "yes" : "no");

		void *pPersistentData = nullptr;
		if(m_aClients[ClientId].m_HasPersistentData)
		{
			pPersistentData = m_aClients[ClientId].m_pPersistentData;
			m_aClients[ClientId].m_HasPersistentData = false;
		}
		m_aClients[ClientId].m_State = CClient::STATE_READY;
		GameServer()->OnClientConnected(ClientId, pPersistentData);
	}

	// Make rejoining session possible before timeout protection triggers
	// https://github.com/ddnet/ddnet/pull/301
	SendConnectionReady(ClientId);
}

void CServer::OnNetMsgEnterGame(int ClientId)
{
	if(m_aClients[ClientId].m_State != CClient::STATE_READY)
		return;
	if(!GameServer()->IsClientReady(ClientId))
		return;

	log_info(
		"server",
		"player has entered the game. ClientId=%d addr=<{%s}> transport=%s sixup=%d",
		ClientId,
		ClientAddrString(ClientId, true),
		ClientTransportName(ClientId),
		IsSixup(ClientId));
	m_aClients[ClientId].m_State = CClient::STATE_INGAME;
	if(!IsSixup(ClientId))
	{
		SendServerInfo(ClientAddr(ClientId), -1, SERVERINFO_EXTENDED, false);
	}
	else
	{
		CMsgPacker ServerInfoMessage(protocol7::NETMSG_SERVERINFO, true, true);
		GetServerInfoSixup(&ServerInfoMessage, false);
		SendMsg(&ServerInfoMessage, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientId);
	}
	GameServer()->OnClientEnter(ClientId);
	if(m_aClients[ClientId].m_Quic && !IssueQuicResume(ClientId))
		DropClient(ClientId, "Could not issue QUIC resume token");
}

void CServer::OnNetMsgRconCmd(int ClientId, const char *pCmd)
{
	if(!str_comp(pCmd, "crashmeplx"))
	{
		int Version = m_aClients[ClientId].m_DDNetVersion;
		if(GameServer()->PlayerExists(ClientId) && Version < VERSION_DDNET_OLD)
		{
			m_aClients[ClientId].m_DDNetVersion = VERSION_DDNET_OLD;
			GameServer()->ReinitPlayerMap(ClientId, false);
		}
	}
	else if(IsRconAuthed(ClientId))
	{
		if(GameServer()->PlayerExists(ClientId))
		{
			log_info("server", "ClientId=%d key='%s' rcon='%s'", ClientId, GetAuthName(ClientId), pCmd);
			m_RconClientId = ClientId;
			m_RconAuthLevel = GetAuthedState(ClientId);
			{
				CRconClientLogger Logger(this, ClientId);
				CLogScope Scope(&Logger);
				Console()->ExecuteLineFlag(pCmd, CFGFLAG_SERVER, ClientId);
			}
			m_RconClientId = IServer::RCON_CID_SERV;
			m_RconAuthLevel = AUTHED_ADMIN;
		}
	}
}

void CServer::OnNetMsgRconAuth(int ClientId, const char *pName, const char *pPw, bool SendRconCmds)
{
	int AuthLevel = -1;
	int KeySlot = -1;

	if(!pName[0])
	{
		if(m_AuthManager.CheckKey((KeySlot = m_AuthManager.DefaultKey(RoleName::ADMIN)), pPw))
			AuthLevel = AUTHED_ADMIN;
		else if(m_AuthManager.CheckKey((KeySlot = m_AuthManager.DefaultKey(RoleName::MODERATOR)), pPw))
			AuthLevel = AUTHED_MOD;
		else if(m_AuthManager.CheckKey((KeySlot = m_AuthManager.DefaultKey(RoleName::HELPER)), pPw))
			AuthLevel = AUTHED_HELPER;
	}
	else
	{
		KeySlot = m_AuthManager.FindKey(pName);
		if(m_AuthManager.CheckKey(KeySlot, pPw))
			AuthLevel = m_AuthManager.KeyLevel(KeySlot);
	}

	if(AuthLevel != -1)
	{
		if(GetAuthedState(ClientId) != AuthLevel)
		{
			if(!IsSixup(ClientId))
			{
				CMsgPacker Msgp(NETMSG_RCON_AUTH_STATUS, true);
				Msgp.AddInt(1); //authed
				Msgp.AddInt(1); //cmdlist
				SendMsg(&Msgp, MSGFLAG_VITAL, ClientId);
			}
			else
			{
				CMsgPacker Msgp(protocol7::NETMSG_RCON_AUTH_ON, true, true);
				SendMsg(&Msgp, MSGFLAG_VITAL, ClientId);
			}

			m_aClients[ClientId].m_AuthKey = KeySlot;
			if(SendRconCmds)
			{
				m_aClients[ClientId].m_pRconCmdToSend = Console()->FirstCommandInfo(ClientId, CFGFLAG_SERVER);
				SendRconCmdGroupStart(ClientId);
				if(m_aClients[ClientId].m_pRconCmdToSend == nullptr)
				{
					SendRconCmdGroupEnd(ClientId);
				}
			}

			const char *pIdent = m_AuthManager.KeyIdent(KeySlot);
			switch(AuthLevel)
			{
			case AUTHED_ADMIN:
			{
				SendRconLine(ClientId, "Admin authentication successful. Full remote console access granted.");
				log_info("server", "ClientId=%d authed with key='%s' (admin)", ClientId, pIdent);
				break;
			}
			case AUTHED_MOD:
			{
				SendRconLine(ClientId, "Moderator authentication successful. Limited remote console access granted.");
				log_info("server", "ClientId=%d authed with key='%s' (moderator)", ClientId, pIdent);
				break;
			}
			case AUTHED_HELPER:
			{
				SendRconLine(ClientId, "Helper authentication successful. Limited remote console access granted.");
				log_info("server", "ClientId=%d authed with key='%s' (helper)", ClientId, pIdent);
				break;
			}
			}

			// DDRace
			GameServer()->OnSetAuthed(ClientId, AuthLevel);
		}
	}
	else if(Config()->m_SvRconMaxTries)
	{
		m_aClients[ClientId].m_AuthTries++;
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Wrong password %d/%d.", m_aClients[ClientId].m_AuthTries, Config()->m_SvRconMaxTries);
		SendRconLine(ClientId, aBuf);
		if(m_aClients[ClientId].m_AuthTries >= Config()->m_SvRconMaxTries)
		{
			if(!Config()->m_SvRconBantime)
				DropClient(ClientId, "Too many remote console authentication tries");
			else
				m_ServerBan.BanAddr(ClientAddr(ClientId), Config()->m_SvRconBantime * 60, "Too many remote console authentication tries", false);
		}
	}
	else
	{
		SendRconLine(ClientId, "Wrong password.");
	}
}

std::optional<bool> CServer::RateLimitServerInfoConnless()
{
	const int64_t Now = time_get();
	if(Now > m_ServerInfoFirstRequest + time_freq())
	{
		m_ServerInfoFirstRequest = Now;
		m_ServerInfoNumRequests = 0;
	}
	m_ServerInfoNumRequests++;

	// In 0.6 the requesting address is not verified, so responses go to whoever the
	// request claims to be from, and they are larger than the request.
	if(Config()->m_SvServerInfoRepliesPerSecond != 0 &&
		m_ServerInfoNumRequests > Config()->m_SvServerInfoRepliesPerSecond)
	{
		return std::nullopt;
	}
	return Config()->m_SvServerInfoPerSecond == 0 ||
	       m_ServerInfoNumRequests <= Config()->m_SvServerInfoPerSecond;
}

static inline int GetCacheIndex(int Type, bool SendClient)
{
	if(Type == SERVERINFO_INGAME)
		Type = SERVERINFO_VANILLA;
	else if(Type == SERVERINFO_EXTENDED_MORE)
		Type = SERVERINFO_EXTENDED;

	return Type * 2 + SendClient;
}

CServer::CCache::CCache()
{
	m_vCache.clear();
}

CServer::CCache::~CCache()
{
	Clear();
}

CServer::CCache::CCacheChunk::CCacheChunk(const void *pData, int Size)
{
	m_vData.assign((const uint8_t *)pData, (const uint8_t *)pData + Size);
}

void CServer::CCache::AddChunk(const void *pData, int Size)
{
	m_vCache.emplace_back(pData, Size);
}

void CServer::CCache::Clear()
{
	m_vCache.clear();
}

const SHA256_DIGEST *CServer::SharedQuicCertificateSha256() const
{
	if(!m_QuicStarted)
		return nullptr;
	return m_QuicTransport.CertificateSha256();
}

void CServer::CacheServerInfo(CCache *pCache, int Type, bool SendClients)
{
	pCache->Clear();

	// One chance to improve the protocol!
	CPacker p;
	char aBuf[128];

	// count the players
	int PlayerCount = 0, ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			if(GameServer()->IsClientPlayer(i))
				PlayerCount++;

			ClientCount++;
		}
	}

	p.Reset();

#define ADD_RAW(p, x) (p).AddRaw(x, sizeof(x))
#define ADD_INT(p, x) \
	do \
	{ \
		str_format(aBuf, sizeof(aBuf), "%d", x); \
		(p).AddString(aBuf, 0); \
	} while(0)

	p.AddString(GameServer()->Version(), 32);
	if(Type != SERVERINFO_VANILLA)
	{
		p.AddString(Config()->m_SvName, 256);
	}
	else
	{
		if(m_NetServer.MaxClients() <= VANILLA_MAX_CLIENTS)
		{
			p.AddString(Config()->m_SvName, 64);
		}
		else
		{
			const int MaxClients = std::max(ClientCount, m_NetServer.MaxClients() - Config()->m_SvReservedSlots);
			str_format(aBuf, sizeof(aBuf), "%s [%d/%d]", Config()->m_SvName, ClientCount, MaxClients);
			p.AddString(aBuf, 64);
		}
	}
	p.AddString(GameServer()->Map()->BaseName(), 32);

	if(Type == SERVERINFO_EXTENDED)
	{
		ADD_INT(p, m_aCurrentMapCrc[MAP_TYPE_SIX]);
		ADD_INT(p, m_aCurrentMapSize[MAP_TYPE_SIX]);
	}

	// gametype
	p.AddString(GameServer()->GameType(), 16);

	// flags
	ADD_INT(p, Config()->m_Password[0] ? SERVER_FLAG_PASSWORD : 0);

	int MaxClients = m_NetServer.MaxClients();
	// How many clients the used serverinfo protocol supports, has to be tracked
	// separately to make sure we don't subtract the reserved slots from it
	int MaxClientsProtocol = MAX_CLIENTS;
	if(Type == SERVERINFO_VANILLA || Type == SERVERINFO_INGAME)
	{
		if(ClientCount >= VANILLA_MAX_CLIENTS)
		{
			if(ClientCount < MaxClients)
				ClientCount = VANILLA_MAX_CLIENTS - 1;
			else
				ClientCount = VANILLA_MAX_CLIENTS;
		}
		MaxClientsProtocol = VANILLA_MAX_CLIENTS;
		if(PlayerCount > ClientCount)
			PlayerCount = ClientCount;
	}

	ADD_INT(p, PlayerCount); // num players
	ADD_INT(p, std::min(MaxClientsProtocol, std::max(MaxClients - std::max(Config()->m_SvSpectatorSlots, Config()->m_SvReservedSlots), PlayerCount))); // max players
	ADD_INT(p, ClientCount); // num clients
	ADD_INT(p, std::min(MaxClientsProtocol, std::max(MaxClients - Config()->m_SvReservedSlots, ClientCount))); // max clients

	if(Type == SERVERINFO_EXTENDED)
	{
		char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE] = {};
		// There is no master server on a LAN, so this answer is the only place a
		// client learns about the modern transports. WebTransport runs without an
		// identity binding, so a server that serves it alone has to be able to say
		// so without one.
		const CServerIdentityBinding *pIdentity = m_QuicStarted ? m_QuicTransport.ServerIdentity() : nullptr;
		if(pIdentity != nullptr || m_WebTransportStarted)
		{
			CQuicServerInfoExtra Extra = {};
			Extra.m_RawQuic = pIdentity != nullptr;
			if(pIdentity != nullptr)
				Extra.m_IdentityFingerprint = sha256(pIdentity->m_PublicKey.data(), pIdentity->m_PublicKey.size());
			Extra.m_WebTransport = m_WebTransportStarted;
			Extra.m_pHostname = m_aModernTransportHostname;
			if(m_WebTransportStarted && m_WebTransportUseCertificateHashes)
			{
				Extra.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::HASH;
				if(const SHA256_DIGEST *pCertificate = m_QuicTransport.CertificateSha256())
					Extra.m_WebTransportCertificateSha256 = *pCertificate;
				if(const SHA256_DIGEST *pNextCertificate = m_QuicTransport.NextCertificateSha256())
				{
					Extra.m_WebTransportNextCertificateSha256 = *pNextCertificate;
					Extra.m_HasWebTransportNextCertificateSha256 = true;
				}
			}
			else if(m_WebTransportStarted)
				Extra.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::WEBPKI;
			FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
		}
		p.AddString(aExtraInfo, sizeof(aExtraInfo), false);
	}

	const void *pPrefix = p.Data();
	int PrefixSize = p.Size();

	CPacker q;
	int ChunksStored = 0;
	int PlayersStored = 0;

#define SAVE(size) \
	do \
	{ \
		pCache->AddChunk(q.Data(), size); \
		ChunksStored++; \
	} while(0)

#define RESET() \
	do \
	{ \
		q.Reset(); \
		q.AddRaw(pPrefix, PrefixSize); \
	} while(0)

	RESET();

	if(Type == SERVERINFO_64_LEGACY)
		q.AddInt(PlayersStored); // offset

	if(!SendClients)
	{
		SAVE(q.Size());
		return;
	}

	if(Type == SERVERINFO_EXTENDED)
	{
		pPrefix = "";
		PrefixSize = 0;
	}

	int Remaining;
	switch(Type)
	{
	case SERVERINFO_EXTENDED: Remaining = -1; break;
	case SERVERINFO_64_LEGACY: Remaining = 24; break;
	case SERVERINFO_VANILLA: Remaining = VANILLA_MAX_CLIENTS; break;
	case SERVERINFO_INGAME: Remaining = VANILLA_MAX_CLIENTS; break;
	default: dbg_assert_failed("Invalid Type: %d", Type);
	}

	// Use the following strategy for sending:
	// For vanilla, send the first 16 players.
	// For legacy 64p, send 24 players per packet.
	// For extended, send as much players as possible.

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			if(Remaining == 0)
			{
				if(Type == SERVERINFO_VANILLA || Type == SERVERINFO_INGAME)
					break;

				// Otherwise we're SERVERINFO_64_LEGACY.
				SAVE(q.Size());
				RESET();
				q.AddInt(PlayersStored); // offset
				Remaining = 24;
			}
			if(Remaining > 0)
			{
				Remaining--;
			}

			int PreviousSize = q.Size();

			q.AddString(ClientName(i), MAX_NAME_LENGTH); // client name
			q.AddString(ClientClan(i), MAX_CLAN_LENGTH); // client clan

			ADD_INT(q, m_aClients[i].m_Country); // client country (ISO 3166-1 numeric)

			int Score;
			if(m_aClients[i].m_Score.has_value())
			{
				Score = m_aClients[i].m_Score.value();
				if(Score == -FinishTime::NOT_FINISHED_TIMESCORE)
					Score = FinishTime::NOT_FINISHED_TIMESCORE - 1;
				else if(Score == 0) // 0 time isn't displayed otherwise.
					Score = -1;
				else
					Score = -Score;
			}
			else
			{
				Score = FinishTime::NOT_FINISHED_TIMESCORE;
			}

			ADD_INT(q, Score); // client score
			ADD_INT(q, GameServer()->IsClientPlayer(i) ? 1 : 0); // is player?
			if(Type == SERVERINFO_EXTENDED)
				q.AddString("", 0); // extra info, reserved

			if(Type == SERVERINFO_EXTENDED)
			{
				if(q.Size() >= NET_MAX_CONNLESS_PAYLOAD - 18) // 8 bytes for type, 10 bytes for the largest token
				{
					// Retry current player.
					i--;
					SAVE(PreviousSize);
					RESET();
					ADD_INT(q, ChunksStored);
					q.AddString("", 0); // extra info, reserved
					continue;
				}
			}
			PlayersStored++;
		}
	}

	SAVE(q.Size());
#undef SAVE
#undef RESET
#undef ADD_RAW
#undef ADD_INT
}

void CServer::CacheServerInfoSixup(CCache *pCache, bool SendClients, int MaxConsideredClients)
{
	pCache->Clear();

	CPacker Packer;
	Packer.Reset();

	// Could be moved to a separate function and cached
	// count the players
	int PlayerCount = 0, ClientCount = 0, ClientCountAll = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			ClientCountAll++;
			if(i < MaxConsideredClients)
			{
				if(GameServer()->IsClientPlayer(i))
					PlayerCount++;

				ClientCount++;
			}
		}
	}

	char aVersion[32];
	str_format(aVersion, sizeof(aVersion), "0.7↔%s", GameServer()->Version());
	Packer.AddString(aVersion, 32);
	if(!SendClients || ClientCountAll == ClientCount)
	{
		Packer.AddString(Config()->m_SvName, 64);
	}
	else
	{
		char aName[64];
		str_format(aName, sizeof(aName), "%s [%d/%d]", Config()->m_SvName, ClientCountAll, m_NetServer.MaxClients() - Config()->m_SvReservedSlots);
		Packer.AddString(aName, 64);
	}
	Packer.AddString(Config()->m_SvHostname, 128);
	Packer.AddString(GameServer()->Map()->BaseName(), 32);

	// gametype
	Packer.AddString(GameServer()->GameType(), 16);

	// flags
	int Flags = SERVER_FLAG_TIMESCORE;
	if(Config()->m_Password[0]) // password set
		Flags |= SERVER_FLAG_PASSWORD;
	Packer.AddInt(Flags);

	int MaxClients = m_NetServer.MaxClients();
	Packer.AddInt(Config()->m_SvSkillLevel); // server skill level
	Packer.AddInt(PlayerCount); // num players
	Packer.AddInt(std::max(MaxClients - std::max(Config()->m_SvSpectatorSlots, Config()->m_SvReservedSlots), PlayerCount)); // max players
	Packer.AddInt(ClientCount); // num clients
	Packer.AddInt(std::max(MaxClients - Config()->m_SvReservedSlots, ClientCount)); // max clients

	if(SendClients)
	{
		for(int i = 0; i < MaxConsideredClients; i++)
		{
			if(m_aClients[i].IncludedInServerInfo())
			{
				Packer.AddString(ClientName(i), MAX_NAME_LENGTH); // client name
				Packer.AddString(ClientClan(i), MAX_CLAN_LENGTH); // client clan
				Packer.AddInt(m_aClients[i].m_Country); // client country (ISO 3166-1 numeric)
				Packer.AddInt(m_aClients[i].m_Score.value_or(-1)); // client score
				Packer.AddInt(GameServer()->IsClientPlayer(i) ? 0 : 1); // flag spectator=1, bot=2 (player=0)

				const int MaxPacketSize = NET_MAX_CONNLESS_PAYLOAD - 128;
				if(MaxConsideredClients == MAX_CLIENTS)
				{
					if(Packer.Size() > MaxPacketSize - 32) // -32 because repacking will increase the length of the name
					{
						// Server info is too large for a packet. Only include as many clients as fit.
						// We need to ensure that the client counts match, otherwise the 0.7 client
						// will ignore the info, so we repack but only consider the first i clients.
						CacheServerInfoSixup(pCache, true, i);
						return;
					}
				}
				else
				{
					dbg_assert(Packer.Size() <= MaxPacketSize, "Max packet size exceeded while repacking. Packer.Size()=%d MaxPacketSize=%d", Packer.Size(), MaxPacketSize);
				}
			}
		}
	}

	pCache->AddChunk(Packer.Data(), Packer.Size());
}

void CServer::SendServerInfo(const NETADDR *pAddr, int Token, int Type, bool SendClients)
{
	CPacker p;
	char aBuf[128];
	p.Reset();

	CCache *pCache = &m_aServerInfoCache[GetCacheIndex(Type, SendClients)];

#define ADD_RAW(p, x) (p).AddRaw(x, sizeof(x))
#define ADD_INT(p, x) \
	do \
	{ \
		str_format(aBuf, sizeof(aBuf), "%d", x); \
		(p).AddString(aBuf, 0); \
	} while(0)

	CNetChunk Packet;
	Packet.m_ClientId = -1;
	Packet.m_Address = *pAddr;
	Packet.m_Flags = NETSENDFLAG_CONNLESS;

	for(const auto &Chunk : pCache->m_vCache)
	{
		p.Reset();
		if(Type == SERVERINFO_EXTENDED)
		{
			if(&Chunk == &pCache->m_vCache.front())
				p.AddRaw(SERVERBROWSE_INFO_EXTENDED, sizeof(SERVERBROWSE_INFO_EXTENDED));
			else
				p.AddRaw(SERVERBROWSE_INFO_EXTENDED_MORE, sizeof(SERVERBROWSE_INFO_EXTENDED_MORE));
			ADD_INT(p, Token);
		}
		else if(Type == SERVERINFO_64_LEGACY)
		{
			ADD_RAW(p, SERVERBROWSE_INFO_64_LEGACY);
			ADD_INT(p, Token);
		}
		else if(Type == SERVERINFO_VANILLA || Type == SERVERINFO_INGAME)
		{
			ADD_RAW(p, SERVERBROWSE_INFO);
			ADD_INT(p, Token);
		}
		else
		{
			dbg_assert_failed("Invalid serverinfo Type: %d", Type);
		}

		p.AddRaw(Chunk.m_vData.data(), Chunk.m_vData.size());
		Packet.m_pData = p.Data();
		Packet.m_DataSize = p.Size();
		m_NetServer.Send(&Packet);
	}
}

void CServer::GetServerInfoSixup(CPacker *pPacker, bool SendClients)
{
	CCache::CCacheChunk &FirstChunk = m_aSixupServerInfoCache[SendClients].m_vCache.front();
	pPacker->AddRaw(FirstChunk.m_vData.data(), FirstChunk.m_vData.size());
}

void CServer::FillAntibot(CAntibotRoundData *pData)
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CAntibotPlayerData *pPlayer = &pData->m_aPlayers[ClientId];
		if(m_aClients[ClientId].m_State == CServer::CClient::STATE_EMPTY)
		{
			pPlayer->m_aAddress[0] = '\0';
		}
		else
		{
			// No need for expensive str_copy since we don't truncate and the string is
			// ASCII anyway
			static_assert(std::size((CAntibotPlayerData{}).m_aAddress) >= NETADDR_MAXSTRSIZE);
			static_assert(std::is_same_v<decltype(CServer{}.ClientAddrStringImpl(ClientId, true)), const std::array<char, NETADDR_MAXSTRSIZE> &>);
			mem_copy(pPlayer->m_aAddress, ClientAddrStringImpl(ClientId, true).data(), NETADDR_MAXSTRSIZE);
			pPlayer->m_Sixup = m_aClients[ClientId].m_Sixup;
			pPlayer->m_DnsblNone = m_aClients[ClientId].m_DnsblState == EDnsblState::NONE;
			pPlayer->m_DnsblPending = m_aClients[ClientId].m_DnsblState == EDnsblState::PENDING;
			pPlayer->m_DnsblBlacklisted = m_aClients[ClientId].m_DnsblState == EDnsblState::BLACKLISTED;
			pPlayer->m_Authed = IsRconAuthed(ClientId);
		}
	}
}

void CServer::ExpireServerInfo()
{
	m_ServerInfoNeedsUpdate = true;
}

void CServer::ExpireServerInfoAndQueueResend()
{
	m_ServerInfoNeedsUpdate = true;
	m_ServerInfoNeedsResend = true;
}

void CServer::UpdateRegisterServerInfo()
{
	// count the players
	int PlayerCount = 0, ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			if(GameServer()->IsClientPlayer(i))
				PlayerCount++;

			ClientCount++;
		}
	}

	int MaxPlayers = std::max(m_NetServer.MaxClients() - std::max(g_Config.m_SvSpectatorSlots, g_Config.m_SvReservedSlots), PlayerCount);
	int MaxClients = std::max(m_NetServer.MaxClients() - g_Config.m_SvReservedSlots, ClientCount);
	char aMapSha256[SHA256_MAXSTRSIZE];

	sha256_str(m_aCurrentMapSha256[MAP_TYPE_SIX], aMapSha256, sizeof(aMapSha256));

	CJsonStringWriter JsonWriter;

	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("max_clients");
	JsonWriter.WriteIntValue(MaxClients);

	JsonWriter.WriteAttribute("max_players");
	JsonWriter.WriteIntValue(MaxPlayers);

	JsonWriter.WriteAttribute("passworded");
	JsonWriter.WriteBoolValue(g_Config.m_Password[0]);

	JsonWriter.WriteAttribute("game_type");
	JsonWriter.WriteStrValue(GameServer()->GameType());

	if(g_Config.m_SvRegisterCommunityToken[0])
	{
		if(g_Config.m_SvFlag != -1)
		{
			JsonWriter.WriteAttribute("country");
			JsonWriter.WriteIntValue(g_Config.m_SvFlag); // ISO 3166-1 numeric
		}
	}

	JsonWriter.WriteAttribute("name");
	JsonWriter.WriteStrValue(g_Config.m_SvName);

	JsonWriter.WriteAttribute("map");
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("name");
	JsonWriter.WriteStrValue(GameServer()->Map()->BaseName());
	JsonWriter.WriteAttribute("sha256");
	JsonWriter.WriteStrValue(aMapSha256);
	JsonWriter.WriteAttribute("size");
	JsonWriter.WriteIntValue(m_aCurrentMapSize[MAP_TYPE_SIX]);
	if(m_aMapDownloadUrl[0])
	{
		JsonWriter.WriteAttribute("url");
		JsonWriter.WriteStrValue(m_aMapDownloadUrl);
	}
	JsonWriter.EndObject();

	JsonWriter.WriteAttribute("version");
	JsonWriter.WriteStrValue(GameServer()->Version());

	JsonWriter.WriteAttribute("client_score_kind");
	JsonWriter.WriteStrValue("time"); // "points" or "time"

	JsonWriter.WriteAttribute("requires_login");
	JsonWriter.WriteBoolValue(false);

	const SHA256_DIGEST *pCertificateSha256 = m_QuicStarted || m_WebTransportStarted ? m_QuicTransport.CertificateSha256() : nullptr;
	if(pCertificateSha256)
	{
		char aCertificateSha256[SHA256_MAXSTRSIZE];
		sha256_str(*pCertificateSha256, aCertificateSha256, sizeof(aCertificateSha256));
		char aNextCertificateSha256[SHA256_MAXSTRSIZE] = {};
		if(const SHA256_DIGEST *pNextCertificateSha256 = m_QuicTransport.NextCertificateSha256())
			sha256_str(*pNextCertificateSha256, aNextCertificateSha256, sizeof(aNextCertificateSha256));
		JsonWriter.WriteAttribute("experimental");
		JsonWriter.BeginObject();
		JsonWriter.WriteAttribute("proto");
		JsonWriter.BeginObject();
		if(m_aModernTransportHostname[0] != '\0')
		{
			JsonWriter.WriteAttribute("hostname");
			JsonWriter.WriteStrValue(m_aModernTransportHostname);
		}
		if(m_QuicStarted)
		{
			JsonWriter.WriteAttribute("quic");
			JsonWriter.BeginObject();
			JsonWriter.WriteAttribute("verify");
			JsonWriter.WriteStrValue(m_QuicUseWebPki ? "webpki" : "identity");
			if(!m_QuicUseWebPki)
			{
				const CServerIdentityBinding *pIdentity = m_QuicTransport.ServerIdentity();
				dbg_assert(pIdentity != nullptr, "QUIC server identity must be available");
				char aIdentityFingerprint[SHA256_MAXSTRSIZE];
				sha256_str(sha256(pIdentity->m_PublicKey.data(), pIdentity->m_PublicKey.size()), aIdentityFingerprint, sizeof(aIdentityFingerprint));
				JsonWriter.WriteAttribute("sha256");
				JsonWriter.WriteStrValue(aIdentityFingerprint);
			}
			JsonWriter.EndObject();
		}
		if(m_WebTransportStarted)
		{
			JsonWriter.WriteAttribute("webtransport");
			JsonWriter.BeginObject();
			JsonWriter.WriteAttribute("verify");
			JsonWriter.WriteStrValue(m_WebTransportUseCertificateHashes ? "hash" : "webpki");
			if(m_WebTransportUseCertificateHashes)
			{
				JsonWriter.WriteAttribute("sha256");
				JsonWriter.BeginArray();
				JsonWriter.WriteStrValue(aCertificateSha256);
				if(aNextCertificateSha256[0] != '\0')
					JsonWriter.WriteStrValue(aNextCertificateSha256);
				JsonWriter.EndArray();
			}
			JsonWriter.EndObject();
		}
		JsonWriter.EndObject();
		JsonWriter.EndObject();
	}

	{
		bool FoundFlags = false;
		auto Flag = [&](const char *pFlag) {
			if(!FoundFlags)
			{
				JsonWriter.WriteAttribute("flags");
				JsonWriter.BeginArray();
				FoundFlags = true;
			}
			JsonWriter.WriteStrValue(pFlag);
		};

		if(g_Config.m_SvRegisterCommunityToken[0] && g_Config.m_SvOfficialTutorial[0])
		{
			SHA256_DIGEST Sha256 = sha256(g_Config.m_SvOfficialTutorial, str_length(g_Config.m_SvOfficialTutorial));
			char aSha256[SHA256_MAXSTRSIZE];
			sha256_str(Sha256, aSha256, sizeof(aSha256));
			if(str_comp(aSha256, "8a11dc71274313e78a09ff58b8e696fb5009ce8606d12077ceb78ebf99a57464") == 0)
			{
				Flag("tutorial");
			}
		}

		if(FoundFlags)
		{
			JsonWriter.EndArray();
		}
	}

	JsonWriter.WriteAttribute("clients");
	JsonWriter.BeginArray();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			JsonWriter.BeginObject();

			JsonWriter.WriteAttribute("name");
			JsonWriter.WriteStrValue(ClientName(i));

			JsonWriter.WriteAttribute("clan");
			JsonWriter.WriteStrValue(ClientClan(i));

			JsonWriter.WriteAttribute("country");
			JsonWriter.WriteIntValue(m_aClients[i].m_Country); // ISO 3166-1 numeric

			JsonWriter.WriteAttribute("score");
			JsonWriter.WriteIntValue(m_aClients[i].m_Score.value_or(FinishTime::NOT_FINISHED_TIMESCORE));

			JsonWriter.WriteAttribute("is_player");
			JsonWriter.WriteBoolValue(GameServer()->IsClientPlayer(i));

			GameServer()->OnUpdatePlayerServerInfo(&JsonWriter, i);

			JsonWriter.EndObject();
		}
	}

	JsonWriter.EndArray();
	JsonWriter.EndObject();

	m_pRegister->OnNewInfo(JsonWriter.GetOutputString().c_str());
}

void CServer::UpdateServerInfo(bool Resend)
{
	if(m_RunServer == UNINITIALIZED)
		return;

	UpdateRegisterServerInfo();

	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 2; j++)
			CacheServerInfo(&m_aServerInfoCache[i * 2 + j], i, j);

	for(int i = 0; i < 2; i++)
		CacheServerInfoSixup(&m_aSixupServerInfoCache[i], i, MAX_CLIENTS);

	if(Resend)
	{
		for(int i = 0; i < MaxClients(); ++i)
		{
			if(m_aClients[i].m_State != CClient::STATE_EMPTY)
			{
				if(!IsSixup(i))
				{
					SendServerInfo(ClientAddr(i), -1, SERVERINFO_INGAME, false);
				}
				else
				{
					CMsgPacker ServerInfoMessage(protocol7::NETMSG_SERVERINFO, true, true);
					GetServerInfoSixup(&ServerInfoMessage, false);
					SendMsg(&ServerInfoMessage, MSGFLAG_VITAL | MSGFLAG_FLUSH, i);
				}
			}
		}
		m_ServerInfoNeedsResend = false;
	}

	m_ServerInfoNeedsUpdate = false;
}

int CServer::GetMaxClients(int ClientId) const
{
	// Shouldn't catch anything currently
	if(ClientId == SERVER_DEMO_CLIENT)
		return MAX_CLIENTS;

	if(m_aClients[ClientId].m_Sixup)
		return LEGACY_MAX_CLIENTS;
	if(m_aClients[ClientId].m_DDNetVersion >= VERSION_DDNET_128_PLAYERS)
		return MAX_CLIENTS;
	if(m_aClients[ClientId].m_DDNetVersion >= VERSION_DDNET_OLD)
		return LEGACY_MAX_CLIENTS;
	return VANILLA_MAX_CLIENTS;
}

bool CServer::ClientSupportsServerMaxClients(int ClientId) const
{
	// server demo pseudo clients operate on untranslated ids
	if(ClientId == SERVER_DEMO_CLIENT)
		return true;

	// We can use `m_NetServer.MaxClients()` instead of `MAX_CLIENTS` here because it can't be changed ingame.
	// The playermapping code currently relies on sixup (0.7) clients taking the route through playermapping.
	return GetMaxClients(ClientId) >= m_NetServer.MaxClients() && !m_aClients[ClientId].m_Sixup;
}

int CServer::FindQuicClient(CQuicSessionId Session) const
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aClients[ClientId].m_Quic && m_aClients[ClientId].m_QuicSession == Session)
			return ClientId;
	}
	return -1;
}

int CServer::FindQuicResume(const CQuicMessage &Message) const
{
	GameWire::CResumeView Resume = {};
	if(GameWire::DecodeResume(
		   {static_cast<const unsigned char *>(Message.m_pData), static_cast<size_t>(Message.m_DataSize)},
		   Resume) != GameWire::EDecodeResult::OK ||
		Resume.m_Token.m_Size != QUIC_RESUME_TOKEN_SIZE)
		return -1;
	const int64_t Now = time_get();
	for(int ClientId = 0; ClientId < MaxClients(); ++ClientId)
	{
		const CClient &Client = m_aClients[ClientId];
		if(!Client.m_QuicResumeArmed || Client.m_QuicResumeSessionId != Resume.m_SessionId ||
			(Client.m_QuicDetached && Now > Client.m_QuicResumeDeadline))
			continue;
		unsigned char Difference = 0;
		for(size_t i = 0; i < Client.m_aQuicResumeToken.size(); ++i)
			Difference |= Client.m_aQuicResumeToken[i] ^ Resume.m_Token.m_pData[i];
		if(Difference == 0)
			return ClientId;
	}
	return -1;
}

bool CServer::IssueQuicResume(int ClientId)
{
	CClient &Client = m_aClients[ClientId];
	if(!Client.m_Quic || !Client.m_QuicSession.IsValid())
		return false;
	if(Client.m_QuicResumeSessionId == 0)
	{
		do
		{
			secure_random_fill(&Client.m_QuicResumeSessionId, sizeof(Client.m_QuicResumeSessionId));
			Client.m_QuicResumeSessionId &= (uint64_t{1} << 62) - 1;
		} while(Client.m_QuicResumeSessionId == 0);
	}
	secure_random_fill(Client.m_aQuicResumeToken.data(), Client.m_aQuicResumeToken.size());
	Client.m_QuicResumeArmed = true;
	Client.m_QuicDetached = false;
	Client.m_QuicResumeDeadline = 0;
	if(m_QuicTransport.IssueResume(Client.m_QuicSession, Client.m_QuicResumeSessionId, Client.m_aQuicResumeToken.data(), Client.m_aQuicResumeToken.size()))
		return true;
	Client.m_aQuicResumeToken.fill(0);
	Client.m_QuicResumeArmed = false;
	return false;
}

void CServer::ExpireQuicResumes()
{
	const int64_t Now = time_get();
	for(int ClientId = 0; ClientId < MaxClients(); ++ClientId)
	{
		if(m_aClients[ClientId].m_QuicDetached && Now > m_aClients[ClientId].m_QuicResumeDeadline)
			DelClientCallback(ClientId, "QUIC resume timed out", this);
	}
}

void CServer::FormatModernTransportFragments(char *pQuicFragment, int QuicFragmentSize, char *pWebTransportFragment, int WebTransportFragmentSize) const
{
	pQuicFragment[0] = '\0';
	pWebTransportFragment[0] = '\0';
	if(m_QuicStarted)
	{
		if(m_QuicUseWebPki)
			str_copy(pQuicFragment, "webpki", QuicFragmentSize);
		else if(const CServerIdentityBinding *pIdentity = m_QuicTransport.ServerIdentity())
		{
			char aFingerprint[SHA256_MAXSTRSIZE];
			sha256_str(sha256(pIdentity->m_PublicKey.data(), pIdentity->m_PublicKey.size()), aFingerprint, sizeof(aFingerprint));
			str_format(pQuicFragment, QuicFragmentSize, "identity-sha256=%s", aFingerprint);
		}
	}
	if(m_WebTransportStarted && m_WebTransportUseCertificateHashes)
	{
		char aFingerprint[SHA256_MAXSTRSIZE];
		sha256_str(*m_QuicTransport.CertificateSha256(), aFingerprint, sizeof(aFingerprint));
		if(const SHA256_DIGEST *pNextFingerprint = m_QuicTransport.NextCertificateSha256())
		{
			char aNextFingerprint[SHA256_MAXSTRSIZE];
			sha256_str(*pNextFingerprint, aNextFingerprint, sizeof(aNextFingerprint));
			str_format(pWebTransportFragment, WebTransportFragmentSize, "cert-sha256=%s,%s", aFingerprint, aNextFingerprint);
		}
		else
			str_format(pWebTransportFragment, WebTransportFragmentSize, "cert-sha256=%s", aFingerprint);
	}
}

void CServer::PumpQuicNetwork()
{
	if(m_QuicStarted || m_WebTransportStarted)
	{
		bool Rotated;
		if(!m_QuicTransport.MaybeRotateManagedCertificate(&Rotated))
			log_error("quic", "could not rotate managed TLS certificate: %s", m_QuicTransport.ErrorString());
		else if(Rotated)
		{
			char aQuicFragment[160];
			char aWebTransportFragment[160];
			FormatModernTransportFragments(aQuicFragment, sizeof(aQuicFragment), aWebTransportFragment, sizeof(aWebTransportFragment));
			if(m_pRegister)
				m_pRegister->OnModernTrustChanged(aQuicFragment, aWebTransportFragment);
			ExpireServerInfo();
			log_info("quic", "rotated managed TLS certificate");
		}
	}
	NETADDR Address;
	unsigned char *pData;
	int DataSize;
	while((DataSize = m_QuicTransport.PollUdpSend(&Address, &pData)) > 0)
		m_NetServer.SendRaw(&Address, pData, DataSize);

	CQuicEvent Event;
	while(m_QuicTransport.Poll(Event))
	{
		if(Event.m_Type == EQuicEventType::MASTER_CHALLENGE)
		{
			CNetChunk Packet = {};
			Packet.m_ClientId = -1;
			Packet.m_Address = Event.m_Message.m_PeerAddress;
			Packet.m_Flags = NETSENDFLAG_CONNLESS;
			Packet.m_pData = Event.m_Message.m_pData;
			Packet.m_DataSize = Event.m_Message.m_DataSize;
			if(m_pRegister)
				m_pRegister->OnPacket(&Packet);
		}
		else if(Event.m_Type == EQuicEventType::CONNECTED)
		{
			char aReason[256];
			if(m_ServerBan.IsBanned(&Event.m_Message.m_PeerAddress, aReason, sizeof(aReason)))
			{
				m_QuicTransport.Close(Event.m_Message.m_Session, aReason);
				continue;
			}
			const bool HasResume = Event.m_Message.m_DataSize > 0;
			const int ResumeClientId = HasResume ? FindQuicResume(Event.m_Message) : -1;
			if(HasResume && ResumeClientId < 0)
			{
				if(Config()->m_SvTestingCommands)
					log_info("server", "rejected invalid or expired QUIC resume token");
				m_QuicTransport.Close(Event.m_Message.m_Session, "Invalid or expired resume token");
				continue;
			}
			int SameIp = 0;
			int ClientId = ResumeClientId;
			for(int i = 0; i < MaxClients(); i++)
			{
				if(i == ResumeClientId)
					continue;
				if(m_aClients[i].m_State == CClient::STATE_EMPTY && ClientId == -1)
					ClientId = i;
				else if(m_aClients[i].m_State != CClient::STATE_EMPTY && net_addr_comp_noport(ClientAddr(i), &Event.m_Message.m_PeerAddress) == 0)
					SameIp++;
			}
			if(ClientId == -1 || SameIp >= Config()->m_SvMaxClientsPerIp)
			{
				m_QuicTransport.Close(Event.m_Message.m_Session, ClientId == -1 ? "This server is full" : "Too many connections from this IP");
				continue;
			}
			auto &Client = m_aClients[ClientId];
			if(ResumeClientId >= 0)
			{
				if(Client.m_Sixup != Event.m_Sixup)
				{
					m_QuicTransport.Close(Event.m_Message.m_Session, "Game protocol changed during resume");
					continue;
				}
				const CQuicSessionId OldSession = Client.m_QuicSession;
				Client.m_QuicSession = Event.m_Message.m_Session;
				Client.m_WebTransport = Event.m_WebTransport;
				Client.m_QuicAddr = Event.m_Message.m_PeerAddress;
				m_NetServer.SetExternalSlot(ClientId, &Client.m_QuicAddr);
				net_addr_str(&Client.m_QuicAddr, Client.m_aQuicAddrString.data(), Client.m_aQuicAddrString.size(), true);
				net_addr_str(&Client.m_QuicAddr, Client.m_aQuicAddrStringNoPort.data(), Client.m_aQuicAddrStringNoPort.size(), false);
				Client.m_QuicDetached = false;
				Client.m_QuicResumeDeadline = 0;
				Client.m_LastAckedSnapshot = -1;
				Client.m_SnapRate = CClient::SNAPRATE_INIT;
				Client.m_Snapshots.PurgeAll();
				if(OldSession.IsValid() && !(OldSession == Client.m_QuicSession))
					m_QuicTransport.Close(OldSession, "Session resumed elsewhere");
				if(!IssueQuicResume(ClientId))
				{
					DropClient(ClientId, "Could not rotate QUIC resume token");
					continue;
				}
				log_info("server", "resumed QUIC session. ClientId=%d addr=<{%s}>", ClientId, ClientAddrString(ClientId, true));
				continue;
			}
			Client.m_Quic = true;
			Client.m_WebTransport = Event.m_WebTransport;
			Client.m_QuicSession = Event.m_Message.m_Session;
			Client.m_QuicAddr = Event.m_Message.m_PeerAddress;
			m_NetServer.SetExternalSlot(ClientId, &Client.m_QuicAddr);
			net_addr_str(&Client.m_QuicAddr, Client.m_aQuicAddrString.data(), Client.m_aQuicAddrString.size(), true);
			net_addr_str(&Client.m_QuicAddr, Client.m_aQuicAddrStringNoPort.data(), Client.m_aQuicAddrStringNoPort.size(), false);
			NewClientCallback(ClientId, this, Event.m_Sixup);
		}
		else if(Event.m_Type == EQuicEventType::MESSAGE)
		{
			const int ClientId = FindQuicClient(Event.m_Message.m_Session);
			if(ClientId < 0 || m_aClients[ClientId].m_State == CClient::STATE_REDIRECTED)
				continue;
			CNetChunk Packet = {};
			Packet.m_ClientId = ClientId;
			Packet.m_Address = m_aClients[ClientId].m_QuicAddr;
			Packet.m_Flags = Event.m_Message.m_Vital ? NET_CHUNKFLAG_VITAL : 0;
			Packet.m_pData = Event.m_Message.m_pData;
			Packet.m_DataSize = Event.m_Message.m_DataSize;
			const int GameFlags = Event.m_Message.m_Vital ? MSGFLAG_VITAL : 0;
			if(!Antibot()->OnEngineClientMessage(ClientId, Packet.m_pData, Packet.m_DataSize, GameFlags))
				ProcessClientPacket(&Packet);
		}
		else if(Event.m_Type == EQuicEventType::PEER_MIGRATED)
		{
			const int ClientId = FindQuicClient(Event.m_Message.m_Session);
			if(ClientId < 0)
				continue;
			char aReason[256];
			if(m_ServerBan.IsBanned(&Event.m_Message.m_PeerAddress, aReason, sizeof(aReason)))
			{
				m_QuicTransport.Close(Event.m_Message.m_Session, aReason);
				continue;
			}
			int SameIp = 0;
			for(int i = 0; i < MaxClients(); i++)
			{
				if(i != ClientId && m_aClients[i].m_State != CClient::STATE_EMPTY && net_addr_comp_noport(ClientAddr(i), &Event.m_Message.m_PeerAddress) == 0)
					SameIp++;
			}
			if(SameIp >= Config()->m_SvMaxClientsPerIp)
			{
				m_QuicTransport.Close(Event.m_Message.m_Session, "Too many connections from this IP");
				continue;
			}
			CClient &Client = m_aClients[ClientId];
			Client.m_QuicAddr = Event.m_Message.m_PeerAddress;
			m_NetServer.SetExternalSlot(ClientId, &Client.m_QuicAddr);
			net_addr_str(&Client.m_QuicAddr, Client.m_aQuicAddrString.data(), Client.m_aQuicAddrString.size(), true);
			net_addr_str(&Client.m_QuicAddr, Client.m_aQuicAddrStringNoPort.data(), Client.m_aQuicAddrStringNoPort.size(), false);
			log_info("server", "migrated QUIC path. ClientId=%d addr=<{%s}>", ClientId, ClientAddrString(ClientId, true));
		}
		else if(Event.m_Type == EQuicEventType::DISCONNECTED)
		{
			const int ClientId = FindQuicClient(Event.m_Message.m_Session);
			if(ClientId >= 0)
			{
				CClient &Client = m_aClients[ClientId];
				if(Client.m_QuicResumeArmed && Client.m_State >= CClient::STATE_READY &&
					str_comp(Event.m_pReason, "application disconnect") != 0)
				{
					Client.m_QuicDetached = true;
					Client.m_QuicResumeDeadline = time_get() + time_freq() * Config()->m_SvQuicResumeGraceMs / 1000;
					log_info("server", "holding QUIC slot for resume. ClientId=%d grace_ms=%d", ClientId, Config()->m_SvQuicResumeGraceMs);
				}
				else
				{
					char aReason[256];
					str_copy(aReason, Event.m_pReason ? Event.m_pReason : "QUIC connection closed");
					DelClientCallback(ClientId, aReason, this);
				}
			}
		}
	}
	ExpireQuicResumes();
}

void CServer::PumpNetwork()
{
	CNetChunk Packet;
	SECURITY_TOKEN ResponseToken;

	m_NetServer.Update();
	PumpQuicNetwork();

	// Coalesce the flushes triggered while handling this burst of incoming
	// packets (preinput broadcasts, timing/ping replies, ...) into one packet
	// per recipient, flushed once all packets have been handled below.
	m_NetServer.BeginFlushBatch();

	// Receive unconditionally, `net_udp_recv()` can hold packets that
	// `net_socket_read_wait()` does not see.
	{
		// process packets
		ResponseToken = NET_SECURITY_TOKEN_UNKNOWN;
		while(m_NetServer.Recv(&Packet, &ResponseToken))
		{
			if(Packet.m_ClientId == -1)
			{
				if(ResponseToken == NET_SECURITY_TOKEN_UNKNOWN && m_pRegister->OnPacket(&Packet))
					continue;

				{
					int ExtraToken = 0;
					int Type = -1;
					if(Packet.m_DataSize >= (int)sizeof(SERVERBROWSE_GETINFO) + 1 &&
						mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO)) == 0)
					{
						if(Packet.m_Flags & NETSENDFLAG_EXTENDED)
						{
							Type = SERVERINFO_EXTENDED;
							ExtraToken = (Packet.m_aExtraData[0] << 8) | Packet.m_aExtraData[1];
						}
						else
						{
							Type = SERVERINFO_VANILLA;
						}
					}
					else if(Packet.m_DataSize >= (int)sizeof(SERVERBROWSE_GETINFO_64_LEGACY) + 1 &&
						mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO_64_LEGACY, sizeof(SERVERBROWSE_GETINFO_64_LEGACY)) == 0)
					{
						Type = SERVERINFO_64_LEGACY;
					}
					if(Type == SERVERINFO_VANILLA && ResponseToken != NET_SECURITY_TOKEN_UNKNOWN && Config()->m_SvSixup)
					{
						CUnpacker Unpacker;
						Unpacker.Reset((unsigned char *)Packet.m_pData + sizeof(SERVERBROWSE_GETINFO), Packet.m_DataSize - sizeof(SERVERBROWSE_GETINFO));
						int SrvBrwsToken = Unpacker.GetInt();
						if(Unpacker.Error())
						{
							continue;
						}

						const std::optional<bool> SendClients = RateLimitServerInfoConnless();
						if(!SendClients.has_value())
						{
							continue;
						}

						CPacker Packer;
						Packer.Reset();
						Packer.AddRaw(SERVERBROWSE_INFO, sizeof(SERVERBROWSE_INFO));
						Packer.AddInt(SrvBrwsToken);
						GetServerInfoSixup(&Packer, SendClients.value());
						m_NetServer.SendPacketConnlessWithToken7(Packet.m_Address, Packer.Data(), Packer.Size(), ResponseToken, m_NetServer.GetToken(Packet.m_Address));
					}
					else if(Type != -1)
					{
						const std::optional<bool> SendClients = RateLimitServerInfoConnless();
						if(!SendClients.has_value())
						{
							continue;
						}

						int Token = ((unsigned char *)Packet.m_pData)[sizeof(SERVERBROWSE_GETINFO)];
						Token |= ExtraToken << 8;
						SendServerInfo(&Packet.m_Address, Token, Type, SendClients.value());
					}
				}
			}
			else
			{
				if(m_aClients[Packet.m_ClientId].m_State == CClient::STATE_REDIRECTED)
					continue;

				const int GameFlags = (Packet.m_Flags & NET_CHUNKFLAG_VITAL) ? MSGFLAG_VITAL : 0;
				if(Antibot()->OnEngineClientMessage(Packet.m_ClientId, Packet.m_pData, Packet.m_DataSize, GameFlags))
				{
					continue;
				}

				ProcessClientPacket(&Packet);
			}
		}
	}
	{
		unsigned char aBuffer[NET_MAX_CHUNK_SIZE];
		int ClientId;
		int DataSize;
		int Flags;
		while(Antibot()->OnEngineSimulateClientMessage(&ClientId, aBuffer, sizeof(aBuffer), &DataSize, &Flags))
		{
			CNetChunk SimulatedPacket = {};
			SimulatedPacket.m_ClientId = ClientId;
			SimulatedPacket.m_Flags = (Flags & MSGFLAG_VITAL) ? NET_CHUNKFLAG_VITAL : 0;
			SimulatedPacket.m_pData = aBuffer;
			SimulatedPacket.m_DataSize = DataSize;
			ProcessClientPacket(&SimulatedPacket);
		}
	}

	m_NetServer.EndFlushBatch();

	m_ServerBan.Update();
	m_Econ.Update();
}

void CServer::ChangeMap(const char *pMap)
{
	str_copy(Config()->m_SvMap, pMap);
	m_MapReload = str_comp(Config()->m_SvMap, GameServer()->Map()->FullName()) != 0;
}

void CServer::ReloadMap()
{
	m_SameMapReload = true;
}

int CServer::LoadMap(const char *pMapName)
{
	m_MapReload = false;
	m_SameMapReload = false;

	char aBuf[IO_MAX_PATH_LENGTH];
	str_format(aBuf, sizeof(aBuf), "maps/%s.map", pMapName);
	if(!str_valid_filename(fs_filename(aBuf)))
	{
		log_error("server", "The name '%s' cannot be used for maps because not all platforms support it", aBuf);
		return 0;
	}
	if(!GameServer()->OnMapChange(aBuf, sizeof(aBuf)))
	{
		return 0;
	}
	if(!GameServer()->Map()->Load(pMapName, Storage(), aBuf, IStorage::TYPE_ALL))
	{
		return 0;
	}

	// reinit snapshot ids
	m_IdPool.TimeoutIds();

	// get the crc of the map
	m_aCurrentMapSha256[MAP_TYPE_SIX] = GameServer()->Map()->Sha256();
	m_aCurrentMapCrc[MAP_TYPE_SIX] = GameServer()->Map()->Crc();
	char aBufMsg[256];
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(m_aCurrentMapSha256[MAP_TYPE_SIX], aSha256, sizeof(aSha256));
	str_format(aBufMsg, sizeof(aBufMsg), "%s sha256 is %s", aBuf, aSha256);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBufMsg);

	// load complete map into memory for download
	{
		free(m_apCurrentMapData[MAP_TYPE_SIX]);
		void *pData;
		Storage()->ReadFile(aBuf, IStorage::TYPE_ALL, &pData, &m_aCurrentMapSize[MAP_TYPE_SIX]);
		m_apCurrentMapData[MAP_TYPE_SIX] = (unsigned char *)pData;
	}

	if(Config()->m_SvMapsBaseUrl[0])
	{
		char aEscaped[256];
		str_format(aBuf, sizeof(aBuf), "%s_%s.map", pMapName, aSha256);
		EscapeUrl(aEscaped, aBuf);
		str_format(m_aMapDownloadUrl, sizeof(m_aMapDownloadUrl), "%s%s", Config()->m_SvMapsBaseUrl, aEscaped);
	}
	else
	{
		m_aMapDownloadUrl[0] = '\0';
	}

	// load sixup version of the map
	if(Config()->m_SvSixup)
	{
		str_format(aBuf, sizeof(aBuf), "maps7/%s.map", pMapName);
		void *pData;
		if(!Storage()->ReadFile(aBuf, IStorage::TYPE_ALL, &pData, &m_aCurrentMapSize[MAP_TYPE_SIXUP]))
		{
			Config()->m_SvSixup = 0;
			if(m_pRegister)
			{
				m_pRegister->OnConfigChange();
			}
			log_error("sixup", "couldn't load map %s", aBuf);
			log_info("sixup", "disabling 0.7 compatibility");
		}
		else
		{
			free(m_apCurrentMapData[MAP_TYPE_SIXUP]);
			m_apCurrentMapData[MAP_TYPE_SIXUP] = (unsigned char *)pData;

			m_aCurrentMapSha256[MAP_TYPE_SIXUP] = sha256(m_apCurrentMapData[MAP_TYPE_SIXUP], m_aCurrentMapSize[MAP_TYPE_SIXUP]);
			m_aCurrentMapCrc[MAP_TYPE_SIXUP] = crc32(0, m_apCurrentMapData[MAP_TYPE_SIXUP], m_aCurrentMapSize[MAP_TYPE_SIXUP]);
			sha256_str(m_aCurrentMapSha256[MAP_TYPE_SIXUP], aSha256, sizeof(aSha256));
			str_format(aBufMsg, sizeof(aBufMsg), "%s sha256 is %s", aBuf, aSha256);
			Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "sixup", aBufMsg);
		}
	}
	if(!Config()->m_SvSixup)
	{
		free(m_apCurrentMapData[MAP_TYPE_SIXUP]);
		m_apCurrentMapData[MAP_TYPE_SIXUP] = nullptr;
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_aPrevStates[i] = m_aClients[i].m_State;
	if(!UpdateQuicMaps())
	{
		log_error("server", "could not register the current map with QUIC");
		return 0;
	}

	return 1;
}

void CServer::UpdateDebugDummies(bool ForceDisconnect)
{
	if(m_PreviousDebugDummies == g_Config.m_DbgDummies && !ForceDisconnect)
		return;

	g_Config.m_DbgDummies = std::clamp(g_Config.m_DbgDummies, 0, MaxClients());
	for(int DummyIndex = 0; DummyIndex < std::max(m_PreviousDebugDummies, g_Config.m_DbgDummies); ++DummyIndex)
	{
		const bool AddDummy = !ForceDisconnect && DummyIndex < g_Config.m_DbgDummies;
		const int ClientId = MaxClients() - DummyIndex - 1;
		CClient &Client = m_aClients[ClientId];
		if(AddDummy && m_aClients[ClientId].m_State == CClient::STATE_EMPTY)
		{
			NewClientCallback(ClientId, this, false);
			Client.m_DebugDummy = true;

			// See https://en.wikipedia.org/wiki/Unique_local_address
			Client.m_DebugDummyAddr.type = NETTYPE_IPV6;
			Client.m_DebugDummyAddr.ip[0] = 0xfd;
			// Global ID (40 bits): random
			secure_random_fill(&Client.m_DebugDummyAddr.ip[1], 5);
			// Subnet ID (16 bits): constant
			Client.m_DebugDummyAddr.ip[6] = 0xc0;
			Client.m_DebugDummyAddr.ip[7] = 0xde;
			// Interface ID (64 bits): set to client ID
			Client.m_DebugDummyAddr.ip[8] = 0x00;
			Client.m_DebugDummyAddr.ip[9] = 0x00;
			Client.m_DebugDummyAddr.ip[10] = 0x00;
			Client.m_DebugDummyAddr.ip[11] = 0x00;
			uint_to_bytes_be(&Client.m_DebugDummyAddr.ip[12], ClientId);
			// Port: random like normal clients
			Client.m_DebugDummyAddr.port = secure_rand_below(65535 - 1024) + 1024;
			m_NetServer.SetExternalSlot(ClientId, &Client.m_DebugDummyAddr);
			net_addr_str(&Client.m_DebugDummyAddr, Client.m_aDebugDummyAddrString.data(), Client.m_aDebugDummyAddrString.size(), true);
			net_addr_str(&Client.m_DebugDummyAddr, Client.m_aDebugDummyAddrStringNoPort.data(), Client.m_aDebugDummyAddrStringNoPort.size(), false);

			GameServer()->OnClientConnected(ClientId, nullptr);
			Client.m_State = CClient::STATE_INGAME;
			Client.m_DDNetVersion = DDNET_VERSION_NUMBER;
			Client.m_GotDDNetVersionPacket = true;
			Client.m_DDNetVersionSettled = true;
			char aDummyName[MAX_NAME_LENGTH];
			str_format(aDummyName, sizeof(aDummyName), "Debug dummy %d", DummyIndex + 1);
			SetClientName(ClientId, aDummyName);
			GameServer()->OnClientEnter(ClientId);
		}
		else if(!AddDummy && Client.m_DebugDummy)
		{
			DelClientCallback(ClientId, "Dropping debug dummy", this);
		}

		if(AddDummy && Client.m_DebugDummy)
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (ClientId & 1) ? -1 : 1;
			Client.m_aInputs[0].m_GameTick = Tick() + 1;
			mem_copy(Client.m_aInputs[0].m_aData, &Input, std::min(sizeof(Input), sizeof(Client.m_aInputs[0].m_aData)));
			Client.m_LatestInput = Client.m_aInputs[0];
			Client.m_CurrentInput = 0;
		}
	}

	m_PreviousDebugDummies = ForceDisconnect ? 0 : g_Config.m_DbgDummies;
}

int CServer::Run()
{
	if(m_RunServer == UNINITIALIZED)
		m_RunServer = RUNNING;

	m_AuthManager.Init();

	if(Config()->m_Debug)
	{
		g_UuidManager.DebugDump();
	}

	{
		int Size = GameServer()->PersistentClientDataSize();
		for(auto &Client : m_aClients)
		{
			Client.m_HasPersistentData = false;
			Client.m_pPersistentData = malloc(Size);
		}
	}
	m_pPersistentData = malloc(GameServer()->PersistentDataSize());

	// load map
	if(!LoadMap(Config()->m_SvMap))
	{
		log_error("server", "failed to load map. mapname='%s'", Config()->m_SvMap);
		return -1;
	}

	if(Config()->m_SvSqliteFile[0] != '\0')
	{
		if(!fs_is_relative_path(Config()->m_SvSqliteFile))
		{
			log_error("server", "sv_sqlite_file must be a relative path. path='%s'", Config()->m_SvSqliteFile);
			return -1;
		}
		char aFullPath[IO_MAX_PATH_LENGTH];
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, Config()->m_SvSqliteFile, aFullPath, sizeof(aFullPath));

		if(Config()->m_SvUseSql)
		{
			DbPool()->RegisterSqliteDatabase(CDbConnectionPool::WRITE_BACKUP, aFullPath);
		}
		else
		{
			DbPool()->RegisterSqliteDatabase(CDbConnectionPool::READ, aFullPath);
			DbPool()->RegisterSqliteDatabase(CDbConnectionPool::WRITE, aFullPath);
		}
	}

	// start server
	NETADDR BindAddr;
	if(g_Config.m_Bindaddr[0] == '\0')
	{
		mem_zero(&BindAddr, sizeof(BindAddr));
	}
	else if(net_host_lookup(g_Config.m_Bindaddr, &BindAddr, NETTYPE_ALL) != 0)
	{
		log_error("server", "The configured bindaddr '%s' cannot be resolved", g_Config.m_Bindaddr);
		return -1;
	}
	BindAddr.type = Config()->m_SvIpv4Only ? (NETTYPE_IPV4 | NETTYPE_WEBSOCKET_IPV4) : NETTYPE_ALL;

	int Port = Config()->m_SvPort;
	BindAddr.port = Port != 0 ? Port : 8303;
	// These settings exist in every build, but only a build with the transport
	// compiled in can act on them. Saying so beats looking enabled and doing
	// nothing.
	bool QuicEnabled = Config()->m_SvQuic != 0;
	bool WebTransportEnabled = Config()->m_SvWebtransport != 0;
	if(QuicEnabled && !CQuicTransport::IsCompiled())
	{
		log_error("server", "sv_quic needs a build with QUIC=ON, continuing without QUIC");
		QuicEnabled = false;
	}
	if(WebTransportEnabled && !CQuicTransport::IsWebTransportServerCompiled())
	{
		log_error("server", "sv_webtransport needs a build with QUIC=ON, continuing without WebTransport");
		WebTransportEnabled = false;
	}
	const bool ModernTransportEnabled = QuicEnabled || WebTransportEnabled;
	const bool SharedTlsConfigured = Config()->m_SvTlsCert[0] != '\0' || Config()->m_SvTlsCertNext[0] != '\0' || Config()->m_SvTlsKey[0] != '\0';
	const char *pModernHostname = Config()->m_SvRegisterHostname[0] != '\0' ? Config()->m_SvRegisterHostname : Config()->m_SvWebtransportHostname;
	const char *pTlsCertificate = SharedTlsConfigured ? Config()->m_SvTlsCert : Config()->m_SvQuicCert;
	const char *pNextTlsCertificate = SharedTlsConfigured ? Config()->m_SvTlsCertNext : Config()->m_SvQuicCertNext;
	const char *pTlsPrivateKey = SharedTlsConfigured ? Config()->m_SvTlsKey : Config()->m_SvQuicKey;
	m_LegacyUdpStarted = Config()->m_SvLegacyUdp != 0;
	if(!m_LegacyUdpStarted && !ModernTransportEnabled)
	{
		log_error("server", "sv_legacy_udp is disabled, but neither QUIC nor WebTransport is enabled");
		return -1;
	}
	m_QuicStarted = false;
	m_WebTransportStarted = false;
	m_QuicUseWebPki = false;
	m_WebTransportUseCertificateHashes = false;
	m_aModernTransportHostname[0] = '\0';
	if(WebTransportEnabled && str_comp(Config()->m_SvWebtransportCertificateMode, "webpki") == 0 && pModernHostname[0] == '\0')
	{
		log_error("server", "sv_webtransport_hostname is required for WebTransport with Web PKI");
		return -1;
	}
	if(WebTransportEnabled && str_comp(Config()->m_SvWebtransportCertificateMode, "webpki") != 0 && str_comp(Config()->m_SvWebtransportCertificateMode, "hash") != 0)
	{
		log_error("server", "sv_webtransport_certificate_mode must be webpki or hash");
		return -1;
	}
	auto StartQuic = [&]() {
		if(SharedTlsConfigured && (pTlsCertificate[0] == '\0' || pTlsPrivateKey[0] == '\0'))
		{
			log_error("server", "sv_tls_cert and sv_tls_key must either both be set or both be empty");
			return false;
		}
		if((QuicEnabled || pTlsCertificate[0] == '\0') && Config()->m_SvQuicIdentityKey[0] == '\0')
		{
			log_error("server", "sv_quic_identity_key must not be empty when native QUIC or managed TLS is enabled");
			return false;
		}
		if(WebTransportEnabled && str_comp(Config()->m_SvWebtransportCertificateMode, "webpki") == 0 && (pTlsCertificate[0] == '\0' || pTlsPrivateKey[0] == '\0'))
		{
			log_error("server", "sv_tls_cert and sv_tls_key are required for WebTransport with Web PKI");
			return false;
		}
		NETADDR QuicBindAddr = {};
		if(g_Config.m_Bindaddr[0] == '\0')
			QuicBindAddr.type = Config()->m_SvIpv4Only ? NETTYPE_IPV4 : NETTYPE_IPV6;
		else if(net_host_lookup(g_Config.m_Bindaddr, &QuicBindAddr, Config()->m_SvIpv4Only ? NETTYPE_IPV4 : NETTYPE_ALL) != 0)
		{
			log_error("server", "The configured bindaddr '%s' cannot be resolved for QUIC", g_Config.m_Bindaddr);
			return false;
		}
		QuicBindAddr.port = BindAddr.port;
		char aModernAddress[NETADDR_MAXSTRSIZE];
		net_addr_str(&QuicBindAddr, aModernAddress, sizeof(aModernAddress), true);
		char aIdentityPath[IO_MAX_PATH_LENGTH] = {};
		if(Config()->m_SvQuicIdentityKey[0] != '\0')
			Storage()->GetCompletePath(IStorage::TYPE_SAVE_OR_ABSOLUTE, Config()->m_SvQuicIdentityKey, aIdentityPath, sizeof(aIdentityPath));
		if(!m_QuicTransport.StartServer(aModernAddress, QuicEnabled, WebTransportEnabled, pTlsCertificate, pNextTlsCertificate, pTlsPrivateKey, aIdentityPath))
		{
			log_error("server", "could not start modern transport: %s", m_QuicTransport.ErrorString());
			return false;
		}
		if(!UpdateQuicMaps())
		{
			log_error("server", "could not register the current map with QUIC");
			m_QuicTransport.Shutdown();
			return false;
		}
		if(QuicEnabled)
		{
			const CServerIdentityBinding *pIdentity = m_QuicTransport.ServerIdentity();
			if(!pIdentity)
				return false;
			const SHA256_DIGEST Fingerprint = sha256(pIdentity->m_PublicKey.data(), pIdentity->m_PublicKey.size());
			char aFingerprint[SHA256_MAXSTRSIZE];
			sha256_str(Fingerprint, aFingerprint, sizeof(aFingerprint));
			log_info("server", "native QUIC listening on %s identity-sha256=%s", aModernAddress, aFingerprint);
		}
		if(WebTransportEnabled)
			log_info("server", "WebTransport listening on %s path=/ddnet", aModernAddress);
		return true;
	};

	const NETFUNC_UDP_FILTER pfnQuicFilter = ModernTransportEnabled ?
							 +[](void *pUser, const NETADDR *pAddress, const void *pData, int DataSize) { return static_cast<CQuicTransport *>(pUser)->FeedUdp(pAddress, pData, DataSize); } :
							 nullptr;
	const NETFUNC_UDP_PEER pfnQuicPeer = ModernTransportEnabled ?
						     +[](void *pUser, const NETADDR *pAddress, bool Known) { static_cast<CQuicTransport *>(pUser)->SetLegacyPeer(pAddress, Known); } :
						     nullptr;
	for(; !m_NetServer.Open(BindAddr, &m_ServerBan, Config()->m_SvMaxClients, Config()->m_SvMaxClientsPerIp, pfnQuicFilter, pfnQuicPeer, &m_QuicTransport); BindAddr.port++)
	{
		if(Port != 0 || BindAddr.port >= 8310)
		{
			log_error("server", "couldn't open socket. port %d might already be in use", BindAddr.port);
			return -1;
		}
	}
	m_NetServer.SetLegacyConnections(m_LegacyUdpStarted);
	if(ModernTransportEnabled && !StartQuic())
		return -1;
	if(ModernTransportEnabled)
	{
		const int ModernTransportPort = Config()->m_SvRegisterPort > 0 ? Config()->m_SvRegisterPort : BindAddr.port;
		char aWebTransportUrl[256];
		if(pModernHostname[0] != '\0')
			str_copy(m_aModernTransportHostname, pModernHostname);
		if(Config()->m_SvRegisterHostname[0] != '\0' && (Config()->m_SvRegisterHostname[0] == '[' || !FormatWebTransportUrl(aWebTransportUrl, sizeof(aWebTransportUrl), Config()->m_SvRegisterHostname, ModernTransportPort)))
		{
			log_error("server", "sv_register_hostname must be a DNS name without a port");
			m_QuicTransport.Shutdown();
			return -1;
		}
		if(WebTransportEnabled && pModernHostname[0] != '\0' && !FormatWebTransportUrl(aWebTransportUrl, sizeof(aWebTransportUrl), pModernHostname, ModernTransportPort))
		{
			log_error("server", "sv_webtransport_hostname must be a DNS name or bracketed IP address without a port");
			m_QuicTransport.Shutdown();
			return -1;
		}
		m_QuicStarted = QuicEnabled;
		m_WebTransportStarted = WebTransportEnabled;
		m_WebTransportUseCertificateHashes = WebTransportEnabled && str_comp(Config()->m_SvWebtransportCertificateMode, "hash") == 0;
		m_QuicUseWebPki = QuicEnabled && m_aModernTransportHostname[0] != '\0' && SharedTlsConfigured;
	}
	log_info("server", "network transports: legacy-udp=%s quic=%s webtransport=%s", m_LegacyUdpStarted ? "enabled" : "disabled", m_QuicStarted ? "enabled" : "disabled", m_WebTransportStarted ? "enabled" : "disabled");

	if(Port == 0)
		log_info("server", "using port %d", BindAddr.port);

#if defined(CONF_UPNP)
	m_UPnP.Open(BindAddr);
#endif

	if(!m_pHttp->Init(std::chrono::seconds{2}))
	{
		log_error("server", "Failed to initialize the HTTP client.");
		return -1;
	}

	m_pEngine = Kernel()->RequestInterface<IEngine>();
	char aQuicFragment[160];
	char aWebTransportFragment[160];
	FormatModernTransportFragments(aQuicFragment, sizeof(aQuicFragment), aWebTransportFragment, sizeof(aWebTransportFragment));
	m_pRegister = CreateRegister(&g_Config, m_pConsole, m_pEngine, m_pHttp, g_Config.m_SvRegisterPort > 0 ? g_Config.m_SvRegisterPort : this->Port(), m_NetServer.GetGlobalToken(), m_LegacyUdpStarted, m_QuicStarted, m_WebTransportStarted, g_Config.m_SvRegisterHostname, aQuicFragment, aWebTransportFragment);

	m_NetServer.SetCallbacks(NewClientCallback, NewClientNoAuthCallback, ClientRejoinCallback, DelClientCallback, this);

	m_Econ.Init(Config(), Console(), &m_ServerBan);

	m_Fifo.Init(Console(), Config()->m_SvInputFifo, CFGFLAG_SERVER);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "server name is '%s'", Config()->m_SvName);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	Antibot()->Init();
	GameServer()->OnInit(nullptr);
	if(ErrorShutdown())
	{
		m_RunServer = STOPPING;
	}
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "version " GAME_RELEASE_VERSION " on " CONF_PLATFORM_STRING " " CONF_ARCH_STRING);
	if(GIT_SHORTREV_HASH)
	{
		str_format(aBuf, sizeof(aBuf), "git revision hash: %s", GIT_SHORTREV_HASH);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}

	ReadAnnouncementsFile();
	InitMaplist();

	// process pending commands
	m_pConsole->StoreCommands(false);
	m_pRegister->OnConfigChange();

	if(m_AuthManager.IsGenerated())
	{
		log_info("server", "+-------------------------+");
		log_info("server", "| rcon password: '%s' |", Config()->m_SvRconPassword);
		log_info("server", "+-------------------------+");
	}

	// start game
	{
		bool NonActive = false;

		m_GameStartTime = time_get();

		UpdateServerInfo(false);
		while(m_RunServer < STOPPING)
		{
			if(NonActive)
				PumpNetwork();

			set_new_tick();

			int64_t LastTime = time_get();
			int NewTicks = 0;

			// load new map
			if(m_MapReload || m_SameMapReload || m_CurrentGameTick >= MAX_TICK) // force reload to make sure the ticks stay within a valid range
			{
				const bool SameMapReload = m_SameMapReload;
				// load map
				if(LoadMap(Config()->m_SvMap))
				{
					// new map loaded

					// ask the game for the data it wants to persist past a map change
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(m_aClients[i].m_State == CClient::STATE_INGAME)
						{
							m_aClients[i].m_HasPersistentData = GameServer()->OnClientDataPersist(i, m_aClients[i].m_pPersistentData);
						}
					}

					UpdateDebugDummies(true);
					GameServer()->OnShutdown(m_pPersistentData);

					for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
					{
						if(m_aClients[ClientId].m_State <= CClient::STATE_AUTH)
							continue;

						if(SameMapReload)
							SendMapReload(ClientId);

						SendMap(ClientId);
						bool HasPersistentData = m_aClients[ClientId].m_HasPersistentData;
						m_aClients[ClientId].Reset();
						m_aClients[ClientId].m_HasPersistentData = HasPersistentData;
						m_aClients[ClientId].m_State = CClient::STATE_CONNECTING;
					}

					m_GameStartTime = time_get();
					m_CurrentGameTick = MIN_TICK;
					Kernel()->ReregisterInterface(GameServer());
					Console()->StoreCommands(true);
					GameServer()->OnInit(m_pPersistentData);
					Console()->StoreCommands(false);

					for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
					{
						CClient &Client = m_aClients[ClientId];
						if(Client.m_State < CClient::STATE_PREAUTH)
							continue;

						// When doing a map change, a new Teehistorian file is created. For players that are already
						// on the server, no PlayerJoin event is produced in Teehistorian from the network engine.
						// Record PlayerJoin events here to record the Sixup version and player join event.
						GameServer()->TeehistorianRecordPlayerJoin(ClientId, Client.m_Sixup);

						// Record the players auth state aswell if needed.
						// This was recorded in AuthInit in the past.
						if(IsRconAuthed(ClientId))
						{
							GameServer()->TeehistorianRecordAuthLogin(ClientId, GetAuthedState(ClientId), GetAuthName(ClientId));
						}
					}

					if(ErrorShutdown())
					{
						break;
					}
					ExpireServerInfo();
				}
				else
				{
					str_format(aBuf, sizeof(aBuf), "failed to load map. mapname='%s'", Config()->m_SvMap);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
					str_copy(Config()->m_SvMap, GameServer()->Map()->FullName());
				}
			}

			while(LastTime > TickStartTime(m_CurrentGameTick + 1))
			{
				const std::chrono::nanoseconds TickWorkStart = time_get_nanoseconds();
				GameServer()->OnPreTickTeehistorian();
				UpdateDebugDummies(false);

				for(int c = 0; c < MAX_CLIENTS; c++)
				{
					if(m_aClients[c].m_State != CClient::STATE_INGAME)
						continue;
					bool ClientHadInput = false;
					for(auto &Input : m_aClients[c].m_aInputs)
					{
						if(Input.m_GameTick == Tick() + 1)
						{
							GameServer()->OnClientPredictedEarlyInput(c, Input.m_aData);
							ClientHadInput = true;
							break;
						}
					}
					if(!ClientHadInput)
						GameServer()->OnClientPredictedEarlyInput(c, nullptr);
				}

				m_CurrentGameTick++;
				NewTicks++;

				// apply new input
				for(int c = 0; c < MAX_CLIENTS; c++)
				{
					if(m_aClients[c].m_State != CClient::STATE_INGAME)
						continue;
					bool ClientHadInput = false;
					for(auto &Input : m_aClients[c].m_aInputs)
					{
						if(Input.m_GameTick == Tick())
						{
							if(Config()->m_SvTestingCommands && Input.m_ReceiveTime > 0 && m_vBaselineInputQueueAgeMicroseconds.size() < 10000)
								m_vBaselineInputQueueAgeMicroseconds.push_back((time_get() - Input.m_ReceiveTime) * 1000000 / time_freq());
							GameServer()->OnClientPredictedInput(c, Input.m_aData);
							ClientHadInput = true;
							break;
						}
					}
					if(!ClientHadInput)
						GameServer()->OnClientPredictedInput(c, nullptr);
				}

				GameServer()->OnTick();
				if(Config()->m_SvTestingCommands && m_vBaselineTickWorkMicroseconds.size() < 10000)
					m_vBaselineTickWorkMicroseconds.push_back((time_get_nanoseconds() - TickWorkStart).count() / 1000);
				if(ErrorShutdown())
				{
					break;
				}
			}

			// snap game
			if(NewTicks)
			{
				DoSnapshot();

				const int CommandSendingClientId = Tick() % MAX_CLIENTS;
				UpdateClientRconCommands(CommandSendingClientId);
				UpdateClientMaplistEntries(CommandSendingClientId);

				m_Fifo.Update();

#if defined(CONF_PLATFORM_ANDROID)
				std::vector<std::string> vAndroidCommandQueue = FetchAndroidServerCommandQueue();
				for(const std::string &Command : vAndroidCommandQueue)
				{
					Console()->ExecuteLineFlag(Command.c_str(), CFGFLAG_SERVER, IConsole::CLIENT_ID_UNSPECIFIED);
				}
#endif

				// master server stuff
				m_pRegister->Update();

				if(m_ServerInfoNeedsUpdate)
				{
					UpdateServerInfo(m_ServerInfoNeedsResend);
				}

				Antibot()->OnEngineTick();

				// handle dnsbl
				if(Config()->m_SvDnsbl)
				{
					for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
					{
						if(m_aClients[ClientId].m_State == CClient::STATE_EMPTY)
							continue;

						if(m_aClients[ClientId].m_DnsblState == EDnsblState::NONE)
						{
							// initiate dnsbl lookup
							InitDnsbl(ClientId);
						}
						else if(m_aClients[ClientId].m_DnsblState == EDnsblState::PENDING &&
							m_aClients[ClientId].m_pDnsblLookup->State() == IJob::STATE_DONE)
						{
							if(m_aClients[ClientId].m_pDnsblLookup->Result() != 0)
							{
								// entry not found -> whitelisted
								m_aClients[ClientId].m_DnsblState = EDnsblState::WHITELISTED;

								str_format(aBuf, sizeof(aBuf), "ClientId=%d addr=<{%s}> secure=%s whitelisted", ClientId, ClientAddrString(ClientId, true), m_NetServer.HasSecurityToken(ClientId) ? "yes" : "no");
								Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "dnsbl", aBuf);
							}
							else
							{
								// entry found -> blacklisted
								m_aClients[ClientId].m_DnsblState = EDnsblState::BLACKLISTED;

								str_format(aBuf, sizeof(aBuf), "ClientId=%d addr=<{%s}> secure=%s blacklisted", ClientId, ClientAddrString(ClientId, true), m_NetServer.HasSecurityToken(ClientId) ? "yes" : "no");
								Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "dnsbl", aBuf);

								if(Config()->m_SvDnsblBan)
								{
									m_NetServer.NetBan()->BanAddr(ClientAddr(ClientId), 60, Config()->m_SvDnsblBanReason, true);
								}
							}
						}
					}
				}
				for(int i = 0; i < MAX_CLIENTS; ++i)
				{
					if(m_aClients[i].m_State == CClient::STATE_REDIRECTED)
					{
						if(time_get() > m_aClients[i].m_RedirectDropTime)
						{
							DropClient(i, "redirected");
						}
					}
				}
			}

			if(!NonActive)
				PumpNetwork();

			for(int ClientId = 0; ClientId < MaxClients(); ++ClientId)
			{
				if(!m_aClients[ClientId].m_QuicDropPending)
					continue;
				m_aClients[ClientId].m_QuicDropPending = false;
				DropClient(ClientId, "QUIC reliable queue full");
			}

			NonActive = true;
			for(const auto &Client : m_aClients)
			{
				if(Client.m_State != CClient::STATE_EMPTY)
				{
					NonActive = false;
					break;
				}
			}

			if(NonActive)
			{
				if(Config()->m_SvReloadWhenEmpty == 1)
				{
					m_MapReload = true;
					Config()->m_SvReloadWhenEmpty = 0;
				}
				else if(Config()->m_SvReloadWhenEmpty == 2 && !m_ReloadedWhenEmpty)
				{
					m_MapReload = true;
					m_ReloadedWhenEmpty = true;
				}
			}
			else
			{
				m_ReloadedWhenEmpty = false;
			}

			// wait for incoming data
			if(NonActive && Config()->m_SvShutdownWhenEmpty)
			{
				m_RunServer = STOPPING;
			}
			else if(NonActive &&
				!m_aDemoRecorder[RECORDER_MANUAL].IsRecording() &&
				!m_aDemoRecorder[RECORDER_AUTO].IsRecording())
			{
				if(m_NetServer.Socket())
				{
					auto Wait = std::chrono::duration_cast<std::chrono::microseconds>(1s);
					const int64_t QuicWait = m_QuicTransport.NextTimeoutMicroseconds();
					if(QuicWait >= 0)
						Wait = std::min(Wait, std::chrono::microseconds(QuicWait));
					if(Wait > 0us)
						net_socket_read_wait(m_NetServer.Socket(), Wait);
				}
				else
				{
					// WebTransport-only servers have no UDP socket to sleep on
					std::this_thread::sleep_for(1ms);
				}
			}
			else
			{
				set_new_tick();
				LastTime = time_get();
				const auto MicrosecondsToWait = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::nanoseconds(TickStartTime(m_CurrentGameTick + 1) - LastTime)) + 1us;
				if(m_NetServer.Socket())
				{
					auto Wait = MicrosecondsToWait;
					const int64_t QuicWait = m_QuicTransport.NextTimeoutMicroseconds();
					if(QuicWait >= 0)
						Wait = std::min(Wait, std::chrono::microseconds(QuicWait));
					if(Wait > 0us)
						net_socket_read_wait(m_NetServer.Socket(), Wait);
				}
				else if(MicrosecondsToWait > 0us)
				{
					// WebTransport-only servers have no UDP socket to sleep on
					std::this_thread::sleep_for(std::min(MicrosecondsToWait, 1000us));
				}
			}
			if(IsInterrupted())
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "interrupted");
				break;
			}
		}
	}
	const char *pDisconnectReason = "Server shutdown";
	if(m_aShutdownReason[0])
		pDisconnectReason = m_aShutdownReason;

	if(ErrorShutdown())
	{
		log_info("server", "shutdown from game server (%s)", m_aErrorShutdownReason);
		pDisconnectReason = m_aErrorShutdownReason;
	}
	// disconnect all clients on shutdown
	int PendingQuicClients = 0;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			PendingQuicClients += m_aClients[i].m_Quic;
			DropClient(i, pDisconnectReason);
		}
	}
	if(PendingQuicClients > 0)
	{
		auto Deadline = std::chrono::steady_clock::now() + 1s;
		while(std::chrono::steady_clock::now() < Deadline)
		{
			m_NetServer.Update();
			CNetChunk Packet;
			SECURITY_TOKEN ResponseToken;
			while(m_NetServer.Recv(&Packet, &ResponseToken))
			{
			}
			NETADDR Address;
			unsigned char *pData;
			int DataSize;
			while((DataSize = m_QuicTransport.PollUdpSend(&Address, &pData)) > 0)
				m_NetServer.SendRaw(&Address, pData, DataSize);
			CQuicEvent Event;
			while(m_QuicTransport.Poll(Event))
			{
				if(Event.m_Type == EQuicEventType::DISCONNECTED && PendingQuicClients > 0)
				{
					PendingQuicClients--;
					if(PendingQuicClients == 0)
						Deadline = std::chrono::steady_clock::now() + 100ms;
				}
			}
			std::this_thread::sleep_for(1ms);
		}
	}
	if(m_QuicTransport.IsRunning())
	{
		const auto &Metrics = m_QuicTransport.Metrics();
		log_info("quic", "connections=%llu/%llu sent=%llu/%llu recv=%llu/%llu bytes=%llu/%llu queue_drop=%llu/%llu queue_high_water=%llu raw_drop=%llu path_change=%llu",
			static_cast<unsigned long long>(Metrics.m_Connections), static_cast<unsigned long long>(Metrics.m_Disconnections),
			static_cast<unsigned long long>(Metrics.m_ReliableSent), static_cast<unsigned long long>(Metrics.m_DatagramsSent),
			static_cast<unsigned long long>(Metrics.m_ReliableReceived), static_cast<unsigned long long>(Metrics.m_DatagramsReceived),
			static_cast<unsigned long long>(Metrics.m_BytesSent), static_cast<unsigned long long>(Metrics.m_BytesReceived),
			static_cast<unsigned long long>(Metrics.m_ReliableQueueFull), static_cast<unsigned long long>(Metrics.m_DatagramsDropped),
			static_cast<unsigned long long>(Metrics.m_CommandQueueHighWater),
			static_cast<unsigned long long>(m_QuicTransport.RawDropCount()),
			static_cast<unsigned long long>(Metrics.m_PathChanges));
	}
	m_QuicTransport.Shutdown();

	m_pRegister->OnShutdown();
	m_Econ.Shutdown();
	m_Fifo.Shutdown();
	m_pHttp->Shutdown();
	Engine()->ShutdownJobs();

	GameServer()->OnShutdown(nullptr);
	GameServer()->Map()->Unload();
	DbPool()->OnShutdown();

#if defined(CONF_UPNP)
	m_UPnP.Shutdown();
#endif
	m_NetServer.Close();

	return ErrorShutdown();
}

void CServer::ConKick(IConsole::IResult *pResult, void *pUser)
{
	if(pResult->NumArguments() > 1)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Kicked (%s)", pResult->GetString(1));
		((CServer *)pUser)->Kick(pResult->GetVictim(), aBuf);
	}
	else
	{
		((CServer *)pUser)->Kick(pResult->GetVictim(), "Kicked by console");
	}
}

void CServer::ConStatus(IConsole::IResult *pResult, void *pUser)
{
	char aBuf[1024];
	CServer *pThis = static_cast<CServer *>(pUser);
	const char *pName = pResult->NumArguments() == 1 ? pResult->GetString(0) : "";
	str_format(aBuf, sizeof(aBuf), "transports legacy-udp=%s quic=%s webtransport=%s", pThis->m_LegacyUdpStarted ? "enabled" : "disabled", pThis->m_QuicStarted ? "enabled" : "disabled", pThis->m_WebTransportStarted ? "enabled" : "disabled");
	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(pThis->m_aClients[i].m_State == CClient::STATE_EMPTY)
			continue;

		if(!str_utf8_find_nocase(pThis->m_aClients[i].m_aName, pName))
			continue;

		if(pThis->m_aClients[i].m_State == CClient::STATE_INGAME)
		{
			char aDnsblStr[64];
			aDnsblStr[0] = '\0';
			if(pThis->Config()->m_SvDnsbl)
			{
				str_format(aDnsblStr, sizeof(aDnsblStr), " dnsbl=%s", DnsblStateStr(pThis->m_aClients[i].m_DnsblState));
			}

			char aAuthStr[128];
			aAuthStr[0] = '\0';
			if(pThis->m_aClients[i].m_AuthKey >= 0)
			{
				const char *pAuthStr = "";
				const int AuthState = pThis->GetAuthedState(i);

				if(AuthState == AUTHED_ADMIN)
				{
					pAuthStr = "(Admin)";
				}
				else if(AuthState == AUTHED_MOD)
				{
					pAuthStr = "(Mod)";
				}
				else if(AuthState == AUTHED_HELPER)
				{
					pAuthStr = "(Helper)";
				}

				str_format(aAuthStr, sizeof(aAuthStr), " key='%s' %s", pThis->m_AuthManager.KeyIdent(pThis->m_aClients[i].m_AuthKey), pAuthStr);
			}

			const char *pClientPrefix = "";
			if(pThis->m_aClients[i].m_Sixup)
			{
				pClientPrefix = "0.7:";
			}
			str_format(aBuf, sizeof(aBuf), "id=%d addr=<{%s}> name='%s' transport=%s client=%s%d secure=%s flags=%d%s%s",
				i, pThis->ClientAddrString(i, true), pThis->m_aClients[i].m_aName, pThis->ClientTransportName(i), pClientPrefix, pThis->m_aClients[i].m_DDNetVersion,
				pThis->m_NetServer.HasSecurityToken(i) ? "yes" : "no", pThis->m_aClients[i].m_Flags, aDnsblStr, aAuthStr);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "id=%d addr=<{%s}> transport=%s connecting", i, pThis->ClientAddrString(i, true), pThis->ClientTransportName(i));
		}
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

void CServer::ConBaselineStats(IConsole::IResult *, void *pUser)
{
	CServer *pSelf = static_cast<CServer *>(pUser);
	auto Percentile = [](const std::vector<int64_t> &vSamples, int Percent) {
		if(vSamples.empty())
			return int64_t{-1};
		std::vector<int64_t> vSorted = vSamples;
		std::sort(vSorted.begin(), vSorted.end());
		return vSorted[(vSorted.size() * Percent - 1) / 100];
	};
	NETSTATS NetStats = {};
	net_stats(&NetStats);
	int RttMilliseconds = -1;
	for(const CClient &Client : pSelf->m_aClients)
	{
		if(Client.m_State == CClient::STATE_INGAME)
		{
			RttMilliseconds = Client.m_Latency;
			break;
		}
	}
	char aBuf[1024];
	str_format(aBuf, sizeof(aBuf),
		"rtt_ms=%d snapshot_ack_age_us=%" PRId64 "/%" PRId64 "/%" PRId64 " input_queue_age_us=%" PRId64 "/%" PRId64 "/%" PRId64 " tick_work_us=%" PRId64 "/%" PRId64 "/%" PRId64 " samples=%" PRIzu "/%" PRIzu "/%" PRIzu " packets=%" PRIu64 "/%" PRIu64 " bytes=%" PRIu64 "/%" PRIu64,
		RttMilliseconds,
		Percentile(pSelf->m_vBaselineSnapshotAckAgeMicroseconds, 50), Percentile(pSelf->m_vBaselineSnapshotAckAgeMicroseconds, 95), Percentile(pSelf->m_vBaselineSnapshotAckAgeMicroseconds, 99),
		Percentile(pSelf->m_vBaselineInputQueueAgeMicroseconds, 50), Percentile(pSelf->m_vBaselineInputQueueAgeMicroseconds, 95), Percentile(pSelf->m_vBaselineInputQueueAgeMicroseconds, 99),
		Percentile(pSelf->m_vBaselineTickWorkMicroseconds, 50), Percentile(pSelf->m_vBaselineTickWorkMicroseconds, 95), Percentile(pSelf->m_vBaselineTickWorkMicroseconds, 99),
		pSelf->m_vBaselineSnapshotAckAgeMicroseconds.size(), pSelf->m_vBaselineInputQueueAgeMicroseconds.size(), pSelf->m_vBaselineTickWorkMicroseconds.size(),
		NetStats.sent_packets, NetStats.recv_packets, NetStats.sent_bytes, NetStats.recv_bytes);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "baseline", aBuf);
}

static int GetAuthLevel(const char *pLevel)
{
	int Level = -1;
	if(!str_comp_nocase(pLevel, "admin"))
		Level = AUTHED_ADMIN;
	else if(str_startswith(pLevel, "mod"))
		Level = AUTHED_MOD;
	else if(!str_comp_nocase(pLevel, "helper"))
		Level = AUTHED_HELPER;

	return Level;
}

bool CServer::CanClientUseCommandCallback(int ClientId, const IConsole::ICommandInfo *pCommand, void *pUser)
{
	return ((CServer *)pUser)->CanClientUseCommand(ClientId, pCommand);
}

bool CServer::CanClientUseCommand(int ClientId, const IConsole::ICommandInfo *pCommand) const
{
	if(pCommand->Flags() & CFGFLAG_CHAT)
		return true;
	if(pCommand->Flags() & CMDFLAG_PRACTICE)
		return true;
	if(!IsRconAuthed(ClientId))
		return false;
	return pCommand->GetAccessLevel() >= ConsoleAccessLevel(ClientId);
}

void CServer::AuthRemoveKey(int KeySlot)
{
	m_AuthManager.RemoveKey(KeySlot);
	LogoutKey(KeySlot, "key removal");

	// Update indices.
	for(auto &Client : m_aClients)
	{
		if(Client.m_AuthKey == KeySlot)
		{
			Client.m_AuthKey = -1;
		}
		else if(Client.m_AuthKey > KeySlot)
		{
			--Client.m_AuthKey;
		}
	}
}

void CServer::ConAuthAdd(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);

	if(!pManager->IsValidIdent(pIdent))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident is invalid");
		return;
	}

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}
	// back compat to change "mod", "modder" and so on as parameters to "moderator"
	pLevel = CAuthManager::AuthLevelToRoleName(Level);

	bool NeedUpdate = !pManager->NumNonDefaultKeys();
	if(pManager->AddKey(pIdent, pPw, pLevel) < 0)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident already exists");
	}
	else
	{
		if(NeedUpdate)
			pThis->SendRconType(-1, true);
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key added");
	}
}

void CServer::ConAuthAddHashed(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);
	const char *pSalt = pResult->GetString(3);

	if(!pManager->IsValidIdent(pIdent))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident is invalid");
		return;
	}

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}
	// back compat to change "mod", "modder" and so on as parameters to "moderator"
	pLevel = CAuthManager::AuthLevelToRoleName(Level);

	MD5_DIGEST Hash;
	unsigned char aSalt[SALT_BYTES];

	if(md5_from_str(&Hash, pPw))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed password hash");
		return;
	}
	if(str_hex_decode(aSalt, sizeof(aSalt), pSalt))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed salt hash");
		return;
	}

	bool NeedUpdate = !pManager->NumNonDefaultKeys();

	if(pManager->AddKeyHash(pIdent, Hash, aSalt, pLevel) < 0)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident already exists");
	}
	else
	{
		if(NeedUpdate)
			pThis->SendRconType(-1, true);
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key added");
	}
}

void CServer::ConAuthUpdate(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);

	int KeySlot = pManager->FindKey(pIdent);
	if(KeySlot == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident couldn't be found");
		return;
	}

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}
	// back compat to change "mod", "modder" and so on as parameters to "moderator"
	pLevel = CAuthManager::AuthLevelToRoleName(Level);

	pManager->UpdateKey(KeySlot, pPw, pLevel);
	pThis->LogoutKey(KeySlot, "key update");

	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key updated");
}

void CServer::ConAuthUpdateHashed(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);
	const char *pSalt = pResult->GetString(3);

	int KeySlot = pManager->FindKey(pIdent);
	if(KeySlot == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident couldn't be found");
		return;
	}

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}
	// back compat to change "mod", "modder" and so on as parameters to "moderator"
	pLevel = CAuthManager::AuthLevelToRoleName(Level);

	MD5_DIGEST Hash;
	unsigned char aSalt[SALT_BYTES];

	if(md5_from_str(&Hash, pPw))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed password hash");
		return;
	}
	if(str_hex_decode(aSalt, sizeof(aSalt), pSalt))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed salt hash");
		return;
	}

	pManager->UpdateKeyHash(KeySlot, Hash, aSalt, pLevel);
	pThis->LogoutKey(KeySlot, "key update");

	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key updated");
}

void CServer::ConAuthRemove(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);

	int KeySlot = pManager->FindKey(pIdent);
	if(KeySlot == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident couldn't be found");
		return;
	}

	pThis->AuthRemoveKey(KeySlot);

	if(!pManager->NumNonDefaultKeys())
		pThis->SendRconType(-1, false);

	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key removed, all users logged out");
}

static void ListKeysCallback(const char *pIdent, const char *pRoleName, void *pUser)
{
	log_info("auth", "%s %s", pIdent, pRoleName);
}

void CServer::ConAuthList(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	pManager->ListKeys(ListKeysCallback, pThis);
}

void CServer::ConShutdown(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = static_cast<CServer *>(pUser);
	pThis->m_RunServer = STOPPING;
	const char *pReason = pResult->GetString(0);
	if(pReason[0])
	{
		str_copy(pThis->m_aShutdownReason, pReason);
	}
}

void CServer::DemoRecorder_HandleAutoStart()
{
	if(Config()->m_SvAutoDemoRecord)
	{
		m_aDemoRecorder[RECORDER_AUTO].Stop(IDemoRecorder::EStopMode::KEEP_FILE);

		char aTimestamp[20];
		str_timestamp(aTimestamp, sizeof(aTimestamp));
		char aFilename[IO_MAX_PATH_LENGTH];
		str_format(aFilename, sizeof(aFilename), "demos/auto/server/%s_%s.demo", GameServer()->Map()->BaseName(), aTimestamp);
		m_aDemoRecorder[RECORDER_AUTO].Start(
			Storage(),
			m_pConsole,
			aFilename,
			GameServer()->NetVersion(),
			GameServer()->Map()->BaseName(),
			m_aCurrentMapSha256[MAP_TYPE_SIX],
			m_aCurrentMapCrc[MAP_TYPE_SIX],
			"server",
			m_aCurrentMapSize[MAP_TYPE_SIX],
			m_apCurrentMapData[MAP_TYPE_SIX],
			nullptr,
			nullptr,
			nullptr);

		if(Config()->m_SvAutoDemoMax)
		{
			// clean up auto recorded demos
			CFileCollection AutoDemos;
			AutoDemos.Init(Storage(), "demos/auto/server", "", ".demo", Config()->m_SvAutoDemoMax);
		}
	}
}

void CServer::SaveDemo(int ClientId, float Time)
{
	if(IsRecording(ClientId))
	{
		char aNewFilename[IO_MAX_PATH_LENGTH];
		str_format(aNewFilename, sizeof(aNewFilename), "demos/%s_%s_%05.2f.demo", GameServer()->Map()->BaseName(), m_aClients[ClientId].m_aName, Time);
		m_aDemoRecorder[ClientId].Stop(IDemoRecorder::EStopMode::KEEP_FILE, aNewFilename);
	}
}

void CServer::StartRecord(int ClientId)
{
	if(Config()->m_SvPlayerDemoRecord)
	{
		char aFilename[IO_MAX_PATH_LENGTH];
		str_format(aFilename, sizeof(aFilename), "demos/%s_%d_%d_tmp.demo", GameServer()->Map()->BaseName(), m_NetServer.Address().port, ClientId);
		m_aDemoRecorder[ClientId].Start(
			Storage(),
			Console(),
			aFilename,
			GameServer()->NetVersion(),
			GameServer()->Map()->BaseName(),
			m_aCurrentMapSha256[MAP_TYPE_SIX],
			m_aCurrentMapCrc[MAP_TYPE_SIX],
			"server",
			m_aCurrentMapSize[MAP_TYPE_SIX],
			m_apCurrentMapData[MAP_TYPE_SIX],
			nullptr,
			nullptr,
			nullptr);
	}
}

void CServer::StopRecord(int ClientId)
{
	if(IsRecording(ClientId))
	{
		m_aDemoRecorder[ClientId].Stop(IDemoRecorder::EStopMode::REMOVE_FILE);
	}
}

bool CServer::IsRecording(int ClientId)
{
	return m_aDemoRecorder[ClientId].IsRecording();
}

void CServer::StopDemos()
{
	for(int i = 0; i < NUM_RECORDERS; i++)
	{
		if(!m_aDemoRecorder[i].IsRecording())
			continue;

		m_aDemoRecorder[i].Stop(i < MAX_CLIENTS ? IDemoRecorder::EStopMode::REMOVE_FILE : IDemoRecorder::EStopMode::KEEP_FILE);
	}
}

void CServer::ConRecord(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->IsRecording(RECORDER_MANUAL))
	{
		pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "demo_recorder", "Demo recorder already recording");
		return;
	}

	char aFilename[IO_MAX_PATH_LENGTH];
	if(pResult->NumArguments())
	{
		str_format(aFilename, sizeof(aFilename), "demos/%s.demo", pResult->GetString(0));
	}
	else
	{
		char aTimestamp[20];
		str_timestamp(aTimestamp, sizeof(aTimestamp));
		str_format(aFilename, sizeof(aFilename), "demos/demo_%s.demo", aTimestamp);
	}
	pServer->m_aDemoRecorder[RECORDER_MANUAL].Start(
		pServer->Storage(),
		pServer->Console(),
		aFilename,
		pServer->GameServer()->NetVersion(),
		pServer->GameServer()->Map()->BaseName(),
		pServer->m_aCurrentMapSha256[MAP_TYPE_SIX],
		pServer->m_aCurrentMapCrc[MAP_TYPE_SIX],
		"server",
		pServer->m_aCurrentMapSize[MAP_TYPE_SIX],
		pServer->m_apCurrentMapData[MAP_TYPE_SIX],
		nullptr,
		nullptr,
		nullptr);
}

void CServer::ConStopRecord(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_aDemoRecorder[RECORDER_MANUAL].Stop(IDemoRecorder::EStopMode::KEEP_FILE);
}

void CServer::ConMapReload(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->ReloadMap();
}

void CServer::ConLogout(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientId >= 0 && pServer->m_RconClientId < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientId].m_State != CServer::CClient::STATE_EMPTY)
	{
		pServer->LogoutClient(pServer->m_RconClientId, "");
	}
}

void CServer::ConShowIps(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientId >= 0 && pServer->m_RconClientId < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientId].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(pResult->NumArguments())
		{
			pServer->m_aClients[pServer->m_RconClientId].m_ShowIps = pResult->GetInteger(0);
		}
		else
		{
			char aStr[9];
			str_format(aStr, sizeof(aStr), "Value: %d", pServer->m_aClients[pServer->m_RconClientId].m_ShowIps);
			pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aStr);
		}
	}
}

void CServer::ConHideAuthStatus(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientId >= 0 && pServer->m_RconClientId < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientId].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(pResult->NumArguments())
		{
			pServer->m_aClients[pServer->m_RconClientId].m_AuthHidden = pResult->GetInteger(0);
		}
		else
		{
			char aStr[9];
			str_format(aStr, sizeof(aStr), "Value: %d", pServer->m_aClients[pServer->m_RconClientId].m_AuthHidden);
			pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aStr);
		}
	}
}

void CServer::ConForceHighBandwidthOnSpectate(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientId >= 0 && pServer->m_RconClientId < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientId].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(pResult->NumArguments())
		{
			pServer->m_aClients[pServer->m_RconClientId].m_ForceHighBandwidthOnSpectate = pResult->GetInteger(0);
		}
		else
		{
			char aStr[9];
			str_format(aStr, sizeof(aStr), "Value: %d", pServer->m_aClients[pServer->m_RconClientId].m_ForceHighBandwidthOnSpectate);
			pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aStr);
		}
	}
}

void CServer::ConAddSqlServer(IConsole::IResult *pResult, void *pUserData)
{
	CServer *pSelf = (CServer *)pUserData;

	if(!MysqlAvailable())
	{
		log_error("server", "can't add MySQL server: compiled without MySQL support");
		return;
	}

	if(!pSelf->Config()->m_SvUseSql)
		return;

	if(pResult->NumArguments() < 7 || pResult->NumArguments() > 9)
	{
		log_error("server", "7 to 9 arguments are required");
		return;
	}

	CMysqlConfig Config;
	bool Write;
	if(str_comp_nocase(pResult->GetString(0), "r") == 0)
	{
		Write = false;
	}
	else if(str_comp_nocase(pResult->GetString(0), "w") == 0)
	{
		Write = true;
	}
	else
	{
		log_error("server", "choose either 'r' for SqlReadServer or 'w' for SqlWriteServer");
		return;
	}

	str_copy(Config.m_aDatabase, pResult->GetString(1));
	str_copy(Config.m_aPrefix, pResult->GetString(2));
	str_copy(Config.m_aUser, pResult->GetString(3));
	str_copy(Config.m_aPass, pResult->GetString(4));
	str_copy(Config.m_aIp, pResult->GetString(5));
	Config.m_aBindaddr[0] = '\0';
	Config.m_Port = pResult->GetInteger(6);
	Config.m_Setup = pResult->NumArguments() >= 8 ? pResult->GetInteger(7) : true;
	Config.m_UseSsl = pResult->NumArguments() >= 9 ? pResult->GetInteger(8) != 0 : false;
	str_copy(Config.m_aSslCa, g_Config.m_SvSqlSslCa);
	str_copy(Config.m_aSslCert, g_Config.m_SvSqlSslCert);
	str_copy(Config.m_aSslKey, g_Config.m_SvSqlSslKey);

	log_info("server",
		"Adding new Sql%sServer: DB: '%s' Prefix: '%s' User: '%s' IP: <{%s}> Port: %d",
		Write ? "Write" : "Read",
		Config.m_aDatabase, Config.m_aPrefix, Config.m_aUser, Config.m_aIp, Config.m_Port);
	pSelf->DbPool()->RegisterMysqlDatabase(Write ? CDbConnectionPool::WRITE : CDbConnectionPool::READ, &Config);
}

void CServer::ConDumpSqlServers(IConsole::IResult *pResult, void *pUserData)
{
	CServer *pSelf = (CServer *)pUserData;

	if(str_comp_nocase(pResult->GetString(0), "w") == 0)
	{
		pSelf->DbPool()->Print(CDbConnectionPool::WRITE);
		pSelf->DbPool()->Print(CDbConnectionPool::WRITE_BACKUP);
	}
	else if(str_comp_nocase(pResult->GetString(0), "r") == 0)
	{
		pSelf->DbPool()->Print(CDbConnectionPool::READ);
	}
	else
	{
		log_error("server", "choose either 'r' for SqlReadServer or 'w' for SqlWriteServer");
		return;
	}
}

void CServer::ConReloadAnnouncement(IConsole::IResult *pResult, void *pUserData)
{
	CServer *pThis = static_cast<CServer *>(pUserData);
	pThis->ReadAnnouncementsFile();
}

#if defined(CONF_WEBSOCKETS)
void CServer::ConReloadWebsocketCert(IConsole::IResult *pResult, void *pUserData)
{
	websocket_reload_certs();
}
#endif

void CServer::ConReloadMaplist(IConsole::IResult *pResult, void *pUserData)
{
	CServer *pThis = static_cast<CServer *>(pUserData);
	pThis->InitMaplist();
}

void CServer::ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		str_clean_whitespaces(pThis->Config()->m_SvName);
		pThis->ExpireServerInfoAndQueueResend();
	}
}

void CServer::ConchainMaxclientsperipUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CServer *)pUserData)->m_NetServer.SetMaxClientsPerIp(pResult->GetInteger(0));
}

void CServer::ConchainCommandAccessUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	if(pResult->NumArguments() == 2)
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		const IConsole::ICommandInfo *pInfo = pThis->Console()->GetCommandInfo(pResult->GetString(0), CFGFLAG_SERVER, false);
		IConsole::EAccessLevel OldAccessLevel = IConsole::EAccessLevel::ADMIN;
		if(pInfo)
			OldAccessLevel = pInfo->GetAccessLevel();
		pfnCallback(pResult, pCallbackUserData);
		if(pInfo && OldAccessLevel != pInfo->GetAccessLevel())
		{
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(pThis->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
					continue;
				if(!pThis->IsRconAuthed(i))
					continue;

				const IConsole::EAccessLevel ClientAccessLevel = pThis->ConsoleAccessLevel(i);
				bool HadAccess = OldAccessLevel >= ClientAccessLevel;
				bool HasAccess = pInfo->GetAccessLevel() >= ClientAccessLevel;

				// Nothing changed
				if(HadAccess == HasAccess)
					continue;
				// Command not sent yet. The sending will happen in alphabetical order with correctly updated permissions.
				if(pThis->m_aClients[i].m_pRconCmdToSend && str_comp(pResult->GetString(0), pThis->m_aClients[i].m_pRconCmdToSend->Name()) >= 0)
					continue;

				if(HasAccess)
					pThis->SendRconCmdAdd(pInfo, i);
				else
					pThis->SendRconCmdRem(pInfo, i);
			}
		}
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

void CServer::LogoutClient(int ClientId, const char *pReason)
{
	if(!IsSixup(ClientId))
	{
		CMsgPacker Msg(NETMSG_RCON_AUTH_STATUS, true);
		Msg.AddInt(0); //authed
		Msg.AddInt(0); //cmdlist
		SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
	}
	else
	{
		CMsgPacker Msg(protocol7::NETMSG_RCON_AUTH_OFF, true, true);
		SendMsg(&Msg, MSGFLAG_VITAL, ClientId);
	}

	m_aClients[ClientId].m_AuthTries = 0;
	m_aClients[ClientId].m_pRconCmdToSend = nullptr;
	m_aClients[ClientId].m_MaplistEntryToSend = CClient::MAPLIST_UNINITIALIZED;

	if(*pReason)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Logged out by %s.", pReason);
		SendRconLine(ClientId, aBuf);
		log_info("server", "ClientId=%d with key='%s' logged out by %s", ClientId, m_AuthManager.KeyIdent(m_aClients[ClientId].m_AuthKey), pReason);
	}
	else
	{
		SendRconLine(ClientId, "Logout successful.");
		log_info("server", "ClientId=%d with key='%s' logged out", ClientId, m_AuthManager.KeyIdent(m_aClients[ClientId].m_AuthKey));
	}

	m_aClients[ClientId].m_AuthKey = -1;

	GameServer()->OnSetAuthed(ClientId, AUTHED_NO);
}

void CServer::LogoutKey(int Key, const char *pReason)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(m_aClients[i].m_AuthKey == Key)
			LogoutClient(i, pReason);
}

void CServer::ConchainRconPasswordChangeGeneric(const char *pRoleName, const char *pCurrent, IConsole::IResult *pResult)
{
	if(pResult->NumArguments() == 1)
	{
		int KeySlot = m_AuthManager.DefaultKey(pRoleName);
		const char *pNew = pResult->GetString(0);
		if(str_comp(pCurrent, pNew) == 0)
		{
			return;
		}
		if(KeySlot == -1 && pNew[0])
		{
			m_AuthManager.AddDefaultKey(pRoleName, pNew);
		}
		else if(KeySlot >= 0)
		{
			if(!pNew[0])
			{
				AuthRemoveKey(KeySlot);
				// Already logs users out.
			}
			else
			{
				m_AuthManager.UpdateKey(KeySlot, pNew, pRoleName);
				LogoutKey(KeySlot, "key update");
			}
		}
	}
}

void CServer::ConchainRconPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pThis = static_cast<CServer *>(pUserData);
	pThis->ConchainRconPasswordChangeGeneric(RoleName::ADMIN, pThis->Config()->m_SvRconPassword, pResult);
	pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainRconModPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pThis = static_cast<CServer *>(pUserData);
	pThis->ConchainRconPasswordChangeGeneric(RoleName::MODERATOR, pThis->Config()->m_SvRconModPassword, pResult);
	pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainRconHelperPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pThis = static_cast<CServer *>(pUserData);
	pThis->ConchainRconPasswordChangeGeneric(RoleName::HELPER, pThis->Config()->m_SvRconHelperPassword, pResult);
	pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainMapUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pThis = static_cast<CServer *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() >= 1 && pThis->GameServer()->Map()->IsLoaded())
	{
		pThis->m_MapReload = str_comp(pThis->Config()->m_SvMap, pThis->GameServer()->Map()->FullName()) != 0;
	}
}

void CServer::ConchainSixupUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	CServer *pThis = static_cast<CServer *>(pUserData);
	if(pResult->NumArguments() >= 1 && pThis->GameServer()->Map()->IsLoaded())
	{
		pThis->m_MapReload |= (pThis->m_apCurrentMapData[MAP_TYPE_SIXUP] != nullptr) != (pResult->GetInteger(0) != 0);
	}
}

void CServer::ConchainRegisterCommunityTokenRedact(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	// community tokens look like this:
	// ddtc_6DnZq5Ix0J2kvDHbkPNtb6bsZxOVQg4ly2jw. The first 11 bytes are
	// shared between the token and the verification token, so they're
	// semi-public. Redact everything beyond that point.
	static constexpr int REDACT_FROM = 11;
	if(pResult->NumArguments() == 0 && str_length(g_Config.m_SvRegisterCommunityToken) > REDACT_FROM)
	{
		char aTruncated[16];
		str_truncate(aTruncated, sizeof(aTruncated), g_Config.m_SvRegisterCommunityToken, REDACT_FROM);
		log_info("config", "Value: %s[REDACTED] (total length %d)", aTruncated, str_length(g_Config.m_SvRegisterCommunityToken));
		return;
	}
	pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainLoglevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pSelf = (CServer *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		pSelf->m_pFileLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_Loglevel)});
	}
}

void CServer::ConchainStdoutOutputLevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pSelf = (CServer *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pSelf->m_pStdoutLogger)
	{
		pSelf->m_pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_StdoutOutputLevel)});
	}
}

void CServer::ConchainAnnouncementFilename(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pSelf = (CServer *)pUserData;
	bool Changed = pResult->NumArguments() && str_comp(pResult->GetString(0), g_Config.m_SvAnnouncementFilename);
	pfnCallback(pResult, pCallbackUserData);
	if(Changed)
	{
		pSelf->ReadAnnouncementsFile();
	}
}

void CServer::ConchainInputFifo(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CServer *pSelf = (CServer *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pSelf->m_Fifo.IsInit())
	{
		pSelf->m_Fifo.Shutdown();
		pSelf->m_Fifo.Init(pSelf->Console(), pSelf->Config()->m_SvInputFifo, CFGFLAG_SERVER);
	}
}

#if defined(CONF_FAMILY_UNIX)
void CServer::ConchainConnLoggingServerChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() == 1)
	{
		CServer *pServer = (CServer *)pUserData;

		// open socket to send new connections
		if(!pServer->m_ConnLoggingSocketCreated)
		{
			pServer->m_ConnLoggingSocket = net_unix_create_unnamed();
			if(pServer->m_ConnLoggingSocket == -1)
			{
				pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Failed to created socket for communication with the connection logging server.");
			}
			else
			{
				pServer->m_ConnLoggingSocketCreated = true;
			}
		}

		// set the destination address for the connection logging
		net_unix_set_addr(&pServer->m_ConnLoggingDestAddr, pResult->GetString(0));
	}
}
#endif

void CServer::RegisterCommands()
{
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pGameServer = Kernel()->RequestInterface<IGameServer>();
	m_pHttp = Kernel()->RequestInterface<IEngineHttp>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pAntibot = Kernel()->RequestInterface<IEngineAntibot>();

	// register console commands
	Console()->Register("kick", "v[id] ?r[reason]", CFGFLAG_SERVER, ConKick, this, "Kick player with specified id for any reason");
	Console()->Register("status", "?r[name]", CFGFLAG_SERVER, ConStatus, this, "List players containing name or all players");
	Console()->Register("dump_baseline_stats", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConBaselineStats, this, "Print bounded transport baseline statistics");
	Console()->Register("shutdown", "?r[reason]", CFGFLAG_SERVER, ConShutdown, this, "Shut down");
	Console()->Register("logout", "", CFGFLAG_SERVER, ConLogout, this, "Logout of rcon");
	Console()->Register("show_ips", "?i[show]", CFGFLAG_SERVER, ConShowIps, this, "Show IP addresses in rcon commands (1 = on, 0 = off)");
	Console()->Register("hide_auth_status", "?i[hide]", CFGFLAG_SERVER, ConHideAuthStatus, this, "Opt out of spectator count and hide auth status to non-authed players (1 = hidden, 0 = shown)");
	Console()->Register("force_high_bandwidth_on_spectate", "?i[enable]", CFGFLAG_SERVER, ConForceHighBandwidthOnSpectate, this, "Force high bandwidth mode when spectating (1 = on, 0 = off)");

	Console()->Register("record", "?s[file]", CFGFLAG_SERVER | CFGFLAG_STORE, ConRecord, this, "Record to a file");
	Console()->Register("stoprecord", "", CFGFLAG_SERVER, ConStopRecord, this, "Stop recording");

	Console()->Register("reload", "", CFGFLAG_SERVER, ConMapReload, this, "Reload the map");

	Console()->Register("add_sqlserver", "s['r'|'w'] s[Database] s[Prefix] s[User] s[Password] s[IP] i[Port] ?i[SetUpDatabase ?] ?i[SSL ?]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAddSqlServer, this, "add a sqlserver");
	Console()->Register("dump_sqlservers", "s['r'|'w']", CFGFLAG_SERVER, ConDumpSqlServers, this, "dumps all sqlservers readservers = r, writeservers = w");

	Console()->Register("auth_add", "s[ident] s[level] r[pw]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthAdd, this, "Add a rcon key");
	Console()->Register("auth_add_p", "s[ident] s[level] s[hash] s[salt]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthAddHashed, this, "Add a prehashed rcon key");
	Console()->Register("auth_change", "s[ident] s[level] r[pw]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthUpdate, this, "Update a rcon key");
	Console()->Register("auth_change_p", "s[ident] s[level] s[hash] s[salt]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthUpdateHashed, this, "Update a rcon key with prehashed data");
	Console()->Register("auth_remove", "s[ident]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthRemove, this, "Remove a rcon key");
	Console()->Register("auth_list", "", CFGFLAG_SERVER, ConAuthList, this, "List all rcon keys");

	Console()->Register("reload_announcement", "", CFGFLAG_SERVER, ConReloadAnnouncement, this, "Reload the announcements");
	Console()->Register("reload_maplist", "", CFGFLAG_SERVER, ConReloadMaplist, this, "Reload the maplist");
#if defined(CONF_WEBSOCKETS)
	Console()->Register("reload_websocket_cert", "", CFGFLAG_SERVER, ConReloadWebsocketCert, this, "Reload the TLS certificate used for websocket connections");
#endif

	RustVersionRegister(*Console());

	Console()->Chain("sv_name", ConchainSpecialInfoupdate, this);
	Console()->Chain("password", ConchainSpecialInfoupdate, this);
	Console()->Chain("sv_reserved_slots", ConchainSpecialInfoupdate, this);
	Console()->Chain("sv_spectator_slots", ConchainSpecialInfoupdate, this);

	Console()->Chain("sv_max_clients_per_ip", ConchainMaxclientsperipUpdate, this);
	Console()->Chain("access_level", ConchainCommandAccessUpdate, this);

	Console()->Chain("sv_rcon_password", ConchainRconPasswordChange, this);
	Console()->Chain("sv_rcon_mod_password", ConchainRconModPasswordChange, this);
	Console()->Chain("sv_rcon_helper_password", ConchainRconHelperPasswordChange, this);
	Console()->Chain("sv_map", ConchainMapUpdate, this);
	Console()->Chain("sv_sixup", ConchainSixupUpdate, this);
	Console()->Chain("sv_register_community_token", ConchainRegisterCommunityTokenRedact, nullptr);

	Console()->Chain("loglevel", ConchainLoglevel, this);
	Console()->Chain("stdout_output_level", ConchainStdoutOutputLevel, this);

	Console()->Chain("sv_announcement_filename", ConchainAnnouncementFilename, this);

	Console()->Chain("sv_input_fifo", ConchainInputFifo, this);

#if defined(CONF_FAMILY_UNIX)
	Console()->Chain("sv_conn_logging_server", ConchainConnLoggingServerChange, this);
#endif

	// register console commands in sub parts
	m_ServerBan.InitServerBan(Console(), Storage(), this);
	m_NameBans.InitConsole(Console());
	m_pGameServer->OnConsoleInit();
	Console()->SetCanUseCommandCallback(CanClientUseCommandCallback, this);
}

std::optional<int> CServer::SnapNewId()
{
	return m_IdPool.NewId();
}

void CServer::SnapFreeId(int Id)
{
	m_IdPool.FreeId(Id);
}

bool CServer::SnapNewItem(int Type, int Id, const void *pData, int Size)
{
	return m_SnapshotBuilder.NewItem(Type, Id, pData, Size);
}

void CServer::SnapSetStaticsize(int ItemType, int Size)
{
	m_SnapshotDelta.SetStaticsize(ItemType, Size);
}

void CServer::SnapSetStaticsize7(int ItemType, int Size)
{
	m_SnapshotDeltaSixup.SetStaticsize(ItemType, Size);
}

CServer *CreateServer() { return new CServer(); }

// DDRace

void CServer::ReadAnnouncementsFile()
{
	m_vAnnouncements.clear();

	if(g_Config.m_SvAnnouncementFilename[0] == '\0')
		return;

	CLineReader LineReader;
	if(!LineReader.OpenFile(m_pStorage->OpenFile(g_Config.m_SvAnnouncementFilename, IOFLAG_READ, IStorage::TYPE_ALL)))
	{
		log_error("server", "Failed load announcements from '%s'", g_Config.m_SvAnnouncementFilename);
		return;
	}
	while(const char *pLine = LineReader.Get())
	{
		if(str_length(pLine) && pLine[0] != '#')
		{
			m_vAnnouncements.emplace_back(pLine);
		}
	}
	log_info("server", "Loaded %" PRIzu " announcements", m_vAnnouncements.size());
}

const char *CServer::GetAnnouncementLine()
{
	if(m_vAnnouncements.empty())
	{
		return nullptr;
	}
	else if(m_vAnnouncements.size() == 1)
	{
		m_AnnouncementLastLine = 0;
	}
	else if(!g_Config.m_SvAnnouncementRandom)
	{
		if(++m_AnnouncementLastLine >= m_vAnnouncements.size())
			m_AnnouncementLastLine %= m_vAnnouncements.size();
	}
	else
	{
		unsigned Rand;
		do
		{
			Rand = rand() % m_vAnnouncements.size();
		} while(Rand == m_AnnouncementLastLine);

		m_AnnouncementLastLine = Rand;
	}

	return m_vAnnouncements[m_AnnouncementLastLine].c_str();
}

struct CSubdirCallbackUserdata
{
	CServer *m_pServer;
	char m_aCurrentFolder[IO_MAX_PATH_LENGTH];
};

int CServer::MaplistEntryCallback(const char *pFilename, int IsDir, int DirType, void *pUser)
{
	CSubdirCallbackUserdata *pUserdata = static_cast<CSubdirCallbackUserdata *>(pUser);
	CServer *pThis = pUserdata->m_pServer;

	if(str_comp(pFilename, ".") == 0 || str_comp(pFilename, "..") == 0)
		return 0;

	char aFilename[IO_MAX_PATH_LENGTH];
	if(pUserdata->m_aCurrentFolder[0] != '\0')
		str_format(aFilename, sizeof(aFilename), "%s/%s", pUserdata->m_aCurrentFolder, pFilename);
	else
		str_copy(aFilename, pFilename);

	if(IsDir)
	{
		CSubdirCallbackUserdata Userdata;
		Userdata.m_pServer = pThis;
		str_copy(Userdata.m_aCurrentFolder, aFilename);
		char aFindPath[IO_MAX_PATH_LENGTH];
		str_format(aFindPath, sizeof(aFindPath), "maps/%s/", aFilename);
		pThis->Storage()->ListDirectory(IStorage::TYPE_ALL, aFindPath, MaplistEntryCallback, &Userdata);
		return 0;
	}

	const char *pSuffix = str_endswith(aFilename, ".map");
	if(!pSuffix) // not ending with .map
		return 0;
	const size_t FilenameLength = pSuffix - aFilename;
	aFilename[FilenameLength] = '\0'; // remove suffix
	if(FilenameLength >= sizeof(CMaplistEntry().m_aName)) // name too long
		return 0;

	pThis->m_vMaplistEntries.emplace_back(aFilename);
	return 0;
}

void CServer::InitMaplist()
{
	m_vMaplistEntries.clear();

	CSubdirCallbackUserdata Userdata;
	Userdata.m_pServer = this;
	Userdata.m_aCurrentFolder[0] = '\0';
	Storage()->ListDirectory(IStorage::TYPE_ALL, "maps/", MaplistEntryCallback, &Userdata);

	std::sort(m_vMaplistEntries.begin(), m_vMaplistEntries.end());
	log_info("server", "Found %d maps for maplist", (int)m_vMaplistEntries.size());

	for(CClient &Client : m_aClients)
	{
		if(Client.m_State != CClient::STATE_INGAME)
			continue;

		// Resend maplist to clients that already got it or are currently getting it
		if(Client.m_MaplistEntryToSend == CClient::MAPLIST_DONE || Client.m_MaplistEntryToSend >= 0)
		{
			Client.m_MaplistEntryToSend = CClient::MAPLIST_UNINITIALIZED;
		}
	}
}

int *CServer::GetIdMap(int ClientId)
{
	return m_aClients[ClientId].m_aIdMap;
}

int *CServer::GetReverseIdMap(int ClientId)
{
	return m_aClients[ClientId].m_aReverseIdMap;
}

bool CServer::SetTimedOut(int ClientId, int OrigId)
{
	if(!m_NetServer.HasErrored(ClientId))
	{
		return false;
	}

	// The login was on the current conn, logout should also be on the current conn
	if(IsRconAuthed(OrigId))
	{
		LogoutClient(OrigId, "Timeout Protection");
	}

	m_NetServer.ResumeOldConnection(ClientId, OrigId);

	m_aClients[ClientId].m_Sixup = m_aClients[OrigId].m_Sixup;
	// This slot keeps playing with the resumed connection, which can use the other
	// protocol version, so its snapshots must not be used as delta base anymore.
	m_aClients[ClientId].m_Snapshots.PurgeAll();
	m_aClients[ClientId].m_LastAckedSnapshot = -1;
	m_aClients[ClientId].m_AuthKey = -1;
	m_aClients[ClientId].m_Flags = m_aClients[OrigId].m_Flags;
	m_aClients[ClientId].m_DDNetVersion = m_aClients[OrigId].m_DDNetVersion;
	m_aClients[ClientId].m_GotDDNetVersionPacket = m_aClients[OrigId].m_GotDDNetVersionPacket;
	m_aClients[ClientId].m_DDNetVersionSettled = m_aClients[OrigId].m_DDNetVersionSettled;

	DelClientCallback(OrigId, "Timeout Protection used", this);

	// ReinitPlayerMap must be called after DelClientCallback to preserve the client id.
	// The order is important for the player initialization algorithm in CPlayerMapping::CPlayerMap::InitPlayer
	// because it loops over all players to find others with the same ip address.
	// IP matching is important for hammerfly/dummy copy to work by guaran-tee-ing dummy and player map have the same ids
	// Never forget: 0.7 really implemented netmsgs for join/leave, means client ids have to be stable across using timeout protection.
	// When InitPlayer runs it has to assign the same client id as before since local id cant be changed in 0.7
	GameServer()->ReinitPlayerMap(ClientId, true);
	return true;
}

void CServer::SetErrorShutdown(const char *pReason)
{
	str_copy(m_aErrorShutdownReason, pReason);
}

void CServer::SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger)
{
	m_pFileLogger = pFileLogger;
	m_pStdoutLogger = pStdoutLogger;
}
