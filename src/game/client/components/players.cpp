/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "players.h"

#include <base/color.h>
#include <base/dbg.h>
#include <base/math.h>

#include <engine/client/enums.h>
#include <engine/demo.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/client_data.h>
#include <generated/client_data7.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/components/controls.h>
#include <game/client/components/effects.h>
#include <game/client/components/skins.h>
#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

static float CalculateHandAngle(vec2 Dir, float AngleOffset)
{
	const float Angle = angle(Dir);
	if(Dir.x < 0.0f)
	{
		return Angle - AngleOffset;
	}
	else
	{
		return Angle + AngleOffset;
	}
}

static vec2 CalculateHandPosition(vec2 CenterPos, vec2 Dir, vec2 PostRotOffset)
{
	vec2 DirY = vec2(-Dir.y, Dir.x);
	if(Dir.x < 0.0f)
	{
		DirY = -DirY;
	}
	return CenterPos + Dir + Dir * PostRotOffset.x + DirY * PostRotOffset.y;
}

void CPlayers::RenderHand(const CTeeRenderInfo *pInfo, vec2 CenterPos, vec2 Dir, float AngleOffset, vec2 PostRotOffset, float Alpha)
{
	const vec2 HandPos = CalculateHandPosition(CenterPos, Dir, PostRotOffset);
	const float HandAngle = CalculateHandAngle(Dir, AngleOffset);
	if(pInfo->m_Sixup.PartTexture(protocol7::SKINPART_HANDS).IsValid())
	{
		RenderHand7(pInfo, HandPos, HandAngle, Alpha);
	}
	else
	{
		RenderHand6(pInfo, HandPos, HandAngle, Alpha);
	}
}

void CPlayers::RenderHand7(const CTeeRenderInfo *pInfo, vec2 HandPos, float HandAngle, float Alpha)
{
	// in-game hand size is 15 when tee size is 64
	const float BaseSize = 15.0f * (pInfo->m_Size / 64.0f);
	IGraphics::CQuadItem QuadOutline(HandPos.x, HandPos.y, 2 * BaseSize, 2 * BaseSize);
	IGraphics::CQuadItem QuadHand = QuadOutline;

	Graphics()->TextureSet(pInfo->m_Sixup.PartTexture(protocol7::SKINPART_HANDS));
	Graphics()->QuadsBegin();
	Graphics()->SetColor(pInfo->m_Sixup.m_aColors[protocol7::SKINPART_HANDS].WithAlpha(Alpha));
	Graphics()->QuadsSetRotation(HandAngle);
	Graphics()->SelectSprite7(client_data7::SPRITE_TEE_HAND_OUTLINE);
	Graphics()->QuadsDraw(&QuadOutline, 1);
	Graphics()->SelectSprite7(client_data7::SPRITE_TEE_HAND);
	Graphics()->QuadsDraw(&QuadHand, 1);
	Graphics()->QuadsEnd();
}

void CPlayers::RenderHand6(const CTeeRenderInfo *pInfo, vec2 HandPos, float HandAngle, float Alpha)
{
	const CSkin::CSkinTextures *pSkinTextures = pInfo->m_CustomColoredSkin ? &pInfo->m_ColorableRenderSkin : &pInfo->m_OriginalRenderSkin;

	Graphics()->SetColor(pInfo->m_ColorBody.WithAlpha(Alpha));
	Graphics()->QuadsSetRotation(HandAngle);
	Graphics()->TextureSet(pSkinTextures->m_HandsOutline);
	Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, NUM_WEAPONS * 2, HandPos.x, HandPos.y);
	Graphics()->TextureSet(pSkinTextures->m_Hands);
	Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, NUM_WEAPONS * 2 + 1, HandPos.x, HandPos.y);
}

float CPlayers::GetPlayerTargetAngle(
	const CGameSessionContext &Session,
	const CGameState &GameState,
	const CGameTickInfo &Time,
	const CNetObj_Character *pPrevChar,
	const CNetObj_Character *pPlayerChar,
	int ClientId,
	float Intra)
{
	const CGameState::CClientSnapshot *pSnapshotClient = in_range(ClientId, MAX_CLIENTS - 1) ? &GameState.Client(ClientId) : nullptr;
	const CGameState *pInputState = nullptr;
	for(const auto &pState : Session.GameStates().States())
	{
		if(pState->LocalClientId() == ClientId)
		{
			pInputState = pState.get();
			break;
		}
	}
	const CGameState::CClientSnapshot *pInputClient = pInputState != nullptr ? &pInputState->Client(ClientId) : nullptr;
	const bool LocalInput = pInputClient != nullptr && (pInputState == &GameState || g_Config.m_ClPredictDummy) && pInputClient->m_HasCharacter && !Time.m_IsDemoPlayback &&
				(!pInputClient->m_HasPlayerInfo || pInputClient->m_PlayerInfo.m_Team != TEAM_SPECTATORS) &&
				(!pInputClient->m_HasDDNetPlayer || (pInputClient->m_DDNetPlayer.m_Flags & (EXPLAYERFLAG_PAUSED | EXPLAYERFLAG_SPEC)) == 0);
	if(LocalInput && pInputState == &GameState)
	{
		const vec2 MousePos = pInputState->Input().m_MousePos;
		vec2 Direction = normalize(vec2((int)MousePos.x, (int)MousePos.y));

		// fix direction if mouse is exactly in the center
		if(Direction == vec2(0.0f, 0.0f))
			Direction = vec2(1.0f, 0.0f);

		return angle(Direction);
	}
	else if(LocalInput)
	{
		const CStreamInputRoute *pRoute = Session.InputRouter().Find(pInputState->StreamId());
		const CNetObj_PlayerInput &Input = pRoute != nullptr && pRoute->m_Policy == EStreamInputPolicy::HAMMER ? pRoute->m_HammerInput : pInputState->Input().m_InputData;
		return angle(vec2(Input.m_TargetX, Input.m_TargetY));
	}

	// using unpredicted angle when rendering other players in-game
	if(ClientId >= 0)
		Intra = Time.m_IntraGameTick;

	if(pSnapshotClient != nullptr && pSnapshotClient->m_HasExtendedCharacter && pSnapshotClient->m_ExtendedCharacter.m_JumpedTotal != -1)
	{
		const CNetObj_DDNetCharacter *pExtendedData = &pSnapshotClient->m_ExtendedCharacter;
		if(pSnapshotClient->m_HasPrevExtendedCharacter)
		{
			float MixX = mix((float)pSnapshotClient->m_PrevExtendedTargetX, (float)pExtendedData->m_TargetX, Intra);
			float MixY = mix((float)pSnapshotClient->m_PrevExtendedTargetY, (float)pExtendedData->m_TargetY, Intra);
			return angle(vec2(MixX, MixY));
		}
		else
		{
			return angle(vec2(pExtendedData->m_TargetX, pExtendedData->m_TargetY));
		}
	}
	else
	{
		// If the player moves their weapon through top, then change
		// the end angle by 2*Pi, so that the mix function will use the
		// short path and not the long one.
		if(pPlayerChar->m_Angle > (256.0f * pi) && pPrevChar->m_Angle < 0)
		{
			return mix((float)pPrevChar->m_Angle, (float)(pPlayerChar->m_Angle - 256.0f * 2 * pi), Intra) / 256.0f;
		}
		else if(pPlayerChar->m_Angle < 0 && pPrevChar->m_Angle > (256.0f * pi))
		{
			return mix((float)pPrevChar->m_Angle, (float)(pPlayerChar->m_Angle + 256.0f * 2 * pi), Intra) / 256.0f;
		}
		else
		{
			return mix((float)pPrevChar->m_Angle, (float)pPlayerChar->m_Angle, Intra) / 256.0f;
		}
	}
}

