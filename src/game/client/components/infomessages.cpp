/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "infomessages.h"

#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/game_view.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/localization.h>

#include <algorithm>

static constexpr float ROW_HEIGHT = 46.0f;
static constexpr float FONT_SIZE = 36.0f;
static constexpr float RACE_FLAG_SIZE = 52.0f;

namespace
{
	bool GetClientName(const CGameState &State, int ClientId, char *pName, int NameSize)
	{
		if(!in_range(ClientId, MAX_CLIENTS - 1) || !State.Client(ClientId).m_HasPlayerInfo)
			return false;
		const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
		if(!Identity.m_Active)
		{
			pName[0] = '\0';
			return true;
		}
		if(!IntsToStr(Identity.m_ClientInfo.m_aName, std::size(Identity.m_ClientInfo.m_aName), pName, NameSize))
			str_copy(pName, "nameless tee", NameSize);
		return true;
	}
}

void CInfoMessages::OnWindowResize()
{
	DeleteAllTextContainers();
}

void CInfoMessages::OnShutdown()
{
	DeleteAllTextContainers();
	m_vCachedInfoMsgs.clear();
}

void CInfoMessages::DeleteTextContainers(CCachedInfoMsg &Cached)
{
	TextRender()->DeleteTextContainer(Cached.m_VictimTextContainerIndex);
	TextRender()->DeleteTextContainer(Cached.m_KillerTextContainerIndex);
	TextRender()->DeleteTextContainer(Cached.m_DiffTextContainerIndex);
	TextRender()->DeleteTextContainer(Cached.m_TimeTextContainerIndex);
}

void CInfoMessages::DeleteAllTextContainers()
{
	for(CCachedInfoMsg &Cached : m_vCachedInfoMsgs)
		DeleteTextContainers(Cached);
}

void CInfoMessages::ResetPresentation(CSessionId SessionId)
{
	for(auto It = m_vCachedInfoMsgs.begin(); It != m_vCachedInfoMsgs.end();)
	{
		if(It->m_SessionId != SessionId)
		{
			++It;
			continue;
		}
		DeleteTextContainers(*It);
		It = m_vCachedInfoMsgs.erase(It);
	}
}

void CInfoMessages::OnInit()
{
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
	m_SpriteQuadContainerIndex = Graphics()->CreateQuadContainer(false);

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_SpriteQuadContainerIndex, 0.f, 0.f, 28.f, 56.f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_SpriteQuadContainerIndex, 0.f, 0.f, 28.f, 56.f);

	Graphics()->QuadsSetSubset(1, 0, 0, 1);
	Graphics()->QuadContainerAddSprite(m_SpriteQuadContainerIndex, 0.f, 0.f, 28.f, 56.f);
	Graphics()->QuadsSetSubset(1, 0, 0, 1);
	Graphics()->QuadContainerAddSprite(m_SpriteQuadContainerIndex, 0.f, 0.f, 28.f, 56.f);

	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		float ScaleX, ScaleY;
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_pSpriteBody, ScaleX, ScaleY);
		Graphics()->QuadContainerAddSprite(m_SpriteQuadContainerIndex, 96.f * ScaleX, 96.f * ScaleY);
	}

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	m_QuadOffsetRaceFlag = Graphics()->QuadContainerAddSprite(m_SpriteQuadContainerIndex, 0.0f, 0.0f, RACE_FLAG_SIZE, RACE_FLAG_SIZE);
	Graphics()->QuadContainerUpload(m_SpriteQuadContainerIndex);
}

CInfoMessages::CCachedInfoMsg *CInfoMessages::FindCachedInfoMsg(CSessionId SessionId, uint64_t MessageId)
{
	const auto It = std::find_if(m_vCachedInfoMsgs.begin(), m_vCachedInfoMsgs.end(), [SessionId, MessageId](const CCachedInfoMsg &Cached) {
		return Cached.m_SessionId == SessionId && Cached.m_MessageId == MessageId;
	});
	return It == m_vCachedInfoMsgs.end() ? nullptr : &*It;
}

