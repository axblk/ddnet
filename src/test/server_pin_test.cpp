#include <base/net.h>
#include <base/str.h>

#include <engine/shared/server_pin.h>

#include <gtest/gtest.h>

static const char IDENTITY[] = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";

static NETADDR Addr(const char *pUrl)
{
	NETADDR Addr;
	EXPECT_EQ(net_addr_from_url(&Addr, pUrl, nullptr, 0), 0);
	return Addr;
}

TEST(ServerPin, FragmentsRoundTrip)
{
	CServerPin Pin;
	Pin.Reset();
	char aFragment[128];
	str_format(aFragment, sizeof(aFragment), "identity-sha256=%s", IDENTITY);
	Pin.AddFragment(Addr("ddnet+quic://127.0.0.1:8303"), aFragment);
	Pin.AddFragment(Addr("ddnet+wt://127.0.0.1:8303"), "cert-sha256=00,11");
	EXPECT_STREQ(Pin.m_aIdentity, IDENTITY);
	EXPECT_STREQ(Pin.m_aWebTransport, "cert-sha256=00,11");

	char aUrl[CServerPin::URL_MAXSTRSIZE];
	char aExpected[CServerPin::URL_MAXSTRSIZE];
	Pin.AddressUrl(Addr("ddnet+ws://127.0.0.1:8303"), aUrl, sizeof(aUrl));
	str_format(aExpected, sizeof(aExpected), "ddnet+ws://127.0.0.1:8303#identity-sha256=%s", IDENTITY);
	EXPECT_STREQ(aUrl, aExpected);
	Pin.AddressUrl(Addr("ddnet+wt://127.0.0.1:8303"), aUrl, sizeof(aUrl));
	EXPECT_STREQ(aUrl, "ddnet+wt://127.0.0.1:8303#cert-sha256=00,11");
	Pin.AddressUrl(Addr("tw-0.6+udp://127.0.0.1:8303"), aUrl, sizeof(aUrl));
	EXPECT_STREQ(aUrl, "127.0.0.1:8303");
}

TEST(ServerPin, FirstOfEachKindStays)
{
	CServerPin Pin;
	Pin.Reset();
	Pin.AddFragment(Addr("ddnet+wt://127.0.0.1:8303"), "webpki");
	Pin.AddFragment(Addr("ddnet+wt://[::1]:8303"), "cert-sha256=00");
	EXPECT_STREQ(Pin.m_aWebTransport, "webpki");
	EXPECT_TRUE(Pin.SignedForName(Addr("ddnet+wt://127.0.0.1:8303")));
	EXPECT_FALSE(Pin.SignedForName(Addr("ddnet+quic://127.0.0.1:8303")));
	EXPECT_TRUE(Pin.SignedForName(Addr("ddnet+wss://127.0.0.1:8303")));
}

TEST(ServerPin, OtherFragmentsPinNothing)
{
	CServerPin Pin;
	Pin.Reset();
	Pin.AddFragment(Addr("ddnet+quic://127.0.0.1:8303"), IDENTITY);
	Pin.AddFragment(Addr("ddnet+quic://127.0.0.1:8303"), "identity-sha256=zz");
	Pin.AddFragment(Addr("ddnet+wt://127.0.0.1:8303"), "sha256=00");
	Pin.AddFragment(Addr("tw-0.6+udp://127.0.0.1:8303"), "webpki");
	EXPECT_STREQ(Pin.m_aIdentity, "");
	EXPECT_STREQ(Pin.m_aWebTransport, "");
}
