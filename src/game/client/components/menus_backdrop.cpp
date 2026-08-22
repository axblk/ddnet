/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "menus.h"

#include <base/color.h>
#include <base/log.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/components/console.h>
#include <game/client/components/motd.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/statboard.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>

#include <algorithm>
#include <cmath>
#include <optional>

// The blurred backdrop is not a menu of its own: the scoreboard, the message of
// the day and the statistics board draw over it as well, and a demo rendered to
// a video file shows all three. It lives apart from the menu pages so that a
// build without them still has it.

void CMenus::DestroyMenuBackdropTextures()
{
	m_MenuBackdropActive = false;
	m_MenuBackdropOverlayActive = false;
	m_MenuBackdropReady = false;
	Graphics()->UnloadTexture(&m_MenuBackdropSceneTexture);
	Graphics()->UnloadTexture(&m_MenuBackdropOverlayTexture);
	for(IGraphics::CTextureHandle &Texture : m_aMenuBackdropDownsampleTextures)
		Graphics()->UnloadTexture(&Texture);
	Graphics()->UnloadTexture(&m_aMenuBackdropBlurTextures[0]);
	Graphics()->UnloadTexture(&m_aMenuBackdropBlurTextures[1]);
	m_MenuBackdropWidth = 0;
	m_MenuBackdropHeight = 0;
}

bool CMenus::EnsureMenuBackdropTextures()
{
	const int Width = Graphics()->ScreenWidth();
	const int Height = Graphics()->ScreenHeight();
	if(Width <= 0 || Height <= 0)
		return false;
	if(Width == m_MenuBackdropWidth && Height == m_MenuBackdropHeight)
		return MenuBackdropTexturesValid();

	DestroyMenuBackdropTextures();
	m_MenuBackdropWidth = Width;
	m_MenuBackdropHeight = Height;

	IGraphics::CTextureDesc Desc;
	Desc.m_Width = Width;
	Desc.m_Height = Height;
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Desc.m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED | IGraphics::TEXTURE_USAGE_COLOR_TARGET;
	m_MenuBackdropSceneTexture = Graphics()->CreateTexture(Desc);
	m_MenuBackdropOverlayTexture = Graphics()->CreateTexture(Desc);

	// The two blur passes run on an eighth of the screen, which is what decides
	// how coarse the result looks. Getting down there in one step would sample
	// four of the sixty-four pixels a target pixel covers, and which four
	// changes as the scene moves, which is what made the blur crawl. Halving
	// three times averages all of them.
	for(int i = 0; i < NUM_MENU_BACKDROP_DOWNSAMPLES; ++i)
	{
		Desc.m_Width = std::max(1, (Width + (2 << i) - 1) / (2 << i));
		Desc.m_Height = std::max(1, (Height + (2 << i) - 1) / (2 << i));
		m_aMenuBackdropDownsampleTextures[i] = Graphics()->CreateTexture(Desc);
	}
	m_aMenuBackdropBlurTextures[0] = Graphics()->CreateTexture(Desc);
	m_aMenuBackdropBlurTextures[1] = Graphics()->CreateTexture(Desc);
	if(MenuBackdropTexturesValid())
	{
		log_debug("menus", "Created menu backdrop targets: scene=%dx%d blur=%dx%d", Width, Height, static_cast<int>(Desc.m_Width), static_cast<int>(Desc.m_Height));
		return true;
	}

	DestroyMenuBackdropTextures();
	log_debug("menus", "Menu backdrop render targets unavailable, using direct rendering");
	return false;
}

bool CMenus::MenuBackdropTexturesValid() const
{
	if(!m_MenuBackdropSceneTexture.IsValid() || !m_MenuBackdropOverlayTexture.IsValid())
		return false;
	for(const IGraphics::CTextureHandle &Texture : m_aMenuBackdropDownsampleTextures)
	{
		if(!Texture.IsValid())
			return false;
	}
	return m_aMenuBackdropBlurTextures[0].IsValid() && m_aMenuBackdropBlurTextures[1].IsValid();
}