void CPlayers::RenderHookCollLine(
	const CRenderContext &Context,
	const CScreenRect &ScreenRect,
	const CNetObj_Character *pPrevChar,
	const CNetObj_Character *pPlayerChar,
	int ClientId)
{
	CNetObj_Character Prev;
	CNetObj_Character Player;
	Prev = *pPrevChar;
	Player = *pPlayerChar;

	dbg_assert(in_range(ClientId, MAX_CLIENTS - 1), "invalid client id (%d)", ClientId);

	const CGameState &GameState = Context.m_State;
	if(!GameState.CoreGameInfo().m_AllowHookColl)
		return;

	bool Local = GameState.LocalClientId() == ClientId;

#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && !g_Config.m_ClVideoShowHookCollOther && !Local)
		return;
#endif

	bool Aim = (Player.m_PlayerFlags & PLAYERFLAG_AIM);
	if(!Client()->ServerCapAnyPlayerFlag())
	{
		for(const auto &pState : Context.m_Session.GameStates().States())
		{
			if(ClientId == pState->LocalClientId())
			{
				Aim = pState->Input().m_ShowHookColl;
				break;
			}
		}
	}

	const CGameState *pSessionLocalState = nullptr;
	for(const auto &pState : Context.m_Session.GameStates().States())
	{
		if(pState->LocalClientId() == ClientId)
		{
			pSessionLocalState = pState.get();
			break;
		}
	}
	const CStreamInputRoute *pInputRoute = pSessionLocalState != nullptr ? Context.m_Session.InputRouter().Find(pSessionLocalState->StreamId()) : nullptr;
	const bool CopyMoves = pInputRoute != nullptr && pInputRoute->m_Policy == EStreamInputPolicy::COPY_MOVES;
	if(CopyMoves)
		Aim = false;

	bool AlwaysRenderHookColl = (Local ? g_Config.m_ClShowHookCollOwn : g_Config.m_ClShowHookCollOther) == 2;
	bool RenderHookCollPlayer = Aim && (Local ? g_Config.m_ClShowHookCollOwn : g_Config.m_ClShowHookCollOther) > 0;
	if(Local)
		RenderHookCollPlayer = GameState.Input().m_ShowHookColl && g_Config.m_ClShowHookCollOwn > 0;

	if(CopyMoves && GameState.Input().m_ShowHookColl)
	{
		RenderHookCollPlayer = g_Config.m_ClShowHookCollOther > 0;
	}

	if(!AlwaysRenderHookColl && !RenderHookCollPlayer)
		return;

	const CGameState::CRenderedClient &RenderedClient = GameState.RenderedClient(ClientId);
	float Intra = RenderedClient.m_IsPredicted ? Context.m_Time.m_PredIntraGameTick : Context.m_Time.m_IntraGameTick;
	float Angle = GetPlayerTargetAngle(Context.m_Session, GameState, Context.m_Time, &Prev, &Player, ClientId, Intra);

	vec2 Position = RenderedClient.m_Position;
	vec2 Direction = direction(Angle);

	// When the other player isn't predicted, we don't know their tunes.
	// Use our own tunes instead. This is wrong, but a good heuristic.
	const CGameState::CPredictedClient &PredictedClient = GameState.PredictedClient(ClientId);
	const CTuningParams &Tuning = RenderedClient.m_IsPredicted && PredictedClient.m_HasCurrent ? PredictedClient.m_Current.m_Tuning : GameState.Tuning();
	float HookLength = Tuning.m_HookLength;
	float HookFireSpeed = Tuning.m_HookFireSpeed;

	// Check, if the player is outside the screen-rect
	// If the map contains hook teleports, we are out of luck since we don't know if it will enter the screen at any point.
	if(!Collision()->HasHookTeleIns())
	{
		const float MaxHookReach = HookLength + HookFireSpeed;

		if(Position.x < ScreenRect.m_TopLeft.x - (Direction.x >= 0 ? MaxHookReach : 0) ||
			Position.x > ScreenRect.m_BottomRight.x + (Direction.x <= 0 ? MaxHookReach : 0) ||
			Position.y < ScreenRect.m_TopLeft.y - (Direction.y >= 0 ? MaxHookReach : 0) ||
			Position.y > ScreenRect.m_BottomRight.y + (Direction.y <= 0 ? MaxHookReach : 0))
		{
			return;
		}
	}

	static constexpr float HOOK_START_DISTANCE = CCharacterCore::PhysicalSize() * 1.5f;

	// janky physics
	if(HookLength < HOOK_START_DISTANCE || HookFireSpeed <= 0.0f)
		return;

	vec2 QuantizedDirection = Direction;
	vec2 StartOffset = Direction * HOOK_START_DISTANCE;
	vec2 BasePos = Position;
	vec2 LineStartPos = BasePos + StartOffset;
	vec2 SegmentStartPos = LineStartPos;

	ColorRGBA HookCollColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorNoColl));
	std::vector<IGraphics::CLineItem> vLineSegments;

	const int MaxHookTicks = 5 * Client()->GameTickSpeed(); // calculating above 5 seconds is very expensive and unlikely to happen

	auto AddHookPlayerSegment = [&](const vec2 &StartPos, const vec2 &EndPos, const vec2 &HookablePlayerPosition, const vec2 &HitPos) {
		HookCollColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorTeeColl));

		// stop hookline at player circle so it looks better
		vec2 aIntersections[2];
		int NumIntersections = intersect_line_circle(StartPos, EndPos, HookablePlayerPosition, CCharacterCore::PhysicalSize() * 1.45f / 2.0f, aIntersections);
		if(NumIntersections == 2)
		{
			if(distance(Position, aIntersections[0]) < distance(Position, aIntersections[1]))
				vLineSegments.emplace_back(StartPos, aIntersections[0]);
			else
				vLineSegments.emplace_back(StartPos, aIntersections[1]);
		}
		else if(NumIntersections == 1)
		{
			vLineSegments.emplace_back(StartPos, aIntersections[0]);
		}
		else
		{
			vLineSegments.emplace_back(StartPos, HitPos);
		}
	};

	// simulate the hook into the future
	int HookTick;
	bool HookEnteredTelehook = false;
	std::optional<IGraphics::CLineItem> HookTipLineSegment;
	for(HookTick = 0; HookTick < MaxHookTicks; ++HookTick)
	{
		int Tele;
		vec2 HitPos, IntersectedPlayerPosition;
		vec2 SegmentEndPos = SegmentStartPos + QuantizedDirection * HookFireSpeed;

		// check if a hook would enter retracting state in this tick
		if(distance(BasePos, SegmentEndPos) > HookLength)
		{
			// check if the retracting hook hits a player
			if(!HookEnteredTelehook)
			{
				vec2 RetractingHookEndPos = BasePos + normalize(SegmentEndPos - BasePos) * HookLength;
				// you can't hook a player, if the hook is behind solids, however you miss the solids as well
				int Hit = Collision()->IntersectLineTeleHook(SegmentStartPos, RetractingHookEndPos, &HitPos, nullptr, &Tele);

				if(GameClient()->IntersectCharacter(SegmentStartPos, HitPos, RetractingHookEndPos, ClientId, &IntersectedPlayerPosition) != -1)
				{
					AddHookPlayerSegment(LineStartPos, SegmentEndPos, IntersectedPlayerPosition, RetractingHookEndPos);
					break;
				}

				// Retracting hooks don't go through hook teleporters
				if(Hit && Hit != TILE_TELEINHOOK)
				{
					// The hook misses the player, but also misses the solid
					vLineSegments.emplace_back(LineStartPos, SegmentStartPos);

					// The player hook misses due to a solid
					HookTipLineSegment = IGraphics::CLineItem(SegmentStartPos, HitPos);
					break;
				}

				// we are missing the player, the solid hookline stopped already, but we want this extra line segment
				// the player-hooking-hook is only longer, if we didn't go through a tele hook
				HookTipLineSegment = IGraphics::CLineItem(SegmentStartPos, RetractingHookEndPos);
			}

			// the line is too long here, and the hook retracts, use old position
			vLineSegments.emplace_back(LineStartPos, SegmentStartPos);
			break;
		}

		// check for map collisions
		int Hit = Collision()->IntersectLineTeleHook(SegmentStartPos, SegmentEndPos, &HitPos, nullptr, &Tele);

		// check if we intersect a player
		if(GameClient()->IntersectCharacter(SegmentStartPos, HitPos, SegmentEndPos, ClientId, &IntersectedPlayerPosition) != -1)
		{
			AddHookPlayerSegment(LineStartPos, HitPos, IntersectedPlayerPosition, SegmentEndPos);
			break;
		}

		// we hit nothing, continue calculating segments
		if(!Hit)
		{
			SegmentStartPos = SegmentEndPos;
			SegmentStartPos.x = round_to_int(SegmentStartPos.x);
			SegmentStartPos.y = round_to_int(SegmentStartPos.y);

			// direction is always the same after the first tick quantization
			if(HookTick == 0)
			{
				QuantizedDirection.x = round_to_int(QuantizedDirection.x * 256.0f) / 256.0f;
				QuantizedDirection.y = round_to_int(QuantizedDirection.y * 256.0f) / 256.0f;
			}
			continue;
		}

		// we hit a solid / hook stopper
		if(Hit != TILE_TELEINHOOK)
		{
			if(Hit != TILE_NOHOOK)
				HookCollColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorHookableColl));
			vLineSegments.emplace_back(LineStartPos, HitPos);
			break;
		}

		// we are hitting TILE_TELEINHOOK
		vLineSegments.emplace_back(LineStartPos, HitPos);
		HookEnteredTelehook = true;

		// check tele outs
		const std::vector<vec2> &vTeleOuts = Collision()->TeleOuts(Tele - 1);
		if(vTeleOuts.empty())
		{
			// the hook gets stuck, this is a feature or a bug
			HookCollColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorHookableColl));
			break;
		}
		else if(vTeleOuts.size() > 1)
		{
			// we don't know which teleout the hook takes, just invert the color
			HookCollColor = color_invert(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorTeeColl)));
			break;
		}

		// go through one teleout, update positions and continue
		BasePos = vTeleOuts[0];
		LineStartPos = BasePos; // make the line start in the teleporter to prevent a gap
		SegmentStartPos = BasePos + Direction * HOOK_START_DISTANCE;
		SegmentStartPos.x = round_to_int(SegmentStartPos.x);
		SegmentStartPos.y = round_to_int(SegmentStartPos.y);

		// direction is always the same after the first tick quantization
		if(HookTick == 0)
		{
			QuantizedDirection.x = round_to_int(QuantizedDirection.x * 256.0f) / 256.0f;
			QuantizedDirection.y = round_to_int(QuantizedDirection.y * 256.0f) / 256.0f;
		}
	}

	// The hook line is too expensive to calculate and didn't hit anything before, just set a straight line
	if(HookTick >= MaxHookTicks && vLineSegments.empty())
	{
		// we simply don't know if we hit anything or not
		HookCollColor = color_invert(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorTeeColl)));
		vLineSegments.emplace_back(LineStartPos, BasePos + QuantizedDirection * HookLength);
	}

	// add a line from the player to the start position to prevent a visual gap
	vLineSegments.emplace_back(Position, Position + StartOffset);

	if(AlwaysRenderHookColl && RenderHookCollPlayer)
	{
		// invert the hook coll colors when using cl_show_hook_coll_always and +showhookcoll is pressed
		HookCollColor = color_invert(HookCollColor);
	}

	// Render hook coll line
	const int HookCollSize = Local ? g_Config.m_ClHookCollSize : g_Config.m_ClHookCollSizeOther;

	float Alpha = GameClient()->IsOtherTeam(ClientId) ? g_Config.m_ClShowOthersAlpha / 100.0f : 1.0f;
	Alpha *= (float)g_Config.m_ClHookCollAlpha / 100;
	if(Alpha <= 0.0f)
		return;
	ColorRGBA HookCollTipColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollTipColor, true));

	Graphics()->TextureClear();
	if(HookCollSize > 0)
	{
		std::vector<IGraphics::CFreeformItem> vLineQuadSegments;
		vLineQuadSegments.reserve(vLineSegments.size());

		float LineWidth = 0.5f + (float)(HookCollSize - 1) * 0.25f;
		const vec2 PerpToAngle = normalize(vec2(Direction.y, -Direction.x)) * GameClient()->m_Camera.Zoom();

		auto ConvertLineSegments = [&](const IGraphics::CLineItem &LineSegment) {
			vec2 DrawInitPos(LineSegment.m_X0, LineSegment.m_Y0);
			vec2 DrawFinishPos(LineSegment.m_X1, LineSegment.m_Y1);
			vec2 Pos0 = DrawFinishPos + PerpToAngle * -LineWidth;
			vec2 Pos1 = DrawFinishPos + PerpToAngle * LineWidth;
			vec2 Pos2 = DrawInitPos + PerpToAngle * -LineWidth;
			vec2 Pos3 = DrawInitPos + PerpToAngle * LineWidth;
			vLineQuadSegments.emplace_back(Pos0.x, Pos0.y, Pos1.x, Pos1.y, Pos2.x, Pos2.y, Pos3.x, Pos3.y);
		};

		for(const auto &LineSegment : vLineSegments)
		{
			ConvertLineSegments(LineSegment);
		}

		vLineSegments.clear();

		Graphics()->QuadsBegin();
		Graphics()->SetColor(HookCollColor.WithAlpha(Alpha));
		Graphics()->QuadsDrawFreeform(vLineQuadSegments.data(), vLineQuadSegments.size());
		if(HookTipLineSegment.has_value() && HookCollTipColor.a > 0.0f)
		{
			vLineQuadSegments.clear();
			ConvertLineSegments(HookTipLineSegment.value());
			Graphics()->SetColor(HookCollTipColor.WithMultipliedAlpha(Alpha));
			Graphics()->QuadsDrawFreeform(vLineQuadSegments.data(), vLineQuadSegments.size());
		}
		Graphics()->QuadsEnd();
	}
	else
	{
		Graphics()->LinesBegin();
		Graphics()->SetColor(HookCollColor.WithAlpha(Alpha));
		Graphics()->LinesDraw(vLineSegments.data(), vLineSegments.size());
		if(HookTipLineSegment.has_value() && HookCollTipColor.a > 0.0f)
		{
			Graphics()->SetColor(HookCollTipColor.WithMultipliedAlpha(Alpha));
			Graphics()->LinesDraw(&HookTipLineSegment.value(), 1);
		}
		Graphics()->LinesEnd();
	}
}

