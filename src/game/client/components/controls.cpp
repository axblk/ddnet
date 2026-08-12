/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/dbg.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>

#include <algorithm>
namespace
{
	enum class EInputField
	{
		DIRECTION_LEFT,
		DIRECTION_RIGHT,
		JUMP,
		HOOK,
		FIRE,
		SHOW_HOOK_COLL,
		WANTED_WEAPON,
		NEXT_WEAPON,
		PREV_WEAPON,
	};

	int &InputField(CGameState::CInputState &Input, EInputField Field)
	{
		switch(Field)
		{
		case EInputField::DIRECTION_LEFT: return Input.m_InputDirectionLeft;
		case EInputField::DIRECTION_RIGHT: return Input.m_InputDirectionRight;
		case EInputField::JUMP: return Input.m_InputData.m_Jump;
		case EInputField::HOOK: return Input.m_InputData.m_Hook;
		case EInputField::FIRE: return Input.m_InputData.m_Fire;
		case EInputField::SHOW_HOOK_COLL: return Input.m_ShowHookColl;
		case EInputField::WANTED_WEAPON: return Input.m_InputData.m_WantedWeapon;
		case EInputField::NEXT_WEAPON: return Input.m_InputData.m_NextWeapon;
		case EInputField::PREV_WEAPON: return Input.m_InputData.m_PrevWeapon;
		}
		dbg_assert_failed("invalid input field");
		return Input.m_InputDirectionLeft;
	}
}

void CControls::OnReset()
{
	for(const auto &pState : GameClient()->SessionContext().GameStates().States())
		pState->Input().Reset();
}

void CControls::ResetInput(int Conn)
{
	ResetInput(GameClient()->GameState(Conn).StreamId());
}

void CControls::ResetInput(CStreamId StreamId)
{
	CGameState *pState = GameClient()->SessionContext().GameStates().FindByStream(StreamId);
	dbg_assert(pState != nullptr, "missing game state for input reset");
	if(!pState)
		return;
	CGameState::CInputState &Input = pState->Input();
	Input.m_LastData.m_Direction = 0;
	// simulate releasing the fire button
	if((Input.m_LastData.m_Fire & 1) != 0)
		Input.m_LastData.m_Fire++;
	Input.m_LastData.m_Fire &= INPUT_STATE_MASK;
	Input.m_LastData.m_Jump = 0;
	Input.m_InputData = Input.m_LastData;

	Input.m_InputDirectionLeft = 0;
	Input.m_InputDirectionRight = 0;
}

void CControls::OnPlayerDeath()
{
	GameClient()->GameState(GameClient()->ActiveConnection()).Input().m_aAmmoCount.fill(0);
}

