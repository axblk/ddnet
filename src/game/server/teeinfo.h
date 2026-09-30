#ifndef GAME_SERVER_TEEINFO_H
#define GAME_SERVER_TEEINFO_H

#include <engine/shared/protocol.h>

#include <generated/protocol7.h>

#include <optional>

class CTeeInfo
{
public:
	char m_aSkinName[MAX_SKIN_LENGTH] = "";
	bool m_UseCustomColor = false;
	int m_ColorBody = 0;
	int m_ColorFeet = 0;

	// 0.7
	char m_aaSkinPartNames[protocol7::NUM_SKINPARTS][protocol7::MAX_SKIN_LENGTH] = {"", "", "", "", "", ""};
	bool m_aUseCustomColors[protocol7::NUM_SKINPARTS] = {false, false, false, false, false, false};
	int m_aSkinPartColors[protocol7::NUM_SKINPARTS] = {0, 0, 0, 0, 0, 0};

	CTeeInfo() = default;
	CTeeInfo(const char *pSkinName, int UseCustomColor, int ColorBody, int ColorFeet);
	CTeeInfo(const char *const apSkinPartNames[protocol7::NUM_SKINPARTS], const int aUseCustomColors[protocol7::NUM_SKINPARTS], const int aSkinPartColors[protocol7::NUM_SKINPARTS]);

	void FromSixup();
	void ToSixup();
};

/**
 * What a mode shows of a player instead of the player's own choice, like the
 * colour of the group the player is in.
 *
 * The player keeps their own skin and gets it back unchanged once the mode
 * lets go, whatever they change in the meantime.
 */
class CTeeInfoOverride
{
	int m_ColorBody = 0;
	std::optional<int> m_ColorFeet;

public:
	/**
	 * Custom colours over the skin the player chose, packed HSL as 0.6 sends them.
	 *
	 * @param ColorBody The colour of the body, also of the decoration and the hands of a 0.7 skin.
	 * @param ColorFeet The colour of the feet, the player's own if there is none.
	 */
	static CTeeInfoOverride Colors(int ColorBody, std::optional<int> ColorFeet = std::nullopt);

	// the tee infos everybody sees
	CTeeInfo Apply(const CTeeInfo &Own) const;
	bool operator==(const CTeeInfoOverride &Other) const = default;
};

#endif // GAME_SERVER_TEEINFO_H
