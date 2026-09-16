#include "map_editor.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <game/map/document/automap.h>
#include <game/map/document/command.h>
#include <game/map/document/edit.h>
#include <game/map/document/explain.h>
#include <game/map/document/map_file.h>
#include <game/map/document/report.h>
#include <game/map/document/structure.h>
#include <game/mapitems.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
	constexpr LOG_COLOR ERROR_LOG_COLOR = LOG_COLOR{255, 0, 0};
	// What a map is called that nobody has given a name.
	constexpr const char *UNNAMED = "untitled";
	// A tileset is sixteen by sixteen and a tile index is a place in it, so
	// what is asked for outside it is not a tile.
	constexpr int TILESET_SIDE = 16;
} // namespace

CMapEditor::CMapEditor(const char *pLogContext) :
	m_pLogContext(pLogContext), m_View(pLogContext)
{
}

CMapEditor::~CMapEditor()
{
	Shutdown();
}

bool CMapEditor::Init(int NumArgs, const char **ppArguments)
{
	return m_View.Init(NumArgs, ppArguments);
}

bool CMapEditor::OpenWindow(int Width, int Height, IEngineGraphicsWindow *pWindow, bool Windowed)
{
	if(!m_View.OpenWindow(Width, Height, pWindow, Windowed))
		return false;
	OnResize(m_View.Width(), m_View.Height());
	return true;
}

int CMapEditor::Add(map_document::CMapState Opened, const char *pName)
{
	auto pMap = std::make_unique<CMap>(std::move(Opened));
	pMap->m_Id = m_NextId++;
	pMap->m_Name = pName == nullptr || pName[0] == '\0' ? UNNAMED : pName;
	pMap->m_View.SetSurface(std::max(m_View.Width(), 1), std::max(m_View.Height(), 1));
	pMap->m_pImages = std::make_unique<CDocumentImages>(m_View.Graphics(), m_View.Storage(), m_View.AssetLoader(), nullptr, m_pLogContext);
	pMap->m_pRenderer = std::make_unique<CDocumentRenderer>();
	pMap->m_pRenderer->OnInit(m_View.Graphics(), pMap->m_pImages.get());

	const int Id = pMap->m_Id;
	m_vpMaps.push_back(std::move(pMap));
	m_Active = Id;
	// The whole map at once is what somebody who just opened one wants to
	// see; where to go from there is their business.
	View(Id)->Fit(WorldSize(Id));
	Touch();
	return Id;
}

int CMapEditor::Open(const char *pPath, int StorageType)
{
	CDataFileReader File;
	if(!File.Open(m_View.Storage(), pPath, StorageType))
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to open map '%s'", pPath);
		return -1;
	}
	map_document::CMapState Read;
	std::vector<std::string> vWarnings;
	const bool Ok = map_document::ReadMapState(File, &Read, &vWarnings);
	File.Close();
	for(const std::string &Warning : vWarnings)
		log_warn(m_pLogContext, "%s", Warning.c_str());
	if(!Ok)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to read map '%s'", pPath);
		return -1;
	}
	char aName[IO_MAX_PATH_LENGTH];
	fs_split_file_extension(fs_filename(pPath), aName, sizeof(aName));
	return Add(std::move(Read), aName);
}

int CMapEditor::Create(int Width, int Height, const char *pName)
{
	map_document::CMapState Map;
	map_document::CGroup Group;
	Group.m_Name = "Game";
	map_document::CTileLayer Game(map_document::ETileLayerKind::GAME, std::max(Width, 1), std::max(Height, 1));
	Game.m_Name = "Game";
	Group.m_vpLayers.push_back(std::make_shared<const map_document::CLayer>(std::move(Game)));
	Map.AddGroup(std::move(Group));
	return Add(std::move(Map), pName);
}

bool CMapEditor::Save(int Id, const char *pPath, int StorageType)
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr)
		return false;
	CDataFileWriter File;
	if(!File.Open(m_View.Storage(), pPath, StorageType))
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to open '%s' for writing", pPath);
		return false;
	}
	map_document::WriteMapState(File, pMap->m_Document.Map());
	File.Finish();
	return true;
}

