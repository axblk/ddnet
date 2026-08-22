#include "image_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <png.h>

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

bool CByteBufferReader::Read(void *pData, size_t Size)
{
	if(m_Error)
		return false;

	if(Size <= m_Size - m_ReadOffset)
	{
		mem_copy(pData, &m_pData[m_ReadOffset], Size);
		m_ReadOffset += Size;
		return true;
	}
	else
	{
		m_Error = true;
		return false;
	}
}

void CByteBufferWriter::Write(const void *pData, size_t Size)
{
	if(!Size)
		return;

	const size_t WriteOffset = m_vBuffer.size();
	m_vBuffer.resize(WriteOffset + Size);
	mem_copy(&m_vBuffer[WriteOffset], pData, Size);
}

class CUserErrorStruct
{
public:
	const char *m_pContextName;
	bool m_LogErrors;
	std::jmp_buf m_JmpBuf;
};

[[noreturn]] static void PngErrorCallback(png_structp pPngStruct, png_const_charp pErrorMessage)
{
	CUserErrorStruct *pUserStruct = static_cast<CUserErrorStruct *>(png_get_error_ptr(pPngStruct));
	if(pUserStruct->m_LogErrors)
		log_error("png", "error for file \"%s\": %s", pUserStruct->m_pContextName, pErrorMessage);
	std::longjmp(pUserStruct->m_JmpBuf, 1);
}

static void PngWarningCallback(png_structp pPngStruct, png_const_charp pWarningMessage)
{
	CUserErrorStruct *pUserStruct = static_cast<CUserErrorStruct *>(png_get_error_ptr(pPngStruct));
	if(pUserStruct->m_LogErrors)
		log_warn("png", "warning for file \"%s\": %s", pUserStruct->m_pContextName, pWarningMessage);
}

static void PngReadDataCallback(png_structp pPngStruct, png_bytep pOutBytes, png_size_t ByteCountToRead)
{
	CByteBufferReader *pReader = static_cast<CByteBufferReader *>(png_get_io_ptr(pPngStruct));
	if(!pReader->Read(pOutBytes, ByteCountToRead))
	{
		png_error(pPngStruct, "Could not read all bytes, file was too small");
	}
}

static CImageInfo::EImageFormat ImageFormatFromChannelCount(int ColorChannelCount)
{
	switch(ColorChannelCount)
	{
	case 1:
		return CImageInfo::FORMAT_R;
	case 2:
		return CImageInfo::FORMAT_RA;
	case 3:
		return CImageInfo::FORMAT_RGB;
	case 4:
		return CImageInfo::FORMAT_RGBA;
	default:
		dbg_assert_failed("ColorChannelCount invalid");
	}
}

static int PngliteIncompatibility(png_structp pPngStruct, png_infop pPngInfo, bool LogErrors)
{
	int Result = 0;

	const int ColorType = png_get_color_type(pPngStruct, pPngInfo);
	switch(ColorType)
	{
	case PNG_COLOR_TYPE_GRAY:
	case PNG_COLOR_TYPE_RGB:
	case PNG_COLOR_TYPE_RGB_ALPHA:
	case PNG_COLOR_TYPE_GRAY_ALPHA:
		break;
	default:
		if(LogErrors)
			log_debug("png", "color type %d unsupported by pnglite", ColorType);
		Result |= CImageLoader::PNGLITE_COLOR_TYPE;
	}

	const int BitDepth = png_get_bit_depth(pPngStruct, pPngInfo);
	switch(BitDepth)
	{
	case 8:
	case 16:
		break;
	default:
		if(LogErrors)
			log_debug("png", "bit depth %d unsupported by pnglite", BitDepth);
		Result |= CImageLoader::PNGLITE_BIT_DEPTH;
	}

	const int InterlaceType = png_get_interlace_type(pPngStruct, pPngInfo);
	if(InterlaceType != PNG_INTERLACE_NONE)
	{
		if(LogErrors)
			log_debug("png", "interlace type %d unsupported by pnglite", InterlaceType);
		Result |= CImageLoader::PNGLITE_INTERLACE_TYPE;
	}

	if(png_get_compression_type(pPngStruct, pPngInfo) != PNG_COMPRESSION_TYPE_BASE)
	{
		if(LogErrors)
			log_debug("png", "non-default compression type unsupported by pnglite");
		Result |= CImageLoader::PNGLITE_COMPRESSION_TYPE;
	}

	if(png_get_filter_type(pPngStruct, pPngInfo) != PNG_FILTER_TYPE_BASE)
	{
		if(LogErrors)
			log_debug("png", "non-default filter type unsupported by pnglite");
		Result |= CImageLoader::PNGLITE_FILTER_TYPE;
	}

	return Result;
}

