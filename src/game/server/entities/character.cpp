/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "character.h"

#include <antibot/antibot_data.h>

#include <base/log.h>

#include <engine/antibot.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/server/teams.h>
#include <game/team_state.h>

MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld, CNetObj_PlayerInput LastInput) :
	CEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER, false, vec2(0, 0), CCharacterCore::PhysicalSize())
{
	m_Health = 0;
	m_Armor = 0;
	m_TriggeredEvents7 = 0;
	m_StrongWeakId = 0;

	m_Input = LastInput;
	// never initialize both to zero
	m_Input.m_TargetX = 0;
	m_Input.m_TargetY = -1;

	m_LatestPrevPrevInput = m_LatestPrevInput = m_LatestInput = m_PrevInput = m_SavedInput = m_Input;

	m_LastTimeCp = -1;
	m_LastTimeCpBroadcasted = -1;
	for(float &CurrentTimeCp : m_aCurrentTimeCp)
	{
		CurrentTimeCp = 0.0f;
	}

	for(auto &Id : m_aUntranslatedId)
		Id = Server()->SnapNewId();
}

CCharacter::~CCharacter()
{
	for(auto Id : m_aUntranslatedId)
		Server()->SnapFreeId(Id.value());
}

void CCharacter::Reset()
{
	StopRecording();
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_EmoteStop = -1;
	m_LastAction = -1;
	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;
	m_LastPenalty = false;

	m_TeleGunTeleport = false;
	m_IsBlueTeleGunTeleport = false;

	m_pPlayer = pPlayer;
	m_Pos = Pos;

	mem_zero(&m_LatestPrevPrevInput, sizeof(m_LatestPrevPrevInput));
	m_LatestPrevPrevInput.m_TargetY = -1;
	m_NumInputs = 0;
	m_SpawnTick = Server()->Tick();
	m_WeaponChangeTick = Server()->Tick();
	Antibot()->OnSpawn(m_pPlayer->GetCid());

	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, Collision());
	m_Core.m_ActiveWeapon = WEAPON_GUN;
	m_Core.m_Pos = m_Pos;
	m_Core.m_Id = m_pPlayer->GetCid();
	m_Core.m_Jumps = 2;
	m_Paused = false;
	m_DDRaceState = ERaceState::NONE;
	m_StartTime = 0;
	m_PrevPos = m_Pos;
	m_TuneZone = Collision()->IsTune(Collision()->GetMapIndex(Pos));
	m_TuneZoneOld = m_TuneZone;
	m_NeededFaketuning = 0;
	m_Core.m_Tuning = TuningList()[m_TuneZone];
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCid()] = &m_Core;

	m_ReckoningTick = 0;
	m_SendCore = CCharacterCore();
	m_ReckoningCore = CCharacterCore();

	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);
	GameServer()->m_pController->RestoreCharacterAfterMapReload(this);
	GameServer()->m_pController->PublishMatchEvent(CMatchEventSpawn{m_pPlayer->GetCid(), m_pPlayer->GetTeam()});

	return true;
}

void CCharacter::Destroy()
{
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCid()] = nullptr;
	m_Alive = false;
	SetSolo(false);
}

void CCharacter::SetWeapon(int W)
{
	if(W == m_Core.m_ActiveWeapon)
		return;

	m_LastWeapon = m_Core.m_ActiveWeapon;
	m_QueuedWeapon = -1;
	m_Core.m_ActiveWeapon = W;
	GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH, TeamMask());

	if(m_Core.m_ActiveWeapon < 0 || m_Core.m_ActiveWeapon >= NUM_WEAPONS)
		m_Core.m_ActiveWeapon = 0;
	m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart = -1;
}

void CCharacter::SetJetpack(bool Active)
{
	m_Core.m_Jetpack = Active;
}

void CCharacter::SetEndlessJump(bool Active)
{
	m_Core.m_EndlessJump = Active;
}

void CCharacter::SetJumps(int Jumps)
{
	m_Core.m_Jumps = Jumps;
}

void CCharacter::SetSolo(bool Solo)
{
	m_Core.m_Solo = Solo;
	TeamsCore()->SetSolo(m_pPlayer->GetCid(), Solo);
}

void CCharacter::SetInvincible(bool Invincible)
{
	m_Core.m_Invincible = Invincible;
	if(Invincible)
		Unfreeze();

	SetEndlessJump(Invincible);
}

void CCharacter::SetCollisionDisabled(bool CollisionDisabled)
{
	m_Core.m_CollisionDisabled = CollisionDisabled;
}

void CCharacter::SetHookHitDisabled(bool HookHitDisabled)
{
	m_Core.m_HookHitDisabled = HookHitDisabled;
}

void CCharacter::SetLiveFrozen(bool Active)
{
	m_Core.m_LiveFrozen = Active;
}

void CCharacter::SetDeepFrozen(bool Active)
{
	m_Core.m_DeepFrozen = Active;
}

bool CCharacter::IsGrounded()
{
	if(Collision()->IsOnGround(m_Pos, GetProximityRadius()))
		return true;

	int MoveRestrictionsBelow = Collision()->GetMoveRestrictions(m_Pos + vec2(0, GetProximityRadius() / 2 + 4), 0.0f);
	return (MoveRestrictionsBelow & CANTMOVE_DOWN) != 0;
}

