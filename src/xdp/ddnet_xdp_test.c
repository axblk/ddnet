// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/* Runs the vectors of the Rust classifier and the SipHash reference vectors against
 * the C code the XDP program is built from, and the token bucket arithmetic the
 * budgets rest on.
 *
 * The classifier has to agree with src/engine/shared/udp_port_mux_classifier.rs. If
 * it does not, a datagram is either handed to two consumers or to none, so the test
 * cases below are the ones from
 * `classifies_legacy_quic_collisions_without_dual_delivery`, kept in the same order.
 */
#include "ddnet_xdp_bucket.h"
#include "ddnet_xdp_classify.h"
#include "siphash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum route
{
	ROUTE_CONNECTIONLESS,
	ROUTE_LEGACY,
	ROUTE_QUIC,
	ROUTE_DROP,
};

static const char *route_name(enum route Route)
{
	switch(Route)
	{
	case ROUTE_CONNECTIONLESS: return "connectionless";
	case ROUTE_LEGACY: return "legacy";
	case ROUTE_QUIC: return "quic";
	default: return "drop";
	}
}

static const uint8_t CID[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t ROTATED_CID[8] = {8, 7, 6, 5, 4, 3, 2, 1};

typedef int (*validate_cid)(const uint8_t *pCid);

static int accepts_cid(const uint8_t *pCid) { return memcmp(pCid, CID, sizeof(CID)) == 0; }
static int accepts_rotated(const uint8_t *pCid) { return memcmp(pCid, ROTATED_CID, sizeof(ROTATED_CID)) == 0; }
static int accepts_both(const uint8_t *pCid) { return accepts_cid(pCid) || accepts_rotated(pCid); }

/* The same composition as `classify` in the Rust module, so the predicates are
 * exercised in the order the real decision uses them. */
static enum route classify(const uint8_t *pData, uint32_t Size, int KnownLegacyPeer, validate_cid pfnValidate)
{
	if(is_connectionless(pData, Size))
		return ROUTE_CONNECTIONLESS;
	if(Size == 0)
		return ROUTE_DROP;
	if((pData[0] & 0x80) && is_quic_long_header(pData, Size))
		return ROUTE_QUIC;
	if((pData[0] & QUIC_FIXED_BIT) && Size >= 1 + 8 && pfnValidate(&pData[1]))
		return ROUTE_QUIC;
	if(KnownLegacyPeer)
		return Size >= LEGACY_PACKET_HEADER_SIZE && Size <= LEGACY_MAX_PACKET_SIZE ? ROUTE_LEGACY : ROUTE_DROP;
	if(pData[0] & QUIC_FIXED_BIT)
		return ROUTE_DROP;
	return is_legacy_packet(pData, Size) ? ROUTE_LEGACY : ROUTE_DROP;
}

static int s_Failures;

static void expect(const char *pName, enum route Got, enum route Want)
{
	if(Got == Want)
		return;
	printf("  %-42s got %-15s want %s\n", pName, route_name(Got), route_name(Want));
	s_Failures++;
}

#define EXPECT(name, data, size, known, validate, want) \
	expect(name, classify(data, size, known, validate), want)

static void test_classifier(void)
{
	static const uint8_t s_aAllOnes[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	static const uint8_t s_aExtended[6] = {'x', 'e', 0, 0, 0, 0};
	static const uint8_t s_aSixupConnless[9] = {0x21, 0, 0, 0, 0, 0, 0, 0, 0};
	static const uint8_t s_a40[3] = {0x40, 0, 0};
	static const uint8_t s_aC0[3] = {0xc0, 0, 0};
	static const uint8_t s_aSevenResend[7] = {0x08, 0, 0, 0, 0, 0, 0};
	static const uint8_t s_aSevenCompressed[8] = {0x10, 0, 1, 0, 0, 0, 0, 1};
	static const uint8_t s_aChunked[4] = {0x10, 0, 0, 1};
	uint8_t aShort[10], aRotatedShort[10], aInitial[32], aLong[32];
	uint32_t InitialSize, LongSize;

	EXPECT("all ones header", s_aAllOnes, sizeof(s_aAllOnes), 0, accepts_cid, ROUTE_CONNECTIONLESS);
	EXPECT("extended connless", s_aExtended, sizeof(s_aExtended), 0, accepts_cid, ROUTE_CONNECTIONLESS);
	EXPECT("0.7 connless", s_aSixupConnless, sizeof(s_aSixupConnless), 0, accepts_cid, ROUTE_CONNECTIONLESS);
	EXPECT("0x40, known peer", s_a40, sizeof(s_a40), 1, accepts_cid, ROUTE_LEGACY);
	EXPECT("0x40, unknown peer", s_a40, sizeof(s_a40), 0, accepts_cid, ROUTE_DROP);
	EXPECT("0xc0, known peer", s_aC0, sizeof(s_aC0), 1, accepts_cid, ROUTE_LEGACY);
	EXPECT("0xc0, unknown peer", s_aC0, sizeof(s_aC0), 0, accepts_cid, ROUTE_DROP);

	aShort[0] = QUIC_FIXED_BIT;
	memcpy(&aShort[1], CID, sizeof(CID));
	aShort[9] = 0;
	EXPECT("short header, valid cid", aShort, sizeof(aShort), 0, accepts_cid, ROUTE_QUIC);
	aShort[1] ^= 1;
	EXPECT("short header, broken cid", aShort, sizeof(aShort), 0, accepts_cid, ROUTE_DROP);
	aShort[1] ^= 1;

	aRotatedShort[0] = QUIC_FIXED_BIT;
	memcpy(&aRotatedShort[1], ROTATED_CID, sizeof(ROTATED_CID));
	aRotatedShort[9] = 0;
	EXPECT("short header during rotation", aShort, sizeof(aShort), 1, accepts_both, ROUTE_QUIC);
	EXPECT("rotated cid during rotation", aRotatedShort, sizeof(aRotatedShort), 1, accepts_both, ROUTE_QUIC);
	EXPECT("retired cid, unknown peer", aShort, sizeof(aShort), 0, accepts_rotated, ROUTE_DROP);
	EXPECT("retired cid, known peer", aShort, sizeof(aShort), 1, accepts_rotated, ROUTE_LEGACY);

	EXPECT("0.7 resend, known peer", s_aSevenResend, sizeof(s_aSevenResend), 1, accepts_cid, ROUTE_LEGACY);
	EXPECT("0.7 compressed, known peer", s_aSevenCompressed, sizeof(s_aSevenCompressed), 1, accepts_cid, ROUTE_LEGACY);
	EXPECT("0.7 resend, unknown peer", s_aSevenResend, sizeof(s_aSevenResend), 0, accepts_cid, ROUTE_DROP);
	EXPECT("0.7 compressed, unknown peer", s_aSevenCompressed, sizeof(s_aSevenCompressed), 0, accepts_cid, ROUTE_DROP);

	InitialSize = 0;
	aInitial[InitialSize++] = 0xc0;
	aInitial[InitialSize++] = 0;
	aInitial[InitialSize++] = 0;
	aInitial[InitialSize++] = 0;
	aInitial[InitialSize++] = 1;
	aInitial[InitialSize++] = 8;
	memset(&aInitial[InitialSize], 1, 8);
	InitialSize += 8;
	aInitial[InitialSize++] = 8;
	memset(&aInitial[InitialSize], 2, 8);
	InitialSize += 8;
	aInitial[InitialSize++] = 0;
	aInitial[InitialSize++] = 1;
	aInitial[InitialSize++] = 0;
	EXPECT("initial, unknown peer", aInitial, InitialSize, 0, accepts_cid, ROUTE_QUIC);
	EXPECT("initial, known peer", aInitial, InitialSize, 1, accepts_cid, ROUTE_QUIC);
	aInitial[4] = 2;
	EXPECT("initial, unknown version", aInitial, InitialSize, 0, accepts_cid, ROUTE_QUIC);
	aInitial[5] = 21;
	EXPECT("initial, oversized cid", aInitial, InitialSize, 0, accepts_cid, ROUTE_DROP);
	{
		static const uint8_t s_aTruncated[7] = {0xc0, 0, 0, 0, 1, 0, 0};
		EXPECT("initial, truncated", s_aTruncated, sizeof(s_aTruncated), 0, accepts_cid, ROUTE_DROP);
	}

	for(int Variant = 0; Variant < 2; Variant++)
	{
		LongSize = 0;
		aLong[LongSize++] = Variant == 0 ? 0xe0 : 0xf0;
		aLong[LongSize++] = 0;
		aLong[LongSize++] = 0;
		aLong[LongSize++] = 0;
		aLong[LongSize++] = 1;
		aLong[LongSize++] = 8;
		memcpy(&aLong[LongSize], CID, sizeof(CID));
		LongSize += 8;
		aLong[LongSize++] = 8;
		memcpy(&aLong[LongSize], ROTATED_CID, sizeof(ROTATED_CID));
		LongSize += 8;
		aLong[LongSize++] = 0;
		EXPECT(Variant == 0 ? "handshake" : "retry", aLong, LongSize, 1, accepts_cid, ROUTE_QUIC);
	}

	EXPECT("legacy with chunks", s_aChunked, sizeof(s_aChunked), 0, accepts_cid, ROUTE_LEGACY);
	EXPECT("empty", s_aChunked, 0, 0, accepts_cid, ROUTE_DROP);
}

static void test_siphash(void)
{
	/* Reference vectors from the SipHash paper: key 000102..0f, input 00 01 .. */
	static const uint64_t s_aExpected[16] = {
		0x726fdb47dd0e0e31ULL,
		0x74f839c593dc67fdULL,
		0x0d6c8009d9a94f5aULL,
		0x85676696d7fb7e2dULL,
		0xcf2794e0277187b7ULL,
		0x18765564cd99a68dULL,
		0xcbc9466e58fee3ceULL,
		0xab0200f58b01d137ULL,
		0x93f5f5799a932462ULL,
		0x9e0082df0ba9e4b0ULL,
		0x7a5dbbc594ddb9f3ULL,
		0xf4b32f46226bada7ULL,
		0x751e8fbc860ee5fbULL,
		0x14ea5627c0843d90ULL,
		0xf723ca908e7af2eeULL,
		0xa129ca6149be45e5ULL,
	};
	const struct siphash_key Key = {0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};
	uint8_t aInput[16];

	for(unsigned Index = 0; Index < sizeof(aInput); Index++)
		aInput[Index] = (uint8_t)Index;
	for(unsigned Index = 0; Index < 16; Index++)
	{
		const uint64_t Got = siphash24(&Key, aInput, Index);
		if(Got == s_aExpected[Index])
			continue;
		printf("  siphash len %2u: got %016llx want %016llx\n", Index,
			(unsigned long long)Got, (unsigned long long)s_aExpected[Index]);
		s_Failures++;
	}
}

static void check(const char *pName, int Condition)
{
	if(Condition)
		return;
	printf("  bucket: %s\n", pName);
	s_Failures++;
}

/* The arithmetic every budget rests on. The same header the XDP program is built
 * from, so what passes here is what the filter does, short of the map lookups. */
static void test_buckets(void)
{
	struct ddnet_xdp_bucket Bucket = {0, 0};
	const uint64_t NsPerToken = 1000;
	const uint64_t Burst = 3;
	uint64_t Now = 5000;

	/* Unlimited means untouched: not even a refill. */
	check("unlimited passes", take_token(&Bucket, Now, 0, 0));
	check("unlimited leaves the bucket alone", Bucket.m_LastNs == 0 && Bucket.m_Tokens == 0);

	/* A fresh bucket starts full and hands out exactly its burst. */
	check("burst 1", take_token(&Bucket, Now, NsPerToken, Burst));
	check("burst 2", take_token(&Bucket, Now, NsPerToken, Burst));
	check("burst 3", take_token(&Bucket, Now, NsPerToken, Burst));
	check("burst exhausted", !take_token(&Bucket, Now, NsPerToken, Burst));

	/* One and a half tokens' worth of time gives one token, and the half is kept
	 * rather than lost, so half a token later the next one is there. */
	Now += 1500;
	check("one token after 1.5", take_token(&Bucket, Now, NsPerToken, Burst));
	check("not two", !take_token(&Bucket, Now, NsPerToken, Burst));
	Now += 500;
	check("the remainder was kept", take_token(&Bucket, Now, NsPerToken, Burst));
	check("and nothing beyond it", !take_token(&Bucket, Now, NsPerToken, Burst));

	/* A long pause refills to the burst and no further. */
	Now += 1000 * 1000;
	check("refilled to the burst", Bucket.m_Tokens == 0);
	check("full again 1", take_token(&Bucket, Now, NsPerToken, Burst));
	check("full again 2", take_token(&Bucket, Now, NsPerToken, Burst));
	check("full again 3", take_token(&Bucket, Now, NsPerToken, Burst));
	check("capped at the burst", !take_token(&Bucket, Now, NsPerToken, Burst));

	/* A clock that went backwards starts the bucket over instead of stalling it. */
	Now = 10;
	check("clock went backwards", take_token(&Bucket, Now, NsPerToken, Burst));
	check("and the bucket started over", Bucket.m_LastNs == Now && Bucket.m_Tokens == Burst - 1);

	{
		/* The pair: a prefix bucket and a port bucket, and a packet is charged to both
		 * or to neither. */
		struct ddnet_xdp_bucket Prefix = {0, 0}, Port = {0, 0};
		const struct ddnet_xdp_budget PrefixBudget = {NsPerToken, 2};
		const struct ddnet_xdp_budget PortBudget = {NsPerToken, 1};
		const struct ddnet_xdp_budget Unlimited = {0, 0};
		Now = 5000;
		check("pair 1", take_token_pair(&Prefix, &PrefixBudget, &Port, &PortBudget, Now));
		check("both charged", Prefix.m_Tokens == 1 && Port.m_Tokens == 0);
		check("port cap refuses", !take_token_pair(&Prefix, &PrefixBudget, &Port, &PortBudget, Now));
		check("the prefix was not charged for it", Prefix.m_Tokens == 1);
		check("unlimited port, prefix decides", take_token_pair(&Prefix, &PrefixBudget, &Port, &Unlimited, Now));
		check("port bucket left alone", Port.m_Tokens == 0);
		check("prefix refuses", !take_token_pair(&Prefix, &PrefixBudget, &Port, &Unlimited, Now));
		check("both unlimited", take_token_pair(&Prefix, &Unlimited, &Port, &Unlimited, Now));
		check("neither touched", Prefix.m_Tokens == 0 && Port.m_Tokens == 0);
		Now += NsPerToken;
		check("both refilled", take_token_pair(&Prefix, &PrefixBudget, &Port, &PortBudget, Now));
		check("and both charged again", Prefix.m_Tokens == 0 && Port.m_Tokens == 0);
	}
}

int main(void)
{
	printf("ddnet-xdp self test\n");
	test_siphash();
	test_classifier();
	test_buckets();
	if(s_Failures == 0)
		printf("  all checks passed\n");
	else
		printf("  %d failed\n", s_Failures);
	return s_Failures == 0 ? 0 : 1;
}
