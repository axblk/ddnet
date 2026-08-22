#include "backend_threaded.h"

#include <base/thread.h>

#if defined(CONF_BACKEND_VULKAN)
#include <engine/client/backend/vulkan/backend_vulkan.h>
#endif
#if defined(CONF_BACKEND_WEBGPU)
#include <engine/client/backend/webgpu/backend_webgpu.h>
#endif

#include <string>
#include <utility>

#if defined(CONF_PLATFORM_MACOS)
#include <objc/objc-runtime.h>

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
	m_GpuTiming.SetEnabled(Enabled);
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

CCommandProcessor_Threaded::CCommandProcessor_Threaded(CCommandProcessorFragment_Renderer *pRendererBackend) :
	m_pRendererBackend(pRendererBackend)
{
	dbg_assert(m_pRendererBackend != nullptr, "graphics command processor backend is unavailable");
}

CCommandProcessor_Threaded::~CCommandProcessor_Threaded()
{
	delete m_pRendererBackend;
}

bool CCommandProcessor_Threaded::RunPlatformCommand(const CCommandBuffer::SCommand *pCommand)
{
	return pCommand->m_Cmd == CCommandProcessorFragment_Renderer::CMD_PRE_INIT ||
	       pCommand->m_Cmd == CCommandProcessorFragment_Renderer::CMD_POST_SHUTDOWN;
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

#if defined(CONF_BACKEND_VULKAN) || defined(CONF_BACKEND_WEBGPU)
class CGraphicsBackend_Offscreen : public CGraphicsBackend_Threaded
{
	EBackendType m_BackendType;
	ICommandProcessor *m_pProcessor = nullptr;
	std::atomic<uint64_t> m_TextureMemoryUsage{0};
	std::atomic<uint64_t> m_BufferMemoryUsage{0};
	std::atomic<uint64_t> m_StreamMemoryUsage{0};
	std::atomic<uint64_t> m_StagingMemoryUsage{0};
	TTwGraphicsGpuList m_GpuList;
	SBackendCapabilities m_Capabilities{};
	int m_Width = 1;
	int m_Height = 1;
	char m_aVendorString[256] = {};
	char m_aVersionString[256] = {};
	char m_aRendererString[256] = {};

public:
	explicit CGraphicsBackend_Offscreen(EBackendType BackendType) :
		m_BackendType(BackendType)
	{
	}

	int Init(const char *pName, int *pScreen, int *pWidth, int *pHeight, int *pRefreshRate, int *pFsaaSamples, int Flags, int *pDesktopWidth, int *pDesktopHeight, int *pCurrentWidth, int *pCurrentHeight, IStorage *pStorage) override
	{
		(void)pName;
		(void)Flags;
		m_Width = std::max(*pWidth, 1);
		m_Height = std::max(*pHeight, 1);
		*pScreen = 0;
		*pWidth = *pDesktopWidth = *pCurrentWidth = m_Width;
		*pHeight = *pDesktopHeight = *pCurrentHeight = m_Height;
		*pRefreshRate = 0;

		CCommandProcessorFragment_Renderer *pRenderer = nullptr;
#if defined(CONF_BACKEND_VULKAN)
		if(m_BackendType == BACKEND_TYPE_VULKAN)
			pRenderer = CreateVulkanCommandProcessorFragment();
#endif
#if defined(CONF_BACKEND_WEBGPU)
		if(m_BackendType == BACKEND_TYPE_WEBGPU)
			pRenderer = CreateWebGpuCommandProcessorFragment({}, WebGpuBackendTypeFromConfig());
#endif
		if(pRenderer == nullptr)
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
		m_pProcessor = new CCommandProcessor_Threaded(pRenderer);
		StartProcessor(m_pProcessor);

		CCommandBuffer CmdBuffer(1024, 512);
		CCommandProcessorFragment_Renderer::SCommand_PreInit CmdPre;
		CmdPre.m_pWindow = nullptr;
		CmdPre.m_BackendMode = EGraphicsBackendMode::OFFSCREEN;
		CmdPre.m_Width = m_Width;
		CmdPre.m_Height = m_Height;
		CmdPre.m_pVendorString = m_aVendorString;
		CmdPre.m_pVersionString = m_aVersionString;
		CmdPre.m_pRendererString = m_aRendererString;
		CmdPre.m_pGpuList = &m_GpuList;
		CmdBuffer.AddCommandUnsafe(CmdPre);
		RunBufferSingleThreadedUnsafe(&CmdBuffer);
		CmdBuffer.Reset();

		int InitError = 0;
		const char *pErrorString = nullptr;
		CCommandProcessorFragment_Renderer::SCommand_Init CmdInit;
		CmdInit.m_pWindow = nullptr;
		CmdInit.m_BackendMode = EGraphicsBackendMode::OFFSCREEN;
		CmdInit.m_Width = m_Width;
		CmdInit.m_Height = m_Height;
		CmdInit.m_pTextureMemoryUsage = &m_TextureMemoryUsage;
		CmdInit.m_pBufferMemoryUsage = &m_BufferMemoryUsage;
		CmdInit.m_pStreamMemoryUsage = &m_StreamMemoryUsage;
		CmdInit.m_pStagingMemoryUsage = &m_StagingMemoryUsage;
		CmdInit.m_pGpuTiming = GpuTimingShared();
		CmdInit.m_pGpuList = &m_GpuList;
		CmdInit.m_pStorage = pStorage;
		CmdInit.m_pCapabilities = &m_Capabilities;
		CmdInit.m_pInitError = &InitError;
		if(m_BackendType == BACKEND_TYPE_WEBGPU)
		{
			CmdInit.m_RequestedMajor = 1;
			CmdInit.m_RequestedMinor = 0;
		}
#if defined(CONF_BACKEND_VULKAN)
		else
		{
			CmdInit.m_RequestedMajor = BACKEND_VULKAN_VERSION_MAJOR;
			CmdInit.m_RequestedMinor = BACKEND_VULKAN_VERSION_MINOR;
		}
#endif
		CmdInit.m_RequestedPatch = 0;
		CmdInit.m_VSync = false;
		CmdInit.m_RequestedMultiSamplingCount = *pFsaaSamples;
		CmdInit.m_pErrStringPtr = &pErrorString;
		CmdInit.m_pVendorString = m_aVendorString;
		CmdInit.m_pVersionString = m_aVersionString;
		CmdInit.m_pRendererString = m_aRendererString;
		CmdInit.m_RequestedBackend = m_BackendType;
		CmdBuffer.AddCommandUnsafe(CmdInit);
		RunBuffer(&CmdBuffer);
		WaitForIdle();

		if(InitError == 0)
			return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_NONE;

		CmdBuffer.Reset();
		if(InitError != -2)
		{
			// The context may have been initialized before the failure, and the post
			// shutdown destroys the device. Destroying it with its children still
			// alive is undefined behaviour, so tear those down first.
			CCommandProcessorFragment_Renderer::SCommand_Shutdown CmdShutdown;
			CmdBuffer.AddCommandUnsafe(CmdShutdown);
			RunBuffer(&CmdBuffer);
			WaitForIdle();
			CmdBuffer.Reset();
		}
		CCommandProcessorFragment_Renderer::SCommand_PostShutdown CmdPost;
		CmdBuffer.AddCommandUnsafe(CmdPost);
		RunBufferSingleThreadedUnsafe(&CmdBuffer);
		StopProcessor();
		delete m_pProcessor;
		m_pProcessor = nullptr;
		return EGraphicsBackendErrorCodes::GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED;
	}

	int Shutdown() override
	{
		if(m_pProcessor == nullptr)
			return 0;
		CCommandBuffer CmdBuffer(1024, 512);
		CCommandProcessorFragment_Renderer::SCommand_Shutdown CmdShutdown;
		CmdBuffer.AddCommandUnsafe(CmdShutdown);
		RunBuffer(&CmdBuffer);
		WaitForIdle();
		CmdBuffer.Reset();
		CCommandProcessorFragment_Renderer::SCommand_PostShutdown CmdPost;
		CmdBuffer.AddCommandUnsafe(CmdPost);
		RunBufferSingleThreadedUnsafe(&CmdBuffer);
		StopProcessor();
		delete m_pProcessor;
		m_pProcessor = nullptr;
		return 0;
	}

	uint64_t TextureMemoryUsage() const override { return m_TextureMemoryUsage; }
	uint64_t BufferMemoryUsage() const override { return m_BufferMemoryUsage; }
	uint64_t StreamedMemoryUsage() const override { return m_StreamMemoryUsage; }
	uint64_t StagingMemoryUsage() const override { return m_StagingMemoryUsage; }
	const TTwGraphicsGpuList &GetGpus() const override { return m_GpuList; }
	void GetVideoModes(CVideoMode *pModes, int MaxModes, int *pNumModes, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int Screen) override
	{
		(void)HiDPIScale;
		(void)MaxWindowWidth;
		(void)MaxWindowHeight;
		(void)Screen;
		*pNumModes = MaxModes > 0 ? 1 : 0;
		if(*pNumModes != 0)
			pModes[0] = {m_Width, m_Height, m_Width, m_Height, 0};
	}
	void GetCurrentVideoMode(CVideoMode &Mode, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int Screen) override { Mode = {m_Width, m_Height, m_Width, m_Height, 0}; }
	int GetNumScreens() const override { return 1; }
	const char *GetScreenName(int Screen) const override { return "Offscreen"; }
	void Minimize() override {}
	void SetWindowParams(int FullscreenMode, bool IsBorderless) override {}
	bool SetWindowScreen(int Index, bool MoveToCenter, ivec2 *pDesktopSize) override { return true; }
	bool UpdateDisplayMode(int Index, ivec2 *pDesktopSize) override
	{
		*pDesktopSize = {m_Width, m_Height};
		return true;
	}
	int GetWindowScreen() override { return 0; }
	int WindowActive() override { return 1; }
	int WindowOpen() override { return 1; }
	void SetWindowGrab(bool Grab) override {}
	bool ResizeWindow(int Width, int Height, int RefreshRate) override
	{
		m_Width = std::max(Width, 1);
		m_Height = std::max(Height, 1);
		return true;
	}
	void GetViewportSize(int &Width, int &Height) override
	{
		Width = m_Width;
		Height = m_Height;
	}
	void NotifyWindow() override {}
	bool IsScreenKeyboardShown() override { return false; }
	void WindowDestroyNtf(uint32_t WindowId) override {}
	void WindowCreateNtf(uint32_t WindowId) override {}
	bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) override
	{
		if(m_BackendType == BACKEND_TYPE_WEBGPU)
		{
			pName = "WebGPU";
			Major = 1;
			Minor = 0;
		}
#if defined(CONF_BACKEND_VULKAN)
		else
		{
			pName = "Vulkan";
			Major = BACKEND_VULKAN_VERSION_MAJOR;
			Minor = BACKEND_VULKAN_VERSION_MINOR;
		}
#endif
		Patch = 0;
		return (BackendType == BACKEND_TYPE_AUTO || BackendType == m_BackendType) && DriverAgeType == GRAPHICS_DRIVER_AGE_TYPE_DEFAULT;
	}
	bool IsConfigModernAPI() override { return true; }
	SBackendCapabilities GetCapabilities() const override { return m_Capabilities; }
	const char *GetVendorString() override { return m_aVendorString; }
	const char *GetVersionString() override { return m_aVersionString; }
	const char *GetRendererString() override { return m_aRendererString; }
	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override { return std::nullopt; }
};
#endif

