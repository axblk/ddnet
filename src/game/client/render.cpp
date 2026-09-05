/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "render.h"

#include "animstate.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/client_data.h>
#include <generated/client_data7.h>
#include <generated/data_types.h>
#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/mapitems.h>

#include <cmath>

CSkinDescriptor::CSkinDescriptor()
{
	Reset();
}

void CSkinDescriptor::Reset()
{
	m_Flags = 0;
	m_aSkinName[0] = '\0';
	for(auto &Sixup : m_aSixup)
	{
		Sixup.Reset();
	}
}

bool CSkinDescriptor::IsValid() const
{
	return (m_Flags & (FLAG_SIX | FLAG_SEVEN)) != 0;
}

bool CSkinDescriptor::operator==(const CSkinDescriptor &Other) const
{
	if(m_Flags != Other.m_Flags)
	{
		return false;
	}

	if(m_Flags & FLAG_SIX)
	{
		if(str_comp(m_aSkinName, Other.m_aSkinName) != 0)
		{
			return false;
		}
	}

	if(m_Flags & FLAG_SEVEN)
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
		{
			if(m_aSixup[Dummy] != Other.m_aSixup[Dummy])
			{
				return false;
			}
		}
	}

	return true;
}

void CSkinDescriptor::CSixup::Reset()
{
	for(auto &aSkinPartName : m_aaSkinPartNames)
	{
		aSkinPartName[0] = '\0';
	}
	m_BotDecoration = false;
	m_XmasHat = false;
}

bool CSkinDescriptor::CSixup::operator==(const CSixup &Other) const
{
	for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
	{
		if(str_comp(m_aaSkinPartNames[Part], Other.m_aaSkinPartNames[Part]) != 0)
		{
			return false;
		}
	}
	return m_BotDecoration == Other.m_BotDecoration &&
	       m_XmasHat == Other.m_XmasHat;
}

void CRenderTools::Init(IGraphics *pGraphics, ITextRender *pTextRender)
{
	m_pGraphics = pGraphics;
	m_pTextRender = pTextRender;
	m_TeeQuadContainerIndex = Graphics()->CreateQuadContainer(false);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f);

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f * 0.4f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f * 0.4f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f * 0.4f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f * 0.4f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, 64.f * 0.4f);

	// Feet
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, -32.f, -16.f, 64.f, 32.f);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, -32.f, -16.f, 64.f, 32.f);

	// Mirrored Feet
	Graphics()->QuadsSetSubsetFree(1, 0, 0, 0, 0, 1, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, -32.f, -16.f, 64.f, 32.f);
	Graphics()->QuadsSetSubsetFree(1, 0, 0, 0, 0, 1, 1, 1);
	QuadContainerAddSprite(m_TeeQuadContainerIndex, -32.f, -16.f, 64.f, 32.f);

	Graphics()->QuadContainerUpload(m_TeeQuadContainerIndex);
}

void CRenderTools::RenderCursor(vec2 Center, float Size) const
{
	Graphics()->WrapClamp();
	Graphics()->TextureSet(g_pData->m_aImages[IMAGE_CURSOR].m_Id);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	IGraphics::CQuadItem QuadItem(Center.x, Center.y, Size, Size);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
}

