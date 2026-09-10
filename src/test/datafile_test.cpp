#include "test.h"

#include <base/io.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/mapitems_ex.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

TEST(Datafile, ExtendedType)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";

	CTestInfo Info;

	CMapItemTest ItemTest;
	ItemTest.m_Version = 1;
	ItemTest.m_aFields[0] = 1234;
	ItemTest.m_aFields[1] = 5678;
	ItemTest.m_Field3 = 9876;
	ItemTest.m_Field4 = 5432;

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));

		Writer.AddItem(MAPITEMTYPE_TEST, 0x8000, sizeof(ItemTest), &ItemTest);

		Writer.Finish();
	}

	{
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));

		int Start, Num;
		Reader.GetType(MAPITEMTYPE_TEST, &Start, &Num);
		EXPECT_EQ(Num, 1);

		int Index = Reader.FindItemIndex(MAPITEMTYPE_TEST, 0x8000);
		EXPECT_EQ(Start, Index);
		ASSERT_GE(Index, 0);
		ASSERT_EQ(Reader.GetItemSize(Index), (int)sizeof(ItemTest));

		int Type, Id;
		const CMapItemTest *pTest = (const CMapItemTest *)Reader.GetItem(Index, &Type, &Id);
		EXPECT_EQ(pTest, Reader.FindItem(MAPITEMTYPE_TEST, 0x8000));
		EXPECT_EQ(Type, MAPITEMTYPE_TEST);
		EXPECT_EQ(Id, 0x8000);

		EXPECT_EQ(pTest->m_Version, ItemTest.m_Version);
		EXPECT_EQ(pTest->m_aFields[0], ItemTest.m_aFields[0]);
		EXPECT_EQ(pTest->m_aFields[1], ItemTest.m_aFields[1]);
		EXPECT_EQ(pTest->m_Field3, ItemTest.m_Field3);
		EXPECT_EQ(pTest->m_Field4, ItemTest.m_Field4);

		Reader.Close();
	}

	if(!HasFailure())
	{
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
	}
}

TEST(Datafile, StringData)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";

	CTestInfo Info;

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));

		EXPECT_EQ(Writer.AddDataString(""), -1); // Empty string is not added
		EXPECT_EQ(Writer.AddDataString("Abc"), 0);
		EXPECT_EQ(Writer.AddDataString("DDNet最好了"), 1);
		EXPECT_EQ(Writer.AddDataString("aβい🐘"), 2);
		EXPECT_EQ(Writer.AddData(3, "Abc"), 3); // Not zero-terminated
		EXPECT_EQ(Writer.AddData(7, "foo\0bar"), 4); // Early zero-terminator
		EXPECT_EQ(Writer.AddData(5, "xyz\xff\0"), 5); // Truncated UTF-8
		EXPECT_EQ(Writer.AddData(4, "XYZ\xff"), 6); // Truncated UTF-8 and not zero-terminated

		Writer.Finish();
	}

	{
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));

		EXPECT_EQ(Reader.GetDataString(-1000), nullptr);
		EXPECT_STREQ(Reader.GetDataString(-1), "");
		EXPECT_STREQ(Reader.GetDataString(0), "Abc");
		EXPECT_STREQ(Reader.GetDataString(1), "DDNet最好了");
		EXPECT_STREQ(Reader.GetDataString(2), "aβい🐘");
		EXPECT_EQ(Reader.GetDataString(3), nullptr);
		EXPECT_EQ(Reader.GetDataString(4), nullptr);
		EXPECT_EQ(Reader.GetDataString(5), nullptr);
		EXPECT_EQ(Reader.GetDataString(6), nullptr);
		EXPECT_EQ(Reader.GetDataString(1000), nullptr);

		Reader.Close();
	}

	if(!HasFailure())
	{
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
	}
}

