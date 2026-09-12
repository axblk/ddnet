/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_viewer_client.h"

#include "session_source_demo.h"
#include "viewer_fullscreen.h"
#include "window_sdl.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonwriter.h>
#include <engine/sound.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cstdlib>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

using namespace std::chrono_literals;

namespace
{
	// How long a held key waits before it starts repeating, and how often it
	// repeats after that. Seeking is the only thing worth holding down.
	constexpr std::chrono::nanoseconds KEY_REPEAT_DELAY = 400ms;
	constexpr std::chrono::nanoseconds KEY_REPEAT_INTERVAL = 60ms;
	// How far one press of a seek key moves.
	constexpr float SEEK_SECONDS = 5.0f;
	// How much of the world one notch of the wheel or one press of a zoom key
	// adds or takes away. The same step the client zooms in.
	constexpr float ZOOM_STEP = 1.0f;

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// The one viewer there is while the page is open, so that the controls
	// beside the canvas have something to talk to. Nothing else ever runs two.
	CDemoViewerClient *g_pDemoViewer = nullptr;
#endif

	int ControlKeyCode(CDemoViewerClient::EControlKey ControlKey)
	{
		switch(ControlKey)
		{
		case CDemoViewerClient::CONTROL_KEY_PAUSE: return KEY_SPACE;
		case CDemoViewerClient::CONTROL_KEY_SEEK_BACK: return KEY_LEFT;
		case CDemoViewerClient::CONTROL_KEY_SEEK_FORWARD: return KEY_RIGHT;
		case CDemoViewerClient::CONTROL_KEY_SPEED_UP: return KEY_UP;
		case CDemoViewerClient::CONTROL_KEY_SPEED_DOWN: return KEY_DOWN;
		case CDemoViewerClient::CONTROL_KEY_RESTART: return KEY_HOME;
		case CDemoViewerClient::CONTROL_KEY_FREE_VIEW: return KEY_F;
		case CDemoViewerClient::CONTROL_KEY_SPECTATE_NEXT: return KEY_N;
		case CDemoViewerClient::CONTROL_KEY_SPECTATE_PREVIOUS: return KEY_P;
		case CDemoViewerClient::CONTROL_KEY_ZOOM_IN: return KEY_KP_PLUS;
		case CDemoViewerClient::CONTROL_KEY_ZOOM_OUT: return KEY_KP_MINUS;
		case CDemoViewerClient::CONTROL_KEY_QUIT: return KEY_ESCAPE;
		default: dbg_assert_failed("Invalid control key");
		}
	}
} // namespace

CDemoViewerClient::CDemoViewerClient()
{
	// A second way through the same demo, for the export to walk at its own
	// pace. It is made here rather than when one is asked for, because the
	// sessions a client has are the sessions it started with.
	auto pExportSource = std::make_unique<CDemoSessionSource>(true, [this](CDemoPlayer &DemoPlayer) { UpdateDemoIntraTimers(DemoPlayer); });
	CDemoSessionSource *pSource = pExportSource.get();
	m_ExportSessionId = m_SessionManager.Create(std::move(pExportSource));
	pSource->SetLifecycleCallbacks(
		[this]() { UpdateDemoSession(m_ExportSessionId); },
		[this](const char *pReason) { StopDemoSession(m_ExportSessionId, pReason); });
}

void CDemoViewerClient::Configure(const char *pDemoPath, const char *pVideoPath, const CVideoExportSettings &Settings)
{
	str_copy(m_aDemoPath, pDemoPath);
	str_copy(m_aVideoPath, pVideoPath == nullptr ? "" : pVideoPath);
	m_Settings = Settings;
}

void CDemoViewerClient::SetPaused(bool Paused)
{
	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	if(Paused)
		Player.Pause();
	else
		Player.Unpause();
}

void CDemoViewerClient::SeekPercent(float Percent)
{
	DemoSource(m_DemoSessionId).DemoPlayer().SeekPercent(std::clamp(Percent, 0.0f, 1.0f));
}

void CDemoViewerClient::SeekTime(float Seconds)
{
	DemoSource(m_DemoSessionId).DemoPlayer().SeekTime(Seconds);
}

void CDemoViewerClient::SeekStart()
{
	DemoSource(m_DemoSessionId).DemoPlayer().SeekPercent(0.0f);
}

void CDemoViewerClient::SetSpeed(float Speed)
{
	DemoSource(m_DemoSessionId).DemoPlayer().SetSpeed(std::clamp(Speed, 0.01f, 100.0f));
}

bool CDemoViewerClient::RequestExport(const CVideoExportSettings &Settings)
{
	if(m_pVideo != nullptr || m_ExportRequested)
	{
		return false;
	}
	m_RequestedSettings = Settings;
	m_ExportRequested = true;
	m_ExportState = EExportState::RUNNING;
	m_ExportStartTime = time_get_nanoseconds();
	m_aExportError[0] = '\0';
	return true;
}

