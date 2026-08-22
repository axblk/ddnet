#include "ddrace_map_entities.h"

#include <base/dbg.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/server/entities/door.h>
#include <game/server/entities/dragger.h>
#include <game/server/entities/gun.h>
#include <game/server/entities/light.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>

bool CreateDDRaceMapEntity(CGameContext *pGameServer, int Index, int x, int y, int Layer, int Flags, int Number)
{
	dbg_assert(pGameServer != nullptr, "Game server must not be null");
	dbg_assert(Index >= 0, "Invalid entity index");

	const vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
	int aSides[8];
	aSides[0] = pGameServer->Collision()->Entity(x, y + 1, Layer);
	aSides[1] = pGameServer->Collision()->Entity(x + 1, y + 1, Layer);
	aSides[2] = pGameServer->Collision()->Entity(x + 1, y, Layer);
	aSides[3] = pGameServer->Collision()->Entity(x + 1, y - 1, Layer);
	aSides[4] = pGameServer->Collision()->Entity(x, y - 1, Layer);
	aSides[5] = pGameServer->Collision()->Entity(x - 1, y - 1, Layer);
	aSides[6] = pGameServer->Collision()->Entity(x - 1, y, Layer);
	aSides[7] = pGameServer->Collision()->Entity(x - 1, y + 1, Layer);

	if(Index == ENTITY_DOOR)
	{
		for(int i = 0; i < 8; i++)
		{
			if(aSides[i] >= ENTITY_LASER_SHORT && aSides[i] <= ENTITY_LASER_LONG)
			{
				new CDoor(
					&pGameServer->m_World,
					Pos,
					pi / 4 * i,
					32 * 3 + 32 * (aSides[i] - ENTITY_LASER_SHORT) * 3,
					Number);
			}
		}
		return true;
	}
	if(Index == ENTITY_CRAZY_SHOTGUN_EX)
	{
		int Dir;
		if(!Flags)
			Dir = 0;
		else if(Flags == ROTATION_90)
			Dir = 1;
		else if(Flags == ROTATION_180)
			Dir = 2;
		else
			Dir = 3;
		const float Deg = Dir * (pi / 2);
		CProjectile *pBullet = new CProjectile(
			&pGameServer->m_World,
			WEAPON_SHOTGUN,
			-1,
			Pos,
			vec2(std::sin(Deg), std::cos(Deg)),
			-2,
			true,
			true,
			g_Config.m_SvShotgunBulletSound ? SOUND_GRENADE_EXPLODE : -1,
			vec2(std::sin(Deg), std::cos(Deg)),
			Layer,
			Number);
		pBullet->SetBouncing(2 - (Dir % 2));
		return true;
	}
	if(Index == ENTITY_CRAZY_SHOTGUN)
	{
		int Dir;
		if(!Flags)
			Dir = 0;
		else if(Flags == TILEFLAG_ROTATE)
			Dir = 1;
		else if(Flags == (TILEFLAG_XFLIP | TILEFLAG_YFLIP))
			Dir = 2;
		else
			Dir = 3;
		const float Deg = Dir * (pi / 2);
		CProjectile *pBullet = new CProjectile(
			&pGameServer->m_World,
			WEAPON_SHOTGUN,
			-1,
			Pos,
			vec2(std::sin(Deg), std::cos(Deg)),
			-2,
			true,
			false,
			SOUND_GRENADE_EXPLODE,
			vec2(std::sin(Deg), std::cos(Deg)),
			Layer,
			Number);
		pBullet->SetBouncing(2 - (Dir % 2));
		return true;
	}
	if(Index >= ENTITY_LASER_FAST_CCW && Index <= ENTITY_LASER_FAST_CW)
	{
		int aSides2[8];
		aSides2[0] = pGameServer->Collision()->Entity(x, y + 2, Layer);
		aSides2[1] = pGameServer->Collision()->Entity(x + 2, y + 2, Layer);
		aSides2[2] = pGameServer->Collision()->Entity(x + 2, y, Layer);
		aSides2[3] = pGameServer->Collision()->Entity(x + 2, y - 2, Layer);
		aSides2[4] = pGameServer->Collision()->Entity(x, y - 2, Layer);
		aSides2[5] = pGameServer->Collision()->Entity(x - 2, y - 2, Layer);
		aSides2[6] = pGameServer->Collision()->Entity(x - 2, y, Layer);
		aSides2[7] = pGameServer->Collision()->Entity(x - 2, y + 2, Layer);

		int Ind = Index - ENTITY_LASER_STOP;
		int M;
		if(Ind < 0)
		{
			Ind = -Ind;
			M = 1;
		}
		else if(Ind == 0)
			M = 0;
		else
			M = -1;

		float AngularSpeed = 0.0f;
		if(Ind == 1)
			AngularSpeed = pi / 360;
		else if(Ind == 2)
			AngularSpeed = pi / 180;
		else if(Ind == 3)
			AngularSpeed = pi / 90;
		AngularSpeed *= M;

		for(int i = 0; i < 8; i++)
		{
			if(aSides[i] >= ENTITY_LASER_SHORT && aSides[i] <= ENTITY_LASER_LONG)
			{
				CLight *pLight = new CLight(&pGameServer->m_World, Pos, pi / 4 * i, 32 * 3 + 32 * (aSides[i] - ENTITY_LASER_SHORT) * 3, Layer, Number);
				pLight->m_AngularSpeed = AngularSpeed;
				if(aSides2[i] >= ENTITY_LASER_C_SLOW && aSides2[i] <= ENTITY_LASER_C_FAST)
				{
					pLight->m_Speed = 1 + (aSides2[i] - ENTITY_LASER_C_SLOW) * 2;
					pLight->m_CurveLength = pLight->m_Length;
				}
				else if(aSides2[i] >= ENTITY_LASER_O_SLOW && aSides2[i] <= ENTITY_LASER_O_FAST)
				{
					pLight->m_Speed = 1 + (aSides2[i] - ENTITY_LASER_O_SLOW) * 2;
					pLight->m_CurveLength = 0;
				}
				else
					pLight->m_CurveLength = pLight->m_Length;
			}
		}
		return true;
	}
	if(Index >= ENTITY_DRAGGER_WEAK && Index <= ENTITY_DRAGGER_STRONG)
	{
		new CDragger(&pGameServer->m_World, Pos, Index - ENTITY_DRAGGER_WEAK + 1, false, Layer, Number);
		return true;
	}
	if(Index >= ENTITY_DRAGGER_WEAK_NW && Index <= ENTITY_DRAGGER_STRONG_NW)
	{
		new CDragger(&pGameServer->m_World, Pos, Index - ENTITY_DRAGGER_WEAK_NW + 1, true, Layer, Number);
		return true;
	}
	if(Index == ENTITY_PLASMAE)
	{
		new CGun(&pGameServer->m_World, Pos, false, true, Layer, Number);
		return true;
	}
	if(Index == ENTITY_PLASMAF)
	{
		new CGun(&pGameServer->m_World, Pos, true, false, Layer, Number);
		return true;
	}
	if(Index == ENTITY_PLASMA)
	{
		new CGun(&pGameServer->m_World, Pos, true, true, Layer, Number);
		return true;
	}
	if(Index == ENTITY_PLASMAU)
	{
		new CGun(&pGameServer->m_World, Pos, false, false, Layer, Number);
		return true;
	}
	return false;
}
