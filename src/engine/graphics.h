/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_GRAPHICS_H
#define ENGINE_GRAPHICS_H

#include "image.h"
#include "kernel.h"
#include "render_handle.h"
#include "warning.h"

#include <base/color.h>
#include <base/dbg.h>
#include <base/vmath.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct SQuadRenderInfo
{
	ColorRGBA m_Color;
	vec2 m_Offsets;
	float m_Rotation;
	// allows easier upload for uniform buffers because of the alignment requirements
	float m_Padding;
};

class CGraphicTile
{
public:
	vec2 m_TopLeft;
	vec2 m_TopRight;
	vec2 m_BottomRight;
	vec2 m_BottomLeft;
};

class CGraphicTileTextureCoords
{
public:
	ubvec4 m_TexCoordTopLeft;
	ubvec4 m_TexCoordTopRight;
	ubvec4 m_TexCoordBottomRight;
	ubvec4 m_TexCoordBottomLeft;
};

/*
	Structure: CVideoMode
*/
class CVideoMode
{
public:
	int m_CanvasWidth, m_CanvasHeight;
	int m_WindowWidth, m_WindowHeight;
	int m_RefreshRate;
};

struct SGraphicsTexCoord3D
{
	SGraphicsTexCoord3D &operator=(const vec2 &TexCoord)
	{
		u = TexCoord.u;
		v = TexCoord.v;
		return *this;
	}

	SGraphicsTexCoord3D &operator=(const vec3 &TexCoord)
	{
		u = TexCoord.u;
		v = TexCoord.v;
		w = TexCoord.w;
		return *this;
	}

	float u, v, w;
};

//use normalized color values
using SGraphicsColor = vector4_base<unsigned char>;

struct SGraphicsVertex
{
	vec2 m_Pos;
	vec2 m_Tex;
	SGraphicsColor m_Color;
};
static_assert(sizeof(SGraphicsVertex) == 20);
static_assert(offsetof(SGraphicsVertex, m_Pos) == 0);
static_assert(offsetof(SGraphicsVertex, m_Tex) == 8);
static_assert(offsetof(SGraphicsVertex, m_Color) == 16);

struct SGraphicsVertexTex3D
{
	vec2 m_Pos;
	ColorRGBA m_Color;
	SGraphicsTexCoord3D m_Tex;
};
static_assert(sizeof(SGraphicsVertexTex3D) == 36);
static_assert(offsetof(SGraphicsVertexTex3D, m_Color) == 8);
static_assert(offsetof(SGraphicsVertexTex3D, m_Tex) == 24);

struct SGraphicsVertexTex3DStream
{
	vec2 m_Pos;
	SGraphicsColor m_Color;
	SGraphicsTexCoord3D m_Tex;
};
static_assert(sizeof(SGraphicsVertexTex3DStream) == 24);
static_assert(offsetof(SGraphicsVertexTex3DStream, m_Color) == 8);
static_assert(offsetof(SGraphicsVertexTex3DStream, m_Tex) == 12);

static constexpr size_t GRAPHICS_MAX_QUADS_RENDER_COUNT = 256;
static constexpr size_t GRAPHICS_MAX_PARTICLES_RENDER_COUNT = 512;

enum EGraphicsDriverAgeType
{
	GRAPHICS_DRIVER_AGE_TYPE_LEGACY = 0,
	GRAPHICS_DRIVER_AGE_TYPE_DEFAULT,
	GRAPHICS_DRIVER_AGE_TYPE_MODERN,

	GRAPHICS_DRIVER_AGE_TYPE_COUNT,
};

enum EBackendType
{
	BACKEND_TYPE_OPENGL = 0,
	BACKEND_TYPE_OPENGL_ES,
	BACKEND_TYPE_VULKAN,
	// Accepts everything and draws nothing: the headless client, and the
	// tests that run the frontend without a device.
	BACKEND_TYPE_NULL,

	// special value to tell the backend to identify the current backend
	BACKEND_TYPE_AUTO,

	BACKEND_TYPE_COUNT,
};

struct STWGraphicGpu
{
	enum ETWGraphicsGpuType
	{
		GRAPHICS_GPU_TYPE_DISCRETE = 0,
		GRAPHICS_GPU_TYPE_INTEGRATED,
		GRAPHICS_GPU_TYPE_VIRTUAL,
		GRAPHICS_GPU_TYPE_CPU,

		// should stay at last position in this enum
		GRAPHICS_GPU_TYPE_INVALID,
	};

	struct STWGraphicGpuItem
	{
		char m_aName[256];
		ETWGraphicsGpuType m_GpuType;
	};
	std::vector<STWGraphicGpuItem> m_vGpus;
	STWGraphicGpuItem m_AutoGpu;
};

typedef STWGraphicGpu TTwGraphicsGpuList;

typedef std::function<void()> WINDOW_RESIZE_FUNC;
typedef std::function<void()> WINDOW_PROPS_CHANGED_FUNC;

struct CDataSprite;

class CScreenRect
{
public:
	CScreenRect(float Left, float Top, float Width, float Height) :
		m_TopLeft(Left, Top), m_BottomRight(Left + Width, Top + Height) {}

	CScreenRect(const vec2 &TopLeft, const vec2 &BottomRight) :
		m_TopLeft(TopLeft), m_BottomRight(BottomRight) {}

