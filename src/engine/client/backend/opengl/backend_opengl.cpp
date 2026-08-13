#include "backend_opengl.h"

#include <base/dbg.h>
#include <base/detect.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client/backend_sdl.h>
#include <engine/graphics.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#if defined(BACKEND_AS_OPENGL_ES) || !defined(CONF_BACKEND_OPENGL_ES)

#include <engine/client/backend/glsl_shader_compiler.h>
#include <engine/client/backend/opengl/opengl_sl.h>
#include <engine/client/backend/opengl/opengl_sl_program.h>
#include <engine/client/blocklist_driver.h>
#include <engine/gfx/image_manipulation.h>

#ifndef BACKEND_AS_OPENGL_ES
#include <GL/glew.h>
#else
#include <GLES3/gl3.h>
// GLES doesn't support GL_QUADS, but the code is also never executed
#define GL_QUADS GL_TRIANGLES
#ifndef CONF_BACKEND_OPENGL_ES3
#include <GLES/gl.h>
#define glOrtho glOrthof
#else
#define BACKEND_GL_MODERN_API 1
#endif
#endif

// ------------ CCommandProcessorFragment_OpenGL
void CCommandProcessorFragment_OpenGL::Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand)
{
	m_PresentationViewportX = pCommand->m_X;
	m_PresentationViewportY = pCommand->m_Y;
	m_PresentationViewportWidth = pCommand->m_Width;
	m_PresentationViewportHeight = pCommand->m_Height;
	if(pCommand->m_ByResize)
	{
		m_CanvasWidth = (uint32_t)pCommand->m_Width;
		m_CanvasHeight = (uint32_t)pCommand->m_Height;
	}
	if(!m_RenderingToTexture)
		glViewport(pCommand->m_X, pCommand->m_Y, pCommand->m_Width, pCommand->m_Height);
}

size_t CCommandProcessorFragment_OpenGL::GLFormatToPixelSize(int GLFormat)
{
	switch(GLFormat)
	{
	case GL_RGBA: return 4;
	case GL_RGB: return 3;
	case GL_RED: return 1;
	case GL_ALPHA: return 1;
	default: return 4;
	}
}

bool CCommandProcessorFragment_OpenGL::IsTexturedState(const CCommandBuffer::SState &State)
{
	return State.m_Texture.IsValid() && State.m_Texture.Id() < (int)m_vTextures.size();
}

void CCommandProcessorFragment_OpenGL::SetScissor(const CCommandBuffer::SState &State)
{
	if(State.m_ClipEnable)
	{
		if(m_RenderingToTexture && m_CanvasWidth != 0 && m_CanvasHeight != 0)
		{
			const int LogicalTopY = static_cast<int>(m_CanvasHeight) - (State.m_ClipY + State.m_ClipH);
			const float ScaleX = static_cast<float>(m_RenderTargetWidth) / static_cast<float>(m_CanvasWidth);
			const float ScaleY = static_cast<float>(m_RenderTargetHeight) / static_cast<float>(m_CanvasHeight);
			glScissor(static_cast<int>(State.m_ClipX * ScaleX), static_cast<int>(LogicalTopY * ScaleY), static_cast<int>(State.m_ClipW * ScaleX), static_cast<int>(State.m_ClipH * ScaleY));
		}
		else
			glScissor(State.m_ClipX, State.m_ClipY, State.m_ClipW, State.m_ClipH);
		glEnable(GL_SCISSOR_TEST);
		m_LastClipEnable = true;
	}
	else if(m_LastClipEnable)
	{
		glDisable(GL_SCISSOR_TEST);
		m_LastClipEnable = false;
	}
}

void CCommandProcessorFragment_OpenGL::SetState(const CCommandBuffer::SState &State, bool Use2DArrayTextures)
{
#ifndef BACKEND_GL_MODERN_API
	// blend
	switch(State.m_BlendMode)
	{
	case EBlendMode::NONE:
		glDisable(GL_BLEND);
		break;
	case EBlendMode::ALPHA:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case EBlendMode::ADDITIVE:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		break;
	default:
		dbg_assert_failed("Invalid blend mode: %d", (int)State.m_BlendMode);
	};
	m_LastBlendMode = State.m_BlendMode;

	SetScissor(State);

	glDisable(GL_TEXTURE_2D);
	if(!m_HasShaders)
	{
		if(m_Has3DTextures)
			glDisable(GL_TEXTURE_3D);
		if(m_Has2DArrayTextures)
		{
			glDisable(m_2DArrayTarget);
		}
	}

	if(m_HasShaders && IsNewApi())
	{
		glBindSampler(0, 0);
	}

	// texture
	if(IsTexturedState(State))
	{
		if(!Use2DArrayTextures)
		{
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, m_vTextures[State.m_Texture.Id()].m_Tex);

			if(m_vTextures[State.m_Texture.Id()].m_LastWrapMode != State.m_WrapMode)
			{
				switch(State.m_WrapMode)
				{
				case EWrapMode::REPEAT:
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
					break;
				case EWrapMode::CLAMP:
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					break;
				default:
					dbg_assert_failed("Invalid wrap mode: %d", (int)State.m_WrapMode);
				};
				m_vTextures[State.m_Texture.Id()].m_LastWrapMode = State.m_WrapMode;
			}
		}
		else if(m_Has2DArrayTextures)
		{
			if(!m_HasShaders)
				glEnable(m_2DArrayTarget);
			glBindTexture(m_2DArrayTarget, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
		}
		else if(m_Has3DTextures)
		{
			if(!m_HasShaders)
				glEnable(GL_TEXTURE_3D);
			glBindTexture(GL_TEXTURE_3D, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
		}
		else
		{
			dbg_assert_failed("Should have either 2D, 3D or no texture array support");
		}
	}

	// screen mapping
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(State.m_ScreenTL.x, State.m_ScreenBR.x, State.m_ScreenBR.y, State.m_ScreenTL.y, -10.0f, 10.f);
#endif
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

bool CCommandProcessorFragment_OpenGL::InitOpenGL(const SCommand_Init *pCommand)
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

			pCommand->m_pCapabilities->m_NPOTTextures = true;
			pCommand->m_pCapabilities->m_TrianglesAsQuads = false;

			if(MajorV >= 4 || (MajorV == 3 && MinorV == 3))
			{
				pCommand->m_pCapabilities->m_ArrayColorPipelines = true;
				pCommand->m_pCapabilities->m_QuadPipelines = true;
				pCommand->m_pCapabilities->m_DualAtlasPipeline = true;
				pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = true;
				pCommand->m_pCapabilities->m_ShaderSupport = true;

				pCommand->m_pCapabilities->m_MipMapping = true;
				pCommand->m_pCapabilities->m_3DTextures = true;
				pCommand->m_pCapabilities->m_2DArrayTextures = true;

				pCommand->m_pCapabilities->m_TrianglesAsQuads = true;
				pCommand->m_pCapabilities->m_RenderTargets = true;
			}
			else if(MajorV == 3)
			{
				pCommand->m_pCapabilities->m_MipMapping = true;
				// check for context native 2D array texture size
				pCommand->m_pCapabilities->m_3DTextures = false;
				pCommand->m_pCapabilities->m_2DArrayTextures = false;
				pCommand->m_pCapabilities->m_ShaderSupport = true;

				int TextureLayers = 0;
				glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &TextureLayers);
				if(TextureLayers >= static_cast<int>(IGraphics::MAX_TEXTURE_LAYERS))
				{
					pCommand->m_pCapabilities->m_2DArrayTextures = true;
				}

				int Texture3DSize = 0;
				glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &Texture3DSize);
				if(Texture3DSize >= static_cast<int>(IGraphics::MAX_TEXTURE_LAYERS))
				{
					pCommand->m_pCapabilities->m_3DTextures = true;
				}

				if(!pCommand->m_pCapabilities->m_3DTextures && !pCommand->m_pCapabilities->m_2DArrayTextures)
				{
					*pCommand->m_pInitError = -2;
					pCommand->m_pCapabilities->m_ContextMajor = 1;
					pCommand->m_pCapabilities->m_ContextMinor = 5;
					pCommand->m_pCapabilities->m_ContextPatch = 0;
				}

				pCommand->m_pCapabilities->m_ArrayColorPipelines = pCommand->m_pCapabilities->m_2DArrayTextures;
				pCommand->m_pCapabilities->m_QuadPipelines = false;
				pCommand->m_pCapabilities->m_DualAtlasPipeline = false;
				pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = false;
			}
			else if(MajorV == 2)
			{
				pCommand->m_pCapabilities->m_MipMapping = true;
				// check for context extension: 2D array texture and its max size
				pCommand->m_pCapabilities->m_3DTextures = false;
				pCommand->m_pCapabilities->m_2DArrayTextures = false;

				pCommand->m_pCapabilities->m_ShaderSupport = false;

				int Texture3DSize = 0;
				glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &Texture3DSize);
				if(Texture3DSize >= static_cast<int>(IGraphics::MAX_TEXTURE_LAYERS))
				{
					pCommand->m_pCapabilities->m_3DTextures = true;
				}

				pCommand->m_pCapabilities->m_ArrayColorPipelines = false;
				pCommand->m_pCapabilities->m_QuadPipelines = false;
				pCommand->m_pCapabilities->m_DualAtlasPipeline = false;
				pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = false;

				pCommand->m_pCapabilities->m_NPOTTextures = GLEW_ARB_texture_non_power_of_two || pCommand->m_GlewMajor > 2;

				if(!pCommand->m_pCapabilities->m_NPOTTextures || (!pCommand->m_pCapabilities->m_3DTextures && !pCommand->m_pCapabilities->m_2DArrayTextures))
				{
					*pCommand->m_pInitError = -2;
					pCommand->m_pCapabilities->m_ContextMajor = 1;
					pCommand->m_pCapabilities->m_ContextMinor = 5;
					pCommand->m_pCapabilities->m_ContextPatch = 0;
				}
			}
			else if(MajorV < 2)
			{
				pCommand->m_pCapabilities->m_ArrayColorPipelines = false;
				pCommand->m_pCapabilities->m_QuadPipelines = false;
				pCommand->m_pCapabilities->m_DualAtlasPipeline = false;
				pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = false;
				pCommand->m_pCapabilities->m_ShaderSupport = false;

				pCommand->m_pCapabilities->m_MipMapping = false;
				pCommand->m_pCapabilities->m_3DTextures = false;
				pCommand->m_pCapabilities->m_2DArrayTextures = false;
				pCommand->m_pCapabilities->m_NPOTTextures = false;
			}
		}
