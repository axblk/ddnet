#include "render_layer.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/localization.h>
#include <game/mapitems.h>

#include <array>
#include <cmath>
#include <cstdlib>

/************************
 * Render Buffer Helper *
 ************************/
class CTexCoords
{
public:
	std::array<uint8_t, 4> m_aTexX;
	std::array<uint8_t, 4> m_aTexY;
};

constexpr static CTexCoords CalculateTexCoords(unsigned int Flags)
{
	CTexCoords TexCoord;
	TexCoord.m_aTexX = {0, 1, 1, 0};
	TexCoord.m_aTexY = {0, 0, 1, 1};

	if(Flags & TILEFLAG_XFLIP)
		std::rotate(std::begin(TexCoord.m_aTexX), std::begin(TexCoord.m_aTexX) + 2, std::end(TexCoord.m_aTexX));

	if(Flags & TILEFLAG_YFLIP)
		std::rotate(std::begin(TexCoord.m_aTexY), std::begin(TexCoord.m_aTexY) + 2, std::end(TexCoord.m_aTexY));

	if(Flags & (TILEFLAG_ROTATE >> 1))
	{
		std::rotate(std::begin(TexCoord.m_aTexX), std::begin(TexCoord.m_aTexX) + 3, std::end(TexCoord.m_aTexX));
		std::rotate(std::begin(TexCoord.m_aTexY), std::begin(TexCoord.m_aTexY) + 3, std::end(TexCoord.m_aTexY));
	}
	return TexCoord;
}

template<std::size_t N>
constexpr static std::array<CTexCoords, N> MakeTexCoordsTable()
{
	std::array<CTexCoords, N> aTexCoords = {};
	for(std::size_t i = 0; i < N; ++i)
		aTexCoords[i] = CalculateTexCoords(i);
	return aTexCoords;
}

constexpr std::array<CTexCoords, 8> TEX_COORDS_TABLE = MakeTexCoordsTable<8>();

static void FillTmpTile(CGraphicTile *pTmpTile, CGraphicTileTextureCoords *pTmpTex, unsigned char Flags, unsigned char Index, int x, int y, const ivec2 &Offset, int Scale)
{
	if(pTmpTex)
	{
		uint8_t TableFlag = (Flags & (TILEFLAG_XFLIP | TILEFLAG_YFLIP)) + ((Flags & TILEFLAG_ROTATE) >> 1);
		const auto &aTexX = TEX_COORDS_TABLE[TableFlag].m_aTexX;
		const auto &aTexY = TEX_COORDS_TABLE[TableFlag].m_aTexY;

		pTmpTex->m_TexCoordTopLeft.x = aTexX[0];
		pTmpTex->m_TexCoordTopLeft.y = aTexY[0];
		pTmpTex->m_TexCoordBottomLeft.x = aTexX[3];
		pTmpTex->m_TexCoordBottomLeft.y = aTexY[3];
		pTmpTex->m_TexCoordTopRight.x = aTexX[1];
		pTmpTex->m_TexCoordTopRight.y = aTexY[1];
		pTmpTex->m_TexCoordBottomRight.x = aTexX[2];
		pTmpTex->m_TexCoordBottomRight.y = aTexY[2];

		pTmpTex->m_TexCoordTopLeft.z = Index;
		pTmpTex->m_TexCoordBottomLeft.z = Index;
		pTmpTex->m_TexCoordTopRight.z = Index;
		pTmpTex->m_TexCoordBottomRight.z = Index;

		bool HasRotation = (Flags & TILEFLAG_ROTATE) != 0;
		pTmpTex->m_TexCoordTopLeft.w = HasRotation;
		pTmpTex->m_TexCoordBottomLeft.w = HasRotation;
		pTmpTex->m_TexCoordTopRight.w = HasRotation;
		pTmpTex->m_TexCoordBottomRight.w = HasRotation;
	}

	vec2 TopLeft(x * Scale + Offset.x, y * Scale + Offset.y);
	vec2 BottomRight(x * Scale + Scale + Offset.x, y * Scale + Scale + Offset.y);
	pTmpTile->m_TopLeft = TopLeft;
	pTmpTile->m_BottomLeft.x = TopLeft.x;
	pTmpTile->m_BottomLeft.y = BottomRight.y;
	pTmpTile->m_TopRight.x = BottomRight.x;
	pTmpTile->m_TopRight.y = TopLeft.y;
	pTmpTile->m_BottomRight = BottomRight;
}

static void FillTmpTileSpeedup(CGraphicTile *pTmpTile, CGraphicTileTextureCoords *pTmpTex, unsigned char Flags, int x, int y, const ivec2 &Offset, int Scale, short AngleRotate)
{
	int Angle = AngleRotate % 360;
	FillTmpTile(pTmpTile, pTmpTex, Angle >= 270 ? ROTATION_270 : (Angle >= 180 ? ROTATION_180 : (Angle >= 90 ? ROTATION_90 : 0)), AngleRotate % 90, x, y, Offset, Scale);
}

bool AddTileToBuffer(std::vector<CGraphicTile> &vTmpTiles, std::vector<CGraphicTileTextureCoords> &vTmpTileTexCoords, unsigned char Index, unsigned char Flags, int x, int y, bool DoTextureCoords, bool FillSpeedup, int AngleRotate, const ivec2 &Offset, int Scale)
{
	if(Index <= 0)
		return false;

	vTmpTiles.emplace_back();
	CGraphicTile &Tile = vTmpTiles.back();
	CGraphicTileTextureCoords *pTileTex = nullptr;
	if(DoTextureCoords)
	{
		vTmpTileTexCoords.emplace_back();
		CGraphicTileTextureCoords &TileTex = vTmpTileTexCoords.back();
		pTileTex = &TileTex;
	}
	if(FillSpeedup)
		FillTmpTileSpeedup(&Tile, pTileTex, Flags, x, y, Offset, Scale, AngleRotate);
	else
		FillTmpTile(&Tile, pTileTex, Flags, Index, x, y, Offset, Scale);

	return true;
}

void DeleteTileBuffer(IGraphics *pGraphics, IGraphics::CBufferHandle &BufferObject, IGraphics::CBufferContainerHandle &BufferContainer)
{
	if(BufferContainer.IsValid())
	{
		pGraphics->DeleteBufferContainer(BufferContainer);
		if(!BufferContainer.IsValid())
			BufferObject.Invalidate();
	}
	else
	{
		pGraphics->DeleteBufferObject(BufferObject);
	}
}

bool UploadTileBuffer(IGraphics *pGraphics, const std::vector<CGraphicTile> &vTiles, const std::vector<CGraphicTileTextureCoords> &vTextureCoords, IGraphics::CBufferHandle &BufferObject, IGraphics::CBufferContainerHandle &BufferContainer)
{
	const bool Textured = !vTextureCoords.empty();
	dbg_assert(!Textured || vTextureCoords.size() == vTiles.size(), "Tile texture coordinate count must match tile count");
	if(vTiles.empty())
	{
		DeleteTileBuffer(pGraphics, BufferObject, BufferContainer);
		return true;
	}

	if(!pGraphics->IndicesNumRequiredNotify(vTiles.size() * 6))
		return false;

	const size_t UploadDataSize = vTiles.size() * sizeof(CGraphicTile) + vTextureCoords.size() * sizeof(CGraphicTileTextureCoords);
	void *pUploadData = malloc(UploadDataSize);
	if(pUploadData == nullptr)
		return false;

	if(Textured)
	{
		class CVertex
		{
		public:
			vec2 m_Pos;
			ubvec4 m_Tex;
		};
		static_assert(sizeof(CVertex) == sizeof(vec2) + sizeof(ubvec4));

		CVertex *pDst = static_cast<CVertex *>(pUploadData);
		dbg_assert(UploadDataSize == vTiles.size() * sizeof(*pDst) * 4, "invalid upload size");
		for(size_t TileIndex = 0; TileIndex < vTiles.size(); ++TileIndex)
		{
			const auto &Tile = vTiles[TileIndex];
			const auto &Coords = vTextureCoords[TileIndex];
			*pDst++ = {Tile.m_TopLeft, Coords.m_TexCoordTopLeft};
			*pDst++ = {Tile.m_TopRight, Coords.m_TexCoordTopRight};
			*pDst++ = {Tile.m_BottomRight, Coords.m_TexCoordBottomRight};
			*pDst++ = {Tile.m_BottomLeft, Coords.m_TexCoordBottomLeft};
		}
	}
	else
	{
		dbg_assert(UploadDataSize == vTiles.size() * sizeof(CGraphicTile), "invalid upload size");
		mem_copy(pUploadData, vTiles.data(), UploadDataSize);
	}

	if(BufferObject.IsValid())
	{
		if(pGraphics->RecreateBufferObject(BufferObject, UploadDataSize, pUploadData, 0, true))
			return true;
		free(pUploadData);
		return false;
	}

	BufferObject = pGraphics->CreateBufferObject(UploadDataSize, pUploadData, 0, true);
	if(!BufferObject.IsValid())
	{
		free(pUploadData);
		return false;
	}

	SBufferContainerInfo ContainerInfo;
	ContainerInfo.m_Stride = Textured ? sizeof(vec2) + sizeof(ubvec4) : 0;
	ContainerInfo.m_VertBufferBinding = BufferObject;
	ContainerInfo.m_vAttributes.emplace_back(2, IGraphics::EVertexAttributeType::FLOAT32, false, 0, IGraphics::EVertexAttributeMode::FLOAT);
	if(Textured)
		ContainerInfo.m_vAttributes.emplace_back(4, IGraphics::EVertexAttributeType::UINT8, false, sizeof(vec2), IGraphics::EVertexAttributeMode::INTEGER);
	BufferContainer = pGraphics->CreateBufferContainer(&ContainerInfo);
	if(BufferContainer.IsValid())
		return true;

	pGraphics->DeleteBufferObject(BufferObject);
	return false;
}

