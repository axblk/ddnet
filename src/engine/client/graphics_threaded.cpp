/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "graphics_threaded.h"

#include <base/dbg.h>
#include <base/detect.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/client/backend/backend_base.h>
#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/jobs.h>
#include <engine/storage.h>

#include <generated/data_types.h>

#include <game/localization.h>

#if defined(CONF_VIDEORECORDER)
#include <engine/shared/video.h>
#endif

#include <algorithm>
#include <limits>
#include <memory>

class CSemaphore;

static CVideoMode g_aFakeModes[] = {
	{8192, 4320, 8192, 4320, 0}, {7680, 4320, 7680, 4320, 0}, {5120, 2880, 5120, 2880, 0},
	{4096, 2160, 4096, 2160, 0}, {3840, 2160, 3840, 2160, 0}, {2560, 1440, 2560, 1440, 0},
	{2048, 1536, 2048, 1536, 0}, {1920, 2400, 1920, 2400, 0}, {1920, 1440, 1920, 1440, 0},
	{1920, 1200, 1920, 1200, 0}, {1920, 1080, 1920, 1080, 0}, {1856, 1392, 1856, 1392, 0},
	{1800, 1440, 1800, 1440, 0}, {1792, 1344, 1792, 1344, 0}, {1680, 1050, 1680, 1050, 0},
	{1600, 1200, 1600, 1200, 0}, {1600, 1000, 1600, 1000, 0}, {1440, 1050, 1440, 1050, 0},
	{1440, 900, 1440, 900, 0}, {1400, 1050, 1400, 1050, 0}, {1368, 768, 1368, 768, 0},
	{1280, 1024, 1280, 1024, 0}, {1280, 960, 1280, 960, 0}, {1280, 800, 1280, 800, 0},
	{1280, 768, 1280, 768, 0}, {1152, 864, 1152, 864, 0}, {1024, 768, 1024, 768, 0},
	{1024, 600, 1024, 600, 0}, {800, 600, 800, 600, 0}, {768, 576, 768, 576, 0},
	{720, 400, 720, 400, 0}, {640, 480, 640, 480, 0}, {400, 300, 400, 300, 0},
	{320, 240, 320, 240, 0}};

namespace
{
	constexpr int LEGACY_TEXTURE_LAYER_COLUMNS = 16;
	constexpr int LEGACY_TEXTURE_LAYER_ROWS = 16;

	EBackendType BackendOverrideFromEnvironment()
	{
		const char *pBackend = std::getenv("GFX_BACKEND");
		if(pBackend == nullptr)
			pBackend = std::getenv("DDNET_DRIVER");
		if(pBackend == nullptr)
			return BACKEND_TYPE_AUTO;
		if(str_comp_nocase(pBackend, "GLES") == 0)
			return BACKEND_TYPE_OPENGL_ES;
#if defined(CONF_BACKEND_VULKAN)
		if(str_comp_nocase(pBackend, "Vulkan") == 0)
			return BACKEND_TYPE_VULKAN;
#endif
#if defined(CONF_BACKEND_WEBGPU)
		if(str_comp_nocase(pBackend, "WebGPU") == 0)
			return BACKEND_TYPE_WEBGPU;
#endif
		return BACKEND_TYPE_OPENGL;
	}

	bool CheckedMul(size_t Left, size_t Right, size_t &Result)
	{
		if(Left != 0 && Right > std::numeric_limits<size_t>::max() / Left)
			return false;
		Result = Left * Right;
		return true;
	}

	bool CheckedAdd(size_t Left, size_t Right, size_t &Result)
	{
		if(Right > std::numeric_limits<size_t>::max() - Left)
			return false;
		Result = Left + Right;
		return true;
	}

	bool CheckedAlign(size_t Value, size_t Alignment, size_t &Result)
	{
		const size_t Remainder = Value % Alignment;
		return CheckedAdd(Value, Remainder == 0 ? 0 : Alignment - Remainder, Result);
	}
}

void CGraphics_Threaded::FlushVertices(bool KeepVertices)
{
	FlushVerticesImpl(KeepVertices, Pipeline(EPipelineProgram::PRIMITIVE), m_aVertices);
}

void CGraphics_Threaded::FlushVerticesTex3D()
{
	FlushVerticesImpl(false, Pipeline(EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY), m_aVerticesTex3D);
}

void CGraphics_Threaded::AddVertices(int Count)
{
	m_NumVertices += Count;
	if((m_NumVertices + Count) >= CCommandBuffer::MAX_VERTICES)
		FlushVertices();
}

void CGraphics_Threaded::AddVertices(int Count, CCommandBuffer::SVertex *pVertices)
{
	AddVertices(Count);
}

void CGraphics_Threaded::AddVertices(int Count, CCommandBuffer::SVertexTex3DStream *pVertices)
{
	m_NumVertices += Count;
	if((m_NumVertices + Count) >= CCommandBuffer::MAX_VERTICES)
		FlushVerticesTex3D();
}

CGraphics_Threaded::CGraphics_Threaded()
{
	m_State.m_ScreenTL.x = 0;
	m_State.m_ScreenTL.y = 0;
	m_State.m_ScreenBR.x = 0;
	m_State.m_ScreenBR.y = 0;
	m_State.m_ClipEnable = false;
	m_State.m_ClipX = 0;
	m_State.m_ClipY = 0;
	m_State.m_ClipW = 0;
	m_State.m_ClipH = 0;
	m_State.m_Texture.Invalidate();
	m_State.m_BlendMode = EBlendMode::ALPHA;
	m_State.m_WrapMode = EWrapMode::REPEAT;

	m_CurrentCommandBuffer = 0;
	m_pCommandBuffer = nullptr;
	m_apCommandBuffers[0] = nullptr;
	m_apCommandBuffers[1] = nullptr;
	m_CurrentReliableCommandBuffer = 0;
	m_pReliableCommandBuffer = nullptr;
	m_apReliableCommandBuffers[0] = nullptr;
	m_apReliableCommandBuffers[1] = nullptr;
	m_pDeferredDestroyCommandBuffer = nullptr;
	m_DropCurrentFrame = false;

	m_NumVertices = 0;

	m_ScreenWidth = -1;
	m_ScreenHeight = -1;
	m_ScreenRefreshRate = -1;

	m_Rotation = 0;
	m_Drawing = EDrawing::NONE;

	m_TextureMemoryUsage = 0;

	m_RenderEnable = true;
	m_DoScreenshot = false;
}

void CGraphics_Threaded::ClipEnable(int x, int y, int w, int h)
{
	if(x < 0)
		w += x;
	if(y < 0)
		h += y;

	x = std::clamp(x, 0, ScreenWidth());
	y = std::clamp(y, 0, ScreenHeight());
	w = std::clamp(w, 0, ScreenWidth() - x);
	h = std::clamp(h, 0, ScreenHeight() - y);

	m_State.m_ClipEnable = true;
	m_State.m_ClipX = x;
	m_State.m_ClipY = ScreenHeight() - (y + h);
	m_State.m_ClipW = w;
	m_State.m_ClipH = h;
}

void CGraphics_Threaded::ClipDisable()
{
	m_State.m_ClipEnable = false;
}

void CGraphics_Threaded::BlendNone()
{
	m_State.m_BlendMode = EBlendMode::NONE;
}

void CGraphics_Threaded::BlendNormal()
{
	m_State.m_BlendMode = EBlendMode::ALPHA;
}

void CGraphics_Threaded::BlendAdditive()
{
	m_State.m_BlendMode = EBlendMode::ADDITIVE;
}

void CGraphics_Threaded::WrapNormal()
{
	m_State.m_WrapMode = EWrapMode::REPEAT;
}

void CGraphics_Threaded::WrapClamp()
{
	m_State.m_WrapMode = EWrapMode::CLAMP;
}

uint64_t CGraphics_Threaded::TextureMemoryUsage() const
{
	return m_pBackend->TextureMemoryUsage();
}

uint64_t CGraphics_Threaded::BufferMemoryUsage() const
{
	return m_pBackend->BufferMemoryUsage();
}

uint64_t CGraphics_Threaded::StreamedMemoryUsage() const
{
	return m_pBackend->StreamedMemoryUsage();
}

uint64_t CGraphics_Threaded::StagingMemoryUsage() const
{
	return m_pBackend->StagingMemoryUsage();
}

IGraphics::SFrameMailboxStats CGraphics_Threaded::FrameMailboxStats() const
{
	return m_pBackend->GetFrameMailboxStats();
}

const TTwGraphicsGpuList &CGraphics_Threaded::GetGpus() const
{
	return m_pBackend->GetGpus();
}

void CGraphics_Threaded::MapScreen(const CScreenRect &ScreenRect)
{
	m_State.m_ScreenTL = ScreenRect.m_TopLeft;
	m_State.m_ScreenBR = ScreenRect.m_BottomRight;
}

CScreenRect CGraphics_Threaded::GetScreen() const
{
	return CScreenRect(m_State.m_ScreenTL, m_State.m_ScreenBR);
}

void CGraphics_Threaded::LinesBegin()
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->LinesBegin twice");
	m_Drawing = EDrawing::LINES;
	SetColor(1, 1, 1, 1);
}

void CGraphics_Threaded::LinesEnd()
{
	dbg_assert(m_Drawing == EDrawing::LINES, "called Graphics()->LinesEnd without begin");
	FlushVertices();
	m_Drawing = EDrawing::NONE;
}

void CGraphics_Threaded::LinesDraw(const CLineItem *pArray, size_t Num)
{
	dbg_assert(m_Drawing == EDrawing::LINES, "called Graphics()->LinesDraw without begin");

	size_t VertexIndex = m_NumVertices;
	for(size_t i = 0; i < Num; ++i)
	{
		m_aVertices[VertexIndex].m_Pos.x = pArray[i].m_X0;
		m_aVertices[VertexIndex].m_Pos.y = pArray[i].m_Y0;
		m_aVertices[VertexIndex].m_Tex = m_aTexture[0];
		SetColor(&m_aVertices[VertexIndex], 0);
		++VertexIndex;

		m_aVertices[VertexIndex].m_Pos.x = pArray[i].m_X1;
		m_aVertices[VertexIndex].m_Pos.y = pArray[i].m_Y1;
		m_aVertices[VertexIndex].m_Tex = m_aTexture[1];
		SetColor(&m_aVertices[VertexIndex], 1);
		++VertexIndex;
	}

	AddVertices(2 * Num);
}

void CGraphics_Threaded::LinesBatchBegin(CLineItemBatch *pBatch)
{
	pBatch->m_NumItems = 0;
	LinesBegin();
}

void CGraphics_Threaded::LinesBatchEnd(CLineItemBatch *pBatch)
{
	if(pBatch->m_NumItems > 0)
	{
		LinesDraw(pBatch->m_aItems, pBatch->m_NumItems);
		pBatch->m_NumItems = 0;
	}
	LinesEnd();
}

void CGraphics_Threaded::LinesBatchDraw(CLineItemBatch *pBatch, const CLineItem *pArray, size_t Num)
{
	if(pBatch->m_NumItems + Num >= std::size(pBatch->m_aItems))
	{
		LinesDraw(pBatch->m_aItems, pBatch->m_NumItems);
		pBatch->m_NumItems = 0;
	}
	if(Num >= std::size(pBatch->m_aItems))
	{
		LinesDraw(pArray, Num);
		return;
	}
	std::copy(pArray, pArray + Num, pBatch->m_aItems + pBatch->m_NumItems);
	pBatch->m_NumItems += Num;
}

IGraphics::CTextureHandle CGraphics_Threaded::FindFreeTextureIndex()
{
	return m_TextureHandles.Allocate();
}

void CGraphics_Threaded::FreeTextureIndex(CTextureHandle *pIndex)
{
	dbg_assert(pIndex->IsValid(), "Cannot free invalid texture index");
	const bool Released = m_TextureHandles.Release(pIndex);
	dbg_assert(Released, "Cannot free stale or already freed texture handle");
}

void CGraphics_Threaded::UnloadTexture(CTextureHandle *pIndex)
{
	if(!pIndex->IsValid() || pIndex->Id() == 0)
		return;
	if(m_RenderPassActive && m_RenderPassTarget == *pIndex)
		EndRenderPass();

	CCommandBuffer::SCommand_Texture_Destroy Cmd;
	Cmd.m_Texture = *pIndex;
	if(!AddCmd(Cmd))
		return;
	const CTextureHandle RetiredHandle = *pIndex;
	dbg_assert(m_TextureHandles.Retire(pIndex), "Cannot retire stale texture handle");
	m_vRetiredTextureHandles.push_back(RetiredHandle);
}

IGraphics::CTextureHandle CGraphics_Threaded::LoadSpriteTexture(const CImageInfo &FromImageInfo, const std::optional<CImageInfo> &FallbackImageInfo, const CDataSprite *pSprite)
{
	int ImageGridX = FromImageInfo.m_Width / pSprite->m_pSet->m_Gridx;
	int ImageGridY = FromImageInfo.m_Height / pSprite->m_pSet->m_Gridy;
	int x = pSprite->m_X * ImageGridX;
	int y = pSprite->m_Y * ImageGridY;
	int w = pSprite->m_W * ImageGridX;
	int h = pSprite->m_H * ImageGridY;

	// check for invisible texture, maybe due to outdated game assets
	if(FallbackImageInfo.has_value() && IsImageSubFullyTransparent(FromImageInfo, x, y, w, h))
	{
		log_warn("graphics", "Asset '%s' appears to be invisible, falling back to default", pSprite->m_pName);
		return LoadSpriteTexture(FallbackImageInfo.value(), std::nullopt, pSprite);
	}

	CImageInfo SpriteInfo;
	SpriteInfo.m_Width = w;
	SpriteInfo.m_Height = h;
	SpriteInfo.m_Format = FromImageInfo.m_Format;
	SpriteInfo.Allocate();
	SpriteInfo.CopyRectFrom(FromImageInfo, x, y, w, h, 0, 0);
	return LoadTextureRawMove(SpriteInfo, 0, pSprite->m_pName);
}

bool CGraphics_Threaded::IsImageSubFullyTransparent(const CImageInfo &FromImageInfo, int x, int y, int w, int h)
{
	if(FromImageInfo.m_Format == CImageInfo::FORMAT_R || FromImageInfo.m_Format == CImageInfo::FORMAT_RA || FromImageInfo.m_Format == CImageInfo::FORMAT_RGBA)
	{
		const uint8_t *pImgData = FromImageInfo.m_pData;
		const size_t PixelSize = FromImageInfo.PixelSize();
		for(int iy = 0; iy < h; ++iy)
		{
			for(int ix = 0; ix < w; ++ix)
			{
				const size_t RealOffset = (x + ix) * PixelSize + (y + iy) * PixelSize * FromImageInfo.m_Width;
				if(pImgData[RealOffset + (PixelSize - 1)] > 0)
					return false;
			}
		}

		return true;
	}
	return false;
}

bool CGraphics_Threaded::IsSpriteTextureFullyTransparent(const CImageInfo &FromImageInfo, const CDataSprite *pSprite)
{
	int ImageGridX = FromImageInfo.m_Width / pSprite->m_pSet->m_Gridx;
	int ImageGridY = FromImageInfo.m_Height / pSprite->m_pSet->m_Gridy;
	int x = pSprite->m_X * ImageGridX;
	int y = pSprite->m_Y * ImageGridY;
	int w = pSprite->m_W * ImageGridX;
	int h = pSprite->m_H * ImageGridY;
	return IsImageSubFullyTransparent(FromImageInfo, x, y, w, h);
}

void CGraphics_Threaded::LoadTextureAddWarning(const CTextureDesc &Desc, const char *pTexName)
{
	if(Desc.m_Layering != ETextureLayering::NONE)
	{
		if(Desc.m_Width == 0 || (Desc.m_Width % Desc.m_LayerColumns) != 0 || Desc.m_Height == 0 || (Desc.m_Height % Desc.m_LayerRows) != 0)
		{
			SWarning NewWarning;
			char aText[128];
			str_format(aText, sizeof(aText), "\"%s\"", pTexName ? pTexName : "(no name)");
			str_format(NewWarning.m_aWarningMsg, sizeof(NewWarning.m_aWarningMsg), Localize("The width of texture %s is not divisible by %d, or the height is not divisible by %d, which might cause visual bugs."), aText, Desc.m_LayerColumns, Desc.m_LayerRows);
			AddWarning(NewWarning);
		}
	}
}

static IGraphics::CTextureDesc LoadTextureDesc(size_t Width, size_t Height, int Flags)
{
	IGraphics::CTextureDesc Desc;
	Desc.m_Width = Width;
	Desc.m_Height = Height;
	Desc.m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::GENERATE;
	Desc.m_Create2D = (Flags & IGraphics::TEXLOAD_NO_2D_TEXTURE) == 0;
	if((Flags & IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE) != 0)
		Desc.m_Layering = IGraphics::ETextureLayering::ARRAY_2D;
	else if((Flags & IGraphics::TEXLOAD_TO_3D_TEXTURE) != 0)
		Desc.m_Layering = IGraphics::ETextureLayering::VOLUME_3D;
	if(Desc.m_Layering != IGraphics::ETextureLayering::NONE)
	{
		Desc.m_LayerColumns = LEGACY_TEXTURE_LAYER_COLUMNS;
		Desc.m_LayerRows = LEGACY_TEXTURE_LAYER_ROWS;
	}

	return Desc;
}

IGraphics::CTextureHandle CGraphics_Threaded::CreateTexture(const CTextureDesc &Desc, const void *pInitialData)
{
	if(!Desc.IsValid() || (Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET) && !m_Capabilities.m_RenderTargets) ||
		!IsTextureLayeringSupported(Desc.m_Layering) ||
		(pInitialData == nullptr && !Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET)))
		return {};

	uint8_t *pOwnedData = nullptr;
	if(pInitialData != nullptr)
	{
		const size_t PixelSize = Desc.m_Format == ETextureFormat::RGBA8_UNORM ? 4 : 1;
		if(Desc.m_Width > std::numeric_limits<size_t>::max() / PixelSize || Desc.m_Height > std::numeric_limits<size_t>::max() / (Desc.m_Width * PixelSize))
			return {};
		const size_t DataSize = Desc.m_Width * Desc.m_Height * PixelSize;
		pOwnedData = static_cast<uint8_t *>(malloc(DataSize));
		if(pOwnedData == nullptr)
			return {};
		mem_copy(pOwnedData, pInitialData, DataSize);
	}

	CTextureHandle Texture = FindFreeTextureIndex();
	CCommandBuffer::SCommand_Texture_Create Cmd;
	Cmd.m_Texture = Texture;
	Cmd.m_Desc = Desc;
	Cmd.m_pData = pOwnedData;
	if(!AddCmd(Cmd))
	{
		free(pOwnedData);
		FreeTextureIndex(&Texture);
	}
	else
	{
		StoreTextureInfo(Texture, Desc);
	}
	return Texture;
}

bool CGraphics_Threaded::ReadTexture(CTextureHandle Texture, CImageInfo &Image)
{
	auto pReadback = ReadTextureAsync(Texture);
	return pReadback != nullptr && pReadback->Wait(Image);
}

namespace
{
	class CTextureReadback final : public IGraphics::ITextureReadback
	{
		std::unique_ptr<CCommandBuffer::SImageReadbackResult> m_pResult;
		bool m_Waited = false;

	public:
		explicit CTextureReadback(std::unique_ptr<CCommandBuffer::SImageReadbackResult> pResult) :
			m_pResult(std::move(pResult))
		{
		}

		~CTextureReadback() override
		{
			if(!m_Waited)
				m_pResult->Wait();
		}

		bool IsReady() const override
		{
			return m_pResult->IsComplete();
		}

		bool Wait(CImageInfo &Image) override
		{
			if(m_Waited)
				return false;
			m_pResult->Wait();
			m_Waited = true;
			if(!m_pResult->m_Ok)
				return false;
			Image = std::move(m_pResult->m_Image);
			return true;
		}
	};
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::ReadTextureAsync(CTextureHandle Texture)
{
	if(m_Drawing != EDrawing::NONE || m_RenderPassActive || !m_TextureHandles.IsAllocated(Texture) || static_cast<size_t>(Texture.Id()) >= m_vTextureInfos.size())
		return nullptr;
	const STextureInfo &Info = m_vTextureInfos[Texture.Id()];
	if(Info.m_Handle != Texture || !Info.m_Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET) || !Info.m_Desc.HasUsage(TEXTURE_USAGE_COPY_SOURCE))
		return nullptr;
	if(!SubmitReliableCommandBuffer(m_pReliableCommandBuffer) || !SubmitFramePacket())
		return nullptr;