void CPlayers::RenderHook(
	const CRenderContext &Context,
	const CScreenRect &ScreenRect,
	const CNetObj_Character *pPrevChar,
	const CNetObj_Character *pPlayerChar,
	const CTeeRenderInfo *pRenderInfo,
	int ClientId,
	float Intra)
{
	if(pPlayerChar->m_HookState <= 0)
		return;

	CNetObj_Character Prev;
	CNetObj_Character Player;
	Prev = *pPrevChar;
	Player = *pPlayerChar;

	CTeeRenderInfo RenderInfo = *pRenderInfo;

	// don't render hooks to not active character cores
	if(in_range(pPlayerChar->m_HookedPlayer, MAX_CLIENTS - 1) && !Context.m_State.RenderedClient(pPlayerChar->m_HookedPlayer).m_Active)
		return;

	if(ClientId >= 0)
		Intra = Context.m_State.RenderedClient(ClientId).m_IsPredicted ? Context.m_Time.m_PredIntraGameTick : Context.m_Time.m_IntraGameTick;

	bool OtherTeam = Context.IsOtherTeam(ClientId);
	float Alpha = (OtherTeam || ClientId < 0) ? g_Config.m_ClShowOthersAlpha / 100.0f : 1.0f;
	if(ClientId == -2) // ghost
		Alpha = g_Config.m_ClRaceGhostAlpha / 100.0f;

	RenderInfo.m_Size = 64.0f;

	vec2 Position;
	if(in_range(ClientId, MAX_CLIENTS - 1))
		Position = Context.m_State.RenderedClient(ClientId).m_Position;
	else
		Position = mix(vec2(Prev.m_X, Prev.m_Y), vec2(Player.m_X, Player.m_Y), Intra);

	// draw hook
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	if(ClientId < 0)
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.5f);

	vec2 Pos = Position;
	vec2 HookPos;

	if(in_range(pPlayerChar->m_HookedPlayer, MAX_CLIENTS - 1))
		HookPos = Context.m_State.RenderedClient(pPlayerChar->m_HookedPlayer).m_Position;
	else
		HookPos = mix(vec2(Prev.m_HookX, Prev.m_HookY), vec2(Player.m_HookX, Player.m_HookY), Intra);

	if((Pos.x < ScreenRect.m_TopLeft.x && HookPos.x < ScreenRect.m_TopLeft.x) ||
		(Pos.x > ScreenRect.m_BottomRight.x && HookPos.x > ScreenRect.m_BottomRight.x) ||
		(Pos.y < ScreenRect.m_TopLeft.y && HookPos.y < ScreenRect.m_TopLeft.y) ||
		(Pos.y > ScreenRect.m_BottomRight.y && HookPos.y > ScreenRect.m_BottomRight.y))
		return;

	float d = distance(Pos, HookPos);
	vec2 Dir = normalize(Pos - HookPos);

	Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookHead);
	Graphics()->QuadsSetRotation(angle(Dir) + pi);
	// render head
	int QuadOffset = NUM_WEAPONS * 2 + 2;
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
	Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, HookPos.x, HookPos.y);

	// render chain
	++QuadOffset;
	static IGraphics::SRenderSpriteInfo s_aHookChainRenderInfo[1024];
	int HookChainCount = 0;
	for(float f = 24; f < d && HookChainCount < 1024; f += 24, ++HookChainCount)
	{
		vec2 p = HookPos + Dir * f;
		s_aHookChainRenderInfo[HookChainCount].m_Pos[0] = p.x;
		s_aHookChainRenderInfo[HookChainCount].m_Pos[1] = p.y;
		s_aHookChainRenderInfo[HookChainCount].m_Scale = 1;
		s_aHookChainRenderInfo[HookChainCount].m_Rotation = angle(Dir) + pi;
	}
	Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookChain);
	Graphics()->RenderQuadContainerAsSpriteMultiple(m_WeaponEmoteQuadContainerIndex, QuadOffset, HookChainCount, s_aHookChainRenderInfo);

	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);

	RenderHand(&RenderInfo, Position, normalize(HookPos - Pos), -pi / 2, vec2(20, 0), Alpha);
}

