#ifndef GAME_CLIENT_COMPONENTS_ENVELOPE_STATE_H
#define GAME_CLIENT_COMPONENTS_ENVELOPE_STATE_H

#include <game/client/component.h>
#include <game/map/render_interfaces.h>
#include <game/map/render_map.h>

#include <chrono>
#include <memory>
#include <vector>

class CEnvelopeState : public CComponent, public IEnvelopeEval
{
public:
	CEnvelopeState() :
		m_pEnvelopePoints(nullptr), m_pMap(nullptr) {}
	CEnvelopeState(IMap *pMap, bool OnlineOnly);
	void EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const override;

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

	std::chrono::nanoseconds OnlineTime() const;

	std::shared_ptr<CMapBasedEnvelopePointAccess> m_pEnvelopePoints;
	IMap *m_pMap;
	bool m_OnlineOnly;
	// Online envelopes only move with the game tick, and every layer that uses
	// one asks for it again each frame. The evaluations of one tick are kept
	// until the tick moves on.
	mutable std::chrono::nanoseconds m_CacheTime{};
	mutable std::vector<SCacheEntry> m_vCache;
};

#endif
