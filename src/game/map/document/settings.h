#ifndef GAME_MAP_DOCUMENT_SETTINGS_H
#define GAME_MAP_DOCUMENT_SETTINGS_H

#include <string>
#include <vector>

namespace map_document
{
	/**
	 * The lines a server runs when it loads a map, and what is wrong with
	 * them.
	 *
	 * A map's settings are a list of strings and nothing checks them until a
	 * server refuses to start. That is the wrong moment to find out that a
	 * variable is spelled wrong, so the editor says so while it is being
	 * typed - which needs a list of what a server would accept, and that list
	 * is the same one the client's config carries: every variable with
	 * `CFGFLAG_GAME`, plus the six commands a map may use.
	 */

	/** One argument of a setting: what it is called and what it is. */
	class CSettingArg
	{
	public:
		std::string m_Name;
		/**
		 * `i` a whole number, `f` a number, `s` a word, `r` the rest of the
		 * line. The letters are the console's own.
		 */
		char m_Type;
		/** Whether the line is still right without it. */
		bool m_Optional;
	};

	/** One thing a map may say. */
	class CMapSetting
	{
	public:
		std::string m_Name;
		std::string m_Help;
		/**
		 * A variable takes one number and has a range; a command takes
		 * whatever its arguments say.
		 */
		bool m_IsVariable;
		int m_Default;
		int m_Min;
		int m_Max;
		std::vector<CSettingArg> m_Args;

		/**
		 * Whether a map may say this more than once.
		 *
		 * `sv_deepfly 0` twice is a mistake; `tune_zone 1 ...` and
		 * `tune_zone 2 ...` are two different things that happen to share a
		 * name. What tells them apart is the first argument, so a command
		 * that may repeat says how many of its arguments have to differ.
		 */
		size_t m_KeyArgs;
	};

	/** Everything a map may say, in the order a list should show it. */
	const std::vector<CMapSetting> &KnownSettings();

	/** The one with this name, or `nullptr`. Case does not matter. */
	const CMapSetting *FindSetting(const char *pName);

	/**
	 * Splits a settings line the way a console would: quotes hold a word
	 * together, a backslash escapes, and an unquoted `#` starts a comment.
	 *
	 * @param pLine The line.
	 * @param pComment Where the comment starts, or -1; may be `nullptr`.
	 *
	 * @return The words, the first of which is the command.
	 */
	std::vector<std::string> SplitSetting(const char *pLine, int *pComment = nullptr);

	/**
	 * What is wrong with one line, in a sentence a mapper can act on.
	 *
	 * @param pLine The line.
	 *
	 * @return The complaint, or an empty string where there is none.
	 */
	std::string CheckSetting(const char *pLine);

	/**
	 * Which earlier line this one says the same thing as, or -1.
	 *
	 * "The same thing" is the name and as many arguments as tell two of them
	 * apart - so a second `sv_deepfly` collides with the first, and
	 * `tune_zone 2` does not collide with `tune_zone 1`.
	 *
	 * @param vLines The lines already there.
	 * @param pLine The line to place.
	 *
	 * @return The place of the line it collides with, or -1.
	 */
	int CollidingSetting(const std::vector<std::string> &vLines, const char *pLine);

	/** The names that begin with this, for a list of what could be meant. */
	std::vector<std::string> CompleteSetting(const char *pPrefix);
} // namespace map_document

#endif
