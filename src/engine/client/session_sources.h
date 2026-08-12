#ifndef ENGINE_CLIENT_SESSION_SOURCES_H
#define ENGINE_CLIENT_SESSION_SOURCES_H

#include "connection.h"
#include "session.h"
#include "stream.h"

#include <engine/client/enums.h>
#include <engine/serverbrowser.h>
#include <engine/shared/demo.h>
#include <engine/shared/translation_context.h>

#include <memory>
#include <vector>

class CSessionSourceBase : public IGameSessionSource
{
	ESessionState m_State = ESessionState::OFFLINE;
	std::string m_Error;
	CServerInfo m_ServerInfo = {};
	bool m_Sixup = false;
	CTranslationContext m_TranslationContext;

public:
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return m_Error.c_str(); }
	void SetState(ESessionState State) override;
	void Fail(const char *pError) override;
	void Update() override;
	void RequestStop() override;
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

		explicit CStreamConnection(CStreamId Id) :
			m_Id(Id)
		{
		}
	};

private:
	uint64_t m_NextStreamId = 1;
	std::vector<std::unique_ptr<CStreamConnection>> m_vpStreams;
	std::unique_ptr<CSnapshotDelta[]> m_pSnapshotDeltas = std::make_unique<CSnapshotDelta[]>(2);

public:
	CNetworkSessionSource();
	ESessionSourceType Type() const override { return ESessionSourceType::NETWORK; }
	CStreamId CreateStream();
	bool DestroyStream(CStreamId Id);
	CConnection *Connection(CStreamId Id);
	const CConnection *Connection(CStreamId Id) const;
	CConnection &ConnectionAt(size_t Index);
	const CConnection &ConnectionAt(size_t Index) const;
	CStreamId StreamIdAt(size_t Index) const;
	std::vector<std::unique_ptr<CStreamConnection>> &Streams() { return m_vpStreams; }
	const std::vector<std::unique_ptr<CStreamConnection>> &Streams() const { return m_vpStreams; }
	size_t NumStreams() const { return m_vpStreams.size(); }
	CSnapshotDelta &SnapshotDelta(bool Sixup) { return m_pSnapshotDeltas[Sixup]; }
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
	CDemoPlayer &DemoPlayer() { return m_DemoPlayer; }
	const CDemoPlayer &DemoPlayer() const { return m_DemoPlayer; }
	CSnapshotDelta &SnapshotDelta(bool Sixup) { return m_pSnapshotDeltas[Sixup]; }
	CConnection &Connection() { return m_Connection; }
	const CConnection &Connection() const { return m_Connection; }
	void PrepareSnapshots();
	void RequestStop() override;
};

#endif // ENGINE_CLIENT_SESSION_SOURCES_H
