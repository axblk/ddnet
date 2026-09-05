#include "window_sdl.h"

#include <base/detect.h>

#ifndef CONF_BACKEND_OPENGL_ES
#include <GL/glew.h>
#endif

#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/backend/vulkan/backend_vulkan.h>
#include <engine/client/graphics_backend.h>
#include <engine/client/presentation_surface.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/localization.h>

#if defined(CONF_VIDEORECORDER)
#include <engine/shared/video.h>
#endif

#include <SDL.h>
#include <SDL_messagebox.h>
#include <SDL_video.h>
#if defined(CONF_PLATFORM_IOS)
#include <ios/ios_main.h>
#endif
#if defined(CONF_BACKEND_VULKAN)
#include <SDL_vulkan.h>
#include <vulkan/vulkan_core.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <vector>

// The modes gfx_display_all_video_modes offers instead of asking the
// screen; the window sizes are filled in from the DPI scale once known.
static CVideoMode g_aFakeModes[] = {
	{8192, 4320, 8192, 4320, 0}, {7680, 4320, 7680, 4320, 0}, {5120, 2880, 5120, 2880, 0},
	{4096, 2160, 4096, 2160, 0}, {3840, 2160, 3840, 2160, 0}, {2560, 1440, 2560, 1440, 0},
	{2048, 1536, 2048, 1536, 0}, {1920, 2400, 1920, 2400, 0}, {1920, 1440, 1920, 1440, 0},
	{1920, 1200, 1920, 1200, 0}, {1920, 1080, 1920, 1080, 0}, {1856, 1392, 1856, 1392, 0},
	{1800, 1440, 1800, 1440, 0}, {1792, 1344, 1792, 1344, 0}, {1680, 1050, 1680, 1050, 0},
	{1600, 1200, 1600, 1200, 0}, {1600, 1000, 1600, 1000, 0}, {1440, 1050, 1440, 1050, 0},
	{1440, 900, 1440, 900, 0}, {1400, 1050, 1400, 1050, 0}, {1368, 768, 1368, 768, 0},
	{1280, 1024, 1280, 1024, 0}, {1280, 960, 1280, 960, 0}, {1280, 800, 1280, 800, 0},
	{1280, 768, 1280, 768, 0}, {1152, 864, 1152, 864, 0}, {1024, 768, 1024, 768, 0},
	{1024, 600, 1024, 600, 0}, {800, 600, 800, 600, 0}, {768, 576, 768, 576, 0},
	{720, 400, 720, 400, 0}, {640, 480, 640, 480, 0}, {400, 300, 400, 300, 0},
	{320, 240, 320, 240, 0}};

class CGraphicsWindow_SDL : public IEngineGraphicsWindow, public IPresentationSurface
{
	SDL_Window *m_pWindow = nullptr;
	SDL_GLContext m_GLContext = nullptr;
	EBackendType m_BackendType = BACKEND_TYPE_AUTO;
	EBackendType m_BackendOverride;
	int m_NumScreens = 0;
	bool m_Hidden = false;
	SGraphicsSurfaceInfo m_Surface;
	ivec2 m_DesktopSize = ivec2(0, 0);
	float m_HiDPIScale = 1.0f;
	IEngineGraphics *m_pGraphics = nullptr;
	std::vector<WINDOW_PROPS_CHANGED_FUNC> m_vPropChangeListeners;

	IEngineGraphics *Graphics();
	EBackendType DetectBackend() const;
	static void ClampDriverVersion(EBackendType BackendType);
	int OpenWindow(SGraphicsBackendInit &BackendInit);
	int TryOpen(IGraphicsBackend **ppBackend);
	void DestroyWindow();
	void DestroyContextAndWindow();
	void GotResized(int w, int h, int RefreshRate);
	void NotifyPropChange();
	void SetWindowParamsImpl(int FullscreenMode, bool IsBorderless);
	bool SetWindowScreenImpl(int Index, bool MoveToCenter, ivec2 *pDesktopSize);
	bool UpdateDisplayMode(int Index, ivec2 *pDesktopSize);
	bool ResizeWindow(int w, int h, int RefreshRate);
	void GetViewportSize(int &w, int &h);
	void ReadDisplayCutout();
	void GetVideoModesImpl(CVideoMode *pModes, int MaxModes, int *pNumModes, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId);
	void GetCurrentVideoModeImpl(CVideoMode &CurMode, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId);

public:
	void Update() override;
	CGraphicsWindow_SDL();

	// IGraphicsWindow
	void SetWindowParams(int FullscreenMode, bool IsBorderless) override;
	bool SetWindowScreen(int Index, bool MoveToCenter) override;
	bool SwitchWindowScreen(int Index, bool MoveToCenter) override;
	int GetWindowScreen() override;
	int GetNumScreens() const override { return m_NumScreens; }
	const char *GetScreenName(int ScreenIndex) const override;
	bool SetVSync(bool State) override;
	bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) override;
	void Move(int x, int y) override;
	bool Resize(int w, int h, int RefreshRate) override;
	void ResizeToScreen() override;
	bool IsScreenKeyboardShown() override;
	int GetVideoModes(CVideoMode *pModes, int MaxModes, int ScreenIndex) override;
	void GetCurrentVideoMode(CVideoMode &CurMode, int ScreenIndex) override;
	void SetWindowGrab(bool Grab) override;
	void NotifyWindow() override;
	void Minimize() override;
	int WindowActive() override;
	int WindowOpen() override;
	void AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc) override;
	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override;

	// IEngineGraphicsWindow
	IGraphicsBackend *Open(bool Hidden) override;
	const SGraphicsSurfaceInfo &Surface() const override { return m_Surface; }
	void Close() override;
	void OnMoved(int x, int y) override;
	void OnSizeChanged(int w, int h) override;
	void OnDisplayChanged(int DisplayIndex) override;
	void OnWindowDestroyed() override;
	void OnWindowCreated(uint32_t WindowId) override;

	// IPresentationSurface, called on the render thread
	void DrawableSize(int &Width, int &Height) const override;
	bool BindGlContext() override;
	void UnbindGlContext() override;
	void SwapGlBuffers() override;
	bool SetGlSwapInterval(bool VSync) override;
	bool VulkanInstanceExtensions(std::vector<std::string> &vExtensions) override;
	bool CreateVulkanSurface(const void *pInstance, void *pSurface) override;
};

#if !defined(CONF_HEADLESS_CLIENT)
static bool BackendInitGlew(EBackendType BackendType, int &GlewMajor, int &GlewMinor, int &GlewPatch)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
#if !defined(CONF_BACKEND_OPENGL_ES)
		// Support graphic cards that are pretty old (and Linux)
		glewExperimental = GL_TRUE;
#ifdef CONF_GLEW_HAS_CONTEXT_INIT
		const GLenum InitResult = glewContextInit();
		if(InitResult != GLEW_OK)
		{
			log_error("gfx", "Unable to init glew (glewContextInit): %s", glewGetErrorString(InitResult));
			return false;
		}
#else
		const GLenum InitResult = glewInit();
		if(InitResult != GLEW_OK)
		{
			// A context that came from EGL has no GLX display for glewInit to
			// look at, and says so instead of failing. It has initialized the
			// context internally by then. Wayland is the common way to get
			// there, but kmsdrm and the offscreen driver arrive the same way,
			// so the answer is about EGL rather than about a video driver.
			if(InitResult != GLEW_ERROR_NO_GLX_DISPLAY)
			{
				log_error("gfx", "Unable to init glew (glewInit): %s", glewGetErrorString(InitResult));
				return false;
			}
		}
#endif

#ifdef GLEW_VERSION_4_6
		if(GLEW_VERSION_4_6)
		{
			GlewMajor = 4;
			GlewMinor = 6;
			GlewPatch = 0;
			return true;
		}
