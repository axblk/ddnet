#include <base/str.h>

#include <engine/external/json-parser/json.h>
#include <engine/serverbrowser.h>
#include <engine/shared/json.h>
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

static CServerInfo2 ParseServerInfoWithTransport(const char *pTransport)
{
	char aJson[2048];
	str_format(aJson, sizeof(aJson), R"({"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DDRace","name":"test","map":{"name":"Tutorial"},"version":"test","clients":[],"transport":%s})", pTransport);
	json_value *pJson = JsonParse(aJson, str_length(aJson));
	EXPECT_NE(pJson, nullptr);
	CServerInfo2 Info = {};
	if(pJson != nullptr)
	{
		EXPECT_FALSE(CServerInfo2::FromJson(&Info, pJson));
		json_value_free(pJson);
	}
	return Info;
}

static CServerInfo2 ParseServerInfoWithExperimental(const char *pExperimental)
{
	char aJson[2048];
	str_format(aJson, sizeof(aJson), R"({"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DDRace","name":"test","map":{"name":"Tutorial"},"version":"test","clients":[],"experimental":%s})", pExperimental);
	json_value *pJson = JsonParse(aJson, str_length(aJson));
	EXPECT_NE(pJson, nullptr);
	CServerInfo2 Info = {};
	if(pJson != nullptr)
	{
		EXPECT_FALSE(CServerInfo2::FromJson(&Info, pJson));
		json_value_free(pJson);
	}
	return Info;
}

TEST(ServerInfo, QuicTransport)
{
	static constexpr const char *CERTIFICATE_SHA256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static constexpr const char *NEXT_CERTIFICATE_SHA256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	const CServerInfo2 ParsedInfo = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tls_certificate_sha256_next":"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789","quic":true,"webtransport":{"url":"https://example.com:8303/ddnet","certificate_mode":"hash"}})");
	EXPECT_TRUE(ParsedInfo.m_QuicSharedPort);
	EXPECT_TRUE(ParsedInfo.m_WebTransport);
	EXPECT_EQ(ParsedInfo.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::HASH);
	EXPECT_STREQ(ParsedInfo.m_aWebTransportPath, "/ddnet");
	EXPECT_STREQ(ParsedInfo.m_aWebTransportUrl, "https://example.com:8303/ddnet");
	EXPECT_EQ(ParsedInfo.m_QuicPort, 8303);
	EXPECT_EQ(ParsedInfo.m_QuicCapabilities, CServerInfo::QUIC_CAPABILITY_DATAGRAM | CServerInfo::QUIC_CAPABILITY_MAP_STREAM | CServerInfo::QUIC_CAPABILITY_RESUME | CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7);
	SHA256_DIGEST CertificateSha256;
	SHA256_DIGEST NextCertificateSha256;
	ASSERT_EQ(sha256_from_str(&CertificateSha256, CERTIFICATE_SHA256), 0);
	ASSERT_EQ(sha256_from_str(&NextCertificateSha256, NEXT_CERTIFICATE_SHA256), 0);
	EXPECT_EQ(ParsedInfo.m_QuicCertificateSha256, CertificateSha256);
	EXPECT_TRUE(ParsedInfo.m_HasQuicNextCertificateSha256);
	EXPECT_EQ(ParsedInfo.m_QuicNextCertificateSha256, NextCertificateSha256);

	const CServerInfo Info = ParsedInfo;
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicCertificateSha256, CertificateSha256);
	EXPECT_EQ(Info.m_QuicNextCertificateSha256, NextCertificateSha256);
	EXPECT_TRUE(Info.m_WebTransport);

	const CServerInfo2 WebTransportOnly = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","webtransport":{"url":"https://[::1]:8303/ddnet","certificate_mode":"webpki"}})");
	EXPECT_FALSE(WebTransportOnly.m_QuicSharedPort);
	EXPECT_TRUE(WebTransportOnly.m_WebTransport);
	EXPECT_EQ(WebTransportOnly.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::WEBPKI);
	EXPECT_EQ(WebTransportOnly.m_WebTransportCertificateSha256, CertificateSha256);
	EXPECT_EQ(WebTransportOnly.m_QuicCapabilities, CServerInfo::QUIC_CAPABILITY_DATAGRAM | CServerInfo::QUIC_CAPABILITY_MAP_STREAM | CServerInfo::QUIC_CAPABILITY_RESUME | CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7);

	const CServerInfo2 InvalidInfo = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"invalid","quic":true})");
	EXPECT_FALSE(InvalidInfo.m_QuicSharedPort);
	const CServerInfo2 InvalidPortInfo = ParseServerInfoWithTransport(R"({"udp_port":65536,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","quic":true})");
	EXPECT_FALSE(InvalidPortInfo.m_QuicSharedPort);
	const CServerInfo2 WrongWebTransportPort = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","webtransport":{"url":"https://example.com:8304/ddnet","certificate_mode":"webpki"}})");
	EXPECT_FALSE(WrongWebTransportPort.m_WebTransport);
	const CServerInfo2 MissingCertificateMode = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","webtransport":{"url":"https://example.com:8303/ddnet"}})");
	EXPECT_FALSE(MissingCertificateMode.m_WebTransport);
	const CServerInfo2 InvalidCertificateMode = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","webtransport":{"url":"https://example.com:8303/ddnet","certificate_mode":"unknown"}})");
	EXPECT_FALSE(InvalidCertificateMode.m_WebTransport);
	const CServerInfo2 DomainlessHash = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","webtransport":{"certificate_mode":"hash"}})");
	EXPECT_TRUE(DomainlessHash.m_WebTransport);
	EXPECT_STREQ(DomainlessHash.m_aWebTransportUrl, "");
	const CServerInfo2 DomainlessWebPki = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","webtransport":{"certificate_mode":"webpki"}})");
	EXPECT_FALSE(DomainlessWebPki.m_WebTransport);
	const CServerInfo2 DuplicatePinInfo = ParseServerInfoWithTransport(R"({"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tls_certificate_sha256_next":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","quic":true})");
	EXPECT_FALSE(DuplicatePinInfo.m_QuicSharedPort);
}

