#include <game/map/document/map_state.h>

#include <gtest/gtest.h>

using namespace map_document;

// Envelopes, images, sounds and the map's own description are versioned like
// everything else, so what is tested here is that changing one of them costs
// that one and leaves the pixels, the points and the tiles where they are.

namespace
{
	CMapState WithAssets()
	{
		CMapState State;

		CEnvelope Colour;
		Colour.m_Name = "colour";
		Colour.m_Channels = 4;
		Colour.m_Points.Mutable().resize(4);
		State.AddEnvelope(std::move(Colour));

		CEnvelope Position;
		Position.m_Name = "position";
		Position.m_Channels = 3;
		Position.m_Points.Mutable().resize(2);
		State.AddEnvelope(std::move(Position));

		CImage Embedded;
		Embedded.m_Name = "grass_main";
		Embedded.m_External = false;
		Embedded.m_Width = 64;
		Embedded.m_Height = 64;
		Embedded.m_Data.Mutable().resize(64 * 64 * 4);
		State.AddImage(std::move(Embedded));

		CImage External;
		External.m_Name = "entities";
		State.AddImage(std::move(External));

		CSound Sound;
		Sound.m_Name = "hammer";
		Sound.m_Data.Mutable().resize(1024);
		State.AddSound(std::move(Sound));

		State.m_Info.m_Author = "someone";
		State.m_Info.m_Settings.Mutable().emplace_back("sv_deepfly 0");
		return State;
	}
}

TEST(Assets, AMapHoldsWhatItsLayersPointAt)
{
	const CMapState State = WithAssets();
	ASSERT_EQ(State.NumEnvelopes(), 2u);
	ASSERT_EQ(State.NumImages(), 2u);
	ASSERT_EQ(State.NumSounds(), 1u);

	EXPECT_EQ(State.Envelope(0)->m_Name, "colour");
	EXPECT_EQ(State.Envelope(1)->m_Points.Size(), 2u);
	EXPECT_FALSE(State.Image(0)->m_External);
	EXPECT_EQ(State.Image(0)->m_Data.Size(), 64u * 64u * 4u);
	// An external image is a name and nothing else - the file it names is
	// somebody else's to read.
	EXPECT_TRUE(State.Image(1)->m_External);
	EXPECT_TRUE(State.Image(1)->m_Data.Empty());
	EXPECT_EQ(State.Sound(0)->m_Data.Size(), 1024u);
	EXPECT_EQ(State.m_Info.m_Settings[0], "sv_deepfly 0");
	// The pixels are the biggest thing in there by far.
	EXPECT_GT(State.Bytes(), 64u * 64u * 4u);
}

TEST(Assets, DraggingAPointCostsThatOneEnvelope)
{
	const CMapState Before = WithAssets();
	CMapState After = Before;

	CEnvelope Changed = *After.Envelope(0);
	Changed.m_Points.Mutable()[1].m_Time = CFixedTime(500);
	After.ReplaceEnvelope(0, std::move(Changed));

	EXPECT_NE(Before.Envelope(0), After.Envelope(0));
	EXPECT_EQ(Before.Envelope(1), After.Envelope(1));
	EXPECT_EQ(Before.Envelope(0)->m_Points[1].m_Time, CFixedTime(0));
	EXPECT_EQ(After.Envelope(0)->m_Points[1].m_Time, CFixedTime(500));

	// Nothing else was touched, the pixels of the embedded image least of all.
	EXPECT_EQ(Before.Image(0), After.Image(0));
	EXPECT_EQ(Before.Sound(0), After.Sound(0));
	std::unordered_set<const void *> Seen;
	const uint64_t Both = Before.BytesOnce(Seen) + After.BytesOnce(Seen);
	EXPECT_LT(Both, Before.Bytes() + 4 * 1024);
}

TEST(Assets, RenamingAnImageKeepsItsPixels)
{
	const CMapState Before = WithAssets();
	CMapState After = Before;

	CImage Renamed = *After.Image(0);
	Renamed.m_Name = "grass_main_2";
	// The pixels come along as the same list, because renaming did not ask
	// for them.
	const void *pPixels = Renamed.m_Data.Id();
	After.ReplaceImage(0, std::move(Renamed));

	EXPECT_EQ(Before.Image(0)->m_Name, "grass_main");
	EXPECT_EQ(After.Image(0)->m_Name, "grass_main_2");
	EXPECT_EQ(After.Image(0)->m_Data.Id(), pPixels);
	EXPECT_EQ(Before.Image(0)->m_Data.Id(), pPixels);

	std::unordered_set<const void *> Seen;
	const uint64_t Both = Before.BytesOnce(Seen) + After.BytesOnce(Seen);
	// One image node more, and not a second copy of the pixels.
	EXPECT_LT(Both, Before.Bytes() + 64 * 64 * 4);
}

TEST(Assets, TheServerSettingsAreVersionedToo)
{
	const CMapState Before = WithAssets();
	CMapState After = Before;
	After.m_Info.m_Settings.Mutable().emplace_back("sv_test 1");

	EXPECT_EQ(Before.m_Info.m_Settings.Size(), 1u);
	EXPECT_EQ(After.m_Info.m_Settings.Size(), 2u);
	EXPECT_NE(Before.m_Info.m_Settings.Id(), After.m_Info.m_Settings.Id());
	EXPECT_EQ(Before.m_Info.m_Author, After.m_Info.m_Author);
}

TEST(Assets, ASharedListOfSettingsIsCountedOnce)
{
	const CMapState Before = WithAssets();
	const CMapState After = Before;

	std::unordered_set<const void *> Seen;
	const uint64_t Both = Before.BytesOnce(Seen) + After.BytesOnce(Seen);
	// The second version shares every last node, so it costs nothing at all.
	EXPECT_EQ(Both, Before.Bytes());
}
