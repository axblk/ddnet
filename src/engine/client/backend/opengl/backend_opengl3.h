// This file can be included several times.
#if (!defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H)) || \
	(defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H_AS_ES))

#if !defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H
#endif

#if defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H_AS_ES)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL3_H_AS_ES
#endif

#include "backend_opengl.h"

class CGLSLPrimitiveExProgram;
class CGLSLQuadProgram;
class CGLSLSpriteMultipleProgram;
class CGLSLTextProgram;

#define MAX_STREAM_BUFFER_COUNT 10

// takes care of opengl 3.3+ related rendering
class CCommandProcessorFragment_OpenGL3_3 : public CCommandProcessorFragment_OpenGL2
{
protected:
	int m_MaxQuadsAtOnce;
	static const int ms_MaxQuadsPossible = 256;

	CGLSLPrimitiveProgram *m_pPrimitiveProgram;
	CGLSLPrimitiveProgram *m_pPrimitiveProgramTextured;
	CGLSLPrimitiveProgram *m_pBlurProgram;
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

	void DestroyBufferContainer(int Index, bool DeleteBOs = true);

	struct SBufferContainer
	{
		SBufferContainer() :
			m_VertArrayId(0), m_LastIndexBufferBound(0) {}
		TWGLuint m_VertArrayId;
		TWGLuint m_LastIndexBufferBound;
		SBufferContainerInfo m_ContainerInfo;
	};
	std::vector<SBufferContainer> m_vBufferContainers;

	std::vector<TWGLuint> m_vBufferObjectIndices;
	// The buffer target each of them was created with, because WebGL2 does not
	// let a buffer be uploaded to through any other one.
	std::vector<TWGLenum> m_vBufferObjectTargets;

	CCommandBuffer::SColorf m_ClearColor;

	void InitPrimExProgram(CGLSLPrimitiveExProgram *pProgram, class CGLSLCompiler *pCompiler, class IStorage *pStorage, bool Textured);

	bool IsNewApi() override { return true; }

	void UseProgram(CGLSLTWProgram *pProgram);
	void UploadStreamBufferData(const void *pVertices, size_t DataSize, bool TextureArray);
	void RenderDualAtlasComposite(const CCommandBuffer::SState &State, uint32_t IndexCount, size_t IndexOffset, int PrimaryTextureIndex, int SecondaryTextureIndex, const CCommandBuffer::SDrawDataDualAtlas &DrawData);

	void TextureUpdate(int Slot, int X, int Y, int Width, int Height, int GLFormat, uint8_t *pTexData);
	void TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData);

	bool Cmd_Init(const SCommand_Init *pCommand) override;
	void Cmd_Shutdown(const SCommand_Shutdown *pCommand) override;
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
	void Cmd_UpdateBufferObject(const CCommandBuffer::SCommand_UpdateBufferObject *pCommand) override;
	void Cmd_CopyBufferObject(const CCommandBuffer::SCommand_CopyBufferObject *pCommand) override;
	void Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand) override;

	void Cmd_CreateBufferContainer(const CCommandBuffer::SCommand_CreateBufferContainer *pCommand) override;
	void Cmd_UpdateBufferContainer(const CCommandBuffer::SCommand_UpdateBufferContainer *pCommand) override;
	void Cmd_DeleteBufferContainer(const CCommandBuffer::SCommand_DeleteBufferContainer *pCommand) override;
	void Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand) override;

public:
	CCommandProcessorFragment_OpenGL3_3() = default;
};

#endif
