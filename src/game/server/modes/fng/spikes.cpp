// The rules and the numbers follow the FNG modes of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "spikes.h"

#include <base/math.h>

#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <algorithm>

Fng::ESpike Fng::SpikeOfTile(int Index)
{
	switch(Index)
	{
	case 7: return ESpike::GOLD;
	case 8: return ESpike::NORMAL;
	case 9: return ESpike::RED;
	case 10: return ESpike::BLUE;
	case 14: return ESpike::GREEN;
	case 15: return ESpike::PURPLE;
	default: return ESpike::NONE;
	}
}

Fng::ESpike Fng::TouchedSpike(vec2 Pos, float ProximityRadius, int Width, int Height, const std::function<ESpike(int x, int y)> &TileAt)
{
	const float Proximity = ProximityRadius / 3.0f;
	const int aX[] = {std::clamp(round_to_int(Pos.x - Proximity) / 32, 0, Width - 1), std::clamp(round_to_int(Pos.x + Proximity) / 32, 0, Width - 1)};
	const int aY[] = {std::clamp(round_to_int(Pos.y - Proximity) / 32, 0, Height - 1), std::clamp(round_to_int(Pos.y + Proximity) / 32, 0, Height - 1)};
	const vec2 Center(std::clamp(round_to_int(Pos.x), 0, (Width - 1) * 32), std::clamp(round_to_int(Pos.y), 0, (Height - 1) * 32));
	ESpike Nearest = ESpike::NONE;
	float NearestDistance = 0.0f;
	for(const int X : aX)
	{
		for(const int Y : aY)
		{
			const ESpike Spike = TileAt(X, Y);
			if(Spike == ESpike::NONE)
				continue;
			const float Distance = distance(vec2(X * 32 + 16, Y * 32 + 16), Center);
			if(Nearest == ESpike::NONE || Distance < NearestDistance)
			{
				Nearest = Spike;
				NearestDistance = Distance;
			}
		}
	}
	return Nearest;
}

Fng::CSpikePoints Fng::SpikePoints(ESpike Spike, int KillerTeam, bool TeamPlay)
{
	switch(Spike)
	{
	case ESpike::NONE: return {};
	case ESpike::NORMAL: return {g_Config.m_SvPlayerScoreSpikeNormal, g_Config.m_SvTeamScoreSpikeNormal};
	case ESpike::GOLD: return {g_Config.m_SvPlayerScoreSpikeGold, g_Config.m_SvTeamScoreSpikeGold};
	case ESpike::GREEN: return {g_Config.m_SvPlayerScoreSpikeGreen, g_Config.m_SvTeamScoreSpikeGreen};
	case ESpike::PURPLE: return {g_Config.m_SvPlayerScoreSpikePurple, g_Config.m_SvTeamScoreSpikePurple};
	case ESpike::RED:
	case ESpike::BLUE:
		break;
	}
	const bool Wrong = TeamPlay && KillerTeam != (Spike == ESpike::RED ? TEAM_RED : TEAM_BLUE);
	if(Wrong)
		return {-g_Config.m_SvPlayerScoreSpikeTeam, 0, true};
	return {g_Config.m_SvPlayerScoreSpikeTeam, g_Config.m_SvTeamScoreSpikeTeam};
}

vec2 Fng::HammerForce(vec2 From, vec2 To, bool Melt)
{
	const vec2 Direction = length(To - From) > 0.0f ? normalize(To - From) : vec2(0.0f, -1.0f);
	const vec2 Push = vec2(0.0f, -1.0f) + normalize(Direction + vec2(0.0f, -1.1f)) * 10.0f;
	if(Melt)
		return vec2(Push.x * g_Config.m_SvMeltHammerScaleX * 0.01f, Push.y * g_Config.m_SvMeltHammerScaleY * 0.01f);
	return vec2(Push.x * g_Config.m_SvHammerScaleX * 0.01f, Push.y * g_Config.m_SvHammerScaleY * 0.01f);
}

bool Fng::IsPredictedHammer()
{
	// the scales the DDNet client has built in
	return g_Config.m_SvFngHammer && g_Config.m_SvHammerScaleX == 320 && g_Config.m_SvHammerScaleY == 120 && g_Config.m_SvMeltHammerScaleX == 50 && g_Config.m_SvMeltHammerScaleY == 50;
}

int Fng::MeltFreeze(int FreezeTicks, int TickSpeed, bool *pThawed)
{
	// the freeze thaws in the tick after the last one
	static constexpr int THAWING_TICKS = 2;
	const int Left = FreezeTicks - 3 * TickSpeed;
	*pThawed = Left < THAWING_TICKS;
	return std::max(Left, THAWING_TICKS);
}
