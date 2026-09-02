// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
// Feeds synthetic packets through the loaded XDP program and checks both the verdict
// and the class it was counted under.
//
// This is the quick way to find out whether the filter does what it should on a given
// machine. Loading the object answers the question the compiler cannot: the verifier
// has to accept the program. BPF_PROG_RUN then runs the very same program that would
// run on the wire, without a network interface, without traffic, and without touching
// anything else the machine is doing.
//
// Needs root, or CAP_BPF and CAP_NET_ADMIN.
#include "ddnet_xdp_classify.h"
#include "ddnet_xdp_shared.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/bpf.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Written out rather than taken from the system headers, because the kernel and the
// libc versions of them cannot be included together.
#define ETH_P_IP_BE 0x0008
#define ETH_P_IPV6_BE 0xdd86
#define IP_PROTO_UDP 17

struct eth_header
{
	uint8_t m_aDestination[6];
	uint8_t m_aSource[6];
	uint16_t m_Protocol;
} __attribute__((packed));

struct ipv4_header
{
	uint8_t m_VersionIhl;
	uint8_t m_Tos;
	uint16_t m_TotalLength;
	uint16_t m_Id;
	uint16_t m_FragmentOffset;
	uint8_t m_Ttl;
	uint8_t m_Protocol;
	uint16_t m_Checksum;
	uint32_t m_Source;
	uint32_t m_Destination;
} __attribute__((packed));

struct ipv6_header
{
	uint32_t m_VersionClassLabel;
	uint16_t m_PayloadLength;
	uint8_t m_NextHeader;
	uint8_t m_HopLimit;
	uint8_t m_aSource[16];
	uint8_t m_aDestination[16];
} __attribute__((packed));

struct udp_header
{
	uint16_t m_Source;
	uint16_t m_Destination;
	uint16_t m_Length;
	uint16_t m_Checksum;
} __attribute__((packed));

#define TEST_PORT 8303
#define OTHER_PORT 9000
#define TEST_EPOCH 1

static const struct siphash_key TEST_KEY = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
static const char *const CLIENT_V4 = "192.0.2.1";
static const char *const CLIENT_V6 = "2001:db8::1";
static const char *const MASTER_V4 = "198.51.100.9";
static const char *const STRANGER_V4 = "203.0.113.7";
static const uint16_t CLIENT_PORT = 40000;

struct packet
{
	uint8_t m_aData[2048];
	uint32_t m_Size;
};

// IPv6 extension header to put in front of UDP in the next packet built, or -1.
static int s_NextExtension = -1;

// Wraps a payload in ethernet, IP and UDP. Checksums stay zero because neither XDP
// nor this filter looks at them.
static void build(struct packet *pPacket, bool Ipv6, const char *pSource, uint16_t SourcePort,
	uint16_t DestinationPort, const uint8_t *pPayload, uint32_t PayloadSize, bool Fragment)
{
	struct eth_header *pEth = (struct eth_header *)pPacket->m_aData;
	uint32_t Offset = sizeof(*pEth);

	memset(pPacket, 0, sizeof(*pPacket));
	memset(pEth->m_aDestination, 0x02, sizeof(pEth->m_aDestination));
	memset(pEth->m_aSource, 0x03, sizeof(pEth->m_aSource));
	pEth->m_Protocol = Ipv6 ? ETH_P_IPV6_BE : ETH_P_IP_BE;

	if(Ipv6)
	{
		struct ipv6_header *pIp = (struct ipv6_header *)&pPacket->m_aData[Offset];
		const int Extension = s_NextExtension;
		s_NextExtension = -1;
		pIp->m_VersionClassLabel = htonl(6u << 28);
		pIp->m_NextHeader = Extension < 0 ? IP_PROTO_UDP : (uint8_t)Extension;
		pIp->m_HopLimit = 64;
		pIp->m_PayloadLength = htons((uint16_t)((Extension < 0 ? 0 : 8) + sizeof(struct udp_header) + PayloadSize));
		inet_pton(AF_INET6, pSource, pIp->m_aSource);
		inet_pton(AF_INET6, "2001:db8::2", pIp->m_aDestination);
		Offset += sizeof(*pIp);
		if(Extension >= 0)
		{
			// One eight byte extension header, next header UDP, length zero.
			pPacket->m_aData[Offset] = IP_PROTO_UDP;
			pPacket->m_aData[Offset + 1] = 0;
			Offset += 8;
		}
	}
	else
	{
		struct ipv4_header *pIp = (struct ipv4_header *)&pPacket->m_aData[Offset];
		pIp->m_VersionIhl = 0x45;
		pIp->m_Ttl = 64;
		pIp->m_Protocol = IP_PROTO_UDP;
		pIp->m_TotalLength = htons((uint16_t)(sizeof(*pIp) + sizeof(struct udp_header) + PayloadSize));
		pIp->m_FragmentOffset = Fragment ? htons(0x2000) : 0;
		inet_pton(AF_INET, pSource, &pIp->m_Source);
		inet_pton(AF_INET, "192.0.2.100", &pIp->m_Destination);
		Offset += sizeof(*pIp);
	}

	{
		struct udp_header *pUdp = (struct udp_header *)&pPacket->m_aData[Offset];
		pUdp->m_Source = htons(SourcePort);
		pUdp->m_Destination = htons(DestinationPort);
		pUdp->m_Length = htons((uint16_t)(sizeof(*pUdp) + PayloadSize));
		Offset += sizeof(*pUdp);
	}

	memcpy(&pPacket->m_aData[Offset], pPayload, PayloadSize);
	pPacket->m_Size = Offset + PayloadSize;
}

