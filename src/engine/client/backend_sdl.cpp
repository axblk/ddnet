#include <base/detect.h>

#ifndef CONF_BACKEND_OPENGL_ES
#include <GL/glew.h>
#endif

#include <base/log.h>
#include <base/math.h>
#include <base/sphore.h>
#include <base/str.h>
#include <base/thread.h>

#include <engine/shared/config.h>
#include <engine/shared/localization.h>

#include <SDL.h>
#include <SDL_messagebox.h>
#if defined(CONF_BACKEND_WEBGPU)
#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/html5.h>
#elif defined(CONF_PLATFORM_MACOS)
#include <SDL_metal.h>
#endif
#endif
#include "backend_sdl.h"

#include <SDL_vulkan.h>

#if defined(CONF_HEADLESS_CLIENT)
#include "backend/null/backend_null.h"
#endif

#if !defined(CONF_BACKEND_OPENGL_ES)
#include "backend/opengl/backend_opengl3.h"
#endif

#if defined(CONF_BACKEND_OPENGL_ES3) || defined(CONF_BACKEND_OPENGL_ES)
#include "backend/opengles/backend_opengles3.h"
#endif

#if defined(CONF_BACKEND_VULKAN)
#include "backend/vulkan/backend_vulkan.h"
#endif

#if defined(CONF_BACKEND_WEBGPU)
#include "backend/webgpu/backend_webgpu.h"
#endif

#include "graphics_threaded.h"

#include <engine/graphics.h>

#include <algorithm>
#include <cstdlib>

#if defined(CONF_BACKEND_WEBGPU) && !defined(CONF_PLATFORM_EMSCRIPTEN) && !defined(CONF_PLATFORM_MACOS)
#include <SDL_syswm.h>
#endif

class IStorage;

// ------------ CGraphicsBackend_Threaded

// Run everything single threaded when compiling for Emscripten, as context binding does not work outside of the main thread with SDL2.
// TODO SDL3: Check if SDL3 supports threaded graphics and PROXY_TO_PTHREAD, OFFSCREENCANVAS_SUPPORT and OFFSCREEN_FRAMEBUFFER correctly.
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
void CGraphicsBackend_Threaded::ThreadFunc(void *pUser)
{
	auto *pSelf = (CGraphicsBackend_Threaded *)pUser;
	pSelf->m_ThreadStarted.Signal();
	CRenderCommandQueue::SEntry QueuedBuffer;
	while(pSelf->m_CommandQueue.WaitDequeue(QueuedBuffer))
	{
		bool CanProcess;
		{
			std::unique_lock Lock(pSelf->m_ProcessorErrorMutex);
			CanProcess = pSelf->m_ProcessorError.m_ErrorType == GFX_ERROR_TYPE_NONE;
		}
#ifdef CONF_PLATFORM_MACOS
		CAutoreleasePool AutoreleasePool;
#endif
		CCommandBuffer *pBuffer = QueuedBuffer.m_pBuffer;
		SGfxErrorContainer ProcessorError;
		if(CanProcess)
		{
			pSelf->m_pProcessor->RunBuffer(pBuffer);
			ProcessorError = pSelf->m_pProcessor->GetError();
		}
		pBuffer->SignalCompletions();

		{
			std::unique_lock Lock(pSelf->m_ProcessorErrorMutex);
			if(pSelf->m_ProcessorError.m_ErrorType == GFX_ERROR_TYPE_NONE && ProcessorError.m_ErrorType != GFX_ERROR_TYPE_NONE)
				pSelf->m_ProcessorError = std::move(ProcessorError);
		}
		pSelf->m_CommandQueue.Recycle(std::move(QueuedBuffer), CanProcess);
	}
}
#endif

void CGraphicsBackend_Threaded::StartProcessor(ICommandProcessor *pProcessor)
{
	dbg_assert(m_CommandQueue.IsStopped(), "Processor was already not shut down.");
	m_pProcessor = pProcessor;
	{
		std::unique_lock Lock(m_ProcessorErrorMutex);
		m_ProcessorError = {};
	}
	m_CommandQueue.Start();
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	m_pThread = thread_init(ThreadFunc, this, "Graphics thread");
	m_ThreadStarted.Wait();
#endif
}

void CGraphicsBackend_Threaded::StopProcessor()
{
	dbg_assert(!m_CommandQueue.IsStopped(), "Processor was already shut down.");
	WaitForIdle();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	m_Warning = m_pProcessor->GetWarning();
	m_CommandQueue.Stop();
#else
	m_Warning = m_pProcessor->GetWarning();
	m_CommandQueue.Stop();
	thread_wait(m_pThread);
#endif
}

void CGraphicsBackend_Threaded::RunBuffer(CCommandBuffer *pBuffer)
{
	SGfxErrorContainer Error;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	Error = m_pProcessor->GetError();
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
	{
		RunBufferSingleThreadedUnsafe(pBuffer);
		pBuffer->SignalCompletions();
	}
#else
	WaitForIdle();
	{
		std::unique_lock Lock(m_ProcessorErrorMutex);
		Error = m_ProcessorError;
	}
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
	{
		const bool Queued = m_CommandQueue.EnqueueBorrowed(pBuffer);
		dbg_assert(Queued, "graphics: borrowed command buffer published while queue stopped");
	}
#endif

	// Process error after lock is released to prevent deadlock
	if(Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
	{
		pBuffer->SignalCompletions();
		ProcessError(Error);
	}
}

bool CGraphicsBackend_Threaded::RunBufferQueued(CCommandBuffer *pBuffer, bool WaitForCapacity)
{
	dbg_assert(pBuffer->SubmissionInfo().m_Channel == CCommandBuffer::ECommandChannel::RELIABLE, "graphics: reliable publish received a frame packet");
	return RunBufferQueuedInternal(pBuffer, WaitForCapacity);
}

bool CGraphicsBackend_Threaded::RunFramePacket(CCommandBuffer *pBuffer, bool WaitForCapacity)
{
	dbg_assert(pBuffer->SubmissionInfo().m_Channel == CCommandBuffer::ECommandChannel::FRAME, "graphics: frame publish received a reliable buffer");
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	(void)WaitForCapacity;
	RunBuffer(pBuffer);
	pBuffer->Reset();
	m_CommandQueue.RecordSynchronousFrame(true);
	return true;
#else
	SGfxErrorContainer Error;
	{
		std::unique_lock Lock(m_ProcessorErrorMutex);
		Error = m_ProcessorError;
	}
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
	{
		if(WaitForCapacity)
			return m_CommandQueue.WaitEnqueuePinnedFrame(pBuffer);
		return m_CommandQueue.EnqueueFrame(pBuffer) != CRenderCommandQueue::EFrameEnqueueResult::RETRY;
	}

	m_CommandQueue.DiscardFrame(pBuffer);
	ProcessError(Error);
	return true;
#endif
}

IGraphicsBackend::SFrameMailboxStats CGraphicsBackend_Threaded::GetFrameMailboxStats() const
{
	return m_CommandQueue.GetFrameMailboxStats();
}

bool CGraphicsBackend_Threaded::RunBufferQueuedInternal(CCommandBuffer *pBuffer, bool WaitForCapacity)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	(void)WaitForCapacity;
	RunBuffer(pBuffer);
	pBuffer->Reset();
	return true;
#else
	SGfxErrorContainer Error;
	{
		std::unique_lock Lock(m_ProcessorErrorMutex);
		Error = m_ProcessorError;
	}
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
		return WaitForCapacity ? m_CommandQueue.WaitEnqueueReliable(pBuffer) : m_CommandQueue.EnqueueReliable(pBuffer);

	pBuffer->SignalCompletions();
	pBuffer->FreeExternalData();
	pBuffer->Reset();
	ProcessError(Error);
	return true;
#endif
}

void CGraphicsBackend_Threaded::RunBufferSingleThreadedUnsafe(CCommandBuffer *pBuffer)
{
	m_pProcessor->RunBuffer(pBuffer);
}

bool CGraphicsBackend_Threaded::IsIdle() const
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	return true;
#else
	return m_CommandQueue.IsIdle();
#endif
}

void CGraphicsBackend_Threaded::WaitForIdle()
{
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	m_CommandQueue.WaitForIdle();
#endif
}

void CGraphicsBackend_Threaded::ProcessError(const SGfxErrorContainer &Error)
{
	m_Error = Error;
	std::string LogMessage = "Graphics Error:";
	for(const auto &ErrStr : Error.m_vErrors)
	{
		LogMessage.append("\n");
		LogMessage.append(ErrStr);
	}
	dbg_assert_failed("%s", LogMessage.c_str());
}

const SGfxErrorContainer &CGraphicsBackend_Threaded::GetError() const
{
	return m_Error;
}

