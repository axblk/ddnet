/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_ALLOC_H
#define GAME_ALLOC_H

#include <base/mem.h>

#include <cstdlib>
#include <new>

// Zeroed storage for members nobody initialises. calloc, because an optimising
// build may drop a mem_zero into storage no object lives in yet.
#define MACRO_ALLOC_HEAP() \
public: \
	void *operator new(size_t Size) \
	{ \
		return calloc(1, Size); \
	} \
	void operator delete(void *pPtr) \
	{ \
		free(pPtr); \
	} \
\
private:

#endif
