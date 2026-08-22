#include "ddrace_character.h"

#include <base/log.h>
#include <base/mem.h>
#include <base/time.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/server/entities/pickup.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/server/teams.h>

#include <algorithm>
#include <cmath>
#include <vector>

void CCharacterDDRace::PreTick()
{
	if(m_StartTime > Server()->Tick())
	{
		// Time penalty tiles can move the race start into the future. Do not expose a negative race time.
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You died of old age");
		Die(GetPlayer()->GetCid(), WEAPON_WORLD);
	}
	CCharacter::PreTick();
}

void CCharacterDDRace::Die(int Killer, int Weapon, bool SendKillMsg)
{
	if(Killer != WEAPON_GAME && m_SetSavePos[RESCUEMODE_AUTO])
		RaceTeams()->PlayerState(GetPlayer()->GetCid()).m_LastDeath = m_RescueTee[RESCUEMODE_AUTO];
	CCharacter::Die(Killer, Weapon, SendKillMsg);
}

void CCharacterDDRace::StopRecording()
{
	if(!Server()->IsRecording(GetPlayer()->GetCid()))
		return;

	CPlayerData *pData = RaceScore()->PlayerData(GetPlayer()->GetCid());
	if(pData->m_RecordStopTick != -1 && pData->m_RecordStopTick - Server()->Tick() <= Server()->TickSpeed())
		Server()->SaveDemo(GetPlayer()->GetCid(), pData->m_RecordFinishTime);
	else
		Server()->StopRecord(GetPlayer()->GetCid());
	pData->m_RecordStopTick = -1;
}

bool CCharacterDDRace::TryStartWarning()
{
	if(m_LastStartWarning >= 0 && m_LastStartWarning >= Server()->Tick() - 3 * Server()->TickSpeed())
		return false;
	m_LastStartWarning = Server()->Tick();
	return true;
}

void CCharacterDDRace::SendStartWarning(const char *pMessage)
{
	if(TryStartWarning())
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), pMessage);
}

void CCharacterDDRace::SetRaceTeams(CGameTeams *pTeams)
{
	m_pRaceTeams = pTeams;
	if(m_pRaceTeams != nullptr)
		SetTeamsCore(&m_pRaceTeams->m_Core);
}

void CCharacterDDRace::SetSuper(bool Super)
{
	if(!HasRaceTeams())
		return;

	// Disable invincible mode before activating super mode. Both modes active at the same time wouldn't necessarily break anything but it's not useful.
	if(Super)
		SetInvincible(false);

	bool WasSuper = m_Core.m_Super;
	m_Core.m_Super = Super;
	if(Super && !WasSuper)
	{
		m_TeamBeforeSuper = Team();
		char aError[512];
		if(!RaceTeams()->SetCharacterTeam(GetPlayer()->GetCid(), TEAM_SUPER, aError, sizeof(aError)))
			log_error("character", "failed to set super: %s", aError);
		m_DDRaceState = ERaceState::CHEATED;
	}
	else if(!Super && WasSuper)
	{
		RaceTeams()->SetForceCharacterTeam(GetPlayer()->GetCid(), m_TeamBeforeSuper);
	}
}

void CCharacterDDRace::SetInvincible(bool Invincible)
{
	// Disable super mode before activating invincible mode. Both modes active at the same time wouldn't necessarily break anything but it's not useful.
	if(Invincible)
		SetSuper(false);
	CCharacter::SetInvincible(Invincible);
}

bool CCharacterDDRace::TrySetRescue(int RescueMode)
{
	if(!HasRaceTeams())
		return false;

	bool Set = false;
	if(g_Config.m_SvRescue || ((g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO || Team() > TEAM_FLOCK) && RaceTeams()->IsValidTeamNumber(Team())))
	{
		// check for nearby health pickups (also freeze)
		bool InHealthPickup = false;
		if(!m_Core.m_IsInFreeze)
		{
			CEntity *apEnts[9];
			int Num = GameWorld()->FindEntities(m_Pos, GetProximityRadius() + CPickup::ms_CollisionExtraSize, apEnts, std::size(apEnts), CGameWorld::ENTTYPE_PICKUP);
			for(int i = 0; i < Num; ++i)
			{
				CPickup *pPickup = static_cast<CPickup *>(apEnts[i]);
				if(pPickup->Type() == POWERUP_HEALTH)
				{
					// This uses a separate variable InHealthPickup instead of setting m_Core.m_IsInFreeze
					// as the latter causes freezebars to flicker when standing in the freeze range of a
					// health pickup. When the same code for client prediction is added, the freezebars
					// still flicker, but only when standing at the edge of the health pickup's freeze range.
					InHealthPickup = true;
					break;
				}
			}
		}

		if(!m_Core.m_IsInFreeze && IsGrounded() && !m_Core.m_DeepFrozen && !InHealthPickup)
		{
			ForceSetRescue(RescueMode);
			Set = true;
		}
	}

	return Set;
}

void CCharacterDDRace::ForceSetRescue(int RescueMode)
{
	m_RescueTee[RescueMode].Save(this);
	m_SetSavePos[RescueMode] = true;
}

bool CCharacterDDRace::Rescue()
{
	if(!HasRaceTeams())
		return false;
	const int RescueMode = RaceTeams()->PlayerState(GetPlayer()->GetCid()).m_RescueMode;
	if(m_SetSavePos[RescueMode] && !m_Core.m_Super && !m_Core.m_Invincible)
	{
		if(m_LastRescue + (int64_t)g_Config.m_SvRescueDelay * Server()->TickSpeed() > Server()->Tick() && !RaceTeams()->IsPractice(Team()))
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "You have to wait %d seconds until you can rescue yourself", (int)((m_LastRescue + (int64_t)g_Config.m_SvRescueDelay * Server()->TickSpeed() - Server()->Tick()) / Server()->TickSpeed()));
			GameServer()->SendChatTarget(GetPlayer()->GetCid(), aBuf);
			return false;
		}

		m_LastRescue = Server()->Tick();
		int StartTime = m_StartTime;
		ERaceState DDRaceState = m_DDRaceState;
		m_RescueTee[RescueMode].Load(this);
		// Don't load these from saved tee:
		m_Core.m_Vel = vec2(0, 0);
		m_Core.m_HookState = HOOK_IDLE;
		m_StartTime = StartTime;
		m_DDRaceState = DDRaceState;
		m_SavedInput.m_Direction = 0;
		m_SavedInput.m_Jump = 0;
		// simulate releasing the fire button
		if((m_SavedInput.m_Fire & 1) != 0)
			m_SavedInput.m_Fire++;
		m_SavedInput.m_Fire &= INPUT_STATE_MASK;
		m_SavedInput.m_Hook = 0;
		m_pPlayer->Pause(CPlayer::PAUSE_NONE, true);
		return true;
	}
	return false;
}

