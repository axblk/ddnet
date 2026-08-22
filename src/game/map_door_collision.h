#ifndef GAME_MAP_DOOR_COLLISION_H
#define GAME_MAP_DOOR_COLLISION_H

#include <base/vmath.h>

class CCollision;

// Door collision is map geometry, not entity state.
//
// A door writes stop tiles into the collision grid when it is created and never
// takes them back: the server stamps them once while it walks the map and no
// door ever clears a tile again. What opens and closes a door is the switcher
// state, which lives per world, not the grid, which is shared by all of them.
//
// So the grid can be built from the map on either side, in the same order, with
// no door entity involved at all.

// The span one door covers, from its tile outwards. Stops at the first solid
// point, and does nothing when the door sits on a solid tile itself.
void StampDoorCollision(CCollision *pCollision, vec2 Pos, vec2 Direction, int Length, int Number);

// Every door the map describes, in the order the server creates them: by row,
// then by column, and per tile the game layer before the front layer before the
// switch layer. Two doors starting on the same tile therefore resolve the same
// way on both sides - the switch-layer one stamps last and wins.
void BuildMapDoorCollision(CCollision *pCollision);

#endif // GAME_MAP_DOOR_COLLISION_H
