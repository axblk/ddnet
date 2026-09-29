#include "config_schema.h"

#include <base/str.h>

#include <engine/antibot.h>
#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/server.h>
#include <engine/server/antibot.h>
#include <engine/server/server.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/quic_transport.h>
#include <engine/storage.h>

#include <game/version.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// Found by reading where the server reads each variable. A variable that is
// neither listed here nor has `CFGFLAG_GAME` counts as live, so a new variable
// that is only read at startup or at map load belongs in one of the lists.
static const char *const gs_apMapLoadVariables[] = {
	// tidy-alphabetical-start
	"sv_auto_demo_max", // at map load and round start
	"sv_auto_demo_record", // at map load and round start
	"sv_ddrace_tune_reset",
	"sv_gametype",
	"sv_map", // its chain starts the map change
	"sv_maps_base_url",
	"sv_powerups", // at map load and match start
	"sv_reset_file",
	"sv_shotgun_bullet_sound",
	"sv_tee_historian",
	"sv_tune_reset",
	"sv_warmup",
	// tidy-alphabetical-end
};

static const char *const gs_apRestartVariables[] = {
	// tidy-alphabetical-start
	"bindaddr",
	"dbg_sql",
	"ec_bindaddr",
	"ec_port",
	"logappend",
	"logfile",
	"sv_ipv4only", // its chain only changes the registration
	"sv_legacy_udp",
	"sv_max_clients",
	"sv_port",
	"sv_port_file",
	"sv_quic",
	"sv_quic_identity_key",
	"sv_register_hostname",
	"sv_register_port",
	"sv_rescue",
	"sv_sql_ssl_ca", // read by add_sqlserver in the startup config
	"sv_sql_ssl_cert",
	"sv_sql_ssl_key",
	"sv_sqlite_file",
	"sv_test_cmds",
	"sv_tls_cert", // read again by reload_tls_cert
	"sv_tls_cert_next",
	"sv_tls_key",
	"sv_use_sql",
	"sv_webtransport",
// tidy-alphabetical-end
#if defined(CONF_UPNP)
	"sv_use_upnp",
#endif
};

static bool Listed(const char *const *ppNames, size_t NumNames, const char *pName)
{
	return std::any_of(ppNames, ppNames + NumNames, [pName](const char *pListed) { return str_comp(pListed, pName) == 0; });
}

// String variables that take one of a few words, see `ParseServerMapConvert`.
static const char *const gs_apMapConvertValues[] = {"hybrid", "remap", "embed", "off"};

std::vector<const char *> ConfigEnumValues(const char *pName)
{
	if(str_comp(pName, "sv_map_convert") == 0)
		return {std::begin(gs_apMapConvertValues), std::end(gs_apMapConvertValues)};
	return {};
}

EConfigApply ConfigApply(const char *pName, int Flags)
{
	if(Flags & CFGFLAG_GAME || Listed(gs_apMapLoadVariables, std::size(gs_apMapLoadVariables), pName))
		return EConfigApply::MAP_LOAD;
	if(Listed(gs_apRestartVariables, std::size(gs_apRestartVariables), pName))
		return EConfigApply::RESTART;
	return EConfigApply::LIVE;
}

std::vector<const char *> ConfigApplyListedNames()
{
	std::vector<const char *> vpNames(std::begin(gs_apMapLoadVariables), std::end(gs_apMapLoadVariables));
	vpNames.insert(vpNames.end(), std::begin(gs_apRestartVariables), std::end(gs_apRestartVariables));
	return vpNames;
}

