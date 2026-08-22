#ifndef ENGINE_CLIENT_VIDEO_H
#define ENGINE_CLIENT_VIDEO_H

#include <base/lock.h>
#include <base/types.h>

#include <engine/graphics.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
};

#include <engine/shared/video.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

class ISound;
class IStorage;

// a wrapper around a single output AVStream
class COutputStream
{
public:
	AVStream *m_pStream = nullptr;
	AVCodecContext *m_pCodecContext = nullptr;

	/* pts of the next frame that will be generated */
	int64_t m_SamplesCount = 0;
	int64_t m_SamplesFrameCount = 0;

	std::vector<AVFrame *> m_vpFrames;
	std::vector<AVFrame *> m_vpTmpFrames;

	std::vector<struct SwsContext *> m_vpSwsContexts;
	std::vector<struct SwrContext *> m_vpSwrContexts;
};

class CVideo : public IVideo
{
public:
	CVideo(IGraphics *pGraphics, ISound *pSound, IStorage *pStorage, CVideoExportSettings Settings, int64_t LocalStartTime, const char *pName, int OutputStorageType, bool AllowOverwrite, bool PauseLiveAudio);
	~CVideo() override;

	bool Start() override REQUIRES(!m_WriteLock, !m_StatusMutex);
	void Stop() override;
	void Cancel();
	void Pause(bool Pause) override;
	bool IsRecording() const override { return m_Recording; }
	bool IsStopped() const { return m_Stopped; }
	bool HasError() const override { return m_HasError.load(std::memory_order_acquire); }
	bool HasAudio() const override { return m_HasAudio; }
	CVideoExportStatus Status() const override NO_THREAD_SAFETY_ANALYSIS;
	const CVideoExportSettings &Settings() const override { return m_Settings; }

	void NextVideoFrame() override;
	bool BeginVideoFrameRender() override;
	void EndVideoFrameRender() override;

	void NextAudioFrame(ISoundMixFunc Mix) override;
	void NextAudioFrameTimeline(ISoundMixFunc Mix) override;

	int64_t Time() const override { return m_Time; }
	float LocalTime() const override { return m_LocalTime; }
	void SetLocalStartTime(int64_t LocalStartTime) override { m_LocalStartTime = LocalStartTime; }

	static void Init();

private:
	void RunVideoThread(size_t ThreadIndex) REQUIRES(!m_WriteLock);
	bool FillVideoFrame(size_t ThreadIndex, size_t FrameIndex) REQUIRES(!m_WriteLock);
	void SubmitVideoFrame(CImageInfo Image);
	bool FinishReadbackSlot(size_t SlotIndex);
	bool DrainReadbackSlots();
	bool CreateOffscreenTargets();
	void DestroyOffscreenTargets();
	void DisableOffscreen(const char *pReason);

	void RunAudioThread(size_t ParentThreadIndex, size_t ThreadIndex) REQUIRES(!m_WriteLock);
	bool FillAudioFrame(size_t ThreadIndex);

	bool OpenVideo();
	bool OpenAudio();
	AVFrame *AllocPicture(enum AVPixelFormat PixFmt, int Width, int Height);
	AVFrame *AllocAudioFrame(enum AVSampleFormat SampleFmt, uint64_t ChannelLayout, int SampleRate, int NbSamples);

	void WriteFrame(COutputStream *pStream, size_t FrameIndex, int64_t Pts) REQUIRES(!m_WriteLock);
	void FinishFrames(COutputStream *pStream);
	void CloseStream(COutputStream *pStream);

	bool AddStream(COutputStream *pStream, AVFormatContext *pFormatContext, const AVCodec **ppCodec, const AVCodec *pCodec);
	[[gnu::format(printf, 2, 3)]] void SetError(const char *pFormat, ...) NO_THREAD_SAFETY_ANALYSIS;
	void SetAvError(const char *pOperation, int Error);