void CPlayers::PrepareRenderInfo(const CGameSessionContext &Session, const CGameState &GameState, int ClientId, bool IsTeamPlay, CTeeRenderInfo &RenderInfo) const
{
	RenderInfo = GameClient()->m_aClients[ClientId].m_RenderInfo;
	RenderInfo.m_TeeRenderFlags = 0;

	// Predict freeze skin only for local players.
	bool Frozen = false;
	bool IsLocal = false;
	for(const auto &pState : Session.GameStates().States())
		IsLocal |= ClientId == pState->LocalClientId();
	if(IsLocal)
	{
		const CCharacterCore &Predicted = GameState.PredictedClient(ClientId).m_Current;
		if(Predicted.m_FreezeEnd != 0)
			RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
		if(Predicted.m_LiveFrozen)
			RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
		if(Predicted.m_Invincible)
			RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_SPARKLE;

		Frozen = Predicted.m_FreezeEnd != 0;
	}
	else
	{
		const CNetObj_DDNetCharacter *pExtended = GameState.ExtendedCharacter(ClientId);
		if(pExtended != nullptr && pExtended->m_FreezeEnd != 0)
			RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
		if(pExtended != nullptr && (pExtended->m_Flags & CHARACTERFLAG_MOVEMENTS_DISABLED) != 0)
			RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
		if(pExtended != nullptr && (pExtended->m_Flags & CHARACTERFLAG_INVINCIBLE) != 0)
			RenderInfo.m_TeeRenderFlags |= TEE_EFFECT_SPARKLE;

		Frozen = pExtended != nullptr && pExtended->m_FreezeEnd != 0;
	}

	if((GameState.RenderedClient(ClientId).m_Cur.m_Weapon == WEAPON_NINJA || (Frozen && !GameState.CoreGameInfo().m_NoSkinChangeForFrozen)) && g_Config.m_ClShowNinja)
	{
		RenderInfo.m_Sixup.Reset();
		RenderInfo.ApplySkin(NinjaTeeRenderInfo()->TeeRenderInfo());
		RenderInfo.m_CustomColoredSkin = IsTeamPlay;
		if(!IsTeamPlay)
		{
			RenderInfo.m_ColorBody = ColorRGBA(1, 1, 1);
			RenderInfo.m_ColorFeet = ColorRGBA(1, 1, 1);
		}
	}
}

