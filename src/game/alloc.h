/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_ALLOC_H
#define GAME_ALLOC_H

#include <base/mem.h>

#include <cstdlib>
#include <new>

// Zeroes the storage before the constructor runs, so a member that nobody
// initialises reads as zero instead of as whatever the allocator handed out.
//
// calloc and not malloc followed by mem_zero, because the two are only
// equivalent while the compiler cannot see them. Once this is inlined into a
// caller the zeroing is a store into storage no object lives in yet, and an
// optimising build is free to drop it; the constructor then runs over whatever
// malloc returned. That is not theoretical: it cost a Release-only crash in
// CPlayer, which was fine only as long as its allocator lived out of line in a
// translation unit of its own. Zeroing that the allocation itself performs
// cannot be separated from it.
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
