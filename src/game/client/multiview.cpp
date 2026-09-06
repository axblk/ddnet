#include "gameclient.h"

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/spectator.h>

#include <algorithm>

void CGameClient::HandleMultiView(const CGameState &State, float LocalTime)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	bool IsTeamZero = IsMultiViewIdSet();
	bool Init = false;
	vec2 MinPos, MaxPos;
	float SumVel = 0.0f;
	int AmountPlayers = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CNetObj_DDNetCharacter *pExtended = State.ExtendedCharacter(ClientId);
		const bool Frozen = pExtended != nullptr && pExtended->m_FreezeEnd != 0;
		// look at players who are vanished
		if(MultiViewState.m_aVanish[ClientId])
		{
			// not in freeze anymore and the delay is over
			if(MultiViewState.m_aLastFreeze[ClientId] + 6.0f <= LocalTime && !Frozen)
			{
				MultiViewState.m_aVanish[ClientId] = false;
				MultiViewState.m_aLastFreeze[ClientId] = 0.0f;
			}
		}

		// we look at team 0 and the player is not in the spec list
		if(IsTeamZero && !MultiViewState.m_aSelected[ClientId])
			continue;

		// player is vanished
		if(MultiViewState.m_aVanish[ClientId])
			continue;

		// the player is not in the team we are spectating
		if(State.Teams().Team(ClientId) != MultiViewState.m_Team)
			continue;

		vec2 PlayerPos;
		if(State.RenderedClient(ClientId).m_Active)
			PlayerPos = State.RenderedClient(ClientId).m_Position;
		else if(const CGameState::CClientSnapshot &Client = State.Client(ClientId);
			Client.m_HasDDNetPlayer && (Client.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0 && Client.m_HasSpecChar)
			PlayerPos = vec2(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
		else
			continue;

		// player is far away and frozen
		if(distance(MultiViewState.m_OldPos, PlayerPos) > 1100 && Frozen)
		{
			// check if the player is frozen for more than 3 seconds, if so vanish them
			if(MultiViewState.m_aLastFreeze[ClientId] == 0.0f)
			{
				MultiViewState.m_aLastFreeze[ClientId] = LocalTime;
			}
			else if(MultiViewState.m_aLastFreeze[ClientId] + 3.0f <= LocalTime)
			{
				MultiViewState.m_aVanish[ClientId] = true;
				// player we want to be vanished is our "main" tee, so lets switch the tee
				if(ClientId == m_Snap.m_SpecInfo.m_SpectatorId)
					m_Spectator.Spectate(FindFirstMultiViewId());
			}
		}
		else if(MultiViewState.m_aLastFreeze[ClientId] != 0)
		{
			MultiViewState.m_aLastFreeze[ClientId] = 0;
		}

		// set the minimum and maximum position
		if(!Init)
		{
			MinPos = PlayerPos;
			MaxPos = PlayerPos;
			Init = true;
		}
		else
		{
			MinPos.x = std::min(MinPos.x, PlayerPos.x);
			MaxPos.x = std::max(MaxPos.x, PlayerPos.x);
			MinPos.y = std::min(MinPos.y, PlayerPos.y);
			MaxPos.y = std::max(MaxPos.y, PlayerPos.y);
		}

		// sum up the velocity of all players we are spectating
		const CNetObj_Character &CurrentCharacter = State.RenderedClient(ClientId).m_Cur;
		SumVel += length(vec2(CurrentCharacter.m_VelX / 256.0f, CurrentCharacter.m_VelY / 256.0f)) * 50.0f / 32.0f;
		AmountPlayers++;
	}

	// if we have found no players, we disable multi view
	if(AmountPlayers == 0)
	{
		if(MultiViewState.m_SecondChance == 0.0f)
		{
			MultiViewState.m_SecondChance = LocalTime + 0.3f;
		}
		else if(MultiViewState.m_SecondChance < LocalTime)
		{
			ResetMultiView();
		}
		return;
	}
	else if(MultiViewState.m_SecondChance != 0.0f)
	{
		MultiViewState.m_SecondChance = 0.0f;
	}

	// if we only have one tee that's in the list, we activate solo-mode
	MultiViewState.m_Solo = std::count(std::begin(MultiViewState.m_aSelected), std::end(MultiViewState.m_aSelected), true) == 1;

	vec2 TargetPos = vec2((MinPos.x + MaxPos.x) / 2.0f, (MinPos.y + MaxPos.y) / 2.0f);
	// dont hide the position hud if its only one player
	MultiViewState.m_ShowHud = AmountPlayers == 1;
	// get the average velocity
	float AvgVel = std::clamp(SumVel / AmountPlayers, 0.0f, 1000.0f);

	if(MultiViewState.m_OldPersonalZoom == MultiViewState.m_PersonalZoom)
		m_Camera.SetZoom(CalculateMultiViewZoom(MinPos, MaxPos, AvgVel), g_Config.m_ClMultiViewZoomSmoothness, false);
	else
		m_Camera.SetZoom(CalculateMultiViewZoom(MinPos, MaxPos, AvgVel), 50, false);

	m_Snap.m_SpecInfo.m_Position = MultiViewState.m_OldPos + ((TargetPos - MultiViewState.m_OldPos) * CalculateMultiViewMultiplier(TargetPos));
	MultiViewState.m_OldPos = m_Snap.m_SpecInfo.m_Position;
	m_Snap.m_SpecInfo.m_UsePosition = true;
}

