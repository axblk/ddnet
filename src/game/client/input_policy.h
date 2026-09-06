#ifndef GAME_CLIENT_INPUT_POLICY_H
#define GAME_CLIENT_INPUT_POLICY_H

#include <engine/client/stream.h>

#include <generated/protocol.h>

#include <vector>

enum class EStreamInputPolicy
{
	DIRECT,
	COPY_MOVES,
	HAMMER,
};

class CStreamInputRoute
{
public:
	CStreamId m_Target;
	CStreamId m_Source;
	EStreamInputPolicy m_Policy = EStreamInputPolicy::DIRECT;
	CNetObj_PlayerInput m_HammerInput = {};
	unsigned int m_HammerCounter = 0;

	bool AdvanceHammer();
	void FinishHammering(CNetObj_PlayerInput &TargetInput);
};

class CStreamInputRouter
{
	std::vector<CStreamInputRoute> m_vRoutes;

public:
	bool Set(CStreamId Target, CStreamId Source, EStreamInputPolicy Policy);
	CStreamInputRoute *Find(CStreamId Target);
	const CStreamInputRoute *Find(CStreamId Target) const;
	bool Remove(CStreamId Stream);
	void Reset() { m_vRoutes.clear(); }
	const std::vector<CStreamInputRoute> &Routes() const { return m_vRoutes; }
	size_t NumRoutes() const { return m_vRoutes.size(); }
};

#endif // GAME_CLIENT_INPUT_POLICY_H
