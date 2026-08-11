#!/usr/bin/env python3

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
CORE_FILES = (
	"src/engine/client/connection.h",
	"src/engine/client/session.cpp",
	"src/engine/client/session.h",
	"src/engine/client/session_sources.cpp",
	"src/engine/client/session_sources.h",
	"src/engine/client/stream.h",
	"src/game/client/game_state.cpp",
	"src/game/client/game_state.h",
	"src/game/client/game_view.cpp",
	"src/game/client/game_view.h",
	"src/game/client/input_policy.cpp",
	"src/game/client/input_policy.h",
	"src/game/client/local_player_profile.cpp",
	"src/game/client/local_player_profile.h",
	"src/game/client/map_context.cpp",
	"src/game/client/map_context.h",
	"src/game/client/session_context.h",
)
RENDER_FILES = (
	"src/game/client/render.cpp",
	"src/game/client/render.h",
)
CONTEXT_RENDER_FILES = ("src/game/client/components/players_context.cpp",)
RENDER_PURITY_FUNCTIONS = (
	("src/game/client/components/ghost.cpp", re.compile(r"\bCGhost::OnRender\s*\(")),
	("src/game/client/components/items.cpp", re.compile(r"\bCItems::(?:OnRender|Render[A-Za-z0-9_]*)\s*\(")),
	("src/game/client/components/players.cpp", re.compile(r"\bCPlayers::(?:OnRender|Render[A-Za-z0-9_]*)\s*\(")),
)
CAMERA_FILES = ("src/game/client/components/camera.h",)
CONTROLS_OWNER_FILES = ("src/game/client/components/controls.h",)
BROADCAST_OWNER_FILES = ("src/game/client/components/broadcast.h",)
DAMAGE_INDICATOR_FILES = (
	"src/game/client/components/damageind.cpp",
	"src/game/client/components/damageind.h",
)
EFFECT_OWNER_FILES = ("src/game/client/components/effects.h",)
GHOST_OWNER_FILES = ("src/game/client/components/ghost.h",)
PREDICTION_ENTITY_FILES = (
	"src/game/client/prediction/entity.cpp",
	"src/game/client/prediction/entity.h",
)
PARTICLE_OWNER_FILES = ("src/game/client/components/particles.h",)
RENDER_TIMING_FILES = (
	"src/game/client/components/items.cpp",
	"src/game/client/components/players.cpp",
	"src/game/client/components/race_demo.cpp",
)
GAME_CLIENT_OWNER_FILES = ("src/game/client/gameclient.h",)
HUD_OWNER_FILES = ("src/game/client/components/hud.h",)
EMOTICON_OWNER_FILES = ("src/game/client/components/emoticon.h",)
MOTD_OWNER_FILES = ("src/game/client/components/motd.h",)
SPECTATOR_OWNER_FILES = ("src/game/client/components/spectator.h",)
VOTING_OWNER_FILES = ("src/game/client/components/voting.h",)
SIXUP_VOTE_FILES = ("src/game/client/sixup_translate_game.cpp",)
GAME_CLIENT_FILES = tuple(path.relative_to(ROOT).as_posix() for path in (ROOT / "src/game/client").rglob("*") if path.suffix in (".cpp", ".h"))
FORBIDDEN_CORE = (
	re.compile(r"g_Config\.m_ClDummy\b"),
	re.compile(r"\bNUM_DUMMIES\b"),
	re.compile(r"\bSTATE_DEMOPLAYBACK\b"),
	re.compile(r"\bCUi\b"),
	re.compile(r"\b(?:IGraphics|ITextRender|STextContainerIndex)\b"),
	re.compile(r"game/client/ui\.h"),
	re.compile(r"engine/textrender\.h"),
	re.compile(r"Snap(?:GetItem|FindItem|NumItems)\(\s*(?:IClient::)?SNAP_"),
)
FORBIDDEN_RENDER = (
	re.compile(r"g_Config\.m_ClDummy\b"),
	re.compile(r"\bNUM_DUMMIES\b"),
)
FORBIDDEN_CONTEXT_RENDER = (
	re.compile(r"\bGameClient\(\)"),
	re.compile(r"\bActiveConnection\(\)"),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\bClient\(\)->State\(\)"),
)
FORBIDDEN_RENDER_MUTATION = (
	re.compile(r"GameClient\(\)->m_(?:Effects|Sounds)\."),
	re.compile(r"\bm_Last(?:Presentation|Render)Tick\s*="),
)
FORBIDDEN_GAME_CLIENT = (
	re.compile(r"\bCStreamStorage\b"),
	re.compile(r"\bCFlow\b"),
	re.compile(r"\bm_Flow(?:Affected)?\b"),
	re.compile(r"\bm_aSixup\b"),
	re.compile(r"\bSelect(?:ed)?Sixup\b"),
	re.compile(r"\b(?:m_DummyInput|m_DummyFire|m_IsDummySwapping)\b"),
	re.compile(r"\bm_CursorInfo\b"),
	re.compile(r"ActiveConnection\(\)\s*\^"),
	re.compile(r"GameState\(\s*!?Dummy\s*\)"),
)
FORBIDDEN_CAMERA = (
	re.compile(r"^\s*(?:bool|int|float|vec2|ivec2|CCubicBezier)\s+m_[A-Za-z0-9_]+(?:\s*=[^;]+)?;\s*$"),
	re.compile(r"\bOnRender\s*\("),
)
FORBIDDEN_CONTROLS_OWNER = (re.compile(r"\bOnRender\s*\("),)
FORBIDDEN_BROADCAST_OWNER = (re.compile(r"\b(?:m_aBroadcastText|m_BroadcastTick)\b"),)
FORBIDDEN_DAMAGE_INDICATOR = (re.compile(r"\b(?:m_aItems|m_NumItems|s_LastLocalTime|CDamageInd::OnReset)\b"),)
FORBIDDEN_EFFECT_OWNER = (re.compile(r"\b(?:m_Add5hz|m_LastUpdate5hz|m_Add50hz|m_LastUpdate50hz|m_Add100hz|m_LastUpdate100hz|m_SkidSoundTimer)\b"),)
FORBIDDEN_GHOST_OWNER = (re.compile(r"\bm_PlaybackPos\b"),)
FORBIDDEN_PREDICTION_ENTITY = (re.compile(r"\bm_LastRenderTick\b"),)
FORBIDDEN_PARTICLE_OWNER = (re.compile(r"\b(?:m_vParticles|m_FirstFree|m_aFirstPart|m_NumParticles|m_FrictionFraction|m_LastRenderTime|m_TimeInitialized|OnReset)\b"),)
FORBIDDEN_RENDER_TIMING = (re.compile(r"\b(?:s_LastGameTickTime|s_LastPredIntraTick|s_LastLocalTime|s_LastIteX|s_LastRaceTick|s_Time)\b"),)
FORBIDDEN_GAME_CLIENT_OWNER = (
	re.compile(r"\b(?:CClientStats|m_aLastUpdateTick|m_aStats|m_CharOrder|m_GameInfo|m_ServerMode|m_Teams)\b"),
	re.compile(r"\b(?:m_MapBestTimeSeconds|m_MapBestTimeMillis|m_aMapDescription)\b"),
	re.compile(r"\b(?:SMultiView|m_MultiView|m_MultiViewTeam|m_MultiViewPersonalZoom|m_MultiViewShowHud|m_MultiViewActivated|m_aMultiViewId)\b"),
	re.compile(r"\b(?:m_PredictedTick|m_LastRoundStartTick|m_LastRaceTick|m_LastFlagCarrierRed|m_LastFlagCarrierBlue|m_aLastPos|m_aLastActive|m_GameOver|m_GamePaused|m_aFlagDropTick|m_ReceivedDDNetPlayer|m_ReceivedDDNetPlayerFinishTimes|m_ReceivedDDNetPlayerFinishTimesMillis)\b"),
	re.compile(r"\b(?:m_Jetpack|m_CollisionDisabled|m_EndlessHook|m_EndlessJump|m_HammerHitDisabled|m_GrenadeHitDisabled|m_LaserHitDisabled|m_ShotgunHitDisabled|m_HookHitDisabled|m_Super|m_Invincible|m_HasTelegunGun|m_HasTelegunGrenade|m_HasTelegunLaser|m_FreezeEnd|m_DeepFrozen|m_LiveFrozen)\b"),
	re.compile(r"\b(?:CCursorInfo|m_CursorOwnerId|m_aTargetSamplesTime|m_aTargetSamplesData|m_NumSamples)\b"),
	re.compile(r"\b(?:m_AuthLevel|m_Afk|m_Paused|m_Spec|m_FinishTimeSeconds|m_FinishTimeMillis|m_SpecCharPresent|m_SpecChar)\b"),
)
FORBIDDEN_HUD_OWNER = (re.compile(r"\b(?:m_TimeCpDiff|m_FinishTimeDiff|m_DDRaceTime|m_FinishTimeLastReceivedTick|m_TimeCpLastReceivedTick|m_ShowFinishTime)\b"),)
FORBIDDEN_EMOTICON_OWNER = (re.compile(r"\b(?:m_WasActive|m_Active|m_SelectorMouse|m_SelectedEmote|m_SelectedEyeEmote|m_TouchPressedOutside)\b"),)
FORBIDDEN_MOTD_OWNER = (re.compile(r"\b(?:m_aServerMotd|m_ServerMotdTime|m_ServerMotdUpdateTime)\b"),)
FORBIDDEN_SPECTATOR_OWNER = (re.compile(r"\b(?:m_Active|m_WasActive|m_SelectedSpectatorId|m_SelectorMouse|m_MultiViewActivateDelay|m_MultiViewActivateTime)\b"),)
FORBIDDEN_VOTING_OWNER = (re.compile(r"\b(?:CHeap|CVoteOptionClient|m_Opentime|m_Closetime|m_aDescription|m_aReason|m_Voted|m_Yes|m_No|m_Pass|m_Total|m_ReceivingOptions|m_NumVoteOptions|m_pFirst|m_pLast|m_pRecycleFirst|m_pRecycleLast)\b"),)
FORBIDDEN_SIXUP_VOTE = (re.compile(r"m_Voting\.OnReset\("),)


