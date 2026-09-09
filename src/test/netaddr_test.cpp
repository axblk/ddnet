#include <base/net.h>

#include <gtest/gtest.h>

TEST(NetAddr, FromUrlStringInvalid)
{
	NETADDR Addr;

	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+udp://127.0", nullptr, 0), -1); // invalid ip
	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+udp://ddnet.org", nullptr, 0), -1); // invalid ip
	EXPECT_EQ(net_addr_from_url(&Addr, "127.0.0.1", nullptr, 0), 1); // not a URL
	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.9+udp://127.0.0.1", nullptr, 0), 1); // invalid tw protocol
	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+tcp://127.0.0.1", nullptr, 0), 1); // invalid internet protocol
}

TEST(NetAddr, FromUrlStringValid)
{
	NETADDR Addr;
	char aBuf1[NETADDR_MAXSTRSIZE];
	char aBuf2[NETADDR_MAXSTRSIZE];

	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.7+udp://127.0.0.1", nullptr, 0), 0);
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "127.0.0.1:0");
	EXPECT_STREQ(aBuf2, "127.0.0.1");

	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+udp://127.0.0.1", nullptr, 0), 0);
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "127.0.0.1:0");
	EXPECT_STREQ(aBuf2, "127.0.0.1");

	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+udp://127.0.0.1", nullptr, 0), 0);
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "127.0.0.1:0");
	EXPECT_STREQ(aBuf2, "127.0.0.1");

	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+udp://[0123:4567:89ab:cdef:1:2:3:4]:5678", nullptr, 0), 0);
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "[123:4567:89ab:cdef:1:2:3:4]:5678");
	EXPECT_STREQ(aBuf2, "[123:4567:89ab:cdef:1:2:3:4]");

	char aHost[128];
	EXPECT_EQ(net_addr_from_url(&Addr, "tw-0.6+udp://ger10.ddnet.org:5678", aHost, sizeof(aHost)), -1);
	EXPECT_STREQ(aHost, "ger10.ddnet.org:5678");
}

TEST(NetAddr, FromUrlStringQuic)
{
	NETADDR Addr;
	char aBuf[NETADDR_MAXSTRSIZE];

	// The fragment pins the server's identity and is not part of the address.
	EXPECT_EQ(net_addr_from_url(&Addr, "ddnet+quic://127.0.0.1:8303#0123456789abcdef", nullptr, 0), 0);
	EXPECT_TRUE(Addr.type & NETTYPE_QUIC);
	EXPECT_FALSE(Addr.type & NETTYPE_TW7);
	net_addr_str(&Addr, aBuf, sizeof(aBuf), true);
	EXPECT_STREQ(aBuf, "127.0.0.1:8303");

	EXPECT_EQ(net_addr_from_url(&Addr, "ddnet+quic://[::1]:8303", nullptr, 0), 0);
	EXPECT_TRUE(Addr.type & NETTYPE_QUIC);
	net_addr_str(&Addr, aBuf, sizeof(aBuf), true);
	EXPECT_STREQ(aBuf, "[::1]:8303");
}

TEST(NetAddr, FromUrlStringWebTransport)
{
	NETADDR Addr;
	EXPECT_EQ(net_addr_from_url(&Addr, "ddnet+wt://127.0.0.1:8303#0123456789abcdef", nullptr, 0), 0);
	EXPECT_TRUE(Addr.type & NETTYPE_QUIC);
	EXPECT_TRUE(Addr.type & NETTYPE_WEBTRANSPORT);
	EXPECT_EQ(Addr.port, 8303);
}

TEST(NetAddr, FromUrlStringWebSocket)
{
	NETADDR Addr;
	EXPECT_EQ(net_addr_from_url(&Addr, "ddnet+ws://127.0.0.1:8303", nullptr, 0), 0);
	EXPECT_TRUE(Addr.type & NETTYPE_WEBSOCKET);
	EXPECT_FALSE(Addr.type & NETTYPE_WEBSOCKET_TLS);
	EXPECT_FALSE(Addr.type & NETTYPE_QUIC);
	EXPECT_EQ(net_addr_from_url(&Addr, "ddnet+wss://[::1]:8303#0123456789abcdef", nullptr, 0), 0);
	EXPECT_TRUE(Addr.type & NETTYPE_WEBSOCKET);
	EXPECT_TRUE(Addr.type & NETTYPE_WEBSOCKET_TLS);
	EXPECT_EQ(Addr.port, 8303);
}

