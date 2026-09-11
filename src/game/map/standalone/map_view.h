#ifndef GAME_MAP_STANDALONE_MAP_VIEW_H
#define GAME_MAP_STANDALONE_MAP_VIEW_H

#include <base/vmath.h>

#include <engine/graphics.h>

#include <game/layers.h>
#include <game/map/map_renderer.h>
#include <game/map/render_map.h>
#include <game/map/standalone/map_view_support.h>

#include <memory>

class CImageInfo;
class IEngineGraphics;
class IEngineGraphicsWindow;
class IKernel;
class IMap;
class IStorage;

/**
 * A map on a surface, and everything between the two: a kernel, the storage,
 * the jobs, the graphics and the map renderer. What it does not bring is the
 * program around it - taking one picture and exiting is one such program, a
 * window somebody looks around in is another.
 *
 * It needs nothing out of `data/` for itself: no fonts, no skins, no game
 * assets, only the map and the images the map names.
 */
class CStandaloneMapView
{
public:
	/**
	 * Where the view looks, how close, and at what moment of the envelopes.
	 */
	struct SRenderParams
	{
		vec2 m_Center = vec2(0.0f, 0.0f);
		float m_Zoom = 1.0f;
		int m_TimeOffsetMillis = 0;
		bool m_IgnoreParallax = false;
	};

	explicit CStandaloneMapView(const char *pLogContext);
	~CStandaloneMapView();

	CStandaloneMapView(const CStandaloneMapView &) = delete;
	CStandaloneMapView &operator=(const CStandaloneMapView &) = delete;

	/**
	 * Brings up the kernel, the storage and the jobs. Opening a window comes
	 * after this, so that whatever else belongs in the kernel - a console, the
	 * settings - is registered before anything reads a setting.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool Init(int NumArgs, const char **ppArguments);

	/**
	 * Brings up the graphics and the map renderer.
	 *
	 * @param Width The width of the surface that is drawn into.
	 * @param Height The height of the surface that is drawn into.
	 * @param pWindow The window to draw into, which the view takes over. A
	 * program that wants no window on any screen hands it the window-less
	 * surface, and only that program then has to link a window system.
	 * @param Windowed Whether that window is on a screen, which decides
	 * whether the frames are paced by a display.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool OpenWindow(int Width, int Height, IEngineGraphicsWindow *pWindow, bool Windowed);

	/**
	 * Loads a map, replacing whatever was loaded before it.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool LoadMap(const char *pPath, int StorageType);
	bool MapLoaded() const { return m_pMap != nullptr; }

	/**
	 * Draws one frame. Putting it on the screen is the caller's business:
	 * a window swaps it, a picture is read back with `SaveImage`.
	 */
	void Render(const SRenderParams &Params);

	/**
	 * The parameters that put the given rectangle of the world exactly on the
	 * surface. The rectangle has to have the shape of the surface, or what is
	 * drawn comes out stretched.
	 */
	SRenderParams ParamsForWorldRect(vec2 TopLeft, vec2 Size) const;

	/**
	 * How much of the world the view shows at zoom 1, in world units.
	 *
	 * This is not what the game would show. A game keeps the area of its view
	 * the same whatever shape the window has, so that nobody sees further by
	 * making their window wider; a map viewer has nobody to be fair to, and a
	 * window that is wider than it is tall is simply asked to show more map.
	 * The height is the one the game's view has on a 16:9 screen, so that a
	 * zoom of one still means here what it has always meant.
	 */
	vec2 ViewSize() const;