bool CMapEditor::Close(int Id)
{
	const auto Found = std::find_if(m_vpMaps.begin(), m_vpMaps.end(), [Id](const auto &pMap) { return pMap->m_Id == Id; });
	if(Found == m_vpMaps.end())
		return false;
	// The geometry and the pictures go back to the graphics card in the order
	// they were taken from it, and both before the map they belong to.
	(*Found)->m_pRenderer->Clear();
	(*Found)->m_pRenderer = nullptr;
	(*Found)->m_pImages = nullptr;
	m_vpMaps.erase(Found);
	if(m_Active == Id)
		m_Active = m_vpMaps.empty() ? -1 : m_vpMaps.back()->m_Id;
	Touch();
	return true;
}

int CMapEditor::IdAt(size_t Index) const
{
	return Index < m_vpMaps.size() ? m_vpMaps[Index]->m_Id : -1;
}

bool CMapEditor::SetActive(int Id)
{
	if(Find(Id) == nullptr)
		return false;
	if(m_Active != Id)
		Touch();
	m_Active = Id;
	return true;
}

CMapEditor::CMap *CMapEditor::Find(int Id)
{
	const auto Found = std::find_if(m_vpMaps.begin(), m_vpMaps.end(), [Id](const auto &pMap) { return pMap->m_Id == Id; });
	return Found == m_vpMaps.end() ? nullptr : Found->get();
}

const CMapEditor::CMap *CMapEditor::Find(int Id) const
{
	return const_cast<CMapEditor *>(this)->Find(Id);
}

const char *CMapEditor::Name(int Id) const
{
	const CMap *pMap = Find(Id);
	return pMap == nullptr ? "" : pMap->m_Name.c_str();
}

std::string CMapEditor::Apply(int Id, const char *pJson)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr)
		return R"({"ok":false,"error":"There is no such map"})";
	std::string Answer = map_document::Apply(pMap->m_Document, pJson);
	Touch();
	return Answer;
}

std::string CMapEditor::StructureJson(int Id) const
{
	const CMap *pMap = Find(Id);
	return pMap == nullptr ? "null" : map_document::StructureJson(pMap->m_Document.Map());
}

std::string CMapEditor::HistoryJson(int Id) const
{
	const CMap *pMap = Find(Id);
	return pMap == nullptr ? "null" : map_document::HistoryJson(pMap->m_Document);
}

map_document::CDocument *CMapEditor::Document(int Id)
{
	CMap *pMap = Find(Id);
	return pMap == nullptr ? nullptr : &pMap->m_Document;
}

const map_document::CDocument *CMapEditor::Document(int Id) const
{
	const CMap *pMap = Find(Id);
	return pMap == nullptr ? nullptr : &pMap->m_Document;
}

const map_document::CImage *CMapEditor::Image(int Id, int Index) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Index < 0)
		return nullptr;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	return (size_t)Index >= Map.NumImages() ? nullptr : Map.Image((size_t)Index);
}

std::string CMapEditor::QuadsJson(int Id, int Group, int Layer) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0)
		return "null";
	return map_document::QuadsJson(pMap->m_Document.Map(), (size_t)Group, (size_t)Layer);
}

std::string CMapEditor::SoundSourcesJson(int Id, int Group, int Layer) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0)
		return "null";
	return map_document::SoundSourcesJson(pMap->m_Document.Map(), (size_t)Group, (size_t)Layer);
}

std::string CMapEditor::ProofJson(int Id, bool Menu) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr)
		return "null";
	return map_document::ProofJson(pMap->m_Document.Map(), pMap->m_View.Center(), Menu);
}

std::string CMapEditor::EnvelopeJson(int Id, int Index) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Index < 0)
		return "null";
	return map_document::EnvelopeJson(pMap->m_Document.Map(), (size_t)Index);
}

int CMapEditor::TileIndex(int Id, int Group, int Layer, int x, int y) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0 || x < 0 || y < 0)
		return -1;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	if((size_t)Group >= Map.NumGroups() || (size_t)Layer >= Map.NumLayers((size_t)Group))
		return -1;
	const auto *pTiles = std::get_if<map_document::CTileLayer>(Map.Layer((size_t)Group, (size_t)Layer));
	if(pTiles == nullptr)
		return -1;
	return map_document::TileMeaning(*pTiles, x, y);
}

