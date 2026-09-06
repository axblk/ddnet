// The token bucket every budget is kept in. It lives in a header without any BPF
// dependency so the XDP program and the user space test compile the same code, and
// the arithmetic that decides what a flood is allowed to cost can be checked without
// a kernel to load the program into.
#ifndef XDP_DDNET_XDP_BUCKET_H
#define XDP_DDNET_XDP_BUCKET_H

#include "ddnet_xdp_shared.h"

/* Brings a bucket up to date with `Now`. A bucket that was never used, or that saw a
 * clock going backwards, starts full. */
static __always_inline void refill_bucket(struct ddnet_xdp_bucket *pBucket, uint64_t Now, uint64_t NsPerToken, uint64_t Burst)
{
	uint64_t Elapsed, Refill;
	if(pBucket->m_LastNs == 0 || pBucket->m_LastNs > Now)
	{
		pBucket->m_LastNs = Now;
		pBucket->m_Tokens = Burst;
	}
	Elapsed = Now - pBucket->m_LastNs;
	Refill = Elapsed / NsPerToken;
	if(Refill > 0)
	{
		/* Advancing by whole tokens instead of to `Now` keeps the remainder, so a
		 * slow bucket does not lose time on every packet and stall forever. */
		pBucket->m_LastNs += Refill * NsPerToken;
		pBucket->m_Tokens = pBucket->m_Tokens + Refill > Burst ? Burst : pBucket->m_Tokens + Refill;
	}
}

/* Refills and takes one token. `NsPerToken` of zero means the class is unlimited. */
static __always_inline int take_token(struct ddnet_xdp_bucket *pBucket, uint64_t Now, uint64_t NsPerToken, uint64_t Burst)
{
	if(NsPerToken == 0)
		return 1;
	refill_bucket(pBucket, Now, NsPerToken, Burst);
	if(pBucket->m_Tokens == 0)
		return 0;
	pBucket->m_Tokens--;
	return 1;
}

/* Takes one token from each of two buckets, or from neither. Both are refilled and
 * looked at before either is charged: a packet the port cap refuses must not eat
 * into a prefix bucket that a player in the same prefix could still have used, and
 * the other way round. Either budget may be unlimited, in which case its bucket is
 * left alone. */
static __always_inline int take_token_pair(
	struct ddnet_xdp_bucket *pFirst, const struct ddnet_xdp_budget *pFirstBudget,
	struct ddnet_xdp_bucket *pSecond, const struct ddnet_xdp_budget *pSecondBudget,
	uint64_t Now)
{
	const int FirstLimited = pFirstBudget->m_NsPerToken != 0;
	const int SecondLimited = pSecondBudget->m_NsPerToken != 0;
	if(FirstLimited)
		refill_bucket(pFirst, Now, pFirstBudget->m_NsPerToken, pFirstBudget->m_Burst);
	if(SecondLimited)
		refill_bucket(pSecond, Now, pSecondBudget->m_NsPerToken, pSecondBudget->m_Burst);
	if((FirstLimited && pFirst->m_Tokens == 0) || (SecondLimited && pSecond->m_Tokens == 0))
		return 0;
	if(FirstLimited)
		pFirst->m_Tokens--;
	if(SecondLimited)
		pSecond->m_Tokens--;
	return 1;
}

#endif
