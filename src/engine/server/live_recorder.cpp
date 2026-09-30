/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "live_recorder.h"

#include <base/dbg.h>
#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>

#include <engine/shared/jsonwriter.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <algorithm>
#include <thread>

namespace
{
	// The index says how the stream grows at most this often; a new segment,
	// a marker or the end are said at once.
	constexpr std::chrono::nanoseconds INDEX_INTERVAL = std::chrono::seconds(1);

	int64_t WallMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}

	const char *MarkerKindName(CLiveRecorder::EMarkerKind Kind)
	{
		switch(Kind)
		{
		case CLiveRecorder::EMarkerKind::MATCH_END: return "match_end";
		case CLiveRecorder::EMarkerKind::MANUAL: return "manual";
		}
		dbg_assert_failed("Invalid marker kind");
	}

	// Files of a stream, which a stream of the same name removes.
	bool IsStreamFile(const char *pName)
	{
		return str_startswith(pName, "init-") || str_startswith(pName, "seg-") || str_comp(pName, "index.json") == 0 || str_comp(pName, "index.json.tmp") == 0;
	}

	bool CopyFileInto(IStorage *pStorage, const char *pFilename, IOHANDLE Destination)
	{
		IOHANDLE Source = pStorage->OpenFile(pFilename, IOFLAG_READ, IStorage::TYPE_SAVE);
		if(!Source)
			return false;
		bool Ok = true;
		unsigned char aChunk[64 * 1024];
		while(true)
		{
			const unsigned Bytes = io_read(Source, aChunk, sizeof(aChunk));
			if(Bytes == 0)
				break;
			if(io_write(Destination, aChunk, Bytes) != Bytes)
			{
				Ok = false;
				break;
			}
		}
		io_close(Source);
		return Ok;
	}
} // namespace

CLiveRecorder::CLiveRecorder(CDemoRecorder *pEncoder) :
	m_pEncoder(pEncoder)
{
}

CLiveRecorder::~CLiveRecorder()
{
	Abort("shutdown");
}

void CLiveRecorder::Path(char *pBuffer, size_t BufferSize, const char *pFile) const
{
	str_format(pBuffer, BufferSize, "%s/%s", m_aDir, pFile);
}

void CLiveRecorder::ClearDirectory()
{
	std::vector<std::string> vFiles;
	m_pStorage->ListDirectory(IStorage::TYPE_SAVE, m_aDir, [](const char *pName, int IsDir, int DirType, void *pUser) {
		if(!IsDir && IsStreamFile(pName))
			static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName);
		return 0; }, &vFiles);
	for(const std::string &File : vFiles)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		Path(aPath, sizeof(aPath), File.c_str());
		m_pStorage->RemoveFile(aPath, IStorage::TYPE_SAVE);
	}
}

