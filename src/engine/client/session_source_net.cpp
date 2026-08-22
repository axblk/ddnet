/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "session_source_net.h"

#include <base/dbg.h>

#include <algorithm>
#include <utility>

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
	(*Found)->m_NetClient.Close();
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

CNetClient *CNetworkSessionSource::NetClient(CStreamId Id)
{
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	return Found == m_vpStreams.end() ? nullptr : &(*Found)->m_NetClient;
}

const CNetClient *CNetworkSessionSource::NetClient(CStreamId Id) const
{
	const auto Found = std::find_if(m_vpStreams.begin(), m_vpStreams.end(), [Id](const auto &pStream) { return pStream->m_Id == Id; });
	return Found == m_vpStreams.end() ? nullptr : &(*Found)->m_NetClient;
}

CNetClient &CNetworkSessionSource::NetClientAt(size_t Index)
{
	dbg_assert(Index < m_vpStreams.size(), "invalid game connection");
	return m_vpStreams[Index]->m_NetClient;
}

const CNetClient &CNetworkSessionSource::NetClientAt(size_t Index) const
{
	dbg_assert(Index < m_vpStreams.size(), "invalid game connection");
	return m_vpStreams[Index]->m_NetClient;
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
