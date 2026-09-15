/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VIEWER_CONTROLS_H
#define ENGINE_CLIENT_VIEWER_CONTROLS_H

#include <base/vmath.h>

#include <engine/input.h>

#include <chrono>
#include <cstddef>
#include <vector>

class IGraphics;
class ITextRender;

/**
 * What two fingers do to the picture a viewer shows.
 *
 * Pinching to zoom and dragging with two fingers is what every program that
 * shows a picture on a touch screen does, and a viewer that does not is a
 * viewer nobody can look around in on a telephone. One finger is not answered
 * here: that is a tap or a drag, and what those mean is for whoever is showing
 * something - the bar takes the tap, the viewer takes the drag.
 *
 * It is used in one call per frame, and it keeps only what it needs to tell
 * this frame from the last one.
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
		 * Whether two fingers are on the picture. While they are, whoever
		 * also moves the view with one pointer leaves it alone: the window
		 * system reports the first finger as a pointer as well, and a view
		 * that is moved twice moves twice as far.
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

/**
 * The bar of controls a viewer draws over what it shows.
 *
 * A viewer is a program somebody looks at, so it has to be usable by somebody
 * who has not read anything about it: the keys it answers to are written
 * nowhere, and a window with nothing in it but a picture offers nothing to try.
 * This is the same bar a video player has, drawn by the viewer itself, which is
 * what makes the native viewers usable on their own. A page that brings its own
 * controls switches it off.
 *
 * It behaves the way a video player's bar does: it lies along the bottom of the
 * window, the seek bar has a line of its own above the buttons, it goes away
 * while nothing happens, and a tap or click on the picture brings it back or
 * sends it away again. Everything on it is at least as big as a fingertip, in
 * the pixels of the screen rather than of the drawing surface, so it stays that
 * big on a telephone that draws three pixels for every one it is measured in.
 *
 * It draws with quads and, where there is one, a text render - the map viewer
 * has none, because it needs nothing out of `data/` and a font is the one thing
 * that would change that. Everything it can say without letters it says with
 * shapes.
 *
 * It is used in one call per frame: the caller describes the items, and what
 * comes back is which of them was pressed. There is no state to keep in the
 * caller and nothing to register.
 */
class CViewerControls
{
public:
	/**
	 * What a button shows when there are no letters to show, and beside the
	 * letters where there are.
	 */
	enum class EIcon
	{
		NONE,
		/** Three lines: what everything else is behind. */
		MENU,
		/** A sparkle: everything a map has that is only there to look at. */
		DETAIL,
		/** Tiles: what a map is made of under what it looks like. */
		ENTITIES,
		PLAY,
		PAUSE,
		RESTART,
		MINUS,
		PLUS,
		FIT,
		SAVE,
		SAVE_ALL,
		STOP,
		EYE,
		/** Arrows to all four sides: a camera that is nobody's to follow. */
		FREEVIEW,
		FULLSCREEN,
	};

	/**
	 * Where the controls sit.
	 */
	enum class EPlacement
	{
		/**
		 * Along the bottom across the whole window, where a video player has
		 * them. For something that is being played: there is a seek bar, and
		 * what is under it is worth a row of its own.
		 */
		BOTTOM_BAR,
		/**
		 * A handful of icons in the top right corner, with the rest behind a
		 * menu that opens under them. For something that is simply being
		 * looked at, where a bar across the window would take away more of the
		 * picture than it is worth.
		 */
		CORNER,
	};

	enum class EItem
	{
		BUTTON,
		/**
		 * The bar somebody drags to seek. It is not put in the row with the
		 * buttons but on a line of its own above them, across the whole width,
		 * where a video player has it. There is room for one.
		 */
		SLIDER,
		TEXT,
		/** Empty room: what comes after it is pushed to the right. */
		SPACER,
	};

	/**
	 * One thing on the bar.
	 */
	struct SItem
	{
		EItem m_Type = EItem::BUTTON;
		EIcon m_Icon = EIcon::NONE;
		/** Shown where there is a text render, ignored where there is none. */
		const char *m_pText = nullptr;
		/** In screen pixels, or 0 for as wide as what is on it. */
		float m_Width = 0.0f;
		/** Where a slider stands, between 0 and 1. */
		float m_Value = 0.0f;
		bool m_Disabled = false;
		/** A button that is on, drawn as held down. */
		bool m_Active = false;
		/** Left out first where the window is too narrow for everything. */
		bool m_Optional = false;
		/**
		 * Left out altogether, for something this program cannot do where it
		 * is running. The caller keeps its list of items the same either way
		 * and says so here, rather than counting differently.
		 */
		bool m_Hidden = false;
		/**
		 * Put in the menu rather than in the row of buttons. What is asked for
		 * once and then left alone goes there, and so does a list too long to
		 * be a row - the players of a demo, say.
		 */
		bool m_InMenu = false;
		/**
		 * Opens its menu rather than being reported as pressed. Where nothing
		 * opens the first menu, the bar adds a button of its own at the end of
		 * the row for it.
		 */
		bool m_OpensMenu = false;
		/**
		 * Leaves the menu open when it is picked. For a row that changes
		 * something rather than doing something, so that two of them can be
		 * changed without opening the menu twice.
		 */
		bool m_KeepsMenu = false;
		/**
		 * Which menu this is in, or opens: items that say the same number
		 * belong together. A bar with two things to unfold - who to watch and
		 * what to make a video of - keeps them apart with this, and one with a
		 * single menu never has to say anything.
		 */
		int m_MenuId = 0;
	};