	CScreenRect Move(const vec2 &Position) const
	{
		CScreenRect Rect(*this);
		Rect.m_TopLeft += Position;
		Rect.m_BottomRight += Position;
		return Rect;
	}

	constexpr vec2 Size() const
	{
		return m_BottomRight - m_TopLeft;
	}

	constexpr float Width() const
	{
		return m_BottomRight.x - m_TopLeft.x;
	}

	constexpr float Height() const
	{
		return m_BottomRight.y - m_TopLeft.y;
	}

	constexpr bool Inside(const vec2 &Position) const
	{
		return !(!in_range(Position.x, m_TopLeft.x, m_BottomRight.x) || !in_range(Position.y, m_TopLeft.y, m_BottomRight.y));
	}

	void Expand(float Width, float Height)
	{
		m_TopLeft.x -= Width;
		m_BottomRight.x += Width;
		m_TopLeft.y -= Height;
		m_BottomRight.y += Height;
	}

	void Expand(float Size)
	{
		Expand(Size, Size);
	}

	vec2 m_TopLeft;
	vec2 m_BottomRight;
};

// What the graphics are told about the surface they draw into: by the window
// when it opens or changes size, or by the surface-less client for the
// virtual screen it keeps instead.
struct SGraphicsSurfaceInfo
{
	// Whether a frame can be presented at all. Without a surface the
	// graphics draw into a virtual screen of their own.
	bool m_Presentable = false;
	// The size of the surface in pixels ...
	int m_DrawableWidth = 0;
	int m_DrawableHeight = 0;
	// ... and the size of the window in screen coordinates, which differ on
	// a HiDPI screen.
	int m_WindowWidth = 0;
	int m_WindowHeight = 0;
	int m_RefreshRate = 0;
	// Pixels at the left and right edge that the display's cutout covers
	// (iOS). Nothing drawn there is visible, so the view excludes them.
	int m_InsetLeft = 0;
	int m_InsetRight = 0;
};

class IGraphics : public IInterface
{
	MACRO_INTERFACE("graphics")
protected:
	int m_ScreenWidth;
	int m_ScreenHeight;
	int m_RenderWidth = 0;
	int m_RenderHeight = 0;
	// The whole surface, of which the screen may only be a part: the view is
	// clamped to an aspect ratio of at most 5:4 and excludes the display cutout.
	int m_ViewportX = 0;
	int m_DrawableWidth = 0;
	int m_DrawableHeight = 0;
	int m_ScreenRefreshRate;
	float m_ScreenHiDPIScale;

public:
	// How many vertices a Begin/End pair may hold before it flushes on its own.
	static constexpr int MAX_VERTICES = 32 * 1024;

	struct CQuadItem;

protected:
	// The vertex buffer that Begin/Draw/End fill. It lives here so that the
	// calls that only write vertices - SetColor, QuadsSetSubset, QuadsDrawTL
	// and the rest between a Begin and an End - are plain member functions
	// on the interface; the backend only sees the flush, which is the one
	// virtual call.
	enum class EDrawing
	{
		NONE,
		QUADS,
		LINES,
		TRIANGLES,
	};
	SGraphicsVertex m_aVertices[MAX_VERTICES];
	SGraphicsVertexTex3DStream m_aVerticesTex3D[MAX_VERTICES];
	int m_NumVertices = 0;
	SGraphicsColor m_aColor[4];
	vec2 m_aTexture[4];
	float m_Rotation = 0.0f;
	int m_CurIndex = -1;
	EDrawing m_Drawing = EDrawing::NONE;

	virtual void FlushVertices(bool KeepVertices = false) = 0;
	virtual void FlushVerticesTex3D() = 0;

	void AddVertices(int Count);
	void AddVertices(int Count, SGraphicsVertex *pVertices);
	void AddVertices(int Count, SGraphicsVertexTex3DStream *pVertices);

	template<typename TName>
	void SetColor(TName *pVertex, int ColorIndex)
	{
		pVertex->m_Color = m_aColor[ColorIndex];
	}

	template<typename TName>
	void Rotate(const vec2 &Center, TName *pPoints, int NumPoints)
	{
		const float c = std::cos(m_Rotation);
		const float s = std::sin(m_Rotation);
		for(int i = 0; i < NumPoints; i++)
		{
			const float x = pPoints[i].m_Pos.x - Center.x;
			const float y = pPoints[i].m_Pos.y - Center.y;
			pPoints[i].m_Pos.x = x * c - y * s + Center.x;
			pPoints[i].m_Pos.y = x * s + y * c + Center.y;
		}
	}

	template<typename TName>
	void QuadsDrawTLImpl(TName *pVertices, const CQuadItem *pArray, int Num);

public:
	enum
	{
		// The texture is a grid of layers and will be sampled by layer index.
		// Whether that becomes an array texture or a volume is the renderer's
		// business; a layer is addressed the same way either way.
		TEXLOAD_LAYERED = 1 << 0,
		TEXLOAD_NO_2D_TEXTURE = 1 << 1,
	};

	struct STextureHandleTag;
	class CTextureHandle : public CGenerationHandle<STextureHandleTag>
	{
		friend class CGenerationHandlePool<CTextureHandle>;

		CTextureHandle(int Id, uint32_t Generation) :
			CGenerationHandle(Id, Generation)
		{
		}