bool CGraphicsBackend_Threaded::GetWarning(SGfxWarningContainer &Warning)
{
	if(m_Warning.m_WarningType != GFX_WARNING_TYPE_NONE)
	{
		Warning = std::move(m_Warning);
		m_Warning = {};
		return true;
	}
	return false;
}

void CCommandProcessor_SDL::Cmd_Init(const SCommand_Init *pCommand)
{
	m_GLContext = pCommand->m_GLContext;
	m_pWindow = pCommand->m_pWindow;
	if(m_GLContext)
		SDL_GL_MakeCurrent(m_pWindow, m_GLContext);
}

void CCommandProcessor_SDL::Cmd_Shutdown()
{
	if(m_GLContext)
		SDL_GL_MakeCurrent(nullptr, nullptr);
}

void CCommandProcessor_SDL::Cmd_Swap()
{
	if(m_GLContext)
		SDL_GL_SwapWindow(m_pWindow);
}

void CCommandProcessor_SDL::Cmd_VSync(const CCommandBuffer::SCommand_VSync *pCommand)
{
	if(m_GLContext)
	{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// SDL_GL_SetSwapInterval is not supported with Emscripten as this is only a wrapper for the
		// emscripten_set_main_loop_timing function which does not work because we do not use the
		// emscripten_set_main_loop function before.
		pCommand->m_pResult->m_Ok = !pCommand->m_VSync;
#else
		pCommand->m_pResult->m_Ok = SDL_GL_SetSwapInterval(pCommand->m_VSync) == 0;
#endif
	}
}

void CCommandProcessor_SDL::Cmd_WindowCreateNtf(const CCommandBuffer::SCommand_WindowCreateNtf *pCommand)
{
	m_pWindow = SDL_GetWindowFromID(pCommand->m_WindowId);
	// Android destroys windows when they are not visible, so we get the new one and work with that
	// The graphic context does not need to be recreated, just unbound see @see SCommand_WindowDestroyNtf
#ifdef CONF_PLATFORM_ANDROID
	if(m_GLContext)
		SDL_GL_MakeCurrent(m_pWindow, m_GLContext);
#endif
}

void CCommandProcessor_SDL::Cmd_WindowDestroyNtf()
{
	// Unbind the graphic context from the window, so it does not get destroyed
#ifdef CONF_PLATFORM_ANDROID
	if(m_GLContext)
		SDL_GL_MakeCurrent(nullptr, nullptr);
#endif
}

void CCommandProcessor_SDL::RunBuffer(CCommandBuffer *pBuffer)
{
	for(CCommandBuffer::SCommand *pCommand = pBuffer->Head(); pCommand; pCommand = pCommand->m_pNext)
	{
		if(pCommand->m_Cmd == CCommandBuffer::CMD_SIGNAL)
		{
			static_cast<const CCommandBuffer::SCommand_Signal *>(pCommand)->Signal();
			continue;
		}

		auto Res = m_pRendererBackend->RunCommand(pCommand);
		if(Res == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED)
		{
			CCommandBuffer::FreeExternalData(pCommand);
			continue;
		}
		else if(Res == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_ERROR)
		{
			m_Error = m_pRendererBackend->GetError();
			pBuffer->FreeExternalDataFrom(pCommand);
			return;
		}
		else if(Res == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_WARNING)
		{
			if(m_pRendererBackend->GetError().m_ErrorType != GFX_ERROR_TYPE_NONE)
			{
				m_Error = m_pRendererBackend->GetError();
			}
			else
			{
				m_Warning = m_pRendererBackend->GetWarning();
			}
			pBuffer->FreeExternalDataFrom(pCommand);
			return;
		}

		bool PlatformCommandHandled = true;
		switch(pCommand->m_Cmd)
		{
		case CCommandBuffer::CMD_WINDOW_CREATE_NTF: Cmd_WindowCreateNtf(static_cast<const CCommandBuffer::SCommand_WindowCreateNtf *>(pCommand)); break;
		case CCommandBuffer::CMD_WINDOW_DESTROY_NTF: Cmd_WindowDestroyNtf(); break;
		case CCommandBuffer::CMD_SWAP: Cmd_Swap(); break;
		case CCommandBuffer::CMD_VSYNC: Cmd_VSync(static_cast<const CCommandBuffer::SCommand_VSync *>(pCommand)); break;
		case CCommandBuffer::CMD_MULTISAMPLING: break;
		case CMD_INIT: Cmd_Init(static_cast<const SCommand_Init *>(pCommand)); break;
		case CMD_SHUTDOWN: Cmd_Shutdown(); break;
		case CCommandProcessorFragment_Renderer::CMD_PRE_INIT: break;
		case CCommandProcessorFragment_Renderer::CMD_POST_SHUTDOWN: break;
		default: PlatformCommandHandled = false;
		}
		if(PlatformCommandHandled)
		{
			CCommandBuffer::FreeExternalData(pCommand);
			continue;
		}

		pBuffer->FreeExternalDataFrom(pCommand);
		dbg_assert_failed("Unknown graphics command %d", pCommand->m_Cmd);
	}

	if(m_pRendererBackend->GetError().m_ErrorType != GFX_ERROR_TYPE_NONE)
	{
		m_Error = m_pRendererBackend->GetError();
	}
}

CCommandProcessor_SDL::CCommandProcessor_SDL(EBackendType BackendType, int GLMajor, int GLMinor, const SWebGpuNativeWindow &WebGpuNativeWindow, EWebGpuBackendType WebGpuBackendType)
{
#if defined(CONF_HEADLESS_CLIENT)
	m_pRendererBackend = new CCommandProcessorFragment_Null();
#else
	if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
#if defined(CONF_BACKEND_OPENGL_ES) || defined(CONF_BACKEND_OPENGL_ES3)
		if(GLMajor < 3)
		{
			m_pRendererBackend = new CCommandProcessorFragment_OpenGLES();
		}
		else
		{
			m_pRendererBackend = new CCommandProcessorFragment_OpenGLES3();
		}
#endif
	}
	else if(BackendType == BACKEND_TYPE_OPENGL)
	{
#if !defined(CONF_BACKEND_OPENGL_ES)
		if(GLMajor < 2)
		{
			m_pRendererBackend = new CCommandProcessorFragment_OpenGL();
		}
		if(GLMajor == 2)
		{
			m_pRendererBackend = new CCommandProcessorFragment_OpenGL2();
		}
		if(GLMajor == 3 && GLMinor == 0)
		{
			m_pRendererBackend = new CCommandProcessorFragment_OpenGL2();
		}
		else if((GLMajor == 3 && GLMinor == 3) || GLMajor >= 4)
		{
			m_pRendererBackend = new CCommandProcessorFragment_OpenGL3_3();
		}
#endif
	}
	else if(BackendType == BACKEND_TYPE_VULKAN)
	{
#if defined(CONF_BACKEND_VULKAN)
		m_pRendererBackend = CreateVulkanCommandProcessorFragment();
#endif
	}
	else if(BackendType == BACKEND_TYPE_WEBGPU)
	{
#if defined(CONF_BACKEND_WEBGPU)
		m_pRendererBackend = CreateWebGpuCommandProcessorFragment(WebGpuNativeWindow, WebGpuBackendType);
#endif
	}
#endif
	dbg_assert(m_pRendererBackend != nullptr, "graphics command processor backend is unavailable");
}

CCommandProcessor_SDL::~CCommandProcessor_SDL()
{
	delete m_pRendererBackend;
}

static EWebGpuBackendType WebGpuBackendTypeFromConfig()
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "auto") != 0)
	{
		log_warn("gfx", "native WebGPU backend selection '%s' is unavailable in the browser, using auto", g_Config.m_GfxWebGpuBackend);
		str_copy(g_Config.m_GfxWebGpuBackend, "auto");
	}
#else
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "D3D12") == 0 || str_comp_nocase(g_Config.m_GfxWebGpuBackend, "DX12") == 0)
		return EWebGpuBackendType::D3D12;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "Vulkan") == 0)
		return EWebGpuBackendType::VULKAN;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "Metal") == 0)
		return EWebGpuBackendType::METAL;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "OpenGL") == 0)
		return EWebGpuBackendType::OPENGL;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "auto") != 0)
	{
		log_warn("gfx", "unknown wgpu-native backend '%s', using auto", g_Config.m_GfxWebGpuBackend);
		str_copy(g_Config.m_GfxWebGpuBackend, "auto");
	}
#endif
	return EWebGpuBackendType::AUTO;
}

const SGfxErrorContainer &CCommandProcessor_SDL::GetError() const
{
	return m_Error;
}

void CCommandProcessor_SDL::ErroneousCleanup()
{
	m_pRendererBackend->ErroneousCleanup();
}

const SGfxWarningContainer &CCommandProcessor_SDL::GetWarning() const
{
	return m_Warning;
}

