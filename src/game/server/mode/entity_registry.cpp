#include "entity_registry.h"

#include <base/dbg.h>
#include <base/log.h>

void CEntityRegistry::RegisterMapEntityFactory(FMapEntityFactory pfnFactory)
{
	dbg_assert(pfnFactory != nullptr, "Map entity factory must not be null");
	m_vMapEntityFactories.push_back(pfnFactory);
}

void CEntityRegistry::IgnoreMapEntityRange(int First, int Last)
{
	dbg_assert(First <= Last, "Invalid ignored map entity range");
	m_vIgnoredMapEntityRanges.push_back({First, Last});
}

bool CEntityRegistry::CreateMapEntity(IGameController &Controller, const CMapEntityContext &Context) const
{
	for(const CMapEntityRange &Range : m_vIgnoredMapEntityRanges)
	{
		if(Context.m_Index >= Range.m_First && Context.m_Index <= Range.m_Last)
			return false;
	}

	for(FMapEntityFactory pfnFactory : m_vMapEntityFactories)
	{
		if(pfnFactory(Controller, Context))
			return true;
	}

	log_warn("map", "unhandled entity index=%d x=%d y=%d layer=%d flags=%d number=%d", Context.m_Index, Context.m_X, Context.m_Y, Context.m_Layer, Context.m_Flags, Context.m_Number);
	return false;
}
