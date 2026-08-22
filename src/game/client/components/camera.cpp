/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "camera.h"

#include "controls.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/vmath.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <limits>

CCamera::CCamera()
{
	m_aAutoSpecCameraTooltip[0] = '\0';
}

CGameView::CCameraState &CCamera::State()
{
	dbg_assert(m_pState != nullptr, "camera state not bound");
	return *m_pState;
}

const CGameView::CCameraState &CCamera::State() const
{
	dbg_assert(m_pState != nullptr, "camera state not bound");
	return *m_pState;
}

float CCamera::CameraSmoothingProgress(float CurrentTime) const
{
	float Progress = (CurrentTime - State().m_CameraSmoothingStart) / (State().m_CameraSmoothingEnd - State().m_CameraSmoothingStart);
	return 1.0 - std::pow(2.0, -10.0 * Progress);
}

float CCamera::ZoomProgress(float CurrentTime) const
{
	return (CurrentTime - State().m_ZoomSmoothingStart) / (State().m_ZoomSmoothingEnd - State().m_ZoomSmoothingStart);
}

void CCamera::ScaleZoom(float Factor)
{
	float CurrentTarget = State().m_Zooming ? State().m_ZoomSmoothingTarget : State().m_Zoom;
	ChangeZoom(CurrentTarget * Factor, GameClient()->Snap().m_SpecInfo.m_Active && GameClient()->MultiView().m_Active ? g_Config.m_ClMultiViewZoomSmoothness : g_Config.m_ClSmoothZoomTime, true);

	State().m_AutoSpecCamera = false;
}

float CCamera::MaxZoomLevel()
{
	return (g_Config.m_ClLimitMaxZoomLevel) ? ((Graphics()->IsTileBufferingEnabled() ? 240 : 30)) : std::numeric_limits<float>::max();
}

float CCamera::MinZoomLevel()
{
	return 0.01f;
}

void CCamera::ChangeZoom(float Target, int Smoothness, bool IsUser)
{
	if(Target > MaxZoomLevel() || Target < MinZoomLevel())
	{
		return;
	}

	float Now = Client()->LocalTime();
	float Current = State().m_Zoom;
	float Derivative = 0.0f;
	if(State().m_Zooming)
	{
		float Progress = ZoomProgress(Now);
		Current = State().m_ZoomSmoothing.Evaluate(Progress);
		Derivative = State().m_ZoomSmoothing.Derivative(Progress);
	}

	State().m_ZoomSmoothingTarget = Target;
	State().m_ZoomSmoothing = CCubicBezier::With(Current, Derivative, 0, State().m_ZoomSmoothingTarget);
	State().m_ZoomSmoothingStart = Now;
	State().m_ZoomSmoothingEnd = Now + (float)Smoothness / 1000;

	if(IsUser)
		State().m_UserZoomTarget = Target;

	State().m_Zooming = true;
}

void CCamera::ResetAutoSpecCamera()
{
	State().m_AutoSpecCamera = true;
}

