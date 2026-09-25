/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_player_client.h"

#if defined(CONF_WEB_PLATFORM)
#include "web/window_web.h"
#else
#include "viewer_fullscreen.h"
#include "window_sdl.h"
#endif

#include <base/fs.h>
#include <base/log.h>
#include <base/math.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/protocol.h>
#include <engine/sound.h>
#include <engine/storage.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

namespace
{
	// How far one press of a seek key moves, as in a video player.
	constexpr float SEEK_SECONDS = 5.0f;
	constexpr float JUMP_SECONDS = 10.0f;
	// One step of the client's zoom in, `CCamera::ZOOM_STEP`.
	constexpr float ZOOM_IN_FACTOR = 0.866025f;
#if defined(CONF_VIDEORECORDER)
	// How often the window is drawn while an export runs as fast as it can.
	constexpr std::chrono::nanoseconds EXPORT_SCREEN_INTERVAL = std::chrono::nanoseconds(std::chrono::seconds(1)) / 30;
#endif

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// The one player of the page, for the controls beside the canvas.
	CDemoPlayerClient *gs_pDemoPlayer = nullptr;
#endif

	void PrintUsage()
	{
		log_info("client", "Usage: ddnet-demo-player [<demo>] [--no-controls] [--no-zoom] [--no-overlays]");
		log_info("client", "                         [--time <seconds>] [--speed <factor>] [--paused] [console commands]");
		log_info("client", "The player draws its own bar over the demo, which --no-controls leaves off");
		log_info("client", "for whoever brings their own. The wheel, the zoom keys and a pinch zoom,");
		log_info("client", "which --no-zoom leaves out. Tab and = show the scoreboard and the");
		log_info("client", "statboard, which --no-overlays leaves out. --time, --speed and --paused");
		log_info("client", "say where in the demo to start and how, which is what a link to a place");
		log_info("client", "in a demo means.");
		log_info("client", "Space or K pauses, the left and right arrows seek by 5 seconds and J and L");
		log_info("client", "by 10, the up and down arrows change the speed, Home starts over, M mutes,");
		log_info("client", "F fills the screen, V switches to the free view and N and P step through");
		log_info("client", "the players. A demo dropped on the window replaces the one that plays, and");
		log_info("client", "is what the player waits for when it was given none.");
	}
} // namespace

CDemoPlayerClient::CDemoPlayerClient()
{
	m_RenderOptions.m_ShowDirection = 0;
	m_RenderOptions.m_HighDetail = true;
#if defined(CONF_VIDEORECORDER)
	// The session an export reads the demo in. Made here, because a client only
	// has the sessions it started with.
	auto pExportSource = std::make_unique<CDemoSessionSource>(true, [this]() { UpdateDemoIntraTimers(m_ExportSessionId); });
	CDemoSessionSource *pSource = pExportSource.get();
	m_ExportSessionId = m_SessionManager.Create(std::move(pExportSource));
	m_ExportDemoListener = CDemoListener(this, m_ExportSessionId);
	pSource->m_DemoPlayer.SetListener(&m_ExportDemoListener);
	m_VideoSessionId = m_DemoSessionId;
#endif
}

std::optional<int> CDemoPlayerClient::ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments)
{
	// The demo is the first argument that names one, everything else goes to
	// the console.
	vArguments.push_back(ppArguments[0]);
	for(int Index = 1; Index < ArgumentCount; Index++)
	{
		const char *pArgument = ppArguments[Index];
		if(str_comp(pArgument, "--help") == 0 || str_comp(pArgument, "-h") == 0)
		{
			PrintUsage();
			return 0;
		}
		else if(str_comp(pArgument, "--no-controls") == 0)
			SetShowControls(false);
		else if(str_comp(pArgument, "--no-zoom") == 0)
			SetZoomEnabled(false);
		else if(str_comp(pArgument, "--no-overlays") == 0)
			SetOverlaysEnabled(false);
		else if(str_comp(pArgument, "--paused") == 0)
			SetStartPaused(true);
		else if(str_comp(pArgument, "--time") == 0 || str_comp(pArgument, "--speed") == 0)
		{
			if(Index + 1 >= ArgumentCount)
			{
				log_error("client", "Missing value for %s.", pArgument);
				return -1;
			}
			const float Value = str_tofloat(ppArguments[++Index]);
			if(str_comp(pArgument, "--time") == 0)
				SetStartTime(std::max(Value, 0.0f));
			else
				SetStartSpeed(Value);
		}
		else if(m_aDemoPath[0] == '\0' && str_endswith(pArgument, ".demo"))
			str_copy(m_aDemoPath, pArgument);
		else
			vArguments.push_back(pArgument);
	}
	ArgumentCount = static_cast<int>(vArguments.size());
	ppArguments = vArguments.data();
	return std::nullopt;
}

void CDemoPlayerClient::UpdateAndSwap()
{
	Graphics()->Swap();
}

void CDemoPlayerClient::SetSize(int Width, int Height)
{
	if(Window() == nullptr)
		return;
	// Sizes the canvas, overriding the page's stylesheet.
	Window()->Resize(std::max(Width, 1), std::max(Height, 1), g_Config.m_GfxScreenRefreshRate);
}

void CDemoPlayerClient::FromPage(std::function<void()> &&Action)
{
	if(!web_unwound())
	{
		Action();
		return;
	}
	m_vPageActions.push_back(std::move(Action));
}

void CDemoPlayerClient::RunPageActions()
{
	if(m_vPageActions.empty())
		return;
	std::vector<std::function<void()>> vActions;
	std::swap(vActions, m_vPageActions);
	for(const std::function<void()> &Action : vActions)
		Action();
}

void CDemoPlayerClient::SetPaused(bool Paused)
{
	if(Paused)
		DemoPlayer().Pause();
	else
		DemoPlayer().Unpause();
}

void CDemoPlayerClient::Play()
{
	if(AtEnd())
		SeekStart();
	SetPaused(false);
}

void CDemoPlayerClient::TogglePause()
{
	if(Paused())
		Play();
	else
		SetPaused(true);
}

bool CDemoPlayerClient::AtEnd() const
{
	const float Length = this->Length();
	if(Length <= 0.0f)
		return false;
	// Two ticks of slack, because seeking to a tick may stop just short of it.
	const float End = HasClip() ? m_ClipEnd : Length;
	return Progress() * Length >= End - 2.0f / (float)SERVER_TICK_SPEED;
}

void CDemoPlayerClient::SeekPercent(float Percent)
{
	DemoPlayer().SeekPercent(std::clamp(Percent, 0.0f, 1.0f));
}

void CDemoPlayerClient::SeekToTime(float Seconds)
{
	// From the beginning of the demo, which is what a link means;
	// `CDemoPlayer::SeekTime` moves relative to where it stands.
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	const float Now = (pInfo->m_CurrentTick - pInfo->m_FirstTick) / (float)SERVER_TICK_SPEED;
	DemoPlayer().SeekTime(Seconds - Now);
}

void CDemoPlayerClient::SeekStart()
{
	if(HasClip())
		SeekToTime(m_ClipStart);
	else
		DemoPlayer().SeekPercent(0.0f);
}

