#ifndef GAME_SERVER_MODES_PVP_KILLING_SPREE_H
#define GAME_SERVER_MODES_PVP_KILLING_SPREE_H

/**
 * Kills in a row without dying, and what the server says about them.
 *
 * Every step of KillsPerStep kills is told to everybody, and so is the end of
 * a spree that got that far, as in ddnet-insta.
 */
class CKillingSpree
{
	int m_Kills = 0;

public:
	int Kills() const { return m_Kills; }

	/**
	 * Counts a kill.
	 *
	 * @param KillsPerStep The kills per step of a spree, 0 when sprees are off.
	 *
	 * @return Whether this kill makes a step everybody is told about.
	 */
	bool Add(int KillsPerStep);
	// ends the spree and returns how long it was
	int End();

	// "'name' is on a killing spree with 5 kills!", and so on with every step
	static void FormatStep(char *pBuf, int BufSize, const char *pName, int Kills, int KillsPerStep);
	// whether the end of a spree of that many kills is told
	static bool IsEndWorthTelling(int Kills, int KillsPerStep) { return KillsPerStep > 0 && Kills >= KillsPerStep; }
	static void FormatEnd(char *pBuf, int BufSize, const char *pVictim, int Kills, const char *pKiller);
};

// Kills close after each other, "'name' multi x3!", as ddnet-insta counts them in FNG.
class CMultiKill
{
	int m_Kills = 0;
	int m_LastTick = -1;

public:
	static constexpr int WINDOW_SECONDS = 5;

	/**
	 * Counts a kill.
	 *
	 * @return How many kills in a row this one makes, 1 for one on its own.
	 */
	int Add(int Tick, int TickSpeed);
	void Reset();
	static void Format(char *pBuf, int BufSize, const char *pName, int Kills);
};

#endif // GAME_SERVER_MODES_PVP_KILLING_SPREE_H
