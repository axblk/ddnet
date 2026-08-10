/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CAMERA_H
#define GAME_CLIENT_COMPONENTS_CAMERA_H
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/game_view.h>

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
	CGameView::CCameraState &State();
	const CGameView::CCameraState &State() const;

	float CameraSmoothingProgress(float CurrentTime) const;

	void ScaleZoom(float Factor);
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
	void OnRender() override;

	// DDRace

	void OnConsoleInit() override;
	void OnReset() override;

	void SetView(ivec2 Pos, bool Relative = false);
	void GotoSwitch(int Number, int Offset = -1);
	void GotoTele(int Number, int Offset = -1);

	void SetZoom(float Target, int Smoothness, bool IsUser);
	bool ZoomAllowed() const;

	int Deadzone() const;
	int FollowFactor() const;
	int CamType() const { return State().m_CamType; }
	vec2 Center() const { return State().m_Center; }
	float Zoom() const { return State().m_Zoom; }
	bool IsZoomSet() const { return State().m_ZoomSet; }
	bool IsZooming() const { return State().m_Zooming; }
	float ZoomSmoothingTarget() const { return State().m_ZoomSmoothingTarget; }
	bool IsAutoSpecCameraZooming() const { return State().m_AutoSpecCameraZooming; }
	bool IsAutoSpecCamera() const { return State().m_AutoSpecCamera; }
	void SetAutoSpecCamera(bool Enabled) { State().m_AutoSpecCamera = Enabled; }
	vec2 DynamicCameraTargetOffset() const { return State().m_DyncamTargetCameraOffset; }
	vec2 DynamicCameraOffset() const { return State().m_DynamicCameraOffset; }
	void BindState(CGameView::CCameraState &State) { m_pState = &State; }

	void UpdateCamera();
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