// The canonical token input, the same bytes the filter and the server both build.
static uint32_t token_for(bool Ipv6, const char *pAddress, uint16_t Port)
{
	uint8_t aInput[19];
	uint32_t Size;

	if(Ipv6)
	{
		aInput[0] = DDNET_XDP_FAMILY_IPV6;
		inet_pton(AF_INET6, pAddress, &aInput[1]);
		Size = 19;
	}
	else
	{
		aInput[0] = DDNET_XDP_FAMILY_IPV4;
		inet_pton(AF_INET, pAddress, &aInput[1]);
		Size = 7;
	}
	aInput[Size - 2] = (uint8_t)(Port >> 8);
	aInput[Size - 1] = (uint8_t)Port;
	return (uint32_t)siphash24(&TEST_KEY, aInput, Size);
}

static void write_be32(uint8_t *pOut, uint32_t Value)
{
	pOut[0] = (uint8_t)(Value >> 24);
	pOut[1] = (uint8_t)(Value >> 16);
	pOut[2] = (uint8_t)(Value >> 8);
	pOut[3] = (uint8_t)Value;
}

static void make_cid(uint8_t *pOut, uint8_t Nonce)
{
	pOut[0] = (uint8_t)((TEST_EPOCH << 5) | (Nonce & 0x1f));
	pOut[1] = Nonce;
	pOut[2] = 0xaa;
	pOut[3] = 0xbb;
	write_be32(&pOut[4], (uint32_t)siphash24(&TEST_KEY, pOut, 4));
}

struct expectation
{
	const char *m_pName;
	struct packet m_Packet;
	bool m_Armed;
	int m_Action;
	int m_Class; // -1 when the packet never reaches the classifier
	int m_Verdict;
	// When set, the returned packet has to be a reply carrying these payload bytes
	// with the addresses and ports turned around.
	const uint8_t *m_pReply;
	uint32_t m_ReplySize;
	// Whether the filter is allowed to answer handshakes for this case.
	bool m_Offload;
};

static int s_Failures;
static int s_Checks;
#define MAX_CASES 64
static struct expectation s_aCases[MAX_CASES];
static int s_NumCases;

static const char *action_name(int Action)
{
	switch(Action)
	{
	case XDP_PASS: return "pass";
	case XDP_DROP: return "drop";
	default: return "something else";
	}
}

static struct packet *add_case(const char *pName, bool Armed, int Action, int Class, int Verdict)
{
	struct expectation *pCase;
	if(s_NumCases >= MAX_CASES)
	{
		// Writing past the end would be silent, and the cases only ever grow.
		fprintf(stderr, "more than %d cases, raise MAX_CASES\n", MAX_CASES);
		exit(2);
	}
	pCase = &s_aCases[s_NumCases++];
	memset(pCase, 0, sizeof(*pCase));
	pCase->m_pName = pName;
	pCase->m_Armed = Armed;
	pCase->m_Action = Action;
	pCase->m_Class = Class;
	pCase->m_Verdict = Verdict;
	pCase->m_Offload = true;
	return &pCase->m_Packet;
}

static void without_offload(void)
{
	s_aCases[s_NumCases - 1].m_Offload = false;
}

static void expect_reply(const uint8_t *pReply, uint32_t Size)
{
	s_aCases[s_NumCases - 1].m_pReply = pReply;
	s_aCases[s_NumCases - 1].m_ReplySize = Size;
}

