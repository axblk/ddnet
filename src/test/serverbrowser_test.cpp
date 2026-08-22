#include "test.h"

#include <base/net.h>

#include <engine/client/serverbrowser.h>
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

TEST(ServerBrowser, CompatibleAddressesIncludeModernTransports)
{
	CServerInfo Info = {};
	ASSERT_FALSE(net_addr_from_url(&Info.m_aAddresses[0], "tw-0.6+udp://127.0.0.1:8303", nullptr, 0));
	Info.m_NumAddresses = 1;
	EXPECT_TRUE(ServerBrowserHasCompatibleAddress(Info, NETTYPE_IPV4 | NETTYPE_IPV6, false, false, false, false));
	EXPECT_FALSE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, false, false));

	Info.m_aAddresses[0].type = NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_TLS;
	EXPECT_TRUE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, false, false));
	EXPECT_FALSE(ServerBrowserHasCompatibleAddress(Info, NETTYPE_IPV4 | NETTYPE_IPV6, false, false, false, false));

	ASSERT_FALSE(net_addr_from_url(&Info.m_aAddresses[0], "tw-0.6+udp://127.0.0.1:8303", nullptr, 0));
	ASSERT_FALSE(net_addr_from_url(&Info.m_aQuicAddresses[0], "tw-0.6+udp://127.0.0.1:8303", nullptr, 0));
	Info.m_NumQuicAddresses = 1;
	Info.m_QuicPort = 8303;
	EXPECT_TRUE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, true, false));
	EXPECT_FALSE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, false, false));

	Info.m_NumQuicAddresses = 0;
	Info.m_WebTransport = true;
	ASSERT_FALSE(net_addr_from_url(&Info.m_aWebTransportAddresses[0], "tw-0.7+udp://127.0.0.1:8303", nullptr, 0));
	Info.m_NumWebTransportAddresses = 1;
	EXPECT_TRUE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, false, true));

	// Without an address the master server verified there is nothing to
	// connect to, whether or not the master challenges modern transports.
	Info.m_NumWebTransportAddresses = 0;
	Info.m_MasterChallengesModernTransports = false;
	EXPECT_FALSE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, false, true));
	Info.m_MasterChallengesModernTransports = true;
	EXPECT_FALSE(ServerBrowserHasCompatibleAddress(Info, 0, true, true, false, true));
}

TEST(ServerBrowser, HttpQuicAddressIsNotLegacyAddress)
{
	static constexpr const char *pJsonText = R"({"modern_transport_challenge":true,"servers":[{"addresses":["tw-0.6+udp://[::1]:8303","tw-0.7+udp://[::1]:8303","ddnet+quic://[::1]:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+quic://[::1]:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tw-0.7+quic://[::1]:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+wt://[::1]:8303#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","tw-0.7+wt://[::1]:8303#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DDRace","name":"test","map":{"name":"Tutorial"},"version":"test","clients":[]}}]})";
	json_value *pJson = JsonParse(pJsonText, str_length(pJsonText));
	ASSERT_NE(pJson, nullptr);

	std::vector<CServerInfo> vServers;
	EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
	json_value_free(pJson);
	ASSERT_EQ(vServers.size(), 1u);

	const CServerInfo &Info = vServers.front();
	EXPECT_TRUE(Info.m_MasterChallengesModernTransports);
	ASSERT_EQ(Info.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_aAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_aAddresses[0].port, 8303);
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicPort, 8303);
	ASSERT_EQ(Info.m_NumQuicAddresses, 2);
	EXPECT_EQ(Info.m_aQuicAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_aQuicAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
	ASSERT_EQ(Info.m_NumWebTransportAddresses, 2);
	EXPECT_EQ(Info.m_aWebTransportAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_aWebTransportAddresses[1].type, NETTYPE_IPV6 | NETTYPE_TW7);
}