#endif
#ifdef GLEW_VERSION_4_5
		if(GLEW_VERSION_4_5)
		{
			GlewMajor = 4;
			GlewMinor = 5;
			GlewPatch = 0;
			return true;
		}
#endif
// Don't use intermediate OpenGL 4.x versions on Windows.
#ifndef CONF_FAMILY_WINDOWS
		if(GLEW_VERSION_4_4)
		{
			GlewMajor = 4;
			GlewMinor = 4;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_3)
		{
			GlewMajor = 4;
			GlewMinor = 3;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_2)
		{
			GlewMajor = 4;
			GlewMinor = 2;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_1)
		{
			GlewMajor = 4;
			GlewMinor = 1;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_0)
		{
			GlewMajor = 4;
			GlewMinor = 0;
			GlewPatch = 0;
			return true;
		}
#endif
		if(GLEW_VERSION_3_3)
		{
			GlewMajor = 3;
			GlewMinor = 3;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_3_0)
		{
			GlewMajor = 3;
			GlewMinor = 0;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_2_1)
		{
			GlewMajor = 2;
			GlewMinor = 1;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_2_0)
		{
			GlewMajor = 2;
			GlewMinor = 0;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_5)
		{
			GlewMajor = 1;
			GlewMinor = 5;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_4)
		{
			GlewMajor = 1;
			GlewMinor = 4;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_3)
		{
			GlewMajor = 1;
			GlewMinor = 3;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_2_1)
		{
			GlewMajor = 1;
			GlewMinor = 2;
			GlewPatch = 1;
			return true;
		}
		if(GLEW_VERSION_1_2)
		{
			GlewMajor = 1;
			GlewMinor = 2;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_1)
		{
			GlewMajor = 1;
			GlewMinor = 1;
			GlewPatch = 0;
			return true;
		}
#endif
	}
	else if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		// just assume the version we need
		GlewMajor = 3;
		GlewMinor = 0;
		GlewPatch = 0;
		return true;
	}
	else
	{
		dbg_assert_failed("Invalid backend type for glew: %d", (int)BackendType);
	}

	return false;
}

static int IsVersionSupportedGlew(EBackendType BackendType, int VersionMajor, int VersionMinor, int VersionPatch, int GlewMajor, int GlewMinor, int GlewPatch)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
		if(VersionMajor >= 4 && GlewMajor < 4)
		{
			return -1;
		}
		else if(VersionMajor >= 3 && GlewMajor < 3)
		{
			return -1;
		}
		else if(VersionMajor == 3 && GlewMajor == 3)
		{
			if(VersionMinor >= 3 && GlewMinor < 3)
			{
				return -1;
			}
			if(VersionMinor >= 2 && GlewMinor < 2)
			{
				return -1;
			}
			if(VersionMinor >= 1 && GlewMinor < 1)
			{
				return -1;
			}
			if(VersionMinor >= 0 && GlewMinor < 0)
			{
				return -1;
			}
		}
		else if(VersionMajor >= 2 && GlewMajor < 2)
		{
			return -1;
		}
		else if(VersionMajor == 2 && GlewMajor == 2)
		{
			if(VersionMinor >= 1 && GlewMinor < 1)
			{
				return -1;
			}
			if(VersionMinor >= 0 && GlewMinor < 0)
			{
				return -1;
			}
		}
		else if(VersionMajor >= 1 && GlewMajor < 1)
		{
			return -1;
		}
		else if(VersionMajor == 1 && GlewMajor == 1)
		{
			if(VersionMinor >= 5 && GlewMinor < 5)
			{
				return -1;
			}
			if(VersionMinor >= 4 && GlewMinor < 4)
			{
				return -1;
			}
			if(VersionMinor >= 3 && GlewMinor < 3)
			{
				return -1;
			}
			if(VersionMinor >= 2 && GlewMinor < 2)
			{
				return -1;
			}
			else if(VersionMinor == 2 && GlewMinor == 2)
			{
				if(VersionPatch >= 1 && GlewPatch < 1)
				{
					return -1;
				}
				if(VersionPatch >= 0 && GlewPatch < 0)
				{
					return -1;
				}
			}
			if(VersionMinor >= 1 && GlewMinor < 1)
			{
				return -1;
			}
			if(VersionMinor >= 0 && GlewMinor < 0)
			{
				return -1;
			}
		}
	}
	return 0;
}
#endif // !CONF_HEADLESS_CLIENT

static void GetDrawableSize(SDL_Window *pWindow, EBackendType BackendType, int *pWidth, int *pHeight)
{
	if(BackendType == BACKEND_TYPE_VULKAN)
		SDL_Vulkan_GetDrawableSize(pWindow, pWidth, pHeight);
	else
		SDL_GL_GetDrawableSize(pWindow, pWidth, pHeight);
}

static Uint32 MessageBoxTypeToSdlFlags(IGraphics::EMessageBoxType Type)
{
	switch(Type)
	{
	case IGraphics::EMessageBoxType::ERROR:
		return SDL_MESSAGEBOX_ERROR;
	case IGraphics::EMessageBoxType::WARNING:
		return SDL_MESSAGEBOX_WARNING;
	case IGraphics::EMessageBoxType::INFO:
		return SDL_MESSAGEBOX_INFORMATION;
	default:
		dbg_assert_failed("Type invalid");
	}
}

static std::optional<int> ShowMessageBoxImpl(const IGraphics::CMessageBox &MessageBox, SDL_Window *pWindow)
{
	dbg_assert(!MessageBox.m_vButtons.empty(), "At least one button is required");

	std::vector<SDL_MessageBoxButtonData> vButtonData;
	vButtonData.reserve(MessageBox.m_vButtons.size());
	for(const auto &Button : MessageBox.m_vButtons)
	{
		SDL_MessageBoxButtonData ButtonData{};
		ButtonData.buttonid = vButtonData.size();
		ButtonData.flags = (Button.m_Confirm ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0) | (Button.m_Cancel ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0);
		ButtonData.text = Button.m_pLabel;
		vButtonData.emplace_back(ButtonData);
	}
#if defined(CONF_FAMILY_WINDOWS)
	// TODO SDL3: The order of buttons is not defined by default, but the flags returned by MessageBoxTypeToSdlFlags do not work together
	//            with SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT with SDL2 on various platforms. Windows appears to be the only platform that
	//            lays out buttons from right to left by default, so we reverse the order manually.
	std::reverse(vButtonData.begin(), vButtonData.end());
#endif
	SDL_MessageBoxData MessageBoxData{};
	MessageBoxData.title = MessageBox.m_pTitle;
	MessageBoxData.message = MessageBox.m_pMessage;
	MessageBoxData.flags = MessageBoxTypeToSdlFlags(MessageBox.m_Type);
	MessageBoxData.numbuttons = vButtonData.size();
	MessageBoxData.buttons = vButtonData.data();
	MessageBoxData.window = pWindow;
	int ButtonId = -1;
	if(SDL_ShowMessageBox(&MessageBoxData, &ButtonId) != 0)
	{
		return std::nullopt;
	}
	return ButtonId;
}

std::optional<int> ShowMessageBoxWithoutGraphics(const IGraphics::CMessageBox &MessageBox)
{
	return ShowMessageBoxImpl(MessageBox, nullptr);
}