bool CImageLoader::LoadPng(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible, bool LogErrors)
{
	class CPngReadState
	{
	public:
		png_structp m_pPngStruct = nullptr;
		png_infop m_pPngInfo = nullptr;
		png_bytepp m_pRowPointers = nullptr;
		CImageInfo m_DecodedImage;
	};

	PngliteIncompatible = 0;
	if(Reader.Size() > MAX_PNG_FILE_SIZE)
	{
		if(LogErrors)
			log_error("png", "file is too large. filename='%s' size=%" PRIzu " maximum=%" PRIzu, pContextName, Reader.Size(), MAX_PNG_FILE_SIZE);
		return false;
	}

	CUserErrorStruct UserErrorStruct = {pContextName, LogErrors, {}};
	const auto pState = std::make_unique<CPngReadState>();

	if(setjmp(UserErrorStruct.m_JmpBuf))
	{
		return false;
	}

	pState->m_pPngStruct = png_create_read_struct(png_get_libpng_ver(nullptr), &UserErrorStruct, PngErrorCallback, PngWarningCallback);
	if(pState->m_pPngStruct == nullptr)
	{
		if(LogErrors)
			log_error("png", "libpng internal failure: png_create_read_struct failed.");
		return false;
	}

	const auto &&Cleanup = [&]() {
		delete[] pState->m_pRowPointers;
		pState->m_pRowPointers = nullptr;
		if(pState->m_pPngInfo != nullptr)
		{
			png_destroy_info_struct(pState->m_pPngStruct, &pState->m_pPngInfo);
		}
		png_destroy_read_struct(&pState->m_pPngStruct, nullptr, nullptr);
	};
	if(setjmp(UserErrorStruct.m_JmpBuf))
	{
		Cleanup();
		return false;
	}

	pState->m_pPngInfo = png_create_info_struct(pState->m_pPngStruct);
	if(pState->m_pPngInfo == nullptr)
	{
		Cleanup();
		if(LogErrors)
			log_error("png", "libpng internal failure: png_create_info_struct failed.");
		return false;
	}

#if defined(PNG_SET_USER_LIMITS_SUPPORTED)
	png_set_user_limits(pState->m_pPngStruct, MAX_IMAGE_DIMENSION, MAX_IMAGE_DIMENSION);
#endif

	png_byte aSignature[8];
	if(!Reader.Read(aSignature, sizeof(aSignature)) || png_sig_cmp(aSignature, 0, sizeof(aSignature)) != 0)
	{
		Cleanup();
		if(LogErrors)
			log_error("png", "file is not a valid PNG file (signature mismatch).");
		return false;
	}

	png_set_read_fn(pState->m_pPngStruct, (png_bytep)&Reader, PngReadDataCallback);
	png_set_sig_bytes(pState->m_pPngStruct, sizeof(aSignature));

	png_read_info(pState->m_pPngStruct, pState->m_pPngInfo);

	if(Reader.Error())
	{
		// error already logged
		Cleanup();
		return false;
	}

	const png_uint_32 PngWidth = png_get_image_width(pState->m_pPngStruct, pState->m_pPngInfo);
	const png_uint_32 PngHeight = png_get_image_height(pState->m_pPngStruct, pState->m_pPngInfo);
	const png_byte BitDepth = png_get_bit_depth(pState->m_pPngStruct, pState->m_pPngInfo);
	const int ColorType = png_get_color_type(pState->m_pPngStruct, pState->m_pPngInfo);

	if(PngWidth == 0 || PngHeight == 0)
	{
		if(LogErrors)
			log_error("png", "image has width (%u) or height (%u) of 0.", PngWidth, PngHeight);
		Cleanup();
		return false;
	}
	if(PngWidth > MAX_IMAGE_DIMENSION || PngHeight > MAX_IMAGE_DIMENSION)
	{
		if(LogErrors)
			log_error("png", "image dimensions are too large. filename='%s' width=%u height=%u maximum=%" PRIzu, pContextName, PngWidth, PngHeight, MAX_IMAGE_DIMENSION);
		Cleanup();
		return false;
	}
	const size_t Width = PngWidth;
	const size_t Height = PngHeight;

	if(BitDepth == 16)
	{
		png_set_strip_16(pState->m_pPngStruct);
	}
	else if(BitDepth > 8 || BitDepth == 0)
	{
		if(LogErrors)
			log_error("png", "bit depth %d not supported.", BitDepth);
		Cleanup();
		return false;
	}

	if(ColorType == PNG_COLOR_TYPE_PALETTE)
	{
		png_set_palette_to_rgb(pState->m_pPngStruct);
	}

	if(ColorType == PNG_COLOR_TYPE_GRAY && BitDepth < 8)
	{
		png_set_expand_gray_1_2_4_to_8(pState->m_pPngStruct);
	}

	if(png_get_valid(pState->m_pPngStruct, pState->m_pPngInfo, PNG_INFO_tRNS))
	{
		png_set_tRNS_to_alpha(pState->m_pPngStruct);
	}

	png_read_update_info(pState->m_pPngStruct, pState->m_pPngInfo);

	const int ColorChannelCount = png_get_channels(pState->m_pPngStruct, pState->m_pPngInfo);
	const png_size_t BytesInRow = png_get_rowbytes(pState->m_pPngStruct, pState->m_pPngInfo);
	if(ColorChannelCount < 1 || ColorChannelCount > 4 || Width > std::numeric_limits<size_t>::max() / ColorChannelCount || BytesInRow != Width * ColorChannelCount)
	{
		if(LogErrors)
			log_error("png", "invalid row size. filename='%s' width=%" PRIzu " channels=%d row_bytes=%" PRIzu, pContextName, Width, ColorChannelCount, BytesInRow);
		Cleanup();
		return false;
	}
	constexpr size_t RgbaPixelSize = 4;
	if(Width > MAX_IMAGE_DATA_SIZE / RgbaPixelSize || Height > MAX_IMAGE_DATA_SIZE / (Width * RgbaPixelSize))
	{
		if(LogErrors)
			log_error("png", "decoded image is too large. filename='%s' width=%" PRIzu " height=%" PRIzu " maximum=%" PRIzu, pContextName, Width, Height, MAX_IMAGE_DATA_SIZE);
		Cleanup();
		return false;
	}

	pState->m_DecodedImage.m_Width = Width;
	pState->m_DecodedImage.m_Height = Height;
	pState->m_DecodedImage.m_Format = ImageFormatFromChannelCount(ColorChannelCount);
	if(!pState->m_DecodedImage.TryAllocate())
	{
		if(LogErrors)
			log_error("png", "failed to allocate image data. filename='%s' size=%" PRIzu, pContextName, pState->m_DecodedImage.DataSize());
		Cleanup();
		return false;
	}
	pState->m_pRowPointers = new(std::nothrow) png_bytep[Height];
	if(pState->m_pRowPointers == nullptr)
	{
		if(LogErrors)
			log_error("png", "failed to allocate PNG row pointers. filename='%s' height=%" PRIzu, pContextName, Height);
		Cleanup();
		return false;
	}
	for(size_t y = 0; y < Height; ++y)
		pState->m_pRowPointers[y] = &pState->m_DecodedImage.m_pData[y * BytesInRow];

	png_read_image(pState->m_pPngStruct, pState->m_pRowPointers);

	if(!Reader.Error())
	{
		PngliteIncompatible = PngliteIncompatibility(pState->m_pPngStruct, pState->m_pPngInfo, LogErrors);
		Image.Free();
		Image = std::move(pState->m_DecodedImage);
	}

	Cleanup();

	return !Reader.Error();
}

