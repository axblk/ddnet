#include <base/str.h>

#include <engine/shared/json.h>

#include <game/map/document/command.h>
#include <game/map/document/document.h>
#include <game/map/document/structure.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace map_document;

// What the interface asks of the map. Every test here goes through the text,
// because that is the only way the interface has of asking - a test that
// called the structure functions directly would prove nothing about the
// entrance they are reached through.

namespace
{
	using CJson = std::unique_ptr<json_value, decltype(&json_value_free)>;

	CMapState TwoGroups()
	{
		CMapState Map;
		CGroup Background;
		Background.m_Name = "background";
		CTileLayer Sky(ETileLayerKind::TILES, 8, 4);
		Sky.m_Name = "sky";
		Background.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Sky)));
		Map.AddGroup(std::move(Background));

		CGroup Game;
		Game.m_Name = "game";
		CTileLayer Physics(ETileLayerKind::GAME, 16, 9);
		Physics.m_Name = "Game";
		Game.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Physics)));
		Map.AddGroup(std::move(Game));
		return Map;
	}

	class CCommands
	{
	public:
		explicit CCommands(CMapState Map) :
			m_Document(std::move(Map)) {}

		/** Sends a command and says it worked. */
		CJson Ok(const std::string &Json)
		{
			CJson pAnswer = Send(Json);
			EXPECT_TRUE(json_boolean_get(json_object_get(pAnswer.get(), "ok")))
				<< Json << " -> " << json_string_get(json_object_get(pAnswer.get(), "error"));
			return pAnswer;
		}

		/** Sends a command that has to be refused, and says why it was. */
		std::string Refused(const std::string &Json)
		{
			CJson pAnswer = Send(Json);
			EXPECT_FALSE(json_boolean_get(json_object_get(pAnswer.get(), "ok"))) << Json;
			return json_string_get(json_object_get(pAnswer.get(), "error"));
		}

		CDocument m_Document;

	private:
		CJson Send(const std::string &Json)
		{
			const std::string Answer = Apply(m_Document, Json.c_str());
			CJson pAnswer(JsonParse(Answer.c_str(), Answer.size()), json_value_free);
			EXPECT_NE(pAnswer, nullptr) << Answer;
			return pAnswer;
		}
	};

	int Number(const CJson &pAnswer, const char *pName)
	{
		return json_int_get(json_object_get(pAnswer.get(), pName));
	}
} // namespace

TEST(Command, AGroupIsAddedAndSaysWhereItWent)
{
	CCommands Commands(TwoGroups());
	const CJson pAnswer = Commands.Ok(R"({"op":"group.add","name":"new"})");
	EXPECT_EQ(Number(pAnswer, "group"), 2);
	ASSERT_EQ(Commands.m_Document.Map().NumGroups(), 3u);
	EXPECT_EQ(Commands.m_Document.Map().Group(2)->m_Name, "new");
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 2u);
	EXPECT_EQ(Commands.m_Document.History().Entry(1).m_Label, "Add group");
}

TEST(Command, ACommandMayNameItsOwnHistoryEntry)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"group.add","label":"Neue Gruppe"})");
	EXPECT_EQ(Commands.m_Document.History().Entry(1).m_Label, "Neue Gruppe");
}

TEST(Command, GroupsAreDeletedAndReordered)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"group.move","group":0,"to":1})");
	EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_Name, "game");
	Commands.Ok(R"({"op":"group.delete","group":0})");
	ASSERT_EQ(Commands.m_Document.Map().NumGroups(), 1u);
	EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_Name, "background");
}

TEST(Command, AGroupPropertyIsSetByName)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"parallaxX","value":50})");
	Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"useClipping","value":true})");
	Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"name","value":"himmel"})");
	const CGroup *pGroup = Commands.m_Document.Map().Group(0);
	EXPECT_EQ(pGroup->m_ParallaxX, 50);
	EXPECT_TRUE(pGroup->m_UseClipping);
	EXPECT_EQ(pGroup->m_Name, "himmel");
	EXPECT_EQ(Commands.m_Document.History().Entry(1).m_Label, "parallaxX");
}

