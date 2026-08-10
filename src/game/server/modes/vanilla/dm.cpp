#include "dm.h"

#include <engine/shared/config.h>

#include <generated/server_data.h>

#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <cmath>
#include <limits>

CGameControllerVanillaDM::CGameControllerVanillaDM(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	IGameController(pGameServer, GameModeInfo)
{
}

CTuningParams CGameControllerVanillaDM::DefaultTuning()
{
	CTuningParams Tuning = CTuningParams::DEFAULT;
	Tuning.Set("laser_bounce_num", 1);
	return Tuning;
}

void CGameControllerVanillaDM::ApplyDamage(int Damage, bool SelfDamage, int &Health, int &Armor)
{
	if(SelfDamage)
		Damage = std::max(1, Damage / 2);

	if(Armor)
	{
		if(Damage > 1)
		{
			Health--;
			Damage--;
		}

		const int Absorbed = std::min(Damage, Armor);
		Armor -= Absorbed;
		Damage -= Absorbed;
	}

	Health -= Damage;
}

void CGameControllerVanillaDM::ApplyDeathScore(std::array<int, MAX_CLIENTS> &aScores, int VictimId, int KillerId, int Weapon)
{
	if(KillerId < 0 || Weapon == WEAPON_GAME)
		return;
	if(KillerId == VictimId)
		aScores[VictimId]--;
	else
		aScores[KillerId]++;
}

CGameControllerVanillaDM::EMatchResult CGameControllerVanillaDM::EvaluateMatch(int NumTopScores, bool LimitReached, bool SuddenDeath)
{
	if(NumTopScores == 0 || (!LimitReached && !SuddenDeath))
		return EMatchResult::RUNNING;
	return NumTopScores == 1 ? EMatchResult::END_ROUND : EMatchResult::SUDDEN_DEATH;
}

vec2 CGameControllerVanillaDM::ShotgunDirection(vec2 Direction, int Pellet, float SpeedDifference)
{
	dbg_assert(Pellet >= -2 && Pellet <= 2, "invalid shotgun pellet index");
	static constexpr float s_aSpreading[] = {-0.185f, -0.070f, 0.0f, 0.070f, 0.185f};
	const float DirectionAngle = angle(Direction) + s_aSpreading[Pellet + 2];
	const float CenterWeight = 1.0f - absolute(Pellet) / 2.0f;
	const float Speed = mix(SpeedDifference, 1.0f, CenterWeight);
	return vec2(std::cos(DirectionAngle), std::sin(DirectionAngle)) * Speed;
}

void CGameControllerVanillaDM::ResetTuning()
{
	*GameServer()->GlobalTuning() = DefaultTuning();
	GameServer()->SendTuningParams(-1);
}

void CGameControllerVanillaDM::InitGameSettings()
{
	const CTuningParams Tuning = DefaultTuning();
	for(int i = 0; i < TuneZone::NUM; i++)
	{
		GameServer()->TuningList()[i] = Tuning;
		GameServer()->m_aaZoneEnterMsg[i][0] = 0;
		GameServer()->m_aaZoneLeaveMsg[i][0] = 0;
	}
	if(g_Config.m_SvTuneReset)
		ResetTuning();

	LoadGameSettings();
}

bool CGameControllerVanillaDM::OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage)
{
	pVictim->AddVelocity(Force);
	if(!CanDamage)
		return true;

	if(Damage == 0 && Weapon >= WEAPON_HAMMER && Weapon < NUM_WEAPONS)
		Damage = g_pData->m_Weapons.m_aId[Weapon].m_Damage;
	if(Damage <= 0)
		return true;

	const int VictimId = pVictim->GetPlayer()->GetCid();
	const int OldHealth = pVictim->GetHealth();
	const int OldArmor = pVictim->GetArmor();
	int Health = OldHealth;
	int Armor = OldArmor;
	ApplyDamage(Damage, From == VictimId, Health, Armor);
	pVictim->SetHealth(Health);
	pVictim->SetArmor(Armor);

	const int DamageTaken = OldHealth - Health + OldArmor - Armor;
	if(DamageTaken > 0)
		GameServer()->CreateDamageInd(pVictim->m_Pos, 0.0f, DamageTaken, pVictim->TeamMask());

	if(From >= 0 && From != VictimId && From < MAX_CLIENTS && GameServer()->m_apPlayers[From])
		GameServer()->CreateSound(GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, CClientMask().set(From));

	if(Health <= 0)
	{
		pVictim->Die(From, Weapon);
		if(From >= 0 && From != VictimId && From < MAX_CLIENTS && GameServer()->m_apPlayers[From])
		{
			CCharacter *pAttacker = GameServer()->m_apPlayers[From]->GetCharacter();
			if(pAttacker)
				pAttacker->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
		}
		return false;
	}

	GameServer()->CreateSound(pVictim->m_Pos, Damage > 2 ? SOUND_PLAYER_PAIN_LONG : SOUND_PLAYER_PAIN_SHORT, pVictim->TeamMask());
	pVictim->SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);
	return true;
}

