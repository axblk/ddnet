#include <base/str.h>

#include <engine/console.h>
#include <engine/shared/config.h>

#include <gtest/gtest.h>

#include <memory>

namespace
{
	void CountCalls(IConsole::IResult *pResult, void *pUser)
	{
		(*static_cast<int *>(pUser))++;
	}

	void PassThrough(IConsole::IResult *pResult, void *pUser, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
	{
		pfnCallback(pResult, pCallbackUserData);
	}

	void CountAndPassThrough(IConsole::IResult *pResult, void *pUser, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
	{
		(*static_cast<int *>(pUser))++;
		pfnCallback(pResult, pCallbackUserData);
	}
}

TEST(Console, OwnedCommandsHaveBoundedLifetime)
{
	auto pConsole = CreateConsole(CFGFLAG_SERVER);
	int Owner = 0;
	int OtherOwner = 0;
	int Calls = 0;
	int StoredCalls = 0;
	int CommonCalls = 0;

	pConsole->Register("common", "", CFGFLAG_SERVER | CFGFLAG_STORE, CountCalls, &CommonCalls, "Common command");
	pConsole->Register("z_last", "", CFGFLAG_SERVER, CountCalls, &CommonCalls, "Last command");
	ASSERT_TRUE(pConsole->RegisterOwned("owned", "", CFGFLAG_SERVER, CountCalls, &Calls, "Owned command", &Owner));
	ASSERT_TRUE(pConsole->RegisterOwned("owned_stored", "", CFGFLAG_SERVER | CFGFLAG_STORE, CountCalls, &StoredCalls, "Queued owned command", &Owner));
	ASSERT_TRUE(pConsole->RegisterOwned("owned_other", "", CFGFLAG_SERVER, CountCalls, &Calls, "Other owned command", &OtherOwner));
	EXPECT_FALSE(pConsole->RegisterOwned("owned", "", CFGFLAG_SERVER, CountCalls, &Calls, "Conflicting command", &OtherOwner));

	pConsole->Chain("owned", PassThrough, nullptr);
	pConsole->ExecuteLine("owned", IConsole::CLIENT_ID_UNSPECIFIED, true);
	pConsole->ExecuteLine("owned_stored", IConsole::CLIENT_ID_UNSPECIFIED, true);
	pConsole->ExecuteLine("common", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(StoredCalls, 0);

	const IConsole::ICommandInfo *pOwnedCommand = pConsole->FirstCommandInfoAtOrAfter("owned", IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_SERVER);
	ASSERT_NE(pOwnedCommand, nullptr);
	char aOwnedName[IConsole::TEMPCMD_NAME_LENGTH];
	str_copy(aOwnedName, pOwnedCommand->Name());
	pConsole->DeregisterOwner(&Owner);
	EXPECT_EQ(pConsole->GetCommandInfo("owned", CFGFLAG_SERVER, false), nullptr);
	EXPECT_EQ(pConsole->GetCommandInfo("owned_stored", CFGFLAG_SERVER, false), nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("owned_other", CFGFLAG_SERVER, false), nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("common", CFGFLAG_SERVER, false), nullptr);
	const IConsole::ICommandInfo *pNextAfterRemoved = pConsole->FirstCommandInfoAtOrAfter(aOwnedName, IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_SERVER);
	ASSERT_NE(pNextAfterRemoved, nullptr);
	EXPECT_STREQ(pNextAfterRemoved->Name(), "owned_other");
	EXPECT_STREQ(pConsole->FirstCommandInfoAtOrAfter("p", IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_SERVER)->Name(), "z_last");

	pConsole->StoreCommands(false);
	EXPECT_EQ(CommonCalls, 1);
	pConsole->ExecuteLine("common", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(CommonCalls, 2);
}

TEST(Console, OwnedChainsHaveBoundedLifetime)
{
	auto pConsole = CreateConsole(CFGFLAG_SERVER);
	int Owner = 0;
	int OtherOwner = 0;
	int Calls = 0;
	int ChainCalls = 0;
	int OtherChainCalls = 0;

	pConsole->Register("common", "", CFGFLAG_SERVER, CountCalls, &Calls, "Common command");
	ASSERT_TRUE(pConsole->ChainOwned("common", CountAndPassThrough, &ChainCalls, &Owner));
	ASSERT_TRUE(pConsole->ChainOwned("common", CountAndPassThrough, &OtherChainCalls, &OtherOwner));
	EXPECT_FALSE(pConsole->ChainOwned("common", CountAndPassThrough, &ChainCalls, nullptr));

	pConsole->ExecuteLine("common", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(ChainCalls, 1);
	EXPECT_EQ(OtherChainCalls, 1);

	pConsole->DeregisterOwner(&Owner);
	pConsole->ExecuteLine("common", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(Calls, 2);
	EXPECT_EQ(ChainCalls, 1);
	EXPECT_EQ(OtherChainCalls, 2);

	pConsole->DeregisterOwner(&OtherOwner);
	pConsole->ExecuteLine("common", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(Calls, 3);
	EXPECT_EQ(ChainCalls, 1);
	EXPECT_EQ(OtherChainCalls, 2);
}

namespace
{
	void CaptureArgument(IConsole::IResult *pResult, void *pUser)
	{
		str_copy(static_cast<char *>(pUser), pResult->GetString(0), 256);
	}
}

class ConsoleComments : public ::testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	ConsoleComments() :
		m_pConsole(CreateConsole(CFGFLAG_SERVER))
	{
		m_aCaptured[0] = '\0';
		m_pConsole->Register("capture", "r[text]", CFGFLAG_SERVER, CaptureArgument, m_aCaptured, "Remember the argument");
	}

	std::unique_ptr<IConsole> m_pConsole;
	char m_aCaptured[256];
};

TEST_F(ConsoleComments, CommentStartsAWord)
{
	// A connect link carries its certificate hashes behind a '#', so reading
	// every '#' as a comment would cut the link short and drop the hashes.
	m_pConsole->ExecuteLine("capture ddnet+wt://localhost:8303#cert-sha256=abc", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_STREQ(m_aCaptured, "ddnet+wt://localhost:8303#cert-sha256=abc");
	EXPECT_TRUE(m_pConsole->LineIsValid("capture ddnet+wt://localhost:8303#cert-sha256=abc"));
}

TEST_F(ConsoleComments, CommentAfterWhitespaceStillEndsTheLine)
{
	m_pConsole->ExecuteLine("capture value # a comment", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(str_find(m_aCaptured, "#"), nullptr);
	EXPECT_EQ(str_find(m_aCaptured, "comment"), nullptr);
}

TEST_F(ConsoleComments, CommentAtLineStartExecutesNothing)
{
	m_pConsole->ExecuteLine("# capture value", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_STREQ(m_aCaptured, "");
	EXPECT_FALSE(m_pConsole->LineIsValid("# capture value"));
}

TEST_F(ConsoleComments, CommentInsideStringIsKept)
{
	m_pConsole->ExecuteLine("capture \"quoted # text\"", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_NE(str_find(m_aCaptured, "#"), nullptr);
}