const char *CMapEditor::Explain(int Id, int Group, int Layer, int Index) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0)
		return nullptr;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	if((size_t)Group >= Map.NumGroups() || (size_t)Layer >= Map.NumLayers((size_t)Group))
		return nullptr;
	const auto *pTiles = std::get_if<map_document::CTileLayer>(Map.Layer((size_t)Group, (size_t)Layer));
	if(pTiles == nullptr)
		return nullptr;
	return map_document::ExplainTile(pTiles->m_Kind, Index);
}

size_t CMapEditor::LoadRules(const char *pName, const char *pText)
{
	if(pName == nullptr || pName[0] == '\0')
		return 0;
	std::vector<int> vNotUnderstood;
	map_document::CAutomapRules Rules = map_document::ParseAutomapRules(pText, &vNotUnderstood);
	const size_t Configs = Rules.NumConfigs();
	m_Rules[pName] = std::move(Rules);
	m_RuleProblems[pName] = std::move(vNotUnderstood);
	return Configs;
}

std::string CMapEditor::RuleProblems(const char *pName) const
{
	CJsonStringWriter Writer;
	Writer.BeginArray();
	if(pName != nullptr)
	{
		const auto Found = m_RuleProblems.find(pName);
		if(Found != m_RuleProblems.end())
		{
			for(const int Line : Found->second)
				Writer.WriteIntValue(Line);
		}
	}
	Writer.EndArray();
	return Writer.GetOutputString();
}

size_t CMapEditor::NumRuleConfigs(const char *pName) const
{
	if(pName == nullptr)
		return 0;
	const auto Found = m_Rules.find(pName);
	return Found == m_Rules.end() ? 0 : Found->second.NumConfigs();
}

const char *CMapEditor::RuleConfigName(const char *pName, size_t Config) const
{
	if(pName == nullptr)
		return "";
	const auto Found = m_Rules.find(pName);
	return Found == m_Rules.end() ? "" : Found->second.ConfigName(Config);
}

bool CMapEditor::Automap(int Id, int Group, int Layer, const char *pRules, int Config, int Seed, int Reference,
	int x, int y, int Width, int Height)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0 || Config < 0 || pRules == nullptr)
		return false;
	const auto Found = m_Rules.find(pRules);
	if(Found == m_Rules.end() || (size_t)Config >= Found->second.NumConfigs())
		return false;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	if((size_t)Group >= Map.NumGroups() || (size_t)Layer >= Map.NumLayers((size_t)Group))
		return false;
	// Asked of the layer rather than of `TileLayer`, which is for a caller
	// that already knows: a quad layer is a thing somebody may well have
	// selected, not a mistake.
	const auto *pTiles = std::get_if<map_document::CTileLayer>(Map.Layer((size_t)Group, (size_t)Layer));
	if(pTiles == nullptr || !map_document::DrawsOwnTiles(pTiles->m_Kind))
		return false;

	// The game layer of the same map, for a run that is filtered by a physics
	// tile. A map without one automaps without the filter.
	const map_document::CTileLayer *pGame = nullptr;
	for(size_t OverGroup = 0; OverGroup < Map.NumGroups() && pGame == nullptr; ++OverGroup)
		for(size_t OverLayer = 0; OverLayer < Map.NumLayers(OverGroup); ++OverLayer)
		{
			const auto *pOne = std::get_if<map_document::CTileLayer>(Map.Layer(OverGroup, OverLayer));
			if(pOne != nullptr && pOne->m_Kind == map_document::ETileLayerKind::GAME)
			{
				pGame = pOne;
				break;
			}
		}

	pMap->m_Document.Begin("Automap");
	map_document::EditTileLayer(pMap->m_Document, (size_t)Group, (size_t)Layer, [&](map_document::CTileLayer &Changed) {
		map_document::Automap(Changed, pGame, Found->second, (size_t)Config, Seed, Reference, x, y, Width, Height);
	});
	pMap->m_Document.Commit();
	Touch();
	return true;
}

int CMapEditor::AddImage(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr || pName == nullptr || pName[0] == '\0' || Width <= 0 || Height <= 0 || pPixels == nullptr)
		return -1;
	map_document::CImage Image;
	Image.m_Name = pName;
	Image.m_External = false;
	Image.m_Width = Width;
	Image.m_Height = Height;
	Image.m_Data.Mutable().assign(pPixels, pPixels + (size_t)Width * (size_t)Height * 4);
	pMap->m_Document.Begin("Add image");
	const size_t Index = map_document::AddImage(pMap->m_Document, std::move(Image));
	pMap->m_Document.Commit();
	Touch();
	return (int)Index;
}

