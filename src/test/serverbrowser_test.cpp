#include "test.h"

#include <base/net.h>

#include <engine/client/serverbrowser_http.h>
#include <engine/client/serverbrowser_ping_cache.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

static std::vector<CServerInfo> ParseServers(const char *pAddresses, const char *pInfoExtra = "")
{
	char aJson[4096];
	str_format(aJson, sizeof(aJson), R"({"servers":[{"addresses":[%s],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DM","name":"test","map":{"name":"dm1"},"version":"test",%s"clients":[]}}]})", pAddresses, pInfoExtra);
	json_value *pJson = JsonParse(aJson, str_length(aJson));
	EXPECT_NE(pJson, nullptr);
	std::vector<CServerInfo> vServers;
	if(pJson != nullptr)
	{
		EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
		json_value_free(pJson);
	}
	return vServers;
}

TEST(ServerBrowser, HttpModernAddresses)
{
	const std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.6+udp://[::1]:8303","tw-0.7+udp://[::1]:8303","ddnet+quic://[::1]:8303#spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+quic://[::1]:8303#spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tw-0.7+quic://[::1]:8303#spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+wt://[::1]:8304#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tw-0.7+wt://[::1]:8304#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")");
	ASSERT_EQ(vServers.size(), 1u);
	const CServerInfo &Info = vServers.front();
	// All endpoints are kept, 0.7 is only dropped when one is picked.
	ASSERT_EQ(Info.m_NumAddresses, 2);
	EXPECT_EQ(Info.m_aAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_aAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
	ASSERT_EQ(Info.m_Quic.m_NumAddresses, 2);
	EXPECT_EQ(Info.m_Quic.m_aAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_Quic.m_aAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
	EXPECT_EQ(Info.m_Quic.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	ASSERT_EQ(Info.m_WebTransport.m_NumAddresses, 2);
	EXPECT_EQ(Info.m_WebTransport.m_aAddresses[0].port, 8304);
	EXPECT_EQ(Info.m_WebTransport.m_aAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
}

TEST(ServerBrowser, HttpModernOnlyServer)
{
	const std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.7+quic://127.0.0.1:8303#spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")");
	ASSERT_EQ(vServers.size(), 1u);
	EXPECT_EQ(vServers.front().m_NumAddresses, 0);
	ASSERT_EQ(vServers.front().m_Quic.m_NumAddresses, 1);
	EXPECT_EQ(vServers.front().m_Quic.m_aAddresses[0].type, NETTYPE_IPV4 | NETTYPE_TW7);
}

TEST(ServerBrowser, HttpModernAddressNeedsPin)
{
	const std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8303","ddnet+wt://127.0.0.1:8303#spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")");
	ASSERT_EQ(vServers.size(), 1u);
	EXPECT_EQ(vServers.front().m_NumAddresses, 1);
	EXPECT_EQ(vServers.front().m_Quic.m_NumAddresses, 0);
	EXPECT_EQ(vServers.front().m_WebTransport.m_NumAddresses, 0);
}

TEST(ServerBrowser, HttpModernAddressHostname)
{
	const std::vector<CServerInfo> vServers = ParseServers(R"("ddnet+quic://localhost:8303#webpki")");
	ASSERT_EQ(vServers.size(), 1u);
	ASSERT_EQ(vServers.front().m_Quic.m_NumAddresses, 1);
	EXPECT_EQ(vServers.front().m_Quic.m_aAddresses[0].port, 8303);
	EXPECT_EQ(vServers.front().m_Quic.m_Pin.m_Trust, EModernTransportTrust::WEBPKI);
	EXPECT_STREQ(vServers.front().m_Quic.m_aHostname, "localhost");
}

static void ExpectSameTransport(const CModernTransportInfo &Transport, const CModernTransportInfo &Expected)
{
	ASSERT_EQ(Transport.m_NumAddresses, Expected.m_NumAddresses);
	for(int i = 0; i < Transport.m_NumAddresses; i++)
		EXPECT_EQ(Transport.m_aAddresses[i], Expected.m_aAddresses[i]) << i;
	EXPECT_EQ(Transport.m_Pin.m_Trust, Expected.m_Pin.m_Trust);
	EXPECT_EQ(Transport.m_Pin.m_Fingerprint, Expected.m_Pin.m_Fingerprint);
	EXPECT_EQ(Transport.m_Pin.m_HasNextFingerprint, Expected.m_Pin.m_HasNextFingerprint);
	EXPECT_EQ(Transport.m_Pin.m_NextFingerprint, Expected.m_Pin.m_NextFingerprint);
	EXPECT_STREQ(Transport.m_aHostname, Expected.m_aHostname);
}

// A server that describes its transports in its info is listed as if the
// master listed their addresses.
static void ExpectInfoTransportsListedLike(const char *pTransports, const char *pListed)
{
	char aInfo[512];
	str_format(aInfo, sizeof(aInfo), R"("experimental":{"transports":"%s"},)", pTransports);
	const char *pUdp = R"("tw-0.6+udp://127.0.0.1:8303","tw-0.7+udp://127.0.0.1:8303","tw-0.6+udp://[::1]:8303")";
	const std::vector<CServerInfo> vDescribed = ParseServers(pUdp, aInfo);
	char aAddresses[2048];
	str_format(aAddresses, sizeof(aAddresses), "%s,%s", pUdp, pListed);
	const std::vector<CServerInfo> vListed = ParseServers(aAddresses);
	ASSERT_EQ(vDescribed.size(), 1u);
	ASSERT_EQ(vListed.size(), 1u);
	const CServerInfo &Described = vDescribed.front();
	const CServerInfo &Listed = vListed.front();
	ASSERT_EQ(Described.m_NumAddresses, 3);
	ASSERT_EQ(Described.m_NumAddresses, Listed.m_NumAddresses);
	for(int i = 0; i < Described.m_NumAddresses; i++)
		EXPECT_EQ(Described.m_aAddresses[i], Listed.m_aAddresses[i]);
	ExpectSameTransport(Described.m_Quic, Listed.m_Quic);
	ExpectSameTransport(Described.m_WebTransport, Listed.m_WebTransport);
}

TEST(ServerBrowser, HttpInfoTransports)
{
	const char *pSpki = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const char *pCertificate = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	const char *pNextCertificate = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
	char aTransports[512];
	char aListed[2048];

	// QUIC and WebTransport with certificate hashes, for both game protocols
	// on the host and port of each UDP address.
	str_format(aTransports, sizeof(aTransports), "quic|spki-sha256=%s|capabilities=datagram,map-stream,resume-v1,game-protocol-7|webtransport=hash|wt-cert-sha256=%s,%s", pSpki, pCertificate, pNextCertificate);
	str_format(aListed, sizeof(aListed),
		R"("ddnet+quic://127.0.0.1:8303#spki-sha256=%s","ddnet+wt://127.0.0.1:8303#cert-sha256=%s,%s",)"
		R"("tw-0.7+quic://127.0.0.1:8303#spki-sha256=%s","tw-0.7+wt://127.0.0.1:8303#cert-sha256=%s,%s",)"
		R"("ddnet+quic://[::1]:8303#spki-sha256=%s","ddnet+wt://[::1]:8303#cert-sha256=%s,%s")",
		pSpki, pCertificate, pNextCertificate, pSpki, pCertificate, pNextCertificate, pSpki, pCertificate, pNextCertificate);
	ExpectInfoTransportsListedLike(aTransports, aListed);

	// WebTransport only, checked by Web PKI for the name the server gives.
	ExpectInfoTransportsListedLike("webtransport|capabilities=datagram,map-stream,resume-v1,game-protocol-7|webtransport=webpki|hostname=localhost",
		R"("ddnet+wt://localhost:8303","tw-0.7+wt://localhost:8303","ddnet+wt://localhost:8303")");

	const char *pInfo = R"("experimental":{"transports":"quic|spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef|capabilities=datagram,map-stream,resume-v1,game-protocol-7|webtransport=hash|wt-cert-sha256=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},)";
	std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","tw-0.7+udp://127.0.0.1:8303","tw-0.6+udp://[::1]:8303")", pInfo);
	ASSERT_EQ(vServers.size(), 1u);
	ASSERT_EQ(vServers.front().m_Quic.m_NumAddresses, 3);
	EXPECT_EQ(vServers.front().m_Quic.m_aAddresses[1].type, NETTYPE_IPV4 | NETTYPE_TW7);
	EXPECT_EQ(vServers.front().m_Quic.m_Pin.m_Trust, EModernTransportTrust::SPKI_HASH);
	EXPECT_EQ(vServers.front().m_WebTransport.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);

	// Addresses the master lists win over the info.
	vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8306#spki-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")", pInfo);
	ASSERT_EQ(vServers.size(), 1u);
	ASSERT_EQ(vServers.front().m_Quic.m_NumAddresses, 1);
	EXPECT_EQ(vServers.front().m_Quic.m_aAddresses[0].port, 8306);
	EXPECT_EQ(vServers.front().m_WebTransport.m_NumAddresses, 0);

	// A text this client does not understand adds nothing.
	vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303")", R"("experimental":{"transports":"quic|identity-sha256=0123456789abcdef"},)");
	ASSERT_EQ(vServers.size(), 1u);
	EXPECT_EQ(vServers.front().m_Quic.m_NumAddresses, 0);
	EXPECT_EQ(vServers.front().m_WebTransport.m_NumAddresses, 0);
}

TEST(ServerBrowser, PingCache)
{
	CTestInfo Info;
	Info.m_DeleteTestStorageFilesOnSuccess = true;

	auto pConsole = CreateConsole(CFGFLAG_CLIENT);
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating test storage";
	auto pPingCache = std::unique_ptr<IServerBrowserPingCache>(CreateServerBrowserPingCache(pConsole.get(), pStorage.get()));

	NETADDR Localhost4, Localhost6, OtherLocalhost4, OtherLocalhost6;
	ASSERT_FALSE(net_addr_from_str(&Localhost4, "127.0.0.1:8303"));
	ASSERT_FALSE(net_addr_from_str(&Localhost6, "[::1]:8304"));
	ASSERT_FALSE(net_addr_from_str(&OtherLocalhost4, "127.0.0.1:8305"));
	ASSERT_FALSE(net_addr_from_str(&OtherLocalhost6, "[::1]:8306"));
	EXPECT_LT(net_addr_comp(&Localhost4, &Localhost6), 0);
	NETADDR aLocalhostBoth[2] = {Localhost4, Localhost6};

	EXPECT_EQ(pPingCache->NumEntries(), 0);
	EXPECT_EQ(pPingCache->GetPing(&Localhost4, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(&Localhost6, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(aLocalhostBoth, 2), -1);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost4, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost6, 1), -1);

	pPingCache->Load();

	EXPECT_EQ(pPingCache->NumEntries(), 0);
	EXPECT_EQ(pPingCache->GetPing(&Localhost4, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(&Localhost6, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(aLocalhostBoth, 2), -1);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost4, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost6, 1), -1);

	// Newer pings overwrite older.
	pPingCache->CachePing(Localhost4, 123);
	pPingCache->CachePing(Localhost4, 234);
	pPingCache->CachePing(Localhost4, 345);
	pPingCache->CachePing(Localhost4, 456);
	pPingCache->CachePing(Localhost4, 567);
	pPingCache->CachePing(Localhost4, 678);
	pPingCache->CachePing(Localhost4, 789);
	pPingCache->CachePing(Localhost4, 890);
	pPingCache->CachePing(Localhost4, 901);
	pPingCache->CachePing(Localhost4, 135);
	pPingCache->CachePing(Localhost4, 246);
	pPingCache->CachePing(Localhost4, 357);
	pPingCache->CachePing(Localhost4, 468);
	pPingCache->CachePing(Localhost4, 579);
	pPingCache->CachePing(Localhost4, 680);
	pPingCache->CachePing(Localhost4, 791);
	pPingCache->CachePing(Localhost4, 802);
	pPingCache->CachePing(Localhost4, 913);

	EXPECT_EQ(pPingCache->NumEntries(), 1);
	EXPECT_EQ(pPingCache->GetPing(&Localhost4, 1), 913);
	EXPECT_EQ(pPingCache->GetPing(&Localhost6, 1), -1);
	EXPECT_EQ(pPingCache->GetPing(aLocalhostBoth, 2), 913);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost4, 1), 913);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost6, 1), -1);

	pPingCache->CachePing(Localhost4, 234);
	pPingCache->CachePing(Localhost6, 345);
	EXPECT_EQ(pPingCache->NumEntries(), 2);
	EXPECT_EQ(pPingCache->GetPing(&Localhost4, 1), 234);
	EXPECT_EQ(pPingCache->GetPing(&Localhost6, 1), 345);
	EXPECT_EQ(pPingCache->GetPing(aLocalhostBoth, 2), 234);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost4, 1), 234);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost6, 1), 345);

	// Port doesn't matter for overwriting.
	pPingCache->CachePing(Localhost4, 1337);
	EXPECT_EQ(pPingCache->NumEntries(), 2);
	EXPECT_EQ(pPingCache->GetPing(&Localhost4, 1), 1337);
	EXPECT_EQ(pPingCache->GetPing(&Localhost6, 1), 345);
	EXPECT_EQ(pPingCache->GetPing(aLocalhostBoth, 2), 345);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost4, 1), 1337);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost6, 1), 345);

	pPingCache.reset(CreateServerBrowserPingCache(pConsole.get(), pStorage.get()));

	// Persistence.
	pPingCache->Load();
	EXPECT_EQ(pPingCache->NumEntries(), 2);
	EXPECT_EQ(pPingCache->GetPing(&Localhost4, 1), 1337);
	EXPECT_EQ(pPingCache->GetPing(&Localhost6, 1), 345);
	EXPECT_EQ(pPingCache->GetPing(aLocalhostBoth, 2), 345);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost4, 1), 1337);
	EXPECT_EQ(pPingCache->GetPing(&OtherLocalhost6, 1), 345);
}
