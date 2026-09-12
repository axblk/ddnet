/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "viewer_controls.h"

#include <base/color.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/textrender.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	// How the bar is put together. Everything here is in the pixels of a screen
	// that draws one pixel for every one it is measured in, and is multiplied by
	// what the screen actually does before anything is drawn - see Scale().
	//
	// A finger covers about nine millimetres, which is where the 44 comes from:
	// it is the size every handheld system asks of anything that can be pressed,
	// and it is what decides the height of the bar rather than the icons in it.
	constexpr float BUTTON_SIZE = 44.0f;
	constexpr float ICON_SIZE = 20.0f;
	constexpr float TEXT_SIZE = 15.0f;
	constexpr float SEEK_ROW_HEIGHT = 22.0f;
	constexpr float PADDING_X = 10.0f;
	constexpr float ITEM_SPACING = 2.0f;
	constexpr float TEXT_PADDING = 8.0f;
	constexpr float TRACK_HEIGHT = 5.0f;
	constexpr float KNOB_RADIUS = 7.0f;
	constexpr float KNOB_RADIUS_HELD = 9.0f;
	constexpr float CORNER_RADIUS = 6.0f;
	// The panel of icons in a corner: how far it stands off the edges of the
	// window, and how much room there is around what is on it.
	constexpr float CORNER_MARGIN = 10.0f;
	constexpr float CORNER_PADDING = 4.0f;
	// The strip the bar sits on does not end at the bar: it fades out upwards,
	// so that white letters on a bright picture still have something dark under
	// them and the bar has no edge to it.
	constexpr float FADE_HEIGHT = 34.0f;
	// How far the pointer may travel and how long it may stay down for a press
	// to still be a tap on the picture rather than somebody dragging the view.
	constexpr float TAP_DISTANCE = 12.0f;
	constexpr std::chrono::milliseconds TAP_TIME(400);
	// How long the bar stays after the last thing that happened, as a video
	// player's does: long enough to reach it, short enough to be out of the way.
	constexpr std::chrono::milliseconds SHOWN_FOR(2500);
	constexpr std::chrono::milliseconds FADE_FOR(300);
	// What is drawn in the colour of the program rather than in grey: the part
	// of the seek bar that has been watched, and the knob that sits on it.
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
	// A pinch with the two fingers on the same spot says nothing about how far
	// apart they were, so it says nothing about the zoom either.
	const float Distance = std::max(distance(First, Second), 1.0f);
	const vec2 Middle = (First + Second) / 2.0f;

	SResult Result;
	Result.m_Active = true;
	if(m_Pinching)
	{
		// Fingers that move apart ask for a closer look, which is less world.
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

bool CViewerControls::Shown() const
{
	return time_get_nanoseconds() <= m_ShownUntil + FADE_FOR;
}

// A screen that draws three pixels for every one it is measured in is a screen
// where everything here would come out a third of the size, which on the
// handhelds that do that is exactly where the bar is hardest to hit.
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

// A round corner out of rows, which is all there is to draw with here: no
// texture, no triangle fan, and at these sizes near enough to a circle that
// nobody looks twice.
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
		// Measured from the middle of the row, so that the corner neither eats
		// into the rectangle nor leaves a step at the end of it.
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

// Everything a button shows that is not a letter, out of rectangles: the one
// thing that is drawn here without a font, and the only thing the map viewer
// has. A triangle is a stack of rows, which at this size is a triangle.
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
		// Three lines, which is what everything that did not fit is behind
		// everywhere else.
		const float Gap = (Size - 3.0f * Thin) / 2.0f;
		for(int i = 0; i < 3; ++i)
		{
			DrawRoundRect(Center.x - Half, Center.y - Half + i * (Thin + Gap), Size, Thin, Thin / 2.0f, 1.0f, 1.0f, 1.0f, Alpha);
		}
		break;
	}
	case EIcon::DETAIL:
	{
		// A sparkle: a map's details are the part of it that is there to be
		// looked at rather than played.
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
		// Four tiles, which is what a map is made of under what it looks like.
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
		// Four corners of a frame, which is what fitting something into a
		// window looks like when there is no room to write it.
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
	case EIcon::FULLSCREEN:
	{
		// A screen: a frame with nothing in it, which is what a window that has
		// taken over the whole screen looks like from the outside.
		const float Thick = std::max(2.0f, Size / 8.0f);
		const float Height = Size * 0.78f;
		const float Top = Center.y - Height / 2.0f;
		DrawRect(Center.x - Half, Top, Size, Thick, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x - Half, Top + Height - Thick, Size, Thick, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x - Half, Top, Thick, Height, 1.0f, 1.0f, 1.0f, Alpha);
		DrawRect(Center.x + Half - Thick, Top, Thick, Height, 1.0f, 1.0f, 1.0f, Alpha);
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
	case EIcon::EYE:
	{
		// A lens with a pupil in it, which is what watching looks like: rows
		// that grow and shrink again, and a dark disc in the middle of them.
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
	const float Unit = Scale();
	const vec2 Mouse = Input.m_MousePos * m_pGraphics->ScreenHiDPIScale();
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	// The first frame shows the controls, the way a video player does: a window
	// with nothing in it but a picture is a window nobody knows what to do
	// with, and they go away by themselves soon enough.
	if(!m_Started)
	{
		m_Started = true;
		Show();
	}
	// Read before anything shows them, because a tap that brings them back
	// must not be read as a tap that sends away what it just brought.
	const bool ShownBeforeInput = Now <= m_ShownUntil + FADE_FOR;

	// Somebody being there is the pointer moving or a key going down. A button
	// held down is not: a finger resting on the picture says nothing, and what
	// a press means is worked out where it is let go of. A menu that is open
	// is somebody reading it, so nothing goes away under them.
	const bool Moved = distance(Mouse, m_LastMousePos) > 1.0f;
	m_LastMousePos = Mouse;
	if(Moved || Input.m_KeyPressed || m_Dragging >= 0 || m_MenuOpen)
	{
		Show();
	}

	const float ScreenWidth = m_pGraphics->ScreenWidth();
	const float ScreenHeight = m_pGraphics->ScreenHeight();
	const bool Corner = m_Placement == EPlacement::CORNER;

	// Where everything goes. Worked out before anything is drawn, so that what
	// answers to a press is the same rectangle that was drawn.
	std::vector<SPlaced> vPlaced;
	vPlaced.reserve(Count + 1);
	// What the controls are drawn on, and what counts as being over them.
	CScreenRect Region(0.0f, 0.0f, 0.0f, 0.0f);
	CScreenRect MenuRegion(0.0f, 0.0f, 0.0f, 0.0f);
	bool HasMenuRegion = false;
	// The seek bar has a line of its own above the buttons, wherever the caller
	// happened to put it in the list, because that is where a video player has
	// it and because a bar as wide as the window is a bar that can be hit.
	size_t Slider = Count;
	for(size_t i = 0; i < Count; ++i)
	{
		if(pItems[i].m_Type == EItem::SLIDER && Slider == Count && !Corner)
		{
			Slider = i;
		}
	}
	const float SeekHeight = Slider < Count ? SEEK_ROW_HEIGHT * Unit : 0.0f;
	const float ButtonSize = BUTTON_SIZE * Unit;

	const auto &&ItemWidth = [&](const SItem &Item) {
		if(Item.m_Width > 0.0f)
			return Item.m_Width * Unit;
		if(Item.m_Type == EItem::SPACER)
			return 0.0f;
		if(!Corner && m_pTextRender != nullptr && Item.m_pText != nullptr)
		{
			const float Text = m_pTextRender->TextWidth(TEXT_SIZE * Unit, Item.m_pText);
			if(Item.m_Type == EItem::TEXT)
				return Text + 2.0f * TEXT_PADDING * Unit;
			return std::max(ButtonSize, Text + ICON_SIZE * Unit + 3.0f * TEXT_PADDING * Unit);
		}
		return ButtonSize;
	};

	// The button that opens the menu is the bar's own, not the caller's: the
	// caller says which of its items belong in a menu and the bar works out
	// that there has to be something to open it with. It answers with nothing,
	// since nothing outside has anything to do about it.
	const size_t MenuButton = Count;
	bool HasMenu = false;
	if(Corner)
	{
		for(size_t i = 0; i < Count; ++i)
		{
			HasMenu = HasMenu || (pItems[i].m_InMenu && !pItems[i].m_Hidden);
		}
	}
	if(!HasMenu)
	{
		m_MenuOpen = false;
	}

	if(Corner)
	{
		// A row of icons in the top right corner, on a panel of their own, and
		// under it the menu when it is open.
		const float Padding = CORNER_PADDING * Unit;
		const float Margin = CORNER_MARGIN * Unit;
		size_t Shown = 0;
		for(size_t i = 0; i < Count; ++i)
		{
			if(pItems[i].m_Type != EItem::SPACER && !pItems[i].m_Hidden && !pItems[i].m_InMenu)
				++Shown;
		}
		const size_t RowCount = Shown + (HasMenu ? 1 : 0);
		const float PanelWidth = RowCount * ButtonSize + (RowCount + 1) * Padding;
		const float Left = std::max(ScreenWidth - Margin - PanelWidth, 0.0f);
		const float Top = Margin;
		Region = CScreenRect(Left, Top, PanelWidth, ButtonSize + 2.0f * Padding);
		float x = Left + Padding;
		for(size_t i = 0; i < Count; ++i)
		{
			if(pItems[i].m_Type == EItem::SPACER || pItems[i].m_Hidden || pItems[i].m_InMenu)
				continue;
			vPlaced.push_back({i, x, Top + Padding, ButtonSize, ButtonSize});
			x += ButtonSize + Padding;
		}
		if(HasMenu)
		{
			vPlaced.push_back({MenuButton, x, Top + Padding, ButtonSize, ButtonSize});
		}
		if(m_MenuOpen)
		{
			size_t InMenu = 0;
			for(size_t i = 0; i < Count; ++i)
			{
				if(pItems[i].m_InMenu && !pItems[i].m_Hidden)
					++InMenu;
			}
			const float MenuTop = Region.m_BottomRight.y + Padding;
			const float MenuHeight = InMenu * ButtonSize + (InMenu + 1) * Padding;
			// The column of the menu stands under the button that opened it.
			const float MenuLeft = Region.m_BottomRight.x - 2.0f * Padding - ButtonSize;
			MenuRegion = CScreenRect(MenuLeft, MenuTop, ButtonSize + 2.0f * Padding, MenuHeight);
			HasMenuRegion = true;
			float y = MenuTop + Padding;
			for(size_t i = 0; i < Count; ++i)
			{
				if(!pItems[i].m_InMenu || pItems[i].m_Hidden)
					continue;
				vPlaced.push_back({i, MenuLeft + Padding, y, ButtonSize, ButtonSize});
				y += ButtonSize + Padding;
			}
		}
	}
	else
	{
		// A bar along the bottom. How wide everything in the row of buttons is,
		// and what has to be left out to make it fit: a window narrower than
		// the bar is a telephone standing upright, and what goes first is what
		// was marked as being nice to have.
		const float BarHeight = SeekHeight + ButtonSize;
		const float BarTop = ScreenHeight - BarHeight;
		Region = CScreenRect(0.0f, BarTop, ScreenWidth, BarHeight);
		std::vector<float> vWidths(Count, 0.0f);
		std::vector<bool> vShown(Count, true);
		for(size_t i = 0; i < Count; ++i)
		{
			vShown[i] = !pItems[i].m_Hidden;
		}
		const float RowWidth = ScreenWidth - 2.0f * PADDING_X * Unit;
		float Total = 0.0f;
		for(size_t i = 0; i < Count; ++i)
		{
			if(i == Slider || !vShown[i])
				continue;
			vWidths[i] = ItemWidth(pItems[i]);
			Total += vWidths[i] + ITEM_SPACING * Unit;
		}
		for(size_t i = Count; i-- > 0 && Total > RowWidth;)
		{
			if(i == Slider || !vShown[i] || !pItems[i].m_Optional)
				continue;
			vShown[i] = false;
			Total -= vWidths[i] + ITEM_SPACING * Unit;
		}
		// Still too much, so the text gives up its letters before a button
		// gives up its place: a button that is gone is a button nobody can
		// press.
		for(size_t i = Count; i-- > 0 && Total > RowWidth;)
		{
			if(i == Slider || !vShown[i] || pItems[i].m_Type != EItem::TEXT)
				continue;
			vShown[i] = false;
			Total -= vWidths[i] + ITEM_SPACING * Unit;
		}
		const float Spare = std::max(RowWidth - Total, 0.0f);
		size_t Spacers = 0;
		for(size_t i = 0; i < Count; ++i)
		{
			if(i != Slider && vShown[i] && pItems[i].m_Type == EItem::SPACER)
				++Spacers;
		}
		float x = PADDING_X * Unit;
		for(size_t i = 0; i < Count; ++i)
		{
			if(i == Slider || !vShown[i])
				continue;
			if(pItems[i].m_Type == EItem::SPACER)
			{
				x += Spare / Spacers + ITEM_SPACING * Unit;
				continue;
			}
			vPlaced.push_back({i, x, BarTop + SeekHeight, vWidths[i], ButtonSize});
			x += vWidths[i] + ITEM_SPACING * Unit;
		}
	}

	m_Hovered = Region.Inside(Mouse) || (HasMenuRegion && MenuRegion.Inside(Mouse));
	if(!Corner)
	{
		// The strip the bar sits on reaches further up than the bar does, and
		// so does what belongs to it.
		m_Hovered = Mouse.x >= 0.0f && Mouse.x <= ScreenWidth && Mouse.y >= Region.m_TopLeft.y - FADE_HEIGHT * Unit;
	}

	if(Now > m_ShownUntil + FADE_FOR)
	{
		// Gone, and out of the way of the pointer with it: controls nobody can
		// see must not swallow a click on the picture. What a press does is
		// still worked out, because a tap on the picture is how they are asked
		// back.
		m_Hovered = false;
		m_Dragging = -1;
		m_MenuOpen = false;
		if(Input.m_MouseClicked || (Input.m_MousePressed && !m_WasPressed))
		{
			m_PressedAt = Mouse;
			m_PressedWhen = Now;
			m_PressedOnBar = false;
		}
		const bool Released = (!Input.m_MousePressed && m_WasPressed) || (Input.m_MouseClicked && !Input.m_MousePressed);
		if(Released && distance(Mouse, m_PressedAt) <= TAP_DISTANCE * Unit && Now - m_PressedWhen <= TAP_TIME)
		{
			Show();
		}
		m_WasPressed = Input.m_MousePressed;
		return -1;
	}
	const float Alpha = Now <= m_ShownUntil ? 1.0f : 1.0f - std::chrono::duration_cast<std::chrono::duration<float>>(Now - m_ShownUntil).count() / std::chrono::duration_cast<std::chrono::duration<float>>(FADE_FOR).count();

	m_pGraphics->MapScreen(CScreenRect(0.0f, 0.0f, ScreenWidth, ScreenHeight));
	m_pGraphics->TextureClear();
	m_pGraphics->QuadsBegin();

	if(Corner)
	{
		DrawRoundRect(Region.m_TopLeft.x, Region.m_TopLeft.y, Region.Width(), Region.Height(), CORNER_RADIUS * Unit, 0.0f, 0.0f, 0.0f, 0.55f * Alpha);
		if(HasMenuRegion)
		{
			DrawRoundRect(MenuRegion.m_TopLeft.x, MenuRegion.m_TopLeft.y, MenuRegion.Width(), MenuRegion.Height(), CORNER_RADIUS * Unit, 0.0f, 0.0f, 0.0f, 0.55f * Alpha);
		}
	}
	else
	{
		// The strip: dark under the bar, nothing above it, and the way from one
		// to the other drawn in one quad with a colour at every corner.
		const float FadeTop = std::max(Region.m_TopLeft.y - FADE_HEIGHT * Unit, 0.0f);
		const ColorRGBA Clear = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
		const ColorRGBA Dark = ColorRGBA(0.0f, 0.0f, 0.0f, 0.72f * Alpha);
		m_pGraphics->SetColor4(Clear, Clear, Dark, Dark);
		IGraphics::CQuadItem Fade(0.0f, FadeTop, ScreenWidth, Region.m_TopLeft.y - FadeTop);
		m_pGraphics->QuadsDrawTL(&Fade, 1);
		DrawRect(0.0f, Region.m_TopLeft.y, ScreenWidth, Region.Height(), 0.0f, 0.0f, 0.0f, 0.72f * Alpha);
	}

	const bool Clicked = Input.m_MouseClicked || (Input.m_MousePressed && !m_WasPressed);
	if(Clicked)
	{
		m_PressedAt = Mouse;
		m_PressedWhen = Now;
		m_PressedOnBar = m_Hovered;
	}
	// A tap that began and ended between two frames is both at once, which is
	// what a slow frame does to a quick finger.
	const bool Released = (!Input.m_MousePressed && m_WasPressed) || (Input.m_MouseClicked && !Input.m_MousePressed);
	if(!Input.m_MousePressed)
	{
		m_Dragging = -1;
	}
	m_WasPressed = Input.m_MousePressed;

	int Pressed = -1;

	// The seek bar. The line somebody has to hit is thin, so what answers to a
	// press is the whole line of the window it stands in.
	if(Slider < Count)
	{
		const SItem &Item = pItems[Slider];
		const float Left = PADDING_X * Unit;
		const float Width = ScreenWidth - 2.0f * PADDING_X * Unit;
		const float Middle = Region.m_TopLeft.y + SeekHeight / 2.0f;
		const bool Over = !Item.m_Disabled && Mouse.y >= Region.m_TopLeft.y && Mouse.y <= Region.m_TopLeft.y + SeekHeight;
		const bool Held = m_Dragging == (int)Slider;
		if(Over && Clicked)
		{
			m_Dragging = (int)Slider;
		}
		float Value = std::clamp(Item.m_Value, 0.0f, 1.0f);
		if(m_Dragging == (int)Slider && pSliderValue != nullptr)
		{
			Value = std::clamp((Mouse.x - Left) / std::max(Width, 1.0f), 0.0f, 1.0f);
			*pSliderValue = Value;
			Pressed = (int)Slider;
		}
		const float TrackHeight = TRACK_HEIGHT * Unit;
		DrawRoundRect(Left, Middle - TrackHeight / 2.0f, Width, TrackHeight, TrackHeight / 2.0f, 1.0f, 1.0f, 1.0f, 0.3f * Alpha);
		DrawRoundRect(Left, Middle - TrackHeight / 2.0f, Width * Value, TrackHeight, TrackHeight / 2.0f, ACCENT.r, ACCENT.g, ACCENT.b, Alpha);
		const float Radius = (Held || Over ? KNOB_RADIUS_HELD : KNOB_RADIUS) * Unit;
		DrawDisc(vec2(Left + Width * Value, Middle), Radius, ACCENT.r, ACCENT.g, ACCENT.b, Alpha);
	}

	struct SLabel
	{
		float m_X;
		float m_Y;
		const char *m_pText;
		float m_Alpha;
	};
	std::vector<SLabel> vLabels;
	bool ToggleMenu = false;
	for(const SPlaced &Placed : vPlaced)
	{
		const bool IsMenuButton = Placed.m_Index == MenuButton;
		const SItem MenuItem = [&] {
			SItem Item;
			Item.m_Icon = EIcon::MENU;
			Item.m_Active = m_MenuOpen;
			return Item;
		}();
		const SItem &Item = IsMenuButton ? MenuItem : pItems[Placed.m_Index];
		const bool Over = !Item.m_Disabled && Mouse.x >= Placed.m_X && Mouse.x <= Placed.m_X + Placed.m_Width &&
				  Mouse.y >= Placed.m_Y && Mouse.y <= Placed.m_Y + Placed.m_Height;
		const bool WithText = !Corner && m_pTextRender != nullptr && Item.m_pText != nullptr;
		if(Item.m_Type == EItem::BUTTON || IsMenuButton)
		{
			const float Inset = 3.0f * Unit;
			if(Over || Item.m_Active)
			{
				const float Shade = Over && Input.m_MousePressed ? 0.35f : (Item.m_Active ? 0.28f : 0.18f);
				DrawRoundRect(Placed.m_X + Inset, Placed.m_Y + Inset, Placed.m_Width - 2.0f * Inset, Placed.m_Height - 2.0f * Inset,
					CORNER_RADIUS * Unit, 1.0f, 1.0f, 1.0f, Shade * Alpha);
			}
			if(Item.m_Icon != EIcon::NONE)
			{
				const float IconX = WithText ? Placed.m_X + TEXT_PADDING * Unit + ICON_SIZE * Unit / 2.0f : Placed.m_X + Placed.m_Width / 2.0f;
				DrawIcon(Item.m_Icon, vec2(IconX, Placed.m_Y + Placed.m_Height / 2.0f), ICON_SIZE * Unit, (Item.m_Disabled ? 0.35f : 1.0f) * Alpha);
			}
			if(Over && Clicked)
			{
				if(IsMenuButton)
					ToggleMenu = true;
				else
					Pressed = (int)Placed.m_Index;
			}
		}
		if(WithText)
		{
			const float TextWidth = m_pTextRender->TextWidth(TEXT_SIZE * Unit, Item.m_pText);
			const bool WithIcon = Item.m_Icon != EIcon::NONE && Item.m_Type == EItem::BUTTON;
			const float TextX = WithIcon ? Placed.m_X + 2.0f * TEXT_PADDING * Unit + ICON_SIZE * Unit : Placed.m_X + (Placed.m_Width - TextWidth) / 2.0f;
			vLabels.push_back({TextX, Placed.m_Y + (Placed.m_Height - TEXT_SIZE * Unit) / 2.0f, Item.m_pText, (Item.m_Disabled ? 0.4f : 1.0f) * Alpha});
		}
	}
	m_pGraphics->QuadsEnd();

	if(ToggleMenu)
	{
		m_MenuOpen = !m_MenuOpen;
	}
	else if(Clicked && !m_Hovered)
	{
		// A menu is closed by pressing anywhere that is not in it, which is
		// what a menu does everywhere.
		m_MenuOpen = false;
	}

	// A press that went down on the picture, stayed where it was and was let go
	// of again is a tap, and a tap on the picture is how a video player is told
	// to show its controls or to get out of the way.
	if(Released && !m_PressedOnBar && distance(Mouse, m_PressedAt) <= TAP_DISTANCE * Unit && Now - m_PressedWhen <= TAP_TIME)
	{
		if(ShownBeforeInput)
			Hide();
		else
			Show();
	}

	// The letters come after the boxes, because they go on top of them and
	// because the text render draws with its own textures.
	for(const SLabel &Label : vLabels)
	{
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, Label.m_Alpha);
		m_pTextRender->Text(Label.m_X, Label.m_Y, TEXT_SIZE * Unit, Label.m_pText, -1.0f);
	}
	if(m_pTextRender != nullptr)
	{
		m_pTextRender->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	return Pressed;
}
