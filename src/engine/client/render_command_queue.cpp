#include "render_command_queue.h"

#include <base/dbg.h>

#include <iterator>
#include <utility>

CRenderCommandQueue::CRenderCommandQueue()
{
	m_vQueue.reserve(FRAME_BUFFER_COUNT + RELIABLE_BUFFER_COUNT);
	for(size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
		m_vpFreeFrameBuffers.emplace_back(std::make_unique<CCommandBuffer>(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE));
	for(size_t i = 0; i < RELIABLE_BUFFER_COUNT; ++i)
		m_vpFreeReliableBuffers.emplace_back(std::make_unique<CCommandBuffer>(CMD_BUFFER_CMD_BUFFER_SIZE, CMD_BUFFER_DATA_BUFFER_SIZE, RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE));
}

void CRenderCommandQueue::Start()
{
	std::unique_lock Lock(m_Mutex);
	dbg_assert(m_Stopped, "graphics: command queue was already started");
	dbg_assert(m_BuffersInFlight == 0 && m_vQueue.empty(), "graphics: command queue restarted while busy");
	m_Stopped = false;
}

void CRenderCommandQueue::Stop()
{
	std::unique_lock Lock(m_Mutex);
	m_Stopped = true;
	m_Condition.notify_all();
}

bool CRenderCommandQueue::IsStopped() const
{
	std::unique_lock Lock(m_Mutex);
	return m_Stopped;
}

bool CRenderCommandQueue::WaitDequeue(SEntry &Entry)
{
	std::unique_lock Lock(m_Mutex);
	m_Condition.wait(Lock, [this] { return !m_vQueue.empty() || m_Stopped; });
	if(m_vQueue.empty())
		return false;
	Entry = std::move(m_vQueue.front());
	// The queue holds exactly seven entries, so erasing from the front is
	// cheaper than the bookkeeping a deque would add.
	m_vQueue.erase(m_vQueue.begin());
	return true;
}

bool CRenderCommandQueue::EnqueueBorrowed(CCommandBuffer *pBuffer)
{
	std::unique_lock Lock(m_Mutex);
	if(m_Stopped)
		return false;
	dbg_assert(m_vQueue.size() < m_vQueue.capacity(), "graphics: borrowed command queue exceeded fixed capacity");
	m_vQueue.push_back({pBuffer, nullptr});
	++m_BuffersInFlight;
	m_Condition.notify_all();
	return true;
}

CRenderCommandQueue::EFrameEnqueueResult CRenderCommandQueue::EnqueueFrame(CCommandBuffer *pBuffer)
{
	std::unique_lock Lock(m_Mutex);
	if(m_Stopped)
		return EFrameEnqueueResult::RETRY;

	if(pBuffer->IsReplaceableFramePacket())
	{
		// Keep an older frame ahead of reliable resource work so continuous resource
		// uploads cannot move the only renderable frame to the back forever.
		for(auto It = m_vQueue.begin(); It != m_vQueue.end();)
		{
			if(It->m_pOwnedBuffer != nullptr && It->m_pBuffer->IsReplaceableFramePacket())
			{
				if(std::next(It) != m_vQueue.end())
				{
					pBuffer->Reset();
					++m_FrameStats.m_Produced;
					++m_FrameStats.m_Dropped;
					return EFrameEnqueueResult::DROPPED;
				}
				dbg_assert(!It->m_pBuffer->ContainsCompletions(), "graphics: mailbox tried to drop a frame completion");
				It->m_pOwnedBuffer->Reset();
				m_vpFreeFrameBuffers.emplace_back(std::move(It->m_pOwnedBuffer));
				It = m_vQueue.erase(It);
				--m_BuffersInFlight;
				++m_FrameStats.m_Dropped;
			}
			else
				++It;
		}
	}

	if(m_vpFreeFrameBuffers.empty())
	{
		if(!pBuffer->IsReplaceableFramePacket())
			return EFrameEnqueueResult::RETRY;
		pBuffer->Reset();
		++m_FrameStats.m_Produced;
		++m_FrameStats.m_Dropped;
		return EFrameEnqueueResult::DROPPED;
	}

	EnqueueFrameLocked(pBuffer);
	return EFrameEnqueueResult::QUEUED;
}

bool CRenderCommandQueue::WaitEnqueuePinnedFrame(CCommandBuffer *pBuffer)
{
	std::unique_lock Lock(m_Mutex);
	dbg_assert(!pBuffer->IsReplaceableFramePacket(), "graphics: replaceable frame waited for mailbox capacity");
	m_Condition.wait(Lock, [this] { return m_Stopped || !m_vpFreeFrameBuffers.empty(); });
	if(m_Stopped)
		return false;
	EnqueueFrameLocked(pBuffer);
	return true;
}

void CRenderCommandQueue::EnqueueFrameLocked(CCommandBuffer *pBuffer)
{
	dbg_assert(!m_vpFreeFrameBuffers.empty(), "graphics: frame enqueue without a free arena");
	std::unique_ptr<CCommandBuffer> pOwnedBuffer = std::move(m_vpFreeFrameBuffers.back());
	m_vpFreeFrameBuffers.pop_back();
	pOwnedBuffer->Swap(*pBuffer);
	CCommandBuffer *pQueuedBuffer = pOwnedBuffer.get();
	dbg_assert(m_vQueue.size() < m_vQueue.capacity(), "graphics: frame mailbox exceeded fixed capacity");
	m_vQueue.push_back({pQueuedBuffer, std::move(pOwnedBuffer)});
	++m_FrameStats.m_Produced;
	++m_BuffersInFlight;
	m_Condition.notify_all();
}

bool CRenderCommandQueue::CanEnqueueReliableLocked(const CCommandBuffer *pBuffer) const
{
	dbg_assert(pBuffer->SubmissionInfo().m_Channel == CCommandBuffer::ECommandChannel::RELIABLE, "graphics: internal reliable queue received a frame packet");
	const size_t ReservedBuffers = !pBuffer->UsesReservedReliableBudget() ? RELIABLE_RESERVED_BUFFER_COUNT : 0;
	if(m_vpFreeReliableBuffers.size() <= ReservedBuffers)
		return false;
	const bool UsesReservedBudget = pBuffer->UsesReservedReliableBudget() || pBuffer->ContainsCompletions();
	const size_t ExternalDataLimit = RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE + (UsesReservedBudget ? RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE : 0);
	if(m_ReliableExternalDataInFlight > ExternalDataLimit || pBuffer->m_ExternalDataSize > ExternalDataLimit - m_ReliableExternalDataInFlight)
		return false;
	return true;
}

void CRenderCommandQueue::EnqueueReliableLocked(CCommandBuffer *pBuffer)
{
	dbg_assert(CanEnqueueReliableLocked(pBuffer), "graphics: reliable enqueue exceeded capacity");

	std::unique_ptr<CCommandBuffer> pOwnedBuffer = std::move(m_vpFreeReliableBuffers.back());
	m_vpFreeReliableBuffers.pop_back();
	pOwnedBuffer->Swap(*pBuffer);
	CCommandBuffer *pQueuedBuffer = pOwnedBuffer.get();
	dbg_assert(m_vQueue.size() < m_vQueue.capacity(), "graphics: owned command queue exceeded fixed capacity");
	m_vQueue.push_back({pQueuedBuffer, std::move(pOwnedBuffer)});
	m_ReliableExternalDataInFlight += pQueuedBuffer->m_ExternalDataSize;
	++m_BuffersInFlight;
	m_Condition.notify_all();
}

bool CRenderCommandQueue::EnqueueReliable(CCommandBuffer *pBuffer)
{
	std::unique_lock Lock(m_Mutex);
	if(m_Stopped || !CanEnqueueReliableLocked(pBuffer))
		return false;
	EnqueueReliableLocked(pBuffer);
	return true;
}

bool CRenderCommandQueue::WaitEnqueueReliable(CCommandBuffer *pBuffer)
{
	std::unique_lock Lock(m_Mutex);
	const bool UsesReservedBudget = pBuffer->UsesReservedReliableBudget() || pBuffer->ContainsCompletions();
	const size_t ExternalDataLimit = RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE + (UsesReservedBudget ? RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE : 0);
	if(pBuffer->m_ExternalDataSize > ExternalDataLimit)
		return false;
	m_Condition.wait(Lock, [this, pBuffer] { return m_Stopped || CanEnqueueReliableLocked(pBuffer); });
	if(m_Stopped)
		return false;
	EnqueueReliableLocked(pBuffer);
	return true;
}

void CRenderCommandQueue::Recycle(SEntry &&Entry, bool Processed)
{
	std::unique_lock Lock(m_Mutex);
	if(Entry.m_pOwnedBuffer != nullptr)
	{
		const CCommandBuffer::ECommandChannel Channel = Entry.m_pBuffer->SubmissionInfo().m_Channel;
		if(Channel == CCommandBuffer::ECommandChannel::RELIABLE)
		{
			dbg_assert(m_ReliableExternalDataInFlight >= Entry.m_pBuffer->m_ExternalDataSize, "graphics: reliable payload accounting underflow");
			m_ReliableExternalDataInFlight -= Entry.m_pBuffer->m_ExternalDataSize;
		}
		else if(Processed)
			++m_FrameStats.m_Rendered;
		else
			++m_FrameStats.m_Dropped;

		if(!Processed)
			Entry.m_pOwnedBuffer->FreeExternalData();
		Entry.m_pOwnedBuffer->Reset();
		auto &vpFreeBuffers = Channel == CCommandBuffer::ECommandChannel::FRAME ? m_vpFreeFrameBuffers : m_vpFreeReliableBuffers;
		vpFreeBuffers.emplace_back(std::move(Entry.m_pOwnedBuffer));
	}
	dbg_assert(m_BuffersInFlight > 0, "graphics: command queue in-flight accounting underflow");
	--m_BuffersInFlight;
	m_Condition.notify_all();
}

void CRenderCommandQueue::DiscardFrame(CCommandBuffer *pBuffer)
{
	pBuffer->SignalCompletions();
	pBuffer->FreeExternalData();
	pBuffer->Reset();
	std::unique_lock Lock(m_Mutex);
	++m_FrameStats.m_Produced;
	++m_FrameStats.m_Dropped;
}

void CRenderCommandQueue::RecordSynchronousFrame(bool Rendered)
{
	std::unique_lock Lock(m_Mutex);
	++m_FrameStats.m_Produced;
	if(Rendered)
		++m_FrameStats.m_Rendered;
	else
		++m_FrameStats.m_Dropped;
}

bool CRenderCommandQueue::IsIdle() const
{
	std::unique_lock Lock(m_Mutex);
	return m_BuffersInFlight == 0;
}

void CRenderCommandQueue::WaitForIdle()
{
	std::unique_lock Lock(m_Mutex);
	m_Condition.wait(Lock, [this] { return m_BuffersInFlight == 0; });
}

IGraphics::SFrameMailboxStats CRenderCommandQueue::GetFrameMailboxStats() const
{
	std::unique_lock Lock(m_Mutex);
	return m_FrameStats;
}
