#include <base/str.h>

#include <engine/client/connect_target.h>
#include <engine/shared/quic_transport.h>

#include <gtest/gtest.h>

#include <string>

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

TEST(ConnectTarget, LegacyAddresses)
{
	CConnectTarget Target;
	NETADDR aAddrs[MAX_SERVER_ADDRESSES];
	ASSERT_TRUE(Target.Parse("127.0.0.1:8303,tw-0.6+udp://127.0.0.2:8303,ddnet-20+ws://127.0.0.3:8303", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	ASSERT_EQ(Target.LegacyAddresses(NETTYPE_ALL, aAddrs), 3);
	EXPECT_EQ(aAddrs[0].type, NETTYPE_IPV4);
	EXPECT_EQ(aAddrs[1].type, NETTYPE_IPV4);
	EXPECT_EQ(aAddrs[2].type, NETTYPE_WEBSOCKET_IPV4);

	// Without UDP, as in a browser, an address without a scheme is a
	// websocket one, and one that names UDP cannot be reached.
	ASSERT_EQ(Target.LegacyAddresses(NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6, aAddrs), 2);
	EXPECT_EQ(aAddrs[0].type, NETTYPE_WEBSOCKET_IPV4);
	EXPECT_EQ(aAddrs[0].ip[3], 1);
	EXPECT_EQ(aAddrs[1].type, NETTYPE_WEBSOCKET_IPV4);
	EXPECT_EQ(aAddrs[1].ip[3], 3);
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
TEST(ConnectTarget, SecureWebsockets)
{
	CConnectTarget Target;
	// Only where the client can open them.
	EXPECT_TRUE(Target.Parse("ddnet-20+wss://127.0.0.1:8303", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	EXPECT_EQ(Target.m_NumAddrs, 0);

	const int NetTypes = NETTYPE_ALL | NETTYPE_WEBSOCKET_TLS;
	ASSERT_TRUE(Target.Parse("ddnet-20+wss://127.0.0.1:8303", NetTypes, EConnectAddressFamily::IPV6));
	ASSERT_EQ(Target.m_NumAddrs, 1);
	EXPECT_EQ(Target.m_aAddrs[0].type, NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_TLS);
	// Without a fragment the key is trusted on first use, as for raw QUIC.
	EXPECT_EQ(Target.m_aWebsocketPins[0].m_Trust, EModernTransportTrust::TOFU);
	NETADDR aAddrs[MAX_SERVER_ADDRESSES];
	ASSERT_EQ(Target.LegacyAddresses(NetTypes, aAddrs), 1);
	EXPECT_EQ(aAddrs[0].type, NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_TLS);

	// The fragment pins the key, and wss:// is read as ddnet-20+wss://.
	char aAddress[256];
	str_format(aAddress, sizeof(aAddress), "wss://127.0.0.1:8304#spki-sha256=%s", SPKI_SHA256);
	ASSERT_TRUE(Target.Parse(aAddress, NetTypes, EConnectAddressFamily::IPV6));
	ASSERT_EQ(Target.m_NumAddrs, 1);
	EXPECT_EQ(Target.m_aAddrs[0].type, NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_TLS);
	EXPECT_EQ(Target.m_aAddrs[0].port, 8304);
	EXPECT_EQ(Target.m_aWebsocketPins[0].m_Trust, EModernTransportTrust::SPKI_HASH);
	SHA256_DIGEST Expected;
	ASSERT_EQ(sha256_from_str(&Expected, SPKI_SHA256), 0);
	EXPECT_EQ(Target.m_aWebsocketPins[0].m_Fingerprint, Expected);

	ASSERT_TRUE(Target.Parse("ddnet-20+ws://127.0.0.1:8303,ddnet-20+wss://127.0.0.1:8304#webpki", NetTypes, EConnectAddressFamily::IPV6));
	ASSERT_EQ(Target.m_NumAddrs, 2);
	EXPECT_EQ(Target.m_aAddrs[0].type, NETTYPE_WEBSOCKET_IPV4);
	EXPECT_EQ(Target.m_aWebsocketPins[1].m_Trust, EModernTransportTrust::WEBPKI);

	// Certificate hashes are for WebTransport, not for wss.
	str_format(aAddress, sizeof(aAddress), "ddnet-20+wss://127.0.0.1:8303#cert-sha256=%s", SPKI_SHA256);
	EXPECT_FALSE(Target.Parse(aAddress, NetTypes, EConnectAddressFamily::IPV6));
	EXPECT_FALSE(Target.Parse("ddnet-20+wss://127.0.0.1:8303#spki-sha256=00", NetTypes, EConnectAddressFamily::IPV6));
	EXPECT_FALSE(Target.Parse("ddnet-20+wss://127.0.0.1:8303#", NetTypes, EConnectAddressFamily::IPV6));
}

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

// A key remembered on first use, then the server changes its key and the list
// pins the new one: the list wins and brings the remembered key up to date, so
// that a link without a pin works afterwards. Natively wss does the same.
TEST(ConnectTarget, KeyCheckPinUpdatesRememberedKey)
{
	SHA256_DIGEST KeyA;
	ASSERT_EQ(sha256_from_str(&KeyA, SPKI_SHA256), 0);
	SHA256_DIGEST KeyB = KeyA;
	KeyB.data[0] ^= 1;

	CModernTransportStart Tofu;
	ASSERT_EQ(net_addr_from_str(&Tofu.m_Address, "127.0.0.1:8303"), 0);
	str_copy(Tofu.m_aHost, "127.0.0.1");
	Tofu.m_Pin.m_Trust = EModernTransportTrust::TOFU;

	CQuicKnownHosts KnownHosts;
	CQuicIdentityCheck Check;
	CModernTransportStart Start = Tofu;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(KeyA.data, sizeof(KeyA.data), &KnownHosts), CQuicIdentityCheck::EResult::STORED);

	// The server now holds B, which a link without a pin refuses.
	Start = Tofu;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::REMEMBERED);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);

	// The list pins B. The pin counts, and the remembered key follows it.
	CModernTransportStart Listed = Tofu;
	Listed.m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
	Listed.m_Pin.m_Fingerprint = KeyB;
	Listed.m_PinSource = EServerIdentitySource::LIST;
	Start = Listed;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, KeyB);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::LIST);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::STORED);
	ASSERT_NE(KnownHosts.Find("127.0.0.1", 8303), nullptr);
	EXPECT_EQ(KnownHosts.Find("127.0.0.1", 8303)->m_SpkiSha256, KeyB);
	// The same key again changes nothing.
	Start = Listed;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// Now a link without a pin works.
	Start = Tofu;
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, KeyB);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// A pin of a host that is not known does not add it, or every listed
	// server would end up in the settings. Another key is refused.
	CModernTransportStart Other = Listed;
	ASSERT_EQ(net_addr_from_str(&Other.m_Address, "127.0.0.1:8304"), 0);
	Check.Prepare(&Other, KnownHosts);
	EXPECT_EQ(Check.Check(KeyA.data, sizeof(KeyA.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);
	Check.Prepare(&Other, KnownHosts);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);
	EXPECT_EQ(KnownHosts.Find("127.0.0.1", 8304), nullptr);
	EXPECT_EQ(KnownHosts.Hosts().size(), 1u);
}