void CInfoMessages::AddInfoMsg(CSessionInfoMessageState &InfoMessages, CSessionId SessionId, CSessionInfoMessageState::CMessage Message, CCachedInfoMsg Cached)
{
	const CSessionInfoMessageState::CMessage &Stored = InfoMessages.Add(Message);
	Cached.m_SessionId = SessionId;
	Cached.m_MessageId = Stored.m_Id;

	int NumSessionMessages = 0;
	auto Oldest = m_vCachedInfoMsgs.end();
	for(auto It = m_vCachedInfoMsgs.begin(); It != m_vCachedInfoMsgs.end(); ++It)
	{
		if(It->m_SessionId != SessionId)
			continue;
		++NumSessionMessages;
		if(Oldest == m_vCachedInfoMsgs.end() || It->m_MessageId < Oldest->m_MessageId)
			Oldest = It;
	}
	if(NumSessionMessages == CSessionInfoMessageState::MAX_MESSAGES)
	{
		DeleteTextContainers(*Oldest);
		m_vCachedInfoMsgs.erase(Oldest);
	}
	m_vCachedInfoMsgs.push_back(std::move(Cached));
}

void CInfoMessages::CreateTextContainersIfNotCreated(const CRenderContext &Context, const CSessionInfoMessageState::CMessage &InfoMsg, CCachedInfoMsg &Cached)
{
	const int LocalClientId = Context.m_State.LocalClientId();
	if(Cached.m_OutputCacheKey != Context.m_OutputCacheKey || Cached.m_LocalClientId != LocalClientId || Cached.m_NormalColor != g_Config.m_ClKillMessageNormalColor || Cached.m_HighlightColor != g_Config.m_ClKillMessageHighlightColor)
	{
		DeleteTextContainers(Cached);
		Cached.m_OutputCacheKey = Context.m_OutputCacheKey;
		Cached.m_LocalClientId = LocalClientId;
		Cached.m_NormalColor = g_Config.m_ClKillMessageNormalColor;
		Cached.m_HighlightColor = g_Config.m_ClKillMessageHighlightColor;
	}

	const auto NameColor = [&](int ClientId) {
		const unsigned Color = ClientId == LocalClientId ? Cached.m_HighlightColor : Cached.m_NormalColor;
		return color_cast<ColorRGBA>(ColorHSLA(Color));
	};
	if(!Cached.m_VictimTextContainerIndex.Valid() && InfoMsg.m_aVictimName[0] != '\0')
	{
		CTextCursor Cursor;
		Cursor.m_FontSize = FONT_SIZE;
		TextRender()->TextColor(NameColor(InfoMsg.m_aVictimIds[0]));
		TextRender()->CreateTextContainer(Cached.m_VictimTextContainerIndex, &Cursor, InfoMsg.m_aVictimName);
	}
	if(!Cached.m_KillerTextContainerIndex.Valid() && InfoMsg.m_aKillerName[0] != '\0')
	{
		CTextCursor Cursor;
		Cursor.m_FontSize = FONT_SIZE;
		TextRender()->TextColor(NameColor(InfoMsg.m_KillerId));
		TextRender()->CreateTextContainer(Cached.m_KillerTextContainerIndex, &Cursor, InfoMsg.m_aKillerName);
	}
	if(!Cached.m_DiffTextContainerIndex.Valid() && InfoMsg.m_aDiffText[0] != '\0')
	{
		CTextCursor Cursor;
		Cursor.m_FontSize = FONT_SIZE;
		if(InfoMsg.m_Diff > 0)
			TextRender()->TextColor(1.0f, 0.5f, 0.5f, 1.0f);
		else if(InfoMsg.m_Diff < 0)
			TextRender()->TextColor(0.5f, 1.0f, 0.5f, 1.0f);
		else
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		TextRender()->CreateTextContainer(Cached.m_DiffTextContainerIndex, &Cursor, InfoMsg.m_aDiffText);
	}
	if(!Cached.m_TimeTextContainerIndex.Valid() && InfoMsg.m_aTimeText[0] != '\0')
	{
		CTextCursor Cursor;
		Cursor.m_FontSize = FONT_SIZE;
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		TextRender()->CreateTextContainer(Cached.m_TimeTextContainerIndex, &Cursor, InfoMsg.m_aTimeText);
	}
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CInfoMessages::HandleMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, bool SuppressEvents, int MsgType, void *pRawMsg)
{
	if(SuppressEvents)
		return;
	switch(MsgType)
	{
	case NETMSGTYPE_SV_KILLMSGTEAM:
		OnTeamKillMessage(InfoMessages, Session, State, SourceTick, static_cast<CNetMsg_Sv_KillMsgTeam *>(pRawMsg));
		break;
	case NETMSGTYPE_SV_KILLMSG:
		OnKillMessage(InfoMessages, Session, State, SourceTick, static_cast<CNetMsg_Sv_KillMsg *>(pRawMsg));
		break;
	case NETMSGTYPE_SV_RACEFINISH:
		OnRaceFinishMessage(InfoMessages, Session, State, SourceTick, static_cast<CNetMsg_Sv_RaceFinish *>(pRawMsg));
		break;
	}
}