void CCamera::UpdateCamera()
{
	// use hardcoded smooth camera for spectating unless player explicitly turn it off
	bool CanUseCameraInfo = !GameClient()->MultiView().m_Active;
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		// only follow mode have the correct camera info
		CanUseCameraInfo = CanUseCameraInfo && GameClient()->m_DemoSpecId == SPEC_FOLLOW;
	}
	else
	{
		CanUseCameraInfo = CanUseCameraInfo && GameClient()->Snap().m_SpecInfo.m_Active &&
				   GameClient()->Snap().m_SpecInfo.m_SpectatorId >= 0 &&
				   GameClient()->Snap().m_SpecInfo.m_SpectatorId != GameClient()->GameState(IClient::CONN_MAIN).LocalClientId() &&
				   (!GameClient()->Client()->DummyConnected() || GameClient()->Snap().m_SpecInfo.m_SpectatorId != GameClient()->GameState(IClient::CONN_DUMMY).LocalClientId());
	}

	const bool CanUseAutoSpecCamera = this->CanUseAutoSpecCamera();
	bool UsingAutoSpecCamera = State().m_AutoSpecCamera && CanUseAutoSpecCamera;
	float CurrentZoom = State().m_Zooming ? State().m_ZoomSmoothingTarget : State().m_Zoom;
	bool ZoomChanged = false;
	if(CanUseCameraInfo && UsingAutoSpecCamera && CurrentZoom != GameClient()->Snap().m_SpecInfo.m_Zoom)
	{
		// start spectating player / turn on auto spec camera
		bool ChangeTarget = State().m_PrevSpecId != GameClient()->Snap().m_SpecInfo.m_SpectatorId;
		float SmoothTime = ChangeTarget ? g_Config.m_ClSmoothSpectatingTime : 250;
		ChangeZoom(GameClient()->Snap().m_SpecInfo.m_Zoom, SmoothTime, false);

		// it is auto spec camera zooming if only the zoom is changed during activation, not at the start of the activation
		State().m_AutoSpecCameraZooming = !ChangeTarget && CanUseCameraInfo && State().m_UsingAutoSpecCamera;

		ZoomChanged = true;
	}
	else if((CanUseCameraInfo && !UsingAutoSpecCamera) && CurrentZoom != State().m_UserZoomTarget)
	{
		// turning off auto spec camera
		ChangeZoom(State().m_UserZoomTarget, g_Config.m_ClSmoothZoomTime, false);
		State().m_AutoSpecCameraZooming = false;

		ZoomChanged = true;
	}
	else if(!CanUseCameraInfo && CurrentZoom != State().m_UserZoomTarget)
	{
		// stop spectating player
		if(!GameClient()->MultiView().m_Active)
			ChangeZoom(State().m_UserZoomTarget, g_Config.m_ClSmoothZoomTime, false);
		State().m_AutoSpecCameraZooming = false;

		ZoomChanged = true;
	}

	// snap zoom when going in and out of spectating
	if(ZoomChanged && State().m_WasSpectating != GameClient()->Snap().m_SpecInfo.m_Active)
	{
		State().m_Zoom = State().m_ZoomSmoothingTarget;
		State().m_Zooming = false;
	}

	if(State().m_Zooming)
	{
		float Time = Client()->LocalTime();
		if(Time >= State().m_ZoomSmoothingEnd)
		{
			State().m_Zoom = State().m_ZoomSmoothingTarget;
			State().m_Zooming = false;
			State().m_AutoSpecCameraZooming = false;
		}
		else
		{
			const float OldLevel = State().m_Zoom;
			State().m_Zoom = State().m_ZoomSmoothing.Evaluate(ZoomProgress(Time));
			if((OldLevel < State().m_ZoomSmoothingTarget && State().m_Zoom > State().m_ZoomSmoothingTarget) || (OldLevel > State().m_ZoomSmoothingTarget && State().m_Zoom < State().m_ZoomSmoothingTarget))
			{
				State().m_Zoom = State().m_ZoomSmoothingTarget;
				State().m_Zooming = false;
				State().m_AutoSpecCameraZooming = false;
			}
		}
		State().m_Zoom = std::clamp(State().m_Zoom, MinZoomLevel(), MaxZoomLevel());
	}

	if(!ZoomAllowed())
	{
		State().m_ZoomSet = false;
		State().m_Zoom = 1.0f;
		State().m_Zooming = false;
		State().m_AutoSpecCameraZooming = false;
	}
	else if(!State().m_ZoomSet && g_Config.m_ClDefaultZoom != 10)
	{
		State().m_ZoomSet = true;
		OnReset();
	}

	if(GameClient()->Snap().m_SpecInfo.m_Active && !GameClient()->Snap().m_SpecInfo.m_UsePosition)
	{
		State().m_DynamicCameraOffset = vec2(0, 0);
		State().m_CanUseCameraInfo = CanUseCameraInfo;
		State().m_CanUseAutoSpecCamera = CanUseAutoSpecCamera;
		State().m_UsingAutoSpecCamera = UsingAutoSpecCamera;
		return;
	}

	vec2 TargetPos = CanUseCameraInfo ? GameClient()->LegacyGameView().SpectatorCursor().Target() : GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MousePos;
	int Smoothness = CanUseCameraInfo ? 50 : g_Config.m_ClDyncamSmoothness;
	int Stabilizing = CanUseCameraInfo ? 50 : g_Config.m_ClDyncamStabilizing;
	bool IsDyncam = CanUseCameraInfo ? true : g_Config.m_ClDyncam;

	float DeltaTime = Client()->RenderFrameTime();

	if(Smoothness > 0)
	{
		float CameraSpeed = (1.0f - (Smoothness / 100.0f)) * 9.5f + 0.5f;
		float CameraStabilizingFactor = 1 + Stabilizing / 100.0f;

		State().m_DyncamSmoothingSpeedBias += CameraSpeed * DeltaTime;
		if(IsDyncam)
		{
			State().m_DyncamSmoothingSpeedBias -= length(TargetPos - State().m_LastTargetPos) * std::log10(CameraStabilizingFactor) * 0.02f;
			State().m_DyncamSmoothingSpeedBias = std::clamp(State().m_DyncamSmoothingSpeedBias, 0.5f, CameraSpeed);
		}
		else
		{
			State().m_DyncamSmoothingSpeedBias = std::max(5.0f, CameraSpeed); // make sure toggle back is fast
		}
	}

	State().m_DyncamTargetCameraOffset = vec2(0, 0);
	float l = length(TargetPos);
	if(l > 0.0001f) // make sure that this isn't 0
	{
		float CurrentDeadzone = Deadzone();
		float CurrentFollowFactor = FollowFactor();

		// use provided camera setting from server
		if(CanUseCameraInfo)
		{
			CurrentDeadzone = GameClient()->Snap().m_SpecInfo.m_Deadzone;
			CurrentFollowFactor = GameClient()->Snap().m_SpecInfo.m_FollowFactor;

			if(!UsingAutoSpecCamera)
			{
				// turn off dyncam if user zooms when spectating
				CurrentDeadzone = 0;
				CurrentFollowFactor = 0;
			}
		}

		float OffsetAmount = std::max(l - CurrentDeadzone, 0.0f) * (CurrentFollowFactor / 100.0f);

		if(CanUseCameraInfo)
		{
			OffsetAmount = std::min(OffsetAmount, 350.0f * State().m_Zoom);
		}

		State().m_DyncamTargetCameraOffset = normalize_pre_length(TargetPos, l) * OffsetAmount;
	}

	State().m_LastTargetPos = TargetPos;
	vec2 CurrentCameraOffset = State().m_DynamicCameraOffset;
	float SpeedBias = State().m_CameraSmoothing ? 50.0f : State().m_DyncamSmoothingSpeedBias;
	if(Smoothness > 0)
		CurrentCameraOffset += (State().m_DyncamTargetCameraOffset - CurrentCameraOffset) * std::min(DeltaTime * SpeedBias, 1.0f);
	else
		CurrentCameraOffset = State().m_DyncamTargetCameraOffset;

	// directly put the camera in place when switching in and out of freeview or spectate mode
	if(State().m_CanUseCameraInfo != CanUseCameraInfo)
	{
		CurrentCameraOffset = State().m_DyncamTargetCameraOffset;
	}

	State().m_DynamicCameraOffset = CurrentCameraOffset;
	State().m_CanUseCameraInfo = CanUseCameraInfo;
	State().m_CanUseAutoSpecCamera = CanUseAutoSpecCamera;
	State().m_UsingAutoSpecCamera = UsingAutoSpecCamera;
}

