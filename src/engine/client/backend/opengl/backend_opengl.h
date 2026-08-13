// This file can be included several times.
#if (!defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H)) || \
	(defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H_AS_ES))

#if !defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H
#endif

#if defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H_AS_ES)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_H_AS_ES
#endif

#include <base/dbg.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/graphics_defines.h>

class CGLSLTWProgram;
class CGLSLPrimitiveProgram;
class CGLSLTileProgram;

#if defined(BACKEND_AS_OPENGL_ES) && defined(CONF_BACKEND_OPENGL_ES3)
#define BACKEND_GL_MODERN_API 1
#endif

// takes care of opengl related rendering
class CCommandProcessorFragment_OpenGL : public CCommandProcessorFragment_Renderer
{
protected:
	struct CTexture
	{
		CTexture() :
			m_Tex(0), m_Tex2DArray(0), m_Sampler(0), m_Sampler2DArray(0), m_Framebuffer(0), m_LastWrapMode(EWrapMode::REPEAT), m_MemSize(0), m_Width(0), m_Height(0), m_SourceWidth(0), m_SourceHeight(0), m_RescaleCount(0), m_ResizeWidth(0), m_ResizeHeight(0)
		{
		}

		TWGLuint m_Tex;
		TWGLuint m_Tex2DArray; // or 3D texture as fallback
		TWGLuint m_Sampler;
		TWGLuint m_Sampler2DArray; // or 3D texture as fallback
		TWGLuint m_Framebuffer;
		EWrapMode m_LastWrapMode;

		int m_MemSize;

		int m_Width;
		int m_Height;
		size_t m_SourceWidth;
		size_t m_SourceHeight;
		IGraphics::ETextureFormat m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
		uint8_t m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED;
		int m_RescaleCount;
		float m_ResizeWidth;
		float m_ResizeHeight;
	};
	std::vector<CTexture> m_vTextures;
	CGenerationHandleStore<IGraphics::CTextureHandle> m_TextureHandles;
	std::vector<CCommandBuffer::STextureBindingDesc> m_vTextureBindings;
	CGenerationHandleStore<CCommandBuffer::CTextureBindingHandle> m_TextureBindingHandles;
	std::vector<EPipelineProgram> m_vPipelines;
	CGenerationHandleStore<CCommandBuffer::CPipelineHandle> m_PipelineHandles;
	CGenerationHandleStore<IGraphics::CBufferHandle> m_BufferHandles;
	CGenerationHandleStore<IGraphics::CBufferContainerHandle> m_BufferContainerHandles;
	std::vector<IGraphics::CBufferHandle> m_vBufferContainerBindings;
	std::atomic<uint64_t> *m_pTextureMemoryUsage;

	uint32_t m_CanvasWidth = 0;
	uint32_t m_CanvasHeight = 0;
	int m_PresentationViewportX = 0;
	int m_PresentationViewportY = 0;
	int m_PresentationViewportWidth = 0;
	int m_PresentationViewportHeight = 0;
	int m_RenderTargetWidth = 0;
	int m_RenderTargetHeight = 0;
	bool m_RenderingToTexture = false;

	TWGLint m_MaxTexSize;

	bool m_Has2DArrayTextures;
	TWGLenum m_2DArrayTarget;
	bool m_Has3DTextures;
	bool m_HasMipMaps;
	bool m_HasNPOTTextures;

	bool m_HasShaders;
	EBlendMode m_LastBlendMode; // avoid all possible opengl state changes
	bool m_LastClipEnable;

	int m_OpenGLTextureLodBIAS;

	bool m_IsOpenGLES;

	bool IsTexturedState(const CCommandBuffer::SState &State);
	void SetScissor(const CCommandBuffer::SState &State);

	bool InitOpenGL(const SCommand_Init *pCommand);

	void SetState(const CCommandBuffer::SState &State, bool Use2DArrayTexture = false);
	virtual bool IsNewApi() { return false; }
	void DestroyTexture(int Slot);

	static size_t GLFormatToPixelSize(int GLFormat);