	public:
		CTextureHandle() = default;
		bool IsNullTexture() const { return Id() == 0; }
	};
	struct SBufferHandleTag;
	using CBufferHandle = CGenerationHandle<SBufferHandleTag>;

	int ScreenWidth() const { return m_RenderWidth > 0 ? m_RenderWidth : m_ScreenWidth; }
	int ScreenHeight() const { return m_RenderHeight > 0 ? m_RenderHeight : m_ScreenHeight; }
	vec2 ScreenSize() const { return vec2(ScreenWidth(), ScreenHeight()); }
	float ScreenAspect() const { return (float)ScreenWidth() / (float)ScreenHeight(); }
	float ScreenHiDPIScale() const { return m_ScreenHiDPIScale; }
	int WindowWidth() const { return m_ScreenWidth / m_ScreenHiDPIScale; }
	int WindowHeight() const { return m_ScreenHeight / m_ScreenHiDPIScale; }

	// Size of the whole drawable area, in the same units as ScreenWidth()/ScreenHeight().
	// The rendered image is clamped to an aspect ratio of at most 5:4 and excludes the
	// area covered by the cutout of the display, so it can be smaller than that area.
	// The rest of the drawable area is not rendered to.
	vec2 DrawableSize() const { return vec2(m_DrawableWidth, m_DrawableHeight); }

	// Distance of the rendered image from the left edge of the drawable area, in the
	// same units as ScreenWidth(). The image is always aligned to the top edge.
	int ViewportX() const { return m_ViewportX; }

	virtual void WarnPngliteIncompatibleImages(bool Warn) = 0;
	virtual void UpdateViewport(int X, int Y, int W, int H, bool ByResize) = 0;

	/**
	 * Listens to a resize event of the canvas, which is usually caused by a window resize.
	 * Will only be triggered if the actual size changed.
	 */
	virtual void AddWindowResizeListener(WINDOW_RESIZE_FUNC pFunc) = 0;

	// ForceClearNow forces the backend to trigger a clear, even at performance cost, else it might be delayed by one frame
	virtual void Clear(float r, float g, float b, bool ForceClearNow = false) = 0;

	enum class ERenderPassLoadOp : uint8_t
	{
		DISCARD,
		CLEAR,
	};

	struct CRenderPassDesc
	{
		// An invalid handle selects the presentation target.
		CTextureHandle m_ColorTarget;
		ERenderPassLoadOp m_LoadOp = ERenderPassLoadOp::DISCARD;
		ColorRGBA m_ClearColor = {0.0f, 0.0f, 0.0f, 0.0f};
	};

	// Drawing into something other than the screen. Fixed function has no
	// render targets, and rather than answer a capability question everywhere,
	// the contract is carried by the handle - CreateTexture with
	// TEXTURE_USAGE_COLOR_TARGET returns an invalid one where targets are
	// unavailable, and a caller that wants a target has to look at what it
	// got. The surface-less client is the only user so far: it draws every
	// frame into a target of screen size because there is no screen.
	//
	// Everything else in this interface draws the same on every backend.

	// A presentation pass is active implicitly for backwards compatibility.
	// Beginning another pass ends the current pass first.
	virtual bool BeginRenderPass(const CRenderPassDesc &Desc) = 0;
	virtual bool EndRenderPass() = 0;
	virtual void ClipEnable(int x, int y, int w, int h) = 0;
	virtual void ClipDisable() = 0;

	virtual void MapScreen(const CScreenRect &ScreenRect) = 0;

	// helper functions
	void CalcScreenParams(float Aspect, float Zoom, float *pWidth, float *pHeight) const;
	CScreenRect MapScreenToWorld(float CenterX, float CenterY, float ParallaxX, float ParallaxY,
		float ParallaxZoom, float OffsetX, float OffsetY, float Aspect, float Zoom) const;
	void MapScreenToInterface(float CenterX, float CenterY, float Zoom = 1.0f);
	void MapScreenToSize(float Width, float Height);

	virtual CScreenRect GetScreen() const = 0;

	// TODO: These should perhaps not be virtuals
	virtual void BlendNone() = 0;
	virtual void BlendNormal() = 0;
	virtual void BlendAdditive() = 0;
	virtual void WrapNormal() = 0;
	virtual void WrapClamp() = 0;

	virtual uint64_t TextureMemoryUsage() const = 0;
	virtual uint64_t BufferMemoryUsage() const = 0;
	virtual uint64_t StreamedMemoryUsage() const = 0;
	virtual uint64_t StagingMemoryUsage() const = 0;

	struct SFrameMailboxStats
	{
		uint64_t m_Produced = 0;
		uint64_t m_Rendered = 0;
		uint64_t m_Dropped = 0;
	};
	virtual SFrameMailboxStats FrameMailboxStats() const = 0;

	class CFrameRenderStats
	{
	public:
		uint64_t m_Commands = 0;
		uint64_t m_ResourceCommands = 0;
		uint64_t m_DrawCommands = 0;
		uint64_t m_DrawCalls = 0;
		uint64_t m_Triangles = 0;
		uint64_t m_Instances = 0;
		uint64_t m_RenderPasses = 0;
		uint64_t m_BufferCreates = 0;
		uint64_t m_BufferRecreates = 0;
		uint64_t m_BufferUpdates = 0;
		uint64_t m_TextureCreates = 0;
		uint64_t m_TextureUpdates = 0;
		uint64_t m_UploadBytes = 0;
		uint64_t m_StreamedBytes = 0;
		uint64_t m_GpuTimeNanoseconds = 0;
		uint64_t m_GpuSample = 0;
		bool m_GpuTimingSupported = false;