#endif
	}
	else if(pCommand->m_RequestedBackend == BACKEND_TYPE_OPENGL_ES)
	{
		if(MajorV < 3)
		{
			pCommand->m_pCapabilities->m_ArrayColorPipelines = false;
			pCommand->m_pCapabilities->m_QuadPipelines = false;
			pCommand->m_pCapabilities->m_DualAtlasPipeline = false;
			pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = false;
			pCommand->m_pCapabilities->m_ShaderSupport = false;

			pCommand->m_pCapabilities->m_MipMapping = false;
			pCommand->m_pCapabilities->m_3DTextures = false;
			pCommand->m_pCapabilities->m_2DArrayTextures = false;
			pCommand->m_pCapabilities->m_NPOTTextures = false;

			pCommand->m_pCapabilities->m_TrianglesAsQuads = false;
		}
		else
		{
			pCommand->m_pCapabilities->m_ArrayColorPipelines = true;
			pCommand->m_pCapabilities->m_QuadPipelines = true;
			pCommand->m_pCapabilities->m_DualAtlasPipeline = true;
			pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = true;
			pCommand->m_pCapabilities->m_ShaderSupport = true;

			pCommand->m_pCapabilities->m_MipMapping = true;
			pCommand->m_pCapabilities->m_3DTextures = true;
			pCommand->m_pCapabilities->m_2DArrayTextures = true;
			pCommand->m_pCapabilities->m_NPOTTextures = true;

			pCommand->m_pCapabilities->m_TrianglesAsQuads = true;
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

bool CCommandProcessorFragment_OpenGL::Cmd_Init(const SCommand_Init *pCommand)
{
	if(!InitOpenGL(pCommand))
		return false;

	m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
	m_pTextureMemoryUsage->store(0, std::memory_order_relaxed);
	m_MaxTexSize = -1;

	m_OpenGLTextureLodBIAS = 0;

	m_Has2DArrayTextures = pCommand->m_pCapabilities->m_2DArrayTextures;
	m_2DArrayTarget = GL_TEXTURE_2D_ARRAY;

	m_Has3DTextures = pCommand->m_pCapabilities->m_3DTextures;
	m_HasMipMaps = pCommand->m_pCapabilities->m_MipMapping;
	m_HasNPOTTextures = pCommand->m_pCapabilities->m_NPOTTextures;

	m_LastBlendMode = EBlendMode::ALPHA;
	m_LastClipEnable = false;

	return true;
}

void CCommandProcessorFragment_OpenGL::TextureUpdate(int Slot, int X, int Y, int Width, int Height, int GLFormat, uint8_t *pTexData)
{
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedTexData(nullptr, free);
	glBindTexture(GL_TEXTURE_2D, m_vTextures[Slot].m_Tex);

	if(!m_HasNPOTTextures)
	{
		float ResizeW = m_vTextures[Slot].m_ResizeWidth;
		float ResizeH = m_vTextures[Slot].m_ResizeHeight;
		if(ResizeW > 0 && ResizeH > 0)
		{
			int ResizedW = (int)(Width * ResizeW);
			int ResizedH = (int)(Height * ResizeH);

			pOwnedTexData.reset(ResizeImage(pTexData, Width, Height, ResizedW, ResizedH, GLFormatToPixelSize(GLFormat)));
			pTexData = pOwnedTexData.get();

			Width = ResizedW;
			Height = ResizedH;
		}
	}

	if(m_vTextures[Slot].m_RescaleCount > 0)
	{
		int OldWidth = Width;
		int OldHeight = Height;
		for(int i = 0; i < m_vTextures[Slot].m_RescaleCount; ++i)
		{
			Width >>= 1;
			Height >>= 1;

			X /= 2;
			Y /= 2;
		}

		pOwnedTexData.reset(ResizeImage(pTexData, OldWidth, OldHeight, Width, Height, GLFormatToPixelSize(GLFormat)));
		pTexData = pOwnedTexData.get();
	}

	glTexSubImage2D(GL_TEXTURE_2D, 0, X, Y, Width, Height, GLFormat, GL_UNSIGNED_BYTE, pTexData);
}

void CCommandProcessorFragment_OpenGL::DestroyTexture(int Slot)
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

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand)
{
	DestroyTexture(pCommand->m_Texture.Id());
}

void CCommandProcessorFragment_OpenGL::TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData)
{
#ifndef BACKEND_GL_MODERN_API
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedTexData(nullptr, free);
	int Width = Desc.m_Width;
	int Height = Desc.m_Height;
	const int GLFormat = Desc.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? GL_RGBA : GL_ALPHA;
	const int GLStoreFormat = GLFormat;

	if(m_MaxTexSize == -1)
	{
		// fix the alignment to allow even 1byte changes, e.g. for alpha components
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_MaxTexSize);
	}

	while(Slot >= (int)m_vTextures.size())
		m_vTextures.resize(m_vTextures.size() * 2);
	m_vTextures[Slot].m_SourceWidth = Desc.m_Width;
	m_vTextures[Slot].m_SourceHeight = Desc.m_Height;
	m_vTextures[Slot].m_Format = Desc.m_Format;
	m_vTextures[Slot].m_Usage = Desc.m_Usage;

	m_vTextures[Slot].m_ResizeWidth = -1.f;
	m_vTextures[Slot].m_ResizeHeight = -1.f;

	if(!m_HasNPOTTextures)
	{
		int PowerOfTwoWidth = HighestBit(Width);
		int PowerOfTwoHeight = HighestBit(Height);
		if(Width != PowerOfTwoWidth || Height != PowerOfTwoHeight)
		{
			pOwnedTexData.reset(ResizeImage(pTexData, Width, Height, PowerOfTwoWidth, PowerOfTwoHeight, GLFormatToPixelSize(GLFormat)));
			pTexData = pOwnedTexData.get();

			m_vTextures[Slot].m_ResizeWidth = (float)PowerOfTwoWidth / (float)Width;
			m_vTextures[Slot].m_ResizeHeight = (float)PowerOfTwoHeight / (float)Height;

			Width = PowerOfTwoWidth;
			Height = PowerOfTwoHeight;
		}
	}

	int RescaleCount = 0;
	if(GLFormat == GL_RGBA)
	{
		int OldWidth = Width;
		int OldHeight = Height;
		bool NeedsResize = false;

		if(Width > m_MaxTexSize || Height > m_MaxTexSize)
		{
			do
			{
				Width >>= 1;
				Height >>= 1;
				++RescaleCount;
			} while(Width > m_MaxTexSize || Height > m_MaxTexSize);
			NeedsResize = true;
		}

		if(NeedsResize)
		{
			pOwnedTexData.reset(ResizeImage(pTexData, OldWidth, OldHeight, Width, Height, GLFormatToPixelSize(GLFormat)));
			pTexData = pOwnedTexData.get();
		}
	}
	m_vTextures[Slot].m_Width = Width;
	m_vTextures[Slot].m_Height = Height;
	m_vTextures[Slot].m_RescaleCount = RescaleCount;

	const size_t PixelSize = GLFormatToPixelSize(GLFormat);

	if(Desc.m_Create2D)
	{
		glGenTextures(1, &m_vTextures[Slot].m_Tex);
		glBindTexture(GL_TEXTURE_2D, m_vTextures[Slot].m_Tex);
	}

	if(Desc.m_Mipmaps == IGraphics::ETextureMipmaps::NONE || !m_HasMipMaps)
	{
		if(Desc.m_Create2D)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GLStoreFormat, Width, Height, 0, GLFormat, GL_UNSIGNED_BYTE, pTexData);
		}
	}
	else
	{
		if(Desc.m_Create2D)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);

#ifndef BACKEND_AS_OPENGL_ES
			if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));
#endif

			glTexImage2D(GL_TEXTURE_2D, 0, GLStoreFormat, Width, Height, 0, GLFormat, GL_UNSIGNED_BYTE, pTexData);
		}

		if(Desc.m_Layering != IGraphics::ETextureLayering::NONE)
		{
			const bool Is3DTexture = Desc.m_Layering == IGraphics::ETextureLayering::VOLUME_3D;
			const int LayerColumns = Desc.m_LayerColumns;
			const int LayerRows = Desc.m_LayerRows;
			const int LayerCount = static_cast<int>(Desc.LayerCount());

			glGenTextures(1, &m_vTextures[Slot].m_Tex2DArray);

			GLenum Target = GL_TEXTURE_3D;

			if(Is3DTexture)
			{
				Target = GL_TEXTURE_3D;
			}
			else
			{
				Target = m_2DArrayTarget;
			}

			glBindTexture(Target, m_vTextures[Slot].m_Tex2DArray);

			if(IsNewApi())
			{
				glGenSamplers(1, &m_vTextures[Slot].m_Sampler2DArray);
				glBindSampler(0, m_vTextures[Slot].m_Sampler2DArray);
			}

			glTexParameteri(Target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			if(Is3DTexture)
			{
				glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				if(IsNewApi())
					glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			}
			else
			{
				glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(Target, GL_GENERATE_MIPMAP, GL_TRUE);
				if(IsNewApi())
					glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			}

			glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(Target, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);

#ifndef BACKEND_AS_OPENGL_ES
			if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
				glTexParameterf(Target, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));
#endif

			if(IsNewApi())
			{
				glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);

#ifndef BACKEND_AS_OPENGL_ES
				if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
					glSamplerParameterf(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));