bool CMenus::RenderMenuBackdropTexture(IGraphics::CTextureHandle Target, IGraphics::CTextureHandle Source, std::optional<IGraphics::EBlurDirection> BlurDirection)
{
	IGraphics::CRenderPassDesc Pass;
	Pass.m_ColorTarget = Target;
	if(!Graphics()->BeginRenderPass(Pass))
		return false;
	const bool Drawn = BlurDirection.has_value() ? Graphics()->BlurTexture(Source, BlurDirection.value()) : Graphics()->BlitTexture(Source);
	const bool Ended = Graphics()->EndRenderPass();
	return Drawn && Ended;
}

bool CMenus::BlurIntoMenuBackdrop(IGraphics::CTextureHandle Source)
{
	IGraphics::CTextureHandle Current = Source;
	for(const IGraphics::CTextureHandle &Downsample : m_aMenuBackdropDownsampleTextures)
	{
		if(!RenderMenuBackdropTexture(Downsample, Current, std::nullopt))
			return false;
		Current = Downsample;
	}
	return RenderMenuBackdropTexture(m_aMenuBackdropBlurTextures[1], Current, IGraphics::EBlurDirection::HORIZONTAL) &&
	       RenderMenuBackdropTexture(m_aMenuBackdropBlurTextures[0], m_aMenuBackdropBlurTextures[1], IGraphics::EBlurDirection::VERTICAL);
}

bool CMenus::BeginMenuBackdrop(ColorRGBA ClearColor)
{
	m_MenuBackdropActive = false;
	m_MenuBackdropOverlayActive = false;
	m_MenuBackdropReady = false;
	m_MenuBackdropBackgroundRendered = false;
	if(!g_Config.m_ClMenuBackgroundBlur)
	{
		if(m_MenuBackdropSceneTexture.IsValid())
			DestroyMenuBackdropTextures();
		return false;
	}

	const IClient::EClientState ClientState = Client()->State();
	if(!BackdropConsumerActive() && (ClientState == IClient::STATE_ONLINE || ClientState == IClient::STATE_DEMOPLAYBACK))
		return false;
	if(!EnsureMenuBackdropTextures())
		return false;

	IGraphics::CRenderPassDesc Pass;
	Pass.m_ColorTarget = m_MenuBackdropSceneTexture;
	Pass.m_LoadOp = IGraphics::ERenderPassLoadOp::CLEAR;
	Pass.m_ClearColor = ClearColor.WithAlpha(0.0f);
	m_MenuBackdropActive = Graphics()->BeginRenderPass(Pass);
	return m_MenuBackdropActive;
}

void CMenus::FinishMenuBackdrop()
{
	if(!m_MenuBackdropActive)
		return;

	const IClient::EClientState ClientState = Client()->State();
	const bool RenderedBackground = ClientState != IClient::STATE_ONLINE && ClientState != IClient::STATE_DEMOPLAYBACK;
	if(RenderedBackground)
		RenderMenuBackground();

	const bool SceneEnded = Graphics()->EndRenderPass();
	// The console blurs its own picture later, so a frame where it is the
	// only thing over the scene does not need the scene blurred at all.
	const bool ApplyBlur = SceneBackdropConsumerActive() || RenderedBackground;
	const bool Blurred = SceneEnded && ApplyBlur && BlurIntoMenuBackdrop(m_MenuBackdropSceneTexture);

	// Everything that is drawn over the scene from here on goes into a second
	// picture rather than straight to the screen, so that whatever is drawn
	// last can have a blurred copy of all of it. The console is what needs
	// that: it covers the menu just as it covers the game.
	IGraphics::CRenderPassDesc OverlayPass;
	OverlayPass.m_ColorTarget = m_MenuBackdropOverlayTexture;
	m_MenuBackdropOverlayActive = Graphics()->BeginRenderPass(OverlayPass) && Graphics()->BlitTexture(m_MenuBackdropSceneTexture);
	if(!m_MenuBackdropOverlayActive)
	{
		IGraphics::CRenderPassDesc PresentationPass;
		const bool Started = Graphics()->BeginRenderPass(PresentationPass);
		const bool Composited = Started && Graphics()->BlitTexture(m_MenuBackdropSceneTexture);
		m_MenuBackdropReady = Blurred && Composited;
		m_MenuBackdropBackgroundRendered = RenderedBackground && Composited;
		m_MenuBackdropActive = false;
		return;
	}
	m_MenuBackdropReady = Blurred;
	m_MenuBackdropBackgroundRendered = RenderedBackground;
	m_MenuBackdropActive = false;
}

