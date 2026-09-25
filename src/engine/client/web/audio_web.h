#ifndef ENGINE_CLIENT_WEB_AUDIO_WEB_H
#define ENGINE_CLIENT_WEB_AUDIO_WEB_H

#include <atomic>
#include <cstdint>
#include <functional>

/**
 * The ring the mixer writes the sound into and the page's audio worklet reads
 * it out of, in the program's memory. Both counters only grow and wrap; the
 * frames between them are the ones not played yet.
 */
struct SWebAudioRing
{
	// Written by the mixer.
	std::atomic<int32_t> m_Write;
	// Written by the worklet, which wakes the mixer on it.
	std::atomic<int32_t> m_Read;
	int32_t m_aPadding[2];

	// Follow the counters: interleaved stereo, as many frames as the ring
	// holds.
	int16_t *Samples() { return reinterpret_cast<int16_t *>(this + 1); }
};
static_assert(sizeof(SWebAudioRing) == 16, "web_platform.js reads the samples 16 bytes in");

/**
 * The sound output of the web tools, in place of an SDL audio device. A thread
 * of its own mixes ahead of the worklet until the ring holds a little more than
 * the browser plays in one go, and sleeps on the worklet's counter in between.
 */
class CWebAudioOutput
{
public:
	/**
	 * Mixes `Frames` frames of interleaved stereo into `pOut`.
	 */
	using FMix = std::function<void(int16_t *pOut, unsigned Frames)>;

	~CWebAudioOutput() { Close(); }

	/**
	 * Opens the page's output and starts mixing into it.
	 *
	 * @param Rate The sample rate asked for.
	 * @param MaxFrames The most frames `Mix` takes at once.
	 * @param Mix What produces the sound, on the mixing thread.
	 *
	 * @return The sample rate the browser plays at, 0 where it has no output.
	 */
	int Open(int Rate, unsigned MaxFrames, FMix &&Mix);
	/**
	 * Whether the program plays sound at all. Where it renders a video
	 * without a page, there is nothing to play it on, and the sound is mixed
	 * for the video alone.
	 */
	static bool Wanted() { return ms_Wanted; }
	static void SetWanted(bool Wanted) { ms_Wanted = Wanted; }
	/**
	 * Stops the mixing thread and the page's output. `Mix` is not called
	 * afterwards.
	 */
	void Close();
	bool IsOpen() const { return m_pThread != nullptr; }
	void SetPaused(bool Paused);

private:
	static constexpr int CAPACITY = 8192;
	static inline bool ms_Wanted = true;

	SWebAudioRing *m_pRing = nullptr;
	void *m_pThread = nullptr;
	std::atomic<bool> m_Stop{false};
	std::atomic<bool> m_Paused{false};
	unsigned m_ChunkFrames = 0;
	unsigned m_TargetFrames = 0;
	FMix m_Mix;

	static void MixerThread(void *pUser);
	void RunMixer();
};

#endif
