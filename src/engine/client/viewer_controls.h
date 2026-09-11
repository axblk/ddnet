/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VIEWER_CONTROLS_H
#define ENGINE_CLIENT_VIEWER_CONTROLS_H

#include <base/vmath.h>

#include <chrono>
#include <cstddef>

class IGraphics;
class ITextRender;

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
		PLAY,
		PAUSE,
		RESTART,
		MINUS,
		PLUS,
		FIT,
		SAVE,
		SAVE_ALL,
		STOP,
	};

	enum class EItem
	{
		BUTTON,
		SLIDER,
		TEXT,
	};

	/**
	 * One thing on the bar: a button, the bar somebody drags to seek, or
	 * something written.
	 */
	struct SItem
	{
		EItem m_Type = EItem::BUTTON;
		EIcon m_Icon = EIcon::NONE;
		/** Shown where there is a text render, ignored where there is none. */
		const char *m_pText = nullptr;
		/** In pixels, or 0 for as wide as what is on it. */
		float m_Width = 0.0f;
		/** Where a slider stands, between 0 and 1. */
		float m_Value = 0.0f;
		bool m_Disabled = false;
	};

	/**
	 * What the pointer and the keyboard did this frame.
	 */
	struct SInput
	{
		vec2 m_MousePos = vec2(0.0f, 0.0f);
		bool m_MousePressed = false;
		/** Whether anything was typed, which counts as somebody being there. */
		bool m_KeyPressed = false;
	};

	/**
	 * @param pGraphics What it draws with.
	 * @param pTextRender What it writes with, or `nullptr` where there is no
	 * font to write with.
	 */
	void Init(IGraphics *pGraphics, ITextRender *pTextRender);

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
	 * the pointer can leave it alone while it is.
	 */
	bool Hovered() const { return m_Hovered; }

	/**
	 * Brings the bar back for a while, as any other use of it does. For what
	 * happens elsewhere and should still count as somebody being there.
	 */
	void Show();

private:
	IGraphics *m_pGraphics = nullptr;
	ITextRender *m_pTextRender = nullptr;
	// Where the pointer was, to tell it having moved from it being somewhere.
	vec2 m_LastMousePos = vec2(-1.0f, -1.0f);
	std::chrono::nanoseconds m_ShownUntil{};
	bool m_Hovered = false;
	// Which item is being dragged, so that the pointer may leave the bar while
	// it is - a slider that is let go of as soon as the pointer slips off it is
	// a slider nobody can use.
	int m_Dragging = -1;
	bool m_WasPressed = false;

	void DrawIcon(EIcon Icon, vec2 Center, float Size, float Alpha);
	void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a);
};

#endif // ENGINE_CLIENT_VIEWER_CONTROLS_H
