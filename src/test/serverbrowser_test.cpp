#include "test.h"

#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

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
#include <string>
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

// The same entry: the same endpoints in the same order, the same pins and
// host name.
static void ExpectSameServer(const CServerInfo &Expected, const CServerInfo &Actual)
{
	ASSERT_EQ(Actual.m_NumAddresses, Expected.m_NumAddresses);
	for(int i = 0; i < Expected.m_NumAddresses; i++)
	{
		char aExpected[NETADDR_URL_MAXSTRSIZE];
		char aActual[NETADDR_URL_MAXSTRSIZE];
		net_addr_url_str(&Expected.m_aAddresses[i], aExpected, sizeof(aExpected), true);
		net_addr_url_str(&Actual.m_aAddresses[i], aActual, sizeof(aActual), true);
		EXPECT_STREQ(aActual, aExpected);
		EXPECT_TRUE(Actual.m_aAddresses[i] == Expected.m_aAddresses[i]) << aActual;
	}
	EXPECT_STREQ(Actual.m_Pin.m_aIdentity, Expected.m_Pin.m_aIdentity);
	EXPECT_STREQ(Actual.m_Pin.m_aWebTransport, Expected.m_Pin.m_aWebTransport);
	EXPECT_STREQ(Actual.m_aHostname, Expected.m_aHostname);
}

#define IDENTITY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define CERTIFICATES "cert-sha256=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789,0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

#define CAPABILITIES "|capabilities=datagram,map-stream,resume-v1,game-protocol-7"

// The transports a server describes in its info, in the text of its LAN extra
// info, become exactly the addresses a master would list for them on the host
// and port of its UDP addresses, read the same way.
TEST(ServerBrowser, HttpInfoTransportsAsListed)
{
	const char *pUdp = R"("tw-0.6+udp://127.0.0.1:8303","tw-0.7+udp://127.0.0.1:8303","tw-0.6+udp://[::1]:8303")";
	std::vector<CServerInfo> vInfo = ParseServers(pUdp, R"("experimental":{"transports":"quic|spki-sha256=)" IDENTITY CAPABILITIES R"(|webtransport=hash|wt-)" CERTIFICATES R"(|websocket=ws|future=whatever"},)");
	std::vector<CServerInfo> vListed = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","tw-0.7+udp://127.0.0.1:8303","tw-0.6+udp://[::1]:8303",)"
							R"("ddnet+quic://127.0.0.1:8303#spki-sha256=)" IDENTITY R"(","ddnet+wt://127.0.0.1:8303#)" CERTIFICATES R"(","ddnet+ws://127.0.0.1:8303",)"
							R"("tw-0.7+quic://127.0.0.1:8303#spki-sha256=)" IDENTITY R"(","tw-0.7+wt://127.0.0.1:8303#)" CERTIFICATES R"(",)"
							R"("ddnet+quic://[::1]:8303#spki-sha256=)" IDENTITY R"(","ddnet+wt://[::1]:8303#)" CERTIFICATES R"(","ddnet+ws://[::1]:8303")");
	ASSERT_EQ(vInfo.size(), 1u);
	ASSERT_EQ(vListed.size(), 1u);
	// Every endpoint stays, 0.7 over UDP as well.
	EXPECT_EQ(vListed.front().m_NumAddresses, 3 + 8);
	ExpectSameServer(vListed.front(), vInfo.front());
	EXPECT_STREQ(vInfo.front().m_Pin.m_aIdentity, IDENTITY);
	EXPECT_STREQ(vInfo.front().m_Pin.m_aWebTransport, CERTIFICATES);

	// With Web PKI, WebTransport is listed under the name the server gives,
	// which is looked up like any address the master lists by name, and wss
	// shows the key raw QUIC shows.
	vInfo = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303")", R"("experimental":{"transports":"quic|spki-sha256=)" IDENTITY CAPABILITIES R"(|webtransport=webpki|hostname=localhost|websocket=wss"},)");
	vListed = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8303#spki-sha256=)" IDENTITY R"(","ddnet+wt://localhost:8303#webpki","ddnet+wss://127.0.0.1:8303#spki-sha256=)" IDENTITY R"(")");
	ASSERT_EQ(vInfo.size(), 1u);
	ASSERT_EQ(vListed.size(), 1u);
	ExpectSameServer(vListed.front(), vInfo.front());
	EXPECT_STREQ(vInfo.front().m_aHostname, "localhost");
	EXPECT_STREQ(vInfo.front().m_Pin.m_aWebTransport, "webpki");

	// Addresses the master lists win over the info.
	vInfo = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8306#spki-sha256=)" IDENTITY R"(")", R"("experimental":{"transports":"quic|spki-sha256=)" IDENTITY CAPABILITIES R"(|websocket=ws"},)");
	ASSERT_EQ(vInfo.size(), 1u);
	ASSERT_EQ(vInfo.front().m_NumAddresses, 2);
	EXPECT_EQ(vInfo.front().m_aAddresses[1].port, 8306);

	// Text this client does not understand adds nothing, and neither does a list.
	for(const char *pTransports : {R"("quic|spki-sha256=0123456789abcdef")", R"("quic|spki-sha256=)" IDENTITY R"(|capabilities=datagram")", R"(["ddnet+quic://connecting-address.invalid:8303"])"})
	{
		char aInfo[256];
		str_format(aInfo, sizeof(aInfo), R"("experimental":{"transports":%s},)", pTransports);
		vInfo = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303")", aInfo);
		ASSERT_EQ(vInfo.size(), 1u);
		EXPECT_EQ(vInfo.front().m_NumAddresses, 1) << pTransports;
	}
}

