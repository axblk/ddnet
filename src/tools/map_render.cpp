#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/client/graphics_threaded.h>
#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/image.h>
#include <engine/kernel.h>
#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/shared/jobs.h>
#include <engine/storage.h>

#include <game/client/render.h>
#include <game/layers.h>
#include <game/localization.h>
#include <game/map/envelope_manager.h>
#include <game/map/map_renderer.h>
#include <game/map/render_interfaces.h>
#include <game/map/render_layer.h>
#include <game/map/render_map.h>
#include <game/mapitems.h>

#include <memory>
#include <string>

// Forward declaration for stub implementations
struct CDataSprite;
struct CDataContainer;

CDataContainer *g_pData = nullptr; // NOLINT(misc-use-internal-linkage)

static constexpr const char *TOOL_NAME = "map_render";

namespace MapRenderer
{
	// A tool that renders one map has nothing to gain from more; the number is
	// here rather than at the call site because the asset loader asks for it.
	static constexpr size_t JOB_THREADS = 2;

	class CMinimalEngine final : public IEngine
	{
	public:
		CJobPool m_JobPool;

		void Init() override {}
		size_t JobThreadCount() const override { return JOB_THREADS; }
		void AddJob(std::shared_ptr<IJob> pJob) override
		{
			m_JobPool.Add(std::move(pJob));
		}
		void ShutdownJobs() override
		{
			m_JobPool.Shutdown();
		}
		void SetAdditionalLogger(std::shared_ptr<ILogger> &&pLogger) override {}
	};

	class CMapRenderEnvelopeEval final : public IEnvelopeEval
	{
	public:
		CMapRenderEnvelopeEval(IMap *pMap, int TimeOffsetMillis) :
			m_pMap(pMap), m_TimeOffsetMillis(TimeOffsetMillis)
		{
			m_pEnvelopePoints = std::make_shared<CMapBasedEnvelopePointAccess>(pMap);
		}

		void EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const override
		{
			int EnvelopeStart, EnvelopeNum;
			m_pMap->GetType(MAPITEMTYPE_ENVELOPE, &EnvelopeStart, &EnvelopeNum);
			if(EnvelopeIndex < 0 || EnvelopeIndex >= EnvelopeNum)
				return;

			const CMapItemEnvelope *pItem = (CMapItemEnvelope *)m_pMap->GetItem(EnvelopeStart + EnvelopeIndex);
			if(pItem->m_Channels <= 0)
				return;
			Channels = std::min({Channels, (size_t)pItem->m_Channels, (size_t)CEnvPoint::MAX_CHANNELS});

			m_pEnvelopePoints->SetPointsRange(pItem->m_StartPoint, pItem->m_NumPoints);
			if(m_pEnvelopePoints->NumPoints() == 0)
				return;
			CRenderMap::RenderEvalEnvelope(m_pEnvelopePoints.get(), std::chrono::milliseconds(TimeOffsetMillis) + std::chrono::milliseconds(m_TimeOffsetMillis), Result, Channels);
		}

	private:
		IMap *m_pMap;
		std::shared_ptr<CMapBasedEnvelopePointAccess> m_pEnvelopePoints;
		int m_TimeOffsetMillis;
	};

	class CToolMapImages final : public IMapImages
	{
		IGraphics *m_pGraphics;
		IMap *m_pMap;
		IGraphics::CTextureHandle m_aTextures[MAX_MAPIMAGES];
		int m_Count;

	public:
		CToolMapImages(IGraphics *pGraphics, IMap *pMap) :
			m_pGraphics(pGraphics),
			m_pMap(pMap),
			m_Count(0)
		{
			std::fill(std::begin(m_aTextures), std::end(m_aTextures), IGraphics::CTextureHandle());
			LoadMapImages();
		}

		~CToolMapImages() override = default;

