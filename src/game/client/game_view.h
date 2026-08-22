#ifndef GAME_CLIENT_GAME_VIEW_H
#define GAME_CLIENT_GAME_VIEW_H

#include "game_state.h"

#include <base/bezier.h>
#include <base/vmath.h>

#include <engine/client/session.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

class CGameSessionContext;

class CGameViewId
{
	uint64_t m_Value = 0;

public:
	CGameViewId() = default;
	explicit CGameViewId(uint64_t Value) :
		m_Value(Value)
	{
	}

	bool IsValid() const { return m_Value != 0; }
	uint64_t Value() const { return m_Value; }
	bool operator==(const CGameViewId &Other) const { return m_Value == Other.m_Value; }
	bool operator!=(const CGameViewId &Other) const { return !(*this == Other); }
};

class CViewport
{
public:
	int m_X = 0;
	int m_Y = 0;
	int m_Width = 0;
	int m_Height = 0;

	bool operator==(const CViewport &Other) const { return m_X == Other.m_X && m_Y == Other.m_Y && m_Width == Other.m_Width && m_Height == Other.m_Height; }
};

class CVisibleWorldRect
{
public:
	vec2 m_TopLeft;
	vec2 m_BottomRight;

	CVisibleWorldRect(vec2 TopLeft, vec2 BottomRight) :
		m_TopLeft(TopLeft),
		m_BottomRight(BottomRight)
	{
	}

	bool Inside(vec2 Position, vec2 Margin) const;
};

class CGameView
{
public:
	class CMotdPresentationState
	{
		CSessionId m_SessionId;
		uint64_t m_Revision = 0;
		int64_t m_VisibleUntil = 0;

	public:
		void Show(CSessionId SessionId, uint64_t Revision, int64_t VisibleUntil)
		{
			m_SessionId = SessionId;
			m_Revision = Revision;
			m_VisibleUntil = VisibleUntil;
		}
		void Dismiss() { m_VisibleUntil = 0; }
		bool IsActive(CSessionId SessionId, uint64_t Revision, int64_t Now) const
		{
			return m_SessionId == SessionId && m_Revision == Revision && Now < m_VisibleUntil;
		}
	};

	class CEmoticonSelectorState
	{
	public:
		bool m_WasActive = false;
		bool m_Active = false;
		vec2 m_SelectorMouse = vec2(0.0f, 0.0f);
		int m_SelectedEmote = -1;
		int m_SelectedEyeEmote = -1;
		bool m_TouchPressedOutside = false;
		CSessionId m_OriginSessionId;
		int m_OriginConnection = -1;

		void UpdateSelection(int NumEmoticons, int NumEyeEmotes, bool AllowEyeWheel);
		void Reset() { *this = {}; }
	};

	class CSpectatorSelectorState
	{
	public:
		static constexpr int MULTI_VIEW = -4;
		static constexpr int NO_SELECTION = -3;

		bool m_Active = false;
		bool m_WasActive = false;
		int m_SelectedSpectatorId = NO_SELECTION;
		int m_SelectedDDTeam = 0;
		vec2 m_SelectorMouse = vec2(0.0f, 0.0f);
		float m_MultiViewActivateTime = 0.0f;
		CSessionId m_OriginSessionId;
		CGameStateId m_OriginStateId;
		int m_OriginConnection = -1;
		bool m_OriginSixup = false;
		bool m_OriginDemo = false;
		int m_PendingSpectatorId = NO_SELECTION;
		int m_PendingDDTeam = 0;

		void UpdateSelection(float ObjWidth, float LineHeight, int PerLine, const std::array<int, MAX_CLIENTS> &aClients, int NumClients, bool AllowFollow);
		void Reset() { *this = {}; }
	};

	class CSpectatorCursorState
	{
	public:
		static constexpr int CURSOR_SAMPLES = 8;
		static constexpr int SAMPLE_FRAME_WINDOW = 3;
		static constexpr int SAMPLE_FRAME_OFFSET = 2;
		static constexpr double INTERP_DELAY = 4.25;
		static constexpr double REST_THRESHOLD = 3.0;