void CDemoPlayerClient::SetSpeed(float Speed)
{
	DemoPlayer().SetSpeed(std::clamp(Speed, 0.01f, 100.0f));
}

bool CDemoPlayerClient::Paused() const
{
	return DemoPlayer().BaseInfo()->m_Paused;
}

float CDemoPlayerClient::Progress() const
{
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
	if(TotalTicks == 0)
		return 0.0f;
	return std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks) / (float)TotalTicks;
}

float CDemoPlayerClient::Speed() const
{
	return DemoPlayer().BaseInfo()->m_Speed;
}

float CDemoPlayerClient::Length() const
{
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	return std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0) / (float)SERVER_TICK_SPEED;
}

void CDemoPlayerClient::MarkClip(bool AsStart)
{
	const float Length = this->Length();
	if(Length <= 0.0f)
		return;
	const float Now = Progress() * Length;
	// A mark on the wrong side of the other one takes the other one with it.
	if(AsStart)
		SetClip(Now, HasClip() && m_ClipEnd > Now ? m_ClipEnd : Length);
	else
		SetClip(HasClip() && m_ClipStart < Now ? m_ClipStart : 0.0f, Now);
}

void CDemoPlayerClient::SetClip(float Start, float End)
{
	const float Length = this->Length();
	m_ClipStart = std::clamp(Start, 0.0f, Length);
	m_ClipEnd = End <= m_ClipStart ? -1.0f : std::min(End, Length);
	// Moves into the piece where the demo stands outside it.
	if(HasClip())
	{
		const float Now = Progress() * Length;
		if(Now < m_ClipStart || Now > m_ClipEnd)
			SeekToTime(m_ClipStart);
	}
}

int CDemoPlayerClient::Spectating()
{
	return ViewControl()->SpectatorId(m_DemoSessionId);
}

void CDemoPlayerClient::ChooseSpectate(int SpectatorId)
{
	Spectate(SpectatorId);
	m_SpectateChosen = true;
}

void CDemoPlayerClient::SpectateStep(int Direction)
{
	const int Current = Spectating();
	for(int Offset = 1; Offset <= MAX_CLIENTS; ++Offset)
	{
		// The free view lies between the last player and the first.
		const int Candidate = ((Current < 0 ? (Direction > 0 ? -1 : MAX_CLIENTS) : Current) + Direction * Offset + MAX_CLIENTS + 1) % (MAX_CLIENTS + 1);
		const int SpectatorId = Candidate == MAX_CLIENTS ? SPEC_FREEVIEW : Candidate;
		if(SpectatorId == SPEC_FREEVIEW || PlayerName(SpectatorId) != nullptr)
		{
			Spectate(SpectatorId);
			return;
		}
	}
}

bool CDemoPlayerClient::ServerDemo() const
{
	return str_comp(DemoPlayer().Info()->m_Header.m_aType, "server") == 0;
}

const char *CDemoPlayerClient::PlayerName(int ClientId)
{
	if(!ViewControl()->PlayerName(m_DemoSessionId, ClientId, m_aPlayerName, sizeof(m_aPlayerName)))
		return nullptr;
	return m_aPlayerName;
}

const char *CDemoPlayerClient::Players()
{
	CJsonStringWriter Writer;
	Writer.BeginArray();
	// Only a demo of a server has players to pick from; a client's demo is of
	// whoever recorded it.
	if(ServerDemo())
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			const char *pName = PlayerName(ClientId);
			if(pName == nullptr)
				continue;
			Writer.BeginObject();
			Writer.WriteAttribute("id");
			Writer.WriteIntValue(ClientId);
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(pName);
			Writer.EndObject();
		}
	}
	Writer.EndArray();
	m_Players = Writer.GetOutputString();
	return m_Players.c_str();
}

// `snd_volume` counts in hundredths, a page between 0 and 1.
float CDemoPlayerClient::Volume() const
{
	// While muted this is the volume before, as `<video>` keeps it.
	const int Volume = Muted() ? m_VolumeBeforeMute : g_Config.m_SndVolume;
	return std::clamp(Volume, 0, 100) / 100.0f;
}

void CDemoPlayerClient::SetVolume(float Volume)
{
	const int Level = std::clamp((int)(Volume * 100.0f + 0.5f), 0, 100);
	// Does not unmute.
	if(Muted() && Level > 0)
	{
		m_VolumeBeforeMute = Level;
		return;
	}
	g_Config.m_SndVolume = Level;
	m_VolumeBeforeMute = 0;
}

bool CDemoPlayerClient::Muted() const
{
	return g_Config.m_SndVolume == 0;
}

void CDemoPlayerClient::SetMuted(bool Muted)
{
	if(Muted == this->Muted())
		return;
	if(Muted)
	{
		m_VolumeBeforeMute = g_Config.m_SndVolume;
		g_Config.m_SndVolume = 0;
	}
	else
	{
		// Unmuting a volume that was never set gives a sensible one.
		g_Config.m_SndVolume = m_VolumeBeforeMute > 0 ? m_VolumeBeforeMute : 30;
		m_VolumeBeforeMute = 0;
	}
}

void CDemoPlayerClient::SetRenderOptions(const CViewRenderOptions &Options)
{
	m_RenderOptions = Options;
	ViewControl()->SetRenderOptions(m_DemoSessionId, m_RenderOptions);
#if defined(CONF_VIDEORECORDER)
	if(m_VideoSessionId != m_DemoSessionId)
		ViewControl()->SetRenderOptions(m_VideoSessionId, m_RenderOptions);
#endif
}

float CDemoPlayerClient::WorldPerPixel() const
{
	IEngineGraphics *pGraphics = const_cast<CDemoPlayerClient *>(this)->Graphics();
	const int ScreenWidth = pGraphics->ScreenWidth();
	if(ScreenWidth <= 0)
		return 0.0f;
	float ViewWidth, ViewHeight;
	pGraphics->CalcScreenParams(pGraphics->ScreenAspect(), const_cast<CDemoPlayerClient *>(this)->Zoom(), &ViewWidth, &ViewHeight);
	return ViewWidth / ScreenWidth;
}

void CDemoPlayerClient::OpenDroppedDemo(const char *pPath)
{
#if defined(CONF_VIDEORECORDER)
	// The export reads the file it was started with, so it has to go first.
	if(m_VideoSessionId != m_DemoSessionId)
		CancelExport();
#endif
	if(SessionState(m_DemoSessionId) != ESessionState::OFFLINE)
		StopDemoSession(nullptr);
	str_copy(m_aDemoPath, pPath);
	m_aError[0] = '\0';
	if(const char *pError = PlayDemo())
	{
		log_error("client", "%s", pError);
		m_aError[0] = '\0';
		return;
	}
	++m_LoadCount;
	m_SpectateChosen = false;
	SetClip(0.0f, -1.0f);
	SetRenderOptions(m_RenderOptions);
}

