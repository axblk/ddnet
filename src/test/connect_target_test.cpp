#include <base/str.h>

#include <engine/client/connect_target.h>
#include <engine/shared/quic_transport.h>

#include <gtest/gtest.h>

static constexpr const char *SPKI_SHA256 = "0101010101010101010101010101010101010101010101010101010101010101";

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
	str_format(aLink, sizeof(aLink), "ddnet+quic://127.0.0.1:8303#spki-sha256=%s", SPKI_SHA256);
	CConnectTarget Target;
	EXPECT_TRUE(Target.Parse(aLink, NETTYPE_ALL, EConnectAddressFamily::IPV6));
	EXPECT_TRUE(Target.m_Link);
	EXPECT_FALSE(Target.m_LinkWebTransport);
	EXPECT_EQ(Target.m_LinkPin.m_Trust, EModernTransportTrust::SPKI_HASH);
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
	Listed.m_Quic.m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
	ASSERT_EQ(sha256_from_str(&Listed.m_Quic.m_Pin.m_Fingerprint, SPKI_SHA256), 0);
	str_copy(Listed.m_Quic.m_aHostname, "example.com");
	ASSERT_EQ(ChooseConnectTransport(Target, Options, FindListed, &Start), EConnectTransport::MODERN);
	EXPECT_EQ(Start.m_Address.port, 8305);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	EXPECT_STREQ(Start.m_aServerName, "example.com");

	// The settings override what the server announces.
	Options.m_pCertificateSha256 = SPKI_SHA256;
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
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);
	SHA256_DIGEST OtherKey = Key;
	OtherKey.data[0] ^= 1;

	CQuicKnownHosts KnownHosts;
	EXPECT_TRUE(KnownHosts.Add("Example.COM.", 8303, Key));
	EXPECT_NE(KnownHosts.Find("example.com", 8303), nullptr);
	EXPECT_EQ(KnownHosts.Find("example.com", 8304), nullptr);
	EXPECT_TRUE(KnownHosts.Add("example.com", 8303, Key));
	EXPECT_FALSE(KnownHosts.Add("example.com", 8303, OtherKey));
	EXPECT_FALSE(KnownHosts.Add("example.com", 0, Key));
	EXPECT_FALSE(KnownHosts.Add("bad host", 8303, Key));
	EXPECT_TRUE(KnownHosts.Add("[::1]", 8303, Key));
	EXPECT_NE(KnownHosts.Find("::1", 8303), nullptr);
	EXPECT_TRUE(KnownHosts.Forget("::1", 0));
	EXPECT_FALSE(KnownHosts.Forget("::1", 0));
	EXPECT_EQ(KnownHosts.Hosts().size(), 1u);
}

