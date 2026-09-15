#ifndef GAME_CLIENT_COMPONENTS_ENVELOPE_STATE_H
#define GAME_CLIENT_COMPONENTS_ENVELOPE_STATE_H

#include <game/client/component.h>
#include <game/client/game_state.h>
#include <game/map/render_interfaces.h>
#include <game/map/render_map.h>

#include <chrono>
#include <memory>
#include <vector>

class CEnvelopeState : public CComponent, public IEnvelopeEval
{
public:
	CEnvelopeState() :
		m_pEnvelopePoints(nullptr), m_pMap(nullptr), m_OnlineOnly(false), m_Time(std::chrono::nanoseconds::zero()) {}
	CEnvelopeState(IMap *pMap, bool OnlineOnly);
	void EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const override;
	void SetOnlineTime(const CGameState &State, const CGameTickInfo &Time, bool UsePredictedTime);

	static std::chrono::nanoseconds CalculateOnlineTime(const CGameState &State, const CGameTickInfo &Time, bool UsePredictedTime)
	{
		using namespace std::chrono;
		if(!State.HasGameInfo() || Time.m_GameTickSpeed <= 0)
			return nanoseconds::zero();

		const nanoseconds NanosPerTick = nanoseconds(1s) / static_cast<int64_t>(Time.m_GameTickSpeed);
		int EnvelopeTick;
		double TickRatio;
		if(UsePredictedTime)
		{
			EnvelopeTick = Time.m_PredGameTick - 1 - State.GameInfo().m_RoundStartTick;
			TickRatio = Time.m_PredIntraGameTick;
		}
		else
		{
			EnvelopeTick = Time.m_PrevGameTick - State.GameInfo().m_RoundStartTick;
			const int CurTick = Time.m_GameTick - State.GameInfo().m_RoundStartTick;
			TickRatio = (CurTick - EnvelopeTick) * static_cast<double>(Time.m_IntraGameTick);
		}
		return duration_cast<nanoseconds>(TickRatio * NanosPerTick) + EnvelopeTick * NanosPerTick;
	}

	int Sizeof() const override { return sizeof(*this); }

private:
	struct SCacheEntry
	{
		int m_TimeOffsetMillis;
		int m_EnvelopeIndex;
		size_t m_RequestedChannels;
		size_t m_ResultChannels;
		ColorRGBA m_Result;
	};

	std::shared_ptr<CMapBasedEnvelopePointAccess> m_pEnvelopePoints;
	IMap *m_pMap;
	bool m_OnlineOnly;
	std::chrono::nanoseconds m_Time;
	mutable std::vector<SCacheEntry> m_vCache;
};

#endif