void CCharacter::HandleJetpack()
{
	if(m_Core.m_ActiveWeapon < 0)
		return;

	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_Core.m_ActiveWeapon == WEAPON_GRENADE || m_Core.m_ActiveWeapon == WEAPON_SHOTGUN || m_Core.m_ActiveWeapon == WEAPON_LASER)
		FullAuto = true;
	if(m_Core.m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN)
		FullAuto = true;

	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire & 1) && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire)
		return;

	// check for ammo
	if(!m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo || m_FreezeTime)
	{
		return;
	}

	switch(m_Core.m_ActiveWeapon)
	{
	case WEAPON_GUN:
	{
		if(m_Core.m_Jetpack)
		{
			float Strength = GetTuning(m_TuneZone)->m_JetpackStrength;
			TakeDamage(Direction * -1.0f * (Strength / 100.0f / 6.11f), 0, m_pPlayer->GetCid(), m_Core.m_ActiveWeapon);
		}
	}
	}
}

void CCharacter::HandleNinja()
{
	if(m_Core.m_ActiveWeapon != WEAPON_NINJA)
		return;

	if((Server()->Tick() - m_Core.m_Ninja.m_ActivationTick) > (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000))
	{
		// time's up, return
		RemoveNinja();
		return;
	}

	int NinjaTime = m_Core.m_Ninja.m_ActivationTick + (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000) - Server()->Tick();

	if(NinjaTime % Server()->TickSpeed() == 0 && NinjaTime / Server()->TickSpeed() <= 5)
	{
		GameServer()->CreateDamageInd(m_Pos, 0, NinjaTime / Server()->TickSpeed(), TeamMask() & GameServer()->ClientsMaskExcludeClientVersionAndHigher(VERSION_DDNET_NEW_HUD));
	}

	GameServer()->m_pController->SetArmorProgress(this, NinjaTime);

	// force ninja Weapon
	SetWeapon(WEAPON_NINJA);

	m_Core.m_Ninja.m_CurrentMoveTime--;

	if(m_Core.m_Ninja.m_CurrentMoveTime == 0)
	{
		// reset velocity
		m_Core.m_Vel = m_Core.m_Ninja.m_ActivationDir * m_Core.m_Ninja.m_OldVelAmount;
	}

	if(m_Core.m_Ninja.m_CurrentMoveTime > 0)
	{
		// Set velocity
		m_Core.m_Vel = m_Core.m_Ninja.m_ActivationDir * g_pData->m_Weapons.m_Ninja.m_Velocity;
		vec2 OldPos = m_Pos;
		vec2 GroundElasticity = vec2(
			GetTuning(m_TuneZone)->m_GroundElasticityX,
			GetTuning(m_TuneZone)->m_GroundElasticityY);

		Collision()->MoveBox(&m_Core.m_Pos, &m_Core.m_Vel, vec2(GetProximityRadius(), GetProximityRadius()), GroundElasticity);

		// reset velocity so the client doesn't predict stuff
		ResetVelocity();

		// check if we Hit anything along the way
		{
			CEntity *apEnts[MAX_CLIENTS];
			float Radius = GetProximityRadius() * 2.0f;
			int Num = GameServer()->m_World.FindEntities(OldPos, Radius, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

			// check that we're not in solo part
			if(TeamsCore()->GetSolo(m_pPlayer->GetCid()))
				return;

			for(int i = 0; i < Num; ++i)
			{
				auto *pChr = static_cast<CCharacter *>(apEnts[i]);
				if(pChr == this)
					continue;

				// Don't hit players in other teams
				if(Team() != pChr->Team())
					continue;

				const int ClientId = pChr->m_pPlayer->GetCid();

				// Don't hit players in solo parts
				if(TeamsCore()->GetSolo(ClientId))
					continue;

				// make sure we haven't Hit this object before
				bool AlreadyHit = false;
				for(int j = 0; j < m_NumObjectsHit; j++)
				{
					if(m_aHitObjects[j] == ClientId)
						AlreadyHit = true;
				}
				if(AlreadyHit)
					continue;

				// check so we are sufficiently close
				if(distance(pChr->m_Pos, m_Pos) > Radius)
					continue;

				// Hit a player, give them damage and stuffs...
				GameServer()->CreateSound(pChr->m_Pos, SOUND_NINJA_HIT, TeamMask());
				// set their velocity to fast upward (for now)
				dbg_assert(m_NumObjectsHit < MAX_CLIENTS, "m_aHitObjects overflow");
				m_aHitObjects[m_NumObjectsHit++] = ClientId;

				pChr->TakeDamage(vec2(0, -10.0f), g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage, m_pPlayer->GetCid(), WEAPON_NINJA);
			}
		}

		return;
	}
}

void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(m_ReloadTimer != 0 || m_QueuedWeapon == -1)
		return;
	if(m_Core.m_aWeapons[WEAPON_NINJA].m_Got || !m_Core.m_aWeapons[m_QueuedWeapon].m_Got)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_Core.m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	bool Anything = false;
	for(int i = 0; i < NUM_WEAPONS - 1; ++i)
		if(m_Core.m_aWeapons[i].m_Got)
			Anything = true;
	if(!Anything)
		return;
	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon + 1) % NUM_WEAPONS;
			if(m_Core.m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon - 1) < 0 ? NUM_WEAPONS - 1 : WantedWeapon - 1;
			if(m_Core.m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon - 1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_Core.m_ActiveWeapon && m_Core.m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer != 0)
	{
		if(m_LatestInput.m_Fire & 1)
		{
			Antibot()->OnHammerFireReloading(m_pPlayer->GetCid());
		}
		return;
	}

	DoWeaponSwitch();
	vec2 MouseTarget = vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY);
	vec2 Direction = normalize(MouseTarget);

	bool FullAuto = false;
	if(m_Core.m_ActiveWeapon == WEAPON_GRENADE || m_Core.m_ActiveWeapon == WEAPON_SHOTGUN || m_Core.m_ActiveWeapon == WEAPON_LASER)
		FullAuto = true;
	if(m_Core.m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN)
		FullAuto = true;
	// allow firing directly after coming out of freeze or being unfrozen
	// by something
	if(m_FrozenLastTick)
		FullAuto = true;

	// don't fire hammer when player is deep and sv_deepfly is disabled
	if(!g_Config.m_SvDeepfly && m_Core.m_ActiveWeapon == WEAPON_HAMMER && m_Core.m_DeepFrozen)
		return;

	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire & 1) && m_Core.m_ActiveWeapon >= 0 && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire)
		return;

	if(m_FreezeTime)
	{
		// Timer stuff to avoid shrieking orchestra caused by unfreeze-plasma
		if(m_PainSoundTimer <= 0 && !(m_LatestPrevInput.m_Fire & 1))
		{
			m_PainSoundTimer = 1 * Server()->TickSpeed();
			GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG, TeamMask()); // NOLINT(clang-analyzer-unix.Malloc)
		}
		return;
	}

	vec2 ProjStartPos = m_Pos + Direction * GetProximityRadius() * 0.75f;
	CWeaponFireContext FireContext = {this, m_Core.m_ActiveWeapon, Direction, MouseTarget, ProjStartPos, GetTuning(m_TuneZone)};
	const CWeaponFireResult FireResult = GameServer()->m_pController->OnCharacterFireWeapon(FireContext);
	if(FireResult.m_ReloadTicks > 0)
		m_ReloadTimer = FireResult.m_ReloadTicks;
	if(!FireResult.m_Fired)
		return;

	m_AttackTick = Server()->Tick();
	if(FireResult.m_ConsumeAmmo && m_Core.m_ActiveWeapon >= 0 && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo > 0)
		m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo--;

	// -1 is no weapon, handled here so pain sound still plays when firing in freeze
	if(!m_ReloadTimer && m_Core.m_ActiveWeapon != -1)
	{
		m_ReloadTimer = GetTuning(m_TuneZone)->GetWeaponFireDelay(m_Core.m_ActiveWeapon) * Server()->TickSpeed();
	}
	GameServer()->m_pController->PublishMatchEvent(CMatchEventShotFired{m_pPlayer->GetCid(), FireContext.m_Weapon});
}

