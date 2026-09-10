/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_FONT_LOADING_H
#define ENGINE_CLIENT_FONT_LOADING_H

#include <cstddef>
#include <string>
#include <vector>

/**
 * The font index, i.e. the parsed contents of `fonts/index.json`.
 *
 * Parsing is separate from loading the fonts so the text render only has to
 * decide which files to read, not how the index describes them.
 */
class CFontIndex
{
public:
	struct SLanguageVariant
	{
		std::string m_LanguageFile;
		std::string m_FamilyName;
	};

	/**
	 * Paths of the font files, in the order in which they are listed in the index.
	 * The order determines the order of the font faces and must be preserved.
	 */
	std::vector<std::string> m_vFontFilePaths;
	/**
	 * Paths of the font files that the client does not wait for, in the order
	 * in which they are listed in the index. They are read like the others but
	 * their faces only arrive once they are there, which for a client that
	 * fetches them over the network is a good deal later. Until then their
	 * glyphs are missing, so a font that the first screen needs does not belong
	 * in here.
	 */
	std::vector<std::string> m_vDeferredFontFilePaths;
	std::string m_DefaultFamilyName;
	std::string m_IconFamilyName;
	std::vector<std::string> m_vFallbackFamilyNames;
	std::vector<SLanguageVariant> m_vLanguageVariants;

	void Reset();

	/**
	 * Parses a font index. Malformed entries are skipped and reported, so a
	 * partially broken index still yields the fonts that could be understood.
	 *
	 * @param pJson The contents of the font index file.
	 * @param Length The length of the contents in bytes.
	 * @param pContextName The name of the index file, for error messages.
	 *
	 * @return `true` if the index was fully understood, `false` otherwise.
	 */
	bool Parse(const char *pJson, unsigned Length, const char *pContextName);
};

/**
 * Tracks how far the asynchronous loading of the font files has progressed.
 *
 * The text render reads the font files in worker threads. The font faces of all
 * files are handed over to the main thread at once, when every file is finished,
 * so the main thread never uses a face while a worker still creates faces from
 * the same FreeType library.
 */
class CFontLoadProgress
{
public:
	enum class EState
	{
		/**
		 * No font file has been requested yet.
		 */
		IDLE,

		/**
		 * Font files are being read. No font face is usable yet.
		 */
		LOADING,

		/**
		 * All font files are finished and their faces have been committed.
		 */
		READY,
	};

	void Reset();

	/**
	 * Starts tracking a load of the given number of font files.
	 *
	 * @param FileCount The number of font files that were requested.
	 * @param IndexSuccess Whether the font index itself was fully understood.
	 */
	void BeginLoading(size_t FileCount, bool IndexSuccess);

	/**
	 * Reports the result of one font file.
	 */
	void ReportFile(bool Success);

	/**
	 * Marks the loaded font faces as usable. Only allowed once every file was reported.
	 *
	 * @param FacesSuccess Whether every font face named by the index was found.
	 */
	void Commit(bool FacesSuccess);

	EState State() const { return m_State; }
	bool Loading() const { return m_State == EState::LOADING; }
	bool Ready() const { return m_State == EState::READY; }
	bool AllFilesFinished() const { return m_FinishedFileCount == m_FileCount; }

	/**
	 * @return `true` if the index, every font file and every named face were
	 * loaded without errors.
	 */
	bool Success() const { return m_IndexSuccess && m_FacesSuccess && m_FailedFileCount == 0; }

	size_t FileCount() const { return m_FileCount; }
	size_t FinishedFileCount() const { return m_FinishedFileCount; }
	size_t FailedFileCount() const { return m_FailedFileCount; }

private:
	EState m_State = EState::IDLE;
	bool m_IndexSuccess = true;
	bool m_FacesSuccess = true;
	size_t m_FileCount = 0;
	size_t m_FinishedFileCount = 0;
	size_t m_FailedFileCount = 0;
};

#endif // ENGINE_CLIENT_FONT_LOADING_H
