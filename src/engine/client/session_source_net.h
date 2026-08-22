/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_SESSION_SOURCE_NET_H
#define ENGINE_CLIENT_SESSION_SOURCE_NET_H

#include "session_sources.h"

#include <base/hash.h>

#include <engine/shared/network.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class IHttpRequest;

class CNetworkSessionSource : public CSessionSourceBase
{
public:
	class CStreamConnection
	{
	public:
		CStreamId m_Id;
		CConnection m_Connection;
		// The socket of this stream. It belongs to the stream and not to the
		// connection state, because a connection state is also what a demo has,
		// and a demo has no socket.
		CNetClient m_NetClient;
		bool m_SendConnectionInfo = false;

		explicit CStreamConnection(CStreamId Id) :
			m_Id(Id)
		{
		}
	};

private:
	uint64_t m_NextStreamId = 1;
	std::vector<std::unique_ptr<CStreamConnection>> m_vpStreams;
	std::unique_ptr<CSnapshotDelta[]> m_pSnapshotDeltas = std::make_unique<CSnapshotDelta[]>(2);
	CStreamId m_PrimaryStreamId;
	CStreamId m_ActiveStreamId;
	CStreamId m_LastActiveStreamId;

public:
	class CMapDetails
	{
	public:
		char m_aName[256] = "";
		int m_Size = 0;
		int m_Crc = 0;
		SHA256_DIGEST m_Sha256 = {};
		char m_aUrl[256] = "";
	};

	std::string m_ConnectAddress;
	CUuid m_ConnectionId = UUID_ZEROED;
	std::string m_Password;
	bool m_SendPassword = false;
	bool m_CanReceiveServerCapabilities = false;
	bool m_ServerSentCapabilities = false;
	CServerCapabilities m_ServerCapabilities;
	int m_UseTempRconCommands = 0;
	int m_ExpectedRconCommands = -1;
	int m_GotRconCommands = 0;
	int m_ExpectedMaplistEntries = -1;
	std::vector<std::string> m_vMaplistEntries;
	std::shared_ptr<IHttpRequest> m_pMapdownloadTask;
	char m_aMapdownloadFilename[256] = "";
	char m_aMapdownloadFilenameTemp[256] = "";
	char m_aMapdownloadName[256] = "";
	IOHANDLE m_MapdownloadFileTemp = nullptr;
	int m_MapdownloadChunk = 0;
	int m_MapdownloadCrc = 0;
	int m_MapdownloadAmount = -1;
	int m_MapdownloadTotalsize = -1;
	std::optional<SHA256_DIGEST> m_MapdownloadSha256;
	std::optional<CMapDetails> m_MapDetails;
	int m_PingInfoType = -1;
	int m_PingBasicToken = -1;
	int m_PingToken = -1;
	CUuid m_PingUuid = UUID_ZEROED;
	int64_t m_CurrentPingTime = -1;
	int64_t m_NextPingTime = -1;