#endif

				glBindSampler(0, 0);
			}

			int ConvertWidth = Width;
			int ConvertHeight = Height;

			if(ConvertWidth == 0 || (ConvertWidth % LayerColumns) != 0 || ConvertHeight == 0 || (ConvertHeight % LayerRows) != 0)
			{
				int NewWidth = std::max(HighestBit(ConvertWidth / LayerColumns), 1) * LayerColumns;
				int NewHeight = std::max(HighestBit(ConvertHeight / LayerRows), 1) * LayerRows;
				pOwnedTexData.reset(ResizeImage(pTexData, ConvertWidth, ConvertHeight, NewWidth, NewHeight, GLFormatToPixelSize(GLFormat)));
				log_debug("gfx/opengl", "3D/2D array texture was resized. Slot=%d Size=(%d, %d) Resized=(%d, %d)", Slot, ConvertWidth, ConvertHeight, NewWidth, NewHeight);

				ConvertWidth = NewWidth;
				ConvertHeight = NewHeight;

				pTexData = pOwnedTexData.get();
			}

			int Image3DWidth, Image3DHeight;
			std::unique_ptr<uint8_t, decltype(&free)> pImageData3D(static_cast<uint8_t *>(malloc((size_t)PixelSize * ConvertWidth * ConvertHeight)), free);
			dbg_assert(pImageData3D != nullptr, "Failed to allocate 2D array texture conversion memory");
			Texture2DTo3D(pTexData, ConvertWidth, ConvertHeight, PixelSize, LayerColumns, LayerRows, pImageData3D.get(), Image3DWidth, Image3DHeight);
			glTexImage3D(Target, 0, GLStoreFormat, Image3DWidth, Image3DHeight, LayerCount, 0, GLFormat, GL_UNSIGNED_BYTE, pImageData3D.get());
		}
	}

	// This is the initial value for the wrap modes
	m_vTextures[Slot].m_LastWrapMode = EWrapMode::REPEAT;

	// calculate memory usage
	m_vTextures[Slot].m_MemSize = (size_t)Width * Height * PixelSize;
	while(Width > 2 && Height > 2)
	{
		Width >>= 1;
		Height >>= 1;
		m_vTextures[Slot].m_MemSize += (size_t)Width * Height * PixelSize;
	}
	m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) + m_vTextures[Slot].m_MemSize, std::memory_order_relaxed);

#endif
}

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand)
{
	TextureCreate(pCommand->m_Texture.Id(), pCommand->m_Desc, pCommand->m_pData);
}

bool CCommandProcessorFragment_OpenGL::IsTextureUpdateValid(const CCommandBuffer::SCommand_Texture_Update *pCommand) const
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

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand)
{
	if(!IsTextureUpdateValid(pCommand))
		return;
	const int GLFormat = pCommand->m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? GL_RGBA : GL_ALPHA;
	TextureUpdate(pCommand->m_Texture.Id(), static_cast<int>(pCommand->m_Region.m_X), static_cast<int>(pCommand->m_Region.m_Y), static_cast<int>(pCommand->m_Region.m_Width), static_cast<int>(pCommand->m_Region.m_Height), GLFormat, pCommand->m_pData);
}

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
}

void CCommandProcessorFragment_OpenGL::Cmd_TextureBinding_Destroy(const CCommandBuffer::SCommand_TextureBinding_Destroy *pCommand)
{
	m_vTextureBindings[pCommand->m_Binding.Id()] = {};
}

void CCommandProcessorFragment_OpenGL::Cmd_TextureBinding_Create(const CCommandBuffer::SCommand_TextureBinding_Create *pCommand)
{
	if(static_cast<size_t>(pCommand->m_Binding.Id()) >= m_vTextureBindings.size())
		m_vTextureBindings.resize(pCommand->m_Binding.Id() + 1);
	m_vTextureBindings[pCommand->m_Binding.Id()] = pCommand->m_Desc;
}

void CCommandProcessorFragment_OpenGL::Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand)
{
	// if clip is still active, force disable it for clearing, enable it again afterwards
	bool ClipWasEnabled = m_LastClipEnable;
	if(ClipWasEnabled)
	{
		glDisable(GL_SCISSOR_TEST);
	}
	glClearColor(pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if(ClipWasEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
	}
}

void CCommandProcessorFragment_OpenGL::Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand)
{
#ifndef BACKEND_GL_MODERN_API
	if(PipelineProgram(pCommand->m_Pipeline) != EPipelineProgram::PRIMITIVE)
		return;
	const auto *pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
	const uint32_t VerticesPerPrim = VerticesPerPrimitive(pCommand->m_PrimitiveType);
	if(pVertices == nullptr || VerticesPerPrim == 0 || pCommand->m_VertexCount == 0 || pCommand->m_VertexCount % VerticesPerPrim != 0)
		return;
	const uint32_t PrimitiveCount = pCommand->m_VertexCount / VerticesPerPrim;
	SetState(pCommand->m_State);

	glVertexPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char *)pVertices);
	glTexCoordPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char *)pVertices + sizeof(float) * 2);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CCommandBuffer::SVertex), (char *)pVertices + sizeof(float) * 4);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	switch(pCommand->m_PrimitiveType)
	{
	case EPrimitiveType::QUADS:
#ifndef BACKEND_AS_OPENGL_ES
		glDrawArrays(GL_QUADS, 0, PrimitiveCount * 4);
#endif
		break;
	case EPrimitiveType::LINES:
		glDrawArrays(GL_LINES, 0, PrimitiveCount * 2);
		break;
	case EPrimitiveType::TRIANGLES:
		glDrawArrays(GL_TRIANGLES, 0, PrimitiveCount * 3);
		break;
	default:
		dbg_assert_failed("Invalid primitive type: %d", (int)pCommand->m_PrimitiveType);
	};
#endif
}

void CCommandProcessorFragment_OpenGL::Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
#ifndef BACKEND_GL_MODERN_API
	const auto *pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
	const auto *pRanges = pCommand->m_RangeData.Get<CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange>(pCommand->m_RangeCount);
	const void *pIndices;
	GLenum IndexType;
	size_t IndexElementSize;
	if(pCommand->m_IndexType == IGraphics::EIndexType::UINT16)
	{
		pIndices = pCommand->m_IndexData.Get<uint16_t>(pCommand->m_IndexCount);
		IndexType = GL_UNSIGNED_SHORT;
		IndexElementSize = sizeof(uint16_t);
	}
	else
	{
		pIndices = pCommand->m_IndexData.Get<uint32_t>(pCommand->m_IndexCount);
		IndexType = GL_UNSIGNED_INT;
		IndexElementSize = sizeof(uint32_t);
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	std::vector<CCommandBuffer::SVertex> vExpandedVertices;
	for(uint32_t RangeIndex = 0; RangeIndex < pCommand->m_RangeCount; ++RangeIndex)
	{
		const auto &Range = pRanges[RangeIndex];
		const auto *pRangeVertices = pVertices + Range.m_VertexOffset;
		bool DrawExpanded = false;
#if defined(BACKEND_AS_OPENGL_ES) && !defined(CONF_BACKEND_OPENGL_ES3)
		DrawExpanded = pCommand->m_IndexType == IGraphics::EIndexType::UINT32;
		if(DrawExpanded)
		{
			const auto *pWideIndices = static_cast<const uint32_t *>(pIndices) + Range.m_FirstIndex;
			vExpandedVertices.resize(Range.m_IndexCount);
			for(uint32_t Index = 0; Index < Range.m_IndexCount; ++Index)
				vExpandedVertices[Index] = pRangeVertices[pWideIndices[Index]];
			pRangeVertices = vExpandedVertices.data();
		}
#endif
		SetState(Range.m_State);
		glVertexPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), reinterpret_cast<const char *>(pRangeVertices));
		glTexCoordPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), reinterpret_cast<const char *>(pRangeVertices) + sizeof(float) * 2);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CCommandBuffer::SVertex), reinterpret_cast<const char *>(pRangeVertices) + sizeof(float) * 4);
		if(DrawExpanded)
			glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(Range.m_IndexCount));
		else
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(Range.m_IndexCount), IndexType, static_cast<const uint8_t *>(pIndices) + static_cast<size_t>(Range.m_FirstIndex) * IndexElementSize);
	}
#endif
}

void CCommandProcessorFragment_OpenGL::Cmd_PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
	GLint aViewport[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_VIEWPORT, aViewport);
	const int ViewportWidth = aViewport[2];
	const int ViewportHeight = aViewport[3];
	if(ViewportWidth <= 0 || ViewportHeight <= 0 || (pCommand->m_ReadPixel && (pCommand->m_Position.x < 0 || pCommand->m_Position.x >= ViewportWidth || pCommand->m_Position.y < 0 || pCommand->m_Position.y >= ViewportHeight)))
		return;

	const int Width = pCommand->m_ReadPixel ? 1 : ViewportWidth;
	const int Height = pCommand->m_ReadPixel ? 1 : ViewportHeight;

	pCommand->m_pResult->m_Image.m_Width = Width;
	pCommand->m_pResult->m_Image.m_Height = Height;
	pCommand->m_pResult->m_Image.m_Format = CImageInfo::FORMAT_RGBA;
	pCommand->m_pResult->m_Image.Allocate();
	uint8_t *pPixelData = pCommand->m_pResult->m_Image.m_pData;
	if(pPixelData == nullptr)
		return;

	GLint Alignment;
	glGetIntegerv(GL_PACK_ALIGNMENT, &Alignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(pCommand->m_ReadPixel ? pCommand->m_Position.x : 0, pCommand->m_ReadPixel ? ViewportHeight - 1 - pCommand->m_Position.y : 0, Width, Height, GL_RGBA, GL_UNSIGNED_BYTE, pPixelData);
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

CCommandProcessorFragment_OpenGL::CCommandProcessorFragment_OpenGL()
{
	m_vTextures.resize(CCommandBuffer::MAX_TEXTURES);
	m_HasShaders = false;
}

void CCommandProcessorFragment_OpenGL::Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand)
{
	// Legacy backends only advertise the implicit presentation target.
	m_RenderingToTexture = false;
	if(pCommand->m_Desc.m_LoadOp == IGraphics::ERenderPassLoadOp::CLEAR)
	{
		CCommandBuffer::SCommand_Clear Clear;
		Clear.m_Color = pCommand->m_Desc.m_ClearColor;
		Clear.m_ForceClear = true;
		Cmd_Clear(&Clear);
	}
}

