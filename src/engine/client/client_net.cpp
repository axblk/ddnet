/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "client_net.h"

#include <base/bytes.h>
#include <base/color.h>
#include <base/dbg.h>
#include <base/fs.h>
#include <base/hash.h>
#include <base/hash_ctxt.h>
#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/os.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/http.h>
#include <engine/map.h>
#include <engine/shared/compression.h>
#include <engine/shared/config.h>
#include <engine/shared/game_wire.h>
#include <engine/shared/masterserver.h>
#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol7.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/protocolglue.h>
#include <engine/shared/serverinfo.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/uuid_manager.h>
#include <engine/storage.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>
#include <generated/protocolglue.h>

#include <game/localization.h>
#include <game/version.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <thread>

using namespace std::chrono_literals;

static constexpr ColorRGBA CLIENT_NETWORK_PRINT_COLOR = ColorRGBA(0.7f, 1, 0.7f, 1.0f);
static constexpr ColorRGBA CLIENT_NETWORK_PRINT_ERROR_COLOR = ColorRGBA(1.0f, 0.25f, 0.25f, 1.0f);
static constexpr size_t MAX_QUIC_KNOWN_HOSTS = 256;

static bool NormalizeQuicTrustHost(const char *pHost, char *pBuffer, int BufferSize)
{
	if(!pHost || pHost[0] == '\0' || str_length(pHost) >= BufferSize || !str_utf8_check(pHost))
		return false;
	NETADDR Address;
	if(net_addr_from_str(&Address, pHost) == 0)
	{
		if(Address.port != 0)
			return false;
		net_addr_str(&Address, pBuffer, BufferSize, false);
		if(Address.type == NETTYPE_IPV6)
		{
			const int Length = str_length(pBuffer);
			mem_move(pBuffer, pBuffer + 1, Length - 2);
			pBuffer[Length - 2] = '\0';
		}
		return true;
	}
	str_utf8_tolower(pHost, pBuffer, BufferSize);
	int Length = str_length(pBuffer);
	if(Length > 0 && pBuffer[Length - 1] == '.')
		pBuffer[--Length] = '\0';
	if(Length == 0)
		return false;
	for(const unsigned char *p = reinterpret_cast<const unsigned char *>(pBuffer); *p; ++p)
	{
		if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_'))
			return false;
	}
	return true;
}

CSessionId CClientWithConnection::CreateNetworkSession()
{
	auto pSource = std::make_unique<CNetworkSessionSource>();
	CNetworkSessionSource *pSourceRaw = pSource.get();
	const CSessionId SessionId = m_SessionManager.Create(std::move(pSource));
	if(!SessionId.IsValid())
		return {};
	if(m_NetworkInitialized)
	{
		for(const auto &pStream : pSourceRaw->Streams())
		{
			int Port = 0;
			char aName[64];
			str_format(aName, sizeof(aName), "session %" PRIu64 " stream %" PRIu64, SessionId.Value(), pStream->m_Id.Value());
			char aError[256];
			if(InitNetworkStream(m_NetworkBindAddr, pStream->m_NetClient, Port, aName, aError, sizeof(aError)))
				continue;
			for(const auto &pOpenedStream : pSourceRaw->Streams())
				pOpenedStream->m_NetClient.Close();
			m_SessionManager.Destroy(SessionId);
			AddWarning(SWarning(Localize("Network error"), aError));
			return {};
		}
	}
	for(bool Sixup : {false, true})
	{
		for(const auto &[ItemType, Size] : m_avSnapshotStaticSizes[Sixup])
			pSourceRaw->SnapshotDelta(Sixup).SetStaticsize(ItemType, Size);
	}
	pSourceRaw->SetLifecycleCallbacks([this, SessionId]() { UpdateNetworkSession(SessionId); }, [this, SessionId](const char *pReason) { StopNetworkSession(SessionId, pReason); });
	GameClient()->OnSessionCreated(SessionId);
	return SessionId;
}

CStreamId CClientWithConnection::ConnectAdditionalStream(CSessionId SessionId)
{
	CGameSession *pSession = m_SessionManager.Find(SessionId);
	if(pSession == nullptr || SessionId == m_NetworkSessionId || pSession->Source().Type() != ESessionSourceType::NETWORK)
		return {};
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	if(Source.State() != ESessionState::READY)
		return {};

	CStreamId StreamId;
	for(const auto &pStream : Source.Streams())
	{
		if(pStream->m_Id != Source.PrimaryStreamId() && pStream->m_NetClient.State() == NETSTATE_OFFLINE)
		{
			StreamId = pStream->m_Id;
			break;
		}
	}
	if(!StreamId.IsValid())
	{
		StreamId = Source.CreateStream();
		CNetClient *pNetClient = Source.NetClient(StreamId);
		dbg_assert(pNetClient != nullptr, "failed to create Network stream");
		int Port = 0;
		char aName[64];
		str_format(aName, sizeof(aName), "session %" PRIu64 " stream %" PRIu64, SessionId.Value(), StreamId.Value());
		char aError[256];
		if(!InitNetworkStream(m_NetworkBindAddr, *pNetClient, Port, aName, aError, sizeof(aError)))
		{
			Source.DestroyStream(StreamId);
			AddWarning(SWarning(Localize("Network error"), aError));
			return {};
		}
		GameClient()->OnSessionStreamsChanged(SessionId);
	}

	CNetworkSessionSource::CStreamConnection *pStream = nullptr;
	for(const auto &pCandidate : Source.Streams())
	{
		if(pCandidate->m_Id == StreamId)
		{
			pStream = pCandidate.get();
			break;
		}
	}
	dbg_assert(pStream != nullptr, "missing additional Network stream");
	const NETADDR ServerAddress = SessionServerAddress(SessionId);
	if(IsSixup(SessionId))
		pStream->m_NetClient.Connect7(&ServerAddress, 1);
	else
		pStream->m_NetClient.Connect(&ServerAddress, 1);
	pStream->m_NetClient.RefreshStun();
	pStream->m_Connection.m_InputtimeMarginGraph.Init(-150.0f, 150.0f);
	pStream->m_Connection.m_GametimeMarginGraph.Init(-150.0f, 150.0f);
	pStream->m_SendConnectionInfo = true;
	Source.SetActiveStream(StreamId);
	GenerateTimeoutCodes(SessionId, &ServerAddress, 1);
	return StreamId;
}

bool CClientWithConnection::DestroyNetworkStream(CSessionId SessionId, CStreamId StreamId)
{
	CGameSession *pSession = m_SessionManager.Find(SessionId);
	if(pSession == nullptr || pSession->Source().Type() != ESessionSourceType::NETWORK)
		return false;
	if(SessionId == m_NetworkSessionId && StreamId == this->StreamId(SessionId, CONN_DUMMY))
		return false;
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	if(!Source.DestroyStream(StreamId))
		return false;
	GameClient()->OnSessionStreamsChanged(SessionId);
	return true;
}

bool CClientWithConnection::DestroyNetworkSession(CSessionId SessionId)
{
	CGameSession *pSession = m_SessionManager.Find(SessionId);
	if(pSession == nullptr || SessionId == m_NetworkSessionId || pSession->Source().Type() != ESessionSourceType::NETWORK)
		return false;
	CSessionSourceBase &Source = SessionSource(SessionId);
	if(Source.State() != ESessionState::OFFLINE)
	{
		m_SessionManager.Close(SessionId);
		if(!Source.IsUpdating())
			m_SessionManager.Update(SessionId);
	}
	if(Source.State() != ESessionState::OFFLINE)
		return false;
	GameClient()->OnSessionDestroyed(SessionId);
	return m_SessionManager.Destroy(SessionId);
}

// ----- send functions -----
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
			else if(MsgId == NETMSG_INFO || MsgId == NETMSG_REQUEST_MAP_DATA)
				;
			else if(MsgId == NETMSG_READY)
				MsgId = protocol7::NETMSG_READY;
			else if(MsgId == NETMSG_RCON_CMD)
				MsgId = protocol7::NETMSG_RCON_CMD;
			else if(MsgId == NETMSG_ENTERGAME)
				MsgId = protocol7::NETMSG_ENTERGAME;
			else if(MsgId == NETMSG_INPUT)
				MsgId = protocol7::NETMSG_INPUT;
			else if(MsgId == NETMSG_RCON_AUTH)
				MsgId = protocol7::NETMSG_RCON_AUTH;
			else if(MsgId == NETMSG_PING)
				MsgId = protocol7::NETMSG_PING;
			else
			{
				log_error("net", "0.7 DROP send sys %d", MsgId);
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

	if(pMsg->m_MsgId < OFFSET_UUID)
	{
		Packer.AddInt((MsgId << 1) | (pMsg->m_System ? 1 : 0));
	}
	else
	{
		Packer.AddInt(pMsg->m_System ? 1 : 0); // NETMSG_EX, NETMSGTYPE_EX
		g_UuidManager.PackUuid(pMsg->m_MsgId, &Packer);
	}
	Packer.AddRaw(pMsg->Data(), pMsg->Size());

	return true;
}

int CClientWithConnection::SendMsg(CSessionId SessionId, CStreamId StreamId, CMsgPacker *pMsg, int Flags)
{
	CNetChunk Packet = {};

	if(SessionState(SessionId) == ESessionState::OFFLINE)
		return 0;

	// repack message (inefficient)
	CPacker Pack;
	if(!RepackMsg(pMsg, Pack, IsSixup(SessionId)))
		return 0;

	Packet.m_ClientId = 0;
	Packet.m_pData = Pack.Data();
	Packet.m_DataSize = Pack.Size();
	if(Flags & MSGFLAG_VITAL)
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	if(Flags & MSGFLAG_FLUSH)
		Packet.m_Flags |= NETSENDFLAG_FLUSH;

	if((Flags & MSGFLAG_RECORD) && SessionId == m_NetworkSessionId && StreamId == ActiveStreamId(SessionId))
	{
		RecordMessage(Pack.Data(), Pack.Size());
	}

	if(!(Flags & MSGFLAG_NOSEND))
	{
		if(m_UseQuic && SessionId == m_NetworkSessionId && StreamId == PrimaryStreamId(SessionId))
		{
			const bool Vital = (Flags & MSGFLAG_VITAL) != 0;
			if(!m_QuicTransport.Send(m_QuicSession, Packet.m_pData, Packet.m_DataSize, Vital) && Vital)
			{
				DisconnectWithReason("QUIC reliable queue full");
				return -1;
			}
		}
		else
			NetworkSource(SessionId).NetClient(StreamId)->Send(&Packet);
	}

	return 0;
}