		int m_CursorOwnerId = -1;
		double m_aTargetSamplesTime[CURSOR_SAMPLES] = {};
		vec2 m_aTargetSamplesData[CURSOR_SAMPLES] = {};
		int m_NumSamples = 0;
		bool m_Available = false;
		int m_Weapon = 0;
		vec2 m_Target = vec2(0.0f, 0.0f);
		vec2 m_WorldTarget = vec2(0.0f, 0.0f);
		vec2 m_Position = vec2(0.0f, 0.0f);

		bool IsAvailable() const { return m_Available; }
		int Weapon() const { return m_Weapon; }
		vec2 Target() const { return m_Target; }
		vec2 WorldTarget() const { return m_WorldTarget; }
		vec2 Position() const { return m_Position; }
		void Reset() { *this = {}; }
	};

	class CMultiViewState
	{
	public:
		int m_Team = 0;
		float m_PersonalZoom = 0.0f;
		bool m_ShowHud = false;
		bool m_Active = false;
		bool m_aSelected[MAX_CLIENTS] = {};
		bool m_Solo = false;
		bool m_IsInit = false;
		bool m_Teleported = false;
		bool m_aVanish[MAX_CLIENTS] = {};
		vec2 m_OldPos = vec2(0.0f, 0.0f);
		int m_OldPersonalZoom = 0;
		float m_SecondChance = 0.0f;
		float m_OldCameraDistance = 0.0f;
		float m_aLastFreeze[MAX_CLIENTS] = {};

		void Reset() { *this = {}; }
	};

	class CCameraState
	{
	public:
		int m_CamType = -1;
		vec2 m_PrevCenter = vec2(0.0f, 0.0f);
		int m_PrevSpecId = -1;
		bool m_WasSpectating = false;
		bool m_CameraSmoothing = false;
		vec2 m_CameraSmoothingCenter = vec2(0.0f, 0.0f);
		vec2 m_CameraSmoothingTarget = vec2(0.0f, 0.0f);
		CCubicBezier m_CameraSmoothingBezierX = {};
		CCubicBezier m_CameraSmoothingBezierY = {};
		float m_CameraSmoothingStart = 0.0f;
		float m_CameraSmoothingEnd = 0.0f;
		vec2 m_CenterBeforeSmoothing = vec2(0.0f, 0.0f);
		CCubicBezier m_ZoomSmoothing = {};
		float m_ZoomSmoothingStart = 0.0f;
		float m_ZoomSmoothingEnd = 0.0f;
		vec2 m_LastTargetPos = vec2(0.0f, 0.0f);
		float m_DyncamSmoothingSpeedBias = 0.5f;
		bool m_CanUseCameraInfo = false;
		bool m_CanUseAutoSpecCamera = false;
		bool m_UsingAutoSpecCamera = false;
		vec2 m_Center = vec2(0.0f, 0.0f);
		bool m_ZoomSet = false;
		bool m_Zooming = false;
		float m_Zoom = 1.0f;
		float m_ZoomSmoothingTarget = 0.0f;
		bool m_AutoSpecCameraZooming = false;
		bool m_AutoSpecCamera = true;
		float m_UserZoomTarget = 0.0f;
		vec2 m_DyncamTargetCameraOffset = vec2(0.0f, 0.0f);
		vec2 m_DynamicCameraOffset = vec2(0.0f, 0.0f);
		vec2 m_LastInputPosition = vec2(0.0f, 0.0f);
		bool m_ForceFreeview = false;
		vec2 m_ForceFreeviewPos = vec2(0.0f, 0.0f);
		int m_GotoSwitchOffset = 0;
		int m_GotoTeleOffset = 0;
		ivec2 m_GotoSwitchLastPos = ivec2(-1, -1);
		ivec2 m_GotoTeleLastPos = ivec2(-1, -1);
		int m_GotoTeleLastNumber = -1;
	};

private:
	CGameViewId m_Id;
	CSessionId m_SessionId;
	CGameStateId m_StateId;
	CViewport m_Viewport;
	CCameraState m_Camera;
	CMotdPresentationState m_Motd;
	CEmoticonSelectorState m_EmoticonSelector;
	CSpectatorSelectorState m_SpectatorSelector;
	CSpectatorCursorState m_SpectatorCursor;
	CMultiViewState m_MultiView;
	vec2 m_CursorPosition = vec2(0.0f, 0.0f);
	bool m_Spectating = false;
	int m_SpectatorId = -1;
	int m_SpectatorMode = SPEC_FREEVIEW;

public:
	CGameView(CGameViewId Id, CSessionId SessionId, CGameStateId StateId) :
		m_Id(Id),
		m_SessionId(SessionId),
		m_StateId(StateId)
	{
	}

