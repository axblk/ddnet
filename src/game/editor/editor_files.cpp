#include "editor_files.h"

#include <base/io.h>
#include <base/log.h>
#include <base/str.h>

#include <engine/storage.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>

CEditorFiles::CFile *CEditorFiles::Find(const char *pPath, int StorageType)
{
	const auto It = std::find_if(m_vFiles.begin(), m_vFiles.end(), [&](const CFile &File) {
		return File.m_StorageType == StorageType && File.m_Path == pPath;
	});
	return It == m_vFiles.end() ? nullptr : &*It;
}

const CEditorFiles::CFile *CEditorFiles::Find(const char *pPath, int StorageType) const
{
	return const_cast<CEditorFiles *>(this)->Find(pPath, StorageType);
}

void CEditorFiles::Add(CAssetLoader &Loader, IStorage *pStorage, const char *pPath, int StorageType)
{
	if(Find(pPath, StorageType) != nullptr)
		return;
	char aUrl[IO_MAX_PATH_LENGTH * 2];
	if(!pStorage->FetchUrl(pPath, StorageType, aUrl, sizeof(aUrl)))
		return;
	CFile &File = m_vFiles.emplace_back();
	File.m_Path = pPath;
	File.m_StorageType = StorageType;
	File.m_Resource = Loader.Load(std::make_shared<CFileAssetJob>(pStorage, pPath, StorageType), EAssetPriority::URGENT);
}

bool CEditorFiles::Update()
{
	bool AllArrived = true;
	for(CFile &File : m_vFiles)
	{
		if(!File.m_Resource)
			continue;
		if(!File.m_Resource.IsFinished())
		{
			AllArrived = false;
			continue;
		}
		if(File.m_Resource.IsReady())
		{
			File.m_vData = File.m_Resource.Result().TakeBytes();
			File.m_Found = true;
		}
		File.m_Resource.Reset();
	}
	return AllArrived;
}

size_t CEditorFiles::NumArrived() const
{
	return std::count_if(m_vFiles.begin(), m_vFiles.end(), [](const CFile &File) { return !File.m_Resource; });
}

bool CEditorFiles::IsMissing(const char *pPath, int StorageType) const
{
	const CFile *pFile = Find(pPath, StorageType);
	return pFile != nullptr && !pFile->m_Resource && !pFile->m_Found;
}

bool CEditorFiles::ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData) const
{
	if(const CFile *pFile = Find(pPath, StorageType))
	{
		if(!pFile->m_Found)
			return false;
		vData = pFile->m_vData;
		return true;
	}
	void *pData;
	unsigned Size;
	if(!pStorage->ReadFile(pPath, StorageType, &pData, &Size))
		return false;
	vData.assign(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
	free(pData);
	return true;
}

bool CEditorFiles::Take(const char *pPath, int StorageType, std::vector<uint8_t> &vData)
{
	CFile *pFile = Find(pPath, StorageType);
	if(pFile == nullptr || !pFile->m_Found)
		return false;
	vData = std::move(pFile->m_vData);
	return true;
}