class CTmpQuadVertexTextured
{
public:
	float m_X, m_Y, m_CenterX, m_CenterY;
	unsigned char m_R, m_G, m_B, m_A;
	float m_U, m_V;
};

class CTmpQuadVertex
{
public:
	float m_X, m_Y, m_CenterX, m_CenterY;
	unsigned char m_R, m_G, m_B, m_A;
};

class CTmpQuad
{
public:
	CTmpQuadVertex m_aVertices[4];
};

class CTmpQuadTextured
{
public:
	CTmpQuadVertexTextured m_aVertices[4];
};

bool CRenderLayerTile::CTileLayerVisuals::Init(unsigned int Width, unsigned int Height)
{
	m_Width = Width;
	m_Height = Height;
	if(Width == 0 || Height == 0)
		return false;
	if constexpr(sizeof(unsigned int) >= sizeof(ptrdiff_t))
		if(Width >= std::numeric_limits<std::ptrdiff_t>::max() || Height >= std::numeric_limits<std::ptrdiff_t>::max())
			return false;

	m_vBorderTop.resize(Width);
	m_vBorderBottom.resize(Width);

	m_vBorderLeft.resize(Height);
	m_vBorderRight.resize(Height);
	return true;
}

/**************
 * Base Layer *
 **************/

CRenderLayer::CRenderLayer(int GroupId, int LayerId, int Flags) :
	m_GroupId(GroupId), m_LayerId(LayerId), m_Flags(Flags) {}

void CRenderLayer::OnInit(IGraphics *pGraphics, ITextRender *pTextRender, CRenderMap *pRenderMap, std::shared_ptr<CEnvelopeManager> &pEnvelopeManager, IMap *pMap, IMapImages *pMapImages, std::optional<FCallbackLayerInit> &CallbackLayerInitOptional)
{
	CRenderComponent::OnInit(pGraphics, pTextRender, pRenderMap);
	m_pMap = pMap;
	m_pMapImages = pMapImages;
	m_InitCallback = CallbackLayerInitOptional;
	m_pEnvelopeManager = pEnvelopeManager;
}

void CRenderLayer::UseTexture(IGraphics::CTextureHandle TextureHandle)
{
	if(TextureHandle.IsValid())
		Graphics()->TextureSet(TextureHandle);
	else
		Graphics()->TextureClear();
}

bool CRenderLayer::IsVisibleInClipRegion(const std::optional<CClipRegion> &ClipRegion) const
{
	// always show unclipped regions
	if(!ClipRegion.has_value())
		return true;

	CScreenRect ScreenRect = Graphics()->GetScreen();
	float Left = ClipRegion->m_X;
	float Top = ClipRegion->m_Y;
	float Right = ClipRegion->m_X + ClipRegion->m_Width;
	float Bottom = ClipRegion->m_Y + ClipRegion->m_Height;

	return Right >= ScreenRect.m_TopLeft.x && Left <= ScreenRect.m_BottomRight.x && Bottom >= ScreenRect.m_TopLeft.y && Top <= ScreenRect.m_BottomRight.y;
}

void CRenderLayer::InitCallback() const
{
	if(m_InitCallback.has_value())
	{
		(*m_InitCallback)(m_GroupId, m_LayerId);
	}
}

/**************
 * Group *
 **************/

CRenderLayerGroup::CRenderLayerGroup(int GroupId, CMapItemGroup *pGroup) :
	CRenderLayer(GroupId, 0, 0), m_pGroup(pGroup) {}

bool CRenderLayerGroup::DoRender(const CRenderLayerParams &Params)
{
	if(!g_Config.m_GfxNoclip || Params.m_RenderType == ERenderType::RENDERTYPE_FULL_DESIGN)
	{
		Graphics()->ClipDisable();
		if(m_pGroup->m_Version >= 2 && m_pGroup->m_UseClipping)
		{
			// set clipping
			Graphics()->MapScreenToInterface(Params.m_Center.x, Params.m_Center.y, Params.m_Zoom);

			CScreenRect ScreenRect = Graphics()->GetScreen();
			float ScreenWidth = ScreenRect.Width();
			float ScreenHeight = ScreenRect.Height();
			float Left = m_pGroup->m_ClipX - ScreenRect.m_TopLeft.x;
			float Top = m_pGroup->m_ClipY - ScreenRect.m_TopLeft.y;
			float Right = m_pGroup->m_ClipX + m_pGroup->m_ClipW - ScreenRect.m_TopLeft.x;
			float Bottom = m_pGroup->m_ClipY + m_pGroup->m_ClipH - ScreenRect.m_TopLeft.y;

			if(Right < 0.0f || Left > ScreenWidth || Bottom < 0.0f || Top > ScreenHeight)
				return false;

			// Render debug before enabling the clip
			if(Params.m_DebugRenderGroupClips)
			{
				char aDebugText[32];
				str_format(aDebugText, sizeof(aDebugText), "Group %d", m_GroupId);
				RenderMap()->RenderDebugClip(m_pGroup->m_ClipX, m_pGroup->m_ClipY, m_pGroup->m_ClipW, m_pGroup->m_ClipH, ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f), Params.m_Zoom, aDebugText);
			}

			int ClipX = (int)std::round(Left * Graphics()->ScreenWidth() / ScreenWidth);
			int ClipY = (int)std::round(Top * Graphics()->ScreenHeight() / ScreenHeight);

			Graphics()->ClipEnable(
				ClipX,
				ClipY,
				(int)std::round(Right * Graphics()->ScreenWidth() / ScreenWidth) - ClipX,
				(int)std::round(Bottom * Graphics()->ScreenHeight() / ScreenHeight) - ClipY);
		}
	}
	return true;
}

void CRenderLayerGroup::InitCallback() const
{
	if(m_InitCallback.has_value())
	{
		// Report layer 0, even if this is not a layer so the loading bar is monotone
		(*m_InitCallback)(m_GroupId, 0);
	}
}

void CRenderLayerGroup::Init()
{
	InitCallback();
}

void CRenderLayerGroup::Render(const CRenderLayerParams &Params)
{
	int ParallaxZoom = std::clamp(std::max(m_pGroup->m_ParallaxX, m_pGroup->m_ParallaxY), 0, 100);
	CScreenRect ScreenRect = Graphics()->MapScreenToWorld(Params.m_Center.x, Params.m_Center.y, m_pGroup->m_ParallaxX, m_pGroup->m_ParallaxY, (float)ParallaxZoom,
		m_pGroup->m_OffsetX, m_pGroup->m_OffsetY, Graphics()->ScreenAspect(), Params.m_Zoom);
	Graphics()->MapScreen(ScreenRect);
}

/**************
 * Tile Layer *
 **************/

