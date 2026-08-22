/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "client.h"

#include "demoedit.h"
#include "friends.h"
#include "serverbrowser.h"

#include <base/bytes.h>
#include <base/crashdump.h>
#include <base/dbg.h>
#include <base/fs.h>
#include <base/hash.h>
#include <base/hash_ctxt.h>
#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/os.h>
#include <base/process.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/time.h>
#include <base/windows.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/discord.h>
#include <engine/editor.h>
#include <engine/engine.h>
#include <engine/external/json-parser/json.h>
#include <engine/favorites.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/map.h>
#include <engine/notifications.h>
#include <engine/serverbrowser.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/compression.h>
#include <engine/shared/config.h>
#include <engine/shared/demo.h>
#include <engine/shared/fifo.h>
#include <engine/shared/filecollection.h>
#include <engine/shared/game_wire.h>
#include <engine/shared/masterserver.h>
#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol7.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/protocolglue.h>
#include <engine/shared/rust_version.h>
#include <engine/shared/serverinfo.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/uuid_manager.h>
#include <engine/sound.h>
#include <engine/steam.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>
#include <generated/protocolglue.h>

#include <game/localization.h>
#include <game/version.h>

#if defined(CONF_VIDEORECORDER)
#include "video.h"
#endif

#if defined(CONF_PLATFORM_ANDROID)
#include <android/android_main.h>
#endif

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

#if !defined(CONF_DEMO_RENDER_TOOL)
#include "SDL.h"
#ifdef main
#undef main
#endif
#endif

#include <algorithm>
#include <chrono>
#include <csignal>
#include <limits>
#include <stack>
#include <thread>
#include <tuple>

using namespace std::chrono_literals;

#if defined(CONF_VIDEORECORDER)
static volatile sig_atomic_t gs_VideoExportInterruptSignaled = 0;

static void HandleVideoExportInterrupt(int)
{
	gs_VideoExportInterruptSignaled = 1;
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
}
#endif

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

CSnapshotDelta *CClient::SnapshotDelta()
{
	return &m_pNetworkSessionSource->SnapshotDelta(IsSixup(m_NetworkSessionId));
}

CClient::CClient() :
	m_FpsGraph(4096, 0, true)
{
	auto pNetworkSource = std::make_unique<CNetworkSessionSource>();
	m_pNetworkSessionSource = pNetworkSource.get();
	m_NetworkSessionId = m_SessionManager.Create(std::move(pNetworkSource));
	m_pNetworkSessionSource->SetLifecycleCallbacks([this, SessionId = m_NetworkSessionId]() { UpdateNetworkSession(SessionId); }, [this, SessionId = m_NetworkSessionId](const char *pReason) { StopNetworkSession(SessionId, pReason); });
	auto pDemoSource = std::make_unique<CDemoSessionSource>(true, [this](CDemoPlayer &DemoPlayer) { UpdateDemoIntraTimers(DemoPlayer); });
	m_pDemoSessionSource = pDemoSource.get();
	m_DemoSessionId = m_SessionManager.Create(std::move(pDemoSource));
	m_pDemoSessionSource->SetLifecycleCallbacks([this, SessionId = m_DemoSessionId]() { UpdateDemoSession(SessionId); }, [this, SessionId = m_DemoSessionId](const char *pReason) { StopDemoSession(SessionId, pReason); });
#if defined(CONF_VIDEORECORDER)
	auto pVideoExportSource = std::make_unique<CDemoSessionSource>(true, [this](CDemoPlayer &DemoPlayer) { UpdateDemoIntraTimers(DemoPlayer); });
	m_pVideoExportSessionSource = pVideoExportSource.get();
	m_VideoExportSessionId = m_SessionManager.Create(std::move(pVideoExportSource));
	m_pVideoExportSessionSource->SetLifecycleCallbacks([this, SessionId = m_VideoExportSessionId]() { UpdateDemoSession(SessionId); }, [this, SessionId = m_VideoExportSessionId](const char *pReason) { StopDemoSession(SessionId, pReason); });
#endif
	m_SessionManager.SetFocused(m_NetworkSessionId);

	m_StateStartTime = time_get();
	for(auto &DemoRecorder : m_aDemoRecorders)
		DemoRecorder = CDemoRecorder(&m_pNetworkSessionSource->SnapshotDelta(false));
	for(auto &DemoRecorder : m_aDemoRecordersSixup)
		DemoRecorder = CDemoRecorder(&m_pNetworkSessionSource->SnapshotDelta(true));
	m_LastRenderTime = time_get();
	mem_zero(&m_Checksum, sizeof(m_Checksum));
}

CClient::~CClient() = default;

CSessionId CClient::CreateNetworkSession()
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
			if(InitNetworkStream(m_NetworkBindAddr, pStream->m_Connection.m_NetClient, Port, aName, aError, sizeof(aError)))
				continue;
			for(const auto &pOpenedStream : pSourceRaw->Streams())
				pOpenedStream->m_Connection.m_NetClient.Close();
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

CStreamId CClient::ConnectAdditionalStream(CSessionId SessionId)
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
		if(pStream->m_Id != Source.PrimaryStreamId() && pStream->m_Connection.m_NetClient.State() == NETSTATE_OFFLINE)
		{
			StreamId = pStream->m_Id;
			break;
		}
	}
	if(!StreamId.IsValid())
	{
		StreamId = Source.CreateStream();
		CConnection *pConnection = Source.Connection(StreamId);
		dbg_assert(pConnection != nullptr, "failed to create Network stream");
		int Port = 0;
		char aName[64];
		str_format(aName, sizeof(aName), "session %" PRIu64 " stream %" PRIu64, SessionId.Value(), StreamId.Value());
		char aError[256];
		if(!InitNetworkStream(m_NetworkBindAddr, pConnection->m_NetClient, Port, aName, aError, sizeof(aError)))
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
		pStream->m_Connection.m_NetClient.Connect7(&ServerAddress, 1);
	else
		pStream->m_Connection.m_NetClient.Connect(&ServerAddress, 1);
	pStream->m_Connection.m_NetClient.RefreshStun();
	pStream->m_Connection.m_InputtimeMarginGraph.Init(-150.0f, 150.0f);
	pStream->m_Connection.m_GametimeMarginGraph.Init(-150.0f, 150.0f);
	pStream->m_SendConnectionInfo = true;
	Source.SetActiveStream(StreamId);
	GenerateTimeoutCodes(SessionId, &ServerAddress, 1);
	return StreamId;
}

bool CClient::DestroyNetworkStream(CSessionId SessionId, CStreamId StreamId)
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

bool CClient::DestroyNetworkSession(CSessionId SessionId)
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

int CClient::SendMsg(CSessionId SessionId, CStreamId StreamId, CMsgPacker *pMsg, int Flags)
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
		for(auto &DemoRecorder : DemoRecorders())
		{
			if(DemoRecorder.IsRecording())
			{
				DemoRecorder.RecordMessage(Pack.Data(), Pack.Size());
			}
		}
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
			Connection(SessionId, StreamId).m_NetClient.Send(&Packet);
	}

	return 0;
}

void CClient::SendInfo(CSessionId SessionId, CStreamId StreamId)
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

void CClient::SendEnterGame(int Conn)
{
	SendEnterGame(m_NetworkSessionId, StreamId(m_NetworkSessionId, Conn));
}

void CClient::SendEnterGame(CSessionId SessionId, CStreamId StreamId)
{
	CMsgPacker Msg(NETMSG_ENTERGAME, true);
	SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CClient::SendReady(int Conn)
{
	SendReady(m_NetworkSessionId, StreamId(m_NetworkSessionId, Conn));
}

void CClient::SendReady(CSessionId SessionId, CStreamId StreamId)
{
	CMsgPacker Msg(NETMSG_READY, true);
	SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CClient::SendMapRequest(CSessionId SessionId)
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

void CClient::RconAuth(int Conn, const char *pName, const char *pPassword)
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

void CClient::Rcon(const char *pCmd)
{
	CMsgPacker Msg(NETMSG_RCON_CMD, true);
	Msg.AddString(pCmd);
	SendMsg(ActiveConnection(), &Msg, MSGFLAG_VITAL);
}

float CClient::GotRconCommandsPercentage() const
{
	const CNetworkSessionSource &Source = *m_pNetworkSessionSource;
	if(Source.m_ExpectedRconCommands <= 0)
		return -1.0f;
	if(Source.m_GotRconCommands > Source.m_ExpectedRconCommands)
		return -1.0f;

	return (float)Source.m_GotRconCommands / (float)Source.m_ExpectedRconCommands;
}

float CClient::GotMaplistPercentage() const
{
	const CNetworkSessionSource &Source = *m_pNetworkSessionSource;
	if(Source.m_ExpectedMaplistEntries <= 0)
		return -1.0f;
	if(Source.m_vMaplistEntries.size() > (size_t)Source.m_ExpectedMaplistEntries)
		return -1.0f;

	return (float)Source.m_vMaplistEntries.size() / (float)Source.m_ExpectedMaplistEntries;
}

bool CClient::ConnectionProblems(CSessionId SessionId, CStreamId StreamId) const
{
	if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
		return false;
	const int64_t MaxLatency = MaxLatencyTicks(SessionId) * time_freq() / GameTickSpeed();
	// Over QUIC nothing arrives through the legacy connection, so asking it when
	// the last packet came in reports trouble for the whole session.
	if(SessionId == m_NetworkSessionId && m_UseQuic && m_QuicConnected)
		return time_get() - m_QuicLastRecvTime > MaxLatency;
	return Connection(SessionId, StreamId).m_NetClient.GotProblems(MaxLatency);
}

void CClient::SendInput(CSessionId SessionId)
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
		if(pStream->m_Id != ActiveStreamId && pStream->m_Connection.m_NetClient.State() == NETSTATE_ONLINE)
			SendStreamInput(pStream->m_Id);
	}
}

const char *CClient::LatestVersion() const
{
	return m_aVersionStr;
}

// TODO: OPT: do this a lot smarter!
int *CClient::GetInput(CSessionId SessionId, CStreamId StreamId, int Tick) const
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
bool CClient::IsOnline() const
{
	const CGameSession *pSession = m_SessionManager.Focused();
	return pSession && pSession->Source().Type() == ESessionSourceType::NETWORK && pSession->State() == ESessionState::READY;
}

bool CClient::IsDemoPlayback() const
{
	const CGameSession *pSession = m_SessionManager.Focused();
	return pSession && pSession->Source().Type() == ESessionSourceType::DEMO && pSession->State() == ESessionState::READY;
}

void CClient::SetState(EClientState State)
{
	if(m_State == IClient::STATE_QUITTING || m_State == IClient::STATE_RESTARTING)
		return;
	if(State == IClient::STATE_CONNECTING || State == IClient::STATE_ONLINE)
		m_SessionManager.SetFocused(m_NetworkSessionId);
	else if(State == IClient::STATE_DEMOPLAYBACK)
		m_SessionManager.SetFocused(m_DemoSessionId);
	SetFocusedState(State, true);
}

void CClient::SetFocusedState(EClientState State, bool ResetSession)
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

	if(State == IClient::STATE_ONLINE)
	{
		const bool Registered = m_ServerBrowser.IsRegistered(ServerAddress());
		Discord()->SetGameInfo(ServerInfo(m_NetworkSessionId), Registered);
		Steam()->SetGameInfo(ServerAddress(), GameClient()->Map(m_NetworkSessionId)->BaseName(), Registered);
	}
	else if(OldState == IClient::STATE_ONLINE)
	{
		Discord()->ClearGameInfo();
		Steam()->ClearGameInfo();
	}
}

void CClient::FocusSession(CSessionId SessionId)
{
	if(SessionId != m_NetworkSessionId && SessionId != m_DemoSessionId)
		return;
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

// called when the map is loaded and we should init for a new round
void CClient::OnEnterGame(int Conn)
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

void CClient::EnterGame(CSessionId SessionId, CStreamId StreamId)
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

void CClient::OnPostConnect(int Conn)
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

void CClient::GenerateTimeoutSeed()
{
	secure_random_password(g_Config.m_ClTimeoutSeed, sizeof(g_Config.m_ClTimeoutSeed), 16);
}

void CClient::GenerateTimeoutCodes(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs)
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

void CClient::StartLegacyConnection(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs, bool Sixup)
{
	if(SessionId == m_NetworkSessionId && m_QuicTransport.IsRunning())
	{
		for(int i = 0; i < NumAddrs; i++)
			m_QuicTransport.SetLegacyPeer(&pAddrs[i], true);
	}
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	Source.SetSixup(Sixup);
	CNetClient &PrimaryNetClient = Source.Connection(Source.PrimaryStreamId())->m_NetClient;
	if(Sixup)
		PrimaryNetClient.Connect7(pAddrs, NumAddrs);
	else
		PrimaryNetClient.Connect(pAddrs, NumAddrs);
	PrimaryNetClient.RefreshStun();
}

void CClient::ClearQuicTrust()
{
	m_aQuicTrustHost[0] = '\0';
	m_QuicTrustPort = 0;
	m_QuicExpectedIdentity = {};
	m_QuicIdentityRequired = false;
	m_QuicIdentityKnown = false;
	m_QuicRememberIdentity = false;
}

const CClient::CQuicKnownHost *CClient::FindQuicKnownHost(const char *pHost, int Port) const
{
	for(const CQuicKnownHost &KnownHost : m_vQuicKnownHosts)
	{
		if(KnownHost.m_Port == Port && str_comp(KnownHost.m_aHost, pHost) == 0)
			return &KnownHost;
	}
	return nullptr;
}

bool CClient::AddQuicKnownHost(const char *pHost, int Port, SHA256_DIGEST IdentityFingerprint)
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

void CClient::Connect(const char *pAddress, const char *pPassword)
{
	ConnectSession(m_NetworkSessionId, pAddress, pPassword);
}

void CClient::ConnectSession(CSessionId SessionId, const char *pAddress, const char *pPassword)
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
	CNetClient &PrimaryNetClient = Source.Connection(Source.PrimaryStreamId())->m_NetClient;
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
	auto FindTransportEntry = [&](NETADDR Address) {
		if(CServerBrowser::CServerEntry *pEntry = m_ServerBrowser.Find(Address))
			return pEntry;
		Address.type &= ~NETTYPE_TW7;
		return m_ServerBrowser.Find(Address);
	};

	// A server that offers QUIC says so in the browser, including the port it
	// listens on, which is not always the one the legacy transport uses.
	NETADDR AdvertisedQuicAddress = {};
	bool ServerOffersQuic = false;
	bool ServerKnown = false;
	for(int i = 0; i < NumConnectAddrs && !ServerOffersQuic; i++)
	{
		const CServerBrowser::CServerEntry *pEntry = FindTransportEntry(aConnectAddrs[i]);
		if(pEntry == nullptr)
			continue;
		ServerKnown = true;
		if(pEntry->m_Info.m_NumQuicAddresses == 0)
			continue;
		ServerOffersQuic = FindModernAddress(pEntry->m_Info.m_aQuicAddresses, pEntry->m_Info.m_NumQuicAddresses, aConnectAddrs[i], OnlySixup, &AdvertisedQuicAddress);
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
			const CServerBrowser::CServerEntry *pEntry = FindTransportEntry(aConnectAddrs[i]);
			if(!pEntry || !pEntry->m_Info.m_WebTransport)
				continue;
			NETADDR PrefixAddress;
			if(!FindModernAddress(pEntry->m_Info.m_aWebTransportAddresses, pEntry->m_Info.m_NumWebTransportAddresses, aConnectAddrs[i], OnlySixup, &PrefixAddress))
				continue;
			if(pWebTransportInfo &&
				(str_comp(pWebTransportInfo->m_aWebTransportUrl, pEntry->m_Info.m_aWebTransportUrl) != 0 ||
					pWebTransportInfo->m_WebTransportCertificateMode != pEntry->m_Info.m_WebTransportCertificateMode ||
					!SameCertificatePins(*pWebTransportInfo, pEntry->m_Info, true)))
			{
				MetadataAmbiguous = true;
				break;
			}
			if(!ToModernTransportAddress(PrefixAddress, &WebTransportAddress))
				continue;
			pWebTransportInfo = &pEntry->m_Info;
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
		PrimaryConnection.m_NetClient.SetPacketFilter(
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
			const CServerBrowser::CServerEntry *pEntry = FindTransportEntry(aConnectAddrs[i]);
			if(!pEntry)
				continue;
			NETADDR PrefixAddress;
			if(!FindModernAddress(pEntry->m_Info.m_aQuicAddresses, pEntry->m_Info.m_NumQuicAddresses, aConnectAddrs[i], OnlySixup, &PrefixAddress))
				continue;
			NETADDR NextQuicAddress;
			if(!ToModernTransportAddress(PrefixAddress, &NextQuicAddress))
				continue;
			if(pQuicInfo && (QuicAddress.port != NextQuicAddress.port || !SameCertificatePins(*pQuicInfo, pEntry->m_Info, false)))
			{
				QuicMetadataAmbiguous = true;
				break;
			}
			pQuicInfo = &pEntry->m_Info;
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
			PrimaryConnection.m_NetClient.SetPacketFilter(
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

void CClient::DisconnectWithReason(const char *pReason)
{
	m_SessionManager.Close(m_NetworkSessionId, pReason);
	if(!m_pNetworkSessionSource->IsUpdating())
		m_SessionManager.Update(m_NetworkSessionId);
}

void CClient::StopNetworkSession(CSessionId SessionId, const char *pReason)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	char aReconnectError[256];
	str_copy(aReconnectError, Source.Connection(Source.PrimaryStreamId())->m_NetClient.ErrorString());
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
			pStream->m_Connection.m_NetClient.Disconnect(pReason);
			pStream->m_Connection.ResetSnapshots();
		}
		ResetMapDownload(SessionId, true);
		if(m_State < IClient::STATE_QUITTING)
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
	CNetClient &PrimaryNetClient = Source.Connection(Source.PrimaryStreamId())->m_NetClient;
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
		if(m_State < IClient::STATE_QUITTING)
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

void CClient::DisconnectDemoWithReason(CSessionId SessionId, const char *pReason)
{
	m_SessionManager.Close(SessionId, pReason);
	if(!DemoSource(SessionId).IsUpdating())
		m_SessionManager.Update(SessionId);
}

void CClient::StopDemoSession(CSessionId SessionId, const char *pReason)
{
#if defined(CONF_VIDEORECORDER)
	if(SessionId == m_VideoSessionId && m_ActiveVideoExport.has_value() && pReason && pReason[0] != '\0' && m_aVideoExportQueueError[0] == '\0')
		str_copy(m_aVideoExportQueueError, pReason);
#endif
	CDemoSessionSource &Source = DemoSource(SessionId);
	CDemoPlayer &Player = Source.DemoPlayer();
	const bool Focused = m_SessionManager.FocusedId() == SessionId;
	char aReason[256];
	str_copy(aReason, pReason ? pReason : "");
	Player.Stop(aReason);
	if(m_State < IClient::STATE_QUITTING)
		GameClient()->OnSessionClosed(SessionId);
	Source.SetState(ESessionState::OFFLINE);
	Connection(SessionId, CONN_MAIN).ResetSnapshots();
	Source.ResetMetadata();
	if(Focused && m_State < IClient::STATE_QUITTING)
	{
		FocusSession(m_NetworkSessionId);
		CConnection &NetworkConnection = Connection(m_NetworkSessionId, ActiveConnection());
		if(SessionSource(m_NetworkSessionId).State() == ESessionState::READY && NetworkConnection.m_apSnapshots[SNAP_PREV] && NetworkConnection.m_apSnapshots[SNAP_CURRENT])
			GameClient()->OnNewSnapshot(m_NetworkSessionId, ActiveStreamId(m_NetworkSessionId));
	}
}

void CClient::Disconnect()
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

bool CClient::DummyConnected() const
{
	return m_DummyConnected;
}

bool CClient::DummyConnecting() const
{
	return m_DummyConnecting;
}

bool CClient::DummyConnectingDelayed() const
{
	return !DummyConnected() && !DummyConnecting() && m_LastDummyConnectTime > 0.0f && m_LastDummyConnectTime + 5.0f > GlobalTime();
}

void CClient::DummyConnect()
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

void CClient::DummyDisconnect(const char *pReason)
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

bool CClient::DummyAllowed() const
{
	return m_pNetworkSessionSource->m_ServerCapabilities.m_AllowDummy;
}

void CClient::RequestServerInfo(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	Source.ServerInfo() = {};
	if(SessionId == m_NetworkSessionId)
		m_CurrentServerInfoRequestTime = 0;
	else
		m_ServerBrowser.RequestCurrentServer(SessionServerAddress(SessionId));
}

void CClient::SetSessionServerInfo(CSessionId SessionId, const CServerInfo &ServerInfo)
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

void CClient::LoadDebugFont()
{
	m_DebugFont = Graphics()->LoadTexture("debug_font.png", IStorage::TYPE_ALL);
}

// ---

IClient::CSnapItem CClient::SnapGetItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Index) const
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

const void *CClient::SnapFindItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Type, int Id) const
{
	if(!Connection(SessionId, StreamId).m_apSnapshots[SnapId])
		return nullptr;

	return Connection(SessionId, StreamId).m_apSnapshots[SnapId]->m_pAltSnap->FindItem(Type, Id);
}

int CClient::SnapNumItems(CSessionId SessionId, CStreamId StreamId, int SnapId) const
{
	dbg_assert(SnapId >= 0 && SnapId < NUM_SNAPSHOT_TYPES, "invalid SnapId");
	if(!Connection(SessionId, StreamId).m_apSnapshots[SnapId])
		return 0;
	return Connection(SessionId, StreamId).m_apSnapshots[SnapId]->m_pAltSnap->NumItems();
}

void CClient::SnapSetStaticsize(int ItemType, int Size)
{
	m_avSnapshotStaticSizes[false].emplace_back(ItemType, Size);
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::NETWORK)
			NetworkSource(SessionId).SnapshotDelta(false).SetStaticsize(ItemType, Size);
		else
			DemoSource(SessionId).SnapshotDelta(false).SetStaticsize(ItemType, Size);
	}
}

