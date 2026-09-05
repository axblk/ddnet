#include "graphics.h"

#include <base/dbg.h>

#include <engine/shared/config.h>

#include <algorithm>
#include <iterator>

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

// The vertex builders. Everything between a Begin and an End writes into
// the vertex buffer that IGraphics itself holds, so none of it needs a
// backend and none of it is virtual; only the flush at the end is, and a
// buffer that runs full flushes on its own.

static unsigned char NormalizeColorComponent(float ColorComponent)
{
	return (unsigned char)(std::clamp(ColorComponent, 0.0f, 1.0f) * 255.0f + 0.5f); // +0.5f to round to nearest
}

void IGraphics::AddVertices(int Count)
{
	m_NumVertices += Count;
	if((m_NumVertices + Count) >= MAX_VERTICES)
		FlushVertices();
}

void IGraphics::AddVertices(int Count, SGraphicsVertex *pVertices)
{
	AddVertices(Count);
}

void IGraphics::AddVertices(int Count, SGraphicsVertexTex3DStream *pVertices)
{
	m_NumVertices += Count;
	if((m_NumVertices + Count) >= MAX_VERTICES)
		FlushVerticesTex3D();
}

void IGraphics::LinesBegin()
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->LinesBegin twice");
	m_Drawing = EDrawing::LINES;
	SetColor(1, 1, 1, 1);
}

void IGraphics::LinesEnd()
{
	dbg_assert(m_Drawing == EDrawing::LINES, "called Graphics()->LinesEnd without begin");
	FlushVertices();
	m_Drawing = EDrawing::NONE;
}

void IGraphics::LinesDraw(const CLineItem *pArray, size_t Num)
{
	dbg_assert(m_Drawing == EDrawing::LINES, "called Graphics()->LinesDraw without begin");

	size_t VertexIndex = m_NumVertices;
	for(size_t i = 0; i < Num; ++i)
	{
		m_aVertices[VertexIndex].m_Pos.x = pArray[i].m_X0;
		m_aVertices[VertexIndex].m_Pos.y = pArray[i].m_Y0;
		m_aVertices[VertexIndex].m_Tex = m_aTexture[0];
		SetColor(&m_aVertices[VertexIndex], 0);
		++VertexIndex;

		m_aVertices[VertexIndex].m_Pos.x = pArray[i].m_X1;
		m_aVertices[VertexIndex].m_Pos.y = pArray[i].m_Y1;
		m_aVertices[VertexIndex].m_Tex = m_aTexture[1];
		SetColor(&m_aVertices[VertexIndex], 1);
		++VertexIndex;
	}

	AddVertices(2 * Num);
}

void IGraphics::LinesBatchBegin(CLineItemBatch *pBatch)
{
	pBatch->m_NumItems = 0;
	LinesBegin();
}

void IGraphics::LinesBatchEnd(CLineItemBatch *pBatch)
{
	if(pBatch->m_NumItems > 0)
	{
		LinesDraw(pBatch->m_aItems, pBatch->m_NumItems);
		pBatch->m_NumItems = 0;
	}
	LinesEnd();
}

void IGraphics::LinesBatchDraw(CLineItemBatch *pBatch, const CLineItem *pArray, size_t Num)
{
	if(pBatch->m_NumItems + Num >= std::size(pBatch->m_aItems))
	{
		LinesDraw(pBatch->m_aItems, pBatch->m_NumItems);
		pBatch->m_NumItems = 0;
	}
	if(Num >= std::size(pBatch->m_aItems))
	{
		LinesDraw(pArray, Num);
		return;
	}
	std::copy(pArray, pArray + Num, pBatch->m_aItems + pBatch->m_NumItems);
	pBatch->m_NumItems += Num;
}

void IGraphics::QuadsBegin()
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->QuadsBegin twice");
	m_Drawing = EDrawing::QUADS;

	QuadsSetSubset(0, 0, 1, 1);
	QuadsSetRotation(0);
	SetColor(1, 1, 1, 1);
}

void IGraphics::QuadsEnd()
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsEnd without begin");
	FlushVertices();
	m_Drawing = EDrawing::NONE;
}

void IGraphics::QuadsTex3DBegin()
{
	QuadsBegin();
}

void IGraphics::QuadsTex3DEnd()
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsEnd without begin");
	FlushVerticesTex3D();
	m_Drawing = EDrawing::NONE;
}

void IGraphics::TrianglesBegin()
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->TrianglesBegin twice");
	m_Drawing = EDrawing::TRIANGLES;

	QuadsSetSubset(0, 0, 1, 1);
	QuadsSetRotation(0);
	SetColor(1, 1, 1, 1);
}

