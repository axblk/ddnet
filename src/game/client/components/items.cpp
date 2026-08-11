/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "items.h"

#include <base/dbg.h>

#include <engine/demo.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/components/effects.h>
#include <game/client/gameclient.h>
#include <game/client/laser_data.h>
#include <game/client/pickup_data.h>
#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/pickup.h>
#include <game/client/prediction/entities/projectile.h>
#include <game/client/projectile_data.h>
#include <game/mapitems.h>

bool CItems::GetProjectileRenderInfo(const CGameState &State, const CGameTickInfo &Time, bool PredictProjectiles, bool IsOtherTeam, const CProjectileData *pCurrent, std::span<const CVisibleWorldRect> vVisibleWorldRects, vec2 VisibilityMargin, int &CurWeapon, vec2 &Pos, vec2 &Vel, float &Alpha) const
{
	const CGameState::CSceneClockState &SceneClock = State.SceneClock();
	CurWeapon = std::clamp(pCurrent->m_Type, 0, NUM_WEAPONS - 1);

	// get positions
	float Curvature = 0;
	float Speed = 0;
	const CTuningParams *pTuning = &State.Tuning(pCurrent->m_TuneZone);
	if(CurWeapon == WEAPON_GRENADE)
	{
		Curvature = pTuning->m_GrenadeCurvature;
		Speed = pTuning->m_GrenadeSpeed;
	}
	else if(CurWeapon == WEAPON_SHOTGUN)
	{
		Curvature = pTuning->m_ShotgunCurvature;
		Speed = pTuning->m_ShotgunSpeed;
	}
	else if(CurWeapon == WEAPON_GUN)
	{
		Curvature = pTuning->m_GunCurvature;
		Speed = pTuning->m_GunSpeed;
	}

	const int LocalClientId = State.LocalClientId();
	const bool LocalPlayerInGame = in_range(LocalClientId, MAX_CLIENTS - 1) && State.Client(LocalClientId).m_HasPlayerInfo && State.Client(LocalClientId).m_PlayerInfo.m_Team != TEAM_SPECTATORS;

	float Ct;
	if(PredictProjectiles && LocalPlayerInGame && !IsOtherTeam)
		Ct = ((float)(Time.m_PredictionTick - 1 - pCurrent->m_StartTick) + Time.m_PredIntraGameTick) / (float)Time.m_GameTickSpeed;
	else
		Ct = (Time.m_PrevGameTick - pCurrent->m_StartTick) / (float)Time.m_GameTickSpeed + SceneClock.m_GameTickTime;
	if(Ct < 0)
	{
		if(Ct > -SceneClock.m_GameTickTime / 2)
		{
			// Fixup the timing which might be screwed during demo playback because
			// SceneClock.m_GameTickTime depends on the system timer, while the other part
			// (Time.m_PrevGameTick - pCurrent->m_StartTick) / (float)Time.m_GameTickSpeed
			// is virtually constant (for projectiles fired on the current game tick):
			// (x - (x+2)) / 50 = -0.04
			//
			// We have a strict comparison for the passed time being more than the time between ticks
			// if(CurtickStart > m_Info.m_CurrentTime) in CDemoPlayer::Update()
			// so in practice the typical preserved game tick time varies from 0.02386 to 0.03999
			// which leads to Ct from -0.00001 to -0.01614.
			// Round up those to 0.0 to fix missing rendering of the projectile.
			Ct = 0;
		}
		else
		{
			return false; // projectile haven't been shot yet
		}
	}

	Pos = CalcPos(pCurrent->m_StartPos, pCurrent->m_StartVel, Curvature, Speed, Ct);
	bool Visible = false;
	for(const CVisibleWorldRect &VisibleWorldRect : vVisibleWorldRects)
		Visible |= VisibleWorldRect.Inside(Pos, VisibilityMargin);
	if(!Visible)
		return false;
	vec2 PrevPos = CalcPos(pCurrent->m_StartPos, pCurrent->m_StartVel, Curvature, Speed, Ct - 0.001f);

	Alpha = 1.f;
	if(IsOtherTeam)
	{
		Alpha = g_Config.m_ClShowOthersAlpha / 100.0f;
	}

	Vel = Pos - PrevPos;
	return true;
}

void CItems::RenderProjectile(const CRenderContext &Context, const CProjectileData *pCurrent, int ItemId, const CScreenRect &ScreenRect)
{
	int CurWeapon;
	vec2 Pos;
	vec2 Vel;
	float Alpha;
	const bool IsOtherTeam = pCurrent->m_ExtraInfo && pCurrent->m_Owner >= 0 && Context.IsOtherTeam(pCurrent->m_Owner);
	const CVisibleWorldRect VisibleWorldRect(ScreenRect.m_TopLeft, ScreenRect.m_BottomRight);
	if(!GetProjectileRenderInfo(Context.m_State, Context.m_Time, GameClient()->Predict() && GameClient()->AntiPingGrenade(), IsOtherTeam, pCurrent, std::span<const CVisibleWorldRect>(&VisibleWorldRect, 1), vec2(0.0f, 0.0f), CurWeapon, Pos, Vel, Alpha))
		return;

	if(CurWeapon == WEAPON_GRENADE)
	{
		Graphics()->QuadsSetRotation(Context.m_State.SceneClock().m_AnimationTime * pi * 2 * 2 + ItemId);
	}
	else
	{
		if(length(Vel) > 0.00001f)
			Graphics()->QuadsSetRotation(angle(Vel));
		else
			Graphics()->QuadsSetRotation(0);
	}

	if(GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon].IsValid())
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon]);
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_aProjectileOffset[CurWeapon], Pos.x, Pos.y);
	}
}

