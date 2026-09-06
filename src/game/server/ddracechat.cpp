/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#include "gamecontext.h"
#include "gamemodes/ddrace.h"
#include "player.h"
#include "score.h"

#include <base/log.h>

#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include <game/server/entities/character.h>
#include <game/version.h>

#include <algorithm>

void CGameControllerDDRace::ConInfo(IConsole::IResult *pResult, void *pUserData)
{
	log_info("chatresp", "DDraceNetwork Mod. Version: " GAME_VERSION);
	if(GIT_SHORTREV_HASH)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Git revision hash: %s", GIT_SHORTREV_HASH);
		log_info("chatresp", "%s", aBuf);
	}
	log_info("chatresp", "Official site: DDNet.org");
	log_info("chatresp", "For more info: /cmdlist");
	log_info("chatresp", "Or visit DDNet.org");
}

void CGameContext::ConList(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int ClientId = pResult->m_ClientId;
	if(!CheckClientId(ClientId))
		return;

	if(pResult->NumArguments() > 0)
		pSelf->List(ClientId, pResult->GetString(0));
	else
		pSelf->List(ClientId, "");
}

void CGameContext::ConHelp(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	if(pResult->NumArguments() == 0)
	{
		log_info("chatresp", "/cmdlist will show a list of all chat commands");
		log_info("chatresp", "/help + any command will show you the help for this command");
		log_info("chatresp", "Example /help settings will display the help about /settings");
	}
	else
	{
		const char *pArg = pResult->GetString(0);
		const IConsole::ICommandInfo *pCmdInfo =
			pSelf->Console()->GetCommandInfo(pArg, CFGFLAG_SERVER | CFGFLAG_CHAT, false);
		if(pCmdInfo)
		{
			if(pCmdInfo->Params())
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "Usage: %s %s", pCmdInfo->Name(), pCmdInfo->Params());
				log_info("chatresp", "%s", aBuf);
			}

			if(pCmdInfo->Help())
				log_info("chatresp", "%s", pCmdInfo->Help());
		}
		else
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Unknown command %s", pArg);
			log_info("chatresp", "%s", aBuf);
		}
	}
}

void CGameContext::ConRules(IConsole::IResult *pResult, void *pUserData)
{
	bool Printed = false;
	if(g_Config.m_SvDDRaceRules)
	{
		log_info("chatresp", "Be nice.");
		Printed = true;
	}
	char *apRuleLines[] = {
		g_Config.m_SvRulesLine1,
		g_Config.m_SvRulesLine2,
		g_Config.m_SvRulesLine3,
		g_Config.m_SvRulesLine4,
		g_Config.m_SvRulesLine5,
		g_Config.m_SvRulesLine6,
		g_Config.m_SvRulesLine7,
		g_Config.m_SvRulesLine8,
		g_Config.m_SvRulesLine9,
		g_Config.m_SvRulesLine10,
	};
	for(auto &pRuleLine : apRuleLines)
	{
		if(pRuleLine[0])
		{
			log_info("chatresp", "%s", pRuleLine);
			Printed = true;
		}
	}
	if(!Printed)
	{
		log_info("chatresp", "No Rules Defined, Kill em all!!");
	}
}

void CGameContext::ConDND(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	pPlayer->m_DND = pResult->NumArguments() == 0 ? !pPlayer->m_DND : pResult->GetInteger(0);
	log_info("chatresp", pPlayer->m_DND ? "You will not receive any further global chat and server messages" : "You will receive global chat and server messages");
}

void CGameContext::ConWhispers(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	pPlayer->m_Whispers = pResult->NumArguments() == 0 ? !pPlayer->m_Whispers : pResult->GetInteger(0);
	log_info("chatresp", pPlayer->m_Whispers ? "You will receive whispers" : "You will not receive any further whispers");
}

void CGameControllerDDRace::ConMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pController = static_cast<CGameControllerDDRace *>(pSelf->GameHost().Controller());
	if(!CheckClientId(pResult->m_ClientId))
		return;

	if(g_Config.m_SvMapVote == 0)
	{
		log_info("chatresp", "/map is disabled");
		return;
	}

	if(pResult->NumArguments() <= 0)
	{
		log_info("chatresp", "Example: /map adr3 to call vote for Adrenaline 3. This means that the map name must start with 'a' and contain the characters 'd', 'r' and '3' in that order");
		return;
	}

	CPlayer *pPlayer = pController->Services().Player(pResult->m_ClientId);
	if(!pPlayer)
		return;

	if(pSelf->RateLimitPlayerVote(pResult->m_ClientId) || pSelf->RateLimitPlayerMapVote(pResult->m_ClientId))
		return;

	pController->RaceScore().MapVote(pResult->m_ClientId, pResult->GetString(0));
}

