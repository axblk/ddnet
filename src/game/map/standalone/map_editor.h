#ifndef GAME_MAP_STANDALONE_MAP_EDITOR_H
#define GAME_MAP_STANDALONE_MAP_EDITOR_H

#include <base/vmath.h>

#include <game/map/document/automap.h>
#include <game/map/document/document.h>
#include <game/map/document/edit.h>
#include <game/map/document/view.h>
#include <game/map/document_images.h>
#include <game/map/document_render.h>
#include <game/map/standalone/map_view.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
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
		/** The quad whose corners are shown, for somebody dragging them. */
		CDocumentRenderer::CParams::CShownQuad m_ShownQuad;

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
	 * Puts a second map into one that is already open - see
	 * `map_document::AppendMap`.
	 *
	 * @param Id The number of the map to put it into.
	 * @param pPath The file to take from.
	 * @param StorageType Where to look for it.
	 *
	 * @return What came over, as JSON, or `null` if the file could not be
	 * read.
	 */
	std::string Append(int Id, const char *pPath, int StorageType);

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
	 * Calls the map something else.
	 *
	 * The name is what the file is called when the map is written out, so
	 * this is what "save as" is made of: rename, then save.
	 *
	 * @param Id The number of the map.
	 * @param pName The new name; an empty one means the unnamed one.
	 *
	 * @return `true` if there was such a map.
	 */
	bool Rename(int Id, const char *pName);

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
	 * One sound of a map, to read its bytes out of.
	 *
	 * The same rule as a picture: the address is good until the map changes,
	 * and whoever wants to play the sound should take a copy.
	 *
	 * @param Id The number of the map.
	 * @param Index Which sound of the map.
	 *
	 * @return The sound, or `nullptr` where there is none.
	 */
	const map_document::CSound *Sound(int Id, int Index) const;

	/**
	 * Reads a sound into the map as bytes, and says which one it became.
	 *
	 * The bytes are an Opus file, and they cross as bytes rather than as a
	 * command for the same reason a picture's pixels do: a sound is hundreds
	 * of kilobytes, and hundreds of kilobytes of JSON are a text nobody
	 * should write or read. What is in them is the map's business as little
	 * as it is this program's - nothing here plays anything.
	 *
	 * @param Id The number of the map.
	 * @param pName What the sound is called.
	 * @param Size How many bytes.
	 * @param pData The bytes.
	 *
	 * @return Which sound of the map it became, or -1.
	 */
	int AddSound(int Id, const char *pName, int Size, const uint8_t *pData);

	/** Puts other bytes into a sound that is already in the map. */
	bool SetSoundData(int Id, int Index, int Size, const uint8_t *pData);

	/**
	 * The quads of one layer, as JSON - see `map_document::QuadsJson`.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Layer Which layer of it.
	 *
	 * @return The JSON text, or `null` for a layer that holds no quads.
	 */
	std::string QuadsJson(int Id, int Group, int Layer) const;

	/** The sound sources of one layer - see `map_document::SoundSourcesJson`. */
	std::string SoundSourcesJson(int Id, int Group, int Layer) const;

	/**
	 * Proof mode around where the view is looking - see
	 * `map_document::ProofJson`.
	 *
	 * The place is the view's own centre rather than something the page
	 * passes in, because proof mode asks "what would a player standing where
	 * I am looking see", and where that is, is the view's answer.
	 */
	std::string ProofJson(int Id, bool Menu) const;

	/** Everything a map may say to a server - see `map_document::SettingsHelpJson`. */
	std::string SettingsHelpJson() const;

	/** What is wrong with each settings line - see `map_document::SettingProblemsJson`. */
	std::string SettingProblemsJson(int Id) const;

	/**
	 * The names of settings that begin with what has been typed.
	 *
	 * Handed over as a JSON array rather than one at a time, because it is a
	 * list somebody is looking at all at once.
	 */
	std::string SettingNamesJson(const char *pPrefix) const;

	/**
	 * What lies in a directory of the storage, as a JSON array.
	 *
	 * For the browser, where the maps that were saved live in the program's
	 * own file system and a page cannot look into it. Each entry says its
	 * name without the `.map` and how big it is; directories and anything
	 * that is not a map are left out.
	 */
	std::string SavedJson(const char *pDirectory, int StorageType);

	/** What is wrong with one settings line - see `map_document::CheckSetting`. */
	std::string CheckSetting(const char *pLine) const;

	/**
	 * Where a pixel of the surface is, in the coordinates one group is drawn
	 * in - which is the plain view for a group without parallax and somewhere
	 * else entirely for one with it.
	 *
	 * This is what a pointer over a quad needs: a quad's points are in its
	 * group's coordinates, and a click is on the surface.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Pixel Where on the surface, in pixels from its top left.
	 *
	 * @return The place, in world units.
	 */
	vec2 WorldInGroup(int Id, size_t Group, vec2 Pixel) const;

	/**
	 * The other way round: where a place in one group's coordinates is on
	 * the surface.
	 *
	 * This is what an overlay drawn in the page needs. A sound source is a
	 * circle in its group's coordinates, and an SVG circle over the canvas is
	 * in pixels; asking the program rather than working it out in the page
	 * is what keeps the shape over the place it belongs to when the view
	 * moves.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param World The place, in world units.
	 *
	 * @return Where on the surface, in pixels from its top left.
	 */
	vec2 PixelInGroup(int Id, size_t Group, vec2 World) const;

	/**
	 * What tile stands in one place of a layer.
	 *
	 * One tile at a time rather than the whole plane, for the same reason the
	 * quads and the envelope points are asked for one at a time: a map of
	 * four million tiles is not a thing to hand out after every stroke.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Layer Which layer of it, which has to hold tiles.
	 * @param x Where, in tiles.
	 * @param y Where, in tiles.
	 *
	 * @return The index, or -1 where there is no such tile.
	 */
	int TileIndex(int Id, int Group, int Layer, int x, int y) const;

	/**
	 * What a tile of a physics layer does, in a sentence - see
	 * `map_document::ExplainTile`.
	 *
	 * The index rather than a place, because the two things that want it are
	 * the map under the pointer and the tileset a brush is picked from, and
	 * only one of those has places.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Layer Which layer of it, whose kind decides what a number means.
	 * @param Index The tile, 0 to 255.
	 *
	 * @return The sentence, or `nullptr` where there is nothing to say.
	 */
	const char *Explain(int Id, int Group, int Layer, int Index) const;

	/**
	 * Keeps a `.rules` file under a name, parsed, for automapping with.
	 *
	 * The file is not read here: natively it comes off the disk and in the
	 * browser the page fetches it, and either way what arrives is text. The
	 * name is the one the map calls the picture, because that is how a layer
	 * finds its rules.
	 *
	 * @param pName What the rules are called - the picture's name.
	 * @param pText The whole file.
	 *
	 * @return How many configurations it holds.
	 */
	size_t LoadRules(const char *pName, const char *pText);

	/**
	 * Which lines of a rules file were passed over, as a JSON array.
	 *
	 * A rules file is read as far as it is understood and what is left over
	 * is skipped, which is what lets a file from a newer editor still
	 * automap - but somebody writing one wants to be told, and a line number
	 * is the only useful way to say it. Counting starts at one.
	 */
	std::string RuleProblems(const char *pName) const;

	/** How many configurations a rules file that was loaded holds. */
	size_t NumRuleConfigs(const char *pName) const;

	/** What one of them is called, or an empty word for one that is not there. */
	const char *RuleConfigName(const char *pName, size_t Config) const;

	/**
	 * Runs one configuration of a rules file over a layer, or over a piece of
	 * one, as a change of its own.
	 *
	 * The game layer of the same map is handed to it, so that a run which is
	 * filtered by a physics tile has something to filter by.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Layer Which layer of it, which has to hold tiles.
	 * @param pRules Which rules file, by the name it was loaded under.
	 * @param Config Which configuration of it.
	 * @param Seed The seed, or 0 for one that is made up.
	 * @param Reference Which physics tile the first run is filtered by, -1
	 * for none.
	 * @param x Where the rectangle starts.
	 * @param y Where the rectangle starts.
	 * @param Width How wide, or -1 for the whole layer.
	 * @param Height How tall, or -1 for the whole layer.
	 *
	 * @return Whether it ran.
	 */
	bool Automap(int Id, int Group, int Layer, const char *pRules, int Config, int Seed, int Reference,
		int x, int y, int Width, int Height);

	/**
	 * Puts a picture into the map with its pixels, and says where it went.
	 *
	 * Not a command like the rest, because the pixels are bytes: a picture
	 * of a thousand by a thousand is four megabytes, and four megabytes of
	 * JSON is a text nobody should have to write or read. The page decodes
	 * the PNG - browsers do that - and hands over what came out.
	 *
	 * @param Id The number of the map.
	 * @param pName What to call it.
	 * @param Width How wide the pixels are.
	 * @param Height How tall.
	 * @param pPixels RGBA, `Width * Height * 4` bytes.
	 *
	 * @return Which picture of the map it became, or -1 where it was refused.
	 */
	int AddImage(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels);

	/**
	 * Turns a picture into tile layers of its own colours - see
	 * `map_document::AddTileArt`.
	 *
	 * @return Which group of the map it became, or -1.
	 */
	int AddTileArt(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels);

	/** How many colours a picture holds, so a page can warn before it asks. */
	int CountArtColors(int Width, int Height, const uint8_t *pPixels) const;

	/**
	 * Turns a picture into one quad per pixel - see
	 * `map_document::AddQuadArt`.
	 *
	 * @return Which group of the map it became, or -1.
	 */
	int AddQuadArt(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels,
		int PixelStep, int QuadSize, bool Centralize, bool Merge);

	/**
	 * Puts other pixels into a picture the map already has, keeping every
	 * layer that is drawn with it.
	 *
	 * @param Id The number of the map.
	 * @param Index Which picture of the map.
	 * @param Width How wide the pixels are.
	 * @param Height How tall.
	 * @param pPixels RGBA, `Width * Height * 4` bytes.
	 *
	 * @return Whether it was done.
	 */
	bool SetImagePixels(int Id, int Index, int Width, int Height, const uint8_t *pPixels);

	/**
	 * The lowest number no tile of a physics layer is using yet - see
	 * `map_document::NextFreeNumber`.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Layer Which layer of it.
	 * @param Checkpoint For a tele layer, whether to count the checkpoints
	 * rather than the rest.
	 *
	 * @return The number, or -1 where there is none to be had.
	 */
	int NextFreeNumber(int Id, int Group, int Layer, bool Checkpoint) const;

	/**
	 * Looks at where a number is used, one place per cluster.
	 *
	 * Which place is the caller's to count, because which one somebody is
	 * standing on is a question about the interface rather than about the
	 * map - the editor only moves the view there.
	 *
	 * @param Id The number of the map.
	 * @param Group Which group.
	 * @param Layer Which layer of it.
	 * @param Number The number to look for.
	 * @param Which Which of the places, counted from zero and wrapped round.
	 *
	 * @return How many places there are, and 0 when the view did not move.
	 */
	size_t GotoNumber(int Id, int Group, int Layer, int Number, size_t Which);

	/**
	 * The points of one envelope, as JSON - see `map_document::EnvelopeJson`.
	 *
	 * @param Id The number of the map.
	 * @param Index Which envelope of it.
	 *
	 * @return The JSON text, or `null` where there is no such envelope.
	 */
	std::string EnvelopeJson(int Id, int Index) const;

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
	 * What goes beside a physics tile the brush puts down - a tele's target,
	 * a switch's group and delay, how hard and which way a speedup pushes.
	 *
	 * They belong to the brush and not to a tile: a number is chosen and then
	 * tiles are put down with it. Grabbing a piece of a layer reads them back
	 * off what was grabbed, so that carrying a piece of a map somewhere else
	 * carries its numbers too.
	 */
	const map_document::CBrushNumbers &Numbers() const { return m_Numbers; }

	/**
	 * Whether the tiles in hand are tele checkpoints.
	 *
	 * Asked because the checkpoints of a tele layer keep a count of their own
	 * apart from the teleporters, so which free number to offer depends on
	 * which of the two is about to be put down. What a tile index means is
	 * the program's to know, not the page's.
	 *
	 * @return Whether any tile of the brush is one.
	 */
	bool BrushIsCheckpoint() const;

	/**
	 * Sets those numbers and writes them onto the brush in hand.
	 *
	 * @param Numbers What to put beside the tiles from now on.
	 */
	void SetNumbers(const map_document::CBrushNumbers &Numbers);

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
	 * Puts the brush down: nothing in hand.
	 *
	 * An empty brush is what grabs, so this is the way to a rectangle - and
	 * on a tablet it is a button, because there is no Escape to press.
	 */
	void ClearBrush();

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

	/**
	 * Begins a picture of a whole map, written as a PNG a band at a time.
	 *
	 * What is in it is the map as it is looked at - the detail layers if
	 * those are shown, the layers that are hidden left out - and none of the
	 * working aids: no grid, no marks, no handles on a quad. It is drawn over
	 * the frames that follow by `StepPicture`, because the whole of a large
	 * map is hundreds of pieces and a browser has one thread.
	 *
	 * @param Id The number of the map.
	 * @param pPath The file to write, as a path of the operating system.
	 * @param PixelBudget How many pixels the picture may have at most.
	 *
	 * @return `true` when there is a picture to step through.
	 */
	bool BeginPicture(int Id, const char *pPath, size_t PixelBudget);

	/**
	 * Whether a tile that does nothing in a physics layer may be put there.
	 * Off, as in the native editor: such a tile is put down as air.
	 */
	void SetAllowUnused(bool Allow) { m_AllowUnused = Allow; }

	/**
	 * Which entities sheet physics layers are drawn out of, for every map
	 * that is open and every one opened later: one of the names in
	 * `data/editor/entities_clear/`, such as `ddnet`, `race`, `fng` or
	 * `vanilla`. A name that is not one of them is refused.
	 *
	 * @return Whether it was one of them.
	 */
	bool SetEntitiesImage(const char *pName);
	const char *EntitiesImage() const { return m_EntitiesImage.c_str(); }
	bool AllowUnused() const { return m_AllowUnused; }
	/** How many tiles the last paint or fill put down as air for that reason. */
	int LastDropped() const { return m_LastDropped; }

	/** Draws pieces of it for that long; `false` once it is done or failed. */
	bool StepPicture(std::chrono::nanoseconds Budget) { return m_View.StepFullImage(Budget); }
	bool PictureRunning() const { return m_View.FullImageRunning(); }
	bool PictureFailed() const { return m_View.FullImageFailed(); }
	float PictureProgress() const { return m_View.FullImageProgress(); }

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

	/** What the renderer is told about a map, in one place because two callers ask. */
	CDocumentRenderer::CParams ParamsFor(const CMap &Map) const;

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
	/** The brush as it is to be put down, with unused tiles taken out unless allowed. */
	const map_document::CBrush &BrushToPlace();
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
	// What is actually put down when unused tiles are not allowed, and how
	// many tiles that took out the last time.
	map_document::CBrush m_PlacedBrush;
	bool m_AllowUnused = false;
	std::string m_EntitiesImage = "ddnet";
	int m_LastDropped = 0;
	map_document::CBrushNumbers m_Numbers;
	std::array<map_document::CBrush, NUM_STORED_BRUSHES> m_aStoredBrushes;
	// The `.rules` files that were handed in, by the name they came under.
	// They belong to the editor rather than to a map: the same rules
	// automap every map that draws with that picture.
	std::map<std::string, map_document::CAutomapRules> m_Rules;
	std::map<std::string, std::vector<int>> m_RuleProblems;
};

#endif // GAME_MAP_STANDALONE_MAP_EDITOR_H