EBackendType CGraphicsWindow_SDL::DetectBackend() const
{
	EBackendType RetBackendType = BACKEND_TYPE_OPENGL;
	if(m_BackendOverride != BACKEND_TYPE_AUTO)
		RetBackendType = m_BackendOverride;
	else
	{
		// load the config backend
		const char *pConfBackend = g_Config.m_GfxBackend;
		if(str_comp_nocase(pConfBackend, "GLES") == 0)
			RetBackendType = BACKEND_TYPE_OPENGL_ES;
#if defined(CONF_BACKEND_VULKAN)
		else if(str_comp_nocase(pConfBackend, "Vulkan") == 0)
			RetBackendType = BACKEND_TYPE_VULKAN;
#endif
		else if(str_comp_nocase(pConfBackend, "OpenGL") == 0)
			RetBackendType = BACKEND_TYPE_OPENGL;
	}
#if !defined(CONF_BACKEND_OPENGL_ES) && !defined(CONF_BACKEND_OPENGL_ES3)
	if(RetBackendType == BACKEND_TYPE_OPENGL_ES)
		RetBackendType = BACKEND_TYPE_OPENGL;
#elif defined(CONF_BACKEND_OPENGL_ES)
	if(RetBackendType == BACKEND_TYPE_OPENGL)
		RetBackendType = BACKEND_TYPE_OPENGL_ES;
#endif
	return RetBackendType;
}

void CGraphicsWindow_SDL::ClampDriverVersion(EBackendType BackendType)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
		// clamp the versions to existing versions(only for OpenGL major <= 3)
		if(g_Config.m_GfxGLMajor == 1)
		{
			g_Config.m_GfxGLMinor = std::clamp(g_Config.m_GfxGLMinor, 1, 5);
			if(g_Config.m_GfxGLMinor == 2)
				g_Config.m_GfxGLPatch = std::clamp(g_Config.m_GfxGLPatch, 0, 1);
			else
				g_Config.m_GfxGLPatch = 0;
		}
		else if(g_Config.m_GfxGLMajor == 2)
		{
			g_Config.m_GfxGLMinor = std::clamp(g_Config.m_GfxGLMinor, 0, 1);
			g_Config.m_GfxGLPatch = 0;
		}
		else if(g_Config.m_GfxGLMajor == 3)
		{
			g_Config.m_GfxGLMinor = std::clamp(g_Config.m_GfxGLMinor, 0, 3);
			g_Config.m_GfxGLPatch = 0;
		}
	}
	else if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
#if !defined(CONF_BACKEND_OPENGL_ES3)
		// Make sure GLES is set to 1.0 (which is equivalent to OpenGL 1.3), if its not set to >= 3.0(which is equivalent to OpenGL 3.3)
		if(g_Config.m_GfxGLMajor < 3)
		{
			g_Config.m_GfxGLMajor = 1;
			g_Config.m_GfxGLMinor = 0;
			g_Config.m_GfxGLPatch = 0;

			// GLES also doesn't know GL_QUAD
			g_Config.m_GfxQuadAsTriangle = 1;
		}
#else
		g_Config.m_GfxGLMajor = 3;
		g_Config.m_GfxGLMinor = 0;
		g_Config.m_GfxGLPatch = 0;
#endif
	}
	else if(BackendType == BACKEND_TYPE_VULKAN)
	{
#if defined(CONF_BACKEND_VULKAN)
		g_Config.m_GfxGLMajor = BACKEND_VULKAN_VERSION_MAJOR;
		g_Config.m_GfxGLMinor = BACKEND_VULKAN_VERSION_MINOR;
		g_Config.m_GfxGLPatch = 0;
#endif
	}
}

static void DisplayToVideoMode(CVideoMode *pVMode, SDL_DisplayMode *pMode, float HiDPIScale, int RefreshRate)
{
	pVMode->m_CanvasWidth = pMode->w * HiDPIScale;
	pVMode->m_CanvasHeight = pMode->h * HiDPIScale;
	pVMode->m_WindowWidth = pMode->w;
	pVMode->m_WindowHeight = pMode->h;
	pVMode->m_RefreshRate = RefreshRate;
}