	auto pResult = std::make_unique<CCommandBuffer::SImageReadbackResult>();
	CCommandBuffer::SCommand_Texture_Readback Cmd;
	Cmd.m_Texture = Texture;
	Cmd.m_pResult = pResult.get();
	Cmd.m_pCompletion = pResult.get();
	if(!AddCmdBlocking(Cmd) || !KickCommandBuffer())
		return nullptr;
	return std::make_unique<CTextureReadback>(std::move(pResult));
}

bool CGraphics_Threaded::BeginOffscreenFrame(CTextureHandle Texture)
{
	if(m_OffscreenFrameTarget.IsValid() || m_Drawing != EDrawing::NONE || !m_TextureHandles.IsAllocated(Texture) || static_cast<size_t>(Texture.Id()) >= m_vTextureInfos.size())
		return false;
	const STextureInfo &Info = m_vTextureInfos[Texture.Id()];
	if(Info.m_Handle != Texture || !Info.m_Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET) || !Info.m_Desc.HasUsage(TEXTURE_USAGE_COPY_SOURCE))
		return false;

	m_OffscreenFrameTarget = Texture;
	CRenderPassDesc Pass;
	Pass.m_ColorTarget = Texture;
	if(!BeginRenderPass(Pass))
	{
		m_OffscreenFrameTarget.Invalidate();
		return false;
	}
	return true;
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::EndOffscreenFrame()
{
	if(!m_OffscreenFrameTarget.IsValid())
		return nullptr;
	const CTextureHandle Target = m_OffscreenFrameTarget;
	const bool CanFinish = m_Drawing == EDrawing::NONE && m_RenderPassActive && m_RenderPassTarget == Target;
	const bool FrameEnded = CanFinish && EndRenderPass();
	m_OffscreenFrameTarget.Invalidate();

	auto pReadback = FrameEnded ? ReadTextureAsync(Target) : nullptr;
	if(!FrameEnded)
		DropCurrentFrame();
	if(pReadback != nullptr)
		SubmitDeferredDestroys();
	m_pCommandBuffer->Reset();
	m_DropCurrentFrame = false;
	m_SubmissionTracker.FinishFrame();
	m_RenderPassActive = true;
	m_RenderPassTarget.Invalidate();
	return pReadback;
}

IGraphics::CTextureHandle CGraphics_Threaded::LoadTextureRaw(const CImageInfo &Image, int Flags, const char *pTexName)
{
	const CTextureDesc Desc = LoadTextureDesc(Image.m_Width, Image.m_Height, Flags);
	if(!Desc.IsValid() || !IsTextureLayeringSupported(Desc.m_Layering))
		return IGraphics::CTextureHandle();
	LoadTextureAddWarning(Desc, pTexName);

	IGraphics::CTextureHandle TextureHandle = FindFreeTextureIndex();
	CCommandBuffer::SCommand_Texture_Create Cmd;
	Cmd.m_Texture = TextureHandle;
	Cmd.m_Desc = Desc;

	// Copy texture data and convert if necessary
	uint8_t *pTmpData;
	if(!ConvertToRgbaAlloc(pTmpData, Image))
	{
		log_warn("graphics", "Converted image '%s' to RGBA, consider making its file format RGBA.", pTexName ? pTexName : "(no name)");
	}
	Cmd.m_pData = pTmpData;

	if(!AddCmd(Cmd))
	{
		free(pTmpData);
		FreeTextureIndex(&TextureHandle);
	}
	else
	{
		StoreTextureInfo(TextureHandle, Desc);
	}

	return TextureHandle;
}

IGraphics::CTextureHandle CGraphics_Threaded::LoadTextureRawMove(CImageInfo &Image, int Flags, const char *pTexName)
{
	if(Image.m_Format != CImageInfo::FORMAT_RGBA)
	{
		// Moving not possible, texture needs to be converted
		IGraphics::CTextureHandle TextureHandle = LoadTextureRaw(Image, Flags, pTexName);
		if(TextureHandle.IsValid())
			Image.Free();
		return TextureHandle;
	}

	const CTextureDesc Desc = LoadTextureDesc(Image.m_Width, Image.m_Height, Flags);
	if(!Desc.IsValid() || !IsTextureLayeringSupported(Desc.m_Layering))
		return IGraphics::CTextureHandle();
	LoadTextureAddWarning(Desc, pTexName);

	IGraphics::CTextureHandle TextureHandle = FindFreeTextureIndex();
	CCommandBuffer::SCommand_Texture_Create Cmd;
	Cmd.m_Texture = TextureHandle;
	Cmd.m_Desc = Desc;
	Cmd.m_pData = Image.m_pData;
	if(AddCmd(Cmd))
	{
		StoreTextureInfo(TextureHandle, Desc);
		Image.m_pData = nullptr;
		Image.Free();
	}
	else
	{
		FreeTextureIndex(&TextureHandle);
	}

	return TextureHandle;
}

IGraphics::CTextureHandle CGraphics_Threaded::LoadTexture(const char *pFilename, int StorageType, int Flags)
{
	dbg_assert(pFilename[0] != '\0', "Cannot load texture from file with empty filename"); // would cause Valgrind to crash otherwise

	CImageInfo Image;
	if(LoadPng(Image, pFilename, StorageType))
	{
		CTextureHandle Id = LoadTextureRawMove(Image, Flags, pFilename);
		if(Id.IsValid())
		{
			if(g_Config.m_Debug)
				log_trace("graphics/texture", "Loaded texture '%s'", pFilename);
			return Id;
		}
		Image.Free();
	}

	return m_NullTexture;
}

bool CGraphics_Threaded::LoadTextTextures(size_t Width, size_t Height, CTextureHandle &TextTexture, CTextureHandle &TextOutlineTexture, uint8_t *pTextData, uint8_t *pTextOutlineData)
{
	if(Width == 0 || Height == 0)
		return false;

	CTextureDesc Desc;
	Desc.m_Width = Width;
	Desc.m_Height = Height;
	Desc.m_Format = ETextureFormat::R8_UNORM;
	Desc.m_Mipmaps = ETextureMipmaps::NONE;
	TextTexture = CreateTexture(Desc, pTextData);
	if(!TextTexture.IsValid())
		return false;
	TextOutlineTexture = CreateTexture(Desc, pTextOutlineData);
	if(!TextOutlineTexture.IsValid())
	{
		UnloadTexture(&TextTexture);
		return false;
	}
	if(!CreateTextureBinding(TextTexture, TextOutlineTexture).IsValid())
	{
		UnloadTexture(&TextTexture);
		UnloadTexture(&TextOutlineTexture);
		return false;
	}

	free(pTextData);
	free(pTextOutlineData);
	return true;
}

bool CGraphics_Threaded::UnloadTextTextures(CTextureHandle &TextTexture, CTextureHandle &TextOutlineTexture)
{
	if(!DeleteTextureBinding(TextTexture, TextOutlineTexture))
		return false;
	UnloadTexture(&TextTexture);
	UnloadTexture(&TextOutlineTexture);
	return !TextTexture.IsValid() && !TextOutlineTexture.IsValid();
}

CCommandBuffer::CTextureBindingHandle CGraphics_Threaded::CreateTextureBinding(CTextureHandle PrimaryTexture, CTextureHandle SecondaryTexture)
{
	dbg_assert(m_TextureHandles.IsAllocated(PrimaryTexture) && m_TextureHandles.IsAllocated(SecondaryTexture), "Cannot bind stale texture handles");
	for(const CTextureHandle Texture : {PrimaryTexture, SecondaryTexture})
	{
		if(static_cast<size_t>(Texture.Id()) >= m_vTextureInfos.size())
			return {};
		const STextureInfo &Info = m_vTextureInfos[Texture.Id()];
		if(Info.m_Handle != Texture || Info.m_Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET))
			return {};
	}
	auto Binding = m_TextureBindingHandles.Allocate();
	CCommandBuffer::SCommand_TextureBinding_Create Cmd;
	Cmd.m_Binding = Binding;
	Cmd.m_Desc.m_aTextures = {PrimaryTexture, SecondaryTexture};
	if(!AddCmd(Cmd))
	{
		m_TextureBindingHandles.Release(&Binding);
		return Binding;
	}
	if(static_cast<size_t>(PrimaryTexture.Id()) >= m_vTextureBindingInfos.size())
		m_vTextureBindingInfos.resize(PrimaryTexture.Id() + 1);
	m_vTextureBindingInfos[PrimaryTexture.Id()] = {{{PrimaryTexture, SecondaryTexture}}, Binding};
	return Binding;
}

CCommandBuffer::CTextureBindingHandle CGraphics_Threaded::FindTextureBinding(CTextureHandle PrimaryTexture, CTextureHandle SecondaryTexture) const
{
	if(!PrimaryTexture.IsValid() || static_cast<size_t>(PrimaryTexture.Id()) >= m_vTextureBindingInfos.size())
		return {};
	const auto &Info = m_vTextureBindingInfos[PrimaryTexture.Id()];
	return Info.m_Desc.m_aTextures == std::array{PrimaryTexture, SecondaryTexture} ? Info.m_Binding : CCommandBuffer::CTextureBindingHandle{};
}

bool CGraphics_Threaded::DeleteTextureBinding(CTextureHandle PrimaryTexture, CTextureHandle SecondaryTexture)
{
	auto Binding = FindTextureBinding(PrimaryTexture, SecondaryTexture);
	if(!Binding.IsValid())
		return true;
	CCommandBuffer::SCommand_TextureBinding_Destroy Cmd;
	Cmd.m_Binding = Binding;
	if(!AddCmd(Cmd))
		return false;
	dbg_assert(m_TextureBindingHandles.Retire(&Binding), "Cannot retire stale texture binding handle");
	m_vRetiredTextureBindingHandles.push_back(Cmd.m_Binding);
	m_vTextureBindingInfos[PrimaryTexture.Id()] = {};
	return true;
}

void CGraphics_Threaded::CreatePipelines()
{
	dbg_assert(m_pReliableCommandBuffer->IsEmpty(), "Pipeline catalog must be the first reliable resource batch");
	m_PipelineHandles.Reset(0);
	for(auto &PipelineHandle : m_aPipelines)
		PipelineHandle.Invalidate();
	for(size_t i = 0; i < m_aPipelines.size(); ++i)
	{
		m_aPipelines[i] = m_PipelineHandles.Allocate();
		CCommandBuffer::SCommand_Pipeline_Create Cmd;
		Cmd.m_Pipeline = m_aPipelines[i];
		Cmd.m_Desc.m_Program = static_cast<EPipelineProgram>(i);
		dbg_assert(m_pReliableCommandBuffer->AddCommandUnsafe(Cmd), "Pipeline catalog exceeds reliable command buffer capacity");
	}
}

bool CGraphics_Threaded::DestroyPipelines()
{
	bool Success = true;
	for(auto &PipelineHandle : m_aPipelines)
	{
		if(!PipelineHandle.IsValid())
			continue;
		CCommandBuffer::SCommand_Pipeline_Destroy Cmd;
		Cmd.m_Pipeline = PipelineHandle;
		if(!AddCmd(Cmd))
		{
			Success = false;
			continue;
		}
		const auto RetiredHandle = PipelineHandle;
		dbg_assert(m_PipelineHandles.Retire(&PipelineHandle), "Cannot retire stale pipeline handle");
		m_vRetiredPipelineHandles.push_back(RetiredHandle);
	}
	return Success;
}

bool CGraphics_Threaded::UpdateTextTexture(CTextureHandle TextureId, int x, int y, size_t Width, size_t Height, uint8_t *pData, bool IsMovedPointer)
{
	if(x < 0 || y < 0)
		return false;
	return UpdateTextureInternal(TextureId, {static_cast<size_t>(x), static_cast<size_t>(y), Width, Height}, ETextureFormat::R8_UNORM, pData, IsMovedPointer);
}

bool CGraphics_Threaded::UpdateTexture(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, const void *pData)
{
	if(!m_TextureHandles.IsAllocated(Texture) || static_cast<size_t>(Texture.Id()) >= m_vTextureInfos.size())
		return false;
	const STextureInfo &Info = m_vTextureInfos[Texture.Id()];
	if(Info.m_Handle != Texture || Info.m_Desc.m_Format != Format || Info.m_Desc.m_Mipmaps != ETextureMipmaps::NONE || !Info.m_Desc.m_Create2D || Info.m_Desc.m_Layering != ETextureLayering::NONE)
		return false;
	if(Region.m_X > Info.m_Desc.m_Width || Region.m_Width > Info.m_Desc.m_Width - Region.m_X || Region.m_Y > Info.m_Desc.m_Height || Region.m_Height > Info.m_Desc.m_Height - Region.m_Y)
		return false;
	return UpdateTextureInternal(Texture, Region, Format, const_cast<uint8_t *>(static_cast<const uint8_t *>(pData)), false);
}

bool CGraphics_Threaded::UpdateTextureInternal(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, uint8_t *pData, bool IsMovedPointer)
{
	if(!m_TextureHandles.IsAllocated(Texture))
		return false;
	size_t PixelSize;
	switch(Format)
	{
	case ETextureFormat::RGBA8_UNORM: PixelSize = 4; break;
	case ETextureFormat::R8_UNORM: PixelSize = 1; break;
	default: return false;
	}
	if(Region.m_Width == 0 || Region.m_Height == 0 || pData == nullptr || Region.m_Width > std::numeric_limits<size_t>::max() / PixelSize || Region.m_Height > std::numeric_limits<size_t>::max() / (Region.m_Width * PixelSize))
		return false;

	CCommandBuffer::SCommand_Texture_Update Cmd;
	Cmd.m_Texture = Texture;
	Cmd.m_Region = Region;
	Cmd.m_Format = Format;

	if(IsMovedPointer)
	{
		Cmd.m_pData = pData;
	}
	else
	{
		const size_t MemSize = Region.m_Width * Region.m_Height * PixelSize;
		uint8_t *pTmpData = static_cast<uint8_t *>(malloc(MemSize));
		if(pTmpData == nullptr)
			return false;
		mem_copy(pTmpData, pData, MemSize);
		Cmd.m_pData = pTmpData;
	}
	if(AddCmd(Cmd))
		return true;
	if(!IsMovedPointer)
		free(Cmd.m_pData);
	return false;
}

static SWarning FormatPngliteIncompatibilityWarning(int PngliteIncompatible, const char *pContextName)
{
	SWarning Warning;
	str_format(Warning.m_aWarningMsg, sizeof(Warning.m_aWarningMsg), Localize("\"%s\" is not compatible with pnglite and cannot be loaded by old DDNet versions:"), pContextName);
	str_append(Warning.m_aWarningMsg, " ");
	static const int FLAGS[] = {CImageLoader::PNGLITE_COLOR_TYPE, CImageLoader::PNGLITE_BIT_DEPTH, CImageLoader::PNGLITE_INTERLACE_TYPE, CImageLoader::PNGLITE_COMPRESSION_TYPE, CImageLoader::PNGLITE_FILTER_TYPE};
	static const char *const EXPLANATION[] = {"color type", "bit depth", "interlace type", "compression type", "filter type"};

	bool First = true;
	for(size_t i = 0; i < std::size(FLAGS); ++i)
	{
		if((PngliteIncompatible & FLAGS[i]) != 0)
		{
			if(!First)
			{
				str_append(Warning.m_aWarningMsg, ", ");
			}
			str_append(Warning.m_aWarningMsg, EXPLANATION[i]);
			First = false;
		}
	}
	str_append(Warning.m_aWarningMsg, " unsupported");
	return Warning;
}

bool CGraphics_Threaded::LoadPng(CImageInfo &Image, const char *pFilename, int StorageType)
{
	IOHANDLE File = m_pStorage->OpenFile(pFilename, IOFLAG_READ, StorageType);

	int PngliteIncompatible;
	if(!CImageLoader::LoadPng(File, pFilename, Image, PngliteIncompatible))
		return false;

	if(m_WarnPngliteIncompatibleImages && PngliteIncompatible != 0)
	{
		AddWarning(FormatPngliteIncompatibilityWarning(PngliteIncompatible, pFilename));
	}

	return true;
}

bool CGraphics_Threaded::LoadPng(CImageInfo &Image, const uint8_t *pData, size_t DataSize, const char *pContextName)
{
	CByteBufferReader Reader(pData, DataSize);
	int PngliteIncompatible;
	if(!CImageLoader::LoadPng(Reader, pContextName, Image, PngliteIncompatible))
		return false;

	if(m_WarnPngliteIncompatibleImages && PngliteIncompatible != 0)
	{
		AddWarning(FormatPngliteIncompatibilityWarning(PngliteIncompatible, pContextName));
	}

	return true;
}

bool CGraphics_Threaded::CheckImageDivisibility(const char *pContextName, CImageInfo &Image, int DivX, int DivY, bool AllowResize)
{
	dbg_assert(DivX != 0 && DivY != 0, "Passing 0 to this function is not allowed.");
	bool ImageIsValid = true;
	bool WidthBroken = Image.m_Width == 0 || (Image.m_Width % DivX) != 0;
	bool HeightBroken = Image.m_Height == 0 || (Image.m_Height % DivY) != 0;
	if(WidthBroken || HeightBroken)
	{
		SWarning NewWarning;
		char aContextNameQuoted[128];
		str_format(aContextNameQuoted, sizeof(aContextNameQuoted), "\"%s\"", pContextName);
		str_format(NewWarning.m_aWarningMsg, sizeof(NewWarning.m_aWarningMsg),
			Localize("The width of texture %s is not divisible by %d, or the height is not divisible by %d, which might cause visual bugs."), aContextNameQuoted, DivX, DivY);
		AddWarning(NewWarning);
		ImageIsValid = false;
	}

	if(AllowResize && !ImageIsValid && Image.m_Width > 0 && Image.m_Height > 0)
	{
		int NewWidth = DivX;
		int NewHeight = DivY;
		if(WidthBroken)
		{
			NewWidth = std::max(HighestBit(Image.m_Width), DivX);
			NewHeight = (NewWidth / DivX) * DivY;
		}
		else
		{
			NewHeight = std::max(HighestBit(Image.m_Height), DivY);
			NewWidth = (NewHeight / DivY) * DivX;
		}
		ResizeImage(Image, NewWidth, NewHeight);
		ImageIsValid = true;
	}

	return ImageIsValid;
}

bool CGraphics_Threaded::IsImageFormatRgba(const char *pContextName, const CImageInfo &Image)
{
	if(Image.m_Format != CImageInfo::FORMAT_RGBA)
	{
		SWarning NewWarning;
		char aContextNameQuoted[128];
		str_format(aContextNameQuoted, sizeof(aContextNameQuoted), "\"%s\"", pContextName);
		str_format(NewWarning.m_aWarningMsg, sizeof(NewWarning.m_aWarningMsg),
			Localize("The format of texture %s is not RGBA which will cause visual bugs."), aContextNameQuoted);
		AddWarning(NewWarning);
		return false;
	}
	return true;
}

void CGraphics_Threaded::CollectBackendQueueWarnings()
{
	SGfxWarningContainer Warning;
	if(m_pBackend->GetWarning(Warning))
	{
		switch(Warning.m_WarningType)
		{
		case GFX_WARNING_TYPE_INIT_FAILED:
			Warning.m_vWarnings.emplace_back(Localize("Could not initialize the given graphics backend, reverting to the default backend now.", "Graphics error"));
			break;
		case GFX_WARNING_TYPE_INIT_FAILED_MISSING_INTEGRATED_GPU_DRIVER:
			Warning.m_vWarnings.emplace_back(Localize("Could not initialize the given graphics backend, this is probably because you didn't install the driver of the integrated graphics card.", "Graphics error"));
			break;
		case GFX_WARNING_TYPE_INIT_FAILED_NO_DEVICE_WITH_REQUIRED_VERSION:
			// A console message was already printed by the backend.
			return;
		case GFX_WARNING_MISSING_EXTENSION:
			break;
		case GFX_WARNING_TYPE_NONE:
			return;
		default:
			dbg_assert_failed("Unhandled graphics warning type %d", (int)Warning.m_WarningType);
			break;
		}

		SWarning NewWarning;
		std::string WarningStr;
		for(const auto &WarnStr : Warning.m_vWarnings)
			WarningStr.append((WarnStr + "\n"));
		str_copy(NewWarning.m_aWarningMsg, WarningStr.c_str());
		AddWarning(NewWarning);
	}
}

