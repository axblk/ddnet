#include "image_loader.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/str.h>

#include <png.h>

#include <csetjmp>
#include <limits>
#include <memory>
#include <new>

class CUserErrorStruct
{
public:
	const char *m_pContextName;
	std::jmp_buf m_JmpBuf;
};

[[noreturn]] static void PngErrorCallback(png_structp pPngStruct, png_const_charp pErrorMessage)
{
	CUserErrorStruct *pUserStruct = static_cast<CUserErrorStruct *>(png_get_error_ptr(pPngStruct));
	log_error("png", "error for file \"%s\": %s", pUserStruct->m_pContextName, pErrorMessage);
	std::longjmp(pUserStruct->m_JmpBuf, 1);
}

static void PngWarningCallback(png_structp pPngStruct, png_const_charp pWarningMessage)
{
	CUserErrorStruct *pUserStruct = static_cast<CUserErrorStruct *>(png_get_error_ptr(pPngStruct));
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

static int PngliteIncompatibility(png_structp pPngStruct, png_infop pPngInfo)
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
		log_debug("png", "bit depth %d unsupported by pnglite", BitDepth);
		Result |= CImageLoader::PNGLITE_BIT_DEPTH;
	}

	const int InterlaceType = png_get_interlace_type(pPngStruct, pPngInfo);
	if(InterlaceType != PNG_INTERLACE_NONE)
	{
		log_debug("png", "interlace type %d unsupported by pnglite", InterlaceType);
		Result |= CImageLoader::PNGLITE_INTERLACE_TYPE;
	}

	if(png_get_compression_type(pPngStruct, pPngInfo) != PNG_COMPRESSION_TYPE_BASE)
	{
		log_debug("png", "non-default compression type unsupported by pnglite");
		Result |= CImageLoader::PNGLITE_COMPRESSION_TYPE;
	}

	if(png_get_filter_type(pPngStruct, pPngInfo) != PNG_FILTER_TYPE_BASE)
	{
		log_debug("png", "non-default filter type unsupported by pnglite");
		Result |= CImageLoader::PNGLITE_FILTER_TYPE;
	}

	return Result;
}

bool CImageLoader::LoadPng(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible)
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
		log_error("png", "file is too large. filename='%s' size=%" PRIzu " maximum=%" PRIzu, pContextName, Reader.Size(), MAX_PNG_FILE_SIZE);
		return false;
	}

	CUserErrorStruct UserErrorStruct = {pContextName, {}};
	const auto pState = std::make_unique<CPngReadState>();

	if(setjmp(UserErrorStruct.m_JmpBuf))
	{
		return false;
	}

	pState->m_pPngStruct = png_create_read_struct(PNG_LIBPNG_VER_STRING, &UserErrorStruct, PngErrorCallback, PngWarningCallback);
	if(pState->m_pPngStruct == nullptr)
	{
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
		log_error("png", "image has width (%u) or height (%u) of 0.", PngWidth, PngHeight);
		Cleanup();
		return false;
	}
	if(PngWidth > MAX_IMAGE_DIMENSION || PngHeight > MAX_IMAGE_DIMENSION)
	{
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
		log_error("png", "invalid row size. filename='%s' width=%" PRIzu " channels=%d row_bytes=%" PRIzu, pContextName, Width, ColorChannelCount, BytesInRow);
		Cleanup();
		return false;
	}
	constexpr size_t RgbaPixelSize = 4;
	if(Width > MAX_IMAGE_DATA_SIZE / RgbaPixelSize || Height > MAX_IMAGE_DATA_SIZE / (Width * RgbaPixelSize))
	{
		log_error("png", "decoded image is too large. filename='%s' width=%" PRIzu " height=%" PRIzu " maximum=%" PRIzu, pContextName, Width, Height, MAX_IMAGE_DATA_SIZE);
		Cleanup();
		return false;
	}

	pState->m_DecodedImage.m_Width = Width;
	pState->m_DecodedImage.m_Height = Height;
	pState->m_DecodedImage.m_Format = ImageFormatFromChannelCount(ColorChannelCount);
	if(!pState->m_DecodedImage.TryAllocate())
	{
		log_error("png", "failed to allocate image data. filename='%s' size=%" PRIzu, pContextName, pState->m_DecodedImage.DataSize());
		Cleanup();
		return false;
	}
	pState->m_pRowPointers = new(std::nothrow) png_bytep[Height];
	if(pState->m_pRowPointers == nullptr)
	{
		log_error("png", "failed to allocate PNG row pointers. filename='%s' height=%" PRIzu, pContextName, Height);
		Cleanup();
		return false;
	}
	for(size_t y = 0; y < Height; ++y)
		pState->m_pRowPointers[y] = &pState->m_DecodedImage.m_pData[y * BytesInRow];

	png_read_image(pState->m_pPngStruct, pState->m_pRowPointers);

	if(!Reader.Error())
	{
		PngliteIncompatible = PngliteIncompatibility(pState->m_pPngStruct, pState->m_pPngInfo);
		Image.Free();
		Image = std::move(pState->m_DecodedImage);
	}

	Cleanup();

	return !Reader.Error();
}