bool CDemoPlayerClient::HandleInput()
{
	m_KeyPressed = false;
	m_MouseClicked = false;
	// Nobody is at the keyboard of a window that is not there.
	if(m_pInput == nullptr)
		return true;
	if(m_pInput->Update())
		return false;

	char aDroppedFile[IO_MAX_PATH_LENGTH];
	if(m_pInput->GetDropFile(aDroppedFile, sizeof(aDroppedFile)))
		OpenDroppedDemo(aDroppedFile);
	if(SessionState(m_DemoSessionId) != ESessionState::READY)
	{
		m_pInput->Clear();
		return true;
	}

	CDemoPlayer &Player = DemoPlayer();
	m_pInput->ConsumeEvents([&](const IInput::CEvent &Event) {
		if((Event.m_Flags & IInput::FLAG_PRESS) == 0)
			return;
		m_KeyPressed = true;
		// Seeking and zooming repeat while a key is held down, the rest does
		// not.
		const bool Repeated = (Event.m_Flags & IInput::FLAG_REPEAT) != 0;
		switch(Event.m_Key)
		{
		case KEY_LEFT: Player.SeekTime(-SEEK_SECONDS); break;
		case KEY_RIGHT: Player.SeekTime(SEEK_SECONDS); break;
		case KEY_J: Player.SeekTime(-JUMP_SECONDS); break;
		case KEY_L: Player.SeekTime(JUMP_SECONDS); break;
		case KEY_KP_PLUS:
		case KEY_MOUSE_WHEEL_UP:
			if(m_ZoomEnabled)
				ScaleZoom(ZOOM_IN_FACTOR);
			break;
		case KEY_KP_MINUS:
		case KEY_MOUSE_WHEEL_DOWN:
			if(m_ZoomEnabled)
				ScaleZoom(1.0f / ZOOM_IN_FACTOR);
			break;
		default:
			if(Repeated)
				break;
			switch(Event.m_Key)
			{
			case KEY_SPACE:
			case KEY_K: TogglePause(); break;
			case KEY_UP: Player.AdjustSpeedIndex(1); break;
			case KEY_DOWN: Player.AdjustSpeedIndex(-1); break;
			case KEY_HOME: Player.SeekPercent(0.0f); break;
			case KEY_M: SetMuted(!Muted()); break;
			case KEY_I: MarkClip(true); break;
			case KEY_O: MarkClip(false); break;
#if !defined(CONF_WEB_PLATFORM)
			// Called from the frame that read the press, which in a browser
			// still counts as the user's action.
			case KEY_F: ViewerFullscreen::Toggle(Window()); break;
#endif
			// Back to whoever recorded the demo, where there is one.
			case KEY_V: ChooseSpectate(Spectating() == SPEC_FREEVIEW ? SPEC_FOLLOW : SPEC_FREEVIEW); break;
			case KEY_N:
				SpectateStep(1);
				m_SpectateChosen = true;
				break;
			case KEY_P:
				SpectateStep(-1);
				m_SpectateChosen = true;
				break;
			default: break;
			}
		}
	});
	UpdateOverlays();

	const CViewerGestures::SResult Gesture = m_Gestures.Update(m_pInput->TouchFingerStates(), vec2(Graphics()->ScreenWidth(), Graphics()->ScreenHeight()));
	if(Gesture.m_Active)
	{
		// Two fingers still drag where zooming is off.
		if(m_ZoomEnabled)
			ScaleZoom(Gesture.m_Zoom);
		MoveFreeView(-Gesture.m_Move * WorldPerPixel());
#if !defined(CONF_WEB_PLATFORM)
		m_Controls.Show();
#endif
	}

	// Dragging moves the free view, in drawn pixels so that the world keeps up
	// with the pointer on high density screens. Presses on the bar and pinches
	// are not drags.
	const vec2 MousePos = m_pInput->NativeMousePos() * Graphics()->ScreenHiDPIScale();
#if defined(CONF_WEB_PLATFORM)
	const bool OverControls = false;
#else
	const bool OverControls = m_Controls.Hovered();
#endif
	if(m_pInput->NativeMousePressed(1) && !OverControls && !Gesture.m_Active)
	{
		if(m_Dragging)
			MoveFreeView((m_LastMousePos - MousePos) * WorldPerPixel());
		m_Dragging = true;
	}
	else
	{
		m_Dragging = false;
	}
	m_LastMousePos = MousePos;

	// From the frame's presses, because a tap can be shorter than a frame. The
	// bar is drawn after the game, which has cleared them by then.
	m_MouseClicked = m_pInput->KeyPress(KEY_MOUSE_1);
	// The keys steer the player, not the components a player steers a tee
	// with.
	m_pInput->Clear();
	return true;
}

void CDemoPlayerClient::UpdateOverlays()
{
	const auto &&Hold = [&](bool &Shown, int Key, const char *pCommand) {
		const bool Wanted = m_OverlaysEnabled && m_pInput->KeyIsPressed(Key);
		if(Wanted != Shown)
		{
			Shown = Wanted;
			m_pConsole->ExecuteLineStroked(Wanted ? 1 : 0, pCommand, IConsole::CLIENT_ID_UNSPECIFIED);
		}
	};
	Hold(m_ScoreboardShown, KEY_TAB, "+scoreboard");
	Hold(m_StatboardShown, KEY_EQUALS, "+statboard");
}

