#include "envelope_state.h"

#include <base/time.h>

#include <game/client/gameclient.h>
#include <game/localization.h>

#include <chrono>

using namespace std::chrono_literals;

CEnvelopeState::CEnvelopeState(IMap *pMap, bool OnlineOnly) :
	m_pMap(pMap)
{
	m_pEnvelopePoints = std::make_shared<CMapBasedEnvelopePointAccess>(m_pMap);
	m_OnlineOnly = OnlineOnly;
}

std::chrono::nanoseconds CEnvelopeState::OnlineTime() const
{
	using namespace std::chrono;

	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return nanoseconds::zero();

	static const nanoseconds s_NanosPerTick = nanoseconds(1s) / static_cast<int64_t>(Client()->GameTickSpeed());

	// get the lerp of the current tick and prev
	int EnvelopeTick;
	double TickRatio;
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK || !g_Config.m_ClPredict ||
		(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW))
	{
		EnvelopeTick = Client()->PrevGameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick;
		const int CurTick = Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick;
		TickRatio = mix<double>(0, CurTick - EnvelopeTick, (double)Client()->IntraGameTick(g_Config.m_ClDummy));
	}
	else
	{
		EnvelopeTick = Client()->PredGameTick(g_Config.m_ClDummy) - 1 - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick;
		TickRatio = (double)Client()->PredIntraGameTick(g_Config.m_ClDummy);
	}
	return duration_cast<nanoseconds>(TickRatio * s_NanosPerTick) + EnvelopeTick * s_NanosPerTick;
}

void CEnvelopeState::EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const
{
	using namespace std::chrono;

	if(!m_pMap)
		return;

	// offline rendering (like menu background) relies on local time
	const nanoseconds Time = m_OnlineOnly ? OnlineTime() : time_get_nanoseconds();
	if(m_OnlineOnly)
	{
		if(Time != m_CacheTime)
		{
			m_CacheTime = Time;
			m_vCache.clear();
		}
		if(EnvelopeIndex >= 0)
		{
			for(const SCacheEntry &Entry : m_vCache)
			{
				if(Entry.m_TimeOffsetMillis == TimeOffsetMillis && Entry.m_EnvelopeIndex == EnvelopeIndex && Entry.m_RequestedChannels == Channels)
				{
					if(Entry.m_ResultChannels >= 1)
						Result.r = Entry.m_Result.r;
					if(Entry.m_ResultChannels >= 2)
						Result.g = Entry.m_Result.g;
					if(Entry.m_ResultChannels >= 3)
						Result.b = Entry.m_Result.b;
					if(Entry.m_ResultChannels >= 4)
						Result.a = Entry.m_Result.a;
					return;
				}
			}
		}
	}

	int EnvelopeStart, EnvelopeNum;
	m_pMap->GetType(MAPITEMTYPE_ENVELOPE, &EnvelopeStart, &EnvelopeNum);
	if(EnvelopeIndex < 0 || EnvelopeIndex >= EnvelopeNum)
		return;

	const CMapItemEnvelope *pItem = (CMapItemEnvelope *)m_pMap->GetItem(EnvelopeStart + EnvelopeIndex);
	if(pItem->m_Channels <= 0)
		return;
	const size_t RequestedChannels = Channels;
	Channels = std::min({Channels, (size_t)pItem->m_Channels, (size_t)CEnvPoint::MAX_CHANNELS});

	m_pEnvelopePoints->SetPointsRange(pItem->m_StartPoint, pItem->m_NumPoints);
	if(m_pEnvelopePoints->NumPoints() == 0)
		return;

	CRenderMap::RenderEvalEnvelope(m_pEnvelopePoints.get(), Time + milliseconds(TimeOffsetMillis), Result, Channels);
	if(m_OnlineOnly)
		m_vCache.push_back({TimeOffsetMillis, EnvelopeIndex, RequestedChannels, Channels, Result});
}
