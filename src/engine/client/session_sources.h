#ifndef ENGINE_CLIENT_SESSION_SOURCES_H
#define ENGINE_CLIENT_SESSION_SOURCES_H

#include "connection.h"
#include "session.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/serverbrowser.h>
#include <engine/sessions.h>
#include <engine/shared/demo.h>
#include <engine/shared/translation_context.h>

#include <memory>
#include <optional>
#include <string>
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

class CSessionSourceBase : public CSessionSource
{
public:
	CConnection m_Connection;
	// Set for a session that plays on the server of another one, the dummy:
	// the server info, the protocol and the translation are kept there.
	CSessionSourceBase *m_pServerSource = nullptr;
	CServerInfo m_ServerInfo = {};
	bool m_Sixup = false;
	CTranslationContext m_TranslationContext;

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
	int64_t m_ReconnectTime = 0;

	ESessionSourceType Type() const override { return ESessionSourceType::NETWORK; }
	CSnapshotDelta &SnapshotDelta(bool Sixup) { return m_aSnapshotDeltas[Sixup]; }
	void CancelReconnect()
	{
		m_ReconnectTime = 0;
		m_HasPendingConnect = false;
		m_Password.clear();
	}
	bool ConsumeReconnect(int64_t Now);
	/**
	 * Remembers a connect that the server asked for from within the session
	 * update. The session only stops once that update returns, so the connect
	 * is carried out by the next client update.
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
	bool ConsumePendingConnect(std::string &Address, std::string &Password);
	void ResetAfterDisconnect(const char *pError, int ReconnectFull, int ReconnectTimeout, int64_t Now, int64_t Frequency);
	void ResetNetworkMetadata();

private:
	CSnapshotDelta m_aSnapshotDeltas[2];
	std::string m_PendingConnectAddress;
	std::string m_PendingConnectPassword;
	bool m_HasPendingConnect = false;
};

class CDemoSessionSource : public CSessionSourceBase
{
	CSnapshotDelta m_aSnapshotDeltas[2];
	CSnapshotStorage::CHolder m_aSnapshotHolders[ISessions::NUM_SNAPSHOT_TYPES];
	CSnapshotBuffer m_aaSnapshotData[ISessions::NUM_SNAPSHOT_TYPES][2];

public:
	CDemoPlayer m_DemoPlayer;

	CDemoSessionSource(bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc);
	ESessionSourceType Type() const override { return ESessionSourceType::DEMO; }
	CSnapshotDelta &SnapshotDelta(bool Sixup) { return m_aSnapshotDeltas[Sixup]; }
	void PrepareSnapshots();
};

#endif // ENGINE_CLIENT_SESSION_SOURCES_H