bool CGameClient::InitMultiView(const CGameState &State, int Team)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	float Width, Height;
	CleanMultiViewIds();
	MultiViewState.m_IsInit = true;

	// get the current view coordinates
	Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), m_Camera.Zoom(), &Width, &Height);
	vec2 AxisX = vec2(m_Camera.Center().x - (Width / 2.0f), m_Camera.Center().x + (Width / 2.0f));
	vec2 AxisY = vec2(m_Camera.Center().y - (Height / 2.0f), m_Camera.Center().y + (Height / 2.0f));

	if(Team > 0)
	{
		MultiViewState.m_Team = Team;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			MultiViewState.m_aSelected[ClientId] = State.Teams().Team(ClientId) == Team;
	}
	else
	{
		// we want to allow spectating players in teams directly if there is no other team on screen
		// to do that, -1 is used temporarily for "we don't know which team to spectate yet"
		MultiViewState.m_Team = -1;

		int Count = 0;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			vec2 PlayerPos;

			// get the position of the player
			if(State.RenderedClient(ClientId).m_Active)
				PlayerPos = State.RenderedClient(ClientId).m_Position;
			else if(const CGameState::CClientSnapshot &Client = State.Client(ClientId);
				Client.m_HasDDNetPlayer && (Client.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0 && Client.m_HasSpecChar)
				PlayerPos = vec2(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
			else
				continue;

			if(PlayerPos.x == 0 || PlayerPos.y == 0)
				continue;

			// skip players that aren't in view
			if(PlayerPos.x <= AxisX.x || PlayerPos.x >= AxisX.y || PlayerPos.y <= AxisY.x || PlayerPos.y >= AxisY.y)
				continue;

			if(MultiViewState.m_Team == -1)
			{
				// use the current player's team for now, but it might switch to team 0 if any other team is found
				MultiViewState.m_Team = State.Teams().Team(ClientId);
			}
			else if(MultiViewState.m_Team != 0 && State.Teams().Team(ClientId) != MultiViewState.m_Team)
			{
				// mismatched teams; remove all previously added players again and switch to team 0 instead
				std::fill_n(MultiViewState.m_aSelected, ClientId, false);
				MultiViewState.m_Team = 0;
			}

			MultiViewState.m_aSelected[ClientId] = true;
			Count++;
		}

		// might still be -1 if not a single player was in view; fallback to team 0 in that case
		if(MultiViewState.m_Team == -1)
			MultiViewState.m_Team = 0;

		// we are spectating only one player
		MultiViewState.m_Solo = Count == 1;
	}

	if(IsMultiViewIdSet())
	{
		int SpectatorId = m_Snap.m_SpecInfo.m_SpectatorId;
		int NewSpectatorId = -1;

		vec2 CurPosition(m_Camera.Center());
		if(SpectatorId != SPEC_FREEVIEW)
		{
			CurPosition = State.RenderedClient(SpectatorId).m_Position;
		}

		int ClosestDistance = std::numeric_limits<int>::max();
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
			if(!SnapshotClient.m_HasPlayerInfo || SnapshotClient.m_PlayerInfo.m_Team == TEAM_SPECTATORS || State.Teams().Team(ClientId) != MultiViewState.m_Team)
				continue;

			vec2 PlayerPos;
			if(State.RenderedClient(ClientId).m_Active)
				PlayerPos = State.RenderedClient(ClientId).m_Position;
			else if(const CGameState::CClientSnapshot &Client = State.Client(ClientId);
				Client.m_HasDDNetPlayer && (Client.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0 && Client.m_HasSpecChar)
				PlayerPos = vec2(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
			else
				continue;

			int Distance = distance(CurPosition, PlayerPos);
			if(NewSpectatorId == -1 || Distance < ClosestDistance)
			{
				NewSpectatorId = ClientId;
				ClosestDistance = Distance;
			}
		}

		if(NewSpectatorId > -1)
			m_Spectator.Spectate(NewSpectatorId);
	}

	return IsMultiViewIdSet();
}

float CGameClient::CalculateMultiViewMultiplier(vec2 TargetPos)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	float MaxCameraDist = 200.0f;
	float MinCameraDist = 20.0f;
	float MaxVel = g_Config.m_ClMultiViewSensitivity / 150.0f;
	float MinVel = 0.007f;
	float CurrentCameraDistance = distance(MultiViewState.m_OldPos, TargetPos);
	float UpperLimit = 1.0f;

	if(MultiViewState.m_Teleported && CurrentCameraDistance <= 100.0f)
		MultiViewState.m_Teleported = false;

	// somebody got teleported very likely
	if((MultiViewState.m_Teleported || CurrentCameraDistance - MultiViewState.m_OldCameraDistance > 100.0f) && MultiViewState.m_OldCameraDistance != 0.0f)
	{
		UpperLimit = 0.1f; // dont try to compensate it by flickering
		MultiViewState.m_Teleported = true;
	}
	MultiViewState.m_OldCameraDistance = CurrentCameraDistance;

	return std::clamp(MapValue(MaxCameraDist, MinCameraDist, MaxVel, MinVel, CurrentCameraDistance), MinVel, UpperLimit);
}