void CCommandProcessorFragment_OpenGL::Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand)
{
	m_RenderingToTexture = false;
}

static const CCommandBuffer::SState *RenderCommandState(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW: return &static_cast<const CCommandBuffer::SCommand_Draw *>(pCommand)->m_State;
	case CCommandBuffer::CMD_DRAW_INDEXED:
	{
		const auto *pDrawCommand = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand);
		return pDrawCommand->IsTransient() ? nullptr : &pDrawCommand->m_State;
	}
	default: return nullptr;
	}
}

static const IGraphics::CBufferContainerHandle *RenderCommandBufferContainer(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW_INDEXED:
	{
		const auto *pDrawCommand = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand);
		return pDrawCommand->IsTransient() ? nullptr : &pDrawCommand->m_BufferContainer;
	}
	default: return nullptr;
	}
}

ERunCommandReturnTypes CCommandProcessorFragment_OpenGL::RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
{
	using EReturn = ERunCommandReturnTypes;
	const auto Handled = EReturn::RUN_COMMAND_COMMAND_HANDLED;

	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_OpenGL::CMD_INIT:
		m_TextureHandles.Clear();
		m_TextureBindingHandles.Clear();
		m_vTextureBindings.clear();
		m_PipelineHandles.Clear();
		m_vPipelines.clear();
		m_BufferHandles.Clear();
		m_BufferContainerHandles.Clear();
		m_vBufferContainerBindings.clear();
		break;
	case CCommandProcessorFragment_OpenGL::CMD_SHUTDOWN:
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
	case CCommandBuffer::CMD_TEXTURE_BINDING_CREATE:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_TextureBinding_Create *>(pBaseCommand);
		if(!m_TextureHandles.IsActive(pCommand->m_Desc.m_aTextures[0]) || !m_TextureHandles.IsActive(pCommand->m_Desc.m_aTextures[1]) || !m_TextureBindingHandles.Activate(pCommand->m_Binding))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_TEXTURE_BINDING_DESTROY:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_TextureBinding_Destroy *>(pBaseCommand);
		if(!m_TextureBindingHandles.Release(pCommand->m_Binding))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_PIPELINE_CREATE:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Pipeline_Create *>(pBaseCommand);
		if(pCommand->m_Desc.m_Program >= EPipelineProgram::COUNT || !m_PipelineHandles.Activate(pCommand->m_Pipeline))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_PIPELINE_DESTROY:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Pipeline_Destroy *>(pBaseCommand);
		if(!m_PipelineHandles.Release(pCommand->m_Pipeline))
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
	case CCommandBuffer::CMD_UPDATE_BUFFER_OBJECT:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_UpdateBufferObject *>(pBaseCommand);
		if(!m_BufferHandles.IsActive(pCommand->m_Buffer))
			return Handled;
		break;
	}
	case CCommandBuffer::CMD_COPY_BUFFER_OBJECT:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_CopyBufferObject *>(pBaseCommand);
		if(!m_BufferHandles.IsActive(pCommand->m_ReadBuffer) || !m_BufferHandles.IsActive(pCommand->m_WriteBuffer))
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
	case CCommandBuffer::CMD_CREATE_BUFFER_CONTAINER:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_CreateBufferContainer *>(pBaseCommand);
		if(!m_BufferHandles.IsActive(pCommand->m_VertBufferBinding) || !m_BufferContainerHandles.Activate(pCommand->m_BufferContainer))
			return Handled;
		if(static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainerBindings.size())
			m_vBufferContainerBindings.resize(pCommand->m_BufferContainer.Id() + 1);
		m_vBufferContainerBindings[pCommand->m_BufferContainer.Id()] = pCommand->m_VertBufferBinding;
		break;
	}
	case CCommandBuffer::CMD_UPDATE_BUFFER_CONTAINER:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_UpdateBufferContainer *>(pBaseCommand);
		if(!m_BufferContainerHandles.IsActive(pCommand->m_BufferContainer) || !m_BufferHandles.IsActive(pCommand->m_VertBufferBinding) ||
			static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainerBindings.size())
			return Handled;
		m_vBufferContainerBindings[pCommand->m_BufferContainer.Id()] = pCommand->m_VertBufferBinding;
		break;
	}
	case CCommandBuffer::CMD_DELETE_BUFFER_CONTAINER:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_DeleteBufferContainer *>(pBaseCommand);
		if(!m_BufferContainerHandles.IsActive(pCommand->m_BufferContainer) ||
			static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainerBindings.size())
			return Handled;
		const auto Buffer = m_vBufferContainerBindings[pCommand->m_BufferContainer.Id()];
		if(pCommand->m_DestroyAllBO && !m_BufferHandles.IsActive(Buffer))
			return Handled;
		m_BufferContainerHandles.Release(pCommand->m_BufferContainer);
		if(pCommand->m_DestroyAllBO)
			m_BufferHandles.Release(Buffer);
		m_vBufferContainerBindings[pCommand->m_BufferContainer.Id()].Invalidate();
		break;
	}
	default:
		break;
	}

	const CCommandBuffer::SState *pState = RenderCommandState(pBaseCommand);
	if(pState != nullptr && pState->m_Texture.IsValid() && !m_TextureHandles.IsActive(pState->m_Texture))
		return Handled;
	const IGraphics::CBufferContainerHandle *pBufferContainer = RenderCommandBufferContainer(pBaseCommand);
	if(pBufferContainer != nullptr && (!m_BufferContainerHandles.IsActive(*pBufferContainer) ||
						  static_cast<size_t>(pBufferContainer->Id()) >= m_vBufferContainerBindings.size() ||
						  !m_BufferHandles.IsActive(m_vBufferContainerBindings[pBufferContainer->Id()])))
		return Handled;
	if(pBaseCommand->m_Cmd == CCommandBuffer::CMD_DRAW)
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Draw *>(pBaseCommand);
		if(!m_PipelineHandles.IsActive(pCommand->m_Pipeline) || static_cast<size_t>(pCommand->m_Pipeline.Id()) >= m_vPipelines.size())
			return Handled;
		if(IsNewApi() && pCommand->m_PrimitiveType == EPrimitiveType::QUADS && !m_BufferHandles.IsActive(pCommand->m_IndexBuffer))
			return Handled;
	}
	if(pBaseCommand->m_Cmd == CCommandBuffer::CMD_DRAW_INDEXED)
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand);
		if(!m_PipelineHandles.IsActive(pCommand->m_Pipeline) || static_cast<size_t>(pCommand->m_Pipeline.Id()) >= m_vPipelines.size())
			return Handled;
		if(pCommand->IsTransient())
		{
			if(!pCommand->ValidateTransient() || PipelineProgram(pCommand->m_Pipeline) != EPipelineProgram::PRIMITIVE)
				return Handled;
			const auto *pRanges = pCommand->m_RangeData.Get<CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange>(pCommand->m_RangeCount);
			for(uint32_t RangeIndex = 0; RangeIndex < pCommand->m_RangeCount; ++RangeIndex)
			{
				const auto Texture = pRanges[RangeIndex].m_State.m_Texture;
				if(Texture.IsValid() && !m_TextureHandles.IsActive(Texture))
					return Handled;
			}
		}
		else if(!m_BufferHandles.IsActive(pCommand->m_IndexBuffer))
			return Handled;
		if(pCommand->m_TextureBinding.IsValid())
		{
			if(!m_TextureBindingHandles.IsActive(pCommand->m_TextureBinding) || static_cast<size_t>(pCommand->m_TextureBinding.Id()) >= m_vTextureBindings.size())
				return Handled;
			for(const auto Texture : m_vTextureBindings[pCommand->m_TextureBinding.Id()].m_aTextures)
				if(!m_TextureHandles.IsActive(Texture))
					return Handled;
		}
		else if(PipelineProgram(pCommand->m_Pipeline) == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
			return Handled;
	}

	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_OpenGL::CMD_INIT:
		Cmd_Init(static_cast<const SCommand_Init *>(pBaseCommand));
		break;
	case CCommandProcessorFragment_OpenGL::CMD_SHUTDOWN:
		Cmd_Shutdown(static_cast<const SCommand_Shutdown *>(pBaseCommand));
		m_TextureHandles.Clear();
		m_TextureBindingHandles.Clear();
		m_vTextureBindings.clear();
		m_PipelineHandles.Clear();
		m_vPipelines.clear();
		m_BufferHandles.Clear();
		m_BufferContainerHandles.Clear();
		m_vBufferContainerBindings.clear();
		break;
	case CCommandBuffer::CMD_TEXTURE_CREATE:
		Cmd_Texture_Create(static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_DESTROY:
		Cmd_Texture_Destroy(static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_BINDING_CREATE:
		Cmd_TextureBinding_Create(static_cast<const CCommandBuffer::SCommand_TextureBinding_Create *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_BINDING_DESTROY:
		Cmd_TextureBinding_Destroy(static_cast<const CCommandBuffer::SCommand_TextureBinding_Destroy *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_PIPELINE_CREATE:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Pipeline_Create *>(pBaseCommand);
		if(static_cast<size_t>(pCommand->m_Pipeline.Id()) >= m_vPipelines.size())
			m_vPipelines.resize(pCommand->m_Pipeline.Id() + 1);
		m_vPipelines[pCommand->m_Pipeline.Id()] = pCommand->m_Desc.m_Program;
		break;
	}
	case CCommandBuffer::CMD_PIPELINE_DESTROY:
	{
		const auto *pCommand = static_cast<const CCommandBuffer::SCommand_Pipeline_Destroy *>(pBaseCommand);
		m_vPipelines[pCommand->m_Pipeline.Id()] = EPipelineProgram::PRIMITIVE;
		break;
	}
	case CCommandBuffer::CMD_TEXTURE_UPDATE:
		Cmd_Texture_Update(static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand));
		break;
	case CCommandBuffer::CMD_TEXTURE_READBACK:
		Cmd_Texture_Readback(static_cast<const CCommandBuffer::SCommand_Texture_Readback *>(pBaseCommand));
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
	case CCommandBuffer::CMD_UPDATE_BUFFER_OBJECT: Cmd_UpdateBufferObject(static_cast<const CCommandBuffer::SCommand_UpdateBufferObject *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT: Cmd_RecreateBufferObject(static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_COPY_BUFFER_OBJECT: Cmd_CopyBufferObject(static_cast<const CCommandBuffer::SCommand_CopyBufferObject *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_DELETE_BUFFER_OBJECT: Cmd_DeleteBufferObject(static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand)); break;

	case CCommandBuffer::CMD_CREATE_BUFFER_CONTAINER: Cmd_CreateBufferContainer(static_cast<const CCommandBuffer::SCommand_CreateBufferContainer *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_UPDATE_BUFFER_CONTAINER: Cmd_UpdateBufferContainer(static_cast<const CCommandBuffer::SCommand_UpdateBufferContainer *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_DELETE_BUFFER_CONTAINER: Cmd_DeleteBufferContainer(static_cast<const CCommandBuffer::SCommand_DeleteBufferContainer *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_DRAW_INDEXED: Cmd_DrawIndexed(static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand)); break;
	default: return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_UNHANDLED;
	}

	return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED;
}

// ------------ CCommandProcessorFragment_OpenGL2

void CCommandProcessorFragment_OpenGL2::UseProgram(CGLSLTWProgram *pProgram)
{
	pProgram->UseProgram();
}

void CCommandProcessorFragment_OpenGL2::SetState(const CCommandBuffer::SState &State, CGLSLTWProgram *pProgram, bool Use2DArrayTextures)
{
	if(State.m_BlendMode != m_LastBlendMode)
	{
		switch(State.m_BlendMode)
		{
		case EBlendMode::NONE:
			glDisable(GL_BLEND);
			break;
		case EBlendMode::ALPHA:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case EBlendMode::ADDITIVE:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		default:
			dbg_assert_failed("Invalid blend mode: %d", (int)State.m_BlendMode);
		};

		m_LastBlendMode = State.m_BlendMode;
	}

	SetScissor(State);

	if(!IsNewApi())
	{
		glDisable(GL_TEXTURE_2D);
		if(!m_HasShaders)
		{
			if(m_Has3DTextures)
				glDisable(GL_TEXTURE_3D);
			if(m_Has2DArrayTextures)
			{
				glDisable(m_2DArrayTarget);
			}
		}
	}

	// texture
	if(IsTexturedState(State))
	{
		int Slot = 0;
		if(!Use2DArrayTextures)
		{
			if(!IsNewApi() && !m_HasShaders)
				glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, m_vTextures[State.m_Texture.Id()].m_Tex);
			if(IsNewApi())
				glBindSampler(Slot, m_vTextures[State.m_Texture.Id()].m_Sampler);
		}
		else
		{
			if(!m_Has2DArrayTextures)
			{
				if(!IsNewApi() && !m_HasShaders)
					glEnable(GL_TEXTURE_3D);
				glBindTexture(GL_TEXTURE_3D, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
				if(IsNewApi())
					glBindSampler(Slot, m_vTextures[State.m_Texture.Id()].m_Sampler2DArray);
			}
			else
			{
				if(!IsNewApi() && !m_HasShaders)
					glEnable(m_2DArrayTarget);
				glBindTexture(m_2DArrayTarget, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
				if(IsNewApi())
					glBindSampler(Slot, m_vTextures[State.m_Texture.Id()].m_Sampler2DArray);
			}
		}

		if(pProgram->m_LastTextureSampler != Slot)
		{
			pProgram->SetUniform(pProgram->m_LocTextureSampler, Slot);
			pProgram->m_LastTextureSampler = Slot;
		}

		if(m_vTextures[State.m_Texture.Id()].m_LastWrapMode != State.m_WrapMode && !Use2DArrayTextures)
		{
			switch(State.m_WrapMode)
			{
			case EWrapMode::REPEAT:
				if(IsNewApi())
				{
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
				}
				break;
			case EWrapMode::CLAMP:
				if(IsNewApi())
				{
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
				break;
			default:
				dbg_assert_failed("Invalid wrap mode: %d", (int)State.m_WrapMode);
			};
			m_vTextures[State.m_Texture.Id()].m_LastWrapMode = State.m_WrapMode;
		}
	}

	CCommandBuffer::SPoint ScreenTL = State.m_ScreenTL;
	CCommandBuffer::SPoint ScreenBR = State.m_ScreenBR;
	if(m_RenderingToTexture)
		std::swap(ScreenTL.y, ScreenBR.y);
	if(pProgram->m_LastScreenTL != ScreenTL || pProgram->m_LastScreenBR != ScreenBR)
	{
		pProgram->m_LastScreenTL = ScreenTL;
		pProgram->m_LastScreenBR = ScreenBR;

		// screen mapping
		// orthographic projection matrix
		// the z coordinate is the same for every vertex, so just ignore the z coordinate and set it in the shaders
		float m[2 * 4] = {
			2.f / (ScreenBR.x - ScreenTL.x),
			0,
			0,
			-((ScreenBR.x + ScreenTL.x) / (ScreenBR.x - ScreenTL.x)),
			0,
			(2.f / (ScreenTL.y - ScreenBR.y)),
			0,
			-((ScreenTL.y + ScreenBR.y) / (ScreenTL.y - ScreenBR.y)),
		};

		// transpose bcs of column-major order of opengl
		glUniformMatrix4x2fv(pProgram->m_LocPos, 1, true, (float *)&m);
	}
}

#ifndef BACKEND_GL_MODERN_API
namespace
{
	constexpr size_t ANALYSIS_COLOR_LEVELS = 5;
	constexpr size_t ANALYSIS_LAYER_COUNT = ANALYSIS_COLOR_LEVELS * ANALYSIS_COLOR_LEVELS * ANALYSIS_COLOR_LEVELS;
	constexpr size_t ANALYSIS_DISPLAY_COLUMNS = ANALYSIS_COLOR_LEVELS * ANALYSIS_COLOR_LEVELS;
	constexpr size_t ANALYSIS_DISPLAY_ROWS = ANALYSIS_COLOR_LEVELS;
	constexpr size_t ANALYSIS_LAYER_WIDTH = 64;
	constexpr size_t ANALYSIS_LAYER_HEIGHT = 64;
	constexpr size_t ANALYSIS_LAYER_SIZE = ANALYSIS_LAYER_WIDTH * ANALYSIS_LAYER_HEIGHT * 4;
}

bool CCommandProcessorFragment_OpenGL2::AnalyzeLayeredTexture(const SGraphicsVertexTex3D *pVertices, size_t VerticesCount, const uint8_t *pTextureData)
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	int Slot = 0;
	if(m_HasShaders)
	{
		CGLSLTWProgram *pProgram = m_pPrimitive3DProgramTextured;
		UseProgram(pProgram);

		pProgram->SetUniform(pProgram->m_LocTextureSampler, Slot);

		float m[2 * 4] = {
			1, 0, 0, 0,
			0, 1, 0, 0};

		// transpose bcs of column-major order of opengl
		glUniformMatrix4x2fv(pProgram->m_LocPos, 1, true, (float *)&m);
	}
	else
	{
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1, 1, -1, 1, -10.0f, 10.f);
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer(2, GL_FLOAT, sizeof(*pVertices), pVertices);
	glColorPointer(4, GL_FLOAT, sizeof(*pVertices), reinterpret_cast<const uint8_t *>(pVertices) + sizeof(vec2));
	glTexCoordPointer(3, GL_FLOAT, sizeof(*pVertices), reinterpret_cast<const uint8_t *>(pVertices) + sizeof(vec2) + sizeof(vec4));

	glDrawArrays(GL_QUADS, 0, VerticesCount);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	if(m_HasShaders)
	{
		glUseProgram(0);
	}

	glFinish();

	GLint aViewport[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_VIEWPORT, aViewport);

	int w = aViewport[2];
	int h = aViewport[3];

	size_t PixelDataSize = (size_t)w * h * 3;
	if(PixelDataSize == 0)
		return false;
	std::unique_ptr<uint8_t, decltype(&free)> pPixelData(static_cast<uint8_t *>(malloc(PixelDataSize)), free);
	if(pPixelData == nullptr)
		return false;

	// fetch the pixels
	GLint Alignment;
	glGetIntegerv(GL_PACK_ALIGNMENT, &Alignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pPixelData.get());
	glPixelStorei(GL_PACK_ALIGNMENT, Alignment);

	// now analyse the image data
	bool CheckFailed = false;
	for(size_t d = 0; d < ANALYSIS_LAYER_COUNT; ++d)
	{
		size_t CurX = d % ANALYSIS_DISPLAY_COLUMNS;
		size_t CurY = d / ANALYSIS_DISPLAY_COLUMNS;
		int CheckX = static_cast<int>(((2 * CurX + 1) * static_cast<size_t>(w)) / (2 * ANALYSIS_DISPLAY_COLUMNS));
		int CheckY = static_cast<int>(((2 * CurY + 1) * static_cast<size_t>(h)) / (2 * ANALYSIS_DISPLAY_ROWS));

		ptrdiff_t OffsetPixelData = (CheckY * (w * 3)) + (CheckX * 3);
		ptrdiff_t OffsetTextureData = ANALYSIS_LAYER_SIZE * d;
		const uint8_t *pPixel = pPixelData.get() + OffsetPixelData;
		const uint8_t *pTexturePixel = pTextureData + OffsetTextureData;
		for(size_t i = 0; i < 3; ++i)
		{
			if((pPixel[i] < pTexturePixel[i] - 25) || (pPixel[i] > pTexturePixel[i] + 25))
			{
				CheckFailed = true;
				break;
			}
		}
	}

	return !CheckFailed;
}

bool CCommandProcessorFragment_OpenGL2::IsLayeredTextureAnalysisSucceeded()
{
	glClearColor(0, 0, 0, 1);

	std::unique_ptr<uint8_t, decltype(&free)> pTextureData(static_cast<uint8_t *>(malloc(ANALYSIS_LAYER_COUNT * ANALYSIS_LAYER_SIZE)), free);
	if(pTextureData == nullptr)
		return false;
	for(size_t d = 0; d < ANALYSIS_LAYER_COUNT; ++d)
	{
		const uint8_t aColor[4] = {
			static_cast<uint8_t>(25 + 50 * (d / (ANALYSIS_COLOR_LEVELS * ANALYSIS_COLOR_LEVELS))),
			static_cast<uint8_t>(25 + 50 * ((d / ANALYSIS_COLOR_LEVELS) % ANALYSIS_COLOR_LEVELS)),
			static_cast<uint8_t>(25 + 50 * (d % ANALYSIS_COLOR_LEVELS)),
			255};
		uint8_t *pLayerData = pTextureData.get() + ANALYSIS_LAYER_SIZE * d;
		for(size_t PixelOffset = 0; PixelOffset < ANALYSIS_LAYER_SIZE; PixelOffset += 4)
		{
			for(size_t Channel = 0; Channel < 4; ++Channel)
			{
				pLayerData[PixelOffset + Channel] = aColor[Channel];
			}
		}
	}

	// upload the texture
	GLuint FakeTexture;
	glGenTextures(1, &FakeTexture);

	GLenum Target = GL_TEXTURE_3D;
	if(m_Has2DArrayTextures)
	{
		Target = m_2DArrayTarget;
	}

	glBindTexture(Target, FakeTexture);
	glTexParameteri(Target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	if(!m_Has2DArrayTextures)
	{
		glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(Target, GL_GENERATE_MIPMAP, GL_TRUE);
	}

	glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(Target, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);

	glTexImage3D(Target, 0, GL_RGBA, ANALYSIS_LAYER_WIDTH, ANALYSIS_LAYER_HEIGHT, ANALYSIS_LAYER_COUNT, 0, GL_RGBA, GL_UNSIGNED_BYTE, pTextureData.get());

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_SCISSOR_TEST);

	if(!m_HasShaders)
	{
		glDisable(GL_TEXTURE_2D);
		if(m_Has3DTextures)
			glDisable(GL_TEXTURE_3D);
		if(m_Has2DArrayTextures)
		{
			glDisable(m_2DArrayTarget);
		}

		if(!m_Has2DArrayTextures)
		{
			glEnable(GL_TEXTURE_3D);
			glBindTexture(GL_TEXTURE_3D, FakeTexture);
		}
		else
		{
			glEnable(m_2DArrayTarget);
			glBindTexture(m_2DArrayTarget, FakeTexture);
		}
	}

	std::array<SGraphicsVertexTex3D, ANALYSIS_LAYER_COUNT * 4> aVertices;
	size_t VertexCount = 0;
	for(size_t i = 0; i < ANALYSIS_LAYER_COUNT; ++i)
	{
		float XPos = (float)(i % ANALYSIS_DISPLAY_COLUMNS);
		float YPos = (float)(i / ANALYSIS_DISPLAY_COLUMNS);

		SGraphicsVertexTex3D *pVertex = &aVertices[VertexCount++];
		SGraphicsVertexTex3D *pVertexBefore = pVertex;
		pVertex->m_Pos.x = XPos / ANALYSIS_DISPLAY_COLUMNS;
		pVertex->m_Pos.y = YPos / ANALYSIS_DISPLAY_ROWS;
		pVertex->m_Color.r = 1;
		pVertex->m_Color.g = 1;
		pVertex->m_Color.b = 1;
		pVertex->m_Color.a = 1;
		pVertex->m_Tex.u = 0;
		pVertex->m_Tex.v = 0;

		pVertex = &aVertices[VertexCount++];
		pVertex->m_Pos.x = (XPos + 1) / ANALYSIS_DISPLAY_COLUMNS;
		pVertex->m_Pos.y = YPos / ANALYSIS_DISPLAY_ROWS;
		pVertex->m_Color.r = 1;
		pVertex->m_Color.g = 1;
		pVertex->m_Color.b = 1;
		pVertex->m_Color.a = 1;
		pVertex->m_Tex.u = 1;
		pVertex->m_Tex.v = 0;

		pVertex = &aVertices[VertexCount++];
		pVertex->m_Pos.x = (XPos + 1) / ANALYSIS_DISPLAY_COLUMNS;
		pVertex->m_Pos.y = (YPos + 1) / ANALYSIS_DISPLAY_ROWS;
		pVertex->m_Color.r = 1;
		pVertex->m_Color.g = 1;
		pVertex->m_Color.b = 1;
		pVertex->m_Color.a = 1;
		pVertex->m_Tex.u = 1;
		pVertex->m_Tex.v = 1;

		pVertex = &aVertices[VertexCount++];
		pVertex->m_Pos.x = XPos / ANALYSIS_DISPLAY_COLUMNS;
		pVertex->m_Pos.y = (YPos + 1) / ANALYSIS_DISPLAY_ROWS;
		pVertex->m_Color.r = 1;
		pVertex->m_Color.g = 1;
		pVertex->m_Color.b = 1;
		pVertex->m_Color.a = 1;
		pVertex->m_Tex.u = 0;
		pVertex->m_Tex.v = 1;

		for(size_t n = 0; n < 4; ++n)
		{
			pVertexBefore[n].m_Pos.x *= 2;
			pVertexBefore[n].m_Pos.x -= 1;
			pVertexBefore[n].m_Pos.y *= 2;
			pVertexBefore[n].m_Pos.y -= 1;
			if(m_Has2DArrayTextures)
			{
				pVertexBefore[n].m_Tex.w = i;
			}
			else
			{
				pVertexBefore[n].m_Tex.w = (i + 0.5f) / ANALYSIS_LAYER_COUNT;
			}
		}
	}

	// everything build up, now do the analyze steps
	bool NoError = AnalyzeLayeredTexture(aVertices.data(), VertexCount, pTextureData.get());

	glDeleteTextures(1, &FakeTexture);

	return NoError;
}

bool CCommandProcessorFragment_OpenGL2::Cmd_Init(const SCommand_Init *pCommand)
{
	if(!CCommandProcessorFragment_OpenGL::Cmd_Init(pCommand))
		return false;

	m_pTileProgram = nullptr;
	m_pTileProgramTextured = nullptr;
	m_pPrimitive3DProgram = nullptr;
	m_pPrimitive3DProgramTextured = nullptr;

	m_OpenGLTextureLodBIAS = g_Config.m_GfxGLTextureLODBIAS;

	m_HasShaders = pCommand->m_pCapabilities->m_ShaderSupport;

	bool HasAllFunc = true;
#ifndef BACKEND_AS_OPENGL_ES
	if(m_HasShaders)
	{
		HasAllFunc &= (glUniformMatrix4x2fv != nullptr) && (glGenBuffers != nullptr);
		HasAllFunc &= (glBindBuffer != nullptr) && (glBufferData != nullptr);
		HasAllFunc &= (glEnableVertexAttribArray != nullptr) && (glVertexAttribPointer != nullptr) && (glVertexAttribIPointer != nullptr);
		HasAllFunc &= (glDisableVertexAttribArray != nullptr) && (glDeleteBuffers != nullptr);
		HasAllFunc &= (glUseProgram != nullptr) && (glTexImage3D != nullptr);
		HasAllFunc &= (glBindAttribLocation != nullptr);
		HasAllFunc &= (glBufferSubData != nullptr) && (glGetUniformLocation != nullptr);
		HasAllFunc &= (glUniform1i != nullptr) && (glUniform1f != nullptr);
		HasAllFunc &= (glUniform1ui != nullptr);
		HasAllFunc &= (glUniform1fv != nullptr) && (glUniform2fv != nullptr);
		HasAllFunc &= (glUniform4fv != nullptr) && (glGetAttachedShaders != nullptr);
		HasAllFunc &= (glGetProgramInfoLog != nullptr) && (glGetProgramiv != nullptr);
		HasAllFunc &= (glLinkProgram != nullptr) && (glDetachShader != nullptr);
		HasAllFunc &= (glAttachShader != nullptr) && (glDeleteProgram != nullptr);
		HasAllFunc &= (glCreateProgram != nullptr) && (glShaderSource != nullptr);
		HasAllFunc &= (glCompileShader != nullptr) && (glGetShaderiv != nullptr);
		HasAllFunc &= (glGetShaderInfoLog != nullptr) && (glDeleteShader != nullptr);
		HasAllFunc &= (glCreateShader != nullptr);
	}
#endif

	bool AnalysisCorrect = true;
	if(HasAllFunc)
	{
		if(m_HasShaders)
		{
			m_pTileProgram = new CGLSLTileProgram;
			m_pTileProgramTextured = new CGLSLTileProgram;
			m_pBorderTileProgram = new CGLSLTileProgram;
			m_pBorderTileProgramTextured = new CGLSLTileProgram;
			m_pPrimitive3DProgram = new CGLSLPrimitiveProgram;
			m_pPrimitive3DProgramTextured = new CGLSLPrimitiveProgram;

			CGLSLCompiler ShaderCompiler(g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch, m_IsOpenGLES, m_OpenGLTextureLodBIAS / 1000.0f);
			ShaderCompiler.SetHasTextureArray(pCommand->m_pCapabilities->m_2DArrayTextures);
			ShaderCompiler.SetTextureReplaceType(pCommand->m_pCapabilities->m_2DArrayTextures ? CGLSLCompiler::GLSL_COMPILER_TEXTURE_REPLACE_TYPE_2D_ARRAY : CGLSLCompiler::GLSL_COMPILER_TEXTURE_REPLACE_TYPE_3D);
			{
				CGLSL PrimitiveVertexShader;
				CGLSL PrimitiveFragmentShader;
				PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.vert", GL_VERTEX_SHADER);
				PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.frag", GL_FRAGMENT_SHADER);

				m_pPrimitive3DProgram->CreateProgram();
				m_pPrimitive3DProgram->AddShader(&PrimitiveVertexShader);
				m_pPrimitive3DProgram->AddShader(&PrimitiveFragmentShader);
				m_pPrimitive3DProgram->LinkProgram();

				UseProgram(m_pPrimitive3DProgram);

				m_pPrimitive3DProgram->m_LocPos = m_pPrimitive3DProgram->GetUniformLoc("gPos");
			}

			{
				CGLSL PrimitiveVertexShader;
				CGLSL PrimitiveFragmentShader;
				ShaderCompiler.AddDefine("TW_TEXTURED", "");
				if(!pCommand->m_pCapabilities->m_2DArrayTextures)
					ShaderCompiler.AddDefine("TW_3D_TEXTURED", "");
				PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.vert", GL_VERTEX_SHADER);
				PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.frag", GL_FRAGMENT_SHADER);
				ShaderCompiler.ClearDefines();

				m_pPrimitive3DProgramTextured->CreateProgram();
				m_pPrimitive3DProgramTextured->AddShader(&PrimitiveVertexShader);
				m_pPrimitive3DProgramTextured->AddShader(&PrimitiveFragmentShader);
				m_pPrimitive3DProgramTextured->LinkProgram();

				UseProgram(m_pPrimitive3DProgramTextured);

				m_pPrimitive3DProgramTextured->m_LocPos = m_pPrimitive3DProgramTextured->GetUniformLoc("gPos");
				m_pPrimitive3DProgramTextured->m_LocTextureSampler = m_pPrimitive3DProgramTextured->GetUniformLoc("gTextureSampler");
			}
			{
				CGLSL VertexShader;
				CGLSL FragmentShader;
				VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.vert", GL_VERTEX_SHADER);
				FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.frag", GL_FRAGMENT_SHADER);

				m_pTileProgram->CreateProgram();
				m_pTileProgram->AddShader(&VertexShader);
				m_pTileProgram->AddShader(&FragmentShader);

				glBindAttribLocation(m_pTileProgram->GetProgramId(), 0, "inVertex");

				m_pTileProgram->LinkProgram();

				UseProgram(m_pTileProgram);

				m_pTileProgram->m_LocPos = m_pTileProgram->GetUniformLoc("gPos");
				m_pTileProgram->m_LocColor = m_pTileProgram->GetUniformLoc("gVertColor");
			}
			{
				CGLSL VertexShader;
				CGLSL FragmentShader;
				ShaderCompiler.AddDefine("TW_TILE_TEXTURED", "");
				if(!pCommand->m_pCapabilities->m_2DArrayTextures)
					ShaderCompiler.AddDefine("TW_TILE_3D_TEXTURED", "");
				VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.vert", GL_VERTEX_SHADER);
				FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.frag", GL_FRAGMENT_SHADER);
				ShaderCompiler.ClearDefines();

				m_pTileProgramTextured->CreateProgram();
				m_pTileProgramTextured->AddShader(&VertexShader);
				m_pTileProgramTextured->AddShader(&FragmentShader);

				glBindAttribLocation(m_pTileProgramTextured->GetProgramId(), 0, "inVertex");
				glBindAttribLocation(m_pTileProgramTextured->GetProgramId(), 1, "inVertexTexCoord");

				m_pTileProgramTextured->LinkProgram();

				UseProgram(m_pTileProgramTextured);

				m_pTileProgramTextured->m_LocPos = m_pTileProgramTextured->GetUniformLoc("gPos");
				m_pTileProgramTextured->m_LocTextureSampler = m_pTileProgramTextured->GetUniformLoc("gTextureSampler");
				m_pTileProgramTextured->m_LocColor = m_pTileProgramTextured->GetUniformLoc("gVertColor");
			}
			{
				CGLSL VertexShader;
				CGLSL FragmentShader;
				VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.vert", GL_VERTEX_SHADER);
				FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.frag", GL_FRAGMENT_SHADER);
				ShaderCompiler.ClearDefines();

				m_pBorderTileProgram->CreateProgram();
				m_pBorderTileProgram->AddShader(&VertexShader);
				m_pBorderTileProgram->AddShader(&FragmentShader);

				glBindAttribLocation(m_pBorderTileProgram->GetProgramId(), 0, "inVertex");

				m_pBorderTileProgram->LinkProgram();

				UseProgram(m_pBorderTileProgram);

				m_pBorderTileProgram->m_LocPos = m_pBorderTileProgram->GetUniformLoc("gPos");
				m_pBorderTileProgram->m_LocColor = m_pBorderTileProgram->GetUniformLoc("gVertColor");
				m_pBorderTileProgram->m_LocOffset = m_pBorderTileProgram->GetUniformLoc("gOffset");
				m_pBorderTileProgram->m_LocScale = m_pBorderTileProgram->GetUniformLoc("gScale");
			}
			{
				CGLSL VertexShader;
				CGLSL FragmentShader;
				ShaderCompiler.AddDefine("TW_TILE_TEXTURED", "");
				if(!pCommand->m_pCapabilities->m_2DArrayTextures)
					ShaderCompiler.AddDefine("TW_TILE_3D_TEXTURED", "");
				VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.vert", GL_VERTEX_SHADER);
				FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.frag", GL_FRAGMENT_SHADER);
				ShaderCompiler.ClearDefines();

				m_pBorderTileProgramTextured->CreateProgram();
				m_pBorderTileProgramTextured->AddShader(&VertexShader);
				m_pBorderTileProgramTextured->AddShader(&FragmentShader);

				glBindAttribLocation(m_pBorderTileProgramTextured->GetProgramId(), 0, "inVertex");
				glBindAttribLocation(m_pBorderTileProgramTextured->GetProgramId(), 1, "inVertexTexCoord");

				m_pBorderTileProgramTextured->LinkProgram();

				UseProgram(m_pBorderTileProgramTextured);

				m_pBorderTileProgramTextured->m_LocPos = m_pBorderTileProgramTextured->GetUniformLoc("gPos");
				m_pBorderTileProgramTextured->m_LocTextureSampler = m_pBorderTileProgramTextured->GetUniformLoc("gTextureSampler");
				m_pBorderTileProgramTextured->m_LocColor = m_pBorderTileProgramTextured->GetUniformLoc("gVertColor");
				m_pBorderTileProgramTextured->m_LocOffset = m_pBorderTileProgramTextured->GetUniformLoc("gOffset");
				m_pBorderTileProgramTextured->m_LocScale = m_pBorderTileProgramTextured->GetUniformLoc("gScale");
			}

			glUseProgram(0);
		}

		if(g_Config.m_Gfx3DTextureAnalysisRan == 0 || str_comp(g_Config.m_Gfx3DTextureAnalysisRenderer, pCommand->m_pRendererString) != 0 || str_comp(g_Config.m_Gfx3DTextureAnalysisVersion, pCommand->m_pVersionString) != 0)
		{
			AnalysisCorrect = IsLayeredTextureAnalysisSucceeded();
			if(AnalysisCorrect)
			{
				g_Config.m_Gfx3DTextureAnalysisRan = 1;
				str_copy(g_Config.m_Gfx3DTextureAnalysisRenderer, pCommand->m_pRendererString);
				str_copy(g_Config.m_Gfx3DTextureAnalysisVersion, pCommand->m_pVersionString);
			}
		}
	}

	if(!AnalysisCorrect || !HasAllFunc)
	{
		// downgrade to opengl 1.5
		*pCommand->m_pInitError = -2;
		pCommand->m_pCapabilities->m_ContextMajor = 1;
		pCommand->m_pCapabilities->m_ContextMinor = 5;
		pCommand->m_pCapabilities->m_ContextPatch = 0;

		return false;
	}

	return true;
}

void CCommandProcessorFragment_OpenGL2::Cmd_Shutdown(const SCommand_Shutdown *pCommand)
{
	delete m_pTileProgram;
	delete m_pTileProgramTextured;
	delete m_pBorderTileProgram;
	delete m_pBorderTileProgramTextured;
	delete m_pPrimitive3DProgram;
	delete m_pPrimitive3DProgramTextured;
	for(auto &BufferObject : m_vBufferObjectIndices)
		free(BufferObject.m_pData);
}

void CCommandProcessorFragment_OpenGL2::Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand)
{
	if(PipelineProgram(pCommand->m_Pipeline) == EPipelineProgram::PRIMITIVE)
	{
		CCommandProcessorFragment_OpenGL::Cmd_Draw(pCommand);
		return;
	}
	if(PipelineProgram(pCommand->m_Pipeline) != EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY)
		return;
	const uint32_t VerticesPerPrim = VerticesPerPrimitive(pCommand->m_PrimitiveType);
	const auto *pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertexTex3DStream>(pCommand->m_VertexCount);
	if(pVertices == nullptr || VerticesPerPrim == 0 || pCommand->m_VertexCount == 0 || pCommand->m_VertexCount % VerticesPerPrim != 0)
		return;

	if(m_HasShaders)
	{
		CGLSLPrimitiveProgram *pProgram = nullptr;
		if(IsTexturedState(pCommand->m_State))
		{
			pProgram = m_pPrimitive3DProgramTextured;
		}
		else
		{
			pProgram = m_pPrimitive3DProgram;
		}

		UseProgram(pProgram);

		SetState(pCommand->m_State, pProgram, true);
	}
	else
	{
		CCommandProcessorFragment_OpenGL::SetState(pCommand->m_State, true);
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer(2, GL_FLOAT, sizeof(pVertices[0]), pVertices);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(pVertices[0]), (uint8_t *)pVertices + (ptrdiff_t)(sizeof(vec2)));
	glTexCoordPointer(3, GL_FLOAT, sizeof(pVertices[0]), (uint8_t *)pVertices + (ptrdiff_t)(sizeof(vec2) + sizeof(unsigned char) * 4));

	switch(pCommand->m_PrimitiveType)
	{
	case EPrimitiveType::QUADS:
		glDrawArrays(GL_QUADS, 0, pCommand->m_VertexCount);
		break;
	case EPrimitiveType::TRIANGLES:
		glDrawArrays(GL_TRIANGLES, 0, pCommand->m_VertexCount);
		break;
	case EPrimitiveType::LINES:
		glDrawArrays(GL_LINES, 0, pCommand->m_VertexCount);
		break;
	default:
		dbg_assert_failed("Invalid primitive type: %d", (int)pCommand->m_PrimitiveType);
	};

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	if(m_HasShaders)
	{
		glUseProgram(0);
	}
}

void CCommandProcessorFragment_OpenGL2::Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	const int Index = pCommand->m_Buffer.Id();
	// create necessary space
	if((size_t)Index >= m_vBufferObjectIndices.size())
	{
		m_vBufferObjectIndices.resize(Index + 1, 0);
	}

	GLuint VertBufferId = 0;

	glGenBuffers(1, &VertBufferId);
	glBindBuffer(GL_ARRAY_BUFFER, VertBufferId);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pCommand->m_Desc.m_Size), pUploadData, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	SBufferObject &BufferObject = m_vBufferObjectIndices[Index];
	BufferObject.m_BufferObjectId = VertBufferId;
	BufferObject.m_pData = static_cast<uint8_t *>(malloc(pCommand->m_Desc.m_Size));
	if(pUploadData)
		mem_copy(BufferObject.m_pData, pUploadData, pCommand->m_Desc.m_Size);
}

void CCommandProcessorFragment_OpenGL2::Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	int Index = pCommand->m_Buffer.Id();
	SBufferObject &BufferObject = m_vBufferObjectIndices[Index];

	glBindBuffer(GL_ARRAY_BUFFER, BufferObject.m_BufferObjectId);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pCommand->m_Desc.m_Size), pUploadData, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	free(BufferObject.m_pData);
	BufferObject.m_pData = static_cast<uint8_t *>(malloc(pCommand->m_Desc.m_Size));
	if(pUploadData)
		mem_copy(BufferObject.m_pData, pUploadData, pCommand->m_Desc.m_Size);
}

