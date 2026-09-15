#include "map_view.h"

#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>

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
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace
{
	constexpr LOG_COLOR ERROR_LOG_COLOR = LOG_COLOR{255, 0, 0};
	// How much of the picture is held in memory at once while it is written.
	constexpr size_t MAX_FULL_IMAGE_BAND_BYTES = 512 * 1024 * 1024;
	// How large a piece of the picture the view draws for itself, and how much
	// memory the band of them is given. Both only apply where the view has a
	// window: a tool draws pieces of the size it was asked for.
	constexpr size_t MAX_FULL_IMAGE_PIECE = 4096;
	constexpr size_t FULL_IMAGE_BAND_BUDGET = 64 * 1024 * 1024;
	// How long a side of the picture may get where a budget of pixels applies.
	// A very long map would otherwise spend its whole budget on one side and
	// come out as a picture few programs will open.
	constexpr double MAX_FULL_IMAGE_SIDE = 16384.0;
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
	m_AssetLoader.Init(m_pEngine, MapViewSupport::JOB_THREADS);
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
	// A map fills the window it was given, whatever shape that window has.
	g_Config.m_GfxWholeWindow = 1;
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
	m_pMapImages = std::make_unique<MapViewSupport::CToolMapImages>(m_pGraphics, m_pStorage.get(), &m_AssetLoader, m_pMap.get(), &m_Layers, m_pLogContext);
	m_pEnvelopeEval = std::make_unique<MapViewSupport::CMapRenderEnvelopeEval>(m_pMap.get(), 0);
	m_MapRenderer.Load(RENDERTYPE_FULL_DESIGN, &m_Layers, m_pMapImages.get(), m_pEnvelopeEval.get(), std::nullopt, m_pEngine);
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

vec2 CStandaloneMapView::ViewSizeForAspect(float Aspect)
{
	float Width, Height;
	CalcViewSize(16.0f / 9.0f, 1.0f, 0.0f, &Width, &Height);
	return vec2(Height * Aspect, Height);
}

vec2 CStandaloneMapView::ViewSize() const
{
	return ViewSizeForAspect(m_pGraphics->ScreenAspect());
}

float CStandaloneMapView::FitZoom()
{
	const vec2 WorldSize = MapWorldSize();
	const vec2 View = ViewSize();
	return std::max(WorldSize.x / View.x, WorldSize.y / View.y);
}

float CStandaloneMapView::FillZoom()
{
	const vec2 WorldSize = MapWorldSize();
	const vec2 View = ViewSize();
	return std::min(WorldSize.x / View.x, WorldSize.y / View.y);
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

	// Before anything is drawn, because this is where a picture out of
	// `data/` may still have to be fetched.
	if(Params.m_EntityOverlayVal > 0)
		m_pMapImages->EnsureEntities();

	CRenderLayerParams RenderParams;
	RenderParams.m_RenderType = RENDERTYPE_FULL_DESIGN;
	RenderParams.m_EntityOverlayVal = Params.m_EntityOverlayVal;
	RenderParams.m_Center = Params.m_Center;
	RenderParams.m_Zoom = Params.m_Zoom;
	RenderParams.m_RenderText = false;
	RenderParams.m_HighDetail = Params.m_HighDetail;
	RenderParams.m_RenderInvalidTiles = false;
	RenderParams.m_RenderTileBorder = true;
	RenderParams.m_DebugRenderGroupClips = false;
	RenderParams.m_DebugRenderQuadClips = false;
	RenderParams.m_DebugRenderClusterClips = false;
	RenderParams.m_DebugRenderTileClips = false;
	RenderParams.m_Window = Params.m_Window;
	RenderParams.m_ViewSize = Params.m_ViewSize.x > 0.0f && Params.m_ViewSize.y > 0.0f ? Params.m_ViewSize : ViewSize();

	// Set up initial screen mapping
	m_pGraphics->MapScreen(CScreenRect(0, 0, m_Width, m_Height));
	m_pGraphics->Clear(0, 0, 0);

	m_MapRenderer.Render(RenderParams);
}

CStandaloneMapView::SRenderParams CStandaloneMapView::ParamsForWorldRect(vec2 TopLeft, vec2 Size) const
{
	SRenderParams Params;
	Params.m_Center = TopLeft + Size / 2.0f;
	const vec2 View = ViewSize();
	Params.m_Zoom = View.x <= 0.0f ? 1.0f : Size.x / View.x;
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

bool CStandaloneMapView::EnsureAsideTarget(int Width, int Height)
{
	Width = std::max(Width, 1);
	Height = std::max(Height, 1);
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

bool CStandaloneMapView::RenderAsideAndRead(const SRenderParams &Params, CImageInfo &Image, int Width, int Height)
{
	if(Width <= 0 || Height <= 0)
	{
		Width = m_Width;
		Height = m_Height;
	}
	if(!m_Windowed)
	{
		// There is no window to keep this out of, and the frontend already
		// draws into a target of its own here.
		Render(Params);
		return ReadFrame(Image);
	}
	if(!EnsureAsideTarget(Width, Height))
		return false;
	if(!m_pGraphics->BeginOffscreenFrame(m_AsideTarget))
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "Could not draw into the picture's own target");
		return false;
	}
	// The frame is drawn for a surface of the target's size, not the window's,
	// because that is what it is going into.
	const int WindowWidth = std::exchange(m_Width, Width);
	const int WindowHeight = std::exchange(m_Height, Height);
	Render(Params);
	m_Width = WindowWidth;
	m_Height = WindowHeight;
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
	if(!BeginFullImage(pPath, TimeOffsetMillis))
		return false;
	// Everything at once, for a program that has nothing else to do with the
	// thread while it waits. A viewer does, and steps instead.
	while(StepFullImage(std::chrono::nanoseconds::max()))
	{
		// Drawing.
	}
	return !m_FullImage.m_Failed;
}

bool CStandaloneMapView::BeginFullImage(const char *pPath, int TimeOffsetMillis, size_t PixelBudget)
{
	CancelFullImage();
	m_FullImage.m_Failed = true;
	if(m_pMap == nullptr)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "No map is loaded");
		return false;
	}

	const vec2 WorldSize = MapWorldSize();
	SFullImage &Full = m_FullImage;
	// A map is drawn at 32 pixels per tile, which for a large one is more
	// pixels than a picture can have: whoever gave a budget gets the whole map
	// within it, in the shape the map has, and nobody gets it larger than it
	// is. What that costs is detail, and what it saves is every piece that
	// would have been drawn for pixels nothing can open.
	const double Pixels = static_cast<double>(WorldSize.x) * static_cast<double>(WorldSize.y);
	double Scale = 1.0;
	if(PixelBudget > 0)
	{
		Scale = std::min({1.0, std::sqrt(static_cast<double>(PixelBudget) / Pixels),
			MAX_FULL_IMAGE_SIDE / static_cast<double>(WorldSize.x),
			MAX_FULL_IMAGE_SIDE / static_cast<double>(WorldSize.y)});
	}
	Full.m_FullWidth = static_cast<size_t>(std::max(1.0, std::round(WorldSize.x * Scale)));
	Full.m_FullHeight = static_cast<size_t>(std::max(1.0, std::round(WorldSize.y * Scale)));
	Full.m_PieceWidth = static_cast<size_t>(std::max(m_Width, 1));
	Full.m_PieceHeight = static_cast<size_t>(std::max(m_Height, 1));
	if(m_Windowed)
	{
		// A piece goes into a target of its own, so it need not be the size of
		// the window: every piece costs a frame and a read back off the
		// graphics card whatever size it is, and abyss at the size of a window
		// is fourteen hundred of them. What bounds a piece is the largest
		// texture the backend will make, and the band it goes into - the band
		// is as wide as the whole picture, so on a wide map it is the height of
		// a piece that costs memory.
		const size_t Limit = std::clamp<size_t>(m_pGraphics->MaxTextureDimension(), 1, MAX_FULL_IMAGE_PIECE);
		Full.m_PieceWidth = std::min(Full.m_FullWidth, Limit);
		Full.m_PieceHeight = std::clamp<size_t>(FULL_IMAGE_BAND_BUDGET / (Full.m_FullWidth * 4), 1, std::min(Full.m_FullHeight, Limit));
	}

	// One band of the picture is as tall as the surface and as wide as the
	// whole map, and it is the only thing here that is held in memory at once.
	// Whoever wants a smaller one asks for a smaller surface.
	const size_t BandBytes = Full.m_FullWidth * Full.m_PieceHeight * 4;
	if(BandBytes > MAX_FULL_IMAGE_BAND_BYTES)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext,
			"A %" PRIzu " by %" PRIzu " picture needs %" PRIzu " MiB per band at this surface height; ask for a surface no taller than %" PRIzu " pixels",
			Full.m_FullWidth, Full.m_FullHeight, BandBytes / (1024 * 1024), MAX_FULL_IMAGE_BAND_BYTES / (Full.m_FullWidth * 4));
		return false;
	}

	IOHANDLE File = io_open(pPath, IOFLAG_WRITE);
	if(!Full.m_Writer.Begin(File, pPath, Full.m_FullWidth, Full.m_FullHeight, CImageInfo::FORMAT_RGBA))
		return false;

	// One view for the whole picture, of the picture's own shape, and every
	// piece a window into it: a layer that moves at its own speed is then laid
	// out across the whole map once, the way it is when the map is looked at,
	// rather than starting again in every piece.
	Full.m_Whole = SRenderParams();
	Full.m_Whole.m_ViewSize = ViewSizeForAspect(Full.m_FullWidth / (float)Full.m_FullHeight);
	Full.m_Whole.m_Zoom = WorldSize.x / Full.m_Whole.m_ViewSize.x;
	Full.m_Whole.m_Center = WorldSize / 2.0f;
	Full.m_Whole.m_TimeOffsetMillis = TimeOffsetMillis;

	if(Scale < 1.0)
	{
		log_info(m_pLogContext, "The whole map is %" PRIzu " by %" PRIzu " pixels, so the picture is drawn at %" PRIzu " by %" PRIzu,
			static_cast<size_t>(std::round(WorldSize.x)), static_cast<size_t>(std::round(WorldSize.y)), Full.m_FullWidth, Full.m_FullHeight);
	}

	Full.m_vBand.assign(BandBytes, 0);
	Full.m_Top = 0;
	Full.m_Left = 0;
	Full.m_Failed = false;
	Full.m_Running = true;
	return true;
}