// ------------ CGraphicsBackend_SDL

#if !defined(CONF_HEADLESS_CLIENT)
static bool BackendInitGlew(EBackendType BackendType, int &GlewMajor, int &GlewMinor, int &GlewPatch)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
#if !defined(CONF_BACKEND_OPENGL_ES)
		// Support graphic cards that are pretty old (and Linux)
		glewExperimental = GL_TRUE;
#ifdef CONF_GLEW_HAS_CONTEXT_INIT
		const GLenum InitResult = glewContextInit();
		if(InitResult != GLEW_OK)
		{
			log_error("gfx", "Unable to init glew (glewContextInit): %s", glewGetErrorString(InitResult));
			return false;
		}
#else
		const GLenum InitResult = glewInit();
		if(InitResult != GLEW_OK)
		{
			// With wayland the glewInit function is allowed to fail with GLEW_ERROR_NO_GLX_DISPLAY,
			// as it will already have initialized the context with glewContextInit internally.
			const char *pVideoDriver = SDL_GetCurrentVideoDriver();
			if(pVideoDriver == nullptr || str_comp(pVideoDriver, "wayland") != 0 || InitResult != GLEW_ERROR_NO_GLX_DISPLAY)
			{
				log_error("gfx", "Unable to init glew (glewInit): %s", glewGetErrorString(InitResult));
				return false;
			}
		}
#endif

#ifdef GLEW_VERSION_4_6
		if(GLEW_VERSION_4_6)
		{
			GlewMajor = 4;
			GlewMinor = 6;
			GlewPatch = 0;
			return true;
		}
#endif
#ifdef GLEW_VERSION_4_5
		if(GLEW_VERSION_4_5)
		{
			GlewMajor = 4;
			GlewMinor = 5;
			GlewPatch = 0;
			return true;
		}
#endif
// Don't use intermediate OpenGL 4.x versions on Windows.
#ifndef CONF_FAMILY_WINDOWS
		if(GLEW_VERSION_4_4)
		{
			GlewMajor = 4;
			GlewMinor = 4;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_3)
		{
			GlewMajor = 4;
			GlewMinor = 3;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_2)
		{
			GlewMajor = 4;
			GlewMinor = 2;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_1)
		{
			GlewMajor = 4;
			GlewMinor = 1;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_4_0)
		{
			GlewMajor = 4;
			GlewMinor = 0;
			GlewPatch = 0;
			return true;
		}
#endif
		if(GLEW_VERSION_3_3)
		{
			GlewMajor = 3;
			GlewMinor = 3;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_3_0)
		{
			GlewMajor = 3;
			GlewMinor = 0;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_2_1)
		{
			GlewMajor = 2;
			GlewMinor = 1;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_2_0)
		{
			GlewMajor = 2;
			GlewMinor = 0;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_5)
		{
			GlewMajor = 1;
			GlewMinor = 5;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_4)
		{
			GlewMajor = 1;
			GlewMinor = 4;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_3)
		{
			GlewMajor = 1;
			GlewMinor = 3;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_2_1)
		{
			GlewMajor = 1;
			GlewMinor = 2;
			GlewPatch = 1;
			return true;
		}
		if(GLEW_VERSION_1_2)
		{
			GlewMajor = 1;
			GlewMinor = 2;
			GlewPatch = 0;
			return true;
		}
		if(GLEW_VERSION_1_1)
		{
			GlewMajor = 1;
			GlewMinor = 1;
			GlewPatch = 0;
			return true;
		}
#endif
	}
	else if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		// just assume the version we need
		GlewMajor = 3;
		GlewMinor = 0;
		GlewPatch = 0;
		return true;
	}
	else
	{
		dbg_assert_failed("Invalid backend type for glew: %d", (int)BackendType);
	}

	return false;
}

static int IsVersionSupportedGlew(EBackendType BackendType, int VersionMajor, int VersionMinor, int VersionPatch, int GlewMajor, int GlewMinor, int GlewPatch)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
		if(VersionMajor >= 4 && GlewMajor < 4)
		{
			return -1;
		}
		else if(VersionMajor >= 3 && GlewMajor < 3)
		{
			return -1;
		}
		else if(VersionMajor == 3 && GlewMajor == 3)
		{
			if(VersionMinor >= 3 && GlewMinor < 3)
			{
				return -1;
			}
			if(VersionMinor >= 2 && GlewMinor < 2)
			{
				return -1;
			}
			if(VersionMinor >= 1 && GlewMinor < 1)
			{
				return -1;
			}
			if(VersionMinor >= 0 && GlewMinor < 0)
			{
				return -1;
			}
		}
		else if(VersionMajor >= 2 && GlewMajor < 2)
		{
			return -1;
		}
		else if(VersionMajor == 2 && GlewMajor == 2)
		{
			if(VersionMinor >= 1 && GlewMinor < 1)
			{
				return -1;
			}
			if(VersionMinor >= 0 && GlewMinor < 0)
			{
				return -1;
			}
		}
		else if(VersionMajor >= 1 && GlewMajor < 1)
		{
			return -1;
		}
		else if(VersionMajor == 1 && GlewMajor == 1)
		{
			if(VersionMinor >= 5 && GlewMinor < 5)
			{
				return -1;
			}
			if(VersionMinor >= 4 && GlewMinor < 4)
			{
				return -1;
			}
			if(VersionMinor >= 3 && GlewMinor < 3)
			{
				return -1;
			}
			if(VersionMinor >= 2 && GlewMinor < 2)
			{
				return -1;
			}
			else if(VersionMinor == 2 && GlewMinor == 2)
			{
				if(VersionPatch >= 1 && GlewPatch < 1)
				{
					return -1;
				}
				if(VersionPatch >= 0 && GlewPatch < 0)
				{
					return -1;
				}
			}
			if(VersionMinor >= 1 && GlewMinor < 1)
			{
				return -1;
			}
			if(VersionMinor >= 0 && GlewMinor < 0)
			{
				return -1;
			}
		}
	}
	return 0;
}
#endif // !CONF_HEADLESS_CLIENT

EBackendType CGraphicsBackend_SDL::DetectBackend() const
{
	EBackendType RetBackendType = BACKEND_TYPE_OPENGL;
	if(m_BackendOverride != BACKEND_TYPE_AUTO)
		RetBackendType = m_BackendOverride;
	else
	{
		// load the config backend
		const char *pConfBackend = g_Config.m_GfxBackend;
		if(str_comp_nocase(pConfBackend, "GLES") == 0)
			RetBackendType = BACKEND_TYPE_OPENGL_ES;
#if defined(CONF_BACKEND_VULKAN)
		else if(str_comp_nocase(pConfBackend, "Vulkan") == 0)
			RetBackendType = BACKEND_TYPE_VULKAN;
#endif
#if defined(CONF_BACKEND_WEBGPU)
		else if(str_comp_nocase(pConfBackend, "WebGPU") == 0)
			RetBackendType = BACKEND_TYPE_WEBGPU;
#endif
		else if(str_comp_nocase(pConfBackend, "OpenGL") == 0)
			RetBackendType = BACKEND_TYPE_OPENGL;
	}
#if !defined(CONF_BACKEND_OPENGL_ES) && !defined(CONF_BACKEND_OPENGL_ES3)
	if(RetBackendType == BACKEND_TYPE_OPENGL_ES)
		RetBackendType = BACKEND_TYPE_OPENGL;
#elif defined(CONF_BACKEND_OPENGL_ES)
	if(RetBackendType == BACKEND_TYPE_OPENGL)
		RetBackendType = BACKEND_TYPE_OPENGL_ES;
#endif
	return RetBackendType;
}

void CGraphicsBackend_SDL::ClampDriverVersion(EBackendType BackendType)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
		// clamp the versions to existing versions(only for OpenGL major <= 3)
		if(g_Config.m_GfxGLMajor == 1)
		{
			g_Config.m_GfxGLMinor = std::clamp(g_Config.m_GfxGLMinor, 1, 5);
			if(g_Config.m_GfxGLMinor == 2)
				g_Config.m_GfxGLPatch = std::clamp(g_Config.m_GfxGLPatch, 0, 1);
			else
				g_Config.m_GfxGLPatch = 0;
		}
		else if(g_Config.m_GfxGLMajor == 2)
		{
			g_Config.m_GfxGLMinor = std::clamp(g_Config.m_GfxGLMinor, 0, 1);
			g_Config.m_GfxGLPatch = 0;
		}
		else if(g_Config.m_GfxGLMajor == 3)
		{
			g_Config.m_GfxGLMinor = std::clamp(g_Config.m_GfxGLMinor, 0, 3);
			if(g_Config.m_GfxGLMinor < 3)
				g_Config.m_GfxGLMinor = 0;
			g_Config.m_GfxGLPatch = 0;
		}
	}
	else if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