bool CPlayers::PreparePlayerRenderState(
	const CGameSessionContext &Session,
	const CGameState &GameState,
	const CGameTickInfo &Time,
	bool IsOtherTeam,
	std::span<const CVisibleWorldRect> vVisibleWorldRects,
	vec2 VisibilityMargin,
	const CNetObj_Character *pPrevChar,
	const CNetObj_Character *pPlayerChar,
	const CTeeRenderInfo *pRenderInfo,
	int ClientId,
	float Intra,
	CPlayerRenderState &State)
{
	State.m_Prev = *pPrevChar;
	State.m_Player = *pPlayerChar;
	State.m_RenderInfo = *pRenderInfo;
	State.m_Intra = Intra;

	if(in_range(ClientId, MAX_CLIENTS - 1))
		State.m_Position = GameState.RenderedClient(ClientId).m_Position;
	else
		State.m_Position = mix(vec2(State.m_Prev.m_X, State.m_Prev.m_Y), vec2(State.m_Player.m_X, State.m_Player.m_Y), State.m_Intra);

	bool Visible = false;
	for(const CVisibleWorldRect &VisibleWorldRect : vVisibleWorldRects)
		Visible |= VisibleWorldRect.Inside(State.m_Position, VisibilityMargin);
	if(!Visible)
		return false;

	const CGameState::CSceneClockState &SceneClock = GameState.SceneClock();
	State.m_Local = GameState.LocalClientId() == ClientId;
	State.m_Alpha = (IsOtherTeam || ClientId < 0) ? g_Config.m_ClShowOthersAlpha / 100.0f : 1.0f;
	if(ClientId == -2) // ghost
		State.m_Alpha = g_Config.m_ClRaceGhostAlpha / 100.0f;

	State.m_RenderInfo.m_Size = 64.0f;

	if(ClientId >= 0)
		State.m_Intra = GameState.RenderedClient(ClientId).m_IsPredicted ? Time.m_PredIntraGameTick : Time.m_IntraGameTick;

	State.m_PredictLocalWeapons = false;
	State.m_AttackTime = (Time.m_PrevGameTick - State.m_Player.m_AttackTick) / (float)Time.m_GameTickSpeed + Time.m_GameTickTime;
	State.m_LastAttackTime = (Time.m_PrevGameTick - State.m_Player.m_AttackTick) / (float)Time.m_GameTickSpeed + SceneClock.m_GameTickTime;
	if(ClientId >= 0 && GameState.RenderedClient(ClientId).m_IsPredictedLocal && g_Config.m_ClAntiPing && g_Config.m_ClAntiPingGrenade && g_Config.m_ClAntiPingWeapons && g_Config.m_ClAntiPingGunfire)
	{
		State.m_PredictLocalWeapons = true;
		State.m_AttackTime = (Time.m_PredIntraGameTick + (Time.m_PredGameTick - 1 - State.m_Player.m_AttackTick)) / (float)Time.m_GameTickSpeed;
		State.m_LastAttackTime = (SceneClock.m_PredIntraTick + (Time.m_PredGameTick - 1 - State.m_Player.m_AttackTick)) / (float)Time.m_GameTickSpeed;
	}
	State.m_AttackTicksPassed = State.m_AttackTime * (float)Time.m_GameTickSpeed;

	State.m_Angle = GetPlayerTargetAngle(Session, GameState, Time, &State.m_Prev, &State.m_Player, ClientId, State.m_Intra);
	State.m_Direction = direction(State.m_Angle);
	State.m_Vel = mix(vec2(State.m_Prev.m_VelX / 256.0f, State.m_Prev.m_VelY / 256.0f), vec2(State.m_Player.m_VelX / 256.0f, State.m_Player.m_VelY / 256.0f), State.m_Intra);

	State.m_RenderInfo.m_GotAirJump = (State.m_Player.m_Jumped & 2) == 0;
	State.m_RenderInfo.m_FeetFlipped = false;

	State.m_Stationary = State.m_Player.m_VelX <= 1 && State.m_Player.m_VelX >= -1;
	State.m_InAir = !Collision()->CheckPoint(State.m_Player.m_X, State.m_Player.m_Y + 16);
	const bool Running = State.m_Player.m_VelX >= 5000 || State.m_Player.m_VelX <= -5000;
	State.m_WantOtherDir = (State.m_Player.m_Direction == -1 && State.m_Vel.x > 0) || (State.m_Player.m_Direction == 1 && State.m_Vel.x < 0);
	const CNetObj_DDNetPlayer *pDDNetPlayer = in_range(ClientId, MAX_CLIENTS - 1) && GameState.Client(ClientId).m_HasDDNetPlayer ? &GameState.Client(ClientId).m_DDNetPlayer : nullptr;
	State.m_Afk = pDDNetPlayer != nullptr && (pDDNetPlayer->m_Flags & EXPLAYERFLAG_AFK) != 0;
	const bool Paused = pDDNetPlayer != nullptr && (pDDNetPlayer->m_Flags & EXPLAYERFLAG_PAUSED) != 0;
	State.m_Inactive = State.m_Afk || Paused;

	float WalkTime = std::fmod(State.m_Position.x, 100.0f) / 100.0f;
	float RunTime = std::fmod(State.m_Position.x, 200.0f) / 200.0f;
	if(WalkTime < 0.0f)
		WalkTime += 1.0f;
	if(RunTime < 0.0f)
		RunTime += 1.0f;

	State.m_Animation.Set(&g_pData->m_aAnimations[ANIM_BASE], 0.0f);
	if(State.m_InAir)
	{
		State.m_Animation.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0.0f, 1.0f);
	}
	else if(State.m_Stationary)
	{
		if(State.m_Inactive)
		{
			State.m_Animation.Add(State.m_Direction.x < 0.0f ? &g_pData->m_aAnimations[ANIM_SIT_LEFT] : &g_pData->m_aAnimations[ANIM_SIT_RIGHT], 0.0f, 1.0f);
			State.m_RenderInfo.m_FeetFlipped = true;
		}
		else
		{
			State.m_Animation.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0.0f, 1.0f);
		}
	}
	else if(!State.m_WantOtherDir)
	{
		if(Running)
			State.m_Animation.Add(State.m_Player.m_VelX < 0 ? &g_pData->m_aAnimations[ANIM_RUN_LEFT] : &g_pData->m_aAnimations[ANIM_RUN_RIGHT], RunTime, 1.0f);
		else
			State.m_Animation.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);
	}

	constexpr float HammerAnimationTimeScale = 5.0f;
	if(State.m_Player.m_Weapon == WEAPON_HAMMER)
		State.m_Animation.Add(&g_pData->m_aAnimations[ANIM_HAMMER_SWING], std::clamp(State.m_LastAttackTime * HammerAnimationTimeScale, 0.0f, 1.0f), 1.0f);
	if(State.m_Player.m_Weapon == WEAPON_NINJA)
		State.m_Animation.Add(&g_pData->m_aAnimations[ANIM_NINJA_SWING], std::clamp(State.m_LastAttackTime * 2.0f, 0.0f, 1.0f), 1.0f);

	return true;
}

void CPlayers::UpdatePlayerPresentation(
	const CPresentationContext &Context,
	const CNetObj_Character *pPrevChar,
	const CNetObj_Character *pPlayerChar,
	const CTeeRenderInfo *pRenderInfo,
	int ClientId,
	float Intra)
{
	CGameState &GameState = Context.m_State;
	CPlayerRenderState State;
	if(!PreparePlayerRenderState(Context.m_Session, GameState, Context.m_Time, Context.IsOtherTeamFromLocalPlayer(ClientId), Context.m_vVisibleWorldRects, vec2(100.0f, 100.0f), pPrevChar, pPlayerChar, pRenderInfo, ClientId, Intra, State))
		return;

	constexpr float Volume = 1.0f;
	if(!State.m_InAir && State.m_WantOtherDir && length(State.m_Vel * 50) > 500.0f)
		GameClient()->m_Effects.SkidTrail(GameState, Context.m_Time, State.m_Position, State.m_Vel, State.m_Player.m_Direction, ClientId, 1.0f, Volume, Context.m_Audio == EPresentationAudio::AUDIBLE);

	if(State.m_Player.m_Weapon == WEAPON_NINJA && !(State.m_RenderInfo.m_TeeRenderFlags & TEE_NO_WEAPON))
	{
		const int CurrentWeapon = std::clamp(State.m_Player.m_Weapon, 0, NUM_WEAPONS - 1);
		vec2 WeaponPosition = State.m_Position;
		WeaponPosition.y += g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsety;
		if(State.m_Inactive && !State.m_InAir && State.m_Stationary)
			WeaponPosition.y += 3.0f;
		if(State.m_Direction.x < 0.0f)
		{
			WeaponPosition.x -= g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsetx;
			GameClient()->m_Effects.PowerupShine(GameState, WeaponPosition + vec2(32.0f, 0.0f), vec2(32.0f, 12.0f), ClientId, 1.0f);
		}
		else
		{
			GameClient()->m_Effects.PowerupShine(GameState, WeaponPosition - vec2(32.0f, 0.0f), vec2(32.0f, 12.0f), ClientId, 1.0f);
		}
	}

	float TeeAnimScale, TeeBaseSize;
	CRenderTools::GetRenderTeeAnimScaleAndBaseSize(&State.m_RenderInfo, TeeAnimScale, TeeBaseSize);
	const vec2 BodyPos = State.m_Position + vec2(State.m_Animation.GetBody()->m_X, State.m_Animation.GetBody()->m_Y) * TeeAnimScale;
	if(State.m_RenderInfo.m_TeeRenderFlags & TEE_EFFECT_FROZEN)
		GameClient()->m_Effects.FreezingFlakes(GameState, BodyPos, vec2(32, 32), ClientId, 1.0f);
	if(State.m_RenderInfo.m_TeeRenderFlags & TEE_EFFECT_SPARKLE)
		GameClient()->m_Effects.SparkleTrail(GameState, BodyPos, ClientId, 1.0f);
}

