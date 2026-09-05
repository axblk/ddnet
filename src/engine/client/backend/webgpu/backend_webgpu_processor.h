#ifndef ENGINE_CLIENT_BACKEND_WEBGPU_BACKEND_WEBGPU_PROCESSOR_H
#define ENGINE_CLIENT_BACKEND_WEBGPU_BACKEND_WEBGPU_PROCESSOR_H

#include <engine/client/backend/webgpu/backend_webgpu.h>

#if defined(CONF_BACKEND_WEBGPU)

// The WebGPU backend's one class. The declaration is here; the definitions
// are in backend_webgpu.cpp, in sections by what they concern.

#include <base/log.h>
#include <base/str.h>

#include <engine/client/backend/backend_base.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/shared/config.h>

#include <webgpu/webgpu.h>
#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#else
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// Defined in backend_webgpu.cpp: hands control back to the browser.
extern "C" void YieldToBrowser(int WaitForFrame);
#endif

using namespace std::chrono_literals; // NOLINT(google-build-using-namespace)

constexpr auto REQUEST_TIMEOUT = 30s;
constexpr uint64_t STREAM_BUFFER_SIZE = 4 * 1024 * 1024;
constexpr uint64_t UNIFORM_BUFFER_SIZE = 1024 * 1024;
constexpr size_t UPLOAD_BUFFER_SLOT_COUNT = 3;
// How many readbacks may be in flight at once. Matches what the video export
// holds in slots, so an export never has to wait for a picture it already
// asked the device for.
constexpr size_t READBACK_SLOT_COUNT = 3;
constexpr size_t GPU_TIMESTAMP_SLOT_COUNT = 4;
constexpr uint64_t GPU_TIMESTAMP_SIZE = 2 * sizeof(uint64_t);
constexpr uint64_t GPU_TIMESTAMP_RESOLVE_STRIDE = 256;
constexpr size_t BLEND_MODE_COUNT = 3;
constexpr size_t PRIMITIVE_PIPELINE_COUNT = 2 * BLEND_MODE_COUNT * 2;
constexpr size_t BUFFERED_PIPELINE_COUNT = BLEND_MODE_COUNT * 2;

struct SPrimitiveTransform
{
	vec2 m_Scale;
	vec2 m_Translate;
	ColorRGBA m_Color;
	vec2 m_RotationCenter;
	float m_Rotation;
	uint32_t m_AlphaTexture;
	vec2 m_VertexOffset;
	vec2 m_VertexScale;
	uint32_t m_QuadBase;
	float m_TextureSize;
	vec2 m_Padding;
	ColorRGBA m_SecondaryColor;
};
static_assert(sizeof(SPrimitiveTransform) == sizeof(float) * 24);

// What a draw says about itself. The screen mapping and whether the texture
// carries only alpha are worked out from the state, so they are not here.
struct SDrawUniforms
{
	ColorRGBA m_Color = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	vec2 m_RotationCenter = vec2(0.0f, 0.0f);
	float m_Rotation = 0.0f;
	vec2 m_VertexOffset = vec2(0.0f, 0.0f);
	vec2 m_VertexScale = vec2(1.0f, 1.0f);
	uint32_t m_QuadBase = 0;
	float m_TextureSize = 1.0f;
	ColorRGBA m_SecondaryColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
};

// The one place that turns an interface format into a WebGPU one. How many
// bytes a pixel of it costs is IGraphics::PixelSize's answer, not a second
// table's, so that a format added to one is never missing from the other.
inline WGPUTextureFormat ToWGPUFormat(IGraphics::ETextureFormat Format)
{
	switch(Format)
	{
	case IGraphics::ETextureFormat::RGBA8_UNORM: return WGPUTextureFormat_RGBA8Unorm;
	case IGraphics::ETextureFormat::RG8_UNORM: return WGPUTextureFormat_RG8Unorm;
	case IGraphics::ETextureFormat::R8_UNORM: return WGPUTextureFormat_R8Unorm;
	}
	dbg_assert(false, "Unknown texture format");
	dbg_break();
}

inline uint64_t AlignUp(uint64_t Value, uint64_t Alignment)
{
	return (Value + Alignment - 1) / Alignment * Alignment;
}

