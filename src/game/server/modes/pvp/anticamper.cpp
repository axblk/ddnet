// The rule follows the anticamper of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "anticamper.h"

#include <base/math.h>

void CAnticamper::Reset()
{
	m_PunishTick = -1;
	m_Warned = false;
}

CAnticamper::EAction CAnticamper::Tick(vec2 Pos, int Tick, int TickSpeed, int Seconds, int Range)
{
	if(m_PunishTick < 0)
	{
		m_Spot = Pos;
		m_PunishTick = Tick + Seconds * TickSpeed;
	}
	if(absolute(m_Spot.x - Pos.x) >= Range || absolute(m_Spot.y - Pos.y) >= Range)
	{
		Reset();
		return EAction::NONE;
	}
	if(m_PunishTick <= Tick)
	{
		Reset();
		return EAction::PUNISH;
	}
	if(!m_Warned && m_PunishTick <= Tick + WARN_SECONDS * TickSpeed)
	{
		m_Warned = true;
		return EAction::WARN;
	}
	return EAction::NONE;
}