vec2 CItems::GetPickupPosition(const CGameTickInfo &Time, const CNetObj_Pickup *pPrev, const CNetObj_Pickup *pCurrent, bool IsPredicted) const
{
	const float IntraTick = IsPredicted ? Time.m_PredIntraGameTick : Time.m_IntraGameTick;
	return mix(vec2(pPrev->m_X, pPrev->m_Y), vec2(pCurrent->m_X, pCurrent->m_Y), IntraTick);
}

void CItems::RenderPickup(const CRenderContext &Context, const CNetObj_Pickup *pPrev, const CNetObj_Pickup *pCurrent, bool IsPredicted, int Flags)
{
	const CGameState::CSceneClockState &SceneClock = Context.m_State.SceneClock();
	int CurWeapon = std::clamp(pCurrent->m_Subtype, 0, NUM_WEAPONS - 1);
	int QuadOffset = 2;
	vec2 Pos = GetPickupPosition(Context.m_Time, pPrev, pCurrent, IsPredicted);
	if(pCurrent->m_Type == POWERUP_HEALTH)
	{
		QuadOffset = m_PickupHealthOffset;
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpritePickupHealth);
	}
	else if(pCurrent->m_Type == POWERUP_ARMOR)
	{
		QuadOffset = m_PickupArmorOffset;
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpritePickupArmor);
	}
	else if(pCurrent->m_Type == POWERUP_WEAPON)
	{
		QuadOffset = m_aPickupWeaponOffset[CurWeapon];
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpritePickupWeapons[CurWeapon]);
	}
	else if(pCurrent->m_Type == POWERUP_NINJA)
	{
		QuadOffset = m_PickupNinjaOffset;
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpritePickupNinja);
	}
	else if(pCurrent->m_Type >= POWERUP_ARMOR_SHOTGUN && pCurrent->m_Type <= POWERUP_ARMOR_LASER)
	{
		QuadOffset = m_aPickupWeaponArmorOffset[pCurrent->m_Type - POWERUP_ARMOR_SHOTGUN];
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpritePickupWeaponArmor[pCurrent->m_Type - POWERUP_ARMOR_SHOTGUN]);
	}
	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	vec2 Scale = vec2(1, 1);
	if(Flags & PICKUPFLAG_XFLIP)
		Scale.x = -Scale.x;

	if(Flags & PICKUPFLAG_YFLIP)
		Scale.y = -Scale.y;

	if(Flags & PICKUPFLAG_ROTATE)
	{
		Graphics()->QuadsSetRotation(90.f * (pi / 180));
		std::swap(Scale.x, Scale.y);

		if(pCurrent->m_Type == POWERUP_NINJA)
		{
			if(Flags & PICKUPFLAG_XFLIP)
				Pos.y += 10.0f;
			else
				Pos.y -= 10.0f;
		}
	}
	else
	{
		if(pCurrent->m_Type == POWERUP_NINJA)
		{
			if(Flags & PICKUPFLAG_XFLIP)
				Pos.x += 10.0f;
			else
				Pos.x -= 10.0f;
		}
	}

	float Offset = Pos.y / 32.0f + Pos.x / 32.0f;
	Pos += direction(SceneClock.m_AnimationTime * 2.0f + Offset) * 2.5f;

	Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, QuadOffset, Pos.x, Pos.y, Scale.x, Scale.y);
	Graphics()->QuadsSetRotation(0);
}

void CItems::RenderFlags(const CRenderContext &Context)
{
	for(int Flag = 0; Flag < GameClient()->m_Snap.m_NumFlags; ++Flag)
	{
		RenderFlag(Context, GameClient()->m_Snap.m_apPrevFlags[Flag], GameClient()->m_Snap.m_apFlags[Flag],
			GameClient()->m_Snap.m_pPrevGameDataObj, GameClient()->m_Snap.m_pGameDataObj);
	}
}

void CItems::RenderFlag(const CRenderContext &Context, const CNetObj_Flag *pPrev, const CNetObj_Flag *pCurrent, const CNetObj_GameData *pPrevGameData, const CNetObj_GameData *pCurGameData)
{
	vec2 Pos = mix(vec2(pPrev->m_X, pPrev->m_Y), vec2(pCurrent->m_X, pCurrent->m_Y), Context.m_Time.m_IntraGameTick);
	if(pCurGameData)
	{
		int FlagCarrier = (pCurrent->m_Team == TEAM_RED) ? pCurGameData->m_FlagCarrierRed : pCurGameData->m_FlagCarrierBlue;
		// use the flagcarriers position if available
		if(FlagCarrier >= 0 && Context.m_State.RenderedClient(FlagCarrier).m_Active)
			Pos = Context.m_State.RenderedClient(FlagCarrier).m_Position;

		// make sure that the flag isn't interpolated between capture and return
		if(pPrevGameData &&
			((pCurrent->m_Team == TEAM_RED && pPrevGameData->m_FlagCarrierRed != pCurGameData->m_FlagCarrierRed) ||
				(pCurrent->m_Team == TEAM_BLUE && pPrevGameData->m_FlagCarrierBlue != pCurGameData->m_FlagCarrierBlue)))
			Pos = vec2(pCurrent->m_X, pCurrent->m_Y);
	}

	float Size = 42.0f;
	int QuadOffset;
	if(pCurrent->m_Team == TEAM_RED)
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagRed);
		QuadOffset = m_RedFlagOffset;
	}
	else
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagBlue);
		QuadOffset = m_BlueFlagOffset;
	}
	Graphics()->QuadsSetRotation(0.0f);
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, QuadOffset, Pos.x, Pos.y - Size * 0.75f);
}