void CPlayers::RenderPlayer(
	const CRenderContext &Context,
	const CScreenRect &ScreenRect,
	const CNetObj_Character *pPrevChar,
	const CNetObj_Character *pPlayerChar,
	const CTeeRenderInfo *pRenderInfo,
	int ClientId,
	float Intra)
{
	CPlayerRenderState RenderState;
	const CVisibleWorldRect VisibleWorldRect(ScreenRect.m_TopLeft, ScreenRect.m_BottomRight);
	if(!PreparePlayerRenderState(Context.m_Session, Context.m_State, Context.m_Time, Context.IsOtherTeam(ClientId), std::span<const CVisibleWorldRect>(&VisibleWorldRect, 1), vec2(0.0f, 0.0f), pPrevChar, pPlayerChar, pRenderInfo, ClientId, Intra, RenderState))
		return;

	CNetObj_Character &Prev = RenderState.m_Prev;
	CNetObj_Character &Player = RenderState.m_Player;
	CTeeRenderInfo &RenderInfo = RenderState.m_RenderInfo;
	CAnimState &State = RenderState.m_Animation;
	const vec2 Position = RenderState.m_Position;
	const vec2 Direction = RenderState.m_Direction;
	const float Angle = RenderState.m_Angle;
	Intra = RenderState.m_Intra;
	const float Alpha = RenderState.m_Alpha;
	const float AttackTime = RenderState.m_AttackTime;
	const float LastAttackTime = RenderState.m_LastAttackTime;
	const float AttackTicksPassed = RenderState.m_AttackTicksPassed;
	const bool Local = RenderState.m_Local;
	const bool PredictLocalWeapons = RenderState.m_PredictLocalWeapons;
	const bool Stationary = RenderState.m_Stationary;
	const bool InAir = RenderState.m_InAir;
	const bool Afk = RenderState.m_Afk;
	const bool Inactive = RenderState.m_Inactive;

	auto MuzzleVariant = [AttackTick = Player.m_AttackTick, ClientId](int Weapon, int NumMuzzles) {
		return static_cast<int>((static_cast<unsigned>(AttackTick) + static_cast<unsigned>(ClientId + 2) + static_cast<unsigned>(Weapon)) % static_cast<unsigned>(NumMuzzles));
	};

	const float HammerAnimationTimeScale = 5.0f;

	// draw gun
	if(Player.m_Weapon >= 0)
	{
		if(!(RenderInfo.m_TeeRenderFlags & TEE_NO_WEAPON))
		{
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);

			// normal weapons
			int CurrentWeapon = std::clamp(Player.m_Weapon, 0, NUM_WEAPONS - 1);
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[CurrentWeapon]);
			int QuadOffset = CurrentWeapon * 2 + (Direction.x < 0.0f ? 1 : 0);

			float Recoil = 0.0f;
			vec2 WeaponPosition;
			bool IsSit = Inactive && !InAir && Stationary;

			if(Player.m_Weapon == WEAPON_HAMMER)
			{
				// static position for hammer
				WeaponPosition = Position + vec2(State.GetAttach()->m_X, State.GetAttach()->m_Y);
				WeaponPosition.y += g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsety;
				if(Direction.x < 0.0f)
					WeaponPosition.x -= g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsetx;
				if(IsSit)
					WeaponPosition.y += 3.0f;

				// if active and attack is under way, bash stuffs
				if(!Inactive || LastAttackTime * HammerAnimationTimeScale < 1.0f)
				{
					if(Direction.x < 0)
						Graphics()->QuadsSetRotation(-pi / 2.0f - State.GetAttach()->m_Angle * pi * 2.0f);
					else
						Graphics()->QuadsSetRotation(-pi / 2.0f + State.GetAttach()->m_Angle * pi * 2.0f);
				}
				else
				{
					Graphics()->QuadsSetRotation(Direction.x < 0.0f ? 100.0f : 500.0f);
				}

				Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, WeaponPosition.x, WeaponPosition.y);
			}
			else if(Player.m_Weapon == WEAPON_NINJA)
			{
				WeaponPosition = Position;
				WeaponPosition.y += g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsety;
				if(IsSit)
					WeaponPosition.y += 3.0f;

				if(Direction.x < 0.0f)
				{
					Graphics()->QuadsSetRotation(-pi / 2 - State.GetAttach()->m_Angle * pi * 2.0f);
					WeaponPosition.x -= g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsetx;
				}
				else
				{
					Graphics()->QuadsSetRotation(-pi / 2 + State.GetAttach()->m_Angle * pi * 2.0f);
				}
				Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, WeaponPosition.x, WeaponPosition.y);

				// HADOKEN
				if(AttackTime <= 1.0f / 6.0f && g_pData->m_Weapons.m_aId[CurrentWeapon].m_NumSpriteMuzzles)
				{
					const int IteX = MuzzleVariant(CurrentWeapon, g_pData->m_Weapons.m_aId[CurrentWeapon].m_NumSpriteMuzzles);
					if(g_pData->m_Weapons.m_aId[CurrentWeapon].m_aSpriteMuzzles[IteX])
					{
						vec2 HadokenDirection;
						if(PredictLocalWeapons || ClientId < 0)
							HadokenDirection = vec2(pPlayerChar->m_X, pPlayerChar->m_Y) - vec2(pPrevChar->m_X, pPrevChar->m_Y);
						else
						{
							const CGameState::CClientSnapshot &SnapshotClient = Context.m_State.Client(ClientId);
							HadokenDirection = vec2(SnapshotClient.m_Character.m_X, SnapshotClient.m_Character.m_Y) - vec2(SnapshotClient.m_PrevCharacter.m_X, SnapshotClient.m_PrevCharacter.m_Y);
						}
						float HadokenAngle = 0.0f;
						if(absolute(HadokenDirection.x) > 0.0001f || absolute(HadokenDirection.y) > 0.0001f)
						{
							HadokenDirection = normalize(HadokenDirection);
							HadokenAngle = angle(HadokenDirection);
						}
						else
						{
							HadokenDirection = vec2(1.0f, 0.0f);
						}
						Graphics()->QuadsSetRotation(HadokenAngle);
						QuadOffset = IteX * 2;
						WeaponPosition = Position;
						float OffsetX = g_pData->m_Weapons.m_aId[CurrentWeapon].m_Muzzleoffsetx;
						WeaponPosition -= HadokenDirection * OffsetX;
						Graphics()->TextureSet(GameClient()->m_GameSkin.m_aaSpriteWeaponsMuzzles[CurrentWeapon][IteX]);
						Graphics()->RenderQuadContainerAsSprite(m_aWeaponSpriteMuzzleQuadContainerIndex[CurrentWeapon], QuadOffset, WeaponPosition.x, WeaponPosition.y);
					}
				}
			}
			else
			{
				// TODO: should be an animation
				Recoil = 0.0f;
				float a = AttackTicksPassed / 5.0f;
				if(a < 1.0f)
					Recoil = std::sin(a * pi);
				WeaponPosition = Position + Direction * g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsetx - Direction * Recoil * 10.0f;
				WeaponPosition.y += g_pData->m_Weapons.m_aId[CurrentWeapon].m_Offsety;
				if(IsSit)
					WeaponPosition.y += 3.0f;
				if(Player.m_Weapon == WEAPON_GUN && g_Config.m_ClOldGunPosition)
					WeaponPosition.y -= 8.0f;
				Graphics()->QuadsSetRotation(State.GetAttach()->m_Angle * pi * 2.0f + Angle);
				Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, WeaponPosition.x, WeaponPosition.y);
			}

			if(Player.m_Weapon == WEAPON_GUN || Player.m_Weapon == WEAPON_SHOTGUN)
			{
				// check if we're firing stuff
				if(g_pData->m_Weapons.m_aId[CurrentWeapon].m_NumSpriteMuzzles) // prev.attackticks)
				{
					float AlphaMuzzle = 0.0f;
					if(AttackTicksPassed < g_pData->m_Weapons.m_aId[CurrentWeapon].m_Muzzleduration + 3.0f)
					{
						float t = AttackTicksPassed / g_pData->m_Weapons.m_aId[CurrentWeapon].m_Muzzleduration;
						AlphaMuzzle = mix(2.0f, 0.0f, std::clamp(t, 0.0f, 1.0f));
					}

					const int IteX = MuzzleVariant(CurrentWeapon, g_pData->m_Weapons.m_aId[CurrentWeapon].m_NumSpriteMuzzles);
					if(AlphaMuzzle > 0.0f && g_pData->m_Weapons.m_aId[CurrentWeapon].m_aSpriteMuzzles[IteX])
					{
						float OffsetY = -g_pData->m_Weapons.m_aId[CurrentWeapon].m_Muzzleoffsety;
						QuadOffset = IteX * 2 + (Direction.x < 0.0f ? 1 : 0);
						if(Direction.x < 0.0f)
							OffsetY = -OffsetY;

						vec2 DirectionY(-Direction.y, Direction.x);
						vec2 MuzzlePos = WeaponPosition + Direction * g_pData->m_Weapons.m_aId[CurrentWeapon].m_Muzzleoffsetx + DirectionY * OffsetY;
						Graphics()->TextureSet(GameClient()->m_GameSkin.m_aaSpriteWeaponsMuzzles[CurrentWeapon][IteX]);
						Graphics()->RenderQuadContainerAsSprite(m_aWeaponSpriteMuzzleQuadContainerIndex[CurrentWeapon], QuadOffset, MuzzlePos.x, MuzzlePos.y);
					}
				}
			}
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			Graphics()->QuadsSetRotation(0.0f);

			switch(Player.m_Weapon)
			{
			case WEAPON_GUN: RenderHand(&RenderInfo, WeaponPosition, Direction, -3.0f * pi / 4.0f, vec2(-15.0f, 4.0f), Alpha); break;
			case WEAPON_SHOTGUN: RenderHand(&RenderInfo, WeaponPosition, Direction, -pi / 2.0f, vec2(-5.0f, 4.0f), Alpha); break;
			case WEAPON_GRENADE: RenderHand(&RenderInfo, WeaponPosition, Direction, -pi / 2.0f, vec2(-4.0f, 7.0f), Alpha); break;
			}
		}
	}

	// render the "shadow" tee
	if(g_Config.m_ClUnpredictedShadow == 3 || (Local && g_Config.m_ClUnpredictedShadow == 1) || (!Local && g_Config.m_ClUnpredictedShadow == 2))
	{
		vec2 ShadowPosition = Position;
		if(ClientId >= 0)
		{
			const CGameState::CClientSnapshot &SnapshotClient = Context.m_State.Client(ClientId);
			ShadowPosition = mix(
				vec2(SnapshotClient.m_PrevCharacter.m_X, SnapshotClient.m_PrevCharacter.m_Y),
				vec2(SnapshotClient.m_Character.m_X, SnapshotClient.m_Character.m_Y),
				Context.m_Time.m_IntraGameTick);
		}

		RenderTools()->RenderTee(&State, &RenderInfo, Player.m_Emote, Direction, ShadowPosition, g_Config.m_ClUnpredictedShadowAlpha / 100.f); // render ghost
	}

	RenderTools()->RenderTee(&State, &RenderInfo, Player.m_Emote, Direction, Position, Alpha);

	if(ClientId < 0)
		return;

	int QuadOffsetToEmoticon = NUM_WEAPONS * 2 + 2 + 2;
	if((Player.m_PlayerFlags & PLAYERFLAG_CHATTING) && !Afk)
	{
		int CurEmoticon = (SPRITE_DOTDOT - SPRITE_OOP);
		Graphics()->TextureSet(GameClient()->m_EmoticonsSkin.m_aSpriteEmoticons[CurEmoticon]);
		int QuadOffset = QuadOffsetToEmoticon + CurEmoticon;
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
		Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, Position.x + 24.f, Position.y - 40.f);

		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		Graphics()->QuadsSetRotation(0);
	}

	bool SessionLocal = false;
	for(const auto &pState : Context.m_Session.GameStates().States())
		SessionLocal |= pState->LocalClientId() == ClientId;
	if(g_Config.m_ClAfkEmote && Afk && !SessionLocal)
	{
		int CurEmoticon = (SPRITE_ZZZ - SPRITE_OOP);
		Graphics()->TextureSet(GameClient()->m_EmoticonsSkin.m_aSpriteEmoticons[CurEmoticon]);
		int QuadOffset = QuadOffsetToEmoticon + CurEmoticon;
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
		Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, Position.x + 24.f, Position.y - 40.f);

		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		Graphics()->QuadsSetRotation(0);
	}

	if(g_Config.m_ClShowEmotes && !GameClient()->m_aClients[ClientId].m_EmoticonIgnore && GameClient()->m_aClients[ClientId].m_EmoticonStartTick != -1)
	{
		float SinceStart = (Context.m_Time.m_GameTick - GameClient()->m_aClients[ClientId].m_EmoticonStartTick) + (Context.m_Time.m_IntraGameTickSincePrev - GameClient()->m_aClients[ClientId].m_EmoticonStartFraction);
		float FromEnd = (2 * Context.m_Time.m_GameTickSpeed) - SinceStart;

		if(0 <= SinceStart && FromEnd > 0)
		{
			float a = 1;

			if(FromEnd < Context.m_Time.m_GameTickSpeed / 5)
				a = FromEnd / (Context.m_Time.m_GameTickSpeed / 5.0f);

			float h = 1;
			if(SinceStart < Context.m_Time.m_GameTickSpeed / 10)
				h = SinceStart / (Context.m_Time.m_GameTickSpeed / 10.0f);

			float Wiggle = 0;
			if(SinceStart < Context.m_Time.m_GameTickSpeed / 5)
				Wiggle = SinceStart / (Context.m_Time.m_GameTickSpeed / 5.0f);

			float WiggleAngle = std::sin(5 * Wiggle);

			Graphics()->QuadsSetRotation(pi / 6 * WiggleAngle);

			Graphics()->SetColor(1.0f, 1.0f, 1.0f, a * Alpha);
			// client_datas::emoticon is an offset from the first emoticon
			int QuadOffset = QuadOffsetToEmoticon + GameClient()->m_aClients[ClientId].m_Emoticon;
			Graphics()->TextureSet(GameClient()->m_EmoticonsSkin.m_aSpriteEmoticons[GameClient()->m_aClients[ClientId].m_Emoticon]);
			Graphics()->RenderQuadContainerAsSprite(m_WeaponEmoteQuadContainerIndex, QuadOffset, Position.x, Position.y - 23.f - 32.f * h, 1.f, (64.f * h) / 64.f);

			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			Graphics()->QuadsSetRotation(0);
		}
	}
}