bool CDemoViewerClient::StartExport(const CVideoExportSettings &Settings)
{
	if(m_pVideo != nullptr)
	{
		return false;
	}
	m_ExportState = EExportState::RUNNING;
	m_aExportError[0] = '\0';
	m_Settings = Settings;
	// An encoder takes whole macroblocks, so an odd size is refused rather than
	// rounded. A window is any size the user dragged it to, so the size that
	// comes from one is brought to an even one here instead of being turned
	// away.
	m_Settings.m_Width = std::clamp(Settings.m_Width, 2, 8192) & ~1;
	m_Settings.m_Height = std::clamp(Settings.m_Height, 2, 8192) & ~1;
	m_Settings.m_FPS = std::clamp(Settings.m_FPS, 1, 240);
	m_Settings.m_Crf = std::clamp(Settings.m_Crf, 0, 51);
	// The name of the demo, so that whoever ends up with the file knows what it
	// is a video of. It is written where the user's own files go and handed to
	// the browser from there.
	char aName[IO_MAX_PATH_LENGTH];
	fs_split_file_extension(fs_filename(m_aDemoPath), aName, sizeof(aName));
	str_format(m_aVideoPath, sizeof(m_aVideoPath), "videos/%s.mp4", aName);
	// The export reads the demo out of a session of its own. What whoever
	// asked for it does next - seek away, pause, watch somebody else - is
	// theirs and no longer the video's.
	m_VideoSessionId = m_ExportSessionId;
	// An export that ended with its demo left the way through it open. It is
	// closed here rather than there, so that what was written stays readable
	// until somebody asks for the next one.
	if(SessionState(m_ExportSessionId) != ESessionState::OFFLINE)
		StopDemoSession(m_ExportSessionId, "");
	// The whole demo, from its first tick, wherever the one being watched has
	// got to. A demo that has played out sits on its last frame - which is
	// where somebody who has just watched it and then asks for a video of it
	// is standing, and starting there would write them a video one frame long.
	const char *pError = PlayDemo(m_ExportSessionId);
	if(pError == nullptr)
		pError = StartVideo();
	if(pError != nullptr)
	{
		log_error("videorecorder", "%s", pError);
		str_copy(m_aExportError, pError);
		m_aError[0] = '\0';
		m_aVideoPath[0] = '\0';
		m_ExportState = EExportState::FAILED;
		StopDemoSession(m_ExportSessionId, "");
		m_VideoSessionId = m_DemoSessionId;
		return false;
	}
	return true;
}

void CDemoViewerClient::CancelExport()
{
	if(m_pVideo == nullptr)
	{
		return;
	}
	if(IVideo::Current() == m_pVideo.get())
	{
		m_pVideo->Cancel();
	}
	// The way through the demo that was being written is closed; the one being
	// watched was never touched. It is closed before the encoder is let go,
	// because a demo that is being written to a video holds the encoder and
	// hands it its last frames as it stops.
	StopDemoSession(m_ExportSessionId, "");
	m_pVideo.reset();
	m_aVideoPath[0] = '\0';
	m_ExportState = EExportState::IDLE;
	m_VideoSessionId = m_DemoSessionId;
}

void CDemoViewerClient::FinishExport()
{
	// The demo player has closed it already, which is what wrote the last of
	// it out and handed it over. Anything else is closed here.
	if(IVideo::Current() == m_pVideo.get())
	{
		m_pVideo->Stop();
	}
	const CVideoExportStatus Status = m_pVideo->Status();
	const bool Failed = Status.m_HasError;
	m_pVideo.reset();
	if(Failed)
	{
		str_copy(m_aExportError, Status.m_aError[0] == '\0' ? "The video could not be written." : Status.m_aError);
		m_aError[0] = '\0';
	}
	log_info("videorecorder", Failed ? "Export failed" : "Export completed");
	m_aVideoPath[0] = '\0';
	m_ExportState = Failed ? EExportState::FAILED : EExportState::FINISHED;
	// Writing the demo to a file played it to its end, which ended the session
	// it was played from. That was the export's own way through the demo; the
	// one being watched carried on the whole time and is where it was.
	m_VideoSessionId = m_DemoSessionId;
}

float CDemoViewerClient::ExportProgress() const
{
	int First, Current, Last;
	if(!DemoPlayer_RenderInfo(&First, &Current, &Last))
		return 0.0f;
	const int Total = std::max(Last - First, 0);
	return Total == 0 ? 0.0f : std::clamp(Current - First, 0, Total) / (float)Total;
}

