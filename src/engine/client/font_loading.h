/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_FONT_LOADING_H
#define ENGINE_CLIENT_FONT_LOADING_H

#include <string>
#include <vector>

/**
 * The parsed contents of `fonts/index.json`.
 */
class CFontIndex
{
public:
	struct SLanguageVariant
	{
		std::string m_LanguageFile;
		std::string m_FamilyName;
	};

	struct SDeferredFontFile
	{
		std::string m_Path;
		// Loaded when one of the families is used, right away if empty
		std::vector<std::string> m_vFamilyNames;
	};

	/**
	 * Paths of the font files, in the order in which they are listed in the index.
	 * The order determines the order of the font faces and must be preserved.
	 */
	std::vector<std::string> m_vFontFilePaths;
	// Loaded in the background, the client does not wait for them
	std::vector<SDeferredFontFile> m_vDeferredFontFiles;
	std::string m_DefaultFamilyName;
	std::string m_IconFamilyName;
	std::vector<std::string> m_vFallbackFamilyNames;
	std::vector<SLanguageVariant> m_vLanguageVariants;

	void Reset();

	/**
	 * Malformed entries are skipped and logged.
	 *
	 * @return `true` if the index was fully valid.
	 */
	bool Parse(const char *pJson, unsigned Length, const char *pContextName);
};

#endif // ENGINE_CLIENT_FONT_LOADING_H