#if !defined(CONF_WEB_PLATFORM)
void CDemoPlayerClient::RenderControls()
{
	// No pointer without a window.
	if(!m_ShowControls || m_pInput == nullptr || SessionState(m_DemoSessionId) != ESessionState::READY)
		return;
	const float Total = Length();
	if(Total <= 0.0f)
		return;

	const auto &&FormatTime = [](char *pBuffer, size_t Size, float Seconds) {
		const int Whole = std::max((int)(Seconds + 0.5f), 0);
		str_format(pBuffer, Size, "%d:%02d", Whole / 60, Whole % 60);
	};
	char aElapsed[16];
	char aLength[16];
	FormatTime(aElapsed, sizeof(aElapsed), Progress() * Total);
	FormatTime(aLength, sizeof(aLength), Total);
	char aTime[40];
	str_format(aTime, sizeof(aTime), "%s / %s", aElapsed, aLength);
	char aSpeed[16];
	str_format(aSpeed, sizeof(aSpeed), "%.2fx", Speed());
	// Long names are cut short so the bar does not cover the demo.
	char aSpectating[20];
	const int SpectatorId = Spectating();
	if(const char *pSpectating = PlayerName(SpectatorId); pSpectating != nullptr)
		str_copy(aSpectating, pSpectating);
	else
		str_copy(aSpectating, SpectatorId == SPEC_FOLLOW ? "Follow" : "Free view");

#if defined(CONF_VIDEORECORDER)
	char aFps[16];
	str_format(aFps, sizeof(aFps), "%d fps", m_ExportFps);
	// While a video is written, the export button shows its progress and
	// stops it.
	const bool IsExporting = Exporting();
	char aExportProgress[32];
	{
		const float SecondsLeft = ExportSecondsLeft();
		char aLeft[16] = "";
		if(SecondsLeft >= 0.0f)
			str_format(aLeft, sizeof(aLeft), " %d:%02d", (int)SecondsLeft / 60, (int)SecondsLeft % 60);
		str_format(aExportProgress, sizeof(aExportProgress), "%d%%%s", (int)(ExportProgress() * 100.0f + 0.5f), aLeft);
	}
	const bool CanExport = VideoEncodingSupported();
#else
	const bool IsExporting = false;
	const bool CanExport = false;
#endif

	// Left to right as in a video player: playback, speed and position, then
	// what is watched and the export.
	enum
	{
		ITEM_SEEK,
		ITEM_PLAY,
		ITEM_RESTART,
		ITEM_SLOWER,
		ITEM_SPEED,
		ITEM_FASTER,
		ITEM_TIME,
		ITEM_CLIP_START,
		ITEM_CLIP_END,
		ITEM_CLIP_CLEAR,
		ITEM_VOLUME,
		ITEM_SPACER,
		ITEM_ZOOM_RESET,
		ITEM_CAMERA,
		ITEM_SPECTATE,
		ITEM_EXPORT,
		ITEM_FULLSCREEN,
		// Picking a size starts the export.
		ITEM_EXPORT_CHOOSE,
		ITEM_EXPORT_SOUND,
		ITEM_EXPORT_FPS,
		ITEM_EXPORT_AS_SHOWN,
		ITEM_EXPORT_720,
		ITEM_EXPORT_1080,
		// Steps rather than a slider, which has no room on a phone.
		ITEM_VOLUME_MUTE,
		ITEM_VOLUME_25,
		ITEM_VOLUME_50,
		ITEM_VOLUME_75,
		ITEM_VOLUME_100,
		NUM_ITEMS,
	};
	constexpr int MenuExport = 1;
	constexpr int MenuVolume = 2;
	// The players to pick from; a demo a client recorded only offers following
	// whoever recorded it or looking around.
	std::vector<int> vPickable;
	std::vector<std::string> vPickableNames;
	if(ServerDemo())
	{
		vPickable.push_back(SPEC_FREEVIEW);
		vPickableNames.emplace_back("Free view");
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(const char *pName = PlayerName(ClientId); pName != nullptr)
			{
				vPickable.push_back(ClientId);
				vPickableNames.emplace_back(pName);
			}
		}
	}

	std::vector<CViewerControls::SItem> vItems(NUM_ITEMS + vPickable.size());
	CViewerControls::SItem *aItems = vItems.data();
	aItems[ITEM_SEEK].m_Type = CViewerControls::EItem::SLIDER;
	aItems[ITEM_SEEK].m_Value = Progress();
	if(HasClip())
	{
		aItems[ITEM_SEEK].m_RangeStart = ClipStart() / Total;
		aItems[ITEM_SEEK].m_RangeEnd = ClipEnd() / Total;
	}
	aItems[ITEM_PLAY].m_Icon = Paused() ? CViewerControls::EIcon::PLAY : CViewerControls::EIcon::PAUSE;
	aItems[ITEM_RESTART].m_Icon = CViewerControls::EIcon::RESTART;
	aItems[ITEM_SLOWER].m_Icon = CViewerControls::EIcon::MINUS;
	aItems[ITEM_SLOWER].m_Optional = true;
	aItems[ITEM_SPEED].m_Type = CViewerControls::EItem::TEXT;
	aItems[ITEM_SPEED].m_pText = aSpeed;
	aItems[ITEM_SPEED].m_Width = 56.0f;
	aItems[ITEM_SPEED].m_Optional = true;
	aItems[ITEM_FASTER].m_Icon = CViewerControls::EIcon::PLUS;
	aItems[ITEM_FASTER].m_Optional = true;
	aItems[ITEM_TIME].m_Type = CViewerControls::EItem::TEXT;
	aItems[ITEM_TIME].m_pText = aTime;
	// The marked piece: both ends, and clearing it while there is one.
	aItems[ITEM_CLIP_START].m_Icon = CViewerControls::EIcon::CLIP_START;
	aItems[ITEM_CLIP_START].m_Optional = true;
	aItems[ITEM_CLIP_END].m_Icon = CViewerControls::EIcon::CLIP_END;
	aItems[ITEM_CLIP_END].m_Optional = true;
	aItems[ITEM_CLIP_CLEAR].m_Icon = CViewerControls::EIcon::CLIP_CLEAR;
	aItems[ITEM_CLIP_CLEAR].m_Optional = true;
	aItems[ITEM_CLIP_CLEAR].m_Hidden = !HasClip();
	aItems[ITEM_VOLUME].m_Icon = Muted() ? CViewerControls::EIcon::VOLUME_OFF : CViewerControls::EIcon::VOLUME;
	aItems[ITEM_VOLUME].m_OpensMenu = true;
	aItems[ITEM_VOLUME].m_MenuId = MenuVolume;
	aItems[ITEM_VOLUME].m_Optional = true;
	aItems[ITEM_SPACER].m_Type = CViewerControls::EItem::SPACER;
	// Only once somebody zoomed.
	aItems[ITEM_ZOOM_RESET].m_Icon = CViewerControls::EIcon::ZOOM_RESET;
	aItems[ITEM_ZOOM_RESET].m_Hidden = !m_ZoomEnabled || !ZoomChanged();
	// Only where the demo brought a view of its own.
	aItems[ITEM_CAMERA].m_Icon = CViewerControls::EIcon::FIT;
	aItems[ITEM_CAMERA].m_Active = RecordedCamera();
	aItems[ITEM_CAMERA].m_Hidden = !RecordedCameraAvailable();
	aItems[ITEM_SPECTATE].m_Icon = CViewerControls::EIcon::EYE;
	aItems[ITEM_SPECTATE].m_pText = aSpectating;
	aItems[ITEM_SPECTATE].m_OpensMenu = !vPickable.empty();
	// A demo a client recorded has nobody to pick from; the keys still step
	// through whoever is there.
	aItems[ITEM_SPECTATE].m_Hidden = vPickable.empty();
	aItems[ITEM_EXPORT].m_Icon = IsExporting ? CViewerControls::EIcon::STOP : CViewerControls::EIcon::SAVE;
#if defined(CONF_VIDEORECORDER)
	aItems[ITEM_EXPORT].m_pText = IsExporting ? aExportProgress : nullptr;
#endif
	aItems[ITEM_EXPORT].m_OpensMenu = !IsExporting;
	aItems[ITEM_EXPORT].m_MenuId = MenuExport;
	aItems[ITEM_EXPORT].m_Hidden = !CanExport;
	aItems[ITEM_FULLSCREEN].m_Icon = CViewerControls::EIcon::FULLSCREEN;
	aItems[ITEM_FULLSCREEN].m_Active = ViewerFullscreen::Active(Window());
	aItems[ITEM_FULLSCREEN].m_Hidden = !ViewerFullscreen::Supported(Window());
	// A server demo's export needs somebody picked first.
	const bool ExportNeedsChoice = ServerDemo() && !m_SpectateChosen;
	aItems[ITEM_EXPORT_CHOOSE].m_Type = CViewerControls::EItem::TEXT;
	aItems[ITEM_EXPORT_CHOOSE].m_pText = "Pick whom to follow first";
	aItems[ITEM_EXPORT_SOUND].m_pText = "Sound";
