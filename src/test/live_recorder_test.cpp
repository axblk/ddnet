#include "test.h"

#include <base/bytes.h>
#include <base/fs.h>
#include <base/io.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/server/live_recorder.h>
#include <engine/shared/demo.h>
#include <engine/shared/json.h>
#include <engine/shared/snapshot.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
	// A stand-in map: the demo only carries it along.
	const unsigned char MAP_DATA[] = "not really a map, but bytes a demo carries";

	class CCountingListener : public CDemoPlayer::IListener
	{
	public:
		int m_Snapshots = 0;
		int m_Messages = 0;
		void OnDemoPlayerSnapshot(void *pData, int Size) override { m_Snapshots++; }
		void OnDemoPlayerMessage(void *pData, int Size) override { m_Messages++; }
	};

	void RemoveTree(const char *pPath)
	{
		std::vector<std::pair<std::string, bool>> vEntries;
		fs_listdir(pPath, [](const char *pName, int IsDir, int, void *pUser) {
			if(str_comp(pName, ".") != 0 && str_comp(pName, "..") != 0)
				static_cast<std::vector<std::pair<std::string, bool>> *>(pUser)->emplace_back(pName, IsDir != 0);
			return 0; }, 0, &vEntries);
		for(const auto &[Name, IsDir] : vEntries)
		{
			const std::string Path = std::string(pPath) + "/" + Name;
			if(IsDir)
				RemoveTree(Path.c_str());
			else
			{
				EXPECT_EQ(fs_remove(Path.c_str()), 0) << Path;
			}
		}
		EXPECT_EQ(fs_removedir(pPath), 0) << pPath;
	}
} // namespace