void CRenderTools::RenderIcon(int ImageId, int SpriteId, const CUIRect *pRect, const ColorRGBA *pColor) const
{
	Graphics()->TextureSet(g_pData->m_aImages[ImageId].m_Id);
	Graphics()->QuadsBegin();
	SelectSprite(SpriteId);
	if(pColor)
		Graphics()->SetColor(pColor->r * pColor->a, pColor->g * pColor->a, pColor->b * pColor->a, pColor->a);
	IGraphics::CQuadItem QuadItem(pRect->x, pRect->y, pRect->w, pRect->h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
}

void CRenderTools::GetRenderTeeAnimScaleAndBaseSize(const CTeeRenderInfo *pInfo, float &AnimScale, float &BaseSize)
{
	AnimScale = pInfo->m_Size * 1.0f / 64.0f;
	BaseSize = pInfo->m_Size;
}

void CRenderTools::GetRenderTeeBodyScale(float BaseSize, float &BodyScale)
{
	BodyScale = g_Config.m_ClFatSkins ? BaseSize * 1.3f : BaseSize;
	BodyScale /= 64.0f;
}

void CRenderTools::GetRenderTeeFeetScale(float BaseSize, float &FeetScaleWidth, float &FeetScaleHeight)
{
	FeetScaleWidth = BaseSize / 64.0f;
	FeetScaleHeight = (BaseSize / 2) / 32.0f;
}

void CRenderTools::GetRenderTeeBodySize(const CAnimState *pAnim, const CTeeRenderInfo *pInfo, vec2 &BodyOffset, float &Width, float &Height)
{
	float AnimScale, BaseSize;
	GetRenderTeeAnimScaleAndBaseSize(pInfo, AnimScale, BaseSize);

	float BodyScale;
	GetRenderTeeBodyScale(BaseSize, BodyScale);

	Width = pInfo->m_SkinMetrics.m_Body.WidthNormalized() * 64.0f * BodyScale;
	Height = pInfo->m_SkinMetrics.m_Body.HeightNormalized() * 64.0f * BodyScale;
	BodyOffset.x = pInfo->m_SkinMetrics.m_Body.OffsetXNormalized() * 64.0f * BodyScale;
	BodyOffset.y = pInfo->m_SkinMetrics.m_Body.OffsetYNormalized() * 64.0f * BodyScale;
}

void CRenderTools::GetRenderTeeFeetSize(const CAnimState *pAnim, const CTeeRenderInfo *pInfo, vec2 &FeetOffset, float &Width, float &Height)
{
	float AnimScale, BaseSize;
	GetRenderTeeAnimScaleAndBaseSize(pInfo, AnimScale, BaseSize);

	float FeetScaleWidth, FeetScaleHeight;
	GetRenderTeeFeetScale(BaseSize, FeetScaleWidth, FeetScaleHeight);

	Width = pInfo->m_SkinMetrics.m_Feet.WidthNormalized() * 64.0f * FeetScaleWidth;
	Height = pInfo->m_SkinMetrics.m_Feet.HeightNormalized() * 32.0f * FeetScaleHeight;
	FeetOffset.x = pInfo->m_SkinMetrics.m_Feet.OffsetXNormalized() * 64.0f * FeetScaleWidth;
	FeetOffset.y = pInfo->m_SkinMetrics.m_Feet.OffsetYNormalized() * 32.0f * FeetScaleHeight;
}

void CRenderTools::GetRenderTeeOffsetToRenderedTee(const CAnimState *pAnim, const CTeeRenderInfo *pInfo, vec2 &TeeOffsetToMid)
{
	if(pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_BODY).IsValid())
	{
		TeeOffsetToMid = vec2(0.0f, pInfo->m_Size * 0.12f);
		return;
	}

	float AnimScale, BaseSize;
	GetRenderTeeAnimScaleAndBaseSize(pInfo, AnimScale, BaseSize);
	vec2 BodyPos = vec2(pAnim->GetBody()->m_X, pAnim->GetBody()->m_Y) * AnimScale;

	float AssumedScale = BaseSize / 64.0f;

	// just use the lowest feet
	vec2 FeetPos;
	const CAnimKeyframe *pFoot = pAnim->GetFrontFoot();
	FeetPos = vec2(pFoot->m_X * AnimScale, pFoot->m_Y * AnimScale);
	pFoot = pAnim->GetBackFoot();
	FeetPos = vec2(FeetPos.x, std::max(FeetPos.y, pFoot->m_Y * AnimScale));

	vec2 BodyOffset;
	float BodyWidth, BodyHeight;
	GetRenderTeeBodySize(pAnim, pInfo, BodyOffset, BodyWidth, BodyHeight);

	// -32 is the assumed min relative position for the quad
	float MinY = -32.0f * AssumedScale;
	// the body pos shifts the body away from center
	MinY += BodyPos.y;
	// the actual body is smaller though, because it doesn't use the full skin image in most cases
	MinY += BodyOffset.y;

	vec2 FeetOffset;
	float FeetWidth, FeetHeight;
	GetRenderTeeFeetSize(pAnim, pInfo, FeetOffset, FeetWidth, FeetHeight);

	// MaxY builds up from the MinY
	float MaxY = MinY + BodyHeight;
	// if the body is smaller than the total feet offset, use feet
	// since feet are smaller in height, respect the assumed relative position
	MaxY = std::max(MaxY, (-16.0f * AssumedScale + FeetPos.y) + FeetOffset.y + FeetHeight);

	// now we got the full rendered size
	float FullHeight = (MaxY - MinY);

	// next step is to calculate the offset that was created compared to the assumed relative position
	float MidOfRendered = MinY + FullHeight / 2.0f;

	// TODO: x coordinate is ignored for now, bcs it's not really used yet anyway
	TeeOffsetToMid.x = 0;
	// negative value, because the calculation that uses this offset should work with addition.
	TeeOffsetToMid.y = -MidOfRendered;
}

void CRenderTools::RenderTee(const CAnimState *pAnim, const CTeeRenderInfo *pInfo, int Emote, vec2 Dir, vec2 Pos, float Alpha) const
{
	if(pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_BODY).IsValid())
		RenderTee7(pAnim, pInfo, Emote, Dir, Pos, Alpha);
	else
		RenderTee6(pAnim, pInfo, Emote, Dir, Pos, Alpha);

	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
	Graphics()->QuadsSetRotation(0);
}