static void read_stats(int StatsMap, int NumCpus, uint64_t *pOut)
{
	uint64_t *pValues = calloc(NumCpus, sizeof(uint64_t));
	if(!pValues)
		return;
	for(uint32_t Index = 0; Index < DDNET_XDP_NUM_COUNTERS; Index++)
	{
		pOut[Index] = 0;
		if(bpf_map_lookup_elem(StatsMap, &Index, pValues) != 0)
			continue;
		for(int Cpu = 0; Cpu < NumCpus; Cpu++)
			pOut[Index] += pValues[Cpu];
	}
	free(pValues);
}

static int s_ConfigMap;

static void run(int ProgramFd, int PortMap, int StatsMap, int NumCpus, const struct expectation *pCase)
{
	{
		struct ddnet_xdp_config Config;
		uint32_t Zero = 0;
		if(bpf_map_lookup_elem(s_ConfigMap, &Zero, &Config) == 0)
		{
			Config.m_Offload = pCase->m_Offload ? 1 : 0;
			bpf_map_update_elem(s_ConfigMap, &Zero, &Config, BPF_ANY);
		}
	}
	static uint64_t s_aBefore[DDNET_XDP_NUM_COUNTERS], s_aAfter[DDNET_XDP_NUM_COUNTERS];
	static uint8_t s_aOut[4096];
	uint16_t Port = TEST_PORT;
	struct ddnet_xdp_port PortEntry;
	LIBBPF_OPTS(bpf_test_run_opts, Options,
		.data_in = pCase->m_Packet.m_aData,
		.data_size_in = pCase->m_Packet.m_Size,
		.data_out = s_aOut,
		.data_size_out = sizeof(s_aOut),
		.repeat = 1);
	const uint8_t *pSent = pCase->m_Packet.m_aData;

	s_Checks++;

	memset(&PortEntry, 0, sizeof(PortEntry));
	PortEntry.m_Armed = pCase->m_Armed ? 1 : 0;
	bpf_map_update_elem(PortMap, &Port, &PortEntry, BPF_ANY);

	read_stats(StatsMap, NumCpus, s_aBefore);
	if(bpf_prog_test_run_opts(ProgramFd, &Options) != 0)
	{
		printf("  %-42s could not run: %s\n", pCase->m_pName, strerror(errno));
		s_Failures++;
		return;
	}
	read_stats(StatsMap, NumCpus, s_aAfter);

	if((int)Options.retval != pCase->m_Action)
	{
		printf("  %-42s %s, expected %s\n", pCase->m_pName,
			action_name((int)Options.retval), action_name(pCase->m_Action));
		s_Failures++;
		return;
	}

	if(!pCase->m_pReply && (int)Options.retval == XDP_PASS && Options.data_size_out != pCase->m_Packet.m_Size)
	{
		// A packet that is passed on must reach the server as it arrived.
		printf("  %-42s passed, but changed size from %u to %u\n", pCase->m_pName,
			pCase->m_Packet.m_Size, Options.data_size_out);
		s_Failures++;
		return;
	}

	if(pCase->m_pReply)
	{
		// A reply that went to the wrong place would be worse than no reply at all,
		// so the turned around addresses are checked, not only the payload.
		const uint32_t Offset = 14 + (pSent[12] == 0x86 ? 40 : 20) + 8;
		if(Options.data_size_out != Offset + pCase->m_ReplySize)
		{
			printf("  %-42s reply is %u bytes, expected %u\n", pCase->m_pName,
				Options.data_size_out, Offset + pCase->m_ReplySize);
			s_Failures++;
			return;
		}
		if(memcmp(s_aOut, pSent + 6, 6) != 0 || memcmp(s_aOut + 6, pSent, 6) != 0)
		{
			printf("  %-42s reply did not swap the hardware addresses\n", pCase->m_pName);
			s_Failures++;
			return;
		}
		if(memcmp(s_aOut + Offset, pCase->m_pReply, pCase->m_ReplySize) != 0)
		{
			printf("  %-42s reply payload differs\n", pCase->m_pName);
			s_Failures++;
			return;
		}
		{
			// Ports swap; the source of the reply is the port that was asked.
			const uint32_t UdpAt = 14 + (pSent[12] == 0x86 ? 40 : 20);
			if(memcmp(s_aOut + UdpAt, pSent + UdpAt + 2, 2) != 0 ||
				memcmp(s_aOut + UdpAt + 2, pSent + UdpAt, 2) != 0)
			{
				printf("  %-42s reply did not swap the ports\n", pCase->m_pName);
				s_Failures++;
				return;
			}
		}
	}

	if(pCase->m_Class >= 0)
	{
		// A verdict alone would not say which class produced it, and a packet ending
		// up in the right verdict by way of the wrong class is exactly the kind of
		// mistake that stays invisible until it matters.
		const uint32_t Wanted = (uint32_t)(pCase->m_Class * DDNET_XDP_NUM_VERDICTS + pCase->m_Verdict);
		for(uint32_t Index = 0; Index < DDNET_XDP_NUM_COUNTERS; Index++)
		{
			const uint64_t Delta = s_aAfter[Index] - s_aBefore[Index];
			if(Index == Wanted && Delta != 1)
			{
				printf("  %-42s not counted as %s/%s\n", pCase->m_pName,
					DDNET_XDP_CLASS_NAMES[pCase->m_Class],
					DDNET_XDP_VERDICT_NAMES[pCase->m_Verdict]);
				s_Failures++;
				return;
			}
			if(Index != Wanted && Delta != 0)
			{
				printf("  %-42s counted as %s/%s, expected %s/%s\n", pCase->m_pName,
					DDNET_XDP_CLASS_NAMES[Index / DDNET_XDP_NUM_VERDICTS],
					DDNET_XDP_VERDICT_NAMES[Index % DDNET_XDP_NUM_VERDICTS],
					DDNET_XDP_CLASS_NAMES[pCase->m_Class],
					DDNET_XDP_VERDICT_NAMES[pCase->m_Verdict]);
				s_Failures++;
				return;
			}
		}
	}
	printf("  %-42s ok\n", pCase->m_pName);
}