void CCharacter::HandleWeapons()
{
	//ninja
	HandleNinja();
	HandleJetpack();

	if(m_PainSoundTimer > 0)
		m_PainSoundTimer--;

	// check reload timer
	if(m_ReloadTimer)
	{
		m_ReloadTimer--;
		return;
	}

	// fire Weapon, if wanted
	FireWeapon();

	// ammo regen
	if(m_Core.m_ActiveWeapon < 0 || m_Core.m_ActiveWeapon >= NUM_WEAPONS)
		return;
	const int AmmoRegenTime = g_pData->m_Weapons.m_aId[m_Core.m_ActiveWeapon].m_Ammoregentime;
	if(AmmoRegenTime && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo >= 0)
	{
		if(m_ReloadTimer <= 0)
		{
			if(m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart < 0)
				m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart = Server()->Tick();

			if(Server()->Tick() - m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart >= AmmoRegenTime * Server()->TickSpeed() / 1000)
			{
				m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo = std::min(
					m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo + 1,
					g_pData->m_Weapons.m_aId[m_Core.m_ActiveWeapon].m_Maxammo);
				m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart = -1;
			}
		}
		else
		{
			m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart = -1;
		}
	}
}

void CCharacter::GiveNinja()
{
	m_Core.m_Ninja.m_ActivationTick = Server()->Tick();
	m_Core.m_aWeapons[WEAPON_NINJA].m_Got = true;
	m_Core.m_aWeapons[WEAPON_NINJA].m_Ammo = -1;
	if(m_Core.m_ActiveWeapon != WEAPON_NINJA)
		m_LastWeapon = m_Core.m_ActiveWeapon;
	m_Core.m_ActiveWeapon = WEAPON_NINJA;

	// not used on ddrace
	// GameServer()->CreateSound(m_Pos, SOUND_PICKUP_NINJA, TeamMask());
}

void CCharacter::RemoveNinja()
{
	m_Core.m_Ninja.m_ActivationDir = vec2(0, 0);
	m_Core.m_Ninja.m_ActivationTick = 0;
	m_Core.m_Ninja.m_CurrentMoveTime = 0;
	m_Core.m_Ninja.m_OldVelAmount = 0;
	m_Core.m_aWeapons[WEAPON_NINJA].m_Got = false;
	m_Core.m_aWeapons[WEAPON_NINJA].m_Ammo = 0;
	m_Core.m_ActiveWeapon = m_LastWeapon;

	SetWeapon(m_Core.m_ActiveWeapon);
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

int CCharacter::DetermineEyeEmote()
{
	const bool IsFrozen = m_Core.m_DeepFrozen || m_FreezeTime > 0 || m_Core.m_LiveFrozen;
	const bool HasNinjajetpack = m_pPlayer->m_NinjaJetpack && m_Core.m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN;

	if(GetPlayer()->IsAfk() || GetPlayer()->IsPaused())
		return (m_Core.m_DeepFrozen || m_FreezeTime > 0) ? EMOTE_NORMAL : EMOTE_BLINK;
	if(m_EmoteType != EMOTE_NORMAL) // user manually set an eye emote using /emote
		return m_EmoteType;
	if(IsFrozen)
		return (m_Core.m_DeepFrozen || m_Core.m_LiveFrozen) ? EMOTE_PAIN : EMOTE_BLINK;
	if(HasNinjajetpack && !m_Core.m_DeepFrozen && m_FreezeTime == 0 && !m_Core.m_HasTelegunGun)
		return EMOTE_HAPPY;
	if(5 * Server()->TickSpeed() - ((Server()->Tick() - m_LastAction) % (5 * Server()->TickSpeed())) < 5)
		return EMOTE_BLINK;
	return EMOTE_NORMAL;
}

void CCharacter::OnPredictedInput(const CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_SavedInput, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));

	// it is not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;

	mem_copy(&m_SavedInput, &m_Input, sizeof(m_SavedInput));
}