class LiveRecorder : public ::testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CTestInfo m_Info;
	std::unique_ptr<IStorage> m_pStorage;
	CSnapshotDelta m_Delta;
	CDemoRecorder m_Encoder{&m_Delta, false};
	CLiveRecorder m_Live{&m_Encoder};
	std::chrono::nanoseconds m_Now = 1000s;
	int m_Tick = 100;
	bool m_GameOver = false;

	void SetUp() override
	{
		m_pStorage = m_Info.CreateTestStorage();
		ASSERT_TRUE(m_pStorage);
	}

	void TearDown() override
	{
		m_Live.Abort("test");
		m_pStorage.reset();
		if(!HasFailure())
			RemoveTree(m_Info.m_aFilename);
	}

	void Start(CLiveRecorder::CSettings Settings, const char *pName = "stream")
	{
		char aError[256];
		ASSERT_TRUE(m_Live.Start(m_pStorage.get(), "live", pName, Settings, m_Now, aError, sizeof(aError))) << aError;
		BeginEpoch("map");
	}

	void BeginEpoch(const char *pMap)
	{
		SHA256_DIGEST Sha256 = sha256(MAP_DATA, sizeof(MAP_DATA));
		m_Live.BeginEpoch("0.6 626fce9a778df4d4", pMap, Sha256, 0x12345678, sizeof(MAP_DATA), MAP_DATA, m_Now);
	}

	// One tick of a server that records: a snapshot with a moving item, now
	// and then a message, and the time going on.
	void Tick()
	{
		CSnapshotBuilder Builder;
		Builder.Init();
		CNetObj_GameInfo GameInfo = {};
		GameInfo.m_GameStateFlags = m_GameOver ? GAMESTATEFLAG_GAMEOVER : 0;
		GameInfo.m_RoundStartTick = 1;
		ASSERT_TRUE(Builder.NewItem(NETOBJTYPE_GAMEINFO, 0, &GameInfo, sizeof(GameInfo)));
		CNetObj_Flag Flag = {};
		Flag.m_X = m_Tick;
		Flag.m_Y = m_Tick * 2;
		ASSERT_TRUE(Builder.NewItem(NETOBJTYPE_FLAG, 0, &Flag, sizeof(Flag)));
		CSnapshotBuffer Buffer;
		const int Size = Builder.Finish(&Buffer);
		m_Live.RecordSnapshot(m_Tick, Buffer.AsSnapshot(), Size, m_Now);
		if(m_Tick % 7 == 0)
		{
			const unsigned char aMessage[] = {1, 2, 3, 4, 5, 6, 7, 8};
			m_Encoder.RecordMessage(aMessage, sizeof(aMessage));
		}
		m_Tick++;
		m_Now += 20ms;
		m_Live.Update(m_Now);
	}

	void Ticks(int Count)
	{
		for(int i = 0; i < Count; i++)
			Tick();
	}

	std::string Path(const char *pFile, const char *pName = "stream") const
	{
		return std::string("live/") + pName + "/" + pFile;
	}

	bool Exists(const char *pFile, const char *pName = "stream") const
	{
		return m_pStorage->FileExists(Path(pFile, pName).c_str(), IStorage::TYPE_SAVE);
	}

	std::vector<unsigned char> Read(const std::string &Path) const
	{
		void *pData;
		unsigned Size;
		if(!m_pStorage->ReadFile(Path.c_str(), IStorage::TYPE_SAVE, &pData, &Size))
			return {};
		std::vector<unsigned char> vData((unsigned char *)pData, (unsigned char *)pData + Size);
		free(pData);
		return vData;
	}

	json_value *Index() const
	{
		const std::vector<unsigned char> vIndex = Read(Path("index.json"));
		return vIndex.empty() ? nullptr : JsonParse((const char *)vIndex.data(), vIndex.size());
	}

	// Writes the init of an epoch and the given segments into one file.
	void Concatenate(const char *pFilename, int Epoch, const std::vector<int> &vSegments) const
	{
		IOHANDLE File = m_pStorage->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		ASSERT_TRUE(File);
		char aFile[32];
		str_format(aFile, sizeof(aFile), "init-%d", Epoch);
		std::vector<unsigned char> vData = Read(Path(aFile));
		io_write(File, vData.data(), vData.size());
		for(int Segment : vSegments)
		{
			str_format(aFile, sizeof(aFile), "seg-%d", Segment);
			vData = Read(Path(aFile));
			ASSERT_FALSE(vData.empty()) << aFile;
			io_write(File, vData.data(), vData.size());
		}
		io_close(File);
	}

	// Plays a demo to its end, the way a player reads it.
	void ExpectPlayable(const char *pFilename, int FirstTick, int LastTick, int *pSnapshots = nullptr) const
	{
		const std::unique_ptr<CSnapshotDelta> pDelta = std::make_unique<CSnapshotDelta>();
		const std::unique_ptr<CDemoPlayer> pPlayer = NewPlayer(pDelta.get());
		CCountingListener Listener;
		pPlayer->SetListener(&Listener);
		ASSERT_EQ(pPlayer->Load(m_pStorage.get(), pFilename, IStorage::TYPE_SAVE), 0) << pPlayer->ErrorMessage();
		EXPECT_EQ(pPlayer->BaseInfo()->m_FirstTick, FirstTick);
		EXPECT_EQ(pPlayer->BaseInfo()->m_LastTick, LastTick);
		pPlayer->Play();
		pPlayer->Update(false);
		EXPECT_TRUE(pPlayer->IsPlaying()) << pPlayer->ErrorMessage();
		EXPECT_EQ(pPlayer->BaseInfo()->m_CurrentTick, LastTick);
		EXPECT_GT(Listener.m_Messages, 0);
		if(pSnapshots)
			*pSnapshots = Listener.m_Snapshots;
		pPlayer->Stop();
	}

	// A snapshot delta and a demo player take about a megabyte together, all
	// the stack a thread has on Windows, so they go on the heap.
	static std::unique_ptr<CDemoPlayer> NewPlayer(CSnapshotDelta *pDelta)
	{
		return std::make_unique<CDemoPlayer>(pDelta, pDelta, false);
	}
};