TEST(ConnectTarget, KnownHostsUpdate)
{
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);
	SHA256_DIGEST OtherKey = Key;
	OtherKey.data[0] ^= 1;
	CQuicKnownHosts KnownHosts;
	EXPECT_FALSE(KnownHosts.Update("example.com", 8303, Key));
	EXPECT_TRUE(KnownHosts.Hosts().empty());
	ASSERT_TRUE(KnownHosts.Add("example.com", 8303, Key));
	EXPECT_FALSE(KnownHosts.Update("Example.com.", 8303, Key));
	EXPECT_TRUE(KnownHosts.Update("Example.com.", 8303, OtherKey));
	EXPECT_EQ(KnownHosts.Find("example.com", 8303)->m_SpkiSha256, OtherKey);
}

TEST(ConnectTarget, ServerIdentityWarning)
{
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);
	SHA256_DIGEST OtherKey;
	ASSERT_EQ(sha256_from_str(&OtherKey, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"), 0);

	CModernTransportStart Start;
	ASSERT_EQ(net_addr_from_str(&Start.m_Address, "127.0.0.1:8303"), 0);
	str_copy(Start.m_aHost, "example.com");
	Start.m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
	Start.m_Pin.m_Fingerprint = Key;
	Start.m_PinSource = EServerIdentitySource::REMEMBERED;
	char aWarning[1024];

	// A remembered key that differs: what it was and is, and how to forget it.
	CServerIdentityFailure Failure = CServerIdentityFailure::Expected(Start, "QUIC");
	Failure.m_HasPresented = true;
	Failure.m_Presented = OtherKey;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), Failure);
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of example.com:8303 over QUIC. "
		"Its key was remembered from an earlier connection. "
		"It should hold the key 0101010101010101…, it showed abcdef0123456789…. "
		"Once you have made sure that the server changed its key, forget the old one with 'quic_forget_host example.com 8303'. "
		"Refresh the server list, or pick another transport next to the address.");

	// A pin from the list or a link, over wss, without the key that was shown.
	Start.m_PinSource = EServerIdentitySource::LIST;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, "wss"));
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of example.com:8303 over wss. "
		"The server list says how to check it. "
		"It should hold the key 0101010101010101…. "
		"Refresh the server list, or pick another transport next to the address.");
	Start.m_PinSource = EServerIdentitySource::LINK;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, "QUIC"));
	EXPECT_NE(str_find(aWarning, " The link says how to check it. "), nullptr);

	// A certificate hash of WebTransport, an IPv6 host.
	str_copy(Start.m_aHost, "2001:db8::1");
	Start.m_Pin.m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
	Start.m_PinSource = EServerIdentitySource::LIST;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, "WebTransport"));
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of [2001:db8::1]:8303 over WebTransport. "
		"The server list says how to check it. "
		"It should show the certificate 0101010101010101…. "
		"Refresh the server list, or pick another transport next to the address.");
	Start.m_PinSource = EServerIdentitySource::SETTING;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, "QUIC"));
	EXPECT_NE(str_find(aWarning, " cl_quic_cert says how to check it. "), nullptr);

	// Web PKI, for an address entered by hand.
	str_copy(Start.m_aHost, "example.com");
	Start.m_Pin.m_Trust = EModernTransportTrust::WEBPKI;
	Start.m_PinSource = EServerIdentitySource::ADDRESS;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, "wss"));
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of example.com:8303 over wss. "
		"The address says how to check it. "
		"Its certificate should be valid for its name (Web PKI). "
		"Refresh the server list, or pick another transport next to the address.");

	EXPECT_STREQ(ServerIdentityWarningTitle(), "Server identity could not be verified");
	EXPECT_STREQ(SERVER_IDENTITY_DISCONNECT_REASON, "server identity could not be verified");
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

