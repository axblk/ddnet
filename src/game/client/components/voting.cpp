/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "voting.h"

#include <base/str.h>
#include <base/time.h>

#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/components/scoreboard.h>
#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

void CVoting::ConCallvote(IConsole::IResult *pResult, void *pUserData)
{
	CVoting *pSelf = (CVoting *)pUserData;
	pSelf->Callvote(pResult->GetString(0), pResult->GetString(1), pResult->NumArguments() > 2 ? pResult->GetString(2) : "");
}

void CVoting::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CVoting *pSelf = (CVoting *)pUserData;
	if(str_comp_nocase(pResult->GetString(0), "yes") == 0)
		pSelf->Vote(1);
	else if(str_comp_nocase(pResult->GetString(0), "no") == 0)
		pSelf->Vote(-1);
}

CSessionVoteState &CVoting::VoteState()
{
	return GameClient()->SessionContext().Vote();
}

const CSessionVoteState &CVoting::VoteState() const
{
	return GameClient()->SessionContext().Vote();
}

void CVoting::Callvote(const char *pType, const char *pValue, const char *pReason)
{
	if(Client()->IsSixup())
	{
		protocol7::CNetMsg_Cl_CallVote Msg;
		Msg.m_pType = pType;
		Msg.m_pValue = pValue;
		Msg.m_pReason = pReason;
		Msg.m_Force = false;
		Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_VITAL, true);
		return;
	}
	CNetMsg_Cl_CallVote Msg = {nullptr};
	Msg.m_pType = pType;
	Msg.m_pValue = pValue;
	Msg.m_pReason = pReason;
	Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_VITAL);
}

void CVoting::CallvoteSpectate(int ClientId, const char *pReason, bool ForceVote)
{
	if(ForceVote)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "set_team %d -1", ClientId);
		Client()->Rcon(aBuf);
	}
	else
	{
		char aBuf[32];
		str_format(aBuf, sizeof(aBuf), "%d", ClientId);
		Callvote("spectate", aBuf, pReason);
	}
}

void CVoting::CallvoteKick(int ClientId, const char *pReason, bool ForceVote)
{
	if(ForceVote)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "force_vote kick %d %s", ClientId, pReason);
		Client()->Rcon(aBuf);
	}
	else
	{
		char aBuf[32];
		str_format(aBuf, sizeof(aBuf), "%d", ClientId);
		Callvote("kick", aBuf, pReason);
	}
}

void CVoting::CallvoteOption(int OptionId, const char *pReason, bool ForceVote)
{
	const std::string *pOption = VoteState().Option(OptionId);
	if(!pOption)
		return;

	if(ForceVote)
	{
		char aBuf[128] = "force_vote option \"";
		char *pDst = aBuf + str_length(aBuf);
		str_escape(&pDst, pOption->c_str(), aBuf + sizeof(aBuf));
		str_append(aBuf, "\" \"");
		pDst = aBuf + str_length(aBuf);
		str_escape(&pDst, pReason, aBuf + sizeof(aBuf));
		str_append(aBuf, "\"");
		Client()->Rcon(aBuf);
	}
	else
		Callvote("option", pOption->c_str(), pReason);
}

void CVoting::RemovevoteOption(int OptionId)
{
	const std::string *pOption = VoteState().Option(OptionId);
	if(!pOption)
		return;

	char aBuf[128] = "remove_vote \"";
	char *pDst = aBuf + str_length(aBuf);
	str_escape(&pDst, pOption->c_str(), aBuf + sizeof(aBuf));
	str_append(aBuf, "\"");
	Client()->Rcon(aBuf);
}

void CVoting::AddvoteOption(const char *pDescription, const char *pCommand)
{
	char aBuf[128] = "add_vote \"";
	char *pDst = aBuf + str_length(aBuf);
	str_escape(&pDst, pDescription, aBuf + sizeof(aBuf));
	str_append(aBuf, "\" \"");
	pDst = aBuf + str_length(aBuf);
	str_escape(&pDst, pCommand, aBuf + sizeof(aBuf));
	str_append(aBuf, "\"");
	Client()->Rcon(aBuf);
}

void CVoting::Vote(int v)
{
	CNetMsg_Cl_Vote Msg = {v};
	Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_VITAL);
}

bool CVoting::IsVoting() const
{
	return VoteState().IsVoting();
}

bool CVoting::IsReceivingOptions() const
{
	return VoteState().IsReceivingOptions();
}

int CVoting::NumOptions() const
{
	return VoteState().NumOptions();
}

const std::list<std::string> &CVoting::Options() const
{
	return VoteState().Options();
}

