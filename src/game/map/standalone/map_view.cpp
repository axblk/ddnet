#include "map_view.h"

#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>

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
#include <cmath>
#include <utility>
#include <vector>

namespace
{
	constexpr LOG_COLOR ERROR_LOG_COLOR = LOG_COLOR{255, 0, 0};
	// How much of the picture is held in memory at once while it is written.
	constexpr size_t MAX_FULL_IMAGE_BAND_BYTES = 512 * 1024 * 1024;
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
	m_Windowed = Windowed;

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
	RenderParams.m_IgnoreParallax = Params.m_IgnoreParallax;

	// Set up initial screen mapping
	m_pGraphics->MapScreen(CScreenRect(0, 0, m_Width, m_Height));
	m_pGraphics->Clear(0, 0, 0);

	m_MapRenderer.Render(RenderParams);
}

CStandaloneMapView::SRenderParams CStandaloneMapView::ParamsForWorldRect(vec2 TopLeft, vec2 Size) const
{
	SRenderParams Params;
	Params.m_Center = TopLeft + Size / 2.0f;
	float ViewWidth, ViewHeight;
	m_pGraphics->CalcScreenParams(m_pGraphics->ScreenAspect(), 1.0f, &ViewWidth, &ViewHeight);
	Params.m_Zoom = ViewWidth <= 0.0f ? 1.0f : Size.x / ViewWidth;
	return Params;
}

bool CStandaloneMapView::ReadFrame(CImageInfo &Image)
{
	// Finish the frame and read the virtual screen back. What is handed over is
	// the picture of the frame before, so that its memory is used again instead
	// of another being allocated for every frame; what is left behind is an
	// empty one, which is what the readback fills.
	std::unique_ptr<IGraphics::ITextureReadback> pReadback = m_pGraphics->PresentAndReadbackAsync(std::exchange(Image, CImageInfo()));
	if(pReadback != nullptr)
		(void)pReadback->Wait(Image);

	if(Image.m_pData == nullptr)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "The backend returned no image data");
		return false;
	}
	return true;
}

bool CStandaloneMapView::EnsureAsideTarget()
{
	const int Width = std::max(m_Width, 1);
	const int Height = std::max(m_Height, 1);
	if(m_AsideTarget.IsValid() && m_AsideWidth == Width && m_AsideHeight == Height)
		return true;
	if(m_AsideTarget.IsValid())
		m_pGraphics->UnloadTexture(&m_AsideTarget);

	IGraphics::CTextureDesc Desc;
	Desc.m_Width = Width;
	Desc.m_Height = Height;
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Desc.m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED | IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE;
	m_AsideTarget = m_pGraphics->CreateTexture(Desc);
	if(!m_AsideTarget.IsValid())
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Could not create a %dx%d target to draw the picture into", Width, Height);
		return false;
	}
	m_AsideWidth = Width;
	m_AsideHeight = Height;
	return true;
}

bool CStandaloneMapView::RenderAsideAndRead(const SRenderParams &Params, CImageInfo &Image)
{
	if(!m_Windowed)
	{
		// There is no window to keep this out of, and the frontend already
		// draws into a target of its own here.
		Render(Params);
		return ReadFrame(Image);
	}
	if(!EnsureAsideTarget())
		return false;
	if(!m_pGraphics->BeginOffscreenFrame(m_AsideTarget))
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Could not draw into the picture's own target");
		return false;
	}
	Render(Params);
	std::unique_ptr<IGraphics::ITextureReadback> pReadback = m_pGraphics->EndOffscreenFrame(std::exchange(Image, CImageInfo()));
	if(pReadback == nullptr || !pReadback->Wait(Image) || Image.m_pData == nullptr)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "The backend returned no image data");
		return false;
	}
	return true;
}

bool CStandaloneMapView::SaveFullImage(const char *pPath, int TimeOffsetMillis)
{
	if(m_pMap == nullptr)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "No map is loaded");
		return false;
	}

	const vec2 WorldSize = MapWorldSize();
	const size_t FullWidth = static_cast<size_t>(std::max(1.0f, std::round(WorldSize.x)));
	const size_t FullHeight = static_cast<size_t>(std::max(1.0f, std::round(WorldSize.y)));
	const size_t TileWidth = static_cast<size_t>(std::max(m_Width, 1));
	const size_t TileHeight = static_cast<size_t>(std::max(m_Height, 1));

	// One band of the picture is as tall as the surface and as wide as the
	// whole map, and it is the only thing here that is held in memory at once.
	// Whoever wants a smaller one asks for a smaller surface.
	const size_t BandBytes = FullWidth * TileHeight * 4;
	if(BandBytes > MAX_FULL_IMAGE_BAND_BYTES)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext,
			"A %" PRIzu " by %" PRIzu " picture needs %" PRIzu " MiB per band at this surface height; ask for a surface no taller than %" PRIzu " pixels",
			FullWidth, FullHeight, BandBytes / (1024 * 1024), MAX_FULL_IMAGE_BAND_BYTES / (FullWidth * 4));
		return false;
	}

	IOHANDLE File = io_open(pPath, IOFLAG_WRITE);
	CPngRowWriter Writer;
	if(!Writer.Begin(File, pPath, FullWidth, FullHeight, CImageInfo::FORMAT_RGBA))
		return false;

	std::vector<uint8_t> vBand(BandBytes);
	CImageInfo Image;
	for(size_t Top = 0; Top < FullHeight; Top += TileHeight)
	{
		const size_t Rows = std::min(TileHeight, FullHeight - Top);
		for(size_t Left = 0; Left < FullWidth; Left += TileWidth)
		{
			const size_t Columns = std::min(TileWidth, FullWidth - Left);
			// The piece that is drawn always has the shape of the surface,
			// even where the map ends inside it; what sticks out is drawn and
			// then left behind.
			SRenderParams Params = ParamsForWorldRect(vec2(Left, Top), vec2(TileWidth, TileHeight));
			Params.m_TimeOffsetMillis = TimeOffsetMillis;
			Params.m_IgnoreParallax = true;
			// Beside the window, not in it: a picture of the whole map is the
			// surface moved over all of it, and drawn into the window that is
			// a sweep across the map that whoever asked for a picture never
			// asked to watch.
			if(!RenderAsideAndRead(Params, Image))
			{
				Image.Free();
				return false;
			}
			if(Image.m_Format != CImageInfo::FORMAT_RGBA || Image.m_Width < Columns || Image.m_Height < Rows)
			{
				log_error_color(ERROR_LOG_COLOR, m_pLogContext, "The backend returned a %" PRIzu " by %" PRIzu " frame where %" PRIzu " by %" PRIzu " was drawn",
					Image.m_Width, Image.m_Height, Columns, Rows);
				Image.Free();
				return false;
			}
			for(size_t Row = 0; Row < Rows; ++Row)
			{
				mem_copy(&vBand[(Row * FullWidth + Left) * 4], Image.m_pData + Row * Image.m_Width * 4, Columns * 4);
			}
		}
		if(!Writer.WriteRows(vBand.data(), Rows))
		{
			Image.Free();
			return false;
		}
	}
	Image.Free();
	return Writer.End();
}

bool CStandaloneMapView::SaveImage(const char *pPath)
{
	CImageInfo Image;
	if(!ReadFrame(Image))
		return false;

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
		if(m_AsideTarget.IsValid())
			m_pGraphics->UnloadTexture(&m_AsideTarget);
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
