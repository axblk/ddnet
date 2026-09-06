#include "session.h"

#include <utility>

bool CSessionSource::SetState(ESessionState State)
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

void CSessionSource::Fail(const char *pError)
{
	m_Error = pError ? pError : "";
	m_State = ESessionState::ERROR;
}

void CSessionSource::RequestStop(const char *pReason)
{
	if(m_State == ESessionState::OFFLINE)
		return;
	m_StopReason = pReason ? pReason : "";
	m_State = ESessionState::STOPPING;
}

std::string CSessionSource::TakeStopReason()
{
	return std::exchange(m_StopReason, {});
}

CSessionId CSessionManager::Create(std::unique_ptr<CSessionSource> pSource)
{
	pSource->m_Id = CSessionId(m_NextId++);
	m_vpSessions.push_back(std::move(pSource));
	if(!m_FocusedSessionId.IsValid())
		m_FocusedSessionId = m_vpSessions.back()->Id();
	return m_vpSessions.back()->Id();
}

bool CSessionManager::SetFocused(CSessionId Id)
{
	if(!Find(Id))
		return false;
	m_FocusedSessionId = Id;
	return true;
}
