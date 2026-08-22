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
	"src/engine/shared/translation_context.cpp",
	"src/engine/shared/translation_context.h",
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
CONTEXT_RENDER_FUNCTIONS = (("src/game/client/components/players.cpp", re.compile(r"\bCPlayers::RenderSpectatorCharacters\s*\(")),)
RENDER_PURITY_FUNCTIONS = (
	("src/game/client/components/ghost.cpp", re.compile(r"\bCGhost::OnRender\s*\(")),
	("src/game/client/components/items.cpp", re.compile(r"\bCItems::(?:OnRender|Render[A-Za-z0-9_]*)\s*\(")),
	("src/game/client/components/players.cpp", re.compile(r"\bCPlayers::(?:OnRender|Render[A-Za-z0-9_]*)\s*\(")),
)
PRESENTATION_UPDATE_FUNCTIONS = (
	("src/game/client/components/ghost.cpp", re.compile(r"\bCGhost::UpdatePresentation\s*\(")),
	("src/game/client/components/items.cpp", re.compile(r"\bCItems::UpdatePresentation\s*\(")),
	("src/game/client/components/players.cpp", re.compile(r"\bCPlayers::UpdatePresentation\s*\(")),
)
OVERLAY_CONTEXT_RENDER_FUNCTIONS = (
	("src/game/client/components/broadcast.cpp", re.compile(r"\bCBroadcast::(?:OnRender|RenderServerBroadcast)\s*\(")),
	("src/game/client/components/motd.cpp", re.compile(r"\bCMotd::OnRender\s*\(")),
)
OVERLAY_VISUAL_FUNCTIONS = (
	("src/game/client/components/chat.cpp", re.compile(r"\bCChat::OnRender\s*\(")),
	("src/game/client/components/emoticon.cpp", re.compile(r"\bCEmoticon::OnRender\s*\(")),
	("src/game/client/components/spectator.cpp", re.compile(r"\bCSpectator::OnRender\s*\(")),
	("src/game/client/components/statboard.cpp", re.compile(r"\bCStatboard::OnRender\s*\(")),
)
ITEMS_PRESENTATION_FUNCTIONS = (("src/game/client/components/items.cpp", re.compile(r"\bCItems::UpdatePresentation\s*\(")),)
ITEMS_RENDER_FUNCTIONS = (("src/game/client/components/items.cpp", re.compile(r"\bCItems::(?:OnRender|Render[A-Za-z0-9_]*)\s*\(")),)
PLAYERS_STATE_FUNCTIONS = (("src/game/client/components/players.cpp", re.compile(r"\bCPlayers::(?:GetPlayerTargetAngle|IntersectCharacter|IsPlayerInfoAvailable|OnRender|PreparePlayerRenderState|RenderHook|RenderHookCollLine|RenderPlayer|UpdatePlayerPresentation|UpdatePresentation|UpdateRenderedClients)\s*\(")),)
NAMEPLATES_STATE_FUNCTIONS = (("src/game/client/components/nameplates.cpp", re.compile(r"\bCNamePlates::(?:OnRender|RenderNamePlateGame)\s*\(")),)
DEBUG_HUD_CONTEXT_FUNCTIONS = (("src/game/client/components/debughud.cpp", re.compile(r"\bCDebugHud::(?:OnRender|RenderNetCorrections|RenderTuning)\s*\(")),)
IMPORTANT_ALERT_CONTEXT_FUNCTIONS = (("src/game/client/components/important_alert.cpp", re.compile(r"\bCImportantAlert::(?:OnRender|RenderImportantAlert)\s*\(")),)
HUD_SPECTATOR_COUNT_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::RenderSpectatorCount\s*\(")),)
HUD_STATUS_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::(?:RenderDDRaceEffects|RenderPauseNotification|RenderSuddenDeath|RenderWarmupTimer)\s*\(")),)
HUD_SCORE_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::RenderScoreHud\s*\(")),)
HUD_INFO_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::(?:RenderConnectionWarning|RenderLocalTime|RenderTeambalanceWarning|RenderTextInfo)\s*\(")),)
HUD_MOVEMENT_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::(?:GetMovementInformation|RenderDummyActions|RenderMovementInformation)\s*\(")),)
HUD_SPECTATOR_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::RenderSpectatorHud\s*\(")),)
HUD_ROOT_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::(?:OnRender|RenderAmmoHealthAndArmor)\s*\(")),)
HUD_CURSOR_CONTEXT_FUNCTIONS = (("src/game/client/components/hud.cpp", re.compile(r"\bCHud::RenderCursor\s*\(")),)
SCOREBOARD_CONTEXT_FUNCTIONS = (
	("src/game/client/components/scoreboard.cpp", re.compile(r"\bCScoreboard::(?:GetTeamName|OnRender|RenderGoals|RenderScoreboard|RenderSpectators|RenderTitle|RenderTitleBar|RenderTitleScore)\s*\(")),
	("src/game/client/components/scoreboard.cpp", re.compile(r"\bCScoreboard::IsActive\s*\(\s*const CRenderContext\s*&")),
)
SCOREBOARD_VISUAL_FUNCTIONS = (("src/game/client/components/scoreboard.cpp", re.compile(r"\bCScoreboard::(?:OnRender|RenderGoals|RenderScoreboard|RenderSpectators|RenderTitle|RenderTitleBar|RenderTitleScore)\s*\(")),)
STATBOARD_CONTEXT_FUNCTIONS = (
	("src/game/client/components/statboard.cpp", re.compile(r"\bCStatboard::(?:OnRender|RenderGlobalStats)\s*\(")),
	("src/game/client/components/statboard.cpp", re.compile(r"\bCStatboard::IsRenderable\s*\(\s*const CRenderContext\s*&")),
)
INFO_MESSAGES_CONTEXT_FUNCTIONS = (("src/game/client/components/infomessages.cpp", re.compile(r"\bCInfoMessages::(?:CreateTextContainersIfNotCreated|OnRender|RenderFinishMsg|RenderKillMsg)\s*\(")),)
INFO_MESSAGES_MESSAGE_FUNCTIONS = (("src/game/client/components/infomessages.cpp", re.compile(r"\bCInfoMessages::(?:HandleMessage|OnKillMessage|OnRaceFinishMessage|OnTeamKillMessage)\s*\(")),)
VOTING_CONTEXT_FUNCTIONS = (("src/game/client/components/voting.cpp", re.compile(r"\bCVoting::(?:Render|RenderBars)\s*\(")),)
VOTING_MESSAGE_FUNCTIONS = (("src/game/client/components/voting.cpp", re.compile(r"\bCVoting::HandleMessage\s*\(")),)
CHAT_CONTEXT_FUNCTIONS = (("src/game/client/components/chat.cpp", re.compile(r"\bCChat::(?:OnPrepareLines|OnRender|RenderLines)\s*\(")),)
CHAT_MESSAGE_FUNCTIONS = (("src/game/client/components/chat.cpp", re.compile(r"\bCChat::(?:AddLine|CacheAppearance|HandleMessage|StoreSave)\s*\(")),)
CHAT_APPLICATION_FUNCTIONS = (("src/game/client/components/chat.cpp", re.compile(r"\bCChat::(?:RenderApplicationOverlay|UpdateController)\s*\(")),)
CHAT_SAVE_FUNCTIONS = (
	("src/game/client/components/chat.cpp", re.compile(r"\bCChat::StoreSave\s*\(")),
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::StoreSave\s*\(")),
)
EMOTICON_CONTEXT_FUNCTIONS = (("src/game/client/components/emoticon.cpp", re.compile(r"\bCEmoticon::(?:EyeWheelAvailable|OnRender|UpdateController)\s*\(")),)
SPECTATOR_CONTEXT_FUNCTIONS = (("src/game/client/components/spectator.cpp", re.compile(r"\bCSpectator::(?:CommitController|OnRender|UpdateController)\s*\(")),)
SPECTATOR_RENDER_FUNCTIONS = (("src/game/client/components/spectator.cpp", re.compile(r"\bCSpectator::OnRender\s*\(")),)
TOUCH_CONTROLLER_FUNCTIONS = (
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::(?:BindController|CalculateScreenSize|CancelController|InitVisibilityFunctions|NextDirectTouchAction|UpdateButtonsEditor|UpdateButtonsGame|UpdateController)\s*\(")),
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::CJoystickTouchButtonBehavior::OnUpdate\s*\(")),
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::CTouchButton::UpdateVisibility(?:Editor|Game)\s*\(")),
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::CTouchButtonBehavior::Set(?:Active|Inactive)\s*\(")),
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::C[A-Za-z0-9_]*TouchButtonBehavior::(?:OnActivate|OnDeactivate|OnUpdate)\s*\(")),
)
TOUCH_COMMAND_ROUTER_FUNCTIONS = (("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::ExecuteStroked\s*\(")),)
TOUCH_RENDER_FUNCTIONS = (
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::(?:CalculateScreenSize|OnRender|RenderApplicationOverlay|RenderButtonsEditor|RenderButtonsGame)\s*\(")),
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::CTouchButton::Render\s*\(")),
	("src/game/client/components/touch_controls.cpp", re.compile(r"\bCTouchControls::C[A-Za-z0-9_]*TouchButtonBehavior::GetLabel\s*\(")),
)
# The per session statistics update moved into the match collector.
SESSION_STATS_UPDATE_FUNCTIONS = (("src/game/client/match_collector.cpp", re.compile(r"\bCSessionStatsState::UpdateSnapshot\s*\(")),)
GAME_CLIENT_RENDER_FUNCTIONS = (("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::OnRender\s*\(")),)
CLIENT_UPDATE_FUNCTIONS = (("src/engine/client/client.cpp", re.compile(r"\bCClient::Update\s*\(")),)
SESSION_CLOSE_ADAPTER_FUNCTIONS = (("src/engine/client/client.cpp", re.compile(r"\bCClient::Disconnect(?:Demo)?WithReason\s*\(")),)
SESSION_MESSAGE_ROUTING_FUNCTIONS = (
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::OnMessage\s*\(")),
	("src/game/client/sixup_translate_game.cpp", re.compile(r"\bCGameClient::TranslateGameMsg\s*\(")),
)
DEMO_SEEK_FUNCTIONS = (("src/game/client/components/menus_demo.cpp", re.compile(r"\bCMenus::HandleDemoSeeking\s*\(")),)
RENDER_PROJECTION_FUNCTIONS = (("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::(?:GetSmoothPos|UpdateRenderedClients)\s*\(")),)
ENGINE_TIMING_QUERY_FUNCTIONS = (("src/engine/client/client.cpp", re.compile(r"\bCClient::(?:ConnectionProblems|GetPredictionTick|GetPredictionTime|GetSmoothTick)\s*\(")),)
PROCESS_SERVER_PACKET_FUNCTIONS = (("src/engine/client/client.cpp", re.compile(r"\bCClient::ProcessServerPacket\s*\(")),)
MAP_SOUNDS_UPDATE_FUNCTIONS = (("src/game/client/components/mapsounds.cpp", re.compile(r"\bCMapSounds::Update\s*\(")),)
MAP_SOUNDS_LOAD_FUNCTIONS = (("src/game/client/components/mapsounds.cpp", re.compile(r"\bCMapSounds::Load\s*\(")),)
SCENE_UPDATE_FUNCTIONS = (
	("src/game/client/components/effects.cpp", re.compile(r"\bCEffects::(?:SkidTrail|Update)\s*\(")),
	("src/game/client/components/particles.cpp", re.compile(r"\bCParticles::(?:Update|UpdatePhysics)\s*\(")),
	("src/game/client/components/damageind.cpp", re.compile(r"\bCDamageInd::Update\s*\(")),
)
MAP_LAYER_CONTEXT_RENDER_FUNCTIONS = (
	("src/game/client/components/background.cpp", re.compile(r"\bCBackground::OnRender\s*\(\s*const CRenderContext\s*&\s*Context\s*\)")),
	("src/game/client/components/maplayers.cpp", re.compile(r"\bCMapLayers::OnRender\s*\(\s*const CRenderContext\s*&\s*Context\s*\)")),
	("src/game/client/components/maplayers.cpp", re.compile(r"\bCMapLayers::Render\s*\(")),
)
WORLD_REQUEST_BOUNDS_FUNCTIONS = (
	("src/game/client/components/freezebars.cpp", re.compile(r"\bCFreezeBars::OnRender\s*\(")),
	("src/game/client/components/ghost.cpp", re.compile(r"\bCGhost::OnRender\s*\(")),
	("src/game/client/components/nameplates.cpp", re.compile(r"\bCNamePlates::RenderNamePlateGame\s*\(")),
	("src/game/client/components/players.cpp", re.compile(r"\bCPlayers::OnRender\s*\(")),
)
MAP_RENDER_IMAGE_FUNCTIONS = (("src/game/client/components/mapimages.cpp", re.compile(r"\bCMapRenderImages::(?:GetEntities|GetTuneColors|Load|SetGameInfo)\s*\(")),)
MAP_LAYER_BINDING_FUNCTIONS = (("src/game/client/components/maplayers.cpp", re.compile(r"\bCMapLayers::(?:Load|Unload)\s*\(")),)
SESSION_PRESENTATION_FUNCTIONS = (("src/game/client/session_presentation.cpp", re.compile(r"\bCSessionPresentation::(?:Load|PrepareRender|Unload|UpdateMapSounds)\s*\(")),)
SESSION_CLIENT_PRESENTATION_FUNCTIONS = (("src/game/client/session_presentation.cpp", re.compile(r"\bCSessionPresentation::(?:ApplyClientColors|CreateClientTee|GetClientSkinDescriptor|UpdateClients)\s*\(")),)
# Resetting one session must not reach into the others. This used to guard
# CGameClient::OnReset, which no longer exists; the per session resets it was
# split into carry the same rule.
SESSION_RESET_FUNCTIONS = (("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::Reset(?:Chat|InfoMessages)\s*\(")),)
SNAPSHOT_SOURCE_FUNCTIONS = (
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::OnNewSnapshot\s*\(")),
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::ProcessEvents\s*\(")),
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::ProcessSnapshot\s*\(")),
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::SnapCollectEntities\s*\(")),
)
SESSION_SNAPSHOT_FUNCTIONS = (
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::OnNewSnapshot\s*\(")),
	("src/game/client/game_state.cpp", re.compile(r"\bCGameState::ApplySnapshot\s*\(")),
)
DEMO_CONNECTION_FUNCTIONS = (("src/engine/client/client.cpp", re.compile(r"\bCClient::(?:DisconnectDemoWithReason|OnDemoPlayerSnapshot|UpdateDemoIntraTimers)\s*\(")),)
SESSION_LIFECYCLE_FUNCTIONS = (
	("src/engine/client/client.cpp", re.compile(r"\bCClient::(?:DisconnectWithReason|DisconnectDemoWithReason|LoadMap|LoadMapSearch)\s*\(")),
	("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::(?:BindLegacyWorld|OnConnected|OnSessionClosed|OnSessionFocused)\s*\(")),
)
SIXUP_SNAPSHOT_FUNCTIONS = (("src/game/client/sixup_translate_snapshot.cpp", re.compile(r"\bCGameClient::TranslateSnap\s*\(")),)
SESSION_MESSAGE_TIME_FUNCTIONS = (("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::SessionMessageTime\s*\(")),)
NETWORK_DUMMY_FUNCTIONS = (("src/game/client/gameclient.cpp", re.compile(r"\bCGameClient::(?:DummyResetInput|OnDummyDisconnect|SendDummyInfo|SendSkinChange7)\s*\(")),)
CAMERA_FILES = ("src/game/client/components/camera.h",)
CONTROLS_OWNER_FILES = ("src/game/client/components/controls.h",)
BROADCAST_OWNER_FILES = ("src/game/client/components/broadcast.h",)
DAMAGE_INDICATOR_FILES = (
	"src/game/client/components/damageind.cpp",
	"src/game/client/components/damageind.h",
)
SCENE_RENDER_FILES = (
	"src/game/client/components/damageind.cpp",
	"src/game/client/components/particles.cpp",
	"src/game/client/components/particles.h",
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
SOURCE_CALLBACK_OWNER_FILES = (
	"src/engine/client.h",
	"src/game/client/gameclient.h",
	"src/game/client/local_player_profile.h",
)
STABLE_STREAM_LOOKUP_FILES = (
	"src/game/client/gameclient.cpp",
	"src/game/client/sixup_translate_game.cpp",
	"src/game/client/sixup_translate_snapshot.cpp",
)
I_CLIENT_SOURCE_API_FILES = ("src/engine/client.h",)
ENGINE_TIMING_OWNER_FILES = ("src/engine/client/client.h",)
EXPLICIT_CONNECTION_TIMING_FILES = (
	"src/engine/client.h",
	"src/engine/client/client.cpp",
	"src/engine/client/client.h",
	"src/game/client/gameclient.cpp",
)
HUD_OWNER_FILES = ("src/game/client/components/hud.h",)
INFO_MESSAGES_OWNER_FILES = ("src/game/client/components/infomessages.h",)
CHAT_OWNER_FILES = ("src/game/client/components/chat.h",)
EMOTICON_OWNER_FILES = ("src/game/client/components/emoticon.h",)
MOTD_OWNER_FILES = ("src/game/client/components/motd.h",)
SPECTATOR_OWNER_FILES = ("src/game/client/components/spectator.h",)
TOUCH_OWNER_FILES = ("src/game/client/components/touch_controls.h",)
SOUNDS_OWNER_FILES = ("src/game/client/components/sounds.h",)
MAP_SOUNDS_OWNER_FILES = ("src/game/client/components/mapsounds.h",)
MAP_LAYER_OWNER_FILES = (
	"src/game/client/components/background.h",
	"src/game/client/components/maplayers.h",
)
MAP_RENDER_IMAGE_OWNER_FILES = ("src/game/client/components/mapimages.h",)
ENVELOPE_STATE_FILES = (
	"src/game/client/components/envelope_state.cpp",
	"src/game/client/components/envelope_state.h",
)
VOTING_OWNER_FILES = ("src/game/client/components/voting.h",)
SIXUP_VOTE_FILES = ("src/game/client/sixup_translate_game.cpp",)
SIXUP_CHAT_FILES = ("src/game/client/sixup_translate_game.cpp",)
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
FORBIDDEN_PRESENTATION_VIEW = (
	re.compile(r"\bCRenderContext\b"),
	re.compile(r"\bm_View\b"),
	re.compile(r"\bLegacyGameView\s*\("),
)
FORBIDDEN_PRESENTATION_EFFECT_ALPHA = (re.compile(r"m_Effects\.[^\n;]*\b(?:State\.m_Alpha|Alpha)\b"),)
FORBIDDEN_OVERLAY_RENDER_FOCUS = (
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\bClient\(\)->(?:State|GameTick|GameTickSpeed)\s*\("),
)
FORBIDDEN_OVERLAY_VISUAL_SIDE_EFFECT = (
	re.compile(r"\b(?:AutoStatCSV|AutoStatScreenshot|Emote|EyeEmote|SendChat|Spectate)\s*\("),
	re.compile(r"GameClient\(\)->ResetMultiView\s*\("),
)
FORBIDDEN_ITEMS_PRESENTATION_FOCUS = (
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bSnap(?:FindItem|GetItem|NumItems)\s*\("),
	re.compile(r"GameClient\(\)->m_(?:Snap|aClients|GameWorld|PrevPredictedWorld)\b"),
	re.compile(r"GameClient\(\)->(?:SnapEntities|SwitchStateTeam|IsLocalCharSuper|Switchers|GetTuning)\s*\("),
)
FORBIDDEN_ITEMS_RENDER_FOCUS = (
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bOtherConnection\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_(?:GameWorld|PrevPredictedWorld)\b"),
	re.compile(r"GameClient\(\)->(?:AntiPing[A-Za-z]*|GameState|GetTuning|IsLocalCharSuper|IsOtherTeam|Predict|SnapEntities|Switchers)\s*\("),
	re.compile(r"Graphics\(\)->GetScreen\s*\("),
)
FORBIDDEN_PLAYERS_STATE_FOCUS = (
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bOtherConnection\s*\("),
	re.compile(r"\bPredictDummy\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_aClients\b"),
	re.compile(r"GameClient\(\)->m_Camera\b"),
	re.compile(r"GameClient\(\)->(?:IntersectCharacter|IsOtherTeam)\s*\("),
	re.compile(r"Client\(\)->(?:GameTickSpeed|ServerCapAnyPlayerFlag)\s*\("),
	re.compile(r"(?<!\.)\bCollision\s*\("),
	re.compile(r"\bIVideo::Current\s*\("),
)
FORBIDDEN_NAMEPLATES_STATE_FOCUS = (
	re.compile(r"GameClient\(\)->m_(?:aClients|Snap|Camera)\b"),
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"GameClient\(\)->(?:GameState|IsTeamPlay)\s*\("),
	re.compile(r"Client\(\)->State\s*\("),
	re.compile(r"\bIVideo::Current\s*\("),
	re.compile(r"Graphics\(\)->GetScreen\s*\("),
)
FORBIDDEN_DEBUG_HUD_CONTEXT_FOCUS = (
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_GameWorld\b"),
	re.compile(r"GameClient\(\)->(?:GameState|GetTuning)\s*\("),
	re.compile(r"Client\(\)->(?:GameTick|GameTickSpeed|State)\s*\("),
)
FORBIDDEN_IMPORTANT_ALERT_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"Client\(\)->State\s*\("),
	re.compile(r"\bIVideo::Current\s*\("),
)
FORBIDDEN_HUD_SPECTATOR_COUNT_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->GameState\s*\("),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_HUD_STATUS_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->GameState\s*\("),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_HUD_SCORE_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:FlagDropTick|GameState|IsTeamPlay|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_HUD_INFO_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|IsTeamPlay|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bIVideo::Current\s*\("),
)
FORBIDDEN_HUD_MOVEMENT_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bRecreateTextContainer\s*\("),
	re.compile(r"\bm_aPlayer(?:Position|Speed)"),
)
FORBIDDEN_HUD_SPECTATOR_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|MultiView|m_Camera|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_HUD_ROOT_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|MultiView|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bIVideo::Current\s*\("),
)
FORBIDDEN_HUD_CURSOR_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|LegacyGameView|m_Camera|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_SCOREBOARD_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|SessionContext|FocusedTeams|IsTeamPlay|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bUi\(\)->(?:MapScreen|Screen)\s*\("),
)
FORBIDDEN_SCOREBOARD_VISUAL_SIDE_EFFECT = (
	re.compile(r"\b(?:LockMouse|RenderApplicationOverlay|RenderRecordingNotification|UpdateApplicationOverlay)\s*\("),
	re.compile(r"\bUi\(\)->(?:DoButtonLogic|DoPopupMenu|FinishCheck|RenderPopupMenus|StartCheck|Update)\s*\("),
	re.compile(r"\bm_(?:Highlight|HasInteraction|Interaction(?:Session|State|View))[A-Za-z0-9_]*\s*=(?!=)"),
)
FORBIDDEN_STATBOARD_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|SessionContext|FocusedTeams|IsTeamPlay|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_INFO_MESSAGES_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_(?:aClients|GameWorld)\b"),
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bIVideo::Current\s*\("),
	re.compile(r"Graphics\(\)->GetScreen\s*\("),
)
FORBIDDEN_INFO_MESSAGES_MESSAGE_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_(?:aClients|GameWorld)\b"),
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bClient\(\)"),
)
FORBIDDEN_VOTING_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\b(?:SessionContext|VoteState)\s*\("),
	re.compile(r"\bClient\(\)"),
	re.compile(r"(?<![A-Za-z])time\s*\("),
	re.compile(r"GameClient\(\)->m_Scoreboard\.IsActive\s*\(\s*\)"),
)
FORBIDDEN_VOTING_MESSAGE_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\b(?:SessionContext|VoteState)\s*\("),
	re.compile(r"Client\(\)->(?:GameTick|GameTickSpeed|State)\s*\("),
)
FORBIDDEN_CHAT_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|SessionContext|m_aClients)\b"),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bIVideo::Current\s*\("),
	re.compile(r"(?<![A-Za-z0-9_])(?:time|time_get|time_freq)\s*\("),
	re.compile(r"\b(?:DisableMode|EnableMode|SendChat)\s*\("),
	re.compile(r"\bm_Input\.(?:Activate|Clear|Deactivate|Render)\s*\("),
)
FORBIDDEN_CHAT_MESSAGE_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|SessionContext|m_aClients)\b"),
	re.compile(r"Client\(\)->(?:GameTick|GameTickSpeed|IsDemoPlayback|IsSixup|State)\s*\("),
)
FORBIDDEN_CHAT_APPLICATION_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->(?:GameState|SessionContext|m_aClients)\b"),
	re.compile(r"Client\(\)->(?:GameTick|GameTickSpeed|IsDemoPlayback|IsSixup|State)\s*\("),
)
FORBIDDEN_CHAT_SAVE_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|Focused[A-Za-z0-9_]*|GameState|SessionContext)\s*\("),
	re.compile(r"(?<![A-Za-z0-9_.])Map\s*\("),
)
FORBIDDEN_EMOTICON_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_aClients\b"),
	re.compile(r"\b(?:GameState|LegacyGameView|SessionContext)\s*\("),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bUi\(\)->Screen\s*\("),
)
FORBIDDEN_SPECTATOR_CONTEXT_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_aClients\b"),
	re.compile(r"\b(?:GameState|LegacyGameView|SessionContext)\s*\("),
	re.compile(r"(?<!\.)\bMultiView\s*\("),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bUi\(\)->Screen\s*\("),
)
FORBIDDEN_SPECTATOR_RENDER_MUTATION = (
	re.compile(r"\bInput\(\)"),
	re.compile(r"\bUpdateTouchState\s*\("),
	re.compile(r"\bOnRelease\s*\("),
	re.compile(r"\bm_TouchState\b"),
	re.compile(r"\bm_PendingSpectatorId\s*="),
)
FORBIDDEN_TOUCH_COMMAND_ROUTER_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"GameClient\(\)->m_Camera\b"),
	re.compile(r"GameClient\(\)->GameState\s*\("),
	re.compile(r"(?<!\.)\bCollision\s*\("),
	re.compile(r"Graphics\(\)->ScreenAspect\s*\("),
	re.compile(r"Client\(\)->(?:DummyAllowed|DummyConnected|IsDemoPlayback|RconAuthed|State)\s*\("),
	re.compile(r"\btime(?:_[A-Za-z0-9_]+)?\s*\("),
)
FORBIDDEN_TOUCH_CONTROLLER_FOCUS = FORBIDDEN_TOUCH_COMMAND_ROUTER_FOCUS + (re.compile(r"Console\(\)->ExecuteLineStroked\s*\("),)
FORBIDDEN_TOUCH_RENDER_MUTATION = FORBIDDEN_TOUCH_CONTROLLER_FOCUS + (
	re.compile(r"\b(?:SetActive|SetInactive|UpdateBackgroundCorners|UpdateScreenFromUnitRect|UpdateVisibilityEditor|UpdateVisibilityGame)\s*\("),
	re.compile(r"\bExecuteLine(?:Stroked)?\s*\("),
	re.compile(r"\bInput\s*\("),
)
FORBIDDEN_TOUCH_OWNER = (
	re.compile(r"\bOnTouchState\s*\("),
	re.compile(r"\bOnRender\s*\(\s*\)"),
)
FORBIDDEN_SESSION_STATS_UPDATE_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection|Focused[A-Za-z0-9_]*)\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\b(?:Client|GameClient|SessionContext)\s*\(\)"),
)
FORBIDDEN_GAME_CLIENT_RENDER = (
	re.compile(r"\bClient\s*\(\)"),
	re.compile(r"\bLegacyGameView\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\bMultiView\s*\("),
	re.compile(r"\btime_get\s*\("),
	re.compile(r"\bm_vpAll\b"),
	re.compile(r"\bm_GameViews\.Create\s*\("),
	re.compile(r"\bm_Camera\.BindState\s*\("),
	re.compile(r"\bm_Camera\.Update(?:Camera|Position)\s*\("),
	re.compile(r"\bm_Controls\.Update\s*\("),
	re.compile(r"\bInput\(\)->Clear\s*\("),
	re.compile(r"\bm_New(?:Predicted)?Tick\s*="),
	re.compile(r"\b(?:SendInfo|SendDummyInfo|SendSkinChange7)\s*\("),
	re.compile(r"\bUpdateManagedTeeRenderInfos\s*\("),
	re.compile(r"\b(?:UpdateRenderedClients|InitMultiView|ResetMultiView|UpdatePositions|UpdateSpectatorCursor)\s*\("),
	re.compile(r"\b(?:SetTarget|SetViewport|SetSpectator|SetCameraPosition)\s*\("),
	re.compile(r"\bMapScreenToWorld\s*\("),
	re.compile(r"\bm_vPreparedRenderEntries\.(?:clear|reserve|push_back|emplace_back)\s*\("),
	re.compile(r"\bCommitController\s*\("),
	re.compile(r"\bUpdateController\s*\("),
	re.compile(r"\bPrepareApplicationOverlay\s*\("),
)
FORBIDDEN_CLIENT_UPDATE_FOCUS_CONFIG = (
	re.compile(r"\bSetActiveConnection\s*\(\s*g_Config\.m_ClDummy\b"),
	re.compile(r"\bUpdate(?:Demo|Network)Session\s*\("),
	re.compile(r"\bDisconnect(?:Demo)?WithReason\s*\("),
	re.compile(r"\bPumpNetwork\s*\("),
	re.compile(r"\bm_pMapdownloadTask\b"),
)
FORBIDDEN_SESSION_CLOSE_ADAPTER_WORK = (
	re.compile(r"\b(?:DemoPlayer|GameClient|NetClient)\s*\("),
	re.compile(r"\b(?:ResetMetadata|ResetSnapshots|OnSessionClosed)\s*\("),
)
FORBIDDEN_SESSION_MESSAGE_ROUTING_FOCUS = (re.compile(r"Client\(\)->ActiveConnection\s*\("),)
FORBIDDEN_DEMO_SEEK_FOCUS = (
	re.compile(r"GameClient\(\)->(?:GameState|SessionContext)\s*\("),
	re.compile(r"\bActiveConnection\s*\("),
)
FORBIDDEN_RENDER_PROJECTION_TIME = (re.compile(r"\btime_get\s*\("),)
FORBIDDEN_ENGINE_TIMING_QUERY_FOCUS = (re.compile(r"\bActiveConnection\s*\("),)
FORBIDDEN_ENGINE_TIMING_OWNER = (re.compile(r"\bCSmoothTime\s+m_PredictedTime\b"),)
FORBIDDEN_PARAMETERLESS_CONNECTION_TIMING = (
	re.compile(r"\bGetPredictionTick\s*\(\s*\)"),
	re.compile(r"\bConnectionProblems\s*\(\s*\)"),
)
FORBIDDEN_MAP_SOUNDS_UPDATE_FOCUS = (
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\bm_Camera\b"),
	re.compile(r"\bm_MapLayersBackground\b"),
	re.compile(r"\bIsDemoPlaybackPaused\s*\("),
)
FORBIDDEN_MAP_SOUNDS_LOAD_FOCUS = (
	re.compile(r"\bGameClient\(\)->Map\s*\("),
	re.compile(r"\bLayers\s*\("),
)
FORBIDDEN_SCENE_UPDATE_FOCUS = (
	re.compile(r"\b(?:time|time_get|time_freq|LocalTime)\s*\("),
	re.compile(r"\bGetAnimationPlaybackSpeed\s*\("),
	re.compile(r"\bClient\(\)->State\s*\("),
)
FORBIDDEN_MAP_LAYER_CONTEXT_RENDER_FOCUS = (
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bGameClient\(\)"),
	re.compile(r"\bGetCurCamera\s*\("),
)
FORBIDDEN_WORLD_REQUEST_BOUNDS = (re.compile(r"Graphics\(\)->GetScreen\s*\("),)
FORBIDDEN_MAP_RENDER_IMAGE_FOCUS = (
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bFocusedGameInfo\s*\("),
	re.compile(r"\bGameClient\(\)"),
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bClient\(\)->IsSixup\s*\("),
)
FORBIDDEN_MAP_LAYER_BINDING_FOCUS = (
	re.compile(r"\bLayers\s*\("),
	re.compile(r"\bm_MapRenderImages\b"),
)
FORBIDDEN_SESSION_PRESENTATION_FOCUS = (
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bClient\s*\("),
	re.compile(r"\bFocusedGameInfo\s*\("),
	re.compile(r"\bGameClient\s*\("),
	re.compile(r"\bSessionContext\s*\("),
)
FORBIDDEN_SESSION_CLIENT_PRESENTATION_FOCUS = (
	re.compile(r"\b(?:ActiveConnection|OtherConnection)\s*\("),
	re.compile(r"\bFocused[A-Za-z0-9_]*\s*\("),
	re.compile(r"GameClient\(\)->m_(?:Snap|aClients)\b"),
	re.compile(r"GameClient\(\)->Client\s*\("),
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bm_View\b"),
)
FORBIDDEN_CROSS_SESSION_RESET = (re.compile(r"\bm_SessionContexts\.Contexts\s*\("),)
FORBIDDEN_SNAPSHOT_SOURCE_FOCUS = (
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bGameState\s*\("),
	re.compile(r"Client\(\)->ActiveConnection\s*\("),
	re.compile(r"Client\(\)->(?:IsDemoPlayback|IsOnline|IsSixup)\s*\(\s*\)"),
)
FORBIDDEN_SESSION_SNAPSHOT_AMBIENT = (
	re.compile(r"\bSnap(?:FindItem|GetItem|NumItems)\s*\(\s*Conn\b"),
	re.compile(r"\b(?:GameTick|PrevGameTick)\s*\(\s*Conn\b"),
	re.compile(r"\bServerInfo\s*\(\s*\)"),
)
FORBIDDEN_DEMO_CONNECTION_ALIAS = (
	re.compile(r"\bConnection\s*\(\s*CONN_MAIN\s*\)"),
	re.compile(r"\bIsSixup\s*\(\s*\)"),
	re.compile(r"\bm_pNetworkSessionSource\b"),
	re.compile(r"\bm_aa?DemorecSnapshot"),
)
FORBIDDEN_SESSION_LIFECYCLE_AMBIENT = (
	re.compile(r"GameClient\(\)->Map\s*\(\s*\)"),
	re.compile(r"\b(?:GameState|SessionContext)\s*\(\s*\)"),
	re.compile(r"\bIsSixup\s*\(\s*\)"),
	re.compile(r"\bConnection\s*\(\s*CONN_MAIN\s*\)"),
)
FORBIDDEN_SIXUP_SNAPSHOT_AMBIENT = (
	re.compile(r"\bGameState\s*\("),
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"\bm_TranslationContext\b"),
)
FORBIDDEN_SESSION_MESSAGE_TIME_FOCUS = (
	re.compile(r"\bFocusedSessionId\s*\("),
	re.compile(r"\b(?:IsDemoPlayback|State)\s*\("),
)
FORBIDDEN_NETWORK_DUMMY_AMBIENT = (
	re.compile(r"\bGameState\s*\("),
	re.compile(r"\bOtherConnection\s*\("),
	re.compile(r"\bSessionContext\s*\("),
	re.compile(r"Client\(\)->IsSixup\s*\(\s*\)"),
)
FORBIDDEN_GAME_CLIENT = (
	re.compile(r"\bCStreamStorage\b"),
	re.compile(r"\bCFlow\b"),
	re.compile(r"\bm_Flow(?:Affected)?\b"),
	re.compile(r"\bm_aSixup\b"),
	re.compile(r"\bSelect(?:ed)?Sixup\b"),
	re.compile(r"\b(?:m_DummyInput|m_DummyFire|m_IsDummySwapping)\b"),
	re.compile(r"\bOtherConnection\b"),
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
FORBIDDEN_SCENE_RENDER = (
	re.compile(r"GameState\(\s*GameClient\(\)->ActiveConnection\(\)\s*\)"),
	re.compile(r"\bvoid OnRender\(\) override"),
	re.compile(r"Graphics\(\)->GetScreen\s*\("),
	re.compile(r"\bClient\(\)->State\s*\("),
)
FORBIDDEN_EFFECT_OWNER = (re.compile(r"\b(?:m_Add5hz|m_LastUpdate5hz|m_Add50hz|m_LastUpdate50hz|m_Add100hz|m_LastUpdate100hz|m_SkidSoundTimer)\b"),)
FORBIDDEN_GHOST_OWNER = (re.compile(r"\bm_PlaybackPos\b"),)
FORBIDDEN_PREDICTION_ENTITY = (re.compile(r"\bm_LastRenderTick\b"),)
FORBIDDEN_PARTICLE_OWNER = (re.compile(r"\b(?:m_vParticles|m_FirstFree|m_aFirstPart|m_NumParticles|m_FrictionFraction|m_LastRenderTime|m_TimeInitialized|OnReset)\b"),)
FORBIDDEN_RENDER_TIMING = (re.compile(r"\b(?:s_LastGameTickTime|s_LastPredIntraTick|s_LastLocalTime|s_LastIteX|s_LastRaceTick|s_Time)\b"),)
FORBIDDEN_GAME_CLIENT_OWNER = (
	re.compile(r"\b(?:CClientStats|m_aLastUpdateTick|m_aStats|m_CharOrder|m_GameInfo|m_ServerMode|m_Teams)\b"),
	re.compile(r"\b(?:m_RenderCur|m_RenderPrev|m_RenderPos|m_IsPredicted|m_IsPredictedLocal|m_aSmoothStart|m_aSmoothLen|m_aPredPos|m_aPredTick)\b"),
	re.compile(r"\b(?:m_MapBestTimeSeconds|m_MapBestTimeMillis|m_aMapDescription)\b"),
	re.compile(r"\b(?:SMultiView|m_MultiView|m_MultiViewTeam|m_MultiViewPersonalZoom|m_MultiViewShowHud|m_MultiViewActivated|m_aMultiViewId)\b"),
	re.compile(r"\b(?:m_PredictedTick|m_LastRoundStartTick|m_LastRaceTick|m_LastFlagCarrierRed|m_LastFlagCarrierBlue|m_aLastPos|m_aLastActive|m_GameOver|m_GamePaused|m_aFlagDropTick|m_ReceivedDDNetPlayer|m_ReceivedDDNetPlayerFinishTimes|m_ReceivedDDNetPlayerFinishTimesMillis)\b"),
	re.compile(r"\b(?:m_Jetpack|m_CollisionDisabled|m_EndlessHook|m_EndlessJump|m_HammerHitDisabled|m_GrenadeHitDisabled|m_LaserHitDisabled|m_ShotgunHitDisabled|m_HookHitDisabled|m_Super|m_Invincible|m_HasTelegunGun|m_HasTelegunGrenade|m_HasTelegunLaser|m_FreezeEnd|m_DeepFrozen|m_LiveFrozen)\b"),
	re.compile(r"\b(?:CCursorInfo|m_CursorOwnerId|m_aTargetSamplesTime|m_aTargetSamplesData|m_NumSamples)\b"),
	re.compile(r"\b(?:m_AuthLevel|m_Afk|m_Paused|m_Spec|m_FinishTimeSeconds|m_FinishTimeMillis|m_SpecCharPresent|m_SpecChar)\b"),
	re.compile(r"\b(?:m_ChatIgnore|m_EmoticonStartFraction|m_EmoticonStartTick|m_EmoticonIgnore)\b"),
)
FORBIDDEN_SOURCE_CALLBACK_OWNER = (
	re.compile(r"\bbool\s+Dummy\b"),
	re.compile(r"\bOnDummySwap\b"),
	re.compile(r"\b(?:OnNewSnapshot|OnMessage|OnPredict|OnSnapInput|OnConnectionFocusChanged|TranslateSnap|ApplySkin7InfoFromSnapObj|OnDemoRecSnap7)\s*\([^;{}]*\bint\s+(?:Previous)?Conn\b"),
)
FORBIDDEN_STABLE_STREAM_LOOKUP = (
	re.compile(r"\bCStreamId(?:\s+[A-Za-z_]\w*)?\s*\(\s*(?:Conn|IClient::CONN_(?:MAIN|DUMMY))\s*\+\s*1\s*\)"),
	re.compile(r"\bStreamId\s*\(\s*\)\s*\.\s*Value\s*\(\s*\)\s*-\s*1\b"),
)
FORBIDDEN_PROCESS_SERVER_PACKET_AMBIENT = (
	re.compile(r"\bConnection\s*\(\s*Conn\s*\)"),
	re.compile(r"\bm_pNetworkSessionSource\b"),
	re.compile(r"\bDisconnectWithReason\s*\("),
	re.compile(r"\b(?:SendMsg|SendReady|SendEnterGame)\s*\(\s*(?:CONN_|IClient::CONN_)"),
	re.compile(r"(?<!Source\.)(?<!TranslationContext\.)\b(?:m_MapDetails|m_pMapdownloadTask|m_aMapdownloadFilename(?:Temp)?|m_aMapdownloadName|m_MapdownloadFileTemp|m_MapdownloadChunk|m_MapdownloadCrc|m_MapdownloadAmount|m_MapdownloadTotalsize|m_MapdownloadSha256)\b"),
	re.compile(r"(?<!Source\.)\bm_(?:UseTempRconCommands|ExpectedRconCommands|GotRconCommands|ExpectedMaplistEntries|vMaplistEntries)\b"),
)
FORBIDDEN_FOCUSED_CLIENT_SOURCE_API = (
	re.compile(r"\b(?:PrevGameTick|GameTick|PredGameTick|IntraGameTick|PredIntraGameTick|IntraGameTickSincePrev|GameTickTime|GetPredictionTime|GetPredictionTick|SnapNumItems|SnapFindItem|SnapGetItem|ConnectionProblems|GetSmoothTick)\s*\(\s*int\s+Conn\b"),
	re.compile(r"\b(?:ServerInfo|IsSixup)\s*\(\s*\)\s*const"),
)
FORBIDDEN_HUD_OWNER = (re.compile(r"\b(?:m_TimeCpDiff|m_FinishTimeDiff|m_DDRaceTime|m_FinishTimeLastReceivedTick|m_TimeCpLastReceivedTick|m_ShowFinishTime)\b"),)
FORBIDDEN_INFO_MESSAGES_OWNER = (
	re.compile(r"\bm_aInfoMsgs\b"),
	re.compile(r"\bm_InfoMsgCurrent\b"),
	re.compile(r"\bOnMessage\s*\("),
)
FORBIDDEN_CHAT_OWNER = (
	re.compile(r"\bOnMessage\s*\("),
	re.compile(r"\bOnRender\s*\(\s*\)"),
	re.compile(r"\b(?:m_CurrentLine|m_vServerCommands)\b"),
)
FORBIDDEN_EMOTICON_OWNER = (re.compile(r"\b(?:m_WasActive|m_Active|m_SelectorMouse|m_SelectedEmote|m_SelectedEyeEmote|m_TouchPressedOutside)\b"),)
FORBIDDEN_MOTD_OWNER = (re.compile(r"\b(?:m_aServerMotd|m_ServerMotdTime|m_ServerMotdUpdateTime)\b"),)
FORBIDDEN_SPECTATOR_OWNER = (re.compile(r"\b(?:m_Active|m_WasActive|m_SelectedSpectatorId|m_SelectorMouse|m_MultiViewActivateDelay|m_MultiViewActivateTime|m_PendingSpectatorId)\b"),)
FORBIDDEN_SOUNDS_OWNER = (re.compile(r"\bOnRender\s*\("),)
FORBIDDEN_MAP_SOUNDS_OWNER = (
	re.compile(r"\bOnMapLoad\s*\("),
	re.compile(r"\bOnRender\s*\("),
	re.compile(r"\bOnStateChange\s*\("),
)
FORBIDDEN_MAP_LAYER_OWNER = (re.compile(r"\bvoid OnRender\(\) override"),)
FORBIDDEN_MAP_RENDER_IMAGE_OWNER = (re.compile(r"\bOnMapLoad\s*\("),)
FORBIDDEN_ENVELOPE_STATE_FOCUS = (
	re.compile(r"\bGameClient\(\)"),
	re.compile(r"\bClient\(\)"),
	re.compile(r"\bActiveConnection\s*\("),
	re.compile(r"\bm_Snap\b"),
	re.compile(r"\bg_Config\b"),
	re.compile(r"\bstatic\b[^;\n]*\bNanosPerTick\b"),
)
FORBIDDEN_VOTING_OWNER = (re.compile(r"\b(?:CHeap|CVoteOptionClient|m_Opentime|m_Closetime|m_aDescription|m_aReason|m_Voted|m_Yes|m_No|m_Pass|m_Total|m_ReceivingOptions|m_NumVoteOptions|m_pFirst|m_pLast|m_pRecycleFirst|m_pRecycleLast)\b"),)
FORBIDDEN_SIXUP_VOTE = (re.compile(r"m_Voting\.(?:AddOption|OnReset)\("),)
FORBIDDEN_SIXUP_CHAT = (
	re.compile(r"m_Chat\.(?:AddLine|HandleMessage)\("),
	re.compile(r"Conn\s*!=\s*ActiveConnection\s*\("),
	re.compile(r"m_aClients\[[^\]]+\]\.m_aName"),
	re.compile(r"SessionContext\s*\(\)\.Stats\s*\("),
	re.compile(r"m_TranslationContext\.m_GameFlags\s*&"),
)


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
		# A pattern that matches nothing would silently stop enforcing its rules,
		# which is what renaming one of the functions below would do.
		if function_pattern.search(text) is None:
			errors.append(f"{relative}: no function matches {function_pattern.pattern}, so its architecture rules are not checked")
			continue
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


def check_ordered_calls(relative: str, function_pattern: re.Pattern[str], calls: tuple[str, ...]) -> list[str]:
	text = (ROOT / relative).read_text(encoding="utf-8")
	function_match = function_pattern.search(text)
	if function_match is None:
		return [f"{relative}: missing function for render-hook order check"]
	body_start = text.find("{", function_match.end())
	depth = 1
	body_end = body_start + 1
	while body_end < len(text) and depth > 0:
		if text[body_end] == "{":
			depth += 1
		elif text[body_end] == "}":
			depth -= 1
		body_end += 1
	body = text[body_start:body_end]
	positions = []
	for call in calls:
		matches = list(re.finditer(rf"(?<![A-Za-z0-9_]){re.escape(call)}", body))
		if len(matches) != 1:
			return [f"{relative}: expected exactly one `{call}` in render-hook boundary"]
		positions.append(matches[0].start())
	if positions != sorted(positions):
		return [f"{relative}: render hooks are not ordered as {' -> '.join(calls)}"]
	return []


errors = (
	check(CORE_FILES, FORBIDDEN_CORE)
	+ check(RENDER_FILES, FORBIDDEN_RENDER)
	+ check_function_bodies(CONTEXT_RENDER_FUNCTIONS, FORBIDDEN_CONTEXT_RENDER)
	+ check_function_bodies(RENDER_PURITY_FUNCTIONS, FORBIDDEN_RENDER_MUTATION)
	+ check_function_bodies(PRESENTATION_UPDATE_FUNCTIONS, FORBIDDEN_PRESENTATION_VIEW)
	+ check_function_bodies(PRESENTATION_UPDATE_FUNCTIONS, FORBIDDEN_PRESENTATION_EFFECT_ALPHA)
	+ check_function_bodies(OVERLAY_CONTEXT_RENDER_FUNCTIONS, FORBIDDEN_OVERLAY_RENDER_FOCUS)
	+ check_function_bodies(OVERLAY_VISUAL_FUNCTIONS, FORBIDDEN_OVERLAY_VISUAL_SIDE_EFFECT)
	+ check_function_bodies(ITEMS_PRESENTATION_FUNCTIONS, FORBIDDEN_ITEMS_PRESENTATION_FOCUS)
	+ check_function_bodies(ITEMS_RENDER_FUNCTIONS, FORBIDDEN_ITEMS_RENDER_FOCUS)
	+ check_function_bodies(PLAYERS_STATE_FUNCTIONS, FORBIDDEN_PLAYERS_STATE_FOCUS)
	+ check_function_bodies(NAMEPLATES_STATE_FUNCTIONS, FORBIDDEN_NAMEPLATES_STATE_FOCUS)
	+ check_function_bodies(DEBUG_HUD_CONTEXT_FUNCTIONS, FORBIDDEN_DEBUG_HUD_CONTEXT_FOCUS)
	+ check_function_bodies(IMPORTANT_ALERT_CONTEXT_FUNCTIONS, FORBIDDEN_IMPORTANT_ALERT_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_SPECTATOR_COUNT_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_SPECTATOR_COUNT_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_STATUS_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_STATUS_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_SCORE_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_SCORE_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_INFO_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_INFO_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_MOVEMENT_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_MOVEMENT_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_SPECTATOR_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_SPECTATOR_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_ROOT_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_ROOT_CONTEXT_FOCUS)
	+ check_function_bodies(HUD_CURSOR_CONTEXT_FUNCTIONS, FORBIDDEN_HUD_CURSOR_CONTEXT_FOCUS)
	+ check_function_bodies(SCOREBOARD_CONTEXT_FUNCTIONS, FORBIDDEN_SCOREBOARD_CONTEXT_FOCUS)
	+ check_function_bodies(SCOREBOARD_VISUAL_FUNCTIONS, FORBIDDEN_SCOREBOARD_VISUAL_SIDE_EFFECT)
	+ check_function_bodies(STATBOARD_CONTEXT_FUNCTIONS, FORBIDDEN_STATBOARD_CONTEXT_FOCUS)
	+ check_function_bodies(INFO_MESSAGES_CONTEXT_FUNCTIONS, FORBIDDEN_INFO_MESSAGES_CONTEXT_FOCUS)
	+ check_function_bodies(INFO_MESSAGES_MESSAGE_FUNCTIONS, FORBIDDEN_INFO_MESSAGES_MESSAGE_FOCUS)
	+ check_function_bodies(VOTING_CONTEXT_FUNCTIONS, FORBIDDEN_VOTING_CONTEXT_FOCUS)
	+ check_function_bodies(VOTING_MESSAGE_FUNCTIONS, FORBIDDEN_VOTING_MESSAGE_FOCUS)
	+ check_function_bodies(CHAT_CONTEXT_FUNCTIONS, FORBIDDEN_CHAT_CONTEXT_FOCUS)
	+ check_function_bodies(CHAT_MESSAGE_FUNCTIONS, FORBIDDEN_CHAT_MESSAGE_FOCUS)
	+ check_function_bodies(CHAT_APPLICATION_FUNCTIONS, FORBIDDEN_CHAT_APPLICATION_FOCUS)
	+ check_function_bodies(CHAT_SAVE_FUNCTIONS, FORBIDDEN_CHAT_SAVE_FOCUS)
	+ check_function_bodies(EMOTICON_CONTEXT_FUNCTIONS, FORBIDDEN_EMOTICON_CONTEXT_FOCUS)
	+ check_function_bodies(SPECTATOR_CONTEXT_FUNCTIONS, FORBIDDEN_SPECTATOR_CONTEXT_FOCUS)
	+ check_function_bodies(SPECTATOR_RENDER_FUNCTIONS, FORBIDDEN_SPECTATOR_RENDER_MUTATION)
	+ check_function_bodies(TOUCH_CONTROLLER_FUNCTIONS, FORBIDDEN_TOUCH_CONTROLLER_FOCUS)
	+ check_function_bodies(TOUCH_COMMAND_ROUTER_FUNCTIONS, FORBIDDEN_TOUCH_COMMAND_ROUTER_FOCUS)
	+ check_function_bodies(TOUCH_RENDER_FUNCTIONS, FORBIDDEN_TOUCH_RENDER_MUTATION)
	+ check_function_bodies(SESSION_STATS_UPDATE_FUNCTIONS, FORBIDDEN_SESSION_STATS_UPDATE_FOCUS)
	+ check_function_bodies(GAME_CLIENT_RENDER_FUNCTIONS, FORBIDDEN_GAME_CLIENT_RENDER)
	+ check_function_bodies(CLIENT_UPDATE_FUNCTIONS, FORBIDDEN_CLIENT_UPDATE_FOCUS_CONFIG)
	+ check_function_bodies(SESSION_CLOSE_ADAPTER_FUNCTIONS, FORBIDDEN_SESSION_CLOSE_ADAPTER_WORK)
	+ check_function_bodies(SESSION_MESSAGE_ROUTING_FUNCTIONS, FORBIDDEN_SESSION_MESSAGE_ROUTING_FOCUS)
	+ check_function_bodies(DEMO_SEEK_FUNCTIONS, FORBIDDEN_DEMO_SEEK_FOCUS)
	+ check_function_bodies(RENDER_PROJECTION_FUNCTIONS, FORBIDDEN_RENDER_PROJECTION_TIME)
	+ check_function_bodies(ENGINE_TIMING_QUERY_FUNCTIONS, FORBIDDEN_ENGINE_TIMING_QUERY_FOCUS)
	+ check_function_bodies(PROCESS_SERVER_PACKET_FUNCTIONS, FORBIDDEN_PROCESS_SERVER_PACKET_AMBIENT)
	+ check_function_bodies(MAP_SOUNDS_UPDATE_FUNCTIONS, FORBIDDEN_MAP_SOUNDS_UPDATE_FOCUS)
	+ check_function_bodies(MAP_SOUNDS_LOAD_FUNCTIONS, FORBIDDEN_MAP_SOUNDS_LOAD_FOCUS)
	+ check_function_bodies(SCENE_UPDATE_FUNCTIONS, FORBIDDEN_SCENE_UPDATE_FOCUS)
	+ check_function_bodies(MAP_LAYER_CONTEXT_RENDER_FUNCTIONS, FORBIDDEN_MAP_LAYER_CONTEXT_RENDER_FOCUS)
	+ check_function_bodies(WORLD_REQUEST_BOUNDS_FUNCTIONS, FORBIDDEN_WORLD_REQUEST_BOUNDS)
	+ check_function_bodies(MAP_RENDER_IMAGE_FUNCTIONS, FORBIDDEN_MAP_RENDER_IMAGE_FOCUS)
	+ check_function_bodies(MAP_LAYER_BINDING_FUNCTIONS, FORBIDDEN_MAP_LAYER_BINDING_FOCUS)
	+ check_function_bodies(SESSION_PRESENTATION_FUNCTIONS, FORBIDDEN_SESSION_PRESENTATION_FOCUS)
	+ check_function_bodies(SESSION_CLIENT_PRESENTATION_FUNCTIONS, FORBIDDEN_SESSION_CLIENT_PRESENTATION_FOCUS)
	+ check_function_bodies(SESSION_RESET_FUNCTIONS, FORBIDDEN_CROSS_SESSION_RESET)
	+ check_function_bodies(SNAPSHOT_SOURCE_FUNCTIONS, FORBIDDEN_SNAPSHOT_SOURCE_FOCUS)
	+ check_function_bodies(SESSION_SNAPSHOT_FUNCTIONS, FORBIDDEN_SESSION_SNAPSHOT_AMBIENT)
	+ check_function_bodies(DEMO_CONNECTION_FUNCTIONS, FORBIDDEN_DEMO_CONNECTION_ALIAS)
	+ check_function_bodies(SESSION_LIFECYCLE_FUNCTIONS, FORBIDDEN_SESSION_LIFECYCLE_AMBIENT)
	+ check_function_bodies(SIXUP_SNAPSHOT_FUNCTIONS, FORBIDDEN_SIXUP_SNAPSHOT_AMBIENT)
	+ check_function_bodies(SESSION_MESSAGE_TIME_FUNCTIONS, FORBIDDEN_SESSION_MESSAGE_TIME_FOCUS)
	+ check_function_bodies(NETWORK_DUMMY_FUNCTIONS, FORBIDDEN_NETWORK_DUMMY_AMBIENT)
	+ check_ordered_calls("src/engine/client/client.cpp", re.compile(r"\bvoid CClient::Run\s*\("), ("GameClient()->OnRenderPrepare();", "Render();", "GameClient()->OnRenderFinalize();"))
	+ check_ordered_calls("src/engine/client/client.cpp", re.compile(r"\bvoid CClient::Render\s*\("), ("GameClient()->OnRender();",))
	+ check(CAMERA_FILES, FORBIDDEN_CAMERA)
	+ check(CONTROLS_OWNER_FILES, FORBIDDEN_CONTROLS_OWNER)
	+ check(BROADCAST_OWNER_FILES, FORBIDDEN_BROADCAST_OWNER)
	+ check(DAMAGE_INDICATOR_FILES, FORBIDDEN_DAMAGE_INDICATOR)
	+ check(SCENE_RENDER_FILES, FORBIDDEN_SCENE_RENDER)
	+ check(EFFECT_OWNER_FILES, FORBIDDEN_EFFECT_OWNER)
	+ check(GHOST_OWNER_FILES, FORBIDDEN_GHOST_OWNER)
	+ check(PREDICTION_ENTITY_FILES, FORBIDDEN_PREDICTION_ENTITY)
	+ check(PARTICLE_OWNER_FILES, FORBIDDEN_PARTICLE_OWNER)
	+ check(RENDER_TIMING_FILES, FORBIDDEN_RENDER_TIMING)
	+ check(GAME_CLIENT_OWNER_FILES, FORBIDDEN_GAME_CLIENT_OWNER)
	+ check(SOURCE_CALLBACK_OWNER_FILES, FORBIDDEN_SOURCE_CALLBACK_OWNER)
	+ check(STABLE_STREAM_LOOKUP_FILES, FORBIDDEN_STABLE_STREAM_LOOKUP)
	+ check(I_CLIENT_SOURCE_API_FILES, FORBIDDEN_FOCUSED_CLIENT_SOURCE_API)
	+ check(ENGINE_TIMING_OWNER_FILES, FORBIDDEN_ENGINE_TIMING_OWNER)
	+ check(EXPLICIT_CONNECTION_TIMING_FILES, FORBIDDEN_PARAMETERLESS_CONNECTION_TIMING)
	+ check(HUD_OWNER_FILES, FORBIDDEN_HUD_OWNER)
	+ check(INFO_MESSAGES_OWNER_FILES, FORBIDDEN_INFO_MESSAGES_OWNER)
	+ check(CHAT_OWNER_FILES, FORBIDDEN_CHAT_OWNER)
	+ check(EMOTICON_OWNER_FILES, FORBIDDEN_EMOTICON_OWNER)
	+ check(MOTD_OWNER_FILES, FORBIDDEN_MOTD_OWNER)
	+ check(SPECTATOR_OWNER_FILES, FORBIDDEN_SPECTATOR_OWNER)
	+ check(TOUCH_OWNER_FILES, FORBIDDEN_TOUCH_OWNER)
	+ check(SOUNDS_OWNER_FILES, FORBIDDEN_SOUNDS_OWNER)
	+ check(MAP_SOUNDS_OWNER_FILES, FORBIDDEN_MAP_SOUNDS_OWNER)
	+ check(MAP_LAYER_OWNER_FILES, FORBIDDEN_MAP_LAYER_OWNER)
	+ check(MAP_RENDER_IMAGE_OWNER_FILES, FORBIDDEN_MAP_RENDER_IMAGE_OWNER)
	+ check(ENVELOPE_STATE_FILES, FORBIDDEN_ENVELOPE_STATE_FOCUS)
	+ check(VOTING_OWNER_FILES, FORBIDDEN_VOTING_OWNER)
	+ check(SIXUP_VOTE_FILES, FORBIDDEN_SIXUP_VOTE)
	+ check(SIXUP_CHAT_FILES, FORBIDDEN_SIXUP_CHAT)
	+ check(GAME_CLIENT_FILES, FORBIDDEN_GAME_CLIENT)
)
if errors:
	print("\n".join(errors), file=sys.stderr)
	sys.exit(1)
print("client multi-state architecture boundaries: OK")
