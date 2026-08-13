/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_HUD_H
#define GAME_CLIENT_COMPONENTS_HUD_H
#include <engine/client.h>
#include <engine/shared/protocol.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/component.h>

struct SScoreInfo
{
	SScoreInfo()
	{
		Reset();
	}

	void Reset()
	{
		m_TextRankContainerIndex.Reset();
		m_TextScoreContainerIndex.Reset();
		m_RoundRectQuadContainerIndex = -1;
		m_OptionalNameTextContainerIndex.Reset();
		m_aScoreText[0] = 0;
		m_aRankText[0] = 0;
		m_aPlayerNameText[0] = 0;
		m_ScoreTextWidth = 0.f;
		m_Initialized = false;
	}

	STextContainerIndex m_TextRankContainerIndex;
	STextContainerIndex m_TextScoreContainerIndex;
	float m_ScoreTextWidth;
	char m_aScoreText[16];
	char m_aRankText[16];
	char m_aPlayerNameText[MAX_NAME_LENGTH];
	int m_RoundRectQuadContainerIndex;
	STextContainerIndex m_OptionalNameTextContainerIndex;

	bool m_Initialized;
};

class CHud : public CComponent
{
	float m_Width, m_Height;

	int m_HudQuadContainerIndex;
	SScoreInfo m_aScoreInfo[2];
	uint64_t m_ScoreHudSessionId = 0;
	uint64_t m_ScoreHudStateId = 0;
	uint64_t m_ScoreHudViewId = 0;
	uint64_t m_ScoreHudOutputKey = 0;
	int m_ScoreHudViewportX = 0;
	int m_ScoreHudViewportY = 0;
	int m_ScoreHudViewportWidth = 0;
	int m_ScoreHudViewportHeight = 0;
	int m_ScoreHudGameFlags = 0;
	bool m_ScoreHudHasGameData = false;
	bool m_ScoreHudCacheValid = false;
	STextContainerIndex m_FPSTextContainerIndex;
	STextContainerIndex m_DDRaceEffectsTextContainerIndex;

	void RenderCursor(const CRenderContext &Context);
	void ResetScoreHudContainers();

	void RenderTextInfo(const CRenderContext &Context);
	void RenderConnectionWarning(const CRenderContext &Context);
	void RenderTeambalanceWarning(const CRenderContext &Context);

	void PrepareAmmoHealthAndArmorQuads();
	void RenderAmmoHealthAndArmor(const CRenderContext &Context, const CNetObj_Character *pCharacter);

	void PreparePlayerStateQuads();
	void RenderPlayerState(const CRenderContext &Context, int ClientId);

	void RenderSpectatorCount(const CRenderContext &Context);
	void RenderDummyActions(const CRenderContext &Context);
	void RenderMovementInformation(const CRenderContext &Context);

	void RenderMovementInformationValue(float FontSize, float Value, const ColorRGBA &Color, float RightX, float Y);

	class CMovementInformation
	{
	public:
		vec2 m_Pos;
		vec2 m_Speed;
		float m_Angle = 0.0f;
	};
	class CMovementInformation GetMovementInformation(const CRenderContext &Context, int ClientId) const;

	void RenderGameTimer(const CRenderContext &Context);
	void RenderPauseNotification(const CRenderContext &Context);
	void RenderSuddenDeath(const CRenderContext &Context);

	void RenderScoreHud(const CRenderContext &Context);
	int m_LastLocalClientId = -1;

	void RenderSpectatorHud(const CRenderContext &Context);
	void RenderWarmupTimer(const CRenderContext &Context);
	void RenderLocalTime(const CRenderContext &Context, float x);

	static constexpr float MOVEMENT_INFORMATION_LINE_HEIGHT = 8.0f;

public:
	CHud();
	int Sizeof() const override { return sizeof(*this); }

	void ResetHudContainers();
	void OnWindowResize() override;
	void OnReset() override;
	void OnRender(const CRenderContext &Context) override;
	void OnInit() override;

	// DDRace

	void RenderNinjaBarPos(float x, float y, float Width, float Height, float Progress, float Alpha = 1.0f);

private:
	void RenderRecord(const CRenderContext &Context);
	void RenderDDRaceEffects(const CRenderContext &Context);

	inline float GetMovementInformationBoxHeight(const CRenderContext &Context);
	inline int GetDigitsIndex(int Value, int Max);

	// Quad Offsets
	int m_aAmmoOffset[NUM_WEAPONS];
	int m_HealthOffset;
	int m_EmptyHealthOffset;
	int m_ArmorOffset;
	int m_EmptyArmorOffset;
	int m_aCursorOffset[NUM_WEAPONS];
	int m_FlagOffset;
	int m_AirjumpOffset;
	int m_AirjumpEmptyOffset;
	int m_aWeaponOffset[NUM_WEAPONS];
	int m_EndlessJumpOffset;
	int m_EndlessHookOffset;
	int m_JetpackOffset;
	int m_TeleportGrenadeOffset;
	int m_TeleportGunOffset;
	int m_TeleportLaserOffset;
	int m_SoloOffset;
	int m_CollisionDisabledOffset;
	int m_HookHitDisabledOffset;
	int m_HammerHitDisabledOffset;
	int m_GunHitDisabledOffset;
	int m_ShotgunHitDisabledOffset;
	int m_GrenadeHitDisabledOffset;
	int m_LaserHitDisabledOffset;
	int m_DeepFrozenOffset;
	int m_LiveFrozenOffset;
	int m_DummyHammerOffset;
	int m_DummyCopyOffset;
	int m_PracticeModeOffset;
	int m_Team0ModeOffset;
	int m_LockModeOffset;
};

#endif