bool CGraphics_Threaded::SubmitReliableCommandBuffer(CCommandBuffer *pCommandBuffer, bool WaitForCapacity)
{
	if(pCommandBuffer == nullptr || pCommandBuffer->IsEmpty())
		return true;
	dbg_assert(pCommandBuffer == m_pReliableCommandBuffer || pCommandBuffer == m_pDeferredDestroyCommandBuffer, "graphics: submitted unknown reliable command buffer");
	if(pCommandBuffer->SubmissionInfo().m_SubmissionSerial == 0)
	{
		// Deferred destroys are ordered after the frame that last used them. They are
		// deliberately not a prerequisite of a later frame while their handles remain retired.
		const bool RequiredByFrame = pCommandBuffer != m_pDeferredDestroyCommandBuffer && pCommandBuffer->ContainsResourceCommands();
		pCommandBuffer->SetSubmissionInfo(m_SubmissionTracker.Prepare(CCommandBuffer::ECommandChannel::RELIABLE, RequiredByFrame, false));
	}

	WaitForCapacity |= pCommandBuffer->ContainsCompletions() || pCommandBuffer->ContainsCommand(CCommandBuffer::CMD_SIGNAL);
	if(!m_pBackend->RunBufferQueued(pCommandBuffer, WaitForCapacity))
		return false;
	CollectBackendQueueWarnings();

	if(pCommandBuffer == m_pReliableCommandBuffer)
	{
		m_CurrentReliableCommandBuffer ^= 1;
		m_pReliableCommandBuffer = m_apReliableCommandBuffers[m_CurrentReliableCommandBuffer];
		m_pReliableCommandBuffer->Reset();
	}
	else
		pCommandBuffer->Reset();
	return true;
}

bool CGraphics_Threaded::SubmitFramePacket()
{
	if(m_DropCurrentFrame || m_pCommandBuffer->IsEmpty())
	{
		m_pCommandBuffer->Reset();
		return !m_DropCurrentFrame;
	}
	if(m_pCommandBuffer->SubmissionInfo().m_SubmissionSerial == 0)
		m_pCommandBuffer->SetSubmissionInfo(m_SubmissionTracker.Prepare(CCommandBuffer::ECommandChannel::FRAME, false, m_pCommandBuffer->ContainsCommand(CCommandBuffer::CMD_SWAP)));

	if(!m_pBackend->RunFramePacket(m_pCommandBuffer))
	{
		// Capture packets carry synchronous work and are deliberately pinned. Normal
		// replaceable frames are accepted or dropped by the bounded mailbox instead.
		if(m_pCommandBuffer->IsReplaceableFramePacket())
		{
			dbg_assert(false, "graphics: replaceable frame packet was rejected by the mailbox");
			return false;
		}
		if(!m_pBackend->RunFramePacket(m_pCommandBuffer, true))
			return false;
	}
	CollectBackendQueueWarnings();
	m_CurrentCommandBuffer ^= 1;
	m_pCommandBuffer = m_apCommandBuffers[m_CurrentCommandBuffer];
	m_pCommandBuffer->Reset();
	return true;
}

void CGraphics_Threaded::RecycleRetiredHandles()
{
	for(const CTextureHandle Handle : m_vRetiredTextureHandles)
		dbg_assert(m_TextureHandles.Recycle(Handle), "graphics: failed to recycle retired texture handle");
	for(const auto Handle : m_vRetiredTextureBindingHandles)
		dbg_assert(m_TextureBindingHandles.Recycle(Handle), "graphics: failed to recycle retired texture binding handle");
	for(const auto Handle : m_vRetiredPipelineHandles)
		dbg_assert(m_PipelineHandles.Recycle(Handle), "graphics: failed to recycle retired pipeline handle");
	for(const CBufferHandle Handle : m_vRetiredBufferHandles)
		dbg_assert(m_BufferHandles.Recycle(Handle), "graphics: failed to recycle retired buffer handle");
	for(const CBufferContainerHandle Handle : m_vRetiredBufferContainerHandles)
		dbg_assert(m_BufferContainerHandles.Recycle(Handle), "graphics: failed to recycle retired buffer container handle");
	m_vRetiredTextureHandles.clear();
	m_vRetiredTextureBindingHandles.clear();
	m_vRetiredPipelineHandles.clear();
	m_vRetiredBufferHandles.clear();
	m_vRetiredBufferContainerHandles.clear();
}

bool CGraphics_Threaded::SubmitDeferredDestroys()
{
	if(m_pDeferredDestroyCommandBuffer->IsEmpty())
		return true;
	if(!SubmitReliableCommandBuffer(m_pDeferredDestroyCommandBuffer))
		return false;
	RecycleRetiredHandles();
	return true;
}

void CGraphics_Threaded::DropCurrentFrame()
{
	m_DropCurrentFrame = true;
	m_pCommandBuffer->Reset();
}

CCommandBuffer *CGraphics_Threaded::GetCommandBuffer(unsigned Command)
{
	if(CCommandBuffer::CommandChannel(Command) == CCommandBuffer::ECommandChannel::FRAME)
		return m_pCommandBuffer;
	if(CCommandBuffer::IsDeferredDestroyCommand(Command))
		return m_pDeferredDestroyCommandBuffer;
	return m_pReliableCommandBuffer;
}

bool CGraphics_Threaded::KickCommandBuffer()
{
	return SubmitReliableCommandBuffer(m_pReliableCommandBuffer);
}

class CScreenshotSaveJob : public IJob
{
	IStorage *m_pStorage;
	char m_aName[IO_MAX_PATH_LENGTH];
	CImageInfo m_Image;

	void Run() override
	{
		static constexpr LOG_COLOR SCREENSHOT_LOG_COLOR = LOG_COLOR{255, 153, 76};
		char aWholePath[IO_MAX_PATH_LENGTH];
		if(CImageLoader::SavePng(m_pStorage->OpenFile(m_aName, IOFLAG_WRITE, IStorage::TYPE_SAVE, aWholePath, sizeof(aWholePath)), m_aName, m_Image))
		{
			log_info_color(SCREENSHOT_LOG_COLOR, "client", "Saved screenshot to '%s'", aWholePath);
		}
		else
		{
			log_error_color(SCREENSHOT_LOG_COLOR, "client", "Failed to save screenshot to '%s'", aWholePath);
		}
	}

public:
	CScreenshotSaveJob(IStorage *pStorage, const char *pName, CImageInfo &&Image) :
		m_pStorage(pStorage),
		m_Image(std::move(Image))
	{
		str_copy(m_aName, pName);
	}

	~CScreenshotSaveJob() override
	{
		m_Image.Free();
	}
};

void CGraphics_Threaded::TextureSet(CTextureHandle TextureId)
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->TextureSet within begin");
	dbg_assert(!TextureId.IsValid() || m_TextureHandles.IsAllocated(TextureId), "Texture handle was not invalid, but also did not correlate to an existing texture.");
	m_State.m_Texture = TextureId;
}

bool CGraphics_Threaded::IsTextureLayeringSupported(ETextureLayering Layering) const
{
	return Layering == ETextureLayering::NONE ||
	       (Layering == ETextureLayering::ARRAY_2D && m_Capabilities.m_2DTextureArrays) ||
	       (Layering == ETextureLayering::VOLUME_3D && !m_Capabilities.m_2DTextureArrays && m_Capabilities.m_TextureArrays);
}

void CGraphics_Threaded::StoreTextureInfo(CTextureHandle Texture, const CTextureDesc &Desc)
{
	if(static_cast<size_t>(Texture.Id()) >= m_vTextureInfos.size())
		m_vTextureInfos.resize(Texture.Id() + 1);
	m_vTextureInfos[Texture.Id()] = {Texture, Desc};
}

void CGraphics_Threaded::Clear(float r, float g, float b, bool ForceClearNow)
{
	CCommandBuffer::SCommand_Clear Cmd;
	Cmd.m_Color.r = r;
	Cmd.m_Color.g = g;
	Cmd.m_Color.b = b;
	Cmd.m_Color.a = 0;
	Cmd.m_ForceClear = ForceClearNow;
	AddCmd(Cmd);
}

bool CGraphics_Threaded::BeginRenderPass(const CRenderPassDesc &Desc)
{
	if(m_Drawing != EDrawing::NONE)
		return false;
	CRenderPassDesc EffectiveDesc = Desc;
	if(m_OffscreenFrameTarget.IsValid() && !EffectiveDesc.m_ColorTarget.IsValid())
		EffectiveDesc.m_ColorTarget = m_OffscreenFrameTarget;
	if(EffectiveDesc.m_ColorTarget.IsValid())
	{
		if(!m_TextureHandles.IsAllocated(EffectiveDesc.m_ColorTarget) || static_cast<size_t>(EffectiveDesc.m_ColorTarget.Id()) >= m_vTextureInfos.size())
			return false;
		const STextureInfo &Info = m_vTextureInfos[EffectiveDesc.m_ColorTarget.Id()];
		if(Info.m_Handle != EffectiveDesc.m_ColorTarget || !Info.m_Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET))
			return false;
	}

	CCommandBuffer::SCommand_BeginRenderPass Cmd;
	Cmd.m_Desc = EffectiveDesc;
	if(!AddCmd(Cmd))
		return false;
	m_RenderPassActive = true;
	m_RenderPassTarget = EffectiveDesc.m_ColorTarget;
	return true;
}

bool CGraphics_Threaded::EndRenderPass()
{
	if(m_Drawing != EDrawing::NONE || !m_RenderPassActive)
		return false;
	CCommandBuffer::SCommand_EndRenderPass Cmd;
	if(!AddCmd(Cmd))
		return false;
	m_RenderPassActive = false;
	m_RenderPassTarget.Invalidate();
	return true;
}

bool CGraphics_Threaded::FlushRenderPass()
{
	if(m_Drawing != EDrawing::NONE || !m_RenderPassActive)
		return false;
	CCommandBuffer::SCommand_FlushRenderPass Cmd;
	return AddCmd(Cmd);
}

bool CGraphics_Threaded::DrawFullscreenTexture(CTextureHandle Source, CCommandBuffer::CPipelineHandle Pipeline, SGraphicsColor Color, uint8_t RequiredUsage, bool UseCurrentClip)
{
	if(m_Drawing != EDrawing::NONE || !m_RenderPassActive || !m_TextureHandles.IsAllocated(Source) || Source == m_RenderPassTarget || static_cast<size_t>(Source.Id()) >= m_vTextureInfos.size())
		return false;
	const STextureInfo &Info = m_vTextureInfos[Source.Id()];
	if(Info.m_Handle != Source || (Info.m_Desc.m_Usage & RequiredUsage) != RequiredUsage)
		return false;

	std::array<CCommandBuffer::SVertex, 4> aVertices;
	const std::array<vec2, 4> aPositions = {vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), vec2(1.0f, 1.0f), vec2(0.0f, 1.0f)};
	for(size_t i = 0; i < aVertices.size(); ++i)
	{
		aVertices[i].m_Pos = aPositions[i];
		aVertices[i].m_Tex = aPositions[i];
		aVertices[i].m_Color = Color;
	}

	CCommandBuffer::SCommand_Draw Cmd;
	Cmd.m_State = m_State;
	Cmd.m_State.m_BlendMode = EBlendMode::NONE;
	Cmd.m_State.m_WrapMode = EWrapMode::CLAMP;
	Cmd.m_State.m_Texture = Source;
	Cmd.m_State.m_ScreenTL = {0.0f, 0.0f};
	Cmd.m_State.m_ScreenBR = {1.0f, 1.0f};
	if(!UseCurrentClip)
		Cmd.m_State.m_ClipEnable = false;
	Cmd.m_Pipeline = Pipeline;
	Cmd.m_PrimitiveType = EPrimitiveType::QUADS;
	Cmd.m_VertexCount = aVertices.size();
	Cmd.m_VertexData.m_Size = sizeof(aVertices);
	Cmd.m_VertexData.m_pData = AllocCommandBufferData(Cmd.m_VertexData.m_Size);
	if(!AddCmd(Cmd, [&] {
		   Cmd.m_VertexData.m_pData = m_pCommandBuffer->AllocData(Cmd.m_VertexData.m_Size);
		   return Cmd.m_VertexData.m_pData != nullptr;
	   }))
		return false;
	mem_copy(const_cast<void *>(Cmd.m_VertexData.m_pData), aVertices.data(), sizeof(aVertices));
	return true;
}

bool CGraphics_Threaded::BlitTexture(CTextureHandle Source, bool UseCurrentClip)
{
	return DrawFullscreenTexture(Source, Pipeline(EPipelineProgram::PRIMITIVE), {255, 255, 255, 255}, TEXTURE_USAGE_SAMPLED, UseCurrentClip);
}

bool CGraphics_Threaded::BlurTexture(CTextureHandle Source, EBlurDirection Direction)
{
	SGraphicsColor Axis;
	if(Direction == EBlurDirection::HORIZONTAL)
		Axis = {255, 0, 0, 255};
	else if(Direction == EBlurDirection::VERTICAL)
		Axis = {0, 255, 0, 255};
	else
		return false;
	return DrawFullscreenTexture(Source, Pipeline(EPipelineProgram::BLUR), Axis, TEXTURE_USAGE_SAMPLED | TEXTURE_USAGE_COLOR_TARGET);
}

void CGraphics_Threaded::QuadsBegin()
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->QuadsBegin twice");
	m_Drawing = EDrawing::QUADS;

	QuadsSetSubset(0, 0, 1, 1);
	QuadsSetRotation(0);
	SetColor(1, 1, 1, 1);
}

void CGraphics_Threaded::QuadsEnd()
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsEnd without begin");
	FlushVertices();
	m_Drawing = EDrawing::NONE;
}

void CGraphics_Threaded::QuadsTex3DBegin()
{
	QuadsBegin();
}

void CGraphics_Threaded::QuadsTex3DEnd()
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsEnd without begin");
	FlushVerticesTex3D();
	m_Drawing = EDrawing::NONE;
}

void CGraphics_Threaded::TrianglesBegin()
{
	dbg_assert(m_Drawing == EDrawing::NONE, "called Graphics()->TrianglesBegin twice");
	m_Drawing = EDrawing::TRIANGLES;

	QuadsSetSubset(0, 0, 1, 1);
	QuadsSetRotation(0);
	SetColor(1, 1, 1, 1);
}

void CGraphics_Threaded::TrianglesEnd()
{
	dbg_assert(m_Drawing == EDrawing::TRIANGLES, "called Graphics()->TrianglesEnd without begin");
	FlushVertices();
	m_Drawing = EDrawing::NONE;
}

void CGraphics_Threaded::QuadsEndKeepVertices()
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsEndKeepVertices without begin");
	FlushVertices(true);
	m_Drawing = EDrawing::NONE;
}

void CGraphics_Threaded::QuadsDrawCurrentVertices(bool KeepVertices)
{
	m_Drawing = EDrawing::QUADS;
	FlushVertices(KeepVertices);
	m_Drawing = EDrawing::NONE;
}

void CGraphics_Threaded::QuadsSetRotation(float Angle)
{
	m_Rotation = Angle;
}

static unsigned char NormalizeColorComponent(float ColorComponent)
{
	return (unsigned char)(std::clamp(ColorComponent, 0.0f, 1.0f) * 255.0f + 0.5f); // +0.5f to round to nearest
}

void CGraphics_Threaded::SetColorVertex(const CColorVertex *pArray, size_t Num)
{
	dbg_assert(m_Drawing != EDrawing::NONE, "called Graphics()->SetColorVertex without begin");

	for(size_t i = 0; i < Num; ++i)
	{
		const CColorVertex &Vertex = pArray[i];
		CCommandBuffer::SColor &Color = m_aColor[Vertex.m_Index];
		Color.r = NormalizeColorComponent(Vertex.m_R);
		Color.g = NormalizeColorComponent(Vertex.m_G);
		Color.b = NormalizeColorComponent(Vertex.m_B);
		Color.a = NormalizeColorComponent(Vertex.m_A);
	}
}

void CGraphics_Threaded::SetColor(float r, float g, float b, float a)
{
	CCommandBuffer::SColor NewColor;
	NewColor.r = NormalizeColorComponent(r);
	NewColor.g = NormalizeColorComponent(g);
	NewColor.b = NormalizeColorComponent(b);
	NewColor.a = NormalizeColorComponent(a);
	std::fill(std::begin(m_aColor), std::end(m_aColor), NewColor);
}

void CGraphics_Threaded::SetColor(ColorRGBA Color)
{
	SetColor(Color.r, Color.g, Color.b, Color.a);
}

void CGraphics_Threaded::SetColor4(ColorRGBA TopLeft, ColorRGBA TopRight, ColorRGBA BottomLeft, ColorRGBA BottomRight)
{
	CColorVertex aArray[] = {
		CColorVertex(0, TopLeft),
		CColorVertex(1, TopRight),
		CColorVertex(2, BottomRight),
		CColorVertex(3, BottomLeft)};
	SetColorVertex(aArray, std::size(aArray));
}

void CGraphics_Threaded::ChangeColorOfCurrentQuadVertices(float r, float g, float b, float a)
{
	m_aColor[0].r = NormalizeColorComponent(r);
	m_aColor[0].g = NormalizeColorComponent(g);
	m_aColor[0].b = NormalizeColorComponent(b);
	m_aColor[0].a = NormalizeColorComponent(a);

	for(int i = 0; i < m_NumVertices; ++i)
	{
		SetColor(&m_aVertices[i], 0);
	}
}

void CGraphics_Threaded::ChangeColorOfQuadVertices(size_t QuadOffset, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	const CCommandBuffer::SColor Color(r, g, b, a);
	const size_t VertNum = g_Config.m_GfxQuadAsTriangle && !m_Capabilities.m_QuadToTriangleConversion ? 6 : 4;
	for(size_t i = 0; i < VertNum; ++i)
	{
		m_aVertices[QuadOffset * VertNum + i].m_Color = Color;
	}
}

void CGraphics_Threaded::QuadsSetSubset(float TlU, float TlV, float BrU, float BrV)
{
	m_aTexture[0].u = TlU;
	m_aTexture[1].u = BrU;
	m_aTexture[0].v = TlV;
	m_aTexture[1].v = TlV;

	m_aTexture[3].u = TlU;
	m_aTexture[2].u = BrU;
	m_aTexture[3].v = BrV;
	m_aTexture[2].v = BrV;
}

void CGraphics_Threaded::QuadsSetSubsetFree(
	float x0, float y0, float x1, float y1,
	float x2, float y2, float x3, float y3, int Index)
{
	m_aTexture[0].u = x0;
	m_aTexture[0].v = y0;
	m_aTexture[1].u = x1;
	m_aTexture[1].v = y1;
	m_aTexture[2].u = x2;
	m_aTexture[2].v = y2;
	m_aTexture[3].u = x3;
	m_aTexture[3].v = y3;
	m_CurIndex = Index;
}

void CGraphics_Threaded::QuadsDraw(CQuadItem *pArray, int Num)
{
	for(int i = 0; i < Num; ++i)
	{
		pArray[i].m_X -= pArray[i].m_Width / 2;
		pArray[i].m_Y -= pArray[i].m_Height / 2;
	}

	QuadsDrawTL(pArray, Num);
}

void CGraphics_Threaded::QuadsDrawTL(const CQuadItem *pArray, int Num)
{
	QuadsDrawTLImpl(m_aVertices, pArray, Num);
}

void CGraphics_Threaded::QuadsTex3DDrawTL(const CQuadItem *pArray, int Num)
{
	const int VertNum = g_Config.m_GfxQuadAsTriangle && !m_Capabilities.m_QuadToTriangleConversion ? 6 : 4;
	const CTextureHandle Texture = m_State.m_Texture;
	const bool HasTextureInfo = Texture.IsValid() && static_cast<size_t>(Texture.Id()) < m_vTextureInfos.size() && m_vTextureInfos[Texture.Id()].m_Handle == Texture;
	dbg_assert(HasTextureInfo && m_vTextureInfos[Texture.Id()].m_Desc.m_Layering != ETextureLayering::NONE, "Layered texture descriptor is missing");
	float CurIndex = m_CurIndex;
	if(HasTextureInfo && m_vTextureInfos[Texture.Id()].m_Desc.m_Layering == ETextureLayering::VOLUME_3D)
	{
		CurIndex = (m_CurIndex + 0.5f) / m_vTextureInfos[Texture.Id()].m_Desc.LayerCount();
	}

	for(int i = 0; i < Num; ++i)
	{
		for(int n = 0; n < VertNum; ++n)
		{
			m_aVerticesTex3D[m_NumVertices + VertNum * i + n].m_Tex.w = CurIndex;
		}
	}

	QuadsDrawTLImpl(m_aVerticesTex3D, pArray, Num);
}