void CItems::RenderLaser(const CLaserData *pCurrent, bool IsPredicted)
{
	int Type = std::clamp(pCurrent->m_Type, -1, NUM_LASERTYPES - 1);
	int ColorIn, ColorOut;
	switch(Type)
	{
	case LASERTYPE_RIFLE:
		ColorOut = g_Config.m_ClLaserRifleOutlineColor;
		ColorIn = g_Config.m_ClLaserRifleInnerColor;
		break;
	case LASERTYPE_SHOTGUN:
		ColorOut = g_Config.m_ClLaserShotgunOutlineColor;
		ColorIn = g_Config.m_ClLaserShotgunInnerColor;
		break;
	case LASERTYPE_DOOR:
		ColorOut = g_Config.m_ClLaserDoorOutlineColor;
		ColorIn = g_Config.m_ClLaserDoorInnerColor;
		break;
	case LASERTYPE_FREEZE:
		ColorOut = g_Config.m_ClLaserFreezeOutlineColor;
		ColorIn = g_Config.m_ClLaserFreezeInnerColor;
		break;
	case LASERTYPE_DRAGGER:
		ColorOut = g_Config.m_ClLaserDraggerOutlineColor;
		ColorIn = g_Config.m_ClLaserDraggerInnerColor;
		break;
	case LASERTYPE_GUN:
	case LASERTYPE_PLASMA:
		if(pCurrent->m_Subtype == LASERGUNTYPE_FREEZE || pCurrent->m_Subtype == LASERGUNTYPE_EXPFREEZE)
		{
			ColorOut = g_Config.m_ClLaserFreezeOutlineColor;
			ColorIn = g_Config.m_ClLaserFreezeInnerColor;
		}
		else
		{
			ColorOut = g_Config.m_ClLaserRifleOutlineColor;
			ColorIn = g_Config.m_ClLaserRifleInnerColor;
		}
		break;
	default:
		ColorOut = g_Config.m_ClLaserRifleOutlineColor;
		ColorIn = g_Config.m_ClLaserRifleInnerColor;
	}

	bool IsOtherTeam = (pCurrent->m_ExtraInfo && pCurrent->m_Owner >= 0 && GameClient()->IsOtherTeam(pCurrent->m_Owner));

	float Alpha = IsOtherTeam ? g_Config.m_ClShowOthersAlpha / 100.0f : 1.f;

	const ColorRGBA OuterColor = color_cast<ColorRGBA>(ColorHSLA(ColorOut).WithAlpha(Alpha));
	const ColorRGBA InnerColor = color_cast<ColorRGBA>(ColorHSLA(ColorIn).WithAlpha(Alpha));

	float Ticks;
	float TicksHead = Client()->GameTick(GameClient()->ActiveConnection());
	if(Type == LASERTYPE_DOOR)
	{
		Ticks = 1.0f;
	}
	else if(IsPredicted)
	{
		int PredictionTick = Client()->GetPredictionTick();
		Ticks = (float)(PredictionTick - pCurrent->m_StartTick) + Client()->PredIntraGameTick(GameClient()->ActiveConnection());
		TicksHead += Client()->PredIntraGameTick(GameClient()->ActiveConnection());
	}
	else
	{
		Ticks = (float)(Client()->GameTick(GameClient()->ActiveConnection()) - pCurrent->m_StartTick) + Client()->IntraGameTick(GameClient()->ActiveConnection());
		TicksHead += Client()->IntraGameTick(GameClient()->ActiveConnection());
	}

	if(Type == LASERTYPE_DRAGGER)
	{
		TicksHead *= (((pCurrent->m_Subtype >> 1) % 3) * 4.0f) + 1;
		TicksHead *= (pCurrent->m_Subtype & 1) ? -1 : 1;
	}
	RenderLaser(pCurrent->m_From, pCurrent->m_To, OuterColor, InnerColor, Ticks, TicksHead, Type);
}