void CVoting::OnConsoleInit()
{
	Console()->Register("callvote", "s['kick'|'spectate'|'option'] s[id|option text] ?r[reason]", CFGFLAG_CLIENT, ConCallvote, this, "Call vote");
	Console()->Register("vote", "r['yes'|'no']", CFGFLAG_CLIENT, ConVote, this, "Vote yes/no");
}

void CVoting::HandleMessage(CSessionVoteState &State, int64_t Now, int64_t TimeFrequency, bool NotifyAdmin, int MsgType, void *pRawMsg)
{
	if(MsgType == NETMSGTYPE_SV_VOTESET)
	{
		CNetMsg_Sv_VoteSet *pMsg = (CNetMsg_Sv_VoteSet *)pRawMsg;
		if(State.ApplyVoteSet(pMsg->m_Timeout, pMsg->m_pDescription, pMsg->m_pReason, Now, TimeFrequency) && NotifyAdmin)
		{
			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "%s (%s)", State.Description(), State.Reason());
			Client()->Notify("DDNet Vote", aBuf);
			GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_HIGHLIGHT, 1.0f);
		}
	}
	else if(MsgType == NETMSGTYPE_SV_VOTESTATUS)
	{
		CNetMsg_Sv_VoteStatus *pMsg = (CNetMsg_Sv_VoteStatus *)pRawMsg;
		State.ApplyStatus(pMsg->m_Yes, pMsg->m_No, pMsg->m_Pass, pMsg->m_Total);
	}
	else if(MsgType == NETMSGTYPE_SV_VOTECLEAROPTIONS)
	{
		State.ClearOptions();
	}
	else if(MsgType == NETMSGTYPE_SV_VOTEOPTIONLISTADD)
	{
		CNetMsg_Sv_VoteOptionListAdd *pMsg = (CNetMsg_Sv_VoteOptionListAdd *)pRawMsg;
		int NumOptions = pMsg->m_NumOptions;
		for(int i = 0; i < NumOptions; ++i)
		{
			switch(i)
			{
			case 0: State.AddOption(pMsg->m_pDescription0); break;
			case 1: State.AddOption(pMsg->m_pDescription1); break;
			case 2: State.AddOption(pMsg->m_pDescription2); break;
			case 3: State.AddOption(pMsg->m_pDescription3); break;
			case 4: State.AddOption(pMsg->m_pDescription4); break;
			case 5: State.AddOption(pMsg->m_pDescription5); break;
			case 6: State.AddOption(pMsg->m_pDescription6); break;
			case 7: State.AddOption(pMsg->m_pDescription7); break;
			case 8: State.AddOption(pMsg->m_pDescription8); break;
			case 9: State.AddOption(pMsg->m_pDescription9); break;
			case 10: State.AddOption(pMsg->m_pDescription10); break;
			case 11: State.AddOption(pMsg->m_pDescription11); break;
			case 12: State.AddOption(pMsg->m_pDescription12); break;
			case 13: State.AddOption(pMsg->m_pDescription13); break;
			case 14: State.AddOption(pMsg->m_pDescription14);
			}
		}
	}
	else if(MsgType == NETMSGTYPE_SV_VOTEOPTIONADD)
	{
		CNetMsg_Sv_VoteOptionAdd *pMsg = (CNetMsg_Sv_VoteOptionAdd *)pRawMsg;
		State.AddOption(pMsg->m_pDescription);
	}
	else if(MsgType == NETMSGTYPE_SV_VOTEOPTIONREMOVE)
	{
		CNetMsg_Sv_VoteOptionRemove *pMsg = (CNetMsg_Sv_VoteOptionRemove *)pRawMsg;
		State.RemoveOption(pMsg->m_pDescription);
	}
	else if(MsgType == NETMSGTYPE_SV_YOURVOTE)
	{
		CNetMsg_Sv_YourVote *pMsg = (CNetMsg_Sv_YourVote *)pRawMsg;
		State.SetVoted(pMsg->m_Voted);
	}
	else if(MsgType == NETMSGTYPE_SV_VOTEOPTIONGROUPSTART)
	{
		State.SetReceivingOptions(true);
	}
	else if(MsgType == NETMSGTYPE_SV_VOTEOPTIONGROUPEND)
	{
		State.SetReceivingOptions(false);
	}
}