TEST(NetAddr, FromStr)
{
	NETADDR Addr;
	char aBuf1[NETADDR_MAXSTRSIZE];
	char aBuf2[NETADDR_MAXSTRSIZE];

	EXPECT_FALSE(net_addr_from_str(&Addr, "127.0.0.1"));
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "127.0.0.1:0");
	EXPECT_STREQ(aBuf2, "127.0.0.1");

	EXPECT_FALSE(net_addr_from_str(&Addr, "1.2.3.4:5678"));
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "1.2.3.4:5678");
	EXPECT_STREQ(aBuf2, "1.2.3.4");

	EXPECT_FALSE(net_addr_from_str(&Addr, "[::1]"));
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "[::1]:0");
	EXPECT_STREQ(aBuf2, "[::1]");

	EXPECT_FALSE(net_addr_from_str(&Addr, "[0123:4567:89ab:cdef:1:2:3:4]:5678"));
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "[123:4567:89ab:cdef:1:2:3:4]:5678");
	EXPECT_STREQ(aBuf2, "[123:4567:89ab:cdef:1:2:3:4]");
}

TEST(NetAddr, StrV6)
{
	NETADDR Addr;
	char aBuf1[NETADDR_MAXSTRSIZE];
	char aBuf2[NETADDR_MAXSTRSIZE];

	// Test vectors from RFC 5952 section 4:
	// https://tools.ietf.org/html/rfc5952#section-4
	// 4.1 Handling Leading Zeros in a 16-Bit Field
	EXPECT_FALSE(net_addr_from_str(&Addr, "[2001:0db8::0001]:1"));
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "[2001:db8::1]:1");
	EXPECT_STREQ(aBuf2, "[2001:db8::1]");

	// 4.2.1 Shorten as Much as Possible
	EXPECT_FALSE(net_addr_from_str(&Addr, "[2001:db8:0:0:0:0:2:1]"));
	net_addr_str(&Addr, aBuf1, sizeof(aBuf1), true);
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf1, "[2001:db8::2:1]:0");
	EXPECT_STREQ(aBuf2, "[2001:db8::2:1]");

	EXPECT_FALSE(net_addr_from_str(&Addr, "[2001:db8::0:1]"));
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf2, "[2001:db8::1]");

	// 4.2.2 Handling One 16-Bit 0 Field
	EXPECT_FALSE(net_addr_from_str(&Addr, "[2001:db8::1:1:1:1:1]"));
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf2, "[2001:db8:0:1:1:1:1:1]");

	// 4.2.3 Choice in Placement of "::"
	EXPECT_FALSE(net_addr_from_str(&Addr, "[2001:0:0:1:0:0:0:1]"));
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf2, "[2001:0:0:1::1]");

	EXPECT_FALSE(net_addr_from_str(&Addr, "[2001:db8:0:0:1:0:0:1]"));
	net_addr_str(&Addr, aBuf2, sizeof(aBuf2), false);
	EXPECT_STREQ(aBuf2, "[2001:db8::1:0:0:1]");
}

TEST(NetAddr, FromStrInvalid)
{
	NETADDR Addr;
	EXPECT_TRUE(net_addr_from_str(&Addr, "127.0.0."));
	EXPECT_TRUE(net_addr_from_str(&Addr, "127.0.0.1a"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "1.1"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "[::1"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "[::"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "127.0.0.1:"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "[::]:"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "127.0.0.1:1a"));
	EXPECT_TRUE(net_addr_from_str(&Addr, "[::]:c"));
}

TEST(NetAddrDeathTest, StrInvalid1)
{
	ASSERT_DEATH({
		NETADDR Addr = {0};
		char aBuf[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aBuf, sizeof(aBuf), true); }, "");
}

TEST(NetAddrDeathTest, StrInvalid2)
{
	ASSERT_DEATH({
		NETADDR Addr = {0};
		char aBuf[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aBuf, sizeof(aBuf), false); }, "");
}

TEST(NetAddr, IsLocal)
{
	NETADDR Addr;
	net_addr_from_str(&Addr, "127.0.0.1");
	EXPECT_TRUE(net_addr_is_local(&Addr));

	net_addr_from_str(&Addr, "[::1]");
	EXPECT_TRUE(net_addr_is_local(&Addr));

	net_addr_from_str(&Addr, "192.168.1.1");
	EXPECT_TRUE(net_addr_is_local(&Addr));

	net_addr_from_str(&Addr, "10.0.0.1");
	EXPECT_TRUE(net_addr_is_local(&Addr));

	net_addr_from_str(&Addr, "8.8.8.8");
	EXPECT_FALSE(net_addr_is_local(&Addr));

	net_addr_from_str(&Addr, "[2001:db8::1]");
	EXPECT_FALSE(net_addr_is_local(&Addr));
}
