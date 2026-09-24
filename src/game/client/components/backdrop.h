/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_BACKDROP_H
#define GAME_CLIENT_COMPONENTS_BACKDROP_H

#include <base/color.h>

#include <engine/graphics.h>

#include <game/client/component.h>

#include <optional>

class CUIRect;

/**
 * The blurred picture of the scene behind the boxes drawn over it: the
 * scoreboard, the statboard, the message of the day, the menus and the
 * console. The frame draws the scene into it, blurs it once something wants
 * it, and every box paints its own piece of it before its tint.
 */
class CBackdrop : public CComponent
{
	enum
	{
		// Halving steps from the screen down to the eighth the blur runs on.
		NUM_DOWNSAMPLES = 2,
	};
	IGraphics::CTextureHandle m_SceneTexture;
	IGraphics::CTextureHandle m_OverlayTexture;
	IGraphics::CTextureHandle m_aDownsampleTextures[NUM_DOWNSAMPLES];
	IGraphics::CTextureHandle m_aBlurTextures[2];
	int m_Width = 0;
	int m_Height = 0;
	bool m_SceneActive = false;
	bool m_OverlayActive = false;
	bool m_Ready = false;

	void DestroyTextures();
	bool EnsureTextures();
	bool TexturesValid() const;
	bool RenderTexture(IGraphics::CTextureHandle Target, IGraphics::CTextureHandle Source, std::optional<IGraphics::EBlurDirection> BlurDirection);
	bool BlurInto(IGraphics::CTextureHandle Source);

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnWindowResize() override;
	void OnShutdown() override;

	/**
	 * Starts drawing the scene into the backdrop instead of onto the screen.
	 *
	 * @param ClearColor What the scene is cleared to.
	 * @param Needed Whether anything will want the backdrop this frame.
	 *
	 * @return `true` if the scene goes into the backdrop, `false` if it goes
	 * to the screen as usual and still has to be cleared.
	 */
	bool Begin(ColorRGBA ClearColor, bool Needed);
	/**
	 * Whether the scene is still being drawn into the backdrop, between
	 * `Begin` and `Finish`.
	 */
	bool DrawingScene() const { return m_SceneActive; }
	/**
	 * Ends the scene and blurs it if anything wants it blurred. Everything
	 * drawn over the scene after this goes into a second picture, so whatever
	 * is drawn last can still have a blurred copy of all of it.
	 *
	 * @param Blur Whether to blur the scene.
	 */
	void Finish(bool Blur);
	/**
	 * Blurs everything drawn over the scene so far and puts it on the screen.
	 * Whoever is drawn after this gets a blurred picture of all of it, which is
	 * what the console needs to sit over the game and over the menu alike.
	 *
	 * @return `true` if a blurred backdrop is available afterwards.
	 */
	bool Capture();
	/**
	 * Puts what was drawn over the scene on the screen unblurred. Called once
	 * at the end of the frame for the case where nothing captured it.
	 */
	void Present();
	/**
	 * Paints the blurred backdrop in the shape a box over it is about to be drawn
	 * in, so that the blur ends exactly where the box does.
	 */
	void RenderRegion(const CUIRect &Rect, int Corners, float Rounding);
	/**
	 * Draws a box that sits straight over the scene: the backdrop, then the tint,
	 * in one shape. A box inside such a box is drawn plainly, since painting the
	 * backdrop again would wipe out the tint it sits on.
	 */
	void DrawSurface(const CUIRect &Rect, ColorRGBA Color, int Corners, float Rounding);
};

#endif
