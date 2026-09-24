#include "quad_buffer_cache.h"

#include <game/map/render_interfaces.h>

#include <algorithm>
#include <limits>

namespace
{

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

} // namespace

bool CQuadBufferCache::Clear()
{
	m_vClusters.clear();
	m_LayerClip = std::nullopt;
	m_BuiltNumQuads = -1;
	m_Dirty = true;
	if(!m_BufferObject.IsValid())
		return false;
	Graphics()->DeleteBufferObject(m_BufferObject);
	m_BufferObject.Invalidate();
	return true;
}

void CQuadBufferCache::Rebuild(const CQuadSource &Source)
{
	m_Dirty = false;
	m_vClusters.clear();
	m_LayerClip = std::nullopt;
	if(m_BufferObject.IsValid())
	{
		Graphics()->DeleteBufferObject(m_BufferObject);
		m_BufferObject.Invalidate();
	}
	m_BuiltNumQuads = Source.m_NumQuads;
	m_BuiltTextured = Source.m_Textured;
	if(Source.m_NumQuads <= 0)
		return;

	std::vector<CTmpQuad> vTmpQuads;
	std::vector<CTmpQuadTextured> vTmpQuadsTextured;
	if(Source.m_Textured)
		vTmpQuadsTextured.resize(Source.m_NumQuads);
	else
		vTmpQuads.resize(Source.m_NumQuads);

	// The shader rotates a quad around its pivot and offsets it, so every
	// vertex carries the pivot and the render info carries the envelope.
	auto SetQuadRenderInfo = [&](SQuadRenderInfo &Info, int QuadId, bool InitInfo) {
		const CQuad *pQuad = &Source.m_pQuads[QuadId];

		if(InitInfo)
		{
			Info.m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
			Info.m_Offsets.x = 0;
			Info.m_Offsets.y = 0;
			Info.m_Rotation = 0;
		}

		for(int j = 0; j < 4; ++j)
		{
			int QuadIdX = j;
			if(j == 2)
				QuadIdX = 3;
			else if(j == 3)
				QuadIdX = 2;
			if(!Source.m_Textured)
			{
				CTmpQuadVertex &Vertex = vTmpQuads[QuadId].m_aVertices[j];
				Vertex.m_X = fx2f(pQuad->m_aPoints[QuadIdX].x);
				Vertex.m_Y = fx2f(pQuad->m_aPoints[QuadIdX].y);
				Vertex.m_CenterX = fx2f(pQuad->m_aPoints[4].x);
				Vertex.m_CenterY = fx2f(pQuad->m_aPoints[4].y);
				Vertex.m_R = (unsigned char)pQuad->m_aColors[QuadIdX].r;
				Vertex.m_G = (unsigned char)pQuad->m_aColors[QuadIdX].g;
				Vertex.m_B = (unsigned char)pQuad->m_aColors[QuadIdX].b;
				Vertex.m_A = (unsigned char)pQuad->m_aColors[QuadIdX].a;
			}
			else
			{
				CTmpQuadVertexTextured &Vertex = vTmpQuadsTextured[QuadId].m_aVertices[j];
				Vertex.m_X = fx2f(pQuad->m_aPoints[QuadIdX].x);
				Vertex.m_Y = fx2f(pQuad->m_aPoints[QuadIdX].y);
				Vertex.m_CenterX = fx2f(pQuad->m_aPoints[4].x);
				Vertex.m_CenterY = fx2f(pQuad->m_aPoints[4].y);
				Vertex.m_R = (unsigned char)pQuad->m_aColors[QuadIdX].r;
				Vertex.m_G = (unsigned char)pQuad->m_aColors[QuadIdX].g;
				Vertex.m_B = (unsigned char)pQuad->m_aColors[QuadIdX].b;
				Vertex.m_A = (unsigned char)pQuad->m_aColors[QuadIdX].a;
				Vertex.m_U = fx2f(pQuad->m_aTexcoords[QuadIdX].x);
				Vertex.m_V = fx2f(pQuad->m_aTexcoords[QuadIdX].y);
			}
		}
	};

	int QuadStart = 0;
	while(QuadStart < Source.m_NumQuads)
	{
		CQuadCluster Cluster;
		Cluster.m_StartIndex = QuadStart;
		Cluster.m_Grouped = true;
		Cluster.m_ColorEnv = Source.m_pQuads[QuadStart].m_ColorEnv;
		Cluster.m_ColorEnvOffset = Source.m_pQuads[QuadStart].m_ColorEnvOffset;
		Cluster.m_PosEnv = Source.m_pQuads[QuadStart].m_PosEnv;
		Cluster.m_PosEnvOffset = Source.m_pQuads[QuadStart].m_PosEnvOffset;

		int QuadOffset = 0;
		for(int ClusterId = 0; ClusterId < Source.m_NumQuads - QuadStart; ++ClusterId)
		{
			const CQuad *pQuad = &Source.m_pQuads[QuadStart + ClusterId];
			const bool IsGrouped = Cluster.m_Grouped && pQuad->m_ColorEnv == Cluster.m_ColorEnv && pQuad->m_ColorEnvOffset == Cluster.m_ColorEnvOffset && pQuad->m_PosEnv == Cluster.m_PosEnv && pQuad->m_PosEnvOffset == Cluster.m_PosEnvOffset;

			// A grouped run needs one render info however long it is, an
			// ungrouped one needs one per quad and the backend takes only so
			// many of those in a call.
			if(ClusterId >= (int)GRAPHICS_MAX_QUADS_RENDER_COUNT && !IsGrouped)
				break;
			QuadOffset++;
			Cluster.m_Grouped = IsGrouped;
		}
		Cluster.m_NumQuads = QuadOffset;

		if(Cluster.m_Grouped)
		{
			Cluster.m_vQuadRenderInfo.resize(1);
			for(int ClusterId = 0; ClusterId < Cluster.m_NumQuads; ++ClusterId)
				SetQuadRenderInfo(Cluster.m_vQuadRenderInfo[0], Cluster.m_StartIndex + ClusterId, ClusterId == 0);
		}
		else
		{
			Cluster.m_vQuadRenderInfo.resize(Cluster.m_NumQuads);
			for(int ClusterId = 0; ClusterId < Cluster.m_NumQuads; ++ClusterId)
				SetQuadRenderInfo(Cluster.m_vQuadRenderInfo[ClusterId], Cluster.m_StartIndex + ClusterId, true);
		}

		CalculateClipping(Source, Cluster);

		m_vClusters.push_back(std::move(Cluster));
		QuadStart += QuadOffset;
	}

	const size_t UploadDataSize = Source.m_Textured ? vTmpQuadsTextured.size() * sizeof(CTmpQuadTextured) : vTmpQuads.size() * sizeof(CTmpQuad);
	if(UploadDataSize == 0)
		return;
	const void *pUploadData = Source.m_Textured ? (const void *)vTmpQuadsTextured.data() : (const void *)vTmpQuads.data();
	if(!Graphics()->IndicesNumRequiredNotify(Source.m_NumQuads * 6))
		return;
	m_BufferObject = Graphics()->CreateBufferObject({static_cast<const uint8_t *>(pUploadData), UploadDataSize});
	m_Layout = Source.m_Textured ? IGraphics::EVertexLayout::QUAD_TEXTURED : IGraphics::EVertexLayout::QUAD;
}