float CDemoViewerClient::ExportSecondsLeft() const
{
	if(m_pVideo == nullptr)
		return -1.0f;
	// How long it has taken to get this far, and how much further it has to
	// go. The same sum the client makes for its own progress box, and for the
	// same reason: the rate the encoder reports is the rate of the moment and
	// jumps about, while this settles as the export runs.
	const float Progress = ExportProgress();
	const float Elapsed = std::chrono::duration<float>(time_get_nanoseconds() - m_ExportStartTime).count();
	if(Elapsed < 1.0f || Progress < 0.01f)
		return -1.0f;
	return Elapsed * (1.0f - Progress) / Progress;
}

bool CDemoViewerClient::Paused() const
{
	return DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_Paused;
}

float CDemoViewerClient::Progress() const
{
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
	const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
	if(TotalTicks == 0)
		return 0.0f;
	return std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks) / (float)TotalTicks;
}

float CDemoViewerClient::Speed() const
{
	return DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_Speed;
}

float CDemoViewerClient::Length() const
{
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
	return std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0) / (float)SERVER_TICK_SPEED;
}

bool CDemoViewerClient::Exporting() const
{
	return m_pVideo != nullptr && IVideo::Current() == m_pVideo.get();
}

void CDemoViewerClient::UpdateAndSwap()
{
	Graphics()->Swap();
}

void CDemoViewerClient::DemoPlayer_CancelActiveRender()
{
	// Asked for while something is being drawn, and letting the encoder go
	// there would pull it out from under the frame that is asking. The loop
	// does it between frames instead.
	RequestCancelExport();
}

bool CDemoViewerClient::KeyPressed(EControlKey ControlKey, bool Repeats)
{
	const bool Pressed = Input()->KeyIsPressed(ControlKeyCode(ControlKey));
	const bool WasPressed = m_aKeyWasPressed[ControlKey];
	m_aKeyWasPressed[ControlKey] = Pressed;
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Pressed && !WasPressed)
	{
		m_aKeyRepeatTime[ControlKey] = Now + KEY_REPEAT_DELAY;
		return true;
	}
	if(Pressed && Repeats && Now >= m_aKeyRepeatTime[ControlKey])
	{
		m_aKeyRepeatTime[ControlKey] = Now + KEY_REPEAT_INTERVAL;
		return true;
	}
	return false;
}