TEST(Command, ALayerIsAddedWithTheSizeOfTheGameLayer)
{
	CCommands Commands(TwoGroups());
	const CJson pAnswer = Commands.Ok(R"({"op":"layer.add","group":0,"type":"tiles","kind":"front","name":"front"})");
	EXPECT_EQ(Number(pAnswer, "group"), 0);
	EXPECT_EQ(Number(pAnswer, "layer"), 1);
	const CTileLayer *pLayer = Commands.m_Document.Map().TileLayer(0, 1);
	EXPECT_EQ(pLayer->m_Kind, ETileLayerKind::FRONT);
	EXPECT_EQ(pLayer->Width(), 16);
	EXPECT_EQ(pLayer->Height(), 9);
	EXPECT_EQ(pLayer->m_Name, "front");
}

TEST(Command, ALayerMayBeAskedForAtASizeOfItsOwn)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"layer.add","group":0,"type":"tiles","width":3,"height":7})");
	EXPECT_EQ(Commands.m_Document.Map().TileLayer(0, 1)->Width(), 3);
	EXPECT_EQ(Commands.m_Document.Map().TileLayer(0, 1)->Height(), 7);
}

TEST(Command, QuadAndSoundLayersCarryNoSize)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"layer.add","group":0,"type":"quads","name":"q"})");
	Commands.Ok(R"({"op":"layer.add","group":0,"type":"sounds","name":"s"})");
	EXPECT_TRUE(std::holds_alternative<CQuadLayer>(*Commands.m_Document.Map().Layer(0, 1)));
	EXPECT_TRUE(std::holds_alternative<CSoundLayer>(*Commands.m_Document.Map().Layer(0, 2)));
}

TEST(Command, ALayerIsMovedIntoAnotherGroup)
{
	CCommands Commands(TwoGroups());
	const CJson pAnswer = Commands.Ok(R"({"op":"layer.move","group":0,"layer":0,"toGroup":1,"to":0})");
	EXPECT_EQ(Number(pAnswer, "group"), 1);
	EXPECT_EQ(Number(pAnswer, "layer"), 0);
	EXPECT_EQ(Commands.m_Document.Map().NumLayers(0), 0u);
	ASSERT_EQ(Commands.m_Document.Map().NumLayers(1), 2u);
	EXPECT_EQ(LayerProperties(*Commands.m_Document.Map().Layer(1, 0)).m_Name, "sky");
}

TEST(Command, ALayerPropertyIsSetByNameAndKind)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"layer.setProp","group":0,"layer":0,"prop":"color","value":[1,2,3,4]})");
	Commands.Ok(R"({"op":"layer.setProp","group":0,"layer":0,"prop":"detail","value":true})");
	const CTileLayer *pLayer = Commands.m_Document.Map().TileLayer(0, 0);
	EXPECT_EQ(pLayer->m_Color.r, 1);
	EXPECT_EQ(pLayer->m_Color.a, 4);
	EXPECT_TRUE(pLayer->m_Detail);

	Commands.Ok(R"({"op":"layer.add","group":0,"type":"quads"})");
	EXPECT_EQ(Commands.Refused(R"({"op":"layer.setProp","group":0,"layer":1,"prop":"color","value":[1,2,3,4]})"),
		"this layer has no 'color'");
}

TEST(Command, AChangeThatIsRefusedLeavesTheMapWhereItWas)
{
	CCommands Commands(TwoGroups());
	EXPECT_EQ(Commands.Refused(R"({"op":"group.setProp","group":0,"prop":"parallaxX","value":"fifty"})"),
		"that property is a whole number");
	EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_ParallaxX, 100);
	// And nothing was written down either.
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 1u);
}

TEST(Command, WhatIsNotThereIsSaidToNotBeThere)
{
	CCommands Commands(TwoGroups());
	EXPECT_EQ(Commands.Refused(R"({"op":"group.delete","group":7})"), "'group' is 7, which is not there");
	EXPECT_EQ(Commands.Refused(R"({"op":"layer.delete","group":0,"layer":3})"), "'layer' is 3, which is not there");
	EXPECT_EQ(Commands.Refused(R"({"op":"nonsense"})"), "There is no command 'nonsense'");
	EXPECT_EQ(Commands.Refused(R"({"group":0})"), "The command has no 'op'");
	EXPECT_EQ(Commands.Refused("not json at all"), "The command is not a JSON object");
}

