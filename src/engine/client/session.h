#ifndef ENGINE_CLIENT_SESSION_H
#define ENGINE_CLIENT_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CSessionId
{
	uint64_t m_Value = 0;

public:
	CSessionId() = default;
	explicit CSessionId(uint64_t Value) :
		m_Value(Value)
	{
	}

	bool IsValid() const { return m_Value != 0; }
	uint64_t Value() const { return m_Value; }
	bool operator==(const CSessionId &Other) const { return m_Value == Other.m_Value; }
	bool operator!=(const CSessionId &Other) const { return !(*this == Other); }
};

/**
 * Finds the entry of a session in a list of objects kept per session.
 *
 * @param vpEntries Objects that know the id of their session.
 * @param Id Id of the session to look for.
 *
 * @return The entry of the session, `nullptr` if there is none.
 */
template<typename T>
T *FindSessionEntry(const std::vector<std::unique_ptr<T>> &vpEntries, CSessionId Id)
{
	for(const std::unique_ptr<T> &pEntry : vpEntries)
	{
		if(pEntry->Id() == Id)
			return pEntry.get();
	}
	return nullptr;
}

enum class ESessionState
{
	OFFLINE,
	CONNECTING,
	LOADING_MAP,
	READY,
	STOPPING,
	ERROR,
};

enum class ESessionSourceType
{
	NETWORK,
	DEMO,
};

class CSessionSource
{
	friend class CSessionManager;

	CSessionId m_Id;
	ESessionState m_State = ESessionState::OFFLINE;
	std::string m_Error;
	std::string m_StopReason;

public:
	virtual ~CSessionSource() = default;
	virtual ESessionSourceType Type() const = 0;

	CSessionId Id() const { return m_Id; }
	ESessionState State() const { return m_State; }
	const char *ErrorString() const { return m_Error.c_str(); }
	bool SetState(ESessionState State);
	void Fail(const char *pError);
	/**
	 * Marks the session as stopping. Whoever updates the session carries the
	 * stop out and takes the reason back with @link TakeStopReason @endlink.
	 *
	 * @param pReason Reason for the stop, can be `nullptr`.
	 */
	void RequestStop(const char *pReason);
	std::string TakeStopReason();
};

class CSessionManager
{
	uint64_t m_NextId = 1;
	std::vector<std::unique_ptr<CSessionSource>> m_vpSessions;
	CSessionId m_FocusedSessionId;

public:
	CSessionId Create(std::unique_ptr<CSessionSource> pSource);
	CSessionSource *Find(CSessionId Id) const { return FindSessionEntry(m_vpSessions, Id); }
	bool SetFocused(CSessionId Id);
	CSessionId FocusedId() const { return m_FocusedSessionId; }
	CSessionSource *Focused() const { return Find(m_FocusedSessionId); }
	const std::vector<std::unique_ptr<CSessionSource>> &Sessions() const { return m_vpSessions; }
};

#endif // ENGINE_CLIENT_SESSION_H