void CClientWithConnection::SendInfo(CSessionId SessionId, CStreamId StreamId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	CMsgPacker MsgVer(NETMSG_CLIENTVER, true);
	MsgVer.AddRaw(&Source.m_ConnectionId, sizeof(Source.m_ConnectionId));
	MsgVer.AddInt(GameClient()->DDNetVersion());
	MsgVer.AddString(GameClient()->DDNetVersionStr());
	SendMsg(SessionId, StreamId, &MsgVer, MSGFLAG_VITAL);

	if(IsSixup(SessionId))
	{
		CMsgPacker Msg(NETMSG_INFO, true);
		Msg.AddString(GAME_NETVERSION7, 128);
		Msg.AddString(Source.m_Password.c_str());
		Msg.AddInt(GameClient()->ClientVersion7());
		SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
		return;
	}

	CMsgPacker Msg(NETMSG_INFO, true);
	Msg.AddString(GameClient()->NetVersion());
	Msg.AddString(Source.m_Password.c_str());
	SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CClientWithConnection::SendEnterGame(int Conn)
{
	SendEnterGame(m_NetworkSessionId, StreamId(m_NetworkSessionId, Conn));
}

void CClientWithConnection::SendEnterGame(CSessionId SessionId, CStreamId StreamId)
{
	CMsgPacker Msg(NETMSG_ENTERGAME, true);
	SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CClientWithConnection::SendReady(int Conn)
{
	SendReady(m_NetworkSessionId, StreamId(m_NetworkSessionId, Conn));
}

void CClientWithConnection::SendReady(CSessionId SessionId, CStreamId StreamId)
{
	CMsgPacker Msg(NETMSG_READY, true);
	SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CClientWithConnection::SendMapRequest(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	dbg_assert(!Source.m_MapdownloadFileTemp, "Map download already in progress");
	Source.m_MapdownloadFileTemp = Storage()->OpenFile(Source.m_aMapdownloadFilenameTemp, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(IsSixup(SessionId))
	{
		CMsgPacker MsgP(protocol7::NETMSG_REQUEST_MAP_DATA, true, true);
		SendMsg(SessionId, Source.PrimaryStreamId(), &MsgP, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	}
	else
	{
		CMsgPacker Msg(NETMSG_REQUEST_MAP_DATA, true);
		Msg.AddInt(Source.m_MapdownloadChunk);
		SendMsg(SessionId, Source.PrimaryStreamId(), &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	}
}

void CClientWithConnection::RconAuth(int Conn, const char *pName, const char *pPassword)
{
	dbg_assert(Conn == CONN_MAIN || Conn == CONN_DUMMY, "invalid game connection");
	if(Connection(Conn).m_RconAuthed != 0)
		return;

	if(pName != m_aRconUsername)
		str_copy(m_aRconUsername, pName);
	if(pPassword != m_aRconPassword)
		str_copy(m_aRconPassword, pPassword);

	if(IsSixup(m_NetworkSessionId))
	{
		CMsgPacker Msg7(protocol7::NETMSG_RCON_AUTH, true, true);
		Msg7.AddString(pPassword);
		SendMsg(Conn, &Msg7, MSGFLAG_VITAL);
		return;
	}

	CMsgPacker Msg(NETMSG_RCON_AUTH, true);
	Msg.AddString(pName);
	Msg.AddString(pPassword);
	Msg.AddInt(1);
	SendMsg(Conn, &Msg, MSGFLAG_VITAL);
}

void CClientWithConnection::Rcon(const char *pCmd)
{
	CMsgPacker Msg(NETMSG_RCON_CMD, true);
	Msg.AddString(pCmd);
	SendMsg(ActiveConnection(), &Msg, MSGFLAG_VITAL);
}

float CClientWithConnection::GotRconCommandsPercentage() const
{
	const CNetworkSessionSource &Source = *m_pNetworkSessionSource;
	if(Source.m_ExpectedRconCommands <= 0)
		return -1.0f;
	if(Source.m_GotRconCommands > Source.m_ExpectedRconCommands)
		return -1.0f;

	return (float)Source.m_GotRconCommands / (float)Source.m_ExpectedRconCommands;
}

float CClientWithConnection::GotMaplistPercentage() const
{
	const CNetworkSessionSource &Source = *m_pNetworkSessionSource;
	if(Source.m_ExpectedMaplistEntries <= 0)
		return -1.0f;
	if(Source.m_vMaplistEntries.size() > (size_t)Source.m_ExpectedMaplistEntries)
		return -1.0f;

	return (float)Source.m_vMaplistEntries.size() / (float)Source.m_ExpectedMaplistEntries;
}

bool CClientWithConnection::ConnectionProblems(CSessionId SessionId, CStreamId StreamId) const
{
	if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
		return false;
	const int64_t MaxLatency = MaxLatencyTicks(SessionId) * time_freq() / GameTickSpeed();
	// Over QUIC nothing arrives through the legacy connection, so asking it when
	// the last packet came in reports trouble for the whole session.
	if(SessionId == m_NetworkSessionId && m_UseQuic && m_QuicConnected)
		return time_get() - m_QuicLastRecvTime > MaxLatency;
	return NetworkSource(SessionId).NetClient(StreamId)->GotProblems(MaxLatency);
}

void CClientWithConnection::SendInput(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	const CStreamId ActiveStreamId = Source.ActiveStreamId();
	if(!ActiveStreamId.IsValid() || Connection(SessionId, ActiveStreamId).m_PredTick <= 0)
		return;

	const int64_t Now = time_get();
	bool Force = false;
	auto SendStreamInput = [&](CStreamId StreamId) {
		CConnection &GameConnection = Connection(SessionId, StreamId);
		if(GameConnection.m_PredTick <= 0)
			return;
		int Size = GameClient()->OnSnapInput(SessionId, GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_aData, StreamId, Force);

		if(Size)
		{
			// pack input
			CMsgPacker Msg(NETMSG_INPUT, true);
			Msg.AddInt(GameConnection.m_AckGameTick);
			Msg.AddInt(GameConnection.m_PredTick);
			Msg.AddInt(Size);

			GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_Tick = GameConnection.m_PredTick;
			GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_PredictedTime = GameConnection.m_PredictedTime.Get(Now);
			GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_PredictionMargin = PredictionMargin(SessionId) * time_freq() / 1000;
			GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_Time = Now;

			// pack it
			for(int k = 0; k < Size / 4; k++)
			{
				static const int FlagsOffset = offsetof(CNetObj_PlayerInput, m_PlayerFlags) / sizeof(int);
				if(k == FlagsOffset && IsSixup(SessionId))
				{
					int PlayerFlags = GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_aData[k];
					Msg.AddInt(PlayerFlags_SixToSeven(PlayerFlags));
				}
				else
				{
					Msg.AddInt(GameConnection.m_aInputs[GameConnection.m_CurrentInput].m_aData[k]);
				}
			}

			GameConnection.m_CurrentInput++;
			GameConnection.m_CurrentInput %= 200;

			SendMsg(SessionId, StreamId, &Msg, MSGFLAG_FLUSH);
			// ugly workaround for dummy. we need to send input with dummy to prevent
			// prediction time resets. but if we do it too often, then it's
			// impossible to use grenade with frozen dummy that gets hammered...
			if(g_Config.m_ClDummyCopyMoves || GameConnection.m_CurrentInput % 2)
				Force = true;
		}
	};
	SendStreamInput(ActiveStreamId);
	for(const auto &pStream : Source.Streams())
	{
		if(pStream->m_Id != ActiveStreamId && pStream->m_NetClient.State() == NETSTATE_ONLINE)
			SendStreamInput(pStream->m_Id);
	}
}

// TODO: OPT: do this a lot smarter!
int *CClientWithConnection::GetInput(CSessionId SessionId, CStreamId StreamId, int Tick) const
{
	int Best = -1;
	const CConnection &GameConnection = Connection(SessionId, StreamId);
	for(int i = 0; i < 200; i++)
	{
		if(GameConnection.m_aInputs[i].m_Tick != -1 && GameConnection.m_aInputs[i].m_Tick <= Tick && (Best == -1 || GameConnection.m_aInputs[Best].m_Tick < GameConnection.m_aInputs[i].m_Tick))
			Best = i;
	}

	if(Best != -1)
		return (int *)GameConnection.m_aInputs[Best].m_aData;
	return nullptr;
}

// ------ state handling -----
// called when the map is loaded and we should init for a new round
void CClientWithConnection::OnEnterGame(int Conn)
{
	CConnection &GameConnection = Connection(Conn);
	GameConnection.ResetGameplay();
	// Also make gameclient aware that snapshots have been purged
	GameClient()->InvalidateSnapshot(m_NetworkSessionId);
	if(Conn == CONN_MAIN)
	{
		m_LastDummyConnectTime = 0.0f;
	}

	GameClient()->OnEnterGame(m_NetworkSessionId);
}

void CClientWithConnection::EnterGame(CSessionId SessionId, CStreamId StreamId)
{
	if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
		return;
	CConnection &GameConnection = Connection(SessionId, StreamId);
	GameConnection.m_DidPostConnect = false;

	// now we will wait for two snapshots
	// to finish the connection
	SendEnterGame(SessionId, StreamId);
	GameConnection.ResetGameplay();
	GameClient()->InvalidateSnapshot(SessionId);
	GameClient()->OnEnterGame(SessionId);

	if(StreamId == NetworkSource(SessionId).PrimaryStreamId())
	{
		CNetworkSessionSource &Source = NetworkSource(SessionId);
		if(SessionId == m_NetworkSessionId)
			m_LastDummyConnectTime = 0.0f;
		Source.m_NextPingTime = time_get() + time_freq() / 2;
		RequestServerInfo(SessionId); // fresh one for timeout protection
	}
}

void CClientWithConnection::OnPostConnect(int Conn)
{
	if(!m_pNetworkSessionSource->m_ServerCapabilities.m_ChatTimeoutCode)
		return;

	char aBufMsg[256];
	if(!g_Config.m_ClRunOnJoin[0] && !g_Config.m_ClDummyDefaultEyes && !g_Config.m_ClPlayerDefaultEyes)
		str_format(aBufMsg, sizeof(aBufMsg), "/timeout %s", Connection(Conn).m_aTimeoutCode);
	else
		str_format(aBufMsg, sizeof(aBufMsg), "/mc;timeout %s", Connection(Conn).m_aTimeoutCode);

	if(g_Config.m_ClDummyDefaultEyes || g_Config.m_ClPlayerDefaultEyes)
	{
		int Emote = Conn == CONN_DUMMY ? g_Config.m_ClDummyDefaultEyes : g_Config.m_ClPlayerDefaultEyes;

		if(Emote != EMOTE_NORMAL)
		{
			char aBuf[32];
			static const char *s_EMOTE_NAMES[] = {
				"pain",
				"happy",
				"surprise",
				"angry",
				"blink",
			};
			static_assert(std::size(s_EMOTE_NAMES) == NUM_EMOTES - 1, "The size of EMOTE_NAMES must match NUM_EMOTES - 1");

			str_append(aBufMsg, ";");
			str_format(aBuf, sizeof(aBuf), "emote %s %d", s_EMOTE_NAMES[Emote - 1], g_Config.m_ClEyeDuration);
			str_append(aBufMsg, aBuf);
		}
	}
	if(g_Config.m_ClRunOnJoin[0])
	{
		str_append(aBufMsg, ";");
		str_append(aBufMsg, g_Config.m_ClRunOnJoin);
	}
	if(IsSixup(m_NetworkSessionId))
	{
		protocol7::CNetMsg_Cl_Say Msg7;
		Msg7.m_Mode = protocol7::CHAT_ALL;
		Msg7.m_Target = -1;
		Msg7.m_pMessage = aBufMsg;
		SendPackMsg(Conn, &Msg7, MSGFLAG_VITAL, true);
	}
	else
	{
		CNetMsg_Cl_Say MsgP;
		MsgP.m_Team = 0;
		MsgP.m_pMessage = aBufMsg;
		CMsgPacker PackerTimeout(&MsgP);
		MsgP.Pack(&PackerTimeout);
		SendMsg(Conn, &PackerTimeout, MSGFLAG_VITAL);
	}
}

static void GenerateTimeoutCode(char *pBuffer, unsigned Size, char *pSeed, const NETADDR *pAddrs, int NumAddrs, bool UseDummyNamespace)
{
	MD5_CTX Md5;
	md5_init(&Md5);
	const char *pDummy = UseDummyNamespace ? "dummy" : "normal";
	md5_update(&Md5, (unsigned char *)pDummy, str_length(pDummy) + 1);
	md5_update(&Md5, (unsigned char *)pSeed, str_length(pSeed) + 1);
	for(int i = 0; i < NumAddrs; i++)
	{
		NETADDR Address;
		mem_zero(&Address, sizeof(Address));
		Address.type = pAddrs[i].type;
		mem_copy(Address.ip, pAddrs[i].ip, sizeof(Address.ip));
		Address.port = pAddrs[i].port;
		md5_update(&Md5, (unsigned char *)&Address, sizeof(Address));
	}
	MD5_DIGEST Digest = md5_finish(&Md5);

	unsigned short aRandom[8];
	mem_copy(aRandom, Digest.data, sizeof(aRandom));
	generate_password(pBuffer, Size, aRandom, 8);
}

void CClientWithConnection::GenerateTimeoutSeed()
{
	secure_random_password(g_Config.m_ClTimeoutSeed, sizeof(g_Config.m_ClTimeoutSeed), 16);
}

void CClientWithConnection::GenerateTimeoutCodes(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs)
{
	if(g_Config.m_ClTimeoutSeed[0] == '\0')
	{
		GenerateTimeoutSeed();
	}
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	for(size_t StreamIndex = 0; StreamIndex < Source.NumStreams(); ++StreamIndex)
	{
		CConnection &GameConnection = Source.ConnectionAt(StreamIndex);
		GenerateTimeoutCode(GameConnection.m_aTimeoutCode, sizeof(GameConnection.m_aTimeoutCode), g_Config.m_ClTimeoutSeed, pAddrs, NumAddrs, StreamIndex != 0);
		log_debug("client", "timeout code '%s' (stream %" PRIu64 ")", GameConnection.m_aTimeoutCode, Source.StreamIdAt(StreamIndex).Value());
	}
}

void CClientWithConnection::StartLegacyConnection(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs, bool Sixup)
{
	if(SessionId == m_NetworkSessionId && m_QuicTransport.IsRunning())
	{
		for(int i = 0; i < NumAddrs; i++)
			m_QuicTransport.SetLegacyPeer(&pAddrs[i], true);
	}
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	Source.SetSixup(Sixup);
	CNetClient &PrimaryNetClient = Source.PrimaryNetClient();
	if(Sixup)
		PrimaryNetClient.Connect7(pAddrs, NumAddrs);
	else
		PrimaryNetClient.Connect(pAddrs, NumAddrs);
	PrimaryNetClient.RefreshStun();
}

void CClientWithConnection::ClearQuicTrust()
{
	m_aQuicTrustHost[0] = '\0';
	m_QuicTrustPort = 0;
	m_QuicExpectedIdentity = {};
	m_QuicIdentityRequired = false;
	m_QuicIdentityKnown = false;
	m_QuicRememberIdentity = false;
}

const CClientWithConnection::CQuicKnownHost *CClientWithConnection::FindQuicKnownHost(const char *pHost, int Port) const
{
	for(const CQuicKnownHost &KnownHost : m_vQuicKnownHosts)
	{
		if(KnownHost.m_Port == Port && str_comp(KnownHost.m_aHost, pHost) == 0)
			return &KnownHost;
	}
	return nullptr;
}

bool CClientWithConnection::AddQuicKnownHost(const char *pHost, int Port, SHA256_DIGEST IdentityFingerprint)
{
	char aNormalizedHost[128];
	if(!in_range(Port, 1, 65535) || !NormalizeQuicTrustHost(pHost, aNormalizedHost, sizeof(aNormalizedHost)))
		return false;
	if(const CQuicKnownHost *pKnownHost = FindQuicKnownHost(aNormalizedHost, Port))
		return pKnownHost->m_IdentityFingerprint == IdentityFingerprint;
	if(m_vQuicKnownHosts.size() >= MAX_QUIC_KNOWN_HOSTS)
		return false;
	CQuicKnownHost &KnownHost = m_vQuicKnownHosts.emplace_back();
	str_copy(KnownHost.m_aHost, aNormalizedHost);
	KnownHost.m_Port = Port;
	KnownHost.m_IdentityFingerprint = IdentityFingerprint;
	return true;
}

// A websocket address carries no IPv4 or IPv6 bit, so reducing it to the
// address family leaves a type of zero, which cannot be formatted or connected
// to. QUIC and WebTransport never run over such an address.
static bool ToModernTransportAddress(const NETADDR &Address, NETADDR *pResult)
{
	if((Address.type & (NETTYPE_IPV4 | NETTYPE_IPV6)) == 0)
		return false;
	*pResult = Address;
	pResult->type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	return true;
}

static bool FindModernAddress(const NETADDR *pAddresses, int NumAddresses, const NETADDR &Reference, bool Sixup, NETADDR *pResult)
{
	const NETADDR *pFallback = nullptr;
	NETADDR ReferenceAddress = Reference;
	ReferenceAddress.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	for(int i = 0; i < NumAddresses; i++)
	{
		if(((pAddresses[i].type & NETTYPE_TW7) != 0) != Sixup)
			continue;
		// Without a match the address family is ours to pick, and IPv6 is
		// the one to grow into.
		if(!pFallback || ((pFallback->type & NETTYPE_IPV6) == 0 && (pAddresses[i].type & NETTYPE_IPV6) != 0))
			pFallback = &pAddresses[i];
		NETADDR Address = pAddresses[i];
		Address.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
		if(net_addr_comp_noport(&Address, &ReferenceAddress) == 0)
		{
			*pResult = pAddresses[i];
			return true;
		}
	}
	if(!pFallback)
		return false;
	*pResult = *pFallback;
	return true;
}

// A connect link carries its certificate hashes as `#cert-sha256=A,B`, so the
// commas that separate addresses are only the ones before the fragment.
static const char *NextConnectAddress(const char *pStr, char *pBuffer, int BufferSize)
{
	while(*pStr == ',')
		pStr++;
	if(*pStr == '\0')
		return nullptr;
	const char *pEnd = pStr;
	while(*pEnd != '\0' && *pEnd != ',' && *pEnd != '#')
		pEnd++;
	if(*pEnd == '#')
		pEnd = pStr + str_length(pStr);
	str_truncate(pBuffer, BufferSize, pStr, pEnd - pStr);
	return pEnd;
}

void CClientWithConnection::Connect(const char *pAddress, const char *pPassword)
{
	ConnectSession(m_NetworkSessionId, pAddress, pPassword);
}

void CClientWithConnection::ConnectSession(CSessionId SessionId, const char *pAddress, const char *pPassword)
{
	// Disconnect will not change the state if we are already quitting/restarting
	if(m_State == IClient::STATE_QUITTING || m_State == IClient::STATE_RESTARTING)
		return;
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	if(Source.IsUpdating())
	{
		// Closing a session only requests the stop, and the stop runs once the
		// session update returns. Servers ask for a reconnect or a redirect
		// from inside that update, so the connect has to wait for the next
		// client update instead of finding the session still stopping.
		Source.ScheduleServerConnect(pAddress, pPassword);
		m_SessionManager.Close(SessionId);
		return;
	}
	Source.CancelReconnect();
	if(Source.State() != ESessionState::OFFLINE)
	{
		m_SessionManager.Close(SessionId);
		m_SessionManager.Update(SessionId);
	}
	dbg_assert(Source.State() == ESessionState::OFFLINE, "network session must be offline before connecting");

	const NETADDR LastAddr = SessionServerAddress(SessionId);

	if(pAddress != Source.m_ConnectAddress)
		Source.m_ConnectAddress = pAddress;

	char aMsg[512];
	str_format(aMsg, sizeof(aMsg), "connecting to '%s'", Source.m_ConnectAddress.c_str());
	m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aMsg, CLIENT_NETWORK_PRINT_COLOR);

	int NumConnectAddrs = 0;
	NETADDR aConnectAddrs[MAX_SERVER_ADDRESSES];
	mem_zero(aConnectAddrs, sizeof(aConnectAddrs));
	char aaConnectHosts[MAX_SERVER_ADDRESSES][128] = {};
	char aQuicServerName[128] = {};
	const char *pNextAddr = pAddress;
	char aBuffer[256];
	int NumConnectTokens = 0;
	bool OnlySixup = true;
	bool DirectQuic = false;
	bool DirectWebTransport = false;
	bool InvalidDirectQuicLink = false;
	bool ExplicitQuic = false;
	bool ExplicitWebTransport = false;
	// Nothing picked yet means the best there is, which is QUIC where the client
	// has it and the server announces it; everything below falls back to the
	// legacy transport on its own.
	const EConnectProtocol Protocol = []() {
		if(g_Config.m_ClConnectProtocol >= 0)
			return (EConnectProtocol)std::clamp(g_Config.m_ClConnectProtocol, 0, (int)EConnectProtocol::COUNT - 1);
		return g_Config.m_ClQuic && CQuicTransport::IsCompiled() ? EConnectProtocol::QUIC : EConnectProtocol::LEGACY;
	}();
	const bool ProtocolPicked = g_Config.m_ClConnectProtocol >= 0;
	// IPv6 is the preference and not a demand, so it is left to the resolver,
	// which already takes IPv6 where a hostname has it and IPv4 where it does
	// not. IPv4 is the one that rules a family out.
	CNetClient &PrimaryNetClient = Source.PrimaryNetClient();
	int LookupNetType = PrimaryNetClient.NetType();
	if((EConnectAddressFamily)g_Config.m_ClConnectAddressFamily == EConnectAddressFamily::IPV4)
		LookupNetType &= ~(NETTYPE_IPV6 | NETTYPE_WEBSOCKET_IPV6);
	EModernTransportTrust DirectQuicTrust = EModernTransportTrust::INVALID;
	SHA256_DIGEST DirectQuicFingerprint = {};
	SHA256_DIGEST DirectQuicNextFingerprint = {};
	bool DirectQuicHasNextFingerprint = false;
	[[maybe_unused]] EModernTransportTrust DirectWebTransportTrust = EModernTransportTrust::INVALID;
	[[maybe_unused]] SHA256_DIGEST DirectWebTransportFingerprint = {};
	[[maybe_unused]] SHA256_DIGEST DirectWebTransportNextFingerprint = {};
	[[maybe_unused]] bool DirectWebTransportHasNextFingerprint = false;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	int WebsocketSecure = -1;
	bool MixedWebsocketSchemes = false;
#endif
	while((pNextAddr = NextConnectAddress(pNextAddr, aBuffer, sizeof(aBuffer))))
	{
		NumConnectTokens++;
		const bool QuicUrl = str_startswith(aBuffer, QUIC_CONNECTLINK_DOUBLE_SLASH) || str_startswith(aBuffer, QUIC_CONNECTLINK7_DOUBLE_SLASH);
		const bool WebTransportUrl = str_startswith(aBuffer, WT_CONNECTLINK_DOUBLE_SLASH) || str_startswith(aBuffer, WT_CONNECTLINK7_DOUBLE_SLASH);
		ExplicitQuic |= QuicUrl;
		ExplicitWebTransport |= WebTransportUrl;
		bool ParsedWebTransport = false;
		EModernTransportTrust ParsedTrust = EModernTransportTrust::INVALID;
		SHA256_DIGEST ParsedFingerprint = {};
		SHA256_DIGEST ParsedNextFingerprint = {};
		bool ParsedHasNextFingerprint = false;
		if(QuicUrl && (!ParseModernTransportUrl(aBuffer, &ParsedWebTransport, &ParsedTrust, &ParsedFingerprint, &ParsedNextFingerprint, &ParsedHasNextFingerprint) || ParsedWebTransport))
		{
			InvalidDirectQuicLink = true;
			continue;
		}
		if(WebTransportUrl && (!ParseModernTransportUrl(aBuffer, &ParsedWebTransport, &ParsedTrust, &ParsedFingerprint, &ParsedNextFingerprint, &ParsedHasNextFingerprint) || !ParsedWebTransport))
		{
			InvalidDirectQuicLink = true;
			continue;
		}
		if(QuicUrl)
		{
			if(DirectQuic || DirectWebTransport || NumConnectTokens != 1)
			{
				InvalidDirectQuicLink = true;
				continue;
			}
			DirectQuic = true;
			DirectQuicTrust = ParsedTrust;
			DirectQuicFingerprint = ParsedFingerprint;
			DirectQuicNextFingerprint = ParsedNextFingerprint;
			DirectQuicHasNextFingerprint = ParsedHasNextFingerprint;
		}
		else if(DirectQuic || DirectWebTransport)
		{
			InvalidDirectQuicLink = true;
			continue;
		}
		if(WebTransportUrl)
		{
			if(DirectWebTransport || DirectQuic || NumConnectTokens != 1)
			{
				InvalidDirectQuicLink = true;
				continue;
			}
			DirectWebTransport = true;
			DirectWebTransportTrust = ParsedTrust;
			DirectWebTransportFingerprint = ParsedFingerprint;
			DirectWebTransportNextFingerprint = ParsedNextFingerprint;
			DirectWebTransportHasNextFingerprint = ParsedHasNextFingerprint;
		}
		NETADDR NextAddr;
		char aHost[128];
		NETADDR ParsedAddr;
		const int UrlResult = net_addr_from_url(&ParsedAddr, aBuffer, aHost, sizeof(aHost));
		if(UrlResult > 0)
		{
			if(net_addr_from_str(&ParsedAddr, aBuffer) == 0)
				net_addr_str(&ParsedAddr, aHost, sizeof(aHost), false);
			else
			{
				str_copy(aHost, aBuffer);
				if(char *pPort = const_cast<char *>(str_rchr(aHost, ':')))
					*pPort = '\0';
			}
		}
		else if(UrlResult == 0)
			net_addr_str(&ParsedAddr, aHost, sizeof(aHost), false);
		else if(char *pPort = const_cast<char *>(str_rchr(aHost, ':')))
			*pPort = '\0';
		if(net_addr_from_url_lookup(&NextAddr, aBuffer, LookupNetType) != 0)
		{
			log_error("client", "could not find address of %s", aBuffer);
			continue;
		}
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// Emscripten tunnels all traffic through websockets, so websocket addresses are
		// used like normal addresses and only their scheme is applied globally.
		if((NextAddr.type & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) != 0)
		{
			const int NextWebsocketSecure = (NextAddr.type & NETTYPE_WEBSOCKET_TLS) != 0;
			MixedWebsocketSchemes |= WebsocketSecure >= 0 && WebsocketSecure != NextWebsocketSecure;
			WebsocketSecure = NextWebsocketSecure;
			const bool Ipv4 = (NextAddr.type & NETTYPE_WEBSOCKET_IPV4) != 0;
			NextAddr.type &= ~(NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6 | NETTYPE_WEBSOCKET_TLS);
			NextAddr.type |= Ipv4 ? NETTYPE_IPV4 : NETTYPE_IPV6;
		}
#else
		if((NextAddr.type & NETTYPE_WEBSOCKET_TLS) != 0)
		{
			log_error("client", "secure websockets (ddnet-20+wss://) are not supported by this client");
			continue;
		}
		if((NextAddr.type & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) != 0 &&
			(PrimaryNetClient.NetType() & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) == 0)
		{
			log_error("client", "websockets (ddnet-20+ws://) are not supported by this client");
			continue;
		}
#endif
		const bool Sixup = (NextAddr.type & NETTYPE_TW7) != 0;
		if(NumConnectAddrs == (int)std::size(aConnectAddrs))
		{
			log_warn("client", "too many connect addresses, ignoring %s", aBuffer);
			continue;
		}
		if(NextAddr.port == 0)
		{
			NextAddr.port = 8303;
		}
		if(!Sixup)
			OnlySixup = false;
		if(!NormalizeQuicTrustHost(aHost, aaConnectHosts[NumConnectAddrs], sizeof(aaConnectHosts[NumConnectAddrs])))
		{
			log_error("client", "invalid connect host '%s'", aHost);
			continue;
		}

		char aNextAddr[NETADDR_URL_MAXSTRSIZE];
		net_addr_url_str(&NextAddr, aNextAddr, sizeof(aNextAddr), true);
		log_debug("client", "resolved connect address '%s' to %s", aBuffer, aNextAddr);

		if(NextAddr == LastAddr)
		{
			Source.m_SendPassword = true;
		}

		aConnectAddrs[NumConnectAddrs] = NextAddr;
		if(aQuicServerName[0] == '\0')
			str_copy(aQuicServerName, aaConnectHosts[NumConnectAddrs]);
		NumConnectAddrs += 1;
	}
	if(InvalidDirectQuicLink)
	{
		log_error("client", "invalid direct QUIC link or multiple connect addresses");
		// A connect that only returns leaves the menu waiting for something that
		// never happens, so the reason has to reach the screen as well.
		char aWarning[256];
		str_format(aWarning, sizeof(aWarning), Localize("'%s' is not a valid connect address. See local console for details."), Source.m_ConnectAddress.c_str());
		SWarning Warning(Localize("Connect address error"), aWarning);
		Warning.m_AutoHide = false;
		AddWarning(Warning);
		return;
	}
	if(SessionId != m_NetworkSessionId && (ExplicitQuic || ExplicitWebTransport))
	{
		log_error("client", "QUIC and WebTransport are only supported for the primary network session");
		return;
	}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(MixedWebsocketSchemes)
	{
		log_error("client", "cannot mix ws and wss connect addresses");
		return;
	}
	if(WebsocketSecure >= 0)
		net_websocket_set_secure(WebsocketSecure != 0);
	else
		net_websocket_reset_secure();
#endif

	if(NumConnectAddrs == 0)
	{
		log_error("client", "could not find any connect address");
		char aWarning[256];
		str_format(aWarning, sizeof(aWarning), Localize("Could not resolve connect address '%s'. See local console for details."), Source.m_ConnectAddress.c_str());
		SWarning Warning(Localize("Connect address error"), aWarning);
		Warning.m_AutoHide = false;
		AddWarning(Warning);
		return;
	}

	Source.m_ConnectionId = RandomUuid();
	if(SessionId == m_NetworkSessionId)
		RequestServerInfo(SessionId);

	if(pPassword)
	{
		Source.m_Password = pPassword;
		Source.m_SendPassword = false;
	}
	else if(Source.m_SendPassword)
	{
		Source.m_Password = g_Config.m_Password;
		Source.m_SendPassword = false;
	}
	else
	{
		Source.m_Password.clear();
	}

	Source.m_CanReceiveServerCapabilities = true;
	Source.SetSixup(OnlySixup);
	if(SessionId == m_NetworkSessionId)
		ClearQuicTrust();

	CConnection &PrimaryConnection = *Source.Connection(Source.PrimaryStreamId());
	PrimaryConnection.m_InputtimeMarginGraph.Init(-150.0f, 150.0f);
	PrimaryConnection.m_GametimeMarginGraph.Init(-150.0f, 150.0f);

	GenerateTimeoutCodes(SessionId, aConnectAddrs, NumConnectAddrs);
	const auto SetConnectingState = [&]() {
		if(m_SessionManager.FocusedId() == SessionId)
			SetFocusedState(IClient::STATE_CONNECTING, true);
		else
			Source.SetState(ESessionState::CONNECTING);
	};
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	if(DirectWebTransport)
	{
		log_error("client", "WebTransport links are only supported by the web client");
		return;
	}
#endif
	auto SameCertificatePins = [](const CServerInfo &Left, const CServerInfo &Right, bool WebTransport) {
		if(Left.m_HasQuicIdentityFingerprint != Right.m_HasQuicIdentityFingerprint ||
			Left.m_QuicTrust != Right.m_QuicTrust ||
			str_comp(Left.m_aModernHostname, Right.m_aModernHostname) != 0 ||
			(Left.m_HasQuicIdentityFingerprint && Left.m_QuicIdentityFingerprint != Right.m_QuicIdentityFingerprint))
			return false;
		const bool HasNext = WebTransport ? Left.m_HasWebTransportNextCertificateSha256 : Left.m_HasQuicNextCertificateSha256;
		if(HasNext != (WebTransport ? Right.m_HasWebTransportNextCertificateSha256 : Right.m_HasQuicNextCertificateSha256))
			return false;
		const SHA256_DIGEST &LeftFirst = WebTransport ? Left.m_WebTransportCertificateSha256 : Left.m_QuicCertificateSha256;
		const SHA256_DIGEST &LeftNext = WebTransport ? Left.m_WebTransportNextCertificateSha256 : Left.m_QuicNextCertificateSha256;
		const SHA256_DIGEST &RightFirst = WebTransport ? Right.m_WebTransportCertificateSha256 : Right.m_QuicCertificateSha256;
		const SHA256_DIGEST &RightNext = WebTransport ? Right.m_WebTransportNextCertificateSha256 : Right.m_QuicNextCertificateSha256;
		const bool SameOrder = LeftFirst == RightFirst && (!HasNext || LeftNext == RightNext);
		const bool ReverseOrder = HasNext && LeftFirst == RightNext && LeftNext == RightFirst;
		return SameOrder || ReverseOrder;
	};

	// A server that offers QUIC says so in the browser, including the port it
	// listens on, which is not always the one the legacy transport uses.
	NETADDR AdvertisedQuicAddress = {};
	bool ServerOffersQuic = false;
	bool ServerKnown = false;
	for(int i = 0; i < NumConnectAddrs && !ServerOffersQuic; i++)
	{
		const CServerInfo *pInfo = KnownServerInfo(aConnectAddrs[i]);
		if(pInfo == nullptr)
			continue;
		ServerKnown = true;
		if(pInfo->m_NumQuicAddresses == 0)
			continue;
		ServerOffersQuic = FindModernAddress(pInfo->m_aQuicAddresses, pInfo->m_NumQuicAddresses, aConnectAddrs[i], OnlySixup, &AdvertisedQuicAddress);
	}
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(DirectWebTransport)
	{
		NETADDR WebTransportAddress;
		if(!ToModernTransportAddress(aConnectAddrs[0], &WebTransportAddress))
		{
			log_error("client", "WebTransport cannot be used with this address");
			return;
		}
		char aWebTransportUrl[256];
		const bool UseCertificateHashes = DirectWebTransportTrust == EModernTransportTrust::CERTIFICATE_HASH;
		if(FormatWebTransportUrl(aWebTransportUrl, sizeof(aWebTransportUrl), aaConnectHosts[0], WebTransportAddress.port) &&
			m_QuicTransport.StartWebTransportClient(aWebTransportUrl, &WebTransportAddress, UseCertificateHashes, DirectWebTransportFingerprint,
				UseCertificateHashes && DirectWebTransportHasNextFingerprint ? &DirectWebTransportNextFingerprint : nullptr, OnlySixup))
		{
			m_UseQuic = true;
			m_UseWebTransport = true;
			m_QuicConnected = false;
			m_QuicServerAddress = WebTransportAddress;
			SetConnectingState();
			return;
		}
		log_error("client", "could not start direct WebTransport: %s", m_QuicTransport.ErrorString());
		return;
	}
	// Picking any other transport next to the address field rules WebTransport
	// out, picking it rules the rest out. A WebTransport link asks for it the
	// same way, as long as no QUIC link is asking for the other one.
	const bool WantWebTransport = SessionId == m_NetworkSessionId &&
				      (Protocol == EConnectProtocol::WEBTRANSPORT ||
					      (ExplicitWebTransport && !DirectQuic && !ExplicitQuic));
	if(WantWebTransport && !CQuicTransport::IsWebTransportClientCompiled())
	{
		log_error("client", "this build has no WebTransport, connecting over the legacy transport");
	}
	else if(WantWebTransport)
	{
		const CServerInfo *pWebTransportInfo = nullptr;
		NETADDR WebTransportAddress = {};
		bool MetadataAmbiguous = false;
		for(int i = 0; i < NumConnectAddrs; i++)
		{
			const CServerInfo *pInfo = KnownServerInfo(aConnectAddrs[i]);
			if(pInfo == nullptr || !pInfo->m_WebTransport)
				continue;
			NETADDR PrefixAddress;
			if(!FindModernAddress(pInfo->m_aWebTransportAddresses, pInfo->m_NumWebTransportAddresses, aConnectAddrs[i], OnlySixup, &PrefixAddress))
				continue;
			if(pWebTransportInfo &&
				(str_comp(pWebTransportInfo->m_aWebTransportUrl, pInfo->m_aWebTransportUrl) != 0 ||
					pWebTransportInfo->m_WebTransportCertificateMode != pInfo->m_WebTransportCertificateMode ||
					!SameCertificatePins(*pWebTransportInfo, *pInfo, true)))
			{
				MetadataAmbiguous = true;
				break;
			}
			if(!ToModernTransportAddress(PrefixAddress, &WebTransportAddress))
				continue;
			pWebTransportInfo = pInfo;
		}
		if(pWebTransportInfo && !MetadataAmbiguous)
		{
			const bool UseCertificateHashes = pWebTransportInfo->m_WebTransportCertificateMode == CServerInfo::EWebTransportCertificateMode::HASH;
			char aWebTransportUrl[256];
			const char *pWebTransportUrl = pWebTransportInfo->m_aWebTransportUrl;
			if(pWebTransportUrl[0] == '\0')
			{
				char aWebTransportHost[NETADDR_MAXSTRSIZE];
				if(pWebTransportInfo->m_aModernHostname[0] != '\0')
					str_copy(aWebTransportHost, pWebTransportInfo->m_aModernHostname);
				else
					net_addr_str(&WebTransportAddress, aWebTransportHost, sizeof(aWebTransportHost), false);
				if(FormatWebTransportUrl(aWebTransportUrl, sizeof(aWebTransportUrl), aWebTransportHost, WebTransportAddress.port))
					pWebTransportUrl = aWebTransportUrl;
			}
			if(pWebTransportUrl[0] != '\0' && m_QuicTransport.StartWebTransportClient(
								  pWebTransportUrl,
								  &WebTransportAddress,
								  UseCertificateHashes,
								  pWebTransportInfo->m_WebTransportCertificateSha256,
								  UseCertificateHashes && pWebTransportInfo->m_HasWebTransportNextCertificateSha256 ? &pWebTransportInfo->m_WebTransportNextCertificateSha256 : nullptr,
								  OnlySixup))
			{
				m_UseQuic = true;
				m_UseWebTransport = true;
				m_QuicConnected = false;
				m_QuicServerAddress = WebTransportAddress;
				SetConnectingState();
				return;
			}
			m_QuicTransport.RecordFallback();
			const auto &Metrics = m_QuicTransport.Metrics();
			log_info("quic", "transport=webtransport attempts=%llu connections=%llu failures=%llu/%llu/%llu fallback=%llu handshake_ms=%llu",
				static_cast<unsigned long long>(Metrics.m_ConnectAttempts), static_cast<unsigned long long>(Metrics.m_Connections),
				static_cast<unsigned long long>(Metrics.m_ConnectFailuresNetwork), static_cast<unsigned long long>(Metrics.m_ConnectFailuresIdentity), static_cast<unsigned long long>(Metrics.m_ConnectFailuresProtocol),
				static_cast<unsigned long long>(Metrics.m_Fallbacks), static_cast<unsigned long long>(Metrics.m_LastHandshakeMilliseconds));
			log_info("client", "WebTransport unavailable, using the configured legacy transport: %s", m_QuicTransport.ErrorString());
		}
		else if(MetadataAmbiguous)
			log_warn("client", "conflicting WebTransport identities for connect addresses, using the configured legacy transport");
	}
#endif
	// Preferred next to the address field and announced by the server, or part
	// of a direct link, which carries the address and the identity to use with
	// it. A known server that does not announce QUIC gets the legacy transport;
	// an address the browser has never seen announces nothing either way, so
	// there a transport picked by hand is the only thing to go on, while the
	// automatic pick stays on the legacy transport rather than guessing. Once
	// QUIC is used there is no fallback: a QUIC connect that fails is reported,
	// not quietly retried over the legacy transport, because that is what made
	// connection problems hard to read before.
	const bool WantQuic = SessionId == m_NetworkSessionId &&
			      (DirectQuic || (Protocol == EConnectProtocol::QUIC && (ServerOffersQuic || (ProtocolPicked && !ServerKnown))));
	if(WantQuic && !CQuicTransport::IsCompiled())
	{
		log_error("client", "this build has no QUIC transport, connecting over the legacy transport");
	}
	else if(WantQuic)
	{
		NETADDR QuicAddress;
		if(ServerOffersQuic)
			QuicAddress = AdvertisedQuicAddress;
		else if(!ToModernTransportAddress(aConnectAddrs[0], &QuicAddress))
		{
			log_error("client", "QUIC cannot be used with this address");
			return;
		}
		char aServerAddress[NETADDR_MAXSTRSIZE];
		net_addr_str(&QuicAddress, aServerAddress, sizeof(aServerAddress), true);
		char aBindAddress[NETADDR_MAXSTRSIZE];
		str_format(aBindAddress, sizeof(aBindAddress), QuicAddress.type == NETTYPE_IPV6 ? "[::]:%d" : "0.0.0.0:%d", g_Config.m_ClPort);
		const char *pServerName = g_Config.m_ClQuicServerName[0] != '\0' ? g_Config.m_ClQuicServerName : aQuicServerName;
		const CQuicKnownHost *pKnownHost = FindQuicKnownHost(aaConnectHosts[0], QuicAddress.port);
		bool Started;
		if(DirectQuic && DirectQuicTrust == EModernTransportTrust::IDENTITY)
		{
			m_QuicExpectedIdentity = DirectQuicFingerprint;
			m_QuicIdentityRequired = true;
			Started = m_QuicTransport.StartClientIdentity(aBindAddress, aServerAddress, pServerName, DirectQuicFingerprint, OnlySixup);
		}
		else if(DirectQuic && DirectQuicTrust == EModernTransportTrust::CERTIFICATE_HASH)
			Started = m_QuicTransport.StartClientSha256(aBindAddress, aServerAddress, pServerName, DirectQuicFingerprint, DirectQuicHasNextFingerprint ? &DirectQuicNextFingerprint : nullptr, OnlySixup);
		else if(DirectQuic && DirectQuicTrust == EModernTransportTrust::WEBPKI)
			Started = m_QuicTransport.StartClientWebPki(aBindAddress, aServerAddress, pServerName, OnlySixup);
		else if(!DirectQuic && g_Config.m_ClQuicCert[0] != '\0')
			Started = m_QuicTransport.StartClient(aBindAddress, aServerAddress, pServerName, g_Config.m_ClQuicCert, OnlySixup);
		else if(pKnownHost)
		{
			str_copy(m_aQuicTrustHost, pKnownHost->m_aHost);
			m_QuicTrustPort = pKnownHost->m_Port;
			m_QuicExpectedIdentity = pKnownHost->m_IdentityFingerprint;
			m_QuicIdentityRequired = true;
			m_QuicIdentityKnown = true;
			Started = m_QuicTransport.StartClientIdentity(aBindAddress, aServerAddress, pServerName, pKnownHost->m_IdentityFingerprint, OnlySixup);
		}
		else
		{
			str_copy(m_aQuicTrustHost, aaConnectHosts[0]);
			m_QuicTrustPort = QuicAddress.port;
			m_QuicIdentityRequired = true;
			m_QuicRememberIdentity = true;
			Started = m_QuicTransport.StartClientTofu(aBindAddress, aServerAddress, pServerName, OnlySixup);
		}
		if(!Started)
		{
			log_error("client", "could not start %sQUIC: %s", DirectQuic ? "direct " : "", m_QuicTransport.ErrorString());
			return;
		}
		m_UseQuic = true;
		Source.PrimaryNetClient().SetPacketFilter(
			[](void *pUser, const NETADDR *pRemoteAddress, const void *pData, int DataSize) { return static_cast<CQuicTransport *>(pUser)->FeedUdp(pRemoteAddress, pData, DataSize); },
			&m_QuicTransport);
		m_QuicConnected = false;
		m_QuicServerAddress = QuicAddress;
		SetConnectingState();
		return;
	}

	const CServerInfo *pQuicInfo = nullptr;
	NETADDR QuicAddress = {};
	bool QuicMetadataAmbiguous = false;
	// QUIC is used when the player asked for it. Picking it automatically and
	// racing a legacy connection against it as a safety net was removed: it
	// made every connect depend on browser metadata that may be stale, and the
	// upstream WebTransport work will decide how selection should work.
	if(SessionId == m_NetworkSessionId && ExplicitQuic)
	{
		for(int i = 0; i < NumConnectAddrs; i++)
		{
			const CServerInfo *pInfo = KnownServerInfo(aConnectAddrs[i]);
			if(pInfo == nullptr)
				continue;
			NETADDR PrefixAddress;
			if(!FindModernAddress(pInfo->m_aQuicAddresses, pInfo->m_NumQuicAddresses, aConnectAddrs[i], OnlySixup, &PrefixAddress))
				continue;
			NETADDR NextQuicAddress;
			if(!ToModernTransportAddress(PrefixAddress, &NextQuicAddress))
				continue;
			if(pQuicInfo && (QuicAddress.port != NextQuicAddress.port || !SameCertificatePins(*pQuicInfo, *pInfo, false)))
			{
				QuicMetadataAmbiguous = true;
				break;
			}
			pQuicInfo = pInfo;
			QuicAddress = NextQuicAddress;
		}
	}
	if(pQuicInfo && !QuicMetadataAmbiguous)
	{
		char aServerAddress[NETADDR_MAXSTRSIZE];
		net_addr_str(&QuicAddress, aServerAddress, sizeof(aServerAddress), true);
		char aBindAddress[NETADDR_MAXSTRSIZE];
		str_format(aBindAddress, sizeof(aBindAddress), QuicAddress.type == NETTYPE_IPV6 ? "[::]:%d" : "0.0.0.0:%d", g_Config.m_ClPort);
		char aAutoQuicServerName[NETADDR_MAXSTRSIZE];
		net_addr_str(&QuicAddress, aAutoQuicServerName, sizeof(aAutoQuicServerName), false);
		if(QuicAddress.type == NETTYPE_IPV6)
		{
			const int Length = str_length(aAutoQuicServerName);
			mem_move(aAutoQuicServerName, aAutoQuicServerName + 1, Length - 2);
			aAutoQuicServerName[Length - 2] = '\0';
		}
		const char *pServerName = g_Config.m_ClQuicServerName[0] != '\0' ? g_Config.m_ClQuicServerName : pQuicInfo->m_aModernHostname[0] != '\0' ? pQuicInfo->m_aModernHostname :
																			   aAutoQuicServerName;
		bool Started = false;
		if(pQuicInfo->m_HasQuicIdentityFingerprint)
		{
			m_QuicExpectedIdentity = pQuicInfo->m_QuicIdentityFingerprint;
			m_QuicIdentityRequired = true;
			m_QuicIdentityKnown = true;
			Started = m_QuicTransport.StartClientIdentity(aBindAddress, aServerAddress, pServerName, pQuicInfo->m_QuicIdentityFingerprint, OnlySixup);
		}
		else if(pQuicInfo->m_QuicTrust == EModernTransportTrust::WEBPKI)
			Started = m_QuicTransport.StartClientWebPki(aBindAddress, aServerAddress, pServerName, OnlySixup);
		else
			Started = m_QuicTransport.StartClientSha256(aBindAddress, aServerAddress, pServerName, pQuicInfo->m_QuicCertificateSha256,
				pQuicInfo->m_HasQuicNextCertificateSha256 ? &pQuicInfo->m_QuicNextCertificateSha256 : nullptr, OnlySixup);
		if(Started)
		{
			m_UseQuic = true;
			Source.PrimaryNetClient().SetPacketFilter(
				[](void *pUser, const NETADDR *pRemoteAddress, const void *pData, int DataSize) { return static_cast<CQuicTransport *>(pUser)->FeedUdp(pRemoteAddress, pData, DataSize); },
				&m_QuicTransport);
			m_QuicConnected = false;
			m_QuicServerAddress = QuicAddress;
			SetConnectingState();
			return;
		}
		log_error("client", "could not start QUIC: %s", m_QuicTransport.ErrorString());
		return;
	}
	else if(QuicMetadataAmbiguous)
	{
		log_warn("client", "conflicting QUIC identities for connect addresses, using legacy UDP");
	}
	if(ExplicitQuic || ExplicitWebTransport)
	{
		log_error("client", "the requested modern transport endpoint is unavailable");
		return;
	}
	SetConnectingState();
	StartLegacyConnection(SessionId, aConnectAddrs, NumConnectAddrs, OnlySixup);
}

void CClientWithConnection::DisconnectWithReason(const char *pReason)
{
	m_SessionManager.Close(m_NetworkSessionId, pReason);
	if(!m_pNetworkSessionSource->IsUpdating())
		m_SessionManager.Update(m_NetworkSessionId);
}

void CClientWithConnection::StopNetworkSession(CSessionId SessionId, const char *pReason)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	char aReconnectError[256];
	str_copy(aReconnectError, Source.PrimaryNetClient().ErrorString());
	const bool Focused = m_SessionManager.FocusedId() == SessionId;
	if(pReason != nullptr && pReason[0] == '\0')
		pReason = nullptr;
	// Over QUIC the legacy connection never saw the disconnect, so its error
	// string is empty and the reason has to come from the caller.
	if(SessionId == m_NetworkSessionId && m_UseQuic && pReason != nullptr)
		str_copy(aReconnectError, pReason);
	if(SessionId != m_NetworkSessionId)
	{
		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "disconnecting session %" PRIu64 ". reason='%s'", SessionId.Value(), pReason ? pReason : "unknown");
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf, CLIENT_NETWORK_PRINT_COLOR);
		for(const auto &pStream : Source.Streams())
		{
			pStream->m_NetClient.Disconnect(pReason);
			pStream->m_Connection.ResetSnapshots();
		}
		ResetMapDownload(SessionId, true);
		GameClient()->OnSessionClosed(SessionId);
		Source.ResetAfterDisconnect(aReconnectError, g_Config.m_ClReconnectFull, g_Config.m_ClReconnectTimeout, time_get(), time_freq());
		Source.SetState(ESessionState::OFFLINE);
		if(Focused && m_State < IClient::STATE_QUITTING)
			FocusSession(m_NetworkSessionId);
		return;
	}

	DummyDisconnect(pReason);

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "disconnecting. reason='%s'", pReason ? pReason : "unknown");
	m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf, CLIENT_NETWORK_PRINT_COLOR);

	// Stop recorders and make sure to remove the replay temporary demo.
	for(int Recorder = 0; Recorder < RECORDER_MAX; Recorder++)
	{
		DemoRecorder(Recorder)->Stop(Recorder == RECORDER_REPLAYS ? IDemoRecorder::EStopMode::REMOVE_FILE : IDemoRecorder::EStopMode::KEEP_FILE);
	}

	// Make sure to clear credentials completely from memory
	mem_zero(m_aRconUsername, sizeof(m_aRconUsername));
	mem_zero(m_aRconPassword, sizeof(m_aRconPassword));
	m_pConsole->DeregisterTempAll();
	GameClient()->ForceUpdateConsoleRemoteCompletionSuggestions();
	CNetClient &PrimaryNetClient = Source.PrimaryNetClient();
	if(m_UseQuic && !m_UseWebTransport && m_QuicConnected && m_QuicSession.IsValid() &&
		m_QuicTransport.Close(m_QuicSession, pReason ? pReason : "application disconnect"))
	{
		const CQuicSessionId ClosingSession = m_QuicSession;
		const auto Deadline = std::chrono::steady_clock::now() + 300ms;
		bool Closed = false;
		while(!Closed && std::chrono::steady_clock::now() < Deadline)
		{
			PrimaryNetClient.Update();
			CNetChunk Packet;
			SECURITY_TOKEN ResponseToken;
			while(PrimaryNetClient.Recv(&Packet, &ResponseToken, IsSixup(SessionId)))
			{
			}
			NETADDR Address;
			unsigned char *pData;
			int DataSize;
			while((DataSize = m_QuicTransport.PollUdpSend(&Address, &pData)) > 0)
				PrimaryNetClient.SendRaw(&Address, pData, DataSize);
			CQuicEvent Event;
			while(m_QuicTransport.Poll(Event))
			{
				if(Event.m_Type == EQuicEventType::DISCONNECTED && Event.m_Message.m_Session == ClosingSession)
					Closed = true;
			}
			if(!Closed)
				std::this_thread::sleep_for(1ms);
		}
	}
	if(m_UseQuic)
	{
		const auto &Metrics = m_QuicTransport.Metrics();
		log_info("quic", "transport=%s attempts=%llu connections=%llu failures=%llu/%llu/%llu fallback=%llu handshake_ms=%llu sent=%llu/%llu recv=%llu/%llu bytes=%llu/%llu queue_drop=%llu/%llu queue_high_water=%llu resume_drop=%llu path_change=%llu",
			m_UseWebTransport ? "webtransport" : "quic",
			static_cast<unsigned long long>(Metrics.m_ConnectAttempts), static_cast<unsigned long long>(Metrics.m_Connections),
			static_cast<unsigned long long>(Metrics.m_ConnectFailuresNetwork), static_cast<unsigned long long>(Metrics.m_ConnectFailuresIdentity), static_cast<unsigned long long>(Metrics.m_ConnectFailuresProtocol),
			static_cast<unsigned long long>(Metrics.m_Fallbacks), static_cast<unsigned long long>(Metrics.m_LastHandshakeMilliseconds),
			static_cast<unsigned long long>(Metrics.m_ReliableSent), static_cast<unsigned long long>(Metrics.m_DatagramsSent),
			static_cast<unsigned long long>(Metrics.m_ReliableReceived), static_cast<unsigned long long>(Metrics.m_DatagramsReceived),
			static_cast<unsigned long long>(Metrics.m_BytesSent), static_cast<unsigned long long>(Metrics.m_BytesReceived),
			static_cast<unsigned long long>(Metrics.m_ReliableQueueFull), static_cast<unsigned long long>(Metrics.m_DatagramsDropped),
			static_cast<unsigned long long>(Metrics.m_CommandQueueHighWater),
			static_cast<unsigned long long>(Metrics.m_ResumeSendDrops),
			static_cast<unsigned long long>(Metrics.m_PathChanges));
	}
	m_QuicTransport.Shutdown();
	PrimaryNetClient.SetPacketFilter(nullptr, nullptr);
	m_QuicSession = CQuicSessionId();
	m_UseQuic = false;
	m_UseWebTransport = false;
	m_QuicConnected = false;
	ClearQuicTrust();
	PrimaryNetClient.Disconnect(pReason);
	if(Focused && m_State < IClient::STATE_QUITTING)
		SetFocusedState(IClient::STATE_OFFLINE, true);
	else
	{
		GameClient()->OnSessionClosed(m_NetworkSessionId);
		m_pNetworkSessionSource->SetState(ESessionState::OFFLINE);
	}
	ResetMapDownload(SessionId, true);

	// clear the current server info
	m_pNetworkSessionSource->ResetAfterDisconnect(aReconnectError, g_Config.m_ClReconnectFull, g_Config.m_ClReconnectTimeout, time_get(), time_freq());

	// clear snapshots
	Connection(m_NetworkSessionId, CONN_MAIN).ResetSnapshots();
	SetActiveConnection(CONN_MAIN);
}

