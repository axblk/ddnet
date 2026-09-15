/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_MAP_H
#define ENGINE_SHARED_MAP_H

#include "datafile.h"

#include <base/types.h>

#include <engine/map.h>

#include <set>

class CMapItemLayerTilemap;
class CMapItemLayerTilemap_v2;

class CMap : public IMap
{
	CDataFileReader m_DataFile;

public:
	CMap();
	~CMap() override;

	int GetDataSize(int Index) const override;
	void *GetData(int Index) override;
	void *GetDataSwapped(int Index) override;
	const char *GetDataString(int Index) override;
	[[nodiscard]] bool GetRawData(int Index, CDataFileRawData &RawData) override;
	void UnloadData(int Index) override;
	int NumData() const override;

	int GetItemSize(int Index) override;
	void *GetItem(int Index, int *pType = nullptr, int *pId = nullptr, CUuid *pUuid = nullptr) override;
	void GetType(int Type, int *pStart, int *pNum) override;
	int FindItemIndex(int Type, int Id) override;
	void *FindItem(int Type, int Id) override;
	int NumItems() const override;

	[[nodiscard]] bool Load(const char *pFullName, IStorage *pStorage, const char *pPath, int StorageType) override;
	[[nodiscard]] bool Load(IStorage *pStorage, const char *pPath, int StorageType) override;
	[[nodiscard]] bool LoadFromMemory(const char *pFullName, const void *pData, unsigned Size, const char *pPath) override;
	void Unload() override;
	bool IsLoaded() const override;
	const unsigned char *MapData() const override;

	const char *FullName() const override;
	const char *BaseName() const override;
	const char *Path() const override;
	SHA256_DIGEST Sha256() const override;
	unsigned Crc() const override;
	int Size() const override;

private:
	/**
	 * Checks over a datafile that was just opened and, if it holds up, puts it
	 * in the place of the map that is loaded now. The map is left alone when
	 * anything is wrong with the new one.
	 */
	[[nodiscard]] bool ValidateAndTake(CDataFileReader &NewDataFile);
	static bool ValidateMapVersion(CDataFileReader &NewDataFile);
	static bool ExtractTiles(class CTile *pDest, size_t DestSize, const class CTile *pSrc, size_t SrcSize);
	bool UpgradeAndValidateTilesLayerItem(CDataFileReader &NewDataFile, int GroupIndex, int LayerIndex,
		CMapItemLayerTilemap_v2 *pLayerTilemapBase, int LayerItemIndex, size_t LayerItemSize);
	bool ValidateAndUnpackTilesLayerData(CDataFileReader &NewDataFile, int GroupIndex, int LayerIndex, const CMapItemLayerTilemap *pLayerTilemap,
		const CMapItemLayerTilemap &GameLayer, std::set<int> &UsedDataIndices);
};

#endif
