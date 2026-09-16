#include <game/map/document/art.h>
#include <game/map/document/document.h>
#include <game/map/document/structure.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

using namespace map_document;

// A picture turned into map. Both ways read pixels rather than a file, because
// a browser decodes PNGs and a decoder here would be a second one.

namespace
{
	class CPicture
	{
	public:
		int m_Width;
		int m_Height;
		std::vector<uint8_t> m_vPixels;

		CPicture(int Width, int Height) :
			m_Width(Width), m_Height(Height), m_vPixels((size_t)Width * Height * 4, 0) {}

		void Set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
		{
			uint8_t *pAt = m_vPixels.data() + ((size_t)y * m_Width + x) * 4;
			pAt[0] = r;
			pAt[1] = g;
			pAt[2] = b;
			pAt[3] = a;
		}

		const uint8_t *Pixels() const { return m_vPixels.data(); }
	};

	// Two colours side by side, and a column that is not opaque.
	CPicture TwoColoursAndAHole()
	{
		CPicture Made(3, 2);
		Made.Set(0, 0, 255, 0, 0);
		Made.Set(0, 1, 255, 0, 0);
		Made.Set(1, 0, 0, 255, 0);
		Made.Set(1, 1, 0, 255, 0);
		// Column 2 stays transparent.
		return Made;
	}
} // namespace

TEST(Art, APictureBecomesItsOwnTilesetAndALayerOfIt)
{
	const CPicture Picture = TwoColoursAndAHole();
	EXPECT_EQ(CountArtColors(Picture.m_Width, Picture.m_Height, Picture.Pixels()), 2u)
		<< "what is not opaque is not a colour";

	CDocument Document{CMapState()};
	Document.Begin("Tile art", nullptr);
	const size_t Group = AddTileArt(Document, "logo", Picture.m_Width, Picture.m_Height, Picture.Pixels());
	Document.Commit();

	const CMapState &Map = Document.Map();
	ASSERT_EQ(Map.NumGroups(), Group + 1);
	ASSERT_EQ(Map.NumLayers(Group), 1u) << "two colours fit on one sheet";
	ASSERT_EQ(Map.NumImages(), 1u);
	EXPECT_EQ(Map.Image(0)->m_Name, "logo");
	EXPECT_FALSE(Map.Image(0)->m_External) << "a palette is made here, not fetched";
	EXPECT_EQ(Map.Image(0)->m_Width, 16 * 64) << "sixteen tiles across, like any tileset";

	const CTileLayer &Layer = std::get<CTileLayer>(*Map.Layer(Group, 0));
	EXPECT_EQ(Layer.m_Image, 0);
	EXPECT_EQ(Layer.Width(), 3);
	EXPECT_EQ(Layer.Height(), 2);
	// Tile 0 is nothing, so the colours start at one, in the order they sort.
	EXPECT_EQ(Layer.m_Tiles.Get(0, 0).m_Index, 2) << "red sorts after green";
	EXPECT_EQ(Layer.m_Tiles.Get(1, 0).m_Index, 1);
	EXPECT_EQ(Layer.m_Tiles.Get(2, 0).m_Index, 0) << "what was not opaque is nothing";

	// And the palette really holds those colours where the tiles point.
	const CSharedList<uint8_t> &Palette = Map.Image(0)->m_Data;
	const auto &&At = [&](int x, int y) {
		const size_t Index = ((size_t)y * (16 * 64) + x) * 4;
		return std::vector<uint8_t>{Palette[Index], Palette[Index + 1], Palette[Index + 2], Palette[Index + 3]};
	};
	EXPECT_EQ(At(64 + 32, 32), (std::vector<uint8_t>{0, 255, 0, 255})) << "tile 1 is the first colour";
	EXPECT_EQ(At(128 + 32, 32), (std::vector<uint8_t>{255, 0, 0, 255})) << "tile 2 is the second";
	EXPECT_EQ(At(32, 32), (std::vector<uint8_t>{0, 0, 0, 0})) << "tile 0 is nothing";
}

TEST(Art, MoreColoursThanASheetHoldsBecomeMoreSheetsAndMoreLayers)
{
	// 300 colours, which is more than the 255 one sheet holds.
	CPicture Many(300, 1);
	for(int x = 0; x < 300; ++x)
		Many.Set(x, 0, (uint8_t)(x % 256), (uint8_t)(x / 256), 0);

	CDocument Document{CMapState()};
	Document.Begin("Tile art", nullptr);
	const size_t Group = AddTileArt(Document, "many", Many.m_Width, Many.m_Height, Many.Pixels());
	Document.Commit();

	const CMapState &Map = Document.Map();
	ASSERT_EQ(Map.NumLayers(Group), 2u);
	EXPECT_EQ(Map.NumImages(), 2u);
	EXPECT_EQ(Map.Image(0)->m_Name, "many 1");
	EXPECT_EQ(Map.Image(1)->m_Name, "many 2");

	// Every pixel is drawn by exactly one of the layers, and the other leaves
	// it empty - together they are the picture.
	const CTileLayer &First = std::get<CTileLayer>(*Map.Layer(Group, 0));
	const CTileLayer &Second = std::get<CTileLayer>(*Map.Layer(Group, 1));
	int Drawn = 0;
	for(int x = 0; x < 300; ++x)
	{
		const int One = First.m_Tiles.Get(x, 0).m_Index;
		const int Two = Second.m_Tiles.Get(x, 0).m_Index;
		EXPECT_FALSE(One != 0 && Two != 0) << "no pixel is drawn twice, at " << x;
		if(One != 0 || Two != 0)
			++Drawn;
	}
	EXPECT_EQ(Drawn, 300);
}