	CGameViewId Id() const { return m_Id; }
	CSessionId SessionId() const { return m_SessionId; }
	CGameStateId StateId() const { return m_StateId; }
	bool MatchesTarget(CSessionId SessionId, CGameStateId StateId) const { return m_SessionId == SessionId && m_StateId == StateId; }
	bool MatchesBinding(CGameViewId ViewId, CSessionId SessionId, CGameStateId StateId) const { return m_Id == ViewId && MatchesTarget(SessionId, StateId); }
	void SetTarget(CSessionId SessionId, CGameStateId StateId)
	{
		if(m_SessionId != SessionId || m_StateId != StateId)
		{
			m_SpectatorCursor.Reset();
			m_SpectatorSelector.Reset();
		}
		if(m_SessionId != SessionId)
			m_MultiView.Reset();
		m_SessionId = SessionId;
		m_StateId = StateId;
	}
	const CViewport &Viewport() const { return m_Viewport; }
	void SetViewport(CViewport Viewport) { m_Viewport = Viewport; }
	CCameraState &Camera() { return m_Camera; }
	const CCameraState &Camera() const { return m_Camera; }
	CMotdPresentationState &Motd() { return m_Motd; }
	const CMotdPresentationState &Motd() const { return m_Motd; }
	CEmoticonSelectorState &EmoticonSelector() { return m_EmoticonSelector; }
	const CEmoticonSelectorState &EmoticonSelector() const { return m_EmoticonSelector; }
	CSpectatorSelectorState &SpectatorSelector() { return m_SpectatorSelector; }
	const CSpectatorSelectorState &SpectatorSelector() const { return m_SpectatorSelector; }
	CSpectatorCursorState &SpectatorCursor() { return m_SpectatorCursor; }
	const CSpectatorCursorState &SpectatorCursor() const { return m_SpectatorCursor; }
	CMultiViewState &MultiView() { return m_MultiView; }
	const CMultiViewState &MultiView() const { return m_MultiView; }
	vec2 CameraPosition() const { return m_Camera.m_Center; }
	void SetCameraPosition(vec2 Position) { m_Camera.m_Center = Position; }
	float Zoom() const { return m_Camera.m_Zoom; }
	void SetZoom(float Zoom) { m_Camera.m_Zoom = Zoom; }
	vec2 CursorPosition() const { return m_CursorPosition; }
	void SetCursorPosition(vec2 Position) { m_CursorPosition = Position; }
	bool IsSpectating() const { return m_Spectating; }
	int SpectatorId() const { return m_SpectatorId; }
	int SpectatorMode() const { return m_SpectatorMode; }
	void SetSpectatorMode(int SpectatorMode) { m_SpectatorMode = SpectatorMode; }
	void SetSpectator(bool Spectating, int SpectatorId = -1)
	{
		m_Spectating = Spectating;
		m_SpectatorId = Spectating ? SpectatorId : -1;
		m_SpectatorMode = Spectating ? SpectatorId : SPEC_FREEVIEW;
	}
};

class CGameViewManager
{
	uint64_t m_NextId = 1;
	std::vector<std::unique_ptr<CGameView>> m_vpViews;

public:
	CGameViewId Create(CSessionId SessionId, CGameStateId StateId);
	CGameView *Find(CGameViewId Id);
	const CGameView *Find(CGameViewId Id) const;
	bool Destroy(CGameViewId Id);
	size_t NumViews() const { return m_vpViews.size(); }
};

enum class EPresentationPlayback
{
	PAUSED,
	PLAYING,
};

enum class EPresentationAudio
{
	MUTED,
	AUDIBLE,
};