TEST(ServerBrowser, HttpModernSevenAddressDoesNotBecomeLegacySix)
{
	static constexpr const char *pJsonText = R"({"modern_transport_challenge":true,"servers":[{"addresses":["tw-0.7+quic://127.0.0.1:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DM","name":"seven","map":{"name":"dm1"},"version":"0.7","clients":[]}}]})";
	json_value *pJson = JsonParse(pJsonText, str_length(pJsonText));
	ASSERT_NE(pJson, nullptr);

	std::vector<CServerInfo> vServers;
	EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
	json_value_free(pJson);
	ASSERT_EQ(vServers.size(), 1u);

	const CServerInfo &Info = vServers.front();
	EXPECT_EQ(Info.m_NumAddresses, 0);
	ASSERT_EQ(Info.m_NumQuicAddresses, 1);
	EXPECT_EQ(Info.m_aQuicAddresses[0].type, NETTYPE_IPV4 | NETTYPE_TW7);
	EXPECT_STREQ(Info.m_aAddress, "tw-0.7+quic://127.0.0.1:8303");
}

TEST(ServerBrowser, HttpModernAddressCarriesOwnPort)
{
	static constexpr const char *pJsonText = R"({"modern_transport_challenge":true,"servers":[{"addresses":["tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8304#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","ddnet+wt://127.0.0.1:8304#cert-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DM","name":"modern port","map":{"name":"dm1"},"version":"test","clients":[]}}]})";
	json_value *pJson = JsonParse(pJsonText, str_length(pJsonText));
	ASSERT_NE(pJson, nullptr);

	std::vector<CServerInfo> vServers;
	EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
	json_value_free(pJson);
	ASSERT_EQ(vServers.size(), 1u);
	EXPECT_EQ(vServers.front().m_NumQuicAddresses, 1);
	EXPECT_EQ(vServers.front().m_NumWebTransportAddresses, 1);
	EXPECT_EQ(vServers.front().m_QuicPort, 8304);
	EXPECT_TRUE(vServers.front().m_QuicSharedPort);
	EXPECT_TRUE(vServers.front().m_WebTransport);
}

TEST(ServerBrowser, HttpOldMasterIgnoresUnchallengedModernPrefixes)
{
	static constexpr const char *pJsonText = R"({"servers":[{"addresses":["tw-0.6+udp://127.0.0.1:8303","ddnet+quic://127.0.0.1:8303"],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DM","name":"old master","map":{"name":"dm1"},"version":"test","clients":[],"experimental":{"proto":{"quic":{"verify":"identity","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}}}}]})";
	json_value *pJson = JsonParse(pJsonText, str_length(pJsonText));
	ASSERT_NE(pJson, nullptr);

	std::vector<CServerInfo> vServers;
	EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
	json_value_free(pJson);
	ASSERT_EQ(vServers.size(), 1u);

	const CServerInfo &Info = vServers.front();
	EXPECT_FALSE(Info.m_MasterChallengesModernTransports);
	EXPECT_EQ(Info.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_NumQuicAddresses, 0);
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicPort, 8303);
}

TEST(ServerBrowser, HttpDomainQuicAddress)
{
	static constexpr const char *pJsonText = R"({"modern_transport_challenge":true,"servers":[{"addresses":["ddnet+quic://localhost:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DM","name":"domain","map":{"name":"dm1"},"version":"test","clients":[],"experimental":{"proto":{"hostname":"localhost","quic":{"verify":"identity","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}}}}]})";
	json_value *pJson = JsonParse(pJsonText, str_length(pJsonText));
	ASSERT_NE(pJson, nullptr);

	std::vector<CServerInfo> vServers;
	EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
	json_value_free(pJson);
	ASSERT_EQ(vServers.size(), 1u);
	ASSERT_EQ(vServers.front().m_NumQuicAddresses, 1);
	EXPECT_EQ(vServers.front().m_aQuicAddresses[0].port, 8303);
	EXPECT_STREQ(vServers.front().m_aModernHostname, "localhost");
}

TEST(ServerBrowser, PingCache)
{
	CTestInfo Info;

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