void CClientWithConnection::Disconnect()
{
	const CGameSession *pFocusedSession = m_SessionManager.Focused();
	if(pFocusedSession && pFocusedSession->Source().Type() == ESessionSourceType::DEMO && m_pDemoSessionSource->State() != ESessionState::OFFLINE)
	{
		DisconnectDemoWithReason(nullptr);
	}
	else if(pFocusedSession && pFocusedSession->Source().Type() == ESessionSourceType::NETWORK && pFocusedSession->State() != ESessionState::OFFLINE)
	{
		const CSessionId SessionId = pFocusedSession->Id();
		m_SessionManager.Close(SessionId);
		if(!NetworkSource(SessionId).IsUpdating())
			m_SessionManager.Update(SessionId);
	}
}

bool CClientWithConnection::DummyConnected() const
{
	return m_DummyConnected;
}

bool CClientWithConnection::DummyConnecting() const
{
	return m_DummyConnecting;
}

bool CClientWithConnection::DummyConnectingDelayed() const
{
	return !DummyConnected() && !DummyConnecting() && m_LastDummyConnectTime > 0.0f && m_LastDummyConnectTime + 5.0f > GlobalTime();
}

void CClientWithConnection::DummyConnect()
{
	if(m_UseQuic)
	{
		log_info("client", "Dummy clients over QUIC are not supported yet.");
		return;
	}
	if(NetClient(CONN_MAIN).State() != NETSTATE_ONLINE)
	{
		log_info("client", "Not online.");
		return;
	}

	if(!DummyAllowed())
	{
		log_info("client", "Dummy is not allowed on this server.");
		return;
	}
	if(DummyConnecting())
	{
		log_info("client", "Dummy is already connecting.");
		return;
	}
	if(DummyConnected())
	{
		// causes log spam with connect+swap binds
		// https://github.com/ddnet/ddnet/issues/9426
		// log_info("client", "Dummy is already connected.");
		return;
	}
	if(DummyConnectingDelayed())
	{
		log_info("client", "Wait before connecting dummy again.");
		return;
	}

	m_LastDummyConnectTime = GlobalTime();
	Connection(CONN_DUMMY).m_RconAuthed = 0;
	m_DummySendConnInfo = true;

	g_Config.m_ClDummyCopyMoves = 0;
	g_Config.m_ClDummyHammer = 0;

	m_DummyConnecting = true;
	// connect to the server
	if(IsSixup(m_NetworkSessionId))
		NetClient(CONN_DUMMY).Connect7(NetClient(CONN_MAIN).ServerAddress(), 1);
	else
		NetClient(CONN_DUMMY).Connect(NetClient(CONN_MAIN).ServerAddress(), 1);

	Connection(CONN_DUMMY).m_InputtimeMarginGraph.Init(-150.0f, 150.0f);
	Connection(CONN_DUMMY).m_GametimeMarginGraph.Init(-150.0f, 150.0f);
}