		CFrameRenderStats &operator+=(const CFrameRenderStats &Other)
		{
			m_Commands += Other.m_Commands;
			m_ResourceCommands += Other.m_ResourceCommands;
			m_DrawCommands += Other.m_DrawCommands;
			m_DrawCalls += Other.m_DrawCalls;
			m_Triangles += Other.m_Triangles;
			m_Instances += Other.m_Instances;
			m_RenderPasses += Other.m_RenderPasses;
			m_BufferCreates += Other.m_BufferCreates;
			m_BufferRecreates += Other.m_BufferRecreates;
			m_BufferUpdates += Other.m_BufferUpdates;
			m_TextureCreates += Other.m_TextureCreates;
			m_TextureUpdates += Other.m_TextureUpdates;
			m_UploadBytes += Other.m_UploadBytes;
			m_StreamedBytes += Other.m_StreamedBytes;
			return *this;
		}
	};
	virtual CFrameRenderStats FrameRenderStats() const = 0;
	virtual void SetRenderStatsEnabled(bool Enabled) = 0;

	virtual const TTwGraphicsGpuList &GetGpus() const = 0;

	virtual bool LoadPng(CImageInfo &Image, const char *pFilename, int StorageType) = 0;
	virtual bool LoadPng(CImageInfo &Image, const uint8_t *pData, size_t DataSize, const char *pContextName) = 0;

	virtual bool CheckImageDivisibility(const char *pContextName, CImageInfo &Image, int DivX, int DivY, bool AllowResize) = 0;
	virtual bool IsImageFormatRgba(const char *pContextName, const CImageInfo &Image) = 0;

	virtual void UnloadTexture(CTextureHandle *pIndex) = 0;
	virtual CTextureHandle LoadTextureRaw(const CImageInfo &Image, int Flags, const char *pTexName = nullptr) = 0;
	// Image data is consumed only when a valid texture handle is returned.
	virtual CTextureHandle LoadTextureRawMove(CImageInfo &Image, int Flags, const char *pTexName = nullptr) = 0;
	virtual CTextureHandle LoadTexture(const char *pFilename, int StorageType, int Flags = 0) = 0;
	virtual void TextureSet(CTextureHandle Texture) = 0;
	void TextureClear() { TextureSet(CTextureHandle()); }

	virtual CTextureHandle LoadSpriteTexture(const CImageInfo &FromImageInfo, const std::optional<CImageInfo> &FallbackImageInfo, const struct CDataSprite *pSprite) = 0;

	virtual bool IsImageSubFullyTransparent(const CImageInfo &FromImageInfo, int x, int y, int w, int h) = 0;
	virtual bool IsSpriteTextureFullyTransparent(const CImageInfo &FromImageInfo, const struct CDataSprite *pSprite) = 0;

	// specific render functions
	// Every buffer the client draws from has one of these shapes. Naming them
	// means a draw says which one it is instead of describing it again, and a
	// backend can decide what to do about it once instead of rediscovering it
	// per buffer.
	enum class EVertexLayout : uint8_t
	{
		// vec2 position - a tile layer that takes its colour from the draw
		TILE,
		// vec2 position, ubvec4 tile index read as integers
		TILE_TEXTURED,
		// vec4 position, ubvec4 colour
		QUAD,
		// vec4 position, ubvec4 colour, vec2 texture coordinate
		QUAD_TEXTURED,
		// vec2 position, vec2 texture coordinate, ubvec4 colour - what text
		// and the quad containers both build
		POSITION_TEXCOORD_COLOR,
		COUNT,
	};

	virtual void RenderTileLayer(CBufferHandle VertexBuffer, EVertexLayout Layout, const ColorRGBA &Color, const uint32_t *pFirstIndices, const uint32_t *pIndexCounts, size_t RangeCount) = 0;
	virtual void RenderBorderTiles(CBufferHandle VertexBuffer, EVertexLayout Layout, const ColorRGBA &Color, uint32_t FirstIndex, const vec2 &Offset, const vec2 &Scale, uint32_t DrawNum) = 0;
	virtual void RenderQuadLayer(CBufferHandle VertexBuffer, EVertexLayout Layout, SQuadRenderInfo *pQuadInfo, size_t QuadNum, int QuadOffset, bool Grouped = false) = 0;
	virtual void RenderText(CBufferHandle VertexBuffer, int TextQuadNum, int TextureSize, CTextureHandle Texture, const ColorRGBA &TextColor, const ColorRGBA &TextOutlineColor) = 0;

	enum class EIndexType : uint8_t
	{
		UINT16,
		UINT32,
	};

	// opengl 3.3 functions

	enum class EBufferLifetime : uint8_t
	{
		PERSISTENT,
		FRAME,
	};

	enum class EBufferUsage : uint8_t
	{
		VERTEX,
		INDEX,
	};

	struct CBufferDesc
	{
		size_t m_Size = 0;
		EBufferLifetime m_Lifetime = EBufferLifetime::PERSISTENT;
		EBufferUsage m_Usage = EBufferUsage::VERTEX;
	};