	IGraphics *m_pGraphics;
	IStorage *m_pStorage;
	ISound *m_pSound;

	const CVideoExportSettings m_Settings;
	const int m_OutputStorageType;
	const bool m_AllowOverwrite;
	char m_aName[IO_MAX_PATH_LENGTH];
	char m_aTemporaryName[IO_MAX_PATH_LENGTH];
	uint64_t m_VideoFrameIndex = 0;
	uint64_t m_AudioFrameIndex = 0;

	int64_t m_TickTime;
	int64_t m_LocalStartTime;
	float m_LocalTime;
	int64_t m_Time;

	bool m_Started;
	bool m_Stopped;
	bool m_Recording;
	bool m_Cancelled = false;
	bool m_Offscreen = false;
	// The backend hands the frames over already converted, so no thread of ours
	// has to touch a pixel between the graphics card and the encoder.
	bool m_YuvReadback = false;
	IGraphics::EPlanarYuvFormat m_YuvFormat = IGraphics::EPlanarYuvFormat::NV12;
	std::chrono::nanoseconds m_ExportStartTime{0};
	bool m_OffscreenFrameActive = false;
	std::atomic<bool> m_HasError = false;
	std::atomic<uint64_t> m_SubmittedFrames = 0;
	std::atomic<uint64_t> m_EncodedFrames = 0;
	mutable CLock m_StatusMutex;
	char m_aError[256] GUARDED_BY(m_StatusMutex) = {};
	// Sampled once per exported frame, so that the number does not depend on how
	// many places happen to be asking for the status.
	void UpdateFrameRate() NO_THREAD_SAFETY_ANALYSIS;
	static constexpr std::chrono::milliseconds RATE_SAMPLE_INTERVAL{500};
	std::chrono::nanoseconds m_RateSampleTime GUARDED_BY(m_StatusMutex){0};
	uint64_t m_RateSampleFrames GUARDED_BY(m_StatusMutex) = 0;
	float m_FramesPerSecond GUARDED_BY(m_StatusMutex) = 0.0f;
	// Roughly half a second of frames at 60 FPS, after which a dropped frame is
	// no longer a hiccup but a broken readback path.
	static constexpr int MAX_CONSECUTIVE_DROPPED_FRAMES = 30;
	int m_ConsecutiveDroppedFrames = 0;

	static constexpr size_t READBACK_SLOT_COUNT = 3;
	class CReadbackSlot
	{
	public:
		IGraphics::CTextureHandle m_Target;
		// Only set while the graphics backend converts frames itself: a quarter
		// as wide and half again as tall, holding four YUV bytes per pixel.
		IGraphics::CTextureHandle m_YuvTarget;
		std::unique_ptr<IGraphics::ITextureReadback> m_pReadback;
		uint64_t m_FrameIndex = 0;
	};
	std::array<CReadbackSlot, READBACK_SLOT_COUNT> m_aReadbackSlots;
	size_t m_CurrentReadbackSlot = 0;
	// A read frame is eight megabytes at 1080p. Asking the system for that much
	// sixty times a second costs more than filling it does, so the frames come
	// back from the encoder threads and are handed to the next read instead.
	std::mutex m_RecycledImageMutex;
	std::vector<CImageInfo> m_vRecycledImages;
	CImageInfo TakeRecycledImage();
	void RecycleImage(CImageInfo &&Image);

