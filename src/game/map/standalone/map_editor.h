#ifndef GAME_MAP_STANDALONE_MAP_EDITOR_H
#define GAME_MAP_STANDALONE_MAP_EDITOR_H

#include <base/vmath.h>

#include <game/map/document/document.h>
#include <game/map/document/edit.h>
#include <game/map/document/view.h>
#include <game/map/document_images.h>
#include <game/map/document_render.h>
#include <game/map/standalone/map_view.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class IEngineGraphicsWindow;
class IGraphics;
class IStorage;

/**
 * A map being edited, on a surface, with everything between the two.
 *
 * This is the whole of the editor that is not an interface: the maps that are
 * open, what was done to them, where each of them is being looked at, and the
 * drawing of the one in front. It knows nothing about a pointer, a key or a
 * panel - those belong to whoever put the surface in front of somebody, which
 * in a browser is the page.
 *
 * **Several maps, one editor.** Two editors side by side on a page is not
 * something anybody wants; two maps open at once is. So everything here takes
 * the number of the map it is about, there is one that is in front, and
 * changing which costs a call rather than a restart. What hangs on the map -
 * its history, where it is being looked at, what of it is shown - stays with
 * it, so that coming back to a tab finds it as it was left.
 */
class CMapEditor
{
public:
	/**
	 * What is shown of a map besides the map: the part of the view that is
	 * the editor's own rather than the document's.
	 */
	class CDisplay
	{
	public:
		/** Whether the layers the map marks as detail are drawn. */
		bool m_HighDetail = true;
		/** How strongly the physics layers are drawn over the design, 0 to 100. */
		int m_EntityOverlayVal = 0;
		/**
		 * Whether the envelopes run. An editor that is not animating shows
		 * one moment of them rather than none: a colour envelope then says
		 * what it does at that moment instead of leaving the layer black.
		 */
		bool m_Animate = false;
		int m_TimeOffsetMillis = 0;
		/**
		 * Which layers are switched off, as the number of the group and the
		 * number of the layer in it. Hiding is a thing about looking rather
		 * than a thing about the map, so it lives here and not in the
		 * document: it survives no undo and goes into no file.
		 *
		 * Because it names layers by their place, moving a layer moves what
		 * is hidden with the place rather than with the layer. That is the
		 * cheap answer, and the one an editor can afford to have wrong for
		 * one click.
		 */
		std::vector<std::pair<size_t, size_t>> m_vHidden;
		/** How many tiles apart the lines of the grid are, 0 for no grid. */
		int m_Grid = 0;
		/**
		 * The rectangle of tiles that is marked, for a gesture that is about
		 * an area: taking a piece of a layer into the brush, filling it,
		 * rubbing it out. Empty while nothing is marked.
		 */
		CDocumentRenderer::CParams::CMarked m_Marked;

		/** Whether that layer is drawn. */
		bool Visible(size_t Group, size_t Layer) const
		{
			return std::find(m_vHidden.begin(), m_vHidden.end(), std::make_pair(Group, Layer)) == m_vHidden.end();
		}
		/** Switches that layer on or off. */
		void SetVisible(size_t Group, size_t Layer, bool Visible)
		{
			const auto It = std::find(m_vHidden.begin(), m_vHidden.end(), std::make_pair(Group, Layer));
			if(Visible && It != m_vHidden.end())
				m_vHidden.erase(It);
			else if(!Visible && It == m_vHidden.end())
				m_vHidden.emplace_back(Group, Layer);
		}
	};

	explicit CMapEditor(const char *pLogContext);
	~CMapEditor();

	CMapEditor(const CMapEditor &) = delete;
	CMapEditor &operator=(const CMapEditor &) = delete;

