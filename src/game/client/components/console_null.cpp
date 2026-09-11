/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL) || defined(CONF_DEMO_VIEWER_TOOL)

#include "console.h"

// The console the client draws over the game. Neither the program that turns a
// demo into a video nor the one that shows it in a window has anything to say
// to it: what the engine console prints goes to the log, which is where both of
// them are read anyway.

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
