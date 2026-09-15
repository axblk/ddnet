#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/logger.h>
#include <base/mem.h>

#include <engine/gfx/image_loader.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <vector>

namespace
{
	std::vector<uint8_t> CompletePngHeader(const std::array<uint8_t, 33> &Header)
	{
		static constexpr uint8_t s_aSuffix[] = {
			0x00, 0x00, 0x00, 0x00, 'I', 'D', 'A', 'T', 0x35, 0xaf, 0x06, 0x1e,
			0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82};
		std::vector<uint8_t> vPng(Header.begin(), Header.end());
		vPng.insert(vPng.end(), std::begin(s_aSuffix), std::end(s_aSuffix));
		return vPng;
	}
}

TEST(ImageLoader, ReaderRejectsOffsetOverflow)
{
	uint8_t Data = 1;
	uint8_t Result;
	CByteBufferReader Reader(&Data, 1);
	ASSERT_TRUE(Reader.Read(&Result, 1));
	EXPECT_FALSE(Reader.Read(&Result, std::numeric_limits<size_t>::max()));
	EXPECT_TRUE(Reader.Error());
}

TEST(ImageLoader, CanSuppressWorkerLogs)
{
	const uint8_t Data = 0;
	CByteBufferReader Reader(&Data, sizeof(Data));
	CImageInfo Image;
	int PngliteIncompatible;
	CMemoryLogger Logger;
	const CLogScope LogScope(&Logger);
	EXPECT_FALSE(CImageLoader::LoadPng(Reader, "worker", Image, PngliteIncompatible, false));
	EXPECT_TRUE(Logger.Lines().empty());
}

TEST(ImageLoader, ImageAllocationRejectsOverflow)
{
	CImageInfo Image;
	Image.m_Format = CImageInfo::FORMAT_RGBA;
	EXPECT_FALSE(Image.TryAllocate());

	Image.m_Width = std::numeric_limits<size_t>::max();
	Image.m_Height = 2;
	EXPECT_FALSE(Image.TryAllocate());
	EXPECT_EQ(Image.m_pData, nullptr);
}

TEST(ImageLoader, RoundTripsRgba)
{
	CImageInfo Source;
	Source.m_Width = 2;
	Source.m_Height = 1;
	Source.m_Format = CImageInfo::FORMAT_RGBA;
	ASSERT_TRUE(Source.TryAllocate());
	for(size_t i = 0; i < Source.DataSize(); ++i)
		Source.m_pData[i] = i * 31;

	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SavePng(Writer, Source));
	CByteBufferReader Reader(Writer.Data(), Writer.Size());
	CImageInfo Result;
	int PngliteIncompatible = -1;
	ASSERT_TRUE(CImageLoader::LoadPng(Reader, "roundtrip", Result, PngliteIncompatible));
	EXPECT_EQ(PngliteIncompatible, 0);
	EXPECT_TRUE(Source.DataEquals(Result));
}

TEST(ImageLoader, TruncatedPixelsKeepDestination)
{
	CImageInfo Source;
	Source.m_Width = 16;
	Source.m_Height = 16;
	Source.m_Format = CImageInfo::FORMAT_RGBA;
	ASSERT_TRUE(Source.TryAllocate());
	for(size_t i = 0; i < Source.DataSize(); ++i)
		Source.m_pData[i] = i;

	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SavePng(Writer, Source));
	ASSERT_GT(Writer.Size(), 16u);
	CByteBufferReader Reader(Writer.Data(), Writer.Size() - 16);

	CImageInfo Destination;
	Destination.m_Width = 1;
	Destination.m_Height = 1;
	Destination.m_Format = CImageInfo::FORMAT_RGBA;
	ASSERT_TRUE(Destination.TryAllocate());
	Destination.m_pData[0] = 123;
	int PngliteIncompatible;
	EXPECT_FALSE(CImageLoader::LoadPng(Reader, "truncated-pixels", Destination, PngliteIncompatible));
	EXPECT_EQ(Destination.m_Width, 1u);
	EXPECT_EQ(Destination.m_Height, 1u);
	EXPECT_EQ(Destination.m_pData[0], 123);
}

TEST(ImageLoader, RejectsOversizedFile)
{
	uint8_t Data = 0;
	CByteBufferReader Reader(&Data, CImageLoader::MAX_PNG_FILE_SIZE + 1);
	CImageInfo Image;
	int PngliteIncompatible;
	EXPECT_FALSE(CImageLoader::LoadPng(Reader, "oversized-file", Image, PngliteIncompatible));
	EXPECT_EQ(Image.m_pData, nullptr);
}

