#include "settings.h"

#include <base/str.h>

#include <engine/shared/config.h>

#include <algorithm>
#include <cstdlib>

namespace map_document
{
	namespace
	{
		/**
		 * Reads the argument list of a command the way the console writes
		 * one: `i[zone] s[tuning] f[value]`, with `?` before a letter for an
		 * argument that may be left out.
		 */
		std::vector<CSettingArg> ReadArgs(const char *pArgs)
		{
			std::vector<CSettingArg> vArgs;
			for(const char *pAt = pArgs; *pAt != '\0'; ++pAt)
			{
				bool Optional = false;
				if(*pAt == '?')
				{
					Optional = true;
					++pAt;
					if(*pAt == '\0')
						break;
				}
				if(*pAt == ' ')
					continue;
				CSettingArg Arg;
				Arg.m_Type = *pAt;
				Arg.m_Optional = Optional;
				++pAt;
				if(*pAt == '[')
				{
					const char *pName = pAt + 1;
					while(*pAt != '\0' && *pAt != ']')
						++pAt;
					Arg.m_Name.assign(pName, pAt - pName);
					if(*pAt == '\0')
						--pAt;
				}
				else
				{
					--pAt;
				}
				vArgs.push_back(std::move(Arg));
			}
			return vArgs;
		}

		CMapSetting Variable(const char *pName, const char *pHelp, int Default, int Min, int Max)
		{
			CMapSetting Setting;
			Setting.m_Name = pName;
			Setting.m_Help = pHelp;
			Setting.m_IsVariable = true;
			Setting.m_Default = Default;
			Setting.m_Min = Min;
			Setting.m_Max = Max;
			Setting.m_Args.push_back(CSettingArg{"value", 'i', false});
			// A variable has one value, so saying it twice is saying it twice.
			Setting.m_KeyArgs = 0;
			return Setting;
		}

		CMapSetting Command(const char *pName, const char *pArgs, const char *pHelp, size_t KeyArgs)
		{
			CMapSetting Setting;
			Setting.m_Name = pName;
			Setting.m_Help = pHelp;
			Setting.m_IsVariable = false;
			Setting.m_Default = 0;
			Setting.m_Min = 0;
			Setting.m_Max = 0;
			Setting.m_Args = ReadArgs(pArgs);
			Setting.m_KeyArgs = KeyArgs;
			return Setting;
		}

		std::vector<CMapSetting> BuildSettings()
		{
			std::vector<CMapSetting> vSettings;

			// Every variable a map is allowed to set - the same list the
			// server reads, taken from the same file rather than written out
			// again here, so that one of them cannot fall behind the other.
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) \
	if((Flags) & CFGFLAG_GAME) \
		vSettings.push_back(Variable(#ScriptName, Desc, Def, Min, Max));
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc)
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc)
#include <engine/shared/config_variables.h>
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR

			// And the six commands, which are not variables and so are not in
			// that file. The number at the end is how many arguments tell two
			// of them apart: `tune_zone 1 ...` and `tune_zone 2 ...` are two
			// different things, `sv_deepfly 0` twice is a mistake.
			vSettings.push_back(Command("tune", "s[tuning] f[value]",
				"Tune variable to value or show current value", 1));
			vSettings.push_back(Command("tune_zone", "i[zone] s[tuning] f[value]",
				"Tune in zone a variable to value", 2));
			vSettings.push_back(Command("tune_zone_enter", "i[zone] r[message]",
				"Which message to display on zone enter; use 0 for normal area", 1));
			vSettings.push_back(Command("tune_zone_leave", "i[zone] r[message]",
				"Which message to display on zone leave; use 0 for normal area", 1));
			vSettings.push_back(Command("mapbug", "s[mapbug]",
				"Enable map compatibility mode using the specified bug", 1));
			vSettings.push_back(Command("switch_open", "i[switch]",
				"Whether a switch is deactivated by default (otherwise activated)", 1));

			std::sort(vSettings.begin(), vSettings.end(),
				[](const CMapSetting &One, const CMapSetting &Other) { return One.m_Name < Other.m_Name; });
			return vSettings;
		}

		/** Whether a word reads as a whole number and nothing else. */
		bool IsWholeNumber(const std::string &Word, int *pValue)
		{
			if(Word.empty())
				return false;
			char *pEnd = nullptr;
			const long Value = std::strtol(Word.c_str(), &pEnd, 10);
			if(pEnd == nullptr || *pEnd != '\0')
				return false;
			*pValue = (int)Value;
			return true;
		}