void CClientWithConnection::DummyDisconnect(const char *pReason)
{
	NetClient(CONN_DUMMY).Disconnect(pReason);
	g_Config.m_ClDummy = 0;
	SetActiveConnection(CONN_MAIN);

	Connection(CONN_DUMMY).m_RconAuthed = 0;
	Connection(CONN_DUMMY).ResetSnapshots();
	m_DummyConnected = false;
	m_DummyConnecting = false;
	m_DummyReconnectOnReload = false;
	m_DummyDeactivateOnReconnect = false;
	GameClient()->OnDummyDisconnect();
}

bool CClientWithConnection::DummyAllowed() const
{
	return m_pNetworkSessionSource->m_ServerCapabilities.m_AllowDummy;
}

void CClientWithConnection::RequestServerInfo(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	Source.ServerInfo() = {};
	if(SessionId == m_NetworkSessionId)
		m_CurrentServerInfoRequestTime = 0;
	else
		RequestServerInfoRefresh(SessionServerAddress(SessionId));
}

void CClientWithConnection::SetSessionServerInfo(CSessionId SessionId, const CServerInfo &ServerInfo)
{
	CServerInfo &CurrentServerInfo = NetworkSource(SessionId).ServerInfo();
	CurrentServerInfo = ServerInfo;
	if(SessionId == m_NetworkSessionId)
		m_CurrentServerInfoRequestTime = -1;
	const IMap *pMap = GameClient()->Map(SessionId);
	str_copy(CurrentServerInfo.m_aMap, pMap->BaseName());
	CurrentServerInfo.m_MapCrc = pMap->Crc();
	CurrentServerInfo.m_MapSize = pMap->Size();
}

