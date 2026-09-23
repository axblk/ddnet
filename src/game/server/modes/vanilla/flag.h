#ifndef GAME_SERVER_MODES_VANILLA_FLAG_H
#define GAME_SERVER_MODES_VANILLA_FLAG_H

#include <game/server/entity.h>

class CCharacter;

class CFlag : public CEntity
{
public:
	static constexpr int PHYSICAL_SIZE = 14;

	CFlag(CGameWorld *pGameWorld, int Team, vec2 StandPos);

	void Reset() override;
	void TickDeferred() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;

	void Grab(CCharacter *pCarrier);
	void Drop();
	void Return();
	bool TakeAutomaticReturn();

	int Team() const { return m_Team; }
	bool IsAtStand() const { return m_AtStand; }
	CCharacter *Carrier() const { return m_pCarrier; }
	int GrabTick() const { return m_GrabTick; }
	int DropTick() const { return m_DropTick; }
	const vec2 &StandPosition() const { return m_StandPos; }

private:
	void ReturnAutomatically();

	int m_Team;
	vec2 m_StandPos;
	bool m_AtStand = true;
	CCharacter *m_pCarrier = nullptr;
	vec2 m_Vel = vec2(0, 0);
	int m_GrabTick = 0;
	int m_DropTick = 0;
	bool m_AutomaticReturn = false;
};

#endif // GAME_SERVER_MODES_VANILLA_FLAG_H