TEST(ConnectTarget, KeyCheck)
{
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);
	SHA256_DIGEST OtherKey = Key;
	OtherKey.data[0] ^= 1;

	CModernTransportStart Start;
	ASSERT_EQ(net_addr_from_str(&Start.m_Address, "127.0.0.1:8303"), 0);
	str_copy(Start.m_aHost, "127.0.0.1");
	Start.m_Pin.m_Trust = EModernTransportTrust::TOFU;

	// The first connect remembers the key.
	CQuicKnownHosts KnownHosts;
	CQuicIdentityCheck Check;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_FALSE(Check.Known());
	EXPECT_EQ(Check.Check(Key.data, 3, &KnownHosts), CQuicIdentityCheck::EResult::MISSING);
	EXPECT_EQ(Check.Check(Key.data, sizeof(Key.data), &KnownHosts), CQuicIdentityCheck::EResult::STORED);
	EXPECT_TRUE(Check.Known());
	EXPECT_EQ(KnownHosts.Hosts().size(), 1u);

	// The next one pins it.
	Start.m_Pin.m_Trust = EModernTransportTrust::TOFU;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, Key);
	EXPECT_EQ(Check.Check(OtherKey.data, sizeof(OtherKey.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);
	EXPECT_EQ(Check.Check(Key.data, sizeof(Key.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// A certificate hash pin has no key to check.
	Start.m_Pin.m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(nullptr, 0, &KnownHosts), CQuicIdentityCheck::EResult::OK);
}

static NETADDR Url(const char *pUrl)
{
	NETADDR Address = {};
	EXPECT_EQ(net_addr_from_url(&Address, pUrl, nullptr, 0), 0) << pUrl;
	return Address;
}

static void AddAddress(CServerInfo *pInfo, const char *pUrl)
{
	pInfo->m_aAddresses[pInfo->m_NumAddresses++] = Url(pUrl);
}

static void AddModern(CModernTransportInfo *pTransport, const char *pUrl)
{
	pTransport->m_aAddresses[pTransport->m_NumAddresses++] = Url(pUrl);
}

// A server with everything: UDP 0.6 and 0.7 on both families, a websocket,
// QUIC for both versions and families and WebTransport on IPv4.
static CServerInfo FullServer()
{
	CServerInfo Info = {};
	AddAddress(&Info, "tw-0.6+udp://1.2.3.4:8303");
	AddAddress(&Info, "tw-0.7+udp://1.2.3.4:8303");
	AddAddress(&Info, "tw-0.6+udp://[2001:db8::1]:8303");
	AddAddress(&Info, "ddnet-20+ws://1.2.3.4:8304");
	AddModern(&Info.m_Quic, "ddnet+quic://1.2.3.4:8303");
	AddModern(&Info.m_Quic, "tw-0.7+quic://1.2.3.4:8303");
	AddModern(&Info.m_Quic, "ddnet+quic://[2001:db8::1]:8303");
	Info.m_Quic.m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
	EXPECT_EQ(sha256_from_str(&Info.m_Quic.m_Pin.m_Fingerprint, SPKI_SHA256), 0);
	AddModern(&Info.m_WebTransport, "ddnet+wt://1.2.3.4:8303");
	AddModern(&Info.m_WebTransport, "tw-0.7+wt://1.2.3.4:8303");
	Info.m_WebTransport.m_Pin.m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
	EXPECT_EQ(sha256_from_str(&Info.m_WebTransport.m_Pin.m_Fingerprint, SPKI_SHA256), 0);
	return Info;
}

static CConnectPlatform Native()
{
	CConnectPlatform Platform;
	Platform.m_Modern = true;
	return Platform;
}

static CConnectPlatform Browser()
{
	CConnectPlatform Platform;
	Platform.m_Browser = true;
	Platform.m_Modern = true;
	return Platform;
}

TEST(ConnectTarget, EndpointsNative)
{
	const CServerInfo Info = FullServer();
	// UDP and QUIC, and of those only DDNet, as there is DDNet.
	CServerEndpoints Endpoints(Info, Native());
	ASSERT_EQ(Endpoints.m_NumEndpoints, 4);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Protocol, EConnectProtocol::LEGACY);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Address, Url("tw-0.6+udp://1.2.3.4:8303"));
	EXPECT_EQ(Endpoints.m_aEndpoints[1].m_Protocol, EConnectProtocol::LEGACY);
	EXPECT_EQ(Endpoints.m_aEndpoints[1].m_Address, Url("tw-0.6+udp://[2001:db8::1]:8303"));
	EXPECT_EQ(Endpoints.m_aEndpoints[2].m_Protocol, EConnectProtocol::QUIC);
	EXPECT_EQ(Endpoints.m_aEndpoints[2].m_Address, Url("ddnet+quic://1.2.3.4:8303"));
	EXPECT_EQ(Endpoints.m_aEndpoints[3].m_Protocol, EConnectProtocol::QUIC);
	EXPECT_EQ(Endpoints.m_pModern, &Info.m_Quic);
	EXPECT_FALSE(Endpoints.Has(EConnectProtocol::WEBSOCKET));
	EXPECT_FALSE(Endpoints.Has(EConnectProtocol::WEBTRANSPORT));

	// Without QUIC, UDP is what is left.
	CConnectPlatform NoQuic = Native();
	NoQuic.m_Modern = false;
	Endpoints = CServerEndpoints(Info, NoQuic);
	ASSERT_EQ(Endpoints.m_NumEndpoints, 2);
	EXPECT_EQ(Endpoints.m_pModern, nullptr);

	// 0.7 is only used where there is no DDNet.
	CServerInfo Sixup = {};
	AddAddress(&Sixup, "tw-0.7+udp://1.2.3.4:8303");
	AddModern(&Sixup.m_Quic, "tw-0.7+quic://1.2.3.4:8303");
	Sixup.m_Quic.m_Pin = Info.m_Quic.m_Pin;
	Endpoints = CServerEndpoints(Sixup, Native());
	ASSERT_EQ(Endpoints.m_NumEndpoints, 2);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Protocol, EConnectProtocol::LEGACY);
	EXPECT_EQ(Endpoints.m_aEndpoints[1].m_Protocol, EConnectProtocol::QUIC);
	// A DDNet endpoint of any transport drops all of 0.7.
	AddModern(&Sixup.m_Quic, "ddnet+quic://1.2.3.4:8303");
	Endpoints = CServerEndpoints(Sixup, Native());
	ASSERT_EQ(Endpoints.m_NumEndpoints, 1);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Address, Url("ddnet+quic://1.2.3.4:8303"));

	// A server only reachable over QUIC is known by its QUIC addresses, which
	// are no UDP endpoints.
	CServerInfo ModernOnly = {};
	AddModern(&ModernOnly.m_Quic, "ddnet+quic://1.2.3.4:8303");
	ModernOnly.m_Quic.m_Pin = Info.m_Quic.m_Pin;
	ModernOnly.m_aAddresses[ModernOnly.m_NumAddresses++] = ModernOnly.m_Quic.m_aAddresses[0];
	ModernOnly.m_ModernOnly = true;
	Endpoints = CServerEndpoints(ModernOnly, Native());
	ASSERT_EQ(Endpoints.m_NumEndpoints, 1);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Protocol, EConnectProtocol::QUIC);
	// Natively a server with only WebTransport and websockets is not listed.
	CServerInfo WebOnly = {};
	AddAddress(&WebOnly, "ddnet-20+ws://1.2.3.4:8304");
	WebOnly.m_WebTransport = Info.m_WebTransport;
	EXPECT_EQ(CServerEndpoints(WebOnly, Native()).m_NumEndpoints, 0);
}