void CRenderTools::RenderTee7(const CAnimState *pAnim, const CTeeRenderInfo *pInfo, int Emote, vec2 Dir, vec2 Pos, float Alpha) const
{
	vec2 Direction = Dir;
	vec2 Position = Pos;
	const bool IsBot = pInfo->m_aSixup[g_Config.m_ClDummy].m_BotTexture.IsValid();

	// first pass we draw the outline
	// second pass we draw the filling
	for(int Pass = 0; Pass < 2; Pass++)
	{
		bool OutLine = Pass == 0;

		for(int Filling = 0; Filling < 2; Filling++)
		{
			float AnimScale = pInfo->m_Size * 1.0f / 64.0f;
			float BaseSize = pInfo->m_Size;
			if(Filling == 1)
			{
				vec2 BodyPos = Position + vec2(pAnim->GetBody()->m_X, pAnim->GetBody()->m_Y) * AnimScale;
				IGraphics::CQuadItem BodyItem(BodyPos.x, BodyPos.y, BaseSize, BaseSize);
				IGraphics::CQuadItem Item;

				if(IsBot && !OutLine)
				{
					IGraphics::CQuadItem BotItem(BodyPos.x + (2.f / 3.f) * AnimScale, BodyPos.y + (-16 + 2.f / 3.f) * AnimScale, BaseSize, BaseSize); // x+0.66, y+0.66 to correct some rendering bug

					// draw bot visuals (background)
					Graphics()->TextureSet(pInfo->m_aSixup[g_Config.m_ClDummy].m_BotTexture);
					Graphics()->QuadsBegin();
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
					SelectSprite7(client_data7::SPRITE_TEE_BOT_BACKGROUND);
					Item = BotItem;
					Graphics()->QuadsDraw(&Item, 1);
					Graphics()->QuadsEnd();

					// draw bot visuals (foreground)
					Graphics()->TextureSet(pInfo->m_aSixup[g_Config.m_ClDummy].m_BotTexture);
					Graphics()->QuadsBegin();
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
					SelectSprite7(client_data7::SPRITE_TEE_BOT_FOREGROUND);
					Item = BotItem;
					Graphics()->QuadsDraw(&Item, 1);
					Graphics()->SetColor(pInfo->m_aSixup[g_Config.m_ClDummy].m_BotColor.WithAlpha(Alpha));
					SelectSprite7(client_data7::SPRITE_TEE_BOT_GLOW);
					Item = BotItem;
					Graphics()->QuadsDraw(&Item, 1);
					Graphics()->QuadsEnd();
				}

				// draw decoration
				const IGraphics::CTextureHandle &DecorationTexture = pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_DECORATION);
				if(DecorationTexture.IsValid())
				{
					Graphics()->TextureSet(DecorationTexture);
					Graphics()->QuadsBegin();
					Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);
					Graphics()->SetColor(pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_DECORATION].WithAlpha(Alpha));
					SelectSprite7(OutLine ? client_data7::SPRITE_TEE_DECORATION_OUTLINE : client_data7::SPRITE_TEE_DECORATION);
					Item = BodyItem;
					Graphics()->QuadsDraw(&Item, 1);
					Graphics()->QuadsEnd();
				}

				// draw body (behind marking)
				const IGraphics::CTextureHandle &BodyTexture = pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_BODY);
				Graphics()->TextureSet(BodyTexture);
				Graphics()->QuadsBegin();
				Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);
				if(OutLine)
				{
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
					SelectSprite7(client_data7::SPRITE_TEE_BODY_OUTLINE);
				}
				else
				{
					Graphics()->SetColor(pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_BODY].WithAlpha(Alpha));
					SelectSprite7(client_data7::SPRITE_TEE_BODY);
				}
				Item = BodyItem;
				Graphics()->QuadsDraw(&Item, 1);
				Graphics()->QuadsEnd();

				// draw marking
				const IGraphics::CTextureHandle &MarkingTexture = pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_MARKING);
				if(MarkingTexture.IsValid() && !OutLine)
				{
					Graphics()->TextureSet(MarkingTexture);
					Graphics()->QuadsBegin();
					Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);
					ColorRGBA MarkingColor = pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_MARKING];
					Graphics()->SetColor(MarkingColor.r * MarkingColor.a, MarkingColor.g * MarkingColor.a, MarkingColor.b * MarkingColor.a, MarkingColor.a * Alpha);
					SelectSprite7(client_data7::SPRITE_TEE_MARKING);
					Item = BodyItem;
					Graphics()->QuadsDraw(&Item, 1);
					Graphics()->QuadsEnd();
				}

				// draw body (in front of marking)
				if(!OutLine)
				{
					Graphics()->TextureSet(BodyTexture);
					Graphics()->QuadsBegin();
					Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
					for(int t = 0; t < 2; t++)
					{
						SelectSprite7(t == 0 ? client_data7::SPRITE_TEE_BODY_SHADOW : client_data7::SPRITE_TEE_BODY_UPPER_OUTLINE);
						Item = BodyItem;
						Graphics()->QuadsDraw(&Item, 1);
					}
					Graphics()->QuadsEnd();
				}

				// draw eyes
				Graphics()->TextureSet(pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_EYES));
				Graphics()->QuadsBegin();
				Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);
				if(IsBot)
				{
					Graphics()->SetColor(pInfo->m_aSixup[g_Config.m_ClDummy].m_BotColor.WithAlpha(Alpha));
					Emote = EMOTE_SURPRISE;
				}
				else
				{
					Graphics()->SetColor(pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_EYES].WithAlpha(Alpha));
				}
				if(Pass == 1)
				{
					switch(Emote)
					{
					case EMOTE_PAIN:
						SelectSprite7(client_data7::SPRITE_TEE_EYES_PAIN);
						break;
					case EMOTE_HAPPY:
						SelectSprite7(client_data7::SPRITE_TEE_EYES_HAPPY);
						break;
					case EMOTE_SURPRISE:
						SelectSprite7(client_data7::SPRITE_TEE_EYES_SURPRISE);
						break;
					case EMOTE_ANGRY:
						SelectSprite7(client_data7::SPRITE_TEE_EYES_ANGRY);
						break;
					default:
						SelectSprite7(client_data7::SPRITE_TEE_EYES_NORMAL);
						break;
					}

					float EyeScale = BaseSize * 0.60f;
					float h = Emote == EMOTE_BLINK ? BaseSize * 0.15f / 2.0f : EyeScale / 2.0f;
					vec2 Offset = vec2(Direction.x * 0.125f, -0.05f + Direction.y * 0.10f) * BaseSize;
					IGraphics::CQuadItem QuadItem(BodyPos.x + Offset.x, BodyPos.y + Offset.y, EyeScale, h);
					Graphics()->QuadsDraw(&QuadItem, 1);
				}
				Graphics()->QuadsEnd();

				// draw xmas hat
				if(!OutLine && pInfo->m_aSixup[g_Config.m_ClDummy].m_HatTexture.IsValid())
				{
					Graphics()->TextureSet(pInfo->m_aSixup[g_Config.m_ClDummy].m_HatTexture);
					Graphics()->QuadsBegin();
					Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
					int Flag = Direction.x < 0.0f ? IGraphics::SPRITE_FLAG_FLIP_X : 0;
					switch(pInfo->m_aSixup[g_Config.m_ClDummy].m_HatSpriteIndex)
					{
					case 0:
						SelectSprite7(client_data7::SPRITE_TEE_HATS_TOP1, Flag);
						break;
					case 1:
						SelectSprite7(client_data7::SPRITE_TEE_HATS_TOP2, Flag);
						break;
					case 2:
						SelectSprite7(client_data7::SPRITE_TEE_HATS_SIDE1, Flag);
						break;
					case 3:
						SelectSprite7(client_data7::SPRITE_TEE_HATS_SIDE2, Flag);
					}
					Item = BodyItem;
					Graphics()->QuadsDraw(&Item, 1);
					Graphics()->QuadsEnd();
				}
			}

			// draw feet
			Graphics()->TextureSet(pInfo->m_aSixup[g_Config.m_ClDummy].PartTexture(protocol7::SKINPART_FEET));
			Graphics()->QuadsBegin();
			const CAnimKeyframe *pFoot = Filling ? pAnim->GetFrontFoot() : pAnim->GetBackFoot();

			float w = BaseSize / 2.1f;
			float h = w;

			Graphics()->QuadsSetRotation(pFoot->m_Angle * pi * 2);

			if(OutLine)
			{
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
				SelectSprite7(client_data7::SPRITE_TEE_FOOT_OUTLINE);
			}
			else
			{
				bool Indicate = !pInfo->m_GotAirJump && g_Config.m_ClAirjumpindicator;
				float ColorScale = 1.0f;
				if(Indicate)
					ColorScale = 0.5f;
				Graphics()->SetColor(
					pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_FEET].r * ColorScale,
					pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_FEET].g * ColorScale,
					pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_FEET].b * ColorScale,
					pInfo->m_aSixup[g_Config.m_ClDummy].m_aColors[protocol7::SKINPART_FEET].a * Alpha);
				SelectSprite7(client_data7::SPRITE_TEE_FOOT);
			}

			IGraphics::CQuadItem QuadItem(Position.x + pFoot->m_X * AnimScale, Position.y + pFoot->m_Y * AnimScale, w, h);
			Graphics()->QuadsDraw(&QuadItem, 1);
			Graphics()->QuadsEnd();
		}
	}
}

