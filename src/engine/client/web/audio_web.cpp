#include "audio_web.h"

#include "web_platform.h"

#include <base/dbg.h>
#include <base/thread.h>

#include <emscripten/threading.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
	// How far ahead the mixer stays: a little more than two of the blocks the
	// browser asks a slow page for, and far less than a frame of the game at
	// its worst.
	constexpr unsigned TARGET_FRAMES = 2048;
	constexpr unsigned CHUNK_FRAMES = 256;
	// Long enough not to spin, short enough to notice a close.
	constexpr double WAIT_MILLISECONDS = 20.0;
} // namespace

int CWebAudioOutput::Open(int Rate, unsigned MaxFrames, FMix &&Mix)
{
	dbg_assert(m_pThread == nullptr, "The web audio output is open already");
	m_pRing = static_cast<SWebAudioRing *>(calloc(1, sizeof(SWebAudioRing) + CAPACITY * 2 * sizeof(int16_t)));
	if(m_pRing == nullptr)
		return 0;
	const int PlayedRate = ddnet_web_audio_open(Rate, m_pRing, CAPACITY);
	if(PlayedRate <= 0)
	{
		free(m_pRing);
		m_pRing = nullptr;
		return 0;
	}
	m_ChunkFrames = std::min(CHUNK_FRAMES, std::max(MaxFrames, 1u));
	m_TargetFrames = TARGET_FRAMES;
	m_Mix = std::move(Mix);
	m_Stop.store(false);
	m_pThread = thread_init(MixerThread, this, "web audio");
	if(m_pThread == nullptr)
	{
		ddnet_web_audio_close();
		free(m_pRing);
		m_pRing = nullptr;
		return 0;
	}
	return PlayedRate;
}

void CWebAudioOutput::Close()
{
	if(m_pThread == nullptr)
		return;
	m_Stop.store(true);
	// Wakes the mixer out of its wait.
	emscripten_futex_wake(&m_pRing->m_Read, 1);
	thread_wait(m_pThread);
	m_pThread = nullptr;
	ddnet_web_audio_close();
	free(m_pRing);
	m_pRing = nullptr;
	m_Mix = nullptr;
}

void CWebAudioOutput::SetPaused(bool Paused)
{
	if(m_Paused.exchange(Paused) == Paused || m_pThread == nullptr)
		return;
	ddnet_web_audio_pause(Paused ? 1 : 0);
}

void CWebAudioOutput::MixerThread(void *pUser)
{
	static_cast<CWebAudioOutput *>(pUser)->RunMixer();
}

void CWebAudioOutput::RunMixer()
{
	int16_t aChunk[CHUNK_FRAMES * 2];
	while(!m_Stop.load())
	{
		const int32_t Read = m_pRing->m_Read.load(std::memory_order_acquire);
		const int32_t Write = m_pRing->m_Write.load(std::memory_order_relaxed);
		const unsigned Queued = static_cast<uint32_t>(Write - Read);
		if(m_Paused.load() || Queued + m_ChunkFrames > m_TargetFrames)
		{
			// The worklet wakes this whenever it took a block.
			emscripten_futex_wait(&m_pRing->m_Read, static_cast<uint32_t>(Read), WAIT_MILLISECONDS);
			continue;
		}
		m_Mix(aChunk, m_ChunkFrames);
		for(unsigned Frame = 0; Frame < m_ChunkFrames;)
		{
			const unsigned Index = static_cast<uint32_t>(Write + Frame) % CAPACITY;
			const unsigned Count = std::min(m_ChunkFrames - Frame, CAPACITY - Index);
			std::memcpy(&m_pRing->Samples()[Index * 2], &aChunk[Frame * 2], Count * 2 * sizeof(int16_t));
			Frame += Count;
		}
		m_pRing->m_Write.store(Write + static_cast<int32_t>(m_ChunkFrames), std::memory_order_release);
	}
}
