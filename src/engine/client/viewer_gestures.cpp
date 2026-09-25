/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "viewer_gestures.h"

#include <algorithm>

CViewerGestures::SResult CViewerGestures::Update(const std::vector<IInput::CTouchFingerState> &vFingers, vec2 ScreenSize)
{
	if(vFingers.size() < 2)
	{
		m_Pinching = false;
		return SResult();
	}

	const vec2 First = vFingers[0].m_Position * ScreenSize;
	const vec2 Second = vFingers[1].m_Position * ScreenSize;
	const float Distance = std::max(distance(First, Second), 1.0f);
	const vec2 Middle = (First + Second) / 2.0f;

	SResult Result;
	Result.m_Active = true;
	if(m_Pinching)
	{
		// Fingers moving apart show less of the world.
		Result.m_Zoom = m_Distance / Distance;
		Result.m_Move = Middle - m_Middle;
	}
	m_Pinching = true;
	m_Distance = Distance;
	m_Middle = Middle;
	return Result;
}
