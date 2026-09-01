/* Definitions shared between the XDP program and the loader. */
#ifndef XDP_DDNET_XDP_SHARED_H
#define XDP_DDNET_XDP_SHARED_H

#include "siphash.h"

/* Written to the key file so a server can tell whether it understands the format. */
#define DDNET_XDP_KEY_MAGIC "DDNXDPK1"
#define DDNET_XDP_KEY_MAGIC_SIZE 8
#define DDNET_XDP_KEY_EPOCHS 4
#define DDNET_XDP_QUIC_CID_LEN 8
/* The part of a connection ID the tag is derived over: epoch and nonce. */
#define DDNET_XDP_CID_TAG_INPUT 4
#define DDNET_XDP_PREFIX_BUCKETS 256
#define DDNET_XDP_MAX_PORTS 64
#define DDNET_XDP_DEFAULT_PIN_DIR "/sys/fs/bpf/ddnet-xdp"
#define DDNET_XDP_DEFAULT_KEY_PATH "/run/ddnet-xdp/key"

/* Canonical input of the token derivation, built identically by the server and by
 * the filter:
 *
 *     family (1 byte, 4 or 6) || address (4 or 16 bytes) || port (2 bytes, big endian)
 *
 * The port is part of it on purpose. Leaving it out, as the SHA256 derivation did,
 * gives every client behind one address the same token.
 */
#define DDNET_XDP_TOKEN_INPUT_V4 7
#define DDNET_XDP_TOKEN_INPUT_MAX 19

enum
{
	DDNET_XDP_FAMILY_IPV4 = 4,
	DDNET_XDP_FAMILY_IPV6 = 6,
};

struct ddnet_xdp_key
{
	struct siphash_key m_Key;
	uint8_t m_Valid;
	uint8_t m_aPad[7];
};

/* A port is only guarded once it has been seen carrying verified traffic, so a
 * server that does not use the key yet is left alone instead of being cut off.
 */
struct ddnet_xdp_port
{
	uint8_t m_Armed;
	/* Counters are kept per port, so it is visible which server is being hit rather
	 * than only that something is. */
	uint8_t m_Index;
	uint8_t m_aPad[6];
	/* Packets that carried a value only this host could have issued. The loader
	 * waits for these before it arms a port, so a server that does not use the key
	 * yet is left alone rather than cut off. */
	uint64_t m_Verified;
};

/* What cannot be verified is bounded, and each kind gets its own budget so that a
 * flood of one kind cannot starve another. Ordered by how much a loss hurts. */
enum
{
	/* 0.6 that carries no readable token: vanilla, or DDNet with compression on. Loses
	 * to everything else, because nothing about it can be told from a flood. */
	DDNET_XDP_BUDGET_LEGACY = 0,
	/* Server info requests. The master servers hand the same answer out, so a lost
	 * request costs little. */
	DDNET_XDP_BUDGET_CONNLESS,
	/* Handshakes that are passed to the server rather than answered here. These are
	 * players trying to get in, and the budget is sized so they usually do. */
	DDNET_XDP_BUDGET_HANDSHAKE,
	/* QUIC connection attempts. */
	DDNET_XDP_BUDGET_NEWCONN,
	DDNET_XDP_NUM_BUDGETS,
};

struct ddnet_xdp_budget
{
	/* The time one token takes, which stays exact when a rate is split over prefix
	 * buckets and CPUs and would otherwise round down to zero. Zero is unlimited. */
	uint64_t m_NsPerToken;
	uint64_t m_Burst;
};

struct ddnet_xdp_config
{
	struct ddnet_xdp_budget m_aBudgets[DDNET_XDP_NUM_BUDGETS];
	uint32_t m_PrefixV4;
	uint32_t m_PrefixV6;
	/* Budget of an established 0.6 connection that was learned from its handshake.
	 * Zero switches the connection table off entirely. */
	uint64_t m_ConnNsPerToken;
	uint64_t m_ConnBurst;
	/* An entry that has not been used for this long is treated as gone. The table is
	 * an LRU, so this is about a stale 4-tuple being reused, not about memory. */
	uint64_t m_ConnIdleNs;
};

/* Identifies one legacy connection. Written out with no implicit padding, because a
 * hash map compares the key bytes and a hole in it would never match. */
struct ddnet_xdp_conn_key
{
	uint8_t m_Family;
	uint8_t m_aPad[1];
	uint16_t m_SourcePort;
	uint16_t m_DestinationPort;
	uint16_t m_aPad2[1];
	uint8_t m_aAddress[16];
};

struct ddnet_xdp_bucket
{
	uint64_t m_LastNs;
	uint64_t m_Tokens;
};

/* Traffic classes, in the order the classifier decides them. */
enum
{
	DDNET_XDP_CLASS_MASTER = 0,
	DDNET_XDP_CLASS_SIXUP,
	DDNET_XDP_CLASS_SIXUP_CONNLESS,
	DDNET_XDP_CLASS_QUIC_SHORT,
	DDNET_XDP_CLASS_QUIC_INITIAL,
	DDNET_XDP_CLASS_QUIC_INITIAL_TOKEN,
	DDNET_XDP_CLASS_QUIC_LONG,
	DDNET_XDP_CLASS_LEGACY_VERIFIED,
	DDNET_XDP_CLASS_LEGACY_TRACKED,
	DDNET_XDP_CLASS_LEGACY,
	DDNET_XDP_CLASS_CONNLESS,
	DDNET_XDP_CLASS_MALFORMED,
	DDNET_XDP_CLASS_HANDSHAKE,
	DDNET_XDP_NUM_CLASSES,
};

enum
{
	DDNET_XDP_VERDICT_PASS = 0,
	DDNET_XDP_VERDICT_WOULD_DROP,
	DDNET_XDP_VERDICT_DROP,
	DDNET_XDP_NUM_VERDICTS,
};

#define DDNET_XDP_NUM_COUNTERS (DDNET_XDP_NUM_CLASSES * DDNET_XDP_NUM_VERDICTS)
#define DDNET_XDP_STATS_ENTRIES (DDNET_XDP_MAX_PORTS * DDNET_XDP_NUM_COUNTERS)
#define DDNET_XDP_STATS_INDEX(port, class, verdict) \
	((uint32_t)(port) * DDNET_XDP_NUM_COUNTERS + (uint32_t)(class) * DDNET_XDP_NUM_VERDICTS + (uint32_t)(verdict))

static const char *const DDNET_XDP_CLASS_NAMES[DDNET_XDP_NUM_CLASSES] = {
	"master",
	"0.7",
	"0.7 connless",
	"quic short",
	"quic initial",
	"quic initial+token",
	"quic long",
	"0.6 verified",
	"0.6 tracked",
	"0.6 unverified",
	"connless 0.6",
	"malformed",
	"handshake answered",
};

static const char *const DDNET_XDP_VERDICT_NAMES[DDNET_XDP_NUM_VERDICTS] = {
	"pass",
	"would drop",
	"drop",
};

#endif