void CGraphics_Threaded::QuadsDrawFreeform(const CFreeformItem *pArray, int Num)
{
	dbg_assert(m_Drawing == EDrawing::QUADS || m_Drawing == EDrawing::TRIANGLES, "called Graphics()->QuadsDrawFreeform without begin");

	if((g_Config.m_GfxQuadAsTriangle && !m_Capabilities.m_QuadToTriangleConversion) || m_Drawing == EDrawing::TRIANGLES)
	{
		for(int i = 0; i < Num; ++i)
		{
			m_aVertices[m_NumVertices + 6 * i].m_Pos.x = pArray[i].m_X0;
			m_aVertices[m_NumVertices + 6 * i].m_Pos.y = pArray[i].m_Y0;
			m_aVertices[m_NumVertices + 6 * i].m_Tex = m_aTexture[0];
			SetColor(&m_aVertices[m_NumVertices + 6 * i], 0);

			m_aVertices[m_NumVertices + 6 * i + 1].m_Pos.x = pArray[i].m_X1;
			m_aVertices[m_NumVertices + 6 * i + 1].m_Pos.y = pArray[i].m_Y1;
			m_aVertices[m_NumVertices + 6 * i + 1].m_Tex = m_aTexture[1];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 1], 1);

			m_aVertices[m_NumVertices + 6 * i + 2].m_Pos.x = pArray[i].m_X3;
			m_aVertices[m_NumVertices + 6 * i + 2].m_Pos.y = pArray[i].m_Y3;
			m_aVertices[m_NumVertices + 6 * i + 2].m_Tex = m_aTexture[3];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 2], 3);

			m_aVertices[m_NumVertices + 6 * i + 3].m_Pos.x = pArray[i].m_X0;
			m_aVertices[m_NumVertices + 6 * i + 3].m_Pos.y = pArray[i].m_Y0;
			m_aVertices[m_NumVertices + 6 * i + 3].m_Tex = m_aTexture[0];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 3], 0);

			m_aVertices[m_NumVertices + 6 * i + 4].m_Pos.x = pArray[i].m_X3;
			m_aVertices[m_NumVertices + 6 * i + 4].m_Pos.y = pArray[i].m_Y3;
			m_aVertices[m_NumVertices + 6 * i + 4].m_Tex = m_aTexture[3];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 4], 3);

			m_aVertices[m_NumVertices + 6 * i + 5].m_Pos.x = pArray[i].m_X2;
			m_aVertices[m_NumVertices + 6 * i + 5].m_Pos.y = pArray[i].m_Y2;
			m_aVertices[m_NumVertices + 6 * i + 5].m_Tex = m_aTexture[2];
			SetColor(&m_aVertices[m_NumVertices + 6 * i + 5], 2);
		}

		AddVertices(3 * 2 * Num);
	}
	else
	{
		for(int i = 0; i < Num; ++i)
		{
			m_aVertices[m_NumVertices + 4 * i].m_Pos.x = pArray[i].m_X0;
			m_aVertices[m_NumVertices + 4 * i].m_Pos.y = pArray[i].m_Y0;
			m_aVertices[m_NumVertices + 4 * i].m_Tex = m_aTexture[0];
			SetColor(&m_aVertices[m_NumVertices + 4 * i], 0);

			m_aVertices[m_NumVertices + 4 * i + 1].m_Pos.x = pArray[i].m_X1;
			m_aVertices[m_NumVertices + 4 * i + 1].m_Pos.y = pArray[i].m_Y1;
			m_aVertices[m_NumVertices + 4 * i + 1].m_Tex = m_aTexture[1];
			SetColor(&m_aVertices[m_NumVertices + 4 * i + 1], 1);

			m_aVertices[m_NumVertices + 4 * i + 2].m_Pos.x = pArray[i].m_X3;
			m_aVertices[m_NumVertices + 4 * i + 2].m_Pos.y = pArray[i].m_Y3;
			m_aVertices[m_NumVertices + 4 * i + 2].m_Tex = m_aTexture[3];
			SetColor(&m_aVertices[m_NumVertices + 4 * i + 2], 3);

			m_aVertices[m_NumVertices + 4 * i + 3].m_Pos.x = pArray[i].m_X2;
			m_aVertices[m_NumVertices + 4 * i + 3].m_Pos.y = pArray[i].m_Y2;
			m_aVertices[m_NumVertices + 4 * i + 3].m_Tex = m_aTexture[2];
			SetColor(&m_aVertices[m_NumVertices + 4 * i + 3], 2);
		}

		AddVertices(4 * Num);
	}
}

void CGraphics_Threaded::QuadsText(float x, float y, float Size, const char *pText)
{
	float StartX = x;

	while(*pText)
	{
		char c = *pText;
		pText++;

		if(c == '\n')
		{
			x = StartX;
			y += Size;
		}
		else
		{
			QuadsSetSubset(
				(c % 16) / 16.0f,
				(c / 16) / 16.0f,
				(c % 16) / 16.0f + 1.0f / 16.0f,
				(c / 16) / 16.0f + 1.0f / 16.0f);

			CQuadItem QuadItem(x, y, Size, Size);
			QuadsDrawTL(&QuadItem, 1);
			x += Size / 2;
		}
	}
}

void CGraphics_Threaded::DrawRectExt(float x, float y, float w, float h, float r, int Corners)
{
	const int NumSegments = 8;
	const float SegmentsAngle = pi / 2 / NumSegments;
	IGraphics::CFreeformItem aFreeform[NumSegments * 4];
	size_t NumItems = 0;

	for(int i = 0; i < NumSegments; i += 2)
	{
		float a1 = i * SegmentsAngle;
		float a2 = (i + 1) * SegmentsAngle;
		float a3 = (i + 2) * SegmentsAngle;
		float Ca1 = std::cos(a1);
		float Ca2 = std::cos(a2);
		float Ca3 = std::cos(a3);
		float Sa1 = std::sin(a1);
		float Sa2 = std::sin(a2);
		float Sa3 = std::sin(a3);

		if(Corners & CORNER_TL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + r,
				x + (1 - Ca1) * r, y + (1 - Sa1) * r,
				x + (1 - Ca3) * r, y + (1 - Sa3) * r,
				x + (1 - Ca2) * r, y + (1 - Sa2) * r);

		if(Corners & CORNER_TR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + r,
				x + w - r + Ca1 * r, y + (1 - Sa1) * r,
				x + w - r + Ca3 * r, y + (1 - Sa3) * r,
				x + w - r + Ca2 * r, y + (1 - Sa2) * r);

		if(Corners & CORNER_BL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + h - r,
				x + (1 - Ca1) * r, y + h - r + Sa1 * r,
				x + (1 - Ca3) * r, y + h - r + Sa3 * r,
				x + (1 - Ca2) * r, y + h - r + Sa2 * r);

		if(Corners & CORNER_BR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + h - r,
				x + w - r + Ca1 * r, y + h - r + Sa1 * r,
				x + w - r + Ca3 * r, y + h - r + Sa3 * r,
				x + w - r + Ca2 * r, y + h - r + Sa2 * r);
	}
	QuadsDrawFreeform(aFreeform, NumItems);

	CQuadItem aQuads[9];
	NumItems = 0;
	aQuads[NumItems++] = CQuadItem(x + r, y + r, w - r * 2, h - r * 2); // center
	aQuads[NumItems++] = CQuadItem(x + r, y, w - r * 2, r); // top
	aQuads[NumItems++] = CQuadItem(x + r, y + h - r, w - r * 2, r); // bottom
	aQuads[NumItems++] = CQuadItem(x, y + r, r, h - r * 2); // left
	aQuads[NumItems++] = CQuadItem(x + w - r, y + r, r, h - r * 2); // right

	if(!(Corners & CORNER_TL))
		aQuads[NumItems++] = CQuadItem(x, y, r, r);
	if(!(Corners & CORNER_TR))
		aQuads[NumItems++] = CQuadItem(x + w, y, -r, r);
	if(!(Corners & CORNER_BL))
		aQuads[NumItems++] = CQuadItem(x, y + h, r, -r);
	if(!(Corners & CORNER_BR))
		aQuads[NumItems++] = CQuadItem(x + w, y + h, -r, -r);

	QuadsDrawTL(aQuads, NumItems);
}

void CGraphics_Threaded::DrawRectExt4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, float r, int Corners)
{
	if(Corners == 0 || r == 0.0f)
	{
		SetColor4(ColorTopLeft, ColorTopRight, ColorBottomLeft, ColorBottomRight);
		CQuadItem ItemQ = CQuadItem(x, y, w, h);
		QuadsDrawTL(&ItemQ, 1);
		return;
	}

	const int NumSegments = 8;
	const float SegmentsAngle = pi / 2 / NumSegments;
	for(int i = 0; i < NumSegments; i += 2)
	{
		float a1 = i * SegmentsAngle;
		float a2 = (i + 1) * SegmentsAngle;
		float a3 = (i + 2) * SegmentsAngle;
		float Ca1 = std::cos(a1);
		float Ca2 = std::cos(a2);
		float Ca3 = std::cos(a3);
		float Sa1 = std::sin(a1);
		float Sa2 = std::sin(a2);
		float Sa3 = std::sin(a3);

		if(Corners & CORNER_TL)
		{
			SetColor(ColorTopLeft);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + r, y + r,
				x + (1 - Ca1) * r, y + (1 - Sa1) * r,
				x + (1 - Ca3) * r, y + (1 - Sa3) * r,
				x + (1 - Ca2) * r, y + (1 - Sa2) * r);
			QuadsDrawFreeform(&ItemF, 1);
		}

		if(Corners & CORNER_TR)
		{
			SetColor(ColorTopRight);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + w - r, y + r,
				x + w - r + Ca1 * r, y + (1 - Sa1) * r,
				x + w - r + Ca3 * r, y + (1 - Sa3) * r,
				x + w - r + Ca2 * r, y + (1 - Sa2) * r);
			QuadsDrawFreeform(&ItemF, 1);
		}

		if(Corners & CORNER_BL)
		{
			SetColor(ColorBottomLeft);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + r, y + h - r,
				x + (1 - Ca1) * r, y + h - r + Sa1 * r,
				x + (1 - Ca3) * r, y + h - r + Sa3 * r,
				x + (1 - Ca2) * r, y + h - r + Sa2 * r);
			QuadsDrawFreeform(&ItemF, 1);
		}

		if(Corners & CORNER_BR)
		{
			SetColor(ColorBottomRight);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + w - r, y + h - r,
				x + w - r + Ca1 * r, y + h - r + Sa1 * r,
				x + w - r + Ca3 * r, y + h - r + Sa3 * r,
				x + w - r + Ca2 * r, y + h - r + Sa2 * r);
			QuadsDrawFreeform(&ItemF, 1);
		}
	}

	SetColor4(ColorTopLeft, ColorTopRight, ColorBottomLeft, ColorBottomRight);
	CQuadItem ItemQ = CQuadItem(x + r, y + r, w - r * 2, h - r * 2); // center
	QuadsDrawTL(&ItemQ, 1);

	SetColor4(ColorTopLeft, ColorTopRight, ColorTopLeft, ColorTopRight);
	ItemQ = CQuadItem(x + r, y, w - r * 2, r); // top
	QuadsDrawTL(&ItemQ, 1);

	SetColor4(ColorBottomLeft, ColorBottomRight, ColorBottomLeft, ColorBottomRight);
	ItemQ = CQuadItem(x + r, y + h - r, w - r * 2, r); // bottom
	QuadsDrawTL(&ItemQ, 1);

	SetColor4(ColorTopLeft, ColorTopLeft, ColorBottomLeft, ColorBottomLeft);
	ItemQ = CQuadItem(x, y + r, r, h - r * 2); // left
	QuadsDrawTL(&ItemQ, 1);

	SetColor4(ColorTopRight, ColorTopRight, ColorBottomRight, ColorBottomRight);
	ItemQ = CQuadItem(x + w - r, y + r, r, h - r * 2); // right
	QuadsDrawTL(&ItemQ, 1);

	if(!(Corners & CORNER_TL))
	{
		SetColor(ColorTopLeft);
		ItemQ = CQuadItem(x, y, r, r);
		QuadsDrawTL(&ItemQ, 1);
	}

	if(!(Corners & CORNER_TR))
	{
		SetColor(ColorTopRight);
		ItemQ = CQuadItem(x + w, y, -r, r);
		QuadsDrawTL(&ItemQ, 1);
	}

	if(!(Corners & CORNER_BL))
	{
		SetColor(ColorBottomLeft);
		ItemQ = CQuadItem(x, y + h, r, -r);
		QuadsDrawTL(&ItemQ, 1);
	}

	if(!(Corners & CORNER_BR))
	{
		SetColor(ColorBottomRight);
		ItemQ = CQuadItem(x + w, y + h, -r, -r);
		QuadsDrawTL(&ItemQ, 1);
	}
}

int CGraphics_Threaded::CreateRectQuadContainer(float x, float y, float w, float h, float r, int Corners)
{
	int ContainerIndex = CreateQuadContainer(false);

	if(Corners == 0 || r == 0.0f)
	{
		CQuadItem ItemQ = CQuadItem(x, y, w, h);
		QuadContainerAddQuads(ContainerIndex, &ItemQ, 1);
		QuadContainerUpload(ContainerIndex);
		QuadContainerChangeAutomaticUpload(ContainerIndex, true);
		return ContainerIndex;
	}

	const int NumSegments = 8;
	const float SegmentsAngle = pi / 2 / NumSegments;
	IGraphics::CFreeformItem aFreeform[NumSegments * 4];
	size_t NumItems = 0;

	for(int i = 0; i < NumSegments; i += 2)
	{
		float a1 = i * SegmentsAngle;
		float a2 = (i + 1) * SegmentsAngle;
		float a3 = (i + 2) * SegmentsAngle;
		float Ca1 = std::cos(a1);
		float Ca2 = std::cos(a2);
		float Ca3 = std::cos(a3);
		float Sa1 = std::sin(a1);
		float Sa2 = std::sin(a2);
		float Sa3 = std::sin(a3);

		if(Corners & CORNER_TL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + r,
				x + (1 - Ca1) * r, y + (1 - Sa1) * r,
				x + (1 - Ca3) * r, y + (1 - Sa3) * r,
				x + (1 - Ca2) * r, y + (1 - Sa2) * r);

		if(Corners & CORNER_TR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + r,
				x + w - r + Ca1 * r, y + (1 - Sa1) * r,
				x + w - r + Ca3 * r, y + (1 - Sa3) * r,
				x + w - r + Ca2 * r, y + (1 - Sa2) * r);

		if(Corners & CORNER_BL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + h - r,
				x + (1 - Ca1) * r, y + h - r + Sa1 * r,
				x + (1 - Ca3) * r, y + h - r + Sa3 * r,
				x + (1 - Ca2) * r, y + h - r + Sa2 * r);

		if(Corners & CORNER_BR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + h - r,
				x + w - r + Ca1 * r, y + h - r + Sa1 * r,
				x + w - r + Ca3 * r, y + h - r + Sa3 * r,
				x + w - r + Ca2 * r, y + h - r + Sa2 * r);
	}

	if(NumItems > 0)
		QuadContainerAddQuads(ContainerIndex, aFreeform, NumItems);

	CQuadItem aQuads[9];
	NumItems = 0;
	aQuads[NumItems++] = CQuadItem(x + r, y + r, w - r * 2, h - r * 2); // center
	aQuads[NumItems++] = CQuadItem(x + r, y, w - r * 2, r); // top
	aQuads[NumItems++] = CQuadItem(x + r, y + h - r, w - r * 2, r); // bottom
	aQuads[NumItems++] = CQuadItem(x, y + r, r, h - r * 2); // left
	aQuads[NumItems++] = CQuadItem(x + w - r, y + r, r, h - r * 2); // right

	if(!(Corners & CORNER_TL))
		aQuads[NumItems++] = CQuadItem(x, y, r, r);
	if(!(Corners & CORNER_TR))
		aQuads[NumItems++] = CQuadItem(x + w, y, -r, r);
	if(!(Corners & CORNER_BL))
		aQuads[NumItems++] = CQuadItem(x, y + h, r, -r);
	if(!(Corners & CORNER_BR))
		aQuads[NumItems++] = CQuadItem(x + w, y + h, -r, -r);

	if(NumItems > 0)
		QuadContainerAddQuads(ContainerIndex, aQuads, NumItems);

	QuadContainerUpload(ContainerIndex);
	QuadContainerChangeAutomaticUpload(ContainerIndex, true);

	return ContainerIndex;
}

void CGraphics_Threaded::DrawRect(float x, float y, float w, float h, ColorRGBA Color, int Corners, float Rounding)
{
	TextureClear();
	QuadsBegin();
	SetColor(Color);
	DrawRectExt(x, y, w, h, Rounding, Corners);
	QuadsEnd();
}

void CGraphics_Threaded::DrawRect4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, int Corners, float Rounding)
{
	TextureClear();
	QuadsBegin();
	DrawRectExt4(x, y, w, h, ColorTopLeft, ColorTopRight, ColorBottomLeft, ColorBottomRight, Rounding, Corners);
	QuadsEnd();
}

void CGraphics_Threaded::DrawCircle(float CenterX, float CenterY, float Radius, int Segments)
{
	IGraphics::CFreeformItem aItems[32];
	size_t NumItems = 0;
	const float SegmentsAngle = 2 * pi / Segments;
	for(int i = 0; i < Segments; i += 2)
	{
		const float a1 = i * SegmentsAngle;
		const float a2 = (i + 1) * SegmentsAngle;
		const float a3 = (i + 2) * SegmentsAngle;
		aItems[NumItems++] = IGraphics::CFreeformItem(
			CenterX, CenterY,
			CenterX + std::cos(a1) * Radius, CenterY + std::sin(a1) * Radius,
			CenterX + std::cos(a3) * Radius, CenterY + std::sin(a3) * Radius,
			CenterX + std::cos(a2) * Radius, CenterY + std::sin(a2) * Radius);
		if(NumItems == std::size(aItems))
		{
			QuadsDrawFreeform(aItems, std::size(aItems));
			NumItems = 0;
		}
	}
	if(NumItems)
		QuadsDrawFreeform(aItems, NumItems);
}

void CGraphics_Threaded::RenderTileLayer(CBufferContainerHandle BufferContainer, const ColorRGBA &Color, char **pOffsets, unsigned int *pIndicedVertexDrawNum, size_t NumIndicesOffset)
{
	if(NumIndicesOffset == 0)
		return;
	dbg_assert(m_BufferContainerHandles.IsAllocated(BufferContainer), "Cannot render with stale buffer container handle");

	// ponytail: one command per visible span; add a generic multi-draw range only
	// if real maps exceed the fixed command arena.
	for(size_t i = 0; i < NumIndicesOffset; ++i)
	{
		if(pIndicedVertexDrawNum[i] == 0)
			continue;
		CCommandBuffer::SCommand_DrawIndexed Cmd;
		Cmd.m_State = m_State;
		Cmd.m_Pipeline = Pipeline(EPipelineProgram::ARRAY_COLOR);
		Cmd.m_IndexCount = pIndicedVertexDrawNum[i];
		Cmd.m_IndexOffset = reinterpret_cast<uintptr_t>(pOffsets[i]);
		Cmd.m_BufferContainer = BufferContainer;
		Cmd.m_IndexBuffer = m_QuadIndexBuffer;
		Cmd.m_DrawData.m_Size = sizeof(CCommandBuffer::SDrawDataArrayColor);
		auto AllocatePayload = [&] {
			auto *pData = static_cast<CCommandBuffer::SDrawDataArrayColor *>(m_pCommandBuffer->AllocData(sizeof(CCommandBuffer::SDrawDataArrayColor)));
			if(pData == nullptr)
				return false;
			pData->m_Color = Color;
			Cmd.m_DrawData.m_pData = pData;
			return true;
		};
		if(!AllocatePayload())
		{
			DropCurrentFrame();
			if(!AllocatePayload())
				return;
		}
		if(!AddCmd(Cmd, AllocatePayload))
			return;
	}
}

void CGraphics_Threaded::RenderBorderTiles(CBufferContainerHandle BufferContainer, const ColorRGBA &Color, char *pIndexBufferOffset, const vec2 &Offset, const vec2 &Scale, uint32_t DrawNum)
{
	if(DrawNum == 0)
		return;
	dbg_assert(m_BufferContainerHandles.IsAllocated(BufferContainer), "Cannot render with stale buffer container handle");
	if(DrawNum > std::numeric_limits<uint32_t>::max() / 6)
	{
		log_error("graphics", "Invalid border tile draw count. DrawCount=%u", DrawNum);
		return;
	}
	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = m_State;
	Cmd.m_Pipeline = Pipeline(EPipelineProgram::ARRAY_COLOR_TRANSFORM);
	Cmd.m_IndexCount = DrawNum * 6;
	Cmd.m_IndexOffset = reinterpret_cast<uintptr_t>(pIndexBufferOffset);
	Cmd.m_BufferContainer = BufferContainer;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;
	Cmd.m_DrawData.m_Size = sizeof(CCommandBuffer::SDrawDataArrayColorTransform);
	auto AllocatePayload = [&] {
		auto *pData = static_cast<CCommandBuffer::SDrawDataArrayColorTransform *>(m_pCommandBuffer->AllocData(sizeof(CCommandBuffer::SDrawDataArrayColorTransform)));
		if(pData == nullptr)
			return false;
		pData->m_Color = Color;
		pData->m_Offset = Offset;
		pData->m_Scale = Scale;
		Cmd.m_DrawData.m_pData = pData;
		return true;
	};
	if(!AllocatePayload())
	{
		DropCurrentFrame();
		if(!AllocatePayload())
			return;
	}
	if(!AddCmd(Cmd, AllocatePayload))
		return;
}