CRenderLayerTile::CRenderLayerTile(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayer(GroupId, LayerId, Flags)
{
	m_pLayerTilemap = pLayerTilemap;
	m_Color = ColorRGBA(m_pLayerTilemap->m_Color.r / 255.0f, m_pLayerTilemap->m_Color.g / 255.0f, m_pLayerTilemap->m_Color.b / 255.0f, pLayerTilemap->m_Color.a / 255.0f);
	m_pTiles = nullptr;
}

void CRenderLayerTile::RenderTileLayer(const ColorRGBA &Color, const CRenderLayerParams &Params, CTileLayerVisuals *pTileLayerVisuals)
{
	CTileLayerVisuals &Visuals = pTileLayerVisuals ? *pTileLayerVisuals : m_VisualTiles.value();
	if(Visuals.m_Width == 0 || Visuals.m_Height == 0)
		return; // no visuals were created

	if(IsVisibleInClipRegion(m_LayerClip))
	{
		CTileChunkCache::CLayerSource Source;
		Source.m_Width = (int)Visuals.m_Width;
		Source.m_Height = (int)Visuals.m_Height;
		Source.m_Textured = Visuals.m_IsTextured;
		Source.m_FillSpeedup = Visuals.m_FillSpeedup;
		const int CurOverlay = Visuals.m_CurOverlay;
		Source.m_ReadTile = [this, CurOverlay](int x, int y, unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate) {
			GetTileData(pIndex, pFlags, pAngleRotate, static_cast<unsigned int>(x), static_cast<unsigned int>(y), CurOverlay);
		};
		// Opaque tiles cover the largest area in the game, so they are worth
		// taking out of the blended pass, the same way the unbuffered path and
		// the editor do it. Tiles within a layer never overlap, so splitting the
		// layer into two passes cannot change what is drawn.
		const bool Opaque = Color.a > 254.0f / 255.0f;
		if(Opaque)
		{
			Graphics()->BlendNone();
			Visuals.m_ChunkCache.Render(Source, Color, false, false);
			Graphics()->BlendNormal();
		}
		Visuals.m_ChunkCache.Render(Source, Color, true, !Opaque);
	}

	if(Params.m_RenderTileBorder)
	{
		const CScreenRect ScreenRect = Graphics()->GetScreen();
		const int ScreenRectY0 = std::floor(ScreenRect.m_TopLeft.y / 32);
		const int ScreenRectX0 = std::floor(ScreenRect.m_TopLeft.x / 32);
		const int ScreenRectY1 = std::ceil(ScreenRect.m_BottomRight.y / 32);
		const int ScreenRectX1 = std::ceil(ScreenRect.m_BottomRight.x / 32);
		if(ScreenRectX1 > (int)Visuals.m_Width || ScreenRectY1 > (int)Visuals.m_Height || ScreenRectX0 < 0 || ScreenRectY0 < 0)
		{
			RenderTileBorder(Color, ScreenRectX0, ScreenRectY0, ScreenRectX1, ScreenRectY1, &Visuals);
		}
	}
}

void CRenderLayerTile::RenderTileBorder(const ColorRGBA &Color, int BorderX0, int BorderY0, int BorderX1, int BorderY1, CTileLayerVisuals *pTileLayerVisuals)
{
	CTileLayerVisuals &Visuals = *pTileLayerVisuals;
	if(!Visuals.m_BufferContainerIndex.IsValid())
		return; // the layer has no border tiles

	int Y0 = std::max(0, BorderY0);
	int X0 = std::max(0, BorderX0);
	int Y1 = std::min((int)Visuals.m_Height, BorderY1);
	int X1 = std::min((int)Visuals.m_Width, BorderX1);

	// corners
	auto DrawCorner = [&](vec2 Offset, vec2 Scale, CTileLayerVisuals::CTileVisual &Visual) {
		Offset *= 32.0f;
		Graphics()->RenderBorderTiles(Visuals.m_BufferContainerIndex, Color, (offset_ptr_size)Visual.IndexBufferByteOffset(), Offset, Scale, 1);
	};

	if(BorderX0 < 0)
	{
		// Draw corners on left side
		if(BorderY0 < 0 && Visuals.m_BorderTopLeft.DoDraw())
		{
			DrawCorner(
				vec2(0, 0),
				vec2(std::abs(BorderX0), std::abs(BorderY0)),
				Visuals.m_BorderTopLeft);
		}
		if(BorderY1 > (int)Visuals.m_Height && Visuals.m_BorderBottomLeft.DoDraw())
		{
			DrawCorner(
				vec2(0, Visuals.m_Height),
				vec2(std::abs(BorderX0), BorderY1 - Visuals.m_Height),
				Visuals.m_BorderBottomLeft);
		}
	}
	if(BorderX1 > (int)Visuals.m_Width)
	{
		// Draw corners on right side
		if(BorderY0 < 0 && Visuals.m_BorderTopRight.DoDraw())
		{
			DrawCorner(
				vec2(Visuals.m_Width, 0),
				vec2(BorderX1 - Visuals.m_Width, std::abs(BorderY0)),
				Visuals.m_BorderTopRight);
		}
		if(BorderY1 > (int)Visuals.m_Height && Visuals.m_BorderBottomRight.DoDraw())
		{
			DrawCorner(
				vec2(Visuals.m_Width, Visuals.m_Height),
				vec2(BorderX1 - Visuals.m_Width, BorderY1 - Visuals.m_Height),
				Visuals.m_BorderBottomRight);
		}
	}

	// borders
	auto DrawBorder = [&](vec2 Offset, vec2 Scale, CTileLayerVisuals::CTileVisual &StartVisual, CTileLayerVisuals::CTileVisual &EndVisual) {
		unsigned int DrawNum = ((EndVisual.IndexBufferByteOffset() - StartVisual.IndexBufferByteOffset()) / (sizeof(unsigned int) * 6)) + (EndVisual.DoDraw() ? 1lu : 0lu);
		offset_ptr_size pOffset = (offset_ptr_size)StartVisual.IndexBufferByteOffset();
		Offset *= 32.0f;
		Graphics()->RenderBorderTiles(Visuals.m_BufferContainerIndex, Color, pOffset, Offset, Scale, DrawNum);
	};

	if(Y0 < (int)Visuals.m_Height && Y1 > 0)
	{
		if(BorderX1 > (int)Visuals.m_Width)
		{
			// Draw right border
			DrawBorder(
				vec2(Visuals.m_Width, 0),
				vec2(BorderX1 - Visuals.m_Width, 1.f),
				Visuals.m_vBorderRight[Y0], Visuals.m_vBorderRight[Y1 - 1]);
		}
		if(BorderX0 < 0)
		{
			// Draw left border
			DrawBorder(
				vec2(0, 0),
				vec2(std::abs(BorderX0), 1),
				Visuals.m_vBorderLeft[Y0], Visuals.m_vBorderLeft[Y1 - 1]);
		}
	}

	if(X0 < (int)Visuals.m_Width && X1 > 0)
	{
		if(BorderY0 < 0)
		{
			// Draw top border
			DrawBorder(
				vec2(0, 0),
				vec2(1, std::abs(BorderY0)),
				Visuals.m_vBorderTop[X0], Visuals.m_vBorderTop[X1 - 1]);
		}
		if(BorderY1 > (int)Visuals.m_Height)
		{
			// Draw bottom border
			DrawBorder(
				vec2(0, Visuals.m_Height),
				vec2(1, BorderY1 - Visuals.m_Height),
				Visuals.m_vBorderBottom[X0], Visuals.m_vBorderBottom[X1 - 1]);
		}
	}
}

void CRenderLayerTile::RenderKillTileBorder(const ColorRGBA &Color)
{
	CTileLayerVisuals &Visuals = m_VisualTiles.value();
	if(!Visuals.m_BufferContainerIndex.IsValid())
		return; // no visuals were created

	CScreenRect ScreenRect = Graphics()->GetScreen();

	int BorderY0 = std::floor(ScreenRect.m_TopLeft.y / 32);
	int BorderX0 = std::floor(ScreenRect.m_TopLeft.x / 32);
	int BorderY1 = std::ceil(ScreenRect.m_BottomRight.y / 32);
	int BorderX1 = std::ceil(ScreenRect.m_BottomRight.x / 32);

	if(BorderX0 >= -BorderRenderDistance && BorderY0 >= -BorderRenderDistance && BorderX1 <= (int)Visuals.m_Width + BorderRenderDistance && BorderY1 <= (int)Visuals.m_Height + BorderRenderDistance)
		return;
	if(!Visuals.m_BorderKillTile.DoDraw())
		return;

	BorderX0 = std::clamp(BorderX0, -300, (int)Visuals.m_Width + 299);
	BorderY0 = std::clamp(BorderY0, -300, (int)Visuals.m_Height + 299);
	BorderX1 = std::clamp(BorderX1, -300, (int)Visuals.m_Width + 299);
	BorderY1 = std::clamp(BorderY1, -300, (int)Visuals.m_Height + 299);

	auto DrawKillBorder = [&](vec2 Offset, vec2 Scale) {
		offset_ptr_size pOffset = (offset_ptr_size)Visuals.m_BorderKillTile.IndexBufferByteOffset();
		Offset *= 32.0f;
		Graphics()->RenderBorderTiles(Visuals.m_BufferContainerIndex, Color, pOffset, Offset, Scale, 1);
	};

	// Draw left kill tile border
	if(BorderX0 < -BorderRenderDistance)
	{
		DrawKillBorder(
			vec2(BorderX0, BorderY0),
			vec2(-BorderRenderDistance - BorderX0, BorderY1 - BorderY0));
	}
	// Draw top kill tile border
	if(BorderY0 < -BorderRenderDistance)
	{
		DrawKillBorder(
			vec2(std::max(BorderX0, -BorderRenderDistance), BorderY0),
			vec2(std::min(BorderX1, (int)Visuals.m_Width + BorderRenderDistance) - std::max(BorderX0, -BorderRenderDistance), -BorderRenderDistance - BorderY0));
	}
	// Draw right kill tile border
	if(BorderX1 > (int)Visuals.m_Width + BorderRenderDistance)
	{
		DrawKillBorder(
			vec2(Visuals.m_Width + BorderRenderDistance, BorderY0),
			vec2(BorderX1 - (Visuals.m_Width + BorderRenderDistance), BorderY1 - BorderY0));
	}
	// Draw bottom kill tile border
	if(BorderY1 > (int)Visuals.m_Height + BorderRenderDistance)
	{
		DrawKillBorder(
			vec2(std::max(BorderX0, -BorderRenderDistance), Visuals.m_Height + BorderRenderDistance),
			vec2(std::min(BorderX1, (int)Visuals.m_Width + BorderRenderDistance) - std::max(BorderX0, -BorderRenderDistance), BorderY1 - (Visuals.m_Height + BorderRenderDistance)));
	}
}

ColorRGBA CRenderLayerTile::GetRenderColor(const CRenderLayerParams &Params) const
{
	ColorRGBA Color = m_Color;
	if(Params.m_EntityOverlayVal && Params.m_RenderType != ERenderType::RENDERTYPE_BACKGROUND_FORCE)
		Color.a *= (100 - Params.m_EntityOverlayVal) / 100.0f;

	ColorRGBA ColorEnv = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	m_pEnvelopeManager->EnvelopeEval()->EnvelopeEval(m_pLayerTilemap->m_ColorEnvOffset, m_pLayerTilemap->m_ColorEnv, ColorEnv, 4);
	Color = Color.Multiply(ColorEnv);
	return Color;
}

void CRenderLayerTile::Render(const CRenderLayerParams &Params)
{
	UseTexture(GetTexture());
	ColorRGBA Color = GetRenderColor(Params);
	if(Graphics()->IsTileBufferingEnabled() && Params.m_TileAndQuadBuffering)
	{
		RenderTileLayerWithTileBuffer(Color, Params);
	}
	else
	{
		RenderTileLayerNoTileBuffer(Color, Params);
	}

	if(Params.m_DebugRenderTileClips && m_LayerClip.has_value())
	{
		const CClipRegion &Clip = m_LayerClip.value();
		char aDebugText[32];
		str_format(aDebugText, sizeof(aDebugText), "Group %d LayerId %d", m_GroupId, m_LayerId);
		RenderMap()->RenderDebugClip(Clip.m_X, Clip.m_Y, Clip.m_Width, Clip.m_Height, ColorRGBA(1.0f, 0.5f, 0.0f, 1.0f), Params.m_Zoom, aDebugText);
	}
}

bool CRenderLayerTile::DoRender(const CRenderLayerParams &Params)
{
	// skip rendering if we render background force, but deactivated tile layer and want to render a tilelayer
	if(!g_Config.m_ClBackgroundShowTilesLayers && Params.m_RenderType == ERenderType::RENDERTYPE_BACKGROUND_FORCE)
		return false;

	// skip rendering anything but entities if we only want to render entities
	if(Params.m_EntityOverlayVal == 100 && Params.m_RenderType != ERenderType::RENDERTYPE_BACKGROUND_FORCE)
		return false;

	// skip rendering if detail layers if not wanted
	if(m_Flags & LAYERFLAG_DETAIL && !g_Config.m_GfxHighDetail && Params.m_RenderType != ERenderType::RENDERTYPE_FULL_DESIGN) // detail but no details
		return false;
	return true;
}

void CRenderLayerTile::RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	RenderTileLayer(Color, Params);
}

