#ifndef GAME_SERVER_GAMEMODES_DDRACE_MAP_ENTITIES_H
#define GAME_SERVER_GAMEMODES_DDRACE_MAP_ENTITIES_H

class CGameServices;
struct CMapEntityContext;

// the map entities only the DDRace modes know
bool CreateDDRaceMapEntity(CGameServices &Services, const CMapEntityContext &Context);

#endif // GAME_SERVER_GAMEMODES_DDRACE_MAP_ENTITIES_H