#if !defined(CONF_BACKEND_OPENGL_ES3)
		// Make sure GLES is set to 1.0 (which is equivalent to OpenGL 1.3), if its not set to >= 3.0(which is equivalent to OpenGL 3.3)
		if(g_Config.m_GfxGLMajor < 3)
		{
			g_Config.m_GfxGLMajor = 1;
			g_Config.m_GfxGLMinor = 0;
			g_Config.m_GfxGLPatch = 0;

			// GLES also doesn't know GL_QUAD
			g_Config.m_GfxQuadAsTriangle = 1;
		}
#else
		g_Config.m_GfxGLMajor = 3;
		g_Config.m_GfxGLMinor = 0;
		g_Config.m_GfxGLPatch = 0;
#endif
	}
	else if(BackendType == BACKEND_TYPE_VULKAN)
	{
#if defined(CONF_BACKEND_VULKAN)
		g_Config.m_GfxGLMajor = BACKEND_VULKAN_VERSION_MAJOR;
		g_Config.m_GfxGLMinor = BACKEND_VULKAN_VERSION_MINOR;
		g_Config.m_GfxGLPatch = 0;
#endif
	}
	else if(BackendType == BACKEND_TYPE_WEBGPU)
	{
		g_Config.m_GfxGLMajor = 1;
		g_Config.m_GfxGLMinor = 0;
		g_Config.m_GfxGLPatch = 0;
	}
}

static void GetDrawableSize(SDL_Window *pWindow, EBackendType BackendType, int *pWidth, int *pHeight)
{
	if(BackendType == BACKEND_TYPE_VULKAN)
		SDL_Vulkan_GetDrawableSize(pWindow, pWidth, pHeight);
	else if(BackendType == BACKEND_TYPE_WEBGPU)
	{
#if defined(CONF_BACKEND_WEBGPU) && defined(CONF_PLATFORM_EMSCRIPTEN)
		if(emscripten_get_canvas_element_size("#canvas", pWidth, pHeight) != EMSCRIPTEN_RESULT_SUCCESS)
			SDL_GetWindowSize(pWindow, pWidth, pHeight);
#elif defined(CONF_BACKEND_WEBGPU) && defined(CONF_PLATFORM_MACOS)
		SDL_Metal_GetDrawableSize(pWindow, pWidth, pHeight);
#elif SDL_VERSION_ATLEAST(2, 26, 0)
		SDL_GetWindowSizeInPixels(pWindow, pWidth, pHeight);
#else
		SDL_GetWindowSize(pWindow, pWidth, pHeight);
#endif
	}
	else
		SDL_GL_GetDrawableSize(pWindow, pWidth, pHeight);
}

#if defined(CONF_BACKEND_WEBGPU) && !defined(CONF_PLATFORM_MACOS) && !defined(CONF_PLATFORM_EMSCRIPTEN)
static bool GetWebGpuNativeWindow(SDL_Window *pWindow, SWebGpuNativeWindow &NativeWindow)
{
	SDL_SysWMinfo Info{};
	SDL_VERSION(&Info.version);
	if(SDL_GetWindowWMInfo(pWindow, &Info) != SDL_TRUE)
		return false;
	switch(Info.subsystem)
	{
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
	case SDL_SYSWM_WINDOWS:
		NativeWindow.m_Type = SWebGpuNativeWindow::EType::WINDOWS;
		NativeWindow.m_pDisplay = Info.info.win.hinstance;
		NativeWindow.m_pWindow = Info.info.win.window;
		return true;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
	case SDL_SYSWM_X11:
		NativeWindow.m_Type = SWebGpuNativeWindow::EType::XLIB;
		NativeWindow.m_pDisplay = Info.info.x11.display;
		NativeWindow.m_WindowId = Info.info.x11.window;
		return true;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
	case SDL_SYSWM_WAYLAND:
		NativeWindow.m_Type = SWebGpuNativeWindow::EType::WAYLAND;
		NativeWindow.m_pDisplay = Info.info.wl.display;
		NativeWindow.m_pWindow = Info.info.wl.surface;
		return true;
#endif
	default:
		return false;
	}
}
#endif

void CGraphicsBackend_SDL::DestroyWindow()
{
#if defined(CONF_BACKEND_WEBGPU) && defined(CONF_PLATFORM_MACOS)
	if(m_pWebGpuMetalView != nullptr)
	{
		SDL_Metal_DestroyView(m_pWebGpuMetalView);
		m_pWebGpuMetalView = nullptr;
	}
#endif
	m_WebGpuNativeWindow = {};
	SDL_DestroyWindow(m_pWindow);
	m_pWindow = nullptr;
}

static Uint32 MessageBoxTypeToSdlFlags(IGraphics::EMessageBoxType Type)
{
	switch(Type)
	{
	case IGraphics::EMessageBoxType::ERROR:
		return SDL_MESSAGEBOX_ERROR;
	case IGraphics::EMessageBoxType::WARNING:
		return SDL_MESSAGEBOX_WARNING;
	case IGraphics::EMessageBoxType::INFO:
		return SDL_MESSAGEBOX_INFORMATION;
	default:
		dbg_assert_failed("Type invalid");
	}
}

static std::optional<int> ShowMessageBoxImpl(const IGraphics::CMessageBox &MessageBox, SDL_Window *pWindow)
{
	dbg_assert(!MessageBox.m_vButtons.empty(), "At least one button is required");

	std::vector<SDL_MessageBoxButtonData> vButtonData;
	vButtonData.reserve(MessageBox.m_vButtons.size());
	for(const auto &Button : MessageBox.m_vButtons)
	{
		SDL_MessageBoxButtonData ButtonData{};
		ButtonData.buttonid = vButtonData.size();
		ButtonData.flags = (Button.m_Confirm ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0) | (Button.m_Cancel ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0);
		ButtonData.text = Button.m_pLabel;
		vButtonData.emplace_back(ButtonData);
	}
#if defined(CONF_FAMILY_WINDOWS)
	// TODO SDL3: The order of buttons is not defined by default, but the flags returned by MessageBoxTypeToSdlFlags do not work together
	//            with SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT with SDL2 on various platforms. Windows appears to be the only platform that
	//            lays out buttons from right to left by default, so we reverse the order manually.
	std::reverse(vButtonData.begin(), vButtonData.end());
#endif
	SDL_MessageBoxData MessageBoxData{};
	MessageBoxData.title = MessageBox.m_pTitle;
	MessageBoxData.message = MessageBox.m_pMessage;
	MessageBoxData.flags = MessageBoxTypeToSdlFlags(MessageBox.m_Type);
	MessageBoxData.numbuttons = vButtonData.size();
	MessageBoxData.buttons = vButtonData.data();
	MessageBoxData.window = pWindow;
	int ButtonId = -1;
	if(SDL_ShowMessageBox(&MessageBoxData, &ButtonId) != 0)
	{
		return std::nullopt;
	}
	return ButtonId;
}

std::optional<int> ShowMessageBoxWithoutGraphics(const IGraphics::CMessageBox &MessageBox)
{
	return ShowMessageBoxImpl(MessageBox, nullptr);
}

std::optional<int> CGraphicsBackend_SDL::ShowMessageBox(const IGraphics::CMessageBox &MessageBox)
{
	if(m_BackendType == BACKEND_TYPE_WEBGPU)
		return ShowMessageBoxImpl(MessageBox, nullptr);
	if(m_pProcessor != nullptr)
	{
		m_pProcessor->ErroneousCleanup();
	}
	// TODO: Remove this workaround when https://github.com/libsdl-org/SDL/issues/3750 is
	// fixed and pass the window to SDL_ShowSimpleMessageBox to make the popup modal instead
	// of destroying the window before opening the popup.
	if(m_pWindow != nullptr)
	{
		SDL_DestroyWindow(m_pWindow);
		m_pWindow = nullptr;
	}
	return ShowMessageBoxImpl(MessageBox, m_pWindow);
}

bool CGraphicsBackend_SDL::IsModernAPI(EBackendType BackendType)
{
	if(BackendType == BACKEND_TYPE_OPENGL)
		return (g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor == 3) || g_Config.m_GfxGLMajor >= 4;
	else if(BackendType == BACKEND_TYPE_OPENGL_ES)
		return g_Config.m_GfxGLMajor >= 3;
	else if(BackendType == BACKEND_TYPE_VULKAN)
		return true;
	else if(BackendType == BACKEND_TYPE_WEBGPU)
		return true;

	return false;
}

