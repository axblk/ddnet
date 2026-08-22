#ifndef ENGINE_SHARED_VIDEO_H
#define ENGINE_SHARED_VIDEO_H

#include <cstdint>
#include <functional>

typedef std::function<void(short *pFinalOut, unsigned Frames)> ISoundMixFunc;

class CVideoExportSettings
{
public:
	int m_Width = 0;
	int m_Height = 0;
	int m_FPS = 60;
	bool m_Audio = true;
	int m_Crf = 18;
	int m_Preset = 5;
};

class CVideoExportStatus
{
public:
	uint64_t m_SubmittedFrames = 0;
	uint64_t m_EncodedFrames = 0;
	bool m_HasError = false;
	char m_aError[256] = {};
};

class IVideo
{
public:
	virtual ~IVideo() = default;

	virtual bool Start() = 0;
	virtual void Stop() = 0;
	virtual void Pause(bool Pause) = 0;
	virtual bool IsRecording() const = 0;
	virtual bool HasError() const = 0;
	virtual bool HasAudio() const = 0;
	virtual CVideoExportStatus Status() const = 0;

	virtual void NextVideoFrame() = 0;
	virtual bool BeginVideoFrameRender() = 0;
	virtual void EndVideoFrameRender() = 0;

	virtual void NextAudioFrame(ISoundMixFunc Mix) = 0;
	virtual void NextAudioFrameTimeline(ISoundMixFunc Mix) = 0;

	virtual int64_t Time() const = 0;
	virtual float LocalTime() const = 0;
	virtual void SetLocalStartTime(int64_t LocalStartTime) = 0;

	static IVideo *Current() { return ms_pCurrentVideo; }

protected:
	static IVideo *ms_pCurrentVideo;
};

#endif