void CItems::RenderLaser(vec2 From, vec2 Pos, ColorRGBA OuterColor, ColorRGBA InnerColor, float TicksBody, float TicksHead, int Type) const
{
	float Len = distance(Pos, From);

	if(Len > 0)
	{
		if(Type == LASERTYPE_DRAGGER)
		{
			// rubber band effect
			float Thickness = std::sqrt(Len) / 5.f;
			TicksBody = std::clamp(Thickness, 1.0f, 5.0f);
		}
		vec2 Dir = normalize_pre_length(Pos - From, Len);

		float Ms = TicksBody * 1000.0f / Client()->GameTickSpeed();
		float a;
		if(Type == LASERTYPE_RIFLE || Type == LASERTYPE_SHOTGUN)
		{
			int TuneZone = (Client()->State() == IClient::STATE_ONLINE && GameClient()->m_GameWorld.m_WorldConfig.m_UseTuneZones) ? Collision()->IsTune(Collision()->GetMapIndex(From)) : 0;
			a = Ms / GameClient()->GetTuning(TuneZone)->m_LaserBounceDelay;
		}
		else
		{
			a = Ms / CTuningParams::DEFAULT.m_LaserBounceDelay;
		}
		a = std::clamp(a, 0.0f, 1.0f);
		float Ia = 1 - a;

		Graphics()->TextureClear();
		Graphics()->QuadsBegin();

		// do outline
		Graphics()->SetColor(OuterColor);
		vec2 Out = vec2(Dir.y, -Dir.x) * (7.0f * Ia);

		IGraphics::CFreeformItem Freeform(
			From - Out, From + Out,
			Pos - Out, Pos + Out);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);

		// do inner
		Out = vec2(Dir.y, -Dir.x) * (5.0f * Ia);
		vec2 ExtraOutlinePos = Dir;
		vec2 ExtraOutlineFrom = Type == LASERTYPE_DOOR ? vec2(0, 0) : Dir;
		Graphics()->SetColor(InnerColor); // center

		Freeform = IGraphics::CFreeformItem(
			From - Out + ExtraOutlineFrom, From + Out + ExtraOutlineFrom,
			Pos - Out - ExtraOutlinePos, Pos + Out - ExtraOutlinePos);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);

		Graphics()->QuadsEnd();
	}

	// render head
	if(Type == LASERTYPE_DOOR)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsSetRotation(0);
		Graphics()->SetColor(OuterColor);
		Graphics()->RenderQuadContainerEx(m_ItemsQuadContainerIndex, m_DoorHeadOffset, 1, Pos.x - 8.0f, Pos.y - 8.0f);
		Graphics()->SetColor(InnerColor);
		Graphics()->RenderQuadContainerEx(m_ItemsQuadContainerIndex, m_DoorHeadOffset, 1, Pos.x - 6.0f, Pos.y - 6.0f, 6.f / 8.f, 6.f / 8.f);
	}
	else if(Type == LASERTYPE_DRAGGER)
	{
		Graphics()->TextureSet(GameClient()->m_ExtrasSkin.m_SpritePulley);
		for(int Inner = 0; Inner < 2; ++Inner)
		{
			Graphics()->SetColor(Inner ? InnerColor : OuterColor);

			float Size = Inner ? 4.f / 5.f : 1.f;

			// circle at laser end
			if(Len > 0)
			{
				Graphics()->QuadsSetRotation(0);
				Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_PulleyHeadOffset, From.x, From.y, Size, Size);
			}

			//rotating orbs
			Size = Inner ? 0.75f - 1.f / 5.f : 0.75f;
			for(int Orb = 0; Orb < 3; ++Orb)
			{
				vec2 Offset(10.f, 0);
				Offset = rotate(Offset, Orb * 120 + TicksHead);
				Graphics()->QuadsSetRotation(TicksHead + Orb * pi * 2.f / 3.f); // rotate the sprite as well, as it might be customized
				Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_PulleyHeadOffset, From.x + Offset.x, From.y + Offset.y, Size, Size);
			}
		}
	}
	else if(Type == LASERTYPE_FREEZE)
	{
		float Pulsation = 6.f / 5.f + 1.f / 10.f * std::sin(TicksHead / 2.f);
		float Angle = angle(Pos - From);
		Graphics()->TextureSet(GameClient()->m_ExtrasSkin.m_SpriteHectagon);
		Graphics()->QuadsSetRotation(Angle);
		Graphics()->SetColor(OuterColor);
		Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_FreezeHeadOffset, Pos.x, Pos.y, 6.f / 5.f * Pulsation, 6.f / 5.f * Pulsation);
		Graphics()->TextureSet(GameClient()->m_ExtrasSkin.m_SpriteParticleSnowflake);
		// snowflakes are white
		Graphics()->SetColor(ColorRGBA(1.f, 1.f, 1.f));
		Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_FreezeHeadOffset, Pos.x, Pos.y, Pulsation, Pulsation);
	}
	else
	{
		int CurParticle = (int)TicksHead % 3;
		Graphics()->TextureSet(GameClient()->m_ParticlesSkin.m_aSpriteParticleSplat[CurParticle]);
		Graphics()->QuadsSetRotation((int)TicksHead);
		Graphics()->SetColor(OuterColor);
		Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_aParticleSplatOffset[CurParticle], Pos.x, Pos.y);
		Graphics()->SetColor(InnerColor);
		Graphics()->RenderQuadContainerAsSprite(m_ItemsQuadContainerIndex, m_aParticleSplatOffset[CurParticle], Pos.x, Pos.y, 20.f / 24.f, 20.f / 24.f);
	}
}

