#include "command.h"

#include <base/str.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>

#include <game/map/document/document.h>
#include <game/map/document/structure.h>

#include <algorithm>
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
