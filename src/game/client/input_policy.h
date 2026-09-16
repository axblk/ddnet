#ifndef GAME_CLIENT_INPUT_POLICY_H
#define GAME_CLIENT_INPUT_POLICY_H

#include <generated/protocol.h>

enum class EStreamInputPolicy
{
	DIRECT,
	COPY_MOVES,
	HAMMER,
};

// How a local player gets its input: from the controls, or from the one that
// has them.
class CStreamInputRoute
{
public:
	EStreamInputPolicy m_Policy = EStreamInputPolicy::DIRECT;
	CNetObj_PlayerInput m_HammerInput = {};
	unsigned int m_HammerCounter = 0;

	bool AdvanceHammer();
	void FinishHammering(CNetObj_PlayerInput &TargetInput);
	void Reset() { *this = CStreamInputRoute(); }
};

#endif // GAME_CLIENT_INPUT_POLICY_H
