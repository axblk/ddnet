#include <game/map/document/edit.h>
#include <game/mapitems.h>

#include <algorithm>
#include <type_traits>
#include <variant>
#include <vector>

namespace map_document
{
	namespace
	{
		/**
		 * Whether the flags of a tile may be turned along with the tile.
		 *
		 * In a layer that is only drawn, every tile may: what a flipped tile
		 * looks like is the tileset's business. In the layers that mean
		 * something to the game, a tile that the game does not read flags off
		 * would come out looking like something it is not, so it loses them -
		 * the same rule the editor has today.
		 */
		bool MayTurn(ETileLayerKind Kind, int Index)
		{
			if(Kind == ETileLayerKind::GAME || Kind == ETileLayerKind::FRONT || Kind == ETileLayerKind::SWITCH)
				return IsRotatableTile(Index);
			return true;
		}

		void FlipFlagsX(unsigned char *pFlags)
		{
			*pFlags ^= (*pFlags & TILEFLAG_ROTATE) ? TILEFLAG_YFLIP : TILEFLAG_XFLIP;
		}

		void FlipFlagsY(unsigned char *pFlags)
		{
			*pFlags ^= (*pFlags & TILEFLAG_ROTATE) ? TILEFLAG_XFLIP : TILEFLAG_YFLIP;
		}

		void TurnFlags(unsigned char *pFlags)
		{
			if(*pFlags & TILEFLAG_ROTATE)
				*pFlags ^= TILEFLAG_YFLIP | TILEFLAG_XFLIP;
			*pFlags ^= TILEFLAG_ROTATE;
		}

		/** The angle a speedup pushes at, mirrored and turned. */
		short FlipAngleX(short Angle) { return (short)((180 - Angle % 360 + 360) % 360); }
		short FlipAngleY(short Angle) { return (short)((360 - Angle % 360 + 360) % 360); }
		short TurnAngle(short Angle) { return (short)((Angle + 90) % 360); }

		template<typename TTile>
		std::vector<TTile> Plane(const CTileStore<TTile> &Store)
		{
			std::vector<TTile> vTiles((size_t)Store.Width() * Store.Height());
			Store.CopyTo(vTiles.data());
			return vTiles;
		}

		/**
		 * Puts the tiles of a store somewhere else in it, and changes each one
		 * on the way.
		 *
		 * A brush is small enough to go through a flat array and back - what
		 * that costs is the brush, not the layer it came out of, and the
		 * alternative is a store that can be read from and written to at once.
		 *
		 * @param Store The tiles, which come out in the new size.
		 * @param Width How wide the result is, `Height` how tall.
		 * @param Where Says which place of the result a tile goes to, as an
		 * index into it.
		 * @param Change What happens to a tile on the way.
		 */
		template<typename TTile, typename FWhere, typename FChange>
		void Move(CTileStore<TTile> &Store, int Width, int Height, FWhere &&Where, FChange &&Change)
		{
			const std::vector<TTile> vTiles = Plane(Store);
			const int Was = Store.Width();
			std::vector<TTile> vMoved((size_t)Width * Height);
			for(int y = 0; y < Store.Height(); ++y)
			{
				for(int x = 0; x < Was; ++x)
				{
					TTile Tile = vTiles[(size_t)y * Was + x];
					Change(&Tile);
					vMoved[Where(x, y)] = Tile;
				}
			}
			Store.Reset(Width, Height);
			Store.SetAll(vMoved.data());
		}

		/**
		 * Goes over the part of a rectangle that is inside the layer, saying
		 * for each place of the layer which place of the brush belongs there.
		 * The brush repeats, so a rectangle larger than the brush is filled
		 * with it over and over.
		 */
		template<typename FCell>
		void Sweep(const CTileLayer &Layer, int x, int y, int Width, int Height, int BrushWidth, int BrushHeight, FCell &&Cell)
		{
			const int Left = std::max(x, 0);
			const int Top = std::max(y, 0);
			const int Right = std::min(x + Width, Layer.Width());
			const int Bottom = std::min(y + Height, Layer.Height());
			for(int ty = Top; ty < Bottom; ++ty)
			{
				for(int tx = Left; tx < Right; ++tx)
					Cell(tx, ty, (tx - x) % BrushWidth, (ty - y) % BrushHeight);
			}
		}
	} // namespace