bool CMapEditor::SetImagePixels(int Id, int Index, int Width, int Height, const uint8_t *pPixels)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr || Index < 0 || Width <= 0 || Height <= 0 || pPixels == nullptr)
		return false;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	if((size_t)Index >= Map.NumImages())
		return false;
	map_document::CImage Changed = *Map.Image((size_t)Index);
	Changed.m_External = false;
	Changed.m_Width = Width;
	Changed.m_Height = Height;
	Changed.m_Data.Mutable().assign(pPixels, pPixels + (size_t)Width * (size_t)Height * 4);
	pMap->m_Document.Begin("Replace image");
	map_document::SetImage(pMap->m_Document, (size_t)Index, std::move(Changed));
	pMap->m_Document.Commit();
	Touch();
	return true;
}

const map_document::CSound *CMapEditor::Sound(int Id, int Index) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Index < 0 || (size_t)Index >= pMap->m_Document.Map().NumSounds())
		return nullptr;
	return pMap->m_Document.Map().Sound((size_t)Index);
}

int CMapEditor::AddSound(int Id, const char *pName, int Size, const uint8_t *pData)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr || pName == nullptr || pName[0] == '\0' || Size <= 0 || pData == nullptr)
		return -1;
	map_document::CSound Sound;
	Sound.m_Name = pName;
	Sound.m_External = false;
	Sound.m_Data.Mutable().assign(pData, pData + (size_t)Size);
	pMap->m_Document.Begin("Add sound");
	const size_t Index = map_document::AddSound(pMap->m_Document, std::move(Sound));
	pMap->m_Document.Commit();
	Touch();
	return (int)Index;
}

bool CMapEditor::SetSoundData(int Id, int Index, int Size, const uint8_t *pData)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr || Index < 0 || Size <= 0 || pData == nullptr)
		return false;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	if((size_t)Index >= Map.NumSounds())
		return false;
	map_document::CSound Changed = *Map.Sound((size_t)Index);
	Changed.m_External = false;
	Changed.m_Data.Mutable().assign(pData, pData + (size_t)Size);
	pMap->m_Document.Begin("Replace sound");
	map_document::SetSound(pMap->m_Document, (size_t)Index, std::move(Changed));
	pMap->m_Document.Commit();
	Touch();
	return true;
}

bool CMapEditor::BrushIsCheckpoint() const
{
	const auto *pTele = std::get_if<map_document::CTileStore<CTeleTile>>(&m_Brush.m_ExtraTiles);
	if(pTele == nullptr)
		return false;
	for(int y = 0; y < pTele->Height(); ++y)
		for(int x = 0; x < pTele->Width(); ++x)
			if(IsTeleTileCheckpoint(pTele->Get(x, y).m_Type))
				return true;
	return false;
}

int CMapEditor::NextFreeNumber(int Id, int Group, int Layer, bool Checkpoint) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0)
		return -1;
	const map_document::CTileLayer *pTiles = pMap->m_Document.Map().TileLayer((size_t)Group, (size_t)Layer);
	return pTiles == nullptr ? -1 : map_document::NextFreeNumber(*pTiles, Checkpoint);
}

size_t CMapEditor::GotoNumber(int Id, int Group, int Layer, int Number, size_t Which)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr || Group < 0 || Layer < 0)
		return 0;
	const map_document::CTileLayer *pTiles = pMap->m_Document.Map().TileLayer((size_t)Group, (size_t)Layer);
	if(pTiles == nullptr)
		return 0;
	const std::vector<ivec2> vPlaces = map_document::NumberPlaces(*pTiles, Number);
	if(vPlaces.empty())
		return 0;
	// The middle of the tile rather than its corner, so that what was looked
	// for is in the middle of the screen.
	const ivec2 Place = vPlaces[Which % vPlaces.size()];
	pMap->m_View.SetCenter(vec2(Place.x * 32.0f + 16.0f, Place.y * 32.0f + 16.0f));
	return vPlaces.size();
}

map_document::CView *CMapEditor::View(int Id)
{
	CMap *pMap = Find(Id);
	return pMap == nullptr ? nullptr : &pMap->m_View;
}

const map_document::CView *CMapEditor::View(int Id) const
{
	const CMap *pMap = Find(Id);
	return pMap == nullptr ? nullptr : &pMap->m_View;
}

