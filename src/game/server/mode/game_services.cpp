#include "game_services.h"

#include <game/server/gamecontext.h>

static_assert((int)CGameServices::FLAG_SIX == (int)CGameContext::FLAG_SIX && (int)CGameServices::FLAG_SIXUP == (int)CGameContext::FLAG_SIXUP);

void CGameVotes::Call(EType Type, int Victim, int ClientId, const char *pDesc, const char *pCmd, const char *pReason, const char *pChatmsg, const char *pSixupDesc) const
{
	m_pGameServer->m_VoteType = Type == EType::KICK ? CGameContext::VOTE_TYPE_KICK : Type == EType::SPECTATE ? CGameContext::VOTE_TYPE_SPECTATE :
														   CGameContext::VOTE_TYPE_OPTION;
	if(Type != EType::OPTION)
		m_pGameServer->m_VoteVictim = Victim;
	m_pGameServer->CallVote(ClientId, pDesc, pCmd, pReason, pChatmsg, pSixupDesc);
}

void CGameVotes::Force(bool Success) const
{
	m_pGameServer->ForceVote(Success);
}

int CGameVotes::KickOrSpecVictim() const
{
	return m_pGameServer->m_VoteCloseTime && (m_pGameServer->IsKickVote() || m_pGameServer->IsSpecVote()) ? m_pGameServer->m_VoteVictim : -1;
}

bool CGameVotes::IsRunningKickOrSpecVote(int ClientId) const
{
	return m_pGameServer->IsRunningKickOrSpecVote(ClientId);
}

int CGameVotes::Creator() const
{
	return m_pGameServer->m_VoteCreator;
}

bool CGameVotes::Moderating() const
{
	return m_pGameServer->PlayerModerating();
}

bool CGameVotes::RateLimit(int ClientId) const
{
	return m_pGameServer->RateLimitPlayerVote(ClientId);
}

bool CGameVotes::RateLimitMap(int ClientId) const
{
	return m_pGameServer->RateLimitPlayerMapVote(ClientId);
}

void CGameVotes::SetLastMapVote(int64_t Time) const
{
	m_pGameServer->m_LastMapVote = Time;
}

IServer *CGameServices::Server() const
{
	return m_pGameServer->Server();
}

IConsole *CGameServices::Console() const
{
	return m_pGameServer->Console();
}

IStorage *CGameServices::Storage() const
{
	return m_pGameServer->Storage();
}

IMap *CGameServices::Map() const
{
	return m_pGameServer->Map();
}

IAntibot *CGameServices::Antibot() const
{
	return m_pGameServer->Antibot();
}

CGameWorld &CGameServices::World() const
{
	return m_pGameServer->m_World;
}

CCollision *CGameServices::Collision() const
{
	return m_pGameServer->Collision();
}

CLayers *CGameServices::Layers() const
{
	return m_pGameServer->Layers();
}

std::vector<SSwitchers> &CGameServices::Switchers() const
{
	return m_pGameServer->Switchers();
}

CPlayerMapping &CGameServices::PlayerMapping() const
{
	return m_pGameServer->m_PlayerMapping;
}

CTeeHistorian *CGameServices::TeeHistorian() const
{
	return m_pGameServer->TeeHistorianActive() ? m_pGameServer->TeeHistorian() : nullptr;
}

CUuid CGameServices::GameUuid() const
{
	return m_pGameServer->GameUuid();
}

CPlayer *CGameServices::Player(int ClientId) const
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS ? m_pGameServer->m_apPlayers[ClientId] : nullptr;
}

CCharacter *CGameServices::Character(int ClientId) const
{
	return m_pGameServer->GetPlayerChar(ClientId);
}

CPlayer *CGameServices::FindPlayerByName(const char *pName) const
{
	return m_pGameServer->FindPlayerByName(pName);
}

std::optional<int> CGameServices::FindClientIdByName(const char *pName) const
{
	return m_pGameServer->FindClientIdByName(pName);
}

int CGameServices::ClientVersion(int ClientId) const
{
	return m_pGameServer->GetClientVersion(ClientId);
}