void CGameControllerDDRace::ConMapInfo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pController = static_cast<CGameControllerDDRace *>(pSelf->GameHost().Controller());
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pController->Services().Player(pResult->m_ClientId);
	if(!pPlayer)
		return;

	// use cached map info for current map
	const bool IsCurrentMap = pResult->NumArguments() == 0 || str_comp_nocase(pResult->GetString(0), pSelf->Map()->BaseName()) == 0;
	if(IsCurrentMap && pController->RaceScore().MapInfoMessage()[0] != '\0')
	{
		pSelf->SendChatTarget(pResult->m_ClientId, pController->RaceScore().MapInfoMessage());
		return;
	}

	if(pResult->NumArguments() > 0)
		pController->RaceScore().MapInfo(pResult->m_ClientId, pResult->GetString(0));
	else
		pController->RaceScore().MapInfo(pResult->m_ClientId, pSelf->Map()->BaseName());
}

void CGameContext::ConTimeout(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	const char *pTimeout = pResult->NumArguments() > 0 ? pResult->GetString(0) : pPlayer->m_aTimeoutCode;

	for(int i = 0; i < pSelf->Server()->MaxClients(); i++)
	{
		if(i == pResult->m_ClientId)
			continue;
		if(!pSelf->m_apPlayers[i])
			continue;
		if(str_comp(pSelf->m_apPlayers[i]->m_aTimeoutCode, pTimeout))
			continue;
		if(pSelf->Server()->SetTimedOut(i, pResult->m_ClientId))
		{
			if(pSelf->m_apPlayers[i]->GetCharacter())
				pSelf->SendTuningParams(i, pSelf->m_apPlayers[i]->GetCharacter()->TuningZone());
			return;
		}
	}

	pSelf->Server()->SetTimeoutProtected(pResult->m_ClientId);
	str_copy(pPlayer->m_aTimeoutCode, pResult->GetString(0));
}

void CGameContext::ConConverse(IConsole::IResult *pResult, void *pUserData)
{
	// This will never be called
}

void CGameContext::ConWhisper(IConsole::IResult *pResult, void *pUserData)
{
	// This will never be called
}

void CGameContext::ConSetEyeEmote(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	if(pResult->NumArguments() == 0)
	{
		log_info("chatresp", pPlayer->m_EyeEmoteEnabled ?
					     "You can now use the preset eye emotes." :
					     "You don't have any eye emotes, remember to bind some.");
		return;
	}
	else if(str_comp_nocase(pResult->GetString(0), "on") == 0)
		pPlayer->m_EyeEmoteEnabled = true;
	else if(str_comp_nocase(pResult->GetString(0), "off") == 0)
		pPlayer->m_EyeEmoteEnabled = false;
	else if(str_comp_nocase(pResult->GetString(0), "toggle") == 0)
		pPlayer->m_EyeEmoteEnabled = !pPlayer->m_EyeEmoteEnabled;
	log_info("chatresp", pPlayer->m_EyeEmoteEnabled ?
				     "You can now use the preset eye emotes." :
				     "You don't have any eye emotes, remember to bind some.");
}

void CGameContext::ConEyeEmote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(g_Config.m_SvEmotionalTees == -1)
	{
		log_info("chatresp", "Emotes are disabled.");
		return;
	}

	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	if(pResult->NumArguments() == 0)
	{
		log_info("chatresp", "Emote commands are: /emote surprise /emote blink /emote close /emote angry /emote happy /emote pain /emote normal");
		log_info("chatresp", "Example: /emote surprise 10 for 10 seconds or /emote surprise (default 1 second)");
	}
	else
	{
		if(!pPlayer->CanOverrideDefaultEmote())
			return;

		int EmoteType = 0;
		if(!str_comp_nocase(pResult->GetString(0), "angry"))
			EmoteType = EMOTE_ANGRY;
		else if(!str_comp_nocase(pResult->GetString(0), "blink"))
			EmoteType = EMOTE_BLINK;
		else if(!str_comp_nocase(pResult->GetString(0), "close"))
			EmoteType = EMOTE_BLINK;
		else if(!str_comp_nocase(pResult->GetString(0), "happy"))
			EmoteType = EMOTE_HAPPY;
		else if(!str_comp_nocase(pResult->GetString(0), "pain"))
			EmoteType = EMOTE_PAIN;
		else if(!str_comp_nocase(pResult->GetString(0), "surprise"))
			EmoteType = EMOTE_SURPRISE;
		else if(!str_comp_nocase(pResult->GetString(0), "normal"))
			EmoteType = EMOTE_NORMAL;
		else
		{
			log_info("chatresp", "Unknown emote... Say /emote");
			return;
		}

		int Duration = 1;
		if(pResult->NumArguments() > 1)
			Duration = std::clamp(pResult->GetInteger(1), 1, 86400);

		pPlayer->OverrideDefaultEmote(EmoteType, pSelf->Server()->Tick() + Duration * pSelf->Server()->TickSpeed());
	}
}

void CGameContext::ConShowAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	if(pResult->NumArguments())
	{
		if(pPlayer->m_ShowAll == (bool)pResult->GetInteger(0))
			return;

		pPlayer->m_ShowAll = pResult->GetInteger(0);
	}
	else
	{
		pPlayer->m_ShowAll = !pPlayer->m_ShowAll;
	}

	if(pPlayer->m_ShowAll)
		pSelf->SendChatTarget(pResult->m_ClientId, "You will now see all tees on this server, no matter the distance");
	else
		pSelf->SendChatTarget(pResult->m_ClientId, "You will no longer see all tees on this server");
}
