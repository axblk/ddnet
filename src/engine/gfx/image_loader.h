#ifndef ENGINE_GFX_IMAGE_LOADER_H
#define ENGINE_GFX_IMAGE_LOADER_H

#include <base/io.h>
#include <base/types.h>

#include <engine/image.h>

#include <vector>

class CByteBufferReader
{
	const uint8_t *m_pData;
	size_t m_Size;
	size_t m_ReadOffset = 0;
	bool m_Error = false;

public:
	CByteBufferReader(const uint8_t *pData, size_t Size) :
		m_pData(pData),
		m_Size(Size) {}

	bool Read(void *pData, size_t Size);
	bool Error() const { return m_Error; }
	size_t Size() const { return m_Size; }
};

class CByteBufferWriter
{
	std::vector<uint8_t> m_vBuffer;

public:
	void Write(const void *pData, size_t Size);
	const uint8_t *Data() const { return m_vBuffer.data(); }
	size_t Size() const { return m_vBuffer.size(); }
};

/**
 * Writes a PNG a band of rows at a time.
 *
 * `SavePng` needs the whole picture in memory twice over: once as pixels and
 * once as the finished file. A picture bigger than one texture is drawn in
 * parts anyway, and one that is bigger than a texture is usually too big to
 * hold in either form, so its rows go out as they are drawn.
 *
 * The rows go from the top down and every row of the picture has to be written
 * exactly once before `End`, which is what libpng's own row interface asks for.
 */
class CPngRowWriter
{
public:
	CPngRowWriter() = default;
	~CPngRowWriter();

	CPngRowWriter(const CPngRowWriter &) = delete;
	CPngRowWriter &operator=(const CPngRowWriter &) = delete;

	/**
	 * Opens the file and writes the header. The file is closed by `End`, or by
	 * the destructor if `End` is never reached.
	 *
	 * @param File The file to write to, which is taken over.
	 * @param pFilename The name of that file, for error messages.
	 * @param Width The width of the picture in pixels.
	 * @param Height The height of the picture in pixels, which is also how many
	 * rows have to be written before `End` will finish the file.
	 * @param Format The format of the rows that will be written.
	 *
	 * @return `true` on success.
	 */
	[[nodiscard]] bool Begin(IOHANDLE File, const char *pFilename, size_t Width, size_t Height, CImageInfo::EImageFormat Format);

	/**
	 * Writes the next rows of the picture, tightly packed, top row first.
	 */
	[[nodiscard]] bool WriteRows(const uint8_t *pRows, size_t RowCount);

	/**
	 * Finishes the file. Fails if rows are still missing.
	 */
	[[nodiscard]] bool End();

	size_t RowsWritten() const { return m_RowsWritten; }
	size_t RowBytes() const { return m_RowBytes; }

private:
	void Close();

	// The libpng handles, kept opaque so that libpng stays out of this header.
	void *m_pPngStruct = nullptr;
	void *m_pPngInfo = nullptr;
	IOHANDLE m_File = nullptr;
	char m_aFilename[IO_MAX_PATH_LENGTH] = {};
	size_t m_Height = 0;
	size_t m_RowBytes = 0;
	size_t m_RowsWritten = 0;
	bool m_Failed = false;
};

class CImageLoader
{
public:
	CImageLoader() = delete;

	static constexpr size_t MAX_PNG_FILE_SIZE = 64 * 1024 * 1024;
	static constexpr size_t MAX_IMAGE_DIMENSION = 16384;
	static constexpr size_t MAX_IMAGE_DATA_SIZE = 64 * 1024 * 1024;

	enum
	{
		PNGLITE_COLOR_TYPE = 1 << 0,
		PNGLITE_BIT_DEPTH = 1 << 1,
		PNGLITE_INTERLACE_TYPE = 1 << 2,
		PNGLITE_COMPRESSION_TYPE = 1 << 3,
		PNGLITE_FILTER_TYPE = 1 << 4,
	};

	static bool LoadPng(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible, bool LogErrors = true);
	static bool LoadPng(IOHANDLE File, const char *pFilename, CImageInfo &Image, int &PngliteIncompatible, bool LogErrors = true);

	static bool SavePng(CByteBufferWriter &Writer, const CImageInfo &Image);
	static bool SavePng(IOHANDLE File, const char *pFilename, const CImageInfo &Image);
};

#endif // ENGINE_GFX_IMAGE_LOADER_H
