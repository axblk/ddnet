#include "session_game_config.h"

#include <utility>

CSessionGameConfig::CSessionGameConfig(const CConfig &BaseValues) :
	m_pConsole(CreateConsole(CFGFLAG_GAME))
{
	const auto AddInt = [this](const char *pName, int *pValue, int Default, int Min, int Max, const char *pDescription) {
		auto pVariable = std::make_unique<SIntConfigVariable>(m_pConsole.get(), pName, SConfigVariable::VAR_INT, CFGFLAG_GAME, pDescription, pValue, Default, Min, Max);
		pVariable->Register();
		m_vpVariables.push_back(std::move(pVariable));
	};
	// A map may set exactly the settings that carry CFGFLAG_GAME, so the list is
	// taken from where it is already written down. Naming them here again is how
	// a setting gets forgotten, and a forgotten one is predicted wrong.
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) \
	if(((Flags) & CFGFLAG_GAME) != 0) \
	{ \
		AddInt(#ScriptName, &m_Values.m_##Name, Def, Min, Max, Desc); \
	}
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc)
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc)
#include <engine/shared/config_variables.h>
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR

	m_pConsole->SetUnknownCommandCallback([](const char *, void *) { return true; }, nullptr);
	Reset(BaseValues);
}

void CSessionGameConfig::ExecuteLine(const char *pLine)
{
	m_pConsole->ExecuteLine(pLine, IConsole::CLIENT_ID_GAME);
}
