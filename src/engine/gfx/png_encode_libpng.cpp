#include "image_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>

#include <png.h>

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

static void PngFileWriteDataCallback(png_structp pPngStruct, png_bytep pOutBytes, png_size_t ByteCountToWrite)
{
	IOHANDLE File = static_cast<IOHANDLE>(png_get_io_ptr(pPngStruct));
	io_write(File, pOutBytes, ByteCountToWrite);
}

static void PngFileFlushCallback(png_structp pPngStruct)
{
	io_flush(static_cast<IOHANDLE>(png_get_io_ptr(pPngStruct)));
}

class CPngRowWriter::CImpl
{
public:
	png_structp m_pPngStruct = nullptr;
	png_infop m_pPngInfo = nullptr;
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

	png_structp pPngStruct = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if(pPngStruct == nullptr)
	{
		log_error("png", "libpng internal failure: png_create_write_struct failed.");
		m_Failed = true;
		return false;
	}
	png_infop pPngInfo = png_create_info_struct(pPngStruct);
	if(pPngInfo == nullptr)
	{
		png_destroy_write_struct(&pPngStruct, nullptr);
		log_error("png", "libpng internal failure: png_create_info_struct failed.");
		m_Failed = true;
		return false;
	}
	m_pImpl = std::make_unique<CImpl>();
	m_pImpl->m_pPngStruct = pPngStruct;
	m_pImpl->m_pPngInfo = pPngInfo;

	png_set_write_fn(pPngStruct, m_File, PngFileWriteDataCallback, PngFileFlushCallback);
	png_set_IHDR(pPngStruct, pPngInfo, Width, Height, 8, PngColorTypeFromFormat(Format), PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(pPngStruct, pPngInfo);

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
	png_structp pPngStruct = m_pImpl->m_pPngStruct;
	for(size_t Row = 0; Row < RowCount; ++Row)
	{
		// libpng does not write through the row it is given, but its interface
		// does not say so.
		png_write_row(pPngStruct, const_cast<png_bytep>(pRows + Row * m_RowBytes));
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
	png_write_end(m_pImpl->m_pPngStruct, m_pImpl->m_pPngInfo);
	Close();
	return true;
}

void CPngRowWriter::Close()
{
	if(m_pImpl != nullptr)
	{
		png_destroy_info_struct(m_pImpl->m_pPngStruct, &m_pImpl->m_pPngInfo);
		png_destroy_write_struct(&m_pImpl->m_pPngStruct, nullptr);
		m_pImpl.reset();
	}
	if(m_File)
	{
		io_close(m_File);
		m_File = nullptr;
	}
}

bool CImageLoader::SavePng(CByteBufferWriter &Writer, const CImageInfo &Image)
{
	png_structp pPngStruct = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
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

	const size_t WidthBytes = Image.m_Width * Image.PixelSize();
	for(size_t y = 0; y < Image.m_Height; ++y)
	{
		png_write_row(pPngStruct, Image.m_pData + y * WidthBytes);
	}
	png_write_end(pPngStruct, pPngInfo);

	png_destroy_info_struct(pPngStruct, &pPngInfo);
	png_destroy_write_struct(&pPngStruct, nullptr);

	return true;
}