bool CLiveRecorder::Start(IStorage *pStorage, const char *pParentDir, const char *pName, const CSettings &Settings, std::chrono::nanoseconds Now, char *pError, size_t ErrorSize)
{
	dbg_assert(m_State == EState::OFF, "Live recorder already running");
	if(pName[0] == '\0' || str_length(pName) >= (int)sizeof(m_aName) || !str_valid_filename(pName) || str_find(pName, "/") || str_find(pName, "\\") || pName[0] == '.')
	{
		str_format(pError, ErrorSize, "invalid stream name '%s'", pName);
		return false;
	}
	if(pParentDir[0] == '\0' || !fs_is_relative_path(pParentDir) || str_find(pParentDir, ".."))
	{
		str_format(pError, ErrorSize, "sv_live_dir must be a relative path without '..', not '%s'", pParentDir);
		return false;
	}

	m_pStorage = pStorage;
	m_Settings = Settings;
	m_Settings.m_SegmentSeconds = std::max(m_Settings.m_SegmentSeconds, 1);
	str_copy(m_aName, pName);
	str_format(m_aDir, sizeof(m_aDir), "%s/%s", pParentDir, pName);

	// The parent may be several folders deep.
	char aFolder[IO_MAX_PATH_LENGTH];
	for(const char *pSeparator = str_find(m_aDir, "/"); pSeparator != nullptr; pSeparator = str_find(pSeparator + 1, "/"))
	{
		str_truncate(aFolder, sizeof(aFolder), m_aDir, pSeparator - m_aDir);
		m_pStorage->CreateFolder(aFolder, IStorage::TYPE_SAVE);
	}
	if(!m_pStorage->CreateFolder(m_aDir, IStorage::TYPE_SAVE) && !m_pStorage->FolderExists(m_aDir, IStorage::TYPE_SAVE))
	{
		str_format(pError, ErrorSize, "could not create '%s'", m_aDir);
		Reset();
		return false;
	}
	ClearDirectory();

	m_State = EState::LIVE;
	m_StreamId = WallMilliseconds();
	m_StartTime = Now;
	m_LastNow = Now;
	WriteIndex(Now);
	if(m_WriteFailed)
	{
		str_format(pError, ErrorSize, "could not write into '%s'", m_aDir);
		Reset();
		return false;
	}

	// The directory in the storage, as `live_status` says it, not where the storage is on the host.
	log_info("live", "started name=%s dir=%s", m_aName, m_aDir);
	return true;
}

void CLiveRecorder::BeginEpoch(const char *pNetVersion, const char *pMap, const SHA256_DIGEST &Sha256, unsigned MapCrc, unsigned MapSize, const unsigned char *pMapData, std::chrono::nanoseconds Now)
{
	if(!NeedsEpoch())
		return;
	m_LastNow = Now;
	m_CaptureEpoch++;
	m_SegmentStartTick = -1;
	m_LastTick = -1;
	m_GameOver.reset();
	m_vPending.clear();

	m_CapturingInit = true;
	m_vInit.clear();
	m_pEncoder->Start(this, pNetVersion, pMap, Sha256, MapCrc, "server", MapSize, pMapData, nullptr, nullptr);
	m_CapturingInit = false;

	Push(EItemKind::EPOCH, -1, Now);
	CItem &Item = m_vQueue.back();
	Item.m_vData = std::move(m_vInit);
	Item.m_Text = pMap;
	Item.m_Sha256 = Sha256;
	m_QueueBytes += Item.m_vData.size();
	m_vInit.clear();
}

void CLiveRecorder::DemoWrite(const void *pData, size_t Size)
{
	std::vector<unsigned char> &vTarget = m_CapturingInit ? m_vInit : m_vPending;
	const unsigned char *pBytes = static_cast<const unsigned char *>(pData);
	vTarget.insert(vTarget.end(), pBytes, pBytes + Size);
}

void CLiveRecorder::DemoStopped()
{
	PushPending(m_LastNow);
	Push(EItemKind::EPOCH_END, m_LastTick, m_LastNow);
}

void CLiveRecorder::Push(EItemKind Kind, int Tick, std::chrono::nanoseconds Now)
{
	CItem &Item = m_vQueue.emplace_back();
	Item.m_Kind = Kind;
	Item.m_Time = Now;
	Item.m_WallTime = WallMilliseconds();
	Item.m_Tick = Tick;
	Item.m_Epoch = m_CaptureEpoch;
}

void CLiveRecorder::PushPending(std::chrono::nanoseconds Now)
{
	if(m_vPending.empty())
		return;
	Push(EItemKind::DATA, m_LastTick, Now);
	CItem &Item = m_vQueue.back();
	m_QueueBytes += m_vPending.size();
	Item.m_vData = std::move(m_vPending);
	m_vPending.clear();
}

