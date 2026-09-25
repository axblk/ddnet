#include "window_web.h"

#include "web_platform.h"

#include <base/log.h>
#include <base/math.h>
#include <base/thread.h>

#include <engine/client/backend/webgpu/backend_webgpu.h>
#include <engine/client/graphics_backend.h>
#include <engine/client/presentation_surface.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <algorithm>
#include <array>

namespace
{
	// What the window is before the page has measured the canvas.
	constexpr int DEFAULT_WIDTH = 800;
	constexpr int DEFAULT_HEIGHT = 600;
	// WebGL 2 is OpenGL ES 3.0, which is what the GLES backend is asked for.
	constexpr int WEBGL_MAJOR = 3;
	constexpr int WEBGL_MINOR = 0;
} // namespace

class CGraphicsWindow_Web : public IEngineGraphicsWindow, public IPresentationSurface
{
	SGraphicsSurfaceInfo m_Surface;
	float m_Scale = 1.0f;
	EBackendType m_BackendType = BACKEND_TYPE_AUTO;
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE m_GlContext = 0;
	SWebGpuNativeWindow m_WebGpuNativeWindow;
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

	// The canvas's size in CSS pixels, and in drawn pixels at the device's
	// pixel ratio.
	void SetSize(int Width, int Height)
	{
		m_Surface.m_WindowWidth = std::max(Width, 1);
		m_Surface.m_WindowHeight = std::max(Height, 1);
		m_Surface.m_DrawableWidth = std::max(round_to_int(m_Surface.m_WindowWidth * m_Scale), 1);
		m_Surface.m_DrawableHeight = std::max(round_to_int(m_Surface.m_WindowHeight * m_Scale), 1);
		g_Config.m_GfxScreenWidth = m_Surface.m_WindowWidth;
		g_Config.m_GfxScreenHeight = m_Surface.m_WindowHeight;
		emscripten_set_canvas_element_size("#canvas", m_Surface.m_DrawableWidth, m_Surface.m_DrawableHeight);
	}

	bool CreateGlContext()
	{
		EmscriptenWebGLContextAttributes Attributes;
		emscripten_webgl_init_context_attributes(&Attributes);
		Attributes.majorVersion = 2;
		Attributes.minorVersion = 0;
		// What SDL asked the browser for.
		Attributes.alpha = false;
		Attributes.depth = true;
		Attributes.stencil = false;
		Attributes.antialias = g_Config.m_GfxFsaaSamples > 0;
		Attributes.preserveDrawingBuffer = false;
		Attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
		m_GlContext = emscripten_webgl_create_context("#canvas", &Attributes);
		if(m_GlContext <= 0)
		{
			m_GlContext = 0;
			log_error("gfx", "The browser gave no WebGL 2 context");
			return false;
		}
		return true;
	}

	void DestroyGlContext()
	{
		if(m_GlContext == 0)
			return;
		if(emscripten_webgl_get_current_context() == m_GlContext)
			emscripten_webgl_make_context_current(0);
		emscripten_webgl_destroy_context(m_GlContext);
		m_GlContext = 0;
	}

	IGraphicsBackend *TryBackend(EBackendType BackendType)
	{
		m_BackendType = BackendType;
		SGraphicsBackendInit Init;
		Init.m_pSurface = this;
		Init.m_Width = m_Surface.m_DrawableWidth;
		Init.m_Height = m_Surface.m_DrawableHeight;
		Init.m_VSync = g_Config.m_GfxVsync != 0;
		Init.m_pStorage = Kernel()->RequestInterface<IStorage>();
		if(BackendType == BACKEND_TYPE_WEBGPU)
		{
			g_Config.m_GfxFsaaSamples = static_cast<int>(WebGpuMultiSamplingCount(std::max(g_Config.m_GfxFsaaSamples, 0)));
			m_WebGpuNativeWindow.m_Type = SWebGpuNativeWindow::EType::CANVAS;
		}
		else
		{
			g_Config.m_GfxFsaaSamples = std::clamp(g_Config.m_GfxFsaaSamples, 0, 8);
			g_Config.m_GfxGLMajor = WEBGL_MAJOR;
			g_Config.m_GfxGLMinor = WEBGL_MINOR;
			g_Config.m_GfxGLPatch = 0;
			Init.m_RequestedMajor = Init.m_GlewMajor = WEBGL_MAJOR;
			Init.m_RequestedMinor = Init.m_GlewMinor = WEBGL_MINOR;
			if(!CreateGlContext())
				return nullptr;
		}
		Init.m_FsaaSamples = g_Config.m_GfxFsaaSamples;

		IGraphicsBackend *pBackend = CreateGraphicsBackend(BackendType, WEBGL_MAJOR, WEBGL_MINOR);
		if(pBackend->Init(Init) != 0)
		{
			delete pBackend;
			DestroyGlContext();
			m_WebGpuNativeWindow = {};
			return nullptr;
		}
		return pBackend;
	}

public:
	IGraphicsBackend *Open(bool Hidden) override
	{
		int Width = 0;
		int Height = 0;
		float Scale = 1.0f;
		if(!ddnet_web_canvas_prepare(&Width, &Height, &Scale))
		{
			log_error("gfx", "The page handed over no canvas to draw into");
			return nullptr;
		}
		m_Scale = Scale > 0.0f ? Scale : 1.0f;
		// A canvas the page has not laid out yet gets the size the settings
		// ask for until the page says otherwise.
		if(Width <= 0 || Height <= 0)
		{
			Width = g_Config.m_GfxScreenWidth > 0 ? g_Config.m_GfxScreenWidth : DEFAULT_WIDTH;
			Height = g_Config.m_GfxScreenHeight > 0 ? g_Config.m_GfxScreenHeight : DEFAULT_HEIGHT;
		}
		m_Surface = {};
		m_Surface.m_Presentable = true;
		SetSize(Width, Height);
		g_Config.m_GfxScreen = 0;
		g_Config.m_GfxFullscreen = 0;
		g_Config.m_GfxScreenRefreshRate = 0;

		// WebGPU unless the settings or the environment ask for WebGL, which
		// is also what is left where the browser has no WebGPU adapter.
		EBackendType Wanted = GraphicsBackendOverrideFromEnvironment();
		if(Wanted == BACKEND_TYPE_AUTO)
			Wanted = BackendTypeFromName(g_Config.m_GfxBackend);
		std::array<EBackendType, 2> aOrder = {BACKEND_TYPE_WEBGPU, BACKEND_TYPE_OPENGL_ES};
		size_t Count = 2;
#if defined(CONF_BACKEND_WEBGPU)
		if(Wanted != BACKEND_TYPE_WEBGPU && Wanted != BACKEND_TYPE_AUTO)
#endif
		{
			aOrder[0] = BACKEND_TYPE_OPENGL_ES;
			Count = 1;
		}
		for(size_t i = 0; i < Count; ++i)
		{
			if(IGraphicsBackend *pBackend = TryBackend(aOrder[i]))
			{
				if(m_BackendType == BACKEND_TYPE_WEBGPU)
					log_info("gfx", "Created %s context", BackendTypeName(m_BackendType));
				else
					log_info("gfx", "Created WebGL 2 context (%s %d.%d)", BackendTypeName(m_BackendType), WEBGL_MAJOR, WEBGL_MINOR);
				return pBackend;
			}
			if(aOrder[i] == BACKEND_TYPE_WEBGPU && i + 1 < Count)
				log_warn("gfx/webgpu", "initialization failed; retrying with WebGL 2");
		}
		log_error("gfx", "Neither WebGPU nor WebGL 2 could draw into the canvas");
		return nullptr;
	}