bool CMenus::CaptureMenuBackdrop()
{
	if(!m_MenuBackdropOverlayActive)
		return false;
	const bool Ended = Graphics()->EndRenderPass();
	const bool Blurred = Ended && BlurIntoMenuBackdrop(m_MenuBackdropOverlayTexture);
	m_MenuBackdropOverlayActive = false;
	IGraphics::CRenderPassDesc PresentationPass;
	const bool Started = Graphics()->BeginRenderPass(PresentationPass);
	const bool Composited = Started && Graphics()->BlitTexture(m_MenuBackdropOverlayTexture);
	m_MenuBackdropReady = Blurred && Composited;
	return m_MenuBackdropReady;
}

void CMenus::PresentMenuBackdrop()
{
	if(!m_MenuBackdropOverlayActive)
		return;
	const bool Ended = Graphics()->EndRenderPass();
	m_MenuBackdropOverlayActive = false;
	IGraphics::CRenderPassDesc PresentationPass;
	if(Ended && Graphics()->BeginRenderPass(PresentationPass))
		Graphics()->BlitTexture(m_MenuBackdropOverlayTexture);
	m_MenuBackdropReady = false;
}

bool CMenus::SceneBackdropConsumerActive() const
{
	return IsActive() || GameClient()->m_Scoreboard.IsActive() || GameClient()->m_Statboard.IsActive() || GameClient()->m_Motd.IsActive();
}

bool CMenus::BackdropConsumerActive() const
{
	return SceneBackdropConsumerActive() || GameClient()->m_GameConsole.IsActive();
}

void CMenus::RenderBackdropRegion(CUIRect Rect)
{
	if(!m_MenuBackdropReady || Rect.w <= 0.0f || Rect.h <= 0.0f)
		return;

	const CScreenRect Screen = Graphics()->GetScreen();
	const float Left = std::max(Rect.x, Screen.m_TopLeft.x);
	const float Top = std::max(Rect.y, Screen.m_TopLeft.y);
	const float Right = std::min(Rect.x + Rect.w, Screen.m_BottomRight.x);
	const float Bottom = std::min(Rect.y + Rect.h, Screen.m_BottomRight.y);
	if(Left >= Right || Top >= Bottom)
		return;

	const float XScale = Graphics()->ScreenWidth() / Screen.Width();
	const float YScale = Graphics()->ScreenHeight() / Screen.Height();
	const int ClipX = std::round((Left - Screen.m_TopLeft.x) * XScale);
	const int ClipY = std::round((Top - Screen.m_TopLeft.y) * YScale);
	const int ClipRight = std::round((Right - Screen.m_TopLeft.x) * XScale);
	const int ClipBottom = std::round((Bottom - Screen.m_TopLeft.y) * YScale);
	Graphics()->ClipEnable(ClipX, ClipY, ClipRight - ClipX, ClipBottom - ClipY);
	const bool Drawn = Graphics()->BlitTexture(m_aMenuBackdropBlurTextures[0], true);
	Graphics()->ClipDisable();
	if(!Drawn)
		m_MenuBackdropReady = false;
}

#if defined(CONF_VIDEORECORDER)
bool CMenus::VideoProgress(CVideoExportStatus &Status, float &Progress, float &Elapsed)
{
	int FirstTick;
	int CurrentTick;
	int LastTick;
	if(!Client()->DemoPlayer_RenderInfo(&FirstTick, &CurrentTick, &LastTick) || IVideo::Current() == nullptr)
		return false;
	Status = IVideo::Current()->Status();
	const int TotalTicks = LastTick - FirstTick;
	const int CurrentTicks = std::clamp(CurrentTick - FirstTick, 0, std::max(TotalTicks, 0));
	Progress = TotalTicks > 0 ? CurrentTicks / static_cast<float>(TotalTicks) : 0.0f;
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(m_DemoRenderStartTime == std::chrono::nanoseconds::zero() || Status.m_SubmittedFrames < m_DemoRenderLastSubmittedFrames)
		m_DemoRenderStartTime = Now;
	m_DemoRenderLastSubmittedFrames = Status.m_SubmittedFrames;
	Elapsed = std::chrono::duration<float>(Now - m_DemoRenderStartTime).count();
	return true;
}
#endif
