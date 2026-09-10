#include "map_view.h"

#include <base/io.h>
#include <base/log.h>
#include <base/math.h>

#include <engine/client/graphics_threaded.h>
#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/kernel.h>
#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/map/render_layer.h>

#include <algorithm>

namespace
{
	constexpr LOG_COLOR ERROR_LOG_COLOR = LOG_COLOR{255, 0, 0};
	constexpr LOG_COLOR WARNING_LOG_COLOR = LOG_COLOR{255, 255, 0};
} // namespace

CStandaloneMapView::CStandaloneMapView(const char *pLogContext) :
	m_pLogContext(pLogContext)
{
}

CStandaloneMapView::~CStandaloneMapView()
{
	Shutdown();
}

IGraphics *CStandaloneMapView::Graphics()
{
	return m_pGraphics;
}

bool CStandaloneMapView::Init(int NumArgs, const char **ppArguments)
{
	m_pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::BASIC, NumArgs, ppArguments));
	if(!m_pStorage)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Error creating storage");
		return false;
	}

	m_pKernel = std::unique_ptr<IKernel>(IKernel::Create());
	if(!m_pKernel)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Error creating kernel");
		return false;
	}

	m_pEngine = new MapViewSupport::CMinimalEngine();
	m_pEngine->m_JobPool.Init(MapViewSupport::JOB_THREADS);
	m_pKernel->RegisterInterface(m_pEngine);
	m_pKernel->RegisterInterface(m_pStorage.get(), false);
	return true;
}

bool CStandaloneMapView::OpenWindow(int Width, int Height, IEngineGraphicsWindow *pWindow, bool Windowed)
{
	m_Width = Width;
	m_Height = Height;

	// The view draws into a surface of its own size, so the size of the
	// picture is the size of the screen and there is nothing else to describe.
	g_Config.m_GfxScreenWidth = Width;
	g_Config.m_GfxScreenHeight = Height;
	g_Config.m_GfxFsaaSamples = 0;
	g_Config.m_GfxNoclip = 1;
	if(Windowed)
	{
		// Somebody is watching this one, so it is paced by their display and
		// it stays out of the way of everything else on it.
		g_Config.m_GfxFullscreen = 0;
		g_Config.m_GfxVsync = 1;
	}
	else
	{
		g_Config.m_GfxVsync = 0;
	}

	m_pWindow = pWindow;
	m_pKernel->RegisterInterface(m_pWindow);
	m_pKernel->RegisterInterface(static_cast<IGraphicsWindow *>(m_pWindow), false);
	m_pGraphics = CreateEngineGraphicsThreaded();
	m_pKernel->RegisterInterface(m_pGraphics);
	m_pKernel->RegisterInterface(static_cast<IGraphics *>(m_pGraphics), false);
	IGraphicsBackend *pBackend = m_pWindow->Open(false);
	if(pBackend == nullptr || m_pGraphics->Init(pBackend, m_pWindow->Surface()) != 0)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to initialize graphics");
		return false;
	}

	// What was asked for is not always what there is: a frame without a window
	// is a texture, and a picture bigger than the graphics card's largest one
	// is drawn smaller. The view goes by what it got, so that what it draws is
	// not stretched on top of being smaller.
	m_Width = m_pGraphics->ScreenWidth();
	m_Height = m_pGraphics->ScreenHeight();

	m_RenderMap.Init(m_pGraphics, nullptr);
	m_MapRenderer.OnInit(m_pGraphics, nullptr, &m_RenderMap);
	return true;
}

bool CStandaloneMapView::LoadMap(const char *pPath, int StorageType)
{
	UnloadMap();

	std::unique_ptr<IMap> pMap(CreateMap());
	if(!pMap)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Error creating map");
		return false;
	}
	if(!pMap->Load(m_pStorage.get(), pPath, StorageType))
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to load map '%s'", pPath);
		return false;
	}

	m_pMap = std::move(pMap);
	m_Layers.Init(m_pMap.get(), false, true);
	m_pMapImages = std::make_unique<MapViewSupport::CToolMapImages>(m_pGraphics, m_pMap.get(), m_pLogContext);
	m_pEnvelopeEval = std::make_unique<MapViewSupport::CMapRenderEnvelopeEval>(m_pMap.get(), 0);
	m_MapRenderer.Load(RENDERTYPE_FULL_DESIGN, &m_Layers, m_pMapImages.get(), m_pEnvelopeEval.get(), std::nullopt);
	return true;
}