void CClientWithConnection::ResetSocket()
{
	NETADDR BindAddr;
	if(g_Config.m_Bindaddr[0] == '\0')
	{
		mem_zero(&BindAddr, sizeof(BindAddr));
	}
	else if(net_host_lookup(g_Config.m_Bindaddr, &BindAddr, NETTYPE_ALL) != 0)
	{
		log_error("client", "The configured bindaddr '%s' cannot be resolved.", g_Config.m_Bindaddr);
		return;
	}
	BindAddr.type = NETTYPE_ALL;
	m_NetworkBindAddr = BindAddr;
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
			continue;
		CNetworkSessionSource &Source = NetworkSource(SessionId);
		for(size_t StreamIndex = 0; StreamIndex < Source.NumStreams(); ++StreamIndex)
		{
			char aError[256];
			if(SessionId == m_NetworkSessionId && StreamIndex <= CONN_DUMMY)
			{
				if(!InitNetworkClientImpl(BindAddr, static_cast<int>(StreamIndex), aError, sizeof(aError)))
					log_error("client", "%s", aError);
			}
			else
			{
				int Port = 0;
				char aName[64];
				str_format(aName, sizeof(aName), "session %" PRIu64 " stream %" PRIu64, SessionId.Value(), Source.StreamIdAt(StreamIndex).Value());
				if(!InitNetworkStream(BindAddr, Source.NetClientAt(StreamIndex), Port, aName, aError, sizeof(aError)))
					log_error("client", "%s", aError);
			}
		}
	}
	char aContactError[256];
	if(!InitNetworkClientImpl(BindAddr, CONN_CONTACT, aContactError, sizeof(aContactError)))
		log_error("client", "%s", aContactError);
	if(m_UseQuic && !m_UseWebTransport)
	{
		NetworkSource(m_NetworkSessionId).PrimaryNetClient().SetPacketFilter([](void *pUser, const NETADDR *pAddress, const void *pData, int DataSize) { return static_cast<CQuicTransport *>(pUser)->FeedUdp(pAddress, pData, DataSize); }, &m_QuicTransport);
		m_QuicTransport.LocalAddressChanged();
	}
}

const char *CClientWithConnection::DummyName()
{
	if(g_Config.m_ClDummyName[0])
	{
		return g_Config.m_ClDummyName;
	}
	const char *pBase = nullptr;
	if(g_Config.m_PlayerName[0])
	{
		pBase = g_Config.m_PlayerName;
	}
	else if(g_Config.m_SteamName[0])
	{
		pBase = g_Config.m_SteamName;
	}
	if(pBase)
	{
		str_format(m_aAutomaticDummyName, sizeof(m_aAutomaticDummyName), "[D] %s", pBase);
		return m_aAutomaticDummyName;
	}
	return "brainless tee";
}

const char *CClientWithConnection::ErrorString() const
{
	return NetClient(CONN_MAIN).ErrorString();
}

static CServerCapabilities GetServerCapabilities(int Version, int Flags, bool Sixup)
{
	CServerCapabilities Result;
	bool DDNet = false;
	if(Version >= 1)
	{
		DDNet = Flags & SERVERCAPFLAG_DDNET;
	}
	Result.m_ChatTimeoutCode = DDNet;
	Result.m_AnyPlayerFlag = !Sixup;
	Result.m_PingEx = false;
	Result.m_AllowDummy = true;
	Result.m_SyncWeaponInput = false;
	if(Version >= 1)
	{
		Result.m_ChatTimeoutCode = Flags & SERVERCAPFLAG_CHATTIMEOUTCODE;
	}
	if(Version >= 2)
	{
		Result.m_AnyPlayerFlag = Flags & SERVERCAPFLAG_ANYPLAYERFLAG;
	}
	if(Version >= 3)
	{
		Result.m_PingEx = Flags & SERVERCAPFLAG_PINGEX;
	}
	if(Version >= 4)
	{
		Result.m_AllowDummy = Flags & SERVERCAPFLAG_ALLOWDUMMY;
	}
	if(Version >= 5)
	{
		Result.m_SyncWeaponInput = Flags & SERVERCAPFLAG_SYNCWEAPONINPUT;
	}
	return Result;
}

