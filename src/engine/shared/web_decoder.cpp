/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "web_decoder.h"

#include <base/dbg.h>
#include <base/thread.h>

#include <emscripten/threading.h>

#include <cmath>

// web_decoder.js, run on the page's thread whichever thread calls them.
extern "C" {
void ddnet_web_decode_opus(const uint8_t *pFile, int FileSize, const uint8_t *pHead, int HeadSize, const uint8_t *pPackets, const uint32_t *pPacketSizes, int NumPackets, int Channels, int16_t *pSamples, int NumFrames, int32_t *pDecodedFrames, std::atomic<int32_t> *pState);
}

CWebDecode::~CWebDecode()
{
	if(Started())
		Wait();
}

void CWebDecode::StartOpus(const uint8_t *pFile, size_t FileSize, const uint8_t *pHead, size_t HeadSize, const uint8_t *pPackets, const uint32_t *pPacketSizes, size_t NumPackets, int Channels, int16_t *pSamples, int64_t NumFrames, int32_t *pDecodedFrames)
{
	dbg_assert(!Started(), "Web decode started twice");
	m_State.store(STATE_PENDING);
	*pDecodedFrames = 0;
	ddnet_web_decode_opus(pFile, (int)FileSize, pHead, (int)HeadSize, pPackets, pPacketSizes, (int)NumPackets, Channels, pSamples, (int)NumFrames, pDecodedFrames, &m_State);
}

bool CWebDecode::Wait()
{
	dbg_assert(Started(), "Web decode was never started");
	while(!Finished())
	{
		// The page's thread gives the browser its turn, a worker waits for
		// the decoder to wake it.
		if(emscripten_is_main_runtime_thread())
			web_yield(1);
		else
			emscripten_futex_wait(&m_State, STATE_PENDING, INFINITY);
	}
	return m_State.load() == STATE_DONE;
}
