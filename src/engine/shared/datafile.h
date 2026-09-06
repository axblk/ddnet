/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_DATAFILE_H
#define ENGINE_SHARED_DATAFILE_H

#include "uuid_manager.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/storage.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

enum
{
	ITEMTYPE_EX = 0xFFFF,
};

/**
 * The contents of one data item as they are stored in the file.
 *
 * Reading the bytes must happen on the thread that owns the file handle,
 * whereas uncompressing them does not touch the file and can be done on any
 * thread, for example on a job thread while the main thread continues.
 */
class CDataFileRawData
{
	std::vector<uint8_t> m_vData;
	size_t m_UncompressedSize = 0;
	bool m_Compressed = false;

public:
	CDataFileRawData() = default;
	CDataFileRawData(std::vector<uint8_t> vData, size_t UncompressedSize, bool Compressed);

	/**
	 * Returns the size that the data has after uncompressing it.
	 *
	 * @return Size of the uncompressed data.
	 */
	size_t UncompressedSize() const { return m_UncompressedSize; }

	/**
	 * Uncompresses the data into a newly allocated buffer.
	 *
	 * @return Buffer of `UncompressedSize()` bytes or `nullptr` if the data is
	 * corrupt or the memory could not be allocated.
	 */
	[[nodiscard]] std::unique_ptr<uint8_t[]> Uncompress() const;
};

/**
 * The data processor function is called after a particular data item is first successfully uncompressed.
 * The function can validate the data and return `std::make_pair(nullptr, 0)` on error or the original data
 * on success. The function can also replace the data and return a new data pointer and size. The new data
 * must be allocated with `malloc`. When returning different data or on errors, the original data must be
 * freed by the function using `free`.
 */
typedef std::function<std::pair<void *, size_t>(void *pData, size_t Size)> FDataProcessor;

// raw datafile access
class CDataFileReader
{
	class CDatafile *m_pDataFile = nullptr;

	int GetExternalItemType(int InternalType, CUuid *pUuid);
	int GetInternalItemType(int ExternalType);

public:
	~CDataFileReader();
	CDataFileReader &operator=(CDataFileReader &&Other);

	[[nodiscard]] bool Open(const char *pFullName, IStorage *pStorage, const char *pPath, int StorageType);
	[[nodiscard]] bool Open(IStorage *pStorage, const char *pPath, int StorageType);
	void Close();
	bool IsOpen() const;
	IOHANDLE File() const;

	int GetDataSize(int Index) const;
	void *GetData(int Index);
	void *GetDataSwapped(int Index); // makes sure that the data is 32bit LE ints when saved
	const char *GetDataString(int Index);
	/**
	 * Reads the stored bytes of a data item without uncompressing them.
	 *
	 * The data is not cached, so `UnloadData` must not be used for it. Data
	 * that is already loaded or that is being intercepted is returned
	 * uncompressed instead, so the result always matches `GetData`.
	 *
	 * @param Index Index of the data item.
	 * @param RawData Receives the data of the item.
	 *
	 * @return `true` on success, `false` if the item does not exist or could
	 * not be read.
	 */
	[[nodiscard]] bool GetRawData(int Index, CDataFileRawData &RawData);
	void AddDataProcessor(int Index, FDataProcessor DataProcessor);
	void UnloadData(int Index);
	int NumData() const;

	int GetItemSize(int Index) const;
	void *GetItem(int Index, int *pType = nullptr, int *pId = nullptr, CUuid *pUuid = nullptr);
	void GetType(int Type, int *pStart, int *pNum);
	int FindItemIndex(int Type, int Id);
	void *FindItem(int Type, int Id);
	bool OverrideItemData(int Index, const void *pData, size_t Size);
	int NumItems() const;

	const char *FullName() const;
	const char *BaseName() const;
	const char *Path() const;
	SHA256_DIGEST Sha256() const;
	unsigned Crc() const;
	int Size() const;
};

// write access
class CDataFileWriter
{
public:
	enum ECompressionLevel
	{
		COMPRESSION_DEFAULT,
		COMPRESSION_BEST,
	};

private:
	class CDataInfo
	{
	public:
		void *m_pUncompressedData;
		int m_UncompressedSize;
		void *m_pCompressedData;
		int m_CompressedSize;
		ECompressionLevel m_CompressionLevel;
	};

	class CItemInfo
	{
	public:
		int m_Type;
		int m_Id;
		int m_Size;
		int m_Next;
		int m_Prev;
		void *m_pData;
	};

	class CItemTypeInfo
	{
	public:
		int m_Num = 0;
		int m_First = -1;
		int m_Last = -1;
	};

	class CExtendedItemType
	{
	public:
		int m_Type;
		CUuid m_Uuid;
	};

	IOHANDLE m_File;
	std::map<uint16_t, CItemTypeInfo, std::less<>> m_ItemTypes; // item types must be sorted in ascending order
	std::vector<CItemInfo> m_vItems;
	std::vector<CDataInfo> m_vDatas;
	std::vector<CExtendedItemType> m_vExtendedItemTypes;

	int GetTypeFromIndex(int Index) const;
	int GetExtendedItemTypeIndex(int Type, const CUuid *pUuid);

public:
	CDataFileWriter();
	CDataFileWriter(CDataFileWriter &&Other)
	{
		m_File = Other.m_File;
		Other.m_File = nullptr;
		m_ItemTypes = std::move(Other.m_ItemTypes);
		m_vItems = std::move(Other.m_vItems);
		m_vDatas = std::move(Other.m_vDatas);
		m_vExtendedItemTypes = std::move(Other.m_vExtendedItemTypes);
	}
	~CDataFileWriter();

	[[nodiscard]] bool Open(class IStorage *pStorage, const char *pFilename, int StorageType = IStorage::TYPE_SAVE);
	int AddItem(int Type, int Id, size_t Size, const void *pData, const CUuid *pUuid = nullptr);
	int AddData(size_t Size, const void *pData, ECompressionLevel CompressionLevel = COMPRESSION_DEFAULT);
	int AddDataSwapped(size_t Size, const void *pData);
	int AddDataString(const char *pStr);
	void Finish();
};

#endif
