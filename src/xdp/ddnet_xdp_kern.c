// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/* XDP filter for the UDP port a DDNet server shares between the 0.6/0.7 protocol,
 * QUIC and WebTransport.
 *
 * The filter holds no per-connection state. Everything it lets through, it lets
 * through because the packet carries a value only the server could have handed out:
 * the 0.7 security token or a QUIC connection ID, both derived with SipHash-2-4 from
 * a key the server and this program share. An off-path source cannot produce either,
 * so spoofed traffic never reaches the socket, while a client that migrates to a new
 * address keeps working because its identity travels in the packet and not in a
 * table here.
 *
 * What cannot be verified is bounded instead: 0.6 keeps its token inside the
 * compressed payload, and 0.6 connless has none at all.
 */
// The order matters and must not be sorted: bpf_helpers.h uses __u32 and __u64
// without including anything that declares them.
// clang-format off
#include <linux/types.h>

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
// clang-format on

#include "ddnet_xdp_classify.h"
#include "ddnet_xdp_shared.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define DDNET_XDP_MAX_VLAN 2
/* Covers the furthest byte any classifier read reaches: a long header with two
 * maximum length connection IDs puts the token length field at offset 47. */
#define DDNET_XDP_HEAD_SIZE 48

/* The two handshake openers, the packets with which a client asks for a token. They
 * are the only ones of their protocol that carry none yet, and both are answered
 * with a twelve byte control packet. */
#define SIXUP_CTRL_TOKEN 5
#define SIXUP_TOKEN_REQUEST_SIZE (SIXUP_PACKET_HEADER_SIZE + 512)
#define LEGACY_CTRL_CONNECT 1
#define LEGACY_CTRL_CONNECTACCEPT 2
#define LEGACY_CTRL_ACCEPT 3
#define LEGACY_ACCEPT_SIZE (LEGACY_PACKET_HEADER_SIZE + 1 + 4)
#define HANDSHAKE_REPLY_SIZE 12
#define ETH_ALEN 6

struct
{
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct ddnet_xdp_config);
} ddnet_config SEC(".maps");

struct
{
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, DDNET_XDP_KEY_EPOCHS);
	__type(key, __u32);
	__type(value, struct ddnet_xdp_key);
} ddnet_keys SEC(".maps");

struct
{
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u16);
	__type(value, struct ddnet_xdp_port);
} ddnet_ports SEC(".maps");

struct
{
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u8);
} ddnet_master_v4 SEC(".maps");

struct
{
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, struct in6_addr);
	__type(value, __u8);
} ddnet_master_v6 SEC(".maps");

struct
{
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, DDNET_XDP_STATS_ENTRIES);
	__type(key, __u32);
	__type(value, __u64);
} ddnet_stats SEC(".maps");

/* One row of prefix buckets per budget. */
struct
{
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, DDNET_XDP_NUM_BUDGETS *DDNET_XDP_PREFIX_BUCKETS);
	__type(key, __u32);
	__type(value, struct ddnet_xdp_bucket);
} ddnet_budgets SEC(".maps");

/* Established 0.6 connections, learned by the filter itself. A packet whose token
 * checks out proves that its source received what the server sent back, so the entry
 * cannot be created off path. It is soft state: losing one only drops that connection
 * back to the shared budget, so an LRU that forgets under pressure is fine. */
struct
{
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 16384);
	__type(key, struct ddnet_xdp_conn_key);
	__type(value, struct ddnet_xdp_bucket);
} ddnet_conns SEC(".maps");

struct ddnet_source
{
	__u8 m_Family;
	__u8 m_aAddress[16];
	__u16 m_Port;
	__u16 m_DestinationPort;
	__u32 m_PrefixHash;
};

static __always_inline void conn_key(const struct ddnet_source *pSource, struct ddnet_xdp_conn_key *pKey)
{
	__builtin_memset(pKey, 0, sizeof(*pKey));
	pKey->m_Family = pSource->m_Family;
	pKey->m_SourcePort = pSource->m_Port;
	pKey->m_DestinationPort = pSource->m_DestinationPort;
	__builtin_memcpy(pKey->m_aAddress, pSource->m_aAddress, sizeof(pKey->m_aAddress));
}

static __always_inline void count(__u32 Port, __u32 Class, __u32 Verdict)
{
	const __u32 Index = DDNET_XDP_STATS_INDEX(Port, Class, Verdict);
	__u64 *pCounter = bpf_map_lookup_elem(&ddnet_stats, &Index);
	if(pCounter)
		(*pCounter)++;
}

/* Refills and takes one token. `NsPerToken` of zero means the class is unlimited. */
static __always_inline int take_token(struct ddnet_xdp_bucket *pBucket, __u64 Now, __u64 NsPerToken, __u64 Burst)
{
	if(NsPerToken == 0)
		return 1;
	if(pBucket->m_LastNs == 0 || pBucket->m_LastNs > Now)
	{
		pBucket->m_LastNs = Now;
		pBucket->m_Tokens = Burst;
	}
	const __u64 Elapsed = Now - pBucket->m_LastNs;
	const __u64 Refill = Elapsed / NsPerToken;
	if(Refill > 0)
	{
		/* Advancing by whole tokens instead of to `Now` keeps the remainder, so a
		 * slow bucket does not lose time on every packet and stall forever. */
		pBucket->m_LastNs += Refill * NsPerToken;
		pBucket->m_Tokens = pBucket->m_Tokens + Refill > Burst ? Burst : pBucket->m_Tokens + Refill;
	}
	if(pBucket->m_Tokens == 0)
		return 0;
	pBucket->m_Tokens--;
	return 1;
}

