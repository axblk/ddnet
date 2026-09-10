/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "menu_background.h"

// The scenery behind the menu pages, for a program that has no menu pages. The
// map layers it is built on stay: a demo is drawn with them.

CMenuBackground::CMenuBackground() :
	m_CurrentPosition(POS_START),
	m_ChangedPosition(false),
	m_MoveTime(0.0f),
	m_IsInit(false),
	m_Loading(false)
{
}

void CMenuBackground::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
}

void CMenuBackground::OnInit() {}
void CMenuBackground::OnUpdate() {}
void CMenuBackground::OnShutdown() {}
void CMenuBackground::OnMapLoad() {}
void CMenuBackground::LoadMenuBackground(bool HasDayHint, bool HasNightHint) {}

bool CMenuBackground::Render()
{
	return false;
}

void CMenuBackground::ChangePosition(int PositionNumber) {}

std::vector<CTheme> &CMenuBackground::GetThemes()
{
	return m_vThemes;
}

#endif
