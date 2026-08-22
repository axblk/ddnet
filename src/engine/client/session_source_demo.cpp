/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "session_source_demo.h"

#include <base/mem.h>

#include <utility>

CDemoSessionSource::CDemoSessionSource(bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc) :
	m_DemoPlayer(&m_pSnapshotDeltas[0], &m_pSnapshotDeltas[1], UseVideo, std::move(UpdateIntraTimesFunc))
{
	mem_zero(m_aSnapshotHolders, sizeof(m_aSnapshotHolders));
	mem_zero(m_aaSnapshotData, sizeof(m_aaSnapshotData));
}

void CDemoSessionSource::PrepareSnapshots()
{
	m_Connection.ResetGameplay();
	mem_zero(m_aSnapshotHolders, sizeof(m_aSnapshotHolders));
	mem_zero(m_aaSnapshotData, sizeof(m_aaSnapshotData));
	for(int SnapshotType = 0; SnapshotType < IClient::NUM_SNAPSHOT_TYPES; SnapshotType++)
	{
		m_Connection.m_apSnapshots[SnapshotType] = &m_aSnapshotHolders[SnapshotType];
		m_Connection.m_apSnapshots[SnapshotType]->m_pSnap = m_aaSnapshotData[SnapshotType][0].AsSnapshot();
		m_Connection.m_apSnapshots[SnapshotType]->m_pAltSnap = m_aaSnapshotData[SnapshotType][1].AsSnapshot();
		m_Connection.m_apSnapshots[SnapshotType]->m_SnapSize = 0;
		m_Connection.m_apSnapshots[SnapshotType]->m_AltSnapSize = 0;
		m_Connection.m_apSnapshots[SnapshotType]->m_Tick = -1;
	}
}