void CInfoMessages::OnTeamKillMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, const CNetMsg_Sv_KillMsgTeam *pMsg)
{
	std::vector<std::pair<int, int>> vStrongWeakSorted;
	vStrongWeakSorted.reserve(MAX_CLIENTS);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!State.Client(ClientId).m_HasPlayerInfo || State.Teams().Team(ClientId) != pMsg->m_Team)
			continue;
		const CCharacter *pCharacter = State.GameWorld().GetCharacterById(ClientId);
		vStrongWeakSorted.emplace_back(ClientId, pMsg->m_First == ClientId ? MAX_CLIENTS : (pCharacter ? pCharacter->GetStrongWeakId() : 0));
	}
	std::stable_sort(vStrongWeakSorted.begin(), vStrongWeakSorted.end(), [](const auto &Left, const auto &Right) { return Left.second > Right.second; });

	CSessionInfoMessageState::CMessage Kill;
	Kill.m_Type = CSessionInfoMessageState::EType::KILL;
	Kill.m_Tick = SourceTick;
	Kill.m_VictimDDTeam = pMsg->m_Team;
	str_format(Kill.m_aVictimName, sizeof(Kill.m_aVictimName), Localize("Team %d"), pMsg->m_Team);
	CCachedInfoMsg Cached;
	CSessionPresentation &Presentation = GameClient()->SessionPresentation(Session.Id());
	for(const auto &[ClientId, StrongWeakId] : vStrongWeakSorted)
	{
		(void)StrongWeakId;
		auto pTee = Presentation.CreateClientTee(State, ClientId);
		if(pTee == nullptr)
			continue;
		const int Index = Kill.m_TeamSize++;
		Kill.m_aVictimIds[Index] = ClientId;
		Cached.m_apVictimManagedTeeRenderInfos[Index] = std::move(pTee);
		if(Kill.m_TeamSize == CSessionInfoMessageState::MAX_TEAM_MEMBERS)
			break;
	}
	AddInfoMsg(InfoMessages, Session.Id(), Kill, std::move(Cached));
}