	/**
	 * Finishes the frame that was drawn and reads it back off the graphics
	 * card. Where there is a window, this is also what puts the frame on it.
	 *
	 * @param Image Where the frame is put. What is handed in is reused when it
	 * already has the size and the format of the frame, and freed otherwise.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool ReadFrame(CImageInfo &Image);

	/**
	 * Draws the whole map at one pixel per world unit - 32 pixels per tile -
	 * and writes it as a PNG, in as many pieces as it takes: the surface is
	 * moved over the map, and the rows of the picture go out as they are
	 * drawn. That is what makes a picture possible that neither fits in one
	 * texture nor in memory.
	 *
	 * @param pPath The file to write, as a path of the operating system.
	 * @param TimeOffsetMillis The moment of the envelopes to draw, so that every
	 * piece of the picture shows the map at the same moment.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool SaveFullImage(const char *pPath, int TimeOffsetMillis);

	/**
	 * Draws one frame of the view, reads it back off the graphics card and
	 * writes it as a PNG.
	 *
	 * The frame is drawn for this alone, beside the window rather than in it,
	 * so that what is written is the view and only the view - a viewer draws
	 * its own controls over the window, and nobody wants those in their
	 * picture.
	 *
	 * @param Params Where the view looks.
	 * @param pPath The file to write, as a path of the operating system.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool SaveImage(const SRenderParams &Params, const char *pPath);

	void Shutdown();

	/**
	 * Tells the view that the surface it draws into has another size now.
	 */
	void OnResize(int Width, int Height);

	/**
	 * The size of the game layer in world units, or the size of the surface
	 * where the map has no game layer.
	 */
	vec2 MapWorldSize();
	/**
	 * The zoom at which the whole map fits on the surface, with room left over
	 * beside it wherever the surface and the map are not the same shape. What
	 * a picture of a map wants.
	 */
	float FitZoom();

	/**
	 * The zoom at which the map covers the surface, with whatever does not fit
	 * hanging over the edges. What a window wants: nobody looking at a map in
	 * one wants a frame of nothing around it, least of all on a telephone held
	 * upright, where a map that fits is a map in a letterbox.
	 */
	float FillZoom();

	/**
	 * The size of the surface that is actually drawn into, which is not always
	 * the size that was asked for.
	 */
	int Width() const { return m_Width; }
	int Height() const { return m_Height; }

	IStorage *Storage() { return m_pStorage.get(); }
	IGraphics *Graphics();
	IEngineGraphicsWindow *Window() { return m_pWindow; }
	IKernel *Kernel() { return m_pKernel.get(); }

private:
	void UnloadMap();
	/**
	 * Draws one frame into a texture of the view's own and reads that back, so
	 * that nothing of it reaches the window. Without a window there is nothing
	 * to keep it from, and the frame is drawn and read the ordinary way.
	 *
	 * @param Params Where that frame looks.
	 * @param Image Where the frame is put, as in `ReadFrame`.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool RenderAsideAndRead(const SRenderParams &Params, CImageInfo &Image);
	/**
	 * Creates the texture `RenderAsideAndRead` draws into, once and again
	 * whenever the surface has another size.
	 *
	 * @return `true` when there is one to draw into.
	 */
	bool EnsureAsideTarget();

	const char *m_pLogContext;
	int m_Width = 0;
	int m_Height = 0;
	bool m_Windowed = false;
	IGraphics::CTextureHandle m_AsideTarget;
	int m_AsideWidth = 0;
	int m_AsideHeight = 0;

	std::unique_ptr<IKernel> m_pKernel;
	std::unique_ptr<IStorage> m_pStorage;
	MapViewSupport::CMinimalEngine *m_pEngine = nullptr;
	IEngineGraphicsWindow *m_pWindow = nullptr;
	IEngineGraphics *m_pGraphics = nullptr;

	std::unique_ptr<IMap> m_pMap;
	CLayers m_Layers;
	std::unique_ptr<MapViewSupport::CToolMapImages> m_pMapImages;
	std::unique_ptr<MapViewSupport::CMapRenderEnvelopeEval> m_pEnvelopeEval;
	CRenderMap m_RenderMap;
	CMapRenderer m_MapRenderer;
};

#endif // GAME_MAP_STANDALONE_MAP_VIEW_H