void CRenderLayerTile::RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	const bool TextureIsValid = GetTexture().IsValid();
	Graphics()->BlendNone();
	RenderMap()->RenderTilemap(m_pTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, TextureIsValid, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_OPAQUE);
	Graphics()->BlendNormal();
	RenderMap()->RenderTilemap(m_pTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, TextureIsValid, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_TRANSPARENT);
}

void CRenderLayerTile::Init()
{
	InitCallback();
	UploadTileData(m_VisualTiles, 0, false);
}

IGraphics::CTextureHandle CRenderLayerTile::GetTexture() const
{
	return HasTexture() ? m_pMapImages->Get(m_pLayerTilemap->m_Image) : IGraphics::CTextureHandle();
}

bool CRenderLayerTile::HasTexture() const
{
	return m_pLayerTilemap->m_Image >= 0 && m_pLayerTilemap->m_Image < m_pMapImages->Num();
}

void CRenderLayerTile::UploadTileData(std::optional<CTileLayerVisuals> &VisualsOptional, int CurOverlay, bool AddAsSpeedup, bool IsGameLayer)
{
	if(!Graphics()->IsTileBufferingEnabled())
		return;

	const bool DoTextureCoords = HasTexture();

	// create the visual and set it in the optional, afterwards get it
	VisualsOptional.emplace();
	CTileLayerVisuals &Visuals = VisualsOptional.value();
	Visuals.OnInit(this);
	Visuals.m_ChunkCache.OnInit(Graphics());
	Visuals.m_IsTextured = DoTextureCoords;
	Visuals.m_CurOverlay = CurOverlay;
	Visuals.m_FillSpeedup = AddAsSpeedup;

	const int Width = m_pLayerTilemap->m_Width;
	const int Height = m_pLayerTilemap->m_Height;
	if(!Visuals.Init(Width, Height))
		return;

	// The layer itself is built per chunk while rendering, only the tiles that
	// repeat the map edges outwards are uploaded here. They are one row or one
	// column each, so this buffer stays small no matter how big the map is.
	std::vector<CGraphicTile> vTiles;
	std::vector<CGraphicTileTextureCoords> vTexCoords;
	const size_t BorderTileCount = 2 * (size_t)(Width + Height) + 5;
	vTiles.reserve(BorderTileCount);
	if(DoTextureCoords)
		vTexCoords.reserve(BorderTileCount);

	// Adds one tile at its final place in the buffer, so that no offset has to
	// be fixed up afterwards. A tile that is not drawn keeps the offset of the
	// next one, which is what makes a range of them a subtraction.
	auto AddBorderTile = [&](CTileLayerVisuals::CTileVisual &Visual, int TileX, int TileY, int PosX, int PosY, const ivec2 &Offset) {
		unsigned char Index = 0;
		unsigned char Flags = 0;
		int AngleRotate = -1;
		GetTileData(&Index, &Flags, &AngleRotate, static_cast<unsigned int>(TileX), static_cast<unsigned int>(TileY), CurOverlay);
		Visual.SetIndexBufferByteOffset((offset_ptr32)vTiles.size());
		if(AddTileToBuffer(vTiles, vTexCoords, Index, Flags, PosX, PosY, DoTextureCoords, AddAsSpeedup, AngleRotate, Offset))
			Visual.Draw(true);
	};

	// append one kill tile to the gamelayer
	if(IsGameLayer)
	{
		Visuals.m_BorderKillTile.SetIndexBufferByteOffset((offset_ptr32)vTiles.size());
		if(AddTileToBuffer(vTiles, vTexCoords, TILE_DEATH, 0, 0, 0, DoTextureCoords))
			Visuals.m_BorderKillTile.Draw(true);
	}

	// the corners are built at the layer origin and moved into place when drawn
	AddBorderTile(Visuals.m_BorderTopLeft, 0, 0, 0, 0, ivec2{-32, -32});
	AddBorderTile(Visuals.m_BorderTopRight, Width - 1, 0, 0, 0, ivec2{0, -32});
	AddBorderTile(Visuals.m_BorderBottomLeft, 0, Height - 1, 0, 0, ivec2{-32, 0});
	AddBorderTile(Visuals.m_BorderBottomRight, Width - 1, Height - 1, 0, 0, ivec2{0, 0});

	// one contiguous range per side, because that is how they are drawn
	for(int x = 0; x < Width; ++x)
		AddBorderTile(Visuals.m_vBorderTop[x], x, 0, x, 0, ivec2{0, -32});
	for(int x = 0; x < Width; ++x)
		AddBorderTile(Visuals.m_vBorderBottom[x], x, Height - 1, x, 0, ivec2{0, 0});
	for(int y = 0; y < Height; ++y)
		AddBorderTile(Visuals.m_vBorderLeft[y], 0, y, 0, y, ivec2{-32, 0});
	for(int y = 0; y < Height; ++y)
		AddBorderTile(Visuals.m_vBorderRight[y], Width - 1, y, 0, y, ivec2{0, 0});

	Visuals.m_BufferContainerIndex.Invalidate();
	if(!UploadTileBuffer(Graphics(), vTiles, vTexCoords, Visuals.m_BufferObjectIndex, Visuals.m_BufferContainerIndex))
	{
		return;
	}
}

void CRenderLayerTile::Unload()
{
	if(m_VisualTiles.has_value())
	{
		if(m_VisualTiles->Unload())
			m_VisualTiles = std::nullopt;
	}
}

bool CRenderLayerTile::CTileLayerVisuals::Unload()
{
	m_ChunkCache.Clear();
	DeleteTileBuffer(Graphics(), m_BufferObjectIndex, m_BufferContainerIndex);
	return !m_BufferContainerIndex.IsValid() && !m_BufferObjectIndex.IsValid();
}

int CRenderLayerTile::GetDataIndex() const
{
	return m_pLayerTilemap->m_Data;
}

void *CRenderLayerTile::GetRawData() const
{
	return m_pMap->GetData(GetDataIndex());
}

void CRenderLayerTile::OnInit(IGraphics *pGraphics, ITextRender *pTextRender, CRenderMap *pRenderMap, std::shared_ptr<CEnvelopeManager> &pEnvelopeManager, IMap *pMap, IMapImages *pMapImages, std::optional<FCallbackLayerInit> &CallbackLayerInitOptional)
{
	CRenderLayer::OnInit(pGraphics, pTextRender, pRenderMap, pEnvelopeManager, pMap, pMapImages, CallbackLayerInitOptional);
	InitTileData();

	// shrink the clip region to the tiles that are actually drawn. Reading the
	// indices is cheap and has to happen for both backends, because the
	// buffered one only builds geometry for the chunks it gets to see.
	int MinX = m_pLayerTilemap->m_Width;
	int MaxX = 0;
	int MinY = m_pLayerTilemap->m_Height;
	int MaxY = 0;
	for(int TileY = 0; TileY < m_pLayerTilemap->m_Height; ++TileY)
	{
		for(int TileX = 0; TileX < m_pLayerTilemap->m_Width; ++TileX)
		{
			unsigned char Index = 0;
			unsigned char Flags = 0;
			int Angle = 0;
			GetTileData(&Index, &Flags, &Angle, static_cast<unsigned int>(TileX), static_cast<unsigned int>(TileY), 0);

			if(Index > 0)
			{
				MinX = std::min(TileX, MinX);
				MaxX = std::max(TileX, MaxX);
				MinY = std::min(TileY, MinY);
				MaxY = std::max(TileY, MaxY);
			}
		}
	}

	if(MinX > MaxX || MinY > MaxY)
	{
		// layer is empty
		m_LayerClip = CClipRegion(0.0f, 0.0f, 0.0f, 0.0f);
	}
	else
	{
		m_LayerClip = CClipRegion(MinX * 32.0f, MinY * 32.0f, (MaxX - MinX + 1) * 32.0f, (MaxY - MinY + 1) * 32.0f);
	}
}

void CRenderLayerTile::InitTileData()
{
	m_pTiles = GetData<CTile>();
}

template<class T>
T *CRenderLayerTile::GetData() const
{
	return (T *)GetRawData();
}

void CRenderLayerTile::GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const
{
	*pIndex = m_pTiles[y * m_pLayerTilemap->m_Width + x].m_Index;
	*pFlags = m_pTiles[y * m_pLayerTilemap->m_Width + x].m_Flags;
}

/**************
 * Quad Layer *
 **************/