void CGraphics_Threaded::RenderQuadLayer(CBufferContainerHandle BufferContainer, SQuadRenderInfo *pQuadInfo, size_t QuadNum, int QuadOffset, bool Grouped)
{
	if(QuadNum == 0)
		return;
	dbg_assert(m_BufferContainerHandles.IsAllocated(BufferContainer), "Cannot render with stale buffer container handle");
	if(QuadOffset < 0 || QuadNum > std::numeric_limits<uint32_t>::max() / 6)
	{
		log_error("graphics", "Invalid quad layer draw range. QuadCount=%" PRIzu " QuadOffset=%d", QuadNum, QuadOffset);
		return;
	}

	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = m_State;
	Cmd.m_Pipeline = Pipeline(Grouped ? EPipelineProgram::QUAD_SHARED : EPipelineProgram::QUAD_PER_ITEM);
	Cmd.m_IndexCount = static_cast<uint32_t>(QuadNum * 6);
	Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
	Cmd.m_BufferContainer = BufferContainer;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;

	const size_t DataCount = Grouped ? 1 : QuadNum;
	const size_t DataSize = DataCount * sizeof(CCommandBuffer::SDrawDataQuadTransform);
	if(Grouped)
		Cmd.m_DrawData.m_Size = DataSize;
	else
		Cmd.m_ArrayData.m_Size = DataSize;

	auto AllocatePayload = [&] {
		auto *pData = static_cast<CCommandBuffer::SDrawDataQuadTransform *>(m_pCommandBuffer->AllocData(DataSize));
		if(pData == nullptr)
			return false;
		for(size_t i = 0; i < DataCount; ++i)
		{
			pData[i].m_Color = pQuadInfo[i].m_Color;
			pData[i].m_Offset = pQuadInfo[i].m_Offsets;
			pData[i].m_Rotation = pQuadInfo[i].m_Rotation;
			pData[i].m_Padding = 0.0f;
		}
		if(Grouped)
			Cmd.m_DrawData.m_pData = pData;
		else
			Cmd.m_ArrayData.m_pData = pData;
		return true;
	};

	if(!AllocatePayload())
	{
		DropCurrentFrame();
		if(!AllocatePayload())
		{
			log_error("graphics", "Failed to allocate quad layer data. QuadCount=%" PRIzu, QuadNum);
			return;
		}
	}

	if(!AddCmd(Cmd, AllocatePayload))
		return;
}

void CGraphics_Threaded::RenderText(CBufferContainerHandle BufferContainer, int TextQuadNum, int TextureSize, CTextureHandle TextTexture, CTextureHandle TextOutlineTexture, const ColorRGBA &TextColor, const ColorRGBA &TextOutlineColor)
{
	if(!BufferContainer.IsValid() || TextQuadNum <= 0)
		return;
	dbg_assert(m_BufferContainerHandles.IsAllocated(BufferContainer), "Cannot render with stale buffer container handle");
	dbg_assert(m_TextureHandles.IsAllocated(TextTexture), "Cannot render text with stale texture handle");
	dbg_assert(m_TextureHandles.IsAllocated(TextOutlineTexture), "Cannot render text with stale outline texture handle");
	if(TextureSize <= 0 || static_cast<uint32_t>(TextQuadNum) > std::numeric_limits<uint32_t>::max() / 6)
	{
		log_error("graphics", "Invalid text draw. QuadCount=%d TextureSize=%d", TextQuadNum, TextureSize);
		return;
	}

	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = m_State;
	Cmd.m_State.m_Texture.Invalidate();
	Cmd.m_Pipeline = Pipeline(EPipelineProgram::DUAL_ATLAS_COMPOSITE);
	Cmd.m_BufferContainer = BufferContainer;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;
	Cmd.m_TextureBinding = FindTextureBinding(TextTexture, TextOutlineTexture);
	if(!Cmd.m_TextureBinding.IsValid())
		return;
	Cmd.m_IndexCount = static_cast<uint32_t>(TextQuadNum) * 6;
	Cmd.m_IndexOffset = 0;
	Cmd.m_DrawData.m_Size = sizeof(CCommandBuffer::SDrawDataDualAtlas);
	auto AllocatePayload = [&] {
		auto *pData = static_cast<CCommandBuffer::SDrawDataDualAtlas *>(m_pCommandBuffer->AllocData(sizeof(CCommandBuffer::SDrawDataDualAtlas)));
		if(pData == nullptr)
			return false;
		pData->m_TextureSize = TextureSize;
		pData->m_PrimaryColor = TextColor;
		pData->m_SecondaryColor = TextOutlineColor;
		Cmd.m_DrawData.m_pData = pData;
		return true;
	};
	if(!AllocatePayload())
	{
		DropCurrentFrame();
		if(!AllocatePayload())
			return;
	}
	if(!AddCmd(Cmd, AllocatePayload))
		return;
}

bool CGraphics_Threaded::RenderTransientIndexed(const SGraphicsVertex *pVertices, uint32_t VertexCount, const void *pIndices, uint32_t IndexCount, EIndexType IndexType, const CTransientIndexedDrawRange *pRanges, uint32_t RangeCount)
{
	if(RangeCount == 0)
		return true;
	if(pRanges == nullptr || m_Drawing != EDrawing::NONE || ScreenWidth() <= 0 || ScreenHeight() <= 0)
		return false;

	size_t IndexElementSize;
	switch(IndexType)
	{
	case EIndexType::UINT16: IndexElementSize = sizeof(uint16_t); break;
	case EIndexType::UINT32: IndexElementSize = sizeof(uint32_t); break;
	default: return false;
	}

	auto ClampedClip = [this](const CTransientIndexedDrawRange &Range, int &x, int &y, int &w, int &h) {
		if(Range.m_ClipW <= 0 || Range.m_ClipH <= 0)
			return false;
		const int64_t Left = std::clamp<int64_t>(Range.m_ClipX, 0, ScreenWidth());
		const int64_t Top = std::clamp<int64_t>(Range.m_ClipY, 0, ScreenHeight());
		const int64_t Right = std::clamp<int64_t>(static_cast<int64_t>(Range.m_ClipX) + Range.m_ClipW, 0, ScreenWidth());
		const int64_t Bottom = std::clamp<int64_t>(static_cast<int64_t>(Range.m_ClipY) + Range.m_ClipH, 0, ScreenHeight());
		if(Right <= Left || Bottom <= Top)
			return false;
		x = static_cast<int>(Left);
		y = ScreenHeight() - static_cast<int>(Bottom);
		w = static_cast<int>(Right - Left);
		h = static_cast<int>(Bottom - Top);
		return true;
	};

	uint32_t ValidRangeCount = 0;
	for(uint32_t RangeIndex = 0; RangeIndex < RangeCount; ++RangeIndex)
	{
		const auto &Range = pRanges[RangeIndex];
		int ClipX, ClipY, ClipW, ClipH;
		if(Range.m_IndexCount == 0 || !ClampedClip(Range, ClipX, ClipY, ClipW, ClipH))
			continue;
		if(Range.m_FirstIndex > IndexCount || Range.m_IndexCount > IndexCount - Range.m_FirstIndex || Range.m_VertexOffset > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) || Range.m_IndexCount > static_cast<uint32_t>(std::numeric_limits<int>::max()))
			return false;
		if(Range.m_Texture.IsValid() && !m_TextureHandles.IsAllocated(Range.m_Texture))
			return false;
		++ValidRangeCount;
	}
	if(ValidRangeCount == 0)
		return true;
	if(pVertices == nullptr || pIndices == nullptr || VertexCount == 0 || IndexCount == 0)
		return false;

	size_t VertexDataSize;
	size_t IndexDataSize;
	size_t RangeDataSize;
	if(!CheckedMul(VertexCount, sizeof(SGraphicsVertex), VertexDataSize) || !CheckedMul(IndexCount, IndexElementSize, IndexDataSize) || !CheckedMul(ValidRangeCount, sizeof(CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange), RangeDataSize))
		return false;
	size_t IndexDataOffset;
	size_t RangeDataOffset;
	size_t TotalDataSize;
	if(!CheckedAlign(VertexDataSize, alignof(uint32_t), IndexDataOffset) || !CheckedAdd(IndexDataOffset, IndexDataSize, RangeDataOffset) || !CheckedAlign(RangeDataOffset, alignof(CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange), RangeDataOffset) || !CheckedAdd(RangeDataOffset, RangeDataSize, TotalDataSize))
		return false;
	if(TotalDataSize > CMD_BUFFER_DATA_BUFFER_SIZE)
		return false;

	for(uint32_t RangeIndex = 0; RangeIndex < RangeCount; ++RangeIndex)
	{
		const auto &Range = pRanges[RangeIndex];
		int ClipX, ClipY, ClipW, ClipH;
		if(Range.m_IndexCount == 0 || !ClampedClip(Range, ClipX, ClipY, ClipW, ClipH))
			continue;
		for(uint32_t Index = 0; Index < Range.m_IndexCount; ++Index)
		{
			uint32_t VertexIndex;
			const size_t ByteOffset = static_cast<size_t>(Range.m_FirstIndex + Index) * IndexElementSize;
			if(IndexType == EIndexType::UINT16)
			{
				uint16_t ShortIndex;
				mem_copy(&ShortIndex, static_cast<const uint8_t *>(pIndices) + ByteOffset, sizeof(ShortIndex));
				VertexIndex = ShortIndex;
			}
			else
			{
				mem_copy(&VertexIndex, static_cast<const uint8_t *>(pIndices) + ByteOffset, sizeof(VertexIndex));
			}
			if(static_cast<uint64_t>(Range.m_VertexOffset) + VertexIndex >= VertexCount)
				return false;
		}
	}

	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = {};
	Cmd.m_Pipeline = Pipeline(EPipelineProgram::PRIMITIVE);
	Cmd.m_IndexType = IndexType;
	Cmd.m_VertexCount = VertexCount;
	Cmd.m_IndexCount = IndexCount;
	Cmd.m_RangeCount = ValidRangeCount;
	auto AllocatePayload = [&] {
		auto *pData = static_cast<uint8_t *>(m_pCommandBuffer->AllocData(TotalDataSize));
		if(pData == nullptr)
			return false;
		Cmd.m_VertexData = {pData, VertexDataSize};
		Cmd.m_IndexData = {pData + IndexDataOffset, IndexDataSize};
		Cmd.m_RangeData = {pData + RangeDataOffset, RangeDataSize};
		mem_copy(pData, pVertices, VertexDataSize);
		mem_copy(pData + IndexDataOffset, pIndices, IndexDataSize);
		auto *pStoredRanges = static_cast<CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange *>(const_cast<void *>(Cmd.m_RangeData.m_pData));
		uint32_t StoredRangeIndex = 0;
		for(uint32_t RangeIndex = 0; RangeIndex < RangeCount; ++RangeIndex)
		{
			const auto &Range = pRanges[RangeIndex];
			int ClipX, ClipY, ClipW, ClipH;
			if(Range.m_IndexCount == 0 || !ClampedClip(Range, ClipX, ClipY, ClipW, ClipH))
				continue;
			auto &StoredRange = pStoredRanges[StoredRangeIndex++];
			StoredRange.m_State = m_State;
			StoredRange.m_State.m_Texture = Range.m_Texture;
			StoredRange.m_State.m_BlendMode = EBlendMode::ALPHA;
			StoredRange.m_State.m_WrapMode = EWrapMode::CLAMP;
			StoredRange.m_State.m_ClipEnable = true;
			StoredRange.m_State.m_ClipX = ClipX;
			StoredRange.m_State.m_ClipY = ClipY;
			StoredRange.m_State.m_ClipW = ClipW;
			StoredRange.m_State.m_ClipH = ClipH;
			StoredRange.m_FirstIndex = Range.m_FirstIndex;
			StoredRange.m_IndexCount = Range.m_IndexCount;
			StoredRange.m_VertexOffset = Range.m_VertexOffset;
		}
		return true;
	};
	if(!AllocatePayload())
	{
		DropCurrentFrame();
		if(!AllocatePayload())
			return false;
	}
	if(!AddCmd(Cmd, AllocatePayload))
		return false;
	return true;
}

int CGraphics_Threaded::CreateQuadContainer(bool AutomaticUpload)
{
	int Index = -1;
	if(m_FirstFreeQuadContainer == -1)
	{
		Index = m_vQuadContainers.size();
		m_vQuadContainers.emplace_back(AutomaticUpload);
	}
	else
	{
		Index = m_FirstFreeQuadContainer;
		m_FirstFreeQuadContainer = m_vQuadContainers[Index].m_FreeIndex;
		m_vQuadContainers[Index].m_FreeIndex = Index;
	}

	return Index;
}

void CGraphics_Threaded::QuadContainerChangeAutomaticUpload(int ContainerIndex, bool AutomaticUpload)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];
	Container.m_AutomaticUpload = AutomaticUpload;
}

void CGraphics_Threaded::QuadContainerUpload(int ContainerIndex)
{
	if(IsQuadContainerBufferingEnabled())
	{
		SQuadContainer &Container = m_vQuadContainers[ContainerIndex];
		if(!Container.m_vQuads.empty())
		{
			bool BufferReady;
			if(!Container.m_QuadBuffer.IsValid())
			{
				size_t UploadDataSize = Container.m_vQuads.size() * sizeof(SQuadContainer::SQuad);
				Container.m_QuadBuffer = CreateBufferObject(UploadDataSize, Container.m_vQuads.data(), 0);
				BufferReady = Container.m_QuadBuffer.IsValid();
			}
			else
			{
				size_t UploadDataSize = Container.m_vQuads.size() * sizeof(SQuadContainer::SQuad);
				BufferReady = RecreateBufferObject(Container.m_QuadBuffer, UploadDataSize, Container.m_vQuads.data(), 0);
			}
			if(!BufferReady)
				return;
			Container.m_UploadedQuadCount = Container.m_vQuads.size();

			if(!Container.m_QuadBufferContainer.IsValid())
			{
				SBufferContainerInfo Info;
				Info.m_Stride = sizeof(CCommandBuffer::SVertex);
				Info.m_VertBufferBinding = Container.m_QuadBuffer;

				Info.m_vAttributes.emplace_back();
				SBufferContainerInfo::SAttribute *pAttr = &Info.m_vAttributes.back();
				pAttr->m_ComponentCount = 2;
				pAttr->m_Mode = IGraphics::EVertexAttributeMode::FLOAT;
				pAttr->m_Normalized = false;
				pAttr->m_Offset = 0;
				pAttr->m_Type = IGraphics::EVertexAttributeType::FLOAT32;
				Info.m_vAttributes.emplace_back();
				pAttr = &Info.m_vAttributes.back();
				pAttr->m_ComponentCount = 2;
				pAttr->m_Mode = IGraphics::EVertexAttributeMode::FLOAT;
				pAttr->m_Normalized = false;
				pAttr->m_Offset = sizeof(float) * 2;
				pAttr->m_Type = IGraphics::EVertexAttributeType::FLOAT32;
				Info.m_vAttributes.emplace_back();
				pAttr = &Info.m_vAttributes.back();
				pAttr->m_ComponentCount = 4;
				pAttr->m_Mode = IGraphics::EVertexAttributeMode::FLOAT;
				pAttr->m_Normalized = true;
				pAttr->m_Offset = sizeof(float) * 2 + sizeof(float) * 2;
				pAttr->m_Type = IGraphics::EVertexAttributeType::UINT8;

				Container.m_QuadBufferContainer = CreateBufferContainer(&Info);
			}
		}
	}
}

int CGraphics_Threaded::QuadContainerAddQuads(int ContainerIndex, CQuadItem *pArray, int Num)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if((int)Container.m_vQuads.size() > Num + CCommandBuffer::CCommandBuffer::MAX_VERTICES)
		return -1;

	int RetOff = (int)Container.m_vQuads.size();

	for(int i = 0; i < Num; ++i)
	{
		Container.m_vQuads.emplace_back();
		SQuadContainer::SQuad &Quad = Container.m_vQuads.back();

		Quad.m_aVertices[0].m_Pos.x = pArray[i].m_X;
		Quad.m_aVertices[0].m_Pos.y = pArray[i].m_Y;
		Quad.m_aVertices[0].m_Tex = m_aTexture[0];
		SetColor(&Quad.m_aVertices[0], 0);

		Quad.m_aVertices[1].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
		Quad.m_aVertices[1].m_Pos.y = pArray[i].m_Y;
		Quad.m_aVertices[1].m_Tex = m_aTexture[1];
		SetColor(&Quad.m_aVertices[1], 1);

		Quad.m_aVertices[2].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
		Quad.m_aVertices[2].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
		Quad.m_aVertices[2].m_Tex = m_aTexture[2];
		SetColor(&Quad.m_aVertices[2], 2);

		Quad.m_aVertices[3].m_Pos.x = pArray[i].m_X;
		Quad.m_aVertices[3].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
		Quad.m_aVertices[3].m_Tex = m_aTexture[3];
		SetColor(&Quad.m_aVertices[3], 3);

		if(m_Rotation != 0)
		{
			CCommandBuffer::SPoint Center;
			Center.x = pArray[i].m_X + pArray[i].m_Width / 2;
			Center.y = pArray[i].m_Y + pArray[i].m_Height / 2;

			Rotate(Center, Quad.m_aVertices, 4);
		}
	}

	if(Container.m_AutomaticUpload)
		QuadContainerUpload(ContainerIndex);

	return RetOff;
}

int CGraphics_Threaded::QuadContainerAddQuads(int ContainerIndex, CFreeformItem *pArray, int Num)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if((int)Container.m_vQuads.size() > Num + CCommandBuffer::CCommandBuffer::MAX_VERTICES)
		return -1;

	int RetOff = (int)Container.m_vQuads.size();

	for(int i = 0; i < Num; ++i)
	{
		Container.m_vQuads.emplace_back();
		SQuadContainer::SQuad &Quad = Container.m_vQuads.back();

		Quad.m_aVertices[0].m_Pos.x = pArray[i].m_X0;
		Quad.m_aVertices[0].m_Pos.y = pArray[i].m_Y0;
		Quad.m_aVertices[0].m_Tex = m_aTexture[0];
		SetColor(&Quad.m_aVertices[0], 0);

		Quad.m_aVertices[1].m_Pos.x = pArray[i].m_X1;
		Quad.m_aVertices[1].m_Pos.y = pArray[i].m_Y1;
		Quad.m_aVertices[1].m_Tex = m_aTexture[1];
		SetColor(&Quad.m_aVertices[1], 1);

		Quad.m_aVertices[2].m_Pos.x = pArray[i].m_X3;
		Quad.m_aVertices[2].m_Pos.y = pArray[i].m_Y3;
		Quad.m_aVertices[2].m_Tex = m_aTexture[3];
		SetColor(&Quad.m_aVertices[2], 3);

		Quad.m_aVertices[3].m_Pos.x = pArray[i].m_X2;
		Quad.m_aVertices[3].m_Pos.y = pArray[i].m_Y2;
		Quad.m_aVertices[3].m_Tex = m_aTexture[2];
		SetColor(&Quad.m_aVertices[3], 2);
	}

	if(Container.m_AutomaticUpload)
		QuadContainerUpload(ContainerIndex);

	return RetOff;
}

void CGraphics_Threaded::QuadContainerReset(int ContainerIndex)
{
	if(ContainerIndex == -1)
		return;

	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];
	if(IsQuadContainerBufferingEnabled())
	{
		DeleteBufferContainer(Container.m_QuadBufferContainer, true);
		if(Container.m_QuadBufferContainer.IsValid())
			return;
	}
	Container.m_vQuads.clear();
	Container.m_QuadBuffer.Invalidate();
	Container.m_UploadedQuadCount = 0;
}

void CGraphics_Threaded::DeleteQuadContainer(int &ContainerIndex)
{
	if(ContainerIndex == -1)
		return;

	QuadContainerReset(ContainerIndex);
	if(m_vQuadContainers[ContainerIndex].m_QuadBufferContainer.IsValid())
		return;

	// also clear the container index
	m_vQuadContainers[ContainerIndex].m_FreeIndex = m_FirstFreeQuadContainer;
	m_FirstFreeQuadContainer = ContainerIndex;
	ContainerIndex = -1;
}

void CGraphics_Threaded::RenderQuadContainer(int ContainerIndex, int QuadDrawNum)
{
	RenderQuadContainer(ContainerIndex, 0, QuadDrawNum);
}