inline bool CPlayers::IsPlayerInfoAvailable(const CGameState &GameState, int ClientId) const
{
	const CGameState::CClientSnapshot &SnapshotClient = GameState.Client(ClientId);
	return GameState.RenderedClient(ClientId).m_Active && SnapshotClient.m_HasPlayerInfo && SnapshotClient.m_HasPrevPlayerInfo;
}

void CPlayers::UpdatePresentation(const CPresentationContext &Context)
{
	if(Context.m_Playback == EPresentationPlayback::PAUSED)
		return;

	CGameState &State = Context.m_State;
	const bool IsTeamPlay = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	CTeeRenderInfo aRenderInfo[MAX_CLIENTS];
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		PrepareRenderInfo(Context.m_Session, State, ClientId, IsTeamPlay, aRenderInfo[ClientId]);

	const int RenderLastId = State.LocalClientId();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == RenderLastId || !IsPlayerInfoAvailable(State, ClientId))
			continue;
		const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(ClientId);
		UpdatePlayerPresentation(Context, &RenderedClient.m_Prev, &RenderedClient.m_Cur, &aRenderInfo[ClientId], ClientId);
	}
	if(RenderLastId != -1 && IsPlayerInfoAvailable(State, RenderLastId))
	{
		const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(RenderLastId);
		UpdatePlayerPresentation(Context, &RenderedClient.m_Prev, &RenderedClient.m_Cur, &aRenderInfo[RenderLastId], RenderLastId);
	}
}

