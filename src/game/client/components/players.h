/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_PLAYERS_H
#define GAME_CLIENT_COMPONENTS_PLAYERS_H
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/component.h>
#include <game/client/render.h>

class CRenderContext;
class CGameState;

class CPlayers : public CComponent
{
	friend class CGhost;

	class CPlayerRenderState
	{
	public:
		CNetObj_Character m_Prev;
		CNetObj_Character m_Player;
		CTeeRenderInfo m_RenderInfo;
		CAnimState m_Animation;
		vec2 m_Position;
		vec2 m_Direction;
		vec2 m_Vel;
		float m_Angle;
		float m_Intra;
		float m_Alpha;
		float m_AttackTime;
		float m_LastAttackTime;
		float m_AttackTicksPassed;
		bool m_Local;
		bool m_PredictLocalWeapons;
		bool m_Stationary;
		bool m_InAir;
		bool m_WantOtherDir;
		bool m_Afk;
		bool m_Inactive;
	};

	void RenderHand6(const CTeeRenderInfo *pInfo, vec2 HandPos, float HandAngle, float Alpha);
	void RenderHand7(const CTeeRenderInfo *pInfo, vec2 HandPos, float HandAngle, float Alpha);

	void RenderHand(const CTeeRenderInfo *pInfo, vec2 CenterPos, vec2 Dir, float AngleOffset, vec2 PostRotOffset, float Alpha);
	void RenderPlayer(
		const CRenderContext &Context,
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		const CTeeRenderInfo *pRenderInfo,
		int ClientId,
		float Intra = 0.f);
	void RenderHook(
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		const CTeeRenderInfo *pRenderInfo,
		int ClientId,
		float Intra = 0.f);
	void RenderHookCollLine(
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		int ClientId);
	void RenderSpectatorCharacters(const CRenderContext &Context, const CScreenRect &ScreenRect) const;
	bool IsPlayerInfoAvailable(int ClientId) const;
	void PrepareRenderInfo(const CRenderContext &Context, int ClientId, bool IsTeamPlay, CTeeRenderInfo &RenderInfo) const;
	bool PreparePlayerRenderState(
		const CRenderContext &Context,
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		const CTeeRenderInfo *pRenderInfo,
		int ClientId,
		float Intra,
		CPlayerRenderState &State);
	void UpdatePlayerPresentation(
		CGameState &GameState,
		const CRenderContext &Context,
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		const CTeeRenderInfo *pRenderInfo,
		int ClientId,
		bool Audible,
		float Intra = 0.0f);

	int m_WeaponEmoteQuadContainerIndex;
	int m_aWeaponSpriteMuzzleQuadContainerIndex[NUM_WEAPONS];

	void CreateNinjaTeeRenderInfo();
	void CreateSpectatorTeeRenderInfo();

	std::shared_ptr<CManagedTeeRenderInfo> m_pNinjaTeeRenderInfo;
	std::shared_ptr<CManagedTeeRenderInfo> m_pSpectatorTeeRenderInfo;

public:
	float GetPlayerTargetAngle(
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		int ClientId,
		float Intra = 0.0f);

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void UpdatePresentation(CGameState &State, const CRenderContext &Context, const CScreenRect &ScreenRect, bool Audible);
	void OnRender(const CRenderContext &Context) override;

	const std::shared_ptr<CManagedTeeRenderInfo> &NinjaTeeRenderInfo() const { return m_pNinjaTeeRenderInfo; }
	const std::shared_ptr<CManagedTeeRenderInfo> &SpectatorTeeRenderInfo() const { return m_pSpectatorTeeRenderInfo; }
};

#endif