void CStandaloneMapView::UnloadMap()
{
	if(m_pMap == nullptr)
		return;
	m_MapRenderer.Clear();
	m_pEnvelopeEval = nullptr;
	// The images go back to the graphics card before the map they came out of
	// goes away.
	m_pMapImages = nullptr;
	m_Layers.Unload();
	m_pMap = nullptr;
}

void CStandaloneMapView::OnResize(int Width, int Height)
{
	m_Width = Width;
	m_Height = Height;
}

vec2 CStandaloneMapView::MapWorldSize()
{
	if(m_Layers.GameLayer() != nullptr)
		return vec2(m_Layers.GameLayer()->m_Width * 32.0f, m_Layers.GameLayer()->m_Height * 32.0f);
	return vec2(m_pGraphics->ScreenWidth(), m_pGraphics->ScreenHeight());
}

float CStandaloneMapView::FitZoom()
{
	const vec2 WorldSize = MapWorldSize();
	float Vw, Vh;
	m_pGraphics->CalcScreenParams(m_pGraphics->ScreenAspect(), 1.0f, &Vw, &Vh);
	return std::max(WorldSize.x / Vw, WorldSize.y / Vh);
}

void CStandaloneMapView::Render(const SRenderParams &Params)
{
	if(m_pMap == nullptr)
	{
		// Nothing to draw is still a frame, and an empty one is better than
		// whatever was left on the surface.
		m_pGraphics->MapScreen(CScreenRect(0, 0, m_Width, m_Height));
		m_pGraphics->Clear(0, 0, 0);
		return;
	}

	m_pEnvelopeEval->SetTimeOffset(Params.m_TimeOffsetMillis);

	CRenderLayerParams RenderParams;
	RenderParams.m_RenderType = RENDERTYPE_FULL_DESIGN;
	RenderParams.m_EntityOverlayVal = 0;
	RenderParams.m_Center = Params.m_Center;
	RenderParams.m_Zoom = Params.m_Zoom;
	RenderParams.m_RenderText = false;
	RenderParams.m_RenderInvalidTiles = false;
	RenderParams.m_RenderTileBorder = true;
	RenderParams.m_DebugRenderGroupClips = false;
	RenderParams.m_DebugRenderQuadClips = false;
	RenderParams.m_DebugRenderClusterClips = false;
	RenderParams.m_DebugRenderTileClips = false;

	// Set up initial screen mapping
	m_pGraphics->MapScreen(CScreenRect(0, 0, m_Width, m_Height));
	m_pGraphics->Clear(0, 0, 0);

	m_MapRenderer.Render(RenderParams);
}

bool CStandaloneMapView::SaveImage(const char *pPath)
{
	// Finish the frame and read the virtual screen back.
	CImageInfo Image;
	std::unique_ptr<IGraphics::ITextureReadback> pReadback = m_pGraphics->PresentAndReadbackAsync();
	if(pReadback != nullptr)
		(void)pReadback->Wait(Image);

	if(Image.m_pData == nullptr)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "The backend returned no image data");
		return false;
	}

	bool Success = false;
	IOHANDLE File = io_open(pPath, IOFLAG_WRITE);
	if(File)
	{
		if(CImageLoader::SavePng(File, pPath, Image))
		{
			Success = true;
		}
		else
		{
			log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to save screenshot to '%s'", pPath);
		}
	}
	else
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Failed to open '%s' for writing", pPath);
	}
	Image.Free();
	return Success;
}

void CStandaloneMapView::Shutdown()
{
	// The map's images are given back while there is still a graphics card to
	// give them back to.
	UnloadMap();
	if(m_pGraphics != nullptr)
	{
		m_pGraphics->Shutdown();
		m_pGraphics = nullptr;
	}
	if(m_pEngine != nullptr)
	{
		m_pEngine->ShutdownJobs();
		m_pEngine = nullptr;
	}
	m_pWindow = nullptr;
	if(m_pKernel != nullptr)
	{
		m_pKernel->Shutdown();
		m_pKernel = nullptr;
	}
	m_pStorage = nullptr;
}
