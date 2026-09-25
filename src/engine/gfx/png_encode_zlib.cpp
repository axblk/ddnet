// Writing PNGs without libpng, for browser builds with WEB_PNG: 8 bits per
// channel, no interlacing, only the chunks libpng writes (IHDR, IDAT, IEND),
// the rows filtered and compressed the way libpng does it by default, so that
// the files come out the same.
#include "image_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <zlib.h>

#include <array>
#include <cstdlib>
#include <functional>
#include <vector>

namespace
{
	enum
	{
		COLOR_TYPE_GRAY = 0,
		COLOR_TYPE_RGB = 2,
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
	// How much compressed data goes into one IDAT chunk, libpng's PNG_ZBUF_SIZE
	constexpr size_t IDAT_SIZE = 8192;

	int ColorTypeFromFormat(CImageInfo::EImageFormat Format)
	{
		switch(Format)
		{
		case CImageInfo::FORMAT_R:
			return COLOR_TYPE_GRAY;
		case CImageInfo::FORMAT_RA:
			return COLOR_TYPE_GRAY_ALPHA;
		case CImageInfo::FORMAT_RGB:
			return COLOR_TYPE_RGB;
		case CImageInfo::FORMAT_RGBA:
			return COLOR_TYPE_RGB_ALPHA;
		default:
			dbg_assert_failed("Format invalid");
		}
	}

	void WriteUint32(uint8_t *pOut, uint32_t Value)
	{
		pOut[0] = Value >> 24;
		pOut[1] = Value >> 16;
		pOut[2] = Value >> 8;
		pOut[3] = Value;
	}

