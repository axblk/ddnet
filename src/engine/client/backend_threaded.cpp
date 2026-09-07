#include "backend_threaded.h"

#include <base/log.h>
#include <base/str.h>
#include <base/thread.h>

#include <engine/client/backend/null/backend_null.h>
#include <engine/client/presentation_surface.h>
#include <engine/shared/config.h>

// CONF_BACKEND_NO_OPENGL leaves out the OpenGL backends, and with them GLEW
// and the GL library, for the windowless programs.
#if !defined(CONF_BACKEND_NO_OPENGL)
#if !defined(CONF_BACKEND_OPENGL_ES)
#include <engine/client/backend/opengl/backend_opengl.h>
#include <engine/client/backend/opengl/backend_opengl3.h>
#endif
#if defined(CONF_BACKEND_OPENGL_ES3) || defined(CONF_BACKEND_OPENGL_ES)
#include <engine/client/backend/opengles/backend_opengles3.h>
#endif
#endif
#if defined(CONF_BACKEND_VULKAN)
#include <engine/client/backend/vulkan/backend_vulkan.h>
#endif
#if defined(CONF_BACKEND_WEBGPU)
#include <engine/client/backend/webgpu/backend_webgpu.h>
#endif

#include <cstdlib>
#include <string>
#include <utility>

#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_IOS)
#include <objc/message.h>
#include <objc/runtime.h>

class CAutoreleasePool
{
private:
	id m_Pool;

public:
	CAutoreleasePool()
	{
		Class NSAutoreleasePoolClass = (Class)objc_getClass("NSAutoreleasePool");
		m_Pool = class_createInstance(NSAutoreleasePoolClass, 0);
		SEL Selector = sel_registerName("init");
		((id (*)(id, SEL))objc_msgSend)(m_Pool, Selector);
	}

	~CAutoreleasePool()
	{
		SEL Selector = sel_registerName("drain");
		((id (*)(id, SEL))objc_msgSend)(m_Pool, Selector);
	}
};
#endif

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
		const bool CanProcess = pSelf->ProcessorError().m_ErrorType == GFX_ERROR_TYPE_NONE;
#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_IOS)
		CAutoreleasePool AutoreleasePool;
#endif
		CCommandBuffer *pBuffer = QueuedBuffer.m_pBuffer;
		if(CanProcess)
			pSelf->ProcessBuffer(pBuffer);
		pBuffer->SignalCompletions();

		if(CanProcess && pSelf->m_RenderThreadError.m_ErrorType != GFX_ERROR_TYPE_NONE)
		{
			std::unique_lock Lock(pSelf->m_ProcessorErrorMutex);
			if(pSelf->m_ProcessorError.m_ErrorType == GFX_ERROR_TYPE_NONE)
				pSelf->m_ProcessorError = pSelf->m_RenderThreadError;
		}
		pSelf->m_CommandQueue.Recycle(std::move(QueuedBuffer), CanProcess);
	}
}
#endif

CGraphicsBackend_Threaded::CGraphicsBackend_Threaded(EBackendType BackendType, int GlMajor, int GlMinor) :
	m_BackendType(BackendType),
	m_GlMajor(GlMajor),
	m_GlMinor(GlMinor)
{
}

