#include "image_loader.h"

#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <cstdlib>

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

static bool ReadPngFile(IOHANDLE File, const char *pFilename, uint8_t *&pFileData, size_t &FileDataSize)
{
	pFileData = nullptr;
	FileDataSize = 0;
	const int64_t Length = io_length(File);
	if(Length > static_cast<int64_t>(CImageLoader::MAX_PNG_FILE_SIZE))
	{
		log_error("png", "file is too large. filename='%s' size=%" PRId64 " maximum=%" PRIzu, pFilename, Length, CImageLoader::MAX_PNG_FILE_SIZE);
		return false;
	}

	void *pData;
	unsigned DataSize;
	if(!io_read_all(File, &pData, &DataSize))
	{
		log_error("png", "failed to read file. filename='%s'", pFilename);
		return false;
	}
	if(DataSize > CImageLoader::MAX_PNG_FILE_SIZE)
	{
		log_error("png", "file is too large. filename='%s' size=%u maximum=%" PRIzu, pFilename, DataSize, CImageLoader::MAX_PNG_FILE_SIZE);
		free(pData);
		return false;
	}
	pFileData = static_cast<uint8_t *>(pData);
	FileDataSize = DataSize;
	return true;
}

bool CImageLoader::LoadPng(IOHANDLE File, const char *pFilename, CImageInfo &Image, int &PngliteIncompatible)
{
	if(!File)
	{
		log_error("png", "failed to open file for reading. filename='%s'", pFilename);
		return false;
	}

	uint8_t *pFileData;
	size_t FileDataSize;
	const bool ReadSuccess = ReadPngFile(File, pFilename, pFileData, FileDataSize);
	io_close(File);
	if(!ReadSuccess)
	{
		return false;
	}

	CByteBufferReader ImageReader(pFileData, FileDataSize);
	const bool LoadResult = CImageLoader::LoadPng(ImageReader, pFilename, Image, PngliteIncompatible);
	free(pFileData);
	if(!LoadResult)
	{
		log_error("png", "failed to load image from file. filename='%s'", pFilename);
		return false;
	}

	if(Image.m_Format != CImageInfo::FORMAT_RGB && Image.m_Format != CImageInfo::FORMAT_RGBA)
	{
		log_error("png", "image has unsupported format. filename='%s' format='%s'", pFilename, Image.FormatName());
		Image.Free();
		return false;
	}

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
