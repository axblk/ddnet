#include <base/str.h>

#include <game/map/document/automap.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cstdint>
#include <cstdio> // sscanf
#include <cstdlib>
#include <vector>

namespace map_document
{
	namespace
	{
		// Based on triple32inc from
		// https://github.com/skeeto/hash-prospector/tree/79a6074062a84907df6e45b756134b74e2956760
		uint32_t HashUInt32(uint32_t Num)
		{
			Num++;
			Num ^= Num >> 17;
			Num *= 0xed5ad4bbu;
			Num ^= Num >> 11;
			Num *= 0xac4c1b51u;
			Num ^= Num >> 15;
			Num *= 0x31848babu;
			Num ^= Num >> 14;
			return Num;
		}

		constexpr int HASH_MAX = 65536;

		// Which of the rules that only fire sometimes fires here. A hash of
		// the place rather than a roll of the dice, so that automapping the
		// same layer twice with the same seed gives the same map - which is
		// what makes automapping a stroke as it is drawn possible.
		int HashLocation(uint32_t Seed, uint32_t Run, uint32_t Rule, uint32_t X, uint32_t Y)
		{
			const uint32_t Prime = 31;
			uint32_t Hash = 1;
			Hash = Hash * Prime + HashUInt32(Seed);
			Hash = Hash * Prime + HashUInt32(Run);
			Hash = Hash * Prime + HashUInt32(Rule);
			Hash = Hash * Prime + HashUInt32(X);
			Hash = Hash * Prime + HashUInt32(Y);
			// Just to double-check that values are well-distributed
			Hash = HashUInt32(Hash * Prime);
			return (int)(Hash % HASH_MAX);
		}

		int WithFlag(int Flag, const char *pFlag, bool CheckNone)
		{
			if(str_comp(pFlag, "XFLIP") == 0)
				Flag |= TILEFLAG_XFLIP;
			else if(str_comp(pFlag, "YFLIP") == 0)
				Flag |= TILEFLAG_YFLIP;
			else if(str_comp(pFlag, "ROTATE") == 0)
				Flag |= TILEFLAG_ROTATE;
			else if(str_comp(pFlag, "NONE") == 0 && CheckNone)
				Flag = 0;
			return Flag;
		}

		/** One line of the file, without the newline it ended with. */
		class CLines
		{
		public:
			explicit CLines(const char *pText) :
				m_pAt(pText == nullptr ? "" : pText) {}

			bool Next(std::string *pLine)
			{
				if(*m_pAt == '\0')
					return false;
				const char *pStart = m_pAt;
				while(*m_pAt != '\0' && *m_pAt != '\n')
					++m_pAt;
				size_t Length = (size_t)(m_pAt - pStart);
				// A file written on Windows ends its lines with two
				// characters, and the second is not part of the word.
				if(Length > 0 && pStart[Length - 1] == '\r')
					--Length;
				pLine->assign(pStart, Length);
				if(*m_pAt == '\n')
					++m_pAt;
				return true;
			}

		private:
			const char *m_pAt;
		};

		/** The tiles of a layer as a flat plane, which is what the rules read. */
		std::vector<CTile> Flatten(const CTileStore<CTile> &Tiles, int FromX, int FromY, int Width, int Height)
		{
			std::vector<CTile> vFlat((size_t)Width * (size_t)Height);
			for(int y = 0; y < Height; ++y)
				for(int x = 0; x < Width; ++x)
					vFlat[(size_t)y * (size_t)Width + (size_t)x] = Tiles.Get(FromX + x, FromY + y);
			return vFlat;
		}
	} // namespace

	const char *CAutomapRules::ConfigName(size_t Index) const
	{
		return Index >= m_vConfigs.size() ? "" : m_vConfigs[Index].m_Name.c_str();
	}

