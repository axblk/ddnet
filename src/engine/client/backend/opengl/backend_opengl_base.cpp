#include "backend_opengl_base.h"

#include <base/dbg.h>
#include <base/detect.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client/backend_threaded.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(BACKEND_AS_OPENGL_ES) || !defined(CONF_BACKEND_OPENGL_ES)

#include <engine/client/blocklist_driver.h>

#ifndef BACKEND_AS_OPENGL_ES
#include <GL/glew.h>
#else
#if defined(CONF_PLATFORM_IOS)
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#else
#include <GLES3/gl3.h>
#endif
#ifndef CONF_BACKEND_OPENGL_ES3
// GLES 1 is the only place the alpha test below still exists.
#include <GLES/gl.h>
#else
#define BACKEND_GL_MODERN_API 1
#endif
#endif

// ------------ CCommandProcessorFragment_OpenGLBase
void CCommandProcessorFragment_OpenGLBase::Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand)
{
	// The viewport is given relative to the top left of the surface, whereas
	// OpenGL places it relative to the bottom left.
	m_PresentationViewportX = pCommand->m_X;
	m_PresentationViewportY = pCommand->m_SurfaceHeight - pCommand->m_Y - pCommand->m_Height;
	m_PresentationViewportWidth = pCommand->m_Width;
	m_PresentationViewportHeight = pCommand->m_Height;
	if(pCommand->m_ByResize)
		m_HasDisplayCutout = pCommand->m_Width != pCommand->m_SurfaceWidth;
	if(!m_RenderingToTexture)
		glViewport(m_PresentationViewportX, m_PresentationViewportY, m_PresentationViewportWidth, m_PresentationViewportHeight);
}

bool CCommandProcessorFragment_OpenGLBase::IsTexturedState(const CCommandBuffer::SState &State)
{
	return State.m_Texture.IsValid() && State.m_Texture.Id() < (int)m_vTextures.size();
}

void CCommandProcessorFragment_OpenGLBase::SetScissor(const CCommandBuffer::SState &State)
{
	if(State.m_ClipEnable)
	{
		if(m_RenderingToTexture)
		{
			// The front-end already expresses the clip in the target's pixels, because
			// ScreenWidth()/ScreenHeight() report the target's size while an offscreen
			// frame is open. The projection is flipped for a render target, so the clip's
			// logical top edge is the framebuffer's bottom edge.
			const int LogicalTopY = m_RenderTargetHeight - (State.m_ClipY + State.m_ClipH);
			glScissor(State.m_ClipX, LogicalTopY, State.m_ClipW, State.m_ClipH);
		}
		else
		{
			// The clip rectangle is relative to the viewport, whereas glScissor is
			// relative to the surface.
			glScissor(m_PresentationViewportX + State.m_ClipX, m_PresentationViewportY + State.m_ClipY, State.m_ClipW, State.m_ClipH);
		}
		glEnable(GL_SCISSOR_TEST);
		m_LastClipEnable = true;
	}
	else if(m_LastClipEnable)
	{
		glDisable(GL_SCISSOR_TEST);
		m_LastClipEnable = false;
	}
}