void CClient::SnapSetStaticsize7(int ItemType, int Size)
{
	m_avSnapshotStaticSizes[true].emplace_back(ItemType, Size);
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::NETWORK)
			NetworkSource(SessionId).SnapshotDelta(true).SetStaticsize(ItemType, Size);
		else
			DemoSource(SessionId).SnapshotDelta(true).SetStaticsize(ItemType, Size);
	}
}

void CClient::RenderDebug()
{
	if(!g_Config.m_Debug)
	{
		return;
	}

	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Now - m_NetstatsLastUpdate > 1s)
	{
		m_NetstatsLastUpdate = Now;
		m_NetstatsPrev = m_NetstatsCurrent;
		net_stats(&m_NetstatsCurrent);
	}

	char aBuffer[512];
	const float FontSize = 16.0f;

	Graphics()->TextureSet(m_DebugFont);
	Graphics()->MapScreenToSize(Graphics()->ScreenWidth(), Graphics()->ScreenHeight());
	Graphics()->QuadsBegin();

	const CSessionId SessionId = FocusedSessionId();
	const int Conn = SessionSource(SessionId).Type() == ESessionSourceType::DEMO ? CONN_MAIN : ActiveConnection();
	str_format(aBuffer, sizeof(aBuffer), "Game/predicted tick: %d/%d", GameTick(SessionId, Conn), PredGameTick(SessionId, Conn));
	Graphics()->QuadsText(2, 2, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "Prediction time: %d ms", GetPredictionTime(SessionId, Conn));
	Graphics()->QuadsText(2, 2 + FontSize, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "FPS: %3d", round_to_int(1.0f / m_FrameTimeAverage));
	Graphics()->QuadsText(20.0f * FontSize, 2, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "Frametime: %4d us", round_to_int(m_FrameTimeAverage * 1000000.0f));
	Graphics()->QuadsText(20.0f * FontSize, 2 + FontSize, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "%16s: %" PRIu64 " KiB", "Texture memory", Graphics()->TextureMemoryUsage() / 1024);
	Graphics()->QuadsText(32.0f * FontSize, 2, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "%16s: %" PRIu64 " KiB", "Buffer memory", Graphics()->BufferMemoryUsage() / 1024);
	Graphics()->QuadsText(32.0f * FontSize, 2 + FontSize, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "%16s: %" PRIu64 " KiB", "Streamed memory", Graphics()->StreamedMemoryUsage() / 1024);
	Graphics()->QuadsText(32.0f * FontSize, 2 + 2 * FontSize, FontSize, aBuffer);

	str_format(aBuffer, sizeof(aBuffer), "%16s: %" PRIu64 " KiB", "Staging memory", Graphics()->StagingMemoryUsage() / 1024);
	Graphics()->QuadsText(32.0f * FontSize, 2 + 3 * FontSize, FontSize, aBuffer);

	const IGraphics::SFrameMailboxStats MailboxStats = Graphics()->FrameMailboxStats();
	str_format(aBuffer, sizeof(aBuffer), "%16s: %" PRIu64 " / %" PRIu64 " / %" PRIu64, "Frames P/R/D", MailboxStats.m_Produced, MailboxStats.m_Rendered, MailboxStats.m_Dropped);
	Graphics()->QuadsText(32.0f * FontSize, 2 + 4 * FontSize, FontSize, aBuffer);

	// Network
	{
		const uint64_t OverheadSize = 14 + 20 + 8; // ETH + IP + UDP
		const uint64_t SendPackets = m_NetstatsCurrent.sent_packets - m_NetstatsPrev.sent_packets;
		const uint64_t SendBytes = m_NetstatsCurrent.sent_bytes - m_NetstatsPrev.sent_bytes;
		const uint64_t SendTotal = SendBytes + SendPackets * OverheadSize;
		const uint64_t RecvPackets = m_NetstatsCurrent.recv_packets - m_NetstatsPrev.recv_packets;
		const uint64_t RecvBytes = m_NetstatsCurrent.recv_bytes - m_NetstatsPrev.recv_bytes;
		const uint64_t RecvTotal = RecvBytes + RecvPackets * OverheadSize;

		str_format(aBuffer, sizeof(aBuffer), "Send: %3" PRIu64 " %5" PRIu64 "+%4" PRIu64 "=%5" PRIu64 " (%3" PRIu64 " Kibit/s) average: %5" PRIu64,
			SendPackets, SendBytes, SendPackets * OverheadSize, SendTotal, (SendTotal * 8) / 1024, SendPackets == 0 ? 0 : SendBytes / SendPackets);
		Graphics()->QuadsText(2, 2 + 3 * FontSize, FontSize, aBuffer);
		str_format(aBuffer, sizeof(aBuffer), "Recv: %3" PRIu64 " %5" PRIu64 "+%4" PRIu64 "=%5" PRIu64 " (%3" PRIu64 " Kibit/s) average: %5" PRIu64,
			RecvPackets, RecvBytes, RecvPackets * OverheadSize, RecvTotal, (RecvTotal * 8) / 1024, RecvPackets == 0 ? 0 : RecvBytes / RecvPackets);
		Graphics()->QuadsText(2, 2 + 4 * FontSize, FontSize, aBuffer);
	}

	// Snapshots
	{
		const float OffsetY = 2 + 6 * FontSize;
		int Row = 0;
		str_format(aBuffer, sizeof(aBuffer), "%5s %20s: %8s %8s %8s", "ID", "Name", "Rate", "Updates", "R/U");
		Graphics()->QuadsText(2, OffsetY + Row * 12, FontSize, aBuffer);
		Row++;
		for(int i = 0; i < NUM_NETOBJTYPES; i++)
		{
			if(SnapshotDelta()->GetDataRate(i))
			{
				str_format(
					aBuffer,
					sizeof(aBuffer),
					"%5d %20s: %8" PRIu64 " %8" PRIu64 " %8" PRIu64,
					i,
					GameClient()->GetItemName(i),
					SnapshotDelta()->GetDataRate(i) / 8, SnapshotDelta()->GetDataUpdates(i),
					(SnapshotDelta()->GetDataRate(i) / SnapshotDelta()->GetDataUpdates(i)) / 8);
				Graphics()->QuadsText(2, OffsetY + Row * 12, FontSize, aBuffer);
				Row++;
			}
		}
		for(int i = CSnapshot::MAX_TYPE; i > (CSnapshot::MAX_TYPE - 64); i--)
		{
			if(SnapshotDelta()->GetDataRate(i) && Connection(ActiveConnection()).m_apSnapshots[IClient::SNAP_CURRENT])
			{
				const int Type = Connection(ActiveConnection()).m_apSnapshots[IClient::SNAP_CURRENT]->m_pAltSnap->GetExternalItemType(i);
				if(Type == UUID_INVALID)
				{
					str_format(
						aBuffer,
						sizeof(aBuffer),
						"%5d %20s: %8" PRIu64 " %8" PRIu64 " %8" PRIu64,
						i,
						"Unknown UUID",
						SnapshotDelta()->GetDataRate(i) / 8,
						SnapshotDelta()->GetDataUpdates(i),
						(SnapshotDelta()->GetDataRate(i) / SnapshotDelta()->GetDataUpdates(i)) / 8);
					Graphics()->QuadsText(2, OffsetY + Row * 12, FontSize, aBuffer);
					Row++;
				}
				else if(Type != i)
				{
					str_format(
						aBuffer,
						sizeof(aBuffer),
						"%5d %20s: %8" PRIu64 " %8" PRIu64 " %8" PRIu64,
						Type,
						GameClient()->GetItemName(Type),
						SnapshotDelta()->GetDataRate(i) / 8,
						SnapshotDelta()->GetDataUpdates(i),
						(SnapshotDelta()->GetDataRate(i) / SnapshotDelta()->GetDataUpdates(i)) / 8);
					Graphics()->QuadsText(2, OffsetY + Row * 12, FontSize, aBuffer);
					Row++;
				}
			}
		}
	}

	Graphics()->QuadsEnd();
}

void CClient::RenderGraphs()
{
	if(!g_Config.m_DbgGraphs)
		return;

	// Make sure graph positions and sizes are aligned with pixels to avoid lines overlapping graph edges
	Graphics()->MapScreenToSize(Graphics()->ScreenWidth(), Graphics()->ScreenHeight());
	const float GraphW = std::round(Graphics()->ScreenWidth() / 4.0f);
	const float GraphH = std::round(Graphics()->ScreenHeight() / 6.0f);
	const float GraphSpacing = std::round(Graphics()->ScreenWidth() / 100.0f);
	const float GraphX = Graphics()->ScreenWidth() - GraphW - GraphSpacing;

	TextRender()->TextColor(TextRender()->DefaultTextColor());
	TextRender()->Text(GraphX, GraphSpacing * 5 - 12.0f - 10.0f, 12.0f, Localize("Press Ctrl+Shift+G to disable debug graphs."));

	m_FpsGraph.Scale(time_freq());
	m_FpsGraph.Render(Graphics(), TextRender(), GraphX, GraphSpacing * 5, GraphW, GraphH, "FPS");
	Connection(ActiveConnection()).m_InputtimeMarginGraph.Scale(5 * time_freq());
	Connection(ActiveConnection()).m_InputtimeMarginGraph.Render(Graphics(), TextRender(), GraphX, GraphSpacing * 6 + GraphH, GraphW, GraphH, "Prediction Margin");
	Connection(ActiveConnection()).m_GametimeMarginGraph.Scale(5 * time_freq());
	Connection(ActiveConnection()).m_GametimeMarginGraph.Render(Graphics(), TextRender(), GraphX, GraphSpacing * 7 + GraphH * 2, GraphW, GraphH, "Gametime Margin");
}

void CClient::Restart()
{
	SetState(IClient::STATE_RESTARTING);
}

void CClient::Quit()
{
	SetState(IClient::STATE_QUITTING);
}

void CClient::ResetSocket()
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
				if(!InitNetworkStream(BindAddr, Source.ConnectionAt(StreamIndex).m_NetClient, Port, aName, aError, sizeof(aError)))
					log_error("client", "%s", aError);
			}
		}
	}
	char aContactError[256];
	if(!InitNetworkClientImpl(BindAddr, CONN_CONTACT, aContactError, sizeof(aContactError)))
		log_error("client", "%s", aContactError);
	if(m_UseQuic && !m_UseWebTransport)
	{
		CNetworkSessionSource &NetworkSessionSource = NetworkSource(m_NetworkSessionId);
		NetworkSessionSource.Connection(NetworkSessionSource.PrimaryStreamId())->m_NetClient.SetPacketFilter([](void *pUser, const NETADDR *pAddress, const void *pData, int DataSize) { return static_cast<CQuicTransport *>(pUser)->FeedUdp(pAddress, pData, DataSize); }, &m_QuicTransport);
		m_QuicTransport.LocalAddressChanged();
	}
}
const char *CClient::PlayerName() const
{
	if(g_Config.m_PlayerName[0])
	{
		return g_Config.m_PlayerName;
	}
	if(g_Config.m_SteamName[0])
	{
		return g_Config.m_SteamName;
	}
	return "nameless tee";
}

const char *CClient::DummyName()
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

const char *CClient::ErrorString() const
{
	return NetClient(CONN_MAIN).ErrorString();
}

void CClient::Render()
{
	if(m_EditorActive)
	{
		m_pEditor->OnRender();
	}
	else
	{
		GameClient()->OnRender();
	}

	RenderDebug();
	RenderGraphs();
}

void CClient::RenderScreen()
{
	if(!m_EditorActive)
		GameClient()->OnRenderPrepare();
	Render();
	if(!m_EditorActive)
		GameClient()->OnRenderFinalize();
}

const char *CClient::LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
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

	// Stop demo recording before loading a new Network map.
	if(SessionId == m_NetworkSessionId)
	{
		for(int Recorder = 0; Recorder < RECORDER_MAX; Recorder++)
			DemoRecorder(Recorder)->Stop(Recorder == RECORDER_REPLAYS ? IDemoRecorder::EStopMode::REMOVE_FILE : IDemoRecorder::EStopMode::KEEP_FILE);
	}

	// Unload the current map and reset all snapshots before loading a new map,
	// because the snapshots are only valid for the old map.
	IMap *pMap = GameClient()->Map(SessionId);
	if(SessionSource(SessionId).Type() == ESessionSourceType::NETWORK)
	{
		for(const auto &pStream : NetworkSource(SessionId).Streams())
			pStream->m_Connection.ResetSnapshots();
	}
	else
		Connection(SessionId, CONN_MAIN).ResetSnapshots();
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

static void FormatMapDownloadFilename(const char *pName, const std::optional<SHA256_DIGEST> &Sha256, int Crc, bool Temp, char *pBuffer, int BufferSize)
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

const char *CClient::LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc)
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

void CClient::ProcessConnlessPacket(CNetChunk *pPacket)
{
	// server info
	if(pPacket->m_DataSize >= (int)sizeof(SERVERBROWSE_INFO))
	{
		int Type = -1;
		if(mem_comp(pPacket->m_pData, SERVERBROWSE_INFO, sizeof(SERVERBROWSE_INFO)) == 0)
			Type = SERVERINFO_VANILLA;
		else if(mem_comp(pPacket->m_pData, SERVERBROWSE_INFO_EXTENDED, sizeof(SERVERBROWSE_INFO_EXTENDED)) == 0)
			Type = SERVERINFO_EXTENDED;
		else if(mem_comp(pPacket->m_pData, SERVERBROWSE_INFO_EXTENDED_MORE, sizeof(SERVERBROWSE_INFO_EXTENDED_MORE)) == 0)
			Type = SERVERINFO_EXTENDED_MORE;

		if(Type != -1)
		{
			void *pData = (unsigned char *)pPacket->m_pData + sizeof(SERVERBROWSE_INFO);
			int DataSize = pPacket->m_DataSize - sizeof(SERVERBROWSE_INFO);
			ProcessServerInfo(Type, &pPacket->m_Address, pData, DataSize);
		}
	}
}

static int SavedServerInfoType(int Type)
{
	if(Type == SERVERINFO_EXTENDED_MORE)
		return SERVERINFO_EXTENDED;

	return Type;
}

void CClient::ProcessServerInfo(int RawType, NETADDR *pFrom, const void *pData, int DataSize)
{
	CServerBrowser::CServerEntry *pEntry = m_ServerBrowser.Find(*pFrom);

	CServerInfo Info = {0};
	int SavedType = SavedServerInfoType(RawType);
	if(SavedType == SERVERINFO_EXTENDED && pEntry && pEntry->m_GotInfo && SavedType == pEntry->m_Info.m_Type)
	{
		Info = pEntry->m_Info;
	}
	else
	{
		Info.m_NumAddresses = 1;
		Info.m_aAddresses[0] = *pFrom;
	}

	Info.m_Type = SavedType;

	net_addr_str(pFrom, Info.m_aAddress, sizeof(Info.m_aAddress), true);

	CUnpacker Up;
	Up.Reset(pData, DataSize);

#define GET_STRING(array) str_copy(array, Up.GetString(CUnpacker::SANITIZE_CC | CUnpacker::SKIP_START_WHITESPACES))
#define GET_INT(integer) (integer) = str_toint(Up.GetString())

	int Token;
	int PacketNo = 0; // Only used if SavedType == SERVERINFO_EXTENDED

	GET_INT(Token);
	if(RawType != SERVERINFO_EXTENDED_MORE)
	{
		GET_STRING(Info.m_aVersion);
		GET_STRING(Info.m_aName);
		GET_STRING(Info.m_aMap);

		if(SavedType == SERVERINFO_EXTENDED)
		{
			GET_INT(Info.m_MapCrc);
			GET_INT(Info.m_MapSize);
		}

		GET_STRING(Info.m_aGameType);
		GET_INT(Info.m_Flags);
		GET_INT(Info.m_NumPlayers);
		GET_INT(Info.m_MaxPlayers);
		GET_INT(Info.m_NumClients);
		GET_INT(Info.m_MaxClients);

		// don't add invalid info to the server browser list
		if(Info.m_NumClients < 0 || Info.m_MaxClients < 0 ||
			Info.m_NumPlayers < 0 || Info.m_MaxPlayers < 0 ||
			Info.m_NumPlayers > Info.m_NumClients || Info.m_MaxPlayers > Info.m_MaxClients)
		{
			return;
		}

		m_ServerBrowser.UpdateServerCommunity(&Info);
		m_ServerBrowser.UpdateServerRank(&Info);

		switch(SavedType)
		{
		case SERVERINFO_VANILLA:
			if(Info.m_MaxPlayers > VANILLA_MAX_CLIENTS ||
				Info.m_MaxClients > VANILLA_MAX_CLIENTS)
			{
				return;
			}
			break;
		case SERVERINFO_64_LEGACY:
			if(Info.m_MaxPlayers > MAX_CLIENTS ||
				Info.m_MaxClients > MAX_CLIENTS)
			{
				return;
			}
			break;
		case SERVERINFO_EXTENDED:
			if(Info.m_NumPlayers > Info.m_NumClients)
				return;
			break;
		default:
			dbg_assert_failed("unknown serverinfo type");
		}

		if(SavedType == SERVERINFO_EXTENDED)
			PacketNo = 0;
	}
	else
	{
		GET_INT(PacketNo);
		// 0 needs to be excluded because that's reserved for the main packet.
		if(PacketNo <= 0 || PacketNo >= 64)
			return;
	}

	bool DuplicatedPacket = false;
	if(SavedType == SERVERINFO_EXTENDED)
	{
		const char *pExtraInfo = Up.GetString();
		if(RawType == SERVERINFO_EXTENDED)
		{
			Info.m_QuicCertificateSha256 = {};
			Info.m_QuicNextCertificateSha256 = {};
			Info.m_QuicIdentityFingerprint = {};
			Info.m_WebTransportCertificateSha256 = {};
			Info.m_WebTransportNextCertificateSha256 = {};
			Info.m_HasWebTransportNextCertificateSha256 = false;
			Info.m_QuicPort = 0;
			Info.m_QuicCapabilities = 0;
			Info.m_QuicSharedPort = false;
			Info.m_RawQuic = false;
			Info.m_HasQuicNextCertificateSha256 = false;
			Info.m_HasQuicIdentityFingerprint = false;
			Info.m_QuicTrust = EModernTransportTrust::INVALID;
			Info.m_aModernHostname[0] = '\0';
			Info.m_WebTransport = false;
			Info.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::NONE;
			Info.m_aWebTransportPath[0] = '\0';
			Info.m_aWebTransportUrl[0] = '\0';
			ParseQuicServerInfoExtra(&Info, pExtraInfo, pFrom->port);
		}

		uint64_t Flag = (uint64_t)1 << PacketNo;
		DuplicatedPacket = Info.m_ReceivedPackets & Flag;
		Info.m_ReceivedPackets |= Flag;
	}

	bool IgnoreError = false;
	for(int i = 0; i < MAX_CLIENTS && (int)Info.m_vClients.size() < MAX_CLIENTS && !Up.Error(); i++)
	{
		CServerInfo::CClient Client = {};
		GET_STRING(Client.m_aName);
		if(Up.Error())
		{
			// Packet end, no problem unless it happens during one
			// player info, so ignore the error.
			IgnoreError = true;
			break;
		}
		GET_STRING(Client.m_aClan);
		GET_INT(Client.m_Country);
		if(!in_range(Client.m_Country, CountryCode::MINIMUM, CountryCode::MAXIMUM))
		{
			Client.m_Country = CountryCode::DEFAULT;
		}
		GET_INT(Client.m_Score);
		GET_INT(Client.m_Player);
		if(SavedType == SERVERINFO_EXTENDED)
		{
			Up.GetString(); // extra info, reserved
		}
		if(!Up.Error())
		{
			if(SavedType == SERVERINFO_64_LEGACY)
			{
				uint64_t Flag = (uint64_t)1 << i;
				if(!(Info.m_ReceivedPackets & Flag))
				{
					Info.m_ReceivedPackets |= Flag;
					Info.m_vClients.push_back(Client);
				}
			}
			else
			{
				Info.m_vClients.push_back(Client);
			}
		}
	}

	str_clean_whitespaces(Info.m_aName);

	if(!Up.Error() || IgnoreError)
	{
		if(!DuplicatedPacket && (!pEntry || !pEntry->m_GotInfo || SavedType >= pEntry->m_Info.m_Type))
		{
			m_ServerBrowser.OnServerInfoUpdate(*pFrom, Token, &Info);
		}

		// Player info is irrelevant for the client (while connected),
		// it gets its info from elsewhere.
		//
		// SERVERINFO_EXTENDED_MORE doesn't carry any server
		// information, so just skip it.
		for(size_t i = 0; i < m_SessionManager.NumSessions(); ++i)
		{
			const CSessionId SessionId = m_SessionManager.SessionAt(i)->Id();
			if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
				continue;
			CNetworkSessionSource &Source = NetworkSource(SessionId);
			CNetClient &PrimaryNetClient = Source.Connection(Source.PrimaryStreamId())->m_NetClient;
			// Over QUIC the legacy connection stays offline and the server is
			// reached under the QUIC address instead.
			const bool QuicSession = SessionId == m_NetworkSessionId && m_UseQuic;
			const bool Online = QuicSession ? m_QuicConnected : PrimaryNetClient.State() == NETSTATE_ONLINE;
			const NETADDR &SessionAddress = QuicSession ? m_QuicServerAddress : *PrimaryNetClient.ServerAddress();
			if(!Online || SessionAddress != *pFrom || RawType == SERVERINFO_EXTENDED_MORE)
				continue;
			// Only accept server info that has a type that is
			// newer or equal to something the server already sent
			// us.
			if(SavedType >= ServerInfo(SessionId).m_Type && GameClient()->Map(SessionId)->IsLoaded())
			{
				SetSessionServerInfo(SessionId, Info);
				if(SessionId == m_NetworkSessionId)
					Discord()->UpdateServerInfo(ServerInfo(SessionId));
			}

			bool ValidPong = false;
			if(!Source.m_ServerCapabilities.m_PingEx && Source.m_CurrentPingTime >= 0 && SavedType >= Source.m_PingInfoType)
			{
				if(RawType == SERVERINFO_VANILLA)
				{
					ValidPong = Token == Source.m_PingBasicToken;
				}
				else if(RawType == SERVERINFO_EXTENDED)
				{
					ValidPong = Token == Source.m_PingToken;
				}
			}
			if(ValidPong)
			{
				int LatencyMs = (time_get() - Source.m_CurrentPingTime) * 1000 / time_freq();
				m_ServerBrowser.SetCurrentServerPing(SessionAddress, LatencyMs);
				Source.m_PingInfoType = SavedType;
				Source.m_CurrentPingTime = -1;

				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "got pong from current server, latency=%dms", LatencyMs);
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf);
			}
		}
	}