void CCharacterDDRace::HandleBroadcast()
{
	CPlayerData *pData = RaceScore()->PlayerData(m_pPlayer->GetCid());

	if(m_DDRaceState == ERaceState::STARTED && m_pPlayer->GetClientVersion() == VERSION_VANILLA && !Server()->IsSixup(m_pPlayer->GetCid()) &&
		m_LastTimeCpBroadcasted != m_LastTimeCp && m_LastTimeCp > -1 &&
		m_TimeCpBroadcastEndTick > Server()->Tick() && pData->m_BestTime && pData->m_aBestTimeCp[m_LastTimeCp] != 0)
	{
		char aBroadcast[128];
		float Diff = m_aCurrentTimeCp[m_LastTimeCp] - pData->m_aBestTimeCp[m_LastTimeCp];
		str_format(aBroadcast, sizeof(aBroadcast), "Checkpoint | Diff : %+5.2f", Diff);
		GameServer()->SendBroadcast(aBroadcast, m_pPlayer->GetCid());
		m_LastTimeCpBroadcasted = m_LastTimeCp;
		m_LastBroadcast = Server()->Tick();
	}
	else if((m_pPlayer->m_TimerType == CPlayer::TIMERTYPE_BROADCAST || m_pPlayer->m_TimerType == CPlayer::TIMERTYPE_GAMETIMER_AND_BROADCAST) && m_DDRaceState == ERaceState::STARTED && m_LastBroadcast + Server()->TickSpeed() * g_Config.m_SvTimeInBroadcastInterval <= Server()->Tick())
	{
		char aBuf[32];
		int Time = (int64_t)100 * ((float)(Server()->Tick() - m_StartTime) / ((float)Server()->TickSpeed()));
		str_time(Time, ETimeFormat::HOURS, aBuf, sizeof(aBuf));
		GameServer()->SendBroadcast(aBuf, m_pPlayer->GetCid(), false);
		m_LastTimeCpBroadcasted = m_LastTimeCp;
		m_LastBroadcast = Server()->Tick();
	}
}

void CCharacterDDRace::HandleSkippableTiles(int Index)
{
	// handle death-tiles and leaving gamelayer
	if(IsOnDeathTile() &&
		!m_Core.m_Super && !m_Core.m_Invincible && !(Team() && RaceTeams()->TeeFinished(m_pPlayer->GetCid())))
	{
		if(RaceTeams()->IsPractice(Team()))
		{
			Freeze();
			// Rate limit death effects to once per second
			if(Server()->Tick() - m_pPlayer->m_DieTick >= Server()->TickSpeed())
			{
				m_pPlayer->m_DieTick = Server()->Tick();
				GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE, TeamMask());
				GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCid(), TeamMask());
			}
		}
		else
		{
			Die(m_pPlayer->GetCid(), WEAPON_WORLD);
			return;
		}
	}

	if(GameLayerClipped(m_Pos))
	{
		Die(m_pPlayer->GetCid(), WEAPON_WORLD);
		return;
	}

	if(Index < 0)
		return;

	// handle speedup tiles
	if(Collision()->IsSpeedup(Index))
	{
		vec2 Direction, TempVel = m_Core.m_Vel;
		int Force, Type, MaxSpeed = 0;
		Collision()->GetSpeedup(Index, &Direction, &Force, &MaxSpeed, &Type);

		if(Type == TILE_SPEED_BOOST_OLD)
		{
			float TeeAngle, SpeederAngle, DiffAngle, SpeedLeft, TeeSpeed;
			if(Force == 255 && MaxSpeed)
			{
				m_Core.m_Vel = Direction * (MaxSpeed / 5);
			}
			else
			{
				if(MaxSpeed > 0 && MaxSpeed < 5)
					MaxSpeed = 5;
				if(MaxSpeed > 0)
				{
					if(Direction.x > 0.0000001f)
						SpeederAngle = -std::atan(Direction.y / Direction.x);
					else if(Direction.x < 0.0000001f)
						SpeederAngle = std::atan(Direction.y / Direction.x) + 2.0f * std::asin(1.0f);
					else if(Direction.y > 0.0000001f)
						SpeederAngle = std::asin(1.0f);
					else
						SpeederAngle = std::asin(-1.0f);

					if(SpeederAngle < 0)
						SpeederAngle = 4.0f * std::asin(1.0f) + SpeederAngle;

					if(TempVel.x > 0.0000001f)
						TeeAngle = -std::atan(TempVel.y / TempVel.x);
					else if(TempVel.x < 0.0000001f)
						TeeAngle = std::atan(TempVel.y / TempVel.x) + 2.0f * std::asin(1.0f);
					else if(TempVel.y > 0.0000001f)
						TeeAngle = std::asin(1.0f);
					else
						TeeAngle = std::asin(-1.0f);

					if(TeeAngle < 0)
						TeeAngle = 4.0f * std::asin(1.0f) + TeeAngle;

					TeeSpeed = std::sqrt(std::pow(TempVel.x, 2) + std::pow(TempVel.y, 2));

					DiffAngle = SpeederAngle - TeeAngle;
					SpeedLeft = MaxSpeed / 5.0f - std::cos(DiffAngle) * TeeSpeed;
					if(absolute((int)SpeedLeft) > Force && SpeedLeft > 0.0000001f)
						TempVel += Direction * Force;
					else if(absolute((int)SpeedLeft) > Force)
						TempVel += Direction * -Force;
					else
						TempVel += Direction * SpeedLeft;
				}
				else
					TempVel += Direction * Force;

				m_Core.m_Vel = ClampVel(m_MoveRestrictions, TempVel);
			}
		}
		else if(Type == TILE_SPEED_BOOST)
		{
			constexpr float MaxSpeedScale = 5.0f;
			if(MaxSpeed == 0)
			{
				float MaxRampSpeed = GetTuning(TuningZone())->m_VelrampRange / (50 * log(std::max((float)GetTuning(TuningZone())->m_VelrampCurvature, 1.01f)));
				MaxSpeed = std::max(MaxRampSpeed, GetTuning(TuningZone())->m_VelrampStart / 50) * MaxSpeedScale;
			}

			// (signed) length of projection
			float CurrentDirectionalSpeed = dot(Direction, m_Core.m_Vel);
			float TempMaxSpeed = MaxSpeed / MaxSpeedScale;
			if(CurrentDirectionalSpeed + Force > TempMaxSpeed)
				TempVel += Direction * (TempMaxSpeed - CurrentDirectionalSpeed);
			else
				TempVel += Direction * Force;

			m_Core.m_Vel = ClampVel(m_MoveRestrictions, TempVel);
		}
	}
}