void CItems::UpdatePresentation(const CPresentationContext &Context)
{
	if(Context.m_Playback == EPresentationPlayback::PAUSED)
		return;

	CGameState &State = Context.m_State;
	const int LocalClientId = State.LocalClientId();
	const CNetObj_DDNetCharacter *pLocalExtended = State.ExtendedCharacter(LocalClientId);
	const bool IsSuper = pLocalExtended != nullptr && (pLocalExtended->m_Flags & CHARACTERFLAG_SUPER) != 0;
	const int Ticks = Context.m_Time.m_GameTick % Context.m_Time.m_GameTickSpeed;
	const bool BlinkingPickup = (Ticks % 22) < 4;
	const bool BlinkingProj = (Ticks % 20) < 2;
	const bool BlinkingProjEx = (Ticks % 6) < 2;
	const int SwitcherTeam = State.Runtime().m_SwitchStateTeam >= 0 ? State.Runtime().m_SwitchStateTeam : in_range(LocalClientId, MAX_CLIENTS - 1) ? State.Teams().Team(LocalClientId) :
																			 0;
	const bool UsePredicted = in_range(LocalClientId, MAX_CLIENTS - 1) && State.PredictedClient(LocalClientId).m_HasCurrent && g_Config.m_ClPredict && !Context.m_Time.m_IsDemoPlayback && g_Config.m_ClAntiPing && g_Config.m_ClAntiPingGrenade && g_Config.m_ClAntiPingWeapons && g_Config.m_ClAntiPingGunfire;
	CGameWorld &GameWorld = State.GameWorld();
	CGameWorld &PrevPredictedWorld = State.PrevPredictedWorld();
	const auto &aSwitchers = GameWorld.m_Core.m_vSwitchers;

	constexpr float TileSize = 64.0f;

	auto AddProjectileEffects = [&](const CProjectileData &Data) {
		int CurWeapon;
		vec2 Pos;
		vec2 Vel;
		float Alpha;
		const bool IsOtherTeam = Data.m_ExtraInfo && Data.m_Owner >= 0 && Context.IsOtherTeamFromLocalPlayer(Data.m_Owner);
		const int OwnerClientId = Data.m_ExtraInfo ? Data.m_Owner : -1;
		if(!GetProjectileRenderInfo(State, Context.m_Time, UsePredicted, IsOtherTeam, &Data, Context.m_vVisibleWorldRects, vec2(TileSize, TileSize), CurWeapon, Pos, Vel, Alpha))
			return;

		// Don't check the projectile type for validity here, to keep effects compatible with mods.
		if(CurWeapon == WEAPON_GRENADE)
			GameClient()->m_Effects.SmokeTrail(State, Pos, Vel * -1, OwnerClientId, 1.0f, 0.0f);
		else
			GameClient()->m_Effects.BulletTrail(State, Pos, OwnerClientId, 1.0f, 0.0f);
	};

	auto AddPickupEffects = [&](const CNetObj_Pickup *pPrev, const CNetObj_Pickup *pCurrent, bool IsPredicted, int Flags) {
		if(pCurrent->m_Type != POWERUP_NINJA)
			return;
		const vec2 Pos = GetPickupPosition(Context.m_Time, pPrev, pCurrent, IsPredicted);
		if(!Context.IsVisible(Pos, vec2(1.75f * TileSize, 0.75f * TileSize)))
			return;
		GameClient()->m_Effects.PowerupShine(State, Pos, Flags & PICKUPFLAG_ROTATE ? vec2(18, 96) : vec2(96, 18), -1, 1.0f);
	};

	if(UsePredicted)
	{
		for(auto *pProj = (CProjectile *)PrevPredictedWorld.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pProj; pProj = (CProjectile *)pProj->NextEntity())
		{
			if(!IsSuper && pProj->m_Number > 0 && pProj->m_Number < (int)aSwitchers.size() && !aSwitchers[pProj->m_Number].m_aStatus[SwitcherTeam] && (pProj->m_Explosive ? BlinkingProjEx : BlinkingProj))
				continue;
			AddProjectileEffects(pProj->GetData());
		}

		for(auto *pPickup = (CPickup *)PrevPredictedWorld.FindFirst(CGameWorld::ENTTYPE_PICKUP); pPickup; pPickup = (CPickup *)pPickup->NextEntity())
		{
			if(!IsSuper && pPickup->m_Layer == LAYER_SWITCH && pPickup->m_Number > 0 && pPickup->m_Number < (int)aSwitchers.size() && !aSwitchers[pPickup->m_Number].m_aStatus[SwitcherTeam] && BlinkingPickup)
				continue;
			if(!pPickup->InDDNetTile())
				continue;
			if(auto *pPrev = (CPickup *)PrevPredictedWorld.GetEntity(pPickup->GetId(), CGameWorld::ENTTYPE_PICKUP))
			{
				CNetObj_Pickup Data, Prev;
				pPickup->FillInfo(&Data);
				pPrev->FillInfo(&Prev);
				AddPickupEffects(&Prev, &Data, true, pPickup->Flags());
			}
		}
	}

	for(const CGameState::CEntitySnapshot &Entity : State.Entities())
	{
		const void *pData = Entity.m_vData.data();
		const CNetObj_EntityEx *pEntEx = Entity.m_HasEntityEx ? &Entity.m_EntityEx : nullptr;

		if(Entity.m_Type == NETOBJTYPE_PROJECTILE || Entity.m_Type == NETOBJTYPE_DDRACEPROJECTILE || Entity.m_Type == NETOBJTYPE_DDNETPROJECTILE)
		{
			CProjectileData Data = ExtractProjectileInfo(Entity.m_Type, pData, &GameWorld, pEntEx);
			const bool Inactive = !IsSuper && Data.m_SwitchNumber > 0 && Data.m_SwitchNumber < (int)aSwitchers.size() && !aSwitchers[Data.m_SwitchNumber].m_aStatus[SwitcherTeam];
			if(Inactive && (Data.m_Explosive ? BlinkingProjEx : BlinkingProj))
				continue;

			if(UsePredicted)
			{
				if(auto *pProj = (CProjectile *)GameWorld.FindMatch(Entity.m_Id, Entity.m_Type, pData))
				{
					const bool IsOtherTeam = Context.IsOtherTeamFromLocalPlayer(pProj->GetOwner());
					if(pProj->m_LastPresentationTick <= 0 && (pProj->m_Type != WEAPON_SHOTGUN || (!pProj->m_Freeze && !pProj->m_Explosive)) // skip ddrace shotgun bullets
						&& (pProj->m_Type == WEAPON_SHOTGUN || absolute(length(pProj->m_Direction) - 1.f) < 0.02f) // workaround to skip grenades on ball mod
						&& (pProj->GetOwner() < 0 || pProj->GetOwner() != LocalClientId || IsOtherTeam) // skip locally predicted projectiles
						&& Entity.m_vPrevData.empty())
					{
						ReconstructSmokeTrail(State, Context.m_Time, &Data, pProj->m_DestroyTick);
					}
					pProj->m_LastPresentationTick = Context.m_Time.m_GameTick;
					if(!IsOtherTeam)
						continue;
				}
			}
			AddProjectileEffects(Data);
		}
		else if(Entity.m_Type == NETOBJTYPE_PICKUP || Entity.m_Type == NETOBJTYPE_DDNETPICKUP)
		{
			const CPickupData Data = ExtractPickupInfo(Entity.m_Type, pData, pEntEx);
			if(!Context.IsVisible(Data.m_Pos, vec2(1.75f * TileSize, 0.75f * TileSize)))
				continue;
			const bool Inactive = !IsSuper && Data.m_SwitchNumber > 0 && Data.m_SwitchNumber < (int)aSwitchers.size() && !aSwitchers[Data.m_SwitchNumber].m_aStatus[SwitcherTeam];
			if(Inactive && BlinkingPickup)
				continue;
			if(UsePredicted)
			{
				auto *pPickup = (CPickup *)GameWorld.FindMatch(Entity.m_Id, Entity.m_Type, pData);
				if(pPickup && pPickup->InDDNetTile())
					continue;
			}
			if(!Entity.m_vPrevData.empty())
				AddPickupEffects((const CNetObj_Pickup *)Entity.m_vPrevData.data(), (const CNetObj_Pickup *)pData, false, Data.m_Flags);
		}
	}
}

