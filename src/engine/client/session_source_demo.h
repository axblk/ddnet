/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_SESSION_SOURCE_DEMO_H
#define ENGINE_CLIENT_SESSION_SOURCE_DEMO_H

#include "session_sources.h"

#include <engine/shared/demo.h>

#include <memory>

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
	CSnapshotDelta &SnapshotDelta(bool Sixup) override { return m_pSnapshotDeltas[Sixup]; }
	CConnection &Connection() { return m_Connection; }
	const CConnection &Connection() const { return m_Connection; }
	CConnection *StreamConnection(CStreamId Id) override { return Id == PrimaryStreamId() ? &m_Connection : nullptr; }
	const CConnection *StreamConnection(CStreamId Id) const override { return Id == PrimaryStreamId() ? &m_Connection : nullptr; }
	// A demo is one stream, and it is the one the legacy connection numbers
	// call the main connection.
	CStreamId StreamIdForIndex(int Index) const override { return Index == 0 ? PrimaryStreamId() : CStreamId{}; }
	int IndexForStream(CStreamId Id) const override { return Id == PrimaryStreamId() ? 0 : -1; }
	void PrepareSnapshots();
};

#endif // ENGINE_CLIENT_SESSION_SOURCE_DEMO_H
