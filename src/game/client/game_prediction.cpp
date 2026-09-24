/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "game_prediction.h"

#include "gameclient.h"

#include <base/log.h>
#include <base/mem.h>

#include <engine/sessions.h>
#include <engine/shared/config.h>

#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/entities/projectile.h>

#include <algorithm>
#include <iterator>

class CGamePrediction final : public IGamePrediction
{
public:
	void OnNewSnapshot(CGameClient &GameClient) override { GameClient.UpdatePrediction(); }
	void Predict(CGameClient &GameClient, CSessionId SessionId) override { GameClient.PredictSession(SessionId); }
	void AddEntities(CGameState &State) const override { State.AddSnapshotEntities(); }
};

IGamePrediction *CreateGamePrediction()
{
	return new CGamePrediction();
}

void CGameState::Predict(const ISessions &Sessions)
{
	const CSessionId SessionId = m_SessionId;
	PredictTo(Sessions.PredGameTick(SessionId), [&Sessions, SessionId](int Tick) {
		return reinterpret_cast<const CNetObj_PlayerInput *>(Sessions.GetInput(SessionId, Tick));
	});
}

void CGameState::PredictTo(int TargetTick, const std::function<const CNetObj_PlayerInput *(int)> &InputAt)
{
	ClearPrediction();
	if(m_FullyPredicted)
		return;
	if(!m_PredictionInitialized || m_LocalClientId < 0 || m_LocalClientId >= MAX_CLIENTS || !m_aClients[m_LocalClientId].m_HasCharacter)
		return;
	if(m_HasGameInfo && (m_GameInfo.m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return;

	m_PredictedWorld.CopyWorld(&m_GameWorld);
	CCharacter *pLocalCharacter = m_PredictedWorld.GetCharacterById(m_LocalClientId);
	if(!pLocalCharacter)
		return;
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
	auto RecordPredictionHistory = [this](int Tick) {
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(const CCharacter *pCharacter = m_PredictedWorld.GetCharacterById(ClientId))
			{
				m_vClientPredictionHistory[ClientId].m_aPredPos[Tick % 200] = pCharacter->Core()->m_Pos;
				m_vClientPredictionHistory[ClientId].m_aPredTick[Tick % 200] = Tick;
			}
		}
	};
	RecordPredictionHistory(m_SnapshotTick);

	for(int Tick = m_SnapshotTick + 1; Tick <= TargetTick; Tick++)
	{
		const CNetObj_PlayerInput *pInput = InputAt(Tick);
		if(pInput)
			pLocalCharacter->OnDirectInput(pInput);
		m_PredictedWorld.m_GameTick = Tick;
		if(pInput)
			pLocalCharacter->OnPredictedInput(pInput);
		if(Tick == TargetTick)
			m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
		m_PredictedWorld.Tick();
		RecordPredictionHistory(Tick);
	}

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(CCharacter *pCharacter = m_PrevPredictedWorld.GetCharacterById(ClientId))
		{
			m_aPredictedClients[ClientId].m_HasPrev = true;
			m_aPredictedClients[ClientId].m_Prev = pCharacter->GetCore();
		}
		if(CCharacter *pCharacter = m_PredictedWorld.GetCharacterById(ClientId))
		{
			m_aPredictedClients[ClientId].m_HasCurrent = true;
			m_aPredictedClients[ClientId].m_Current = pCharacter->GetCore();
		}
	}
}

void CGameState::AddSnapshotEntities()
{
	for(const CEntitySnapshot &Entity : m_vEntities)
		m_GameWorld.NetObjAdd(Entity.m_Id, Entity.m_Type, Entity.m_vData.data(), Entity.m_HasEntityEx ? &Entity.m_EntityEx : nullptr);
}