void CItems::OnRender(const CRenderContext &Context)
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	bool IsSuper = GameClient()->IsLocalCharSuper();
	int Ticks = Client()->GameTick(GameClient()->ActiveConnection()) % Client()->GameTickSpeed();
	bool BlinkingPickup = (Ticks % 22) < 4;
	bool BlinkingGun = (Ticks % 22) < 4;
	bool BlinkingDragger = (Ticks % 22) < 4;
	bool BlinkingProj = (Ticks % 20) < 2;
	bool BlinkingProjEx = (Ticks % 6) < 2;
	bool BlinkingLight = (Ticks % 6) < 2;
	int SwitcherTeam = GameClient()->SwitchStateTeam();
	int DraggerStartTick = std::max((Client()->GameTick(GameClient()->ActiveConnection()) / 7) * 7, Client()->GameTick(GameClient()->ActiveConnection()) - 4);
	int GunStartTick = (Client()->GameTick(GameClient()->ActiveConnection()) / 7) * 7;

	bool UsePredicted = GameClient()->Predict() && GameClient()->AntiPingGunfire();
	auto &aSwitchers = GameClient()->Switchers();

	CScreenRect ScreenRectLaser = Graphics()->GetScreen();
	CScreenRect ScreenRectProjectile = ScreenRectLaser;
	CScreenRect ScreenRectPickup = ScreenRectLaser;

	constexpr float TileSize = 64.0f;
	ScreenRectProjectile.Expand(TileSize);
	ScreenRectLaser.Expand(TileSize / 2.0f);
	ScreenRectPickup.Expand(1.75f * TileSize, 0.75f * TileSize);

	auto IsLaserInside = [&](const CLaserData &LaserData) -> bool {
		const vec2 &From = LaserData.m_From;
		const vec2 &To = LaserData.m_To;
		return !((From.x < ScreenRectLaser.m_TopLeft.x && To.x < ScreenRectLaser.m_TopLeft.x) || (From.x > ScreenRectLaser.m_BottomRight.x && To.x > ScreenRectLaser.m_BottomRight.x) ||
			 (From.y < ScreenRectLaser.m_TopLeft.y && To.y < ScreenRectLaser.m_TopLeft.y) || (From.y > ScreenRectLaser.m_BottomRight.y && To.y > ScreenRectLaser.m_BottomRight.y));
	};

	if(UsePredicted)
	{
		for(auto *pProj = (CProjectile *)GameClient()->m_PrevPredictedWorld.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pProj; pProj = (CProjectile *)pProj->NextEntity())
		{
			if(!IsSuper && pProj->m_Number > 0 && pProj->m_Number < (int)aSwitchers.size() && !aSwitchers[pProj->m_Number].m_aStatus[SwitcherTeam] && (pProj->m_Explosive ? BlinkingProjEx : BlinkingProj))
				continue;

			CProjectileData Data = pProj->GetData();
			RenderProjectile(Context, &Data, pProj->GetId(), ScreenRectProjectile);
		}
		for(CEntity *pEnt = GameClient()->m_PrevPredictedWorld.FindFirst(CGameWorld::ENTTYPE_LASER); pEnt; pEnt = pEnt->NextEntity())
		{
			auto *const pLaser = dynamic_cast<CLaser *>(pEnt);
			if(!pLaser || pLaser->GetOwner() < 0 || !Context.m_State.RenderedClient(pLaser->GetOwner()).m_IsPredictedLocal)
				continue;
			CLaserData Data = pLaser->GetData();
			if(!IsLaserInside(Data))
				continue;
			RenderLaser(&Data, true);
		}
		for(auto *pPickup = (CPickup *)GameClient()->m_PrevPredictedWorld.FindFirst(CGameWorld::ENTTYPE_PICKUP); pPickup; pPickup = (CPickup *)pPickup->NextEntity())
		{
			if(!IsSuper && pPickup->m_Layer == LAYER_SWITCH && pPickup->m_Number > 0 && pPickup->m_Number < (int)aSwitchers.size() && !aSwitchers[pPickup->m_Number].m_aStatus[SwitcherTeam] && BlinkingPickup)
				continue;

			if(pPickup->InDDNetTile())
			{
				if(auto *pPrev = (CPickup *)GameClient()->m_PrevPredictedWorld.GetEntity(pPickup->GetId(), CGameWorld::ENTTYPE_PICKUP))
				{
					CNetObj_Pickup Data, Prev;
					pPickup->FillInfo(&Data);
					pPrev->FillInfo(&Prev);
					RenderPickup(Context, &Prev, &Data, true, pPickup->Flags());
				}
			}
		}
	}

	for(const CSnapEntities &Ent : GameClient()->SnapEntities())
	{
		const IClient::CSnapItem Item = Ent.m_Item;
		const void *pData = Item.m_pData;
		const CNetObj_EntityEx *pEntEx = Ent.m_pDataEx;

		if(Item.m_Type == NETOBJTYPE_PROJECTILE || Item.m_Type == NETOBJTYPE_DDRACEPROJECTILE || Item.m_Type == NETOBJTYPE_DDNETPROJECTILE)
		{
			CProjectileData Data = ExtractProjectileInfo(Item.m_Type, pData, &GameClient()->m_GameWorld, pEntEx);
			bool Inactive = !IsSuper && Data.m_SwitchNumber > 0 && Data.m_SwitchNumber < (int)aSwitchers.size() && !aSwitchers[Data.m_SwitchNumber].m_aStatus[SwitcherTeam];
			if(Inactive && (Data.m_Explosive ? BlinkingProjEx : BlinkingProj))
				continue;
			if(UsePredicted)

			{
				if(auto *pProj = (CProjectile *)GameClient()->m_GameWorld.FindMatch(Item.m_Id, Item.m_Type, pData))
				{
					bool IsOtherTeam = Context.IsOtherTeam(pProj->GetOwner());
					if(!IsOtherTeam)
						continue;
				}
			}
			RenderProjectile(Context, &Data, Item.m_Id, ScreenRectProjectile);
		}
		else if(Item.m_Type == NETOBJTYPE_PICKUP || Item.m_Type == NETOBJTYPE_DDNETPICKUP)
		{
			CPickupData Data = ExtractPickupInfo(Item.m_Type, pData, pEntEx);
			if(!ScreenRectPickup.Inside(Data.m_Pos))
				continue;
			bool Inactive = !IsSuper && Data.m_SwitchNumber > 0 && Data.m_SwitchNumber < (int)aSwitchers.size() && !aSwitchers[Data.m_SwitchNumber].m_aStatus[SwitcherTeam];

			if(Inactive && BlinkingPickup)
				continue;
			if(UsePredicted)
			{
				auto *pPickup = (CPickup *)GameClient()->m_GameWorld.FindMatch(Item.m_Id, Item.m_Type, pData);
				if(pPickup && pPickup->InDDNetTile())
					continue;
			}
			const void *pPrev = Client()->SnapFindItem(Client()->ActiveConnection(), IClient::SNAP_PREV, Item.m_Type, Item.m_Id);
			if(pPrev)
				RenderPickup(Context, (const CNetObj_Pickup *)pPrev, (const CNetObj_Pickup *)pData, false, Data.m_Flags);
		}
		else if(Item.m_Type == NETOBJTYPE_LASER || Item.m_Type == NETOBJTYPE_DDNETLASER)
		{
			if(UsePredicted)
			{
				auto *pLaser = dynamic_cast<CLaser *>(GameClient()->m_GameWorld.FindMatch(Item.m_Id, Item.m_Type, pData));
				if(pLaser && pLaser->GetOwner() >= 0 && Context.m_State.RenderedClient(pLaser->GetOwner()).m_IsPredictedLocal)
					continue;
			}

			CLaserData Data = ExtractLaserInfo(Item.m_Type, pData, &GameClient()->m_GameWorld, pEntEx);
			if(!IsLaserInside(Data))
				continue;
			bool Inactive = !IsSuper && Data.m_SwitchNumber > 0 && Data.m_SwitchNumber < (int)aSwitchers.size() && !aSwitchers[Data.m_SwitchNumber].m_aStatus[SwitcherTeam];

			bool IsEntBlink = false;
			int EntStartTick = -1;
			if(Data.m_Type == LASERTYPE_FREEZE)
			{
				IsEntBlink = BlinkingLight;
				EntStartTick = DraggerStartTick;
			}
			else if(Data.m_Type == LASERTYPE_GUN)
			{
				IsEntBlink = BlinkingGun;
				EntStartTick = GunStartTick;
			}
			else if(Data.m_Type == LASERTYPE_DRAGGER)
			{
				IsEntBlink = BlinkingDragger;
				EntStartTick = DraggerStartTick;
			}
			else if(Data.m_Type == LASERTYPE_DOOR)
			{
				if(Data.m_Predict && (Inactive || IsSuper))
				{
					Data.m_From.x = Data.m_To.x;
					Data.m_From.y = Data.m_To.y;
				}
				EntStartTick = Client()->GameTick(GameClient()->ActiveConnection());
			}
			else
			{
				IsEntBlink = BlinkingDragger;
				EntStartTick = Client()->GameTick(GameClient()->ActiveConnection());
			}

			if(Data.m_Predict && Inactive && IsEntBlink)
			{
				continue;
			}

			if(Data.m_StartTick <= 0 && EntStartTick != -1)
			{
				Data.m_StartTick = EntStartTick;
			}

			RenderLaser(&Data);
		}
	}

	RenderFlags(Context);

	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
}

