#include <base/str.h>

#include <engine/client/font_loading.h>

#include <gtest/gtest.h>

namespace
{
	const char *const FULL_INDEX = R"({
		"font files": ["DejaVuSans.ttf", "Icons.otf"],
		"deferred font files": [
			"Huge.ttc",
			{"file": "Named.otf", "families": ["Some Font", "Some Font Bold"]}
		],
		"default": "DejaVu Sans",
		"language variants": {
			"japanese": "Glow Sans J",
			"korean": "Source Han Sans K"
		},
		"fallbacks": ["Source Han Sans", "Some Other Font"],
		"icon": "Font Awesome 6 Free"
	})";

	bool Parse(CFontIndex &Index, const char *pJson)
	{
		return Index.Parse(pJson, str_length(pJson), "test/index.json");
	}
}

TEST(FontIndex, ParsesCompleteIndex)
{
	CFontIndex Index;
	EXPECT_TRUE(Parse(Index, FULL_INDEX));

	ASSERT_EQ(Index.m_vFontFilePaths.size(), 2U);
	EXPECT_EQ(Index.m_vFontFilePaths[0], "fonts/DejaVuSans.ttf");
	EXPECT_EQ(Index.m_vFontFilePaths[1], "fonts/Icons.otf");

	ASSERT_EQ(Index.m_vDeferredFontFiles.size(), 2U);
	EXPECT_EQ(Index.m_vDeferredFontFiles[0].m_Path, "fonts/Huge.ttc");
	EXPECT_TRUE(Index.m_vDeferredFontFiles[0].m_vFamilyNames.empty());
	EXPECT_EQ(Index.m_vDeferredFontFiles[1].m_Path, "fonts/Named.otf");
	ASSERT_EQ(Index.m_vDeferredFontFiles[1].m_vFamilyNames.size(), 2U);
	EXPECT_EQ(Index.m_vDeferredFontFiles[1].m_vFamilyNames[0], "Some Font");
	EXPECT_EQ(Index.m_vDeferredFontFiles[1].m_vFamilyNames[1], "Some Font Bold");

	EXPECT_EQ(Index.m_DefaultFamilyName, "DejaVu Sans");
	EXPECT_EQ(Index.m_IconFamilyName, "Font Awesome 6 Free");

	ASSERT_EQ(Index.m_vFallbackFamilyNames.size(), 2U);
	EXPECT_EQ(Index.m_vFallbackFamilyNames[0], "Source Han Sans");
	EXPECT_EQ(Index.m_vFallbackFamilyNames[1], "Some Other Font");

	ASSERT_EQ(Index.m_vLanguageVariants.size(), 2U);
	EXPECT_EQ(Index.m_vLanguageVariants[0].m_LanguageFile, "languages/japanese.txt");
	EXPECT_EQ(Index.m_vLanguageVariants[0].m_FamilyName, "Glow Sans J");
	EXPECT_EQ(Index.m_vLanguageVariants[1].m_LanguageFile, "languages/korean.txt");
	EXPECT_EQ(Index.m_vLanguageVariants[1].m_FamilyName, "Source Han Sans K");
}

TEST(FontIndex, RejectsBrokenJson)
{
	CFontIndex Index;
	EXPECT_FALSE(Parse(Index, "{"));
	EXPECT_TRUE(Index.m_vFontFilePaths.empty());

	EXPECT_FALSE(Parse(Index, "[]"));
	EXPECT_TRUE(Index.m_vFontFilePaths.empty());
}

TEST(FontIndex, ReportsMissingEntriesButKeepsTheRest)
{
	CFontIndex Index;
	EXPECT_FALSE(Parse(Index, R"({"font files": ["DejaVuSans.ttf"]})"));
	ASSERT_EQ(Index.m_vFontFilePaths.size(), 1U);
	EXPECT_EQ(Index.m_vFontFilePaths[0], "fonts/DejaVuSans.ttf");
	EXPECT_TRUE(Index.m_DefaultFamilyName.empty());
	EXPECT_TRUE(Index.m_IconFamilyName.empty());
	EXPECT_TRUE(Index.m_vFallbackFamilyNames.empty());
	EXPECT_TRUE(Index.m_vLanguageVariants.empty());
}