CMapEditor::CDisplay *CMapEditor::Display(int Id)
{
	CMap *pMap = Find(Id);
	return pMap == nullptr ? nullptr : &pMap->m_Display;
}

vec2 CMapEditor::WorldSize(int Id) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr)
		return vec2(0.0f, 0.0f);
	const map_document::CMapState &Map = pMap->m_Document.Map();
	const std::optional<map_document::CLayerAddress> Game = map_document::FindGameLayer(Map);
	if(!Game.has_value())
		return vec2(m_View.Width(), m_View.Height());
	const map_document::CTileLayer *pGame = Map.TileLayer(Game->m_Group, Game->m_Layer);
	return vec2(pGame->Width() * 32.0f, pGame->Height() * 32.0f);
}

bool CMapEditor::Loading() const
{
	const CMap *pMap = Find(m_Active);
	return pMap != nullptr && pMap->m_pImages->Loading();
}

bool CMapEditor::NeedsRedraw() const
{
	if(m_NeedsRedraw)
		return true;
	const CMap *pMap = Find(m_Active);
	if(pMap == nullptr)
		return false;
	// A picture that arrives changes what is on the screen without anybody
	// asking; so does a running envelope.
	return pMap->m_Display.m_Animate || pMap->m_pImages->Loading() ||
	       pMap->m_View.Center() != pMap->m_DrawnCenter || pMap->m_View.Zoom() != pMap->m_DrawnZoom;
}

void CMapEditor::Update()
{
	CMap *pMap = Find(m_Active);
	if(pMap == nullptr)
		return;
	// Which pictures the map wants is asked before anything is drawn rather
	// than while it is: whoever is waiting for them has to be able to ask
	// whether they are here, and a version that nobody has looked at yet has
	// asked for nothing.
	pMap->m_pImages->Use(pMap->m_Document.Map());
	pMap->m_pImages->Update();
}

CDocumentRenderer::CParams CMapEditor::ParamsFor(const CMap &Map) const
{
	CDocumentRenderer::CParams Params;
	Params.m_Center = Map.m_View.Center();
	Params.m_Zoom = Map.m_View.Zoom();
	Params.m_ViewSize = Map.m_View.ViewSize();
	Params.m_HighDetail = Map.m_Display.m_HighDetail;
	Params.m_EntityOverlayVal = Map.m_Display.m_EntityOverlayVal;
	Params.m_TimeOffsetMillis = Map.m_Display.m_TimeOffsetMillis;
	Params.m_pHidden = &Map.m_Display.m_vHidden;
	Params.m_Grid = Map.m_Display.m_Grid;
	Params.m_Marked = Map.m_Display.m_Marked;
	Params.m_ShownQuad = Map.m_Display.m_ShownQuad;
	// The grid belongs to the group that is being worked in, and the editor
	// itself holds no selection - so it follows the group the game layer is
	// in, which is the one the tiles of a map are measured against.
	const std::optional<map_document::CLayerAddress> Game = map_document::FindGameLayer(Map.m_Document.Map());
	if(Game.has_value())
		Params.m_GridGroup = Game->m_Group;
	return Params;
}

vec2 CMapEditor::PixelInGroup(int Id, size_t Group, vec2 World) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group >= pMap->m_Document.Map().NumGroups())
		return vec2(0.0f, 0.0f);
	const CScreenRect Shown = pMap->m_pRenderer->GroupScreen(*pMap->m_Document.Map().m_vpGroups[Group], ParamsFor(*pMap));
	const float Width = std::max(1, m_View.Width());
	const float Height = std::max(1, m_View.Height());
	// The inverse of `WorldInGroup`, and deliberately the same sum read
	// backwards: a group with no width on the screen would divide by nothing,
	// and that is a group nobody can point at anyway.
	if(Shown.Width() == 0.0f || Shown.Height() == 0.0f)
		return vec2(0.0f, 0.0f);
	return vec2(
		(World.x - Shown.m_TopLeft.x) / Shown.Width() * Width,
		(World.y - Shown.m_TopLeft.y) / Shown.Height() * Height);
}