CRenderLayerQuads::CRenderLayerQuads(int GroupId, int LayerId, int Flags, CMapItemLayerQuads *pLayerQuads) :
	CRenderLayer(GroupId, LayerId, Flags)
{
	m_pLayerQuads = pLayerQuads;
	m_pQuads = nullptr;
}

void CRenderLayerQuads::RenderQuadLayer(float Alpha, const CRenderLayerParams &Params)
{
	CQuadLayerVisuals &Visuals = m_VisualQuad.value();
	if(!Visuals.m_BufferContainerIndex.IsValid())
		return; // no visuals were created

	for(auto &QuadCluster : m_vQuadClusters)
	{
		if(!IsVisibleInClipRegion(QuadCluster.m_ClipRegion))
			continue;

		if(!QuadCluster.m_Grouped)
		{
			bool AnyVisible = false;
			for(int QuadClusterId = 0; QuadClusterId < QuadCluster.m_NumQuads; ++QuadClusterId)
			{
				CQuad *pQuad = &m_pQuads[QuadCluster.m_StartIndex + QuadClusterId];

				ColorRGBA Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
				if(pQuad->m_ColorEnv >= 0)
				{
					m_pEnvelopeManager->EnvelopeEval()->EnvelopeEval(pQuad->m_ColorEnvOffset, pQuad->m_ColorEnv, Color, 4);
				}
				Color.a *= Alpha;

				SQuadRenderInfo &QInfo = QuadCluster.m_vQuadRenderInfo[QuadClusterId];
				if(Color.a < 0.0f)
					Color.a = 0.0f;
				QInfo.m_Color = Color;

				if(Color.a > 0.0f)
				{
					AnyVisible = true;
					ColorRGBA Position = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
					m_pEnvelopeManager->EnvelopeEval()->EnvelopeEval(pQuad->m_PosEnvOffset, pQuad->m_PosEnv, Position, 3);
					QInfo.m_Offsets.x = Position.r;
					QInfo.m_Offsets.y = Position.g;
					QInfo.m_Rotation = Position.b / 180.0f * pi;
				}
			}
			if(AnyVisible)
				Graphics()->RenderQuadLayer(Visuals.m_BufferContainerIndex, QuadCluster.m_vQuadRenderInfo.data(), QuadCluster.m_NumQuads, QuadCluster.m_StartIndex);
		}
		else
		{
			SQuadRenderInfo &QInfo = QuadCluster.m_vQuadRenderInfo[0];

			ColorRGBA Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
			if(QuadCluster.m_ColorEnv >= 0)
			{
				m_pEnvelopeManager->EnvelopeEval()->EnvelopeEval(QuadCluster.m_ColorEnvOffset, QuadCluster.m_ColorEnv, Color, 4);
			}

			Color.a *= Alpha;
			if(Color.a <= 0.0f)
				continue;
			QInfo.m_Color = Color;

			if(QuadCluster.m_PosEnv >= 0)
			{
				ColorRGBA Position = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
				m_pEnvelopeManager->EnvelopeEval()->EnvelopeEval(QuadCluster.m_PosEnvOffset, QuadCluster.m_PosEnv, Position, 3);

				QInfo.m_Offsets.x = Position.r;
				QInfo.m_Offsets.y = Position.g;
				QInfo.m_Rotation = Position.b / 180.0f * pi;
			}
			Graphics()->RenderQuadLayer(Visuals.m_BufferContainerIndex, &QInfo, (size_t)QuadCluster.m_NumQuads, QuadCluster.m_StartIndex, true);
		}
	}

	if(Params.m_DebugRenderClusterClips)
	{
		for(auto &QuadCluster : m_vQuadClusters)
		{
			if(!IsVisibleInClipRegion(QuadCluster.m_ClipRegion) || !QuadCluster.m_ClipRegion.has_value())
				continue;

			char aDebugText[64];
			str_format(aDebugText, sizeof(aDebugText), "Group %d, quad layer %d, quad start %d, grouped %d", m_GroupId, m_LayerId, QuadCluster.m_StartIndex, QuadCluster.m_Grouped);
			RenderMap()->RenderDebugClip(QuadCluster.m_ClipRegion->m_X, QuadCluster.m_ClipRegion->m_Y, QuadCluster.m_ClipRegion->m_Width, QuadCluster.m_ClipRegion->m_Height, ColorRGBA(1.0f, 0.0f, 1.0f, 1.0f), Params.m_Zoom, aDebugText);
		}
	}
}

void CRenderLayerQuads::OnInit(IGraphics *pGraphics, ITextRender *pTextRender, CRenderMap *pRenderMap, std::shared_ptr<CEnvelopeManager> &pEnvelopeManager, IMap *pMap, IMapImages *pMapImages, std::optional<FCallbackLayerInit> &CallbackLayerInitOptional)
{
	CRenderLayer::OnInit(pGraphics, pTextRender, pRenderMap, pEnvelopeManager, pMap, pMapImages, CallbackLayerInitOptional);
	int DataSize = m_pMap->GetDataSize(m_pLayerQuads->m_Data);
	if(m_pLayerQuads->m_NumQuads > 0 && DataSize / (int)sizeof(CQuad) >= m_pLayerQuads->m_NumQuads)
		m_pQuads = (CQuad *)m_pMap->GetDataSwapped(m_pLayerQuads->m_Data);
}