static void ParseVersionString(EBackendType BackendType, const char *pStr, int &VersionMajor, int &VersionMinor, int &VersionPatch)
{
	// If the backend is GLES, the version string starts with `OpenGL ES ` or `OpenGL ES-CM ` for older contexts, rest is the same.
	if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		const char *pSkippedPrefix;
		if((pSkippedPrefix = str_startswith(pStr, "OpenGL ES ")) != nullptr ||
			(pSkippedPrefix = str_startswith(pStr, "OpenGL ES-CM ")) != nullptr)
		{
			pStr = pSkippedPrefix;
		}
	}

	char aCurNumberStr[10];
	size_t CurNumberStrLen = 0;
	size_t TotalNumbersPassed = 0;
	int aNumbers[3] = {0};
	bool LastWasNumber = false;
	bool Error = false;
	while(true)
	{
		if(str_isnum(*pStr))
		{
			if(CurNumberStrLen >= std::size(aCurNumberStr) - 1)
			{
				Error = true;
				break;
			}
			aCurNumberStr[CurNumberStrLen++] = *pStr;
			LastWasNumber = true;
		}
		else if(LastWasNumber && (*pStr == '.' || *pStr == ' ' || *pStr == '\0'))
		{
			aCurNumberStr[CurNumberStrLen] = '\0';
			aNumbers[TotalNumbersPassed] = str_toint(aCurNumberStr);
			CurNumberStrLen = 0;
			TotalNumbersPassed++;
			LastWasNumber = false;
			if(TotalNumbersPassed == std::size(aNumbers) || *pStr != '.')
			{
				break;
			}
		}
		else
		{
			break;
		}
		++pStr;
	}

	if(Error || TotalNumbersPassed == 0)
	{
		// Use the newest supported OpenGL version if the version string could not be parsed.
		// We assume that the format was changed in a future driver that supports all OpenGL
		// capabilities that we use.
		VersionMajor = 3;
		VersionMinor = BackendType == BACKEND_TYPE_OPENGL_ES ? 0 : 3;
		VersionPatch = 0;
	}
	else
	{
		VersionMajor = aNumbers[0];
		VersionMinor = aNumbers[1];
		VersionPatch = aNumbers[2];
	}
}

#ifndef BACKEND_AS_OPENGL_ES
static LEVEL GetLogSeverity(GLenum Severity)
{
	switch(Severity)
	{
	case GL_DEBUG_SEVERITY_HIGH: return LEVEL_ERROR;
	case GL_DEBUG_SEVERITY_MEDIUM: return LEVEL_WARN;
	case GL_DEBUG_SEVERITY_LOW: return LEVEL_INFO;
	case GL_DEBUG_SEVERITY_NOTIFICATION: return LEVEL_DEBUG;
	default: dbg_assert_failed("Severity invalid: %d", (int)Severity);
	}
}

static const char *GetErrorName(GLenum Type)
{
	switch(Type)
	{
	case GL_DEBUG_TYPE_ERROR: return "ERROR";
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "DEPRECATED BEHAVIOR";
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "UNDEFINED BEHAVIOR";
	case GL_DEBUG_TYPE_PORTABILITY: return "PORTABILITY";
	case GL_DEBUG_TYPE_PERFORMANCE: return "PERFORMANCE";
	case GL_DEBUG_TYPE_OTHER: return "OTHER";
	case GL_DEBUG_TYPE_MARKER: return "MARKER";
	case GL_DEBUG_TYPE_PUSH_GROUP: return "PUSH_GROUP";
	case GL_DEBUG_TYPE_POP_GROUP: return "POP_GROUP";
	default: return "UNKNOWN";
	}
}

static const char *GetSeverityString(GLenum Severity)
{
	switch(Severity)
	{
	// All OpenGL Errors, shader compilation/linking errors, or highly-dangerous undefined behavior
	case GL_DEBUG_SEVERITY_HIGH: return "high";
	// Major performance warnings, shader compilation/linking warnings, or the use of deprecated functionality
	case GL_DEBUG_SEVERITY_MEDIUM: return "medium";
	// Redundant state change performance warning, or unimportant undefined behavior
	case GL_DEBUG_SEVERITY_LOW: return "low";
	// Anything that isn't an error or performance issue.
	case GL_DEBUG_SEVERITY_NOTIFICATION: return "notification";
	default: dbg_assert_failed("Severity invalid: %d", (int)Severity);
	}
}

static void GLAPIENTRY
GfxOpenGLMessageCallback(GLenum Source,
	GLenum Type,
	GLuint Id,
	GLenum Severity,
	GLsizei Length,
	const GLchar *pMsg,
	const void *pUserParam)
{
	log_log(GetLogSeverity(Severity), "gfx/opengl", "[%s] (importance: %s) %s", GetErrorName(Type), GetSeverityString(Severity), pMsg);
}
#endif

