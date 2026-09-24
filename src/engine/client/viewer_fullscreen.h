/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VIEWER_FULLSCREEN_H
#define ENGINE_CLIENT_VIEWER_FULLSCREEN_H

class IGraphicsWindow;

/**
 * Filling the screen with a viewer, which has no settings to ask the client's
 * way. In a browser the page's loader does it.
 */
namespace ViewerFullscreen
{

	/** Whether this can be asked for at all. A button that cannot is not shown. */
	bool Supported(IGraphicsWindow *pWindow);

	/** Whether the screen is filled right now. */
	bool Active(IGraphicsWindow *pWindow);

	/**
	 * Fills the screen, or stops filling it. Called from the frame that read the
	 * press, which in a browser still counts as the user's action.
	 */
	void Toggle(IGraphicsWindow *pWindow);

} // namespace ViewerFullscreen

#endif // ENGINE_CLIENT_VIEWER_FULLSCREEN_H
