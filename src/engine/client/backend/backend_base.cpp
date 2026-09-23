#include "backend_base.h"

#include <base/log.h>
#include <base/mem.h>

#include <engine/gfx/image_manipulation.h>

#include <algorithm>
#include <limits>
#include <utility>

void CCommandProcessorFragment_Renderer::DropCommand(const char *pReason) const
{
	if(std::find(m_vDroppedCommandReasons.begin(), m_vDroppedCommandReasons.end(), pReason) != m_vDroppedCommandReasons.end())
		return;
	m_vDroppedCommandReasons.emplace_back(pReason);
	log_error("gfx", "dropped a command the backend cannot carry out: %s", pReason);
#if defined(CONF_DEBUG)
	// In a development build this is a bug in the frontend or a gap in the
	// backend, and either way it should stop here rather than in a screenshot.
	dbg_assert_failed("The backend dropped a command: %s", pReason);
#endif
}

static void Texture2DTo3D(uint8_t *pImageBuffer, int ImageWidth, int ImageHeight, size_t PixelSize, int SplitCountWidth, int SplitCountHeight, uint8_t *pTarget3DImageData, int &Target3DImageWidth, int &Target3DImageHeight)
{
	Target3DImageWidth = ImageWidth / SplitCountWidth;
	Target3DImageHeight = ImageHeight / SplitCountHeight;

	const size_t FullImageWidth = (size_t)ImageWidth * PixelSize;

	for(int Y = 0; Y < SplitCountHeight; ++Y)
	{
		for(int X = 0; X < SplitCountWidth; ++X)
		{
			for(int Y3D = 0; Y3D < Target3DImageHeight; ++Y3D)
			{
				int DepthIndex = X + Y * SplitCountWidth;

				size_t TargetImageFullWidth = (size_t)Target3DImageWidth * PixelSize;
				size_t TargetImageFullSize = TargetImageFullWidth * Target3DImageHeight;
				ptrdiff_t ImageOffset = (ptrdiff_t)(((size_t)Y * FullImageWidth * (size_t)Target3DImageHeight) + ((size_t)Y3D * FullImageWidth) + ((size_t)X * TargetImageFullWidth));
				ptrdiff_t TargetImageOffset = (ptrdiff_t)(TargetImageFullSize * (size_t)DepthIndex + ((size_t)Y3D * TargetImageFullWidth));
				mem_copy(pTarget3DImageData + TargetImageOffset, pImageBuffer + ImageOffset, TargetImageFullWidth);
			}
		}
	}
}

std::unique_ptr<uint8_t, decltype(&free)> CCommandProcessorFragment_Renderer::PrepareLayeredImage(const uint8_t *pData, int Width, int Height, size_t PixelSize, int LayerColumns, int LayerRows, int &LayerWidth, int &LayerHeight)
{
	LayerWidth = 0;
	LayerHeight = 0;
	if(pData == nullptr || LayerColumns <= 0 || LayerRows <= 0)
		return {nullptr, free};

	std::unique_ptr<uint8_t, decltype(&free)> pResized(nullptr, free);
	int ConvertWidth = Width;
	int ConvertHeight = Height;
	// A zero side counts as not dividing into whole layers: the resize below
	// turns it into one layer's worth rather than an empty upload.
	if(ConvertWidth == 0 || ConvertHeight == 0 || (ConvertWidth % LayerColumns) != 0 || (ConvertHeight % LayerRows) != 0)
	{
		const int NewWidth = std::max(HighestBit(ConvertWidth / LayerColumns), 1) * LayerColumns;
		const int NewHeight = std::max(HighestBit(ConvertHeight / LayerRows), 1) * LayerRows;
		pResized.reset(ResizeImage(pData, ConvertWidth, ConvertHeight, NewWidth, NewHeight, static_cast<int>(PixelSize)));
		if(pResized == nullptr)
			return {nullptr, free};
		log_debug("gfx", "Layered texture was resized. Size=(%d, %d) Resized=(%d, %d)", ConvertWidth, ConvertHeight, NewWidth, NewHeight);
		pData = pResized.get();
		ConvertWidth = NewWidth;
		ConvertHeight = NewHeight;
	}

	std::unique_ptr<uint8_t, decltype(&free)> pConverted(static_cast<uint8_t *>(malloc(PixelSize * ConvertWidth * ConvertHeight)), free);
	if(pConverted == nullptr)
		return {nullptr, free};
	Texture2DTo3D(const_cast<uint8_t *>(pData), ConvertWidth, ConvertHeight, PixelSize, LayerColumns, LayerRows, pConverted.get(), LayerWidth, LayerHeight);
	return pConverted;
}