static std::string ConnectAddress(const CServerInfo &Info, const CConnectPlatform &Platform, int Protocol, EConnectAddressFamily Family, bool FamilyFirst = false)
{
	char aAddress[512];
	if(!FormatConnectAddress(aAddress, sizeof(aAddress), Info, Platform, Protocol, Family, FamilyFirst))
		return "";
	return aAddress;
}

TEST(ConnectTarget, ConnectAddressNative)
{
	const CServerInfo Info = FullServer();
	const std::string QuicFragment = std::string("#spki-sha256=") + SPKI_SHA256;
	// QUIC by default, DDNet, IPv6 where there is IPv6.
	EXPECT_EQ(ConnectAddress(Info, Native(), -1, EConnectAddressFamily::IPV6), "ddnet+quic://[2001:db8::1]:8303" + QuicFragment);
	EXPECT_EQ(ConnectAddress(Info, Native(), -1, EConnectAddressFamily::IPV4), "ddnet+quic://1.2.3.4:8303" + QuicFragment);
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::LEGACY, EConnectAddressFamily::IPV6), "[2001:db8::1]:8303");
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::LEGACY, EConnectAddressFamily::IPV4), "1.2.3.4:8303");
	// A transport the server or the platform does not have falls back to the best.
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::WEBTRANSPORT, EConnectAddressFamily::IPV4), "ddnet+quic://1.2.3.4:8303" + QuicFragment);
	CConnectPlatform NoQuic = Native();
	NoQuic.m_Modern = false;
	EXPECT_EQ(ConnectAddress(Info, NoQuic, (int)EConnectProtocol::QUIC, EConnectAddressFamily::IPV6), "[2001:db8::1]:8303");

	// 0.7 is only written where there is nothing else.
	CServerInfo Sixup = {};
	AddAddress(&Sixup, "tw-0.7+udp://1.2.3.4:8303");
	AddModern(&Sixup.m_Quic, "tw-0.7+quic://1.2.3.4:8303");
	Sixup.m_Quic.m_Pin = Info.m_Quic.m_Pin;
	EXPECT_EQ(ConnectAddress(Sixup, Native(), -1, EConnectAddressFamily::IPV6), "tw-0.7+quic://1.2.3.4:8303" + QuicFragment);
	EXPECT_EQ(ConnectAddress(Sixup, Native(), (int)EConnectProtocol::LEGACY, EConnectAddressFamily::IPV6), "tw-0.7+udp://1.2.3.4:8303");

	CServerInfo Nothing = {};
	EXPECT_EQ(ConnectAddress(Nothing, Native(), -1, EConnectAddressFamily::IPV6), "");
}

