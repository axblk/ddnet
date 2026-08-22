#ifndef ENGINE_CLIENT_BACKEND_THREADED_H
#define ENGINE_CLIENT_BACKEND_THREADED_H

#include <base/detect.h>
#include <base/sphore.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/graphics_threaded.h>
#include <engine/client/render_command_queue.h>

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

class CCommandProcessor_Threaded : public CGraphicsBackend_Threaded::ICommandProcessor
{
private:
	CCommandProcessorFragment_Renderer *m_pRendererBackend;
	SGfxErrorContainer m_Error;
	SGfxWarningContainer m_Warning;

protected:
	virtual bool RunPlatformCommand(const CCommandBuffer::SCommand *pCommand);

public:
	explicit CCommandProcessor_Threaded(CCommandProcessorFragment_Renderer *pRendererBackend);
	~CCommandProcessor_Threaded() override;

	void RunBuffer(CCommandBuffer *pBuffer) override;
	const SGfxErrorContainer &GetError() const override;
	void ErroneousCleanup() override;
	const SGfxWarningContainer &GetWarning() const override;
};

IGraphicsBackend *CreateOffscreenGraphicsBackend(EBackendType BackendOverride);
#if !defined(CONF_DEMO_RENDER_TOOL)
IGraphicsBackend *CreatePresentationGraphicsBackend(EBackendType BackendOverride);
#endif

#endif
