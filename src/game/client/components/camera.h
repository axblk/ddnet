/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CAMERA_H
#define GAME_CLIENT_COMPONENTS_CAMERA_H
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/game_state.h>
#include <game/client/game_view.h>

class CGameSessionContext;

class CCamera : public CComponent
{
public:
	enum
	{
		CAMTYPE_UNDEFINED = -1,
		CAMTYPE_SPEC,
		CAMTYPE_PLAYER,
	};

private:
	CGameView::CCameraState *m_pState = nullptr;
	// The view the camera currently drives. Only an interactive one takes input.
	CGameSessionContext *m_pSession = nullptr;
	CGameState *m_pGameState = nullptr;
	CGameView *m_pView = nullptr;
	bool m_Interactive = true;
	float m_LocalTime = 0.0f;
	CGameView::CCameraState &State();
	const CGameView::CCameraState &State() const;
	CGameSessionContext &Session() const;
	CGameState &GameState() const;
	CGameView &View() const;
	CGameState::CSnapState &Snap() const;
	CGameView::CMultiViewState &MultiView() const;
	vec2 LocalCharacterPos() const;
	bool IsDemoSession() const;
	bool IsLocalClientId(int ClientId) const;

	float CameraSmoothingProgress(float CurrentTime) const;

	void ChangeZoom(float Target, int Smoothness, bool IsUser);
	float ZoomProgress(float CurrentTime) const;

	float MinZoomLevel();
	float MaxZoomLevel();

	char m_aAutoSpecCameraTooltip[512];

public:
	static constexpr float ZOOM_STEP = 0.866025f;

	/**
	 * Convert zoom steps to zoom value
	 *
	 * @param Steps - Zoom steps, 0.0f converts to default zoom (returns 1.0f)
	 * @return converted zoom value
	 **/
	static float ZoomStepsToValue(float Steps) { return std::pow(CCamera::ZOOM_STEP, Steps); }

	CCamera();
	int Sizeof() const override { return sizeof(*this); }

	// DDRace

	void OnConsoleInit() override;
	void OnReset() override;

	void SetView(ivec2 Pos, bool Relative = false);
	/**
	 * The same in world units rather than in tiles, for dragging the free view.
	 *
	 * @param Pos Where to look, or how far to move when `Relative`.
	 * @param Relative Whether `Pos` is measured from where the view is now.
	 */
	void SetViewPos(vec2 Pos, bool Relative = false);
	/** Multiplies the zoom, as one notch of a wheel does. */
	void ScaleZoom(float Factor);
	void GotoSwitch(int Number, int Offset = -1);
	void GotoTele(int Number, int Offset = -1);

	void SetZoom(float Target, int Smoothness, bool IsUser);
	bool ZoomAllowed() const;

	int Deadzone() const;
	int FollowFactor() const;
	int CamType() const { return State().m_CamType; }
	vec2 Center() const { return State().m_Center; }
	float Zoom() const { return State().m_Zoom; }
	/**
	 * The zoom last asked for, which differs from `Zoom` while the camera follows
	 * the one a demo brought.
	 */
	float UserZoomTarget() const { return State().m_UserZoomTarget; }
	bool IsZoomSet() const { return State().m_ZoomSet; }
	bool IsZooming() const { return State().m_Zooming; }
	float ZoomSmoothingTarget() const { return State().m_ZoomSmoothingTarget; }
	bool IsAutoSpecCameraZooming() const { return State().m_AutoSpecCameraZooming; }
	bool IsAutoSpecCamera() const { return State().m_AutoSpecCamera; }
	void SetAutoSpecCamera(bool Enabled) { State().m_AutoSpecCamera = Enabled; }
	vec2 DynamicCameraTargetOffset() const { return State().m_DyncamTargetCameraOffset; }
	vec2 DynamicCameraOffset() const { return State().m_DynamicCameraOffset; }
	void BindState(CGameView::CCameraState &State) { m_pState = &State; }
	void BindTarget(CGameSessionContext &Session, CGameState &State, CGameView &View, bool Interactive, float LocalTime);

	void UpdateCamera();
	void UpdatePosition();
	void ResetAutoSpecCamera();
	bool SpectatingPlayer() const { return State().m_CanUseCameraInfo; }
	bool CanUseAutoSpecCamera() const;
	void ToggleAutoSpecCamera();
	void UpdateAutoSpecCameraTooltip();

	const char *AutoSpecCameraTooltip() { return m_aAutoSpecCameraTooltip; }

private:
	static void ConZoomPlus(IConsole::IResult *pResult, void *pUserData);
	static void ConZoomMinus(IConsole::IResult *pResult, void *pUserData);
	static void ConZoom(IConsole::IResult *pResult, void *pUserData);
	static void ConSetView(IConsole::IResult *pResult, void *pUserData);
	static void ConSetViewRelative(IConsole::IResult *pResult, void *pUserData);
	static void ConGotoSwitch(IConsole::IResult *pResult, void *pUserData);
	static void ConGotoTele(IConsole::IResult *pResult, void *pUserData);
};

#endif
