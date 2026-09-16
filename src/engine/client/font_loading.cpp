/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "font_loading.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/str.h>
#include <base/types.h>

#include <engine/shared/json.h>

void CFontIndex::Reset()
{
	m_vFontFilePaths.clear();
	m_vDeferredFontFiles.clear();
	m_DefaultFamilyName.clear();
	m_IconFamilyName.clear();
	m_vFallbackFamilyNames.clear();
	m_vLanguageVariants.clear();
}

bool CFontIndex::Parse(const char *pJson, unsigned Length, const char *pContextName)
{
	Reset();

	json_settings JsonSettings{};
	char aError[256];
	json_value *pJsonData = JsonParseEx(&JsonSettings, pJson, Length, aError);
	if(pJsonData == nullptr)
	{
		log_error("textrender", "Failed to parse font index file '%s': %s", pContextName, aError);
		return false;
	}
	if(pJsonData->type != json_object)
	{
		log_error("textrender", "Font index malformed: root must be an object in file '%s'", pContextName);
		json_value_free(pJsonData);
		return false;
	}

	bool Success = true;

	// extract font file definitions
	const auto &&ExtractFontFiles = [&](const char *pKey, std::vector<std::string> &vPaths, bool Required) {
		const json_value &FontFiles = (*pJsonData)[pKey];
		if(FontFiles.type == json_none && !Required)
		{
			return;
		}
		if(FontFiles.type != json_array)
		{
			log_error("textrender", "Font index malformed: '%s' must be an array", pKey);
			Success = false;
			return;
		}
		for(unsigned FontFileIndex = 0; FontFileIndex < FontFiles.u.array.length; ++FontFileIndex)
		{
			if(FontFiles[FontFileIndex].type != json_string)
			{
				log_error("textrender", "Font index malformed: '%s' must be an array of strings (error at index %d)", pKey, FontFileIndex);
				Success = false;
				continue;
			}

			char aFontPath[IO_MAX_PATH_LENGTH];
			str_format(aFontPath, sizeof(aFontPath), "fonts/%s", FontFiles[FontFileIndex].u.string.ptr);
			vPaths.emplace_back(aFontPath);
		}
	};
	ExtractFontFiles("font files", m_vFontFilePaths, true);

	// The client starts without these, so an index that names none is fine.
	// Either a file name on its own, or an object that also says which font
	// families the file brings - and one that says so is only read when one of
	// them is wanted, which for twenty megabytes of glyphs that most sessions
	// never draw is the difference between fetching them and not.
	const json_value &DeferredFontFiles = (*pJsonData)["deferred font files"];
	if(DeferredFontFiles.type == json_array)
	{
		for(unsigned FontFileIndex = 0; FontFileIndex < DeferredFontFiles.u.array.length; ++FontFileIndex)
		{
			const json_value &Entry = DeferredFontFiles[FontFileIndex];
			const json_value &FileName = Entry.type == json_object ? Entry["file"] : Entry;
			if(FileName.type != json_string)
			{
				log_error("textrender", "Font index malformed: 'deferred font files' must be an array of strings or of objects with a 'file' (error at index %d)", FontFileIndex);
				Success = false;
				continue;
			}
			SDeferredFontFile DeferredFontFile;
			char aFontPath[IO_MAX_PATH_LENGTH];
			str_format(aFontPath, sizeof(aFontPath), "fonts/%s", FileName.u.string.ptr);
			DeferredFontFile.m_Path = aFontPath;
			if(Entry.type == json_object)
			{
				const json_value &Families = Entry["families"];
				if(Families.type == json_array)
				{
					for(unsigned FamilyIndex = 0; FamilyIndex < Families.u.array.length; ++FamilyIndex)
					{
						if(Families[FamilyIndex].type != json_string)
						{
							log_error("textrender", "Font index malformed: 'families' must be an array of strings (error at index %d of deferred font file %d)", FamilyIndex, FontFileIndex);
							Success = false;
							continue;
						}
						DeferredFontFile.m_vFamilyNames.emplace_back(Families[FamilyIndex].u.string.ptr);
					}
				}
				else if(Families.type != json_none)
				{
					log_error("textrender", "Font index malformed: 'families' must be an array (error at index %d of 'deferred font files')", FontFileIndex);
					Success = false;
				}
			}
			m_vDeferredFontFiles.push_back(std::move(DeferredFontFile));
		}
	}
	else if(DeferredFontFiles.type != json_none)
	{
		log_error("textrender", "Font index malformed: 'deferred font files' must be an array");
		Success = false;
	}

	// extract default family name
	const json_value &DefaultFace = (*pJsonData)["default"];
	if(DefaultFace.type == json_string)
	{
		m_DefaultFamilyName = DefaultFace.u.string.ptr;
	}
	else
	{
		log_error("textrender", "Font index malformed: 'default' must be a string");
		Success = false;
	}

	// extract language variant family names
	const json_value &Variants = (*pJsonData)["language variants"];
	if(Variants.type == json_object)
	{
		m_vLanguageVariants.reserve(Variants.u.object.length);
		for(size_t i = 0; i < Variants.u.object.length; ++i)
		{
			const json_value *pFamilyName = Variants.u.object.values[i].value;
			if(pFamilyName->type != json_string)
			{
				log_error("textrender", "Font index malformed: 'language variants' entries must have string values (error on entry '%s')", Variants.u.object.values[i].name);
				Success = false;
				continue;
			}

			char aLanguageFile[IO_MAX_PATH_LENGTH];
			str_format(aLanguageFile, sizeof(aLanguageFile), "languages/%s.txt", Variants.u.object.values[i].name);
			m_vLanguageVariants.emplace_back(SLanguageVariant{aLanguageFile, pFamilyName->u.string.ptr});
		}
	}
	else
	{
		log_error("textrender", "Font index malformed: 'language variants' must be an array");
		Success = false;
	}

	// extract fallback family names
	const json_value &FallbackFaces = (*pJsonData)["fallbacks"];
	if(FallbackFaces.type == json_array)
	{
		for(unsigned i = 0; i < FallbackFaces.u.array.length; ++i)
		{
			if(FallbackFaces[i].type != json_string)
			{
				log_error("textrender", "Font index malformed: 'fallbacks' must be an array of strings (error at index %d)", i);
				Success = false;
				continue;
			}
			m_vFallbackFamilyNames.emplace_back(FallbackFaces[i].u.string.ptr);
		}
	}
	else
	{
		log_error("textrender", "Font index malformed: 'fallbacks' must be an array");
		Success = false;
	}

	// extract icon font family name
	const json_value &IconFace = (*pJsonData)["icon"];
	if(IconFace.type == json_string)
	{
		m_IconFamilyName = IconFace.u.string.ptr;
	}
	else
	{
		log_error("textrender", "Font index malformed: 'icon' must be a string");
		Success = false;
	}

	json_value_free(pJsonData);
	return Success;
}