#if defined(CONF_VIDEORECORDER)
	aItems[ITEM_EXPORT_SOUND].m_Active = m_ExportAudio;
	aItems[ITEM_EXPORT_FPS].m_pText = aFps;
#endif
	aItems[ITEM_EXPORT_SOUND].m_KeepsMenu = true;
	aItems[ITEM_EXPORT_FPS].m_KeepsMenu = true;
	aItems[ITEM_EXPORT_AS_SHOWN].m_Icon = CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT_AS_SHOWN].m_pText = "As shown";
	aItems[ITEM_EXPORT_720].m_Icon = CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT_720].m_pText = "1280 x 720";
	aItems[ITEM_EXPORT_1080].m_Icon = CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT_1080].m_pText = "1920 x 1080";
	for(int i = ITEM_EXPORT_CHOOSE; i <= ITEM_EXPORT_1080; ++i)
	{
		aItems[i].m_InMenu = true;
		aItems[i].m_MenuId = MenuExport;
		aItems[i].m_Hidden = !CanExport || IsExporting;
		aItems[i].m_Disabled = ExportNeedsChoice && i >= ITEM_EXPORT_AS_SHOWN;
	}
	aItems[ITEM_EXPORT_CHOOSE].m_Hidden = aItems[ITEM_EXPORT_CHOOSE].m_Hidden || !ExportNeedsChoice;
	aItems[ITEM_VOLUME_MUTE].m_Icon = CViewerControls::EIcon::VOLUME_OFF;
	aItems[ITEM_VOLUME_MUTE].m_pText = "Mute";
	aItems[ITEM_VOLUME_MUTE].m_Active = Muted();
	static const char *const s_apVolumeText[] = {"25%", "50%", "75%", "100%"};
	for(int i = ITEM_VOLUME_25; i <= ITEM_VOLUME_100; ++i)
	{
		const float Level = (i - ITEM_VOLUME_25 + 1) * 0.25f;
		aItems[i].m_Icon = CViewerControls::EIcon::VOLUME;
		aItems[i].m_pText = s_apVolumeText[i - ITEM_VOLUME_25];
		// The nearest step, also for volumes a page set in between.
		aItems[i].m_Active = !Muted() && std::abs(Volume() - Level) < 0.125f;
	}
	for(int i = ITEM_VOLUME_MUTE; i <= ITEM_VOLUME_100; ++i)
	{
		aItems[i].m_InMenu = true;
		aItems[i].m_MenuId = MenuVolume;
	}
	for(size_t i = 0; i < vPickable.size(); ++i)
	{
		CViewerControls::SItem &Item = aItems[NUM_ITEMS + i];
		Item.m_InMenu = true;
		Item.m_Active = vPickable[i] == SpectatorId;
		Item.m_Icon = vPickable[i] == SPEC_FREEVIEW ? CViewerControls::EIcon::FREEVIEW : CViewerControls::EIcon::EYE;
		Item.m_pText = vPickableNames[i].c_str();
	}

	CViewerControls::SInput Input;
	Input.m_MousePos = m_pInput->NativeMousePos();
	Input.m_MousePressed = m_pInput->NativeMousePressed(1);
	Input.m_MouseClicked = m_MouseClicked;
	Input.m_KeyPressed = m_KeyPressed;

	float SeekTo = 0.0f;
	const int Pressed = m_Controls.Render(aItems, vItems.size(), Input, &SeekTo);

	// The demo stands still while the seek bar is dragged, and goes on
	// afterwards if it was going before.
	if(m_Controls.Dragging() != m_Seeking)
	{
		m_Seeking = m_Controls.Dragging();
		if(m_Seeking)
		{
			m_PausedBeforeSeeking = Paused();
			SetPaused(true);
		}
		else if(!m_PausedBeforeSeeking)
		{
			SetPaused(false);
		}
	}

	if(Pressed >= NUM_ITEMS)
	{
		ChooseSpectate(vPickable[Pressed - NUM_ITEMS]);
		return;
	}

	switch(Pressed)
	{
	case ITEM_PLAY: TogglePause(); break;
	case ITEM_SEEK: DemoPlayer().SeekPercent(SeekTo); break;
	case ITEM_CLIP_START: MarkClip(true); break;
	case ITEM_CLIP_END: MarkClip(false); break;
	case ITEM_CLIP_CLEAR: SetClip(0.0f, -1.0f); break;
	case ITEM_VOLUME_MUTE: SetMuted(!Muted()); break;
	case ITEM_VOLUME_25:
	case ITEM_VOLUME_50:
	case ITEM_VOLUME_75:
	case ITEM_VOLUME_100: SetVolume((Pressed - ITEM_VOLUME_25 + 1) * 0.25f); break;
	case ITEM_SLOWER: DemoPlayer().AdjustSpeedIndex(-1); break;
	case ITEM_FASTER: DemoPlayer().AdjustSpeedIndex(1); break;
	case ITEM_RESTART: SeekStart(); break;
	case ITEM_ZOOM_RESET: ResetZoom(); break;
	case ITEM_CAMERA: SetRecordedCamera(!RecordedCamera()); break;
	case ITEM_SPECTATE: SpectateStep(1); break;
	// Only while a video is written, when the menu does not open.
	case ITEM_EXPORT: RequestCancelExport(); break;
	case ITEM_FULLSCREEN: ViewerFullscreen::Toggle(Window()); break;
#if defined(CONF_VIDEORECORDER)
	case ITEM_EXPORT_SOUND: m_ExportAudio = !m_ExportAudio; break;
	case ITEM_EXPORT_FPS: m_ExportFps = m_ExportFps == 60 ? 30 : 60; break;
#endif
	case ITEM_EXPORT_AS_SHOWN: ExportFromControls(Graphics()->ScreenWidth(), Graphics()->ScreenHeight()); break;
	case ITEM_EXPORT_720: ExportFromControls(1280, 720); break;
	case ITEM_EXPORT_1080: ExportFromControls(1920, 1080); break;
	default: break;
	}
}
#endif

void CDemoPlayerClient::RenderWindowFrame()
{
	// The window keeps its own clock, apart from an export's.
	const int64_t Now = time_get();
	const int64_t Last = m_LastWindowRenderTime == 0 ? m_LastRenderTime : m_LastWindowRenderTime;
	m_RenderFrameTime = (Now - Last) / (float)time_freq();
	m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;
	m_LastWindowRenderTime = Now;
#if defined(CONF_VIDEORECORDER)
	if(m_pVideo == nullptr)
#endif
		m_LastRenderTime = Now;

	GameClient()->OnRenderPrepare();
	GameClient()->OnRender();
	GameClient()->OnRenderFinalize();
#if !defined(CONF_WEB_PLATFORM)
	// Over everything else, before the frame goes out.
	RenderControls();
#endif
	Graphics()->Swap();
	// The clock the camera and the zoom ease on; without it they never arrive.
	m_LocalTime = (time_get() - m_LocalStartTime) / (float)time_freq();
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}