void CItems::OnInit()
{
	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	m_ItemsQuadContainerIndex = Graphics()->CreateQuadContainer(false);

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_RedFlagOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, -21.f, -42.f, 42.f, 84.f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_BlueFlagOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, -21.f, -42.f, 42.f, 84.f);

	float ScaleX, ScaleY;
	Graphics()->GetSpriteScale(SPRITE_PICKUP_HEALTH, ScaleX, ScaleY);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_PickupHealthOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 64.f * ScaleX, 64.f * ScaleY);
	Graphics()->GetSpriteScale(SPRITE_PICKUP_ARMOR, ScaleX, ScaleY);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_PickupArmorOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 64.f * ScaleX, 64.f * ScaleY);

	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_pSpriteBody, ScaleX, ScaleY);
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		m_aPickupWeaponOffset[i] = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleX, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleY);
	}
	Graphics()->GetSpriteScale(SPRITE_PICKUP_NINJA, ScaleX, ScaleY);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_PickupNinjaOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 128.f * ScaleX, 128.f * ScaleY);

	for(int i = 0; i < 4; i++)
	{
		Graphics()->GetSpriteScale(SPRITE_PICKUP_ARMOR_SHOTGUN + i, ScaleX, ScaleY);
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		m_aPickupWeaponArmorOffset[i] = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 64.f * ScaleX, 64.f * ScaleY);
	}

	for(int &ProjectileOffset : m_aProjectileOffset)
	{
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		ProjectileOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 32.f);
	}

	for(int &ParticleSplatOffset : m_aParticleSplatOffset)
	{
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		ParticleSplatOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 24.f);
	}

	Graphics()->GetSpriteScale(SPRITE_PART_PULLEY, ScaleX, ScaleY);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_PulleyHeadOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 20.f * ScaleX);

	Graphics()->GetSpriteScale(SPRITE_PART_HECTAGON, ScaleX, ScaleY);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_FreezeHeadOffset = Graphics()->QuadContainerAddSprite(m_ItemsQuadContainerIndex, 20.f * ScaleX);

	IGraphics::CQuadItem Brick(0, 0, 16.0f, 16.0f);
	m_DoorHeadOffset = Graphics()->QuadContainerAddQuads(m_ItemsQuadContainerIndex, &Brick, 1);

	Graphics()->QuadContainerUpload(m_ItemsQuadContainerIndex);
}