TEST_F(LiveRecorder, SegmentsStartAtKeyframesAndMakeADemo)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_SegmentSeconds = 1;
	Start(Settings);
	Ticks(5 * SERVER_TICK_SPEED + 10);

	json_value *pIndex = Index();
	ASSERT_TRUE(pIndex);
	EXPECT_STREQ(json_string_get(json_object_get(pIndex, "state")), "live");
	const json_value *pSegments = json_object_get(pIndex, "segments");
	ASSERT_EQ(json_array_length(pSegments), 6);
	std::vector<int> vSegments;
	for(int i = 0; i < json_array_length(pSegments); i++)
	{
		const json_value *pSegment = json_array_get(pSegments, i);
		const int Number = json_int_get(json_object_get(pSegment, "n"));
		const int StartTick = json_int_get(json_object_get(pSegment, "start_tick"));
		EXPECT_EQ(Number, i);
		EXPECT_EQ(StartTick, 100 + i * SERVER_TICK_SPEED);
		if(i < 5)
		{
			EXPECT_EQ(json_int_get(json_object_get(pSegment, "end_tick")), StartTick + SERVER_TICK_SPEED - 1);
		}
		EXPECT_EQ(json_boolean_get(json_object_get(pSegment, "complete")), i < 5);
		// Every segment opens with the tick marker of a keyframe.
		// The index says how the last segment grows once a second.
		const std::vector<unsigned char> vData = Read(Path(json_string_get(json_object_get(pSegment, "file"))));
		if(i < 5)
		{
			EXPECT_EQ((int64_t)vData.size(), json_int_get(json_object_get(pSegment, "bytes")));
		}
		else
		{
			EXPECT_GE((int64_t)vData.size(), json_int_get(json_object_get(pSegment, "bytes")));
		}
		ASSERT_GE(vData.size(), 5u);
		EXPECT_EQ(vData[0], 0x80 | 0x40);
		EXPECT_EQ(bytes_be_to_uint(&vData[1]), (unsigned)StartTick);
		vSegments.push_back(Number);
	}
	const json_value *pEpochs = json_object_get(pIndex, "epochs");
	ASSERT_EQ(json_array_length(pEpochs), 1);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pEpochs, 0), "map")), "map");
	json_value_free(pIndex);

	// All of it, and only the newest part, are demos.
	Concatenate("all.demo", 0, vSegments);
	int Snapshots;
	ExpectPlayable("all.demo", 100, 100 + 5 * SERVER_TICK_SPEED + 9, &Snapshots);
	EXPECT_EQ(Snapshots, 5 * SERVER_TICK_SPEED + 10);
	Concatenate("tail.demo", 0, {3, 4, 5});
	ExpectPlayable("tail.demo", 100 + 3 * SERVER_TICK_SPEED, 100 + 5 * SERVER_TICK_SPEED + 9);
	m_pStorage->RemoveFile("all.demo", IStorage::TYPE_SAVE);
	m_pStorage->RemoveFile("tail.demo", IStorage::TYPE_SAVE);
}

TEST_F(LiveRecorder, NothingYoungerThanTheDelayIsWritten)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_Delay = 2s;
	Settings.m_SegmentSeconds = 1;
	Start(Settings);
	EXPECT_TRUE(Exists("index.json"));
	Ticks(SERVER_TICK_SPEED * 3 / 2);
	// Not even the map is out before the delay.
	EXPECT_FALSE(Exists("init-0"));
	EXPECT_FALSE(Exists("seg-0"));
	EXPECT_GT(m_Live.QueuedBytes(), (int64_t)sizeof(MAP_DATA));

	Ticks(SERVER_TICK_SPEED);
	EXPECT_TRUE(Exists("init-0"));
	json_value *pIndex = Index();
	ASSERT_TRUE(pIndex);
	const json_value *pSegments = json_object_get(pIndex, "segments");
	ASSERT_EQ(json_array_length(pSegments), 1);
	EXPECT_EQ(json_int_get(json_object_get(pIndex, "delay_ms")), 2000);
	json_value_free(pIndex);
	// 2.5 s were recorded, a tick every 20 ms from tick 100 on. What is
	// older than 2 s is on disk: up to tick 125, and no tick more.
	Concatenate("delayed.demo", 0, {0});
	ExpectPlayable("delayed.demo", 100, 100 + SERVER_TICK_SPEED / 2);
	m_pStorage->RemoveFile("delayed.demo", IStorage::TYPE_SAVE);

	// Stopping writes the rest as its delay passes, then the stream ends.
	m_Live.Stop(false, "test");
	EXPECT_TRUE(m_Live.IsActive());
	for(int i = 0; i < 2 * SERVER_TICK_SPEED && m_Live.IsActive(); i++)
	{
		m_Now += 20ms;
		m_Live.Update(m_Now);
	}
	EXPECT_FALSE(m_Live.IsActive());
	pIndex = Index();
	ASSERT_TRUE(pIndex);
	EXPECT_STREQ(json_string_get(json_object_get(pIndex, "state")), "ended");
	pSegments = json_object_get(pIndex, "segments");
	const json_value *pLast = json_array_get(pSegments, json_array_length(pSegments) - 1);
	EXPECT_EQ(json_int_get(json_object_get(pLast, "end_tick")), m_Tick - 1);
	EXPECT_TRUE(json_boolean_get(json_object_get(pLast, "complete")));
	json_value_free(pIndex);
}