void CRenderTools::RenderTee6(const CAnimState *pAnim, const CTeeRenderInfo *pInfo, int Emote, vec2 Dir, vec2 Pos, float Alpha) const
{
	vec2 Direction = Dir;
	vec2 Position = Pos;

	const CSkin::CSkinTextures *pSkinTextures = pInfo->m_CustomColoredSkin ? &pInfo->m_ColorableRenderSkin : &pInfo->m_OriginalRenderSkin;

	// first pass we draw the outline
	// second pass we draw the filling
	for(int Pass = 0; Pass < 2; Pass++)
	{
		int OutLine = Pass == 0 ? 1 : 0;

		for(int Filling = 0; Filling < 2; Filling++)
		{
			float AnimScale, BaseSize;
			GetRenderTeeAnimScaleAndBaseSize(pInfo, AnimScale, BaseSize);
			if(Filling == 1)
			{
				Graphics()->QuadsSetRotation(pAnim->GetBody()->m_Angle * pi * 2);

				// draw body
				Graphics()->SetColor(pInfo->m_ColorBody.r, pInfo->m_ColorBody.g, pInfo->m_ColorBody.b, Alpha);
				vec2 BodyPos = Position + vec2(pAnim->GetBody()->m_X, pAnim->GetBody()->m_Y) * AnimScale;
				float BodyScale;
				GetRenderTeeBodyScale(BaseSize, BodyScale);
				Graphics()->TextureSet(OutLine == 1 ? pSkinTextures->m_BodyOutline : pSkinTextures->m_Body);
				Graphics()->RenderQuadContainerAsSprite(m_TeeQuadContainerIndex, OutLine, BodyPos.x, BodyPos.y, BodyScale, BodyScale);

				// draw eyes
				if(Pass == 1)
				{
					int QuadOffset = 2;
					int EyeQuadOffset = 0;
					int TeeEye = 0;

					switch(Emote)
					{
					case EMOTE_PAIN:
						EyeQuadOffset = 0;
						TeeEye = SPRITE_TEE_EYE_PAIN - SPRITE_TEE_EYE_NORMAL;
						break;
					case EMOTE_HAPPY:
						EyeQuadOffset = 1;
						TeeEye = SPRITE_TEE_EYE_HAPPY - SPRITE_TEE_EYE_NORMAL;
						break;
					case EMOTE_SURPRISE:
						EyeQuadOffset = 2;
						TeeEye = SPRITE_TEE_EYE_SURPRISE - SPRITE_TEE_EYE_NORMAL;
						break;
					case EMOTE_ANGRY:
						EyeQuadOffset = 3;
						TeeEye = SPRITE_TEE_EYE_ANGRY - SPRITE_TEE_EYE_NORMAL;
						break;
					default:
						EyeQuadOffset = 4;
						break;
					}

					float EyeScale = BaseSize * 0.40f;
					float h = Emote == EMOTE_BLINK ? BaseSize * 0.15f : EyeScale;
					float EyeSeparation = (0.075f - 0.010f * absolute(Direction.x)) * BaseSize;
					vec2 Offset = vec2(Direction.x * 0.125f, -0.05f + Direction.y * 0.10f) * BaseSize;

					Graphics()->TextureSet(pSkinTextures->m_aEyes[TeeEye]);
					Graphics()->RenderQuadContainerAsSprite(m_TeeQuadContainerIndex, QuadOffset + EyeQuadOffset, BodyPos.x - EyeSeparation + Offset.x, BodyPos.y + Offset.y, EyeScale / (64.f * 0.4f), h / (64.f * 0.4f));
					Graphics()->RenderQuadContainerAsSprite(m_TeeQuadContainerIndex, QuadOffset + EyeQuadOffset, BodyPos.x + EyeSeparation + Offset.x, BodyPos.y + Offset.y, -EyeScale / (64.f * 0.4f), h / (64.f * 0.4f));
				}
			}

			// draw feet
			const CAnimKeyframe *pFoot = Filling ? pAnim->GetFrontFoot() : pAnim->GetBackFoot();

			float w = BaseSize;
			float h = BaseSize / 2;

			int QuadOffset = 7;
			if(Dir.x < 0 && pInfo->m_FeetFlipped)
			{
				QuadOffset += 2;
			}

			Graphics()->QuadsSetRotation(pFoot->m_Angle * pi * 2);

			bool Indicate = !pInfo->m_GotAirJump && g_Config.m_ClAirjumpindicator;
			float ColorScale = 1.0f;

			if(!OutLine)
			{
				++QuadOffset;
				if(Indicate)
					ColorScale = 0.5f;
			}

			Graphics()->SetColor(pInfo->m_ColorFeet.r * ColorScale, pInfo->m_ColorFeet.g * ColorScale, pInfo->m_ColorFeet.b * ColorScale, Alpha);

			Graphics()->TextureSet(OutLine == 1 ? pSkinTextures->m_FeetOutline : pSkinTextures->m_Feet);
			Graphics()->RenderQuadContainerAsSprite(m_TeeQuadContainerIndex, QuadOffset, Position.x + pFoot->m_X * AnimScale, Position.y + pFoot->m_Y * AnimScale, w / 64.f, h / 32.f);
		}
	}
}

