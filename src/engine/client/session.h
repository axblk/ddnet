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

class IGameSessionSource
{
public:
	virtual ~IGameSessionSource() = default;
	virtual ESessionSourceType Type() const = 0;
	virtual ESessionState State() const = 0;
	virtual const char *ErrorString() const = 0;
	virtual void SetState(ESessionState State) = 0;
	virtual void Fail(const char *pError) = 0;
	virtual void Update() = 0;
	virtual void RequestStop() = 0;
};

class CGameSession
{
	CSessionId m_Id;
	std::unique_ptr<IGameSessionSource> m_pSource;

public:
	CGameSession(CSessionId Id, std::unique_ptr<IGameSessionSource> pSource);

	CSessionId Id() const { return m_Id; }
	ESessionState State() const { return m_pSource->State(); }
	IGameSessionSource &Source() { return *m_pSource; }
	const IGameSessionSource &Source() const { return *m_pSource; }
	void Update() { m_pSource->Update(); }
};

class CSessionManager
{
	uint64_t m_NextId = 1;
	std::vector<std::unique_ptr<CGameSession>> m_vpSessions;
	CSessionId m_FocusedSessionId;

public:
	CSessionId Create(std::unique_ptr<IGameSessionSource> pSource);
	CGameSession *Find(CSessionId Id);
	const CGameSession *Find(CSessionId Id) const;
	bool SetFocused(CSessionId Id);
	CSessionId FocusedId() const { return m_FocusedSessionId; }
	CGameSession *Focused() { return Find(m_FocusedSessionId); }
	const CGameSession *Focused() const { return Find(m_FocusedSessionId); }
	bool Close(CSessionId Id);
	bool Destroy(CSessionId Id);
	void Update();
	size_t NumSessions() const { return m_vpSessions.size(); }
};

#endif // ENGINE_CLIENT_SESSION_H