bool CDemoViewerClient::HandleInput()
{
	// Nobody is at the keyboard of a window that is not there.
	if(m_pInput == nullptr)
	{
		return true;
	}
	if(Input()->Update())
	{
		return false;
	}

	char aDroppedFile[IO_MAX_PATH_LENGTH];
	if(Input()->GetDropFile(aDroppedFile, sizeof(aDroppedFile)))
	{
		// Whatever is playing gives way to what was just dropped, and a viewer
		// that was waiting for a demo has one now.
		if(SessionState(m_DemoSessionId) == ESessionState::READY)
		{
			StopDemoSession(nullptr);
		}
		m_aError[0] = '\0';
		const char *pLoadError = DemoPlayer_Play(aDroppedFile, IStorage::TYPE_ALL_OR_ABSOLUTE);
		if(pLoadError != nullptr)
		{
			log_error("client", "%s", pLoadError);
			m_aError[0] = '\0';
		}
	}

	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	if(KeyPressed(CONTROL_KEY_QUIT, false))
	{
		return false;
	}
	if(KeyPressed(CONTROL_KEY_PAUSE, false))
	{
		if(Player.BaseInfo()->m_Paused)
			Player.Unpause();
		else
			Player.Pause();
	}
	if(KeyPressed(CONTROL_KEY_SEEK_BACK, true))
	{
		Player.SeekTime(-SEEK_SECONDS);
	}
	if(KeyPressed(CONTROL_KEY_SEEK_FORWARD, true))
	{
		Player.SeekTime(SEEK_SECONDS);
	}
	if(KeyPressed(CONTROL_KEY_SPEED_UP, false))
	{
		Player.AdjustSpeedIndex(1);
	}
	if(KeyPressed(CONTROL_KEY_SPEED_DOWN, false))
	{
		Player.AdjustSpeedIndex(-1);
	}
	if(KeyPressed(CONTROL_KEY_RESTART, false))
	{
		Player.SeekPercent(0.0f);
	}
	if(KeyPressed(CONTROL_KEY_FREE_VIEW, false))
	{
		// Back to whoever the demo was recorded by when there is one, so that
		// the same key both leaves a player and comes back to them.
		SetSpectate(Spectating() == SPEC_FREEVIEW ? SPEC_FOLLOW : SPEC_FREEVIEW);
	}
	if(KeyPressed(CONTROL_KEY_SPECTATE_NEXT, false))
	{
		SpectateStep(1);
	}
	if(KeyPressed(CONTROL_KEY_SPECTATE_PREVIOUS, false))
	{
		SpectateStep(-1);
	}
	if(KeyPressed(CONTROL_KEY_ZOOM_IN, true) || Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
	{
		ScaleZoom(CCamera::ZoomStepsToValue(ZOOM_STEP));
	}
	if(KeyPressed(CONTROL_KEY_ZOOM_OUT, true) || Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
	{
		ScaleZoom(CCamera::ZoomStepsToValue(-ZOOM_STEP));
	}

	// Two fingers pinch the demo closer or further away and drag it about, the
	// way every picture on a touch screen is handled.
	const CViewerGestures::SResult Gesture = m_Gestures.Update(Input()->TouchFingerStates(), vec2(Graphics()->ScreenWidth(), Graphics()->ScreenHeight()));
	if(Gesture.m_Active)
	{
		ScaleZoom(Gesture.m_Zoom);
		MoveFreeView(-Gesture.m_Move * WorldPerPixel());
		m_Controls.Show();
	}

	// Dragging moves the free view, the way a map is dragged. In the pixels
	// that are drawn, not the ones the window is measured in, so that on a
	// screen with more of the former the world keeps up with the pointer. A
	// press that landed on the bar belongs to the bar, and one that is part of
	// a pinch belongs to the pinch.
	const vec2 MousePos = Input()->NativeMousePos() * Graphics()->ScreenHiDPIScale();
	if(Input()->NativeMousePressed(1) && !m_Controls.Hovered() && !Gesture.m_Active)
	{
		if(m_Dragging)
		{
			MoveFreeView((m_LastMousePos - MousePos) * WorldPerPixel());
		}
		m_Dragging = true;
	}
	else
	{
		m_Dragging = false;
	}
	m_LastMousePos = MousePos;

	// Everything this frame brought has been read. Nothing carries over to the
	// next one, and the events pile up until they are let go of.
	Input()->Clear();
	return true;
}

const char *CDemoViewerClient::Players()
{
	// Written by the JSON writer rather than put together by hand: a name is
	// whatever somebody typed, quotation marks and all.
	CJsonStringWriter Writer;
	Writer.BeginArray();
	// Who there is to pick from, which is nobody in a demo a client recorded:
	// that demo is of whoever recorded it, and all there is to choose is
	// whether to look over their shoulder or to look around. The same rule the
	// bar draws itself by, see `RenderControls`.
	if(ServerDemo())
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			const char *pName = SpectatePlayerName(ClientId);
			if(pName == nullptr)
			{
				continue;
			}
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

void CDemoViewerClient::RenderControls()
{
	// Nothing to steer, and nobody to steer it: a window that was never opened
	// has no pointer over it, and a demo that is being written to a file is
	// not being watched.
	if(!m_ShowControls || m_pInput == nullptr || SessionState(m_DemoSessionId) != ESessionState::READY)
	{
		return;
	}
	const float Total = Length();
	if(Total <= 0.0f)
	{
		return;
	}

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
	// Who is being watched, cut short: a bar as wide as the longest name
	// somebody chose is a bar that covers the demo.
	char aSpectating[20];
	const char *pSpectating = SpectatePlayerName(Spectating());
	if(pSpectating != nullptr)
		str_copy(aSpectating, pSpectating);
	else
		str_copy(aSpectating, Spectating() == SPEC_FOLLOW ? "Follow" : "Free view");

	char aFps[16];
	str_format(aFps, sizeof(aFps), "%d fps", m_ExportFps);
	// While one is being written the export button says how far it has come
	// and stops it, because that is all there is to do about it then.
	const bool IsExporting = Exporting();
	char aExportProgress[32];
	{
		const float SecondsLeft = ExportSecondsLeft();
		char aLeft[16] = "";
		if(SecondsLeft >= 0.0f)
			str_format(aLeft, sizeof(aLeft), " %d:%02d", (int)SecondsLeft / 60, (int)SecondsLeft % 60);
		str_format(aExportProgress, sizeof(aExportProgress), "%d%%%s", (int)(ExportProgress() * 100.0f + 0.5f), aLeft);
	}

	// Left to right, the way a video player has it: what it is doing, how fast,
	// how far along, and off on the other side what is being watched and what
	// to make a video of.
	enum
	{
		ITEM_SEEK,
		ITEM_PLAY,
		ITEM_RESTART,
		ITEM_SLOWER,
		ITEM_SPEED,
		ITEM_FASTER,
		ITEM_TIME,
		ITEM_SPACER,
		ITEM_SPECTATE,
		ITEM_EXPORT,
		ITEM_FULLSCREEN,
		// What the export menu offers: two things to set, and three sizes to
		// ask for - picking a size is what starts it.
		ITEM_EXPORT_SOUND,
		ITEM_EXPORT_FPS,
		ITEM_EXPORT_AS_SHOWN,
		ITEM_EXPORT_720,
		ITEM_EXPORT_1080,
		NUM_ITEMS,
	};
	// The menu of the export button, told apart from the one the eye opens.
	constexpr int MenuExport = 1;
	// The players to pick from, and what picking one means. A demo a client
	// recorded is a demo of whoever recorded it, and the button then only
	// says whether to look over their shoulder or to look around: the list of
	// everybody who happened to be on the server belongs to a demo the server
	// recorded, where there is nobody whose demo it is.
	std::vector<int> vPickable;
	if(ServerDemo())
	{
		vPickable.push_back(SPEC_FREEVIEW);
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(SpectatePlayerName(ClientId) != nullptr)
				vPickable.push_back(ClientId);
		}
	}

	std::vector<CViewerControls::SItem> vItems(NUM_ITEMS + vPickable.size());
	// A browser without an encoder cannot make a video, and says so before
	// anything is asked of it.
	const bool CanExport = VideoEncodingSupported();
	CViewerControls::SItem *aItems = vItems.data();
	aItems[ITEM_SEEK].m_Type = CViewerControls::EItem::SLIDER;
	aItems[ITEM_SEEK].m_Value = Progress();
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
	aItems[ITEM_SPACER].m_Type = CViewerControls::EItem::SPACER;
	aItems[ITEM_SPECTATE].m_Icon = CViewerControls::EIcon::EYE;
	aItems[ITEM_SPECTATE].m_pText = aSpectating;
	aItems[ITEM_SPECTATE].m_OpensMenu = !vPickable.empty();
	aItems[ITEM_EXPORT].m_Icon = IsExporting ? CViewerControls::EIcon::STOP : CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT].m_pText = IsExporting ? aExportProgress : nullptr;
	aItems[ITEM_EXPORT].m_OpensMenu = !IsExporting;
	aItems[ITEM_EXPORT].m_MenuId = MenuExport;
	aItems[ITEM_EXPORT].m_Hidden = !CanExport;
	aItems[ITEM_FULLSCREEN].m_Icon = CViewerControls::EIcon::FULLSCREEN;
	aItems[ITEM_FULLSCREEN].m_Active = ViewerFullscreen::Active(Window());
	aItems[ITEM_FULLSCREEN].m_Hidden = !ViewerFullscreen::Supported(Window());
	aItems[ITEM_EXPORT_SOUND].m_pText = "Sound";
	aItems[ITEM_EXPORT_SOUND].m_Active = m_ExportAudio;
	aItems[ITEM_EXPORT_SOUND].m_KeepsMenu = true;
	aItems[ITEM_EXPORT_FPS].m_pText = aFps;
	aItems[ITEM_EXPORT_FPS].m_KeepsMenu = true;
	aItems[ITEM_EXPORT_AS_SHOWN].m_Icon = CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT_AS_SHOWN].m_pText = "As shown";
	aItems[ITEM_EXPORT_720].m_Icon = CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT_720].m_pText = "1280 x 720";
	aItems[ITEM_EXPORT_1080].m_Icon = CViewerControls::EIcon::SAVE;
	aItems[ITEM_EXPORT_1080].m_pText = "1920 x 1080";
	for(int i = ITEM_EXPORT_SOUND; i <= ITEM_EXPORT_1080; ++i)
	{
		aItems[i].m_InMenu = true;
		aItems[i].m_MenuId = MenuExport;
		aItems[i].m_Hidden = !CanExport || IsExporting;
	}
	for(size_t i = 0; i < vPickable.size(); ++i)
	{
		CViewerControls::SItem &Item = aItems[NUM_ITEMS + i];
		Item.m_InMenu = true;
		Item.m_Active = vPickable[i] == Spectating();
		if(vPickable[i] == SPEC_FREEVIEW)
		{
			Item.m_Icon = CViewerControls::EIcon::FREEVIEW;
			Item.m_pText = "Free view";
		}
		else
		{
			// The name the demo holds, not a copy of it: it stays where it is
			// for as long as this frame lasts, which is as long as the bar
			// needs it.
			Item.m_Icon = CViewerControls::EIcon::EYE;
			Item.m_pText = SpectatePlayerName(vPickable[i]);
		}
	}

	CViewerControls::SInput Input;
	Input.m_MousePos = m_pInput->NativeMousePos();
	Input.m_MousePressed = m_pInput->NativeMousePressed(1);
	// From the events rather than from the state, because a frame can take
	// longer than a tap does and the state alone would never see it.
	Input.m_MouseClicked = m_pInput->KeyPress(KEY_MOUSE_1);
	Input.m_KeyPressed = std::any_of(m_aKeyWasPressed.begin(), m_aKeyWasPressed.end(), [](bool Pressed) { return Pressed; });

	float SeekTo = 0.0f;
	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	const int Pressed = m_Controls.Render(aItems, vItems.size(), Input, &SeekTo);

	// A demo that goes on playing while somebody drags along the seek bar
	// runs out from under them: every frame moves the place they are looking
	// for further from where they are pointing. It stands still until they let
	// go, and then goes on if it was going on before.
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
		SetSpectate(vPickable[Pressed - NUM_ITEMS]);
		return;
	}

	switch(Pressed)
	{
	case ITEM_PLAY:
		SetPaused(!Paused());
		break;
	case ITEM_SEEK:
		Player.SeekPercent(SeekTo);
		break;
	case ITEM_SLOWER:
		Player.AdjustSpeedIndex(-1);
		break;
	case ITEM_FASTER:
		Player.AdjustSpeedIndex(1);
		break;
	case ITEM_RESTART:
		SeekStart();
		break;
	case ITEM_SPECTATE:
		SpectateStep(1);
		break;
	case ITEM_EXPORT:
		// Only reported while the menu is not what it opens, which is while
		// one is being written.
		RequestCancelExport();
		break;
	case ITEM_FULLSCREEN:
		ViewerFullscreen::Toggle(Window());
		break;
	case ITEM_EXPORT_SOUND:
		m_ExportAudio = !m_ExportAudio;
		break;
	case ITEM_EXPORT_FPS:
		m_ExportFps = m_ExportFps == 60 ? 30 : 60;
		break;
	case ITEM_EXPORT_AS_SHOWN:
		ExportFromControls(Graphics()->ScreenWidth(), Graphics()->ScreenHeight());
		break;
	case ITEM_EXPORT_720:
		ExportFromControls(1280, 720);
		break;
	case ITEM_EXPORT_1080:
		ExportFromControls(1920, 1080);
		break;
	default:
		break;
	}
}

