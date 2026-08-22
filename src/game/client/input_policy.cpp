#include "input_policy.h"

#include <algorithm>

bool CStreamInputRoute::AdvanceHammer()
{
	const bool Send = m_HammerCounter % 25 == 0;
	m_HammerCounter++;
	return Send;
}

void CStreamInputRoute::FinishHammering(CNetObj_PlayerInput &TargetInput)
{
	if(m_HammerCounter == 0)
		return;
	TargetInput.m_Fire = (m_HammerInput.m_Fire + 1) & ~1;
	m_HammerCounter = 0;
}

bool CStreamInputRouter::Set(CStreamId Target, CStreamId Source, EStreamInputPolicy Policy)
{
	if(!Target.IsValid() || !Source.IsValid() || (Policy == EStreamInputPolicy::DIRECT && Target != Source))
		return false;
	const auto Found = std::find_if(m_vRoutes.begin(), m_vRoutes.end(), [Target](const CStreamInputRoute &Route) { return Route.m_Target == Target; });
	if(Found != m_vRoutes.end())
	{
		Found->m_Source = Source;
		Found->m_Policy = Policy;
		return true;
	}
	m_vRoutes.emplace_back();
	m_vRoutes.back().m_Target = Target;
	m_vRoutes.back().m_Source = Source;
	m_vRoutes.back().m_Policy = Policy;
	return true;
}

CStreamInputRoute *CStreamInputRouter::Find(CStreamId Target)
{
	const auto Found = std::find_if(m_vRoutes.begin(), m_vRoutes.end(), [Target](const CStreamInputRoute &Route) { return Route.m_Target == Target; });
	return Found == m_vRoutes.end() ? nullptr : &*Found;
}

const CStreamInputRoute *CStreamInputRouter::Find(CStreamId Target) const
{
	const auto Found = std::find_if(m_vRoutes.begin(), m_vRoutes.end(), [Target](const CStreamInputRoute &Route) { return Route.m_Target == Target; });
	return Found == m_vRoutes.end() ? nullptr : &*Found;
}

bool CStreamInputRouter::Remove(CStreamId Stream)
{
	const auto NewEnd = std::remove_if(m_vRoutes.begin(), m_vRoutes.end(), [Stream](const CStreamInputRoute &Route) { return Route.m_Target == Stream || Route.m_Source == Stream; });
	if(NewEnd == m_vRoutes.end())
		return false;
	m_vRoutes.erase(NewEnd, m_vRoutes.end());
	return true;
}