	// Only the file is shared between the streams, and only while a finished
	// packet goes into it. An encoder takes one frame at a time, but there is
	// one per stream, so the sound never waits behind the picture.
	CLock m_WriteLock;
	std::mutex m_VideoEncodeMutex;
	std::mutex m_AudioEncodeMutex;
	// Upper bound for the configurable encoder thread budget
	static constexpr int MAX_ENCODE_THREADS = 64;
	int m_EncodeThreads = 1;
	static constexpr size_t MAX_VIDEO_THREADS = 8;
	size_t m_VideoThreads = 2;
	// A frame goes to whichever encoder thread is free, not to the next one in
	// turn: the frames cost different amounts of work, and handing them out in
	// a fixed rotation makes every one of them wait for the slowest thread of
	// the round. The order the encoder needs comes back further down, where the
	// finished frames are written.
	std::mutex m_VideoDispatchMutex;
	std::condition_variable m_VideoDispatchCond;
	std::vector<size_t> m_vFreeVideoThreads;
	uint64_t m_VideoDispatchSequence = 0;
	// One thread writes, so that no encoder thread has to be woken for its
	// turn: waking the right one of eight per frame costs more than the write
	// itself, and the frames arrive nearly in order anyway. A converted frame
	// is left here and its thread goes straight back to work.
	std::mutex m_VideoWriteMutex;
	std::condition_variable m_VideoWriteCond;
	// A sequence is outstanding from the moment a converting thread is handed it
	// until the writer has taken it out again, and it then either belongs to a
	// thread or holds a frame of the pool. One per thread plus one per pool
	// frame is therefore all that can be waiting, and a ring that size is never
	// overtaken, so the sequence can index it directly. Leaving a frame here
	// costs nothing that way, which a tree node did not.
	static constexpr size_t NO_PENDING_WRITE = (size_t)-1;
	std::array<size_t, 2 * MAX_VIDEO_THREADS + 2> m_aPendingVideoWrites;
	uint64_t m_NextVideoFrameToWrite = 0;
	bool m_VideoWriterFinished = false;
	std::thread m_VideoWriterThread;
	// Frames the converting threads fill and the writer hands back. There are
	// more of them than there are threads, so that a thread that is done never
	// waits for the writer to catch up.
	std::mutex m_VideoFrameMutex;
	std::condition_variable m_VideoFrameCond;
	std::vector<size_t> m_vFreeVideoFrames;
	void RunVideoWriterThread() REQUIRES(!m_WriteLock);
	size_t m_AudioThreads = 2;
	size_t m_CurAudioThreadIndex = 0;

	class CVideoRecorderThread
	{
	public:
		std::thread m_Thread;
		std::mutex m_Mutex;
		std::condition_variable m_Cond;

		bool m_Started = false;
		// Also read while a thread waits for a free frame, which is a wait
		// under a different lock than the one this is set under.
		std::atomic<bool> m_Finished = false;
		bool m_HasVideoFrame = false;
		uint64_t m_VideoFrameToFill = 0;
	};

	std::vector<std::unique_ptr<CVideoRecorderThread>> m_vpVideoThreads;

	class CAudioRecorderThread
	{
	public:
		std::thread m_Thread;
		std::mutex m_Mutex;
		std::condition_variable m_Cond;

		bool m_Started = false;
		bool m_Finished = false;
		bool m_HasAudioFrame = false;

		std::mutex m_AudioFillMutex;
		std::condition_variable m_AudioFillCond;
		uint64_t m_AudioFrameToFill = 0;
		int64_t m_SampleCountStart = 0;
	};

	std::vector<std::unique_ptr<CAudioRecorderThread>> m_vpAudioThreads;

	std::atomic<int32_t> m_ProcessingVideoFrame;
	std::atomic<int32_t> m_ProcessingAudioFrame;

	bool m_HasAudio;
	bool m_PauseLiveAudio;

	class CVideoBuffer
	{
	public:
		CImageInfo m_Image;
	};
	std::vector<CVideoBuffer> m_vVideoBuffers;
	class CAudioBuffer
	{
	public:
		int16_t m_aBuffer[4096];
	};
	std::vector<CAudioBuffer> m_vAudioBuffers;

	COutputStream m_VideoStream;
	COutputStream m_AudioStream;

	const AVCodec *m_pVideoCodec;
	const AVCodec *m_pAudioCodec;

	AVDictionary *m_pOptDict;

	AVFormatContext *m_pFormatContext;
	const AVOutputFormat *m_pFormat;
};

#endif
