//  The pure part of the shared-port classifier: the predicates that decide from the
// bytes alone which protocol a datagram belongs to.
//
// This is a port of src/engine/shared/udp_port_mux_classifier.rs and has to agree
// with it, otherwise a datagram is either delivered twice or not at all. It lives in
// a header without any BPF dependency so the same code can be compiled for the XDP
// program and for the test that runs the Rust test vectors against it.
//
#ifndef XDP_DDNET_XDP_CLASSIFY_H
#define XDP_DDNET_XDP_CLASSIFY_H

#include <stdint.h>

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

/* Legacy DDNet wire constants, mirroring src/engine/shared/network.h. */
#define LEGACY_MAX_PACKET_SIZE 1400
#define LEGACY_PACKET_HEADER_SIZE 3
#define SIXUP_PACKET_HEADER_SIZE 7
#define SIXUP_CONNLESS_HEADER_SIZE 9
#define LEGACY_CONNLESS_HEADER_SIZE 6

#define LEGACY_FLAG_UNUSED (1 << 0)
#define LEGACY_FLAG_CONTROL (1 << 2)
#define LEGACY_FLAG_RESEND (1 << 4)
#define LEGACY_FLAG_COMPRESSION (1 << 5)

#define SIXUP_FLAG_CONTROL (1 << 0)
#define SIXUP_FLAG_RESEND (1 << 1)
#define SIXUP_FLAG_COMPRESSION (1 << 2)

#define QUIC_FIXED_BIT 0x40
#define QUIC_LONG_HEADER_BIT 0x80
#define QUIC_MAX_CID_SIZE 20
/* RFC 9000 14.1: a client Initial that carries less than this is not a legal
 * attempt, it is a cheap way to ask a server for work. */
#define QUIC_MIN_INITIAL_SIZE 1200

/* Mirrors `is_connectionless` in src/engine/shared/udp_port_mux_classifier.rs. */
static __always_inline int is_connectionless(const uint8_t *pData, uint32_t Size)
{
	if(Size > LEGACY_MAX_PACKET_SIZE)
		return 0;
	if(Size >= LEGACY_CONNLESS_HEADER_SIZE)
	{
		if(pData[0] == 0xff && pData[1] == 0xff && pData[2] == 0xff &&
			pData[3] == 0xff && pData[4] == 0xff && pData[5] == 0xff)
			return 1;
		if(pData[0] == 'x' && pData[1] == 'e')
			return 1;
	}
	return Size >= SIXUP_CONNLESS_HEADER_SIZE && pData[0] == ((1 << 3) << 2 | 1);
}

/* Mirrors `is_legacy_packet`. */
static __always_inline int is_legacy_packet(const uint8_t *pData, uint32_t Size)
{
	uint8_t RawFlags, AllowedFlags, ControlFlag, ResendFlag, CompressionFlag;
	uint32_t DataStart;

	if(Size < LEGACY_PACKET_HEADER_SIZE || Size > LEGACY_MAX_PACKET_SIZE)
		return 0;
	RawFlags = pData[0] >> 2;
	if(RawFlags & LEGACY_FLAG_UNUSED)
	{
		AllowedFlags = SIXUP_FLAG_CONTROL | SIXUP_FLAG_RESEND | SIXUP_FLAG_COMPRESSION;
		ControlFlag = SIXUP_FLAG_CONTROL;
		ResendFlag = SIXUP_FLAG_RESEND;
		CompressionFlag = SIXUP_FLAG_COMPRESSION;
		DataStart = SIXUP_PACKET_HEADER_SIZE;
	}
	else
	{
		AllowedFlags = LEGACY_FLAG_CONTROL | LEGACY_FLAG_RESEND | LEGACY_FLAG_COMPRESSION;
		ControlFlag = LEGACY_FLAG_CONTROL;
		ResendFlag = LEGACY_FLAG_RESEND;
		CompressionFlag = LEGACY_FLAG_COMPRESSION;
		DataStart = LEGACY_PACKET_HEADER_SIZE;
	}
	if((RawFlags & ~AllowedFlags) || Size < DataStart)
		return 0;
	if(RawFlags & ControlFlag)
		return pData[2] == 0 && Size > DataStart && !(RawFlags & CompressionFlag);
	return pData[2] > 0 || (RawFlags & ResendFlag);
}

/* Version independent part of a QUIC long header, mirroring `is_quic_long_header`.
 * Unknown versions have to stay recognizable so quinn can answer them with version
 * negotiation. */
static __always_inline int is_quic_long_header(const uint8_t *pData, uint32_t Size)
{
	uint32_t DestinationLen, SourceLenOffset, SourceLen;

	if(Size < 7 || (pData[0] & (QUIC_LONG_HEADER_BIT | QUIC_FIXED_BIT)) != 0xc0)
		return 0;
	DestinationLen = pData[5];
	if(DestinationLen > QUIC_MAX_CID_SIZE || Size < 7 + DestinationLen)
		return 0;
	SourceLenOffset = 6 + DestinationLen;
	SourceLen = pData[SourceLenOffset];
	return SourceLen <= QUIC_MAX_CID_SIZE && Size > SourceLenOffset + 1 + SourceLen;
}

#endif
