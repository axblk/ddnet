#ifndef GAME_SERVER_MODES_INSTA_GRENADE_AMMO_H
#define GAME_SERVER_MODES_INSTA_GRENADE_AMMO_H

/**
 * The grenades of gCTF as ddnet-insta has them: a few, coming back one by
 * one over time, all or one of them back with a hit, and one back for a
 * grenade that pushed its own shooter.
 *
 * Without sv_grenade_ammo_regen the grenades are endless (-1).
 */
class CGrenadeAmmo
{
public:
	class CSettings
	{
	public:
		bool m_Regen = false;
		// ticks for a grenade to come back
		int m_RegenTicks = 0;
		int m_Max = 0;
		bool m_SelfPushRefund = false;
		// 0 nothing, 1 one grenade, 2 all
		int m_RefillOnHit = 0;

		static CSettings FromConfig(int TickSpeed);
	};

	static int Spawn(const CSettings &Settings) { return Settings.m_Regen ? Settings.m_Max : -1; }
	static int AfterHit(int Ammo, const CSettings &Settings);
	static int AfterSelfPush(int Ammo, const CSettings &Settings);

	/**
	 * The regeneration of one player: a grenade comes back after the fire
	 * delay and then every m_RegenTicks and one more tick, as in ddnet-insta.
	 */
	class CRegen
	{
		// counts up to m_RegenTicks, negative during the fire delay
		int m_Ticks = 0;

	public:
		void OnFire(int FireDelayTicks) { m_Ticks = -FireDelayTicks - 1; }
		// a tick of the character, returns the ammo after it
		int Tick(int Ammo, const CSettings &Settings);
	};
};

#endif // GAME_SERVER_MODES_INSTA_GRENADE_AMMO_H
