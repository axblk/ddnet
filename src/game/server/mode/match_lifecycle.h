#ifndef GAME_SERVER_MODE_MATCH_LIFECYCLE_H
#define GAME_SERVER_MODE_MATCH_LIFECYCLE_H

class CMatchLifecycle
{
	int m_RoundStartTick;
	int m_GameOverTick = -1;
	// negative while waiting for players, which only the mode ends
	int m_WarmupTicks = 0;
	int m_RoundCount = 0;
	bool m_SuddenDeath = false;

public:
	explicit CMatchLifecycle(int RoundStartTick) :
		m_RoundStartTick(RoundStartTick)
	{
	}

	bool IsWarmup() const { return m_WarmupTicks != 0; }
	bool IsWaitingForPlayers() const { return m_WarmupTicks < 0; }
	bool IsGameOver() const { return m_GameOverTick >= 0; }
	bool IsRunning() const { return !IsWarmup() && !IsGameOver(); }
	bool IsSuddenDeath() const { return m_SuddenDeath; }
	int RoundStartTick() const { return m_RoundStartTick; }
	// the ticks left of a warmup that has an end, 0 otherwise
	int WarmupTicks() const { return m_WarmupTicks > 0 ? m_WarmupTicks : 0; }
	int RoundCount() const { return m_RoundCount; }
	int GameOverTick() const { return m_GameOverTick; }

	void SetWarmupTicks(int Ticks) { m_WarmupTicks = Ticks; }
	// a warmup without an end, until there are enough players for a match
	void WaitForPlayers() { m_WarmupTicks = -1; }
	bool TickWarmup()
	{
		if(m_WarmupTicks <= 0)
			return false;
		return --m_WarmupTicks == 0;
	}

	bool EndRound(int Tick)
	{
		if(IsWarmup() || IsGameOver())
			return false;
		m_GameOverTick = Tick;
		m_SuddenDeath = false;
		return true;
	}

	void StartRound(int Tick)
	{
		m_RoundStartTick = Tick;
		m_GameOverTick = -1;
		m_SuddenDeath = false;
	}

	bool ShouldRestartRound(int Tick, int RestartDelayTicks) const
	{
		return IsGameOver() && Tick > m_GameOverTick + RestartDelayTicks;
	}

	// the clock of the round runs from here, a mode with rounds inside a match sets it for each of them
	void SetRoundStartTick(int Tick) { m_RoundStartTick = Tick; }
	void AdvanceRound() { ++m_RoundCount; }
	void BeginSuddenDeath() { m_SuddenDeath = true; }
};

#endif // GAME_SERVER_MODE_MATCH_LIFECYCLE_H