SGfxErrorContainer CGraphicsBackend_Threaded::ProcessorError() const
{
	std::unique_lock Lock(m_ProcessorErrorMutex);
	return m_ProcessorError;
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

void CGraphicsBackend_Threaded::StartProcessor()
{
	dbg_assert(m_CommandQueue.IsStopped(), "Processor was already not shut down.");
	{
		std::unique_lock Lock(m_ProcessorErrorMutex);
		m_ProcessorError = {};
	}
	m_RenderThreadError = {};
	m_RenderThreadWarning = {};
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
	m_Warning = m_RenderThreadWarning;
	m_CommandQueue.Stop();
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	thread_wait(m_pThread);
#endif
}

void CGraphicsBackend_Threaded::RunBuffer(CCommandBuffer *pBuffer)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	const SGfxErrorContainer Error = m_RenderThreadError;
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
	{
		ProcessBuffer(pBuffer);
		pBuffer->SignalCompletions();
		pBuffer->Reset();
		return;
	}
#else
	WaitForIdle();
	const SGfxErrorContainer Error = ProcessorError();
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
	{
		const bool Queued = m_CommandQueue.EnqueueBorrowed(pBuffer);
		dbg_assert(Queued, "graphics: borrowed command buffer published while queue stopped");
		WaitForIdle();
		pBuffer->Reset();
		return;
	}
#endif
	pBuffer->SignalCompletions();
	ProcessError(Error);
}

bool CGraphicsBackend_Threaded::RunBufferQueued(CCommandBuffer *pBuffer)
{
	dbg_assert(pBuffer->SubmissionInfo().m_Channel == CCommandBuffer::ECommandChannel::RELIABLE, "graphics: reliable publish received a frame packet");
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	RunBuffer(pBuffer);
	return true;
#else
	const SGfxErrorContainer Error = ProcessorError();
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
		return m_CommandQueue.WaitEnqueueReliable(pBuffer);

	pBuffer->SignalCompletions();
	pBuffer->FreeExternalData();
	pBuffer->Reset();
	ProcessError(Error);
#endif
}