void CGraphics_Threaded::RenderQuadContainer(int ContainerIndex, int QuadOffset, int QuadDrawNum, bool ChangeWrapMode)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if(QuadDrawNum == -1)
		QuadDrawNum = (int)(IsQuadContainerBufferingEnabled() ? Container.m_UploadedQuadCount : Container.m_vQuads.size()) - QuadOffset;

	if((int)Container.m_vQuads.size() < QuadOffset + QuadDrawNum || QuadDrawNum == 0)
		return;

	if(IsQuadContainerBufferingEnabled())
	{
		if(!Container.m_QuadBufferContainer.IsValid() || Container.m_UploadedQuadCount < static_cast<size_t>(QuadOffset) + static_cast<size_t>(QuadDrawNum))
			return;

		if(ChangeWrapMode)
			WrapClamp();

		CCommandBuffer::SCommand_DrawIndexed Cmd;
		Cmd.m_State = m_State;
		Cmd.m_Pipeline = Pipeline(EPipelineProgram::PRIMITIVE);
		Cmd.m_IndexCount = static_cast<uint32_t>(QuadDrawNum) * 6;
		Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
		Cmd.m_BufferContainer = Container.m_QuadBufferContainer;
		Cmd.m_IndexBuffer = m_QuadIndexBuffer;

		AddCmd(Cmd);
	}
	else
	{
		if(g_Config.m_GfxQuadAsTriangle)
		{
			for(int i = 0; i < QuadDrawNum; ++i)
			{
				SQuadContainer::SQuad &Quad = Container.m_vQuads[QuadOffset + i];
				m_aVertices[i * 6] = Quad.m_aVertices[0];
				m_aVertices[i * 6 + 1] = Quad.m_aVertices[1];
				m_aVertices[i * 6 + 2] = Quad.m_aVertices[2];
				m_aVertices[i * 6 + 3] = Quad.m_aVertices[0];
				m_aVertices[i * 6 + 4] = Quad.m_aVertices[2];
				m_aVertices[i * 6 + 5] = Quad.m_aVertices[3];
				m_NumVertices += 6;
			}
		}
		else
		{
			mem_copy(m_aVertices, &Container.m_vQuads[QuadOffset], sizeof(CCommandBuffer::SVertex) * 4 * QuadDrawNum);
			m_NumVertices += 4 * QuadDrawNum;
		}
		m_Drawing = EDrawing::QUADS;
		if(ChangeWrapMode)
			WrapClamp();
		FlushVertices(false);
		m_Drawing = EDrawing::NONE;
	}
	WrapNormal();
}

void CGraphics_Threaded::RenderQuadContainerEx(int ContainerIndex, int QuadOffset, int QuadDrawNum, float X, float Y, float ScaleX, float ScaleY)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if((int)Container.m_vQuads.size() < QuadOffset + 1)
		return;

	if(QuadDrawNum == -1)
		QuadDrawNum = (int)(IsQuadContainerBufferingEnabled() ? Container.m_UploadedQuadCount : Container.m_vQuads.size()) - QuadOffset;

	if(IsQuadContainerBufferingEnabled())
	{
		if(!Container.m_QuadBufferContainer.IsValid() || Container.m_UploadedQuadCount < static_cast<size_t>(QuadOffset) + static_cast<size_t>(QuadDrawNum))
			return;

		SQuadContainer::SQuad &Quad = Container.m_vQuads[QuadOffset];
		CCommandBuffer::SCommand_DrawIndexed Cmd;
		CCommandBuffer::SDrawDataPrimitiveUniformColor DrawData;

		WrapClamp();

		CScreenRect ScreenRect = GetScreen();
		CScreenRect CommandScreenRect = ScreenRect.Move({-X, -Y});
		CommandScreenRect.m_TopLeft /= vec2(ScaleX, ScaleY);
		CommandScreenRect.m_BottomRight /= vec2(ScaleX, ScaleY);
		Cmd.m_State = m_State;
		Cmd.m_State.m_ScreenTL = CommandScreenRect.m_TopLeft;
		Cmd.m_State.m_ScreenBR = CommandScreenRect.m_BottomRight;

		Cmd.m_Pipeline = Pipeline(EPipelineProgram::PRIMITIVE_UNIFORM_COLOR);
		Cmd.m_IndexCount = static_cast<uint32_t>(QuadDrawNum) * 6;
		Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
		Cmd.m_BufferContainer = Container.m_QuadBufferContainer;
		Cmd.m_IndexBuffer = m_QuadIndexBuffer;

		DrawData.m_Color.r = (float)m_aColor[0].r / 255.f;
		DrawData.m_Color.g = (float)m_aColor[0].g / 255.f;
		DrawData.m_Color.b = (float)m_aColor[0].b / 255.f;
		DrawData.m_Color.a = (float)m_aColor[0].a / 255.f;

		DrawData.m_Rotation = m_Rotation;

		// rotate before positioning
		DrawData.m_RotationCenter.x = Quad.m_aVertices[0].m_Pos.x + (Quad.m_aVertices[1].m_Pos.x - Quad.m_aVertices[0].m_Pos.x) / 2.f;
		DrawData.m_RotationCenter.y = Quad.m_aVertices[0].m_Pos.y + (Quad.m_aVertices[2].m_Pos.y - Quad.m_aVertices[0].m_Pos.y) / 2.f;

		void *pDrawData = AllocCommandBufferData(sizeof(DrawData));
		Cmd.m_DrawData.m_pData = pDrawData;
		Cmd.m_DrawData.m_Size = sizeof(DrawData);
		mem_copy(pDrawData, &DrawData, sizeof(DrawData));
		AddCmd(Cmd, [&] {
			void *pRetryDrawData = m_pCommandBuffer->AllocData(sizeof(DrawData));
			if(pRetryDrawData == nullptr)
				return false;
			Cmd.m_DrawData.m_pData = pRetryDrawData;
			mem_copy(pRetryDrawData, &DrawData, sizeof(DrawData));
			return true;
		});
	}
	else
	{
		if(g_Config.m_GfxQuadAsTriangle)
		{
			for(int i = 0; i < QuadDrawNum; ++i)
			{
				SQuadContainer::SQuad &Quad = Container.m_vQuads[QuadOffset + i];
				m_aVertices[i * 6 + 0] = Quad.m_aVertices[0];
				m_aVertices[i * 6 + 1] = Quad.m_aVertices[1];
				m_aVertices[i * 6 + 2] = Quad.m_aVertices[2];
				m_aVertices[i * 6 + 3] = Quad.m_aVertices[0];
				m_aVertices[i * 6 + 4] = Quad.m_aVertices[2];
				m_aVertices[i * 6 + 5] = Quad.m_aVertices[3];

				for(int n = 0; n < 6; ++n)
				{
					m_aVertices[i * 6 + n].m_Pos.x *= ScaleX;
					m_aVertices[i * 6 + n].m_Pos.y *= ScaleY;

					SetColor(&m_aVertices[i * 6 + n], 0);
				}

				if(m_Rotation != 0)
				{
					CCommandBuffer::SPoint Center;
					Center.x = m_aVertices[i * 6 + 0].m_Pos.x + (m_aVertices[i * 6 + 1].m_Pos.x - m_aVertices[i * 6 + 0].m_Pos.x) / 2.f;
					Center.y = m_aVertices[i * 6 + 0].m_Pos.y + (m_aVertices[i * 6 + 2].m_Pos.y - m_aVertices[i * 6 + 0].m_Pos.y) / 2.f;

					Rotate(Center, &m_aVertices[i * 6 + 0], 6);
				}

				for(int n = 0; n < 6; ++n)
				{
					m_aVertices[i * 6 + n].m_Pos.x += X;
					m_aVertices[i * 6 + n].m_Pos.y += Y;
				}
				m_NumVertices += 6;
			}
		}
		else
		{
			mem_copy(m_aVertices, &Container.m_vQuads[QuadOffset], sizeof(CCommandBuffer::SVertex) * 4 * QuadDrawNum);
			for(int i = 0; i < QuadDrawNum; ++i)
			{
				for(int n = 0; n < 4; ++n)
				{
					m_aVertices[i * 4 + n].m_Pos.x *= ScaleX;
					m_aVertices[i * 4 + n].m_Pos.y *= ScaleY;
					SetColor(&m_aVertices[i * 4 + n], 0);
				}

				if(m_Rotation != 0)
				{
					CCommandBuffer::SPoint Center;
					Center.x = m_aVertices[i * 4 + 0].m_Pos.x + (m_aVertices[i * 4 + 1].m_Pos.x - m_aVertices[i * 4 + 0].m_Pos.x) / 2.f;
					Center.y = m_aVertices[i * 4 + 0].m_Pos.y + (m_aVertices[i * 4 + 2].m_Pos.y - m_aVertices[i * 4 + 0].m_Pos.y) / 2.f;

					Rotate(Center, &m_aVertices[i * 4 + 0], 4);
				}

				for(int n = 0; n < 4; ++n)
				{
					m_aVertices[i * 4 + n].m_Pos.x += X;
					m_aVertices[i * 4 + n].m_Pos.y += Y;
				}
				m_NumVertices += 4;
			}
		}
		m_Drawing = EDrawing::QUADS;
		WrapClamp();
		FlushVertices(false);
		m_Drawing = EDrawing::NONE;
	}
	WrapNormal();
}

void CGraphics_Threaded::RenderQuadContainerAsSprite(int ContainerIndex, int QuadOffset, float X, float Y, float ScaleX, float ScaleY)
{
	RenderQuadContainerEx(ContainerIndex, QuadOffset, 1, X, Y, ScaleX, ScaleY);
}

void CGraphics_Threaded::RenderQuadContainerAsSpriteMultiple(int ContainerIndex, int QuadOffset, int DrawCount, SRenderSpriteInfo *pRenderInfo)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if(DrawCount == 0)
		return;

	if(IsQuadContainerBufferingEnabled())
	{
		if(!Container.m_QuadBufferContainer.IsValid() || Container.m_UploadedQuadCount <= static_cast<size_t>(QuadOffset))
			return;

		WrapClamp();
		SQuadContainer::SQuad &Quad = Container.m_vQuads[0];
		CCommandBuffer::SCommand_DrawIndexed Cmd;
		CCommandBuffer::SDrawDataPrimitiveInstanced DrawData;

		Cmd.m_State = m_State;
		Cmd.m_Pipeline = Pipeline(EPipelineProgram::PRIMITIVE_INSTANCED);
		Cmd.m_IndexCount = 6;
		Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
		Cmd.m_InstanceCount = static_cast<uint32_t>(DrawCount);
		Cmd.m_BufferContainer = Container.m_QuadBufferContainer;
		Cmd.m_IndexBuffer = m_QuadIndexBuffer;

		DrawData.m_Color.r = (float)m_aColor[0].r / 255.f;
		DrawData.m_Color.g = (float)m_aColor[0].g / 255.f;
		DrawData.m_Color.b = (float)m_aColor[0].b / 255.f;
		DrawData.m_Color.a = (float)m_aColor[0].a / 255.f;

		// rotate before positioning
		DrawData.m_RotationCenter.x = Quad.m_aVertices[0].m_Pos.x + (Quad.m_aVertices[1].m_Pos.x - Quad.m_aVertices[0].m_Pos.x) / 2.f;
		DrawData.m_RotationCenter.y = Quad.m_aVertices[0].m_Pos.y + (Quad.m_aVertices[2].m_Pos.y - Quad.m_aVertices[0].m_Pos.y) / 2.f;

		Cmd.m_DrawData.m_Size = sizeof(DrawData);
		Cmd.m_ArrayData.m_Size = sizeof(CCommandBuffer::SInstanceDataPositionScaleRotation) * static_cast<size_t>(DrawCount);
		auto AllocatePayload = [&] {
			void *pDrawData = m_pCommandBuffer->AllocData(sizeof(DrawData));
			auto *pInstanceData = static_cast<CCommandBuffer::SInstanceDataPositionScaleRotation *>(m_pCommandBuffer->AllocData(Cmd.m_ArrayData.m_Size));
			if(pDrawData == nullptr || pInstanceData == nullptr)
				return false;
			mem_copy(pDrawData, &DrawData, sizeof(DrawData));
			for(int i = 0; i < DrawCount; ++i)
			{
				pInstanceData[i].m_Position = pRenderInfo[i].m_Pos;
				pInstanceData[i].m_Scale = pRenderInfo[i].m_Scale;
				pInstanceData[i].m_Rotation = pRenderInfo[i].m_Rotation;
			}
			Cmd.m_DrawData.m_pData = pDrawData;
			Cmd.m_ArrayData.m_pData = pInstanceData;
			return true;
		};

		if(!AllocatePayload())
		{
			DropCurrentFrame();
			if(!AllocatePayload())
			{
				log_error("graphics", "Failed to allocate instance data. InstanceCount=%d", DrawCount);
				WrapNormal();
				return;
			}
		}

		if(!AddCmd(Cmd, AllocatePayload))
		{
			WrapNormal();
			return;
		}

		WrapNormal();
	}
	else
	{
		for(int i = 0; i < DrawCount; ++i)
		{
			QuadsSetRotation(pRenderInfo[i].m_Rotation);
			RenderQuadContainerAsSprite(ContainerIndex, QuadOffset, pRenderInfo[i].m_Pos.x, pRenderInfo[i].m_Pos.y, pRenderInfo[i].m_Scale, pRenderInfo[i].m_Scale);
		}
	}
}

void *CGraphics_Threaded::AllocCommandBufferData(size_t AllocSize)
{
	CCommandBuffer *pCommandBuffer = GetCommandBuffer(CCommandBuffer::CMD_DRAW);
	void *pData = pCommandBuffer->AllocData(AllocSize);
	if(pData == nullptr)
	{
		// ponytail: a frame larger than the fixed arena is discarded; add chunked
		// frame storage only if real maps demonstrate that this bound is too small.
		DropCurrentFrame();
		pCommandBuffer = GetCommandBuffer(CCommandBuffer::CMD_DRAW);

		pData = pCommandBuffer->AllocData(AllocSize);
		dbg_assert(pData, "graphics: failed to allocate data (size %" PRIzu ") for command buffer", AllocSize);
	}
	return pData;
}

void *CGraphics_Threaded::AllocReliableCommandBufferData(size_t AllocSize)
{
	CCommandBuffer *pCommandBuffer = GetCommandBuffer(CCommandBuffer::CMD_UPDATE_BUFFER_OBJECT);
	if(pCommandBuffer == nullptr)
		return nullptr;
	void *pData = pCommandBuffer->AllocData(AllocSize);
	if(pData == nullptr)
	{
		if(!SubmitReliableCommandBuffer(pCommandBuffer))
			return nullptr;
		pCommandBuffer = GetCommandBuffer(CCommandBuffer::CMD_UPDATE_BUFFER_OBJECT);
		if(pCommandBuffer == nullptr)
			return nullptr;

		pData = pCommandBuffer->AllocData(AllocSize);
	}
	return pData;
}

CGraphics_Threaded::CBufferHandle CGraphics_Threaded::CreateBufferObjectInternal(size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer, EBufferUsage Usage)
{
	CBufferHandle Buffer = m_BufferHandles.Allocate();

	dbg_assert((CreateFlags & EBufferObjectCreateFlags::BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) == 0 || (UploadDataSize <= CCommandBuffer::MAX_VERTICES * std::max(sizeof(CCommandBuffer::SVertexTex3DStream), sizeof(CCommandBuffer::SVertexTex3D))),
		"If BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT is used, then the buffer size must not exceed max(sizeof(CCommandBuffer::SVertexTex3DStream), sizeof(CCommandBuffer::SVertexTex3D)) * CCommandBuffer::MAX_VERTICES");

	CCommandBuffer::SCommand_CreateBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	Cmd.m_Desc.m_Size = UploadDataSize;
	Cmd.m_Desc.m_Lifetime = (CreateFlags & EBufferObjectCreateFlags::BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) != 0 ? EBufferLifetime::FRAME : EBufferLifetime::PERSISTENT;
	Cmd.m_Desc.m_Usage = Usage;

	if(IsMovedPointer)
	{
		Cmd.m_pUploadData = pUploadData;
		Cmd.m_DeletePointer = true;
	}
	else
	{
		Cmd.m_pUploadData = UploadDataSize <= CMD_BUFFER_DATA_BUFFER_SIZE ? AllocReliableCommandBufferData(UploadDataSize) : nullptr;
		if(Cmd.m_pUploadData != nullptr)
		{
			mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
			Cmd.m_DeletePointer = false;
		}
		else
		{
			Cmd.m_pUploadData = malloc(UploadDataSize);
			if(Cmd.m_pUploadData == nullptr && UploadDataSize != 0)
			{
				m_BufferHandles.Release(&Buffer);
				return Buffer;
			}
			mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
			Cmd.m_DeletePointer = true;
		}
	}

	if(!AddCmd(Cmd))
	{
		if(Cmd.m_DeletePointer && !IsMovedPointer)
			free(Cmd.m_pUploadData);
		m_BufferHandles.Release(&Buffer);
	}
	return Buffer;
}

CGraphics_Threaded::CBufferHandle CGraphics_Threaded::CreateBufferObject(size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer)
{
	return CreateBufferObjectInternal(UploadDataSize, pUploadData, CreateFlags, IsMovedPointer, EBufferUsage::VERTEX);
}

bool CGraphics_Threaded::RecreateBufferObjectInternal(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer, EBufferUsage Usage)
{
	dbg_assert(m_BufferHandles.IsAllocated(Buffer), "Cannot recreate stale buffer handle");
	CCommandBuffer::SCommand_RecreateBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	Cmd.m_Desc.m_Size = UploadDataSize;
	Cmd.m_Desc.m_Lifetime = (CreateFlags & EBufferObjectCreateFlags::BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) != 0 ? EBufferLifetime::FRAME : EBufferLifetime::PERSISTENT;
	Cmd.m_Desc.m_Usage = Usage;

	dbg_assert((CreateFlags & EBufferObjectCreateFlags::BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) == 0 || (UploadDataSize <= CCommandBuffer::MAX_VERTICES * std::max(sizeof(CCommandBuffer::SVertexTex3DStream), sizeof(CCommandBuffer::SVertexTex3D))),
		"If BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT is used, then the buffer size must not exceed max(sizeof(CCommandBuffer::SVertexTex3DStream), sizeof(CCommandBuffer::SVertexTex3D)) * CCommandBuffer::MAX_VERTICES");

	if(IsMovedPointer)
	{
		Cmd.m_pUploadData = pUploadData;
		Cmd.m_DeletePointer = true;
	}
	else
	{
		Cmd.m_pUploadData = UploadDataSize <= CMD_BUFFER_DATA_BUFFER_SIZE ? AllocReliableCommandBufferData(UploadDataSize) : nullptr;
		if(Cmd.m_pUploadData != nullptr)
		{
			mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
			Cmd.m_DeletePointer = false;
		}
		else
		{
			Cmd.m_pUploadData = malloc(UploadDataSize);
			if(Cmd.m_pUploadData == nullptr && UploadDataSize != 0)
				return false;
			mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
			Cmd.m_DeletePointer = true;
		}
	}
	if(AddCmd(Cmd))
		return true;
	if(Cmd.m_DeletePointer && !IsMovedPointer)
		free(Cmd.m_pUploadData);
	return false;
}

bool CGraphics_Threaded::RecreateBufferObject(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer)
{
	return RecreateBufferObjectInternal(Buffer, UploadDataSize, pUploadData, CreateFlags, IsMovedPointer, EBufferUsage::VERTEX);
}

bool CGraphics_Threaded::UpdateBufferObjectInternal(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, size_t Offset, bool IsMovedPointer)
{
	dbg_assert(m_BufferHandles.IsAllocated(Buffer), "Cannot update stale buffer handle");
	CCommandBuffer::SCommand_UpdateBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	Cmd.m_DataSize = UploadDataSize;
	Cmd.m_Offset = Offset;
	Cmd.m_DeletePointer = IsMovedPointer;

	if(IsMovedPointer)
	{
		Cmd.m_pUploadData = pUploadData;
	}
	else
	{
		Cmd.m_pUploadData = AllocReliableCommandBufferData(UploadDataSize);
		if(Cmd.m_pUploadData != nullptr)
			mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
		else
		{
			Cmd.m_pUploadData = malloc(UploadDataSize);
			if(Cmd.m_pUploadData == nullptr && UploadDataSize != 0)
				return false;
			mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
			Cmd.m_DeletePointer = true;
		}
	}
	if(AddCmd(Cmd))
		return true;
	if(Cmd.m_DeletePointer && !IsMovedPointer)
		free(Cmd.m_pUploadData);
	return false;
}

