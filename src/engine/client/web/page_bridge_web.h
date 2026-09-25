#ifndef ENGINE_CLIENT_WEB_PAGE_BRIDGE_WEB_H
#define ENGINE_CLIENT_WEB_PAGE_BRIDGE_WEB_H

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

/**
 * What a tool's exports are made of. The page calls them on its own thread,
 * while the tool runs on a thread of its own and may be anywhere in a frame,
 * so a call from the page never touches the tool: what it asks for waits
 * here for the tool to take it between two frames, and what it reads is what
 * the tool published after the last one.
 *
 * The page only ever holds the lock for as long as it takes to copy an entry
 * in or out, and the tool never waits on the page while it holds it.
 *
 * @tparam TState What the page may read, copied as a whole.
 */
template<typename TState>
class CWebPageBridge
{
public:
	/**
	 * Hands `Action` to the tool, which runs it in `RunActions`. From any
	 * thread.
	 */
	void Post(std::function<void()> &&Action)
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_vActions.push_back(std::move(Action));
	}

	/**
	 * Runs what the page asked for, in the order it asked. Only from the tool.
	 */
	void RunActions()
	{
		std::vector<std::function<void()>> vActions;
		{
			std::lock_guard<std::mutex> Lock(m_Mutex);
			std::swap(vActions, m_vActions);
		}
		for(const std::function<void()> &Action : vActions)
			Action();
	}

	/**
	 * Replaces what the page reads. Only from the tool. Left out while the
	 * page asked for something the tool has not taken yet, so that what the
	 * page just set does not read back as it was.
	 */
	void Publish(TState State)
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		if(!m_vActions.empty())
			return;
		m_State = std::move(State);
		m_Published = true;
	}

	/**
	 * Changes what the page reads until the tool next publishes, so that a
	 * value the page just set reads back before the tool took it.
	 */
	template<typename TChange>
	void Change(TChange &&Change)
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		Change(m_State);
	}

	/**
	 * What the tool last published, or a default state before it did.
	 */
	TState State() const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		return m_State;
	}

	/**
	 * Whether the tool published at least once, which is when it runs.
	 */
	bool Published() const
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		return m_Published;
	}

	/**
	 * Drops what the page asked for and what it reads, when the tool ends.
	 */
	void Reset()
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_vActions.clear();
		m_State = TState();
		m_Published = false;
	}

private:
	mutable std::mutex m_Mutex;
	std::vector<std::function<void()>> m_vActions;
	TState m_State;
	bool m_Published = false;
};

#endif
