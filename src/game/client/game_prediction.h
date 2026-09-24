/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_GAME_PREDICTION_H
#define GAME_CLIENT_GAME_PREDICTION_H

#include <engine/client/session.h>
#include <engine/kernel.h>

class CGameClient;
class CGameState;

/**
 * Carries the world past the last snapshot with the inputs of the local
 * players. Only a program that plays registers it; one that only watches a
 * demo shows what the snapshots hold and does not link it.
 */
class IGamePrediction : public IInterface
{
	MACRO_INTERFACE("gameprediction")
public:
	/**
	 * Brings the world the prediction starts from up to the snapshot the input
	 * session just got.
	 *
	 * @param GameClient The game that got the snapshot.
	 */
	virtual void OnNewSnapshot(CGameClient &GameClient) = 0;
	/**
	 * Predicts a session from its last snapshot up to its prediction tick.
	 *
	 * @param GameClient The game the session is played in.
	 * @param SessionId The session to predict.
	 */
	virtual void Predict(CGameClient &GameClient, CSessionId SessionId) = 0;
	/**
	 * Adds the projectiles, pickups and lasers of a game state's snapshot to
	 * its world, which is being rebuilt from that snapshot.
	 *
	 * @param State The game state whose world is rebuilt.
	 */
	virtual void AddEntities(CGameState &State) const = 0;
};

IGamePrediction *CreateGamePrediction();

#endif
