#include <base/net.h>
#include <base/str.h>

#include <engine/external/json-parser/json.h>
#include <engine/serverbrowser.h>
#include <engine/shared/serverinfo.h>
#include <engine/shared/transport_pin.h>

#include <gtest/gtest.h>

TEST(ServerInfo, ParseLocation)
{
	int Result;
	EXPECT_TRUE(CServerInfo::ParseLocation(&Result, "xx"));
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "an"));
	EXPECT_EQ(Result, CServerInfo::LOC_UNKNOWN);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "af"));
	EXPECT_EQ(Result, CServerInfo::LOC_AFRICA);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "eu-n"));
	EXPECT_EQ(Result, CServerInfo::LOC_EUROPE);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "na"));
	EXPECT_EQ(Result, CServerInfo::LOC_NORTH_AMERICA);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "sa"));
	EXPECT_EQ(Result, CServerInfo::LOC_SOUTH_AMERICA);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "as:e"));
	EXPECT_EQ(Result, CServerInfo::LOC_ASIA);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "as:cn"));
	EXPECT_EQ(Result, CServerInfo::LOC_CHINA);
	EXPECT_FALSE(CServerInfo::ParseLocation(&Result, "oc"));
	EXPECT_EQ(Result, CServerInfo::LOC_AUSTRALIA);
}

static unsigned int ParseCrcOrDeadbeef(const char *pString)
{
	unsigned int Result;
	if(ParseCrc(&Result, pString))
	{
		Result = 0xdeadbeef;
	}
	return Result;
}

TEST(ServerInfo, Crc)
{
	EXPECT_EQ(ParseCrcOrDeadbeef("00000000"), 0);
	EXPECT_EQ(ParseCrcOrDeadbeef("00000001"), 1);
	EXPECT_EQ(ParseCrcOrDeadbeef("12345678"), 0x12345678);
	EXPECT_EQ(ParseCrcOrDeadbeef("9abcdef0"), 0x9abcdef0);

	EXPECT_EQ(ParseCrcOrDeadbeef(""), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("a"), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("x"), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("ç"), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("😢"), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("0"), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("000000000"), 0xdeadbeef);
	EXPECT_EQ(ParseCrcOrDeadbeef("00000000x"), 0xdeadbeef);
}