void CClientWithConnection::ProcessServerPacket(CSessionId SessionId, CStreamId StreamId, CNetChunk *pPacket)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	CConnection &GameConnection = Connection(SessionId, StreamId);
	const int Conn = Source.StreamIndex(StreamId);
	dbg_assert(Conn >= 0, "missing packet stream index");
	const bool PrimaryStream = StreamId == Source.PrimaryStreamId();
	const bool InactiveStream = StreamId != Source.ActiveStreamId();
	CUnpacker Unpacker;
	Unpacker.Reset(pPacket->m_pData, pPacket->m_DataSize);
	CMsgPacker Packer(NETMSG_EX, true);
	const bool Vital = (pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0;

	// unpack msgid and system flag
	int Msg;
	bool Sys;
	CUuid Uuid;

	int Result = UnpackMessageId(&Msg, &Sys, &Uuid, &Unpacker, &Packer);
	if(Result == UNPACKMESSAGE_ERROR)
	{
		return;
	}
	else if(Result == UNPACKMESSAGE_ANSWER)
	{
		SendMsg(SessionId, StreamId, &Packer, MSGFLAG_VITAL);
	}

	// allocates the memory for the translated data
	CPacker Packer6;
	if(IsSixup(SessionId))
	{
		bool IsExMsg = false;
		int Success = !TranslateSysMsg(SessionId, &Msg, Sys, &Unpacker, &Packer6, &pPacket->m_Address, &IsExMsg);
		if(Msg < 0)
			return;
		if(Success && !IsExMsg)
		{
			Unpacker.Reset(Packer6.Data(), Packer6.Size());
		}
	}

	if(Sys)
	{
		// system message
		if(PrimaryStream && Vital && Msg == NETMSG_MAP_DETAILS)
		{
			const char *pMap = Unpacker.GetString(CUnpacker::SANITIZE_CC | CUnpacker::SKIP_START_WHITESPACES);
			SHA256_DIGEST *pMapSha256 = (SHA256_DIGEST *)Unpacker.GetRaw(sizeof(*pMapSha256));
			int MapCrc = Unpacker.GetInt();
			int MapSize = Unpacker.GetInt();
			if(Unpacker.Error())
			{
				return;
			}

			const char *pMapUrl = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(Unpacker.Error())
			{
				pMapUrl = "";
			}

			Source.m_MapDetails = CNetworkSessionSource::CMapDetails{};
			CNetworkSessionSource::CMapDetails &MapDetails = Source.m_MapDetails.value();
			str_copy(MapDetails.m_aName, pMap);
			MapDetails.m_Size = MapSize;
			MapDetails.m_Crc = MapCrc;
			MapDetails.m_Sha256 = *pMapSha256;
			str_copy(MapDetails.m_aUrl, pMapUrl);
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_CAPABILITIES)
		{
			if(!Source.m_CanReceiveServerCapabilities)
			{
				return;
			}
			int Version = Unpacker.GetInt();
			int Flags = Unpacker.GetInt();
			if(Unpacker.Error() || Version <= 0)
			{
				return;
			}
			Source.m_ServerCapabilities = GetServerCapabilities(Version, Flags, IsSixup(SessionId));
			Source.m_CanReceiveServerCapabilities = false;
			Source.m_ServerSentCapabilities = true;
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_MAP_CHANGE)
		{
			if(Source.m_CanReceiveServerCapabilities)
			{
				Source.m_ServerCapabilities = GetServerCapabilities(0, 0, IsSixup(SessionId));
				Source.m_CanReceiveServerCapabilities = false;
			}
			std::optional<CNetworkSessionSource::CMapDetails> MapDetails = std::nullopt;
			std::swap(MapDetails, Source.m_MapDetails);

			const char *pMap = Unpacker.GetString(CUnpacker::SANITIZE_CC | CUnpacker::SKIP_START_WHITESPACES);
			int MapCrc = Unpacker.GetInt();
			int MapSize = Unpacker.GetInt();
			if(Unpacker.Error())
			{
				return;
			}
			if(MapSize < 0 || MapSize > 1024 * 1024 * 1024) // 1 GiB
			{
				m_SessionManager.Close(SessionId, "invalid map size");
				return;
			}

			if(!str_valid_filename(pMap))
			{
				m_SessionManager.Close(SessionId, "map name is not a valid filename");
				return;
			}

			if(SessionId == m_NetworkSessionId && m_DummyConnected && !m_DummyReconnectOnReload)
			{
				DummyDisconnect(nullptr);
			}

			ResetMapDownload(SessionId, true);

			std::optional<SHA256_DIGEST> MapSha256;
			const char *pMapUrl = nullptr;
			if(MapDetails.has_value() &&
				str_comp(MapDetails->m_aName, pMap) == 0 &&
				MapDetails->m_Size == MapSize &&
				MapDetails->m_Crc == MapCrc)
			{
				MapSha256 = MapDetails->m_Sha256;
				pMapUrl = MapDetails->m_aUrl[0] ? MapDetails->m_aUrl : nullptr;
			}

			if(LoadMapSearch(SessionId, pMap, MapSha256, MapCrc) == nullptr)
			{
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client/network", "loading done");
				if(m_SessionManager.FocusedId() == SessionId)
					SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_SENDING_READY);
				SendReady(SessionId, Source.PrimaryStreamId());
			}
			else
			{
				// start map download
				FormatMapDownloadFilename(pMap, MapSha256, MapCrc, false, Source.m_aMapdownloadFilename, sizeof(Source.m_aMapdownloadFilename));
				FormatMapDownloadFilename(pMap, MapSha256, MapCrc, true, Source.m_aMapdownloadFilenameTemp, sizeof(Source.m_aMapdownloadFilenameTemp));
				if(SessionId != m_NetworkSessionId)
				{
					char aBase[sizeof(Source.m_aMapdownloadFilenameTemp)];
					char aSuffix[32];
					str_copy(aBase, Source.m_aMapdownloadFilenameTemp);
					str_format(aSuffix, sizeof(aSuffix), ".session%" PRIu64, SessionId.Value());
					str_truncate(Source.m_aMapdownloadFilenameTemp, sizeof(Source.m_aMapdownloadFilenameTemp), aBase, sizeof(Source.m_aMapdownloadFilenameTemp) - str_length(aSuffix) - 1);
					str_append(Source.m_aMapdownloadFilenameTemp, aSuffix);
				}

				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "starting to download map to '%s'", Source.m_aMapdownloadFilenameTemp);
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client/network", aBuf);

				str_copy(Source.m_aMapdownloadName, pMap);
				Source.m_MapdownloadSha256 = MapSha256;
				Source.m_MapdownloadCrc = MapCrc;
				Source.m_MapdownloadTotalsize = MapSize;

				if(MapSha256.has_value())
				{
					char aUrl[256];
					char aEscaped[256];
					str_url_encode(aEscaped, str_startswith(Source.m_aMapdownloadFilename, "downloadedmaps/"));
					bool UseConfigUrl = str_comp(g_Config.m_ClMapDownloadUrl, "https://maps.ddnet.org") != 0 || MapDownloadUrl()[0] == '\0';
					str_format(aUrl, sizeof(aUrl), "%s/%s", UseConfigUrl ? g_Config.m_ClMapDownloadUrl : MapDownloadUrl(), aEscaped);

					Source.m_pMapdownloadTask = Http()->CreateGetFile(pMapUrl ? pMapUrl : aUrl, Storage(), Source.m_aMapdownloadFilenameTemp, IStorage::TYPE_SAVE);
					Source.m_pMapdownloadTask->Timeout(CTimeout{g_Config.m_ClMapDownloadConnectTimeoutMs, 0, g_Config.m_ClMapDownloadLowSpeedLimit, g_Config.m_ClMapDownloadLowSpeedTime});
					Source.m_pMapdownloadTask->MaxResponseSize(MapSize);
					Source.m_pMapdownloadTask->ExpectSha256(MapSha256.value());
					Http()->Run(Source.m_pMapdownloadTask);
				}
				else
				{
					SendMapRequest(SessionId);
				}
			}
		}
		else if(PrimaryStream && Msg == NETMSG_MAP_DATA)
		{
			if(!Source.m_MapdownloadFileTemp)
			{
				return;
			}
			int Last = -1;
			int MapCRC = -1;
			int Chunk = -1;
			int Size = -1;
			CTranslationContext &TranslationContext = Source.TranslationContext();

			if(IsSixup(SessionId))
			{
				if(TranslationContext.m_MapdownloadTotalsize <= 0 ||
					TranslationContext.m_MapDownloadChunkSize <= 0 ||
					TranslationContext.m_MapDownloadChunksPerRequest <= 0)
				{
					return;
				}
				MapCRC = Source.m_MapdownloadCrc;
				Chunk = Source.m_MapdownloadChunk;
				Size = std::min(TranslationContext.m_MapDownloadChunkSize, TranslationContext.m_MapdownloadTotalsize - Source.m_MapdownloadAmount);
			}
			else
			{
				Last = Unpacker.GetInt();
				MapCRC = Unpacker.GetInt();
				Chunk = Unpacker.GetInt();
				Size = Unpacker.GetInt();
			}

			const unsigned char *pData = Unpacker.GetRaw(Size);
			if(Unpacker.Error() || Size <= 0 || MapCRC != Source.m_MapdownloadCrc || Chunk != Source.m_MapdownloadChunk)
			{
				return;
			}

			io_write(Source.m_MapdownloadFileTemp, pData, Size);

			Source.m_MapdownloadAmount += Size;

			if(IsSixup(SessionId))
				Last = Source.m_MapdownloadAmount == TranslationContext.m_MapdownloadTotalsize;

			if(Last)
			{
				if(Source.m_MapdownloadFileTemp)
				{
					io_close(Source.m_MapdownloadFileTemp);
					Source.m_MapdownloadFileTemp = nullptr;
				}
				FinishMapDownload(SessionId);
			}
			else
			{
				// request new chunk
				Source.m_MapdownloadChunk++;

				if(IsSixup(SessionId) && (Source.m_MapdownloadChunk % TranslationContext.m_MapDownloadChunksPerRequest == 0))
				{
					CMsgPacker MsgP(protocol7::NETMSG_REQUEST_MAP_DATA, true, true);
					SendMsg(SessionId, Source.PrimaryStreamId(), &MsgP, MSGFLAG_VITAL | MSGFLAG_FLUSH);
				}
				else
				{
					CMsgPacker MsgP(NETMSG_REQUEST_MAP_DATA, true);
					MsgP.AddInt(Source.m_MapdownloadChunk);
					SendMsg(SessionId, Source.PrimaryStreamId(), &MsgP, MSGFLAG_VITAL | MSGFLAG_FLUSH);
				}

				if(g_Config.m_Debug)
				{
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "requested chunk %d", Source.m_MapdownloadChunk);
					m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client/network", aBuf);
				}
			}
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_MAP_RELOAD)
		{
			if(SessionId == m_NetworkSessionId && m_DummyConnected)
			{
				m_DummyReconnectOnReload = true;
				m_DummyDeactivateOnReconnect = ActiveConnection() == CONN_MAIN;
				g_Config.m_ClDummy = 0;
				SetActiveConnection(CONN_MAIN);
			}
			else if(SessionId == m_NetworkSessionId)
			{
				m_DummyDeactivateOnReconnect = false;
			}
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_CON_READY)
		{
			if(!GameClient()->Map(SessionId)->IsLoaded())
			{
				return;
			}
			GameClient()->OnConnected(SessionId);
			if(SessionId == m_NetworkSessionId && m_DummyReconnectOnReload)
			{
				m_DummySendConnInfo = true;
				m_DummyReconnectOnReload = false;
			}
		}
		else if(SessionId == m_NetworkSessionId && Conn == CONN_DUMMY && Msg == NETMSG_CON_READY)
		{
			m_DummyConnected = true;
			m_DummyConnecting = false;
			g_Config.m_ClDummy = 1;
			SetActiveConnection(CONN_DUMMY);
			Rcon("crashmeplx");
			if(Connection(CONN_MAIN).m_RconAuthed && !Connection(CONN_DUMMY).m_RconAuthed)
				RconAuth(ActiveConnection(), m_aRconUsername, m_aRconPassword);
		}
		else if(Msg == NETMSG_PING)
		{
			CMsgPacker MsgP(NETMSG_PING_REPLY, true);
			SendMsg(SessionId, StreamId, &MsgP, (Vital ? MSGFLAG_VITAL : 0) | MSGFLAG_FLUSH);
		}
		else if(Msg == NETMSG_PINGEX)
		{
			CUuid *pId = (CUuid *)Unpacker.GetRaw(sizeof(*pId));
			if(Unpacker.Error())
			{
				return;
			}
			CMsgPacker MsgP(NETMSG_PONGEX, true);
			MsgP.AddRaw(pId, sizeof(*pId));
			SendMsg(SessionId, StreamId, &MsgP, (Vital ? MSGFLAG_VITAL : 0) | MSGFLAG_FLUSH);
		}
		else if(PrimaryStream && Msg == NETMSG_PONGEX)
		{
			CUuid *pId = (CUuid *)Unpacker.GetRaw(sizeof(*pId));
			if(Unpacker.Error())
			{
				return;
			}
			if(Source.m_ServerCapabilities.m_PingEx && Source.m_CurrentPingTime >= 0 && *pId == Source.m_PingUuid)
			{
				int LatencyMs = (time_get() - Source.m_CurrentPingTime) * 1000 / time_freq();
				OnCurrentServerPing(SessionServerAddress(SessionId), LatencyMs);
				Source.m_CurrentPingTime = -1;

				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "got pong from current server, latency=%dms", LatencyMs);
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf);
			}
		}
		else if(Msg == NETMSG_CHECKSUM_REQUEST)
		{
			CUuid *pUuid = (CUuid *)Unpacker.GetRaw(sizeof(*pUuid));
			if(Unpacker.Error())
			{
				return;
			}
			int ResultCheck = HandleChecksum(SessionId, StreamId, *pUuid, &Unpacker);
			if(ResultCheck)
			{
				CMsgPacker MsgP(NETMSG_CHECKSUM_ERROR, true);
				MsgP.AddRaw(pUuid, sizeof(*pUuid));
				MsgP.AddInt(ResultCheck);
				SendMsg(SessionId, StreamId, &MsgP, MSGFLAG_VITAL);
			}
		}
		else if(Msg == NETMSG_RECONNECT)
		{
			if(PrimaryStream)
			{
				const std::string ConnectAddress = Source.m_ConnectAddress;
				const std::string Password = Source.m_SendPassword ? g_Config.m_Password : Source.m_Password;
				ConnectSession(SessionId, ConnectAddress.c_str(), Password.c_str());
			}
			else if(SessionId == m_NetworkSessionId)
			{
				DummyDisconnect("reconnect");
				// Reset dummy connect time to allow immediate reconnect
				m_LastDummyConnectTime = 0.0f;
				DummyConnect();
			}
		}
		else if(Msg == NETMSG_REDIRECT)
		{
			int RedirectPort = Unpacker.GetInt();
			if(Unpacker.Error())
			{
				return;
			}
			if(PrimaryStream)
			{
				NETADDR ServerAddr = SessionServerAddress(SessionId);
				ServerAddr.port = RedirectPort;
				char aAddr[NETADDR_MAXSTRSIZE];
				net_addr_str(&ServerAddr, aAddr, sizeof(aAddr), true);
				const std::string Password = Source.m_SendPassword ? g_Config.m_Password : Source.m_Password;
				ConnectSession(SessionId, aAddr, Password.c_str());
			}
			else if(SessionId == m_NetworkSessionId)
			{
				DummyDisconnect("redirect");
				if(ServerAddress().port != RedirectPort)
				{
					// Only allow redirecting to the same port to reconnect. The dummy
					// should not be connected to a different server than the main, as
					// the client assumes that main and dummy use the same map.
					return;
				}
				// Reset dummy connect time to allow immediate reconnect
				m_LastDummyConnectTime = 0.0f;
				DummyConnect();
			}
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_RCON_CMD_ADD)
		{
			const char *pName = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			const char *pHelp = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			const char *pParams = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(!Unpacker.Error() && SessionId == m_NetworkSessionId)
			{
				m_pConsole->RegisterTemp(pName, pParams, CFGFLAG_SERVER, pHelp);
				GameClient()->ForceUpdateConsoleRemoteCompletionSuggestions();
			}
			Source.m_GotRconCommands++;
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_RCON_CMD_REM)
		{
			const char *pName = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(!Unpacker.Error() && SessionId == m_NetworkSessionId)
			{
				m_pConsole->DeregisterTemp(pName);
				GameClient()->ForceUpdateConsoleRemoteCompletionSuggestions();
			}
		}
		else if(Vital && Msg == NETMSG_RCON_AUTH_STATUS)
		{
			int ResultInt = Unpacker.GetInt();
			if(!Unpacker.Error())
			{
				GameConnection.m_RconAuthed = ResultInt;

				if(SessionId == m_NetworkSessionId && GameConnection.m_RconAuthed)
					RconAuth(ActiveConnection() == CONN_MAIN ? CONN_DUMMY : CONN_MAIN, m_aRconUsername, m_aRconPassword);
			}
			if(PrimaryStream)
			{
				const int Old = Source.m_UseTempRconCommands;
				Source.m_UseTempRconCommands = Unpacker.GetInt();
				if(Unpacker.Error())
				{
					Source.m_UseTempRconCommands = 0;
				}
				if(Old != 0 && Source.m_UseTempRconCommands == 0)
				{
					Source.m_ExpectedRconCommands = -1;
					Source.m_vMaplistEntries.clear();
					Source.m_ExpectedMaplistEntries = -1;
					if(SessionId == m_NetworkSessionId)
					{
						m_pConsole->DeregisterTempAll();
						GameClient()->ForceUpdateConsoleRemoteCompletionSuggestions();
					}
				}
			}
		}
		else if(!InactiveStream && Vital && Msg == NETMSG_RCON_LINE)
		{
			const char *pLine = Unpacker.GetString();
			if(!Unpacker.Error() && SessionId == m_NetworkSessionId)
			{
				GameClient()->OnRconLine(pLine);
			}
		}
		else if(SessionId == m_NetworkSessionId && PrimaryStream && Msg == NETMSG_PING_REPLY)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "latency %.2f", (time_get() - m_PingStartTime) * 1000 / (float)time_freq());
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/network", aBuf);
		}
		else if(Msg == NETMSG_INPUTTIMING)
		{
			int InputPredTick = Unpacker.GetInt();
			int TimeLeft = Unpacker.GetInt();
			if(Unpacker.Error())
			{
				return;
			}

			int64_t Now = time_get();

			// adjust our prediction time
			int64_t Target = 0;
			for(const auto &Input : GameConnection.m_aInputs)
			{
				if(Input.m_Tick == InputPredTick)
				{
					Target = Input.m_PredictedTime + (Now - Input.m_Time);
					Target = Target - (int64_t)((TimeLeft / 1000.0f) * time_freq());
					break;
				}
			}

			if(Target)
				GameConnection.m_PredictedTime.Update(&GameConnection.m_InputtimeMarginGraph, Target, TimeLeft, CSmoothTime::ADJUSTDIRECTION_UP);
		}
		else if(Msg == NETMSG_SNAP || Msg == NETMSG_SNAPSINGLE || Msg == NETMSG_SNAPEMPTY)
		{
			// We are not allowed to process snapshots yet.
			if(Source.State() != ESessionState::LOADING_MAP && Source.State() != ESessionState::READY)
			{
				return;
			}
			if(!GameClient()->Map(SessionId)->IsLoaded())
			{
				return;
			}

			int GameTick = Unpacker.GetInt();
			int DeltaTick = GameTick - Unpacker.GetInt();

			int NumParts = 1;
			int Part = 0;
			if(Msg == NETMSG_SNAP)
			{
				NumParts = Unpacker.GetInt();
				Part = Unpacker.GetInt();
			}

			unsigned int Crc = 0;
			int PartSize = 0;
			if(Msg != NETMSG_SNAPEMPTY)
			{
				Crc = Unpacker.GetInt();
				PartSize = Unpacker.GetInt();
			}

			const char *pData = (const char *)Unpacker.GetRaw(PartSize);
			if(Unpacker.Error() || NumParts < 1 || NumParts > CSnapshot::MAX_PARTS || Part < 0 || Part >= NumParts || PartSize < 0 || PartSize > MAX_SNAPSHOT_PACKSIZE)
			{
				return;
			}

			// Check m_aAckGameTick to see if we already got a snapshot for that tick
			if(GameTick >= GameConnection.m_CurrentRecvTick && GameTick > GameConnection.m_AckGameTick)
			{
				if(GameTick != GameConnection.m_CurrentRecvTick)
				{
					GameConnection.m_SnapshotParts = 0;
					GameConnection.m_CurrentRecvTick = GameTick;
					GameConnection.m_SnapshotIncomingDataSize = 0;
				}

				mem_copy(GameConnection.m_aSnapshotIncomingData + Part * MAX_SNAPSHOT_PACKSIZE, pData, std::clamp(PartSize, 0, (int)sizeof(GameConnection.m_aSnapshotIncomingData) - Part * MAX_SNAPSHOT_PACKSIZE));
				GameConnection.m_SnapshotParts |= (uint64_t)(1) << Part;

				if(Part == NumParts - 1)
				{
					GameConnection.m_SnapshotIncomingDataSize = (NumParts - 1) * MAX_SNAPSHOT_PACKSIZE + PartSize;
				}

				if((NumParts < CSnapshot::MAX_PARTS && GameConnection.m_SnapshotParts == (((uint64_t)(1) << NumParts) - 1)) ||
					(NumParts == CSnapshot::MAX_PARTS && GameConnection.m_SnapshotParts == std::numeric_limits<uint64_t>::max()))
				{
					unsigned char aTmpBuffer2[CSnapshot::MAX_SIZE];
					CSnapshotBuffer TmpBuffer3;

					// reset snapshotting
					GameConnection.m_SnapshotParts = 0;

					// find snapshot that we should use as delta
					const CSnapshot *pDeltaShot = CSnapshot::EmptySnapshot();
					if(DeltaTick >= 0)
					{
						int DeltashotSize = GameConnection.m_SnapshotStorage.Get(DeltaTick, nullptr, &pDeltaShot, nullptr);

						if(DeltashotSize < 0)
						{
							// couldn't find the delta snapshots that the server used
							// to compress this snapshot. force the server to resync
							if(g_Config.m_Debug)
							{
								m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client", "error, couldn't find the delta snapshot");
							}

							// ack snapshot
							GameConnection.m_AckGameTick = -1;
							SendInput(SessionId);
							return;
						}
					}

					// decompress snapshot
					const void *pDeltaData = Source.SnapshotDelta(IsSixup(SessionId)).EmptyDelta();
					int DeltaSize = sizeof(int) * 3;

					if(GameConnection.m_SnapshotIncomingDataSize)
					{
						int IntSize = CVariableInt::Decompress(GameConnection.m_aSnapshotIncomingData, GameConnection.m_SnapshotIncomingDataSize, aTmpBuffer2, sizeof(aTmpBuffer2));

						if(IntSize < 0) // failure during decompression
							return;

						pDeltaData = aTmpBuffer2;
						DeltaSize = IntSize;
					}

					// unpack delta
					const int SnapSize = Source.SnapshotDelta(IsSixup(SessionId)).UnpackDelta(pDeltaShot, &TmpBuffer3, pDeltaData, DeltaSize);
					if(SnapSize < 0)
					{
						dbg_msg("client", "delta unpack failed. error=%d", SnapSize);
						return;
					}
					if(!TmpBuffer3.AsSnapshot()->IsValid(SnapSize))
					{
						dbg_msg("client", "snapshot invalid. SnapSize=%d, DeltaSize=%d", SnapSize, DeltaSize);
						return;
					}

					if(Msg != NETMSG_SNAPEMPTY && TmpBuffer3.AsSnapshot()->Crc() != Crc)
					{
						log_error("client", "snapshot crc error #%d - tick=%d wantedcrc=%d gotcrc=%d compressed_size=%d delta_tick=%d",
							GameConnection.m_SnapCrcErrors, GameTick, Crc, TmpBuffer3.AsSnapshot()->Crc(), GameConnection.m_SnapshotIncomingDataSize, DeltaTick);

						GameConnection.m_SnapCrcErrors++;
						if(GameConnection.m_SnapCrcErrors > 10)
						{
							// to many errors, send reset
							GameConnection.m_AckGameTick = -1;
							SendInput(SessionId);
							GameConnection.m_SnapCrcErrors = 0;
						}
						return;
					}
					else
					{
						if(GameConnection.m_SnapCrcErrors)
							GameConnection.m_SnapCrcErrors--;
					}

					// purge old snapshots
					int PurgeTick = DeltaTick;
					if(GameConnection.m_apSnapshots[SNAP_PREV] && GameConnection.m_apSnapshots[SNAP_PREV]->m_Tick < PurgeTick)
						PurgeTick = GameConnection.m_apSnapshots[SNAP_PREV]->m_Tick;
					if(GameConnection.m_apSnapshots[SNAP_CURRENT] && GameConnection.m_apSnapshots[SNAP_CURRENT]->m_Tick < PurgeTick)
						PurgeTick = GameConnection.m_apSnapshots[SNAP_CURRENT]->m_Tick;
					GameConnection.m_SnapshotStorage.PurgeUntil(PurgeTick);

					// create a verified and unpacked snapshot
					int AltSnapSize = -1;
					CSnapshotBuffer AltSnapBuffer;

					if(IsSixup(SessionId))
					{
						CSnapshotBuffer TmpTransSnapBuffer;
						mem_copy(&TmpTransSnapBuffer, &TmpBuffer3, sizeof(TmpTransSnapBuffer));
						AltSnapSize = GameClient()->TranslateSnap(SessionId, &AltSnapBuffer, TmpTransSnapBuffer.AsSnapshot(), StreamId);
					}
					else
					{
						AltSnapSize = UnpackAndValidateSnapshot(TmpBuffer3.AsSnapshot(), &AltSnapBuffer);
					}

					if(AltSnapSize < 0)
					{
						dbg_msg("client", "unpack snapshot and validate failed. error=%d", AltSnapSize);
						return;
					}

					// add new
					GameConnection.m_SnapshotStorage.Add(GameTick, time_get(), SnapSize, TmpBuffer3.AsSnapshot(), AltSnapSize, AltSnapBuffer.AsSnapshot());

					if(SessionId == m_NetworkSessionId && !InactiveStream)
					{
						GameClient()->ProcessDemoSnapshot(TmpBuffer3.AsSnapshot());

						CSnapshotBuffer SnapSeven;
						int DemoSnapSize = SnapSize;
						if(IsSixup(SessionId))
						{
							DemoSnapSize = GameClient()->OnDemoRecSnap7(SessionId, TmpBuffer3.AsSnapshot(), &SnapSeven, StreamId);
							if(DemoSnapSize < 0)
							{
								dbg_msg("sixup", "demo snapshot failed. error=%d", DemoSnapSize);
							}
						}

						if(DemoSnapSize >= 0)
						{
							// add snapshot to demo
							RecordSnapshot(GameTick, IsSixup(SessionId) ? SnapSeven.AsSnapshot() : TmpBuffer3.AsSnapshot(), DemoSnapSize);
						}
					}

					// apply snapshot, cycle pointers
					GameConnection.m_ReceivedSnapshots++;

					// we got two snapshots until we see us self as connected
					if(GameConnection.m_ReceivedSnapshots == 2)
					{
						// start at 200ms and work from there
						GameConnection.m_PredictedTime.Init(GameTick * time_freq() / GameTickSpeed());
						GameConnection.m_PredictedTime.SetAdjustSpeed(CSmoothTime::ADJUSTDIRECTION_UP, 1000.0f);
						GameConnection.m_PredictedTime.UpdateMargin(PredictionMargin(SessionId) * time_freq() / 1000);
						GameConnection.m_GameTime.Init((GameTick - 1) * time_freq() / GameTickSpeed());
						GameConnection.m_apSnapshots[SNAP_PREV] = GameConnection.m_SnapshotStorage.m_pFirst;
						GameConnection.m_apSnapshots[SNAP_CURRENT] = GameConnection.m_SnapshotStorage.m_pLast;
						GameConnection.m_PrevGameTick = GameConnection.m_apSnapshots[SNAP_PREV]->m_Tick;
						GameConnection.m_CurGameTick = GameConnection.m_apSnapshots[SNAP_CURRENT]->m_Tick;
						if(SessionId == m_NetworkSessionId && PrimaryStream)
						{
							m_LocalStartTime = time_get();
						}
						GameClient()->OnNewSnapshot(SessionId, StreamId);
						if(m_SessionManager.FocusedId() == SessionId)
							SetFocusedState(IClient::STATE_ONLINE, false);
						else
							Source.SetState(ESessionState::READY);
						if(SessionId == m_NetworkSessionId && PrimaryStream)
						{
							DemoRecorder_HandleAutoStart();
						}
					}

					// adjust game time
					if(GameConnection.m_ReceivedSnapshots > 2)
					{
						int64_t Now = GameConnection.m_GameTime.Get(time_get());
						int64_t TickStart = GameTick * time_freq() / GameTickSpeed();
						int64_t TimeLeft = (TickStart - Now) * 1000 / time_freq();
						GameConnection.m_GameTime.Update(&GameConnection.m_GametimeMarginGraph, (GameTick - 1) * time_freq() / GameTickSpeed(), TimeLeft, CSmoothTime::ADJUSTDIRECTION_DOWN);
					}

					if(GameConnection.m_ReceivedSnapshots > GameTickSpeed() && !GameConnection.m_DidPostConnect)
					{
						if(SessionId == m_NetworkSessionId)
							OnPostConnect(Conn);
						GameConnection.m_DidPostConnect = true;
					}

					// ack snapshot
					GameConnection.m_AckGameTick = GameTick;
				}
			}
		}
		else if(PrimaryStream && Msg == NETMSG_RCONTYPE)
		{
			const bool UsernameReq = (Unpacker.GetInt() & 1) != 0;
			if(!Unpacker.Error() && SessionId == m_NetworkSessionId)
			{
				GameClient()->OnRconType(UsernameReq);
			}
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_RCON_CMD_GROUP_START)
		{
			const int ExpectedRconCommands = Unpacker.GetInt();
			if(Unpacker.Error() || ExpectedRconCommands < 0)
				return;

			Source.m_ExpectedRconCommands = ExpectedRconCommands;
			Source.m_GotRconCommands = 0;
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_RCON_CMD_GROUP_END)
		{
			Source.m_ExpectedRconCommands = -1;
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_MAPLIST_ADD)
		{
			while(true)
			{
				const char *pMapName = Unpacker.GetString(CUnpacker::SANITIZE_CC | CUnpacker::SKIP_START_WHITESPACES);
				if(Unpacker.Error())
				{
					return;
				}
				if(pMapName[0] != '\0')
				{
					Source.m_vMaplistEntries.emplace_back(pMapName);
					if(SessionId == m_NetworkSessionId)
						GameClient()->ForceUpdateConsoleRemoteCompletionSuggestions();
				}
			}
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_MAPLIST_GROUP_START)
		{
			const int ExpectedMaplistEntries = Unpacker.GetInt();
			if(Unpacker.Error() || ExpectedMaplistEntries < 0)
				return;

			Source.m_vMaplistEntries.clear();
			Source.m_ExpectedMaplistEntries = ExpectedMaplistEntries;
			if(SessionId == m_NetworkSessionId)
				GameClient()->ForceUpdateConsoleRemoteCompletionSuggestions();
		}
		else if(PrimaryStream && Vital && Msg == NETMSG_MAPLIST_GROUP_END)
		{
			Source.m_ExpectedMaplistEntries = -1;
		}
	}
	// the client handles only vital messages https://github.com/ddnet/ddnet/issues/11178
	else if(Vital || Msg == NETMSGTYPE_SV_PREINPUT)
	{
		// game message
		if(SessionId == m_NetworkSessionId && !InactiveStream && Msg != NETMSG_MATCH_REPORT_LOCAL_PARTICIPANT)
		{
			RecordMessage(pPacket->m_pData, pPacket->m_DataSize);
		}

		GameClient()->OnMessage(SessionId, Msg, &Unpacker, StreamId);
	}
}