void CCharacter::OnDirectInput(const CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));
	m_NumInputs++;

	// it is not allowed to aim in the center
	if(m_LatestInput.m_TargetX == 0 && m_LatestInput.m_TargetY == 0)
		m_LatestInput.m_TargetY = -1;

	Antibot()->OnDirectInput(m_pPlayer->GetCid());

	if(m_NumInputs > 1 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevPrevInput, &m_LatestPrevInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ReleaseHook()
{
	m_Core.SetHookedPlayer(-1);
	m_Core.m_HookState = HOOK_RETRACTED;
	m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
}

void CCharacter::ResetHook()
{
	ReleaseHook();
	m_Core.m_HookPos = m_Core.m_Pos;
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire & 1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

void CCharacter::PreTick()
{
	if(HasRaceTeams() && m_StartTime > Server()->Tick())
	{
		// Prevent the player from getting a negative time
		// The main reason why this can happen is because of time penalty tiles
		// However, other reasons are hereby also excluded
		GameServer()->SendChatTarget(m_pPlayer->GetCid(), "You died of old age");
		Die(m_pPlayer->GetCid(), WEAPON_WORLD);
	}

	if(m_Paused)
		return;

	// set emote
	if(m_EmoteStop < Server()->Tick())
	{
		SetEmote(m_pPlayer->GetDefaultEmote(), -1);
	}

	GameServer()->m_pController->TickCharacterPreCore(this);

	Antibot()->OnCharacterTick(m_pPlayer->GetCid());

	m_Core.m_Input = m_Input;
	m_Core.Tick(true, !g_Config.m_SvNoWeakHook);
}

void CCharacter::Tick()
{
	if(g_Config.m_SvNoWeakHook)
	{
		if(m_Paused)
			return;

		m_Core.TickDeferred();
	}
	else
	{
		PreTick();
	}

	if(!m_PrevInput.m_Hook && m_Input.m_Hook && !(m_Core.m_TriggeredEvents & COREEVENT_HOOK_ATTACH_PLAYER))
	{
		Antibot()->OnHookAttach(m_pPlayer->GetCid(), false);
	}

	// handle Weapons
	HandleWeapons();

	GameServer()->m_pController->TickCharacterPostCore(this);

	if(m_Core.m_TriggeredEvents & COREEVENT_HOOK_ATTACH_PLAYER)
	{
		const int HookedPlayer = m_Core.HookedPlayer();
		if(HookedPlayer != -1 && GameServer()->m_apPlayers[HookedPlayer]->GetTeam() != TEAM_SPECTATORS)
		{
			Antibot()->OnHookAttach(m_pPlayer->GetCid(), true);
		}
	}

	// Previnput
	m_PrevInput = m_Input;

	m_PrevPos = m_Core.m_Pos;
}

void CCharacter::TickDeferred()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, Collision(), TeamsCore());
		m_ReckoningCore.m_Id = m_pPlayer->GetCid();
		m_ReckoningCore.m_Tuning = CTuningParams();
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	//lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = Collision()->TestBox(m_Core.m_Pos, CCharacterCore::PhysicalSizeVec2());

	m_Core.m_Id = m_pPlayer->GetCid();
	m_Core.Move();
	bool StuckAfterMove = Collision()->TestBox(m_Core.m_Pos, CCharacterCore::PhysicalSizeVec2());
	m_Core.Quantize();
	bool StuckAfterQuant = Collision()->TestBox(m_Core.m_Pos, CCharacterCore::PhysicalSizeVec2());
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		} StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	{
		int Events = m_Core.m_TriggeredEvents;
		int CID = m_pPlayer->GetCid();

		// Some sounds are triggered client-side for the acting player (or for all players on Sixup)
		// so we need to avoid duplicating them
		CClientMask TeamMaskExceptSelfAndSixup = TeamMask();
		TeamMaskExceptSelfAndSixup.reset(CID);
		// Some are triggered client-side but only on Sixup
		CClientMask TeamMaskExceptSixup = TeamMask();
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(Server()->IsSixup(ClientId))
			{
				TeamMaskExceptSelfAndSixup.reset(ClientId);
				TeamMaskExceptSixup.reset(ClientId);
			}
		}

		if(Events & COREEVENT_GROUND_JUMP)
			GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, TeamMaskExceptSelfAndSixup);

		if(Events & COREEVENT_HOOK_ATTACH_PLAYER)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, TeamMaskExceptSixup);

		if(Events & COREEVENT_HOOK_ATTACH_GROUND)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, TeamMaskExceptSelfAndSixup);

		if(Events & COREEVENT_HOOK_HIT_NOHOOK)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, TeamMaskExceptSelfAndSixup);

		if(Events & COREEVENT_GROUND_JUMP)
			m_TriggeredEvents7 |= protocol7::COREEVENTFLAG_GROUND_JUMP;
		if(Events & COREEVENT_AIR_JUMP)
			m_TriggeredEvents7 |= protocol7::COREEVENTFLAG_AIR_JUMP;
		if(Events & COREEVENT_HOOK_ATTACH_PLAYER)
			m_TriggeredEvents7 |= protocol7::COREEVENTFLAG_HOOK_ATTACH_PLAYER;
		if(Events & COREEVENT_HOOK_ATTACH_GROUND)
			m_TriggeredEvents7 |= protocol7::COREEVENTFLAG_HOOK_ATTACH_GROUND;
		if(Events & COREEVENT_HOOK_HIT_NOHOOK)
			m_TriggeredEvents7 |= protocol7::COREEVENTFLAG_HOOK_HIT_NOHOOK;
	}

	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}

	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reckoning for a top of 3 seconds
		if(m_Core.m_Reset || m_ReckoningTick + Server()->TickSpeed() * 3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
			m_Core.m_Reset = false;
		}
	}
}

