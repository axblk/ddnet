/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "view_control.h"

#include "gameclient.h"

#include <base/math.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/gamecore.h>

#include <algorithm>

namespace
{
	float DefaultZoom()
	{
		return CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10);
	}
} // namespace

void CGameViewControl::WithCamera(CSessionId SessionId, const std::function<void(CCamera &Camera)> &Use) const
{
	CGameSessionContext *pSession = m_GameClient.FindSessionContext(SessionId);
	if(pSession == nullptr || !pSession->Contains(SessionId))
		return;
	CCamera &Camera = m_GameClient.m_Camera;
	Camera.BindTarget(*pSession, pSession->GameState(SessionId), m_GameClient.ViewOf(SessionId), false, m_GameClient.ViewLocalTime(SessionId));
	Use(Camera);
	CGameSessionContext &InputSession = m_GameClient.SessionContext();
	Camera.BindTarget(InputSession, InputSession.GameState(m_GameClient.InputSessionId()), m_GameClient.InputView(), true, m_GameClient.Client()->LocalTime());
}

int CGameViewControl::SpectatorId(CSessionId SessionId) const
{
	const CGameSessionContext *pSession = m_GameClient.FindSessionContext(SessionId);
	return pSession == nullptr ? SPEC_FOLLOW : pSession->m_DemoSpecId;
}

void CGameViewControl::SetSpectatorId(CSessionId SessionId, int SpectatorId)
{
	CGameSessionContext *pSession = m_GameClient.FindSessionContext(SessionId);
	SpectatorId = std::clamp(SpectatorId, (int)SPEC_FOLLOW, MAX_CLIENTS - 1);
	if(pSession == nullptr || pSession->m_DemoSpecId == SpectatorId)
		return;
	pSession->m_DemoSpecId = SpectatorId;
	// The tick must be rendered for the spectator mode to be updated, so we do it manually when demo playback is paused
	if(SessionId == m_GameClient.Sessions()->DemoSessionId() && m_GameClient.DemoPlayer()->BaseInfo()->m_Paused)
		m_GameClient.DemoSeekTick(IDemoPlayer::TICK_CURRENT);
}

bool CGameViewControl::PlayerName(CSessionId SessionId, int ClientId, char *pBuffer, int BufferSize) const
{
	const CGameSessionContext *pSession = m_GameClient.FindSessionContext(SessionId);
	if(pSession == nullptr || !pSession->Contains(SessionId) || !in_range(ClientId, 0, MAX_CLIENTS - 1))
		return false;
	const CGameState::CClientIdentityState &Identity = pSession->GameState(SessionId).ClientIdentity(ClientId);
	return Identity.m_Active && IntsToStr(Identity.m_ClientInfo.m_aName, std::size(Identity.m_ClientInfo.m_aName), pBuffer, BufferSize);
}

void CGameViewControl::MoveFreeView(CSessionId SessionId, vec2 Offset)
{
	if(Offset == vec2(0.0f, 0.0f) || SpectatorId(SessionId) != SPEC_FREEVIEW)
		return;
	WithCamera(SessionId, [Offset](CCamera &Camera) { Camera.SetViewPos(Offset, true); });
}

float CGameViewControl::Zoom(CSessionId SessionId) const
{
	return m_GameClient.ViewOf(SessionId).Zoom();
}

void CGameViewControl::ScaleZoom(CSessionId SessionId, float Factor)
{
	WithCamera(SessionId, [Factor](CCamera &Camera) {
		if(Camera.ZoomAllowed())
			Camera.ScaleZoom(Factor);
	});
}

void CGameViewControl::ResetZoom(CSessionId SessionId)
{
	WithCamera(SessionId, [](CCamera &Camera) {
		if(Camera.ZoomAllowed())
			Camera.SetZoom(DefaultZoom(), g_Config.m_ClSmoothZoomTime, true);
	});
}

bool CGameViewControl::ZoomChanged(CSessionId SessionId) const
{
	bool Changed = false;
	// Not exactly, because zooming in and out by the same notches lands a
	// hair beside where it started.
	WithCamera(SessionId, [&Changed](CCamera &Camera) {
		Changed = Camera.ZoomAllowed() && absolute(Camera.UserZoomTarget() - DefaultZoom()) > 0.001f * DefaultZoom();
	});
	return Changed;
}

bool CGameViewControl::RecordedCameraAvailable(CSessionId SessionId) const
{
	bool Available = false;
	WithCamera(SessionId, [&Available](CCamera &Camera) { Available = Camera.CanUseAutoSpecCamera(); });
	return Available;
}

bool CGameViewControl::RecordedCamera(CSessionId SessionId) const
{
	bool Used = false;
	WithCamera(SessionId, [&Used](CCamera &Camera) { Used = Camera.IsAutoSpecCamera() && Camera.CanUseAutoSpecCamera(); });
	return Used;
}

void CGameViewControl::SetRecordedCamera(CSessionId SessionId, bool Use)
{
	WithCamera(SessionId, [Use](CCamera &Camera) { Camera.SetAutoSpecCamera(Use); });
}

CViewRenderOptions CGameViewControl::RenderOptions(CSessionId SessionId) const
{
	return m_GameClient.ViewOf(SessionId).RenderOptions();
}

void CGameViewControl::SetRenderOptions(CSessionId SessionId, const CViewRenderOptions &Options)
{
	m_GameClient.ViewOf(SessionId).SetRenderOptions(Options);
}
