/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_INFOMESSAGES_H
#define GAME_CLIENT_COMPONENTS_INFOMESSAGES_H

#include <engine/textrender.h>

#include <game/client/component.h>
#include <game/client/render.h>
#include <game/client/session_context.h>

#include <array>
#include <memory>
#include <vector>

class CInfoMessages : public CComponent
{
	struct CCachedInfoMsg
	{
		CSessionId m_SessionId;
		uint64_t m_MessageId = 0;
		std::array<std::shared_ptr<CManagedTeeRenderInfo>, CSessionInfoMessageState::MAX_TEAM_MEMBERS> m_apVictimManagedTeeRenderInfos;
		std::shared_ptr<CManagedTeeRenderInfo> m_pKillerManagedTeeRenderInfo;
		STextContainerIndex m_VictimTextContainerIndex;
		STextContainerIndex m_KillerTextContainerIndex;
		STextContainerIndex m_TimeTextContainerIndex;
		STextContainerIndex m_DiffTextContainerIndex;
		uint64_t m_OutputCacheKey = 0;
		int m_LocalClientId = -2;
		unsigned m_NormalColor = 0;
		unsigned m_HighlightColor = 0;
	};

	int m_SpriteQuadContainerIndex = -1;
	int m_QuadOffsetRaceFlag = -1;
	std::vector<CCachedInfoMsg> m_vCachedInfoMsgs;

	CCachedInfoMsg *FindCachedInfoMsg(CSessionId SessionId, uint64_t MessageId);
	void AddInfoMsg(CSessionInfoMessageState &InfoMessages, CSessionId SessionId, CSessionInfoMessageState::CMessage Message, CCachedInfoMsg Cached);
	void RenderKillMsg(const CRenderContext &Context, const CSessionInfoMessageState::CMessage &InfoMsg, const CCachedInfoMsg &Cached, float x, float y);
	void RenderFinishMsg(const CSessionInfoMessageState::CMessage &InfoMsg, const CCachedInfoMsg &Cached, float x, float y);

	void OnTeamKillMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, const struct CNetMsg_Sv_KillMsgTeam *pMsg);
	void OnKillMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, const struct CNetMsg_Sv_KillMsg *pMsg);
	void OnRaceFinishMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, const struct CNetMsg_Sv_RaceFinish *pMsg);

	void CreateTextContainersIfNotCreated(const CRenderContext &Context, const CSessionInfoMessageState::CMessage &InfoMsg, CCachedInfoMsg &Cached);
	void DeleteTextContainers(CCachedInfoMsg &Cached);
	void DeleteAllTextContainers();

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnWindowResize() override;
	void OnShutdown() override;
	void OnRender(const CRenderContext &Context) override;
	void OnInit() override;
	void HandleMessage(CSessionInfoMessageState &InfoMessages, const CGameSessionContext &Session, const CGameState &State, int SourceTick, bool SuppressEvents, int MsgType, void *pRawMsg);
	void ResetPresentation(CSessionId SessionId);
};

#endif