void CLiveRecorder::RecordSnapshot(int Tick, const void *pData, int Size, std::chrono::nanoseconds Now)
{
	m_LastNow = Now;
	if(m_State != EState::LIVE || !m_pEncoder->IsRecording())
		return;

	// Segments start at a keyframe, so that each one can be played without
	// the ones before it. What was recorded since the last snapshot still
	// belongs to the segment before.
	if(m_SegmentStartTick < 0 || Tick - m_SegmentStartTick >= m_Settings.m_SegmentSeconds * SERVER_TICK_SPEED)
	{
		if(m_SegmentStartTick >= 0)
			PushPending(Now);
		m_pEncoder->ForceKeyframe();
		Push(EItemKind::SEGMENT, Tick, Now);
		m_SegmentStartTick = Tick;
	}
	m_pEncoder->RecordSnapshot(Tick, pData, Size);
	m_LastTick = Tick;
	PushPending(Now);

	// A match ends where the game over comes on, which is what a demo player
	// sees as well.
	const CSnapshot *pSnapshot = static_cast<const CSnapshot *>(pData);
	const CNetObj_GameInfo *pGameInfo = static_cast<const CNetObj_GameInfo *>(pSnapshot->FindItem(NETOBJTYPE_GAMEINFO, 0));
	if(pGameInfo != nullptr)
	{
		const bool GameOver = (pGameInfo->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) != 0;
		if(m_GameOver.has_value() && GameOver && !*m_GameOver)
			AddMarker(EMarkerKind::MATCH_END, "", Now);
		m_GameOver = GameOver;
	}
}

int CLiveRecorder::AddMarker(EMarkerKind Kind, const char *pLabel, std::chrono::nanoseconds Now)
{
	if(m_State != EState::LIVE || m_LastTick < 0 || !m_pEncoder->IsRecording())
		return -1;
	Push(EItemKind::MARKER, m_LastTick, Now);
	CItem &Item = m_vQueue.back();
	Item.m_MarkerKind = Kind;
	Item.m_Text = pLabel;
	log_info("live", "marker tick=%d kind=%s", m_LastTick, MarkerKindName(Kind));
	return m_LastTick;
}

void CLiveRecorder::Stop(bool Keep, const char *pReason)
{
	if(m_State != EState::LIVE)
		return;
	m_Keep = Keep;
	str_copy(m_aStopReason, pReason);
	if(m_pEncoder->IsRecording())
		m_pEncoder->Stop(IDemoRecorder::EStopMode::KEEP_FILE);
	m_State = EState::DRAINING;
}

void CLiveRecorder::Abort(const char *pReason)
{
	if(m_State == EState::OFF)
		return;
	str_copy(m_aStopReason, pReason);
	if(m_pEncoder->IsRecording())
		m_pEncoder->Stop(IDemoRecorder::EStopMode::KEEP_FILE);
	// What is younger than the delay is never written.
	m_vQueue.clear();
	m_QueueBytes = 0;
	Finish();
}

void CLiveRecorder::Apply(CItem &Item)
{
	char aPath[IO_MAX_PATH_LENGTH];
	switch(Item.m_Kind)
	{
	case EItemKind::EPOCH:
	{
		char aFile[32];
		str_format(aFile, sizeof(aFile), "init-%d", Item.m_Epoch);
		WriteFileAtomically(aFile, Item.m_vData.data(), Item.m_vData.size());
		m_vEpochs.push_back({Item.m_Epoch, Item.m_Text, Item.m_Sha256, (int64_t)Item.m_vData.size()});
		m_WrittenBytes += Item.m_vData.size();
		m_IndexDirty = true;
		break;
	}
	case EItemKind::SEGMENT:
	{
		CloseSegment();
		const int Number = m_NextSegment++;
		char aFile[32];
		str_format(aFile, sizeof(aFile), "seg-%d", Number);
		Path(aPath, sizeof(aPath), aFile);
		m_SegmentFile = m_pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(!m_SegmentFile && !m_WriteFailed)
		{
			log_error("live", "could not open '%s' for writing", aPath);
			m_WriteFailed = true;
		}
		m_vSegments.push_back({Number, Item.m_Epoch, Item.m_Tick, Item.m_Tick, 0, Item.m_WallTime, false});
		m_IndexDirty = true;
		break;
	}
	case EItemKind::DATA:
		if(!m_SegmentFile || m_vSegments.empty())
			break;
		if(io_write(m_SegmentFile, Item.m_vData.data(), Item.m_vData.size()) != Item.m_vData.size() && !m_WriteFailed)
		{
			log_error("live", "could not write segment %d", m_vSegments.back().m_Number);
			m_WriteFailed = true;
		}
		m_vSegments.back().m_Bytes += Item.m_vData.size();
		if(Item.m_Tick >= 0)
			m_vSegments.back().m_EndTick = Item.m_Tick;
		m_WrittenBytes += Item.m_vData.size();
		m_SegmentDirty = true;
		break;
	case EItemKind::MARKER:
		m_vMarkers.push_back({Item.m_Epoch, Item.m_Tick, Item.m_MarkerKind, Item.m_Text});
		m_IndexDirty = true;
		break;
	case EItemKind::EPOCH_END:
		CloseSegment();
		m_IndexDirty = true;
		break;
	}
}

