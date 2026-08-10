#include <base/str.h>

#include <engine/console.h>
#include <engine/shared/config.h>

#include <gtest/gtest.h>

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
