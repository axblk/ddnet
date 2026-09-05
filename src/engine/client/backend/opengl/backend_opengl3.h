// This file can be included several times.
#if (!defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H)) || \
	(defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H_AS_ES))

#if !defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H
#endif

#if defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H_AS_ES)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H_AS_ES
#endif

#include "backend_opengl_base.h"

class CGLSLTWProgram;
class CGLSLPrimitiveProgram;
class CGLSLTileProgram;
class CGLSLPrimitiveExProgram;
class CGLSLQuadProgram;
class CGLSLSpriteMultipleProgram;
class CGLSLTextProgram;

// The programs the shader backends share. Everything below OpenGL 3 has no
// programs at all and is served by CCommandProcessorFragment_OpenGL, so this is
// a place to keep what a shader backend needs rather than a backend of its own.
class CCommandProcessorFragment_OpenGLShared : public CCommandProcessorFragment_OpenGLBase
{
	void UseProgram(CGLSLTWProgram *pProgram);

protected:
	void SetState(const CCommandBuffer::SState &State, CGLSLTWProgram *pProgram, bool Use2DArrayTextures = false);

	CGLSLTileProgram *m_pTileProgram = nullptr;
	CGLSLTileProgram *m_pTileProgramTextured = nullptr;
	CGLSLTileProgram *m_pBorderTileProgram = nullptr;
	CGLSLTileProgram *m_pBorderTileProgramTextured = nullptr;
	CGLSLPrimitiveProgram *m_pPrimitive3DProgram = nullptr;
	CGLSLPrimitiveProgram *m_pPrimitive3DProgramTextured = nullptr;
};

#define MAX_STREAM_BUFFER_COUNT 10

// takes care of opengl 3.3+ related rendering
class CCommandProcessorFragment_OpenGL3_3 : public CCommandProcessorFragment_OpenGLShared
{
protected:
	int m_MaxQuadsAtOnce;
	static const int ms_MaxQuadsPossible = 256;

	CGLSLPrimitiveProgram *m_pPrimitiveProgram;
	CGLSLPrimitiveProgram *m_pPrimitiveProgramTextured;
	CGLSLPrimitiveProgram *m_pPlanarYuvProgram;
	CGLSLQuadProgram *m_pQuadProgram;
	CGLSLQuadProgram *m_pQuadProgramTextured;
	CGLSLQuadProgram *m_pQuadProgramGrouped;
	CGLSLQuadProgram *m_pQuadProgramTexturedGrouped;
	CGLSLTextProgram *m_pTextProgram;
	CGLSLPrimitiveExProgram *m_pPrimitiveExProgram;
	CGLSLPrimitiveExProgram *m_pPrimitiveExProgramTextured;
	CGLSLSpriteMultipleProgram *m_pSpriteProgramMultiple;

	TWGLuint m_LastProgramId;

	TWGLuint m_aPrimitiveDrawVertexId[MAX_STREAM_BUFFER_COUNT];
	TWGLuint m_PrimitiveDrawVertexIdTex3D;
	TWGLuint m_aPrimitiveDrawBufferId[MAX_STREAM_BUFFER_COUNT];
	TWGLuint m_aPrimitiveDrawIndexBufferId[MAX_STREAM_BUFFER_COUNT];
	TWGLuint m_PrimitiveDrawBufferIdTex3D;

	TWGLuint m_aLastIndexBufferBound[MAX_STREAM_BUFFER_COUNT];

	int m_LastStreamBuffer;

	// A vertex array is the one place the container idea still belongs: OpenGL
	// wants the attribute layout recorded against the buffer, so it is built on
	// first use for a (buffer, layout) pair and kept until the buffer goes away.
	struct SVertexArray
	{
		TWGLuint m_VertArrayId = 0;
		TWGLuint m_LastIndexBufferBound = 0;
		IGraphics::EVertexLayout m_Layout = IGraphics::EVertexLayout::COUNT;
	};
	std::vector<SVertexArray> m_vVertexArrays;
	SVertexArray *VertexArrayFor(IGraphics::CBufferHandle Buffer, IGraphics::EVertexLayout Layout);
	void DestroyVertexArray(size_t Index);

	std::vector<TWGLuint> m_vBufferObjectIndices;
	// The buffer target each of them was created with, because WebGL2 does not
	// let a buffer be uploaded to through any other one.
	std::vector<TWGLenum> m_vBufferObjectTargets;

	CCommandBuffer::SColorf m_ClearColor;

	void InitPrimExProgram(CGLSLPrimitiveExProgram *pProgram, class CGLSLCompiler *pCompiler, bool Textured);

	bool IsNewApi() override { return true; }

	void UseProgram(CGLSLTWProgram *pProgram);
	void UploadStreamBufferData(const void *pVertices, size_t DataSize, bool TextureArray);
	void RenderDualAtlasComposite(const CCommandBuffer::SState &State, uint32_t IndexCount, size_t IndexOffset, int AtlasTextureIndex, const CCommandBuffer::SDrawDataDualAtlas &DrawData);

	// The one place that turns an interface format into a modern OpenGL one.
	static int ToGLFormat(IGraphics::ETextureFormat Format);
	void TextureUpdate(int Slot, int X, int Y, int Width, int Height, IGraphics::ETextureFormat Format, uint8_t *pTexData);
	void TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData);

	bool Cmd_Init(const SCommand_Init *pCommand) override;
	void Cmd_Shutdown(const SCommand_Shutdown *pCommand) override;
	void Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand) override;
	void Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand) override;
	void Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand) override;
	void Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand) override;
	// A render target readback does not wait for its picture. The pixels go
	// into a pack buffer, a fence marks when they are there, and the result is
	// handed over from CollectFinishedReadbacks once the fence has signalled -
	// which is what lets the export keep its frames in flight on OpenGL, as it
	// does on Vulkan. The export keeps three; one more here would
	// only buy memory, so the oldest is waited out instead.
	static constexpr size_t READBACK_SLOT_COUNT = 3;
	struct SPendingReadback
	{
		TWGLuint m_PackBuffer = 0;
		// A GLsync; the header keeps the GL types out.
		void *m_pFence = nullptr;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		CCommandBuffer::SImageReadbackResult *m_pResult = nullptr;
	};
	std::vector<SPendingReadback> m_vPendingReadbacks;
	void FinishReadback(SPendingReadback &Pending, bool Wait);
	void CollectFinishedReadbacks() override;
	void FinishReadbacks() override;
	void Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand) override;
	void Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand) override;
	void Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand) override;
	void Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand) override;

	void Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand) override;
	void Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand) override;
	void Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand) override;

	void Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand) override;

public:
	CCommandProcessorFragment_OpenGL3_3() = default;
};

#endif