	/**
	 * What the pointer and the keyboard did this frame.
	 *
	 * A finger is a pointer here: the window system reports a touch as a click
	 * as well, and one finger on a bar is a click on it. What two fingers mean
	 * is up to whoever is showing something, and it never reaches this.
	 */
	struct SInput
	{
		vec2 m_MousePos = vec2(0.0f, 0.0f);
		bool m_MousePressed = false;
		/**
		 * Whether the button went down since the last frame. A frame can take
		 * longer than somebody's finger does, so the state alone would lose a
		 * quick tap altogether; this comes from the events and does not.
		 */
		bool m_MouseClicked = false;
		/** Whether anything was typed, which counts as somebody being there. */
		bool m_KeyPressed = false;
	};

	/**
	 * @param pGraphics What it draws with.
	 * @param pTextRender What it writes with, or `nullptr` where there is no
	 * font to write with.
	 */
	void Init(IGraphics *pGraphics, ITextRender *pTextRender);

	/** Where the controls sit. A bar along the bottom unless this says otherwise. */
	void SetPlacement(EPlacement Placement) { m_Placement = Placement; }

	/**
	 * Draws the bar and says what was done with it.
	 *
	 * @param pItems The items, left to right.
	 * @param Count How many there are.
	 * @param Input What the pointer and the keyboard did this frame.
	 * @param pSliderValue Where a slider that was dragged writes its new
	 * position, between 0 and 1. May be `nullptr` where there is no slider.
	 *
	 * @return The index of the item that was pressed, or -1 when none was. A
	 * slider that is being dragged answers with its own index every frame.
	 */
	int Render(const SItem *pItems, size_t Count, const SInput &Input, float *pSliderValue);

	/**
	 * Whether the pointer is over the bar, so that whoever also steers with
	 * the pointer can leave it alone while it is. True while a slider is being
	 * dragged, wherever the pointer has wandered off to.
	 */
	bool Hovered() const { return m_Hovered || m_Dragging >= 0; }

	/**
	 * Whether a slider is being held. What is being watched stands still
	 * while somebody drags along it: they are looking for a place in it, and
	 * a place that moves away while it is being pointed at is not one.
	 */
	bool Dragging() const { return m_Dragging >= 0; }

	/**
	 * Brings the bar back for a while, as any other use of it does. For what
	 * happens elsewhere and should still count as somebody being there.
	 */
	void Show();

	/** Sends the bar away, as a tap on the picture does. */
	void Hide();

	/** Whether the bar can be seen at the moment. */
	bool Shown() const;

private:
	IGraphics *m_pGraphics = nullptr;
	ITextRender *m_pTextRender = nullptr;
	EPlacement m_Placement = EPlacement::BOTTOM_BAR;
	bool m_MenuOpen = false;
	int m_OpenMenuId = 0;
	// Where the pointer was, to tell it having moved from it being somewhere.
	vec2 m_LastMousePos = vec2(-1.0f, -1.0f);
	std::chrono::nanoseconds m_ShownUntil{};
	bool m_Hovered = false;
	bool m_Started = false;
	// Which item is being dragged, so that the pointer may leave the bar while
	// it is - a slider that is let go of as soon as the pointer slips off it is
	// a slider nobody can use.
	int m_Dragging = -1;
	bool m_WasPressed = false;
	// Where the button went down and when, to tell a tap on the picture from
	// somebody dragging the view around.
	vec2 m_PressedAt = vec2(0.0f, 0.0f);
	std::chrono::nanoseconds m_PressedWhen{};
	bool m_PressedOnBar = false;

	// Where an item was drawn, so that what was pressed is worked out from the
	// same rectangles that were drawn rather than from a second guess at them.
	struct SPlaced
	{
		size_t m_Index;
		float m_X;
		float m_Y;
		float m_Width;
		float m_Height;
	};

	float Scale() const;
	void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a);
	void DrawRoundRect(float x, float y, float w, float h, float Radius, float r, float g, float b, float a);
	void DrawDisc(vec2 Center, float Radius, float r, float g, float b, float a);
	void DrawIcon(EIcon Icon, vec2 Center, float Size, float Alpha);
};

#endif // ENGINE_CLIENT_VIEWER_CONTROLS_H
