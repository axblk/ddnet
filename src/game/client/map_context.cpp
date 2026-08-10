#include "map_context.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/str.h>

#include <game/mapitems.h>

CMapContext::CMapContext() :
	m_GameConfig(g_Config)
{
	IConsole *pConsole = m_GameConfig.Console();
	pConsole->Register("tune", "s[tuning] ?f[value]", CFGFLAG_GAME, ConTuneParam, this, "Tune variable to value");
	pConsole->Register("tune_zone", "i[zone] s[tuning] f[value]", CFGFLAG_GAME, ConTuneZone, this, "Tune in zone a variable to value");
	pConsole->Register("mapbug", "s[mapbug]", CFGFLAG_GAME, ConMapbug, this, "Enable map compatibility mode using the specified bug");
	ResetSettings(g_Config);
}

void CMapContext::Init()
{
	m_pMap = CreateMap();
}

void CMapContext::Load(const CConfig &BaseConfig)
{
	ResetSettings(BaseConfig);
	m_MapBugs = CMapBugs::Create(Map()->BaseName(), Map()->Size(), Map()->Sha256());

	int Start, Num;
	Map()->GetType(MAPITEMTYPE_INFO, &Start, &Num);
	for(int i = Start; i < Start + Num; i++)
	{
		int ItemId;
		CMapItemInfoSettings *pItem = (CMapItemInfoSettings *)Map()->GetItem(i, nullptr, &ItemId);
		const int ItemSize = Map()->GetItemSize(i);
		if(!pItem || ItemId != 0)
			continue;
		if(ItemSize < (int)sizeof(CMapItemInfoSettings) || pItem->m_Settings < 0)
			break;

		const int Size = Map()->GetDataSize(pItem->m_Settings);
		char *pSettings = (char *)Map()->GetData(pItem->m_Settings);
		char *pNext = pSettings;
		while(pNext < pSettings + Size)
		{
			m_GameConfig.ExecuteLine(pNext);
			pNext += str_length(pNext) + 1;
		}
		Map()->UnloadData(pItem->m_Settings);
		break;
	}
}

void CMapContext::ResetSettings(const CConfig &BaseConfig)
{
	m_GameConfig.Reset(BaseConfig);
	for(int TuneZone = 0; TuneZone < TuneZone::NUM; TuneZone++)
	{
		m_aTuningList[TuneZone] = CTuningParams::DEFAULT;
		m_aTuningList[TuneZone].Set("gun_curvature", 0);
		m_aTuningList[TuneZone].Set("gun_speed", 1400);
		m_aTuningList[TuneZone].Set("shotgun_curvature", 0);
		m_aTuningList[TuneZone].Set("shotgun_speed", 500);
		m_aTuningList[TuneZone].Set("shotgun_speeddiff", 0);
	}
}

void CMapContext::Unload()
{
	m_Collision.Unload();
	m_Layers.Unload();
}

void CMapContext::SetTuning(int TuneZone, const char *pName, float Value)
{
	if(TuneZone >= 0 && TuneZone < TuneZone::NUM)
		m_aTuningList[TuneZone].Set(pName, Value);
}

void CMapContext::EnableMapBug(const char *pName)
{
	switch(m_MapBugs.Update(pName))
	{
	case EMapBugUpdate::OK:
		break;
	case EMapBugUpdate::OVERRIDDEN:
		log_debug("mapbugs", "map-internal setting overridden by database");
		break;
	case EMapBugUpdate::NOTFOUND:
		log_debug("mapbugs", "unknown map bug '%s', ignoring", pName);
		break;
	default:
		dbg_assert_failed("unreachable");
	}
}

void CMapContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	if(pResult->NumArguments() == 2)
		static_cast<CMapContext *>(pUserData)->SetTuning(0, pResult->GetString(0), pResult->GetFloat(1));
}

void CMapContext::ConTuneZone(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CMapContext *>(pUserData)->SetTuning(pResult->GetInteger(0), pResult->GetString(1), pResult->GetFloat(2));
}

void CMapContext::ConMapbug(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CMapContext *>(pUserData)->EnableMapBug(pResult->GetString(0));
}