void CGameClient::ApplyPreInputs(int Tick, bool Direct, CGameWorld &GameWorld)
{
	if(!g_Config.m_ClAntiPingPreInput)
		return;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(CCharacter *pChar = GameWorld.GetCharacterById(ClientId))
		{
			if(ClientId == SessionContext().SeatState(IClient::CONN_MAIN).LocalClientId() || (DummyConnected() && ClientId == SessionContext().SeatState(IClient::CONN_DUMMY).LocalClientId()))
				continue;

			const CNetMsg_Sv_PreInput PreInput = m_aClients[ClientId].m_aPreInputs[Tick % 200];
			if(PreInput.m_IntendedTick != Tick)
				continue;

			//convert preinput to input
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = PreInput.m_Direction;
			Input.m_TargetX = PreInput.m_TargetX;
			Input.m_TargetY = PreInput.m_TargetY;
			Input.m_Jump = PreInput.m_Jump;
			Input.m_Fire = PreInput.m_Fire;
			Input.m_Hook = PreInput.m_Hook;
			Input.m_WantedWeapon = PreInput.m_WantedWeapon;
			Input.m_NextWeapon = PreInput.m_NextWeapon;
			Input.m_PrevWeapon = PreInput.m_PrevWeapon;

			if(Direct)
			{
				pChar->OnDirectInput(&Input);
			}
			else
			{
				pChar->OnPredictedInput(&Input);
			}
		}
	}
}

void CGameClient::PredictSession(CSessionId SessionId)
{
	CGameState &State = GameState(SessionId);
	State.SetFullyPredicted(SessionId == InputSessionId());
	State.Predict(*Sessions());
	if(State.IsFullyPredicted())
		ProcessPrediction();
}

