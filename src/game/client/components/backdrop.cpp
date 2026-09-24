/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "backdrop.h"

#include <base/log.h>

#include <engine/shared/config.h>

#include <game/client/render.h>
#include <game/client/ui_rect.h>

#include <algorithm>

void CBackdrop::OnWindowResize()
{
	DestroyTextures();
}

void CBackdrop::OnShutdown()
{
	DestroyTextures();
}

void CBackdrop::DestroyTextures()
{
	m_SceneActive = false;
	m_OverlayActive = false;
	m_Ready = false;
	Graphics()->UnloadTexture(&m_SceneTexture);
	Graphics()->UnloadTexture(&m_OverlayTexture);
	for(IGraphics::CTextureHandle &Texture : m_aDownsampleTextures)
		Graphics()->UnloadTexture(&Texture);
	Graphics()->UnloadTexture(&m_aBlurTextures[0]);
	Graphics()->UnloadTexture(&m_aBlurTextures[1]);
	m_Width = 0;
	m_Height = 0;
}

bool CBackdrop::EnsureTextures()
{
	const int Width = Graphics()->ScreenWidth();
	const int Height = Graphics()->ScreenHeight();
	if(Width <= 0 || Height <= 0)
		return false;
	if(Width == m_Width && Height == m_Height)
		return TexturesValid();

	DestroyTextures();
	m_Width = Width;
	m_Height = Height;

	IGraphics::CTextureDesc Desc;
	Desc.m_Width = Width;
	Desc.m_Height = Height;
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Desc.m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED | IGraphics::TEXTURE_USAGE_COLOR_TARGET;
	m_SceneTexture = Graphics()->CreateTexture(Desc);
	m_OverlayTexture = Graphics()->CreateTexture(Desc);

	// The two blur passes run on an eighth of the screen, which is what decides
	// how coarse the result looks. Getting down there in one step would sample
	// four of the sixty-four pixels a target pixel covers, and which four
	// changes as the scene moves, which is what made the blur crawl. Halving
	// three times averages all of them.
	for(int i = 0; i < NUM_DOWNSAMPLES; ++i)
	{
		Desc.m_Width = std::max(1, (Width + (2 << i) - 1) / (2 << i));
		Desc.m_Height = std::max(1, (Height + (2 << i) - 1) / (2 << i));
		m_aDownsampleTextures[i] = Graphics()->CreateTexture(Desc);
	}
	m_aBlurTextures[0] = Graphics()->CreateTexture(Desc);
	m_aBlurTextures[1] = Graphics()->CreateTexture(Desc);
	if(TexturesValid())
	{
		log_debug("backdrop", "Created backdrop targets: scene=%dx%d blur=%dx%d", Width, Height, static_cast<int>(Desc.m_Width), static_cast<int>(Desc.m_Height));
		return true;
	}

	DestroyTextures();
	log_debug("backdrop", "Backdrop render targets unavailable, using direct rendering");
	return false;
}

bool CBackdrop::TexturesValid() const
{
	if(!m_SceneTexture.IsValid() || !m_OverlayTexture.IsValid())
		return false;
	for(const IGraphics::CTextureHandle &Texture : m_aDownsampleTextures)
	{
		if(!Texture.IsValid())
			return false;
	}
	return m_aBlurTextures[0].IsValid() && m_aBlurTextures[1].IsValid();
}

bool CBackdrop::RenderTexture(IGraphics::CTextureHandle Target, IGraphics::CTextureHandle Source, std::optional<IGraphics::EBlurDirection> BlurDirection)
{
	IGraphics::CRenderPassDesc Pass;
	Pass.m_ColorTarget = Target;
	if(!Graphics()->BeginRenderPass(Pass))
		return false;
	const bool Drawn = BlurDirection.has_value() ? Graphics()->BlurTexture(Source, BlurDirection.value()) : Graphics()->BlitTexture(Source);
	const bool Ended = Graphics()->EndRenderPass();
	return Drawn && Ended;
}

bool CBackdrop::BlurInto(IGraphics::CTextureHandle Source)
{
	IGraphics::CTextureHandle Current = Source;
	for(const IGraphics::CTextureHandle &Downsample : m_aDownsampleTextures)
	{
		if(!RenderTexture(Downsample, Current, std::nullopt))
			return false;
		Current = Downsample;
	}
	return RenderTexture(m_aBlurTextures[1], Current, IGraphics::EBlurDirection::HORIZONTAL) &&
	       RenderTexture(m_aBlurTextures[0], m_aBlurTextures[1], IGraphics::EBlurDirection::VERTICAL);
}

