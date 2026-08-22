#ifndef GAME_SERVER_MODE_GAME_MODE_MAP_RELOAD_STATE_H
#define GAME_SERVER_MODE_GAME_MODE_MAP_RELOAD_STATE_H

class IGameModeMapReloadState
{
public:
	virtual ~IGameModeMapReloadState() = default;

	virtual void DiscardClient(int ClientId) = 0;
};

#endif // GAME_SERVER_MODE_GAME_MODE_MAP_RELOAD_STATE_H