TEST(Datafile, RawData)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";

	CTestInfo Info;

	std::vector<uint8_t> vData;
	vData.reserve(1000);
	for(int i = 0; i < 1000; i++)
	{
		vData.push_back(static_cast<uint8_t>(i % 251));
	}

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));
		EXPECT_EQ(Writer.AddData(vData.size(), vData.data()), 0);
		EXPECT_EQ(Writer.AddDataString("Abc"), 1);
		Writer.Finish();
	}

	{
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));

		CDataFileRawData RawData;
		EXPECT_FALSE(Reader.GetRawData(-1, RawData));
		EXPECT_FALSE(Reader.GetRawData(1000, RawData));

		// The data is uncompressed without loading it into the reader
		ASSERT_TRUE(Reader.GetRawData(0, RawData));
		EXPECT_EQ(RawData.UncompressedSize(), vData.size());
		const std::unique_ptr<uint8_t[]> pRawData = RawData.Uncompress();
		ASSERT_NE(pRawData, nullptr);
		EXPECT_TRUE(std::equal(vData.begin(), vData.end(), pRawData.get()));

		// Data that is already loaded is returned uncompressed
		EXPECT_STREQ(Reader.GetDataString(1), "Abc");
		CDataFileRawData LoadedRawData;
		ASSERT_TRUE(Reader.GetRawData(1, LoadedRawData));
		EXPECT_EQ(LoadedRawData.UncompressedSize(), 4U);
		const std::unique_ptr<uint8_t[]> pLoadedRawData = LoadedRawData.Uncompress();
		ASSERT_NE(pLoadedRawData, nullptr);
		EXPECT_STREQ(reinterpret_cast<const char *>(pLoadedRawData.get()), "Abc");

		Reader.Close();
	}

	if(!HasFailure())
	{
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
	}
}

TEST(Datafile, ReadsAfterTheFileIsGone)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";

	CTestInfo Info;

	std::vector<uint8_t> vData;
	vData.reserve(1000);
	for(int i = 0; i < 1000; i++)
	{
		vData.push_back(static_cast<uint8_t>(i % 251));
	}

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));
		EXPECT_EQ(Writer.AddData(vData.size(), vData.data()), 0);
		EXPECT_EQ(Writer.AddDataString("Abc"), 1);
		EXPECT_EQ(Writer.AddData(vData.size(), vData.data()), 2);
		Writer.Finish();
	}

	CDataFileReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));
	const int FileSize = Reader.Size();
	ASSERT_GT(FileSize, 0);

	// Overwrite the file where it stands. A reader that still went to the
	// file for its data items would read these bytes; one that read the file
	// when it opened it does not notice.
	{
		IOHANDLE OverwriteFile = pStorage->OpenFile(Info.m_aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		ASSERT_NE(OverwriteFile, nullptr);
		const std::vector<uint8_t> vZeros(FileSize, 0);
		EXPECT_EQ(io_write(OverwriteFile, vZeros.data(), vZeros.size()), vZeros.size());
		io_close(OverwriteFile);
	}

	const void *pLoadedData = Reader.GetData(0);
	ASSERT_NE(pLoadedData, nullptr);
	ASSERT_EQ(Reader.GetDataSize(0), (int)vData.size());
	EXPECT_TRUE(std::equal(vData.begin(), vData.end(), static_cast<const uint8_t *>(pLoadedData)));

	EXPECT_STREQ(Reader.GetDataString(1), "Abc");

	CDataFileRawData RawData;
	ASSERT_TRUE(Reader.GetRawData(2, RawData));
	ASSERT_EQ(RawData.UncompressedSize(), vData.size());
	const std::unique_ptr<uint8_t[]> pRawData = RawData.Uncompress();
	ASSERT_NE(pRawData, nullptr);
	EXPECT_TRUE(std::equal(vData.begin(), vData.end(), pRawData.get()));

	// Windows refuses to remove a file that is still open, so this also shows
	// that the reader left no handle behind.
	EXPECT_TRUE(pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE));

	Reader.Close();
}
