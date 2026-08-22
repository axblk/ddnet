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

	bool Start() override REQUIRES(!m_WriteLock);
	void Stop() override;
	void Cancel();
	void Pause(bool Pause) override;
	bool IsRecording() const override { return m_Recording; }
	bool IsStopped() const { return m_Stopped; }
	bool HasError() const override { return m_HasError.load(std::memory_order_acquire); }
	bool HasAudio() const override { return m_HasAudio; }
	CVideoExportStatus Status() const override NO_THREAD_SAFETY_ANALYSIS;

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
	void RunVideoThread(size_t ParentThreadIndex, size_t ThreadIndex) REQUIRES(!m_WriteLock);
	bool FillVideoFrame(size_t ThreadIndex) REQUIRES(!m_WriteLock);
	void SubmitVideoFrame(uint64_t FrameIndex, CImageInfo Image);
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

	void WriteFrame(COutputStream *pStream, size_t ThreadIndex) REQUIRES(m_WriteLock);
	void FinishFrames(COutputStream *pStream);
	void CloseStream(COutputStream *pStream);

	bool AddStream(COutputStream *pStream, AVFormatContext *pFormatContext, const AVCodec **ppCodec, enum AVCodecID CodecId);
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
	bool m_OffscreenFrameActive = false;
	std::atomic<bool> m_HasError = false;
	std::atomic<uint64_t> m_SubmittedFrames = 0;
	std::atomic<uint64_t> m_EncodedFrames = 0;
	mutable CLock m_StatusMutex;
	char m_aError[256] GUARDED_BY(m_StatusMutex) = {};
	// Roughly half a second of frames at 60 FPS, after which a dropped frame is
	// no longer a hiccup but a broken readback path.
	static constexpr int MAX_CONSECUTIVE_DROPPED_FRAMES = 30;
	int m_ConsecutiveDroppedFrames = 0;

	static constexpr size_t READBACK_SLOT_COUNT = 3;
	class CReadbackSlot
	{
	public:
		IGraphics::CTextureHandle m_Target;
		std::unique_ptr<IGraphics::ITextureReadback> m_pReadback;
		uint64_t m_FrameIndex = 0;
	};
	std::array<CReadbackSlot, READBACK_SLOT_COUNT> m_aReadbackSlots;
	size_t m_CurrentReadbackSlot = 0;

	CLock m_WriteLock;
	size_t m_VideoThreads = 2;
	size_t m_CurVideoThreadIndex = 0;
	size_t m_AudioThreads = 2;
	size_t m_CurAudioThreadIndex = 0;

	class CVideoRecorderThread
	{
	public:
		std::thread m_Thread;
		std::mutex m_Mutex;
		std::condition_variable m_Cond;

		bool m_Started = false;
		bool m_Finished = false;
		bool m_HasVideoFrame = false;

		std::mutex m_VideoFillMutex;
		std::condition_variable m_VideoFillCond;
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
