/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "viewer_controls.h"

#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/textrender.h>

#include <algorithm>
#include <vector>

namespace
{
	// How the bar is put together, in pixels of the window. A bar over a picture
	// has to be readable without being what the picture is about, so it is small,
	// dark and only as wide as what is on it.
	constexpr float BAR_HEIGHT = 40.0f;
	constexpr float BAR_MARGIN = 20.0f;
	constexpr float ITEM_HEIGHT = 28.0f;
	constexpr float ITEM_SPACING = 6.0f;
	constexpr float ITEM_PADDING = 10.0f;
	constexpr float SQUARE_WIDTH = 32.0f;
	constexpr float TEXT_SIZE = 14.0f;
	constexpr float ICON_SIZE = 12.0f;
	// How long the bar stays after the last thing that happened, as a video
	// player's does: long enough to reach it, short enough to be out of the way.
	constexpr std::chrono::milliseconds SHOWN_FOR(2500);
	constexpr std::chrono::milliseconds FADE_FOR(300);
} // namespace

void CViewerControls::Init(IGraphics *pGraphics, ITextRender *pTextRender)
{
	m_pGraphics = pGraphics;
	m_pTextRender = pTextRender;
}

void CViewerControls::Show()
{
	m_ShownUntil = time_get_nanoseconds() + SHOWN_FOR;
}

void CViewerControls::DrawRect(float x, float y, float w, float h, float r, float g, float b, float a)
{
	m_pGraphics->SetColor(r, g, b, a);
	IGraphics::CQuadItem Quad(x, y, w, h);
	m_pGraphics->QuadsDrawTL(&Quad, 1);
}