	void TextureUpdate(int Slot, int X, int Y, int Width, int Height, int GLFormat, uint8_t *pTexData);
	void TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData);
	bool IsTextureUpdateValid(const CCommandBuffer::SCommand_Texture_Update *pCommand) const;

	virtual bool Cmd_Init(const SCommand_Init *pCommand);
	virtual void Cmd_Shutdown(const SCommand_Shutdown *pCommand) {}
	virtual void Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand);
	virtual void Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand);
	virtual void Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand);
	virtual void Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand);
	void Cmd_TextureBinding_Destroy(const CCommandBuffer::SCommand_TextureBinding_Destroy *pCommand);
	void Cmd_TextureBinding_Create(const CCommandBuffer::SCommand_TextureBinding_Create *pCommand);
	EPipelineProgram PipelineProgram(CCommandBuffer::CPipelineHandle Pipeline) const { return m_vPipelines[Pipeline.Id()]; }
	virtual void Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand);
	virtual void Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand);
	virtual void Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand);
	virtual void Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand);
	virtual void Cmd_PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand);

	virtual void Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand);

	virtual void Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand) { dbg_assert_failed("Call of unsupported Cmd_CreateBufferObject"); }
	virtual void Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand) { dbg_assert_failed("Call of unsupported Cmd_RecreateBufferObject"); }
	virtual void Cmd_UpdateBufferObject(const CCommandBuffer::SCommand_UpdateBufferObject *pCommand) { dbg_assert_failed("Call of unsupported Cmd_UpdateBufferObject"); }
	virtual void Cmd_CopyBufferObject(const CCommandBuffer::SCommand_CopyBufferObject *pCommand) { dbg_assert_failed("Call of unsupported Cmd_CopyBufferObject"); }
	virtual void Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand) { dbg_assert_failed("Call of unsupported Cmd_DeleteBufferObject"); }

	virtual void Cmd_CreateBufferContainer(const CCommandBuffer::SCommand_CreateBufferContainer *pCommand) { dbg_assert_failed("Call of unsupported Cmd_CreateBufferContainer"); }
	virtual void Cmd_UpdateBufferContainer(const CCommandBuffer::SCommand_UpdateBufferContainer *pCommand) { dbg_assert_failed("Call of unsupported Cmd_UpdateBufferContainer"); }
	virtual void Cmd_DeleteBufferContainer(const CCommandBuffer::SCommand_DeleteBufferContainer *pCommand) { dbg_assert_failed("Call of unsupported Cmd_DeleteBufferContainer"); }
	virtual void Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand);

public:
	CCommandProcessorFragment_OpenGL();

	ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override;
};

class CCommandProcessorFragment_OpenGL2 : public CCommandProcessorFragment_OpenGL
{
	struct SBufferContainer
	{
		SBufferContainerInfo m_ContainerInfo;
	};
	std::vector<SBufferContainer> m_vBufferContainers;

	struct SBufferObject
	{
		SBufferObject(TWGLuint BufferObjectId) :
			m_BufferObjectId(BufferObjectId)
		{
			m_pData = nullptr;
		}
		TWGLuint m_BufferObjectId;
		uint8_t *m_pData;
	};

	std::vector<SBufferObject> m_vBufferObjectIndices;

#ifndef BACKEND_GL_MODERN_API
	bool AnalyzeLayeredTexture(const SGraphicsVertexTex3D *pVertices, size_t VerticesCount, const uint8_t *pTextureData);
	bool IsLayeredTextureAnalysisSucceeded();
#endif

	void UseProgram(CGLSLTWProgram *pProgram);

protected:
	void SetState(const CCommandBuffer::SState &State, CGLSLTWProgram *pProgram, bool Use2DArrayTextures = false);

#ifndef BACKEND_GL_MODERN_API
	bool Cmd_Init(const SCommand_Init *pCommand) override;
	void Cmd_Shutdown(const SCommand_Shutdown *pCommand) override;

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
#endif

	CGLSLTileProgram *m_pTileProgram = nullptr;
	CGLSLTileProgram *m_pTileProgramTextured = nullptr;
	CGLSLTileProgram *m_pBorderTileProgram = nullptr;
	CGLSLTileProgram *m_pBorderTileProgramTextured = nullptr;
	CGLSLPrimitiveProgram *m_pPrimitive3DProgram = nullptr;
	CGLSLPrimitiveProgram *m_pPrimitive3DProgramTextured = nullptr;
};

#if defined(BACKEND_AS_OPENGL_ES) && defined(CONF_BACKEND_OPENGL_ES3)
#undef BACKEND_GL_MODERN_API
#endif

#endif
