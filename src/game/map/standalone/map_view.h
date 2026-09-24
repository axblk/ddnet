#ifndef GAME_MAP_STANDALONE_MAP_VIEW_H
#define GAME_MAP_STANDALONE_MAP_VIEW_H

#include <base/vmath.h>

#include <engine/gfx/image_loader.h>
#include <engine/graphics.h>
#include <engine/http.h>

#include <game/layers.h>
#include <game/map/map_renderer.h>
#include <game/map/render_map.h>
#include <game/map/standalone/map_view_support.h>

#include <chrono>
#include <memory>
#include <vector>

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
		/** The view's size at zoom 1 in world units, or zero for the surface's shape. */
		vec2 m_ViewSize = vec2(0.0f, 0.0f);
		/** The part of the view to draw, as fractions of it, for pictures drawn in pieces. */
		CScreenRect m_Window = CScreenRect(0.0f, 0.0f, 1.0f, 1.0f);
		/** Whether layers the map marks as detail are drawn. */
		bool m_HighDetail = true;
		/**
		 * How strongly the entity overlay is drawn over the design, from 0 to
		 * 100. At 100 the design is left out altogether and what is left is
		 * what the map does rather than what it looks like.
		 */
		int m_EntityOverlayVal = 0;
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
	 * How much of the world the view shows at zoom 1, in world units. Unlike
	 * in the game, a wider window shows more of the map; the height is that of
	 * the game's view on a 16:9 screen.
	 */
	vec2 ViewSize() const;

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
	 * Starts the same picture in steps, so that a browser's only thread gets
	 * its turn between them. `StepFullImage` draws the pieces.
	 *
	 * @param pPath The file to write, as a path of the operating system.
	 * @param TimeOffsetMillis The moment of the envelopes to draw.
	 * @param PixelBudget How many pixels the picture may have at most, or 0
	 * for the map at its own size. A map with more than this is drawn smaller,
	 * keeping its shape; one with fewer is drawn as it is.
	 *
	 * @return `true` when there is a picture to step through.
	 */
	bool BeginFullImage(const char *pPath, int TimeOffsetMillis, size_t PixelBudget = 0);

	/**
	 * The pixel budget of a viewer's picture of the whole map. Large maps at
	 * their own size have billions of pixels, more than a browser can hold.
	 */
	static constexpr size_t VIEWER_FULL_IMAGE_PIXELS = 64 * 1000 * 1000;

	/**
	 * Draws pieces of the picture until the time given is up, at least one.
	 *
	 * @param Budget How long to draw for before coming back.
	 *
	 * @return `true` while there is more to draw. On `false` the picture is
	 * finished or failed, which `FullImageFailed` tells apart.
	 */
	bool StepFullImage(std::chrono::nanoseconds Budget);

	/** Whether a picture of the whole map is being drawn. */
	bool FullImageRunning() const { return m_FullImage.m_Running; }

	/** Whether the last one stopped because something went wrong. */
	bool FullImageFailed() const { return m_FullImage.m_Failed; }

	/** How far the picture has got, from 0 to 1. */
	float FullImageProgress() const;

	/**
	 * Draws one frame of the view beside the window, without the viewer's
	 * controls, and writes it as a PNG.
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
	 * The zoom at which the whole map fits on the surface.
	 */
	float FitZoom();

	/** The zoom at which the map covers the surface, cropping what does not fit. */
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
	/** `ViewSize` for a picture whose shape is not the surface's. */
	static vec2 ViewSizeForAspect(float Aspect);
	/** Gives up on the picture of the whole map and lets go of what it held. */
	void CancelFullImage();
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
	 * Draws one frame into a texture of the view's own and reads that back, so
	 * that nothing of it reaches the window. Without a window there is nothing
	 * to keep it from, and the frame is drawn and read the ordinary way.
	 *
	 * @param Params Where that frame looks.
	 * @param Image Where the frame is put, as in `ReadFrame`.
	 * @param Width How wide to draw it, or zero for the surface's own width.
	 * @param Height How tall to draw it, or zero for the surface's own height.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool RenderAsideAndRead(const SRenderParams &Params, CImageInfo &Image, int Width = 0, int Height = 0);
	/**
	 * Creates the texture `RenderAsideAndRead` draws into, once and again
	 * whenever what is drawn has another size.
	 *
	 * @param Width How wide the texture has to be.
	 * @param Height How tall the texture has to be.
	 *
	 * @return `true` when there is one to draw into.
	 */
	bool EnsureAsideTarget(int Width, int Height);
	/** Draws one piece of the picture and puts it in the band. */
	bool StepOneFullImagePiece();
	/** Closes the file, lets the band go, and answers what to report. */
	bool EndFullImage(bool Success);

	/**
	 * A picture of the whole map while it is being drawn: the file it goes to,
	 * the row of pieces being gathered, and where in the sweep it is.
	 */
	struct SFullImage
	{
		bool m_Running = false;
		bool m_Failed = false;
		CPngRowWriter m_Writer;
		std::vector<uint8_t> m_vBand;
		CImageInfo m_Image;
		SRenderParams m_Whole;
		size_t m_FullWidth = 0;
		size_t m_FullHeight = 0;
		size_t m_PieceWidth = 0;
		size_t m_PieceHeight = 0;
		size_t m_Top = 0;
		size_t m_Left = 0;
	};
	SFullImage m_FullImage;

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
	IEngineHttp *m_pHttp = nullptr;
	CAssetLoader m_AssetLoader;
	std::unique_ptr<MapViewSupport::CToolMapImages> m_pMapImages;
	std::unique_ptr<MapViewSupport::CMapRenderEnvelopeEval> m_pEnvelopeEval;
	CRenderMap m_RenderMap;
	CMapRenderer m_MapRenderer;
};

#endif // GAME_MAP_STANDALONE_MAP_VIEW_H