	// How far a filtered byte is from zero, taken as signed: what libpng
	// minimises to choose the filter of a row.
	unsigned Distance(uint8_t Value)
	{
		return Value < 128 ? Value : 256 - Value;
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

	/**
	 * Turns rows into the bytes of a PNG, which go to `m_Write` as they are
	 * ready: a chunk at a time, one IDAT whenever enough has been compressed.
	 */
	class CPngEncoder
	{
	public:
		using FWrite = std::function<bool(const uint8_t *pData, size_t Size)>;

	private:
		FWrite m_Write;
		z_stream m_Stream = {};
		bool m_StreamOpen = false;
		size_t m_PixelSize = 0;
		size_t m_RowBytes = 0;
		std::vector<uint8_t> m_vPrevious;
		// Every filter's version of the row, the filter's number first
		std::array<std::vector<uint8_t>, NUM_FILTERS> m_avFiltered;
		std::vector<uint8_t> m_vCompressed;
		size_t m_DataSize = 0;
		bool m_WroteData = false;

		bool WriteChunk(const char *pType, const uint8_t *pData, size_t Size)
		{
			uint8_t aHeader[8];
			WriteUint32(aHeader, Size);
			mem_copy(aHeader + 4, pType, 4);
			uLong Crc = crc32(0, aHeader + 4, 4);
			if(Size > 0)
				Crc = crc32(Crc, pData, Size);
			uint8_t aCrc[4];
			WriteUint32(aCrc, Crc);
			return m_Write(aHeader, sizeof(aHeader)) && (Size == 0 || m_Write(pData, Size)) && m_Write(aCrc, sizeof(aCrc));
		}

		// The zlib header of a small picture says it needs the smallest window
		// that holds it, which zlib does not write below 512 bytes. libpng's
		// optimize_cmf.
		void OptimizeZlibHeader()
		{
			uint8_t *pHeader = m_vCompressed.data();
			if(m_DataSize > 16384 || (pHeader[0] & 0x0f) != Z_DEFLATED || (pHeader[0] & 0xf0) > 0x70)
				return;
			unsigned WindowInfo = pHeader[0] >> 4;
			size_t HalfWindowSize = (size_t)1 << (WindowInfo + 7);
			if(m_DataSize > HalfWindowSize)
				return;
			do
			{
				HalfWindowSize >>= 1;
				--WindowInfo;
			} while(WindowInfo > 0 && m_DataSize <= HalfWindowSize);
			pHeader[0] = (pHeader[0] & 0x0f) | (WindowInfo << 4);
			// The check bits make the two bytes a multiple of 31
			const unsigned Flags = pHeader[1] & 0xe0;
			pHeader[1] = Flags + 0x1f - ((pHeader[0] << 8) + Flags) % 0x1f;
		}

		bool WriteCompressed()
		{
			const size_t Size = m_vCompressed.size() - m_Stream.avail_out;
			if(Size >= 2 && !m_WroteData)
				OptimizeZlibHeader();
			m_WroteData = m_WroteData || Size > 0;
			m_Stream.next_out = m_vCompressed.data();
			m_Stream.avail_out = m_vCompressed.size();
			return Size == 0 || WriteChunk("IDAT", m_vCompressed.data(), Size);
		}

		bool Compress(const uint8_t *pData, size_t Size, int Flush)
		{
			m_Stream.next_in = const_cast<Bytef *>(pData);
			m_Stream.avail_in = Size;
			while(true)
			{
				const int Result = deflate(&m_Stream, Flush);
				if(Result != Z_OK && Result != Z_STREAM_END && Result != Z_BUF_ERROR)
				{
					log_error("png", "zlib failure: %s", m_Stream.msg != nullptr ? m_Stream.msg : "deflate failed");
					return false;
				}
				// A full chunk goes out at once, like libpng does it
				if(m_Stream.avail_out == 0 && !WriteCompressed())
					return false;
				if(Flush == Z_FINISH ? Result == Z_STREAM_END : m_Stream.avail_in == 0)
					return true;
			}
		}

		// The row filtered with the filter that makes it smallest by libpng's
		// measure, the first of equal ones.
		const std::vector<uint8_t> &Filter(const uint8_t *pRow)
		{
			const uint8_t *pUp = m_vPrevious.data();
			const size_t Bpp = m_PixelSize;
			size_t Best = FILTER_NONE;
			size_t BestSum = 0;
			for(size_t Filter = FILTER_NONE; Filter < NUM_FILTERS; ++Filter)
			{
				uint8_t *pOut = m_avFiltered[Filter].data() + 1;
				size_t Sum = 0;
				for(size_t i = 0; i < m_RowBytes; ++i)
				{
					const uint8_t Left = i >= Bpp ? pRow[i - Bpp] : 0;
					const uint8_t UpLeft = i >= Bpp ? pUp[i - Bpp] : 0;
					uint8_t Predicted;
					switch(Filter)
					{
					case FILTER_SUB: Predicted = Left; break;
					case FILTER_UP: Predicted = pUp[i]; break;
					case FILTER_AVERAGE: Predicted = ((unsigned)Left + pUp[i]) / 2; break;
					case FILTER_PAETH: Predicted = Paeth(Left, pUp[i], UpLeft); break;
					default: Predicted = 0;
					}
					pOut[i] = pRow[i] - Predicted;
					Sum += Distance(pOut[i]);
					// Worse than the best already
					if(Filter != FILTER_NONE && Sum > BestSum)
						break;
				}
				if(Filter == FILTER_NONE || Sum < BestSum)
				{
					Best = Filter;
					BestSum = Sum;
				}
			}
			return m_avFiltered[Best];
		}

	public:
		CPngEncoder() = default;
		CPngEncoder(const CPngEncoder &) = delete;
		CPngEncoder &operator=(const CPngEncoder &) = delete;

		~CPngEncoder()
		{
			if(m_StreamOpen)
				deflateEnd(&m_Stream);
		}

		bool Begin(size_t Width, size_t Height, CImageInfo::EImageFormat Format, FWrite Write)
		{
			m_Write = std::move(Write);
			m_PixelSize = CImageInfo::PixelSize(Format);
			m_RowBytes = Width * m_PixelSize;

			// A smaller window for a small picture, which libpng chooses to
			// make the header say what the decoder needs
			int WindowBits = 15;
			m_DataSize = (m_RowBytes + 1) * Height;
			if(m_DataSize <= 16384)
			{
				size_t HalfWindowSize = (size_t)1 << (WindowBits - 1);
				while(m_DataSize + 262 <= HalfWindowSize)
				{
					HalfWindowSize >>= 1;
					--WindowBits;
				}
			}
			// zlib takes no 8 when it writes
			if(WindowBits == 8)
				WindowBits = 9;
			if(deflateInit2(&m_Stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, WindowBits, 8, Z_FILTERED) != Z_OK)
			{
				log_error("png", "zlib failure: deflateInit2 failed");
				return false;
			}
			m_StreamOpen = true;

			m_vPrevious.assign(m_RowBytes, 0);
			for(size_t Filter = FILTER_NONE; Filter < NUM_FILTERS; ++Filter)
			{
				m_avFiltered[Filter].resize(m_RowBytes + 1);
				m_avFiltered[Filter][0] = Filter;
			}
			m_vCompressed.resize(IDAT_SIZE);
			m_Stream.next_out = m_vCompressed.data();
			m_Stream.avail_out = m_vCompressed.size();

			uint8_t aHeader[13];
			WriteUint32(aHeader, Width);
			WriteUint32(aHeader + 4, Height);
			aHeader[8] = 8;
			aHeader[9] = ColorTypeFromFormat(Format);
			aHeader[10] = 0;
			aHeader[11] = 0;
			aHeader[12] = 0;
			return m_Write(PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) && WriteChunk("IHDR", aHeader, sizeof(aHeader));
		}

		bool WriteRow(const uint8_t *pRow)
		{
			const std::vector<uint8_t> &vFiltered = Filter(pRow);
			if(!Compress(vFiltered.data(), vFiltered.size(), Z_NO_FLUSH))
				return false;
			mem_copy(m_vPrevious.data(), pRow, m_RowBytes);
			return true;
		}

		bool End()
		{
			return Compress(nullptr, 0, Z_FINISH) && WriteCompressed() && WriteChunk("IEND", nullptr, 0);
		}
	};
} // namespace

class CPngRowWriter::CImpl
{
public:
	CPngEncoder m_Encoder;
};

CPngRowWriter::CPngRowWriter() = default;

CPngRowWriter::~CPngRowWriter()
{
	Close();
}

bool CPngRowWriter::Begin(IOHANDLE File, const char *pFilename, size_t Width, size_t Height, CImageInfo::EImageFormat Format)
{
	dbg_assert(m_pImpl == nullptr, "PNG row writer already begun");
	str_copy(m_aFilename, pFilename);
	if(!File)
	{
		log_error("png", "failed to open file for writing. filename='%s'", m_aFilename);
		m_Failed = true;
		return false;
	}
	m_File = File;
	if(Width == 0 || Height == 0)
	{
		log_error("png", "refusing to write an empty image. filename='%s'", m_aFilename);
		m_Failed = true;
		return false;
	}

	m_pImpl = std::make_unique<CImpl>();
	const bool Success = m_pImpl->m_Encoder.Begin(Width, Height, Format, [this](const uint8_t *pData, size_t Size) {
		if(io_write(m_File, pData, Size) == Size)
			return true;
		log_error("png", "failed to write PNG data to file. filename='%s'", m_aFilename);
		return false;
	});
	if(!Success)
	{
		m_Failed = true;
		return false;
	}

	m_Height = Height;
	m_RowBytes = Width * CImageInfo::PixelSize(Format);
	m_RowsWritten = 0;
	return true;
}

bool CPngRowWriter::WriteRows(const uint8_t *pRows, size_t RowCount)
{
	if(m_Failed || m_pImpl == nullptr)
		return false;
	if(m_RowsWritten + RowCount > m_Height)
	{
		log_error("png", "more rows written than the image has. filename='%s'", m_aFilename);
		m_Failed = true;
		return false;
	}
	for(size_t Row = 0; Row < RowCount; ++Row)
	{
		if(!m_pImpl->m_Encoder.WriteRow(pRows + Row * m_RowBytes))
		{
			m_Failed = true;
			return false;
		}
	}
	m_RowsWritten += RowCount;
	return true;
}

bool CPngRowWriter::End()
{
	if(m_Failed || m_pImpl == nullptr)
	{
		Close();
		return false;
	}
	if(m_RowsWritten != m_Height)
	{
		log_error("png", "image is missing %" PRIzu " rows. filename='%s'", m_Height - m_RowsWritten, m_aFilename);
		Close();
		return false;
	}
	const bool Success = m_pImpl->m_Encoder.End();
	Close();
	return Success;
}

void CPngRowWriter::Close()
{
	m_pImpl.reset();
	if(m_File)
	{
		io_close(m_File);
		m_File = nullptr;
	}
}

bool CImageLoader::SavePng(CByteBufferWriter &Writer, const CImageInfo &Image)
{
	if(Image.m_Width == 0 || Image.m_Height == 0)
	{
		log_error("png", "refusing to write an empty image.");
		return false;
	}
	CPngEncoder Encoder;
	const bool Began = Encoder.Begin(Image.m_Width, Image.m_Height, Image.m_Format, [&Writer](const uint8_t *pData, size_t Size) {
		Writer.Write(pData, Size);
		return true;
	});
	if(!Began)
		return false;
	const size_t WidthBytes = Image.m_Width * Image.PixelSize();
	for(size_t y = 0; y < Image.m_Height; ++y)
	{
		if(!Encoder.WriteRow(Image.m_pData + y * WidthBytes))
			return false;
	}
	return Encoder.End();
}