		// The client works out per image whether a tile layer, a quad layer or
		// both use it, and loads only what is sampled. The tool loads every
		// image both ways: it renders one picture and never has to care what
		// the extra copy costs.
		void LoadMapImages()
		{
			int Start;
			m_pMap->GetType(MAPITEMTYPE_IMAGE, &Start, &m_Count);
			m_Count = std::clamp<int>(m_Count, 0, MAX_MAPIMAGES);

			constexpr LOG_COLOR WarningLogColor = LOG_COLOR{255, 255, 0};

			for(int i = 0; i < m_Count; i++)
			{
				const CMapItemImage_v2 *pImg = static_cast<const CMapItemImage_v2 *>(m_pMap->GetItem(Start + i));

				const char *pName = m_pMap->GetDataString(pImg->m_ImageName);
				if(pName == nullptr || pName[0] == '\0')
				{
					if(pImg->m_External)
					{
						log_warn_color(WarningLogColor, TOOL_NAME, "Failed to load map image %d: failed to load name.", i);
						if(pImg->m_ImageName >= 0)
							m_pMap->UnloadData(pImg->m_ImageName);
						continue;
					}
					pName = "(error)";
				}

				if(pImg->m_Version > 1 && pImg->m_MustBe1 != 1)
				{
					log_warn_color(WarningLogColor, TOOL_NAME, "Failed to load map image %d '%s': invalid map image type.", i, pName);
					if(pImg->m_ImageName >= 0)
						m_pMap->UnloadData(pImg->m_ImageName);
					continue;
				}

				if(pImg->m_External)
				{
					char aPath[IO_MAX_PATH_LENGTH];
					str_format(aPath, sizeof(aPath), "mapres/%s.png", pName);
					m_aTextures[i] = m_pGraphics->LoadTexture(aPath, IStorage::TYPE_ALL, IGraphics::TEXLOAD_LAYERED);
				}
				else
				{
					CImageInfo ImageInfo;
					ImageInfo.m_Width = pImg->m_Width;
					ImageInfo.m_Height = pImg->m_Height;
					ImageInfo.m_Format = CImageInfo::FORMAT_RGBA;
					ImageInfo.m_pData = static_cast<uint8_t *>(m_pMap->GetData(pImg->m_ImageData));
					if(ImageInfo.m_pData && (size_t)m_pMap->GetDataSize(pImg->m_ImageData) >= ImageInfo.DataSize() && pImg->m_Height > 0 && pImg->m_Width > 0)
					{
						char aTexName[IO_MAX_PATH_LENGTH];
						str_format(aTexName, sizeof(aTexName), "embedded: %s", pName);
						m_aTextures[i] = m_pGraphics->LoadTextureRaw(ImageInfo, IGraphics::TEXLOAD_LAYERED, aTexName);
						m_pMap->UnloadData(pImg->m_ImageData);
					}
					else
					{
						log_warn_color(WarningLogColor, TOOL_NAME, "Failed to load map image %d '%s': failed to load data.", i, pName);
						m_pMap->UnloadData(pImg->m_ImageData);
						if(pImg->m_ImageName >= 0)
							m_pMap->UnloadData(pImg->m_ImageName);
						continue;
					}
				}
				if(pImg->m_ImageName >= 0)
					m_pMap->UnloadData(pImg->m_ImageName);
			}
		}

		IGraphics::CTextureHandle Get(int Index) const override
		{
			if(Index >= 0 && Index < m_Count)
				return m_aTextures[Index];
			return IGraphics::CTextureHandle();
		}

		int Num() const override { return m_Count; }

		// The tool draws the design, never the entity overlays. An invalid
		// handle is what the renderer already expects from an image that is
		// not there, and it skips the draw.
		IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType) override
		{
			(void)EntityLayerType;
			return IGraphics::CTextureHandle();
		}

		IGraphics::CTextureHandle GetSpeedupArrow() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetTuneColors() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetOverlayBottom() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetOverlayTop() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetOverlayCenter() override { return IGraphics::CTextureHandle(); }
	};
}

static void PrintUsage(const char *pProgramName)
{
	log_error(TOOL_NAME, "Usage: %s [-o <output>] [-w <width>] [-h <height>] [-z <zoom>] [-x <x-position>] [-y <y-position>] [-t <time>] <input.map>", pProgramName);
	log_error(TOOL_NAME, "  -o <output>      Output PNG file (default: output.png)");
	log_error(TOOL_NAME, "  -w <width>       Output image width (default: 1920)");
	log_error(TOOL_NAME, "  -h <height>      Output image height (default: 1080)");
	log_error(TOOL_NAME, "  -z <zoom>        Map zoom (default: auto, expects: 1.0f)");
	log_error(TOOL_NAME, "  -x <x-position>  X-Position (default: auto, expects ingame X coordinate i.e. 20.32)");
	log_error(TOOL_NAME, "  -y <y-position>  Y-Position (default: auto, expects ingame Y coordinate i.e. 20.32)");
	log_error(TOOL_NAME, "  -t <time>        Time offset for envelopes in ms (default: 0 ms, minimum: 0)");
}

