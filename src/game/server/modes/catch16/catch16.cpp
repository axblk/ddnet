// The rules follow catch16 by Clarus, rebuilt from its description, and its successor catch64
// (AssassinTee, zlib licence, https://github.com/AssassinTee/catch64).
#include "groups.h"

#include <base/log.h>
#include <base/secure.h>
#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/modes/insta/instagib.h>
#include <game/server/modes/pvp/pvp.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/player.h>
#include <game/server/teeinfo.h>

#include <bitset>

namespace
{
	using CCatch16Base = CGameControllerSpawnWeaponInstagib<CGameControllerPvP<CGameControllerVanillaDM>>;

	/**
	 * catch16: everybody starts as a group of their own colour, and whoever
	 * is hit comes back right away in the group and the colour of the one
	 * who hit them. Hits inside a group only push. The round is over when
	 * all players are one group; its founder wins and gets
	 * sv_catch16_win_bonus points on top of the kills, which count as in DM.
	 *
	 * Who joins goes into one of the biggest groups. When the founder of a
	 * group leaves, the others in it are their own groups again.
	 */
	class CGameControllerCatch16 final : public CCatch16Base
	{
		CCatch16Groups m_Groups;

		std::bitset<MAX_CLIENTS> Playing() const
		{
			std::bitset<MAX_CLIENTS> Playing;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				const CPlayer *pPlayer = Services().Player(ClientId);
				Playing.set(ClientId, pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS && Server()->ClientIngame(ClientId));
			}
			return Playing;
		}

		void TellGroup(int ClientId)
		{
			const int Group = m_Groups.Group(ClientId);
			char aColor[32];
			Catch16::GroupColorName(Group, aColor, sizeof(aColor));
			char aBuf[128];
			if(Group == ClientId)
				str_format(aBuf, sizeof(aBuf), "You are back in your team (%s)", aColor);
			else
				str_format(aBuf, sizeof(aBuf), "You are now in Team '%s' (%s)", Server()->ClientName(Group), aColor);
			Services().SendBroadcast(aBuf, ClientId);
		}

		void JoinBiggestGroup(CPlayer *pPlayer)
		{
			const int ClientId = pPlayer->GetCid();
			std::bitset<MAX_CLIENTS> Others = Playing();
			Others.reset(ClientId);
			const std::vector<int> vGroups = m_Groups.BiggestGroups(Others);
			if(vGroups.empty())
				m_Groups.Leave(ClientId);
			else
				m_Groups.Join(ClientId, vGroups[secure_rand_below(vGroups.size())]);
			TellGroup(ClientId);
		}

		void UpdateColors(CPlayer *pPlayer)
		{
			if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			{
				pPlayer->SetTeeInfoOverride(std::nullopt);
				return;
			}
			const Catch16::CColors Colors = Catch16::GroupColors(m_Groups.Group(pPlayer->GetCid()));
			pPlayer->SetTeeInfoOverride(CTeeInfoOverride::Colors(Colors.m_Body, Colors.m_Feet));
		}

	public:
		using CCatch16Base::CCatch16Base;

		bool OnCharacterTakeDamage(CCharacter *pVictim, const CGameDamageContext &Context) override
		{
			const int VictimId = pVictim->GetPlayer()->GetCid();
			if(Context.m_From < 0 || Context.m_From >= MAX_CLIENTS || Context.m_From == VictimId || !m_Groups.SameGroup(VictimId, Context.m_From))
				return CCatch16Base::OnCharacterTakeDamage(pVictim, Context);
			// a hit inside the group only pushes
			CGameDamageContext Push = Context;
			Push.m_CanDamage = false;
			return CCatch16Base::OnCharacterTakeDamage(pVictim, Push);
		}

		void OnCharacterDeath(const CGameCharacterDeathContext &Context) override
		{
			CCatch16Base::OnCharacterDeath(Context);
			CPlayer *pKiller = Context.m_pKiller;
			const int VictimId = Context.m_pVictim->GetPlayer()->GetCid();
			if(!pKiller || pKiller->GetCid() == VictimId || Context.m_Weapon == WEAPON_GAME || Match().IsGameOver())
				return;
			if(!m_Groups.Join(VictimId, m_Groups.Group(pKiller->GetCid())))
				return;
			AddMatchMetric(pKiller, "converts");
			TellGroup(VictimId);
			// back right away, in the new colour
			VanillaPlayer(VictimId)->m_EarliestRespawnTick = Server()->Tick();
		}

		void OnPlayerConnect(CPlayer *pPlayer) override
		{
			CCatch16Base::OnPlayerConnect(pPlayer);
			m_Groups.Leave(pPlayer->GetCid());
			if(pPlayer->GetTeam() != TEAM_SPECTATORS)
				JoinBiggestGroup(pPlayer);
			UpdateColors(pPlayer);
		}

		void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override
		{
			for(const int ClientId : m_Groups.Dissolve(pPlayer->GetCid()))
				TellGroup(ClientId);
			CCatch16Base::OnPlayerDisconnect(pPlayer, pReason);
		}

		void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override
		{
			const int OldTeam = pPlayer->GetTeam();
			CCatch16Base::DoTeamChange(pPlayer, Team, DoChatMsg);
			if(pPlayer->GetTeam() == OldTeam)
				return;
			if(pPlayer->GetTeam() == TEAM_SPECTATORS)
				m_Groups.Leave(pPlayer->GetCid());
			else
				JoinBiggestGroup(pPlayer);
			UpdateColors(pPlayer);
		}

		void StartRound() override
		{
			m_Groups.Reset();
			CCatch16Base::StartRound();
		}

		void TickMatch() override
		{
			if(Match().IsRunning() && !IsGamePaused() && !Services().World().ResetRequested())
			{
				const int Group = m_Groups.OnlyGroup(Playing());
				if(CPlayer *pFounder = Group >= 0 ? Services().Player(Group) : nullptr)
				{
					VanillaPlayer(Group)->m_Score += g_Config.m_SvCatch16WinBonus;
					SetMatchWinners({pFounder});
					char aColor[32];
					Catch16::GroupColorName(Group, aColor, sizeof(aColor));
					char aBuf[128];
					str_format(aBuf, sizeof(aBuf), "Team '%s' of player '%s' won the round!", aColor, Server()->ClientName(Group));
					Services().SendBroadcast(aBuf, -1);
					Services().SendChat(-1, TEAM_ALL, aBuf);
					log_info("catch16", "the group of '%s' caught everybody", Server()->ClientName(Group));
					EndRound();
					return;
				}
			}
			// or by the score and the time limit, as in DM
			CCatch16Base::TickMatch();
		}

		void Tick() override
		{
			CCatch16Base::Tick();
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(CPlayer *pPlayer = Services().Player(ClientId))
					UpdateColors(pPlayer);
			}
		}
	};
}

static const CGameModeRegistration gs_Catch16({"catch16", "catch16", 0, false}, NewGameController<CGameControllerCatch16>);
