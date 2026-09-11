#include "test.h"

#include <base/fs.h>
#include <base/hash.h>
#include <base/mem.h>

#include <engine/client/ghost.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace
{

	constexpr const char *GHOST_MAP = "Test Map";
	constexpr const char *GHOST_OWNER = "Ghost Owner";
	constexpr int GHOST_TICKS = 25;
	constexpr int GHOST_TIME = 1234;

	enum
	{
		ITEM_TYPE_SKIN = 0,
		ITEM_TYPE_POSITION,
	};

	// A skin at the front and then a position per tick, which is the shape of a
	// real ghost: one item of one type, then many of another, told apart only by
	// the type byte in front of each chunk.
	struct SSkinItem
	{
		int m_aColors[3];
	};

	struct SPositionItem
	{
		int m_Tick;
		int m_X;
		int m_Y;
		int m_Angle;
	};

	SSkinItem TestSkin()
	{
		return SSkinItem{{0x112233, 0x445566, 0x778899}};
	}

	SPositionItem TestPosition(int Index)
	{
		// Values that both grow and jump around, because consecutive items are
		// written as the difference to the one before.
		return SPositionItem{Index, Index * 32, 900 - Index * 7, Index % 3 == 0 ? -1234 : Index * 100};
	}

	SHA256_DIGEST TestMapSha256()
	{
		SHA256_DIGEST Sha256;
		for(size_t i = 0; i < sizeof(Sha256.data); i++)
		{
			Sha256.data[i] = (unsigned char)i;
		}
		return Sha256;
	}

	// Writes the ghost that every test here reads.
	void RecordGhost(IStorage *pStorage, const char *pFilename)
	{
		CGhostRecorder Recorder;
		Recorder.Init(pStorage);
		ASSERT_EQ(Recorder.Start(pFilename, GHOST_MAP, TestMapSha256(), GHOST_OWNER), 0);
		const SSkinItem Skin = TestSkin();
		Recorder.WriteData(ITEM_TYPE_SKIN, &Skin, sizeof(Skin));
		for(int i = 0; i < GHOST_TICKS; i++)
		{
			const SPositionItem Position = TestPosition(i);
			Recorder.WriteData(ITEM_TYPE_POSITION, &Position, sizeof(Position));
		}
		Recorder.Stop(GHOST_TICKS, GHOST_TIME);
		ASSERT_FALSE(Recorder.IsRecording());
	}

	// Reads everything the ghost holds and checks it is what was written.
	void ExpectGhostContents(CGhostLoader &Loader)
	{
		const CGhostInfo *pInfo = Loader.GetInfo();
		EXPECT_STREQ(pInfo->m_aOwner, GHOST_OWNER);
		EXPECT_STREQ(pInfo->m_aMap, GHOST_MAP);
		EXPECT_EQ(pInfo->m_NumTicks, GHOST_TICKS);
		EXPECT_EQ(pInfo->m_Time, GHOST_TIME);

		int Type;
		ASSERT_TRUE(Loader.ReadNextType(&Type));
		ASSERT_EQ(Type, ITEM_TYPE_SKIN);
		SSkinItem Skin;
		ASSERT_TRUE(Loader.ReadData(Type, &Skin, sizeof(Skin)));
		const SSkinItem ExpectedSkin = TestSkin();
		EXPECT_EQ(mem_comp(&Skin, &ExpectedSkin, sizeof(Skin)), 0);

		for(int i = 0; i < GHOST_TICKS; i++)
		{
			ASSERT_TRUE(Loader.ReadNextType(&Type));
			ASSERT_EQ(Type, ITEM_TYPE_POSITION);
			SPositionItem Position;
			ASSERT_TRUE(Loader.ReadData(Type, &Position, sizeof(Position)));
			const SPositionItem Expected = TestPosition(i);
			EXPECT_EQ(mem_comp(&Position, &Expected, sizeof(Position)), 0) << "item " << i;
		}

		// Nothing follows the last item.
		EXPECT_FALSE(Loader.ReadNextType(&Type));
	}

} // namespace

TEST(Ghost, RecordAndLoad)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);

	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), ".ghost");
	RecordGhost(pStorage.get(), aFilename);

	CGhostLoader Loader;
	Loader.Init(pStorage.get());
	ASSERT_TRUE(Loader.Load(aFilename, GHOST_MAP, TestMapSha256(), 0));
	ExpectGhostContents(Loader);
	Loader.Close();
}

TEST(Ghost, LoadFromMemory)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);

	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), ".ghost");
	RecordGhost(pStorage.get(), aFilename);

	void *pData;
	unsigned DataSize;
	ASSERT_TRUE(pStorage->ReadFile(aFilename, IStorage::TYPE_SAVE, &pData, &DataSize));
	std::vector<uint8_t> vData(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + DataSize);
	free(pData);

	CGhostLoader Loader;
	Loader.Init(pStorage.get());
	ASSERT_TRUE(Loader.LoadFromMemory(std::move(vData), aFilename, GHOST_MAP, TestMapSha256(), 0));
	ExpectGhostContents(Loader);
	Loader.Close();
}

// A ghost that has been loaded holds nothing but memory, so the file it came
// from can be deleted while it is being read.
TEST(Ghost, LoadAndDeleteFile)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);

	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), ".ghost");
	RecordGhost(pStorage.get(), aFilename);

	CGhostLoader Loader;
	Loader.Init(pStorage.get());
	ASSERT_TRUE(Loader.Load(aFilename, GHOST_MAP, TestMapSha256(), 0));

	char aCompletePath[IO_MAX_PATH_LENGTH];
	pStorage->GetCompletePath(IStorage::TYPE_SAVE, aFilename, aCompletePath, sizeof(aCompletePath));
	ASSERT_FALSE(fs_remove(aCompletePath));

	ExpectGhostContents(Loader);
	Loader.Close();
}

TEST(Ghost, GhostInfo)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);

	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), ".ghost");
	RecordGhost(pStorage.get(), aFilename);

	CGhostLoader Loader;
	Loader.Init(pStorage.get());
	CGhostInfo GhostInfo;
	ASSERT_TRUE(Loader.GetGhostInfo(aFilename, &GhostInfo, GHOST_MAP, TestMapSha256(), 0));
	EXPECT_STREQ(GhostInfo.m_aOwner, GHOST_OWNER);
	EXPECT_STREQ(GhostInfo.m_aMap, GHOST_MAP);
	EXPECT_EQ(GhostInfo.m_NumTicks, GHOST_TICKS);
	EXPECT_EQ(GhostInfo.m_Time, GHOST_TIME);

	// A ghost of another map is not this map's ghost.
	EXPECT_FALSE(Loader.GetGhostInfo(aFilename, &GhostInfo, "Another Map", TestMapSha256(), 0));
}

// Nothing of a ghost can be read out of bytes that are not one.
TEST(Ghost, LoadInvalid)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);

	CGhostLoader Loader;
	Loader.Init(pStorage.get());
	EXPECT_FALSE(Loader.LoadFromMemory({}, "empty.ghost", GHOST_MAP, TestMapSha256(), 0));
	EXPECT_FALSE(Loader.LoadFromMemory(std::vector<uint8_t>(64, 0), "zeroes.ghost", GHOST_MAP, TestMapSha256(), 0));
}
