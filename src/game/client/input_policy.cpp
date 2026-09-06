#include "input_policy.h"

bool CInputRoute::AdvanceHammer()
{
	const bool Send = m_HammerCounter % 25 == 0;
	m_HammerCounter++;
	return Send;
}

void CInputRoute::FinishHammering(CNetObj_PlayerInput &TargetInput)
{
	if(m_HammerCounter == 0)
		return;
	TargetInput.m_Fire = (m_HammerInput.m_Fire + 1) & ~1;
	m_HammerCounter = 0;
}