static void WriteFlags(CJsonWriter *pJson, int Flags)
{
	static const struct
	{
		int m_Flag;
		const char *m_pName;
	} s_aFlags[] = {
		{CFGFLAG_SAVE, "save"},
		{CFGFLAG_CLIENT, "client"},
		{CFGFLAG_SERVER, "server"},
		{CFGFLAG_STORE, "store"},
		{CFGFLAG_ECON, "econ"},
		{CMDFLAG_TEST, "test"},
		{CFGFLAG_CHAT, "chat"},
		{CFGFLAG_GAME, "game"},
		{CFGFLAG_NONTEEHISTORIC, "nonteehistoric"},
		{CFGFLAG_COLLIGHT, "collight"},
		{CFGFLAG_COLLIGHT7, "collight7"},
		{CFGFLAG_COLALPHA, "colalpha"},
		{CFGFLAG_INSENSITIVE, "insensitive"},
		{CMDFLAG_PRACTICE, "practice"},
	};
	pJson->WriteAttribute("flags");
	pJson->BeginArray();
	for(const auto &Flag : s_aFlags)
	{
		if(Flags & Flag.m_Flag)
			pJson->WriteStrValue(Flag.m_pName);
	}
	pJson->EndArray();
}

static const char *AccessLevelName(IConsole::EAccessLevel AccessLevel)
{
	switch(AccessLevel)
	{
	case IConsole::EAccessLevel::ADMIN: return "admin";
	case IConsole::EAccessLevel::MODERATOR: return "moderator";
	case IConsole::EAccessLevel::HELPER: return "helper";
	case IConsole::EAccessLevel::USER: return "user";
	}
	return "admin";
}

static const char *ApplyName(EConfigApply Apply)
{
	switch(Apply)
	{
	case EConfigApply::LIVE: return "live";
	case EConfigApply::MAP_LOAD: return "map_load";
	case EConfigApply::RESTART: return "restart";
	}
	return "live";
}

static void WriteVariable(CJsonWriter *pJson, IConsole *pConsole, const SConfigVariable *pVariable)
{
	pJson->BeginObject();
	pJson->WriteAttribute("name");
	pJson->WriteStrValue(pVariable->m_pScriptName);
	pJson->WriteAttribute("type");
	switch(pVariable->m_Type)
	{
	case SConfigVariable::VAR_INT:
	{
		const auto *pInt = static_cast<const SIntConfigVariable *>(pVariable);
		pJson->WriteStrValue("int");
		pJson->WriteAttribute("default");
		pJson->WriteIntValue(pInt->m_Default);
		// 0 as both bounds, or as the maximum, means there is no bound.
		const bool Bounded = pInt->m_Min != 0 || pInt->m_Max != 0;
		pJson->WriteAttribute("min");
		if(Bounded)
			pJson->WriteIntValue(pInt->m_Min);
		else
			pJson->WriteNullValue();
		pJson->WriteAttribute("max");
		if(pInt->m_Max != 0)
			pJson->WriteIntValue(pInt->m_Max);
		else
			pJson->WriteNullValue();
		break;
	}
	case SConfigVariable::VAR_COLOR:
		pJson->WriteStrValue("color");
		pJson->WriteAttribute("default");
		pJson->WriteInt64Value(static_cast<const SColorConfigVariable *>(pVariable)->m_Default);
		break;
	case SConfigVariable::VAR_STRING:
	{
		const auto *pString = static_cast<const SStringConfigVariable *>(pVariable);
		pJson->WriteStrValue("str");
		pJson->WriteAttribute("default");
		pJson->WriteStrValue(pString->m_pDefault);
		pJson->WriteAttribute("max_length");
		pJson->WriteIntValue(pString->m_MaxSize - 1);
		const std::vector<const char *> vpValues = ConfigEnumValues(pVariable->m_pScriptName);
		if(!vpValues.empty())
		{
			pJson->WriteAttribute("enum");
			pJson->BeginArray();
			for(const char *pValue : vpValues)
				pJson->WriteStrValue(pValue);
			pJson->EndArray();
		}
		break;
	}
	}
	WriteFlags(pJson, pVariable->m_Flags);
	const IConsole::ICommandInfo *pCommand = pConsole->GetCommandInfo(pVariable->m_pScriptName, pVariable->m_Flags, false);
	pJson->WriteAttribute("apply");
	pJson->WriteStrValue(ApplyName(ConfigApply(pVariable->m_pScriptName, pVariable->m_Flags)));
	pJson->WriteAttribute("chained");
	pJson->WriteBoolValue(pCommand != nullptr && pCommand->IsChained());
	pJson->WriteAttribute("help");
	// The console has the help a game may have set, such as the list of game types.
	pJson->WriteStrValue(pCommand != nullptr ? pCommand->Help() : pVariable->m_pHelp);
	pJson->EndObject();
}

