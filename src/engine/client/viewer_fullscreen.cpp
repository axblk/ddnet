/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "viewer_fullscreen.h"

#include <base/detect.h>

#include <engine/graphics_window.h>
#include <engine/shared/config.h>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten.h>
#endif

// In a browser this goes through the page's loader, the same way the pages'
// own buttons do: what a browser will do about filling the screen, and what
// else to ask for while doing it, is knowledge that belongs there. The loader
// hands it over when it starts the program - it is a module and claims no
// global name for anything to look up. A page put together without it has no
// full screen, and asking for one then does nothing rather than throwing.
//
// The comparisons in that JavaScript are the loose ones. This is C++ as far as
// the style formatter is concerned, and it writes `!==` as `!= =`.
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
		// What was last asked for. Somebody who leaves the full screen the way the
		// window system offers rather than the way this does has told the window
		// and not us, so the button then says the wrong thing until it is pressed.
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
		// The whole screen at the resolution it already has: a viewer draws for
		// the window it is in, so there is nothing to be gained by making the
		// screen do something else.
		if(pWindow == nullptr)
			return;
		pWindow->SetWindowParams(Active(pWindow) ? 0 : 2, g_Config.m_GfxBorderless != 0);
#endif
	}

} // namespace ViewerFullscreen