void CCamera::UpdatePosition()
{
	if(State().m_CameraSmoothing)
	{
		if(!GameClient()->Snap().m_SpecInfo.m_Active)
		{
			State().m_Center = State().m_CameraSmoothingTarget;
			State().m_CameraSmoothing = false;
		}
		else
		{
			float Time = Client()->LocalTime();
			if(Time >= State().m_CameraSmoothingEnd)
			{
				State().m_Center = State().m_CameraSmoothingTarget;
				State().m_CameraSmoothing = false;
			}
			else
			{
				State().m_CameraSmoothingCenter = vec2(State().m_CameraSmoothingBezierX.Evaluate(CameraSmoothingProgress(Time)), State().m_CameraSmoothingBezierY.Evaluate(CameraSmoothingProgress(Time)));
				if(distance(State().m_CameraSmoothingCenter, State().m_CameraSmoothingTarget) <= 0.1f)
				{
					State().m_Center = State().m_CameraSmoothingTarget;
					State().m_CameraSmoothing = false;
				}
			}
		}
	}

	// update camera center
	if(GameClient()->Snap().m_SpecInfo.m_Active && !GameClient()->Snap().m_SpecInfo.m_UsePosition)
	{
		if(State().m_CamType != CAMTYPE_SPEC)
		{
			State().m_LastInputPosition = GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MousePos;
			GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MousePos = State().m_PrevCenter;
			GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MouseInputType = CGameState::EMouseInputType::AUTOMATED;
			GameClient()->m_Controls.ClampMousePos();
			State().m_CamType = CAMTYPE_SPEC;
		}
		State().m_Center = GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MousePos;
	}
	else
	{
		if(State().m_CamType != CAMTYPE_PLAYER)
		{
			GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MousePos = State().m_LastInputPosition;
			GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MouseInputType = CGameState::EMouseInputType::AUTOMATED;
			GameClient()->m_Controls.ClampMousePos();
			State().m_CamType = CAMTYPE_PLAYER;
		}

		if(GameClient()->Snap().m_SpecInfo.m_Active)
			State().m_Center = GameClient()->Snap().m_SpecInfo.m_Position + State().m_DynamicCameraOffset;
		else
			State().m_Center = GameClient()->m_LocalCharacterPos + State().m_DynamicCameraOffset;
	}

	if(State().m_ForceFreeview && State().m_CamType == CAMTYPE_SPEC)
	{
		GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MouseInputType = CGameState::EMouseInputType::AUTOMATED;
		State().m_Center = GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_MousePos = State().m_ForceFreeviewPos;
		State().m_ForceFreeview = false;
	}
	else
	{
		State().m_ForceFreeviewPos = State().m_Center;
	}

	const int SpecId = GameClient()->Snap().m_SpecInfo.m_SpectatorId;

	// start smoothing from the current position when the target changes
	if(State().m_CameraSmoothing && SpecId != State().m_PrevSpecId)
		State().m_CameraSmoothing = false;

	if(GameClient()->Snap().m_SpecInfo.m_Active &&
		(SpecId != State().m_PrevSpecId ||
			(State().m_CameraSmoothing && State().m_CameraSmoothingTarget != State().m_Center)) && // the target is moving during camera smoothing
		!(!State().m_WasSpectating && State().m_Center != State().m_PrevCenter) && // dont smooth when starting to spectate
		State().m_CamType != CAMTYPE_SPEC &&
		!GameClient()->MultiView().m_Active)
	{
		float Now = Client()->LocalTime();
		if(!State().m_CameraSmoothing)
			State().m_CenterBeforeSmoothing = State().m_PrevCenter;

		vec2 Derivative = {0.f, 0.f};
		if(State().m_CameraSmoothing)
		{
			float Progress = CameraSmoothingProgress(Now);
			Derivative.x = State().m_CameraSmoothingBezierX.Derivative(Progress);
			Derivative.y = State().m_CameraSmoothingBezierY.Derivative(Progress);
		}

		State().m_CameraSmoothingTarget = State().m_Center;
		State().m_CameraSmoothingBezierX = CCubicBezier::With(State().m_CenterBeforeSmoothing.x, Derivative.x, 0, State().m_CameraSmoothingTarget.x);
		State().m_CameraSmoothingBezierY = CCubicBezier::With(State().m_CenterBeforeSmoothing.y, Derivative.y, 0, State().m_CameraSmoothingTarget.y);

		if(!State().m_CameraSmoothing)
		{
			State().m_CameraSmoothingStart = Now;
			State().m_CameraSmoothingEnd = Now + (float)g_Config.m_ClSmoothSpectatingTime / 1000.0f;
		}

		if(!State().m_CameraSmoothing)
			State().m_CameraSmoothingCenter = State().m_PrevCenter;

		State().m_CameraSmoothing = true;
	}

	if(State().m_CameraSmoothing)
		State().m_Center = State().m_CameraSmoothingCenter;

	State().m_PrevCenter = State().m_Center;
	State().m_PrevSpecId = SpecId;

	// demo always count as spectating
	State().m_WasSpectating = GameClient()->Snap().m_SpecInfo.m_Active;
}