	/**
	 * Brings up the kernel, the storage and the jobs.
	 *
	 * @param NumArgs How many arguments the program was started with.
	 * @param ppArguments Those arguments, for the storage to find its paths.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool Init(int NumArgs, const char **ppArguments);

	/**
	 * Brings up the graphics.
	 *
	 * @param Width The width of the surface that is drawn into.
	 * @param Height The height of the surface that is drawn into.
	 * @param pWindow The window to draw into, which the editor takes over.
	 * @param Windowed Whether that window is on a screen, which decides
	 * whether the frames are paced by a display.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool OpenWindow(int Width, int Height, IEngineGraphicsWindow *pWindow, bool Windowed);

	/**
	 * Opens a map from a file and puts it in front.
	 *
	 * @param pPath Where the file is.
	 * @param StorageType Which of the storage's directories to look in.
	 *
	 * @return The number of the map, or -1 after reporting what went wrong.
	 */
	int Open(const char *pPath, int StorageType);

	/**
	 * An empty map with a game layer, which is what "new map" means.
	 *
	 * @param Width How many tiles wide the game layer is.
	 * @param Height How many tiles tall the game layer is.
	 * @param pName What to call the map.
	 *
	 * @return The number of the map.
	 */
	int Create(int Width, int Height, const char *pName);

	/**
	 * Writes a map to a file, in the format the game reads.
	 *
	 * @param Id The number of the map.
	 * @param pPath Where to write it.
	 * @param StorageType Which of the storage's directories to write in.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool Save(int Id, const char *pPath, int StorageType);

	/**
	 * Closes a map and gives up everything it held. The one in front becomes
	 * whichever is left, or none.
	 *
	 * @param Id The number of the map.
	 *
	 * @return `true` if there was such a map.
	 */
	bool Close(int Id);

	/** How many maps are open. */
	size_t Count() const { return m_vpMaps.size(); }
	/** The number of the map at a place in the list, or -1. */
	int IdAt(size_t Index) const;
	/** The number of the map that is in front, or -1 if none is open. */
	int Active() const { return m_Active; }
	/**
	 * Puts a map in front.
	 *
	 * @param Id The number of the map.
	 *
	 * @return `true` if there was such a map.
	 */
	bool SetActive(int Id);

	/** What the map is called, or an empty string. */
	const char *Name(int Id) const;

	/**
	 * What the interface asks of a map - see `map_document::Apply`.
	 *
	 * @param Id The number of the map.
	 * @param pJson The command, as a JSON object.
	 *
	 * @return The answer, as a JSON object.
	 */
	std::string Apply(int Id, const char *pJson);

	/**
	 * What the map is made of, as JSON - see `map_document::StructureJson`.
	 *
	 * @param Id The number of the map.
	 *
	 * @return The JSON text, or `null` where there is no such map.
	 */
	std::string StructureJson(int Id) const;

	/**
	 * The history of a map, as JSON - see `map_document::HistoryJson`.
	 *
	 * @param Id The number of the map.
	 *
	 * @return The JSON text, or `null` where there is no such map.
	 */
	std::string HistoryJson(int Id) const;

	/**
	 * The document of a map, for whoever changes it with something other than
	 * a command - a brush, which is numbers rather than text.
	 *
	 * Whoever does has to say so afterwards with `Touch`, or the frame that
	 * follows may be the one before.
	 *
	 * @param Id The number of the map.
	 *
	 * @return The document, or `nullptr`.
	 */
	map_document::CDocument *Document(int Id);
	const map_document::CDocument *Document(int Id) const;

	/**
	 * One picture of the map as the version holds it.
	 *
	 * Only for reading, and only until the map changes: a picture packed into
	 * the map file holds its pixels here, and whoever wants to show them
	 * should take a copy rather than the address.
	 *
	 * @param Id The number of the map.
	 * @param Index Which picture of the map.
	 *
	 * @return The picture, or `nullptr` where there is none.
	 */
	const map_document::CImage *Image(int Id, int Index) const;

