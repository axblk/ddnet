#include <base/webfs.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
	bool Parse(CWebDataIndex &Index, const std::string &Text)
	{
		return Index.Parse(Text.c_str(), Text.size());
	}

	std::vector<std::string> List(const CWebDataIndex &Index, const char *pPath)
	{
		std::vector<std::string> vPaths;
		Index.List(pPath, [&](const CWebDataIndex::CEntry &Entry) { vPaths.push_back(Entry.m_Path); });
		return vPaths;
	}

	const char *const EXAMPLE =
		"ddnet-web-data 1\n"
		"d assets\n"
		"d assets/entities\n"
		"f assets/entities/ddnet.png 24576 9f2c1a4b7e6d5038\n"
		"d maps\n"
		"f maps/Gold Mine.map 234567 0123456789abcdef\n"
		"f settings.cfg 12 00112233445566aa\n";
} // namespace

TEST(WebDataIndex, ReadsAnIndex)
{
	CWebDataIndex Index;
	ASSERT_TRUE(Parse(Index, EXAMPLE));
	EXPECT_EQ(Index.Count(), 6);

	const CWebDataIndex::CEntry *pEntry = Index.Find("assets/entities/ddnet.png");
	ASSERT_NE(pEntry, nullptr);
	EXPECT_FALSE(pEntry->m_IsDirectory);
	EXPECT_EQ(pEntry->m_Size, 24576);
	EXPECT_EQ(pEntry->m_Hash, "9f2c1a4b7e6d5038");

	EXPECT_TRUE(Index.IsFile("settings.cfg"));
	EXPECT_FALSE(Index.IsDirectory("settings.cfg"));
	EXPECT_TRUE(Index.IsDirectory("assets/entities"));
	EXPECT_FALSE(Index.IsFile("assets/entities"));
	EXPECT_EQ(Index.Find("nothing/here"), nullptr);
	// The data directory itself is in there without being written down.
	EXPECT_TRUE(Index.IsDirectory(""));
}

TEST(WebDataIndex, KeepsSpacesInAName)
{
	CWebDataIndex Index;
	ASSERT_TRUE(Parse(Index, EXAMPLE));
	const CWebDataIndex::CEntry *pEntry = Index.Find("maps/Gold Mine.map");
	ASSERT_NE(pEntry, nullptr);
	EXPECT_EQ(pEntry->m_Size, 234567);
	EXPECT_EQ(pEntry->m_Hash, "0123456789abcdef");
}

TEST(WebDataIndex, ListsOneDirectoryDeep)
{
	CWebDataIndex Index;
	ASSERT_TRUE(Parse(Index, EXAMPLE));
	EXPECT_EQ(List(Index, ""), (std::vector<std::string>{"assets", "maps", "settings.cfg"}));
	EXPECT_EQ(List(Index, "assets"), (std::vector<std::string>{"assets/entities"}));
	EXPECT_EQ(List(Index, "assets/entities"), (std::vector<std::string>{"assets/entities/ddnet.png"}));
	EXPECT_TRUE(List(Index, "assets/entities/ddnet.png").empty());
	EXPECT_TRUE(List(Index, "nothing").empty());
}

TEST(WebDataIndex, RefusesWhatIsNotAnIndex)
{
	CWebDataIndex Index;
	EXPECT_FALSE(Parse(Index, ""));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 2\n"));
	EXPECT_FALSE(Parse(Index, "something else\nd assets\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nx assets\n"));
	// Neither field behind the path may be missing or malformed.
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf a.png 24576\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf a.png xxx 9f2c1a4b7e6d5038\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf a.png 24576 9F2C1A4B7E6D5038\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf a.png 24576 9f2c1a4b\n"));
	// A path that leaves the data directory or that no file can have.
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf ../secret 1 9f2c1a4b7e6d5038\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf /etc/passwd 1 9f2c1a4b7e6d5038\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf a\\b.png 1 9f2c1a4b7e6d5038\n"));
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nd \n"));
	// The same path twice would make a lookup depend on the order.
	EXPECT_FALSE(Parse(Index, "ddnet-web-data 1\nf a.png 1 9f2c1a4b7e6d5038\nf a.png 2 9f2c1a4b7e6d5039\n"));
	// Nothing is kept of an index that could not be read.
	EXPECT_EQ(Index.Count(), 0);
}

TEST(WebDataIndex, ReadsWindowsLineEndings)
{
	CWebDataIndex Index;
	ASSERT_TRUE(Parse(Index, "ddnet-web-data 1\r\nd assets\r\nf assets/a.png 7 9f2c1a4b7e6d5038\r\n"));
	EXPECT_TRUE(Index.IsFile("assets/a.png"));
	EXPECT_EQ(Index.Find("assets/a.png")->m_Hash, "9f2c1a4b7e6d5038");
}