bool CCommandProcessorFragment_OpenGLBase::InitOpenGL(const SCommand_Init *pCommand)
{
	m_IsOpenGLES = pCommand->m_RequestedBackend == BACKEND_TYPE_OPENGL_ES;
	pCommand->m_pCapabilities->m_RenderTargets = false;

	const char *pVendorString = (const char *)glGetString(GL_VENDOR);
	dbg_assert(pVendorString != nullptr, "glGetString(GL_VENDOR) failure");
	log_info("gfx/opengl", "Vendor string: %s", pVendorString);

	// check what this context can do
	const char *pVersionString = (const char *)glGetString(GL_VERSION);
	dbg_assert(pVersionString != nullptr, "glGetString(GL_VERSION) failure");
	log_info("gfx/opengl", "Version string: %s", pVersionString);

	const char *pRendererString = (const char *)glGetString(GL_RENDERER);
	dbg_assert(pRendererString != nullptr, "glGetString(GL_RENDERER) failure");

	str_copy(pCommand->m_pVendorString, pVendorString, GPU_INFO_STRING_SIZE);
	str_copy(pCommand->m_pVersionString, pVersionString, GPU_INFO_STRING_SIZE);
	str_copy(pCommand->m_pRendererString, pRendererString, GPU_INFO_STRING_SIZE);

	// parse version string
	ParseVersionString(pCommand->m_RequestedBackend, pVersionString, pCommand->m_pCapabilities->m_ContextMajor, pCommand->m_pCapabilities->m_ContextMinor, pCommand->m_pCapabilities->m_ContextPatch);

	*pCommand->m_pInitError = 0;

	int BlocklistMajor = -1, BlocklistMinor = -1, BlocklistPatch = -1;
	bool RequiresWarning = false;
	const char *pErrString = ParseBlocklistDriverVersions(pVendorString, pVersionString, BlocklistMajor, BlocklistMinor, BlocklistPatch, RequiresWarning);
	// if the driver is buggy, and the requested GL version is the default, fallback
	if(pErrString != nullptr && pCommand->m_RequestedMajor == 3 && pCommand->m_RequestedMinor == 0 && pCommand->m_RequestedPatch == 0)
	{
		// if not already in the error state, set the GL version
		if(g_Config.m_GfxDriverIsBlocked == 0)
		{
			// fallback to known good GL version
			pCommand->m_pCapabilities->m_ContextMajor = BlocklistMajor;
			pCommand->m_pCapabilities->m_ContextMinor = BlocklistMinor;
			pCommand->m_pCapabilities->m_ContextPatch = BlocklistPatch;

			// set backend error string
			if(RequiresWarning)
				*pCommand->m_pErrStringPtr = pErrString;
			*pCommand->m_pInitError = -2;

			g_Config.m_GfxDriverIsBlocked = 1;
		}
	}
	// if the driver was in a blocked error state, but is not anymore, reset all config variables
	else if(pErrString == nullptr && g_Config.m_GfxDriverIsBlocked == 1)
	{
		pCommand->m_pCapabilities->m_ContextMajor = 3;
		pCommand->m_pCapabilities->m_ContextMinor = 0;
		pCommand->m_pCapabilities->m_ContextPatch = 0;

		// tell the caller to reinitialize the context
		*pCommand->m_pInitError = -2;

		g_Config.m_GfxDriverIsBlocked = 0;
	}

	int MajorV = pCommand->m_pCapabilities->m_ContextMajor;

	if(pCommand->m_RequestedBackend == BACKEND_TYPE_OPENGL)
	{
#ifndef BACKEND_AS_OPENGL_ES
		int MinorV = pCommand->m_pCapabilities->m_ContextMinor;
		if(*pCommand->m_pInitError == 0)
		{
			if(MajorV < pCommand->m_RequestedMajor)
			{
				*pCommand->m_pInitError = -2;
			}
			else if(MajorV == pCommand->m_RequestedMajor)
			{
				if(MinorV < pCommand->m_RequestedMinor)
				{
					*pCommand->m_pInitError = -2;
				}
				else if(MinorV == pCommand->m_RequestedMinor)
				{
					int PatchV = pCommand->m_pCapabilities->m_ContextPatch;
					if(PatchV < pCommand->m_RequestedPatch)
					{
						*pCommand->m_pInitError = -2;
					}
				}
			}
		}

		if(*pCommand->m_pInitError == 0)
		{
			MajorV = pCommand->m_RequestedMajor;
			MinorV = pCommand->m_RequestedMinor;

			m_HasNPOTTextures = true;

			if(MajorV >= 4 || (MajorV == 3 && MinorV >= 3))
			{
				// Everything the backend with programs uses is core in OpenGL
				// 3.3: array textures, sampler objects, instancing, copying
				// between buffers, and an attribute that says where it goes.
				m_HasShaders = true;

				m_HasMipMaps = true;

				pCommand->m_pCapabilities->m_RenderTargets = true;
				pCommand->m_pCapabilities->m_PlanarYuvConversion = true;

				// Both OpenGL 3.3 core and OpenGL ES 3 guarantee at least 256
				// array layers, which is exactly what a layered texture needs,
				// so there is nothing to ask the driver about.
				pCommand->m_pCapabilities->m_2DArrayTextures = true;
			}
			else
			{
				// There are no programs here, so every pipeline is carried out
				// on the vertices before they are drawn. That costs the CPU
				// what a program would have cost the GPU, and it buys the game
				// code one renderer to talk to instead of two.
				m_HasShaders = false;

				m_HasMipMaps = true;
				pCommand->m_pCapabilities->m_2DArrayTextures = false;
				m_HasNPOTTextures = GLEW_ARB_texture_non_power_of_two || pCommand->m_GlewMajor > 2;

				// Buffer objects are core from OpenGL 1.5 on, and the
				// extension before that is on everything that matters. What
				// they buy here is that a converted vertex array is uploaded
				// once instead of walked every frame.
				m_HasBufferObjects = MajorV > 1 || MinorV >= 5 || GLEW_ARB_vertex_buffer_object;

				// A layered texture has to live somewhere, and on this path
				// that is a volume. Without one there is no tile layer and no
				// point in going on, so say it here rather than in a frame.
				int Texture3DSize = 0;
				glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &Texture3DSize);
				m_Has3DTextures = Texture3DSize >= static_cast<int>(IGraphics::MAX_TEXTURE_LAYERS);
				if(!m_Has3DTextures)
				{
					*pCommand->m_pErrStringPtr = "Your GPU or driver does not support 3D textures, which this client needs to draw a map.";
					*pCommand->m_pInitError = -1;
				}
			}
		}
#endif
	}
	else if(pCommand->m_RequestedBackend == BACKEND_TYPE_OPENGL_ES)
	{
		if(MajorV < 3)
		{
			// A layered texture has nowhere to live before OpenGL ES 3: there
			// are no array textures and no volumes either, only the extension
			// nobody has. Every map needs one, so say so here rather than
			// finding out during the first tile layer.
			*pCommand->m_pErrStringPtr = "This client needs OpenGL ES 3, which your GPU or driver does not provide.";
			*pCommand->m_pInitError = -1;

			m_HasShaders = false;
			m_HasMipMaps = false;
			m_Has3DTextures = false;
			pCommand->m_pCapabilities->m_2DArrayTextures = false;
			m_HasNPOTTextures = false;
		}
		else
		{
			m_HasShaders = true;

			m_HasMipMaps = true;
			m_Has3DTextures = true;
			pCommand->m_pCapabilities->m_2DArrayTextures = true;
			m_HasNPOTTextures = true;

			// Framebuffers are core in OpenGL ES 3, and the backend reaches
			// them through the same code the desktop one does.
			pCommand->m_pCapabilities->m_RenderTargets = true;
			pCommand->m_pCapabilities->m_PlanarYuvConversion = true;
		}
	}

	if(*pCommand->m_pInitError != -2)
	{
		// set some default settings
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);