void CDemoViewerClient::ExportFromControls(int Width, int Height)
{
	CVideoExportSettings Settings;
	// An encoder counts in whole pairs of lines, so a window of an odd height
	// is asked for one line less rather than refused.
	Settings.m_Width = std::max(Width & ~1, 2);
	Settings.m_Height = std::max(Height & ~1, 2);
	Settings.m_FPS = m_ExportFps;
	Settings.m_Audio = m_ExportAudio;
	RequestExport(Settings);
}

void CDemoViewerClient::RenderWindowFrame()
{
	const int64_t Now = time_get();
	m_RenderFrameTime = (Now - m_LastRenderTime) / (float)time_freq();
	m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;
	m_LastRenderTime = Now;

	GameClient()->OnRenderPrepare();
	GameClient()->OnRender();
	GameClient()->OnRenderFinalize();
	// Over everything else, because it is what is in front of the demo, and
	// before the frame goes out.
	RenderControls();
	Graphics()->Swap();
	// The clock everything that moves by itself runs on: the camera easing
	// towards a player it was just put on, and the zoom easing towards what
	// the wheel asked for. Without it they are handed the same instant every
	// frame and never arrive.
	m_LocalTime = (time_get() - m_LocalStartTime) / (float)time_freq();
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}

void CDemoViewerClient::Run()
{
	m_LocalStartTime = m_GlobalStartTime = time_get();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pDemoViewer = this;
#endif

	// The same escape hatch the client has: a machine with no display can still
	// draw, into a surface without a window. It is the only way to try the
	// viewer where there is nothing to show it on, and it is what an export
	// from here wants anyway. Nobody is at the keyboard of a window that is not
	// there, so the keys stay off as well.
	m_Surfaceless = std::getenv("GFX_SURFACELESS") != nullptr;
	// A demo fills the window it was given. The game keeps what it draws
	// within five by four so that nobody sees further by making their window
	// taller than everybody else's; there is nobody to be fair to here, and a
	// telephone held upright is exactly the window that rule would leave two
	// fifths of black.
	g_Config.m_GfxWholeWindow = 1;
	if(!InitGraphics(m_Surfaceless ? CreateOffscreenGraphicsWindow() : CreateSdlGraphicsWindow()))
	{
		m_ExitCode = 1;
		return;
	}

	if(!m_Surfaceless)
	{
		m_pInput = Kernel()->RequestInterface<IEngineInput>();
		m_pInput->Init();
		// Nobody aims here. The input takes the pointer when it starts, because
		// a game wants it; a viewer wants it left where it is, so that it can be
		// put on a button and taken out of the window again.
		m_pInput->MouseModeAbsolute();
	}
	GameClient()->InitializeLanguage();
	if(Sound()->Init() != 0)
	{
		log_warn("client", "The audio device could not be initialised, watching without sound.");
	}
	InitVideoBackend();

	InitTextRender();
	m_Controls.Init(Graphics(), TextRender());
	Graphics()->AddWindowResizeListener([this] { OnWindowResize(); });
	GameClient()->OnInit();

	// A viewer opens with whatever it was given, which may be nothing: there it
	// waits for a demo to be dropped on it, the way the map viewer waits for a
	// map. A surface without a window has nobody to drop one, so there a demo is
	// the only reason to be running at all.
	const char *pError = nullptr;
	if(m_aDemoPath[0] != '\0')
	{
		pError = PlayDemo();
		if(pError == nullptr && m_aVideoPath[0] != '\0')
		{
			pError = StartVideo();
		}
	}
	else if(m_Surfaceless)
	{
		pError = "No demo was given, and a surface without a window has nowhere to drop one.";
	}
	if(pError != nullptr)
	{
		log_error("client", "%s", pError);
		m_ExitCode = 1;
	}
	else
	{
		while(m_State != IClient::STATE_QUITTING)
		{
			if(!HandleInput())
			{
				break;
			}
			// What a page asked for, done here where the stack belongs to the
			// viewer: both of these wait for the browser, and what waits gets
			// its stack unwound underneath it.
			if(m_CancelRequested)
			{
				m_CancelRequested = false;
				m_ExportRequested = false;
				CancelExport();
			}
			if(m_ExportRequested)
			{
				m_ExportRequested = false;
				StartExport(m_RequestedSettings);
			}
			// A demo that has run out pauses on its last frame, which is what
			// somebody watching wants: they can seek back into it. Nobody
			// watches a surface without a window, so there it is the end.
			if(m_Surfaceless && DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_CurrentTick >= DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_LastTick)
			{
				break;
			}
			set_new_tick();
			m_SessionManager.Update();
			// A player asked for by name is only found once the demo has named
			// them, which is a snapshot or two in.
			UpdatePendingSpectate();
			Sound()->Update();
			GameClient()->OnUpdate();
			// An export takes every frame in its own time and may run as fast
			// as the machine can write them. The window is drawn in between,
			// often enough to stay usable and no more often than that: every
			// picture drawn for whoever is watching is a picture the export
			// does not encode.
			if(m_pVideo != nullptr && IVideo::Current() == m_pVideo.get())
			{
				RenderExportFrame();
				const std::chrono::nanoseconds Now = time_get_nanoseconds();
				constexpr std::chrono::nanoseconds ScreenInterval = std::chrono::nanoseconds(std::chrono::seconds(1)) / 30;
				// Only where there is a window and the export is not the demo
				// on it: an export asked for on the command line is the whole
				// job, and drawing it twice is time taken from it.
				const bool Watching = m_pInput != nullptr && m_VideoSessionId != m_DemoSessionId;
				if(Watching && Now - m_LastExportScreenRender >= ScreenInterval)
				{
					m_LastExportScreenRender = Now;
					// The window measures its own clock: what is on it moves at
					// the speed it is shown at, not at the speed frames are
					// encoded.
					const int64_t ExportRenderTime = m_LastRenderTime;
					m_LastRenderTime = m_LastWindowRenderTime == 0 ? ExportRenderTime : m_LastWindowRenderTime;
					RenderWindowFrame();
					m_LastWindowRenderTime = m_LastRenderTime;
					m_LastRenderTime = ExportRenderTime;
				}
			}
			else if(m_pVideo != nullptr)
			{
				// The demo player closes the video itself when the demo runs
				// out under it, which is what says the export is complete:
				// what was being written is no longer the video being written.
				FinishExport();
			}
			else
			{
				RenderWindowFrame();
				thread_sleep_until_next_frame(m_NextFrameTime, g_Config.m_ClRefreshRate);
			}
		}
		if(m_aError[0] != '\0')
		{
			log_error("client", "%s", m_aError);
			m_ExitCode = 1;
		}
	}

	SetState(IClient::STATE_QUITTING);
	// The demo an export was reading from goes first, because it hands the
	// encoder its last frames as it stops and must not be the one holding it
	// once it is gone.
	if(SessionState(m_ExportSessionId) != ESessionState::OFFLINE)
	{
		StopDemoSession(m_ExportSessionId, "");
	}
	if(m_pVideo != nullptr)
	{
		if(IVideo::Current() == m_pVideo.get())
			m_pVideo->Stop();
		m_pVideo.reset();
	}
	if(SessionState(m_DemoSessionId) != ESessionState::OFFLINE)
	{
		m_SessionManager.Close(m_DemoSessionId);
		m_SessionManager.Update(m_DemoSessionId);
	}
	// The jobs run on their own threads and load assets into the graphics and
	// the sound, so none of the two may be shut down while one is still going.
	Engine()->ShutdownJobs();
	GameClient()->OnShutdown();
	// The text render gives its textures back, which needs the graphics.
	TextRender()->Shutdown();
	Graphics()->Shutdown();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pDemoViewer = nullptr;
#endif
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// What the controls beside the canvas call. A browser shows no key bindings
// and nobody would find them, so the page has buttons and a slider and these
// are the other end of them. Every one of them is safe to call before the
// demo is playing or after it has stopped.
extern "C" {

EMSCRIPTEN_KEEPALIVE void DemoViewerSetPaused(int Paused)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetPaused(Paused != 0);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekPercent(float Percent)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SeekPercent(Percent);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekTime(float Seconds)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SeekTime(Seconds);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpeed(float Speed)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetSpeed(Speed);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekStart()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SeekStart();
}

// Who the demo is watched over the shoulder of. -1 is the free view, -2 is
// whoever recorded it, and everything from 0 up is one of its players.
EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpectate(int SpectatorId)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetSpectate(SpectatorId);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpectateName(const char *pName)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetSpectateName(pName == nullptr ? "" : pName);
}