inline std::vector<uint8_t> DownsampleMip(const uint8_t *pSource, uint32_t Width, uint32_t Height, uint32_t Layers, size_t PixelSize)
{
	const uint32_t NewWidth = std::max(Width / 2, 1u);
	const uint32_t NewHeight = std::max(Height / 2, 1u);
	std::vector<uint8_t> vResult(static_cast<size_t>(NewWidth) * NewHeight * Layers * PixelSize);
	for(uint32_t Layer = 0; Layer < Layers; ++Layer)
	{
		for(uint32_t Y = 0; Y < NewHeight; ++Y)
		{
			const float SourceY = (Y + 0.5f) * Height / NewHeight - 0.5f;
			const uint32_t SourceY0 = static_cast<uint32_t>(SourceY);
			const uint32_t SourceY1 = std::min(SourceY0 + 1, Height - 1);
			const float WeightY = SourceY - SourceY0;
			for(uint32_t X = 0; X < NewWidth; ++X)
			{
				const float SourceX = (X + 0.5f) * Width / NewWidth - 0.5f;
				const uint32_t SourceX0 = static_cast<uint32_t>(SourceX);
				const uint32_t SourceX1 = std::min(SourceX0 + 1, Width - 1);
				const float WeightX = SourceX - SourceX0;
				for(size_t Channel = 0; Channel < PixelSize; ++Channel)
				{
					auto Sample = [&](uint32_t SampleX, uint32_t SampleY) { return pSource[((static_cast<size_t>(Layer) * Height + SampleY) * Width + SampleX) * PixelSize + Channel]; };
					const float Top = Sample(SourceX0, SourceY0) * (1.0f - WeightX) + Sample(SourceX1, SourceY0) * WeightX;
					const float Bottom = Sample(SourceX0, SourceY1) * (1.0f - WeightX) + Sample(SourceX1, SourceY1) * WeightX;
					vResult[((static_cast<size_t>(Layer) * NewHeight + Y) * NewWidth + X) * PixelSize + Channel] = static_cast<uint8_t>(Top * (1.0f - WeightY) + Bottom * WeightY + 0.5f);
				}
			}
		}
	}
	return vResult;
}

inline WGPUStringView StringView(const char *pString)
{
	return {pString, WGPU_STRLEN};
}

inline std::string ToString(WGPUStringView View)
{
	return View.data == nullptr ? std::string() : std::string(View.data, View.length);
}