void CCommandProcessorFragment_OpenGL2::Cmd_UpdateBufferObject(const CCommandBuffer::SCommand_UpdateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	int Index = pCommand->m_Buffer.Id();
	SBufferObject &BufferObject = m_vBufferObjectIndices[Index];

	glBindBuffer(GL_ARRAY_BUFFER, BufferObject.m_BufferObjectId);
	glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(pCommand->m_Offset), (GLsizeiptr)(pCommand->m_DataSize), pUploadData);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	if(pUploadData)
		mem_copy(BufferObject.m_pData + pCommand->m_Offset, pUploadData, pCommand->m_DataSize);
}

void CCommandProcessorFragment_OpenGL2::Cmd_CopyBufferObject(const CCommandBuffer::SCommand_CopyBufferObject *pCommand)
{
	int WriteIndex = pCommand->m_WriteBuffer.Id();
	int ReadIndex = pCommand->m_ReadBuffer.Id();

	SBufferObject &ReadBufferObject = m_vBufferObjectIndices[ReadIndex];
	SBufferObject &WriteBufferObject = m_vBufferObjectIndices[WriteIndex];

	mem_copy(WriteBufferObject.m_pData + (ptrdiff_t)pCommand->m_WriteOffset, ReadBufferObject.m_pData + (ptrdiff_t)pCommand->m_ReadOffset, pCommand->m_CopySize);

	glBindBuffer(GL_ARRAY_BUFFER, WriteBufferObject.m_BufferObjectId);
	glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(pCommand->m_WriteOffset), (GLsizeiptr)(pCommand->m_CopySize), WriteBufferObject.m_pData + (ptrdiff_t)pCommand->m_WriteOffset);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void CCommandProcessorFragment_OpenGL2::Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand)
{
	int Index = pCommand->m_Buffer.Id();
	SBufferObject &BufferObject = m_vBufferObjectIndices[Index];

	glDeleteBuffers(1, &BufferObject.m_BufferObjectId);

	free(BufferObject.m_pData);
	BufferObject.m_pData = nullptr;
}

