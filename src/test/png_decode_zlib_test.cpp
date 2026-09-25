// The zlib reader of browser builds against libpng: for every PNG of the data
// directory, for made-up images of every kind libpng reads, and for broken
// files, both have to give the same pixels, or refuse with the same error.
#include <base/fs.h>
#include <base/io.h>
#include <base/logger.h>
#include <base/str.h>

#include <engine/gfx/image_loader.h>
#include <engine/gfx/png_decode_zlib.h>

#include <gtest/gtest.h>
#include <zlib.h>
#if __has_include(<png.h>)
#include <png.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace
{
	enum
	{
		COLOR_TYPE_GRAY = 0,
		COLOR_TYPE_RGB = 2,
		COLOR_TYPE_PALETTE = 3,
		COLOR_TYPE_GRAY_ALPHA = 4,
		COLOR_TYPE_RGB_ALPHA = 6,
	};

	class CCollectLogger : public ILogger
	{
	public:
		std::vector<std::string> m_vErrors;
		std::vector<std::string> m_vWarnings;

		void Log(const CLogMessage *pMessage) override
		{
			if(pMessage->m_Level == LEVEL_ERROR)
				m_vErrors.emplace_back(pMessage->Message());
			else if(pMessage->m_Level == LEVEL_WARN)
				m_vWarnings.emplace_back(pMessage->Message());
		}
	};

	class CResult
	{
	public:
		bool m_Success = false;
		int m_PngliteIncompatible = 0;
		size_t m_Width = 0;
		size_t m_Height = 0;
		CImageInfo::EImageFormat m_Format = CImageInfo::FORMAT_UNDEFINED;
		std::vector<uint8_t> m_vPixels;
		std::vector<std::string> m_vErrors;
		std::vector<std::string> m_vWarnings;
	};

	using FLoadPng = bool (*)(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible);

	CResult Load(FLoadPng pfnLoadPng, const std::vector<uint8_t> &vFile)
	{
		CCollectLogger Logger;
		CResult Result;
		CImageInfo Image;
		{
			CLogScope LogScope(&Logger);
			CByteBufferReader Reader(vFile.data(), vFile.size());
			Result.m_Success = pfnLoadPng(Reader, "test.png", Image, Result.m_PngliteIncompatible);
		}
		if(Result.m_Success)
		{
			Result.m_Width = Image.m_Width;
			Result.m_Height = Image.m_Height;
			Result.m_Format = Image.m_Format;
			Result.m_vPixels.assign(Image.m_pData, Image.m_pData + Image.DataSize());
		}
		Result.m_vErrors = std::move(Logger.m_vErrors);
		Result.m_vWarnings = std::move(Logger.m_vWarnings);
		return Result;
	}

	// Whether the warnings are some of the others, in the same order
	bool IsSubsequence(const std::vector<std::string> &vSome, const std::vector<std::string> &vAll)
	{
		size_t Next = 0;
		for(const std::string &Warning : vAll)
		{
			if(Next < vSome.size() && vSome[Next] == Warning)
				++Next;
		}
		return Next == vSome.size();
	}

	std::string Describe(const std::vector<std::string> &vMessages)
	{
		std::string Result;
		for(const std::string &Message : vMessages)
			Result += Message + "\n";
		return Result;
	}

	// Compares both readers on the file and returns whether libpng read it.
	bool ExpectSame(const std::vector<uint8_t> &vFile, const std::string &What)
	{
		SCOPED_TRACE(What);
		const CResult Libpng = Load(CImageLoader::LoadPng, vFile);
		const CResult Zlib = Load(LoadPngWithZlib, vFile);
		EXPECT_EQ(Libpng.m_Success, Zlib.m_Success) << "libpng:\n"
							    << Describe(Libpng.m_vErrors) << "zlib:\n"
							    << Describe(Zlib.m_vErrors);
		EXPECT_EQ(Libpng.m_vErrors, Zlib.m_vErrors);
		EXPECT_TRUE(IsSubsequence(Zlib.m_vWarnings, Libpng.m_vWarnings)) << "libpng:\n"
										 << Describe(Libpng.m_vWarnings) << "zlib:\n"
										 << Describe(Zlib.m_vWarnings);
		EXPECT_EQ(Libpng.m_PngliteIncompatible, Zlib.m_PngliteIncompatible);
		EXPECT_EQ(Libpng.m_Width, Zlib.m_Width);
		EXPECT_EQ(Libpng.m_Height, Zlib.m_Height);
		EXPECT_EQ(Libpng.m_Format, Zlib.m_Format);
		EXPECT_TRUE(Libpng.m_vPixels == Zlib.m_vPixels);
		return Libpng.m_Success;
	}

	void PutUint32(std::vector<uint8_t> &vOut, uint32_t Value)
	{
		vOut.push_back(Value >> 24);
		vOut.push_back(Value >> 16);
		vOut.push_back(Value >> 8);
		vOut.push_back(Value);
	}

	std::vector<uint8_t> Signature()
	{
		return {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	}

	void PutChunk(std::vector<uint8_t> &vFile, const char *pType, const std::vector<uint8_t> &vData, bool BadCrc = false)
	{
		const size_t Start = vFile.size();
		PutUint32(vFile, vData.size());
		vFile.insert(vFile.end(), pType, pType + 4);
		vFile.insert(vFile.end(), vData.begin(), vData.end());
		PutUint32(vFile, crc32(0, vFile.data() + Start + 4, vData.size() + 4) ^ (BadCrc ? 1 : 0));
	}

	std::vector<uint8_t> Header(uint32_t Width, uint32_t Height, int BitDepth, int ColorType, int Interlace)
	{
		std::vector<uint8_t> vData;
		PutUint32(vData, Width);
		PutUint32(vData, Height);
		vData.push_back(BitDepth);
		vData.push_back(ColorType);
		vData.push_back(0);
		vData.push_back(0);
		vData.push_back(Interlace);
		return vData;
	}

	int SampleCount(int ColorType)
	{
		switch(ColorType)
		{
		case COLOR_TYPE_RGB: return 3;
		case COLOR_TYPE_GRAY_ALPHA: return 2;
		case COLOR_TYPE_RGB_ALPHA: return 4;
		default: return 1;
		}
	}

	std::vector<uint8_t> RandomBytes(std::mt19937 &Rng, size_t Size)
	{
		std::vector<uint8_t> vBytes(Size);
		for(uint8_t &Byte : vBytes)
			Byte = Rng();
		return vBytes;
	}

	// Rows of random bytes with random filters, pass by pass
	std::vector<uint8_t> RandomImageData(std::mt19937 &Rng, uint32_t Width, uint32_t Height, int BitDepth, int ColorType, int Interlace)
	{
		static const uint32_t s_aaPasses[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
		static const uint32_t s_aaNoPasses[1][4] = {{0, 0, 1, 1}};
		const size_t PixelBits = (size_t)SampleCount(ColorType) * BitDepth;
		std::vector<uint8_t> vData;
		for(int Pass = 0; Pass < (Interlace ? 7 : 1); ++Pass)
		{
			const uint32_t *pPass = Interlace ? s_aaPasses[Pass] : s_aaNoPasses[0];
			if(pPass[0] >= Width || pPass[1] >= Height)
				continue;
			const size_t PassWidth = (Width - pPass[0] + pPass[2] - 1) / pPass[2];
			const size_t PassHeight = (Height - pPass[1] + pPass[3] - 1) / pPass[3];
			for(size_t y = 0; y < PassHeight; ++y)
			{
				vData.push_back(Rng() % 5);
				const std::vector<uint8_t> vRow = RandomBytes(Rng, (PassWidth * PixelBits + 7) / 8);
				vData.insert(vData.end(), vRow.begin(), vRow.end());
			}
		}
		return vData;
	}

	std::vector<uint8_t> Compress(const std::vector<uint8_t> &vData, int Level)
	{
		uLongf Size = compressBound(vData.size());
		std::vector<uint8_t> vCompressed(Size);
		EXPECT_EQ(compress2(vCompressed.data(), &Size, vData.data(), vData.size(), Level), Z_OK);
		vCompressed.resize(Size);
		return vCompressed;
	}

	// The compressed data in IDAT chunks of random sizes, some of them empty
	void PutImageData(std::mt19937 &Rng, std::vector<uint8_t> &vFile, const std::vector<uint8_t> &vCompressed)
	{
		size_t Offset = 0;
		do
		{
			const size_t Size = Rng() % 4 == 0 ? vCompressed.size() - Offset : std::min<size_t>(vCompressed.size() - Offset, Rng() % 40);
			PutChunk(vFile, "IDAT", std::vector<uint8_t>(vCompressed.begin() + Offset, vCompressed.begin() + Offset + Size));
			Offset += Size;
		} while(Offset < vCompressed.size());
	}

	class CImageKind
	{
	public:
		int m_ColorType;
		int m_BitDepth;
	};

	const CImageKind IMAGE_KINDS[] = {
		{COLOR_TYPE_GRAY, 1}, {COLOR_TYPE_GRAY, 2}, {COLOR_TYPE_GRAY, 4}, {COLOR_TYPE_GRAY, 8}, {COLOR_TYPE_GRAY, 16},
		{COLOR_TYPE_RGB, 8}, {COLOR_TYPE_RGB, 16},
		{COLOR_TYPE_PALETTE, 1}, {COLOR_TYPE_PALETTE, 2}, {COLOR_TYPE_PALETTE, 4}, {COLOR_TYPE_PALETTE, 8},
		{COLOR_TYPE_GRAY_ALPHA, 8}, {COLOR_TYPE_GRAY_ALPHA, 16},
		{COLOR_TYPE_RGB_ALPHA, 8}, {COLOR_TYPE_RGB_ALPHA, 16}};

	// A valid image of random pixels, with a palette and transparency in
	// all the ways libpng takes them
	std::vector<uint8_t> RandomPng(std::mt19937 &Rng, const CImageKind &Kind, uint32_t Width, uint32_t Height, int Interlace)
	{
		std::vector<uint8_t> vFile = Signature();
		PutChunk(vFile, "IHDR", Header(Width, Height, Kind.m_BitDepth, Kind.m_ColorType, Interlace));
		if(Rng() % 3 == 0)
			PutChunk(vFile, "gAMA", {0, 0, 0xb1, 0x8f});
		const uint32_t MaxEntries = Kind.m_ColorType == COLOR_TYPE_PALETTE ? 1 << Kind.m_BitDepth : 0;
		uint32_t Entries = 0;
		if(MaxEntries > 0)
		{
			// Fewer entries than the pixels use, or more than they can
			switch(Rng() % 3)
			{
			case 0: Entries = MaxEntries; break;
			case 1: Entries = 1 + Rng() % MaxEntries; break;
			default: Entries = std::min<uint32_t>(256, MaxEntries + 1 + Rng() % 8);
			}
			PutChunk(vFile, "PLTE", RandomBytes(Rng, Entries * 3));
		}
		if(Rng() % 2 == 0)
		{
			std::vector<uint8_t> vTransparency;
			if(Kind.m_ColorType == COLOR_TYPE_PALETTE)
			{
				vTransparency = RandomBytes(Rng, 1 + Rng() % std::min(Entries, MaxEntries));
			}
			else if(Kind.m_ColorType == COLOR_TYPE_GRAY || Kind.m_ColorType == COLOR_TYPE_RGB)
			{
				// Small values to hit pixels now and then, some out of range
				for(int i = 0; i < SampleCount(Kind.m_ColorType); ++i)
				{
					const uint16_t Value = Rng() % 2 == 0 ? Rng() % 4 : Rng();
					vTransparency.push_back(Value >> 8);
					vTransparency.push_back(Value);
				}
			}
			if(!vTransparency.empty())
				PutChunk(vFile, "tRNS", vTransparency);
		}
		if(Rng() % 3 == 0)
			PutChunk(vFile, "tEXt", {'a', 0, 'b'});
		std::vector<uint8_t> vData = RandomImageData(Rng, Width, Height, Kind.m_BitDepth, Kind.m_ColorType, Interlace);
		if(Kind.m_ColorType != COLOR_TYPE_PALETTE && Kind.m_BitDepth <= 8 && Rng() % 2 == 0)
		{
			// Values from the tRNS colour, to hit it where it is small
			for(uint8_t &Byte : vData)
				Byte &= Rng() % 4 == 0 ? 0x03 : 0xff;
		}
		PutImageData(Rng, vFile, Compress(vData, Rng() % 10));
		PutChunk(vFile, "IEND", {});
		return vFile;
	}

	// The zlib reader refuses broken files the way libpng 1.6.47 and later do,
	// whose chunk handling was rewritten: older versions refuse with other
	// messages, and some files not at all.
	bool LibpngRefusesLikeTheZlibReader()
	{
#if defined(PNG_LIBPNG_VER)
		return png_access_version_number() >= 10647;
#else
		return true;
#endif
	}
} // namespace

TEST(PngDecodeZlib, DataDirectory)
{
	class CCollect
	{
	public:
		std::vector<std::string> m_vFiles;

		static int Add(const char *pName, int IsDir, int DirType, void *pUser)
		{
			(void)DirType;
			CCollect *pCollect = static_cast<CCollect *>(pUser);
			if(pName[0] == '.')
				return 0;
			const std::string Path = pCollect->m_Directory + "/" + pName;
			if(IsDir)
			{
				CCollect Inner;
				Inner.m_Directory = Path;
				fs_listdir(Path.c_str(), Add, 0, &Inner);
				pCollect->m_vFiles.insert(pCollect->m_vFiles.end(), Inner.m_vFiles.begin(), Inner.m_vFiles.end());
			}
			else if(str_endswith(pName, ".png"))
			{
				pCollect->m_vFiles.push_back(Path);
			}
			return 0;
		}

		std::string m_Directory = "data";
	};
	CCollect Collect;
	fs_listdir(Collect.m_Directory.c_str(), CCollect::Add, 0, &Collect);
	ASSERT_GT(Collect.m_vFiles.size(), 100u);

	for(const std::string &Path : Collect.m_vFiles)
	{
		IOHANDLE File = io_open(Path.c_str(), IOFLAG_READ);
		ASSERT_TRUE(File) << Path;
		void *pData;
		unsigned Size;
		ASSERT_TRUE(io_read_all(File, &pData, &Size)) << Path;
		io_close(File);
		const std::vector<uint8_t> vFile(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
		free(pData);
		EXPECT_TRUE(ExpectSame(vFile, Path)) << Path;
	}
}

TEST(PngDecodeZlib, EveryKindOfImage)
{
	std::mt19937 Rng(1);
	const uint32_t aaSizes[][2] = {{1, 1}, {2, 3}, {3, 2}, {5, 7}, {8, 8}, {9, 1}, {1, 9}, {13, 11}, {33, 17}, {70, 3}};
	for(const CImageKind &Kind : IMAGE_KINDS)
	{
		for(int Interlace = 0; Interlace < 2; ++Interlace)
		{
			for(const auto &aSize : aaSizes)
			{
				for(int Variant = 0; Variant < 8; ++Variant)
				{
					char aWhat[128];
					str_format(aWhat, sizeof(aWhat), "color type %d, bit depth %d, interlace %d, %ux%u, variant %d", Kind.m_ColorType, Kind.m_BitDepth, Interlace, aSize[0], aSize[1], Variant);
					EXPECT_TRUE(ExpectSame(RandomPng(Rng, Kind, aSize[0], aSize[1], Interlace), aWhat)) << aWhat;
				}
			}
		}
	}
}

TEST(PngDecodeZlib, LargeImages)
{
	// Image data in several pieces of what libpng reads at a time
	std::mt19937 Rng(2);
	for(const CImageKind &Kind : IMAGE_KINDS)
	{
		for(int Interlace = 0; Interlace < 2; ++Interlace)
		{
			char aWhat[128];
			str_format(aWhat, sizeof(aWhat), "color type %d, bit depth %d, interlace %d", Kind.m_ColorType, Kind.m_BitDepth, Interlace);
			EXPECT_TRUE(ExpectSame(RandomPng(Rng, Kind, 300, 200, Interlace), aWhat)) << aWhat;
		}
	}

	// What libpng writes itself
	for(const CImageInfo::EImageFormat Format : {CImageInfo::FORMAT_RGB, CImageInfo::FORMAT_RGBA, CImageInfo::FORMAT_R, CImageInfo::FORMAT_RA})
	{
		CImageInfo Image;
		Image.m_Width = 257;
		Image.m_Height = 131;
		Image.m_Format = Format;
		ASSERT_TRUE(Image.TryAllocate());
		for(size_t i = 0; i < Image.DataSize(); ++i)
			Image.m_pData[i] = (i * 7 + i / 1000) & 0xff;
		CByteBufferWriter Writer;
		ASSERT_TRUE(CImageLoader::SavePng(Writer, Image));
		EXPECT_TRUE(ExpectSame(std::vector<uint8_t>(Writer.Data(), Writer.Data() + Writer.Size()), Image.FormatName()));
	}
}

TEST(PngDecodeZlib, BrokenFiles)
{
	if(!LibpngRefusesLikeTheZlibReader())
		GTEST_SKIP() << "libpng before 1.6.47 refuses broken files differently";
	std::mt19937 Rng(3);
	for(const CImageKind &Kind : IMAGE_KINDS)
	{
		const std::vector<uint8_t> vFile = RandomPng(Rng, Kind, 6, 5, Rng() % 2);
		char aWhat[128];
		for(size_t Size = 0; Size < vFile.size(); ++Size)
		{
			str_format(aWhat, sizeof(aWhat), "color type %d, bit depth %d, cut to %d bytes", Kind.m_ColorType, Kind.m_BitDepth, (int)Size);
			ExpectSame(std::vector<uint8_t>(vFile.begin(), vFile.begin() + Size), aWhat);
		}
		for(size_t Offset = 8; Offset < vFile.size(); ++Offset)
		{
			// A changed byte, once with the CRC of its chunk as it was and
			// once with the CRC that fits, to get past the CRC check
			std::vector<uint8_t> vBroken = vFile;
			vBroken[Offset] ^= 1 << (Rng() % 8);
			str_format(aWhat, sizeof(aWhat), "color type %d, bit depth %d, byte %d changed", Kind.m_ColorType, Kind.m_BitDepth, (int)Offset);
			ExpectSame(vBroken, aWhat);
			size_t Chunk = 8;
			while(true)
			{
				const size_t Length = (size_t)vFile[Chunk] << 24 | vFile[Chunk + 1] << 16 | vFile[Chunk + 2] << 8 | vFile[Chunk + 3];
				if(Offset < Chunk + 12 + Length)
				{
					if(Offset >= Chunk + 4 && Offset < Chunk + 8 + Length)
					{
						const uint32_t Crc = crc32(0, vBroken.data() + Chunk + 4, Length + 4);
						for(int i = 0; i < 4; ++i)
							vBroken[Chunk + 8 + Length + i] = Crc >> (24 - i * 8);
						str_format(aWhat, sizeof(aWhat), "color type %d, bit depth %d, byte %d changed, CRC fixed", Kind.m_ColorType, Kind.m_BitDepth, (int)Offset);
						ExpectSame(vBroken, aWhat);
					}
					break;
				}
				Chunk += 12 + Length;
			}
		}
	}
}

TEST(PngDecodeZlib, ChunksInAnyOrder)
{
	if(!LibpngRefusesLikeTheZlibReader())
		GTEST_SKIP() << "libpng before 1.6.47 refuses broken files differently";
	// Files of random chunks, right and wrong, in random order
	std::mt19937 Rng(4);
	for(int Case = 0; Case < 4000; ++Case)
	{
		const CImageKind &Kind = IMAGE_KINDS[Rng() % std::size(IMAGE_KINDS)];
		const uint32_t Width = 1 + Rng() % 9;
		const uint32_t Height = 1 + Rng() % 9;
		const int Interlace = Rng() % 2;
		std::vector<uint8_t> vCompressed = Compress(RandomImageData(Rng, Width, Height, Kind.m_BitDepth, Kind.m_ColorType, Interlace), 6);
		switch(Rng() % 12)
		{
		case 0: // Without the Adler-32 checksum, or with a wrong one
			vCompressed.resize(vCompressed.size() - 1 - Rng() % 4);
			break;
		case 1:
			vCompressed.back() ^= 1;
			break;
		case 2: // More after the end of the stream
			vCompressed.push_back(Rng());
			break;
		case 3: // A window size zlib does not know, or a preset dictionary
			vCompressed[0] = Rng() % 2 == 0 ? 0x88 : 0x78;
			vCompressed[1] = Rng() % 2 == 0 ? 0x20 : vCompressed[1];
			break;
		case 4: // Too little image data
			vCompressed = Compress(RandomImageData(Rng, Width, Height - 1 + (Height == 1), Kind.m_BitDepth, Kind.m_ColorType, Interlace), 6);
			break;
		}

		std::vector<std::vector<uint8_t>> vvChunks;
		const auto &&Add = [&](const char *pType, const std::vector<uint8_t> &vData) {
			std::vector<uint8_t> vChunk;
			PutChunk(vChunk, pType, vData, Rng() % 16 == 0);
			vvChunks.push_back(vChunk);
		};
		const int NumChunks = Rng() % 6;
		for(int i = 0; i < NumChunks; ++i)
		{
			switch(Rng() % 9)
			{
			case 0: Add("PLTE", RandomBytes(Rng, Rng() % 4 == 0 ? Rng() % 20 : 3 * (Rng() % 20))); break;
			case 1: Add("tRNS", RandomBytes(Rng, Rng() % 8)); break;
			case 2: Add("IHDR", Header(Width, Height, Kind.m_BitDepth, Kind.m_ColorType, Interlace)); break;
			case 3: Add("IEND", {}); break;
			case 4: Add("gAMA", RandomBytes(Rng, 4)); break;
			case 5: Add("prVt", RandomBytes(Rng, Rng() % 5)); break;
			case 6: Add("CRIT", RandomBytes(Rng, Rng() % 5)); break;
			case 7: Add(Rng() % 2 == 0 ? "fcTL" : "bKGD", RandomBytes(Rng, Rng() % 2 == 0 ? 6 : Rng() % 8)); break;
			default: Add("IDAT", std::vector<uint8_t>(vCompressed.begin(), vCompressed.begin() + Rng() % (vCompressed.size() + 1))); break;
			}
		}
		std::vector<uint8_t> vFile = Signature();
		const size_t HeaderAt = Rng() % 8 == 0 ? Rng() % (vvChunks.size() + 1) : 0;
		const size_t DataAt = Rng() % (vvChunks.size() + 1);
		for(size_t i = 0; i <= vvChunks.size(); ++i)
		{
			if(i == HeaderAt)
			{
				const int BitDepth = Rng() % 16 == 0 ? Rng() % 17 : Kind.m_BitDepth;
				const int ColorType = Rng() % 16 == 0 ? Rng() % 8 : Kind.m_ColorType;
				PutChunk(vFile, "IHDR", Header(Rng() % 32 == 0 ? Rng() % 3 * 20000 : Width, Height, BitDepth, ColorType, Rng() % 32 == 0 ? 2 : Interlace), Rng() % 32 == 0);
			}
			if(i == DataAt)
			{
				PutImageData(Rng, vFile, vCompressed);
				if(Rng() % 4 == 0)
					PutChunk(vFile, "IDAT", RandomBytes(Rng, Rng() % 5));
			}
			if(i < vvChunks.size())
				vFile.insert(vFile.end(), vvChunks[i].begin(), vvChunks[i].end());
		}
		PutChunk(vFile, "IEND", {});
		char aWhat[64];
		str_format(aWhat, sizeof(aWhat), "case %d", Case);
		ExpectSame(vFile, aWhat);
	}
}

TEST(PngDecodeZlib, RefusedBeforeReadingThePixels)
{
	if(!LibpngRefusesLikeTheZlibReader())
		GTEST_SKIP() << "libpng before 1.6.47 refuses broken files differently";
	// Chunk names, lengths and sizes libpng refuses outright
	std::vector<uint8_t> vFile = Signature();
	PutChunk(vFile, "IHDR", Header(16385, 1, 8, COLOR_TYPE_RGB_ALPHA, 0));
	ExpectSame(vFile, "too wide");

	vFile = Signature();
	PutChunk(vFile, "IHDR", Header(8192, 4096, 8, COLOR_TYPE_RGB_ALPHA, 0));
	PutChunk(vFile, "IDAT", {});
	ExpectSame(vFile, "too much decoded data");

	vFile = Signature();
	PutChunk(vFile, "IH\x01R", Header(1, 1, 8, COLOR_TYPE_RGB_ALPHA, 0));
	ExpectSame(vFile, "chunk name");

	vFile = Signature();
	PutUint32(vFile, 0x80000000);
	vFile.insert(vFile.end(), {'I', 'H', 'D', 'R'});
	ExpectSame(vFile, "chunk length");

	vFile = Signature();
	PutChunk(vFile, "IHDR", {1, 2, 3});
	ExpectSame(vFile, "short header");

	vFile = {'G', 'I', 'F', '8'};
	ExpectSame(vFile, "not a PNG");
}
