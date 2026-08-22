#ifndef GAME_SERVER_MODE_MATCH_LIFECYCLE_H
#define GAME_SERVER_MODE_MATCH_LIFECYCLE_H

class CMatchLifecycle
{
	int m_RoundStartTick;
	int m_GameOverTick = -1;
	int m_WarmupTicks = 0;
	int m_RoundCount = 0;
	bool m_SuddenDeath = false;

public:
	explicit CMatchLifecycle(int RoundStartTick) :
		m_RoundStartTick(RoundStartTick)
	{
	}

	bool IsWarmup() const { return m_WarmupTicks > 0; }
	bool IsGameOver() const { return m_GameOverTick >= 0; }
	bool IsRunning() const { return !IsWarmup() && !IsGameOver(); }
	bool IsSuddenDeath() const { return m_SuddenDeath; }
	int RoundStartTick() const { return m_RoundStartTick; }
	int WarmupTicks() const { return m_WarmupTicks; }
	int RoundCount() const { return m_RoundCount; }

	void SetWarmupTicks(int Ticks) { m_WarmupTicks = Ticks; }
	bool TickWarmup()
	{
		if(!IsWarmup())
			return false;
		return --m_WarmupTicks == 0;
	}

	bool EndRound(int Tick)
	{
		if(IsWarmup())
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

	void AdvanceRound() { ++m_RoundCount; }
	void BeginSuddenDeath() { m_SuddenDeath = true; }
};

#endif // GAME_SERVER_MODE_MATCH_LIFECYCLE_H
