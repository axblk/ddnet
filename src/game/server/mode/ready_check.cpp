#include "ready_check.h"

#include <base/dbg.h>

void CReadyCheck::Begin(EWait Wait, int Tick)
{
	dbg_assert(Wait != EWait::NONE, "a wait needs something to wait for");
	m_Wait = Wait;
	m_WaitStartTick = Tick;
	m_Ready.reset();
}

void CReadyCheck::End()
{
	m_Wait = EWait::NONE;
	m_Ready.reset();
}

bool CReadyCheck::IsReady(int ClientId) const
{
	return !IsWaiting() || m_Ready.test(ClientId);
}

void CReadyCheck::SetReady(int ClientId, bool Ready)
{
	m_Ready.set(ClientId, Ready);
}

void CReadyCheck::SetAllReady()
{
	m_Ready.set();
}

CClientMask CReadyCheck::NotReady(const CClientMask &Participants) const
{
	return IsWaiting() ? Participants & ~m_Ready : CClientMask();
}

bool CReadyCheck::TakeChange(int ClientId, int Tick, int TickSpeed)
{
	int &LastChangeTick = m_aLastChangeTick[ClientId];
	if(LastChangeTick != 0 && Tick < LastChangeTick + CHANGE_INTERVAL_SECONDS * TickSpeed)
		return false;
	LastChangeTick = Tick;
	return true;
}

void CReadyCheck::Forget(int ClientId)
{
	m_Ready.reset(ClientId);
	m_aLastChangeTick[ClientId] = 0;
}