bool CCommandProcessorFragment_Renderer::IsDrawCommandValid(const CCommandBuffer::SCommand *pCommand) const
{
	const CCommandBuffer::SState *pState;
	EPipelineProgram Program;
	if(pCommand->m_Cmd == CCommandBuffer::CMD_DRAW)
	{
		const auto *pDraw = static_cast<const CCommandBuffer::SCommand_Draw *>(pCommand);
		pState = &pDraw->m_State;
		Program = pDraw->m_Program;
	}
	else if(pCommand->m_Cmd == CCommandBuffer::CMD_DRAW_INDEXED)
	{
		const auto *pDraw = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand);
		if(!m_BufferHandles.IsActive(pDraw->m_VertexBuffer) || !m_BufferHandles.IsActive(pDraw->m_IndexBuffer))
			return false;
		if(pDraw->m_Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE && !pDraw->m_State.m_Texture.IsValid())
			return false;
		pState = &pDraw->m_State;
		Program = pDraw->m_Program;
	}
	else
		return true;
	if(pState->m_Texture.IsValid() && !m_TextureHandles.IsActive(pState->m_Texture))
		return false;
	return Program < EPipelineProgram::COUNT;
}

void CCommandProcessorFragment_Renderer::CopyReadbackRows(const uint8_t *pSource, size_t Pitch, uint32_t Width, uint32_t Height, bool BGRA, bool OpaqueAlpha, uint8_t *pDestination)
{
	const size_t RowSize = (size_t)Width * 4;
	if(Pitch == RowSize)
		mem_copy(pDestination, pSource, RowSize * Height);
	else
	{
		for(uint32_t Y = 0; Y < Height; ++Y)
			mem_copy(pDestination + Y * RowSize, pSource + Y * Pitch, RowSize);
	}
	if(!BGRA && !OpaqueAlpha)
		return;
	for(uint32_t Y = 0; Y < Height; ++Y)
	{
		uint8_t *pRow = pDestination + Y * RowSize;
		for(uint32_t X = 0; X < Width; ++X)
		{
			if(BGRA)
				std::swap(pRow[X * 4], pRow[X * 4 + 2]);
			if(OpaqueAlpha)
				pRow[X * 4 + 3] = 255;
		}
	}
}

uint64_t CCommandProcessorFragment_Renderer::TicksToNanoseconds(uint64_t Ticks, float PeriodNanoseconds)
{
	const long double Nanoseconds = static_cast<long double>(Ticks) * PeriodNanoseconds;
	if(Nanoseconds >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(Nanoseconds + 0.5L);
}

bool CCommandProcessorFragment_Renderer::ScreenToClip(const CCommandBuffer::SPoint &ScreenTL, const CCommandBuffer::SPoint &ScreenBR, bool ClipYUp, vec2 &Scale, vec2 &Translate)
{
	const float Width = ScreenBR.x - ScreenTL.x;
	const float Height = ScreenBR.y - ScreenTL.y;
	if(Width == 0.0f || Height == 0.0f)
		return false;
	const float YSign = ClipYUp ? -1.0f : 1.0f;
	Scale = vec2(2.0f / Width, YSign * 2.0f / Height);
	Translate = vec2(-(ScreenTL.x + ScreenBR.x) / Width, -YSign * (ScreenTL.y + ScreenBR.y) / Height);
	return true;
}