// What the formatter writes for an endpoint, the list reads back as it.
TEST(ServerBrowser, HttpAddressRoundTrip)
{
	std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","tw-0.7+udp://[::1]:8303","ddnet+quic://127.0.0.1:8303#spki-sha256=)" IDENTITY R"(",)"
							 R"("tw-0.7+quic://[::1]:8303#spki-sha256=)" IDENTITY R"(","ddnet+wt://127.0.0.1:8303#)" CERTIFICATES R"(","ddnet+wss://[::1]:8303#spki-sha256=)" IDENTITY R"(","ddnet+ws://127.0.0.1:8303")");
	ASSERT_EQ(vServers.size(), 1u);
	const CServerInfo &Info = vServers.front();
	ASSERT_EQ(Info.m_NumAddresses, 7);
	std::string Addresses;
	for(int i = 0; i < Info.m_NumAddresses; i++)
	{
		char aUrl[CServerPin::URL_MAXSTRSIZE];
		Info.m_Pin.AddressUrl(Info.m_aAddresses[i], nullptr, aUrl, sizeof(aUrl));
		// What the master lists always has a scheme.
		NETADDR Plain;
		Addresses += Addresses.empty() ? "" : ",";
		Addresses += std::string("\"") + (net_addr_from_str(&Plain, aUrl) == 0 ? "tw-0.6+udp://" : "") + aUrl + "\"";
	}
	const std::vector<CServerInfo> vRead = ParseServers(Addresses.c_str());
	ASSERT_EQ(vRead.size(), 1u);
	ExpectSameServer(Info, vRead.front());

	// By name as well.
	char aNamed[CServerPin::NAMED_URL_MAXSTRSIZE];
	vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","ddnet+wt://localhost:8303#webpki")");
	ASSERT_EQ(vServers.size(), 1u);
	ASSERT_EQ(vServers.front().m_NumAddresses, 2);
	vServers.front().m_Pin.AddressUrl(vServers.front().m_aAddresses[1], vServers.front().m_aHostname, aNamed, sizeof(aNamed));
	EXPECT_STREQ(aNamed, "ddnet+wt://localhost:8303#webpki");
}

#undef IDENTITY
#undef CERTIFICATES

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
