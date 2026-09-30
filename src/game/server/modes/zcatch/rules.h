#ifndef GAME_SERVER_MODES_ZCATCH_RULES_H
#define GAME_SERVER_MODES_ZCATCH_RULES_H

// The numbers of zCatch, as ddnet-insta has them.
namespace ZCatch
{
	// kills that count a winner needs, with as many players as a round needs to start
	inline constexpr int KILLS_TO_WIN = 4;

	// the kills that count a winner needs: 4, fewer only if a round starts with fewer than 5 players
	int KillsToWin(int MinPlayers);
	// the points for winning with that many kills that count
	int WinPoints(int Kills);

	enum class EColors
	{
		// from green to red with the kills, as on the teetime servers
		TEETIME,
		// from green to yellow, as on the savander servers
		SAVANDER,
	};
	// sv_zcatch_colors, teetime if it names no scheme
	EColors ParseColors(const char *pSetting);
	// packed HSL for the body of a player with that many kills that count
	int BodyColor(EColors Colors, int Kills);
}

#endif // GAME_SERVER_MODES_ZCATCH_RULES_H
