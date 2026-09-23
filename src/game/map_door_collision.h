#ifndef GAME_MAP_DOOR_COLLISION_H
#define GAME_MAP_DOOR_COLLISION_H

#include <base/vmath.h>

class CCollision;

// Doors stamp stop tiles into the shared collision grid once and never clear
// them, so both sides can build the grid from the map without door entities.

// Stamps one door from its tile outwards, up to the first solid point.
void StampDoorCollision(CCollision *pCollision, vec2 Pos, vec2 Direction, int Length, int Number);

// Stamps every door of the map in the order the server creates them.
void BuildMapDoorCollision(CCollision *pCollision);

#endif // GAME_MAP_DOOR_COLLISION_H
