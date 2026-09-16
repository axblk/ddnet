#include <base/dbg.h>

#include <game/map/document/structure.h>

#include <algorithm>
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

	namespace
	{
		/** A quad layer of the map being changed, to be written to. */
		CQuadLayer QuadsOf(CDocument &Doc, const CLayerAddress &Layer)
		{
			const CLayer *pLayer = Doc.Edit().Layer(Layer.m_Group, Layer.m_Layer);
			dbg_assert(std::holds_alternative<CQuadLayer>(*pLayer), "That layer holds no quads");
			return std::get<CQuadLayer>(*pLayer);
		}
	} // namespace

	CQuad MakeQuad(int CenterX, int CenterY, int Width, int Height)
	{
		CQuad Quad = {};
		Quad.m_PosEnv = -1;
		Quad.m_ColorEnv = -1;
		const int HalfWidth = Width / 2;
		const int HalfHeight = Height / 2;
		Quad.m_aPoints[0] = CPoint{i2fx(CenterX - HalfWidth), i2fx(CenterY - HalfHeight)};
		Quad.m_aPoints[1] = CPoint{i2fx(CenterX + HalfWidth), i2fx(CenterY - HalfHeight)};
		Quad.m_aPoints[2] = CPoint{i2fx(CenterX - HalfWidth), i2fx(CenterY + HalfHeight)};
		Quad.m_aPoints[3] = CPoint{i2fx(CenterX + HalfWidth), i2fx(CenterY + HalfHeight)};
		Quad.m_aPoints[4] = CPoint{i2fx(CenterX), i2fx(CenterY)};
		Quad.m_aTexcoords[0] = CPoint{i2fx(0), i2fx(0)};
		Quad.m_aTexcoords[1] = CPoint{i2fx(1), i2fx(0)};
		Quad.m_aTexcoords[2] = CPoint{i2fx(0), i2fx(1)};
		Quad.m_aTexcoords[3] = CPoint{i2fx(1), i2fx(1)};
		for(CColor &Color : Quad.m_aColors)
			Color = CColor{255, 255, 255, 255};
		return Quad;
	}

	size_t AddQuad(CDocument &Doc, const CLayerAddress &Layer, const CQuad &Quad)
	{
		CQuadLayer Changed = QuadsOf(Doc, Layer);
		Changed.m_Quads.Mutable().push_back(Quad);
		const size_t Index = Changed.m_Quads.Size() - 1;
		Doc.Edit().ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Changed));
		return Index;
	}

	void DeleteQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad)
	{
		CQuadLayer Changed = QuadsOf(Doc, Layer);
		dbg_assert(Quad < Changed.m_Quads.Size(), "Quad out of range");
		std::vector<CQuad> &vQuads = Changed.m_Quads.Mutable();
		vQuads.erase(vQuads.begin() + Quad);
		Doc.Edit().ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Changed));
	}

	void SetQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad, const CQuad &Changed)
	{
		CQuadLayer Layers = QuadsOf(Doc, Layer);
		dbg_assert(Quad < Layers.m_Quads.Size(), "Quad out of range");
		Layers.m_Quads.Mutable()[Quad] = Changed;
		Doc.Edit().ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Layers));
	}

	size_t AddImage(CDocument &Doc, CImage Image)
	{
		CMapState &Map = Doc.Edit();
		Map.AddImage(std::move(Image));
		return Map.NumImages() - 1;
	}

	void DeleteImage(CDocument &Doc, size_t Image)
	{
		CMapState &Map = Doc.Edit();
		dbg_assert(Image < Map.NumImages(), "Image out of range");
		Map.m_vpImages.erase(Map.m_vpImages.begin() + Image);

		// A layer names a picture by its place, so the places have to be read
		// again - the same sum as for an envelope that is taken away.
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
		{
			for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
			{
				CLayer Changed = *Map.Layer(Group, Layer);
				int *pBound = nullptr;
				if(CTileLayer *pTiles = std::get_if<CTileLayer>(&Changed); pTiles != nullptr)
					pBound = &pTiles->m_Image;
				else if(CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Changed); pQuads != nullptr)
					pBound = &pQuads->m_Image;
				if(pBound == nullptr)
					continue;
				const int Was = *pBound;
				if(*pBound == (int)Image)
					*pBound = -1;
				else if(*pBound > (int)Image)
					--*pBound;
				// A layer that used another picture is the node it was, which
				// is what keeps this from costing the whole map.
				if(*pBound != Was)
					Map.ReplaceLayer(Group, Layer, std::move(Changed));
			}
		}
	}

	void SetImage(CDocument &Doc, size_t Index, CImage Changed)
	{
		Doc.Edit().ReplaceImage(Index, std::move(Changed));
	}

	size_t AddEnvelope(CDocument &Doc, CEnvelope Envelope)
	{
		Doc.Edit().AddEnvelope(std::move(Envelope));
		return Doc.Edit().NumEnvelopes() - 1;
	}

	void DeleteEnvelope(CDocument &Doc, size_t Envelope)
	{
		CMapState &Map = Doc.Edit();
		dbg_assert(Envelope < Map.NumEnvelopes(), "Envelope out of range");
		Map.m_vpEnvelopes.erase(Map.m_vpEnvelopes.begin() + Envelope);

		// Whatever was bound to an envelope named it by its place, so the
		// places have to be read again: what pointed past the one that is
		// gone comes down one, and what pointed at it points at nothing.
		const auto Rebind = [Envelope](int &Bound) {
			if(Bound == (int)Envelope)
				Bound = -1;
			else if(Bound > (int)Envelope)
				--Bound;
		};
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
		{
			for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
			{
				CLayer Changed = *Map.Layer(Group, Layer);
				bool Touched = false;
				if(CTileLayer *pTiles = std::get_if<CTileLayer>(&Changed); pTiles != nullptr)
				{
					const int Was = pTiles->m_ColorEnvelope;
					Rebind(pTiles->m_ColorEnvelope);
					Touched = pTiles->m_ColorEnvelope != Was;
				}
				else if(CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Changed); pQuads != nullptr)
				{
					for(size_t Index = 0; Index < pQuads->m_Quads.Size(); ++Index)
					{
						CQuad Quad = pQuads->m_Quads[Index];
						const int WasColor = Quad.m_ColorEnv;
						const int WasPosition = Quad.m_PosEnv;
						Rebind(Quad.m_ColorEnv);
						Rebind(Quad.m_PosEnv);
						if(Quad.m_ColorEnv == WasColor && Quad.m_PosEnv == WasPosition)
							continue;
						pQuads->m_Quads.Mutable()[Index] = Quad;
						Touched = true;
					}
				}
				else if(CSoundLayer *pSounds = std::get_if<CSoundLayer>(&Changed); pSounds != nullptr)
				{
					for(size_t Index = 0; Index < pSounds->m_Sources.Size(); ++Index)
					{
						CSoundSource Source = pSounds->m_Sources[Index];
						const int WasSound = Source.m_SoundEnv;
						const int WasPosition = Source.m_PosEnv;
						Rebind(Source.m_SoundEnv);
						Rebind(Source.m_PosEnv);
						if(Source.m_SoundEnv == WasSound && Source.m_PosEnv == WasPosition)
							continue;
						pSounds->m_Sources.Mutable()[Index] = Source;
						Touched = true;
					}
				}
				// A layer that was bound to nothing is left as the node it is,
				// which is the whole point of the design: taking an envelope
				// out of a large map costs the layers that used it.
				if(Touched)
					Map.ReplaceLayer(Group, Layer, std::move(Changed));
			}
		}
	}

	namespace
	{
		/** Where a point of that time belongs, after any point at the same time. */
		size_t PlaceFor(const CSharedList<CEnvPoint_runtime> &Points, CFixedTime Time)
		{
			size_t Place = 0;
			while(Place < Points.Size() && !(Time < Points[Place].m_Time))
				++Place;
			return Place;
		}
	} // namespace

	size_t AddEnvelopePoint(CDocument &Doc, size_t Envelope, const CEnvPoint_runtime &Point)
	{
		CEnvelope Changed = *Doc.Edit().Envelope(Envelope);
		const size_t Place = PlaceFor(Changed.m_Points, Point.m_Time);
		std::vector<CEnvPoint_runtime> &vPoints = Changed.m_Points.Mutable();
		vPoints.insert(vPoints.begin() + Place, Point);
		Doc.Edit().ReplaceEnvelope(Envelope, std::move(Changed));
		return Place;
	}

	void DeleteEnvelopePoint(CDocument &Doc, size_t Envelope, size_t Point)
	{
		CEnvelope Changed = *Doc.Edit().Envelope(Envelope);
		dbg_assert(Point < Changed.m_Points.Size(), "Envelope point out of range");
		std::vector<CEnvPoint_runtime> &vPoints = Changed.m_Points.Mutable();
		vPoints.erase(vPoints.begin() + Point);
		Doc.Edit().ReplaceEnvelope(Envelope, std::move(Changed));
	}

	size_t SetEnvelopePoint(CDocument &Doc, size_t Envelope, size_t Point, const CEnvPoint_runtime &Changed)
	{
		CEnvelope Envelopes = *Doc.Edit().Envelope(Envelope);
		dbg_assert(Point < Envelopes.m_Points.Size(), "Envelope point out of range");
		std::vector<CEnvPoint_runtime> &vPoints = Envelopes.m_Points.Mutable();
		vPoints.erase(vPoints.begin() + Point);
		// Taken out first and put back where its new time belongs, so that
		// dragging a point past its neighbour is the ordinary case rather
		// than a list that is quietly out of order.
		size_t Place = 0;
		while(Place < vPoints.size() && !(Changed.m_Time < vPoints[Place].m_Time))
			++Place;
		vPoints.insert(vPoints.begin() + Place, Changed);
		Doc.Edit().ReplaceEnvelope(Envelope, std::move(Envelopes));
		return Place;
	}
} // namespace map_document