bool CQuadBufferCache::CalculateQuadClipping(const CQuadSource &Source, const CQuadCluster &Cluster, float aOffsetMin[2], float aOffsetMax[2]) const
{
	if(!Source.m_Extrema)
		return false;

	// A cluster whose envelope has no known extrema could move anywhere, so
	// there is nothing to clip it against.
	if(Cluster.m_Grouped && !Source.m_Extrema(Cluster.m_PosEnv).m_Available)
		return false;

	for(int Channel = 0; Channel < 2; ++Channel)
	{
		aOffsetMin[Channel] = std::numeric_limits<float>::max();
		aOffsetMax[Channel] = std::numeric_limits<float>::lowest();
	}

	for(int QuadId = Cluster.m_StartIndex; QuadId < Cluster.m_StartIndex + Cluster.m_NumQuads; ++QuadId)
	{
		const CQuad *pQuad = &Source.m_pQuads[QuadId];
		const CEnvelopeExtrema::CEnvelopeExtremaItem &Extrema = Source.m_Extrema(pQuad->m_PosEnv);
		if(!Extrema.m_Available)
			return false;

		if(!Extrema.m_Rotating)
		{
			for(int Point = 0; Point < 4; ++Point)
			{
				for(int Channel = 0; Channel < 2; ++Channel)
				{
					float OffsetMinimum = fx2f(pQuad->m_aPoints[Point][Channel]);
					float OffsetMaximum = OffsetMinimum;
					if(!Cluster.m_Grouped && pQuad->m_PosEnv >= 0)
					{
						OffsetMinimum += fx2f(Extrema.m_Minima[Channel]);
						OffsetMaximum += fx2f(Extrema.m_Maxima[Channel]);
					}
					aOffsetMin[Channel] = std::min(aOffsetMin[Channel], OffsetMinimum);
					aOffsetMax[Channel] = std::max(aOffsetMax[Channel], OffsetMaximum);
				}
			}
		}
		else
		{
			// A rotating quad sweeps the circle around its pivot.
			const CPoint &CenterFX = pQuad->m_aPoints[4];
			const vec2 Center(fx2f(CenterFX.x), fx2f(CenterFX.y));
			float MaxDistance = 0.0f;
			for(int Point = 0; Point < 4; ++Point)
			{
				const CPoint &QuadPointFX = pQuad->m_aPoints[Point];
				MaxDistance = std::max(length(Center - vec2(fx2f(QuadPointFX.x), fx2f(QuadPointFX.y))), MaxDistance);
			}

			for(int Channel = 0; Channel < 2; ++Channel)
			{
				float OffsetMinimum = Center[Channel] - MaxDistance;
				float OffsetMaximum = Center[Channel] + MaxDistance;
				if(!Cluster.m_Grouped && pQuad->m_PosEnv >= 0)
				{
					OffsetMinimum += fx2f(Extrema.m_Minima[Channel]);
					OffsetMaximum += fx2f(Extrema.m_Maxima[Channel]);
				}
				aOffsetMin[Channel] = std::min(aOffsetMin[Channel], OffsetMinimum);
				aOffsetMax[Channel] = std::max(aOffsetMax[Channel], OffsetMaximum);
			}
		}
	}

	if(Cluster.m_Grouped && Cluster.m_PosEnv >= 0)
	{
		const CEnvelopeExtrema::CEnvelopeExtremaItem &Extrema = Source.m_Extrema(Cluster.m_PosEnv);
		for(int Channel = 0; Channel < 2; ++Channel)
		{
			aOffsetMin[Channel] += fx2f(Extrema.m_Minima[Channel]);
			aOffsetMax[Channel] += fx2f(Extrema.m_Maxima[Channel]);
		}
	}
	return true;
}

