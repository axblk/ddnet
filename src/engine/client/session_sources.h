#ifndef ENGINE_CLIENT_SESSION_SOURCES_H
#define ENGINE_CLIENT_SESSION_SOURCES_H

#include "connection.h"
#include "session.h"
#include "stream.h"

#include <engine/client/enums.h>
#include <engine/serverbrowser.h>
#include <engine/shared/demo.h>
#include <engine/shared/translation_context.h>

#include <functional>
#include <memory>
#include <vector>

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
