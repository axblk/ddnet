/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_GRAPHICS_H
#define ENGINE_GRAPHICS_H

#include "image.h"
#include "kernel.h"
#include "render_handle.h"
#include "warning.h"

#include <base/color.h>
#include <base/vmath.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

struct SBufferContainerInfo;

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
	BACKEND_TYPE_WEBGPU,

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

class IGraphics : public IInterface
{
	MACRO_INTERFACE("graphics")
protected:
	int m_ScreenWidth;
	int m_ScreenHeight;
	int m_ScreenRefreshRate;
	float m_ScreenHiDPIScale;
	ivec2 m_DesktopSize;

public:
	enum
	{
		TEXLOAD_TO_3D_TEXTURE = 1 << 0,
		TEXLOAD_TO_2D_ARRAY_TEXTURE = 1 << 1,
		TEXLOAD_NO_2D_TEXTURE = 1 << 2,
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
	struct SBufferContainerHandleTag;
	using CBufferContainerHandle = CGenerationHandle<SBufferContainerHandleTag>;

	int ScreenWidth() const { return m_ScreenWidth; }
	int ScreenHeight() const { return m_ScreenHeight; }
	vec2 ScreenSize() const { return vec2(m_ScreenWidth, m_ScreenHeight); }
	float ScreenAspect() const { return (float)ScreenWidth() / (float)ScreenHeight(); }
	float ScreenHiDPIScale() const { return m_ScreenHiDPIScale; }
	int WindowWidth() const { return m_ScreenWidth / m_ScreenHiDPIScale; }
	int WindowHeight() const { return m_ScreenHeight / m_ScreenHiDPIScale; }

	virtual void WarnPngliteIncompatibleImages(bool Warn) = 0;
	virtual void SetWindowParams(int FullscreenMode, bool IsBorderless) = 0;
	virtual bool SetWindowScreen(int Index, bool MoveToCenter) = 0;
	virtual bool SwitchWindowScreen(int Index, bool MoveToCenter) = 0;
	virtual bool SetVSync(bool State) = 0;
	virtual bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) = 0;
	virtual int GetWindowScreen() = 0;
	virtual void Move(int x, int y) = 0;
	virtual bool Resize(int w, int h, int RefreshRate) = 0;
	virtual void ResizeToScreen() = 0;
	virtual void GotResized(int w, int h, int RefreshRate) = 0;
	virtual void UpdateViewport(int X, int Y, int W, int H, bool ByResize) = 0;
	virtual bool IsScreenKeyboardShown() = 0;