// Shapes and sprites: quads built on the client side from what the game
// asks for. Nothing here touches a backend; it is drawing convenience,
// and it lives with the rest of the game's rendering helpers.

void CRenderTools::DrawRectExt(float x, float y, float w, float h, float r, int Corners) const
{
	const int NumSegments = 8;
	const float SegmentsAngle = pi / 2 / NumSegments;
	IGraphics::CFreeformItem aFreeform[NumSegments * 4];
	size_t NumItems = 0;

	for(int i = 0; i < NumSegments; i += 2)
	{
		float a1 = i * SegmentsAngle;
		float a2 = (i + 1) * SegmentsAngle;
		float a3 = (i + 2) * SegmentsAngle;
		float Ca1 = std::cos(a1);
		float Ca2 = std::cos(a2);
		float Ca3 = std::cos(a3);
		float Sa1 = std::sin(a1);
		float Sa2 = std::sin(a2);
		float Sa3 = std::sin(a3);

		if(Corners & IGraphics::CORNER_TL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + r,
				x + (1 - Ca1) * r, y + (1 - Sa1) * r,
				x + (1 - Ca3) * r, y + (1 - Sa3) * r,
				x + (1 - Ca2) * r, y + (1 - Sa2) * r);

		if(Corners & IGraphics::CORNER_TR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + r,
				x + w - r + Ca1 * r, y + (1 - Sa1) * r,
				x + w - r + Ca3 * r, y + (1 - Sa3) * r,
				x + w - r + Ca2 * r, y + (1 - Sa2) * r);

		if(Corners & IGraphics::CORNER_BL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + h - r,
				x + (1 - Ca1) * r, y + h - r + Sa1 * r,
				x + (1 - Ca3) * r, y + h - r + Sa3 * r,
				x + (1 - Ca2) * r, y + h - r + Sa2 * r);

		if(Corners & IGraphics::CORNER_BR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + h - r,
				x + w - r + Ca1 * r, y + h - r + Sa1 * r,
				x + w - r + Ca3 * r, y + h - r + Sa3 * r,
				x + w - r + Ca2 * r, y + h - r + Sa2 * r);
	}
	Graphics()->QuadsDrawFreeform(aFreeform, NumItems);

	IGraphics::CQuadItem aQuads[9];
	NumItems = 0;
	aQuads[NumItems++] = IGraphics::CQuadItem(x + r, y + r, w - r * 2, h - r * 2); // center
	aQuads[NumItems++] = IGraphics::CQuadItem(x + r, y, w - r * 2, r); // top
	aQuads[NumItems++] = IGraphics::CQuadItem(x + r, y + h - r, w - r * 2, r); // bottom
	aQuads[NumItems++] = IGraphics::CQuadItem(x, y + r, r, h - r * 2); // left
	aQuads[NumItems++] = IGraphics::CQuadItem(x + w - r, y + r, r, h - r * 2); // right

	if(!(Corners & IGraphics::CORNER_TL))
		aQuads[NumItems++] = IGraphics::CQuadItem(x, y, r, r);
	if(!(Corners & IGraphics::CORNER_TR))
		aQuads[NumItems++] = IGraphics::CQuadItem(x + w, y, -r, r);
	if(!(Corners & IGraphics::CORNER_BL))
		aQuads[NumItems++] = IGraphics::CQuadItem(x, y + h, r, -r);
	if(!(Corners & IGraphics::CORNER_BR))
		aQuads[NumItems++] = IGraphics::CQuadItem(x + w, y + h, -r, -r);

	Graphics()->QuadsDrawTL(aQuads, NumItems);
}

