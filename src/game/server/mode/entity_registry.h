#ifndef GAME_SERVER_MODE_ENTITY_REGISTRY_H
#define GAME_SERVER_MODE_ENTITY_REGISTRY_H

#include <vector>

class IGameController;

struct CMapEntityContext
{
	int m_Index;
	int m_X;
	int m_Y;
	int m_Layer;
	int m_Flags;
	bool m_Initial;
	int m_Number;
};

class CEntityRegistry
{
public:
	using FMapEntityFactory = bool (*)(IGameController &Controller, const CMapEntityContext &Context);

	void RegisterMapEntityFactory(FMapEntityFactory pfnFactory);
	void IgnoreMapEntityRange(int First, int Last);
	bool CreateMapEntity(IGameController &Controller, const CMapEntityContext &Context) const;

private:
	struct CMapEntityRange
	{
		int m_First;
		int m_Last;
	};

	std::vector<FMapEntityFactory> m_vMapEntityFactories;
	std::vector<CMapEntityRange> m_vIgnoredMapEntityRanges;
};

#endif // GAME_SERVER_MODE_ENTITY_REGISTRY_H
