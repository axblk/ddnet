#include "map_door_collision.h"

#include <base/math.h>

#include <game/collision.h>
#include <game/mapitems.h>

#include <cmath>

void StampDoorCollision(CCollision *pCollision, vec2 Pos, vec2 Direction, int Length, int Number)
{
	if(pCollision->GetTile(Pos.x, Pos.y) || pCollision->GetFrontTile(Pos.x, Pos.y))
		return;

	for(int i = 0; i < Length - 1; i++)
	{
		const vec2 CurrentPos = Pos + Direction * i;
		if(pCollision->CheckPoint(CurrentPos))
			break;
		pCollision->SetDoorCollisionAt(CurrentPos.x, CurrentPos.y, TILE_STOPA, 0, Number);
	}
}

// One door tile can carry up to eight doors, one per neighbour that names a
// length. The neighbour's index says how long, its direction which way.
static void StampDoorsAt(CCollision *pCollision, int x, int y, int Layer, int Number)
{
	const vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
	static const int s_aOffsetX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
	static const int s_aOffsetY[8] = {1, 1, 0, -1, -1, -1, 0, 1};
	for(int i = 0; i < 8; i++)
	{
		const int Side = pCollision->Entity(x + s_aOffsetX[i], y + s_aOffsetY[i], Layer);
		if(Side < ENTITY_LASER_SHORT || Side > ENTITY_LASER_LONG)
			continue;
		const float Rotation = pi / 4 * i;
		StampDoorCollision(pCollision, Pos, vec2(std::sin(Rotation), std::cos(Rotation)),
			32 * 3 + 32 * (Side - ENTITY_LASER_SHORT) * 3, Number);
	}
}

void BuildMapDoorCollision(CCollision *pCollision)
{
	const CTile *pTiles = pCollision->GameLayer();
	const CTile *pFront = pCollision->FrontLayer();
	const CSwitchTile *pSwitch = pCollision->SwitchLayer();

	for(int y = 0; y < pCollision->GetHeight(); y++)
	{
		for(int x = 0; x < pCollision->GetWidth(); x++)
		{
			const int Index = y * pCollision->GetWidth() + x;
			if(pTiles[Index].m_Index - ENTITY_OFFSET == ENTITY_DOOR)
				StampDoorsAt(pCollision, x, y, LAYER_GAME, 0);
			if(pFront != nullptr && pFront[Index].m_Index - ENTITY_OFFSET == ENTITY_DOOR)
				StampDoorsAt(pCollision, x, y, LAYER_FRONT, 0);
			if(pSwitch != nullptr && pSwitch[Index].m_Type - ENTITY_OFFSET == ENTITY_DOOR)
				StampDoorsAt(pCollision, x, y, LAYER_SWITCH, pSwitch[Index].m_Number);
		}
	}
}