TEST_F(LiveRecorder, AbortDropsWhatIsHeldBack)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_Delay = 5s;
	Start(Settings);
	Ticks(SERVER_TICK_SPEED);
	m_Live.Abort("test");
	EXPECT_FALSE(m_Live.IsActive());
	EXPECT_FALSE(Exists("init-0"));
	EXPECT_FALSE(Exists("seg-0"));
	json_value *pIndex = Index();
	ASSERT_TRUE(pIndex);
	EXPECT_STREQ(json_string_get(json_object_get(pIndex, "state")), "ended");
	EXPECT_EQ(json_array_length(json_object_get(pIndex, "segments")), 0);
	json_value_free(pIndex);
}

TEST_F(LiveRecorder, LimitsDeleteTheOldestSegments)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_SegmentSeconds = 1;
	Settings.m_MaxDurationSeconds = 3;
	Start(Settings);
	Ticks(10 * SERVER_TICK_SPEED + 5);
	// 3 s at most: two whole segments and the one that grows.
	EXPECT_LE(m_Live.WindowSeconds(), 3.0f);
	EXPECT_GE(m_Live.WindowSeconds(), 2.0f);
	EXPECT_EQ(m_Live.NumSegments(), 3);
	EXPECT_FALSE(Exists("seg-0"));
	EXPECT_FALSE(Exists("seg-7"));
	EXPECT_TRUE(Exists("seg-8"));
	EXPECT_TRUE(Exists("seg-10"));
	EXPECT_TRUE(Exists("init-0"));
	Concatenate("window.demo", 0, {8, 9, 10});
	ExpectPlayable("window.demo", 100 + 8 * SERVER_TICK_SPEED, 100 + 10 * SERVER_TICK_SPEED + 4);
	m_pStorage->RemoveFile("window.demo", IStorage::TYPE_SAVE);
	m_Live.Abort("test");

	// The size limit counts the segments.
	Settings.m_MaxDurationSeconds = 0;
	Settings.m_MaxBytes = 4000;
	char aError[256];
	ASSERT_TRUE(m_Live.Start(m_pStorage.get(), "live", "sized", Settings, m_Now, aError, sizeof(aError))) << aError;
	BeginEpoch("map");
	Ticks(20 * SERVER_TICK_SPEED);
	EXPECT_LE(m_Live.SegmentBytes(), 4000);
	EXPECT_GE(m_Live.NumSegments(), 1);
	EXPECT_FALSE(Exists("seg-0", "sized"));
	EXPECT_TRUE(Exists("seg-19", "sized"));
}

TEST_F(LiveRecorder, MatchEndAndManualMarkers)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_SegmentSeconds = 2;
	Start(Settings);
	// A game over at the start of the stream is no match that ended in it.
	m_GameOver = true;
	Ticks(10);
	m_GameOver = false;
	Ticks(SERVER_TICK_SPEED);
	const int MatchEnd = m_Tick;
	m_GameOver = true;
	Ticks(SERVER_TICK_SPEED + 5);
	const int Manual = m_Live.AddMarker(CLiveRecorder::EMarkerKind::MANUAL, "look", m_Now);
	EXPECT_EQ(Manual, m_Tick - 1);
	Ticks(10);

	json_value *pIndex = Index();
	ASSERT_TRUE(pIndex);
	const json_value *pMarkers = json_object_get(pIndex, "markers");
	ASSERT_EQ(json_array_length(pMarkers), 2);
	EXPECT_EQ(json_int_get(json_object_get(json_array_get(pMarkers, 0), "tick")), MatchEnd);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pMarkers, 0), "kind")), "match_end");
	EXPECT_EQ(json_int_get(json_object_get(json_array_get(pMarkers, 1), "tick")), Manual);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pMarkers, 1), "kind")), "manual");
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pMarkers, 1), "label")), "look");
	json_value_free(pIndex);

	// A kept demo has them in its header, like a recorder's markers.
	m_Live.Stop(true, "test");
	m_Live.Update(m_Now);
	EXPECT_FALSE(m_Live.IsActive());
	const std::unique_ptr<CSnapshotDelta> pDelta = std::make_unique<CSnapshotDelta>();
	const std::unique_ptr<CDemoPlayer> pPlayer = NewPlayer(pDelta.get());
	CDemoHeader Header;
	CTimelineMarkers TimelineMarkers;
	CMapInfo MapInfo;
	ASSERT_TRUE(pPlayer->GetDemoInfo(m_pStorage.get(), "demos/stream.demo", IStorage::TYPE_SAVE, &Header, &TimelineMarkers, &MapInfo));
	EXPECT_EQ(bytes_be_to_uint(TimelineMarkers.m_aNumTimelineMarkers), 2u);
	EXPECT_EQ(bytes_be_to_uint(TimelineMarkers.m_aTimelineMarkers[0]), (unsigned)MatchEnd);
	EXPECT_EQ(bytes_be_to_uint(TimelineMarkers.m_aTimelineMarkers[1]), (unsigned)Manual);
	EXPECT_EQ(bytes_be_to_uint(Header.m_aLength), (unsigned)((m_Tick - 1 - 100) / SERVER_TICK_SPEED));
	EXPECT_EQ(MapInfo.m_Size, sizeof(MAP_DATA));
	ExpectPlayable("demos/stream.demo", 100, m_Tick - 1);
	m_pStorage->RemoveFile("demos/stream.demo", IStorage::TYPE_SAVE);
}

