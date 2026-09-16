#include <game/map/document/view.h>

#include <gtest/gtest.h>

using namespace map_document;

// Where the editor looks. What matters here is that the page and the renderer
// cannot disagree about it: a click has to land on the tile it points at, and
// the wheel has to zoom towards the pointer rather than away from what is
// being looked at.

namespace
{
	CView Surface(int Width = 1600, int Height = 900)
	{
		CView View;
		View.SetSurface(Width, Height);
		return View;
	}
} // namespace

TEST(View, TheMiddleOfTheSurfaceIsTheMiddleOfTheView)
{
	CView View = Surface();
	View.SetCenter(vec2(1000.0f, 500.0f));
	const vec2 World = View.ScreenToWorld(vec2(800.0f, 450.0f));
	EXPECT_FLOAT_EQ(World.x, 1000.0f);
	EXPECT_FLOAT_EQ(World.y, 500.0f);
}

TEST(View, ScreenAndWorldAreTheSameWayRound)
{
	CView View = Surface();
	View.SetCenter(vec2(1234.0f, 567.0f));
	View.SetZoom(2.5f);
	for(const vec2 &Pixel : {vec2(0.0f, 0.0f), vec2(1600.0f, 900.0f), vec2(37.0f, 811.0f)})
	{
		const vec2 Back = View.WorldToScreen(View.ScreenToWorld(Pixel));
		EXPECT_NEAR(Back.x, Pixel.x, 0.01f);
		EXPECT_NEAR(Back.y, Pixel.y, 0.01f);
	}
}

TEST(View, ASurfaceTwiceAsWideShowsTwiceAsMuchMap)
{
	const vec2 Narrow = Surface(1000, 1000).ViewSize();
	const vec2 Wide = Surface(2000, 1000).ViewSize();
	// A wider window is asked to show more map rather than to trade its height
	// for the width, which is what a game would do and what nobody editing a
	// map wants.
	EXPECT_FLOAT_EQ(Wide.x, Narrow.x * 2.0f);
	EXPECT_FLOAT_EQ(Wide.y, Narrow.y);
}

TEST(View, AtSixteenToNineTheViewIsTheOneTheGameHas)
{
	// Which is what makes a zoom of one mean the same in the editor, in the
	// map viewer and in the client.
	float Width, Height;
	CalcViewSize(16.0f / 9.0f, 1.0f, 178.0f / 100.0f, &Width, &Height);
	const vec2 Ours = Surface(1600, 900).ViewSize();
	EXPECT_NEAR(Ours.x, Width, 0.01f);
	EXPECT_NEAR(Ours.y, Height, 0.01f);
}

TEST(View, ZoomingShowsMoreOrLessOfTheWorld)
{
	CView View = Surface();
	const vec2 At1 = View.VisibleSize();
	View.SetZoom(2.0f);
	EXPECT_FLOAT_EQ(View.VisibleSize().x, At1.x * 2.0f);
	View.SetZoom(0.5f);
	EXPECT_FLOAT_EQ(View.VisibleSize().y, At1.y * 0.5f);
}

TEST(View, ZoomIsKeptWhereTheMapCanStillBeFound)
{
	CView View = Surface();
	View.SetZoom(0.0f);
	EXPECT_FLOAT_EQ(View.Zoom(), CView::MIN_ZOOM);
	View.SetZoom(1e9f);
	EXPECT_FLOAT_EQ(View.Zoom(), CView::MAX_ZOOM);
}

TEST(View, ZoomingAboutAPointLeavesItWhereItIs)
{
	CView View = Surface();
	View.SetCenter(vec2(500.0f, 500.0f));
	const vec2 Pointer(1400.0f, 200.0f);
	const vec2 Was = View.ScreenToWorld(Pointer);

	View.ZoomAt(Pointer, 1.1f);
	const vec2 Now = View.ScreenToWorld(Pointer);
	EXPECT_NEAR(Now.x, Was.x, 0.01f);
	EXPECT_NEAR(Now.y, Was.y, 0.01f);

	// And the other way, several notches of a wheel deep.
	for(int i = 0; i < 10; ++i)
		View.ZoomAt(Pointer, 1.0f / 1.1f);
	const vec2 Later = View.ScreenToWorld(Pointer);
	EXPECT_NEAR(Later.x, Was.x, 0.05f);
	EXPECT_NEAR(Later.y, Was.y, 0.05f);
}

TEST(View, ZoomingAtTheLimitDoesNotMoveTheMap)
{
	CView View = Surface();
	View.SetZoom(CView::MAX_ZOOM);
	const vec2 Pointer(100.0f, 100.0f);
	const vec2 Was = View.ScreenToWorld(Pointer);
	View.ZoomAt(Pointer, 2.0f);
	const vec2 Now = View.ScreenToWorld(Pointer);
	EXPECT_FLOAT_EQ(Now.x, Was.x);
	EXPECT_FLOAT_EQ(Now.y, Was.y);
}

TEST(View, DraggingMovesTheWorldWithThePointer)
{
	CView View = Surface();
	View.SetCenter(vec2(0.0f, 0.0f));
	const vec2 Pointer(800.0f, 450.0f);
	const vec2 Was = View.ScreenToWorld(Pointer);
	// The view moves against the drag, so what was grabbed stays under the
	// pointer when it has moved by the same distance.
	View.MoveByPixels(vec2(-40.0f, -25.0f));
	const vec2 Now = View.ScreenToWorld(Pointer + vec2(40.0f, 25.0f));
	EXPECT_NEAR(Now.x, Was.x, 0.01f);
	EXPECT_NEAR(Now.y, Was.y, 0.01f);
}

TEST(View, AFittedMapIsWhollyOnTheScreen)
{
	CView View = Surface();
	const vec2 WorldSize(4000.0f, 3000.0f);
	View.Fit(WorldSize);

	EXPECT_FLOAT_EQ(View.Center().x, 2000.0f);
	EXPECT_FLOAT_EQ(View.Center().y, 1500.0f);
	const vec2 Visible = View.VisibleSize();
	EXPECT_GE(Visible.x, WorldSize.x - 0.01f);
	EXPECT_GE(Visible.y, WorldSize.y - 0.01f);
	// And no further out than it has to be: one of the two sides fits exactly.
	EXPECT_TRUE(std::abs(Visible.x - WorldSize.x) < 0.01f || std::abs(Visible.y - WorldSize.y) < 0.01f);
}

TEST(View, AFilledMapCoversTheScreen)
{
	CView View = Surface();
	const vec2 WorldSize(4000.0f, 3000.0f);
	View.Fill(WorldSize);
	const vec2 Visible = View.VisibleSize();
	EXPECT_LE(Visible.x, WorldSize.x + 0.01f);
	EXPECT_LE(Visible.y, WorldSize.y + 0.01f);
}

TEST(View, ATileIsWhatThePointerIsOver)
{
	CView View = Surface();
	View.SetCenter(vec2(0.0f, 0.0f));
	View.SetZoom(1.0f);
	// The middle of the surface is world (0,0), which is the corner of tile
	// (0,0); a pixel to the left and up of it is the tile before it.
	EXPECT_EQ(View.ScreenToTile(vec2(801.0f, 451.0f)).x, 0);
	EXPECT_EQ(View.ScreenToTile(vec2(801.0f, 451.0f)).y, 0);
	EXPECT_EQ(View.ScreenToTile(vec2(799.0f, 449.0f)).x, -1);
	EXPECT_EQ(View.ScreenToTile(vec2(799.0f, 449.0f)).y, -1);
}
