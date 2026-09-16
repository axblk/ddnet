#include "command.h"

#include <base/str.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>

#include <game/map/document/art.h>
#include <game/map/document/document.h>
#include <game/map/document/edit.h>
#include <game/map/document/structure.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace map_document
{
	namespace
	{
		std::string Failed(const std::string &Message)
		{
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(false);
			Writer.WriteAttribute("error");
			Writer.WriteStrValue(Message.c_str());
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		/**
		 * `{"ok":true}`, with one number in it where the command has one to
		 * give back - where a new group or layer ended up.
		 */
		std::string Succeeded(const char *pName = nullptr, int Value = 0, const char *pSecondName = nullptr, int SecondValue = 0)
		{
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(true);
			if(pName != nullptr)
			{
				Writer.WriteAttribute(pName);
				Writer.WriteIntValue(Value);
			}
			if(pSecondName != nullptr)
			{
				Writer.WriteAttribute(pSecondName);
				Writer.WriteIntValue(SecondValue);
			}
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		/**
		 * The arguments of one command, read one at a time. The first one
		 * that is missing or of the wrong sort leaves its complaint here, and
		 * the caller looks once at the end - so that reading a command is a
		 * list of lines rather than a staircase of conditions.
		 */
		class CArguments
		{
		public:
			explicit CArguments(const json_value *pCommand) :
				m_pCommand(pCommand) {}

			bool Failed() const { return !m_Error.empty(); }
			const std::string &Error() const { return m_Error; }

			int Int(const char *pName, int Default = 0)
			{
				const json_value *pValue = json_object_get(m_pCommand, pName);
				if(pValue->type == json_none)
					return Default;
				if(pValue->type != json_integer)
					return Complain(pName, "a whole number", Default);
				return json_int_get(pValue);
			}

			/**
			 * A number that has to be there and has to name something that
			 * is - the group a command is about, or where a layer is to go.
			 *
			 * @param pName The name of the argument.
			 * @param Count How many there are, so that `Count` itself is
			 * allowed where a command says where something goes and refused
			 * where it says which one it means.
			 * @param PastTheEnd Whether `Count` is allowed.
			 *
			 * @return The number, or zero after complaining.
			 */
			size_t Index(const char *pName, size_t Count, bool PastTheEnd = false)
			{
				const json_value *pValue = json_object_get(m_pCommand, pName);
				if(pValue->type != json_integer)
					return Complain(pName, "a whole number", 0);
				const int Value = json_int_get(pValue);
				const size_t Limit = PastTheEnd ? Count : (Count == 0 ? 0 : Count - 1);
				if(Value < 0 || Count == 0 || (size_t)Value > Limit)
				{
					char aMessage[128];
					str_format(aMessage, sizeof(aMessage), "'%s' is %d, which is not there", pName, Value);
					if(m_Error.empty())
						m_Error = aMessage;
					return 0;
				}
				return (size_t)Value;
			}

			const char *Str(const char *pName, const char *pDefault = "")
			{
				const json_value *pValue = json_object_get(m_pCommand, pName);
				if(pValue->type == json_none)
					return pDefault;
				if(pValue->type != json_string)
					return Complain(pName, "a string", pDefault);
				return json_string_get(pValue);
			}

		private:
			template<typename T>
			T Complain(const char *pName, const char *pWanted, T Fallback)
			{
				char aMessage[128];
				str_format(aMessage, sizeof(aMessage), "'%s' has to be %s", pName, pWanted);
				if(m_Error.empty())
					m_Error = aMessage;
				return Fallback;
			}

			const json_value *m_pCommand;
			std::string m_Error;
		};

		bool ReadColor(const json_value *pValue, CColor *pColor, std::string *pError)
		{
			if(pValue->type != json_array || json_array_length(pValue) != 4)
			{
				*pError = "a colour is four whole numbers";
				return false;
			}
			int aChannels[4];
			for(int i = 0; i < 4; ++i)
			{
				const json_value *pChannel = json_array_get(pValue, i);
				if(pChannel->type != json_integer)
				{
					*pError = "a colour is four whole numbers";
					return false;
				}
				aChannels[i] = std::clamp(json_int_get(pChannel), 0, 255);
			}
			*pColor = CColor(aChannels[0], aChannels[1], aChannels[2], aChannels[3]);
			return true;
		}

		bool ReadInt(const json_value *pValue, int *pOut, std::string *pError)
		{
			if(pValue->type != json_integer)
			{
				*pError = "that property is a whole number";
				return false;
			}
			*pOut = json_int_get(pValue);
			return true;
		}

		bool ReadBool(const json_value *pValue, bool *pOut, std::string *pError)
		{
			if(pValue->type != json_boolean)
			{
				*pError = "that property is true or false";
				return false;
			}
			*pOut = json_boolean_get(pValue) != 0;
			return true;
		}

		bool ReadString(const json_value *pValue, std::string *pOut, std::string *pError)
		{
			if(pValue->type != json_string)
			{
				*pError = "that property is a string";
				return false;
			}
			*pOut = json_string_get(pValue);
			return true;
		}

		/** A quad's corner colour, which is four whole numbers from 0 to 255. */
		bool ReadQuadColor(const json_value *pValue, CColor *pColor, std::string *pError)
		{
			if(pValue->type != json_array || json_array_length(pValue) != 4)
			{
				*pError = "a colour is four whole numbers";
				return false;
			}
			int aChannels[4];
			for(int Channel = 0; Channel < 4; ++Channel)
			{
				const json_value *pChannel = json_array_get(pValue, Channel);
				if(pChannel->type != json_integer)
				{
					*pError = "a colour is four whole numbers";
					return false;
				}
				aChannels[Channel] = std::clamp(json_int_get(pChannel), 0, 255);
			}
			*pColor = CColor(aChannels[0], aChannels[1], aChannels[2], aChannels[3]);
			return true;
		}

		/**
		 * What a quad has beside its points and colours: which envelopes move and
		 * colour it, and how far into them it starts.
		 *
		 * An envelope is named by its place, so -1 for none and anything else has
		 * to be an envelope the map has - a binding to one that is not there is a
		 * map that reads back differently than it was written.
		 */
		bool SetQuadProp(CQuad &Quad, const char *pProp, const json_value *pValue, size_t NumEnvelopes, std::string *pError)
		{
			int Value = 0;
			if(!ReadInt(pValue, &Value, pError))
				return false;
			const bool Envelope = str_comp(pProp, "posEnv") == 0 || str_comp(pProp, "colorEnv") == 0;
			if(Envelope && (Value < -1 || Value >= (int)NumEnvelopes))
			{
				*pError = "there is no such envelope";
				return false;
			}
			if(str_comp(pProp, "posEnv") == 0)
				Quad.m_PosEnv = Value;
			else if(str_comp(pProp, "posEnvOffset") == 0)
				Quad.m_PosEnvOffset = Value;
			else if(str_comp(pProp, "colorEnv") == 0)
				Quad.m_ColorEnv = Value;
			else if(str_comp(pProp, "colorEnvOffset") == 0)
				Quad.m_ColorEnvOffset = Value;
			else
			{
				*pError = std::string("a quad has no '") + pProp + "'";
				return false;
			}
			return true;
		}

		/**
		 * Reads what a command says about a sound source onto one.
		 *
		 * The shape is the awkward one: which of the two it is and how large
		 * it is are three numbers in a union, so setting the shape sets a
		 * size with it rather than leaving whatever the other kind had.
		 */
		bool SetSoundProp(CSoundSource &Source, const char *pProp, const json_value *pValue, size_t NumEnvelopes, std::string *pError)
		{
			if(str_comp(pProp, "loop") == 0 || str_comp(pProp, "pan") == 0)
			{
				bool Value = false;
				if(!ReadBool(pValue, &Value, pError))
					return false;
				(str_comp(pProp, "loop") == 0 ? Source.m_Loop : Source.m_Pan) = Value ? 1 : 0;
				return true;
			}
			if(str_comp(pProp, "shape") == 0)
			{
				std::string Value;
				if(!ReadString(pValue, &Value, pError))
					return false;
				if(Value == "circle")
				{
					if(Source.m_Shape.m_Type != CSoundShape::SHAPE_CIRCLE)
					{
						Source.m_Shape.m_Type = CSoundShape::SHAPE_CIRCLE;
						Source.m_Shape.m_Circle.m_Radius = 96;
					}
					return true;
				}
				if(Value == "rectangle")
				{
					if(Source.m_Shape.m_Type != CSoundShape::SHAPE_RECTANGLE)
					{
						Source.m_Shape.m_Type = CSoundShape::SHAPE_RECTANGLE;
						Source.m_Shape.m_Rectangle.m_Width = i2fx(192);
						Source.m_Shape.m_Rectangle.m_Height = i2fx(192);
					}
					return true;
				}
				*pError = "a source is heard within a circle or a rectangle";
				return false;
			}

			int Value = 0;
			if(!ReadInt(pValue, &Value, pError))
				return false;
			const bool Envelope = str_comp(pProp, "posEnv") == 0 || str_comp(pProp, "soundEnv") == 0;
			if(Envelope && (Value < -1 || Value >= (int)NumEnvelopes))
			{
				*pError = "there is no such envelope";
				return false;
			}
			if(str_comp(pProp, "radius") == 0)
			{
				if(Source.m_Shape.m_Type != CSoundShape::SHAPE_CIRCLE)
				{
					*pError = "this source is heard within a rectangle";
					return false;
				}
				if(Value <= 0)
				{
					*pError = "a source is heard within something";
					return false;
				}
				Source.m_Shape.m_Circle.m_Radius = Value;
			}
			else if(str_comp(pProp, "width") == 0 || str_comp(pProp, "height") == 0)
			{
				if(Source.m_Shape.m_Type != CSoundShape::SHAPE_RECTANGLE)
				{
					*pError = "this source is heard within a circle";
					return false;
				}
				if(Value <= 0)
				{
					*pError = "a source is heard within something";
					return false;
				}
				(str_comp(pProp, "width") == 0 ? Source.m_Shape.m_Rectangle.m_Width : Source.m_Shape.m_Rectangle.m_Height) = i2fx(Value);
			}
			else if(str_comp(pProp, "timeDelay") == 0)
				Source.m_TimeDelay = Value;
			else if(str_comp(pProp, "falloff") == 0)
				Source.m_Falloff = std::clamp(Value, 0, 255);
			else if(str_comp(pProp, "posEnv") == 0)
				Source.m_PosEnv = Value;
			else if(str_comp(pProp, "posEnvOffset") == 0)
				Source.m_PosEnvOffset = Value;
			else if(str_comp(pProp, "soundEnv") == 0)
				Source.m_SoundEnv = Value;
			else if(str_comp(pProp, "soundEnvOffset") == 0)
				Source.m_SoundEnvOffset = Value;
			else
			{
				*pError = std::string("a sound source has no '") + pProp + "'";
				return false;
			}
			return true;
		}

		bool SetEnvelopeProp(CEnvelope &Envelope, const char *pProp, const json_value *pValue, std::string *pError)
		{
			if(str_comp(pProp, "name") == 0)
				return ReadString(pValue, &Envelope.m_Name, pError);
			if(str_comp(pProp, "synchronized") == 0)
				return ReadBool(pValue, &Envelope.m_Synchronized, pError);
			*pError = std::string("an envelope has no '") + pProp + "'";
			return false;
		}

		/**
		 * Reads what a command says about an envelope point onto one.
		 *
		 * Everything is optional, so that moving a point in time and changing
		 * what it is worth are the same command with different parts of it
		 * filled in. Times are whole milliseconds and values are the map's own
		 * 22.10 fixed point, because that is what the file holds - what a value
		 * means is a question about the envelope's channels, and belongs to
		 * whoever knows that.
		 */
		bool ReadEnvelopePoint(const json_value *pCommand, int Channels, CEnvPoint_runtime *pPoint, std::string *pError)
		{
			const json_value *pTime = json_object_get(pCommand, "time");
			if(pTime->type != json_none)
			{
				int Millis = 0;
				if(!ReadInt(pTime, &Millis, pError))
					return false;
				pPoint->m_Time = CFixedTime(std::max(0, Millis));
			}
			const json_value *pCurve = json_object_get(pCommand, "curve");
			if(pCurve->type != json_none)
			{
				int Curve = 0;
				if(!ReadInt(pCurve, &Curve, pError))
					return false;
				if(Curve < 0 || Curve >= NUM_CURVETYPES)
				{
					*pError = "that is not a kind of curve";
					return false;
				}
				pPoint->m_Curvetype = Curve;
			}
			const json_value *pValues = json_object_get(pCommand, "values");
			if(pValues->type != json_none)
			{
				if(pValues->type != json_array || json_array_length(pValues) != Channels)
				{
					*pError = "a point carries one value for each of the envelope's channels";
					return false;
				}
				for(int Channel = 0; Channel < Channels; ++Channel)
				{
					if(!ReadInt(json_array_get(pValues, Channel), &pPoint->m_aValues[Channel], pError))
						return false;
				}
			}
			return true;
		}

		bool SetGroupProp(CGroup &Group, const char *pProp, const json_value *pValue, std::string *pError)
		{
			if(str_comp(pProp, "name") == 0)
				return ReadString(pValue, &Group.m_Name, pError);
			if(str_comp(pProp, "offsetX") == 0)
				return ReadInt(pValue, &Group.m_OffsetX, pError);
			if(str_comp(pProp, "offsetY") == 0)
				return ReadInt(pValue, &Group.m_OffsetY, pError);
			if(str_comp(pProp, "parallaxX") == 0)
				return ReadInt(pValue, &Group.m_ParallaxX, pError);
			if(str_comp(pProp, "parallaxY") == 0)
				return ReadInt(pValue, &Group.m_ParallaxY, pError);
			if(str_comp(pProp, "useClipping") == 0)
				return ReadBool(pValue, &Group.m_UseClipping, pError);
			if(str_comp(pProp, "clipX") == 0)
				return ReadInt(pValue, &Group.m_ClipX, pError);
			if(str_comp(pProp, "clipY") == 0)
				return ReadInt(pValue, &Group.m_ClipY, pError);
			if(str_comp(pProp, "clipW") == 0)
				return ReadInt(pValue, &Group.m_ClipW, pError);
			if(str_comp(pProp, "clipH") == 0)
				return ReadInt(pValue, &Group.m_ClipH, pError);
			*pError = std::string("a group has no '") + pProp + "'";
			return false;
		}

		bool SetLayerProp(CLayer &Layer, const char *pProp, const json_value *pValue, std::string *pError)
		{
			CLayerProperties &Properties = std::visit([](auto &Kind) -> CLayerProperties & { return Kind; }, Layer);
			if(str_comp(pProp, "name") == 0)
				return ReadString(pValue, &Properties.m_Name, pError);
			if(str_comp(pProp, "detail") == 0)
				return ReadBool(pValue, &Properties.m_Detail, pError);

			if(CTileLayer *pTiles = std::get_if<CTileLayer>(&Layer); pTiles != nullptr)
			{
				if(str_comp(pProp, "image") == 0)
					return ReadInt(pValue, &pTiles->m_Image, pError);
				if(str_comp(pProp, "color") == 0)
					return ReadColor(pValue, &pTiles->m_Color, pError);
				if(str_comp(pProp, "colorEnvelope") == 0)
					return ReadInt(pValue, &pTiles->m_ColorEnvelope, pError);
				if(str_comp(pProp, "colorEnvelopeOffset") == 0)
					return ReadInt(pValue, &pTiles->m_ColorEnvelopeOffset, pError);
				if(str_comp(pProp, "automapperConfig") == 0)
					return ReadInt(pValue, &pTiles->m_AutomapperConfig, pError);
				if(str_comp(pProp, "automapperSeed") == 0)
					return ReadInt(pValue, &pTiles->m_AutomapperSeed, pError);
				if(str_comp(pProp, "automapperAutomatic") == 0)
					return ReadBool(pValue, &pTiles->m_AutomapperAutomatic, pError);
			}
			else if(CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Layer); pQuads != nullptr)
			{
				if(str_comp(pProp, "image") == 0)
					return ReadInt(pValue, &pQuads->m_Image, pError);
			}
			else if(CSoundLayer *pSounds = std::get_if<CSoundLayer>(&Layer); pSounds != nullptr)
			{
				if(str_comp(pProp, "sound") == 0)
					return ReadInt(pValue, &pSounds->m_Sound, pError);
			}
			*pError = std::string("this layer has no '") + pProp + "'";
			return false;
		}

		/**
		 * The thirteen construct operations by name, in the order the editor
		 * in the client offers them.
		 */
		bool ReadGameTile(const char *pName, EGameTile *pOut)
		{
			static const struct
			{
				const char *m_pName;
				EGameTile m_Tile;
			} s_aTiles[] = {
				{"air", EGameTile::AIR},
				{"hookable", EGameTile::HOOKABLE},
				{"death", EGameTile::DEATH},
				{"unhookable", EGameTile::UNHOOKABLE},
				{"hookthrough", EGameTile::HOOKTHROUGH},
				{"freeze", EGameTile::FREEZE},
				{"unfreeze", EGameTile::UNFREEZE},
				{"deepFreeze", EGameTile::DEEP_FREEZE},
				{"deepUnfreeze", EGameTile::DEEP_UNFREEZE},
				{"blueCheckTele", EGameTile::BLUE_CHECK_TELE},
				{"redCheckTele", EGameTile::RED_CHECK_TELE},
				{"liveFreeze", EGameTile::LIVE_FREEZE},
				{"liveUnfreeze", EGameTile::LIVE_UNFREEZE},
			};
			const auto *pFound = std::find_if(std::begin(s_aTiles), std::end(s_aTiles),
				[pName](const auto &Known) { return str_comp(pName, Known.m_pName) == 0; });
			if(pFound == std::end(s_aTiles))
				return false;
			*pOut = pFound->m_Tile;
			return true;
		}

		/** The four ways a quad is put into shape, by name. */
		bool ReadQuadShape(const char *pName, EQuadShape *pOut)
		{
			if(str_comp(pName, "square") == 0)
				*pOut = EQuadShape::SQUARE;
			else if(str_comp(pName, "aspect") == 0)
				*pOut = EQuadShape::ASPECT;
			else if(str_comp(pName, "centerPivot") == 0)
				*pOut = EQuadShape::CENTER_PIVOT;
			else if(str_comp(pName, "align") == 0)
				*pOut = EQuadShape::ALIGN;
			else
				return false;
			return true;
		}

		/** How long a side of a tile layer may be, as in the client. */
		constexpr int MAX_LAYER_SIDE = 100000;

		bool ReadTileLayerKind(const char *pKind, ETileLayerKind *pOut)
		{
			if(str_comp(pKind, "tiles") == 0)
				*pOut = ETileLayerKind::TILES;
			else if(str_comp(pKind, "game") == 0)
				*pOut = ETileLayerKind::GAME;
			else if(str_comp(pKind, "front") == 0)
				*pOut = ETileLayerKind::FRONT;
			else if(str_comp(pKind, "tele") == 0)
				*pOut = ETileLayerKind::TELE;
			else if(str_comp(pKind, "speedup") == 0)
				*pOut = ETileLayerKind::SPEEDUP;
			else if(str_comp(pKind, "switch") == 0)
				*pOut = ETileLayerKind::SWITCH;
			else if(str_comp(pKind, "tune") == 0)
				*pOut = ETileLayerKind::TUNE;
			else
				return false;
			return true;
		}

		/**
		 * How large a new tile layer is when the command does not say: the
		 * size of the game layer, because a physics layer that is not that
		 * size is not a physics layer, and a drawn layer that is not is a
		 * layer that stops where the map does not.
		 */
		void DefaultLayerSize(const CMapState &Map, int *pWidth, int *pHeight)
		{
			*pWidth = 50;
			*pHeight = 50;
			const std::optional<CLayerAddress> Game = FindGameLayer(Map);
			if(!Game.has_value())
				return;
			const CTileLayer *pGame = Map.TileLayer(Game->m_Group, Game->m_Layer);
			*pWidth = pGame->Width();
			*pHeight = pGame->Height();
		}
	} // namespace

	std::string Apply(CDocument &Document, const char *pJson)
	{
		const std::unique_ptr<json_value, decltype(&json_value_free)> pParsed(
			JsonParse(pJson, str_length(pJson)), json_value_free);
		if(pParsed == nullptr || pParsed->type != json_object)
			return Failed("The command is not a JSON object");

		CArguments Arguments(pParsed.get());
		const char *pOp = Arguments.Str("op", nullptr);
		if(pOp == nullptr)
			return Failed("The command has no 'op'");
		const CMapState &Map = Document.Map();
		// What this change is *of*, for the history to fold a run of them into
		// one entry - see `CDocument::Begin`. Nothing merges that does not say
		// so, so leaving it out is the ordinary case.
		const char *pMerge = Arguments.Str("merge", nullptr);

		// What a command does to the map is one transaction and therefore one
		// history entry - unless the interface already has one open, in which
		// case this joins it. Everything is read and checked before the
		// transaction starts, so that there is no half-made change to give up
		// on: a command that is refused leaves the map where it was.
		if(str_comp(pOp, "group.add") == 0)
		{
			const char *pName = Arguments.Str("name");
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			CGroup Group;
			Group.m_Name = pName;
			Document.Begin(Arguments.Str("label", "Add group"), pMerge);
			const size_t Index = AddGroup(Document, std::move(Group));
			Document.Commit();
			return Succeeded("group", (int)Index);
		}
		if(str_comp(pOp, "group.delete") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Delete group"), pMerge);
			DeleteGroup(Document, Group);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "group.move") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t To = Arguments.Index("to", Map.NumGroups());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Move group"), pMerge);
			const size_t Index = MoveGroup(Document, Group, To);
			Document.Commit();
			return Succeeded("group", (int)Index);
		}
		if(str_comp(pOp, "group.setProp") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const char *pProp = Arguments.Str("prop", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pProp == nullptr)
				return Failed("The command has no 'prop'");
			CGroup Changed = *Map.Group(Group);
			std::string Error;
			if(!SetGroupProp(Changed, pProp, json_object_get(pParsed.get(), "value"), &Error))
				return Failed(Error);
			Document.Begin(Arguments.Str("label", pProp), pMerge);
			Document.Edit().ReplaceGroup(Group, std::move(Changed));
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "layer.add") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const char *pType = Arguments.Str("type", "tiles");
			const char *pName = Arguments.Str("name");
			int Width, Height;
			DefaultLayerSize(Map, &Width, &Height);
			Width = Arguments.Int("width", Width);
			Height = Arguments.Int("height", Height);
			if(Arguments.Failed())
				return Failed(Arguments.Error());

			CLayer Layer;
			if(str_comp(pType, "tiles") == 0)
			{
				ETileLayerKind Kind;
				if(!ReadTileLayerKind(Arguments.Str("kind", "tiles"), &Kind))
					return Failed("There is no tile layer of that kind");
				if(Width <= 0 || Height <= 0)
					return Failed("A tile layer has to have a size");
				Layer = CTileLayer(Kind, Width, Height);
			}
			else if(str_comp(pType, "quads") == 0)
			{
				Layer = CQuadLayer();
			}
			else if(str_comp(pType, "sounds") == 0)
			{
				Layer = CSoundLayer();
			}
			else
			{
				return Failed("There is no layer of that type");
			}
			std::visit([pName](auto &Kind) { Kind.m_Name = pName; }, Layer);

			Document.Begin(Arguments.Str("label", "Add layer"), pMerge);
			const CLayerAddress Address = AddLayer(Document, Group, std::move(Layer));
			Document.Commit();
			return Succeeded("group", (int)Address.m_Group, "layer", (int)Address.m_Layer);
		}
		if(str_comp(pOp, "layer.delete") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Delete layer"), pMerge);
			DeleteLayer(Document, CLayerAddress{Group, Layer});
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "layer.move") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			const size_t ToGroup = Arguments.Index("toGroup", Map.NumGroups());
			// One past the end is where "to the bottom" is, and the target is
			// counted in the list the layer is no longer in - see `MoveLayer`.
			const size_t To = Arguments.Index("to", Arguments.Failed() ? 0 : Map.NumLayers(ToGroup), true);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Move layer"), pMerge);
			const CLayerAddress Address = MoveLayer(Document, CLayerAddress{Group, Layer}, CLayerAddress{ToGroup, To});
			Document.Commit();
			return Succeeded("group", (int)Address.m_Group, "layer", (int)Address.m_Layer);
		}
		if(str_comp(pOp, "layer.resize") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			const CTileLayer *pTiles = std::get_if<CTileLayer>(Map.Layer(Group, Layer));
			if(pTiles == nullptr)
				return Failed("that layer holds no tiles");
			const int Width = Arguments.Int("width", pTiles->Width());
			const int Height = Arguments.Int("height", pTiles->Height());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			// The same bounds the editor in the client offers, and for the
			// same reason: a layer is written out as a plain array of tiles,
			// so a size nobody could save is not one to let in.
			if(Width <= 0 || Height <= 0)
				return Failed("a layer has to be at least one tile");
			if(Width > MAX_LAYER_SIDE || Height > MAX_LAYER_SIDE)
				return Failed("a layer is at most 100000 tiles a side");
			Document.Begin(Arguments.Str("label", "Resize layer"), pMerge);
			ResizeLayer(Document, CLayerAddress{Group, Layer}, Width, Height);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "layer.type") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			const int X = Arguments.Int("x");
			const int Y = Arguments.Int("y");
			const char *pText = Arguments.Str("text", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pText == nullptr)
				return Failed("The command has no 'text'");
			const CTileLayer *pTiles = std::get_if<CTileLayer>(Map.Layer(Group, Layer));
			if(pTiles == nullptr)
				return Failed("that layer holds no tiles");
			if(!DrawsOwnTiles(pTiles->m_Kind))
				return Failed("a physics layer draws no letters");
			Document.Begin(Arguments.Str("label", "Type"), pMerge);
			const int Wrote = TypeText(Document, CLayerAddress{Group, Layer}, X, Y, pText);
			Document.Commit();
			return Succeeded("tiles", Wrote);
		}
		if(str_comp(pOp, "layer.constructGameTiles") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			const char *pTile = Arguments.Str("tile", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pTile == nullptr)
				return Failed("The command has no 'tile'");
			EGameTile Tile;
			if(!ReadGameTile(pTile, &Tile))
				return Failed(std::string("there is no game tile called '") + pTile + "'");
			if(!CanConstructGameTiles(Map, Group, Layer))
				return Failed("this layer does not lie over the game layer tile for tile");
			Document.Begin(Arguments.Str("label", "Construct game tiles"), pMerge);
			const int Written = ConstructGameTiles(Document, Group, Layer, Tile);
			Document.Commit();
			return Succeeded("tiles", Written);
		}
		if(str_comp(pOp, "layer.setProp") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			const char *pProp = Arguments.Str("prop", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pProp == nullptr)
				return Failed("The command has no 'prop'");
			CLayer Changed = *Map.Layer(Group, Layer);
			std::string Error;
			if(!SetLayerProp(Changed, pProp, json_object_get(pParsed.get(), "value"), &Error))
				return Failed(Error);
			Document.Begin(Arguments.Str("label", pProp), pMerge);
			Document.Edit().ReplaceLayer(Group, Layer, std::move(Changed));
			Document.Commit();
			return Succeeded();
		}

		if(str_comp(pOp, "quad.add") == 0 || str_comp(pOp, "quad.delete") == 0 ||
			str_comp(pOp, "quad.setPoint") == 0 || str_comp(pOp, "quad.setColor") == 0 ||
			str_comp(pOp, "quad.setTexcoord") == 0 || str_comp(pOp, "quad.shape") == 0 ||
			str_comp(pOp, "quad.carve") == 0 || str_comp(pOp, "quad.setProp") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			const CQuadLayer *pQuads = Arguments.Failed() ? nullptr : std::get_if<CQuadLayer>(Map.Layer(Group, Layer));
			if(pQuads == nullptr)
				return Failed("that layer holds no quads");
			const CLayerAddress Address{Group, Layer};

			if(str_comp(pOp, "quad.add") == 0)
			{
				// In world units, which is what the page has after turning a
				// click into a place; a tile is thirty-two of them.
				const int X = Arguments.Int("x");
				const int Y = Arguments.Int("y");
				const int Width = Arguments.Int("width", 64);
				const int Height = Arguments.Int("height", 64);
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				Document.Begin(Arguments.Str("label", "Add quad"), pMerge);
				const size_t Index = AddQuad(Document, Address, MakeQuad(X, Y, Width, Height));
				Document.Commit();
				return Succeeded("quad", (int)Index);
			}

			const size_t Quad = Arguments.Index("quad", pQuads->m_Quads.Size());
			if(Arguments.Failed())
				return Failed(Arguments.Error());

			if(str_comp(pOp, "quad.delete") == 0)
			{
				Document.Begin(Arguments.Str("label", "Delete quad"), pMerge);
				DeleteQuad(Document, Address, Quad);
				Document.Commit();
				return Succeeded();
			}

			if(str_comp(pOp, "quad.carve") == 0)
			{
				// Four places inside the quad, in the order somebody clicked
				// them, in world units.
				const json_value *pPoints = json_object_get(pParsed.get(), "points");
				if(pPoints->type != json_array || pPoints->u.array.length != 8)
					return Failed("a knife cuts along four places, which is eight numbers");
				vec2 aRing[4];
				for(size_t Corner = 0; Corner < 4; ++Corner)
				{
					// Whole world units, the same as everywhere else a place
					// leaves the page: a thirty-second of a tile is finer than
					// anybody can click.
					const json_value *pX = json_array_get(pPoints, (int)Corner * 2);
					const json_value *pY = json_array_get(pPoints, (int)Corner * 2 + 1);
					if(pX->type != json_integer || pY->type != json_integer)
						return Failed("a place is two whole numbers");
					aRing[Corner] = vec2((float)json_int_get(pX), (float)json_int_get(pY));
				}
				for(const vec2 &Place : aRing)
				{
					if(!PointInQuad(pQuads->m_Quads[Quad], Place))
						return Failed("a knife cuts inside the quad, not outside it");
				}
				Document.Begin(Arguments.Str("label", "Carve quad"), pMerge);
				const size_t Made = CarveQuad(Document, Address, Quad, aRing);
				Document.Commit();
				return Succeeded("quad", (int)Made);
			}

			if(str_comp(pOp, "quad.shape") == 0)
			{
				const char *pShape = Arguments.Str("shape", nullptr);
				const int Grid = Arguments.Int("grid", 32);
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				if(pShape == nullptr)
					return Failed("The command has no 'shape'");
				EQuadShape Shape;
				if(!ReadQuadShape(pShape, &Shape))
					return Failed(std::string("there is no way of shaping a quad called '") + pShape + "'");
				if(Grid <= 0)
					return Failed("a grid is at least one unit wide");
				Document.Begin(Arguments.Str("label", pShape), pMerge);
				if(!ShapeQuad(Document, Address, Quad, Shape, Grid))
				{
					Document.Abort();
					return Failed("this layer is drawn with no picture, so it has no proportions");
				}
				Document.Commit();
				return Succeeded();
			}

			CQuad Changed = pQuads->m_Quads[Quad];
			if(str_comp(pOp, "quad.setTexcoord") == 0)
			{
				// Where a corner sits in the picture, in the same numbers the
				// file holds and the editor in the client shows: 1024 is the
				// whole picture across, so 0 and 1024 are its two edges and
				// anything else repeats or crops it.
				const size_t Corner = Arguments.Index("corner", std::size(Changed.m_aTexcoords));
				const int U = Arguments.Int("u");
				const int V = Arguments.Int("v");
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				Changed.m_aTexcoords[Corner] = CPoint{U, V};
			}
			else if(str_comp(pOp, "quad.setPoint") == 0)
			{
				// Five points: four corners in the order the file keeps them,
				// and the pivot it turns about.
				const size_t Point = Arguments.Index("point", std::size(Changed.m_aPoints));
				const int X = Arguments.Int("x");
				const int Y = Arguments.Int("y");
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				// The pivot carries the corners with it, because that is what
				// a pivot is: dragging it moves the quad rather than bending
				// it out of shape.
				if(Point + 1 == std::size(Changed.m_aPoints))
				{
					const int MovedX = i2fx(X) - Changed.m_aPoints[Point].x;
					const int MovedY = i2fx(Y) - Changed.m_aPoints[Point].y;
					for(CPoint &Corner : Changed.m_aPoints)
					{
						Corner.x += MovedX;
						Corner.y += MovedY;
					}
				}
				else
				{
					Changed.m_aPoints[Point] = CPoint{i2fx(X), i2fx(Y)};
				}
			}
			else if(str_comp(pOp, "quad.setColor") == 0)
			{
				const size_t Corner = Arguments.Index("corner", std::size(Changed.m_aColors));
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				CColor Color;
				std::string Error;
				if(!ReadQuadColor(json_object_get(pParsed.get(), "value"), &Color, &Error))
					return Failed(Error);
				Changed.m_aColors[Corner] = Color;
			}
			else
			{
				const char *pProp = Arguments.Str("prop", nullptr);
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				if(pProp == nullptr)
					return Failed("The command has no 'prop'");
				std::string Error;
				if(!SetQuadProp(Changed, pProp, json_object_get(pParsed.get(), "value"), Map.NumEnvelopes(), &Error))
					return Failed(Error);
			}
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", str_comp(pOp, "quad.setPoint") == 0 ? "Move quad" : "Quad"), pMerge);
			SetQuad(Document, Address, Quad, Changed);
			Document.Commit();
			return Succeeded();
		}

		if(str_comp(pOp, "source.add") == 0 || str_comp(pOp, "source.delete") == 0 ||
			str_comp(pOp, "source.setPoint") == 0 || str_comp(pOp, "source.setProp") == 0)
		{
			const size_t Group = Arguments.Index("group", Map.NumGroups());
			const size_t Layer = Arguments.Index("layer", Arguments.Failed() ? 0 : Map.NumLayers(Group));
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			const CSoundLayer *pSounds = std::get_if<CSoundLayer>(Map.Layer(Group, Layer));
			if(pSounds == nullptr)
				return Failed("that layer holds no sounds");
			const CLayerAddress Address{Group, Layer};

			if(str_comp(pOp, "source.add") == 0)
			{
				// In world units, the same as a quad: a page turns a click
				// into a place and hands the place over.
				const int X = Arguments.Int("x");
				const int Y = Arguments.Int("y");
				const int Radius = Arguments.Int("radius", 96);
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				if(Radius <= 0)
					return Failed("a source is heard within something");
				Document.Begin(Arguments.Str("label", "Add sound source"), pMerge);
				const size_t Index = AddSoundSource(Document, Address, MakeSoundSource(X, Y, Radius));
				Document.Commit();
				return Succeeded("source", (int)Index);
			}

			const size_t Source = Arguments.Index("source", pSounds->m_Sources.Size());
			if(Arguments.Failed())
				return Failed(Arguments.Error());

			if(str_comp(pOp, "source.delete") == 0)
			{
				Document.Begin(Arguments.Str("label", "Delete sound source"), pMerge);
				DeleteSoundSource(Document, Address, Source);
				Document.Commit();
				return Succeeded();
			}

			CSoundSource Changed = pSounds->m_Sources[Source];
			if(str_comp(pOp, "source.setPoint") == 0)
			{
				const int X = Arguments.Int("x");
				const int Y = Arguments.Int("y");
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				Changed.m_Position = CPoint{i2fx(X), i2fx(Y)};
			}
			else
			{
				const char *pProp = Arguments.Str("prop", nullptr);
				if(Arguments.Failed())
					return Failed(Arguments.Error());
				if(pProp == nullptr)
					return Failed("The command has no 'prop'");
				std::string Error;
				if(!SetSoundProp(Changed, pProp, json_object_get(pParsed.get(), "value"), Map.NumEnvelopes(), &Error))
					return Failed(Error);
			}

			Document.Begin(Arguments.Str("label", str_comp(pOp, "source.setPoint") == 0 ? "Move sound source" : "Sound source"), pMerge);
			SetSoundSource(Document, Address, Source, Changed);
			Document.Commit();
			return Succeeded();
		}

		if(str_comp(pOp, "envelope.add") == 0)
		{
			const char *pName = Arguments.Str("name");
			// One channel is a sound, three are a position, four a colour;
			// nothing else is an envelope any renderer knows how to read.
			const int Channels = Arguments.Int("channels", 4);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(Channels != 1 && Channels != 3 && Channels != 4)
				return Failed("an envelope has one, three or four channels");
			CEnvelope Envelope;
			Envelope.m_Name = pName;
			Envelope.m_Channels = Channels;
			Document.Begin(Arguments.Str("label", "Add envelope"), pMerge);
			const size_t Index = AddEnvelope(Document, std::move(Envelope));
			Document.Commit();
			return Succeeded("envelope", (int)Index);
		}
		if(str_comp(pOp, "envelope.delete") == 0)
		{
			const size_t Envelope = Arguments.Index("envelope", Map.NumEnvelopes());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Delete envelope"), pMerge);
			DeleteEnvelope(Document, Envelope);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "envelope.deleteUnused") == 0)
		{
			const std::vector<size_t> vUnused = UnusedEnvelopes(Map);
			if(vUnused.empty())
				return Failed("every envelope is used by something");
			// One entry for all of them: taking six envelopes out is one
			// thing somebody did, and six steps to undo would be six times
			// the surprise.
			Document.Begin(Arguments.Str("label", "Delete unused envelopes"), pMerge);
			// Highest first, so that the places of the ones still to go do
			// not move under the loop.
			for(const size_t Envelope : vUnused)
				DeleteEnvelope(Document, Envelope);
			Document.Commit();
			return Succeeded("envelopes", (int)vUnused.size());
		}
		if(str_comp(pOp, "envelope.setProp") == 0)
		{
			const size_t Envelope = Arguments.Index("envelope", Map.NumEnvelopes());
			const char *pProp = Arguments.Str("prop", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pProp == nullptr)
				return Failed("The command has no 'prop'");
			CEnvelope Changed = *Map.Envelope(Envelope);
			std::string Error;
			if(!SetEnvelopeProp(Changed, pProp, json_object_get(pParsed.get(), "value"), &Error))
				return Failed(Error);
			Document.Begin(Arguments.Str("label", pProp), pMerge);
			Document.Edit().ReplaceEnvelope(Envelope, std::move(Changed));
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "info.setProp") == 0)
		{
			const char *pProp = Arguments.Str("prop", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pProp == nullptr)
				return Failed("The command has no 'prop'");
			const json_value *pValue = json_object_get(pParsed.get(), "value");
			if(pValue->type != json_string)
				return Failed("that is a word");
			CMapInfo Changed = Map.m_Info;
			if(str_comp(pProp, "author") == 0)
				Changed.m_Author = pValue->u.string.ptr;
			else if(str_comp(pProp, "mapVersion") == 0)
				Changed.m_MapVersion = pValue->u.string.ptr;
			else if(str_comp(pProp, "credits") == 0)
				Changed.m_Credits = pValue->u.string.ptr;
			else if(str_comp(pProp, "license") == 0)
				Changed.m_License = pValue->u.string.ptr;
			else
				return Failed(std::string("a map has no '") + pProp + "'");
			Document.Begin(Arguments.Str("label", pProp), pMerge);
			Document.Edit().m_Info = std::move(Changed);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "info.settings.add") == 0 || str_comp(pOp, "info.settings.set") == 0 ||
			str_comp(pOp, "info.settings.delete") == 0)
		{
			const size_t Count = Map.m_Info.m_Settings.Size();
			const bool Adding = str_comp(pOp, "info.settings.add") == 0;
			const size_t Line = Adding ? Count : Arguments.Index("line", Count);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			CMapInfo Changed = Map.m_Info;
			std::vector<std::string> &vSettings = Changed.m_Settings.Mutable();
			if(str_comp(pOp, "info.settings.delete") == 0)
			{
				vSettings.erase(vSettings.begin() + Line);
				Document.Begin(Arguments.Str("label", "Delete setting"), pMerge);
			}
			else
			{
				const json_value *pValue = json_object_get(pParsed.get(), "value");
				if(pValue->type != json_string)
					return Failed("a setting is a line of console");
				// A line with a newline in it would come back as two lines,
				// and then the map would not be the map that was written.
				const char *pLine = pValue->u.string.ptr;
				if(str_find(pLine, "\n") != nullptr || str_find(pLine, "\r") != nullptr)
					return Failed("a setting is one line");
				if(Adding)
					vSettings.emplace_back(pLine);
				else
					vSettings[Line] = pLine;
				Document.Begin(Arguments.Str("label", Adding ? "Add setting" : "Setting"), pMerge);
			}
			Document.Edit().m_Info = std::move(Changed);
			Document.Commit();
			return Adding ? Succeeded("line", (int)Line) : Succeeded();
		}
		if(str_comp(pOp, "image.add") == 0)
		{
			// Only a picture that lies beside the map: the pixels of an
			// embedded one do not go through JSON, they go through
			// `CMapEditor::AddImage` as the bytes they are.
			const char *pName = Arguments.Str("name", nullptr);
			const int Width = Arguments.Int("width", 0);
			const int Height = Arguments.Int("height", 0);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pName == nullptr || pName[0] == '\0')
				return Failed("a picture needs a name");
			CImage Image;
			Image.m_Name = pName;
			Image.m_External = true;
			Image.m_Width = std::max(0, Width);
			Image.m_Height = std::max(0, Height);
			Document.Begin(Arguments.Str("label", "Add image"), pMerge);
			const size_t Index = AddImage(Document, std::move(Image));
			Document.Commit();
			return Succeeded("image", (int)Index);
		}
		if(str_comp(pOp, "image.delete") == 0)
		{
			const size_t Image = Arguments.Index("image", Map.NumImages());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Delete image"), pMerge);
			DeleteImage(Document, Image);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "image.setProp") == 0)
		{
			const size_t Index = Arguments.Index("image", Map.NumImages());
			const char *pProp = Arguments.Str("prop", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pProp == nullptr)
				return Failed("The command has no 'prop'");
			CImage Changed = *Map.Image(Index);
			const json_value *pValue = json_object_get(pParsed.get(), "value");
			if(str_comp(pProp, "name") == 0)
			{
				if(pValue->type != json_string || pValue->u.string.length == 0)
					return Failed("a name is a word");
				Changed.m_Name = pValue->u.string.ptr;
			}
			else if(str_comp(pProp, "external") == 0)
			{
				if(pValue->type != json_boolean)
					return Failed("that is yes or no");
				const bool External = pValue->u.boolean != 0;
				// Going the other way needs pixels, and pixels do not come
				// through here - a picture that has none cannot be embedded
				// by being called embedded.
				if(!External && Changed.m_Data.Empty())
					return Failed("that picture has no pixels of its own");
				Changed.m_External = External;
				if(External)
					Changed.m_Data = CSharedList<uint8_t>();
			}
			else
			{
				return Failed(std::string("a picture has no '") + pProp + "'");
			}
			Document.Begin(Arguments.Str("label", pProp), pMerge);
			SetImage(Document, Index, std::move(Changed));
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "sound.add") == 0)
		{
			// Only a sound that lies beside the map: the bytes of an embedded
			// one do not go through JSON, they go through
			// `CMapEditor::AddSound` as the bytes they are.
			const char *pName = Arguments.Str("name", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pName == nullptr || pName[0] == '\0')
				return Failed("a sound needs a name");
			CSound Sound;
			Sound.m_Name = pName;
			Sound.m_External = true;
			Document.Begin(Arguments.Str("label", "Add sound"), pMerge);
			const size_t Index = AddSound(Document, std::move(Sound));
			Document.Commit();
			return Succeeded("sound", (int)Index);
		}
		if(str_comp(pOp, "sound.delete") == 0)
		{
			const size_t Sound = Arguments.Index("sound", Map.NumSounds());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Delete sound"), pMerge);
			DeleteSound(Document, Sound);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "sound.setProp") == 0)
		{
			const size_t Index = Arguments.Index("sound", Map.NumSounds());
			const char *pProp = Arguments.Str("prop", nullptr);
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			if(pProp == nullptr)
				return Failed("The command has no 'prop'");
			CSound Changed = *Map.Sound(Index);
			const json_value *pValue = json_object_get(pParsed.get(), "value");
			if(str_comp(pProp, "name") == 0)
			{
				if(pValue->type != json_string || pValue->u.string.length == 0)
					return Failed("a name is a word");
				Changed.m_Name = pValue->u.string.ptr;
			}
			else if(str_comp(pProp, "external") == 0)
			{
				if(pValue->type != json_boolean)
					return Failed("that is yes or no");
				const bool External = pValue->u.boolean != 0;
				// The same rule as a picture: going the other way needs bytes,
				// and bytes do not come through here.
				if(!External && Changed.m_Data.Empty())
					return Failed("that sound has no bytes of its own");
				Changed.m_External = External;
				if(External)
					Changed.m_Data = CSharedList<uint8_t>();
			}
			else
			{
				return Failed(std::string("a sound has no '") + pProp + "'");
			}
			Document.Begin(Arguments.Str("label", pProp), pMerge);
			SetSound(Document, Index, std::move(Changed));
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "envelope.point.add") == 0)
		{
			const size_t Envelope = Arguments.Index("envelope", Map.NumEnvelopes());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			CEnvPoint_runtime Point = {};
			Point.m_Curvetype = CURVETYPE_LINEAR;
			std::string Error;
			if(!ReadEnvelopePoint(pParsed.get(), Map.Envelope(Envelope)->m_Channels, &Point, &Error))
				return Failed(Error);
			Document.Begin(Arguments.Str("label", "Add point"), pMerge);
			const size_t Index = AddEnvelopePoint(Document, Envelope, Point);
			Document.Commit();
			return Succeeded("point", (int)Index);
		}
		if(str_comp(pOp, "envelope.point.delete") == 0)
		{
			const size_t Envelope = Arguments.Index("envelope", Map.NumEnvelopes());
			const size_t Point = Arguments.Index("point", Arguments.Failed() ? 0 : Map.Envelope(Envelope)->m_Points.Size());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.Begin(Arguments.Str("label", "Delete point"), pMerge);
			DeleteEnvelopePoint(Document, Envelope, Point);
			Document.Commit();
			return Succeeded();
		}
		if(str_comp(pOp, "envelope.point.set") == 0)
		{
			const size_t Envelope = Arguments.Index("envelope", Map.NumEnvelopes());
			const size_t Point = Arguments.Index("point", Arguments.Failed() ? 0 : Map.Envelope(Envelope)->m_Points.Size());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			// Read onto the point as it stands, so that a command which says
			// only a time leaves the values where they were.
			CEnvPoint_runtime Changed = Map.Envelope(Envelope)->m_Points[Point];
			std::string Error;
			if(!ReadEnvelopePoint(pParsed.get(), Map.Envelope(Envelope)->m_Channels, &Changed, &Error))
				return Failed(Error);
			Document.Begin(Arguments.Str("label", "Move point"), pMerge);
			const size_t Index = SetEnvelopePoint(Document, Envelope, Point, Changed);
			Document.Commit();
			return Succeeded("point", (int)Index);
		}

		// Stepping through the versions is not a change to the map, so it is
		// not something a change may contain either: what an undo inside a
		// half-made stroke would mean is a question nobody has answered.
		if(str_comp(pOp, "history.undo") == 0 || str_comp(pOp, "history.redo") == 0 || str_comp(pOp, "history.jump") == 0)
		{
			if(Document.IsEditing())
				return Failed("The map cannot be stepped about while it is being changed");
			if(str_comp(pOp, "history.undo") == 0)
			{
				Document.Undo();
				return Succeeded("current", (int)Document.History().CurrentIndex());
			}
			if(str_comp(pOp, "history.redo") == 0)
			{
				Document.Redo();
				return Succeeded("current", (int)Document.History().CurrentIndex());
			}
			const size_t Index = Arguments.Index("index", Document.History().NumEntries());
			if(Arguments.Failed())
				return Failed(Arguments.Error());
			Document.JumpTo(Index);
			return Succeeded("current", (int)Document.History().CurrentIndex());
		}

		return Failed(std::string("There is no command '") + pOp + "'");
	}
} // namespace map_document