#undef GET_STRING
#undef GET_INT
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

void CClient::ProcessServerPacket(CSessionId SessionId, CStreamId StreamId, CNetChunk *pPacket)
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
					EscapeUrl(aEscaped, str_startswith(Source.m_aMapdownloadFilename, "downloadedmaps/"));
					bool UseConfigUrl = str_comp(g_Config.m_ClMapDownloadUrl, "https://maps.ddnet.org") != 0 || m_aMapDownloadUrl[0] == '\0';
					str_format(aUrl, sizeof(aUrl), "%s/%s", UseConfigUrl ? g_Config.m_ClMapDownloadUrl : m_aMapDownloadUrl, aEscaped);

					Source.m_pMapdownloadTask = HttpGetFile(pMapUrl ? pMapUrl : aUrl, Storage(), Source.m_aMapdownloadFilenameTemp, IStorage::TYPE_SAVE);
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
				m_ServerBrowser.SetCurrentServerPing(SessionServerAddress(SessionId), LatencyMs);
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
							for(auto &DemoRecorder : DemoRecorders())
							{
								if(DemoRecorder.IsRecording())
								{
									// write snapshot
									DemoRecorder.RecordSnapshot(GameTick, IsSixup(SessionId) ? SnapSeven.AsSnapshot() : TmpBuffer3.AsSnapshot(), DemoSnapSize);
								}
							}
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
		if(SessionId == m_NetworkSessionId && !InactiveStream)
		{
			for(auto &DemoRecorder : DemoRecorders())
			{
				if(DemoRecorder.IsRecording())
				{
					DemoRecorder.RecordMessage(pPacket->m_pData, pPacket->m_DataSize);
				}
			}
		}

		GameClient()->OnMessage(SessionId, Msg, &Unpacker, StreamId);
	}
}

int CClient::UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo)
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

void CClient::ResetMapDownload(CSessionId SessionId, bool ResetActive)
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

void CClient::FinishMapDownload(CSessionId SessionId)
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

void CClient::ResetDDNetInfoTask()
{
	if(m_pDDNetInfoTask)
	{
		m_pDDNetInfoTask->Abort();
		m_pDDNetInfoTask = nullptr;
	}
}

typedef std::tuple<int, int, int> TVersion;
static const TVersion gs_InvalidVersion = std::make_tuple(-1, -1, -1);

static TVersion ToVersion(char *pStr)
{
	int aVersion[3] = {0, 0, 0};
	const char *p = strtok(pStr, ".");

	for(int i = 0; i < 3 && p; ++i)
	{
		if(!str_isallnum(p))
			return gs_InvalidVersion;

		aVersion[i] = str_toint(p);
		p = strtok(nullptr, ".");
	}

	if(p)
		return gs_InvalidVersion;

	return std::make_tuple(aVersion[0], aVersion[1], aVersion[2]);
}

void CClient::LoadDDNetInfo()
{
	const json_value *pDDNetInfo = m_ServerBrowser.LoadDDNetInfo();

	if(!pDDNetInfo)
	{
		m_InfoState = EInfoState::ERROR;
		return;
	}

	const json_value &DDNetInfo = *pDDNetInfo;
	const json_value &CurrentVersion = DDNetInfo["version"];
	if(CurrentVersion.type == json_string)
	{
		char aNewVersionStr[64];
		str_copy(aNewVersionStr, CurrentVersion);
		char aCurVersionStr[64];
		str_copy(aCurVersionStr, GAME_RELEASE_VERSION);
		if(ToVersion(aNewVersionStr) > ToVersion(aCurVersionStr))
		{
			str_copy(m_aVersionStr, CurrentVersion);
		}
		else
		{
			m_aVersionStr[0] = '0';
			m_aVersionStr[1] = '\0';
		}
	}

	const json_value &News = DDNetInfo["news"];
	if(News.type == json_string)
	{
		// Only mark news button if something new was added to the news
		if(m_aNews[0] && str_find(m_aNews, News) == nullptr)
			g_Config.m_UiUnreadNews = true;

		str_copy(m_aNews, News);
	}

	const json_value &MapDownloadUrl = DDNetInfo["map-download-url"];
	if(MapDownloadUrl.type == json_string)
	{
		str_copy(m_aMapDownloadUrl, MapDownloadUrl);
	}

	const json_value &Points = DDNetInfo["points"];
	if(Points.type == json_integer)
	{
		m_Points = Points.u.integer;
	}

	const json_value &StunServersIpv6 = DDNetInfo["stun-servers-ipv6"];
	if(StunServersIpv6.type == json_array && StunServersIpv6[0].type == json_string)
	{
		NETADDR Addr;
		if(!net_addr_from_str(&Addr, StunServersIpv6[0]))
		{
			NetClient(CONN_MAIN).FeedStunServer(Addr);
		}
	}
	const json_value &StunServersIpv4 = DDNetInfo["stun-servers-ipv4"];
	if(StunServersIpv4.type == json_array && StunServersIpv4[0].type == json_string)
	{
		NETADDR Addr;
		if(!net_addr_from_str(&Addr, StunServersIpv4[0]))
		{
			NetClient(CONN_MAIN).FeedStunServer(Addr);
		}
	}
	const json_value &ConnectingIp = DDNetInfo["connecting-ip"];
	if(ConnectingIp.type == json_string)
	{
		NETADDR Addr;
		if(!net_addr_from_str(&Addr, ConnectingIp))
		{
			m_HaveGlobalTcpAddr = true;
			m_GlobalTcpAddr = Addr;
			log_debug("info", "got global tcp ip address: %s", (const char *)ConnectingIp);
		}
	}
	const json_value &WarnPngliteIncompatibleImages = DDNetInfo["warn-pnglite-incompatible-images"];
	Graphics()->WarnPngliteIncompatibleImages(WarnPngliteIncompatibleImages.type == json_boolean && (bool)WarnPngliteIncompatibleImages);
	m_InfoState = EInfoState::SUCCESS;
}

int CClient::ConnectNetTypes() const
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

const NETADDR &CClient::SessionServerAddress(CSessionId SessionId) const
{
	return SessionId == m_NetworkSessionId && m_UseQuic ? m_QuicServerAddress : *NetworkSource(SessionId).Connection(NetworkSource(SessionId).PrimaryStreamId())->m_NetClient.ServerAddress();
}

void CClient::PumpNetwork(CSessionId SessionId)
{
	CNetworkSessionSource &Source = NetworkSource(SessionId);
	CNetClient &PrimaryNetClient = Source.Connection(Source.PrimaryStreamId())->m_NetClient;
	const bool QuicSession = SessionId == m_NetworkSessionId && m_UseQuic;
	for(const auto &pStream : Source.Streams())
	{
		pStream->m_Connection.m_NetClient.Update();
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
			if(pStream->m_Id == Source.PrimaryStreamId() || (SessionId == m_NetworkSessionId && pStream->m_Id == Source.StreamIdAt(CONN_DUMMY)) || pStream->m_Connection.m_NetClient.State() != NETSTATE_OFFLINE || pStream->m_Connection.m_NetClient.ErrorString()[0] == '\0')
				continue;
			const CStreamId StreamId = pStream->m_Id;
			char aError[256];
			str_copy(aError, pStream->m_Connection.m_NetClient.ErrorString());
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
		if(!pStream->m_SendConnectionInfo || pStream->m_Connection.m_NetClient.State() != NETSTATE_ONLINE)
			continue;
		pStream->m_SendConnectionInfo = false;
		SendInfo(SessionId, pStream->m_Id);
		pStream->m_Connection.m_NetClient.Update();
		SendReady(SessionId, pStream->m_Id);
		GameClient()->SendStreamInfo(SessionId, pStream->m_Id, true);
		SendEnterGame(SessionId, pStream->m_Id);
	}

	// process packets
	CNetChunk Packet;
	SECURITY_TOKEN ResponseToken;
	for(const auto &pStream : Source.Streams())
	{
		while(pStream->m_Connection.m_NetClient.Recv(&Packet, &ResponseToken, IsSixup(SessionId)))
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

void CClient::OnDemoPlayerSnapshot(CDemoPlayer &DemoPlayer, void *pData, int Size)
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

void CClient::OnDemoPlayerMessage(CDemoPlayer &DemoPlayer, void *pData, int Size)
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

CSessionId CClient::FindDemoSessionId(const CDemoPlayer &DemoPlayer) const
{
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::DEMO && &DemoSource(SessionId).DemoPlayer() == &DemoPlayer)
			return SessionId;
	}
	return {};
}

void CClient::UpdateDemoIntraTimers(CDemoPlayer &DemoPlayer)
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

int64_t CClient::DemoPlaybackTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).DemoPlayer().Video())
		return pVideo->Time();
#endif
	return time_get();
}

float CClient::DemoPlaybackLocalTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(const IVideo *pVideo = DemoSource(SessionId).DemoPlayer().Video())
		return pVideo->LocalTime();
#endif
	return LocalTime();
}

void CClient::UpdateDemoSession(CSessionId SessionId)
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

void CClient::UpdateNetworkSession(CSessionId SessionId)
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
				m_ServerBrowser.RequestCurrentServer(ServerAddress());
				m_CurrentServerInfoRequestTime = ClockNow + time_freq() * 2;
			}
			if(Source.m_NextPingTime >= 0 && ClockNow > Source.m_NextPingTime)
			{
				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "pinging current server%s", !Source.m_ServerCapabilities.m_PingEx ? ", using fallback via server info" : "");
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);
				Source.m_PingUuid = RandomUuid();
				if(!Source.m_ServerCapabilities.m_PingEx)
					m_ServerBrowser.RequestCurrentServerWithRandomToken(SessionServerAddress(SessionId), &Source.m_PingBasicToken, &Source.m_PingToken);
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
void CClient::Update()
{
	m_SessionManager.Update();
#if defined(CONF_VIDEORECORDER)
	UpdateVideoExportQueue();
#endif

	// STRESS TEST: join the server again
	if(g_Config.m_DbgStress)
	{
		static int64_t s_ActionTaken = 0;
		int64_t Now = time_get();
		if(State() == IClient::STATE_OFFLINE)
		{
			if(Now > s_ActionTaken + time_freq() * 2)
			{
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "stress", "reconnecting!");
				Connect(g_Config.m_DbgStressServer);
				s_ActionTaken = Now;
			}
		}
		else
		{
			if(Now > s_ActionTaken + time_freq() * (10 + g_Config.m_DbgStress))
			{
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "stress", "disconnecting!");
				Disconnect();
				s_ActionTaken = Now;
			}
		}
	}

	if(m_pDDNetInfoTask)
	{
		if(m_pDDNetInfoTask->State() == EHttpState::DONE)
		{
			if(m_ServerBrowser.DDNetInfoSha256() == m_pDDNetInfoTask->ResultSha256())
			{
				log_debug("client/info", "DDNet info already up-to-date");
				m_InfoState = EInfoState::SUCCESS;
			}
			else
			{
				log_debug("client/info", "Loading new DDNet info");
				LoadDDNetInfo();
			}

			ResetDDNetInfoTask();
		}
		else if(m_pDDNetInfoTask->State() == EHttpState::ERROR || m_pDDNetInfoTask->State() == EHttpState::ABORTED)
		{
			ResetDDNetInfoTask();
			m_InfoState = EInfoState::ERROR;
		}
	}

	if(IsOnline())
	{
		if(!m_EditJobs.empty())
		{
			std::shared_ptr<CDemoEdit> pJob = m_EditJobs.front();
			if(pJob->State() == IJob::STATE_DONE)
			{
				char aBuf[IO_MAX_PATH_LENGTH + 64];
				if(pJob->Success())
				{
					str_format(aBuf, sizeof(aBuf), "Successfully saved the replay to '%s'!", pJob->Destination());
					m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", aBuf);

					GameClient()->Echo(Localize("Successfully saved the replay!"));
				}
				else
				{
					str_format(aBuf, sizeof(aBuf), "Failed saving the replay to '%s'...", pJob->Destination());
					m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", aBuf);

					GameClient()->Echo(Localize("Failed saving the replay!"));
				}
				m_EditJobs.pop_front();
			}
		}
	}

	// update the server browser
	m_ServerBrowser.Update();

	// update editor/gameclient
	if(m_EditorActive)
		m_pEditor->OnUpdate();
	else
		GameClient()->OnUpdate();

	Discord()->Update();
	Steam()->Update();
	if(Steam()->GetConnectAddress())
	{
		HandleConnectAddress(Steam()->GetConnectAddress());
		Steam()->ClearConnectAddress();
	}

	// Walked by index: asking for the ids would hand back a fresh vector every
	// frame, and this runs on every one of them.
	for(size_t SessionIndex = 0; SessionIndex < m_SessionManager.NumSessions(); ++SessionIndex)
	{
		const CSessionId SessionId = m_SessionManager.SessionAt(SessionIndex)->Id();
		if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
			continue;
		CNetworkSessionSource &Source = NetworkSource(SessionId);
		std::string PendingAddress;
		std::string PendingPassword;
		if(Source.ConsumePendingConnect(PendingAddress, PendingPassword))
		{
			ConnectSession(SessionId, PendingAddress.c_str(), PendingPassword.c_str());
		}
		else if(Source.ConsumeReconnect(time_get()))
		{
			const std::string ConnectAddress = Source.m_ConnectAddress;
			const std::string Password = Source.m_SendPassword ? g_Config.m_Password : Source.m_Password;
			ConnectSession(SessionId, ConnectAddress.c_str(), Password.c_str());
		}
		for(const auto &pStream : Source.Streams())
			pStream->m_Connection.m_PredictedTime.UpdateMargin(PredictionMargin(SessionId) * time_freq() / 1000);
	}
}

void CClient::RegisterInterfaces()
{
	Kernel()->RegisterInterface(static_cast<IDemoPlayer *>(&DemoPlayer()), false);
	Kernel()->RegisterInterface(static_cast<IGhostRecorder *>(&m_GhostRecorder), false);
	Kernel()->RegisterInterface(static_cast<IGhostLoader *>(&m_GhostLoader), false);
	Kernel()->RegisterInterface(static_cast<IServerBrowser *>(&m_ServerBrowser), false);
#if defined(CONF_AUTOUPDATE)
	Kernel()->RegisterInterface(static_cast<IUpdater *>(&m_Updater), false);
#endif
	Kernel()->RegisterInterface(static_cast<IFriends *>(&m_Friends), false);
	Kernel()->ReregisterInterface(static_cast<IFriends *>(&m_Foes));
}

void CClient::InitInterfaces()
{
	// fetch interfaces
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pEditor = Kernel()->RequestInterface<IEditor>();
	m_pFavorites = Kernel()->RequestInterface<IFavorites>();
	m_pSound = Kernel()->RequestInterface<IEngineSound>();
	m_pGameClient = Kernel()->RequestInterface<IGameClient>();
	m_pHttp = Kernel()->RequestInterface<IEngineHttp>();
	m_pInput = Kernel()->RequestInterface<IEngineInput>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = m_pConfigManager->Values();
#if defined(CONF_AUTOUPDATE)
	m_pUpdater = Kernel()->RequestInterface<IUpdater>();
#endif
	m_pDiscord = Kernel()->RequestInterface<IDiscord>();
	m_pSteam = Kernel()->RequestInterface<ISteam>();
	m_pNotifications = Kernel()->RequestInterface<INotifications>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	m_DemoEditor.Init(&m_pNetworkSessionSource->SnapshotDelta(false), &m_pNetworkSessionSource->SnapshotDelta(true), m_pConsole, m_pStorage);

	m_ServerBrowser.SetBaseInfo(&m_ContactNetClient, m_pGameClient->NetVersion());

#if defined(CONF_AUTOUPDATE)
	m_Updater.Init();
#endif

	m_pConfigManager->RegisterCallback(IFavorites::ConfigSaveCallback, m_pFavorites);
	m_pConfigManager->RegisterCallback(QuicKnownHostsConfigSaveCallback, this);
	m_Friends.Init();
	m_Foes.Init(true);

	m_GhostRecorder.Init();
	m_GhostLoader.Init();
}

static void SleepIdle(std::chrono::nanoseconds Duration)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// Sleeping keeps the browser's main thread to itself, so the page neither
	// paints nor delivers input for as long as it lasts. Emscripten's sleep is the
	// one that hands control back, and it counts in whole milliseconds.
	const int64_t Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Duration).count();
	if(Milliseconds > 0)
		emscripten_sleep(Milliseconds);
#else
	std::this_thread::sleep_for(Duration);
#endif
}

void CClient::Run()
{
	bool NonInteractive = false;
#if defined(CONF_VIDEORECORDER)
	NonInteractive = m_CommandLineVideoExport;
#endif
	m_LocalStartTime = m_GlobalStartTime = time_get();
	Connection(CONN_MAIN).m_SnapshotParts = 0;
	Connection(CONN_DUMMY).m_SnapshotParts = 0;

	if(m_GenerateTimeoutSeed)
	{
		GenerateTimeoutSeed();
	}

	unsigned int Seed;
	secure_random_fill(&Seed, sizeof(Seed));
	srand(Seed);

	if(g_Config.m_Debug)
	{
		g_UuidManager.DebugDump();
	}

	char aNetworkError[256];
	if(!InitNetworkClient(aNetworkError, sizeof(aNetworkError)))
	{
		log_error("client", "%s", aNetworkError);
		if(!NonInteractive)
			ShowMessageBox({.m_pTitle = "Network Error", .m_pMessage = aNetworkError});
#if defined(CONF_VIDEORECORDER)
		m_CommandLineExitCode = 1;
#endif
		return;
	}

	if(!m_pHttp->Init(std::chrono::seconds{1}))
	{
		const char *pErrorMessage = "Failed to initialize the HTTP client.";
		log_error("client", "%s", pErrorMessage);
		if(!NonInteractive)
			ShowMessageBox({.m_pTitle = "HTTP Error", .m_pMessage = pErrorMessage});
#if defined(CONF_VIDEORECORDER)
		m_CommandLineExitCode = 1;
#endif
		return;
	}

	// init graphics
	m_pGraphics = CreateEngineGraphicsThreaded(
#if defined(CONF_VIDEORECORDER)
		m_CommandLineVideoExport ? EGraphicsBackendMode::OFFSCREEN :
#endif
					   EGraphicsBackendMode::PRESENTATION,
		m_HiddenWindow);
	Kernel()->RegisterInterface(m_pGraphics); // IEngineGraphics
	Kernel()->RegisterInterface(static_cast<IGraphics *>(m_pGraphics), false);
	{
		CMemoryLogger MemoryLogger;
		MemoryLogger.SetParent(log_get_scope_logger());
		bool Success;
		{
			CLogScope LogScope(&MemoryLogger);
			Success = m_pGraphics->Init() == 0;
		}
		if(!Success)
		{
			log_error("client", "Failed to initialize the graphics (see details above)");
			const std::string Message = std::string(
							    "Failed to initialize the graphics. See details below.\n\n"
							    "For detailed troubleshooting instructions please read our Wiki:\n"
							    "https://wiki.ddnet.org/wiki/GFX_Troubleshooting\n\n") +
						    MemoryLogger.ConcatenatedLines();
			const std::vector<IGraphics::CMessageBoxButton> vButtons = {
				{.m_pLabel = "Show Wiki"},
				{.m_pLabel = "OK", .m_Confirm = true, .m_Cancel = true},
			};
			const std::optional<int> MessageResult = NonInteractive ? std::nullopt : ShowMessageBox({.m_pTitle = "Graphics Initialization Error", .m_pMessage = Message.c_str(), .m_vButtons = vButtons});
			if(MessageResult && *MessageResult == 0)
			{
				ViewLink("https://wiki.ddnet.org/wiki/GFX_Troubleshooting");
			}
#if defined(CONF_VIDEORECORDER)
			m_CommandLineExitCode = 1;
#endif
			return;
		}
	}

	// make sure the first frame just clears everything to prevent undesired colors when waiting for io
	if(!NonInteractive)
	{
		Graphics()->Clear(0, 0, 0);
		Graphics()->Swap();
	}

	// init localization first, making sure all errors during init can be localized
	GameClient()->InitializeLanguage();

	// init sound, allowed to fail
	const bool SoundInitFailed = Sound()->Init() != 0;

#if defined(CONF_VIDEORECORDER)
	// init video recorder aka ffmpeg
	CVideo::Init();
#endif

	// init text render
	m_pTextRender = Kernel()->RequestInterface<IEngineTextRender>();
	m_pTextRender->Init();

	// init the input
	Input()->Init();

	// init the editor
	m_pEditor->Init();

	m_ServerBrowser.OnInit();
	// loads the existing ddnet info file if it exists
	LoadDDNetInfo();

	LoadDebugFont();

	if(Steam()->GetPlayerName())
	{
		str_copy(g_Config.m_SteamName, Steam()->GetPlayerName());
	}

	Graphics()->AddWindowResizeListener([this] { OnWindowResize(); });

	GameClient()->OnInit();

	m_Fifo.Init(m_pConsole, g_Config.m_ClInputFifo, CFGFLAG_CLIENT);

	m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", "version " GAME_RELEASE_VERSION " on " CONF_PLATFORM_STRING " " CONF_ARCH_STRING, ColorRGBA(0.7f, 0.7f, 1.0f, 1.0f));
	if(GIT_SHORTREV_HASH)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "git revision hash: %s", GIT_SHORTREV_HASH);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf, ColorRGBA(0.7f, 0.7f, 1.0f, 1.0f));
	}

	//
	m_FpsGraph.Init(0.0f, 120.0f);

	// never start with the editor
	g_Config.m_ClEditor = 0;

	// process pending commands
	m_pConsole->StoreCommands(false);
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(g_Config.m_ClWebtransport && m_aCmdConnect[0])
		m_ServerBrowser.Refresh(IServerBrowser::TYPE_INTERNET);
