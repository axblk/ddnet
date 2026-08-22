#include "graphics.h"

#include <engine/shared/config.h>

#include <algorithm>

// helper functions
void CalcViewSize(float Aspect, float Zoom, float MaxAspect, float *pWidth, float *pHeight)
{
	const float Amount = 1150 * 1000;
	const float WMax = 1500;
	const float HMax = 1050;

	// The view keeps its area, so a wider screen is also a shorter one: at 21:9
	// it stands a fifth lower than at 16:9, which is what makes an ultrawide
	// screen look zoomed in instead of wide. Above MaxAspect the height is the
	// one that aspect gives and only the width follows the screen, so the usual
	// screens keep the view they always had.
	const float AreaAspect = MaxAspect > 0.0f ? std::min(Aspect, MaxAspect) : Aspect;

	const float f = std::sqrt(Amount) / std::sqrt(AreaAspect);
	*pWidth = f * AreaAspect;
	*pHeight = f;

	// limit the view
	if(*pWidth > WMax)
	{
		*pWidth = WMax;
		*pHeight = *pWidth / AreaAspect;
	}

	if(*pHeight > HMax)
	{
		*pHeight = HMax;
		*pWidth = *pHeight * AreaAspect;
	}

	// Give back the sides the area computation left out. The client sends the
	// size it ends up with, so the wider view is also the wider snapshot.
	*pWidth = *pHeight * Aspect;

	*pWidth *= Zoom;
	*pHeight *= Zoom;
}

vec2 CalcUncoveredViewSides(float ViewWidth, float ViewCenterX, float FilledCenterX, float FilledHalfWidth)
{
	const float ViewLeft = ViewCenterX - ViewWidth / 2.0f;
	const float ViewRight = ViewCenterX + ViewWidth / 2.0f;
	return vec2(
		std::clamp(FilledCenterX - FilledHalfWidth - ViewLeft, 0.0f, ViewWidth),
		std::clamp(ViewRight - FilledCenterX - FilledHalfWidth, 0.0f, ViewWidth));
}

void IGraphics::CalcScreenParams(float Aspect, float Zoom, float *pWidth, float *pHeight) const
{
	CalcViewSize(Aspect, Zoom, g_Config.m_ClViewMaxAspect / 100.0f, pWidth, pHeight);
}

CScreenRect IGraphics::MapScreenToWorld(float CenterX, float CenterY, float ParallaxX, float ParallaxY,
	float ParallaxZoom, float OffsetX, float OffsetY, float Aspect, float Zoom) const
{
	float Width, Height;
	CalcScreenParams(Aspect, Zoom, &Width, &Height);

	float Scale = (ParallaxZoom * (Zoom - 1.0f) + 100.0f) / 100.0f / Zoom;
	Width *= Scale;
	Height *= Scale;

	CenterX *= ParallaxX / 100.0f;
	CenterY *= ParallaxY / 100.0f;

	return CScreenRect(
		OffsetX + CenterX - Width / 2,
		OffsetY + CenterY - Height / 2,
		Width,
		Height);
}

void IGraphics::MapScreenToInterface(float CenterX, float CenterY, float Zoom)
{
	CScreenRect ScreenRect = MapScreenToWorld(CenterX, CenterY, 100.0f, 100.0f, 100.0f,
		0, 0, ScreenAspect(), Zoom);
	MapScreen(ScreenRect);
}

void IGraphics::MapScreenToSize(float Width, float Height)
{
	MapScreen(CScreenRect(0, 0, Width, Height));
}