bool CGraphicsBackend_SDL::GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType)
{
	if(BackendType == BACKEND_TYPE_AUTO)
		BackendType = m_BackendType;
	if(BackendType == BACKEND_TYPE_OPENGL)
	{
		pName = "OpenGL";
#ifndef CONF_BACKEND_OPENGL_ES
		if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_LEGACY)
		{
			Major = 1;
			Minor = 4;
			Patch = 0;
			return true;
		}
		else if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_DEFAULT)
		{
			Major = 3;
			Minor = 0;
			Patch = 0;
			return true;
		}
		else if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_MODERN)
		{
			Major = 3;
			Minor = 3;
			Patch = 0;
			return true;
		}
#endif
	}
	else if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		pName = "GLES";
#ifdef CONF_BACKEND_OPENGL_ES
		if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_LEGACY)
		{
			Major = 1;
			Minor = 0;
			Patch = 0;
			return true;
		}
		else if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_DEFAULT)
		{
			Major = 3;
			Minor = 0;
			Patch = 0;
			// there isn't really a default one
			return false;
		}
#endif
#ifdef CONF_BACKEND_OPENGL_ES3
		if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_MODERN)
		{
			Major = 3;
			Minor = 0;
			Patch = 0;
			return true;
		}
#endif
	}
	else if(BackendType == BACKEND_TYPE_VULKAN)
	{
		pName = "Vulkan";
#ifdef CONF_BACKEND_VULKAN
		if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_DEFAULT)
		{
			Major = BACKEND_VULKAN_VERSION_MAJOR;
			Minor = BACKEND_VULKAN_VERSION_MINOR;
			Patch = 0;
			return true;
		}
#else
		return false;
#endif
	}
	else if(BackendType == BACKEND_TYPE_WEBGPU)
	{
		pName = "WebGPU";
#ifdef CONF_BACKEND_WEBGPU
		if(DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_DEFAULT)
		{
			Major = 1;
			Minor = 0;
			Patch = 0;
			return true;
		}
#else
		return false;
#endif
	}
	return false;
}

const char *CGraphicsBackend_SDL::GetScreenName(int ScreenIndex) const
{
	const char *pName = SDL_GetDisplayName(ScreenIndex);
	return pName == nullptr ? "unknown/error" : pName;
}

static void DisplayToVideoMode(CVideoMode *pVMode, SDL_DisplayMode *pMode, float HiDPIScale, int RefreshRate)
{
	pVMode->m_CanvasWidth = pMode->w * HiDPIScale;
	pVMode->m_CanvasHeight = pMode->h * HiDPIScale;
	pVMode->m_WindowWidth = pMode->w;
	pVMode->m_WindowHeight = pMode->h;
	pVMode->m_RefreshRate = RefreshRate;
}

void CGraphicsBackend_SDL::GetVideoModes(CVideoMode *pModes, int MaxModes, int *pNumModes, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId)
{
	SDL_DisplayMode DesktopMode;
	int MaxModesAvailable = SDL_GetNumDisplayModes(ScreenId);

	// Only collect fullscreen modes when requested, that makes sure in windowed mode no refresh rates are shown that aren't supported without
	// fullscreen anyway(except fullscreen desktop)
	bool IsFullscreenDesktop = m_pWindow != nullptr && (((SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP) || g_Config.m_GfxFullscreen == 3);
	bool CollectFullscreenModes = m_pWindow == nullptr || ((SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN) != 0 && !IsFullscreenDesktop);

	if(SDL_GetDesktopDisplayMode(ScreenId, &DesktopMode) < 0)
	{
		log_error("gfx", "Unable to get desktop display mode of screen %d: %s", ScreenId, SDL_GetError());
	}

	constexpr int ModeCount = 256;
	SDL_DisplayMode aModes[ModeCount];
	int NumModes = 0;
	for(int i = 0; i < MaxModesAvailable && NumModes < ModeCount; i++)
	{
		SDL_DisplayMode Mode;
		if(SDL_GetDisplayMode(ScreenId, i, &Mode) < 0)
		{
			log_error("gfx", "Unable to get display mode %d of screen %d: %s", i, ScreenId, SDL_GetError());
			continue;
		}

		aModes[NumModes] = Mode;
		++NumModes;
	}

	int NumModesInserted = 0;
	auto &&ModeInsert = [&](SDL_DisplayMode &Mode) {
		if(NumModesInserted < MaxModes)
		{
			// if last mode was equal, ignore this one --- in fullscreen this can really only happen if the screen
			// supports different color modes
			// in non fullscreen these are the modes that show different refresh rate, but are basically the same
			if(NumModesInserted > 0 && pModes[NumModesInserted - 1].m_WindowWidth == Mode.w && pModes[NumModesInserted - 1].m_WindowHeight == Mode.h && (pModes[NumModesInserted - 1].m_RefreshRate == Mode.refresh_rate || (Mode.refresh_rate != DesktopMode.refresh_rate && !CollectFullscreenModes)))
				return;

			DisplayToVideoMode(&pModes[NumModesInserted], &Mode, HiDPIScale, !CollectFullscreenModes ? DesktopMode.refresh_rate : Mode.refresh_rate);
			NumModesInserted++;
		}
	};

	for(int i = 0; i < NumModes; i++)
	{
		SDL_DisplayMode &Mode = aModes[i];

		if(Mode.w > MaxWindowWidth || Mode.h > MaxWindowHeight)
			continue;

		ModeInsert(Mode);

		if(IsFullscreenDesktop)
			break;

		if(NumModesInserted >= MaxModes)
			break;
	}
	*pNumModes = NumModesInserted;
}

void CGraphicsBackend_SDL::GetCurrentVideoMode(CVideoMode &CurMode, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId)
{
	SDL_DisplayMode DpMode;
	// if "real" fullscreen, obtain the video mode for that
	if((SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN)
	{
		if(SDL_GetCurrentDisplayMode(ScreenId, &DpMode))
		{
			log_error("gfx", "Unable to get current display mode of screen %d: %s", ScreenId, SDL_GetError());
		}
	}
	else
	{
		if(SDL_GetDesktopDisplayMode(ScreenId, &DpMode) < 0)
		{
			log_error("gfx", "Unable to get desktop display mode of screen %d: %s", ScreenId, SDL_GetError());
		}
		else
		{
			int Width = 0;
			int Height = 0;
			GetDrawableSize(m_pWindow, m_BackendType, &Width, &Height);
			// SDL video modes are in screen space which are logical pixels
			DpMode.w = Width / HiDPIScale;
			DpMode.h = Height / HiDPIScale;
		}
	}
	DisplayToVideoMode(&CurMode, &DpMode, HiDPIScale, DpMode.refresh_rate);
}

CGraphicsBackend_SDL::CGraphicsBackend_SDL(EBackendType BackendOverride) :
	m_BackendOverride(BackendOverride)
{
	m_aErrorString[0] = '\0';
}

int CGraphicsBackend_SDL::Init(const char *pName, int *pScreen, int *pWidth, int *pHeight, int *pRefreshRate, int *pFsaaSamples, int Flags, int *pDesktopWidth, int *pDesktopHeight, int *pCurrentWidth, int *pCurrentHeight, IStorage *pStorage)
{
#if defined(CONF_HEADLESS_CLIENT)
	m_BackendType = BACKEND_TYPE_OPENGL;
	g_Config.m_GfxGLMajor = 0;
	g_Config.m_GfxGLMinor = 0;
	g_Config.m_GfxGLPatch = 0;
	int InitError = 0;
	int GlewMajor = 0;
	int GlewMinor = 0;
	int GlewPatch = 0;
	*pScreen = 0;
	*pWidth = *pDesktopWidth = *pCurrentWidth = 800;
	*pHeight = *pDesktopHeight = *pCurrentHeight = 600;
	*pRefreshRate = 60;
	*pFsaaSamples = 0;
	log_info("gfx", "Created headless context");
#else
	// print sdl version
	{
		SDL_version Compiled;
		SDL_version Linked;

		SDL_VERSION(&Compiled);
		SDL_GetVersion(&Linked);
		log_info("sdl", "SDL version %d.%d.%d (compiled = %d.%d.%d)",
			Linked.major, Linked.minor, Linked.patch,
			Compiled.major, Compiled.minor, Compiled.patch);

#if CONF_PLATFORM_LINUX && SDL_VERSION_ATLEAST(2, 0, 22)
		// needed to workaround SDL from forcing exclusively X11 if linking against the GLX flavour of GLEW instead of the EGL one
		// w/o this on Wayland systems (no XWayland support) SDL's Video subsystem will fail to load (starting from SDL2.30+)
		if(Linked.major == 2 && Linked.minor >= 30)
			SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11,wayland");
#endif
	}

	if(!SDL_WasInit(SDL_INIT_VIDEO))
	{
		if(SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
		{
			log_error("gfx", "Unable to initialize SDL video: %s", SDL_GetError());
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SDL_INIT_FAILED;
		}
	}

	EBackendType OldBackendType = m_BackendType;
	m_BackendType = DetectBackend();
	if(OldBackendType == BACKEND_TYPE_WEBGPU && m_BackendType == BACKEND_TYPE_WEBGPU)
	{
		log_warn("gfx/webgpu", "initialization failed; retrying with OpenGL");
		m_BackendType = BACKEND_TYPE_OPENGL;
		g_Config.m_GfxGLMajor = 3;
		g_Config.m_GfxGLMinor = 0;
		g_Config.m_GfxGLPatch = 0;
	}
	// little fallback for Vulkan
	if(OldBackendType != BACKEND_TYPE_AUTO &&
		m_BackendType == BACKEND_TYPE_VULKAN)
	{
		// try default opengl settings
		str_copy(g_Config.m_GfxBackend, "OpenGL");
		g_Config.m_GfxGLMajor = 3;
		g_Config.m_GfxGLMinor = 0;
		g_Config.m_GfxGLPatch = 0;
		// do another analysis round too, just in case
		g_Config.m_Gfx3DTextureAnalysisRan = 0;
		g_Config.m_GfxDriverIsBlocked = 0;
		m_BackendType = DetectBackend();
	}

	ClampDriverVersion(m_BackendType);

	const bool UseModernGL = IsModernAPI(m_BackendType);
	const bool IsOpenGLFamilyBackend = m_BackendType == BACKEND_TYPE_OPENGL || m_BackendType == BACKEND_TYPE_OPENGL_ES;

	if(IsOpenGLFamilyBackend)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, g_Config.m_GfxGLMajor);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, g_Config.m_GfxGLMinor);
	}

	const char *pBackendName;
	switch(m_BackendType)
	{
	case BACKEND_TYPE_OPENGL:
		pBackendName = "OpenGL";
		break;
	case BACKEND_TYPE_OPENGL_ES:
		pBackendName = "OpenGL ES";
		break;
	case BACKEND_TYPE_VULKAN:
		pBackendName = "Vulkan";
		break;
	case BACKEND_TYPE_WEBGPU:
		pBackendName = "experimental WebGPU";
		break;
	default:
		dbg_assert_failed("Invalid m_BackendType: %d", m_BackendType);
	}
	if(m_BackendType == BACKEND_TYPE_WEBGPU)
		log_info("gfx", "Created %s context", pBackendName);
	else
		log_info("gfx", "Created %s %d.%d context", pBackendName, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor);

	if(m_BackendType == BACKEND_TYPE_OPENGL)
	{
		if(g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor == 0)
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		}
		else if(UseModernGL)
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		}
	}
	else if(m_BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	}

	if(IsOpenGLFamilyBackend)
	{
		*pFsaaSamples = std::clamp(*pFsaaSamples, 0, 8);
	}
	else if(m_BackendType == BACKEND_TYPE_WEBGPU)
	{
		*pFsaaSamples = *pFsaaSamples >= 2 ? 4 : 0;
	}

	// set screen
	m_NumScreens = SDL_GetNumVideoDisplays();
	if(m_NumScreens > 0)
	{
		SDL_Rect ScreenPos;
		*pScreen = std::clamp(*pScreen, 0, m_NumScreens - 1);
		if(SDL_GetDisplayBounds(*pScreen, &ScreenPos) != 0)
		{
			log_error("gfx", "Unable to get display bounds of screen %d: %s", *pScreen, SDL_GetError());
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SDL_SCREEN_INFO_REQUEST_FAILED;
		}
	}
	else
	{
		log_error("gfx", "Unable to get number of screens: %s", SDL_GetError());
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SDL_SCREEN_REQUEST_FAILED;
	}

	// store desktop resolution for settings reset button
	SDL_DisplayMode DisplayMode;
	if(SDL_GetDesktopDisplayMode(*pScreen, &DisplayMode))
	{
		log_error("gfx", "Unable to get desktop display mode of screen %d: %s", *pScreen, SDL_GetError());
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SDL_SCREEN_RESOLUTION_REQUEST_FAILED;
	}

	bool IsDesktopChanged = *pDesktopWidth == 0 || *pDesktopHeight == 0 || *pDesktopWidth != DisplayMode.w || *pDesktopHeight != DisplayMode.h;

	*pDesktopWidth = DisplayMode.w;
	*pDesktopHeight = DisplayMode.h;

	// fetch supported video modes
	bool SupportedResolution = false;

	CVideoMode aModes[256];
	int ModesCount = 0;
	int IndexOfResolution = -1;
	GetVideoModes(aModes, std::size(aModes), &ModesCount, 1, *pDesktopWidth, *pDesktopHeight, *pScreen);

	for(int i = 0; i < ModesCount; i++)
	{
		if(*pWidth == aModes[i].m_WindowWidth && *pHeight == aModes[i].m_WindowHeight && (*pRefreshRate == aModes[i].m_RefreshRate || *pRefreshRate == 0))
		{
			SupportedResolution = true;
			IndexOfResolution = i;
			break;
		}
	}

	// set flags
	int SdlFlags = SDL_WINDOW_INPUT_GRABBED | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_ALLOW_HIGHDPI;
	if(IsOpenGLFamilyBackend)
		SdlFlags |= SDL_WINDOW_OPENGL;
	else if(m_BackendType == BACKEND_TYPE_VULKAN)
		SdlFlags |= SDL_WINDOW_VULKAN;