vec2 CMapEditor::WorldInGroup(int Id, size_t Group, vec2 Pixel) const
{
	const CMap *pMap = Find(Id);
	if(pMap == nullptr || Group >= pMap->m_Document.Map().NumGroups())
		return vec2(0.0f, 0.0f);
	// The same sum the renderer draws that group with, asked of the renderer
	// itself - so that where a pointer says it is and where a quad is drawn
	// cannot be two different answers.
	const CScreenRect Shown = pMap->m_pRenderer->GroupScreen(*pMap->m_Document.Map().m_vpGroups[Group], ParamsFor(*pMap));
	const float Width = std::max(1, m_View.Width());
	const float Height = std::max(1, m_View.Height());
	return vec2(
		Shown.m_TopLeft.x + Pixel.x / Width * Shown.Width(),
		Shown.m_TopLeft.y + Pixel.y / Height * Shown.Height());
}

void CMapEditor::Render()
{
	CMap *pMap = Find(m_Active);
	if(pMap == nullptr)
	{
		m_NeedsRedraw = false;
		return;
	}

	// The version that is drawn is the one the document is in right now,
	// which while something is being dragged is the half-made one. Handing it
	// over costs the pointers of the map: every layer that did not change
	// comes along as the same node, and the renderer keeps its geometry.
	const auto pShown = std::make_shared<const map_document::CMapState>(pMap->m_Document.Map());
	pMap->m_pRenderer->Use(pShown);

	const CDocumentRenderer::CParams Params = ParamsFor(*pMap);

	IGraphics *pGraphics = m_View.Graphics();
	pGraphics->MapScreen(CScreenRect(0.0f, 0.0f, m_View.Width(), m_View.Height()));
	pGraphics->Clear(0.0f, 0.0f, 0.0f);
	pMap->m_pRenderer->Render(Params);

	pMap->m_DrawnCenter = pMap->m_View.Center();
	pMap->m_DrawnZoom = pMap->m_View.Zoom();
	m_NeedsRedraw = false;
}

CMapEditor::CMap *CMapEditor::ForTiles(int Id, size_t Group, size_t Layer, bool NeedsBrush)
{
	CMap *pMap = Find(Id);
	if(pMap == nullptr)
		return nullptr;
	const map_document::CMapState &Map = pMap->m_Document.Map();
	if(Group >= Map.NumGroups() || Layer >= Map.NumLayers(Group))
		return nullptr;
	const map_document::CLayer *pLayer = Map.Layer(Group, Layer);
	if(!std::holds_alternative<map_document::CTileLayer>(*pLayer))
		return nullptr;
	if(NeedsBrush && !map_document::CanStamp(std::get<map_document::CTileLayer>(*pLayer).m_Kind, m_Brush.m_Kind))
		return nullptr;
	return pMap;
}

void CMapEditor::SetBrushTile(map_document::CBrush &Brush, int x, int y, int Index)
{
	const unsigned char Value = (unsigned char)std::clamp(Index, 0, 255);
	// A physics layer says what it means in its second plane, so a brush for
	// one carries the index there rather than in the plane that is drawn -
	// the same way the file holds it. What goes beside it - a tele number, a
	// speedup's force, a switch's delay - stays at zero: putting a number
	// there is a tool's business, not a tile's.
	if(map_document::DrawsOwnTiles(Brush.m_Kind))
	{
		CTile Tile;
		Tile.m_Index = Value;
		Brush.m_Tiles.Set(x, y, Tile);
		return;
	}
	std::visit([x, y, Value](auto &Extra) {
		if constexpr(!std::is_same_v<std::decay_t<decltype(Extra)>, std::monostate>)
		{
			auto PhysicsTile = Extra.Get(x, y);
			PhysicsTile.m_Type = Value;
			Extra.Set(x, y, PhysicsTile);
		}
	},
		Brush.m_ExtraTiles);
}

bool CMapEditor::PickTiles(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height)
{
	const CMap *pMap = ForTiles(Id, Group, Layer, false);
	if(pMap == nullptr)
		return false;
	x = std::clamp(x, 0, TILESET_SIDE - 1);
	y = std::clamp(y, 0, TILESET_SIDE - 1);
	Width = std::clamp(Width, 1, TILESET_SIDE - x);
	Height = std::clamp(Height, 1, TILESET_SIDE - y);

	map_document::CBrush Brush(pMap->m_Document.Map().TileLayer(Group, Layer)->m_Kind, Width, Height);
	for(int ty = 0; ty < Height; ++ty)
	{
		for(int tx = 0; tx < Width; ++tx)
			SetBrushTile(Brush, tx, ty, (y + ty) * TILESET_SIDE + x + tx);
	}
	// A tile out of the tileset says what it does; the numbers say to which
	// of them, and they are the ones last chosen rather than whatever the
	// tileset would suggest, which is nothing.
	map_document::SetBrushNumbers(Brush, m_Numbers);
	m_Brush = std::move(Brush);
	return true;
}

