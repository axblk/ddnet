#ifndef ENGINE_CLIENT_BACKEND_GPU_TIMESTAMP_H
#define ENGINE_CLIENT_BACKEND_GPU_TIMESTAMP_H

#include <engine/graphics.h>

#include <array>
#include <cstdint>

/**
 * How many render zones a single frame can time. A zone that begins after the
 * frame has run out of intervals is dropped from the result rather than
 * reported with someone else's numbers.
 */
constexpr uint32_t GPU_TIMESTAMP_MAX_INTERVALS = 128;

/**
 * The queries a frame writes: one pair for the frame as a whole, then one pair
 * per zone interval. Both timing backends use this layout, so the offsets are
 * worked out here and neither has to know how the other counts.
 */
constexpr uint32_t GPU_TIMESTAMP_QUERY_COUNT = 2 + GPU_TIMESTAMP_MAX_INTERVALS * 2;

using TGpuTimestampIntervalZones = std::array<IGraphics::EGpuRenderZone, GPU_TIMESTAMP_MAX_INTERVALS>;

/**
 * Which zone owns which interval of a frame's timestamp queries. The Vulkan and
 * the WebGPU backend hand out the same intervals in the same order and drop a
 * zone from the result under the same conditions; what differs between them is
 * only how a query is written, which is why this hands back query offsets and
 * writes nothing itself.
 */
class CGpuTimestampZones
{
	std::array<int32_t, IGraphics::GPU_RENDER_ZONE_COUNT> m_aActiveIntervals{};
	TGpuTimestampIntervalZones m_aIntervalZones{};
	uint32_t m_IntervalCount = 0;
	uint32_t m_InvalidZoneMask = 0;

	static uint32_t QueryOffset(uint32_t Interval, bool Begin)
	{
		return 2 + Interval * 2 + (Begin ? 0 : 1);
	}

public:
	CGpuTimestampZones()
	{
		Reset();
	}

	void Reset()
	{
		m_aActiveIntervals.fill(-1);
		m_IntervalCount = 0;
		m_InvalidZoneMask = 0;
	}

	/**
	 * Opens a zone and says at which query offset its first timestamp goes.
	 * A zone that is already open, or one the frame has no interval left for,
	 * writes nothing - the second case also drops the zone from the result,
	 * because half of an interval is not a measurement.
	 */
	bool Begin(IGraphics::EGpuRenderZone Zone, uint32_t &Query)
	{
		const size_t Index = static_cast<size_t>(Zone);
		if(Index >= m_aActiveIntervals.size() || m_aActiveIntervals[Index] >= 0)
			return false;
		if(m_IntervalCount >= GPU_TIMESTAMP_MAX_INTERVALS)
		{
			m_InvalidZoneMask |= 1U << Index;
			return false;
		}
		const uint32_t Interval = m_IntervalCount++;
		m_aIntervalZones[Interval] = Zone;
		m_aActiveIntervals[Index] = static_cast<int32_t>(Interval);
		Query = QueryOffset(Interval, true);
		return true;
	}

	/**
	 * Closes a zone and says at which query offset its second timestamp goes.
	 * A zone that was never opened writes nothing.
	 */
	bool End(IGraphics::EGpuRenderZone Zone, uint32_t &Query)
	{
		const size_t Index = static_cast<size_t>(Zone);
		if(Index >= m_aActiveIntervals.size() || m_aActiveIntervals[Index] < 0)
			return false;
		Query = QueryOffset(static_cast<uint32_t>(m_aActiveIntervals[Index]), false);
		m_aActiveIntervals[Index] = -1;
		return true;
	}

	/**
	 * A zone still open at the end of the frame gets its closing query written
	 * anyway - the query set is resolved as a whole - but is not reported: its
	 * interval is not what the caller measured.
	 */
	template<typename TWriteQuery>
	void CloseOpenZones(TWriteQuery &&WriteQuery)
	{
		for(size_t Zone = 0; Zone < m_aActiveIntervals.size(); ++Zone)
		{
			if(m_aActiveIntervals[Zone] < 0)
				continue;
			WriteQuery(QueryOffset(static_cast<uint32_t>(m_aActiveIntervals[Zone]), false));
			m_aActiveIntervals[Zone] = -1;
			m_InvalidZoneMask |= 1U << Zone;
		}
	}

	uint32_t IntervalCount() const { return m_IntervalCount; }
	const TGpuTimestampIntervalZones &IntervalZones() const { return m_aIntervalZones; }

	/**
	 * The zones this frame measured whole, one bit each.
	 */
	uint32_t ZoneMask() const
	{
		uint32_t Mask = 0;
		for(uint32_t Interval = 0; Interval < m_IntervalCount; ++Interval)
			Mask |= 1U << static_cast<size_t>(m_aIntervalZones[Interval]);
		return Mask & ~m_InvalidZoneMask;
	}

	/**
	 * How many of the frame's queries were written, and so how many have to be
	 * resolved and read back.
	 */
	uint32_t WrittenQueryCount() const { return 2 + m_IntervalCount * 2; }
};

#endif
