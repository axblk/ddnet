#ifndef GAME_EDITOR_EDITOR_FILES_H
#define GAME_EDITOR_EDITOR_FILES_H

#include <engine/client/asset_loader.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class IStorage;

/**
 * The files the editor reads before it can go on, such as a map and what it
 * names. A file the storage fetches rather than reads (see
 * `IStorage::FetchUrl`) is fetched here through the asset loader, so that
 * reading it later does not wait for the network on the main thread. A file
 * the storage reads is left alone and read where it is used, as always, so
 * natively nothing changes.
 */
class CEditorFiles
{
	class CFile
	{
	public:
		std::string m_Path;
		int m_StorageType;
		CTypedAssetResource<CFileAssetJob> m_Resource;
		std::vector<uint8_t> m_vData;
		bool m_Found = false;
	};
	std::vector<CFile> m_vFiles;

	CFile *Find(const char *pPath, int StorageType);
	const CFile *Find(const char *pPath, int StorageType) const;

public:
	/**
	 * Fetches a file, if the storage fetches it and it is not here yet.
	 */
	void Add(CAssetLoader &Loader, IStorage *pStorage, const char *pPath, int StorageType);
	/**
	 * Takes the files that arrived.
	 *
	 * @return Whether every file arrived or failed.
	 */
	bool Update();
	size_t NumArrived() const;
	size_t Num() const { return m_vFiles.size(); }
	/**
	 * Whether the file was fetched and failed, which a file that is not
	 * there does.
	 */
	bool IsMissing(const char *pPath, int StorageType) const;
	/**
	 * Reads a file: the bytes that were fetched for it, from the storage
	 * otherwise.
	 *
	 * @return `false` when the file is not there.
	 */
	bool ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData) const;
	/**
	 * Takes the bytes that were fetched for a file, to hand them on without a
	 * copy. Only once.
	 *
	 * @return `false` when the file was not fetched or is not there.
	 */
	bool Take(const char *pPath, int StorageType, std::vector<uint8_t> &vData);
};

#endif