	bool CanStamp(ETileLayerKind Layer, ETileLayerKind Brush)
	{
		if(DrawsOwnTiles(Layer))
			return DrawsOwnTiles(Brush);
		return Layer == Brush;
	}

	CBrush GrabTiles(const CTileLayer &Layer, int x, int y, int Width, int Height)
	{
		const int Left = std::max(x, 0);
		const int Top = std::max(y, 0);
		const int Right = std::min(x + Width, Layer.Width());
		const int Bottom = std::min(y + Height, Layer.Height());
		dbg_assert(Right > Left && Bottom > Top, "A brush is taken from inside the layer");

		CBrush Brush(Layer.m_Kind, Right - Left, Bottom - Top);
		// What the brush is drawn with, so that a preview of it looks like
		// what stamping it would put there.
		Brush.m_Image = Layer.m_Image;
		Brush.m_Color = Layer.m_Color;
		for(int ty = Top; ty < Bottom; ++ty)
		{
			for(int tx = Left; tx < Right; ++tx)
				Brush.m_Tiles.Set(tx - Left, ty - Top, Layer.m_Tiles.Get(tx, ty));
		}
		std::visit([&](auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(!std::is_same_v<TStore, std::monostate>)
			{
				const TStore &Source = std::get<TStore>(Layer.m_ExtraTiles);
				for(int ty = Top; ty < Bottom; ++ty)
				{
					for(int tx = Left; tx < Right; ++tx)
						Extra.Set(tx - Left, ty - Top, Source.Get(tx, ty));
				}
			}
		},
			Brush.m_ExtraTiles);
		return Brush;
	}

	void FillTiles(CTileLayer &Layer, int x, int y, int Width, int Height, const CBrush &Brush)
	{
		dbg_assert(CanStamp(Layer.m_Kind, Brush.m_Kind), "This brush does not belong in this layer");
		dbg_assert(Brush.Width() > 0 && Brush.Height() > 0, "A brush has to hold something");

		if(DrawsOwnTiles(Layer.m_Kind))
		{
			Sweep(Layer, x, y, Width, Height, Brush.Width(), Brush.Height(), [&](int TileX, int TileY, int BrushX, int BrushY) {
				Layer.m_Tiles.Set(TileX, TileY, Brush.m_Tiles.Get(BrushX, BrushY));
			});
			return;
		}
		// A physics layer keeps a plane of air where its tiles would be and
		// says what it means in the second one, so that is the plane that is
		// painted - see `DocumentLayerSource` for how it is drawn from there.
		std::visit([&](auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(!std::is_same_v<TStore, std::monostate>)
			{
				const TStore &Source = std::get<TStore>(Brush.m_ExtraTiles);
				Sweep(Layer, x, y, Width, Height, Brush.Width(), Brush.Height(), [&](int TileX, int TileY, int BrushX, int BrushY) {
					Extra.Set(TileX, TileY, Source.Get(BrushX, BrushY));
				});
			}
		},
			Layer.m_ExtraTiles);
	}

	void StampTiles(CTileLayer &Layer, int x, int y, const CBrush &Brush)
	{
		FillTiles(Layer, x, y, Brush.Width(), Brush.Height(), Brush);
	}

	void EraseTiles(CTileLayer &Layer, int x, int y, int Width, int Height)
	{
		// One tile of air, of the layer's own kind, laid over the rectangle.
		const CBrush Air(Layer.m_Kind, 1, 1);
		FillTiles(Layer, x, y, Width, Height, Air);
	}

