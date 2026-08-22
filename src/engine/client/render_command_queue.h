#ifndef ENGINE_CLIENT_RENDER_COMMAND_QUEUE_H
#define ENGINE_CLIENT_RENDER_COMMAND_QUEUE_H

#include <engine/client/graphics_threaded.h>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

class CRenderCommandQueue
{
public:
	enum class EFrameEnqueueResult
	{
		QUEUED,
		DROPPED,
		RETRY,
	};

	struct SEntry
	{
		CCommandBuffer *m_pBuffer = nullptr;
		std::unique_ptr<CCommandBuffer> m_pOwnedBuffer;
	};

	CRenderCommandQueue();

	void Start();
	void Stop();
	bool IsStopped() const;
	bool WaitDequeue(SEntry &Entry);

	bool EnqueueBorrowed(CCommandBuffer *pBuffer);
	EFrameEnqueueResult EnqueueFrame(CCommandBuffer *pBuffer);
	bool WaitEnqueuePinnedFrame(CCommandBuffer *pBuffer);
	bool EnqueueReliable(CCommandBuffer *pBuffer);
	bool WaitEnqueueReliable(CCommandBuffer *pBuffer);
	void Recycle(SEntry &&Entry, bool Processed);
	void DiscardFrame(CCommandBuffer *pBuffer);
	void RecordSynchronousFrame(bool Rendered);

	bool IsIdle() const;
	void WaitForIdle();
	IGraphics::SFrameMailboxStats GetFrameMailboxStats() const;

private:
	static constexpr size_t FRAME_BUFFER_COUNT = 3;
	static constexpr size_t RELIABLE_BUFFER_COUNT = 4;
	static constexpr size_t RELIABLE_RESERVED_BUFFER_COUNT = 1;

	void EnqueueFrameLocked(CCommandBuffer *pBuffer);
	bool CanEnqueueReliableLocked(const CCommandBuffer *pBuffer) const;
	void EnqueueReliableLocked(CCommandBuffer *pBuffer);

	mutable std::mutex m_Mutex;
	std::condition_variable m_Condition;
	std::vector<SEntry> m_vQueue;
	std::vector<std::unique_ptr<CCommandBuffer>> m_vpFreeFrameBuffers;
	std::vector<std::unique_ptr<CCommandBuffer>> m_vpFreeReliableBuffers;
	size_t m_ReliableExternalDataInFlight = 0;
	size_t m_BuffersInFlight = 0;
	bool m_Stopped = true;
	IGraphics::SFrameMailboxStats m_FrameStats;
};

#endif
