// The texts and steps follow ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "killing_spree.h"

#include <base/str.h>

#include <algorithm>

bool CKillingSpree::Add(int KillsPerStep)
{
	m_Kills++;
	return KillsPerStep > 0 && m_Kills % KillsPerStep == 0;
}

int CKillingSpree::End()
{
	const int Kills = m_Kills;
	m_Kills = 0;
	return Kills;
}

void CKillingSpree::FormatStep(char *pBuf, int BufSize, const char *pName, int Kills, int KillsPerStep)
{
	static const char *const s_apSteps[] = {"is on a killing spree", "is on a rampage", "is dominating", "is unstoppable", "is godlike"};
	const int Step = std::clamp(Kills / std::max(KillsPerStep, 1) - 1, 0, (int)std::size(s_apSteps) - 1);
	str_format(pBuf, BufSize, "'%s' %s with %d kills!", pName, s_apSteps[Step], Kills);
}

void CKillingSpree::FormatEnd(char *pBuf, int BufSize, const char *pVictim, int Kills, const char *pKiller)
{
	str_format(pBuf, BufSize, "'%s' %d-kills killing spree was ended by '%s'", pVictim, Kills, pKiller);
}

int CMultiKill::Add(int Tick, int TickSpeed)
{
	if(m_LastTick < 0 || Tick - m_LastTick > WINDOW_SECONDS * TickSpeed)
		m_Kills = 0;
	m_Kills++;
	m_LastTick = Tick;
	return m_Kills;
}

void CMultiKill::Reset()
{
	m_Kills = 0;
	m_LastTick = -1;
}

void CMultiKill::Format(char *pBuf, int BufSize, const char *pName, int Kills)
{
	str_format(pBuf, BufSize, "'%s' multi x%d!", pName, Kills);
}
