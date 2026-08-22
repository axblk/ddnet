#ifndef GAME_SERVER_GAMEMODES_DDRACE_MAP_ENTITIES_H
#define GAME_SERVER_GAMEMODES_DDRACE_MAP_ENTITIES_H

class CGameContext;

// Creates map entities that are specific to the DDRace/DDNet entity set.
// Controllers opt into this set explicitly instead of inheriting it from the
// common game controller.
bool CreateDDRaceMapEntity(CGameContext *pGameServer, int Index, int x, int y, int Layer, int Flags, int Number);

#endif // GAME_SERVER_GAMEMODES_DDRACE_MAP_ENTITIES_H
