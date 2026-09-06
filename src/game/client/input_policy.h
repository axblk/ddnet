#ifndef GAME_CLIENT_INPUT_POLICY_H
#define GAME_CLIENT_INPUT_POLICY_H

#include <generated/protocol.h>

enum class EInputPolicy
{
	DIRECT,
	COPY_MOVES,
	HAMMER,
};

class CInputRoute
{
public:
	int m_Source = 0;
	EInputPolicy m_Policy = EInputPolicy::DIRECT;
	CNetObj_PlayerInput m_HammerInput = {};
	unsigned int m_HammerCounter = 0;

	bool AdvanceHammer();
	void FinishHammering(CNetObj_PlayerInput &TargetInput);
	void Reset() { *this = {}; }
};

#endif // GAME_CLIENT_INPUT_POLICY_H