#endif

	InitChecksum();
	m_pConsole->InitChecksum(ChecksumData());

	// request the new ddnet info from server if already past the welcome dialog
	if(g_Config.m_ClShowWelcome)
		g_Config.m_ClShowWelcome = 0;
	else
		RequestDDNetInfo();

	if(SoundInitFailed)
	{
		SWarning Warning(Localize("Sound error"), Localize("The audio device couldn't be initialised."));
		Warning.m_AutoHide = false;
		AddWarning(Warning);
	}

	bool LastD = false;
	bool LastE = false;
	bool LastG = false;

	auto LastTime = time_get_nanoseconds();
	int64_t LastRenderTime = time_get();

#if defined(CONF_VIDEORECORDER)
	if(m_CommandLineVideoExport)
	{
		CVideoExportSettings Settings = m_CommandLineVideoSettings;
		const CVideoExportSettings Defaults = DefaultVideoExportSettings();
		if(Settings.m_Width == 0)
			Settings.m_Width = Defaults.m_Width;
		if(Settings.m_Height == 0)
			Settings.m_Height = Defaults.m_Height;
		const char *pError = QueueVideoExport(m_aCommandLineDemoPath, IStorage::TYPE_ALL_OR_ABSOLUTE, m_aCommandLineVideoPath, Settings, DEMO_SPEED_INDEX_DEFAULT, true, true);
		if(pError)
		{
			log_error("videorecorder", "Could not queue command-line export: %s", pError);
			m_CommandLineExitCode = 1;
			return;
		}
	}
#endif

	while(true)
	{
		set_new_tick();
#if defined(CONF_VIDEORECORDER)
		if(m_CommandLineVideoExport && gs_VideoExportInterruptSignaled)
		{
			gs_VideoExportInterruptSignaled = 0;
			str_copy(m_aVideoExportQueueError, "Video rendering interrupted.");
			m_VideoExportQueue.clear();
			if(m_pVideo && IVideo::Current() == m_pVideo.get())
			{
				m_pVideo->Cancel();
				if(CDemoPlayer *pPlayer = VideoDemoPlayer())
					pPlayer->Stop(m_aVideoExportQueueError);
			}
			else if(m_ActiveVideoExport.has_value() && m_VideoSessionId.IsValid())
				DisconnectDemoWithReason(m_VideoSessionId, m_aVideoExportQueueError);
			else
				m_VideoExportQueueRunning = true;
		}
#endif

		// handle pending connects
		if(m_aCmdConnect[0]
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			&& (!g_Config.m_ClWebtransport || !m_ServerBrowser.IsGettingServerlist())
#endif
		)
		{
			str_copy(g_Config.m_UiServerAddress, m_aCmdConnect);
			Connect(m_aCmdConnect);
			m_aCmdConnect[0] = 0;
		}

		// handle pending demo play
		if(m_aCmdPlayDemo[0])
		{
			const char *pError = DemoPlayer_Play(m_aCmdPlayDemo, IStorage::TYPE_ALL_OR_ABSOLUTE);
			if(pError)
				log_error("demo_player", "playing passed demo file '%s' failed: %s", m_aCmdPlayDemo, pError);
			m_aCmdPlayDemo[0] = 0;
		}

		// handle pending map edits
		if(m_aCmdEditMap[0])
		{
			int Result = m_pEditor->HandleMapDrop(m_aCmdEditMap, IStorage::TYPE_ALL_OR_ABSOLUTE);
			if(Result)
				g_Config.m_ClEditor = true;
			else
				log_error("editor", "editing passed map file '%s' failed", m_aCmdEditMap);
			m_aCmdEditMap[0] = 0;
		}

		// update input
		if(Input()->Update())
		{
			if(State() == IClient::STATE_QUITTING)
				break;
			else
				SetState(IClient::STATE_QUITTING); // SDL_QUIT
		}

		char aFile[IO_MAX_PATH_LENGTH];
		if(Input()->GetDropFile(aFile, sizeof(aFile)))
		{
			if(str_startswith(aFile, CONNECTLINK_NO_SLASH) || str_startswith(aFile, QUIC_CONNECTLINK_DOUBLE_SLASH) || str_startswith(aFile, QUIC_CONNECTLINK7_DOUBLE_SLASH) || str_startswith(aFile, WT_CONNECTLINK_DOUBLE_SLASH) || str_startswith(aFile, WT_CONNECTLINK7_DOUBLE_SLASH))
				HandleConnectLink(aFile);
			else if(str_endswith(aFile, ".demo"))
				HandleDemoPath(aFile);
			else if(str_endswith(aFile, ".map"))
				HandleMapPath(aFile);
		}

#if defined(CONF_AUTOUPDATE)
		Updater()->Update();
#endif

		// update sound
		Sound()->Update();

		if(CtrlShiftKey(KEY_D, LastD))
			g_Config.m_Debug ^= 1;

		if(CtrlShiftKey(KEY_G, LastG))
			g_Config.m_DbgGraphs ^= 1;

		if(CtrlShiftKey(KEY_E, LastE))
		{
			if(g_Config.m_ClEditor)
				m_pEditor->OnClose();
			g_Config.m_ClEditor = g_Config.m_ClEditor ^ 1;
		}

		// render
		{
			if(g_Config.m_ClEditor)
			{
				if(!m_EditorActive)
				{
					Input()->MouseModeRelative();
					GameClient()->OnActivateEditor();
					m_pEditor->OnActivate();
					m_EditorActive = true;
				}
			}
			else if(m_EditorActive)
			{
				m_EditorActive = false;
			}

			Update();
			int64_t Now = time_get();

			bool IsRenderActive = (g_Config.m_GfxBackgroundRender || m_pGraphics->WindowOpen());

			int GfxRefreshRate = g_Config.m_GfxRefreshRate;

#if defined(CONF_VIDEORECORDER)
			// keep rendering synced
			if(IVideo::Current())
			{
				GfxRefreshRate = 0;
				IsRenderActive = true;
			}
#endif

			if(IsRenderActive &&
				(!GfxRefreshRate || (time_freq() / (int64_t)g_Config.m_GfxRefreshRate) <= Now - LastRenderTime))
			{
				// update frametime
				m_RenderFrameTime = (Now - m_LastRenderTime) / (float)time_freq();
				m_FpsGraph.Add(1.0f / m_RenderFrameTime);

				if(m_BenchmarkFile)
				{
					char aBuf[64];
					str_format(aBuf, sizeof(aBuf), "Frametime %d us\n", (int)(m_RenderFrameTime * 1000000));
					io_write(m_BenchmarkFile, aBuf, str_length(aBuf));
					if(time_get() > m_BenchmarkStopTime)
					{
						io_close(m_BenchmarkFile);
						m_BenchmarkFile = nullptr;
						Quit();
					}
				}

				m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;

				// keep the overflow time - it's used to make sure the gfx refreshrate is reached
				int64_t AdditionalTime = GfxRefreshRate ? ((Now - LastRenderTime) - (time_freq() / (int64_t)GfxRefreshRate)) : 0;
				// if the value is over the frametime of a 60 fps frame, reset the additional time (drop the frames, that are lost already)
				if(AdditionalTime > (time_freq() / 60))
					AdditionalTime = (time_freq() / 60);
				LastRenderTime = Now - AdditionalTime;
				m_LastRenderTime = Now;

#if defined(CONF_VIDEORECORDER)
				bool VideoFrameHandled = false;
				IVideo *pVideo = IVideo::Current();
				if(pVideo != nullptr)
					VideoFrameHandled = pVideo->BeginVideoFrameRender();
				if(VideoFrameHandled)
				{
					dbg_assert(m_VideoSessionId.IsValid(), "missing video session");
					GameClient()->OnRenderVideoPrepare(m_VideoSessionId, pVideo->Settings());
					GameClient()->OnRender();
					if(pVideo->HasAudio())
					{
						const bool OfflineAudio = m_VideoOfflineAudio;
						pVideo->NextAudioFrameTimeline([this, OfflineAudio](short *pFinalOut, unsigned Frames) {
							if(OfflineAudio)
								Sound()->MixOffline(pFinalOut, Frames);
							else
								Sound()->Mix(pFinalOut, Frames);
						});
					}
					GameClient()->OnRenderFinalize();
				}
				else if(!m_CommandLineVideoExport)
#endif
					RenderScreen();
#if defined(CONF_VIDEORECORDER)
				// Rendering dispatches user input, which can stop the recording
				// through the console. Stopping destroys the recorder, so the
				// current one has to be looked up again instead of reused.
				pVideo = IVideo::Current();
				if(VideoFrameHandled && pVideo != nullptr)
				{
					pVideo->EndVideoFrameRender();
					const std::chrono::nanoseconds ProgressRenderTime = time_get_nanoseconds();
					const std::chrono::nanoseconds ProgressInterval = m_CommandLineVideoExport ? std::chrono::seconds(1) : std::chrono::milliseconds(100);
					if(ProgressRenderTime - m_LastVideoProgressRender >= ProgressInterval)
					{
						m_LastVideoProgressRender = ProgressRenderTime;
						bool Cancel = false;
						if(m_CommandLineVideoExport)
						{
							const CDemoPlayer *pPlayer = VideoDemoPlayer();
							dbg_assert(pPlayer != nullptr, "missing video demo player");
							const IDemoPlayer::CInfo *pInfo = pPlayer->BaseInfo();
							const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
							const int CurrentTicks = std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks);
							const float Progress = TotalTicks == 0 ? 0.0f : CurrentTicks / static_cast<float>(TotalTicks);
							const CVideoExportStatus Status = pVideo->Status();
							log_info("videorecorder", "Rendering %.1f%% (%" PRIu64 " / %" PRIu64 " frames encoded)", Progress * 100.0f, Status.m_EncodedFrames, Status.m_SubmittedFrames);
						}
						else
						{
							const bool BackgroundExport = m_VideoSessionId == m_VideoExportSessionId;
							if(BackgroundExport)
								RenderScreen();
							Cancel = GameClient()->OnRenderVideoProgress(BackgroundExport);
						}
						if(!m_CommandLineVideoExport)
							m_pGraphics->Swap();
						if(Cancel && IVideo::Current() == m_pVideo.get())
						{
							str_copy(m_aVideoExportQueueError, "Video rendering cancelled.");
							m_pVideo->Cancel();
							if(CDemoPlayer *pPlayer = VideoDemoPlayer())
								pPlayer->Stop("Video rendering cancelled.");
						}
					}
				}
				else if(!m_CommandLineVideoExport)
					m_pGraphics->Swap();
#else
				m_pGraphics->Swap();
#endif
#if defined(CONF_VIDEORECORDER)
				if(pVideo != nullptr && pVideo->HasError())
				{
					const CVideoExportStatus Status = pVideo->Status();
					str_copy(m_aVideoError, Status.m_aError[0] == '\0' ? "Video recording failed." : Status.m_aError);
					pVideo->Stop();
					if(CDemoPlayer *pPlayer = VideoDemoPlayer())
						pPlayer->Stop(m_aVideoError);
				}
#endif
			}
			else if(!IsRenderActive)
			{
				// if the client does not render, it should reset its render time to a time where it would render the first frame, when it wakes up again
				LastRenderTime = g_Config.m_GfxRefreshRate ? (Now - (time_freq() / (int64_t)g_Config.m_GfxRefreshRate)) : Now;
			}
		}

		AutoScreenshot_Cleanup();
		AutoStatScreenshot_Cleanup();
		AutoCSV_Cleanup();

		m_Fifo.Update();

		if(State() == IClient::STATE_QUITTING || State() == IClient::STATE_RESTARTING)
			break;

		// beNice
		auto Now = time_get_nanoseconds();
		decltype(Now) SleepTimeInNanoSeconds{0};
		bool Slept = false;
		int ClientRefreshRate = g_Config.m_ClRefreshRate;
		int InactiveRefreshRate = g_Config.m_ClRefreshRateInactive;
#if defined(CONF_VIDEORECORDER)
		if(IVideo::Current() && IVideo::Current()->IsRecording())
		{
			ClientRefreshRate = 0;
			InactiveRefreshRate = 0;
		}
#endif
		if(InactiveRefreshRate && !m_pGraphics->WindowActive())
		{
			SleepTimeInNanoSeconds = (std::chrono::nanoseconds(1s) / (int64_t)InactiveRefreshRate) - (Now - LastTime);
			SleepIdle(SleepTimeInNanoSeconds);
			Slept = true;
		}
		else if(ClientRefreshRate)
		{
			SleepTimeInNanoSeconds = (std::chrono::nanoseconds(1s) / (int64_t)ClientRefreshRate) - (Now - LastTime);
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			// Waiting on a socket cannot block in the browser: the wait reports what
			// is ready and returns, so the loop below would spin the budget away
			// instead of waiting it out, and hold the page for the whole of it.
			SleepIdle(SleepTimeInNanoSeconds);
#else
			auto SleepTimeInNanoSecondsInner = SleepTimeInNanoSeconds;
			auto NowInner = Now;
			while(std::chrono::duration_cast<std::chrono::microseconds>(SleepTimeInNanoSecondsInner) > 0us)
			{
				net_socket_read_wait(NetClient(CONN_MAIN).m_Socket, SleepTimeInNanoSecondsInner);
				auto NowInnerCalc = time_get_nanoseconds();
				SleepTimeInNanoSecondsInner -= (NowInnerCalc - NowInner);
				NowInner = NowInnerCalc;
			}
#endif
			Slept = true;
		}
		if(Slept)
		{
			// if the diff gets too small it shouldn't get even smaller (drop the updates, that could not be handled)
			if(SleepTimeInNanoSeconds < -16666666ns)
				SleepTimeInNanoSeconds = -16666666ns;
			// don't go higher than the frametime of a 60 fps frame
			else if(SleepTimeInNanoSeconds > 16666666ns)
				SleepTimeInNanoSeconds = 16666666ns;
			// the time diff between the time that was used actually used and the time the thread should sleep/wait
			// will be calculated in the sleep time of the next update tick by faking the time it should have slept/wait.
			// so two cases (and the case it slept exactly the time it should):
			//	- the thread slept/waited too long, then it adjust the time to sleep/wait less in the next update tick
			//	- the thread slept/waited too less, then it adjust the time to sleep/wait more in the next update tick
			LastTime = Now + SleepTimeInNanoSeconds;
		}
		else
		{
			LastTime = Now;
		}

		// update local and global time
		m_LocalTime = (time_get() - m_LocalStartTime) / (float)time_freq();
		m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
	}

	if(!NonInteractive)
		GameClient()->RenderShutdownMessage();
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::DEMO && SessionSource(SessionId).State() != ESessionState::OFFLINE)
			DisconnectDemoWithReason(SessionId, nullptr);
	}
#if defined(CONF_VIDEORECORDER)
	if(m_pVideo)
	{
		if(IVideo::Current() == m_pVideo.get())
			m_pVideo->Stop();
		m_pVideo.reset();
		m_VideoSessionId = {};
		m_VideoOfflineAudio = false;
	}
#endif
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::NETWORK && SessionSource(SessionId).State() != ESessionState::OFFLINE)
		{
			m_SessionManager.Close(SessionId);
			m_SessionManager.Update(SessionId);
		}
	}

	if(!m_pConfigManager->Save())
	{
		char aError[128];
		str_format(aError, sizeof(aError), Localize("Saving settings to '%s' failed"), CONFIG_FILE);
		m_vQuittingWarnings.emplace_back(Localize("Error saving settings"), aError);
	}
	m_Fifo.Shutdown();
	m_pHttp->Shutdown();
	Engine()->ShutdownJobs();

	if(!NonInteractive)
		GameClient()->RenderShutdownMessage();
	GameClient()->OnShutdown();
	delete m_pEditor;

	// close sockets
	for(CSessionId SessionId : m_SessionManager.SessionIds())
	{
		if(SessionSource(SessionId).Type() != ESessionSourceType::NETWORK)
			continue;
		for(const auto &pStream : NetworkSource(SessionId).Streams())
			pStream->m_Connection.m_NetClient.Close();
	}
	m_ContactNetClient.Close();

	// shutdown text render while graphics are still available
	m_pTextRender->Shutdown();
}

bool CClient::InitNetworkClient(char *pError, size_t ErrorSize)
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

bool CClient::InitNetworkClientImpl(NETADDR BindAddr, int Conn, char *pError, size_t ErrorSize)
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

bool CClient::InitNetworkStream(NETADDR BindAddr, CNetClient &NetClient, int &Port, const char *pName, char *pError, size_t ErrorSize)
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

bool CClient::CtrlShiftKey(int Key, bool &Last)
{
	if(Input()->ModifierIsPressed() && Input()->ShiftIsPressed() && !Last && Input()->KeyIsPressed(Key))
	{
		Last = true;
		return true;
	}
	else if(Last && !Input()->KeyIsPressed(Key))
	{
		Last = false;
	}

	return false;
}

void CClient::Con_Connect(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->HandleConnectLink(pResult->GetString(0));
}

void CClient::Con_DbgConnectSession(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	const CSessionId SessionId = pSelf->CreateNetworkSession();
	if(!SessionId.IsValid())
		return;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "created Network session %" PRIu64, SessionId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
	pSelf->ConnectSession(SessionId, pResult->GetString(0), nullptr);
}