// Everything a button shows that is not a letter, out of rectangles: the one
// thing that is drawn here without a font, and the only thing the map viewer
// has. A triangle is a stack of rows, which at this size is a triangle.
void CViewerControls::DrawIcon(EIcon Icon, vec2 Center, float Size, float Alpha)
{
	const float Half = Size / 2.0f;
	const float Thin = std::max(2.0f, Size / 6.0f);
	const auto &&Triangle = [&](float Direction) {
		constexpr int Rows = 8;
		for(int i = 0; i < Rows; ++i)
		{
			const float Fraction = (i + 0.5f) / Rows;
			const float RowHeight = Size / Rows;
			const float Length = Size * (1.0f - std::abs(Fraction * 2.0f - 1.0f));
			const float Y = Center.y - Half + i * RowHeight;
			const float X = Direction > 0.0f ? Center.x - Half : Center.x + Half - Length;
			DrawRect(X, Y, Length, RowHeight + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
		}
	};
	switch(Icon)
	{
	case EIcon::NONE:
		break;
	case EIcon::PLAY:
		Triangle(1.0f);
		break;
	case EIcon::PAUSE:
		DrawRect(Center.x - Half, Center.y - Half, Thin, Size, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x + Half - Thin, Center.y - Half, Thin, Size, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::RESTART:
		DrawRect(Center.x - Half, Center.y - Half, Thin, Size, 1.0f, 1.0f, 1.0f, Alpha);
		Triangle(-1.0f);
		break;
	case EIcon::MINUS:
		DrawRect(Center.x - Half, Center.y - Thin / 2.0f, Size, Thin, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::PLUS:
		DrawRect(Center.x - Half, Center.y - Thin / 2.0f, Size, Thin, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x - Thin / 2.0f, Center.y - Half, Thin, Size, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::FIT:
	{
		// Four corners of a frame, which is what fitting something into a
		// window looks like when there is no room to write it.
		const float Arm = Size / 2.5f;
		for(int Corner = 0; Corner < 4; ++Corner)
		{
			const float DirX = (Corner & 1) == 0 ? 1.0f : -1.0f;
			const float DirY = (Corner & 2) == 0 ? 1.0f : -1.0f;
			const float X = Center.x + (DirX > 0.0f ? -Half : Half - Thin);
			const float Y = Center.y + (DirY > 0.0f ? -Half : Half - Thin);
			DrawRect(DirX > 0.0f ? X : X + Thin - Arm, Y, Arm, Thin, 1.0f, 1.0f, 1.0f, Alpha);
			DrawRect(X, DirY > 0.0f ? Y : Y + Thin - Arm, Thin, Arm, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::SAVE:
	case EIcon::SAVE_ALL:
	{
		// An arrow into a tray, which is what every browser draws for a file
		// on its way somewhere. Two of them side by side for all of it.
		const int Count = Icon == EIcon::SAVE ? 1 : 2;
		const float Width = Size / Count - (Count > 1 ? Thin : 0.0f);
		for(int i = 0; i < Count; ++i)
		{
			const float X = Center.x - Half + i * (Width + Thin);
			DrawRect(X + Width / 2.0f - Thin / 2.0f, Center.y - Half, Thin, Size * 0.45f, 1.0f, 1.0f, 1.0f, Alpha);
			constexpr int Rows = 5;
			for(int Row = 0; Row < Rows; ++Row)
			{
				const float Length = Width * (1.0f - Row / (float)Rows);
				DrawRect(X + (Width - Length) / 2.0f, Center.y - Half + Size * 0.45f + Row * (Size * 0.25f / Rows),
					Length, Size * 0.25f / Rows + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
			}
			DrawRect(X, Center.y + Half - Thin, Width, Thin, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::STOP:
		DrawRect(Center.x - Half, Center.y - Half, Size, Size, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::EYE:
	{
		// A lens with a pupil in it, which is what watching looks like: rows
		// that grow and shrink again, and a dark square in the middle of them.
		constexpr int Rows = 8;
		for(int i = 0; i < Rows; ++i)
		{
			const float Fraction = (i + 0.5f) / Rows;
			const float RowHeight = Size / Rows;
			const float Length = Size * (1.0f - std::abs(Fraction * 2.0f - 1.0f));
			DrawRect(Center.x - Length / 2.0f, Center.y - Half / 2.0f + i * RowHeight / 2.0f, Length, RowHeight / 2.0f + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
		}
		DrawRect(Center.x - Thin / 2.0f, Center.y - Thin / 2.0f, Thin, Thin, 0.0f, 0.0f, 0.0f, Alpha);
		break;
	}
	}
}

int CViewerControls::Render(const SItem *pItems, size_t Count, const SInput &Input, float *pSliderValue)
{
	if(m_pGraphics == nullptr || Count == 0)
	{
		return -1;
	}

	// The pointer is where the window system says it is, which is not where
	// the frame is drawn: on a screen that draws more pixels than it is wide
	// in window units, everything here would be at half the distance from
	// everything else. The same conversion the client's console makes.
	const vec2 Mouse = Input.m_MousePos * m_pGraphics->ScreenHiDPIScale();

	// Somebody being there is the pointer moving, a key going down, or the
	// button being held - the last one because dragging a slider is somebody
	// using this and nothing else is moving while they do.
	const bool Moved = distance(Mouse, m_LastMousePos) > 1.0f;
	m_LastMousePos = Mouse;
	if(Moved || Input.m_KeyPressed || Input.m_MousePressed)
	{
		Show();
	}

	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Now > m_ShownUntil + FADE_FOR)
	{
		// Gone, and out of the way of the pointer with it: a bar nobody can
		// see must not be a bar that swallows a click on the picture.
		m_Hovered = false;
		m_Dragging = -1;
		m_WasPressed = Input.m_MousePressed;
		return -1;
	}
	const float Alpha = Now <= m_ShownUntil ? 1.0f : 1.0f - std::chrono::duration_cast<std::chrono::duration<float>>(Now - m_ShownUntil).count() / std::chrono::duration_cast<std::chrono::duration<float>>(FADE_FOR).count();

	const float ScreenWidth = m_pGraphics->ScreenWidth();
	const float ScreenHeight = m_pGraphics->ScreenHeight();

	// How wide everything is, so that the bar can be as wide as its contents
	// and in the middle of the window.
	std::vector<float> vWidths(Count);
	float Total = 0.0f;
	for(size_t i = 0; i < Count; ++i)
	{
		float Width = pItems[i].m_Width;
		if(Width <= 0.0f)
		{
			if(m_pTextRender != nullptr && pItems[i].m_pText != nullptr)
				Width = m_pTextRender->TextWidth(TEXT_SIZE, pItems[i].m_pText) + 2.0f * ITEM_PADDING;
			else
				Width = SQUARE_WIDTH;
		}
		vWidths[i] = Width;
		Total += Width + (i + 1 < Count ? ITEM_SPACING : 0.0f);
	}
	const float BarWidth = std::min(Total + 2.0f * ITEM_PADDING, ScreenWidth - 2.0f * BAR_MARGIN);
	const float BarLeft = (ScreenWidth - BarWidth) / 2.0f;
	const float BarTop = ScreenHeight - BAR_MARGIN - BAR_HEIGHT;

	m_pGraphics->MapScreen(CScreenRect(0.0f, 0.0f, ScreenWidth, ScreenHeight));
	m_pGraphics->TextureClear();
	m_pGraphics->QuadsBegin();
	DrawRect(BarLeft, BarTop, BarWidth, BAR_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f * Alpha);

	m_Hovered = Mouse.x >= BarLeft && Mouse.x <= BarLeft + BarWidth && Mouse.y >= BarTop && Mouse.y <= BarTop + BAR_HEIGHT;
	const bool Clicked = Input.m_MousePressed && !m_WasPressed;
	if(!Input.m_MousePressed)
	{
		m_Dragging = -1;
	}
	m_WasPressed = Input.m_MousePressed;

	int Pressed = -1;
	float x = BarLeft + ITEM_PADDING;
	const float ItemTop = BarTop + (BAR_HEIGHT - ITEM_HEIGHT) / 2.0f;
	struct SLabel
	{
		float m_X;
		float m_Y;
		const char *m_pText;
		float m_Alpha;
	};
	std::vector<SLabel> vLabels;
	for(size_t i = 0; i < Count; ++i)
	{
		const SItem &Item = pItems[i];
		const float Width = vWidths[i];
		const bool Over = !Item.m_Disabled && Mouse.x >= x && Mouse.x <= x + Width && Mouse.y >= ItemTop && Mouse.y <= ItemTop + ITEM_HEIGHT;
		switch(Item.m_Type)
		{
		case EItem::BUTTON:
		{
			const float Shade = Item.m_Disabled ? 0.1f : (Over ? 0.45f : 0.25f);
			DrawRect(x, ItemTop, Width, ITEM_HEIGHT, Shade, Shade, Shade, 0.9f * Alpha);
			if(Item.m_Icon != EIcon::NONE)
			{
				const bool WithText = m_pTextRender != nullptr && Item.m_pText != nullptr;
				const float IconX = WithText ? x + ITEM_PADDING + ICON_SIZE / 2.0f : x + Width / 2.0f;
				DrawIcon(Item.m_Icon, vec2(IconX, ItemTop + ITEM_HEIGHT / 2.0f), ICON_SIZE, (Item.m_Disabled ? 0.4f : 1.0f) * Alpha);
			}
			if(Over && Clicked)
			{
				Pressed = (int)i;
			}
			break;
		}
		case EItem::SLIDER:
		{
			constexpr float TrackHeight = 6.0f;
			const float TrackTop = ItemTop + (ITEM_HEIGHT - TrackHeight) / 2.0f;
			DrawRect(x, TrackTop, Width, TrackHeight, 0.25f, 0.25f, 0.25f, 0.9f * Alpha);
			const float Value = std::clamp(Item.m_Value, 0.0f, 1.0f);
			DrawRect(x, TrackTop, Width * Value, TrackHeight, 1.0f, 0.65f, 0.0f, 0.95f * Alpha);
			DrawRect(x + Width * Value - 3.0f, ItemTop + 4.0f, 6.0f, ITEM_HEIGHT - 8.0f, 1.0f, 1.0f, 1.0f, 0.95f * Alpha);
			if(Over && Clicked)
			{
				m_Dragging = (int)i;
			}
			if(m_Dragging == (int)i && pSliderValue != nullptr)
			{
				*pSliderValue = std::clamp((Mouse.x - x) / std::max(Width, 1.0f), 0.0f, 1.0f);
				Pressed = (int)i;
			}
			break;
		}
		case EItem::TEXT:
			break;
		}
		if(m_pTextRender != nullptr && Item.m_pText != nullptr && Item.m_Type != EItem::SLIDER)
		{
			const bool WithIcon = Item.m_Icon != EIcon::NONE && Item.m_Type == EItem::BUTTON;
			const float TextWidth = m_pTextRender->TextWidth(TEXT_SIZE, Item.m_pText);
			const float TextX = WithIcon ? x + ITEM_PADDING + ICON_SIZE + ITEM_SPACING : x + (Width - TextWidth) / 2.0f;
			vLabels.push_back({TextX, ItemTop + (ITEM_HEIGHT - TEXT_SIZE) / 2.0f, Item.m_pText, (Item.m_Disabled ? 0.4f : 1.0f) * Alpha});
		}
		x += Width + ITEM_SPACING;
	}
	m_pGraphics->QuadsEnd();

	// The letters come after the boxes, because they go on top of them and
	// because the text render draws with its own textures.
	for(const SLabel &Label : vLabels)
	{
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, Label.m_Alpha);
		m_pTextRender->Text(Label.m_X, Label.m_Y, TEXT_SIZE, Label.m_pText, -1.0f);
	}
	if(m_pTextRender != nullptr)
	{
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	return Pressed;
}