void CRenderTools::DrawRectExt4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, float r, int Corners) const
{
	if(Corners == 0 || r == 0.0f)
	{
		Graphics()->SetColor4(ColorTopLeft, ColorTopRight, ColorBottomLeft, ColorBottomRight);
		IGraphics::CQuadItem ItemQ = IGraphics::CQuadItem(x, y, w, h);
		Graphics()->QuadsDrawTL(&ItemQ, 1);
		return;
	}

	const int NumSegments = 8;
	const float SegmentsAngle = pi / 2 / NumSegments;
	for(int i = 0; i < NumSegments; i += 2)
	{
		float a1 = i * SegmentsAngle;
		float a2 = (i + 1) * SegmentsAngle;
		float a3 = (i + 2) * SegmentsAngle;
		float Ca1 = std::cos(a1);
		float Ca2 = std::cos(a2);
		float Ca3 = std::cos(a3);
		float Sa1 = std::sin(a1);
		float Sa2 = std::sin(a2);
		float Sa3 = std::sin(a3);

		if(Corners & IGraphics::CORNER_TL)
		{
			Graphics()->SetColor(ColorTopLeft);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + r, y + r,
				x + (1 - Ca1) * r, y + (1 - Sa1) * r,
				x + (1 - Ca3) * r, y + (1 - Sa3) * r,
				x + (1 - Ca2) * r, y + (1 - Sa2) * r);
			Graphics()->QuadsDrawFreeform(&ItemF, 1);
		}

		if(Corners & IGraphics::CORNER_TR)
		{
			Graphics()->SetColor(ColorTopRight);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + w - r, y + r,
				x + w - r + Ca1 * r, y + (1 - Sa1) * r,
				x + w - r + Ca3 * r, y + (1 - Sa3) * r,
				x + w - r + Ca2 * r, y + (1 - Sa2) * r);
			Graphics()->QuadsDrawFreeform(&ItemF, 1);
		}

		if(Corners & IGraphics::CORNER_BL)
		{
			Graphics()->SetColor(ColorBottomLeft);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + r, y + h - r,
				x + (1 - Ca1) * r, y + h - r + Sa1 * r,
				x + (1 - Ca3) * r, y + h - r + Sa3 * r,
				x + (1 - Ca2) * r, y + h - r + Sa2 * r);
			Graphics()->QuadsDrawFreeform(&ItemF, 1);
		}

		if(Corners & IGraphics::CORNER_BR)
		{
			Graphics()->SetColor(ColorBottomRight);
			IGraphics::CFreeformItem ItemF = IGraphics::CFreeformItem(
				x + w - r, y + h - r,
				x + w - r + Ca1 * r, y + h - r + Sa1 * r,
				x + w - r + Ca3 * r, y + h - r + Sa3 * r,
				x + w - r + Ca2 * r, y + h - r + Sa2 * r);
			Graphics()->QuadsDrawFreeform(&ItemF, 1);
		}
	}

	Graphics()->SetColor4(ColorTopLeft, ColorTopRight, ColorBottomLeft, ColorBottomRight);
	IGraphics::CQuadItem ItemQ = IGraphics::CQuadItem(x + r, y + r, w - r * 2, h - r * 2); // center
	Graphics()->QuadsDrawTL(&ItemQ, 1);

	Graphics()->SetColor4(ColorTopLeft, ColorTopRight, ColorTopLeft, ColorTopRight);
	ItemQ = IGraphics::CQuadItem(x + r, y, w - r * 2, r); // top
	Graphics()->QuadsDrawTL(&ItemQ, 1);

	Graphics()->SetColor4(ColorBottomLeft, ColorBottomRight, ColorBottomLeft, ColorBottomRight);
	ItemQ = IGraphics::CQuadItem(x + r, y + h - r, w - r * 2, r); // bottom
	Graphics()->QuadsDrawTL(&ItemQ, 1);

	Graphics()->SetColor4(ColorTopLeft, ColorTopLeft, ColorBottomLeft, ColorBottomLeft);
	ItemQ = IGraphics::CQuadItem(x, y + r, r, h - r * 2); // left
	Graphics()->QuadsDrawTL(&ItemQ, 1);

	Graphics()->SetColor4(ColorTopRight, ColorTopRight, ColorBottomRight, ColorBottomRight);
	ItemQ = IGraphics::CQuadItem(x + w - r, y + r, r, h - r * 2); // right
	Graphics()->QuadsDrawTL(&ItemQ, 1);

	if(!(Corners & IGraphics::CORNER_TL))
	{
		Graphics()->SetColor(ColorTopLeft);
		ItemQ = IGraphics::CQuadItem(x, y, r, r);
		Graphics()->QuadsDrawTL(&ItemQ, 1);
	}

	if(!(Corners & IGraphics::CORNER_TR))
	{
		Graphics()->SetColor(ColorTopRight);
		ItemQ = IGraphics::CQuadItem(x + w, y, -r, r);
		Graphics()->QuadsDrawTL(&ItemQ, 1);
	}

	if(!(Corners & IGraphics::CORNER_BL))
	{
		Graphics()->SetColor(ColorBottomLeft);
		ItemQ = IGraphics::CQuadItem(x, y + h, r, -r);
		Graphics()->QuadsDrawTL(&ItemQ, 1);
	}

	if(!(Corners & IGraphics::CORNER_BR))
	{
		Graphics()->SetColor(ColorBottomRight);
		ItemQ = IGraphics::CQuadItem(x + w, y + h, -r, -r);
		Graphics()->QuadsDrawTL(&ItemQ, 1);
	}
}

