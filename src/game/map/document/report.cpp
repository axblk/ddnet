#include "report.h"

#include <engine/shared/jsonwriter.h>

#include <game/map/document/document.h>

#include <variant>

namespace map_document
{
	namespace
	{
		const char *TileLayerKindName(ETileLayerKind Kind)
		{
			switch(Kind)
			{
			case ETileLayerKind::TILES: return "tiles";
			case ETileLayerKind::GAME: return "game";
			case ETileLayerKind::FRONT: return "front";
			case ETileLayerKind::TELE: return "tele";
			case ETileLayerKind::SPEEDUP: return "speedup";
			case ETileLayerKind::SWITCH: return "switch";
			case ETileLayerKind::TUNE: return "tune";
			}
			return "tiles";
		}

		void WriteIntPair(CJsonWriter &Writer, const char *pName, int First, int Second)
		{
			Writer.WriteAttribute(pName);
			Writer.BeginArray();
			Writer.WriteIntValue(First);
			Writer.WriteIntValue(Second);
			Writer.EndArray();
		}

		void WriteTileLayer(CJsonWriter &Writer, const CTileLayer &Layer)
		{
			Writer.WriteAttribute("type");
			Writer.WriteStrValue("tiles");
			Writer.WriteAttribute("kind");
			Writer.WriteStrValue(TileLayerKindName(Layer.m_Kind));
			WriteIntPair(Writer, "size", Layer.Width(), Layer.Height());
			Writer.WriteAttribute("image");
			Writer.WriteIntValue(Layer.m_Image);
			Writer.WriteAttribute("color");
			Writer.BeginArray();
			Writer.WriteIntValue(Layer.m_Color.r);
			Writer.WriteIntValue(Layer.m_Color.g);
			Writer.WriteIntValue(Layer.m_Color.b);
			Writer.WriteIntValue(Layer.m_Color.a);
			Writer.EndArray();
			Writer.WriteAttribute("colorEnvelope");
			Writer.WriteIntValue(Layer.m_ColorEnvelope);
			Writer.WriteAttribute("colorEnvelopeOffset");
			Writer.WriteIntValue(Layer.m_ColorEnvelopeOffset);
			Writer.WriteAttribute("automapperConfig");
			Writer.WriteIntValue(Layer.m_AutomapperConfig);
			Writer.WriteAttribute("automapperSeed");
			Writer.WriteIntValue(Layer.m_AutomapperSeed);
			Writer.WriteAttribute("automapperAutomatic");
			Writer.WriteBoolValue(Layer.m_AutomapperAutomatic);
		}

		void WriteLayer(CJsonWriter &Writer, const CLayer &Layer)
		{
			Writer.BeginObject();
			const CLayerProperties &Properties = LayerProperties(Layer);
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(Properties.m_Name.c_str());
			Writer.WriteAttribute("detail");
			Writer.WriteBoolValue(Properties.m_Detail);
			std::visit([&Writer](const auto &Kind) {
				using TLayer = std::decay_t<decltype(Kind)>;
				if constexpr(std::is_same_v<TLayer, CTileLayer>)
				{
					WriteTileLayer(Writer, Kind);
				}
				else if constexpr(std::is_same_v<TLayer, CQuadLayer>)
				{
					Writer.WriteAttribute("type");
					Writer.WriteStrValue("quads");
					Writer.WriteAttribute("image");
					Writer.WriteIntValue(Kind.m_Image);
					Writer.WriteAttribute("quads");
					Writer.WriteIntValue((int)Kind.m_Quads.Size());
				}
				else
				{
					Writer.WriteAttribute("type");
					Writer.WriteStrValue("sounds");
					Writer.WriteAttribute("sound");
					Writer.WriteIntValue(Kind.m_Sound);
					Writer.WriteAttribute("sources");
					Writer.WriteIntValue((int)Kind.m_Sources.Size());
				}
			},
				Layer);
			Writer.EndObject();
		}

		void WriteGroup(CJsonWriter &Writer, const CGroup &Group)
		{
			Writer.BeginObject();
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(Group.m_Name.c_str());
			WriteIntPair(Writer, "offset", Group.m_OffsetX, Group.m_OffsetY);
			WriteIntPair(Writer, "parallax", Group.m_ParallaxX, Group.m_ParallaxY);
			Writer.WriteAttribute("useClipping");
			Writer.WriteBoolValue(Group.m_UseClipping);
			Writer.WriteAttribute("clip");
			Writer.BeginArray();
			Writer.WriteIntValue(Group.m_ClipX);
			Writer.WriteIntValue(Group.m_ClipY);
			Writer.WriteIntValue(Group.m_ClipW);
			Writer.WriteIntValue(Group.m_ClipH);
			Writer.EndArray();
			Writer.WriteAttribute("layers");
			Writer.BeginArray();
			for(const auto &pLayer : Group.m_vpLayers)
				WriteLayer(Writer, *pLayer);
			Writer.EndArray();
			Writer.EndObject();
		}

