/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "viewer_controls.h"

#include <base/color.h>
#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/textrender.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace
{
	// In pixels of a screen that draws one pixel per unit it is measured in,
	// scaled by `Scale`. 44 is the smallest target handhelds ask for.
	constexpr float BUTTON_SIZE = 44.0f;
	constexpr float ICON_SIZE = 20.0f;
	// Smaller in a corner, where the controls lie over the picture.
	constexpr float CORNER_BUTTON_SIZE = 34.0f;
	constexpr float CORNER_ICON_SIZE = 17.0f;
	constexpr float TEXT_SIZE = 15.0f;
	constexpr float SEEK_ROW_HEIGHT = 22.0f;
	constexpr float PADDING_X = 10.0f;
	constexpr float ITEM_SPACING = 2.0f;
	constexpr float TEXT_PADDING = 8.0f;
	constexpr float TRACK_HEIGHT = 5.0f;
	constexpr float KNOB_RADIUS = 7.0f;
	constexpr float KNOB_RADIUS_HELD = 9.0f;
	constexpr float CORNER_RADIUS = 6.0f;
	// The corner panel's distance from the edges, and its padding.
	constexpr float CORNER_MARGIN = 10.0f;
	constexpr float CORNER_PADDING = 4.0f;
	// The strip under the bar fades out upwards.
	constexpr float FADE_HEIGHT = 34.0f;
	// How far and how long a press may go and still be a tap.
	constexpr float TAP_DISTANCE = 12.0f;
	constexpr std::chrono::milliseconds TAP_TIME(400);
	// How long the bar stays after the last activity, as in a video player.
	constexpr std::chrono::milliseconds SHOWN_FOR(2500);
	constexpr std::chrono::milliseconds FADE_FOR(300);
	// The watched part of the seek bar and its knob.
	const ColorRGBA ACCENT = ColorRGBA(1.0f, 0.65f, 0.16f, 1.0f);
} // namespace

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

void CViewerControls::Init(IGraphics *pGraphics, ITextRender *pTextRender)
{
	m_pGraphics = pGraphics;
	m_pTextRender = pTextRender;
}

void CViewerControls::Show()
{
	m_ShownUntil = time_get_nanoseconds() + SHOWN_FOR;
}

void CViewerControls::Hide()
{
	m_ShownUntil = std::chrono::nanoseconds::zero();
}

// Scaled with the screen, or it would shrink on high density handhelds.
float CViewerControls::Scale() const
{
	return std::max(1.0f, m_pGraphics->ScreenHiDPIScale());
}

void CViewerControls::DrawRect(float x, float y, float w, float h, float r, float g, float b, float a)
{
	if(w <= 0.0f || h <= 0.0f)
		return;
	m_pGraphics->SetColor(r, g, b, a);
	IGraphics::CQuadItem Quad(x, y, w, h);
	m_pGraphics->QuadsDrawTL(&Quad, 1);
}

// A round corner out of rows.
void CViewerControls::DrawRoundRect(float x, float y, float w, float h, float Radius, float r, float g, float b, float a)
{
	Radius = std::min({Radius, w / 2.0f, h / 2.0f});
	if(Radius <= 0.5f)
	{
		DrawRect(x, y, w, h, r, g, b, a);
		return;
	}
	DrawRect(x, y + Radius, w, h - 2.0f * Radius, r, g, b, a);
	const int Rows = std::max(2, (int)std::ceil(Radius));
	for(int i = 0; i < Rows; ++i)
	{
		const float RowHeight = Radius / Rows;
		const float Distance = Radius - (i + 0.5f) * RowHeight;
		const float Inset = Radius - std::sqrt(std::max(Radius * Radius - Distance * Distance, 0.0f));
		DrawRect(x + Inset, y + i * RowHeight, w - 2.0f * Inset, RowHeight + 0.5f, r, g, b, a);
		DrawRect(x + Inset, y + h - (i + 1) * RowHeight - 0.5f, w - 2.0f * Inset, RowHeight + 0.5f, r, g, b, a);
	}
}

void CViewerControls::DrawDisc(vec2 Center, float Radius, float r, float g, float b, float a)
{
	DrawRoundRect(Center.x - Radius, Center.y - Radius, 2.0f * Radius, 2.0f * Radius, Radius, r, g, b, a);
}