void CClient::Con_DbgConnectStream(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	const CSessionId SessionId(pResult->GetInteger(0));
	const CStreamId StreamId = pSelf->ConnectAdditionalStream(SessionId);
	if(!StreamId.IsValid())
		return;
	char aBuf[96];
	str_format(aBuf, sizeof(aBuf), "connected session %" PRIu64 " stream %" PRIu64, SessionId.Value(), StreamId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
}

void CClient::Con_DbgDestroyStream(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	const CSessionId SessionId(pResult->GetInteger(0));
	const CStreamId StreamId(pResult->GetInteger(1));
	if(!pSelf->DestroyNetworkStream(SessionId, StreamId))
		return;
	char aBuf[96];
	str_format(aBuf, sizeof(aBuf), "destroyed session %" PRIu64 " stream %" PRIu64, SessionId.Value(), StreamId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
}

void CClient::Con_DbgDestroySession(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	const CSessionId SessionId(pResult->GetInteger(0));
	if(!pSelf->DestroyNetworkSession(SessionId))
		return;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "destroyed Network session %" PRIu64, SessionId.Value());
	pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client/session", aBuf);
}

void CClient::Con_DbgDumpSessions(IConsole::IResult *, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
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

void CClient::Con_Disconnect(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->Disconnect();
}

void CClient::Con_DummyConnect(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->DummyConnect();
}

void CClient::Con_DummyDisconnect(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->DummyDisconnect(nullptr);
}

void CClient::Con_DummyResetInput(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->GameClient()->DummyResetInput();
}

void CClient::Con_Quit(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->Quit();
}

void CClient::Con_Restart(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->Restart();
}

void CClient::Con_Minimize(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->Graphics()->Minimize();
}

void CClient::Con_Ping(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;

	CMsgPacker Msg(NETMSG_PING, true);
	pSelf->SendMsg(CONN_MAIN, &Msg, MSGFLAG_FLUSH);
	pSelf->m_PingStartTime = time_get();
}

void CClient::ConNetReset(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->ResetSocket();
}

void CClient::Con_QuicReconnect(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(!pSelf->m_UseQuic || !pSelf->m_QuicConnected || !pSelf->m_QuicTransport.Reconnect(pSelf->m_QuicSession))
		log_error("client", "cannot reconnect inactive QUIC transport");
}

void CClient::Con_QuicKnownHost(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = static_cast<CClient *>(pUserData);
	SHA256_DIGEST IdentityFingerprint;
	if(!in_range(pResult->GetInteger(1), 1, 65535) || sha256_from_str(&IdentityFingerprint, pResult->GetString(2)) != 0 ||
		!pSelf->AddQuicKnownHost(pResult->GetString(0), pResult->GetInteger(1), IdentityFingerprint))
		log_error("client", "invalid or conflicting QUIC known host");
}

void CClient::Con_QuicForgetHost(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = static_cast<CClient *>(pUserData);
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

void CClient::QuicKnownHostsConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData)
{
	const CClient *pSelf = static_cast<const CClient *>(pUserData);
	for(const CQuicKnownHost &KnownHost : pSelf->m_vQuicKnownHosts)
	{
		char aFingerprint[SHA256_MAXSTRSIZE];
		sha256_str(KnownHost.m_IdentityFingerprint, aFingerprint, sizeof(aFingerprint));
		char aLine[256];
		str_format(aLine, sizeof(aLine), "quic_known_host \"%s\" %d %s", KnownHost.m_aHost, KnownHost.m_Port, aFingerprint);
		pConfigManager->WriteLine(aLine);
	}
}

void CClient::AutoScreenshot_Start()
{
	if(g_Config.m_ClAutoScreenshot)
	{
		Graphics()->TakeScreenshot("auto/autoscreen");
		m_AutoScreenshotRecycle = true;
	}
}

void CClient::AutoStatScreenshot_Start()
{
	if(g_Config.m_ClAutoStatboardScreenshot)
	{
		Graphics()->TakeScreenshot("auto/stats/autoscreen");
		m_AutoStatScreenshotRecycle = true;
	}
}

void CClient::AutoScreenshot_Cleanup()
{
	if(m_AutoScreenshotRecycle)
	{
		if(g_Config.m_ClAutoScreenshotMax)
		{
			// clean up auto taken screens
			CFileCollection AutoScreens;
			AutoScreens.Init(Storage(), "screenshots/auto", "autoscreen", ".png", g_Config.m_ClAutoScreenshotMax);
		}
		m_AutoScreenshotRecycle = false;
	}
}

void CClient::AutoStatScreenshot_Cleanup()
{
	if(m_AutoStatScreenshotRecycle)
	{
		if(g_Config.m_ClAutoStatboardScreenshotMax)
		{
			// clean up auto taken screens
			CFileCollection AutoScreens;
			AutoScreens.Init(Storage(), "screenshots/auto/stats", "autoscreen", ".png", g_Config.m_ClAutoStatboardScreenshotMax);
		}
		m_AutoStatScreenshotRecycle = false;
	}
}

void CClient::AutoCSV_Start()
{
	if(g_Config.m_ClAutoCSV)
		m_AutoCSVRecycle = true;
}

void CClient::AutoCSV_Cleanup()
{
	if(m_AutoCSVRecycle)
	{
		if(g_Config.m_ClAutoCSVMax)
		{
			// clean up auto csvs
			CFileCollection AutoRecord;
			AutoRecord.Init(Storage(), "record/csv", "autorecord", ".csv", g_Config.m_ClAutoCSVMax);
		}
		m_AutoCSVRecycle = false;
	}
}

void CClient::Con_Screenshot(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->Graphics()->TakeScreenshot(nullptr);
}

#if defined(CONF_VIDEORECORDER)

void CClient::Con_StartVideo(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = static_cast<CClient *>(pUserData);

	if(pResult->NumArguments())
	{
		pSelf->StartVideo(pSelf->m_DemoSessionId, pResult->GetString(0), false, pSelf->DefaultVideoExportSettings(), false);
	}
	else
	{
		pSelf->StartVideo(pSelf->m_DemoSessionId, "video", true, pSelf->DefaultVideoExportSettings(), false);
	}
}

CVideoExportSettings CClient::DefaultVideoExportSettings()
{
	CVideoExportSettings Settings;
	Settings.m_Width = Graphics()->ScreenWidth() & ~1;
	Settings.m_Height = Graphics()->ScreenHeight() & ~1;
	Settings.m_FPS = g_Config.m_ClVideoRecorderFPS;
	Settings.m_Audio = g_Config.m_ClVideoSndEnable != 0;
	Settings.m_Crf = g_Config.m_ClVideoX264Crf;
	Settings.m_Preset = g_Config.m_ClVideoX264Preset;
	str_copy(Settings.m_aVideoCodec, g_Config.m_ClVideoCodec);
	Settings.m_EncodeThreads = g_Config.m_ClVideoEncodeThreads;
	Settings.m_ShowHud = g_Config.m_ClVideoShowhud != 0;
	Settings.m_ShowChat = g_Config.m_ClVideoShowChat != 0;
	Settings.m_ShowHookCollOther = g_Config.m_ClVideoShowHookCollOther != 0;
	Settings.m_ShowDirection = g_Config.m_ClVideoShowDirection;
	Settings.m_ShowImportantAlerts = g_Config.m_ClVideoShowImportantAlerts != 0;
	return Settings;
}

CDemoPlayer *CClient::VideoDemoPlayer()
{
	return m_VideoSessionId.IsValid() ? &DemoSource(m_VideoSessionId).DemoPlayer() : nullptr;
}

bool CClient::DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const
{
	if(!m_VideoSessionId.IsValid())
		return false;
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_VideoSessionId).DemoPlayer().BaseInfo();
	*pFirstTick = pInfo->m_FirstTick;
	*pCurrentTick = pInfo->m_CurrentTick;
	*pLastTick = pInfo->m_LastTick;
	return true;
}

void CClient::DemoPlayer_CancelActiveRender()
{
	// Both being null passes an inequality check, which is exactly the state
	// between taking a job off the queue and creating its recorder.
	if(!m_ActiveVideoExport.has_value() || m_pVideo == nullptr || IVideo::Current() != m_pVideo.get())
		return;
	str_copy(m_aVideoExportQueueError, "Video rendering cancelled.");
	m_pVideo->Cancel();
	if(CDemoPlayer *pPlayer = VideoDemoPlayer())
		pPlayer->Stop(m_aVideoExportQueueError);
}

const char *CClient::StartVideo(CSessionId SessionId, const char *pFilename, bool WithTimestamp, const CVideoExportSettings &Settings, bool ExactFilename)
{
	m_aVideoError[0] = '\0';
	if(SessionSource(SessionId).Type() != ESessionSourceType::DEMO || SessionSource(SessionId).State() != ESessionState::READY)
	{
		str_copy(m_aVideoError, "Video can only be recorded in demo player.");
		log_error("videorecorder", "%s", m_aVideoError);
		return m_aVideoError;
	}

	if(IVideo::Current())
	{
		str_copy(m_aVideoError, "Already recording.");
		log_error("videorecorder", "%s", m_aVideoError);
		return m_aVideoError;
	}
	if(Settings.m_Audio && !GameClient()->IsSoundReady())
	{
		str_copy(m_aVideoError, "Sound assets are still loading.");
		log_error("videorecorder", "%s", m_aVideoError);
		return m_aVideoError;
	}
	m_pVideo.reset();

	char aFilename[IO_MAX_PATH_LENGTH];
	if(ExactFilename)
	{
		str_copy(aFilename, pFilename);
		if(!str_endswith(aFilename, ".mp4"))
			str_append(aFilename, ".mp4");
	}
	else if(WithTimestamp)
	{
		char aTimestamp[20];
		str_timestamp(aTimestamp, sizeof(aTimestamp));
		str_format(aFilename, sizeof(aFilename), "videos/%s_%s.mp4", pFilename, aTimestamp);
	}
	else
	{
		str_format(aFilename, sizeof(aFilename), "videos/%s.mp4", pFilename);
	}

	// wait for idle, so there is no data race
	Graphics()->WaitForIdle();
	const int OutputStorageType = ExactFilename && !fs_is_relative_path(aFilename) ? IStorage::TYPE_ABSOLUTE : IStorage::TYPE_SAVE;
	const bool OfflineAudio = SessionId != FocusedSessionId();
	m_pVideo = std::make_unique<CVideo>(Graphics(), Sound(), Storage(), Settings, m_LocalStartTime, aFilename, OutputStorageType, !ExactFilename, !OfflineAudio);
	CDemoPlayer &Player = DemoSource(SessionId).DemoPlayer();
	m_VideoSessionId = SessionId;
	m_VideoOfflineAudio = OfflineAudio;
	Player.SetVideo(m_pVideo.get());
	if(!m_pVideo->Start())
	{
		log_error("videorecorder", "Failed to start recording to '%s'", aFilename);
		Player.Stop("Failed to start video recording. See local console for details.");
		const CVideoExportStatus Status = m_pVideo->Status();
		str_copy(m_aVideoError, Status.m_aError[0] == '\0' ? "Failed to start video recording." : Status.m_aError);
		return m_aVideoError;
	}
	if(Player.Info()->m_Info.m_Paused)
	{
		IVideo::Current()->Pause(true);
	}
	log_info("videorecorder", "Recording to '%s'", aFilename);
	return nullptr;
}

void CClient::Con_StopVideo(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = static_cast<CClient *>(pUserData);
	if(!pSelf->m_pVideo || IVideo::Current() != pSelf->m_pVideo.get())
	{
		log_error("videorecorder", "Not recording.");
		return;
	}

	pSelf->m_pVideo->Stop();
	CDemoPlayer *pPlayer = pSelf->VideoDemoPlayer();
	if(pPlayer && pPlayer->Video() == pSelf->m_pVideo.get())
		pPlayer->SetVideo(nullptr);
	if(pSelf->m_ActiveVideoExport.has_value())
	{
		str_copy(pSelf->m_aVideoExportQueueError, "Video rendering stopped.");
		if(pPlayer)
			pPlayer->Stop(pSelf->m_aVideoExportQueueError);
	}
	log_info("videorecorder", "Stopped recording.");
}

#endif

void CClient::Con_Rcon(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->Rcon(pResult->GetString(0));
}

void CClient::Con_RconAuth(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->RconAuth(pSelf->ActiveConnection(), "", pResult->GetString(0));
}

void CClient::Con_RconLogin(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->RconAuth(pSelf->ActiveConnection(), pResult->GetString(0), pResult->GetString(1));
}

void CClient::Con_BeginFavoriteGroup(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pSelf->m_FavoritesGroup)
	{
		log_error("client", "opening favorites group while there is already one, discarding old one");
		for(int i = 0; i < pSelf->m_FavoritesGroupNum; i++)
		{
			char aAddr[NETADDR_MAXSTRSIZE];
			net_addr_str(&pSelf->m_aFavoritesGroupAddresses[i], aAddr, sizeof(aAddr), true);
			log_warn("client", "discarding %s", aAddr);
		}
	}
	pSelf->m_FavoritesGroup = true;
	pSelf->m_FavoritesGroupAllowPing = false;
	pSelf->m_FavoritesGroupNum = 0;
}

void CClient::Con_EndFavoriteGroup(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(!pSelf->m_FavoritesGroup)
	{
		log_error("client", "closing favorites group while there is none, ignoring");
		return;
	}
	log_info("client", "adding group of %d favorites", pSelf->m_FavoritesGroupNum);
	pSelf->m_pFavorites->Add(pSelf->m_aFavoritesGroupAddresses, pSelf->m_FavoritesGroupNum);
	if(pSelf->m_FavoritesGroupAllowPing)
	{
		pSelf->m_pFavorites->AllowPing(pSelf->m_aFavoritesGroupAddresses, pSelf->m_FavoritesGroupNum, true);
	}
	pSelf->m_FavoritesGroup = false;
}

void CClient::Con_AddFavorite(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	NETADDR Addr;

	if(net_addr_from_url(&Addr, pResult->GetString(0), nullptr, 0) != 0 && net_addr_from_str(&Addr, pResult->GetString(0)) != 0)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "invalid address '%s'", pResult->GetString(0));
		pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "client", aBuf);
		return;
	}
	bool AllowPing = pResult->NumArguments() > 1 && str_find(pResult->GetString(1), "allow_ping");
	char aAddr[NETADDR_MAXSTRSIZE];
	net_addr_str(&Addr, aAddr, sizeof(aAddr), true);
	if(pSelf->m_FavoritesGroup)
	{
		if(pSelf->m_FavoritesGroupNum == (int)std::size(pSelf->m_aFavoritesGroupAddresses))
		{
			log_error("client", "discarding %s because groups can have at most a size of %d", aAddr, pSelf->m_FavoritesGroupNum);
			return;
		}
		log_info("client", "adding %s to favorites group", aAddr);
		pSelf->m_aFavoritesGroupAddresses[pSelf->m_FavoritesGroupNum] = Addr;
		pSelf->m_FavoritesGroupAllowPing = pSelf->m_FavoritesGroupAllowPing || AllowPing;
		pSelf->m_FavoritesGroupNum += 1;
	}
	else
	{
		log_info("client", "adding %s to favorites", aAddr);
		pSelf->m_pFavorites->Add(&Addr, 1);
		if(AllowPing)
		{
			pSelf->m_pFavorites->AllowPing(&Addr, 1, true);
		}
	}
}

void CClient::Con_RemoveFavorite(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	NETADDR Addr;
	if(net_addr_from_str(&Addr, pResult->GetString(0)) == 0)
		pSelf->m_pFavorites->Remove(&Addr, 1);
}

void CClient::DemoSliceBegin()
{
	const CDemoPlayer::CPlaybackInfo *pInfo = DemoPlayer().Info();
	g_Config.m_ClDemoSliceBegin = pInfo->m_Info.m_CurrentTick;
}

void CClient::DemoSliceEnd()
{
	const CDemoPlayer::CPlaybackInfo *pInfo = DemoPlayer().Info();
	g_Config.m_ClDemoSliceEnd = pInfo->m_Info.m_CurrentTick;
}

void CClient::Con_DemoSliceBegin(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->DemoSliceBegin();
}

void CClient::Con_DemoSliceEnd(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->DemoSliceEnd();
}

void CClient::Con_SaveReplay(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pResult->NumArguments())
	{
		int Length = pResult->GetInteger(0);
		if(Length <= 0)
		{
			pSelf->m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", "ERROR: length must be greater than 0 second.");
		}
		else
		{
			if(pResult->NumArguments() >= 2)
				pSelf->SaveReplay(Length, pResult->GetString(1));
			else
				pSelf->SaveReplay(Length);
		}
	}
	else
	{
		pSelf->SaveReplay(g_Config.m_ClReplayLength);
	}
}

void CClient::SaveReplay(const int Length, const char *pFilename)
{
	if(!g_Config.m_ClReplays)
	{
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", "Feature is disabled. Please enable it via configuration.");
		GameClient()->Echo(Localize("Replay feature is disabled!"));
		return;
	}

	if(!DemoRecorder(RECORDER_REPLAYS)->IsRecording())
	{
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", "ERROR: demorecorder isn't recording. Try to rejoin to fix that.");
	}
	else if(DemoRecorder(RECORDER_REPLAYS)->Length() < 1)
	{
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", "ERROR: demorecorder isn't recording for at least 1 second.");
	}
	else
	{
		char aFilename[IO_MAX_PATH_LENGTH];
		if(pFilename[0] == '\0')
		{
			char aTimestamp[20];
			str_timestamp(aTimestamp, sizeof(aTimestamp));
			str_format(aFilename, sizeof(aFilename), "demos/replays/%s_%s_(replay).demo", GameClient()->Map(m_NetworkSessionId)->BaseName(), aTimestamp);
		}
		else
		{
			str_format(aFilename, sizeof(aFilename), "demos/replays/%s.demo", pFilename);
			IOHANDLE Handle = m_pStorage->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
			if(!Handle)
			{
				m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", "ERROR: invalid filename. Try a different one!");
				return;
			}
			io_close(Handle);
			m_pStorage->RemoveFile(aFilename, IStorage::TYPE_SAVE);
		}

		// Stop the recorder to correctly slice the demo after
		DemoRecorder(RECORDER_REPLAYS)->Stop(IDemoRecorder::EStopMode::KEEP_FILE);

		// Slice the demo to get only the last cl_replay_length seconds
		const char *pSrc = DemoRecorder(RECORDER_REPLAYS)->CurrentFilename();
		const int EndTick = GameTick(m_NetworkSessionId, ActiveConnection());
		const int StartTick = EndTick - Length * GameTickSpeed();

		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "replay", "Saving replay...");

		// Create a job to do this slicing in background because it can be a bit long depending on the file size
		std::shared_ptr<CDemoEdit> pDemoEditTask = std::make_shared<CDemoEdit>(GameClient()->NetVersion(), &m_pNetworkSessionSource->SnapshotDelta(false), &m_pNetworkSessionSource->SnapshotDelta(true), m_pStorage, pSrc, aFilename, StartTick, EndTick);
		Engine()->AddJob(pDemoEditTask);
		m_EditJobs.push_back(pDemoEditTask);

		// And we restart the recorder
		DemoRecorder_UpdateReplayRecorder();
	}
}

void CClient::DemoSlice(const char *pDstPath, CLIENTFUNC_FILTER pfnFilter, void *pUser)
{
	if(DemoPlayer().IsPlaying())
	{
		m_DemoEditor.Slice(DemoPlayer().Filename(), pDstPath, g_Config.m_ClDemoSliceBegin, g_Config.m_ClDemoSliceEnd, pfnFilter, pUser);
	}
}

const char *CClient::DemoPlayer_Play(const char *pFilename, int StorageType)
{
#if defined(CONF_VIDEORECORDER)
	if(m_ActiveVideoExport.has_value() && !m_LoadingQueuedVideoExport)
		return "A queued video export is active.";
#endif
	return DemoPlayer_Play(m_DemoSessionId, pFilename, StorageType, true);
}

const char *CClient::DemoPlayer_Play(CSessionId SessionId, const char *pFilename, int StorageType, bool Focus)
{
	// Don't disconnect unless the file exists (only for play command)
	if(!Storage()->FileExists(pFilename, StorageType))
		return Localize("No demo with this filename exists");

	CDemoSessionSource &Source = DemoSource(SessionId);
	CDemoPlayer &Player = Source.DemoPlayer();
	if(Player.IsPlaying() || Source.State() != ESessionState::OFFLINE)
		DisconnectDemoWithReason(SessionId, nullptr);

	if(Focus)
	{
		m_SessionManager.SetFocused(SessionId);
		SetFocusedState(IClient::STATE_LOADING, false);
		GameClient()->OnSessionFocused(SessionId);
		SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_LOADING_DEMO);
		if((bool)m_LoadingCallback)
			m_LoadingCallback(IClient::LOADING_CALLBACK_DETAIL_DEMO);
	}
	else
		Source.SetState(ESessionState::LOADING_MAP);

	// try to start playback
	Player.SetListener(this);
	if(Player.Load(Storage(), m_pConsole, pFilename, StorageType))
	{
		DisconnectDemoWithReason(SessionId, Player.ErrorMessage());
		return Player.ErrorMessage();
	}

	Source.SetSixup(Player.IsSixup());

	// load map
	const CMapInfo *pMapInfo = Player.GetMapInfo();
	const char *pError = LoadMapSearch(SessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc);
	if(pError)
	{
		if(!Player.ExtractMap(Storage()))
		{
			DisconnectDemoWithReason(SessionId, pError);
			return pError;
		}

		pError = LoadMapSearch(SessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc);
		if(pError)
		{
			DisconnectDemoWithReason(SessionId, pError);
			return pError;
		}
	}

	// setup current server info
	CServerInfo &DemoServerInfo = Source.ServerInfo();
	DemoServerInfo = {};
	str_copy(DemoServerInfo.m_aMap, pMapInfo->m_aName);
	DemoServerInfo.m_MapCrc = pMapInfo->m_Crc;
	DemoServerInfo.m_MapSize = pMapInfo->m_Size;

	// enter demo playback state
	if(Focus)
		SetState(IClient::STATE_DEMOPLAYBACK);
	else
		Source.SetState(ESessionState::READY);

	GameClient()->OnConnected(SessionId);

	// setup buffers
	Source.PrepareSnapshots();

	Player.Play();
	GameClient()->OnEnterGame(SessionId);

	return nullptr;
}

#if defined(CONF_VIDEORECORDER)
void CClient::UpdateVideoExportQueue()
{
	if(m_ActiveVideoExport.has_value())
	{
		if(!m_VideoSessionId.IsValid() || DemoSource(m_VideoSessionId).State() != ESessionState::OFFLINE || (m_pVideo && !m_pVideo->IsStopped()))
			return;
		if(m_pVideo)
		{
			const CVideoExportStatus Status = m_pVideo->Status();
			if(Status.m_HasError)
			{
				if(m_aVideoExportQueueError[0] == '\0')
					str_copy(m_aVideoExportQueueError, Status.m_aError[0] == '\0' ? "Video recording failed." : Status.m_aError);
				log_error("videorecorder", "Export of '%s' failed: %s", m_ActiveVideoExport->m_aDemoPath, Status.m_aError);
			}
		}
		m_pVideo.reset();
		m_VideoSessionId = {};
		m_VideoOfflineAudio = false;
		m_ActiveVideoExport.reset();
	}
	if(m_CommandLineVideoExport && !m_ActiveVideoExport.has_value() && m_VideoExportQueue.empty() && m_VideoExportQueueRunning)
	{
		m_VideoExportQueueRunning = false;
		m_CommandLineExitCode = m_aVideoExportQueueError[0] == '\0' ? 0 : 1;
		if(m_CommandLineExitCode == 0)
			log_info("videorecorder", "Export completed: %s", m_aCommandLineVideoPath);
		else
			log_error("videorecorder", "Export failed: %s", m_aVideoExportQueueError);
		Quit();
		return;
	}

	if(!m_VideoExportQueueRunning || m_VideoExportQueue.empty())
	{
		if(m_VideoExportQueue.empty())
			m_VideoExportQueueRunning = false;
		return;
	}
	if(m_VideoExportQueue.front().m_Settings.m_Audio && !GameClient()->IsSoundReady())
		return;

	m_ActiveVideoExport = m_VideoExportQueue.front();
	m_VideoExportQueue.pop_front();
	const CVideoExportJob &Job = *m_ActiveVideoExport;
	const CSessionId SessionId = m_VideoExportSessionId;
	m_VideoSessionId = SessionId;
	m_VideoOfflineAudio = true;
	m_LoadingQueuedVideoExport = true;
	const char *pError = DemoPlayer_Play(SessionId, Job.m_aDemoPath, Job.m_StorageType, false);
	m_LoadingQueuedVideoExport = false;
	if(!pError)
		pError = StartVideo(SessionId, Job.m_aVideoName, false, Job.m_Settings, Job.m_ExactVideoPath);
	if(pError)
	{
		if(m_aVideoExportQueueError[0] == '\0')
			str_copy(m_aVideoExportQueueError, pError);
		log_error("videorecorder", "Could not start queued export '%s': %s", Job.m_aDemoPath, pError);
		DisconnectDemoWithReason(SessionId, pError);
		return;
	}
	CDemoPlayer &Player = DemoSource(SessionId).DemoPlayer();
	Player.SetSpeedIndex(Job.m_SpeedIndex);
}

