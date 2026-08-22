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
#include <base/rust.h>
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

static ITextRender::CTextRenderStats TextRenderStatsDelta(const ITextRender::CTextRenderStats &Current, const ITextRender::CTextRenderStats &Previous)
{
	ITextRender::CTextRenderStats Delta;
	Delta.m_LayoutTimeNanoseconds = Current.m_LayoutTimeNanoseconds - Previous.m_LayoutTimeNanoseconds;
	Delta.m_LayoutCalls = Current.m_LayoutCalls - Previous.m_LayoutCalls;
	Delta.m_GlyphsLaidOut = Current.m_GlyphsLaidOut - Previous.m_GlyphsLaidOut;
	Delta.m_ContainerCreates = Current.m_ContainerCreates - Previous.m_ContainerCreates;
	Delta.m_ContainerSoftRecreates = Current.m_ContainerSoftRecreates - Previous.m_ContainerSoftRecreates;
	Delta.m_ContainerDeletes = Current.m_ContainerDeletes - Previous.m_ContainerDeletes;
	Delta.m_ContainerRenders = Current.m_ContainerRenders - Previous.m_ContainerRenders;
	Delta.m_UploadBytes = Current.m_UploadBytes - Previous.m_UploadBytes;
	return Delta;
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

void CClient::LoadDebugFont()
{
	m_DebugFont = Graphics()->LoadTexture("debug_font.png", IStorage::TYPE_ALL);
}

// ---

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

	const IGraphics::CFrameRenderStats RenderStats = Graphics()->FrameRenderStats();
	if(RenderStats.m_GpuTimingSupported)
		str_format(aBuffer, sizeof(aBuffer), "Render/GPU: %.3f / %.3f ms", m_RenderWallTimeNanoseconds / 1000000.0, RenderStats.m_GpuTimeNanoseconds / 1000000.0);
	else
		str_format(aBuffer, sizeof(aBuffer), "Render/GPU: %.3f ms / unavailable", m_RenderWallTimeNanoseconds / 1000000.0);
	Graphics()->QuadsText(20.0f * FontSize, 2 + 2 * FontSize, FontSize, aBuffer);
	if(RenderStats.m_GpuRenderZoneMask != 0)
	{
		char aWorld[32] = "-";
		char aInterface[32] = "-";
		if((RenderStats.m_GpuRenderZoneMask & (1U << static_cast<uint32_t>(IGraphics::EGpuRenderZone::WORLD))) != 0)
			str_format(aWorld, sizeof(aWorld), "%.3f", RenderStats.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::WORLD)] / 1000000.0);
		if((RenderStats.m_GpuRenderZoneMask & (1U << static_cast<uint32_t>(IGraphics::EGpuRenderZone::INTERFACE))) != 0)
			str_format(aInterface, sizeof(aInterface), "%.3f", RenderStats.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::INTERFACE)] / 1000000.0);
		str_format(aBuffer, sizeof(aBuffer), "GPU world/UI: %s / %s ms", aWorld, aInterface);
		Graphics()->QuadsText(20.0f * FontSize, 2 + 3 * FontSize, FontSize, aBuffer);
	}
	str_format(aBuffer, sizeof(aBuffer), "Cmd/draw/tri: %" PRIu64 " / %" PRIu64 " / %" PRIu64, RenderStats.m_Commands, RenderStats.m_DrawCalls, RenderStats.m_Triangles);
	Graphics()->QuadsText(20.0f * FontSize, 2 + 4 * FontSize, FontSize, aBuffer);

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

