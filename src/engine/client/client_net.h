/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_NET_H
#define ENGINE_CLIENT_CLIENT_NET_H

#include "client_core.h"
#include "session_source_net.h"

#include <base/hash.h>

#include <engine/console.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/shared/network.h>
#include <engine/shared/quic_transport.h>

#include <string>
#include <vector>

class CMsgPacker;
class CUnpacker;
class IConfigManager;
class IEngineHttp;

/**
 * The half of the client that talks to a server: the sessions that have a
 * socket, connecting and disconnecting, the dummy, rcon, the map download and
 * everything that arrives over a wire.
 *
 * It knows nothing about drawing. A program that draws puts a presentation on
 * top of it, and a program that does not - a headless client for AI clients or
 * for stress testing a server - runs it as it is. What it wants from the
 * program above it, a server browser's knowledge above all, it asks for through
 * the hooks below, which answer nothing by default.
 */
class CClientWithConnection : public CClientCore
{
protected:
	struct CQuicKnownHost
	{
		char m_aHost[128];
		int m_Port;
		SHA256_DIGEST m_IdentityFingerprint;
	};

	IEngineHttp *m_pHttp = nullptr;

	CSessionId m_NetworkSessionId;
	CNetworkSessionSource *m_pNetworkSessionSource = nullptr;

	CNetClient m_ContactNetClient;
	NETADDR m_NetworkBindAddr = NETADDR_ZEROED;
	bool m_NetworkInitialized = false;
	CQuicTransport m_QuicTransport;
	CQuicSessionId m_QuicSession;
	NETADDR m_QuicServerAddress = {};
	bool m_UseQuic = false;
	bool m_UseWebTransport = false;
	bool m_QuicConnected = false;
	// When the last message arrived over QUIC, for the connection warning
	int64_t m_QuicLastRecvTime = 0;
	std::vector<CQuicKnownHost> m_vQuicKnownHosts;
	char m_aQuicTrustHost[128] = {};
	int m_QuicTrustPort = 0;
	SHA256_DIGEST m_QuicExpectedIdentity = {};
	bool m_QuicIdentityRequired = false;
	bool m_QuicIdentityKnown = false;
	bool m_QuicRememberIdentity = false;

	bool m_HaveGlobalTcpAddr = false;
	NETADDR m_GlobalTcpAddr = NETADDR_ZEROED;

	char m_aRconUsername[64] = "";
	char m_aRconPassword[sizeof(g_Config.m_SvRconPassword)] = "";

	// pinging
	int64_t m_PingStartTime = 0;
	int64_t m_CurrentServerInfoRequestTime = -1; // >= 0 should request, == -1 got info

	bool m_GenerateTimeoutSeed = true;

	bool m_DummySendConnInfo = false;
	bool m_DummyConnecting = false;
	bool m_DummyConnected = false;
	float m_LastDummyConnectTime = 0.0f;
	bool m_DummyReconnectOnReload = false;
	bool m_DummyDeactivateOnReconnect = false;

	// For DummyName function
	char m_aAutomaticDummyName[MAX_NAME_LENGTH] = "";