int CRenderTools::CreateRectQuadContainer(float x, float y, float w, float h, float r, int Corners) const
{
	int ContainerIndex = Graphics()->CreateQuadContainer(false);

	if(Corners == 0 || r == 0.0f)
	{
		IGraphics::CQuadItem ItemQ = IGraphics::CQuadItem(x, y, w, h);
		Graphics()->QuadContainerAddQuads(ContainerIndex, &ItemQ, 1);
		Graphics()->QuadContainerUpload(ContainerIndex);
		Graphics()->QuadContainerChangeAutomaticUpload(ContainerIndex, true);
		return ContainerIndex;
	}

	const int NumSegments = 8;
	const float SegmentsAngle = pi / 2 / NumSegments;
	IGraphics::CFreeformItem aFreeform[NumSegments * 4];
	size_t NumItems = 0;

	for(int i = 0; i < NumSegments; i += 2)
	{
		float a1 = i * SegmentsAngle;
		float a2 = (i + 1) * SegmentsAngle;
		float a3 = (i + 2) * SegmentsAngle;
		float Ca1 = std::cos(a1);
		float Ca2 = std::cos(a2);
		float Ca3 = std::cos(a3);
		float Sa1 = std::sin(a1);
		float Sa2 = std::sin(a2);
		float Sa3 = std::sin(a3);

		if(Corners & IGraphics::CORNER_TL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + r,
				x + (1 - Ca1) * r, y + (1 - Sa1) * r,
				x + (1 - Ca3) * r, y + (1 - Sa3) * r,
				x + (1 - Ca2) * r, y + (1 - Sa2) * r);

		if(Corners & IGraphics::CORNER_TR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + r,
				x + w - r + Ca1 * r, y + (1 - Sa1) * r,
				x + w - r + Ca3 * r, y + (1 - Sa3) * r,
				x + w - r + Ca2 * r, y + (1 - Sa2) * r);

		if(Corners & IGraphics::CORNER_BL)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + r, y + h - r,
				x + (1 - Ca1) * r, y + h - r + Sa1 * r,
				x + (1 - Ca3) * r, y + h - r + Sa3 * r,
				x + (1 - Ca2) * r, y + h - r + Sa2 * r);

		if(Corners & IGraphics::CORNER_BR)
			aFreeform[NumItems++] = IGraphics::CFreeformItem(
				x + w - r, y + h - r,
				x + w - r + Ca1 * r, y + h - r + Sa1 * r,
				x + w - r + Ca3 * r, y + h - r + Sa3 * r,
				x + w - r + Ca2 * r, y + h - r + Sa2 * r);
	}

	if(NumItems > 0)
		Graphics()->QuadContainerAddQuads(ContainerIndex, aFreeform, NumItems);

	IGraphics::CQuadItem aQuads[9];
	NumItems = 0;
	aQuads[NumItems++] = IGraphics::CQuadItem(x + r, y + r, w - r * 2, h - r * 2); // center
	aQuads[NumItems++] = IGraphics::CQuadItem(x + r, y, w - r * 2, r); // top
	aQuads[NumItems++] = IGraphics::CQuadItem(x + r, y + h - r, w - r * 2, r); // bottom
	aQuads[NumItems++] = IGraphics::CQuadItem(x, y + r, r, h - r * 2); // left
	aQuads[NumItems++] = IGraphics::CQuadItem(x + w - r, y + r, r, h - r * 2); // right

	if(!(Corners & IGraphics::CORNER_TL))
		aQuads[NumItems++] = IGraphics::CQuadItem(x, y, r, r);
	if(!(Corners & IGraphics::CORNER_TR))
		aQuads[NumItems++] = IGraphics::CQuadItem(x + w, y, -r, r);
	if(!(Corners & IGraphics::CORNER_BL))
		aQuads[NumItems++] = IGraphics::CQuadItem(x, y + h, r, -r);
	if(!(Corners & IGraphics::CORNER_BR))
		aQuads[NumItems++] = IGraphics::CQuadItem(x + w, y + h, -r, -r);

	if(NumItems > 0)
		Graphics()->QuadContainerAddQuads(ContainerIndex, aQuads, NumItems);

	Graphics()->QuadContainerUpload(ContainerIndex);
	Graphics()->QuadContainerChangeAutomaticUpload(ContainerIndex, true);

	return ContainerIndex;
}

void CRenderTools::DrawRect(float x, float y, float w, float h, ColorRGBA Color, int Corners, float Rounding) const
{
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(Color);
	DrawRectExt(x, y, w, h, Rounding, Corners);
	Graphics()->QuadsEnd();
}