void CGraphicsWindow_SDL::GetVideoModesImpl(CVideoMode *pModes, int MaxModes, int *pNumModes, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId)
{
	SDL_DisplayMode DesktopMode;
	int MaxModesAvailable = SDL_GetNumDisplayModes(ScreenId);

	// Only collect fullscreen modes when requested, that makes sure in windowed mode no refresh rates are shown that aren't supported without
	// fullscreen anyway(except fullscreen desktop)
	bool IsFullscreenDesktop = m_pWindow != nullptr && (((SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP) || g_Config.m_GfxFullscreen == 3);
	bool CollectFullscreenModes = m_pWindow == nullptr || ((SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN) != 0 && !IsFullscreenDesktop);

	if(SDL_GetDesktopDisplayMode(ScreenId, &DesktopMode) < 0)
	{
		log_error("gfx", "Unable to get desktop display mode of screen %d: %s", ScreenId, SDL_GetError());
	}

	constexpr int ModeCount = 256;
	SDL_DisplayMode aModes[ModeCount];
	int NumModes = 0;
	for(int i = 0; i < MaxModesAvailable && NumModes < ModeCount; i++)
	{
		SDL_DisplayMode Mode;
		if(SDL_GetDisplayMode(ScreenId, i, &Mode) < 0)
		{
			log_error("gfx", "Unable to get display mode %d of screen %d: %s", i, ScreenId, SDL_GetError());
			continue;
		}

		aModes[NumModes] = Mode;
		++NumModes;
	}

	int NumModesInserted = 0;
	auto &&ModeInsert = [&](SDL_DisplayMode &Mode) {
		if(NumModesInserted < MaxModes)
		{
			// if last mode was equal, ignore this one --- in fullscreen this can really only happen if the screen
			// supports different color modes
			// in non fullscreen these are the modes that show different refresh rate, but are basically the same
			if(NumModesInserted > 0 && pModes[NumModesInserted - 1].m_WindowWidth == Mode.w && pModes[NumModesInserted - 1].m_WindowHeight == Mode.h && (pModes[NumModesInserted - 1].m_RefreshRate == Mode.refresh_rate || (Mode.refresh_rate != DesktopMode.refresh_rate && !CollectFullscreenModes)))
				return;

			DisplayToVideoMode(&pModes[NumModesInserted], &Mode, HiDPIScale, !CollectFullscreenModes ? DesktopMode.refresh_rate : Mode.refresh_rate);
			NumModesInserted++;
		}
	};

	for(int i = 0; i < NumModes; i++)
	{
		SDL_DisplayMode &Mode = aModes[i];

		if(Mode.w > MaxWindowWidth || Mode.h > MaxWindowHeight)
			continue;

		ModeInsert(Mode);

		if(IsFullscreenDesktop)
			break;

		if(NumModesInserted >= MaxModes)
			break;
	}
	*pNumModes = NumModesInserted;
}

void CGraphicsWindow_SDL::GetCurrentVideoModeImpl(CVideoMode &CurMode, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId)
{
	SDL_DisplayMode DpMode;
	// if "real" fullscreen, obtain the video mode for that
	if((SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN)
	{
		if(SDL_GetCurrentDisplayMode(ScreenId, &DpMode))
		{
			log_error("gfx", "Unable to get current display mode of screen %d: %s", ScreenId, SDL_GetError());
		}
	}
	else
	{
		if(SDL_GetDesktopDisplayMode(ScreenId, &DpMode) < 0)
		{
			log_error("gfx", "Unable to get desktop display mode of screen %d: %s", ScreenId, SDL_GetError());
		}
		else
		{
			int Width = 0;
			int Height = 0;
			GetDrawableSize(m_pWindow, m_BackendType, &Width, &Height);
			// SDL video modes are in screen space which are logical pixels
			DpMode.w = Width / HiDPIScale;
			DpMode.h = Height / HiDPIScale;
		}
	}
	DisplayToVideoMode(&CurMode, &DpMode, HiDPIScale, DpMode.refresh_rate);
}

void CGraphicsWindow_SDL::SetWindowParamsImpl(int FullscreenMode, bool IsBorderless)
{
	// The flags have to be kept consistent with the ones OpenWindow() creates the window with!

	if(FullscreenMode > 0)
	{
		bool IsDesktopFullscreen = FullscreenMode == 2;
#ifndef CONF_FAMILY_WINDOWS
		//  Windowed fullscreen is only available on Windows, use desktop fullscreen on other platforms
		IsDesktopFullscreen |= FullscreenMode == 3;
#endif
		if(FullscreenMode == 1)
		{
#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_HAIKU)
			// Todo SDL: remove this when fixed (game freezes when losing focus in fullscreen)
			SDL_SetWindowFullscreen(m_pWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
#else
			SDL_SetWindowFullscreen(m_pWindow, SDL_WINDOW_FULLSCREEN);
#endif
			SDL_SetWindowResizable(m_pWindow, SDL_FALSE);
		}
		else if(IsDesktopFullscreen)
		{
			SDL_SetWindowFullscreen(m_pWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
			SDL_SetWindowResizable(m_pWindow, SDL_FALSE);
		}
		else // Windowed fullscreen
		{
			SDL_SetWindowFullscreen(m_pWindow, 0);
			SDL_SetWindowBordered(m_pWindow, SDL_TRUE);
			SDL_SetWindowResizable(m_pWindow, SDL_FALSE);
			SDL_DisplayMode DpMode;
			if(SDL_GetDesktopDisplayMode(g_Config.m_GfxScreen, &DpMode) < 0)
			{
				log_error("gfx", "Unable to get desktop display mode of screen %d: %s", g_Config.m_GfxScreen, SDL_GetError());
			}
			else
			{
				ResizeWindow(DpMode.w, DpMode.h, DpMode.refresh_rate);
				SDL_SetWindowPosition(m_pWindow, SDL_WINDOWPOS_CENTERED_DISPLAY(g_Config.m_GfxScreen), SDL_WINDOWPOS_CENTERED_DISPLAY(g_Config.m_GfxScreen));
			}
		}
	}
	else // Windowed
	{
		SDL_SetWindowFullscreen(m_pWindow, 0);
		SDL_SetWindowBordered(m_pWindow, SDL_bool(!IsBorderless));
		SDL_SetWindowResizable(m_pWindow, SDL_TRUE);
	}
}

bool CGraphicsWindow_SDL::SetWindowScreenImpl(int Index, bool MoveToCenter, ivec2 *pDesktopSize)
{
	if(Index < 0 || Index >= m_NumScreens)
	{
		log_error("gfx", "Invalid screen number: %d (min: 0, max: %d)", Index, m_NumScreens);
		return false;
	}

	SDL_Rect ScreenPos;
	if(SDL_GetDisplayBounds(Index, &ScreenPos) != 0)
	{
		log_error("gfx", "Unable to get bounds of screen %d: %s", Index, SDL_GetError());
		return false;
	}

	if(MoveToCenter)
	{
		SDL_SetWindowPosition(m_pWindow,
			SDL_WINDOWPOS_CENTERED_DISPLAY(Index),
			SDL_WINDOWPOS_CENTERED_DISPLAY(Index));
	}
	else
	{
		SDL_SetWindowPosition(m_pWindow,
			SDL_WINDOWPOS_UNDEFINED_DISPLAY(Index),
			SDL_WINDOWPOS_UNDEFINED_DISPLAY(Index));
	}

	return UpdateDisplayMode(Index, pDesktopSize);
}

bool CGraphicsWindow_SDL::UpdateDisplayMode(int Index, ivec2 *pDesktopSize)
{
	SDL_DisplayMode DisplayMode;
	if(SDL_GetDesktopDisplayMode(Index, &DisplayMode) < 0)
	{
		log_error("gfx", "Unable to get desktop display mode of screen %d: %s", Index, SDL_GetError());
		return false;
	}

	g_Config.m_GfxScreen = Index;
	pDesktopSize->x = DisplayMode.w;
	pDesktopSize->y = DisplayMode.h;
	return true;
}

int CGraphicsWindow_SDL::GetWindowScreen()
{
	return SDL_GetWindowDisplayIndex(m_pWindow);
}

int CGraphicsWindow_SDL::WindowActive()
{
	return m_pWindow && SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_INPUT_FOCUS;
}

int CGraphicsWindow_SDL::WindowOpen()
{
	return m_pWindow && SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_SHOWN;
}

void CGraphicsWindow_SDL::SetWindowGrab(bool Grab)
{
	// Works around https://github.com/libsdl-org/sdl2-compat/issues/578.
	if(!m_pWindow)
		return;

	SDL_SetWindowGrab(m_pWindow, Grab ? SDL_TRUE : SDL_FALSE);
}

bool CGraphicsWindow_SDL::ResizeWindow(int w, int h, int RefreshRate)
{
	// don't call resize events when the window is at fullscreen desktop
	if(!m_pWindow || (SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
		return false;

	// if the window is at fullscreen use SDL_SetWindowDisplayMode instead, suggested by SDL
	if(SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN)
	{
#ifdef CONF_FAMILY_WINDOWS
		// in windows make the window windowed mode first, this prevents strange window glitches (other games probably do something similar)
		SetWindowParamsImpl(0, true);
#endif
		SDL_DisplayMode SetMode = {};
		SDL_DisplayMode ClosestMode = {};
		SetMode.format = 0;
		SetMode.w = w;
		SetMode.h = h;
		SetMode.refresh_rate = RefreshRate;
		SDL_SetWindowDisplayMode(m_pWindow, SDL_GetClosestDisplayMode(g_Config.m_GfxScreen, &SetMode, &ClosestMode));
#ifdef CONF_FAMILY_WINDOWS
		// now change it back to fullscreen, this will restore the above set state, bcs SDL saves fullscreen modes apart from other video modes (as of SDL 2.0.16)
		// see implementation of SDL_SetWindowDisplayMode
		SetWindowParamsImpl(1, false);
#endif
		return true;
	}
	else
	{
		SDL_SetWindowSize(m_pWindow, w, h);
		if(SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_MAXIMIZED)
			// remove maximize flag
			SDL_RestoreWindow(m_pWindow);
	}

	return false;
}

void CGraphicsWindow_SDL::GetViewportSize(int &w, int &h)
{
	GetDrawableSize(m_pWindow, m_BackendType, &w, &h);
}

void CGraphicsWindow_SDL::ReadDisplayCutout()
{
#if defined(CONF_PLATFORM_IOS)
	IosDisplayCutoutInsets(m_pWindow, &m_Surface.m_InsetLeft, &m_Surface.m_InsetRight);
#else
	m_Surface.m_InsetLeft = 0;
	m_Surface.m_InsetRight = 0;
#endif
}

void CGraphicsWindow_SDL::Update()
{
#if defined(CONF_PLATFORM_IOS)
	if(m_pWindow == nullptr)
		return;
	int InsetLeft, InsetRight;
	IosDisplayCutoutInsets(m_pWindow, &InsetLeft, &InsetRight);
	if(InsetLeft != m_Surface.m_InsetLeft || InsetRight != m_Surface.m_InsetRight)
		GotResized(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, -1);
#endif
}

void CGraphicsWindow_SDL::NotifyWindow()
{
	// Minimum version 2.0.16, after version 2.0.22 the naming is changed to 2.24.0 etc.
#if SDL_MAJOR_VERSION > 2 || (SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION == 0 && SDL_PATCHLEVEL >= 16) || (SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION > 0)
	if(SDL_FlashWindow(m_pWindow, SDL_FlashOperation::SDL_FLASH_UNTIL_FOCUSED) != 0)
	{
		// fails if SDL hasn't implemented it
		return;
	}
#endif
}

bool CGraphicsWindow_SDL::IsScreenKeyboardShown()
{
	return SDL_IsScreenKeyboardShown(m_pWindow);
}

CGraphicsWindow_SDL::CGraphicsWindow_SDL() :
	m_BackendOverride(GraphicsBackendOverrideFromEnvironment())
{
}

IEngineGraphics *CGraphicsWindow_SDL::Graphics()
{
	if(m_pGraphics == nullptr)
		m_pGraphics = Kernel()->RequestInterface<IEngineGraphics>();
	return m_pGraphics;
}

void CGraphicsWindow_SDL::DestroyWindow()
{
	if(m_pWindow != nullptr)
		SDL_DestroyWindow(m_pWindow);
	m_pWindow = nullptr;
}

void CGraphicsWindow_SDL::DestroyContextAndWindow()
{
	if(m_GLContext != nullptr)
		SDL_GL_DeleteContext(m_GLContext);
	m_GLContext = nullptr;
	DestroyWindow();
}

// Creates the window and, for OpenGL, the context, and says what the backend
// has to be started with. The config carries the request and gets the
// answer: screen, size and refresh rate are corrected to what the screen
// offers, the FSAA count to what the API allows.
int CGraphicsWindow_SDL::OpenWindow(SGraphicsBackendInit &BackendInit)
{
	m_Surface = {};
	BackendInit = {};
#if defined(CONF_HEADLESS_CLIENT)
	m_BackendType = BACKEND_TYPE_NULL;
	g_Config.m_GfxGLMajor = 0;
	g_Config.m_GfxGLMinor = 0;
	g_Config.m_GfxGLPatch = 0;
	g_Config.m_GfxScreen = 0;
	g_Config.m_GfxScreenWidth = 800;
	g_Config.m_GfxScreenHeight = 600;
	g_Config.m_GfxScreenRefreshRate = 60;
	g_Config.m_GfxFsaaSamples = 0;
	m_DesktopSize = ivec2(800, 600);
	m_Surface.m_DrawableWidth = 800;
	m_Surface.m_DrawableHeight = 600;
	m_NumScreens = 1;
	log_info("gfx", "Created headless context");
#else
	// The flags have to be kept consistent with SetWindowParamsImpl()!
	const bool IsPurelyWindowed = g_Config.m_GfxFullscreen == 0;
	const bool ExclusiveFullscreen = !m_Hidden && g_Config.m_GfxFullscreen == 1;
	bool DesktopFullscreen = !m_Hidden && g_Config.m_GfxFullscreen == 2;
#ifndef CONF_FAMILY_WINDOWS
	// Windowed fullscreen is only available on Windows, use desktop fullscreen on other platforms
	DesktopFullscreen |= !m_Hidden && g_Config.m_GfxFullscreen == 3;
#endif
	const bool Resizable = !m_Hidden && !ExclusiveFullscreen && !DesktopFullscreen && IsPurelyWindowed;
	const bool Borderless = Resizable && g_Config.m_GfxBorderless;
	const bool VSync = !m_Hidden && g_Config.m_GfxVsync;

	// print sdl version
	{
		SDL_version Compiled;
		SDL_version Linked;

		SDL_VERSION(&Compiled);
		SDL_GetVersion(&Linked);
		log_info("sdl", "SDL version %d.%d.%d (compiled = %d.%d.%d)",
			Linked.major, Linked.minor, Linked.patch,
			Compiled.major, Compiled.minor, Compiled.patch);

#if CONF_PLATFORM_LINUX && SDL_VERSION_ATLEAST(2, 0, 22)
		// needed to workaround SDL from forcing exclusively X11 if linking against the GLX flavour of GLEW instead of the EGL one
		// w/o this on Wayland systems (no XWayland support) SDL's Video subsystem will fail to load (starting from SDL2.30+)
		if(Linked.major == 2 && Linked.minor >= 30)
			SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11,wayland");
#endif
	}

	if(!SDL_WasInit(SDL_INIT_VIDEO))
	{
		if(SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
		{
			log_error("gfx", "Unable to initialize SDL video: %s", SDL_GetError());
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_WINDOW_SYSTEM_INIT_FAILED;
		}
	}

	EBackendType OldBackendType = m_BackendType;
	m_BackendType = DetectBackend();
	// little fallback for Vulkan
	if(OldBackendType != BACKEND_TYPE_AUTO &&
		m_BackendType == BACKEND_TYPE_VULKAN)
	{
		// try default opengl settings
		str_copy(g_Config.m_GfxBackend, "OpenGL");
		g_Config.m_GfxGLMajor = 3;
		g_Config.m_GfxGLMinor = 0;
		g_Config.m_GfxGLPatch = 0;
		// do another analysis round too, just in case
		g_Config.m_Gfx3DTextureAnalysisRan = 0;
		g_Config.m_GfxDriverIsBlocked = 0;
		m_BackendType = DetectBackend();
	}

	ClampDriverVersion(m_BackendType);

	const bool UseModernGL = IsModernGraphicsApi(m_BackendType);
	const bool IsOpenGLFamilyBackend = m_BackendType == BACKEND_TYPE_OPENGL || m_BackendType == BACKEND_TYPE_OPENGL_ES;

	if(IsOpenGLFamilyBackend)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, g_Config.m_GfxGLMajor);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, g_Config.m_GfxGLMinor);
	}

	const char *pBackendName;
	switch(m_BackendType)
	{
	case BACKEND_TYPE_OPENGL:
		pBackendName = "OpenGL";
		break;
	case BACKEND_TYPE_OPENGL_ES:
		pBackendName = "OpenGL ES";
		break;
	case BACKEND_TYPE_VULKAN:
		pBackendName = "Vulkan";
		break;
	default:
		dbg_assert_failed("Invalid m_BackendType: %d", m_BackendType);
	}
	log_info("gfx", "Created %s %d.%d context", pBackendName, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor);

	if(m_BackendType == BACKEND_TYPE_OPENGL)
	{
		if(UseModernGL)
		{
			// The backend that has programs uses nothing OpenGL deprecated, and
			// the context is asked to hold it to that. Profiles only exist from
			// 3.2 on, but the forward compatible flag does the same job from
			// 3.0, so every context this backend runs in refuses the calls it
			// does not make - which is what keeps it that way.
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
			if(g_Config.m_GfxGLMajor > 3 || g_Config.m_GfxGLMinor >= 2)
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		}
		else
		{
			// Everything below it is fixed function, which only a compatibility
			// context has.
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		}
	}
	else if(m_BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	}

	if(IsOpenGLFamilyBackend)
	{
		g_Config.m_GfxFsaaSamples = std::clamp(g_Config.m_GfxFsaaSamples, 0, 8);
	}

	// set screen
	m_NumScreens = SDL_GetNumVideoDisplays();
	if(m_NumScreens > 0)
	{
		SDL_Rect ScreenPos;
		g_Config.m_GfxScreen = std::clamp(g_Config.m_GfxScreen, 0, m_NumScreens - 1);
		if(SDL_GetDisplayBounds(g_Config.m_GfxScreen, &ScreenPos) != 0)
		{
			log_error("gfx", "Unable to get display bounds of screen %d: %s", g_Config.m_GfxScreen, SDL_GetError());
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SCREEN_INFO_REQUEST_FAILED;
		}
	}
	else
	{
		log_error("gfx", "Unable to get number of screens: %s", SDL_GetError());
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SCREEN_REQUEST_FAILED;
	}

	// store desktop resolution for settings reset button
	SDL_DisplayMode DisplayMode;
	if(SDL_GetDesktopDisplayMode(g_Config.m_GfxScreen, &DisplayMode))
	{
		log_error("gfx", "Unable to get desktop display mode of screen %d: %s", g_Config.m_GfxScreen, SDL_GetError());
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SCREEN_RESOLUTION_REQUEST_FAILED;
	}

	bool IsDesktopChanged = m_DesktopSize.x == 0 || m_DesktopSize.y == 0 || m_DesktopSize.x != DisplayMode.w || m_DesktopSize.y != DisplayMode.h;

	m_DesktopSize.x = DisplayMode.w;
	m_DesktopSize.y = DisplayMode.h;

	// fetch supported video modes
	bool SupportedResolution = false;

	CVideoMode aModes[256];
	int ModesCount = 0;
	int IndexOfResolution = -1;
	GetVideoModesImpl(aModes, std::size(aModes), &ModesCount, 1, m_DesktopSize.x, m_DesktopSize.y, g_Config.m_GfxScreen);

	for(int i = 0; i < ModesCount; i++)
	{
		if(g_Config.m_GfxScreenWidth == aModes[i].m_WindowWidth && g_Config.m_GfxScreenHeight == aModes[i].m_WindowHeight && (g_Config.m_GfxScreenRefreshRate == aModes[i].m_RefreshRate || g_Config.m_GfxScreenRefreshRate == 0))
		{
			SupportedResolution = true;
			IndexOfResolution = i;
			break;
		}
	}

	// set flags
	int SdlFlags = SDL_WINDOW_INPUT_GRABBED | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_ALLOW_HIGHDPI;
	if(IsOpenGLFamilyBackend)
		SdlFlags |= SDL_WINDOW_OPENGL;
	else if(m_BackendType == BACKEND_TYPE_VULKAN)
		SdlFlags |= SDL_WINDOW_VULKAN;
	if(Resizable)
		SdlFlags |= SDL_WINDOW_RESIZABLE;
	if(Borderless)
		SdlFlags |= SDL_WINDOW_BORDERLESS;
	if(m_Hidden)
		SdlFlags |= SDL_WINDOW_HIDDEN;
	if(ExclusiveFullscreen)
		SdlFlags |= SDL_WINDOW_FULLSCREEN;
	else if(DesktopFullscreen)
		SdlFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	bool IsFullscreen = (SdlFlags & SDL_WINDOW_FULLSCREEN) != 0 || g_Config.m_GfxFullscreen == 3;
	// use desktop resolution as default resolution, clamp resolution if users's display is smaller than we remembered
	// if the user starts in fullscreen, and the resolution was not found use the desktop one
	if((IsFullscreen && !SupportedResolution) || g_Config.m_GfxScreenWidth == 0 || g_Config.m_GfxScreenHeight == 0 || (IsDesktopChanged && (!SupportedResolution || !IsFullscreen) && (g_Config.m_GfxScreenWidth > m_DesktopSize.x || g_Config.m_GfxScreenHeight > m_DesktopSize.y)))
	{
		g_Config.m_GfxScreenWidth = m_DesktopSize.x;
		g_Config.m_GfxScreenHeight = m_DesktopSize.y;
		g_Config.m_GfxScreenRefreshRate = DisplayMode.refresh_rate;
	}

	// if in fullscreen and refresh rate wasn't set yet, just use the one from the found list
	if(g_Config.m_GfxScreenRefreshRate == 0 && SupportedResolution)
	{
		g_Config.m_GfxScreenRefreshRate = aModes[IndexOfResolution].m_RefreshRate;
	}
	else if(g_Config.m_GfxScreenRefreshRate == 0)
	{
		g_Config.m_GfxScreenRefreshRate = DisplayMode.refresh_rate;
	}

	// set gl attributes
	if(IsOpenGLFamilyBackend)
	{
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		if(g_Config.m_GfxFsaaSamples)
		{
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_Config.m_GfxFsaaSamples);
		}
		else
		{
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}
	}

	m_pWindow = SDL_CreateWindow(
		"DDNet Client",
		SDL_WINDOWPOS_CENTERED_DISPLAY(g_Config.m_GfxScreen),
		SDL_WINDOWPOS_CENTERED_DISPLAY(g_Config.m_GfxScreen),
		g_Config.m_GfxScreenWidth,
		g_Config.m_GfxScreenHeight,
		SdlFlags);

	// set caption
	if(m_pWindow == nullptr)
	{
		log_error("gfx", "Unable to create window: %s", SDL_GetError());
		if(m_BackendType == BACKEND_TYPE_VULKAN)
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
		else
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_WINDOW_CREATE_FAILED;
	}

	int GlewMajor = 0;
	int GlewMinor = 0;
	int GlewPatch = 0;

	if(IsOpenGLFamilyBackend)
	{
		m_GLContext = SDL_GL_CreateContext(m_pWindow);

		if(m_GLContext == nullptr)
		{
			log_error("gfx", "Unable to create graphics context: %s", SDL_GetError());
			DestroyWindow();
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
		}

		if(!BackendInitGlew(m_BackendType, GlewMajor, GlewMinor, GlewPatch))
		{
			SDL_GL_DeleteContext(m_GLContext);
			DestroyWindow();
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_INTERFACE_INIT_FAILED;
		}
	}

	int InitError = IsVersionSupportedGlew(m_BackendType, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch, GlewMajor, GlewMinor, GlewPatch);

	// SDL_GL_GetDrawableSize reports HiDPI resolution even with SDL_WINDOW_ALLOW_HIGHDPI not set, which is wrong
	if(SdlFlags & SDL_WINDOW_ALLOW_HIGHDPI)
	{
		GetDrawableSize(m_pWindow, m_BackendType, &m_Surface.m_DrawableWidth, &m_Surface.m_DrawableHeight);
	}
	else
	{
		SDL_GetWindowSize(m_pWindow, &m_Surface.m_DrawableWidth, &m_Surface.m_DrawableHeight);
	}
	SDL_GetWindowSize(m_pWindow, &g_Config.m_GfxScreenWidth, &g_Config.m_GfxScreenHeight);

	if(IsOpenGLFamilyBackend)
	{
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
		// SDL_GL_SetSwapInterval is not supported with Emscripten as this is only a wrapper for the
		// emscripten_set_main_loop_timing function which does not work because we do not use the
		// emscripten_set_main_loop function before.
		SDL_GL_SetSwapInterval(VSync ? 1 : 0);
#endif
		SDL_GL_MakeCurrent(nullptr, nullptr);
	}

	if(InitError != 0)
	{
		if(m_GLContext)
			SDL_GL_DeleteContext(m_GLContext);
		DestroyWindow();

		// try setting to glew supported version
		g_Config.m_GfxGLMajor = GlewMajor;
		g_Config.m_GfxGLMinor = GlewMinor;
		g_Config.m_GfxGLPatch = GlewPatch;

		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED;
	}

	BackendInit.m_pSurface = this;
	BackendInit.m_GlContext = m_GLContext != nullptr;
	BackendInit.m_VSync = VSync;
	BackendInit.m_GlewMajor = GlewMajor;
	BackendInit.m_GlewMinor = GlewMinor;
	BackendInit.m_GlewPatch = GlewPatch;
	m_Surface.m_Presentable = true;
#endif
	BackendInit.m_Width = m_Surface.m_DrawableWidth;
	BackendInit.m_Height = m_Surface.m_DrawableHeight;
	BackendInit.m_FsaaSamples = g_Config.m_GfxFsaaSamples;
	BackendInit.m_RequestedMajor = g_Config.m_GfxGLMajor;
	BackendInit.m_RequestedMinor = g_Config.m_GfxGLMinor;
	BackendInit.m_RequestedPatch = g_Config.m_GfxGLPatch;
	BackendInit.m_pStorage = Kernel()->RequestInterface<IStorage>();
	ReadDisplayCutout();
	m_Surface.m_WindowWidth = g_Config.m_GfxScreenWidth;
	m_Surface.m_WindowHeight = g_Config.m_GfxScreenHeight;
	m_Surface.m_RefreshRate = g_Config.m_GfxScreenRefreshRate;
	m_HiDPIScale = m_Surface.m_DrawableWidth / (float)std::max(m_Surface.m_WindowWidth, 1);
	return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_NONE;
}

int CGraphicsWindow_SDL::TryOpen(IGraphicsBackend **ppBackend)
{
	SGraphicsBackendInit BackendInit;
	const int WindowError = OpenWindow(BackendInit);
	if(WindowError != 0)
		return WindowError;

	IGraphicsBackend *pBackend = CreateGraphicsBackend(m_BackendType, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor);
	const int BackendError = pBackend->Init(BackendInit);
	// What the backend has to say about a failed start is worth showing,
	// even when the next attempt works.
	if(pBackend->GetErrorString() != nullptr)
	{
		SWarning Warning;
		str_copy(Warning.m_aWarningMsg, Localize(pBackend->GetErrorString()));
		Graphics()->AddWarning(Warning);
	}
	if(BackendError != 0)
	{
		delete pBackend;
		DestroyContextAndWindow();
		return BackendError;
	}
	*ppBackend = pBackend;
	return 0;
}

IGraphicsBackend *CGraphicsWindow_SDL::Open(bool Hidden)
{
	m_Hidden = Hidden;
	IGraphicsBackend *pBackend = nullptr;
	int ErrorCode = TryOpen(&pBackend);
	if(ErrorCode == 0)
		return pBackend;

	// try disabling fsaa
	while(g_Config.m_GfxFsaaSamples)
	{
		// 4 is the minimum required by OpenGL ES spec (GL_MAX_SAMPLES - https://www.khronos.org/registry/OpenGL-Refpages/es3.0/html/glGet.xhtml),
		// so can probably also be assumed for OpenGL
		if(g_Config.m_GfxFsaaSamples > 4)
			g_Config.m_GfxFsaaSamples = 4;
		else
			g_Config.m_GfxFsaaSamples = 0;

		if(g_Config.m_GfxFsaaSamples)
			log_warn("gfx", "Failed to initialize graphics. Lowering FSAA to %d and trying again.", g_Config.m_GfxFsaaSamples);
		else
			log_warn("gfx", "Failed to initialize graphics. Disabling FSAA and trying again.");

		ErrorCode = TryOpen(&pBackend);
		if(ErrorCode == 0)
			return pBackend;
	}

	size_t GLInitTryCount = 0;
	while(ErrorCode == EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED ||
		ErrorCode == EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED)
	{
		if(ErrorCode == EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED)
		{
			// try next smaller major/minor or patch version
			if(g_Config.m_GfxGLMajor >= 4)
			{
				g_Config.m_GfxGLMajor = 3;
				g_Config.m_GfxGLMinor = 3;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor >= 1)
			{
				g_Config.m_GfxGLMajor = 3;
				g_Config.m_GfxGLMinor = 0;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor == 0)
			{
				g_Config.m_GfxGLMajor = 2;
				g_Config.m_GfxGLMinor = 1;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 2 && g_Config.m_GfxGLMinor >= 1)
			{
				g_Config.m_GfxGLMajor = 2;
				g_Config.m_GfxGLMinor = 0;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 2 && g_Config.m_GfxGLMinor == 0)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 5;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 5)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 4;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 4)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 3;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 3)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 2;
				g_Config.m_GfxGLPatch = 1;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 2)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 1;
				g_Config.m_GfxGLPatch = 0;
			}
		}
		log_warn("gfx", "Failed to initialize graphics. Setting GL version %d.%d.%d and trying again.", g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch);

		// new gl version was set by backend, try again
		ErrorCode = TryOpen(&pBackend);
		if(ErrorCode == 0)
			return pBackend;

		if(++GLInitTryCount >= 9)
		{
			// try something else
			break;
		}
	}

	// try lowering the resolution
	if(g_Config.m_GfxScreenWidth != 640 || g_Config.m_GfxScreenHeight != 480)
	{
		g_Config.m_GfxScreenWidth = 640;
		g_Config.m_GfxScreenHeight = 480;
		log_warn("gfx", "Failed to initialize graphics. Setting resolution to %dx%d and trying again.", g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight);

		if(TryOpen(&pBackend) == 0)
			return pBackend;
	}

	// at the very end, just try to set to gl 1.4
	{
		g_Config.m_GfxGLMajor = 1;
		g_Config.m_GfxGLMinor = 4;
		g_Config.m_GfxGLPatch = 0;
		log_warn("gfx", "Failed to initialize graphics. Setting GL version %d.%d.%d and trying again.", g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch);

		if(TryOpen(&pBackend) == 0)
			return pBackend;
	}

	log_error("gfx", "Failed to initialize graphics. Out of ideas.");
	return nullptr;
}