	CNetworkSessionSource &NetworkSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::NETWORK, "game session is not a Network source");
		return static_cast<CNetworkSessionSource &>(Source);
	}
	const CNetworkSessionSource &NetworkSource(CSessionId SessionId) const
	{
		const CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::NETWORK, "game session is not a Network source");
		return static_cast<const CNetworkSessionSource &>(Source);
	}
	using CClientCore::Connection;
	CConnection &Connection(int Conn)
	{
		return m_pNetworkSessionSource->ConnectionAt(Conn);
	}
	const CConnection &Connection(int Conn) const
	{
		return m_pNetworkSessionSource->ConnectionAt(Conn);
	}
	CNetClient &NetClient(int Conn)
	{
		dbg_assert(Conn >= CONN_MAIN && Conn < NUM_CONNS, "invalid network connection");
		return Conn == CONN_CONTACT ? m_ContactNetClient : m_pNetworkSessionSource->NetClientAt(Conn);
	}
	const CNetClient &NetClient(int Conn) const
	{
		dbg_assert(Conn >= CONN_MAIN && Conn < NUM_CONNS, "invalid network connection");
		return Conn == CONN_CONTACT ? m_ContactNetClient : m_pNetworkSessionSource->NetClientAt(Conn);
	}

	// ----- what only a program with a server browser can answer -----
	/**
	 * What is already known about a server, which is where a connect finds out
	 * whether the server speaks QUIC and on which port.
	 *
	 * @param Address Address of the server to look up.
	 *
	 * @return `nullptr` when nothing is known about it, which is the answer of
	 * a program that browses no servers.
	 */
	virtual const CServerInfo *KnownServerInfo(const NETADDR &Address) { return nullptr; }
	/**
	 * Asks the server the client is connected to for its info again.
	 *
	 * @param Address Address of the server to ask.
	 */
	virtual void RequestServerInfoRefresh(const NETADDR &Address) {}
	/**
	 * The same request, used as a ping by a server that has no `PINGEX`.
	 *
	 * @param Address Address of the server to ask.
	 * @param pBasicToken Receives the token of the vanilla request.
	 * @param pToken Receives the token of the extended request.
	 */
	virtual void RequestServerInfoWithToken(const NETADDR &Address, int *pBasicToken, int *pToken) {}
	/**
	 * How long the server took to answer.
	 *
	 * @param Address Address of the server that answered.
	 * @param LatencyMs Time the answer took, in milliseconds.
	 */
	virtual void OnCurrentServerPing(const NETADDR &Address, int LatencyMs) {}
	/**
	 * Where a map that the server does not send itself is downloaded from.
	 *
	 * @return The base URL, empty when there is nowhere to download from.
	 */
	virtual const char *MapDownloadUrl() const { return ""; }
	/**
	 * A packet that arrived without a connection behind it. Only the server
	 * info that a browser collects comes that way.
	 *
	 * @param pPacket The packet that arrived.
	 */
	virtual void ProcessConnlessPacket(CNetChunk *pPacket) {}
	/**
	 * Writes a message that was sent to the server into whatever demo the
	 * program is recording. A program that records nothing writes nothing.
	 *
	 * @param pData The packed message.
	 * @param Size Size of the packed message.
	 */
	virtual void RecordMessage(const void *pData, int Size) {}
	/**
	 * Writes a snapshot into whatever demo the program is recording.
	 *
	 * @param Tick Tick the snapshot belongs to.
	 * @param pData The snapshot.
	 * @param Size Size of the snapshot.
	 */
	virtual void RecordSnapshot(int Tick, const class CSnapshot *pData, int Size) {}
	/** Stops the demo that is being played back, if the program plays one. */
	virtual void DisconnectDemoWithReason(const char *pReason) {}
	/**
	 * Answers the checksum the server challenges the client with, which only a
	 * program that has an executable and components to sum up can do.
	 *
	 * @return Nonzero when the challenge cannot be answered.
	 */
	virtual int HandleChecksum(CSessionId SessionId, CStreamId StreamId, CUuid Uuid, CUnpacker *pUnpacker) { return 1; }
	/**
	 * Turns a 0.7 connless packet into one the rest can read.
	 *
	 * @return `false` when the packet is not for us.
	 */
	virtual bool PreprocessConnlessPacket7(CNetChunk *pPacket) { return true; }
	/**
	 * Connects to what a `ddnet://` link names. A program that has a startup of
	 * its own may want to remember it instead of connecting right away.
	 *
	 * @param pLink The link, with or without its scheme.
	 */
	virtual void HandleConnectLink(const char *pLink);

