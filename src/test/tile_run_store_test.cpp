#include <game/map/tile_run_store.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

	// A layer of made-up tiles, and the same layer read back out of the store.
	class CLayer
	{
	public:
		CLayer(int Width, int Height) :
			m_Width(Width), m_Height(Height), m_vTiles((size_t)Width * Height * 2, 0) {}

		void Set(int x, int y, unsigned char Index, unsigned char Flags = 0)
		{
			m_vTiles[((size_t)y * m_Width + x) * 2] = Index;
			m_vTiles[((size_t)y * m_Width + x) * 2 + 1] = Flags;
		}

		auto Reader() const
		{
			return [this](int x, int y) -> uint16_t {
				return (uint16_t)m_vTiles[((size_t)y * m_Width + x) * 2] | (uint16_t)((uint16_t)m_vTiles[((size_t)y * m_Width + x) * 2 + 1] << 8);
			};
		}

		void ExpectSameAs(const CTileRunStore &Store) const
		{
			for(int y = 0; y < m_Height; ++y)
			{
				for(int x = 0; x < m_Width; ++x)
				{
					unsigned char Index = 0;
					unsigned char Flags = 0;
					Store.ReadTile(x, y, &Index, &Flags);
					EXPECT_EQ(Index, m_vTiles[((size_t)y * m_Width + x) * 2]) << "at " << x << "," << y;
					EXPECT_EQ(Flags, m_vTiles[((size_t)y * m_Width + x) * 2 + 1]) << "at " << x << "," << y;
				}
			}
		}

		int m_Width;
		int m_Height;
		std::vector<unsigned char> m_vTiles;
	};

	size_t CountRuns(const CTileRunStore &Store)
	{
		size_t Count = 0;
		for(int ChunkY = 0; ChunkY < Store.Rows(); ++ChunkY)
			for(int ChunkX = 0; ChunkX < Store.Columns(); ++ChunkX)
				Count += Store.End(ChunkX, ChunkY) - Store.Begin(ChunkX, ChunkY);
		return Count;
	}

} // namespace

TEST(TileRunStore, AirCostsNothing)
{
	CLayer Layer(200, 150);
	CTileRunStore Store;
	Store.Build(Layer.m_Width, Layer.m_Height, Layer.Reader());
	EXPECT_EQ(CountRuns(Store), 0u);
	Layer.ExpectSameAs(Store);
}

TEST(TileRunStore, OneStretchPerRowOfTheSameTile)
{
	CLayer Layer(100, 3);
	for(int x = 0; x < 100; ++x)
		for(int y = 0; y < 3; ++y)
			Layer.Set(x, y, 7);
	CTileRunStore Store;
	Store.Build(Layer.m_Width, Layer.m_Height, Layer.Reader());
	// Two chunks across, three rows each, and a stretch never crosses either.
	EXPECT_EQ(CountRuns(Store), 6u);
	EXPECT_EQ(Store.Begin(0, 0)->m_Length, 64);
	EXPECT_EQ(Store.Begin(1, 0)->m_Length, 36);
	Layer.ExpectSameAs(Store);
}

TEST(TileRunStore, FlagsBreakAStretch)
{
	CLayer Layer(8, 1);
	for(int x = 0; x < 8; ++x)
		Layer.Set(x, 0, 3, x < 4 ? 0 : 1);
	CTileRunStore Store;
	Store.Build(Layer.m_Width, Layer.m_Height, Layer.Reader());
	EXPECT_EQ(CountRuns(Store), 2u);
	Layer.ExpectSameAs(Store);
}

TEST(TileRunStore, HolesAndEdgesReadBack)
{
	CLayer Layer(70, 70);
	for(int y = 0; y < 70; ++y)
		for(int x = 0; x < 70; ++x)
			if((x / 3 + y / 5) % 4 != 0)
				Layer.Set(x, y, (unsigned char)(1 + (x * 7 + y * 13) % 200), (unsigned char)((x + y) % 4));
	CTileRunStore Store;
	Store.Build(Layer.m_Width, Layer.m_Height, Layer.Reader());
	EXPECT_EQ(Store.Columns(), 2);
	EXPECT_EQ(Store.Rows(), 2);
	Layer.ExpectSameAs(Store);
}

TEST(TileRunStore, AskingOutsideTheLayerIsEmpty)
{
	CLayer Layer(10, 10);
	Layer.Set(5, 5, 9);
	CTileRunStore Store;
	Store.Build(Layer.m_Width, Layer.m_Height, Layer.Reader());
	EXPECT_EQ(Store.Begin(-1, 0), Store.End(-1, 0));
	EXPECT_EQ(Store.Begin(5, 5), Store.End(5, 5));
	unsigned char Index = 42;
	unsigned char Flags = 42;
	Store.ReadTile(-1, -1, &Index, &Flags);
	Store.ReadTile(100, 100, &Index, &Flags);
	EXPECT_EQ(Index, 42);
	EXPECT_EQ(Flags, 42);
}

TEST(TileRunStore, ClearGivesEverythingBack)
{
	CLayer Layer(64, 64);
	for(int x = 0; x < 64; ++x)
		Layer.Set(x, 0, 1);
	CTileRunStore Store;
	Store.Build(Layer.m_Width, Layer.m_Height, Layer.Reader());
	EXPECT_TRUE(Store.IsBuilt());
	EXPECT_GT(Store.Bytes(), 0u);
	Store.Clear();
	EXPECT_FALSE(Store.IsBuilt());
	EXPECT_EQ(Store.Bytes(), 0u);
}