const char *CClient::LatestVersion() const
{
	return m_aVersionStr;
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

void CClient::Render()
{
	if(m_EditorActive)
	{
		CRenderTraceScope TraceScope(&m_RenderTrace, "client/editor");
		m_pEditor->OnRender();
	}
	else
	{
		CRenderTraceScope TraceScope(&m_RenderTrace, "client/game");
		GameClient()->OnRender();
	}

	{
		CRenderTraceScope TraceScope(&m_RenderTrace, "client/debug");
		RenderDebug();
		RenderGraphs();
	}
}

void CClient::RenderScreen()
{
	if(!m_EditorActive)
		GameClient()->OnRenderPrepare();
	Render();
	if(!m_EditorActive)
		GameClient()->OnRenderFinalize();
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
			CNetClient &PrimaryNetClient = Source.PrimaryNetClient();
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
	// A build without an editor registers none, and everything that would
	// open one is skipped instead of calling into a stand-in that does nothing.
	m_pEditor = Kernel()->TryGetInterface<IEditor>();
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

	m_GhostRecorder.Init(m_pStorage);
	m_GhostLoader.Init(m_pStorage);
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
	InitVideoBackend();
#endif

	// init text render
	m_pTextRender = Kernel()->RequestInterface<IEngineTextRender>();
	m_pTextRender->Init();

	// init the input
	Input()->Init();

	// init the editor
	if(m_pEditor != nullptr)
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
		if(m_aCmdEditMap[0] && m_pEditor != nullptr)
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

		if(CtrlShiftKey(KEY_E, LastE) && m_pEditor != nullptr)
		{
			if(g_Config.m_ClEditor)
				m_pEditor->OnClose();
			g_Config.m_ClEditor = g_Config.m_ClEditor ^ 1;
		}

		// render
		{
			if(g_Config.m_ClEditor && m_pEditor != nullptr)
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

				m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;

				// keep the overflow time - it's used to make sure the gfx refreshrate is reached
				int64_t AdditionalTime = GfxRefreshRate ? ((Now - LastRenderTime) - (time_freq() / (int64_t)GfxRefreshRate)) : 0;
				// if the value is over the frametime of a 60 fps frame, reset the additional time (drop the frames, that are lost already)
				if(AdditionalTime > (time_freq() / 60))
					AdditionalTime = (time_freq() / 60);
				LastRenderTime = Now - AdditionalTime;
				m_LastRenderTime = Now;
				Graphics()->SetRenderStatsEnabled(m_BenchmarkFile != nullptr || m_RenderTrace.Enabled() || g_Config.m_Debug);
				m_RenderTrace.BeginFrame();
				const bool MeasureRenderWall = m_BenchmarkFile != nullptr || m_RenderTrace.Enabled() || g_Config.m_Debug;
				const std::chrono::nanoseconds RenderWallStart = MeasureRenderWall ? time_get_nanoseconds() : std::chrono::nanoseconds{};

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
					// A frame that goes to the export does not go to the screen, so this
					// is what decides how often the game and the menu are drawn while a
					// demo renders in the background. The export is the job and may run as
					// fast as it can; the screen only has to stay usable, which is a floor
					// to hold rather than a rate to reach. Every picture drawn for the
					// watcher is a picture the export does not encode. A command line
					// export has nobody watching and only prints a line.
					constexpr int MinInteractiveRefreshRate = 30;
					const int InteractiveRefreshRate = std::min(g_Config.m_GfxRefreshRate > 0 ? g_Config.m_GfxRefreshRate : MinInteractiveRefreshRate, MinInteractiveRefreshRate);
					const std::chrono::nanoseconds ProgressInterval = m_CommandLineVideoExport ? std::chrono::nanoseconds(std::chrono::seconds(1)) : std::chrono::nanoseconds(std::chrono::seconds(1)) / InteractiveRefreshRate;
					if(ProgressRenderTime - m_LastVideoProgressRender >= ProgressInterval)
					{
						// Everything on the screen moves with the frame time, and
						// during an export that is the time one exported frame took.
						// The watcher sees a picture far less often than that, so
						// without this the menu and the game animate in the export's
						// steps instead of in the ones they are shown in.
						const float ExportFrameTime = m_RenderFrameTime;
						if(m_LastVideoProgressRender != std::chrono::nanoseconds::zero())
							m_RenderFrameTime = std::chrono::duration<float>(ProgressRenderTime - m_LastVideoProgressRender).count();
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
						m_RenderFrameTime = ExportFrameTime;
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
				if(MeasureRenderWall)
					m_RenderWallTimeNanoseconds = (time_get_nanoseconds() - RenderWallStart).count();
				if(m_RenderTrace.Enabled())
				{
					const ITextRender::CTextRenderStats TextStats = TextRender()->TextRenderStats();
					CRenderTrace::CFrame Frame;
					Frame.m_FrametimeNanoseconds = static_cast<uint64_t>(m_RenderFrameTime * 1000000000.0f);
					Frame.m_RenderWallNanoseconds = m_RenderWallTimeNanoseconds;
					Frame.m_Render = Graphics()->FrameRenderStats();
					Frame.m_Text = TextRenderStatsDelta(TextStats, m_RenderTracePreviousTextRenderStats);
					Frame.m_Mailbox = Graphics()->FrameMailboxStats();
					Frame.m_TextureMemory = Graphics()->TextureMemoryUsage();
					Frame.m_BufferMemory = Graphics()->BufferMemoryUsage();
					Frame.m_StreamedMemory = Graphics()->StreamedMemoryUsage();
					Frame.m_StagingMemory = Graphics()->StagingMemoryUsage();
					m_RenderTrace.RecordFrame(Frame);
					m_RenderTracePreviousTextRenderStats = TextStats;
					if(m_RenderTrace.ShouldStop())
						StopRenderTrace();
				}
				if(m_BenchmarkFile)
				{
					const IGraphics::CFrameRenderStats RenderStats = Graphics()->FrameRenderStats();
					const IGraphics::SFrameMailboxStats MailboxStats = Graphics()->FrameMailboxStats();
					const ITextRender::CTextRenderStats TextStats = TextRender()->TextRenderStats();
					const ITextRender::CTextRenderStats &PreviousTextStats = m_BenchmarkPreviousTextRenderStats;
					char aBuf[2048];
					str_format(aBuf, sizeof(aBuf),
						"Frametime %d us RenderWall %" PRIu64 " us GpuTime %" PRIu64 " us GpuWorld %" PRIu64 " us GpuInterface %" PRIu64 " us GpuZoneMask %u GpuSample %" PRIu64 " GpuSupported %d Commands %" PRIu64 " ResourceCommands %" PRIu64 " DrawCommands %" PRIu64 " DrawCalls %" PRIu64 " Triangles %" PRIu64 " Instances %" PRIu64 " RenderPasses %" PRIu64 " BufferCreates %" PRIu64 " BufferRecreates %" PRIu64 " BufferUpdates %" PRIu64 " TextureCreates %" PRIu64 " TextureUpdates %" PRIu64 " UploadBytes %" PRIu64 " StreamedBytes %" PRIu64 " TextLayout %" PRIu64 " us TextLayoutCalls %" PRIu64 " Glyphs %" PRIu64 " TextCreates %" PRIu64 " TextSoftRecreates %" PRIu64 " TextDeletes %" PRIu64 " TextRenders %" PRIu64 " TextUploadBytes %" PRIu64 " FramesProduced %" PRIu64 " FramesRendered %" PRIu64 " FramesDropped %" PRIu64 " TextureMemory %" PRIu64 " BufferMemory %" PRIu64 " StreamedMemory %" PRIu64 " StagingMemory %" PRIu64 "\n",
						(int)(m_RenderFrameTime * 1000000), m_RenderWallTimeNanoseconds / 1000,
						RenderStats.m_GpuTimeNanoseconds / 1000,
						RenderStats.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::WORLD)] / 1000,
						RenderStats.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::INTERFACE)] / 1000,
						RenderStats.m_GpuRenderZoneMask, RenderStats.m_GpuSample, RenderStats.m_GpuTimingSupported,
						RenderStats.m_Commands, RenderStats.m_ResourceCommands, RenderStats.m_DrawCommands, RenderStats.m_DrawCalls, RenderStats.m_Triangles, RenderStats.m_Instances, RenderStats.m_RenderPasses,
						RenderStats.m_BufferCreates, RenderStats.m_BufferRecreates, RenderStats.m_BufferUpdates, RenderStats.m_TextureCreates, RenderStats.m_TextureUpdates, RenderStats.m_UploadBytes, RenderStats.m_StreamedBytes,
						(TextStats.m_LayoutTimeNanoseconds - PreviousTextStats.m_LayoutTimeNanoseconds) / 1000, TextStats.m_LayoutCalls - PreviousTextStats.m_LayoutCalls, TextStats.m_GlyphsLaidOut - PreviousTextStats.m_GlyphsLaidOut,
						TextStats.m_ContainerCreates - PreviousTextStats.m_ContainerCreates, TextStats.m_ContainerSoftRecreates - PreviousTextStats.m_ContainerSoftRecreates, TextStats.m_ContainerDeletes - PreviousTextStats.m_ContainerDeletes, TextStats.m_ContainerRenders - PreviousTextStats.m_ContainerRenders, TextStats.m_UploadBytes - PreviousTextStats.m_UploadBytes,
						MailboxStats.m_Produced, MailboxStats.m_Rendered, MailboxStats.m_Dropped, Graphics()->TextureMemoryUsage(), Graphics()->BufferMemoryUsage(), Graphics()->StreamedMemoryUsage(), Graphics()->StagingMemoryUsage());
					io_write(m_BenchmarkFile, aBuf, str_length(aBuf));
					m_BenchmarkPreviousTextRenderStats = TextStats;
					if(time_get() > m_BenchmarkStopTime)
					{
						io_close(m_BenchmarkFile);
						m_BenchmarkFile = nullptr;
						Graphics()->SetRenderStatsEnabled(m_RenderTrace.Enabled() || g_Config.m_Debug);
						TextRender()->SetTextRenderStatsEnabled(m_RenderTrace.Enabled());
						Quit();
					}
				}
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

	if(m_RenderTrace.Enabled())
		StopRenderTrace();

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

	// The demo render tool reads the settings to render the way the client would,
	// but it is a command line tool that may well run while the client is open,
	// so it does not write them back.
