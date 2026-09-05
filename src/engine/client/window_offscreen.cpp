// The window of the surface-less client: none. Every question about it is
// answered with the size of the virtual screen the graphics draw into, and
// the backend it opens is one that draws into render targets alone.

#include <base/log.h>

#include <engine/client/backend_threaded.h>
#include <engine/client/graphics_backend.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#if defined(CONF_BACKEND_VULKAN)
#include <engine/client/backend/vulkan/backend_vulkan.h>
#endif

#include <algorithm>
#include <vector>

class CGraphicsWindow_Offscreen : public IEngineGraphicsWindow
{
	SGraphicsSurfaceInfo m_Surface;
	std::vector<WINDOW_PROPS_CHANGED_FUNC> m_vPropChangeListeners;
	IEngineGraphics *m_pGraphics = nullptr;

	IEngineGraphics *Graphics()
	{
		if(m_pGraphics == nullptr)
			m_pGraphics = Kernel()->RequestInterface<IEngineGraphics>();
		return m_pGraphics;
	}

	CVideoMode Mode() const
	{
		return {m_Surface.m_DrawableWidth, m_Surface.m_DrawableHeight, m_Surface.m_WindowWidth, m_Surface.m_WindowHeight, m_Surface.m_RefreshRate};
	}

public:
	IGraphicsBackend *Open(bool Hidden) override
	{
		IGraphicsBackend *pBackend = CreateOffscreenGraphicsBackend(GraphicsBackendOverrideFromEnvironment());
		if(pBackend == nullptr)
		{
			log_error("gfx", "The requested graphics backend does not support offscreen rendering.");
			return nullptr;
		}
		const int Width = std::max(g_Config.m_GfxScreenWidth, 1);
		const int Height = std::max(g_Config.m_GfxScreenHeight, 1);

		SGraphicsBackendInit Init;
		Init.m_Width = Width;
		Init.m_Height = Height;
		Init.m_FsaaSamples = g_Config.m_GfxFsaaSamples;
		Init.m_pStorage = Kernel()->RequestInterface<IStorage>();
		switch(pBackend->BackendType())
		{
#if defined(CONF_BACKEND_VULKAN)
		case BACKEND_TYPE_VULKAN:
			Init.m_RequestedMajor = BACKEND_VULKAN_VERSION_MAJOR;
			Init.m_RequestedMinor = BACKEND_VULKAN_VERSION_MINOR;
			break;
#endif
		case BACKEND_TYPE_WEBGPU:
			Init.m_RequestedMajor = 1;
			Init.m_RequestedMinor = 0;
			break;
		default:
			break;
		}
		if(pBackend->Init(Init) != 0)
		{
			log_error("gfx", "Failed to initialize the offscreen graphics backend.");
			delete pBackend;
			return nullptr;
		}

		g_Config.m_GfxScreen = 0;
		g_Config.m_GfxScreenWidth = Width;
		g_Config.m_GfxScreenHeight = Height;
		g_Config.m_GfxScreenRefreshRate = 0;
		m_Surface = {};
		m_Surface.m_DrawableWidth = m_Surface.m_WindowWidth = Width;
		m_Surface.m_DrawableHeight = m_Surface.m_WindowHeight = Height;
		return pBackend;
	}
	const SGraphicsSurfaceInfo &Surface() const override { return m_Surface; }
	void Close() override {}

	void OnMoved(int x, int y) override {}
	void OnSizeChanged(int w, int h) override { Resize(w, h, 0); }
	void OnDisplayChanged(int Display) override {}
	void OnWindowDestroyed() override {}
	void OnWindowCreated(uint32_t WindowId) override {}

	void SetWindowParams(int FullscreenMode, bool IsBorderless) override {}
	bool SetWindowScreen(int Index, bool MoveToCenter) override { return true; }
	bool SwitchWindowScreen(int Index, bool MoveToCenter) override { return true; }
	int GetWindowScreen() override { return 0; }
	int GetNumScreens() const override { return 1; }
	const char *GetScreenName(int Screen) const override { return "Offscreen"; }
	bool SetVSync(bool State) override { return Graphics()->SetVSync(State); }
	bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) override { return Graphics()->SetMultiSampling(ReqMultiSamplingCount, MultiSamplingCountBackend); }
	void Move(int x, int y) override {}
	bool Resize(int w, int h, int RefreshRate) override
	{
		w = std::max(w, 1);
		h = std::max(h, 1);
		if(w == m_Surface.m_WindowWidth && h == m_Surface.m_WindowHeight)
			return false;
		m_Surface.m_DrawableWidth = m_Surface.m_WindowWidth = w;
		m_Surface.m_DrawableHeight = m_Surface.m_WindowHeight = h;
		g_Config.m_GfxScreenWidth = w;
		g_Config.m_GfxScreenHeight = h;
		Graphics()->Resized(m_Surface);
		return true;
	}
	void ResizeToScreen() override { Resize(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate); }
	bool IsScreenKeyboardShown() override { return false; }
	int GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen) override
	{
		if(MaxModes <= 0)
			return 0;
		pModes[0] = Mode();
		return 1;
	}
	void GetCurrentVideoMode(CVideoMode &CurMode, int Screen) override { CurMode = Mode(); }
	void SetWindowGrab(bool Grab) override {}
	void NotifyWindow() override {}
	void Minimize() override {}
	int WindowActive() override { return 1; }
	int WindowOpen() override { return 1; }
	void AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc) override { m_vPropChangeListeners.emplace_back(pFunc); }
	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override { return std::nullopt; }
};

IEngineGraphicsWindow *CreateOffscreenGraphicsWindow()
{
	return new CGraphicsWindow_Offscreen();
}