CClientMask CGameServices::ClientsMaskExcludeClientVersionAndHigher(int Version) const
{
	return m_pGameServer->ClientsMaskExcludeClientVersionAndHigher(Version);
}

CNetObj_PlayerInput CGameServices::LastPlayerInput(int ClientId) const
{
	return m_pGameServer->GetLastPlayerInput(ClientId);
}

CTuningParams *CGameServices::GlobalTuning() const
{
	return m_pGameServer->GlobalTuning();
}

CTuningParams *CGameServices::TuningList() const
{
	return m_pGameServer->TuningList();
}

void CGameServices::SendTuningParams(int ClientId, int Zone) const
{
	m_pGameServer->SendTuningParams(ClientId, Zone);
}

const char *CGameServices::ZoneMessage(int Zone, bool Enter) const
{
	return Enter ? m_pGameServer->m_aaZoneEnterMsg[Zone] : m_pGameServer->m_aaZoneLeaveMsg[Zone];
}

void CGameServices::SetZoneMessage(int Zone, bool Enter, const char *pText) const
{
	if(Enter)
		str_copy(m_pGameServer->m_aaZoneEnterMsg[Zone], pText);
	else
		str_copy(m_pGameServer->m_aaZoneLeaveMsg[Zone], pText);
}

void CGameServices::CreateSound(vec2 Position, int Sound, CClientMask Mask) const
{
	m_pGameServer->CreateSound(Position, Sound, Mask);
}

void CGameServices::CreateDamageInd(vec2 Position, float Angle, int Amount, CClientMask Mask) const
{
	m_pGameServer->CreateDamageInd(Position, Angle, Amount, Mask);
}

void CGameServices::CreateDeath(vec2 Position, int ClientId, CClientMask Mask) const
{
	m_pGameServer->CreateDeath(Position, ClientId, Mask);
}

void CGameServices::CreateExplosionEvent(vec2 Position, CClientMask Mask) const
{
	m_pGameServer->CreateExplosionEvent(Position, Mask);
}

void CGameServices::CreateFinishEffect(vec2 Position, CClientMask Mask) const
{
	m_pGameServer->CreateFinishEffect(Position, Mask);
}

void CGameServices::CreateBirthdayEffect(vec2 Position, CClientMask Mask) const
{
	m_pGameServer->CreateBirthdayEffect(Position, Mask);
}

void CGameServices::CreateLegacySoundGlobal(int Sound, int Target) const
{
	m_pGameServer->CreateSoundGlobal(Sound, Target, CGameContext::FLAG_SIX);
}

void CGameServices::SendLegacyChatGlobal(const char *pText) const
{
	m_pGameServer->SendChatTarget(-1, pText, CGameContext::FLAG_SIX);
}

void CGameServices::SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters, int Target) const
{
	m_pGameServer->SendGameMessage7(GameMessageId, Parameters, Target);
}

void CGameServices::SendWeaponPickup(int ClientId, int Weapon) const
{
	m_pGameServer->SendWeaponPickup(ClientId, Weapon);
}

void CGameServices::SendChatTarget(int To, const char *pText, int VersionFlags) const
{
	m_pGameServer->SendChatTarget(To, pText, VersionFlags);
}

void CGameServices::SendChatTeam(int Team, const char *pText) const
{
	m_pGameServer->SendChatTeam(Team, pText);
}

void CGameServices::SendChat(int ClientId, int Team, const char *pText, int SpamProtectionClientId, int VersionFlags) const
{
	m_pGameServer->SendChat(ClientId, Team, pText, SpamProtectionClientId, VersionFlags);
}

void CGameServices::SendBroadcast(const char *pText, int ClientId, bool IsImportant) const
{
	m_pGameServer->SendBroadcast(pText, ClientId, IsImportant);
}

bool CGameServices::ProcessSpamProtection(int ClientId, bool RespectChatInitialDelay) const
{
	return m_pGameServer->ProcessSpamProtection(ClientId, RespectChatInitialDelay);
}