bool CCharacterDDRace::IsSwitchActiveCb(unsigned char Number, void *pUser)
{
	CCharacterDDRace *pThis = static_cast<CCharacterDDRace *>(pUser);
	auto &aSwitchers = pThis->Switchers();
	return !aSwitchers.empty() && pThis->Team() != TEAM_SUPER && aSwitchers[Number].m_aStatus[pThis->Team()];
}

void CCharacterDDRace::SetTimeCheckpoint(int TimeCheckpoint)
{
	if(TimeCheckpoint > -1 && m_DDRaceState == ERaceState::STARTED && m_aCurrentTimeCp[TimeCheckpoint] == 0.0f && m_Time != 0.0f)
	{
		m_LastTimeCp = TimeCheckpoint;
		m_aCurrentTimeCp[m_LastTimeCp] = m_Time;
		m_TimeCpBroadcastEndTick = Server()->Tick() + Server()->TickSpeed() * 2;
		if(m_pPlayer->GetClientVersion() >= VERSION_DDRACE || Server()->IsSixup(m_pPlayer->GetCid()))
		{
			CPlayerData *pData = RaceScore()->PlayerData(m_pPlayer->GetCid());
			if(pData->m_aBestTimeCp[m_LastTimeCp] != 0.0f)
			{
				if(Server()->IsSixup(m_pPlayer->GetCid()))
				{
					protocol7::CNetMsg_Sv_Checkpoint Msg;
					float Diff = (m_aCurrentTimeCp[m_LastTimeCp] - pData->m_aBestTimeCp[m_LastTimeCp]) * 1000;
					Msg.m_Diff = (int)Diff;
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, m_pPlayer->GetCid());
				}
				else
				{
					CNetMsg_Sv_DDRaceTime Msg;
					Msg.m_Time = (int)(m_Time * 100.0f);
					Msg.m_Finish = 0;
					float Diff = (m_aCurrentTimeCp[m_LastTimeCp] - pData->m_aBestTimeCp[m_LastTimeCp]) * 100;
					Msg.m_Check = (int)Diff;
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, m_pPlayer->GetCid());
				}
			}
		}
	}
}

