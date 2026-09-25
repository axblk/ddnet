/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VIEWER_CONTROLS_H
#define ENGINE_CLIENT_VIEWER_CONTROLS_H

#include <base/vmath.h>

#include <engine/graphics.h>
#include <engine/input.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

class ITextRender;

/**
 * The controls the native viewers draw over their picture, like a video
 * player's: they fade out while nothing happens and a tap on the picture
 * toggles them. Drawn with quads, and with text where there is a text render
 * (the map viewer has none).
 *
 * Called once per frame with the items; answers which one was pressed.
 */
class CViewerControls
{
public:
	enum class EIcon
	{
		NONE,
		MENU,
		DETAIL,
		ENTITIES,
		PLAY,
		PAUSE,
		RESTART,
		MINUS,
		PLUS,
		FIT,
		ZOOM_RESET,
		SAVE,
		SAVE_ALL,
		STOP,
		EYE,
		FREEVIEW,
		CLIP_START,
		CLIP_END,
		CLIP_CLEAR,
		FULLSCREEN,
		VOLUME,
		VOLUME_OFF,
	};

	enum class EPlacement
	{
		/** Along the bottom, with the seek bar above the buttons. */
		BOTTOM_BAR,
		/** A few icons in the top right corner, the menu under them. */
		CORNER,
	};

	enum class EItem
	{
		BUTTON,
		/** The seek bar, on a line of its own above the buttons. One at most. */
		SLIDER,
		TEXT,
		/** Empty room: what comes after it is pushed to the right. */
		SPACER,
	};

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
		/** A marked stretch of a slider, between 0 and 1; none if equal. */
		float m_RangeStart = 0.0f;
		float m_RangeEnd = 0.0f;
		bool m_Disabled = false;
		/** A button that is on, drawn as held down. */
		bool m_Active = false;
		/** Left out first where the window is too narrow for everything. */
		bool m_Optional = false;
		/** Left out altogether. */
		bool m_Hidden = false;
		/** In the menu `m_MenuId` rather than in the row. */
		bool m_InMenu = false;
		/** Opens the menu `m_MenuId` rather than being reported as pressed. */
		bool m_OpensMenu = false;
		/** Leaves its menu open when picked, for a setting. */
		bool m_KeepsMenu = false;
		int m_MenuId = 0;
	};

	/** What the pointer (or one finger) and the keyboard did this frame. */
	struct SInput
	{
		vec2 m_MousePos = vec2(0.0f, 0.0f);
		bool m_MousePressed = false;
		/** Whether the button went down since the last frame, from the events. */
		bool m_MouseClicked = false;
		bool m_KeyPressed = false;
	};

	/**
	 * @param pGraphics What it draws with.
	 * @param pTextRender What it writes with, or `nullptr` where there is no
	 * font to write with.
	 */
	void Init(IGraphics *pGraphics, ITextRender *pTextRender);

	void SetPlacement(EPlacement Placement) { m_Placement = Placement; }

	/**
	 * Draws the controls.
	 *
	 * @param pItems The items, left to right.
	 * @param Count How many there are.
	 * @param Input What the pointer and the keyboard did this frame.
	 * @param pSliderValue Receives the position a slider was dragged to.
	 *
	 * @return The index of the item that was pressed, or -1. A slider that is
	 * being dragged answers every frame.
	 */
	int Render(const SItem *pItems, size_t Count, const SInput &Input, float *pSliderValue);

	/** Whether the pointer is over the controls or dragging a slider. */
	bool Hovered() const { return m_Hovered || m_Dragging >= 0; }

	/** Whether a slider is being dragged. */
	bool Dragging() const { return m_Dragging >= 0; }

	/** Shows the controls for a while, as any use of them does. */
	void Show();
	void Hide();

private:
	IGraphics *m_pGraphics = nullptr;
	ITextRender *m_pTextRender = nullptr;
	EPlacement m_Placement = EPlacement::BOTTOM_BAR;
	bool m_MenuOpen = false;
	int m_OpenMenuId = 0;
	vec2 m_LastMousePos = vec2(-1.0f, -1.0f);
	std::chrono::nanoseconds m_ShownUntil{};
	bool m_Hovered = false;
	bool m_Started = false;
	// The slider being dragged, held while the pointer leaves the bar.
	int m_Dragging = -1;
	bool m_WasPressed = false;
	// Where and when the button went down, to tell a tap from a drag.
	vec2 m_PressedAt = vec2(0.0f, 0.0f);
	std::chrono::nanoseconds m_PressedWhen{};
	bool m_PressedOnBar = false;

	// Where an item is drawn; presses are tested against the same rectangles.
	struct SPlaced
	{
		size_t m_Index;
		float m_X;
		float m_Y;
		float m_Width;
		float m_Height;
	};
	struct SLayout
	{
		float m_Unit = 1.0f;
		float m_ButtonSize = 0.0f;
		float m_IconSize = 0.0f;
		float m_SeekHeight = 0.0f;
		// The index of the slider, or the item count.
		size_t m_Slider = 0;
		CScreenRect m_Region = CScreenRect(0.0f, 0.0f, 0.0f, 0.0f);
		CScreenRect m_MenuRegion = CScreenRect(0.0f, 0.0f, 0.0f, 0.0f);
		bool m_HasMenu = false;
		std::vector<SPlaced> m_vPlaced;
	};
	struct SLabel
	{
		float m_X;
		float m_Y;
		const char *m_pText;
		float m_Alpha;
	};

	float Scale() const;
	float ItemWidth(const SItem &Item, const SLayout &Layout) const;
	void LayoutCorner(const SItem *pItems, size_t Count, SLayout &Layout) const;
	void LayoutBar(const SItem *pItems, size_t Count, SLayout &Layout) const;
	void LayoutMenu(const SItem *pItems, size_t Count, size_t Owner, SLayout &Layout) const;
	void DrawBackground(const SLayout &Layout, float Alpha);
	bool RenderSlider(const SItem &Item, const SLayout &Layout, vec2 Mouse, bool Clicked, float Alpha, float *pSliderValue);
	void RenderItems(const SItem *pItems, const SLayout &Layout, const SInput &Input, vec2 Mouse, bool Clicked, float Alpha, std::vector<SLabel> &vLabels, int &Pressed, std::optional<int> &PressedMenuId);
	void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a);
	void DrawRoundRect(float x, float y, float w, float h, float Radius, float r, float g, float b, float a);
	void DrawDisc(vec2 Center, float Radius, float r, float g, float b, float a);
	void DrawIcon(EIcon Icon, vec2 Center, float Size, float Alpha);
};

#endif // ENGINE_CLIENT_VIEWER_CONTROLS_H
