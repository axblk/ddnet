#ifndef GAME_MAP_DOCUMENT_HISTORY_H
#define GAME_MAP_DOCUMENT_HISTORY_H

#include <base/dbg.h>
#include <base/time.h>

#include <game/map/document/map_state.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * The versions of one map, in the order they were made.
 *
 * Undoing is not the work of undoing: a version was never thrown away, so
 * going back is pointing at the one before. What that costs is the parts the
 * two versions do not have in common, which is what `CMapState` is built to
 * keep small - see the comment there.
 *
 * The line is straight. Pushing a version while an older one is current drops
 * everything that came after it, the way the editor has always behaved; a
 * history that branches is a bigger idea than this one.
 *
 * There is always a version in here, because a map is always in some state.
 * The first one is what was opened.
 */
class CHistory
{
public:
	class CEntry
	{
	public:
		CMapState m_State;
		/** What was done to get here, for the history view: "Draw", "Fill". */
		std::string m_Label;
		/** When it was done, so the view can group or fade old entries. */
		int64_t m_TimeNanos = 0;
	};

	/**
	 * The history of a map that was just opened. The label is what the first
	 * entry is called; there is no way back past it.
	 */
	explicit CHistory(CMapState Opened, const char *pLabel = "Opened")
	{
		m_vEntries.push_back(CEntry{std::move(Opened), pLabel, Now()});
	}

	/** The version the map is in. Everything drawn and saved reads this. */
	const CMapState &Current() const { return m_vEntries[m_Current].m_State; }

	size_t NumEntries() const { return m_vEntries.size(); }
	size_t CurrentIndex() const { return m_Current; }

	const CEntry &Entry(size_t Index) const
	{
		dbg_assert(Index < m_vEntries.size(), "History entry out of range");
		return m_vEntries[Index];
	}

	bool CanUndo() const { return m_Current > 0; }
	bool CanRedo() const { return m_Current + 1 < m_vEntries.size(); }

	/**
	 * Writes a new version down and makes it the current one.
	 *
	 * Whatever could have been redone before is gone - the state it described
	 * is not reachable from here any more, and keeping it would only cost
	 * memory that the limit wants for the way back.
	 */
	void Push(CMapState State, const char *pLabel)
	{
		m_vEntries.resize(m_Current + 1);
		m_vEntries.push_back(CEntry{std::move(State), pLabel, Now()});
		m_Current = m_vEntries.size() - 1;
		Prune();
	}

	/** One step back, or nothing if this is where the history starts. */
	bool Undo()
	{
		if(!CanUndo())
			return false;
		--m_Current;
		return true;
	}

	/** One step forward, or nothing if nothing was undone. */
	bool Redo()
	{
		if(!CanRedo())
			return false;
		++m_Current;
		return true;
	}

	/**
	 * Straight to an entry, which is what clicking a line of the history view
	 * does. Nothing is dropped by going somewhere: the entries after this one
	 * stay until something new is pushed.
	 */
	void JumpTo(size_t Index)
	{
		dbg_assert(Index < m_vEntries.size(), "History entry out of range");
		m_Current = Index;
	}

	/**
	 * What the whole history holds, in bytes.
	 *
	 * Every block is counted once, however many versions share it, because
	 * all of them are walked with one `Seen` between them - a version that
	 * changed one block adds one block here, not a map. That is what makes
	 * this the number to hold the limit against.
	 */
	uint64_t Bytes() const
	{
		std::unordered_set<const void *> Seen;
		uint64_t Total = 0;
		for(const CEntry &Entry : m_vEntries)
		{
			Total += Entry.m_State.BytesOnce(Seen);
		}
		return Total;
	}

	/**
	 * How far back the history is allowed to reach.
	 *
	 * The bytes are the real limit - a stroke over one chunk and a fill over
	 * the whole layer are one entry each and nothing alike. The count is
	 * there because a history view with ten thousand lines in it is of no use
	 * to anybody.
	 *
	 * The oldest entries go first, and the version the map is in is never one
	 * of them: a limit small enough to forbid the current version leaves the
	 * history one entry long rather than empty.
	 */
	void SetLimits(uint64_t MaxBytes, size_t MaxEntries)
	{
		dbg_assert(MaxEntries >= 1, "A history holds at least the version the map is in");
		m_MaxBytes = MaxBytes;
		m_MaxEntries = MaxEntries;
		Prune();
	}

	uint64_t MaxBytes() const { return m_MaxBytes; }
	size_t MaxEntries() const { return m_MaxEntries; }

private:
	static int64_t Now() { return time_get_nanoseconds().count(); }

	bool OverLimit() const { return m_vEntries.size() > m_MaxEntries || Bytes() > m_MaxBytes; }

	void Prune()
	{
		// Dropping the front is what frees memory: the versions there are the
		// only ones holding the blocks that were painted over long ago.
		while(m_Current > 0 && OverLimit())
		{
			m_vEntries.erase(m_vEntries.begin());
			--m_Current;
		}
		// Past the current version there is only the way forward, and giving
		// that up is better than giving up the way back.
		while(m_vEntries.size() > m_Current + 1 && OverLimit())
		{
			m_vEntries.pop_back();
		}
	}

	std::vector<CEntry> m_vEntries;
	size_t m_Current = 0;

	// 256 MiB is what the plan suggests for a desktop browser; whoever knows
	// better about the machine says so with `SetLimits`.
	uint64_t m_MaxBytes = (uint64_t)256 * 1024 * 1024;
	size_t m_MaxEntries = 1000;
};

#endif // GAME_MAP_DOCUMENT_HISTORY_H