static __always_inline int take_budget(__u32 Budget, __u32 PrefixHash, const struct ddnet_xdp_config *pConfig)
{
	/* Spreading sources over buckets means an attacker holding few prefixes can only
	 * empty the buckets those prefixes land in, and a player from an uninvolved
	 * prefix still finds a full one. */
	const __u32 Index = (Budget % DDNET_XDP_NUM_BUDGETS) * DDNET_XDP_PREFIX_BUCKETS +
			    PrefixHash % DDNET_XDP_PREFIX_BUCKETS;
	struct ddnet_xdp_bucket *pBucket = bpf_map_lookup_elem(&ddnet_budgets, &Index);
	if(!pBucket || Budget >= DDNET_XDP_NUM_BUDGETS)
		return 0;
	return take_token(pBucket, bpf_ktime_get_ns(),
		pConfig->m_aBudgets[Budget % DDNET_XDP_NUM_BUDGETS].m_NsPerToken,
		pConfig->m_aBudgets[Budget % DDNET_XDP_NUM_BUDGETS].m_Burst);
}

/* Builds the canonical token input described in ddnet_xdp_shared.h. */
static __always_inline __u32 token_input(const struct ddnet_source *pSource, __u8 *pOut)
{
	pOut[0] = pSource->m_Family;
	if(pSource->m_Family == DDNET_XDP_FAMILY_IPV4)
	{
		__builtin_memcpy(&pOut[1], pSource->m_aAddress, 4);
		pOut[5] = (__u8)(pSource->m_Port >> 8);
		pOut[6] = (__u8)pSource->m_Port;
		return DDNET_XDP_TOKEN_INPUT_V4;
	}
	__builtin_memcpy(&pOut[1], pSource->m_aAddress, 16);
	pOut[17] = (__u8)(pSource->m_Port >> 8);
	pOut[18] = (__u8)pSource->m_Port;
	return DDNET_XDP_TOKEN_INPUT_MAX;
}

/* The hash is called at its call sites rather than inlined into them.
 *
 * Inlined, every copy sits at its own instruction index, and nothing the verifier
 * learns about one carries over to the next: it walks all of them along every path
 * that reaches them. One shared body it can prune against itself.
 *
 * The length has to reach the rounds as a constant for their loops to unroll, so
 * there is an entry point per length instead of a length argument. A length the
 * verifier cannot pin down turns them into loops it has to prove terminate, over a
 * buffer it then cannot bound. */
__attribute__((noinline)) static __u32 token_hash_v4(const struct siphash_key *pKey, const __u8 *pInput)
{
	return (__u32)siphash24(pKey, pInput, DDNET_XDP_TOKEN_INPUT_V4);
}

__attribute__((noinline)) static __u32 token_hash_full(const struct siphash_key *pKey, const __u8 *pInput)
{
	return (__u32)siphash24(pKey, pInput, DDNET_XDP_TOKEN_INPUT_MAX);
}

__attribute__((noinline)) static __u32 cid_hash(const struct siphash_key *pKey, const __u8 *pInput)
{
	return (__u32)siphash24(pKey, pInput, DDNET_XDP_CID_TAG_INPUT);
}

/* The tokens one source has under every epoch that currently holds a key.
 *
 * The classifier asks whether a token is one of ours in more than one place, and
 * between them the answer cannot change: it is the same packet's address every
 * time. So it is derived once and compared afterwards, rather than hashed again for
 * every question. */
struct ddnet_tokens
{
	__u32 m_aToken[DDNET_XDP_KEY_EPOCHS];
	__u8 m_aValid[DDNET_XDP_KEY_EPOCHS];
};

/* Returns the token of one epoch, and says through `pValid` whether that epoch
 * holds a key at all.
 *
 * Written per epoch rather than as a loop, so the caller can spell the epochs out
 * one by one: a loop counter whose address is handed to a map lookup lives on the
 * stack, and the verifier loses its bounds there and cannot see the loop end. For
 * the same reason the epoch does not choose the slot in here: it arrives through
 * the stack the lookup read it from, where the verifier no longer knows its value,
 * and indexing with it becomes a write it cannot place. The caller stores, and
 * there the epoch is a constant. */
static __always_inline __u32 derive_epoch(__u32 Epoch, const __u8 *pInput, __u32 Size, __u8 *pValid)
{
	const struct ddnet_xdp_key *pKey = bpf_map_lookup_elem(&ddnet_keys, &Epoch);

	if(!pKey || !pKey->m_Valid)
	{
		*pValid = 0;
		return 0;
	}
	*pValid = 1;
	return Size == DDNET_XDP_TOKEN_INPUT_V4 ? token_hash_v4(&pKey->m_Key, pInput) : token_hash_full(&pKey->m_Key, pInput);
}

#define DERIVE_EPOCHS(SIZE) \
	do \
	{ \
		pOut->m_aToken[0] = derive_epoch(0, pInput, (SIZE), &pOut->m_aValid[0]); \
		pOut->m_aToken[1] = derive_epoch(1, pInput, (SIZE), &pOut->m_aValid[1]); \
		pOut->m_aToken[2] = derive_epoch(2, pInput, (SIZE), &pOut->m_aValid[2]); \
		pOut->m_aToken[3] = derive_epoch(3, pInput, (SIZE), &pOut->m_aValid[3]); \
	} while(0)

static __always_inline void derive_epochs(const __u8 *pInput, __u32 Size, struct ddnet_tokens *pOut)
{
	if(Size == DDNET_XDP_TOKEN_INPUT_V4)
		DERIVE_EPOCHS(DDNET_XDP_TOKEN_INPUT_V4);
	else
		DERIVE_EPOCHS(DDNET_XDP_TOKEN_INPUT_MAX);
}

/* Two epochs overlap during a rotation, otherwise every connection would break
 * whenever the key changes, so all of them are derived and any of them may match. */
static __always_inline void derive_tokens(const struct ddnet_source *pSource, struct ddnet_tokens *pOut)
{
	__u8 aInput[DDNET_XDP_TOKEN_INPUT_MAX] = {};
	const __u32 Size = token_input(pSource, aInput);

	derive_epochs(aInput, Size, pOut);
}