#if !defined(CONF_DEMO_RENDER_TOOL)
	if(!m_pConfigManager->Save())
	{
		char aError[128];
		str_format(aError, sizeof(aError), Localize("Saving settings to '%s' failed"), CONFIG_FILE);
		m_vQuittingWarnings.emplace_back(Localize("Error saving settings"), aError);
	}
#endif
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
			pStream->m_NetClient.Close();
	}
	m_ContactNetClient.Close();
	CNetBase::CloseLog();

	// shutdown text render while graphics are still available
	m_pTextRender->Shutdown();
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

// Queues a demo the way the demo browser does, so that a render can be started
// from a config or a bind while the game keeps running.
void CClient::Con_RenderDemo(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = static_cast<CClient *>(pUserData);
	char aVideoName[IO_MAX_PATH_LENGTH];
	str_copy(aVideoName, fs_filename(pResult->GetString(0)));
	if(char *pExtension = (char *)str_endswith(aVideoName, ".demo"))
		*pExtension = '\0';
	const char *pError = pSelf->QueueVideoExport(pResult->GetString(0), IStorage::TYPE_ALL, aVideoName, pSelf->DefaultVideoExportSettings(), DEMO_SPEED_INDEX_DEFAULT, true, false);
	if(pError != nullptr)
		log_error("videorecorder", "Could not queue '%s': %s", pResult->GetString(0), pError);
}