void CClientWithConnection::ResetMapDownload(CSessionId SessionId, bool ResetActive)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	if(Source.m_pMapdownloadTask)
	{
		Source.m_pMapdownloadTask->Abort();
		Source.m_pMapdownloadTask = nullptr;
	}

	if(Source.m_MapdownloadFileTemp)
	{
		io_close(Source.m_MapdownloadFileTemp);
		Source.m_MapdownloadFileTemp = nullptr;
	}

	if(Storage()->FileExists(Source.m_aMapdownloadFilenameTemp, IStorage::TYPE_SAVE))
	{
		Storage()->RemoveFile(Source.m_aMapdownloadFilenameTemp, IStorage::TYPE_SAVE);
	}

	if(ResetActive)
	{
		Source.m_MapdownloadChunk = 0;
		Source.m_MapdownloadSha256 = std::nullopt;
		Source.m_MapdownloadCrc = 0;
		Source.m_MapdownloadTotalsize = -1;
		Source.m_MapdownloadAmount = 0;
		Source.m_aMapdownloadFilename[0] = '\0';
		Source.m_aMapdownloadFilenameTemp[0] = '\0';
		Source.m_aMapdownloadName[0] = '\0';
	}
}

void CClientWithConnection::FinishMapDownload(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client/network", "download complete, loading map");

	if(!Storage()->RenameFile(Source.m_aMapdownloadFilenameTemp, Source.m_aMapdownloadFilename, IStorage::TYPE_SAVE))
	{
		char aError[128 + IO_MAX_PATH_LENGTH];
		str_format(aError, sizeof(aError), Localize("Could not save downloaded map. Try manually deleting this file: %s"), Source.m_aMapdownloadFilename);
		m_SessionManager.Close(SessionId, aError);
		return;
	}

	const char *pError = LoadMap(SessionId, Source.m_aMapdownloadName, Source.m_aMapdownloadFilename, Source.m_MapdownloadSha256, Source.m_MapdownloadCrc);
	if(!pError)
	{
		ResetMapDownload(SessionId, true);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client/network", "loading done");
		SendReady(SessionId, Source.PrimaryStreamId());
	}
	else if(Source.m_pMapdownloadTask) // fallback
	{
		ResetMapDownload(SessionId, false);
		SendMapRequest(SessionId);
	}
	else
	{
		m_SessionManager.Close(SessionId, pError);
	}
}

int CClientWithConnection::ConnectNetTypes() const
{
	if(m_UseQuic)
		return m_QuicServerAddress.type;
	const NETADDR *pConnectAddrs;
	int NumConnectAddrs;
	NetClient(CONN_MAIN).ConnectAddresses(&pConnectAddrs, &NumConnectAddrs);
	int NetType = 0;
	for(int i = 0; i < NumConnectAddrs; i++)
	{
		NetType |= pConnectAddrs[i].type;
	}
	return NetType;
}

bool CClientWithConnection::RconAuthed() const
{
	return Connection(ActiveConnection()).m_RconAuthed != 0;
}

const NETADDR &CClientWithConnection::ServerAddress() const
{
	return SessionServerAddress(m_NetworkSessionId);
}

const NETADDR &CClientWithConnection::SessionServerAddress(CSessionId SessionId) const
{
	return SessionId == m_NetworkSessionId && m_UseQuic ? m_QuicServerAddress : *NetworkSource(SessionId).PrimaryNetClient().ServerAddress();
}

void CClientWithConnection::PumpNetwork(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	CNetClient &PrimaryNetClient = Source.PrimaryNetClient();
	const bool QuicSession = SessionId == m_NetworkSessionId && m_UseQuic;
	for(const auto &pStream : Source.Streams())
	{
		pStream->m_NetClient.Update();
	}
	if(SessionId == m_NetworkSessionId)
		m_ContactNetClient.Update();

	// check for errors of main and dummy
	if(Source.State() != ESessionState::OFFLINE && m_State < IClient::STATE_QUITTING)
	{
		if(!QuicSession && PrimaryNetClient.State() == NETSTATE_OFFLINE)
		{
			m_SessionManager.Close(SessionId);
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "offline error='%s'", PrimaryNetClient.ErrorString());
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf, CLIENT_NETWORK_PRINT_ERROR_COLOR);
		}
		else if(SessionId == m_NetworkSessionId && (DummyConnecting() || DummyConnected()) && NetClient(CONN_DUMMY).State() == NETSTATE_OFFLINE)
		{
			const bool WasConnecting = DummyConnecting();
			DummyDisconnect(nullptr);
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "offline dummy error='%s'", NetClient(CONN_DUMMY).ErrorString());
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf, CLIENT_NETWORK_PRINT_ERROR_COLOR);
			if(WasConnecting)
			{
				str_format(aBuf, sizeof(aBuf), "%s: %s", Localize("Could not connect dummy"), NetClient(CONN_DUMMY).ErrorString());
				GameClient()->Echo(aBuf);
			}
		}
		for(const auto &pStream : Source.Streams())
		{
			if(pStream->m_Id == Source.PrimaryStreamId() || (SessionId == m_NetworkSessionId && pStream->m_Id == Source.StreamIdAt(CONN_DUMMY)) || pStream->m_NetClient.State() != NETSTATE_OFFLINE || pStream->m_NetClient.ErrorString()[0] == '\0')
				continue;
			const CStreamId StreamId = pStream->m_Id;
			char aError[256];
			str_copy(aError, pStream->m_NetClient.ErrorString());
			if(DestroyNetworkStream(SessionId, StreamId))
			{
				char aBuf[320];
				str_format(aBuf, sizeof(aBuf), "offline stream %" PRIu64 " error='%s'", StreamId.Value(), aError);
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf, CLIENT_NETWORK_PRINT_ERROR_COLOR);
			}
			break;
		}
	}
	if(SessionId == m_NetworkSessionId)
	{
		NETADDR QuicAddress;
		unsigned char *pQuicData;
		int QuicDataSize;
		while((QuicDataSize = m_QuicTransport.PollUdpSend(&QuicAddress, &pQuicData)) > 0)
			PrimaryNetClient.SendRaw(&QuicAddress, pQuicData, QuicDataSize);

		CQuicEvent QuicEvent;
		while(m_QuicTransport.Poll(QuicEvent))
		{
			if(QuicEvent.m_Type == EQuicEventType::CONNECTED && Source.State() == ESessionState::CONNECTING)
			{
				if(m_QuicIdentityRequired)
				{
					if(QuicEvent.m_Message.m_DataSize != SHA256_DIGEST_LENGTH)
					{
						DisconnectWithReason("QUIC server identity proof did not return a fingerprint");
						break;
					}
					SHA256_DIGEST IdentityFingerprint;
					mem_copy(IdentityFingerprint.data, QuicEvent.m_Message.m_pData, sizeof(IdentityFingerprint.data));
					if(m_QuicIdentityKnown && IdentityFingerprint != m_QuicExpectedIdentity)
					{
						DisconnectWithReason("QUIC server identity changed");
						break;
					}
					if(m_QuicRememberIdentity)
					{
						if(!AddQuicKnownHost(m_aQuicTrustHost, m_QuicTrustPort, IdentityFingerprint))
						{
							DisconnectWithReason("could not store QUIC server identity");
							break;
						}
						m_QuicExpectedIdentity = IdentityFingerprint;
						m_QuicIdentityKnown = true;
						m_QuicRememberIdentity = false;
						if(!m_pConfigManager->Save())
							log_warn("client", "could not persist trusted QUIC server identity");
					}
				}
				m_QuicSession = QuicEvent.m_Message.m_Session;
				m_QuicServerAddress = QuicEvent.m_Message.m_PeerAddress;
				m_QuicConnected = true;
				m_QuicLastRecvTime = time_get();
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", m_UseWebTransport ? "WebTransport connected, sending info" : "QUIC connected, sending info", CLIENT_NETWORK_PRINT_COLOR);
				if(m_SessionManager.FocusedId() == SessionId)
				{
					SetFocusedState(IClient::STATE_LOADING, true);
					SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_INITIAL);
				}
				else
				{
					GameClient()->OnSessionClosed(SessionId);
					Source.SetState(ESessionState::LOADING_MAP);
				}
				SendInfo(SessionId, Source.PrimaryStreamId());
			}
			else if(QuicEvent.m_Type == EQuicEventType::MESSAGE && QuicEvent.m_Message.m_Session == m_QuicSession)
			{
				m_QuicLastRecvTime = time_get();
				CNetChunk Packet = {};
				Packet.m_ClientId = 0;
				Packet.m_Address = QuicEvent.m_Message.m_PeerAddress;
				Packet.m_Flags = QuicEvent.m_Message.m_Vital ? NET_CHUNKFLAG_VITAL : 0;
				Packet.m_pData = QuicEvent.m_Message.m_pData;
				Packet.m_DataSize = QuicEvent.m_Message.m_DataSize;
				ProcessServerPacket(SessionId, Source.PrimaryStreamId(), &Packet);
			}
			else if(QuicEvent.m_Type == EQuicEventType::MAP_HEADER && QuicEvent.m_Message.m_Session == m_QuicSession)
			{
				GameWire::CMapHeaderView Header = {};
				const auto Result = GameWire::DecodeMapHeader(
					{static_cast<const unsigned char *>(QuicEvent.m_Message.m_pData), static_cast<size_t>(QuicEvent.m_Message.m_DataSize)},
					Header);
				const size_t NameLength = str_length(Source.m_aMapdownloadName);
				if(Result != GameWire::EDecodeResult::OK ||
					!Source.m_MapdownloadFileTemp ||
					Header.m_Size != static_cast<uint64_t>(Source.m_MapdownloadTotalsize) ||
					Header.m_Crc != static_cast<uint32_t>(Source.m_MapdownloadCrc) ||
					Header.m_Name.m_Size != NameLength ||
					mem_comp(Header.m_Name.m_pData, Source.m_aMapdownloadName, NameLength) != 0 ||
					(Source.m_MapdownloadSha256.has_value() && mem_comp(Header.m_aSha256, Source.m_MapdownloadSha256->data, sizeof(Header.m_aSha256)) != 0))
				{
					DisconnectWithReason("QUIC map header does not match MAP_CHANGE");
					break;
				}
				if(!Source.m_MapdownloadSha256.has_value())
				{
					SHA256_DIGEST Sha256;
					mem_copy(Sha256.data, Header.m_aSha256, sizeof(Sha256.data));
					Source.m_MapdownloadSha256 = Sha256;
				}
			}
			else if(QuicEvent.m_Type == EQuicEventType::MAP_DATA && QuicEvent.m_Message.m_Session == m_QuicSession)
			{
				const int Size = QuicEvent.m_Message.m_DataSize;
				if(!Source.m_MapdownloadFileTemp || Size <= 0 || Source.m_MapdownloadAmount < 0 || Source.m_MapdownloadAmount > Source.m_MapdownloadTotalsize || Size > Source.m_MapdownloadTotalsize - Source.m_MapdownloadAmount ||
					io_write(Source.m_MapdownloadFileTemp, QuicEvent.m_Message.m_pData, Size) != static_cast<unsigned>(Size))
				{
					DisconnectWithReason("could not write QUIC map stream");
					break;
				}
				Source.m_MapdownloadAmount += Size;
			}
			else if(QuicEvent.m_Type == EQuicEventType::MAP_END && QuicEvent.m_Message.m_Session == m_QuicSession)
			{
				if(!Source.m_MapdownloadFileTemp || Source.m_MapdownloadAmount != Source.m_MapdownloadTotalsize)
				{
					DisconnectWithReason("QUIC map stream ended at the wrong size");
					break;
				}
				io_close(Source.m_MapdownloadFileTemp);
				Source.m_MapdownloadFileTemp = nullptr;
				FinishMapDownload(SessionId);
			}
			else if(QuicEvent.m_Type == EQuicEventType::MAP_FAILED && QuicEvent.m_Message.m_Session == m_QuicSession)
			{
				char aReason[256];
				str_format(aReason, sizeof(aReason), "QUIC map stream failed: %s", QuicEvent.m_pReason ? QuicEvent.m_pReason : "unknown error");
				ResetMapDownload(SessionId, false);
				DisconnectWithReason(aReason);
				break;
			}
			else if(QuicEvent.m_Type == EQuicEventType::DISCONNECTED && m_UseQuic)
			{
				m_QuicConnected = false;
				char aReason[256];
				str_copy(aReason, QuicEvent.m_pReason ? QuicEvent.m_pReason : "QUIC connection closed");
				if(m_QuicIdentityKnown && m_QuicTransport.ConnectFailure() == EQuicConnectFailure::IDENTITY)
				{
					char aExpected[SHA256_MAXSTRSIZE];
					sha256_str(m_QuicExpectedIdentity, aExpected, sizeof(aExpected));
					char aWarning[768];
					str_format(aWarning, sizeof(aWarning), "The QUIC identity of %s:%d changed. Expected %s; %s. The connection was blocked. Verify the server before using quic_forget_host.", m_aQuicTrustHost, m_QuicTrustPort, aExpected, aReason);
					SWarning Warning(Localize("Server identity changed"), aWarning);
					Warning.m_AutoHide = false;
					AddWarning(Warning);
				}
				DisconnectWithReason(aReason);
				break;
			}
		}
	}

	// check if the primary legacy stream was connected
	if(!QuicSession && Source.State() == ESessionState::CONNECTING && PrimaryNetClient.State() == NETSTATE_ONLINE)
	{
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", "connected, sending info", CLIENT_NETWORK_PRINT_COLOR);
		if(m_SessionManager.FocusedId() == SessionId)
		{
			SetFocusedState(IClient::STATE_LOADING, true);
			SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_INITIAL);
		}
		else
		{
			GameClient()->OnSessionClosed(SessionId);
			Source.SetState(ESessionState::LOADING_MAP);
		}
		SendInfo(SessionId, Source.PrimaryStreamId());
	}

	// progress on dummy connect when the connection is online
	if(SessionId == m_NetworkSessionId && m_DummySendConnInfo && NetClient(CONN_DUMMY).State() == NETSTATE_ONLINE)
	{
		m_DummySendConnInfo = false;
		SendInfo(SessionId, Source.StreamIdAt(CONN_DUMMY));
		NetClient(CONN_DUMMY).Update();
		SendReady(CONN_DUMMY);
		GameClient()->SendDummyInfo(true);
		SendEnterGame(CONN_DUMMY);
	}
	for(const auto &pStream : Source.Streams())
	{
		if(!pStream->m_SendConnectionInfo || pStream->m_NetClient.State() != NETSTATE_ONLINE)
			continue;
		pStream->m_SendConnectionInfo = false;
		SendInfo(SessionId, pStream->m_Id);
		pStream->m_NetClient.Update();
		SendReady(SessionId, pStream->m_Id);
		GameClient()->SendStreamInfo(SessionId, pStream->m_Id, true);
		SendEnterGame(SessionId, pStream->m_Id);
	}

	// process packets
	CNetChunk Packet;
	SECURITY_TOKEN ResponseToken;
	for(const auto &pStream : Source.Streams())
	{
		while(pStream->m_NetClient.Recv(&Packet, &ResponseToken, IsSixup(SessionId)))
		{
			if(Packet.m_ClientId == -1)
			{
				if(ResponseToken != NET_SECURITY_TOKEN_UNKNOWN && !PreprocessConnlessPacket7(&Packet))
					continue;

				ProcessConnlessPacket(&Packet);
				continue;
			}
			ProcessServerPacket(SessionId, pStream->m_Id, &Packet);
		}
	}
	if(SessionId == m_NetworkSessionId)
	{
		while(m_ContactNetClient.Recv(&Packet, &ResponseToken, IsSixup(SessionId)))
		{
			if(Packet.m_ClientId != -1)
				continue;
			if(ResponseToken != NET_SECURITY_TOKEN_UNKNOWN && !PreprocessConnlessPacket7(&Packet))
				continue;
			ProcessConnlessPacket(&Packet);
		}
	}
}

