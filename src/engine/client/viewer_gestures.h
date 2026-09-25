/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VIEWER_GESTURES_H
#define ENGINE_CLIENT_VIEWER_GESTURES_H

#include <base/vmath.h>

#include <engine/input.h>

#include <vector>

/**
 * Pinching and dragging with two fingers, for the viewers. One finger is a
 * tap or a drag and left to the caller.
 */
class CViewerGestures
{
public:
	struct SResult
	{
		/** What to multiply the zoom by. Above one shows more of the world. */
		float m_Zoom = 1.0f;
		/** How far the picture was dragged, in the pixels that are drawn. */
		vec2 m_Move = vec2(0.0f, 0.0f);
		/**
		 * Whether two fingers are down. The first one is also reported as a
		 * pointer, which should then not move the view as well.
		 */
		bool m_Active = false;
	};

	/**
	 * @param vFingers The fingers on the screen, as the input reports them.
	 * @param ScreenSize How big the picture is, in the pixels that are drawn.
	 */
	SResult Update(const std::vector<IInput::CTouchFingerState> &vFingers, vec2 ScreenSize);

private:
	bool m_Pinching = false;
	float m_Distance = 0.0f;
	vec2 m_Middle = vec2(0.0f, 0.0f);
};

#endif
