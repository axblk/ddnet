#include "report.h"

#include <base/math.h>

#include <engine/shared/jsonwriter.h>

#include <game/map/document/document.h>
#include <game/map/document/edit.h>
#include <game/map/document/proof.h>
#include <game/map/document/settings.h>

#include <string>
#include <variant>
#include <vector>

namespace map_document
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

	namespace
	{
		void WriteIntPair(CJsonWriter &Writer, const char *pName, int First, int Second)
		{
			Writer.WriteAttribute(pName);
			Writer.BeginArray();
			Writer.WriteIntValue(First);
			Writer.WriteIntValue(Second);
			Writer.EndArray();
		}

		/**
		 * A proof rectangle as four whole world units: left, top, right,
		 * bottom.
		 *
		 * Whole units, because a world unit is a thirty-second of a tile and
		 * nobody can see a thirty-second of a tile - and because everything
		 * else that leaves here is a whole number too.
		 */
		void WriteProofRect(CJsonWriter &Writer, const CProofRect &Rect)
		{
			Writer.BeginArray();
			Writer.WriteIntValue(round_to_int(Rect.m_TopLeft.x));
			Writer.WriteIntValue(round_to_int(Rect.m_TopLeft.y));
			Writer.WriteIntValue(round_to_int(Rect.m_BottomRight.x));
			Writer.WriteIntValue(round_to_int(Rect.m_BottomRight.y));
			Writer.EndArray();
		}

		void WriteTileLayer(CJsonWriter &Writer, const CTileLayer &Layer, bool Construct)
		{
			// Whether this layer's tiles can be turned into game tiles. The
			// rule is the document's - a page cannot work out for itself
			// whether a group lies over the game layer - so the answer comes
			// with the layer rather than being asked for separately.
			Writer.WriteAttribute("construct");
			Writer.WriteBoolValue(Construct);
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

		void WriteLayer(CJsonWriter &Writer, const CLayer &Layer, bool Construct)
		{
			Writer.BeginObject();
			const CLayerProperties &Properties = LayerProperties(Layer);
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(Properties.m_Name.c_str());
			Writer.WriteAttribute("detail");
			Writer.WriteBoolValue(Properties.m_Detail);
			std::visit([&Writer, Construct](const auto &Kind) {
				using TLayer = std::decay_t<decltype(Kind)>;
				if constexpr(std::is_same_v<TLayer, CTileLayer>)
				{
					WriteTileLayer(Writer, Kind, Construct);
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

		void WriteGroup(CJsonWriter &Writer, const CMapState &Map, size_t Index)
		{
			const CGroup &Group = *Map.Group(Index);
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
			for(size_t Layer = 0; Layer < Group.m_vpLayers.size(); ++Layer)
				WriteLayer(Writer, *Group.m_vpLayers[Layer], CanConstructGameTiles(Map, Index, Layer));
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
			WriteGroup(Writer, Map, Group);
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
			// Where the corners sit in the picture, in the numbers the file
			// holds: 1024 is the whole picture across, so 0 and 1024 are its
			// edges. Handing these out as a fraction would lose what a map
			// that repeats its picture forty times holds.
			Writer.WriteAttribute("texcoords");
			Writer.BeginArray();
			for(const CPoint &Point : Quad.m_aTexcoords)
			{
				Writer.WriteIntValue(Point.x);
				Writer.WriteIntValue(Point.y);
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

	std::string SoundSourcesJson(const CMapState &Map, size_t Group, size_t Layer)
	{
		if(Group >= Map.NumGroups() || Layer >= Map.NumLayers(Group))
			return "null";
		const CSoundLayer *pSounds = std::get_if<CSoundLayer>(Map.Layer(Group, Layer));
		if(pSounds == nullptr)
			return "null";
		CJsonStringWriter Writer;
		Writer.BeginArray();
		for(size_t Index = 0; Index < pSounds->m_Sources.Size(); ++Index)
		{
			const CSoundSource &Source = pSounds->m_Sources[Index];
			Writer.BeginObject();
			WriteIntPair(Writer, "position", fx2i(Source.m_Position.x), fx2i(Source.m_Position.y));
			Writer.WriteAttribute("shape");
			Writer.WriteStrValue(Source.m_Shape.m_Type == CSoundShape::SHAPE_CIRCLE ? "circle" : "rectangle");
			if(Source.m_Shape.m_Type == CSoundShape::SHAPE_CIRCLE)
			{
				Writer.WriteAttribute("radius");
				Writer.WriteIntValue(Source.m_Shape.m_Circle.m_Radius);
			}
			else
			{
				// The two sides of a rectangle are the file's own fixed
				// point, so they come out in world units like everything
				// else that is a distance.
				WriteIntPair(Writer, "size", fx2i(Source.m_Shape.m_Rectangle.m_Width), fx2i(Source.m_Shape.m_Rectangle.m_Height));
			}
			Writer.WriteAttribute("loop");
			Writer.WriteBoolValue(Source.m_Loop != 0);
			Writer.WriteAttribute("pan");
			Writer.WriteBoolValue(Source.m_Pan != 0);
			Writer.WriteAttribute("timeDelay");
			Writer.WriteIntValue(Source.m_TimeDelay);
			Writer.WriteAttribute("falloff");
			Writer.WriteIntValue(Source.m_Falloff);
			Writer.WriteAttribute("posEnv");
			Writer.WriteIntValue(Source.m_PosEnv);
			Writer.WriteAttribute("posEnvOffset");
			Writer.WriteIntValue(Source.m_PosEnvOffset);
			Writer.WriteAttribute("soundEnv");
			Writer.WriteIntValue(Source.m_SoundEnv);
			Writer.WriteAttribute("soundEnvOffset");
			Writer.WriteIntValue(Source.m_SoundEnvOffset);
			Writer.EndObject();
		}
		Writer.EndArray();
		return Writer.GetOutputString();
	}

	std::string ProofJson(const CMapState &Map, vec2 Center, bool Menu)
	{
		// A menu stands further back than a game does.
		const float Zoom = Menu ? 0.7f : 1.0f;

		CJsonStringWriter Writer;
		Writer.BeginObject();
		Writer.WriteAttribute("menu");
		Writer.WriteBoolValue(Menu);
		WriteIntPair(Writer, "center", round_to_int(Center.x), round_to_int(Center.y));

		// From a square screen to 16:9 in twenty steps. What the page draws
		// from it is one outline, so the corners come in order and the page
		// does not have to know which end is which.
		Writer.WriteAttribute("steps");
		Writer.BeginArray();
		constexpr int NumSteps = 20;
		for(int Step = 0; Step <= NumSteps; ++Step)
		{
			const float Aspect = 1.0f + (16.0f / 9.0f - 1.0f) * (Step / (float)NumSteps);
			WriteProofRect(Writer, ProofScreen(Center, Aspect, Zoom));
		}
		Writer.EndArray();

		// The two a mapper is told to check by name, so the page can say which
		// is which rather than colouring two rectangles and hoping.
		Writer.WriteAttribute("named");
		Writer.BeginArray();
		static const struct
		{
			const char *m_pName;
			float m_Aspect;
		} s_aNamed[] = {
			{"4:3", 4.0f / 3.0f},
			{"16:10", 16.0f / 10.0f}};
		for(const auto &Named : s_aNamed)
		{
			Writer.BeginObject();
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(Named.m_pName);
			Writer.WriteAttribute("rect");
			WriteProofRect(Writer, ProofScreen(Center, Named.m_Aspect, Zoom));
			Writer.EndObject();
		}
		Writer.EndArray();

		// Only in menu mode, and only the places the map itself names.
		Writer.WriteAttribute("positions");
		Writer.BeginArray();
		if(Menu)
		{
			for(const CMenuPosition &Position : MenuPositions(Map))
			{
				Writer.BeginObject();
				Writer.WriteAttribute("index");
				Writer.WriteIntValue(Position.m_Index);
				WriteIntPair(Writer, "position", round_to_int(Position.m_Position.x), round_to_int(Position.m_Position.y));
				Writer.EndObject();
			}
		}
		Writer.EndArray();

		Writer.EndObject();
		return Writer.GetOutputString();
	}

	std::string SettingsHelpJson()
	{
		CJsonStringWriter Writer;
		Writer.BeginArray();
		for(const CMapSetting &Setting : KnownSettings())
		{
			Writer.BeginObject();
			Writer.WriteAttribute("name");
			Writer.WriteStrValue(Setting.m_Name.c_str());
			Writer.WriteAttribute("help");
			Writer.WriteStrValue(Setting.m_Help.c_str());
			Writer.WriteAttribute("variable");
			Writer.WriteBoolValue(Setting.m_IsVariable);
			if(Setting.m_IsVariable)
			{
				Writer.WriteAttribute("default");
				Writer.WriteIntValue(Setting.m_Default);
				WriteIntPair(Writer, "range", Setting.m_Min, Setting.m_Max);
			}
			Writer.WriteAttribute("args");
			Writer.BeginArray();
			for(const CSettingArg &Arg : Setting.m_Args)
			{
				Writer.BeginObject();
				Writer.WriteAttribute("name");
				Writer.WriteStrValue(Arg.m_Name.c_str());
				Writer.WriteAttribute("type");
				const char aType[2] = {Arg.m_Type, '\0'};
				Writer.WriteStrValue(aType);
				Writer.WriteAttribute("optional");
				Writer.WriteBoolValue(Arg.m_Optional);
				Writer.EndObject();
			}
			Writer.EndArray();
			Writer.EndObject();
		}
		Writer.EndArray();
		return Writer.GetOutputString();
	}

	std::string SettingProblemsJson(const CMapState &Map)
	{
		// A line repeats an earlier one, never a later one, so each is held
		// against what stands above it and nothing else.
		std::vector<std::string> vAbove;
		CJsonStringWriter Writer;
		Writer.BeginArray();
		for(size_t Line = 0; Line < Map.m_Info.m_Settings.Size(); ++Line)
		{
			const std::string &Text = Map.m_Info.m_Settings[Line];
			Writer.BeginObject();
			Writer.WriteAttribute("problem");
			Writer.WriteStrValue(CheckSetting(Text.c_str()).c_str());
			Writer.WriteAttribute("repeats");
			Writer.WriteIntValue(CollidingSetting(vAbove, Text.c_str()));
			Writer.EndObject();
			vAbove.push_back(Text);
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