#if defined(CONF_BACKEND_WEBGPU) && defined(CONF_PLATFORM_MACOS)
	else if(m_BackendType == BACKEND_TYPE_WEBGPU)
		SdlFlags |= SDL_WINDOW_METAL;
#endif
	if(Flags & IGraphicsBackend::INITFLAG_RESIZABLE)
		SdlFlags |= SDL_WINDOW_RESIZABLE;
	if(Flags & IGraphicsBackend::INITFLAG_BORDERLESS)
		SdlFlags |= SDL_WINDOW_BORDERLESS;
	if(Flags & IGraphicsBackend::INITFLAG_FULLSCREEN)
		SdlFlags |= SDL_WINDOW_FULLSCREEN;
	else if(Flags & (IGraphicsBackend::INITFLAG_DESKTOP_FULLSCREEN))
		SdlFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	bool IsFullscreen = (SdlFlags & SDL_WINDOW_FULLSCREEN) != 0 || g_Config.m_GfxFullscreen == 3;
	// use desktop resolution as default resolution, clamp resolution if users's display is smaller than we remembered
	// if the user starts in fullscreen, and the resolution was not found use the desktop one
	if((IsFullscreen && !SupportedResolution) || *pWidth == 0 || *pHeight == 0 || (IsDesktopChanged && (!SupportedResolution || !IsFullscreen) && (*pWidth > *pDesktopWidth || *pHeight > *pDesktopHeight)))
	{
		*pWidth = *pDesktopWidth;
		*pHeight = *pDesktopHeight;
		*pRefreshRate = DisplayMode.refresh_rate;
	}

	// if in fullscreen and refresh rate wasn't set yet, just use the one from the found list
	if(*pRefreshRate == 0 && SupportedResolution)
	{
		*pRefreshRate = aModes[IndexOfResolution].m_RefreshRate;
	}
	else if(*pRefreshRate == 0)
	{
		*pRefreshRate = DisplayMode.refresh_rate;
	}

	// set gl attributes
	if(IsOpenGLFamilyBackend)
	{
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		if(*pFsaaSamples)
		{
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, *pFsaaSamples);
		}
		else
		{
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}
	}

	m_pWindow = SDL_CreateWindow(
		pName,
		SDL_WINDOWPOS_CENTERED_DISPLAY(*pScreen),
		SDL_WINDOWPOS_CENTERED_DISPLAY(*pScreen),
		*pWidth,
		*pHeight,
		SdlFlags);

	// set caption
	if(m_pWindow == nullptr)
	{
		log_error("gfx", "Unable to create window: %s", SDL_GetError());
		if(m_BackendType == BACKEND_TYPE_VULKAN)
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
		else
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_SDL_WINDOW_CREATE_FAILED;
	}

#if defined(CONF_BACKEND_WEBGPU)
	if(m_BackendType == BACKEND_TYPE_WEBGPU)
	{
		bool HasNativeWindow;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		m_WebGpuNativeWindow.m_Type = SWebGpuNativeWindow::EType::CANVAS;
		HasNativeWindow = true;
#elif defined(CONF_PLATFORM_MACOS)
		m_pWebGpuMetalView = SDL_Metal_CreateView(m_pWindow);
		m_WebGpuNativeWindow.m_Type = SWebGpuNativeWindow::EType::METAL;
		m_WebGpuNativeWindow.m_pWindow = m_pWebGpuMetalView != nullptr ? SDL_Metal_GetLayer(m_pWebGpuMetalView) : nullptr;
		HasNativeWindow = m_WebGpuNativeWindow.m_pWindow != nullptr;
#else
		HasNativeWindow = GetWebGpuNativeWindow(m_pWindow, m_WebGpuNativeWindow);
#endif
		if(!HasNativeWindow)
		{
			log_error("gfx/webgpu", "Unable to obtain the native SDL window: %s", SDL_GetError());
			DestroyWindow();
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
		}
	}
