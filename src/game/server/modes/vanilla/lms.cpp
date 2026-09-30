#include "dm.h"
#include "survival.h"

#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/protocol.h>

#include <generated/protocol7.h>

#include <game/server/player.h>

namespace
{
	// Last Man Standing: who is left alone scores the round
	class CGameControllerLMS final : public CGameControllerSurvival<CGameControllerVanillaDM>
	{
		using CBase = CGameControllerSurvival<CGameControllerVanillaDM>;

	public:
		using CBase::CBase;

	protected:
		// the score limit is checked when a round ends, not on each kill
		void TickMatch() override {}

		bool HasEnoughPlayers() const override
		{
			int NumPlayers = 0;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				NumPlayers += IsPlaying(ClientId);
			return NumPlayers > 1;
		}

		void DoWincheckRound() override
		{
			char aResult[128];
			if(IsRoundTimeUp())
			{
				// everybody still in scores
				int NumAlive = 0;
				for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				{
					if(!IsAlive(ClientId))
						continue;
					VanillaPlayer(ClientId)->m_Score++;
					AddRoundWon(Services().Player(ClientId));
					NumAlive++;
				}
				str_format(aResult, sizeof(aResult), "Round %d: time is up, %d still standing", Round() + 1, NumAlive);
				EndSurvivalRound(aResult);
				return;
			}

			int NumAlive = 0;
			int AliveId = -1;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(IsAlive(ClientId))
				{
					NumAlive++;
					AliveId = ClientId;
				}
			}
			if(NumAlive == 0)
			{
				str_format(aResult, sizeof(aResult), "Round %d: nobody is left", Round() + 1);
				EndSurvivalRound(aResult);
			}
			else if(NumAlive == 1)
			{
				VanillaPlayer(AliveId)->m_Score++;
				AddRoundWon(Services().Player(AliveId));
				str_format(aResult, sizeof(aResult), "'%s' wins round %d", Server()->ClientName(AliveId), Round() + 1);
				EndSurvivalRound(aResult);
			}
		}

		bool DoWincheckMatch() const override
		{
			// spectators keep their score and count too, like in stock vanilla
			int TopScore = 0;
			int NumTopScores = 0;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				const CPlayerVanilla *pPlayer = VanillaPlayer(ClientId);
				if(!pPlayer)
					continue;
				if(pPlayer->m_Score > TopScore)
				{
					TopScore = pPlayer->m_Score;
					NumTopScores = 1;
				}
				else if(pPlayer->m_Score == TopScore)
				{
					NumTopScores++;
				}
			}
			// a tie is played out in more rounds
			return ((ScoreLimit() > 0 && TopScore >= ScoreLimit()) || IsRoundTimeUp()) && NumTopScores == 1;
		}
	};
}

static const CGameModeRegistration gs_LMS({"lms", "LMS", protocol7::GAMEFLAG_SURVIVAL, false}, NewGameController<CGameControllerLMS>);
