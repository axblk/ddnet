/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_DEMO_H
#define ENGINE_SHARED_DEMO_H

#include "snapshot.h"

#include <base/hash.h>

#include <engine/demo.h>
#include <engine/shared/protocol.h>

#include <functional>
#include <vector>

typedef std::function<void()> TUpdateIntraTimesFunc;

class CSnapshotDelta;
class IStorage;
#if defined(CONF_VIDEORECORDER)
class IVideo;
#endif

/**
 * Where a demo recorder that writes no file of its own puts its bytes, such as
 * the live stream of a server (see `CLiveRecorder`).
 */
class IDemoSink
{
public:
	virtual ~IDemoSink() = default;
	/**
	 * Takes the next bytes of the demo, in the order a file would have them.
	 *
	 * @param pData The bytes.
	 * @param Size How many.
	 */
	virtual void DemoWrite(const void *pData, size_t Size) = 0;
	/**
	 * The recorder stopped, nothing follows until it is started again.
	 */
	virtual void DemoStopped() = 0;
};

class CDemoRecorder : public IDemoRecorder
{
	IStorage *m_pStorage;

	IOHANDLE m_File;
	IDemoSink *m_pSink = nullptr;
	char m_aCurrentFilename[IO_MAX_PATH_LENGTH];
	int m_LastTickMarker;
	int m_LastKeyFrame;
	int m_FirstTick;

	CSnapshotBuffer m_LastSnapshotData;
	CSnapshotDelta *m_pSnapshotDelta;

	int m_NumTimelineMarkers;
	int m_aTimelineMarkers[MAX_TIMELINE_MARKERS];

	bool m_NoMapData;

	DEMOFUNC_FILTER m_pfnFilter;
	void *m_pUser;

	void WriteOut(const void *pData, size_t Size);
	void WriteHeader(const char *pNetVersion, const char *pMap, const SHA256_DIGEST &Sha256, unsigned MapCrc, const char *pType, unsigned MapSize);
	void WriteTickMarker(int Tick, bool Keyframe);
	bool Write(int Type, const void *pData, int Size);

public:
	CDemoRecorder(CSnapshotDelta *pSnapshotDelta, bool NoMapData = false);
	CDemoRecorder() = default;
	~CDemoRecorder() override;

	int Start(IStorage *pStorage, const char *pFilename, const char *pNetversion, const char *pMap, const SHA256_DIGEST &Sha256, unsigned MapCrc, const char *pType, unsigned MapSize, const unsigned char *pMapData, DEMOFUNC_FILTER pfnFilter, void *pUser);
	/**
	 * Starts recording into a sink instead of a file. The header and the map
	 * are the first bytes the sink gets, as in a file.
	 *
	 * @param pSink Takes the bytes until the recorder is stopped.
	 * @param pNetversion The network version the demo is of.
	 * @param pMap The name of the map.
	 * @param Sha256 The map's SHA256.
	 * @param MapCrc The map's CRC.
	 * @param pType What recorded the demo, such as `server`.
	 * @param MapSize How large the map is.
	 * @param pMapData The map, which goes into the demo.
	 * @param pfnFilter Leaves messages out of the demo, or `nullptr`.
	 * @param pUser Passed to `pfnFilter`.
	 */
	void Start(IDemoSink *pSink, const char *pNetversion, const char *pMap, const SHA256_DIGEST &Sha256, unsigned MapCrc, const char *pType, unsigned MapSize, const unsigned char *pMapData, DEMOFUNC_FILTER pfnFilter, void *pUser);
	int Stop(IDemoRecorder::EStopMode Mode, const char *pTargetFilename = "") override;
	/**
	 * Fills in what the header of a demo file only knows at the end: its
	 * length and its timeline markers.
	 *
	 * @param File The demo file, open for writing; where it stands afterwards is undefined.
	 * @param Length The length in seconds.
	 * @param pMarkers The ticks of the markers.
	 * @param NumMarkers How many, at most `MAX_TIMELINE_MARKERS`.
	 *
	 * @return Whether it was written.
	 */
	static bool WriteLengthAndMarkers(IOHANDLE File, int Length, const int *pMarkers, int NumMarkers);
	/**
	 * Makes the next snapshot a keyframe, which is where a demo can be cut.
	 */
	void ForceKeyframe() { m_LastKeyFrame = -1; }

	void AddDemoMarker();
	void AddDemoMarker(int Tick);

	void RecordSnapshot(int Tick, const void *pData, int Size);
	void RecordMessage(const void *pData, int Size);

	bool IsRecording() const override { return m_File != nullptr || m_pSink != nullptr; }
	const char *CurrentFilename() const override { return m_aCurrentFilename; }

	int Length() const override { return (m_LastTickMarker - m_FirstTick) / SERVER_TICK_SPEED; }
};

class CDemoPlayer : public IDemoPlayer
{
public:
	class IListener
	{
	public:
		virtual ~IListener() = default;
		virtual void OnDemoPlayerSnapshot(void *pData, int Size) = 0;
		virtual void OnDemoPlayerMessage(void *pData, int Size) = 0;
	};

	class CPlaybackInfo
	{
	public:
		CDemoHeader m_Header;
		CTimelineMarkers m_TimelineMarkers;

		IDemoPlayer::CInfo m_Info;

		int64_t m_LastUpdate;
		int64_t m_LastScan;
		int64_t m_CurrentTime;

		int m_NextTick;
		int m_PreviousTick;

		float m_IntraTick;
		float m_IntraTickSincePrev;
		float m_TickTime;

		bool m_LiveStateUpdating;
		int m_LiveStateFailedCount;
		int m_LiveStateUnchangedCount;
	};

private:
	IListener *m_pListener;

