#include <base/net.h>
#include <base/str.h>

#include <engine/serverbrowser.h>
#include <engine/shared/connect_choice.h>

#include <gtest/gtest.h>

#include <string>

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
	// No pick, or one the server has nothing for, is the best.
	EXPECT_EQ(Choices.ProtocolIndex(-1), 0);
	EXPECT_EQ(Choices.ProtocolIndex((int)EConnectProtocol::WEBSOCKET), 0);
	ASSERT_EQ(Choices.m_NumFamilies, 2);
	EXPECT_EQ(Choices.m_aFamilies[0], EConnectAddressFamily::IPV6);
	EXPECT_EQ(Choices.m_aFamilies[1], EConnectAddressFamily::IPV4);
	EXPECT_EQ(Choices.FamilyIndex((int)EConnectAddressFamily::IPV4), 1);
}

TEST_F(ConnectChoice, LinkAndTypedAddress)
{
	// The box shows what the address connects with, among the server's
	// own choices.
	const CConnectChoices Link(&m_Info, "ddnet+wt://127.0.0.1:8303#webpki");
	EXPECT_EQ(Link.m_aProtocols[Link.ProtocolIndex((int)EConnectProtocol::QUIC)], EConnectProtocol::WEBTRANSPORT);
	EXPECT_EQ(Link.FamilyIndex((int)EConnectAddressFamily::IPV6), 1);

	const CConnectChoices Plain(&m_Info, "[::1]:8303");
	EXPECT_EQ(Plain.m_aProtocols[Plain.ProtocolIndex(-1)], EConnectProtocol::LEGACY);
	EXPECT_EQ(Plain.FamilyIndex((int)EConnectAddressFamily::IPV4), 0);

	const CConnectChoices UnknownLink(nullptr, "ddnet+wt://127.0.0.1:8303#webpki");
	ASSERT_EQ(UnknownLink.m_NumProtocols, 1);
	EXPECT_EQ(UnknownLink.m_aProtocols[0], EConnectProtocol::WEBTRANSPORT);

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

// Writes the address a connect with the picks uses.
static std::string Address(const CServerInfo &Info, int Protocol, int Family, EConnectPrecedence Precedence = EConnectPrecedence::PROTOCOL)
{
	char aAddress[512];
	if(!ConnectAddressFor(Info, Protocol, Family, Precedence, aAddress, sizeof(aAddress)))
		return "";
	return aAddress;
}

static constexpr int QUIC = (int)EConnectProtocol::QUIC;
static constexpr int LEGACY = (int)EConnectProtocol::LEGACY;
static constexpr int IPV4 = (int)EConnectAddressFamily::IPV4;
static constexpr int IPV6 = (int)EConnectAddressFamily::IPV6;

TEST_F(ConnectChoice, ConnectAddress)
{
#if defined(CONF_NETWORKING_QUIC)
	char aQuic[256];
	str_format(aQuic, sizeof(aQuic), "ddnet+quic://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
	// Transport, then family; QUIC is the best.
	EXPECT_EQ(Address(m_Info, -1, IPV6), aQuic);
	EXPECT_EQ(Address(m_Info, QUIC, IPV4), aQuic);
	// The certificate is signed for the name, so the name is connected by.
	EXPECT_EQ(Address(m_Info, (int)EConnectProtocol::WEBTRANSPORT, IPV4), "ddnet+wt://ger10.ddnet.org:8303#webpki");
#endif
	EXPECT_EQ(Address(m_Info, LEGACY, IPV6), "[::1]:8303");
	EXPECT_EQ(Address(m_Info, LEGACY, IPV4), "127.0.0.1:8303");

	CServerInfo Empty{};
	char aAddress[64];
	str_copy(aAddress, "unchanged");
	EXPECT_FALSE(ConnectAddressFor(Empty, LEGACY, IPV4, EConnectPrecedence::PROTOCOL, aAddress, sizeof(aAddress)));
	EXPECT_STREQ(aAddress, "unchanged");
}

#if defined(CONF_NETWORKING_QUIC)
// Transport and family are picked independently; where the server lacks the
// combination, the pick just made wins and the other falls back.
TEST_F(ConnectChoice, IndependentPicks)
{
	char aQuic[256];
	str_format(aQuic, sizeof(aQuic), "ddnet+quic://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
	// QUIC only over IPv4: picking QUIC keeps QUIC, picking IPv6 keeps IPv6.
	EXPECT_EQ(Address(m_Info, QUIC, IPV6, EConnectPrecedence::PROTOCOL), aQuic);
	EXPECT_EQ(Address(m_Info, QUIC, IPV6, EConnectPrecedence::ADDRESS_FAMILY), "[::1]:8303");
	EXPECT_EQ(Address(m_Info, -1, IPV6, EConnectPrecedence::ADDRESS_FAMILY), "[::1]:8303");
	EXPECT_EQ(Address(m_Info, QUIC, IPV4, EConnectPrecedence::ADDRESS_FAMILY), aQuic);
	EXPECT_EQ(Address(m_Info, LEGACY, IPV4, EConnectPrecedence::ADDRESS_FAMILY), "127.0.0.1:8303");
	EXPECT_EQ(Address(m_Info, LEGACY, IPV6, EConnectPrecedence::PROTOCOL), "[::1]:8303");

	// What is shown is what the address connects with, the picks stay.
	const CConnectChoices Shown(&m_Info, aQuic);
	EXPECT_EQ(Shown.m_aFamilies[Shown.FamilyIndex(IPV6)], EConnectAddressFamily::IPV4);
	EXPECT_EQ(Shown.m_aProtocols[Shown.ProtocolIndex(LEGACY)], EConnectProtocol::QUIC);

	// Where both are there, the combination is taken whatever came last.
	CServerInfo Full{};
	AddAddress(Full, "tw-0.6+udp://127.0.0.1:8303");
	AddAddress(Full, "tw-0.6+udp://[::1]:8303");
	AddAddress(Full, "ddnet+quic://127.0.0.1:8303");
	AddAddress(Full, "ddnet+quic://[::1]:8303");
	for(const EConnectPrecedence Precedence : {EConnectPrecedence::PROTOCOL, EConnectPrecedence::ADDRESS_FAMILY})
	{
		EXPECT_EQ(Address(Full, QUIC, IPV4, Precedence), "ddnet+quic://127.0.0.1:8303");
		EXPECT_EQ(Address(Full, QUIC, IPV6, Precedence), "ddnet+quic://[::1]:8303");
		EXPECT_EQ(Address(Full, LEGACY, IPV4, Precedence), "127.0.0.1:8303");
		EXPECT_EQ(Address(Full, LEGACY, IPV6, Precedence), "[::1]:8303");
		EXPECT_EQ(Address(Full, -1, IPV6, Precedence), "ddnet+quic://[::1]:8303");
	}
}
#endif

TEST_F(ConnectChoice, ServerHasAddress)
{
	EXPECT_TRUE(ServerHasAddress(m_Info, "127.0.0.1:8303"));
	EXPECT_TRUE(ServerHasAddress(m_Info, "ddnet+wt://127.0.0.1:8303"));
	EXPECT_TRUE(ServerHasAddress(m_Info, "ddnet+wt://ger10.ddnet.org:8303#webpki"));
	EXPECT_FALSE(ServerHasAddress(m_Info, "ddnet+wt://ger10.ddnet.org:8304"));
	EXPECT_FALSE(ServerHasAddress(m_Info, "ddnet+wt://ger11.ddnet.org:8303"));
	EXPECT_FALSE(ServerHasAddress(m_Info, "127.0.0.2:8303"));
}
