// This file can be included several times.
#if (!defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_BASE_H)) || \
	(defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_BASE_H_AS_ES))

#if !defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_BASE_H)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_BASE_H
#endif

#if defined(BACKEND_AS_OPENGL_ES) && !defined(ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_BASE_H_AS_ES)
#define ENGINE_CLIENT_BACKEND_OPENGL_BACKEND_OPENGL_BASE_H_AS_ES
#endif

#include <base/dbg.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/graphics_defines.h>

// What every OpenGL backend needs regardless of whether it has programs: the
// textures, the handle bookkeeping the frontend's generation counters are
// checked against, the context probing, and the command dispatch. The two
// things that differ - how a vertex is drawn, and what a texture looks like to
// the sampler - are left to the classes below this one.
class CCommandProcessorFragment_OpenGLBase : public CCommandProcessorFragment_Renderer
{
protected:
	struct CTexture
	{
		CTexture() :
			m_Tex(0), m_Tex2DArray(0), m_Sampler(0), m_Sampler2DArray(0), m_Framebuffer(0), m_LastWrapMode(EWrapMode::REPEAT), m_MemSize(0), m_Width(0), m_Height(0), m_SourceWidth(0), m_SourceHeight(0), m_RescaleCount(0), m_ResizeWidth(0), m_ResizeHeight(0)
		{
		}

		TWGLuint m_Tex;
		// Fixed function cannot pick a texture's second channel out in the
		// fragment stage, so a two channel texture lives here as two.
		TWGLuint m_TexSecondChannel = 0;
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
		// A layered texture that lives as a volume needs its layer count to
		// turn an index back into the depth coordinate a sampler wants.
		int m_LayerCount = 0;
	};
	std::vector<CTexture> m_vTextures;
	CGenerationHandleStore<IGraphics::CTextureHandle> m_TextureHandles;
	CGenerationHandleStore<IGraphics::CBufferHandle> m_BufferHandles;
	std::atomic<uint64_t> *m_pTextureMemoryUsage;

	// Where the presented image lies on the surface, with the bottom left
	// origin that OpenGL uses. It is aligned to the top left, so this is not
	// the surface's origin when the viewport is clamped.
	int m_PresentationViewportX = 0;
	int m_PresentationViewportY = 0;
	int m_PresentationViewportWidth = 0;
	int m_PresentationViewportHeight = 0;
	// The image is narrower than the surface: a display cutout is excluded.
	bool m_HasDisplayCutout = false;
	int m_RenderTargetWidth = 0;
	int m_RenderTargetHeight = 0;
	bool m_RenderingToTexture = false;

	TWGLint m_MaxTexSize;

	// What the context turned out to offer, filled in by InitOpenGL. The
	// fixed function path is the only one that can be without any of it, but
	// it is answered here because that is where the context is probed.
	bool m_Has2DArrayTextures;
	TWGLenum m_2DArrayTarget;
	bool m_Has3DTextures;
	bool m_HasMipMaps;
	bool m_HasNPOTTextures;
	// Buffer objects exist from OpenGL 1.5 on. Without them the converted data
	// is drawn from memory, which is the same array, one indirection earlier.
	bool m_HasBufferObjects = false;

	bool m_HasShaders;
	EBlendMode m_LastBlendMode; // avoid all possible opengl state changes
	bool m_LastClipEnable;

	int m_OpenGLTextureLodBIAS;

	bool m_IsOpenGLES;

	bool IsTexturedState(const CCommandBuffer::SState &State);
	void SetScissor(const CCommandBuffer::SState &State);

	bool InitOpenGL(const SCommand_Init *pCommand);
	virtual bool IsNewApi() { return false; }

	void DestroyTexture(int Slot);
	bool IsTextureUpdateValid(const CCommandBuffer::SCommand_Texture_Update *pCommand) const;

	void Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand);
	void Cmd_PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand);

	virtual void Cmd_Shutdown(const SCommand_Shutdown *pCommand) {}

	// A backend that leaves a readback to the device hands the pixels over
	// from here, once they have landed; the first is run before every command,
	// the second when the frontend has to have a picture now.
	virtual void CollectFinishedReadbacks() {}
	virtual void FinishReadbacks() {}

	virtual bool Cmd_Init(const SCommand_Init *pCommand) = 0;
	virtual void Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand) = 0;
	virtual void Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand) = 0;
	virtual void Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand) = 0;
	virtual void Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand) = 0;
	virtual void Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand) = 0;
	virtual void Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand) = 0;
	virtual void Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand) = 0;
	virtual void Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand) = 0;

	virtual void Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand) = 0;
	virtual void Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand) = 0;
	virtual void Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand) = 0;

	virtual void Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand) = 0;

public:
	CCommandProcessorFragment_OpenGLBase();

	ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override;
};

#endif