void IGraphics::TrianglesEnd()
{
	dbg_assert(m_Drawing == EDrawing::TRIANGLES, "called Graphics()->TrianglesEnd without begin");
	FlushVertices();
	m_Drawing = EDrawing::NONE;
}

void IGraphics::QuadsEndKeepVertices()
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsEndKeepVertices without begin");
	FlushVertices(true);
	m_Drawing = EDrawing::NONE;
}

void IGraphics::QuadsDrawCurrentVertices(bool KeepVertices)
{
	m_Drawing = EDrawing::QUADS;
	FlushVertices(KeepVertices);
	m_Drawing = EDrawing::NONE;
}

void IGraphics::QuadsSetRotation(float Angle)
{
	m_Rotation = Angle;
}

static SGraphicsColor NormalizeColor(ColorRGBA Color)
{
	SGraphicsColor NormalizedColor;
	NormalizedColor.r = NormalizeColorComponent(Color.r);
	NormalizedColor.g = NormalizeColorComponent(Color.g);
	NormalizedColor.b = NormalizeColorComponent(Color.b);
	NormalizedColor.a = NormalizeColorComponent(Color.a);
	return NormalizedColor;
}

void IGraphics::SetColor(float r, float g, float b, float a)
{
	SetColor(ColorRGBA(r, g, b, a));
}

void IGraphics::SetColor(ColorRGBA Color)
{
	std::fill(std::begin(m_aColor), std::end(m_aColor), NormalizeColor(Color));
}

void IGraphics::SetColor2(ColorRGBA First, ColorRGBA Second)
{
	dbg_assert(m_Drawing == EDrawing::LINES, "Called Graphics()->SetColor2 while not drawing lines");

	m_aColor[0] = NormalizeColor(First);
	m_aColor[1] = NormalizeColor(Second);
}

void IGraphics::SetColor4(ColorRGBA TopLeft, ColorRGBA TopRight, ColorRGBA BottomLeft, ColorRGBA BottomRight)
{
	dbg_assert(m_Drawing == EDrawing::QUADS || m_Drawing == EDrawing::TRIANGLES, "Called Graphics()->SetColor4 while not drawing quads or triangles");

	m_aColor[0] = NormalizeColor(TopLeft);
	m_aColor[1] = NormalizeColor(TopRight);
	m_aColor[2] = NormalizeColor(BottomRight);
	m_aColor[3] = NormalizeColor(BottomLeft);
}

void IGraphics::ChangeColorOfQuadVertices(size_t QuadOffset, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	const SGraphicsColor Color(r, g, b, a);
	const size_t VertNum = 4;
	for(size_t i = 0; i < VertNum; ++i)
	{
		m_aVertices[QuadOffset * VertNum + i].m_Color = Color;
	}
}

void IGraphics::QuadsSetSubset(float TlU, float TlV, float BrU, float BrV)
{
	m_aTexture[0].u = TlU;
	m_aTexture[1].u = BrU;
	m_aTexture[0].v = TlV;
	m_aTexture[1].v = TlV;

	m_aTexture[3].u = TlU;
	m_aTexture[2].u = BrU;
	m_aTexture[3].v = BrV;
	m_aTexture[2].v = BrV;
}

void IGraphics::QuadsSetSubsetFree(
	float x0, float y0, float x1, float y1,
	float x2, float y2, float x3, float y3, int Index)
{
	m_aTexture[0].u = x0;
	m_aTexture[0].v = y0;
	m_aTexture[1].u = x1;
	m_aTexture[1].v = y1;
	m_aTexture[2].u = x2;
	m_aTexture[2].v = y2;
	m_aTexture[3].u = x3;
	m_aTexture[3].v = y3;
	m_CurIndex = Index;
}

void IGraphics::QuadsDraw(CQuadItem *pArray, int Num)
{
	for(int i = 0; i < Num; ++i)
	{
		pArray[i].m_X -= pArray[i].m_Width / 2;
		pArray[i].m_Y -= pArray[i].m_Height / 2;
	}

	QuadsDrawTL(pArray, Num);
}

void IGraphics::QuadsDrawTL(const CQuadItem *pArray, int Num)
{
	QuadsDrawTLImpl(m_aVertices, pArray, Num);
}

