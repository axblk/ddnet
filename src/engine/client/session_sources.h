#ifndef ENGINE_CLIENT_SESSION_SOURCES_H
#define ENGINE_CLIENT_SESSION_SOURCES_H

#include "connection.h"
#include "session.h"
#include "stream.h"

#include <engine/client/enums.h>
#include <engine/shared/demo.h>

#include <memory>
#include <vector>

class CSessionSourceBase : public IGameSessionSource
{
	ESessionState m_State = ESessionState::OFFLINE;
	std::string m_Error;

public:
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return m_Error.c_str(); }
	void SetState(ESessionState State) override;
	void Fail(const char *pError) override;
	void Update() override;
	void RequestStop() override;
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
};

class CDemoSessionSource : public CSessionSourceBase
{
	CDemoPlayer m_DemoPlayer;

public:
	CDemoSessionSource(CSnapshotDelta *pSnapshotDelta, CSnapshotDelta *pSnapshotDeltaSixup, bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc);
	ESessionSourceType Type() const override { return ESessionSourceType::DEMO; }
	CDemoPlayer &DemoPlayer() { return m_DemoPlayer; }
	const CDemoPlayer &DemoPlayer() const { return m_DemoPlayer; }
	void RequestStop() override;
};

#endif // ENGINE_CLIENT_SESSION_SOURCES_H