bool CMapEditor::Grab(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height)
{
	const CMap *pMap = ForTiles(Id, Group, Layer, false);
	if(pMap == nullptr)
		return false;
	m_Brush = map_document::GrabTiles(*pMap->m_Document.Map().TileLayer(Group, Layer), x, y, Width, Height);
	// A piece of a layer comes with the numbers that were on it, and now they
	// are the ones in hand - so putting the piece down somewhere else puts
	// down what was picked up.
	m_Numbers = map_document::BrushNumbers(m_Brush);
	return true;
}

void CMapEditor::SetNumbers(const map_document::CBrushNumbers &Numbers)
{
	m_Numbers = Numbers;
	map_document::SetBrushNumbers(m_Brush, m_Numbers);
}

bool CMapEditor::Paint(int Id, size_t Group, size_t Layer, int x, int y)
{
	CMap *pMap = ForTiles(Id, Group, Layer, true);
	if(pMap == nullptr || m_Brush.Width() == 0 || m_Brush.Height() == 0)
		return false;
	// Its own transaction, which joins the one the page opened when the
	// button went down - so a stroke is one entry and a single stamp is one
	// as well.
	pMap->m_Document.Begin("Draw");
	map_document::PaintTiles(pMap->m_Document, Group, Layer, x, y, m_Brush);
	pMap->m_Document.Commit();
	Touch();
	return true;
}

bool CMapEditor::Fill(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height)
{
	CMap *pMap = ForTiles(Id, Group, Layer, true);
	if(pMap == nullptr || m_Brush.Width() == 0 || m_Brush.Height() == 0)
		return false;
	pMap->m_Document.Begin("Fill");
	map_document::EditTileLayer(pMap->m_Document, Group, Layer, [&](map_document::CTileLayer &Changed) {
		map_document::FillTiles(Changed, x, y, Width, Height, m_Brush);
	});
	pMap->m_Document.Commit();
	Touch();
	return true;
}

bool CMapEditor::Erase(int Id, size_t Group, size_t Layer, int x, int y, int Width, int Height)
{
	CMap *pMap = ForTiles(Id, Group, Layer, false);
	if(pMap == nullptr)
		return false;
	pMap->m_Document.Begin("Erase");
	map_document::EditTileLayer(pMap->m_Document, Group, Layer, [&](map_document::CTileLayer &Changed) {
		map_document::EraseTiles(Changed, x, y, Width, Height);
	});
	pMap->m_Document.Commit();
	Touch();
	return true;
}

void CMapEditor::FlipBrushX()
{
	map_document::FlipBrushX(m_Brush);
}

void CMapEditor::FlipBrushY()
{
	map_document::FlipBrushY(m_Brush);
}

void CMapEditor::RotateBrush()
{
	map_document::RotateBrush(m_Brush);
}

bool CMapEditor::StoreBrush(size_t Slot)
{
	if(Slot >= m_aStoredBrushes.size())
		return false;
	m_aStoredBrushes[Slot] = m_Brush;
	return true;
}

bool CMapEditor::UseBrush(size_t Slot)
{
	if(Slot >= m_aStoredBrushes.size() || m_aStoredBrushes[Slot].Width() == 0)
		return false;
	m_Brush = m_aStoredBrushes[Slot];
	// A brush taken out of a slot brings its own numbers back with it, the
	// same way a grabbed one does.
	m_Numbers = map_document::BrushNumbers(m_Brush);
	return true;
}

void CMapEditor::OnResize(int Width, int Height)
{
	m_View.OnResize(Width, Height);
	for(const auto &pMap : m_vpMaps)
		pMap->m_View.SetSurface(std::max(m_View.Width(), 1), std::max(m_View.Height(), 1));
	Touch();
}

void CMapEditor::Shutdown()
{
	// Everything that holds something of the graphics card lets go of it
	// before the graphics card goes.
	while(!m_vpMaps.empty())
		Close(m_vpMaps.back()->m_Id);
	m_View.Shutdown();
}