#endif

	int GlewMajor = 0;
	int GlewMinor = 0;
	int GlewPatch = 0;

	if(IsOpenGLFamilyBackend)
	{
		m_GLContext = SDL_GL_CreateContext(m_pWindow);

		if(m_GLContext == nullptr)
		{
			log_error("gfx", "Unable to create graphics context: %s", SDL_GetError());
			DestroyWindow();
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
		}

		if(!BackendInitGlew(m_BackendType, GlewMajor, GlewMinor, GlewPatch))
		{
			SDL_GL_DeleteContext(m_GLContext);
			DestroyWindow();
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_INTERFACE_INIT_FAILED;
		}
	}

	int InitError = IsVersionSupportedGlew(m_BackendType, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch, GlewMajor, GlewMinor, GlewPatch);

	// SDL_GL_GetDrawableSize reports HiDPI resolution even with SDL_WINDOW_ALLOW_HIGHDPI not set, which is wrong
	if(SdlFlags & SDL_WINDOW_ALLOW_HIGHDPI)
	{
		GetDrawableSize(m_pWindow, m_BackendType, pCurrentWidth, pCurrentHeight);
	}
	else
	{
		SDL_GetWindowSize(m_pWindow, pCurrentWidth, pCurrentHeight);
	}
	SDL_GetWindowSize(m_pWindow, pWidth, pHeight);

	if(IsOpenGLFamilyBackend)
	{
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
		// SDL_GL_SetSwapInterval is not supported with Emscripten as this is only a wrapper for the
		// emscripten_set_main_loop_timing function which does not work because we do not use the
		// emscripten_set_main_loop function before.
		SDL_GL_SetSwapInterval(Flags & IGraphicsBackend::INITFLAG_VSYNC ? 1 : 0);
#endif
		SDL_GL_MakeCurrent(nullptr, nullptr);
	}

	if(InitError != 0)
	{
		if(m_GLContext)
			SDL_GL_DeleteContext(m_GLContext);
		DestroyWindow();

		// try setting to glew supported version
		g_Config.m_GfxGLMajor = GlewMajor;
		g_Config.m_GfxGLMinor = GlewMinor;
		g_Config.m_GfxGLPatch = GlewPatch;

		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED;
	}
#endif // !CONF_HEADLESS_CLIENT

	// start the command processor
	dbg_assert(m_pProcessor == nullptr, "Processor was not cleaned up properly.");
	const EWebGpuBackendType WebGpuBackendType = m_BackendType == BACKEND_TYPE_WEBGPU ? WebGpuBackendTypeFromConfig() : EWebGpuBackendType::AUTO;
	m_pProcessor = new CCommandProcessor_SDL(m_BackendType, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, m_WebGpuNativeWindow, WebGpuBackendType);
	StartProcessor(m_pProcessor);

	// issue init commands for OpenGL and SDL
	CCommandBuffer CmdBuffer(1024, 512);
	CCommandProcessorFragment_Renderer::SCommand_PreInit CmdPre;
	CmdPre.m_pWindow = m_pWindow;
	CmdPre.m_Width = *pCurrentWidth;
	CmdPre.m_Height = *pCurrentHeight;
	CmdPre.m_pVendorString = m_aVendorString;
	CmdPre.m_pVersionString = m_aVersionString;
	CmdPre.m_pRendererString = m_aRendererString;
	CmdPre.m_pGpuList = &m_GpuList;
	CmdBuffer.AddCommandUnsafe(CmdPre);
	RunBufferSingleThreadedUnsafe(&CmdBuffer);
	CmdBuffer.Reset();

	// run sdl first to have the context in the thread
	CCommandProcessor_SDL::SCommand_Init CmdSDL;
	CmdSDL.m_pWindow = m_pWindow;
	CmdSDL.m_GLContext = m_GLContext;
	CmdBuffer.AddCommandUnsafe(CmdSDL);
	RunBuffer(&CmdBuffer);
	WaitForIdle();
	CmdBuffer.Reset();

	const char *pErrorStr = nullptr;
	m_Capabilities = {};
	if(InitError == 0)
	{
		CCommandProcessorFragment_Renderer::SCommand_Init CmdGL;
		CmdGL.m_pWindow = m_pWindow;
		CmdGL.m_Width = *pCurrentWidth;
		CmdGL.m_Height = *pCurrentHeight;
		CmdGL.m_pTextureMemoryUsage = &m_TextureMemoryUsage;
		CmdGL.m_pBufferMemoryUsage = &m_BufferMemoryUsage;
		CmdGL.m_pStreamMemoryUsage = &m_StreamMemoryUsage;
		CmdGL.m_pStagingMemoryUsage = &m_StagingMemoryUsage;
		CmdGL.m_pGpuList = &m_GpuList;
		CmdGL.m_pStorage = pStorage;
		CmdGL.m_pCapabilities = &m_Capabilities;
		CmdGL.m_pInitError = &InitError;
		CmdGL.m_RequestedMajor = g_Config.m_GfxGLMajor;
		CmdGL.m_RequestedMinor = g_Config.m_GfxGLMinor;
		CmdGL.m_RequestedPatch = g_Config.m_GfxGLPatch;
		CmdGL.m_VSync = (Flags & IGraphicsBackend::INITFLAG_VSYNC) != 0;
		CmdGL.m_RequestedMultiSamplingCount = *pFsaaSamples;
		CmdGL.m_GlewMajor = GlewMajor;
		CmdGL.m_GlewMinor = GlewMinor;
		CmdGL.m_GlewPatch = GlewPatch;
		CmdGL.m_pErrStringPtr = &pErrorStr;
		CmdGL.m_pVendorString = m_aVendorString;
		CmdGL.m_pVersionString = m_aVersionString;
		CmdGL.m_pRendererString = m_aRendererString;
		CmdGL.m_RequestedBackend = m_BackendType;
		CmdBuffer.AddCommandUnsafe(CmdGL);

		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();
	}

	if(InitError != 0)
	{
		if(InitError != -2)
		{
			// shutdown the context, as it might have been initialized
			CCommandProcessorFragment_Renderer::SCommand_Shutdown CmdGL;
			CmdBuffer.AddCommandUnsafe(CmdGL);
			RunBuffer(&CmdBuffer);
			WaitForIdle();
			CmdBuffer.Reset();
		}

		CCommandProcessor_SDL::SCommand_Shutdown Cmd;
		CmdBuffer.AddCommandUnsafe(Cmd);
		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();

		CCommandProcessorFragment_Renderer::SCommand_PostShutdown CmdPost;
		CmdBuffer.AddCommandUnsafe(CmdPost);
		RunBufferSingleThreadedUnsafe(&CmdBuffer);
		CmdBuffer.Reset();

		// stop and delete the processor
		StopProcessor();
		delete m_pProcessor;
		m_pProcessor = nullptr;

		if(m_GLContext)
			SDL_GL_DeleteContext(m_GLContext);
		DestroyWindow();

		// try setting to version string's supported version
		if(InitError == -2)
		{
			g_Config.m_GfxGLMajor = m_Capabilities.m_ContextMajor;
			g_Config.m_GfxGLMinor = m_Capabilities.m_ContextMinor;
			g_Config.m_GfxGLPatch = m_Capabilities.m_ContextPatch;
		}

		if(pErrorStr != nullptr)
		{
			str_copy(m_aErrorString, pErrorStr);
		}

		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED;
	}

	{
		CCommandBuffer::SCommand_Update_Viewport CmdSDL2;
		CmdSDL2.m_X = 0;
		CmdSDL2.m_Y = 0;
		CmdSDL2.m_Width = *pCurrentWidth;
		CmdSDL2.m_Height = *pCurrentHeight;
		CmdSDL2.m_SurfaceWidth = *pCurrentWidth;
		CmdSDL2.m_SurfaceHeight = *pCurrentHeight;
		CmdSDL2.m_ByResize = true;
		CmdBuffer.AddCommandUnsafe(CmdSDL2);
		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();
	}

	return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_NONE;
}

int CGraphicsBackend_SDL::Shutdown()
{
	if(m_pProcessor != nullptr)
	{
		// issue a shutdown command
		CCommandBuffer CmdBuffer(1024, 512);
		CCommandProcessorFragment_Renderer::SCommand_Shutdown CmdGL;
		CmdBuffer.AddCommandUnsafe(CmdGL);
		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();

		CCommandProcessor_SDL::SCommand_Shutdown Cmd;
		CmdBuffer.AddCommandUnsafe(Cmd);
		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();

		CCommandProcessorFragment_Renderer::SCommand_PostShutdown CmdPost;
		CmdBuffer.AddCommandUnsafe(CmdPost);
		RunBufferSingleThreadedUnsafe(&CmdBuffer);
		CmdBuffer.Reset();

		// stop and delete the processor
		StopProcessor();
		delete m_pProcessor;
		m_pProcessor = nullptr;
	}

	if(m_GLContext != nullptr)
		SDL_GL_DeleteContext(m_GLContext);
	DestroyWindow();

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return 0;
}