IGraphicsBackend *CreateOffscreenGraphicsBackend(EBackendType BackendOverride)
{
#if defined(CONF_BACKEND_VULKAN)
	if(BackendOverride == BACKEND_TYPE_AUTO || BackendOverride == BACKEND_TYPE_VULKAN)
		return new CGraphicsBackend_Offscreen(BACKEND_TYPE_VULKAN);
#endif
#if defined(CONF_BACKEND_WEBGPU)
	if(BackendOverride == BACKEND_TYPE_AUTO || BackendOverride == BACKEND_TYPE_WEBGPU)
		return new CGraphicsBackend_Offscreen(BACKEND_TYPE_WEBGPU);
#endif
	return nullptr;
}

IGraphicsBackend *CreateGraphicsBackend(EBackendType BackendOverride, EGraphicsBackendMode BackendMode)
{
	if(BackendMode == EGraphicsBackendMode::OFFSCREEN)
		return CreateOffscreenGraphicsBackend(BackendOverride);
#if !defined(CONF_DEMO_RENDER_TOOL)
	return CreatePresentationGraphicsBackend(BackendOverride);
#else
	return nullptr;
#endif
}

#if defined(CONF_DEMO_RENDER_TOOL)
std::optional<int> ShowMessageBoxWithoutGraphics(const IGraphics::CMessageBox &)
{
	return std::nullopt;
}
#endif
