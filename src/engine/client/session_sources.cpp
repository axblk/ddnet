#include "session_sources.h"

#include <base/dbg.h>
#include <base/mem.h>
#include <base/str.h>

#include <algorithm>
#include <utility>

bool CSessionSourceBase::SetState(ESessionState State)
{
	const bool Valid = State == m_State ||
			   State == ESessionState::OFFLINE ||
			   State == ESessionState::ERROR ||
			   (State == ESessionState::STOPPING && m_State != ESessionState::OFFLINE) ||
			   (m_State == ESessionState::OFFLINE && (State == ESessionState::CONNECTING || State == ESessionState::LOADING_MAP)) ||
			   (m_State == ESessionState::CONNECTING && State == ESessionState::LOADING_MAP) ||
			   (m_State == ESessionState::LOADING_MAP && State == ESessionState::READY) ||
			   (m_State == ESessionState::READY && State == ESessionState::LOADING_MAP);
	if(!Valid)
		return false;
	m_State = State;
	if(State != ESessionState::ERROR)
		m_Error.clear();
	return true;
}

void CSessionSourceBase::Fail(const char *pError)
{
	m_Error = pError ? pError : "";
	m_State = ESessionState::ERROR;
}

void CSessionSourceBase::Update()
{
	if(m_State == ESessionState::STOPPING)
	{
		Stop();
		return;
	}
	if(m_UpdateFunc)
	{
		m_Updating = true;
		m_UpdateFunc();
		m_Updating = false;
	}
	if(m_State == ESessionState::STOPPING)
		Stop();
}

void CSessionSourceBase::RequestStop(const char *pReason)
{
	if(m_State != ESessionState::OFFLINE)
	{
		m_StopReason = pReason ? pReason : "";
		SetState(ESessionState::STOPPING);
	}
}

void CSessionSourceBase::SetLifecycleCallbacks(std::function<void()> UpdateFunc, std::function<void(const char *)> StopFunc)
{
	m_UpdateFunc = std::move(UpdateFunc);
	m_StopFunc = std::move(StopFunc);
}

void CSessionSourceBase::Stop()
{
	if(m_StopFunc)
		m_StopFunc(m_StopReason.empty() ? nullptr : m_StopReason.c_str());
	m_StopReason.clear();
	SetState(ESessionState::OFFLINE);
}

CNetworkSessionSource::CNetworkSessionSource()
{
	CreateStream();
	CreateStream();
}

void CNetworkSessionSource::ScheduleReconnect(const char *pError, int ReconnectFull, int ReconnectTimeout, int64_t Now, int64_t Frequency)
{
	m_ReconnectTime = 0;
	if(pError == nullptr)
		return;
	if(ReconnectFull > 0 && (str_find_nocase(pError, "full") || str_find_nocase(pError, "reserved")))
		m_ReconnectTime = Now + Frequency * ReconnectFull;
	else if(ReconnectTimeout > 0 && (str_find_nocase(pError, "Timeout") || str_find_nocase(pError, "Too weak connection")))
		m_ReconnectTime = Now + Frequency * ReconnectTimeout;
}

bool CNetworkSessionSource::ConsumeReconnect(int64_t Now)
{
	if(m_ReconnectTime <= 0 || Now <= m_ReconnectTime)
		return false;
	m_ReconnectTime = 0;
	return true;
}

void CNetworkSessionSource::ResetAfterDisconnect(const char *pError, int ReconnectFull, int ReconnectTimeout, int64_t Now, int64_t Frequency)
{
	const std::string Password = m_Password;
	ResetNetworkMetadata();
	ScheduleReconnect(pError, ReconnectFull, ReconnectTimeout, Now, Frequency);
	if(m_ReconnectTime > 0)
		m_Password = Password;
}

CStreamId CNetworkSessionSource::CreateStream()
{
	const CStreamId Id(m_NextStreamId++);
	m_vpStreams.push_back(std::make_unique<CStreamConnection>(Id));
	if(!m_PrimaryStreamId.IsValid())
	{
		m_PrimaryStreamId = Id;
		m_ActiveStreamId = Id;
		m_LastActiveStreamId = Id;
	}
	return Id;
}

bool CNetworkSessionSource::DestroyStream(CStreamId Id)
{
	if(Id == m_PrimaryStreamId)
		return false;
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	if(Found == m_vpStreams.end())
		return false;
	(*Found)->m_Connection.m_NetClient.Close();
	m_vpStreams.erase(Found);
	if(m_ActiveStreamId == Id)
		m_ActiveStreamId = m_PrimaryStreamId;
	if(m_LastActiveStreamId == Id)
	{
		m_LastActiveStreamId = m_ActiveStreamId;
	}
	return true;
}

std::vector<CStreamId> CNetworkSessionSource::StreamIds() const
{
	std::vector<CStreamId> vIds;
	vIds.reserve(m_vpStreams.size());
	for(const auto &pStream : m_vpStreams)
		vIds.push_back(pStream->m_Id);
	return vIds;
}

bool CNetworkSessionSource::SetActiveStream(CStreamId Id)
{
	if(Connection(Id) == nullptr)
		return false;
	m_ActiveStreamId = Id;
	return true;
}

CConnection *CNetworkSessionSource::Connection(CStreamId Id)
{
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	return Found == m_vpStreams.end() ? nullptr : &(*Found)->m_Connection;
}

const CConnection *CNetworkSessionSource::Connection(CStreamId Id) const
{
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	return Found == m_vpStreams.end() ? nullptr : &(*Found)->m_Connection;
}

CConnection &CNetworkSessionSource::ConnectionAt(size_t Index)
{
	dbg_assert(Index < m_vpStreams.size(), "invalid game connection");
	return m_vpStreams[Index]->m_Connection;
}

const CConnection &CNetworkSessionSource::ConnectionAt(size_t Index) const
{
	dbg_assert(Index < m_vpStreams.size(), "invalid game connection");
	return m_vpStreams[Index]->m_Connection;
}

CStreamId CNetworkSessionSource::StreamIdAt(size_t Index) const
{
	return Index < m_vpStreams.size() ? m_vpStreams[Index]->m_Id : CStreamId{};
}

int CNetworkSessionSource::StreamIndex(CStreamId Id) const
{
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	return Found == m_vpStreams.end() ? -1 : static_cast<int>(Found - m_vpStreams.begin());
}

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