	CAutomapRules ParseAutomapRules(const char *pText, std::vector<int> *pvNotUnderstood)
	{
		CAutomapRules Rules;
		CAutomapRules::CConfig *pConfig = nullptr;
		CAutomapRules::CRun *pRun = nullptr;
		CAutomapRules::CIndexRule *pIndex = nullptr;

		CLines Lines(pText);
		std::string Line;
		int Number = 0;
		while(Lines.Next(&Line))
		{
			++Number;
			const char *pLine = Line.c_str();
			// A line that starts with a space, a tab or a hash is a comment
			// or a blank - the same rule the editor in the client uses.
			if(Line.empty() || pLine[0] == '#' || pLine[0] == '\t' || pLine[0] == '\v' || pLine[0] == ' ')
				continue;

			if(pLine[0] == '[')
			{
				CAutomapRules::CConfig Config;
				// The name is what stands between the brackets.
				const char *pName = pLine + 1;
				const size_t Length = Line.size() - 1;
				Config.m_Name.assign(pName, Length > 0 && pName[Length - 1] == ']' ? Length - 1 : Length);
				Config.m_vRuns.emplace_back();
				Rules.m_vConfigs.push_back(std::move(Config));
				pConfig = &Rules.m_vConfigs.back();
				pRun = &pConfig->m_vRuns.back();
				pIndex = nullptr;
			}
			else if(str_startswith(pLine, "NewRun") && pConfig != nullptr)
			{
				pConfig->m_vRuns.emplace_back();
				pRun = &pConfig->m_vRuns.back();
				pIndex = nullptr;
			}
			else if(str_startswith(pLine, "Index") && pRun != nullptr)
			{
				CAutomapRules::CIndexRule Rule;
				char aOne[128] = "";
				char aTwo[128] = "";
				char aThree[128] = "";
				// NOLINTNEXTLINE(cert-err34-c): the grammar is what it is; a
				// line that does not parse leaves the defaults, which is how
				// the editor in the client reads it too.
				sscanf(pLine, "Index %d %127s %127s %127s", &Rule.m_Id, aOne, aTwo, aThree);
				for(const char *pWord : {aOne, aTwo, aThree})
					if(pWord[0] != '\0')
						Rule.m_Flag = WithFlag(Rule.m_Flag, pWord, false);
				pRun->m_vIndexRules.push_back(std::move(Rule));
				pIndex = &pRun->m_vIndexRules.back();
			}
			else if(str_startswith(pLine, "Pos") && pIndex != nullptr)
			{
				int x = 0;
				int y = 0;
				char aValue[128] = "";
				int Value = CAutomapRules::CPosRule::NORULE;
				std::vector<CAutomapRules::CIndexInfo> vIndexList;
				// NOLINTNEXTLINE(cert-err34-c)
				sscanf(pLine, "Pos %d %d %127s", &x, &y, aValue);

				if(str_comp(aValue, "EMPTY") == 0)
				{
					Value = CAutomapRules::CPosRule::INDEX;
					vIndexList.push_back({0, 0, false});
				}
				else if(str_comp(aValue, "FULL") == 0)
				{
					Value = CAutomapRules::CPosRule::NOTINDEX;
					vIndexList.push_back({0, 0, false});
				}
				else if(str_comp(aValue, "INDEX") == 0 || str_comp(aValue, "NOTINDEX") == 0)
				{
					Value = str_comp(aValue, "INDEX") == 0 ? CAutomapRules::CPosRule::INDEX : CAutomapRules::CPosRule::NOTINDEX;
					int Word = 4;
					while(true)
					{
						CAutomapRules::CIndexInfo Info;
						char aOrient[4][128] = {"", "", "", ""};
						// NOLINTNEXTLINE(cert-err34-c)
						sscanf(str_trim_words(pLine, Word), "%d %127s %127s %127s %127s",
							&Info.m_Id, aOrient[0], aOrient[1], aOrient[2], aOrient[3]);
						Info.m_Flag = 0;
						Info.m_TestFlag = false;

						// The words after the index are its flags, and an
						// "OR" among them starts another index that shares
						// the position.
						bool More = false;
						bool Done = false;
						for(int At = 0; At < 4 && !Done; ++At)
						{
							if(str_comp(aOrient[At], "OR") == 0)
							{
								vIndexList.push_back(Info);
								Word += At + 2;
								More = true;
								Done = true;
							}
							else if(At < 3 && aOrient[At][0] != '\0' && (At == 0 || Info.m_Flag != 0))
							{
								// A tile has three flags, so the fourth word
								// is only ever an "OR" - reading it as a flag
								// would be reading something that is not
								// there.
								Info.m_Flag = WithFlag(Info.m_Flag, aOrient[At], At == 0);
								if(At == 0)
									Info.m_TestFlag = !(Info.m_Flag == 0 && str_comp(aOrient[0], "NONE") == 0);
							}
							else
							{
								vIndexList.push_back(Info);
								Done = true;
							}
						}
						if(!More)
							break;
					}
				}

				if(Value != CAutomapRules::CPosRule::NORULE)
				{
					CAutomapRules::CPosRule Rule;
					Rule.m_X = x;
					Rule.m_Y = y;
					Rule.m_Value = Value;
					Rule.m_vIndexList = vIndexList;
					pIndex->m_vRules.push_back(std::move(Rule));

					pConfig->m_StartX = std::min(pConfig->m_StartX, x);
					pConfig->m_StartY = std::min(pConfig->m_StartY, y);
					pConfig->m_EndX = std::max(pConfig->m_EndX, x);
					pConfig->m_EndY = std::max(pConfig->m_EndY, y);

					if(x == 0 && y == 0)
					{
						for(const auto &Info : vIndexList)
						{
							// A rule about the tile itself says whether air
							// or a tile can be skipped outright, which is
							// what makes a whole-layer run fast.
							if(Info.m_Id == 0 && Value == CAutomapRules::CPosRule::INDEX)
								pIndex->m_SkipFull = true;
							else if((Info.m_Id > 0 && Value == CAutomapRules::CPosRule::INDEX) ||
								(Info.m_Id == 0 && Value == CAutomapRules::CPosRule::NOTINDEX))
								pIndex->m_SkipEmpty = true;
						}
					}
				}
			}
			else if(str_startswith(pLine, "Random") && pIndex != nullptr)
			{
				float Value = 1.0f;
				char Specifier = ' ';
				// NOLINTNEXTLINE(cert-err34-c)
				sscanf(pLine, "Random %f%c", &Value, &Specifier);
				if(Specifier == '%')
					pIndex->m_RandomProbability = Value / 100.0f;
				else if(Value != 0.0f)
					pIndex->m_RandomProbability = 1.0f / Value;
			}
			else if(str_startswith(pLine, "Modulo") && pIndex != nullptr)
			{
				CAutomapRules::CModuloRule Rule;
				// NOLINTNEXTLINE(cert-err34-c)
				sscanf(pLine, "Modulo %d %d %d %d", &Rule.m_ModX, &Rule.m_ModY, &Rule.m_OffsetX, &Rule.m_OffsetY);
				if(Rule.m_ModX == 0)
					Rule.m_ModX = 1;
				if(Rule.m_ModY == 0)
					Rule.m_ModY = 1;
				pIndex->m_vModuloRules.push_back(Rule);
			}
			else if(str_startswith(pLine, "NoDefaultRule") && pIndex != nullptr)
			{
				pIndex->m_DefaultRule = false;
			}
			else if(str_startswith(pLine, "NoLayerCopy") && pRun != nullptr)
			{
				pRun->m_AutomapCopy = false;
			}
			else if(pvNotUnderstood != nullptr)
			{
				// Either a word the grammar does not have, or one it has in a
				// place where it means nothing - a `Pos` before any `Index`
				// has nothing to belong to. Both are worth saying out loud to
				// somebody writing a rules file; neither stops the rest.
				pvNotUnderstood->push_back(Number);
			}
		}

		// A rule that says nothing about the tile it is standing on is about
		// a tile that is there: "not air" is written in for it, unless it
		// asked not to have it.
		for(auto &Config : Rules.m_vConfigs)
		{
			for(auto &Run : Config.m_vRuns)
			{
				for(auto &Index : Run.m_vIndexRules)
				{
					bool Found = false;
					for(const auto &Rule : Index.m_vRules)
					{
						if(Rule.m_X != 0 || Rule.m_Y != 0 || Rule.m_Value != CAutomapRules::CPosRule::INDEX)
							continue;
						for(const auto &Info : Rule.m_vIndexList)
							if(Info.m_Id == 0)
								Found = true;
						break;
					}
					if(!Found && Index.m_DefaultRule)
					{
						CAutomapRules::CPosRule Rule;
						Rule.m_Value = CAutomapRules::CPosRule::NOTINDEX;
						Rule.m_vIndexList.push_back({0, 0, false});
						Index.m_vRules.push_back(std::move(Rule));
						Index.m_SkipEmpty = true;
						Index.m_SkipFull = false;
					}
					// Skipping both would skip everything, which is not what
					// either of them meant.
					if(Index.m_SkipEmpty && Index.m_SkipFull)
					{
						Index.m_SkipEmpty = false;
						Index.m_SkipFull = false;
					}
				}
			}
		}
		return Rules;
	}