void CGameClient::ProcessPrediction()
{
	const CSessionId SessionId = InputSessionId();
	CGameState &ActiveState = GameState(SessionId);
	CGameState::CRuntimeState &Runtime = ActiveState.m_Runtime;
	// store the previous values so we can detect prediction errors
	CCharacterCore BeforePrevChar = m_PredictedPrevChar;
	CCharacterCore BeforeChar = m_PredictedChar;

	// we can't predict without our own id or own character
	if(Snap().m_LocalClientId == -1 || !Snap().m_aCharacters[Snap().m_LocalClientId].m_Active)
		return;

	// don't predict anything if we are paused
	if(Snap().m_pGameInfoObj && Snap().m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED)
	{
		if(Snap().m_pLocalCharacter)
		{
			m_PredictedChar.Read(Snap().m_pLocalCharacter);
			m_PredictedChar.m_ActiveWeapon = Snap().m_pLocalCharacter->m_Weapon;
		}
		if(Snap().m_pLocalPrevCharacter)
		{
			m_PredictedPrevChar.Read(Snap().m_pLocalPrevCharacter);
			m_PredictedPrevChar.m_ActiveWeapon = Snap().m_pLocalPrevCharacter->m_Weapon;
		}
		return;
	}

	vec2 aBeforeRender[MAX_CLIENTS];
	const int64_t SmoothNow = time_get();
	for(int i = 0; i < MAX_CLIENTS; i++)
		aBeforeRender[i] = GetSmoothPos(SessionId, ActiveState, i, SmoothNow, m_aClients[i].m_PrevPredicted, m_aClients[i].m_Predicted);

	// init
	// The other seat on the server, whose character is predicted along.
	CGameState &OtherState = SessionContext(SessionId).SeatState(ActiveState.m_Seat ^ 1);

	// PredictedEvents are only handled in predicted world, so update them here
	GameWorld().m_PredictedEvents = PredictedWorld().m_PredictedEvents;
	PredictedWorld().CopyWorld(&GameWorld());

	// don't predict inactive players, or entities from other teams
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(CCharacter *pChar = PredictedWorld().GetCharacterById(i))
			if((!Snap().m_aCharacters[i].m_Active && pChar->m_SnapTicks > 10) || IsOtherTeam(i))
				pChar->Destroy();

	CProjectile *pProjNext = nullptr;
	for(CProjectile *pProj = (CProjectile *)PredictedWorld().FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pProj; pProj = pProjNext)
	{
		pProjNext = (CProjectile *)pProj->TypeNext();
		if(IsOtherTeam(pProj->GetOwner()))
		{
			pProj->Destroy();
		}
	}

	CCharacter *pLocalChar = PredictedWorld().GetCharacterById(Snap().m_LocalClientId);
	if(!pLocalChar)
		return;
	CCharacter *pDummyChar = nullptr;
	if(PredictDummy(OtherState))
		pDummyChar = PredictedWorld().GetCharacterById(OtherState.LocalClientId());

	int PredictionTick = Sessions()->GetPredictionTick(SessionId);
	// predict
	for(int Tick = Sessions()->GameTick(SessionId) + 1; Tick <= Sessions()->PredGameTick(SessionId); Tick++)
	{
		// fetch the previous characters
		if(Tick == PredictionTick)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = PredictedWorld().GetCharacterById(i))
					m_aClients[i].m_PrevPredicted = pChar->GetCore();
		}

		if(Tick == Sessions()->PredGameTick(SessionId))
		{
			m_PredictedPrevChar = pLocalChar->GetCore();
			m_aClients[Snap().m_LocalClientId].m_PrevPredicted = pLocalChar->GetCore();

			if(pDummyChar)
				m_aClients[OtherState.LocalClientId()].m_PrevPredicted = pDummyChar->GetCore();
		}

		// optionally allow some movement in freeze by not predicting freeze the last one to two ticks
		if(g_Config.m_ClPredictFreeze == 2 && Sessions()->PredGameTick(SessionId) - 1 - Sessions()->PredGameTick(SessionId) % 2 <= Tick)
			pLocalChar->m_CanMoveInFreeze = true;

		// apply inputs and tick
		CNetObj_PlayerInput *pInputData = (CNetObj_PlayerInput *)Sessions()->GetInput(SessionId, Tick);
		CNetObj_PlayerInput *pDummyInputData = !pDummyChar ? nullptr : (CNetObj_PlayerInput *)Sessions()->GetInput(OtherState.m_SessionId, Tick);
		bool DummyFirst = pInputData && pDummyInputData && pDummyChar->GetCid() < pLocalChar->GetCid();

		if(DummyFirst)
			pDummyChar->OnDirectInput(pDummyInputData);
		if(pInputData)
			pLocalChar->OnDirectInput(pInputData);
		if(pDummyInputData && !DummyFirst)
			pDummyChar->OnDirectInput(pDummyInputData);

		ApplyPreInputs(Tick, true, PredictedWorld());

		PredictedWorld().m_GameTick = Tick;
		if(pInputData)
			pLocalChar->OnPredictedInput(pInputData);
		if(pDummyInputData)
			pDummyChar->OnPredictedInput(pDummyInputData);

		ApplyPreInputs(Tick, false, PredictedWorld());

		PredictedWorld().Tick();

		// fetch the current characters
		if(Tick == PredictionTick)
		{
			PrevPredictedWorld().CopyWorld(&PredictedWorld());

			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = PredictedWorld().GetCharacterById(i))
					m_aClients[i].m_Predicted = pChar->GetCore();
		}

		if(Tick == Sessions()->PredGameTick(SessionId))
		{
			m_PredictedChar = pLocalChar->GetCore();
			m_aClients[Snap().m_LocalClientId].m_Predicted = pLocalChar->GetCore();

			if(pDummyChar)
				m_aClients[OtherState.LocalClientId()].m_Predicted = pDummyChar->GetCore();
		}

		for(int i = 0; i < MAX_CLIENTS; i++)
			if(CCharacter *pChar = PredictedWorld().GetCharacterById(i))
			{
				ActiveState.PredictionHistory(i).m_aPredPos[Tick % 200] = pChar->Core()->m_Pos;
				ActiveState.PredictionHistory(i).m_aPredTick[Tick % 200] = Tick;
			}

		// check if we want to trigger effects
		if(Tick > Runtime.m_LastNewPredictedTick)
		{
			Runtime.m_LastNewPredictedTick = Tick;
			m_NewPredictedTick = true;
			vec2 Pos = pLocalChar->Core()->m_Pos;
			int Events = pLocalChar->Core()->m_TriggeredEvents;
			if(g_Config.m_ClPredict && !m_SuppressEvents)
				if(Events & COREEVENT_AIR_JUMP)
					m_Effects.AirJump(ActiveState, Pos, pLocalChar->GetCid(), 1.0f, 1.0f);
			if(g_Config.m_SndGame && !m_SuppressEvents)
			{
				if(Events & COREEVENT_GROUND_JUMP)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_PLAYER_JUMP, 1.0f, Pos);
				if(Events & COREEVENT_HOOK_ATTACH_GROUND)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_HOOK_ATTACH_GROUND, 1.0f, Pos);
				if(Events & COREEVENT_HOOK_HIT_NOHOOK)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_HOOK_NOATTACH, 1.0f, Pos);
				if(Events & COREEVENT_HOOK_ATTACH_PLAYER)
				{
					PredictedWorld().CreatePredictedSound(Pos, SOUND_HOOK_ATTACH_PLAYER, pLocalChar->GetCid());
				}
			}
		}

		// check if we want to trigger predicted airjump for dummy
		if(AntiPingPlayers() && pDummyChar && Tick > OtherState.m_Runtime.m_LastNewPredictedTick)
		{
			OtherState.m_Runtime.m_LastNewPredictedTick = Tick;
			vec2 Pos = pDummyChar->Core()->m_Pos;
			int Events = pDummyChar->Core()->m_TriggeredEvents;
			if(g_Config.m_ClPredict && !m_SuppressEvents)
				if(Events & COREEVENT_AIR_JUMP)
					m_Effects.AirJump(ActiveState, Pos, pDummyChar->GetCid(), 1.0f, 1.0f);
		}

		HandlePredictedEvents(Tick);
	}

	// detect mispredictions of other players and make corrections smoother when possible
	if(g_Config.m_ClAntiPingSmooth && Predict() && AntiPingPlayers() && m_NewTick && Runtime.m_LegacyPredictedTick >= MIN_TICK && absolute(Runtime.m_LegacyPredictedTick - Sessions()->PredGameTick(SessionId)) <= 1 && absolute(Sessions()->GameTick(SessionId) - Sessions()->PrevGameTick(SessionId)) <= 2)
	{
		int PredTime = std::clamp(Sessions()->GetPredictionTime(SessionId), 0, 800);
		float SmoothPace = 4 - 1.5f * PredTime / 800.f; // smoothing pace (a lower value will make the smoothing quicker)
		int64_t Len = 1000 * PredTime * SmoothPace;

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!Snap().m_aCharacters[i].m_Active || i == Snap().m_LocalClientId || !Runtime.m_aLastPredictedActive[i])
				continue;
			vec2 NewPos = (Runtime.m_LegacyPredictedTick == Sessions()->PredGameTick(SessionId)) ? m_aClients[i].m_Predicted.m_Pos : m_aClients[i].m_PrevPredicted.m_Pos;
			vec2 PredErr = (Runtime.m_aLastPredictedPosition[i] - NewPos) / (float)std::min(Sessions()->GetPredictionTime(SessionId), 200);
			if(in_range(length(PredErr), 0.05f, 5.f))
			{
				vec2 PredPos = mix(m_aClients[i].m_PrevPredicted.m_Pos, m_aClients[i].m_Predicted.m_Pos, Sessions()->PredIntraGameTick(SessionId));
				vec2 CurPos = mix(
					vec2(Snap().m_aCharacters[i].m_Prev.m_X, Snap().m_aCharacters[i].m_Prev.m_Y),
					vec2(Snap().m_aCharacters[i].m_Cur.m_X, Snap().m_aCharacters[i].m_Cur.m_Y),
					Sessions()->IntraGameTick(SessionId));
				vec2 RenderDiff = PredPos - aBeforeRender[i];
				vec2 PredDiff = PredPos - CurPos;

				float aMixAmount[2];
				for(int j = 0; j < 2; j++)
				{
					aMixAmount[j] = 1.0f;
					if(absolute(PredErr[j]) > 0.05f)
					{
						aMixAmount[j] = 0.0f;
						if(absolute(RenderDiff[j]) > 0.01f)
						{
							aMixAmount[j] = 1.f - std::clamp(RenderDiff[j] / PredDiff[j], 0.f, 1.f);
							aMixAmount[j] = 1.f - std::pow(1.f - aMixAmount[j], 1 / 1.2f);
						}
					}
					CGameState::CClientPredictionHistory &PredictionHistory = ActiveState.PredictionHistory(i);
					int64_t TimePassed = time_get() - PredictionHistory.m_aSmoothStart[j];
					if(in_range(TimePassed, (int64_t)0, Len - 1))
						aMixAmount[j] = std::min(aMixAmount[j], (float)(TimePassed / (double)Len));
				}
				for(int j = 0; j < 2; j++)
					if(absolute(RenderDiff[j]) < 0.01f && absolute(PredDiff[j]) < 0.01f && absolute(m_aClients[i].m_PrevPredicted.m_Pos[j] - m_aClients[i].m_Predicted.m_Pos[j]) < 0.01f && aMixAmount[j] > aMixAmount[j ^ 1])
						aMixAmount[j] = aMixAmount[j ^ 1];
				for(int j = 0; j < 2; j++)
				{
					// don't smooth for longer than 700ms, or more than 300ms longer along one axis than the other axis
					int64_t Remaining = std::min({
						(1.f - aMixAmount[j]) * Len,
						time_freq() * 0.700f,
						(1.f - aMixAmount[j ^ 1]) * Len + time_freq() * 0.300f,
					});
					int64_t Start = time_get() - (Len - Remaining);
					CGameState::CClientPredictionHistory &PredictionHistory = ActiveState.PredictionHistory(i);
					if(!in_range(Start + Len, PredictionHistory.m_aSmoothStart[j], PredictionHistory.m_aSmoothStart[j] + Len))
					{
						PredictionHistory.m_aSmoothStart[j] = Start;
						PredictionHistory.m_aSmoothLen[j] = Len;
					}
				}
			}
		}
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(Snap().m_aCharacters[i].m_Active)
		{
			CGameState::CPredictedClient &PredictedClient = ActiveState.PredictedClient(i);
			PredictedClient.m_HasPrev = true;
			PredictedClient.m_Prev = m_aClients[i].m_PrevPredicted;
			PredictedClient.m_HasCurrent = true;
			PredictedClient.m_Current = m_aClients[i].m_Predicted;
			Runtime.m_aLastPredictedPosition[i] = m_aClients[i].m_Predicted.m_Pos;
			Runtime.m_aLastPredictedActive[i] = true;
		}
		else
		{
			Runtime.m_aLastPredictedActive[i] = false;
		}
	}

	if(g_Config.m_Debug && g_Config.m_ClPredict && Runtime.m_LegacyPredictedTick == Sessions()->PredGameTick(SessionId))
	{
		CNetObj_CharacterCore Before = {0}, Now = {0}, BeforePrev = {0}, NowPrev = {0};
		BeforeChar.Write(&Before);
		BeforePrevChar.Write(&BeforePrev);
		m_PredictedChar.Write(&Now);
		m_PredictedPrevChar.Write(&NowPrev);

		if(mem_comp(&Before, &Now, sizeof(CNetObj_CharacterCore)) != 0)
		{
			log_trace("client", "prediction error");
			for(unsigned i = 0; i < sizeof(CNetObj_CharacterCore) / sizeof(int); i++)
			{
				if(((int *)&Before)[i] != ((int *)&Now)[i])
				{
					log_trace("client", "	%d %d %d (%d %d)", i, ((int *)&Before)[i], ((int *)&Now)[i], ((int *)&BeforePrev)[i], ((int *)&NowPrev)[i]);
				}
			}
		}
	}

	Runtime.m_LegacyPredictedTick = Sessions()->PredGameTick(SessionId);

	if(m_NewPredictedTick)
		m_Ghost.OnNewPredictedSnapshot();
}

