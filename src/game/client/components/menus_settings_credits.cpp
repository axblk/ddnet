#include "menus.h"

#include <engine/graphics.h>
#include <engine/textrender.h>

#include <game/client/ui.h>
#include <game/client/ui_scrollregion.h>

void CMenus::RenderSettingsCredits(CUIRect MainView)
{
	static constexpr const char *const CREDITS =
		"\n"
		"Help and code by eeeee, HMH, east, CookieMichal, Learath2, "
		"Savander, laxa, Tobii, BeaR, Wohoo, nuborn, timakro, Shiki, "
		"trml, Soreu, hi_leute_gll, Lady Saavik, Chairn, heinrich5991, "
		"swick, oy, necropotame, Ryozuki, Redix, d3fault, marcelherd, "
		"BannZay, ACTom, SiuFuWong, PathosEthosLogos, TsFreddie, "
		"Jupeyy, noby, ChillerDragon, ZombieToad, weez15, z6zzz, "
		"Piepow, QingGo, RafaelFF, sctt, jao, daverck, fokkonaut, "
		"Bojidar, FallenKN, ardadem, archimede67, sirius1242, Aerll, "
		"trafilaw, Zwelf, Patiga, Konsti, ElXreno, MikiGamer, "
		"Fireball, Banana090, axblk, yangfl, Kaffeine, Zodiac, "
		"c0d3d3v, GiuCcc, Ravie, Robyt3, simpygirl, Tater, Cellegen, "
		"srdante, Nouaa, Voxel, luk51, Vy0x2, Avolicious, louis, "
		"Marmare314, hus3h, ArijanJ, tarunsamanta2k20, Possseidon, "
		"+KZ, Teero, furo, dobrykafe, Moiman, JSaurusRex, "
		"Steinchen, ewancg, gerdoe-jr, melon, KebsCS, bencie, "
		"DynamoFox, MilkeeyCat, iMilchshake, SchrodingerZhu, "
		"catseyenebulous, Rei-Tw, Matodor, Emilcha, art0007i, SollyBunny, "
		"0xfaulty, AssassinTee, Pioooooo, ASKLL-STAR, K1nop1c0, "
		"Bamcane, qxdFox, ZerolAcqua, swarfeya, Scrumplex, 12944qwerty, "
		"Pointer31, ProfSapphire, 0xpixty, GlimmeR, horoni & others\n"
		"\n"
		"Based on DDRace by the DDRace developers,";
	const float FontSize = 16.0f;

	static CScrollRegion s_ScrollRegion;
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 3.0f * FontSize;
	s_ScrollRegion.Begin(&MainView, &ScrollParams);

	size_t LinkTextIndex = 0;
	const auto &&RenderCreditsLink = [&](const void *pLinkId, const char *pPrefix, const char *pLink, const char *pSuffix, const char *pUrl) {
		CCachedText &PrefixText = m_aSettingsCreditsLinkTexts[LinkTextIndex++];
		CCachedText &LinkText = m_aSettingsCreditsLinkTexts[LinkTextIndex++];
		CCachedText &SuffixText = m_aSettingsCreditsLinkTexts[LinkTextIndex++];
		PrefixText.Update(TextRender(), pPrefix, FontSize);
		LinkText.Update(TextRender(), pLink, FontSize);
		SuffixText.Update(TextRender(), pSuffix, FontSize);
		const float LineHeight = LinkText.Height();
		const bool PrefixOwnLine = PrefixText.Width() + LinkText.Width() + SuffixText.Width() > MainView.w;

		CUIRect Line, Prefix, LinkRect;
		MainView.HSplitTop(LineHeight, &Line, &MainView);
		s_ScrollRegion.AddRect(Line);
		if(PrefixOwnLine)
		{
			PrefixText.Render(TextRender(), vec2(Line.x, Line.y), TextRender()->DefaultTextColor());
			MainView.HSplitTop(LineHeight, &Line, &MainView);
			s_ScrollRegion.AddRect(Line);
		}
		else
		{
			Line.VSplitLeft(PrefixText.Width(), &Prefix, &Line);
			PrefixText.Render(TextRender(), vec2(Prefix.x, Prefix.y), TextRender()->DefaultTextColor());
		}

		Line.VSplitLeft(LinkText.Width(), &LinkRect, &Line);
		const ColorRGBA LinkColor = Ui()->HotItem() == pLinkId ? ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f) : ColorRGBA(0.4f, 0.7f, 1.0f, 1.0f);
		LinkText.Render(TextRender(), vec2(LinkRect.x, LinkRect.y), LinkColor);

		CUIRect Underline;
		LinkRect.HSplitBottom(1.0f, nullptr, &Underline);
		Underline.Draw(LinkColor, IGraphics::CORNER_NONE, 0.0f);

		if(Ui()->DoButtonLogic(pLinkId, 0, &LinkRect, BUTTONFLAG_LEFT))
		{
			Client()->ViewLink(pUrl);
		}

		SuffixText.Render(TextRender(), vec2(Line.x, Line.y), TextRender()->DefaultTextColor());
	};

	static char s_StaffLinkId;
	RenderCreditsLink(&s_StaffLinkId, "DDNet is run by the ", "DDNet staff", ".", "https://ddnet.org/staff");
	static char s_MapsLinkId;
	RenderCreditsLink(&s_MapsLinkId, "", "Great maps", " and many ideas from the community.", "https://ddnet.org/releases/");

	m_SettingsCreditsText.Update(TextRender(), CREDITS, FontSize, MainView.w);
	CUIRect CreditsLabel;
	MainView.HSplitTop(m_SettingsCreditsText.Height(), &CreditsLabel, &MainView);
	s_ScrollRegion.AddRect(CreditsLabel);
	m_SettingsCreditsText.Render(TextRender(), vec2(CreditsLabel.x, CreditsLabel.y), TextRender()->DefaultTextColor());

	static char s_TeeworldsLinkId;
	RenderCreditsLink(&s_TeeworldsLinkId, "which is a mod of ", "Teeworlds", " by the Teeworlds developers.", "https://teeworlds.com/");

	s_ScrollRegion.End();
}
