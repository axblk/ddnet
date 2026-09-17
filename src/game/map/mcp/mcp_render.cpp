#include "mcp_render.h"

#include <base/log.h>
#include <base/time.h>

#include <engine/gfx/image_loader.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/image.h>

#include <game/map/document/structure.h>
#include <game/map/document/view.h>
#include <game/map/document_images.h>
#include <game/map/document_render.h>
#include <game/map/standalone/map_view.h>

#include <memory>
#include <string>
#include <vector>

namespace map_mcp
{
	namespace
	{
		constexpr const char *LOG_CONTEXT = "mcp/render";

		class CHeadlessRenderer : public IRenderer
		{
		public:
			CHeadlessRenderer(int NumArgs, const char **ppArguments) :
				m_View(LOG_CONTEXT), m_NumArgs(NumArgs), m_ppArguments(ppArguments) {}

			~CHeadlessRenderer() override
			{
				if(!m_Opened)
					return;
				// The pictures go back to the graphics card before it is shut
				// down, so they are let go of by hand.
				if(m_pRenderer != nullptr)
					m_pRenderer->Clear();
				m_pRenderer = nullptr;
				m_pImages = nullptr;
				m_View.Shutdown();
			}

			bool Render(const map_document::CMapState &Map, const CRenderRequest &Request, std::vector<uint8_t> *pvPng, int *pWidth, int *pHeight, std::string *pError) override
			{
				const int64_t Start = time_get_nanoseconds().count();
				if(!Open(Request.m_Width, Request.m_Height, pError))
					return false;
				if(Request.m_Entities != m_Entities)
				{
					m_pImages->SetEntities(Request.m_Entities.c_str());
					m_Entities = Request.m_Entities;
				}
				// Handing the version over costs its pointers; the renderer
				// keeps the geometry of every layer that is the same node as
				// last time, so a picture after a small change is cheap.
				const auto pMap = std::make_shared<const map_document::CMapState>(Map);
				m_pImages->Use(*pMap);
				while(m_pImages->Loading())
					m_pImages->Update();
				m_pRenderer->Use(pMap);

				map_document::CView Camera;
				Camera.SetSurface(m_View.Width(), m_View.Height());
				CDocumentRenderer::CParams Params;
				Params.m_Center = Request.m_Center;
				Params.m_Zoom = Request.m_Zoom;
				Params.m_ViewSize = Camera.ViewSize();
				Params.m_TimeOffsetMillis = Request.m_TimeOffsetMillis;
				Params.m_EntityOverlayVal = Request.m_EntityOverlay;
				Params.m_pHidden = &Request.m_vHidden;
				Params.m_Grid = Request.m_Grid;
				Params.m_GridGroup = Request.m_GridGroup;
				Params.m_Marked.m_Group = Request.m_MarkGroup;
				Params.m_Marked.m_X = Request.m_Mark.m_X;
				Params.m_Marked.m_Y = Request.m_Mark.m_Y;
				Params.m_Marked.m_Width = Request.m_Mark.m_Width;
				Params.m_Marked.m_Height = Request.m_Mark.m_Height;
				Params.m_ShownQuad.m_Shown = Request.m_ShowQuad;
				Params.m_ShownQuad.m_Group = Request.m_QuadGroup;
				Params.m_ShownQuad.m_Layer = Request.m_QuadLayer;
				Params.m_ShownQuad.m_Quad = Request.m_Quad;

				IGraphics *pGraphics = m_View.Graphics();
				pGraphics->MapScreen(CScreenRect(0.0f, 0.0f, m_View.Width(), m_View.Height()));
				pGraphics->Clear(0.0f, 0.0f, 0.0f);
				m_pRenderer->Render(Params);

				CImageInfo Image;
				if(!m_View.ReadFrame(Image))
				{
					*pError = "the graphics backend gave no picture back";
					return false;
				}
				CByteBufferWriter Writer;
				const bool Saved = CImageLoader::SavePng(Writer, Image);
				*pWidth = (int)Image.m_Width;
				*pHeight = (int)Image.m_Height;
				Image.Free();
				if(!Saved)
				{
					*pError = "the picture could not be written as PNG";
					return false;
				}
				pvPng->assign(Writer.Data(), Writer.Data() + Writer.Size());
				m_LastNanos = time_get_nanoseconds().count() - Start;
				return true;
			}

			int64_t LastRenderNanos() const override { return m_LastNanos; }

		private:
			bool Open(int Width, int Height, std::string *pError)
			{
				if(m_Failed)
				{
					*pError = m_Error;
					return false;
				}
				if(!m_Opened)
				{
					if(!m_View.Init(m_NumArgs, m_ppArguments))
						return Fail("the storage could not be set up", pError);
					if(!m_View.OpenWindow(Width, Height, CreateOffscreenGraphicsWindow(), false))
						return Fail("no graphics backend could be started; a Vulkan driver is needed, a software one will do", pError);
					m_pImages = std::make_unique<CDocumentImages>(m_View.Graphics(), m_View.Storage(), m_View.AssetLoader(), nullptr, LOG_CONTEXT);
					m_pImages->SetEntities(m_Entities.c_str());
					m_pRenderer = std::make_unique<CDocumentRenderer>();
					m_pRenderer->OnInit(m_View.Graphics(), m_pImages.get());
					m_Opened = true;
					log_info(LOG_CONTEXT, "opened an offscreen surface of %dx%d", m_View.Width(), m_View.Height());
				}
				if(m_View.Width() != Width || m_View.Height() != Height)
				{
					m_View.Window()->Resize(Width, Height, 0);
					m_View.OnResize(m_View.Graphics()->ScreenWidth(), m_View.Graphics()->ScreenHeight());
				}
				return true;
			}

			bool Fail(const char *pMessage, std::string *pError)
			{
				m_Failed = true;
				m_Error = pMessage;
				*pError = pMessage;
				return false;
			}

			CStandaloneMapView m_View;
			int m_NumArgs;
			const char **m_ppArguments;
			bool m_Opened = false;
			bool m_Failed = false;
			std::string m_Error;
			std::string m_Entities = "ddnet";
			std::unique_ptr<CDocumentImages> m_pImages;
			std::unique_ptr<CDocumentRenderer> m_pRenderer;
			int64_t m_LastNanos = 0;
		};
	} // namespace

	std::unique_ptr<IRenderer> CreateHeadlessRenderer(int NumArgs, const char **ppArguments, std::string *pError)
	{
		(void)pError;
		return std::make_unique<CHeadlessRenderer>(NumArgs, ppArguments);
	}
} // namespace map_mcp