void CGameClient::UpdatePrediction()
{
	const CSessionId SessionId = InputSessionId();
	CGameState &ActiveState = GameState(SessionId);
	const CGameState &OtherState = SessionContext(SessionId).SeatState(ActiveState.m_Seat ^ 1);
	CGameState::CRuntimeState &Runtime = ActiveState.m_Runtime;
	GameWorld().m_WorldConfig.m_IsVanilla = FocusedGameInfo().m_PredictVanilla;
	GameWorld().m_WorldConfig.m_IsDDRace = FocusedGameInfo().m_PredictDDRace;
	GameWorld().m_WorldConfig.m_IsFNG = FocusedGameInfo().m_PredictFNG;
	GameWorld().m_WorldConfig.m_PredictDDRace = FocusedGameInfo().m_PredictDDRace;
	GameWorld().m_WorldConfig.m_PredictTiles = FocusedGameInfo().m_PredictDDRace && FocusedGameInfo().m_PredictDDRaceTiles;
	GameWorld().m_WorldConfig.m_PredictFreeze = g_Config.m_ClPredictFreeze;
	GameWorld().m_WorldConfig.m_PredictWeapons = AntiPingWeapons();
	GameWorld().m_WorldConfig.m_BugDDRaceInput = FocusedGameInfo().m_BugDDRaceInput;
	GameWorld().m_WorldConfig.m_NoWeakHookAndBounce = FocusedGameInfo().m_NoWeakHookAndBounce;
	GameWorld().m_WorldConfig.m_PredictEvents = FocusedGameInfo().m_PredictEvents;
	GameWorld().m_WorldConfig.m_OldLaser = FocusedGameInfo().m_OldLaser;

	if(!Snap().m_pLocalCharacter)
	{
		if(CCharacter *pLocalChar = GameWorld().GetCharacterById(Snap().m_LocalClientId))
			pLocalChar->Destroy();
		return;
	}

	if(Snap().m_pLocalCharacter->m_AmmoCount > 0 && Snap().m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		GameWorld().m_WorldConfig.m_InfiniteAmmo = false;
	const CTuningParams &CurrentTuning = Runtime.m_CurrentTuning;
	GameWorld().m_WorldConfig.m_IsSolo = !Snap().m_aCharacters[Snap().m_LocalClientId].m_HasExtendedData && !CurrentTuning.m_PlayerCollision && !CurrentTuning.m_PlayerHooking;

	CCharacter *pLocalChar = GameWorld().GetCharacterById(Snap().m_LocalClientId);
	CCharacter *pDummyChar = nullptr;
	const bool PredictOther = PredictDummy(OtherState);
	if(PredictOther)
		pDummyChar = GameWorld().GetCharacterById(OtherState.LocalClientId());

	// update strong and weak hook
	if(pLocalChar && !Snap().m_SpecInfo.m_Active && !Client()->IsDemoPlayback() && (CurrentTuning.m_PlayerCollision || CurrentTuning.m_PlayerHooking))
	{
		if(Snap().m_aCharacters[Snap().m_LocalClientId].m_HasExtendedData)
		{
			int aIds[MAX_CLIENTS];
			std::fill(std::begin(aIds), std::end(aIds), -1);
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = GameWorld().GetCharacterById(i))
					aIds[pChar->GetStrongWeakId()] = i;
			for(int Id : aIds)
				if(Id >= 0)
					Runtime.m_CharOrder.GiveStrong(Id);
		}
		else
		{
			// manual detection
			DetectStrongHook(Runtime);
		}
		for(int i : Runtime.m_CharOrder.m_Ids)
		{
			if(CCharacter *pChar = GameWorld().GetCharacterById(i))
			{
				GameWorld().RemoveEntity(pChar);
				GameWorld().InsertEntity(pChar);
			}
		}
	}

	// advance the gameworld to the current gametick
	if(pLocalChar && absolute(GameWorld().GameTick() - Sessions()->GameTick(SessionId)) < Sessions()->GameTickSpeed())
	{
		for(int Tick = GameWorld().GameTick() + 1; Tick <= Sessions()->GameTick(SessionId); Tick++)
		{
			CNetObj_PlayerInput *pInput = (CNetObj_PlayerInput *)Sessions()->GetInput(SessionId, Tick);
			CNetObj_PlayerInput *pDummyInput = nullptr;
			if(pDummyChar)
				pDummyInput = (CNetObj_PlayerInput *)Sessions()->GetInput(OtherState.m_SessionId, Tick);
			if(pInput)
				pLocalChar->OnDirectInput(pInput);
			if(pDummyInput)
				pDummyChar->OnDirectInput(pDummyInput);

			ApplyPreInputs(Tick, true, GameWorld());

			GameWorld().m_GameTick = Tick;
			if(pInput)
				pLocalChar->OnPredictedInput(pInput);
			if(pDummyInput)
				pDummyChar->OnPredictedInput(pDummyInput);

			ApplyPreInputs(Tick, false, GameWorld());

			GameWorld().Tick();

			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = GameWorld().GetCharacterById(i))
				{
					ActiveState.PredictionHistory(i).m_aPredPos[Tick % 200] = pChar->Core()->m_Pos;
					ActiveState.PredictionHistory(i).m_aPredTick[Tick % 200] = Tick;
				}
		}
	}
	else
	{
		// skip to current gametick
		GameWorld().m_GameTick = Sessions()->GameTick(SessionId);
		if(pLocalChar)
			if(CNetObj_PlayerInput *pInput = (CNetObj_PlayerInput *)Sessions()->GetInput(SessionId, Sessions()->GameTick(SessionId)))
				pLocalChar->SetInput(pInput);
		if(pDummyChar)
			if(CNetObj_PlayerInput *pInput = (CNetObj_PlayerInput *)Sessions()->GetInput(OtherState.m_SessionId, Sessions()->GameTick(SessionId)))
				pDummyChar->SetInput(pInput);
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(CCharacter *pChar = GameWorld().GetCharacterById(i))
		{
			ActiveState.PredictionHistory(i).m_aPredPos[Sessions()->GameTick(SessionId) % 200] = pChar->Core()->m_Pos;
			ActiveState.PredictionHistory(i).m_aPredTick[Sessions()->GameTick(SessionId) % 200] = Sessions()->GameTick(SessionId);
		}

	// update the local gameworld with the new snapshot
	GameWorld().NetObjBegin(FocusedTeams(), Snap().m_LocalClientId);

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(Snap().m_aCharacters[i].m_Active)
		{
			bool IsLocal = i == Snap().m_LocalClientId || (PredictOther && i == OtherState.LocalClientId());
			int GameTeam = IsTeamPlay() ? m_aClients[i].m_Team : i;
			GameWorld().NetCharAdd(i, &Snap().m_aCharacters[i].m_Cur,
				Snap().m_aCharacters[i].m_HasExtendedData ? &Snap().m_aCharacters[i].m_ExtendedData : nullptr,
				GameTeam, IsLocal);
		}

	for(const CSnapEntities &EntData : SnapEntities())
		GameWorld().NetObjAdd(EntData.m_Item.m_Id, EntData.m_Item.m_Type, EntData.m_Item.m_pData, EntData.m_pDataEx);

	GameWorld().NetObjEnd();
}