class CPresentationContext
{
public:
	const CGameSessionContext &m_Session;
	CGameState &m_State;
	CGameTickInfo m_Time;
	std::span<const CVisibleWorldRect> m_vVisibleWorldRects;
	EPresentationPlayback m_Playback;
	EPresentationAudio m_Audio;

	CPresentationContext(const CGameSessionContext &Session, CGameState &State, CGameTickInfo Time, std::span<const CVisibleWorldRect> vVisibleWorldRects, EPresentationPlayback Playback, EPresentationAudio Audio);

	bool IsVisible(vec2 Position, vec2 Margin) const;
	bool IsOtherTeamFromLocalPlayer(int ClientId) const;
};

class CRenderContext
{
public:
	const CGameSessionContext &m_Session;
	const CGameState &m_State;
	const CGameView &m_View;
	CGameTickInfo m_Time;
	CVisibleWorldRect m_VisibleWorldRect;
	uint64_t m_OutputCacheKey;
	bool m_IsVideoOutput;

	CRenderContext(const CGameSessionContext &Session, const CGameState &State, const CGameView &View, CGameTickInfo Time, CVisibleWorldRect VisibleWorldRect, uint64_t OutputCacheKey = 0, bool IsVideoOutput = false);

	float AspectRatio(float DefaultAspectRatio) const;
	bool IsOtherTeam(int ClientId) const;
	float AlphaForOwner(int OwnerClientId, float OtherTeamAlpha) const;
};

class CRenderOutput
{
public:
	virtual ~CRenderOutput() = default;
	virtual uint64_t PresentationCacheKey() const { return reinterpret_cast<uintptr_t>(this); }
	virtual bool IsVideoOutput() const { return false; }
	virtual void BeginView(const CViewport &Viewport, vec2 CameraPosition, float Zoom) = 0;
	virtual void DrawCharacter(int ClientId, vec2 Position, bool Local) = 0;
	virtual void DrawSpectatorCharacter(int ClientId, vec2 Position, bool OtherTeam) = 0;
	virtual void EndView() = 0;
};

class CGameRenderRequest
{
public:
	const CGameSessionContext &m_Session;
	CGameState &m_State;
	const CGameView &m_View;
	CGameTickInfo m_Time;
	CVisibleWorldRect m_VisibleWorldRect;
	EPresentationPlayback m_Playback;
	EPresentationAudio m_Audio;
	CRenderOutput &m_Output;

	CGameRenderRequest(const CGameSessionContext &Session, CGameState &State, const CGameView &View, CGameTickInfo Time, CVisibleWorldRect VisibleWorldRect, EPresentationPlayback Playback, EPresentationAudio Audio, CRenderOutput &Output);
};

const CGameRenderRequest *FindAudibleRenderRequest(std::span<const CGameRenderRequest> vRequests);

class CGameRenderScheduler
{
public:
	using FUpdatePresentation = std::function<void(const CPresentationContext &Context)>;
	using FRenderView = std::function<void(const CRenderContext &Context, CRenderOutput &Output)>;

	void Run(std::span<const CGameRenderRequest> vRequests, const FUpdatePresentation &UpdatePresentation, const FRenderView &RenderView);

private:
	class CStateGroup
	{
	public:
		const CGameSessionContext *m_pSession = nullptr;
		CGameState *m_pState = nullptr;
		CGameTickInfo m_Time;
		EPresentationPlayback m_Playback = EPresentationPlayback::PLAYING;
		EPresentationAudio m_Audio = EPresentationAudio::MUTED;
		std::vector<CVisibleWorldRect> m_vVisibleWorldRects;
	};
	// Kept between runs: a frame runs the scheduler several times over the same
	// requests, so the groups it forms would otherwise be built from nothing every
	// time. Entries beyond the ones a run uses are kept for their storage.
	std::vector<CStateGroup> m_vGroups;
	std::vector<size_t> m_vRequestGroups;
};

class CGameStateRenderer
{
public:
	void Render(const CRenderContext &Context, CRenderOutput &Output) const;
};

#endif // GAME_CLIENT_GAME_VIEW_H