void CInfoMessages::OnKillMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, const CNetMsg_Sv_KillMsg *pMsg)
{
	CSessionInfoMessageState::CMessage Kill;
	Kill.m_Type = CSessionInfoMessageState::EType::KILL;
	Kill.m_Tick = SourceTick;
	CCachedInfoMsg Cached;
	CSessionPresentation &Presentation = GameClient()->SessionPresentation(Session.Id());
	if(GetClientName(State, pMsg->m_Victim, Kill.m_aVictimName, sizeof(Kill.m_aVictimName)))
	{
		auto pTee = Presentation.CreateClientTee(State, pMsg->m_Victim);
		if(pTee != nullptr)
		{
			Kill.m_TeamSize = 1;
			Kill.m_aVictimIds[0] = pMsg->m_Victim;
			Kill.m_VictimDDTeam = State.Teams().Team(pMsg->m_Victim);
			Cached.m_apVictimManagedTeeRenderInfos[0] = std::move(pTee);
		}
	}
	if(GetClientName(State, pMsg->m_Killer, Kill.m_aKillerName, sizeof(Kill.m_aKillerName)))
	{
		auto pTee = Presentation.CreateClientTee(State, pMsg->m_Killer);
		if(pTee != nullptr)
		{
			Kill.m_KillerId = pMsg->m_Killer;
			Cached.m_pKillerManagedTeeRenderInfo = std::move(pTee);
		}
	}
	Kill.m_Weapon = pMsg->m_Weapon;
	Kill.m_ModeSpecial = pMsg->m_ModeSpecial;
	Kill.m_FlagCarrierBlue = State.GameData() ? State.GameData()->m_FlagCarrierBlue : -1;
	if(Kill.m_TeamSize == 0 && Kill.m_KillerId == -1 && Kill.m_Weapon < 0)
		return;
	AddInfoMsg(InfoMessages, Session.Id(), Kill, std::move(Cached));
}

void CInfoMessages::OnRaceFinishMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, const CNetMsg_Sv_RaceFinish *pMsg)
{
	CSessionInfoMessageState::CMessage Finish;
	Finish.m_Type = CSessionInfoMessageState::EType::FINISH;
	Finish.m_Tick = SourceTick;
	if(!GetClientName(State, pMsg->m_ClientId, Finish.m_aVictimName, sizeof(Finish.m_aVictimName)))
		return;
	CCachedInfoMsg Cached;
	auto pTee = GameClient()->SessionPresentation(Session.Id()).CreateClientTee(State, pMsg->m_ClientId);
	if(pTee == nullptr)
		return;
	Finish.m_TeamSize = 1;
	Finish.m_aVictimIds[0] = pMsg->m_ClientId;
	Finish.m_VictimDDTeam = State.Teams().Team(pMsg->m_ClientId);
	Cached.m_apVictimManagedTeeRenderInfos[0] = std::move(pTee);
	Finish.m_Diff = pMsg->m_Diff;
	Finish.m_RecordPersonal = pMsg->m_RecordPersonal || pMsg->m_RecordServer;
	if(Finish.m_Diff)
	{
		char aBuf[64];
		str_time_float(absolute(Finish.m_Diff) / 1000.0f, ETimeFormat::HOURS_CENTISECS, aBuf, sizeof(aBuf));
		str_format(Finish.m_aDiffText, sizeof(Finish.m_aDiffText), "(%c%s)", Finish.m_Diff < 0 ? '-' : '+', aBuf);
	}
	str_time_float(pMsg->m_Time / 1000.0f, ETimeFormat::HOURS_CENTISECS, Finish.m_aTimeText, sizeof(Finish.m_aTimeText));
	AddInfoMsg(InfoMessages, Session.Id(), Finish, std::move(Cached));
}