void CGraphics_Threaded::CopyBufferObjectInternal(CBufferHandle WriteBuffer, CBufferHandle ReadBuffer, size_t WriteOffset, size_t ReadOffset, size_t CopyDataSize)
{
	dbg_assert(m_BufferHandles.IsAllocated(WriteBuffer), "Cannot copy to stale buffer handle");
	dbg_assert(m_BufferHandles.IsAllocated(ReadBuffer), "Cannot copy from stale buffer handle");
	CCommandBuffer::SCommand_CopyBufferObject Cmd;
	Cmd.m_WriteBuffer = WriteBuffer;
	Cmd.m_ReadBuffer = ReadBuffer;
	Cmd.m_WriteOffset = WriteOffset;
	Cmd.m_ReadOffset = ReadOffset;
	Cmd.m_CopySize = CopyDataSize;
	AddCmd(Cmd);
}

void CGraphics_Threaded::DeleteBufferObject(CBufferHandle &Buffer)
{
	if(!Buffer.IsValid())
		return;
	dbg_assert(m_BufferHandles.IsAllocated(Buffer), "Cannot delete stale buffer handle");

	CCommandBuffer::SCommand_DeleteBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	if(!AddCmd(Cmd))
		return;

	const CBufferHandle RetiredHandle = Buffer;
	dbg_assert(m_BufferHandles.Retire(&Buffer), "Cannot retire stale buffer handle");
	m_vRetiredBufferHandles.push_back(RetiredHandle);
}

CGraphics_Threaded::CBufferContainerHandle CGraphics_Threaded::CreateBufferContainer(SBufferContainerInfo *pContainerInfo)
{
	dbg_assert(m_BufferHandles.IsAllocated(pContainerInfo->m_VertBufferBinding), "Cannot bind stale buffer handle");
	CBufferContainerHandle Container = m_BufferContainerHandles.Allocate();
	if(static_cast<size_t>(Container.Id()) >= m_vVertexArrayInfo.size())
		m_vVertexArrayInfo.resize(Container.Id() + 1);

	CCommandBuffer::SCommand_CreateBufferContainer Cmd;
	Cmd.m_BufferContainer = Container;
	Cmd.m_AttrCount = pContainerInfo->m_vAttributes.size();
	Cmd.m_Stride = pContainerInfo->m_Stride;
	Cmd.m_VertBufferBinding = pContainerInfo->m_VertBufferBinding;
	Cmd.m_pAttributes = (SBufferContainerInfo::SAttribute *)AllocReliableCommandBufferData(Cmd.m_AttrCount * sizeof(SBufferContainerInfo::SAttribute));
	if(Cmd.m_pAttributes == nullptr)
	{
		m_BufferContainerHandles.Release(&Container);
		return Container;
	}

	if(!AddCmd(Cmd, [&] {
		   Cmd.m_pAttributes = m_pReliableCommandBuffer == nullptr ? nullptr : (SBufferContainerInfo::SAttribute *)m_pReliableCommandBuffer->AllocData(Cmd.m_AttrCount * sizeof(SBufferContainerInfo::SAttribute));
		   return Cmd.m_pAttributes != nullptr;
	   }))
	{
		m_BufferContainerHandles.Release(&Container);
		return Container;
	}

	mem_copy(Cmd.m_pAttributes, pContainerInfo->m_vAttributes.data(), Cmd.m_AttrCount * sizeof(SBufferContainerInfo::SAttribute));

	m_vVertexArrayInfo[Container.Id()].m_AssociatedBuffer = pContainerInfo->m_VertBufferBinding;

	return Container;
}

void CGraphics_Threaded::DeleteBufferContainer(CBufferContainerHandle &Container, bool DestroyAllBO)
{
	if(!Container.IsValid())
		return;
	dbg_assert(m_BufferContainerHandles.IsAllocated(Container), "Cannot delete stale buffer container handle");

	CCommandBuffer::SCommand_DeleteBufferContainer Cmd;
	Cmd.m_BufferContainer = Container;
	Cmd.m_DestroyAllBO = DestroyAllBO;
	if(!AddCmd(Cmd))
		return;

	SVertexArrayInfo &Info = m_vVertexArrayInfo[Container.Id()];
	if(DestroyAllBO)
	{
		if(Info.m_AssociatedBuffer.IsValid())
		{
			const CBufferHandle RetiredBuffer = Info.m_AssociatedBuffer;
			dbg_assert(m_BufferHandles.Retire(&Info.m_AssociatedBuffer), "Cannot retire stale associated buffer handle");
			m_vRetiredBufferHandles.push_back(RetiredBuffer);
		}
	}
	Info.m_AssociatedBuffer.Invalidate();
	const CBufferContainerHandle RetiredContainer = Container;
	dbg_assert(m_BufferContainerHandles.Retire(&Container), "Cannot retire stale buffer container handle");
	m_vRetiredBufferContainerHandles.push_back(RetiredContainer);
}

void CGraphics_Threaded::UpdateBufferContainerInternal(CBufferContainerHandle Container, SBufferContainerInfo *pContainerInfo)
{
	dbg_assert(m_BufferContainerHandles.IsAllocated(Container), "Cannot update stale buffer container handle");
	dbg_assert(m_BufferHandles.IsAllocated(pContainerInfo->m_VertBufferBinding), "Cannot bind stale buffer handle");
	CCommandBuffer::SCommand_UpdateBufferContainer Cmd;
	Cmd.m_BufferContainer = Container;
	Cmd.m_AttrCount = pContainerInfo->m_vAttributes.size();
	Cmd.m_Stride = pContainerInfo->m_Stride;
	Cmd.m_VertBufferBinding = pContainerInfo->m_VertBufferBinding;
	Cmd.m_pAttributes = (SBufferContainerInfo::SAttribute *)AllocReliableCommandBufferData(Cmd.m_AttrCount * sizeof(SBufferContainerInfo::SAttribute));
	if(Cmd.m_pAttributes == nullptr)
		return;

	if(!AddCmd(Cmd, [&] {
		   Cmd.m_pAttributes = m_pReliableCommandBuffer == nullptr ? nullptr : (SBufferContainerInfo::SAttribute *)m_pReliableCommandBuffer->AllocData(Cmd.m_AttrCount * sizeof(SBufferContainerInfo::SAttribute));
		   return Cmd.m_pAttributes != nullptr;
	   }))
		return;

	mem_copy(Cmd.m_pAttributes, pContainerInfo->m_vAttributes.data(), Cmd.m_AttrCount * sizeof(SBufferContainerInfo::SAttribute));

	m_vVertexArrayInfo[Container.Id()].m_AssociatedBuffer = pContainerInfo->m_VertBufferBinding;
}

bool CGraphics_Threaded::IndicesNumRequiredNotify(unsigned int RequiredIndicesCount)
{
	if(RequiredIndicesCount <= m_QuadIndexCount)
		return true;
	if(RequiredIndicesCount % 6 != 0)
		return false;

	std::vector<uint32_t> vIndices(RequiredIndicesCount);
	uint32_t Vertex = 0;
	for(unsigned int Index = 0; Index < RequiredIndicesCount; Index += 6, Vertex += 4)
	{
		vIndices[Index] = Vertex;
		vIndices[Index + 1] = Vertex + 1;
		vIndices[Index + 2] = Vertex + 2;
		vIndices[Index + 3] = Vertex;
		vIndices[Index + 4] = Vertex + 2;
		vIndices[Index + 5] = Vertex + 3;
	}

	const size_t DataSize = vIndices.size() * sizeof(vIndices[0]);
	const bool Ready = m_QuadIndexBuffer.IsValid() ?
				   RecreateBufferObjectInternal(m_QuadIndexBuffer, DataSize, vIndices.data(), 0, false, EBufferUsage::INDEX) :
				   (m_QuadIndexBuffer = CreateBufferObjectInternal(DataSize, vIndices.data(), 0, false, EBufferUsage::INDEX)).IsValid();
	if(Ready)
		m_QuadIndexCount = RequiredIndicesCount;
	return Ready;
}

int CGraphics_Threaded::IssueInit()
{
	// The flags have to be kept consistent with flags set in the CGraphicsBackend_SDL::SetWindowParams function!

	bool IsPurelyWindowed = g_Config.m_GfxFullscreen == 0;
	bool IsExclusiveFullscreen = g_Config.m_GfxFullscreen == 1;
	bool IsDesktopFullscreen = g_Config.m_GfxFullscreen == 2;
#ifndef CONF_FAMILY_WINDOWS
	//  Windowed fullscreen is only available on Windows, use desktop fullscreen on other platforms
	IsDesktopFullscreen |= g_Config.m_GfxFullscreen == 3;
#endif

	int Flags = 0;
	if(IsExclusiveFullscreen)
	{
		Flags |= IGraphicsBackend::INITFLAG_FULLSCREEN;
	}
	else if(IsDesktopFullscreen)
	{
		Flags |= IGraphicsBackend::INITFLAG_DESKTOP_FULLSCREEN;
	}
	else if(IsPurelyWindowed)
	{
		Flags |= IGraphicsBackend::INITFLAG_RESIZABLE;
		if(g_Config.m_GfxBorderless)
		{
			Flags |= IGraphicsBackend::INITFLAG_BORDERLESS;
		}
	}
	if(g_Config.m_GfxVsync)
	{
		Flags |= IGraphicsBackend::INITFLAG_VSYNC;
	}

	const int Result = m_pBackend->Init("DDNet Client", &g_Config.m_GfxScreen, &g_Config.m_GfxScreenWidth, &g_Config.m_GfxScreenHeight, &g_Config.m_GfxScreenRefreshRate, &g_Config.m_GfxFsaaSamples, Flags, &m_DesktopSize.x, &m_DesktopSize.y, &m_ScreenWidth, &m_ScreenHeight, m_pStorage);
	AddBackEndWarningIfExists();
	if(Result == 0)
	{
		const SBackendCapabilities BackendCapabilities = m_pBackend->GetCapabilities();
		m_Capabilities.m_TileBuffering = BackendCapabilities.m_ArrayColorPipelines;
		m_Capabilities.m_QuadBuffering = BackendCapabilities.m_QuadPipelines;
		m_Capabilities.m_QuadContainerBuffering = BackendCapabilities.m_BufferedPrimitivePipelines;
		m_Capabilities.m_TextBuffering = m_Capabilities.m_QuadContainerBuffering && BackendCapabilities.m_DualAtlasPipeline;
		m_Capabilities.m_TextureArrays = BackendCapabilities.m_2DArrayTextures || BackendCapabilities.m_3DTextures;
		m_Capabilities.m_2DTextureArrays = BackendCapabilities.m_2DArrayTextures;
		m_Capabilities.m_QuadToTriangleConversion = BackendCapabilities.m_TrianglesAsQuads;
		m_Capabilities.m_RenderTargets = BackendCapabilities.m_RenderTargets;
		m_ScreenHiDPIScale = m_ScreenWidth / (float)g_Config.m_GfxScreenWidth;
		m_ScreenRefreshRate = g_Config.m_GfxScreenRefreshRate;
	}
	return Result;
}

void CGraphics_Threaded::AdjustViewport(bool SendViewportChangeToBackend)
{
	const int SurfaceWidth = m_ScreenWidth;
	const int SurfaceHeight = m_ScreenHeight;
	// adjust the viewport to only allow certain aspect ratios
	// keep this in sync with backend_vulkan GetSwapImageSize's check
	if(m_ScreenHeight > 4 * m_ScreenWidth / 5)
	{
		m_IsForcedViewport = true;
		m_ScreenHeight = 4 * m_ScreenWidth / 5;

		if(SendViewportChangeToBackend)
		{
			UpdateViewportInternal(0, 0, m_ScreenWidth, m_ScreenHeight, true, SurfaceWidth, SurfaceHeight);
		}
	}
	else
	{
		m_IsForcedViewport = false;
	}
}

void CGraphics_Threaded::UpdateViewport(int X, int Y, int W, int H, bool ByResize)

{
	UpdateViewportInternal(X, Y, W, H, ByResize, W, H);
}

void CGraphics_Threaded::UpdateViewportInternal(int X, int Y, int W, int H, bool ByResize, int SurfaceW, int SurfaceH)
{
	CCommandBuffer::SCommand_Update_Viewport Cmd;
	Cmd.m_X = X;
	Cmd.m_Y = Y;
	Cmd.m_Width = W;
	Cmd.m_Height = H;
	Cmd.m_SurfaceWidth = SurfaceW;
	Cmd.m_SurfaceHeight = SurfaceH;
	Cmd.m_ByResize = ByResize;
	AddCmd(Cmd);
}

void CGraphics_Threaded::AddBackEndWarningIfExists()
{
	const char *pErrStr = m_pBackend->GetErrorString();
	if(pErrStr != nullptr)
	{
		SWarning NewWarning;
		str_copy(NewWarning.m_aWarningMsg, Localize(pErrStr));
		AddWarning(NewWarning);
	}
}

int CGraphics_Threaded::InitWindow()
{
	int ErrorCode = IssueInit();
	if(ErrorCode == 0)
		return 0;

	// try disabling fsaa
	while(g_Config.m_GfxFsaaSamples)
	{
		// 4 is the minimum required by OpenGL ES spec (GL_MAX_SAMPLES - https://www.khronos.org/registry/OpenGL-Refpages/es3.0/html/glGet.xhtml),
		// so can probably also be assumed for OpenGL
		if(g_Config.m_GfxFsaaSamples > 4)
			g_Config.m_GfxFsaaSamples = 4;
		else
			g_Config.m_GfxFsaaSamples = 0;

		if(g_Config.m_GfxFsaaSamples)
			log_warn("gfx", "Failed to initialize graphics. Lowering FSAA to %d and trying again.", g_Config.m_GfxFsaaSamples);
		else
			log_warn("gfx", "Failed to initialize graphics. Disabling FSAA and trying again.");

		ErrorCode = IssueInit();
		if(ErrorCode == 0)
			return 0;
	}

	size_t GLInitTryCount = 0;
	while(ErrorCode == EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED ||
		ErrorCode == EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED)
	{
		if(ErrorCode == EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED)
		{
			// try next smaller major/minor or patch version
			if(g_Config.m_GfxGLMajor >= 4)
			{
				g_Config.m_GfxGLMajor = 3;
				g_Config.m_GfxGLMinor = 3;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor >= 1)
			{
				g_Config.m_GfxGLMajor = 3;
				g_Config.m_GfxGLMinor = 0;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor == 0)
			{
				g_Config.m_GfxGLMajor = 2;
				g_Config.m_GfxGLMinor = 1;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 2 && g_Config.m_GfxGLMinor >= 1)
			{
				g_Config.m_GfxGLMajor = 2;
				g_Config.m_GfxGLMinor = 0;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 2 && g_Config.m_GfxGLMinor == 0)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 5;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 5)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 4;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 4)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 3;
				g_Config.m_GfxGLPatch = 0;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 3)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 2;
				g_Config.m_GfxGLPatch = 1;
			}
			else if(g_Config.m_GfxGLMajor == 1 && g_Config.m_GfxGLMinor == 2)
			{
				g_Config.m_GfxGLMajor = 1;
				g_Config.m_GfxGLMinor = 1;
				g_Config.m_GfxGLPatch = 0;
			}
		}
		log_warn("gfx", "Failed to initialize graphics. Setting GL version %d.%d.%d and trying again.", g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch);

		// new gl version was set by backend, try again
		ErrorCode = IssueInit();
		if(ErrorCode == 0)
		{
			return 0;
		}

		if(++GLInitTryCount >= 9)
		{
			// try something else
			break;
		}
	}

	// try lowering the resolution
	if(g_Config.m_GfxScreenWidth != 640 || g_Config.m_GfxScreenHeight != 480)
	{
		g_Config.m_GfxScreenWidth = 640;
		g_Config.m_GfxScreenHeight = 480;
		log_warn("gfx", "Failed to initialize graphics. Setting resolution to %dx%d and trying again.", g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight);

		if(IssueInit() == 0)
			return 0;
	}

	// at the very end, just try to set to gl 1.4
	{
		g_Config.m_GfxGLMajor = 1;
		g_Config.m_GfxGLMinor = 4;
		g_Config.m_GfxGLPatch = 0;
		log_warn("gfx", "Failed to initialize graphics. Setting GL version %d.%d.%d and trying again.", g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch);

		if(IssueInit() == 0)
			return 0;
	}

	log_error("gfx", "Failed to initialize graphics. Out of ideas.");
	return -1;
}

int CGraphics_Threaded::Init()
{
	// fetch pointers
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();

	// init textures
	m_TextureHandles.Reset(CCommandBuffer::MAX_TEXTURES);
	m_TextureBindingHandles.Reset(0);
	m_vTextureBindingInfos.clear();

	m_BufferHandles.Reset(0);
	m_BufferContainerHandles.Reset(0);
	m_FirstFreeQuadContainer = -1;

	m_pBackend = CreateGraphicsBackend(BackendOverrideFromEnvironment());
	if(InitWindow() != 0)
		return -1;

	for(auto &FakeMode : g_aFakeModes)
	{
		FakeMode.m_WindowWidth = FakeMode.m_CanvasWidth / m_ScreenHiDPIScale;
		FakeMode.m_WindowHeight = FakeMode.m_CanvasHeight / m_ScreenHiDPIScale;
		FakeMode.m_RefreshRate = g_Config.m_GfxScreenRefreshRate;
	}

	// create command buffers
	for(auto &pCommandBuffer : m_apCommandBuffers)
		pCommandBuffer = new CCommandBuffer(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE);
	m_pCommandBuffer = m_apCommandBuffers[0];
	for(auto &pCommandBuffer : m_apReliableCommandBuffers)
		pCommandBuffer = new CCommandBuffer(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE, RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE);
	m_pReliableCommandBuffer = m_apReliableCommandBuffers[0];
	m_pDeferredDestroyCommandBuffer = new CCommandBuffer(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE);
	CreatePipelines();

	m_QuadIndexBuffer.Invalidate();
	m_QuadIndexCount = 0;
	if((m_Capabilities.m_QuadToTriangleConversion || m_Capabilities.m_TileBuffering || m_Capabilities.m_QuadBuffering || m_Capabilities.m_TextBuffering || m_Capabilities.m_QuadContainerBuffering) &&
		!IndicesNumRequiredNotify(CCommandBuffer::MAX_VERTICES / 4 * 6))
		return -1;

	// create null texture, will get id=0
	{
		const size_t PixelSize = 4;
		const unsigned char aRed[] = {0xff, 0x00, 0x00, 0xff};
		const unsigned char aGreen[] = {0x00, 0xff, 0x00, 0xff};
		const unsigned char aBlue[] = {0x00, 0x00, 0xff, 0xff};
		const unsigned char aYellow[] = {0xff, 0xff, 0x00, 0xff};
		constexpr size_t NullTextureDimension = 16;
		unsigned char aNullTextureData[NullTextureDimension * NullTextureDimension * PixelSize];
		for(size_t y = 0; y < NullTextureDimension; ++y)
		{
			for(size_t x = 0; x < NullTextureDimension; ++x)
			{
				const unsigned char *pColor;
				if(x < NullTextureDimension / 2 && y < NullTextureDimension / 2)
					pColor = aRed;
				else if(x >= NullTextureDimension / 2 && y < NullTextureDimension / 2)
					pColor = aGreen;
				else if(x < NullTextureDimension / 2 && y >= NullTextureDimension / 2)
					pColor = aBlue;
				else
					pColor = aYellow;
				mem_copy(&aNullTextureData[(y * NullTextureDimension + x) * PixelSize], pColor, PixelSize);
			}
		}
		CImageInfo NullTextureInfo;
		NullTextureInfo.m_Width = NullTextureDimension;
		NullTextureInfo.m_Height = NullTextureDimension;
		NullTextureInfo.m_Format = CImageInfo::FORMAT_RGBA;
		NullTextureInfo.m_pData = aNullTextureData;
		m_NullTexture.Invalidate();
		m_NullTexture = LoadTextureRaw(NullTextureInfo, TextureLoadFlags(), "null-texture");
		dbg_assert(m_NullTexture.IsValid() && m_NullTexture.Id() == 0, "Null texture invalid");
	}

	static constexpr LOG_COLOR GPU_INFO_LOG_COLOR = LOG_COLOR{153, 127, 255};
	log_info_color(GPU_INFO_LOG_COLOR, "gfx", "GPU vendor: %s", GetVendorString());
	log_info_color(GPU_INFO_LOG_COLOR, "gfx", "GPU renderer: %s", GetRendererString());
	log_info_color(GPU_INFO_LOG_COLOR, "gfx", "GPU version: %s", GetVersionString());

	AdjustViewport(true);

	return 0;
}

