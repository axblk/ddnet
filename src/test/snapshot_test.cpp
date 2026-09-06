#include <base/mem.h>

#include <engine/shared/snapshot.h>

#include <generated/protocol.h>

#include <gtest/gtest.h>

#include <algorithm>

TEST(Snapshot, CrcOneInt)
{
	CSnapshotBuilder Builder;
	Builder.Init();

	CNetObj_Flag Flag;
	Flag.m_X = 4;
	Flag.m_Y = 0;
	Flag.m_Team = 0;
	ASSERT_TRUE(Builder.NewItem(NETOBJTYPE_FLAG, 0, &Flag, sizeof(Flag)));

	CSnapshotBuffer Buffer;
	Builder.Finish(&Buffer);
	ASSERT_EQ(Buffer.AsSnapshot()->Crc(), 4);
}

TEST(Snapshot, CrcTwoInts)
{
	CSnapshotBuilder Builder;
	Builder.Init();

	CNetObj_Flag Flag;
	Flag.m_X = 1;
	Flag.m_Y = 1;
	Flag.m_Team = 0;
	ASSERT_TRUE(Builder.NewItem(NETOBJTYPE_FLAG, 0, &Flag, sizeof(Flag)));

	CSnapshotBuffer Buffer;
	Builder.Finish(&Buffer);
	ASSERT_EQ(Buffer.AsSnapshot()->Crc(), 2);
}

TEST(Snapshot, CrcBiggerInts)
{
	CSnapshotBuilder Builder;
	Builder.Init();

	CNetObj_Flag Flag;
	Flag.m_X = 99999999;
	Flag.m_Y = 1;
	Flag.m_Team = 1;
	ASSERT_TRUE(Builder.NewItem(NETOBJTYPE_FLAG, 0, &Flag, sizeof(Flag)));

	CSnapshotBuffer Buffer;
	Builder.Finish(&Buffer);
	ASSERT_EQ(Buffer.AsSnapshot()->Crc(), 100000001);
}

TEST(Snapshot, CrcOverflow)
{
	CSnapshotBuilder Builder;
	Builder.Init();

	CNetObj_Flag Flag;
	Flag.m_X = 0xFFFFFFFF;
	Flag.m_Y = 1;
	Flag.m_Team = 1;
	ASSERT_TRUE(Builder.NewItem(NETOBJTYPE_FLAG, 0, &Flag, sizeof(Flag)));

	CSnapshotBuffer Buffer;
	Builder.Finish(&Buffer);
	ASSERT_EQ(Buffer.AsSnapshot()->Crc(), 1);
}

TEST(Snapshot, StorageGet)
{
	CSnapshotStorage Storage;

	// `CSnapshotStorage` needs snapshots in increasing tick order.
	const char aData[8] = {0};
	Storage.Add(10, 1000, 1, aData, 0, nullptr);
	Storage.Add(20, 2000, 2, aData, 0, nullptr);
	Storage.Add(30, 3000, 3, aData, 0, nullptr);
	Storage.Add(40, 4000, 4, aData, 0, nullptr);

	int64_t Tagtime = -1;

	// Retrieve existing snapshots.
	EXPECT_EQ(Storage.Get(40, &Tagtime, nullptr, nullptr), 4);
	EXPECT_EQ(Tagtime, 4000);
	EXPECT_EQ(Storage.Get(10, &Tagtime, nullptr, nullptr), 1);
	EXPECT_EQ(Tagtime, 1000);
	EXPECT_EQ(Storage.Get(30, &Tagtime, nullptr, nullptr), 3);
	EXPECT_EQ(Tagtime, 3000);

	// Check non-existing snapshots in before, within and after the range.
	EXPECT_EQ(Storage.Get(50, nullptr, nullptr, nullptr), -1);
	EXPECT_EQ(Storage.Get(5, nullptr, nullptr, nullptr), -1);
	EXPECT_EQ(Storage.Get(25, nullptr, nullptr, nullptr), -1);
}

TEST(Snapshot, StoragePurgeAndReuse)
{
	// Holders removed by `PurgeUntil` are reused by later `Add` calls, while
	// the holders that are still in the storage must neither move nor lose
	// their data. Vary the sizes so that holders are reused as they are, grown
	// and then reused again.
	static const auto SnapSize = [](int Tick) { return (size_t)(8 + (Tick * 37) % 4088); };
	static const auto AltSize = [](int Tick) { return Tick % 3 == 0 ? (size_t)0 : (size_t)(8 + (Tick * 53) % 4088); };

	char aData[4096];
	for(size_t i = 0; i < sizeof(aData); i++)
		aData[i] = (char)i;

	CSnapshotStorage Storage;
	const void *apPrevSnaps[8] = {nullptr};

	for(int Tick = 1; Tick < 500; Tick++)
	{
		Storage.Add(Tick, Tick * 10, SnapSize(Tick), aData, AltSize(Tick), aData);
		Storage.PurgeUntil(Tick - 3);

		// Every holder still in the storage keeps its data, and the ones that
		// were there before are still at the same address.
		int NumHolders = 0;
		for(const CSnapshotStorage::CHolder *pHolder = Storage.m_pFirst; pHolder; pHolder = pHolder->m_pNext)
		{
			const int HolderTick = pHolder->m_Tick;
			ASSERT_EQ(pHolder->m_Tagtime, HolderTick * 10);
			ASSERT_EQ((size_t)pHolder->m_SnapSize, SnapSize(HolderTick));
			ASSERT_EQ(mem_comp(pHolder->m_pSnap, aData, SnapSize(HolderTick)), 0);
			ASSERT_EQ((size_t)pHolder->m_AltSnapSize, AltSize(HolderTick));
			if(AltSize(HolderTick))
			{
				ASSERT_EQ(mem_comp(pHolder->m_pAltSnap, aData, AltSize(HolderTick)), 0);
			}
			else
			{
				ASSERT_EQ(pHolder->m_pAltSnap, nullptr);
			}
			if(HolderTick < Tick)
			{
				ASSERT_EQ(apPrevSnaps[HolderTick % 8], pHolder->m_pSnap);
			}
			ASSERT_EQ(Storage.Get(HolderTick, nullptr, nullptr, nullptr), pHolder->m_SnapSize);
			NumHolders++;
		}
		ASSERT_EQ(NumHolders, std::min(Tick, 4));

		apPrevSnaps[Tick % 8] = Storage.m_pLast->m_pSnap;
	}
}