void CClientWithConnection::UpdateNetworkSession(CSessionId SessionId)
{
	PumpNetwork(SessionId);
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	if(Source.State() == ESessionState::READY)
	{
		const CStreamId ActiveStreamId = Source.ActiveStreamId();
		const CStreamId LastActiveStreamId = Source.LastActiveStreamId();
		const int ActiveConn = Source.StreamIndex(ActiveStreamId);
		m_vRepredict.clear();
		bool SendNewInput = false;
		const int64_t ClockNow = time_get();

		if(LastActiveStreamId != ActiveStreamId && m_SessionManager.FocusedId() == SessionId)
		{
			GameClient()->InvalidateSnapshot(SessionId);
			GameClient()->OnConnectionFocusChanged(SessionId, LastActiveStreamId, ActiveStreamId);
		}

		auto AdvanceStream = [&](CStreamId StreamId) {
			CConnection &GameConnection = Connection(SessionId, StreamId);
			if(!GameConnection.m_apSnapshots[SNAP_CURRENT])
				return;

			bool Repredict = false;
			const int64_t Now = GameConnection.m_GameTime.Get(ClockNow);
			if(StreamId == ActiveStreamId && LastActiveStreamId != ActiveStreamId && GameConnection.m_apSnapshots[SNAP_PREV])
			{
				GameClient()->OnNewSnapshot(SessionId, StreamId);
				Repredict = true;
			}

			while(GameConnection.m_apSnapshots[SNAP_CURRENT]->m_pNext)
			{
				const int64_t TickStart = GameConnection.m_apSnapshots[SNAP_CURRENT]->m_Tick * time_freq() / GameTickSpeed();
				if(TickStart >= Now)
					break;
				GameConnection.m_apSnapshots[SNAP_PREV] = GameConnection.m_apSnapshots[SNAP_CURRENT];
				GameConnection.m_apSnapshots[SNAP_CURRENT] = GameConnection.m_apSnapshots[SNAP_CURRENT]->m_pNext;
				GameConnection.m_CurGameTick = GameConnection.m_apSnapshots[SNAP_CURRENT]->m_Tick;
				GameConnection.m_PrevGameTick = GameConnection.m_apSnapshots[SNAP_PREV]->m_Tick;
				GameClient()->OnNewSnapshot(SessionId, StreamId);
				Repredict = true;
			}

			if(GameConnection.m_apSnapshots[SNAP_PREV])
			{
				const int64_t CurTickStart = GameConnection.m_apSnapshots[SNAP_CURRENT]->m_Tick * time_freq() / GameTickSpeed();
				const int NewPredTick = GameConnection.UpdateTiming(Now, GameConnection.m_PredictedTime.Get(ClockNow), GameTickSpeed(), time_freq());
				if(absolute(NewPredTick - GameConnection.m_apSnapshots[SNAP_PREV]->m_Tick) > MaxLatencyTicks(SessionId))
				{
					m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", "prediction time reset!");
					GameConnection.m_PredictedTime.Init(CurTickStart + 2 * time_freq() / GameTickSpeed());
				}
				if(NewPredTick > GameConnection.m_PredTick)
				{
					GameConnection.m_PredTick = NewPredTick;
					Repredict = true;
					SendNewInput |= StreamId == ActiveStreamId;
				}
			}

			if(Repredict)
				m_vRepredict.push_back(StreamId);
		};

		if(ActiveStreamId.IsValid())
			AdvanceStream(ActiveStreamId);
		for(const auto &pStream : Source.Streams())
		{
			if(pStream->m_Id != ActiveStreamId)
				AdvanceStream(pStream->m_Id);
		}

		if(SendNewInput)
			SendInput(SessionId);
		for(CStreamId StreamId : m_vRepredict)
		{
			const CConnection &GameConnection = Connection(SessionId, StreamId);
			if(GameConnection.m_PredTick > GameConnection.m_CurGameTick && GameConnection.m_PredTick < GameConnection.m_CurGameTick + MaxLatencyTicks(SessionId))
				GameClient()->OnPredict(SessionId, StreamId);
		}

		if(ActiveStreamId.IsValid() && Connection(SessionId, ActiveStreamId).m_apSnapshots[SNAP_CURRENT])
		{
			if(SessionId == m_NetworkSessionId && m_CurrentServerInfoRequestTime >= 0 && ClockNow > m_CurrentServerInfoRequestTime)
			{
				RequestServerInfoRefresh(ServerAddress());
				m_CurrentServerInfoRequestTime = ClockNow + time_freq() * 2;
			}
			if(Source.m_NextPingTime >= 0 && ClockNow > Source.m_NextPingTime)
			{
				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "pinging current server%s", !Source.m_ServerCapabilities.m_PingEx ? ", using fallback via server info" : "");
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);
				Source.m_PingUuid = RandomUuid();
				if(!Source.m_ServerCapabilities.m_PingEx)
					RequestServerInfoWithToken(SessionServerAddress(SessionId), &Source.m_PingBasicToken, &Source.m_PingToken);
				else
				{
					CMsgPacker Msg(NETMSG_PINGEX, true);
					Msg.AddRaw(&Source.m_PingUuid, sizeof(Source.m_PingUuid));
					SendMsg(SessionId, Source.PrimaryStreamId(), &Msg, MSGFLAG_FLUSH);
				}
				Source.m_CurrentPingTime = ClockNow;
				Source.m_NextPingTime = ClockNow + 600 * time_freq();
			}
		}

		if(SessionId == m_NetworkSessionId && m_DummyDeactivateOnReconnect && ActiveConn == CONN_DUMMY)
		{
			m_DummyDeactivateOnReconnect = false;
			g_Config.m_ClDummy = 0;
			SetActiveConnection(CONN_MAIN);
		}
		else if(SessionId == m_NetworkSessionId && !m_DummyConnected && m_DummyDeactivateOnReconnect)
		{
			m_DummyDeactivateOnReconnect = false;
		}

		Source.SetLastActiveStreamId(ActiveStreamId);
	}

	if(Source.m_pMapdownloadTask)
	{
		if(Source.m_pMapdownloadTask->State() == EHttpState::DONE)
			FinishMapDownload(SessionId);
		else if(Source.m_pMapdownloadTask->State() == EHttpState::ERROR || Source.m_pMapdownloadTask->State() == EHttpState::ABORTED)
		{
			dbg_msg("webdl", "http failed, falling back to gameserver");
			ResetMapDownload(SessionId, false);
			SendMapRequest(SessionId);
		}
	}
}

bool CClientWithConnection::InitNetworkClient(char *pError, size_t ErrorSize)
{
	NETADDR BindAddr;
	if(g_Config.m_Bindaddr[0] == '\0')
	{
		mem_zero(&BindAddr, sizeof(BindAddr));
	}
	else if(net_host_lookup(g_Config.m_Bindaddr, &BindAddr, NETTYPE_ALL) != 0)
	{
		str_format(pError, ErrorSize, "The configured bindaddr '%s' cannot be resolved.", g_Config.m_Bindaddr);
		return false;
	}
	BindAddr.type = NETTYPE_ALL;
	m_NetworkBindAddr = BindAddr;
	for(int i = 0; i < NUM_CONNS; i++)
	{
		if(!InitNetworkClientImpl(BindAddr, i, pError, ErrorSize))
		{
			return false;
		}
	}
	m_NetworkInitialized = true;
	return true;
}

bool CClientWithConnection::InitNetworkClientImpl(NETADDR BindAddr, int Conn, char *pError, size_t ErrorSize)
{
	int *pPort;
	const char *pName;
	switch(Conn)
	{
	case CONN_MAIN:
		pPort = &g_Config.m_ClPort;
		pName = "main";
		break;
	case CONN_DUMMY:
		pPort = &g_Config.m_ClDummyPort;
		pName = "dummy";
		break;
	case CONN_CONTACT:
		pPort = &g_Config.m_ClContactPort;
		pName = "contact";
		break;
	default:
		dbg_assert_failed("unreachable");
	}
	return InitNetworkStream(BindAddr, NetClient(Conn), *pPort, pName, pError, ErrorSize);
}

bool CClientWithConnection::InitNetworkStream(NETADDR BindAddr, CNetClient &NetClient, int &Port, const char *pName, char *pError, size_t ErrorSize)
{
	if(NetClient.State() != NETSTATE_OFFLINE)
	{
		str_format(pError, ErrorSize, "Could not open network client %s while already connected.", pName);
		return false;
	}
	if(Port < 1024) // Reject users setting ports that we don't want to use
		Port = 0;
	BindAddr.port = Port;

	unsigned RemainingAttempts = 25;
	while(!NetClient.Open(BindAddr))
	{
		--RemainingAttempts;
		if(RemainingAttempts == 0)
		{
			if(g_Config.m_Bindaddr[0])
				str_format(pError, ErrorSize, "Could not open network client %s, try changing or unsetting the bindaddr '%s'.", pName, g_Config.m_Bindaddr);
			else
				str_format(pError, ErrorSize, "Could not open network client %s.", pName);
			return false;
		}
		if(BindAddr.port != 0)
			BindAddr.port = 0;
	}
	return true;
}

IHttp *CClientWithConnection::Http()
{
	return m_pHttp;
}

void CClientWithConnection::HandleConnectLink(const char *pLink)
{
	// Chrome works fine with ddnet:// but not with ddnet:
	// Check ddnet:// before ddnet: because we don't want the // as part of the address
	const char *pAddress = str_startswith(pLink, CONNECTLINK_DOUBLE_SLASH);
	if(pAddress == nullptr)
		pAddress = str_startswith(pLink, CONNECTLINK_NO_SLASH);
	if(pAddress == nullptr)
		pAddress = pLink;
	char aAddress[512];
	str_copy(aAddress, pAddress);
	// Edge appends / to the URL
	const int Length = str_length(aAddress);
	if(Length > 0 && aAddress[Length - 1] == '/')
		aAddress[Length - 1] = '\0';
	Connect(aAddress);
}

void CClientWithConnection::Con_Connect(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->HandleConnectLink(pResult->GetString(0));
}

void CClientWithConnection::Con_DbgConnectSession(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	const CSessionId SessionId = pSelf->CreateNetworkSession();
	if(!SessionId.IsValid())
		return;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "created Network session %" PRIu64, SessionId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
	pSelf->ConnectSession(SessionId, pResult->GetString(0), nullptr);
}

void CClientWithConnection::Con_DbgConnectStream(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	const CSessionId SessionId(pResult->GetInteger(0));
	const CStreamId StreamId = pSelf->ConnectAdditionalStream(SessionId);
	if(!StreamId.IsValid())
		return;
	char aBuf[96];
	str_format(aBuf, sizeof(aBuf), "connected session %" PRIu64 " stream %" PRIu64, SessionId.Value(), StreamId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
}

void CClientWithConnection::Con_DbgDestroyStream(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	const CSessionId SessionId(pResult->GetInteger(0));
	const CStreamId StreamId(pResult->GetInteger(1));
	if(!pSelf->DestroyNetworkStream(SessionId, StreamId))
		return;
	char aBuf[96];
	str_format(aBuf, sizeof(aBuf), "destroyed session %" PRIu64 " stream %" PRIu64, SessionId.Value(), StreamId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
}

void CClientWithConnection::Con_DbgDestroySession(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	const CSessionId SessionId(pResult->GetInteger(0));
	if(!pSelf->DestroyNetworkSession(SessionId))
		return;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "destroyed Network session %" PRIu64, SessionId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
}

void CClientWithConnection::Con_DbgDumpSessions(IConsole::IResult *, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	for(CSessionId SessionId : pSelf->m_SessionManager.SessionIds())
	{
		for(CStreamId StreamId : pSelf->SessionSource(SessionId).StreamIds())
		{
			char aBuf[192];
			str_format(aBuf, sizeof(aBuf), "session=%" PRIu64 " type=%d state=%d stream=%" PRIu64 " active=%d tick=%d map=%s", SessionId.Value(), static_cast<int>(pSelf->SessionType(SessionId)), static_cast<int>(pSelf->SessionState(SessionId)), StreamId.Value(), StreamId == pSelf->ActiveStreamId(SessionId), pSelf->GameTick(SessionId, StreamId), pSelf->ServerInfo(SessionId).m_aMap);
			pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
		}
	}
}

void CClientWithConnection::Con_Disconnect(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->Disconnect();
}

void CClientWithConnection::Con_DummyConnect(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->DummyConnect();
}

void CClientWithConnection::Con_DummyDisconnect(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->DummyDisconnect(nullptr);
}

void CClientWithConnection::Con_DummyResetInput(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->GameClient()->DummyResetInput();
}

void CClientWithConnection::Con_Ping(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;

	CMsgPacker Msg(NETMSG_PING, true);
	pSelf->SendMsg(CONN_MAIN, &Msg, MSGFLAG_FLUSH);
	pSelf->m_PingStartTime = time_get();
}

void CClientWithConnection::ConNetReset(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->ResetSocket();
}

void CClientWithConnection::Con_QuicReconnect(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	if(!pSelf->m_UseQuic || !pSelf->m_QuicConnected || !pSelf->m_QuicTransport.Reconnect(pSelf->m_QuicSession))
		log_error("client", "cannot reconnect inactive QUIC transport");
}

void CClientWithConnection::Con_QuicKnownHost(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = static_cast<CClientWithConnection *>(pUserData);
	SHA256_DIGEST IdentityFingerprint;
	if(!in_range(pResult->GetInteger(1), 1, 65535) || sha256_from_str(&IdentityFingerprint, pResult->GetString(2)) != 0 ||
		!pSelf->AddQuicKnownHost(pResult->GetString(0), pResult->GetInteger(1), IdentityFingerprint))
		log_error("client", "invalid or conflicting QUIC known host");
}

void CClientWithConnection::Con_QuicForgetHost(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = static_cast<CClientWithConnection *>(pUserData);
	char aHost[128];
	if(!NormalizeQuicTrustHost(pResult->GetString(0), aHost, sizeof(aHost)))
	{
		log_error("client", "invalid QUIC known host");
		return;
	}
	const int Port = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : 0;
	const auto NewEnd = std::remove_if(pSelf->m_vQuicKnownHosts.begin(), pSelf->m_vQuicKnownHosts.end(), [&](const CQuicKnownHost &KnownHost) {
		return str_comp(KnownHost.m_aHost, aHost) == 0 && (Port == 0 || KnownHost.m_Port == Port);
	});
	if(NewEnd == pSelf->m_vQuicKnownHosts.end())
	{
		log_info("client", "QUIC known host not found");
		return;
	}
	pSelf->m_vQuicKnownHosts.erase(NewEnd, pSelf->m_vQuicKnownHosts.end());
	pSelf->m_pConfigManager->Save();
}

void CClientWithConnection::QuicKnownHostsConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData)
{
	const CClientWithConnection *pSelf = static_cast<const CClientWithConnection *>(pUserData);
	for(const CQuicKnownHost &KnownHost : pSelf->m_vQuicKnownHosts)
	{
		char aFingerprint[SHA256_MAXSTRSIZE];
		sha256_str(KnownHost.m_IdentityFingerprint, aFingerprint, sizeof(aFingerprint));
		char aLine[256];
		str_format(aLine, sizeof(aLine), "quic_known_host \"%s\" %d %s", KnownHost.m_aHost, KnownHost.m_Port, aFingerprint);
		pConfigManager->WriteLine(aLine);
	}
}

void CClientWithConnection::Con_Rcon(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->Rcon(pResult->GetString(0));
}

void CClientWithConnection::Con_RconAuth(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->RconAuth(pSelf->ActiveConnection(), "", pResult->GetString(0));
}

void CClientWithConnection::Con_RconLogin(IConsole::IResult *pResult, void *pUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pSelf->RconAuth(pSelf->ActiveConnection(), pResult->GetString(0), pResult->GetString(1));
}

void CClientWithConnection::ConchainTimeoutSeed(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		pSelf->m_GenerateTimeoutSeed = false;
}

void CClientWithConnection::ConchainPassword(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pSelf->m_LocalStartTime) //won't set m_pNetworkSessionSource->m_SendPassword before game has started
		pSelf->m_pNetworkSessionSource->m_SendPassword = true;
}

void CClientWithConnection::ConchainNetReset(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClientWithConnection *pSelf = (CClientWithConnection *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		pSelf->ResetSocket();
}

int CClientWithConnection::UdpConnectivity(int NetType)
{
	static const int NETTYPES[2] = {NETTYPE_IPV6, NETTYPE_IPV4};
	int Connectivity = CONNECTIVITY_UNKNOWN;
	for(int PossibleNetType : NETTYPES)
	{
		if((NetType & PossibleNetType) == 0)
		{
			continue;
		}
		NETADDR GlobalUdpAddr;
		int NewConnectivity;
		switch(NetClient(CONN_MAIN).GetConnectivity(PossibleNetType, &GlobalUdpAddr))
		{
		case CONNECTIVITY::UNKNOWN:
			NewConnectivity = CONNECTIVITY_UNKNOWN;
			break;
		case CONNECTIVITY::CHECKING:
			NewConnectivity = CONNECTIVITY_CHECKING;
			break;
		case CONNECTIVITY::UNREACHABLE:
			NewConnectivity = CONNECTIVITY_UNREACHABLE;
			break;
		case CONNECTIVITY::REACHABLE:
			NewConnectivity = CONNECTIVITY_REACHABLE;
			break;
		case CONNECTIVITY::ADDRESS_KNOWN:
			GlobalUdpAddr.port = 0;
			if(m_HaveGlobalTcpAddr && NetType == (int)m_GlobalTcpAddr.type && net_addr_comp(&m_GlobalTcpAddr, &GlobalUdpAddr) != 0)
			{
				NewConnectivity = CONNECTIVITY_DIFFERING_UDP_TCP_IP_ADDRESSES;
				break;
			}
			NewConnectivity = CONNECTIVITY_REACHABLE;
			break;
		default:
			dbg_assert(0, "invalid connectivity value");
			return CONNECTIVITY_UNKNOWN;
		}
		Connectivity = std::max(Connectivity, NewConnectivity);
	}
	return Connectivity;
}
