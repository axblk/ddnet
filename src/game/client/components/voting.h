/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_VOTING_H
#define GAME_CLIENT_COMPONENTS_VOTING_H

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/ui_rect.h>
#include <game/voting.h>

#include <list>
#include <string>

class CSessionVoteState;

class CVoting : public CComponent
{
	static void ConCallvote(IConsole::IResult *pResult, void *pUserData);
	static void ConVote(IConsole::IResult *pResult, void *pUserData);

	void Callvote(const char *pType, const char *pValue, const char *pReason);

	CSessionVoteState &VoteState();
	const CSessionVoteState &VoteState() const;
	void RenderBars(CUIRect Bars, const CSessionVoteState &State) const;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnUpdate() override;
	void OnConsoleInit() override;
	void HandleMessage(int MsgType, void *pRawMsg);

	void Render();

	void CallvoteSpectate(int ClientId, const char *pReason, bool ForceVote = false);
	void CallvoteKick(int ClientId, const char *pReason, bool ForceVote = false);
	void CallvoteOption(int OptionId, const char *pReason, bool ForceVote = false);
	void RemovevoteOption(int OptionId);
	void AddvoteOption(const char *pDescription, const char *pCommand);
	void AddOption(const char *pDescription);

	void Vote(int v); // -1 = no, 1 = yes

	int SecondsLeft() const;
	bool IsVoting() const;
	int TakenChoice() const;
	const char *VoteDescription() const;
	const char *VoteReason() const;
	bool IsReceivingOptions() const;
	int NumOptions() const;
	const std::list<std::string> &Options() const;
};

#endif
