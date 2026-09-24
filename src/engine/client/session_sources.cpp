#include "session_sources.h"

#include <base/mem.h>
#include <base/str.h>

#include <utility>

bool CNetworkSessionSource::ConsumeReconnect(int64_t Now)
{
	if(m_ReconnectTime <= 0 || Now <= m_ReconnectTime)
		return false;
	m_ReconnectTime = 0;
	return true;
}

bool CNetworkSessionSource::ConsumePendingConnect(std::string &Address, std::string &Password)
{
	if(!m_HasPendingConnect)
		return false;
	m_HasPendingConnect = false;
	m_ReconnectTime = 0;
	Address = m_PendingConnectAddress;
	Password = m_PendingConnectPassword;
	return true;
}

void CNetworkSessionSource::ResetNetworkMetadata()
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
	m_Connection.m_RconAuthed = 0;
	m_MapDetails.reset();
	m_PingInfoType = -1;
	m_PingBasicToken = -1;
	m_PingToken = -1;
	m_PingUuid = UUID_ZEROED;
	m_CurrentPingTime = -1;
	m_NextPingTime = -1;
	m_ReconnectTime = 0;
}

void CNetworkSessionSource::ResetAfterDisconnect(const char *pError, int ReconnectFull, int ReconnectTimeout, int64_t Now, int64_t Frequency)
{
	std::string Password = std::move(m_Password);
	ResetNetworkMetadata();
	if(pError == nullptr)
		return;
	if(ReconnectFull > 0 && (str_find_nocase(pError, "full") || str_find_nocase(pError, "reserved")))
		m_ReconnectTime = Now + Frequency * ReconnectFull;
	else if(ReconnectTimeout > 0 && (str_find_nocase(pError, "Timeout") || str_find_nocase(pError, "Too weak connection")))
		m_ReconnectTime = Now + Frequency * ReconnectTimeout;
	if(m_ReconnectTime > 0)
		m_Password = std::move(Password);
}

CDemoSessionSource::CDemoSessionSource(bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc) :
	m_DemoPlayer(&m_aSnapshotDeltas[0], &m_aSnapshotDeltas[1], UseVideo, std::move(UpdateIntraTimesFunc))
{
	mem_zero(m_aSnapshotHolders, sizeof(m_aSnapshotHolders));
	mem_zero(m_aaSnapshotData, sizeof(m_aaSnapshotData));
}

void CDemoSessionSource::PrepareSnapshots()
{
	m_Connection.ResetGameplay();
	mem_zero(m_aSnapshotHolders, sizeof(m_aSnapshotHolders));
	mem_zero(m_aaSnapshotData, sizeof(m_aaSnapshotData));
	for(int SnapshotType = 0; SnapshotType < ISessions::NUM_SNAPSHOT_TYPES; SnapshotType++)
	{
		CSnapshotStorage::CHolder &Holder = m_aSnapshotHolders[SnapshotType];
		Holder.m_pSnap = m_aaSnapshotData[SnapshotType][0].AsSnapshot();
		Holder.m_pAltSnap = m_aaSnapshotData[SnapshotType][1].AsSnapshot();
		Holder.m_SnapSize = 0;
		Holder.m_AltSnapSize = 0;
		Holder.m_Tick = -1;
		m_Connection.m_apSnapshots[SnapshotType] = &Holder;
	}
}