	/**
	 * Where a map is being looked at.
	 *
	 * @param Id The number of the map.
	 *
	 * @return The camera, or `nullptr`.
	 */
	map_document::CView *View(int Id);
	const map_document::CView *View(int Id) const;

	/**
	 * What is shown of a map besides the map.
	 *
	 * @param Id The number of the map.
	 *
	 * @return The display settings, or `nullptr`.
	 */
	CDisplay *Display(int Id);

	/**
	 * How many brushes the editor keeps beside the one in hand. Ten, and the
	 * reason is the keyboard: that is how many digits there are.
	 */
	static constexpr size_t NUM_STORED_BRUSHES = 10;

	/**
	 * The tiles in hand - what a stamp puts down. A brush is a tile layer,
	 * both planes of it, so a piece of a switch layer carries its numbers and
	 * its delays with it.
	 *
	 * It belongs to the editor rather than to a map: somebody who copies a
	 * piece of one map into another is doing the ordinary thing.
	 */
	const map_document::CBrush &Brush() const { return m_Brush; }

	/**
	 * A brush taken out of the tileset rather than out of the map: the
	 * rectangle of tile indexes somebody dragged over the picture of the
	 * tiles. The kind comes from the layer it is meant for, because what a
	 * tile index means is a question about the layer.
	 *
	 * A tileset is sixteen by sixteen, which is where the numbers come from:
	 * the index of a tile is its place in that square.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group the layer is in.
	 * @param Layer Which layer of that group.
	 * @param x The left edge in the tileset, 0 to 15.
	 * @param y The top edge in the tileset, 0 to 15.
	 * @param Width How many tiles wide, clipped to the tileset.
	 * @param Height How many tiles tall, clipped to the tileset.
	 *
	 * @return `true` if there was such a tile layer.
	 */
	bool PickTiles(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height);

	/**
	 * Takes a rectangle of a layer into the brush, clipped to the layer.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group the layer is in.
	 * @param Layer Which layer of that group.
	 * @param x The left edge of the rectangle, in tiles.
	 * @param y The top edge of the rectangle, in tiles.
	 * @param Width How many tiles wide.
	 * @param Height How many tiles tall.
	 *
	 * @return `true` if there was such a tile layer.
	 */
	bool Grab(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height);

	/**
	 * One stamp of a stroke, with the brush's top left corner at `x`, `y`.
	 *
	 * A stroke is one transaction and many of these: the page opens it when
	 * the button goes down, calls this on every move and closes it when the
	 * button comes up, and the history gets one entry however many tiles were
	 * touched.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group the layer is in.
	 * @param Layer Which layer of that group.
	 * @param x Where the brush's left edge goes, in tiles.
	 * @param y Where the brush's top edge goes, in tiles.
	 *
	 * @return `true` if the brush may go in that layer at all.
	 */
	bool Paint(int Id, size_t Group, size_t Layer, int x, int y);

	/**
	 * The brush repeated over a rectangle - what the editor calls filling a
	 * selection.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group the layer is in.
	 * @param Layer Which layer of that group.
	 * @param x The left edge of the rectangle, in tiles.
	 * @param y The top edge of the rectangle, in tiles.
	 * @param Width How many tiles wide.
	 * @param Height How many tiles tall.
	 *
	 * @return `true` if the brush may go in that layer at all.
	 */
	bool Fill(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height);

	/**
	 * Air back in a rectangle, in whichever plane the layer draws.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group the layer is in.
	 * @param Layer Which layer of that group.
	 * @param x The left edge of the rectangle, in tiles.
	 * @param y The top edge of the rectangle, in tiles.
	 * @param Width How many tiles wide.
	 * @param Height How many tiles tall.
	 *
	 * @return `true` if there was such a tile layer.
	 */
	bool Erase(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height);

	/** Turns the brush over, or a quarter turn clockwise. */
	void FlipBrushX();
	void FlipBrushY();
	void RotateBrush();