TEST_F(LiveRecorder, AMapChangeStartsAnEpoch)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_SegmentSeconds = 1;
	Start(Settings);
	Ticks(SERVER_TICK_SPEED + 20);
	// The server stops its recorders for the map change, and ticks begin
	// anew on the next map.
	m_Encoder.Stop(IDemoRecorder::EStopMode::KEEP_FILE);
	EXPECT_TRUE(m_Live.NeedsEpoch());
	m_Live.Update(m_Now);
	m_Tick = 0;
	BeginEpoch("other");
	EXPECT_FALSE(m_Live.NeedsEpoch());
	Ticks(SERVER_TICK_SPEED / 2);

	json_value *pIndex = Index();
	ASSERT_TRUE(pIndex);
	const json_value *pEpochs = json_object_get(pIndex, "epochs");
	ASSERT_EQ(json_array_length(pEpochs), 2);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pEpochs, 1), "map")), "other");
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pEpochs, 1), "init")), "init-1");
	const json_value *pSegments = json_object_get(pIndex, "segments");
	ASSERT_EQ(json_array_length(pSegments), 3);
	EXPECT_EQ(json_int_get(json_object_get(json_array_get(pSegments, 1), "epoch")), 0);
	EXPECT_TRUE(json_boolean_get(json_object_get(json_array_get(pSegments, 1), "complete")));
	EXPECT_EQ(json_int_get(json_object_get(json_array_get(pSegments, 2), "epoch")), 1);
	EXPECT_EQ(json_int_get(json_object_get(json_array_get(pSegments, 2), "start_tick")), 0);
	json_value_free(pIndex);

	// Each map has a demo of its own.
	Concatenate("second.demo", 1, {2});
	ExpectPlayable("second.demo", 0, SERVER_TICK_SPEED / 2 - 1);
	m_pStorage->RemoveFile("second.demo", IStorage::TYPE_SAVE);
	m_Live.Stop(true, "test");
	m_Live.Update(m_Now);
	ExpectPlayable("demos/stream_0.demo", 100, 100 + SERVER_TICK_SPEED + 19);
	ExpectPlayable("demos/stream_1.demo", 0, SERVER_TICK_SPEED / 2 - 1);
	m_pStorage->RemoveFile("demos/stream_0.demo", IStorage::TYPE_SAVE);
	m_pStorage->RemoveFile("demos/stream_1.demo", IStorage::TYPE_SAVE);
}