#ifndef BACKEND_GL_MODERN_API
		if(!IsNewApi())
		{
			glAlphaFunc(GL_GREATER, 0);
			glEnable(GL_ALPHA_TEST);
		}
#endif

		glDepthMask(0);

#ifndef BACKEND_AS_OPENGL_ES
		if(g_Config.m_DbgGfx != DEBUG_GFX_MODE_NONE)
		{
			if(GLEW_KHR_debug || GLEW_ARB_debug_output)
			{
				// During init, enable debug output
				if(GLEW_KHR_debug)
				{
					glEnable(GL_DEBUG_OUTPUT);
					glDebugMessageCallback((GLDEBUGPROC)GfxOpenGLMessageCallback, nullptr);
				}
				else if(GLEW_ARB_debug_output)
				{
					glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
					glDebugMessageCallbackARB((GLDEBUGPROC)GfxOpenGLMessageCallback, nullptr);
				}
				log_info("gfx/opengl", "Enabled OpenGL debug mode");
			}
			else
			{
				log_warn("gfx/opengl", "Requested OpenGL debug mode, but the driver does not support the required extension");
			}
		}
#endif

		return true;
	}
	else
	{
		return false;
	}
}

void CCommandProcessorFragment_OpenGLBase::DestroyTexture(int Slot)
{
	m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) - m_vTextures[Slot].m_MemSize, std::memory_order_relaxed);

	// OpenGL keeps deleted objects alive while earlier submitted commands still
	// reference them. CPU handle retirement is therefore sufficient here.
	if(m_vTextures[Slot].m_Framebuffer != 0)
	{
		glDeleteFramebuffers(1, &m_vTextures[Slot].m_Framebuffer);
	}
	if(m_vTextures[Slot].m_Tex != 0)
	{
		glDeleteTextures(1, &m_vTextures[Slot].m_Tex);
	}

	if(m_vTextures[Slot].m_TexSecondChannel != 0)
	{
		glDeleteTextures(1, &m_vTextures[Slot].m_TexSecondChannel);
		m_vTextures[Slot].m_TexSecondChannel = 0;
	}

	if(m_vTextures[Slot].m_Tex2DArray != 0)
	{
		glDeleteTextures(1, &m_vTextures[Slot].m_Tex2DArray);
	}

	if(IsNewApi())
	{
		if(m_vTextures[Slot].m_Sampler != 0)
		{
			glDeleteSamplers(1, &m_vTextures[Slot].m_Sampler);
		}
		if(m_vTextures[Slot].m_Sampler2DArray != 0)
		{
			glDeleteSamplers(1, &m_vTextures[Slot].m_Sampler2DArray);
		}
	}

	m_vTextures[Slot].m_Tex = 0;
	m_vTextures[Slot].m_Sampler = 0;
	m_vTextures[Slot].m_Tex2DArray = 0;
	m_vTextures[Slot].m_Sampler2DArray = 0;
	m_vTextures[Slot].m_Framebuffer = 0;
	m_vTextures[Slot].m_LastWrapMode = EWrapMode::REPEAT;
}