void CCharacter::TickPaused()
{
	++m_AttackTick;
	++m_DamageTakenTick;
	++m_Core.m_Ninja.m_ActivationTick;
	++m_ReckoningTick;
	if(m_LastAction != -1)
		++m_LastAction;
	if(m_Core.m_ActiveWeapon >= 0 && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart > -1)
		++m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10)
		return false;
	m_Health = std::clamp(m_Health + Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = std::clamp(m_Armor + Amount, 0, 10);
	return true;
}

void CCharacter::StopRecording()
{
	if(Server()->IsRecording(m_pPlayer->GetCid()))
	{
		if(!GameServer()->HasRaceScore())
		{
			Server()->StopRecord(m_pPlayer->GetCid());
			return;
		}
		CPlayerData *pData = GameServer()->RaceScore()->PlayerData(m_pPlayer->GetCid());

		if(pData->m_RecordStopTick - Server()->Tick() <= Server()->TickSpeed() && pData->m_RecordStopTick != -1)
			Server()->SaveDemo(m_pPlayer->GetCid(), pData->m_RecordFinishTime);
		else
			Server()->StopRecord(m_pPlayer->GetCid());

		pData->m_RecordStopTick = -1;
	}
}

void CCharacter::Die(int Killer, int Weapon, bool SendKillMsg)
{
	StopRecording();
	CPlayer *pKiller = Killer >= 0 && Killer < MAX_CLIENTS ? GameServer()->m_apPlayers[Killer] : nullptr;
	GameServer()->m_pController->OnCharacterDeath({this, pKiller, Killer, Weapon, SendKillMsg});
}

void CCharacter::FinalizeDeath(int Killer, int Weapon, bool SendKillMessage, int ModeSpecial)
{
	log_info("game", "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCid(), Server()->ClientName(m_pPlayer->GetCid()), Weapon, ModeSpecial);

	if(SendKillMessage)
	{
		CNetMsg_Sv_KillMsg Msg;
		Msg.m_Killer = Killer;
		Msg.m_Victim = m_pPlayer->GetCid();
		Msg.m_Weapon = Weapon;
		Msg.m_ModeSpecial = ModeSpecial;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
	}

	// a nice sound, and bursting tee death effect
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE, TeamMask());
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCid(), TeamMask());

	// Retain both death ticks for mode-owned respawn policies.
	m_pPlayer->m_PreviousDieTick = m_pPlayer->m_DieTick;
	m_pPlayer->m_DieTick = Server()->Tick();

	m_Alive = false;
	SetSolo(false);

	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCid()] = nullptr;
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon, bool CanDamage, int AttackerTeam)
{
	if(From >= 0 && From < MAX_CLIENTS && GameServer()->m_apPlayers[From] && Weapon >= 0 && Weapon < NUM_WEAPONS)
		GameServer()->m_pController->PublishMatchEvent(CMatchEventWeaponHit{From, m_pPlayer->GetCid(), Weapon});
	return GameServer()->m_pController->OnCharacterTakeDamage(this, Force, Dmg, From, Weapon, CanDamage, AttackerTeam);
}