static bool ReadPngFile(IOHANDLE File, const char *pFilename, uint8_t *&pFileData, size_t &FileDataSize, bool LogErrors)
{
	pFileData = nullptr;
	FileDataSize = 0;
	const int64_t Length = io_length(File);
	if(Length > static_cast<int64_t>(CImageLoader::MAX_PNG_FILE_SIZE))
	{
		if(LogErrors)
			log_error("png", "file is too large. filename='%s' size=%" PRId64 " maximum=%" PRIzu, pFilename, Length, CImageLoader::MAX_PNG_FILE_SIZE);
		return false;
	}

	size_t Capacity = Length > 0 ? static_cast<size_t>(Length) : 4096;
	Capacity = std::min(Capacity, CImageLoader::MAX_PNG_FILE_SIZE);
	pFileData = static_cast<uint8_t *>(malloc(Capacity));
	if(pFileData == nullptr)
	{
		if(LogErrors)
			log_error("png", "failed to allocate file buffer. filename='%s' size=%" PRIzu, pFilename, Capacity);
		return false;
	}

	while(true)
	{
		if(FileDataSize == Capacity)
		{
			uint8_t ExtraByte;
			if(io_read(File, &ExtraByte, 1) == 0)
				return true;
			if(Capacity == CImageLoader::MAX_PNG_FILE_SIZE)
			{
				if(LogErrors)
					log_error("png", "file is too large. filename='%s' maximum=%" PRIzu, pFilename, CImageLoader::MAX_PNG_FILE_SIZE);
				free(pFileData);
				pFileData = nullptr;
				FileDataSize = 0;
				return false;
			}

			const size_t NewCapacity = std::min(Capacity * 2, CImageLoader::MAX_PNG_FILE_SIZE);
			uint8_t *pNewFileData = static_cast<uint8_t *>(realloc(pFileData, NewCapacity));
			if(pNewFileData == nullptr)
			{
				if(LogErrors)
					log_error("png", "failed to grow file buffer. filename='%s' size=%" PRIzu, pFilename, NewCapacity);
				free(pFileData);
				pFileData = nullptr;
				FileDataSize = 0;
				return false;
			}
			pFileData = pNewFileData;
			Capacity = NewCapacity;
			pFileData[FileDataSize++] = ExtraByte;
		}

		const unsigned BytesRead = io_read(File, &pFileData[FileDataSize], static_cast<unsigned>(Capacity - FileDataSize));
		if(BytesRead == 0)
			return true;
		FileDataSize += BytesRead;
	}
}

