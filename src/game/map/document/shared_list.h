#ifndef GAME_MAP_DOCUMENT_SHARED_LIST_H
#define GAME_MAP_DOCUMENT_SHARED_LIST_H

#include <base/dbg.h>

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * A list that versions of the map share until one of them writes to it.
 *
 * Tiles have their own store, because a layer of them is too big to copy for
 * every version - see `CTileStore`. Everything else a map is made of comes in
 * lists that are short enough to copy whole: the quads of a layer, the sound
 * sources of a layer, the points of an envelope, the bytes of an embedded
 * image. A version that leaves such a list alone shares it; one that moves a
 * single quad copies that one list and nothing else in the map.
 *
 * Reading goes through the list as it is. Asking for `Mutable` is what takes
 * it apart from whoever else holds it, once, however many changes follow.
 */
template<typename T>
class CSharedList
{
public:
	CSharedList() = default;
	explicit CSharedList(std::vector<T> Items) :
		m_pItems(std::make_shared<const std::vector<T>>(std::move(Items)))
	{
	}

	size_t Size() const { return m_pItems == nullptr ? 0 : m_pItems->size(); }
	bool Empty() const { return Size() == 0; }

	const T &operator[](size_t Index) const
	{
		dbg_assert(Index < Size(), "List index out of range");
		return (*m_pItems)[Index];
	}

	/** All of it at once, for walking it or handing it to a writer. */
	const std::vector<T> &All() const
	{
		static const std::vector<T> s_Empty;
		return m_pItems == nullptr ? s_Empty : *m_pItems;
	}

	/**
	 * The list to be written to.
	 *
	 * Whoever else holds it holds it as it was, so it is copied away from
	 * them before the first change reaches them.
	 */
	std::vector<T> &Mutable()
	{
		if(m_pItems == nullptr)
		{
			m_pItems = std::make_shared<const std::vector<T>>();
		}
		else if(m_pItems.use_count() > 1)
		{
			m_pItems = std::make_shared<const std::vector<T>>(*m_pItems);
		}
		// Nobody else holds it, so what is written here reaches nobody else.
		return const_cast<std::vector<T> &>(*m_pItems);
	}

	/**
	 * What this list is, rather than what is in it: two versions whose lists
	 * answer the same here hold the same items and were never written to
	 * apart.
	 */
	const void *Id() const { return m_pItems.get(); }

	/** What the list holds, as if it were the only one holding it. */
	uint64_t Bytes() const
	{
		std::unordered_set<const void *> Seen;
		return BytesOnce(Seen);
	}

	/**
	 * The same, counting nothing that is already in `Seen`.
	 *
	 * What an item points at somewhere else is not counted - a list of
	 * strings is counted as the strings' own size, not the text they hold.
	 */
	uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const
	{
		if(m_pItems == nullptr || !Seen.insert(m_pItems.get()).second)
			return 0;
		return sizeof(std::vector<T>) + sizeof(T) * m_pItems->capacity();
	}

private:
	// Left empty for a list that holds nothing, so that an empty list costs
	// nothing at all - most layers have no quads and most maps no sounds.
	std::shared_ptr<const std::vector<T>> m_pItems;
};

#endif // GAME_MAP_DOCUMENT_SHARED_LIST_H
