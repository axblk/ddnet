#include "backend_webgpu.h"

#if defined(CONF_BACKEND_WEBGPU)

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
#include <vector>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// Handing control back to the browser. Waiting for an animation frame is what a
// frame wants: the canvas is composited once per refresh whatever the loop does.
// A wait inside a frame, and a loop that is meant to run faster than the screen,
// want the shortest turn instead, and that is not a timer: a timer costs a
// millisecond, and four once a handful of them nest, which is most of a frame
// spent waiting for nothing. A message channel posts a plain task with no such
// floor, so the browser gets its turn and we get it straight back.
//
// Neither wait works on a hidden page. It gets no animation frames, and it must
// not be spun through microtasks either -- a microtask that queues another one
// never lets the browser run the task that says we are visible again, so the page
// would stay hidden and busy for good. A slow timer is the only way out.
// clang-format off
EM_ASYNC_JS(void, YieldToBrowser, (int WaitForFrame), {
	if(document.hidden)
	{
		await new Promise(function(resolve) { setTimeout(resolve, 100); });
		return;
	}
	if(WaitForFrame)
	{
		await new Promise(function(resolve) {
			var frame = requestAnimationFrame(function() {
				document.removeEventListener("visibilitychange", hidden);
				resolve();
			});
			function hidden()
			{
				if(document.hidden)
				{
					cancelAnimationFrame(frame);
					document.removeEventListener("visibilitychange", hidden);
					resolve();
				}
			}
			document.addEventListener("visibilitychange", hidden);
		});
		return;
	}
	if(Module.ddnetYield === undefined)
	{
		var channel = new MessageChannel();
		Module.ddnetYield = {channel: channel, resolve: null};
		channel.port1.onmessage = function() {
			var resolve = Module.ddnetYield.resolve;
			Module.ddnetYield.resolve = null;
			resolve();
		};
	}
	await new Promise(function(resolve) {
		Module.ddnetYield.resolve = resolve;
		Module.ddnetYield.channel.port2.postMessage(0);
	});
});
// clang-format on
#endif

namespace
{
	using namespace std::chrono_literals;

	constexpr auto REQUEST_TIMEOUT = 30s;
	constexpr uint64_t STREAM_BUFFER_SIZE = 4 * 1024 * 1024;
	constexpr uint64_t UNIFORM_BUFFER_SIZE = 1024 * 1024;
	constexpr size_t UPLOAD_BUFFER_SLOT_COUNT = 3;
	constexpr size_t GPU_TIMESTAMP_SLOT_COUNT = 4;
	constexpr uint32_t GPU_TIMESTAMP_MAX_INTERVALS = 128;
	constexpr uint32_t GPU_TIMESTAMP_QUERY_COUNT = 2 + GPU_TIMESTAMP_MAX_INTERVALS * 2;
	constexpr uint64_t GPU_TIMESTAMP_SIZE = GPU_TIMESTAMP_QUERY_COUNT * sizeof(uint64_t);
	constexpr uint64_t GPU_TIMESTAMP_RESOLVE_STRIDE = (GPU_TIMESTAMP_SIZE + 255) / 256 * 256;
	using TGpuTimestampIntervalZones = std::array<IGraphics::EGpuRenderZone, GPU_TIMESTAMP_MAX_INTERVALS>;
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

	uint64_t AlignUp(uint64_t Value, uint64_t Alignment)
	{
		return (Value + Alignment - 1) / Alignment * Alignment;
	}

	std::vector<uint8_t> DownsampleMip(const uint8_t *pSource, uint32_t Width, uint32_t Height, uint32_t Layers, size_t PixelSize)
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

	WGPUStringView StringView(const char *pString)
	{
		return {pString, WGPU_STRLEN};
	}

	std::string ToString(WGPUStringView View)
	{
		return View.data == nullptr ? std::string() : std::string(View.data, View.length);
	}

	const char *BackendName(WGPUBackendType Backend)
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
		struct SBufferContainer
		{
			IGraphics::CBufferHandle m_VertexBuffer;
			size_t m_Stride = 0;
			uint32_t m_AttributeCount = 0;
		};
		struct STextureBinding
		{
			CCommandBuffer::STextureBindingDesc m_Desc;
			std::array<WGPUBindGroup, 2> m_aBindGroups{};
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
			uint32_t m_ZoneMask = 0;
			uint32_t m_IntervalCount = 0;
			TGpuTimestampIntervalZones m_aIntervalZones{};
			uint64_t m_Generation = 0;
		};

		SWebGpuNativeWindow m_NativeWindow;
		EWebGpuBackendType m_BackendType;
		EGraphicsBackendMode m_BackendMode = EGraphicsBackendMode::PRESENTATION;
		WGPUInstance m_Instance = nullptr;
		WGPUSurface m_Surface = nullptr;
		WGPUAdapter m_Adapter = nullptr;
		WGPUDevice m_Device = nullptr;
		WGPUQueue m_Queue = nullptr;
		WGPUSurfaceTexture m_SurfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
		WGPUTextureView m_SurfaceView = nullptr;
		WGPUTexture m_SurfaceMultisampleTexture = nullptr;
		WGPUTextureView m_SurfaceMultisampleView = nullptr;
		size_t m_SurfaceMultisampleMemorySize = 0;
		WGPUCommandEncoder m_CommandEncoder = nullptr;
		WGPURenderPassEncoder m_RenderPass = nullptr;
		WGPUQuerySet m_GpuTimestampQuerySet = nullptr;
		WGPUBuffer m_GpuTimestampResolveBuffer = nullptr;
		std::array<SGpuTimestampSlot, GPU_TIMESTAMP_SLOT_COUNT> m_aGpuTimestampSlots;
		std::array<SQueueResult, UPLOAD_BUFFER_SLOT_COUNT> m_aUploadBufferResults;
		WGPUShaderModule m_PrimitiveShader = nullptr;
		WGPUBindGroupLayout m_UniformBindGroupLayout = nullptr;
		WGPUBindGroupLayout m_EmptyBindGroupLayout = nullptr;
		WGPUBindGroupLayout m_TextureBindGroupLayout = nullptr;
		WGPUBindGroupLayout m_ArrayTextureBindGroupLayout = nullptr;
		WGPUBindGroupLayout m_DualTextureBindGroupLayout = nullptr;
		WGPUBindGroupLayout m_QuadBindGroupLayout = nullptr;
		WGPUPipelineLayout m_UntexturedPipelineLayout = nullptr;
		WGPUPipelineLayout m_PrimitivePipelineLayout = nullptr;
		WGPUPipelineLayout m_ArrayTexturePipelineLayout = nullptr;
		WGPUPipelineLayout m_DualTexturePipelineLayout = nullptr;
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
		std::array<int32_t, IGraphics::GPU_RENDER_ZONE_COUNT> m_aGpuTimestampZoneActiveIntervals{};
		TGpuTimestampIntervalZones m_aGpuTimestampIntervalZones{};
		uint32_t m_GpuTimestampIntervalCount = 0;
		uint32_t m_GpuTimestampInvalidZoneMask = 0;
		uint64_t m_GpuTimestampGeneration = 0;
		IGraphics::CTextureHandle m_RenderTarget;
		WGPULoadOp m_RenderPassLoadOp = WGPULoadOp_Load;
		WGPUColor m_RenderPassClearColor = WGPU_COLOR_INIT;
		bool m_SupportsFifo = false;
		bool m_SupportsImmediate = false;
		bool m_SupportsMailbox = false;
		bool m_SurfaceConfigured = false;
		bool m_SurfaceDirty = true;
		bool m_Minimized = false;
		bool m_SkipPresentationFrame = false;
		bool m_SurfaceSuboptimal = false;
		bool m_DeviceLost = false;
		bool m_PresentedOnce = false;
		bool m_GpuTimestampSupported = false;
		bool m_GpuTimestampInsidePassesSupported = false;
		bool m_GpuTimestampResourcesFailed = false;
		bool m_GpuTimestampActiveSubmitted = false;
		std::atomic<bool> m_UncapturedError = false;
		std::string m_ErrorMessage;
		SRequestAdapterResult m_AdapterResult;
		SRequestDeviceResult m_DeviceResult;
		CGenerationHandleStore<IGraphics::CTextureHandle> m_TextureHandles;
		std::vector<STexture> m_vTextures;
		CGenerationHandleStore<CCommandBuffer::CTextureBindingHandle> m_TextureBindingHandles;
		std::vector<STextureBinding> m_vTextureBindings;
		CGenerationHandleStore<CCommandBuffer::CPipelineHandle> m_PipelineHandles;
		std::vector<EPipelineProgram> m_vPipelines;
		CGenerationHandleStore<IGraphics::CBufferHandle> m_BufferHandles;
		std::vector<SBuffer> m_vBuffers;
		CGenerationHandleStore<IGraphics::CBufferContainerHandle> m_BufferContainerHandles;
		std::vector<SBufferContainer> m_vBufferContainers;
		std::vector<CCommandBuffer::SVertex> m_vQuadVertices;
		std::vector<CCommandBuffer::SVertexTex3DStream> m_vLayeredQuadVertices;
		std::vector<uint8_t> m_vStreamUpload;
		std::vector<uint8_t> m_vUniformUpload;
		std::vector<uint8_t> m_vUploadScratch;
		std::atomic<uint64_t> *m_pTextureMemoryUsage = nullptr;
		std::atomic<uint64_t> *m_pBufferMemoryUsage = nullptr;
		std::atomic<uint64_t> *m_pStreamMemoryUsage = nullptr;
		SGpuTimingShared *m_pGpuTiming = nullptr;

		static void AdapterCallback(WGPURequestAdapterStatus Status, WGPUAdapter Adapter, WGPUStringView Message, void *pUserdata1, void *)
		{
			auto *pResult = static_cast<SRequestAdapterResult *>(pUserdata1);
			pResult->m_Status = Status;
			pResult->m_Adapter = Adapter;
			pResult->m_Message = ToString(Message);
			pResult->m_Done = true;
		}

		static void DeviceCallback(WGPURequestDeviceStatus Status, WGPUDevice Device, WGPUStringView Message, void *pUserdata1, void *)
		{
			auto *pResult = static_cast<SRequestDeviceResult *>(pUserdata1);
			pResult->m_Status = Status;
			pResult->m_Device = Device;
			pResult->m_Message = ToString(Message);
			pResult->m_Done = true;
		}

		static void MapCallback(WGPUMapAsyncStatus Status, WGPUStringView, void *pUserdata1, void *)
		{
			auto pResult = std::move(*static_cast<std::shared_ptr<SMapResult> *>(pUserdata1));
			delete static_cast<std::shared_ptr<SMapResult> *>(pUserdata1);
			pResult->m_Status = Status;
			pResult->m_Done = true;
		}

		static void QueueCallback(WGPUQueueWorkDoneStatus Status, WGPUStringView, void *pUserdata1, void *)
		{
			auto *pResult = static_cast<SQueueResult *>(pUserdata1);
			pResult->m_Status = Status;
			pResult->m_Done = true;
		}

		bool AdvanceUploadBufferSlot()
		{
			auto &Result = m_aUploadBufferResults[m_UploadBufferSlot];
			Result = {};
			Result.m_Pending = true;
			WGPUQueueWorkDoneCallbackInfo CallbackInfo = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
			CallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
			CallbackInfo.callback = QueueCallback;
			CallbackInfo.userdata1 = &Result;
			wgpuQueueOnSubmittedWorkDone(m_Queue, CallbackInfo);

			m_UploadBufferSlot = (m_UploadBufferSlot + 1) % UPLOAD_BUFFER_SLOT_COUNT;
			auto &NextResult = m_aUploadBufferResults[m_UploadBufferSlot];
			if(NextResult.m_Pending && (!ProcessUntilDone(NextResult, "wait for WebGPU upload buffers") || NextResult.m_Status != WGPUQueueWorkDoneStatus_Success))
				return false;
			NextResult.m_Pending = false;
			return true;
		}

		static void DeviceLostCallback(WGPUDevice const *, WGPUDeviceLostReason Reason, WGPUStringView Message, void *pUserdata1, void *)
		{
			if(Reason == WGPUDeviceLostReason_CallbackCancelled)
				return;
			auto *pSelf = static_cast<CCommandProcessorFragment_WebGpu *>(pUserdata1);
			pSelf->m_DeviceLost = true;
			log_error("gfx/webgpu", "device lost (%d): %.*s", static_cast<int>(Reason), static_cast<int>(Message.length), Message.data != nullptr ? Message.data : "");
		}