static __always_inline int token_is(const struct ddnet_tokens *pTokens, __u32 Token)
{
	return (pTokens->m_aValid[0] && pTokens->m_aToken[0] == Token) ||
	       (pTokens->m_aValid[1] && pTokens->m_aToken[1] == Token) ||
	       (pTokens->m_aValid[2] && pTokens->m_aToken[2] == Token) ||
	       (pTokens->m_aValid[3] && pTokens->m_aToken[3] == Token);
}

/* The number the server derives for this address, which is what both handshakes ask
 * for. Stamped with the current epoch; the previous one stays valid, so a token
 * handed out just before a rotation keeps working after it. */
static __always_inline __u32 token_for(const struct ddnet_source *pSource, __u32 Epoch)
{
	__u8 aInput[DDNET_XDP_TOKEN_INPUT_MAX] = {};
	const __u32 Size = token_input(pSource, aInput);
	const struct ddnet_xdp_key *pKey;

	if(Epoch >= DDNET_XDP_KEY_EPOCHS)
		return 0;
	pKey = bpf_map_lookup_elem(&ddnet_keys, &Epoch);
	if(!pKey || !pKey->m_Valid)
		return 0;
	if(Size == DDNET_XDP_TOKEN_INPUT_V4)
		return token_hash_v4(&pKey->m_Key, aInput);
	return token_hash_full(&pKey->m_Key, aInput);
}

/* The global token is the derivation over an all-zero address, the same value
 * `CNetServer::GetGlobalToken()` produces. */
static __always_inline int global_token_valid(__u32 Token)
{
	__u8 aInput[DDNET_XDP_TOKEN_INPUT_MAX] = {};
	struct ddnet_tokens Tokens = {};

	aInput[0] = DDNET_XDP_FAMILY_IPV4;
	derive_epochs(aInput, DDNET_XDP_TOKEN_INPUT_V4, &Tokens);
	return token_is(&Tokens, Token);
}

/* A connection ID this endpoint handed out. Layout, following QUIC-LB:
 *
 *     byte 0, bits 7..5 : key epoch (config rotation)
 *     byte 0, bits 4..0 and bytes 1..4 : nonce
 *     bytes 4..8        : SipHash-2-4 over bytes 0..4, truncated to 32 bits
 */
static __always_inline int cid_valid(const __u8 *pCid)
{
	__u32 Epoch = pCid[0] >> 5;
	const struct ddnet_xdp_key *pKey;
	__u8 aInput[DDNET_XDP_CID_TAG_INPUT];
	__u32 Tag;

	if(Epoch >= DDNET_XDP_KEY_EPOCHS)
		return 0;
	pKey = bpf_map_lookup_elem(&ddnet_keys, &Epoch);
	if(!pKey || !pKey->m_Valid)
		return 0;
	__builtin_memcpy(aInput, pCid, sizeof(aInput));
	Tag = cid_hash(&pKey->m_Key, aInput);
	return pCid[4] == (__u8)(Tag >> 24) && pCid[5] == (__u8)(Tag >> 16) &&
	       pCid[6] == (__u8)(Tag >> 8) && pCid[7] == (__u8)Tag;
}

static __always_inline void write_be32(__u8 *pOut, __u32 Value)
{
	pOut[0] = (__u8)(Value >> 24);
	pOut[1] = (__u8)(Value >> 16);
	pOut[2] = (__u8)(Value >> 8);
	pOut[3] = (__u8)Value;
}

static __always_inline __u32 read_be32(const __u8 *pData)
{
	return ((__u32)pData[0] << 24) | ((__u32)pData[1] << 16) | ((__u32)pData[2] << 8) | pData[3];
}

struct ddnet_decision
{
	__u32 m_Class;
	__u8 m_Pass;
	__s8 m_Budget; /* one of DDNET_XDP_BUDGET_*, or -1 for none */
	__u8 m_Verified; /* carried a token or connection ID this host issued */
	__u8 m_Track; /* 1 remember this connection, 2 look it up */
};

static __always_inline int master_allowed(const struct ddnet_source *pSource)
{
	if(pSource->m_Family == DDNET_XDP_FAMILY_IPV4)
	{
		__u32 Address;
		__builtin_memcpy(&Address, pSource->m_aAddress, 4);
		return bpf_map_lookup_elem(&ddnet_master_v4, &Address) != NULL;
	}
	struct in6_addr Address;
	__builtin_memcpy(&Address, pSource->m_aAddress, 16);
	return bpf_map_lookup_elem(&ddnet_master_v6, &Address) != NULL;
}

/* Ones complement sum, folded to sixteen bits. `Size` has to be an even constant:
 * everything summed here is a header or the fixed reply, and a variable length would
 * put a variable index on a packet pointer, which is the one thing the verifier will
 * not follow. */
static __always_inline __u32 checksum_add(const __u8 *pData, __u32 Size, __u32 Sum)
{
	__u32 Index;
#pragma unroll
	for(Index = 0; Index + 1 < Size; Index += 2)
		Sum += ((__u32)pData[Index] << 8) | pData[Index + 1];
	return Sum;
}

static __always_inline __u16 checksum_fold(__u32 Sum)
{
	Sum = (Sum & 0xffff) + (Sum >> 16);
	Sum = (Sum & 0xffff) + (Sum >> 16);
	return (__u16)~Sum;
}

/* Turns the packet around: the reply goes back out of the interface it arrived on,
 * to the address it claims to come from. A spoofed source therefore never receives
 * what it asked for, which is the entire point of a token handshake.
 *
 * Every bound below is a constant. After a tail adjustment the verifier knows
 * nothing about the packet any more, and a length derived from a pointer difference
 * does not give it back: it only tracks the lower half of one, so a check against it
 * proves nothing. The two families are written out separately for that reason.
 */