TEST(ConnectTarget, ConnectAddressCombinations)
{
	// QUIC only over IPv4, UDP over both.
	CServerInfo Info = {};
	AddAddress(&Info, "tw-0.6+udp://1.2.3.4:8303");
	AddAddress(&Info, "tw-0.6+udp://[2001:db8::1]:8303");
	AddModern(&Info.m_Quic, "ddnet+quic://1.2.3.4:8303");
	Info.m_Quic.m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
	ASSERT_EQ(sha256_from_str(&Info.m_Quic.m_Pin.m_Fingerprint, SPKI_SHA256), 0);
	const std::string Quic4 = std::string("ddnet+quic://1.2.3.4:8303#spki-sha256=") + SPKI_SHA256;

	const CConnectChoices Choices = ConnectChoicesFor(&Info, "", Native());
	ASSERT_EQ(Choices.m_NumProtocols, 2);
	EXPECT_EQ(Choices.m_aProtocols[0], EConnectProtocol::QUIC);
	EXPECT_EQ(Choices.m_aProtocols[1], EConnectProtocol::LEGACY);
	ASSERT_EQ(Choices.m_NumFamilies, 2);
	EXPECT_EQ(Choices.m_aFamilies[0], EConnectAddressFamily::IPV6);
	EXPECT_EQ(Choices.m_aFamilies[1], EConnectAddressFamily::IPV4);

	// The transport was just picked, or the row: it wins and the family falls back.
	EXPECT_EQ(ConnectAddress(Info, Native(), -1, EConnectAddressFamily::IPV6), Quic4);
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::QUIC, EConnectAddressFamily::IPV6), Quic4);
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::LEGACY, EConnectAddressFamily::IPV6), "[2001:db8::1]:8303");
	// The family was just picked: it wins and the transport falls back.
	EXPECT_EQ(ConnectAddress(Info, Native(), -1, EConnectAddressFamily::IPV6, true), "[2001:db8::1]:8303");
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::QUIC, EConnectAddressFamily::IPV6, true), "[2001:db8::1]:8303");
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::QUIC, EConnectAddressFamily::IPV4, true), Quic4);
	EXPECT_EQ(ConnectAddress(Info, Native(), (int)EConnectProtocol::LEGACY, EConnectAddressFamily::IPV4, true), "1.2.3.4:8303");
	EXPECT_EQ(ConnectAddress(Info, Native(), -1, EConnectAddressFamily::IPV4, true), Quic4);

	// What the box shows is what the dropdowns show.
	EXPECT_EQ(ConnectProtocolOf(Quic4.c_str(), Native()), EConnectProtocol::QUIC);
	EXPECT_EQ(ConnectProtocolOf("[2001:db8::1]:8303", Native()), EConnectProtocol::LEGACY);
	NETADDR Address;
	ASSERT_TRUE(FirstConnectAddress(Quic4.c_str(), &Address));
	EXPECT_EQ(ConnectAddressFamily(Address), EConnectAddressFamily::IPV4);

	// One choice is no choice.
	CServerInfo Single = {};
	AddAddress(&Single, "tw-0.6+udp://1.2.3.4:8303");
	const CConnectChoices SingleChoices = ConnectChoicesFor(&Single, "1.2.3.4:8303", Native());
	EXPECT_EQ(SingleChoices.m_NumProtocols, 1);
	EXPECT_EQ(SingleChoices.m_NumFamilies, 1);
	EXPECT_EQ(SingleChoices.m_aFamilies[0], EConnectAddressFamily::IPV4);
}