TEST(ServerInfo, WebTransportUrl)
{
	char aUrl[256];
	EXPECT_TRUE(FormatWebTransportUrl(aUrl, sizeof(aUrl), "example.com", 8303));
	EXPECT_STREQ(aUrl, "https://example.com:8303/ddnet");
	EXPECT_TRUE(FormatWebTransportUrl(aUrl, sizeof(aUrl), "[::1]", 8303));
	EXPECT_STREQ(aUrl, "https://[::1]:8303/ddnet");
	EXPECT_FALSE(FormatWebTransportUrl(aUrl, sizeof(aUrl), "::1", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("http://example.com:8303/ddnet", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://example.com:8304/ddnet", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://user@example.com:8303/ddnet", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://example.com:8303/other", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://[not-an-ip]:8303/ddnet", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://example\\.com:8303/ddnet", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://-example.com:8303/ddnet", 8303));
	EXPECT_FALSE(ValidateWebTransportUrl("https://example-.com:8303/ddnet", 8303));
}

static constexpr const char *FINGERPRINT = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static constexpr const char *NEXT_FINGERPRINT = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

TEST(ServerInfo, ModernTransportUrl)
{
	SHA256_DIGEST Fingerprint, NextFingerprint;
	ASSERT_EQ(sha256_from_str(&Fingerprint, FINGERPRINT), 0);
	ASSERT_EQ(sha256_from_str(&NextFingerprint, NEXT_FINGERPRINT), 0);
	bool WebTransport;
	CModernTransportPin Pin;
	EXPECT_TRUE(ParseModernTransportUrl("ddnet+quic://example.com:8303", &WebTransport, &Pin));
	EXPECT_FALSE(WebTransport);
	EXPECT_EQ(Pin.m_Trust, EModernTransportTrust::TOFU);
	EXPECT_TRUE(ParseModernTransportUrl("ddnet+quic://example.com:8303#webpki", &WebTransport, &Pin));
	EXPECT_EQ(Pin.m_Trust, EModernTransportTrust::WEBPKI);
	char aUrl[256];
	str_format(aUrl, sizeof(aUrl), "tw-0.7+quic://[::1]:8303#identity-sha256=%s", FINGERPRINT);
	EXPECT_TRUE(ParseModernTransportUrl(aUrl, &WebTransport, &Pin));
	EXPECT_EQ(Pin.m_Trust, EModernTransportTrust::IDENTITY);
	EXPECT_EQ(Pin.m_Fingerprint, Fingerprint);
	str_format(aUrl, sizeof(aUrl), "ddnet+quic://example.com:8303#cert-sha256=%s,%s", FINGERPRINT, NEXT_FINGERPRINT);
	EXPECT_TRUE(ParseModernTransportUrl(aUrl, &WebTransport, &Pin));
	EXPECT_EQ(Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
	EXPECT_EQ(Pin.m_Fingerprint, Fingerprint);
	EXPECT_TRUE(Pin.m_HasNextFingerprint);
	EXPECT_EQ(Pin.m_NextFingerprint, NextFingerprint);
	EXPECT_TRUE(ParseModernTransportUrl("ddnet+wt://example.com:8303", &WebTransport, &Pin));
	EXPECT_TRUE(WebTransport);
	EXPECT_EQ(Pin.m_Trust, EModernTransportTrust::WEBPKI);

	str_format(aUrl, sizeof(aUrl), "ddnet+wt://example.com:8303#identity-sha256=%s", FINGERPRINT);
	EXPECT_FALSE(ParseModernTransportUrl(aUrl, &WebTransport, &Pin));
	str_format(aUrl, sizeof(aUrl), "ddnet+quic://example.com:8303#cert-sha256=%s,%s", FINGERPRINT, FINGERPRINT);
	EXPECT_FALSE(ParseModernTransportUrl(aUrl, &WebTransport, &Pin));
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+quic://example.com:8303#identity-sha256=00", &WebTransport, &Pin));
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+quic://user@example.com:8303", &WebTransport, &Pin));
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+quic://example.com:8303/path", &WebTransport, &Pin));
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+quic://example.com:8303#unknown", &WebTransport, &Pin));
	EXPECT_FALSE(ParseModernTransportUrl("example.com:8303", &WebTransport, &Pin));
}

TEST(ServerInfo, QuicLanExtra)
{
	SHA256_DIGEST Fingerprint;
	ASSERT_EQ(sha256_from_str(&Fingerprint, FINGERPRINT), 0);
	NETADDR Addr;
	ASSERT_EQ(net_addr_from_str(&Addr, "192.168.0.2:8303"), 0);
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = true;
	Extra.m_IdentityFingerprint = Fingerprint;
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	char aExpected[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	str_format(aExpected, sizeof(aExpected), "ddnet-transport-v2|quic|identity-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7", FINGERPRINT);
	EXPECT_STREQ(aExtraInfo, aExpected);

	CServerInfo Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	ASSERT_EQ(Info.m_Quic.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_Quic.m_aAddresses[0], Addr);
	EXPECT_EQ(Info.m_Quic.m_Pin.m_Trust, EModernTransportTrust::IDENTITY);
	EXPECT_EQ(Info.m_Quic.m_Pin.m_Fingerprint, Fingerprint);
	EXPECT_EQ(Info.m_WebTransport.m_NumAddresses, 0);

	aExtraInfo[0] = 'x';
	EXPECT_TRUE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	EXPECT_EQ(Info.m_Quic.m_NumAddresses, 0);
	str_format(aExtraInfo, sizeof(aExtraInfo), "ddnet-transport-v2|quic|identity-sha256=%s|capabilities=datagram,map-stream,resume-v2,game-protocol-7", FINGERPRINT);
	EXPECT_TRUE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	str_format(aExtraInfo, sizeof(aExtraInfo), "ddnet-transport-v1|quic|tls-certificate-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7", FINGERPRINT);
	EXPECT_TRUE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
}

TEST(ServerInfo, QuicLanExtraWebTransport)
{
	SHA256_DIGEST Fingerprint, Certificate, NextCertificate;
	ASSERT_EQ(sha256_from_str(&Fingerprint, FINGERPRINT), 0);
	ASSERT_EQ(sha256_from_str(&Certificate, NEXT_FINGERPRINT), 0);
	ASSERT_EQ(sha256_from_str(&NextCertificate, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"), 0);
	NETADDR Addr;
	ASSERT_EQ(net_addr_from_str(&Addr, "192.168.0.2:8303"), 0);

	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = true;
	Extra.m_IdentityFingerprint = Fingerprint;
	Extra.m_WebTransport = true;
	Extra.m_WebTransportPin = {EModernTransportTrust::CERTIFICATE_HASH, Certificate, NextCertificate, true};
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);

	CServerInfo Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	EXPECT_EQ(Info.m_Quic.m_Pin.m_Trust, EModernTransportTrust::IDENTITY);
	EXPECT_EQ(Info.m_Quic.m_Pin.m_Fingerprint, Fingerprint);
	ASSERT_EQ(Info.m_WebTransport.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_WebTransport.m_aAddresses[0], Addr);
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Fingerprint, Certificate);
	EXPECT_TRUE(Info.m_WebTransport.m_Pin.m_HasNextFingerprint);
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_NextFingerprint, NextCertificate);

	Extra.m_WebTransportPin = {EModernTransportTrust::WEBPKI, {}, {}, false};
	Extra.m_pHostname = "server.example.com";
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Trust, EModernTransportTrust::WEBPKI);
	EXPECT_STREQ(Info.m_WebTransport.m_aHostname, "server.example.com");
	EXPECT_STREQ(Info.m_Quic.m_aHostname, "");

	// Unknown segments are skipped instead of rejecting the whole string.
	str_format(aExtraInfo, sizeof(aExtraInfo), "ddnet-transport-v2|quic|identity-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7|future=whatever|webtransport=hash", FINGERPRINT);
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
}

TEST(ServerInfo, QuicLanExtraWebTransportOnly)
{
	SHA256_DIGEST Certificate;
	ASSERT_EQ(sha256_from_str(&Certificate, FINGERPRINT), 0);
	NETADDR Addr;
	ASSERT_EQ(net_addr_from_str(&Addr, "192.168.0.2:8303"), 0);

	CQuicServerInfoExtra Extra = {};
	Extra.m_WebTransport = true;
	Extra.m_WebTransportPin = {EModernTransportTrust::CERTIFICATE_HASH, Certificate, {}, false};
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	EXPECT_TRUE(str_startswith(aExtraInfo, "ddnet-transport-v2|webtransport|capabilities="));

	CServerInfo Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, Addr));
	EXPECT_EQ(Info.m_Quic.m_NumAddresses, 0);
	ASSERT_EQ(Info.m_WebTransport.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Fingerprint, Certificate);
}