	enum class ETextureFormat : uint8_t
	{
		RGBA8_UNORM,
		// Rejected by CTextureDesc::IsValid: nothing creates a single channel
		// texture since the glyph atlas became RG8, and the four backends never
		// agreed on what one channel means. A backend may sample it as coverage
		// (1,1,1,r), Vulkan and modern OpenGL as red (r,0,0,1), legacy OpenGL as
		// alpha (0,0,0,r). Whoever needs it again decides which of the three it
		// is and makes all four agree first.
		R8_UNORM,
		// Two single channel images in one, which is what a glyph and its
		// outline are: the same coordinates, always drawn together.
		RG8_UNORM,
	};

	// What one pixel of a format costs. Every place that copies texture data
	// has to agree on this, so there is one of it.
	static constexpr size_t PixelSize(ETextureFormat Format)
	{
		switch(Format)
		{
		case ETextureFormat::RGBA8_UNORM: return 4;
		case ETextureFormat::RG8_UNORM: return 2;
		case ETextureFormat::R8_UNORM: return 1;
		}
		return 0;
	}

	enum class ETextureMipmaps : uint8_t
	{
		NONE,
		GENERATE,
	};

	// A layered texture is derived by splitting the uploaded 2D image into a grid.
	// Whether the layers end up as an array or as a volume is the backend's own
	// decision - the caller cannot make it, and every backend that has both picks
	// the array.
	enum class ETextureLayering : uint8_t
	{
		NONE,
		LAYERED,
	};
	static constexpr size_t MAX_TEXTURE_LAYERS = 256;

	enum ETextureUsage : uint8_t
	{
		TEXTURE_USAGE_SAMPLED = 1 << 0,
		TEXTURE_USAGE_COLOR_TARGET = 1 << 1,
		TEXTURE_USAGE_COPY_SOURCE = 1 << 2,
	};

	struct CTextureDesc
	{
		size_t m_Width = 0;
		size_t m_Height = 0;
		ETextureFormat m_Format = ETextureFormat::RGBA8_UNORM;
		ETextureMipmaps m_Mipmaps = ETextureMipmaps::GENERATE;
		ETextureLayering m_Layering = ETextureLayering::NONE;
		int m_LayerColumns = 1;
		int m_LayerRows = 1;
		uint8_t m_Usage = TEXTURE_USAGE_SAMPLED;
		bool m_Create2D = true;

		bool HasUsage(ETextureUsage Usage) const { return (m_Usage & Usage) != 0; }
		size_t LayerCount() const { return static_cast<size_t>(m_LayerColumns) * m_LayerRows; }
		bool IsValid() const
		{
			constexpr uint8_t AllUsages = TEXTURE_USAGE_SAMPLED | TEXTURE_USAGE_COLOR_TARGET | TEXTURE_USAGE_COPY_SOURCE;
			if(m_Width == 0 || m_Height == 0 || m_Usage == 0 || (m_Usage & ~AllUsages) != 0)
				return false;
			if(m_Format == ETextureFormat::R8_UNORM)
				return false;
			if(m_LayerColumns <= 0 || m_LayerRows <= 0 || m_LayerColumns > std::numeric_limits<int>::max() / m_LayerRows)
				return false;
			if(LayerCount() > MAX_TEXTURE_LAYERS)
				return false;
			if(m_Layering == ETextureLayering::NONE && (m_LayerColumns != 1 || m_LayerRows != 1))
				return false;
			if(HasUsage(TEXTURE_USAGE_COLOR_TARGET) && (m_Format != ETextureFormat::RGBA8_UNORM || !HasUsage(TEXTURE_USAGE_SAMPLED) || !m_Create2D || m_Layering != ETextureLayering::NONE || m_Mipmaps != ETextureMipmaps::NONE))
				return false;
			if(HasUsage(TEXTURE_USAGE_COPY_SOURCE) && (!m_Create2D || m_Layering != ETextureLayering::NONE))
				return false;
			return m_Create2D || m_Layering != ETextureLayering::NONE;
		}
	};

	// Initial data is copied before this call returns. A null pointer creates an
	// uninitialized color target and is rejected for ordinary sampled textures.
	virtual CTextureHandle CreateTexture(const CTextureDesc &Desc, const void *pInitialData = nullptr) = 0;
	class ITextureReadback
	{
	public:
		virtual ~ITextureReadback() = default;
		[[nodiscard]] virtual bool IsReady() const = 0;
		// Waits for completion and moves the image on success. May only be called once.
		virtual bool Wait(CImageInfo &Image) = 0;
	};
	// Presents the current frame and returns its queued top-left RGBA readback.
	// Recycled is an image the caller is done with. When it already has the
	// size and format the readback produces, it is filled instead of a fresh
	// allocation; anything else about it is ignored.
	[[nodiscard]] virtual std::unique_ptr<ITextureReadback> PresentAndReadbackAsync(CImageInfo &&Recycled = CImageInfo()) = 0;

	struct CTextureRegion
	{
		size_t m_X = 0;
		size_t m_Y = 0;
		size_t m_Width = 0;
		size_t m_Height = 0;
	};
	// Copies one tightly packed, non-empty region of an unmipmapped 2D texture
	// created with CreateTexture before this call returns.
	virtual bool UpdateTexture(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, const void *pData) = 0;