void CGraphicsWindow_SDL::Close()
{
	DestroyContextAndWindow();
#if !defined(CONF_HEADLESS_CLIENT)
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
}

void CGraphicsWindow_SDL::NotifyPropChange()
{
	for(auto &PropChangedListener : m_vPropChangeListeners)
		PropChangedListener();
}

void CGraphicsWindow_SDL::AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc)
{
	m_vPropChangeListeners.emplace_back(pFunc);
}

// The window changed size, by us or by the window manager. The graphics get
// the new drawable size; the config gets the new window size.
void CGraphicsWindow_SDL::GotResized(int w, int h, int RefreshRate)
{
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && IVideo::Current()->IsRecording())
		return;
#endif
	// if RefreshRate is -1 use the current config refresh rate
	if(RefreshRate == -1)
		RefreshRate = g_Config.m_GfxScreenRefreshRate;

	GetViewportSize(m_Surface.m_DrawableWidth, m_Surface.m_DrawableHeight);
	ReadDisplayCutout();
	m_Surface.m_WindowWidth = w;
	m_Surface.m_WindowHeight = h;
	m_Surface.m_RefreshRate = RefreshRate;
	g_Config.m_GfxScreenWidth = w;
	g_Config.m_GfxScreenHeight = h;
	g_Config.m_GfxScreenRefreshRate = RefreshRate;

	const float OldDpi = m_HiDPIScale;
	m_HiDPIScale = m_Surface.m_DrawableWidth / (float)std::max(w, 1);
	for(auto &FakeMode : g_aFakeModes)
	{
		FakeMode.m_WindowWidth = FakeMode.m_CanvasWidth / m_HiDPIScale;
		FakeMode.m_WindowHeight = FakeMode.m_CanvasHeight / m_HiDPIScale;
		FakeMode.m_RefreshRate = RefreshRate;
	}

	Graphics()->Resized(m_Surface);

	// A DPI change must notify the listeners, since e.g. video modes
	// currently depend on it.
	if(OldDpi != m_HiDPIScale)
		NotifyPropChange();
}