void CCharacterDDRace::HandleTiles(int Index)
{
	int MapIndex = Index;
	m_TileIndex = Collision()->GetTileIndex(MapIndex);
	m_TileFIndex = Collision()->GetFrontTileIndex(MapIndex);
	m_MoveRestrictions = Collision()->GetMoveRestrictions(IsSwitchActiveCb, this, m_Pos, 18.0f, MapIndex);
	if(Index < 0)
	{
		m_LastRefillJumps = false;
		m_LastPenalty = false;
		m_LastBonus = false;
		return;
	}
	SetTimeCheckpoint(Collision()->IsTimeCheckpoint(MapIndex));
	SetTimeCheckpoint(Collision()->IsFrontTimeCheckpoint(MapIndex));
	int TeleCheckpoint = Collision()->IsTeleCheckpoint(MapIndex);
	if(TeleCheckpoint)
		m_TeleCheckpoint = TeleCheckpoint;

	GameServer()->GameHost().Controller()->HandleCharacterTiles(this, Index);
	if(!m_Alive)
		return;

	// freeze
	if(((m_TileIndex == TILE_FREEZE) || (m_TileFIndex == TILE_FREEZE)) && !m_Core.m_Super && !m_Core.m_Invincible && !m_Core.m_DeepFrozen)
	{
		Freeze();
	}
	else if(((m_TileIndex == TILE_UNFREEZE) || (m_TileFIndex == TILE_UNFREEZE)) && !m_Core.m_DeepFrozen)
		Unfreeze();

	// deep freeze
	if(((m_TileIndex == TILE_DFREEZE) || (m_TileFIndex == TILE_DFREEZE)) && !m_Core.m_Super && !m_Core.m_Invincible && !m_Core.m_DeepFrozen)
		m_Core.m_DeepFrozen = true;
	else if(((m_TileIndex == TILE_DUNFREEZE) || (m_TileFIndex == TILE_DUNFREEZE)) && !m_Core.m_Super && !m_Core.m_Invincible && m_Core.m_DeepFrozen)
		m_Core.m_DeepFrozen = false;

	// live freeze
	if(((m_TileIndex == TILE_LFREEZE) || (m_TileFIndex == TILE_LFREEZE)) && !m_Core.m_Super && !m_Core.m_Invincible)
	{
		m_Core.m_LiveFrozen = true;
	}
	else if(((m_TileIndex == TILE_LUNFREEZE) || (m_TileFIndex == TILE_LUNFREEZE)) && !m_Core.m_Super && !m_Core.m_Invincible)
	{
		m_Core.m_LiveFrozen = false;
	}

	// endless hook
	if(((m_TileIndex == TILE_EHOOK_ENABLE) || (m_TileFIndex == TILE_EHOOK_ENABLE)))
	{
		SetEndlessHook(true);
	}
	else if(((m_TileIndex == TILE_EHOOK_DISABLE) || (m_TileFIndex == TILE_EHOOK_DISABLE)))
	{
		SetEndlessHook(false);
	}

	// hit others
	if(((m_TileIndex == TILE_HIT_DISABLE) || (m_TileFIndex == TILE_HIT_DISABLE)) && (!m_Core.m_HammerHitDisabled || !m_Core.m_ShotgunHitDisabled || !m_Core.m_GrenadeHitDisabled || !m_Core.m_LaserHitDisabled))
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't hit others");
		m_Core.m_HammerHitDisabled = true;
		m_Core.m_ShotgunHitDisabled = true;
		m_Core.m_GrenadeHitDisabled = true;
		m_Core.m_LaserHitDisabled = true;
	}
	else if(((m_TileIndex == TILE_HIT_ENABLE) || (m_TileFIndex == TILE_HIT_ENABLE)) && (m_Core.m_HammerHitDisabled || m_Core.m_ShotgunHitDisabled || m_Core.m_GrenadeHitDisabled || m_Core.m_LaserHitDisabled))
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can hit others");
		m_Core.m_ShotgunHitDisabled = false;
		m_Core.m_GrenadeHitDisabled = false;
		m_Core.m_HammerHitDisabled = false;
		m_Core.m_LaserHitDisabled = false;
	}

	// collide with others
	if(((m_TileIndex == TILE_NPC_DISABLE) || (m_TileFIndex == TILE_NPC_DISABLE)) && !m_Core.m_CollisionDisabled)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't collide with others");
		m_Core.m_CollisionDisabled = true;
	}
	else if(((m_TileIndex == TILE_NPC_ENABLE) || (m_TileFIndex == TILE_NPC_ENABLE)) && m_Core.m_CollisionDisabled)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can collide with others");
		m_Core.m_CollisionDisabled = false;
	}

	// hook others
	if(((m_TileIndex == TILE_NPH_DISABLE) || (m_TileFIndex == TILE_NPH_DISABLE)) && !m_Core.m_HookHitDisabled)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't hook others");
		m_Core.m_HookHitDisabled = true;
	}
	else if(((m_TileIndex == TILE_NPH_ENABLE) || (m_TileFIndex == TILE_NPH_ENABLE)) && m_Core.m_HookHitDisabled)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can hook others");
		m_Core.m_HookHitDisabled = false;
	}

	// unlimited air jumps
	if(((m_TileIndex == TILE_UNLIMITED_JUMPS_ENABLE) || (m_TileFIndex == TILE_UNLIMITED_JUMPS_ENABLE)) && !m_Core.m_EndlessJump)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You have unlimited air jumps");
		m_Core.m_EndlessJump = true;
	}
	else if(((m_TileIndex == TILE_UNLIMITED_JUMPS_DISABLE) || (m_TileFIndex == TILE_UNLIMITED_JUMPS_DISABLE)) && m_Core.m_EndlessJump)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You don't have unlimited air jumps");
		m_Core.m_EndlessJump = false;
	}

	// walljump
	if((m_TileIndex == TILE_WALLJUMP) || (m_TileFIndex == TILE_WALLJUMP))
	{
		if(m_Core.m_Vel.y > 0 && m_Core.m_Colliding && m_Core.m_LeftWall)
		{
			m_Core.m_LeftWall = false;
			m_Core.m_JumpedTotal = m_Core.m_Jumps >= 2 ? m_Core.m_Jumps - 2 : 0;
			m_Core.m_Jumped = 1;
		}
	}

	// jetpack gun
	if(((m_TileIndex == TILE_JETPACK_ENABLE) || (m_TileFIndex == TILE_JETPACK_ENABLE)) && !m_Core.m_Jetpack)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You have a jetpack gun");
		m_Core.m_Jetpack = true;
	}
	else if(((m_TileIndex == TILE_JETPACK_DISABLE) || (m_TileFIndex == TILE_JETPACK_DISABLE)) && m_Core.m_Jetpack)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You lost your jetpack gun");
		m_Core.m_Jetpack = false;
	}

	// refill jumps
	if(((m_TileIndex == TILE_REFILL_JUMPS) || (m_TileFIndex == TILE_REFILL_JUMPS)) && !m_LastRefillJumps)
	{
		m_Core.m_JumpedTotal = 0;
		m_Core.m_Jumped = 0;
		m_LastRefillJumps = true;
	}
	if((m_TileIndex != TILE_REFILL_JUMPS) && (m_TileFIndex != TILE_REFILL_JUMPS))
	{
		m_LastRefillJumps = false;
	}

	// Teleport gun
	if(((m_TileIndex == TILE_TELE_GUN_ENABLE) || (m_TileFIndex == TILE_TELE_GUN_ENABLE)) && !m_Core.m_HasTelegunGun)
	{
		m_Core.m_HasTelegunGun = true;

		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "Teleport gun enabled");
	}
	else if(((m_TileIndex == TILE_TELE_GUN_DISABLE) || (m_TileFIndex == TILE_TELE_GUN_DISABLE)) && m_Core.m_HasTelegunGun)
	{
		m_Core.m_HasTelegunGun = false;

		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "Teleport gun disabled");
	}

	if(((m_TileIndex == TILE_TELE_GRENADE_ENABLE) || (m_TileFIndex == TILE_TELE_GRENADE_ENABLE)) && !m_Core.m_HasTelegunGrenade)
	{
		m_Core.m_HasTelegunGrenade = true;

		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "Teleport grenade enabled");
	}
	else if(((m_TileIndex == TILE_TELE_GRENADE_DISABLE) || (m_TileFIndex == TILE_TELE_GRENADE_DISABLE)) && m_Core.m_HasTelegunGrenade)
	{
		m_Core.m_HasTelegunGrenade = false;

		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "Teleport grenade disabled");
	}

	if(((m_TileIndex == TILE_TELE_LASER_ENABLE) || (m_TileFIndex == TILE_TELE_LASER_ENABLE)) && !m_Core.m_HasTelegunLaser)
	{
		m_Core.m_HasTelegunLaser = true;

		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "Teleport laser enabled");
	}
	else if(((m_TileIndex == TILE_TELE_LASER_DISABLE) || (m_TileFIndex == TILE_TELE_LASER_DISABLE)) && m_Core.m_HasTelegunLaser)
	{
		m_Core.m_HasTelegunLaser = false;

		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "Teleport laser disabled");
	}

	// stopper
	if(m_Core.m_Vel.y > 0 && (m_MoveRestrictions & CANTMOVE_DOWN))
	{
		m_Core.m_Jumped = 0;
		m_Core.m_JumpedTotal = 0;
	}
	ApplyMoveRestrictions();

	// handle switch tiles
	const int SwitchType = Collision()->GetSwitchType(MapIndex);
	const int SwitchNumber = Collision()->GetSwitchNumber(MapIndex);
	const int SwitchDelay = Collision()->GetSwitchDelay(MapIndex);
	if(SwitchType == TILE_SWITCHOPEN && Team() != TEAM_SUPER && SwitchNumber > 0)
	{
		Switchers()[SwitchNumber].m_aStatus[Team()] = true;
		Switchers()[SwitchNumber].m_aEndTick[Team()] = 0;
		Switchers()[SwitchNumber].m_aType[Team()] = TILE_SWITCHOPEN;
		Switchers()[SwitchNumber].m_aLastUpdateTick[Team()] = Server()->Tick();
	}
	else if(SwitchType == TILE_SWITCHTIMEDOPEN && Team() != TEAM_SUPER && SwitchNumber > 0)
	{
		Switchers()[SwitchNumber].m_aStatus[Team()] = true;
		Switchers()[SwitchNumber].m_aEndTick[Team()] = Server()->Tick() + 1 + SwitchDelay * Server()->TickSpeed();
		Switchers()[SwitchNumber].m_aType[Team()] = TILE_SWITCHTIMEDOPEN;
		Switchers()[SwitchNumber].m_aLastUpdateTick[Team()] = Server()->Tick();
	}
	else if(SwitchType == TILE_SWITCHTIMEDCLOSE && Team() != TEAM_SUPER && SwitchNumber > 0)
	{
		Switchers()[SwitchNumber].m_aStatus[Team()] = false;
		Switchers()[SwitchNumber].m_aEndTick[Team()] = Server()->Tick() + 1 + SwitchDelay * Server()->TickSpeed();
		Switchers()[SwitchNumber].m_aType[Team()] = TILE_SWITCHTIMEDCLOSE;
		Switchers()[SwitchNumber].m_aLastUpdateTick[Team()] = Server()->Tick();
	}
	else if(SwitchType == TILE_SWITCHCLOSE && Team() != TEAM_SUPER && SwitchNumber > 0)
	{
		Switchers()[SwitchNumber].m_aStatus[Team()] = false;
		Switchers()[SwitchNumber].m_aEndTick[Team()] = 0;
		Switchers()[SwitchNumber].m_aType[Team()] = TILE_SWITCHCLOSE;
		Switchers()[SwitchNumber].m_aLastUpdateTick[Team()] = Server()->Tick();
	}
	else if(SwitchType == TILE_FREEZE && Team() != TEAM_SUPER && !m_Core.m_Invincible)
	{
		if(SwitchNumber == 0 || Switchers()[SwitchNumber].m_aStatus[Team()])
		{
			Freeze(SwitchDelay);
		}
	}
	else if(SwitchType == TILE_DFREEZE && Team() != TEAM_SUPER && !m_Core.m_Invincible)
	{
		if(SwitchNumber == 0 || Switchers()[SwitchNumber].m_aStatus[Team()])
			m_Core.m_DeepFrozen = true;
	}
	else if(SwitchType == TILE_DUNFREEZE && Team() != TEAM_SUPER && !m_Core.m_Invincible)
	{
		if(SwitchNumber == 0 || Switchers()[SwitchNumber].m_aStatus[Team()])
			m_Core.m_DeepFrozen = false;
	}
	else if(SwitchType == TILE_LFREEZE && Team() != TEAM_SUPER && !m_Core.m_Invincible)
	{
		if(SwitchNumber == 0 || Switchers()[SwitchNumber].m_aStatus[Team()])
		{
			m_Core.m_LiveFrozen = true;
		}
	}
	else if(SwitchType == TILE_LUNFREEZE && Team() != TEAM_SUPER && !m_Core.m_Invincible)
	{
		if(SwitchNumber == 0 || Switchers()[SwitchNumber].m_aStatus[Team()])
		{
			m_Core.m_LiveFrozen = false;
		}
	}
	else if(SwitchType == TILE_HIT_ENABLE && m_Core.m_HammerHitDisabled && SwitchDelay == WEAPON_HAMMER)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can hammer hit others");
		m_Core.m_HammerHitDisabled = false;
	}
	else if(SwitchType == TILE_HIT_DISABLE && !(m_Core.m_HammerHitDisabled) && SwitchDelay == WEAPON_HAMMER)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't hammer hit others");
		m_Core.m_HammerHitDisabled = true;
	}
	else if(SwitchType == TILE_HIT_ENABLE && m_Core.m_ShotgunHitDisabled && SwitchDelay == WEAPON_SHOTGUN)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can shoot others with shotgun");
		m_Core.m_ShotgunHitDisabled = false;
	}
	else if(SwitchType == TILE_HIT_DISABLE && !(m_Core.m_ShotgunHitDisabled) && SwitchDelay == WEAPON_SHOTGUN)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't shoot others with shotgun");
		m_Core.m_ShotgunHitDisabled = true;
	}
	else if(SwitchType == TILE_HIT_ENABLE && m_Core.m_GrenadeHitDisabled && SwitchDelay == WEAPON_GRENADE)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can shoot others with grenade");
		m_Core.m_GrenadeHitDisabled = false;
	}
	else if(SwitchType == TILE_HIT_DISABLE && !(m_Core.m_GrenadeHitDisabled) && SwitchDelay == WEAPON_GRENADE)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't shoot others with grenade");
		m_Core.m_GrenadeHitDisabled = true;
	}
	else if(SwitchType == TILE_HIT_ENABLE && m_Core.m_LaserHitDisabled && SwitchDelay == WEAPON_LASER)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can shoot others with laser");
		m_Core.m_LaserHitDisabled = false;
	}
	else if(SwitchType == TILE_HIT_DISABLE && !(m_Core.m_LaserHitDisabled) && SwitchDelay == WEAPON_LASER)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCid(), "You can't shoot others with laser");
		m_Core.m_LaserHitDisabled = true;
	}
	else if(SwitchType == TILE_JUMP)
	{
		int NewJumps = SwitchDelay;
		if(NewJumps == 255)
		{
			NewJumps = -1;
		}

		if(NewJumps != m_Core.m_Jumps)
		{
			char aBuf[256];
			if(NewJumps == -1)
				str_copy(aBuf, "You only have your ground jump now");
			else if(NewJumps == 1)
				str_format(aBuf, sizeof(aBuf), "You can jump %d time", NewJumps);
			else
				str_format(aBuf, sizeof(aBuf), "You can jump %d times", NewJumps);
			GameServer()->SendChatTarget(GetPlayer()->GetCid(), aBuf);
			m_Core.m_Jumps = NewJumps;
		}
	}
	else if(SwitchType == TILE_ADD_TIME && !m_LastPenalty)
	{
		const int Minutes = SwitchDelay;
		const int Seconds = SwitchNumber;
		int Team = TeamsCore()->Team(m_Core.m_Id);

		m_StartTime -= (Minutes * 60 + Seconds) * Server()->TickSpeed();

		if((g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO || (Team != TEAM_FLOCK && !RaceTeams()->TeamFlock(Team))) && Team != TEAM_SUPER)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(TeamsCore()->Team(i) == Team && i != m_Core.m_Id && GameServer()->m_apPlayers[i])
				{
					CCharacterDDRace *pChar = static_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[i]->GetCharacter());

					if(pChar)
						pChar->m_StartTime = m_StartTime;
				}
			}
		}

		m_LastPenalty = true;
	}
	else if(SwitchType == TILE_SUBTRACT_TIME && !m_LastBonus)
	{
		const int Minutes = SwitchDelay;
		const int Seconds = SwitchNumber;
		int Team = TeamsCore()->Team(m_Core.m_Id);

		m_StartTime += (Minutes * 60 + Seconds) * Server()->TickSpeed();
		if(m_StartTime > Server()->Tick())
			m_StartTime = Server()->Tick();

		if((g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO || (Team != TEAM_FLOCK && !RaceTeams()->TeamFlock(Team))) && Team != TEAM_SUPER)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(TeamsCore()->Team(i) == Team && i != m_Core.m_Id && GameServer()->m_apPlayers[i])
				{
					CCharacterDDRace *pChar = static_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[i]->GetCharacter());

					if(pChar)
						pChar->m_StartTime = m_StartTime;
				}
			}
		}

		m_LastBonus = true;
	}

	if(SwitchType != TILE_ADD_TIME)
	{
		m_LastPenalty = false;
	}

	if(SwitchType != TILE_SUBTRACT_TIME)
	{
		m_LastBonus = false;
	}

	int z = Collision()->IsTeleport(MapIndex);
	if(!g_Config.m_SvOldTeleportHook && !g_Config.m_SvOldTeleportWeapons && z && !Collision()->TeleOuts(z - 1).empty())
	{
		if(m_Core.m_Super || m_Core.m_Invincible)
			return;
		int TeleOut = GameWorld()->m_Core.RandomOr0(Collision()->TeleOuts(z - 1).size());
		m_Core.m_Pos = Collision()->TeleOuts(z - 1)[TeleOut];
		if(!g_Config.m_SvTeleportHoldHook)
		{
			ResetHook();
		}
		if(g_Config.m_SvTeleportLoseWeapons)
			ResetPickups();
		return;
	}
	const int EvilTeleport = Collision()->IsEvilTeleport(MapIndex);
	if(EvilTeleport && !Collision()->TeleOuts(EvilTeleport - 1).empty())
	{
		if(m_Core.m_Super || m_Core.m_Invincible)
			return;
		int TeleOut = GameWorld()->m_Core.RandomOr0(Collision()->TeleOuts(EvilTeleport - 1).size());
		m_Core.m_Pos = Collision()->TeleOuts(EvilTeleport - 1)[TeleOut];
		if(!g_Config.m_SvOldTeleportHook && !g_Config.m_SvOldTeleportWeapons)
		{
			m_Core.m_Vel = vec2(0, 0);

			if(!g_Config.m_SvTeleportHoldHook)
			{
				ResetHook();
				GameWorld()->ReleaseHooked(GetPlayer()->GetCid());
			}
			if(g_Config.m_SvTeleportLoseWeapons)
			{
				ResetPickups();
			}
		}
		return;
	}
	if(Collision()->IsCheckEvilTeleport(MapIndex))
	{
		if(m_Core.m_Super || m_Core.m_Invincible)
			return;
		// first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous checkpoints
		for(int k = m_TeleCheckpoint - 1; k >= 0; k--)
		{
			if(!Collision()->TeleCheckOuts(k).empty())
			{
				int TeleOut = GameWorld()->m_Core.RandomOr0(Collision()->TeleCheckOuts(k).size());
				m_Core.m_Pos = Collision()->TeleCheckOuts(k)[TeleOut];
				m_Core.m_Vel = vec2(0, 0);

				if(!g_Config.m_SvTeleportHoldHook)
				{
					ResetHook();
					GameWorld()->ReleaseHooked(GetPlayer()->GetCid());
				}

				return;
			}
		}
		// if no checkpointout have been found (or if there no recorded checkpoint), teleport to start
		vec2 SpawnPos;
		if(GameServer()->GameHost().Controller()->CanSpawn(m_pPlayer->GetTeam(), &SpawnPos, GetPlayer()->GetCid()))
		{
			m_Core.m_Pos = SpawnPos;
			m_Core.m_Vel = vec2(0, 0);

			if(!g_Config.m_SvTeleportHoldHook)
			{
				ResetHook();
				GameWorld()->ReleaseHooked(GetPlayer()->GetCid());
			}
		}
		return;
	}
	if(Collision()->IsCheckTeleport(MapIndex))
	{
		if(m_Core.m_Super || m_Core.m_Invincible)
			return;
		// first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous checkpoints
		for(int k = m_TeleCheckpoint - 1; k >= 0; k--)
		{
			if(!Collision()->TeleCheckOuts(k).empty())
			{
				int TeleOut = GameWorld()->m_Core.RandomOr0(Collision()->TeleCheckOuts(k).size());
				m_Core.m_Pos = Collision()->TeleCheckOuts(k)[TeleOut];

				if(!g_Config.m_SvTeleportHoldHook)
				{
					ResetHook();
				}

				return;
			}
		}
		// if no checkpointout have been found (or if there no recorded checkpoint), teleport to start
		vec2 SpawnPos;
		if(GameServer()->GameHost().Controller()->CanSpawn(m_pPlayer->GetTeam(), &SpawnPos, GetPlayer()->GetCid()))
		{
			m_Core.m_Pos = SpawnPos;

			if(!g_Config.m_SvTeleportHoldHook)
			{
				ResetHook();
			}
		}
		return;
	}
}