#define REPLY_V4_SIZE (sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + HANDSHAKE_REPLY_SIZE)
#define REPLY_V6_SIZE (sizeof(struct ethhdr) + sizeof(struct ipv6hdr) + sizeof(struct udphdr) + HANDSHAKE_REPLY_SIZE)

static __always_inline void swap_mac(__u8 *pEth)
{
	__u8 aMac[ETH_ALEN];
	__builtin_memcpy(aMac, pEth, ETH_ALEN);
	__builtin_memcpy(pEth, pEth + ETH_ALEN, ETH_ALEN);
	__builtin_memcpy(pEth + ETH_ALEN, aMac, ETH_ALEN);
}

static __always_inline int send_reply_v4(struct xdp_md *pCtx, const __u8 *pReply)
{
	__u8 *pData;
	struct iphdr *pIp;
	struct udphdr *pUdp;
	__u32 Address, Sum;
	__u16 Port;

	if(bpf_xdp_adjust_tail(pCtx, (int)REPLY_V4_SIZE - (int)(pCtx->data_end - pCtx->data)) != 0)
		return XDP_DROP;
	pData = (__u8 *)(long)pCtx->data;
	if((void *)(pData + REPLY_V4_SIZE) > (void *)(long)pCtx->data_end)
		return XDP_DROP;

	swap_mac(pData);
	pIp = (struct iphdr *)(pData + sizeof(struct ethhdr));
	Address = pIp->saddr;
	pIp->saddr = pIp->daddr;
	pIp->daddr = Address;
	pIp->tot_len = bpf_htons((__u16)(sizeof(*pIp) + sizeof(*pUdp) + HANDSHAKE_REPLY_SIZE));
	pIp->ttl = 64;
	pIp->frag_off = 0;
	pIp->check = 0;
	Sum = checksum_add((const __u8 *)pIp, sizeof(*pIp), 0);
	pIp->check = bpf_htons(checksum_fold(Sum));

	pUdp = (struct udphdr *)(pIp + 1);
	Port = pUdp->source;
	pUdp->source = pUdp->dest;
	pUdp->dest = Port;
	pUdp->len = bpf_htons((__u16)(sizeof(*pUdp) + HANDSHAKE_REPLY_SIZE));
	/* Optional over IPv4, and leaving it out keeps this path short. */
	pUdp->check = 0;
	__builtin_memcpy(pUdp + 1, pReply, HANDSHAKE_REPLY_SIZE);
	return XDP_TX;
}

static __always_inline int send_reply_v6(struct xdp_md *pCtx, const __u8 *pReply)
{
	__u8 *pData;
	struct ipv6hdr *pIp;
	struct udphdr *pUdp;
	struct in6_addr Address;
	__u32 Sum;
	__u16 Port;

	if(bpf_xdp_adjust_tail(pCtx, (int)REPLY_V6_SIZE - (int)(pCtx->data_end - pCtx->data)) != 0)
		return XDP_DROP;
	pData = (__u8 *)(long)pCtx->data;
	if((void *)(pData + REPLY_V6_SIZE) > (void *)(long)pCtx->data_end)
		return XDP_DROP;

	swap_mac(pData);
	pIp = (struct ipv6hdr *)(pData + sizeof(struct ethhdr));
	Address = pIp->saddr;
	pIp->saddr = pIp->daddr;
	pIp->daddr = Address;
	pIp->payload_len = bpf_htons((__u16)(sizeof(*pUdp) + HANDSHAKE_REPLY_SIZE));
	pIp->hop_limit = 64;

	pUdp = (struct udphdr *)(pIp + 1);
	Port = pUdp->source;
	pUdp->source = pUdp->dest;
	pUdp->dest = Port;
	pUdp->len = bpf_htons((__u16)(sizeof(*pUdp) + HANDSHAKE_REPLY_SIZE));
	pUdp->check = 0;
	__builtin_memcpy(pUdp + 1, pReply, HANDSHAKE_REPLY_SIZE);
	/* IPv6 has no header checksum but demands the UDP one, so it is built here from
	 * the pseudo header, the UDP header and the reply. */
	Sum = checksum_add((const __u8 *)&pIp->saddr, 32, 0);
	Sum += (__u32)(sizeof(*pUdp) + HANDSHAKE_REPLY_SIZE);
	Sum += IPPROTO_UDP;
	Sum = checksum_add((const __u8 *)pUdp, sizeof(*pUdp), Sum);
	Sum = checksum_add(pReply, HANDSHAKE_REPLY_SIZE, Sum);
	pUdp->check = bpf_htons(checksum_fold(Sum));
	if(pUdp->check == 0)
		pUdp->check = 0xffff;
	return XDP_TX;
}

/* Aggregates a source to its network prefix, so that neighbouring addresses share a
 * budget and an attacker gains nothing by walking through a range he controls. */
static __always_inline __u32 prefix_hash(const struct ddnet_source *pSource, const struct ddnet_xdp_config *pConfig)
{
	if(pSource->m_Family == DDNET_XDP_FAMILY_IPV4)
	{
		const __u32 Bits = pConfig->m_PrefixV4 > 32 ? 32 : pConfig->m_PrefixV4;
		const __u32 Address = read_be32(pSource->m_aAddress);
		return Bits == 0 ? 0 : (Bits >= 32 ? Address : (Address >> (32 - Bits)));
	}
	{
		/* Allocation boundaries in IPv6 are at /48 to /64, all inside the first eight
		 * bytes, so the rest of the address says nothing about who is responsible. */
		const __u32 Bits = pConfig->m_PrefixV6 > 64 ? 64 : pConfig->m_PrefixV6;
		const __u64 High = ((__u64)read_be32(pSource->m_aAddress) << 32) | read_be32(&pSource->m_aAddress[4]);
		const __u64 Prefix = Bits == 0 ? 0 : (Bits >= 64 ? High : (High >> (64 - Bits)));
		return (__u32)(Prefix ^ (Prefix >> 32));
	}
}

/* The 0.7 token request: a control packet whose payload is padded to 512 bytes and
 * carries the token the client wants its answer addressed with. */
