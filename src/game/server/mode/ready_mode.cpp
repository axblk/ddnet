#include "ready_mode.h"

#include <base/log.h>
#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/server/mode/game_services.h>
#include <game/server/player.h>

// how often 0.6 clients are told whom the game waits for
static constexpr int READY_BROADCAST_SECONDS = 8;

CReadyMode::CReadyMode(CGameServices &Services, IGame &Game) :
	m_Services(Services),
	m_Game(Game)
{
}

CReadyMode::~CReadyMode()
{
	if(m_Available)
		m_Services.Console()->DeregisterOwner(this);
}

void CReadyMode::Init()
{
	m_Available = true;
	// registered for good, so that sv_player_ready_mode can be switched by a vote;
	// "pause" as in ddnet-insta, for the pause key of DDNet clients
	for(const char *pName : {"ready", "pause"})
		dbg_assert(m_Services.Console()->RegisterOwned(pName, "", CFGFLAG_CHAT, ConReady, this, "Say that you are ready, or pause the game until everybody is (with sv_player_ready_mode)", this), "duplicate mode command '%s'", pName);
}

bool CReadyMode::IsOn() const
{
	return m_Available && g_Config.m_SvPlayerReadyMode;
}

CClientMask CReadyMode::Participants() const
{
	CClientMask Participants;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		Participants.set(ClientId, m_Game.IsReadyParticipant(ClientId));
	return Participants;
}

const char *CReadyMode::ChangeRefusal(int ClientId) const
{
	if(!IsOn())
		return "There is no ready mode on this server";
	if(!m_Game.IsReadyParticipant(ClientId))
		return "Only players in the game can be ready";
	// a running game can be paused, anything else goes on by itself
	if(!m_ReadyCheck.IsWaiting() && !m_Game.IsRunningUnpaused())
		return "Nothing is waiting for you to be ready";
	return nullptr;
}

const char *CReadyMode::OnPlayerReadyChange(int ClientId)
{
	if(const char *pRefusal = ChangeRefusal(ClientId))
		return pRefusal;
	IServer *pServer = m_Services.Server();
	if(!m_ReadyCheck.TakeChange(ClientId, pServer->Tick(), pServer->TickSpeed()))
		return nullptr;

	if(m_ReadyCheck.IsWaiting())
	{
		const bool Ready = !m_ReadyCheck.IsReady(ClientId);
		m_ReadyCheck.SetReady(ClientId, Ready);
		log_info("game", "ready player='%d:%s' ready=%d not_ready=%d", ClientId, pServer->ClientName(ClientId), Ready, (int)m_ReadyCheck.NotReady(Participants()).count());
		return nullptr;
	}

	// not ready any more: the game stands still until everybody is ready again
	m_Game.PauseUntilReady(true);
	UpdateWait();
	if(m_ReadyCheck.Wait() != CReadyCheck::EWait::RESUME)
		return nullptr;
	log_info("game", "ready player='%d:%s' paused the game", ClientId, pServer->ClientName(ClientId));
	for(int Receiver = 0; Receiver < MAX_CLIENTS; Receiver++)
	{
		int TranslatedId = ClientId;
		if(m_Services.Player(Receiver) && pServer->IsSixup(Receiver) && pServer->Translate(TranslatedId, Receiver))
			m_Services.SendGameMessage7(protocol7::GAMEMSG_GAME_PAUSED, {TranslatedId}, Receiver);
	}
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "'%s' paused the game until everybody is ready", pServer->ClientName(ClientId));
	// 0.7 clients say so themselves from the game message
	m_Services.SendChat(-1, TEAM_ALL, aBuf, -1, CGameServices::FLAG_SIX);
	return nullptr;
}

void CReadyMode::ForceReady(int ClientId)
{
	if(!m_ReadyCheck.IsWaiting())
		return;
	if(ClientId < 0)
	{
		m_ReadyCheck.SetAllReady();
		m_Services.SendChat(-1, TEAM_ALL, "Everybody was set ready by an admin");
		return;
	}
	if(ClientId >= MAX_CLIENTS || !m_Game.IsReadyParticipant(ClientId) || m_ReadyCheck.IsReady(ClientId))
		return;
	m_ReadyCheck.SetReady(ClientId, true);
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "'%s' was set ready by an admin", m_Services.Server()->ClientName(ClientId));
	m_Services.SendChat(-1, TEAM_ALL, aBuf);
}

