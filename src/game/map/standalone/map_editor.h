#ifndef GAME_MAP_STANDALONE_MAP_EDITOR_H
#define GAME_MAP_STANDALONE_MAP_EDITOR_H

#include <base/vmath.h>

#include <game/map/document/document.h>
#include <game/map/document/view.h>
#include <game/map/document_images.h>
#include <game/map/document_render.h>
#include <game/map/standalone/map_view.h>

#include <cstddef>
#include <memory>
#include <string>
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

	const char *m_pLogContext;
	CStandaloneMapView m_View;
	std::vector<std::unique_ptr<CMap>> m_vpMaps;
	int m_Active = -1;
	int m_NextId = 1;
	bool m_NeedsRedraw = true;
};

#endif // GAME_MAP_STANDALONE_MAP_EDITOR_H