	namespace
	{
		/**
		 * The rules themselves, over a plane of tiles that is already cut out
		 * and already has its margin. `SeedOffset` says where the plane sits
		 * in the layer, so that the rules which only fire sometimes fire in
		 * the same places whether the whole layer or one stroke was run.
		 */
		void Run(std::vector<CTile> &vTiles, const std::vector<CTile> &vGame, int Width, int Height,
			const CAutomapRules::CConfig &Config, int Seed, int Reference, int SeedOffsetX, int SeedOffsetY)
		{
			static const int s_aTileIndex[] = {TILE_SOLID, TILE_DEATH, TILE_NOHOOK, TILE_FREEZE, TILE_UNFREEZE,
				TILE_DFREEZE, TILE_DUNFREEZE, TILE_LFREEZE, TILE_LUNFREEZE};
			const bool HasGame = vGame.size() == vTiles.size();

			for(size_t h = 0; h < Config.m_vRuns.size(); ++h)
			{
				const CAutomapRules::CRun &TheRun = Config.m_vRuns[h];
				// Only the first run may be filtered by a physics tile, and
				// only where there is a game layer to filter by.
				const bool Filterable = h == 0 && Reference >= 0 && HasGame;
				const std::vector<CTile> &vFrom = Filterable ? vGame : vTiles;

				std::vector<CTile> vRead;
				const std::vector<CTile> *pRead = &vTiles;
				if(TheRun.m_AutomapCopy)
				{
					vRead.resize(vTiles.size());
					for(size_t At = 0; At < vRead.size(); ++At)
					{
						// A run that is filtered reads only the one physics
						// tile it was told about; everything else is air to
						// it.
						if(Filterable && Reference >= 1 && vFrom[At].m_Index != s_aTileIndex[Reference - 1])
							vRead[At].m_Index = 0;
						else
							vRead[At].m_Index = vFrom[At].m_Index;
						vRead[At].m_Flags = vFrom[At].m_Flags;
					}
					pRead = &vRead;
				}
				else if(Filterable)
				{
					pRead = &vGame;
				}

				for(int y = 0; y < Height; ++y)
				{
					for(int x = 0; x < Width; ++x)
					{
						const size_t At = (size_t)y * (size_t)Width + (size_t)x;
						CTile &Tile = vTiles[At];
						const CTile &Read = (*pRead)[At];

						for(size_t i = 0; i < TheRun.m_vIndexRules.size(); ++i)
						{
							const CAutomapRules::CIndexRule &Index = TheRun.m_vIndexRules[i];
							if(Read.m_Index == 0)
							{
								if(Tile.m_Index != 0 && Filterable)
								{
									Tile.m_Index = 0;
									Tile.m_Flags = (unsigned char)Index.m_Flag;
									continue;
								}
								if(Index.m_SkipEmpty)
									continue;
							}
							if(Index.m_SkipFull && Read.m_Index != 0)
								continue;

							bool Respects = true;
							for(size_t j = 0; j < Index.m_vRules.size() && Respects; ++j)
							{
								const CAutomapRules::CPosRule &Rule = Index.m_vRules[j];
								const int CheckX = x + Rule.m_X;
								const int CheckY = y + Rule.m_Y;
								int CheckIndex = -1;
								int CheckFlags = 0;
								if(CheckX >= 0 && CheckX < Width && CheckY >= 0 && CheckY < Height)
								{
									const CTile &Looked = (*pRead)[(size_t)CheckY * (size_t)Width + (size_t)CheckX];
									CheckIndex = Looked.m_Index;
									CheckFlags = Looked.m_Flags & (TILEFLAG_ROTATE | TILEFLAG_XFLIP | TILEFLAG_YFLIP);
								}

								if(Rule.m_Value == CAutomapRules::CPosRule::INDEX)
								{
									Respects = false;
									for(const auto &Info : Rule.m_vIndexList)
									{
										if(CheckIndex == Info.m_Id && (!Info.m_TestFlag || CheckFlags == Info.m_Flag))
										{
											Respects = true;
											break;
										}
									}
								}
								else if(Rule.m_Value == CAutomapRules::CPosRule::NOTINDEX)
								{
									for(const auto &Info : Rule.m_vIndexList)
									{
										if(CheckIndex == Info.m_Id && (!Info.m_TestFlag || CheckFlags == Info.m_Flag))
										{
											Respects = false;
											break;
										}
									}
								}
							}
							if(!Respects)
								continue;

							if(!Index.m_vModuloRules.empty() &&
								!std::any_of(Index.m_vModuloRules.begin(), Index.m_vModuloRules.end(),
									[&](const CAutomapRules::CModuloRule &Modulo) {
										return (x + SeedOffsetX + Modulo.m_OffsetX) % Modulo.m_ModX == 0 &&
										       (y + SeedOffsetY + Modulo.m_OffsetY) % Modulo.m_ModY == 0;
									}))
								continue;

							if(Index.m_RandomProbability < 1.0f &&
								HashLocation(Seed, h, i, x + SeedOffsetX, y + SeedOffsetY) >= HASH_MAX * Index.m_RandomProbability)
								continue;

							Tile.m_Index = (unsigned char)Index.m_Id;
							Tile.m_Flags = (unsigned char)Index.m_Flag;
						}
					}
				}
			}
		}
	} // namespace

