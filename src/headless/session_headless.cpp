#include <engine/client/session.h>

class CHeadlessSource : public CSessionSource
{
public:
	ESessionSourceType Type() const override { return ESessionSourceType::NETWORK; }
};

int main()
{
	CSessionManager Manager;
	const CSessionId Id = Manager.Create(std::make_unique<CHeadlessSource>());
	CSessionSource *pSource = Manager.Find(Id);
	if(!pSource || !pSource->SetState(ESessionState::CONNECTING) || !pSource->SetState(ESessionState::LOADING_MAP) || !pSource->SetState(ESessionState::READY))
		return 1;
	pSource->RequestStop("done");
	if(pSource->State() != ESessionState::STOPPING || pSource->TakeStopReason() != "done")
		return 2;
	return pSource->SetState(ESessionState::OFFLINE) ? 0 : 3;
}