TEST(ConnectTarget, ConnectAddressBrowser)
{
	CServerInfo Info = FullServer();
	const std::string WebTransportFragment = std::string("#cert-sha256=") + SPKI_SHA256;
	// WebTransport by default, which here is only on IPv4.
	EXPECT_EQ(ConnectAddress(Info, Browser(), -1, EConnectAddressFamily::IPV6), "ddnet+wt://1.2.3.4:8303" + WebTransportFragment);
	EXPECT_EQ(ConnectAddress(Info, Browser(), (int)EConnectProtocol::WEBSOCKET, EConnectAddressFamily::IPV6), "ddnet-20+ws://1.2.3.4:8304");
	// UDP and QUIC are never written in a browser.
	EXPECT_EQ(ConnectAddress(Info, Browser(), (int)EConnectProtocol::LEGACY, EConnectAddressFamily::IPV6), "ddnet+wt://1.2.3.4:8303" + WebTransportFragment);
	EXPECT_EQ(ConnectAddress(Info, Browser(), (int)EConnectProtocol::QUIC, EConnectAddressFamily::IPV6, true), "ddnet+wt://1.2.3.4:8303" + WebTransportFragment);
	const CConnectChoices Choices = ConnectChoicesFor(&Info, "", Browser());
	ASSERT_EQ(Choices.m_NumProtocols, 2);
	EXPECT_EQ(Choices.m_aProtocols[0], EConnectProtocol::WEBTRANSPORT);
	EXPECT_EQ(Choices.m_aProtocols[1], EConnectProtocol::WEBSOCKET);
	ASSERT_EQ(Choices.m_NumFamilies, 1);
	EXPECT_EQ(Choices.m_aFamilies[0], EConnectAddressFamily::IPV4);

	// Web PKI is checked for the name the server registered.
	Info.m_WebTransport.m_Pin = {EModernTransportTrust::WEBPKI, {}, {}, false};
	str_copy(Info.m_WebTransport.m_aHostname, "game.example.org");
	EXPECT_EQ(ConnectAddress(Info, Browser(), -1, EConnectAddressFamily::IPV6), "ddnet+wt://game.example.org:8303");
	EXPECT_TRUE(ServerHasConnectAddress(Info, "ddnet+wt://game.example.org:8303"));
	EXPECT_FALSE(ServerHasConnectAddress(Info, "ddnet+wt://other.example.org:8303"));
	EXPECT_FALSE(ServerHasConnectAddress(Info, "ddnet+wt://game.example.org:8305"));
}