bool CStandaloneMapView::StepOneFullImagePiece()
{
	SFullImage &Full = m_FullImage;
	const size_t Rows = std::min(Full.m_PieceHeight, Full.m_FullHeight - Full.m_Top);
	const size_t Columns = std::min(Full.m_PieceWidth, Full.m_FullWidth - Full.m_Left);
	// The piece that is drawn always has the shape of the surface, even where
	// the map ends inside it; what sticks out is drawn and then left behind.
	SRenderParams Params = Full.m_Whole;
	Params.m_Window = CScreenRect(
		vec2(Full.m_Left / (float)Full.m_FullWidth, Full.m_Top / (float)Full.m_FullHeight),
		vec2((Full.m_Left + Full.m_PieceWidth) / (float)Full.m_FullWidth, (Full.m_Top + Full.m_PieceHeight) / (float)Full.m_FullHeight));
	// Beside the window, not in it: a picture of the whole map is the surface
	// moved over all of it, and drawn into the window that is a sweep across
	// the map that whoever asked for a picture never asked to watch.
	if(!RenderAsideAndRead(Params, Full.m_Image, (int)Full.m_PieceWidth, (int)Full.m_PieceHeight))
		return false;
	if(Full.m_Image.m_Format != CImageInfo::FORMAT_RGBA || Full.m_Image.m_Width < Columns || Full.m_Image.m_Height < Rows)
	{
		log_error_color(ERROR_LOG_COLOR, m_pLogContext, "The backend returned a %" PRIzu " by %" PRIzu " frame where %" PRIzu " by %" PRIzu " was drawn",
			Full.m_Image.m_Width, Full.m_Image.m_Height, Columns, Rows);
		return false;
	}
	for(size_t Row = 0; Row < Rows; ++Row)
	{
		mem_copy(&Full.m_vBand[(Row * Full.m_FullWidth + Full.m_Left) * 4], Full.m_Image.m_pData + Row * Full.m_Image.m_Width * 4, Columns * 4);
	}

	Full.m_Left += Full.m_PieceWidth;
	if(Full.m_Left < Full.m_FullWidth)
		return true;
	// The band is full, so it goes out and the sweep drops to the next one.
	if(!Full.m_Writer.WriteRows(Full.m_vBand.data(), Rows))
		return false;
	Full.m_Left = 0;
	Full.m_Top += Full.m_PieceHeight;
	return true;
}

