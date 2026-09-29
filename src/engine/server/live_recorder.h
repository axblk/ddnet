/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SERVER_LIVE_RECORDER_H
#define ENGINE_SERVER_LIVE_RECORDER_H

#include <base/hash.h>
#include <base/types.h>

#include <engine/shared/demo.h>

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <vector>

class IStorage;

/**
 * Streams the server demo into a directory that any static web server can
 * hand out, the way a HLS muxer writes a playlist and its segments:
 *
 * - `init-<epoch>`: the demo header and the map, once per map (epoch);
 * - `seg-<n>`: the demo from one keyframe to the next cut, the last one grows;
 * - `index.json`: the epochs, the segments with their ticks and sizes, the
 *   markers and whether the stream is still live, replaced atomically. Its
 *   sizes are in bytes, also the limit `max_bytes`, which `sv_live_max_size`
 *   sets in KiB.
 *
 * An `init` followed by the segments of its epoch, oldest first, is a valid
 * demo, also after old segments were deleted. Nothing younger than the delay
 * is ever written: what the demo recorder hands over is held back in memory
 * until it is old enough.
 */
class CLiveRecorder : public IDemoSink
{
public:
	/**
	 * How a stream is written, taken when it starts.
	 */
	class CSettings
	{
	public:
		/**
		 * How long everything is held back before it is written.
		 */
		std::chrono::nanoseconds m_Delay{0};
		/**
		 * After how many seconds a segment is cut at the next snapshot.
		 */
		int m_SegmentSeconds = 10;
		/**
		 * How many seconds of segments are kept at most, 0 for no limit.
		 */
		int m_MaxDurationSeconds = 0;
		/**
		 * How many bytes of segments are kept at most, 0 for no limit.
		 */
		int64_t m_MaxBytes = 0;
	};

	enum class EMarkerKind
	{
		MATCH_END,
		MANUAL,
	};

private:
	enum class EItemKind
	{
		EPOCH,
		SEGMENT,
		DATA,
		MARKER,
		EPOCH_END,
	};

	// Something the demo recorder produced, waiting for the delay to pass.
	class CItem
	{
	public:
		EItemKind m_Kind;
		std::chrono::nanoseconds m_Time;
		int64_t m_WallTime;
		int m_Tick;
		int m_Epoch;
		std::vector<unsigned char> m_vData;
		EMarkerKind m_MarkerKind = EMarkerKind::MANUAL;
		std::string m_Text;
		SHA256_DIGEST m_Sha256 = {};
	};

	class CEpoch
	{
	public:
		int m_Id;
		std::string m_Map;
		SHA256_DIGEST m_Sha256;
		int64_t m_InitBytes;
	};

	class CSegment
	{
	public:
		int m_Number;
		int m_Epoch;
		int m_StartTick;
		int m_EndTick;
		int64_t m_Bytes;
		int64_t m_WallTime;
		bool m_Complete;
	};

	class CMarker
	{
	public:
		int m_Epoch;
		int m_Tick;
		EMarkerKind m_Kind;
		std::string m_Label;
	};

	enum class EState
	{
		OFF,
		LIVE,
		DRAINING,
	};

	CDemoRecorder *m_pEncoder;
	IStorage *m_pStorage = nullptr;

	EState m_State = EState::OFF;
	CSettings m_Settings;
	char m_aName[128] = "";
	char m_aDir[IO_MAX_PATH_LENGTH] = "";
	int64_t m_StreamId = 0;
	std::chrono::nanoseconds m_StartTime{0};
	std::chrono::nanoseconds m_LastNow{0};
	bool m_Keep = false;
	char m_aStopReason[32] = "";

	// What the demo recorder hands over goes to the init of the epoch that is
	// starting, or into the pending data.
	bool m_CapturingInit = false;
	std::vector<unsigned char> m_vInit;
	std::vector<unsigned char> m_vPending;
	int m_CaptureEpoch = -1;
	int m_SegmentStartTick = -1;
	int m_LastTick = -1;
	std::optional<bool> m_GameOver;
	std::deque<CItem> m_vQueue;
	int64_t m_QueueBytes = 0;

	// What is on disk.
	int m_NextSegment = 0;
	std::vector<CEpoch> m_vEpochs;
	std::vector<CSegment> m_vSegments;
	std::vector<CMarker> m_vMarkers;
	IOHANDLE m_SegmentFile = nullptr;
	bool m_SegmentDirty = false;
	bool m_IndexDirty = false;
	bool m_IndexStale = false;
	std::chrono::nanoseconds m_LastIndexWrite{0};
	int64_t m_WrittenBytes = 0;
	bool m_WriteFailed = false;

