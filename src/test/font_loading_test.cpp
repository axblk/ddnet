#include <base/str.h>

#include <engine/client/font_loading.h>

#include <gtest/gtest.h>

namespace
{
	const char *const FULL_INDEX = R"({
		"font files": ["DejaVuSans.ttf", "Icons.otf"],
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

	// The order of the font files determines the order of the font faces
	// and therefore which face is used for a character.
	ASSERT_EQ(Index.m_vFontFilePaths.size(), 2U);
	EXPECT_EQ(Index.m_vFontFilePaths[0], "fonts/DejaVuSans.ttf");
	EXPECT_EQ(Index.m_vFontFilePaths[1], "fonts/Icons.otf");

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
	EXPECT_TRUE(Index.m_DefaultFamilyName.empty());
	EXPECT_TRUE(Index.m_vFallbackFamilyNames.empty());
	EXPECT_TRUE(Index.m_vLanguageVariants.empty());
}

TEST(FontLoadProgress, StartsIdle)
{
	CFontLoadProgress Progress;
	EXPECT_EQ(Progress.State(), CFontLoadProgress::EState::IDLE);
	EXPECT_FALSE(Progress.Loading());
	EXPECT_FALSE(Progress.Ready());
	EXPECT_EQ(Progress.FileCount(), 0U);
}

TEST(FontLoadProgress, BecomesReadyOnlyAfterEveryFile)
{
	CFontLoadProgress Progress;
	Progress.BeginLoading(2, true);
	EXPECT_TRUE(Progress.Loading());
	EXPECT_FALSE(Progress.AllFilesFinished());

	Progress.ReportFile(true);
	EXPECT_EQ(Progress.FinishedFileCount(), 1U);
	EXPECT_FALSE(Progress.AllFilesFinished());
	EXPECT_TRUE(Progress.Loading());

	Progress.ReportFile(true);
	EXPECT_TRUE(Progress.AllFilesFinished());
	EXPECT_TRUE(Progress.Loading());

	Progress.Commit(true);
	EXPECT_EQ(Progress.State(), CFontLoadProgress::EState::READY);
	EXPECT_TRUE(Progress.Ready());
	EXPECT_FALSE(Progress.Loading());
	EXPECT_TRUE(Progress.Success());
}

TEST(FontLoadProgress, IsReadyImmediatelyWithoutFiles)
{
	// A font index that could not be read leaves the text render without fonts,
	// but it must not leave it waiting for files that were never requested.
	CFontLoadProgress Progress;
	Progress.BeginLoading(0, false);
	EXPECT_TRUE(Progress.AllFilesFinished());
	Progress.Commit(true);
	EXPECT_TRUE(Progress.Ready());
	EXPECT_FALSE(Progress.Success());
}

TEST(FontLoadProgress, ReportsFailures)
{
	CFontLoadProgress Progress;
	Progress.BeginLoading(3, true);
	Progress.ReportFile(true);
	Progress.ReportFile(false);
	Progress.ReportFile(true);
	EXPECT_EQ(Progress.FailedFileCount(), 1U);
	Progress.Commit(true);
	EXPECT_TRUE(Progress.Ready());
	EXPECT_FALSE(Progress.Success());
}

TEST(FontLoadProgress, ReportsMissingFaces)
{
	CFontLoadProgress Progress;
	Progress.BeginLoading(1, true);
	Progress.ReportFile(true);
	Progress.Commit(false);
	EXPECT_TRUE(Progress.Ready());
	EXPECT_FALSE(Progress.Success());
}

TEST(FontLoadProgress, ResetsToIdle)
{
	CFontLoadProgress Progress;
	Progress.BeginLoading(1, false);
	Progress.ReportFile(false);
	Progress.Commit(false);
	ASSERT_FALSE(Progress.Success());

	Progress.Reset();
	EXPECT_EQ(Progress.State(), CFontLoadProgress::EState::IDLE);
	EXPECT_EQ(Progress.FileCount(), 0U);
	EXPECT_EQ(Progress.FinishedFileCount(), 0U);
	EXPECT_EQ(Progress.FailedFileCount(), 0U);
	EXPECT_TRUE(Progress.Success());
}
