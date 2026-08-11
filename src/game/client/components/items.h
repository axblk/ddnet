/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_ITEMS_H
#define GAME_CLIENT_COMPONENTS_ITEMS_H

#include <base/color.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CProjectileData;
class CLaserData;
class CGameState;
class CRenderContext;
class CScreenRect;

class CItems : public CComponent
{
	bool GetProjectileRenderInfo(const CRenderContext &Context, const CProjectileData *pCurrent, const CScreenRect &ScreenRect, int &CurWeapon, vec2 &Pos, vec2 &Vel, float &Alpha) const;
	vec2 GetPickupPosition(const CRenderContext &Context, const CNetObj_Pickup *pPrev, const CNetObj_Pickup *pCurrent, bool IsPredicted) const;
	void RenderProjectile(const CRenderContext &Context, const CProjectileData *pCurrent, int ItemId, const CScreenRect &ScreenRect);
	void RenderPickup(const CRenderContext &Context, const CNetObj_Pickup *pPrev, const CNetObj_Pickup *pCurrent, bool IsPredicted, int Flags);
	void RenderFlags();
	void RenderFlag(const CNetObj_Flag *pPrev, const CNetObj_Flag *pCurrent, const CNetObj_GameData *pPrevGameData, const CNetObj_GameData *pCurGameData);
	void RenderLaser(const CLaserData *pCurrent, bool IsPredicted = false);

	int m_ItemsQuadContainerIndex;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender(const CRenderContext &Context) override;
	void OnInit() override;

	void UpdatePresentation(CGameState &State, const CRenderContext &Context, const CScreenRect &ScreenRect);
	void ReconstructSmokeTrail(CGameState &State, const CRenderContext &Context, const CProjectileData *pCurrent, int DestroyTick);
	void RenderLaser(vec2 From, vec2 Pos, ColorRGBA OuterColor, ColorRGBA InnerColor, float TicksBody, float TicksHead, int Type) const;

private:
	int m_BlueFlagOffset;
	int m_RedFlagOffset;
	int m_PickupHealthOffset;
	int m_PickupArmorOffset;
	int m_aPickupWeaponOffset[NUM_WEAPONS];
	int m_PickupNinjaOffset;
	int m_aPickupWeaponArmorOffset[4];
	int m_aProjectileOffset[NUM_WEAPONS];
	int m_aParticleSplatOffset[3];
	int m_DoorHeadOffset;
	int m_PulleyHeadOffset;
	int m_FreezeHeadOffset;
};

#endif