struct CInputState
{
	CControls *m_pControls;
	EInputField m_Field;
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->FocusedGameInfo().m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	CGameState::CInputState &Input = pState->m_pControls->GameClient()->GameState(pState->m_pControls->GameClient()->ActiveConnection()).Input();
	InputField(Input, pState->m_Field) = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->FocusedGameInfo().m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	CGameState::CInputState &Input = pState->m_pControls->GameClient()->GameState(pState->m_pControls->GameClient()->ActiveConnection()).Input();
	int *pVariable = &InputField(Input, pState->m_Field);
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CInputState m_State;
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		CGameState::CInputState &Input = pSet->m_State.m_pControls->GameClient()->GameState(pSet->m_State.m_pControls->GameClient()->ActiveConnection()).Input();
		InputField(Input, pSet->m_State.m_Field) = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, &pSet->m_State);
	pSet->m_State.m_pControls->GameClient()->GameState(pSet->m_State.m_pControls->GameClient()->ActiveConnection()).Input().m_InputData.m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, EInputField::DIRECTION_LEFT};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, EInputField::DIRECTION_RIGHT};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, EInputField::JUMP};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, EInputField::HOOK};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, EInputField::FIRE};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, EInputField::SHOW_HOOK_COLL};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {{this, EInputField::WANTED_WEAPON}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {{this, EInputField::WANTED_WEAPON}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {{this, EInputField::WANTED_WEAPON}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {{this, EInputField::WANTED_WEAPON}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {{this, EInputField::WANTED_WEAPON}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {{this, EInputField::NEXT_WEAPON}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {{this, EInputField::PREV_WEAPON}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	CGameState::CInputState &Input = GameClient()->GameState(GameClient()->ActiveConnection()).Input();
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			Input.m_InputData.m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		Input.m_aAmmoCount[std::max(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	CGameState::CInputState &Input = GameClient()->GameState(GameClient()->ActiveConnection()).Input();
	CGameState::CInputState &OtherInput = GameClient()->GameState(GameClient()->OtherConnection()).Input();
	// update player state
	if(GameClient()->m_Chat.IsActive())
		Input.m_InputData.m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		Input.m_InputData.m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		Input.m_InputData.m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		Input.m_InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && Input.m_ShowHookColl)
		Input.m_InputData.m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		Input.m_InputData.m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(Input.m_MouseInputType)
	{
	case CGameState::EMouseInputType::AUTOMATED:
		Input.m_InputData.m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CGameState::EMouseInputType::ABSOLUTE:
		Input.m_InputData.m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CGameState::EMouseInputType::RELATIVE:
		Input.m_InputData.m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	bool Send = Input.m_LastData.m_PlayerFlags != Input.m_InputData.m_PlayerFlags;

	Input.m_LastData.m_PlayerFlags = Input.m_InputData.m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(Input.m_InputData.m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->FocusedGameInfo().m_BugDDRaceInput)
			ResetInput(GameClient()->ActiveConnection());

		mem_copy(pData, &Input.m_InputData, sizeof(Input.m_InputData));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		Input.m_InputData.m_TargetX = (int)Input.m_MousePos.x;
		Input.m_InputData.m_TargetY = (int)Input.m_MousePos.y;

		// send once a second just to be sure
		Send = Send || time_get() > Input.m_LastSendTime + time_freq();
	}
	else
	{
		Input.m_InputData.m_TargetX = (int)Input.m_MousePos.x;
		Input.m_InputData.m_TargetY = (int)Input.m_MousePos.y;

		if(g_Config.m_ClSubTickAiming && Input.m_MousePosOnAction != vec2(0.0f, 0.0f))
		{
			Input.m_InputData.m_TargetX = (int)Input.m_MousePosOnAction.x;
			Input.m_InputData.m_TargetY = (int)Input.m_MousePosOnAction.y;
			Input.m_MousePosOnAction = vec2(0.0f, 0.0f);
		}

		if(!Input.m_InputData.m_TargetX && !Input.m_InputData.m_TargetY)
		{
			Input.m_InputData.m_TargetX = 1;
			Input.m_MousePos.x = 1;
		}

		// set direction
		Input.m_InputData.m_Direction = 0;
		if(Input.m_InputDirectionLeft && !Input.m_InputDirectionRight)
			Input.m_InputData.m_Direction = -1;
		if(!Input.m_InputDirectionLeft && Input.m_InputDirectionRight)
			Input.m_InputData.m_Direction = 1;

		// dummy copy moves
		const CStreamId Source = GameClient()->GameState(GameClient()->ActiveConnection()).StreamId();
		for(const CStreamInputRoute &Route : GameClient()->SessionContext().InputRouter().Routes())
		{
			if(Route.m_Policy != EStreamInputPolicy::COPY_MOVES || Route.m_Source != Source)
				continue;
			CGameState *pTargetState = GameClient()->SessionContext().GameStates().FindByStream(Route.m_Target);
			dbg_assert(pTargetState != nullptr, "missing copy-moves target state");
			if(!pTargetState)
				continue;
			CNetObj_PlayerInput &TargetInput = pTargetState->Input().m_InputData;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				TargetInput.m_Direction = Input.m_InputData.m_Direction;
				TargetInput.m_Hook = Input.m_InputData.m_Hook;
				TargetInput.m_Jump = Input.m_InputData.m_Jump;
				TargetInput.m_PlayerFlags = Input.m_InputData.m_PlayerFlags;
				TargetInput.m_TargetX = Input.m_InputData.m_TargetX;
				TargetInput.m_TargetY = Input.m_InputData.m_TargetY;
				TargetInput.m_WantedWeapon = Input.m_InputData.m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					TargetInput.m_Fire += Input.m_InputData.m_Fire - Input.m_LastData.m_Fire;

				TargetInput.m_NextWeapon += Input.m_InputData.m_NextWeapon - Input.m_LastData.m_NextWeapon;
				TargetInput.m_PrevWeapon += Input.m_InputData.m_PrevWeapon - Input.m_LastData.m_PrevWeapon;
			}
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &OtherInput.m_InputData;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&Input.m_InputData, sizeof(Input.m_InputData));

			Input.m_InputData.m_Direction = ((int)t / 2) & 1;
			Input.m_InputData.m_Jump = ((int)t);
			Input.m_InputData.m_Fire = ((int)(t * 10));
			Input.m_InputData.m_Hook = ((int)t * 2) & 1;
			Input.m_InputData.m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			Input.m_InputData.m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			Input.m_InputData.m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || Input.m_InputData.m_Direction != Input.m_LastData.m_Direction;
		Send = Send || Input.m_InputData.m_Jump != Input.m_LastData.m_Jump;
		Send = Send || Input.m_InputData.m_Fire != Input.m_LastData.m_Fire;
		Send = Send || Input.m_InputData.m_Hook != Input.m_LastData.m_Hook;
		Send = Send || Input.m_InputData.m_WantedWeapon != Input.m_LastData.m_WantedWeapon;
		Send = Send || Input.m_InputData.m_NextWeapon != Input.m_LastData.m_NextWeapon;
		Send = Send || Input.m_InputData.m_PrevWeapon != Input.m_LastData.m_PrevWeapon;
		Send = Send || time_get() > Input.m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (Input.m_InputData.m_Direction || Input.m_InputData.m_Jump || Input.m_InputData.m_Hook));
	}

	// copy and return size
	Input.m_LastData = Input.m_InputData;

	if(!Send)
		return 0;

	Input.m_LastSendTime = time_get();
	mem_copy(pData, &Input.m_InputData, sizeof(Input.m_InputData));
	return sizeof(Input.m_InputData);
}

void CControls::Update()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	CGameState::CInputState &Input = GameClient()->GameState(GameClient()->ActiveConnection()).Input();

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->FocusedGameInfo().m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		Input.m_aAmmoCount[std::max(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(Input.m_InputData.m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(Input.m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				Input.m_InputData.m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.DynamicCameraTargetOffset() - GameClient()->m_Camera.DynamicCameraOffset();
		float Zoom = GameClient()->m_Camera.Zoom();
		Input.m_TargetPos = GameClient()->m_LocalCharacterPos + Input.m_MousePos - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		Input.m_TargetPos = GameClient()->m_Snap.m_SpecInfo.m_Position + Input.m_MousePos;
	}
	else
	{
		Input.m_TargetPos = Input.m_MousePos;
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->IsWorldPaused())
		return false;
	CGameState::CInputState &Input = GameClient()->GameState(GameClient()->ActiveConnection()).Input();

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(this->Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			Input.m_MousePos = AbsoluteDirection * GetMaxMouseDistance();
			Input.m_MouseInputType = CGameState::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.Zoom();

	Input.m_MousePos += vec2(x, y) * Factor;
	Input.m_MouseInputType = CGameState::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	CGameState::CInputState &Input = GameClient()->GameState(GameClient()->ActiveConnection()).Input();
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		Input.m_MousePos.x = std::clamp(Input.m_MousePos.x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		Input.m_MousePos.y = std::clamp(Input.m_MousePos.y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
		ClampMousePos(Input);
}

void CControls::ClampMousePos(CGameState::CInputState &Input) const
{
	const float MouseMin = GetMinMouseDistance();
	const float MouseMax = GetMaxMouseDistance();

	float MouseDistance = length(Input.m_MousePos);
	if(MouseDistance < 0.001f)
	{
		Input.m_MousePos.x = 0.001f;
		Input.m_MousePos.y = 0;
		MouseDistance = 0.001f;
	}
	if(MouseDistance < MouseMin)
		Input.m_MousePos = normalize_pre_length(Input.m_MousePos, MouseDistance) * MouseMin;
	MouseDistance = length(Input.m_MousePos);
	if(MouseDistance > MouseMax)
		Input.m_MousePos = normalize_pre_length(Input.m_MousePos, MouseDistance) * MouseMax;
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return std::min(FollowFactor != 0.0f ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance, MaxDistance);
}