TEST(Art, APictureBecomesQuadsAndARunOfOneColourBecomesOneOfThem)
{
	const CPicture Picture = TwoColoursAndAHole();
	CDocument Document{CMapState()};
	CQuadArtOptions Options;
	Options.m_QuadSize = 64;
	Document.Begin("Quad art", nullptr);
	const size_t Group = AddQuadArt(Document, "logo", Picture.m_Width, Picture.m_Height, Picture.Pixels(), Options);
	Document.Commit();

	const CMapState &Map = Document.Map();
	ASSERT_EQ(Map.NumLayers(Group), 1u);
	EXPECT_TRUE(Map.m_vpGroups[Group]->m_UseClipping);
	EXPECT_EQ(Map.m_vpGroups[Group]->m_ClipW, 3 * 64);
	EXPECT_EQ(Map.m_vpGroups[Group]->m_ClipH, 2 * 64);

	const CQuadLayer &Layer = std::get<CQuadLayer>(*Map.Layer(Group, 0));
	// Two columns of one colour each, two pixels tall: two quads, not four.
	ASSERT_EQ(Layer.m_Quads.Size(), 2u);
	const CQuad &Red = Layer.m_Quads[0];
	EXPECT_EQ(fx2i(Red.m_aPoints[0].x), 0);
	EXPECT_EQ(fx2i(Red.m_aPoints[3].x), 64);
	EXPECT_EQ(fx2i(Red.m_aPoints[3].y), 128) << "both rows in one quad";
	EXPECT_EQ(Red.m_aColors[0].r, 255);
	EXPECT_EQ(Red.m_aColors[0].g, 0);
	EXPECT_EQ(fx2i(Red.m_aPoints[4].x), 32) << "its pivot in its own middle";

	// Without merging, every pixel is its own quad.
	CDocument Apart{CMapState()};
	Options.m_Merge = false;
	Apart.Begin("Quad art", nullptr);
	const size_t Other = AddQuadArt(Apart, "logo", Picture.m_Width, Picture.m_Height, Picture.Pixels(), Options);
	Apart.Commit();
	EXPECT_EQ(std::get<CQuadLayer>(*Apart.Map().Layer(Other, 0)).m_Quads.Size(), 4u)
		<< "the column that is not opaque is still nothing";
}

TEST(Art, QuadArtCanPutEveryPivotInTheSamePlaceAndCanReadEveryOtherPixel)
{
	CPicture Picture(4, 4);
	for(int y = 0; y < 4; ++y)
	{
		for(int x = 0; x < 4; ++x)
			Picture.Set(x, y, (uint8_t)(x * 60), (uint8_t)(y * 60), 0);
	}

	CQuadArtOptions Options;
	Options.m_Centralize = true;
	Options.m_Merge = false;
	CDocument Document{CMapState()};
	Document.Begin("Quad art", nullptr);
	const size_t Group = AddQuadArt(Document, "grid", 4, 4, Picture.Pixels(), Options);
	Document.Commit();
	const CQuadLayer &Together = std::get<CQuadLayer>(*Document.Map().Layer(Group, 0));
	EXPECT_EQ(Together.m_Quads.Size(), 16u);
	for(size_t Quad = 0; Quad < Together.m_Quads.Size(); ++Quad)
	{
		EXPECT_EQ(Together.m_Quads[Quad].m_aPoints[4].x, 0) << "every pivot in the same place, at " << Quad;
		EXPECT_EQ(Together.m_Quads[Quad].m_aPoints[4].y, 0);
	}

	// Every second pixel: a quarter as many quads, over the same area.
	Options.m_PixelStep = 2;
	CDocument Coarse{CMapState()};
	Coarse.Begin("Quad art", nullptr);
	const size_t Fewer = AddQuadArt(Coarse, "grid", 4, 4, Picture.Pixels(), Options);
	Coarse.Commit();
	EXPECT_EQ(std::get<CQuadLayer>(*Coarse.Map().Layer(Fewer, 0)).m_Quads.Size(), 4u);
	EXPECT_EQ(Coarse.Map().m_vpGroups[Fewer]->m_ClipW, 2 * 64);
}