bool CStandaloneMapView::StepFullImage(std::chrono::nanoseconds Budget)
{
	SFullImage &Full = m_FullImage;
	if(!Full.m_Running)
		return false;
	const std::chrono::nanoseconds Until = time_get_nanoseconds() + Budget;
	// At least one piece, however little time there is: a budget that buys
	// nothing would leave the picture standing still for good.
	do
	{
		if(Full.m_Top >= Full.m_FullHeight)
			return EndFullImage(true);
		if(!StepOneFullImagePiece())
			return EndFullImage(false);
	} while(Budget == std::chrono::nanoseconds::max() || time_get_nanoseconds() < Until);
	return true;
}

float CStandaloneMapView::FullImageProgress() const
{
	const SFullImage &Full = m_FullImage;
	if(!Full.m_Running || Full.m_FullHeight == 0 || Full.m_FullWidth == 0)
		return 0.0f;
	const float Bands = std::ceil(Full.m_FullHeight / (float)Full.m_PieceHeight);
	const float InBand = std::ceil(Full.m_FullWidth / (float)Full.m_PieceWidth);
	const float Done = Full.m_Top / (float)Full.m_PieceHeight * InBand + Full.m_Left / (float)Full.m_PieceWidth;
	return std::clamp(Done / (Bands * InBand), 0.0f, 1.0f);
}

bool CStandaloneMapView::EndFullImage(bool Success)
{
	SFullImage &Full = m_FullImage;
	Full.m_Image.Free();
	Full.m_vBand.clear();
	Full.m_vBand.shrink_to_fit();
	Full.m_Running = false;
	Full.m_Failed = !Success || !Full.m_Writer.End();
	return false;
}

void CStandaloneMapView::CancelFullImage()
{
	if(m_FullImage.m_Running)
	{
		// The file is left where it is, half written: the writer closes it
		// without finishing it, and half a PNG is a file a viewer refuses
		// rather than one that quietly shows half a map.
		EndFullImage(false);
	}
}

bool CStandaloneMapView::SaveImage(const SRenderParams &Params, const char *pPath)
{
	CImageInfo Image;
	if(!RenderAsideAndRead(Params, Image))
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
		// Before the pool, because it is the pool that runs what it holds.
		m_AssetLoader.Shutdown();
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
