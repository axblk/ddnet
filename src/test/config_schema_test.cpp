#include <base/str.h>

#include <engine/server/config_schema.h>
#include <engine/server/map_conversion.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{
	struct SExpectedVariable
	{
		const char *m_pName;
		const char *m_pType;
		int m_Flags;
	};

	const SExpectedVariable gs_aExpectedVariables[] = {
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) {#ScriptName, "int", Flags},
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc) {#ScriptName, "color", Flags},
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc) {#ScriptName, "str", Flags},
#include <engine/shared/config_variables.h>
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR
	};

	// Builds the schema like `--config-schema` does, which resets the config.
	std::string Schema()
	{
		const CConfig Backup = g_Config;
		std::string Result = ServerConfigSchema();
		g_Config = Backup;
		return Result;
	}

	const json_value *FindByName(const json_value *pArray, const char *pName)
	{
		for(int i = 0; i < json_array_length(pArray); i++)
		{
			const json_value *pEntry = json_array_get(pArray, i);
			if(str_comp(json_string_get(json_object_get(pEntry, "name")), pName) == 0)
				return pEntry;
		}
		return nullptr;
	}

	bool HasFlag(const json_value *pEntry, const char *pFlag)
	{
		const json_value *pFlags = json_object_get(pEntry, "flags");
		for(int i = 0; i < json_array_length(pFlags); i++)
		{
			if(str_comp(json_string_get(json_array_get(pFlags, i)), pFlag) == 0)
				return true;
		}
		return false;
	}
}

TEST(ConfigSchema, CoversEveryVariable)
{
	const std::string Output = Schema();
	json_value *pSchema = JsonParse(Output.c_str(), Output.size());
	ASSERT_NE(pSchema, nullptr);
	EXPECT_EQ(json_int_get(json_object_get(pSchema, "v")), 1);
	EXPECT_EQ(json_object_get(pSchema, "build")->type, json_object);
	EXPECT_TRUE(json_boolean_get(json_object_get(json_object_get(pSchema, "build"), "sqlite_long_path")));

	const json_value *pVariables = json_object_get(pSchema, "variables");
	ASSERT_EQ(pVariables->type, json_array);
	std::map<std::string, const json_value *> Variables;
	for(int i = 0; i < json_array_length(pVariables); i++)
	{
		const json_value *pVariable = json_array_get(pVariables, i);
		const char *pName = json_string_get(json_object_get(pVariable, "name"));
		ASSERT_NE(pName, nullptr);
		EXPECT_TRUE(Variables.emplace(pName, pVariable).second) << pName << " is listed twice";
		if(i > 0)
		{
			EXPECT_LT(str_comp(json_string_get(json_object_get(json_array_get(pVariables, i - 1), "name")), pName), 0) << "not sorted at " << pName;
		}
		const char *pApply = json_string_get(json_object_get(pVariable, "apply"));
		ASSERT_NE(pApply, nullptr) << pName;
		EXPECT_TRUE(str_comp(pApply, "live") == 0 || str_comp(pApply, "map_load") == 0 || str_comp(pApply, "restart") == 0) << pName;
		EXPECT_EQ(json_object_get(pVariable, "chained")->type, json_boolean) << pName;
		EXPECT_NE(json_string_get(json_object_get(pVariable, "help")), nullptr) << pName;
	}

	size_t NumServerVariables = 0;
	for(const SExpectedVariable &Expected : gs_aExpectedVariables)
	{
		const auto It = Variables.find(Expected.m_pName);
		if((Expected.m_Flags & (CFGFLAG_SERVER | CFGFLAG_ECON)) == 0)
		{
			EXPECT_EQ(It, Variables.end()) << Expected.m_pName << " is no server variable";
			continue;
		}
		NumServerVariables++;
		ASSERT_NE(It, Variables.end()) << Expected.m_pName << " is missing";
		EXPECT_STREQ(json_string_get(json_object_get(It->second, "type")), Expected.m_pType) << Expected.m_pName;
		EXPECT_EQ(HasFlag(It->second, "game"), (Expected.m_Flags & CFGFLAG_GAME) != 0) << Expected.m_pName;
		if(Expected.m_Flags & CFGFLAG_GAME)
		{
			EXPECT_STREQ(json_string_get(json_object_get(It->second, "apply")), "map_load") << Expected.m_pName;
		}
	}
	EXPECT_EQ(Variables.size(), NumServerVariables);

	// The lists behind `apply` must name variables, or a rename loses the class.
	for(const char *pName : ConfigApplyListedNames())
		EXPECT_NE(Variables.find(pName), Variables.end()) << pName << " is listed for apply, but no server variable";

	const json_value *pPort = Variables.at("sv_port");
	EXPECT_STREQ(json_string_get(json_object_get(pPort, "apply")), "restart");
	EXPECT_EQ(json_int_get(json_object_get(pPort, "min")), 0);
	EXPECT_EQ(json_int_get(json_object_get(pPort, "max")), 65535);
	EXPECT_TRUE(HasFlag(pPort, "server"));
	const json_value *pName = Variables.at("sv_name");
	EXPECT_STREQ(json_string_get(json_object_get(pName, "apply")), "live");
	EXPECT_TRUE(json_boolean_get(json_object_get(pName, "chained")));
	EXPECT_EQ(json_int_get(json_object_get(pName, "max_length")), (int)sizeof(g_Config.m_SvName) - 1);
	EXPECT_STREQ(json_string_get(json_object_get(Variables.at("sv_gametype"), "apply")), "map_load");
	EXPECT_TRUE(HasFlag(Variables.at("sv_tls_key"), "nonteehistoric"));

	const json_value *pCommands = json_object_get(pSchema, "commands");
	const json_value *pStatus = FindByName(pCommands, "status");
	ASSERT_NE(pStatus, nullptr);
	EXPECT_STREQ(json_string_get(json_object_get(pStatus, "params")), "?r[name]");
	EXPECT_STREQ(json_string_get(json_object_get(pStatus, "access")), "admin");
	EXPECT_NE(FindByName(pCommands, "status_json"), nullptr);
	EXPECT_NE(FindByName(pCommands, "reload_tls_cert"), nullptr);
	EXPECT_EQ(FindByName(pCommands, "sv_port"), nullptr) << "variables are not listed as commands";
	EXPECT_EQ(json_object_get(pStatus, "gametypes")->type, json_none) << "status is there whatever the game type";
	for(int i = 0; i < json_array_length(pCommands); i++)
	{
		if(i > 0)
		{
			EXPECT_LT(str_comp(json_string_get(json_object_get(json_array_get(pCommands, i - 1), "name")), json_string_get(json_object_get(json_array_get(pCommands, i), "name"))), 0) << "commands not sorted at " << i;
		}
	}
	// The DDRace modes add their commands when a map starts with them.
	for(const char *pModeCommand : {"tele", "switch_open", "kill", "tune_zone"})
	{
		const json_value *pCommand = FindByName(pCommands, pModeCommand);
		ASSERT_NE(pCommand, nullptr) << pModeCommand;
		const json_value *pOf = json_object_get(pCommand, "gametypes");
		ASSERT_EQ(pOf->type, json_array) << pModeCommand;
		std::vector<std::string> vOf;
		vOf.reserve(json_array_length(pOf));
		for(int i = 0; i < json_array_length(pOf); i++)
			vOf.emplace_back(json_string_get(json_array_get(pOf, i)));
		EXPECT_EQ(vOf, (std::vector<std::string>{"ddnet", "mod"})) << pModeCommand;
	}

	const json_value *pGameTypes = json_object_get(pSchema, "gametypes");
	int NumDefault = 0;
	for(int i = 0; i < json_array_length(pGameTypes); i++)
		NumDefault += json_boolean_get(json_object_get(json_array_get(pGameTypes, i), "default"));
	EXPECT_EQ(NumDefault, 1);
	const json_value *pDDNet = FindByName(pGameTypes, "ddnet");
	ASSERT_NE(pDDNet, nullptr);
	EXPECT_STREQ(json_string_get(json_object_get(pDDNet, "display")), "DDraceNetwork");
	EXPECT_TRUE(json_boolean_get(json_object_get(pDDNet, "default")));

	json_value_free(pSchema);
}