namespace
{
	// A command as the schema lists it.
	struct CCommandEntry
	{
		std::string m_Name;
		std::string m_Params;
		int m_Flags;
		IConsole::EAccessLevel m_AccessLevel;
		std::string m_Help;
		// The game types that add the command, when it is not always there
		std::vector<std::string> m_vGameTypes;
		bool m_OfGameTypes;
	};
}

static void WriteConfigSchema(CJsonWriter *pJson, IConsole *pConsole, IConfigManager *pConfigManager, IGameServer *pGameServer)
{
	const int FlagMask = CFGFLAG_SERVER | CFGFLAG_ECON;

	pJson->BeginObject();
	pJson->WriteAttribute("v");
	pJson->WriteIntValue(1);
	pJson->WriteAttribute("version");
	pJson->WriteStrValue(GAME_RELEASE_VERSION);
	pJson->WriteAttribute("git");
	if(GIT_SHORTREV_HASH)
		pJson->WriteStrValue(GIT_SHORTREV_HASH);
	else
		pJson->WriteNullValue();

	pJson->WriteAttribute("build");
	pJson->BeginObject();
	pJson->WriteAttribute("websockets");
#if defined(CONF_WEBSOCKETS)
	pJson->WriteBoolValue(true);
#else
	pJson->WriteBoolValue(false);
#endif
	pJson->WriteAttribute("quic");
	pJson->WriteBoolValue(CQuicTransport::IsCompiled());
	pJson->WriteAttribute("webtransport");
	pJson->WriteBoolValue(CQuicTransport::IsCompiled());
	// An SQLite path is kept whole, not cut at 63 bytes as before.
	pJson->WriteAttribute("sqlite_long_path");
	pJson->WriteBoolValue(true);
	pJson->EndObject();

	std::vector<const SConfigVariable *> vpVariables;
	pConfigManager->PossibleConfigVariables(
		"", FlagMask, [](const SConfigVariable *pVariable, void *pUser) { static_cast<std::vector<const SConfigVariable *> *>(pUser)->push_back(pVariable); }, &vpVariables);
	std::sort(vpVariables.begin(), vpVariables.end(), [](const SConfigVariable *pA, const SConfigVariable *pB) { return str_comp(pA->m_pScriptName, pB->m_pScriptName) < 0; });
	pJson->WriteAttribute("variables");
	pJson->BeginArray();
	for(const SConfigVariable *pVariable : vpVariables)
		WriteVariable(pJson, pConsole, pVariable);
	pJson->EndArray();

	// What the console has, and what each game type adds while it runs,
	// listed with the game types that have it.
	std::vector<CCommandEntry> vCommands;
	const auto CollectCommands = [&](const char *pGameType) {
		for(const IConsole::ICommandInfo *pCommand = pConsole->FirstCommandInfo(IConsole::CLIENT_ID_UNSPECIFIED, FlagMask); pCommand; pCommand = pConsole->NextCommandInfo(pCommand, IConsole::CLIENT_ID_UNSPECIFIED, FlagMask))
		{
			const bool IsVariable = std::any_of(vpVariables.begin(), vpVariables.end(), [pCommand](const SConfigVariable *pVariable) { return str_comp(pVariable->m_pScriptName, pCommand->Name()) == 0; });
			if(IsVariable)
				continue;
			auto Found = std::find_if(vCommands.begin(), vCommands.end(), [pCommand](const CCommandEntry &Entry) { return Entry.m_Name == pCommand->Name(); });
			if(Found == vCommands.end())
			{
				vCommands.push_back({pCommand->Name(), pCommand->Params(), pCommand->Flags(), pCommand->GetAccessLevel(), pCommand->Help(), {}, pGameType != nullptr});
				Found = std::prev(vCommands.end());
			}
			if(pGameType != nullptr && Found->m_OfGameTypes)
				Found->m_vGameTypes.emplace_back(pGameType);
		}
	};
	CollectCommands(nullptr);
	const std::vector<IGameServer::CGameTypeName> vGameTypes = pGameServer->GameTypes();
	for(const IGameServer::CGameTypeName &GameType : vGameTypes)
		pGameServer->VisitGameTypeCommands(GameType.m_pName, [&]() { CollectCommands(GameType.m_pName); });
	std::sort(vCommands.begin(), vCommands.end(), [](const CCommandEntry &A, const CCommandEntry &B) { return str_comp(A.m_Name.c_str(), B.m_Name.c_str()) < 0; });

	pJson->WriteAttribute("commands");
	pJson->BeginArray();
	for(const CCommandEntry &Command : vCommands)
	{
		pJson->BeginObject();
		pJson->WriteAttribute("name");
		pJson->WriteStrValue(Command.m_Name.c_str());
		pJson->WriteAttribute("params");
		pJson->WriteStrValue(Command.m_Params.c_str());
		WriteFlags(pJson, Command.m_Flags);
		pJson->WriteAttribute("access");
		pJson->WriteStrValue(AccessLevelName(Command.m_AccessLevel));
		pJson->WriteAttribute("help");
		pJson->WriteStrValue(Command.m_Help.c_str());
		if(Command.m_OfGameTypes)
		{
			pJson->WriteAttribute("gametypes");
			pJson->BeginArray();
			for(const std::string &GameType : Command.m_vGameTypes)
				pJson->WriteStrValue(GameType.c_str());
			pJson->EndArray();
		}
		pJson->EndObject();
	}
	pJson->EndArray();

	const char *pDefaultGameType = DefaultConfig::SvGametype;
	pJson->WriteAttribute("gametypes");
	pJson->BeginArray();
	for(const IGameServer::CGameTypeName &GameType : vGameTypes)
	{
		pJson->BeginObject();
		pJson->WriteAttribute("name");
		pJson->WriteStrValue(GameType.m_pName);
		pJson->WriteAttribute("display");
		pJson->WriteStrValue(GameType.m_pDisplay);
		pJson->WriteAttribute("default");
		pJson->WriteBoolValue(str_comp_nocase(GameType.m_pName, pDefaultGameType) == 0);
		pJson->EndObject();
	}
	pJson->EndArray();
	pJson->EndObject();
}

