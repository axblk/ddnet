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

#include <algorithm>
#include <limits>
#include <memory>

class CSemaphore;

namespace
{
	constexpr int LEGACY_TEXTURE_LAYER_COLUMNS = 16;
	constexpr int LEGACY_TEXTURE_LAYER_ROWS = 16;

}

void CGraphics_Threaded::FlushVertices(bool KeepVertices)
{
	FlushVerticesImpl(KeepVertices, EPipelineProgram::PRIMITIVE, m_aVertices);
}

void CGraphics_Threaded::FlushVerticesTex3D()
{
	FlushVerticesImpl(false, EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY, m_aVerticesTex3D);
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

	m_ScreenWidth = -1;
	m_ScreenHeight = -1;
	m_ScreenRefreshRate = -1;

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

IGraphics::CFrameRenderStats CGraphics_Threaded::FrameRenderStats() const
{
	CFrameRenderStats Stats = m_LastFrameRenderStats;
	const SGpuTiming GpuTiming = m_pBackend->GpuTiming();
	Stats.m_GpuTimeNanoseconds = GpuTiming.m_TimeNanoseconds;
	Stats.m_GpuSample = GpuTiming.m_Sample;
	Stats.m_GpuTimingSupported = GpuTiming.m_Supported;
	return Stats;
}

void CGraphics_Threaded::SetRenderStatsEnabled(bool Enabled)
{
	m_RenderStatsEnabled = Enabled;
	m_pBackend->SetGpuTimingEnabled(Enabled);
	if(Enabled)
	{
		m_CurrentFrameRenderStats = {};
		m_LastFrameRenderStats = {};
	}
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
	{
		ReportLostDestroy("texture");
		return;
	}
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
	if((Flags & IGraphics::TEXLOAD_LAYERED) != 0)
		Desc.m_Layering = IGraphics::ETextureLayering::LAYERED;
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
		(pInitialData == nullptr && !Desc.HasUsage(TEXTURE_USAGE_COLOR_TARGET)))
		return {};

	uint8_t *pOwnedData = nullptr;
	if(pInitialData != nullptr)
	{
		const size_t PixelSize = IGraphics::PixelSize(Desc.m_Format);
		if(PixelSize == 0 || Desc.m_Width > std::numeric_limits<size_t>::max() / PixelSize || Desc.m_Height > std::numeric_limits<size_t>::max() / (Desc.m_Width * PixelSize))
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

namespace
{
	class CTextureReadback final : public IGraphics::ITextureReadback
	{
		CGraphics_Threaded *m_pGraphics;
		std::unique_ptr<CCommandBuffer::SImageReadbackResult> m_pResult;
		bool m_Waited = false;

		// The backend may still be waiting for the device, and it only looks
		// for that while it runs commands. So one is sent before blocking.
		void WaitForResult()
		{
			if(!m_pResult->IsComplete())
				m_pGraphics->FinishReadbacks();
			m_pResult->Wait();
		}

	public:
		CTextureReadback(CGraphics_Threaded *pGraphics, std::unique_ptr<CCommandBuffer::SImageReadbackResult> pResult) :
			m_pGraphics(pGraphics),
			m_pResult(std::move(pResult))
		{
		}

		~CTextureReadback() override
		{
			if(!m_Waited)
				WaitForResult();
		}

		bool IsReady() const override
		{
			return m_pResult->IsComplete();
		}

		bool Wait(CImageInfo &Image) override
		{
			if(m_Waited)
				return false;
			WaitForResult();
			m_Waited = true;
			if(!m_pResult->m_Ok)
				return false;
			Image = std::move(m_pResult->m_Image);
			return true;
		}
	};
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::ReadTextureAsync(CTextureHandle Texture, CImageInfo &&Recycled)
{
	const CTextureDesc *pDesc = FindTextureDesc(Texture);
	if(m_Drawing != EDrawing::NONE || m_RenderPassActive || pDesc == nullptr || !pDesc->HasUsage(TEXTURE_USAGE_COLOR_TARGET) || !pDesc->HasUsage(TEXTURE_USAGE_COPY_SOURCE))
		return nullptr;
	if(!SubmitReliableCommandBuffer(m_pReliableCommandBuffer) || !SubmitFramePacket())
		return nullptr;

	auto pResult = std::make_unique<CCommandBuffer::SImageReadbackResult>();
	pResult->m_Image = std::move(Recycled);
	CCommandBuffer::SCommand_Texture_Readback Cmd;
	Cmd.m_Texture = Texture;
	Cmd.m_pResult = pResult.get();
	Cmd.m_pCompletion = pResult.get();
	if(!AddCmd(Cmd) || !KickCommandBuffer())
		return nullptr;
	return std::make_unique<CTextureReadback>(this, std::move(pResult));
}

bool CGraphics_Threaded::BeginOffscreenFrame(CTextureHandle Texture)
{
	if(m_VirtualScreen.IsValid() && m_OffscreenFrameTarget == m_VirtualScreen && Texture != m_VirtualScreen)
	{
		// The surface-less client keeps a frame open at all times. A recorder
		// that wants its own target gets it; the frame it replaces has nothing
		// in it yet, because this runs before anything is drawn.
		FinishOffscreenFrame(false, CImageInfo(), CTextureHandle(), EPlanarYuvFormat::NV12);
	}
	const CTextureDesc *pDesc = FindTextureDesc(Texture);
	if(m_OffscreenFrameTarget.IsValid() || m_Drawing != EDrawing::NONE || pDesc == nullptr || !pDesc->HasUsage(TEXTURE_USAGE_COLOR_TARGET) || !pDesc->HasUsage(TEXTURE_USAGE_COPY_SOURCE) || pDesc->m_Width > std::numeric_limits<int>::max() || pDesc->m_Height > std::numeric_limits<int>::max())
		return false;
	if(!HasPresentationSurface() && !m_pCommandBuffer->IsEmpty())
	{
		// The surface-less client can accumulate an implicit loading frame before
		// the export target exists. It has no presentation target and is obsolete.
		m_pCommandBuffer->Reset();
		m_DropCurrentFrame = false;
	}

	m_OffscreenFrameTarget = Texture;
	m_RenderWidth = static_cast<int>(pDesc->m_Width);
	m_RenderHeight = static_cast<int>(pDesc->m_Height);
	CRenderPassDesc Pass;
	Pass.m_ColorTarget = Texture;
	if(!BeginRenderPass(Pass))
	{
		m_OffscreenFrameTarget.Invalidate();
		m_RenderWidth = 0;
		m_RenderHeight = 0;
		return false;
	}
	return true;
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::EndOffscreenFrame(CImageInfo &&Recycled, CTextureHandle YuvTarget, EPlanarYuvFormat YuvFormat)
{
	return FinishOffscreenFrame(true, std::move(Recycled), YuvTarget, YuvFormat);
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::FinishOffscreenFrame(bool WantImage, CImageInfo &&Recycled, CTextureHandle YuvTarget, EPlanarYuvFormat YuvFormat)
{
	if(!m_OffscreenFrameTarget.IsValid())
		return nullptr;
	const CTextureHandle Target = m_OffscreenFrameTarget;
	const bool CanFinish = m_Drawing == EDrawing::NONE && m_RenderPassActive && m_RenderPassTarget == Target;
	const bool FrameEnded = CanFinish && EndRenderPass();
	m_OffscreenFrameTarget.Invalidate();
	m_RenderWidth = 0;
	m_RenderHeight = 0;

	// The conversion is a second pass over the finished frame, so it runs
	// after the frame's own pass closed and before anything is read back.
	CTextureHandle ReadbackTarget = Target;
	const CTextureDesc *pYuvDesc = FindTextureDesc(YuvTarget);
	if(FrameEnded && pYuvDesc != nullptr && m_Capabilities.m_PlanarYuvConversion)
	{
		CRenderPassDesc Pass;
		Pass.m_ColorTarget = YuvTarget;
		m_RenderWidth = static_cast<int>(pYuvDesc->m_Width);
		m_RenderHeight = static_cast<int>(pYuvDesc->m_Height);
		if(BeginRenderPass(Pass))
		{
			// The pass has to be closed whether the conversion worked or not,
			// because the frame that is read back next expects no pass to be
			// open and the packed frame is only usable if both parts did work.
			const bool Converted = ConvertTextureToPlanarYuv(Target, YuvFormat);
			if(EndRenderPass() && Converted)
				ReadbackTarget = YuvTarget;
		}
		m_RenderWidth = 0;
		m_RenderHeight = 0;
	}
	std::unique_ptr<ITextureReadback> pReadback;
	bool FramePublished = false;
	m_FramePacketEndsFrame = true;
	if(FrameEnded && WantImage)
	{
		pReadback = ReadTextureAsync(ReadbackTarget, std::move(Recycled));
		FramePublished = pReadback != nullptr;
	}
	else if(FrameEnded)
	{
		// Nobody wants the picture, so the frame is only sent; a dropped readback
		// would wait for the device. Without a surface the swap still marks the
		// end of the frame for the backend.
		if(!HasPresentationSurface())
		{
			CCommandBuffer::SCommand_Swap SwapCmd;
			AddCmd(SwapCmd);
		}
		FramePublished = SubmitReliableCommandBuffer(m_pReliableCommandBuffer) && SubmitFramePacket();
	}
	m_FramePacketEndsFrame = false;
	if(!FramePublished)
		DropCurrentFrame();
	else
		SubmitDeferredDestroys();
	m_pCommandBuffer->Reset();
	m_DropCurrentFrame = false;
	// The screen needs a pass of its own again, or everything drawn between two
	// export frames is dropped.
	m_RenderPassActive = false;
	m_RenderPassTarget.Invalidate();
	if(HasPresentationSurface())
	{
		CRenderPassDesc PresentationPass;
		BeginRenderPass(PresentationPass);
	}
	return pReadback;
}

IGraphics::CTextureHandle CGraphics_Threaded::LoadTextureRaw(const CImageInfo &Image, int Flags, const char *pTexName)
{
	const CTextureDesc Desc = LoadTextureDesc(Image.m_Width, Image.m_Height, Flags);
	if(!Desc.IsValid())
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
	if(!Desc.IsValid())
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

bool CGraphics_Threaded::UpdateTexture(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, const void *pData)
{
	const CTextureDesc *pDesc = FindTextureDesc(Texture);
	if(pDesc == nullptr || pDesc->m_Format != Format || pDesc->m_Mipmaps != ETextureMipmaps::NONE || !pDesc->m_Create2D || pDesc->m_Layering != ETextureLayering::NONE)
		return false;
	if(Region.m_X > pDesc->m_Width || Region.m_Width > pDesc->m_Width - Region.m_X || Region.m_Y > pDesc->m_Height || Region.m_Height > pDesc->m_Height - Region.m_Y)
		return false;
	return UpdateTextureInternal(Texture, Region, Format, const_cast<uint8_t *>(static_cast<const uint8_t *>(pData)), false);
}

bool CGraphics_Threaded::UpdateTextureInternal(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, uint8_t *pData, bool IsMovedPointer)
{
	if(!m_TextureHandles.IsAllocated(Texture))
		return false;
	// A texture that is being rendered into is in a layout the upload path does
	// not expect, and no backend breaks the pass open to change it. The caller
	// has to end the pass before it writes into its own target.
	if(m_RenderPassActive && m_RenderPassTarget == Texture)
		return false;
	const size_t PixelSize = IGraphics::PixelSize(Format);
	if(PixelSize == 0)
		return false;
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

bool CGraphics_Threaded::SubmitReliableCommandBuffer(CCommandBuffer *pCommandBuffer)
{
	if(pCommandBuffer == nullptr || pCommandBuffer->IsEmpty())
		return true;
	dbg_assert(pCommandBuffer == m_pReliableCommandBuffer || pCommandBuffer == m_pDeferredDestroyCommandBuffer, "graphics: submitted unknown reliable command buffer");
	pCommandBuffer->SetSubmissionInfo({CCommandBuffer::ECommandChannel::RELIABLE});

	const CFrameRenderStats RenderStats = m_RenderStatsEnabled ? pCommandBuffer->RenderStats() : CFrameRenderStats{};
	if(!m_pBackend->RunBufferQueued(pCommandBuffer))
	{
		// Nothing will ever execute these commands anymore, so drop them instead of
		// leaving them behind pointing at results their callers are about to destroy.
		pCommandBuffer->SignalCompletions();
		pCommandBuffer->FreeExternalData();
		pCommandBuffer->Reset();
		return false;
	}
	if(m_RenderStatsEnabled)
		m_CurrentFrameRenderStats += RenderStats;
	CollectBackendQueueWarnings();
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
	// Without a surface the swap is just the frame boundary.
	const bool Presented = HasPresentationSurface() && m_pCommandBuffer->ContainsCommand(CCommandBuffer::CMD_SWAP);
	m_pCommandBuffer->SetSubmissionInfo({CCommandBuffer::ECommandChannel::FRAME, m_FramePacketEndsFrame || Presented, Presented});

	const CFrameRenderStats RenderStats = m_RenderStatsEnabled ? m_pCommandBuffer->RenderStats() : CFrameRenderStats{};
	const bool EndsFrame = m_pCommandBuffer->SubmissionInfo().m_EndsFrame;
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
	if(m_RenderStatsEnabled)
	{
		m_CurrentFrameRenderStats += RenderStats;
		if(EndsFrame)
		{
			m_LastFrameRenderStats = m_CurrentFrameRenderStats;
			m_CurrentFrameRenderStats = {};
		}
	}
	CollectBackendQueueWarnings();
	m_pCommandBuffer->Reset();
	return true;
}

void CGraphics_Threaded::RecycleRetiredHandles()
{
	for(const CTextureHandle Handle : m_vRetiredTextureHandles)
		dbg_assert(m_TextureHandles.Recycle(Handle), "graphics: failed to recycle retired texture handle");
	for(const CBufferHandle Handle : m_vRetiredBufferHandles)
		dbg_assert(m_BufferHandles.Recycle(Handle), "graphics: failed to recycle retired buffer handle");
	m_vRetiredTextureHandles.clear();
	m_vRetiredBufferHandles.clear();
}

void CGraphics_Threaded::ReportLostDestroy(const char *pWhat)
{
	if(m_ReportedLostDestroy)
		return;
	m_ReportedLostDestroy = true;
	log_error("graphics", "Could not queue a %s destroy, the resource stays allocated until the backend shuts down", pWhat);
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

void CGraphics_Threaded::FinishReadbacks()
{
	CCommandBuffer::SCommand_FinishReadbacks Cmd;
	if(AddCmd(Cmd))
		KickCommandBuffer();
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
		const CTextureDesc *pDesc = FindTextureDesc(EffectiveDesc.m_ColorTarget);
		if(pDesc == nullptr || !pDesc->HasUsage(TEXTURE_USAGE_COLOR_TARGET))
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

bool CGraphics_Threaded::DrawFullscreenTexture(CTextureHandle Source, EPipelineProgram Program, SGraphicsColor Color, uint8_t RequiredUsage, bool UseCurrentClip)
{
	const CTextureDesc *pDesc = FindTextureDesc(Source);
	if(m_Drawing != EDrawing::NONE || !m_RenderPassActive || pDesc == nullptr || Source == m_RenderPassTarget || (pDesc->m_Usage & RequiredUsage) != RequiredUsage)
		return false;
	if(!ReportRejectedDraw(TextureKindMismatch(Program, Source), Program, NO_LAYOUT))
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
	Cmd.m_Program = Program;
	Cmd.m_PrimitiveType = EPrimitiveType::QUADS;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;
	Cmd.m_VertexCount = aVertices.size();
	void *pData = AllocCommandBufferData(sizeof(aVertices));
	mem_copy(pData, aVertices.data(), sizeof(aVertices));
	Cmd.m_VertexData = {pData, sizeof(aVertices)};
	return AddCmd(Cmd);
}

bool CGraphics_Threaded::BlitTexture(CTextureHandle Source, bool UseCurrentClip)
{
	return DrawFullscreenTexture(Source, EPipelineProgram::PRIMITIVE, {255, 255, 255, 255}, TEXTURE_USAGE_SAMPLED, UseCurrentClip);
}

bool CGraphics_Threaded::ConvertTextureToPlanarYuv(CTextureHandle Source, EPlanarYuvFormat Format)
{
	if(!m_Capabilities.m_PlanarYuvConversion)
		return false;
	// The layout rides along in the vertex color, the way the blur passes its
	// axis, so that both formats share one pipeline.
	SGraphicsColor Layout;
	if(Format == EPlanarYuvFormat::NV12)
		Layout = {0, 0, 0, 255};
	else if(Format == EPlanarYuvFormat::I420)
		Layout = {255, 255, 255, 255};
	else
		return false;
	return DrawFullscreenTexture(Source, EPipelineProgram::PLANAR_YUV, Layout, TEXTURE_USAGE_SAMPLED | TEXTURE_USAGE_COLOR_TARGET);
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
	return DrawFullscreenTexture(Source, EPipelineProgram::BLUR, Axis, TEXTURE_USAGE_SAMPLED | TEXTURE_USAGE_COLOR_TARGET);
}

void CGraphics_Threaded::QuadsTex3DDrawTL(const CQuadItem *pArray, int Num)
{
	const int VertNum = 4;
	const CTextureHandle Texture = m_State.m_Texture;
	// A texture that is still loading, or that failed to, is an invalid handle
	// and a normal state of the world - the draw just has nothing to show
	// this frame. A valid handle that is not layered is a programming error.
	if(!Texture.IsValid())
		return;
	const CTextureDesc *pDesc = FindTextureDesc(Texture);
	dbg_assert(pDesc != nullptr && pDesc->m_Layering != ETextureLayering::NONE, "Layered texture descriptor is missing");
	const float CurIndex = m_CurIndex;

	for(int i = 0; i < Num; ++i)
	{
		for(int n = 0; n < VertNum; ++n)
		{
			m_aVerticesTex3D[m_NumVertices + VertNum * i + n].m_Tex.w = CurIndex;
		}
	}

	QuadsDrawTLImpl(m_aVerticesTex3D, pArray, Num);
}

void CGraphics_Threaded::RenderTileLayer(CBufferHandle VertexBuffer, EVertexLayout Layout, const ColorRGBA &Color, const uint32_t *pFirstIndices, const uint32_t *pIndexCounts, size_t RangeCount, const vec2 &Offset, const vec2 &Scale)
{
	if(RangeCount == 0 || !VertexBuffer.IsValid())
		return;

	// One command per visible span. A generic multi-draw range is only worth
	// adding if real maps turn out to exceed the fixed command arena.
	for(size_t i = 0; i < RangeCount; ++i)
	{
		if(pIndexCounts[i] == 0)
			continue;
		CCommandBuffer::SCommand_DrawIndexed Cmd;
		Cmd.m_State = m_State;
		Cmd.m_Program = EPipelineProgram::ARRAY_COLOR;
		Cmd.m_IndexCount = pIndexCounts[i];
		Cmd.m_IndexOffset = static_cast<size_t>(pFirstIndices[i]) * sizeof(uint32_t);
		Cmd.m_VertexBuffer = VertexBuffer;
		Cmd.m_Layout = Layout;
		Cmd.m_IndexBuffer = m_QuadIndexBuffer;
		if(!SubmitIndexedDraw<CCommandBuffer::SDrawDataArrayColor>(Cmd, 1, false, [&](CCommandBuffer::SDrawDataArrayColor *pData) {
			   pData->m_Color = Color;
			   pData->m_Offset = Offset;
			   pData->m_Scale = Scale;
		   }))
			return;
	}
}

void CGraphics_Threaded::RenderQuadLayer(CBufferHandle VertexBuffer, EVertexLayout Layout, SQuadRenderInfo *pQuadInfo, size_t QuadNum, int QuadOffset, bool Grouped)
{
	if(QuadNum == 0 || !VertexBuffer.IsValid())
		return;
	if(QuadOffset < 0 || QuadNum > std::numeric_limits<uint32_t>::max() / 6)
	{
		log_error("graphics", "Invalid quad layer draw range. QuadCount=%" PRIzu " QuadOffset=%d", QuadNum, QuadOffset);
		return;
	}

	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = m_State;
	Cmd.m_Program = Grouped ? EPipelineProgram::QUAD_SHARED : EPipelineProgram::QUAD_PER_ITEM;
	Cmd.m_IndexCount = static_cast<uint32_t>(QuadNum * 6);
	Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
	Cmd.m_VertexBuffer = VertexBuffer;
	Cmd.m_Layout = Layout;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;

	// Grouped quads share one transform for the whole draw; ungrouped ones
	// carry one each, which is an array the shader indexes per instance.
	const size_t DataCount = Grouped ? 1 : QuadNum;
	SubmitIndexedDraw<CCommandBuffer::SDrawDataQuadTransform>(Cmd, DataCount, !Grouped, [&](CCommandBuffer::SDrawDataQuadTransform *pData) {
		for(size_t i = 0; i < DataCount; ++i)
		{
			pData[i].m_Color = pQuadInfo[i].m_Color;
			pData[i].m_Offset = pQuadInfo[i].m_Offsets;
			pData[i].m_Rotation = pQuadInfo[i].m_Rotation;
			pData[i].m_Padding = 0.0f;
		}
	});
}

void CGraphics_Threaded::RenderText(CBufferHandle VertexBuffer, int TextQuadNum, int TextureSize, CTextureHandle Texture, const ColorRGBA &TextColor, const ColorRGBA &TextOutlineColor)
{
	if(!VertexBuffer.IsValid() || TextQuadNum <= 0)
		return;
	dbg_assert(m_TextureHandles.IsAllocated(Texture), "Cannot render text with stale texture handle");
	if(TextureSize <= 0 || static_cast<uint32_t>(TextQuadNum) > std::numeric_limits<uint32_t>::max() / 6)
	{
		log_error("graphics", "Invalid text draw. QuadCount=%d TextureSize=%d", TextQuadNum, TextureSize);
		return;
	}

	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = m_State;
	Cmd.m_State.m_Texture = Texture;
	Cmd.m_Program = EPipelineProgram::DUAL_ATLAS_COMPOSITE;
	Cmd.m_VertexBuffer = VertexBuffer;
	Cmd.m_Layout = EVertexLayout::POSITION_TEXCOORD_COLOR;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;
	Cmd.m_IndexCount = static_cast<uint32_t>(TextQuadNum) * 6;
	Cmd.m_IndexOffset = 0;
	SubmitIndexedDraw<CCommandBuffer::SDrawDataDualAtlas>(Cmd, 1, false, [&](CCommandBuffer::SDrawDataDualAtlas *pData) {
		pData->m_TextureSize = TextureSize;
		pData->m_PrimaryColor = TextColor;
		pData->m_SecondaryColor = TextOutlineColor;
	});
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
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];
	if(!Container.m_vQuads.empty())
	{
		bool BufferReady;
		if(!Container.m_QuadBuffer.IsValid())
		{
			size_t UploadDataSize = Container.m_vQuads.size() * sizeof(SQuadContainer::SQuad);
			Container.m_QuadBuffer = CreateBufferObject({reinterpret_cast<const uint8_t *>(Container.m_vQuads.data()), UploadDataSize});
			BufferReady = Container.m_QuadBuffer.IsValid();
		}
		else
		{
			size_t UploadDataSize = Container.m_vQuads.size() * sizeof(SQuadContainer::SQuad);
			BufferReady = RecreateBufferObject(Container.m_QuadBuffer, {reinterpret_cast<const uint8_t *>(Container.m_vQuads.data()), UploadDataSize});
		}
		if(!BufferReady)
			return;
		Container.m_UploadedQuadCount = Container.m_vQuads.size();
	}
}

int CGraphics_Threaded::QuadContainerAddQuads(int ContainerIndex, CQuadItem *pArray, int Num)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if((int)Container.m_vQuads.size() > Num + CCommandBuffer::MAX_VERTICES)
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

	if((int)Container.m_vQuads.size() > Num + CCommandBuffer::MAX_VERTICES)
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
	if(Container.m_QuadBuffer.IsValid())
	{
		DeleteBufferObject(Container.m_QuadBuffer);
		if(Container.m_QuadBuffer.IsValid())
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
	if(m_vQuadContainers[ContainerIndex].m_QuadBuffer.IsValid())
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
		QuadDrawNum = (int)Container.m_UploadedQuadCount - QuadOffset;

	if(QuadDrawNum <= 0 || (int)Container.m_vQuads.size() < QuadOffset + QuadDrawNum)
		return;

	if(!Container.m_QuadBuffer.IsValid() || Container.m_UploadedQuadCount < static_cast<size_t>(QuadOffset) + static_cast<size_t>(QuadDrawNum))
		return;

	if(ChangeWrapMode)
		WrapClamp();

	CCommandBuffer::SCommand_DrawIndexed Cmd;
	Cmd.m_State = m_State;
	Cmd.m_Program = EPipelineProgram::PRIMITIVE;
	Cmd.m_IndexCount = static_cast<uint32_t>(QuadDrawNum) * 6;
	Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
	Cmd.m_VertexBuffer = Container.m_QuadBuffer;
	Cmd.m_Layout = EVertexLayout::POSITION_TEXCOORD_COLOR;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;

	if(CheckIndexedDraw(Cmd))
		AddCmd(Cmd);
	WrapNormal();
}

void CGraphics_Threaded::RenderQuadContainerEx(int ContainerIndex, int QuadOffset, int QuadDrawNum, float X, float Y, float ScaleX, float ScaleY)
{
	SQuadContainer &Container = m_vQuadContainers[ContainerIndex];

	if((int)Container.m_vQuads.size() < QuadOffset + 1)
		return;

	if(QuadDrawNum == -1)
		QuadDrawNum = (int)Container.m_UploadedQuadCount - QuadOffset;

	if(QuadDrawNum <= 0 || !Container.m_QuadBuffer.IsValid() || Container.m_UploadedQuadCount < static_cast<size_t>(QuadOffset) + static_cast<size_t>(QuadDrawNum))
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

	Cmd.m_Program = EPipelineProgram::PRIMITIVE_UNIFORM_COLOR;
	Cmd.m_IndexCount = static_cast<uint32_t>(QuadDrawNum) * 6;
	Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
	Cmd.m_VertexBuffer = Container.m_QuadBuffer;
	Cmd.m_Layout = EVertexLayout::POSITION_TEXCOORD_COLOR;
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
	mem_copy(pDrawData, &DrawData, sizeof(DrawData));
	Cmd.m_DrawData = {pDrawData, sizeof(DrawData)};
	AddCmd(Cmd);
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

	if(!Container.m_QuadBuffer.IsValid() || Container.m_UploadedQuadCount <= static_cast<size_t>(QuadOffset))
		return;

	WrapClamp();
	SQuadContainer::SQuad &Quad = Container.m_vQuads[0];
	CCommandBuffer::SCommand_DrawIndexed Cmd;
	CCommandBuffer::SDrawDataPrimitiveInstanced DrawData;

	Cmd.m_State = m_State;
	Cmd.m_Program = EPipelineProgram::PRIMITIVE_INSTANCED;
	Cmd.m_IndexCount = 6;
	Cmd.m_IndexOffset = static_cast<size_t>(QuadOffset) * 6 * sizeof(uint32_t);
	Cmd.m_InstanceCount = static_cast<uint32_t>(DrawCount);
	Cmd.m_VertexBuffer = Container.m_QuadBuffer;
	Cmd.m_Layout = EVertexLayout::POSITION_TEXCOORD_COLOR;
	Cmd.m_IndexBuffer = m_QuadIndexBuffer;

	DrawData.m_Color.r = (float)m_aColor[0].r / 255.f;
	DrawData.m_Color.g = (float)m_aColor[0].g / 255.f;
	DrawData.m_Color.b = (float)m_aColor[0].b / 255.f;
	DrawData.m_Color.a = (float)m_aColor[0].a / 255.f;

	// rotate before positioning
	DrawData.m_RotationCenter.x = Quad.m_aVertices[0].m_Pos.x + (Quad.m_aVertices[1].m_Pos.x - Quad.m_aVertices[0].m_Pos.x) / 2.f;
	DrawData.m_RotationCenter.y = Quad.m_aVertices[0].m_Pos.y + (Quad.m_aVertices[2].m_Pos.y - Quad.m_aVertices[0].m_Pos.y) / 2.f;

	void *pDrawData = AllocCommandBufferData(sizeof(DrawData));
	mem_copy(pDrawData, &DrawData, sizeof(DrawData));
	Cmd.m_DrawData = {pDrawData, sizeof(DrawData)};
	SubmitIndexedDraw<CCommandBuffer::SInstanceDataPositionScaleRotation>(Cmd, DrawCount, true, [&](CCommandBuffer::SInstanceDataPositionScaleRotation *pInstanceData) {
		for(int i = 0; i < DrawCount; ++i)
		{
			pInstanceData[i].m_Position = pRenderInfo[i].m_Pos;
			pInstanceData[i].m_Scale = pRenderInfo[i].m_Scale;
			pInstanceData[i].m_Rotation = pRenderInfo[i].m_Rotation;
		}
	});
	WrapNormal();
}

void *CGraphics_Threaded::AllocReliableCommandBufferData(size_t AllocSize)
{
	void *pData = m_pReliableCommandBuffer->AllocData(AllocSize);
	if(pData == nullptr && SubmitReliableCommandBuffer(m_pReliableCommandBuffer))
		pData = m_pReliableCommandBuffer->AllocData(AllocSize);
	return pData;
}

CGraphics_Threaded::CBufferHandle CGraphics_Threaded::CreateBufferObjectInternal(size_t UploadDataSize, void *pUploadData, EBufferLifetime Lifetime, bool IsMovedPointer, EBufferUsage Usage)
{
	CBufferHandle Buffer = m_BufferHandles.Allocate();

	dbg_assert(Lifetime != EBufferLifetime::FRAME || (UploadDataSize <= CCommandBuffer::MAX_VERTICES * std::max(sizeof(CCommandBuffer::SVertexTex3DStream), sizeof(CCommandBuffer::SVertexTex3D))),
		"A buffer that only has to last a frame is bounded by the largest streamed vertex format times MAX_VERTICES");

	CCommandBuffer::SCommand_CreateBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	Cmd.m_Desc.m_Size = UploadDataSize;
	Cmd.m_Desc.m_Lifetime = Lifetime;
	Cmd.m_Desc.m_Usage = Usage;

	if(IsMovedPointer)
	{
		Cmd.m_pUploadData = pUploadData;
		Cmd.m_DeletePointer = true;
	}
	else
	{
		Cmd.m_pUploadData = UploadDataSize <= CMD_BUFFER_DATA_BUFFER_SIZE ? AllocReliableCommandBufferData(UploadDataSize) : nullptr;
		Cmd.m_DeletePointer = Cmd.m_pUploadData == nullptr;
		if(Cmd.m_DeletePointer)
		{
			Cmd.m_pUploadData = malloc(UploadDataSize);
			if(Cmd.m_pUploadData == nullptr && UploadDataSize != 0)
			{
				m_BufferHandles.Release(&Buffer);
				return Buffer;
			}
		}
	}

	if(!AddCmd(Cmd, [&] {
		   if(Cmd.m_DeletePointer)
			   return true;
		   Cmd.m_pUploadData = m_pReliableCommandBuffer->AllocData(UploadDataSize);
		   return Cmd.m_pUploadData != nullptr;
	   }))
	{
		// A pointer that was moved in belongs to the command, and the command
		// is not going to run, so this is the only place left that can free it.
		if(Cmd.m_DeletePointer)
			free(Cmd.m_pUploadData);
		m_BufferHandles.Release(&Buffer);
		return Buffer;
	}

	if(!IsMovedPointer)
		mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
	return Buffer;
}

CGraphics_Threaded::CBufferHandle CGraphics_Threaded::CreateBufferObject(std::span<const uint8_t> Data, EBufferLifetime Lifetime)
{
	return CreateBufferObjectInternal(Data.size(), const_cast<uint8_t *>(Data.data()), Lifetime, false, EBufferUsage::VERTEX);
}

CGraphics_Threaded::CBufferHandle CGraphics_Threaded::CreateBufferObjectMoved(void *pData, size_t Size, EBufferLifetime Lifetime)
{
	return CreateBufferObjectInternal(Size, pData, Lifetime, true, EBufferUsage::VERTEX);
}

bool CGraphics_Threaded::RecreateBufferObjectInternal(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, EBufferLifetime Lifetime, bool IsMovedPointer, EBufferUsage Usage)
{
	dbg_assert(m_BufferHandles.IsAllocated(Buffer), "Cannot recreate stale buffer handle");
	CCommandBuffer::SCommand_RecreateBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	Cmd.m_Desc.m_Size = UploadDataSize;
	Cmd.m_Desc.m_Lifetime = Lifetime;
	Cmd.m_Desc.m_Usage = Usage;

	dbg_assert(Lifetime != EBufferLifetime::FRAME || (UploadDataSize <= CCommandBuffer::MAX_VERTICES * std::max(sizeof(CCommandBuffer::SVertexTex3DStream), sizeof(CCommandBuffer::SVertexTex3D))),
		"A buffer that only has to last a frame is bounded by the largest streamed vertex format times MAX_VERTICES");

	if(IsMovedPointer)
	{
		Cmd.m_pUploadData = pUploadData;
		Cmd.m_DeletePointer = true;
	}
	else
	{
		Cmd.m_pUploadData = UploadDataSize <= CMD_BUFFER_DATA_BUFFER_SIZE ? AllocReliableCommandBufferData(UploadDataSize) : nullptr;
		Cmd.m_DeletePointer = Cmd.m_pUploadData == nullptr;
		if(Cmd.m_DeletePointer)
		{
			Cmd.m_pUploadData = malloc(UploadDataSize);
			if(Cmd.m_pUploadData == nullptr && UploadDataSize != 0)
				return false;
		}
	}

	if(!AddCmd(Cmd, [&] {
		   if(Cmd.m_DeletePointer)
			   return true;
		   Cmd.m_pUploadData = m_pReliableCommandBuffer->AllocData(UploadDataSize);
		   return Cmd.m_pUploadData != nullptr;
	   }))
	{
		// See CreateBufferObjectInternal: a moved pointer has no other owner.
		if(Cmd.m_DeletePointer)
			free(Cmd.m_pUploadData);
		return false;
	}

	if(!IsMovedPointer)
		mem_copy(Cmd.m_pUploadData, pUploadData, UploadDataSize);
	return true;
}

bool CGraphics_Threaded::RecreateBufferObject(CBufferHandle Buffer, std::span<const uint8_t> Data, EBufferLifetime Lifetime)
{
	return RecreateBufferObjectInternal(Buffer, Data.size(), const_cast<uint8_t *>(Data.data()), Lifetime, false, EBufferUsage::VERTEX);
}

bool CGraphics_Threaded::RecreateBufferObjectMoved(CBufferHandle Buffer, void *pData, size_t Size, EBufferLifetime Lifetime)
{
	return RecreateBufferObjectInternal(Buffer, Size, pData, Lifetime, true, EBufferUsage::VERTEX);
}

void CGraphics_Threaded::DeleteBufferObject(CBufferHandle &Buffer)
{
	if(!Buffer.IsValid())
		return;
	dbg_assert(m_BufferHandles.IsAllocated(Buffer), "Cannot delete stale buffer handle");

	CCommandBuffer::SCommand_DeleteBufferObject Cmd;
	Cmd.m_Buffer = Buffer;
	if(!AddCmd(Cmd))
	{
		ReportLostDestroy("buffer object");
		return;
	}

	const CBufferHandle RetiredHandle = Buffer;
	dbg_assert(m_BufferHandles.Retire(&Buffer), "Cannot retire stale buffer handle");
	m_vRetiredBufferHandles.push_back(RetiredHandle);
}

bool CGraphics_Threaded::IndicesNumRequiredNotify(unsigned int RequiredIndicesCount)
{
	if(RequiredIndicesCount <= m_QuadIndexCount)
		return true;
	if(RequiredIndicesCount % 6 != 0)
		return false;

	// Written straight into the command's block, without a second copy.
	const size_t DataSize = (size_t)RequiredIndicesCount * sizeof(uint32_t);
	uint32_t *pIndices = static_cast<uint32_t *>(malloc(DataSize));
	if(pIndices == nullptr)
		return false;
	uint32_t Vertex = 0;
	for(unsigned int Index = 0; Index < RequiredIndicesCount; Index += 6, Vertex += 4)
	{
		pIndices[Index] = Vertex;
		pIndices[Index + 1] = Vertex + 1;
		pIndices[Index + 2] = Vertex + 2;
		pIndices[Index + 3] = Vertex;
		pIndices[Index + 4] = Vertex + 2;
		pIndices[Index + 5] = Vertex + 3;
	}

	const bool Ready = m_QuadIndexBuffer.IsValid() ?
				   RecreateBufferObjectInternal(m_QuadIndexBuffer, DataSize, pIndices, EBufferLifetime::PERSISTENT, true, EBufferUsage::INDEX) :
				   (m_QuadIndexBuffer = CreateBufferObjectInternal(DataSize, pIndices, EBufferLifetime::PERSISTENT, true, EBufferUsage::INDEX)).IsValid();
	if(Ready)
		m_QuadIndexCount = RequiredIndicesCount;
	return Ready;
}

void CGraphics_Threaded::AdjustViewport(bool SendViewportChangeToBackend)
{
	// exclude the area covered by the display cutout, as nothing rendered there is visible
	m_ViewportX = m_InsetLeft;
	m_ScreenWidth = m_DrawableWidth - m_InsetLeft - m_InsetRight;
	m_ScreenHeight = m_DrawableHeight;

	// A frame without a window is drawn into a texture, so it can be no bigger
	// than one.
	const uint32_t MaxDimension = m_Capabilities.m_MaxTextureDimension;
	if(!HasPresentationSurface() && MaxDimension > 0)
	{
		m_ScreenWidth = std::min<int>(m_ScreenWidth, MaxDimension);
		m_ScreenHeight = std::min<int>(m_ScreenHeight, MaxDimension);
	}

	// adjust the viewport to only allow certain aspect ratios
	// keep this in sync with backend_vulkan GetSwapImageSize's check
	if(m_ScreenHeight > 4 * m_ScreenWidth / 5)
	{
		m_IsForcedViewport = true;
		m_ScreenHeight = 4 * m_ScreenWidth / 5;
	}
	else
	{
		m_IsForcedViewport = false;
	}

	if(SendViewportChangeToBackend && (m_ScreenWidth != m_DrawableWidth || m_ScreenHeight != m_DrawableHeight))
	{
		UpdateViewportInternal(m_ViewportX, 0, m_ScreenWidth, m_ScreenHeight, true, m_DrawableWidth, m_DrawableHeight);
	}
}

void CGraphics_Threaded::UpdateViewport(int X, int Y, int W, int H, bool ByResize)
{
	const bool Whole = ByResize || (X == 0 && Y == 0 && W == ScreenWidth() && H == ScreenHeight());
	m_DrawViewportX = Whole ? 0 : X;
	m_DrawViewportY = Whole ? 0 : Y;
	m_DrawViewportWidth = Whole ? 0 : W;
	m_DrawViewportHeight = Whole ? 0 : H;
	// A backend with its origin at the bottom flips the rectangle within what is
	// drawn to, which is the screen and not the rectangle itself.
	UpdateViewportInternal(X, Y, W, H, ByResize, ByResize ? W : ScreenWidth(), ByResize ? H : ScreenHeight());
}

void CGraphics_Threaded::UpdateViewportInternal(int X, int Y, int W, int H, bool ByResize, int SurfaceW, int SurfaceH)
{
	// A picture drawn without a window is still drawn into parts of itself.
	if(ByResize && !HasPresentationSurface())
		return;
	CCommandBuffer::SCommand_Update_Viewport Cmd;
	Cmd.m_X = X;
	Cmd.m_Y = Y;
	Cmd.m_Width = W;
	Cmd.m_Height = H;
	Cmd.m_SurfaceWidth = SurfaceW;
	Cmd.m_SurfaceHeight = SurfaceH;
	Cmd.m_ByResize = ByResize;
	if(!ByResize)
		Cmd.m_Cmd = CCommandBuffer::CMD_DRAW_VIEWPORT;
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

// Takes what the window (or the surface-less client) found out about the
// surface: its size in pixels, the window's size in screen coordinates and
// the refresh rate. The DPI scale is the ratio of the two sizes.
void CGraphics_Threaded::TakeSurfaceInfo(const SGraphicsSurfaceInfo &Surface)
{
	m_Presentable = Surface.m_Presentable;
	m_DrawableWidth = Surface.m_DrawableWidth;
	m_DrawableHeight = Surface.m_DrawableHeight;
	m_InsetLeft = Surface.m_InsetLeft;
	m_InsetRight = Surface.m_InsetRight;
	m_ScreenWidth = m_DrawableWidth;
	m_ScreenHeight = m_DrawableHeight;
	m_ScreenRefreshRate = Surface.m_RefreshRate;
	m_ScreenHiDPIScale = m_DrawableWidth / (float)std::max(Surface.m_WindowWidth, 1);
}

int CGraphics_Threaded::Init(IGraphicsBackend *pBackend, const SGraphicsSurfaceInfo &Surface)
{
	dbg_assert(pBackend != nullptr, "graphics need a backend");

	// fetch pointers
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();

	// init textures
	m_TextureHandles.Reset(CCommandBuffer::MAX_TEXTURES);

	m_BufferHandles.Reset(0);
	m_FirstFreeQuadContainer = -1;

	m_pBackend = pBackend;
	TakeSurfaceInfo(Surface);
	AddBackEndWarningIfExists();
	m_Capabilities = m_pBackend->GetCapabilities();
	m_Capabilities.m_PlanarYuvConversion = m_Capabilities.m_RenderTargets && m_Capabilities.m_PlanarYuvConversion;

	m_pCommandBuffer = new CCommandBuffer(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE);
	m_pReliableCommandBuffer = new CCommandBuffer(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE, RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE);
	m_pDeferredDestroyCommandBuffer = new CCommandBuffer(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE);

	m_QuadIndexBuffer.Invalidate();
	m_QuadIndexCount = 0;
	// Every backend draws quads through this, so it is always needed.
	if(!IndicesNumRequiredNotify(CCommandBuffer::MAX_VERTICES / 4 * 6))
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
		m_NullTexture = LoadTextureRaw(NullTextureInfo, IGraphics::TEXLOAD_LAYERED, "null-texture");
		dbg_assert(m_NullTexture.IsValid() && m_NullTexture.Id() == 0, "Null texture invalid");
	}

	static constexpr LOG_COLOR GPU_INFO_LOG_COLOR = LOG_COLOR{153, 127, 255};
	log_info_color(GPU_INFO_LOG_COLOR, "gfx", "GPU vendor: %s", GetVendorString());
	log_info_color(GPU_INFO_LOG_COLOR, "gfx", "GPU renderer: %s", GetRendererString());
	log_info_color(GPU_INFO_LOG_COLOR, "gfx", "GPU version: %s", GetVersionString());

	AdjustViewport(HasPresentationSurface());

	// Without a surface the first frame has no screen to open, so the frontend
	// opens the one it keeps for itself.
	if(!HasPresentationSurface() && !BeginVirtualScreenFrame())
	{
		log_error("gfx", "Could not create the target a surface-less client draws into.");
		return -1;
	}

	return 0;
}

void CGraphics_Threaded::Shutdown()
{
	// Init may have failed before the backend or the command buffers exist.
	if(m_pBackend == nullptr)
		return;
	if(m_pCommandBuffer == nullptr)
	{
		m_pBackend->Shutdown();
		delete m_pBackend;
		m_pBackend = nullptr;
		return;
	}
	DeleteBufferObject(m_QuadIndexBuffer);
	m_QuadIndexCount = 0;
	if(!SubmitReliableCommandBuffer(m_pReliableCommandBuffer))
		log_error("graphics", "Failed to submit reliable rendering commands during shutdown");
	SubmitFramePacket();
	if(!SubmitDeferredDestroys())
		log_error("graphics", "Failed to submit deferred rendering destroys during shutdown");
	m_pBackend->WaitForIdle();

	// shutdown the backend
	m_pBackend->Shutdown();
	delete m_pBackend;
	m_pBackend = nullptr;

	delete m_pCommandBuffer;
	delete m_pReliableCommandBuffer;
	delete m_pDeferredDestroyCommandBuffer;
	m_pCommandBuffer = nullptr;
	m_pReliableCommandBuffer = nullptr;
	m_pDeferredDestroyCommandBuffer = nullptr;
	m_ShutDown = true;
}

void CGraphics_Threaded::WarnPngliteIncompatibleImages(bool Warn)
{
	m_WarnPngliteIncompatibleImages = Warn;
}

void CGraphics_Threaded::AddWindowResizeListener(WINDOW_RESIZE_FUNC pFunc)
{
	m_vResizeListeners.emplace_back(pFunc);
}

bool CGraphics_Threaded::Resized(const SGraphicsSurfaceInfo &Surface)
{
	// Reopen the virtual screen frame at the new size right away.
	const bool VirtualFrameOpen = !HasPresentationSurface() && m_VirtualScreen.IsValid() && m_OffscreenFrameTarget == m_VirtualScreen;
	if(VirtualFrameOpen)
		FinishOffscreenFrame(false, CImageInfo(), CTextureHandle(), EPlanarYuvFormat::NV12);
	// Commands recorded against the old drawable size must never become a partial
	// frame followed by a viewport update for the new size.
	if(!m_pCommandBuffer->IsEmpty())
		DropCurrentFrame();

	const int PrevCanvasWidth = m_ScreenWidth;
	const int PrevCanvasHeight = m_ScreenHeight;
	TakeSurfaceInfo(Surface);

	AdjustViewport(false);
	UpdateViewportInternal(m_ViewportX, 0, m_ScreenWidth, m_ScreenHeight, true, m_DrawableWidth, m_DrawableHeight);

	// The reliable queue orders the viewport update before the next frame.
	KickCommandBuffer();
	if(VirtualFrameOpen && !BeginVirtualScreenFrame())
		log_error("graphics", "Failed to open the surface-less frame at the new size");

	const bool CanvasChanged = PrevCanvasWidth != m_ScreenWidth || PrevCanvasHeight != m_ScreenHeight;
	if(CanvasChanged)
	{
		for(auto &ResizeListener : m_vResizeListeners)
			ResizeListener();
	}
	return CanvasChanged;
}

template<typename TCommand>
void CGraphics_Threaded::NotifySurface(const char *pWhat)
{
	if(!HasPresentationSurface())
		return;
	if(!m_pCommandBuffer->IsEmpty())
		DropCurrentFrame();

	auto pCompletion = std::make_unique<CCommandBuffer::CCompletion>();
	TCommand Cmd;
	Cmd.m_pCompletion = pCompletion.get();
	if(!AddCmd(Cmd) || !KickCommandBuffer())
	{
		log_error("graphics", "Failed to queue window %s notification", pWhat);
		return;
	}
	pCompletion->Wait();
}

void CGraphics_Threaded::PresentationSurfaceLost()
{
	NotifySurface<CCommandBuffer::SCommand_WindowDestroyNtf>("destroy");
}

void CGraphics_Threaded::PresentationSurfaceRestored()
{
	NotifySurface<CCommandBuffer::SCommand_WindowCreateNtf>("create");
}

void CGraphics_Threaded::ReleaseSurfaceForMessageBox()
{
	if(m_pBackend == nullptr)
		return;
	m_pBackend->WaitForIdle();
	m_pBackend->ErroneousCleanup();
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

bool CGraphics_Threaded::EnsureVirtualScreen()
{
	if(HasPresentationSurface())
		return false;
	const uint32_t Width = static_cast<uint32_t>(std::max(m_ScreenWidth, 1));
	const uint32_t Height = static_cast<uint32_t>(std::max(m_ScreenHeight, 1));
	if(m_VirtualScreen.IsValid())
	{
		const STextureInfo &Info = m_vTextureInfos[m_VirtualScreen.Id()];
		if(Info.m_Desc.m_Width == Width && Info.m_Desc.m_Height == Height)
			return true;
		UnloadTexture(&m_VirtualScreen);
	}
	CTextureDesc Desc;
	Desc.m_Width = Width;
	Desc.m_Height = Height;
	Desc.m_Mipmaps = ETextureMipmaps::NONE;
	Desc.m_Usage = TEXTURE_USAGE_SAMPLED | TEXTURE_USAGE_COLOR_TARGET | TEXTURE_USAGE_COPY_SOURCE;
	m_VirtualScreen = CreateTexture(Desc);
	return m_VirtualScreen.IsValid();
}

bool CGraphics_Threaded::BeginVirtualScreenFrame()
{
	return EnsureVirtualScreen() && BeginOffscreenFrame(m_VirtualScreen);
}

// A screenshot is saved opaque. A frame read from a render target carries
// the alpha it was drawn with.
void CGraphics_Threaded::MakeScreenshotOpaque(CImageInfo &Image)
{
	if(Image.m_pData == nullptr || Image.m_Format != CImageInfo::FORMAT_RGBA)
		return;
	const size_t PixelCount = static_cast<size_t>(Image.m_Width) * Image.m_Height;
	for(size_t Index = 0; Index < PixelCount; ++Index)
		Image.m_pData[Index * 4 + 3] = 255;
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::PresentVirtualFrame(bool Readback, CImageInfo &&Recycled)
{
	// Only for a frame nobody else claimed. If a recorder already closed its
	// frame, the screen needs its own again.
	if(m_OffscreenFrameTarget.IsValid() && m_OffscreenFrameTarget != m_VirtualScreen)
		return nullptr;
	const bool Screenshot = m_DoScreenshot;
	m_DoScreenshot = false;
	m_pReadPixelColor = nullptr;
	auto pReadback = FinishOffscreenFrame(Readback || Screenshot, std::move(Recycled), CTextureHandle(), EPlanarYuvFormat::NV12);
	if(Screenshot)
	{
		CImageInfo Image;
		if(pReadback != nullptr && pReadback->Wait(Image))
		{
			MakeScreenshotOpaque(Image);
			m_pEngine->AddJob(std::make_shared<CScreenshotSaveJob>(m_pStorage, m_aScreenshotName, std::move(Image)));
		}
		else
			log_error("graphics", "Failed to create screenshot");
		pReadback.reset();
	}
	if(!BeginVirtualScreenFrame())
		log_error("graphics", "Failed to open the next surface-less frame");
	return Readback ? std::move(pReadback) : nullptr;
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::PresentFrame(bool Readback, CImageInfo &&Recycled)
{
	if(!HasPresentationSurface())
		return PresentVirtualFrame(Readback, std::move(Recycled));
	if(m_RenderPassActive)
		EndRenderPass();
	const bool Screenshot = m_DoScreenshot;
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
		pResult->m_Image = std::move(Recycled);
		CCommandBuffer::SCommand_PresentationTarget_Readback ReadbackCmd;
		ReadbackCmd.m_ReadPixel = !Readback && !Screenshot && ReadPixel;
		ReadbackCmd.m_Position = m_ReadPixelPosition;
		ReadbackCmd.m_pResult = pResult.get();
		ReadbackCmd.m_pCompletion = pResult.get();
		CCommandBuffer::SCommand_Swap SwapCmd;
		SwapCmd.m_PaceWithDisplay = m_PresentWaitsForDisplay;
		if(AddCmd(ReadbackCmd) && AddCmd(SwapCmd))
			FramePublished = SubmitFramePacket();
	}
	else if(ResourcesReady)
	{
		CCommandBuffer::SCommand_Swap Cmd;
		Cmd.m_PaceWithDisplay = m_PresentWaitsForDisplay;
		AddCmd(Cmd);
		FramePublished = SubmitFramePacket();
	}
	else
		DropCurrentFrame();

	if(ResourcesReady && (FramePublished || m_DropCurrentFrame))
		SubmitDeferredDestroys();
	m_pCommandBuffer->Reset();
	m_DropCurrentFrame = false;
	m_RenderPassActive = true;
	m_RenderPassTarget.Invalidate();
	// The swap happens on the render thread, so the main thread could handle a
	// display event while Apple's GL on Metal is inside SDL_GL_SwapWindow.
	// TODO: Remove when https://github.com/libsdl-org/SDL/issues/5203 is fixed
#ifdef CONF_PLATFORM_MACOS
	if(str_find(GetVersionString(), "Metal"))
		WaitForIdle();
#endif

	if(pResult != nullptr && FramePublished && (Screenshot || ReadPixel))
	{
		if(!pResult->IsComplete())
			FinishReadbacks();
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
		return std::make_unique<CTextureReadback>(this, std::move(pResult));
	return nullptr;
}

void CGraphics_Threaded::Swap()
{
	PresentFrame(false);
}

std::unique_ptr<IGraphics::ITextureReadback> CGraphics_Threaded::PresentAndReadbackAsync(CImageInfo &&Recycled)
{
	return PresentFrame(true, std::move(Recycled));
}

bool CGraphics_Threaded::SetVSync(bool State)
{
	if(!m_pCommandBuffer)
		return true;
	if(!HasPresentationSurface())
		return false;

	// add vsync command
	auto pResult = std::make_unique<CCommandBuffer::SCommand_VSync::SResult>();
	CCommandBuffer::SCommand_VSync Cmd;
	Cmd.m_VSync = State ? 1 : 0;
	Cmd.m_pResult = pResult.get();
	Cmd.m_pCompletion = pResult.get();
	if(!AddCmd(Cmd) || !KickCommandBuffer())
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
	if(!HasPresentationSurface())
		return false;

	// add multisampling command
	auto pResult = std::make_unique<CCommandBuffer::SCommand_MultiSampling::SResult>();
	pResult->m_MultiSamplingCount = MultiSamplingCountBackend;
	CCommandBuffer::SCommand_MultiSampling Cmd;
	Cmd.m_RequestedMultiSamplingCount = ReqMultiSamplingCount;
	Cmd.m_pResult = pResult.get();
	Cmd.m_pCompletion = pResult.get();
	if(!AddCmd(Cmd) || !KickCommandBuffer())
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
	if(!AddCmd(Cmd))
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

extern IEngineGraphics *CreateEngineGraphicsThreaded()
{
	return new CGraphics_Threaded();
}