bool CGameControllerVanillaDM::CanCharacterHitCharacter(CCharacter *, CCharacter *pTarget) const
{
	return pTarget->IsAlive();
}

CWeaponFireResult CGameControllerVanillaDM::OnCharacterFireWeapon(const CWeaponFireContext &Context)
{
	CCharacter *pCharacter = Context.m_pCharacter;
	if(!pCharacter || Context.m_Weapon < 0 || Context.m_Weapon >= NUM_WEAPONS)
		return {};

	const int Owner = pCharacter->GetPlayer()->GetCid();
	if(pCharacter->GetWeaponAmmo(Context.m_Weapon) == 0)
	{
		if(m_aLastNoAmmoSoundTicks[Owner] + Server()->TickSpeed() <= Server()->Tick())
		{
			GameServer()->CreateSound(pCharacter->m_Pos, SOUND_WEAPON_NOAMMO, pCharacter->TeamMask());
			m_aLastNoAmmoSoundTicks[Owner] = Server()->Tick();
		}
		return {false, false, std::max(1, 125 * Server()->TickSpeed() / 1000)};
	}

	switch(Context.m_Weapon)
	{
	case WEAPON_GUN:
		new CProjectile(
			pCharacter->GameWorld(),
			WEAPON_GUN,
			Owner,
			Context.m_ProjectileStartPosition,
			Context.m_Direction,
			Server()->TickSpeed() * Context.m_pTuning->m_GunLifetime,
			false,
			false,
			-1,
			Context.m_MouseTarget);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_GUN_FIRE, pCharacter->TeamMask());
		return {true, true, 0};
	case WEAPON_SHOTGUN:
		for(int Pellet = -2; Pellet <= 2; Pellet++)
		{
			new CProjectile(
				pCharacter->GameWorld(),
				WEAPON_SHOTGUN,
				Owner,
				Context.m_ProjectileStartPosition,
				ShotgunDirection(Context.m_Direction, Pellet, Context.m_pTuning->m_ShotgunSpeeddiff),
				Server()->TickSpeed() * Context.m_pTuning->m_ShotgunLifetime,
				false,
				false,
				-1,
				Context.m_MouseTarget);
		}
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_SHOTGUN_FIRE, pCharacter->TeamMask());
		return {true, true, 0};
	default:
	{
		CWeaponFireResult Result = IGameController::OnCharacterFireWeapon(Context);
		Result.m_ConsumeAmmo = Result.m_Fired && pCharacter->GetWeaponAmmo(Context.m_Weapon) > 0;
		return Result;
	}
	}
}

CGamePickupResult CGameControllerVanillaDM::OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position)
{
	switch(Type)
	{
	case POWERUP_HEALTH:
		if(pCharacter->IncreaseHealth(1))
		{
			GameServer()->CreateSound(Position, SOUND_PICKUP_HEALTH, pCharacter->TeamMask());
			return {true, 15, -1};
		}
		break;
	case POWERUP_ARMOR:
		if(pCharacter->IncreaseArmor(1))
		{
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
			return {true, 15, -1};
		}
		break;
	case POWERUP_WEAPON:
		if(Subtype >= WEAPON_SHOTGUN && Subtype <= WEAPON_LASER)
		{
			const int MaxAmmo = g_pData->m_Weapons.m_aId[Subtype].m_Maxammo;
			if(!pCharacter->GetWeaponGot(Subtype) || pCharacter->GetWeaponAmmo(Subtype) < MaxAmmo)
			{
				pCharacter->SetWeaponGot(Subtype, true);
				pCharacter->SetWeaponAmmo(Subtype, MaxAmmo);
				const int PickupSound = Subtype == WEAPON_GRENADE ? SOUND_PICKUP_GRENADE : SOUND_PICKUP_SHOTGUN;
				GameServer()->CreateSound(Position, PickupSound, pCharacter->TeamMask());
				GameServer()->SendWeaponPickup(pCharacter->GetPlayer()->GetCid(), Subtype);
				return {true, 15, SOUND_WEAPON_SPAWN};
			}
		}
		break;
	case POWERUP_NINJA:
		pCharacter->GiveNinja();
		GameServer()->CreateSound(Position, SOUND_PICKUP_NINJA, pCharacter->TeamMask());
		for(CCharacter *pOther = static_cast<CCharacter *>(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_CHARACTER)); pOther; pOther = static_cast<CCharacter *>(pOther->TypeNext()))
		{
			if(pOther != pCharacter)
				pOther->SetEmote(EMOTE_SURPRISE, Server()->Tick() + Server()->TickSpeed());
		}
		pCharacter->SetEmote(EMOTE_ANGRY, Server()->Tick() + 1200 * Server()->TickSpeed() / 1000);
		return {true, 90, -1};
	default:
		break;
	}
	return {};
}