std::string ServerConfigSchema()
{
	std::unique_ptr<IKernel> pKernel(IKernel::Create());
	CServer *pServer = CreateServer();
	pKernel->RegisterInterface(pServer);
	pKernel->RegisterInterface(CreateLocalStorage().release());
	IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON).release();
	pKernel->RegisterInterface(pConsole);
	IConfigManager *pConfigManager = CreateConfigManager();
	pKernel->RegisterInterface(pConfigManager);
	IEngineHttp *pEngineHttp = CreateEngineHttp();
	pKernel->RegisterInterface(pEngineHttp);
	pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);
	IEngineAntibot *pEngineAntibot = CreateEngineAntibot();
	pKernel->RegisterInterface(pEngineAntibot);
	pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);
	IGameServer *pGameServer = CreateGameServer();
	pKernel->RegisterInterface(pGameServer);
	// The server asks for the engine when it registers its commands; the quiet
	// one of the tests is enough, as nothing runs.
	pKernel->RegisterInterface(CreateTestEngine(GAME_NAME));

	pConsole->Init();
	pConfigManager->Init();
	pServer->RegisterCommands();

	CJsonStringWriter Json;
	WriteConfigSchema(&Json, pConsole, pConfigManager, pGameServer);
	return Json.GetOutputString();
}