		void WriteInfo(CJsonWriter &Writer, const CMapInfo &Info)
		{
			Writer.WriteAttribute("info");
			Writer.BeginObject();
			Writer.WriteAttribute("author");
			Writer.WriteStrValue(Info.m_Author.c_str());
			Writer.WriteAttribute("mapVersion");
			Writer.WriteStrValue(Info.m_MapVersion.c_str());
			Writer.WriteAttribute("credits");
			Writer.WriteStrValue(Info.m_Credits.c_str());
			Writer.WriteAttribute("license");
			Writer.WriteStrValue(Info.m_License.c_str());
			Writer.WriteAttribute("settings");
			Writer.BeginArray();
			for(const std::string &Setting : Info.m_Settings.All())
				Writer.WriteStrValue(Setting.c_str());
			Writer.EndArray();
			Writer.EndObject();
		}
	} // namespace

	std::string StructureJson(const CMapState &Map)
	{
		CJsonStringWriter Writer;
		Writer.BeginObject();
		WriteInfo(Writer, Map.m_Info);

		Writer.WriteAttribute("groups");
		Writer.BeginArray();
		for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
			WriteGroup(Writer, *Map.Group(Group));
		Writer.EndArray();

		Writer.WriteAttribute("envelopes");
		Writer.BeginArray();
		for(size_t Index = 0; Index < Map.NumEnvelopes(); ++Index)
		{
			const CEnvelope *pEnvelope = Map.Envelope(Index);
			Writer.BeginObject();
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(pEnvelope->m_Name.c_str());
			Writer.WriteAttribute("channels");
			Writer.WriteIntValue(pEnvelope->m_Channels);
			Writer.WriteAttribute("synchronized");
			Writer.WriteBoolValue(pEnvelope->m_Synchronized);
			Writer.WriteAttribute("points");
			Writer.WriteIntValue((int)pEnvelope->m_Points.Size());
			Writer.EndObject();
		}
		Writer.EndArray();

		Writer.WriteAttribute("images");
		Writer.BeginArray();
		for(size_t Index = 0; Index < Map.NumImages(); ++Index)
		{
			const CImage *pImage = Map.Image(Index);
			Writer.BeginObject();
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(pImage->m_Name.c_str());
			Writer.WriteAttribute("external");
			Writer.WriteBoolValue(pImage->m_External);
			WriteIntPair(Writer, "size", pImage->m_Width, pImage->m_Height);
			Writer.EndObject();
		}
		Writer.EndArray();

		Writer.WriteAttribute("sounds");
		Writer.BeginArray();
		for(size_t Index = 0; Index < Map.NumSounds(); ++Index)
		{
			const CSound *pSound = Map.Sound(Index);
			Writer.BeginObject();
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(pSound->m_Name.c_str());
			Writer.WriteAttribute("external");
			Writer.WriteBoolValue(pSound->m_External);
			Writer.WriteAttribute("bytes");
			Writer.WriteInt64Value((int64_t)pSound->m_Data.Size());
			Writer.EndObject();
		}
		Writer.EndArray();

		Writer.EndObject();
		return Writer.GetOutputString();
	}

	std::string EnvelopeJson(const CMapState &Map, size_t Index)
	{
		if(Index >= Map.NumEnvelopes())
			return "null";
		const CEnvelope &Envelope = *Map.Envelope(Index);
		CJsonStringWriter Writer;
		Writer.BeginObject();
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(Envelope.m_Name.c_str());
		Writer.WriteAttribute("channels");
		Writer.WriteIntValue(Envelope.m_Channels);
		Writer.WriteAttribute("synchronized");
		Writer.WriteBoolValue(Envelope.m_Synchronized);
		Writer.WriteAttribute("points");
		Writer.BeginArray();
		for(size_t Point = 0; Point < Envelope.m_Points.Size(); ++Point)
		{
			const CEnvPoint_runtime &Made = Envelope.m_Points[Point];
			Writer.BeginObject();
			Writer.WriteAttribute("time");
			Writer.WriteIntValue(Made.m_Time.GetInternal());
			Writer.WriteAttribute("curve");
			Writer.WriteIntValue(Made.m_Curvetype);
			Writer.WriteAttribute("values");
			Writer.BeginArray();
			for(int Channel = 0; Channel < Envelope.m_Channels; ++Channel)
				Writer.WriteIntValue(Made.m_aValues[Channel]);
			Writer.EndArray();
			// The tangents are only read by a curve of the kind that has
			// them, but they are held either way - so they go out either way,
			// and a point that was bezier once and is linear now still knows
			// what it looked like.
			Writer.WriteAttribute("in");
			Writer.BeginArray();
			for(int Channel = 0; Channel < Envelope.m_Channels; ++Channel)
			{
				Writer.WriteIntValue(Made.m_Bezier.m_aInTangentDeltaX[Channel].GetInternal());
				Writer.WriteIntValue(Made.m_Bezier.m_aInTangentDeltaY[Channel]);
			}
			Writer.EndArray();
			Writer.WriteAttribute("out");
			Writer.BeginArray();
			for(int Channel = 0; Channel < Envelope.m_Channels; ++Channel)
			{
				Writer.WriteIntValue(Made.m_Bezier.m_aOutTangentDeltaX[Channel].GetInternal());
				Writer.WriteIntValue(Made.m_Bezier.m_aOutTangentDeltaY[Channel]);
			}
			Writer.EndArray();
			Writer.EndObject();
		}
		Writer.EndArray();
		Writer.EndObject();
		return Writer.GetOutputString();
	}