void CCharacter::SnapCharacter(int SnappingClient, int MapId)
{
	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	CCharacterCore *pCore;
	int Weapon = m_Core.m_ActiveWeapon, AmmoCount = 0,
	    Health = 0, Armor = 0;
	int Emote = DetermineEyeEmote();
	int Tick;
	if(!m_ReckoningTick || GameServer()->m_pController->IsGamePaused())
	{
		Tick = 0;
		pCore = &m_Core;
	}
	else
	{
		Tick = m_ReckoningTick;
		pCore = &m_SendCore;
	}

	// use ninja graphic for old clients if player is frozen
	if((m_Core.m_DeepFrozen || m_FreezeTime > 0) && SnappingClientVersion < VERSION_DDNET_NEW_HUD)
		Weapon = WEAPON_NINJA;

	// solo, collision, jetpack and ninjajetpack prediction
	if(m_pPlayer->GetCid() == SnappingClient)
	{
		int Faketuning = 0;
		if(m_pPlayer->GetClientVersion() < VERSION_DDNET_NEW_HUD)
		{
			if(m_Core.m_Jetpack && Weapon != WEAPON_NINJA)
				Faketuning |= FAKETUNE_JETPACK;
			if(m_Core.m_Solo)
				Faketuning |= FAKETUNE_SOLO;
			if(m_Core.m_HammerHitDisabled)
				Faketuning |= FAKETUNE_NOHAMMER;
			if(m_Core.m_CollisionDisabled)
				Faketuning |= FAKETUNE_NOCOLL;
			if(m_Core.m_HookHitDisabled)
				Faketuning |= FAKETUNE_NOHOOK;
			if(!m_Core.m_EndlessJump && m_Core.m_Jumps == 0)
				Faketuning |= FAKETUNE_NOJUMP;
		}
		if(Faketuning != m_NeededFaketuning)
		{
			m_NeededFaketuning = Faketuning;
			GameServer()->SendTuningParams(m_pPlayer->GetCid(), m_TuneZone); // update tunings
		}
	}

	// use ninja graphic and set ammo count if player has ninjajetpack
	if(m_pPlayer->m_NinjaJetpack && m_Core.m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN && !m_Core.m_DeepFrozen && m_FreezeTime == 0 && !m_Core.m_HasTelegunGun)
	{
		Weapon = WEAPON_NINJA;
		AmmoCount = 10;
	}

	if(m_pPlayer->GetCid() == SnappingClient || SnappingClient == SERVER_DEMO_CLIENT ||
		(!g_Config.m_SvStrictSpectateMode && m_pPlayer->GetCid() == GameServer()->m_apPlayers[SnappingClient]->SpectatorId()))
	{
		Health = m_Health;
		Armor = m_Armor;
		AmmoCount = (m_FreezeTime == 0 && m_Core.m_ActiveWeapon >= 0) ? m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo : 0;
	}

	if(!Server()->IsSixup(SnappingClient))
	{
		CNetObj_Character Character = {};

		pCore->Write(&Character);

		Character.m_Tick = Tick;
		Character.m_Emote = Emote;

		if(!Server()->Translate(Character.m_HookedPlayer, SnappingClient))
			Character.m_HookedPlayer = -1;

		Character.m_AttackTick = m_AttackTick;
		Character.m_Direction = m_Input.m_Direction;
		Character.m_Weapon = Weapon;
		Character.m_AmmoCount = AmmoCount;
		Character.m_Health = Health;
		Character.m_Armor = Armor;
		Character.m_PlayerFlags = GetPlayer()->m_PlayerFlags;

		Server()->SnapNewItem(MapId, Character);
	}
	else
	{
		protocol7::CNetObj_Character Character = {};

		pCore->Write(reinterpret_cast<CNetObj_CharacterCore *>(static_cast<protocol7::CNetObj_CharacterCore *>(&Character)));
		if(Character.m_Angle > (int)(pi * 256.0f))
		{
			Character.m_Angle -= (int)(2.0f * pi * 256.0f);
		}

		if(!Server()->Translate(Character.m_HookedPlayer, SnappingClient))
			Character.m_HookedPlayer = -1;

		// m_HookTick can be negative when using the hook_duration tune, which 0.7 clients
		// will consider invalid. https://github.com/ddnet/ddnet/issues/3915
		Character.m_HookTick = std::max(0, Character.m_HookTick);

		Character.m_Tick = Tick;
		Character.m_Emote = Emote;
		Character.m_AttackTick = m_AttackTick;
		Character.m_Direction = m_Input.m_Direction;
		Character.m_Weapon = Weapon;
		Character.m_AmmoCount = AmmoCount;

		if(m_FreezeTime > 0 || m_Core.m_DeepFrozen)
			Character.m_AmmoCount = m_Core.m_FreezeStart + g_Config.m_SvFreezeDelay * Server()->TickSpeed();
		else if(Weapon == WEAPON_NINJA)
			Character.m_AmmoCount = m_Core.m_Ninja.m_ActivationTick + g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000;

		Character.m_Health = Health;
		Character.m_Armor = Armor;
		Character.m_TriggeredEvents = m_TriggeredEvents7;

		Server()->SnapNewItem(MapId, Character);
	}
}

bool CCharacter::CanSnapCharacter(int SnappingClient)
{
	return GameServer()->m_pController->CanSnapCharacter(this, SnappingClient);
}

bool CCharacter::IsSnappingCharacterInView(int SnappingClientId)
{
	// A player may not be clipped away if their hook or a hook attached to them is in the field of view
	bool PlayerAndHookNotInView = NetworkClippedLine(SnappingClientId, m_Pos, m_Core.m_HookPos);
	bool AttachedHookInView = false;
	if(PlayerAndHookNotInView)
	{
		for(const auto &AttachedPlayerId : m_Core.m_AttachedPlayers)
		{
			const CCharacter *pOtherPlayer = GameServer()->GetPlayerChar(AttachedPlayerId);
			if(pOtherPlayer && pOtherPlayer->m_Core.HookedPlayer() == m_pPlayer->GetCid())
			{
				if(!NetworkClippedLine(SnappingClientId, m_Pos, pOtherPlayer->m_Pos))
				{
					AttachedHookInView = true;
					break;
				}
			}
		}
	}
	if(PlayerAndHookNotInView && !AttachedHookInView)
	{
		return false;
	}
	return true;
}

void CCharacter::Snap(int SnappingClient)
{
	if(!CanSnapCharacter(SnappingClient))
	{
		return;
	}

	// always snap the snapping client, even if it is not in view
	if(!IsSnappingCharacterInView(SnappingClient) && m_pPlayer->GetCid() != SnappingClient)
		return;

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);

	// Translate id, if we are not in the map of the other person display us as weapon and our hook as a laser.
	// This shouldn't happen but is realistically impossible to avoid as soon as you zoom out a little or simply
	// more than 62 tees are around you. A bug might also occur in the playermapping algorithm, so best practice is to never let
	// a player be confused by why they got hooked or why some projectiles randomly appear by showing the player as weapon.
	int TranslatedId = m_pPlayer->GetCid();
	if(SnappingClient > -1 && !Server()->Translate(TranslatedId, SnappingClient))
	{
		CSnapContext SnapContext = CSnapContext(SnappingClientVersion, Server()->IsSixup(SnappingClient), SnappingClient);

		int Subtype = GetActiveWeapon();
		int Type = Subtype == WEAPON_NINJA ? POWERUP_NINJA : POWERUP_WEAPON;
		GameServer()->SnapPickup(SnapContext, m_aUntranslatedId[EUntranslatedMap::ID_WEAPON].value(), m_Pos, Type, Subtype, 0, PICKUPFLAG_NO_PREDICT);

		if(m_Core.m_HookState != HOOK_IDLE && m_Core.m_HookState != HOOK_RETRACTED)
		{
			int StartTick = Server()->Tick() - 3;
			GameServer()->SnapLaserObject(SnapContext, m_aUntranslatedId[EUntranslatedMap::ID_HOOK].value(), m_Core.m_HookPos, m_Pos, StartTick, -1, LASERTYPE_RIFLE);
		}
		return;
	}

	// otherwise show our normal tee and send ddnet character stuff
	SnapCharacter(SnappingClient, TranslatedId);
	GameServer()->m_pController->SnapCharacterMode(this, SnappingClient, TranslatedId);
}