void CPlayers::OnRender(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive)
		return;

	// update render info for ninja
	CTeeRenderInfo aRenderInfo[MAX_CLIENTS];
	const CGameState &State = Context.m_State;
	const bool IsTeamPlay = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	for(int i = 0; i < MAX_CLIENTS; ++i)
		PrepareRenderInfo(Context.m_Session, State, i, IsTeamPlay, aRenderInfo[i]);

	// get screen edges to avoid rendering offscreen
	CScreenRect ScreenRect(Context.m_VisibleWorldRect.m_TopLeft, Context.m_VisibleWorldRect.m_BottomRight);
	// expand the edges to prevent popping in/out onscreen
	//
	// it is assumed that the tee, all its weapons, and emotes fit into a 200x200 box centered on the tee
	// this may need to be changed or calculated differently in the future
	constexpr float PlayerBorderBuffer = 100.0f;
	ScreenRect.Expand(PlayerBorderBuffer);

	// render everyone else's hook, then our own
	const int LocalClientId = State.LocalClientId();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(ClientId == LocalClientId || !IsPlayerInfoAvailable(State, ClientId))
		{
			continue;
		}
		const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(ClientId);
		RenderHook(Context, ScreenRect, &RenderedClient.m_Prev, &RenderedClient.m_Cur, &aRenderInfo[ClientId], ClientId);
	}
	if(LocalClientId != -1 && IsPlayerInfoAvailable(State, LocalClientId))
	{
		const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(LocalClientId);
		RenderHook(Context, ScreenRect, &RenderedClient.m_Prev, &RenderedClient.m_Cur, &aRenderInfo[LocalClientId], LocalClientId);
	}

	RenderSpectatorCharacters(Context, ScreenRect);

	// render everyone else's tee, then either our own or the tee we are spectating.
	const int RenderLastId = Context.m_View.IsSpectating() && Context.m_View.SpectatorId() != SPEC_FREEVIEW ? Context.m_View.SpectatorId() : LocalClientId;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(ClientId == RenderLastId || !IsPlayerInfoAvailable(State, ClientId))
		{
			continue;
		}

		const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(ClientId);
		RenderHookCollLine(Context, ScreenRect, &RenderedClient.m_Prev, &RenderedClient.m_Cur, ClientId);
		RenderPlayer(Context, ScreenRect, &RenderedClient.m_Prev, &RenderedClient.m_Cur, &aRenderInfo[ClientId], ClientId);
	}
	if(RenderLastId != -1 && IsPlayerInfoAvailable(State, RenderLastId))
	{
		const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(RenderLastId);
		RenderHookCollLine(Context, ScreenRect, &RenderedClient.m_Prev, &RenderedClient.m_Cur, RenderLastId);
		RenderPlayer(Context, ScreenRect, &RenderedClient.m_Prev, &RenderedClient.m_Cur, &aRenderInfo[RenderLastId], RenderLastId);
	}
}

void CPlayers::CreateNinjaTeeRenderInfo()
{
	CTeeRenderInfo NinjaTeeRenderInfo;
	NinjaTeeRenderInfo.m_Size = 64.0f;
	CSkinDescriptor NinjaSkinDescriptor;
	NinjaSkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SIX;
	str_copy(NinjaSkinDescriptor.m_aSkinName, "x_ninja");
	m_pNinjaTeeRenderInfo = GameClient()->CreateManagedTeeRenderInfo(NinjaTeeRenderInfo, NinjaSkinDescriptor);
}

void CPlayers::CreateSpectatorTeeRenderInfo()
{
	CTeeRenderInfo SpectatorTeeRenderInfo;
	SpectatorTeeRenderInfo.m_Size = 64.0f;
	CSkinDescriptor SpectatorSkinDescriptor;
	SpectatorSkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SIX;
	str_copy(SpectatorSkinDescriptor.m_aSkinName, "x_spec");
	m_pSpectatorTeeRenderInfo = GameClient()->CreateManagedTeeRenderInfo(SpectatorTeeRenderInfo, SpectatorSkinDescriptor);
}

void CPlayers::OnInit()
{
	m_WeaponEmoteQuadContainerIndex = Graphics()->CreateQuadContainer(false);

	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_pSpriteBody, ScaleX, ScaleY);
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleX, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleY);
		Graphics()->QuadsSetSubset(0, 1, 1, 0);
		Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleX, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleY);
	}
	float ScaleX, ScaleY;

	// at the end the hand
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, 20.f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, 20.f);

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, -12.f, -8.f, 24.f, 16.f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, -12.f, -8.f, 24.f, 16.f);

	for(int i = 0; i < NUM_EMOTICONS; ++i)
	{
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->QuadContainerAddSprite(m_WeaponEmoteQuadContainerIndex, 64.f);
	}
	Graphics()->QuadContainerUpload(m_WeaponEmoteQuadContainerIndex);

	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		m_aWeaponSpriteMuzzleQuadContainerIndex[i] = Graphics()->CreateQuadContainer(false);
		for(int n = 0; n < g_pData->m_Weapons.m_aId[i].m_NumSpriteMuzzles; ++n)
		{
			if(g_pData->m_Weapons.m_aId[i].m_aSpriteMuzzles[n])
			{
				if(i == WEAPON_GUN || i == WEAPON_SHOTGUN)
				{
					// TODO: hardcoded for now to get the same particle size as before
					Graphics()->GetSpriteScaleImpl(96, 64, ScaleX, ScaleY);
				}
				else
				{
					Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_aSpriteMuzzles[n], ScaleX, ScaleY);
				}
			}

			float SWidth = (g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleX) * (4.0f / 3.0f);
			float SHeight = g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleY;

			Graphics()->QuadsSetSubset(0, 0, 1, 1);
			if(WEAPON_NINJA == i)
				Graphics()->QuadContainerAddSprite(m_aWeaponSpriteMuzzleQuadContainerIndex[i], 160.f * ScaleX, 160.f * ScaleY);
			else
				Graphics()->QuadContainerAddSprite(m_aWeaponSpriteMuzzleQuadContainerIndex[i], SWidth, SHeight);

			Graphics()->QuadsSetSubset(0, 1, 1, 0);
			if(WEAPON_NINJA == i)
				Graphics()->QuadContainerAddSprite(m_aWeaponSpriteMuzzleQuadContainerIndex[i], 160.f * ScaleX, 160.f * ScaleY);
			else
				Graphics()->QuadContainerAddSprite(m_aWeaponSpriteMuzzleQuadContainerIndex[i], SWidth, SHeight);
		}
		Graphics()->QuadContainerUpload(m_aWeaponSpriteMuzzleQuadContainerIndex[i]);
	}

	Graphics()->QuadsSetSubset(0.f, 0.f, 1.f, 1.f);
	Graphics()->QuadsSetRotation(0.f);

	CreateNinjaTeeRenderInfo();
	CreateSpectatorTeeRenderInfo();
}