	const SGraphicsSurfaceInfo &Surface() const override { return m_Surface; }

	void Close() override
	{
		DestroyGlContext();
		m_WebGpuNativeWindow = {};
	}

	// IPresentationSurface, asked on the render thread.
	void DrawableSize(int &Width, int &Height) const override
	{
		Width = m_Surface.m_DrawableWidth;
		Height = m_Surface.m_DrawableHeight;
	}
	bool BindGlContext() override
	{
		return m_GlContext != 0 && emscripten_webgl_make_context_current(m_GlContext) == EMSCRIPTEN_RESULT_SUCCESS;
	}
	void UnbindGlContext() override
	{
		if(m_GlContext != 0 && emscripten_webgl_get_current_context() == m_GlContext)
			emscripten_webgl_make_context_current(0);
	}
	// The browser shows what was drawn once the thread hands it its turn,
	// which is here, as SDL's swap did.
	void SwapGlBuffers() override { web_yield(0); }
	// The browser draws at the display's pace and cannot be asked otherwise.
	bool SetGlSwapInterval(bool VSync) override { return false; }
	bool VulkanInstanceExtensions(std::vector<std::string> &vExtensions) override { return false; }
	bool CreateVulkanSurface(const void *pInstance, void *pSurface) override { return false; }
	const SWebGpuNativeWindow &WebGpuNativeWindow() const override { return m_WebGpuNativeWindow; }

	// The page places the canvas; there is nothing to move and one screen.
	void OnMoved(int x, int y) override {}
	void OnSizeChanged(int w, int h) override { Resize(w, h, 0); }
	void OnDisplayChanged(int Display) override {}
	void OnWindowDestroyed() override {}
	void OnWindowCreated(uint32_t WindowId) override {}
	void SetWindowParams(int FullscreenMode, bool IsBorderless) override {}
	bool SwitchWindowScreen(int Index, bool MoveToCenter) override { return true; }
	int GetNumScreens() const override { return 1; }
	const char *GetScreenName(int Screen) const override { return "Page"; }
	bool SetVSync(bool State) override { return Graphics()->SetVSync(State); }
	bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) override { return Graphics()->SetMultiSampling(ReqMultiSamplingCount, MultiSamplingCountBackend); }

	bool Resize(int w, int h, int RefreshRate) override
	{
		// The pixel ratio changes with the page's zoom and between screens.
		const float Scale = emscripten_get_device_pixel_ratio();
		w = std::max(w, 1);
		h = std::max(h, 1);
		if(w == m_Surface.m_WindowWidth && h == m_Surface.m_WindowHeight && (Scale <= 0.0f || Scale == m_Scale))
			return false;
		if(Scale > 0.0f)
			m_Scale = Scale;
		SetSize(w, h);
		Graphics()->Resized(m_Surface);
		return true;
	}
	void ResizeToScreen() override {}
	bool IsScreenKeyboardShown() override { return false; }
	int GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen) override
	{
		if(MaxModes <= 0)
			return 0;
		pModes[0] = Mode();
		return 1;
	}
	void SetWindowGrab(bool Grab) override {}
	void NotifyWindow() override {}
	void Minimize() override {}
	int WindowActive() override { return WebPageVisible() ? 1 : 0; }
	int WindowOpen() override { return 1; }
	void AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc) override {}
	// A page has its own ways of saying what went wrong.
	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override { return std::nullopt; }
};

IEngineGraphicsWindow *CreateWebGraphicsWindow()
{
	return new CGraphicsWindow_Web();
}

// Nothing is shown before or after the window either.
std::optional<int> ShowMessageBoxWithoutGraphics(const IGraphics::CMessageBox &MessageBox)
{
	return std::nullopt;
}