int main(int argc, const char **argv)
{
	using namespace MapRenderer;
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	std::string OutputFile = "output.png";
	int OutputWidth = 1920;
	int OutputHeight = 1080;
	std::string InputMap;
	bool AutoPosition = true;
	vec2 Position(0, 0);
	bool AutoZoom = true;
	float Zoom = 1.0f;
	int TimeOffsetMillis = 0;

	bool InvalidUsage = false;

	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
		{
			OutputFile = argv[++i];
		}
		else if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
		{
			OutputWidth = std::max(1, atoi(argv[++i]));
		}
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
		{
			OutputHeight = std::max(1, atoi(argv[++i]));
		}
		else if(str_comp(argv[i], "-z") == 0 && i + 1 < argc)
		{
			AutoZoom = false;
			Zoom = std::max(0.001f, (float)atof(argv[++i]));
		}
		else if(str_comp(argv[i], "-x") == 0 && i + 1 < argc)
		{
			AutoPosition = false;
			Position.x = std::max(0.0f, (float)atof(argv[++i]));
		}
		else if(str_comp(argv[i], "-y") == 0 && i + 1 < argc)
		{
			AutoPosition = false;
			Position.y = std::max(0.0f, (float)atof(argv[++i]));
		}
		else if(str_comp(argv[i], "-t") == 0 && i + 1 < argc)
		{
			TimeOffsetMillis = std::max(0, atoi(argv[++i]));
		}
		else if(argv[i][0] != '-')
		{
			if(!InputMap.empty())
			{
				InvalidUsage = true;
				break;
			}
			InputMap = argv[i];
		}
		else
		{
			InvalidUsage = true;
			break;
		}
	}

	if(InputMap.empty() || InvalidUsage)
	{
		PrintUsage(argv[0]);
		return 1;
	}

	std::unique_ptr<IStorage> pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::BASIC, argc, argv));

	constexpr LOG_COLOR ErrorLogColor = LOG_COLOR{255, 0, 0};
	constexpr LOG_COLOR WarningLogColor = LOG_COLOR{255, 255, 0};
	constexpr LOG_COLOR SuccessLogColor = LOG_COLOR{0, 255, 128};

	if(!OutputFile.ends_with(".png") && !OutputFile.ends_with(".PNG"))
	{
		log_warn_color(WarningLogColor, TOOL_NAME, "Output name '%s' does not end with '.png' suffix", OutputFile.c_str());
	}

	if(!pStorage)
	{
		log_error_color(ErrorLogColor, TOOL_NAME, "Error creating storage");
		return 1;
	}

	// The tool draws into a virtual screen of its own, so the size of the
	// picture is the size of the screen and there is no window to describe.
	g_Config.m_GfxScreenWidth = OutputWidth;
	g_Config.m_GfxScreenHeight = OutputHeight;
	g_Config.m_GfxVsync = 0;
	g_Config.m_GfxFsaaSamples = 0;
	g_Config.m_GfxNoclip = 1;

	std::unique_ptr<IKernel> pKernel(IKernel::Create());
	if(!pKernel)
	{
		log_error_color(ErrorLogColor, TOOL_NAME, "Error creating kernel");
		return 1;
	}

	CMinimalEngine *pEngine = new CMinimalEngine();
	pEngine->m_JobPool.Init(JOB_THREADS);
	pKernel->RegisterInterface(pEngine);

	pKernel->RegisterInterface(pStorage.get(), false);

	IEngineGraphicsWindow *pWindow = CreateOffscreenGraphicsWindow();
	pKernel->RegisterInterface(pWindow);
	pKernel->RegisterInterface(static_cast<IGraphicsWindow *>(pWindow), false);
	IEngineGraphics *pGraphics = CreateEngineGraphicsThreaded();
	pKernel->RegisterInterface(pGraphics);
	pKernel->RegisterInterface(static_cast<IGraphics *>(pGraphics), false);
	IGraphicsBackend *pBackend = pWindow->Open(false);
	if(pBackend == nullptr || pGraphics->Init(pBackend, pWindow->Surface()) != 0)
	{
		log_error_color(ErrorLogColor, TOOL_NAME, "Failed to initialize graphics");
		return 1;
	}
	IGraphics &Graphics = *pGraphics;

	std::unique_ptr<IMap> pMap(CreateMap());
	if(!pMap)
	{
		log_error_color(ErrorLogColor, TOOL_NAME, "Error creating map");
		return 1;
	}

	// Load map from absolute path
	if(!pMap->Load(pStorage.get(), InputMap.c_str(), IStorage::TYPE_ABSOLUTE))
	{
		log_error_color(ErrorLogColor, TOOL_NAME, "Failed to load map '%s'", InputMap.c_str());
		return 1;
	}

	CLayers Layers;
	Layers.Init(pMap.get(), false, true);

	CToolMapImages MapImages(&Graphics, pMap.get());

	CRenderTools RenderTools;
	RenderTools.Init(&Graphics, nullptr);

	CRenderMap RenderMap;
	RenderMap.Init(&Graphics, nullptr, &RenderTools);

	CMapRenderer MapRenderer;
	MapRenderer.OnInit(&Graphics, nullptr, &RenderMap);

	CMapRenderEnvelopeEval EnvelopeEval(pMap.get(), TimeOffsetMillis);
	MapRenderer.Load(RENDERTYPE_FULL_DESIGN, &Layers, &MapImages, &EnvelopeEval, std::nullopt);

	// Calculate center and zoom to fit the map
	float MapWorldWidth = 0.0f, MapWorldHeight = 0.0f;
	if(Layers.GameLayer())
	{
		MapWorldWidth = Layers.GameLayer()->m_Width * 32.0f;
		MapWorldHeight = Layers.GameLayer()->m_Height * 32.0f;
	}
	else
	{
		MapWorldWidth = Graphics.ScreenWidth();
		MapWorldHeight = Graphics.ScreenHeight();
	}

	float Vw, Vh;
	Graphics.CalcScreenParams(Graphics.ScreenAspect(), 1.0f, &Vw, &Vh);
	if(AutoZoom)
		Zoom = std::max(MapWorldWidth / Vw, MapWorldHeight / Vh);

	CRenderLayerParams RenderParams;
	RenderParams.m_RenderType = RENDERTYPE_FULL_DESIGN;
	RenderParams.m_EntityOverlayVal = 0;
	RenderParams.m_Center = AutoPosition ? vec2(MapWorldWidth / 2.0f, MapWorldHeight / 2.0f) : Position * 32.0f;
	RenderParams.m_Zoom = Zoom;
	RenderParams.m_RenderText = false;
	RenderParams.m_RenderInvalidTiles = false;
	RenderParams.m_RenderTileBorder = true;
	RenderParams.m_DebugRenderGroupClips = false;
	RenderParams.m_DebugRenderQuadClips = false;
	RenderParams.m_DebugRenderClusterClips = false;
	RenderParams.m_DebugRenderTileClips = false;

	// Set up initial screen mapping
	Graphics.MapScreen(CScreenRect(0, 0, OutputWidth, OutputHeight));
	Graphics.Clear(0, 0, 0);

	MapRenderer.Render(RenderParams);

	// Finish the frame and read the virtual screen back.
	CImageInfo Image;
	std::unique_ptr<IGraphics::ITextureReadback> pReadback = Graphics.PresentAndReadbackAsync();
	if(pReadback != nullptr)
		(void)pReadback->Wait(Image);

	int ReturnCode = 1;
	if(Image.m_pData)
	{
		IOHANDLE File = io_open(OutputFile.c_str(), IOFLAG_WRITE);
		if(File)
		{
			if(CImageLoader::SavePng(File, OutputFile.c_str(), Image))
			{
				if((int)Image.m_Width != OutputWidth || (int)Image.m_Height != OutputHeight)
					log_warn_color(WarningLogColor, TOOL_NAME, "Image was scaled down to %dx%d", (int)Image.m_Width, (int)Image.m_Height);
				log_info_color(SuccessLogColor, TOOL_NAME, "Saved screenshot to '%s'", OutputFile.c_str());
				ReturnCode = 0;
			}
			else
			{
				log_error_color(ErrorLogColor, TOOL_NAME, "Failed to save screenshot to '%s'", OutputFile.c_str());
			}
		}
		else
		{
			log_error_color(ErrorLogColor, TOOL_NAME, "Failed to open '%s' for writing", OutputFile.c_str());
		}
		Image.Free();
	}
	else
	{
		log_error_color(ErrorLogColor, TOOL_NAME, "The backend returned no image data");
	}

	Graphics.Shutdown();
	pEngine->ShutdownJobs();
	pKernel->Shutdown();

	return ReturnCode;
}