	void Path(char *pBuffer, size_t BufferSize, const char *pFile) const;
	void ClearDirectory();
	void Push(EItemKind Kind, int Tick, std::chrono::nanoseconds Now);
	void PushPending(std::chrono::nanoseconds Now);
	void Apply(CItem &Item);
	void CloseSegment();
	void EnforceLimits();
	bool WriteFileAtomically(const char *pFile, const void *pData, size_t Size);
	// false when the index could not be replaced; it stays due, so the next update tries again
	bool WriteIndex(std::chrono::nanoseconds Now);
	void WriteKeepDemos();
	void Finish();
	void Reset();

public:
	/**
	 * @param pEncoder The recorder that makes the demo, `RECORDER_LIVE` of
	 * the server. It writes into this while a stream runs.
	 */
	explicit CLiveRecorder(CDemoRecorder *pEncoder);
	~CLiveRecorder() override;

	/**
	 * Starts a stream in `<pParentDir>/<pName>` of the save directory. Files of
	 * an earlier stream of that name are removed.
	 *
	 * @param pStorage Where the files go.
	 * @param pParentDir The directory the streams are in, relative to the save directory.
	 * @param pName The name of the stream, which is its directory.
	 * @param Settings How it is written.
	 * @param Now The current time, `time_get_nanoseconds`.
	 * @param pError Says what went wrong.
	 * @param ErrorSize The size of `pError`.
	 *
	 * @return Whether it started. The demo only begins with `BeginEpoch`.
	 */
	bool Start(IStorage *pStorage, const char *pParentDir, const char *pName, const CSettings &Settings, std::chrono::nanoseconds Now, char *pError, size_t ErrorSize);
	/**
	 * Starts the demo of a map, a new epoch of the stream.
	 *
	 * @param pNetVersion The network version.
	 * @param pMap The name of the map.
	 * @param Sha256 The map's SHA256.
	 * @param MapCrc The map's CRC.
	 * @param MapSize How large the map is.
	 * @param pMapData The map.
	 * @param Now The current time.
	 */
	void BeginEpoch(const char *pNetVersion, const char *pMap, const SHA256_DIGEST &Sha256, unsigned MapCrc, unsigned MapSize, const unsigned char *pMapData, std::chrono::nanoseconds Now);
	/**
	 * Records the snapshot of a tick, cuts a segment when it is due and notes
	 * the end of a match (`GAMESTATEFLAG_GAMEOVER` coming on).
	 *
	 * @param Tick The tick.
	 * @param pData The snapshot.
	 * @param Size Its size.
	 * @param Now The current time.
	 */
	void RecordSnapshot(int Tick, const void *pData, int Size, std::chrono::nanoseconds Now);
	/**
	 * Marks the last recorded tick.
	 *
	 * @param Kind Why.
	 * @param pLabel What a manual marker says, may be empty.
	 * @param Now The current time.
	 *
	 * @return The tick, or -1 if nothing was recorded yet.
	 */
	int AddMarker(EMarkerKind Kind, const char *pLabel, std::chrono::nanoseconds Now);
	/**
	 * Stops recording. What is still held back is written as its delay
	 * passes, then the stream ends.
	 *
	 * @param Keep Whether a normal demo is written to `demos/` at the end.
	 * @param pReason Why, for the log.
	 */
	void Stop(bool Keep, const char *pReason);
	/**
	 * Ends the stream at once. What is still held back is dropped.
	 *
	 * @param pReason Why, for the log.
	 */
	void Abort(const char *pReason);
	/**
	 * Writes what is old enough, cuts old segments to the limits and brings
	 * the index up to date. Called every tick.
	 *
	 * @param Now The current time.
	 */
	void Update(std::chrono::nanoseconds Now);

	/**
	 * Whether a stream records.
	 */
	bool IsLive() const { return m_State == EState::LIVE; }
	/**
	 * Whether a stream records or still writes what it held back.
	 */
	bool IsActive() const { return m_State != EState::OFF; }
	/**
	 * Whether the stream waits for the demo of a map to begin.
	 */
	bool NeedsEpoch() const { return m_State == EState::LIVE && !m_pEncoder->IsRecording(); }
	/**
	 * The stream's directory relative to the save directory.
	 */
	const char *Directory() const { return m_aDir; }
	const char *Name() const { return m_aName; }
	/**
	 * One line on how the stream is, for `live_status`.
	 *
	 * @param pBuffer Takes the line.
	 * @param BufferSize Its size.
	 * @param Now The current time.
	 */
	void Status(char *pBuffer, size_t BufferSize, std::chrono::nanoseconds Now) const;

	/**
	 * The number of segments on disk.
	 */
	int NumSegments() const { return (int)m_vSegments.size(); }
	/**
	 * The bytes of the segments on disk.
	 */
	int64_t SegmentBytes() const;
	/**
	 * The seconds of demo in the segments on disk.
	 */
	float WindowSeconds() const;
	/**
	 * How many bytes are held back in memory.
	 */
	int64_t QueuedBytes() const { return m_QueueBytes + (int64_t)m_vPending.size(); }

	void DemoWrite(const void *pData, size_t Size) override;
	void DemoStopped() override;
};

#endif