uint64_t CGraphicsBackend_SDL::TextureMemoryUsage() const
{
	return m_TextureMemoryUsage;
}

uint64_t CGraphicsBackend_SDL::BufferMemoryUsage() const
{
	return m_BufferMemoryUsage;
}

uint64_t CGraphicsBackend_SDL::StreamedMemoryUsage() const
{
	return m_StreamMemoryUsage;
}

uint64_t CGraphicsBackend_SDL::StagingMemoryUsage() const
{
	return m_StagingMemoryUsage;
}

const TTwGraphicsGpuList &CGraphicsBackend_SDL::GetGpus() const
{
	return m_GpuList;
}

void CGraphicsBackend_SDL::Minimize()
{
	SDL_MinimizeWindow(m_pWindow);
}

void CGraphicsBackend_SDL::SetWindowParams(int FullscreenMode, bool IsBorderless)
{
	// The flags have to be kept consistent with flags set in the CGraphics_Threaded::IssueInit function!

	if(FullscreenMode > 0)
	{
		bool IsDesktopFullscreen = FullscreenMode == 2;
#ifndef CONF_FAMILY_WINDOWS
		//  Windowed fullscreen is only available on Windows, use desktop fullscreen on other platforms
		IsDesktopFullscreen |= FullscreenMode == 3;
#endif
		if(FullscreenMode == 1)
		{
#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_HAIKU)
			// Todo SDL: remove this when fixed (game freezes when losing focus in fullscreen)
			SDL_SetWindowFullscreen(m_pWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
#else
			SDL_SetWindowFullscreen(m_pWindow, SDL_WINDOW_FULLSCREEN);
#endif
			SDL_SetWindowResizable(m_pWindow, SDL_FALSE);
		}
		else if(IsDesktopFullscreen)
		{
			SDL_SetWindowFullscreen(m_pWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
			SDL_SetWindowResizable(m_pWindow, SDL_FALSE);
		}
		else // Windowed fullscreen
		{
			SDL_SetWindowFullscreen(m_pWindow, 0);
			SDL_SetWindowBordered(m_pWindow, SDL_TRUE);
			SDL_SetWindowResizable(m_pWindow, SDL_FALSE);
			SDL_DisplayMode DpMode;
			if(SDL_GetDesktopDisplayMode(g_Config.m_GfxScreen, &DpMode) < 0)
			{
				log_error("gfx", "Unable to get desktop display mode of screen %d: %s", g_Config.m_GfxScreen, SDL_GetError());
			}
			else
			{
				ResizeWindow(DpMode.w, DpMode.h, DpMode.refresh_rate);
				SDL_SetWindowPosition(m_pWindow, SDL_WINDOWPOS_CENTERED_DISPLAY(g_Config.m_GfxScreen), SDL_WINDOWPOS_CENTERED_DISPLAY(g_Config.m_GfxScreen));
			}
		}
	}
	else // Windowed
	{
		SDL_SetWindowFullscreen(m_pWindow, 0);
		SDL_SetWindowBordered(m_pWindow, SDL_bool(!IsBorderless));
		SDL_SetWindowResizable(m_pWindow, SDL_TRUE);
	}
}

bool CGraphicsBackend_SDL::SetWindowScreen(int Index, bool MoveToCenter, ivec2 *pDesktopSize)
{
	if(Index < 0 || Index >= m_NumScreens)
	{
		log_error("gfx", "Invalid screen number: %d (min: 0, max: %d)", Index, m_NumScreens);
		return false;
	}

	SDL_Rect ScreenPos;
	if(SDL_GetDisplayBounds(Index, &ScreenPos) != 0)
	{
		log_error("gfx", "Unable to get bounds of screen %d: %s", Index, SDL_GetError());
		return false;
	}

	if(MoveToCenter)
	{
		SDL_SetWindowPosition(m_pWindow,
			SDL_WINDOWPOS_CENTERED_DISPLAY(Index),
			SDL_WINDOWPOS_CENTERED_DISPLAY(Index));
	}
	else
	{
		SDL_SetWindowPosition(m_pWindow,
			SDL_WINDOWPOS_UNDEFINED_DISPLAY(Index),
			SDL_WINDOWPOS_UNDEFINED_DISPLAY(Index));
	}

	return UpdateDisplayMode(Index, pDesktopSize);
}

bool CGraphicsBackend_SDL::UpdateDisplayMode(int Index, ivec2 *pDesktopSize)
{
	SDL_DisplayMode DisplayMode;
	if(SDL_GetDesktopDisplayMode(Index, &DisplayMode) < 0)
	{
		log_error("gfx", "Unable to get desktop display mode of screen %d: %s", Index, SDL_GetError());
		return false;
	}

	g_Config.m_GfxScreen = Index;
	pDesktopSize->x = DisplayMode.w;
	pDesktopSize->y = DisplayMode.h;
	return true;
}

int CGraphicsBackend_SDL::GetWindowScreen()
{
	return SDL_GetWindowDisplayIndex(m_pWindow);
}

int CGraphicsBackend_SDL::WindowActive()
{
	return m_pWindow && SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_INPUT_FOCUS;
}

int CGraphicsBackend_SDL::WindowOpen()
{
	return m_pWindow && SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_SHOWN;
}

void CGraphicsBackend_SDL::SetWindowGrab(bool Grab)
{
	// Works around https://github.com/libsdl-org/sdl2-compat/issues/578.
	if(!m_pWindow)
		return;

	SDL_SetWindowGrab(m_pWindow, Grab ? SDL_TRUE : SDL_FALSE);
}

bool CGraphicsBackend_SDL::ResizeWindow(int w, int h, int RefreshRate)
{
	// don't call resize events when the window is at fullscreen desktop
	if(!m_pWindow || (SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
		return false;

	// if the window is at fullscreen use SDL_SetWindowDisplayMode instead, suggested by SDL
	if(SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_FULLSCREEN)
	{
#ifdef CONF_FAMILY_WINDOWS
		// in windows make the window windowed mode first, this prevents strange window glitches (other games probably do something similar)
		SetWindowParams(0, true);
#endif
		SDL_DisplayMode SetMode = {};
		SDL_DisplayMode ClosestMode = {};
		SetMode.format = 0;
		SetMode.w = w;
		SetMode.h = h;
		SetMode.refresh_rate = RefreshRate;
		SDL_SetWindowDisplayMode(m_pWindow, SDL_GetClosestDisplayMode(g_Config.m_GfxScreen, &SetMode, &ClosestMode));
#ifdef CONF_FAMILY_WINDOWS
		// now change it back to fullscreen, this will restore the above set state, bcs SDL saves fullscreen modes apart from other video modes (as of SDL 2.0.16)
		// see implementation of SDL_SetWindowDisplayMode
		SetWindowParams(1, false);
#endif
		return true;
	}
	else
	{
		SDL_SetWindowSize(m_pWindow, w, h);
		if(SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_MAXIMIZED)
			// remove maximize flag
			SDL_RestoreWindow(m_pWindow);
	}

	return false;
}

void CGraphicsBackend_SDL::GetViewportSize(int &w, int &h)
{
	GetDrawableSize(m_pWindow, m_BackendType, &w, &h);
}

void CGraphicsBackend_SDL::NotifyWindow()
{
	// Minimum version 2.0.16, after version 2.0.22 the naming is changed to 2.24.0 etc.
#if SDL_MAJOR_VERSION > 2 || (SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION == 0 && SDL_PATCHLEVEL >= 16) || (SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION > 0)
	if(SDL_FlashWindow(m_pWindow, SDL_FlashOperation::SDL_FLASH_UNTIL_FOCUSED) != 0)
	{
		// fails if SDL hasn't implemented it
		return;
	}
#endif
}

bool CGraphicsBackend_SDL::IsScreenKeyboardShown()
{
	return SDL_IsScreenKeyboardShown(m_pWindow);
}

void CGraphicsBackend_SDL::WindowDestroyNtf(uint32_t WindowId)
{
}

void CGraphicsBackend_SDL::WindowCreateNtf(uint32_t WindowId)
{
	m_pWindow = SDL_GetWindowFromID(WindowId);
}

IGraphicsBackend *CreateGraphicsBackend(EBackendType BackendOverride) { return new CGraphicsBackend_SDL(BackendOverride); }