void CCommandProcessorFragment_OpenGL2::Cmd_CreateBufferContainer(const CCommandBuffer::SCommand_CreateBufferContainer *pCommand)
{
	const int Index = pCommand->m_BufferContainer.Id();
	// create necessary space
	if((size_t)Index >= m_vBufferContainers.size())
	{
		SBufferContainer Container;
		Container.m_ContainerInfo.m_Stride = 0;
		Container.m_ContainerInfo.m_VertBufferBinding.Invalidate();
		m_vBufferContainers.resize(Index + 1, Container);
	}

	SBufferContainer &BufferContainer = m_vBufferContainers[Index];

	for(size_t i = 0; i < pCommand->m_AttrCount; ++i)
	{
		BufferContainer.m_ContainerInfo.m_vAttributes.push_back(pCommand->m_pAttributes[i]);
	}

	BufferContainer.m_ContainerInfo.m_Stride = pCommand->m_Stride;
	BufferContainer.m_ContainerInfo.m_VertBufferBinding = pCommand->m_VertBufferBinding;
}

void CCommandProcessorFragment_OpenGL2::Cmd_UpdateBufferContainer(const CCommandBuffer::SCommand_UpdateBufferContainer *pCommand)
{
	SBufferContainer &BufferContainer = m_vBufferContainers[pCommand->m_BufferContainer.Id()];

	BufferContainer.m_ContainerInfo.m_vAttributes.clear();

	for(size_t i = 0; i < pCommand->m_AttrCount; ++i)
	{
		BufferContainer.m_ContainerInfo.m_vAttributes.push_back(pCommand->m_pAttributes[i]);
	}

	BufferContainer.m_ContainerInfo.m_Stride = pCommand->m_Stride;
	BufferContainer.m_ContainerInfo.m_VertBufferBinding = pCommand->m_VertBufferBinding;
}