		static void UncapturedErrorCallback(WGPUDevice const *, WGPUErrorType Type, WGPUStringView Message, void *pUserdata1, void *)
		{
			auto *pSelf = static_cast<CCommandProcessorFragment_WebGpu *>(pUserdata1);
			pSelf->m_UncapturedError.store(true, std::memory_order_release);
			log_error("gfx/webgpu", "uncaptured error (%d): %.*s", static_cast<int>(Type), static_cast<int>(Message.length), Message.data != nullptr ? Message.data : "");
		}

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
				YieldToBrowser(0);
#else
				std::this_thread::sleep_for(1ms);
#endif
			}
			if(Result.m_Done)
				return true;
			m_ErrorMessage = std::string(pOperation) + " timed out";
			return false;
		}

		void SetError(EGfxErrorType Type, const std::string &Message)
		{
			if(m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
				return;
			m_Error.m_ErrorType = Type;
			m_Error.m_vErrors.push_back(Message);
		}

		static size_t SamplerIndex(EWrapMode WrapMode)
		{
			return WrapMode == EWrapMode::REPEAT ? 0 : 1;
		}

		uint32_t SampleCount() const
		{
			return m_MultiSamplingCount == 0 ? 1 : m_MultiSamplingCount;
		}

		void ReleaseMultisampleTarget(WGPUTexture &Texture, WGPUTextureView &View, size_t &MemorySize)
		{
			if(MemorySize != 0 && m_pTextureMemoryUsage != nullptr)
				m_pTextureMemoryUsage->fetch_sub(MemorySize, std::memory_order_relaxed);
			if(View != nullptr)
				wgpuTextureViewRelease(View);
			if(Texture != nullptr)
				wgpuTextureRelease(Texture);
			View = nullptr;
			Texture = nullptr;
			MemorySize = 0;
		}

		bool CreateMultisampleTarget(WGPUTextureFormat Format, uint32_t Width, uint32_t Height, uint32_t SampleCount, WGPUTexture &Texture, WGPUTextureView &View, size_t &MemorySize)
		{
			if(SampleCount == 1 || View != nullptr)
				return true;
			WGPUTextureDescriptor Descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
			Descriptor.label = StringView("DDNet WebGPU multisample target");
			Descriptor.usage = WGPUTextureUsage_RenderAttachment;
			Descriptor.dimension = WGPUTextureDimension_2D;
			Descriptor.size = {Width, Height, 1};
			Descriptor.format = Format;
			Descriptor.mipLevelCount = 1;
			Descriptor.sampleCount = SampleCount;
			Texture = wgpuDeviceCreateTexture(m_Device, &Descriptor);
			View = Texture == nullptr ? nullptr : wgpuTextureCreateView(Texture, nullptr);
			if(View != nullptr)
			{
				MemorySize = static_cast<size_t>(Width) * Height * 4 * SampleCount;
				if(m_pTextureMemoryUsage != nullptr)
					m_pTextureMemoryUsage->fetch_add(MemorySize, std::memory_order_relaxed);
				return true;
			}
			ReleaseMultisampleTarget(Texture, View, MemorySize);
			SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to create a multisample target");
			return false;
		}

		static size_t BlendIndex(EBlendMode BlendMode)
		{
			switch(BlendMode)
			{
			case EBlendMode::NONE: return 0;
			case EBlendMode::ALPHA: return 1;
			case EBlendMode::ADDITIVE: return 2;
			}
			return 0;
		}

		static size_t PrimitivePipelineIndex(EPrimitiveType PrimitiveType, EBlendMode BlendMode, bool Textured)
		{
			const size_t Topology = PrimitiveType == EPrimitiveType::LINES ? 0 : 1;
			return (Topology * BLEND_MODE_COUNT + BlendIndex(BlendMode)) * 2 + Textured;
		}

		static WGPUBlendState BlendState(size_t Blend)
		{
			WGPUBlendState State = WGPU_BLEND_STATE_INIT;
			State.color.operation = WGPUBlendOperation_Add;
			State.alpha.operation = WGPUBlendOperation_Add;
			if(Blend == 1)
			{
				State.color.srcFactor = WGPUBlendFactor_SrcAlpha;
				State.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
				State.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
				State.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
			}
			else if(Blend == 2)
			{
				State.color.srcFactor = WGPUBlendFactor_SrcAlpha;
				State.color.dstFactor = WGPUBlendFactor_One;
				State.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
				State.alpha.dstFactor = WGPUBlendFactor_One;
			}
			return State;
		}

		void ReleaseTexture(STexture &Texture)
		{
			if(Texture.m_MemorySize != 0 && m_pTextureMemoryUsage != nullptr)
				m_pTextureMemoryUsage->fetch_sub(Texture.m_MemorySize, std::memory_order_relaxed);
			for(auto &BindGroup : Texture.m_aBindGroups)
			{
				if(BindGroup != nullptr)
					wgpuBindGroupRelease(BindGroup);
				BindGroup = nullptr;
			}
			for(auto &BindGroup : Texture.m_aArrayBindGroups)
			{
				if(BindGroup != nullptr)
					wgpuBindGroupRelease(BindGroup);
				BindGroup = nullptr;
			}
			if(Texture.m_View != nullptr)
				wgpuTextureViewRelease(Texture.m_View);
			if(Texture.m_Texture != nullptr)
				wgpuTextureRelease(Texture.m_Texture);
			ReleaseMultisampleTarget(Texture.m_MultisampleTexture, Texture.m_MultisampleView, Texture.m_MultisampleMemorySize);
			if(Texture.m_ArrayView != nullptr)
				wgpuTextureViewRelease(Texture.m_ArrayView);
			if(Texture.m_ArrayTexture != nullptr)
				wgpuTextureRelease(Texture.m_ArrayTexture);
			Texture = {};
		}

		bool CreateTextureBindGroups(WGPUTextureView View, WGPUBindGroupLayout Layout, uint32_t TextureBinding, std::array<WGPUBindGroup, 2> &aBindGroups, bool TextureArray)
		{
			for(size_t i = 0; i < aBindGroups.size(); ++i)
			{
				// An array texture ignores the wrap mode the draw asks for and
				// always uses the array sampler, like the Vulkan backend does.
				const size_t SamplerIndex = TextureArray ? static_cast<size_t>(ESamplerKind::ARRAY) : i;
				std::array<WGPUBindGroupEntry, 2> aEntries{};
				aEntries[0].binding = 0;
				aEntries[0].sampler = m_aSamplers[SamplerIndex];
				aEntries[1].binding = TextureBinding;
				aEntries[1].textureView = View;
				WGPUBindGroupDescriptor Descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
				Descriptor.label = StringView("DDNet WebGPU sampled texture binding");
				Descriptor.layout = Layout;
				Descriptor.entryCount = aEntries.size();
				Descriptor.entries = aEntries.data();
				aBindGroups[i] = wgpuDeviceCreateBindGroup(m_Device, &Descriptor);
				if(aBindGroups[i] == nullptr)
					return false;
			}
			return true;
		}

		bool CreateBufferedPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, const char *pVertexEntry, bool Instanced, uint32_t SampleCount)
		{
			std::array<WGPUVertexAttribute, 3> aVertexAttributes{};
			aVertexAttributes[0].format = WGPUVertexFormat_Float32x2;
			aVertexAttributes[0].offset = offsetof(CCommandBuffer::SVertex, m_Pos);
			aVertexAttributes[0].shaderLocation = 0;
			aVertexAttributes[1].format = WGPUVertexFormat_Float32x2;
			aVertexAttributes[1].offset = offsetof(CCommandBuffer::SVertex, m_Tex);
			aVertexAttributes[1].shaderLocation = 1;
			aVertexAttributes[2].format = WGPUVertexFormat_Unorm8x4;
			aVertexAttributes[2].offset = offsetof(CCommandBuffer::SVertex, m_Color);
			aVertexAttributes[2].shaderLocation = 2;
			std::array<WGPUVertexAttribute, 2> aInstanceAttributes{};
			aInstanceAttributes[0].format = WGPUVertexFormat_Float32x2;
			aInstanceAttributes[0].offset = offsetof(CCommandBuffer::SInstanceDataPositionScaleRotation, m_Position);
			aInstanceAttributes[0].shaderLocation = 3;
			aInstanceAttributes[1].format = WGPUVertexFormat_Float32x2;
			aInstanceAttributes[1].offset = offsetof(CCommandBuffer::SInstanceDataPositionScaleRotation, m_Scale);
			aInstanceAttributes[1].shaderLocation = 4;
			std::array<WGPUVertexBufferLayout, 2> aVertexBuffers{};
			aVertexBuffers[0] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			aVertexBuffers[0].arrayStride = sizeof(CCommandBuffer::SVertex);
			aVertexBuffers[0].stepMode = WGPUVertexStepMode_Vertex;
			aVertexBuffers[0].attributeCount = aVertexAttributes.size();
			aVertexBuffers[0].attributes = aVertexAttributes.data();
			aVertexBuffers[1] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			aVertexBuffers[1].arrayStride = sizeof(CCommandBuffer::SInstanceDataPositionScaleRotation);
			aVertexBuffers[1].stepMode = WGPUVertexStepMode_Instance;
			aVertexBuffers[1].attributeCount = aInstanceAttributes.size();
			aVertexBuffers[1].attributes = aInstanceAttributes.data();

			for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
			{
				for(size_t Textured = 0; Textured < 2; ++Textured)
				{
					WGPUBlendState BlendState = WGPU_BLEND_STATE_INIT;
					BlendState.color.operation = WGPUBlendOperation_Add;
					BlendState.alpha.operation = WGPUBlendOperation_Add;
					if(Blend == 1)
					{
						BlendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
						BlendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
						BlendState.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
						BlendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
					}
					else if(Blend == 2)
					{
						BlendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
						BlendState.color.dstFactor = WGPUBlendFactor_One;
						BlendState.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
						BlendState.alpha.dstFactor = WGPUBlendFactor_One;
					}
					WGPUColorTargetState ColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
					ColorTarget.format = Format;
					ColorTarget.blend = Blend == 0 ? nullptr : &BlendState;
					ColorTarget.writeMask = WGPUColorWriteMask_All;
					WGPUFragmentState Fragment = WGPU_FRAGMENT_STATE_INIT;
					Fragment.module = m_PrimitiveShader;
					Fragment.entryPoint = StringView(Textured != 0 ? "fs_textured" : "fs_untextured");
					Fragment.targetCount = 1;
					Fragment.targets = &ColorTarget;
					WGPURenderPipelineDescriptor Descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
					Descriptor.label = StringView("DDNet WebGPU buffered primitive pipeline");
					Descriptor.layout = Textured != 0 ? m_PrimitivePipelineLayout : m_UntexturedPipelineLayout;
					Descriptor.vertex.module = m_PrimitiveShader;
					Descriptor.vertex.entryPoint = StringView(pVertexEntry);
					Descriptor.vertex.bufferCount = Instanced ? 2 : 1;
					Descriptor.vertex.buffers = aVertexBuffers.data();
					Descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
					Descriptor.primitive.frontFace = WGPUFrontFace_CCW;
					Descriptor.primitive.cullMode = WGPUCullMode_None;
					Descriptor.multisample.count = SampleCount;
					Descriptor.multisample.mask = UINT32_MAX;
					Descriptor.fragment = &Fragment;
					const size_t Index = Blend * 2 + Textured;
					aPipelines[Index] = wgpuDeviceCreateRenderPipeline(m_Device, &Descriptor);
					if(aPipelines[Index] == nullptr)
						return false;
				}
			}
			return true;
		}

		bool CreateLayeredPrimitivePipelines(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount)
		{
			std::array<WGPUVertexAttribute, 3> aAttributes{};
			aAttributes[0].format = WGPUVertexFormat_Float32x2;
			aAttributes[0].offset = offsetof(CCommandBuffer::SVertexTex3DStream, m_Pos);
			aAttributes[0].shaderLocation = 0;
			aAttributes[1].format = WGPUVertexFormat_Unorm8x4;
			aAttributes[1].offset = offsetof(CCommandBuffer::SVertexTex3DStream, m_Color);
			aAttributes[1].shaderLocation = 1;
			aAttributes[2].format = WGPUVertexFormat_Float32x3;
			aAttributes[2].offset = offsetof(CCommandBuffer::SVertexTex3DStream, m_Tex);
			aAttributes[2].shaderLocation = 2;
			WGPUVertexBufferLayout VertexBuffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			VertexBuffer.arrayStride = sizeof(CCommandBuffer::SVertexTex3DStream);
			VertexBuffer.stepMode = WGPUVertexStepMode_Vertex;
			VertexBuffer.attributeCount = aAttributes.size();
			VertexBuffer.attributes = aAttributes.data();
			for(size_t Topology = 0; Topology < 2; ++Topology)
			{
				for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
				{
					for(size_t Textured = 0; Textured < 2; ++Textured)
					{
						WGPUBlendState BlendConfig = BlendState(Blend);
						WGPUColorTargetState ColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
						ColorTarget.format = Format;
						ColorTarget.blend = Blend == 0 ? nullptr : &BlendConfig;
						ColorTarget.writeMask = WGPUColorWriteMask_All;
						WGPUFragmentState Fragment = WGPU_FRAGMENT_STATE_INIT;
						Fragment.module = m_PrimitiveShader;
						Fragment.entryPoint = StringView(Textured != 0 ? "fs_layered" : "fs_layered_untextured");
						Fragment.targetCount = 1;
						Fragment.targets = &ColorTarget;
						WGPURenderPipelineDescriptor Descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
						Descriptor.label = StringView("DDNet WebGPU layered primitive pipeline");
						Descriptor.layout = Textured != 0 ? m_ArrayTexturePipelineLayout : m_UntexturedPipelineLayout;
						Descriptor.vertex.module = m_PrimitiveShader;
						Descriptor.vertex.entryPoint = StringView("vs_layered");
						Descriptor.vertex.bufferCount = 1;
						Descriptor.vertex.buffers = &VertexBuffer;
						Descriptor.primitive.topology = Topology == 0 ? WGPUPrimitiveTopology_LineList : WGPUPrimitiveTopology_TriangleList;
						Descriptor.primitive.frontFace = WGPUFrontFace_CCW;
						Descriptor.primitive.cullMode = WGPUCullMode_None;
						Descriptor.multisample.count = SampleCount;
						Descriptor.multisample.mask = UINT32_MAX;
						Descriptor.fragment = &Fragment;
						const size_t Index = (Topology * BLEND_MODE_COUNT + Blend) * 2 + Textured;
						Pipelines.m_aLayeredPrimitive[Index] = wgpuDeviceCreateRenderPipeline(m_Device, &Descriptor);
						if(Pipelines.m_aLayeredPrimitive[Index] == nullptr)
							return false;
					}
				}
			}
			return true;
		}

		bool CreateArrayColorPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, bool Transform, uint32_t SampleCount)
		{
			for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
			{
				for(size_t Textured = 0; Textured < 2; ++Textured)
				{
					std::array<WGPUVertexAttribute, 2> aAttributes{};
					aAttributes[0].format = WGPUVertexFormat_Float32x2;
					aAttributes[0].offset = 0;
					aAttributes[0].shaderLocation = 0;
					aAttributes[1].format = WGPUVertexFormat_Uint8x4;
					aAttributes[1].offset = sizeof(vec2);
					aAttributes[1].shaderLocation = 1;
					WGPUVertexBufferLayout VertexBuffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
					VertexBuffer.arrayStride = Textured != 0 ? sizeof(vec2) + sizeof(ubvec4) : sizeof(vec2);
					VertexBuffer.stepMode = WGPUVertexStepMode_Vertex;
					VertexBuffer.attributeCount = Textured != 0 ? 2 : 1;
					VertexBuffer.attributes = aAttributes.data();
					WGPUBlendState BlendConfig = BlendState(Blend);
					WGPUColorTargetState ColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
					ColorTarget.format = Format;
					ColorTarget.blend = Blend == 0 ? nullptr : &BlendConfig;
					ColorTarget.writeMask = WGPUColorWriteMask_All;
					WGPUFragmentState Fragment = WGPU_FRAGMENT_STATE_INIT;
					Fragment.module = m_PrimitiveShader;
					Fragment.entryPoint = StringView(Textured != 0 ? (Transform ? "fs_layered_border" : "fs_layered") : "fs_untextured");
					Fragment.targetCount = 1;
					Fragment.targets = &ColorTarget;
					WGPURenderPipelineDescriptor Descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
					Descriptor.label = StringView("DDNet WebGPU array-color pipeline");
					Descriptor.layout = Textured != 0 ? m_ArrayTexturePipelineLayout : m_UntexturedPipelineLayout;
					Descriptor.vertex.module = m_PrimitiveShader;
					Descriptor.vertex.entryPoint = StringView(Transform ? (Textured != 0 ? "vs_array_color_transform" : "vs_array_color_transform_untextured") : (Textured != 0 ? "vs_array_color" : "vs_array_color_untextured"));
					Descriptor.vertex.bufferCount = 1;
					Descriptor.vertex.buffers = &VertexBuffer;
					Descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
					Descriptor.primitive.frontFace = WGPUFrontFace_CCW;
					Descriptor.primitive.cullMode = WGPUCullMode_None;
					Descriptor.multisample.count = SampleCount;
					Descriptor.multisample.mask = UINT32_MAX;
					Descriptor.fragment = &Fragment;
					const size_t Index = Blend * 2 + Textured;
					aPipelines[Index] = wgpuDeviceCreateRenderPipeline(m_Device, &Descriptor);
					if(aPipelines[Index] == nullptr)
						return false;
				}
			}
			return true;
		}

		bool CreateQuadPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, bool Shared, uint32_t SampleCount)
		{
			for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
			{
				for(size_t Textured = 0; Textured < 2; ++Textured)
				{
					std::array<WGPUVertexAttribute, 3> aAttributes{};
					aAttributes[0].format = WGPUVertexFormat_Float32x4;
					aAttributes[0].offset = 0;
					aAttributes[0].shaderLocation = 0;
					aAttributes[1].format = WGPUVertexFormat_Unorm8x4;
					aAttributes[1].offset = sizeof(float) * 4;
					aAttributes[1].shaderLocation = 1;
					aAttributes[2].format = WGPUVertexFormat_Float32x2;
					aAttributes[2].offset = sizeof(float) * 4 + sizeof(ubvec4);
					aAttributes[2].shaderLocation = 2;
					WGPUVertexBufferLayout VertexBuffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
					VertexBuffer.arrayStride = sizeof(float) * 4 + sizeof(ubvec4) + (Textured != 0 ? sizeof(float) * 2 : 0);
					VertexBuffer.stepMode = WGPUVertexStepMode_Vertex;
					VertexBuffer.attributeCount = Textured != 0 ? 3 : 2;
					VertexBuffer.attributes = aAttributes.data();
					WGPUBlendState BlendConfig = BlendState(Blend);
					WGPUColorTargetState ColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
					ColorTarget.format = Format;
					ColorTarget.blend = Blend == 0 ? nullptr : &BlendConfig;
					ColorTarget.writeMask = WGPUColorWriteMask_All;
					WGPUFragmentState Fragment = WGPU_FRAGMENT_STATE_INIT;
					Fragment.module = m_PrimitiveShader;
					Fragment.entryPoint = StringView(Textured != 0 ? "fs_textured" : "fs_untextured");
					Fragment.targetCount = 1;
					Fragment.targets = &ColorTarget;
					WGPURenderPipelineDescriptor Descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
					Descriptor.label = StringView("DDNet WebGPU quad pipeline");
					Descriptor.layout = Shared ? (Textured != 0 ? m_PrimitivePipelineLayout : m_UntexturedPipelineLayout) : (Textured != 0 ? m_QuadTexturedPipelineLayout : m_QuadUntexturedPipelineLayout);
					Descriptor.vertex.module = m_PrimitiveShader;
					Descriptor.vertex.entryPoint = StringView(Shared ? (Textured != 0 ? "vs_quad_shared_textured" : "vs_quad_shared") : (Textured != 0 ? "vs_quad_per_item_textured" : "vs_quad_per_item"));
					Descriptor.vertex.bufferCount = 1;
					Descriptor.vertex.buffers = &VertexBuffer;
					Descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
					Descriptor.primitive.frontFace = WGPUFrontFace_CCW;
					Descriptor.primitive.cullMode = WGPUCullMode_None;
					Descriptor.multisample.count = SampleCount;
					Descriptor.multisample.mask = UINT32_MAX;
					Descriptor.fragment = &Fragment;
					const size_t Index = Blend * 2 + Textured;
					aPipelines[Index] = wgpuDeviceCreateRenderPipeline(m_Device, &Descriptor);
					if(aPipelines[Index] == nullptr)
						return false;
				}
			}
			return true;
		}

		bool CreateDualAtlasPipelines(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount)
		{
			std::array<WGPUVertexAttribute, 3> aAttributes{};
			aAttributes[0].format = WGPUVertexFormat_Float32x2;
			aAttributes[0].offset = offsetof(CCommandBuffer::SVertex, m_Pos);
			aAttributes[0].shaderLocation = 0;
			aAttributes[1].format = WGPUVertexFormat_Float32x2;
			aAttributes[1].offset = offsetof(CCommandBuffer::SVertex, m_Tex);
			aAttributes[1].shaderLocation = 1;
			aAttributes[2].format = WGPUVertexFormat_Unorm8x4;
			aAttributes[2].offset = offsetof(CCommandBuffer::SVertex, m_Color);
			aAttributes[2].shaderLocation = 2;
			WGPUVertexBufferLayout VertexBuffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			VertexBuffer.arrayStride = sizeof(CCommandBuffer::SVertex);
			VertexBuffer.stepMode = WGPUVertexStepMode_Vertex;
			VertexBuffer.attributeCount = aAttributes.size();
			VertexBuffer.attributes = aAttributes.data();
			for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
			{
				WGPUBlendState BlendConfig = BlendState(Blend);
				WGPUColorTargetState ColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
				ColorTarget.format = Format;
				ColorTarget.blend = Blend == 0 ? nullptr : &BlendConfig;
				ColorTarget.writeMask = WGPUColorWriteMask_All;
				WGPUFragmentState Fragment = WGPU_FRAGMENT_STATE_INIT;
				Fragment.module = m_PrimitiveShader;
				Fragment.entryPoint = StringView("fs_dual_atlas");
				Fragment.targetCount = 1;
				Fragment.targets = &ColorTarget;
				WGPURenderPipelineDescriptor Descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
				Descriptor.label = StringView("DDNet WebGPU dual-atlas pipeline");
				Descriptor.layout = m_DualTexturePipelineLayout;
				Descriptor.vertex.module = m_PrimitiveShader;
				Descriptor.vertex.entryPoint = StringView("vs_dual_atlas");
				Descriptor.vertex.bufferCount = 1;
				Descriptor.vertex.buffers = &VertexBuffer;
				Descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
				Descriptor.primitive.frontFace = WGPUFrontFace_CCW;
				Descriptor.primitive.cullMode = WGPUCullMode_None;
				Descriptor.multisample.count = SampleCount;
				Descriptor.multisample.mask = UINT32_MAX;
				Descriptor.fragment = &Fragment;
				Pipelines.m_aDualAtlas[Blend] = wgpuDeviceCreateRenderPipeline(m_Device, &Descriptor);
				if(Pipelines.m_aDualAtlas[Blend] == nullptr)
					return false;
			}
			return true;
		}

		bool CreatePipelineSet(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount)
		{
			if(Pipelines.m_PlanarYuv != nullptr)
			{
				wgpuRenderPipelineRelease(Pipelines.m_PlanarYuv);
				Pipelines.m_PlanarYuv = nullptr;
			}
			if(Pipelines.m_Blur != nullptr)
			{
				wgpuRenderPipelineRelease(Pipelines.m_Blur);
				Pipelines.m_Blur = nullptr;
			}
			for(auto &Pipeline : Pipelines.m_aPrimitive)
			{
				if(Pipeline != nullptr)
					wgpuRenderPipelineRelease(Pipeline);
				Pipeline = nullptr;
			}
			for(auto &Pipeline : Pipelines.m_aLayeredPrimitive)
			{
				if(Pipeline != nullptr)
					wgpuRenderPipelineRelease(Pipeline);
				Pipeline = nullptr;
			}
			for(auto *pPipelineArray : {&Pipelines.m_aUniformColor, &Pipelines.m_aInstanced, &Pipelines.m_aArrayColor, &Pipelines.m_aArrayColorTransform, &Pipelines.m_aQuadPerItem, &Pipelines.m_aQuadShared})
			{
				for(auto &Pipeline : *pPipelineArray)
				{
					if(Pipeline != nullptr)
						wgpuRenderPipelineRelease(Pipeline);
					Pipeline = nullptr;
				}
			}
			for(auto &Pipeline : Pipelines.m_aDualAtlas)
			{
				if(Pipeline != nullptr)
					wgpuRenderPipelineRelease(Pipeline);
				Pipeline = nullptr;
			}

			std::array<WGPUVertexAttribute, 3> aAttributes{};
			aAttributes[0].format = WGPUVertexFormat_Float32x2;
			aAttributes[0].offset = offsetof(CCommandBuffer::SVertex, m_Pos);
			aAttributes[0].shaderLocation = 0;
			aAttributes[1].format = WGPUVertexFormat_Float32x2;
			aAttributes[1].offset = offsetof(CCommandBuffer::SVertex, m_Tex);
			aAttributes[1].shaderLocation = 1;
			aAttributes[2].format = WGPUVertexFormat_Unorm8x4;
			aAttributes[2].offset = offsetof(CCommandBuffer::SVertex, m_Color);
			aAttributes[2].shaderLocation = 2;
			WGPUVertexBufferLayout VertexBufferLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			VertexBufferLayout.arrayStride = sizeof(CCommandBuffer::SVertex);
			VertexBufferLayout.stepMode = WGPUVertexStepMode_Vertex;
			VertexBufferLayout.attributeCount = aAttributes.size();
			VertexBufferLayout.attributes = aAttributes.data();

			for(size_t Topology = 0; Topology < 2; ++Topology)
			{
				for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
				{
					for(size_t Textured = 0; Textured < 2; ++Textured)
					{
						WGPUBlendState BlendState = WGPU_BLEND_STATE_INIT;
						BlendState.color.operation = WGPUBlendOperation_Add;
						BlendState.alpha.operation = WGPUBlendOperation_Add;
						if(Blend == 1)
						{
							BlendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
							BlendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
							BlendState.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
							BlendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
						}
						else if(Blend == 2)
						{
							BlendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
							BlendState.color.dstFactor = WGPUBlendFactor_One;
							BlendState.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
							BlendState.alpha.dstFactor = WGPUBlendFactor_One;
						}

						WGPUColorTargetState ColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
						ColorTarget.format = Format;
						ColorTarget.blend = Blend == 0 ? nullptr : &BlendState;
						ColorTarget.writeMask = WGPUColorWriteMask_All;
						WGPUFragmentState Fragment = WGPU_FRAGMENT_STATE_INIT;
						Fragment.module = m_PrimitiveShader;
						Fragment.entryPoint = StringView(Textured != 0 ? "fs_textured" : "fs_untextured");
						Fragment.targetCount = 1;
						Fragment.targets = &ColorTarget;
						WGPURenderPipelineDescriptor Descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
						Descriptor.label = StringView("DDNet WebGPU primitive pipeline");
						Descriptor.layout = Textured != 0 ? m_PrimitivePipelineLayout : m_UntexturedPipelineLayout;
						Descriptor.vertex.module = m_PrimitiveShader;
						Descriptor.vertex.entryPoint = StringView("vs_main");
						Descriptor.vertex.bufferCount = 1;
						Descriptor.vertex.buffers = &VertexBufferLayout;
						Descriptor.primitive.topology = Topology == 0 ? WGPUPrimitiveTopology_LineList : WGPUPrimitiveTopology_TriangleList;
						Descriptor.primitive.frontFace = WGPUFrontFace_CCW;
						Descriptor.primitive.cullMode = WGPUCullMode_None;
						Descriptor.multisample.count = SampleCount;
						Descriptor.multisample.mask = UINT32_MAX;
						Descriptor.fragment = &Fragment;
						const size_t Index = (Topology * BLEND_MODE_COUNT + Blend) * 2 + Textured;
						Pipelines.m_aPrimitive[Index] = wgpuDeviceCreateRenderPipeline(m_Device, &Descriptor);
						if(Pipelines.m_aPrimitive[Index] == nullptr)
							return false;
					}
				}
			}
			WGPUColorTargetState BlurTarget = WGPU_COLOR_TARGET_STATE_INIT;
			BlurTarget.format = Format;
			BlurTarget.writeMask = WGPUColorWriteMask_All;
			WGPUFragmentState BlurFragment = WGPU_FRAGMENT_STATE_INIT;
			BlurFragment.module = m_PrimitiveShader;
			BlurFragment.entryPoint = StringView("fs_blur");
			BlurFragment.targetCount = 1;
			BlurFragment.targets = &BlurTarget;
			WGPURenderPipelineDescriptor BlurDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
			BlurDescriptor.label = StringView("DDNet WebGPU blur pipeline");
			BlurDescriptor.layout = m_PrimitivePipelineLayout;
			BlurDescriptor.vertex.module = m_PrimitiveShader;
			BlurDescriptor.vertex.entryPoint = StringView("vs_main");
			BlurDescriptor.vertex.bufferCount = 1;
			BlurDescriptor.vertex.buffers = &VertexBufferLayout;
			BlurDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
			BlurDescriptor.primitive.frontFace = WGPUFrontFace_CCW;
			BlurDescriptor.primitive.cullMode = WGPUCullMode_None;
			BlurDescriptor.multisample.count = SampleCount;
			BlurDescriptor.multisample.mask = UINT32_MAX;
			BlurDescriptor.fragment = &BlurFragment;
			Pipelines.m_Blur = wgpuDeviceCreateRenderPipeline(m_Device, &BlurDescriptor);
			if(Pipelines.m_Blur == nullptr)
				return false;
			BlurFragment.entryPoint = StringView("fs_planar_yuv");
			BlurDescriptor.label = StringView("DDNet WebGPU planar YUV pipeline");
			Pipelines.m_PlanarYuv = wgpuDeviceCreateRenderPipeline(m_Device, &BlurDescriptor);
			if(Pipelines.m_PlanarYuv == nullptr)
				return false;
			return CreateBufferedPipelines(Pipelines.m_aUniformColor, Format, "vs_uniform_color", false, SampleCount) &&
			       CreateBufferedPipelines(Pipelines.m_aInstanced, Format, "vs_instanced", true, SampleCount) &&
			       CreateLayeredPrimitivePipelines(Pipelines, Format, SampleCount) &&
			       CreateArrayColorPipelines(Pipelines.m_aArrayColor, Format, false, SampleCount) &&
			       CreateArrayColorPipelines(Pipelines.m_aArrayColorTransform, Format, true, SampleCount) &&
			       CreateQuadPipelines(Pipelines.m_aQuadPerItem, Format, false, SampleCount) &&
			       CreateQuadPipelines(Pipelines.m_aQuadShared, Format, true, SampleCount) &&
			       CreateDualAtlasPipelines(Pipelines, Format, SampleCount);
		}

		bool CreatePrimitivePipelines()
		{
			if(m_BackendMode == EGraphicsBackendMode::OFFSCREEN)
				return CreatePipelineSet(m_aPipelineSets[1], WGPUTextureFormat_RGBA8Unorm, 1);
			return CreatePipelineSet(m_aPipelineSets[0], m_SurfaceFormat, SampleCount()) && CreatePipelineSet(m_aPipelineSets[1], WGPUTextureFormat_RGBA8Unorm, 1);
		}

		bool ApplyMultiSampling()
		{
			if(m_MultiSamplingCount == m_NextMultiSamplingCount)
				return true;
			m_MultiSamplingCount = m_NextMultiSamplingCount;
			ReleaseMultisampleTarget(m_SurfaceMultisampleTexture, m_SurfaceMultisampleView, m_SurfaceMultisampleMemorySize);
			for(auto &Texture : m_vTextures)
				ReleaseMultisampleTarget(Texture.m_MultisampleTexture, Texture.m_MultisampleView, Texture.m_MultisampleMemorySize);
			m_RenderPassLoadOp = WGPULoadOp_Clear;
			if(CreatePrimitivePipelines())
				return true;
			SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to recreate pipelines for multisampling");
			return false;
		}

		bool CreateDrawResources()
		{
			static constexpr char s_aShader[] = R"(
struct PrimitiveTransform {
	scale: vec2f,
	translate: vec2f,
	color: vec4f,
	rotation_center: vec2f,
	rotation: f32,
	alpha_texture: u32,
	vertex_offset: vec2f,
	vertex_scale: vec2f,
	quad_base: u32,
	texture_size: f32,
	padding: vec2f,
	secondary_color: vec4f,
};
@group(0) @binding(0) var<uniform> transform: PrimitiveTransform;
struct VertexOutput {
	@builtin(position) position: vec4f,
	@location(0) uv: vec2f,
	@location(1) color: vec4f,
};
@vertex fn vs_main(@location(0) position: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f) -> VertexOutput {
	var output: VertexOutput;
	output.position = vec4f(position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = uv;
	output.color = color;
	return output;
}
fn rotate(position: vec2f, center: vec2f, rotation: f32) -> vec2f {
	let offset = position - center;
	let sine = sin(rotation);
	let cosine = cos(rotation);
	return vec2f(offset.x * cosine - offset.y * sine, offset.x * sine + offset.y * cosine) + center;
}
@vertex fn vs_uniform_color(@location(0) position: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f) -> VertexOutput {
	var output: VertexOutput;
	let final_position = rotate(position, transform.rotation_center, transform.rotation);
	output.position = vec4f(final_position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = uv;
	output.color = color * transform.color;
	return output;
}
@vertex fn vs_instanced(@location(0) position: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f, @location(3) instance_position: vec2f, @location(4) scale_rotation: vec2f) -> VertexOutput {
	var output: VertexOutput;
	var final_position = rotate(position, transform.rotation_center, scale_rotation.y);
	final_position = final_position * scale_rotation.x + instance_position;
	output.position = vec4f(final_position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = uv;
	output.color = color * transform.color;
	return output;
}
struct LayeredVertexOutput {
	@builtin(position) position: vec4f,
	@location(0) @interpolate(linear) uv: vec3f,
	@location(1) color: vec4f,
};
struct LayeredBorderVertexOutput {
	@builtin(position) position: vec4f,
	@location(0) @interpolate(linear, centroid) uv: vec3f,
	@location(1) color: vec4f,
};
@vertex fn vs_layered(@location(0) position: vec2f, @location(1) color: vec4f, @location(2) uv: vec3f) -> LayeredVertexOutput {
	var output: LayeredVertexOutput;
	output.position = vec4f(position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = uv;
	output.color = color;
	return output;
}
@vertex fn vs_array_color(@location(0) position: vec2f, @location(1) uv: vec4u) -> LayeredVertexOutput {
	var output: LayeredVertexOutput;
	output.position = vec4f(position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = vec3f(vec2f(uv.xy), f32(uv.z));
	output.color = transform.color;
	return output;
}
@vertex fn vs_array_color_transform(@location(0) position: vec2f, @location(1) uv: vec4u) -> LayeredBorderVertexOutput {
	var output: LayeredBorderVertexOutput;
	let vertex_position = position * transform.vertex_scale + transform.vertex_offset;
	output.position = vec4f(vertex_position * transform.scale + transform.translate, 0.0, 1.0);
	let texture_scale = select(transform.vertex_scale, transform.vertex_scale.yx, uv.w > 0u);
	output.uv = vec3f(vec2f(uv.xy) * texture_scale, f32(uv.z));
	output.color = transform.color;
	return output;
}
@vertex fn vs_array_color_untextured(@location(0) position: vec2f) -> VertexOutput {
	var output: VertexOutput;
	output.position = vec4f(position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = vec2f(0.0, 0.0);
	output.color = transform.color;
	return output;
}
@vertex fn vs_array_color_transform_untextured(@location(0) position: vec2f) -> VertexOutput {
	var output: VertexOutput;
	let vertex_position = position * transform.vertex_scale + transform.vertex_offset;
	output.position = vec4f(vertex_position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = vec2f(0.0, 0.0);
	output.color = transform.color;
	return output;
}
struct QuadTransform {
	color: vec4f,
	offset: vec2f,
	rotation: f32,
	padding: f32,
};
struct QuadTransforms {
	entries: array<QuadTransform, 256>,
};
@group(2) @binding(0) var<uniform> quad_transforms: QuadTransforms;
fn quad_vertex(position: vec4f, color: vec4f, uv: vec2f, quad: QuadTransform) -> VertexOutput {
	var output: VertexOutput;
	var final_position = position.xy;
	if quad.rotation != 0.0 {
		final_position = rotate(final_position, position.zw, quad.rotation);
	}
	final_position += quad.offset;
	output.position = vec4f(final_position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = uv;
	output.color = color * quad.color;
	return output;
}
@vertex fn vs_quad_shared(@location(0) position: vec4f, @location(1) color: vec4f) -> VertexOutput {
	let quad = QuadTransform(transform.color, transform.vertex_offset, transform.rotation, 0.0);
	return quad_vertex(position, color, vec2f(0.0), quad);
}
@vertex fn vs_quad_shared_textured(@location(0) position: vec4f, @location(1) color: vec4f, @location(2) uv: vec2f) -> VertexOutput {
	let quad = QuadTransform(transform.color, transform.vertex_offset, transform.rotation, 0.0);
	return quad_vertex(position, color, uv, quad);
}
@vertex fn vs_quad_per_item(@builtin(vertex_index) vertex_index: u32, @location(0) position: vec4f, @location(1) color: vec4f) -> VertexOutput {
	let quad_index = vertex_index / 4u - transform.quad_base;
	return quad_vertex(position, color, vec2f(0.0), quad_transforms.entries[quad_index]);
}
@vertex fn vs_quad_per_item_textured(@builtin(vertex_index) vertex_index: u32, @location(0) position: vec4f, @location(1) color: vec4f, @location(2) uv: vec2f) -> VertexOutput {
	let quad_index = vertex_index / 4u - transform.quad_base;
	return quad_vertex(position, color, uv, quad_transforms.entries[quad_index]);
}
@group(1) @binding(0) var image_sampler: sampler;
@group(1) @binding(1) var image_texture: texture_2d<f32>;
@group(1) @binding(2) var image_array_texture: texture_2d_array<f32>;
@group(1) @binding(3) var secondary_sampler: sampler;
@group(1) @binding(4) var secondary_texture: texture_2d<f32>;
@fragment fn fs_untextured(input: VertexOutput) -> @location(0) vec4f {
	return input.color;
}
@fragment fn fs_textured(input: VertexOutput) -> @location(0) vec4f {
	let sample = textureSample(image_texture, image_sampler, input.uv);
	let texture_color = select(sample, vec4f(1.0, 1.0, 1.0, sample.r), transform.alpha_texture != 0u);
	return texture_color * input.color;
}
@fragment fn fs_blur(input: VertexOutput) -> @location(0) vec4f {
	let texel_offset = input.color.rg / vec2f(textureDimensions(image_texture));
	var color = textureSample(image_texture, image_sampler, input.uv) * 0.2270270270;
	color += textureSample(image_texture, image_sampler, input.uv + texel_offset * 1.3846153846) * 0.3162162162;
	color += textureSample(image_texture, image_sampler, input.uv - texel_offset * 1.3846153846) * 0.3162162162;
	color += textureSample(image_texture, image_sampler, input.uv + texel_offset * 3.2307692308) * 0.0702702703;
	color += textureSample(image_texture, image_sampler, input.uv - texel_offset * 3.2307692308) * 0.0702702703;
	return color;
}
// Turns a rendered frame into the planar YUV layout an encoder wants. See
// shader/vulkan/planar_yuv.frag for what the layout is; input.color.r picks
// between interleaved NV12 and three separate planes, the way the blur above
// takes its axis from the same place.
fn yuv_luma(color: vec3f) -> f32 {
	return (16.0 + 219.0 * dot(color, vec3f(0.2126, 0.7152, 0.0722))) / 255.0;
}
fn yuv_chroma_blue(color: vec3f) -> f32 {
	return (128.0 + 224.0 * dot(color, vec3f(-0.1146, -0.3854, 0.5))) / 255.0;
}
fn yuv_chroma_red(color: vec3f) -> f32 {
	return (128.0 + 224.0 * dot(color, vec3f(0.5, -0.4542, -0.0458))) / 255.0;
}
fn yuv_block(chroma: vec2i) -> vec3f {
	let origin = chroma * 2;
	return 0.25 * (textureLoad(image_texture, origin, 0).rgb +
		textureLoad(image_texture, origin + vec2i(1, 0), 0).rgb +
		textureLoad(image_texture, origin + vec2i(0, 1), 0).rgb +
		textureLoad(image_texture, origin + vec2i(1, 1), 0).rgb);
}
@fragment fn fs_planar_yuv(input: VertexOutput) -> @location(0) vec4f {
	let source_size = vec2i(textureDimensions(image_texture));
	let texel = vec2i(input.position.xy);
	let planar = input.color.r > 0.5;
	let chroma_width = source_size.x / 2;
	let second_plane_row = source_size.y + source_size.y / 4;
	var result = vec4f(0.0);
	for(var component = 0; component < 4; component++) {
		let byte = texel.x * 4 + component;
		if texel.y < source_size.y {
			result[component] = yuv_luma(textureLoad(image_texture, vec2i(byte, texel.y), 0).rgb);
			continue;
		}
		if !planar {
			let block = yuv_block(vec2i(byte / 2, texel.y - source_size.y));
			result[component] = select(yuv_chroma_red(block), yuv_chroma_blue(block), byte % 2 == 0);
			continue;
		}
		let second = texel.y >= second_plane_row;
		let plane_row = texel.y - select(source_size.y, second_plane_row, second);
		let lower = byte >= chroma_width;
		let block = yuv_block(vec2i(select(byte, byte - chroma_width, lower), plane_row * 2 + select(0, 1, lower)));
		result[component] = select(yuv_chroma_blue(block), yuv_chroma_red(block), second);
	}
	return result;
}
@fragment fn fs_layered(input: LayeredVertexOutput) -> @location(0) vec4f {
	let sample = textureSample(image_array_texture, image_sampler, input.uv.xy, i32(input.uv.z));
	let texture_color = select(sample, vec4f(1.0, 1.0, 1.0, sample.r), transform.alpha_texture != 0u);
	return texture_color * input.color;
}
@fragment fn fs_layered_untextured(input: LayeredVertexOutput) -> @location(0) vec4f {
	return input.color;
}
@fragment fn fs_layered_border(input: LayeredBorderVertexOutput) -> @location(0) vec4f {
	let coordinates = fract(input.uv.xy);
	let sample = textureSampleGrad(image_array_texture, image_sampler, coordinates, i32(input.uv.z), dpdx(input.uv.xy), dpdy(input.uv.xy));
	let texture_color = select(sample, vec4f(1.0, 1.0, 1.0, sample.r), transform.alpha_texture != 0u);
	return texture_color * input.color;
}
@vertex fn vs_dual_atlas(@location(0) position: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f) -> VertexOutput {
	var output: VertexOutput;
	output.position = vec4f(position * transform.scale + transform.translate, 0.0, 1.0);
	output.uv = uv / transform.texture_size;
	output.color = color;
	return output;
}
@fragment fn fs_dual_atlas(input: VertexOutput) -> @location(0) vec4f {
	let primary = transform.color * input.color * vec4f(1.0, 1.0, 1.0, textureSample(image_texture, image_sampler, input.uv).r);
	let secondary = transform.secondary_color * vec4f(1.0, 1.0, 1.0, textureSample(secondary_texture, secondary_sampler, input.uv).r);
	let outline = vec4f(secondary.rgb * secondary.a, secondary.a) * (1.0 - primary.a);
	let alpha = outline.a + primary.a;
	if alpha > 0.0 {
		return vec4f((outline.rgb + primary.rgb * primary.a) / alpha, alpha);
	}
	return vec4f(0.0);
}
)";
			WGPUShaderSourceWGSL ShaderSource{};
			ShaderSource.chain.sType = WGPUSType_ShaderSourceWGSL;
			ShaderSource.code = StringView(s_aShader);
			WGPUShaderModuleDescriptor ShaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
			ShaderDescriptor.nextInChain = &ShaderSource.chain;
			ShaderDescriptor.label = StringView("DDNet WebGPU primitive shader");
			m_PrimitiveShader = wgpuDeviceCreateShaderModule(m_Device, &ShaderDescriptor);
			if(m_PrimitiveShader == nullptr)
				return false;

			WGPUBindGroupLayoutEntry UniformEntry{};
			UniformEntry.binding = 0;
			UniformEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
			UniformEntry.buffer.type = WGPUBufferBindingType_Uniform;
			UniformEntry.buffer.hasDynamicOffset = WGPU_TRUE;
			UniformEntry.buffer.minBindingSize = sizeof(SPrimitiveTransform);
			WGPUBindGroupLayoutDescriptor UniformLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
			UniformLayoutDescriptor.label = StringView("DDNet WebGPU primitive uniforms");
			UniformLayoutDescriptor.entryCount = 1;
			UniformLayoutDescriptor.entries = &UniformEntry;
			m_UniformBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &UniformLayoutDescriptor);
			WGPUBindGroupLayoutDescriptor EmptyLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
			EmptyLayoutDescriptor.label = StringView("DDNet WebGPU empty binding");
			m_EmptyBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &EmptyLayoutDescriptor);
			std::array<WGPUBindGroupLayoutEntry, 2> aTextureEntries{};
			aTextureEntries[0].binding = 0;
			aTextureEntries[0].visibility = WGPUShaderStage_Fragment;
			aTextureEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;
			aTextureEntries[1].binding = 1;
			aTextureEntries[1].visibility = WGPUShaderStage_Fragment;
			aTextureEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
			aTextureEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
			WGPUBindGroupLayoutDescriptor TextureLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
			TextureLayoutDescriptor.label = StringView("DDNet WebGPU sampled texture");
			TextureLayoutDescriptor.entryCount = aTextureEntries.size();
			TextureLayoutDescriptor.entries = aTextureEntries.data();
			m_TextureBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &TextureLayoutDescriptor);
			aTextureEntries[1].binding = 2;
			aTextureEntries[1].texture.viewDimension = WGPUTextureViewDimension_2DArray;
			TextureLayoutDescriptor.label = StringView("DDNet WebGPU sampled array texture");
			m_ArrayTextureBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &TextureLayoutDescriptor);
			std::array<WGPUBindGroupLayoutEntry, 4> aDualTextureEntries{};
			aDualTextureEntries[0] = aTextureEntries[0];
			aDualTextureEntries[1] = aTextureEntries[1];
			aDualTextureEntries[1].binding = 1;
			aDualTextureEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
			aDualTextureEntries[2] = aTextureEntries[0];
			aDualTextureEntries[2].binding = 3;
			aDualTextureEntries[3] = aDualTextureEntries[1];
			aDualTextureEntries[3].binding = 4;
			WGPUBindGroupLayoutDescriptor DualTextureLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
			DualTextureLayoutDescriptor.label = StringView("DDNet WebGPU dual sampled texture");
			DualTextureLayoutDescriptor.entryCount = aDualTextureEntries.size();
			DualTextureLayoutDescriptor.entries = aDualTextureEntries.data();
			m_DualTextureBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &DualTextureLayoutDescriptor);
			WGPUBindGroupLayoutEntry QuadEntry{};
			QuadEntry.binding = 0;
			QuadEntry.visibility = WGPUShaderStage_Vertex;
			QuadEntry.buffer.type = WGPUBufferBindingType_Uniform;
			QuadEntry.buffer.hasDynamicOffset = WGPU_TRUE;
			QuadEntry.buffer.minBindingSize = GRAPHICS_MAX_QUADS_RENDER_COUNT * sizeof(CCommandBuffer::SDrawDataQuadTransform);
			WGPUBindGroupLayoutDescriptor QuadLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
			QuadLayoutDescriptor.label = StringView("DDNet WebGPU quad transforms");
			QuadLayoutDescriptor.entryCount = 1;
			QuadLayoutDescriptor.entries = &QuadEntry;
			m_QuadBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &QuadLayoutDescriptor);
			if(m_UniformBindGroupLayout == nullptr || m_EmptyBindGroupLayout == nullptr || m_TextureBindGroupLayout == nullptr || m_ArrayTextureBindGroupLayout == nullptr || m_DualTextureBindGroupLayout == nullptr || m_QuadBindGroupLayout == nullptr)
				return false;

			const std::array<WGPUBindGroupLayout, 2> aLayouts = {m_UniformBindGroupLayout, m_TextureBindGroupLayout};
			const std::array<WGPUBindGroupLayout, 2> aArrayLayouts = {m_UniformBindGroupLayout, m_ArrayTextureBindGroupLayout};
			const std::array<WGPUBindGroupLayout, 2> aDualLayouts = {m_UniformBindGroupLayout, m_DualTextureBindGroupLayout};
			const std::array<WGPUBindGroupLayout, 3> aQuadTexturedLayouts = {m_UniformBindGroupLayout, m_TextureBindGroupLayout, m_QuadBindGroupLayout};
			const std::array<WGPUBindGroupLayout, 3> aQuadUntexturedLayouts = {m_UniformBindGroupLayout, m_EmptyBindGroupLayout, m_QuadBindGroupLayout};
			WGPUPipelineLayoutDescriptor UntexturedPipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
			UntexturedPipelineLayoutDescriptor.label = StringView("DDNet WebGPU untextured primitive layout");
			UntexturedPipelineLayoutDescriptor.bindGroupLayoutCount = 1;
			UntexturedPipelineLayoutDescriptor.bindGroupLayouts = &m_UniformBindGroupLayout;
			m_UntexturedPipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &UntexturedPipelineLayoutDescriptor);
			WGPUPipelineLayoutDescriptor PipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
			PipelineLayoutDescriptor.label = StringView("DDNet WebGPU primitive layout");
			PipelineLayoutDescriptor.bindGroupLayoutCount = aLayouts.size();
			PipelineLayoutDescriptor.bindGroupLayouts = aLayouts.data();
			m_PrimitivePipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
			PipelineLayoutDescriptor.label = StringView("DDNet WebGPU array texture layout");
			PipelineLayoutDescriptor.bindGroupLayouts = aArrayLayouts.data();
			m_ArrayTexturePipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
			PipelineLayoutDescriptor.label = StringView("DDNet WebGPU dual texture layout");
			PipelineLayoutDescriptor.bindGroupLayouts = aDualLayouts.data();
			m_DualTexturePipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
			PipelineLayoutDescriptor.label = StringView("DDNet WebGPU textured quad layout");
			PipelineLayoutDescriptor.bindGroupLayoutCount = aQuadTexturedLayouts.size();
			PipelineLayoutDescriptor.bindGroupLayouts = aQuadTexturedLayouts.data();
			m_QuadTexturedPipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
			PipelineLayoutDescriptor.label = StringView("DDNet WebGPU untextured quad layout");
			PipelineLayoutDescriptor.bindGroupLayouts = aQuadUntexturedLayouts.data();
			m_QuadUntexturedPipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
			if(m_UntexturedPipelineLayout == nullptr || m_PrimitivePipelineLayout == nullptr || m_ArrayTexturePipelineLayout == nullptr || m_DualTexturePipelineLayout == nullptr || m_QuadTexturedPipelineLayout == nullptr || m_QuadUntexturedPipelineLayout == nullptr)
				return false;

			static const WGPUAddressMode s_aaSamplerAddressModes[static_cast<size_t>(ESamplerKind::COUNT)][3] = {
				{WGPUAddressMode_Repeat, WGPUAddressMode_Repeat, WGPUAddressMode_Repeat},
				{WGPUAddressMode_ClampToEdge, WGPUAddressMode_ClampToEdge, WGPUAddressMode_ClampToEdge},
				{WGPUAddressMode_ClampToEdge, WGPUAddressMode_ClampToEdge, WGPUAddressMode_MirrorRepeat},
			};
			for(size_t i = 0; i < m_aSamplers.size(); ++i)
			{
				WGPUSamplerDescriptor SamplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
				SamplerDescriptor.label = StringView("DDNet WebGPU sampler");
				SamplerDescriptor.addressModeU = s_aaSamplerAddressModes[i][0];
				SamplerDescriptor.addressModeV = s_aaSamplerAddressModes[i][1];
				SamplerDescriptor.addressModeW = s_aaSamplerAddressModes[i][2];
				SamplerDescriptor.magFilter = WGPUFilterMode_Linear;
				SamplerDescriptor.minFilter = WGPUFilterMode_Linear;
				SamplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Linear;
				SamplerDescriptor.maxAnisotropy = 1;
				m_aSamplers[i] = wgpuDeviceCreateSampler(m_Device, &SamplerDescriptor);
				if(m_aSamplers[i] == nullptr)
					return false;
			}

			WGPUBufferDescriptor BufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			BufferDescriptor.label = StringView("DDNet WebGPU frame stream");
			BufferDescriptor.size = STREAM_BUFFER_SIZE * UPLOAD_BUFFER_SLOT_COUNT;
			BufferDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex | WGPUBufferUsage_Index;
			m_StreamBuffer = wgpuDeviceCreateBuffer(m_Device, &BufferDescriptor);
			BufferDescriptor.label = StringView("DDNet WebGPU frame uniforms");
			BufferDescriptor.size = UNIFORM_BUFFER_SIZE * UPLOAD_BUFFER_SLOT_COUNT;
			BufferDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform;
			m_UniformBuffer = wgpuDeviceCreateBuffer(m_Device, &BufferDescriptor);
			if(m_StreamBuffer == nullptr || m_UniformBuffer == nullptr)
				return false;
			m_pStreamMemoryUsage->fetch_add((STREAM_BUFFER_SIZE + UNIFORM_BUFFER_SIZE) * UPLOAD_BUFFER_SLOT_COUNT, std::memory_order_relaxed);

			WGPUBindGroupEntry UniformBindGroupEntry{};
			UniformBindGroupEntry.binding = 0;
			UniformBindGroupEntry.buffer = m_UniformBuffer;
			UniformBindGroupEntry.offset = 0;
			UniformBindGroupEntry.size = sizeof(SPrimitiveTransform);
			WGPUBindGroupDescriptor UniformBindGroupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			UniformBindGroupDescriptor.label = StringView("DDNet WebGPU primitive uniform binding");
			UniformBindGroupDescriptor.layout = m_UniformBindGroupLayout;
			UniformBindGroupDescriptor.entryCount = 1;
			UniformBindGroupDescriptor.entries = &UniformBindGroupEntry;
			m_UniformBindGroup = wgpuDeviceCreateBindGroup(m_Device, &UniformBindGroupDescriptor);
			UniformBindGroupEntry.size = GRAPHICS_MAX_QUADS_RENDER_COUNT * sizeof(CCommandBuffer::SDrawDataQuadTransform);
			UniformBindGroupDescriptor.label = StringView("DDNet WebGPU quad transform binding");
			UniformBindGroupDescriptor.layout = m_QuadBindGroupLayout;
			m_QuadBindGroup = wgpuDeviceCreateBindGroup(m_Device, &UniformBindGroupDescriptor);
			if(m_UniformBindGroup == nullptr || m_QuadBindGroup == nullptr)
				return false;

			WGPULimits Limits = WGPU_LIMITS_INIT;
			if(wgpuDeviceGetLimits(m_Device, &Limits) == WGPUStatus_Success && Limits.minUniformBufferOffsetAlignment != WGPU_LIMIT_U32_UNDEFINED && Limits.minUniformBufferOffsetAlignment != 0)
				m_UniformAlignment = Limits.minUniformBufferOffsetAlignment;
			return CreatePrimitivePipelines();
		}

		void DestroyDrawResources()
		{
			for(auto &Buffer : m_vBuffers)
				ReleaseBuffer(Buffer);
			m_vBuffers.clear();
			m_BufferHandles.Clear();
			m_vBufferContainers.clear();
			m_BufferContainerHandles.Clear();
			for(auto &Binding : m_vTextureBindings)
				ReleaseTextureBinding(Binding);
			m_vTextureBindings.clear();
			m_TextureBindingHandles.Clear();
			for(auto &Texture : m_vTextures)
				ReleaseTexture(Texture);
			m_vTextures.clear();
			m_TextureHandles.Clear();
			m_vPipelines.clear();
			m_PipelineHandles.Clear();
			for(auto &Pipelines : m_aPipelineSets)
			{
				for(auto *pPipelineArray : {&Pipelines.m_aPrimitive, &Pipelines.m_aLayeredPrimitive})
				{
					for(auto &Pipeline : *pPipelineArray)
						if(Pipeline != nullptr)
							wgpuRenderPipelineRelease(Pipeline);
				}
				for(auto *pPipelineArray : {&Pipelines.m_aUniformColor, &Pipelines.m_aInstanced, &Pipelines.m_aArrayColor, &Pipelines.m_aArrayColorTransform, &Pipelines.m_aQuadPerItem, &Pipelines.m_aQuadShared})
				{
					for(auto &Pipeline : *pPipelineArray)
						if(Pipeline != nullptr)
							wgpuRenderPipelineRelease(Pipeline);
				}
				for(auto &Pipeline : Pipelines.m_aDualAtlas)
					if(Pipeline != nullptr)
						wgpuRenderPipelineRelease(Pipeline);
				if(Pipelines.m_Blur != nullptr)
					wgpuRenderPipelineRelease(Pipelines.m_Blur);
				if(Pipelines.m_PlanarYuv != nullptr)
					wgpuRenderPipelineRelease(Pipelines.m_PlanarYuv);
				Pipelines = {};
			}
			if(m_QuadBindGroup != nullptr)
				wgpuBindGroupRelease(m_QuadBindGroup);
			if(m_UniformBindGroup != nullptr)
				wgpuBindGroupRelease(m_UniformBindGroup);
			if(m_UniformBuffer != nullptr)
				wgpuBufferRelease(m_UniformBuffer);
			if(m_StreamBuffer != nullptr)
				wgpuBufferRelease(m_StreamBuffer);
			if(m_pStreamMemoryUsage != nullptr)
				m_pStreamMemoryUsage->store(0, std::memory_order_relaxed);
			for(auto &Sampler : m_aSamplers)
			{
				if(Sampler != nullptr)
					wgpuSamplerRelease(Sampler);
				Sampler = nullptr;
			}
			if(m_PrimitivePipelineLayout != nullptr)
				wgpuPipelineLayoutRelease(m_PrimitivePipelineLayout);
			if(m_ArrayTexturePipelineLayout != nullptr)
				wgpuPipelineLayoutRelease(m_ArrayTexturePipelineLayout);
			if(m_DualTexturePipelineLayout != nullptr)
				wgpuPipelineLayoutRelease(m_DualTexturePipelineLayout);
			if(m_QuadTexturedPipelineLayout != nullptr)
				wgpuPipelineLayoutRelease(m_QuadTexturedPipelineLayout);
			if(m_QuadUntexturedPipelineLayout != nullptr)
				wgpuPipelineLayoutRelease(m_QuadUntexturedPipelineLayout);
			if(m_UntexturedPipelineLayout != nullptr)
				wgpuPipelineLayoutRelease(m_UntexturedPipelineLayout);
			if(m_TextureBindGroupLayout != nullptr)
				wgpuBindGroupLayoutRelease(m_TextureBindGroupLayout);
			if(m_ArrayTextureBindGroupLayout != nullptr)
				wgpuBindGroupLayoutRelease(m_ArrayTextureBindGroupLayout);
			if(m_DualTextureBindGroupLayout != nullptr)
				wgpuBindGroupLayoutRelease(m_DualTextureBindGroupLayout);
			if(m_QuadBindGroupLayout != nullptr)
				wgpuBindGroupLayoutRelease(m_QuadBindGroupLayout);
			if(m_EmptyBindGroupLayout != nullptr)
				wgpuBindGroupLayoutRelease(m_EmptyBindGroupLayout);
			if(m_UniformBindGroupLayout != nullptr)
				wgpuBindGroupLayoutRelease(m_UniformBindGroupLayout);
			if(m_PrimitiveShader != nullptr)
				wgpuShaderModuleRelease(m_PrimitiveShader);
			m_QuadBindGroup = nullptr;
			m_UniformBindGroup = nullptr;
			m_UniformBuffer = nullptr;
			m_StreamBuffer = nullptr;
			m_PrimitivePipelineLayout = nullptr;
			m_ArrayTexturePipelineLayout = nullptr;
			m_DualTexturePipelineLayout = nullptr;
			m_QuadTexturedPipelineLayout = nullptr;
			m_QuadUntexturedPipelineLayout = nullptr;
			m_UntexturedPipelineLayout = nullptr;
			m_TextureBindGroupLayout = nullptr;
			m_ArrayTextureBindGroupLayout = nullptr;
			m_DualTextureBindGroupLayout = nullptr;
			m_QuadBindGroupLayout = nullptr;
			m_EmptyBindGroupLayout = nullptr;
			m_UniformBindGroupLayout = nullptr;
			m_PrimitiveShader = nullptr;
			m_StreamOffset = 0;
			m_UniformOffset = 0;
			m_UploadBufferSlot = 0;
			m_aUploadBufferResults = {};
		}

		bool CreateTexture(const CCommandBuffer::SCommand_Texture_Create *pCommand)
		{
			const auto &Desc = pCommand->m_Desc;
			const bool CreateArray = Desc.m_Layering == IGraphics::ETextureLayering::ARRAY_2D;
			const bool ColorTarget = Desc.HasUsage(IGraphics::TEXTURE_USAGE_COLOR_TARGET);
			if(!Desc.IsValid() || (!Desc.m_Create2D && !CreateArray) || (Desc.m_Layering != IGraphics::ETextureLayering::NONE && !CreateArray) || (!ColorTarget && pCommand->m_pData == nullptr) || Desc.m_Width > std::numeric_limits<int>::max() || Desc.m_Height > std::numeric_limits<int>::max() || !m_TextureHandles.Activate(pCommand->m_Texture))
				return true;
			if(static_cast<size_t>(pCommand->m_Texture.Id()) >= m_vTextures.size())
				m_vTextures.resize(pCommand->m_Texture.Id() + 1);
			auto &Texture = m_vTextures[pCommand->m_Texture.Id()];
			Texture.m_Width = Desc.m_Width;
			Texture.m_Height = Desc.m_Height;
			Texture.m_Format = Desc.m_Format;
			Texture.m_Usage = Desc.m_Usage;
			const size_t PixelSize = Desc.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? 4 : 1;
			const WGPUTextureFormat Format = Desc.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_R8Unorm;
			size_t MemorySize = 0;
			auto CreateNativeTexture = [&](WGPUTexture &NativeTexture, WGPUTextureView &View, std::array<WGPUBindGroup, 2> &aBindGroups, uint32_t Width, uint32_t Height, uint32_t Layers, WGPUBindGroupLayout Layout, uint32_t TextureBinding, const uint8_t *pData) {
				const uint32_t MipCount = Desc.m_Mipmaps == IGraphics::ETextureMipmaps::GENERATE ? std::bit_width(std::max(Width, Height)) : 1;
				WGPUTextureDescriptor TextureDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
				TextureDescriptor.label = StringView("DDNet WebGPU sampled texture");
				TextureDescriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | (ColorTarget ? WGPUTextureUsage_RenderAttachment : WGPUTextureUsage_None) | (Desc.HasUsage(IGraphics::TEXTURE_USAGE_COPY_SOURCE) ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_None);
				TextureDescriptor.dimension = WGPUTextureDimension_2D;
				TextureDescriptor.size = {Width, Height, Layers};
				TextureDescriptor.format = Format;
				TextureDescriptor.mipLevelCount = MipCount;
				TextureDescriptor.sampleCount = 1;
				NativeTexture = wgpuDeviceCreateTexture(m_Device, &TextureDescriptor);
				WGPUTextureViewDescriptor ViewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
				const bool TextureArray = Layers != 1;
				ViewDescriptor.dimension = TextureArray ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
				ViewDescriptor.arrayLayerCount = Layers;
				View = NativeTexture == nullptr ? nullptr : wgpuTextureCreateView(NativeTexture, &ViewDescriptor);
				if(View == nullptr || !CreateTextureBindGroups(View, Layout, TextureBinding, aBindGroups, TextureArray))
					return false;
				std::vector<uint8_t> vMipData;
				for(uint32_t Mip = 0; Mip < MipCount; ++Mip)
				{
					const size_t DataSize = static_cast<size_t>(Width) * Height * Layers * PixelSize;
					MemorySize += DataSize;
					if(pData != nullptr)
					{
						WGPUTexelCopyTextureInfo Destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
						Destination.texture = NativeTexture;
						Destination.mipLevel = Mip;
						Destination.aspect = WGPUTextureAspect_All;
						WGPUTexelCopyBufferLayout UploadLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
						UploadLayout.bytesPerRow = static_cast<size_t>(Width) * PixelSize;
						UploadLayout.rowsPerImage = Height;
						const WGPUExtent3D Extent{Width, Height, Layers};
						wgpuQueueWriteTexture(m_Queue, &Destination, pData, DataSize, &UploadLayout, &Extent);
					}
					if(Mip + 1 < MipCount)
					{
						vMipData = DownsampleMip(pData, Width, Height, Layers, PixelSize);
						pData = vMipData.data();
						Width = std::max(Width / 2, 1u);
						Height = std::max(Height / 2, 1u);
					}
				}
				return true;
			};

			bool Created = true;
			if(Desc.m_Create2D)
				Created = CreateNativeTexture(Texture.m_Texture, Texture.m_View, Texture.m_aBindGroups, Desc.m_Width, Desc.m_Height, 1, m_TextureBindGroupLayout, 1, pCommand->m_pData);
			if(Created && CreateArray)
			{
				int ConvertWidth = Desc.m_Width;
				int ConvertHeight = Desc.m_Height;
				uint8_t *pArraySource = pCommand->m_pData;
				std::unique_ptr<uint8_t, decltype(&free)> pResizedData(nullptr, free);
				if(ConvertWidth % Desc.m_LayerColumns != 0 || ConvertHeight % Desc.m_LayerRows != 0)
				{
					const int NewWidth = std::max(HighestBit(ConvertWidth / Desc.m_LayerColumns), 1) * Desc.m_LayerColumns;
					const int NewHeight = std::max(HighestBit(ConvertHeight / Desc.m_LayerRows), 1) * Desc.m_LayerRows;
					pResizedData.reset(ResizeImage(pArraySource, ConvertWidth, ConvertHeight, NewWidth, NewHeight, PixelSize));
					pArraySource = pResizedData.get();
					ConvertWidth = NewWidth;
					ConvertHeight = NewHeight;
				}
				const size_t ConvertedSize = static_cast<size_t>(ConvertWidth) * ConvertHeight * PixelSize;
				std::unique_ptr<uint8_t, decltype(&free)> pConvertedData(static_cast<uint8_t *>(malloc(ConvertedSize)), free);
				if(pArraySource == nullptr || pConvertedData == nullptr)
					Created = false;
				else
				{
					int LayerWidth, LayerHeight;
					Texture2DTo3D(pArraySource, ConvertWidth, ConvertHeight, PixelSize, Desc.m_LayerColumns, Desc.m_LayerRows, pConvertedData.get(), LayerWidth, LayerHeight);
					Created = CreateNativeTexture(Texture.m_ArrayTexture, Texture.m_ArrayView, Texture.m_aArrayBindGroups, LayerWidth, LayerHeight, static_cast<uint32_t>(Desc.LayerCount()), m_ArrayTextureBindGroupLayout, 2, pConvertedData.get());
				}
			}
			if(!Created)
			{
				ReleaseTexture(Texture);
				m_TextureHandles.Release(pCommand->m_Texture);
				SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to create a sampled texture");
				return false;
			}
			Texture.m_MemorySize = MemorySize;
			m_pTextureMemoryUsage->fetch_add(Texture.m_MemorySize, std::memory_order_relaxed);
			return true;
		}

		bool UpdateTexture(const CCommandBuffer::SCommand_Texture_Update *pCommand)
		{
			if(!m_TextureHandles.IsActive(pCommand->m_Texture) || static_cast<size_t>(pCommand->m_Texture.Id()) >= m_vTextures.size())
				return true;
			const auto &Texture = m_vTextures[pCommand->m_Texture.Id()];
			const auto &Region = pCommand->m_Region;
			if(Texture.m_Texture == nullptr || pCommand->m_pData == nullptr || pCommand->m_Format != Texture.m_Format || Region.m_Width == 0 || Region.m_Height == 0 || Region.m_X > Texture.m_Width || Region.m_Y > Texture.m_Height || Region.m_Width > Texture.m_Width - Region.m_X || Region.m_Height > Texture.m_Height - Region.m_Y)
				return true;
			if(!SubmitCommands())
				return false;
			WGPUTexelCopyTextureInfo Destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			Destination.texture = Texture.m_Texture;
			Destination.origin = {static_cast<uint32_t>(Region.m_X), static_cast<uint32_t>(Region.m_Y), 0};
			Destination.aspect = WGPUTextureAspect_All;
			const size_t PixelSize = Texture.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? 4 : 1;
			WGPUTexelCopyBufferLayout Layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			Layout.bytesPerRow = Region.m_Width * PixelSize;
			Layout.rowsPerImage = Region.m_Height;
			const WGPUExtent3D Extent{static_cast<uint32_t>(Region.m_Width), static_cast<uint32_t>(Region.m_Height), 1};
			wgpuQueueWriteTexture(m_Queue, &Destination, pCommand->m_pData, Region.m_Width * Region.m_Height * PixelSize, &Layout, &Extent);
			return true;
		}

		void DestroyTexture(IGraphics::CTextureHandle Handle)
		{
			if(!m_TextureHandles.IsActive(Handle) || static_cast<size_t>(Handle.Id()) >= m_vTextures.size())
				return;
			if(m_RenderTarget == Handle)
			{
				EndRenderPass();
				m_RenderTarget.Invalidate();
			}
			ReleaseTexture(m_vTextures[Handle.Id()]);
			m_TextureHandles.Release(Handle);
		}

		void ReleaseTextureBinding(STextureBinding &Binding)
		{
			for(auto &BindGroup : Binding.m_aBindGroups)
			{
				if(BindGroup != nullptr)
					wgpuBindGroupRelease(BindGroup);
				BindGroup = nullptr;
			}
			Binding = {};
		}

		bool CreateTextureBinding(const CCommandBuffer::SCommand_TextureBinding_Create *pCommand)
		{
			if(!m_TextureHandles.IsActive(pCommand->m_Desc.m_aTextures[0]) || !m_TextureHandles.IsActive(pCommand->m_Desc.m_aTextures[1]) || !m_TextureBindingHandles.Activate(pCommand->m_Binding))
				return true;
			const auto PrimaryId = pCommand->m_Desc.m_aTextures[0].Id();
			const auto SecondaryId = pCommand->m_Desc.m_aTextures[1].Id();
			if(static_cast<size_t>(PrimaryId) >= m_vTextures.size() || static_cast<size_t>(SecondaryId) >= m_vTextures.size() || m_vTextures[PrimaryId].m_View == nullptr || m_vTextures[SecondaryId].m_View == nullptr)
			{
				m_TextureBindingHandles.Release(pCommand->m_Binding);
				return true;
			}
			if(static_cast<size_t>(pCommand->m_Binding.Id()) >= m_vTextureBindings.size())
				m_vTextureBindings.resize(pCommand->m_Binding.Id() + 1);
			auto &Binding = m_vTextureBindings[pCommand->m_Binding.Id()];
			Binding.m_Desc = pCommand->m_Desc;
			for(size_t i = 0; i < Binding.m_aBindGroups.size(); ++i)
			{
				std::array<WGPUBindGroupEntry, 4> aEntries{};
				aEntries[0].binding = 0;
				aEntries[0].sampler = m_aSamplers[i];
				aEntries[1].binding = 1;
				aEntries[1].textureView = m_vTextures[PrimaryId].m_View;
				aEntries[2].binding = 3;
				aEntries[2].sampler = m_aSamplers[i];
				aEntries[3].binding = 4;
				aEntries[3].textureView = m_vTextures[SecondaryId].m_View;
				WGPUBindGroupDescriptor Descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
				Descriptor.label = StringView("DDNet WebGPU dual sampled texture binding");
				Descriptor.layout = m_DualTextureBindGroupLayout;
				Descriptor.entryCount = aEntries.size();
				Descriptor.entries = aEntries.data();
				Binding.m_aBindGroups[i] = wgpuDeviceCreateBindGroup(m_Device, &Descriptor);
				if(Binding.m_aBindGroups[i] == nullptr)
				{
					ReleaseTextureBinding(Binding);
					m_TextureBindingHandles.Release(pCommand->m_Binding);
					SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to create a dual texture binding");
					return false;
				}
			}
			return true;
		}

		void DestroyTextureBinding(CCommandBuffer::CTextureBindingHandle Handle)
		{
			if(!m_TextureBindingHandles.Release(Handle) || static_cast<size_t>(Handle.Id()) >= m_vTextureBindings.size())
				return;
			ReleaseTextureBinding(m_vTextureBindings[Handle.Id()]);
		}

		void ReleaseBuffer(SBuffer &Buffer)
		{
			if(Buffer.m_AllocatedSize != 0 && m_pBufferMemoryUsage != nullptr)
				m_pBufferMemoryUsage->fetch_sub(Buffer.m_AllocatedSize, std::memory_order_relaxed);
			if(Buffer.m_Buffer != nullptr)
			{
				wgpuBufferDestroy(Buffer.m_Buffer);
				wgpuBufferRelease(Buffer.m_Buffer);
			}
			Buffer = {};
		}

		bool WriteBufferData(WGPUBuffer Buffer, uint64_t Offset, const void *pData, size_t Size, bool AllowEndPadding)
		{
			if(Size == 0)
				return true;
			if(pData == nullptr || Offset % 4 != 0 || Size > std::numeric_limits<size_t>::max() - 3)
				return false;
			const size_t UploadSize = AlignUp(Size, 4);
			if(UploadSize != Size && !AllowEndPadding)
				return false;
			if(UploadSize == Size)
				wgpuQueueWriteBuffer(m_Queue, Buffer, Offset, pData, Size);
			else
			{
				m_vUploadScratch.resize(UploadSize);
				std::memcpy(m_vUploadScratch.data(), pData, Size);
				std::memset(m_vUploadScratch.data() + Size, 0, UploadSize - Size);
				wgpuQueueWriteBuffer(m_Queue, Buffer, Offset, m_vUploadScratch.data(), UploadSize);
			}
			return true;
		}

		bool CreateNativeBuffer(SBuffer &Buffer, const IGraphics::CBufferDesc &Desc, const void *pData)
		{
			if(Desc.m_Size > std::numeric_limits<size_t>::max() - 3)
				return false;
			const uint64_t AllocatedSize = std::max<uint64_t>(AlignUp(Desc.m_Size, 4), 4);
			WGPUBufferDescriptor Descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			Descriptor.label = StringView("DDNet WebGPU resource buffer");
			Descriptor.size = AllocatedSize;
			Descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc | (Desc.m_Usage == IGraphics::EBufferUsage::INDEX ? WGPUBufferUsage_Index : WGPUBufferUsage_Vertex);
			Buffer.m_Buffer = wgpuDeviceCreateBuffer(m_Device, &Descriptor);
			if(Buffer.m_Buffer == nullptr || !WriteBufferData(Buffer.m_Buffer, 0, pData, Desc.m_Size, true))
			{
				if(Buffer.m_Buffer != nullptr)
				{
					wgpuBufferDestroy(Buffer.m_Buffer);
					wgpuBufferRelease(Buffer.m_Buffer);
				}
				Buffer = {};
				return false;
			}
			Buffer.m_Size = Desc.m_Size;
			Buffer.m_Usage = Desc.m_Usage;
			return true;
		}

		bool CreateBuffer(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
		{
			if(!m_BufferHandles.Activate(pCommand->m_Buffer))
				return true;
			if(static_cast<size_t>(pCommand->m_Buffer.Id()) >= m_vBuffers.size())
				m_vBuffers.resize(pCommand->m_Buffer.Id() + 1);
			auto &Buffer = m_vBuffers[pCommand->m_Buffer.Id()];
			if(!CreateNativeBuffer(Buffer, pCommand->m_Desc, pCommand->m_pUploadData))
			{
				m_BufferHandles.Release(pCommand->m_Buffer);
				SetError(GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "WebGPU failed to create a resource buffer");
				return false;
			}
			Buffer.m_AllocatedSize = std::max<size_t>(AlignUp(Buffer.m_Size, 4), 4);
			m_pBufferMemoryUsage->fetch_add(Buffer.m_AllocatedSize, std::memory_order_relaxed);
			return true;
		}

		bool RecreateBuffer(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
		{
			if(!m_BufferHandles.IsActive(pCommand->m_Buffer) || static_cast<size_t>(pCommand->m_Buffer.Id()) >= m_vBuffers.size())
				return true;
			if(!SubmitCommands())
				return false;
			SBuffer NewBuffer;
			if(!CreateNativeBuffer(NewBuffer, pCommand->m_Desc, pCommand->m_pUploadData))
			{
				SetError(GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "WebGPU failed to recreate a resource buffer");
				return false;
			}
			ReleaseBuffer(m_vBuffers[pCommand->m_Buffer.Id()]);
			NewBuffer.m_AllocatedSize = std::max<size_t>(AlignUp(NewBuffer.m_Size, 4), 4);
			m_pBufferMemoryUsage->fetch_add(NewBuffer.m_AllocatedSize, std::memory_order_relaxed);
			m_vBuffers[pCommand->m_Buffer.Id()] = NewBuffer;
			return true;
		}

		bool UpdateBuffer(const CCommandBuffer::SCommand_UpdateBufferObject *pCommand)
		{
			if(!m_BufferHandles.IsActive(pCommand->m_Buffer) || static_cast<size_t>(pCommand->m_Buffer.Id()) >= m_vBuffers.size())
				return true;
			auto &Buffer = m_vBuffers[pCommand->m_Buffer.Id()];
			if(pCommand->m_Offset > Buffer.m_Size || pCommand->m_DataSize > Buffer.m_Size - pCommand->m_Offset)
				return true;
			if(!SubmitCommands())
				return false;
			if(!WriteBufferData(Buffer.m_Buffer, pCommand->m_Offset, pCommand->m_pUploadData, pCommand->m_DataSize, pCommand->m_Offset + pCommand->m_DataSize == Buffer.m_Size))
			{
				SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU buffer updates require four-byte aligned ranges");
				return false;
			}
			return true;
		}

		void DestroyGpuTimestampResources()
		{
			for(auto &Slot : m_aGpuTimestampSlots)
			{
				if(Slot.m_ReadbackBuffer != nullptr)
				{
					wgpuBufferDestroy(Slot.m_ReadbackBuffer);
					wgpuBufferRelease(Slot.m_ReadbackBuffer);
				}
				Slot = {};
			}
			if(m_GpuTimestampResolveBuffer != nullptr)
			{
				wgpuBufferDestroy(m_GpuTimestampResolveBuffer);
				wgpuBufferRelease(m_GpuTimestampResolveBuffer);
				m_GpuTimestampResolveBuffer = nullptr;
			}
			if(m_GpuTimestampQuerySet != nullptr)
			{
				wgpuQuerySetRelease(m_GpuTimestampQuerySet);
				m_GpuTimestampQuerySet = nullptr;
			}
			m_GpuTimestampActiveSlot = -1;
			m_GpuTimestampActiveSubmitted = false;
			m_aGpuTimestampZoneActiveIntervals.fill(-1);
			m_GpuTimestampIntervalCount = 0;
			m_GpuTimestampInvalidZoneMask = 0;
		}

		bool EnsureGpuTimestampResources()
		{
			if(m_GpuTimestampQuerySet != nullptr)
				return true;
			if(!m_GpuTimestampSupported || m_GpuTimestampResourcesFailed)
				return false;

			WGPUQuerySetDescriptor QuerySetDescriptor = WGPU_QUERY_SET_DESCRIPTOR_INIT;
			QuerySetDescriptor.label = StringView("DDNet WebGPU frame timestamps");
			QuerySetDescriptor.type = WGPUQueryType_Timestamp;
			QuerySetDescriptor.count = GPU_TIMESTAMP_SLOT_COUNT * GPU_TIMESTAMP_QUERY_COUNT;
			m_GpuTimestampQuerySet = wgpuDeviceCreateQuerySet(m_Device, &QuerySetDescriptor);

			WGPUBufferDescriptor BufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			BufferDescriptor.label = StringView("DDNet WebGPU timestamp resolve buffer");
			BufferDescriptor.size = GPU_TIMESTAMP_SLOT_COUNT * GPU_TIMESTAMP_RESOLVE_STRIDE;
			BufferDescriptor.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
			m_GpuTimestampResolveBuffer = wgpuDeviceCreateBuffer(m_Device, &BufferDescriptor);
			for(auto &Slot : m_aGpuTimestampSlots)
			{
				BufferDescriptor.label = StringView("DDNet WebGPU timestamp readback buffer");
				BufferDescriptor.size = GPU_TIMESTAMP_SIZE;
				BufferDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
				Slot.m_ReadbackBuffer = wgpuDeviceCreateBuffer(m_Device, &BufferDescriptor);
				if(Slot.m_ReadbackBuffer == nullptr)
					break;
			}
			if(m_GpuTimestampQuerySet != nullptr && m_GpuTimestampResolveBuffer != nullptr && std::ranges::all_of(m_aGpuTimestampSlots, [](const SGpuTimestampSlot &Slot) { return Slot.m_ReadbackBuffer != nullptr; }))
				return true;

			DestroyGpuTimestampResources();
			m_GpuTimestampResourcesFailed = true;
			m_GpuTimestampSupported = false;
			m_pGpuTiming->m_Supported.store(false, std::memory_order_relaxed);
			log_warn("gfx/webgpu", "GPU timestamp resources are unavailable; continuing without GPU timing");
			return false;
		}

		void CollectGpuTimestampResults()
		{
			for(auto &Slot : m_aGpuTimestampSlots)
			{
				if(!Slot.m_InFlight || Slot.m_pMapResult == nullptr || !Slot.m_pMapResult->m_Done)
					continue;
				if(Slot.m_pMapResult->m_Status == WGPUMapAsyncStatus_Success)
				{
					const auto *pMappedData = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(Slot.m_ReadbackBuffer, 0, GPU_TIMESTAMP_SIZE));
					if(pMappedData != nullptr && Slot.m_Publish && m_pGpuTiming->CanPublish(Slot.m_Generation))
					{
						std::array<uint64_t, GPU_TIMESTAMP_QUERY_COUNT> aTimestamps;
						std::memcpy(aTimestamps.data(), pMappedData, GPU_TIMESTAMP_SIZE);
						const uint64_t Start = aTimestamps[0];
						const uint64_t End = aTimestamps[1];
						const double Duration = End >= Start ? static_cast<double>(End - Start) * m_GpuTimestampPeriod : -1.0;
						if(Duration >= 0.0 && std::isfinite(Duration) && Duration <= static_cast<double>(std::numeric_limits<uint64_t>::max()))
						{
							std::array<uint64_t, IGraphics::GPU_RENDER_ZONE_COUNT> aZoneNanoseconds{};
							for(uint32_t Interval = 0; Interval < Slot.m_IntervalCount; ++Interval)
							{
								const size_t Zone = static_cast<size_t>(Slot.m_aIntervalZones[Interval]);
								if((Slot.m_ZoneMask & (1U << Zone)) == 0)
									continue;
								const size_t Query = 2 + Interval * 2;
								const double ZoneDuration = aTimestamps[Query + 1] >= aTimestamps[Query] ? static_cast<double>(aTimestamps[Query + 1] - aTimestamps[Query]) * m_GpuTimestampPeriod : -1.0;
								if(ZoneDuration >= 0.0 && std::isfinite(ZoneDuration) && ZoneDuration <= static_cast<double>(std::numeric_limits<uint64_t>::max()))
								{
									const uint64_t IntervalNanoseconds = static_cast<uint64_t>(ZoneDuration + 0.5);
									aZoneNanoseconds[Zone] = std::numeric_limits<uint64_t>::max() - aZoneNanoseconds[Zone] < IntervalNanoseconds ? std::numeric_limits<uint64_t>::max() : aZoneNanoseconds[Zone] + IntervalNanoseconds;
								}
							}
							m_pGpuTiming->Publish(static_cast<uint64_t>(Duration + 0.5), aZoneNanoseconds, Slot.m_ZoneMask);
						}
					}
					wgpuBufferUnmap(Slot.m_ReadbackBuffer);
				}
				Slot.m_pMapResult.reset();
				Slot.m_InFlight = false;
				Slot.m_Publish = false;
			}
		}

		/**
		 * Gives the timestamp slot of the current frame back without publishing
		 * a measurement.
		 *
		 * A frame that never reaches the queue has no timings, and its slot has
		 * to become free again. There are only a handful of slots, so a slot
		 * left claimed on an error path means GPU timing is off from then on
		 * while still reporting itself as supported.
		 */
		void AbandonGpuTimestamp()
		{
			if(m_GpuTimestampActiveSlot < 0)
				return;
			SGpuTimestampSlot &Slot = m_aGpuTimestampSlots[m_GpuTimestampActiveSlot];
			Slot.m_pMapResult.reset();
			Slot.m_InFlight = false;
			Slot.m_Publish = false;
			m_GpuTimestampActiveSlot = -1;
			m_GpuTimestampActiveSubmitted = false;
			m_aGpuTimestampZoneActiveIntervals.fill(-1);
			m_GpuTimestampIntervalCount = 0;
			m_GpuTimestampInvalidZoneMask = 0;
		}

		void BeginGpuTimestamp()
		{
			if(m_GpuTimestampActiveSlot >= 0 || !m_GpuTimestampSupported || m_pGpuTiming == nullptr || !m_pGpuTiming->m_Enabled.load(std::memory_order_relaxed) || !EnsureGpuTimestampResources())
				return;
			for(size_t i = 0; i < m_aGpuTimestampSlots.size(); ++i)
			{
				if(m_aGpuTimestampSlots[i].m_InFlight)
					continue;
				m_GpuTimestampActiveSlot = static_cast<int>(i);
				m_GpuTimestampActiveSubmitted = false;
				m_aGpuTimestampZoneActiveIntervals.fill(-1);
				m_GpuTimestampIntervalCount = 0;
				m_GpuTimestampInvalidZoneMask = 0;
				m_GpuTimestampGeneration = m_pGpuTiming->Generation();
				wgpuCommandEncoderWriteTimestamp(m_CommandEncoder, m_GpuTimestampQuerySet, static_cast<uint32_t>(i * GPU_TIMESTAMP_QUERY_COUNT));
				return;
			}
		}

		bool GpuRenderZone(const CCommandBuffer::SCommand_GpuRenderZone *pCommand)
		{
			if(!m_GpuTimestampInsidePassesSupported)
				return true;
			if(!EnsureCommandEncoder() || m_GpuTimestampActiveSlot < 0)
				return m_CommandEncoder != nullptr;
			const size_t Zone = static_cast<size_t>(pCommand->m_Zone);
			if(Zone >= m_aGpuTimestampZoneActiveIntervals.size())
				return true;
			uint32_t Query;
			if(pCommand->m_Begin)
			{
				if(m_aGpuTimestampZoneActiveIntervals[Zone] >= 0)
					return true;
				if(m_GpuTimestampIntervalCount >= GPU_TIMESTAMP_MAX_INTERVALS)
				{
					m_GpuTimestampInvalidZoneMask |= 1U << Zone;
					return true;
				}
				const uint32_t Interval = m_GpuTimestampIntervalCount++;
				m_aGpuTimestampIntervalZones[Interval] = pCommand->m_Zone;
				m_aGpuTimestampZoneActiveIntervals[Zone] = static_cast<int32_t>(Interval);
				Query = static_cast<uint32_t>(m_GpuTimestampActiveSlot * GPU_TIMESTAMP_QUERY_COUNT + 2 + Interval * 2);
			}
			else
			{
				const int32_t Interval = m_aGpuTimestampZoneActiveIntervals[Zone];
				if(Interval < 0)
					return true;
				Query = static_cast<uint32_t>(m_GpuTimestampActiveSlot * GPU_TIMESTAMP_QUERY_COUNT + 3 + static_cast<uint32_t>(Interval) * 2);
			}
			if(m_RenderPass != nullptr)
			{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
				return true;
#else
				wgpuRenderPassEncoderWriteTimestamp(m_RenderPass, m_GpuTimestampQuerySet, Query);
#endif
			}
			else
				wgpuCommandEncoderWriteTimestamp(m_CommandEncoder, m_GpuTimestampQuerySet, Query);
			if(!pCommand->m_Begin)
				m_aGpuTimestampZoneActiveIntervals[Zone] = -1;
			return true;
		}

		void MapGpuTimestampSlot(int SlotIndex, bool Publish)
		{
			auto &Slot = m_aGpuTimestampSlots[SlotIndex];
			Slot.m_pMapResult = std::make_shared<SMapResult>();
			Slot.m_InFlight = true;
			Slot.m_Publish = Publish;
			uint32_t ZoneMask = 0;
			for(uint32_t Interval = 0; Interval < m_GpuTimestampIntervalCount; ++Interval)
				ZoneMask |= 1U << static_cast<size_t>(m_aGpuTimestampIntervalZones[Interval]);
			Slot.m_ZoneMask = ZoneMask & ~m_GpuTimestampInvalidZoneMask;
			Slot.m_IntervalCount = m_GpuTimestampIntervalCount;
			Slot.m_aIntervalZones = m_aGpuTimestampIntervalZones;
			Slot.m_Generation = m_GpuTimestampGeneration;
			WGPUBufferMapCallbackInfo CallbackInfo = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
			CallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
			CallbackInfo.callback = MapCallback;
			CallbackInfo.userdata1 = new std::shared_ptr<SMapResult>(Slot.m_pMapResult);
			wgpuBufferMapAsync(Slot.m_ReadbackBuffer, WGPUMapMode_Read, 0, GPU_TIMESTAMP_SIZE, CallbackInfo);
		}

		bool EnsureCommandEncoder()
		{
			if(m_CommandEncoder != nullptr)
				return true;
			WGPUCommandEncoderDescriptor Descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
			Descriptor.label = StringView("DDNet WebGPU frame encoder");
			m_CommandEncoder = wgpuDeviceCreateCommandEncoder(m_Device, &Descriptor);
			if(m_CommandEncoder != nullptr)
				BeginGpuTimestamp();
			return m_CommandEncoder != nullptr;
		}

		bool CopyBuffer(const CCommandBuffer::SCommand_CopyBufferObject *pCommand)
		{
			if(!m_BufferHandles.IsActive(pCommand->m_ReadBuffer) || !m_BufferHandles.IsActive(pCommand->m_WriteBuffer) || static_cast<size_t>(pCommand->m_ReadBuffer.Id()) >= m_vBuffers.size() || static_cast<size_t>(pCommand->m_WriteBuffer.Id()) >= m_vBuffers.size())
				return true;
			const auto &ReadBuffer = m_vBuffers[pCommand->m_ReadBuffer.Id()];
			const auto &WriteBuffer = m_vBuffers[pCommand->m_WriteBuffer.Id()];
			if(pCommand->m_ReadOffset > ReadBuffer.m_Size || pCommand->m_CopySize > ReadBuffer.m_Size - pCommand->m_ReadOffset || pCommand->m_WriteOffset > WriteBuffer.m_Size || pCommand->m_CopySize > WriteBuffer.m_Size - pCommand->m_WriteOffset)
				return true;
			if(pCommand->m_CopySize == 0)
				return true;
			if(pCommand->m_ReadOffset % 4 != 0 || pCommand->m_WriteOffset % 4 != 0 || pCommand->m_CopySize % 4 != 0)
			{
				SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU buffer copies require four-byte aligned ranges");
				return false;
			}
			EndRenderPass();
			if(!EnsureCommandEncoder())
				return false;
			wgpuCommandEncoderCopyBufferToBuffer(m_CommandEncoder, ReadBuffer.m_Buffer, pCommand->m_ReadOffset, WriteBuffer.m_Buffer, pCommand->m_WriteOffset, pCommand->m_CopySize);
			return true;
		}

		bool DestroyBuffer(IGraphics::CBufferHandle Handle)
		{
			if(!m_BufferHandles.IsActive(Handle) || static_cast<size_t>(Handle.Id()) >= m_vBuffers.size())
				return true;
			if(!SubmitCommands())
				return false;
			ReleaseBuffer(m_vBuffers[Handle.Id()]);
			m_BufferHandles.Release(Handle);
			return true;
		}

		void CreateBufferContainer(const CCommandBuffer::SCommand_CreateBufferContainer *pCommand)
		{
			if(!m_BufferHandles.IsActive(pCommand->m_VertBufferBinding) || !m_BufferContainerHandles.Activate(pCommand->m_BufferContainer))
				return;
			if(static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainers.size())
				m_vBufferContainers.resize(pCommand->m_BufferContainer.Id() + 1);
			auto &Container = m_vBufferContainers[pCommand->m_BufferContainer.Id()];
			Container.m_VertexBuffer = pCommand->m_VertBufferBinding;
			Container.m_Stride = pCommand->m_Stride;
			Container.m_AttributeCount = pCommand->m_AttrCount;
		}

		void UpdateBufferContainer(const CCommandBuffer::SCommand_UpdateBufferContainer *pCommand)
		{
			if(!m_BufferContainerHandles.IsActive(pCommand->m_BufferContainer) || !m_BufferHandles.IsActive(pCommand->m_VertBufferBinding) || static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainers.size())
				return;
			auto &Container = m_vBufferContainers[pCommand->m_BufferContainer.Id()];
			Container.m_VertexBuffer = pCommand->m_VertBufferBinding;
			Container.m_Stride = pCommand->m_Stride;
			Container.m_AttributeCount = pCommand->m_AttrCount;
		}

		bool DestroyBufferContainer(const CCommandBuffer::SCommand_DeleteBufferContainer *pCommand)
		{
			if(!m_BufferContainerHandles.IsActive(pCommand->m_BufferContainer) || static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainers.size())
				return true;
			auto &Container = m_vBufferContainers[pCommand->m_BufferContainer.Id()];
			if(pCommand->m_DestroyAllBO && !DestroyBuffer(Container.m_VertexBuffer))
				return false;
			Container = {};
			m_BufferContainerHandles.Release(pCommand->m_BufferContainer);
			return true;
		}

		void CreatePipeline(const CCommandBuffer::SCommand_Pipeline_Create *pCommand)
		{
			if(pCommand->m_Desc.m_Program >= EPipelineProgram::COUNT || !m_PipelineHandles.Activate(pCommand->m_Pipeline))
				return;
			if(static_cast<size_t>(pCommand->m_Pipeline.Id()) >= m_vPipelines.size())
				m_vPipelines.resize(pCommand->m_Pipeline.Id() + 1);
			m_vPipelines[pCommand->m_Pipeline.Id()] = pCommand->m_Desc.m_Program;
		}

		void DestroyPipeline(CCommandBuffer::CPipelineHandle Handle)
		{
			if(!m_PipelineHandles.Release(Handle) || static_cast<size_t>(Handle.Id()) >= m_vPipelines.size())
				return;
			m_vPipelines[Handle.Id()] = EPipelineProgram::PRIMITIVE;
		}

		bool HasPrimitivePipeline(CCommandBuffer::CPipelineHandle Handle) const
		{
			return m_PipelineHandles.IsActive(Handle) && static_cast<size_t>(Handle.Id()) < m_vPipelines.size() && m_vPipelines[Handle.Id()] == EPipelineProgram::PRIMITIVE;
		}

		bool ResolvePipeline(CCommandBuffer::CPipelineHandle Handle, EPipelineProgram &Program) const
		{
			if(!m_PipelineHandles.IsActive(Handle) || static_cast<size_t>(Handle.Id()) >= m_vPipelines.size())
				return false;
			Program = m_vPipelines[Handle.Id()];
			return true;
		}

		STexture *RenderTarget()
		{
			if(!m_RenderTarget.IsValid() || !m_TextureHandles.IsActive(m_RenderTarget) || static_cast<size_t>(m_RenderTarget.Id()) >= m_vTextures.size())
				return nullptr;
			return &m_vTextures[m_RenderTarget.Id()];
		}

		bool EnsureRenderPass()
		{
			if(m_RenderPass != nullptr)
				return true;
			STexture *pTarget = RenderTarget();
			if(m_BackendMode == EGraphicsBackendMode::OFFSCREEN && pTarget == nullptr)
			{
				SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU offscreen draws require a render target");
				return false;
			}
			if(m_RenderTarget.IsValid() && pTarget == nullptr)
				return false;
			if(pTarget == nullptr && (m_Minimized || !AcquireFrame()))
				return false;
			if(!EnsureCommandEncoder())
				return false;
			WGPUTextureView ResolveView = pTarget != nullptr ? pTarget->m_View : m_SurfaceView;
			WGPUTextureView RenderView = ResolveView;
			bool FreshMultisampleTarget = false;
			const uint32_t RenderSampleCount = pTarget != nullptr ? 1 : SampleCount();
			if(RenderSampleCount != 1)
			{
				WGPUTexture &MultisampleTexture = pTarget != nullptr ? pTarget->m_MultisampleTexture : m_SurfaceMultisampleTexture;
				WGPUTextureView &MultisampleView = pTarget != nullptr ? pTarget->m_MultisampleView : m_SurfaceMultisampleView;
				size_t &MultisampleMemorySize = pTarget != nullptr ? pTarget->m_MultisampleMemorySize : m_SurfaceMultisampleMemorySize;
				FreshMultisampleTarget = MultisampleView == nullptr;
				const uint32_t Width = pTarget != nullptr ? static_cast<uint32_t>(pTarget->m_Width) : m_SurfaceWidth;
				const uint32_t Height = pTarget != nullptr ? static_cast<uint32_t>(pTarget->m_Height) : m_SurfaceHeight;
				if(!CreateMultisampleTarget(pTarget != nullptr ? WGPUTextureFormat_RGBA8Unorm : m_SurfaceFormat, Width, Height, RenderSampleCount, MultisampleTexture, MultisampleView, MultisampleMemorySize))
					return false;
				RenderView = MultisampleView;
			}
			WGPURenderPassColorAttachment ColorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			ColorAttachment.view = RenderView;
			ColorAttachment.resolveTarget = RenderSampleCount == 1 ? nullptr : ResolveView;
			ColorAttachment.loadOp = FreshMultisampleTarget ? WGPULoadOp_Clear : m_RenderPassLoadOp;
			ColorAttachment.storeOp = WGPUStoreOp_Store;
			ColorAttachment.clearValue = m_RenderPassClearColor;
			ColorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			WGPURenderPassDescriptor PassDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			PassDescriptor.label = StringView("DDNet WebGPU surface pass");
			PassDescriptor.colorAttachmentCount = 1;
			PassDescriptor.colorAttachments = &ColorAttachment;
			m_RenderPass = wgpuCommandEncoderBeginRenderPass(m_CommandEncoder, &PassDescriptor);
			m_RenderPassLoadOp = WGPULoadOp_Load;
			return m_RenderPass != nullptr;
		}

		bool WriteStream(const void *pData, size_t Size, uint64_t Alignment, uint64_t &Offset)
		{
			const size_t UploadSize = AlignUp(Size, 4);
			m_StreamOffset = AlignUp(m_StreamOffset, Alignment);
			if(UploadSize > STREAM_BUFFER_SIZE - m_StreamOffset)
			{
				SetError(GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "WebGPU frame stream buffer is exhausted");
				return false;
			}
			Offset = m_UploadBufferSlot * STREAM_BUFFER_SIZE + m_StreamOffset;
			m_vStreamUpload.resize(m_StreamOffset + UploadSize);
			std::memcpy(m_vStreamUpload.data() + m_StreamOffset, pData, Size);
			if(UploadSize != Size)
				std::memset(m_vStreamUpload.data() + m_StreamOffset + Size, 0, UploadSize - Size);
			m_StreamOffset += UploadSize;
			return true;
		}

		bool ApplyState(const CCommandBuffer::SState &State, WGPURenderPipeline Pipeline, const ColorRGBA &Color, const vec2 &RotationCenter, float Rotation, bool TextureArray = false, const vec2 &VertexOffset = vec2(0.0f, 0.0f), const vec2 &VertexScale = vec2(1.0f, 1.0f), uint32_t QuadBase = 0, float TextureSize = 1.0f, const ColorRGBA &SecondaryColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f))
		{
			const bool Textured = State.m_Texture.IsValid();
			const STexture *pTexture = nullptr;
			if(Textured)
			{
				if(!m_TextureHandles.IsActive(State.m_Texture) || static_cast<size_t>(State.m_Texture.Id()) >= m_vTextures.size())
					return false;
				pTexture = &m_vTextures[State.m_Texture.Id()];
				if((TextureArray && pTexture->m_ArrayView == nullptr) || (!TextureArray && pTexture->m_View == nullptr))
					return false;
			}
			const float Width = State.m_ScreenBR.x - State.m_ScreenTL.x;
			const float Height = State.m_ScreenBR.y - State.m_ScreenTL.y;
			if(Width == 0.0f || Height == 0.0f)
				return false;
			SPrimitiveTransform Transform{};
			Transform.m_Scale = vec2(2.0f / Width, -2.0f / Height);
			Transform.m_Translate = vec2(-(State.m_ScreenTL.x + State.m_ScreenBR.x) / Width, (State.m_ScreenTL.y + State.m_ScreenBR.y) / Height);
			Transform.m_Color = Color;
			Transform.m_RotationCenter = RotationCenter;
			Transform.m_Rotation = Rotation;
			Transform.m_AlphaTexture = pTexture != nullptr && pTexture->m_Format == IGraphics::ETextureFormat::R8_UNORM;
			Transform.m_VertexOffset = VertexOffset;
			Transform.m_VertexScale = VertexScale;
			Transform.m_QuadBase = QuadBase;
			Transform.m_TextureSize = TextureSize;
			Transform.m_SecondaryColor = SecondaryColor;
			m_UniformOffset = AlignUp(m_UniformOffset, m_UniformAlignment);
			if(sizeof(Transform) > UNIFORM_BUFFER_SIZE - m_UniformOffset || m_UniformOffset > UINT32_MAX)
			{
				SetError(GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "WebGPU frame uniform buffer is exhausted");
				return false;
			}
			const uint32_t UniformOffset = static_cast<uint32_t>(m_UploadBufferSlot * UNIFORM_BUFFER_SIZE + m_UniformOffset);
			m_vUniformUpload.resize(m_UniformOffset + sizeof(Transform));
			std::memcpy(m_vUniformUpload.data() + m_UniformOffset, &Transform, sizeof(Transform));
			m_UniformOffset += sizeof(Transform);
			wgpuRenderPassEncoderSetPipeline(m_RenderPass, Pipeline);
			wgpuRenderPassEncoderSetBindGroup(m_RenderPass, 0, m_UniformBindGroup, 1, &UniformOffset);
			if(pTexture != nullptr)
			{
				const auto &aBindGroups = TextureArray ? pTexture->m_aArrayBindGroups : pTexture->m_aBindGroups;
				wgpuRenderPassEncoderSetBindGroup(m_RenderPass, 1, aBindGroups[SamplerIndex(State.m_WrapMode)], 0, nullptr);
			}

			const STexture *pTarget = RenderTarget();
			const uint32_t TargetWidth = pTarget != nullptr ? static_cast<uint32_t>(pTarget->m_Width) : m_SurfaceWidth;
			const uint32_t TargetHeight = pTarget != nullptr ? static_cast<uint32_t>(pTarget->m_Height) : m_SurfaceHeight;
			const uint32_t ViewportX = pTarget != nullptr ? 0 : std::min(m_ViewportX, TargetWidth);
			const uint32_t ViewportY = pTarget != nullptr ? 0 : std::min(m_ViewportY, TargetHeight);
			const uint32_t RequestedWidth = pTarget != nullptr ? TargetWidth : (m_ViewportWidth > 0 ? m_ViewportWidth : TargetWidth);
			const uint32_t RequestedHeight = pTarget != nullptr ? TargetHeight : (m_ViewportHeight > 0 ? m_ViewportHeight : TargetHeight);
			const uint32_t ViewportWidth = std::min(RequestedWidth, TargetWidth - ViewportX);
			const uint32_t ViewportHeight = std::min(RequestedHeight, TargetHeight - ViewportY);
			if(ViewportWidth == 0 || ViewportHeight == 0)
				return false;
			wgpuRenderPassEncoderSetViewport(m_RenderPass, ViewportX, ViewportY, ViewportWidth, ViewportHeight, 0.0f, 1.0f);
			uint32_t ScissorX = ViewportX;
			uint32_t ScissorY = ViewportY;
			uint32_t ScissorW = ViewportWidth;
			uint32_t ScissorH = ViewportHeight;
			if(State.m_ClipEnable)
			{
				const uint32_t ClipBaseWidth = std::max(m_ViewportWidth, 1u);
				const uint32_t ClipBaseHeight = std::max(m_ViewportHeight, 1u);
				const int64_t Left = std::clamp<int64_t>(static_cast<int64_t>(State.m_ClipX) * ViewportWidth / ClipBaseWidth, 0, ViewportWidth);
				const int64_t Right = std::clamp<int64_t>((static_cast<int64_t>(State.m_ClipX) + State.m_ClipW) * ViewportWidth / ClipBaseWidth, 0, ViewportWidth);
				const int64_t Bottom = std::clamp<int64_t>(static_cast<int64_t>(State.m_ClipY) * ViewportHeight / ClipBaseHeight, 0, ViewportHeight);
				const int64_t Top = std::clamp<int64_t>((static_cast<int64_t>(State.m_ClipY) + State.m_ClipH) * ViewportHeight / ClipBaseHeight, 0, ViewportHeight);
				ScissorX += static_cast<uint32_t>(Left);
				ScissorY += ViewportHeight - static_cast<uint32_t>(Top);
				ScissorW = static_cast<uint32_t>(std::max<int64_t>(Right - Left, 0));
				ScissorH = static_cast<uint32_t>(std::max<int64_t>(Top - Bottom, 0));
			}
			wgpuRenderPassEncoderSetScissorRect(m_RenderPass, ScissorX, ScissorY, ScissorW, ScissorH);
			return ScissorW != 0 && ScissorH != 0;
		}

		bool WriteQuadTransforms(const CCommandBuffer::SDrawDataQuadTransform *pData, uint32_t Count, uint32_t &Offset)
		{
			constexpr size_t BlockSize = GRAPHICS_MAX_QUADS_RENDER_COUNT * sizeof(CCommandBuffer::SDrawDataQuadTransform);
			const size_t UploadSize = static_cast<size_t>(Count) * sizeof(*pData);
			m_UniformOffset = AlignUp(m_UniformOffset, m_UniformAlignment);
			if(Count == 0 || Count > GRAPHICS_MAX_QUADS_RENDER_COUNT || BlockSize > UNIFORM_BUFFER_SIZE - m_UniformOffset || m_UniformOffset > UINT32_MAX)
			{
				SetError(GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "WebGPU frame uniform buffer is exhausted");
				return false;
			}
			Offset = static_cast<uint32_t>(m_UploadBufferSlot * UNIFORM_BUFFER_SIZE + m_UniformOffset);
			m_vUniformUpload.resize(m_UniformOffset + UploadSize);
			std::memcpy(m_vUniformUpload.data() + m_UniformOffset, pData, UploadSize);
			m_UniformOffset += UploadSize;
			return true;
		}

		bool Draw(const CCommandBuffer::SCommand_Draw *pCommand)
		{
			EPipelineProgram Program;
			if(!ResolvePipeline(pCommand->m_Pipeline, Program) || (Program != EPipelineProgram::PRIMITIVE && Program != EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY && Program != EPipelineProgram::BLUR && Program != EPipelineProgram::PLANAR_YUV) || pCommand->m_IndexBuffer.IsValid())
				return true;
			const bool Layered = Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY;
			const bool Blur = Program == EPipelineProgram::BLUR;
			const bool PlanarYuv = Program == EPipelineProgram::PLANAR_YUV;
			const auto *pVertices = Layered ? nullptr : pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
			const auto *pLayeredVertices = Layered ? pCommand->m_VertexData.Get<CCommandBuffer::SVertexTex3DStream>(pCommand->m_VertexCount) : nullptr;
			if((Layered ? pLayeredVertices == nullptr : pVertices == nullptr) || pCommand->m_VertexCount == 0)
				return true;
			const void *pUploadVertices = Layered ? static_cast<const void *>(pLayeredVertices) : pVertices;
			size_t VertexSize = Layered ? sizeof(*pLayeredVertices) : sizeof(*pVertices);
			uint32_t VertexCount = pCommand->m_VertexCount;
			EPrimitiveType PrimitiveType = pCommand->m_PrimitiveType;
			constexpr std::array<uint32_t, 6> aQuadIndices = {0, 1, 2, 0, 2, 3};
			if(PrimitiveType == EPrimitiveType::QUADS)
			{
				if(VertexCount % 4 != 0)
					return true;
				if(Layered)
				{
					m_vLayeredQuadVertices.resize(VertexCount / 4 * 6);
					for(uint32_t Quad = 0; Quad < VertexCount / 4; ++Quad)
					{
						const uint32_t Source = Quad * 4;
						const uint32_t Destination = Quad * 6;
						for(uint32_t i = 0; i < 6; ++i)
							m_vLayeredQuadVertices[Destination + i] = pLayeredVertices[Source + aQuadIndices[i]];
					}
					pUploadVertices = m_vLayeredQuadVertices.data();
				}
				else
				{
					m_vQuadVertices.resize(VertexCount / 4 * 6);
					for(uint32_t Quad = 0; Quad < VertexCount / 4; ++Quad)
					{
						const uint32_t Source = Quad * 4;
						const uint32_t Destination = Quad * 6;
						for(uint32_t i = 0; i < 6; ++i)
							m_vQuadVertices[Destination + i] = pVertices[Source + aQuadIndices[i]];
					}
					pUploadVertices = m_vQuadVertices.data();
				}
				VertexCount = VertexCount / 4 * 6;
				PrimitiveType = EPrimitiveType::TRIANGLES;
			}
			else
			{
				const uint32_t VerticesPerPrim = VerticesPerPrimitive(PrimitiveType);
				if(VerticesPerPrim == 0 || VertexCount % VerticesPerPrim != 0)
					return true;
			}
			uint64_t VertexOffset = 0;
			if(!EnsureRenderPass())
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			if(!WriteStream(pUploadVertices, VertexCount * VertexSize, 4, VertexOffset))
				return false;
			const bool Textured = pCommand->m_State.m_Texture.IsValid();
			if(Blur && (!Textured || pCommand->m_State.m_BlendMode != EBlendMode::NONE || PrimitiveType != EPrimitiveType::TRIANGLES))
				return true;
			const auto &Pipelines = m_aPipelineSets[m_RenderTarget.IsValid() ? 1 : 0];
			const auto &aPipelines = Layered ? Pipelines.m_aLayeredPrimitive : Pipelines.m_aPrimitive;
			const WGPURenderPipeline Pipeline = PlanarYuv ? Pipelines.m_PlanarYuv : (Blur ? Pipelines.m_Blur : aPipelines[PrimitivePipelineIndex(PrimitiveType, pCommand->m_State.m_BlendMode, Textured)]);
			if(!ApplyState(pCommand->m_State, Pipeline, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), vec2(0.0f, 0.0f), 0.0f, Layered))
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, m_StreamBuffer, VertexOffset, VertexCount * VertexSize);
			wgpuRenderPassEncoderDraw(m_RenderPass, VertexCount, 1, 0, 0);
			return true;
		}

		bool DrawTransient(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
		{
			if(!HasPrimitivePipeline(pCommand->m_Pipeline) || !pCommand->ValidateTransient())
				return true;
			const auto *pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
			const auto *pRanges = pCommand->m_RangeData.Get<CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange>(pCommand->m_RangeCount);
			const void *pIndices = pCommand->m_IndexData.m_pData;
			const size_t IndexSize = pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? sizeof(uint16_t) : sizeof(uint32_t);
			const auto &Pipelines = m_aPipelineSets[m_RenderTarget.IsValid() ? 1 : 0];
			for(uint32_t i = 0; i < pCommand->m_RangeCount; ++i)
			{
				const auto Texture = pRanges[i].m_State.m_Texture;
				if(Texture.IsValid() && !m_TextureHandles.IsActive(Texture))
					return true;
			}
			uint64_t VertexOffset = 0;
			uint64_t IndexOffset = 0;
			if(!EnsureRenderPass())
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			if(!WriteStream(pVertices, pCommand->m_VertexCount * sizeof(*pVertices), 4, VertexOffset) || !WriteStream(pIndices, pCommand->m_IndexCount * IndexSize, 4, IndexOffset))
				return false;
			wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, m_StreamBuffer, VertexOffset, pCommand->m_VertexCount * sizeof(*pVertices));
			wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, m_StreamBuffer, pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, IndexOffset, pCommand->m_IndexCount * IndexSize);
			for(uint32_t i = 0; i < pCommand->m_RangeCount; ++i)
			{
				const bool Textured = pRanges[i].m_State.m_Texture.IsValid();
				if(!ApplyState(pRanges[i].m_State, Pipelines.m_aPrimitive[PrimitivePipelineIndex(EPrimitiveType::TRIANGLES, pRanges[i].m_State.m_BlendMode, Textured)], ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), vec2(0.0f, 0.0f), 0.0f))
				{
					if(m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
						return false;
					continue;
				}
				wgpuRenderPassEncoderDrawIndexed(m_RenderPass, pRanges[i].m_IndexCount, 1, pRanges[i].m_FirstIndex, static_cast<int32_t>(pRanges[i].m_VertexOffset), 0);
			}
			return true;
		}

		bool DrawBuffered(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
		{
			if(!m_PipelineHandles.IsActive(pCommand->m_Pipeline) || static_cast<size_t>(pCommand->m_Pipeline.Id()) >= m_vPipelines.size() || !m_BufferContainerHandles.IsActive(pCommand->m_BufferContainer) || static_cast<size_t>(pCommand->m_BufferContainer.Id()) >= m_vBufferContainers.size() || !m_BufferHandles.IsActive(pCommand->m_IndexBuffer) || static_cast<size_t>(pCommand->m_IndexBuffer.Id()) >= m_vBuffers.size())
				return true;
			const auto &Container = m_vBufferContainers[pCommand->m_BufferContainer.Id()];
			const auto VertexHandle = Container.m_VertexBuffer;
			if(!m_BufferHandles.IsActive(VertexHandle) || static_cast<size_t>(VertexHandle.Id()) >= m_vBuffers.size())
				return true;
			const auto &VertexBuffer = m_vBuffers[VertexHandle.Id()];
			const auto &IndexBuffer = m_vBuffers[pCommand->m_IndexBuffer.Id()];
			if(VertexBuffer.m_Usage != IGraphics::EBufferUsage::VERTEX || IndexBuffer.m_Usage != IGraphics::EBufferUsage::INDEX || VertexBuffer.m_Size == 0 || pCommand->m_IndexCount == 0)
				return true;
			const size_t IndexSize = pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? sizeof(uint16_t) : sizeof(uint32_t);
			if(pCommand->m_IndexOffset % IndexSize != 0 || pCommand->m_IndexOffset > IndexBuffer.m_Size || static_cast<uint64_t>(pCommand->m_IndexCount) * IndexSize > IndexBuffer.m_Size - pCommand->m_IndexOffset)
				return true;

			const EPipelineProgram Program = m_vPipelines[pCommand->m_Pipeline.Id()];
			const auto &Pipelines = m_aPipelineSets[m_RenderTarget.IsValid() ? 1 : 0];
			const bool Textured = pCommand->m_State.m_Texture.IsValid();
			const size_t PipelineIndex = BlendIndex(pCommand->m_State.m_BlendMode) * 2 + Textured;
			constexpr size_t QuadIndexBytes = 6 * sizeof(uint32_t);
			if(Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
			{
				const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataDualAtlas>();
				if(pCommand->m_IndexType != IGraphics::EIndexType::UINT32 || pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % QuadIndexBytes != 0 || pCommand->m_InstanceCount != 1 || Container.m_Stride != sizeof(CCommandBuffer::SVertex) || Container.m_AttributeCount != 3 || !pCommand->m_TextureBinding.IsValid() || !m_TextureBindingHandles.IsActive(pCommand->m_TextureBinding) || static_cast<size_t>(pCommand->m_TextureBinding.Id()) >= m_vTextureBindings.size() || pDrawData == nullptr || pDrawData->m_TextureSize <= 0.0f)
					return true;
				const auto &Binding = m_vTextureBindings[pCommand->m_TextureBinding.Id()];
				if(!m_TextureHandles.IsActive(Binding.m_Desc.m_aTextures[0]) || !m_TextureHandles.IsActive(Binding.m_Desc.m_aTextures[1]) || Binding.m_aBindGroups[SamplerIndex(pCommand->m_State.m_WrapMode)] == nullptr || !EnsureRenderPass())
					return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
				if(!ApplyState(pCommand->m_State, Pipelines.m_aDualAtlas[BlendIndex(pCommand->m_State.m_BlendMode)], pDrawData->m_PrimaryColor, vec2(0.0f, 0.0f), 0.0f, false, vec2(0.0f, 0.0f), vec2(1.0f, 1.0f), 0, pDrawData->m_TextureSize, pDrawData->m_SecondaryColor))
					return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
				wgpuRenderPassEncoderSetBindGroup(m_RenderPass, 1, Binding.m_aBindGroups[SamplerIndex(pCommand->m_State.m_WrapMode)], 0, nullptr);
				wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, VertexBuffer.m_Buffer, 0, VertexBuffer.m_Size);
				wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, IndexBuffer.m_Buffer, WGPUIndexFormat_Uint32, pCommand->m_IndexOffset, static_cast<uint64_t>(pCommand->m_IndexCount) * sizeof(uint32_t));
				wgpuRenderPassEncoderDrawIndexed(m_RenderPass, pCommand->m_IndexCount, 1, 0, 0, 0);
				return true;
			}
			if(Program == EPipelineProgram::QUAD_PER_ITEM || Program == EPipelineProgram::QUAD_SHARED)
			{
				const bool Shared = Program == EPipelineProgram::QUAD_SHARED;
				const uint32_t QuadCount = pCommand->m_IndexCount / 6;
				const auto *pQuadData = Shared ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataQuadTransform>() : pCommand->m_ArrayData.Get<CCommandBuffer::SDrawDataQuadTransform>(QuadCount);
				const size_t ExpectedStride = sizeof(float) * 4 + sizeof(ubvec4) + (Textured ? sizeof(float) * 2 : 0);
				const uint32_t ExpectedAttributes = Textured ? 3 : 2;
				const size_t BaseQuadOffset = pCommand->m_IndexOffset / QuadIndexBytes;
				if(pCommand->m_IndexType != IGraphics::EIndexType::UINT32 || pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % QuadIndexBytes != 0 || pCommand->m_InstanceCount != 1 || Container.m_Stride != ExpectedStride || Container.m_AttributeCount != ExpectedAttributes || pQuadData == nullptr || BaseQuadOffset > UINT32_MAX - (QuadCount - 1))
					return true;
				if(!EnsureRenderPass())
					return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
				wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, VertexBuffer.m_Buffer, 0, VertexBuffer.m_Size);
				wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, IndexBuffer.m_Buffer, WGPUIndexFormat_Uint32, pCommand->m_IndexOffset, static_cast<uint64_t>(pCommand->m_IndexCount) * sizeof(uint32_t));
				if(Shared || QuadCount == 1)
				{
					if(!ApplyState(pCommand->m_State, Pipelines.m_aQuadShared[PipelineIndex], pQuadData->m_Color, vec2(0.0f, 0.0f), pQuadData->m_Rotation, false, pQuadData->m_Offset))
						return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
					wgpuRenderPassEncoderDrawIndexed(m_RenderPass, pCommand->m_IndexCount, 1, 0, 0, 0);
					return true;
				}
				uint32_t QuadsLeft = QuadCount;
				uint32_t RenderOffset = 0;
				while(QuadsLeft > 0)
				{
					const uint32_t DrawCount = std::min<uint32_t>(QuadsLeft, GRAPHICS_MAX_QUADS_RENDER_COUNT);
					uint32_t QuadUniformOffset = 0;
					if(!WriteQuadTransforms(pQuadData + RenderOffset, DrawCount, QuadUniformOffset))
						return false;
					if(!ApplyState(pCommand->m_State, Pipelines.m_aQuadPerItem[PipelineIndex], ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), vec2(0.0f, 0.0f), 0.0f, false, vec2(0.0f, 0.0f), vec2(1.0f, 1.0f), static_cast<uint32_t>(BaseQuadOffset) + RenderOffset))
						return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
					wgpuRenderPassEncoderSetBindGroup(m_RenderPass, 2, m_QuadBindGroup, 1, &QuadUniformOffset);
					wgpuRenderPassEncoderDrawIndexed(m_RenderPass, DrawCount * 6, 1, RenderOffset * 6, 0, 0);
					RenderOffset += DrawCount;
					QuadsLeft -= DrawCount;
				}
				return true;
			}

			ColorRGBA Color(1.0f, 1.0f, 1.0f, 1.0f);
			vec2 RotationCenter(0.0f, 0.0f);
			vec2 VertexOffset(0.0f, 0.0f);
			vec2 VertexScale(1.0f, 1.0f);
			float Rotation = 0.0f;
			uint32_t InstanceCount = 1;
			bool TextureArray = false;
			WGPURenderPipeline Pipeline = nullptr;
			const CCommandBuffer::SInstanceDataPositionScaleRotation *pInstances = nullptr;
			if(Program == EPipelineProgram::PRIMITIVE)
			{
				if(Container.m_Stride != sizeof(CCommandBuffer::SVertex) || Container.m_AttributeCount != 3)
					return true;
				Pipeline = Pipelines.m_aPrimitive[PrimitivePipelineIndex(EPrimitiveType::TRIANGLES, pCommand->m_State.m_BlendMode, Textured)];
			}
			else if(Program == EPipelineProgram::PRIMITIVE_UNIFORM_COLOR)
			{
				const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>();
				if(pDrawData == nullptr || pCommand->m_InstanceCount != 1 || Container.m_Stride != sizeof(CCommandBuffer::SVertex) || Container.m_AttributeCount != 3)
					return true;
				Color = pDrawData->m_Color;
				RotationCenter = pDrawData->m_RotationCenter;
				Rotation = pDrawData->m_Rotation;
				Pipeline = Pipelines.m_aUniformColor[PipelineIndex];
			}
			else if(Program == EPipelineProgram::PRIMITIVE_INSTANCED)
			{
				const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveInstanced>();
				pInstances = pCommand->m_ArrayData.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(pCommand->m_InstanceCount);
				if(pDrawData == nullptr || pInstances == nullptr || pCommand->m_InstanceCount == 0 || Container.m_Stride != sizeof(CCommandBuffer::SVertex) || Container.m_AttributeCount != 3)
					return true;
				Color = pDrawData->m_Color;
				RotationCenter = pDrawData->m_RotationCenter;
				InstanceCount = pCommand->m_InstanceCount;
				Pipeline = Pipelines.m_aInstanced[PipelineIndex];
			}
			else if(Program == EPipelineProgram::ARRAY_COLOR || Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM)
			{
				const bool Transform = Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM;
				const auto *pColorData = Transform ? nullptr : pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColor>();
				const auto *pTransformData = Transform ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColorTransform>() : nullptr;
				const size_t ExpectedStride = Textured ? sizeof(vec2) + sizeof(ubvec4) : 0;
				const uint32_t ExpectedAttributes = Textured ? 2 : 1;
				if(pCommand->m_IndexType != IGraphics::EIndexType::UINT32 || pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % QuadIndexBytes != 0 || pCommand->m_InstanceCount != 1 || Container.m_Stride != ExpectedStride || Container.m_AttributeCount != ExpectedAttributes || (Transform ? pTransformData == nullptr : pColorData == nullptr))
					return true;
				Color = Transform ? pTransformData->m_Color : pColorData->m_Color;
				if(Transform)
				{
					VertexOffset = pTransformData->m_Offset;
					VertexScale = pTransformData->m_Scale;
				}
				TextureArray = Textured;
				Pipeline = (Transform ? Pipelines.m_aArrayColorTransform : Pipelines.m_aArrayColor)[PipelineIndex];
			}
			else
				return true;

			if(!EnsureRenderPass())
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			uint64_t InstanceOffset = 0;
			if(pInstances != nullptr && !WriteStream(pInstances, static_cast<size_t>(InstanceCount) * sizeof(*pInstances), 4, InstanceOffset))
				return false;
			if(!ApplyState(pCommand->m_State, Pipeline, Color, RotationCenter, Rotation, TextureArray, VertexOffset, VertexScale))
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, VertexBuffer.m_Buffer, 0, VertexBuffer.m_Size);
			if(pInstances != nullptr)
				wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 1, m_StreamBuffer, InstanceOffset, static_cast<size_t>(InstanceCount) * sizeof(*pInstances));
			wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, IndexBuffer.m_Buffer, pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, pCommand->m_IndexOffset, static_cast<uint64_t>(pCommand->m_IndexCount) * IndexSize);
			wgpuRenderPassEncoderDrawIndexed(m_RenderPass, pCommand->m_IndexCount, InstanceCount, 0, 0, 0);
			return true;
		}

		WGPUSurface CreateSurface() const
		{
			WGPUSurfaceDescriptor Descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
			Descriptor.label = StringView("DDNet WebGPU surface");
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::CANVAS)
			{
				WGPUEmscriptenSurfaceSourceCanvasHTMLSelector Source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
				Source.selector = StringView("#canvas");
				Descriptor.nextInChain = &Source.chain;
				return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
			}
