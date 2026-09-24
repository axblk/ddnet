#ifndef ENGINE_CLIENT_BACKEND_THREADED_H
#define ENGINE_CLIENT_BACKEND_THREADED_H

#include <base/detect.h>
#include <base/sphore.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/graphics_backend.h>
#include <engine/client/render_command_queue.h>

#include <atomic>
#include <mutex>

// A renderer of the given type on the render thread, drawing into the surface
// it is initialized with, or into render targets alone without one. Besides
// the renderer's commands it runs the few that belong to an OpenGL context:
// binding it to the thread, swapping and the swap interval.
class CGraphicsBackend_Threaded : public IGraphicsBackend
{
public:
	CGraphicsBackend_Threaded(EBackendType BackendType, int GlMajor, int GlMinor);

	int Init(const SGraphicsBackendInit &Params) override;
	int Shutdown() override;

	EBackendType BackendType() const override { return m_BackendType; }

	uint64_t TextureMemoryUsage() const override { return m_TextureMemoryUsage; }
	uint64_t BufferMemoryUsage() const override { return m_BufferMemoryUsage; }
	uint64_t StreamedMemoryUsage() const override { return m_StreamMemoryUsage; }
	uint64_t StagingMemoryUsage() const override { return m_StagingMemoryUsage; }
	const TTwGraphicsGpuList &GetGpus() const override { return m_GpuList; }

	bool RunBufferQueued(CCommandBuffer *pBuffer) override;
	bool RunFramePacket(CCommandBuffer *pBuffer, bool WaitForCapacity) override;
	SFrameMailboxStats GetFrameMailboxStats() const override { return m_CommandQueue.GetFrameMailboxStats(); }
	SGpuTiming GpuTiming() const override { return m_GpuTiming.Snapshot(); }
	void SetGpuTimingEnabled(bool Enabled) override { m_GpuTiming.SetEnabled(Enabled); }
	bool IsIdle() const override;
	void WaitForIdle() override;

	void ErroneousCleanup() override;

	SBackendCapabilities GetCapabilities() const override { return m_Capabilities; }
	const char *GetErrorString() override { return m_aErrorString[0] != '\0' ? m_aErrorString : nullptr; }
	const char *GetVendorString() override { return m_aVendorString; }
	const char *GetVersionString() override { return m_aVersionString; }
	const char *GetRendererString() override { return m_aRendererString; }

	const SGfxErrorContainer &GetError() const override { return m_Error; }
	bool GetWarning(SGfxWarningContainer &Warning) override;

private:
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

	EBackendType m_BackendType;
	// Only desktop OpenGL asks for the context version.
	int m_GlMajor;
	int m_GlMinor;
	std::atomic<uint64_t> m_TextureMemoryUsage{0};
	std::atomic<uint64_t> m_BufferMemoryUsage{0};
	std::atomic<uint64_t> m_StreamMemoryUsage{0};
	std::atomic<uint64_t> m_StagingMemoryUsage{0};
	TTwGraphicsGpuList m_GpuList;
	SBackendCapabilities m_Capabilities{};
	SGpuTimingShared m_GpuTiming;
	char m_aVendorString[GPU_INFO_STRING_SIZE] = {};
	char m_aVersionString[GPU_INFO_STRING_SIZE] = {};
	char m_aRendererString[GPU_INFO_STRING_SIZE] = {};
	char m_aErrorString[256] = {};
	SGfxErrorContainer m_Error;
	SGfxWarningContainer m_Warning;

	// Owned by the render thread while it runs.
	CCommandProcessorFragment_Renderer *m_pRenderer = nullptr;
	IPresentationSurface *m_pSurface = nullptr;
	SGfxErrorContainer m_RenderThreadError;
	SGfxWarningContainer m_RenderThreadWarning;

	// What the render thread reported, for the main thread.
	mutable std::mutex m_ProcessorErrorMutex;
	SGfxErrorContainer m_ProcessorError;
	CRenderCommandQueue m_CommandQueue;
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	CSemaphore m_ThreadStarted;
	void *m_pThread = nullptr;
	static void ThreadFunc(void *pUser);
#endif

	CCommandProcessorFragment_Renderer *CreateRenderer() const;
	SGfxErrorContainer ProcessorError() const;
	[[noreturn]] void ProcessError(const SGfxErrorContainer &Error);
	void StartProcessor();
	void StopProcessor();
	void StopAndDeleteProcessor(bool RendererInitialized);
	// Hands a buffer the caller keeps to the render thread and returns once it
	// has been taken; WaitForIdle waits for it to be run.
	void RunBuffer(CCommandBuffer *pBuffer);
	// Runs the commands of a buffer on the calling thread.
	void ProcessBuffer(CCommandBuffer *pBuffer);
	bool RunPlatformCommand(const CCommandBuffer::SCommand *pCommand);
};

// The backend the surface-less client uses: Vulkan or WebGPU without a
// surface, chosen by the override, else by the config. A configured backend
// that needs a surface falls through to the first that does not, with a
// warning. Null when neither is compiled in.
IGraphicsBackend *CreateOffscreenGraphicsBackend(EBackendType BackendOverride);

#endif