static __always_inline int is_sixup_token_request(const __u8 *pData, __u32 Size)
{
	return Size >= SIXUP_TOKEN_REQUEST_SIZE &&
	       pData[0] == ((SIXUP_FLAG_CONTROL) << 2) &&
	       pData[2] == 0 &&
	       pData[SIXUP_PACKET_HEADER_SIZE] == SIXUP_CTRL_TOKEN;
}

/* The 0.6 DDNet connect: a control packet carrying the magic that says this client
 * understands security tokens. Vanilla 0.6 has no magic and no token. */
static __always_inline int is_legacy_connect(const __u8 *pData, __u32 Size)
{
	return Size >= LEGACY_PACKET_HEADER_SIZE + 1 + 4 + 4 &&
	       (pData[0] >> 2) == LEGACY_FLAG_CONTROL &&
	       pData[2] == 0 &&
	       pData[LEGACY_PACKET_HEADER_SIZE] == LEGACY_CTRL_CONNECT &&
	       pData[4] == 'T' && pData[5] == 'K' && pData[6] == 'E' && pData[7] == 'N';
}

/* Called rather than inlined, and that is what keeps the program verifiable.
 *
 * Inlined, every way out of the classifier is a state of its own, each carrying its
 * own constants, and the verifier follows all of them through everything that comes
 * after: the budgets, the counters. Called, the frame goes away at the return and
 * they are one state again.
 *
 * `Trailer` is the last four payload bytes, read by the caller because they are not
 * inside the window the rest of the classification works on. */
__attribute__((noinline)) static void classify(const __u8 *pData, __u32 Size, __u32 Trailer,
	const struct ddnet_source *pSource, struct ddnet_decision *pOut)
{
	struct ddnet_tokens Tokens = {};

	pOut->m_Budget = -1;

	if(is_connectionless(pData, Size))
	{
		/* 0.7 puts a token in the connless header, so those can be told apart from
		 * anyone claiming to be a server browser. */
		if(pData[0] == ((1 << 3) << 2 | 1))
		{
			const __u32 Token = read_be32(&pData[1]);
			derive_tokens(pSource, &Tokens);
			pOut->m_Class = DDNET_XDP_CLASS_SIXUP_CONNLESS;
			pOut->m_Pass = token_is(&Tokens, Token) || global_token_valid(Token);
			pOut->m_Verified = pOut->m_Pass;
			return;
		}
		if(master_allowed(pSource))
		{
			/* Whatever a master server sends, challenge or probe, decides whether
			 * this server is listed. Losing it does not cost throughput, it costs the
			 * server its place in the list, so a master is never budgeted. */
			pOut->m_Class = DDNET_XDP_CLASS_MASTER;
			pOut->m_Pass = 1;
			return;
		}
		pOut->m_Class = DDNET_XDP_CLASS_CONNLESS;
		pOut->m_Pass = 1;
		pOut->m_Budget = DDNET_XDP_BUDGET_CONNLESS;
		return;
	}

	if((pData[0] & QUIC_LONG_HEADER_BIT) && is_quic_long_header(pData, Size))
	{
		const __u32 DestinationLen = pData[5];
		const __u32 SourceLen = pData[6 + DestinationLen];
		const __u32 Type = (pData[0] >> 4) & 0x3;

		/* A packet for a connection this endpoint already has carries a destination
		 * connection ID it handed out, and needs no budget. */
		if(DestinationLen == DDNET_XDP_QUIC_CID_LEN && cid_valid(&pData[6]))
		{
			pOut->m_Class = DDNET_XDP_CLASS_QUIC_SHORT;
			pOut->m_Pass = 1;
			pOut->m_Verified = 1;
			return;
		}
		if(Type != 0)
		{
			/* Handshake, 0-RTT or Retry without one of our connection IDs. */
			pOut->m_Class = DDNET_XDP_CLASS_QUIC_LONG;
			pOut->m_Pass = 1;
			pOut->m_Budget = DDNET_XDP_BUDGET_NEWCONN;
			return;
		}
		if(Size < QUIC_MIN_INITIAL_SIZE)
		{
			pOut->m_Class = DDNET_XDP_CLASS_QUIC_INITIAL;
			pOut->m_Pass = 0;
			return;
		}
		{
			/* The token length says whether the source has answered a Retry. It is
			 * forgeable, so this only buys a separate budget, never a free pass. */
			const __u32 Offset = 6 + DestinationLen + 1 + SourceLen;
			pOut->m_Class = (Offset < Size && pData[Offset] != 0) ?
						DDNET_XDP_CLASS_QUIC_INITIAL_TOKEN :
						DDNET_XDP_CLASS_QUIC_INITIAL;
			pOut->m_Pass = 1;
			pOut->m_Budget = DDNET_XDP_BUDGET_NEWCONN;
		}
		return;
	}

	if((pData[0] & QUIC_FIXED_BIT) && Size >= 1 + DDNET_XDP_QUIC_CID_LEN && cid_valid(&pData[1]))
	{
		pOut->m_Class = DDNET_XDP_CLASS_QUIC_SHORT;
		pOut->m_Pass = 1;
		pOut->m_Verified = 1;
		return;
	}

	/* 0.7 keeps its token at a fixed offset in every packet, outside the compression.
	 * Trying it settles the protocol far better than the flag bits do: 0.6 and 0.7
	 * give the same bits different meanings, and with no table of established peers
	 * there is nothing here to break the tie with. The token is the tie breaker. */
	/* Both protocols below ask after the token, and the answer is the same for
	 * either: it is this packet's address every time. */
	derive_tokens(pSource, &Tokens);

	if(Size >= SIXUP_PACKET_HEADER_SIZE && token_is(&Tokens, read_be32(&pData[3])))
	{
		pOut->m_Class = DDNET_XDP_CLASS_SIXUP;
		pOut->m_Pass = 1;
		pOut->m_Verified = 1;
		return;
	}

	/* 0.6 appends its token to the payload and compresses the result, so it is out of
	 * reach whenever the sender chose to compress. When it did not, and control
	 * packets never do, the token sits in the clear at the end. */
	if(((pData[0] >> 2) & LEGACY_FLAG_COMPRESSION) == 0 &&
		Size >= LEGACY_PACKET_HEADER_SIZE + sizeof(__u32) &&
		token_is(&Tokens, Trailer))
	{
		pOut->m_Class = DDNET_XDP_CLASS_LEGACY_VERIFIED;
		pOut->m_Pass = 1;
		pOut->m_Verified = 1;
		/* The handshake is never compressed, so this is where a connection whose data
		 * packets are compressed gets remembered. */
		pOut->m_Track = 1;
		return;
	}

	/* The 0.6 accept is the packet that makes the server hand out a slot, and being a
	 * control packet it is never compressed, so its token is in the clear and was
	 * just checked above. One that did not check out is refused rather than merely
	 * bounded: at this size and shape there is nothing else it could be. A vanilla
	 * client sends the same message four bytes shorter, without a token, and is left
	 * to the budget where it belongs. */
	if(Size == LEGACY_ACCEPT_SIZE &&
		(pData[0] >> 2) == LEGACY_FLAG_CONTROL &&
		pData[2] == 0 &&
		pData[LEGACY_PACKET_HEADER_SIZE] == LEGACY_CTRL_ACCEPT)
	{
		pOut->m_Class = DDNET_XDP_CLASS_LEGACY;
		pOut->m_Pass = 0;
		return;
	}

	/* The two handshake openers carry no token yet, by definition: they are how a
	 * client asks for one. They go to the server under a budget of their own, because
	 * these are players trying to get in and must not lose to a flood of what cannot
	 * be told apart. Recognised before the rule below, which would read the 0.7 one
	 * as a forgery and lock every 0.7 client out. */
	if(is_sixup_token_request(pData, Size) || is_legacy_connect(pData, Size))
	{
		pOut->m_Class = DDNET_XDP_CLASS_HANDSHAKE;
		pOut->m_Pass = 1;
		pOut->m_Budget = DDNET_XDP_BUDGET_HANDSHAKE;
		return;
	}

	if(is_legacy_packet(pData, Size))
	{
		if((pData[0] >> 2) & LEGACY_FLAG_UNUSED)
		{
			/* This one says it is 0.7, and a 0.7 packet that is not the token request
			 * always carries a token. The one it carries did not check out, so there
			 * is nothing to weigh up. */
			pOut->m_Class = DDNET_XDP_CLASS_SIXUP;
			pOut->m_Pass = 0;
			return;
		}
		/* 0.6 that compresses and has not proved itself. Bounded, not trusted. */
		pOut->m_Class = DDNET_XDP_CLASS_LEGACY;
		pOut->m_Pass = 1;
		pOut->m_Budget = DDNET_XDP_BUDGET_LEGACY;
		pOut->m_Track = 2;
		return;
	}

	pOut->m_Class = DDNET_XDP_CLASS_MALFORMED;
	pOut->m_Pass = 0;
}

