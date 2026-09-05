#ifndef ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H

#include "backend_opengl_base.h"

#include <base/vmath.h>

// Everything below OpenGL 3 has no programs at all, so a pipeline is emulated
// here: a buffer is converted into a form fixed function can read, and a draw
// becomes a transform, a colour and a triangle list. Never compiled for GLES,
// which this client only ever asks for in version 3.
class CCommandProcessorFragment_OpenGL : public CCommandProcessorFragment_OpenGLBase
{
protected:
	// Without shaders there is nowhere to put a buffer but memory, and nothing
	// to read it with but the expansion below. The data still arrives through
	// the same commands, so the frontend does not have to know.
	struct SEmulatedBuffer
	{
		std::vector<uint8_t> m_vData;
	};
	std::vector<SEmulatedBuffer> m_vEmulatedBuffers;

	// What the fixed function path can read straight out of a vertex array.
	// A container is converted into one of these once, when its layout and its
	// data are both known, instead of once per draw.
	enum class EVertexForm
	{
		NONE,
		PRIMITIVE, // position, texture coordinate, colour
		LAYERED, // position, colour, texture coordinate with a layer
	};

	// Fixed function cannot read the client's vertex layouts directly, so a
	// buffer is converted once into a form it can, and kept that way until the
	// buffer changes. Keyed by buffer, like the vertex arrays the shader path
	// keeps for the same reason.
	struct SConvertedBuffer
	{
		IGraphics::EVertexLayout m_Layout = IGraphics::EVertexLayout::COUNT;
		IGraphics::CBufferHandle m_SourceBuffer;
		std::vector<uint8_t> m_vConverted;
		// The colours stay here even when the geometry has gone to the device,
		// because a draw that tints them has to read them back.
		std::vector<CCommandBuffer::SColor> m_vColors;
		TWGLuint m_BufferId = 0;
		size_t m_VertexCount = 0;
		EVertexForm m_Form = EVertexForm::NONE;
	};
	std::vector<SConvertedBuffer> m_vConvertedBuffers;

	// Expansion writes here and the draw reads from here, once per draw, so
	// the buffers stay warm instead of being allocated per frame.
	std::vector<CCommandBuffer::SVertex> m_vExpandedVertices;
	std::vector<CCommandBuffer::SVertexTex3DStream> m_vExpandedLayeredVertices;

	void SetState(const CCommandBuffer::SState &State, bool Use2DArrayTexture = false);
	void CreateSplitChannelTexture(int Slot, const IGraphics::CTextureDesc &Desc, const uint8_t *pTexData);
	void UpdateSplitChannelTexture(int Slot, int X, int Y, int Width, int Height, const uint8_t *pTexData);

	// The one place that turns an interface format into a legacy OpenGL one.
	// How many bytes a pixel costs is IGraphics::PixelSize's answer.
	static int ToGLFormat(IGraphics::ETextureFormat Format);

	void TextureUpdate(int Slot, int X, int Y, int Width, int Height, IGraphics::ETextureFormat Format, uint8_t *pTexData);
	void TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData);

	bool Cmd_Init(const SCommand_Init *pCommand) override;
	void Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand) override;
	void Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand) override;
	void Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand) override;
	void Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand) override;
	void Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand) override;
	void Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand) override;
	void Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand) override;
	void Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand) override;

	void Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand) override;
	void Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand) override;
	void Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand) override;

	void Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand) override;

	// Everything a pipeline does to a vertex before it is drawn, done here
	// because there is no program to do it in. What comes out is what the
	// fixed-function path can draw: a triangle list, coloured and textured.
	static ColorRGBA ReadAttribute(const uint8_t *pVertex, const IGraphics::CVertexAttributeDesc &Attribute);
	float LayerCoordinate(const CCommandBuffer::SState &State, float Layer) const;
	[[nodiscard]] bool CollectIndexedDraw(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer *&pConverted, const uint8_t *&pIndices, size_t &IndexStride, uint32_t &IndexCount);
	uint32_t ReadIndex(const uint8_t *pIndices, size_t IndexStride, uint32_t Position) const;
	void DrawEmulatedArrayColor(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Converted, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount);
	void DrawEmulatedQuads(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Converted, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount);
	void DrawEmulatedPrimitives(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Converted, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount, EPipelineProgram Program);
	void DrawEmulatedDualAtlas(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Converted, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount);
	void DrawExpandedVertices(const CCommandBuffer::SState &State, bool Layered, TWGLuint OverrideTexture = 0);
	SConvertedBuffer *ConvertedBufferFor(IGraphics::CBufferHandle Buffer, IGraphics::EVertexLayout Layout);
	void ReleaseConvertedBuffer(SConvertedBuffer &Converted);
	void InvalidateContainersOfBuffer(size_t BufferIndex);
	// Binds the converted array, from the buffer object if there is one.
	void BindConvertedContainer(const SConvertedBuffer &Converted, const CCommandBuffer::SColor *pColors = nullptr);
	void UnbindConvertedContainer(const SConvertedBuffer &Converted);
	// Vertex colours tinted by the draw, rebuilt per draw because that is the
	// only part of a converted container a draw can change.
	std::vector<CCommandBuffer::SColor> m_vTintedColors;
	const CCommandBuffer::SColor *TintColors(const SConvertedBuffer &Converted, const ColorRGBA &Tint);
	const CCommandBuffer::SColor *FlatColors(const SConvertedBuffer &Converted, const ColorRGBA &Color);
	// The transform a draw applies to every vertex alike, which fixed function
	// has had since OpenGL 1.0: a matrix for the geometry, one for the texture
	// coordinates, and one colour.
	void SetDrawTransform(const vec2 &Offset, const vec2 &Scale, const vec2 &RotationCenter, float Rotation);
	void SetTextureTransform(const CCommandBuffer::SState &State, const vec2 &TexScale, bool LayerCoordinates);
	void ResetTransforms();
	void DrawPrimitives(EPrimitiveType PrimitiveType, uint32_t VertexCount, IGraphics::CBufferHandle IndexBuffer);

public:
	CCommandProcessorFragment_OpenGL() = default;
};

#endif