TEST(ServerInfo, ExperimentalProtocolTransport)
{
	static constexpr const char *CERTIFICATE_SHA256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static constexpr const char *IDENTITY_SHA256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	const CServerInfo2 Info = ParseServerInfoWithExperimental(R"({"proto":{"quic":{"verify":"identity","sha256":"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},"webtransport":{"verify":"hash","sha256":["0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"]}}})");
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicTrust, EModernTransportTrust::IDENTITY);
	EXPECT_TRUE(Info.m_HasQuicIdentityFingerprint);
	EXPECT_TRUE(Info.m_WebTransport);
	EXPECT_EQ(Info.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::HASH);
	EXPECT_EQ(Info.m_QuicPort, 0);
	SHA256_DIGEST CertificateSha256;
	SHA256_DIGEST IdentitySha256;
	ASSERT_EQ(sha256_from_str(&CertificateSha256, CERTIFICATE_SHA256), 0);
	ASSERT_EQ(sha256_from_str(&IdentitySha256, IDENTITY_SHA256), 0);
	EXPECT_EQ(Info.m_WebTransportCertificateSha256, CertificateSha256);
	EXPECT_EQ(Info.m_QuicCertificateSha256, SHA256_DIGEST{});
	EXPECT_EQ(Info.m_QuicIdentityFingerprint, IdentitySha256);

	const CServerInfo2 WebPki = ParseServerInfoWithExperimental(R"({"proto":{"hostname":"example.com","quic":{"verify":"webpki"},"webtransport":{"verify":"webpki"}}})");
	EXPECT_EQ(WebPki.m_QuicTrust, EModernTransportTrust::WEBPKI);
	EXPECT_EQ(WebPki.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::WEBPKI);
	EXPECT_STREQ(WebPki.m_aModernHostname, "example.com");
	EXPECT_STREQ(WebPki.m_aWebTransportUrl, "");

	const CServerInfo2 MissingWebPkiHostname = ParseServerInfoWithExperimental(R"({"proto":{"quic":{"verify":"webpki"}}})");
	EXPECT_FALSE(MissingWebPkiHostname.m_QuicSharedPort);
	const CServerInfo2 InvalidWebTransportIdentity = ParseServerInfoWithExperimental(R"({"proto":{"webtransport":{"verify":"identity","sha256":"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"}}})");
	EXPECT_FALSE(InvalidWebTransportIdentity.m_WebTransport);
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

