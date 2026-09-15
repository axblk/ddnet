/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VIEWER_FULLSCREEN_H
#define ENGINE_CLIENT_VIEWER_FULLSCREEN_H

class IGraphicsWindow;

/**
 * Filling the screen with what a viewer shows.
 *
 * A viewer draws its own controls, so it needs its own way of asking for the
 * screen; the client asks through its settings, which a viewer does not have.
 *
 * Where that is asked is not the same everywhere. A program on a desktop tells
 * its window what to be; a program in a browser tells the page, because the
 * window there is the page and the rules for filling the screen with it - it
 * only counts while what the user did still counts, and the shape of the
 * screen may be asked for at the same time - belong to the page's loader,
 * which is where they already are.
 */
namespace ViewerFullscreen
{

	/** Whether this can be asked for at all. A button that cannot is not shown. */
	bool Supported(IGraphicsWindow *pWindow);

	/** Whether the screen is filled right now. */
	bool Active(IGraphicsWindow *pWindow);

	/**
	 * Fills the screen, or stops filling it. Called from the frame that read the
	 * press, which in a browser is still close enough to the press to count.
	 */
	void Toggle(IGraphicsWindow *pWindow);

} // namespace ViewerFullscreen

#endif // ENGINE_CLIENT_VIEWER_FULLSCREEN_H