	TUpdateIntraTimesFunc m_UpdateIntraTimesFunc;

	// Playback
	class CKeyFrame
	{
	public:
		int64_t m_Filepos;
		int m_Tick;

		CKeyFrame(int64_t Filepos, int Tick) :
			m_Filepos(Filepos), m_Tick(Tick)
		{
		}
	};

	IOHANDLE m_File;
	int64_t m_MapOffset;
	char m_aFilename[IO_MAX_PATH_LENGTH];
	char m_aErrorMessage[256];
	std::vector<CKeyFrame> m_vKeyFrames;
	CMapInfo m_MapInfo;
	int m_SpeedIndex;

	CPlaybackInfo m_Info;
	unsigned char m_aCompressedSnapshotData[CSnapshot::MAX_SIZE];
	unsigned char m_aDecompressedSnapshotData[CSnapshot::MAX_SIZE];

	// Depending on the chunk header
	// this is either a full CSnapshot or a CSnapshotDelta.
	unsigned char m_aChunkData[CSnapshot::MAX_SIZE];
	// Storage for the full snapshot
	// where the delta gets unpacked into.
	CSnapshotBuffer m_Snapshot;
	CSnapshotBuffer m_LastSnapshotData;
	int m_LastSnapshotDataSize;
	CSnapshotDelta *m_pSnapshotDelta;
	CSnapshotDelta *m_pSnapshotDeltaSixup;

	bool m_UseVideo;
	// Somebody else keeps appending to the file, see `SetLive`.
	bool m_LiveHeld = false;
#if defined(CONF_VIDEORECORDER)
	IVideo *m_pVideo = nullptr;
	bool m_WasRecording = false;
#endif

	enum EReadChunkHeaderResult
	{
		CHUNKHEADER_SUCCESS,
		CHUNKHEADER_ERROR,
		CHUNKHEADER_EOF,
	};
	EReadChunkHeaderResult ReadChunkHeader(int *pType, int *pSize, int *pTick);
	void DoTick();
	enum class EScanFileResult
	{
		SUCCESS,
		ERROR_RECOVERABLE,
		ERROR_UNRECOVERABLE,
	};
	EScanFileResult ScanFile();
	void UpdateTimes();

	int64_t Time();
	bool m_Sixup;

	CSnapshotDelta *SnapshotDelta();
	void Construct(CSnapshotDelta *pSnapshotDelta, CSnapshotDelta *pSnapshotDeltaSixup, bool UseVideo);

public:
	CDemoPlayer(CSnapshotDelta *pSnapshotDelta, CSnapshotDelta *pSnapshotDeltaSixup, bool UseVideo);
	CDemoPlayer(CSnapshotDelta *pSnapshotDelta, CSnapshotDelta *pSnapshotDeltaSixup, bool UseVideo, TUpdateIntraTimesFunc &&UpdateIntraTimesFunc);
	~CDemoPlayer() override;

	void SetListener(IListener *pListener);
#if defined(CONF_VIDEORECORDER)
	void SetVideo(IVideo *pVideo);
	IVideo *Video() const { return m_pVideo; }
#endif

	int Load(IStorage *pStorage, const char *pFilename, int StorageType);
	unsigned char *GetMapData(IStorage *pStorage);
	bool ExtractMap(IStorage *pStorage);
	void Play();
	void Pause() override;
	void Unpause() override;
	void Stop(const char *pErrorMessage = "");
	void SetSpeed(float Speed) override;
	void SetSpeedIndex(int SpeedIndex) override;
	void AdjustSpeedIndex(int Offset) override;
	bool SeekPercent(float Percent) override;
	bool SeekTime(float Seconds) override;
	bool SeekTick(ETickOffset TickOffset) override;
	bool SetPos(int WantedTick) override;
	const CInfo *BaseInfo() const override { return &m_Info.m_Info; }
	void GetDemoName(char *pBuffer, size_t BufferSize) const override;
	bool GetDemoInfo(IStorage *pStorage, const char *pFilename, int StorageType, CDemoHeader *pDemoHeader, CTimelineMarkers *pTimelineMarkers, CMapInfo *pMapInfo, IOHANDLE *pFile = nullptr, char *pErrorMessage = nullptr, size_t ErrorMessageSize = 0) const override;
	const char *Filename() const { return m_aFilename; }
	const char *ErrorMessage() const override { return m_aErrorMessage; }

	void Update(bool RealTime = true);
	/**
	 * Says whether the file still grows, rather than guessing it from whether
	 * it grew lately. A live demo plays up to its last whole tick and waits
	 * there for the next one; seeking stays two seconds before its end. A
	 * demo that stops being live plays to its end like any other.
	 *
	 * @param Live Whether more is appended to the file.
	 */
	void SetLive(bool Live);
	bool IsSixup() const { return m_Sixup; }

	const CPlaybackInfo *Info() const { return &m_Info; }
	bool IsPlaying() const override { return m_File != nullptr; }
	const CMapInfo *GetMapInfo() const { return &m_MapInfo; }
};

class CDemoEditor : public IDemoEditor
{
	IStorage *m_pStorage;
	CSnapshotDelta *m_pSnapshotDelta;
	CSnapshotDelta *m_pSnapshotDeltaSixup;

public:
	virtual void Init(CSnapshotDelta *pSnapshotDelta, CSnapshotDelta *pSnapshotDeltaSixup, IStorage *pStorage);
	bool Slice(const char *pDemo, const char *pDst, int StartTick, int EndTick, DEMOFUNC_FILTER pfnFilter, void *pUser) override;
};

#endif