bool CImageLoader::LoadPng(IOHANDLE File, const char *pFilename, CImageInfo &Image, int &PngliteIncompatible, bool LogErrors)
{
	std::chrono::nanoseconds ReadTime;
	std::chrono::nanoseconds DecodeTime;
	return LoadPngTimed(File, pFilename, Image, PngliteIncompatible, ReadTime, DecodeTime, LogErrors);
}

bool CImageLoader::LoadPngTimed(IOHANDLE File, const char *pFilename, CImageInfo &Image, int &PngliteIncompatible, std::chrono::nanoseconds &ReadTime, std::chrono::nanoseconds &DecodeTime, bool LogErrors)
{
	ReadTime = std::chrono::nanoseconds::zero();
	DecodeTime = std::chrono::nanoseconds::zero();
	if(!File)
	{
		if(LogErrors)
			log_error("png", "failed to open file for reading. filename='%s'", pFilename);
		return false;
	}

	uint8_t *pFileData;
	size_t FileDataSize;
	const std::chrono::nanoseconds ReadStart = time_get_nanoseconds();
	const bool ReadSuccess = ReadPngFile(File, pFilename, pFileData, FileDataSize, LogErrors);
	io_close(File);
	ReadTime = time_get_nanoseconds() - ReadStart;
	if(!ReadSuccess)
	{
		return false;
	}

	CByteBufferReader ImageReader(pFileData, FileDataSize);

	const std::chrono::nanoseconds DecodeStart = time_get_nanoseconds();
	const bool LoadResult = CImageLoader::LoadPng(ImageReader, pFilename, Image, PngliteIncompatible, LogErrors);
	DecodeTime = time_get_nanoseconds() - DecodeStart;
	free(pFileData);
	if(!LoadResult)
	{
		if(LogErrors)
			log_error("png", "failed to load image from file. filename='%s'", pFilename);
		return false;
	}

	if(Image.m_Format != CImageInfo::FORMAT_RGB && Image.m_Format != CImageInfo::FORMAT_RGBA)
	{
		if(LogErrors)
			log_error("png", "image has unsupported format. filename='%s' format='%s'", pFilename, Image.FormatName());
		Image.Free();
		return false;
	}

	return true;
}