TEST(ServerInfo, QuicDirectLinkFingerprint)
{
	static constexpr const char *FINGERPRINT = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	SHA256_DIGEST Fingerprint;
	EXPECT_TRUE(ParseQuicDirectLinkFingerprint("ddnet+quic://example.com:8303#sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &Fingerprint));
	char aFingerprint[SHA256_MAXSTRSIZE];
	sha256_str(Fingerprint, aFingerprint, sizeof(aFingerprint));
	EXPECT_STREQ(aFingerprint, FINGERPRINT);
	EXPECT_TRUE(ParseQuicDirectLinkFingerprint("ddnet+quic://[::1]:8303#sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &Fingerprint));
	EXPECT_TRUE(ParseQuicDirectLinkFingerprint("tw-0.7+quic://example.com:8303#sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &Fingerprint));
	EXPECT_FALSE(ParseQuicDirectLinkFingerprint("example.com:8303", &Fingerprint));
	EXPECT_FALSE(ParseQuicDirectLinkFingerprint("ddnet+quic://example.com:8303", &Fingerprint));
	EXPECT_FALSE(ParseQuicDirectLinkFingerprint("ddnet+quic://example.com:8303/path#sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &Fingerprint));
	EXPECT_FALSE(ParseQuicDirectLinkFingerprint("ddnet+quic://user@example.com:8303#sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &Fingerprint));
	EXPECT_FALSE(ParseQuicDirectLinkFingerprint("ddnet+quic://example.com:8303#sha256=00", &Fingerprint));
}

TEST(ServerInfo, ModernTransportUrl)
{
	static constexpr const char *FINGERPRINT = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static constexpr const char *NEXT_FINGERPRINT = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	bool WebTransport;
	EModernTransportTrust Trust;
	SHA256_DIGEST Fingerprint;
	SHA256_DIGEST NextFingerprint;
	bool HasNextFingerprint;
	EXPECT_TRUE(ParseModernTransportUrl("ddnet+quic://example.com:8303", &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
	EXPECT_FALSE(WebTransport);
	EXPECT_EQ(Trust, EModernTransportTrust::TOFU);
	EXPECT_TRUE(ParseModernTransportUrl("ddnet+quic://example.com:8303#webpki", &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
	EXPECT_EQ(Trust, EModernTransportTrust::WEBPKI);
	char aUrl[256];
	str_format(aUrl, sizeof(aUrl), "ddnet+quic://example.com:8303#cert-sha256=%s,%s", FINGERPRINT, NEXT_FINGERPRINT);
	EXPECT_TRUE(ParseModernTransportUrl(aUrl, &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
	EXPECT_EQ(Trust, EModernTransportTrust::CERTIFICATE_HASH);
	EXPECT_TRUE(HasNextFingerprint);
	EXPECT_TRUE(ParseModernTransportUrl("ddnet+wt://example.com:8303", &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
	EXPECT_TRUE(WebTransport);
	EXPECT_EQ(Trust, EModernTransportTrust::WEBPKI);
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+wt://example.com:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+quic://example.com:8303/path", &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
	EXPECT_FALSE(ParseModernTransportUrl("ddnet+quic://example.com:8303#unknown", &WebTransport, &Trust, &Fingerprint, &NextFingerprint, &HasNextFingerprint));
}

TEST(ServerInfo, QuicLanExtra)
{
	static constexpr const char *FINGERPRINT = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	SHA256_DIGEST Fingerprint;
	ASSERT_EQ(sha256_from_str(&Fingerprint, FINGERPRINT), 0);
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = true;
	Extra.m_IdentityFingerprint = Fingerprint;
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);

	CServerInfo Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicPort, 8303);
	EXPECT_TRUE(Info.m_HasQuicIdentityFingerprint);
	EXPECT_EQ(Info.m_QuicIdentityFingerprint, Fingerprint);
	EXPECT_EQ(Info.m_QuicCapabilities, CServerInfo::QUIC_CAPABILITY_DATAGRAM | CServerInfo::QUIC_CAPABILITY_MAP_STREAM | CServerInfo::QUIC_CAPABILITY_RESUME | CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7);

	str_format(aExtraInfo, sizeof(aExtraInfo), "ddnet-transport-v1|quic|tls-certificate-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7", FINGERPRINT);
	Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	EXPECT_FALSE(Info.m_HasQuicIdentityFingerprint);
	EXPECT_EQ(Info.m_QuicCertificateSha256, Fingerprint);

	aExtraInfo[0] = 'x';
	EXPECT_TRUE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	str_format(aExtraInfo, sizeof(aExtraInfo), "ddnet-transport-v2|quic|identity-sha256=%s|capabilities=datagram,map-stream,resume-v2,game-protocol-7", FINGERPRINT);
	EXPECT_TRUE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	str_format(aExtraInfo, sizeof(aExtraInfo), "ddnet-transport-v2|quic|identity-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7", FINGERPRINT);
	EXPECT_TRUE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 0));
}

TEST(ServerInfo, QuicLanExtraWebTransport)
{
	static constexpr const char *FINGERPRINT = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static constexpr const char *CERTIFICATE = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
	static constexpr const char *NEXT_CERTIFICATE = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
	SHA256_DIGEST Fingerprint, Certificate, NextCertificate;
	ASSERT_EQ(sha256_from_str(&Fingerprint, FINGERPRINT), 0);
	ASSERT_EQ(sha256_from_str(&Certificate, CERTIFICATE), 0);
	ASSERT_EQ(sha256_from_str(&NextCertificate, NEXT_CERTIFICATE), 0);

	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = true;
	Extra.m_IdentityFingerprint = Fingerprint;
	Extra.m_WebTransport = true;
	Extra.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::HASH;
	Extra.m_WebTransportCertificateSha256 = Certificate;
	Extra.m_WebTransportNextCertificateSha256 = NextCertificate;
	Extra.m_HasWebTransportNextCertificateSha256 = true;
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);

	CServerInfo Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	EXPECT_TRUE(Info.m_HasQuicIdentityFingerprint);
	EXPECT_EQ(Info.m_QuicIdentityFingerprint, Fingerprint);
	EXPECT_TRUE(Info.m_WebTransport);
	EXPECT_EQ(Info.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::HASH);
	EXPECT_EQ(Info.m_WebTransportCertificateSha256, Certificate);
	EXPECT_TRUE(Info.m_HasWebTransportNextCertificateSha256);
	EXPECT_EQ(Info.m_WebTransportNextCertificateSha256, NextCertificate);
	// The WebTransport certificate must not be taken for the raw QUIC one.
	EXPECT_EQ(Info.m_QuicCertificateSha256, SHA256_DIGEST{});
	EXPECT_EQ(Info.m_QuicTrust, EModernTransportTrust::IDENTITY);

	Extra.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::WEBPKI;
	Extra.m_pHostname = "server.example.com";
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	EXPECT_TRUE(Info.m_WebTransport);
	EXPECT_EQ(Info.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::WEBPKI);
	EXPECT_EQ(Info.m_QuicTrust, EModernTransportTrust::IDENTITY);
	EXPECT_STREQ(Info.m_aModernHostname, "server.example.com");

	// A server that only speaks QUIC must still produce the string an older
	// client expects byte for byte.
	Extra.m_WebTransport = false;
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);
	char aExpected[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	str_format(aExpected, sizeof(aExpected), "ddnet-transport-v2|quic|identity-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7", FINGERPRINT);
	EXPECT_STREQ(aExtraInfo, aExpected);

	// Unknown segments are skipped instead of rejecting the whole string.
	str_append(aExtraInfo, "|future=whatever|webtransport=hash", sizeof(aExtraInfo));
	Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	EXPECT_TRUE(Info.m_WebTransport);
	EXPECT_EQ(Info.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::HASH);
}

TEST(ServerInfo, QuicLanExtraWebTransportOnly)
{
	static constexpr const char *CERTIFICATE = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
	SHA256_DIGEST Certificate;
	ASSERT_EQ(sha256_from_str(&Certificate, CERTIFICATE), 0);

	// `sv_webtransport` is on by default and `sv_quic` is not, so this is what a
	// LAN server answers unless it was told otherwise. It has no identity
	// binding, and it must still advertise the transport it does serve.
	CQuicServerInfoExtra Extra = {};
	Extra.m_RawQuic = false;
	Extra.m_WebTransport = true;
	Extra.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::HASH;
	Extra.m_WebTransportCertificateSha256 = Certificate;
	char aExtraInfo[QUIC_SERVERINFO_EXTRA_MAXSIZE];
	FormatQuicServerInfoExtra(aExtraInfo, sizeof(aExtraInfo), Extra);

	CServerInfo Info = {};
	EXPECT_FALSE(ParseQuicServerInfoExtra(&Info, aExtraInfo, 8303));
	EXPECT_FALSE(Info.m_RawQuic);
	EXPECT_FALSE(Info.m_HasQuicIdentityFingerprint);
	EXPECT_TRUE(Info.m_WebTransport);
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicPort, 8303);
	EXPECT_EQ(Info.m_WebTransportCertificateSha256, Certificate);

	// A client that predates the prefix reads nothing from it rather than a
	// server that is not there.
	EXPECT_TRUE(str_startswith(aExtraInfo, "ddnet-transport-v2|webtransport|capabilities=") != nullptr);
}

TEST(ServerInfo, PreserveWebTransportMetadataWithLanIdentity)
{
	CServerInfo PreviousInfo = {};
	PreviousInfo.m_WebTransport = true;
	PreviousInfo.m_WebTransportCertificateMode = CServerInfo::EWebTransportCertificateMode::HASH;
	str_copy(PreviousInfo.m_aWebTransportPath, "/ddnet");
	str_copy(PreviousInfo.m_aWebTransportUrl, "https://example.com:8303/ddnet");
	PreviousInfo.m_WebTransportCertificateSha256.data[0] = 1;
	PreviousInfo.m_WebTransportNextCertificateSha256.data[0] = 2;
	PreviousInfo.m_HasWebTransportNextCertificateSha256 = true;
	PreviousInfo.m_QuicCapabilities = CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7;

	CServerInfo LanInfo = {};
	LanInfo.m_QuicSharedPort = true;
	LanInfo.m_QuicIdentityFingerprint.data[0] = 3;
	LanInfo.m_HasQuicIdentityFingerprint = true;
	PreserveWebTransportMetadata(&LanInfo, PreviousInfo);

	EXPECT_TRUE(LanInfo.m_WebTransport);
	EXPECT_EQ(LanInfo.m_WebTransportCertificateMode, CServerInfo::EWebTransportCertificateMode::HASH);
	EXPECT_STREQ(LanInfo.m_aWebTransportPath, "/ddnet");
	EXPECT_STREQ(LanInfo.m_aWebTransportUrl, "https://example.com:8303/ddnet");
	EXPECT_EQ(LanInfo.m_WebTransportCertificateSha256, PreviousInfo.m_WebTransportCertificateSha256);
	EXPECT_EQ(LanInfo.m_WebTransportNextCertificateSha256, PreviousInfo.m_WebTransportNextCertificateSha256);
	EXPECT_TRUE(LanInfo.m_HasWebTransportNextCertificateSha256);
	EXPECT_EQ(LanInfo.m_QuicIdentityFingerprint.data[0], 3);
	EXPECT_TRUE(LanInfo.m_HasQuicIdentityFingerprint);
	EXPECT_TRUE(LanInfo.m_QuicCapabilities & CServerInfo::QUIC_CAPABILITY_GAME_PROTOCOL_7);
}
