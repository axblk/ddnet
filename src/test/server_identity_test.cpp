#include <base/net.h>
#include <base/str.h>

#include <engine/client/server_identity.h>

#include <gtest/gtest.h>

static constexpr const char *SPKI_SHA256 = "0101010101010101010101010101010101010101010101010101010101010101";

static NETADDR Url(const char *pUrl)
{
	NETADDR Address = {};
	EXPECT_EQ(net_addr_from_url(&Address, pUrl, nullptr, 0), 0) << pUrl;
	return Address;
}

// Whether a first connect remembers the key the server shows.
static bool Remembers(CModernTransportStart Start)
{
	CQuicKnownHosts KnownHosts;
	CQuicIdentityCheck Check;
	Check.Prepare(&Start, KnownHosts);
	const SHA256_DIGEST Key = {};
	return Check.Check(Key.data, sizeof(Key.data), &KnownHosts) == CQuicIdentityCheck::EResult::STORED;
}

TEST(ServerIdentity, Start)
{
	char aFragment[256];
	str_format(aFragment, sizeof(aFragment), "spki-sha256=%s", SPKI_SHA256);
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);

	CModernTransportStart Start;
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+quic://[::1]:8303"), "", aFragment, false));
	EXPECT_STREQ(Start.m_aHost, "::1");
	EXPECT_EQ(Start.m_Address.port, 8303);
	EXPECT_STREQ(ModernTransportName(Start), "QUIC");
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, Key);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::LINK);
	// A pin does not make a new host known.
	EXPECT_FALSE(Remembers(Start));
	char aSpki[128];
	FormatModernTransportFragment(aSpki, sizeof(aSpki), Start.m_WebTransport, Start.m_Pin);
	EXPECT_STREQ(aSpki, aFragment);

	// The name it came as counts, not the address it has.
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+wss://127.0.0.1:8304"), "Example.COM.", aFragment, true));
	EXPECT_STREQ(Start.m_aHost, "example.com");
	EXPECT_STREQ(ModernTransportName(Start), "wss");
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::LIST);

	// Without a fragment the key is trusted on first use.
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("tw-0.7+quic://127.0.0.1:8303"), "", "", true));
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::TOFU);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::ADDRESS);
	EXPECT_TRUE(Remembers(Start));
	FormatModernTransportFragment(aSpki, sizeof(aSpki), Start.m_WebTransport, Start.m_Pin);
	EXPECT_STREQ(aSpki, "");

	// WebTransport pins certificates and remembers nothing.
	str_format(aFragment, sizeof(aFragment), "cert-sha256=%s,%s", SPKI_SHA256, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+wt://127.0.0.1:8303"), "", aFragment, false));
	EXPECT_STREQ(ModernTransportName(Start), "WebTransport");
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, Key);
	EXPECT_FALSE(Remembers(Start));
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+quic://127.0.0.1:8303"), "", "webpki", false));
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::WEBPKI);
	EXPECT_FALSE(Remembers(Start));
	// WebTransport without a fragment is checked by Web PKI, the name is the
	// one the certificate is checked for.
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+wt://127.0.0.1:8303"), "example.com", "", false));
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::WEBPKI);
	EXPECT_STREQ(Start.m_aServerName, "example.com");

	// Transports that show no identity, and fragments that cannot be read.
	EXPECT_FALSE(SetModernTransportStart(&Start, Url("tw-0.6+udp://127.0.0.1:8303"), "", "", false));
	EXPECT_FALSE(SetModernTransportStart(&Start, Url("ddnet+ws://127.0.0.1:8303"), "", "", false));
	EXPECT_FALSE(SetModernTransportStart(&Start, Url("ddnet+quic://127.0.0.1:8303"), "", "spki-sha256=zz", false));
	EXPECT_FALSE(SetModernTransportStart(&Start, Url("ddnet+quic://127.0.0.1:8303"), "", "identity-sha256=00", false));
	str_format(aFragment, sizeof(aFragment), "spki-sha256=%s", SPKI_SHA256);
	EXPECT_FALSE(SetModernTransportStart(&Start, Url("ddnet+wt://127.0.0.1:8303"), "", aFragment, false));
}

TEST(ServerIdentity, KnownHosts)
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

TEST(ServerIdentity, KnownHostsUpdate)
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

