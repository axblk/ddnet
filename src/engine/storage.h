/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_STORAGE_H
#define ENGINE_STORAGE_H

#include "kernel.h"

#include <base/hash.h>
#include <base/types.h>

#include <memory>
#include <set>
#include <string>

enum
{
	MAX_PATHS = 16
};

class IStorage : public IInterface
{
	MACRO_INTERFACE("storage")
public:
	enum
	{
		TYPE_SAVE = 0,
		TYPE_ALL = -1,
		TYPE_ABSOLUTE = -2,
		/**
		 * Translates to TYPE_SAVE if a path is relative
		 * and to TYPE_ABSOLUTE if a path is absolute.
		 * Only usable with OpenFile, ReadFile, ReadFileStr,
		 * GetCompletePath, FileExists and FolderExists.
		 */
		TYPE_SAVE_OR_ABSOLUTE = -3,
		/**
		 * Translates to TYPE_ALL if a path is relative
		 * and to TYPE_ABSOLUTE if a path is absolute.
		 * Only usable with OpenFile, ReadFile, ReadFileStr,
		 * GetCompletePath, FileExists and FolderExists.
		 */
		TYPE_ALL_OR_ABSOLUTE = -4,
	};

	enum class EInitializationType
	{
		BASIC,
		SERVER,
		CLIENT,
	};

	virtual int NumPaths() const = 0;

	virtual void ListDirectory(int Type, const char *pPath, FS_LISTDIR_CALLBACK pfnCallback, void *pUser) = 0;
	virtual void ListDirectoryInfo(int Type, const char *pPath, FS_LISTDIR_CALLBACK_FILEINFO pfnCallback, void *pUser) = 0;
	virtual IOHANDLE OpenFile(const char *pFilename, int Flags, int Type, char *pBuffer = nullptr, int BufferSize = 0) = 0;
	virtual bool FileExists(const char *pFilename, int Type) = 0;
	virtual bool FolderExists(const char *pFilename, int Type) = 0;
	virtual bool ReadFile(const char *pFilename, int Type, void **ppResult, unsigned *pResultLen) = 0;
	virtual char *ReadFileStr(const char *pFilename, int Type) = 0;
	virtual bool RetrieveTimes(const char *pFilename, int Type, time_t *pCreated, time_t *pModified) = 0;
	virtual bool CalculateHashes(const char *pFilename, int Type, SHA256_DIGEST *pSha256, unsigned *pCrc = nullptr) = 0;
	virtual bool FindFile(const char *pFilename, const char *pPath, int Type, char *pBuffer, int BufferSize) = 0;
	virtual size_t FindFiles(const char *pFilename, const char *pPath, int Type, std::set<std::string> *pEntries) = 0;
	virtual bool RemoveFile(const char *pFilename, int Type) = 0;
	virtual bool RemoveFolder(const char *pFilename, int Type) = 0;
	virtual bool RenameFile(const char *pOldFilename, const char *pNewFilename, int Type) = 0;
	virtual bool CreateFolder(const char *pFoldername, int Type) = 0;
	virtual void GetCompletePath(int Type, const char *pDir, char *pBuffer, unsigned BufferSize) = 0;
	/**
	 * Schedules synchronization of persistent storage where required by the platform.
	 */
	virtual void SyncPersistentStorage() = 0;

	/**
	 * Hands a file to the user on a platform where they cannot reach the file
	 * system themselves. Does nothing where they can.
	 *
	 * @param pFilename Path of the file to hand over.
	 * @param Type Storage type to look the file up in.
	 */
	virtual void SendFileToUser(const char *pFilename, int Type) = 0;

	/**
	 * Asks the user for files and stores them in the given folder of TYPE_SAVE.
	 * Blocks until the user has chosen or cancelled. Does nothing on a platform
	 * where the user can reach the file system themselves.
	 *
	 * @param pFolder Folder to store the files in.
	 * @param pAccept Comma separated file extensions to offer, e.g. ".demo".
	 *
	 * @return Number of files that were stored.
	 */
	virtual int RequestFilesFromUser(const char *pFolder, const char *pAccept) = 0;

	virtual bool RemoveBinaryFile(const char *pFilename) = 0;
	virtual bool RenameBinaryFile(const char *pOldFilename, const char *pNewFilename) = 0;
	virtual const char *GetBinaryPath(const char *pFilename, char *pBuffer, unsigned BufferSize) = 0;
	virtual const char *GetBinaryPathAbsolute(const char *pFilename, char *pBuffer, unsigned BufferSize) = 0;

	static const char *FormatTmpPath(char *aBuf, unsigned BufSize, const char *pPath);
};

extern IStorage *CreateStorage(IStorage::EInitializationType InitializationType, int NumArgs, const char **ppArguments);
extern std::unique_ptr<IStorage> CreateLocalStorage();
extern std::unique_ptr<IStorage> CreateTempStorage(const char *pDirectory, int NumArgs, const char **ppArguments);

#endif