bool CCommandProcessorFragment_OpenGLBase::IsTextureUpdateValid(const CCommandBuffer::SCommand_Texture_Update *pCommand) const
{
	const size_t Slot = static_cast<size_t>(pCommand->m_Texture.Id());
	if(Slot >= m_vTextures.size())
		return false;
	const CTexture &Texture = m_vTextures[Slot];
	const IGraphics::CTextureRegion &Region = pCommand->m_Region;
	return pCommand->m_Format == Texture.m_Format &&
	       Region.m_X <= Texture.m_SourceWidth && Region.m_Width <= Texture.m_SourceWidth - Region.m_X &&
	       Region.m_Y <= Texture.m_SourceHeight && Region.m_Height <= Texture.m_SourceHeight - Region.m_Y;
}

void CCommandProcessorFragment_OpenGLBase::Cmd_PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
	GLint aViewport[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_VIEWPORT, aViewport);
	const int ViewportX = aViewport[0];
	const int ViewportY = aViewport[1];
	const int ViewportWidth = aViewport[2];
	const int ViewportHeight = aViewport[3];
	if(ViewportWidth <= 0 || ViewportHeight <= 0 || (pCommand->m_ReadPixel && (pCommand->m_Position.x < 0 || pCommand->m_Position.x >= ViewportWidth || pCommand->m_Position.y < 0 || pCommand->m_Position.y >= ViewportHeight)))
		return;

	const int Width = pCommand->m_ReadPixel ? 1 : ViewportWidth;
	const int Height = pCommand->m_ReadPixel ? 1 : ViewportHeight;

	if(!pCommand->m_pResult->m_Image.TryReuse(Width, Height, CImageInfo::FORMAT_RGBA))
		return;
	uint8_t *pPixelData = pCommand->m_pResult->m_Image.m_pData;

	GLint Alignment;
	glGetIntegerv(GL_PACK_ALIGNMENT, &Alignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	// The position is relative to the top left of the viewport, whereas
	// glReadPixels reads relative to the bottom left of the surface.
	glReadPixels(ViewportX + (pCommand->m_ReadPixel ? pCommand->m_Position.x : 0), ViewportY + (pCommand->m_ReadPixel ? ViewportHeight - 1 - pCommand->m_Position.y : 0), Width, Height, GL_RGBA, GL_UNSIGNED_BYTE, pPixelData);
	glPixelStorei(GL_PACK_ALIGNMENT, Alignment);

	if(!pCommand->m_ReadPixel)
	{
		const size_t RowSize = static_cast<size_t>(Width) * 4;
		for(int Y = 0; Y < Height / 2; ++Y)
		{
			const size_t TopOffset = static_cast<size_t>(Y) * RowSize;
			const size_t BottomOffset = static_cast<size_t>(Height - Y - 1) * RowSize;
			std::swap_ranges(pPixelData + TopOffset, pPixelData + TopOffset + RowSize, pPixelData + BottomOffset);
		}
	}
	for(size_t Offset = 3; Offset < pCommand->m_pResult->m_Image.DataSize(); Offset += 4)
		pPixelData[Offset] = 255;
	pCommand->m_pResult->m_Ok = true;
}

