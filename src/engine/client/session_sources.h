#ifndef ENGINE_CLIENT_SESSION_SOURCES_H
#define ENGINE_CLIENT_SESSION_SOURCES_H

#include "connection.h"
#include "session.h"
#include "stream.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/client/enums.h>
#include <engine/serverbrowser.h>
#include <engine/shared/demo.h>
#include <engine/shared/translation_context.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class IHttpRequest;

class CServerCapabilities
{
public:
	bool m_ChatTimeoutCode = false;
	bool m_AnyPlayerFlag = false;
	bool m_PingEx = false;
	bool m_AllowDummy = false;
	bool m_SyncWeaponInput = false;
};

class CSessionSourceBase : public IGameSessionSource
{
	ESessionState m_State = ESessionState::OFFLINE;
	std::string m_Error;
	CServerInfo m_ServerInfo = {};
	bool m_Sixup = false;
	CTranslationContext m_TranslationContext;
	std::function<void()> m_UpdateFunc;
	std::function<void(const char *)> m_StopFunc;
	std::string m_StopReason;
	bool m_Updating = false;

	void Stop();

public:
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return m_Error.c_str(); }
	bool SetState(ESessionState State) override;
	void Fail(const char *pError) override;
	void Update() override;
	void RequestStop(const char *pReason = nullptr) override;
	void SetLifecycleCallbacks(std::function<void()> UpdateFunc, std::function<void(const char *)> StopFunc);
	bool IsUpdating() const { return m_Updating; }
	CServerInfo &ServerInfo() { return m_ServerInfo; }
	const CServerInfo &ServerInfo() const { return m_ServerInfo; }
	bool IsSixup() const { return m_Sixup; }
	void SetSixup(bool Sixup) { m_Sixup = Sixup; }
	CTranslationContext &TranslationContext() { return m_TranslationContext; }
	const CTranslationContext &TranslationContext() const { return m_TranslationContext; }
	void ResetMetadata()
	{
		m_ServerInfo = {};
		m_Sixup = false;
		m_TranslationContext.Reset();
	}
};

class CNetworkSessionSource : public CSessionSourceBase
{
public:
	class CStreamConnection
	{
	public:
		CStreamId m_Id;
		CConnection m_Connection;
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
	CConnection &ConnectionAt(size_t Index);
	const CConnection &ConnectionAt(size_t Index) const;
	CStreamId StreamIdAt(size_t Index) const;
	int StreamIndex(CStreamId Id) const;
	std::vector<std::unique_ptr<CStreamConnection>> &Streams() { return m_vpStreams; }
	const std::vector<std::unique_ptr<CStreamConnection>> &Streams() const { return m_vpStreams; }
	size_t NumStreams() const { return m_vpStreams.size(); }
	CSnapshotDelta &SnapshotDelta(bool Sixup) { return m_pSnapshotDeltas[Sixup]; }
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

class CDemoSessionSource : public CSessionSourceBase
{
	std::unique_ptr<CSnapshotDelta[]> m_pSnapshotDeltas = std::make_unique<CSnapshotDelta[]>(2);
	CDemoPlayer m_DemoPlayer;
	CConnection m_Connection;
	CSnapshotStorage::CHolder m_aSnapshotHolders[IClient::NUM_SNAPSHOT_TYPES];
	CSnapshotBuffer m_aaSnapshotData[IClient::NUM_SNAPSHOT_TYPES][2];

public:
	CDemoSessionSource(bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc);
	ESessionSourceType Type() const override { return ESessionSourceType::DEMO; }
	std::vector<CStreamId> StreamIds() const override { return {CStreamId(1)}; }
	CStreamId PrimaryStreamId() const override { return CStreamId(1); }
	CStreamId ActiveStreamId() const override { return CStreamId(1); }
	CDemoPlayer &DemoPlayer() { return m_DemoPlayer; }
	const CDemoPlayer &DemoPlayer() const { return m_DemoPlayer; }
	CSnapshotDelta &SnapshotDelta(bool Sixup) { return m_pSnapshotDeltas[Sixup]; }
	CConnection &Connection() { return m_Connection; }
	const CConnection &Connection() const { return m_Connection; }
	void PrepareSnapshots();
};

#endif // ENGINE_CLIENT_SESSION_SOURCES_H
