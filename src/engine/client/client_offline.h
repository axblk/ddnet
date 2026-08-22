/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_OFFLINE_H
#define ENGINE_CLIENT_CLIENT_OFFLINE_H

#include "checksum.h"
#include "client_core.h"

#include <base/net.h>

#include <string>
#include <vector>

/**
 * The answers of a program that never opens a connection.
 *
 * `IClient` still covers connecting, the dummy, rcon, map downloads, demo and
 * ghost recording and the server browser in one interface, so a program that
 * does none of that has to answer for all of it anyway. It answers here, once,
 * instead of every such program carrying its own set of empty methods, and
 * nothing of the connection is linked to make that happen.
 *
 * This disappears when `IClient` is split into the core and the two halves
 * that a program picks up: what is left over here is exactly the half that
 * belongs to a connection.
 */
class CClientWithoutConnection : public CClientCore
{
	// The game fills this in whether or not anyone is going to ask for it, and
	// only a server ever asks.
	CChecksumData m_ChecksumData;

public:
	using CClientCore::ConnectionProblems;
	using CClientCore::EnterGame;
	using CClientCore::GetInput;
	using CClientCore::SendMsg;

	CChecksumData *ChecksumData() override { return &m_ChecksumData; }
	CSessionId NetworkSessionId() const override { return {}; }
	EInfoState InfoState() const override { return EInfoState::ERROR; }
	IFriends *Foes() override { return nullptr; }
	bool ConnectionProblems(CSessionId SessionId, CStreamId StreamId) const override { return false; }
	bool DummyAllowed() const override { return false; }
	bool DummyConnected() const override { return false; }
	bool DummyConnecting() const override { return false; }
	bool DummyConnectingDelayed() const override { return false; }
	bool EditorHasUnsavedData() const override { return false; }
	bool RaceRecord_IsRecording() override { return false; }
	bool RconAuthed() const override { return false; }
	bool ReceivingMaplist() const override { return false; }
	bool ReceivingRconCommands() const override { return false; }
	bool ServerCapAnyPlayerFlag(CSessionId SessionId) const override { return false; }
	bool UseTempRconCommands() const override { return false; }
	class IDemoRecorder *DemoRecorder(int Recorder) override { return nullptr; }
	const NETADDR &ServerAddress() const override
	{
		static const NETADDR s_Address = NETADDR_ZEROED;
		return s_Address;
	}
	const char *ConnectAddressString() const override { return ""; }
	const char *LatestVersion() const override { return ""; }
	const char *MapDownloadName() const override { return ""; }
	const std::vector<std::string> &MaplistEntries() const override
	{
		static const std::vector<std::string> s_vEmpty;
		return s_vEmpty;
	}
	float GotMaplistPercentage() const override { return 0.0f; }
	float GotRconCommandsPercentage() const override { return 0.0f; }
	int *GetInput(CSessionId SessionId, CStreamId StreamId, int Tick) const override { return nullptr; }
	int ConnectNetTypes() const override { return 0; }
	int MapDownloadAmount() const override { return 0; }
	int MapDownloadTotalsize() const override { return 0; }
	int SendMsg(CSessionId SessionId, CStreamId StreamId, CMsgPacker *pMsg, int Flags) override { return 0; }
	int UdpConnectivity(int NetType) override { return 0; }
	int64_t ReconnectTime() const override { return 0; }
	void AutoCSV_Start() override {}
	void AutoScreenshot_Start() override {}
	void AutoStatScreenshot_Start() override {}
	void CancelReconnect() override {}
	void Connect(const char *pAddress, const char *pPassword) override {}
	void DemoRecorder_HandleAutoStart() override {}
	void DemoRecorder_Start(const char *pFilename, bool WithTimestamp, int Recorder) override {}
	void DemoRecorder_UpdateReplayRecorder() override {}
	void DemoSlice(const char *pDstPath, CLIENTFUNC_FILTER pfnFilter, void *pUser) override {}
	void DemoSliceBegin() override {}
	void DemoSliceEnd() override {}
	void Disconnect() override {}
	void DummyConnect() override {}
	void DummyDisconnect(const char *pReason) override {}
	void EnterGame(CSessionId SessionId, CStreamId StreamId) override {}
	void GenerateTimeoutSeed() override {}
	void RaceRecord_Start(const char *pFilename) override {}
	void RaceRecord_Stop() override {}
	void Rcon(const char *pLine) override {}
	void RconAuth(int Conn, const char *pUsername, const char *pPassword) override {}
	void RequestDDNetInfo() override {}
	void ServerBrowserUpdate() override {}

#if defined(CONF_FAMILY_WINDOWS)
	void ShellRegister() override {}
	void ShellUnregister() override {}
#endif
};

#endif // ENGINE_CLIENT_CLIENT_OFFLINE_H