static void build_cases(void)
{
	static uint8_t s_aInitial[1300];
	static const uint8_t s_aNothing[16] = {0};
	static const uint8_t s_aChallenge[20] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 'c', 'h', 'a', 'l'};
	static const uint8_t s_aServerInfo[20] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 'g', 'i', 'e', '3'};
	static const uint8_t s_aLegacy[16] = {0x00, 0, 1};
	// Uncompressed 0.6, the security token in the clear at the end of the payload.
	static uint8_t s_aLegacyVerified[16] = {0x00, 0, 1};
	// The same shape, but with the compression flag set, so the trailing bytes are
	// coded data and mean nothing.
	static uint8_t s_aLegacyCompressed[16] = {0x80, 0, 1};
	static const uint8_t s_aResend[16] = {0x40, 0, 0};
	static const uint8_t s_aGarbage[16] = {0x02, 0, 0, 0};
	uint8_t aSixup[16] = {0x00, 0, 1};
	uint8_t aWrong[16] = {0x04, 0, 0};
	uint8_t aOtherPort[16] = {0x04, 0, 0};
	uint8_t aSixupV6[16] = {0x00, 0, 1};
	uint8_t aConnless[16] = {0x21};
	uint8_t aConnlessBad[16] = {0x21, 1, 2, 3, 4};
	uint8_t aShort[32] = {QUIC_FIXED_BIT};
	uint8_t aShortBad[32] = {QUIC_FIXED_BIT};

	// Traffic that is none of the server's business has to stay untouched.
	build(add_case("a port that is not ours", true, XDP_PASS, -1, 0),
		false, CLIENT_V4, CLIENT_PORT, OTHER_PORT, s_aNothing, sizeof(s_aNothing), false);

	// 0.7 carries its token at a fixed offset in every packet, outside the compression.
	write_be32(&aSixup[3], token_for(false, CLIENT_V4, CLIENT_PORT));
	build(add_case("0.7 with the right token", true, XDP_PASS, DDNET_XDP_CLASS_SIXUP, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aSixup, sizeof(aSixup), false);

	write_be32(&aWrong[3], token_for(false, CLIENT_V4, CLIENT_PORT) ^ 1u);
	build(add_case("0.7 with a wrong token", true, XDP_DROP, DDNET_XDP_CLASS_SIXUP, DDNET_XDP_VERDICT_DROP),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aWrong, sizeof(aWrong), false);
	build(add_case("the same, on a port not armed yet", false, XDP_PASS, DDNET_XDP_CLASS_SIXUP, DDNET_XDP_VERDICT_WOULD_DROP),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aWrong, sizeof(aWrong), false);

	// The port is part of the derivation, so a neighbour's token is of no use.
	write_be32(&aOtherPort[3], token_for(false, CLIENT_V4, CLIENT_PORT + 1));
	build(add_case("0.7 token of another source port", true, XDP_DROP, DDNET_XDP_CLASS_SIXUP, DDNET_XDP_VERDICT_DROP),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aOtherPort, sizeof(aOtherPort), false);

	write_be32(&aSixupV6[3], token_for(true, CLIENT_V6, CLIENT_PORT));
	build(add_case("0.7 over IPv6", true, XDP_PASS, DDNET_XDP_CLASS_SIXUP, DDNET_XDP_VERDICT_PASS),
		true, CLIENT_V6, CLIENT_PORT, TEST_PORT, aSixupV6, sizeof(aSixupV6), false);

	write_be32(&aConnless[1], token_for(false, CLIENT_V4, CLIENT_PORT));
	build(add_case("0.7 connless, right token", true, XDP_PASS, DDNET_XDP_CLASS_SIXUP_CONNLESS, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aConnless, sizeof(aConnless), false);
	build(add_case("0.7 connless, wrong token", true, XDP_DROP, DDNET_XDP_CLASS_SIXUP_CONNLESS, DDNET_XDP_VERDICT_DROP),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aConnlessBad, sizeof(aConnlessBad), false);

	// A QUIC short header says who it belongs to only through its connection id.
	make_cid(&aShort[1], 7);
	build(add_case("quic short header, our id", true, XDP_PASS, DDNET_XDP_CLASS_QUIC_SHORT, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aShort, sizeof(aShort), false);
	make_cid(&aShortBad[1], 7);
	aShortBad[5] ^= 0x40;
	// The fixed bit it sets is the one 0.6 sets on a resend, so without a table of
	// established peers this falls through to the legacy check, where it is bounded
	// rather than trusted. That is the documented difference to the Rust classifier.
	build(add_case("quic short header, forged id", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, aShortBad, sizeof(aShortBad), false);

	// RFC 9000 14.1: an Initial below 1200 bytes is not a legal attempt, it is a cheap
	// way to ask the server for work.
	s_aInitial[0] = 0xc0;
	s_aInitial[4] = 1;
	s_aInitial[5] = 8;
	memset(&s_aInitial[6], 0x11, 8);
	s_aInitial[14] = 8;
	memset(&s_aInitial[15], 0x22, 8);
	s_aInitial[23] = 0;
	build(add_case("quic initial, full size", true, XDP_PASS, DDNET_XDP_CLASS_QUIC_INITIAL, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aInitial, 1250, false);
	build(add_case("quic initial, undersized", true, XDP_DROP, DDNET_XDP_CLASS_QUIC_INITIAL, DDNET_XDP_VERDICT_DROP),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aInitial, 100, false);
	s_aInitial[23] = 4;
	build(add_case("quic initial after a retry", true, XDP_PASS, DDNET_XDP_CLASS_QUIC_INITIAL_TOKEN, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aInitial, 1250, false);

	// Losing these costs the server its place in the server list, not throughput.
	build(add_case("master challenge from the master", true, XDP_PASS, DDNET_XDP_CLASS_MASTER, DDNET_XDP_VERDICT_PASS),
		false, MASTER_V4, 8283, TEST_PORT, s_aChallenge, sizeof(s_aChallenge), false);
	build(add_case("master challenge from a stranger", true, XDP_PASS, DDNET_XDP_CLASS_CONNLESS, DDNET_XDP_VERDICT_PASS),
		false, STRANGER_V4, 8283, TEST_PORT, s_aChallenge, sizeof(s_aChallenge), false);
	build(add_case("0.6 server info", true, XDP_PASS, DDNET_XDP_CLASS_CONNLESS, DDNET_XDP_VERDICT_PASS),
		false, STRANGER_V4, 40000, TEST_PORT, s_aServerInfo, sizeof(s_aServerInfo), false);

	// A 0.6 client that does not compress leaves its token readable, which makes this
	// class exactly as trustworthy as 0.7.
	write_be32(&s_aLegacyVerified[sizeof(s_aLegacyVerified) - 4], token_for(false, CLIENT_V4, CLIENT_PORT));
	build(add_case("0.6 uncompressed, right token", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY_VERIFIED, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aLegacyVerified, sizeof(s_aLegacyVerified), false);

	// The same bytes with the compression flag set must not be trusted: what is at the
	// end of a compressed payload is coded data, not a token.
	memcpy(s_aLegacyCompressed, s_aLegacyVerified, sizeof(s_aLegacyCompressed));
	s_aLegacyCompressed[0] = 0x80;
	build(add_case("0.6 compressed, token is not readable", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aLegacyCompressed, sizeof(s_aLegacyCompressed), false);

	// 0.6 that compresses and never proved itself can only be bounded.
	build(add_case("0.6 data, cannot be verified", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aLegacy, sizeof(s_aLegacy), false);
	// A resend sets the bit that also marks a QUIC short header. Without a connection
	// table the only way to tell them apart is to fall through to the legacy check,
	// and this is the case that confirms it.
	build(add_case("0.6 resend, collides with the quic bit", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aResend, sizeof(s_aResend), false);

	build(add_case("garbage", true, XDP_DROP, DDNET_XDP_CLASS_MALFORMED, DDNET_XDP_VERDICT_DROP),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aGarbage, sizeof(s_aGarbage), false);

	static uint8_t s_aTokenRequest[520];
	static uint8_t s_aConnect[12];
	// The two handshakes the filter can answer itself. Both hand back the number the
	// server would have derived from the source address.
	{
		static uint8_t s_aTokenReply[12];
		s_aTokenRequest[0] = 1 << 2; // 0.7 control
		s_aTokenRequest[7] = 5; // NET_CTRLMSG_TOKEN
		s_aTokenRequest[8] = 0xaa;
		s_aTokenRequest[9] = 0xbb;
		s_aTokenRequest[10] = 0xcc;
		s_aTokenRequest[11] = 0xdd;
		s_aTokenReply[0] = 1 << 2;
		memcpy(&s_aTokenReply[3], &s_aTokenRequest[8], 4);
		s_aTokenReply[7] = 5;
		write_be32(&s_aTokenReply[8], token_for(false, CLIENT_V4, CLIENT_PORT));
		build(add_case("0.7 token request answered here", true, XDP_TX, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_ANSWERED),
			false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aTokenRequest, sizeof(s_aTokenRequest), false);
		expect_reply(s_aTokenReply, sizeof(s_aTokenReply));

		static uint8_t s_aTokenReplyV6[12];
		memcpy(s_aTokenReplyV6, s_aTokenReply, sizeof(s_aTokenReplyV6));
		write_be32(&s_aTokenReplyV6[8], token_for(true, CLIENT_V6, CLIENT_PORT));
		build(add_case("0.7 token request over IPv6", true, XDP_TX, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_ANSWERED),
			true, CLIENT_V6, CLIENT_PORT, TEST_PORT, s_aTokenRequest, sizeof(s_aTokenRequest), false);
		expect_reply(s_aTokenReplyV6, sizeof(s_aTokenReplyV6));

		static uint8_t s_aConnectReply[12];
		s_aConnect[0] = 4 << 2; // 0.6 control
		s_aConnect[3] = 1; // NET_CTRLMSG_CONNECT
		s_aConnect[4] = 'T';
		s_aConnect[5] = 'K';
		s_aConnect[6] = 'E';
		s_aConnect[7] = 'N';
		s_aConnectReply[0] = 4 << 2;
		s_aConnectReply[3] = 2; // NET_CTRLMSG_CONNECTACCEPT
		memcpy(&s_aConnectReply[4], "TKEN", 4);
		write_be32(&s_aConnectReply[8], token_for(false, CLIENT_V4, CLIENT_PORT));
		build(add_case("0.6 ddnet connect answered here", true, XDP_TX, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_ANSWERED),
			false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aConnect, sizeof(s_aConnect), false);
		expect_reply(s_aConnectReply, sizeof(s_aConnectReply));

		// Vanilla 0.6 has no magic and no token, so there is nothing to answer.
		static uint8_t s_aVanillaConnect[12];
		memcpy(s_aVanillaConnect, s_aConnect, sizeof(s_aVanillaConnect));
		s_aVanillaConnect[4] = 'X';
		build(add_case("vanilla connect is not answered", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
			false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aVanillaConnect, sizeof(s_aVanillaConnect), false);
	}

	// The accept is what makes the server hand out a slot, so a wrong token on one is
	// refused rather than bounded.
	{
		static uint8_t s_aAccept[8];
		s_aAccept[0] = 4 << 2;
		s_aAccept[3] = 3; // NET_CTRLMSG_ACCEPT
		write_be32(&s_aAccept[4], token_for(false, CLIENT_V4, CLIENT_PORT));
		build(add_case("0.6 accept with the right token", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY_VERIFIED, DDNET_XDP_VERDICT_PASS),
			false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aAccept, sizeof(s_aAccept), false);

		static uint8_t s_aAcceptBad[8];
		memcpy(s_aAcceptBad, s_aAccept, sizeof(s_aAcceptBad));
		s_aAcceptBad[7] ^= 1;
		build(add_case("0.6 accept with a wrong token", true, XDP_DROP, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_DROP),
			false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aAcceptBad, sizeof(s_aAcceptBad), false);

		// A vanilla client sends the same message without a token and must not be
		// mistaken for a forgery.
		static const uint8_t s_aVanillaAccept[4] = {4 << 2, 0, 0, 3};
		build(add_case("vanilla accept is only bounded", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
			false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aVanillaAccept, sizeof(s_aVanillaAccept), false);
	}

	// Without the offload both openers go to the server, under the handshake budget:
	// they carry no token yet by definition, and must not be mistaken for a forgery.
	build(add_case("0.7 token request passed to the server", true, XDP_PASS, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aTokenRequest, sizeof(s_aTokenRequest), false);
	without_offload();
	build(add_case("0.6 connect passed to the server", true, XDP_PASS, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aConnect, sizeof(s_aConnect), false);
	without_offload();
	// On a port that is not armed the server has not proven it uses the key, so an
	// answer from here would hand out tokens it has never heard of.
	build(add_case("no answer on a port not armed", false, XDP_PASS, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aTokenRequest, sizeof(s_aTokenRequest), false);

	// Whatever a master sends is never budgeted, not only the challenge.
	build(add_case("server info request from the master", true, XDP_PASS, DDNET_XDP_CLASS_MASTER, DDNET_XDP_VERDICT_PASS),
		false, MASTER_V4, 8283, TEST_PORT, s_aServerInfo, sizeof(s_aServerInfo), false);

	// An extension header in front of UDP must not be a way around the filter.
	s_NextExtension = 0; // hop-by-hop
	build(add_case("IPv6 hop-by-hop, then a real 0.7 packet", true, XDP_PASS, DDNET_XDP_CLASS_SIXUP, DDNET_XDP_VERDICT_PASS),
		true, CLIENT_V6, CLIENT_PORT, TEST_PORT, aSixupV6, sizeof(aSixupV6), false);
	s_NextExtension = 0;
	build(add_case("IPv6 hop-by-hop, then garbage", true, XDP_DROP, DDNET_XDP_CLASS_MALFORMED, DDNET_XDP_VERDICT_DROP),
		true, CLIENT_V6, CLIENT_PORT, TEST_PORT, s_aGarbage, sizeof(s_aGarbage), false);
	s_NextExtension = 44; // fragment
	build(add_case("IPv6 fragment", true, XDP_DROP, -1, 0),
		true, CLIENT_V6, CLIENT_PORT, TEST_PORT, s_aGarbage, sizeof(s_aGarbage), false);

	// Nothing this server speaks is ever fragmented.
	build(add_case("ipv4 fragment", true, XDP_DROP, -1, 0),
		false, CLIENT_V4, CLIENT_PORT, TEST_PORT, s_aLegacy, sizeof(s_aLegacy), true);
}

// Runs only with the connection table switched on. The first packet is the handshake
// that teaches the filter about this 4-tuple, the second is the compressed traffic
// that the filter can only place because of it.
static void build_tracking_cases(void)
{
	static uint8_t s_aVerified[16] = {0x00, 0, 1};
	static uint8_t s_aCompressed[16] = {0x80, 0, 1};

	write_be32(&s_aVerified[sizeof(s_aVerified) - 4], token_for(false, CLIENT_V4, CLIENT_PORT + 5));
	build(add_case("0.6 handshake teaches the connection", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY_VERIFIED, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT + 5, TEST_PORT, s_aVerified, sizeof(s_aVerified), false);
	build(add_case("0.6 compressed, now recognised", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY_TRACKED, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT + 5, TEST_PORT, s_aCompressed, sizeof(s_aCompressed), false);
	// A different source port is a different connection and was never taught.
	build(add_case("0.6 compressed from elsewhere", true, XDP_PASS, DDNET_XDP_CLASS_LEGACY, DDNET_XDP_VERDICT_PASS),
		false, CLIENT_V4, CLIENT_PORT + 6, TEST_PORT, s_aCompressed, sizeof(s_aCompressed), false);
}

// Passes everything libbpf has to say through, including the verifier log, which is
// the only useful thing to look at when a load fails.
static int print_libbpf(enum libbpf_print_level Level, const char *pFormat, va_list Args)
{
	(void)Level;
	return vfprintf(stderr, pFormat, Args);
}

int main(int argc, char **argv)
{
	const char *pObjectPath = argc > 1 ? argv[1] : "ddnet_xdp_kern.o";
	struct bpf_object *pObject;
	struct bpf_program *pProgram;
	int ProgramFd, ConfigMap, KeyMap, PortMap, MasterV4Map, StatsMap;
	int NumCpus = libbpf_num_possible_cpus();
	uint32_t Zero = 0, Epoch = TEST_EPOCH;
	struct ddnet_xdp_config Config;
	struct ddnet_xdp_key Key;
	struct ddnet_xdp_port Port;
	uint16_t PortNumber = TEST_PORT;
	struct in_addr MasterAddress;
	const uint8_t One = 1;

	libbpf_set_print(print_libbpf);

	printf("ddnet-xdp verify: loading %s\n", pObjectPath);
	fflush(stdout);
	pObject = bpf_object__open_file(pObjectPath, NULL);
	if(!pObject)
	{
		fprintf(stderr, "cannot open %s: %s\n", pObjectPath, strerror(errno));
		return 1;
	}
	pProgram = bpf_object__find_program_by_name(pObject, "ddnet_xdp_filter");
	if(!pProgram)
	{
		fprintf(stderr, "the object has no ddnet_xdp_filter program\n");
		return 1;
	}
	// Asks the verifier for its full reasoning rather than the last few lines, which
	// is the difference between a log that explains a rejection and one that does not.
	bpf_program__set_log_level(pProgram, 1);

	// The check a compiler cannot make: the verifier has to accept the program.
	if(bpf_object__load(pObject) != 0)
	{
		fprintf(stderr, "\nthe verifier rejected the program: %s\n", strerror(errno));
		fprintf(stderr, "the log above says why, its last lines are the ones that matter\n");
		return 1;
	}
	printf("  the verifier accepted the program\n\n");
	ProgramFd = bpf_program__fd(pProgram);
	ConfigMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_config");
	KeyMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_keys");
	PortMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_ports");
	MasterV4Map = bpf_object__find_map_fd_by_name(pObject, "ddnet_master_v4");
	StatsMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_stats");
	s_ConfigMap = ConfigMap;
	if(ConfigMap < 0 || KeyMap < 0 || PortMap < 0 || MasterV4Map < 0 || StatsMap < 0)
	{
		fprintf(stderr, "the object is missing a map\n");
		return 1;
	}

	// Budgets are switched off, so a verdict says something about the classification
	// rather than about how many packets happened to come before it.
	memset(&Config, 0, sizeof(Config));
	Config.m_PrefixV4 = 24;
	Config.m_PrefixV6 = 56;
	Config.m_Offload = 1;
	Config.m_CurrentEpoch = TEST_EPOCH;
	bpf_map_update_elem(ConfigMap, &Zero, &Config, BPF_ANY);

	memset(&Key, 0, sizeof(Key));
	Key.m_Key = TEST_KEY;
	Key.m_Valid = 1;
	bpf_map_update_elem(KeyMap, &Epoch, &Key, BPF_ANY);

	memset(&Port, 0, sizeof(Port));
	bpf_map_update_elem(PortMap, &PortNumber, &Port, BPF_ANY);
	inet_pton(AF_INET, MASTER_V4, &MasterAddress);
	bpf_map_update_elem(MasterV4Map, &MasterAddress.s_addr, &One, BPF_ANY);

	build_cases();
	printf("running %d cases against the loaded program\n", s_NumCases);
	for(int Index = 0; Index < s_NumCases; Index++)
		run(ProgramFd, PortMap, StatsMap, NumCpus, &s_aCases[Index]);

	// The connection table, if it was switched on: a packet that proved itself makes
	// the filter remember the 4-tuple, and a compressed one from the same place is
	// then recognised instead of falling into the shared budget.
	{
		struct ddnet_xdp_config Tracked;
		memset(&Tracked, 0, sizeof(Tracked));
		Tracked.m_PrefixV4 = 24;
		Tracked.m_PrefixV6 = 56;
		Tracked.m_ConnNsPerToken = 1000000;
		Tracked.m_ConnBurst = 64;
		Tracked.m_ConnIdleNs = 60000000000ULL;
		bpf_map_update_elem(ConfigMap, &Zero, &Tracked, BPF_ANY);

		s_NumCases = 0;
		build_tracking_cases();
		printf("\nwith the connection table on\n");
		for(int Index = 0; Index < s_NumCases; Index++)
			run(ProgramFd, PortMap, StatsMap, NumCpus, &s_aCases[Index]);
	}

	printf("\n%d of %d passed\n", s_Checks - s_Failures, s_Checks);
	bpf_object__close(pObject);
	return s_Failures == 0 ? 0 : 1;
}