	void FlipBrushX(CBrush &Brush)
	{
		const int Width = Brush.Width();
		const int Height = Brush.Height();
		const ETileLayerKind Kind = Brush.m_Kind;
		const auto Mirror = [Width](int x, int y) { return (size_t)y * Width + (Width - 1 - x); };

		Move(Brush.m_Tiles, Width, Height, Mirror, [Kind](CTile *pTile) {
			if(!DrawsOwnTiles(Kind))
				return;
			if(MayTurn(Kind, pTile->m_Index))
				FlipFlagsX(&pTile->m_Flags);
			else
				pTile->m_Flags = 0;
		});
		std::visit([&](auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				Move(Extra, Width, Height, Mirror, [](CSpeedupTile *pTile) { pTile->m_Angle = FlipAngleX(pTile->m_Angle); });
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
			{
				// The flags of a switch layer sit on the switch tile, which is
				// what the file holds and what is drawn. The editor turns the
				// other plane instead and loses them when it saves.
				Move(Extra, Width, Height, Mirror, [Kind](CSwitchTile *pTile) {
					if(MayTurn(Kind, pTile->m_Type))
						FlipFlagsX(&pTile->m_Flags);
					else
						pTile->m_Flags = 0;
				});
			}
			else if constexpr(!std::is_same_v<TStore, std::monostate>)
			{
				Move(Extra, Width, Height, Mirror, [](auto *) {});
			}
		},
			Brush.m_ExtraTiles);
	}

	void FlipBrushY(CBrush &Brush)
	{
		const int Width = Brush.Width();
		const int Height = Brush.Height();
		const ETileLayerKind Kind = Brush.m_Kind;
		const auto Mirror = [Width, Height](int x, int y) { return (size_t)(Height - 1 - y) * Width + x; };

		Move(Brush.m_Tiles, Width, Height, Mirror, [Kind](CTile *pTile) {
			if(!DrawsOwnTiles(Kind))
				return;
			if(MayTurn(Kind, pTile->m_Index))
				FlipFlagsY(&pTile->m_Flags);
			else
				pTile->m_Flags = 0;
		});
		std::visit([&](auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				Move(Extra, Width, Height, Mirror, [](CSpeedupTile *pTile) { pTile->m_Angle = FlipAngleY(pTile->m_Angle); });
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
			{
				Move(Extra, Width, Height, Mirror, [Kind](CSwitchTile *pTile) {
					if(MayTurn(Kind, pTile->m_Type))
						FlipFlagsY(&pTile->m_Flags);
					else
						pTile->m_Flags = 0;
				});
			}
			else if constexpr(!std::is_same_v<TStore, std::monostate>)
			{
				Move(Extra, Width, Height, Mirror, [](auto *) {});
			}
		},
			Brush.m_ExtraTiles);
	}

	void RotateBrush(CBrush &Brush)
	{
		const int Width = Brush.Height();
		const int Height = Brush.Width();
		const ETileLayerKind Kind = Brush.m_Kind;
		// A quarter turn clockwise: the leftmost column becomes the top row,
		// so the result is as wide as the brush was tall.
		const auto Turn = [Width](int x, int y) { return (size_t)x * Width + (Width - 1 - y); };

		Move(Brush.m_Tiles, Width, Height, Turn, [Kind](CTile *pTile) {
			if(!DrawsOwnTiles(Kind))
				return;
			if(MayTurn(Kind, pTile->m_Index))
				TurnFlags(&pTile->m_Flags);
			else
				pTile->m_Flags = 0;
		});
		std::visit([&](auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				Move(Extra, Width, Height, Turn, [](CSpeedupTile *pTile) { pTile->m_Angle = TurnAngle(pTile->m_Angle); });
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
			{
				Move(Extra, Width, Height, Turn, [Kind](CSwitchTile *pTile) {
					if(MayTurn(Kind, pTile->m_Type))
						TurnFlags(&pTile->m_Flags);
					else
						pTile->m_Flags = 0;
				});
			}
			else if constexpr(!std::is_same_v<TStore, std::monostate>)
			{
				Move(Extra, Width, Height, Turn, [](auto *) {});
			}
		},
			Brush.m_ExtraTiles);
	}

	void PaintTiles(CDocument &Doc, size_t Group, size_t Layer, int x, int y, const CBrush &Brush)
	{
		EditTileLayer(Doc, Group, Layer, [&](CTileLayer &Changed) {
			StampTiles(Changed, x, y, Brush);
		});
	}
} // namespace map_document