void CCamera::OnConsoleInit()
{
	Console()->Register("zoom+", "?f[amount]", CFGFLAG_CLIENT, ConZoomPlus, this, "Zoom increase");
	Console()->Register("zoom-", "?f[amount]", CFGFLAG_CLIENT, ConZoomMinus, this, "Zoom decrease");
	Console()->Register("zoom", "?f", CFGFLAG_CLIENT, ConZoom, this, "Change zoom");
	Console()->Register("set_view", "i[x]i[y]", CFGFLAG_CLIENT, ConSetView, this, "Set camera position to x and y in the map");
	Console()->Register("set_view_relative", "i[x]i[y]", CFGFLAG_CLIENT, ConSetViewRelative, this, "Set camera position relative to current view in the map");
	Console()->Register("goto_switch", "i[number]?i[offset]", CFGFLAG_CLIENT, ConGotoSwitch, this, "View switch found (at offset) with given number");
	Console()->Register("goto_tele", "i[number]?i[offset]", CFGFLAG_CLIENT, ConGotoTele, this, "View tele found (at offset) with given number");
}

void CCamera::OnReset()
{
	State().m_CameraSmoothing = false;

	State().m_Zoom = CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10);
	State().m_Zooming = false;
	State().m_AutoSpecCameraZooming = false;
	State().m_UserZoomTarget = CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10);
}