void CVoting::Render(const CRenderContext &Context)
{
	const CSessionVoteState &State = Context.m_Session.Vote();
	if((!g_Config.m_ClShowVotesAfterVoting && !GameClient()->m_Scoreboard.IsActive(Context) && State.Voted()) || !State.IsVoting())
		return;
	const int Seconds = State.SecondsLeft(Context.m_Time.m_PresentationTime, Context.m_Time.m_PresentationTimeFrequency);
	if(Seconds < 0)
		return;

	CUIRect View = {0.0f, 60.0f, 120.0f, 38.0f};
	View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_R, 3.0f);
	View.Margin(3.0f, &View);

	SLabelProperties Props;
	Props.m_EllipsisAtEnd = true;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), Localize("%ds left"), Seconds);

	CUIRect Row, LeftColumn, RightColumn, ProgressSpinner;
	View.HSplitTop(6.0f, &Row, &View);
	Row.VSplitRight(TextRender()->TextWidth(6.0f, aBuf), &LeftColumn, &RightColumn);
	LeftColumn.VSplitRight(2.0f, &LeftColumn, nullptr);
	LeftColumn.VSplitRight(6.0f, &LeftColumn, &ProgressSpinner);
	LeftColumn.VSplitRight(2.0f, &LeftColumn, nullptr);

	SProgressSpinnerProperties ProgressProps;
	ProgressProps.m_Progress = std::clamp((Context.m_Time.m_PresentationTime - State.OpenTime()) / (float)(State.CloseTime() - State.OpenTime()), 0.0f, 1.0f);
	Ui()->RenderProgressSpinner(ProgressSpinner.Center(), ProgressSpinner.h / 2.0f, ProgressProps);

	Ui()->DoLabel(&RightColumn, aBuf, 6.0f, TEXTALIGN_MR);

	Props.m_MaxWidth = LeftColumn.w;
	Ui()->DoLabel(&LeftColumn, State.Description(), 6.0f, TEXTALIGN_ML, Props);

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(6.0f, &Row, &View);
	str_format(aBuf, sizeof(aBuf), "%s %s", Localize("Reason:"), State.Reason());
	Props.m_MaxWidth = Row.w;
	Ui()->DoLabel(&Row, aBuf, 6.0f, TEXTALIGN_ML, Props);

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(4.0f, &Row, &View);
	RenderBars(Row, State);

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(6.0f, &Row, &View);
	Row.VSplitMid(&LeftColumn, &RightColumn, 4.0f);

	char aKey[64];
	GameClient()->m_Binds.GetKey("vote yes", aKey, sizeof(aKey));
	str_format(aBuf, sizeof(aBuf), "%s - %s", aKey, Localize("Vote yes"));
	TextRender()->TextColor(State.Voted() == 1 ? ColorRGBA(0.2f, 0.9f, 0.2f, 0.85f) : TextRender()->DefaultTextColor());
	Ui()->DoLabel(&LeftColumn, aBuf, 6.0f, TEXTALIGN_ML);

	GameClient()->m_Binds.GetKey("vote no", aKey, sizeof(aKey));
	str_format(aBuf, sizeof(aBuf), "%s - %s", Localize("Vote no"), aKey);
	TextRender()->TextColor(State.Voted() == -1 ? ColorRGBA(0.95f, 0.25f, 0.25f, 0.85f) : TextRender()->DefaultTextColor());
	Ui()->DoLabel(&RightColumn, aBuf, 6.0f, TEXTALIGN_MR);

	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CVoting::RenderBars(CUIRect Bars, const CSessionVoteState &State) const
{
	Bars.Draw(ColorRGBA(0.8f, 0.8f, 0.8f, 0.5f), IGraphics::CORNER_ALL, Bars.h / 2.0f);

	CUIRect Splitter;
	Bars.VMargin((Bars.w - 2.0f) / 2.0f, &Splitter);
	Splitter.Draw(ColorRGBA(0.4f, 0.4f, 0.4f, 0.5f), IGraphics::CORNER_NONE, 0.0f);

	if(State.Total())
	{
		if(State.Yes())
		{
			CUIRect YesArea;
			Bars.VSplitLeft(Bars.w * State.Yes() / State.Total(), &YesArea, nullptr);
			YesArea.Draw(ColorRGBA(0.2f, 0.9f, 0.2f, 0.85f), IGraphics::CORNER_ALL, YesArea.h / 2.0f);
		}

		if(State.No())
		{
			CUIRect NoArea;
			Bars.VSplitRight(Bars.w * State.No() / State.Total(), nullptr, &NoArea);
			NoArea.Draw(ColorRGBA(0.9f, 0.2f, 0.2f, 0.85f), IGraphics::CORNER_ALL, NoArea.h / 2.0f);
		}
	}
}