TEST(FontIndex, SkipsMalformedEntries)
{
	CFontIndex Index;
	EXPECT_FALSE(Parse(Index, R"({
		"font files": ["DejaVuSans.ttf", 42, "Icons.otf"],
		"default": "DejaVu Sans",
		"language variants": {"japanese": 42, "korean": "Source Han Sans K"},
		"fallbacks": ["Source Han Sans", 42],
		"icon": "Font Awesome 6 Free"
	})"));
	ASSERT_EQ(Index.m_vFontFilePaths.size(), 2U);
	EXPECT_EQ(Index.m_vFontFilePaths[0], "fonts/DejaVuSans.ttf");
	EXPECT_EQ(Index.m_vFontFilePaths[1], "fonts/Icons.otf");
	ASSERT_EQ(Index.m_vFallbackFamilyNames.size(), 1U);
	EXPECT_EQ(Index.m_vFallbackFamilyNames[0], "Source Han Sans");
	ASSERT_EQ(Index.m_vLanguageVariants.size(), 1U);
	EXPECT_EQ(Index.m_vLanguageVariants[0].m_LanguageFile, "languages/korean.txt");
}

TEST(FontIndex, ResetsBetweenParses)
{
	CFontIndex Index;
	ASSERT_TRUE(Parse(Index, FULL_INDEX));
	EXPECT_FALSE(Parse(Index, R"({"font files": []})"));
	EXPECT_TRUE(Index.m_vFontFilePaths.empty());
	EXPECT_TRUE(Index.m_vDeferredFontFiles.empty());
	EXPECT_TRUE(Index.m_DefaultFamilyName.empty());
	EXPECT_TRUE(Index.m_vFallbackFamilyNames.empty());
	EXPECT_TRUE(Index.m_vLanguageVariants.empty());
}

TEST(FontIndex, TakesAnIndexWithoutDeferredFonts)
{
	CFontIndex Index;
	EXPECT_TRUE(Parse(Index, R"({
		"font files": ["DejaVuSans.ttf"],
		"default": "DejaVu Sans",
		"language variants": {},
		"fallbacks": [],
		"icon": "Font Awesome 6 Free"
	})"));
	EXPECT_EQ(Index.m_vFontFilePaths.size(), 1U);
	EXPECT_TRUE(Index.m_vDeferredFontFiles.empty());
}

TEST(FontIndex, SkipsDeferredFontsWithoutAFileName)
{
	CFontIndex Index;
	EXPECT_FALSE(Parse(Index, R"({
		"font files": ["DejaVuSans.ttf"],
		"deferred font files": [
			{"families": ["Some Font"]},
			{"file": "Good.otf", "families": ["Some Font"]}
		],
		"default": "DejaVu Sans",
		"language variants": {},
		"fallbacks": [],
		"icon": "Font Awesome 6 Free"
	})"));
	ASSERT_EQ(Index.m_vDeferredFontFiles.size(), 1U);
	EXPECT_EQ(Index.m_vDeferredFontFiles[0].m_Path, "fonts/Good.otf");
}

TEST(FontIndex, SkipsDeferredFontFamiliesThatAreNotStrings)
{
	CFontIndex Index;
	EXPECT_FALSE(Parse(Index, R"({
		"font files": ["DejaVuSans.ttf"],
		"deferred font files": [
			{"file": "Huge.ttc", "families": ["Good Font", 7]},
			{"file": "Other.otf", "families": "Not A List"}
		],
		"default": "DejaVu Sans",
		"language variants": {},
		"fallbacks": [],
		"icon": "Font Awesome 6 Free"
	})"));
	ASSERT_EQ(Index.m_vDeferredFontFiles.size(), 2U);
	ASSERT_EQ(Index.m_vDeferredFontFiles[0].m_vFamilyNames.size(), 1U);
	EXPECT_EQ(Index.m_vDeferredFontFiles[0].m_vFamilyNames[0], "Good Font");
	EXPECT_TRUE(Index.m_vDeferredFontFiles[1].m_vFamilyNames.empty());
}

TEST(FontIndex, RejectsDeferredFontsThatAreNotAList)
{
	CFontIndex Index;
	EXPECT_FALSE(Parse(Index, R"({
		"font files": ["DejaVuSans.ttf"],
		"deferred font files": "Huge.ttc",
		"default": "DejaVu Sans",
		"language variants": {},
		"fallbacks": [],
		"icon": "Font Awesome 6 Free"
	})"));
	EXPECT_TRUE(Index.m_vDeferredFontFiles.empty());
}