	// Only what the layout table actually uses. The wider set that used to be
	// here had no producer, so its conversion branches in the OpenGL backends
	// were never taken and never tested.
	enum class EVertexAttributeType : uint8_t
	{
		FLOAT32,
		UINT8,
	};

	enum class EVertexAttributeMode : uint8_t
	{
		FLOAT,
		INTEGER,
	};

	struct CVertexAttributeDesc
	{
		uint32_t m_ComponentCount = 0;
		EVertexAttributeType m_Type = EVertexAttributeType::FLOAT32;
		bool m_Normalized = false;
		size_t m_Offset = 0;
		EVertexAttributeMode m_Mode = EVertexAttributeMode::FLOAT;
	};

	struct SVertexLayoutDesc
	{
		size_t m_Stride = 0;
		uint32_t m_AttributeCount = 0;
		std::array<CVertexAttributeDesc, 3> m_aAttributes = {};
	};

	// Defined here rather than in the backend so pipelines, the drawing
	// interface and the tests all read the same table.
	static const SVertexLayoutDesc &VertexLayout(EVertexLayout Layout)
	{
		using EType = EVertexAttributeType;
		using EMode = EVertexAttributeMode;
		static const std::array<SVertexLayoutDesc, (size_t)EVertexLayout::COUNT> s_aLayouts = {{
			// TILE
			{sizeof(float) * 2, 1, {{{2, EType::FLOAT32, false, 0, EMode::FLOAT}}}},
			// TILE_TEXTURED
			{sizeof(float) * 2 + 4, 2, {{{2, EType::FLOAT32, false, 0, EMode::FLOAT}, {4, EType::UINT8, false, sizeof(float) * 2, EMode::INTEGER}}}},
			// QUAD
			{sizeof(float) * 4 + 4, 2, {{{4, EType::FLOAT32, false, 0, EMode::FLOAT}, {4, EType::UINT8, true, sizeof(float) * 4, EMode::FLOAT}}}},
			// QUAD_TEXTURED
			{sizeof(float) * 6 + 4, 3, {{{4, EType::FLOAT32, false, 0, EMode::FLOAT}, {4, EType::UINT8, true, sizeof(float) * 4, EMode::FLOAT}, {2, EType::FLOAT32, false, sizeof(float) * 4 + 4, EMode::FLOAT}}}},
			// POSITION_TEXCOORD_COLOR
			{sizeof(float) * 4 + 4, 3, {{{2, EType::FLOAT32, false, 0, EMode::FLOAT}, {2, EType::FLOAT32, false, sizeof(float) * 2, EMode::FLOAT}, {4, EType::UINT8, true, sizeof(float) * 4, EMode::FLOAT}}}},
		}};
		dbg_assert(Layout < EVertexLayout::COUNT, "Unknown vertex layout");
		return s_aLayouts[(size_t)Layout];
	}

	// Copies what it is given. Create returns an invalid handle on failure.
	virtual CBufferHandle CreateBufferObject(std::span<const uint8_t> Data, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) = 0;
	virtual bool RecreateBufferObject(CBufferHandle Buffer, std::span<const uint8_t> Data, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) = 0;
	// Takes an allocation made with malloc() and frees it once the command is
	// consumed, so on success the caller must not touch it afterwards. On
	// failure - an invalid handle, or false - the allocation is still the
	// caller's, and the caller frees it.
	virtual CBufferHandle CreateBufferObjectMoved(void *pData, size_t Size, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) = 0;
	virtual bool RecreateBufferObjectMoved(CBufferHandle Buffer, void *pData, size_t Size, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) = 0;
	// Failed destroys leave the passed handle valid so they can be retried.
	virtual void DeleteBufferObject(CBufferHandle &Buffer) = 0;

	[[nodiscard]] virtual bool IndicesNumRequiredNotify(unsigned int RequiredIndicesCount) = 0;