const char *CClient::QueueVideoExport(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue, bool ExactVideoPath)
{
	if(IVideo::Current() && !m_ActiveVideoExport.has_value())
		return "Already recording.";
	if(!pFilename[0] || !pVideoName[0])
		return "Demo and video names must not be empty.";
	if(Settings.m_Width < 2 || Settings.m_Height < 2 || Settings.m_Width > 8192 || Settings.m_Height > 8192 || static_cast<int64_t>(Settings.m_Width) * Settings.m_Height > 8192LL * 4320 || Settings.m_Width % 2 != 0 || Settings.m_Height % 2 != 0)
		return "Invalid video resolution.";

	CVideoExportJob Job;
	str_copy(Job.m_aDemoPath, pFilename);
	Job.m_StorageType = StorageType;
	str_copy(Job.m_aVideoName, pVideoName);
	Job.m_Settings = Settings;
	Job.m_SpeedIndex = SpeedIndex;
	Job.m_ExactVideoPath = ExactVideoPath;
	auto UsesVideoName = [&Job](const CVideoExportJob &Other) { return str_comp(Other.m_aVideoName, Job.m_aVideoName) == 0; };
	if((m_ActiveVideoExport.has_value() && UsesVideoName(*m_ActiveVideoExport)) || std::ranges::any_of(m_VideoExportQueue, UsesVideoName))
		return "A video with this name is already queued.";
	if(!m_ActiveVideoExport.has_value() && m_VideoExportQueue.empty())
		m_aVideoExportQueueError[0] = '\0';
	m_VideoExportQueue.push_back(Job);
	if(StartQueue)
		m_VideoExportQueueRunning = true;
	return nullptr;
}

const char *CClient::DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue)
{
	return QueueVideoExport(pFilename, StorageType, pVideoName, Settings, SpeedIndex, StartQueue, false);
}

void CClient::ConfigureCommandLineVideoExport(const char *pDemoPath, const char *pVideoPath, const CVideoExportSettings &Settings)
{
	m_CommandLineVideoExport = true;
	m_CommandLineExitCode = 1;
	m_HiddenWindow = true;
	str_copy(m_aCommandLineDemoPath, pDemoPath);
	str_copy(m_aCommandLineVideoPath, pVideoPath);
	m_CommandLineVideoSettings = Settings;
}
#endif

void CClient::Con_Play(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->HandleDemoPath(pResult->GetString(0));
}

void CClient::Con_DemoPlay(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pSelf->DemoPlayer().IsPlaying())
	{
		if(pSelf->DemoPlayer().BaseInfo()->m_Paused)
		{
			pSelf->DemoPlayer().Unpause();
		}
		else
		{
			pSelf->DemoPlayer().Pause();
		}
	}
}

void CClient::Con_DemoSpeed(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->DemoPlayer().SetSpeed(pResult->GetFloat(0));
}

void CClient::DemoRecorder_Start(const char *pFilename, bool WithTimestamp, int Recorder)
{
	dbg_assert(IsOnline(), "Client must be online to record demo");

	char aFilename[IO_MAX_PATH_LENGTH];
	if(WithTimestamp)
	{
		char aTimestamp[20];
		str_timestamp(aTimestamp, sizeof(aTimestamp));
		str_format(aFilename, sizeof(aFilename), "demos/%s_%s.demo", pFilename, aTimestamp);
	}
	else
	{
		str_format(aFilename, sizeof(aFilename), "demos/%s.demo", pFilename);
	}

	DemoRecorders()[Recorder].Start(
		Storage(),
		m_pConsole,
		aFilename,
		IsSixup(m_NetworkSessionId) ? GameClient()->NetVersion7() : GameClient()->NetVersion(),
		GameClient()->Map(m_NetworkSessionId)->BaseName(),
		GameClient()->Map(m_NetworkSessionId)->Sha256(),
		GameClient()->Map(m_NetworkSessionId)->Crc(),
		"client",
		GameClient()->Map(m_NetworkSessionId)->Size(),
		nullptr,
		GameClient()->Map(m_NetworkSessionId)->File(),
		nullptr,
		nullptr);
}

void CClient::DemoRecorder_HandleAutoStart()
{
	if(State() != IClient::STATE_ONLINE)
	{
		return;
	}

	if(g_Config.m_ClAutoDemoRecord)
	{
		DemoRecorder(RECORDER_AUTO)->Stop(IDemoRecorder::EStopMode::KEEP_FILE);

		char aFilename[IO_MAX_PATH_LENGTH];
		str_format(aFilename, sizeof(aFilename), "auto/%s", GameClient()->Map(m_NetworkSessionId)->BaseName());
		DemoRecorder_Start(aFilename, true, RECORDER_AUTO);

		if(g_Config.m_ClAutoDemoMax)
		{
			// clean up auto recorded demos
			CFileCollection AutoDemos;
			AutoDemos.Init(Storage(), "demos/auto", "" /* empty for wild card */, ".demo", g_Config.m_ClAutoDemoMax);
		}
	}

	DemoRecorder_UpdateReplayRecorder();
}

void CClient::DemoRecorder_UpdateReplayRecorder()
{
	if(!g_Config.m_ClReplays && DemoRecorder(RECORDER_REPLAYS)->IsRecording())
	{
		DemoRecorder(RECORDER_REPLAYS)->Stop(IDemoRecorder::EStopMode::REMOVE_FILE);
	}

	if(g_Config.m_ClReplays && !DemoRecorder(RECORDER_REPLAYS)->IsRecording())
	{
		char aFilename[IO_MAX_PATH_LENGTH];
		str_format(aFilename, sizeof(aFilename), "replays/replay_tmp_%s", GameClient()->Map(m_NetworkSessionId)->BaseName());
		DemoRecorder_Start(aFilename, true, RECORDER_REPLAYS);
	}
}

void CClient::DemoRecorder_AddDemoMarker(int Recorder)
{
	DemoRecorders()[Recorder].AddDemoMarker();
}

CDemoRecorder (&CClient::DemoRecorders())[RECORDER_MAX]
{
	if(IsSixup(m_NetworkSessionId))
	{
		return m_aDemoRecordersSixup;
	}
	return m_aDemoRecorders;
}

IDemoRecorder *CClient::DemoRecorder(int Recorder)
{
	return &DemoRecorders()[Recorder];
}

void CClient::Con_Record(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;

	if(!pSelf->IsOnline())
	{
		log_error("demo_recorder", "Client is not online.");
		return;
	}
	if(pSelf->DemoRecorder(RECORDER_MANUAL)->IsRecording())
	{
		log_error("demo_recorder", "Demo recorder already recording to '%s'.", pSelf->DemoRecorder(RECORDER_MANUAL)->CurrentFilename());
		return;
	}

	if(pResult->NumArguments())
		pSelf->DemoRecorder_Start(pResult->GetString(0), false, RECORDER_MANUAL);
	else
		pSelf->DemoRecorder_Start(pSelf->GameClient()->Map(pSelf->m_NetworkSessionId)->BaseName(), true, RECORDER_MANUAL);
}

void CClient::Con_StopRecord(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pSelf->DemoRecorder(RECORDER_MANUAL)->Stop(IDemoRecorder::EStopMode::KEEP_FILE);
}

void CClient::Con_AddDemoMarker(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	for(int Recorder = 0; Recorder < RECORDER_MAX; Recorder++)
		pSelf->DemoRecorder_AddDemoMarker(Recorder);
}

void CClient::Con_BenchmarkQuit(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	int Seconds = pResult->GetInteger(0);
	const char *pFilename = pResult->GetString(1);
	pSelf->BenchmarkQuit(Seconds, pFilename);
}

void CClient::BenchmarkQuit(int Seconds, const char *pFilename)
{
	m_BenchmarkFile = Storage()->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
	m_BenchmarkStopTime = time_get() + time_freq() * Seconds;
}

void CClient::UpdateAndSwap()
{
	Input()->Update();
#if defined(CONF_VIDEORECORDER)
	if(m_CommandLineVideoExport)
		return;
#endif
	Graphics()->Swap();
	Graphics()->Clear(0, 0, 0);
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}

void CClient::ServerBrowserUpdate()
{
	m_ServerBrowser.RequestResort();
}

void CClient::ConchainServerBrowserUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CClient *)pUserData)->ServerBrowserUpdate();
}

void CClient::InitChecksum()
{
	CChecksumData *pData = &m_Checksum.m_Data;
	pData->m_SizeofData = sizeof(*pData);
	str_copy(pData->m_aVersionStr, GAME_NAME " " GAME_RELEASE_VERSION " (" CONF_PLATFORM_STRING "; " CONF_ARCH_STRING ")");
	pData->m_Start = time_get();
	os_version_str(pData->m_aOsVersion, sizeof(pData->m_aOsVersion));
	secure_random_fill(&pData->m_Random, sizeof(pData->m_Random));
	pData->m_Version = GameClient()->DDNetVersion();
	pData->m_SizeofClient = sizeof(*this);
	pData->m_SizeofConfig = sizeof(pData->m_Config);
	pData->InitFiles();
}

#ifndef DDNET_CHECKSUM_SALT
// salt@checksum.ddnet.tw: db877f2b-2ddb-3ba6-9f67-a6d169ec671d
#define DDNET_CHECKSUM_SALT \
	{ \
		{ \
			0xdb, 0x87, 0x7f, 0x2b, 0x2d, 0xdb, 0x3b, 0xa6, \
				0x9f, 0x67, 0xa6, 0xd1, 0x69, 0xec, 0x67, 0x1d, \
		} \
	}
#endif

int CClient::HandleChecksum(CSessionId SessionId, CStreamId StreamId, CUuid Uuid, CUnpacker *pUnpacker)
{
	int Start = pUnpacker->GetInt();
	int Length = pUnpacker->GetInt();
	if(pUnpacker->Error())
	{
		return 1;
	}
	if(Start < 0 || Length < 0 || Start > std::numeric_limits<int>::max() - Length)
	{
		return 2;
	}
	int End = Start + Length;
	int ChecksumBytesEnd = std::min(End, (int)sizeof(m_Checksum.m_aBytes));
	int FileStart = std::max(Start, (int)sizeof(m_Checksum.m_aBytes));
	unsigned char aStartBytes[sizeof(int32_t)];
	unsigned char aEndBytes[sizeof(int32_t)];
	uint_to_bytes_be(aStartBytes, Start);
	uint_to_bytes_be(aEndBytes, End);

	if(Start <= (int)sizeof(m_Checksum.m_aBytes))
	{
		mem_zero(&m_Checksum.m_Data.m_Config, sizeof(m_Checksum.m_Data.m_Config));
#define CHECKSUM_RECORD(Flags) (((Flags) & CFGFLAG_CLIENT) == 0 || ((Flags) & CFGFLAG_INSENSITIVE) != 0)
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) \
	if(CHECKSUM_RECORD(Flags)) \
	{ \
		m_Checksum.m_Data.m_Config.m_##Name = g_Config.m_##Name; \
	}
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc) \
	if(CHECKSUM_RECORD(Flags)) \
	{ \
		m_Checksum.m_Data.m_Config.m_##Name = g_Config.m_##Name; \
	}
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc) \
	if(CHECKSUM_RECORD(Flags)) \
	{ \
		str_copy(m_Checksum.m_Data.m_Config.m_##Name, g_Config.m_##Name); \
	}
#include <engine/shared/config_variables.h>
#undef CHECKSUM_RECORD
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR
	}
	if(End > (int)sizeof(m_Checksum.m_aBytes))
	{
		if(m_OwnExecutableSize == 0)
		{
			m_OwnExecutable = io_current_exe();
			// io_length returns -1 on error.
			m_OwnExecutableSize = m_OwnExecutable ? io_length(m_OwnExecutable) : -1;
		}
		// Own executable not available.
		if(m_OwnExecutableSize < 0)
		{
			return 3;
		}
		if(End - (int)sizeof(m_Checksum.m_aBytes) > m_OwnExecutableSize)
		{
			return 4;
		}
	}

	SHA256_CTX Sha256Ctxt;
	sha256_init(&Sha256Ctxt);
	CUuid Salt = DDNET_CHECKSUM_SALT;
	sha256_update(&Sha256Ctxt, &Salt, sizeof(Salt));
	sha256_update(&Sha256Ctxt, &Uuid, sizeof(Uuid));
	sha256_update(&Sha256Ctxt, aStartBytes, sizeof(aStartBytes));
	sha256_update(&Sha256Ctxt, aEndBytes, sizeof(aEndBytes));
	if(Start < (int)sizeof(m_Checksum.m_aBytes))
	{
		sha256_update(&Sha256Ctxt, m_Checksum.m_aBytes + Start, ChecksumBytesEnd - Start);
	}
	if(End > (int)sizeof(m_Checksum.m_aBytes))
	{
		unsigned char aBuf[2048];
		if(io_seek(m_OwnExecutable, FileStart - sizeof(m_Checksum.m_aBytes), EIoSeekOrigin::START))
		{
			return 5;
		}
		for(int i = FileStart; i < End; i += sizeof(aBuf))
		{
			int Read = io_read(m_OwnExecutable, aBuf, std::min((int)sizeof(aBuf), End - i));
			sha256_update(&Sha256Ctxt, aBuf, Read);
		}
	}
	SHA256_DIGEST Sha256 = sha256_finish(&Sha256Ctxt);

	CMsgPacker Msg(NETMSG_CHECKSUM_RESPONSE, true);
	Msg.AddRaw(&Uuid, sizeof(Uuid));
	Msg.AddRaw(&Sha256, sizeof(Sha256));
	SendMsg(SessionId, StreamId, &Msg, MSGFLAG_VITAL);

	return 0;
}

void CClient::ConchainWindowScreen(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pSelf->Graphics() && pResult->NumArguments())
	{
		if(g_Config.m_GfxScreen != pResult->GetInteger(0))
			pSelf->Graphics()->SwitchWindowScreen(pResult->GetInteger(0), true);
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

void CClient::ConchainFullscreen(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pSelf->Graphics() && pResult->NumArguments())
	{
		if(g_Config.m_GfxFullscreen != pResult->GetInteger(0))
			pSelf->Graphics()->SetWindowParams(pResult->GetInteger(0), g_Config.m_GfxBorderless);
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

void CClient::ConchainWindowBordered(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pSelf->Graphics() && pResult->NumArguments())
	{
		if(!g_Config.m_GfxFullscreen && (g_Config.m_GfxBorderless != pResult->GetInteger(0)))
			pSelf->Graphics()->SetWindowParams(g_Config.m_GfxFullscreen, !g_Config.m_GfxBorderless);
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

void CClient::Notify(const char *pTitle, const char *pMessage)
{
	if(m_pGraphics->WindowActive() || !g_Config.m_ClShowNotifications)
		return;

	Notifications()->Notify(pTitle, pMessage);
	Graphics()->NotifyWindow();
}

void CClient::OnWindowResize()
{
	GameClient()->OnWindowResize();
	m_pEditor->OnWindowResize();
	TextRender()->OnWindowResize();
}

void CClient::ConchainWindowVSync(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	if(pSelf->Graphics() && pResult->NumArguments())
	{
		if(g_Config.m_GfxVsync != pResult->GetInteger(0))
			pSelf->Graphics()->SetVSync(pResult->GetInteger(0));
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

void CClient::ConchainWindowResize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pSelf->Graphics() && pResult->NumArguments())
	{
		pSelf->Graphics()->ResizeToScreen();
	}
}

void CClient::ConchainTimeoutSeed(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		pSelf->m_GenerateTimeoutSeed = false;
}

void CClient::ConchainPassword(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pSelf->m_LocalStartTime) //won't set m_pNetworkSessionSource->m_SendPassword before game has started
		pSelf->m_pNetworkSessionSource->m_SendPassword = true;
}

void CClient::ConchainReplays(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pSelf->IsOnline())
	{
		pSelf->DemoRecorder_UpdateReplayRecorder();
	}
}

void CClient::ConchainInputFifo(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pSelf->m_Fifo.IsInit())
	{
		pSelf->m_Fifo.Shutdown();
		pSelf->m_Fifo.Init(pSelf->m_pConsole, pSelf->Config()->m_ClInputFifo, CFGFLAG_CLIENT);
	}
}

void CClient::ConchainNetReset(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		pSelf->ResetSocket();
}

void CClient::ConchainLoglevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		pSelf->m_pFileLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_Loglevel)});
	}
}

void CClient::ConchainStdoutOutputLevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CClient *pSelf = (CClient *)pUserData;
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pSelf->m_pStdoutLogger)
	{
		pSelf->m_pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_StdoutOutputLevel)});
	}
}

