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

static std::vector<CServerInfo> ParseServers(const char *pAddresses)
{
	char aJson[2048];
	str_format(aJson, sizeof(aJson), R"({"servers":[{"addresses":[%s],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DM","name":"test","map":{"name":"dm1"},"version":"test","clients":[]}}]})", pAddresses);
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
	const std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.6+udp://[::1]:8303","tw-0.7+udp://[::1]:8303","ddnet+quic://[::1]:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+quic://[::1]:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tw-0.7+quic://[::1]:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+wt://[::1]:8304#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tw-0.7+wt://[::1]:8304#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")");
	ASSERT_EQ(vServers.size(), 1u);
	const CServerInfo &Info = vServers.front();
	ASSERT_EQ(Info.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_aAddresses[0].type, NETTYPE_IPV6);
	ASSERT_EQ(Info.m_Quic.m_NumAddresses, 2);
	EXPECT_EQ(Info.m_Quic.m_aAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_Quic.m_aAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
	EXPECT_EQ(Info.m_Quic.m_Pin.m_Trust, EModernTransportTrust::IDENTITY);
	ASSERT_EQ(Info.m_WebTransport.m_NumAddresses, 2);
	EXPECT_EQ(Info.m_WebTransport.m_aAddresses[0].port, 8304);
	EXPECT_EQ(Info.m_WebTransport.m_aAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
	EXPECT_EQ(Info.m_WebTransport.m_Pin.m_Trust, EModernTransportTrust::CERTIFICATE_HASH);
}

TEST(ServerBrowser, HttpModernOnlyServer)
{
	const std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.7+quic://127.0.0.1:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")");
	ASSERT_EQ(vServers.size(), 1u);
	EXPECT_EQ(vServers.front().m_NumAddresses, 0);
	ASSERT_EQ(vServers.front().m_Quic.m_NumAddresses, 1);
	EXPECT_EQ(vServers.front().m_Quic.m_aAddresses[0].type, NETTYPE_IPV4 | NETTYPE_TW7);
}

TEST(ServerBrowser, HttpModernAddressNeedsPin)
{
	const std::vector<CServerInfo> vServers = ParseServers(R"("tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8303","ddnet+wt://127.0.0.1:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")");
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