EMSCRIPTEN_KEEPALIVE int DemoViewerSpectating()
{
	return g_pDemoViewer == nullptr ? SPEC_FREEVIEW : g_pDemoViewer->Spectating();
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSpectateStep(int Direction)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SpectateStep(Direction);
}

// The players the demo has named so far, as JSON. What is pointed at stays
// there until this is called again, which is all a page reading it out needs.
EMSCRIPTEN_KEEPALIVE const char *DemoViewerPlayers()
{
	return g_pDemoViewer == nullptr ? "[]" : g_pDemoViewer->Players();
}

// How much of the world is in the window. A page has no wheel over the canvas
// while the pointer is on a button of its own, so it can ask for this instead.
EMSCRIPTEN_KEEPALIVE void DemoViewerZoomBy(float Factor)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->ScaleZoom(Factor);
}

EMSCRIPTEN_KEEPALIVE float DemoViewerZoom()
{
	return g_pDemoViewer == nullptr ? 1.0f : g_pDemoViewer->Zoom();
}

EMSCRIPTEN_KEEPALIVE void DemoViewerQuit()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->Quit();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerStartExport(int Width, int Height, int Fps, int Audio, int Crf, const char *pCodec, int Hud, int Chat)
{
	if(g_pDemoViewer == nullptr)
	{
		return 0;
	}
	// This only asks. Whether it worked is what `DemoViewerExportState` says,
	// a moment later - a browser is asked for an encoder before there is an
	// answer, and asking it takes a turn of its event loop.
	CVideoExportSettings Settings;
	Settings.m_Width = Width;
	Settings.m_Height = Height;
	Settings.m_FPS = Fps;
	Settings.m_Audio = Audio != 0;
	Settings.m_Crf = Crf;
	str_copy(Settings.m_aVideoCodec, pCodec == nullptr ? "" : pCodec);
	Settings.m_ShowHud = Hud != 0;
	Settings.m_ShowChat = Chat != 0;
	return g_pDemoViewer->RequestExport(Settings) ? 1 : 0;
}