void CCharacterDDRace::HandleTuneLayer()
{
	m_TuneZoneOld = TuningZone();
	SetTuningZone(GameServer()->GameHost().Controller()->TuningZoneAt(m_Pos));

	if(TuningZone() != m_TuneZoneOld) // don't send tunigs all the time
	{
		// send zone msgs
		SendZoneMsgs();
	}
}

void CCharacterDDRace::SendZoneMsgs()
{
	// send zone leave msg
	// (m_TuneZoneOld >= 0: avoid zone leave msgs on spawn)
	if(m_TuneZoneOld >= 0 && GameServer()->m_aaZoneLeaveMsg[m_TuneZoneOld][0])
	{
		const char *pCur = GameServer()->m_aaZoneLeaveMsg[m_TuneZoneOld];
		const char *pPos;
		while((pPos = str_find(pCur, "\\n")))
		{
			char aBuf[256];
			str_copy(aBuf, pCur, pPos - pCur + 1);
			aBuf[pPos - pCur + 1] = '\0';
			pCur = pPos + 2;
			GameServer()->SendChatTarget(m_pPlayer->GetCid(), aBuf);
		}
		GameServer()->SendChatTarget(m_pPlayer->GetCid(), pCur);
	}
	// send zone enter msg
	if(GameServer()->m_aaZoneEnterMsg[TuningZone()][0])
	{
		const char *pCur = GameServer()->m_aaZoneEnterMsg[TuningZone()];
		const char *pPos;
		while((pPos = str_find(pCur, "\\n")))
		{
			char aBuf[256];
			str_copy(aBuf, pCur, pPos - pCur + 1);
			aBuf[pPos - pCur + 1] = '\0';
			pCur = pPos + 2;
			GameServer()->SendChatTarget(m_pPlayer->GetCid(), aBuf);
		}
		GameServer()->SendChatTarget(m_pPlayer->GetCid(), pCur);
	}
}