	void Automap(CTileLayer &Layer, const CTileLayer *pGame, const CAutomapRules &Rules, size_t Config,
		int Seed, int Reference, int x, int y, int Width, int Height)
	{
		if(Config >= Rules.NumConfigs())
			return;
		const CAutomapRules::CConfig &TheConfig = Rules.m_vConfigs[Config];
		const int LayerWidth = Layer.Width();
		const int LayerHeight = Layer.Height();
		if(LayerWidth <= 0 || LayerHeight <= 0)
			return;
		if(Width < 0)
			Width = LayerWidth;
		if(Height < 0)
			Height = LayerHeight;
		if(Seed == 0)
			Seed = rand();

		// What is written back, and - three times as wide, because a rule
		// reads a tile that another rule wrote - what is worked out.
		const int CommitFromX = std::clamp(x + TheConfig.m_StartX, 0, LayerWidth);
		const int CommitFromY = std::clamp(y + TheConfig.m_StartY, 0, LayerHeight);
		const int CommitToX = std::clamp(x + Width + TheConfig.m_EndX, 0, LayerWidth);
		const int CommitToY = std::clamp(y + Height + TheConfig.m_EndY, 0, LayerHeight);
		const int FromX = std::clamp(x + 3 * TheConfig.m_StartX, 0, LayerWidth);
		const int FromY = std::clamp(y + 3 * TheConfig.m_StartY, 0, LayerHeight);
		const int ToX = std::clamp(x + Width + 3 * TheConfig.m_EndX, 0, LayerWidth);
		const int ToY = std::clamp(y + Height + 3 * TheConfig.m_EndY, 0, LayerHeight);
		if(ToX <= FromX || ToY <= FromY || CommitToX <= CommitFromX || CommitToY <= CommitFromY)
			return;

		const int PlaneWidth = ToX - FromX;
		const int PlaneHeight = ToY - FromY;
		std::vector<CTile> vTiles = Flatten(Layer.m_Tiles, FromX, FromY, PlaneWidth, PlaneHeight);
		std::vector<CTile> vGame;
		if(pGame != nullptr)
		{
			// Where the game layer does not reach, the filter reads air - the
			// same as a layer that is smaller than the one being drawn in.
			vGame.resize(vTiles.size());
			const int Across = std::min(ToX, pGame->Width());
			const int Down = std::min(ToY, pGame->Height());
			for(int At = FromY; At < Down; ++At)
				for(int Along = FromX; Along < Across; ++Along)
					vGame[(size_t)(At - FromY) * (size_t)PlaneWidth + (size_t)(Along - FromX)] = pGame->m_Tiles.Get(Along, At);
		}

		Run(vTiles, vGame, PlaneWidth, PlaneHeight, TheConfig, Seed, Reference, FromX, FromY);

		// Only the tiles that changed are written, because writing a tile
		// copies the block it is in.
		for(int At = CommitFromY; At < CommitToY; ++At)
		{
			for(int Across = CommitFromX; Across < CommitToX; ++Across)
			{
				const CTile &Made = vTiles[(size_t)(At - FromY) * (size_t)PlaneWidth + (size_t)(Across - FromX)];
				const CTile Was = Layer.m_Tiles.Get(Across, At);
				if(Was.m_Index != Made.m_Index || Was.m_Flags != Made.m_Flags)
					Layer.m_Tiles.Set(Across, At, Made);
			}
		}
	}
} // namespace map_document
