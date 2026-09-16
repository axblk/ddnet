#include "art.h"

#include "structure.h"

#include <base/math.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace map_document
{
	namespace
	{
		/** One colour, as four bytes, in an order that sorts. */
		using CColorKey = uint32_t;

		CColorKey ColorAt(const uint8_t *pPixels, int Width, int x, int y)
		{
			const uint8_t *pAt = pPixels + ((size_t)y * Width + x) * 4;
			return ((CColorKey)pAt[0] << 24) | ((CColorKey)pAt[1] << 16) | ((CColorKey)pAt[2] << 8) | pAt[3];
		}

		uint8_t Channel(CColorKey Color, int Which)
		{
			return (uint8_t)((Color >> (24 - Which * 8)) & 0xFF);
		}

		bool Opaque(CColorKey Color)
		{
			return (Color & 0xFF) != 0;
		}

		/**
		 * Every colour the picture holds, once each, in a settled order.
		 *
		 * Settled rather than "as found", so that the same picture always
		 * gives the same palette and a map made twice is the same map.
		 */
		std::vector<CColorKey> UniqueColors(int Width, int Height, const uint8_t *pPixels)
		{
			std::vector<CColorKey> vColors;
			for(int y = 0; y < Height; ++y)
			{
				for(int x = 0; x < Width; ++x)
				{
					const CColorKey Color = ColorAt(pPixels, Width, x, y);
					if(Opaque(Color))
						vColors.push_back(Color);
				}
			}
			std::sort(vColors.begin(), vColors.end());
			vColors.erase(std::unique(vColors.begin(), vColors.end()), vColors.end());
			return vColors;
		}

		/** A 16-by-16 sheet whose tiles are flat colours; tile 0 is nothing. */
		CImage PaletteImage(const std::string &Name, const std::vector<CColorKey> &vColors, size_t From)
		{
			// Sixty-four pixels a tile, the same as the tilesets a map is
			// usually drawn with, so a palette looks like a tileset.
			constexpr int SIDE = 16;
			constexpr int TILE = 64;
			CImage Palette;
			Palette.m_Name = Name;
			Palette.m_External = false;
			Palette.m_Width = SIDE * TILE;
			Palette.m_Height = SIDE * TILE;
			std::vector<uint8_t> vPixels((size_t)Palette.m_Width * Palette.m_Height * 4, 0);
			for(int Index = 1; Index < SIDE * SIDE; ++Index)
			{
				const size_t Which = From + Index - 1;
				if(Which >= vColors.size())
					break;
				const CColorKey Color = vColors[Which];
				const int Left = (Index % SIDE) * TILE;
				const int Top = (Index / SIDE) * TILE;
				for(int y = Top; y < Top + TILE; ++y)
				{
					for(int x = Left; x < Left + TILE; ++x)
					{
						uint8_t *pAt = vPixels.data() + ((size_t)y * Palette.m_Width + x) * 4;
						for(int Part = 0; Part < 4; ++Part)
							pAt[Part] = Channel(Color, Part);
					}
				}
			}
			Palette.m_Data.Mutable() = std::move(vPixels);
			return Palette;
		}
	} // namespace

	size_t CountArtColors(int Width, int Height, const uint8_t *pPixels)
	{
		if(Width <= 0 || Height <= 0 || pPixels == nullptr)
			return 0;
		return UniqueColors(Width, Height, pPixels).size();
	}

	size_t AddTileArt(CDocument &Doc, const char *pName, int Width, int Height, const uint8_t *pPixels)
	{
		CMapState &Map = Doc.Edit();
		dbg_assert(Width > 0 && Height > 0 && pPixels != nullptr, "Not a picture");

		const std::vector<CColorKey> vColors = UniqueColors(Width, Height, pPixels);
		// Where in the palettes each colour sits, so that a pixel becomes a
		// tile by looking up rather than by searching.
		std::map<CColorKey, size_t> Places;
		for(size_t Index = 0; Index < vColors.size(); ++Index)
			Places[vColors[Index]] = Index;

		const std::string Name = pName == nullptr || pName[0] == '\0' ? "art" : pName;
		const size_t PerSheet = ART_PALETTE_SIZE - 1;
		const size_t Sheets = std::max<size_t>(1, (vColors.size() + PerSheet - 1) / PerSheet);

		CGroup Group;
		Group.m_Name = Name;
		for(size_t Sheet = 0; Sheet < Sheets; ++Sheet)
		{
			const size_t From = Sheet * PerSheet;
			const std::string SheetName = Sheets == 1 ? Name : Name + " " + std::to_string(Sheet + 1);
			Map.AddImage(PaletteImage(SheetName, vColors, From));

			CTileLayer Layer(ETileLayerKind::TILES, Width, Height);
			Layer.m_Name = SheetName;
			Layer.m_Image = (int)Map.NumImages() - 1;
			for(int y = 0; y < Height; ++y)
			{
				for(int x = 0; x < Width; ++x)
				{
					const CColorKey Color = ColorAt(pPixels, Width, x, y);
					if(!Opaque(Color))
						continue;
					const size_t Place = Places[Color];
					// A colour belongs to one sheet, so every other layer
					// leaves that pixel empty and the layers stack up into
					// the picture.
					if(Place < From || Place >= From + PerSheet)
						continue;
					CTile Tile;
					Tile.m_Index = (unsigned char)(Place - From + 1);
					Layer.m_Tiles.Set(x, y, Tile);
				}
			}
			Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Layer)));
		}
		Map.AddGroup(std::move(Group));
		return Map.NumGroups() - 1;
	}

	int TypeText(CDocument &Doc, const CLayerAddress &Layer, int x, int y, const char *pText)
	{
		if(pText == nullptr)
			return 0;
		CMapState &Map = Doc.Edit();
		CLayer Changed = *Map.Layer(Layer.m_Group, Layer.m_Layer);
		CTileLayer *pTiles = std::get_if<CTileLayer>(&Changed);
		dbg_assert(pTiles != nullptr, "Layer holds no tiles");

		const int Started = x;
		int Wrote = 0;
		int At = x;
		int Line = y;
		const auto Put = [&](int Index) {
			// A line that runs off the right-hand edge goes on below, in the
			// column it started in, so a block of text stays a block.
			if(At >= pTiles->Width())
			{
				At = Started;
				++Line;
			}
			if(At < 0 || Line < 0 || Line >= pTiles->Height())
			{
				++At;
				return;
			}
			CTile Tile;
			Tile.m_Index = (unsigned char)Index;
			pTiles->m_Tiles.Set(At, Line, Tile);
			++At;
			++Wrote;
		};

		for(const char *pAt = pText; *pAt != '\0'; ++pAt)
		{
			const char Letter = *pAt;
			if(Letter == '\n')
			{
				At = Started;
				++Line;
			}
			else if(Letter == ' ')
			{
				Put(0);
			}
			else if(Letter >= '1' && Letter <= '9')
			{
				Put(FONT_DIGIT_TILE + (Letter - '1'));
			}
			else if(Letter == '0')
			{
				// Nine digits then the zero, which is the order they stand in
				// on the sheets people draw.
				Put(FONT_DIGIT_TILE + 9);
			}
			else
			{
				const char Upper = Letter >= 'a' && Letter <= 'z' ? (char)(Letter - 'a' + 'A') : Letter;
				if(Upper >= 'A' && Upper <= 'Z')
					Put(FONT_LETTER_TILE + (Upper - 'A'));
				// Anything else is passed over: a font tileset has letters and
				// digits and nothing else.
			}
		}
		if(Wrote > 0)
			Map.ReplaceLayer(Layer.m_Group, Layer.m_Layer, std::move(Changed));
		return Wrote;
	}

	size_t AddQuadArt(CDocument &Doc, const char *pName, int Width, int Height, const uint8_t *pPixels,
		const CQuadArtOptions &Options)
	{
		CMapState &Map = Doc.Edit();
		dbg_assert(Width > 0 && Height > 0 && pPixels != nullptr, "Not a picture");
		const int Step = std::max(1, Options.m_PixelStep);
		const int Size = std::max(1, Options.m_QuadSize);

		const int Across = (Width + Step - 1) / Step;
		const int Down = (Height + Step - 1) / Step;
		std::vector<bool> vTaken((size_t)Across * Down, false);

		CQuadLayer Layer;
		Layer.m_Name = pName == nullptr || pName[0] == '\0' ? "art" : pName;
		Layer.m_Detail = true;
		std::vector<CQuad> vQuads;
		for(int Row = 0; Row < Down; ++Row)
		{
			for(int Column = 0; Column < Across; ++Column)
			{
				if(vTaken[(size_t)Row * Across + Column])
					continue;
				const CColorKey Color = ColorAt(pPixels, Width, Column * Step, Row * Step);
				if(!Opaque(Color))
					continue;

				// A run of one colour becomes one quad: grow to the right as
				// far as the colour holds, then down as far as whole rows of
				// it hold. Greedy, which is not the fewest quads there could
				// be, but it is one pass and it is what makes a flat picture
				// cheap.
				int Wide = 1;
				int Tall = 1;
				if(Options.m_Merge)
				{
					while(Column + Wide < Across && !vTaken[(size_t)Row * Across + Column + Wide] &&
						ColorAt(pPixels, Width, (Column + Wide) * Step, Row * Step) == Color)
						++Wide;
					bool Whole = true;
					while(Whole && Row + Tall < Down)
					{
						for(int At = 0; At < Wide && Whole; ++At)
						{
							Whole = !vTaken[(size_t)(Row + Tall) * Across + Column + At] &&
								ColorAt(pPixels, Width, (Column + At) * Step, (Row + Tall) * Step) == Color;
						}
						if(Whole)
							++Tall;
					}
					for(int Line = 0; Line < Tall; ++Line)
					{
						for(int At = 0; At < Wide; ++At)
							vTaken[(size_t)(Row + Line) * Across + Column + At] = true;
					}
				}

				const int Left = Column * Size;
				const int Top = Row * Size;
				const int Right = Left + Wide * Size;
				const int Bottom = Top + Tall * Size;
				CQuad Quad;
				Quad.m_aPoints[0] = CPoint{i2fx(Left), i2fx(Top)};
				Quad.m_aPoints[1] = CPoint{i2fx(Right), i2fx(Top)};
				Quad.m_aPoints[2] = CPoint{i2fx(Left), i2fx(Bottom)};
				Quad.m_aPoints[3] = CPoint{i2fx(Right), i2fx(Bottom)};
				// Every quad about the same place, or each about its own. The
				// first is what an envelope wants: one envelope then turns the
				// whole picture rather than every pixel on the spot.
				const CPoint Middle{i2fx((Left + Right) / 2), i2fx((Top + Bottom) / 2)};
				Quad.m_aPoints[4] = Options.m_Centralize ? CPoint{0, 0} : Middle;
				for(size_t Corner = 0; Corner < 4; ++Corner)
				{
					Quad.m_aColors[Corner] = CColor(Channel(Color, 0), Channel(Color, 1),
						Channel(Color, 2), Channel(Color, 3));
					Quad.m_aTexcoords[Corner] = CPoint{(int)(Corner % 2) * 1024, (int)(Corner / 2) * 1024};
				}
				vQuads.push_back(Quad);
			}
		}
		Layer.m_Quads.Mutable() = std::move(vQuads);

		CGroup Group;
		Group.m_Name = Layer.m_Name;
		// Clipped to what was drawn, so a picture put on a map stays where it
		// was put instead of spreading over the rest of it.
		Group.m_UseClipping = true;
		Group.m_ClipX = 0;
		Group.m_ClipY = 0;
		Group.m_ClipW = Across * Size;
		Group.m_ClipH = Down * Size;
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Layer)));
		Map.AddGroup(std::move(Group));
		return Map.NumGroups() - 1;
	}
} // namespace map_document
