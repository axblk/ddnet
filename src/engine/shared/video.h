#ifndef ENGINE_SHARED_VIDEO_H
#define ENGINE_SHARED_VIDEO_H

#include <base/types.h>

#include <cstdint>
#include <functional>
#include <memory>
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

/**
 * Whether @link VideoEncoders @endlink can be called without waiting.
 *
 * Finding out which encoders work here opens each of them once, and opening a
 * hardware encoder starts its driver, which takes seconds. Anything that asks
 * while a user is looking at it should wait for this instead of blocking the
 * frame it is drawn in.
 *
 * @return `true` once the list is built.
 */
bool VideoEncodersProbed();

/**
 * Builds the encoder list on a worker thread if that has not happened yet.
 *
 * @param pEngine Engine whose job pool runs the probe.
 */
void ProbeVideoEncoders(class IEngine *pEngine);

/**
 * Logs how the video export arguments are used.
 *
 * @param pUsageName Name of the program as the usage message should show it.
 */
void PrintVideoExportUsage(const char *pUsageName);

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

/**
 * Takes over the interrupt and termination signals while an export runs.
 *
 * An export writes its file as it goes, so whoever runs the export loop asks
 * `VideoExportInterrupted()` and cancels the encoder, which removes the
 * unfinished file. A second signal is not caught.
 */
void CatchVideoExportInterrupt();

/**
 * Whether a signal arrived since the last call. Reading it clears it.
 */
bool VideoExportInterrupted();

/**
 * Asks for the same stop as the interrupt signal, for a browser, which has
 * no signals.
 */
void InterruptVideoExport();

/**
 * The video export arguments of a command line: the demo, the output file,
 * `--list-codecs` and `--help`. Everything else about the video comes from the
 * `cl_video_*` settings, which the rest of the command line can set.
 */
class CCommandLineVideoExport
{
public:
	/**
	 * Whether a demo to export was named.
	 */
	bool m_Export = false;
	bool m_Help = false;
	bool m_ListCodecs = false;
	char m_aDemoPath[IO_MAX_PATH_LENGTH] = {};
	char m_aVideoPath[IO_MAX_PATH_LENGTH] = {};

	/**
	 * Takes the video export arguments off the command line and leaves the
	 * rest, in order, for the console.
	 *
	 * @param ArgumentCount Number of arguments, set to the number that is left.
	 * @param ppArguments The arguments, set to the ones that are left.
	 * @param vArguments Storage for the arguments that are left, which has to
	 * outlive the command line.
	 * @param pUsageName Name of the program in the usage message.
	 * @param AcceptPositional Whether the demo and the output file may be named
	 * without a flag.
	 *
	 * @return `false` when the arguments are invalid, which has been logged.
	 */
	bool ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments, const char *pUsageName, bool AcceptPositional = false);

	/**
	 * The settings of the export, read from the configuration. Call after the
	 * configuration and the command line were executed.
	 */
	static CVideoExportSettings Settings();
};

class CVideoExportStatus
{
public:
	uint64_t m_SubmittedFrames = 0;
	uint64_t m_EncodedFrames = 0;
	// Frames encoded per second over the last stretch, so that the number says
	// what the export is doing now rather than what it averaged since it began.
	float m_FramesPerSecond = 0.0f;
	bool m_HasError = false;
	char m_aError[256] = {};
};

class IVideo
{
public:
	virtual ~IVideo() = default;

	/**
	 * How long the video is going to be, so that the header written at the
	 * start of a streamed file is right from the first fragment. Only the
	 * browser's encoder uses it.
	 *
	 * @param Seconds How long the export will run, 0 where that is not known.
	 */
	virtual void SetExpectedDuration(float Seconds) {}

	virtual bool Start() = 0;
	virtual void Stop() = 0;
	/**
	 * Stops the recording and throws away what it produced so far.
	 */
	virtual void Cancel() = 0;
	virtual void Pause(bool Pause) = 0;
	virtual bool IsRecording() const = 0;
	/**
	 * Whether the recording has ended, either because it was stopped or because
	 * it failed. A stopped video produces no more frames and can be released.
	 */
	virtual bool IsStopped() const = 0;
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

/**
 * Prepares the linked video export before the first one is created.
 */
void InitVideoBackend();

/** Whether this build can encode video where it is running. A browser may not. */
bool VideoEncodingSupported();

/**
 * Creates the video export this build was linked with.
 *
 * @param pGraphics Graphics the frames are read back from.
 * @param pSound Sound the audio track is mixed from.
 * @param pStorage Storage the output file is created in.
 * @param Settings Resolution, rate and quality of the export.
 * @param LocalStartTime Time the exported timeline starts at.
 * @param pName Output file, in the given storage.
 * @param OutputStorageType Storage type the output path is relative to.
 * @param AllowOverwrite Whether an existing file may be replaced.
 * @param PauseLiveAudio Whether the sound device is silenced while the export
 * runs.
 *
 * @return The export, which still has to be started.
 */
std::unique_ptr<IVideo> CreateVideo(class IGraphics *pGraphics, class ISound *pSound, class IStorage *pStorage,
	CVideoExportSettings Settings, int64_t LocalStartTime, const char *pName, int OutputStorageType,
	bool AllowOverwrite, bool PauseLiveAudio);

#endif