bool CDemoPlayerClient::RequestExport(const CVideoExportSettings &Settings, int SpectatorId)
{
#if defined(CONF_VIDEORECORDER)
	if(m_pVideo != nullptr || m_ExportState == EExportState::RUNNING)
		return false;
	// Starting waits for the browser, so it is done between two frames.
	m_vPageActions.emplace_back([this, Settings, SpectatorId] { StartExport(Settings, SpectatorId); });
	m_ExportState = EExportState::RUNNING;
	m_ExportStartTime = time_get_nanoseconds();
	m_aExportError[0] = '\0';
	return true;
#else
	return false;
#endif
}

void CDemoPlayerClient::ExportFromControls(int Width, int Height)
{
#if defined(CONF_VIDEORECORDER)
	CVideoExportSettings Settings = CCommandLineVideoExport::Settings();
	// Encoders take whole pairs of lines.
	Settings.m_Width = std::max(Width & ~1, 2);
	Settings.m_Height = std::max(Height & ~1, 2);
	Settings.m_FPS = m_ExportFps;
	Settings.m_Audio = m_ExportAudio;
	RequestExport(Settings, ServerDemo() ? Spectating() : SPEC_FOLLOW);
#endif
}

void CDemoPlayerClient::RequestCancelExport()
{
#if defined(CONF_VIDEORECORDER)
	// Letting the encoder go while a frame is drawn would pull it out from
	// under that frame, so the loop does it between frames.
	m_vPageActions.emplace_back([this] { CancelExport(); });
#endif
}

float CDemoPlayerClient::ExportProgress() const
{
#if defined(CONF_VIDEORECORDER)
	if(m_pVideo == nullptr)
		return 0.0f;
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_VideoSessionId).m_DemoPlayer.BaseInfo();
	// A marked piece starts and ends apart from the demo.
	const int First = m_VideoFirstTick >= 0 ? m_VideoFirstTick : pInfo->m_FirstTick;
	const int Last = m_VideoLastTick >= 0 ? m_VideoLastTick : pInfo->m_LastTick;
	const int Total = std::max(Last - First, 0);
	return Total == 0 ? 0.0f : std::clamp(pInfo->m_CurrentTick - First, 0, Total) / (float)Total;
#else
	return 0.0f;
#endif
}

float CDemoPlayerClient::ExportSecondsLeft() const
{
#if defined(CONF_VIDEORECORDER)
	if(m_pVideo == nullptr)
		return -1.0f;
	// Averaged over the whole export, like the client's progress box: the rate
	// the encoder reports jumps about.
	const float Progress = ExportProgress();
	const float Elapsed = std::chrono::duration<float>(time_get_nanoseconds() - m_ExportStartTime).count();
	if(Elapsed < 1.0f || Progress < 0.01f)
		return -1.0f;
	return Elapsed * (1.0f - Progress) / Progress;
#else
	return -1.0f;
#endif
}

CDemoPlayerClient::EExportState CDemoPlayerClient::ExportState() const
{
#if defined(CONF_VIDEORECORDER)
	return m_ExportState;
#else
	return EExportState::IDLE;
#endif
}

const char *CDemoPlayerClient::ExportError() const
{
#if defined(CONF_VIDEORECORDER)
	return m_aExportError;
#else
	return "";
#endif
}

#if defined(CONF_VIDEORECORDER)
bool CDemoPlayerClient::StartExport(const CVideoExportSettings &Settings, int SpectatorId)
{
	if(m_pVideo != nullptr)
		return false;
	m_ExportState = EExportState::RUNNING;
	m_aExportError[0] = '\0';
	m_Settings = Settings;
	// Encoders take whole macroblocks, and a window can have any size.
	m_Settings.m_Width = std::clamp(Settings.m_Width, 2, 8192) & ~1;
	m_Settings.m_Height = std::clamp(Settings.m_Height, 2, 8192) & ~1;
	m_Settings.m_FPS = std::clamp(Settings.m_FPS, 1, 240);
	m_Settings.m_Crf = std::clamp(Settings.m_Crf, 0, 51);
	// The name of the demo, so that whoever ends up with the file knows what
	// it is a video of. It is written where the user's own files go and handed
	// to the browser from there.
	char aName[IO_MAX_PATH_LENGTH];
	fs_split_file_extension(fs_filename(m_aDemoPath), aName, sizeof(aName));
	str_format(m_aVideoPath, sizeof(m_aVideoPath), "videos/%s.mp4", aName);
	// The session of the last export is closed here rather than when it
	// ended, so that its file stays readable until then.
	if(SessionState(m_ExportSessionId) != ESessionState::OFFLINE)
		StopDemoSession(m_ExportSessionId, nullptr);
	m_VideoSessionId = m_ExportSessionId;
	// From the first tick, wherever the watched demo stands: a demo that
	// played out sits on its last frame.
	const char *pError = PlayDemo(m_ExportSessionId);
	m_VideoFirstTick = -1;
	m_VideoLastTick = -1;
	if(pError == nullptr)
	{
		// Whom it follows and how it draws are its own.
		ViewControl()->SetSpectatorId(m_ExportSessionId, SpectatorId);
		ViewControl()->SetRenderOptions(m_ExportSessionId, m_RenderOptions);
		// Only the marked piece, where there is one.
		if(HasClip())
		{
			CDemoPlayer &ExportPlayer = DemoSource(m_ExportSessionId).m_DemoPlayer;
			const IDemoPlayer::CInfo *pInfo = ExportPlayer.BaseInfo();
			m_VideoFirstTick = pInfo->m_FirstTick + round_truncate(m_ClipStart * (float)SERVER_TICK_SPEED);
			m_VideoLastTick = pInfo->m_FirstTick + round_truncate(m_ClipEnd * (float)SERVER_TICK_SPEED);
			ExportPlayer.SeekTime(m_ClipStart);
		}
		pError = StartVideo();
	}
	if(pError != nullptr)
	{
		log_error("videorecorder", "%s", pError);
		str_copy(m_aExportError, pError);
		m_aError[0] = '\0';
		m_aVideoPath[0] = '\0';
		m_ExportState = EExportState::FAILED;
		m_pVideo.reset();
		StopDemoSession(m_ExportSessionId, nullptr);
		m_VideoSessionId = m_DemoSessionId;
		return false;
	}
	return true;
}

void CDemoPlayerClient::CancelExport()
{
	if(m_pVideo == nullptr)
		return;
	if(IVideo::Current() == m_pVideo.get())
		m_pVideo->Cancel();
	// The export's session goes before the encoder, because it hands the
	// encoder its last frames as it stops.
	if(SessionState(m_ExportSessionId) != ESessionState::OFFLINE)
		StopDemoSession(m_ExportSessionId, nullptr);
	m_pVideo.reset();
	m_aVideoPath[0] = '\0';
	m_ExportState = EExportState::IDLE;
	m_VideoSessionId = m_DemoSessionId;
}

