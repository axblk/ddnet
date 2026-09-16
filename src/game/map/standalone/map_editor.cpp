#include "map_editor.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/command.h>
#include <game/map/document/map_file.h>
#include <game/map/document/report.h>
#include <game/map/document/structure.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace
{
	constexpr LOG_COLOR ERROR_LOG_COLOR = LOG_COLOR{255, 0, 0};
	// What a map is called that nobody has given a name.
	constexpr const char *UNNAMED = "untitled";
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

int CMapEditor::OpenFromMemory(const void *pData, size_t Size, const char *pName)
{
	CDataFileReader File;
	if(!File.OpenFromMemory(pName, pData, (unsigned)Size, pName))
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to read the map that was handed over");
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
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to read the map that was handed over");
		return -1;
	}
	return Add(std::move(Read), pName);
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

	CDocumentRenderer::CParams Params;
	Params.m_Center = pMap->m_View.Center();
	Params.m_Zoom = pMap->m_View.Zoom();
	Params.m_ViewSize = pMap->m_View.ViewSize();
	Params.m_HighDetail = pMap->m_Display.m_HighDetail;
	Params.m_EntityOverlayVal = pMap->m_Display.m_EntityOverlayVal;
	Params.m_TimeOffsetMillis = pMap->m_Display.m_TimeOffsetMillis;

	IGraphics *pGraphics = m_View.Graphics();
	pGraphics->MapScreen(CScreenRect(0.0f, 0.0f, m_View.Width(), m_View.Height()));
	pGraphics->Clear(0.0f, 0.0f, 0.0f);
	pMap->m_pRenderer->Render(Params);

	pMap->m_DrawnCenter = pMap->m_View.Center();
	pMap->m_DrawnZoom = pMap->m_View.Zoom();
	m_NeedsRedraw = false;
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