void CRenderLayerQuads::Init()
{
	InitCallback();
	if(!Graphics()->IsQuadBufferingEnabled())
	{
		// create clip region for unbuffered backends
		CQuadCluster QuadCluster;
		QuadCluster.m_Grouped = false;
		QuadCluster.m_StartIndex = 0;
		QuadCluster.m_NumQuads = m_pLayerQuads->m_NumQuads;

		// unused, because cluster is not grouped
		QuadCluster.m_PosEnv = -1;
		QuadCluster.m_PosEnvOffset = 0;
		QuadCluster.m_ColorEnv = -1;
		QuadCluster.m_ColorEnvOffset = 0;

		CalculateClipping(QuadCluster);
		return;
	}

	std::vector<CTmpQuad> vTmpQuads;
	std::vector<CTmpQuadTextured> vTmpQuadsTextured;
	CQuadLayerVisuals v;
	v.OnInit(this);
	m_VisualQuad = v;
	CQuadLayerVisuals *pQLayerVisuals = &(m_VisualQuad.value());

	const bool Textured = HasTexture();

	if(Textured)
		vTmpQuadsTextured.resize(m_pLayerQuads->m_NumQuads);
	else
		vTmpQuads.resize(m_pLayerQuads->m_NumQuads);

	auto SetQuadRenderInfo = [&](SQuadRenderInfo &QInfo, int QuadId, bool InitInfo) {
		CQuad *pQuad = &m_pQuads[QuadId];

		// init for envelopeless quad layers
		if(InitInfo)
		{
			QInfo.m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
			QInfo.m_Offsets.x = 0;
			QInfo.m_Offsets.y = 0;
			QInfo.m_Rotation = 0;
		}

		for(int j = 0; j < 4; ++j)
		{
			int QuadIdX = j;
			if(j == 2)
				QuadIdX = 3;
			else if(j == 3)
				QuadIdX = 2;
			if(!Textured)
			{
				// ignore the conversion for the position coordinates
				vTmpQuads[QuadId].m_aVertices[j].m_X = fx2f(pQuad->m_aPoints[QuadIdX].x);
				vTmpQuads[QuadId].m_aVertices[j].m_Y = fx2f(pQuad->m_aPoints[QuadIdX].y);
				vTmpQuads[QuadId].m_aVertices[j].m_CenterX = fx2f(pQuad->m_aPoints[4].x);
				vTmpQuads[QuadId].m_aVertices[j].m_CenterY = fx2f(pQuad->m_aPoints[4].y);
				vTmpQuads[QuadId].m_aVertices[j].m_R = (unsigned char)pQuad->m_aColors[QuadIdX].r;
				vTmpQuads[QuadId].m_aVertices[j].m_G = (unsigned char)pQuad->m_aColors[QuadIdX].g;
				vTmpQuads[QuadId].m_aVertices[j].m_B = (unsigned char)pQuad->m_aColors[QuadIdX].b;
				vTmpQuads[QuadId].m_aVertices[j].m_A = (unsigned char)pQuad->m_aColors[QuadIdX].a;
			}
			else
			{
				// ignore the conversion for the position coordinates
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_X = fx2f(pQuad->m_aPoints[QuadIdX].x);
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_Y = fx2f(pQuad->m_aPoints[QuadIdX].y);
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_CenterX = fx2f(pQuad->m_aPoints[4].x);
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_CenterY = fx2f(pQuad->m_aPoints[4].y);
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_U = fx2f(pQuad->m_aTexcoords[QuadIdX].x);
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_V = fx2f(pQuad->m_aTexcoords[QuadIdX].y);
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_R = (unsigned char)pQuad->m_aColors[QuadIdX].r;
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_G = (unsigned char)pQuad->m_aColors[QuadIdX].g;
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_B = (unsigned char)pQuad->m_aColors[QuadIdX].b;
				vTmpQuadsTextured[QuadId].m_aVertices[j].m_A = (unsigned char)pQuad->m_aColors[QuadIdX].a;
			}
		}
	};

	m_vQuadClusters.clear();

	// create quad clusters
	int QuadStart = 0;
	while(QuadStart < m_pLayerQuads->m_NumQuads)
	{
		CQuadCluster QuadCluster;
		QuadCluster.m_StartIndex = QuadStart;
		QuadCluster.m_Grouped = true;
		QuadCluster.m_ColorEnv = m_pQuads[QuadStart].m_ColorEnv;
		QuadCluster.m_ColorEnvOffset = m_pQuads[QuadStart].m_ColorEnvOffset;
		QuadCluster.m_PosEnv = m_pQuads[QuadStart].m_PosEnv;
		QuadCluster.m_PosEnvOffset = m_pQuads[QuadStart].m_PosEnvOffset;

		int QuadOffset = 0;
		for(int QuadClusterId = 0; QuadClusterId < m_pLayerQuads->m_NumQuads - QuadStart; ++QuadClusterId)
		{
			const CQuad *pQuad = &m_pQuads[QuadStart + QuadClusterId];
			bool IsGrouped = QuadCluster.m_Grouped && pQuad->m_ColorEnv == QuadCluster.m_ColorEnv && pQuad->m_ColorEnvOffset == QuadCluster.m_ColorEnvOffset && pQuad->m_PosEnv == QuadCluster.m_PosEnv && pQuad->m_PosEnvOffset == QuadCluster.m_PosEnvOffset;

			// we are reaching gpu batch limit, here we break and close the QuadCluster if it's ungrouped
			if(QuadClusterId >= (int)GRAPHICS_MAX_QUADS_RENDER_COUNT)
			{
				// expand a cluster, if it's grouped
				if(!IsGrouped)
					break;
			}
			QuadOffset++;
			QuadCluster.m_Grouped = IsGrouped;
		}
		QuadCluster.m_NumQuads = QuadOffset;

		// fill cluster info
		if(QuadCluster.m_Grouped)
		{
			// grouped quads only need one render info, because all their envs and env offsets are equal
			QuadCluster.m_vQuadRenderInfo.resize(1);
			for(int QuadClusterId = 0; QuadClusterId < QuadCluster.m_NumQuads; ++QuadClusterId)
				SetQuadRenderInfo(QuadCluster.m_vQuadRenderInfo[0], QuadCluster.m_StartIndex + QuadClusterId, QuadClusterId == 0);
		}
		else
		{
			QuadCluster.m_vQuadRenderInfo.resize(QuadCluster.m_NumQuads);
			for(int QuadClusterId = 0; QuadClusterId < QuadCluster.m_NumQuads; ++QuadClusterId)
				SetQuadRenderInfo(QuadCluster.m_vQuadRenderInfo[QuadClusterId], QuadCluster.m_StartIndex + QuadClusterId, true);
		}

		CalculateClipping(QuadCluster);

		m_vQuadClusters.push_back(QuadCluster);
		QuadStart += QuadOffset;
	}

	// gpu upload
	size_t UploadDataSize = 0;
	if(Textured)
		UploadDataSize = vTmpQuadsTextured.size() * sizeof(CTmpQuadTextured);
	else
		UploadDataSize = vTmpQuads.size() * sizeof(CTmpQuad);

	if(UploadDataSize > 0)
	{
		void *pUploadData = nullptr;
		if(Textured)
			pUploadData = vTmpQuadsTextured.data();
		else
			pUploadData = vTmpQuads.data();
		// create the buffer object
		IGraphics::CBufferHandle BufferObject = Graphics()->CreateBufferObject(UploadDataSize, pUploadData, 0);
		if(!BufferObject.IsValid())
		{
			return;
		}
		pQLayerVisuals->m_BufferObjectIndex = BufferObject;
		// then create the buffer container
		SBufferContainerInfo ContainerInfo;
		ContainerInfo.m_Stride = (Textured ? (sizeof(CTmpQuadTextured) / 4) : (sizeof(CTmpQuad) / 4));
		ContainerInfo.m_VertBufferBinding = BufferObject;
		ContainerInfo.m_vAttributes.emplace_back();
		SBufferContainerInfo::SAttribute *pAttr = &ContainerInfo.m_vAttributes.back();
		pAttr->m_ComponentCount = 4;
		pAttr->m_Type = IGraphics::EVertexAttributeType::FLOAT32;
		pAttr->m_Normalized = false;
		pAttr->m_Offset = 0;
		pAttr->m_Mode = IGraphics::EVertexAttributeMode::FLOAT;
		ContainerInfo.m_vAttributes.emplace_back();
		pAttr = &ContainerInfo.m_vAttributes.back();
		pAttr->m_ComponentCount = 4;
		pAttr->m_Type = IGraphics::EVertexAttributeType::UINT8;
		pAttr->m_Normalized = true;
		pAttr->m_Offset = sizeof(float) * 4;
		pAttr->m_Mode = IGraphics::EVertexAttributeMode::FLOAT;
		if(Textured)
		{
			ContainerInfo.m_vAttributes.emplace_back();
			pAttr = &ContainerInfo.m_vAttributes.back();
			pAttr->m_ComponentCount = 2;
			pAttr->m_Type = IGraphics::EVertexAttributeType::FLOAT32;
			pAttr->m_Normalized = false;
			pAttr->m_Offset = sizeof(float) * 4 + sizeof(unsigned char) * 4;
			pAttr->m_Mode = IGraphics::EVertexAttributeMode::FLOAT;
		}

		if(!Graphics()->IndicesNumRequiredNotify(m_pLayerQuads->m_NumQuads * 6))
		{
			return;
		}
		pQLayerVisuals->m_BufferContainerIndex = Graphics()->CreateBufferContainer(&ContainerInfo);
	}
}

IGraphics::CTextureHandle CRenderLayerQuads::GetTexture() const
{
	return HasTexture() ? m_pMapImages->Get(m_pLayerQuads->m_Image) : IGraphics::CTextureHandle();
}

bool CRenderLayerQuads::HasTexture() const
{
	return m_pLayerQuads->m_Image >= 0 && m_pLayerQuads->m_Image < m_pMapImages->Num();
}

void CRenderLayerQuads::Unload()
{
	if(m_VisualQuad.has_value())
	{
		if(m_VisualQuad->Unload())
			m_VisualQuad = std::nullopt;
	}
}

bool CRenderLayerQuads::CQuadLayerVisuals::Unload()
{
	if(m_BufferContainerIndex.IsValid())
	{
		Graphics()->DeleteBufferContainer(m_BufferContainerIndex);
		if(!m_BufferContainerIndex.IsValid())
			m_BufferObjectIndex.Invalidate();
	}
	else
	{
		Graphics()->DeleteBufferObject(m_BufferObjectIndex);
	}
	return !m_BufferContainerIndex.IsValid() && !m_BufferObjectIndex.IsValid();
}

bool CRenderLayerQuads::CalculateQuadClipping(const CQuadCluster &QuadCluster, float aQuadOffsetMin[2], float aQuadOffsetMax[2]) const
{
	// check if the grouped clipping is available for early exit
	if(QuadCluster.m_Grouped)
	{
		const CEnvelopeExtrema::CEnvelopeExtremaItem &Extrema = m_pEnvelopeManager->EnvelopeExtrema()->GetExtrema(QuadCluster.m_PosEnv);
		if(!Extrema.m_Available)
			return false;
	}

	// calculate quad position offsets
	for(int Channel = 0; Channel < 2; ++Channel)
	{
		aQuadOffsetMin[Channel] = std::numeric_limits<float>::max(); // minimum of channel
		aQuadOffsetMax[Channel] = std::numeric_limits<float>::lowest(); // maximum of channel
	}

	for(int QuadId = QuadCluster.m_StartIndex; QuadId < QuadCluster.m_StartIndex + QuadCluster.m_NumQuads; ++QuadId)
	{
		const CQuad *pQuad = &m_pQuads[QuadId];

		const CEnvelopeExtrema::CEnvelopeExtremaItem &Extrema = m_pEnvelopeManager->EnvelopeExtrema()->GetExtrema(pQuad->m_PosEnv);
		if(!Extrema.m_Available)
			return false;

		// calculate clip region
		if(!Extrema.m_Rotating)
		{
			for(int QuadIdPoint = 0; QuadIdPoint < 4; ++QuadIdPoint)
			{
				for(int Channel = 0; Channel < 2; ++Channel)
				{
					float OffsetMinimum = fx2f(pQuad->m_aPoints[QuadIdPoint][Channel]);
					float OffsetMaximum = fx2f(pQuad->m_aPoints[QuadIdPoint][Channel]);

					// calculate env offsets for every ungrouped quad
					if(!QuadCluster.m_Grouped && pQuad->m_PosEnv >= 0)
					{
						OffsetMinimum += fx2f(Extrema.m_Minima[Channel]);
						OffsetMaximum += fx2f(Extrema.m_Maxima[Channel]);
					}
					aQuadOffsetMin[Channel] = std::min(aQuadOffsetMin[Channel], OffsetMinimum);
					aQuadOffsetMax[Channel] = std::max(aQuadOffsetMax[Channel], OffsetMaximum);
				}
			}
		}
		else
		{
			const CPoint &CenterFX = pQuad->m_aPoints[4];
			vec2 Center(fx2f(CenterFX.x), fx2f(CenterFX.y));
			float MaxDistance = 0;
			for(int QuadIdPoint = 0; QuadIdPoint < 4; ++QuadIdPoint)
			{
				const CPoint &QuadPointFX = pQuad->m_aPoints[QuadIdPoint];
				vec2 QuadPoint(fx2f(QuadPointFX.x), fx2f(QuadPointFX.y));
				float Distance = length(Center - QuadPoint);
				MaxDistance = std::max(Distance, MaxDistance);
			}

			for(int Channel = 0; Channel < 2; ++Channel)
			{
				float OffsetMinimum = Center[Channel] - MaxDistance;
				float OffsetMaximum = Center[Channel] + MaxDistance;
				if(!QuadCluster.m_Grouped && pQuad->m_PosEnv >= 0)
				{
					OffsetMinimum += fx2f(Extrema.m_Minima[Channel]);
					OffsetMaximum += fx2f(Extrema.m_Maxima[Channel]);
				}
				aQuadOffsetMin[Channel] = std::min(aQuadOffsetMin[Channel], OffsetMinimum);
				aQuadOffsetMax[Channel] = std::max(aQuadOffsetMax[Channel], OffsetMaximum);
			}
		}
	}

	// add env offsets for the quad group
	if(QuadCluster.m_Grouped && QuadCluster.m_PosEnv >= 0)
	{
		const CEnvelopeExtrema::CEnvelopeExtremaItem &Extrema = m_pEnvelopeManager->EnvelopeExtrema()->GetExtrema(QuadCluster.m_PosEnv);

		for(int Channel = 0; Channel < 2; ++Channel)
		{
			aQuadOffsetMin[Channel] += fx2f(Extrema.m_Minima[Channel]);
			aQuadOffsetMax[Channel] += fx2f(Extrema.m_Maxima[Channel]);
		}
	}
	return true;
}