void CCharacterDDRace::SnapDDRace(int SnappingClient, int Id)
{
	CNetObj_DDNetCharacter DDNetCharacter = {};

	DDNetCharacter.m_Flags = 0;
	if(m_Core.m_Solo)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_SOLO;
	if(m_Core.m_Super)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_SUPER;
	if(m_Core.m_Invincible)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_INVINCIBLE;
	if(m_Core.m_EndlessHook)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_ENDLESS_HOOK;
	if(m_Core.m_CollisionDisabled || !GetTuning(TuningZone())->m_PlayerCollision)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_COLLISION_DISABLED;
	if(m_Core.m_HookHitDisabled || !GetTuning(TuningZone())->m_PlayerHooking)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_HOOK_HIT_DISABLED;
	if(m_Core.m_EndlessJump)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_ENDLESS_JUMP;
	if(m_Core.m_Jetpack)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_JETPACK;
	if(m_Core.m_HammerHitDisabled)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_HAMMER_HIT_DISABLED;
	if(m_Core.m_ShotgunHitDisabled)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_SHOTGUN_HIT_DISABLED;
	if(m_Core.m_GrenadeHitDisabled)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_GRENADE_HIT_DISABLED;
	if(m_Core.m_LaserHitDisabled)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_LASER_HIT_DISABLED;
	if(m_Core.m_HasTelegunGun)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_TELEGUN_GUN;
	if(m_Core.m_HasTelegunGrenade)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_TELEGUN_GRENADE;
	if(m_Core.m_HasTelegunLaser)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_TELEGUN_LASER;
	if(m_Core.m_aWeapons[WEAPON_HAMMER].m_Got)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_WEAPON_HAMMER;
	if(m_Core.m_aWeapons[WEAPON_GUN].m_Got)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_WEAPON_GUN;
	if(m_Core.m_aWeapons[WEAPON_SHOTGUN].m_Got)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_WEAPON_SHOTGUN;
	if(m_Core.m_aWeapons[WEAPON_GRENADE].m_Got)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_WEAPON_GRENADE;
	if(m_Core.m_aWeapons[WEAPON_LASER].m_Got)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_WEAPON_LASER;
	if(m_Core.m_ActiveWeapon == WEAPON_NINJA)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_WEAPON_NINJA;
	if(m_Core.m_LiveFrozen)
		DDNetCharacter.m_Flags |= CHARACTERFLAG_MOVEMENTS_DISABLED;

	DDNetCharacter.m_FreezeEnd = m_Core.m_DeepFrozen ? -1 : (m_FreezeTime == 0 ? 0 : Server()->Tick() + m_FreezeTime);
	DDNetCharacter.m_Jumps = m_Core.m_Jumps;
	DDNetCharacter.m_TeleCheckpoint = m_TeleCheckpoint;
	int StrongWeakId = m_StrongWeakId;
	if(!Server()->ClientSupportsServerMaxClients(SnappingClient) && SnappingClient >= 0 && GameServer()->m_apPlayers[SnappingClient])
		StrongWeakId = GameServer()->m_apPlayers[SnappingClient]->m_aStrongWeakId[Id];
	DDNetCharacter.m_StrongWeakId = StrongWeakId;

	// Display Information
	DDNetCharacter.m_JumpedTotal = m_Core.m_JumpedTotal;
	DDNetCharacter.m_NinjaActivationTick = m_Core.m_Ninja.m_ActivationTick;
	DDNetCharacter.m_FreezeStart = m_Core.m_FreezeStart;
	if(m_Core.m_IsInFreeze)
	{
		DDNetCharacter.m_Flags |= CHARACTERFLAG_IN_FREEZE;
	}
	if(RaceTeams()->IsPractice(Team()))
	{
		DDNetCharacter.m_Flags |= CHARACTERFLAG_PRACTICE_MODE;
	}
	if(RaceTeams()->TeamLocked(Team()))
	{
		DDNetCharacter.m_Flags |= CHARACTERFLAG_LOCK_MODE;
	}
	if(RaceTeams()->TeamFlock(Team()))
	{
		DDNetCharacter.m_Flags |= CHARACTERFLAG_TEAM0_MODE;
	}
	DDNetCharacter.m_TargetX = m_Core.m_Input.m_TargetX;
	DDNetCharacter.m_TargetY = m_Core.m_Input.m_TargetY;

	// OVERRIDE_NONE is the default value, the object is zeroed, so it would incorrectly become 0
	DDNetCharacter.m_TuneZoneOverride = TuneZone::OVERRIDE_NONE;

	Server()->SnapNewItem(Id, DDNetCharacter);
}

