#include "envelope_state.h"

#include <base/time.h>

#include <engine/map.h>

#include <chrono>

using namespace std::chrono_literals;

CEnvelopeState::CEnvelopeState(IMap *pMap, bool OnlineOnly) :
	m_pMap(pMap),
	m_OnlineOnly(OnlineOnly),
	m_Time(std::chrono::nanoseconds::zero())
{
	m_pEnvelopePoints = std::make_shared<CMapBasedEnvelopePointAccess>(m_pMap);
}

void CEnvelopeState::SetOnlineTime(const CGameState &State, const CGameTickInfo &Time, bool UsePredictedTime)
{
	if(m_OnlineOnly)
		m_Time = CalculateOnlineTime(State, Time, UsePredictedTime);
}

void CEnvelopeState::EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const
{
	using namespace std::chrono;

	if(!m_pMap)
		return;

	int EnvelopeStart, EnvelopeNum;
	m_pMap->GetType(MAPITEMTYPE_ENVELOPE, &EnvelopeStart, &EnvelopeNum);
	if(EnvelopeIndex < 0 || EnvelopeIndex >= EnvelopeNum)
		return;

	const CMapItemEnvelope *pItem = (CMapItemEnvelope *)m_pMap->GetItem(EnvelopeStart + EnvelopeIndex);
	if(pItem->m_Channels <= 0)
		return;
	Channels = std::min({Channels, (size_t)pItem->m_Channels, (size_t)CEnvPoint::MAX_CHANNELS});

	m_pEnvelopePoints->SetPointsRange(pItem->m_StartPoint, pItem->m_NumPoints);
	if(m_pEnvelopePoints->NumPoints() == 0)
		return;

	// offline rendering (like menu background) relies on local time
	const nanoseconds Time = m_OnlineOnly ? m_Time : time_get_nanoseconds();

	CRenderMap::RenderEvalEnvelope(m_pEnvelopePoints.get(), Time + milliseconds(TimeOffsetMillis), Result, Channels);
}
