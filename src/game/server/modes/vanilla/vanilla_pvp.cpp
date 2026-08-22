#include "vanilla_pvp.h"

#include <engine/server.h>
#include <engine/shared/config.h>

#include <generated/server_data.h>

#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>
#include <game/server/mode/game_services.h>
#include <game/server/player.h>

#include <algorithm>
#include <cmath>

CGameControllerVanillaPvP::CGameControllerVanillaPvP(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	IGameController(Services, GameModeInfo)
{
	// A mode that produces no report at all has no namespace to hang metric ids
	// off, so there is nothing to declare either.
	if(!Info().m_Report.m_ModeId.empty())
		m_vPickupReportMetrics = VanillaPickupGameModeMetrics(Info().m_Report.m_ModeId, static_cast<int>(Info().m_Report.m_vMetrics.size()));
}

CPlayer *CGameControllerVanillaPvP::CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team)
{
	return new CPlayerVanilla(Services(), UniqueClientId, ClientId, Team);
}

void CGameControllerVanillaPvP::SetRespawnDelay(int VictimId, int Weapon)
{
	// Stock 0.7 waits half a second before respawning, and three seconds when the
	// tee killed itself.
	const int Delay = Weapon == WEAPON_SELF ? Server()->TickSpeed() * 3 : Server()->TickSpeed() / 2;
	VanillaPlayer(VictimId)->m_EarliestRespawnTick = Server()->Tick() + Delay;
}

CPlayerVanilla *CGameControllerVanillaPvP::VanillaPlayer(int ClientId) const
{
	return static_cast<CPlayerVanilla *>(Services().Player(ClientId));
}

CTuningParams CGameControllerVanillaPvP::DefaultTuning()
{
	CTuningParams Tuning = CTuningParams::DEFAULT;
	Tuning.Set("laser_bounce_num", 1);
	return Tuning;
}

void CGameControllerVanillaPvP::ApplyDamage(int Damage, bool SelfDamage, int &Health, int &Armor)
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

int CGameControllerVanillaPvP::DeathScoreDelta(int VictimId, int KillerId, int Weapon, bool TeamKill)
{
	if(KillerId < 0 || Weapon == WEAPON_GAME)
		return 0;
	return KillerId == VictimId || TeamKill ? -1 : 1;
}

CGameControllerVanillaPvP::EMatchResult CGameControllerVanillaPvP::EvaluateMatch(int NumTopScores, bool LimitReached, bool SuddenDeath)
{
	if(NumTopScores == 0 || (!LimitReached && !SuddenDeath))
		return EMatchResult::RUNNING;
	return NumTopScores == 1 ? EMatchResult::END_ROUND : EMatchResult::SUDDEN_DEATH;
}

vec2 CGameControllerVanillaPvP::ShotgunDirection(vec2 Direction, int Pellet, float SpeedDifference)
{
	dbg_assert(Pellet >= -2 && Pellet <= 2, "invalid shotgun pellet index");
	static constexpr float s_aSpreading[] = {-0.185f, -0.070f, 0.0f, 0.070f, 0.185f};
	const float DirectionAngle = angle(Direction) + s_aSpreading[Pellet + 2];
	const float CenterWeight = 1.0f - absolute(Pellet) / 2.0f;
	const float Speed = mix(SpeedDifference, 1.0f, CenterWeight);
	return vec2(std::cos(DirectionAngle), std::sin(DirectionAngle)) * Speed;
}

void CGameControllerVanillaPvP::ResetTuning()
{
	Services().SetGlobalTuning(DefaultTuning());
}

void CGameControllerVanillaPvP::InitGameSettings()
{
	const CTuningParams Tuning = DefaultTuning();
	Services().ResetTuningZones(Tuning);
	if(g_Config.m_SvTuneReset)
		ResetTuning();

	LoadGameSettings();
}