		bool IsNumber(const std::string &Word)
		{
			if(Word.empty())
				return false;
			char *pEnd = nullptr;
			(void)std::strtod(Word.c_str(), &pEnd);
			return pEnd != nullptr && *pEnd == '\0';
		}
	} // namespace

	const std::vector<CMapSetting> &KnownSettings()
	{
		static const std::vector<CMapSetting> s_vSettings = BuildSettings();
		return s_vSettings;
	}

	const CMapSetting *FindSetting(const char *pName)
	{
		if(pName == nullptr)
			return nullptr;
		for(const CMapSetting &Setting : KnownSettings())
		{
			if(str_comp_nocase(pName, Setting.m_Name.c_str()) == 0)
				return &Setting;
		}
		return nullptr;
	}

	std::vector<std::string> SplitSetting(const char *pLine, int *pComment)
	{
		std::vector<std::string> vWords;
		if(pComment != nullptr)
			*pComment = -1;
		if(pLine == nullptr)
			return vWords;

		std::string Word;
		bool InWord = false;
		bool Quoted = false;
		for(const char *pAt = pLine; *pAt != '\0'; ++pAt)
		{
			if(Quoted && *pAt == '\\' && pAt[1] != '\0')
			{
				Word.push_back(pAt[1]);
				++pAt;
				continue;
			}
			if(*pAt == '"')
			{
				Quoted = !Quoted;
				InWord = true;
				continue;
			}
			if(!Quoted && *pAt == '#')
			{
				if(pComment != nullptr)
					*pComment = (int)(pAt - pLine);
				break;
			}
			if(!Quoted && *pAt == ' ')
			{
				if(InWord)
					vWords.push_back(std::move(Word));
				Word.clear();
				InWord = false;
				continue;
			}
			Word.push_back(*pAt);
			InWord = true;
		}
		if(InWord)
			vWords.push_back(std::move(Word));
		return vWords;
	}

	std::string CheckSetting(const char *pLine)
	{
		int Comment = -1;
		const std::vector<std::string> vWords = SplitSetting(pLine, &Comment);
		// A line that is only a comment says nothing, and nothing is fine.
		if(vWords.empty())
			return Comment == 0 ? std::string() : std::string("this line says nothing");

		const CMapSetting *pSetting = FindSetting(vWords[0].c_str());
		if(pSetting == nullptr)
			return "a server has no '" + vWords[0] + "'";

		const std::vector<CSettingArg> &vArgs = pSetting->m_Args;
		size_t Needed = 0;
		for(const CSettingArg &Arg : vArgs)
		{
			if(!Arg.m_Optional)
				++Needed;
		}
		const size_t Given = vWords.size() - 1;
		if(Given < Needed)
			return "'" + pSetting->m_Name + "' wants " + vArgs[Given].m_Name;
		// The rest of the line is one argument however many words it is
		// written in, so a command that ends in one is never given too much.
		const bool TakesRest = !vArgs.empty() && vArgs.back().m_Type == 'r';
		if(!TakesRest && Given > vArgs.size())
			return "'" + pSetting->m_Name + "' takes " + std::to_string(vArgs.size()) +
			       (vArgs.size() == 1 ? " argument, not " : " arguments, not ") + std::to_string(Given);

		for(size_t Index = 0; Index < vArgs.size() && Index < Given; ++Index)
		{
			const CSettingArg &Arg = vArgs[Index];
			const std::string &Word = vWords[Index + 1];
			if(Arg.m_Type == 'i')
			{
				int Value = 0;
				if(!IsWholeNumber(Word, &Value))
					return Arg.m_Name + " is a whole number, not '" + Word + "'";
				if(pSetting->m_IsVariable && (Value < pSetting->m_Min || Value > pSetting->m_Max))
					return Arg.m_Name + " is between " + std::to_string(pSetting->m_Min) +
					       " and " + std::to_string(pSetting->m_Max);
			}
			else if(Arg.m_Type == 'f' && !IsNumber(Word))
			{
				return Arg.m_Name + " is a number, not '" + Word + "'";
			}
		}
		return std::string();
	}

	int CollidingSetting(const std::vector<std::string> &vLines, const char *pLine)
	{
		const std::vector<std::string> vWords = SplitSetting(pLine);
		if(vWords.empty())
			return -1;
		const CMapSetting *pSetting = FindSetting(vWords[0].c_str());
		// Nothing collides with a line nobody understands: there is no telling
		// what it would be the same as.
		if(pSetting == nullptr)
			return -1;

		for(size_t Line = 0; Line < vLines.size(); ++Line)
		{
			const std::vector<std::string> vOther = SplitSetting(vLines[Line].c_str());
			if(vOther.empty() || str_comp_nocase(vOther[0].c_str(), vWords[0].c_str()) != 0)
				continue;
			bool Same = true;
			for(size_t Key = 1; Key <= pSetting->m_KeyArgs; ++Key)
			{
				const bool HasOne = Key < vWords.size();
				const bool HasOther = Key < vOther.size();
				if(HasOne != HasOther || (HasOne && str_comp_nocase(vWords[Key].c_str(), vOther[Key].c_str()) != 0))
				{
					Same = false;
					break;
				}
			}
			if(Same)
				return (int)Line;
		}
		return -1;
	}

	std::vector<std::string> CompleteSetting(const char *pPrefix)
	{
		std::vector<std::string> vNames;
		const size_t Length = pPrefix == nullptr ? 0 : str_length(pPrefix);
		for(const CMapSetting &Setting : KnownSettings())
		{
			if(Length == 0 || str_comp_nocase_num(Setting.m_Name.c_str(), pPrefix, Length) == 0)
				vNames.push_back(Setting.m_Name);
		}
		return vNames;
	}
} // namespace map_document