#else
			if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::METAL)
			{
				WGPUSurfaceSourceMetalLayer Source{};
				Source.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
				Source.layer = m_NativeWindow.m_pWindow;
				Descriptor.nextInChain = &Source.chain;
				return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
			}
			if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::WINDOWS)
			{
				WGPUSurfaceSourceWindowsHWND Source{};
				Source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
				Source.hinstance = m_NativeWindow.m_pDisplay;
				Source.hwnd = m_NativeWindow.m_pWindow;
				Descriptor.nextInChain = &Source.chain;
				return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
			}
			if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::XLIB)
			{
				WGPUSurfaceSourceXlibWindow Source{};
				Source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
				Source.display = m_NativeWindow.m_pDisplay;
				Source.window = m_NativeWindow.m_WindowId;
				Descriptor.nextInChain = &Source.chain;
				return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
			}
			if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::WAYLAND)
			{
				WGPUSurfaceSourceWaylandSurface Source{};
				Source.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
				Source.display = m_NativeWindow.m_pDisplay;
				Source.surface = m_NativeWindow.m_pWindow;
				Descriptor.nextInChain = &Source.chain;
				return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
			}
#endif
			return nullptr;
		}

		bool QuerySurfaceConfiguration()
		{
			const WGPUTextureFormat PreviousFormat = m_SurfaceFormat;
			WGPUSurfaceCapabilities Capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
			if(wgpuSurfaceGetCapabilities(m_Surface, m_Adapter, &Capabilities) != WGPUStatus_Success)
			{
				m_ErrorMessage = "WebGPU could not query the presentation surface";
				return false;
			}
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			// Emdawnwebgpu from Emscripten 4.0.22 does not report canvas usages yet.
			const WGPUTextureUsage SurfaceUsages = Capabilities.usages == WGPUTextureUsage_None ? WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc : Capabilities.usages;
#else
			const WGPUTextureUsage SurfaceUsages = Capabilities.usages;
#endif
			const bool Complete = Capabilities.formatCount > 0 && Capabilities.alphaModeCount > 0 && Capabilities.presentModeCount > 0 && (SurfaceUsages & WGPUTextureUsage_CopySrc) != 0;
			if(Complete)
			{
				m_SurfaceFormat = WGPUTextureFormat_Undefined;
				for(size_t i = 0; i < Capabilities.formatCount; ++i)
				{
					const auto Format = Capabilities.formats[i];
					if(Format == WGPUTextureFormat_BGRA8Unorm || Format == WGPUTextureFormat_RGBA8Unorm)
					{
						if(m_SurfaceFormat == WGPUTextureFormat_Undefined)
							m_SurfaceFormat = Format;
						if(PreviousFormat == Format)
						{
							m_SurfaceFormat = Format;
							break;
						}
					}
				}
				m_AlphaMode = Capabilities.alphaModes[0];
				m_SupportsFifo = false;
				m_SupportsImmediate = false;
				m_SupportsMailbox = false;
				for(size_t i = 0; i < Capabilities.presentModeCount; ++i)
				{
					m_SupportsFifo |= Capabilities.presentModes[i] == WGPUPresentMode_Fifo;
					m_SupportsImmediate |= Capabilities.presentModes[i] == WGPUPresentMode_Immediate;
					m_SupportsMailbox |= Capabilities.presentModes[i] == WGPUPresentMode_Mailbox;
				}
			}
			if((SurfaceUsages & WGPUTextureUsage_CopySrc) == 0)
				m_ErrorMessage = "The selected WebGPU implementation does not support presentation readback";
			else if(!Complete)
				m_ErrorMessage = "The selected WebGPU implementation has incomplete surface capabilities";
			wgpuSurfaceCapabilitiesFreeMembers(Capabilities);
			if(!Complete || !m_SupportsFifo || m_SurfaceFormat == WGPUTextureFormat_Undefined)
				return false;
			if(m_PrimitiveShader != nullptr && PreviousFormat != WGPUTextureFormat_Undefined && PreviousFormat != m_SurfaceFormat && !CreatePipelineSet(m_aPipelineSets[0], m_SurfaceFormat, SampleCount()))
				return false;
			if(m_PresentMode != WGPUPresentMode_Fifo && !SupportsPresentMode(m_PresentMode))
			{
				log_warn("gfx/webgpu", "present mode changed after surface recreation; falling back to FIFO");
				m_PresentMode = WGPUPresentMode_Fifo;
			}
			return true;
		}

		bool SupportsPresentMode(WGPUPresentMode Mode) const
		{
			return (Mode == WGPUPresentMode_Fifo && m_SupportsFifo) || (Mode == WGPUPresentMode_Immediate && m_SupportsImmediate) || (Mode == WGPUPresentMode_Mailbox && m_SupportsMailbox);
		}

		void ReleaseFrame()
		{
			if(m_SurfaceView != nullptr)
			{
				wgpuTextureViewRelease(m_SurfaceView);
				m_SurfaceView = nullptr;
			}
			if(m_SurfaceTexture.texture != nullptr)
				wgpuTextureRelease(m_SurfaceTexture.texture);
			m_SurfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
		}

		void EndRenderPass()
		{
			if(m_RenderPass == nullptr)
				return;
			wgpuRenderPassEncoderEnd(m_RenderPass);
			wgpuRenderPassEncoderRelease(m_RenderPass);
			m_RenderPass = nullptr;
		}

		bool SubmitCommands(bool EndsFrame = false, bool PublishGpuTimestamp = true)
		{
			EndRenderPass();
			const int GpuTimestampSlot = EndsFrame ? m_GpuTimestampActiveSlot : -1;
			if(GpuTimestampSlot >= 0 && m_CommandEncoder == nullptr && !EnsureCommandEncoder())
			{
				AbandonGpuTimestamp();
				return false;
			}
			if(m_CommandEncoder == nullptr)
				return true;
			if(GpuTimestampSlot >= 0)
			{
				const uint32_t FirstQuery = static_cast<uint32_t>(GpuTimestampSlot * GPU_TIMESTAMP_QUERY_COUNT);
				const uint64_t ResolveOffset = GpuTimestampSlot * GPU_TIMESTAMP_RESOLVE_STRIDE;
				for(size_t Zone = 0; Zone < m_aGpuTimestampZoneActiveIntervals.size(); ++Zone)
				{
					const int32_t Interval = m_aGpuTimestampZoneActiveIntervals[Zone];
					if(Interval < 0)
						continue;
					const uint32_t Query = FirstQuery + 2 + static_cast<uint32_t>(Interval) * 2;
					wgpuCommandEncoderWriteTimestamp(m_CommandEncoder, m_GpuTimestampQuerySet, Query + 1);
					m_GpuTimestampInvalidZoneMask |= 1U << Zone;
				}
				wgpuCommandEncoderWriteTimestamp(m_CommandEncoder, m_GpuTimestampQuerySet, FirstQuery + 1);
				const uint32_t QueryCount = 2 + m_GpuTimestampIntervalCount * 2;
				const uint64_t QuerySize = QueryCount * sizeof(uint64_t);
				wgpuCommandEncoderResolveQuerySet(m_CommandEncoder, m_GpuTimestampQuerySet, FirstQuery, QueryCount, m_GpuTimestampResolveBuffer, ResolveOffset);
				wgpuCommandEncoderCopyBufferToBuffer(m_CommandEncoder, m_GpuTimestampResolveBuffer, ResolveOffset, m_aGpuTimestampSlots[GpuTimestampSlot].m_ReadbackBuffer, 0, QuerySize);
			}
			WGPUCommandBuffer CommandBuffer = wgpuCommandEncoderFinish(m_CommandEncoder, nullptr);
			wgpuCommandEncoderRelease(m_CommandEncoder);
			m_CommandEncoder = nullptr;
			if(CommandBuffer == nullptr)
			{
				AbandonGpuTimestamp();
				SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to finish the frame command buffer");
				return false;
			}
			const bool UsesUploadBuffers = m_StreamOffset != 0 || m_UniformOffset != 0;
			if(m_StreamOffset != 0)
				wgpuQueueWriteBuffer(m_Queue, m_StreamBuffer, m_UploadBufferSlot * STREAM_BUFFER_SIZE, m_vStreamUpload.data(), m_StreamOffset);
			if(m_UniformOffset != 0)
				wgpuQueueWriteBuffer(m_Queue, m_UniformBuffer, m_UploadBufferSlot * UNIFORM_BUFFER_SIZE, m_vUniformUpload.data(), m_UniformOffset);
			wgpuQueueSubmit(m_Queue, 1, &CommandBuffer);
			wgpuCommandBufferRelease(CommandBuffer);
			if(UsesUploadBuffers && !AdvanceUploadBufferSlot())
			{
				AbandonGpuTimestamp();
				return false;
			}
			if(m_GpuTimestampActiveSlot >= 0)
				m_GpuTimestampActiveSubmitted = true;
			if(GpuTimestampSlot >= 0)
			{
				MapGpuTimestampSlot(GpuTimestampSlot, PublishGpuTimestamp);
				m_GpuTimestampActiveSlot = -1;
				m_GpuTimestampActiveSubmitted = false;
				m_aGpuTimestampZoneActiveIntervals.fill(-1);
				m_GpuTimestampIntervalCount = 0;
				m_GpuTimestampInvalidZoneMask = 0;
			}
			m_StreamOffset = 0;
			m_UniformOffset = 0;
			return true;
		}

		bool ReadTextureData(WGPUTexture Texture, WGPUOrigin3D Origin, uint32_t Width, uint32_t Height, bool BGRA, bool OpaqueAlpha, CCommandBuffer::SImageReadbackResult &Result)
		{
			const uint32_t BytesPerRow = static_cast<uint32_t>(AlignUp(static_cast<uint64_t>(Width) * 4, 256));
			const uint64_t BufferSize = static_cast<uint64_t>(BytesPerRow) * Height;
			WGPUBufferDescriptor BufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			BufferDescriptor.label = StringView("DDNet WebGPU texture readback");
			BufferDescriptor.size = BufferSize;
			BufferDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
			WGPUBuffer Buffer = wgpuDeviceCreateBuffer(m_Device, &BufferDescriptor);
			if(Buffer == nullptr || !EnsureCommandEncoder())
			{
				if(Buffer != nullptr)
					wgpuBufferRelease(Buffer);
				return false;
			}

			EndRenderPass();
			WGPUTexelCopyTextureInfo Source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			Source.texture = Texture;
			Source.origin = Origin;
			Source.aspect = WGPUTextureAspect_All;
			WGPUTexelCopyBufferInfo Destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
			Destination.buffer = Buffer;
			Destination.layout.bytesPerRow = BytesPerRow;
			Destination.layout.rowsPerImage = Height;
			const WGPUExtent3D Extent{Width, Height, 1};
			wgpuCommandEncoderCopyTextureToBuffer(m_CommandEncoder, &Source, &Destination, &Extent);
			if(!SubmitCommands())
			{
				wgpuBufferRelease(Buffer);
				return false;
			}

			auto pMapResult = std::make_shared<SMapResult>();
			WGPUBufferMapCallbackInfo CallbackInfo = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
			CallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
			CallbackInfo.callback = MapCallback;
			CallbackInfo.userdata1 = new std::shared_ptr<SMapResult>(pMapResult);
			wgpuBufferMapAsync(Buffer, WGPUMapMode_Read, 0, BufferSize, CallbackInfo);
			if(!ProcessUntilDone(*pMapResult, "map texture readback") || pMapResult->m_Status != WGPUMapAsyncStatus_Success)
			{
				wgpuBufferRelease(Buffer);
				return false;
			}
			const auto *pMappedData = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(Buffer, 0, BufferSize));
			if(pMappedData != nullptr)
			{
				Result.m_Image.m_Width = Width;
				Result.m_Image.m_Height = Height;
				Result.m_Image.m_Format = CImageInfo::FORMAT_RGBA;
				Result.m_Image.Allocate();
				if(Result.m_Image.m_pData != nullptr)
				{
					for(uint32_t Y = 0; Y < Height; ++Y)
					{
						const uint8_t *pSource = pMappedData + static_cast<size_t>(Y) * BytesPerRow;
						uint8_t *pDestination = Result.m_Image.m_pData + static_cast<size_t>(Y) * Width * 4;
						for(uint32_t X = 0; X < Width; ++X)
						{
							pDestination[X * 4] = pSource[X * 4 + (BGRA ? 2 : 0)];
							pDestination[X * 4 + 1] = pSource[X * 4 + 1];
							pDestination[X * 4 + 2] = pSource[X * 4 + (BGRA ? 0 : 2)];
							pDestination[X * 4 + 3] = OpaqueAlpha ? 255 : pSource[X * 4 + 3];
						}
					}
					Result.m_Ok = true;
				}
			}
			wgpuBufferUnmap(Buffer);
			wgpuBufferRelease(Buffer);
			return Result.m_Ok;
		}

		void PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand)
		{
			auto &Result = *pCommand->m_pResult;
			Result.m_Ok = false;
			const bool BGRA = m_SurfaceFormat == WGPUTextureFormat_BGRA8Unorm || m_SurfaceFormat == WGPUTextureFormat_BGRA8UnormSrgb;
			if(!BGRA && m_SurfaceFormat != WGPUTextureFormat_RGBA8Unorm && m_SurfaceFormat != WGPUTextureFormat_RGBA8UnormSrgb)
			{
				log_warn("gfx/webgpu", "presentation target readback does not support surface format %d", static_cast<int>(m_SurfaceFormat));
				return;
			}
			if(!AcquireFrame())
			{
				log_warn("gfx/webgpu", "presentation target readback could not acquire a frame");
				return;
			}
			const uint32_t ViewportX = std::min(m_ViewportX, m_SurfaceWidth);
			const uint32_t ViewportY = std::min(m_ViewportY, m_SurfaceHeight);
			const uint32_t ViewportWidth = std::min(m_ViewportWidth > 0 ? m_ViewportWidth : m_SurfaceWidth, m_SurfaceWidth - ViewportX);
			const uint32_t ViewportHeight = std::min(m_ViewportHeight > 0 ? m_ViewportHeight : m_SurfaceHeight, m_SurfaceHeight - ViewportY);
			if(ViewportWidth == 0 || ViewportHeight == 0 || (pCommand->m_ReadPixel && (pCommand->m_Position.x < 0 || pCommand->m_Position.y < 0 || pCommand->m_Position.x >= static_cast<int>(ViewportWidth) || pCommand->m_Position.y >= static_cast<int>(ViewportHeight))))
				return;

			const uint32_t Width = pCommand->m_ReadPixel ? 1 : ViewportWidth;
			const uint32_t Height = pCommand->m_ReadPixel ? 1 : ViewportHeight;
			const WGPUOrigin3D Origin{ViewportX + (pCommand->m_ReadPixel ? static_cast<uint32_t>(pCommand->m_Position.x) : 0), ViewportY + (pCommand->m_ReadPixel ? static_cast<uint32_t>(pCommand->m_Position.y) : 0), 0};
			if(!ReadTextureData(m_SurfaceTexture.texture, Origin, Width, Height, BGRA, true, Result))
				log_warn("gfx/webgpu", "presentation target readback failed");
		}

		void TextureReadback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
		{
			auto &Result = *pCommand->m_pResult;
			Result.m_Ok = false;
			if(!m_TextureHandles.IsActive(pCommand->m_Texture) || static_cast<size_t>(pCommand->m_Texture.Id()) >= m_vTextures.size())
				return;
			const auto &Texture = m_vTextures[pCommand->m_Texture.Id()];
			if(Texture.m_Texture == nullptr || (Texture.m_Usage & (IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE)) != (IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE))
				return;
			if(!ReadTextureData(Texture.m_Texture, {}, static_cast<uint32_t>(Texture.m_Width), static_cast<uint32_t>(Texture.m_Height), false, false, Result))
				log_warn("gfx/webgpu", "texture readback failed");
		}

		void DiscardFrame()
		{
			EndRenderPass();
			if(m_CommandEncoder != nullptr)
			{
				wgpuCommandEncoderRelease(m_CommandEncoder);
				m_CommandEncoder = nullptr;
			}
			m_StreamOffset = 0;
			m_UniformOffset = 0;
			if(m_GpuTimestampActiveSlot >= 0)
			{
				const int GpuTimestampSlot = m_GpuTimestampActiveSlot;
				if(m_GpuTimestampActiveSubmitted)
				{
					if(!SubmitCommands(true, false))
					{
						m_aGpuTimestampSlots[GpuTimestampSlot].m_InFlight = true;
						m_GpuTimestampActiveSlot = -1;
						m_GpuTimestampActiveSubmitted = false;
					}
				}
				else
					m_GpuTimestampActiveSlot = -1;
			}
			ReleaseFrame();
			ReleaseMultisampleTarget(m_SurfaceMultisampleTexture, m_SurfaceMultisampleView, m_SurfaceMultisampleMemorySize);
			m_SurfaceSuboptimal = false;
		}

		bool RecreateSurface()
		{
			if(m_CommandEncoder != nullptr && !SubmitCommands())
				return false;
			DiscardFrame();
			if(m_SurfaceConfigured)
				wgpuSurfaceUnconfigure(m_Surface);
			m_SurfaceConfigured = false;
			if(m_Surface != nullptr)
				wgpuSurfaceRelease(m_Surface);
			m_Surface = CreateSurface();
			m_SurfaceDirty = true;
			return m_Surface != nullptr && QuerySurfaceConfiguration();
		}

		bool ConfigureIfNeeded()
		{
			if(m_SurfaceView != nullptr || m_SurfaceTexture.texture != nullptr)
				return true;
			if(m_Minimized || m_SurfaceWidth == 0 || m_SurfaceHeight == 0)
			{
				if(m_SurfaceConfigured)
					wgpuSurfaceUnconfigure(m_Surface);
				m_SurfaceConfigured = false;
				return true;
			}
			if(m_SurfaceConfigured && !m_SurfaceDirty)
				return true;
			if(m_SurfaceConfigured)
				wgpuSurfaceUnconfigure(m_Surface);
			WGPUSurfaceConfiguration Config = WGPU_SURFACE_CONFIGURATION_INIT;
			Config.device = m_Device;
			Config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
			Config.format = m_SurfaceFormat;
			Config.width = m_SurfaceWidth;
			Config.height = m_SurfaceHeight;
			Config.presentMode = m_PresentMode;
			Config.alphaMode = m_AlphaMode;
			wgpuSurfaceConfigure(m_Surface, &Config);
			m_SurfaceConfigured = true;
			m_SurfaceDirty = false;
			log_info("gfx/webgpu", "configured surface %ux%u", m_SurfaceWidth, m_SurfaceHeight);
			return true;
		}

		bool AcquireFrame()
		{
			if(m_SkipPresentationFrame)
				return false;
			if(m_SurfaceTexture.texture != nullptr)
				return true;
			if(!ConfigureIfNeeded() || !m_SurfaceConfigured)
				return false;
			for(int Attempt = 0; Attempt < 2; ++Attempt)
			{
				m_SurfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
				wgpuSurfaceGetCurrentTexture(m_Surface, &m_SurfaceTexture);
				if(m_SurfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal || m_SurfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
				{
					m_SurfaceSuboptimal = m_SurfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
					m_SurfaceView = wgpuTextureCreateView(m_SurfaceTexture.texture, nullptr);
					if(m_SurfaceView != nullptr)
						return true;
					ReleaseFrame();
					SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to create the surface texture view");
					return false;
				}
				if(m_SurfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Timeout
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
					|| m_SurfaceTexture.status == static_cast<WGPUSurfaceGetCurrentTextureStatus>(WGPUSurfaceGetCurrentTextureStatus_Occluded)
#endif
				)
				{
					ReleaseFrame();
					m_SkipPresentationFrame = true;
					return false;
				}
				if(m_SurfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated)
				{
					ReleaseFrame();
					m_SurfaceDirty = true;
					m_SkipPresentationFrame = true;
					return false;
				}
				if(m_SurfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
				{
					ReleaseFrame();
					if(Attempt == 0 && RecreateSurface() && ConfigureIfNeeded())
						continue;
				}
				else
					ReleaseFrame();
				SetError(GFX_ERROR_TYPE_SWAP_FAILED, "WebGPU failed to acquire the surface texture");
				return false;
			}
			return false;
		}

		bool Clear(const CCommandBuffer::SCommand_Clear *pCommand)
		{
			EndRenderPass();
			m_RenderPassLoadOp = WGPULoadOp_Clear;
			m_RenderPassClearColor = {pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a};
			return EnsureRenderPass() || m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
		}

		bool Present()
		{
			if(m_SurfaceTexture.texture == nullptr)
			{
				if(!SubmitCommands(true))
					return false;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
				emscripten_sleep(0);
#else
				wgpuDevicePoll(m_Device, WGPU_FALSE, nullptr);
#endif
				m_SkipPresentationFrame = false;
				m_RenderPassLoadOp = WGPULoadOp_Clear;
				return ApplyMultiSampling();
			}
			if(!SubmitCommands(true))
			{
				ReleaseFrame();
				return false;
			}
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			const WGPUStatus Status = WGPUStatus_Success;
			ReleaseFrame();
			// The canvas is composited once per refresh whatever this does, so
			// waiting for a frame is the right default: it costs nothing. Someone
			// who asked for a rate of their own gets the short yield instead and
			// lets the client's own limiter do the pacing -- for a benchmark, or
			// for the shortest path from an input to the frame that carries it.
			YieldToBrowser(g_Config.m_GfxRefreshRate == 0 ? 1 : 0);
#else
			const WGPUStatus Status = wgpuSurfacePresent(m_Surface);
			ReleaseFrame();
#endif
			m_SkipPresentationFrame = false;
			if(m_SurfaceSuboptimal)
			{
				m_SurfaceDirty = true;
				m_SurfaceSuboptimal = false;
			}
			if(Status != WGPUStatus_Success)
			{
				m_SurfaceDirty = true;
				SetError(GFX_ERROR_TYPE_SWAP_FAILED, "WebGPU failed to present the surface texture");
				return false;
			}
			if(!m_PresentedOnce)
			{
				log_info("gfx/webgpu", "presented first rendered frame");
				m_PresentedOnce = true;
			}
			m_RenderPassLoadOp = WGPULoadOp_Clear;
			return ApplyMultiSampling();
		}

		bool Initialize(const SCommand_Init *pCommand)
		{
			m_BackendMode = pCommand->m_BackendMode;
			m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
			m_pBufferMemoryUsage = pCommand->m_pBufferMemoryUsage;
			m_pStreamMemoryUsage = pCommand->m_pStreamMemoryUsage;
			m_pGpuTiming = pCommand->m_pGpuTiming;
			m_pTextureMemoryUsage->store(0, std::memory_order_relaxed);
			m_pBufferMemoryUsage->store(0, std::memory_order_relaxed);
			m_pStreamMemoryUsage->store(0, std::memory_order_relaxed);
			if(m_pGpuTiming != nullptr)
			{
				m_pGpuTiming->m_Supported.store(false, std::memory_order_relaxed);
				m_pGpuTiming->m_TimeNanoseconds.store(0, std::memory_order_relaxed);
				m_pGpuTiming->m_Sample.store(0, std::memory_order_relaxed);
			}
			m_SurfaceWidth = pCommand->m_Width;
			m_SurfaceHeight = pCommand->m_Height;
			m_MultiSamplingCount = pCommand->m_RequestedMultiSamplingCount >= 2 ? 4 : 0;
			m_NextMultiSamplingCount = m_MultiSamplingCount;
			const char *pBackendName = "auto";
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			pBackendName = "browser";
#else
			WGPUInstanceExtras Extras{};
			Extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
			switch(m_BackendType)
			{
			case EWebGpuBackendType::D3D12:
				Extras.backends = WGPUInstanceBackend_DX12;
				pBackendName = "D3D12";
				break;
			case EWebGpuBackendType::VULKAN:
				Extras.backends = WGPUInstanceBackend_Vulkan;
				pBackendName = "Vulkan";
				break;
			case EWebGpuBackendType::METAL:
				Extras.backends = WGPUInstanceBackend_Metal;
				pBackendName = "Metal";
				break;
			case EWebGpuBackendType::OPENGL:
				Extras.backends = WGPUInstanceBackend_GL;
				pBackendName = "OpenGL";
				break;
			case EWebGpuBackendType::AUTO:
				Extras.backends = WGPUInstanceBackend_All;
				pBackendName = "auto";
				break;
			}
#if !defined(CONF_PLATFORM_MACOS)
			if(m_BackendType == EWebGpuBackendType::METAL)
			{
				m_ErrorMessage = "The Metal wgpu-native backend is only available on macOS";
				return false;
			}
#endif
#if !defined(CONF_FAMILY_WINDOWS)
			if(m_BackendType == EWebGpuBackendType::D3D12)
			{
				m_ErrorMessage = "The D3D12 wgpu-native backend is only available on Windows";
				return false;
			}
#endif
#if defined(CONF_DEBUG)
			Extras.flags = WGPUInstanceFlag_Validation;
#else
			Extras.flags = WGPUInstanceFlag_DiscardHalLabels;
#endif
			if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::XLIB)
			{
				Extras.displayHandle.type = WGPUNativeDisplayHandleType_Xlib;
				Extras.displayHandle.data.xlib.display = m_NativeWindow.m_pDisplay;
				Extras.displayHandle.data.xlib.screen = 0;
			}
			else if(m_NativeWindow.m_Type == SWebGpuNativeWindow::EType::WAYLAND)
			{
				Extras.displayHandle.type = WGPUNativeDisplayHandleType_Wayland;
				Extras.displayHandle.data.wayland.display = m_NativeWindow.m_pDisplay;
			}
#endif
			log_info("gfx/webgpu", "requested backend=%s", pBackendName);
			WGPUInstanceDescriptor InstanceDescriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
			InstanceDescriptor.nextInChain = &Extras.chain;
#endif
			m_Instance = wgpuCreateInstance(&InstanceDescriptor);
			if(m_Instance == nullptr)
			{
				m_ErrorMessage = std::string(WEBGPU_IMPLEMENTATION_NAME) + " could not create the requested " + pBackendName + " backend instance";
				return false;
			}
			if(m_BackendMode == EGraphicsBackendMode::PRESENTATION)
			{
				m_Surface = CreateSurface();
				if(m_Surface == nullptr)
				{
					m_ErrorMessage = std::string(WEBGPU_IMPLEMENTATION_NAME) + " could not create the presentation surface";
					return false;
				}
			}

			m_AdapterResult = {};
			WGPURequestAdapterOptions AdapterOptions = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
			AdapterOptions.compatibleSurface = m_BackendMode == EGraphicsBackendMode::PRESENTATION ? m_Surface : nullptr;
			AdapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;
			WGPURequestAdapterCallbackInfo AdapterCallbackInfo = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
			AdapterCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
			AdapterCallbackInfo.callback = AdapterCallback;
			AdapterCallbackInfo.userdata1 = &m_AdapterResult;
			wgpuInstanceRequestAdapter(m_Instance, &AdapterOptions, AdapterCallbackInfo);
			if(!ProcessUntilDone(m_AdapterResult, "request adapter") || m_AdapterResult.m_Status != WGPURequestAdapterStatus_Success || m_AdapterResult.m_Adapter == nullptr)
			{
				if(!m_AdapterResult.m_Message.empty())
					m_ErrorMessage = m_AdapterResult.m_Message;
				if(m_ErrorMessage.empty())
					m_ErrorMessage = std::string(WEBGPU_IMPLEMENTATION_NAME) + " could not find a " + pBackendName + " adapter";
				return false;
			}
			m_Adapter = m_AdapterResult.m_Adapter;

			WGPUAdapterInfo AdapterInfo = WGPU_ADAPTER_INFO_INIT;
			if(wgpuAdapterGetInfo(m_Adapter, &AdapterInfo) == WGPUStatus_Success)
			{
				const std::string Description = ToString(AdapterInfo.description);
				const std::string Device = ToString(AdapterInfo.device);
				const std::string Vendor = ToString(AdapterInfo.vendor);
				str_copy(pCommand->m_pVendorString, Vendor.empty() ? WEBGPU_IMPLEMENTATION_NAME : Vendor.c_str(), 256);
				str_copy(pCommand->m_pVersionString, WEBGPU_IMPLEMENTATION_VERSION, 256);
				const std::string &Renderer = Device.empty() ? Description : Device;
				if(Renderer.empty())
					str_copy(pCommand->m_pRendererString, BackendName(AdapterInfo.backendType), 256);
				else
					str_format(pCommand->m_pRendererString, 256, "%s (%s)", Renderer.c_str(), BackendName(AdapterInfo.backendType));
				log_info("gfx/webgpu", "adapter=%s backend=%s", Renderer.c_str(), BackendName(AdapterInfo.backendType));
				wgpuAdapterInfoFreeMembers(AdapterInfo);
			}

			m_DeviceResult = {};
			WGPUDeviceDescriptor DeviceDescriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
			DeviceDescriptor.label = StringView("DDNet experimental WebGPU device");
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
			const std::array GpuTimestampFeatures{
				WGPUFeatureName_TimestampQuery,
				static_cast<WGPUFeatureName>(WGPUNativeFeature_TimestampQueryInsideEncoders),
				static_cast<WGPUFeatureName>(WGPUNativeFeature_TimestampQueryInsidePasses)};
			m_GpuTimestampSupported = m_pGpuTiming != nullptr && std::ranges::all_of(GpuTimestampFeatures.begin(), GpuTimestampFeatures.begin() + 2, [&](WGPUFeatureName Feature) { return wgpuAdapterHasFeature(m_Adapter, Feature); });
			m_GpuTimestampInsidePassesSupported = m_GpuTimestampSupported && wgpuAdapterHasFeature(m_Adapter, GpuTimestampFeatures[2]);
			if(m_GpuTimestampSupported)
			{
				DeviceDescriptor.requiredFeatureCount = m_GpuTimestampInsidePassesSupported ? GpuTimestampFeatures.size() : GpuTimestampFeatures.size() - 1;
				DeviceDescriptor.requiredFeatures = GpuTimestampFeatures.data();
			}
#endif
			DeviceDescriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
			DeviceDescriptor.deviceLostCallbackInfo.callback = DeviceLostCallback;
			DeviceDescriptor.deviceLostCallbackInfo.userdata1 = this;
			DeviceDescriptor.uncapturedErrorCallbackInfo.callback = UncapturedErrorCallback;
			DeviceDescriptor.uncapturedErrorCallbackInfo.userdata1 = this;
			WGPURequestDeviceCallbackInfo DeviceCallbackInfo = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
			DeviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
			DeviceCallbackInfo.callback = DeviceCallback;
			DeviceCallbackInfo.userdata1 = &m_DeviceResult;
			wgpuAdapterRequestDevice(m_Adapter, &DeviceDescriptor, DeviceCallbackInfo);
			if(!ProcessUntilDone(m_DeviceResult, "request device") || m_DeviceResult.m_Status != WGPURequestDeviceStatus_Success || m_DeviceResult.m_Device == nullptr)
			{
				if(!m_DeviceResult.m_Message.empty())
					m_ErrorMessage = m_DeviceResult.m_Message;
				if(m_ErrorMessage.empty())
					m_ErrorMessage = std::string(WEBGPU_IMPLEMENTATION_NAME) + " could not create a device for the " + pBackendName + " adapter";
				return false;
			}
			m_Device = m_DeviceResult.m_Device;
			m_Queue = wgpuDeviceGetQueue(m_Device);
			if(m_GpuTimestampSupported && m_Queue != nullptr)
			{
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
				m_GpuTimestampPeriod = wgpuQueueGetTimestampPeriod(m_Queue);
#endif
				m_GpuTimestampSupported = m_GpuTimestampPeriod > 0.0f && std::isfinite(m_GpuTimestampPeriod);
			}
			if(m_pGpuTiming != nullptr)
				m_pGpuTiming->m_Supported.store(m_GpuTimestampSupported, std::memory_order_relaxed);
			if(m_Queue == nullptr || (m_BackendMode == EGraphicsBackendMode::PRESENTATION && !QuerySurfaceConfiguration()))
				return false;
			if(m_BackendMode == EGraphicsBackendMode::PRESENTATION && !pCommand->m_VSync)
			{
				const WGPUPresentMode PresentMode = m_SupportsImmediate ? WGPUPresentMode_Immediate : WGPUPresentMode_Mailbox;
				if(SupportsPresentMode(PresentMode))
					m_PresentMode = PresentMode;
			}
			if(!CreateDrawResources())
				return false;

			*pCommand->m_pCapabilities = {};
			pCommand->m_pCapabilities->m_ContextMajor = 1;
			pCommand->m_pCapabilities->m_ContextMinor = 0;
			pCommand->m_pCapabilities->m_ContextPatch = 0;
			pCommand->m_pCapabilities->m_NPOTTextures = true;
			pCommand->m_pCapabilities->m_MipMapping = true;
			pCommand->m_pCapabilities->m_ShaderSupport = true;
			pCommand->m_pCapabilities->m_2DArrayTextures = true;
			pCommand->m_pCapabilities->m_ArrayColorPipelines = true;
			pCommand->m_pCapabilities->m_BufferedPrimitivePipelines = true;
			pCommand->m_pCapabilities->m_QuadPipelines = true;
			pCommand->m_pCapabilities->m_DualAtlasPipeline = true;
			pCommand->m_pCapabilities->m_RenderTargets = true;
			pCommand->m_pCapabilities->m_PlanarYuvConversion = true;
			m_ViewportWidth = m_SurfaceWidth;
			m_ViewportHeight = m_SurfaceHeight;
			m_SurfaceDirty = true;
			return m_BackendMode == EGraphicsBackendMode::OFFSCREEN || ConfigureIfNeeded();
		}

		void Cleanup()
		{
			DiscardFrame();
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
			if(m_Device != nullptr)
				wgpuDevicePoll(m_Device, WGPU_TRUE, nullptr);
#endif
			if(m_Instance != nullptr)
				wgpuInstanceProcessEvents(m_Instance);
			for(auto &Result : m_aUploadBufferResults)
			{
				if(Result.m_Pending)
					ProcessUntilDone(Result, "wait for WebGPU upload buffers during cleanup");
				Result.m_Pending = false;
			}
			CollectGpuTimestampResults();
			DestroyGpuTimestampResources();
			if(m_pGpuTiming != nullptr)
				m_pGpuTiming->m_Supported.store(false, std::memory_order_relaxed);
			DestroyDrawResources();
			if(m_SurfaceConfigured && m_Surface != nullptr)
				wgpuSurfaceUnconfigure(m_Surface);
			m_SurfaceConfigured = false;
			if(m_Queue != nullptr)
				wgpuQueueRelease(m_Queue);
			if(m_Device != nullptr)
				wgpuDeviceRelease(m_Device);
			if(m_Adapter != nullptr)
				wgpuAdapterRelease(m_Adapter);
			if(m_Surface != nullptr)
				wgpuSurfaceRelease(m_Surface);
			if(m_Instance != nullptr)
				wgpuInstanceRelease(m_Instance);
			m_Queue = nullptr;
			m_Device = nullptr;
			m_Adapter = nullptr;
			m_Surface = nullptr;
			m_Instance = nullptr;
		}

	public:
		CCommandProcessorFragment_WebGpu(const SWebGpuNativeWindow &NativeWindow, EWebGpuBackendType BackendType) :
			m_NativeWindow(NativeWindow),
			m_BackendType(BackendType)
		{
		}

		~CCommandProcessorFragment_WebGpu() override
		{
			dbg_assert(m_Instance == nullptr, "WebGPU resources must be released on the graphics worker");
		}

		ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override
		{
			if(m_Instance != nullptr && pBaseCommand->m_Cmd == CCommandBuffer::CMD_SWAP)
			{
				wgpuInstanceProcessEvents(m_Instance);
				CollectGpuTimestampResults();
			}
			if(m_UncapturedError.exchange(false, std::memory_order_acq_rel) && m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
			{
				log_error("gfx/webgpu", "uncaptured WebGPU validation or device error");
				SetError(GFX_ERROR_TYPE_UNKNOWN, "WebGPU reported an uncaptured validation or device error");
			}
			if(m_DeviceLost && m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
				SetError(GFX_ERROR_TYPE_UNKNOWN, "WebGPU device was lost");
			if(m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
			{
				Cleanup();
				return RUN_COMMAND_COMMAND_ERROR;
			}
			switch(pBaseCommand->m_Cmd)
			{
			case CMD_PRE_INIT:
			{
				auto *pCommand = static_cast<const SCommand_PreInit *>(pBaseCommand);
				str_copy(pCommand->m_pVendorString, WEBGPU_IMPLEMENTATION_NAME, 256);
				str_copy(pCommand->m_pVersionString, WEBGPU_IMPLEMENTATION_VERSION, 256);
				str_copy(pCommand->m_pRendererString, "pending adapter selection", 256);
				return RUN_COMMAND_COMMAND_HANDLED;
			}
			case CMD_INIT:
			{
				auto *pCommand = static_cast<const SCommand_Init *>(pBaseCommand);
				if(Initialize(pCommand))
					return RUN_COMMAND_COMMAND_HANDLED;
				*pCommand->m_pInitError = -1;
				if(m_ErrorMessage.empty())
					m_ErrorMessage = "WebGPU initialization failed";
				log_error("gfx/webgpu", "%s", m_ErrorMessage.c_str());
				*pCommand->m_pErrStringPtr = "WebGPU initialization failed";
				Cleanup();
				m_Warning.m_WarningType = GFX_WARNING_TYPE_INIT_FAILED;
				return RUN_COMMAND_COMMAND_WARNING;
			}
			case CMD_SHUTDOWN:
				Cleanup();
				return RUN_COMMAND_COMMAND_HANDLED;
			case CMD_POST_SHUTDOWN:
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_UPDATE_VIEWPORT:
			{
				auto *pCommand = static_cast<const CCommandBuffer::SCommand_Update_Viewport *>(pBaseCommand);
				m_ViewportX = std::max(pCommand->m_X, 0);
				m_ViewportY = std::max(pCommand->m_Y, 0);
				m_ViewportWidth = std::max(pCommand->m_Width, 0);
				m_ViewportHeight = std::max(pCommand->m_Height, 0);
				if(pCommand->m_ByResize)
				{
					const uint32_t SurfaceWidth = pCommand->m_SurfaceWidth > 0 ? pCommand->m_SurfaceWidth : 0;
					const uint32_t SurfaceHeight = pCommand->m_SurfaceHeight > 0 ? pCommand->m_SurfaceHeight : 0;
					const bool Minimized = SurfaceWidth == 0 || SurfaceHeight == 0;
					if(SurfaceWidth != m_SurfaceWidth || SurfaceHeight != m_SurfaceHeight || Minimized != m_Minimized)
					{
						if(m_CommandEncoder != nullptr && !SubmitCommands())
							return RUN_COMMAND_COMMAND_ERROR;
						DiscardFrame();
						m_SurfaceWidth = SurfaceWidth;
						m_SurfaceHeight = SurfaceHeight;
						m_Minimized = Minimized;
						m_SurfaceDirty = true;
						ConfigureIfNeeded();
					}
				}
				return RUN_COMMAND_COMMAND_HANDLED;
			}
			case CCommandBuffer::CMD_TEXTURE_CREATE:
				if(!CreateTexture(static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_TEXTURE_UPDATE:
				if(!UpdateTexture(static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_TEXTURE_READBACK:
				TextureReadback(static_cast<const CCommandBuffer::SCommand_Texture_Readback *>(pBaseCommand));
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_TEXTURE_DESTROY:
				DestroyTexture(static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand)->m_Texture);
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_TEXTURE_BINDING_CREATE:
				if(!CreateTextureBinding(static_cast<const CCommandBuffer::SCommand_TextureBinding_Create *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_TEXTURE_BINDING_DESTROY:
				DestroyTextureBinding(static_cast<const CCommandBuffer::SCommand_TextureBinding_Destroy *>(pBaseCommand)->m_Binding);
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_PIPELINE_CREATE:
				CreatePipeline(static_cast<const CCommandBuffer::SCommand_Pipeline_Create *>(pBaseCommand));
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_PIPELINE_DESTROY:
				DestroyPipeline(static_cast<const CCommandBuffer::SCommand_Pipeline_Destroy *>(pBaseCommand)->m_Pipeline);
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_CREATE_BUFFER_OBJECT:
				if(!CreateBuffer(static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT:
				if(!RecreateBuffer(static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_UPDATE_BUFFER_OBJECT:
				if(!UpdateBuffer(static_cast<const CCommandBuffer::SCommand_UpdateBufferObject *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_COPY_BUFFER_OBJECT:
				if(!CopyBuffer(static_cast<const CCommandBuffer::SCommand_CopyBufferObject *>(pBaseCommand)))
					return RUN_COMMAND_COMMAND_ERROR;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_DELETE_BUFFER_OBJECT:
				return DestroyBuffer(static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand)->m_Buffer) ? RUN_COMMAND_COMMAND_HANDLED : RUN_COMMAND_COMMAND_ERROR;
			case CCommandBuffer::CMD_CREATE_BUFFER_CONTAINER:
				CreateBufferContainer(static_cast<const CCommandBuffer::SCommand_CreateBufferContainer *>(pBaseCommand));
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_UPDATE_BUFFER_CONTAINER:
				UpdateBufferContainer(static_cast<const CCommandBuffer::SCommand_UpdateBufferContainer *>(pBaseCommand));
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_DELETE_BUFFER_CONTAINER:
				return DestroyBufferContainer(static_cast<const CCommandBuffer::SCommand_DeleteBufferContainer *>(pBaseCommand)) ? RUN_COMMAND_COMMAND_HANDLED : RUN_COMMAND_COMMAND_ERROR;
			case CCommandBuffer::CMD_GPU_RENDER_ZONE:
				return GpuRenderZone(static_cast<const CCommandBuffer::SCommand_GpuRenderZone *>(pBaseCommand)) ? RUN_COMMAND_COMMAND_HANDLED : RUN_COMMAND_COMMAND_ERROR;
			case CCommandBuffer::CMD_BEGIN_RENDER_PASS:
			{
				const auto *pCommand = static_cast<const CCommandBuffer::SCommand_BeginRenderPass *>(pBaseCommand);
				EndRenderPass();
				m_RenderTarget.Invalidate();
				if(pCommand->m_Desc.m_ColorTarget.IsValid())
				{
					const auto Target = pCommand->m_Desc.m_ColorTarget;
					if(!m_TextureHandles.IsActive(Target) || static_cast<size_t>(Target.Id()) >= m_vTextures.size() || (m_vTextures[Target.Id()].m_Usage & IGraphics::TEXTURE_USAGE_COLOR_TARGET) == 0)
						return RUN_COMMAND_COMMAND_HANDLED;
					m_RenderTarget = Target;
				}
				m_RenderPassLoadOp = WGPULoadOp_Clear;
				const ColorRGBA ClearColor = pCommand->m_Desc.m_LoadOp == IGraphics::ERenderPassLoadOp::CLEAR ? pCommand->m_Desc.m_ClearColor : ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
				m_RenderPassClearColor = {ClearColor.r, ClearColor.g, ClearColor.b, ClearColor.a};
				if(!EnsureRenderPass() && m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
					SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to begin a render pass");
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE ? RUN_COMMAND_COMMAND_HANDLED : RUN_COMMAND_COMMAND_ERROR;
			}
			case CCommandBuffer::CMD_END_RENDER_PASS:
				EndRenderPass();
				m_RenderTarget.Invalidate();
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_FLUSH_RENDER_PASS:
				EndRenderPass();
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_CLEAR:
				if(!Clear(static_cast<const CCommandBuffer::SCommand_Clear *>(pBaseCommand)))
					SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to record the clear pass");
				if(m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
					Cleanup();
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE ? RUN_COMMAND_COMMAND_HANDLED : RUN_COMMAND_COMMAND_ERROR;
			case CCommandBuffer::CMD_DRAW:
				if(!Draw(static_cast<const CCommandBuffer::SCommand_Draw *>(pBaseCommand)))
				{
					if(m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
						SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to record an immediate draw");
					return RUN_COMMAND_COMMAND_ERROR;
				}
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_DRAW_INDEXED:
			{
				const auto *pCommand = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand);
				const bool Drawn = pCommand->IsTransient() ? DrawTransient(pCommand) : DrawBuffered(pCommand);
				if(!Drawn)
				{
					if(m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
						SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to record a transient draw");
					return RUN_COMMAND_COMMAND_ERROR;
				}
				return RUN_COMMAND_COMMAND_HANDLED;
			}
			case CCommandBuffer::CMD_PRESENTATION_TARGET_READBACK:
				PresentationTargetReadback(static_cast<const CCommandBuffer::SCommand_PresentationTarget_Readback *>(pBaseCommand));
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_SWAP:
				if(!Present())
				{
					Cleanup();
					return RUN_COMMAND_COMMAND_ERROR;
				}
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_WINDOW_DESTROY_NTF:
				log_info("gfx/webgpu", "surface suspended");
				if(m_CommandEncoder != nullptr && !SubmitCommands())
					return RUN_COMMAND_COMMAND_ERROR;
				DiscardFrame();
				m_Minimized = true;
				m_SurfaceDirty = true;
				ConfigureIfNeeded();
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_WINDOW_CREATE_NTF:
				log_info("gfx/webgpu", "surface resumed");
				m_Minimized = false;
				m_SurfaceDirty = true;
				return RUN_COMMAND_COMMAND_HANDLED;
			case CCommandBuffer::CMD_VSYNC:
			{
				auto *pCommand = static_cast<const CCommandBuffer::SCommand_VSync *>(pBaseCommand);
				pCommand->m_pResult->m_Ok = false;
				const WGPUPresentMode Requested = pCommand->m_VSync ? WGPUPresentMode_Fifo : (m_SupportsImmediate ? WGPUPresentMode_Immediate : WGPUPresentMode_Mailbox);
				if(SupportsPresentMode(Requested))
				{
					m_PresentMode = Requested;
					m_SurfaceDirty = true;
					ConfigureIfNeeded();
					pCommand->m_pResult->m_Ok = true;
				}
				return RUN_COMMAND_COMMAND_HANDLED;
			}
			case CCommandBuffer::CMD_MULTISAMPLING:
			{
				auto *pCommand = static_cast<const CCommandBuffer::SCommand_MultiSampling *>(pBaseCommand);
				m_NextMultiSamplingCount = pCommand->m_RequestedMultiSamplingCount >= 2 ? 4 : 0;
				pCommand->m_pResult->m_MultiSamplingCount = m_NextMultiSamplingCount;
				pCommand->m_pResult->m_Ok = true;
				return RUN_COMMAND_COMMAND_HANDLED;
			}
			default:
				return RUN_COMMAND_COMMAND_UNHANDLED;
			}
		}
	};
}

CCommandProcessorFragment_Renderer *CreateWebGpuCommandProcessorFragment(const SWebGpuNativeWindow &NativeWindow, EWebGpuBackendType BackendType)
{
	return new CCommandProcessorFragment_WebGpu(NativeWindow, BackendType);
}

EWebGpuBackendType WebGpuBackendTypeFromConfig()
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "auto") != 0)
	{
		log_warn("gfx", "native WebGPU backend selection '%s' is unavailable in the browser, using auto", g_Config.m_GfxWebGpuBackend);
		str_copy(g_Config.m_GfxWebGpuBackend, "auto");
	}
#else
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "D3D12") == 0 || str_comp_nocase(g_Config.m_GfxWebGpuBackend, "DX12") == 0)
		return EWebGpuBackendType::D3D12;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "Vulkan") == 0)
		return EWebGpuBackendType::VULKAN;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "Metal") == 0)
		return EWebGpuBackendType::METAL;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "OpenGL") == 0)
		return EWebGpuBackendType::OPENGL;
	if(str_comp_nocase(g_Config.m_GfxWebGpuBackend, "auto") != 0)
	{
		log_warn("gfx", "unknown wgpu-native backend '%s', using auto", g_Config.m_GfxWebGpuBackend);
		str_copy(g_Config.m_GfxWebGpuBackend, "auto");
	}
#endif
	return EWebGpuBackendType::AUTO;
}

#endif