bool CGameControllerVanillaPvP::OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam)
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
	{
		// Hits that land in quick succession are spread around the tee instead of
		// printing every number on top of the last one.
		CPlayerVanilla *pVictimPlayer = VanillaPlayer(VictimId);
		if(Server()->Tick() >= pVictimPlayer->m_DamageTakenTick + Server()->TickSpeed() / 2)
			pVictimPlayer->m_DamageTaken = 0;
		Services().CreateDamageInd(pVictim->m_Pos, pVictimPlayer->m_DamageTaken * 0.25f, DamageTaken, pVictim->TeamMask());
		pVictimPlayer->m_DamageTaken++;
		pVictimPlayer->m_DamageTakenTick = Server()->Tick();
		AddCharacterDamageMatchMetrics(From != VictimId ? Services().Player(From) : nullptr, pVictim->GetPlayer(), Weapon, DamageTaken);
	}

	if(CPlayer *pAttackerPlayer = From != VictimId ? Services().Player(From) : nullptr)
		Services().CreateSound(pAttackerPlayer->m_ViewPos, SOUND_HIT, CClientMask().set(From));

	if(Health <= 0)
	{
		pVictim->Die(From, Weapon);
		if(CPlayer *pAttackerPlayer = From != VictimId ? Services().Player(From) : nullptr)
		{
			CCharacter *pAttacker = pAttackerPlayer->GetCharacter();
			if(pAttacker)
				pAttacker->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
		}
		return false;
	}

	Services().CreateSound(pVictim->m_Pos, Damage > 2 ? SOUND_PLAYER_PAIN_LONG : SOUND_PLAYER_PAIN_SHORT, pVictim->TeamMask());
	pVictim->SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);
	return true;
}

CWeaponFireResult CGameControllerVanillaPvP::OnCharacterFireWeapon(const CWeaponFireContext &Context)
{
	CCharacter *pCharacter = Context.m_pCharacter;
	if(!pCharacter || Context.m_Weapon < 0 || Context.m_Weapon >= NUM_WEAPONS)
		return {};

	const int Owner = pCharacter->GetPlayer()->GetCid();
	if(pCharacter->GetWeaponAmmo(Context.m_Weapon) == 0)
	{
		CPlayerVanilla *pPlayer = VanillaPlayer(Owner);
		if(pPlayer->m_LastNoAmmoSoundTick + Server()->TickSpeed() <= Server()->Tick())
		{
			Services().CreateSound(pCharacter->m_Pos, SOUND_WEAPON_NOAMMO, pCharacter->TeamMask());
			pPlayer->m_LastNoAmmoSoundTick = Server()->Tick();
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
		Services().CreateSound(pCharacter->m_Pos, SOUND_GUN_FIRE, pCharacter->TeamMask()); // NOLINT(clang-analyzer-unix.Malloc)
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
		Services().CreateSound(pCharacter->m_Pos, SOUND_SHOTGUN_FIRE, pCharacter->TeamMask());
		return {true, true, 0};
	default:
	{
		CWeaponFireResult Result = IGameController::OnCharacterFireWeapon(Context);
		Result.m_ConsumeAmmo = Result.m_Fired && pCharacter->GetWeaponAmmo(Context.m_Weapon) > 0;
		return Result;
	}
	}
}

CGamePickupResult CGameControllerVanillaPvP::OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position)
{
	switch(Type)
	{
	case POWERUP_HEALTH:
		// A full tee refuses the pickup, so reaching here means the point was
		// really gained and is worth counting.
		if(pCharacter->IncreaseHealth(1))
		{
			AddParticipantMatchMetric(pCharacter->GetPlayer(), "health_picked_up", 1);
			Services().CreateSound(Position, SOUND_PICKUP_HEALTH, pCharacter->TeamMask());
			return {true, 15, -1};
		}
		break;
	case POWERUP_ARMOR:
		if(pCharacter->IncreaseArmor(1))
		{
			AddParticipantMatchMetric(pCharacter->GetPlayer(), "armor_picked_up", 1);
			Services().CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
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
				Services().CreateSound(Position, PickupSound, pCharacter->TeamMask());
				Services().SendWeaponPickup(pCharacter->GetPlayer()->GetCid(), Subtype);
				return {true, 15, SOUND_WEAPON_SPAWN};
			}
		}
		break;
	case POWERUP_NINJA:
		pCharacter->GiveNinja();
		Services().CreateSound(Position, SOUND_PICKUP_NINJA, pCharacter->TeamMask());
		for(CCharacter *pOther = static_cast<CCharacter *>(Services().World().FindFirst(CGameWorld::ENTTYPE_CHARACTER)); pOther; pOther = static_cast<CCharacter *>(pOther->TypeNext()))
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

int CGameControllerVanillaPvP::PickupInitialSpawnDelaySeconds(int Type, int Subtype) const
{
	return Type == POWERUP_NINJA ? 90 : 0;
}

void CGameControllerVanillaPvP::OnCharacterSpawn(CCharacter *pChr)
{
	IGameController::OnCharacterSpawn(pChr);
	pChr->SetArmor(0);
	pChr->SetWeaponAmmo(WEAPON_GUN, 10);
}

void CGameControllerVanillaPvP::OnPlayerConnect(CPlayer *pPlayer)
{
	static_cast<CPlayerVanilla *>(pPlayer)->ResetRoundState(Server()->Tick() - Server()->TickSpeed());
	IGameController::OnPlayerConnect(pPlayer);
}

void CGameControllerVanillaPvP::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	const int ClientId = pPlayer->GetCid();
	DetachProjectiles(ClientId);
	IGameController::OnPlayerDisconnect(pPlayer, pReason);
}