// The icons, out of rectangles, since the map viewer has no font.
void CViewerControls::DrawIcon(EIcon Icon, vec2 Center, float Size, float Alpha)
{
	const float Half = Size / 2.0f;
	const float Thin = std::max(2.0f, Size / 5.0f);
	const auto &&Triangle = [&](float Direction) {
		constexpr int Rows = 12;
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
	case EIcon::MENU:
	{
		// Three lines, for the menu.
		const float Gap = (Size - 3.0f * Thin) / 2.0f;
		for(int i = 0; i < 3; ++i)
		{
			DrawRoundRect(Center.x - Half, Center.y - Half + i * (Thin + Gap), Size, Thin, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::DETAIL:
	{
		// A sparkle for the detail layers.
		const float Arm = Half;
		const float Waist = Thin * 0.7f;
		constexpr int Rows = 10;
		for(int i = 0; i < Rows; ++i)
		{
			const float Fraction = (i + 0.5f) / Rows;
			const float Narrow = std::abs(Fraction * 2.0f - 1.0f);
			const float Length = 2.0f * Arm * (1.0f - Narrow) + Waist * Narrow;
			const float RowHeight = Size / Rows;
			DrawRect(Center.x - Length / 2.0f, Center.y - Half + i * RowHeight, Length, RowHeight + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
			DrawRect(Center.x - Half + i * RowHeight, Center.y - Length / 2.0f, RowHeight + 0.5f, Length, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::ENTITIES:
	{
		// Four tiles, for the entity overlay.
		const float Tile = (Size - Thin) / 2.0f;
		for(int i = 0; i < 4; ++i)
		{
			const float X = Center.x - Half + ((i & 1) == 0 ? 0.0f : Tile + Thin);
			const float Y = Center.y - Half + ((i & 2) == 0 ? 0.0f : Tile + Thin);
			DrawRoundRect(X, Y, Tile, Tile, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::PLAY:
		Triangle(1.0f);
		break;
	case EIcon::PAUSE:
		DrawRoundRect(Center.x - Half, Center.y - Half, Thin, Size, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRoundRect(Center.x + Half - Thin, Center.y - Half, Thin, Size, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::RESTART:
		DrawRoundRect(Center.x - Half, Center.y - Half, Thin, Size, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		Triangle(-1.0f);
		break;
	case EIcon::MINUS:
		DrawRoundRect(Center.x - Half, Center.y - Thin / 2.0f, Size, Thin, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::PLUS:
		DrawRoundRect(Center.x - Half, Center.y - Thin / 2.0f, Size, Thin, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRoundRect(Center.x - Thin / 2.0f, Center.y - Half, Thin, Size, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::FIT:
	{
		// A filled frame.
		const float Height = Size * 0.8f;
		const float Top = Center.y - Height / 2.0f;
		DrawRect(Center.x - Half, Top, Size, Thin, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x - Half, Top + Height - Thin, Size, Thin, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x - Half, Top, Thin, Height, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x + Half - Thin, Top, Thin, Height, 1.0f, 1.0f, 1.0f, Alpha);
		const float Inset = Thin * 2.0f;
		DrawRect(Center.x - Half + Inset, Top + Inset, Size - 2.0f * Inset, Height - 2.0f * Inset, 1.0f, 1.0f, 1.0f, 0.55f * Alpha);
		break;
	}
	case EIcon::ZOOM_RESET:
	{
		// Four corners pointing inwards.
		const float Inner = Size * 0.1f;
		const float Arm = Size * 0.36f;
		for(int Corner = 0; Corner < 4; ++Corner)
		{
			const float DirX = (Corner & 1) == 0 ? 1.0f : -1.0f;
			const float DirY = (Corner & 2) == 0 ? 1.0f : -1.0f;
			const float X = Center.x + (DirX > 0.0f ? -Inner - Thin : Inner);
			const float Y = Center.y + (DirY > 0.0f ? -Inner - Thin : Inner);
			DrawRect(DirX > 0.0f ? X + Thin - Arm : X, Y, Arm, Thin, 1.0f, 1.0f, 1.0f, Alpha);
			DrawRect(X, DirY > 0.0f ? Y + Thin - Arm : Y, Thin, Arm, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::FULLSCREEN:
	{
		// Four corners pointing outwards.
		const float Arm = Size / 2.2f;
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
		// An arrow into a tray, doubled for the whole map.
		const int Count = Icon == EIcon::SAVE ? 1 : 2;
		const float Width = Size / Count - (Count > 1 ? Thin : 0.0f);
		for(int i = 0; i < Count; ++i)
		{
			const float X = Center.x - Half + i * (Width + Thin);
			DrawRect(X + Width / 2.0f - Thin / 2.0f, Center.y - Half, Thin, Size * 0.45f, 1.0f, 1.0f, 1.0f, Alpha);
			constexpr int Rows = 6;
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
		DrawRoundRect(Center.x - Half, Center.y - Half, Size, Size, Size / 6.0f, 1.0f, 1.0f, 1.0f, Alpha);
		break;
	case EIcon::FREEVIEW:
	{
		// Arrows to all four sides, for the free view.
		DrawRect(Center.x - Thin / 2.0f, Center.y - Half, Thin, Size, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x - Half, Center.y - Thin / 2.0f, Size, Thin, 1.0f, 1.0f, 1.0f, Alpha);
		constexpr int Rows = 5;
		const float Head = Size * 0.3f;
		for(int i = 0; i < Rows; ++i)
		{
			const float Length = Head * (1.0f - i / (float)Rows);
			const float Step = Head / Rows;
			DrawRect(Center.x - Length / 2.0f, Center.y - Half + i * Step, Length, Step + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
			DrawRect(Center.x - Length / 2.0f, Center.y + Half - (i + 1) * Step, Length, Step + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
			DrawRect(Center.x - Half + i * Step, Center.y - Length / 2.0f, Step + 0.5f, Length, 1.0f, 1.0f, 1.0f, Alpha);
			DrawRect(Center.x + Half - (i + 1) * Step, Center.y - Length / 2.0f, Step + 0.5f, Length, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::EYE:
	{
		// An eye.
		constexpr int Rows = 10;
		for(int i = 0; i < Rows; ++i)
		{
			const float Fraction = (i + 0.5f) / Rows;
			const float RowHeight = Size / Rows;
			const float Length = Size * (1.0f - std::abs(Fraction * 2.0f - 1.0f));
			DrawRect(Center.x - Length / 2.0f, Center.y - Half / 2.0f + i * RowHeight / 2.0f, Length, RowHeight / 2.0f + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
		}
		DrawDisc(Center, Thin * 0.8f, 0.0f, 0.0f, 0.0f, Alpha);
		break;
	}
	case EIcon::CLIP_START:
	case EIcon::CLIP_END:
	case EIcon::CLIP_CLEAR:
	{
		// A bar at the start or the end of the piece, with the kept stretch beside
		// it; both bars and a cross clear the mark.
		const float BarWidth = std::max(2.0f, Size * 0.12f);
		const float Body = Size * 0.42f;
		const bool Start = Icon == EIcon::CLIP_START;
		const bool Clear = Icon == EIcon::CLIP_CLEAR;
		const float LeftBar = Center.x - Half * 0.8f;
		const float RightBar = Center.x + Half * 0.8f - BarWidth;
		if(Start || Clear)
			DrawRect(LeftBar, Center.y - Half * 0.85f, BarWidth, Size * 0.85f, 1.0f, 1.0f, 1.0f, Alpha);
		if(!Start || Clear)
			DrawRect(RightBar, Center.y - Half * 0.85f, BarWidth, Size * 0.85f, 1.0f, 1.0f, 1.0f, Alpha);
		if(Clear)
		{
			constexpr int Steps = 6;
			const float Step = Size * 0.34f / Steps;
			for(int i = 0; i < Steps; ++i)
			{
				const float Offset = (i - (Steps - 1) / 2.0f) * Step;
				DrawRect(Center.x + Offset - Step / 2.0f, Center.y + Offset, Step + 0.5f, Step + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
				DrawRect(Center.x + Offset - Step / 2.0f, Center.y - Offset - Step, Step + 0.5f, Step + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
			}
		}
		else
		{
			const float BodyLeft = Start ? LeftBar + BarWidth * 2.0f : RightBar - BarWidth - Body;
			DrawRect(BodyLeft, Center.y - Body / 2.0f, Body + BarWidth, Body, 1.0f, 1.0f, 1.0f, 0.55f * Alpha);
		}
		break;
	}
	case EIcon::VOLUME:
	case EIcon::VOLUME_OFF:
	{
		// A speaker, with two strokes for sound or a cross for muted.
		const float BoxHeight = Size * 0.34f;
		const float BoxWidth = Size * 0.16f;
		const float ConeWidth = Size * 0.22f;
		const float Left = Center.x - Half * 0.9f;
		DrawRect(Left, Center.y - BoxHeight / 2.0f, BoxWidth, BoxHeight, 1.0f, 1.0f, 1.0f, Alpha);
		constexpr int Rows = 10;
		for(int i = 0; i < Rows; ++i)
		{
			const float Fraction = (i + 0.5f) / Rows;
			const float Height = mix(BoxHeight, Size * 0.92f, Fraction);
			DrawRect(Left + BoxWidth + Fraction * ConeWidth - ConeWidth / Rows, Center.y - Height / 2.0f,
				ConeWidth / Rows + 0.5f, Height, 1.0f, 1.0f, 1.0f, Alpha);
		}
		const float RightOfCone = Left + BoxWidth + ConeWidth;
		if(Icon == EIcon::VOLUME)
		{
			for(int i = 0; i < 2; ++i)
			{
				const float Height = Size * (0.34f + i * 0.28f);
				DrawRect(RightOfCone + Size * (0.12f + i * 0.2f), Center.y - Height / 2.0f,
					Thin * 0.8f, Height, 1.0f, 1.0f, 1.0f, Alpha);
			}
		}
		else
		{
			// A cross as a stair of squares.
			constexpr int Steps = 7;
			const float Step = Size * 0.46f / Steps;
			const float CrossX = RightOfCone + Size * 0.16f;
			for(int i = 0; i < Steps; ++i)
			{
				const float Offset = (i - (Steps - 1) / 2.0f) * Step;
				DrawRect(CrossX + Offset + Size * 0.22f, Center.y + Offset, Step + 0.5f, Step + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
				DrawRect(CrossX + Offset + Size * 0.22f, Center.y - Offset - Step, Step + 0.5f, Step + 0.5f, 1.0f, 1.0f, 1.0f, Alpha);
			}
		}
		break;
	}
	}
}

float CViewerControls::ItemWidth(const SItem &Item, const SLayout &Layout) const
{
	if(Item.m_Width > 0.0f)
		return Item.m_Width * Layout.m_Unit;
	if(Item.m_Type == EItem::SPACER)
		return 0.0f;
	if(m_pTextRender != nullptr && Item.m_pText != nullptr)
	{
		const float Text = m_pTextRender->TextWidth(TEXT_SIZE * Layout.m_Unit, Item.m_pText);
		if(Item.m_Type == EItem::TEXT)
			return Text + 2.0f * TEXT_PADDING * Layout.m_Unit;
		return std::max(Layout.m_ButtonSize, Text + Layout.m_IconSize + 3.0f * TEXT_PADDING * Layout.m_Unit);
	}
	return Layout.m_ButtonSize;
}

// A row of icons on a panel in the top right corner.
void CViewerControls::LayoutCorner(const SItem *pItems, size_t Count, SLayout &Layout) const
{
	const float Unit = Layout.m_Unit;
	const float Padding = CORNER_PADDING * Unit;
	const float Margin = CORNER_MARGIN * Unit;
	size_t RowCount = 0;
	for(size_t i = 0; i < Count; ++i)
	{
		if(pItems[i].m_Type != EItem::SPACER && !pItems[i].m_Hidden && !pItems[i].m_InMenu)
			++RowCount;
	}
	const float PanelWidth = RowCount * Layout.m_ButtonSize + (RowCount + 1) * Padding;
	const float Left = std::max(m_pGraphics->ScreenWidth() - Margin - PanelWidth, 0.0f);
	Layout.m_Region = CScreenRect(Left, Margin, PanelWidth, Layout.m_ButtonSize + 2.0f * Padding);
	float x = Left + Padding;
	for(size_t i = 0; i < Count; ++i)
	{
		if(pItems[i].m_Type == EItem::SPACER || pItems[i].m_Hidden || pItems[i].m_InMenu)
			continue;
		Layout.m_vPlaced.push_back({i, x, Margin + Padding, Layout.m_ButtonSize, Layout.m_ButtonSize});
		x += Layout.m_ButtonSize + Padding;
	}
}

// A bar along the bottom, the seek bar on a line of its own above the
// buttons. What does not fit goes, optional items first and text next.
void CViewerControls::LayoutBar(const SItem *pItems, size_t Count, SLayout &Layout) const
{
	const float Unit = Layout.m_Unit;
	const float ScreenWidth = m_pGraphics->ScreenWidth();
	const float BarHeight = Layout.m_SeekHeight + Layout.m_ButtonSize;
	const float BarTop = m_pGraphics->ScreenHeight() - BarHeight;
	Layout.m_Region = CScreenRect(0.0f, BarTop, ScreenWidth, BarHeight);
	std::vector<float> vWidths(Count, 0.0f);
	std::vector<bool> vShown(Count, true);
	const float RowWidth = ScreenWidth - 2.0f * PADDING_X * Unit;
	float Total = 0.0f;
	for(size_t i = 0; i < Count; ++i)
	{
		vShown[i] = i != Layout.m_Slider && !pItems[i].m_Hidden && !pItems[i].m_InMenu;
		if(!vShown[i])
			continue;
		vWidths[i] = ItemWidth(pItems[i], Layout);
		Total += vWidths[i] + ITEM_SPACING * Unit;
	}
	const auto &&Drop = [&](auto &&Droppable) {
		for(size_t i = Count; i-- > 0 && Total > RowWidth;)
		{
			if(!vShown[i] || !Droppable(pItems[i]))
				continue;
			vShown[i] = false;
			Total -= vWidths[i] + ITEM_SPACING * Unit;
		}
	};
	Drop([](const SItem &Item) { return Item.m_Optional; });
	Drop([](const SItem &Item) { return Item.m_Type == EItem::TEXT; });
	const float Spare = std::max(RowWidth - Total, 0.0f);
	size_t Spacers = 0;
	for(size_t i = 0; i < Count; ++i)
	{
		if(vShown[i] && pItems[i].m_Type == EItem::SPACER)
			++Spacers;
	}
	float x = PADDING_X * Unit;
	for(size_t i = 0; i < Count; ++i)
	{
		if(!vShown[i])
			continue;
		if(pItems[i].m_Type == EItem::SPACER)
		{
			x += Spare / Spacers + ITEM_SPACING * Unit;
			continue;
		}
		Layout.m_vPlaced.push_back({i, x, BarTop + Layout.m_SeekHeight, vWidths[i], Layout.m_ButtonSize});
		x += vWidths[i] + ITEM_SPACING * Unit;
	}
}

// The open menu: rows lined up with the button that opened it, below it in a
// corner and above it on a bar, in more columns where the window is too low.
void CViewerControls::LayoutMenu(const SItem *pItems, size_t Count, size_t Owner, SLayout &Layout) const
{
	const float Unit = Layout.m_Unit;
	const bool Corner = m_Placement == EPlacement::CORNER;
	const float Padding = CORNER_PADDING * Unit;
	const float Gap = Corner ? Padding : ITEM_SPACING * Unit;
	const float ButtonSize = Layout.m_ButtonSize;
	std::vector<size_t> vMenuItems;
	float RowWidth = ButtonSize;
	for(size_t i = 0; i < Count; ++i)
	{
		if(!pItems[i].m_InMenu || pItems[i].m_Hidden || pItems[i].m_MenuId != m_OpenMenuId)
			continue;
		vMenuItems.push_back(i);
		RowWidth = std::max(RowWidth, ItemWidth(pItems[i], Layout));
	}
	const CScreenRect &Region = Layout.m_Region;
	float AnchorRight = Region.m_BottomRight.x - Padding;
	for(const SPlaced &Placed : Layout.m_vPlaced)
	{
		if(Placed.m_Index == Owner)
			AnchorRight = Placed.m_X + Placed.m_Width;
	}
	const float ScreenWidth = m_pGraphics->ScreenWidth();
	const float Room = Corner ? m_pGraphics->ScreenHeight() - Region.m_BottomRight.y - Gap - CORNER_MARGIN * Unit :
				    Region.m_TopLeft.y - Gap - CORNER_MARGIN * Unit;
	const size_t MaxRows = std::max<size_t>(1, (size_t)std::max(0.0f, (Room - Padding) / (ButtonSize + Padding)));
	const size_t Columns = std::max<size_t>(1, (vMenuItems.size() + MaxRows - 1) / MaxRows);
	const size_t Rows = std::max<size_t>(1, (vMenuItems.size() + Columns - 1) / Columns);
	const float MenuWidth = Columns * RowWidth + (Columns + 1) * Padding;
	const float MenuHeight = Rows * ButtonSize + (Rows + 1) * Padding;
	const float MenuLeft = std::clamp(AnchorRight + Padding - MenuWidth, 0.0f, std::max(ScreenWidth - MenuWidth, 0.0f));
	const float MenuTop = Corner ? Region.m_BottomRight.y + Gap : Region.m_TopLeft.y - Gap - MenuHeight;
	Layout.m_MenuRegion = CScreenRect(MenuLeft, MenuTop, MenuWidth, MenuHeight);
	Layout.m_HasMenu = true;
	for(size_t i = 0; i < vMenuItems.size(); ++i)
	{
		const float ItemX = MenuLeft + Padding + (i / Rows) * (RowWidth + Padding);
		const float ItemY = MenuTop + Padding + (i % Rows) * (ButtonSize + Padding);
		Layout.m_vPlaced.push_back({vMenuItems[i], ItemX, ItemY, RowWidth, ButtonSize});
	}
}

void CViewerControls::DrawBackground(const SLayout &Layout, float Alpha)
{
	const CScreenRect &Region = Layout.m_Region;
	if(m_Placement == EPlacement::CORNER)
	{
		DrawRoundRect(Region.m_TopLeft.x, Region.m_TopLeft.y, Region.Width(), Region.Height(), CORNER_RADIUS * Layout.m_Unit, 0.0f, 0.0f, 0.0f, 0.55f * Alpha);
	}
	else
	{
		// Dark under the bar and fading out above it.
		const float ScreenWidth = m_pGraphics->ScreenWidth();
		const float FadeTop = std::max(Region.m_TopLeft.y - FADE_HEIGHT * Layout.m_Unit, 0.0f);
		const ColorRGBA Clear = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
		const ColorRGBA Dark = ColorRGBA(0.0f, 0.0f, 0.0f, 0.72f * Alpha);
		m_pGraphics->SetColor4(Clear, Clear, Dark, Dark);
		IGraphics::CQuadItem Fade(0.0f, FadeTop, ScreenWidth, Region.m_TopLeft.y - FadeTop);
		m_pGraphics->QuadsDrawTL(&Fade, 1);
		DrawRect(0.0f, Region.m_TopLeft.y, ScreenWidth, Region.Height(), 0.0f, 0.0f, 0.0f, 0.72f * Alpha);
	}
	if(Layout.m_HasMenu)
	{
		const CScreenRect &Menu = Layout.m_MenuRegion;
		DrawRoundRect(Menu.m_TopLeft.x, Menu.m_TopLeft.y, Menu.Width(), Menu.Height(), CORNER_RADIUS * Layout.m_Unit, 0.0f, 0.0f, 0.0f, 0.82f * Alpha);
	}
}

// The whole height of the seek row answers to the pointer, not just the line.
bool CViewerControls::RenderSlider(const SItem &Item, const SLayout &Layout, vec2 Mouse, bool Clicked, float Alpha, float *pSliderValue)
{
	const float Unit = Layout.m_Unit;
	const float Top = Layout.m_Region.m_TopLeft.y;
	const float Left = PADDING_X * Unit;
	const float Width = m_pGraphics->ScreenWidth() - 2.0f * PADDING_X * Unit;
	const float Middle = Top + Layout.m_SeekHeight / 2.0f;
	const int Index = (int)Layout.m_Slider;
	const bool Over = !Item.m_Disabled && Mouse.y >= Top && Mouse.y <= Top + Layout.m_SeekHeight;
	const bool Held = m_Dragging == Index;
	if(Over && Clicked)
		m_Dragging = Index;
	bool Moved = false;
	float Value = std::clamp(Item.m_Value, 0.0f, 1.0f);
	if(m_Dragging == Index && pSliderValue != nullptr)
	{
		Value = std::clamp((Mouse.x - Left) / std::max(Width, 1.0f), 0.0f, 1.0f);
		*pSliderValue = Value;
		Moved = true;
	}
	const float TrackHeight = TRACK_HEIGHT * Unit;
	DrawRoundRect(Left, Middle - TrackHeight / 2.0f, Width, TrackHeight, TrackHeight / 2.0f, 1.0f, 1.0f, 1.0f, 0.3f * Alpha);
	if(Item.m_RangeEnd > Item.m_RangeStart)
	{
		const float RangeStart = std::clamp(Item.m_RangeStart, 0.0f, 1.0f);
		const float RangeEnd = std::clamp(Item.m_RangeEnd, RangeStart, 1.0f);
		DrawRect(Left + Width * RangeStart, Middle - TrackHeight / 2.0f, Width * (RangeEnd - RangeStart), TrackHeight, 1.0f, 1.0f, 1.0f, 0.55f * Alpha);
	}
	DrawRoundRect(Left, Middle - TrackHeight / 2.0f, Width * Value, TrackHeight, TrackHeight / 2.0f, ACCENT.r, ACCENT.g, ACCENT.b, Alpha);
	const float Radius = (Held || Over ? KNOB_RADIUS_HELD : KNOB_RADIUS) * Unit;
	DrawDisc(vec2(Left + Width * Value, Middle), Radius, ACCENT.r, ACCENT.g, ACCENT.b, Alpha);
	return Moved;
}

// Draws the boxes and icons, collects the letters for afterwards, and says
// what was pressed.
void CViewerControls::RenderItems(const SItem *pItems, const SLayout &Layout, const SInput &Input, vec2 Mouse, bool Clicked, float Alpha, std::vector<SLabel> &vLabels, int &Pressed, std::optional<int> &PressedMenuId)
{
	const float Unit = Layout.m_Unit;
	for(const SPlaced &Placed : Layout.m_vPlaced)
	{
		const SItem &Item = pItems[Placed.m_Index];
		const bool Active = Item.m_Active || (Item.m_OpensMenu && m_MenuOpen && Item.m_MenuId == m_OpenMenuId);
		const bool Over = !Item.m_Disabled && Mouse.x >= Placed.m_X && Mouse.x <= Placed.m_X + Placed.m_Width &&
				  Mouse.y >= Placed.m_Y && Mouse.y <= Placed.m_Y + Placed.m_Height;
		const bool WithText = m_pTextRender != nullptr && Item.m_pText != nullptr;
		if(Item.m_Type == EItem::BUTTON)
		{
			const float Inset = 3.0f * Unit;
			if(Over || Active)
			{
				const float Shade = Over && Input.m_MousePressed ? 0.35f : (Active ? 0.28f : 0.18f);
				DrawRoundRect(Placed.m_X + Inset, Placed.m_Y + Inset, Placed.m_Width - 2.0f * Inset, Placed.m_Height - 2.0f * Inset,
					CORNER_RADIUS * Unit, 1.0f, 1.0f, 1.0f, Shade * Alpha);
			}
			if(Item.m_Icon != EIcon::NONE)
			{
				const float IconX = WithText ? Placed.m_X + TEXT_PADDING * Unit + Layout.m_IconSize / 2.0f : Placed.m_X + Placed.m_Width / 2.0f;
				DrawIcon(Item.m_Icon, vec2(IconX, Placed.m_Y + Placed.m_Height / 2.0f), Layout.m_IconSize, (Item.m_Disabled ? 0.35f : 1.0f) * Alpha);
			}
			if(Over && Clicked)
			{
				if(Item.m_OpensMenu)
					PressedMenuId = Item.m_MenuId;
				else
					Pressed = (int)Placed.m_Index;
			}
		}
		if(WithText)
		{
			// Menu rows line up on the left, a button's text sits in its middle.
			const float TextWidth = m_pTextRender->TextWidth(TEXT_SIZE * Unit, Item.m_pText);
			const bool WithIcon = Item.m_Icon != EIcon::NONE && Item.m_Type == EItem::BUTTON;
			const float TextX = WithIcon ? Placed.m_X + 2.0f * TEXT_PADDING * Unit + Layout.m_IconSize :
						       (Item.m_InMenu ? Placed.m_X + TEXT_PADDING * Unit : Placed.m_X + (Placed.m_Width - TextWidth) / 2.0f);
			vLabels.push_back({TextX, Placed.m_Y + (Placed.m_Height - TEXT_SIZE * Unit) / 2.0f, Item.m_pText, (Item.m_Disabled ? 0.4f : 1.0f) * Alpha});
		}
	}
}

int CViewerControls::Render(const SItem *pItems, size_t Count, const SInput &Input, float *pSliderValue)
{
	if(m_pGraphics == nullptr || Count == 0)
	{
		return -1;
	}

	// The pointer comes in window units, the frame is drawn in pixels.
	SLayout Layout;
	Layout.m_Unit = Scale();
	const vec2 Mouse = Input.m_MousePos * m_pGraphics->ScreenHiDPIScale();
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	// Shown on the first frame, as a video player does.
	if(!m_Started)
	{
		m_Started = true;
		Show();
	}
	// Read first, so that the tap that shows them does not also hide them.
	const bool ShownBeforeInput = Now <= m_ShownUntil + FADE_FOR;

	// A moving pointer, a key or an open menu keep them shown.
	const bool Moved = distance(Mouse, m_LastMousePos) > 1.0f;
	m_LastMousePos = Mouse;
	if(Moved || Input.m_KeyPressed || m_Dragging >= 0 || m_MenuOpen)
	{
		Show();
	}

	const bool Corner = m_Placement == EPlacement::CORNER;
	Layout.m_Slider = Count;
	for(size_t i = 0; i < Count && !Corner; ++i)
	{
		if(pItems[i].m_Type == EItem::SLIDER)
		{
			Layout.m_Slider = i;
			break;
		}
	}
	Layout.m_SeekHeight = Layout.m_Slider < Count ? SEEK_ROW_HEIGHT * Layout.m_Unit : 0.0f;
	Layout.m_ButtonSize = (Corner ? CORNER_BUTTON_SIZE : BUTTON_SIZE) * Layout.m_Unit;
	Layout.m_IconSize = (Corner ? CORNER_ICON_SIZE : ICON_SIZE) * Layout.m_Unit;
	Layout.m_vPlaced.reserve(Count);

	// A menu whose button is no longer offered is closed.
	size_t MenuOwner = Count;
	for(size_t i = 0; i < Count; ++i)
	{
		if(!pItems[i].m_Hidden && pItems[i].m_OpensMenu && pItems[i].m_MenuId == m_OpenMenuId)
			MenuOwner = i;
	}
	if(MenuOwner == Count)
		m_MenuOpen = false;

	if(Corner)
		LayoutCorner(pItems, Count, Layout);
	else
		LayoutBar(pItems, Count, Layout);
	if(m_MenuOpen)
		LayoutMenu(pItems, Count, MenuOwner, Layout);

	const float ScreenWidth = m_pGraphics->ScreenWidth();
	m_Hovered = Layout.m_Region.Inside(Mouse) || (Layout.m_HasMenu && Layout.m_MenuRegion.Inside(Mouse));
	if(!Corner)
	{
		// The strip the bar sits on reaches above it.
		m_Hovered = m_Hovered || (Mouse.x >= 0.0f && Mouse.x <= ScreenWidth && Mouse.y >= Layout.m_Region.m_TopLeft.y - FADE_HEIGHT * Layout.m_Unit);
	}

	const bool Clicked = Input.m_MouseClicked || (Input.m_MousePressed && !m_WasPressed);
	// A tap that began and ended between two frames is both at once.
	const bool Released = (!Input.m_MousePressed && m_WasPressed) || (Input.m_MouseClicked && !Input.m_MousePressed);
	const bool Hidden = Now > m_ShownUntil + FADE_FOR;
	if(Hidden)
	{
		// Hidden controls take no clicks, but a tap still brings them back.
		m_Hovered = false;
		m_Dragging = -1;
		m_MenuOpen = false;
	}
	if(Clicked)
	{
		m_PressedAt = Mouse;
		m_PressedWhen = Now;
		m_PressedOnBar = m_Hovered;
	}
	if(!Input.m_MousePressed)
	{
		m_Dragging = -1;
	}
	m_WasPressed = Input.m_MousePressed;
	const bool Tap = Released && !m_PressedOnBar && distance(Mouse, m_PressedAt) <= TAP_DISTANCE * Layout.m_Unit && Now - m_PressedWhen <= TAP_TIME;
	if(Hidden)
	{
		if(Tap)
			Show();
		return -1;
	}
	const float Alpha = Now <= m_ShownUntil ? 1.0f : 1.0f - std::chrono::duration_cast<std::chrono::duration<float>>(Now - m_ShownUntil).count() / std::chrono::duration_cast<std::chrono::duration<float>>(FADE_FOR).count();

	m_pGraphics->MapScreen(CScreenRect(0.0f, 0.0f, ScreenWidth, m_pGraphics->ScreenHeight()));
	m_pGraphics->TextureClear();
	m_pGraphics->QuadsBegin();
	DrawBackground(Layout, Alpha);
	const bool MenuWasOpen = m_MenuOpen;
	int Pressed = -1;
	if(Layout.m_Slider < Count && RenderSlider(pItems[Layout.m_Slider], Layout, Mouse, Clicked, Alpha, pSliderValue))
		Pressed = (int)Layout.m_Slider;
	std::vector<SLabel> vLabels;
	// Acted on after drawing, because opening a menu moves what is under it.
	std::optional<int> PressedMenuId;
	RenderItems(pItems, Layout, Input, Mouse, Clicked, Alpha, vLabels, Pressed, PressedMenuId);
	m_pGraphics->QuadsEnd();

	if(PressedMenuId.has_value())
	{
		// The same button again closes what it opened.
		m_MenuOpen = !(m_MenuOpen && m_OpenMenuId == PressedMenuId.value());
		m_OpenMenuId = PressedMenuId.value();
	}
	else if((Pressed >= 0 && pItems[Pressed].m_InMenu && !pItems[Pressed].m_KeepsMenu) || (Clicked && !m_Hovered))
	{
		// Picking from a menu or pressing outside it closes it.
		m_MenuOpen = false;
	}

	// A tap on the picture shows or hides the controls; one that only closed
	// a menu does nothing else.
	if(Tap && !MenuWasOpen)
	{
		if(ShownBeforeInput)
			Hide();
		else
			Show();
	}

	// The letters go on top, and the text render draws with its own textures.
	for(const SLabel &Label : vLabels)
	{
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, Label.m_Alpha);
		m_pTextRender->Text(Label.m_X, Label.m_Y, TEXT_SIZE * Layout.m_Unit, Label.m_pText, -1.0f);
	}
	if(m_pTextRender != nullptr)
	{
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	return Pressed;
}