	// returns true if the driver age type is supported, passing BACKEND_TYPE_AUTO for BackendType will query the values for the currently used backend
	virtual bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) = 0;
	virtual bool IsConfigModernAPI() = 0;

	virtual const char *GetVendorString() = 0;
	virtual const char *GetVersionString() = 0;
	virtual const char *GetRendererString() = 0;
	virtual const char *GetFatalError() const = 0;

	class CLineItem
	{
	public:
		float m_X0, m_Y0, m_X1, m_Y1;
		CLineItem() = default;
		CLineItem(float x0, float y0, float x1, float y1) :
			m_X0(x0), m_Y0(y0), m_X1(x1), m_Y1(y1) {}
		CLineItem(vec2 From, vec2 To)
		{
			m_X0 = From.x;
			m_Y0 = From.y;
			m_X1 = To.x;
			m_Y1 = To.y;
		}
	};
	void LinesBegin();
	void LinesEnd();
	void LinesDraw(const CLineItem *pArray, size_t Num);

	class CLineItemBatch
	{
	public:
		IGraphics::CLineItem m_aItems[256];
		size_t m_NumItems = 0;
	};
	void LinesBatchBegin(CLineItemBatch *pBatch);
	void LinesBatchEnd(CLineItemBatch *pBatch);
	void LinesBatchDraw(CLineItemBatch *pBatch, const CLineItem *pArray, size_t Num);

	void QuadsBegin();
	void QuadsEnd();
	void QuadsTex3DBegin();
	void QuadsTex3DEnd();
	void TrianglesBegin();
	void TrianglesEnd();
	void QuadsEndKeepVertices();
	void QuadsDrawCurrentVertices(bool KeepVertices = true);
	void QuadsSetRotation(float Angle);
	void QuadsSetSubset(float TopLeftU, float TopLeftV, float BottomRightU, float BottomRightV);
	void QuadsSetSubsetFree(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int Index = -1);

	struct CFreeformItem
	{
		float m_X0, m_Y0, m_X1, m_Y1, m_X2, m_Y2, m_X3, m_Y3;
		CFreeformItem() = default;
		CFreeformItem(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) :
			m_X0(x0), m_Y0(y0), m_X1(x1), m_Y1(y1), m_X2(x2), m_Y2(y2), m_X3(x3), m_Y3(y3) {}
		CFreeformItem(vec2 Point1, vec2 Point2, vec2 Point3, vec2 Point4) :
			m_X0(Point1.x), m_Y0(Point1.y), m_X1(Point2.x), m_Y1(Point2.y), m_X2(Point3.x), m_Y2(Point3.y), m_X3(Point4.x), m_Y3(Point4.y) {}
	};

	struct CQuadItem
	{
		float m_X, m_Y, m_Width, m_Height;
		CQuadItem() = default;
		CQuadItem(float x, float y, float w, float h) :
			m_X(x), m_Y(y), m_Width(w), m_Height(h) {}
		CQuadItem(vec2 Position, vec2 Size) :
			m_X(Position.x), m_Y(Position.y), m_Width(Size.x), m_Height(Size.y) {}
	};
	void QuadsDraw(CQuadItem *pArray, int Num);
	void QuadsDrawTL(const CQuadItem *pArray, int Num);

	virtual void QuadsTex3DDrawTL(const CQuadItem *pArray, int Num) = 0;

	virtual int CreateQuadContainer(bool AutomaticUpload = true) = 0;
	virtual void QuadContainerChangeAutomaticUpload(int ContainerIndex, bool AutomaticUpload) = 0;
	virtual void QuadContainerUpload(int ContainerIndex) = 0;
	virtual int QuadContainerAddQuads(int ContainerIndex, CQuadItem *pArray, int Num) = 0;
	virtual int QuadContainerAddQuads(int ContainerIndex, CFreeformItem *pArray, int Num) = 0;
	virtual void QuadContainerReset(int ContainerIndex) = 0;
	virtual void DeleteQuadContainer(int &ContainerIndex) = 0;
	virtual void RenderQuadContainer(int ContainerIndex, int QuadDrawNum) = 0;
	virtual void RenderQuadContainer(int ContainerIndex, int QuadOffset, int QuadDrawNum, bool ChangeWrapMode = true) = 0;
	virtual void RenderQuadContainerEx(int ContainerIndex, int QuadOffset, int QuadDrawNum, float X, float Y, float ScaleX = 1.f, float ScaleY = 1.f) = 0;
	virtual void RenderQuadContainerAsSprite(int ContainerIndex, int QuadOffset, float X, float Y, float ScaleX = 1.f, float ScaleY = 1.f) = 0;

	struct SRenderSpriteInfo
	{
		vec2 m_Pos;
		float m_Scale;
		float m_Rotation;
	};

	virtual void RenderQuadContainerAsSpriteMultiple(int ContainerIndex, int QuadOffset, int DrawCount, SRenderSpriteInfo *pRenderInfo) = 0;

	void QuadsDrawFreeform(const CFreeformItem *pArray, int Num);
	void QuadsText(float x, float y, float Size, const char *pText);

	// Sprite flags for the game's sprite helpers (CRenderTools).
	enum
	{
		SPRITE_FLAG_FLIP_Y = 1,
		SPRITE_FLAG_FLIP_X = 2,
	};

	enum
	{
		CORNER_NONE = 0,
		CORNER_TL = 1,
		CORNER_TR = 2,
		CORNER_BL = 4,
		CORNER_BR = 8,

		CORNER_T = CORNER_TL | CORNER_TR,
		CORNER_B = CORNER_BL | CORNER_BR,
		CORNER_R = CORNER_TR | CORNER_BR,
		CORNER_L = CORNER_TL | CORNER_BL,

		CORNER_ALL = CORNER_T | CORNER_B,
	};
	/**
	 * @deprecated Use @link SetColor(ColorRGBA) @endlink instead of this function (avoid primitive obsession code smell).
	 */
	void SetColor(float r, float g, float b, float a);
	void SetColor(ColorRGBA Color);
	void SetColor2(ColorRGBA First, ColorRGBA Second);
	void SetColor4(ColorRGBA TopLeft, ColorRGBA TopRight, ColorRGBA BottomLeft, ColorRGBA BottomRight);
	void ChangeColorOfQuadVertices(size_t QuadOffset, unsigned char r, unsigned char g, unsigned char b, unsigned char a);

	/**
	 * Reads the color at the specified position from the backbuffer once,
	 * after the next swap operation.
	 *
	 * @param Position The pixel position to read.
	 * @param pColor Pointer that will receive the read pixel color.
	 * The pointer must be valid until the next swap operation.
	 */
	virtual void ReadPixel(ivec2 Position, ColorRGBA *pColor) = 0;
	virtual void TakeScreenshot(const char *pFilename) = 0;
	virtual void TakeCustomScreenshot(const char *pFilename) = 0;
	virtual void Swap() = 0;

	// synchronization
	virtual void InsertSignal(class CSemaphore *pSemaphore) = 0;
	virtual bool IsIdle() const = 0;
	virtual void WaitForIdle() = 0;

	virtual std::optional<SWarning> CurrentWarning() = 0;

	/**
	 * Type of a message box popup.
	 *
	 * @see CMessageBox
	 */
	enum class EMessageBoxType
	{
		ERROR,
		WARNING,
		INFO,
	};
	/**
	 * Description of a message box popup button.
	 *
	 * @see CMessageBox
	 */
	class CMessageBoxButton
	{
	public:
		/**
		 * The label of this button.
		 *
		 * @remark This needs to be short because some systems do not increase the button sizes.
		 */
		const char *m_pLabel = nullptr;
		/**
		 * Whether the enter key activates this button.
		 */
		bool m_Confirm = false;
		/**
		 * Whether the escape key activates this button.
		 *
		 * @remark Closing the popup with the window manager will also cause this button to be activated.
		 */
		bool m_Cancel = false;
	};
	/**
	 * Description of a message box popup.
	 *
	 * @see ShowMessageBox
	 */
	class CMessageBox
	{
	public:
		/**
		 * Title of the message box.
		 */
		const char *m_pTitle = nullptr;
		/**
		 * Main message of the message box.
		 */
		const char *m_pMessage = nullptr;
		/**
		 * Type of the message box.
		 */
		EMessageBoxType m_Type = EMessageBoxType::ERROR;
		/**
		 * Buttons shown in the message box. At least one button is required.
		 * The buttons are laid out from left to right.
		 */
		std::vector<CMessageBoxButton> m_vButtons = {{.m_pLabel = "OK", .m_Confirm = true, .m_Cancel = true}};
	};

	virtual bool IsBackendInitialized() = 0;
};

