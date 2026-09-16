#ifndef GAME_MAP_DOCUMENT_VIEW_H
#define GAME_MAP_DOCUMENT_VIEW_H

#include <base/vmath.h>

#include <engine/graphics.h>

#include <algorithm>
#include <cmath>

namespace map_document
{
	/**
	 * Where the editor looks, as arithmetic and nothing else.
	 *
	 * This is the one piece of the editor that a tool in the page needs to
	 * agree with to the pixel: where somebody clicked is a place in the world,
	 * and the place the brush lands has to be the place under the pointer.
	 * That is why it is here rather than in the renderer - it is asked more
	 * often than it is drawn, it has no business knowing about a graphics card,
	 * and it can be held against the sums it has to agree with in a test.
	 *
	 * The view is the game's own view: how much world a screen shows at zoom 1
	 * is what a player would see, so that a map looks in the editor the way it
	 * will look in the game. A larger zoom shows more world, which is the way
	 * round the renderer and the map viewer already count it.
	 */
	class CView
	{
	public:
		/**
		 * How far the view may be taken either way, so that neither a wheel
		 * that keeps turning nor a page that asks for anything can put the map
		 * where it is no longer to be found.
		 */
		static constexpr float MIN_ZOOM = 0.01f;
		static constexpr float MAX_ZOOM = 1000.0f;

		/** The size of what is drawn into, in pixels. */
		void SetSurface(int Width, int Height)
		{
			m_SurfaceWidth = std::max(1, Width);
			m_SurfaceHeight = std::max(1, Height);
		}
		int SurfaceWidth() const { return m_SurfaceWidth; }
		int SurfaceHeight() const { return m_SurfaceHeight; }
		float Aspect() const { return (float)m_SurfaceWidth / (float)m_SurfaceHeight; }

		vec2 Center() const { return m_Center; }
		void SetCenter(vec2 Center) { m_Center = Center; }

		float Zoom() const { return m_Zoom; }
		void SetZoom(float Zoom) { m_Zoom = std::clamp(Zoom, MIN_ZOOM, MAX_ZOOM); }

		/**
		 * How much world the view shows at zoom 1, in world units. What is
		 * actually shown is this times the zoom - which is also what the
		 * renderer is handed, so that the two cannot drift apart.
		 *
		 * This is not quite what the game would show. A game keeps the area of
		 * its view the same whatever shape the window has, so that nobody sees
		 * further by making their window wider; an editor has nobody to be
		 * fair to, and a window that is wider than it is tall is simply asked
		 * to show more map. The height is the one the game's view has on a
		 * 16:9 screen, so that a zoom of one still means what it means in the
		 * map viewer, which decided this first.
		 */
		vec2 ViewSize() const
		{
			float Width, Height;
			CalcViewSize(16.0f / 9.0f, 1.0f, 0.0f, &Width, &Height);
			return vec2(Height * Aspect(), Height);
		}

		/** What is on the screen, in world units. */
		vec2 VisibleSize() const { return ViewSize() * m_Zoom; }

		/** Where a pixel of the surface is in the world. */
		vec2 ScreenToWorld(vec2 Pixel) const
		{
			const vec2 Visible = VisibleSize();
			return m_Center + vec2(
						  (Pixel.x / (float)m_SurfaceWidth - 0.5f) * Visible.x,
						  (Pixel.y / (float)m_SurfaceHeight - 0.5f) * Visible.y);
		}

		/** Where a place in the world is on the surface. */
		vec2 WorldToScreen(vec2 World) const
		{
			const vec2 Visible = VisibleSize();
			return vec2(
				((World.x - m_Center.x) / Visible.x + 0.5f) * (float)m_SurfaceWidth,
				((World.y - m_Center.y) / Visible.y + 0.5f) * (float)m_SurfaceHeight);
		}

		/** Which tile a pixel of the surface is over, as whole tiles. */
		ivec2 ScreenToTile(vec2 Pixel) const
		{
			const vec2 World = ScreenToWorld(Pixel);
			return ivec2((int)std::floor(World.x / 32.0f), (int)std::floor(World.y / 32.0f));
		}

		/** Moves the view by a distance on the surface, which is dragging it. */
		void MoveByPixels(vec2 Pixels)
		{
			const vec2 Visible = VisibleSize();
			m_Center += vec2(Pixels.x / (float)m_SurfaceWidth * Visible.x, Pixels.y / (float)m_SurfaceHeight * Visible.y);
		}

		/**
		 * Zooms about a place on the surface, leaving what is under it where
		 * it is. A wheel over the pointer has to zoom towards the pointer -
		 * anything else and the map slides away from what is being looked at.
		 */
		void ZoomAt(vec2 Pixel, float Factor)
		{
			const vec2 Was = ScreenToWorld(Pixel);
			SetZoom(m_Zoom * Factor);
			const vec2 Now = ScreenToWorld(Pixel);
			m_Center += Was - Now;
		}

		/**
		 * The whole of something on the screen, with room left over beside it
		 * wherever the surface and it are not the same shape. What a picture
		 * of a map wants.
		 */
		void Fit(vec2 WorldSize)
		{
			const vec2 View = ViewSize();
			m_Center = WorldSize / 2.0f;
			SetZoom(std::max(WorldSize.x / View.x, WorldSize.y / View.y));
		}

		/**
		 * The same thing covering the screen, with whatever does not fit
		 * hanging over the edges. What a window wants.
		 */
		void Fill(vec2 WorldSize)
		{
			const vec2 View = ViewSize();
			m_Center = WorldSize / 2.0f;
			SetZoom(std::min(WorldSize.x / View.x, WorldSize.y / View.y));
		}

	private:
		int m_SurfaceWidth = 1;
		int m_SurfaceHeight = 1;
		vec2 m_Center = vec2(0.0f, 0.0f);
		float m_Zoom = 1.0f;
	};
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_VIEW_H
