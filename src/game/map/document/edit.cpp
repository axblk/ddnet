#include <game/map/document/edit.h>
#include <game/map/document/structure.h>
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

	void SetBrushNumbers(CBrush &Brush, const CBrushNumbers &Numbers)
	{
		const int Number = std::clamp(Numbers.m_Number, 0, 255);
		const int Delay = std::clamp(Numbers.m_Delay, 0, 255);
		const int Force = std::clamp(Numbers.m_Force, 0, 255);
		const int MaxSpeed = std::clamp(Numbers.m_MaxSpeed, 0, 255);
		// Degrees, and the way round is what matters rather than how many
		// turns it took to get there.
		const short Angle = (short)(((Numbers.m_Angle % 360) + 360) % 360);
		std::visit([&](auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(std::is_same_v<TStore, std::monostate>)
				return;
			else
			{
				for(int y = 0; y < Extra.Height(); ++y)
				{
					for(int x = 0; x < Extra.Width(); ++x)
					{
						auto Tile = Extra.Get(x, y);
						// Air carries nothing: a number on a tile that does
						// nothing would go into the file and come back as
						// exactly that.
						if(Tile.m_Type == 0)
							continue;
						if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
						{
							Tile.m_Force = (unsigned char)Force;
							Tile.m_MaxSpeed = (unsigned char)MaxSpeed;
							Tile.m_Angle = Angle;
						}
						else
						{
							Tile.m_Number = (unsigned char)Number;
							if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
								Tile.m_Delay = (unsigned char)Delay;
						}
						Extra.Set(x, y, Tile);
					}
				}
			}
		},
			Brush.m_ExtraTiles);
	}

	CBrushNumbers BrushNumbers(const CBrush &Brush)
	{
		CBrushNumbers Numbers;
		std::visit([&](const auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(std::is_same_v<TStore, std::monostate>)
				return;
			else
			{
				for(int y = 0; y < Extra.Height(); ++y)
				{
					for(int x = 0; x < Extra.Width(); ++x)
					{
						const auto Tile = Extra.Get(x, y);
						if(Tile.m_Type == 0)
							continue;
						if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
						{
							Numbers.m_Force = Tile.m_Force;
							Numbers.m_MaxSpeed = Tile.m_MaxSpeed;
							Numbers.m_Angle = Tile.m_Angle;
						}
						else
						{
							Numbers.m_Number = Tile.m_Number;
							if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
								Numbers.m_Delay = Tile.m_Delay;
						}
						return;
					}
				}
			}
		},
			Brush.m_ExtraTiles);
		return Numbers;
	}

	namespace
	{
		/**
		 * Whether a tile of this store carries the number that is being
		 * counted, which is a different question for every kind.
		 */
		template<typename TStore, typename TTile>
		bool CarriesNumber(const TTile &Tile, bool Checkpoint)
		{
			if constexpr(std::is_same_v<TStore, CTileStore<CTeleTile>>)
				return IsValidTeleTile(Tile.m_Type) && IsTeleTileNumberUsed(Tile.m_Type, Checkpoint);
			else if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
				return IsValidSwitchTile(Tile.m_Type) && IsSwitchTileNumberUsed(Tile.m_Type);
			else if constexpr(std::is_same_v<TStore, CTileStore<CTuneTile>>)
				return IsValidTuneTile(Tile.m_Type);
			else
				return false;
		}

		// Two tiles of the same number closer together than this are one
		// place: a teleporter is drawn several tiles wide, and somebody
		// looking for where number seven is wants seven places, not seventy.
		constexpr float MIN_CLUSTER_DISTANCE = 10.0f;
	} // namespace

	int NextFreeNumber(const CTileLayer &Layer, bool Checkpoint)
	{
		bool aTaken[256] = {};
		std::visit([&](const auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(!std::is_same_v<TStore, std::monostate> && !std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				for(int y = 0; y < Extra.Height(); ++y)
					for(int x = 0; x < Extra.Width(); ++x)
					{
						const auto Tile = Extra.Get(x, y);
						if(CarriesNumber<TStore>(Tile, Checkpoint))
							aTaken[Tile.m_Number] = true;
					}
			}
		},
			Layer.m_ExtraTiles);
		for(int Number = 1; Number <= 255; ++Number)
			if(!aTaken[Number])
				return Number;
		return -1;
	}

	std::vector<ivec2> NumberPlaces(const CTileLayer &Layer, int Number)
	{
		std::vector<ivec2> vPlaces;
		if(Number <= 0 || Number > 255)
			return vPlaces;
		std::visit([&](const auto &Extra) {
			using TStore = std::decay_t<decltype(Extra)>;
			if constexpr(!std::is_same_v<TStore, std::monostate> && !std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				for(int y = 0; y < Extra.Height(); ++y)
					for(int x = 0; x < Extra.Width(); ++x)
					{
						const auto Tile = Extra.Get(x, y);
						if(Tile.m_Number != Number || !CarriesNumber<TStore>(Tile, IsTeleTileCheckpoint(Tile.m_Type)))
							continue;
						if(!vPlaces.empty() && distance(vec2(vPlaces.back().x, vPlaces.back().y), vec2(x, y)) < MIN_CLUSTER_DISTANCE)
							continue;
						vPlaces.emplace_back(x, y);
					}
			}
		},
			Layer.m_ExtraTiles);
		return vPlaces;
	}

	namespace
	{
		/** Which tile index one of the thirteen construct operations writes. */
		int GameTileIndex(EGameTile Tile)
		{
			switch(Tile)
			{
			case EGameTile::AIR: return TILE_AIR;
			case EGameTile::HOOKABLE: return TILE_SOLID;
			case EGameTile::DEATH: return TILE_DEATH;
			case EGameTile::UNHOOKABLE: return TILE_NOHOOK;
			case EGameTile::HOOKTHROUGH: return TILE_THROUGH_CUT;
			case EGameTile::FREEZE: return TILE_FREEZE;
			case EGameTile::UNFREEZE: return TILE_UNFREEZE;
			case EGameTile::DEEP_FREEZE: return TILE_DFREEZE;
			case EGameTile::DEEP_UNFREEZE: return TILE_DUNFREEZE;
			case EGameTile::BLUE_CHECK_TELE: return TILE_TELECHECKIN;
			case EGameTile::RED_CHECK_TELE: return TILE_TELECHECKINEVIL;
			case EGameTile::LIVE_FREEZE: return TILE_LFREEZE;
			case EGameTile::LIVE_UNFREEZE: return TILE_LUNFREEZE;
			}
			dbg_assert(false, "There is no such game tile: %d", (int)Tile);
			return TILE_AIR;
		}

		/**
		 * Where the tele layer is, making one if the map has none.
		 *
		 * A new one goes at the end of the group the game layer is in and is
		 * that layer's size, because that is what a physics layer is.
		 */
		CLayerAddress TeleLayer(CDocument &Doc)
		{
			const CMapState &Map = Doc.Edit();
			const CLayerAddress Game = *FindGameLayer(Map);
			for(size_t Layer = 0; Layer < Map.NumLayers(Game.m_Group); ++Layer)
			{
				const CTileLayer *pLayer = std::get_if<CTileLayer>(Map.Layer(Game.m_Group, Layer));
				if(pLayer != nullptr && pLayer->m_Kind == ETileLayerKind::TELE)
					return CLayerAddress{Game.m_Group, Layer};
			}
			const CTileLayer *pGame = Map.TileLayer(Game.m_Group, Game.m_Layer);
			CTileLayer Made(ETileLayerKind::TELE, pGame->Width(), pGame->Height());
			Made.m_Name = "Tele";
			return AddLayer(Doc, Game.m_Group, std::move(Made));
		}
	} // namespace

	bool CanConstructGameTiles(const CMapState &Map, size_t Group, size_t Layer)
	{
		if(!FindGameLayer(Map).has_value())
			return false;
		const CTileLayer *pDesign = std::get_if<CTileLayer>(Map.Layer(Group, Layer));
		if(pDesign == nullptr || pDesign->m_Kind != ETileLayerKind::TILES)
			return false;
		const CGroup *pGroup = Map.Group(Group);
		// The group has to sit still and sit on the grid, or "under this tile"
		// is not a place.
		return pGroup->m_ParallaxX == 100 && pGroup->m_ParallaxY == 100 &&
		       pGroup->m_OffsetX % 32 == 0 && pGroup->m_OffsetY % 32 == 0;
	}

	int ConstructGameTiles(CDocument &Doc, size_t Group, size_t Layer, EGameTile Tile)
	{
		dbg_assert(CanConstructGameTiles(Doc.Edit(), Group, Layer), "This layer does not lie over the game layer");
		const int Index = GameTileIndex(Tile);
		const bool Checkpoint = Tile == EGameTile::BLUE_CHECK_TELE || Tile == EGameTile::RED_CHECK_TELE;

		// Where the design layer's top left corner is in the game layer: the
		// group moves the layer, and it moves it in whole tiles.
		const CGroup *pGroup = Doc.Edit().Group(Group);
		const int OffsetX = -pGroup->m_OffsetX / 32;
		const int OffsetY = -pGroup->m_OffsetY / 32;
		const int Width = Doc.Edit().TileLayer(Group, Layer)->Width();
		const int Height = Doc.Edit().TileLayer(Group, Layer)->Height();

		const CLayerAddress Target = Checkpoint ? TeleLayer(Doc) : *FindGameLayer(Doc.Edit());
		const CTileLayer *pTarget = Doc.Edit().TileLayer(Target.m_Group, Target.m_Layer);
		if(pTarget->Width() < Width + OffsetX || pTarget->Height() < Height + OffsetY)
		{
			ResizeLayer(Doc, Target, std::max(pTarget->Width(), Width + OffsetX),
				std::max(pTarget->Height(), Height + OffsetY));
		}

		int Written = 0;
		const CTileLayer Design = *Doc.Edit().TileLayer(Group, Layer);
		EditTileLayer(Doc, Target.m_Group, Target.m_Layer, [&](CTileLayer &Changed) {
			for(int y = std::max(-OffsetY, 0); y < Height; ++y)
			{
				for(int x = std::max(-OffsetX, 0); x < Width; ++x)
				{
					if(Design.m_Tiles.Get(x, y).m_Index == 0)
						continue;
					if(Checkpoint)
					{
						CTeleTile Check = {};
						Check.m_Number = 1;
						Check.m_Type = (unsigned char)Index;
						std::get<CTileStore<CTeleTile>>(Changed.m_ExtraTiles).Set(x + OffsetX, y + OffsetY, Check);
					}
					else
					{
						CTile Wall = {};
						Wall.m_Index = (unsigned char)Index;
						Changed.m_Tiles.Set(x + OffsetX, y + OffsetY, Wall);
					}
					++Written;
				}
			}
		});
		return Written;
	}

	void PaintTiles(CDocument &Doc, size_t Group, size_t Layer, int x, int y, const CBrush &Brush)
	{
		EditTileLayer(Doc, Group, Layer, [&](CTileLayer &Changed) {
			StampTiles(Changed, x, y, Brush);
		});
	}
} // namespace map_document