void CGraphicsWindow_SDL::SetWindowParams(int FullscreenMode, bool IsBorderless)
{
	g_Config.m_GfxFullscreen = std::clamp(FullscreenMode, 0, 3);
	g_Config.m_GfxBorderless = (int)IsBorderless;

	SetWindowParamsImpl(g_Config.m_GfxFullscreen, g_Config.m_GfxBorderless);
	CVideoMode CurMode;
	GetCurrentVideoModeImpl(CurMode, m_HiDPIScale, m_DesktopSize.x, m_DesktopSize.y, g_Config.m_GfxScreen);
	GotResized(CurMode.m_WindowWidth, CurMode.m_WindowHeight, CurMode.m_RefreshRate);

	NotifyPropChange();
}

bool CGraphicsWindow_SDL::SetWindowScreen(int Index, bool MoveToCenter)
{
	if(!SetWindowScreenImpl(Index, MoveToCenter, &m_DesktopSize))
		return false;

	// send a got resized event so that the current canvas size is requested
	GotResized(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate);

	NotifyPropChange();
	return true;
}

bool CGraphicsWindow_SDL::SwitchWindowScreen(int Index, bool MoveToCenter)
{
	const int IsFullscreen = g_Config.m_GfxFullscreen;
	const int IsBorderless = g_Config.m_GfxBorderless;
	const bool IsPurelyWindowed = IsFullscreen == 0 && !IsBorderless;

	if(!SetWindowScreen(Index, !IsPurelyWindowed || MoveToCenter))
		return false;

	if(IsFullscreen != 3 && !IsPurelyWindowed)
	{
		// Prevent window from being stretched over multiple monitors by temporarily switching to
		// windowed fullscreen mode on Windows, which is desktop fullscreen mode on other systems.
		SetWindowParams(3, false);
	}

	// In purely windowed mode we preserve the window's size instead of resizing to the screen.
	if(!IsPurelyWindowed)
	{
		CVideoMode CurMode;
		GetCurrentVideoMode(CurMode, Index);

		g_Config.m_GfxScreenWidth = CurMode.m_WindowWidth;
		g_Config.m_GfxScreenHeight = CurMode.m_WindowHeight;
		g_Config.m_GfxScreenRefreshRate = CurMode.m_RefreshRate;

		ResizeToScreen();
	}

	SetWindowParams(IsFullscreen, IsBorderless);
	return true;
}