void CReadyMode::OnPlayerLeave(int ClientId)
{
	m_ReadyCheck.Forget(ClientId);
}

void CReadyMode::UpdateWait()
{
	CReadyCheck::EWait Wait = CReadyCheck::EWait::NONE;
	if(IsOn())
	{
		if(m_Game.IsMatchWaitingForReady())
			Wait = CReadyCheck::EWait::START;
		else if(m_Game.IsPausedUntilReady())
			Wait = CReadyCheck::EWait::RESUME;
	}
	if(Wait == m_ReadyCheck.Wait())
		return;
	if(m_ReadyCheck.IsWaiting())
		SendBroadcasts(true);
	if(Wait == CReadyCheck::EWait::NONE)
	{
		m_ReadyCheck.End();
		return;
	}
	m_ReadyCheck.Begin(Wait, m_Services.Server()->Tick());
	m_NextBroadcastTick = m_Services.Server()->Tick();
	log_info("game", "ready wait=%s", Wait == CReadyCheck::EWait::START ? "start" : "resume");
}

void CReadyMode::Tick()
{
	// without ready mode there is nobody to wait for, as in 0.7
	if(m_Game.IsMatchWaitingForReady() && !IsOn())
		m_Game.StartMatch();
	UpdateWait();
	if(!m_ReadyCheck.IsWaiting())
		return;

	// a player who leaves the game is not ready when coming back
	const CClientMask Participants = this->Participants();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		if(!Participants.test(ClientId))
			m_ReadyCheck.SetReady(ClientId, false);

	// a match only starts with somebody to play it, a pause ends when nobody is left to wait for
	if(m_ReadyCheck.NotReady(Participants).none() && (Participants.any() || m_ReadyCheck.Wait() == CReadyCheck::EWait::RESUME))
	{
		FinishWait();
		return;
	}

	const int Tick = m_Services.Server()->Tick();
	const int TickSpeed = m_Services.Server()->TickSpeed();
	if(const int ForceTick = ForceReadyTick())
	{
		const int SecondsLeft = (ForceTick - Tick) / TickSpeed;
		if(Tick >= ForceTick)
		{
			m_Services.SendChat(-1, TEAM_ALL, "Not everybody was ready in time, the game goes on");
			FinishWait();
			return;
		}
		if((ForceTick - Tick) % TickSpeed == 0 && (SecondsLeft == 60 || SecondsLeft == 10))
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), SecondsLeft == 60 ? "The game goes on in 1 minute, ready or not" : "The game goes on in %d seconds, ready or not", SecondsLeft);
			m_Services.SendChat(-1, TEAM_ALL, aBuf);
		}
	}

	// the players see whom they wait for as soon as that changes
	if(Tick >= m_NextBroadcastTick || m_ReadyCheck.NotReady(Participants) != m_BroadcastNotReady)
	{
		SendBroadcasts(false);
		// a broadcast fades after a while, a change of mind is shown right away
		m_NextBroadcastTick = Tick + READY_BROADCAST_SECONDS * TickSpeed;
	}
}

void CReadyMode::FinishWait()
{
	const CReadyCheck::EWait Wait = m_ReadyCheck.Wait();
	SendBroadcasts(true);
	m_ReadyCheck.End();
	log_info("game", "ready everybody");
	if(Wait == CReadyCheck::EWait::START)
		m_Game.StartMatch();
	else
		m_Game.PauseUntilReady(false);
}

int CReadyMode::ForceReadyTick() const
{
	if(!m_ReadyCheck.IsWaiting() || !g_Config.m_SvForceReadyAll)
		return 0;
	return m_ReadyCheck.WaitStartTick() + g_Config.m_SvForceReadyAll * 60 * m_Services.Server()->TickSpeed();
}

