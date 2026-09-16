#ifndef GAME_MAP_DOCUMENT_AUTOMAP_H
#define GAME_MAP_DOCUMENT_AUTOMAP_H

#include <game/map/document/layer.h>

#include <cstddef>
#include <string>
#include <vector>

namespace map_document
{
	/**
	 * The rules of one `.rules` file, parsed.
	 *
	 * A rules file is a list of named configurations, each a list of runs,
	 * each a list of "put index `i` here when the tiles around here look like
	 * this". The grammar is the one `doc/automap.md` describes and the one
	 * the editor in the client reads; this holds the same thing, parsed the
	 * same way, so that a map automapped in the browser comes out as it would
	 * have come out natively.
	 *
	 * The file itself is not read here. Natively it comes off the disk and in
	 * the browser the page fetches it, and either way what arrives is text.
	 */
	class CAutomapRules
	{
	public:
		/** How many configurations the file holds. */
		size_t NumConfigs() const { return m_vConfigs.size(); }
		/** What one of them is called, or an empty word for one that is not there. */
		const char *ConfigName(size_t Index) const;

		class CIndexInfo
		{
		public:
			int m_Id = 0;
			int m_Flag = 0;
			bool m_TestFlag = false;
		};

		class CPosRule
		{
		public:
			enum
			{
				NORULE = 0,
				INDEX,
				NOTINDEX
			};

			int m_X = 0;
			int m_Y = 0;
			int m_Value = NORULE;
			std::vector<CIndexInfo> m_vIndexList;
		};

		class CModuloRule
		{
		public:
			int m_ModX = 1;
			int m_ModY = 1;
			int m_OffsetX = 0;
			int m_OffsetY = 0;
		};

		class CIndexRule
		{
		public:
			int m_Id = 0;
			std::vector<CPosRule> m_vRules;
			int m_Flag = 0;
			float m_RandomProbability = 1.0f;
			std::vector<CModuloRule> m_vModuloRules;
			bool m_DefaultRule = true;
			bool m_SkipEmpty = false;
			bool m_SkipFull = false;
		};

		class CRun
		{
		public:
			std::vector<CIndexRule> m_vIndexRules;
			bool m_AutomapCopy = true;
		};

		class CConfig
		{
		public:
			std::vector<CRun> m_vRuns;
			std::string m_Name;
			/** How far outside a rectangle the rules of this config reach. */
			int m_StartX = 0;
			int m_StartY = 0;
			int m_EndX = 0;
			int m_EndY = 0;
		};

		std::vector<CConfig> m_vConfigs;
	};

	/**
	 * Reads a `.rules` file that is already text.
	 *
	 * Anything it does not understand it passes over, the way the editor in
	 * the client does: a rules file that has a line from a newer editor in it
	 * still automaps with the rules that are left.
	 *
	 * @param pText The whole file, newline-separated.
	 * @param pvNotUnderstood Where the numbers of the lines it passed over
	 * are put, counting from one, or `nullptr` to not be told. A line is in
	 * there either because the grammar has no such word or because it has it
	 * in a place where it means nothing - a `Pos` before any `Index` belongs
	 * to nothing. Blank lines and comments are not passed over, they are
	 * nothing, and they are not in there.
	 *
	 * @return What it understood, which is empty for a file with no
	 * configuration in it.
	 */
	CAutomapRules ParseAutomapRules(const char *pText, std::vector<int> *pvNotUnderstood = nullptr);

	/**
	 * Runs one configuration of a rules file over a rectangle of a layer.
	 *
	 * The tiles that come out depend on the tiles around them, so a rectangle
	 * is worked out with a margin - as wide as the rules reach - and only the
	 * rectangle itself is written back. Which is what makes automapping a
	 * brush stroke as it is drawn possible at all.
	 *
	 * @param Layer The layer to write into.
	 * @param pGame The game layer of the same map, for a run that is filtered
	 * by a physics tile, or `nullptr`.
	 * @param Rules The parsed rules file.
	 * @param Config Which configuration of it.
	 * @param Seed The seed for the rules that only fire sometimes; 0 asks for
	 * one to be made up, which means the answer is not the same twice.
	 * @param Reference Which physics tile the first run is filtered by: -1
	 * for no filter at all, 0 for the game layer as it stands, and 1 to 9 for
	 * hookable, death, unhookable, freeze, unfreeze, deep freeze, deep
	 * unfreeze, live freeze and live unfreeze - the order the editor in the
	 * client offers them in.
	 * @param x Where the rectangle starts.
	 * @param y Where the rectangle starts.
	 * @param Width How wide it is, or -1 for the whole layer.
	 * @param Height How tall it is, or -1 for the whole layer.
	 */
	void Automap(CTileLayer &Layer, const CTileLayer *pGame, const CAutomapRules &Rules, size_t Config,
		int Seed, int Reference, int x, int y, int Width, int Height);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_AUTOMAP_H