TEST(Command, StepsThroughTheVersionsGoThroughTheSameEntrance)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"group.add","name":"new"})");
	Commands.Ok(R"({"op":"group.add","name":"newer"})");
	EXPECT_EQ(Number(Commands.Ok(R"({"op":"history.undo"})"), "current"), 1);
	EXPECT_EQ(Commands.m_Document.Map().NumGroups(), 3u);
	EXPECT_EQ(Number(Commands.Ok(R"({"op":"history.redo"})"), "current"), 2);
	EXPECT_EQ(Number(Commands.Ok(R"({"op":"history.jump","index":0})"), "current"), 0);
	EXPECT_EQ(Commands.m_Document.Map().NumGroups(), 2u);
	// One past the last entry is not an entry.
	EXPECT_EQ(Commands.Refused(R"({"op":"history.jump","index":3})"), "'index' is 3, which is not there");
}

TEST(Command, UndoAtTheStartIsNotAnErrorAndChangesNothing)
{
	CCommands Commands(TwoGroups());
	EXPECT_EQ(Number(Commands.Ok(R"({"op":"history.undo"})"), "current"), 0);
}

TEST(Command, StepsAreRefusedWhileAChangeIsHalfMade)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"group.add"})");
	Commands.m_Document.Begin("Dragging");
	EXPECT_EQ(Commands.Refused(R"({"op":"history.undo"})"),
		"The map cannot be stepped about while it is being changed");
	Commands.m_Document.Abort();
}

TEST(Command, CommandsInsideAnOpenChangeAreOneEntry)
{
	CCommands Commands(TwoGroups());
	// What a slider does: the dragging opens the change, every step is a
	// command, and letting go closes it.
	Commands.m_Document.Begin("Parallaxe");
	for(int Parallax = 100; Parallax > 40; Parallax -= 10)
	{
		char aCommand[128];
		str_format(aCommand, sizeof(aCommand), R"({"op":"group.setProp","group":0,"prop":"parallaxX","value":%d})", Parallax);
		Commands.Ok(aCommand);
		// Every step is already what is drawn.
		EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_ParallaxX, Parallax);
	}
	Commands.m_Document.Commit();

	ASSERT_EQ(Commands.m_Document.History().NumEntries(), 2u);
	EXPECT_EQ(Commands.m_Document.History().Entry(1).m_Label, "Parallaxe");
	EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_ParallaxX, 50);
}

TEST(Command, AChangeThatChangesNothingIsNotAnEntry)
{
	CCommands Commands(TwoGroups());
	Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"parallaxX","value":100})");
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 1u);
}

// A number field stepped with its arrows sends one command per step. Ten
// steps are one change of one property, and the command says so by naming
// what it is of - which has to be the property and the thing it belongs to,
// or two layers' names would be the same change.
TEST(Command, ARunOfChangesOfTheSameThingIsOneEntry)
{
	CCommands Commands(TwoGroups());
	for(int Parallax = 100; Parallax > 90; --Parallax)
	{
		Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"parallaxX","merge":"group:0:parallaxX","value":)" +
			    std::to_string(Parallax) + "}");
	}
	EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_ParallaxX, 91);
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 2u);
	EXPECT_EQ(Commands.m_Document.History().Entry(1).m_Label, "parallaxX");

	// The other group's parallax is another thing, and so is another property
	// of the same group.
	Commands.Ok(R"({"op":"group.setProp","group":1,"prop":"parallaxX","merge":"group:1:parallaxX","value":50})");
	Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"parallaxY","merge":"group:0:parallaxY","value":50})");
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 4u);

	// And going back goes back over the whole run at once.
	ASSERT_TRUE(Commands.m_Document.Undo());
	ASSERT_TRUE(Commands.m_Document.Undo());
	ASSERT_TRUE(Commands.m_Document.Undo());
	EXPECT_EQ(Commands.m_Document.Map().Group(0)->m_ParallaxX, 100);
}

TEST(Command, WithoutSayingSoNothingMerges)
{
	CCommands Commands(TwoGroups());
	// From 99, because a group starts at 100 and setting it to what it
	// already is changes nothing and writes nothing.
	for(int Parallax = 99; Parallax > 94; --Parallax)
	{
		Commands.Ok(R"({"op":"group.setProp","group":0,"prop":"parallaxX","value":)" + std::to_string(Parallax) + "}");
	}
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 6u);
}