	/**
	 * Puts the brush in hand into one of the slots, or takes one out again.
	 *
	 * @param Slot Which slot, below `NUM_STORED_BRUSHES`.
	 *
	 * @return Whether there was such a slot, and for `UseBrush` whether
	 * anything was in it.
	 */
	bool StoreBrush(size_t Slot);
	bool UseBrush(size_t Slot);

	/**
	 * How large a map is in world units, taken from its game layer, or the
	 * size of the surface where it has none.
	 *
	 * @param Id The number of the map.
	 *
	 * @return The size in world units.
	 */
	vec2 WorldSize(int Id) const;

	/** Whether the pictures of the map in front are all here yet. */
	bool Loading() const;

	/** Says that the next frame has to be drawn. */
	void Touch() { m_NeedsRedraw = true; }

	/**
	 * Whether anything has changed since the last frame was drawn. An editor
	 * that draws sixty frames a second at a map nobody is touching empties a
	 * telephone for nothing.
	 */
	bool NeedsRedraw() const;

	/** Takes in whatever of the pictures has arrived since the last frame. */
	void Update();

	/** Draws the map that is in front, if there is one. */
	void Render();

	/** Tells the editor that the surface it draws into has another size. */
	void OnResize(int Width, int Height);

	void Shutdown();

	IGraphics *Graphics() { return m_View.Graphics(); }
	IStorage *Storage() { return m_View.Storage(); }
	/** The surface and everything under it. */
	CStandaloneMapView &Surface() { return m_View; }

private:
	/**
	 * One open map: everything that belongs to it and to no other.
	 *
	 * The renderer is one of those things, which it need not be forever - two
	 * maps that use the same image could share the picture, and the one that
	 * is not in front draws nothing and could give up its geometry. Both are
	 * worth doing when there is a second tab to measure them on.
	 */
	class CMap
	{
	public:
		int m_Id = -1;
		std::string m_Name;
		map_document::CDocument m_Document;
		map_document::CView m_View;
		CDisplay m_Display;
		std::unique_ptr<CDocumentImages> m_pImages;
		std::unique_ptr<CDocumentRenderer> m_pRenderer;
		/** Where the camera stood when the last frame was drawn. */
		vec2 m_DrawnCenter = vec2(0.0f, 0.0f);
		float m_DrawnZoom = 0.0f;

		explicit CMap(map_document::CMapState Opened) :
			m_Document(std::move(Opened)) {}
	};

	CMap *Find(int Id);
	const CMap *Find(int Id) const;
	/** Puts a freshly read map in the list and in front. */
	int Add(map_document::CMapState Opened, const char *pName);

	/**
	 * The one layer of a change, checked: which layer it is about, and
	 * whether the brush may go in it.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group the layer is in.
	 * @param Layer Which layer of that group.
	 * @param NeedsBrush Whether the brush has to fit the layer as well.
	 *
	 * @return The map, or `nullptr` when any of that does not hold.
	 */
	CMap *ForTiles(int Id, size_t Group, size_t Layer, bool NeedsBrush);
	/**
	 * Puts one tile index in a brush, in whichever plane the kind keeps it.
	 *
	 * @param Brush The brush to write to.
	 * @param x Where in the brush, in tiles.
	 * @param y Where in the brush, in tiles.
	 * @param Index The tile index, 0 to 255.
	 */
	static void SetBrushTile(map_document::CBrush &Brush, int x, int y, int Index);

	const char *m_pLogContext;
	CStandaloneMapView m_View;
	std::vector<std::unique_ptr<CMap>> m_vpMaps;
	int m_Active = -1;
	int m_NextId = 1;
	bool m_NeedsRedraw = true;
	map_document::CBrush m_Brush;
	std::array<map_document::CBrush, NUM_STORED_BRUSHES> m_aStoredBrushes;
};

#endif // GAME_MAP_STANDALONE_MAP_EDITOR_H
