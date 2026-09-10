/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "console.h"

// The console the client draws over the game, for a program that answers to
// nobody at a keyboard. What the engine console prints goes to the log, which
// is where a tool is read anyway.

CGameConsole::CInstance::CInstance(int Type) :
	m_Type(Type)
{
}

CGameConsole::CGameConsole() :
	m_LocalConsole(CONSOLETYPE_LOCAL), m_RemoteConsole(CONSOLETYPE_REMOTE)
{
}

CGameConsole::~CGameConsole() = default;

void CGameConsole::PrintLine(int Type, const char *pLine) {}
void CGameConsole::RequireUsername(bool UsernameReq) {}
void CGameConsole::ForceUpdateRemoteCompletionSuggestions() {}

void CGameConsole::OnStateChange(int NewState, int OldState) {}
void CGameConsole::OnConsoleInit() {}
void CGameConsole::OnInit() {}
void CGameConsole::OnWindowResize() {}
void CGameConsole::OnShutdown() {}
void CGameConsole::OnReset() {}
void CGameConsole::OnRenderApplicationOverlay() {}
void CGameConsole::OnMessage(int MsgType, void *pRawMsg) {}

bool CGameConsole::OnInput(const IInput::CEvent &Event)
{
	return false;
}

#endif