float CGameClient::CalculateMultiViewZoom(vec2 MinPos, vec2 MaxPos, float Vel)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	float Ratio = Graphics()->ScreenAspect();
	float ZoomX = 0.0f, ZoomY;

	// only calc two axis if the aspect ratio is not 1:1
	if(Ratio != 1.0f)
		ZoomX = (0.001309f - 0.000328f * Ratio) * (MaxPos.x - MinPos.x) + (0.741413f - 0.032959f * Ratio);

	// calculate the according zoom with linear function
	ZoomY = 0.001309f * (MaxPos.y - MinPos.y) + 0.741413f;
	// choose the highest zoom
	float Zoom = std::max(ZoomX, ZoomY);
	// zoom out to maximum 10 percent of the current zoom for 70 velocity
	float Diff = std::clamp(MapValue(70.0f, 15.0f, Zoom * 0.10f, 0.0f, Vel), 0.0f, Zoom * 0.10f);
	// zoom should stay between 1.1 and 20.0
	Zoom = std::clamp(Zoom + Diff, 1.1f, 20.0f);
	// dont go below default zoom
	Zoom = std::max(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), Zoom);
	// add the user preference
	Zoom -= Zoom * 0.1f * MultiViewState.m_PersonalZoom;
	MultiViewState.m_OldPersonalZoom = MultiViewState.m_PersonalZoom;

	return Zoom;
}

float CGameClient::MapValue(float MaxValue, float MinValue, float MaxRange, float MinRange, float Value)
{
	return (MaxRange - MinRange) / (MaxValue - MinValue) * (Value - MinValue) + MinRange;
}

void CGameClient::ResetMultiView()
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	m_Camera.SetZoom(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), g_Config.m_ClSmoothZoomTime, true);
	MultiViewState.m_PersonalZoom = 0.0f;
	MultiViewState.m_Active = false;
	MultiViewState.m_Solo = false;
	MultiViewState.m_IsInit = false;
	MultiViewState.m_Teleported = false;
	MultiViewState.m_OldCameraDistance = 0.0f;
}

void CGameClient::CleanMultiViewIds()
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	std::fill(std::begin(MultiViewState.m_aSelected), std::end(MultiViewState.m_aSelected), false);
	std::fill(std::begin(MultiViewState.m_aLastFreeze), std::end(MultiViewState.m_aLastFreeze), 0.0f);
	std::fill(std::begin(MultiViewState.m_aVanish), std::end(MultiViewState.m_aVanish), false);
}

void CGameClient::CleanMultiViewId(int ClientId)
{
	if(ClientId >= MAX_CLIENTS || ClientId < 0)
		return;

	CGameView::CMultiViewState &MultiViewState = MultiView();
	MultiViewState.m_aSelected[ClientId] = false;
	MultiViewState.m_aLastFreeze[ClientId] = 0.0f;
	MultiViewState.m_aVanish[ClientId] = false;
}

bool CGameClient::IsMultiViewIdSet()
{
	const CGameView::CMultiViewState &MultiViewState = MultiView();
	return std::any_of(std::begin(MultiViewState.m_aSelected), std::end(MultiViewState.m_aSelected), [](bool IsSet) { return IsSet; });
}

int CGameClient::FindFirstMultiViewId()
{
	int ClientId = -1;
	const CGameView::CMultiViewState &MultiViewState = MultiView();
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(MultiViewState.m_aSelected[i] && !MultiViewState.m_aVanish[i])
			return i;
	}
	return ClientId;
}
