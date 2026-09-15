#include <base/dbg.h>

#include <game/map/document/structure.h>

#include <memory>
#include <variant>
#include <vector>

namespace map_document
{
	namespace
	{
		/**
		 * Puts an element of a list somewhere else in that list.
		 *
		 * The place is a place in the list without the element, which is the
		 * only reading that lets a panel hand over what somebody dragged
		 * without knowing whether it went up or down.
		 */
		template<typename T>
		void MoveWithin(std::vector<T> *pvList, size_t From, size_t To)
		{
			dbg_assert(From < pvList->size(), "Nothing to move at %d", (int)From);
			dbg_assert(To + 1 <= pvList->size(), "Nowhere to move to at %d", (int)To);
			T Moved = (*pvList)[From];
			pvList->erase(pvList->begin() + From);
			pvList->insert(pvList->begin() + To, std::move(Moved));
		}
	} // namespace

	bool IsPhysicsLayer(const CLayer &Layer)
	{
		if(!std::holds_alternative<CTileLayer>(Layer))
			return false;
		return std::get<CTileLayer>(Layer).m_Kind != ETileLayerKind::TILES;
	}

	std::optional<CLayerAddress> FindGameLayer(const CMapState &Map)
	{
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
		{
			for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
			{
				const CLayer *pLayer = Map.Layer(Group, Layer);
				if(std::holds_alternative<CTileLayer>(*pLayer) &&
					std::get<CTileLayer>(*pLayer).m_Kind == ETileLayerKind::GAME)
					return CLayerAddress{Group, Layer};
			}
		}
		return std::nullopt;
	}

	size_t AddGroup(CDocument &Doc, CGroup Group)
	{
		Doc.Edit().AddGroup(std::move(Group));
		return Doc.Edit().NumGroups() - 1;
	}

	void DeleteGroup(CDocument &Doc, size_t Group)
	{
		CMapState &Map = Doc.Edit();
		dbg_assert(Group < Map.NumGroups(), "Group out of range");
		Map.m_vpGroups.erase(Map.m_vpGroups.begin() + Group);
	}

	size_t MoveGroup(CDocument &Doc, size_t From, size_t To)
	{
		MoveWithin(&Doc.Edit().m_vpGroups, From, To);
		return To;
	}

	CLayerAddress AddLayer(CDocument &Doc, size_t Group, CLayer Layer)
	{
		CGroup Changed = *Doc.Edit().Group(Group);
		Changed.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Layer)));
		const size_t Index = Changed.m_vpLayers.size() - 1;
		Doc.Edit().ReplaceGroup(Group, std::move(Changed));
		return CLayerAddress{Group, Index};
	}

	void DeleteLayer(CDocument &Doc, const CLayerAddress &Layer)
	{
		CGroup Changed = *Doc.Edit().Group(Layer.m_Group);
		dbg_assert(Layer.m_Layer < Changed.m_vpLayers.size(), "Layer out of range");
		Changed.m_vpLayers.erase(Changed.m_vpLayers.begin() + Layer.m_Layer);
		Doc.Edit().ReplaceGroup(Layer.m_Group, std::move(Changed));
	}

	CLayerAddress MoveLayer(CDocument &Doc, const CLayerAddress &From, const CLayerAddress &To)
	{
		if(From.m_Group == To.m_Group)
		{
			CGroup Changed = *Doc.Edit().Group(From.m_Group);
			MoveWithin(&Changed.m_vpLayers, From.m_Layer, To.m_Layer);
			Doc.Edit().ReplaceGroup(From.m_Group, std::move(Changed));
			return To;
		}

		// Between two groups it is the same move, only that the two ends of it
		// are two nodes: the layer itself comes along as the node it is, so
		// what this costs is the two groups and nothing of what they hold.
		CGroup Left = *Doc.Edit().Group(From.m_Group);
		dbg_assert(From.m_Layer < Left.m_vpLayers.size(), "Layer out of range");
		const std::shared_ptr<const CLayer> pMoved = Left.m_vpLayers[From.m_Layer];
		Left.m_vpLayers.erase(Left.m_vpLayers.begin() + From.m_Layer);
		Doc.Edit().ReplaceGroup(From.m_Group, std::move(Left));

		CGroup Right = *Doc.Edit().Group(To.m_Group);
		dbg_assert(To.m_Layer <= Right.m_vpLayers.size(), "Nowhere to move to");
		Right.m_vpLayers.insert(Right.m_vpLayers.begin() + To.m_Layer, pMoved);
		Doc.Edit().ReplaceGroup(To.m_Group, std::move(Right));
		return To;
	}
} // namespace map_document