void IGraphics::QuadsDrawFreeform(const CFreeformItem *pArray, int Num)
{
	dbg_assert(m_Drawing == EDrawing::QUADS || m_Drawing == EDrawing::TRIANGLES, "called Graphics()->QuadsDrawFreeform without begin");

	if(m_Drawing == EDrawing::TRIANGLES)
	{
		for(int i = 0; i < Num; ++i)
		{
			m_aVertices[m_NumVertices + 6 * i].m_Pos.x = pArray[i].m_X0;
			m_aVertices[m_NumVertices + 6 * i].m_Pos.y = pArray[i].m_Y0;
			m_aVertices[m_NumVertices + 6 * i].m_Tex = m_aTexture[0];
			SetColor(&m_aVertices[m_NumVertices + 6 * i], 0);

			m_aVertices[m_NumVertices + 6 * i + 1].m_Pos.x = pArray[i].m_X1;
			m_aVertices[m_NumVertices + 6 * i + 1].m_Pos.y = pArray[i].m_Y1;
			m_aVertices[m_NumVertices + 6 * i + 1].m_Tex = m_aTexture[1];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 1], 1);

			m_aVertices[m_NumVertices + 6 * i + 2].m_Pos.x = pArray[i].m_X3;
			m_aVertices[m_NumVertices + 6 * i + 2].m_Pos.y = pArray[i].m_Y3;
			m_aVertices[m_NumVertices + 6 * i + 2].m_Tex = m_aTexture[3];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 2], 3);

			m_aVertices[m_NumVertices + 6 * i + 3].m_Pos.x = pArray[i].m_X0;
			m_aVertices[m_NumVertices + 6 * i + 3].m_Pos.y = pArray[i].m_Y0;
			m_aVertices[m_NumVertices + 6 * i + 3].m_Tex = m_aTexture[0];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 3], 0);

			m_aVertices[m_NumVertices + 6 * i + 4].m_Pos.x = pArray[i].m_X3;
			m_aVertices[m_NumVertices + 6 * i + 4].m_Pos.y = pArray[i].m_Y3;
			m_aVertices[m_NumVertices + 6 * i + 4].m_Tex = m_aTexture[3];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 4], 3);

			m_aVertices[m_NumVertices + 6 * i + 5].m_Pos.x = pArray[i].m_X2;
			m_aVertices[m_NumVertices + 6 * i + 5].m_Pos.y = pArray[i].m_Y2;
			m_aVertices[m_NumVertices + 6 * i + 5].m_Tex = m_aTexture[2];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 5], 2);
		}

		AddVertices(3 * 2 * Num);
	}
	else
	{
		for(int i = 0; i < Num; ++i)
		{
			m_aVertices[m_NumVertices + 4 * i].m_Pos.x = pArray[i].m_X0;
			m_aVertices[m_NumVertices + 4 * i].m_Pos.y = pArray[i].m_Y0;
			m_aVertices[m_NumVertices + 4 * i].m_Tex = m_aTexture[0];
			SetColor(&m_aVertices[m_NumVertices + 4 * i], 0);

			m_aVertices[m_NumVertices + 4 * i + 1].m_Pos.x = pArray[i].m_X1;
			m_aVertices[m_NumVertices + 4 * i + 1].m_Pos.y = pArray[i].m_Y1;
			m_aVertices[m_NumVertices + 4 * i + 1].m_Tex = m_aTexture[1];
			SetColor(&m_aVertices[m_NumVertices + 4 * i + 1], 1);

			m_aVertices[m_NumVertices + 4 * i + 2].m_Pos.x = pArray[i].m_X3;
			m_aVertices[m_NumVertices + 4 * i + 2].m_Pos.y = pArray[i].m_Y3;
			m_aVertices[m_NumVertices + 4 * i + 2].m_Tex = m_aTexture[3];
			SetColor(&m_aVertices[m_NumVertices + 4 * i + 2], 3);

			m_aVertices[m_NumVertices + 4 * i + 3].m_Pos.x = pArray[i].m_X2;
			m_aVertices[m_NumVertices + 4 * i + 3].m_Pos.y = pArray[i].m_Y2;
			m_aVertices[m_NumVertices + 4 * i + 3].m_Tex = m_aTexture[2];
			SetColor(&m_aVertices[m_NumVertices + 4 * i + 3], 2);
		}

		AddVertices(4 * Num);
	}
}

void IGraphics::QuadsText(float x, float y, float Size, const char *pText)
{
	float StartX = x;

	while(*pText)
	{
		char c = *pText;
		pText++;

		if(c == '\n')
		{
			x = StartX;
			y += Size;
		}
		else
		{
			QuadsSetSubset(
				(c % 16) / 16.0f,
				(c / 16) / 16.0f,
				(c % 16) / 16.0f + 1.0f / 16.0f,
				(c / 16) / 16.0f + 1.0f / 16.0f);

			CQuadItem QuadItem(x, y, Size, Size);
			QuadsDrawTL(&QuadItem, 1);
			x += Size / 2;
		}
	}
}
