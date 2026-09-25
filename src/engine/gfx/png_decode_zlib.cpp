// Reading PNGs without libpng, for browser builds with WEB_PNG. The file is
// read the way libpng reads it for png_decode_libpng.cpp, so that the same
// files give the same pixels and the same errors: the chunks up to the image
// data one after the other with libpng's checks, then the image data in the
// pieces libpng reads, inflated a row at a time, and nothing after it. The
// pixels come with the transforms png_decode_libpng.cpp asks libpng for:
// palette to RGB, tRNS to alpha, 16 bits cut to the upper 8, grey below 8
// bits scaled to 8.
//
// Only IHDR, PLTE, tRNS and IDAT matter for the pixels. Of the other chunks
// only the CRC is checked; what libpng warns about their content is not
// repeated here.
#include "png_decode_zlib.h"

#include "image_loader.h"

#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
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

	enum
	{
		FILTER_NONE,
		FILTER_SUB,
		FILTER_UP,
		FILTER_AVERAGE,
		FILTER_PAETH,
		NUM_FILTERS,
	};

	constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	// The largest number a PNG may state as a length or a size
	constexpr uint32_t MAX_UINT31 = 0x7fffffff;
	// How much image data libpng reads at a time (PNG_IDAT_READ_SIZE), and
	// how much it inflates at a time after the last row and skips at a time
	// (PNG_INFLATE_BUF_SIZE)
	constexpr uint32_t IDAT_READ_SIZE = 8192;
	constexpr uint32_t BUFFER_SIZE = 1024;
	constexpr uint32_t MAX_PALETTE_SIZE = 256;

	constexpr uint32_t ChunkName(const char (&aName)[5])
	{
		return (uint32_t)aName[0] << 24 | (uint32_t)aName[1] << 16 | (uint32_t)aName[2] << 8 | (uint32_t)aName[3];
	}

	constexpr uint32_t CHUNK_IHDR = ChunkName("IHDR");
	constexpr uint32_t CHUNK_PLTE = ChunkName("PLTE");
	constexpr uint32_t CHUNK_IDAT = ChunkName("IDAT");
	constexpr uint32_t CHUNK_IEND = ChunkName("IEND");
	constexpr uint32_t CHUNK_TRNS = ChunkName("tRNS");
	constexpr uint32_t CHUNK_BKGD = ChunkName("bKGD");
	// The other chunks libpng reads itself, which it refuses before IHDR
	constexpr uint32_t KNOWN_ANCILLARY_CHUNKS[] = {
		ChunkName("cHRM"), ChunkName("cICP"), ChunkName("cLLI"),
		ChunkName("eXIf"), ChunkName("gAMA"), ChunkName("hIST"), ChunkName("iCCP"),
		ChunkName("iTXt"), ChunkName("mDCV"), ChunkName("oFFs"), ChunkName("pCAL"),
		ChunkName("pHYs"), ChunkName("sBIT"), ChunkName("sCAL"), ChunkName("sPLT"),
		ChunkName("sRGB"), ChunkName("tEXt"), ChunkName("tIME"), ChunkName("zTXt")};

	bool IsAncillary(uint32_t Name)
	{
		return (Name >> 29) & 1;
	}

	// Four letters, the third upper case
	bool ValidChunkName(uint32_t Name)
	{
		for(int i = 0; i < 4; ++i)
		{
			int Char = (Name >> (24 - i * 8)) & 0xff;
			if(i != 2)
				Char &= ~0x20;
			if(Char < 'A' || Char > 'Z')
				return false;
		}
		return true;
	}

	uint32_t ReadUint32(const uint8_t *pData)
	{
		return (uint32_t)pData[0] << 24 | (uint32_t)pData[1] << 16 | (uint32_t)pData[2] << 8 | (uint32_t)pData[3];
	}

	uint8_t Paeth(uint8_t Left, uint8_t Up, uint8_t UpLeft)
	{
		const int Estimate = (int)Left + Up - UpLeft;
		const int DistanceLeft = std::abs(Estimate - Left);
		const int DistanceUp = std::abs(Estimate - Up);
		const int DistanceUpLeft = std::abs(Estimate - UpLeft);
		if(DistanceLeft <= DistanceUp && DistanceLeft <= DistanceUpLeft)
			return Left;
		return DistanceUp <= DistanceUpLeft ? Up : UpLeft;
	}

	void Unfilter(int Filter, uint8_t *pRow, const uint8_t *pPrevious, size_t Size, size_t PixelSize)
	{
		for(size_t i = 0; i < Size; ++i)
		{
			const uint8_t Left = i >= PixelSize ? pRow[i - PixelSize] : 0;
			const uint8_t UpLeft = i >= PixelSize ? pPrevious[i - PixelSize] : 0;
			switch(Filter)
			{
			case FILTER_SUB: pRow[i] += Left; break;
			case FILTER_UP: pRow[i] += pPrevious[i]; break;
			case FILTER_AVERAGE: pRow[i] += ((unsigned)Left + pPrevious[i]) / 2; break;
			case FILTER_PAETH: pRow[i] += Paeth(Left, pPrevious[i], UpLeft); break;
			}
		}
	}

	// A pass of Adam7: where its pixels start and how far they lie apart
	class CPass
	{
	public:
		uint32_t m_X;
		uint32_t m_Y;
		uint32_t m_StepX;
		uint32_t m_StepY;
	};

	constexpr CPass ADAM7_PASSES[] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
	constexpr CPass NO_PASSES[] = {{0, 0, 1, 1}};

	class CPngDecoder
	{
		CByteBufferReader &m_Reader;
		const char *m_pContextName;
		// The chunk being read, and the CRC of what has been read of it
		uint32_t m_ChunkName = 0;
		uint32_t m_Crc = 0;

		bool m_HaveHeader = false;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		int m_BitDepth = 0;
		int m_ColorType = 0;
		int m_Interlace = 0;

		// libpng keeps 256 entries of both, black and opaque past the file's
		bool m_HavePalette = false;
		uint32_t m_NumPalette = 0;
		uint8_t m_aPalette[MAX_PALETTE_SIZE * 3] = {};
		bool m_HaveTransparency = false;
		uint8_t m_aTransparency[MAX_PALETTE_SIZE] = {};
		bool m_HaveBackground = false;
		// The colour tRNS makes transparent: grey, or red, green and blue
		uint16_t m_aTransparentColor[3] = {};

		z_stream m_Stream = {};
		bool m_StreamOpen = false;
		bool m_StreamStarted = false;
		bool m_StreamEnded = false;
		// What is left to read of the IDAT chunk being read
		uint32_t m_ImageDataLeft = 0;
		std::vector<uint8_t> m_vImageData;

		bool Fail(const char *pMessage) const
		{
			log_error("png", "error for file \"%s\": %s", m_pContextName, pMessage);
			return false;
		}

		void Warn(const char *pMessage) const
		{
			log_warn("png", "warning for file \"%s\": %s", m_pContextName, pMessage);
		}

		// What libpng says about the chunk being read, after its name
		void ChunkMessage(char *pBuffer, size_t BufferSize, const char *pMessage) const
		{
			char aName[17] = "";
			for(int Shift = 24; Shift >= 0; Shift -= 8)
			{
				const int Char = (m_ChunkName >> Shift) & 0xff;
				char aChar[5];
				if(Char < 'A' || Char > 'z' || (Char > 'Z' && Char < 'a'))
					str_format(aChar, sizeof(aChar), "[%02X]", Char);
				else
					str_format(aChar, sizeof(aChar), "%c", Char);
				str_append(aName, aChar);
			}
			str_format(pBuffer, BufferSize, "%s: %s", aName, pMessage);
		}

		bool ChunkFail(const char *pMessage) const
		{
			char aMessage[256];
			ChunkMessage(aMessage, sizeof(aMessage), pMessage);
			return Fail(aMessage);
		}

		void ChunkWarn(const char *pMessage) const
		{
			char aMessage[256];
			ChunkMessage(aMessage, sizeof(aMessage), pMessage);
			Warn(aMessage);
		}

		const char *ZlibMessage(int Result) const
		{
			if(m_Stream.msg != nullptr)
				return m_Stream.msg;
			switch(Result)
			{
			case Z_NEED_DICT: return "missing LZ dictionary";
			case Z_DATA_ERROR: return "damaged LZ stream";
			case Z_MEM_ERROR: return "insufficient memory";
			case Z_BUF_ERROR: return "truncated";
			default: return "unexpected zlib return code";
			}
		}

		bool Read(uint8_t *pData, size_t Size)
		{
			if(!m_Reader.Read(pData, Size))
				return Fail("Could not read all bytes, file was too small");
			return true;
		}

		// Reads data of the chunk, which goes into its CRC
		bool ReadData(uint8_t *pData, size_t Size)
		{
			if(!Read(pData, Size))
				return false;
			m_Crc = crc32(m_Crc, pData, Size);
			return true;
		}

		bool ReadChunkHeader(uint32_t &Length)
		{
			uint8_t aHeader[8];
			if(!Read(aHeader, sizeof(aHeader)))
				return false;
			Length = ReadUint32(aHeader);
			if(Length > MAX_UINT31)
				return Fail("PNG unsigned integer out of range");
			m_ChunkName = ReadUint32(aHeader + 4);
			m_Crc = crc32(0, aHeader + 4, 4);
			if(!ValidChunkName(m_ChunkName))
				return ChunkFail("bad header (invalid type)");
			return true;
		}

		/**
		 * Skips the rest of the chunk and checks its CRC. A wrong CRC fails a
		 * critical chunk, unless it is read like an ancillary one, and is
		 * only warned about otherwise.
		 */
		bool FinishChunk(uint32_t Skip, bool AsAncillary, bool *pCrcValid = nullptr)
		{
			uint8_t aBuffer[BUFFER_SIZE];
			while(Skip > 0)
			{
				const uint32_t Size = std::min(Skip, BUFFER_SIZE);
				if(!ReadData(aBuffer, Size))
					return false;
				Skip -= Size;
			}
			uint8_t aCrc[4];
			if(!Read(aCrc, sizeof(aCrc)))
				return false;
			const bool CrcValid = ReadUint32(aCrc) == m_Crc;
			if(pCrcValid != nullptr)
				*pCrcValid = CrcValid;
			if(!CrcValid)
			{
				if(!AsAncillary && !IsAncillary(m_ChunkName))
					return ChunkFail("CRC error");
				ChunkWarn("CRC error");
			}
			return true;
		}

		bool ReadHeader(uint32_t Length)
		{
			if(m_HaveHeader)
				return ChunkFail("out of place");
			if(Length != 13)
				return ChunkFail(Length < 13 ? "too short" : "too long");
			uint8_t aData[13];
			if(!ReadData(aData, sizeof(aData)) || !FinishChunk(0, false))
				return false;
			m_HaveHeader = true;
			m_Width = ReadUint32(aData);
			m_Height = ReadUint32(aData + 4);
			if(m_Width > MAX_UINT31 || m_Height > MAX_UINT31)
				return Fail("PNG unsigned integer out of range");
			m_BitDepth = aData[8];
			m_ColorType = aData[9];
			m_Interlace = aData[12];

			// libpng warns about everything wrong before it gives up
			bool Invalid = false;
			const auto &&Check = [&](bool Problem, const char *pWarning) {
				if(Problem)
				{
					Warn(pWarning);
					Invalid = true;
				}
			};
			Check(m_Width == 0, "Image width is zero in IHDR");
			Check(m_Width > CImageLoader::MAX_IMAGE_DIMENSION, "Image width exceeds user limit in IHDR");
			Check(m_Height == 0, "Image height is zero in IHDR");
			Check(m_Height > CImageLoader::MAX_IMAGE_DIMENSION, "Image height exceeds user limit in IHDR");
			Check(m_BitDepth != 1 && m_BitDepth != 2 && m_BitDepth != 4 && m_BitDepth != 8 && m_BitDepth != 16, "Invalid bit depth in IHDR");
			Check(m_ColorType == 1 || m_ColorType == 5 || m_ColorType > COLOR_TYPE_RGB_ALPHA, "Invalid color type in IHDR");
			Check((m_ColorType == COLOR_TYPE_PALETTE && m_BitDepth > 8) ||
					((m_ColorType == COLOR_TYPE_RGB || m_ColorType == COLOR_TYPE_GRAY_ALPHA || m_ColorType == COLOR_TYPE_RGB_ALPHA) && m_BitDepth < 8),
				"Invalid color type/bit depth combination in IHDR");
			Check(m_Interlace > 1, "Unknown interlace method in IHDR");
			Check(aData[10] != 0, "Unknown compression method in IHDR");
			Check(aData[11] != 0, "Unknown filter method in IHDR");
			if(Invalid)
				return Fail("Invalid IHDR data");
			return true;
		}

		bool ReadPalette(uint32_t Length)
		{
			// Only a suggestion for the display of other images, where libpng
			// only warns about what is wrong with it
			const bool Needed = m_ColorType == COLOR_TYPE_PALETTE;
			const char *pError = nullptr;
			if(m_HavePalette)
				pError = "duplicate";
			else if(m_ColorType == COLOR_TYPE_GRAY || m_ColorType == COLOR_TYPE_GRAY_ALPHA)
				pError = "ignored in grayscale PNG";
			else if(Length > 3 * MAX_PALETTE_SIZE || Length % 3 != 0)
				pError = "invalid";
			else if(!Needed && (m_HaveTransparency || m_HaveBackground))
				pError = "out of place";
			if(pError != nullptr)
			{
				if(!FinishChunk(Length, !Needed))
					return false;
				if(Needed)
					return ChunkFail(pError);
				ChunkWarn(pError);
				return true;
			}

			// Entries the bit depth cannot reach are left out
			m_NumPalette = std::min(Length / 3, Needed ? 1u << m_BitDepth : MAX_PALETTE_SIZE);
			if(!ReadData(m_aPalette, m_NumPalette * 3) || !FinishChunk(Length - m_NumPalette * 3, !Needed))
				return false;
			m_HavePalette = true;
			if(m_NumPalette == 0)
				return Fail("Invalid palette");
			return true;
		}

		// Only whether there is a valid one, which makes libpng drop a later
		// PLTE of an RGB image
		bool ReadBackground(uint32_t Length)
		{
			uint8_t aData[6];
			if((m_ColorType != COLOR_TYPE_RGB && m_ColorType != COLOR_TYPE_RGB_ALPHA) || Length != sizeof(aData))
				return FinishChunk(Length, false);
			bool CrcValid;
			if(!ReadData(aData, sizeof(aData)) || !FinishChunk(0, false, &CrcValid))
				return false;
			m_HaveBackground = m_HaveBackground || (CrcValid && (m_BitDepth == 16 || (aData[0] == 0 && aData[2] == 0 && aData[4] == 0)));
			return true;
		}

		bool ReadTransparency(uint32_t Length)
		{
			// Where libpng ignores the chunk, and warns about it
			const char *pIgnored = nullptr;
			if(m_HaveTransparency)
				pIgnored = "duplicate";
			else if(Length > MAX_PALETTE_SIZE)
				pIgnored = "too long";
			else if(m_ColorType == COLOR_TYPE_GRAY || m_ColorType == COLOR_TYPE_RGB)
				pIgnored = Length != (m_ColorType == COLOR_TYPE_GRAY ? 2 : 6) ? "invalid" : nullptr;
			else if(m_ColorType == COLOR_TYPE_PALETTE && !m_HavePalette)
				pIgnored = "out of place";
			else if(m_ColorType == COLOR_TYPE_PALETTE)
				pIgnored = Length > m_NumPalette || Length == 0 ? "invalid" : nullptr;
			else
				pIgnored = "invalid with alpha channel";
			if(pIgnored != nullptr)
			{
				if(!FinishChunk(Length, false))
					return false;
				ChunkWarn(pIgnored);
				return true;
			}

			uint8_t aData[MAX_PALETTE_SIZE];
			bool CrcValid;
			if(!ReadData(aData, Length) || !FinishChunk(0, false, &CrcValid))
				return false;
			if(!CrcValid)
				return true;
			if(m_ColorType == COLOR_TYPE_PALETTE)
			{
				std::fill(std::begin(m_aTransparency), std::end(m_aTransparency), 255);
				mem_copy(m_aTransparency, aData, Length);
			}
			else
			{
				bool OutOfRange = false;
				for(uint32_t i = 0; i < Length / 2; ++i)
				{
					m_aTransparentColor[i] = aData[i * 2] << 8 | aData[i * 2 + 1];
					OutOfRange = OutOfRange || (m_BitDepth < 16 && m_aTransparentColor[i] >= 1 << m_BitDepth);
				}
				if(OutOfRange)
					Warn("tRNS chunk has out-of-range samples for bit_depth");
			}
			m_HaveTransparency = true;
			return true;
		}

		// The chunks up to the image data: libpng's png_read_info
		bool ReadInfo()
		{
			while(true)
			{
				uint32_t Length;
				if(!ReadChunkHeader(Length))
					return false;
				const bool Known = m_ChunkName == CHUNK_PLTE || m_ChunkName == CHUNK_IEND || m_ChunkName == CHUNK_TRNS || m_ChunkName == CHUNK_BKGD ||
						   std::find(std::begin(KNOWN_ANCILLARY_CHUNKS), std::end(KNOWN_ANCILLARY_CHUNKS), m_ChunkName) != std::end(KNOWN_ANCILLARY_CHUNKS);
				bool Success;
				if(m_ChunkName == CHUNK_IDAT)
				{
					if(!m_HaveHeader)
						return ChunkFail("Missing IHDR before IDAT");
					if(m_ColorType == COLOR_TYPE_PALETTE && !m_HavePalette)
						return ChunkFail("Missing PLTE before IDAT");
					m_ImageDataLeft = Length;
					return true;
				}
				else if(m_ChunkName == CHUNK_IHDR)
					Success = ReadHeader(Length);
				else if(Known && !m_HaveHeader)
					Success = ChunkFail("missing IHDR");
				else if(m_ChunkName == CHUNK_IEND)
					Success = ChunkFail("out of place");
				else if(m_ChunkName == CHUNK_PLTE)
					Success = ReadPalette(Length);
				else if(m_ChunkName == CHUNK_TRNS)
					Success = ReadTransparency(Length);
				else if(m_ChunkName == CHUNK_BKGD)
					Success = ReadBackground(Length);
				else
					Success = FinishChunk(Length, false) && (Known || IsAncillary(m_ChunkName) || ChunkFail("unhandled critical chunk"));
				if(!Success)
					return false;
			}
		}

		/**
		 * Inflates image data into the buffer, or after the last row, with
		 * no buffer, what is left of it: libpng's png_read_IDAT_data, which
		 * reads the data in pieces as zlib needs it.
		 */
		bool Inflate(uint8_t *pOut, size_t Size)
		{
			uint8_t aExtra[BUFFER_SIZE];
			// Room left in the buffer, or how much was inflated after the last row
			size_t Left = pOut != nullptr ? Size : 0;
			m_Stream.next_out = pOut;
			do
			{
				if(m_Stream.avail_in == 0)
				{
					while(m_ImageDataLeft == 0)
					{
						uint32_t Length;
						if(!FinishChunk(0, false) || !ReadChunkHeader(Length))
							return false;
						if(m_ChunkName != CHUNK_IDAT)
							return Fail("Not enough image data");
						m_ImageDataLeft = Length;
					}
					const uint32_t ReadSize = std::min(m_ImageDataLeft, IDAT_READ_SIZE);
					if(!ReadData(m_vImageData.data(), ReadSize))
						return false;
					m_ImageDataLeft -= ReadSize;
					m_Stream.next_in = m_vImageData.data();
					m_Stream.avail_in = ReadSize;
				}

				if(pOut != nullptr)
				{
					m_Stream.avail_out = Left;
				}
				else
				{
					m_Stream.next_out = aExtra;
					m_Stream.avail_out = sizeof(aExtra);
				}
				int Result;
				const char *pError = nullptr;
				// libpng checks the window size itself
				if(!m_StreamStarted && (m_Stream.next_in[0] >> 4) > 7)
				{
					Result = Z_DATA_ERROR;
					pError = "invalid window size (libpng)";
				}
				else
				{
					m_StreamStarted = true;
					Result = inflate(&m_Stream, Z_NO_FLUSH);
				}
				if(pOut != nullptr)
				{
					Left = m_Stream.avail_out;
				}
				else
				{
					Left += sizeof(aExtra) - m_Stream.avail_out;
					m_Stream.next_out = nullptr;
				}
				m_Stream.avail_out = 0;

				if(Result == Z_STREAM_END)
				{
					m_StreamEnded = true;
					if(m_Stream.avail_in > 0 || m_ImageDataLeft > 0)
						ChunkWarn("Extra compressed data");
					break;
				}
				if(Result != Z_OK)
				{
					if(pError == nullptr)
						pError = ZlibMessage(Result);
					if(pOut != nullptr)
						return ChunkFail(pError);
					ChunkWarn(pError);
					return true;
				}
			} while(Left > 0);

			if(Left > 0)
			{
				if(pOut != nullptr)
					return Fail("Not enough image data");
				ChunkWarn("Too much image data");
			}
			return true;
		}

		// The rest of the image data after the last row: libpng's
		// png_read_finish_IDAT
		bool FinishImageData()
		{
			if(!m_StreamEnded && !Inflate(nullptr, 0))
				return false;
			return FinishChunk(m_ImageDataLeft, false);
		}

		int SampleCount() const
		{
			switch(m_ColorType)
			{
			case COLOR_TYPE_RGB: return 3;
			case COLOR_TYPE_GRAY_ALPHA: return 2;
			case COLOR_TYPE_RGB_ALPHA: return 4;
			default: return 1;
			}
		}

		// The channels of the image libpng gives
		int ChannelCount() const
		{
			if(m_ColorType == COLOR_TYPE_PALETTE)
				return m_HaveTransparency ? 4 : 3;
			return SampleCount() + (m_HaveTransparency ? 1 : 0);
		}

		unsigned Sample(const uint8_t *pRow, size_t Index) const
		{
			switch(m_BitDepth)
			{
			case 16: return pRow[Index * 2] << 8 | pRow[Index * 2 + 1];
			case 8: return pRow[Index];
			default:
			{
				const size_t Bit = Index * m_BitDepth;
				return (pRow[Bit / 8] >> (8 - m_BitDepth - Bit % 8)) & ((1 << m_BitDepth) - 1);
			}
			}
		}

		// The pixels of an unfiltered row of a pass into the image, `Step`
		// bytes apart
		void StoreRow(const uint8_t *pRow, size_t Width, uint8_t *pOut, size_t Step) const
		{
			const int Samples = SampleCount();
			if(m_BitDepth == 8 && m_ColorType != COLOR_TYPE_PALETTE && !m_HaveTransparency && Step == (size_t)Samples)
			{
				mem_copy(pOut, pRow, Width * Samples);
				return;
			}
			const unsigned Max = m_BitDepth == 16 ? 0xffff : (1 << m_BitDepth) - 1;
			for(size_t x = 0; x < Width; ++x, pOut += Step)
			{
				if(m_ColorType == COLOR_TYPE_PALETTE)
				{
					const unsigned Index = Sample(pRow, x);
					mem_copy(pOut, &m_aPalette[Index * 3], 3);
					if(m_HaveTransparency)
						pOut[3] = m_aTransparency[Index];
					continue;
				}
				bool Transparent = m_HaveTransparency;
				for(int i = 0; i < Samples; ++i)
				{
					const unsigned Value = Sample(pRow, x * Samples + i);
					pOut[i] = m_BitDepth == 16 ? Value >> 8 : Value * (255 / Max);
					// Compared in the bit depth of the image, the colour cut to it
					Transparent = Transparent && Value == (m_aTransparentColor[i] & Max);
				}
				if(m_HaveTransparency)
					pOut[Samples] = Transparent ? 0 : 255;
			}
		}

		bool ReadImage(CImageInfo &Image)
		{
			const size_t PixelBits = (size_t)SampleCount() * m_BitDepth;
			const size_t Channels = Image.PixelSize();
			std::vector<uint8_t> vRow(((size_t)m_Width * PixelBits + 7) / 8 + 1);
			std::vector<uint8_t> vPrevious(vRow.size());
			const CPass *pPasses = m_Interlace ? ADAM7_PASSES : NO_PASSES;
			const size_t NumPasses = m_Interlace ? std::size(ADAM7_PASSES) : std::size(NO_PASSES);
			for(size_t p = 0; p < NumPasses; ++p)
			{
				const CPass &Pass = pPasses[p];
				if(Pass.m_X >= m_Width || Pass.m_Y >= m_Height)
					continue;
				const size_t Width = (m_Width - Pass.m_X + Pass.m_StepX - 1) / Pass.m_StepX;
				const size_t RowSize = (Width * PixelBits + 7) / 8;
				std::fill(vPrevious.begin(), vPrevious.end(), 0);
				for(size_t y = Pass.m_Y; y < m_Height; y += Pass.m_StepY)
				{
					if(!Inflate(vRow.data(), RowSize + 1))
						return false;
					if(vRow[0] >= NUM_FILTERS)
						return Fail("bad adaptive filter value");
					Unfilter(vRow[0], vRow.data() + 1, vPrevious.data() + 1, RowSize, (PixelBits + 7) / 8);
					StoreRow(vRow.data() + 1, Width, Image.m_pData + (y * m_Width + Pass.m_X) * Channels, Pass.m_StepX * Channels);
					std::swap(vRow, vPrevious);
				}
			}
			return FinishImageData();
		}

		// What png_decode_libpng.cpp finds: it asks libpng after the
		// transforms, which leave neither a palette nor other than 8 bits,
		// so only the interlacing shows.
		int PngliteIncompatibility() const
		{
			if(m_Interlace == 0)
				return 0;
			log_debug("png", "interlace type %d unsupported by pnglite", m_Interlace);
			return CImageLoader::PNGLITE_INTERLACE_TYPE;
		}

	public:
		CPngDecoder(CByteBufferReader &Reader, const char *pContextName) :
			m_Reader(Reader), m_pContextName(pContextName) {}

		~CPngDecoder()
		{
			if(m_StreamOpen)
				inflateEnd(&m_Stream);
		}

		CPngDecoder(const CPngDecoder &) = delete;
		CPngDecoder &operator=(const CPngDecoder &) = delete;

		bool Decode(CImageInfo &Image, int &PngliteIncompatible)
		{
			if(!ReadInfo())
				return false;

			// The window the stream states, as libpng asks for it
			const int Result = inflateInit2(&m_Stream, 0);
			if(Result != Z_OK)
				return Fail(ZlibMessage(Result));
			m_StreamOpen = true;
			m_vImageData.resize(IDAT_READ_SIZE);

			const size_t Width = m_Width;
			const size_t Height = m_Height;
			constexpr size_t RgbaPixelSize = 4;
			if(Width > CImageLoader::MAX_IMAGE_DATA_SIZE / RgbaPixelSize || Height > CImageLoader::MAX_IMAGE_DATA_SIZE / (Width * RgbaPixelSize))
			{
				log_error("png", "decoded image is too large. filename='%s' width=%" PRIzu " height=%" PRIzu " maximum=%" PRIzu, m_pContextName, Width, Height, CImageLoader::MAX_IMAGE_DATA_SIZE);
				return false;
			}

			CImageInfo DecodedImage;
			DecodedImage.m_Width = Width;
			DecodedImage.m_Height = Height;
			switch(ChannelCount())
			{
			case 1: DecodedImage.m_Format = CImageInfo::FORMAT_R; break;
			case 2: DecodedImage.m_Format = CImageInfo::FORMAT_RA; break;
			case 3: DecodedImage.m_Format = CImageInfo::FORMAT_RGB; break;
			default: DecodedImage.m_Format = CImageInfo::FORMAT_RGBA;
			}
			if(!DecodedImage.TryAllocate())
			{
				log_error("png", "failed to allocate image data. filename='%s' size=%" PRIzu, m_pContextName, DecodedImage.DataSize());
				return false;
			}
			if(!ReadImage(DecodedImage))
				return false;

			PngliteIncompatible = PngliteIncompatibility();
			Image.Free();
			Image = std::move(DecodedImage);
			return true;
		}
	};
} // namespace

bool LoadPngWithZlib(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible)
{
	PngliteIncompatible = 0;
	if(Reader.Size() > CImageLoader::MAX_PNG_FILE_SIZE)
	{
		log_error("png", "file is too large. filename='%s' size=%" PRIzu " maximum=%" PRIzu, pContextName, Reader.Size(), CImageLoader::MAX_PNG_FILE_SIZE);
		return false;
	}

	uint8_t aSignature[sizeof(PNG_SIGNATURE)];
	if(!Reader.Read(aSignature, sizeof(aSignature)) || mem_comp(aSignature, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) != 0)
	{
		log_error("png", "file is not a valid PNG file (signature mismatch).");
		return false;
	}

	CPngDecoder Decoder(Reader, pContextName);
	return Decoder.Decode(Image, PngliteIncompatible);
}

#if defined(CONF_WEB_PNG)
bool CImageLoader::LoadPng(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible)
{
	return LoadPngWithZlib(Reader, pContextName, Image, PngliteIncompatible);
}
#endif
