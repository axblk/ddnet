#ifndef GAME_CLIENT_INPUT_POLICY_H
#define GAME_CLIENT_INPUT_POLICY_H

#include <engine/client/stream.h>

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
};

class CStreamInputRouter
{
	std::vector<CStreamInputRoute> m_vRoutes;

public:
	bool Set(CStreamId Target, CStreamId Source, EStreamInputPolicy Policy);
	const CStreamInputRoute *Find(CStreamId Target) const;
	bool Remove(CStreamId Target);
	size_t NumRoutes() const { return m_vRoutes.size(); }
};

#endif // GAME_CLIENT_INPUT_POLICY_H
