#include <base/net.h>
#include <base/str.h>

#include <engine/shared/connect_target.h>

#include <gtest/gtest.h>

static const char IDENTITY[] = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";
static const char CERTIFICATE[] = "0e2bdc9e1d3a3b4a41d4b1b8a5a3cb3e1d7e1a4b6a1c90b95d2e6c8f0a1b2c3d";

TEST(ConnectTarget, LegacyAndModernAddresses)
{
	char aAddresses[256];
	str_format(aAddresses, sizeof(aAddresses), "127.0.0.1:8303,ddnet+quic://127.0.0.1:8304#identity-sha256=%s", IDENTITY);
	CConnectTarget Target;
	ASSERT_TRUE(Target.Parse(aAddresses, NETTYPE_ALL));
	ASSERT_EQ(Target.m_NumAddrs, 2);
	EXPECT_FALSE(Target.m_Sixup);
	EXPECT_EQ(Target.m_aAddrs[0].type & NETTYPE_QUIC, 0);
	EXPECT_NE(Target.m_aAddrs[1].type & NETTYPE_QUIC, 0);
	EXPECT_EQ(Target.m_aAddrs[1].port, 8304);
	char aFragment[128];
	str_format(aFragment, sizeof(aFragment), "identity-sha256=%s", IDENTITY);
	EXPECT_STREQ(Target.m_aFragment, aFragment);
	// An IP address has no name to connect by.
	EXPECT_STREQ(Target.m_aHost, "");
}

TEST(ConnectTarget, CommaInFragment)
{
	char aAddresses[512];
	str_format(aAddresses, sizeof(aAddresses), "ddnet+wt://127.0.0.1:8303#identity-sha256=%s,cert-sha256=%s,127.0.0.1:8304", IDENTITY, CERTIFICATE);
	CConnectTarget Target;
	ASSERT_TRUE(Target.Parse(aAddresses, NETTYPE_ALL));
	ASSERT_EQ(Target.m_NumAddrs, 2);
	EXPECT_NE(Target.m_aAddrs[0].type & NETTYPE_WEBTRANSPORT, 0);
	EXPECT_EQ(Target.m_aAddrs[1].port, 8304);
	char aFragment[256];
	str_format(aFragment, sizeof(aFragment), "identity-sha256=%s,cert-sha256=%s", IDENTITY, CERTIFICATE);
	EXPECT_STREQ(Target.m_aFragment, aFragment);
}

TEST(ConnectTarget, Sixup)
{
	CConnectTarget Target;
	ASSERT_TRUE(Target.Parse("tw-0.7+udp://127.0.0.1:8303", NETTYPE_ALL));
	ASSERT_EQ(Target.m_NumAddrs, 1);
	EXPECT_TRUE(Target.m_Sixup);
	EXPECT_NE(Target.m_aAddrs[0].type & NETTYPE_TW7, 0);

	// One 0.6 address makes the whole target 0.6.
	ASSERT_TRUE(Target.Parse("tw-0.7+udp://127.0.0.1:8303,127.0.0.1:8304", NETTYPE_ALL));
	EXPECT_FALSE(Target.m_Sixup);
}

TEST(ConnectTarget, DefaultPortAndNothing)
{
	CConnectTarget Target;
	ASSERT_TRUE(Target.Parse("127.0.0.1", NETTYPE_ALL));
	ASSERT_EQ(Target.m_NumAddrs, 1);
	EXPECT_EQ(Target.m_aAddrs[0].port, 8303);

	EXPECT_FALSE(Target.Parse("", NETTYPE_ALL));
	EXPECT_EQ(Target.m_NumAddrs, 0);
}