TEST(ServerIdentity, KeyCheck)
{
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);
	SHA256_DIGEST OtherKey = Key;
	OtherKey.data[0] ^= 1;

	CModernTransportStart Start;
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+quic://127.0.0.1:8303"), "", "", false));

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
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+quic://127.0.0.1:8303"), "", "", false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, Key);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::REMEMBERED);
	EXPECT_EQ(Check.Check(OtherKey.data, sizeof(OtherKey.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);
	EXPECT_EQ(Check.Check(Key.data, sizeof(Key.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// Native wss shares the known hosts.
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+wss://127.0.0.1:8303"), "", "", false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::REMEMBERED);

	// A certificate hash, WebTransport and Web PKI have no key to check.
	char aFragment[160];
	str_format(aFragment, sizeof(aFragment), "cert-sha256=%s", SPKI_SHA256);
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+wt://127.0.0.1:8303"), "", aFragment, false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(nullptr, 0, &KnownHosts), CQuicIdentityCheck::EResult::OK);
	ASSERT_TRUE(SetModernTransportStart(&Start, Url("ddnet+quic://127.0.0.1:8303"), "", "webpki", false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(OtherKey.data, sizeof(OtherKey.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);
	EXPECT_EQ(KnownHosts.Find("127.0.0.1", 8303)->m_SpkiSha256, Key);
}

// A key remembered on first use, then the server changes its key and the list
// pins the new one: the list wins and brings the remembered key up to date, so
// that a link without a pin works afterwards. Natively wss does the same.
TEST(ServerIdentity, KeyCheckPinUpdatesRememberedKey)
{
	SHA256_DIGEST KeyA;
	ASSERT_EQ(sha256_from_str(&KeyA, SPKI_SHA256), 0);
	SHA256_DIGEST KeyB = KeyA;
	KeyB.data[0] ^= 1;
	char aKeyB[SHA256_MAXSTRSIZE];
	sha256_str(KeyB, aKeyB, sizeof(aKeyB));
	char aPinB[128];
	str_format(aPinB, sizeof(aPinB), "spki-sha256=%s", aKeyB);
	const NETADDR Address = Url("ddnet+quic://127.0.0.1:8303");

	CQuicKnownHosts KnownHosts;
	CQuicIdentityCheck Check;
	CModernTransportStart Start;
	ASSERT_TRUE(SetModernTransportStart(&Start, Address, "", "", false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(KeyA.data, sizeof(KeyA.data), &KnownHosts), CQuicIdentityCheck::EResult::STORED);

	// The server now holds B, which a link without a pin refuses.
	ASSERT_TRUE(SetModernTransportStart(&Start, Address, "", "", false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::REMEMBERED);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);

	// The list pins B. The pin counts, and the remembered key follows it.
	ASSERT_TRUE(SetModernTransportStart(&Start, Address, "", aPinB, true));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, KeyB);
	EXPECT_EQ(Start.m_PinSource, EServerIdentitySource::LIST);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::STORED);
	ASSERT_NE(KnownHosts.Find("127.0.0.1", 8303), nullptr);
	EXPECT_EQ(KnownHosts.Find("127.0.0.1", 8303)->m_SpkiSha256, KeyB);
	// The same key again changes nothing.
	ASSERT_TRUE(SetModernTransportStart(&Start, Address, "", aPinB, true));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// Now a link without a pin works.
	ASSERT_TRUE(SetModernTransportStart(&Start, Address, "", "", false));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Start.m_Pin.m_Fingerprint, KeyB);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);

	// A pin of a host that is not known does not add it, or every listed
	// server would end up in the settings. Another key is refused.
	const NETADDR Other = Url("ddnet+quic://127.0.0.1:8304");
	ASSERT_TRUE(SetModernTransportStart(&Start, Other, "", aPinB, true));
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(KeyA.data, sizeof(KeyA.data), &KnownHosts), CQuicIdentityCheck::EResult::CHANGED);
	Check.Prepare(&Start, KnownHosts);
	EXPECT_EQ(Check.Check(KeyB.data, sizeof(KeyB.data), &KnownHosts), CQuicIdentityCheck::EResult::OK);
	EXPECT_EQ(KnownHosts.Find("127.0.0.1", 8304), nullptr);
	EXPECT_EQ(KnownHosts.Hosts().size(), 1u);
}

TEST(ServerIdentity, Warning)
{
	SHA256_DIGEST Key;
	ASSERT_EQ(sha256_from_str(&Key, SPKI_SHA256), 0);
	SHA256_DIGEST OtherKey;
	ASSERT_EQ(sha256_from_str(&OtherKey, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"), 0);

	CModernTransportStart Start;
	const char *pTransport;
	str_copy(Start.m_aHost, "example.com");
	Start.m_Address.port = 8303;
	pTransport = "QUIC";
	Start.m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
	Start.m_Pin.m_Fingerprint = Key;
	Start.m_PinSource = EServerIdentitySource::REMEMBERED;
	char aWarning[1024];

	// A remembered key that differs: what it was and is, and how to forget it.
	CServerIdentityFailure Failure = CServerIdentityFailure::Expected(Start, pTransport);
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
	pTransport = "wss";
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, pTransport));
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of example.com:8303 over wss. "
		"The server list says how to check it. "
		"It should hold the key 0101010101010101…. "
		"Refresh the server list, or pick another transport next to the address.");
	Start.m_PinSource = EServerIdentitySource::LINK;
	pTransport = "QUIC";
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, pTransport));
	EXPECT_NE(str_find(aWarning, " The link says how to check it. "), nullptr);

	// A certificate hash of WebTransport, an IPv6 host.
	str_copy(Start.m_aHost, "2001:db8::1");
	pTransport = "WebTransport";
	Start.m_Pin.m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
	Start.m_PinSource = EServerIdentitySource::LIST;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, pTransport));
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of [2001:db8::1]:8303 over WebTransport. "
		"The server list says how to check it. "
		"It should show the certificate 0101010101010101…. "
		"Refresh the server list, or pick another transport next to the address.");

	// Web PKI, for an address entered by hand.
	str_copy(Start.m_aHost, "example.com");
	pTransport = "wss";
	Start.m_Pin.m_Trust = EModernTransportTrust::WEBPKI;
	Start.m_PinSource = EServerIdentitySource::ADDRESS;
	FormatServerIdentityWarning(aWarning, sizeof(aWarning), CServerIdentityFailure::Expected(Start, pTransport));
	EXPECT_STREQ(aWarning,
		"Could not verify the identity of example.com:8303 over wss. "
		"The address says how to check it. "
		"Its certificate should be valid for its name (Web PKI). "
		"Refresh the server list, or pick another transport next to the address.");

	EXPECT_STREQ(ServerIdentityWarningTitle(), "Server identity could not be verified");
	EXPECT_STREQ(SERVER_IDENTITY_DISCONNECT_REASON, "server identity could not be verified");
}