CCommandProcessorFragment_OpenGLBase::CCommandProcessorFragment_OpenGLBase()
{
	m_vTextures.resize(CCommandBuffer::MAX_TEXTURES);
	m_HasShaders = false;
}

static const CCommandBuffer::SState *RenderCommandState(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW: return &static_cast<const CCommandBuffer::SCommand_Draw *>(pCommand)->m_State;
	case CCommandBuffer::CMD_DRAW_INDEXED: return &static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand)->m_State;
	default: return nullptr;
	}
}

static const IGraphics::CBufferHandle *RenderCommandVertexBuffer(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW_INDEXED: return &static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand)->m_VertexBuffer;
	default: return nullptr;
	}
}

ERunCommandReturnTypes CCommandProcessorFragment_OpenGLBase::RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
{
	using EReturn = ERunCommandReturnTypes;
	const auto Handled = EReturn::RUN_COMMAND_COMMAND_HANDLED;

	CollectFinishedReadbacks();
	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_OpenGLBase::CMD_INIT:
		m_TextureHandles.Clear();
		m_BufferHandles.Clear();
		break;
	case CCommandProcessorFragment_OpenGLBase::CMD_SHUTDOWN:
		break;
	case CCommandBuffer::CMD_TEXTURE_CREATE:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand);
		if(!m_TextureHandles.Activate(pCommand->m_Texture))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_TEXTURE_DESTROY:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand);
		if(!m_TextureHandles.Release(pCommand->m_Texture))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_TEXTURE_UPDATE:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand);
		if(!m_TextureHandles.IsActive(pCommand->m_Texture))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_TEXTURE_READBACK:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Texture_Readback *>(pBaseCommand);
		if(!m_TextureHandles.IsActive(pCommand->m_Texture))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_BEGIN_RENDER_PASS:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_BeginRenderPass *>(pBaseCommand);
		if(pCommand->m_Desc.m_ColorTarget.IsValid() && !m_TextureHandles.IsActive(pCommand->m_Desc.m_ColorTarget))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_CREATE_BUFFER_OBJECT:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(pBaseCommand);
		if(!m_BufferHandles.Activate(pCommand->m_Buffer))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand);
		if(!m_BufferHandles.IsActive(pCommand->m_Buffer))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_DELETE_BUFFER_OBJECT:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand);
		if(!m_BufferHandles.Release(pCommand->m_Buffer))
			return Handled;
		break;
	}
	default:
		break;
	}

	const CCommandBuffer::SState *pState = RenderCommandState(pBaseCommand);
	if(pState != nullptr && pState->m_Texture.IsValid() && !m_TextureHandles.IsActive(pState->m_Texture))
		return Handled;
	const IGraphics::CBufferHandle *pVertexBuffer = RenderCommandVertexBuffer(pBaseCommand);
	if(pVertexBuffer != nullptr && !m_BufferHandles.IsActive(*pVertexBuffer))
		return Handled;
	if(pBaseCommand->m_Cmd == CCommandBuffer::CMD_DRAW)
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Draw *>(pBaseCommand);
		if(pCommand->m_Program >= EPipelineProgram::COUNT)
			return Handled;
		if(IsNewApi() && pCommand->m_PrimitiveType == EPrimitiveType::QUADS && !m_BufferHandles.IsActive(pCommand->m_IndexBuffer))
			return Handled;
	}
	if(pBaseCommand->m_Cmd == CCommandBuffer::CMD_DRAW_INDEXED)
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand);
		if(pCommand->m_Program >= EPipelineProgram::COUNT)
			return Handled;
		if(!m_BufferHandles.IsActive(pCommand->m_IndexBuffer))
			return Handled;
		if(pCommand->m_Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE && !pCommand->m_State.m_Texture.IsValid())
			return Handled;
	}

	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_OpenGLBase::CMD_INIT:
		Cmd_Init(static_cast<const SCommand_Init *>(pBaseCommand));
		break;
	case CCommandProcessorFragment_OpenGLBase::CMD_SHUTDOWN:
		Cmd_Shutdown(static_cast<const SCommand_Shutdown *>(pBaseCommand));
		m_TextureHandles.Clear();
		m_BufferHandles.Clear();
		break;
	case CCommandBuffer::CMD_TEXTURE_CREATE:
		Cmd_Texture_Create(static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_DESTROY:
		Cmd_Texture_Destroy(static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_UPDATE:
		Cmd_Texture_Update(static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_READBACK:
		Cmd_Texture_Readback(static_cast<const CCommandBuffer::SCommand_Texture_Readback *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_FINISH_READBACKS:
		FinishReadbacks();
		break;
	case CCommandBuffer::CMD_CLEAR:
		Cmd_Clear(static_cast<const CCommandBuffer::SCommand_Clear *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_BEGIN_RENDER_PASS:
		Cmd_BeginRenderPass(static_cast<const CCommandBuffer::SCommand_BeginRenderPass *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_END_RENDER_PASS:
		Cmd_EndRenderPass(static_cast<const CCommandBuffer::SCommand_EndRenderPass *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_FLUSH_RENDER_PASS:
		break;
	case CCommandBuffer::CMD_DRAW:
		Cmd_Draw(static_cast<const CCommandBuffer::SCommand_Draw *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_PRESENTATION_TARGET_READBACK:
		Cmd_PresentationTargetReadback(static_cast<const CCommandBuffer::SCommand_PresentationTarget_Readback *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_UPDATE_VIEWPORT:
		Cmd_Update_Viewport(static_cast<const CCommandBuffer::SCommand_Update_Viewport *>(pBaseCommand));
		break;

	case CCommandBuffer::CMD_CREATE_BUFFER_OBJECT: Cmd_CreateBufferObject(static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT: Cmd_RecreateBufferObject(static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_DELETE_BUFFER_OBJECT: Cmd_DeleteBufferObject(static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand)); break;

	case CCommandBuffer::CMD_DRAW_INDEXED: Cmd_DrawIndexed(static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand)); break;
	default: return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_UNHANDLED;
	}

	return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED;
}

#ifdef BACKEND_GL_MODERN_API
#undef BACKEND_GL_MODERN_API
#endif

#endif