public:
	using IClient::EnterGame;
	using IClient::SendMsg;

	IHttp *Http();

	void SetActiveConnection(int Conn) override
	{
		IClient::SetActiveConnection(Conn);
		if(m_pNetworkSessionSource != nullptr)
			m_pNetworkSessionSource->SetActiveStream(m_pNetworkSessionSource->StreamIdAt(Conn));
	}
	CSessionId NetworkSessionId() const override { return m_NetworkSessionId; }
	bool ServerCapAnyPlayerFlag(CSessionId SessionId) const override { return NetworkSource(SessionId).m_ServerCapabilities.m_AnyPlayerFlag; }

	// ----- send functions -----
	void SendInfo(CSessionId SessionId, CStreamId StreamId);
	void SendEnterGame(int Conn);
	void SendEnterGame(CSessionId SessionId, CStreamId StreamId);
	void SendReady(int Conn);
	void SendReady(CSessionId SessionId, CStreamId StreamId);
	void SendMapRequest(CSessionId SessionId);
	void SendInput(CSessionId SessionId);
	int SendMsg(CSessionId SessionId, CStreamId StreamId, CMsgPacker *pMsg, int Flags) override;

	bool RconAuthed() const override { return Connection(ActiveConnection()).m_RconAuthed != 0; }
	bool UseTempRconCommands() const override { return m_pNetworkSessionSource->m_UseTempRconCommands != 0; }
	void RconAuth(int Conn, const char *pName, const char *pPassword) override;
	void Rcon(const char *pCmd) override;
	bool ReceivingRconCommands() const override { return m_pNetworkSessionSource->m_ExpectedRconCommands > 0; }
	float GotRconCommandsPercentage() const override;
	bool ReceivingMaplist() const override { return m_pNetworkSessionSource->m_ExpectedMaplistEntries > 0; }
	float GotMaplistPercentage() const override;
	const std::vector<std::string> &MaplistEntries() const override { return m_pNetworkSessionSource->m_vMaplistEntries; }

	bool ConnectionProblems(CSessionId SessionId, CStreamId StreamId) const override;
	int *GetInput(CSessionId SessionId, CStreamId StreamId, int Tick) const override;
	const char *ErrorString() const override;
	const char *DummyName() override;

	int64_t ReconnectTime() const override { return m_pNetworkSessionSource->ReconnectTime(); }
	void CancelReconnect() override { m_pNetworkSessionSource->CancelReconnect(); }

	// called when the map is loaded and we should init for a new round
	void OnEnterGame(int Conn);
	void EnterGame(CSessionId SessionId, CStreamId StreamId) override;
	// called once after being ingame for 1 second
	void OnPostConnect(int Conn);

	void Connect(const char *pAddress, const char *pPassword = nullptr) override;
	CSessionId CreateNetworkSession();
	CStreamId ConnectAdditionalStream(CSessionId SessionId);
	bool DestroyNetworkStream(CSessionId SessionId, CStreamId StreamId);
	bool DestroyNetworkSession(CSessionId SessionId);
	void ConnectSession(CSessionId SessionId, const char *pAddress, const char *pPassword);
	void DisconnectWithReason(const char *pReason);
	void Disconnect() override;

	void DummyDisconnect(const char *pReason) override;
	void DummyConnect() override;
	bool DummyConnected() const override;
	bool DummyConnecting() const override;
	bool DummyConnectingDelayed() const override;
	bool DummyAllowed() const override;

	void RequestServerInfo(CSessionId SessionId);
	void SetSessionServerInfo(CSessionId SessionId, const CServerInfo &ServerInfo);

	int TranslateSysMsg(CSessionId SessionId, int *pMsgId, bool System, CUnpacker *pUnpacker, CPacker *pPacker, const NETADDR *pPeerAddress, bool *pIsExMsg);
	void ProcessServerPacket(CSessionId SessionId, CStreamId StreamId, CNetChunk *pPacket);
	void ClearQuicTrust();
	const CQuicKnownHost *FindQuicKnownHost(const char *pHost, int Port) const;
	bool AddQuicKnownHost(const char *pHost, int Port, SHA256_DIGEST IdentityFingerprint);
	void StartLegacyConnection(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs, bool Sixup);
	const NETADDR &SessionServerAddress(CSessionId SessionId) const;

	void ResetMapDownload(CSessionId SessionId, bool ResetActive);
	void FinishMapDownload(CSessionId SessionId);

	const NETADDR &ServerAddress() const override { return SessionServerAddress(m_NetworkSessionId); }
	int ConnectNetTypes() const override;
	const char *ConnectAddressString() const override { return m_pNetworkSessionSource->m_ConnectAddress.c_str(); }
	const char *MapDownloadName() const override { return m_pNetworkSessionSource->m_aMapdownloadName; }
	int MapDownloadAmount() const override { return !m_pNetworkSessionSource->m_pMapdownloadTask ? m_pNetworkSessionSource->m_MapdownloadAmount : (int)m_pNetworkSessionSource->m_pMapdownloadTask->Current(); }
	int MapDownloadTotalsize() const override { return !m_pNetworkSessionSource->m_pMapdownloadTask ? m_pNetworkSessionSource->m_MapdownloadTotalsize : (int)m_pNetworkSessionSource->m_pMapdownloadTask->Size(); }

	void PumpNetwork(CSessionId SessionId);
	// Storage for UpdateNetworkSession, kept so that advancing the streams of a
	// session does not allocate on every frame. One buffer serves every session,
	// which only holds while UpdateNetworkSession is not nested: the session
	// manager updates one session at a time and nothing it calls updates another.
	// Nesting it needs a buffer per session instead.
	std::vector<CStreamId> m_vRepredict;
	void UpdateNetworkSession(CSessionId SessionId);
	void StopNetworkSession(CSessionId SessionId, const char *pReason);

	void ResetSocket();
	bool InitNetworkClient(char *pError, size_t ErrorSize);
	bool InitNetworkClientImpl(NETADDR BindAddr, int Conn, char *pError, size_t ErrorSize);
	bool InitNetworkStream(NETADDR BindAddr, CNetClient &NetClient, int &Port, const char *pName, char *pError, size_t ErrorSize);

	void GenerateTimeoutSeed() override;
	void GenerateTimeoutCodes(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs);
	int UdpConnectivity(int NetType) override;

	static void Con_Connect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgConnectSession(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgConnectStream(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDestroyStream(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDestroySession(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDumpSessions(IConsole::IResult *pResult, void *pUserData);
	static void Con_Disconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyConnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyDisconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyResetInput(IConsole::IResult *pResult, void *pUserData);
	static void Con_Ping(IConsole::IResult *pResult, void *pUserData);
	static void ConNetReset(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicReconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicKnownHost(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicForgetHost(IConsole::IResult *pResult, void *pUserData);
	static void QuicKnownHostsConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData);
	static void Con_Rcon(IConsole::IResult *pResult, void *pUserData);
	static void Con_RconAuth(IConsole::IResult *pResult, void *pUserData);
	static void Con_RconLogin(IConsole::IResult *pResult, void *pUserData);
	static void ConchainTimeoutSeed(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainPassword(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainNetReset(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
};

#endif // ENGINE_CLIENT_CLIENT_NET_H
