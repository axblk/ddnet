/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "viewer_fullscreen.h"

#include <base/detect.h>

#include <engine/graphics_window.h>
#include <engine/shared/config.h>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten.h>
#endif

// In a browser this goes through the page's loader, which the loader hands
// over at start. Without it there is no full screen. The comparisons are the
// loose ones because the formatter splits `!==`.
namespace ViewerFullscreen
{

	bool Supported(IGraphicsWindow *pWindow)
	{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		(void)pWindow;
		return EM_ASM_INT({
			return !Module.ddnetFullscreen || !Module.ddnetFullscreen.supported() ? 0 : 1;
		}) != 0;
#else
		return pWindow != nullptr;
#endif
	}

	bool Active(IGraphicsWindow *pWindow)
	{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		(void)pWindow;
		return EM_ASM_INT({
			return !Module.ddnetFullscreen || !Module.ddnetFullscreen.active() ? 0 : 1;
		}) != 0;
#else
		// What was last asked for; leaving through the window system is not seen.
		return pWindow != nullptr && g_Config.m_GfxFullscreen != 0;
#endif
	}

	void Toggle(IGraphicsWindow *pWindow)
	{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		(void)pWindow;
		EM_ASM({
			if(Module.ddnetFullscreen)
			{
				Module.ddnetFullscreen.toggle();
			}
		});
#else
		// The whole screen at the resolution it already has.
		if(pWindow == nullptr)
			return;
		pWindow->SetWindowParams(Active(pWindow) ? 0 : 2, g_Config.m_GfxBorderless != 0);
#endif
	}

} // namespace ViewerFullscreen
