#ifndef ENGINE_GRAPHICS_WINDOW_H
#define ENGINE_GRAPHICS_WINDOW_H

#include <base/vmath.h>

#include <engine/graphics.h>
#include <engine/kernel.h>

#include <cstdint>
#include <optional>

// The window the game draws into, as opposed to the drawing itself: which
// screen it is on, how big it is, whether it is fullscreen, whether it has
// the focus. The graphics never see any of this; the window owns the surface
// and hands the graphics a backend that draws into it. Code that only draws
// asks for IGraphics, code that touches the window asks for this.
//
// The surface-less client has a window too - one that is not there. It
// answers every question with the size of the virtual screen and does
// nothing when asked to move.
class IGraphicsWindow : public IInterface
{
	MACRO_INTERFACE("graphicswindow")
public:
	virtual void SetWindowParams(int FullscreenMode, bool IsBorderless) = 0;
	virtual bool SetWindowScreen(int Index, bool MoveToCenter) = 0;
	virtual bool SwitchWindowScreen(int Index, bool MoveToCenter) = 0;
	virtual int GetWindowScreen() = 0;
	virtual int GetNumScreens() const = 0;
	virtual const char *GetScreenName(int Screen) const = 0;
	virtual bool SetVSync(bool State) = 0;
	virtual bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) = 0;
	virtual void Move(int x, int y) = 0;
	virtual bool Resize(int w, int h, int RefreshRate) = 0;
	virtual void ResizeToScreen() = 0;
	virtual bool IsScreenKeyboardShown() = 0;
	virtual int GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen) = 0;
	virtual void GetCurrentVideoMode(CVideoMode &CurMode, int Screen) = 0;
	virtual void SetWindowGrab(bool Grab) = 0;
	virtual void NotifyWindow() = 0;
	virtual void Minimize() = 0;
	virtual int WindowActive() = 0;
	virtual int WindowOpen() = 0;

	/**
	 * Listens to window property changes: minimize, maximize, move,
	 * fullscreen mode, the screen, the DPI scale.
	 */
	virtual void AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc) = 0;

	/**
	 * Shows a modal message box. The window is destroyed first, so this only
	 * makes sense for fatal errors.
	 *
	 * @return The index of the pressed button, or nothing if the box could
	 * not be shown.
	 */
	virtual std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) = 0;
};

class IGraphicsBackend;

// What the client needs from the window beyond what the game sees: opening
// and closing it, and the events the window system delivers.
class IEngineGraphicsWindow : public IGraphicsWindow
{
	MACRO_INTERFACE("enginegraphicswindow")
public:
	/**
	 * Opens the window and the graphics backend that draws into it, trying
	 * lesser settings until one works. The backend belongs to the caller,
	 * who hands it to the graphics; the window keeps the surface and closes
	 * it after the graphics have shut down.
	 *
	 * @return The initialized backend, or null when nothing worked.
	 */
	virtual IGraphicsBackend *Open(bool Hidden) = 0;
	virtual const SGraphicsSurfaceInfo &Surface() const = 0;
	virtual void Close() = 0;

	// Delivered by whoever pumps the window system's events.
	virtual void OnMoved(int x, int y) = 0;
	virtual void OnSizeChanged(int w, int h) = 0;
	virtual void OnDisplayChanged(int Display) = 0;
	// The window went away (Android takes it when the app goes to the
	// background) and came back, possibly as a different one.
	virtual void OnWindowDestroyed() = 0;
	virtual void OnWindowCreated(uint32_t WindowId) = 0;

	// Called once per client loop. Rotating an iOS device by 180 degrees
	// moves the display cutout to the other side without changing the window
	// size, so there is no resize event to learn it from.
	virtual void Update() {}
};

// A window that is not there: the surface-less client draws into targets
// only, so this answers with the configured size and never moves.
extern IEngineGraphicsWindow *CreateOffscreenGraphicsWindow();

/**
 * Shows a message box before there is a window, or after showing one through
 * the window failed.
 */
extern std::optional<int> ShowMessageBoxWithoutGraphics(const IGraphics::CMessageBox &MessageBox);

#endif