static void PngWriteDataCallback(png_structp pPngStruct, png_bytep pOutBytes, png_size_t ByteCountToWrite)
{
	CByteBufferWriter *pWriter = static_cast<CByteBufferWriter *>(png_get_io_ptr(pPngStruct));
	pWriter->Write(pOutBytes, ByteCountToWrite);
}

static void PngOutputFlushCallback(png_structp pPngStruct)
{
	// no need to flush memory buffer
}

static int PngColorTypeFromFormat(CImageInfo::EImageFormat Format)
{
	switch(Format)
	{
	case CImageInfo::FORMAT_R:
		return PNG_COLOR_TYPE_GRAY;
	case CImageInfo::FORMAT_RA:
		return PNG_COLOR_TYPE_GRAY_ALPHA;
	case CImageInfo::FORMAT_RGB:
		return PNG_COLOR_TYPE_RGB;
	case CImageInfo::FORMAT_RGBA:
		return PNG_COLOR_TYPE_RGBA;
	default:
		dbg_assert_failed("Format invalid");
	}
}

bool CImageLoader::SavePng(CByteBufferWriter &Writer, const CImageInfo &Image)
{
	png_structp pPngStruct = png_create_write_struct(png_get_libpng_ver(nullptr), nullptr, nullptr, nullptr);
	if(pPngStruct == nullptr)
	{
		log_error("png", "libpng internal failure: png_create_write_struct failed.");
		return false;
	}

	png_infop pPngInfo = png_create_info_struct(pPngStruct);
	if(pPngInfo == nullptr)
	{
		png_destroy_read_struct(&pPngStruct, nullptr, nullptr);
		log_error("png", "libpng internal failure: png_create_info_struct failed.");
		return false;
	}

	png_set_write_fn(pPngStruct, (png_bytep)&Writer, PngWriteDataCallback, PngOutputFlushCallback);

	png_set_IHDR(pPngStruct, pPngInfo, Image.m_Width, Image.m_Height, 8, PngColorTypeFromFormat(Image.m_Format), PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(pPngStruct, pPngInfo);

	png_bytepp pRowPointers = new png_bytep[Image.m_Height];
	const int WidthBytes = Image.m_Width * Image.PixelSize();
	ptrdiff_t BufferOffset = 0;
	for(size_t y = 0; y < Image.m_Height; ++y)
	{
		pRowPointers[y] = new png_byte[WidthBytes];
		mem_copy(pRowPointers[y], Image.m_pData + BufferOffset, WidthBytes);
		BufferOffset += (ptrdiff_t)WidthBytes;
	}
	png_write_image(pPngStruct, pRowPointers);
	png_write_end(pPngStruct, pPngInfo);

	for(size_t y = 0; y < Image.m_Height; ++y)
	{
		delete[] pRowPointers[y];
	}
	delete[] pRowPointers;

	png_destroy_info_struct(pPngStruct, &pPngInfo);
	png_destroy_write_struct(&pPngStruct, nullptr);

	return true;
}

bool CImageLoader::SavePng(IOHANDLE File, const char *pFilename, const CImageInfo &Image)
{
	if(!File)
	{
		log_error("png", "failed to open file for writing. filename='%s'", pFilename);
		return false;
	}

	CByteBufferWriter Writer;
	if(!CImageLoader::SavePng(Writer, Image))
	{
		// error already logged
		io_close(File);
		return false;
	}

	const bool WriteSuccess = io_write(File, Writer.Data(), Writer.Size()) == Writer.Size();
	if(!WriteSuccess)
	{
		log_error("png", "failed to write PNG data to file. filename='%s'", pFilename);
	}
	io_close(File);
	return WriteSuccess;
}