class IGraphicsBackend;

// What the client and the window need from the graphics beyond drawing. The
// window opens the surface and the backend that draws into it; from then on
// it tells the graphics what happened to the surface, and asks them for the
// two presentation settings that are carried out on the render thread.
class IEngineGraphics : public IGraphics
{
	MACRO_INTERFACE("enginegraphics")
public:
	/**
	 * Takes over the initialized backend and makes the graphics ready to
	 * draw. The backend is owned from here on and shut down with the
	 * graphics.
	 */
	virtual int Init(IGraphicsBackend *pBackend, const SGraphicsSurfaceInfo &Surface) = 0;
	void Shutdown() override = 0;

	// The surface changed size. Returns whether the canvas size changed,
	// which is when the resize listeners have run.
	virtual bool Resized(const SGraphicsSurfaceInfo &Surface) = 0;
	// The surface went away and came back; the renderer rebuilds what it
	// had on it. Both block until the render thread is done with it.
	virtual void PresentationSurfaceLost() = 0;
	virtual void PresentationSurfaceRestored() = 0;

	virtual bool SetVSync(bool State) = 0;
	virtual bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) = 0;

	// Lets go of everything the renderer holds on the surface, for a window
	// that is about to be destroyed under a message box.
	virtual void ReleaseSurfaceForMessageBox() = 0;

	// A warning for the user, shown by the menus. The window adds what a
	// failed start had to say.
	virtual void AddWarning(const SWarning &Warning) = 0;
};

extern IEngineGraphics *CreateEngineGraphicsThreaded();

template<typename TName>
void IGraphics::QuadsDrawTLImpl(TName *pVertices, const CQuadItem *pArray, int Num)
{
	dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsDrawTL without begin");

	for(int i = 0; i < Num; ++i)
	{
		pVertices[m_NumVertices + 4 * i].m_Pos.x = pArray[i].m_X;
		pVertices[m_NumVertices + 4 * i].m_Pos.y = pArray[i].m_Y;
		pVertices[m_NumVertices + 4 * i].m_Tex = m_aTexture[0];
		SetColor(&pVertices[m_NumVertices + 4 * i], 0);

		pVertices[m_NumVertices + 4 * i + 1].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
		pVertices[m_NumVertices + 4 * i + 1].m_Pos.y = pArray[i].m_Y;
		pVertices[m_NumVertices + 4 * i + 1].m_Tex = m_aTexture[1];
		SetColor(&pVertices[m_NumVertices + 4 * i + 1], 1);

		pVertices[m_NumVertices + 4 * i + 2].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
		pVertices[m_NumVertices + 4 * i + 2].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
		pVertices[m_NumVertices + 4 * i + 2].m_Tex = m_aTexture[2];
		SetColor(&pVertices[m_NumVertices + 4 * i + 2], 2);

		pVertices[m_NumVertices + 4 * i + 3].m_Pos.x = pArray[i].m_X;
		pVertices[m_NumVertices + 4 * i + 3].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
		pVertices[m_NumVertices + 4 * i + 3].m_Tex = m_aTexture[3];
		SetColor(&pVertices[m_NumVertices + 4 * i + 3], 3);

		if(m_Rotation != 0)
		{
			const vec2 Center(pArray[i].m_X + pArray[i].m_Width / 2, pArray[i].m_Y + pArray[i].m_Height / 2);
			Rotate(Center, &pVertices[m_NumVertices + 4 * i], 4);
		}
	}

	AddVertices(4 * Num, pVertices);
}

#endif