	CNetworkSessionSource();
	ESessionSourceType Type() const override { return ESessionSourceType::NETWORK; }
	std::vector<CStreamId> StreamIds() const override;
	CStreamId PrimaryStreamId() const override { return m_PrimaryStreamId; }
	CStreamId ActiveStreamId() const override { return m_ActiveStreamId; }
	CStreamId LastActiveStreamId() const { return m_LastActiveStreamId; }
	void SetLastActiveStreamId(CStreamId Id) { m_LastActiveStreamId = Id; }
	bool SetActiveStream(CStreamId Id);
	CStreamId CreateStream();
	bool DestroyStream(CStreamId Id);
	CConnection *Connection(CStreamId Id);
	const CConnection *Connection(CStreamId Id) const;
	CNetClient *NetClient(CStreamId Id);
	const CNetClient *NetClient(CStreamId Id) const;
	CConnection *StreamConnection(CStreamId Id) override { return Connection(Id); }
	const CConnection *StreamConnection(CStreamId Id) const override { return Connection(Id); }
	CStreamId StreamIdForIndex(int Index) const override { return Index < 0 ? CStreamId{} : StreamIdAt(static_cast<size_t>(Index)); }
	int IndexForStream(CStreamId Id) const override { return StreamIndex(Id); }
	CConnection &ConnectionAt(size_t Index);
	const CConnection &ConnectionAt(size_t Index) const;
	CNetClient &NetClientAt(size_t Index);
	const CNetClient &NetClientAt(size_t Index) const;
	CNetClient &PrimaryNetClient() { return *NetClient(PrimaryStreamId()); }
	const CNetClient &PrimaryNetClient() const { return *NetClient(PrimaryStreamId()); }
	CStreamId StreamIdAt(size_t Index) const;
	int StreamIndex(CStreamId Id) const;
	std::vector<std::unique_ptr<CStreamConnection>> &Streams() { return m_vpStreams; }
	const std::vector<std::unique_ptr<CStreamConnection>> &Streams() const { return m_vpStreams; }
	size_t NumStreams() const { return m_vpStreams.size(); }
	CSnapshotDelta &SnapshotDelta(bool Sixup) override { return m_pSnapshotDeltas[Sixup]; }
	bool SyncWeaponInput() const override { return m_ServerCapabilities.m_SyncWeaponInput; }
	int64_t ReconnectTime() const { return m_ReconnectTime; }
	void CancelReconnect()
	{
		m_ReconnectTime = 0;
		m_HasPendingConnect = false;
		m_Password.clear();
	}
	void ScheduleReconnect(const char *pError, int ReconnectFull, int ReconnectTimeout, int64_t Now, int64_t Frequency);
	bool ConsumeReconnect(int64_t Now);
	/**
	 * Remembers a connect that the server asked for. Closing a session only
	 * requests the stop; the stop itself runs when the session update returns,
	 * so a connect started from within that update would find the session still
	 * stopping. The request is therefore carried out by the next client update.
	 *
	 * @param pAddress Address to connect to once the session is offline.
	 * @param pPassword Password to use for that connect.
	 */
	void ScheduleServerConnect(const char *pAddress, const char *pPassword)
	{
		m_PendingConnectAddress = pAddress;
		m_PendingConnectPassword = pPassword;
		m_HasPendingConnect = true;
	}
	/**
	 * Takes over a connect previously remembered by
	 * @link ScheduleServerConnect @endlink.
	 *
	 * @param Address Receives the address to connect to.
	 * @param Password Receives the password to use.
	 *
	 * @return `true` if a connect was pending.
	 */
	bool ConsumePendingConnect(std::string &Address, std::string &Password)
	{
		if(!m_HasPendingConnect)
			return false;
		m_HasPendingConnect = false;
		m_ReconnectTime = 0;
		Address = m_PendingConnectAddress;
		Password = m_PendingConnectPassword;
		return true;
	}
	void ResetAfterDisconnect(const char *pError, int ReconnectFull, int ReconnectTimeout, int64_t Now, int64_t Frequency);
	void ResetNetworkMetadata()
	{
		ResetMetadata();
		m_Password.clear();
		m_SendPassword = false;
		m_CanReceiveServerCapabilities = false;
		m_ServerSentCapabilities = false;
		m_ServerCapabilities = {};
		m_UseTempRconCommands = 0;
		m_ExpectedRconCommands = -1;
		m_GotRconCommands = 0;
		m_ExpectedMaplistEntries = -1;
		m_vMaplistEntries.clear();
		for(const auto &pStream : m_vpStreams)
			pStream->m_Connection.m_RconAuthed = 0;
		m_MapDetails.reset();
		m_PingInfoType = -1;
		m_PingBasicToken = -1;
		m_PingToken = -1;
		m_PingUuid = UUID_ZEROED;
		m_CurrentPingTime = -1;
		m_NextPingTime = -1;
		m_ReconnectTime = 0;
		m_ActiveStreamId = m_PrimaryStreamId;
		m_LastActiveStreamId = m_PrimaryStreamId;
	}

private:
	int64_t m_ReconnectTime = 0;
	std::string m_PendingConnectAddress;
	std::string m_PendingConnectPassword;
	bool m_HasPendingConnect = false;
};

#endif // ENGINE_CLIENT_SESSION_SOURCE_NET_H
