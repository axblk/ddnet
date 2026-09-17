#include "tiles.h"

#include <base/str.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>

#include <game/map/document/document.h>
#include <game/map/document/explain.h>
#include <game/map/document/structure.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace map_document
{
	CTileRect ClipTileRect(const CTileLayer &Layer, const CTileRect &Rect)
	{
		CTileRect Clipped;
		const int Left = std::max(0, Rect.m_X);
		const int Top = std::max(0, Rect.m_Y);
		const int Right = std::min(Layer.Width(), Rect.Right());
		const int Bottom = std::min(Layer.Height(), Rect.Bottom());
		if(Right <= Left || Bottom <= Top)
			return Clipped;
		Clipped.m_X = Left;
		Clipped.m_Y = Top;
		Clipped.m_Width = Right - Left;
		Clipped.m_Height = Bottom - Top;
		return Clipped;
	}

	bool ReadTileEncoding(const char *pName, ETileEncoding *pOut)
	{
		if(str_comp(pName, "rle") == 0)
			*pOut = ETileEncoding::RLE;
		else if(str_comp(pName, "rows") == 0)
			*pOut = ETileEncoding::ROWS;
		else if(str_comp(pName, "sparse") == 0)
			*pOut = ETileEncoding::SPARSE;
		else if(str_comp(pName, "glyph") == 0)
			*pOut = ETileEncoding::GLYPH;
		else
			return false;
		return true;
	}

	const char *TileEncodingName(ETileEncoding Encoding)
	{
		switch(Encoding)
		{
		case ETileEncoding::RLE: return "rle";
		case ETileEncoding::ROWS: return "rows";
		case ETileEncoding::SPARSE: return "sparse";
		case ETileEncoding::GLYPH: return "glyph";
		}
		return "rle";
	}

	CTileValue GetTileValue(const CTileLayer &Layer, int x, int y)
	{
		CTileValue Value;
		if(DrawsOwnTiles(Layer.m_Kind))
		{
			const CTile Tile = Layer.m_Tiles.Get(x, y);
			Value.m_aFields[0] = Tile.m_Index;
			Value.m_aFields[1] = Tile.m_Flags;
			return Value;
		}
		std::visit([&](const auto &Store) {
			using TStore = std::decay_t<decltype(Store)>;
			if constexpr(std::is_same_v<TStore, CTileStore<CTeleTile>>)
			{
				const CTeleTile Tile = Store.Get(x, y);
				Value.m_aFields[0] = Tile.m_Type;
				Value.m_aFields[1] = Tile.m_Number;
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CTuneTile>>)
			{
				const CTuneTile Tile = Store.Get(x, y);
				Value.m_aFields[0] = Tile.m_Type;
				Value.m_aFields[1] = Tile.m_Number;
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
			{
				const CSwitchTile Tile = Store.Get(x, y);
				Value.m_aFields[0] = Tile.m_Type;
				Value.m_aFields[1] = Tile.m_Number;
				Value.m_aFields[2] = Tile.m_Delay;
				Value.m_aFields[3] = Tile.m_Flags;
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				const CSpeedupTile Tile = Store.Get(x, y);
				Value.m_aFields[0] = Tile.m_Type;
				Value.m_aFields[1] = Tile.m_Force;
				Value.m_aFields[2] = Tile.m_MaxSpeed;
				Value.m_aFields[3] = Tile.m_Angle;
			}
		},
			Layer.m_ExtraTiles);
		return Value;
	}

	void SetTileValue(CTileLayer &Layer, int x, int y, const CTileValue &Value)
	{
		if(DrawsOwnTiles(Layer.m_Kind))
		{
			CTile Tile = {};
			Tile.m_Index = (unsigned char)Value.m_aFields[0];
			Tile.m_Flags = (unsigned char)Value.m_aFields[1];
			Layer.m_Tiles.Set(x, y, Tile);
			return;
		}
		// The drawn plane of a physics layer is air, whatever it means - see
		// `DrawsOwnTiles`.
		Layer.m_Tiles.Set(x, y, CTile{});
		std::visit([&](auto &Store) {
			using TStore = std::decay_t<decltype(Store)>;
			if constexpr(std::is_same_v<TStore, CTileStore<CTeleTile>>)
			{
				CTeleTile Tile = {};
				Tile.m_Type = (unsigned char)Value.m_aFields[0];
				Tile.m_Number = (unsigned char)Value.m_aFields[1];
				Store.Set(x, y, Tile);
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CTuneTile>>)
			{
				CTuneTile Tile = {};
				Tile.m_Type = (unsigned char)Value.m_aFields[0];
				Tile.m_Number = (unsigned char)Value.m_aFields[1];
				Store.Set(x, y, Tile);
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSwitchTile>>)
			{
				CSwitchTile Tile = {};
				Tile.m_Type = (unsigned char)Value.m_aFields[0];
				Tile.m_Number = (unsigned char)Value.m_aFields[1];
				Tile.m_Delay = (unsigned char)Value.m_aFields[2];
				Tile.m_Flags = (unsigned char)Value.m_aFields[3];
				Store.Set(x, y, Tile);
			}
			else if constexpr(std::is_same_v<TStore, CTileStore<CSpeedupTile>>)
			{
				CSpeedupTile Tile = {};
				Tile.m_Type = (unsigned char)Value.m_aFields[0];
				Tile.m_Force = (unsigned char)Value.m_aFields[1];
				Tile.m_MaxSpeed = (unsigned char)Value.m_aFields[2];
				Tile.m_Angle = (short)Value.m_aFields[3];
				Store.Set(x, y, Tile);
			}
		},
			Layer.m_ExtraTiles);
	}

	std::string TileToken(const CTileValue &Value)
	{
		int Fields = 4;
		while(Fields > 1 && Value.m_aFields[Fields - 1] == 0)
			--Fields;
		std::string Token = std::to_string(Value.m_aFields[0]);
		for(int i = 1; i < Fields; ++i)
		{
			Token += '/';
			Token += std::to_string(Value.m_aFields[i]);
		}
		return Token;
	}

	namespace
	{
		/** How many of the four numbers a tile of this kind carries. */
		int NumFields(ETileLayerKind Kind)
		{
			switch(Kind)
			{
			case ETileLayerKind::TILES:
			case ETileLayerKind::GAME:
			case ETileLayerKind::FRONT:
			case ETileLayerKind::TELE:
			case ETileLayerKind::TUNE:
				return 2;
			case ETileLayerKind::SWITCH:
			case ETileLayerKind::SPEEDUP:
				return 4;
			}
			return 2;
		}

		/** The largest value one of the four numbers may hold. */
		int FieldLimit(ETileLayerKind Kind, int Field)
		{
			if(Kind == ETileLayerKind::SPEEDUP && Field == 3)
				return 359;
			return 255;
		}

		const char *KindName(ETileLayerKind Kind)
		{
			switch(Kind)
			{
			case ETileLayerKind::TILES: return "tiles";
			case ETileLayerKind::GAME: return "game";
			case ETileLayerKind::FRONT: return "front";
			case ETileLayerKind::TELE: return "tele";
			case ETileLayerKind::SPEEDUP: return "speedup";
			case ETileLayerKind::SWITCH: return "switch";
			case ETileLayerKind::TUNE: return "tune";
			}
			return "tiles";
		}

		bool ReadKindName(const char *pName, ETileLayerKind *pOut)
		{
			static const ETileLayerKind s_aKinds[] = {ETileLayerKind::TILES, ETileLayerKind::GAME, ETileLayerKind::FRONT, ETileLayerKind::TELE, ETileLayerKind::SPEEDUP, ETileLayerKind::SWITCH, ETileLayerKind::TUNE};
			const auto *pFound = std::find_if(std::begin(s_aKinds), std::end(s_aKinds), [pName](ETileLayerKind Kind) { return str_comp(pName, KindName(Kind)) == 0; });
			if(pFound == std::end(s_aKinds))
				return false;
			*pOut = *pFound;
			return true;
		}

		/** A whole number and nothing else, or false. */
		bool ParseNumber(const char *pStart, const char *pEnd, int *pOut)
		{
			if(pStart == pEnd)
				return false;
			int Value = 0;
			for(const char *p = pStart; p != pEnd; ++p)
			{
				if(*p < '0' || *p > '9')
					return false;
				Value = Value * 10 + (*p - '0');
				if(Value > 100000)
					return false;
			}
			*pOut = Value;
			return true;
		}
	} // namespace

	bool ParseTileToken(const char *pToken, ETileLayerKind Kind, CTileValue *pOut, std::string *pError)
	{
		CTileValue Value;
		const char *pRead = pToken;
		int Field = 0;
		while(true)
		{
			const char *pEnd = pRead;
			while(*pEnd != '\0' && *pEnd != '/')
				++pEnd;
			int Number = 0;
			if(Field >= 4 || !ParseNumber(pRead, pEnd, &Number))
			{
				*pError = std::string("'") + pToken + "' is not a tile";
				return false;
			}
			if(Field >= NumFields(Kind) && Number != 0)
			{
				*pError = std::string("a ") + KindName(Kind) + " tile carries " + std::to_string(NumFields(Kind)) + " numbers, not '" + pToken + "'";
				return false;
			}
			if(Number > FieldLimit(Kind, Field))
			{
				*pError = std::string("'") + pToken + "' is more than a tile can hold";
				return false;
			}
			Value.m_aFields[Field] = Number;
			++Field;
			if(*pEnd == '\0')
				break;
			pRead = pEnd + 1;
		}
		*pOut = Value;
		return true;
	}

	namespace
	{
		/**
		 * The glyphs that stand for the tiles somebody reads a physics
		 * layer for. Whatever else occurs gets a letter from the pool as it
		 * turns up, and the legend says which.
		 */
		struct SGlyph
		{
			char m_Glyph;
			int m_Index;
		};

		const SGlyph *FixedGlyphs(ETileLayerKind Kind, size_t *pCount)
		{
			static const SGlyph s_aGame[] = {
				{'.', TILE_AIR}, {'#', TILE_SOLID}, {'x', TILE_DEATH}, {'n', TILE_NOHOOK}, {'l', TILE_NOLASER},
				{'c', TILE_THROUGH_CUT}, {'t', TILE_THROUGH}, {'f', TILE_FREEZE}, {'u', TILE_UNFREEZE},
				{'F', TILE_DFREEZE}, {'U', TILE_DUNFREEZE}, {'S', TILE_START}, {'E', TILE_FINISH},
				{'p', ENTITY_OFFSET + ENTITY_SPAWN}, {'r', ENTITY_OFFSET + ENTITY_SPAWN_RED}, {'b', ENTITY_OFFSET + ENTITY_SPAWN_BLUE}};
			static const SGlyph s_aTele[] = {
				{'.', TILE_AIR}, {'i', TILE_TELEIN}, {'o', TILE_TELEOUT}, {'e', TILE_TELEINEVIL}, {'c', TILE_TELECHECK},
				{'C', TILE_TELECHECKOUT}, {'k', TILE_TELECHECKIN}, {'K', TILE_TELECHECKINEVIL}, {'w', TILE_TELEINWEAPON}, {'h', TILE_TELEINHOOK}};
			static const SGlyph s_aSwitch[] = {
				{'.', TILE_AIR}, {'o', TILE_SWITCHOPEN}, {'c', TILE_SWITCHCLOSE}, {'O', TILE_SWITCHTIMEDOPEN}, {'C', TILE_SWITCHTIMEDCLOSE}};
			static const SGlyph s_aAir[] = {{'.', TILE_AIR}};
			switch(Kind)
			{
			case ETileLayerKind::GAME:
			case ETileLayerKind::FRONT:
				*pCount = std::size(s_aGame);
				return s_aGame;
			case ETileLayerKind::TELE:
				*pCount = std::size(s_aTele);
				return s_aTele;
			case ETileLayerKind::SWITCH:
				*pCount = std::size(s_aSwitch);
				return s_aSwitch;
			case ETileLayerKind::TILES:
			case ETileLayerKind::SPEEDUP:
			case ETileLayerKind::TUNE:
				break;
			}
			*pCount = std::size(s_aAir);
			return s_aAir;
		}

		constexpr const char *GLYPH_POOL = "ABDGHIJLMNOPQRTVWXYZadgjkmoqsvwyz0123456789@$%&*+=";

		class CGlyphTable
		{
		public:
			explicit CGlyphTable(ETileLayerKind Kind) :
				m_Kind(Kind)
			{
				size_t Count = 0;
				const SGlyph *pFixed = FixedGlyphs(Kind, &Count);
				for(size_t i = 0; i < Count; ++i)
				{
					m_Glyphs[pFixed[i].m_Index] = pFixed[i].m_Glyph;
					m_Used.push_back(pFixed[i].m_Glyph);
				}
			}

			char Glyph(int Index)
			{
				const auto Found = m_Glyphs.find(Index);
				if(Found != m_Glyphs.end())
					return Found->second;
				for(const char *p = GLYPH_POOL; *p != '\0'; ++p)
				{
					if(std::find(m_Used.begin(), m_Used.end(), *p) == m_Used.end())
					{
						m_Used.push_back(*p);
						m_Glyphs[Index] = *p;
						m_Seen.push_back(Index);
						return *p;
					}
				}
				m_Glyphs[Index] = '?';
				return '?';
			}

			void Note(int Index)
			{
				if(std::find(m_Seen.begin(), m_Seen.end(), Index) == m_Seen.end())
					m_Seen.push_back(Index);
			}

			std::string Legend() const
			{
				std::vector<int> vIndices = m_Seen;
				std::sort(vIndices.begin(), vIndices.end());
				std::string Legend;
				for(const int Index : vIndices)
				{
					const auto Found = m_Glyphs.find(Index);
					if(Found == m_Glyphs.end())
						continue;
					Legend += Found->second;
					Legend += '=';
					Legend += std::to_string(Index);
					const char *pName = ExplainTile(m_Kind, Index);
					if(pName != nullptr)
					{
						Legend += ' ';
						// The sentence up to its first colon is the name.
						const char *pColon = str_find(pName, ":");
						Legend.append(pName, pColon == nullptr ? str_length(pName) : (size_t)(pColon - pName));
					}
					Legend += '\n';
				}
				return Legend;
			}

		private:
			ETileLayerKind m_Kind;
			std::map<int, char> m_Glyphs;
			std::vector<char> m_Used;
			std::vector<int> m_Seen;
		};
	} // namespace

	std::string EncodeTiles(const CTileLayer &Layer, const CTileRect &Rect, ETileEncoding Encoding, std::string *pLegend)
	{
		std::string Text;
		if(Rect.Empty())
			return Text;
		if(Encoding == ETileEncoding::GLYPH)
		{
			CGlyphTable Table(Layer.m_Kind);
			for(int y = Rect.m_Y; y < Rect.Bottom(); ++y)
			{
				for(int x = Rect.m_X; x < Rect.Right(); ++x)
				{
					const int Index = GetTileValue(Layer, x, y).Index();
					Text += Table.Glyph(Index);
					if(Index != 0)
						Table.Note(Index);
				}
				Text += '\n';
			}
			if(pLegend != nullptr)
				*pLegend = Table.Legend();
			return Text;
		}
		if(Encoding == ETileEncoding::SPARSE)
		{
			for(int y = Rect.m_Y; y < Rect.Bottom(); ++y)
			{
				for(int x = Rect.m_X; x < Rect.Right(); ++x)
				{
					const CTileValue Value = GetTileValue(Layer, x, y);
					if(Value.IsAir())
						continue;
					if(!Text.empty())
						Text += ' ';
					Text += std::to_string(x - Rect.m_X);
					Text += ',';
					Text += std::to_string(y - Rect.m_Y);
					Text += ':';
					Text += TileToken(Value);
				}
			}
			return Text;
		}
		const bool Runs = Encoding == ETileEncoding::RLE;
		for(int y = Rect.m_Y; y < Rect.Bottom(); ++y)
		{
			int x = Rect.m_X;
			bool First = true;
			while(x < Rect.Right())
			{
				const CTileValue Value = GetTileValue(Layer, x, y);
				int Length = 1;
				if(Runs)
				{
					while(x + Length < Rect.Right() && GetTileValue(Layer, x + Length, y) == Value)
						++Length;
				}
				if(!First)
					Text += ' ';
				First = false;
				Text += TileToken(Value);
				if(Length > 1)
				{
					Text += 'x';
					Text += std::to_string(Length);
				}
				x += Length;
			}
			Text += '\n';
		}
		return Text;
	}

	namespace
	{
		/** One token with its run, `12/8x3` being three tiles of `12/8`. */
		bool ParseRun(const std::string &Token, ETileLayerKind Kind, CTileValue *pValue, int *pLength, std::string *pError)
		{
			const size_t Times = Token.find('x');
			*pLength = 1;
			if(Times != std::string::npos)
			{
				const std::string Count = Token.substr(Times + 1);
				if(!ParseNumber(Count.c_str(), Count.c_str() + Count.size(), pLength) || *pLength <= 0)
				{
					*pError = "'" + Token + "' is not a run of tiles";
					return false;
				}
			}
			return ParseTileToken(Token.substr(0, Times).c_str(), Kind, pValue, pError);
		}

		std::vector<std::string> SplitLines(const char *pText)
		{
			std::vector<std::string> vLines;
			std::string Line;
			for(const char *p = pText; *p != '\0'; ++p)
			{
				if(*p == '\n')
				{
					vLines.push_back(Line);
					Line.clear();
				}
				else if(*p != '\r')
				{
					Line += *p;
				}
			}
			if(!Line.empty())
				vLines.push_back(Line);
			return vLines;
		}

		std::vector<std::string> SplitTokens(const std::string &Line)
		{
			std::vector<std::string> vTokens;
			std::string Token;
			for(const char c : Line)
			{
				if(c == ' ' || c == '\t' || c == '\n' || c == '\r')
				{
					if(!Token.empty())
						vTokens.push_back(Token);
					Token.clear();
				}
				else
				{
					Token += c;
				}
			}
			if(!Token.empty())
				vTokens.push_back(Token);
			return vTokens;
		}

		class CPlacedTile
		{
		public:
			int m_X;
			int m_Y;
			CTileValue m_Value;
		};
	} // namespace

	bool DecodeTiles(const char *pText, ETileEncoding Encoding, ETileLayerKind Kind, int Width, int Height, CBrush *pBrush, std::string *pError)
	{
		if(Encoding == ETileEncoding::GLYPH)
		{
			*pError = "glyphs are for reading; write tiles as rle, rows or sparse";
			return false;
		}
		std::vector<CPlacedTile> vTiles;
		int Wide = 0;
		int High = 0;
		if(Encoding == ETileEncoding::SPARSE)
		{
			for(const std::string &Item : SplitTokens(pText))
			{
				const size_t Comma = Item.find(',');
				const size_t Colon = Item.find(':');
				int X = 0;
				int Y = 0;
				if(Comma == std::string::npos || Colon == std::string::npos || Colon < Comma ||
					!ParseNumber(Item.c_str(), Item.c_str() + Comma, &X) ||
					!ParseNumber(Item.c_str() + Comma + 1, Item.c_str() + Colon, &Y))
				{
					*pError = "'" + Item + "' is not 'x,y:tile'";
					return false;
				}
				CTileValue Value;
				if(!ParseTileToken(Item.c_str() + Colon + 1, Kind, &Value, pError))
					return false;
				vTiles.push_back(CPlacedTile{X, Y, Value});
				Wide = std::max(Wide, X + 1);
				High = std::max(High, Y + 1);
			}
		}
		else
		{
			const std::vector<std::string> vLines = SplitLines(pText);
			High = (int)vLines.size();
			for(int y = 0; y < High; ++y)
			{
				int x = 0;
				for(const std::string &Token : SplitTokens(vLines[y]))
				{
					CTileValue Value;
					int Length = 0;
					if(!ParseRun(Token, Kind, &Value, &Length, pError))
					{
						*pError += " (row " + std::to_string(y) + ")";
						return false;
					}
					for(int i = 0; i < Length; ++i)
					{
						if(!Value.IsAir())
							vTiles.push_back(CPlacedTile{x + i, y, Value});
					}
					x += Length;
				}
				Wide = std::max(Wide, x);
			}
		}
		if(Width <= 0)
			Width = Wide;
		if(Height <= 0)
			Height = High;
		if(Width <= 0 || Height <= 0)
		{
			*pError = "there are no tiles in the text";
			return false;
		}
		if(Wide > Width || High > Height)
		{
			*pError = "the text holds " + std::to_string(Wide) + " by " + std::to_string(High) + " tiles, more than the " + std::to_string(Width) + " by " + std::to_string(Height) + " asked for";
			return false;
		}
		*pBrush = CBrush(Kind, Width, Height);
		for(const CPlacedTile &Tile : vTiles)
			SetTileValue(*pBrush, Tile.m_X, Tile.m_Y, Tile.m_Value);
		return true;
	}

	bool TileIsUsed(ETileLayerKind Kind, int Index)
	{
		if(Kind == ETileLayerKind::TILES || Index == 0)
			return true;
		CBrush Brush(Kind, 1, 1);
		CTileValue Value;
		Value.m_aFields[0] = Index;
		SetTileValue(Brush, 0, 0, Value);
		return DropUnusedTiles(Brush) == 0;
	}

	int WriteTiles(CTileLayer &Layer, int x, int y, const CBrush &Brush, bool Overlay)
	{
		int Written = 0;
		for(int BrushY = 0; BrushY < Brush.Height(); ++BrushY)
		{
			const int LayerY = y + BrushY;
			if(LayerY < 0 || LayerY >= Layer.Height())
				continue;
			for(int BrushX = 0; BrushX < Brush.Width(); ++BrushX)
			{
				const int LayerX = x + BrushX;
				if(LayerX < 0 || LayerX >= Layer.Width())
					continue;
				const CTileValue Value = GetTileValue(Brush, BrushX, BrushY);
				if(Overlay && Value.IsAir())
					continue;
				SetTileValue(Layer, LayerX, LayerY, Value);
				++Written;
			}
		}
		return Written;
	}

	int FillTileRect(CTileLayer &Layer, const CTileRect &Rect, const CTileValue &Value, int Border)
	{
		const CTileRect Clipped = ClipTileRect(Layer, Rect);
		int Written = 0;
		for(int y = Clipped.m_Y; y < Clipped.Bottom(); ++y)
		{
			for(int x = Clipped.m_X; x < Clipped.Right(); ++x)
			{
				// The rim is measured on the rectangle that was asked for, not
				// on what was left of it after clipping - a border along the
				// edge of the layer is still the border of the rectangle.
				if(Border > 0)
				{
					const bool OnRim = x < Rect.m_X + Border || x >= Rect.Right() - Border || y < Rect.m_Y + Border || y >= Rect.Bottom() - Border;
					if(!OnRim)
						continue;
				}
				SetTileValue(Layer, x, y, Value);
				++Written;
			}
		}
		return Written;
	}

	int ReplaceTileIndex(CTileLayer &Layer, const CTileRect &Rect, int From, int To)
	{
		const CTileRect Clipped = ClipTileRect(Layer, Rect);
		int Changed = 0;
		for(int y = Clipped.m_Y; y < Clipped.Bottom(); ++y)
		{
			for(int x = Clipped.m_X; x < Clipped.Right(); ++x)
			{
				CTileValue Value = GetTileValue(Layer, x, y);
				if(Value.Index() != From)
					continue;
				Value.m_aFields[0] = To;
				if(To == 0)
					Value = CTileValue();
				SetTileValue(Layer, x, y, Value);
				++Changed;
			}
		}
		return Changed;
	}

	std::vector<CTileRun> FindTileRuns(const CTileLayer &Layer, const CTileRect &Rect, const std::vector<int> &vIndices, size_t Limit, size_t *pTotal)
	{
		std::vector<CTileRun> vRuns;
		*pTotal = 0;
		const CTileRect Clipped = ClipTileRect(Layer, Rect);
		for(int y = Clipped.m_Y; y < Clipped.Bottom(); ++y)
		{
			int x = Clipped.m_X;
			while(x < Clipped.Right())
			{
				const int Index = GetTileValue(Layer, x, y).Index();
				if(std::find(vIndices.begin(), vIndices.end(), Index) == vIndices.end())
				{
					++x;
					continue;
				}
				int Length = 1;
				while(x + Length < Clipped.Right() && GetTileValue(Layer, x + Length, y).Index() == Index)
					++Length;
				++*pTotal;
				if(vRuns.size() < Limit)
					vRuns.push_back(CTileRun{x, y, Length, Index});
				x += Length;
			}
		}
		return vRuns;
	}

	CTileStats CountTiles(const CTileLayer &Layer, const CTileRect &Rect)
	{
		CTileStats Stats;
		const CTileRect Clipped = ClipTileRect(Layer, Rect);
		std::map<int, size_t> Counts;
		int Left = Clipped.Right();
		int Top = Clipped.Bottom();
		int Right = Clipped.m_X;
		int Bottom = Clipped.m_Y;
		// Block by block, so that a layer that is mostly air is mostly skipped.
		constexpr int CHUNK = CTileStore<CTile>::CHUNK_SIZE;
		for(int ChunkY = Clipped.m_Y / CHUNK; ChunkY * CHUNK < Clipped.Bottom(); ++ChunkY)
		{
			for(int ChunkX = Clipped.m_X / CHUNK; ChunkX * CHUNK < Clipped.Right(); ++ChunkX)
			{
				const bool Air = DrawsOwnTiles(Layer.m_Kind) ? Layer.m_Tiles.Chunk(ChunkX, ChunkY) == nullptr : std::visit([&](const auto &Store) {
					using TStore = std::decay_t<decltype(Store)>;
					if constexpr(std::is_same_v<TStore, std::monostate>)
						return true;
					else
						return Store.Chunk(ChunkX, ChunkY) == nullptr;
				},
																	Layer.m_ExtraTiles);
				if(Air)
					continue;
				for(int y = std::max(Clipped.m_Y, ChunkY * CHUNK); y < std::min(Clipped.Bottom(), (ChunkY + 1) * CHUNK); ++y)
				{
					for(int x = std::max(Clipped.m_X, ChunkX * CHUNK); x < std::min(Clipped.Right(), (ChunkX + 1) * CHUNK); ++x)
					{
						const CTileValue Value = GetTileValue(Layer, x, y);
						if(Value.IsAir())
							continue;
						++Counts[Value.Index()];
						++Stats.m_Tiles;
						Left = std::min(Left, x);
						Top = std::min(Top, y);
						Right = std::max(Right, x + 1);
						Bottom = std::max(Bottom, y + 1);
					}
				}
			}
		}
		if(Stats.m_Tiles > 0)
		{
			Stats.m_Bounds.m_X = Left;
			Stats.m_Bounds.m_Y = Top;
			Stats.m_Bounds.m_Width = Right - Left;
			Stats.m_Bounds.m_Height = Bottom - Top;
		}
		for(const auto &[Index, Count] : Counts)
			Stats.m_vCounts.push_back(CTileCount{Index, Count});
		return Stats;
	}

	size_t ChangedChunks(const CTileLayer &Before, const CTileLayer &After)
	{
		size_t Changed = 0;
		After.m_Tiles.ForEachChangedChunk(Before.m_Tiles, [&](int, int) { ++Changed; });
		if(After.m_ExtraTiles.index() != Before.m_ExtraTiles.index())
			return Changed;
		std::visit([&](const auto &Store) {
			using TStore = std::decay_t<decltype(Store)>;
			if constexpr(!std::is_same_v<TStore, std::monostate>)
			{
				const TStore &Older = std::get<TStore>(Before.m_ExtraTiles);
				Store.ForEachChangedChunk(Older, [&](int, int) { ++Changed; });
			}
		},
			After.m_ExtraTiles);
		return Changed;
	}

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

		int IntArg(const json_value *pCommand, const char *pName, int Default, bool *pFailed, std::string *pError)
		{
			const json_value *pValue = json_object_get(pCommand, pName);
			if(pValue->type == json_none)
				return Default;
			if(pValue->type != json_integer)
			{
				if(pError->empty())
					*pError = std::string("'") + pName + "' has to be a whole number";
				*pFailed = true;
				return Default;
			}
			return json_int_get(pValue);
		}

		const char *StrArg(const json_value *pCommand, const char *pName, const char *pDefault)
		{
			const json_value *pValue = json_object_get(pCommand, pName);
			return pValue->type == json_string ? json_string_get(pValue) : pDefault;
		}

		bool BoolArg(const json_value *pCommand, const char *pName, bool Default)
		{
			const json_value *pValue = json_object_get(pCommand, pName);
			return pValue->type == json_boolean ? json_boolean_get(pValue) != 0 : Default;
		}

		/** Where the command points, as a rectangle; missing sides mean the whole layer. */
		CTileRect RectArg(const json_value *pCommand, const CTileLayer &Layer, bool *pFailed, std::string *pError)
		{
			CTileRect Rect;
			Rect.m_X = IntArg(pCommand, "x", 0, pFailed, pError);
			Rect.m_Y = IntArg(pCommand, "y", 0, pFailed, pError);
			Rect.m_Width = IntArg(pCommand, "w", Layer.Width() - Rect.m_X, pFailed, pError);
			Rect.m_Height = IntArg(pCommand, "h", Layer.Height() - Rect.m_Y, pFailed, pError);
			return Rect;
		}

		/**
		 * The tile layer a command names, or an error. `group` and `layer`
		 * have to be there and have to name a layer that holds tiles.
		 */
		const CTileLayer *LayerArg(const CMapState &Map, const json_value *pCommand, CLayerAddress *pAddress, std::string *pError)
		{
			bool Failed = false;
			const int Group = IntArg(pCommand, "group", -1, &Failed, pError);
			const int Layer = IntArg(pCommand, "layer", -1, &Failed, pError);
			if(Failed)
				return nullptr;
			if(Group < 0 || (size_t)Group >= Map.NumGroups())
			{
				*pError = "'group' is " + std::to_string(Group) + ", which is not there";
				return nullptr;
			}
			if(Layer < 0 || (size_t)Layer >= Map.NumLayers(Group))
			{
				*pError = "'layer' is " + std::to_string(Layer) + ", which is not in group " + std::to_string(Group);
				return nullptr;
			}
			const CTileLayer *pTiles = std::get_if<CTileLayer>(Map.Layer(Group, Layer));
			if(pTiles == nullptr)
			{
				*pError = "layer " + std::to_string(Layer) + " of group " + std::to_string(Group) + " holds no tiles";
				return nullptr;
			}
			pAddress->m_Group = Group;
			pAddress->m_Layer = Layer;
			return pTiles;
		}

		/**
		 * Every tile layer a command means: the one it names, or all of a
		 * kind when it names none.
		 */
		std::vector<CLayerAddress> LayersArg(const CMapState &Map, const json_value *pCommand, std::optional<ETileLayerKind> DefaultKind, std::string *pError)
		{
			std::vector<CLayerAddress> vLayers;
			if(json_object_get(pCommand, "group")->type != json_none || json_object_get(pCommand, "layer")->type != json_none)
			{
				CLayerAddress Address;
				if(LayerArg(Map, pCommand, &Address, pError) == nullptr)
					return vLayers;
				vLayers.push_back(Address);
				return vLayers;
			}
			std::optional<ETileLayerKind> Kind = DefaultKind;
			const char *pKind = StrArg(pCommand, "kind", nullptr);
			if(pKind != nullptr)
			{
				ETileLayerKind Read;
				if(!ReadKindName(pKind, &Read))
				{
					*pError = std::string("there is no kind of tile layer called '") + pKind + "'";
					return vLayers;
				}
				Kind = Read;
			}
			for(size_t Group = 0; Group < Map.NumGroups(); ++Group)
			{
				for(size_t Layer = 0; Layer < Map.NumLayers(Group); ++Layer)
				{
					const CTileLayer *pTiles = std::get_if<CTileLayer>(Map.Layer(Group, Layer));
					if(pTiles != nullptr && (!Kind.has_value() || pTiles->m_Kind == *Kind))
						vLayers.push_back(CLayerAddress{Group, Layer});
				}
			}
			if(vLayers.empty())
				*pError = Kind.has_value() ? std::string("the map has no ") + KindName(*Kind) + " layer" : std::string("the map has no tile layer");
			return vLayers;
		}

		void WriteRect(CJsonWriter &Writer, const CTileRect &Rect)
		{
			Writer.WriteAttribute("x");
			Writer.WriteIntValue(Rect.m_X);
			Writer.WriteAttribute("y");
			Writer.WriteIntValue(Rect.m_Y);
			Writer.WriteAttribute("w");
			Writer.WriteIntValue(Rect.m_Width);
			Writer.WriteAttribute("h");
			Writer.WriteIntValue(Rect.m_Height);
		}

		/** The `ok`, and what changed, for a command that wrote to one layer. */
		std::string Changed(const CDocument &Document, const CLayerAddress &Address, const CTileLayer &Before, const char *pName, int Count, int Dropped)
		{
			const CTileLayer &After = *Document.Map().TileLayer(Address.m_Group, Address.m_Layer);
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(true);
			Writer.WriteAttribute(pName);
			Writer.WriteIntValue(Count);
			if(Dropped > 0)
			{
				Writer.WriteAttribute("dropped");
				Writer.WriteIntValue(Dropped);
			}
			Writer.WriteAttribute("chunks");
			Writer.WriteIntValue((int)ChangedChunks(Before, After));
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		/** How many tiles one read may hand back, and with `large`. */
		constexpr int READ_SIDE = 128;
		constexpr int READ_SIDE_LARGE = 256;
		/** How many tiles one write may take. */
		constexpr int64_t WRITE_TILES = 1000 * 1000;
	} // namespace

	std::string ApplyTilesCommand(CDocument &Document, const json_value *pCommand, const char *pOp, const char *pMerge)
	{
		const CMapState &Map = Document.Map();
		std::string Error;
		bool Failed = false;

		if(str_comp(pOp, "tiles.read") == 0)
		{
			CLayerAddress Address;
			const CTileLayer *pLayer = LayerArg(Map, pCommand, &Address, &Error);
			if(pLayer == nullptr)
				return map_document::Failed(Error);
			const CTileRect Asked = RectArg(pCommand, *pLayer, &Failed, &Error);
			if(Failed)
				return map_document::Failed(Error);
			ETileEncoding Encoding = ETileEncoding::RLE;
			if(!ReadTileEncoding(StrArg(pCommand, "encoding", "rle"), &Encoding))
				return map_document::Failed("there is no encoding of that name; rle, rows, sparse and glyph are the ones there are");
			const CTileRect Rect = ClipTileRect(*pLayer, Asked);
			if(Rect.Empty())
				return map_document::Failed("that rectangle lies off the layer, which is " + std::to_string(pLayer->Width()) + " by " + std::to_string(pLayer->Height()));
			const int Side = BoolArg(pCommand, "large", false) ? READ_SIDE_LARGE : READ_SIDE;
			if((int64_t)Rect.m_Width * Rect.m_Height > (int64_t)Side * Side)
				return map_document::Failed("that is more than " + std::to_string(Side) + " by " + std::to_string(Side) + " tiles; read it in pieces, or ask tiles.stats and tiles.find first");
			std::string Legend;
			const std::string Text = EncodeTiles(*pLayer, Rect, Encoding, &Legend);
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(true);
			Writer.WriteAttribute("kind");
			Writer.WriteStrValue(KindName(pLayer->m_Kind));
			WriteRect(Writer, Rect);
			Writer.WriteAttribute("encoding");
			Writer.WriteStrValue(TileEncodingName(Encoding));
			Writer.WriteAttribute("tiles");
			Writer.WriteStrValue(Text.c_str());
			if(Encoding == ETileEncoding::GLYPH)
			{
				Writer.WriteAttribute("legend");
				Writer.WriteStrValue(Legend.c_str());
			}
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		if(str_comp(pOp, "tiles.write") == 0)
		{
			CLayerAddress Address;
			const CTileLayer *pLayer = LayerArg(Map, pCommand, &Address, &Error);
			if(pLayer == nullptr)
				return map_document::Failed(Error);
			const int X = IntArg(pCommand, "x", 0, &Failed, &Error);
			const int Y = IntArg(pCommand, "y", 0, &Failed, &Error);
			const int Width = IntArg(pCommand, "w", 0, &Failed, &Error);
			const int Height = IntArg(pCommand, "h", 0, &Failed, &Error);
			if(Failed)
				return map_document::Failed(Error);
			const char *pText = StrArg(pCommand, "tiles", nullptr);
			if(pText == nullptr)
				return map_document::Failed("The command has no 'tiles'");
			ETileEncoding Encoding = ETileEncoding::RLE;
			if(!ReadTileEncoding(StrArg(pCommand, "encoding", "rle"), &Encoding))
				return map_document::Failed("there is no encoding of that name; rle, rows and sparse can be written");
			const char *pMode = StrArg(pCommand, "mode", "replace");
			const bool Overlay = str_comp(pMode, "overlay") == 0;
			if(!Overlay && str_comp(pMode, "replace") != 0)
				return map_document::Failed("'mode' is replace or overlay");
			if((int64_t)std::max(Width, 0) * std::max(Height, 0) > WRITE_TILES)
				return map_document::Failed("that is more than a million tiles at once");
			CBrush Brush;
			if(!DecodeTiles(pText, Encoding, pLayer->m_Kind, Width, Height, &Brush, &Error))
				return map_document::Failed(Error);
			if((int64_t)Brush.Width() * Brush.Height() > WRITE_TILES)
				return map_document::Failed("that is more than a million tiles at once");
			const int Dropped = BoolArg(pCommand, "allowUnused", false) ? 0 : DropUnusedTiles(Brush);
			const CTileLayer Before = *pLayer;
			Document.Begin(StrArg(pCommand, "label", "Write tiles"), pMerge);
			int Written = 0;
			EditTileLayer(Document, Address.m_Group, Address.m_Layer, [&](CTileLayer &Layer) {
				Written = WriteTiles(Layer, X, Y, Brush, Overlay);
			});
			Document.Commit();
			return Changed(Document, Address, Before, "written", Written, Dropped);
		}

		if(str_comp(pOp, "tiles.fill") == 0)
		{
			CLayerAddress Address;
			const CTileLayer *pLayer = LayerArg(Map, pCommand, &Address, &Error);
			if(pLayer == nullptr)
				return map_document::Failed(Error);
			const CTileRect Rect = RectArg(pCommand, *pLayer, &Failed, &Error);
			CTileValue Value;
			Value.m_aFields[0] = IntArg(pCommand, "index", -1, &Failed, &Error);
			const int Border = IntArg(pCommand, "border", 0, &Failed, &Error);
			static const char *const s_apFields[][4] = {
				{"flags", nullptr, nullptr, nullptr},
				{"number", nullptr, nullptr, nullptr},
				{"number", "delay", "flags", nullptr},
				{"force", "maxSpeed", "angle", nullptr},
			};
			const int Table = DrawsOwnTiles(pLayer->m_Kind) ? 0 : pLayer->m_Kind == ETileLayerKind::SWITCH ? 2 :
								      pLayer->m_Kind == ETileLayerKind::SPEEDUP        ? 3 :
															 1;
			for(int Field = 1; Field < 4 && s_apFields[Table][Field - 1] != nullptr; ++Field)
				Value.m_aFields[Field] = IntArg(pCommand, s_apFields[Table][Field - 1], 0, &Failed, &Error);
			if(Failed)
				return map_document::Failed(Error);
			if(Value.m_aFields[0] < 0)
				return map_document::Failed("The command has no 'index'");
			for(int Field = 0; Field < 4; ++Field)
			{
				if(Value.m_aFields[Field] < 0 || Value.m_aFields[Field] > FieldLimit(pLayer->m_Kind, Field))
					return map_document::Failed("a tile's numbers are 0 to 255");
			}
			if(Rect.m_Width <= 0 || Rect.m_Height <= 0)
				return map_document::Failed("a rectangle has a width and a height");
			if((int64_t)Rect.m_Width * Rect.m_Height > WRITE_TILES)
				return map_document::Failed("that is more than a million tiles at once");
			if(!BoolArg(pCommand, "allowUnused", false) && !TileIsUsed(pLayer->m_Kind, Value.Index()))
				return map_document::Failed("index " + std::to_string(Value.Index()) + " does nothing in a " + KindName(pLayer->m_Kind) + " layer; pass allowUnused to write it anyway");
			if(Border < 0)
				return map_document::Failed("a border is at least nothing");
			const CTileLayer Before = *pLayer;
			Document.Begin(StrArg(pCommand, "label", "Fill tiles"), pMerge);
			int Written = 0;
			EditTileLayer(Document, Address.m_Group, Address.m_Layer, [&](CTileLayer &Layer) {
				Written = FillTileRect(Layer, Rect, Value, Border);
			});
			Document.Commit();
			return Changed(Document, Address, Before, "written", Written, 0);
		}

		if(str_comp(pOp, "tiles.replace") == 0)
		{
			const int From = IntArg(pCommand, "from", -1, &Failed, &Error);
			const int To = IntArg(pCommand, "to", -1, &Failed, &Error);
			if(Failed)
				return map_document::Failed(Error);
			if(From < 0 || To < 0)
				return map_document::Failed("The command needs 'from' and 'to'");
			if(From > 255 || To > 255)
				return map_document::Failed("an index is 0 to 255");
			const std::vector<CLayerAddress> vLayers = LayersArg(Map, pCommand, ETileLayerKind::GAME, &Error);
			if(vLayers.empty())
				return map_document::Failed(Error);
			for(const CLayerAddress &Address : vLayers)
			{
				const CTileLayer *pLayer = Map.TileLayer(Address.m_Group, Address.m_Layer);
				if(!BoolArg(pCommand, "allowUnused", false) && !TileIsUsed(pLayer->m_Kind, To))
					return map_document::Failed("index " + std::to_string(To) + " does nothing in a " + KindName(pLayer->m_Kind) + " layer; pass allowUnused to write it anyway");
			}
			std::vector<std::pair<CLayerAddress, int>> vCounts;
			size_t Chunks = 0;
			Document.Begin(StrArg(pCommand, "label", "Replace tiles"), pMerge);
			for(const CLayerAddress &Address : vLayers)
			{
				const CTileLayer Before = *Document.Map().TileLayer(Address.m_Group, Address.m_Layer);
				const CTileRect Rect = RectArg(pCommand, Before, &Failed, &Error);
				if(Failed)
				{
					Document.Abort();
					return map_document::Failed(Error);
				}
				int Count = 0;
				EditTileLayer(Document, Address.m_Group, Address.m_Layer, [&](CTileLayer &Layer) {
					Count = ReplaceTileIndex(Layer, Rect, From, To);
				});
				Chunks += ChangedChunks(Before, *Document.Map().TileLayer(Address.m_Group, Address.m_Layer));
				vCounts.emplace_back(Address, Count);
			}
			Document.Commit();
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(true);
			int Total = 0;
			Writer.WriteAttribute("layers");
			Writer.BeginArray();
			for(const auto &[Address, Count] : vCounts)
			{
				Total += Count;
				Writer.BeginObject();
				Writer.WriteAttribute("group");
				Writer.WriteIntValue((int)Address.m_Group);
				Writer.WriteAttribute("layer");
				Writer.WriteIntValue((int)Address.m_Layer);
				Writer.WriteAttribute("replaced");
				Writer.WriteIntValue(Count);
				Writer.EndObject();
			}
			Writer.EndArray();
			Writer.WriteAttribute("replaced");
			Writer.WriteIntValue(Total);
			Writer.WriteAttribute("chunks");
			Writer.WriteIntValue((int)Chunks);
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		if(str_comp(pOp, "tiles.find") == 0)
		{
			std::vector<int> vIndices;
			const json_value *pIndices = json_object_get(pCommand, "indices");
			if(pIndices->type == json_array)
			{
				for(int i = 0; i < json_array_length(pIndices); ++i)
				{
					const json_value *pIndex = json_array_get(pIndices, i);
					if(pIndex->type != json_integer)
						return map_document::Failed("'indices' is a list of whole numbers");
					vIndices.push_back(json_int_get(pIndex));
				}
			}
			const int Index = IntArg(pCommand, "index", -1, &Failed, &Error);
			const int Limit = IntArg(pCommand, "limit", 200, &Failed, &Error);
			if(Failed)
				return map_document::Failed(Error);
			if(Index >= 0)
				vIndices.push_back(Index);
			if(vIndices.empty())
				return map_document::Failed("The command needs an 'index' or 'indices'");
			if(Limit <= 0 || Limit > 10000)
				return map_document::Failed("'limit' is 1 to 10000");
			const std::vector<CLayerAddress> vLayers = LayersArg(Map, pCommand, ETileLayerKind::GAME, &Error);
			if(vLayers.empty())
				return map_document::Failed(Error);
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(true);
			Writer.WriteAttribute("runs");
			Writer.BeginArray();
			size_t Total = 0;
			size_t Listed = 0;
			for(const CLayerAddress &Address : vLayers)
			{
				const CTileLayer *pLayer = Map.TileLayer(Address.m_Group, Address.m_Layer);
				const CTileRect Rect = RectArg(pCommand, *pLayer, &Failed, &Error);
				if(Failed)
					return map_document::Failed(Error);
				size_t LayerTotal = 0;
				const size_t Left = (size_t)Limit > Listed ? (size_t)Limit - Listed : 0;
				const std::vector<CTileRun> vRuns = FindTileRuns(*pLayer, Rect, vIndices, Left, &LayerTotal);
				Total += LayerTotal;
				for(const CTileRun &Run : vRuns)
				{
					++Listed;
					Writer.BeginObject();
					Writer.WriteAttribute("group");
					Writer.WriteIntValue((int)Address.m_Group);
					Writer.WriteAttribute("layer");
					Writer.WriteIntValue((int)Address.m_Layer);
					Writer.WriteAttribute("x");
					Writer.WriteIntValue(Run.m_X);
					Writer.WriteAttribute("y");
					Writer.WriteIntValue(Run.m_Y);
					Writer.WriteAttribute("len");
					Writer.WriteIntValue(Run.m_Length);
					Writer.WriteAttribute("index");
					Writer.WriteIntValue(Run.m_Index);
					Writer.EndObject();
				}
			}
			Writer.EndArray();
			Writer.WriteAttribute("total");
			Writer.WriteIntValue((int)Total);
			Writer.WriteAttribute("truncated");
			Writer.WriteBoolValue(Total > Listed);
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		if(str_comp(pOp, "tiles.stats") == 0)
		{
			const std::vector<CLayerAddress> vLayers = LayersArg(Map, pCommand, std::nullopt, &Error);
			if(vLayers.empty())
				return map_document::Failed(Error);
			CJsonStringWriter Writer;
			Writer.BeginObject();
			Writer.WriteAttribute("ok");
			Writer.WriteBoolValue(true);
			Writer.WriteAttribute("layers");
			Writer.BeginArray();
			for(const CLayerAddress &Address : vLayers)
			{
				const CTileLayer *pLayer = Map.TileLayer(Address.m_Group, Address.m_Layer);
				const CTileRect Rect = RectArg(pCommand, *pLayer, &Failed, &Error);
				if(Failed)
					return map_document::Failed(Error);
				const CTileStats Stats = CountTiles(*pLayer, Rect);
				Writer.BeginObject();
				Writer.WriteAttribute("group");
				Writer.WriteIntValue((int)Address.m_Group);
				Writer.WriteAttribute("layer");
				Writer.WriteIntValue((int)Address.m_Layer);
				Writer.WriteAttribute("name");
				Writer.WriteStrValue(pLayer->m_Name.c_str());
				Writer.WriteAttribute("kind");
				Writer.WriteStrValue(KindName(pLayer->m_Kind));
				Writer.WriteAttribute("width");
				Writer.WriteIntValue(pLayer->Width());
				Writer.WriteAttribute("height");
				Writer.WriteIntValue(pLayer->Height());
				Writer.WriteAttribute("chunks");
				Writer.WriteIntValue(DrawsOwnTiles(pLayer->m_Kind) ? pLayer->m_Tiles.UsedChunks() : std::visit([](const auto &Store) {
					using TStore = std::decay_t<decltype(Store)>;
					if constexpr(std::is_same_v<TStore, std::monostate>)
						return 0;
					else
						return Store.UsedChunks();
				},
															    pLayer->m_ExtraTiles));
				Writer.WriteAttribute("tiles");
				Writer.WriteIntValue((int)Stats.m_Tiles);
				Writer.WriteAttribute("bounds");
				Writer.BeginObject();
				WriteRect(Writer, Stats.m_Bounds);
				Writer.EndObject();
				Writer.WriteAttribute("histogram");
				Writer.BeginArray();
				for(const CTileCount &Count : Stats.m_vCounts)
				{
					Writer.BeginObject();
					Writer.WriteAttribute("index");
					Writer.WriteIntValue(Count.m_Index);
					Writer.WriteAttribute("count");
					Writer.WriteIntValue((int)Count.m_Count);
					if(pLayer->m_Kind != ETileLayerKind::TILES)
					{
						const char *pName = ExplainTile(pLayer->m_Kind, Count.m_Index);
						Writer.WriteAttribute("name");
						if(pName == nullptr)
							Writer.WriteNullValue();
						else
							Writer.WriteStrValue(pName);
						Writer.WriteAttribute("used");
						Writer.WriteBoolValue(TileIsUsed(pLayer->m_Kind, Count.m_Index));
					}
					Writer.EndObject();
				}
				Writer.EndArray();
				Writer.EndObject();
			}
			Writer.EndArray();
			Writer.EndObject();
			return Writer.GetOutputString();
		}

		return map_document::Failed(std::string("There is no command '") + pOp + "'");
	}
} // namespace map_document