void CCamera::ConZoomPlus(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	if(!pSelf->ZoomAllowed())
		return;

	float ZoomAmount = pResult->NumArguments() ? pResult->GetFloat(0) : 1.0f;

	pSelf->ScaleZoom(CCamera::ZoomStepsToValue(ZoomAmount));

	if(pSelf->GameClient()->MultiView().m_Active)
		pSelf->GameClient()->MultiView().m_PersonalZoom += ZoomAmount;
}
void CCamera::ConZoomMinus(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	if(!pSelf->ZoomAllowed())
		return;

	float ZoomAmount = pResult->NumArguments() ? pResult->GetFloat(0) : 1.0f;
	ZoomAmount *= -1.0f;

	pSelf->ScaleZoom(CCamera::ZoomStepsToValue(ZoomAmount));

	if(pSelf->GameClient()->MultiView().m_Active)
		pSelf->GameClient()->MultiView().m_PersonalZoom += ZoomAmount;
}
void CCamera::ConZoom(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	if(!pSelf->ZoomAllowed())
		return;

	bool IsReset = !pResult->NumArguments();

	float TargetLevel = !IsReset ? pResult->GetFloat(0) : g_Config.m_ClDefaultZoom;

	if(!pSelf->CanUseAutoSpecCamera() || !pSelf->State().m_CanUseCameraInfo)
		pSelf->ChangeZoom(CCamera::ZoomStepsToValue(TargetLevel - 10.0f), pSelf->GameClient()->Snap().m_SpecInfo.m_Active && pSelf->GameClient()->MultiView().m_Active ? g_Config.m_ClMultiViewZoomSmoothness : g_Config.m_ClSmoothZoomTime, true);
	else
		pSelf->State().m_UserZoomTarget = CCamera::ZoomStepsToValue(TargetLevel - 10.0f);

	pSelf->State().m_AutoSpecCamera = IsReset;

	if(pSelf->GameClient()->MultiView().m_Active && pSelf->GameClient()->Snap().m_SpecInfo.m_Active)
		pSelf->GameClient()->MultiView().m_PersonalZoom = TargetLevel - 10.0f;
}
void CCamera::ConSetView(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	// wait until free view camera type to update the position
	pSelf->SetView(ivec2(pResult->GetInteger(0), pResult->GetInteger(1)));
}
void CCamera::ConSetViewRelative(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	// wait until free view camera type to update the position
	pSelf->SetView(ivec2(pResult->GetInteger(0), pResult->GetInteger(1)), true);
}
void CCamera::ConGotoSwitch(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	pSelf->GotoSwitch(pResult->GetInteger(0), pResult->NumArguments() > 1 ? pResult->GetInteger(1) : -1);
}
void CCamera::ConGotoTele(IConsole::IResult *pResult, void *pUserData)
{
	CCamera *pSelf = (CCamera *)pUserData;
	pSelf->GotoTele(pResult->GetInteger(0), pResult->NumArguments() > 1 ? pResult->GetInteger(1) : -1);
}

