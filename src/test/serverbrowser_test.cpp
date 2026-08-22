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

TEST(ServerBrowser, HttpQuicAddressIsNotLegacyAddress)
{
	static constexpr const char *pJsonText = R"({"servers":[{"addresses":["tw-0.6+udp://[::1]:8303","tw-0.7+udp://[::1]:8303","ddnet+quic://[::1]:8304"],"info":{"max_clients":16,"max_players":16,"client_score_kind":"points","passworded":false,"game_type":"DDRace","name":"test","map":{"name":"Tutorial"},"version":"test","clients":[],"transport":{"udp_port":8303,"tls_certificate_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","quic":true}}}]})";
	json_value *pJson = JsonParse(pJsonText, str_length(pJsonText));
	ASSERT_NE(pJson, nullptr);

	std::vector<CServerInfo> vServers;
	EXPECT_FALSE(ServerBrowserHttpParse(pJson, &vServers));
	json_value_free(pJson);
	ASSERT_EQ(vServers.size(), 1u);

	const CServerInfo &Info = vServers.front();
	ASSERT_EQ(Info.m_NumAddresses, 1);
	EXPECT_EQ(Info.m_aAddresses[0].type, NETTYPE_IPV6);
	EXPECT_EQ(Info.m_aAddresses[0].port, 8303);
	EXPECT_TRUE(Info.m_QuicSharedPort);
	EXPECT_EQ(Info.m_QuicPort, 8303);
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
