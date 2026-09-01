//  SipHash-2-4, the shared derivation for DDNet security tokens and QUIC connection
// IDs. The same source is compiled for the BPF target and for user space, so the
// filter and the server cannot drift apart.
//
// Reference: Jean-Philippe Aumasson and Daniel J. Bernstein, "SipHash: a fast
// short-input PRF" (2012).
//
#ifndef XDP_SIPHASH_H
#define XDP_SIPHASH_H

/* Clang ships a freestanding <stdint.h> that the BPF target can use too, so both
 * builds agree on the integer widths without dragging in kernel or libc headers. */
#include <stdint.h>

#ifndef __always_inline
#if defined(_MSC_VER)
/* MSVC never sees the BPF target, but it does compile the user-space half. */
#define __always_inline __forceinline
#else
#define __always_inline inline __attribute__((always_inline))
#endif
#endif

struct siphash_key
{
	uint64_t m_K0;
	uint64_t m_K1;
};

static __always_inline uint64_t siphash_rotl(uint64_t X, int B)
{
	return (X << B) | (X >> (64 - B));
}

#define SIPHASH_ROUND() \
	do \
	{ \
		V0 += V1; \
		V1 = siphash_rotl(V1, 13); \
		V1 ^= V0; \
		V0 = siphash_rotl(V0, 32); \
		V2 += V3; \
		V3 = siphash_rotl(V3, 16); \
		V3 ^= V2; \
		V0 += V3; \
		V3 = siphash_rotl(V3, 21); \
		V3 ^= V0; \
		V2 += V1; \
		V1 = siphash_rotl(V1, 17); \
		V1 ^= V2; \
		V2 = siphash_rotl(V2, 32); \
	} while(0)

/* `pData` must hold at least `Size` bytes and `Size` must be known to be bounded by
 * the caller. The verifier has to see a constant bound, so every call site in the BPF
 * program passes a fixed-size buffer.
 */
static __always_inline uint64_t siphash24(const struct siphash_key *pKey, const uint8_t *pData, uint32_t Size)
{
	uint64_t V0 = pKey->m_K0 ^ 0x736f6d6570736575ULL;
	uint64_t V1 = pKey->m_K1 ^ 0x646f72616e646f6dULL;
	uint64_t V2 = pKey->m_K0 ^ 0x6c7967656e657261ULL;
	uint64_t V3 = pKey->m_K1 ^ 0x7465646279746573ULL;
	const uint32_t Left = Size & 7;
	const uint32_t Blocks = Size / 8;
	uint64_t Tail = (uint64_t)Size << 56;
	uint32_t Index;

	for(Index = 0; Index < Blocks; Index++)
	{
		uint64_t Word = 0;
		uint32_t Byte;
		for(Byte = 0; Byte < 8; Byte++)
			Word |= (uint64_t)pData[Index * 8 + Byte] << (8 * Byte);
		V3 ^= Word;
		SIPHASH_ROUND();
		SIPHASH_ROUND();
		V0 ^= Word;
	}

	for(Index = 0; Index < Left; Index++)
		Tail |= (uint64_t)pData[Blocks * 8 + Index] << (8 * Index);

	V3 ^= Tail;
	SIPHASH_ROUND();
	SIPHASH_ROUND();
	V0 ^= Tail;

	V2 ^= 0xff;
	SIPHASH_ROUND();
	SIPHASH_ROUND();
	SIPHASH_ROUND();
	SIPHASH_ROUND();
	return V0 ^ V1 ^ V2 ^ V3;
}

#endif