	std::string QuadsJson(const CMapState &Map, size_t Group, size_t Layer)
	{
		if(Group >= Map.NumGroups() || Layer >= Map.NumLayers(Group))
			return "null";
		const CQuadLayer *pQuads = std::get_if<CQuadLayer>(Map.Layer(Group, Layer));
		if(pQuads == nullptr)
			return "null";
		CJsonStringWriter Writer;
		Writer.BeginArray();
		for(size_t Index = 0; Index < pQuads->m_Quads.Size(); ++Index)
		{
			const CQuad &Quad = pQuads->m_Quads[Index];
			Writer.BeginObject();
			Writer.WriteAttribute("points");
			Writer.BeginArray();
			for(const CPoint &Point : Quad.m_aPoints)
			{
				Writer.WriteIntValue(fx2i(Point.x));
				Writer.WriteIntValue(fx2i(Point.y));
			}
			Writer.EndArray();
			Writer.WriteAttribute("colors");
			Writer.BeginArray();
			for(const CColor &Color : Quad.m_aColors)
			{
				Writer.WriteIntValue(Color.r);
				Writer.WriteIntValue(Color.g);
				Writer.WriteIntValue(Color.b);
				Writer.WriteIntValue(Color.a);
			}
			Writer.EndArray();
			Writer.WriteAttribute("posEnv");
			Writer.WriteIntValue(Quad.m_PosEnv);
			Writer.WriteAttribute("posEnvOffset");
			Writer.WriteIntValue(Quad.m_PosEnvOffset);
			Writer.WriteAttribute("colorEnv");
			Writer.WriteIntValue(Quad.m_ColorEnv);
			Writer.WriteAttribute("colorEnvOffset");
			Writer.WriteIntValue(Quad.m_ColorEnvOffset);
			Writer.EndObject();
		}
		Writer.EndArray();
		return Writer.GetOutputString();
	}

	std::string HistoryJson(const CDocument &Document)
	{
		const CHistory &History = Document.History();
		CJsonStringWriter Writer;
		Writer.BeginObject();
		Writer.WriteAttribute("current");
		Writer.WriteIntValue((int)History.CurrentIndex());
		Writer.WriteAttribute("canUndo");
		Writer.WriteBoolValue(History.CanUndo());
		Writer.WriteAttribute("canRedo");
		Writer.WriteBoolValue(History.CanRedo());
		Writer.WriteAttribute("bytes");
		Writer.WriteInt64Value((int64_t)History.Bytes());
		Writer.WriteAttribute("maxBytes");
		Writer.WriteInt64Value((int64_t)History.MaxBytes());
		// Whether a change is half made right now. What is drawn is then the
		// version inside the transaction, which is not one of the entries
		// below - a panel that shows the map as "at entry 7" while a slider is
		// being dragged would be telling the truth about the wrong thing.
		Writer.WriteAttribute("editing");
		Writer.WriteBoolValue(Document.IsEditing());
		Writer.WriteAttribute("entries");
		Writer.BeginArray();
		for(size_t Index = 0; Index < History.NumEntries(); ++Index)
		{
			const CHistory::CEntry &Entry = History.Entry(Index);
			Writer.BeginObject();
			Writer.WriteAttribute("label");
			Writer.WriteStrValue(Entry.m_Label.c_str());
			Writer.WriteAttribute("timeNanos");
			Writer.WriteInt64Value(Entry.m_TimeNanos);
			Writer.EndObject();
		}
		Writer.EndArray();
		Writer.EndObject();
		return Writer.GetOutputString();
	}
} // namespace map_document