void CCharacter::PostGlobalSnap()
{
	m_TriggeredEvents7 = 0;
}

// DDRace

bool CCharacter::CanCollide(int ClientId)
{
	return TeamsCore()->CanCollide(GetPlayer()->GetCid(), ClientId);
}
bool CCharacter::SameTeam(int ClientId)
{
	return TeamsCore()->SameTeam(GetPlayer()->GetCid(), ClientId);
}

int CCharacter::Team()
{
	return TeamsCore()->Team(m_pPlayer->GetCid());
}

void CCharacter::FillAntibot(CAntibotCharacterData *pData)
{
	pData->m_Pos = m_Pos;
	pData->m_Vel = m_Core.m_Vel;
	pData->m_Angle = m_Core.m_Angle;
	pData->m_HookedPlayer = m_Core.HookedPlayer();
	pData->m_SpawnTick = m_SpawnTick;
	pData->m_WeaponChangeTick = m_WeaponChangeTick;

	// 0
	pData->m_aLatestInputs[0].m_Direction = m_LatestInput.m_Direction;
	pData->m_aLatestInputs[0].m_TargetX = m_LatestInput.m_TargetX;
	pData->m_aLatestInputs[0].m_TargetY = m_LatestInput.m_TargetY;
	pData->m_aLatestInputs[0].m_Jump = m_LatestInput.m_Jump;
	pData->m_aLatestInputs[0].m_Fire = m_LatestInput.m_Fire;
	pData->m_aLatestInputs[0].m_Hook = m_LatestInput.m_Hook;
	pData->m_aLatestInputs[0].m_PlayerFlags = m_LatestInput.m_PlayerFlags;
	pData->m_aLatestInputs[0].m_WantedWeapon = m_LatestInput.m_WantedWeapon;
	pData->m_aLatestInputs[0].m_NextWeapon = m_LatestInput.m_NextWeapon;
	pData->m_aLatestInputs[0].m_PrevWeapon = m_LatestInput.m_PrevWeapon;

	// 1
	pData->m_aLatestInputs[1].m_Direction = m_LatestPrevInput.m_Direction;
	pData->m_aLatestInputs[1].m_TargetX = m_LatestPrevInput.m_TargetX;
	pData->m_aLatestInputs[1].m_TargetY = m_LatestPrevInput.m_TargetY;
	pData->m_aLatestInputs[1].m_Jump = m_LatestPrevInput.m_Jump;
	pData->m_aLatestInputs[1].m_Fire = m_LatestPrevInput.m_Fire;
	pData->m_aLatestInputs[1].m_Hook = m_LatestPrevInput.m_Hook;
	pData->m_aLatestInputs[1].m_PlayerFlags = m_LatestPrevInput.m_PlayerFlags;
	pData->m_aLatestInputs[1].m_WantedWeapon = m_LatestPrevInput.m_WantedWeapon;
	pData->m_aLatestInputs[1].m_NextWeapon = m_LatestPrevInput.m_NextWeapon;
	pData->m_aLatestInputs[1].m_PrevWeapon = m_LatestPrevInput.m_PrevWeapon;

	// 2
	pData->m_aLatestInputs[2].m_Direction = m_LatestPrevPrevInput.m_Direction;
	pData->m_aLatestInputs[2].m_TargetX = m_LatestPrevPrevInput.m_TargetX;
	pData->m_aLatestInputs[2].m_TargetY = m_LatestPrevPrevInput.m_TargetY;
	pData->m_aLatestInputs[2].m_Jump = m_LatestPrevPrevInput.m_Jump;
	pData->m_aLatestInputs[2].m_Fire = m_LatestPrevPrevInput.m_Fire;
	pData->m_aLatestInputs[2].m_Hook = m_LatestPrevPrevInput.m_Hook;
	pData->m_aLatestInputs[2].m_PlayerFlags = m_LatestPrevPrevInput.m_PlayerFlags;
	pData->m_aLatestInputs[2].m_WantedWeapon = m_LatestPrevPrevInput.m_WantedWeapon;
	pData->m_aLatestInputs[2].m_NextWeapon = m_LatestPrevPrevInput.m_NextWeapon;
	pData->m_aLatestInputs[2].m_PrevWeapon = m_LatestPrevPrevInput.m_PrevWeapon;
}

bool CCharacter::IsOnDeathTile()
{
	return Collision()->GetCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetFrontCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetFrontCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetFrontCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
	       Collision()->GetFrontCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH;
}

IAntibot *CCharacter::Antibot()
{
	return GameServer()->Antibot();
}

void CCharacter::SetTeamsCore(CTeamsCore *pTeamsCore)
{
	m_pTeamsCore = pTeamsCore;
	m_Core.SetTeamsCore(m_pTeamsCore);
}

void CCharacter::SetRaceTeams(CGameTeams *pTeams)
{
	m_pRaceTeams = pTeams;
	if(m_pRaceTeams != nullptr)
		SetTeamsCore(&m_pRaceTeams->m_Core);
}