bool CGraphicsBackend_Threaded::RunFramePacket(CCommandBuffer *pBuffer, bool WaitForCapacity)
{
	dbg_assert(pBuffer->SubmissionInfo().m_Channel == CCommandBuffer::ECommandChannel::FRAME, "graphics: frame publish received a reliable buffer");
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	(void)WaitForCapacity;
	RunBuffer(pBuffer);
	m_CommandQueue.RecordSynchronousFrame(true);
	return true;
#else
	const SGfxErrorContainer Error = ProcessorError();
	if(Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
	{
		if(WaitForCapacity)
			return m_CommandQueue.WaitEnqueuePinnedFrame(pBuffer);
		return m_CommandQueue.EnqueueFrame(pBuffer) != CRenderCommandQueue::EFrameEnqueueResult::RETRY;
	}

	m_CommandQueue.DiscardFrame(pBuffer);
	ProcessError(Error);
#endif
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

bool CGraphicsBackend_Threaded::RunPlatformCommand(const CCommandBuffer::SCommand *pCommand)
{
	// The surface's OpenGL calls do nothing where it has no context.
	switch(pCommand->m_Cmd)
	{
	case CMD_BIND_SURFACE:
		if(m_pSurface != nullptr)
			m_pSurface->BindGlContext();
		return true;
	case CMD_UNBIND_SURFACE:
		if(m_pSurface != nullptr)
			m_pSurface->UnbindGlContext();
		return true;
	case CCommandBuffer::CMD_SWAP:
		if(m_pSurface != nullptr)
			m_pSurface->SwapGlBuffers();
		return true;
	case CCommandBuffer::CMD_VSYNC:
		if(m_pSurface != nullptr)
			static_cast<const CCommandBuffer::SCommand_VSync *>(pCommand)->m_pResult->m_Ok = m_pSurface->SetGlSwapInterval(static_cast<const CCommandBuffer::SCommand_VSync *>(pCommand)->m_VSync);
		return true;
	case CCommandBuffer::CMD_MULTISAMPLING:
		// An OpenGL context has its sample count from creation on.
		return true;
	case CCommandBuffer::CMD_WINDOW_CREATE_NTF:
		// Android destroys the window while the app is away and hands out a
		// new one; the context survives and only has to be bound to it.
#if defined(CONF_PLATFORM_ANDROID)
		if(m_pSurface != nullptr)
			m_pSurface->BindGlContext();
#endif
		return true;
	case CCommandBuffer::CMD_WINDOW_DESTROY_NTF:
#if defined(CONF_PLATFORM_ANDROID)
		if(m_pSurface != nullptr)
			m_pSurface->UnbindGlContext();
#endif
		return true;
	case CCommandProcessorFragment_Renderer::CMD_PRE_INIT:
	case CCommandProcessorFragment_Renderer::CMD_POST_SHUTDOWN:
		return true;
	default:
		return false;
	}
}

void CGraphicsBackend_Threaded::ProcessBuffer(CCommandBuffer *pBuffer)
{
	for(CCommandBuffer::SCommand *pCommand = pBuffer->Head(); pCommand; pCommand = pCommand->m_pNext)
	{
		if(pCommand->m_Cmd == CCommandBuffer::CMD_SIGNAL)
		{
			static_cast<const CCommandBuffer::SCommand_Signal *>(pCommand)->Signal();
			continue;
		}

		const ERunCommandReturnTypes Result = m_pRenderer->RunCommand(pCommand);
		if(Result == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED)
		{
			CCommandBuffer::FreeExternalData(pCommand);
			continue;
		}
		if(Result == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_ERROR)
		{
			m_RenderThreadError = m_pRenderer->GetError();
			pBuffer->FreeExternalDataFrom(pCommand);
			return;
		}
		if(Result == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_WARNING)
		{
			if(m_pRenderer->GetError().m_ErrorType != GFX_ERROR_TYPE_NONE)
				m_RenderThreadError = m_pRenderer->GetError();
			else
				m_RenderThreadWarning = m_pRenderer->GetWarning();
			pBuffer->FreeExternalDataFrom(pCommand);
			return;
		}

		if(RunPlatformCommand(pCommand))
		{
			CCommandBuffer::FreeExternalData(pCommand);
			continue;
		}

		pBuffer->FreeExternalDataFrom(pCommand);
		dbg_assert_failed("Unknown graphics command %d", pCommand->m_Cmd);
	}

	if(m_pRenderer->GetError().m_ErrorType != GFX_ERROR_TYPE_NONE)
		m_RenderThreadError = m_pRenderer->GetError();
}

CCommandProcessorFragment_Renderer *CGraphicsBackend_Threaded::CreateRenderer() const
{
	switch(m_BackendType)
	{
	case BACKEND_TYPE_NULL:
		return new CCommandProcessorFragment_Null();
	case BACKEND_TYPE_OPENGL_ES:
#if (defined(CONF_BACKEND_OPENGL_ES) || defined(CONF_BACKEND_OPENGL_ES3)) && !defined(CONF_BACKEND_NO_OPENGL)
		// The window forces GLES 3.0 wherever ES is offered.
		return new CCommandProcessorFragment_OpenGLES3();
#else
		return nullptr;
#endif
	case BACKEND_TYPE_OPENGL:
#if !defined(CONF_BACKEND_OPENGL_ES) && !defined(CONF_BACKEND_NO_OPENGL)
		// The backend with programs needs OpenGL 3.3 core.
		if(m_GlMajor < 3 || (m_GlMajor == 3 && m_GlMinor < 3))
			return new CCommandProcessorFragment_OpenGL();
		return new CCommandProcessorFragment_OpenGL3_3();
#else
		// Read here too, so that the fields count as used without OpenGL.
		(void)m_GlMajor;
		(void)m_GlMinor;
		return nullptr;
#endif
	case BACKEND_TYPE_VULKAN:
#if defined(CONF_BACKEND_VULKAN)
		return CreateVulkanCommandProcessorFragment();
#else
		return nullptr;
#endif
	case BACKEND_TYPE_WEBGPU:
#if defined(CONF_BACKEND_WEBGPU)
		return CreateWebGpuCommandProcessorFragment(WebGpuBackendTypeFromConfig());
#else
		return nullptr;
#endif
	default:
		return nullptr;
	}
}

void CGraphicsBackend_Threaded::StopAndDeleteProcessor(bool RendererInitialized)
{
	CCommandBuffer CmdBuffer(1024, 512);
	if(RendererInitialized)
	{
		// Tear down the device's children before the post shutdown destroys it.
		CCommandProcessorFragment_Renderer::SCommand_Shutdown CmdShutdown;
		CmdBuffer.AddCommandUnsafe(CmdShutdown);
		RunBuffer(&CmdBuffer);
	}
	SCommand_UnbindSurface CmdUnbind;
	CmdBuffer.AddCommandUnsafe(CmdUnbind);
	RunBuffer(&CmdBuffer);

	CCommandProcessorFragment_Renderer::SCommand_PostShutdown CmdPost;
	CmdBuffer.AddCommandUnsafe(CmdPost);
	ProcessBuffer(&CmdBuffer);
	CmdBuffer.Reset();

	StopProcessor();
	delete m_pRenderer;
	m_pRenderer = nullptr;
	m_pSurface = nullptr;
}

int CGraphicsBackend_Threaded::Init(const SGraphicsBackendInit &Params)
{
	dbg_assert(m_pRenderer == nullptr, "Processor was not cleaned up properly.");
	m_aErrorString[0] = '\0';
	m_Capabilities = {};

	m_pRenderer = CreateRenderer();
	if(m_pRenderer == nullptr)
	{
		log_error("gfx", "The requested graphics backend is not available in this build.");
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
	}
	m_pSurface = Params.m_pSurface;
	StartProcessor();

	const CCommandProcessorFragment_Renderer::SPresentationSurface Surface = {Params.m_pSurface, static_cast<uint32_t>(std::max(Params.m_Width, 1)), static_cast<uint32_t>(std::max(Params.m_Height, 1))};

	CCommandBuffer CmdBuffer(1024, 512);
	CCommandProcessorFragment_Renderer::SCommand_PreInit CmdPre;
	CmdPre.m_Surface = Surface;
	CmdPre.m_pVendorString = m_aVendorString;
	CmdPre.m_pVersionString = m_aVersionString;
	CmdPre.m_pRendererString = m_aRendererString;
	CmdPre.m_pGpuList = &m_GpuList;
	CmdBuffer.AddCommandUnsafe(CmdPre);
	ProcessBuffer(&CmdBuffer);
	CmdBuffer.Reset();

	// The OpenGL context has to be bound to the render thread before the
	// renderer touches it.
	SCommand_BindSurface CmdBind;
	CmdBuffer.AddCommandUnsafe(CmdBind);
	RunBuffer(&CmdBuffer);

	int InitError = 0;
	const char *pErrorString = nullptr;
	CCommandProcessorFragment_Renderer::SCommand_Init CmdInit;
	CmdInit.m_Surface = Surface;
	CmdInit.m_pTextureMemoryUsage = &m_TextureMemoryUsage;
	CmdInit.m_pBufferMemoryUsage = &m_BufferMemoryUsage;
	CmdInit.m_pStreamMemoryUsage = &m_StreamMemoryUsage;
	CmdInit.m_pStagingMemoryUsage = &m_StagingMemoryUsage;
	CmdInit.m_pGpuTiming = &m_GpuTiming;
	CmdInit.m_pGpuList = &m_GpuList;
	CmdInit.m_pStorage = Params.m_pStorage;
	CmdInit.m_pCapabilities = &m_Capabilities;
	CmdInit.m_pInitError = &InitError;
	CmdInit.m_RequestedMajor = Params.m_RequestedMajor;
	CmdInit.m_RequestedMinor = Params.m_RequestedMinor;
	CmdInit.m_RequestedPatch = Params.m_RequestedPatch;
	CmdInit.m_VSync = Params.m_VSync;
	CmdInit.m_RequestedMultiSamplingCount = Params.m_FsaaSamples;
	CmdInit.m_GlewMajor = Params.m_GlewMajor;
	CmdInit.m_GlewMinor = Params.m_GlewMinor;
	CmdInit.m_GlewPatch = Params.m_GlewPatch;
	CmdInit.m_pErrStringPtr = &pErrorString;
	CmdInit.m_pVendorString = m_aVendorString;
	CmdInit.m_pVersionString = m_aVersionString;
	CmdInit.m_pRendererString = m_aRendererString;
	CmdInit.m_RequestedBackend = m_BackendType;
	CmdBuffer.AddCommandUnsafe(CmdInit);
	RunBuffer(&CmdBuffer);

	if(InitError != 0)
	{
		StopAndDeleteProcessor(InitError != -2);

		// The renderer says which version the context really has; the window
		// tries again with that one.
		if(InitError == -2)
		{
			g_Config.m_GfxGLMajor = m_Capabilities.m_ContextMajor;
			g_Config.m_GfxGLMinor = m_Capabilities.m_ContextMinor;
			g_Config.m_GfxGLPatch = m_Capabilities.m_ContextPatch;
		}
		if(pErrorString != nullptr)
			str_copy(m_aErrorString, pErrorString);
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED;
	}

	if(Surface.IsPresentable())
	{
		CCommandBuffer::SCommand_Update_Viewport CmdViewport;
		CmdViewport.m_X = 0;
		CmdViewport.m_Y = 0;
		CmdViewport.m_Width = Surface.m_Width;
		CmdViewport.m_Height = Surface.m_Height;
		CmdViewport.m_SurfaceWidth = Surface.m_Width;
		CmdViewport.m_SurfaceHeight = Surface.m_Height;
		CmdViewport.m_ByResize = true;
		CmdBuffer.AddCommandUnsafe(CmdViewport);
		RunBuffer(&CmdBuffer);
	}

	return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_NONE;
}

int CGraphicsBackend_Threaded::Shutdown()
{
	if(m_pRenderer != nullptr)
		StopAndDeleteProcessor(true);
	return 0;
}

void CGraphicsBackend_Threaded::ErroneousCleanup()
{
	if(m_pRenderer != nullptr)
		m_pRenderer->ErroneousCleanup();
}

// ------------ free functions

bool IsModernGraphicsApi(EBackendType BackendType)
{
	switch(BackendType)
	{
	case BACKEND_TYPE_OPENGL:
		return (g_Config.m_GfxGLMajor == 3 && g_Config.m_GfxGLMinor == 3) || g_Config.m_GfxGLMajor >= 4;
	case BACKEND_TYPE_OPENGL_ES: // clamped to 3.0 and always the program backend, see the window
	case BACKEND_TYPE_VULKAN:
	case BACKEND_TYPE_WEBGPU:
	case BACKEND_TYPE_NULL:
		return true;
	default:
		return false;
	}
}

EBackendType BackendTypeFromName(const char *pName)
{
	if(str_comp_nocase(pName, "OpenGL") == 0)
		return BACKEND_TYPE_OPENGL;
	if(str_comp_nocase(pName, "GLES") == 0)
		return BACKEND_TYPE_OPENGL_ES;
#if defined(CONF_BACKEND_VULKAN)
	if(str_comp_nocase(pName, "Vulkan") == 0)
		return BACKEND_TYPE_VULKAN;
#endif
#if defined(CONF_BACKEND_WEBGPU)
	if(str_comp_nocase(pName, "WebGPU") == 0)
		return BACKEND_TYPE_WEBGPU;
#endif
	if(str_comp_nocase(pName, "Null") == 0)
		return BACKEND_TYPE_NULL;
	return BACKEND_TYPE_AUTO;
}

const char *BackendTypeName(EBackendType BackendType)
{
	switch(BackendType)
	{
	case BACKEND_TYPE_OPENGL: return "OpenGL";
	case BACKEND_TYPE_OPENGL_ES: return "GLES";
	case BACKEND_TYPE_VULKAN: return "Vulkan";
	case BACKEND_TYPE_WEBGPU: return "WebGPU";
	case BACKEND_TYPE_NULL: return "Null";
	default: return "Auto";
	}
}

EBackendType GraphicsBackendOverrideFromEnvironment()
{
	const char *pBackend = std::getenv("GFX_BACKEND");
	if(pBackend == nullptr)
		pBackend = std::getenv("DDNET_DRIVER");
	if(pBackend == nullptr)
		return BACKEND_TYPE_AUTO;
	const EBackendType BackendType = BackendTypeFromName(pBackend);
	return BackendType == BACKEND_TYPE_AUTO ? BACKEND_TYPE_OPENGL : BackendType;
}

std::vector<IGraphics::SRendererChoice> GraphicsBackendRendererChoices()
{
	std::vector<IGraphics::SRendererChoice> vChoices;
#ifndef CONF_BACKEND_OPENGL_ES
	// Legacy first, then the versions a blocked driver is not offered.
	vChoices.push_back({"OpenGL", 1, 4, 0});
	if(g_Config.m_GfxDriverIsBlocked == 0)
	{
		vChoices.push_back({"OpenGL", 3, 0, 0});
		vChoices.push_back({"OpenGL", 3, 3, 0});
	}
#endif
#ifdef CONF_BACKEND_OPENGL_ES3
	vChoices.push_back({"GLES", 3, 0, 0});
#endif
#ifdef CONF_BACKEND_VULKAN
	vChoices.push_back({"Vulkan", BACKEND_VULKAN_VERSION_MAJOR, BACKEND_VULKAN_VERSION_MINOR, 0});
#endif
#if defined(CONF_BACKEND_WEBGPU) && defined(CONF_PLATFORM_EMSCRIPTEN)
	// The browser picks what WebGPU runs on.
	vChoices.push_back({"WebGPU", 1, 0, 0});
#elif defined(CONF_BACKEND_WEBGPU)
	// What wgpu-native can run on here. It runs on OpenGL everywhere except
	// macOS, where it only has Metal.
	static constexpr const char *s_apWebGpuDeviceApis[] = {
#if defined(CONF_FAMILY_WINDOWS)
		"D3D12",
#endif
#if defined(CONF_PLATFORM_MACOS)
		"Metal",
#else
		"Vulkan",
		"OpenGL",
#endif
	};
	vChoices.push_back({"WebGPU", 1, 0, 0, s_apWebGpuDeviceApis});
#endif
	return vChoices;
}

IGraphicsBackend *CreateGraphicsBackend(EBackendType BackendType, int GlMajor, int GlMinor)
{
	return new CGraphicsBackend_Threaded(BackendType, GlMajor, GlMinor);
}

IGraphicsBackend *CreateOffscreenGraphicsBackend(EBackendType BackendOverride)
{
	// Without an override gfx_backend decides. A backend that needs a surface
	// falls through to the first one that does not, with a warning.
	EBackendType BackendType = BackendOverride;
	if(BackendType == BACKEND_TYPE_AUTO)
	{
		BackendType = BackendTypeFromName(g_Config.m_GfxBackend);
		if(BackendType != BACKEND_TYPE_VULKAN && BackendType != BACKEND_TYPE_WEBGPU && BackendType != BACKEND_TYPE_NULL)
		{
#if defined(CONF_BACKEND_VULKAN)
			BackendType = BACKEND_TYPE_VULKAN;
#elif defined(CONF_BACKEND_WEBGPU)
			BackendType = BACKEND_TYPE_WEBGPU;
#else
			BackendType = BACKEND_TYPE_NULL;
#endif
			log_warn("gfx", "gfx_backend '%s' cannot draw without a surface, using %s", g_Config.m_GfxBackend, BackendTypeName(BackendType));
		}
	}
	switch(BackendType)
	{
	case BACKEND_TYPE_VULKAN:
	case BACKEND_TYPE_WEBGPU:
	case BACKEND_TYPE_NULL:
		return new CGraphicsBackend_Threaded(BackendType, 0, 0);
	default:
		return nullptr;
	}
}