inline const char *BackendName(WGPUBackendType Backend)
{
	switch(Backend)
	{
	case WGPUBackendType_WebGPU: return "WebGPU";
	case WGPUBackendType_D3D12: return "D3D12";
	case WGPUBackendType_Vulkan: return "Vulkan";
	case WGPUBackendType_OpenGL: return "OpenGL";
	case WGPUBackendType_OpenGLES: return "OpenGL ES";
	case WGPUBackendType_Metal: return "Metal";
	default: return "unknown";
	}
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
constexpr const char *WEBGPU_IMPLEMENTATION_NAME = "browser WebGPU";
constexpr const char *WEBGPU_IMPLEMENTATION_VERSION = "WebGPU";
#else
constexpr const char *WEBGPU_IMPLEMENTATION_NAME = "wgpu-native";
constexpr const char *WEBGPU_IMPLEMENTATION_VERSION = "wgpu-native v29.0.1.1";
#endif

struct SRequestAdapterResult
{
	bool m_Done = false;
	WGPURequestAdapterStatus m_Status = WGPURequestAdapterStatus_Error;
	WGPUAdapter m_Adapter = nullptr;
	std::string m_Message;
};

struct SRequestDeviceResult
{
	bool m_Done = false;
	WGPURequestDeviceStatus m_Status = WGPURequestDeviceStatus_Error;
	WGPUDevice m_Device = nullptr;
	std::string m_Message;
};
struct SMapResult
{
	bool m_Done = false;
	WGPUMapAsyncStatus m_Status = WGPUMapAsyncStatus_Error;
};
// A readback whose copy is submitted and whose mapping was asked for, but
// whose pixels have not arrived yet.
struct SPendingReadback
{
	WGPUBuffer m_Buffer = nullptr;
	uint64_t m_BufferSize = 0;
	uint32_t m_BytesPerRow = 0;
	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	bool m_BGRA = false;
	bool m_OpaqueAlpha = false;
	CCommandBuffer::SImageReadbackResult *m_pResult = nullptr;
	std::shared_ptr<SMapResult> m_pMapResult;
};
struct SQueueResult
{
	bool m_Pending = false;
	bool m_Done = false;
	WGPUQueueWorkDoneStatus m_Status = WGPUQueueWorkDoneStatus_Error;
};

class CCommandProcessorFragment_WebGpu final : public CCommandProcessorFragment_Renderer
{
	struct STexture
	{
		WGPUTexture m_Texture = nullptr;
		WGPUTextureView m_View = nullptr;
		WGPUTexture m_MultisampleTexture = nullptr;
		WGPUTextureView m_MultisampleView = nullptr;
		size_t m_MultisampleMemorySize = 0;
		std::array<WGPUBindGroup, 2> m_aBindGroups{};
		WGPUTexture m_ArrayTexture = nullptr;
		WGPUTextureView m_ArrayView = nullptr;
		std::array<WGPUBindGroup, 2> m_aArrayBindGroups{};
		size_t m_Width = 0;
		size_t m_Height = 0;
		size_t m_MemorySize = 0;
		IGraphics::ETextureFormat m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
		uint8_t m_Usage = 0;
	};
	struct SBuffer
	{
		WGPUBuffer m_Buffer = nullptr;
		size_t m_Size = 0;
		size_t m_AllocatedSize = 0;
		IGraphics::EBufferUsage m_Usage = IGraphics::EBufferUsage::VERTEX;
	};
	struct SPipelineSet
	{
		std::array<WGPURenderPipeline, PRIMITIVE_PIPELINE_COUNT> m_aPrimitive{};
		std::array<WGPURenderPipeline, PRIMITIVE_PIPELINE_COUNT> m_aLayeredPrimitive{};
		std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> m_aUniformColor{};
		std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> m_aInstanced{};
		std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> m_aArrayColor{};
		std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> m_aArrayColorTransform{};
		std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> m_aQuadPerItem{};
		std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> m_aQuadShared{};
		std::array<WGPURenderPipeline, BLEND_MODE_COUNT> m_aDualAtlas{};
		WGPURenderPipeline m_Blur = nullptr;
		WGPURenderPipeline m_PlanarYuv = nullptr;
	};
	struct SGpuTimestampSlot
	{
		WGPUBuffer m_ReadbackBuffer = nullptr;
		std::shared_ptr<SMapResult> m_pMapResult;
		bool m_InFlight = false;
		bool m_Publish = false;
	};

	EWebGpuBackendType m_BackendType;
	// The native window handle, asked of the surface owner every time it is
	// needed: Android takes the window away while the app is in the
	// background and hands out another one on resume, and the surface has to
	// be built from that one. Without a surface there is no window, and an
	// empty handle says so.
	const SWebGpuNativeWindow &NativeWindow() const
	{
		static const SWebGpuNativeWindow s_NoWindow;
		return m_Presentation.IsPresentable() ? m_Presentation.m_pSurface->WebGpuNativeWindow() : s_NoWindow;
	}
	// The surface a frame is presented to, or none. Everything that only
	// exists because there is one - the swapchain configuration, vsync, the
	// pipeline set built for the surface format - hangs off this.
	CCommandProcessorFragment_Renderer::SPresentationSurface m_Presentation;
	WGPUInstance m_Instance = nullptr;
	WGPUSurface m_Surface = nullptr;
	WGPUAdapter m_Adapter = nullptr;
	WGPUDevice m_Device = nullptr;
	WGPUQueue m_Queue = nullptr;
	WGPUSurfaceTexture m_SurfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
	WGPUTexture m_SurfaceMultisampleTexture = nullptr;
	WGPUTextureView m_SurfaceMultisampleView = nullptr;
	size_t m_SurfaceMultisampleMemorySize = 0;
	// The image a frame is drawn into instead of the surface's own texture.
	// See EnsureScreenTexture.
	WGPUTexture m_ScreenTexture = nullptr;
	WGPUTextureView m_ScreenView = nullptr;
	size_t m_ScreenMemorySize = 0;
	bool m_ScreenTouched = false;
	WGPUCommandEncoder m_CommandEncoder = nullptr;
	WGPURenderPassEncoder m_RenderPass = nullptr;
	// A run of glyphs, a tile layer, a column of server browser rows: most
	// consecutive draws keep the pipeline, the texture and the clip
	// rectangle they already had, and setting each of them again is an
	// encoder call that changes nothing. A render pass encoder begins with
	// none of this state, so the cache is cleared whenever one begins.
	// The bind group layout each slot of a pipeline's layout expects. A
	// bind group stays set across a pipeline switch, and is valid for the
	// next pipeline wherever the layouts agree - which they mostly do: the
	// switches here change the shader far more often than the layout.
	using SBindGroupLayouts = std::array<WGPUBindGroupLayout, 3>;
	std::unordered_map<WGPURenderPipeline, SBindGroupLayouts> m_PipelineBindGroupLayouts;

	struct SPassState
	{
		WGPURenderPipeline m_Pipeline = nullptr;
		SBindGroupLayouts m_aBindGroupLayouts = {};
		std::array<WGPUBindGroup, 3> m_aBindGroups = {};
		std::array<uint32_t, 3> m_aBindGroupOffsets = {};
		std::array<uint32_t, 4> m_aViewport = {};
		std::array<uint32_t, 4> m_aScissor = {};
		bool m_HasViewport = false;
		bool m_HasScissor = false;
	};
	SPassState m_PassState;
	// GPU time per frame, published through the frame statistics.
	WGPUQuerySet m_GpuTimestampQuerySet = nullptr;
	WGPUBuffer m_GpuTimestampResolveBuffer = nullptr;
	std::array<SGpuTimestampSlot, GPU_TIMESTAMP_SLOT_COUNT> m_aGpuTimestampSlots;
	std::array<SQueueResult, UPLOAD_BUFFER_SLOT_COUNT> m_aUploadBufferResults;
	std::vector<SPendingReadback> m_vPendingReadbacks;
	WGPUShaderModule m_PrimitiveShader = nullptr;
	WGPUBindGroupLayout m_UniformBindGroupLayout = nullptr;
	WGPUBindGroupLayout m_EmptyBindGroupLayout = nullptr;
	WGPUBindGroupLayout m_TextureBindGroupLayout = nullptr;
	WGPUBindGroupLayout m_ArrayTextureBindGroupLayout = nullptr;
	WGPUBindGroupLayout m_QuadBindGroupLayout = nullptr;
	WGPUPipelineLayout m_UntexturedPipelineLayout = nullptr;
	WGPUPipelineLayout m_PrimitivePipelineLayout = nullptr;
	WGPUPipelineLayout m_ArrayTexturePipelineLayout = nullptr;
	WGPUPipelineLayout m_QuadTexturedPipelineLayout = nullptr;
	WGPUPipelineLayout m_QuadUntexturedPipelineLayout = nullptr;
	std::array<SPipelineSet, 2> m_aPipelineSets{};
	// Wrap modes, matching the Vulkan backend. Array textures hold one tile
	// per layer, so their layer axis repeats while the tile itself must not
	// bleed into its neighbour.
	enum class ESamplerKind
	{
		REPEAT,
		CLAMP_TO_EDGE,
		ARRAY,
		COUNT,
	};
	std::array<WGPUSampler, static_cast<size_t>(ESamplerKind::COUNT)> m_aSamplers{};
	WGPUBuffer m_StreamBuffer = nullptr;
	WGPUBuffer m_UniformBuffer = nullptr;
	WGPUBindGroup m_UniformBindGroup = nullptr;
	WGPUBindGroup m_QuadBindGroup = nullptr;
	WGPUTextureFormat m_SurfaceFormat = WGPUTextureFormat_Undefined;
	WGPUCompositeAlphaMode m_AlphaMode = WGPUCompositeAlphaMode_Auto;
	WGPUPresentMode m_PresentMode = WGPUPresentMode_Fifo;
	uint32_t m_SurfaceWidth = 0;
	uint32_t m_SurfaceHeight = 0;
	uint32_t m_ViewportX = 0;
	uint32_t m_ViewportY = 0;
	uint32_t m_ViewportWidth = 0;
	uint32_t m_ViewportHeight = 0;
	uint32_t m_UniformAlignment = 256;
	uint32_t m_MultiSamplingCount = 0;
	uint32_t m_NextMultiSamplingCount = 0;
	uint64_t m_StreamOffset = 0;
	uint64_t m_UniformOffset = 0;
	size_t m_UploadBufferSlot = 0;
	float m_GpuTimestampPeriod = 0.0f;
	int m_GpuTimestampActiveSlot = -1;
	IGraphics::CTextureHandle m_RenderTarget;
	WGPULoadOp m_RenderPassLoadOp = WGPULoadOp_Load;
	WGPUColor m_RenderPassClearColor = WGPU_COLOR_INIT;
	bool m_SupportsFifo = false;
	bool m_SupportsImmediate = false;
	bool m_SupportsMailbox = false;
	bool m_SurfaceConfigured = false;
	bool m_SurfaceDirty = true;
	// wgpu's GL backend never reports CopySrc on the surface - the surface
	// there is the default framebuffer, and there is nothing to copy out of
	// a framebuffer. Rendering does not need it; only reading the presented
	// frame back for a screenshot does, so that is what goes without it.
	bool m_Minimized = false;
	bool m_SkipPresentationFrame = false;
	bool m_SurfaceSuboptimal = false;
	bool m_DeviceLost = false;
	bool m_PresentedOnce = false;
	bool m_GpuTimestampSupported = false;
	bool m_GpuTimestampResourcesFailed = false;
	bool m_GpuTimestampActiveSubmitted = false;
	std::atomic<bool> m_UncapturedError = false;
	std::string m_ErrorMessage;
	SRequestAdapterResult m_AdapterResult;
	SRequestDeviceResult m_DeviceResult;
	CGenerationHandleStore<IGraphics::CTextureHandle> m_TextureHandles;
	std::vector<STexture> m_vTextures;
	CGenerationHandleStore<IGraphics::CBufferHandle> m_BufferHandles;
	std::vector<SBuffer> m_vBuffers;
	std::vector<uint8_t> m_vStreamUpload;
	std::vector<uint8_t> m_vUniformUpload;
	std::vector<uint8_t> m_vUploadScratch;
	std::atomic<uint64_t> *m_pTextureMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pBufferMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pStreamMemoryUsage = nullptr;
	SGpuTimingShared *m_pGpuTiming = nullptr;

	static void AdapterCallback(WGPURequestAdapterStatus Status, WGPUAdapter Adapter, WGPUStringView Message, void *pUserdata1, void *);

	static void DeviceCallback(WGPURequestDeviceStatus Status, WGPUDevice Device, WGPUStringView Message, void *pUserdata1, void *);

	static void MapCallback(WGPUMapAsyncStatus Status, WGPUStringView, void *pUserdata1, void *);

	static void QueueCallback(WGPUQueueWorkDoneStatus Status, WGPUStringView, void *pUserdata1, void *);

	bool AdvanceUploadBufferSlot();

	static void DeviceLostCallback(WGPUDevice const *, WGPUDeviceLostReason Reason, WGPUStringView Message, void *pUserdata1, void *);

	static void UncapturedErrorCallback(WGPUDevice const *, WGPUErrorType Type, WGPUStringView Message, void *pUserdata1, void *);

	template<typename T>
	bool ProcessUntilDone(const T &Result, const char *pOperation)
	{
		const auto Deadline = std::chrono::steady_clock::now() + REQUEST_TIMEOUT;
		while(true)
		{
			wgpuInstanceProcessEvents(m_Instance);
			// Pumping events usually completes the work already. Sleeping
			// before checking would put a millisecond on every wait, and
			// the upload buffer rotation waits once per submit inside a
			// frame, which caps the frame rate for no reason.
			if(Result.m_Done)
				return true;
			if(std::chrono::steady_clock::now() >= Deadline)
				break;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			// The browser has to run before it can resolve anything we are
			// waiting for, but a timer would hold a frame that is already
			// half rendered for a millisecond or more per poll.
			YieldFrame();
#else
			// Natively there is a wait to be had, and waiting for the queue
			// to catch up is what every one of these is waiting for anyway.
			// Sleeping instead would put a millisecond on each of them.
			if(m_Device != nullptr)
				wgpuDevicePoll(m_Device, WGPU_TRUE, nullptr);
			else
				std::this_thread::sleep_for(1ms);
#endif
		}
		if(Result.m_Done)
			return true;
		m_ErrorMessage = std::string(pOperation) + " timed out";
		return false;
	}

	void SetError(EGfxErrorType Type, const std::string &Message);

	static size_t SamplerIndex(EWrapMode WrapMode);

	uint32_t SampleCount() const;

	void ReleaseMultisampleTarget(WGPUTexture &Texture, WGPUTextureView &View, size_t &MemorySize);

	bool CreateMultisampleTarget(WGPUTextureFormat Format, uint32_t Width, uint32_t Height, uint32_t SampleCount, WGPUTexture &Texture, WGPUTextureView &View, size_t &MemorySize);

	static size_t BlendIndex(EBlendMode BlendMode);

	static size_t PrimitivePipelineIndex(EPrimitiveType PrimitiveType, EBlendMode BlendMode, bool Textured);

	static WGPUBlendState BlendState(size_t Blend);

	void ReleaseTexture(STexture &Texture);

	void RememberPipelineLayout(WGPURenderPipeline Pipeline, WGPUPipelineLayout Layout);

	bool CreateTextureBindGroups(WGPUTextureView View, WGPUBindGroupLayout Layout, uint32_t TextureBinding, std::array<WGPUBindGroup, 2> &aBindGroups, bool TextureArray);

	// The vertex input a pipeline is built for comes from IGraphics::VertexLayout,
	// the same table CGraphics_Threaded tags every draw with, so a pipeline and
	// the buffer it reads cannot drift apart.
	static WGPUVertexFormat VertexAttributeFormat(const IGraphics::CVertexAttributeDesc &Attribute);

	template<size_t ArraySize>
	static uint64_t FillVertexInput(IGraphics::EVertexLayout Layout, std::array<WGPUVertexAttribute, ArraySize> &aAttributes, uint32_t &AttributeCount)
	{
		const IGraphics::SVertexLayoutDesc &Desc = IGraphics::VertexLayout(Layout);
		dbg_assert(Desc.m_AttributeCount <= ArraySize, "Pipeline has room for fewer attributes than the vertex layout has");
		for(uint32_t Index = 0; Index < Desc.m_AttributeCount; ++Index)
		{
			const IGraphics::CVertexAttributeDesc &Attribute = Desc.m_aAttributes[Index];
			aAttributes[Index].format = VertexAttributeFormat(Attribute);
			aAttributes[Index].offset = Attribute.m_Offset;
			aAttributes[Index].shaderLocation = Index;
		}
		AttributeCount = Desc.m_AttributeCount;
		return Desc.m_Stride;
	}

	bool CreateBufferedPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, const char *pVertexEntry, bool Instanced, uint32_t SampleCount);

	bool CreateLayeredPrimitivePipelines(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount);

	bool CreateArrayColorPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, bool Transform, uint32_t SampleCount);

	bool CreateQuadPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, bool Shared, uint32_t SampleCount);

	bool CreateDualAtlasPipelines(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount);

	bool CreatePipelineSet(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount);

	bool CreatePrimitivePipelines();

	bool ApplyMultiSampling();

	bool CreateDrawResources();

	void DestroyDrawResources();

	bool CreateTexture(const CCommandBuffer::SCommand_Texture_Create *pCommand);

	bool UpdateTexture(const CCommandBuffer::SCommand_Texture_Update *pCommand);

	void DestroyTexture(IGraphics::CTextureHandle Handle);

	void ReleaseBuffer(SBuffer &Buffer);

	bool WriteBufferData(WGPUBuffer Buffer, uint64_t Offset, const void *pData, size_t Size, bool AllowEndPadding);

	bool CreateNativeBuffer(SBuffer &Buffer, const IGraphics::CBufferDesc &Desc, const void *pData);

	bool CreateBuffer(const CCommandBuffer::SCommand_CreateBufferObject *pCommand);

	bool RecreateBuffer(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand);

	void DestroyGpuTimestampResources();

	bool EnsureGpuTimestampResources();

	void CollectGpuTimestampResults();

	void BeginGpuTimestamp();

	void MapGpuTimestampSlot(int SlotIndex, bool Publish);

	bool EnsureCommandEncoder();

	bool DestroyBuffer(IGraphics::CBufferHandle Handle);

	STexture *RenderTarget();

	bool EnsureRenderPass();

	// The upload rings hold one frame's worth of vertices and uniforms. A
	// frame that needs more than that used to be the end of the renderer;
	// now what is drawn so far goes to the queue and the rings start over.
	// This has to happen before a pass is opened, because a submit ends the
	// pass it is inside and everything bound to it.
	bool EnsureUploadSpace(uint64_t StreamBytes, uint64_t UniformBytes);

	bool WriteStream(const void *pData, size_t Size, uint64_t Alignment, uint64_t &Offset);

	void SetPipelineCached(WGPURenderPipeline Pipeline);

	void SetBindGroupCached(uint32_t Slot, WGPUBindGroup BindGroup, const uint32_t *pDynamicOffset = nullptr);

	void SetViewportCached(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height);

	void SetScissorCached(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height);

	bool ApplyState(const CCommandBuffer::SState &State, WGPURenderPipeline Pipeline, const SDrawUniforms &Uniforms = {}, bool TextureArray = false);

	bool WriteQuadTransforms(const CCommandBuffer::SDrawDataQuadTransform *pData, uint32_t Count, uint32_t &Offset);

	bool Draw(const CCommandBuffer::SCommand_Draw *pCommand);

	bool DrawBuffered(const CCommandBuffer::SCommand_DrawIndexed *pCommand);

	WGPUSurface CreateSurface() const;

	bool QuerySurfaceConfiguration();

	bool SupportsPresentMode(WGPUPresentMode Mode) const;

	void ReleaseFrame();

	// The frame is drawn into an image of our own; the surface texture is
	// acquired at the end of the frame, gets a copy of the finished image
	// and is handed straight back, with nothing in between that could
	// wait. In the browser that is forced: a surface texture is ours only
	// until the browser gets its turn back, and every wait for the device
	// gives it one. Natively it costs a copy per frame and buys the same
	// frame everywhere - a screenshot reads this image whatever the surface
	// allows, and a surface that is not there to be had costs one picture
	// instead of the frame's recording.
	bool EnsureScreenTexture();

	void ReleaseScreenTexture();

	// Only ever called with the frame's own work already submitted, so the
	// submit below cannot land in a wait for the upload rings - the one
	// thing that would let the browser take the surface texture back while
	// the copy into it is still being recorded.
	bool CopyScreenToSurface();

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	void YieldFrame()
	{
		// The frame lives in an image of its own, so the browser may run
		// here without anything being lost. What is already encoded still
		// has to reach the queue: the wait that led here is a wait for the
		// device, and an unsubmitted encoder would never let it end.
		(void)SubmitCommands();
		YieldToBrowser(0);
	}