void CInfoMessages::RenderKillMsg(const CRenderContext &Context, const CSessionInfoMessageState::CMessage &InfoMsg, const CCachedInfoMsg &Cached, float x, float y)
{
	const ColorRGBA TextColor = InfoMsg.m_VictimDDTeam ? GameClient()->GetDDTeamColor(InfoMsg.m_VictimDDTeam, 0.75f) : TextRender()->DefaultTextColor();
	if(Cached.m_VictimTextContainerIndex.Valid())
	{
		x -= TextRender()->GetBoundingBoxTextContainer(Cached.m_VictimTextContainerIndex).m_W;
		TextRender()->RenderTextContainer(Cached.m_VictimTextContainerIndex, TextColor, TextRender()->DefaultTextOutlineColor(), x, y + (ROW_HEIGHT - FONT_SIZE) / 2.0f);
	}

	x -= 24.0f;
	if(Context.m_State.HasGameInfo() && (Context.m_State.GameInfo().m_GameFlags & GAMEFLAG_FLAGS) && (InfoMsg.m_ModeSpecial & 1))
	{
		const bool Blue = InfoMsg.m_aVictimIds[0] == InfoMsg.m_FlagCarrierBlue;
		Graphics()->TextureSet(Blue ? GameClient()->m_GameSkin.m_SpriteFlagBlue : GameClient()->m_GameSkin.m_SpriteFlagRed);
		Graphics()->RenderQuadContainerAsSprite(m_SpriteQuadContainerIndex, Blue ? 0 : 1, x, y - 16);
	}
	for(int j = InfoMsg.m_TeamSize - 1; j >= 0; --j)
	{
		const auto &pTee = Cached.m_apVictimManagedTeeRenderInfos[j];
		if(pTee == nullptr)
			continue;
		vec2 OffsetToMid;
		CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &pTee->TeeRenderInfo(), OffsetToMid);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &pTee->TeeRenderInfo(), EMOTE_PAIN, vec2(-1, 0), vec2(x, y + ROW_HEIGHT / 2.0f + OffsetToMid.y));
		x -= 44.0f;
	}
	x -= 32.0f;
	if(InfoMsg.m_Weapon >= 0)
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[InfoMsg.m_Weapon]);
		Graphics()->RenderQuadContainerAsSprite(m_SpriteQuadContainerIndex, 4 + InfoMsg.m_Weapon, x, y + 28);
	}
	x -= 52.0f;
	if(InfoMsg.m_aVictimIds[0] == InfoMsg.m_KillerId)
		return;
	if(Context.m_State.HasGameInfo() && (Context.m_State.GameInfo().m_GameFlags & GAMEFLAG_FLAGS) && (InfoMsg.m_ModeSpecial & 2))
	{
		const bool Blue = InfoMsg.m_KillerId == InfoMsg.m_FlagCarrierBlue;
		Graphics()->TextureSet(Blue ? GameClient()->m_GameSkin.m_SpriteFlagBlue : GameClient()->m_GameSkin.m_SpriteFlagRed);
		Graphics()->RenderQuadContainerAsSprite(m_SpriteQuadContainerIndex, Blue ? 2 : 3, x - 56, y - 16);
	}
	x -= 24.0f;
	if(Cached.m_pKillerManagedTeeRenderInfo != nullptr)
	{
		vec2 OffsetToMid;
		const CTeeRenderInfo &RenderInfo = Cached.m_pKillerManagedTeeRenderInfo->TeeRenderInfo();
		CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &RenderInfo, OffsetToMid);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &RenderInfo, EMOTE_ANGRY, vec2(1, 0), vec2(x, y + ROW_HEIGHT / 2.0f + OffsetToMid.y));
	}
	x -= 32.0f;
	if(Cached.m_KillerTextContainerIndex.Valid())
	{
		x -= TextRender()->GetBoundingBoxTextContainer(Cached.m_KillerTextContainerIndex).m_W;
		TextRender()->RenderTextContainer(Cached.m_KillerTextContainerIndex, TextColor, TextRender()->DefaultTextOutlineColor(), x, y + (ROW_HEIGHT - FONT_SIZE) / 2.0f);
	}
}