def check(paths: tuple[str, ...], patterns: tuple[re.Pattern[str], ...]) -> list[str]:
	errors = []
	for relative in paths:
		text = (ROOT / relative).read_text(encoding="utf-8")
		for line_number, line in enumerate(text.splitlines(), 1):
			for pattern in patterns:
				if pattern.search(line):
					errors.append(f"{relative}:{line_number}: forbidden architecture dependency: {line.strip()}")
	return errors


def check_function_bodies(functions: tuple[tuple[str, re.Pattern[str]], ...], patterns: tuple[re.Pattern[str], ...]) -> list[str]:
	errors = []
	for relative, function_pattern in functions:
		text = (ROOT / relative).read_text(encoding="utf-8")
		for function_match in function_pattern.finditer(text):
			body_start = text.find("{", function_match.end())
			if body_start < 0:
				continue
			depth = 1
			body_end = body_start + 1
			while body_end < len(text) and depth > 0:
				if text[body_end] == "{":
					depth += 1
				elif text[body_end] == "}":
					depth -= 1
				body_end += 1
			body = text[body_start:body_end]
			for pattern in patterns:
				for match in pattern.finditer(body):
					line_number = text.count("\n", 0, body_start + match.start()) + 1
					errors.append(f"{relative}:{line_number}: forbidden render mutation: {match.group(0)}")
	return errors


