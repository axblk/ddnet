/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "session_source_net.h"

#include <base/str.h>

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