void CCamera::SetView(ivec2 Pos, bool Relative)
{
	vec2 RealPos = vec2(Pos.x * 32.0, Pos.y * 32.0);
	vec2 UntestedViewPos = Relative ? State().m_ForceFreeviewPos + RealPos : RealPos;

	State().m_ForceFreeview = true;

	State().m_ForceFreeviewPos = vec2(
		std::clamp(UntestedViewPos.x, 200.0f, Collision()->GetWidth() * 32 - 200.0f),
		std::clamp(UntestedViewPos.y, 200.0f, Collision()->GetHeight() * 32 - 200.0f));
}

void CCamera::GotoSwitch(int Number, int Offset)
{
	if(Collision()->SwitchLayer() == nullptr)
		return;

	int Match = -1;
	ivec2 MatchPos = ivec2(-1, -1);

	auto FindTile = [this, &Match, &MatchPos, &Number, &Offset]() {
		for(int x = 0; x < Collision()->GetWidth(); x++)
		{
			for(int y = 0; y < Collision()->GetHeight(); y++)
			{
				int i = y * Collision()->GetWidth() + x;
				if(Number == Collision()->GetSwitchNumber(i))
				{
					Match++;
					if(Offset != -1)
					{
						if(Match == Offset)
						{
							MatchPos = ivec2(x, y);
							State().m_GotoSwitchOffset = Match;
							return;
						}
						continue;
					}
					MatchPos = ivec2(x, y);
					if(Match == State().m_GotoSwitchOffset)
						return;
				}
			}
		}
	};
	FindTile();

	if(MatchPos == ivec2(-1, -1))
		return;
	if(Match < State().m_GotoSwitchOffset)
		State().m_GotoSwitchOffset = -1;
	SetView(MatchPos);
	State().m_GotoSwitchOffset++;
}