bool CGraphicsWindow_SDL::SetVSync(bool State)
{
	return Graphics()->SetVSync(State);
}

bool CGraphicsWindow_SDL::SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend)
{
	return Graphics()->SetMultiSampling(ReqMultiSamplingCount, MultiSamplingCountBackend);
}

void CGraphicsWindow_SDL::Move(int x, int y)
{
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && IVideo::Current()->IsRecording())
		return;
#endif

	// Only handling CurScreen != m_GfxScreen doesn't work reliably
	const int CurScreen = GetWindowScreen();
	if(!UpdateDisplayMode(CurScreen, &m_DesktopSize))
		return;

	// send a got resized event so that the current canvas size is requested
	GotResized(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate);

	NotifyPropChange();
}

bool CGraphicsWindow_SDL::Resize(int w, int h, int RefreshRate)
{
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && IVideo::Current()->IsRecording())
		return false;
#endif

	if(m_Surface.m_WindowWidth == w && m_Surface.m_WindowHeight == h && RefreshRate == m_Surface.m_RefreshRate)
		return false;

	// if the size is changed manually, only set the window resize, a window size changed event is triggered anyway
	if(ResizeWindow(w, h, RefreshRate))
	{
		GotResized(w, h, RefreshRate);
		return true;
	}
	return false;
}