void CCharacterDDRace::DDRaceTick()
{
	mem_copy(&m_Input, &m_SavedInput, sizeof(m_Input));
	GameServer()->GameHost().Controller()->SetArmorProgress(this, m_FreezeTime);
	if(m_Core.m_LiveFrozen && !m_Core.m_Super && !m_Core.m_Invincible)
	{
		m_Input.m_Direction = 0;
		m_Input.m_Jump = 0;
		// Hook is possible in live freeze
	}
	if(m_FreezeTime > 0)
	{
		if(m_FreezeTime % Server()->TickSpeed() == Server()->TickSpeed() - 1)
		{
			GameServer()->CreateDamageInd(m_Pos, 0, (m_FreezeTime + 1) / Server()->TickSpeed(), TeamMask() & GameServer()->ClientsMaskExcludeClientVersionAndHigher(VERSION_DDNET_NEW_HUD));
		}
		m_FreezeTime--;
		m_Input.m_Direction = 0;
		m_Input.m_Jump = 0;
		m_Input.m_Hook = 0;
		if(m_FreezeTime == 1)
			Unfreeze();
	}

	HandleTuneLayer(); // need this before coretick

	// check if the tee is in any type of freeze
	int Index = Collision()->GetPureMapIndex(m_Pos);
	const int aTiles[] = {
		Collision()->GetTileIndex(Index),
		Collision()->GetFrontTileIndex(Index),
		Collision()->GetSwitchType(Index)};
	m_Core.m_IsInFreeze = false;
	for(const int Tile : aTiles)
	{
		if(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE || Tile == TILE_DEATH)
		{
			m_Core.m_IsInFreeze = true;
			break;
		}
	}
	m_Core.m_IsInFreeze |= IsOnDeathTile();

	// look for save position for rescue feature
	// always update auto rescue
	TrySetRescue(RESCUEMODE_AUTO);

	m_Core.m_Id = GetPlayer()->GetCid();
}

