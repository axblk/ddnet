#ifndef ENGINE_CLIENT_BACKEND_GPU_TIMESTAMP_H
#define ENGINE_CLIENT_BACKEND_GPU_TIMESTAMP_H

#include <engine/graphics.h>

#include <array>
#include <cstdint>

// Zone intervals per frame. Zones beyond this are dropped from the result.
constexpr uint32_t GPU_TIMESTAMP_MAX_INTERVALS = 128;

// One query pair for the whole frame, then one pair per zone interval.
constexpr uint32_t GPU_TIMESTAMP_QUERY_COUNT = 2 + GPU_TIMESTAMP_MAX_INTERVALS * 2;

using TGpuTimestampIntervalZones = std::array<uint8_t, GPU_TIMESTAMP_MAX_INTERVALS>;

/**
 * Assigns a frame's timestamp query intervals to render zones. Shared by the
 * Vulkan and WebGPU backends; it only hands out query offsets.
 */
class CGpuTimestampZones
{
	std::array<int32_t, IGraphics::MAX_GPU_RENDER_ZONES> m_aActiveIntervals{};
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

	// Returns false if the zone is already open or no interval is left; the
	// latter drops the zone from the result.
	bool Begin(IGraphics::CGpuRenderZone Zone, uint32_t &Query)
	{
		const size_t Index = static_cast<size_t>(Zone.Index());
		if(!Zone.IsValid() || Index >= m_aActiveIntervals.size() || m_aActiveIntervals[Index] >= 0)
			return false;
		if(m_IntervalCount >= GPU_TIMESTAMP_MAX_INTERVALS)
		{
			m_InvalidZoneMask |= 1U << Index;
			return false;
		}
		const uint32_t Interval = m_IntervalCount++;
		m_aIntervalZones[Interval] = static_cast<uint8_t>(Index);
		m_aActiveIntervals[Index] = static_cast<int32_t>(Interval);
		Query = QueryOffset(Interval, true);
		return true;
	}

	bool End(IGraphics::CGpuRenderZone Zone, uint32_t &Query)
	{
		const size_t Index = static_cast<size_t>(Zone.Index());
		if(!Zone.IsValid() || Index >= m_aActiveIntervals.size() || m_aActiveIntervals[Index] < 0)
			return false;
		Query = QueryOffset(static_cast<uint32_t>(m_aActiveIntervals[Index]), false);
		m_aActiveIntervals[Index] = -1;
		return true;
	}

	// Zones still open at frame end are closed so the query set resolves, but
	// are not reported.
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

	uint32_t ZoneMask() const
	{
		uint32_t Mask = 0;
		for(uint32_t Interval = 0; Interval < m_IntervalCount; ++Interval)
			Mask |= 1U << m_aIntervalZones[Interval];
		return Mask & ~m_InvalidZoneMask;
	}

	uint32_t WrittenQueryCount() const { return 2 + m_IntervalCount * 2; }
};

#endif
