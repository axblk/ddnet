#ifndef ENGINE_SHARED_VIDEO_H
#define ENGINE_SHARED_VIDEO_H

#include <cstdint>
#include <functional>
#include <vector>

typedef std::function<void(short *pFinalOut, unsigned Frames)> ISoundMixFunc;

/**
 * A video encoder that the linked libavcodec provides.
 */
class CVideoEncoder
{
public:
	/**
	 * Encoder name as libavcodec knows it, empty for the container default.
	 */
	char m_aName[32] = {};
	/**
	 * Name shown to the user.
	 */
	char m_aDisplayName[64] = {};
};

/**
 * Lists the video encoders that are both known to us and actually present in
 * the linked libavcodec, in the order in which they should be offered. The
 * first entry is always available and is used when no encoder was chosen.
 *
 * @return Reference to the list, which is built once and never changes.
 */
const std::vector<CVideoEncoder> &VideoEncoders();

class CVideoExportSettings
{
public:
	int m_Width = 0;
	int m_Height = 0;
	int m_FPS = 60;
	bool m_Audio = true;
	int m_Crf = 18;
	int m_Preset = 5;
	/**
	 * Encoder name from @link VideoEncoders @endlink, empty for the default.
	 */
	char m_aVideoCodec[32] = {};
	/**
	 * Hardware threads the encoder may use, 0 to determine it automatically.
	 */
	int m_EncodeThreads = 0;
	bool m_ShowHud = false;
	bool m_ShowChat = true;
	bool m_ShowHookCollOther = false;
	int m_ShowDirection = 0;
	bool m_ShowImportantAlerts = true;
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
	virtual const CVideoExportSettings &Settings() const = 0;

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
