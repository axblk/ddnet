/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_ALLOC_H
#define GAME_ALLOC_H

#include <base/mem.h>

#include <cstdlib>
#include <new>

#define MACRO_ALLOC_HEAP() \
public: \
	void *operator new(size_t Size) \
	{ \
		void *pObj = malloc(Size); \
		mem_zero(pObj, Size); \
		return pObj; \
	} \
	void operator delete(void *pPtr) \
	{ \
		free(pPtr); \
	} \
\
private:

#define MACRO_ALLOC_POOL_ID() \
public: \
	void *operator new(size_t Size, int Id); \
	void operator delete(void *pObj, int Id); \
	void operator delete(void *pObj); /* NOLINT(misc-new-delete-overloads) */ \
\
private:

// Heap allocated: a pool indexed by client id would be shared by every game in
// the process, and players and characters are not allocated often enough to need one.
#define MACRO_ALLOC_POOL_ID_IMPL(POOLTYPE, PoolSize) \
	void *POOLTYPE::operator new(size_t Size, [[maybe_unused]] int Id) \
	{ \
		void *pObj = malloc(Size); \
		mem_zero(pObj, Size); \
		return pObj; \
	} \
	void POOLTYPE::operator delete(void *pObj, [[maybe_unused]] int Id) \
	{ \
		free(pObj); \
	} \
	void POOLTYPE::operator delete(void *pObj) /* NOLINT(misc-new-delete-overloads) */ \
	{ \
		free(pObj); \
	}

#endif