#endif

	// The view a screen pass resolves into.
	WGPUTextureView PresentationView() const;

	void EndRenderPass();

	bool SubmitCommands(bool EndsFrame = false, bool PublishGpuTimestamp = true);

	// Records the copy that reads a texture back and asks for the mapping,
	// without waiting for either. The pixels are handed over in
	// FinishReadback once the map callback has run, which is what lets the
	// caller keep several readbacks in flight.
	bool StartTextureReadback(WGPUTexture Texture, WGPUOrigin3D Origin, uint32_t Width, uint32_t Height, bool BGRA, bool OpaqueAlpha, CCommandBuffer::SImageReadbackResult *pResult);

	// Copies the mapped pixels out and releases the caller. The map callback
	// has run by the time this is called, successfully or not.
	void FinishReadback(SPendingReadback &Pending);

	bool FinishOldestReadback();

	// Hands over every readback whose mapping has already arrived, without
	// waiting for any that has not.
	void CollectFinishedReadbacks();

	// Waits out every readback still in flight. This is what the frontend
	// sends when it has to have a picture now.
	bool FinishReadbacks();

	// Releases callers that will never get their picture.
	void AbandonReadbacks();

	void PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand);

	void TextureReadback(const CCommandBuffer::SCommand_Texture_Readback *pCommand);

	void DiscardFrame();

	bool RecreateSurface();

	bool ConfigureIfNeeded();

	bool AcquireFrame();

	bool Clear(const CCommandBuffer::SCommand_Clear *pCommand);

	bool Present();

	bool Initialize(const SCommand_Init *pCommand);

	void Cleanup();

public:
	explicit CCommandProcessorFragment_WebGpu(EWebGpuBackendType BackendType) :
		m_BackendType(BackendType)
	{
	}

	~CCommandProcessorFragment_WebGpu() override
	{
		dbg_assert(m_Instance == nullptr, "WebGPU resources must be released on the graphics worker");
	}

	ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override;
};
#endif

#endif