void CFontLoadProgress::Reset()
{
	m_State = EState::IDLE;
	m_IndexLoaded = false;
	m_IndexSuccess = true;
	m_FacesSuccess = true;
	m_FileCount = 0;
	m_FinishedFileCount = 0;
	m_FailedFileCount = 0;
}

void CFontLoadProgress::BeginLoadingIndex()
{
	dbg_assert(m_State == EState::IDLE, "Font loading was already started");
	m_State = EState::LOADING;
	m_IndexLoaded = false;
	m_IndexSuccess = true;
	m_FacesSuccess = true;
	m_FileCount = 0;
	m_FinishedFileCount = 0;
	m_FailedFileCount = 0;
}

void CFontLoadProgress::IndexLoaded(size_t FileCount, bool IndexSuccess)
{
	dbg_assert(WaitingForIndex(), "Font index reported while none was being read");
	m_IndexLoaded = true;
	m_IndexSuccess = IndexSuccess;
	m_FileCount = FileCount;
}

void CFontLoadProgress::ReportFile(bool Success)
{
	dbg_assert(m_State == EState::LOADING && m_IndexLoaded, "Font file reported while no font files were being loaded");
	dbg_assert(m_FinishedFileCount < m_FileCount, "More font files reported than were being loaded");
	++m_FinishedFileCount;
	if(!Success)
		++m_FailedFileCount;
}

void CFontLoadProgress::Commit(bool FacesSuccess)
{
	dbg_assert(m_State == EState::LOADING, "Font faces committed while no font files were being loaded");
	dbg_assert(AllFilesFinished(), "Font faces committed before all font files were finished");
	m_FacesSuccess = FacesSuccess;
	m_State = EState::READY;
}
