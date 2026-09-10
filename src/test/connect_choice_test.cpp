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
	// A native client uses UDP and QUIC, not the WebTransport address.
	const CConnectChoices Choices(&m_Info, nullptr);
	ASSERT_EQ(Choices.m_NumProtocols, 2);
	EXPECT_EQ(Choices.m_aProtocols[0], EConnectProtocol::QUIC);
	EXPECT_EQ(Choices.m_aProtocols[1], EConnectProtocol::LEGACY);
	EXPECT_EQ(Choices.ProtocolIndex((int)EConnectProtocol::LEGACY), 1);
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
	// own choices, even where this client would not offer it.
	const CConnectChoices Link(&m_Info, "ddnet+wt://127.0.0.1:8303#webpki");
	ASSERT_EQ(Link.m_NumProtocols, 3);
	EXPECT_EQ(Link.m_aProtocols[0], EConnectProtocol::QUIC);
	EXPECT_EQ(Link.m_aProtocols[1], EConnectProtocol::LEGACY);
	EXPECT_EQ(Link.m_aProtocols[2], EConnectProtocol::WEBTRANSPORT);
	EXPECT_EQ(Link.ProtocolIndex((int)EConnectProtocol::QUIC), 2);
	EXPECT_EQ(Link.FamilyIndex((int)EConnectAddressFamily::IPV6), 1);

	const CConnectChoices Plain(&m_Info, "[::1]:8303");
	EXPECT_EQ(Plain.ProtocolIndex(-1), 1);
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
	char aQuic[256];
	str_format(aQuic, sizeof(aQuic), "ddnet+quic://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
	// Transport, then family; QUIC is the best.
	EXPECT_EQ(Address(m_Info, -1, IPV6), aQuic);
	EXPECT_EQ(Address(m_Info, QUIC, IPV4), aQuic);
	EXPECT_EQ(Address(m_Info, LEGACY, IPV6), "[::1]:8303");
	EXPECT_EQ(Address(m_Info, LEGACY, IPV4), "127.0.0.1:8303");
	// WebTransport is not for a native client, the best is taken.
	EXPECT_EQ(Address(m_Info, (int)EConnectProtocol::WEBTRANSPORT, IPV4), aQuic);

	CServerInfo Empty{};
	char aAddress[64];
	str_copy(aAddress, "unchanged");
	EXPECT_FALSE(ConnectAddressFor(Empty, LEGACY, IPV4, EConnectPrecedence::PROTOCOL, aAddress, sizeof(aAddress)));
	EXPECT_STREQ(aAddress, "unchanged");
}

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

// Where a DDNet endpoint is left, the 0.7 ones are not used at all.
TEST_F(ConnectChoice, DdnetBeforeSixup)
{
	CServerInfo Mixed{};
	AddAddress(Mixed, "tw-0.7+udp://[::1]:8303");
	AddAddress(Mixed, "tw-0.6+udp://127.0.0.1:8303");
	EXPECT_FALSE(ConnectEndpointUsable(Mixed, Mixed.m_aAddresses[0]));
	EXPECT_TRUE(ConnectEndpointUsable(Mixed, Mixed.m_aAddresses[1]));
	const CConnectChoices Choices(&Mixed, nullptr);
	ASSERT_EQ(Choices.m_NumProtocols, 1);
	EXPECT_EQ(Choices.m_aProtocols[0], EConnectProtocol::LEGACY);
	ASSERT_EQ(Choices.m_NumFamilies, 1);
	EXPECT_EQ(Choices.m_aFamilies[0], EConnectAddressFamily::IPV4);
	EXPECT_EQ(Address(Mixed, QUIC, IPV6), "127.0.0.1:8303");
	EXPECT_EQ(Address(Mixed, QUIC, IPV6, EConnectPrecedence::ADDRESS_FAMILY), "127.0.0.1:8303");

	// DDNet over QUIC is DDNet as well.
	CServerInfo QuicOnly{};
	AddAddress(QuicOnly, "tw-0.7+udp://127.0.0.1:8303");
	AddAddress(QuicOnly, "ddnet+quic://127.0.0.1:8303");
	EXPECT_EQ(Address(QuicOnly, LEGACY, IPV4), "ddnet+quic://127.0.0.1:8303");

	// Without DDNet, 0.7 it is.
	CServerInfo Sixup{};
	AddAddress(Sixup, "tw-0.7+udp://127.0.0.1:8303");
	EXPECT_EQ(Address(Sixup, -1, IPV6), "tw-0.7+udp://127.0.0.1:8303");
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

TEST_F(ConnectChoice, Reachable)
{
	EXPECT_TRUE(ServerReachable(m_Info));
	CServerInfo Empty{};
	EXPECT_FALSE(ServerReachable(Empty));
	// A native client has UDP and QUIC, a browser WebTransport and
	// WebSockets.
	EXPECT_TRUE(ConnectProtocolAvailable(EConnectProtocol::LEGACY));
	EXPECT_TRUE(ConnectProtocolAvailable(EConnectProtocol::QUIC));
	EXPECT_FALSE(ConnectProtocolAvailable(EConnectProtocol::WEBTRANSPORT));
	EXPECT_FALSE(ConnectProtocolAvailable(EConnectProtocol::WEBSOCKET));
	CServerInfo Browser{};
	AddAddress(Browser, "ddnet+wt://127.0.0.1:8303#webpki");
	AddAddress(Browser, "ddnet+ws://127.0.0.1:8303");
	EXPECT_FALSE(ServerReachable(Browser));
}