void CCamera::GotoTele(int Number, int Offset)
{
	if(Collision()->TeleLayer() == nullptr)
		return;
	Number--;

	if(State().m_GotoTeleLastNumber != Number)
		State().m_GotoTeleLastPos = ivec2(-1, -1);

	ivec2 MatchPos = ivec2(-1, -1);
	const size_t NumTeles = Collision()->TeleAllSize(Number);
	if(!NumTeles)
	{
		log_error("camera", "No teleporter with number %d found.", Number + 1);
		return;
	}

	if(Offset != -1 || State().m_GotoTeleLastPos == ivec2(-1, -1))
	{
		if((size_t)Offset >= NumTeles || Offset < 0)
			Offset = 0;
		vec2 Tele = Collision()->TeleAllGet(Number, Offset);
		MatchPos = ivec2(Tele.x / 32, Tele.y / 32);
		State().m_GotoTeleOffset = Offset;
	}
	else
	{
		bool FullRound = false;
		do
		{
			vec2 Tele = Collision()->TeleAllGet(Number, State().m_GotoTeleOffset);
			MatchPos = ivec2(Tele.x / 32, Tele.y / 32);
			State().m_GotoTeleOffset++;
			if((size_t)State().m_GotoTeleOffset >= NumTeles)
			{
				State().m_GotoTeleOffset = 0;
				if(FullRound)
				{
					MatchPos = State().m_GotoTeleLastPos;
					break;
				}
				FullRound = true;
			}
		} while(distance(State().m_GotoTeleLastPos, MatchPos) < 10.0f);
	}

	if(MatchPos == ivec2(-1, -1))
		return;
	State().m_GotoTeleLastPos = MatchPos;
	State().m_GotoTeleLastNumber = Number;
	SetView(MatchPos);
}

void CCamera::SetZoom(float Target, int Smoothness, bool IsUser)
{
	ChangeZoom(Target, Smoothness, IsUser);
}

bool CCamera::ZoomAllowed() const
{
	return GameClient()->Snap().m_SpecInfo.m_Active ||
	       GameClient()->FocusedGameInfo().m_AllowZoom ||
	       Client()->State() == IClient::STATE_DEMOPLAYBACK;
}

int CCamera::Deadzone() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
}

int CCamera::FollowFactor() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor;
}

bool CCamera::CanUseAutoSpecCamera() const
{
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		// only follow mode has the correct camera info
		return GameClient()->Snap().m_SpecInfo.m_HasCameraInfo && GameClient()->m_DemoSpecId == SPEC_FOLLOW;
	}

	return g_Config.m_ClSpecAutoSync && GameClient()->Snap().m_SpecInfo.m_HasCameraInfo &&
	       GameClient()->Snap().m_SpecInfo.m_SpectatorId != GameClient()->GameState(IClient::CONN_MAIN).LocalClientId() &&
	       (!GameClient()->Client()->DummyConnected() || GameClient()->Snap().m_SpecInfo.m_SpectatorId != GameClient()->GameState(IClient::CONN_DUMMY).LocalClientId());
}

void CCamera::ToggleAutoSpecCamera()
{
	if(!g_Config.m_ClSpecAutoSync)
	{
		g_Config.m_ClSpecAutoSync = 1;
		State().m_AutoSpecCamera = true;
	}
	else if(State().m_AutoSpecCamera && SpectatingPlayer() && CanUseAutoSpecCamera())
	{
		State().m_AutoSpecCamera = false;
	}
	else
	{
		g_Config.m_ClSpecAutoSync = 0;
	}
}

void CCamera::UpdateAutoSpecCameraTooltip()
{
	const char *pFeatureText = Localize("Auto-sync player camera");

	if(!g_Config.m_ClSpecAutoSync)
		str_format(m_aAutoSpecCameraTooltip, sizeof(m_aAutoSpecCameraTooltip), "%s: %s", pFeatureText, Localize("Disabled", "Auto camera"));
	else if(!SpectatingPlayer())
		str_format(m_aAutoSpecCameraTooltip, sizeof(m_aAutoSpecCameraTooltip), "%s: %s", pFeatureText, Localize("Enabled", "Auto camera"));
	else if(!CanUseAutoSpecCamera())
		str_format(m_aAutoSpecCameraTooltip, sizeof(m_aAutoSpecCameraTooltip), "%s: %s", pFeatureText, Localize("Unavailable for this player", "Auto camera"));
	else if(!State().m_AutoSpecCamera)
		str_format(m_aAutoSpecCameraTooltip, sizeof(m_aAutoSpecCameraTooltip), "%s: %s", pFeatureText, Localize("Inactive", "Auto camera"));
	else
		str_format(m_aAutoSpecCameraTooltip, sizeof(m_aAutoSpecCameraTooltip), "%s: %s", pFeatureText, Localize("Active", "Auto camera"));
}