void CLiveRecorder::CloseSegment()
{
	if(m_SegmentFile)
	{
		io_close(m_SegmentFile);
		m_SegmentFile = nullptr;
		m_SegmentDirty = false;
	}
	if(!m_vSegments.empty())
		m_vSegments.back().m_Complete = true;
}

int64_t CLiveRecorder::SegmentBytes() const
{
	int64_t Bytes = 0;
	for(const CSegment &Segment : m_vSegments)
		Bytes += Segment.m_Bytes;
	return Bytes;
}

float CLiveRecorder::WindowSeconds() const
{
	int Ticks = 0;
	for(const CSegment &Segment : m_vSegments)
		Ticks += Segment.m_EndTick - Segment.m_StartTick;
	return Ticks / (float)SERVER_TICK_SPEED;
}

void CLiveRecorder::EnforceLimits()
{
	const int64_t MaxBytes = m_Settings.m_MaxBytes;
	const int64_t MaxTicks = (int64_t)m_Settings.m_MaxDurationSeconds * SERVER_TICK_SPEED;
	if(MaxBytes <= 0 && MaxTicks <= 0)
		return;
	int64_t Bytes = SegmentBytes();
	int64_t Ticks = 0;
	for(const CSegment &Segment : m_vSegments)
		Ticks += Segment.m_EndTick - Segment.m_StartTick;

	bool Removed = false;
	// The segment that is written to stays, even if it alone is too much.
	while(m_vSegments.size() > 1 && m_vSegments.front().m_Complete &&
		((MaxBytes > 0 && Bytes > MaxBytes) || (MaxTicks > 0 && Ticks > MaxTicks)))
	{
		const CSegment &Oldest = m_vSegments.front();
		char aFile[32];
		str_format(aFile, sizeof(aFile), "seg-%d", Oldest.m_Number);
		char aPath[IO_MAX_PATH_LENGTH];
		Path(aPath, sizeof(aPath), aFile);
		m_pStorage->RemoveFile(aPath, IStorage::TYPE_SAVE);
		Bytes -= Oldest.m_Bytes;
		Ticks -= Oldest.m_EndTick - Oldest.m_StartTick;
		m_vSegments.erase(m_vSegments.begin());
		Removed = true;
	}
	if(!Removed)
		return;
	m_IndexDirty = true;

	// An old map without segments goes with its init, and markers go with the
	// part of the demo they were in.
	const auto FirstTickOf = [&](int Epoch) -> std::optional<int> {
		for(const CSegment &Segment : m_vSegments)
			if(Segment.m_Epoch == Epoch)
				return Segment.m_StartTick;
		return std::nullopt;
	};
	for(auto It = m_vEpochs.begin(); It != m_vEpochs.end();)
	{
		if(It + 1 != m_vEpochs.end() && !FirstTickOf(It->m_Id).has_value())
		{
			char aFile[32];
			str_format(aFile, sizeof(aFile), "init-%d", It->m_Id);
			char aPath[IO_MAX_PATH_LENGTH];
			Path(aPath, sizeof(aPath), aFile);
			m_pStorage->RemoveFile(aPath, IStorage::TYPE_SAVE);
			It = m_vEpochs.erase(It);
		}
		else
			++It;
	}
	std::erase_if(m_vMarkers, [&](const CMarker &Marker) {
		const std::optional<int> FirstTick = FirstTickOf(Marker.m_Epoch);
		return !FirstTick.has_value() || Marker.m_Tick < *FirstTick;
	});
}

