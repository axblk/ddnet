#ifndef GAME_CLIENT_GAME_VIEW_H
#define GAME_CLIENT_GAME_VIEW_H

#include "game_state.h"

#include <base/vmath.h>

#include <cstdint>
#include <memory>
#include <vector>

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

class CGameView
{
	CGameViewId m_Id;
	CGameStateId m_StateId;
	CViewport m_Viewport;
	vec2 m_CameraPosition = vec2(0.0f, 0.0f);
	float m_Zoom = 1.0f;
	vec2 m_CursorPosition = vec2(0.0f, 0.0f);
	bool m_Spectating = false;
	int m_SpectatorId = -1;

public:
	CGameView(CGameViewId Id, CGameStateId StateId) :
		m_Id(Id),
		m_StateId(StateId)
	{
	}

	CGameViewId Id() const { return m_Id; }
	CGameStateId StateId() const { return m_StateId; }
	void SetStateId(CGameStateId StateId) { m_StateId = StateId; }
	const CViewport &Viewport() const { return m_Viewport; }
	void SetViewport(CViewport Viewport) { m_Viewport = Viewport; }
	vec2 CameraPosition() const { return m_CameraPosition; }
	void SetCameraPosition(vec2 Position) { m_CameraPosition = Position; }
	float Zoom() const { return m_Zoom; }
	void SetZoom(float Zoom) { m_Zoom = Zoom; }
	vec2 CursorPosition() const { return m_CursorPosition; }
	void SetCursorPosition(vec2 Position) { m_CursorPosition = Position; }
	bool IsSpectating() const { return m_Spectating; }
	int SpectatorId() const { return m_SpectatorId; }
	void SetSpectator(bool Spectating, int SpectatorId = -1)
	{
		m_Spectating = Spectating;
		m_SpectatorId = Spectating ? SpectatorId : -1;
	}
};

class CGameViewManager
{
	uint64_t m_NextId = 1;
	std::vector<std::unique_ptr<CGameView>> m_vpViews;

public:
	CGameViewId Create(CGameStateId StateId);
	CGameView *Find(CGameViewId Id);
	const CGameView *Find(CGameViewId Id) const;
	bool Destroy(CGameViewId Id);
	size_t NumViews() const { return m_vpViews.size(); }
};

class CRenderContext
{
public:
	const CGameState &m_State;
	const CGameView &m_View;
};

class CRenderOutput
{
public:
	virtual ~CRenderOutput() = default;
	virtual void BeginView(const CViewport &Viewport, vec2 CameraPosition, float Zoom) = 0;
	virtual void DrawCharacter(int ClientId, vec2 Position, bool Local) = 0;
	virtual void EndView() = 0;
};

class CGameStateRenderer
{
public:
	void Render(const CRenderContext &Context, CRenderOutput &Output) const;
};

#endif // GAME_CLIENT_GAME_VIEW_H