void CGameControllerVanillaPvP::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(IsValidTeam(Team) && Team != pPlayer->GetTeam())
		DetachProjectiles(pPlayer->GetCid());
	IGameController::DoTeamChange(pPlayer, Team, DoChatMsg);
}

void CGameControllerVanillaPvP::DetachProjectiles(int ClientId)
{
	for(CProjectile *pProjectile = static_cast<CProjectile *>(Services().World().FindFirst(CGameWorld::ENTTYPE_PROJECTILE)); pProjectile; pProjectile = static_cast<CProjectile *>(pProjectile->TypeNext()))
	{
		if(pProjectile->GetOwnerId() == ClientId)
			pProjectile->LoseOwner();
	}
}

void CGameControllerVanillaPvP::StartRound()
{
	FinalizeMatchReportForRestart();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CPlayer *pPlayer = Services().Player(ClientId);
		if(pPlayer)
			static_cast<CPlayerVanilla *>(pPlayer)->ResetRoundState(Server()->Tick() - Server()->TickSpeed());
	}
	IGameController::StartRound();
}

int CGameControllerVanillaPvP::SnapPlayerScore(int SnappingClient, CPlayer *pPlayer)
{
	return static_cast<CPlayerVanilla *>(pPlayer)->m_Score;
}

int CGameControllerVanillaPvP::GameInfoFlags(int SnappingClient) const
{
	// The game type alone tells the client nothing anymore, so the ruleset has to
	// be spelled out: without it the shotgun spread this mode fires is not
	// predicted at all and the entities overlay is drawn from the DDNet tileset.
	return GAMEINFOFLAG_GAMETYPE_VANILLA |
	       GAMEINFOFLAG_ALLOW_EYE_WHEEL |
	       GAMEINFOFLAG_ALLOW_ZOOM |
	       GAMEINFOFLAG_PREDICT_VANILLA |
	       GAMEINFOFLAG_ENTITIES_VANILLA;
}

int CGameControllerVanillaPvP::GameInfoFlags2(int SnappingClient) const
{
	// Firing, bouncing and exploding are predicted here, so their effects may play
	// on the spot instead of a ping late.
	return GAMEINFOFLAG2_HUD_AMMO | GAMEINFOFLAG2_HUD_HEALTH_ARMOR | GAMEINFOFLAG2_PREDICT_EVENTS;
}

int CGameControllerVanillaPvP::ScoreLimit() const
{
	return g_Config.m_SvScorelimit;
}

int CGameControllerVanillaPvP::TimeLimit() const
{
	return g_Config.m_SvTimelimit;
}
