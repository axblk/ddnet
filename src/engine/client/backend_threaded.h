#ifndef ENGINE_CLIENT_BACKEND_THREADED_H
#define ENGINE_CLIENT_BACKEND_THREADED_H

#include <base/detect.h>
#include <base/sphore.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/graphics_backend.h>
#include <engine/client/render_command_queue.h>

#include <atomic>
#include <mutex>

// Basic threaded backend, abstract, missing init and shutdown functions.
class CGraphicsBackend_Threaded : public IGraphicsBackend
{
private:
	SGfxErrorContainer m_Error;
	SGfxErrorContainer m_ProcessorError;
	SGfxWarningContainer m_Warning;
	SGpuTimingShared m_GpuTiming;

protected:
	SGpuTimingShared *GpuTimingShared() { return &m_GpuTiming; }

public:
	// Constructed on the main thread, the rest of the functions is run on the render thread.
	class ICommandProcessor
	{
	public:
		virtual ~ICommandProcessor() = default;
		virtual void RunBuffer(CCommandBuffer *pBuffer) = 0;

		virtual const SGfxErrorContainer &GetError() const = 0;
		virtual void ErroneousCleanup() = 0;

		virtual const SGfxWarningContainer &GetWarning() const = 0;
	};

	void RunBuffer(CCommandBuffer *pBuffer) override;
	bool RunBufferQueued(CCommandBuffer *pBuffer, bool WaitForCapacity) override;
	bool RunFramePacket(CCommandBuffer *pBuffer, bool WaitForCapacity) override;
	SFrameMailboxStats GetFrameMailboxStats() const override;
	SGpuTiming GpuTiming() const override;
	void SetGpuTimingEnabled(bool Enabled) override;
	void RunBufferSingleThreadedUnsafe(CCommandBuffer *pBuffer) override;
	bool IsIdle() const override;
	void WaitForIdle() override;

	void ProcessError(const SGfxErrorContainer &Error);
	bool RunBufferQueuedInternal(CCommandBuffer *pBuffer, bool WaitForCapacity);

protected:
	void StartProcessor(ICommandProcessor *pProcessor);
	void StopProcessor();

private:
	ICommandProcessor *m_pProcessor = nullptr;
	mutable std::mutex m_ProcessorErrorMutex;
	CRenderCommandQueue m_CommandQueue;
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	CSemaphore m_ThreadStarted;
	void *m_pThread;
	static void ThreadFunc(void *pUser);
#endif

public:
	const SGfxErrorContainer &GetError() const override;
	bool GetWarning(SGfxWarningContainer &Warning) override;
};

// Runs the renderer's commands on the render thread, and the few that belong
// to the surface rather than the renderer: binding an OpenGL context to the
// thread, swapping its buffers, its swap interval. A renderer that presents
// on its own (Vulkan) handles those commands itself and they never
// get here.
class CCommandProcessor_Threaded : public CGraphicsBackend_Threaded::ICommandProcessor
{
public:
	enum
	{
		CMD_BIND_SURFACE = CCommandBuffer::CMDGROUP_PLATFORM,
		CMD_UNBIND_SURFACE,
	};

	struct SCommand_BindSurface : public CCommandBuffer::SCommand
	{
		SCommand_BindSurface() :
			SCommand(CMD_BIND_SURFACE) {}
	};

	struct SCommand_UnbindSurface : public CCommandBuffer::SCommand
	{
		SCommand_UnbindSurface() :
			SCommand(CMD_UNBIND_SURFACE) {}
	};

private:
	CCommandProcessorFragment_Renderer *m_pRendererBackend;
	IPresentationSurface *m_pSurface;
	bool m_GlContext;
	SGfxErrorContainer m_Error;
	SGfxWarningContainer m_Warning;

	bool RunPlatformCommand(const CCommandBuffer::SCommand *pCommand);

public:
	CCommandProcessor_Threaded(CCommandProcessorFragment_Renderer *pRendererBackend, IPresentationSurface *pSurface, bool GlContext);
	~CCommandProcessor_Threaded() override;

	void RunBuffer(CCommandBuffer *pBuffer) override;
	const SGfxErrorContainer &GetError() const override;
	void ErroneousCleanup() override;
	const SGfxWarningContainer &GetWarning() const override;
};

// The one backend: a renderer of the given type on the render thread, drawing
// into the surface it is initialized with, or into render targets alone
// without one. The window is somebody else's.
class CGraphicsBackend_Renderer : public CGraphicsBackend_Threaded
{
	EBackendType m_BackendType;
	int m_GlMajor;
	int m_GlMinor;
	CCommandProcessor_Threaded *m_pProcessor = nullptr;
	std::atomic<uint64_t> m_TextureMemoryUsage{0};
	std::atomic<uint64_t> m_BufferMemoryUsage{0};
	std::atomic<uint64_t> m_StreamMemoryUsage{0};
	std::atomic<uint64_t> m_StagingMemoryUsage{0};
	TTwGraphicsGpuList m_GpuList;
	SBackendCapabilities m_Capabilities{};
	char m_aVendorString[GPU_INFO_STRING_SIZE] = {};
	char m_aVersionString[GPU_INFO_STRING_SIZE] = {};
	char m_aRendererString[GPU_INFO_STRING_SIZE] = {};
	char m_aErrorString[256] = {};

	CCommandProcessorFragment_Renderer *CreateRenderer() const;
	void StopAndDeleteProcessor(bool RendererInitialized);

public:
	CGraphicsBackend_Renderer(EBackendType BackendType, int GlMajor, int GlMinor);

	int Init(const SGraphicsBackendInit &Params) override;
	int Shutdown() override;

	EBackendType BackendType() const override { return m_BackendType; }

	uint64_t TextureMemoryUsage() const override { return m_TextureMemoryUsage; }
	uint64_t BufferMemoryUsage() const override { return m_BufferMemoryUsage; }
	uint64_t StreamedMemoryUsage() const override { return m_StreamMemoryUsage; }
	uint64_t StagingMemoryUsage() const override { return m_StagingMemoryUsage; }
	const TTwGraphicsGpuList &GetGpus() const override { return m_GpuList; }

	void ErroneousCleanup() override;

	bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) override;
	bool IsConfigModernAPI() override { return IsModernGraphicsApi(m_BackendType); }
	SBackendCapabilities GetCapabilities() const override { return m_Capabilities; }
	const char *GetErrorString() override { return m_aErrorString[0] != '\0' ? m_aErrorString : nullptr; }
	const char *GetVendorString() override { return m_aVendorString; }
	const char *GetVersionString() override { return m_aVersionString; }
	const char *GetRendererString() override { return m_aRendererString; }
};

// The backend the surface-less client uses: Vulkan without a surface,
// chosen by the override, else by the config. A configured backend that
// needs a surface falls through to the first that does not, with a warning.
// Null when Vulkan is not compiled in.
IGraphicsBackend *CreateOffscreenGraphicsBackend(EBackendType BackendOverride);

#endif