void CQuadBufferCache::CalculateClipping(const CQuadSource &Source, CQuadCluster &Cluster)
{
	float aOffsetMin[2];
	float aOffsetMax[2];
	if(!CalculateQuadClipping(Source, Cluster, aOffsetMin, aOffsetMax))
		return;

	Cluster.m_ClipRegion = std::make_optional<CClipRegion>(aOffsetMin[0], aOffsetMin[1], aOffsetMax[0] - aOffsetMin[0], aOffsetMax[1] - aOffsetMin[1]);
	const CClipRegion &ClipRegion = Cluster.m_ClipRegion.value();

	if(!m_LayerClip.has_value())
	{
		m_LayerClip = ClipRegion;
		return;
	}
	const float ClipRight = std::max(ClipRegion.m_X + ClipRegion.m_Width, m_LayerClip->m_X + m_LayerClip->m_Width);
	const float ClipBottom = std::max(ClipRegion.m_Y + ClipRegion.m_Height, m_LayerClip->m_Y + m_LayerClip->m_Height);
	m_LayerClip->m_X = std::min(ClipRegion.m_X, m_LayerClip->m_X);
	m_LayerClip->m_Y = std::min(ClipRegion.m_Y, m_LayerClip->m_Y);
	m_LayerClip->m_Width = ClipRight - m_LayerClip->m_X;
	m_LayerClip->m_Height = ClipBottom - m_LayerClip->m_Y;
}

