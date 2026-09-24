/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_VIEW_CONTROL_H
#define ENGINE_VIEW_CONTROL_H

#include <base/vmath.h>

#include <engine/client/session.h>

/**
 * How a view draws what the client takes from its settings. The client reads
 * them from `cl_show_direction` and `gfx_high_detail`; a program that shows a
 * demo sets them for its view instead.
 */
class CViewRenderOptions
{
public:
	/**
	 * Whose key presses are shown, as `cl_show_direction`.
	 */
	int m_ShowDirection = 0;
	/**
	 * Whether the map's detail layers are drawn, as `gfx_high_detail`.
	 */
	bool m_HighDetail = true;
};

/**
 * What a program that shows a demo may steer of the view of a session: whom it
 * follows, its camera and how it draws. Everything takes the session whose
 * view it means; a session the game does not know is ignored.
 */
class IViewControl
{
public:
	virtual ~IViewControl() = default;

	/**
	 * @return Whom a demo is watched from: a client id, `SPEC_FREEVIEW`, or
	 * `SPEC_FOLLOW` for whoever recorded it.
	 */
	virtual int SpectatorId(CSessionId SessionId) const = 0;
	/**
	 * Changes whom a demo is watched from, see `SpectatorId`. A paused demo
	 * shows the change at once.
	 */
	virtual void SetSpectatorId(CSessionId SessionId, int SpectatorId) = 0;
	/**
	 * The name of a player of the session.
	 *
	 * @return `false` where there is no player with that id.
	 */
	virtual bool PlayerName(CSessionId SessionId, int ClientId, char *pBuffer, int BufferSize) const = 0;

	/**
	 * Moves the free view by an offset in world units. Does nothing while a
	 * player is followed.
	 */
	virtual void MoveFreeView(CSessionId SessionId, vec2 Offset) = 0;
	virtual float Zoom(CSessionId SessionId) const = 0;
	/**
	 * Multiplies the zoom, as a notch of the mouse wheel does.
	 */
	virtual void ScaleZoom(CSessionId SessionId, float Factor) = 0;
	/**
	 * Puts the zoom back to `cl_default_zoom`.
	 */
	virtual void ResetZoom(CSessionId SessionId) = 0;
	/**
	 * @return Whether somebody zoomed away from `cl_default_zoom`.
	 */
	virtual bool ZoomChanged(CSessionId SessionId) const = 0;
	/**
	 * @return Whether the demo brought a camera of its own, which it does
	 * while it follows whoever recorded it and they were spectating.
	 */
	virtual bool RecordedCameraAvailable(CSessionId SessionId) const = 0;
	/**
	 * @return Whether the camera the demo brought is the one in use.
	 */
	virtual bool RecordedCamera(CSessionId SessionId) const = 0;
	virtual void SetRecordedCamera(CSessionId SessionId, bool Use) = 0;

	virtual CViewRenderOptions RenderOptions(CSessionId SessionId) const = 0;
	virtual void SetRenderOptions(CSessionId SessionId, const CViewRenderOptions &Options) = 0;
};

#endif // ENGINE_VIEW_CONTROL_H
