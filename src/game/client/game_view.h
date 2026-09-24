#ifndef GAME_CLIENT_GAME_VIEW_H
#define GAME_CLIENT_GAME_VIEW_H

#include "game_state.h"

#include <base/bezier.h>
#include <base/vmath.h>

#include <engine/client/session.h>
#include <engine/shared/video.h>

#include <array>
#include <cstdint>
#include <span>

class CGameSessionContext;

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

class CGameView;

/**
 * A view together with the session it showed at some point.
 */
class CViewBinding
{
public:
	const CGameView *m_pView = nullptr;
	CSessionId m_SessionId;

	bool operator==(const CViewBinding &Other) const { return m_pView == Other.m_pView && m_SessionId == Other.m_SessionId; }
	bool operator!=(const CViewBinding &Other) const { return !(*this == Other); }
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
		float m_UserZoomTarget = 1.0f;
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

	CCameraState m_Camera;
	CMotdPresentationState m_Motd;
	CEmoticonSelectorState m_EmoticonSelector;
	CSpectatorSelectorState m_SpectatorSelector;
	CSpectatorCursorState m_SpectatorCursor;
	CMultiViewState m_MultiView;

private:
	// The session shown: a server, the dummy on it, or a demo.
	CSessionId m_SessionId;
	CViewport m_Viewport;
	vec2 m_CursorPosition = vec2(0.0f, 0.0f);
	bool m_Spectating = false;
	int m_SpectatorId = -1;
	int m_SpectatorMode = SPEC_FREEVIEW;

public:
	CSessionId SessionId() const { return m_SessionId; }
	CViewBinding Binding() const { return {this, m_SessionId}; }
	void SetTarget(CSessionId SessionId)
	{
		if(m_SessionId != SessionId)
			m_MultiView.Reset();
		SwitchSeat(SessionId);
	}
	/**
	 * Shows the other seat on the same server; what the view follows on the
	 * server is kept.
	 */
	void SwitchSeat(CSessionId SessionId)
	{
		if(m_SessionId != SessionId)
		{
			m_SpectatorCursor.Reset();
			m_SpectatorSelector.Reset();
		}
		m_SessionId = SessionId;
	}
	const CViewport &Viewport() const { return m_Viewport; }
	void SetViewport(CViewport Viewport) { m_Viewport = Viewport; }
	/**
	 * Where a point given as a fraction of the whole screen lies, as a
	 * fraction of this view.
	 */
	vec2 ScreenFractionToView(vec2 Fraction, vec2 ScreenSize) const
	{
		if(m_Viewport.m_Width <= 0 || m_Viewport.m_Height <= 0)
			return Fraction;
		return (Fraction * ScreenSize - vec2(m_Viewport.m_X, m_Viewport.m_Y)) / vec2(m_Viewport.m_Width, m_Viewport.m_Height);
	}
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

/**
 * Everything besides the content that decides how an overlay lays out what it
 * draws in a view: whose it is, and the size of the view in pixels. Where on
 * the screen the view sits does not move a glyph, so views of the same size
 * share what was laid out for any of them.
 */
class CLayoutKey
{
public:
	/**
	 * The session whose text it is: the server or demo for what all of its
	 * seats read alike, the seat's own session for what differs between them.
	 */
	CSessionId m_SessionId;
	int m_Width = 0;
	int m_Height = 0;

	bool operator==(const CLayoutKey &Other) const = default;
};

/**
 * What an overlay laid out, kept once for each differently keyed view it is
 * drawn in. The views of a frame take turns, and with a single copy every one
 * of them would throw away and build again what the one before it laid out.
 */
template<class TLayout>
class CLayoutCache
{
	class CSlot
	{
	public:
		CLayoutKey m_Key;
		uint64_t m_LastUse = 0;
		TLayout m_Layout;
	};
	// A column for the player, the dummy and the demo.
	std::array<CSlot, 3> m_aSlots;
	uint64_t m_Uses = 0;

public:
	/**
	 * The layout kept for the key. When there is none, the one used longest
	 * ago is handed to `Clear` and then taken over.
	 */
	template<class FClear>
	TLayout &Find(const CLayoutKey &Key, FClear &&Clear)
	{
		CSlot *pOldest = m_aSlots.data();
		for(CSlot &Slot : m_aSlots)
		{
			if(Slot.m_LastUse != 0 && Slot.m_Key == Key)
			{
				Slot.m_LastUse = ++m_Uses;
				return Slot.m_Layout;
			}
			if(Slot.m_LastUse < pOldest->m_LastUse)
				pOldest = &Slot;
		}
		Clear(pOldest->m_Layout);
		pOldest->m_Key = Key;
		pOldest->m_LastUse = ++m_Uses;
		return pOldest->m_Layout;
	}

	template<class FClear>
	void ClearAll(FClear &&Clear)
	{
		for(CSlot &Slot : m_aSlots)
		{
			Clear(Slot.m_Layout);
			Slot.m_LastUse = 0;
		}
	}
};

class CRenderContext
{
public:
	const CGameSessionContext &m_Session;
	const CGameState &m_State;
	const CGameView &m_View;
	CGameTickInfo m_Time;
	CVisibleWorldRect m_VisibleWorldRect;
	bool m_IsVideoOutput;
	CVideoExportSettings m_VideoSettings;

	CRenderContext(const CGameSessionContext &Session, const CGameState &State, const CGameView &View, CGameTickInfo Time, CVisibleWorldRect VisibleWorldRect, bool IsVideoOutput = false, CVideoExportSettings VideoSettings = {});

	float AspectRatio(float DefaultAspectRatio) const;
	/**
	 * The key for what an overlay lays out in this view.
	 *
	 * @param PerState Pass true for what differs between the seats of one
	 * server, such as whose name plate is the local one.
	 */
	CLayoutKey LayoutKey(bool PerState) const;
	bool IsOtherTeam(int ClientId) const;
	float AlphaForOwner(int OwnerClientId, float OtherTeamAlpha) const;
};

#endif // GAME_CLIENT_GAME_VIEW_H
