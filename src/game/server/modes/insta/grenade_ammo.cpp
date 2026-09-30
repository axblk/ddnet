// The rules follow the grenade ammo of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "grenade_ammo.h"

#include <engine/shared/config.h>

#include <algorithm>

CGrenadeAmmo::CSettings CGrenadeAmmo::CSettings::FromConfig(int TickSpeed)
{
	CSettings Settings;
	Settings.m_Regen = g_Config.m_SvGrenadeAmmoRegen;
	Settings.m_RegenTicks = g_Config.m_SvGrenadeAmmoRegenTime * TickSpeed / 1000;
	Settings.m_Max = g_Config.m_SvGrenadeAmmoRegenNum;
	Settings.m_SelfPushRefund = g_Config.m_SvGrenadeAmmoRegenSpeed;
	Settings.m_RefillOnHit = g_Config.m_SvGrenadeAmmoRegenOnKill;
	return Settings;
}

int CGrenadeAmmo::AfterHit(int Ammo, const CSettings &Settings)
{
	if(!Settings.m_Regen || Ammo < 0)
		return Ammo;
	switch(Settings.m_RefillOnHit)
	{
	case 1: return std::min(Ammo + 1, Settings.m_Max);
	case 2: return Settings.m_Max;
	default: return Ammo;
	}
}

int CGrenadeAmmo::AfterSelfPush(int Ammo, const CSettings &Settings)
{
	if(!Settings.m_Regen || !Settings.m_SelfPushRefund || Ammo < 0)
		return Ammo;
	return std::min(Ammo + 1, Settings.m_Max);
}

int CGrenadeAmmo::CRegen::Tick(int Ammo, const CSettings &Settings)
{
	if(!Settings.m_Regen || Ammo < 0 || Ammo >= Settings.m_Max)
	{
		m_Ticks = 0;
		return Ammo;
	}
	if(m_Ticks++ < Settings.m_RegenTicks)
		return Ammo;
	m_Ticks = 0;
	return Ammo + 1;
}