void CDemoPlayerClient::FinishExport()
{
	// The demo player has closed the file already when the demo ran out.
	if(IVideo::Current() == m_pVideo.get())
		m_pVideo->Stop();
	// An export of a marked piece stops before its demo ends.
	if(SessionState(m_ExportSessionId) != ESessionState::OFFLINE)
		StopDemoSession(m_ExportSessionId, nullptr);
	const CVideoExportStatus Status = m_pVideo->Status();
	const bool Failed = Status.m_HasError;
	m_pVideo.reset();
	if(Failed)
		str_copy(m_aExportError, Status.m_aError[0] == '\0' ? "The video could not be written." : Status.m_aError);
	log_info("videorecorder", Failed ? "Export failed" : "Export completed");
	m_aVideoPath[0] = '\0';
	m_ExportState = Failed ? EExportState::FAILED : EExportState::FINISHED;
	m_VideoSessionId = m_DemoSessionId;
}

void CDemoPlayerClient::UpdateExport()
{
	if(!Exporting())
	{
		// The demo player closes the video when the demo runs out under it.
		FinishExport();
		return;
	}
	if(m_VideoLastTick >= 0 && DemoSource(m_ExportSessionId).m_DemoPlayer.BaseInfo()->m_CurrentTick >= m_VideoLastTick)
	{
		FinishExport();
		return;
	}
	RenderExportFrame();
	// The export encodes as fast as it can; the window is drawn in between
	// only often enough to stay usable.
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(m_pInput != nullptr && Now - m_LastExportScreenRender >= EXPORT_SCREEN_INTERVAL)
	{
		m_LastExportScreenRender = Now;
		RenderWindowFrame();
	}
}
#endif

int CDemoPlayerClient::Run()
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	gs_pDemoPlayer = this;
#endif
	// A machine without a display can still draw into a surface without a
	// window, the same way the client can. Nobody is at its keyboard.
	m_Surfaceless = std::getenv("GFX_SURFACELESS") != nullptr;
	if(!m_Surfaceless)
		m_pInput = Kernel()->RequestInterface<IEngineInput>();
	// The game limits the view to 5:4 so that nobody sees further than others;
	// a player fills the window, which matters most on a phone held upright.
	g_Config.m_GfxWholeWindow = 1;
	int ExitCode = 1;
#if defined(CONF_WEB_PLATFORM)
	IEngineGraphicsWindow *pWindow = m_Surfaceless ? CreateOffscreenGraphicsWindow() : CreateWebGraphicsWindow();
#else
	IEngineGraphicsWindow *pWindow = m_Surfaceless ? CreateOffscreenGraphicsWindow() : CreateSdlGraphicsWindow();
#endif
	if(InitGame(pWindow, m_pInput))
	{
		// The input grabs the pointer for aiming; a player leaves it free.
		if(m_pInput != nullptr)
			m_pInput->MouseModeAbsolute();
#if !defined(CONF_WEB_PLATFORM)
		m_Controls.Init(Graphics(), TextRender());
#endif

		// Without a demo, a window waits for one to be dropped on it. A
		// surface has nobody to drop one.
		const char *pError = nullptr;
		if(m_aDemoPath[0] != '\0')
			pError = PlayDemo();
		else if(m_Surfaceless)
			pError = "No demo was given, and a surface without a window has nowhere to drop one.";
		if(pError != nullptr)
			log_error("client", "%s", pError);
		else
		{
			if(m_aDemoPath[0] != '\0')
			{
				++m_LoadCount;
				SetRenderOptions(m_RenderOptions);
				// The start a link asked for, applied here because seeking reads
				// the file, which in a browser waits on this stack.
				if(m_StartTime >= 0.0f)
					SeekToTime(m_StartTime);
				if(m_StartSpeed > 0.0f)
					SetSpeed(m_StartSpeed);
				if(m_StartPaused)
					SetPaused(true);
			}
			while(State() != IClient::STATE_QUITTING)
			{
				// See `FromPage`.
				RunPageActions();
				if(!HandleInput())
					break;
				const bool Playing = SessionState(m_DemoSessionId) == ESessionState::READY;
				// Playback stops at the end of a marked piece as at the end of the
				// demo.
				if(Playing && HasClip() && !Paused() && Progress() * Length() >= m_ClipEnd)
				{
					SeekToTime(m_ClipEnd);
					SetPaused(true);
				}
				// A demo that has run out pauses on its last frame, so the
				// player can seek back into it. Without a window it is the end.
				if(m_Surfaceless && (!Playing || DemoPlayer().BaseInfo()->m_CurrentTick >= DemoPlayer().BaseInfo()->m_LastTick))
					break;
				Update();
#if defined(CONF_VIDEORECORDER)
				if(m_pVideo != nullptr)
				{
					UpdateExport();
					continue;
				}
#endif
				RenderWindowFrame();
				thread_sleep_until_next_frame(m_NextFrameTime, g_Config.m_ClRefreshRate);
			}
			if(m_aError[0] != '\0')
				log_error("client", "%s", m_aError);
			else
				ExitCode = 0;
		}
	}
	ShutdownGame();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	gs_pDemoPlayer = nullptr;
#endif
	return ExitCode;
}

