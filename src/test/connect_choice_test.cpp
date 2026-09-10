#include <base/net.h>
#include <base/str.h>

#include <engine/serverbrowser.h>
#include <engine/shared/connect_choice.h>

#include <gtest/gtest.h>

static const char IDENTITY[] = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";

// Adds an address to a listed server, and what the master lists in its
// fragment.
static void AddAddress(CServerInfo &Info, const char *pUrl)
{
	const char *pFragment = str_find(pUrl, "#");
	char aAddress[NETADDR_URL_MAXSTRSIZE];
	str_truncate(aAddress, sizeof(aAddress), pUrl, pFragment != nullptr ? pFragment - pUrl : str_length(pUrl));
	NETADDR &Addr = Info.m_aAddresses[Info.m_NumAddresses++];
	ASSERT_EQ(net_addr_from_url(&Addr, aAddress, nullptr, 0), 0);
	if(pFragment == nullptr)
		return;
	if((Addr.type & NETTYPE_WEBTRANSPORT) != 0)
		str_copy(Info.m_aWebTransportFragment, pFragment + 1);
	else if(const char *pIdentity = str_startswith(pFragment + 1, "identity-sha256="))
		str_copy(Info.m_aIdentity, pIdentity);
}

class ConnectChoice : public ::testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CServerInfo m_Info{};

	void SetUp() override
	{
		char aQuic[256];
		str_format(aQuic, sizeof(aQuic), "ddnet+quic://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
		AddAddress(m_Info, "tw-0.6+udp://127.0.0.1:8303");
		AddAddress(m_Info, "tw-0.6+udp://[::1]:8303");
		AddAddress(m_Info, aQuic);
		AddAddress(m_Info, "ddnet+wt://127.0.0.1:8303#webpki");
		str_copy(m_Info.m_aHostname, "ger10.ddnet.org");
	}
};

TEST_F(ConnectChoice, ListedServer)
{
	const CConnectChoices Choices(&m_Info, nullptr);
#if defined(CONF_NETWORKING_QUIC)
	ASSERT_EQ(Choices.m_NumProtocols, 3);
	EXPECT_EQ(Choices.m_aProtocols[0], EConnectProtocol::QUIC);
	EXPECT_EQ(Choices.m_aProtocols[1], EConnectProtocol::WEBTRANSPORT);
	EXPECT_EQ(Choices.m_aProtocols[2], EConnectProtocol::LEGACY);
	EXPECT_EQ(Choices.ProtocolIndex((int)EConnectProtocol::LEGACY), 2);
#else
	ASSERT_EQ(Choices.m_NumProtocols, 1);
	EXPECT_EQ(Choices.m_aProtocols[0], EConnectProtocol::LEGACY);
#endif
	// A pick the server has nothing for falls back to the best.
	EXPECT_EQ(Choices.ProtocolIndex((int)EConnectProtocol::WEBSOCKET), 0);
	ASSERT_EQ(Choices.m_NumFamilies, 2);
	EXPECT_EQ(Choices.m_aFamilies[0], EConnectAddressFamily::IPV6);
	EXPECT_EQ(Choices.m_aFamilies[1], EConnectAddressFamily::IPV4);
	EXPECT_EQ(Choices.FamilyIndex((int)EConnectAddressFamily::IPV4), 1);
}

TEST_F(ConnectChoice, LinkAndTypedAddress)
{
	const CConnectChoices Link(&m_Info, "ddnet+wt://127.0.0.1:8303#webpki");
	ASSERT_EQ(Link.m_NumProtocols, 1);
	EXPECT_EQ(Link.m_aProtocols[0], EConnectProtocol::WEBTRANSPORT);

	const CConnectChoices Typed(nullptr, "127.0.0.1:8303");
	ASSERT_EQ(Typed.m_NumProtocols, 1);
	EXPECT_EQ(Typed.m_aProtocols[0], EConnectProtocol::LEGACY);
	ASSERT_EQ(Typed.m_NumFamilies, 1);
	EXPECT_EQ(Typed.m_aFamilies[0], EConnectAddressFamily::IPV4);

	// A name is left to the resolver.
	const CConnectChoices Name(nullptr, "ger10.ddnet.org:8303");
	ASSERT_EQ(Name.m_NumFamilies, 1);
	EXPECT_EQ(Name.m_aFamilies[0], EConnectAddressFamily::IPV6);
}

TEST_F(ConnectChoice, ConnectAddress)
{
	char aAddress[512];
#if defined(CONF_NETWORKING_QUIC)
	ASSERT_TRUE(ConnectAddressFor(m_Info, (int)EConnectProtocol::QUIC, (int)EConnectAddressFamily::IPV6, aAddress, sizeof(aAddress)));
	char aExpected[256];
	str_format(aExpected, sizeof(aExpected), "ddnet+quic://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
	EXPECT_STREQ(aAddress, aExpected);

	// The certificate is signed for the name, so the name is connected by.
	ASSERT_TRUE(ConnectAddressFor(m_Info, (int)EConnectProtocol::WEBTRANSPORT, (int)EConnectAddressFamily::IPV4, aAddress, sizeof(aAddress)));
	EXPECT_STREQ(aAddress, "ddnet+wt://ger10.ddnet.org:8303#webpki");
#endif

	ASSERT_TRUE(ConnectAddressFor(m_Info, (int)EConnectProtocol::LEGACY, (int)EConnectAddressFamily::IPV6, aAddress, sizeof(aAddress)));
	EXPECT_STREQ(aAddress, "[::1]:8303");
	ASSERT_TRUE(ConnectAddressFor(m_Info, (int)EConnectProtocol::LEGACY, (int)EConnectAddressFamily::IPV4, aAddress, sizeof(aAddress)));
	EXPECT_STREQ(aAddress, "127.0.0.1:8303");

	CServerInfo Empty{};
	str_copy(aAddress, "unchanged");
	EXPECT_FALSE(ConnectAddressFor(Empty, (int)EConnectProtocol::LEGACY, (int)EConnectAddressFamily::IPV4, aAddress, sizeof(aAddress)));
	EXPECT_STREQ(aAddress, "unchanged");
}

TEST_F(ConnectChoice, ServerHasAddress)
{
	EXPECT_TRUE(ServerHasAddress(m_Info, "127.0.0.1:8303"));
	EXPECT_TRUE(ServerHasAddress(m_Info, "ddnet+wt://127.0.0.1:8303"));
	EXPECT_TRUE(ServerHasAddress(m_Info, "ddnet+wt://ger10.ddnet.org:8303#webpki"));
	EXPECT_FALSE(ServerHasAddress(m_Info, "ddnet+wt://ger10.ddnet.org:8304"));
	EXPECT_FALSE(ServerHasAddress(m_Info, "ddnet+wt://ger11.ddnet.org:8303"));
	EXPECT_FALSE(ServerHasAddress(m_Info, "127.0.0.2:8303"));
}
