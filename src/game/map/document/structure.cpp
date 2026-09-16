#include <base/dbg.h>
#include <base/math.h>

#include <game/map/document/structure.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

	namespace
	{
		/**
		 * The nearest multiple of `Step`, rounding half away from zero.
		 *
		 * Integer division truncates towards zero, so a point at -3 with a
		 * step of 8 would land on 0 rather than on -8 if it were left to it.
		 */
		int Nearest(int Value, int Step)
		{
			const int Half = Step / 2;
			return Value >= 0 ? (Value + Half) / Step * Step : -((-Value + Half) / Step * Step);
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

	void ResizeLayer(CDocument &Doc, const CLayerAddress &Layer, int Width, int Height)
	{
		dbg_assert(Width > 0 && Height > 0, "A layer has to be at least one tile: %dx%d", Width, Height);
		CMapState &Map = Doc.Edit();
		const CTileLayer *pLayer = std::get_if<CTileLayer>(Map.Layer(Layer.m_Group, Layer.m_Layer));
		dbg_assert(pLayer != nullptr, "Layer %d of group %d holds no tiles", (int)Layer.m_Layer, (int)Layer.m_Group);
		if(pLayer->Width() == Width && pLayer->Height() == Height)
			return;

		if(pLayer->m_Kind == ETileLayerKind::TILES)
		{
			CTileLayer Changed = *pLayer;
			Changed.Resize(Width, Height);
			Map.ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Changed));
			return;
		}

		// A physics layer is not resized by itself - the game plays one size,
		// and that is the size all of them are.
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
		{
			for(size_t Index = 0; Index < Map.NumLayers(Group); ++Index)
			{
				const CLayer *pOther = Map.Layer(Group, Index);
				if(!IsPhysicsLayer(*pOther))
					continue;
				CTileLayer Changed = std::get<CTileLayer>(*pOther);
				if(Changed.Width() == Width && Changed.Height() == Height)
					continue;
				Changed.Resize(Width, Height);
				Map.ReplaceLayer(Group, Index, std::move(Changed));
			}
		}
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

	namespace
	{
		/** A sound layer of the map being changed, to be written to. */
		CSoundLayer SoundsOf(CDocument &Doc, const CLayerAddress &Layer)
		{
			const CLayer *pLayer = Doc.Edit().Layer(Layer.m_Group, Layer.m_Layer);
			dbg_assert(std::holds_alternative<CSoundLayer>(*pLayer), "That layer holds no sounds");
			return std::get<CSoundLayer>(*pLayer);
		}
	} // namespace

	CSoundSource MakeSoundSource(int X, int Y, int Radius)
	{
		CSoundSource Source = {};
		Source.m_Position = CPoint{i2fx(X), i2fx(Y)};
		Source.m_Loop = 1;
		Source.m_Pan = 0;
		Source.m_TimeDelay = 0;
		Source.m_Falloff = 0;
		Source.m_PosEnv = -1;
		Source.m_PosEnvOffset = 0;
		Source.m_SoundEnv = -1;
		Source.m_SoundEnvOffset = 0;
		Source.m_Shape.m_Type = CSoundShape::SHAPE_CIRCLE;
		Source.m_Shape.m_Circle.m_Radius = Radius;
		return Source;
	}

	size_t AddSoundSource(CDocument &Doc, const CLayerAddress &Layer, const CSoundSource &Source)
	{
		CSoundLayer Sounds = SoundsOf(Doc, Layer);
		Sounds.m_Sources.Mutable().push_back(Source);
		const size_t Index = Sounds.m_Sources.Size() - 1;
		Doc.Edit().ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Sounds));
		return Index;
	}

	void DeleteSoundSource(CDocument &Doc, const CLayerAddress &Layer, size_t Source)
	{
		CSoundLayer Sounds = SoundsOf(Doc, Layer);
		dbg_assert(Source < Sounds.m_Sources.Size(), "Sound source out of range");
		std::vector<CSoundSource> &vSources = Sounds.m_Sources.Mutable();
		vSources.erase(vSources.begin() + Source);
		Doc.Edit().ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Sounds));
	}

	void SetSoundSource(CDocument &Doc, const CLayerAddress &Layer, size_t Source, const CSoundSource &Changed)
	{
		CSoundLayer Sounds = SoundsOf(Doc, Layer);
		dbg_assert(Source < Sounds.m_Sources.Size(), "Sound source out of range");
		Sounds.m_Sources.Mutable()[Source] = Changed;
		Doc.Edit().ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Sounds));
	}

	bool ShapeQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad, EQuadShape Shape, int Grid)
	{
		const CQuadLayer *pQuads = std::get_if<CQuadLayer>(Doc.Edit().Layer(Layer.m_Group, Layer.m_Layer));
		dbg_assert(pQuads != nullptr, "Layer %d of group %d holds no quads", (int)Layer.m_Layer, (int)Layer.m_Group);
		dbg_assert(Quad < pQuads->m_Quads.Size(), "Quad out of range");
		CQuad Changed = pQuads->m_Quads[Quad];

		// The rectangle the four corners span. A quad that was dragged out of
		// shape is inside it; a quad that is already a rectangle is it.
		int Left = Changed.m_aPoints[0].x;
		int Right = Changed.m_aPoints[0].x;
		int Top = Changed.m_aPoints[0].y;
		int Bottom = Changed.m_aPoints[0].y;
		for(int Corner = 1; Corner < 4; ++Corner)
		{
			Left = std::min(Left, Changed.m_aPoints[Corner].x);
			Right = std::max(Right, Changed.m_aPoints[Corner].x);
			Top = std::min(Top, Changed.m_aPoints[Corner].y);
			Bottom = std::max(Bottom, Changed.m_aPoints[Corner].y);
		}

		if(Shape == EQuadShape::ASPECT)
		{
			if(pQuads->m_Image < 0 || (size_t)pQuads->m_Image >= Doc.Edit().NumImages())
				return false;
			const CImage *pImage = Doc.Edit().Image(pQuads->m_Image);
			if(pImage->m_Width <= 0 || pImage->m_Height <= 0)
				return false;
			// As wide as it is; the height follows from the picture. The
			// top-left corner stays where it is, so the quad grows downwards
			// rather than out of both sides.
			Bottom = Top + (int)((int64_t)(Right - Left) * pImage->m_Height / pImage->m_Width);
		}

		if(Shape == EQuadShape::SQUARE || Shape == EQuadShape::ASPECT)
		{
			Changed.m_aPoints[0] = CPoint{Left, Top};
			Changed.m_aPoints[1] = CPoint{Right, Top};
			Changed.m_aPoints[2] = CPoint{Left, Bottom};
			Changed.m_aPoints[3] = CPoint{Right, Bottom};
		}
		else if(Shape == EQuadShape::CENTER_PIVOT)
		{
			Changed.m_aPoints[4] = CPoint{Left + (Right - Left) / 2, Top + (Bottom - Top) / 2};
		}
		else
		{
			dbg_assert(Grid > 0, "A grid is at least one unit wide: %d", Grid);
			// Every corner to the nearest crossing, the pivot with them - the
			// editor in the client leaves the first corner where it is, which
			// is a slip rather than a rule.
			const int Step = i2fx(Grid);
			for(CPoint &Point : Changed.m_aPoints)
			{
				Point.x = Nearest(Point.x, Step);
				Point.y = Nearest(Point.y, Step);
			}
		}

		SetQuad(Doc, Layer, Quad, Changed);
		return true;
	}

	namespace
	{
		/** Twice the area of a triangle, signed - the sign says which way round. */
		float TwiceArea(vec2 A, vec2 B, vec2 C)
		{
			return (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
		}

		bool InTriangle(vec2 Point, vec2 A, vec2 B, vec2 C)
		{
			// Compared against the whole triangle's own winding, so a triangle
			// given either way round answers the same.
			const float Whole = TwiceArea(A, B, C);
			if(Whole == 0.0f)
				return false;
			const float One = TwiceArea(Point, B, C) / Whole;
			const float Two = TwiceArea(A, Point, C) / Whole;
			const float Three = TwiceArea(A, B, Point) / Whole;
			return One >= 0.0f && Two >= 0.0f && Three >= 0.0f;
		}

		vec2 CornerOf(const CQuad &Quad, size_t Corner)
		{
			return vec2(fx2f(Quad.m_aPoints[Corner].x), fx2f(Quad.m_aPoints[Corner].y));
		}
	} // namespace

	bool PointInQuad(const CQuad &Quad, vec2 Point)
	{
		// A quad is two triangles about the corner-0-to-corner-3 diagonal.
		const vec2 TopLeft = CornerOf(Quad, 0);
		const vec2 BottomRight = CornerOf(Quad, 3);
		return InTriangle(Point, TopLeft, CornerOf(Quad, 1), BottomRight) ||
		       InTriangle(Point, TopLeft, BottomRight, CornerOf(Quad, 2));
	}

	size_t CarveQuad(CDocument &Doc, const CLayerAddress &Layer, size_t Quad, const vec2 *apPoints)
	{
		CMapState &Map = Doc.Edit();
		CLayer Changed = *Map.Layer(Layer.m_Group, Layer.m_Layer);
		CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Changed);
		dbg_assert(pQuads != nullptr, "Layer holds no quads");
		dbg_assert(Quad < pQuads->m_Quads.Size(), "Quad out of range");
		const CQuad &Cut = pQuads->m_Quads[Quad];

		// The four places come in as a ring, because that is how somebody
		// clicks them; the file keeps corners as two rows. A ring that folds
		// over itself is the same four places wound the other way, so it is
		// unfolded rather than refused.
		vec2 aRing[4] = {apPoints[0], apPoints[1], apPoints[2], apPoints[3]};
		if(InTriangle(aRing[3], aRing[0], aRing[1], aRing[2]) ||
			InTriangle(aRing[1], aRing[0], aRing[2], aRing[3]))
		{
			std::swap(aRing[0], aRing[3]);
			std::swap(aRing[1], aRing[2]);
		}
		std::swap(aRing[2], aRing[3]);

		CQuad Made = Cut;
		const vec2 TopLeft = CornerOf(Cut, 0);
		const vec2 BottomRight = CornerOf(Cut, 3);
		for(size_t Corner = 0; Corner < 4; ++Corner)
		{
			// Which half of the old quad the place falls in decides which
			// third corner it is measured against.
			const size_t Third = InTriangle(aRing[Corner], TopLeft, BottomRight, CornerOf(Cut, 2)) ? 2 : 1;
			const vec2 Away = CornerOf(Cut, Third);
			const float Whole = TwiceArea(TopLeft, BottomRight, Away);
			const float Here = Whole == 0.0f ? 1.0f : TwiceArea(aRing[Corner], BottomRight, Away) / Whole;
			const float There = Whole == 0.0f ? 0.0f : TwiceArea(TopLeft, aRing[Corner], Away) / Whole;
			const float Rest = Whole == 0.0f ? 0.0f : TwiceArea(TopLeft, BottomRight, aRing[Corner]) / Whole;

			const auto Mixed = [&](int One, int Two, int Three) {
				return (int)std::lround(One * Here + Two * There + Three * Rest);
			};
			Made.m_aColors[Corner].r = Mixed(Cut.m_aColors[0].r, Cut.m_aColors[3].r, Cut.m_aColors[Third].r);
			Made.m_aColors[Corner].g = Mixed(Cut.m_aColors[0].g, Cut.m_aColors[3].g, Cut.m_aColors[Third].g);
			Made.m_aColors[Corner].b = Mixed(Cut.m_aColors[0].b, Cut.m_aColors[3].b, Cut.m_aColors[Third].b);
			Made.m_aColors[Corner].a = Mixed(Cut.m_aColors[0].a, Cut.m_aColors[3].a, Cut.m_aColors[Third].a);
			Made.m_aTexcoords[Corner].x = Mixed(Cut.m_aTexcoords[0].x, Cut.m_aTexcoords[3].x, Cut.m_aTexcoords[Third].x);
			Made.m_aTexcoords[Corner].y = Mixed(Cut.m_aTexcoords[0].y, Cut.m_aTexcoords[3].y, Cut.m_aTexcoords[Third].y);
			Made.m_aPoints[Corner] = CPoint{f2fx(aRing[Corner].x), f2fx(aRing[Corner].y)};
		}
		// The pivot in the middle of what was cut, which is where a pivot
		// belongs on a quad nobody has moved yet.
		Made.m_aPoints[4].x = ((Made.m_aPoints[0].x + Made.m_aPoints[3].x) / 2 + (Made.m_aPoints[1].x + Made.m_aPoints[2].x) / 2) / 2;
		Made.m_aPoints[4].y = ((Made.m_aPoints[0].y + Made.m_aPoints[3].y) / 2 + (Made.m_aPoints[1].y + Made.m_aPoints[2].y) / 2) / 2;

		pQuads->m_Quads.Mutable().push_back(Made);
		const size_t At = pQuads->m_Quads.Size() - 1;
		Map.ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Changed));
		return At;
	}

	CAppendReport AppendMap(CDocument &Doc, const CMapState &Other)
	{
		CAppendReport Report;
		CMapState &Map = Doc.Edit();

		// Pictures first, because everything that is drawn names one and the
		// names have to be settled before the layers are read again.
		std::vector<int> vImageAt(Other.NumImages(), -1);
		for(size_t Index = 0; Index < Other.NumImages(); ++Index)
		{
			CImage Coming = *Other.Image(Index);
			size_t Taken = Map.NumImages();
			for(size_t Have = 0; Have < Map.NumImages(); ++Have)
			{
				if(Map.Image(Have)->m_Name == Coming.m_Name)
				{
					Taken = Have;
					break;
				}
			}
			if(Taken < Map.NumImages())
			{
				const CImage &Had = *Map.Image(Taken);
				// The same name and the same bytes is the same picture. An
				// external one carries no bytes, so for those the name is all
				// there is to go on, which is also all a game has to go on.
				const auto SameBytes = [](const CSharedList<uint8_t> &First, const CSharedList<uint8_t> &Second) {
					if(First.Size() != Second.Size())
						return false;
					for(size_t At = 0; At < First.Size(); ++At)
					{
						if(First[At] != Second[At])
							return false;
					}
					return true;
				};
				if(Had.m_External == Coming.m_External && Had.m_Width == Coming.m_Width &&
					Had.m_Height == Coming.m_Height && SameBytes(Had.m_Data, Coming.m_Data))
				{
					vImageAt[Index] = (int)Taken;
					++Report.m_SharedImages;
					continue;
				}
				// The name is taken by something else, so this one gets
				// another: dropping it would change what the map looks like.
				const auto NameIsTaken = [&Map](const std::string &Name) {
					for(size_t Have = 0; Have < Map.NumImages(); ++Have)
					{
						if(Map.Image(Have)->m_Name == Name)
							return true;
					}
					return false;
				};
				std::string Renamed = Coming.m_Name;
				for(int Try = 1; NameIsTaken(Renamed); ++Try)
					Renamed = Coming.m_Name + " (" + std::to_string(Try) + ")";
				Coming.m_Name = std::move(Renamed);
				++Report.m_RenamedImages;
			}
			vImageAt[Index] = (int)Map.NumImages();
			Map.AddImage(std::move(Coming));
			++Report.m_Images;
		}

		const int SoundAt = (int)Map.NumSounds();
		for(size_t Index = 0; Index < Other.NumSounds(); ++Index)
		{
			Map.AddSound(*Other.Sound(Index));
			++Report.m_Sounds;
		}

		const int EnvelopeAt = (int)Map.NumEnvelopes();
		for(size_t Index = 0; Index < Other.NumEnvelopes(); ++Index)
		{
			Map.AddEnvelope(*Other.Envelope(Index));
			++Report.m_Envelopes;
		}

		// Everything that is drawn, which is every group but the one the game
		// is played in.
		const std::optional<CLayerAddress> Game = FindGameLayer(Other);
		for(size_t Group = 0; Group < Other.NumGroups(); ++Group)
		{
			if(Game.has_value() && Group == Game->m_Group)
				continue;
			CGroup Coming = *Other.m_vpGroups[Group];
			for(size_t Layer = 0; Layer < Coming.m_vpLayers.size(); ++Layer)
			{
				CLayer Changed = *Coming.m_vpLayers[Layer];
				const auto Shift = [](int &Bound, int By) {
					if(Bound >= 0)
						Bound += By;
				};
				if(CTileLayer *pTiles = std::get_if<CTileLayer>(&Changed); pTiles != nullptr)
				{
					if(pTiles->m_Image >= 0 && (size_t)pTiles->m_Image < vImageAt.size())
						pTiles->m_Image = vImageAt[pTiles->m_Image];
					Shift(pTiles->m_ColorEnvelope, EnvelopeAt);
				}
				else if(CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Changed); pQuads != nullptr)
				{
					if(pQuads->m_Image >= 0 && (size_t)pQuads->m_Image < vImageAt.size())
						pQuads->m_Image = vImageAt[pQuads->m_Image];
					for(size_t Quad = 0; Quad < pQuads->m_Quads.Size(); ++Quad)
					{
						CQuad Point = pQuads->m_Quads[Quad];
						Shift(Point.m_ColorEnv, EnvelopeAt);
						Shift(Point.m_PosEnv, EnvelopeAt);
						pQuads->m_Quads.Mutable()[Quad] = Point;
					}
				}
				else if(CSoundLayer *pSounds = std::get_if<CSoundLayer>(&Changed); pSounds != nullptr)
				{
					Shift(pSounds->m_Sound, SoundAt);
					for(size_t Source = 0; Source < pSounds->m_Sources.Size(); ++Source)
					{
						CSoundSource Heard = pSounds->m_Sources[Source];
						Shift(Heard.m_PosEnv, EnvelopeAt);
						Shift(Heard.m_SoundEnv, EnvelopeAt);
						pSounds->m_Sources.Mutable()[Source] = Heard;
					}
				}
				Coming.m_vpLayers[Layer] = std::make_shared<const CLayer>(std::move(Changed));
			}
			Map.AddGroup(std::move(Coming));
			++Report.m_Groups;
		}

		// The lines a server runs. A line that is already there is already
		// there; saying it twice is what the settings check complains about.
		for(size_t Line = 0; Line < Other.m_Info.m_Settings.Size(); ++Line)
		{
			const std::string &Says = Other.m_Info.m_Settings[Line];
			bool Had = false;
			for(size_t Have = 0; Have < Map.m_Info.m_Settings.Size() && !Had; ++Have)
				Had = Map.m_Info.m_Settings[Have] == Says;
			if(Had)
				continue;
			Map.m_Info.m_Settings.Mutable().push_back(Says);
			++Report.m_Settings;
		}
		return Report;
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

	size_t AddSound(CDocument &Doc, CSound Sound)
	{
		CMapState &Map = Doc.Edit();
		Map.AddSound(std::move(Sound));
		return Map.NumSounds() - 1;
	}

	void DeleteSound(CDocument &Doc, size_t Sound)
	{
		CMapState &Map = Doc.Edit();
		dbg_assert(Sound < Map.NumSounds(), "Sound out of range");
		Map.m_vpSounds.erase(Map.m_vpSounds.begin() + Sound);

		// A layer names a sound by its place, the same way it names a
		// picture, so the places have to be read again.
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
		{
			for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
			{
				CLayer Changed = *Map.Layer(Group, Layer);
				CSoundLayer *pSounds = std::get_if<CSoundLayer>(&Changed);
				if(pSounds == nullptr)
					continue;
				const int Was = pSounds->m_Sound;
				if(pSounds->m_Sound == (int)Sound)
					pSounds->m_Sound = -1;
				else if(pSounds->m_Sound > (int)Sound)
					--pSounds->m_Sound;
				if(pSounds->m_Sound != Was)
					Map.ReplaceLayer(Group, Layer, std::move(Changed));
			}
		}
	}

	void SetSound(CDocument &Doc, size_t Index, CSound Changed)
	{
		Doc.Edit().ReplaceSound(Index, std::move(Changed));
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

	std::vector<size_t> UnusedEnvelopes(const CMapState &Map)
	{
		std::vector<bool> vUsed(Map.NumEnvelopes(), false);
		const auto Mark = [&vUsed](int Bound) {
			if(Bound >= 0 && (size_t)Bound < vUsed.size())
				vUsed[(size_t)Bound] = true;
		};
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
		{
			for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
			{
				const CLayer &Which = *Map.Layer(Group, Layer);
				if(const CTileLayer *pTiles = std::get_if<CTileLayer>(&Which); pTiles != nullptr)
				{
					Mark(pTiles->m_ColorEnvelope);
				}
				else if(const CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Which); pQuads != nullptr)
				{
					for(size_t Index = 0; Index < pQuads->m_Quads.Size(); ++Index)
					{
						Mark(pQuads->m_Quads[Index].m_ColorEnv);
						Mark(pQuads->m_Quads[Index].m_PosEnv);
					}
				}
				else if(const CSoundLayer *pSounds = std::get_if<CSoundLayer>(&Which); pSounds != nullptr)
				{
					for(size_t Index = 0; Index < pSounds->m_Sources.Size(); ++Index)
					{
						Mark(pSounds->m_Sources[Index].m_SoundEnv);
						Mark(pSounds->m_Sources[Index].m_PosEnv);
					}
				}
			}
		}
		std::vector<size_t> vUnused;
		for(size_t Envelope = Map.NumEnvelopes(); Envelope > 0; --Envelope)
		{
			if(!vUsed[Envelope - 1])
				vUnused.push_back(Envelope - 1);
		}
		return vUnused;
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