TEST(ConnectTarget, ConnectAddressOfServer)
{
	const CServerInfo Info = FullServer();
	char aLink[256];
	str_format(aLink, sizeof(aLink), "ddnet+quic://[2001:db8::1]:8303#spki-sha256=%s", SPKI_SHA256);
	EXPECT_TRUE(ServerHasConnectAddress(Info, aLink));
	EXPECT_TRUE(ServerHasConnectAddress(Info, "1.2.3.4:8303"));
	EXPECT_TRUE(ServerHasConnectAddress(Info, "tw-0.7+udp://1.2.3.4:8303"));
	EXPECT_TRUE(ServerHasConnectAddress(Info, "ddnet+wt://1.2.3.4:8303"));
	EXPECT_FALSE(ServerHasConnectAddress(Info, "ddnet+wt://[2001:db8::1]:8303"));
	EXPECT_FALSE(ServerHasConnectAddress(Info, "1.2.3.5:8303"));
	EXPECT_FALSE(ServerHasConnectAddress(Info, "example.com:8303"));

	// A link to a known server keeps all its transports on offer, a link to
	// an unknown one only says what it is.
	const CConnectChoices Known = ConnectChoicesFor(&Info, aLink, Native());
	EXPECT_EQ(Known.m_NumProtocols, 2);
	const CConnectChoices Unknown = ConnectChoicesFor(nullptr, aLink, Native());
	ASSERT_EQ(Unknown.m_NumProtocols, 1);
	EXPECT_EQ(Unknown.m_aProtocols[0], EConnectProtocol::QUIC);
	ASSERT_EQ(Unknown.m_NumFamilies, 1);
	EXPECT_EQ(Unknown.m_aFamilies[0], EConnectAddressFamily::IPV6);
	const CConnectChoices Typed = ConnectChoicesFor(nullptr, "1.2.3.4:8303", Browser());
	ASSERT_EQ(Typed.m_NumProtocols, 1);
	EXPECT_EQ(Typed.m_aProtocols[0], EConnectProtocol::WEBSOCKET);
	EXPECT_EQ(Typed.m_aFamilies[0], EConnectAddressFamily::IPV4);
}

#if !defined(CONF_PLATFORM_EMSCRIPTEN)
TEST(ConnectTarget, ConnectAddressIsConnected)
{
	// The address written for a server is connected to as it is, without
	// the server list: on any tab and before the list is loaded.
	const CServerInfo Info = FullServer();
	const std::string Address = ConnectAddress(Info, Native(), -1, EConnectAddressFamily::IPV4);
	CConnectTarget Target;
	ASSERT_TRUE(Target.Parse(Address.c_str(), NETTYPE_ALL, EConnectAddressFamily::IPV4));
	CConnectTransportOptions Options;
	CModernTransportStart Start;
	ASSERT_EQ(ChooseConnectTransport(Target, Options, nullptr, &Start), EConnectTransport::MODERN);
	EXPECT_EQ(Start.m_Address, Url("ddnet+quic://1.2.3.4:8303"));
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, Info.m_Quic.m_Pin.m_Fingerprint);
	EXPECT_FALSE(Start.m_Sixup);

	// UDP written for the IPv6 that QUIC does not have stays UDP, even where
	// the server is listed with QUIC on IPv4.
	CServerInfo Listed = {};
	AddAddress(&Listed, "tw-0.6+udp://[::1]:8303");
	AddModern(&Listed.m_Quic, "ddnet+quic://127.0.0.1:8303");
	Listed.m_Quic.m_Pin = Info.m_Quic.m_Pin;
	ASSERT_TRUE(Target.Parse("[::1]:8303", NETTYPE_ALL, EConnectAddressFamily::IPV6));
	const auto FindListed = [&](const NETADDR &) -> const CServerInfo * { return &Listed; };
	EXPECT_EQ(ChooseConnectTransport(Target, Options, FindListed, &Start), EConnectTransport::LEGACY);
}
#endif
