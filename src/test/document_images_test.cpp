#include <game/map/document_images.h>

#include <gtest/gtest.h>

using namespace map_document;

// Which pictures a map draws with, and how it samples them. What this decides
// is what is loaded at all: a map that names twenty images and draws with
// three should cost three, and a tileset has to be an array of layers where a
// quad wants the picture whole.

namespace
{
	CLayer Tiles(int Image)
	{
		CTileLayer Layer(ETileLayerKind::TILES, 8, 8);
		Layer.m_Image = Image;
		return Layer;
	}

	CLayer Quads(int Image)
	{
		CQuadLayer Layer;
		Layer.m_Image = Image;
		return Layer;
	}

	CMapState MapWith(std::vector<CLayer> vLayers, size_t Images)
	{
		CMapState Map;
		CGroup Group;
		for(CLayer &Layer : vLayers)
			Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Layer)));
		Map.AddGroup(std::move(Group));
		for(size_t i = 0; i < Images; ++i)
			Map.AddImage(CImage());
		return Map;
	}
} // namespace

TEST(DocumentImages, AnImageNothingDrawsWithIsNotLoaded)
{
	const CMapState Map = MapWith({Tiles(1)}, 3);
	const std::vector<unsigned char> vUsage = ImageUsage(Map);
	ASSERT_EQ(vUsage.size(), 3u);
	EXPECT_EQ(vUsage[0], 0);
	EXPECT_EQ(vUsage[1], IMAGEUSE_TILES);
	EXPECT_EQ(vUsage[2], 0);
}

TEST(DocumentImages, AQuadSamplesThePictureWhole)
{
	const CMapState Map = MapWith({Quads(0)}, 1);
	EXPECT_EQ(ImageUsage(Map)[0], IMAGEUSE_QUADS);
}

TEST(DocumentImages, AnImageThatIsBothIsBoth)
{
	const CMapState Map = MapWith({Tiles(0), Quads(0)}, 1);
	EXPECT_EQ(ImageUsage(Map)[0], IMAGEUSE_TILES | IMAGEUSE_QUADS);
}

TEST(DocumentImages, ALayerWithoutAPictureNamesNone)
{
	const CMapState Map = MapWith({Tiles(-1), Quads(-1)}, 2);
	EXPECT_EQ(ImageUsage(Map)[0], 0);
	EXPECT_EQ(ImageUsage(Map)[1], 0);
}

TEST(DocumentImages, APictureThatIsNotThereIsNotCountedTwice)
{
	// A layer may name an image the map does not have - the file allows it,
	// and an image that was deleted while a layer still pointed at it is how
	// it happens. Nothing crashes and nothing is marked.
	const CMapState Map = MapWith({Tiles(7)}, 2);
	const std::vector<unsigned char> vUsage = ImageUsage(Map);
	ASSERT_EQ(vUsage.size(), 2u);
	EXPECT_EQ(vUsage[0], 0);
	EXPECT_EQ(vUsage[1], 0);
}

TEST(DocumentImages, AMapWithoutPicturesAnswersWithNothing)
{
	const CMapState Empty;
	EXPECT_TRUE(ImageUsage(Empty).empty());
}