// Whether the viewer draws its own controls. A page with a bar of its own
// beside the canvas says so and gets a bare picture.
EMSCRIPTEN_KEEPALIVE void DemoViewerSetControls(int Show)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetShowControls(Show != 0);
}

EMSCRIPTEN_KEEPALIVE int DemoViewerControls()
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->ShowControls() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoViewerCancelExport()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->RequestCancelExport();
}

// 0 while nothing was ever asked for, 1 while a video is being written, 2 when
// the last one was handed over and 3 when it failed.
EMSCRIPTEN_KEEPALIVE const char *DemoViewerExportError()
{
	return g_pDemoViewer == nullptr ? "" : g_pDemoViewer->ExportError();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerExportState()
{
	return g_pDemoViewer == nullptr ? 0 : (int)g_pDemoViewer->ExportState();
}

// How far the video that is being written has got, between 0 and 1. It is not
// where the demo on the window is: an export reads the demo through a way of
// its own, so whoever is watching can spool about while it is written.
EMSCRIPTEN_KEEPALIVE float DemoViewerExportSecondsLeft()
{
	return g_pDemoViewer == nullptr ? -1.0f : g_pDemoViewer->ExportSecondsLeft();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerExportProgress()
{
	return g_pDemoViewer == nullptr ? 0.0f : g_pDemoViewer->ExportProgress();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerPaused()
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->Paused() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float DemoViewerProgress()
{
	return g_pDemoViewer == nullptr ? 0.0f : g_pDemoViewer->Progress();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerSpeed()
{
	return g_pDemoViewer == nullptr ? 1.0f : g_pDemoViewer->Speed();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerLength()
{
	return g_pDemoViewer == nullptr ? 0.0f : g_pDemoViewer->Length();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerExporting()
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->Exporting() ? 1 : 0;
}
}
#endif
