#include "backend_threaded.h"

#include <base/log.h>
#include <base/str.h>
#include <base/thread.h>

#include <engine/client/backend/null/backend_null.h>
#include <engine/client/presentation_surface.h>
#include <engine/shared/config.h>

#if !defined(CONF_BACKEND_OPENGL_ES)
#include <engine/client/backend/opengl/backend_opengl.h>
#include <engine/client/backend/opengl/backend_opengl3.h>
#endif
#if defined(CONF_BACKEND_OPENGL_ES3) || defined(CONF_BACKEND_OPENGL_ES)
#include <engine/client/backend/opengles/backend_opengles3.h>
#endif
#if defined(CONF_BACKEND_VULKAN)
#include <engine/client/backend/vulkan/backend_vulkan.h>
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
		bool CanProcess;
		{
			std::unique_lock Lock(pSelf->m_ProcessorErrorMutex);
			CanProcess = pSelf->m_ProcessorError.m_ErrorType == GFX_ERROR_TYPE_NONE;
		}
#if defined(CONF_PLATFORM_MACOS) || defined(CONF_PLATFORM_IOS)
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

	// Process error after lock is released to prevent deadlock.
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

SGpuTiming CGraphicsBackend_Threaded::GpuTiming() const
{
	return m_GpuTiming.Snapshot();
}