void CItems::ReconstructSmokeTrail(CGameState &State, const CGameTickInfo &Time, const CProjectileData *pCurrent, int DestroyTick)
{
	const int LocalClientId = State.LocalClientId();
	const bool LocalPlayerInGame = in_range(LocalClientId, MAX_CLIENTS - 1) && State.Client(LocalClientId).m_HasPlayerInfo && State.Client(LocalClientId).m_PlayerInfo.m_Team != TEAM_SPECTATORS;
	if(!LocalPlayerInGame)
		return;

	const int PredictionTick = Time.m_PredictionTick;

	if(PredictionTick == pCurrent->m_StartTick)
		return;

	// get positions
	float Curvature = 0;
	float Speed = 0;
	const CTuningParams *pTuning = &State.Tuning(pCurrent->m_TuneZone);

	if(pCurrent->m_Type == WEAPON_GRENADE)
	{
		Curvature = pTuning->m_GrenadeCurvature;
		Speed = pTuning->m_GrenadeSpeed;
	}
	else if(pCurrent->m_Type == WEAPON_SHOTGUN)
	{
		Curvature = pTuning->m_ShotgunCurvature;
		Speed = pTuning->m_ShotgunSpeed;
	}
	else if(pCurrent->m_Type == WEAPON_GUN)
	{
		Curvature = pTuning->m_GunCurvature;
		Speed = pTuning->m_GunSpeed;
	}

	float Pt = ((float)(PredictionTick - pCurrent->m_StartTick) + Time.m_PredIntraGameTick) / (float)Time.m_GameTickSpeed;
	if(Pt < 0)
		return; // projectile haven't been shot yet

	float Gt = (Time.m_PrevGameTick - pCurrent->m_StartTick) / (float)Time.m_GameTickSpeed + Time.m_GameTickTime;

	const int OwnerClientId = pCurrent->m_ExtraInfo ? pCurrent->m_Owner : -1;

	float T = Pt;
	if(DestroyTick >= 0)
		T = std::min(Pt, ((float)(DestroyTick - 1 - pCurrent->m_StartTick) + Time.m_PredIntraGameTick) / (float)Time.m_GameTickSpeed);

	float MinTrailSpan = 0.4f * ((pCurrent->m_Type == WEAPON_GRENADE) ? 0.5f : 0.25f);
	float Step = std::max(Client()->FrameTimeAverage(), (pCurrent->m_Type == WEAPON_GRENADE) ? 0.02f : 0.01f);
	for(int i = 1 + (int)(Gt / Step); i < (int)(T / Step); i++)
	{
		float t = Step * (float)i + 0.4f * Step * random_float(-0.5f, 0.5f);
		vec2 Pos = CalcPos(pCurrent->m_StartPos, pCurrent->m_StartVel, Curvature, Speed, t);
		vec2 PrevPos = CalcPos(pCurrent->m_StartPos, pCurrent->m_StartVel, Curvature, Speed, t - 0.001f);
		vec2 Vel = Pos - PrevPos;
		float TimePassed = Pt - t;
		if(Pt - MinTrailSpan > 0.01f)
			TimePassed = std::min(TimePassed, (TimePassed - MinTrailSpan) / (Pt - MinTrailSpan) * (MinTrailSpan * 0.5f) + MinTrailSpan);
		// add particle for this projectile
		if(pCurrent->m_Type == WEAPON_GRENADE)
			GameClient()->m_Effects.SmokeTrail(State, Pos, Vel * -1, OwnerClientId, 1.0f, TimePassed);
		else
			GameClient()->m_Effects.BulletTrail(State, Pos, OwnerClientId, 1.0f, TimePassed);
	}
}
