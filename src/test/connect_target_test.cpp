#include <base/str.h>

#include <engine/client/connect_target.h>
#include <engine/shared/quic_transport.h>

#include <gtest/gtest.h>

static constexpr const char *IDENTITY = "0101010101010101010101010101010101010101010101010101010101010101";

TEST(ConnectTarget, Addresses)
{
	CConnectTarget Target;
	EXPECT_TRUE(Target.Parse("127.0.0.1:8303,tw-0.7+udp://[::1]:8304", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	ASSERT_EQ(Target.m_NumAddrs, 2);
	EXPECT_FALSE(Target.m_OnlySixup);
	EXPECT_FALSE(Target.m_Link);
	EXPECT_STREQ(Target.m_aaHosts[0], "127.0.0.1");
	EXPECT_STREQ(Target.m_aaHosts[1], "::1");
	EXPECT_EQ(Target.m_aAddrs[1].port, 8304);

	EXPECT_TRUE(Target.Parse("tw-0.7+udp://127.0.0.1", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	ASSERT_EQ(Target.m_NumAddrs, 1);
	EXPECT_TRUE(Target.m_OnlySixup);
	EXPECT_EQ(Target.m_aAddrs[0].port, 8303);
}

TEST(ConnectTarget, Links)
{
	char aLink[256];
	str_format(aLink, sizeof(aLink), "ddnet+quic://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
	CConnectTarget Target;
	EXPECT_TRUE(Target.Parse(aLink, NETTYPE_ALL, EConnectAddressFamily::IPV6));
	EXPECT_TRUE(Target.m_Link);
	EXPECT_FALSE(Target.m_LinkWebTransport);
	EXPECT_EQ(Target.m_LinkPin.m_Trust, EModernTransportTrust::IDENTITY);
	EXPECT_EQ(Target.m_NumAddrs, 1);

	// A link names the one server to connect to.
	str_append(aLink, ",127.0.0.1:8304");
	EXPECT_FALSE(Target.Parse(aLink, NETTYPE_ALL, EConnectAddressFamily::IPV6));
	EXPECT_FALSE(Target.Parse("127.0.0.1:8304,ddnet+quic://127.0.0.1:8303", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	EXPECT_FALSE(Target.Parse("ddnet+quic://127.0.0.1:8303#unknown", NETTYPE_ALL, EConnectAddressFamily::IPV6));
}

#if !defined(CONF_PLATFORM_EMSCRIPTEN)
TEST(ConnectTarget, ChooseTransport)
{
	CConnectTarget Target;
	ASSERT_TRUE(Target.Parse("127.0.0.1:8303", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	CConnectTransportOptions Options;
	CModernTransportStart Start;

	Options.m_Protocol = (int)EConnectProtocol::LEGACY;
	EXPECT_EQ(ChooseConnectTransport(Target, Options, nullptr, &Start), EConnectTransport::LEGACY);

	// Picked by hand for a server nobody lists, QUIC is taken at its word and
	// the server trusted on first use.
	Options.m_Protocol = (int)EConnectProtocol::QUIC;
	ASSERT_EQ(ChooseConnectTransport(Target, Options, nullptr, &Start), EConnectTransport::MODERN);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::TOFU);
	EXPECT_EQ(Start.m_Address.type, NETTYPE_IPV4);
	EXPECT_EQ(Start.m_Address.port, 8303);
	EXPECT_STREQ(Start.m_aHost, "127.0.0.1");
	EXPECT_STREQ(Start.m_aServerName, "");
	EXPECT_FALSE(Start.m_WebTransport);

	// A listed server that does not announce QUIC gets the legacy transport.
	CServerInfo Listed = {};
	const auto FindListed = [&](const NETADDR &Addr) -> const CServerInfo * {
		return net_addr_comp(&Addr, &Target.m_aAddrs[0]) == 0 ? &Listed : nullptr;
	};
	EXPECT_EQ(ChooseConnectTransport(Target, Options, FindListed, &Start), EConnectTransport::LEGACY);

	// One that does is connected at its QUIC address, with its pin and name.
	Listed.m_Quic.m_NumAddresses = 1;
	ASSERT_EQ(net_addr_from_str(&Listed.m_Quic.m_aAddresses[0], "127.0.0.1:8305"), 0);
	Listed.m_Quic.m_Pin.m_Trust = EModernTransportTrust::IDENTITY;
	ASSERT_EQ(sha256_from_str(&Listed.m_Quic.m_Pin.m_Fingerprint, IDENTITY), 0);
	str_copy(Listed.m_Quic.m_aHostname, "example.com");
	ASSERT_EQ(ChooseConnectTransport(Target, Options, FindListed, &Start), EConnectTransport::MODERN);
	EXPECT_EQ(Start.m_Address.port, 8305);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::IDENTITY);
	EXPECT_STREQ(Start.m_aServerName, "example.com");

	// The settings override what the server announces.
	Options.m_pCertificateSha256 = IDENTITY;
	Options.m_pServerName = "other.example.com";
	ASSERT_EQ(ChooseConnectTransport(Target, Options, FindListed, &Start), EConnectTransport::MODERN);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
	EXPECT_STREQ(Start.m_aServerName, "other.example.com");
	Options.m_pCertificateSha256 = "not a hash";
	EXPECT_EQ(ChooseConnectTransport(Target, Options, FindListed, &Start), EConnectTransport::FAILED);

	// Natively there is no WebTransport client.
	ASSERT_TRUE(Target.Parse("ddnet+wt://127.0.0.1:8303", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	EXPECT_EQ(ChooseConnectTransport(Target, Options, nullptr, &Start), EConnectTransport::FAILED);
}
#endif

TEST(ConnectTarget, KnownHosts)
{
	SHA256_DIGEST Identity;
	ASSERT_EQ(sha256_from_str(&Identity, IDENTITY), 0);
	SHA256_DIGEST OtherIdentity = Identity;
	OtherIdentity.data[0] ^= 1;

	CQuicKnownHosts KnownHosts;
	EXPECT_TRUE(KnownHosts.Add("Example.COM.", 8303, Identity));
	EXPECT_NE(KnownHosts.Find("example.com", 8303), nullptr);
	EXPECT_EQ(KnownHosts.Find("example.com", 8304), nullptr);
	EXPECT_TRUE(KnownHosts.Add("example.com", 8303, Identity));
	EXPECT_FALSE(KnownHosts.Add("example.com", 8303, OtherIdentity));
	EXPECT_FALSE(KnownHosts.Add("example.com", 0, Identity));
	EXPECT_FALSE(KnownHosts.Add("bad host", 8303, Identity));
	EXPECT_TRUE(KnownHosts.Add("[::1]", 8303, Identity));
	EXPECT_NE(KnownHosts.Find("::1", 8303), nullptr);
	EXPECT_TRUE(KnownHosts.Forget("::1", 0));
	EXPECT_FALSE(KnownHosts.Forget("::1", 0));
	EXPECT_EQ(KnownHosts.Hosts().size(), 1u);
}

TEST(ConnectTarget, IdentityCheck)
{
	SHA256_DIGEST Identity;
	ASSERT_EQ(sha256_from_str(&Identity, IDENTITY), 0);
	SHA256_DIGEST OtherIdentity = Identity;
	OtherIdentity.data[0] ^= 1;

	CModernTransportStart Start;
	ASSERT_EQ(net_addr_from_str(&Start.m_Address, "127.0.0.1:8303"), 0);
	str_copy(Start.m_aHost, "127.0.0.1");
	Start.m_Pin.m_Trust = EModernTransportTrust::TOFU;

	// The first connect remembers the identity.
	CQuicKnownHosts KnownHosts;
	CQuicIdentityCheck Check;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_FALSE(Check.Known());
	EXPECT_EQ(Check.Check(Identity.data, 3, &KnownHosts), CQuicIdentityCheck::EResult::MISSING);
	EXPECT_EQ(Check.Check(Identity.data, sizeof(Identity.data), &KnownHosts), CQuicIdentityCheck::EResult::STORED);
	EXPECT_TRUE(Check.Known());
	EXPECT_EQ(KnownHosts.Hosts().size(), 1u);

	// The next one pins it.
	Start.m_Pin.m_Trust = EModernTransportTrust::TOFU;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::IDENTITY);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, Identity);
	EXPECT_EQ(Check.Check(OtherIdentity.data, sizeof(OtherIdentity.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);
	EXPECT_EQ(Check.Check(Identity.data, sizeof(Identity.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// A certificate hash pin has no identity to check.
	Start.m_Pin.m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(nullptr, 0, &KnownHosts), CQuicIdentityCheck::EResult::OK);
}