void CGraphicsWindow_SDL::ResizeToScreen()
{
	if(Resize(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate))
		return;

	// Revert config variables if the change was not accepted
	g_Config.m_GfxScreenWidth = Graphics()->ScreenWidth();
	g_Config.m_GfxScreenHeight = Graphics()->ScreenHeight();
	g_Config.m_GfxScreenRefreshRate = m_Surface.m_RefreshRate;
}

int CGraphicsWindow_SDL::GetVideoModes(CVideoMode *pModes, int MaxModes, int ScreenIndex)
{
	if(g_Config.m_GfxDisplayAllVideoModes)
	{
		const int Count = std::min(std::size(g_aFakeModes), (size_t)MaxModes);
		mem_copy(pModes, g_aFakeModes, Count * sizeof(CVideoMode));
		return Count;
	}

	int NumModes = 0;
	GetVideoModesImpl(pModes, MaxModes, &NumModes, m_HiDPIScale, m_DesktopSize.x, m_DesktopSize.y, ScreenIndex);
	return NumModes;
}

void CGraphicsWindow_SDL::GetCurrentVideoMode(CVideoMode &CurMode, int ScreenIndex)
{
	GetCurrentVideoModeImpl(CurMode, m_HiDPIScale, m_DesktopSize.x, m_DesktopSize.y, ScreenIndex);
}

void CGraphicsWindow_SDL::Minimize()
{
	SDL_MinimizeWindow(m_pWindow);
	NotifyPropChange();
}

const char *CGraphicsWindow_SDL::GetScreenName(int ScreenIndex) const
{
	const char *pName = SDL_GetDisplayName(ScreenIndex);
	return pName == nullptr ? "unknown/error" : pName;
}

std::optional<int> CGraphicsWindow_SDL::ShowMessageBox(const IGraphics::CMessageBox &MessageBox)
{
	if(Graphics() != nullptr)
		Graphics()->ReleaseSurfaceForMessageBox();
	// TODO: Remove this workaround when https://github.com/libsdl-org/SDL/issues/3750 is
	// fixed and pass the window to SDL_ShowSimpleMessageBox to make the popup modal instead
	// of destroying the window before opening the popup.
	if(m_pWindow != nullptr)
	{
		SDL_DestroyWindow(m_pWindow);
		m_pWindow = nullptr;
	}
	return ShowMessageBoxImpl(MessageBox, m_pWindow);
}

void CGraphicsWindow_SDL::OnMoved(int x, int y)
{
	Move(x, y);
}

void CGraphicsWindow_SDL::OnSizeChanged(int w, int h)
{
	GotResized(w, h, -1);
}

void CGraphicsWindow_SDL::OnDisplayChanged(int DisplayIndex)
{
	SwitchWindowScreen(DisplayIndex, false);
}

void CGraphicsWindow_SDL::OnWindowDestroyed()
{
	Graphics()->PresentationSurfaceLost();
}

void CGraphicsWindow_SDL::OnWindowCreated(uint32_t WindowId)
{
	m_pWindow = SDL_GetWindowFromID(WindowId);
	Graphics()->PresentationSurfaceRestored();
}

// ------------ the surface, seen from the render thread

void CGraphicsWindow_SDL::DrawableSize(int &Width, int &Height) const
{
	GetDrawableSize(m_pWindow, m_BackendType, &Width, &Height);
}

bool CGraphicsWindow_SDL::BindGlContext()
{
	return m_GLContext != nullptr && SDL_GL_MakeCurrent(m_pWindow, m_GLContext) == 0;
}

void CGraphicsWindow_SDL::UnbindGlContext()
{
	if(m_GLContext != nullptr)
		SDL_GL_MakeCurrent(nullptr, nullptr);
}

void CGraphicsWindow_SDL::SwapGlBuffers()
{
	if(m_GLContext != nullptr)
		SDL_GL_SwapWindow(m_pWindow);
}

bool CGraphicsWindow_SDL::SetGlSwapInterval(bool VSync)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// SDL_GL_SetSwapInterval is not supported with Emscripten as this is only a wrapper for the
	// emscripten_set_main_loop_timing function which does not work because we do not use the
	// emscripten_set_main_loop function before.
	return !VSync;
#else
	return m_GLContext != nullptr && SDL_GL_SetSwapInterval(VSync ? 1 : 0) == 0;
#endif
}

bool CGraphicsWindow_SDL::VulkanInstanceExtensions(std::vector<std::string> &vExtensions)
{
#if defined(CONF_BACKEND_VULKAN)
	unsigned int ExtCount = 0;
	if(!SDL_Vulkan_GetInstanceExtensions(m_pWindow, &ExtCount, nullptr))
	{
		log_error("gfx/vulkan", "Could not get instance extensions from SDL: %s", SDL_GetError());
		return false;
	}
	std::vector<const char *> vExtensionList(ExtCount);
	if(!SDL_Vulkan_GetInstanceExtensions(m_pWindow, &ExtCount, vExtensionList.data()))
	{
		log_error("gfx/vulkan", "Could not get instance extensions from SDL: %s", SDL_GetError());
		return false;
	}
	vExtensions.reserve(vExtensions.size() + ExtCount);
	for(uint32_t i = 0; i < ExtCount; i++)
		vExtensions.emplace_back(vExtensionList[i]);
	return true;
#else
	return false;
#endif
}

bool CGraphicsWindow_SDL::CreateVulkanSurface(const void *pInstance, void *pSurface)
{
#if defined(CONF_BACKEND_VULKAN)
	if(!SDL_Vulkan_CreateSurface(m_pWindow, *static_cast<const VkInstance *>(pInstance), static_cast<VkSurfaceKHR *>(pSurface)))
	{
		log_error("gfx/vulkan", "Failed to create surface. SDL error: %s", SDL_GetError());
		return false;
	}
	return true;
#else
	return false;
#endif
}

IEngineGraphicsWindow *CreateSdlGraphicsWindow()
{
	return new CGraphicsWindow_SDL();
}