void CCommandProcessorFragment_OpenGL2::Cmd_DeleteBufferContainer(const CCommandBuffer::SCommand_DeleteBufferContainer *pCommand)
{
	SBufferContainer &BufferContainer = m_vBufferContainers[pCommand->m_BufferContainer.Id()];

	if(pCommand->m_DestroyAllBO)
	{
		int VertBufferId = BufferContainer.m_ContainerInfo.m_VertBufferBinding.Id();
		if(VertBufferId != -1)
		{
			glDeleteBuffers(1, &m_vBufferObjectIndices[VertBufferId].m_BufferObjectId);

			free(m_vBufferObjectIndices[VertBufferId].m_pData);
			m_vBufferObjectIndices[VertBufferId].m_pData = nullptr;
		}
	}

	BufferContainer.m_ContainerInfo.m_vAttributes.clear();
}

void CCommandProcessorFragment_OpenGL2::Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
	if(pCommand->IsTransient())
	{
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		CCommandProcessorFragment_OpenGL::Cmd_DrawIndexed(pCommand);
		return;
	}
	const EPipelineProgram Program = PipelineProgram(pCommand->m_Pipeline);
	if(Program != EPipelineProgram::ARRAY_COLOR && Program != EPipelineProgram::ARRAY_COLOR_TRANSFORM)
		return;
	constexpr size_t QuadIndexBytes = 6 * sizeof(uint32_t);
	if(pCommand->m_IndexCount == 0 || pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % QuadIndexBytes != 0)
		return;

	int Index = pCommand->m_BufferContainer.Id();
	if((size_t)Index >= m_vBufferContainers.size())
		return;
	SBufferContainer &BufferContainer = m_vBufferContainers[Index];

	const bool HasTransform = Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM;
	const auto *pColorData = HasTransform ? nullptr : pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColor>();
	const auto *pTransformData = HasTransform ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColorTransform>() : nullptr;
	if((HasTransform && pTransformData == nullptr) || (!HasTransform && pColorData == nullptr))
		return;

	CGLSLTileProgram *pProgram;
	if(HasTransform)
		pProgram = IsTexturedState(pCommand->m_State) ? m_pBorderTileProgramTextured : m_pBorderTileProgram;
	else
		pProgram = IsTexturedState(pCommand->m_State) ? m_pTileProgramTextured : m_pTileProgram;
	UseProgram(pProgram);
	SetState(pCommand->m_State, pProgram, true);
	const ColorRGBA &Color = HasTransform ? pTransformData->m_Color : pColorData->m_Color;
	pProgram->SetUniformVec4(pProgram->m_LocColor, 1, (float *)&Color);
	if(HasTransform)
	{
		pProgram->SetUniformVec2(pProgram->m_LocOffset, 1, (float *)&pTransformData->m_Offset);
		pProgram->SetUniformVec2(pProgram->m_LocScale, 1, (float *)&pTransformData->m_Scale);
	}

	const bool IsTextured = BufferContainer.m_ContainerInfo.m_vAttributes.size() == 2;
	SBufferObject &BufferObject = m_vBufferObjectIndices[(size_t)BufferContainer.m_ContainerInfo.m_VertBufferBinding.Id()];
	glBindBuffer(GL_ARRAY_BUFFER, BufferObject.m_BufferObjectId);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, false, static_cast<GLsizei>(BufferContainer.m_ContainerInfo.m_Stride), reinterpret_cast<const void *>(BufferContainer.m_ContainerInfo.m_vAttributes[0].m_Offset));
	if(IsTextured)
	{
		glEnableVertexAttribArray(1);
		glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, static_cast<GLsizei>(BufferContainer.m_ContainerInfo.m_Stride), reinterpret_cast<const void *>(BufferContainer.m_ContainerInfo.m_vAttributes[1].m_Offset));
	}

	const GLsizei RealDrawCount = static_cast<GLsizei>((pCommand->m_IndexCount / 6) * 4);
	const GLint RealOffset = static_cast<GLint>((pCommand->m_IndexOffset / QuadIndexBytes) * 4);
	glDrawArrays(GL_QUADS, RealOffset, RealDrawCount);

	glDisableVertexAttribArray(0);
	if(IsTextured)
		glDisableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

#undef BACKEND_GL_MODERN_API

#endif

#endif