void CReadyMode::SendBroadcasts(bool Clear)
{
	if(Clear && !m_BroadcastSent)
		return;
	m_BroadcastSent = !Clear;
	IServer *pServer = m_Services.Server();

	char aWaiting[256] = "";
	if(!Clear)
	{
		static constexpr int MAX_NAMES = 6;
		const CClientMask NotReady = m_ReadyCheck.NotReady(Participants());
		m_BroadcastNotReady = NotReady;
		int NumNames = 0;
		str_copy(aWaiting, "Waiting for: ");
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(!NotReady.test(ClientId) || NumNames++ >= MAX_NAMES)
				continue;
			if(NumNames > 1)
				str_append(aWaiting, ", ");
			str_append(aWaiting, pServer->ClientName(ClientId));
		}
		if(NumNames > MAX_NAMES)
		{
			char aMore[32];
			str_format(aMore, sizeof(aMore), " and %d more", NumNames - MAX_NAMES);
			str_append(aWaiting, aMore);
		}
		else if(NumNames == 0)
		{
			str_copy(aWaiting, "Waiting for players");
		}
	}

	// 0.7 clients and those that got the ready state show who is ready themselves
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!m_Services.Player(ClientId) || !pServer->ClientIngame(ClientId) || pServer->IsSixup(ClientId) || ShowsReadyState(ClientId))
			continue;
		if(Clear)
		{
			m_Services.SendBroadcast("", ClientId);
			continue;
		}
		char aBuf[512];
		if(m_Game.IsReadyParticipant(ClientId) && !m_ReadyCheck.IsReady(ClientId))
			str_format(aBuf, sizeof(aBuf), "%s\nSay /ready when you are ready", aWaiting);
		else
			str_copy(aBuf, aWaiting);
		m_Services.SendBroadcast(aBuf, ClientId);
	}
}

void CReadyMode::OnPlayerShowsReadyStateChanged(int ClientId)
{
	// a client can only say so once it is in the game, after its first broadcast
	if(!m_BroadcastSent || m_Services.Server()->IsSixup(ClientId))
		return;
	if(ShowsReadyState(ClientId))
		m_Services.SendBroadcast("", ClientId);
	else
		m_NextBroadcastTick = m_Services.Server()->Tick();
}

bool CReadyMode::ShowsReadyState(int SnappingClient) const
{
	if(SnappingClient == SERVER_DEMO_CLIENT)
		return true;
	const CPlayer *pPlayer = m_Services.Player(SnappingClient);
	return pPlayer && pPlayer->m_EnableReadyState && !m_Services.Server()->IsSixup(SnappingClient);
}

void CReadyMode::Snap(int SnappingClient)
{
	// the object says the server has ready mode, so it is there even while nothing waits
	if(!IsOn() || !ShowsReadyState(SnappingClient))
		return;

	CNetObj_ReadyState ReadyState = {};
	switch(m_ReadyCheck.Wait())
	{
	case CReadyCheck::EWait::NONE: ReadyState.m_Wait = READYWAIT_NONE; break;
	case CReadyCheck::EWait::START: ReadyState.m_Wait = READYWAIT_START; break;
	case CReadyCheck::EWait::RESUME: ReadyState.m_Wait = READYWAIT_RESUME; break;
	}
	ReadyState.m_ForceReadyTick = ForceReadyTick();
	m_Services.Server()->SnapNewItem(0, ReadyState);
	if(!m_ReadyCheck.IsWaiting())
		return;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		int TranslatedId = ClientId;
		if(!m_Game.IsReadyParticipant(ClientId) || !m_Services.Server()->Translate(TranslatedId, SnappingClient))
			continue;
		CNetObj_PlayerReady PlayerReady = {};
		PlayerReady.m_Ready = m_ReadyCheck.IsReady(ClientId);
		m_Services.Server()->SnapNewItem(TranslatedId, PlayerReady);
	}
}

void CReadyMode::ConReady(IConsole::IResult *pResult, void *pUserData)
{
	CReadyMode *pSelf = static_cast<CReadyMode *>(pUserData);
	if(pResult->m_ClientId < 0 || pResult->m_ClientId >= MAX_CLIENTS || !pSelf->m_Services.Player(pResult->m_ClientId))
		return;
	if(const char *pRefusal = pSelf->OnPlayerReadyChange(pResult->m_ClientId))
		pSelf->m_Services.SendChatTarget(pResult->m_ClientId, pRefusal);
}