TEST(ConnectTarget, EndpointsBrowser)
{
	const CServerInfo Info = FullServer();
	CServerEndpoints Endpoints(Info, Browser());
	ASSERT_EQ(Endpoints.m_NumEndpoints, 2);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Protocol, EConnectProtocol::WEBSOCKET);
	EXPECT_EQ(Endpoints.m_aEndpoints[1].m_Protocol, EConnectProtocol::WEBTRANSPORT);
	EXPECT_EQ(Endpoints.m_aEndpoints[1].m_Address, Url("ddnet+wt://1.2.3.4:8303"));
	EXPECT_EQ(Endpoints.m_pModern, &Info.m_WebTransport);

	// A page served over https only opens secure websockets.
	CConnectPlatform Secure = Browser();
	Secure.m_SecureWebsocketsOnly = true;
	Endpoints = CServerEndpoints(Info, Secure);
	ASSERT_EQ(Endpoints.m_NumEndpoints, 1);
	EXPECT_EQ(Endpoints.m_aEndpoints[0].m_Protocol, EConnectProtocol::WEBTRANSPORT);

	// In a browser a server with only UDP and QUIC is not listed.
	CServerInfo Native = {};
	AddAddress(&Native, "tw-0.6+udp://1.2.3.4:8303");
	Native.m_Quic = Info.m_Quic;
	EXPECT_EQ(CServerEndpoints(Native, Browser()).m_NumEndpoints, 0);
}