int main(int argc, const char **argv)
{
	return DemoClientMain(new CDemoPlayerClient, argc, argv);
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
namespace
{
	// The page may call in while the player is unwound in a wait, where
	// waiting again would take it down. See `CDemoPlayerClient::FromPage`.
	void FromPage(std::function<void()> &&Action)
	{
		if(gs_pDemoPlayer != nullptr)
			gs_pDemoPlayer->FromPage(std::move(Action));
	}
} // namespace

// What the page calls. Everything is safe to call before a demo plays and
// after it stopped.
extern "C" {

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetPaused(int Paused)
{
	FromPage([Paused] {
		if(Paused != 0)
			gs_pDemoPlayer->SetPaused(true);
		else
			gs_pDemoPlayer->Play();
	});
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSeekPercent(float Percent)
{
	FromPage([Percent] { gs_pDemoPlayer->SeekPercent(Percent); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSeekToTime(float Seconds)
{
	FromPage([Seconds] { gs_pDemoPlayer->SeekToTime(Seconds); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetSpeed(float Speed)
{
	FromPage([Speed] { gs_pDemoPlayer->SetSpeed(Speed); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSeekStart()
{
	FromPage([] { gs_pDemoPlayer->SeekStart(); });
}

// -1 is the free view, -2 whoever recorded the demo, 0 and up its players.
EMSCRIPTEN_KEEPALIVE void DemoPlayerSetSpectate(int SpectatorId)
{
	FromPage([SpectatorId] { gs_pDemoPlayer->ChooseSpectate(SpectatorId); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetSpectateName(const char *pName)
{
	// The page frees the name once this returns.
	std::string Name = pName == nullptr ? "" : pName;
	FromPage([Name = std::move(Name)] { gs_pDemoPlayer->SetSpectateName(Name.c_str()); });
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerSpectating()
{
	return gs_pDemoPlayer == nullptr ? SPEC_FREEVIEW : gs_pDemoPlayer->Spectating();
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSpectateStep(int Direction)
{
	FromPage([Direction] {
		gs_pDemoPlayer->SpectateStep(Direction);
		gs_pDemoPlayer->ChooseSpectate(gs_pDemoPlayer->Spectating());
	});
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerServerDemo()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->LoadCount() > 0 && gs_pDemoPlayer->ServerDemo() ? 1 : 0;
}

// Valid until the next call.
EMSCRIPTEN_KEEPALIVE const char *DemoPlayerPlayers()
{
	return gs_pDemoPlayer == nullptr ? "[]" : gs_pDemoPlayer->Players();
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerLoadCount()
{
	return gs_pDemoPlayer == nullptr ? 0 : gs_pDemoPlayer->LoadCount();
}

// Between 0 and 1.
EMSCRIPTEN_KEEPALIVE float DemoPlayerVolume()
{
	return gs_pDemoPlayer == nullptr ? 0.0f : gs_pDemoPlayer->Volume();
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetVolume(float Volume)
{
	FromPage([Volume] { gs_pDemoPlayer->SetVolume(Volume); });
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerMuted()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->Muted() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetMuted(int Muted)
{
	FromPage([Muted] { gs_pDemoPlayer->SetMuted(Muted != 0); });
}

// The marked piece in seconds, with a negative end where nothing is marked.
EMSCRIPTEN_KEEPALIVE float DemoPlayerClipStart()
{
	return gs_pDemoPlayer == nullptr ? 0.0f : gs_pDemoPlayer->ClipStart();
}

EMSCRIPTEN_KEEPALIVE float DemoPlayerClipEnd()
{
	return gs_pDemoPlayer == nullptr ? -1.0f : gs_pDemoPlayer->ClipEnd();
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetClip(float Start, float End)
{
	FromPage([Start, End] { gs_pDemoPlayer->SetClip(Start, End); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerMarkClip(int AsStart)
{
	FromPage([AsStart] { gs_pDemoPlayer->MarkClip(AsStart != 0); });
}

// How much of the world is in the window, for zoom buttons of the page.
EMSCRIPTEN_KEEPALIVE void DemoPlayerZoomBy(float Factor)
{
	FromPage([Factor] { gs_pDemoPlayer->ScaleZoom(Factor); });
}

EMSCRIPTEN_KEEPALIVE float DemoPlayerZoom()
{
	return gs_pDemoPlayer == nullptr ? 1.0f : gs_pDemoPlayer->Zoom();
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerResetZoom()
{
	FromPage([] { gs_pDemoPlayer->ResetZoom(); });
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerZoomChanged()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->ZoomChanged() ? 1 : 0;
}

// Whether the wheel, the zoom keys and a pinch zoom; `DemoPlayerZoomBy` works
// either way.
EMSCRIPTEN_KEEPALIVE void DemoPlayerSetZoomEnabled(int Enabled)
{
	FromPage([Enabled] { gs_pDemoPlayer->SetZoomEnabled(Enabled != 0); });
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerZoomEnabled()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->ZoomEnabled() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetOverlays(int Enabled)
{
	FromPage([Enabled] { gs_pDemoPlayer->SetOverlaysEnabled(Enabled != 0); });
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerOverlays()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->OverlaysEnabled() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetKeyPresses(int Show)
{
	FromPage([Show] {
		CViewRenderOptions Options = gs_pDemoPlayer->RenderOptions();
		Options.m_ShowDirection = Show != 0 ? 1 : 0;
		gs_pDemoPlayer->SetRenderOptions(Options);
	});
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerKeyPresses()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->RenderOptions().m_ShowDirection != 0 ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetHighDetail(int On)
{
	FromPage([On] {
		CViewRenderOptions Options = gs_pDemoPlayer->RenderOptions();
		Options.m_HighDetail = On != 0;
		gs_pDemoPlayer->SetRenderOptions(Options);
	});
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerHighDetail()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->RenderOptions().m_HighDetail ? 1 : 0;
}

// 0 where the demo brought no view of its own, 1 where it did and it is not
// followed, 2 where it is.
EMSCRIPTEN_KEEPALIVE int DemoPlayerRecordedCamera()
{
	if(gs_pDemoPlayer == nullptr || !gs_pDemoPlayer->RecordedCameraAvailable())
		return 0;
	return gs_pDemoPlayer->RecordedCamera() ? 2 : 1;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetRecordedCamera(int Use)
{
	FromPage([Use] { gs_pDemoPlayer->SetRecordedCamera(Use != 0); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerQuit()
{
	FromPage([] { gs_pDemoPlayer->Quit(); });
}

// Only asks: `DemoPlayerExportState` says a moment later whether the browser
// had an encoder.
EMSCRIPTEN_KEEPALIVE int DemoPlayerStartExport(int Width, int Height, int Fps, int Audio, int Crf, const char *pCodec, int Hud, int Chat, int SpectatorId)
{
	if(gs_pDemoPlayer == nullptr)
		return 0;
	CVideoExportSettings Settings;
	Settings.m_Width = Width;
	Settings.m_Height = Height;
	Settings.m_FPS = Fps;
	Settings.m_Audio = Audio != 0;
	Settings.m_Crf = Crf;
	str_copy(Settings.m_aVideoCodec, pCodec == nullptr ? "" : pCodec);
	Settings.m_ShowHud = Hud != 0;
	Settings.m_ShowChat = Chat != 0;
	return gs_pDemoPlayer->RequestExport(Settings, SpectatorId) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetSize(int Width, int Height)
{
	FromPage([Width, Height] { gs_pDemoPlayer->SetSize(Width, Height); });
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerSetControls(int Show)
{
	FromPage([Show] { gs_pDemoPlayer->SetShowControls(Show != 0); });
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerControls()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->ShowControls() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoPlayerCancelExport()
{
	if(gs_pDemoPlayer != nullptr)
		gs_pDemoPlayer->RequestCancelExport();
}

EMSCRIPTEN_KEEPALIVE const char *DemoPlayerExportError()
{
	return gs_pDemoPlayer == nullptr ? "" : gs_pDemoPlayer->ExportError();
}

// 0 while none was asked for, 1 while one is written, 2 when the last one was
// handed over and 3 when it failed.
EMSCRIPTEN_KEEPALIVE int DemoPlayerExportState()
{
	return gs_pDemoPlayer == nullptr ? 0 : (int)gs_pDemoPlayer->ExportState();
}

EMSCRIPTEN_KEEPALIVE float DemoPlayerExportSecondsLeft()
{
	return gs_pDemoPlayer == nullptr ? -1.0f : gs_pDemoPlayer->ExportSecondsLeft();
}

// Between 0 and 1, of the export rather than of the demo in the window.
EMSCRIPTEN_KEEPALIVE float DemoPlayerExportProgress()
{
	return gs_pDemoPlayer == nullptr ? 0.0f : gs_pDemoPlayer->ExportProgress();
}

EMSCRIPTEN_KEEPALIVE int DemoPlayerPaused()
{
	return gs_pDemoPlayer != nullptr && gs_pDemoPlayer->Paused() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float DemoPlayerProgress()
{
	return gs_pDemoPlayer == nullptr ? 0.0f : gs_pDemoPlayer->Progress();
}

EMSCRIPTEN_KEEPALIVE float DemoPlayerSpeed()
{
	return gs_pDemoPlayer == nullptr ? 1.0f : gs_pDemoPlayer->Speed();
}

EMSCRIPTEN_KEEPALIVE float DemoPlayerLength()
{
	return gs_pDemoPlayer == nullptr ? 0.0f : gs_pDemoPlayer->Length();
}
}
#endif