// Envelopes through the text. What they are made of is points in time order,
// so what is tested here is that the order is the document's business and not
// the caller's, and that taking one away puts right what was bound to it.

namespace
{
	CMapState WithAnEnvelope()
	{
		CMapState Map = TwoGroups();
		CEnvelope Envelope;
		Envelope.m_Name = "colour";
		Map.AddEnvelope(std::move(Envelope));
		CTileLayer Bound = *Map.TileLayer(0, 0);
		Bound.m_ColorEnvelope = 0;
		Map.ReplaceLayer(0, 0, std::move(Bound));
		return Map;
	}
} // namespace

TEST(Command, AnEnvelopeIsAddedAndItsPointsGoInTimeOrder)
{
	CCommands Commands(WithAnEnvelope());
	const CJson pAdded = Commands.Ok(R"({"op":"envelope.add","name":"moving","channels":3})");
	EXPECT_EQ(Number(pAdded, "envelope"), 1);
	ASSERT_EQ(Commands.m_Document.Map().NumEnvelopes(), 2u);
	EXPECT_EQ(Commands.m_Document.Map().Envelope(1)->m_Channels, 3);

	EXPECT_EQ(Number(Commands.Ok(R"({"op":"envelope.point.add","envelope":1,"time":2000,"values":[1,2,3]})"), "point"), 0);
	EXPECT_EQ(Number(Commands.Ok(R"({"op":"envelope.point.add","envelope":1,"time":1000,"values":[4,5,6]})"), "point"), 0);
	const CEnvelope *pEnvelope = Commands.m_Document.Map().Envelope(1);
	ASSERT_EQ(pEnvelope->m_Points.Size(), 2u);
	EXPECT_EQ(pEnvelope->m_Points[0].m_Time.GetInternal(), 1000);
	EXPECT_EQ(pEnvelope->m_Points[1].m_aValues[0], 1);

	// Dragged past its neighbour, which the answer says.
	EXPECT_EQ(Number(Commands.Ok(R"({"op":"envelope.point.set","envelope":1,"point":0,"time":3000})"), "point"), 1);
	EXPECT_EQ(Commands.m_Document.Map().Envelope(1)->m_Points[1].m_aValues[0], 4) << "and it took its values along";

	Commands.Ok(R"({"op":"envelope.setProp","envelope":1,"prop":"synchronized","value":true})");
	EXPECT_TRUE(Commands.m_Document.Map().Envelope(1)->m_Synchronized);
}

TEST(Command, AnEnvelopeThatIsTakenAwayIsTakenOffWhatUsedIt)
{
	CCommands Commands(WithAnEnvelope());
	Commands.Ok(R"({"op":"envelope.add","name":"second"})");
	// The layer is bound to the first one; taking the second one away leaves
	// it where it is.
	Commands.Ok(R"({"op":"envelope.delete","envelope":1})");
	EXPECT_EQ(Commands.m_Document.Map().TileLayer(0, 0)->m_ColorEnvelope, 0);
	// And taking away the one it uses leaves it bound to nothing.
	Commands.Ok(R"({"op":"envelope.delete","envelope":0})");
	EXPECT_EQ(Commands.m_Document.Map().NumEnvelopes(), 0u);
	EXPECT_EQ(Commands.m_Document.Map().TileLayer(0, 0)->m_ColorEnvelope, -1);
}

TEST(Command, AnEnvelopeCommandThatMakesNoSenseIsRefused)
{
	CCommands Commands(WithAnEnvelope());
	EXPECT_EQ(Commands.Refused(R"({"op":"envelope.add","channels":2})"), "an envelope has one, three or four channels");
	EXPECT_EQ(Commands.Refused(R"({"op":"envelope.point.add","envelope":0,"time":0,"values":[1,2]})"),
		"a point carries one value for each of the envelope's channels");
	EXPECT_EQ(Commands.Refused(R"({"op":"envelope.setProp","envelope":0,"prop":"channels","value":3})"),
		"an envelope has no 'channels'");
	EXPECT_EQ(Commands.Refused(R"({"op":"envelope.point.delete","envelope":0,"point":0})"), "'point' is 0, which is not there");
	// Nothing of that reached the map.
	EXPECT_EQ(Commands.m_Document.History().NumEntries(), 1u);
}
