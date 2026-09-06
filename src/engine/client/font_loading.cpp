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
	const json_value &FontFiles = (*pJsonData)["font files"];
	if(FontFiles.type == json_array)
	{
		for(unsigned FontFileIndex = 0; FontFileIndex < FontFiles.u.array.length; ++FontFileIndex)
		{
			if(FontFiles[FontFileIndex].type != json_string)
			{
				log_error("textrender", "Font index malformed: 'font files' must be an array of strings (error at index %d)", FontFileIndex);
				Success = false;
				continue;
			}

			char aFontPath[IO_MAX_PATH_LENGTH];
			str_format(aFontPath, sizeof(aFontPath), "fonts/%s", FontFiles[FontFileIndex].u.string.ptr);
			m_vFontFilePaths.emplace_back(aFontPath);
		}
	}
	else
	{
		log_error("textrender", "Font index malformed: 'font files' must be an array");
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
	m_IndexSuccess = true;
	m_FacesSuccess = true;
	m_FileCount = 0;
	m_FinishedFileCount = 0;
	m_FailedFileCount = 0;
}

void CFontLoadProgress::BeginLoading(size_t FileCount, bool IndexSuccess)
{
	dbg_assert(m_State == EState::IDLE, "Font loading was already started");
	m_State = EState::LOADING;
	m_IndexSuccess = IndexSuccess;
	m_FacesSuccess = true;
	m_FileCount = FileCount;
	m_FinishedFileCount = 0;
	m_FailedFileCount = 0;
}

void CFontLoadProgress::ReportFile(bool Success)
{
	dbg_assert(m_State == EState::LOADING, "Font file reported while no font files were being loaded");
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