void CClient::RegisterCommands()
{
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	m_pConsole->Register("dummy_connect", "", CFGFLAG_CLIENT, Con_DummyConnect, this, "Connect dummy");
	m_pConsole->Register("dummy_disconnect", "", CFGFLAG_CLIENT, Con_DummyDisconnect, this, "Disconnect dummy");
	m_pConsole->Register("dummy_reset", "", CFGFLAG_CLIENT, Con_DummyResetInput, this, "Reset dummy");

	m_pConsole->Register("quit", "", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_Quit, this, "Quit the client");
	m_pConsole->Register("exit", "", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_Quit, this, "Quit the client");
	m_pConsole->Register("restart", "", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_Restart, this, "Restart the client");
	m_pConsole->Register("minimize", "", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_Minimize, this, "Minimize the client");
	m_pConsole->Register("connect", "r[host|ip]", CFGFLAG_CLIENT, Con_Connect, this, "Connect to the specified host/ip");
	m_pConsole->Register("dbg_connect_session", "r[host|ip]", CFGFLAG_CLIENT, Con_DbgConnectSession, this, "Connect an additional Network session");
	m_pConsole->Register("dbg_connect_stream", "i[session]", CFGFLAG_CLIENT, Con_DbgConnectStream, this, "Connect an additional stream for a Network session");
	m_pConsole->Register("dbg_destroy_stream", "i[session] i[stream]", CFGFLAG_CLIENT, Con_DbgDestroyStream, this, "Destroy an additional Network stream");
	m_pConsole->Register("dbg_destroy_session", "i[session]", CFGFLAG_CLIENT, Con_DbgDestroySession, this, "Destroy an additional Network session");
	m_pConsole->Register("dbg_dump_sessions", "", CFGFLAG_CLIENT, Con_DbgDumpSessions, this, "Print game session and stream ticks");
	m_pConsole->Register("disconnect", "", CFGFLAG_CLIENT, Con_Disconnect, this, "Disconnect from the server");
	m_pConsole->Register("ping", "", CFGFLAG_CLIENT, Con_Ping, this, "Ping the current server");
	m_pConsole->Register("screenshot", "", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_Screenshot, this, "Take a screenshot");
	m_pConsole->Register("net_reset", "", CFGFLAG_CLIENT, ConNetReset, this, "Rebinds the client's listening address and port");
	m_pConsole->Register("quic_reconnect", "", CFGFLAG_CLIENT, Con_QuicReconnect, this, "Reconnect the active QUIC transport using application resume");
	m_pConsole->Register("quic_known_host", "s[host] i[port] s[sha256]", CFGFLAG_CLIENT, Con_QuicKnownHost, this, "Remember a verified QUIC server identity");
	m_pConsole->Register("quic_forget_host", "s[host] ?i[port]", CFGFLAG_CLIENT, Con_QuicForgetHost, this, "Forget a trusted QUIC server identity");

#if defined(CONF_VIDEORECORDER)
	m_pConsole->Register("start_video", "?r[file]", CFGFLAG_CLIENT, Con_StartVideo, this, "Start recording a video");
	m_pConsole->Register("stop_video", "", CFGFLAG_CLIENT, Con_StopVideo, this, "Stop recording a video");
#endif

	m_pConsole->Register("rcon", "r[rcon-command]", CFGFLAG_CLIENT, Con_Rcon, this, "Send specified command to rcon");
	m_pConsole->Register("rcon_auth", "r[password]", CFGFLAG_CLIENT, Con_RconAuth, this, "Authenticate to rcon");
	m_pConsole->Register("rcon_login", "s[username] r[password]", CFGFLAG_CLIENT, Con_RconLogin, this, "Authenticate to rcon with a username");
	m_pConsole->Register("play", "r[file]", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_Play, this, "Play back a demo");
	m_pConsole->Register("record", "?r[file]", CFGFLAG_CLIENT, Con_Record, this, "Start recording a demo");
	m_pConsole->Register("stoprecord", "", CFGFLAG_CLIENT, Con_StopRecord, this, "Stop recording a demo");
	m_pConsole->Register("add_demomarker", "", CFGFLAG_CLIENT, Con_AddDemoMarker, this, "Add demo timeline marker");
	m_pConsole->Register("begin_favorite_group", "", CFGFLAG_CLIENT, Con_BeginFavoriteGroup, this, "Use this before `add_favorite` to group favorites. End with `end_favorite_group`");
	m_pConsole->Register("end_favorite_group", "", CFGFLAG_CLIENT, Con_EndFavoriteGroup, this, "Use this after `add_favorite` to group favorites. Start with `begin_favorite_group`");
	m_pConsole->Register("add_favorite", "s[host|ip] ?s['allow_ping']", CFGFLAG_CLIENT, Con_AddFavorite, this, "Add a server as a favorite");
	m_pConsole->Register("remove_favorite", "r[host|ip]", CFGFLAG_CLIENT, Con_RemoveFavorite, this, "Remove a server from favorites");
	m_pConsole->Register("demo_slice_start", "", CFGFLAG_CLIENT, Con_DemoSliceBegin, this, "Mark the beginning of a demo cut");
	m_pConsole->Register("demo_slice_end", "", CFGFLAG_CLIENT, Con_DemoSliceEnd, this, "Mark the end of a demo cut");
	m_pConsole->Register("demo_play", "", CFGFLAG_CLIENT, Con_DemoPlay, this, "Play/pause the current demo");
	m_pConsole->Register("demo_speed", "f[speed]", CFGFLAG_CLIENT, Con_DemoSpeed, this, "Set current demo speed");

	m_pConsole->Register("save_replay", "?i[length] ?r[filename]", CFGFLAG_CLIENT, Con_SaveReplay, this, "Save a replay of the last defined amount of seconds");
	m_pConsole->Register("benchmark_quit", "i[seconds] r[file]", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_BenchmarkQuit, this, "Benchmark frame times for number of seconds to file, then quit");

	RustVersionRegister(*m_pConsole);

	m_pConsole->Chain("cl_timeout_seed", ConchainTimeoutSeed, this);
	m_pConsole->Chain("cl_replays", ConchainReplays, this);
	m_pConsole->Chain("cl_input_fifo", ConchainInputFifo, this);
	m_pConsole->Chain("cl_port", ConchainNetReset, this);
	m_pConsole->Chain("cl_dummy_port", ConchainNetReset, this);
	m_pConsole->Chain("cl_contact_port", ConchainNetReset, this);
	m_pConsole->Chain("bindaddr", ConchainNetReset, this);

	m_pConsole->Chain("password", ConchainPassword, this);

	// used for server browser update
	m_pConsole->Chain("br_filter_string", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_exclude_string", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_full", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_empty", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_spectators", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_friends", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_country", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_country_index", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_pw", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_gametype", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_gametype_strict", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_connecting_players", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_serveraddress", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_unfinished_map", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("br_filter_login", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("add_favorite", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("remove_favorite", ConchainServerBrowserUpdate, this);
	m_pConsole->Chain("end_favorite_group", ConchainServerBrowserUpdate, this);

	m_pConsole->Chain("gfx_screen", ConchainWindowScreen, this);
	m_pConsole->Chain("gfx_screen_width", ConchainWindowResize, this);
	m_pConsole->Chain("gfx_screen_height", ConchainWindowResize, this);
	m_pConsole->Chain("gfx_screen_refresh_rate", ConchainWindowResize, this);
	m_pConsole->Chain("gfx_fullscreen", ConchainFullscreen, this);
	m_pConsole->Chain("gfx_borderless", ConchainWindowBordered, this);
	m_pConsole->Chain("gfx_vsync", ConchainWindowVSync, this);

	m_pConsole->Chain("loglevel", ConchainLoglevel, this);
	m_pConsole->Chain("stdout_output_level", ConchainStdoutOutputLevel, this);
}

static CClient *CreateClient()
{
	return new CClient;
}

void CClient::HandleConnectAddress(const NETADDR *pAddr)
{
	net_addr_str(pAddr, m_aCmdConnect, sizeof(m_aCmdConnect), true);
}

void CClient::HandleConnectLink(const char *pLink)
{
	// Chrome works fine with ddnet:// but not with ddnet:
	// Check ddnet:// before ddnet: because we don't want the // as part of connect command
	const char *pConnectLink = nullptr;
	if((pConnectLink = str_startswith(pLink, CONNECTLINK_DOUBLE_SLASH)))
		str_copy(m_aCmdConnect, pConnectLink);
	else if((pConnectLink = str_startswith(pLink, CONNECTLINK_NO_SLASH)))
		str_copy(m_aCmdConnect, pConnectLink);
	else
		str_copy(m_aCmdConnect, pLink);
	// Edge appends / to the URL
	const int Length = str_length(m_aCmdConnect);
	if(m_aCmdConnect[Length - 1] == '/')
		m_aCmdConnect[Length - 1] = '\0';
}

void CClient::HandleDemoPath(const char *pPath)
{
	str_copy(m_aCmdPlayDemo, pPath);
}

void CClient::HandleMapPath(const char *pPath)
{
	str_copy(m_aCmdEditMap, pPath);
}

static bool UnknownArgumentCallback(const char *pCommand, void *pUser)
{
	CClient *pClient = static_cast<CClient *>(pUser);
	if(str_startswith(pCommand, CONNECTLINK_NO_SLASH) || str_startswith(pCommand, QUIC_CONNECTLINK_DOUBLE_SLASH) || str_startswith(pCommand, QUIC_CONNECTLINK7_DOUBLE_SLASH) || str_startswith(pCommand, WT_CONNECTLINK_DOUBLE_SLASH) || str_startswith(pCommand, WT_CONNECTLINK7_DOUBLE_SLASH))
	{
		pClient->HandleConnectLink(pCommand);
		return true;
	}
	else if(str_endswith(pCommand, ".demo"))
	{
		pClient->HandleDemoPath(pCommand);
		return true;
	}
	else if(str_endswith(pCommand, ".map"))
	{
		pClient->HandleMapPath(pCommand);
		return true;
	}
	return false;
}

static bool SaveUnknownCommandCallback(const char *pCommand, void *pUser)
{
	CClient *pClient = static_cast<CClient *>(pUser);
	pClient->ConfigManager()->StoreUnknownCommand(pCommand);
	return true;
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
extern "C" {

// This will be called from Emscripten JS code
void EmscriptenCallbackQuitForce()
{
	emscripten_force_exit(-1);
}
}
#endif

/*
	Server Time
	Client Mirror Time
	Client Predicted Time

	Snapshot Latency
		Downstream latency

	Prediction Latency
		Upstream latency
*/

#if defined(CONF_PLATFORM_MACOS)
extern "C" int TWMain(int argc, const char **argv)
#elif defined(CONF_PLATFORM_ANDROID)
static int gs_AndroidStarted = false;
extern "C" [[gnu::visibility("default")]] int SDL_main(int argc, char *argv[]);
int SDL_main(int argc, char *argv2[])
#else
int main(int argc, const char **argv)
#endif
{
	const int64_t MainStart = time_get();

#if defined(CONF_PLATFORM_ANDROID)
	const char **argv = const_cast<const char **>(argv2);
	// Android might not unload the library from memory, causing globals like gs_AndroidStarted
	// not to be initialized correctly when starting the app again.
	if(gs_AndroidStarted)
	{
		ShowMessageBoxWithoutGraphics({.m_pTitle = "Android Error", .m_pMessage = "The app was started, but not closed properly, this causes bugs. Please restart or manually close this task."});
		std::exit(0);
	}
	gs_AndroidStarted = true;
#elif defined(CONF_FAMILY_WINDOWS)
	CWindowsComLifecycle WindowsComLifecycle(true);
#endif
	CCmdlineFix CmdlineFix(&argc, &argv);
	bool CommandLineVideoExportRequested = false;
	for(int Argument = 1; Argument < argc; ++Argument)
	{
		if(str_comp(argv[Argument], "--render-demo") == 0)
		{
			CommandLineVideoExportRequested = true;
			break;
		}
	}

	std::vector<std::shared_ptr<ILogger>> vpLoggers;
	std::shared_ptr<ILogger> pStdoutLogger = nullptr;
#if defined(CONF_PLATFORM_ANDROID)
	pStdoutLogger = std::shared_ptr<ILogger>(log_logger_android());
#else
	bool Silent = false;
	for(int i = 1; i < argc; i++)
	{
		if(str_comp("-s", argv[i]) == 0 || str_comp("--silent", argv[i]) == 0)
		{
			Silent = true;
		}
	}
	if(!Silent)
	{
		pStdoutLogger = std::shared_ptr<ILogger>(log_logger_stdout());
	}
#endif
	if(pStdoutLogger)
	{
		vpLoggers.push_back(pStdoutLogger);
	}
	std::shared_ptr<CFutureLogger> pFutureFileLogger = std::make_shared<CFutureLogger>();
	vpLoggers.push_back(pFutureFileLogger);
	std::shared_ptr<CFutureLogger> pFutureConsoleLogger = std::make_shared<CFutureLogger>();
	vpLoggers.push_back(pFutureConsoleLogger);
	std::shared_ptr<CFutureLogger> pFutureAssertionLogger = std::make_shared<CFutureLogger>();
	vpLoggers.push_back(pFutureAssertionLogger);
	log_set_global_logger(log_logger_collection(std::move(vpLoggers)).release());

#if defined(CONF_DEMO_RENDER_TOOL)
	if(!CommandLineVideoExportRequested)
	{
		log_error("videorecorder", "Usage: ddnet-demo-render --render-demo <demo> --output <video.mp4> [--width <even>] [--height <even>] [--fps <1-1000>] [--crf <0-51>] [--preset <0-9>] [--no-audio]");
		return -1;
	}
#endif

#if defined(CONF_PLATFORM_ANDROID)
	// Initialize Android after logger is available
	const char *pAndroidInitError = InitAndroid();
	if(pAndroidInitError != nullptr)
	{
		log_error("android", "%s", pAndroidInitError);
		ShowMessageBoxWithoutGraphics({.m_pTitle = "Android Error", .m_pMessage = pAndroidInitError});
		std::exit(0);
	}
#endif

	std::stack<std::function<void()>> CleanerFunctions;
	std::function<void()> PerformCleanup = [&CleanerFunctions]() mutable {
		while(!CleanerFunctions.empty())
		{
			CleanerFunctions.top()();
			CleanerFunctions.pop();
		}
	};
	std::function<void()> PerformFinalCleanup = []() {
#if defined(CONF_PLATFORM_ANDROID)
		// Forcefully terminate the entire process, to ensure that static variables
		// will be initialized correctly when the app is started again after quitting.
		// Returning from the main function is not enough, as this only results in the
		// native thread terminating, but the Java thread will continue. Java does not
		// support unloading libraries once they have been loaded, so all static
		// variables will not have their expected initial values anymore when the app
		// is started again after quitting. The variable gs_AndroidStarted above is
		// used to check that static variables have been initialized properly.
		// TODO: This is not the correct way to close an activity on Android, as it
		//       ignores the activity lifecycle entirely, which may cause issues if
		//       we ever used any global resources like the camera.
		std::exit(0);
#elif defined(CONF_PLATFORM_EMSCRIPTEN)
		// We cannot use atexit with Emscripten so we finish the global logger here.
		// See comment in the log_set_global_logger function for details.
		log_global_logger_finish();
#endif
	};
	std::function<void()> PerformAllCleanup = [PerformCleanup, PerformFinalCleanup]() mutable {
		PerformCleanup();
		PerformFinalCleanup();
	};

	// Register SDL for cleanup before creating the kernel and client,
	// so SDL is shutdown after kernel and client. Otherwise the client
	// may crash when shutting down after SDL is already shutdown.
#if !defined(CONF_DEMO_RENDER_TOOL)
	CleanerFunctions.emplace([]() { SDL_Quit(); });
#endif

	CClient *pClient = CreateClient();
	pClient->SetLoggers(pFutureFileLogger, std::move(pStdoutLogger));

	IKernel *pKernel = IKernel::Create();
	pKernel->RegisterInterface(pClient, false);
	pClient->RegisterInterfaces();
	CleanerFunctions.emplace([pKernel, pClient]() {
		// Ensure that the assert handler doesn't use the client/graphics after they've been destroyed
		dbg_assert_set_handler(nullptr);
		pKernel->Shutdown();
		delete pKernel;
		delete pClient;
	});

	const std::thread::id MainThreadId = std::this_thread::get_id();
	dbg_assert_set_handler([MainThreadId, pClient, CommandLineVideoExportRequested](const char *pMsg) {
		if(MainThreadId != std::this_thread::get_id())
			return;

		const char *pGraphicsError = pClient->Graphics() == nullptr ? "" : pClient->Graphics()->GetFatalError();
		const bool GotGraphicsError = pGraphicsError[0] != '\0';
		const char *pTitle;
		const char *pPreamble;
		const char *pPostamble;
		if(GotGraphicsError)
		{
			pTitle = "Graphics Error";
			pPreamble =
				"A graphics error occurred. Please see details and instructions below.\n\n";
			pPostamble =
				"For detailed troubleshooting instructions please read our Wiki:\n"
				"https://wiki.ddnet.org/wiki/GFX_Troubleshooting\n\n"
				"If this did not resolve the issue, please take a screenshot and report this error.\n"
				"Please also share the assert log"
#if defined(CONF_CRASHDUMP)
				" and crash log"
#endif
				" found in the 'dumps' folder in your config directory.\n\n";
			// This is more human readable and we don't care about the source location here,
			// because all graphics assertions come from CGraphicsBackend_Threaded::ProcessError
			// and the original message is also logged separately by the assertion system.
			pMsg = pGraphicsError;
		}
		else
		{
			pTitle = "Assertion Error";
			pPreamble =
				"An assertion error occurred. Please take a screenshot and report this error.\n"
				"Please also share the assert log"
#if defined(CONF_CRASHDUMP)
				" and crash log"
#endif
				" found in the 'dumps' folder in your config directory.\n\n";
			pPostamble = "";
		}

		char aOsVersionString[128];
		if(!os_version_str(aOsVersionString, sizeof(aOsVersionString)))
		{
			str_copy(aOsVersionString, "unknown");
		}

		char aGpuInfo[512];
		pClient->GetGpuInfoString(aGpuInfo);

		char aMessage[2048];
		str_format(aMessage, sizeof(aMessage),
			"%s"
			"%s\n\n"
			"%s"
			"Platform: %s (%s)\n"
			"Configuration: base"
#if defined(CONF_AUTOUPDATE)
			" + autoupdate"
#endif
#if defined(CONF_CRASHDUMP)
			" + crashdump"
#endif
#if defined(CONF_DEBUG)
			" + debug"
#endif
#if defined(CONF_DISCORD)
			" + discord"
#endif
#if defined(CONF_VIDEORECORDER)
			" + videorecorder"
#endif
#if defined(CONF_WEBSOCKETS)
			" + websockets"
#endif
			"\n"
			"Game version: %s %s %s\n"
			"OS version: %s\n\n"
			"%s", // GPU info
			pPreamble,
			pMsg,
			pPostamble,
			CONF_PLATFORM_STRING, CONF_ARCH_ENDIAN_STRING,
			GAME_NAME, GAME_RELEASE_VERSION, GIT_SHORTREV_HASH != nullptr ? GIT_SHORTREV_HASH : "",
			aOsVersionString,
			aGpuInfo);
		// Also log all of this information to the assertion log file
		log_error("assertion", "%s", aMessage);
		std::vector<IGraphics::CMessageBoxButton> vButtons;
		if(GotGraphicsError)
		{
			vButtons.push_back({.m_pLabel = "Show Wiki"});
		}
		// Storage may not have been initialized yet and viewing files is not supported on Android yet
#if !defined(CONF_PLATFORM_ANDROID)
		if(pClient->Storage() != nullptr)
		{
			vButtons.push_back({.m_pLabel = "Show dumps"});
		}
#endif
		vButtons.push_back({.m_pLabel = "OK", .m_Confirm = true, .m_Cancel = true});
		const std::optional<int> MessageResult = CommandLineVideoExportRequested ? std::nullopt : pClient->ShowMessageBox({.m_pTitle = pTitle, .m_pMessage = aMessage, .m_vButtons = vButtons});
		if(GotGraphicsError && MessageResult && *MessageResult == 0)
		{
			pClient->ViewLink("https://wiki.ddnet.org/wiki/GFX_Troubleshooting");
		}
#if !defined(CONF_PLATFORM_ANDROID)
		if(pClient->Storage() != nullptr && MessageResult && *MessageResult == (GotGraphicsError ? 1 : 0))
		{
			char aDumpsPath[IO_MAX_PATH_LENGTH];
			pClient->Storage()->GetCompletePath(IStorage::TYPE_SAVE, "dumps", aDumpsPath, sizeof(aDumpsPath));
			pClient->ViewFile(aDumpsPath);
		}
#endif
		// Client will crash due to assertion, don't call PerformAllCleanup in this inconsistent state
	});

	// create the components
	IEngine *pEngine = CreateEngine(GAME_NAME, pFutureConsoleLogger);
	pKernel->RegisterInterface(pEngine, false);
	CleanerFunctions.emplace([pEngine]() {
		// Engine has to be destroyed before the graphics so that skin download thread can finish
		delete pEngine;
	});

	IStorage *pStorage;
	{
		CMemoryLogger MemoryLogger;
		MemoryLogger.SetParent(log_get_scope_logger());
		{
			CLogScope LogScope(&MemoryLogger);
			pStorage = CreateStorage(IStorage::EInitializationType::CLIENT, argc, argv);
		}
		if(!pStorage)
		{
			log_error("client", "Failed to initialize the storage location (see details above)");
			std::string Message = std::string("Failed to initialize the storage location. See details below.\n\n") + MemoryLogger.ConcatenatedLines();
			if(!CommandLineVideoExportRequested)
				pClient->ShowMessageBox({.m_pTitle = "Storage Error", .m_pMessage = Message.c_str()});
			PerformAllCleanup();
			return -1;
		}
	}
	pKernel->RegisterInterface(pStorage);

	pFutureAssertionLogger->Set(CreateAssertionLogger(pStorage, GAME_NAME));

	{
		char aTimestamp[20];
		str_timestamp(aTimestamp, sizeof(aTimestamp));

		char aBufName[IO_MAX_PATH_LENGTH];
		str_format(aBufName, sizeof(aBufName), "dumps/%s_%s_%s_%s_crash_log_%s_%d_%s.RTP",
			GAME_NAME,
			GAME_RELEASE_VERSION,
			CONF_PLATFORM_STRING,
			CONF_ARCH_STRING,
			aTimestamp,
			process_id(),
			GIT_SHORTREV_HASH != nullptr ? GIT_SHORTREV_HASH : "");

		char aBufPath[IO_MAX_PATH_LENGTH];
		pStorage->GetCompletePath(IStorage::TYPE_SAVE, aBufName, aBufPath, sizeof(aBufPath));
		crashdump_init_if_available(aBufPath);
	}

	IConsole *pConsole = CreateConsole(CFGFLAG_CLIENT).release();
	pKernel->RegisterInterface(pConsole);

	IConfigManager *pConfigManager = CreateConfigManager();
	pKernel->RegisterInterface(pConfigManager);

	IEngineSound *pEngineSound = CreateEngineSound();
	pKernel->RegisterInterface(pEngineSound); // IEngineSound
	pKernel->RegisterInterface(static_cast<ISound *>(pEngineSound), false);

	IEngineInput *pEngineInput = CreateEngineInput();
	pKernel->RegisterInterface(pEngineInput); // IEngineInput
	pKernel->RegisterInterface(static_cast<IInput *>(pEngineInput), false);

	IEngineTextRender *pEngineTextRender = CreateEngineTextRender();
	pKernel->RegisterInterface(pEngineTextRender); // IEngineTextRender
	pKernel->RegisterInterface(static_cast<ITextRender *>(pEngineTextRender), false);

	IEngineHttp *pEngineHttp = CreateEngineHttp();
	pKernel->RegisterInterface(pEngineHttp); // IEngineHttp
	pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);

	IDiscord *pDiscord = CreateDiscord();
	pKernel->RegisterInterface(pDiscord);

	ISteam *pSteam = CreateSteam();
	pKernel->RegisterInterface(pSteam);

	INotifications *pNotifications = CreateNotifications();
	pKernel->RegisterInterface(pNotifications);

	pKernel->RegisterInterface(CreateEditor(), false);
	pKernel->RegisterInterface(CreateFavorites().release());
	pKernel->RegisterInterface(CreateGameClient());

	pEngine->Init();
	pConsole->Init();
	pConfigManager->Init();
	pNotifications->Init(GAME_NAME " Client");

	// register all console commands
	pClient->RegisterCommands();

	pKernel->RequestInterface<IGameClient>()->OnConsoleInit();

	// init client's interfaces
	pClient->InitInterfaces();

	// execute config file
	if(pStorage->FileExists(CONFIG_FILE, IStorage::TYPE_ALL))
	{
		pConsole->SetUnknownCommandCallback(SaveUnknownCommandCallback, pClient);
		if(!pConsole->ExecuteFile(CONFIG_FILE, IConsole::CLIENT_ID_UNSPECIFIED))
		{
			const char *pError = "Failed to load config from '" CONFIG_FILE "'.";
			log_error("client", "%s", pError);
			if(!CommandLineVideoExportRequested)
				pClient->ShowMessageBox({.m_pTitle = "Config File Error", .m_pMessage = pError});
			PerformAllCleanup();
			return -1;
		}
		pConsole->SetUnknownCommandCallback(IConsole::EmptyUnknownCommandCallback, nullptr);
	}

	// execute autoexec file
	if(pStorage->FileExists(AUTOEXEC_CLIENT_FILE, IStorage::TYPE_ALL))
	{
		pConsole->ExecuteFile(AUTOEXEC_CLIENT_FILE, IConsole::CLIENT_ID_UNSPECIFIED);
	}
	else // fallback
	{
		pConsole->ExecuteFile(AUTOEXEC_FILE, IConsole::CLIENT_ID_UNSPECIFIED);
	}

	if(g_Config.m_ClConfigVersion < 1)
	{
		if(g_Config.m_ClAntiPing == 0)
		{
			g_Config.m_ClAntiPingPlayers = 1;
			g_Config.m_ClAntiPingGrenade = 1;
			g_Config.m_ClAntiPingWeapons = 1;
		}
	}
	g_Config.m_ClConfigVersion = 1;

	// Parse video export arguments separately, so the remaining arguments keep
	// using the regular console command line interface.
#if defined(CONF_VIDEORECORDER)
	bool CommandLineVideoExport = false;
	bool HasVideoExportArgument = false;
	bool CommandLineVideoNoAudio = false;
	int CommandLineVideoWidth = 0;
	int CommandLineVideoHeight = 0;
	int CommandLineVideoFps = 0;
	int CommandLineVideoCrf = -1;
	int CommandLineVideoPreset = -1;
	char aCommandLineDemoPath[IO_MAX_PATH_LENGTH] = {};
	char aCommandLineVideoPath[IO_MAX_PATH_LENGTH] = {};
	char aVideoArgumentError[256] = {};
	std::vector<const char *> vArguments;
	vArguments.reserve(argc);
	vArguments.push_back(argv[0]);
	for(int Argument = 1; Argument < argc && aVideoArgumentError[0] == '\0'; ++Argument)
	{
		const char *pArgument = argv[Argument];
		auto ReadValue = [&]() -> const char * {
			if(Argument + 1 >= argc)
				return nullptr;
			return argv[++Argument];
		};
		auto ReadInteger = [&](int &Value) {
			const char *pValue = ReadValue();
			return pValue != nullptr && pValue[0] != '\0' && str_toint(pValue, &Value);
		};
		if(str_comp(pArgument, "--render-demo") == 0)
		{
			HasVideoExportArgument = true;
			const char *pValue = ReadValue();
			if(pValue == nullptr || pValue[0] == '\0' || str_length(pValue) >= static_cast<int>(sizeof(aCommandLineDemoPath)))
				str_copy(aVideoArgumentError, "Invalid value for --render-demo.");
			else
			{
				str_copy(aCommandLineDemoPath, pValue);
				CommandLineVideoExport = true;
			}
		}
		else if(str_comp(pArgument, "--output") == 0)
		{
			HasVideoExportArgument = true;
			const char *pValue = ReadValue();
			if(pValue == nullptr || pValue[0] == '\0' || str_length(pValue) >= static_cast<int>(sizeof(aCommandLineVideoPath)) - str_length(".mp4.partial"))
				str_copy(aVideoArgumentError, "Invalid value for --output.");
			else
				str_copy(aCommandLineVideoPath, pValue);
		}
		else if(str_comp(pArgument, "--width") == 0)
		{
			HasVideoExportArgument = true;
			if(!ReadInteger(CommandLineVideoWidth) || CommandLineVideoWidth < 2 || CommandLineVideoWidth > 8192 || CommandLineVideoWidth % 2 != 0)
				str_copy(aVideoArgumentError, "--width must be an even number between 2 and 8192.");
		}
		else if(str_comp(pArgument, "--height") == 0)
		{
			HasVideoExportArgument = true;
			if(!ReadInteger(CommandLineVideoHeight) || CommandLineVideoHeight < 2 || CommandLineVideoHeight > 8192 || CommandLineVideoHeight % 2 != 0)
				str_copy(aVideoArgumentError, "--height must be an even number between 2 and 8192.");
		}
		else if(str_comp(pArgument, "--fps") == 0)
		{
			HasVideoExportArgument = true;
			if(!ReadInteger(CommandLineVideoFps) || CommandLineVideoFps < 1 || CommandLineVideoFps > 1000)
				str_copy(aVideoArgumentError, "--fps must be between 1 and 1000.");
		}
		else if(str_comp(pArgument, "--crf") == 0)
		{
			HasVideoExportArgument = true;
			if(!ReadInteger(CommandLineVideoCrf) || CommandLineVideoCrf < 0 || CommandLineVideoCrf > 51)
				str_copy(aVideoArgumentError, "--crf must be between 0 and 51.");
		}
		else if(str_comp(pArgument, "--preset") == 0)
		{
			HasVideoExportArgument = true;
			if(!ReadInteger(CommandLineVideoPreset) || CommandLineVideoPreset < 0 || CommandLineVideoPreset > 9)
				str_copy(aVideoArgumentError, "--preset must be between 0 and 9.");
		}
		else if(str_comp(pArgument, "--no-audio") == 0)
		{
			HasVideoExportArgument = true;
			CommandLineVideoNoAudio = true;
		}
		else
			vArguments.push_back(pArgument);
	}
	if(aVideoArgumentError[0] == '\0' && HasVideoExportArgument && (!CommandLineVideoExport || aCommandLineVideoPath[0] == '\0'))
		str_copy(aVideoArgumentError, "--render-demo and --output must be used together.");
	if(aVideoArgumentError[0] != '\0')
	{
		log_error("videorecorder", "%s", aVideoArgumentError);
		log_error("videorecorder", "Usage: DDNet --render-demo <demo> --output <video.mp4> [--width <even>] [--height <even>] [--fps <1-1000>] [--crf <0-51>] [--preset <0-9>] [--no-audio]");
		PerformAllCleanup();
		return -1;
	}
	argc = static_cast<int>(vArguments.size());
	argv = vArguments.data();
#else
	if(CommandLineVideoExportRequested)
	{
		log_error("videorecorder", "This client was built without video recorder support.");
		PerformAllCleanup();
		return -1;
	}
#endif

	// parse the command line arguments
	pConsole->SetUnknownCommandCallback(UnknownArgumentCallback, pClient);
	pConsole->ParseArguments(argc - 1, &argv[1]);
	pConsole->SetUnknownCommandCallback(IConsole::EmptyUnknownCommandCallback, nullptr);

#if defined(CONF_VIDEORECORDER)
	if(CommandLineVideoExport)
	{
		if(!str_endswith(aCommandLineVideoPath, ".mp4"))
			str_append(aCommandLineVideoPath, ".mp4");
		if(pStorage->FileExists(aCommandLineVideoPath, IStorage::TYPE_SAVE_OR_ABSOLUTE))
		{
			log_error("videorecorder", "Output file '%s' already exists.", aCommandLineVideoPath);
			PerformAllCleanup();
			return -1;
		}
		CVideoExportSettings Settings;
		Settings.m_Width = CommandLineVideoWidth;
		Settings.m_Height = CommandLineVideoHeight;
		Settings.m_FPS = CommandLineVideoFps == 0 ? g_Config.m_ClVideoRecorderFPS : CommandLineVideoFps;
		Settings.m_Audio = !CommandLineVideoNoAudio && g_Config.m_ClVideoSndEnable != 0;
		Settings.m_Crf = CommandLineVideoCrf < 0 ? g_Config.m_ClVideoX264Crf : CommandLineVideoCrf;
		Settings.m_Preset = CommandLineVideoPreset < 0 ? g_Config.m_ClVideoX264Preset : CommandLineVideoPreset;
		Settings.m_ShowHud = g_Config.m_ClVideoShowhud != 0;
		Settings.m_ShowChat = g_Config.m_ClVideoShowChat != 0;
		Settings.m_ShowHookCollOther = g_Config.m_ClVideoShowHookCollOther != 0;
		Settings.m_ShowDirection = g_Config.m_ClVideoShowDirection;
		Settings.m_ShowImportantAlerts = g_Config.m_ClVideoShowImportantAlerts != 0;
		pClient->ConfigureCommandLineVideoExport(aCommandLineDemoPath, aCommandLineVideoPath, Settings);
		signal(SIGINT, HandleVideoExportInterrupt);
		signal(SIGTERM, HandleVideoExportInterrupt);
	}
#endif

	if(pSteam->GetConnectAddress())
	{
		pClient->HandleConnectAddress(pSteam->GetConnectAddress());
		pSteam->ClearConnectAddress();
	}

	if(g_Config.m_Logfile[0])
	{
		const int Mode = g_Config.m_Logappend ? IOFLAG_APPEND : IOFLAG_WRITE;
		IOHANDLE Logfile = pStorage->OpenFile(g_Config.m_Logfile, Mode, IStorage::TYPE_SAVE_OR_ABSOLUTE);
		if(Logfile)
		{
			auto pFileLogger = log_logger_file(Logfile);
			pFileLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_Loglevel)});
			pFutureFileLogger->Set(std::move(pFileLogger));
		}
		else
		{
			log_error("client", "failed to open '%s' for logging", g_Config.m_Logfile);
			pFutureFileLogger->Set(log_logger_noop());
		}
	}
	else
	{
		pFutureFileLogger->Set(log_logger_noop());
	}

	// Register protocol and file extensions
