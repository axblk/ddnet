#include <base/hash.h>
#include <base/net.h>
#include <base/str.h>

#include <engine/external/json-parser/json.h>
#include <engine/serverbrowser.h>
#include <engine/shared/serverinfo.h>

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

static constexpr const char *FINGERPRINT = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static constexpr const char *NEXT_FINGERPRINT = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

static NETADDR Udp(const char *pAddress)
{
	NETADDR Addr;
	EXPECT_EQ(net_addr_from_url(&Addr, pAddress, nullptr, 0), 0) << pAddress;
	return Addr;
}

// The text is the one of the QUIC transport of the other branch, which reads it.
TEST(ServerInfo, QuicLanExtra)
{
	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = true;
	ASSERT_EQ(sha256_from_str(&Extra.m_QuicSpkiSha256, FINGERPRINT), 0);
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	char aExpected[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	str_format(aExpected, sizeof(aExpected), "quic|spki-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7", FINGERPRINT);
	EXPECT_STREQ(aExtraInfo, aExpected);

	char aaUrls[QUIC_SERVERINFO_MAX_URLS][QUIC_SERVERINFO_URL_MAXSIZE];
	ASSERT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.6+udp://192.168.0.2:8303"), aaUrls), 1);
	str_format(aExpected, sizeof(aExpected), "ddnet+quic://192.168.0.2:8303#spki-sha256=%s", FINGERPRINT);
	EXPECT_STREQ(aaUrls[0], aExpected);
	ASSERT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.7+udp://[::1]:8304"), aaUrls), 1);
	str_format(aExpected, sizeof(aExpected), "tw-0.7+quic://[::1]:8304#spki-sha256=%s", FINGERPRINT);
	EXPECT_STREQ(aaUrls[0], aExpected);

	aExtraInfo[0] = 'x';
	EXPECT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.6+udp://192.168.0.2:8303"), aaUrls), -1);
	str_format(aExtraInfo, sizeof(aExtraInfo), "quic|spki-sha256=%s|capabilities=datagram,map-stream,resume-v2,game-protocol-7", FINGERPRINT);
	EXPECT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.6+udp://192.168.0.2:8303"), aaUrls), -1);
}

TEST(ServerInfo, QuicLanExtraWebTransportAndWebsockets)
{
	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = true;
	ASSERT_EQ(sha256_from_str(&Extra.m_QuicSpkiSha256, FINGERPRINT), 0);
	Extra.m_WebTransport = true;
	char aCertificates[160];
	str_format(aCertificates, sizeof(aCertificates), "cert-sha256=%s,%s", NEXT_FINGERPRINT, FINGERPRINT);
	ASSERT_TRUE(ParseModernTransportFragment(aCertificates, true, &Extra.m_WebTransportPin));
	Extra.m_Websocket = true;
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	char aExpected[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	str_format(aExpected, sizeof(aExpected), "quic|spki-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7|webtransport=hash|wt-cert-sha256=%s,%s|websocket=ws", FINGERPRINT, NEXT_FINGERPRINT, FINGERPRINT);
	EXPECT_STREQ(aExtraInfo, aExpected);

	char aaUrls[QUIC_SERVERINFO_MAX_URLS][QUIC_SERVERINFO_URL_MAXSIZE];
	ASSERT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.6+udp://192.168.0.2:8303"), aaUrls), 3);
	str_format(aExpected, sizeof(aExpected), "ddnet+wt://192.168.0.2:8303#%s", aCertificates);
	EXPECT_STREQ(aaUrls[1], aExpected);
	EXPECT_STREQ(aaUrls[2], "ddnet+ws://192.168.0.2:8303");
	// WebSockets carry no 0.7.
	ASSERT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.7+udp://192.168.0.2:8303"), aaUrls), 2);

	// Web PKI names the host, wss then shows the key raw QUIC shows.
	Extra.m_WebTransportPin = {};
	Extra.m_WebTransportPin.m_Trust = EModernTransportTrust::WEBPKI;
	Extra.m_pHostname = "server.example.com";
	Extra.m_WebsocketTls = true;
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	ASSERT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.6+udp://192.168.0.2:8303"), aaUrls), 3);
	EXPECT_STREQ(aaUrls[1], "ddnet+wt://server.example.com:8303#webpki");
	str_format(aExpected, sizeof(aExpected), "ddnet+wss://192.168.0.2:8303#spki-sha256=%s", FINGERPRINT);
	EXPECT_STREQ(aaUrls[2], aExpected);

	// A server with WebTransport only, and segments a client does not know.
	Extra.m_RawQuic = false;
	Extra.m_Websocket = false;
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	EXPECT_TRUE(str_startswith(aExtraInfo, "webtransport|capabilities="));
	str_append(aExtraInfo, "|future=whatever");
	ASSERT_EQ(ParseQuicServerInfoExtra(aExtraInfo, Udp("tw-0.6+udp://192.168.0.2:8303"), aaUrls), 1);
	EXPECT_STREQ(aaUrls[0], "ddnet+wt://server.example.com:8303#webpki");
}