SEC("xdp")
int ddnet_xdp_filter(struct xdp_md *pCtx)
{
	void *pDataEnd = (void *)(long)pCtx->data_end;
	void *pData = (void *)(long)pCtx->data;
	struct ethhdr *pEth = pData;
	struct ddnet_source Source = {};
	struct ddnet_decision Decision = {};
	struct ddnet_xdp_config *pConfig;
	struct ddnet_xdp_port *pPort;
	void *pCursor = pEth + 1;
	__u16 Protocol;
	__u16 DestinationPort;
	__u16 DatagramSize;
	__u8 *pPayload;
	__u32 PayloadSize;
	__u32 ConfigIndex = 0;
	int Fragment = 0;
	int Vlan;

	if((void *)(pEth + 1) > pDataEnd)
		return XDP_PASS;
	Protocol = pEth->h_proto;
	for(Vlan = 0; Vlan < DDNET_XDP_MAX_VLAN; Vlan++)
	{
		struct vlan_hdr
		{
			__be16 m_Tci;
			__be16 m_Proto;
		} *pVlan;
		if(Protocol != bpf_htons(ETH_P_8021Q) && Protocol != bpf_htons(ETH_P_8021AD))
			break;
		pVlan = pCursor;
		if((void *)(pVlan + 1) > pDataEnd)
			return XDP_PASS;
		Protocol = pVlan->m_Proto;
		pCursor = pVlan + 1;
	}

	if(Protocol == bpf_htons(ETH_P_IP))
	{
		struct iphdr *pIp = pCursor;
		if((void *)(pIp + 1) > pDataEnd)
			return XDP_PASS;
		if(pIp->protocol != IPPROTO_UDP)
			return XDP_PASS;
		/* Nothing this server speaks is ever fragmented: legacy packets stop at
		 * 1400 bytes and QUIC sets the do-not-fragment bit. A fragment is either an
		 * attack or something this filter cannot reassemble to look at - but only
		 * once it is addressed to a port this program guards, which is decided
		 * below. A fragment past the first carries no port at all and is left to
		 * the stack like any other traffic this program does not speak for. */
		if(pIp->frag_off & bpf_htons(0x1fff))
			return XDP_PASS;
		if(pIp->frag_off & bpf_htons(0x2000))
			Fragment = 1;
		if(pIp->ihl < 5)
			return XDP_PASS;
		pCursor = (void *)pIp + pIp->ihl * 4;
		if(pCursor > pDataEnd)
			return XDP_PASS;
		Source.m_Family = DDNET_XDP_FAMILY_IPV4;
		__builtin_memcpy(Source.m_aAddress, &pIp->saddr, 4);
	}
	else if(Protocol == bpf_htons(ETH_P_IPV6))
	{
		struct ipv6hdr *pIp = pCursor;
		if((void *)(pIp + 1) > pDataEnd)
			return XDP_PASS;
		/* Nothing this server speaks sends extension headers, but a packet that
		 * carries them still reaches the socket, so leaving them to the stack would
		 * leave a way around this program: prefix any flood with a hop-by-hop header.
		 * The chain is walked for a bounded number of steps, and a fragment is
		 * dropped like its IPv4 counterpart. */
		{
			__u8 NextHeader = pIp->nexthdr;
			int Step;
			pCursor = pIp + 1;
#pragma unroll
			for(Step = 0; Step < 3; Step++)
			{
				const __u8 *pExtension;
				if(NextHeader == IPPROTO_UDP)
					break;
				if(NextHeader == 44)
				{
					const __u8 *pFragment = pCursor;
					if((void *)(pFragment + 8) > pDataEnd)
						return XDP_PASS;
					/* The offset is the top thirteen bits of the third and fourth
					 * byte. Only the first fragment still has the UDP header that
					 * says which port this is for; the rest are left to the stack
					 * like any other traffic this program does not speak for. */
					if((pFragment[2] | (pFragment[3] & 0xf8)) != 0)
						return XDP_PASS;
					Fragment = 1;
					NextHeader = pFragment[0];
					pCursor = (void *)(pFragment + 8);
					continue;
				}
				if(NextHeader != 0 && NextHeader != 43 && NextHeader != 60)
					return XDP_PASS;
				pExtension = pCursor;
				if((void *)(pExtension + 8) > pDataEnd)
					return XDP_PASS;
				NextHeader = pExtension[0];
				/* Length is in eight octet units, not counting the first eight. */
				pCursor = (void *)(pExtension + 8 + (__u32)pExtension[1] * 8);
				if(pCursor > pDataEnd)
					return XDP_PASS;
			}
			if(NextHeader != IPPROTO_UDP)
				return XDP_PASS;
		}
		Source.m_Family = DDNET_XDP_FAMILY_IPV6;
		__builtin_memcpy(Source.m_aAddress, &pIp->saddr, 16);
	}
	else
	{
		return XDP_PASS;
	}

	{
		struct udphdr *pUdp = pCursor;
		if((void *)(pUdp + 1) > pDataEnd)
			return XDP_PASS;
		DestinationPort = bpf_ntohs(pUdp->dest);
		Source.m_Port = bpf_ntohs(pUdp->source);
		Source.m_DestinationPort = DestinationPort;
		pPayload = (__u8 *)(pUdp + 1);
		DatagramSize = bpf_ntohs(pUdp->len);
	}

	pPort = bpf_map_lookup_elem(&ddnet_ports, &DestinationPort);
	if(!pPort)
		return XDP_PASS;

	pConfig = bpf_map_lookup_elem(&ddnet_config, &ConfigIndex);
	if(!pConfig)
		return XDP_PASS;

	if(Fragment)
	{
		count(pPort->m_Index, DDNET_XDP_CLASS_MALFORMED, pPort->m_Armed ? DDNET_XDP_VERDICT_DROP : DDNET_XDP_VERDICT_WOULD_DROP);
		return pPort->m_Armed ? XDP_DROP : XDP_PASS;
	}

	if((void *)pPayload > pDataEnd)
		return XDP_PASS;
	PayloadSize = (__u32)(pDataEnd - (void *)pPayload);
	/* Ethernet pads a frame out to sixty bytes, so for every packet shorter than
	 * that the card delivers more than was sent and the padding would be read as
	 * payload - the 0.6 token sits at the end of it. The UDP header says how long
	 * the datagram is; the frame only bounds it. */
	if(DatagramSize >= sizeof(struct udphdr) && (__u32)(DatagramSize - sizeof(struct udphdr)) < PayloadSize)
		PayloadSize = (__u32)(DatagramSize - sizeof(struct udphdr));
	/* Anything longer than a legal packet cannot be one, and reading it would need
	 * bounds the verifier cannot follow. */
	if(PayloadSize < 1 || PayloadSize > LEGACY_MAX_PACKET_SIZE)
	{
		count(pPort->m_Index, DDNET_XDP_CLASS_MALFORMED, pPort->m_Armed ? DDNET_XDP_VERDICT_DROP : DDNET_XDP_VERDICT_WOULD_DROP);
		return pPort->m_Armed ? XDP_DROP : XDP_PASS;
	}

	/* The classifier reads at fixed offsets up to the connless header, so give the
	 * verifier one bound that covers all of them. */
	if((void *)(pPayload + SIXUP_CONNLESS_HEADER_SIZE) > pDataEnd)
	{
		if(PayloadSize < LEGACY_PACKET_HEADER_SIZE)
		{
			count(pPort->m_Index, DDNET_XDP_CLASS_MALFORMED, pPort->m_Armed ? DDNET_XDP_VERDICT_DROP : DDNET_XDP_VERDICT_WOULD_DROP);
			return pPort->m_Armed ? XDP_DROP : XDP_PASS;
		}
	}

	/* Answering a handshake here means the server never sees it, and a spoofed
	 * source never sees the answer. Done before the classifier because the reply is
	 * the whole handling of these two packets.
	 *
	 * Only on an armed port. Arming is the proof that the server behind it derives
	 * tokens the way this program does; before that, an answer from here would hand
	 * out tokens the server has never heard of and lock every new player out. */
	if(pConfig->m_Offload && pPort->m_Armed)
	{
		/* Zeroed, because the copy below stops at the end of the packet and the
		 * verifier counts anything it did not write as unreadable. */
		__u8 aRequest[SIXUP_PACKET_HEADER_SIZE + 5] = {};
		__u8 aReply[HANDSHAKE_REPLY_SIZE] = {};
		int Index;
#pragma unroll
		for(Index = 0; Index < (int)sizeof(aRequest); Index++)
		{
			if((void *)(pPayload + Index + 1) > pDataEnd)
				break;
			aRequest[Index] = pPayload[Index];
		}
		const int SixupRequest = is_sixup_token_request(aRequest, PayloadSize);
		const int LegacyConnect = is_legacy_connect(aRequest, PayloadSize);
		/* An answer costs this host a packet on the wire, the same as letting the
		 * handshake through costs the server one, so it is paid for out of the same
		 * budget. Without this the offload answers every handshake it is sent, at
		 * whatever rate they arrive, and the handshake limit the operator set
		 * applies to everything except the path that sends the most. */
		if((SixupRequest || LegacyConnect) &&
			!take_budget(DDNET_XDP_BUDGET_HANDSHAKE, prefix_hash(&Source, pConfig), pConfig))
		{
			count(pPort->m_Index, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_DROP);
			return XDP_DROP;
		}
		if(SixupRequest)
		{
			/* The answer is addressed with the token the client picked, and carries
			 * the one this host would have issued for the address it came from. */
			aReply[0] = (SIXUP_FLAG_CONTROL) << 2;
			__builtin_memcpy(&aReply[3], &aRequest[SIXUP_PACKET_HEADER_SIZE + 1], 4);
			aReply[7] = SIXUP_CTRL_TOKEN;
			write_be32(&aReply[8], token_for(&Source, pConfig->m_CurrentEpoch));
			count(pPort->m_Index, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_ANSWERED);
			return Source.m_Family == DDNET_XDP_FAMILY_IPV6 ?
				       send_reply_v6(pCtx, aReply) :
				       send_reply_v4(pCtx, aReply);
		}
		if(LegacyConnect)
		{
			aReply[0] = LEGACY_FLAG_CONTROL << 2;
			aReply[3] = LEGACY_CTRL_CONNECTACCEPT;
			aReply[4] = 'T';
			aReply[5] = 'K';
			aReply[6] = 'E';
			aReply[7] = 'N';
			/* 0.6 appends the token to the payload rather than the header. Control
			 * packets are never compressed, so it stays where it is put. */
			write_be32(&aReply[8], token_for(&Source, pConfig->m_CurrentEpoch));
			count(pPort->m_Index, DDNET_XDP_CLASS_HANDSHAKE, DDNET_XDP_VERDICT_ANSWERED);
			return Source.m_Family == DDNET_XDP_FAMILY_IPV6 ?
				       send_reply_v6(pCtx, aReply) :
				       send_reply_v4(pCtx, aReply);
		}
	}

	{
		/* Every read the classifier makes lands within this window, so copying it out
		 * once means the verifier does not have to follow a packet pointer through
		 * the whole decision. Bytes past the end of a short packet stay zero and are
		 * never consulted, because the classifier is given the real size. */
		__u8 aHead[DDNET_XDP_HEAD_SIZE] = {};
		int Index;
#pragma unroll
		for(Index = 0; Index < DDNET_XDP_HEAD_SIZE; Index++)
		{
			if((void *)(pPayload + Index + 1) > pDataEnd)
				break;
			aHead[Index] = pPayload[Index];
		}
		__u32 Trailer = 0;
		if(PayloadSize >= sizeof(Trailer))
		{
			/* The size came out of a pointer difference, and the verifier only keeps
			 * track of its lower half; adding it to a packet pointer is rejected
			 * because the upper half could be anything. Masking it says what the
			 * check above already established, that it fits in eleven bits, and
			 * changes no value because the payload is at most 1400 bytes. */
			const __u32 Offset = (PayloadSize - (__u32)sizeof(Trailer)) & 0x7ff;
			if((void *)(pPayload + Offset + sizeof(Trailer)) <= pDataEnd)
				Trailer = read_be32(pPayload + Offset);
		}
		classify(aHead, PayloadSize, Trailer, &Source, &Decision);
	}

	if(Decision.m_Track != 0 && pConfig->m_ConnNsPerToken != 0)
	{
		struct ddnet_xdp_conn_key Key;
		conn_key(&Source, &Key);
		struct ddnet_xdp_bucket *pConn = bpf_map_lookup_elem(&ddnet_conns, &Key);
		const __u64 Now = bpf_ktime_get_ns();
		if(Decision.m_Track == 1)
		{
			/* Refreshed rather than replaced, so a connection that keeps proving
			 * itself does not have its budget handed back on every packet. */
			if(pConn)
			{
				pConn->m_LastNs = Now;
			}
			else
			{
				struct ddnet_xdp_bucket New = {Now, pConfig->m_ConnBurst};
				bpf_map_update_elem(&ddnet_conns, &Key, &New, BPF_ANY);
			}
		}
		/* An LRU entry can outlive the connection it was made for, and whoever holds
		 * that 4-tuple next must not inherit it. */
		else if(pConn && Now - pConn->m_LastNs <= pConfig->m_ConnIdleNs)
		{
			Decision.m_Class = DDNET_XDP_CLASS_LEGACY_TRACKED;
			Decision.m_Budget = 0;
			Decision.m_Pass = take_token(pConn, Now, pConfig->m_ConnNsPerToken, pConfig->m_ConnBurst);
		}
	}

	if(Decision.m_Pass && Decision.m_Budget >= 0)
	{
		Source.m_PrefixHash = prefix_hash(&Source, pConfig);
		Decision.m_Pass = take_budget((__u32)Decision.m_Budget, Source.m_PrefixHash, pConfig);
	}

	if(Decision.m_Verified)
		__sync_fetch_and_add(&pPort->m_Verified, 1);

	if(Decision.m_Pass)
	{
		count(pPort->m_Index, Decision.m_Class, DDNET_XDP_VERDICT_PASS);
		return XDP_PASS;
	}
	count(pPort->m_Index, Decision.m_Class, pPort->m_Armed ? DDNET_XDP_VERDICT_DROP : DDNET_XDP_VERDICT_WOULD_DROP);
	return pPort->m_Armed ? XDP_DROP : XDP_PASS;
}