TEST(ImageLoader, RejectsOversizedDimensions)
{
	const std::array<uint8_t, 33> Header = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
		0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x06, 0x00, 0x00, 0x00, 0xc9, 0x5d, 0xdd, 0x66};
	const std::vector<uint8_t> vPng = CompletePngHeader(Header);
	CByteBufferReader Reader(vPng.data(), vPng.size());
	CImageInfo Image;
	int PngliteIncompatible;
	EXPECT_FALSE(CImageLoader::LoadPng(Reader, "oversized-dimensions", Image, PngliteIncompatible));
	EXPECT_EQ(Image.m_pData, nullptr);
}

TEST(ImageLoader, RejectsOversizedDecodedData)
{
	const std::array<uint8_t, 33> Header = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
		0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x01,
		0x08, 0x06, 0x00, 0x00, 0x00, 0x39, 0xff, 0xf7, 0xb2};
	const std::vector<uint8_t> vPng = CompletePngHeader(Header);
	CByteBufferReader Reader(vPng.data(), vPng.size());
	CImageInfo Image;
	int PngliteIncompatible;
	EXPECT_FALSE(CImageLoader::LoadPng(Reader, "oversized-decoded-data", Image, PngliteIncompatible));
	EXPECT_EQ(Image.m_pData, nullptr);
}

TEST(ImageLoader, RowWriterWritesTheSamePngAsSavePng)
{
	// A picture written a band at a time has to come out as the picture it is,
	// whatever the bands were.
	constexpr size_t Width = 37;
	constexpr size_t Height = 24;
	CImageInfo Image;
	Image.m_Width = Width;
	Image.m_Height = Height;
	Image.m_Format = CImageInfo::FORMAT_RGBA;
	std::vector<uint8_t> vPixels(Width * Height * 4);
	for(size_t Index = 0; Index < vPixels.size(); ++Index)
	{
		vPixels[Index] = static_cast<uint8_t>((Index * 7 + Index / 13) & 0xff);
	}
	Image.m_pData = vPixels.data();

	CTestInfo Info;
	CPngRowWriter Writer;
	IOHANDLE File = io_open(Info.m_aFilename, IOFLAG_WRITE);
	ASSERT_TRUE(File);
	ASSERT_TRUE(Writer.Begin(File, Info.m_aFilename, Width, Height, CImageInfo::FORMAT_RGBA));
	EXPECT_EQ(Writer.RowBytes(), Width * 4);
	// Bands of different heights, and one row on its own at the end.
	size_t Row = 0;
	for(const size_t Band : {7u, 11u, 5u})
	{
		ASSERT_TRUE(Writer.WriteRows(vPixels.data() + Row * Width * 4, Band));
		Row += Band;
	}
	ASSERT_TRUE(Writer.WriteRows(vPixels.data() + Row * Width * 4, Height - Row));
	ASSERT_TRUE(Writer.End());

	CByteBufferWriter Expected;
	ASSERT_TRUE(CImageLoader::SavePng(Expected, Image));

	IOHANDLE ReadFile = io_open(Info.m_aFilename, IOFLAG_READ);
	ASSERT_TRUE(ReadFile);
	void *pData;
	unsigned Size;
	ASSERT_TRUE(io_read_all(ReadFile, &pData, &Size));
	io_close(ReadFile);
	EXPECT_EQ(Size, Expected.Size());
	EXPECT_EQ(mem_comp(pData, Expected.Data(), std::min<size_t>(Size, Expected.Size())), 0);
	free(pData);
	EXPECT_FALSE(fs_remove(Info.m_aFilename));

	Image.m_pData = nullptr;
}

TEST(ImageLoader, RowWriterRefusesAnUnfinishedPicture)
{
	CTestInfo Info;
	CPngRowWriter Writer;
	IOHANDLE File = io_open(Info.m_aFilename, IOFLAG_WRITE);
	ASSERT_TRUE(File);
	ASSERT_TRUE(Writer.Begin(File, Info.m_aFilename, 4, 4, CImageInfo::FORMAT_RGBA));
	const std::vector<uint8_t> vRows(4 * 4 * 4);
	ASSERT_TRUE(Writer.WriteRows(vRows.data(), 3));
	// One row short.
	EXPECT_FALSE(Writer.End());
	EXPECT_FALSE(fs_remove(Info.m_aFilename));
}

TEST(ImageLoader, RowWriterRefusesMoreRowsThanTheresRoomFor)
{
	CTestInfo Info;
	CPngRowWriter Writer;
	IOHANDLE File = io_open(Info.m_aFilename, IOFLAG_WRITE);
	ASSERT_TRUE(File);
	ASSERT_TRUE(Writer.Begin(File, Info.m_aFilename, 4, 4, CImageInfo::FORMAT_RGBA));
	const std::vector<uint8_t> vRows(4 * 8 * 4);
	EXPECT_FALSE(Writer.WriteRows(vRows.data(), 8));
	EXPECT_FALSE(Writer.End());
	EXPECT_FALSE(fs_remove(Info.m_aFilename));
}
