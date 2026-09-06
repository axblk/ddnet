#include "session.h"

#include <algorithm>
#include <utility>

CGameSession::CGameSession(CSessionId Id, std::unique_ptr<IGameSessionSource> pSource) :
	m_Id(Id),
	m_pSource(std::move(pSource))
{
}

CSessionId CSessionManager::Create(std::unique_ptr<IGameSessionSource> pSource)
{
	if(!pSource)
		return {};

	const CSessionId Id(m_NextId++);
	m_pLastFound = nullptr;
	m_vpSessions.push_back(std::make_unique<CGameSession>(Id, std::move(pSource)));
	if(!m_FocusedSessionId.IsValid())
		m_FocusedSessionId = Id;
	return Id;
}

CGameSession *CSessionManager::Find(CSessionId Id)
{
	if(m_pLastFound != nullptr && m_LastFoundId == Id)
		return m_pLastFound;
	const auto It = std::find_if(m_vpSessions.begin(), m_vpSessions.end(), [Id](const auto &pSession) { return pSession->Id() == Id; });
	if(It == m_vpSessions.end())
		return nullptr;
	m_LastFoundId = Id;
	m_pLastFound = It->get();
	return m_pLastFound;
}

const CGameSession *CSessionManager::Find(CSessionId Id) const
{
	return const_cast<CSessionManager *>(this)->Find(Id);
}

bool CSessionManager::SetFocused(CSessionId Id)
{
	if(!Find(Id))
		return false;
	m_FocusedSessionId = Id;
	return true;
}

bool CSessionManager::Close(CSessionId Id, const char *pReason)
{
	CGameSession *pSession = Find(Id);
	if(!pSession)
		return false;
	pSession->Source().RequestStop(pReason);
	return true;
}

bool CSessionManager::Destroy(CSessionId Id)
{
	const auto It = std::find_if(m_vpSessions.begin(), m_vpSessions.end(), [Id](const auto &pSession) { return pSession->Id() == Id; });
	if(It == m_vpSessions.end())
		return false;
	const ESessionState State = (*It)->State();
	if(State != ESessionState::OFFLINE && State != ESessionState::ERROR)
		return false;

	m_pLastFound = nullptr;
	m_vpSessions.erase(It);
	if(m_FocusedSessionId == Id)
		m_FocusedSessionId = m_vpSessions.empty() ? CSessionId() : m_vpSessions.front()->Id();
	return true;
}

void CSessionManager::Update()
{
	for(const auto &pSession : m_vpSessions)
		pSession->Update();
}

bool CSessionManager::Update(CSessionId Id)
{
	CGameSession *pSession = Find(Id);
	if(!pSession)
		return false;
	pSession->Update();
	return true;
}

std::vector<CSessionId> CSessionManager::SessionIds() const
{
	std::vector<CSessionId> vIds;
	vIds.reserve(m_vpSessions.size());
	for(const auto &pSession : m_vpSessions)
		vIds.push_back(pSession->Id());
	return vIds;
}
