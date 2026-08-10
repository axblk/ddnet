#include "session_sources.h"

#include <base/dbg.h>

#include <algorithm>
#include <utility>

void CSessionSourceBase::SetState(ESessionState State)
{
	m_State = State;
	if(State != ESessionState::ERROR)
		m_Error.clear();
}

void CSessionSourceBase::Fail(const char *pError)
{
	m_Error = pError ? pError : "";
	m_State = ESessionState::ERROR;
}

void CSessionSourceBase::Update()
{
	if(m_State == ESessionState::STOPPING)
		SetState(ESessionState::OFFLINE);
}

void CSessionSourceBase::RequestStop()
{
	if(m_State != ESessionState::OFFLINE)
		SetState(ESessionState::STOPPING);
}

CNetworkSessionSource::CNetworkSessionSource()
{
	CreateStream();
	CreateStream();
}

CStreamId CNetworkSessionSource::CreateStream()
{
	const CStreamId Id(m_NextStreamId++);
	m_vpStreams.push_back(std::make_unique<CStreamConnection>(Id));
	return Id;
}

bool CNetworkSessionSource::DestroyStream(CStreamId Id)
{
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	if(Found == m_vpStreams.end())
		return false;
	(*Found)->m_Connection.m_NetClient.Close();
	m_vpStreams.erase(Found);
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

CDemoSessionSource::CDemoSessionSource(CSnapshotDelta *pSnapshotDelta, CSnapshotDelta *pSnapshotDeltaSixup, bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc) :
	m_DemoPlayer(pSnapshotDelta, pSnapshotDeltaSixup, UseVideo, std::move(UpdateIntraTimesFunc))
{
}

void CDemoSessionSource::RequestStop()
{
	m_DemoPlayer.Stop();
	CSessionSourceBase::RequestStop();
}