TEST_F(LiveRecorder, ANameIsReusedAndChecked)
{
	CLiveRecorder::CSettings Settings;
	Start(Settings);
	Ticks(3 * SERVER_TICK_SPEED);
	m_Live.Stop(false, "test");
	m_Live.Update(m_Now);
	EXPECT_TRUE(Exists("seg-0"));

	// The files of the stream before are gone, the rest stays.
	IOHANDLE Other = m_pStorage->OpenFile(Path("notes.txt").c_str(), IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_TRUE(Other);
	io_close(Other);
	Settings.m_Delay = 10s;
	char aError[256];
	ASSERT_TRUE(m_Live.Start(m_pStorage.get(), "live", "stream", Settings, m_Now, aError, sizeof(aError))) << aError;
	EXPECT_FALSE(Exists("seg-0"));
	EXPECT_FALSE(Exists("init-0"));
	EXPECT_TRUE(Exists("notes.txt"));
	m_Live.Abort("test");
	m_pStorage->RemoveFile(Path("notes.txt").c_str(), IStorage::TYPE_SAVE);

	EXPECT_FALSE(m_Live.Start(m_pStorage.get(), "live", "../up", Settings, m_Now, aError, sizeof(aError)));
	EXPECT_FALSE(m_Live.Start(m_pStorage.get(), "live", "", Settings, m_Now, aError, sizeof(aError)));
	EXPECT_FALSE(m_Live.Start(m_pStorage.get(), "../live", "name", Settings, m_Now, aError, sizeof(aError)));
	EXPECT_FALSE(m_Live.IsActive());
}

TEST_F(LiveRecorder, APlayerFollowsTheGrowingDemo)
{
	CLiveRecorder::CSettings Settings;
	Settings.m_SegmentSeconds = 1;
	Start(Settings);
	Ticks(SERVER_TICK_SPEED + 10);
	// What a page puts together from the stream: the init and the segments,
	// the last one still growing.
	Concatenate("follow.demo", 0, {0, 1});
	const size_t HadBytes = Read(Path("seg-1")).size();

	const std::unique_ptr<CSnapshotDelta> pDelta = std::make_unique<CSnapshotDelta>();
	const std::unique_ptr<CDemoPlayer> pPlayer = NewPlayer(pDelta.get());
	CDemoPlayer &Player = *pPlayer;
	CCountingListener Listener;
	Player.SetListener(&Listener);
	ASSERT_EQ(Player.Load(m_pStorage.get(), "follow.demo", IStorage::TYPE_SAVE), 0) << Player.ErrorMessage();
	Player.Play();
	Player.SetLive(true);
	EXPECT_TRUE(Player.BaseInfo()->m_LiveDemo);
	const int LastTick = 100 + SERVER_TICK_SPEED + 9;
	EXPECT_EQ(Player.BaseInfo()->m_LastTick, LastTick);

	// It plays up to the last tick that is whole and waits there, rather
	// than running into the end of the file.
	Player.Update(false);
	EXPECT_TRUE(Player.IsPlaying()) << Player.ErrorMessage();
	EXPECT_FALSE(Player.BaseInfo()->m_Paused);
	EXPECT_EQ(Player.BaseInfo()->m_CurrentTick, LastTick - 1);

	// What was appended since is played on.
	Ticks(SERVER_TICK_SPEED);
	IOHANDLE File = m_pStorage->OpenFile("follow.demo", IOFLAG_APPEND, IStorage::TYPE_SAVE);
	ASSERT_TRUE(File);
	std::vector<unsigned char> vData = Read(Path("seg-1"));
	io_write(File, vData.data() + HadBytes, vData.size() - HadBytes);
	vData = Read(Path("seg-2"));
	io_write(File, vData.data(), vData.size());
	io_close(File);
	// The player looks for more at the tick rate, by the time of the frame.
	std::this_thread::sleep_for(50ms);
	set_new_tick();
	Player.Update(false);
	EXPECT_TRUE(Player.IsPlaying()) << Player.ErrorMessage();
	EXPECT_EQ(Player.BaseInfo()->m_LastTick, 100 + 2 * SERVER_TICK_SPEED + 9);
	EXPECT_EQ(Player.BaseInfo()->m_CurrentTick, 100 + 2 * SERVER_TICK_SPEED + 8);
	EXPECT_TRUE(Player.BaseInfo()->m_LiveDemo);

	// Seeking stays two seconds before the end of a live demo.
	EXPECT_TRUE(Player.SeekPercent(1.0f));
	EXPECT_LE(Player.BaseInfo()->m_CurrentTick, Player.BaseInfo()->m_LastTick - 2 * SERVER_TICK_SPEED + 1);

	// Once it stops growing, it plays to its end like any demo.
	Player.SetLive(false);
	EXPECT_FALSE(Player.BaseInfo()->m_LiveDemo);
	Player.Update(false);
	EXPECT_TRUE(Player.IsPlaying()) << Player.ErrorMessage();
	EXPECT_TRUE(Player.BaseInfo()->m_Paused);
	EXPECT_EQ(Player.BaseInfo()->m_CurrentTick, 100 + 2 * SERVER_TICK_SPEED + 9);
	EXPECT_GT(Listener.m_Snapshots, 2 * SERVER_TICK_SPEED);
	Player.Stop();
	m_pStorage->RemoveFile("follow.demo", IStorage::TYPE_SAVE);
}