void CRenderLayerQuads::CalculateClipping(CQuadCluster &QuadCluster)
{
	float aQuadOffsetMin[2];
	float aQuadOffsetMax[2];

	bool CreateClip = CalculateQuadClipping(QuadCluster, aQuadOffsetMin, aQuadOffsetMax);

	if(!CreateClip)
		return;

	QuadCluster.m_ClipRegion = std::make_optional<CClipRegion>();
	std::optional<CClipRegion> &ClipRegion = QuadCluster.m_ClipRegion;

	// X channel
	ClipRegion->m_X = aQuadOffsetMin[0];
	ClipRegion->m_Width = aQuadOffsetMax[0] - aQuadOffsetMin[0];

	// Y channel
	ClipRegion->m_Y = aQuadOffsetMin[1];
	ClipRegion->m_Height = aQuadOffsetMax[1] - aQuadOffsetMin[1];

	// update layer clip
	if(!m_LayerClip.has_value())
	{
		m_LayerClip = ClipRegion;
	}
	else
	{
		float ClipRight = std::max(ClipRegion->m_X + ClipRegion->m_Width, m_LayerClip->m_X + m_LayerClip->m_Width);
		float ClipBottom = std::max(ClipRegion->m_Y + ClipRegion->m_Height, m_LayerClip->m_Y + m_LayerClip->m_Height);
		m_LayerClip->m_X = std::min(ClipRegion->m_X, m_LayerClip->m_X);
		m_LayerClip->m_Y = std::min(ClipRegion->m_Y, m_LayerClip->m_Y);
		m_LayerClip->m_Width = ClipRight - m_LayerClip->m_X;
		m_LayerClip->m_Height = ClipBottom - m_LayerClip->m_Y;
	}
}

void CRenderLayerQuads::Render(const CRenderLayerParams &Params)
{
	UseTexture(GetTexture());

	bool Force = Params.m_RenderType == ERenderType::RENDERTYPE_BACKGROUND_FORCE || Params.m_RenderType == ERenderType::RENDERTYPE_FULL_DESIGN;
	float Alpha = Force ? 1.f : (100 - Params.m_EntityOverlayVal) / 100.0f;
	if(!Graphics()->IsQuadBufferingEnabled() || !Params.m_TileAndQuadBuffering)
	{
		RenderMap()->ForceRenderQuads(m_pQuads, m_pLayerQuads->m_NumQuads, LAYERRENDERFLAG_TRANSPARENT, m_pEnvelopeManager->EnvelopeEval(), Alpha);
	}
	else
	{
		RenderQuadLayer(Alpha, Params);
	}

	if(Params.m_DebugRenderQuadClips && m_LayerClip.has_value())
	{
		char aDebugText[64];
		str_format(aDebugText, sizeof(aDebugText), "Group %d, quad layer %d", m_GroupId, m_LayerId);
		RenderMap()->RenderDebugClip(m_LayerClip->m_X, m_LayerClip->m_Y, m_LayerClip->m_Width, m_LayerClip->m_Height, ColorRGBA(1.0f, 0.0f, 0.5f, 1.0f), Params.m_Zoom, aDebugText);
	}
}

bool CRenderLayerQuads::DoRender(const CRenderLayerParams &Params)
{
	// skip rendering anything but entities if we only want to render entities
	if(Params.m_EntityOverlayVal == 100 && Params.m_RenderType != ERenderType::RENDERTYPE_BACKGROUND_FORCE)
		return false;

	// skip rendering if detail layers if not wanted
	if(m_Flags & LAYERFLAG_DETAIL && !g_Config.m_GfxHighDetail && Params.m_RenderType != ERenderType::RENDERTYPE_FULL_DESIGN) // detail but no details
		return false;

	// this option only deactivates quads in the background
	if(Params.m_RenderType == ERenderType::RENDERTYPE_BACKGROUND || Params.m_RenderType == ERenderType::RENDERTYPE_BACKGROUND_FORCE)
	{
		if(!g_Config.m_ClShowQuads)
			return false;
	}

	return IsVisibleInClipRegion(m_LayerClip);
}

/****************
 * Entity Layer *
 ****************/
// BASE
CRenderLayerEntityBase::CRenderLayerEntityBase(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerTile(GroupId, LayerId, Flags, pLayerTilemap) {}

bool CRenderLayerEntityBase::DoRender(const CRenderLayerParams &Params)
{
	// skip rendering if we render background force or full design
	if(Params.m_RenderType == ERenderType::RENDERTYPE_BACKGROUND_FORCE || Params.m_RenderType == ERenderType::RENDERTYPE_FULL_DESIGN)
		return false;

	// skip rendering of entities if don't want them
	if(!Params.m_EntityOverlayVal)
		return false;

	return true;
}

IGraphics::CTextureHandle CRenderLayerEntityBase::GetTexture() const
{
	return m_pMapImages->GetEntities(MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH);
}

// GAME
CRenderLayerEntityGame::CRenderLayerEntityGame(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerEntityBase(GroupId, LayerId, Flags, pLayerTilemap) {}

void CRenderLayerEntityGame::Init()
{
	InitCallback();
	UploadTileData(m_VisualTiles, 0, false, true);
}

void CRenderLayerEntityGame::RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	if(Params.m_RenderTileBorder)
		RenderKillTileBorder(Color.Multiply(GetDeathBorderColor()));
	RenderTileLayer(Color, Params);
}

void CRenderLayerEntityGame::RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	const bool TextureIsValid = GetTexture().IsValid();
	Graphics()->BlendNone();
	RenderMap()->RenderTilemap(m_pTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, TextureIsValid, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_OPAQUE);
	Graphics()->BlendNormal();

	if(Params.m_RenderTileBorder)
	{
		RenderMap()->RenderTileRectangle(-BorderRenderDistance, -BorderRenderDistance, m_pLayerTilemap->m_Width + 2 * BorderRenderDistance, m_pLayerTilemap->m_Height + 2 * BorderRenderDistance,
			TILE_AIR, TILE_DEATH, // display air inside, death outside
			32.0f, Color.Multiply(GetDeathBorderColor()), TILERENDERFLAG_EXTEND | LAYERRENDERFLAG_TRANSPARENT);
	}

	RenderMap()->RenderTilemap(m_pTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, TextureIsValid, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_TRANSPARENT);
}

ColorRGBA CRenderLayerEntityGame::GetDeathBorderColor() const
{
	// draw kill tiles outside the entity clipping rectangle
	// slow blinking to hint that it's not a part of the map
	float Seconds = time_get() / (float)time_freq();
	float Alpha = 0.3f + 0.35f * (1.f + std::sin(2.f * pi * Seconds / 3.f));
	return ColorRGBA(1.f, 1.f, 1.f, Alpha);
}

// FRONT
CRenderLayerEntityFront::CRenderLayerEntityFront(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerEntityBase(GroupId, LayerId, Flags, pLayerTilemap) {}

int CRenderLayerEntityFront::GetDataIndex() const
{
	return m_pLayerTilemap->m_Front;
}

// TELE
CRenderLayerEntityTele::CRenderLayerEntityTele(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerEntityBase(GroupId, LayerId, Flags, pLayerTilemap) {}

int CRenderLayerEntityTele::GetDataIndex() const
{
	return m_pLayerTilemap->m_Tele;
}

void CRenderLayerEntityTele::Init()
{
	InitCallback();
	UploadTileData(m_VisualTiles, 0, false);
	UploadTileData(m_VisualTeleNumbers, 1, false);
}

void CRenderLayerEntityTele::InitTileData()
{
	m_pTeleTiles = GetData<CTeleTile>();
}

void CRenderLayerEntityTele::Unload()
{
	CRenderLayerTile::Unload();
	if(m_VisualTeleNumbers.has_value())
	{
		m_VisualTeleNumbers->Unload();
		m_VisualTeleNumbers = std::nullopt;
	}
}

void CRenderLayerEntityTele::RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	RenderTileLayer(Color, Params);
	if(Params.m_RenderText)
	{
		Graphics()->TextureSet(m_pMapImages->GetOverlayCenter());
		RenderTileLayer(Color, Params, &m_VisualTeleNumbers.value());
	}
}

void CRenderLayerEntityTele::RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	Graphics()->BlendNone();
	RenderMap()->RenderTelemap(m_pTeleTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_OPAQUE);
	Graphics()->BlendNormal();
	RenderMap()->RenderTelemap(m_pTeleTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_TRANSPARENT);
	int OverlayRenderFlags = (Params.m_RenderText ? OVERLAYRENDERFLAG_TEXT : 0) | (Params.m_RenderInvalidTiles ? OVERLAYRENDERFLAG_EDITOR : 0);
	RenderMap()->RenderTeleOverlay(m_pTeleTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, OverlayRenderFlags, Color.a);
}