#if defined(CONF_FAMILY_WINDOWS) && !defined(CONF_DEMO_RENDER_TOOL)
	pClient->ShellRegister();
#endif

#if !defined(CONF_DEMO_RENDER_TOOL)
	// Do not automatically translate touch events to mouse events and vice versa.
	SDL_SetHint("SDL_TOUCH_MOUSE_EVENTS", "0");
	SDL_SetHint("SDL_MOUSE_TOUCH_EVENTS", "0");

	// Support longer IME composition strings (enables SDL_TEXTEDITING_EXT).
#if SDL_VERSION_ATLEAST(2, 0, 22)
	SDL_SetHint(SDL_HINT_IME_SUPPORT_EXTENDED_TEXT, "1");
#endif

#if defined(CONF_PLATFORM_MACOS)
	// Hints will not be set if there is an existing override hint or environment variable that takes precedence.
	// So this respects cli environment overrides.
	SDL_SetHint("SDL_MAC_OPENGL_ASYNC_DISPATCH", "1");
#endif

#if defined(CONF_FAMILY_WINDOWS)
	SDL_SetHint("SDL_IME_SHOW_UI", g_Config.m_InpImeNativeUi ? "1" : "0");
#else
	SDL_SetHint("SDL_IME_SHOW_UI", "1");
#endif

#if defined(CONF_PLATFORM_ANDROID)
	// Trap the Android back button so it can be handled in our code reliably
	// instead of letting the system handle it.
	SDL_SetHint("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
	// Force landscape screen orientation.
	SDL_SetHint("SDL_IOS_ORIENTATIONS", "LandscapeLeft LandscapeRight");
#endif

	// init SDL
	if(SDL_Init(0) < 0)
	{
		char aError[256];
		str_format(aError, sizeof(aError), "Unable to initialize SDL base: %s", SDL_GetError());
		log_error("client", "%s", aError);
		if(!CommandLineVideoExportRequested)
			pClient->ShowMessageBox({.m_pTitle = "SDL Error", .m_pMessage = aError});
		PerformAllCleanup();
		return -1;
	}
#endif

	// run the client
	log_trace("client", "initialization finished after %.2fms, starting...", (time_get() - MainStart) * 1000.0f / (float)time_freq());
	pClient->Run();

	const bool Restarting = pClient->State() == CClient::STATE_RESTARTING;
#if defined(CONF_VIDEORECORDER)
	const int ExitCode = pClient->CommandLineExitCode();
#else
	const int ExitCode = 0;
#endif
#if !defined(CONF_PLATFORM_ANDROID)
	char aRestartBinaryPath[IO_MAX_PATH_LENGTH];
	if(Restarting)
	{
		pStorage->GetBinaryPath(PLAT_CLIENT_EXEC, aRestartBinaryPath, sizeof(aRestartBinaryPath));
	}
#endif

	std::vector<SWarning> vQuittingWarnings = pClient->QuittingWarnings();

	PerformCleanup();

	for(const SWarning &Warning : vQuittingWarnings)
	{
		if(!CommandLineVideoExportRequested)
			ShowMessageBoxWithoutGraphics({.m_pTitle = Warning.m_aWarningTitle, .m_pMessage = Warning.m_aWarningMsg});
	}

	if(Restarting)
	{
#if defined(CONF_PLATFORM_ANDROID)
		RestartAndroidApp();
#else
		process_execute(aRestartBinaryPath, EShellExecuteWindowState::FOREGROUND);
#endif
	}

	PerformFinalCleanup();

	return ExitCode;
}

// DDRace

void CClient::RaceRecord_Start(const char *pFilename)
{
	dbg_assert(IsOnline(), "Client must be online to record demo");

	DemoRecorders()[RECORDER_RACE].Start(
		Storage(),
		m_pConsole,
		pFilename,
		IsSixup(m_NetworkSessionId) ? GameClient()->NetVersion7() : GameClient()->NetVersion(),
		GameClient()->Map(m_NetworkSessionId)->BaseName(),
		GameClient()->Map(m_NetworkSessionId)->Sha256(),
		GameClient()->Map(m_NetworkSessionId)->Crc(),
		"client",
		GameClient()->Map(m_NetworkSessionId)->Size(),
		nullptr,
		GameClient()->Map(m_NetworkSessionId)->File(),
		nullptr,
		nullptr);
}

void CClient::RaceRecord_Stop()
{
	if(DemoRecorder(RECORDER_RACE)->IsRecording())
	{
		DemoRecorder(RECORDER_RACE)->Stop(IDemoRecorder::EStopMode::KEEP_FILE);
	}
}

bool CClient::RaceRecord_IsRecording()
{
	return DemoRecorder(RECORDER_RACE)->IsRecording();
}

void CClient::RequestDDNetInfo()
{
	if(m_pDDNetInfoTask && !m_pDDNetInfoTask->Done())
		return;

	char aUrl[256];
	str_copy(aUrl, DDNET_INFO_URL);

	if(g_Config.m_BrIndicateFinished)
	{
		char aEscaped[128];
		EscapeUrl(aEscaped, PlayerName());
		str_append(aUrl, "?name=");
		str_append(aUrl, aEscaped);
	}

	m_pDDNetInfoTask = HttpGetFile(aUrl, Storage(), DDNET_INFO_FILE, IStorage::TYPE_SAVE);
	m_pDDNetInfoTask->Timeout(CTimeout{10000, 0, 500, 10});
	m_pDDNetInfoTask->SkipByFileTime(false); // Always re-download.
	// Use ipv4 so we can know the ingame ip addresses of players before they join game servers
	m_pDDNetInfoTask->IpResolve(IPRESOLVE::V4);
	Http()->Run(m_pDDNetInfoTask);
	m_InfoState = EInfoState::LOADING;
}

int CClient::GetPredictionTime(CSessionId SessionId, CStreamId StreamId)
{
	int64_t Now = time_get();
	return (int)((Connection(SessionId, StreamId).m_PredictedTime.Get(Now) - Connection(SessionId, StreamId).m_GameTime.Get(Now)) * 1000 / (float)time_freq());
}

int CClient::GetPredictionTick(CSessionId SessionId, CStreamId StreamId)
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

void CClient::GetSmoothTick(CSessionId SessionId, CStreamId StreamId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount)
{
	int64_t GameTime = Connection(SessionId, StreamId).m_GameTime.Get(Now);
	int64_t PredTime = Connection(SessionId, StreamId).m_PredictedTime.Get(Now);
	int64_t SmoothTime = std::clamp(GameTime + (int64_t)(MixAmount * (PredTime - GameTime)), GameTime, PredTime);

	*pSmoothTick = (int)(SmoothTime * GameTickSpeed() / time_freq()) + 1;
	*pSmoothIntraTick = (SmoothTime - (*pSmoothTick - 1) * time_freq() / GameTickSpeed()) / (float)(time_freq() / GameTickSpeed());
}

void CClient::AddWarning(const SWarning &Warning)
{
	const std::unique_lock<std::mutex> Lock(m_WarningsMutex);
	m_vWarnings.emplace_back(Warning);
}

std::optional<SWarning> CClient::CurrentWarning()
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

int CClient::MaxLatencyTicks(CSessionId SessionId) const
{
	return GameTickSpeed() + (PredictionMargin(SessionId) * GameTickSpeed()) / 1000;
}

int CClient::PredictionMargin(CSessionId SessionId) const
{
	return NetworkSource(SessionId).m_ServerCapabilities.m_SyncWeaponInput ? g_Config.m_ClPredictionMargin : 10;
}

int CClient::UdpConnectivity(int NetType)
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

static bool ViewLinkImpl(const char *pLink)
{
#if defined(CONF_PLATFORM_ANDROID)
	if(SDL_OpenURL(pLink) == 0)
	{
		return true;
	}
	log_error("client", "Failed to open link '%s' (%s)", pLink, SDL_GetError());
	return false;
#else
	if(os_open_link(pLink))
	{
		return true;
	}
	log_error("client", "Failed to open link '%s'", pLink);
	return false;
#endif
}

bool CClient::ViewLink(const char *pLink)
{
	if(!str_startswith(pLink, "https://"))
	{
		log_error("client", "Failed to open link '%s': only https-links are allowed", pLink);
		return false;
	}
	return ViewLinkImpl(pLink);
}

bool CClient::ViewFile(const char *pFilename)
{
#if defined(CONF_PLATFORM_MACOS)
	return ViewLinkImpl(pFilename);
#else
	// Create a file link so the path can contain forward and
	// backward slashes. But the file link must be absolute.
	char aWorkingDir[IO_MAX_PATH_LENGTH];
	if(fs_is_relative_path(pFilename))
	{
		if(!fs_getcwd(aWorkingDir, sizeof(aWorkingDir)))
		{
			log_error("client", "Failed to open file '%s' (failed to get working directory)", pFilename);
			return false;
		}
		str_append(aWorkingDir, "/");
	}
	else
	{
		aWorkingDir[0] = '\0';
	}

	char aFileLink[IO_MAX_PATH_LENGTH];
	str_format(aFileLink, sizeof(aFileLink), "file://%s%s", aWorkingDir, pFilename);
	return ViewLinkImpl(aFileLink);
#endif
}

#if defined(CONF_FAMILY_WINDOWS)
void CClient::ShellRegister()
{
	char aFullPath[IO_MAX_PATH_LENGTH];
	Storage()->GetBinaryPathAbsolute(PLAT_CLIENT_EXEC, aFullPath, sizeof(aFullPath));
	if(!aFullPath[0])
	{
		log_error("client", "Failed to register protocol and file extensions: could not determine absolute path");
		return;
	}

	bool Updated = false;
	if(!windows_shell_register_protocol("ddnet", aFullPath, &Updated))
		log_error("client", "Failed to register ddnet protocol");
	if(!windows_shell_register_protocol("ddnet+quic", aFullPath, &Updated))
		log_error("client", "Failed to register ddnet+quic protocol");
	if(!windows_shell_register_protocol("tw-0.7+quic", aFullPath, &Updated))
		log_error("client", "Failed to register tw-0.7+quic protocol");
	if(!windows_shell_register_extension(".map", "Map File", GAME_NAME, aFullPath, &Updated))
		log_error("client", "Failed to register .map file extension");
	if(!windows_shell_register_extension(".demo", "Demo File", GAME_NAME, aFullPath, &Updated))
		log_error("client", "Failed to register .demo file extension");
	if(!windows_shell_register_application(GAME_NAME, aFullPath, &Updated))
		log_error("client", "Failed to register application");
	if(Updated)
		windows_shell_update();
}

void CClient::ShellUnregister()
{
	char aFullPath[IO_MAX_PATH_LENGTH];
	Storage()->GetBinaryPathAbsolute(PLAT_CLIENT_EXEC, aFullPath, sizeof(aFullPath));
	if(!aFullPath[0])
	{
		log_error("client", "Failed to unregister protocol and file extensions: could not determine absolute path");
		return;
	}

	bool Updated = false;
	if(!windows_shell_unregister_class("ddnet", &Updated))
		log_error("client", "Failed to unregister ddnet protocol");
	if(!windows_shell_unregister_class("ddnet+quic", &Updated))
		log_error("client", "Failed to unregister ddnet+quic protocol");
	if(!windows_shell_unregister_class("tw-0.7+quic", &Updated))
		log_error("client", "Failed to unregister tw-0.7+quic protocol");
	if(!windows_shell_unregister_class(GAME_NAME ".map", &Updated))
		log_error("client", "Failed to unregister .map file extension");
	if(!windows_shell_unregister_class(GAME_NAME ".demo", &Updated))
		log_error("client", "Failed to unregister .demo file extension");
	if(!windows_shell_unregister_application(aFullPath, &Updated))
		log_error("client", "Failed to unregister application");
	if(Updated)
		windows_shell_update();
}
#endif

std::optional<int> CClient::ShowMessageBox(const IGraphics::CMessageBox &MessageBox)
{
	std::optional<int> Result = m_pGraphics == nullptr ? std::nullopt : m_pGraphics->ShowMessageBox(MessageBox);
	if(!Result)
	{
		Result = ShowMessageBoxWithoutGraphics(MessageBox);
	}
	return Result;
}

void CClient::GetGpuInfoString(char (&aGpuInfo)[512])
{
#if defined(CONF_HEADLESS_CLIENT)
	if(m_pGraphics == nullptr || !m_pGraphics->IsBackendInitialized())
	{
		str_format(aGpuInfo, std::size(aGpuInfo),
			"Configured graphics backend: headless\n"
			"Graphics %s not yet initialized.",
			m_pGraphics == nullptr ? "were" : "backend was");
	}
	else
	{
		str_copy(aGpuInfo, "Configured graphics backend: headless");
	}
#else
	if(m_pGraphics == nullptr || !m_pGraphics->IsBackendInitialized())
	{
		str_format(aGpuInfo, std::size(aGpuInfo),
			"Configured graphics backend: %s %d.%d.%d\n"
			"Graphics %s not yet initialized.",
			g_Config.m_GfxBackend, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch,
			m_pGraphics == nullptr ? "were" : "backend was");
	}
	else
	{
		str_format(aGpuInfo, std::size(aGpuInfo),
			"Configured graphics backend: %s %d.%d.%d\n"
			"GPU: %s - %s - %s\n"
			"Texture: %.2f MiB, "
			"Buffer: %.2f MiB, "
			"Streamed: %.2f MiB, "
			"Staging: %.2f MiB",
			g_Config.m_GfxBackend, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch,
			m_pGraphics->GetVendorString(), m_pGraphics->GetRendererString(), m_pGraphics->GetVersionString(),
			m_pGraphics->TextureMemoryUsage() / 1024.0 / 1024.0,
			m_pGraphics->BufferMemoryUsage() / 1024.0 / 1024.0,
			m_pGraphics->StreamedMemoryUsage() / 1024.0 / 1024.0,
			m_pGraphics->StagingMemoryUsage() / 1024.0 / 1024.0);
	}
#endif
}

void CClient::SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger)
{
	m_pFileLogger = pFileLogger;
	m_pStdoutLogger = pStdoutLogger;
}