errors = (
	check(CORE_FILES, FORBIDDEN_CORE)
	+ check(RENDER_FILES, FORBIDDEN_RENDER)
	+ check(CONTEXT_RENDER_FILES, FORBIDDEN_CONTEXT_RENDER)
	+ check_function_bodies(RENDER_PURITY_FUNCTIONS, FORBIDDEN_RENDER_MUTATION)
	+ check(CAMERA_FILES, FORBIDDEN_CAMERA)
	+ check(CONTROLS_OWNER_FILES, FORBIDDEN_CONTROLS_OWNER)
	+ check(BROADCAST_OWNER_FILES, FORBIDDEN_BROADCAST_OWNER)
	+ check(DAMAGE_INDICATOR_FILES, FORBIDDEN_DAMAGE_INDICATOR)
	+ check(EFFECT_OWNER_FILES, FORBIDDEN_EFFECT_OWNER)
	+ check(GHOST_OWNER_FILES, FORBIDDEN_GHOST_OWNER)
	+ check(PREDICTION_ENTITY_FILES, FORBIDDEN_PREDICTION_ENTITY)
	+ check(PARTICLE_OWNER_FILES, FORBIDDEN_PARTICLE_OWNER)
	+ check(RENDER_TIMING_FILES, FORBIDDEN_RENDER_TIMING)
	+ check(GAME_CLIENT_OWNER_FILES, FORBIDDEN_GAME_CLIENT_OWNER)
	+ check(HUD_OWNER_FILES, FORBIDDEN_HUD_OWNER)
	+ check(EMOTICON_OWNER_FILES, FORBIDDEN_EMOTICON_OWNER)
	+ check(MOTD_OWNER_FILES, FORBIDDEN_MOTD_OWNER)
	+ check(SPECTATOR_OWNER_FILES, FORBIDDEN_SPECTATOR_OWNER)
	+ check(VOTING_OWNER_FILES, FORBIDDEN_VOTING_OWNER)
	+ check(SIXUP_VOTE_FILES, FORBIDDEN_SIXUP_VOTE)
	+ check(GAME_CLIENT_FILES, FORBIDDEN_GAME_CLIENT)
)
if errors:
	print("\n".join(errors), file=sys.stderr)
	sys.exit(1)
print("client multi-state architecture boundaries: OK")
