/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_VIEW_CONTROL_H
#define GAME_CLIENT_VIEW_CONTROL_H

#include <engine/view_control.h>

#include <functional>

class CCamera;
class CGameClient;

/**
 * The game's side of `IViewControl`.
 */
class CGameViewControl final : public IViewControl
{
	CGameClient &m_GameClient;

	/**
	 * Runs `Use` with the camera on the view of a session, and puts the
	 * camera back on the view that takes input afterwards.
	 */
	void WithCamera(CSessionId SessionId, const std::function<void(CCamera &Camera)> &Use) const;

public:
	explicit CGameViewControl(CGameClient &GameClient) :
		m_GameClient(GameClient)
	{
	}

	int SpectatorId(CSessionId SessionId) const override;
	void SetSpectatorId(CSessionId SessionId, int SpectatorId) override;
	bool PlayerName(CSessionId SessionId, int ClientId, char *pBuffer, int BufferSize) const override;

	void MoveFreeView(CSessionId SessionId, vec2 Offset) override;
	float Zoom(CSessionId SessionId) const override;
	void ScaleZoom(CSessionId SessionId, float Factor) override;
	void ResetZoom(CSessionId SessionId) override;
	bool ZoomChanged(CSessionId SessionId) const override;
	bool RecordedCameraAvailable(CSessionId SessionId) const override;
	bool RecordedCamera(CSessionId SessionId) const override;
	void SetRecordedCamera(CSessionId SessionId, bool Use) override;

	CViewRenderOptions RenderOptions(CSessionId SessionId) const override;
	void SetRenderOptions(CSessionId SessionId, const CViewRenderOptions &Options) override;
};

#endif // GAME_CLIENT_VIEW_CONTROL_H
