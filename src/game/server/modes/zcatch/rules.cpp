// The numbers follow the zCatch of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "rules.h"

#include <base/str.h>

#include <algorithm>
#include <array>

int ZCatch::KillsToWin(int MinPlayers)
{
	return std::clamp(MinPlayers - 1, 1, KILLS_TO_WIN);
}

int ZCatch::WinPoints(int Kills)
{
	// 4 to 6 kills give 1 point, 7 and 8 give 2, and from 9 on it grows faster
	static constexpr std::array<int, 17> s_aPoints = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 5, 7, 9, 11, 12, 14, 16};
	return s_aPoints[std::clamp(Kills, 0, (int)s_aPoints.size() - 1)];
}

ZCatch::EColors ZCatch::ParseColors(const char *pSetting)
{
	return str_comp_nocase(pSetting, "savander") == 0 ? EColors::SAVANDER : EColors::TEETIME;
}

int ZCatch::BodyColor(EColors Colors, int Kills)
{
	switch(Colors)
	{
	case EColors::SAVANDER:
		// yellow without kills and from 16 on, a greener yellow with each kill before
		return Kills <= 0 || Kills >= 16 ? 0xFFBB00 : 0x00FF00 + (Kills - 1) * 0x110000;
	case EColors::TEETIME:
		break;
	}
	return std::max(0, 160 - Kills * 10) * 0x010000 + 0xFF00;
}
