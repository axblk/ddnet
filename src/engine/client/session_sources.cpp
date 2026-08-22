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
