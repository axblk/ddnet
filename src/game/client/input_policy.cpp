#include "input_policy.h"

#include <algorithm>

bool CStreamInputRouter::Set(CStreamId Target, CStreamId Source, EStreamInputPolicy Policy)
{
	if(!Target.IsValid() || !Source.IsValid())
		return false;
	const auto Found = std::find_if(m_vRoutes.begin(), m_vRoutes.end(), [Target](const CStreamInputRoute &Route) { return Route.m_Target == Target; });
	if(Found != m_vRoutes.end())
	{
		Found->m_Source = Source;
		Found->m_Policy = Policy;
		return true;
	}
	m_vRoutes.push_back({Target, Source, Policy});
	return true;
}

const CStreamInputRoute *CStreamInputRouter::Find(CStreamId Target) const
{
	const auto Found = std::find_if(m_vRoutes.begin(), m_vRoutes.end(), [Target](const CStreamInputRoute &Route) { return Route.m_Target == Target; });
	return Found == m_vRoutes.end() ? nullptr : &*Found;
}

bool CStreamInputRouter::Remove(CStreamId Target)
{
	const auto Found = std::find_if(m_vRoutes.begin(), m_vRoutes.end(), [Target](const CStreamInputRoute &Route) { return Route.m_Target == Target; });
	if(Found == m_vRoutes.end())
		return false;
	m_vRoutes.erase(Found);
	return true;
}