void CRenderLayerEntityTele::GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const
{
	*pIndex = m_pTeleTiles[y * m_pLayerTilemap->m_Width + x].m_Type;
	*pFlags = 0;
	if(CurOverlay == 1)
	{
		if(IsTeleTileNumberUsedAny(*pIndex))
			*pIndex = m_pTeleTiles[y * m_pLayerTilemap->m_Width + x].m_Number;
		else
			*pIndex = 0;
	}
}

// SPEEDUP
CRenderLayerEntitySpeedup::CRenderLayerEntitySpeedup(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerEntityBase(GroupId, LayerId, Flags, pLayerTilemap) {}

IGraphics::CTextureHandle CRenderLayerEntitySpeedup::GetTexture() const
{
	return m_pMapImages->GetSpeedupArrow();
}

int CRenderLayerEntitySpeedup::GetDataIndex() const
{
	return m_pLayerTilemap->m_Speedup;
}

void CRenderLayerEntitySpeedup::Init()
{
	InitCallback();
	UploadTileData(m_VisualTiles, 0, true);
	UploadTileData(m_VisualForce, 1, false);
	UploadTileData(m_VisualMaxSpeed, 2, false);
}

void CRenderLayerEntitySpeedup::InitTileData()
{
	m_pSpeedupTiles = GetData<CSpeedupTile>();
}

void CRenderLayerEntitySpeedup::Unload()
{
	CRenderLayerTile::Unload();
	if(m_VisualForce.has_value())
	{
		m_VisualForce->Unload();
		m_VisualForce = std::nullopt;
	}
	if(m_VisualMaxSpeed.has_value())
	{
		m_VisualMaxSpeed->Unload();
		m_VisualMaxSpeed = std::nullopt;
	}
}

void CRenderLayerEntitySpeedup::GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const
{
	*pIndex = m_pSpeedupTiles[y * m_pLayerTilemap->m_Width + x].m_Type;
	unsigned char Force = m_pSpeedupTiles[y * m_pLayerTilemap->m_Width + x].m_Force;
	unsigned char MaxSpeed = m_pSpeedupTiles[y * m_pLayerTilemap->m_Width + x].m_MaxSpeed;
	*pFlags = 0;
	*pAngleRotate = m_pSpeedupTiles[y * m_pLayerTilemap->m_Width + x].m_Angle;
	if((Force == 0 && *pIndex == TILE_SPEED_BOOST_OLD) || (Force == 0 && MaxSpeed == 0 && *pIndex == TILE_SPEED_BOOST) || !IsValidSpeedupTile(*pIndex))
		*pIndex = 0;
	else if(CurOverlay == 1)
		*pIndex = Force;
	else if(CurOverlay == 2)
		*pIndex = MaxSpeed;
}

void CRenderLayerEntitySpeedup::RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	// draw arrow -- clamp to the edge of the arrow image
	Graphics()->WrapClamp();
	UseTexture(GetTexture());
	RenderTileLayer(Color, Params);
	Graphics()->WrapNormal();

	if(Params.m_RenderText)
	{
		Graphics()->TextureSet(m_pMapImages->GetOverlayBottom());
		RenderTileLayer(Color, Params, &m_VisualForce.value());
		Graphics()->TextureSet(m_pMapImages->GetOverlayTop());
		RenderTileLayer(Color, Params, &m_VisualMaxSpeed.value());
	}
}

void CRenderLayerEntitySpeedup::RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	int OverlayRenderFlags = (Params.m_RenderText ? OVERLAYRENDERFLAG_TEXT : 0) | (Params.m_RenderInvalidTiles ? OVERLAYRENDERFLAG_EDITOR : 0);
	RenderMap()->RenderSpeedupOverlay(m_pSpeedupTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, OverlayRenderFlags, Color.a);
}

// SWITCH
CRenderLayerEntitySwitch::CRenderLayerEntitySwitch(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerEntityBase(GroupId, LayerId, Flags, pLayerTilemap) {}

IGraphics::CTextureHandle CRenderLayerEntitySwitch::GetTexture() const
{
	return m_pMapImages->GetEntities(MAP_IMAGE_ENTITY_LAYER_TYPE_SWITCH);
}

int CRenderLayerEntitySwitch::GetDataIndex() const
{
	return m_pLayerTilemap->m_Switch;
}

void CRenderLayerEntitySwitch::Init()
{
	InitCallback();
	UploadTileData(m_VisualTiles, 0, false);
	UploadTileData(m_VisualSwitchNumberTop, 1, false);
	UploadTileData(m_VisualSwitchNumberBottom, 2, false);
}

void CRenderLayerEntitySwitch::InitTileData()
{
	m_pSwitchTiles = GetData<CSwitchTile>();
}

void CRenderLayerEntitySwitch::Unload()
{
	CRenderLayerTile::Unload();
	if(m_VisualSwitchNumberTop.has_value())
	{
		m_VisualSwitchNumberTop->Unload();
		m_VisualSwitchNumberTop = std::nullopt;
	}
	if(m_VisualSwitchNumberBottom.has_value())
	{
		m_VisualSwitchNumberBottom->Unload();
		m_VisualSwitchNumberBottom = std::nullopt;
	}
}

void CRenderLayerEntitySwitch::GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const
{
	*pFlags = 0;
	*pIndex = m_pSwitchTiles[y * m_pLayerTilemap->m_Width + x].m_Type;
	if(CurOverlay == 0)
	{
		*pFlags = m_pSwitchTiles[y * m_pLayerTilemap->m_Width + x].m_Flags;
		if(*pIndex == TILE_SWITCHTIMEDOPEN)
			*pIndex = 8;
	}
	else if(CurOverlay == 1)
		*pIndex = m_pSwitchTiles[y * m_pLayerTilemap->m_Width + x].m_Number;
	else if(CurOverlay == 2)
		*pIndex = m_pSwitchTiles[y * m_pLayerTilemap->m_Width + x].m_Delay;
}

void CRenderLayerEntitySwitch::RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	RenderTileLayer(Color, Params);
	if(Params.m_RenderText)
	{
		Graphics()->TextureSet(m_pMapImages->GetOverlayTop());
		RenderTileLayer(Color, Params, &m_VisualSwitchNumberTop.value());
		Graphics()->TextureSet(m_pMapImages->GetOverlayBottom());
		RenderTileLayer(Color, Params, &m_VisualSwitchNumberBottom.value());
	}
}

void CRenderLayerEntitySwitch::RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	Graphics()->BlendNone();
	RenderMap()->RenderSwitchmap(m_pSwitchTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_OPAQUE);
	Graphics()->BlendNormal();
	RenderMap()->RenderSwitchmap(m_pSwitchTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_TRANSPARENT);
	int OverlayRenderFlags = (Params.m_RenderText ? OVERLAYRENDERFLAG_TEXT : 0) | (Params.m_RenderInvalidTiles ? OVERLAYRENDERFLAG_EDITOR : 0);
	RenderMap()->RenderSwitchOverlay(m_pSwitchTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, OverlayRenderFlags, Color.a);
}

// TUNE
CRenderLayerEntityTune::CRenderLayerEntityTune(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap) :
	CRenderLayerEntityBase(GroupId, LayerId, Flags, pLayerTilemap) {}

IGraphics::CTextureHandle CRenderLayerEntityTune::GetTexture() const
{
	return m_pMapImages->GetTuneColors();
}

void CRenderLayerEntityTune::GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const
{
	const unsigned char Number = m_pTuneTiles[y * m_pLayerTilemap->m_Width + x].m_Number;
	unsigned char Index = 0;

	if(Number != 0)
	{
		// assign color index instead of tune number for higher color distance
		Index = m_TuneColorMapper.TuneNumberToColorIndex(Number);
	}

	*pIndex = Index;
	*pFlags = 0;
}

void CRenderLayerEntityTune::Init()
{
	m_TuneColorMapper.Reset();

	// The color index is handed out in the order the tune numbers are first
	// seen. Geometry is built per chunk in the order the chunks come into view,
	// so the numbers have to be walked in map order once, or which color a tune
	// zone gets would depend on where the camera looked first.
	for(int y = 0; y < m_pLayerTilemap->m_Height; ++y)
	{
		for(int x = 0; x < m_pLayerTilemap->m_Width; ++x)
		{
			unsigned char Index = 0;
			unsigned char Flags = 0;
			int AngleRotate = -1;
			GetTileData(&Index, &Flags, &AngleRotate, static_cast<unsigned int>(x), static_cast<unsigned int>(y), 0);
		}
	}

	CRenderLayerTile::Init();
}

int CRenderLayerEntityTune::GetDataIndex() const
{
	return m_pLayerTilemap->m_Tune;
}

void CRenderLayerEntityTune::InitTileData()
{
	m_pTuneTiles = GetData<CTuneTile>();
}

void CRenderLayerEntityTune::RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params)
{
	Graphics()->BlendNone();
	RenderMap()->RenderTunemap(m_pTuneTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_OPAQUE, &m_TuneColorMapper);
	Graphics()->BlendNormal();
	RenderMap()->RenderTunemap(m_pTuneTiles, m_pLayerTilemap->m_Width, m_pLayerTilemap->m_Height, 32.0f, Color, (Params.m_RenderTileBorder ? TILERENDERFLAG_EXTEND : 0) | LAYERRENDERFLAG_TRANSPARENT, &m_TuneColorMapper);
}