TEST(ConfigSchema, Enum)
{
	// What the schema offers is what the server takes.
	const std::vector<const char *> vpValues = ConfigEnumValues("sv_map_convert");
	ASSERT_EQ(vpValues.size(), 4u);
	for(const char *pValue : vpValues)
	{
		std::optional<EMapConvertMode> Mode;
		EXPECT_TRUE(ParseServerMapConvert(pValue, Mode)) << pValue;
	}
	std::optional<EMapConvertMode> Mode;
	EXPECT_FALSE(ParseServerMapConvert("mark", Mode));
	EXPECT_TRUE(ConfigEnumValues("sv_name").empty());

	const std::string Output = Schema();
	json_value *pSchema = JsonParse(Output.c_str(), Output.size());
	ASSERT_NE(pSchema, nullptr);
	const json_value *pVariables = json_object_get(pSchema, "variables");
	const json_value *pEnum = json_object_get(FindByName(pVariables, "sv_map_convert"), "enum");
	ASSERT_EQ(pEnum->type, json_array);
	ASSERT_EQ(json_array_length(pEnum), (int)vpValues.size());
	for(int i = 0; i < json_array_length(pEnum); i++)
		EXPECT_STREQ(json_string_get(json_array_get(pEnum, i)), vpValues[i]);
	EXPECT_EQ(json_object_get(FindByName(pVariables, "sv_name"), "enum")->type, json_none);
	json_value_free(pSchema);
}

TEST(ConfigSchema, Deterministic)
{
	EXPECT_EQ(Schema(), Schema());
}

TEST(ConfigSchema, Apply)
{
	EXPECT_EQ(ConfigApply("sv_hit", CFGFLAG_SERVER | CFGFLAG_GAME), EConfigApply::MAP_LOAD);
	EXPECT_EQ(ConfigApply("sv_map", CFGFLAG_SERVER), EConfigApply::MAP_LOAD);
	EXPECT_EQ(ConfigApply("sv_port", CFGFLAG_SERVER), EConfigApply::RESTART);
	EXPECT_EQ(ConfigApply("sv_name", CFGFLAG_SERVER), EConfigApply::LIVE);
}