bool CQuadBufferCache::IsVisible(const std::optional<CClipRegion> &ClipRegion) const
{
	if(!ClipRegion.has_value())
		return true;

	const CScreenRect ScreenRect = Graphics()->GetScreen();
	const float Left = ClipRegion->m_X;
	const float Top = ClipRegion->m_Y;
	const float Right = ClipRegion->m_X + ClipRegion->m_Width;
	const float Bottom = ClipRegion->m_Y + ClipRegion->m_Height;
	return Right >= ScreenRect.m_TopLeft.x && Left <= ScreenRect.m_BottomRight.x && Bottom >= ScreenRect.m_TopLeft.y && Top <= ScreenRect.m_BottomRight.y;
}

void CQuadBufferCache::Render(const CQuadSource &Source, const IEnvelopeEval *pEnvelopeEval, float Alpha)
{
	if(Source.m_NumQuads != m_BuiltNumQuads || Source.m_Textured != m_BuiltTextured)
		m_Dirty = true;
	if(m_Dirty)
		Rebuild(Source);
	if(!m_BufferObject.IsValid())
		return;

	for(CQuadCluster &Cluster : m_vClusters)
	{
		if(!IsVisible(Cluster.m_ClipRegion))
			continue;

		if(!Cluster.m_Grouped)
		{
			bool AnyVisible = false;
			for(int ClusterId = 0; ClusterId < Cluster.m_NumQuads; ++ClusterId)
			{
				const CQuad *pQuad = &Source.m_pQuads[Cluster.m_StartIndex + ClusterId];

				ColorRGBA Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
				if(pQuad->m_ColorEnv >= 0)
					pEnvelopeEval->EnvelopeEval(pQuad->m_ColorEnvOffset, pQuad->m_ColorEnv, Color, 4);
				Color.a = std::max(0.0f, Color.a * Alpha);

				SQuadRenderInfo &Info = Cluster.m_vQuadRenderInfo[ClusterId];
				Info.m_Color = Color;
				if(Color.a > 0.0f)
				{
					AnyVisible = true;
					ColorRGBA Position = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
					pEnvelopeEval->EnvelopeEval(pQuad->m_PosEnvOffset, pQuad->m_PosEnv, Position, 3);
					Info.m_Offsets.x = Position.r;
					Info.m_Offsets.y = Position.g;
					Info.m_Rotation = Position.b / 180.0f * pi;
				}
			}
			if(AnyVisible)
				Graphics()->RenderQuadLayer(m_BufferObject, m_Layout, Cluster.m_vQuadRenderInfo.data(), Cluster.m_NumQuads, Cluster.m_StartIndex);
		}
		else
		{
			SQuadRenderInfo &Info = Cluster.m_vQuadRenderInfo[0];

			ColorRGBA Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
			if(Cluster.m_ColorEnv >= 0)
				pEnvelopeEval->EnvelopeEval(Cluster.m_ColorEnvOffset, Cluster.m_ColorEnv, Color, 4);
			Color.a *= Alpha;
			if(Color.a <= 0.0f)
				continue;
			Info.m_Color = Color;

			if(Cluster.m_PosEnv >= 0)
			{
				ColorRGBA Position = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
				pEnvelopeEval->EnvelopeEval(Cluster.m_PosEnvOffset, Cluster.m_PosEnv, Position, 3);
				Info.m_Offsets.x = Position.r;
				Info.m_Offsets.y = Position.g;
				Info.m_Rotation = Position.b / 180.0f * pi;
			}
			Graphics()->RenderQuadLayer(m_BufferObject, m_Layout, &Info, (size_t)Cluster.m_NumQuads, Cluster.m_StartIndex, true);
		}
	}
}

void CQuadBufferCache::EachClip(const std::function<void(const CClipRegion &, int StartIndex, bool Grouped)> &Callback) const
{
	for(const CQuadCluster &Cluster : m_vClusters)
	{
		if(Cluster.m_ClipRegion.has_value() && IsVisible(Cluster.m_ClipRegion))
			Callback(Cluster.m_ClipRegion.value(), Cluster.m_StartIndex, Cluster.m_Grouped);
	}
}