bool CBackdrop::Begin(ColorRGBA ClearColor, bool Needed)
{
	m_SceneActive = false;
	m_OverlayActive = false;
	m_Ready = false;
	if(!g_Config.m_ClMenuBackgroundBlur)
	{
		if(m_SceneTexture.IsValid())
			DestroyTextures();
		return false;
	}
	if(!Needed || !EnsureTextures())
		return false;

	IGraphics::CRenderPassDesc Pass;
	Pass.m_ColorTarget = m_SceneTexture;
	Pass.m_LoadOp = IGraphics::ERenderPassLoadOp::CLEAR;
	Pass.m_ClearColor = ClearColor.WithAlpha(0.0f);
	m_SceneActive = Graphics()->BeginRenderPass(Pass);
	return m_SceneActive;
}

void CBackdrop::Finish(bool Blur)
{
	if(!m_SceneActive)
		return;

	const bool SceneEnded = Graphics()->EndRenderPass();
	const bool Blurred = SceneEnded && Blur && BlurInto(m_SceneTexture);

	// Everything that is drawn over the scene from here on goes into a second
	// picture rather than straight to the screen, so that whatever is drawn
	// last can have a blurred copy of all of it. The console is what needs
	// that: it covers the menu just as it covers the game.
	IGraphics::CRenderPassDesc OverlayPass;
	OverlayPass.m_ColorTarget = m_OverlayTexture;
	m_OverlayActive = Graphics()->BeginRenderPass(OverlayPass) && Graphics()->BlitTexture(m_SceneTexture);
	if(!m_OverlayActive)
	{
		IGraphics::CRenderPassDesc PresentationPass;
		const bool Started = Graphics()->BeginRenderPass(PresentationPass);
		const bool Composited = Started && Graphics()->BlitTexture(m_SceneTexture);
		m_Ready = Blurred && Composited;
		m_SceneActive = false;
		return;
	}
	m_Ready = Blurred;
	m_SceneActive = false;
}

bool CBackdrop::Capture()
{
	if(!m_OverlayActive)
		return false;
	const bool Ended = Graphics()->EndRenderPass();
	const bool Blurred = Ended && BlurInto(m_OverlayTexture);
	m_OverlayActive = false;
	IGraphics::CRenderPassDesc PresentationPass;
	const bool Started = Graphics()->BeginRenderPass(PresentationPass);
	const bool Composited = Started && Graphics()->BlitTexture(m_OverlayTexture);
	m_Ready = Blurred && Composited;
	return m_Ready;
}

void CBackdrop::Present()
{
	if(!m_OverlayActive)
		return;
	const bool Ended = Graphics()->EndRenderPass();
	m_OverlayActive = false;
	IGraphics::CRenderPassDesc PresentationPass;
	if(Ended && Graphics()->BeginRenderPass(PresentationPass))
		Graphics()->BlitTexture(m_OverlayTexture);
	m_Ready = false;
}

void CBackdrop::RenderRegion(const CUIRect &Rect, int Corners, float Rounding)
{
	if(!m_Ready || Rect.w <= 0.0f || Rect.h <= 0.0f)
		return;

	// The very geometry the box is drawn with, sampling the blurred picture at
	// the spot on the screen each corner lands on. A rectangle cut out with a
	// scissor showed the blur past rounded corners and snapped to whole pixels
	// where the box did not.
	Graphics()->TextureSet(m_aBlurTextures[0]);
	Graphics()->BlendNone();
	Graphics()->WrapClamp();
	Graphics()->QuadsBegin();
	Graphics()->QuadsSetScreenTexCoords();
	RenderTools()->DrawRectExt(Rect.x, Rect.y, Rect.w, Rect.h, Rounding, Corners);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
	Graphics()->BlendNormal();
	Graphics()->TextureClear();
}

void CBackdrop::DrawSurface(const CUIRect &Rect, ColorRGBA Color, int Corners, float Rounding)
{
	RenderRegion(Rect, Corners, Rounding);
	Rect.Draw(Color, Corners, Rounding);
}