void CGraphicsBackend_Threaded::SetGpuTimingEnabled(bool Enabled)
{
	m_GpuTiming.m_Enabled.store(Enabled, std::memory_order_relaxed);
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

CCommandProcessor_Threaded::CCommandProcessor_Threaded(CCommandProcessorFragment_Renderer *pRendererBackend, IPresentationSurface *pSurface, bool GlContext) :
	m_pRendererBackend(pRendererBackend),
	m_pSurface(pSurface),
	m_GlContext(GlContext && pSurface != nullptr)
{
	dbg_assert(m_pRendererBackend != nullptr, "graphics command processor backend is unavailable");
}

CCommandProcessor_Threaded::~CCommandProcessor_Threaded()
{
	delete m_pRendererBackend;
}

bool CCommandProcessor_Threaded::RunPlatformCommand(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CMD_BIND_SURFACE:
		if(m_GlContext)
			m_pSurface->BindGlContext();
		return true;
	case CMD_UNBIND_SURFACE:
		if(m_GlContext)
			m_pSurface->UnbindGlContext();
		return true;
	case CCommandBuffer::CMD_SWAP:
		if(m_GlContext)
			m_pSurface->SwapGlBuffers();
		return true;
	case CCommandBuffer::CMD_VSYNC:
		if(m_GlContext)
			static_cast<const CCommandBuffer::SCommand_VSync *>(pCommand)->m_pResult->m_Ok = m_pSurface->SetGlSwapInterval(static_cast<const CCommandBuffer::SCommand_VSync *>(pCommand)->m_VSync);
		return true;
	case CCommandBuffer::CMD_MULTISAMPLING:
		// An OpenGL context has its sample count from creation on.
		return true;
	case CCommandBuffer::CMD_WINDOW_CREATE_NTF:
		// Android destroys the window while the app is away and hands out a
		// new one; the context survives and only has to be bound to it.
#if defined(CONF_PLATFORM_ANDROID)
		if(m_GlContext)
			m_pSurface->BindGlContext();
#endif
		return true;
	case CCommandBuffer::CMD_WINDOW_DESTROY_NTF:
#if defined(CONF_PLATFORM_ANDROID)
		if(m_GlContext)
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

void CCommandProcessor_Threaded::RunBuffer(CCommandBuffer *pBuffer)
{
	for(CCommandBuffer::SCommand *pCommand = pBuffer->Head(); pCommand; pCommand = pCommand->m_pNext)
	{
		if(pCommand->m_Cmd == CCommandBuffer::CMD_SIGNAL)
		{
			static_cast<const CCommandBuffer::SCommand_Signal *>(pCommand)->Signal();
			continue;
		}

		const ERunCommandReturnTypes Result = m_pRendererBackend->RunCommand(pCommand);
		if(Result == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED)
		{
			CCommandBuffer::FreeExternalData(pCommand);
			continue;
		}
		if(Result == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_ERROR)
		{
			m_Error = m_pRendererBackend->GetError();
			pBuffer->FreeExternalDataFrom(pCommand);
			return;
		}
		if(Result == ERunCommandReturnTypes::RUN_COMMAND_COMMAND_WARNING)
		{
			if(m_pRendererBackend->GetError().m_ErrorType != GFX_ERROR_TYPE_NONE)
				m_Error = m_pRendererBackend->GetError();
			else
				m_Warning = m_pRendererBackend->GetWarning();
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

	if(m_pRendererBackend->GetError().m_ErrorType != GFX_ERROR_TYPE_NONE)
		m_Error = m_pRendererBackend->GetError();
}

const SGfxErrorContainer &CCommandProcessor_Threaded::GetError() const
{
	return m_Error;
}

void CCommandProcessor_Threaded::ErroneousCleanup()
{
	m_pRendererBackend->ErroneousCleanup();
}

const SGfxWarningContainer &CCommandProcessor_Threaded::GetWarning() const
{
	return m_Warning;
}

// ------------ CGraphicsBackend_Renderer

CGraphicsBackend_Renderer::CGraphicsBackend_Renderer(EBackendType BackendType, int GlMajor, int GlMinor) :
	m_BackendType(BackendType),
	m_GlMajor(GlMajor),
	m_GlMinor(GlMinor)
{
}

CCommandProcessorFragment_Renderer *CGraphicsBackend_Renderer::CreateRenderer() const
{
	switch(m_BackendType)
	{
	case BACKEND_TYPE_NULL:
		return new CCommandProcessorFragment_Null();
	case BACKEND_TYPE_OPENGL_ES:
#if defined(CONF_BACKEND_OPENGL_ES) || defined(CONF_BACKEND_OPENGL_ES3)
		// GLES below 3.0 has no programs, and the window has already forced
		// the version to 3.0 on every platform that offers ES at all - Android
		// and Emscripten build both ES defines together, Linux only ever
		// defines ES3. There is no live path below 3.0 to serve.
		return new CCommandProcessorFragment_OpenGLES3();
#else
		return nullptr;
#endif
	case BACKEND_TYPE_OPENGL:
#if !defined(CONF_BACKEND_OPENGL_ES)
		// The backend with programs is written against OpenGL 3.3 core and
		// nothing else. Every context below that - including 3.0 to 3.2, which
		// is missing four things it uses - goes to the one that stands in for
		// programs altogether.
		if(m_GlMajor < 3 || (m_GlMajor == 3 && m_GlMinor < 3))
			return new CCommandProcessorFragment_OpenGL();
		return new CCommandProcessorFragment_OpenGL3_3();
#else
		return nullptr;
#endif
	case BACKEND_TYPE_VULKAN:
#if defined(CONF_BACKEND_VULKAN)
		return CreateVulkanCommandProcessorFragment();
#else
		return nullptr;
#endif
	default:
		return nullptr;
	}
}

void CGraphicsBackend_Renderer::StopAndDeleteProcessor(bool RendererInitialized)
{
	CCommandBuffer CmdBuffer(1024, 512);
	if(RendererInitialized)
	{
		// The context may have been initialized before a failure, and the post
		// shutdown destroys the device. Destroying it with its children still
		// alive is undefined behaviour, so tear those down first.
		CCommandProcessorFragment_Renderer::SCommand_Shutdown CmdShutdown;
		CmdBuffer.AddCommandUnsafe(CmdShutdown);
		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();
	}
	CCommandProcessor_Threaded::SCommand_UnbindSurface CmdUnbind;
	CmdBuffer.AddCommandUnsafe(CmdUnbind);
	RunBuffer(&CmdBuffer);
	WaitForIdle();
	CmdBuffer.Reset();

	CCommandProcessorFragment_Renderer::SCommand_PostShutdown CmdPost;
	CmdBuffer.AddCommandUnsafe(CmdPost);
	RunBufferSingleThreadedUnsafe(&CmdBuffer);
	CmdBuffer.Reset();

	StopProcessor();
	delete m_pProcessor;
	m_pProcessor = nullptr;
}

int CGraphicsBackend_Renderer::Init(const SGraphicsBackendInit &Params)
{
	dbg_assert(m_pProcessor == nullptr, "Processor was not cleaned up properly.");
	m_aErrorString[0] = '\0';
	m_Capabilities = {};

	CCommandProcessorFragment_Renderer *pRenderer = CreateRenderer();
	if(pRenderer == nullptr)
	{
		log_error("gfx", "The requested graphics backend is not available in this build.");
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
	}
	m_pProcessor = new CCommandProcessor_Threaded(pRenderer, Params.m_pSurface, Params.m_GlContext);
	StartProcessor(m_pProcessor);

	const CCommandProcessorFragment_Renderer::SPresentationSurface Surface = {Params.m_pSurface, static_cast<uint32_t>(std::max(Params.m_Width, 1)), static_cast<uint32_t>(std::max(Params.m_Height, 1))};

	CCommandBuffer CmdBuffer(1024, 512);
	CCommandProcessorFragment_Renderer::SCommand_PreInit CmdPre;
	CmdPre.m_Surface = Surface;
	CmdPre.m_pVendorString = m_aVendorString;
	CmdPre.m_pVersionString = m_aVersionString;
	CmdPre.m_pRendererString = m_aRendererString;
	CmdPre.m_pGpuList = &m_GpuList;
	CmdBuffer.AddCommandUnsafe(CmdPre);
	RunBufferSingleThreadedUnsafe(&CmdBuffer);
	CmdBuffer.Reset();

	// The OpenGL context has to be bound to the render thread before the
	// renderer touches it.
	CCommandProcessor_Threaded::SCommand_BindSurface CmdBind;
	CmdBuffer.AddCommandUnsafe(CmdBind);
	RunBuffer(&CmdBuffer);
	WaitForIdle();
	CmdBuffer.Reset();

	int InitError = 0;
	const char *pErrorString = nullptr;
	CCommandProcessorFragment_Renderer::SCommand_Init CmdInit;
	CmdInit.m_Surface = Surface;
	CmdInit.m_pTextureMemoryUsage = &m_TextureMemoryUsage;
	CmdInit.m_pBufferMemoryUsage = &m_BufferMemoryUsage;
	CmdInit.m_pStreamMemoryUsage = &m_StreamMemoryUsage;
	CmdInit.m_pStagingMemoryUsage = &m_StagingMemoryUsage;
	CmdInit.m_pGpuTiming = GpuTimingShared();
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
	WaitForIdle();
	CmdBuffer.Reset();

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
		WaitForIdle();
		CmdBuffer.Reset();
	}

	return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_NONE;
}

int CGraphicsBackend_Renderer::Shutdown()
{
	if(m_pProcessor != nullptr)
		StopAndDeleteProcessor(true);
	return 0;
}

void CGraphicsBackend_Renderer::ErroneousCleanup()
{
	if(m_pProcessor != nullptr)
		m_pProcessor->ErroneousCleanup();
}

bool CGraphicsBackend_Renderer::GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType)
{
	return GraphicsBackendDriverVersion(BackendType == BACKEND_TYPE_AUTO ? m_BackendType : BackendType, DriverAgeType, Major, Minor, Patch, pName);
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
	case BACKEND_TYPE_NULL:
		return true;
	default:
		return false;
	}
}

EBackendType GraphicsBackendOverrideFromEnvironment()
{
	const char *pBackend = std::getenv("GFX_BACKEND");
	if(pBackend == nullptr)
		pBackend = std::getenv("DDNET_DRIVER");
	if(pBackend == nullptr)
		return BACKEND_TYPE_AUTO;
	if(str_comp_nocase(pBackend, "GLES") == 0)
		return BACKEND_TYPE_OPENGL_ES;
#if defined(CONF_BACKEND_VULKAN)
	if(str_comp_nocase(pBackend, "Vulkan") == 0)
		return BACKEND_TYPE_VULKAN;
#endif
	if(str_comp_nocase(pBackend, "Null") == 0)
		return BACKEND_TYPE_NULL;
	return BACKEND_TYPE_OPENGL;
}

bool GraphicsBackendDriverVersion(EBackendType BackendType, EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName)
{
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
	else if(BackendType == BACKEND_TYPE_NULL)
	{
		pName = "Null";
		return false;
	}
	return false;
}

IGraphicsBackend *CreateGraphicsBackend(EBackendType BackendType, int GlMajor, int GlMinor)
{
	return new CGraphicsBackend_Renderer(BackendType, GlMajor, GlMinor);
}

IGraphicsBackend *CreateOffscreenGraphicsBackend(EBackendType BackendOverride)
{
	// Without an override the choice is gfx_backend, the same way the window
	// client reads it. A backend that needs a surface cannot be meant here,
	// so it falls through to the first one that draws without a window - and
	// says so, because a run that silently uses another backend than the one
	// asked for is worse than one that does not start.
	EBackendType BackendType = BackendOverride;
	if(BackendType == BACKEND_TYPE_AUTO)
	{
		const char *pConfBackend = g_Config.m_GfxBackend;
#if defined(CONF_BACKEND_VULKAN)
		if(str_comp_nocase(pConfBackend, "Vulkan") == 0)
			BackendType = BACKEND_TYPE_VULKAN;
#endif
		if(str_comp_nocase(pConfBackend, "Null") == 0)
			BackendType = BACKEND_TYPE_NULL;
		if(BackendType == BACKEND_TYPE_AUTO)
		{
#if defined(CONF_BACKEND_VULKAN)
			BackendType = BACKEND_TYPE_VULKAN;
#else
			BackendType = BACKEND_TYPE_NULL;
#endif
			log_warn("gfx", "gfx_backend '%s' cannot draw without a surface, using %s", pConfBackend, BackendType == BACKEND_TYPE_VULKAN ? "Vulkan" : "Null");
		}
	}
	switch(BackendType)
	{
	case BACKEND_TYPE_VULKAN:
	case BACKEND_TYPE_NULL:
		return new CGraphicsBackend_Renderer(BackendType, 0, 0);
	default:
		return nullptr;
	}
}