bool CLiveRecorder::WriteFileAtomically(const char *pFile, const void *pData, size_t Size)
{
	char aPath[IO_MAX_PATH_LENGTH];
	Path(aPath, sizeof(aPath), pFile);
	char aTempPath[IO_MAX_PATH_LENGTH];
	str_format(aTempPath, sizeof(aTempPath), "%s.tmp", aPath);
	IOHANDLE File = m_pStorage->OpenFile(aTempPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	bool Ok = File != nullptr;
	if(File)
	{
		Ok = io_write(File, pData, Size) == Size;
		Ok = io_close(File) == 0 && Ok;
	}
	Ok = Ok && m_pStorage->RenameFile(aTempPath, aPath, IStorage::TYPE_SAVE);
	if(!Ok && !m_WriteFailed)
	{
		log_error("live", "could not write '%s'", aPath);
		m_WriteFailed = true;
	}
	return Ok;
}

bool CLiveRecorder::WriteIndex(std::chrono::nanoseconds Now)
{
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("version");
	Writer.WriteIntValue(INDEX_VERSION);
	Writer.WriteAttribute("name");
	Writer.WriteStrValue(m_aName);
	Writer.WriteAttribute("stream");
	Writer.WriteInt64Value(m_StreamId);
	Writer.WriteAttribute("state");
	Writer.WriteStrValue(m_State == EState::OFF ? "ended" : "live");
	Writer.WriteAttribute("updated");
	Writer.WriteInt64Value(WallMilliseconds());
	Writer.WriteAttribute("tick_speed");
	Writer.WriteIntValue(SERVER_TICK_SPEED);
	Writer.WriteAttribute("delay_ms");
	Writer.WriteInt64Value(std::chrono::duration_cast<std::chrono::milliseconds>(m_Settings.m_Delay).count());
	Writer.WriteAttribute("segment_seconds");
	Writer.WriteIntValue(m_Settings.m_SegmentSeconds);
	Writer.WriteAttribute("max_duration_seconds");
	Writer.WriteIntValue(m_Settings.m_MaxDurationSeconds);
	// Bytes, where `sv_live_max_size` is KiB
	Writer.WriteAttribute("max_bytes");
	Writer.WriteInt64Value(m_Settings.m_MaxBytes);

	Writer.WriteAttribute("epochs");
	Writer.BeginArray();
	for(const CEpoch &Epoch : m_vEpochs)
	{
		char aFile[32];
		str_format(aFile, sizeof(aFile), "init-%d", Epoch.m_Id);
		char aSha256[SHA256_MAXSTRSIZE];
		sha256_str(Epoch.m_Sha256, aSha256, sizeof(aSha256));
		Writer.BeginObject();
		Writer.WriteAttribute("epoch");
		Writer.WriteIntValue(Epoch.m_Id);
		Writer.WriteAttribute("init");
		Writer.WriteStrValue(aFile);
		Writer.WriteAttribute("bytes");
		Writer.WriteInt64Value(Epoch.m_InitBytes);
		Writer.WriteAttribute("map");
		Writer.WriteStrValue(Epoch.m_Map.c_str());
		Writer.WriteAttribute("sha256");
		Writer.WriteStrValue(aSha256);
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("segments");
	Writer.BeginArray();
	for(const CSegment &Segment : m_vSegments)
	{
		char aFile[32];
		str_format(aFile, sizeof(aFile), "seg-%d", Segment.m_Number);
		Writer.BeginObject();
		Writer.WriteAttribute("n");
		Writer.WriteIntValue(Segment.m_Number);
		Writer.WriteAttribute("file");
		Writer.WriteStrValue(aFile);
		Writer.WriteAttribute("epoch");
		Writer.WriteIntValue(Segment.m_Epoch);
		Writer.WriteAttribute("start_tick");
		Writer.WriteIntValue(Segment.m_StartTick);
		Writer.WriteAttribute("end_tick");
		Writer.WriteIntValue(Segment.m_EndTick);
		Writer.WriteAttribute("bytes");
		Writer.WriteInt64Value(Segment.m_Bytes);
		Writer.WriteAttribute("time");
		Writer.WriteInt64Value(Segment.m_WallTime);
		Writer.WriteAttribute("complete");
		Writer.WriteBoolValue(Segment.m_Complete);
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("markers");
	Writer.BeginArray();
	for(const CMarker &Marker : m_vMarkers)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("epoch");
		Writer.WriteIntValue(Marker.m_Epoch);
		Writer.WriteAttribute("tick");
		Writer.WriteIntValue(Marker.m_Tick);
		Writer.WriteAttribute("kind");
		Writer.WriteStrValue(MarkerKindName(Marker.m_Kind));
		if(!Marker.m_Label.empty())
		{
			Writer.WriteAttribute("label");
			Writer.WriteStrValue(Marker.m_Label.c_str());
		}
		Writer.EndObject();
	}
	Writer.EndArray();
	Writer.EndObject();

	const std::string Index = Writer.GetOutputString();
	// On Windows a reader that has the index open can keep it from being
	// replaced for a moment. What the index says must not get lost then.
	if(!WriteFileAtomically("index.json", Index.data(), Index.size()))
	{
		m_IndexDirty = true;
		return false;
	}
	m_IndexDirty = false;
	m_IndexStale = false;
	m_LastIndexWrite = Now;
	return true;
}

void CLiveRecorder::Update(std::chrono::nanoseconds Now)
{
	m_LastNow = Now;
	if(m_State == EState::OFF)
		return;

	while(!m_vQueue.empty() && m_vQueue.front().m_Time + m_Settings.m_Delay <= Now)
	{
		CItem &Item = m_vQueue.front();
		Apply(Item);
		m_QueueBytes -= Item.m_vData.size();
		m_vQueue.pop_front();
	}
	// Every tick is on disk as soon as it may be, for the range requests of
	// the players at the live edge.
	if(m_SegmentDirty && m_SegmentFile)
	{
		io_flush(m_SegmentFile);
		m_SegmentDirty = false;
		m_IndexStale = true;
	}
	EnforceLimits();

	if(m_State == EState::DRAINING && m_vQueue.empty())
	{
		Finish();
		return;
	}
	if(m_IndexDirty || (m_IndexStale && Now - m_LastIndexWrite >= INDEX_INTERVAL))
		WriteIndex(Now);
}

void CLiveRecorder::WriteKeepDemos()
{
	int NumDemos = 0;
	for(const CEpoch &Epoch : m_vEpochs)
		NumDemos += std::any_of(m_vSegments.begin(), m_vSegments.end(), [&](const CSegment &Segment) { return Segment.m_Epoch == Epoch.m_Id; });
	m_pStorage->CreateFolder("demos", IStorage::TYPE_SAVE);

	for(const CEpoch &Epoch : m_vEpochs)
	{
		std::vector<const CSegment *> vpSegments;
		for(const CSegment &Segment : m_vSegments)
			if(Segment.m_Epoch == Epoch.m_Id)
				vpSegments.push_back(&Segment);
		if(vpSegments.empty())
			continue;

		char aFilename[IO_MAX_PATH_LENGTH];
		if(NumDemos == 1)
			str_format(aFilename, sizeof(aFilename), "demos/%s.demo", m_aName);
		else
			str_format(aFilename, sizeof(aFilename), "demos/%s_%d.demo", m_aName, Epoch.m_Id);
		IOHANDLE File = m_pStorage->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(!File)
		{
			log_error("live", "could not open '%s' for writing", aFilename);
			continue;
		}
		char aFile[32];
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aFile, sizeof(aFile), "init-%d", Epoch.m_Id);
		Path(aPath, sizeof(aPath), aFile);
		bool Ok = CopyFileInto(m_pStorage, aPath, File);
		for(const CSegment *pSegment : vpSegments)
		{
			str_format(aFile, sizeof(aFile), "seg-%d", pSegment->m_Number);
			Path(aPath, sizeof(aPath), aFile);
			Ok = Ok && CopyFileInto(m_pStorage, aPath, File);
		}

		// The same markers a recorder takes: inside the demo, one a second
		// at most, and as many as the header holds.
		const int FirstTick = vpSegments.front()->m_StartTick;
		const int LastTick = vpSegments.back()->m_EndTick;
		std::vector<int> vMarkers;
		for(const CMarker &Marker : m_vMarkers)
		{
			if(Marker.m_Epoch != Epoch.m_Id || Marker.m_Tick < FirstTick || Marker.m_Tick > LastTick)
				continue;
			if(!vMarkers.empty() && Marker.m_Tick - vMarkers.back() < SERVER_TICK_SPEED)
				continue;
			if((int)vMarkers.size() == MAX_TIMELINE_MARKERS)
				break;
			vMarkers.push_back(Marker.m_Tick);
		}
		Ok = Ok && CDemoRecorder::WriteLengthAndMarkers(File, (LastTick - FirstTick) / SERVER_TICK_SPEED, vMarkers.data(), (int)vMarkers.size());
		Ok = io_close(File) == 0 && Ok;
		if(Ok)
			log_info("live", "kept name=%s demo=%s", m_aName, aFilename);
		else
			log_error("live", "could not write '%s'", aFilename);
	}
}

void CLiveRecorder::Finish()
{
	CloseSegment();
	m_vQueue.clear();
	m_QueueBytes = 0;
	m_vPending.clear();
	m_State = EState::OFF;
	// No update comes after this one to try again, and the index has to say that the stream ended.
	for(int Attempt = 0; Attempt < 5 && !WriteIndex(m_LastNow); Attempt++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	if(m_Keep)
		WriteKeepDemos();
	log_info("live", "stopped name=%s reason=%s", m_aName, m_aStopReason);
	Reset();
}

void CLiveRecorder::Reset()
{
	m_State = EState::OFF;
	m_aName[0] = '\0';
	m_aDir[0] = '\0';
	m_Keep = false;
	m_aStopReason[0] = '\0';
	m_CapturingInit = false;
	m_vInit.clear();
	m_vPending.clear();
	m_CaptureEpoch = -1;
	m_SegmentStartTick = -1;
	m_LastTick = -1;
	m_GameOver.reset();
	m_vQueue.clear();
	m_QueueBytes = 0;
	m_NextSegment = 0;
	m_vEpochs.clear();
	m_vSegments.clear();
	m_vMarkers.clear();
	if(m_SegmentFile)
		io_close(m_SegmentFile);
	m_SegmentFile = nullptr;
	m_SegmentDirty = false;
	m_IndexDirty = false;
	m_IndexStale = false;
	m_WrittenBytes = 0;
	m_WriteFailed = false;
}

void CLiveRecorder::Status(char *pBuffer, size_t BufferSize, std::chrono::nanoseconds Now) const
{
	if(m_State == EState::OFF)
	{
		str_copy(pBuffer, "state=off", BufferSize);
		return;
	}
	const float Duration = std::chrono::duration<float>(Now - m_StartTime).count();
	const float Delay = std::chrono::duration<float>(m_Settings.m_Delay).count();
	str_format(pBuffer, BufferSize, "state=%s name=%s dir=%s duration=%.1fs bytes=%lld segments=%d delay=%.0fs window=%.1fs epochs=%d written=%lld held=%lld",
		m_State == EState::LIVE ? "live" : "draining", m_aName, m_aDir, Duration, (long long)SegmentBytes(), NumSegments(), Delay, WindowSeconds(), (int)m_vEpochs.size(), (long long)m_WrittenBytes, (long long)QueuedBytes());
}