	/**
	 * Listens to a resize event of the canvas, which is usually caused by a window resize.
	 * Will only be triggered if the actual size changed.
	 */
	virtual void AddWindowResizeListener(WINDOW_RESIZE_FUNC pFunc) = 0;
	/**
	 * Listens to various window property changes, such as minimize, maximize, move, fullscreen mode
	 */
	virtual void AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc) = 0;

	virtual void WindowDestroyNtf(uint32_t WindowId) = 0;
	virtual void WindowCreateNtf(uint32_t WindowId) = 0;

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

	// A presentation pass is active implicitly for backwards compatibility.
	// Beginning another pass ends the current pass first.
	virtual bool BeginRenderPass(const CRenderPassDesc &Desc) = 0;
	virtual bool EndRenderPass() = 0;
	// Orders all draws recorded so far before later draws in the same pass.
	virtual bool FlushRenderPass() = 0;
	// Draws Source over the active render pass using the existing transient
	// primitive path. By default the complete target is covered; optionally the
	// current pixel clip limits the result. This also provides backend-neutral scaling.
	virtual bool BlitTexture(CTextureHandle Source, bool UseCurrentClip = false) = 0;
	enum class EBlurDirection : uint8_t
	{
		HORIZONTAL,
		VERTICAL,
	};
	// Applies one fixed separable blur pass over the complete active render pass.
	virtual bool BlurTexture(CTextureHandle Source, EBlurDirection Direction) = 0;

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

	// Moved upload pointers are consumed only when the command is accepted.
	virtual bool LoadTextTextures(size_t Width, size_t Height, CTextureHandle &TextTexture, CTextureHandle &TextOutlineTexture, uint8_t *pTextData, uint8_t *pTextOutlineData) = 0;
	virtual bool UnloadTextTextures(CTextureHandle &TextTexture, CTextureHandle &TextOutlineTexture) = 0;
	// If IsMovedPointer is true, pData remains owned by the caller on failure.
	virtual bool UpdateTextTexture(CTextureHandle TextureId, int x, int y, size_t Width, size_t Height, uint8_t *pData, bool IsMovedPointer) = 0;

	virtual CTextureHandle LoadSpriteTexture(const CImageInfo &FromImageInfo, const std::optional<CImageInfo> &FallbackImageInfo, const struct CDataSprite *pSprite) = 0;

	virtual bool IsImageSubFullyTransparent(const CImageInfo &FromImageInfo, int x, int y, int w, int h) = 0;
	virtual bool IsSpriteTextureFullyTransparent(const CImageInfo &FromImageInfo, const struct CDataSprite *pSprite) = 0;

	virtual void FlushVertices(bool KeepVertices = false) = 0;
	virtual void FlushVerticesTex3D() = 0;

	// specific render functions
	virtual void RenderTileLayer(CBufferContainerHandle BufferContainer, const ColorRGBA &Color, char **pOffsets, unsigned int *pIndicedVertexDrawNum, size_t NumIndicesOffset) = 0;
	virtual void RenderBorderTiles(CBufferContainerHandle BufferContainer, const ColorRGBA &Color, char *pIndexBufferOffset, const vec2 &Offset, const vec2 &Scale, uint32_t DrawNum) = 0;
	virtual void RenderQuadLayer(CBufferContainerHandle BufferContainer, SQuadRenderInfo *pQuadInfo, size_t QuadNum, int QuadOffset, bool Grouped = false) = 0;
	virtual void RenderText(CBufferContainerHandle BufferContainer, int TextQuadNum, int TextureSize, CTextureHandle TextTexture, CTextureHandle TextOutlineTexture, const ColorRGBA &TextColor, const ColorRGBA &TextOutlineColor) = 0;

	enum class EIndexType : uint8_t
	{
		UINT16,
		UINT32,
	};

	struct CTransientIndexedDrawRange
	{
		CTextureHandle m_Texture;
		// Top-left based framebuffer pixels. The toolkit adapter applies its
		// display offset and framebuffer scale before submitting the range.
		int m_ClipX = 0;
		int m_ClipY = 0;
		int m_ClipW = 0;
		int m_ClipH = 0;
		uint32_t m_FirstIndex = 0;
		uint32_t m_IndexCount = 0;
		uint32_t m_VertexOffset = 0;
	};

	// Copies one toolkit-neutral draw list into frame-owned storage. Each range
	// uses alpha blending, clamp addressing and its own texture and pixel clip.
	[[nodiscard]] virtual bool RenderTransientIndexed(const SGraphicsVertex *pVertices, uint32_t VertexCount, const void *pIndices, uint32_t IndexCount, EIndexType IndexType, const CTransientIndexedDrawRange *pRanges, uint32_t RangeCount) = 0;

	// opengl 3.3 functions

	enum EBufferObjectCreateFlags
	{
		// tell the backend that the buffer only needs to be valid for the span of one frame. Buffer size is bounded by the largest streamed vertex format times MAX_VERTICES
		BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT = 1 << 0,
	};

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
		R8_UNORM,
	};

	enum class ETextureMipmaps : uint8_t
	{
		NONE,
		GENERATE,
	};

	// A layered texture is derived by splitting the uploaded 2D image into a grid.
	enum class ETextureLayering : uint8_t
	{
		NONE,
		ARRAY_2D,
		VOLUME_3D,
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
	// Synchronously reads a completed COLOR_TARGET | COPY_SOURCE texture into a top-left RGBA image.
	// The caller must end the texture's render pass first.
	virtual bool ReadTexture(CTextureHandle Texture, CImageInfo &Image) = 0;
	class ITextureReadback
	{
	public:
		virtual ~ITextureReadback() = default;
		[[nodiscard]] virtual bool IsReady() const = 0;
		// Waits for completion and moves the image on success. May only be called once.
		virtual bool Wait(CImageInfo &Image) = 0;
	};
	// Queues a readback without waiting for the render thread. Destroying the
	// returned handle waits for pending work, so its command storage stays valid.
	[[nodiscard]] virtual std::unique_ptr<ITextureReadback> ReadTextureAsync(CTextureHandle Texture) = 0;
	// Redirects the presentation target to Texture for one complete frame. This
	// includes presentation passes opened by nested effects such as menu blur.
	virtual bool BeginOffscreenFrame(CTextureHandle Texture) = 0;
	// Finishes the frame without presenting and returns its queued readback.
	[[nodiscard]] virtual std::unique_ptr<ITextureReadback> EndOffscreenFrame() = 0;
	// Presents the current frame and returns its queued top-left RGBA readback.
	[[nodiscard]] virtual std::unique_ptr<ITextureReadback> PresentAndReadbackAsync() = 0;

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

	enum class EVertexAttributeType : uint8_t
	{
		FLOAT32,
		UINT8,
		UINT16,
		INT32,
		UINT32,
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

	// A moved pointer must be allocated with malloc() and is consumed only when
	// the command is accepted. Create returns an invalid handle on failure.
	virtual CBufferHandle CreateBufferObject(size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer = false) = 0;
	virtual bool RecreateBufferObject(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer = false) = 0;
	// Failed destroys leave the passed handle valid so they can be retried.
	virtual void DeleteBufferObject(CBufferHandle &Buffer) = 0;

	virtual CBufferContainerHandle CreateBufferContainer(struct SBufferContainerInfo *pContainerInfo) = 0;
	// destroying all buffer objects means, that all referenced VBOs are destroyed automatically, so the user does not need to save references to them
	// Failed destroys leave the passed handle valid so they can be retried.
	virtual void DeleteBufferContainer(CBufferContainerHandle &Container, bool DestroyAllBO = true) = 0;
	[[nodiscard]] virtual bool IndicesNumRequiredNotify(unsigned int RequiredIndicesCount) = 0;

	// returns true if the driver age type is supported, passing BACKEND_TYPE_AUTO for BackendType will query the values for the currently used backend
	virtual bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) = 0;
	virtual bool IsConfigModernAPI() = 0;
	virtual bool IsTileBufferingEnabled() = 0;
	virtual bool IsQuadBufferingEnabled() = 0;
	virtual bool IsTextBufferingEnabled() = 0;
	virtual bool IsQuadContainerBufferingEnabled() = 0;
	virtual bool Uses2DTextureArrays() = 0;
	virtual int TextureLoadFlags() = 0;
	virtual bool HasTextureArraysSupport() = 0;

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
	virtual void LinesBegin() = 0;
	virtual void LinesEnd() = 0;
	virtual void LinesDraw(const CLineItem *pArray, size_t Num) = 0;

	class CLineItemBatch
	{
	public:
		IGraphics::CLineItem m_aItems[256];
		size_t m_NumItems = 0;
	};
	virtual void LinesBatchBegin(CLineItemBatch *pBatch) = 0;
	virtual void LinesBatchEnd(CLineItemBatch *pBatch) = 0;
	virtual void LinesBatchDraw(CLineItemBatch *pBatch, const CLineItem *pArray, size_t Num) = 0;

	virtual void QuadsBegin() = 0;
	virtual void QuadsEnd() = 0;
	virtual void QuadsTex3DBegin() = 0;
	virtual void QuadsTex3DEnd() = 0;
	virtual void TrianglesBegin() = 0;
	virtual void TrianglesEnd() = 0;
	virtual void QuadsEndKeepVertices() = 0;
	virtual void QuadsDrawCurrentVertices(bool KeepVertices = true) = 0;
	virtual void QuadsSetRotation(float Angle) = 0;
	virtual void QuadsSetSubset(float TopLeftU, float TopLeftV, float BottomRightU, float BottomRightV) = 0;
	virtual void QuadsSetSubsetFree(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int Index = -1) = 0;

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
	virtual void QuadsDraw(CQuadItem *pArray, int Num) = 0;
	virtual void QuadsDrawTL(const CQuadItem *pArray, int Num) = 0;

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

	virtual void QuadsDrawFreeform(const CFreeformItem *pArray, int Num) = 0;
	virtual void QuadsText(float x, float y, float Size, const char *pText) = 0;

	// sprites
	enum
	{
		SPRITE_FLAG_FLIP_Y = 1,
		SPRITE_FLAG_FLIP_X = 2,
	};
	virtual void SelectSprite(int Id, int Flags = 0) = 0;
	virtual void SelectSprite7(int Id, int Flags = 0) = 0;

	virtual void GetSpriteScale(const CDataSprite *pSprite, float &ScaleX, float &ScaleY) const = 0;
	virtual void GetSpriteScale(int Id, float &ScaleX, float &ScaleY) const = 0;
	virtual void GetSpriteScaleImpl(int Width, int Height, float &ScaleX, float &ScaleY) const = 0;

	virtual void DrawSprite(float x, float y, float Size) = 0;
	virtual void DrawSprite(float x, float y, float ScaledWidth, float ScaledHeight) = 0;

	virtual int QuadContainerAddSprite(int QuadContainerIndex, float x, float y, float Size) = 0;
	virtual int QuadContainerAddSprite(int QuadContainerIndex, float Size) = 0;
	virtual int QuadContainerAddSprite(int QuadContainerIndex, float Width, float Height) = 0;
	virtual int QuadContainerAddSprite(int QuadContainerIndex, float X, float Y, float Width, float Height) = 0;

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
	virtual void DrawRectExt(float x, float y, float w, float h, float r, int Corners) = 0;
	virtual void DrawRectExt4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, float r, int Corners) = 0;
	virtual int CreateRectQuadContainer(float x, float y, float w, float h, float r, int Corners) = 0;
	virtual void DrawRect(float x, float y, float w, float h, ColorRGBA Color, int Corners, float Rounding) = 0;
	virtual void DrawRect4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, int Corners, float Rounding) = 0;
	virtual void DrawCircle(float CenterX, float CenterY, float Radius, int Segments) = 0;

	struct CColorVertex
	{
		int m_Index;
		float m_R, m_G, m_B, m_A;
		CColorVertex() = default;
		CColorVertex(int i, float r, float g, float b, float a) :
			m_Index(i), m_R(r), m_G(g), m_B(b), m_A(a) {}
		CColorVertex(int i, ColorRGBA Color) :
			m_Index(i), m_R(Color.r), m_G(Color.g), m_B(Color.b), m_A(Color.a) {}
	};
	virtual void SetColorVertex(const CColorVertex *pArray, size_t Num) = 0;
	virtual void SetColor(float r, float g, float b, float a) = 0;
	virtual void SetColor(ColorRGBA Color) = 0;
	virtual void SetColor4(ColorRGBA TopLeft, ColorRGBA TopRight, ColorRGBA BottomLeft, ColorRGBA BottomRight) = 0;
	virtual void ChangeColorOfCurrentQuadVertices(float r, float g, float b, float a) = 0;
	virtual void ChangeColorOfQuadVertices(size_t QuadOffset, unsigned char r, unsigned char g, unsigned char b, unsigned char a) = 0;

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
	virtual int GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen) = 0;
	virtual void GetCurrentVideoMode(CVideoMode &CurMode, int Screen) = 0;
	virtual void Swap() = 0;
	virtual int GetNumScreens() const = 0;
	virtual const char *GetScreenName(int Screen) const = 0;

	// synchronization
	virtual void InsertSignal(class CSemaphore *pSemaphore) = 0;
	virtual bool IsIdle() const = 0;
	virtual void WaitForIdle() = 0;

	virtual void SetWindowGrab(bool Grab) = 0;
	virtual void NotifyWindow() = 0;

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
	/**
	 * Shows a modal message box with configuration title, message and buttons.
	 *
	 * @param MessageBox Description of the message box.
	 *
	 * @return Optional containing the index of the pressed button if the popup was shown successfully.
	 * @return Empty optional if the message box was not shown successfully.
	 *
	 * @remark Note that calling this function will destroy the current window,
	 *         so it only makes sense for fatal errors at the moment.
	 */
	virtual std::optional<int> ShowMessageBox(const CMessageBox &MessageBox) = 0;

	virtual bool IsBackendInitialized() = 0;
};

struct SBufferContainerInfo
{
	size_t m_Stride;
	IGraphics::CBufferHandle m_VertBufferBinding;

	// the attributes of the container
	using SAttribute = IGraphics::CVertexAttributeDesc;
	std::vector<SAttribute> m_vAttributes;
};

class IEngineGraphics : public IGraphics
{
	MACRO_INTERFACE("enginegraphics")
public:
	virtual int Init() = 0;
	void Shutdown() override = 0;

	virtual void Minimize() = 0;

	virtual int WindowActive() = 0;
	virtual int WindowOpen() = 0;
};

extern IEngineGraphics *CreateEngineGraphicsThreaded();

/**
 * This function should only be used when the graphics are not initialized or when @link IGraphics::ShowMessageBox @endlink failed.
 *
 * @see IGraphics::ShowMessageBox
 */
extern std::optional<int> ShowMessageBoxWithoutGraphics(const IGraphics::CMessageBox &MessageBox);

#endif
