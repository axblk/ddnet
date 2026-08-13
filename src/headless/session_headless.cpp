#include <engine/client/session.h>

class CHeadlessSource : public IGameSessionSource
{
	ESessionState m_State = ESessionState::CONNECTING;

public:
	ESessionSourceType Type() const override { return ESessionSourceType::NETWORK; }
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return ""; }
	bool SetState(ESessionState State) override
	{
		m_State = State;
		return true;
	}
	void Fail(const char *) override { m_State = ESessionState::ERROR; }
	void Update() override
	{
		if(m_State == ESessionState::CONNECTING)
			m_State = ESessionState::READY;
		else if(m_State == ESessionState::STOPPING)
			m_State = ESessionState::OFFLINE;
	}
	void RequestStop(const char *) override { m_State = ESessionState::STOPPING; }
};

int main()
{
	CSessionManager Manager;
	const CSessionId Id = Manager.Create(std::make_unique<CHeadlessSource>());
	Manager.Update();
	if(!Manager.Find(Id) || Manager.Find(Id)->State() != ESessionState::READY)
		return 1;
	Manager.Close(Id);
	Manager.Update();
	return Manager.Find(Id)->State() == ESessionState::OFFLINE ? 0 : 2;
}