CVideoExportSettings CClient::DefaultVideoExportSettings()
{
	CVideoExportSettings Settings;
	// A command from the command line runs before the window exists, so the
	// configured size stands in for the one nobody can be asked for yet.
	const bool HasScreen = Graphics() != nullptr && Graphics()->ScreenWidth() > 0 && Graphics()->ScreenHeight() > 0;
	Settings.m_Width = (HasScreen ? Graphics()->ScreenWidth() : g_Config.m_GfxScreenWidth) & ~1;
	Settings.m_Height = (HasScreen ? Graphics()->ScreenHeight() : g_Config.m_GfxScreenHeight) & ~1;
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
	m_pVideo = CreateVideo(Graphics(), Sound(), Storage(), Settings, m_LocalStartTime, aFilename, OutputStorageType, !ExactFilename, !OfflineAudio);
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

bool CClient::ConfigureCommandLineVideoExport(const CCommandLineVideoExport &Export)
{
	str_copy(m_aCommandLineVideoPath, Export.m_aVideoPath);
	if(!str_endswith(m_aCommandLineVideoPath, ".mp4"))
		str_append(m_aCommandLineVideoPath, ".mp4");
	if(Storage()->FileExists(m_aCommandLineVideoPath, IStorage::TYPE_SAVE_OR_ABSOLUTE))
	{
		log_error("videorecorder", "Output file '%s' already exists.", m_aCommandLineVideoPath);
		return false;
	}

	m_CommandLineVideoExport = true;
	m_CommandLineExitCode = 1;
	m_HiddenWindow = true;
	str_copy(m_aCommandLineDemoPath, Export.m_aDemoPath);
	m_CommandLineVideoSettings = Export.Settings();

	// The export writes the video file as it goes, so an interrupt has to reach
	// the encoder instead of killing the process with a half written file.
	signal(SIGINT, HandleVideoExportInterrupt);
	signal(SIGTERM, HandleVideoExportInterrupt);
	return true;
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
	Graphics()->SetRenderStatsEnabled(m_BenchmarkFile != nullptr || m_RenderTrace.Enabled() || g_Config.m_Debug);
	TextRender()->SetTextRenderStatsEnabled(m_BenchmarkFile != nullptr || m_RenderTrace.Enabled());
	m_BenchmarkPreviousTextRenderStats = TextRender()->TextRenderStats();
}

void CClient::Con_RenderTraceStart(IConsole::IResult *pResult, void *pUserData)
{
	CClient *pSelf = static_cast<CClient *>(pUserData);
	if(!pSelf->m_RenderTrace.Start(pSelf->Storage(), pResult->GetInteger(0), pResult->GetString(1)))
	{
		log_error("render_trace", "trace is already active or the arguments are invalid");
		return;
	}
	pSelf->Graphics()->SetRenderStatsEnabled(true);
	pSelf->TextRender()->SetTextRenderStatsEnabled(true);
	pSelf->m_RenderTracePreviousTextRenderStats = pSelf->TextRender()->TextRenderStats();
}

void CClient::Con_RenderTraceStop(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CClient *>(pUserData)->StopRenderTrace();
}

void CClient::StopRenderTrace()
{
	if(!m_RenderTrace.Enabled())
		return;
	m_RenderTrace.Stop();
	Graphics()->SetRenderStatsEnabled(m_BenchmarkFile != nullptr || g_Config.m_Debug);
	TextRender()->SetTextRenderStatsEnabled(m_BenchmarkFile != nullptr);
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

const CServerInfo *CClient::KnownServerInfo(const NETADDR &Address)
{
	if(const CServerBrowser::CServerEntry *pEntry = m_ServerBrowser.Find(Address))
		return &pEntry->m_Info;
	// A 0.7 server is listed under its own address, without the flag that says
	// which protocol it was found with.
	NETADDR Legacy = Address;
	Legacy.type &= ~NETTYPE_TW7;
	const CServerBrowser::CServerEntry *pEntry = m_ServerBrowser.Find(Legacy);
	return pEntry == nullptr ? nullptr : &pEntry->m_Info;
}

void CClient::RequestServerInfoRefresh(const NETADDR &Address)
{
	m_ServerBrowser.RequestCurrentServer(Address);
}

void CClient::RequestServerInfoWithToken(const NETADDR &Address, int *pBasicToken, int *pToken)
{
	m_ServerBrowser.RequestCurrentServerWithRandomToken(Address, pBasicToken, pToken);
}

void CClient::OnCurrentServerPing(const NETADDR &Address, int LatencyMs)
{
	m_ServerBrowser.SetCurrentServerPing(Address, LatencyMs);
}

void CClient::RecordSnapshot(int Tick, const CSnapshot *pData, int Size)
{
	for(CDemoRecorder &Recorder : DemoRecorders())
	{
		if(Recorder.IsRecording())
		{
			Recorder.RecordSnapshot(Tick, pData, Size);
		}
	}
}

void CClient::RecordMessage(const void *pData, int Size)
{
	for(CDemoRecorder &Recorder : DemoRecorders())
	{
		if(Recorder.IsRecording())
		{
			Recorder.RecordMessage(pData, Size);
		}
	}
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
	if(m_pEditor != nullptr)
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

	CNetBase::RegisterLogCommand(m_pConsole, Storage());

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
	m_pConsole->Register("render_demo", "r[file]", CFGFLAG_CLIENT, Con_RenderDemo, this, "Queue a demo to be rendered into a video");
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
	m_pConsole->Register("render_trace_start", "i[seconds] r[file]", CFGFLAG_CLIENT | CFGFLAG_STORE, Con_RenderTraceStart, this, "Trace rendering to memory and save it to a JSON file");
	m_pConsole->Register("render_trace_stop", "", CFGFLAG_CLIENT, Con_RenderTraceStop, this, "Stop and save the active rendering trace");

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

CClient *CreateClient()
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

// The demo render tool has an entry point of its own, in demo_render_main.cpp.
#if !defined(CONF_DEMO_RENDER_TOOL)

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
	// A panic in the Rust half should fail the same way an assertion does.
	// The engine used to install this, which made every program that has a
	// job pool link the Rust library, and most of them run no Rust at all.
	rust_panic_use_dbg_assert();
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
	CleanerFunctions.emplace([]() { SDL_Quit(); });

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
	CCommandLineVideoExport VideoExport;
	std::vector<const char *> vArguments;
	if(!VideoExport.ParseArguments(argc, argv, vArguments, "DDNet"))
	{
		PerformAllCleanup();
		return -1;
	}
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
	if(VideoExport.m_Export && !pClient->ConfigureCommandLineVideoExport(VideoExport))
	{
		PerformAllCleanup();
		return -1;
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
#if defined(CONF_FAMILY_WINDOWS)
	pClient->ShellRegister();
#endif

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

#endif

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
		str_url_encode(aEscaped, PlayerName());
		str_append(aUrl, "?name=");
		str_append(aUrl, aEscaped);
	}

	m_pDDNetInfoTask = Http()->CreateGetFile(aUrl, Storage(), DDNET_INFO_FILE, IStorage::TYPE_SAVE);
	m_pDDNetInfoTask->Timeout(CTimeout{10000, 0, 500, 10});
	m_pDDNetInfoTask->SkipByFileTime(false); // Always re-download.
	// Use ipv4 so we can know the ingame ip addresses of players before they join game servers
	m_pDDNetInfoTask->IpResolve(IPRESOLVE::V4);
	Http()->Run(m_pDDNetInfoTask);
	m_InfoState = EInfoState::LOADING;
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

void CClient::FocusSessionForState(EClientState State)
{
	if(State == IClient::STATE_CONNECTING || State == IClient::STATE_ONLINE)
		m_SessionManager.SetFocused(m_NetworkSessionId);
	CClientCore::FocusSessionForState(State);
}

void CClient::OnStateChanged(EClientState State, EClientState OldState)
{
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

void CClient::OnMapLoadStarted(CSessionId SessionId)
{
	// Stop demo recording before loading a new network map.
	if(SessionId != m_NetworkSessionId)
		return;
	for(int Recorder = 0; Recorder < RECORDER_MAX; Recorder++)
		DemoRecorder(Recorder)->Stop(Recorder == RECORDER_REPLAYS ? IDemoRecorder::EStopMode::REMOVE_FILE : IDemoRecorder::EStopMode::KEEP_FILE);
}