void CInfoMessages::RenderFinishMsg(const CSessionInfoMessageState::CMessage &InfoMsg, const CCachedInfoMsg &Cached, float x, float y)
{
	if(Cached.m_DiffTextContainerIndex.Valid())
	{
		x -= TextRender()->GetBoundingBoxTextContainer(Cached.m_DiffTextContainerIndex).m_W;
		TextRender()->RenderTextContainer(Cached.m_DiffTextContainerIndex, TextRender()->DefaultTextColor(), TextRender()->DefaultTextOutlineColor(), x, y + (ROW_HEIGHT - FONT_SIZE) / 2.0f);
	}
	if(Cached.m_TimeTextContainerIndex.Valid())
	{
		x -= TextRender()->GetBoundingBoxTextContainer(Cached.m_TimeTextContainerIndex).m_W;
		TextRender()->RenderTextContainer(Cached.m_TimeTextContainerIndex, TextRender()->DefaultTextColor(), TextRender()->DefaultTextOutlineColor(), x, y + (ROW_HEIGHT - FONT_SIZE) / 2.0f);
	}
	x -= RACE_FLAG_SIZE;
	Graphics()->TextureSet(g_pData->m_aImages[IMAGE_RACEFLAG].m_Id);
	Graphics()->RenderQuadContainerAsSprite(m_SpriteQuadContainerIndex, m_QuadOffsetRaceFlag, x, y);
	if(Cached.m_VictimTextContainerIndex.Valid())
	{
		x -= TextRender()->GetBoundingBoxTextContainer(Cached.m_VictimTextContainerIndex).m_W;
		const ColorRGBA TextColor = InfoMsg.m_VictimDDTeam ? GameClient()->GetDDTeamColor(InfoMsg.m_VictimDDTeam, 0.75f) : TextRender()->DefaultTextColor();
		TextRender()->RenderTextContainer(Cached.m_VictimTextContainerIndex, TextColor, TextRender()->DefaultTextOutlineColor(), x, y + (ROW_HEIGHT - FONT_SIZE) / 2.0f);
	}
	x -= 24.0f;
	const auto &pTee = Cached.m_apVictimManagedTeeRenderInfos[0];
	if(pTee == nullptr)
		return;
	vec2 OffsetToMid;
	CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &pTee->TeeRenderInfo(), OffsetToMid);
	RenderTools()->RenderTee(CAnimState::GetIdle(), &pTee->TeeRenderInfo(), InfoMsg.m_RecordPersonal ? EMOTE_HAPPY : EMOTE_NORMAL, vec2(-1, 0), vec2(x, y + ROW_HEIGHT / 2.0f + OffsetToMid.y));
}

void CInfoMessages::OnRender(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive)
		return;
	const float Height = 1.5f * 400.0f * 3.0f;
	const float Width = Height * Context.AspectRatio(Graphics()->ScreenAspect());
	Graphics()->MapScreenToSize(Width, Height);
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	const bool ShowFps = g_Config.m_ClShowfps && !Context.m_IsVideoOutput;
	const float StartX = Width - 10.0f;
	const float StartY = 30.0f + (ShowFps ? 100.0f : 0.0f) + (g_Config.m_ClShowpred && !Context.m_Time.m_IsDemoPlayback ? 100.0f : 0.0f);

	float y = StartY;
	const CSessionInfoMessageState &InfoMessages = Context.m_Session.InfoMessages();
	for(int i = 0; i < InfoMessages.Count(); ++i)
	{
		const CSessionInfoMessageState::CMessage &InfoMsg = InfoMessages.Message(i);
		if(Context.m_Time.m_GameTick > InfoMsg.m_Tick + Context.m_Time.m_GameTickSpeed * 10)
			continue;
		CCachedInfoMsg *pCached = FindCachedInfoMsg(Context.m_Session.Id(), InfoMsg.m_Id);
		if(pCached == nullptr)
			continue;
		CreateTextContainersIfNotCreated(Context, InfoMsg, *pCached);
		if(InfoMsg.m_Type == CSessionInfoMessageState::EType::KILL && g_Config.m_ClShowKillMessages)
		{
			RenderKillMsg(Context, InfoMsg, *pCached, StartX, y);
			y += ROW_HEIGHT;
		}
		else if(InfoMsg.m_Type == CSessionInfoMessageState::EType::FINISH && g_Config.m_ClShowFinishMessages)
		{
			RenderFinishMsg(InfoMsg, *pCached, StartX, y);
			y += ROW_HEIGHT;
		}
	}
}