void CCharacterDDRace::DDRacePostCoreTick()
{
	m_Time = (float)(Server()->Tick() - m_StartTime) / ((float)Server()->TickSpeed());

	if(m_Core.m_EndlessHook || (m_Core.m_Super && g_Config.m_SvEndlessSuperHook))
		m_Core.m_HookTick = 0;

	m_FrozenLastTick = false;

	if(m_Core.m_DeepFrozen && !m_Core.m_Super && !m_Core.m_Invincible)
		Freeze();

	// following jump rules can be overridden by tiles, like Refill Jumps, Stopper and Wall Jump
	if(m_Core.m_Jumps == -1)
	{
		// The player has only one ground jump, so their feet are always dark
		m_Core.m_Jumped |= 2;
	}
	else if(m_Core.m_Jumps == 0)
	{
		// The player has no jumps at all, so their feet are always dark
		m_Core.m_Jumped |= 2;
	}
	else if(m_Core.m_Jumps == 1 && m_Core.m_Jumped > 0)
	{
		// If the player has only one jump, each jump is the last one
		m_Core.m_Jumped |= 2;
	}
	else if(m_Core.m_JumpedTotal < m_Core.m_Jumps - 1 && m_Core.m_Jumped > 1)
	{
		// The player has not yet used up all their jumps, so their feet remain light
		m_Core.m_Jumped = 1;
	}

	if((m_Core.m_Super || m_Core.m_EndlessJump) && m_Core.m_Jumped > 1)
	{
		// Super players and players with infinite jumps always have light feet
		m_Core.m_Jumped = 1;
	}

	int CurrentIndex = Collision()->GetMapIndex(m_Pos);
	HandleSkippableTiles(CurrentIndex);
	if(!m_Alive)
		return;

	// handle Anti-Skip tiles
	std::vector<int> vIndices = Collision()->GetMapIndices(m_PrevPos, m_Pos);
	if(!vIndices.empty())
	{
		for(int &Index : vIndices)
		{
			HandleTiles(Index);
			if(!m_Alive)
				return;
		}
	}
	else
	{
		HandleTiles(CurrentIndex);
		if(!m_Alive)
			return;
	}

	// teleport gun
	if(m_TeleGunTeleport)
	{
		GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCid(), TeamMask());
		m_Core.m_Pos = m_TeleGunPos;
		if(!m_IsBlueTeleGunTeleport)
			m_Core.m_Vel = vec2(0, 0);
		GameServer()->CreateDeath(m_TeleGunPos, m_pPlayer->GetCid(), TeamMask());
		GameServer()->CreateSound(m_TeleGunPos, SOUND_WEAPON_SPAWN, TeamMask());
		m_TeleGunTeleport = false;
		m_IsBlueTeleGunTeleport = false;
	}

	HandleBroadcast();
}

void CCharacterDDRace::DDRaceInit()
{
	m_DDRaceState = ERaceState::NONE;
	m_StartTime = 0;
	for(bool &Set : m_SetSavePos)
		Set = false;
	m_LastBroadcast = 0;
	m_LastRefillJumps = false;
	m_LastBonus = false;
	m_TeamBeforeSuper = 0;
	m_TeleCheckpoint = 0;
	m_Core.m_EndlessHook = g_Config.m_SvEndlessDrag;
	if(g_Config.m_SvHit)
	{
		m_Core.m_HammerHitDisabled = false;
		m_Core.m_ShotgunHitDisabled = false;
		m_Core.m_GrenadeHitDisabled = false;
		m_Core.m_LaserHitDisabled = false;
	}
	else
	{
		m_Core.m_HammerHitDisabled = true;
		m_Core.m_ShotgunHitDisabled = true;
		m_Core.m_GrenadeHitDisabled = true;
		m_Core.m_LaserHitDisabled = true;
	}
	int Team = TeamsCore()->Team(m_Core.m_Id);

	if(RaceTeams()->TeamLocked(Team) && !RaceTeams()->TeamFlock(Team))
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(TeamsCore()->Team(i) == Team && i != m_Core.m_Id && GameServer()->m_apPlayers[i])
			{
				CCharacterDDRace *pChar = static_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[i]->GetCharacter());

				if(pChar)
				{
					m_DDRaceState = pChar->m_DDRaceState;
					m_StartTime = pChar->m_StartTime;
				}
			}
		}
	}

	if(g_Config.m_SvTeam == SV_TEAM_MANDATORY && Team == TEAM_FLOCK)
	{
		SendStartWarning("Please join a team before you start");
	}

	SetTuningZone(GameServer()->GameHost().Controller()->TuningZoneAt(m_Pos));
	m_TuneZoneOld = -1; // no zone leave msg on spawn
	SendZoneMsgs(); // we want an enter message also on spawn
	GameServer()->SendTuningParams(m_pPlayer->GetCid(), TuningZone());
	TrySetRescue(RESCUEMODE_MANUAL);
	Server()->StartRecord(m_pPlayer->GetCid());
}
