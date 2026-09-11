#ifndef ENGINE_CLIENT_GHOST_H
#define ENGINE_CLIENT_GHOST_H

#include <engine/ghost.h>

#include <cstdint>
#include <optional>
#include <vector>

enum
{
	MAX_ITEM_SIZE = 128,
	NUM_ITEMS_PER_CHUNK = 50,
	MAX_CHUNK_SIZE = MAX_ITEM_SIZE * NUM_ITEMS_PER_CHUNK,
};
static_assert(MAX_CHUNK_SIZE % sizeof(uint32_t) == 0, "Chunk size must be aligned with uint32_t");

// version 4-6
struct CGhostHeader
{
	unsigned char m_aMarker[8];
	unsigned char m_Version;
	char m_aOwner[MAX_NAME_LENGTH];
	char m_aMap[64];
	unsigned char m_aZeroes[sizeof(int32_t)]; // Crc before version 6
	unsigned char m_aNumTicks[sizeof(int32_t)];
	unsigned char m_aTime[sizeof(int32_t)];
	SHA256_DIGEST m_MapSha256;

	int GetTicks() const;
	int GetTime() const;
	CGhostInfo ToGhostInfo() const;
};

class CGhostItem
{
public:
	alignas(uint32_t) unsigned char m_aData[MAX_ITEM_SIZE];
	size_t m_Size;
	int m_Type;
};

class CGhostRecorder : public IGhostRecorder
{
	IOHANDLE m_File;
	char m_aFilename[IO_MAX_PATH_LENGTH];
	class IStorage *m_pStorage;

	alignas(uint32_t) char m_aBuffer[MAX_CHUNK_SIZE];
	alignas(uint32_t) char m_aBufferTemp[MAX_CHUNK_SIZE];
	char *m_pBufferPos;
	const char *m_pBufferEnd;
	int m_BufferNumItems;
	std::optional<CGhostItem> m_LastItem;

	void ResetBuffer();
	void FlushChunk();

public:
	CGhostRecorder();

	void Init(IStorage *pStorage);

	int Start(const char *pFilename, const char *pMap, const SHA256_DIGEST &MapSha256, const char *pName) override;
	void Stop(int Ticks, int Time) override;

	void WriteData(int Type, const void *pData, size_t Size) override;
	bool IsRecording() const override { return m_File != nullptr; }
};

class CGhostLoader : public IGhostLoader
{
	// A ghost is read whole and then let go of: what is kept is the file, not
	// the handle to it, and how far through it the reading has got. That is
	// what lets the bytes come from somewhere other than a file - a job that
	// fetched them, in a browser - and it is why nothing here holds a file
	// open while a race is being watched.
	std::vector<uint8_t> m_vData;
	size_t m_ReadPos = 0;
	bool m_Loaded = false;
	char m_aFilename[IO_MAX_PATH_LENGTH];
	class IStorage *m_pStorage;

	CGhostHeader m_Header;
	CGhostInfo m_Info;

	alignas(uint32_t) char m_aBuffer[MAX_CHUNK_SIZE];
	alignas(uint32_t) char m_aBufferTemp[MAX_CHUNK_SIZE];
	char *m_pBufferPos;
	const char *m_pBufferEnd;
	int m_BufferNumItems;
	int m_BufferCurItem;
	int m_BufferPrevItem;
	std::optional<CGhostItem> m_LastItem;

	void ResetBuffer();
	/**
	 * Takes the next bytes of what was loaded, as far as there are any.
	 *
	 * @param pData Where the bytes are put.
	 * @param Size How many bytes are wanted.
	 *
	 * @return How many bytes were taken, which is less than was asked for at
	 * the end of the ghost, exactly as reading a file would answer.
	 */
	size_t Read(void *pData, size_t Size);
	/**
	 * Reads and checks the header at the front of a ghost.
	 *
	 * @param Header Where the header is read to.
	 * @param pData The front of the ghost.
	 * @param Size How many bytes of the ghost are there.
	 * @param pHeaderSize Where the ghost's items begin, which depends on the
	 * version: the map's hash was only written from version 6 on.
	 * @param pFilename What the ghost is called, for the log.
	 * @param pMap The map the ghost has to belong to.
	 * @param MapSha256 The hash that map has.
	 * @param MapCrc The checksum that map has, for ghosts written before the
	 * hash was.
	 * @param LogMapMismatch Whether a ghost of another map is worth a line in
	 * the log, which it is not where every ghost of a directory is looked at.
	 *
	 * @return `true` when there is a header of this map's ghost there.
	 */
	bool ReadHeader(CGhostHeader &Header, const unsigned char *pData, size_t Size, size_t *pHeaderSize, const char *pFilename, const char *pMap, const SHA256_DIGEST &MapSha256, unsigned MapCrc, bool LogMapMismatch) const;
	bool ValidateHeader(const CGhostHeader &Header, const char *pFilename) const;
	bool CheckHeaderMap(const CGhostHeader &Header, const char *pFilename, const char *pMap, const SHA256_DIGEST &MapSha256, unsigned MapCrc, bool LogMapMismatch) const;
	bool ReadChunk(int *pType);

public:
	CGhostLoader();

	// Takes the storage explicitly so that a loader can also be created off
	// the kernel, for example inside a job.
	void Init(IStorage *pStorage);

	bool Load(const char *pFilename, const char *pMap, const SHA256_DIGEST &MapSha256, unsigned MapCrc) override;
	/**
	 * The same with the bytes of the ghost instead of the name of a file, for
	 * whoever already has them: they were downloaded, or read by a job.
	 *
	 * @param vData The whole ghost.
	 * @param pFilename What the ghost is called, for the log.
	 * @param pMap The map the ghost has to belong to.
	 * @param MapSha256 The hash that map has.
	 * @param MapCrc The checksum that map has, for ghosts written before the
	 * hash was.
	 *
	 * @return `true` on success, `false` after reporting what went wrong.
	 */
	bool LoadFromMemory(std::vector<uint8_t> vData, const char *pFilename, const char *pMap, const SHA256_DIGEST &MapSha256, unsigned MapCrc);
	void Close() override;
	const CGhostInfo *GetInfo() const override { return &m_Info; }

	bool ReadNextType(int *pType) override;
	bool ReadData(int Type, void *pData, size_t Size) override;

	bool GetGhostInfo(const char *pFilename, CGhostInfo *pGhostInfo, const char *pMap, const SHA256_DIGEST &MapSha256, unsigned MapCrc) override;
};
#endif