void CGraphics_Threaded::Shutdown()
{
	if(!DestroyPipelines())
		log_error("graphics", "Failed to queue pipeline destruction during shutdown");
	DeleteBufferObject(m_QuadIndexBuffer);
	m_QuadIndexCount = 0;
	if(!SubmitReliableCommandBuffer(m_pReliableCommandBuffer))
	{
		m_pBackend->WaitForIdle();
		if(!SubmitReliableCommandBuffer(m_pReliableCommandBuffer))
			log_error("graphics", "Failed to submit reliable rendering commands during shutdown");
	}
	SubmitFramePacket();
	if(!SubmitDeferredDestroys())
	{
		m_pBackend->WaitForIdle();
		if(!SubmitDeferredDestroys())
			log_error("graphics", "Failed to submit deferred rendering destroys during shutdown");
	}
	m_pBackend->WaitForIdle();

	// shutdown the backend
	m_pBackend->Shutdown();
	delete m_pBackend;
	m_pBackend = nullptr;

	// delete the command buffers
	for(auto &pCommandBuffer : m_apCommandBuffers)
		delete pCommandBuffer;
	for(auto &pCommandBuffer : m_apReliableCommandBuffers)
		delete pCommandBuffer;
	delete m_pDeferredDestroyCommandBuffer;
}

int CGraphics_Threaded::GetNumScreens() const
{
	return m_pBackend->GetNumScreens();
}

const char *CGraphics_Threaded::GetScreenName(int Screen) const
{
	return m_pBackend->GetScreenName(Screen);
}

void CGraphics_Threaded::Minimize()
{
	m_pBackend->Minimize();

	for(auto &PropChangedListener : m_vPropChangeListeners)
		PropChangedListener();
}

void CGraphics_Threaded::WarnPngliteIncompatibleImages(bool Warn)
{
	m_WarnPngliteIncompatibleImages = Warn;
}

void CGraphics_Threaded::SetWindowParams(int FullscreenMode, bool IsBorderless)
{
	g_Config.m_GfxFullscreen = std::clamp(FullscreenMode, 0, 3);
	g_Config.m_GfxBorderless = (int)IsBorderless;

	m_pBackend->SetWindowParams(g_Config.m_GfxFullscreen, g_Config.m_GfxBorderless);
	CVideoMode CurMode;
	m_pBackend->GetCurrentVideoMode(CurMode, m_ScreenHiDPIScale, m_DesktopSize.x, m_DesktopSize.y, g_Config.m_GfxScreen);
	GotResized(CurMode.m_WindowWidth, CurMode.m_WindowHeight, CurMode.m_RefreshRate);

	for(auto &PropChangedListener : m_vPropChangeListeners)
		PropChangedListener();
}

bool CGraphics_Threaded::SetWindowScreen(int Index, bool MoveToCenter)
{
	if(!m_pBackend->SetWindowScreen(Index, MoveToCenter, &m_DesktopSize))
	{
		return false;
	}

	// send a got resized event so that the current canvas size is requested
	GotResized(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate);

	for(auto &PropChangedListener : m_vPropChangeListeners)
		PropChangedListener();

	return true;
}

bool CGraphics_Threaded::SwitchWindowScreen(int Index, bool MoveToCenter)
{
	const int IsFullscreen = g_Config.m_GfxFullscreen;
	const int IsBorderless = g_Config.m_GfxBorderless;
	const bool IsPurelyWindowed = IsFullscreen == 0 && !IsBorderless;

	if(!SetWindowScreen(Index, !IsPurelyWindowed || MoveToCenter))
	{
		return false;
	}

	if(IsFullscreen != 3 && !IsPurelyWindowed)
	{
		// Prevent window from being stretched over multiple monitors by temporarily switching to
		// windowed fullscreen mode on Windows, which is desktop fullscreen mode on other systems.
		SetWindowParams(3, false);
	}

	// In purely windowed mode we preserve the window's size instead of resizing to the screen.
	if(!IsPurelyWindowed)
	{
		CVideoMode CurMode;
		GetCurrentVideoMode(CurMode, Index);

		g_Config.m_GfxScreenWidth = CurMode.m_WindowWidth;
		g_Config.m_GfxScreenHeight = CurMode.m_WindowHeight;
		g_Config.m_GfxScreenRefreshRate = CurMode.m_RefreshRate;

		ResizeToScreen();
	}

	SetWindowParams(IsFullscreen, IsBorderless);
	return true;
}

void CGraphics_Threaded::Move(int x, int y)
{
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && IVideo::Current()->IsRecording())
		return;
#endif

	// Only handling CurScreen != m_GfxScreen doesn't work reliably
	const int CurScreen = m_pBackend->GetWindowScreen();
	if(!m_pBackend->UpdateDisplayMode(CurScreen, &m_DesktopSize))
		return;

	// send a got resized event so that the current canvas size is requested
	GotResized(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate);

	for(auto &PropChangedListener : m_vPropChangeListeners)
		PropChangedListener();
}

bool CGraphics_Threaded::Resize(int w, int h, int RefreshRate)
{
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && IVideo::Current()->IsRecording())
		return false;
#endif

	if(WindowWidth() == w && WindowHeight() == h && RefreshRate == m_ScreenRefreshRate)
		return false;

	// if the size is changed manually, only set the window resize, a window size changed event is triggered anyway
	if(m_pBackend->ResizeWindow(w, h, RefreshRate))
	{
		CVideoMode CurMode;
		m_pBackend->GetCurrentVideoMode(CurMode, m_ScreenHiDPIScale, m_DesktopSize.x, m_DesktopSize.y, g_Config.m_GfxScreen);
		GotResized(w, h, RefreshRate);
		return true;
	}
	return false;
}

void CGraphics_Threaded::ResizeToScreen()
{
	if(Resize(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight, g_Config.m_GfxScreenRefreshRate))
		return;

	// Revert config variables if the change was not accepted
	g_Config.m_GfxScreenWidth = ScreenWidth();
	g_Config.m_GfxScreenHeight = ScreenHeight();
	g_Config.m_GfxScreenRefreshRate = m_ScreenRefreshRate;
}

void CGraphics_Threaded::GotResized(int w, int h, int RefreshRate)
{
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current() && IVideo::Current()->IsRecording())
		return;
#endif
	// Commands recorded against the old drawable size must never become a partial
	// frame followed by a viewport update for the new size.
	if(!m_pCommandBuffer->IsEmpty())
		DropCurrentFrame();

	// if RefreshRate is -1 use the current config refresh rate
	if(RefreshRate == -1)
		RefreshRate = g_Config.m_GfxScreenRefreshRate;

	// if the size change event is triggered, set all parameters and change the viewport
	auto PrevCanvasWidth = m_ScreenWidth;
	auto PrevCanvasHeight = m_ScreenHeight;
	m_pBackend->GetViewportSize(m_ScreenWidth, m_ScreenHeight);
	const int SurfaceWidth = m_ScreenWidth;
	const int SurfaceHeight = m_ScreenHeight;

	AdjustViewport(false);

	m_ScreenRefreshRate = RefreshRate;

	g_Config.m_GfxScreenWidth = w;
	g_Config.m_GfxScreenHeight = h;
	g_Config.m_GfxScreenRefreshRate = m_ScreenRefreshRate;

	auto OldDpi = m_ScreenHiDPIScale;
	m_ScreenHiDPIScale = m_ScreenWidth / (float)g_Config.m_GfxScreenWidth;

	// A DPI change must notify the listeners, since e.g. video modes
	// currently depend on it.
	if(OldDpi != m_ScreenHiDPIScale)
	{
		for(auto &PropChangedListener : m_vPropChangeListeners)
			PropChangedListener();
	}

	UpdateViewportInternal(0, 0, m_ScreenWidth, m_ScreenHeight, true, SurfaceWidth, SurfaceHeight);

	// The reliable queue orders the viewport update before the next frame.
	KickCommandBuffer();

	if(PrevCanvasWidth != m_ScreenWidth || PrevCanvasHeight != m_ScreenHeight)
	{
		for(auto &ResizeListener : m_vResizeListeners)
			ResizeListener();
	}
}

bool CGraphics_Threaded::IsScreenKeyboardShown()
{
	return m_pBackend->IsScreenKeyboardShown();
}

void CGraphics_Threaded::AddWindowResizeListener(WINDOW_RESIZE_FUNC pFunc)
{
	m_vResizeListeners.emplace_back(pFunc);
}

void CGraphics_Threaded::AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc)
{
	m_vPropChangeListeners.emplace_back(pFunc);
}

int CGraphics_Threaded::GetWindowScreen()
{
	return m_pBackend->GetWindowScreen();
}

void CGraphics_Threaded::WindowDestroyNtf(uint32_t WindowId)
{
	if(!m_pCommandBuffer->IsEmpty())
		DropCurrentFrame();
	m_pBackend->WindowDestroyNtf(WindowId);

	auto pCompletion = std::make_unique<CCommandBuffer::CCompletion>();
	CCommandBuffer::SCommand_WindowDestroyNtf Cmd;
	Cmd.m_WindowId = WindowId;
	Cmd.m_pCompletion = pCompletion.get();
	if(!AddCmdBlocking(Cmd) || !KickCommandBuffer())
	{
		log_error("graphics", "Failed to queue window destroy notification");
		return;
	}
	pCompletion->Wait();
}

void CGraphics_Threaded::WindowCreateNtf(uint32_t WindowId)
{
	if(!m_pCommandBuffer->IsEmpty())
		DropCurrentFrame();
	m_pBackend->WindowCreateNtf(WindowId);

	auto pCompletion = std::make_unique<CCommandBuffer::CCompletion>();
	CCommandBuffer::SCommand_WindowCreateNtf Cmd;
	Cmd.m_WindowId = WindowId;
	Cmd.m_pCompletion = pCompletion.get();
	if(!AddCmdBlocking(Cmd) || !KickCommandBuffer())
	{
		log_error("graphics", "Failed to queue window create notification");
		return;
	}
	pCompletion->Wait();
}

int CGraphics_Threaded::WindowActive()
{
	return m_pBackend->WindowActive();
}

int CGraphics_Threaded::WindowOpen()
{
	return m_pBackend->WindowOpen();
}

void CGraphics_Threaded::SetWindowGrab(bool Grab)
{
	m_pBackend->SetWindowGrab(Grab);
}

void CGraphics_Threaded::NotifyWindow()
{
	m_pBackend->NotifyWindow();
}

void CGraphics_Threaded::ReadPixel(ivec2 Position, ColorRGBA *pColor)
{
	dbg_assert(Position.x >= 0 && Position.x < ScreenWidth(), "ReadPixel position x out of range");
	dbg_assert(Position.y >= 0 && Position.y < ScreenHeight(), "ReadPixel position y out of range");

	m_ReadPixelPosition = Position;
	m_pReadPixelColor = pColor;
}

void CGraphics_Threaded::TakeScreenshot(const char *pFilename)
{
	char aDate[20];
	str_timestamp(aDate, sizeof(aDate));
	str_format(m_aScreenshotName, sizeof(m_aScreenshotName), "screenshots/%s_%s.png", pFilename ? pFilename : "screenshot", aDate);
	m_DoScreenshot = true;
}

void CGraphics_Threaded::TakeCustomScreenshot(const char *pFilename)
{
	str_copy(m_aScreenshotName, pFilename);
	m_DoScreenshot = true;
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::PresentFrame(bool Readback)
{
	if(m_RenderPassActive)
		EndRenderPass();
	const bool Screenshot = m_DoScreenshot && WindowActive();
	m_DoScreenshot = false;
	const bool ReadPixel = m_pReadPixelColor != nullptr;
	if(ReadPixel)
		*m_pReadPixelColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	const bool CaptureFrame = Readback || Screenshot || ReadPixel;
	std::unique_ptr<CCommandBuffer::SImageReadbackResult> pResult;
	bool FramePublished = false;
	const bool ResourcesReady = SubmitReliableCommandBuffer(m_pReliableCommandBuffer);
	if(ResourcesReady && CaptureFrame)
	{
		pResult = std::make_unique<CCommandBuffer::SImageReadbackResult>();
		CCommandBuffer::SCommand_PresentationTarget_Readback ReadbackCmd;
		ReadbackCmd.m_ReadPixel = !Readback && !Screenshot && ReadPixel;
		ReadbackCmd.m_Position = m_ReadPixelPosition;
		ReadbackCmd.m_pResult = pResult.get();
		ReadbackCmd.m_pCompletion = pResult.get();
		CCommandBuffer::SCommand_Swap SwapCmd;
		if(AddCmd(ReadbackCmd) && AddCmd(SwapCmd))
			FramePublished = SubmitFramePacket();
	}
	else if(ResourcesReady)
	{
		CCommandBuffer::SCommand_Swap Cmd;
		AddCmd(Cmd);
		FramePublished = SubmitFramePacket();
	}
	else
		DropCurrentFrame();

	if(ResourcesReady && (FramePublished || m_DropCurrentFrame))
		SubmitDeferredDestroys();
	m_pCommandBuffer->Reset();
	m_DropCurrentFrame = false;
	m_SubmissionTracker.FinishFrame();
	m_RenderPassActive = true;
	m_RenderPassTarget.Invalidate();
	// TODO: Remove when https://github.com/libsdl-org/SDL/issues/5203 is fixed
#ifdef CONF_PLATFORM_MACOS
	if(str_find(GetVersionString(), "Metal"))
		WaitForIdle();
#endif

	if(pResult != nullptr && FramePublished && (Screenshot || ReadPixel))
	{
		pResult->Wait();
		if(ReadPixel)
		{
			if(pResult->m_Ok)
			{
				const size_t X = pResult->m_Image.m_Width == 1 ? 0 : static_cast<size_t>(m_ReadPixelPosition.x);
				const size_t Y = pResult->m_Image.m_Height == 1 ? 0 : static_cast<size_t>(m_ReadPixelPosition.y);
				const size_t Offset = (Y * pResult->m_Image.m_Width + X) * 4;
				const uint8_t *pPixel = pResult->m_Image.m_pData + Offset;
				*m_pReadPixelColor = ColorRGBA(pPixel[0] / 255.0f, pPixel[1] / 255.0f, pPixel[2] / 255.0f, 1.0f);
			}
		}
		if(Screenshot && pResult->m_Ok)
		{
			CImageInfo Image = Readback ? pResult->m_Image.DeepCopy() : std::move(pResult->m_Image);
			m_pEngine->AddJob(std::make_shared<CScreenshotSaveJob>(m_pStorage, m_aScreenshotName, std::move(Image)));
		}
		else if(Screenshot)
			log_error("graphics", "Failed to create screenshot");
	}
	m_pReadPixelColor = nullptr;
	if(Readback && pResult != nullptr && FramePublished)
		return std::make_unique<CTextureReadback>(std::move(pResult));
	return nullptr;
}

void CGraphics_Threaded::Swap()
{
	PresentFrame(false);
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::PresentAndReadbackAsync()
{
	return PresentFrame(true);
}

bool CGraphics_Threaded::SetVSync(bool State)
{
	if(!m_pCommandBuffer)
		return true;

	// add vsync command
	auto pResult = std::make_unique<CCommandBuffer::SCommand_VSync::SResult>();
	CCommandBuffer::SCommand_VSync Cmd;
	Cmd.m_VSync = State ? 1 : 0;
	Cmd.m_pResult = pResult.get();
	Cmd.m_pCompletion = pResult.get();
	if(!AddCmdBlocking(Cmd) || !KickCommandBuffer())
		return false;
	pResult->Wait();

	if(pResult->m_Ok)
	{
		g_Config.m_GfxVsync = State;
	}
	return pResult->m_Ok;
}

bool CGraphics_Threaded::SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend)
{
	if(!m_pCommandBuffer)
		return true;

	// add multisampling command
	auto pResult = std::make_unique<CCommandBuffer::SCommand_MultiSampling::SResult>();
	pResult->m_MultiSamplingCount = MultiSamplingCountBackend;
	CCommandBuffer::SCommand_MultiSampling Cmd;
	Cmd.m_RequestedMultiSamplingCount = ReqMultiSamplingCount;
	Cmd.m_pResult = pResult.get();
	Cmd.m_pCompletion = pResult.get();
	if(!AddCmdBlocking(Cmd) || !KickCommandBuffer())
		return false;
	pResult->Wait();
	MultiSamplingCountBackend = pResult->m_MultiSamplingCount;
	return pResult->m_Ok;
}

// synchronization
void CGraphics_Threaded::InsertSignal(CSemaphore *pSemaphore)
{
	CCommandBuffer::SCommand_Signal Cmd;
	Cmd.m_pSemaphore = pSemaphore;
	if(!AddCmdBlocking(Cmd))
		log_error("graphics", "Failed to queue graphics signal");
}

bool CGraphics_Threaded::IsIdle() const
{
	return m_pBackend->IsIdle();
}

void CGraphics_Threaded::WaitForIdle()
{
	m_pBackend->WaitForIdle();
}

void CGraphics_Threaded::AddWarning(const SWarning &Warning)
{
	const std::unique_lock<std::mutex> Lock(m_WarningsMutex);
	m_vWarnings.emplace_back(Warning);
}

std::optional<SWarning> CGraphics_Threaded::CurrentWarning()
{
	const std::unique_lock<std::mutex> Lock(m_WarningsMutex);
	if(m_vWarnings.empty())
	{
		return std::nullopt;
	}
	else
	{
		std::optional<SWarning> Result = std::make_optional(m_vWarnings[0]);
		m_vWarnings.erase(m_vWarnings.begin());
		return Result;
	}
}

std::optional<int> CGraphics_Threaded::ShowMessageBox(const CMessageBox &MessageBox)
{
	if(m_pBackend == nullptr)
	{
		return std::nullopt;
	}
	m_pBackend->WaitForIdle();
	return m_pBackend->ShowMessageBox(MessageBox);
}

bool CGraphics_Threaded::IsBackendInitialized()
{
	return m_pBackend != nullptr;
}

const char *CGraphics_Threaded::GetVendorString()
{
	return m_pBackend->GetVendorString();
}

const char *CGraphics_Threaded::GetVersionString()
{
	return m_pBackend->GetVersionString();
}

const char *CGraphics_Threaded::GetRendererString()
{
	return m_pBackend->GetRendererString();
}

const char *CGraphics_Threaded::GetFatalError() const
{
	m_FatalError.clear();
	if(m_pBackend == nullptr)
		return m_FatalError.c_str();

	const SGfxErrorContainer &Error = m_pBackend->GetError();
	for(const auto &ErrorString : Error.m_vErrors)
	{
		if(!m_FatalError.empty())
			m_FatalError.append("\n");
		m_FatalError.append(ErrorString);
	}

	const char *pAdvice = nullptr;
	switch(Error.m_ErrorType)
	{
	case GFX_ERROR_TYPE_NONE:
		break;
	case GFX_ERROR_TYPE_INIT:
		pAdvice = Localize("Failed during initialization. Try selecting a different graphics backend in the client settings.", "Graphics error");
		break;
	case GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE:
		[[fallthrough]];
	case GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER:
		[[fallthrough]];
	case GFX_ERROR_TYPE_OUT_OF_MEMORY_STAGING:
		pAdvice = Localize("Out of VRAM. Reduce the number or resolution of loaded textures and other graphics resources.", "Graphics error");
		break;
	case GFX_ERROR_TYPE_RENDER_RECORDING:
		pAdvice = Localize("An error during command recording occurred. Try to update your GPU drivers.", "Graphics error");
		break;
	case GFX_ERROR_TYPE_RENDER_CMD_FAILED:
		pAdvice = Localize("A render command failed. Try to update your GPU drivers.", "Graphics error");
		break;
	case GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED:
		pAdvice = Localize("Submitting the render commands failed. Try to update your GPU drivers.", "Graphics error");
		break;
	case GFX_ERROR_TYPE_SWAP_FAILED:
		pAdvice = Localize("Failed to swap framebuffers. Try to update your GPU drivers.", "Graphics error");
		break;
	case GFX_ERROR_TYPE_UNKNOWN:
		pAdvice = Localize("Unknown error. Try selecting a different graphics backend in the client settings.", "Graphics error");
		break;
	}
	if(pAdvice != nullptr)
	{
		if(!m_FatalError.empty())
			m_FatalError.append("\n");
		m_FatalError.append(pAdvice);
	}
	return m_FatalError.c_str();
}

int CGraphics_Threaded::GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen)
{
	if(g_Config.m_GfxDisplayAllVideoModes)
	{
		const int Count = std::min(std::size(g_aFakeModes), (size_t)MaxModes);
		mem_copy(pModes, g_aFakeModes, Count * sizeof(CVideoMode));
		return Count;
	}

	int NumModes = 0;
	m_pBackend->GetVideoModes(pModes, MaxModes, &NumModes, m_ScreenHiDPIScale, m_DesktopSize.x, m_DesktopSize.y, Screen);
	return NumModes;
}

void CGraphics_Threaded::GetCurrentVideoMode(CVideoMode &CurMode, int Screen)
{
	m_pBackend->GetCurrentVideoMode(CurMode, m_ScreenHiDPIScale, m_DesktopSize.x, m_DesktopSize.y, Screen);
}

extern IEngineGraphics *CreateEngineGraphicsThreaded()
{
	return new CGraphics_Threaded();
}