bool CCharacter::Freeze(int Seconds)
{
	if(Seconds <= 0 || m_Core.m_Super || m_Core.m_Invincible || m_FreezeTime > Seconds * Server()->TickSpeed())
		return false;
	if(m_FreezeTime == 0 || m_Core.m_FreezeStart < Server()->Tick() - Server()->TickSpeed())
	{
		m_Armor = 0;
		m_FreezeTime = Seconds * Server()->TickSpeed();
		m_Core.m_FreezeStart = Server()->Tick();
		return true;
	}
	return false;
}

bool CCharacter::Freeze()
{
	return Freeze(g_Config.m_SvFreezeDelay);
}

bool CCharacter::Unfreeze()
{
	if(m_FreezeTime > 0)
	{
		m_Armor = 10;
		if(m_Core.m_ActiveWeapon >= 0 && !m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Got)
			m_Core.m_ActiveWeapon = WEAPON_GUN;
		m_FreezeTime = 0;
		m_Core.m_FreezeStart = 0;
		m_FrozenLastTick = true;
		return true;
	}
	return false;
}

void CCharacter::ResetJumps()
{
	m_Core.m_JumpedTotal = 0;
	m_Core.m_Jumped = 0;
}

void CCharacter::GiveWeapon(int Weapon, bool Remove)
{
	if(Weapon == WEAPON_NINJA)
	{
		if(Remove)
			RemoveNinja();
		else
			GiveNinja();
		return;
	}

	if(Remove)
	{
		if(GetActiveWeapon() == Weapon)
			SetActiveWeapon(WEAPON_GUN);
	}
	else
	{
		m_Core.m_aWeapons[Weapon].m_Ammo = -1;
	}

	m_Core.m_aWeapons[Weapon].m_Got = !Remove;
}

void CCharacter::GiveAllWeapons()
{
	for(int i = WEAPON_GUN; i < NUM_WEAPONS - 1; i++)
	{
		GiveWeapon(i);
	}
}

void CCharacter::ResetPickups()
{
	for(int i = WEAPON_SHOTGUN; i < NUM_WEAPONS - 1; i++)
	{
		m_Core.m_aWeapons[i].m_Got = false;
		if(m_Core.m_ActiveWeapon == i)
			m_Core.m_ActiveWeapon = WEAPON_GUN;
	}
}

void CCharacter::SetEndlessHook(bool Enable)
{
	if(m_Core.m_EndlessHook == Enable)
	{
		return;
	}
	GameServer()->SendChatTarget(GetPlayer()->GetCid(), Enable ? "Endless hook has been activated" : "Endless hook has been deactivated");

	m_Core.m_EndlessHook = Enable;
}

void CCharacter::Pause(bool Pause)
{
	m_Paused = Pause;
	if(Pause)
	{
		GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCid()] = nullptr;
		GameServer()->m_World.RemoveEntity(this);

		if(m_Core.HookedPlayer() != -1) // Keeping hook would allow cheats
		{
			ResetHook();
			GameWorld()->ReleaseHooked(GetPlayer()->GetCid());
		}
		m_PausedTick = Server()->Tick();
	}
	else
	{
		m_Core.m_Vel = vec2(0, 0);
		GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCid()] = &m_Core;
		GameServer()->m_World.InsertEntity(this);
		if(m_Core.m_FreezeStart > 0 && m_PausedTick >= 0)
		{
			m_Core.m_FreezeStart += Server()->Tick() - m_PausedTick;
		}
	}
}

CClientMask CCharacter::TeamMask()
{
	return GameServer()->m_pController->GetMaskForPlayerWorldEvent(GetPlayer()->GetCid());
}

void CCharacter::SetPosition(const vec2 &Position)
{
	m_Core.m_Pos = Position;
}

void CCharacter::Move(vec2 RelPos)
{
	m_Core.m_Pos += RelPos;
}

void CCharacter::ResetVelocity()
{
	m_Core.m_Vel = vec2(0, 0);
}

void CCharacter::SetVelocity(vec2 NewVelocity)
{
	m_Core.m_Vel = ClampVel(m_MoveRestrictions, NewVelocity);
}

// The method is needed only to reproduce 'shotgun bug' ddnet#5258
// Use SetVelocity() instead.
void CCharacter::SetRawVelocity(vec2 NewVelocity)
{
	m_Core.m_Vel = NewVelocity;
}

void CCharacter::AddVelocity(vec2 Addition)
{
	SetVelocity(m_Core.m_Vel + Addition);
}

vec2 CCharacter::VelocityDeltaAfterClamping(vec2 Addition) const
{
	return ClampVel(m_MoveRestrictions, m_Core.m_Vel + Addition) - m_Core.m_Vel;
}

void CCharacter::ActivateNinja(vec2 Direction)
{
	m_NumObjectsHit = 0;
	m_Core.m_Ninja.m_ActivationDir = Direction;
	m_Core.m_Ninja.m_CurrentMoveTime = g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000;
	m_Core.m_Ninja.m_OldVelAmount = std::clamp(length(m_Core.m_Vel), 0.0f, 6000.0f);
}

void CCharacter::ApplyMoveRestrictions()
{
	m_Core.m_Vel = ClampVel(m_MoveRestrictions, m_Core.m_Vel);
}

void CCharacter::SwapClients(int Client1, int Client2)
{
	const int HookedPlayer = m_Core.HookedPlayer();
	m_Core.SetHookedPlayer(HookedPlayer == Client1 ? Client2 : (HookedPlayer == Client2 ? Client1 : HookedPlayer));
}