void CRenderTools::DrawRect4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, int Corners, float Rounding) const
{
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	DrawRectExt4(x, y, w, h, ColorTopLeft, ColorTopRight, ColorBottomLeft, ColorBottomRight, Rounding, Corners);
	Graphics()->QuadsEnd();
}

void CRenderTools::DrawCircle(float CenterX, float CenterY, float Radius, int Segments) const
{
	IGraphics::CFreeformItem aItems[32];
	size_t NumItems = 0;
	const float SegmentsAngle = 2 * pi / Segments;
	for(int i = 0; i < Segments; i += 2)
	{
		const float a1 = i * SegmentsAngle;
		const float a2 = (i + 1) * SegmentsAngle;
		const float a3 = (i + 2) * SegmentsAngle;
		aItems[NumItems++] = IGraphics::CFreeformItem(
			CenterX, CenterY,
			CenterX + std::cos(a1) * Radius, CenterY + std::sin(a1) * Radius,
			CenterX + std::cos(a3) * Radius, CenterY + std::sin(a3) * Radius,
			CenterX + std::cos(a2) * Radius, CenterY + std::sin(a2) * Radius);
		if(NumItems == std::size(aItems))
		{
			Graphics()->QuadsDrawFreeform(aItems, std::size(aItems));
			NumItems = 0;
		}
	}
	if(NumItems)
		Graphics()->QuadsDrawFreeform(aItems, NumItems);
}

void CRenderTools::SelectSprite(const CDataSprite *pSprite, int Flags) const
{
	int x = pSprite->m_X;
	int y = pSprite->m_Y;
	int w = pSprite->m_W;
	int h = pSprite->m_H;
	int cx = pSprite->m_pSet->m_Gridx;
	int cy = pSprite->m_pSet->m_Gridy;

	GetSpriteScaleImpl(w, h, m_SpriteScale.x, m_SpriteScale.y);

	float x1 = x / (float)cx + 0.5f / (float)(cx * 32);
	float x2 = (x + w) / (float)cx - 0.5f / (float)(cx * 32);
	float y1 = y / (float)cy + 0.5f / (float)(cy * 32);
	float y2 = (y + h) / (float)cy - 0.5f / (float)(cy * 32);

	if(Flags & IGraphics::SPRITE_FLAG_FLIP_Y)
		std::swap(y1, y2);

	if(Flags & IGraphics::SPRITE_FLAG_FLIP_X)
		std::swap(x1, x2);

	Graphics()->QuadsSetSubset(x1, y1, x2, y2);
}

void CRenderTools::SelectSprite(int Id, int Flags) const
{
	dbg_assert(Id >= 0 && Id < g_pData->m_NumSprites, "Id invalid");
	SelectSprite(&g_pData->m_aSprites[Id], Flags);
}

void CRenderTools::SelectSprite7(int Id, int Flags) const
{
	dbg_assert(Id >= 0 && Id < client_data7::g_pData->m_NumSprites, "Id invalid");
	SelectSprite(&client_data7::g_pData->m_aSprites[Id], Flags);
}

void CRenderTools::GetSpriteScale(const CDataSprite *pSprite, float &ScaleX, float &ScaleY) const
{
	int w = pSprite->m_W;
	int h = pSprite->m_H;
	GetSpriteScaleImpl(w, h, ScaleX, ScaleY);
}

void CRenderTools::GetSpriteScale(int Id, float &ScaleX, float &ScaleY) const
{
	GetSpriteScale(&g_pData->m_aSprites[Id], ScaleX, ScaleY);
}

void CRenderTools::GetSpriteScaleImpl(int Width, int Height, float &ScaleX, float &ScaleY) const
{
	const float f = length(vec2(Width, Height));
	ScaleX = Width / f;
	ScaleY = Height / f;
}

void CRenderTools::DrawSprite(float x, float y, float Size) const
{
	IGraphics::CQuadItem QuadItem(x, y, Size * m_SpriteScale.x, Size * m_SpriteScale.y);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

void CRenderTools::DrawSprite(float x, float y, float ScaledWidth, float ScaledHeight) const
{
	IGraphics::CQuadItem QuadItem(x, y, ScaledWidth, ScaledHeight);
	Graphics()->QuadsDraw(&QuadItem, 1);
}

int CRenderTools::QuadContainerAddSprite(int QuadContainerIndex, float x, float y, float Size) const
{
	IGraphics::CQuadItem QuadItem(x, y, Size, Size);
	return Graphics()->QuadContainerAddQuads(QuadContainerIndex, &QuadItem, 1);
}

int CRenderTools::QuadContainerAddSprite(int QuadContainerIndex, float Size) const
{
	IGraphics::CQuadItem QuadItem(-Size / 2.f, -Size / 2.f, Size, Size);
	return Graphics()->QuadContainerAddQuads(QuadContainerIndex, &QuadItem, 1);
}

int CRenderTools::QuadContainerAddSprite(int QuadContainerIndex, float Width, float Height) const
{
	IGraphics::CQuadItem QuadItem(-Width / 2.f, -Height / 2.f, Width, Height);
	return Graphics()->QuadContainerAddQuads(QuadContainerIndex, &QuadItem, 1);
}

int CRenderTools::QuadContainerAddSprite(int QuadContainerIndex, float X, float Y, float Width, float Height) const
{
	IGraphics::CQuadItem QuadItem(X, Y, Width, Height);
	return Graphics()->QuadContainerAddQuads(QuadContainerIndex, &QuadItem, 1);
}
