// The rules follow the zCatch of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "catches.h"

#include <algorithm>

int CCatches::LeaderId() const
{
	int LeaderId = NONE;
	int MostKills = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aStates[ClientId].m_KillsThatCount > MostKills)
		{
			LeaderId = ClientId;
			MostKills = m_aStates[ClientId].m_KillsThatCount;
		}
	}
	return LeaderId;
}

void CCatches::Catch(int VictimId, int CatcherId, bool Counts)
{
	m_aStates[VictimId].m_CatcherId = CatcherId;
	std::vector<int> &vVictimIds = m_aStates[CatcherId].m_vVictimIds;
	if(std::find(vVictimIds.begin(), vVictimIds.end(), VictimId) != vVictimIds.end())
		return;
	vVictimIds.push_back(VictimId);
	if(Counts)
		m_aStates[CatcherId].m_KillsThatCount++;
}

std::vector<int> CCatches::ReleaseLast(int CatcherId)
{
	CState &Catcher = m_aStates[CatcherId];
	if(Catcher.m_vVictimIds.empty())
		return {};
	const int VictimId = Catcher.m_vVictimIds.back();
	Catcher.m_vVictimIds.pop_back();
	m_aStates[VictimId].m_CatcherId = NONE;
	// never below 0, the ones who joined and were caught never counted (ddnet-insta issue #225)
	Catcher.m_KillsThatCount = std::max(0, Catcher.m_KillsThatCount - 1);
	std::vector<int> vReleased = {VictimId};
	if(Catcher.m_KillsThatCount == 0)
	{
		const std::vector<int> vOthers = ReleaseAll(CatcherId);
		vReleased.insert(vReleased.end(), vOthers.rbegin(), vOthers.rend());
	}
	return vReleased;
}

std::vector<int> CCatches::ReleaseAll(int CatcherId)
{
	CState &Catcher = m_aStates[CatcherId];
	std::vector<int> vReleased;
	vReleased.swap(Catcher.m_vVictimIds);
	for(const int VictimId : vReleased)
		m_aStates[VictimId].m_CatcherId = NONE;
	Catcher.m_KillsThatCount = 0;
	return vReleased;
}

std::vector<int> CCatches::Leave(int ClientId)
{
	const int CatcherId = m_aStates[ClientId].m_CatcherId;
	if(CatcherId != NONE)
	{
		std::vector<int> &vVictimIds = m_aStates[CatcherId].m_vVictimIds;
		vVictimIds.erase(std::remove(vVictimIds.begin(), vVictimIds.end(), ClientId), vVictimIds.end());
		m_aStates[ClientId].m_CatcherId = NONE;
	}
	return ReleaseAll(ClientId);
}

void CCatches::Clear()
{
	m_aStates = {};
}