int CGameControllerVanillaDM::PickupInitialSpawnDelaySeconds(int Type, int Subtype) const
{
	return Type == POWERUP_NINJA ? 90 : 0;
}

CGameProjectileRules CGameControllerVanillaDM::ProjectileRules(const CGameProjectileContext &Context) const
{
	return {true, false, 0.001f, Context.m_OwnerConnected ? EProjectileOwnerLossAction::KEEP : EProjectileOwnerLossAction::DETACH};
}

int CGameControllerVanillaDM::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	const int VictimId = pVictim->GetPlayer()->GetCid();
	m_aEarliestRespawnTicks[VictimId] = Server()->Tick() + Server()->TickSpeed() / 2;

	ApplyDeathScore(m_aScores, VictimId, pKiller ? pKiller->GetCid() : -1, Weapon);
	return 0;
}

void CGameControllerVanillaDM::OnCharacterSpawn(CCharacter *pChr)
{
	IGameController::OnCharacterSpawn(pChr);
	pChr->SetArmor(0);
	pChr->SetWeaponAmmo(WEAPON_GUN, 10);
}

void CGameControllerVanillaDM::OnPlayerConnect(CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	m_aScores[ClientId] = 0;
	m_aEarliestRespawnTicks[ClientId] = 0;
	m_aLastNoAmmoSoundTicks[ClientId] = Server()->Tick() - Server()->TickSpeed();
	IGameController::OnPlayerConnect(pPlayer);
}

void CGameControllerVanillaDM::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	const int ClientId = pPlayer->GetCid();
	IGameController::OnPlayerDisconnect(pPlayer, pReason);
	m_aScores[ClientId] = 0;
	m_aEarliestRespawnTicks[ClientId] = 0;
	m_aLastNoAmmoSoundTicks[ClientId] = 0;
}

void CGameControllerVanillaDM::StartRound()
{
	m_aScores.fill(0);
	m_aEarliestRespawnTicks.fill(0);
	m_aLastNoAmmoSoundTicks.fill(Server()->Tick() - Server()->TickSpeed());
	IGameController::StartRound();
}

void CGameControllerVanillaDM::Tick()
{
	IGameController::Tick();
	if(m_GameOverTick != -1 || m_Warmup)
		return;

	int TopScore = std::numeric_limits<int>::min();
	int NumTopScores = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;

		if(m_aScores[ClientId] > TopScore)
		{
			TopScore = m_aScores[ClientId];
			NumTopScores = 1;
		}
		else if(m_aScores[ClientId] == TopScore)
		{
			NumTopScores++;
		}
	}

	const bool ScoreLimitReached = ScoreLimit() > 0 && TopScore >= ScoreLimit();
	const bool TimeLimitReached = TimeLimit() > 0 && Server()->Tick() - m_RoundStartTick >= TimeLimit() * Server()->TickSpeed() * 60;
	const EMatchResult MatchResult = EvaluateMatch(NumTopScores, ScoreLimitReached || TimeLimitReached, m_SuddenDeath != 0);
	if(MatchResult == EMatchResult::END_ROUND)
		EndRound();
	else if(MatchResult == EMatchResult::SUDDEN_DEATH)
		m_SuddenDeath = 1;
}

int CGameControllerVanillaDM::SnapPlayerScore(int SnappingClient, CPlayer *pPlayer)
{
	return m_aScores[pPlayer->GetCid()];
}

bool CGameControllerVanillaDM::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || Server()->Tick() < m_aEarliestRespawnTicks[ClientId])
		return false;
	return IGameController::CanSpawn(Team, pOutPos, ClientId);
}

int CGameControllerVanillaDM::GameInfoFlags(int SnappingClient) const
{
	return GAMEINFOFLAG_GAMETYPE_VANILLA |
	       GAMEINFOFLAG_ALLOW_EYE_WHEEL |
	       GAMEINFOFLAG_ALLOW_ZOOM;
}

int CGameControllerVanillaDM::GameInfoFlags2(int SnappingClient) const
{
	return GAMEINFOFLAG2_HUD_AMMO | GAMEINFOFLAG2_HUD_HEALTH_ARMOR;
}

int CGameControllerVanillaDM::ScoreLimit() const
{
	return g_Config.m_SvScorelimit;
}

int CGameControllerVanillaDM::TimeLimit() const
{
	return g_Config.m_SvTimelimit;
}
