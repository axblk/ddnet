#include <engine/client/backend/webgpu/backend_webgpu_processor.h>

#if defined(CONF_BACKEND_WEBGPU)

// ---------------------------------------------------------------------------
// Core: adapter and device, callbacks, encoder and submission, timestamps,
// dispatch.
// ---------------------------------------------------------------------------

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

void CCommandProcessorFragment_WebGpu::AdapterCallback(WGPURequestAdapterStatus Status, WGPUAdapter Adapter, WGPUStringView Message, void *pUserdata1, void *)
{
	auto *pResult = static_cast<SRequestAdapterResult *>(pUserdata1);
	pResult->m_Status = Status;
	pResult->m_Adapter = Adapter;
	pResult->m_Message = ToString(Message);
	pResult->m_Done = true;
}

void CCommandProcessorFragment_WebGpu::DeviceCallback(WGPURequestDeviceStatus Status, WGPUDevice Device, WGPUStringView Message, void *pUserdata1, void *)
{
	auto *pResult = static_cast<SRequestDeviceResult *>(pUserdata1);
	pResult->m_Status = Status;
	pResult->m_Device = Device;
	pResult->m_Message = ToString(Message);
	pResult->m_Done = true;
}

void CCommandProcessorFragment_WebGpu::MapCallback(WGPUMapAsyncStatus Status, WGPUStringView, void *pUserdata1, void *)
{
	auto pResult = std::move(*static_cast<std::shared_ptr<SMapResult> *>(pUserdata1));
	delete static_cast<std::shared_ptr<SMapResult> *>(pUserdata1);
	pResult->m_Status = Status;
	pResult->m_Done = true;
}

void CCommandProcessorFragment_WebGpu::QueueCallback(WGPUQueueWorkDoneStatus Status, WGPUStringView, void *pUserdata1, void *)
{
	auto *pResult = static_cast<SQueueResult *>(pUserdata1);
	pResult->m_Status = Status;
	pResult->m_Done = true;
}

void CCommandProcessorFragment_WebGpu::DeviceLostCallback(WGPUDevice const *, WGPUDeviceLostReason Reason, WGPUStringView Message, void *pUserdata1, void *)
{
	if(Reason == WGPUDeviceLostReason_CallbackCancelled)
		return;
	auto *pSelf = static_cast<CCommandProcessorFragment_WebGpu *>(pUserdata1);
	pSelf->m_DeviceLost = true;
	log_error("gfx/webgpu", "device lost (%d): %.*s", static_cast<int>(Reason), static_cast<int>(Message.length), Message.data != nullptr ? Message.data : "");
}

void CCommandProcessorFragment_WebGpu::UncapturedErrorCallback(WGPUDevice const *, WGPUErrorType Type, WGPUStringView Message, void *pUserdata1, void *)
{
	auto *pSelf = static_cast<CCommandProcessorFragment_WebGpu *>(pUserdata1);
	pSelf->m_UncapturedError.store(true, std::memory_order_release);
	log_error("gfx/webgpu", "uncaptured error (%d): %.*s", static_cast<int>(Type), static_cast<int>(Message.length), Message.data != nullptr ? Message.data : "");
}

void CCommandProcessorFragment_WebGpu::SetError(EGfxErrorType Type, const std::string &Message)
{
	if(m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
		return;
	m_Error.m_ErrorType = Type;
	m_Error.m_vErrors.push_back(Message);
}

void CCommandProcessorFragment_WebGpu::DestroyGpuTimestampResources()
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
}

bool CCommandProcessorFragment_WebGpu::EnsureGpuTimestampResources()
{
	if(m_GpuTimestampQuerySet != nullptr)
		return true;
	if(!m_GpuTimestampSupported || m_GpuTimestampResourcesFailed)
		return false;

	WGPUQuerySetDescriptor QuerySetDescriptor = WGPU_QUERY_SET_DESCRIPTOR_INIT;
	QuerySetDescriptor.label = StringView("DDNet WebGPU frame timestamps");
	QuerySetDescriptor.type = WGPUQueryType_Timestamp;
	QuerySetDescriptor.count = GPU_TIMESTAMP_SLOT_COUNT * 2;
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

void CCommandProcessorFragment_WebGpu::CollectGpuTimestampResults()
{
	for(auto &Slot : m_aGpuTimestampSlots)
	{
		if(!Slot.m_InFlight || Slot.m_pMapResult == nullptr || !Slot.m_pMapResult->m_Done)
			continue;
		if(Slot.m_pMapResult->m_Status == WGPUMapAsyncStatus_Success)
		{
			const auto *pMappedData = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(Slot.m_ReadbackBuffer, 0, GPU_TIMESTAMP_SIZE));
			if(pMappedData != nullptr && Slot.m_Publish)
			{
				uint64_t Start;
				uint64_t End;
				std::memcpy(&Start, pMappedData, sizeof(Start));
				std::memcpy(&End, pMappedData + sizeof(Start), sizeof(End));
				const double Duration = End >= Start ? static_cast<double>(End - Start) * m_GpuTimestampPeriod : -1.0;
				if(Duration >= 0.0 && std::isfinite(Duration) && Duration <= std::numeric_limits<uint64_t>::max())
					m_pGpuTiming->Publish(static_cast<uint64_t>(Duration + 0.5));
			}
			wgpuBufferUnmap(Slot.m_ReadbackBuffer);
		}
		Slot.m_pMapResult.reset();
		Slot.m_InFlight = false;
		Slot.m_Publish = false;
	}
}

void CCommandProcessorFragment_WebGpu::BeginGpuTimestamp()
{
	if(m_GpuTimestampActiveSlot >= 0 || !m_GpuTimestampSupported || m_pGpuTiming == nullptr || !m_pGpuTiming->m_Enabled.load(std::memory_order_relaxed) || !EnsureGpuTimestampResources())
		return;
	for(size_t i = 0; i < m_aGpuTimestampSlots.size(); ++i)
	{
		if(m_aGpuTimestampSlots[i].m_InFlight)
			continue;
		m_GpuTimestampActiveSlot = static_cast<int>(i);
		m_GpuTimestampActiveSubmitted = false;
		wgpuCommandEncoderWriteTimestamp(m_CommandEncoder, m_GpuTimestampQuerySet, static_cast<uint32_t>(i * 2));
		return;
	}
}

void CCommandProcessorFragment_WebGpu::MapGpuTimestampSlot(int SlotIndex, bool Publish)
{
	auto &Slot = m_aGpuTimestampSlots[SlotIndex];
	Slot.m_pMapResult = std::make_shared<SMapResult>();
	Slot.m_InFlight = true;
	Slot.m_Publish = Publish;
	WGPUBufferMapCallbackInfo CallbackInfo = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	CallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
	CallbackInfo.callback = MapCallback;
	CallbackInfo.userdata1 = new std::shared_ptr<SMapResult>(Slot.m_pMapResult);
	wgpuBufferMapAsync(Slot.m_ReadbackBuffer, WGPUMapMode_Read, 0, GPU_TIMESTAMP_SIZE, CallbackInfo);
}

bool CCommandProcessorFragment_WebGpu::EnsureCommandEncoder()
{
	if(m_CommandEncoder != nullptr)
		return true;
	WGPUCommandEncoderDescriptor Descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
	Descriptor.label = StringView("DDNet WebGPU frame encoder");
	m_CommandEncoder = wgpuDeviceCreateCommandEncoder(m_Device, &Descriptor);
	if(m_CommandEncoder == nullptr)
	{
		SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU could not create a command encoder");
		return false;
	}
	BeginGpuTimestamp();
	return true;
}

bool CCommandProcessorFragment_WebGpu::SubmitCommands(bool EndsFrame, bool PublishGpuTimestamp)
{
	EndRenderPass();
	const int GpuTimestampSlot = EndsFrame ? m_GpuTimestampActiveSlot : -1;
	if(GpuTimestampSlot >= 0 && m_CommandEncoder == nullptr && !EnsureCommandEncoder())
		return false;
	if(m_CommandEncoder == nullptr)
		return true;
	if(GpuTimestampSlot >= 0)
	{
		const uint32_t FirstQuery = static_cast<uint32_t>(GpuTimestampSlot * 2);
		const uint64_t ResolveOffset = GpuTimestampSlot * GPU_TIMESTAMP_RESOLVE_STRIDE;
		wgpuCommandEncoderWriteTimestamp(m_CommandEncoder, m_GpuTimestampQuerySet, FirstQuery + 1);
		wgpuCommandEncoderResolveQuerySet(m_CommandEncoder, m_GpuTimestampQuerySet, FirstQuery, 2, m_GpuTimestampResolveBuffer, ResolveOffset);
		wgpuCommandEncoderCopyBufferToBuffer(m_CommandEncoder, m_GpuTimestampResolveBuffer, ResolveOffset, m_aGpuTimestampSlots[GpuTimestampSlot].m_ReadbackBuffer, 0, GPU_TIMESTAMP_SIZE);
	}
	WGPUCommandBuffer CommandBuffer = wgpuCommandEncoderFinish(m_CommandEncoder, nullptr);
	wgpuCommandEncoderRelease(m_CommandEncoder);
	m_CommandEncoder = nullptr;
	if(CommandBuffer == nullptr)
	{
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
		return false;
	if(m_GpuTimestampActiveSlot >= 0)
		m_GpuTimestampActiveSubmitted = true;
	if(GpuTimestampSlot >= 0)
	{
		MapGpuTimestampSlot(GpuTimestampSlot, PublishGpuTimestamp);
		m_GpuTimestampActiveSlot = -1;
		m_GpuTimestampActiveSubmitted = false;
	}
	m_StreamOffset = 0;
	m_UniformOffset = 0;
	return true;
}

bool CCommandProcessorFragment_WebGpu::Clear(const CCommandBuffer::SCommand_Clear *pCommand)
{
	EndRenderPass();
	m_RenderPassLoadOp = WGPULoadOp_Clear;
	m_RenderPassClearColor = {pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a};
	return EnsureRenderPass() || m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
}

bool CCommandProcessorFragment_WebGpu::Initialize(const SCommand_Init *pCommand)
{
	m_Presentation = pCommand->m_Surface;
	const SWebGpuNativeWindow &NativeWindow = this->NativeWindow();
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
	m_SurfaceWidth = pCommand->m_Surface.m_Width;
	m_SurfaceHeight = pCommand->m_Surface.m_Height;
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
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::XLIB)
	{
		Extras.displayHandle.type = WGPUNativeDisplayHandleType_Xlib;
		Extras.displayHandle.data.xlib.display = NativeWindow.m_pDisplay;
		Extras.displayHandle.data.xlib.screen = 0;
	}
	else if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::WAYLAND)
	{
		Extras.displayHandle.type = WGPUNativeDisplayHandleType_Wayland;
		Extras.displayHandle.data.wayland.display = NativeWindow.m_pDisplay;
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
	if(m_Presentation.IsPresentable())
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
	AdapterOptions.compatibleSurface = m_Presentation.IsPresentable() ? m_Surface : nullptr;
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
		const std::string Vendor = ToString(AdapterInfo.vendor);
		str_copy(pCommand->m_pVendorString, Vendor.empty() ? WEBGPU_IMPLEMENTATION_NAME : Vendor.c_str(), 256);
		str_copy(pCommand->m_pVersionString, WEBGPU_IMPLEMENTATION_VERSION, 256);
		str_copy(pCommand->m_pRendererString, Description.empty() ? BackendName(AdapterInfo.backendType) : Description.c_str(), 256);
		log_info("gfx/webgpu", "adapter=%s backend=%s", Description.c_str(), BackendName(AdapterInfo.backendType));
		wgpuAdapterInfoFreeMembers(AdapterInfo);
	}

	m_DeviceResult = {};
	WGPUDeviceDescriptor DeviceDescriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
	DeviceDescriptor.label = StringView("DDNet experimental WebGPU device");
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	const std::array GpuTimestampFeatures{
		WGPUFeatureName_TimestampQuery,
		static_cast<WGPUFeatureName>(WGPUNativeFeature_TimestampQueryInsideEncoders)};
	m_GpuTimestampSupported = m_pGpuTiming != nullptr && std::ranges::all_of(GpuTimestampFeatures, [&](WGPUFeatureName Feature) { return wgpuAdapterHasFeature(m_Adapter, Feature); });
	if(m_GpuTimestampSupported)
	{
		DeviceDescriptor.requiredFeatureCount = GpuTimestampFeatures.size();
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
	if(m_Queue == nullptr || (m_Presentation.IsPresentable() && !QuerySurfaceConfiguration()))
		return false;
	if(m_Presentation.IsPresentable() && !pCommand->m_VSync)
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
	pCommand->m_pCapabilities->m_2DArrayTextures = true;
	pCommand->m_pCapabilities->m_RenderTargets = true;
	pCommand->m_pCapabilities->m_PlanarYuvConversion = true;
	m_ViewportWidth = m_SurfaceWidth;
	m_ViewportHeight = m_SurfaceHeight;
	m_SurfaceDirty = true;
	return !m_Presentation.IsPresentable() || ConfigureIfNeeded();
}

void CCommandProcessorFragment_WebGpu::Cleanup()
{
	DiscardFrame();
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
	if(m_Device != nullptr)
		wgpuDevicePoll(m_Device, WGPU_TRUE, nullptr);
#endif
	if(m_Instance != nullptr)
		wgpuInstanceProcessEvents(m_Instance);
	// Whoever is waiting for a picture has to be told one way or the
	// other, and after this there is nothing left to tell them with.
	if(m_Instance != nullptr && m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE)
		(void)FinishReadbacks();
	AbandonReadbacks();
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

ERunCommandReturnTypes CCommandProcessorFragment_WebGpu::RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
{
	if(m_Instance != nullptr && pBaseCommand->m_Cmd == CCommandBuffer::CMD_SWAP)
	{
		wgpuInstanceProcessEvents(m_Instance);
		CollectGpuTimestampResults();
	}
	// A readback whose mapping already arrived is handed over here, so a
	// caller polling with IsReady never has to send anything to find out.
	if(m_Instance != nullptr)
		CollectFinishedReadbacks();
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
	case CCommandBuffer::CMD_FINISH_READBACKS:
		if(!FinishReadbacks())
			return RUN_COMMAND_COMMAND_ERROR;
		return RUN_COMMAND_COMMAND_HANDLED;
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
	case CCommandBuffer::CMD_CREATE_BUFFER_OBJECT:
		if(!CreateBuffer(static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(pBaseCommand)))
			return RUN_COMMAND_COMMAND_ERROR;
		return RUN_COMMAND_COMMAND_HANDLED;
	case CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT:
		if(!RecreateBuffer(static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand)))
			return RUN_COMMAND_COMMAND_ERROR;
		return RUN_COMMAND_COMMAND_HANDLED;
	case CCommandBuffer::CMD_DELETE_BUFFER_OBJECT:
		return DestroyBuffer(static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand)->m_Buffer) ? RUN_COMMAND_COMMAND_HANDLED : RUN_COMMAND_COMMAND_ERROR;
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
		// A pass on the presentation target opens with the first thing
		// drawn on it, the same way the one a swap leaves behind does.
		// An export frame is handed this pass and then renders offscreen:
		// acquiring the canvas for it would hold the surface texture
		// across the readback wait, and what the browser composites when
		// that wait gives it its turn is the empty frame this pass cleared.
		if(m_RenderTarget.IsValid() && !EnsureRenderPass() && m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
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
		const bool Drawn = DrawBuffered(static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand));
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
		// The window may be a different one now, and a surface built
		// on the old one is not usable anymore. The SDL backend has
		// already read the new one by the time this arrives.
		if(!RecreateSurface())
			return RUN_COMMAND_COMMAND_ERROR;
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

// ---------------------------------------------------------------------------
// Surface: screen image, multisampling and presenting a frame.
// ---------------------------------------------------------------------------

uint32_t CCommandProcessorFragment_WebGpu::SampleCount() const
{
	return m_MultiSamplingCount == 0 ? 1 : m_MultiSamplingCount;
}

void CCommandProcessorFragment_WebGpu::ReleaseMultisampleTarget(WGPUTexture &Texture, WGPUTextureView &View, size_t &MemorySize)
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

bool CCommandProcessorFragment_WebGpu::CreateMultisampleTarget(WGPUTextureFormat Format, uint32_t Width, uint32_t Height, uint32_t SampleCount, WGPUTexture &Texture, WGPUTextureView &View, size_t &MemorySize)
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

bool CCommandProcessorFragment_WebGpu::ApplyMultiSampling()
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

WGPUSurface CCommandProcessorFragment_WebGpu::CreateSurface() const
{
	const SWebGpuNativeWindow &NativeWindow = this->NativeWindow();
	WGPUSurfaceDescriptor Descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
	Descriptor.label = StringView("DDNet WebGPU surface");
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::CANVAS)
	{
		WGPUEmscriptenSurfaceSourceCanvasHTMLSelector Source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
		Source.selector = StringView("#canvas");
		Descriptor.nextInChain = &Source.chain;
		return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
	}
#else
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::METAL)
	{
		WGPUSurfaceSourceMetalLayer Source{};
		Source.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
		Source.layer = NativeWindow.m_pWindow;
		Descriptor.nextInChain = &Source.chain;
		return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
	}
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::WINDOWS)
	{
		WGPUSurfaceSourceWindowsHWND Source{};
		Source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
		Source.hinstance = NativeWindow.m_pDisplay;
		Source.hwnd = NativeWindow.m_pWindow;
		Descriptor.nextInChain = &Source.chain;
		return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
	}
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::XLIB)
	{
		WGPUSurfaceSourceXlibWindow Source{};
		Source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
		Source.display = NativeWindow.m_pDisplay;
		Source.window = NativeWindow.m_WindowId;
		Descriptor.nextInChain = &Source.chain;
		return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
	}
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::WAYLAND)
	{
		WGPUSurfaceSourceWaylandSurface Source{};
		Source.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
		Source.display = NativeWindow.m_pDisplay;
		Source.surface = NativeWindow.m_pWindow;
		Descriptor.nextInChain = &Source.chain;
		return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
	}
	if(NativeWindow.m_Type == SWebGpuNativeWindow::EType::ANDROID)
	{
		WGPUSurfaceSourceAndroidNativeWindow Source{};
		Source.chain.sType = WGPUSType_SurfaceSourceAndroidNativeWindow;
		Source.window = NativeWindow.m_pWindow;
		Descriptor.nextInChain = &Source.chain;
		return wgpuInstanceCreateSurface(m_Instance, &Descriptor);
	}
#endif
	return nullptr;
}

bool CCommandProcessorFragment_WebGpu::QuerySurfaceConfiguration()
{
	const WGPUTextureFormat PreviousFormat = m_SurfaceFormat;
	WGPUSurfaceCapabilities Capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
	if(wgpuSurfaceGetCapabilities(m_Surface, m_Adapter, &Capabilities) != WGPUStatus_Success)
	{
		m_ErrorMessage = "WebGPU could not query the presentation surface";
		return false;
	}
	// Emdawnwebgpu from Emscripten 4.0.22 does not report canvas usages
	// yet; a surface that says nothing is taken to allow the copy.
	const WGPUTextureUsage SurfaceUsages = Capabilities.usages == WGPUTextureUsage_None ? WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst : Capabilities.usages;
	// The frame is drawn into an image of ours and copied into the
	// surface texture, so the surface has to take a copy. Reading the
	// presented picture back needs nothing from it.
	const bool CanCopyInto = (SurfaceUsages & WGPUTextureUsage_CopyDst) != 0;
	const bool Complete = Capabilities.formatCount > 0 && Capabilities.alphaModeCount > 0 && Capabilities.presentModeCount > 0 && CanCopyInto;
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
	if(!CanCopyInto)
		m_ErrorMessage = "The selected WebGPU implementation cannot copy into its surface texture";
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

bool CCommandProcessorFragment_WebGpu::SupportsPresentMode(WGPUPresentMode Mode) const
{
	return (Mode == WGPUPresentMode_Fifo && m_SupportsFifo) || (Mode == WGPUPresentMode_Immediate && m_SupportsImmediate) || (Mode == WGPUPresentMode_Mailbox && m_SupportsMailbox);
}

void CCommandProcessorFragment_WebGpu::ReleaseFrame()
{
	if(m_SurfaceTexture.texture != nullptr)
		wgpuTextureRelease(m_SurfaceTexture.texture);
	m_SurfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
}

bool CCommandProcessorFragment_WebGpu::EnsureScreenTexture()
{
	if(m_ScreenTexture != nullptr)
		return true;
	if(m_SurfaceWidth == 0 || m_SurfaceHeight == 0 || m_SurfaceFormat == WGPUTextureFormat_Undefined)
		return false;
	WGPUTextureDescriptor Descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	Descriptor.label = StringView("DDNet WebGPU screen image");
	// The same format as the surface, so the screen pass keeps the
	// pipelines built for it and the copy needs no conversion.
	Descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
	Descriptor.dimension = WGPUTextureDimension_2D;
	Descriptor.size = {m_SurfaceWidth, m_SurfaceHeight, 1};
	Descriptor.format = m_SurfaceFormat;
	Descriptor.mipLevelCount = 1;
	Descriptor.sampleCount = 1;
	m_ScreenTexture = wgpuDeviceCreateTexture(m_Device, &Descriptor);
	m_ScreenView = m_ScreenTexture == nullptr ? nullptr : wgpuTextureCreateView(m_ScreenTexture, nullptr);
	if(m_ScreenView == nullptr)
	{
		ReleaseScreenTexture();
		SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU failed to create the screen image");
		return false;
	}
	m_ScreenMemorySize = static_cast<size_t>(m_SurfaceWidth) * m_SurfaceHeight * 4;
	if(m_pTextureMemoryUsage != nullptr)
		m_pTextureMemoryUsage->fetch_add(m_ScreenMemorySize, std::memory_order_relaxed);
	return true;
}

void CCommandProcessorFragment_WebGpu::ReleaseScreenTexture()
{
	if(m_ScreenMemorySize != 0 && m_pTextureMemoryUsage != nullptr)
		m_pTextureMemoryUsage->fetch_sub(m_ScreenMemorySize, std::memory_order_relaxed);
	m_ScreenMemorySize = 0;
	if(m_ScreenView != nullptr)
		wgpuTextureViewRelease(m_ScreenView);
	if(m_ScreenTexture != nullptr)
		wgpuTextureRelease(m_ScreenTexture);
	m_ScreenView = nullptr;
	m_ScreenTexture = nullptr;
	m_ScreenTouched = false;
}

bool CCommandProcessorFragment_WebGpu::CopyScreenToSurface()
{
	if(m_ScreenTexture == nullptr || m_SurfaceTexture.texture == nullptr || !EnsureCommandEncoder())
		return false;
	WGPUTexelCopyTextureInfo Source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	Source.texture = m_ScreenTexture;
	Source.aspect = WGPUTextureAspect_All;
	WGPUTexelCopyTextureInfo Destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	Destination.texture = m_SurfaceTexture.texture;
	Destination.aspect = WGPUTextureAspect_All;
	// A resize between the two is handled by dropping the image, but a
	// copy that runs off the end of either one is a validation error and
	// would take the device with it.
	const WGPUExtent3D Extent{
		std::min(wgpuTextureGetWidth(m_ScreenTexture), wgpuTextureGetWidth(m_SurfaceTexture.texture)),
		std::min(wgpuTextureGetHeight(m_ScreenTexture), wgpuTextureGetHeight(m_SurfaceTexture.texture)),
		1};
	wgpuCommandEncoderCopyTextureToTexture(m_CommandEncoder, &Source, &Destination, &Extent);
	return SubmitCommands();
}

WGPUTextureView CCommandProcessorFragment_WebGpu::PresentationView() const
{
	return m_ScreenView;
}

void CCommandProcessorFragment_WebGpu::DiscardFrame()
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
	// Made again in the size and format the surface has then.
	ReleaseScreenTexture();
	m_SurfaceSuboptimal = false;
}

bool CCommandProcessorFragment_WebGpu::RecreateSurface()
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

bool CCommandProcessorFragment_WebGpu::ConfigureIfNeeded()
{
	// A surface-less renderer has nothing to configure - everything it draws
	// goes into a render target. A viewport update or a screenshot reaches
	// this from the offscreen mode too.
	if(m_Surface == nullptr)
		return true;
	if(m_SurfaceTexture.texture != nullptr)
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
	// The frame is drawn into an image of our own and copied here.
	Config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
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

bool CCommandProcessorFragment_WebGpu::AcquireFrame()
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
			return true;
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

bool CCommandProcessorFragment_WebGpu::Present()
{
	// Whether there is a picture to show does not depend on holding a
	// surface texture here: the frame was drawn into an image of ours.
	const bool DrewToScreen = m_ScreenTouched;
	m_ScreenTouched = false;
	if(!DrewToScreen)
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
		return false;
	// The frame is complete and in the queue and nothing below waits for
	// the device, so this is the one point where the surface texture is
	// held. A surface that is not there to be had - a resize, a tab or
	// a window that went away - costs this one picture and no more.
	m_SkipPresentationFrame = false;
	WGPUStatus Status = WGPUStatus_Success;
	if(AcquireFrame())
	{
		if(!CopyScreenToSurface())
			Status = WGPUStatus_Error;
#if !defined(CONF_PLATFORM_EMSCRIPTEN)
		// The browser composites the canvas by itself.
		else
			Status = wgpuSurfacePresent(m_Surface);
#endif
	}
	else if(m_Error.m_ErrorType != GFX_ERROR_TYPE_NONE)
		return false;
	ReleaseFrame();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// The canvas is composited once per refresh whatever this does, so
	// waiting for a frame is the right default: it costs nothing. Someone
	// who asked for a rate of their own gets the short yield instead and
	// lets the client's own limiter do the pacing -- for a benchmark, or
	// for the shortest path from an input to the frame that carries it.
	YieldToBrowser(g_Config.m_GfxRefreshRate == 0 ? 1 : 0);
#endif
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

// ---------------------------------------------------------------------------
// Buffers: creation, the upload ring and streamed vertex data.
// ---------------------------------------------------------------------------

bool CCommandProcessorFragment_WebGpu::AdvanceUploadBufferSlot()
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
	{
		// Without this the render thread would carry on against a queue that
		// never answers and turn a graphics error into a hang or a crash.
		SetError(GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "WebGPU did not finish the submitted work");
		return false;
	}
	NextResult.m_Pending = false;
	return true;
}

void CCommandProcessorFragment_WebGpu::ReleaseBuffer(SBuffer &Buffer)
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

bool CCommandProcessorFragment_WebGpu::WriteBufferData(WGPUBuffer Buffer, uint64_t Offset, const void *pData, size_t Size, bool AllowEndPadding)
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

bool CCommandProcessorFragment_WebGpu::CreateNativeBuffer(SBuffer &Buffer, const IGraphics::CBufferDesc &Desc, const void *pData)
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

bool CCommandProcessorFragment_WebGpu::CreateBuffer(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
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

bool CCommandProcessorFragment_WebGpu::RecreateBuffer(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
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

bool CCommandProcessorFragment_WebGpu::DestroyBuffer(IGraphics::CBufferHandle Handle)
{
	if(!m_BufferHandles.IsActive(Handle) || static_cast<size_t>(Handle.Id()) >= m_vBuffers.size())
		return true;
	if(!SubmitCommands())
		return false;
	ReleaseBuffer(m_vBuffers[Handle.Id()]);
	m_BufferHandles.Release(Handle);
	return true;
}

bool CCommandProcessorFragment_WebGpu::EnsureUploadSpace(uint64_t StreamBytes, uint64_t UniformBytes)
{
	const uint64_t StreamNeeded = AlignUp(m_StreamOffset, 16) + AlignUp(StreamBytes, 4);
	const uint64_t UniformNeeded = AlignUp(m_UniformOffset, m_UniformAlignment) + UniformBytes;
	if(StreamNeeded <= STREAM_BUFFER_SIZE && UniformNeeded <= UNIFORM_BUFFER_SIZE)
		return true;
	if(StreamBytes > STREAM_BUFFER_SIZE || UniformBytes > UNIFORM_BUFFER_SIZE)
	{
		DropCommand("a single draw that does not fit in a whole upload buffer");
		return false;
	}
	return SubmitCommands();
}

bool CCommandProcessorFragment_WebGpu::WriteStream(const void *pData, size_t Size, uint64_t Alignment, uint64_t &Offset)
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

// ---------------------------------------------------------------------------
// Textures: views, samplers and bind groups.
// ---------------------------------------------------------------------------

size_t CCommandProcessorFragment_WebGpu::SamplerIndex(EWrapMode WrapMode)
{
	return WrapMode == EWrapMode::REPEAT ? 0 : 1;
}

void CCommandProcessorFragment_WebGpu::ReleaseTexture(STexture &Texture)
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

bool CCommandProcessorFragment_WebGpu::CreateTextureBindGroups(WGPUTextureView View, WGPUBindGroupLayout Layout, uint32_t TextureBinding, std::array<WGPUBindGroup, 2> &aBindGroups, bool TextureArray)
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

bool CCommandProcessorFragment_WebGpu::CreateTexture(const CCommandBuffer::SCommand_Texture_Create *pCommand)
{
	const auto &Desc = pCommand->m_Desc;
	const bool CreateArray = Desc.m_Layering == IGraphics::ETextureLayering::LAYERED;
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
	const size_t PixelSize = IGraphics::PixelSize(Desc.m_Format);
	const WGPUTextureFormat Format = ToWGPUFormat(Desc.m_Format);
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
			// A texture created without data - a colour target - has nothing to
			// downsample from, and its mips are written by rendering anyway.
			if(Mip + 1 < MipCount && pData != nullptr)
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
		int LayerWidth, LayerHeight;
		std::unique_ptr<uint8_t, decltype(&free)> pConvertedData = PrepareLayeredImage(pCommand->m_pData, Desc.m_Width, Desc.m_Height, PixelSize, Desc.m_LayerColumns, Desc.m_LayerRows, LayerWidth, LayerHeight);
		if(pConvertedData == nullptr)
			Created = false;
		else
			Created = CreateNativeTexture(Texture.m_ArrayTexture, Texture.m_ArrayView, Texture.m_aArrayBindGroups, LayerWidth, LayerHeight, static_cast<uint32_t>(Desc.LayerCount()), m_ArrayTextureBindGroupLayout, 2, pConvertedData.get());
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

bool CCommandProcessorFragment_WebGpu::UpdateTexture(const CCommandBuffer::SCommand_Texture_Update *pCommand)
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
	const size_t PixelSize = IGraphics::PixelSize(Texture.m_Format);
	WGPUTexelCopyBufferLayout Layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	Layout.bytesPerRow = Region.m_Width * PixelSize;
	Layout.rowsPerImage = Region.m_Height;
	const WGPUExtent3D Extent{static_cast<uint32_t>(Region.m_Width), static_cast<uint32_t>(Region.m_Height), 1};
	wgpuQueueWriteTexture(m_Queue, &Destination, pCommand->m_pData, Region.m_Width * Region.m_Height * PixelSize, &Layout, &Extent);
	return true;
}

void CCommandProcessorFragment_WebGpu::DestroyTexture(IGraphics::CTextureHandle Handle)
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

// ---------------------------------------------------------------------------
// Pipelines: layouts, the pipeline sets per blend mode and the draw
// resources.
// ---------------------------------------------------------------------------

size_t CCommandProcessorFragment_WebGpu::BlendIndex(EBlendMode BlendMode)
{
	switch(BlendMode)
	{
	case EBlendMode::NONE: return 0;
	case EBlendMode::ALPHA: return 1;
	case EBlendMode::ADDITIVE: return 2;
	}
	return 0;
}

size_t CCommandProcessorFragment_WebGpu::PrimitivePipelineIndex(EPrimitiveType PrimitiveType, EBlendMode BlendMode, bool Textured)
{
	const size_t Topology = PrimitiveType == EPrimitiveType::LINES ? 0 : 1;
	return (Topology * BLEND_MODE_COUNT + BlendIndex(BlendMode)) * 2 + Textured;
}

WGPUBlendState CCommandProcessorFragment_WebGpu::BlendState(size_t Blend)
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

void CCommandProcessorFragment_WebGpu::RememberPipelineLayout(WGPURenderPipeline Pipeline, WGPUPipelineLayout Layout)
{
	if(Pipeline == nullptr)
		return;
	SBindGroupLayouts aLayouts = {};
	if(Layout == m_UntexturedPipelineLayout)
		aLayouts = {m_UniformBindGroupLayout, nullptr, nullptr};
	else if(Layout == m_PrimitivePipelineLayout)
		aLayouts = {m_UniformBindGroupLayout, m_TextureBindGroupLayout, nullptr};
	else if(Layout == m_ArrayTexturePipelineLayout)
		aLayouts = {m_UniformBindGroupLayout, m_ArrayTextureBindGroupLayout, nullptr};
	else if(Layout == m_QuadTexturedPipelineLayout)
		aLayouts = {m_UniformBindGroupLayout, m_TextureBindGroupLayout, m_QuadBindGroupLayout};
	else if(Layout == m_QuadUntexturedPipelineLayout)
		aLayouts = {m_UniformBindGroupLayout, m_EmptyBindGroupLayout, m_QuadBindGroupLayout};
	m_PipelineBindGroupLayouts[Pipeline] = aLayouts;
}

WGPUVertexFormat CCommandProcessorFragment_WebGpu::VertexAttributeFormat(const IGraphics::CVertexAttributeDesc &Attribute)
{
	switch(Attribute.m_Type)
	{
	case IGraphics::EVertexAttributeType::FLOAT32:
		switch(Attribute.m_ComponentCount)
		{
		case 1: return WGPUVertexFormat_Float32;
		case 2: return WGPUVertexFormat_Float32x2;
		case 3: return WGPUVertexFormat_Float32x3;
		case 4: return WGPUVertexFormat_Float32x4;
		default: break;
		}
		break;
	case IGraphics::EVertexAttributeType::UINT8:
		if(Attribute.m_ComponentCount == 4)
			return Attribute.m_Mode == IGraphics::EVertexAttributeMode::INTEGER ? WGPUVertexFormat_Uint8x4 : WGPUVertexFormat_Unorm8x4;
		break;
	default:
		break;
	}
	dbg_assert(false, "Vertex attribute has no WebGPU format");
	return WGPUVertexFormat_Float32x2;
}

bool CCommandProcessorFragment_WebGpu::CreateBufferedPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, const char *pVertexEntry, bool Instanced, uint32_t SampleCount)
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
			RememberPipelineLayout(aPipelines[Index], Descriptor.layout);
			if(aPipelines[Index] == nullptr)
				return false;
		}
	}
	return true;
}

bool CCommandProcessorFragment_WebGpu::CreateLayeredPrimitivePipelines(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount)
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
				RememberPipelineLayout(Pipelines.m_aLayeredPrimitive[Index], Descriptor.layout);
				if(Pipelines.m_aLayeredPrimitive[Index] == nullptr)
					return false;
			}
		}
	}
	return true;
}

bool CCommandProcessorFragment_WebGpu::CreateArrayColorPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, bool Transform, uint32_t SampleCount)
{
	for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
	{
		for(size_t Textured = 0; Textured < 2; ++Textured)
		{
			std::array<WGPUVertexAttribute, 2> aAttributes{};
			uint32_t AttributeCount = 0;
			const uint64_t Stride = FillVertexInput(Textured != 0 ? IGraphics::EVertexLayout::TILE_TEXTURED : IGraphics::EVertexLayout::TILE, aAttributes, AttributeCount);
			WGPUVertexBufferLayout VertexBuffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			VertexBuffer.arrayStride = Stride;
			VertexBuffer.stepMode = WGPUVertexStepMode_Vertex;
			VertexBuffer.attributeCount = AttributeCount;
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
			RememberPipelineLayout(aPipelines[Index], Descriptor.layout);
			if(aPipelines[Index] == nullptr)
				return false;
		}
	}
	return true;
}

bool CCommandProcessorFragment_WebGpu::CreateQuadPipelines(std::array<WGPURenderPipeline, BUFFERED_PIPELINE_COUNT> &aPipelines, WGPUTextureFormat Format, bool Shared, uint32_t SampleCount)
{
	for(size_t Blend = 0; Blend < BLEND_MODE_COUNT; ++Blend)
	{
		for(size_t Textured = 0; Textured < 2; ++Textured)
		{
			std::array<WGPUVertexAttribute, 3> aAttributes{};
			uint32_t AttributeCount = 0;
			const uint64_t Stride = FillVertexInput(Textured != 0 ? IGraphics::EVertexLayout::QUAD_TEXTURED : IGraphics::EVertexLayout::QUAD, aAttributes, AttributeCount);
			WGPUVertexBufferLayout VertexBuffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			VertexBuffer.arrayStride = Stride;
			VertexBuffer.stepMode = WGPUVertexStepMode_Vertex;
			VertexBuffer.attributeCount = AttributeCount;
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
			RememberPipelineLayout(aPipelines[Index], Descriptor.layout);
			if(aPipelines[Index] == nullptr)
				return false;
		}
	}
	return true;
}

bool CCommandProcessorFragment_WebGpu::CreateDualAtlasPipelines(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount)
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
		Descriptor.layout = m_PrimitivePipelineLayout;
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
		RememberPipelineLayout(Pipelines.m_aDualAtlas[Blend], Descriptor.layout);
		if(Pipelines.m_aDualAtlas[Blend] == nullptr)
			return false;
	}
	return true;
}

bool CCommandProcessorFragment_WebGpu::CreatePipelineSet(SPipelineSet &Pipelines, WGPUTextureFormat Format, uint32_t SampleCount)
{
	// Whatever is released below may come back at the same address.
	m_PipelineBindGroupLayouts.clear();
	if(Pipelines.m_PlanarYuv != nullptr)
	{
		wgpuRenderPipelineRelease(Pipelines.m_PlanarYuv);
		Pipelines.m_PlanarYuv = nullptr;
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
				RememberPipelineLayout(Pipelines.m_aPrimitive[Index], Descriptor.layout);
				if(Pipelines.m_aPrimitive[Index] == nullptr)
					return false;
			}
		}
	}
	WGPUColorTargetState PlanarYuvTarget = WGPU_COLOR_TARGET_STATE_INIT;
	PlanarYuvTarget.format = Format;
	PlanarYuvTarget.writeMask = WGPUColorWriteMask_All;
	WGPUFragmentState PlanarYuvFragment = WGPU_FRAGMENT_STATE_INIT;
	PlanarYuvFragment.module = m_PrimitiveShader;
	PlanarYuvFragment.entryPoint = StringView("fs_planar_yuv");
	PlanarYuvFragment.targetCount = 1;
	PlanarYuvFragment.targets = &PlanarYuvTarget;
	WGPURenderPipelineDescriptor PlanarYuvDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	PlanarYuvDescriptor.label = StringView("DDNet WebGPU planar YUV pipeline");
	PlanarYuvDescriptor.layout = m_PrimitivePipelineLayout;
	PlanarYuvDescriptor.vertex.module = m_PrimitiveShader;
	PlanarYuvDescriptor.vertex.entryPoint = StringView("vs_main");
	PlanarYuvDescriptor.vertex.bufferCount = 1;
	PlanarYuvDescriptor.vertex.buffers = &VertexBufferLayout;
	PlanarYuvDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	PlanarYuvDescriptor.primitive.frontFace = WGPUFrontFace_CCW;
	PlanarYuvDescriptor.primitive.cullMode = WGPUCullMode_None;
	PlanarYuvDescriptor.multisample.count = SampleCount;
	PlanarYuvDescriptor.multisample.mask = UINT32_MAX;
	PlanarYuvDescriptor.fragment = &PlanarYuvFragment;
	Pipelines.m_PlanarYuv = wgpuDeviceCreateRenderPipeline(m_Device, &PlanarYuvDescriptor);
	RememberPipelineLayout(Pipelines.m_PlanarYuv, PlanarYuvDescriptor.layout);
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

bool CCommandProcessorFragment_WebGpu::CreatePrimitivePipelines()
{
	if(!m_Presentation.IsPresentable())
		return CreatePipelineSet(m_aPipelineSets[1], WGPUTextureFormat_RGBA8Unorm, 1);
	return CreatePipelineSet(m_aPipelineSets[0], m_SurfaceFormat, SampleCount()) && CreatePipelineSet(m_aPipelineSets[1], WGPUTextureFormat_RGBA8Unorm, 1);
}

bool CCommandProcessorFragment_WebGpu::CreateDrawResources()
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
@fragment fn fs_untextured(input: VertexOutput) -> @location(0) vec4f {
	return input.color;
}
@fragment fn fs_textured(input: VertexOutput) -> @location(0) vec4f {
	let sample = textureSample(image_texture, image_sampler, input.uv);
	let texture_color = select(sample, vec4f(1.0, 1.0, 1.0, sample.r), transform.alpha_texture != 0u);
	return texture_color * input.color;
}
// Turns a rendered frame into the planar YUV layout an encoder wants. See
// shader/vulkan/planar_yuv.frag for what the layout is; input.color.r picks
// between interleaved NV12 and three separate planes.
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
	// One atlas, two channels: red is the glyph body, green is its outline.
	let atlas = textureSample(image_texture, image_sampler, input.uv).rg;
	let primary = transform.color * input.color * vec4f(1.0, 1.0, 1.0, atlas.r);
	let secondary = transform.secondary_color * vec4f(1.0, 1.0, 1.0, atlas.g);
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
	if(m_UniformBindGroupLayout == nullptr || m_EmptyBindGroupLayout == nullptr || m_TextureBindGroupLayout == nullptr || m_ArrayTextureBindGroupLayout == nullptr || m_QuadBindGroupLayout == nullptr)
		return false;

	const std::array<WGPUBindGroupLayout, 2> aLayouts = {m_UniformBindGroupLayout, m_TextureBindGroupLayout};
	const std::array<WGPUBindGroupLayout, 2> aArrayLayouts = {m_UniformBindGroupLayout, m_ArrayTextureBindGroupLayout};
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
	PipelineLayoutDescriptor.label = StringView("DDNet WebGPU textured quad layout");
	PipelineLayoutDescriptor.bindGroupLayoutCount = aQuadTexturedLayouts.size();
	PipelineLayoutDescriptor.bindGroupLayouts = aQuadTexturedLayouts.data();
	m_QuadTexturedPipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
	PipelineLayoutDescriptor.label = StringView("DDNet WebGPU untextured quad layout");
	PipelineLayoutDescriptor.bindGroupLayouts = aQuadUntexturedLayouts.data();
	m_QuadUntexturedPipelineLayout = wgpuDeviceCreatePipelineLayout(m_Device, &PipelineLayoutDescriptor);
	if(m_UntexturedPipelineLayout == nullptr || m_PrimitivePipelineLayout == nullptr || m_ArrayTexturePipelineLayout == nullptr || m_QuadTexturedPipelineLayout == nullptr || m_QuadUntexturedPipelineLayout == nullptr)
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

void CCommandProcessorFragment_WebGpu::DestroyDrawResources()
{
	// Whatever is released below may come back at the same address.
	m_PipelineBindGroupLayouts.clear();
	for(auto &Buffer : m_vBuffers)
		ReleaseBuffer(Buffer);
	m_vBuffers.clear();
	m_BufferHandles.Clear();
	for(auto &Texture : m_vTextures)
		ReleaseTexture(Texture);
	m_vTextures.clear();
	m_TextureHandles.Clear();
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
	m_QuadTexturedPipelineLayout = nullptr;
	m_QuadUntexturedPipelineLayout = nullptr;
	m_UntexturedPipelineLayout = nullptr;
	m_TextureBindGroupLayout = nullptr;
	m_ArrayTextureBindGroupLayout = nullptr;
	m_QuadBindGroupLayout = nullptr;
	m_EmptyBindGroupLayout = nullptr;
	m_UniformBindGroupLayout = nullptr;
	m_PrimitiveShader = nullptr;
	m_StreamOffset = 0;
	m_UniformOffset = 0;
	m_UploadBufferSlot = 0;
	m_aUploadBufferResults = {};
}

// ---------------------------------------------------------------------------
// Drawing: render passes, cached state and the draw commands.
// ---------------------------------------------------------------------------

CCommandProcessorFragment_WebGpu::STexture *CCommandProcessorFragment_WebGpu::RenderTarget()
{
	if(!m_RenderTarget.IsValid() || !m_TextureHandles.IsActive(m_RenderTarget) || static_cast<size_t>(m_RenderTarget.Id()) >= m_vTextures.size())
		return nullptr;
	return &m_vTextures[m_RenderTarget.Id()];
}

bool CCommandProcessorFragment_WebGpu::EnsureRenderPass()
{
	if(m_RenderPass != nullptr)
		return true;
	STexture *pTarget = RenderTarget();
	if(!m_Presentation.IsPresentable() && pTarget == nullptr)
	{
		SetError(GFX_ERROR_TYPE_RENDER_RECORDING, "WebGPU offscreen draws require a render target");
		return false;
	}
	if(m_RenderTarget.IsValid() && pTarget == nullptr)
		return false;
	if(pTarget == nullptr)
	{
		// The first screen pass of a frame clears, whatever load
		// operation the last render target left behind. Natively that
		// is also all a fresh surface texture could give us; the image
		// drawn into on the web still holds the frame before.
		const bool FreshFrame = !m_ScreenTouched;
		if(m_Minimized || !EnsureScreenTexture())
			return false;
		m_ScreenTouched = true;
		if(FreshFrame)
			m_RenderPassLoadOp = WGPULoadOp_Clear;
	}
	if(!EnsureCommandEncoder())
		return false;
	WGPUTextureView ResolveView = pTarget != nullptr ? pTarget->m_View : PresentationView();
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
	m_PassState = SPassState{};
	m_RenderPassLoadOp = WGPULoadOp_Load;
	return m_RenderPass != nullptr;
}

void CCommandProcessorFragment_WebGpu::SetPipelineCached(WGPURenderPipeline Pipeline)
{
	if(m_PassState.m_Pipeline == Pipeline)
		return;
	wgpuRenderPassEncoderSetPipeline(m_RenderPass, Pipeline);
	m_PassState.m_Pipeline = Pipeline;
	// A bind group set before the switch is still set, and it is what
	// the new pipeline wants wherever the layout of its slot is the
	// same. Only a slot whose layout changed has to be set again; a
	// pipeline this does not know is treated as changing every slot.
	const auto Found = m_PipelineBindGroupLayouts.find(Pipeline);
	const SBindGroupLayouts aLayouts = Found != m_PipelineBindGroupLayouts.end() ? Found->second : SBindGroupLayouts{};
	for(size_t Slot = 0; Slot < aLayouts.size(); ++Slot)
	{
		if(aLayouts[Slot] == nullptr || aLayouts[Slot] != m_PassState.m_aBindGroupLayouts[Slot])
		{
			m_PassState.m_aBindGroups[Slot] = nullptr;
			m_PassState.m_aBindGroupOffsets[Slot] = 0;
		}
	}
	m_PassState.m_aBindGroupLayouts = aLayouts;
}

void CCommandProcessorFragment_WebGpu::SetBindGroupCached(uint32_t Slot, WGPUBindGroup BindGroup, const uint32_t *pDynamicOffset)
{
	const uint32_t Offset = pDynamicOffset != nullptr ? *pDynamicOffset : 0;
	if(m_PassState.m_aBindGroups[Slot] == BindGroup && m_PassState.m_aBindGroupOffsets[Slot] == Offset)
		return;
	wgpuRenderPassEncoderSetBindGroup(m_RenderPass, Slot, BindGroup, pDynamicOffset != nullptr ? 1 : 0, pDynamicOffset);
	m_PassState.m_aBindGroups[Slot] = BindGroup;
	m_PassState.m_aBindGroupOffsets[Slot] = Offset;
}

void CCommandProcessorFragment_WebGpu::SetViewportCached(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height)
{
	const std::array<uint32_t, 4> Viewport = {X, Y, Width, Height};
	if(m_PassState.m_HasViewport && m_PassState.m_aViewport == Viewport)
		return;
	wgpuRenderPassEncoderSetViewport(m_RenderPass, X, Y, Width, Height, 0.0f, 1.0f);
	m_PassState.m_aViewport = Viewport;
	m_PassState.m_HasViewport = true;
}

void CCommandProcessorFragment_WebGpu::SetScissorCached(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height)
{
	const std::array<uint32_t, 4> Scissor = {X, Y, Width, Height};
	if(m_PassState.m_HasScissor && m_PassState.m_aScissor == Scissor)
		return;
	wgpuRenderPassEncoderSetScissorRect(m_RenderPass, X, Y, Width, Height);
	m_PassState.m_aScissor = Scissor;
	m_PassState.m_HasScissor = true;
}

bool CCommandProcessorFragment_WebGpu::ApplyState(const CCommandBuffer::SState &State, WGPURenderPipeline Pipeline, const SDrawUniforms &Uniforms, bool TextureArray)
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
	SPrimitiveTransform Transform{};
	// WebGPU's clip space has y pointing up.
	if(!ScreenToClip(State.m_ScreenTL, State.m_ScreenBR, true, Transform.m_Scale, Transform.m_Translate))
		return false;
	Transform.m_Color = Uniforms.m_Color;
	Transform.m_RotationCenter = Uniforms.m_RotationCenter;
	Transform.m_Rotation = Uniforms.m_Rotation;
	Transform.m_AlphaTexture = pTexture != nullptr && pTexture->m_Format == IGraphics::ETextureFormat::R8_UNORM;
	Transform.m_VertexOffset = Uniforms.m_VertexOffset;
	Transform.m_VertexScale = Uniforms.m_VertexScale;
	Transform.m_QuadBase = Uniforms.m_QuadBase;
	Transform.m_TextureSize = Uniforms.m_TextureSize;
	Transform.m_SecondaryColor = Uniforms.m_SecondaryColor;
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
	SetPipelineCached(Pipeline);
	SetBindGroupCached(0, m_UniformBindGroup, &UniformOffset);
	if(pTexture != nullptr)
	{
		const auto &aBindGroups = TextureArray ? pTexture->m_aArrayBindGroups : pTexture->m_aBindGroups;
		SetBindGroupCached(1, aBindGroups[SamplerIndex(State.m_WrapMode)]);
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
	SetViewportCached(ViewportX, ViewportY, ViewportWidth, ViewportHeight);
	uint32_t ScissorX = ViewportX;
	uint32_t ScissorY = ViewportY;
	uint32_t ScissorW = ViewportWidth;
	uint32_t ScissorH = ViewportHeight;
	if(State.m_ClipEnable)
	{
		// The clip is expressed in whatever ScreenWidth()/ScreenHeight() reported when
		// it was set: the render target's size while an offscreen frame is open, the
		// presentation viewport otherwise.
		const uint32_t ClipBaseWidth = pTarget != nullptr ? std::max(TargetWidth, 1u) : std::max(m_ViewportWidth, 1u);
		const uint32_t ClipBaseHeight = pTarget != nullptr ? std::max(TargetHeight, 1u) : std::max(m_ViewportHeight, 1u);
		const int64_t Left = std::clamp<int64_t>(static_cast<int64_t>(State.m_ClipX) * ViewportWidth / ClipBaseWidth, 0, ViewportWidth);
		const int64_t Right = std::clamp<int64_t>((static_cast<int64_t>(State.m_ClipX) + State.m_ClipW) * ViewportWidth / ClipBaseWidth, 0, ViewportWidth);
		const int64_t Bottom = std::clamp<int64_t>(static_cast<int64_t>(State.m_ClipY) * ViewportHeight / ClipBaseHeight, 0, ViewportHeight);
		const int64_t Top = std::clamp<int64_t>((static_cast<int64_t>(State.m_ClipY) + State.m_ClipH) * ViewportHeight / ClipBaseHeight, 0, ViewportHeight);
		ScissorX += static_cast<uint32_t>(Left);
		ScissorY += ViewportHeight - static_cast<uint32_t>(Top);
		ScissorW = static_cast<uint32_t>(std::max<int64_t>(Right - Left, 0));
		ScissorH = static_cast<uint32_t>(std::max<int64_t>(Top - Bottom, 0));
	}
	SetScissorCached(ScissorX, ScissorY, ScissorW, ScissorH);
	return ScissorW != 0 && ScissorH != 0;
}

bool CCommandProcessorFragment_WebGpu::WriteQuadTransforms(const CCommandBuffer::SDrawDataQuadTransform *pData, uint32_t Count, uint32_t &Offset)
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

bool CCommandProcessorFragment_WebGpu::Draw(const CCommandBuffer::SCommand_Draw *pCommand)
{
	EPipelineProgram Program;
	if(Program = pCommand->m_Program; (Program != EPipelineProgram::PRIMITIVE && Program != EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY && Program != EPipelineProgram::PLANAR_YUV))
	{
		DropCommand("a transient draw on a pipeline that only indexed draws reach");
		return true;
	}
	const bool Layered = Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY;
	const bool PlanarYuv = Program == EPipelineProgram::PLANAR_YUV;
	const auto *pVertices = Layered ? nullptr : pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
	const auto *pLayeredVertices = Layered ? pCommand->m_VertexData.Get<CCommandBuffer::SVertexTex3DStream>(pCommand->m_VertexCount) : nullptr;
	if((Layered ? pLayeredVertices == nullptr : pVertices == nullptr) || pCommand->m_VertexCount == 0)
		return true;
	const void *pUploadVertices = Layered ? static_cast<const void *>(pLayeredVertices) : pVertices;
	size_t VertexSize = Layered ? sizeof(*pLayeredVertices) : sizeof(*pVertices);
	uint32_t VertexCount = pCommand->m_VertexCount;
	EPrimitiveType PrimitiveType = pCommand->m_PrimitiveType;
	// A quad is four vertices and six indices, and the frontend has an
	// index buffer that says so once for every quad it will ever draw.
	// Taking it means the vertices go up as they are, instead of one
	// and a half times as many rebuilt here every frame.
	uint32_t QuadIndexCount = 0;
	const SBuffer *pQuadIndexBuffer = nullptr;
	if(PrimitiveType == EPrimitiveType::QUADS)
	{
		if(VertexCount % 4 != 0 || !pCommand->m_IndexBuffer.IsValid() ||
			!m_BufferHandles.IsActive(pCommand->m_IndexBuffer) ||
			static_cast<size_t>(pCommand->m_IndexBuffer.Id()) >= m_vBuffers.size())
		{
			DropCommand("a quad draw without the index buffer that turns quads into triangles");
			return true;
		}
		pQuadIndexBuffer = &m_vBuffers[pCommand->m_IndexBuffer.Id()];
		QuadIndexCount = VertexCount / 4 * 6;
		if(pQuadIndexBuffer->m_Buffer == nullptr || pQuadIndexBuffer->m_Size < QuadIndexCount * sizeof(uint32_t))
		{
			DropCommand("a quad draw whose index buffer is too small for it");
			return true;
		}
		PrimitiveType = EPrimitiveType::TRIANGLES;
	}
	else
	{
		const uint32_t VerticesPerPrim = VerticesPerPrimitive(PrimitiveType);
		if(VerticesPerPrim == 0 || VertexCount % VerticesPerPrim != 0)
			return true;
	}
	uint64_t VertexOffset = 0;
	if(!EnsureUploadSpace(static_cast<uint64_t>(VertexCount) * VertexSize, sizeof(SPrimitiveTransform)))
		return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
	if(!EnsureRenderPass())
		return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
	if(!WriteStream(pUploadVertices, VertexCount * VertexSize, 4, VertexOffset))
		return false;
	const bool Textured = pCommand->m_State.m_Texture.IsValid();
	const auto &Pipelines = m_aPipelineSets[m_RenderTarget.IsValid() ? 1 : 0];
	const auto &aPipelines = Layered ? Pipelines.m_aLayeredPrimitive : Pipelines.m_aPrimitive;
	const WGPURenderPipeline Pipeline = PlanarYuv ? Pipelines.m_PlanarYuv : aPipelines[PrimitivePipelineIndex(PrimitiveType, pCommand->m_State.m_BlendMode, Textured)];
	if(!ApplyState(pCommand->m_State, Pipeline, {}, Layered))
		return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
	wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, m_StreamBuffer, VertexOffset, VertexCount * VertexSize);
	if(pQuadIndexBuffer != nullptr)
	{
		wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, pQuadIndexBuffer->m_Buffer, WGPUIndexFormat_Uint32, 0, QuadIndexCount * sizeof(uint32_t));
		wgpuRenderPassEncoderDrawIndexed(m_RenderPass, QuadIndexCount, 1, 0, 0, 0);
	}
	else
	{
		wgpuRenderPassEncoderDraw(m_RenderPass, VertexCount, 1, 0, 0);
	}
	return true;
}

bool CCommandProcessorFragment_WebGpu::DrawBuffered(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
	if(pCommand->m_Program >= EPipelineProgram::COUNT || !m_BufferHandles.IsActive(pCommand->m_VertexBuffer) || !m_BufferHandles.IsActive(pCommand->m_IndexBuffer) || static_cast<size_t>(pCommand->m_IndexBuffer.Id()) >= m_vBuffers.size())
		return true;
	const auto VertexHandle = pCommand->m_VertexBuffer;
	if(!m_BufferHandles.IsActive(VertexHandle) || static_cast<size_t>(VertexHandle.Id()) >= m_vBuffers.size())
		return true;
	const auto &VertexBuffer = m_vBuffers[VertexHandle.Id()];
	const auto &IndexBuffer = m_vBuffers[pCommand->m_IndexBuffer.Id()];
	if(VertexBuffer.m_Usage != IGraphics::EBufferUsage::VERTEX || IndexBuffer.m_Usage != IGraphics::EBufferUsage::INDEX || VertexBuffer.m_Size == 0 || pCommand->m_IndexCount == 0)
		return true;
	const size_t IndexSize = pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? sizeof(uint16_t) : sizeof(uint32_t);
	if(pCommand->m_IndexOffset % IndexSize != 0 || pCommand->m_IndexOffset > IndexBuffer.m_Size || static_cast<uint64_t>(pCommand->m_IndexCount) * IndexSize > IndexBuffer.m_Size - pCommand->m_IndexOffset)
		return true;

	// CGraphics_Threaded::CheckIndexedDraw has already rejected
	// everything this would catch; the assert is here so a new producer
	// notices at once instead of drawing through a pipeline whose
	// vertex stride does not match the buffer.
	dbg_assert(IsIndexedDrawConsistent(*pCommand), "Backend received an inconsistent indexed draw");
	const EPipelineProgram Program = pCommand->m_Program;
	const auto &Pipelines = m_aPipelineSets[m_RenderTarget.IsValid() ? 1 : 0];
	const bool Textured = pCommand->m_State.m_Texture.IsValid();
	const size_t PipelineIndex = BlendIndex(pCommand->m_State.m_BlendMode) * 2 + Textured;
	constexpr size_t QuadIndexBytes = 6 * sizeof(uint32_t);
	if(Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
	{
		const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataDualAtlas>();
		if(pCommand->m_InstanceCount != 1 || pDrawData == nullptr || pDrawData->m_TextureSize <= 0.0f)
			return true;
		if(!EnsureUploadSpace(0, sizeof(SPrimitiveTransform) + m_UniformAlignment))
			return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
		if(!EnsureRenderPass())
			return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
		if(!ApplyState(pCommand->m_State, Pipelines.m_aDualAtlas[BlendIndex(pCommand->m_State.m_BlendMode)], {.m_Color = pDrawData->m_PrimaryColor, .m_TextureSize = pDrawData->m_TextureSize, .m_SecondaryColor = pDrawData->m_SecondaryColor}))
			return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
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
		const size_t BaseQuadOffset = pCommand->m_IndexOffset / QuadIndexBytes;
		if(pCommand->m_InstanceCount != 1 || pQuadData == nullptr || BaseQuadOffset > UINT32_MAX - (QuadCount - 1))
			return true;
		// One chunk of quads is reserved at a time. Reserving the whole draw at
		// once would ask for more than a whole uniform buffer from roughly
		// thirty thousand quads on, and the draw was then dropped as a single
		// oversized one rather than split across submissions.
		const uint64_t ChunkUniformBytes = GRAPHICS_MAX_QUADS_RENDER_COUNT * sizeof(CCommandBuffer::SDrawDataQuadTransform) + sizeof(SPrimitiveTransform) + 2 * m_UniformAlignment;
		uint32_t QuadsLeft = QuadCount;
		uint32_t RenderOffset = 0;
		bool BuffersBound = false;
		do
		{
			const uint32_t DrawCount = std::min<uint32_t>(QuadsLeft, GRAPHICS_MAX_QUADS_RENDER_COUNT);
			if(!EnsureUploadSpace(0, ChunkUniformBytes))
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			// A submission inside EnsureUploadSpace closes the pass, and the pass
			// carries the vertex and index buffer, so they are bound again.
			if(m_RenderPass == nullptr)
				BuffersBound = false;
			if(!EnsureRenderPass())
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			if(!BuffersBound)
			{
				wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, VertexBuffer.m_Buffer, 0, VertexBuffer.m_Size);
				wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, IndexBuffer.m_Buffer, WGPUIndexFormat_Uint32, pCommand->m_IndexOffset, static_cast<uint64_t>(pCommand->m_IndexCount) * sizeof(uint32_t));
				BuffersBound = true;
			}
			if(Shared || QuadCount == 1)
			{
				if(!ApplyState(pCommand->m_State, Pipelines.m_aQuadShared[PipelineIndex], {.m_Color = pQuadData->m_Color, .m_Rotation = pQuadData->m_Rotation, .m_VertexOffset = pQuadData->m_Offset}))
					return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
				wgpuRenderPassEncoderDrawIndexed(m_RenderPass, pCommand->m_IndexCount, 1, 0, 0, 0);
				return true;
			}
			uint32_t QuadUniformOffset = 0;
			if(!WriteQuadTransforms(pQuadData + RenderOffset, DrawCount, QuadUniformOffset))
				return false;
			if(!ApplyState(pCommand->m_State, Pipelines.m_aQuadPerItem[PipelineIndex], {.m_QuadBase = static_cast<uint32_t>(BaseQuadOffset) + RenderOffset}))
				return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
			SetBindGroupCached(2, m_QuadBindGroup, &QuadUniformOffset);
			wgpuRenderPassEncoderDrawIndexed(m_RenderPass, DrawCount * 6, 1, RenderOffset * 6, 0, 0);
			RenderOffset += DrawCount;
			QuadsLeft -= DrawCount;
		} while(QuadsLeft > 0);
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
		Pipeline = Pipelines.m_aPrimitive[PrimitivePipelineIndex(EPrimitiveType::TRIANGLES, pCommand->m_State.m_BlendMode, Textured)];
	}
	else if(Program == EPipelineProgram::PRIMITIVE_UNIFORM_COLOR)
	{
		const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>();
		if(pDrawData == nullptr || pCommand->m_InstanceCount != 1)
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
		if(pDrawData == nullptr || pInstances == nullptr || pCommand->m_InstanceCount == 0)
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
		if(pCommand->m_InstanceCount != 1 || (Transform ? pTransformData == nullptr : pColorData == nullptr))
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

	if(!EnsureUploadSpace(pInstances != nullptr ? static_cast<uint64_t>(InstanceCount) * sizeof(*pInstances) : 0, sizeof(SPrimitiveTransform) + m_UniformAlignment))
		return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
	if(!EnsureRenderPass())
		return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
	uint64_t InstanceOffset = 0;
	if(pInstances != nullptr && !WriteStream(pInstances, static_cast<size_t>(InstanceCount) * sizeof(*pInstances), 4, InstanceOffset))
		return false;
	if(!ApplyState(pCommand->m_State, Pipeline, {.m_Color = Color, .m_RotationCenter = RotationCenter, .m_Rotation = Rotation, .m_VertexOffset = VertexOffset, .m_VertexScale = VertexScale}, TextureArray))
		return m_Error.m_ErrorType == GFX_ERROR_TYPE_NONE;
	wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 0, VertexBuffer.m_Buffer, 0, VertexBuffer.m_Size);
	if(pInstances != nullptr)
		wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, 1, m_StreamBuffer, InstanceOffset, static_cast<size_t>(InstanceCount) * sizeof(*pInstances));
	wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, IndexBuffer.m_Buffer, pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, pCommand->m_IndexOffset, static_cast<uint64_t>(pCommand->m_IndexCount) * IndexSize);
	wgpuRenderPassEncoderDrawIndexed(m_RenderPass, pCommand->m_IndexCount, InstanceCount, 0, 0, 0);
	return true;
}

void CCommandProcessorFragment_WebGpu::EndRenderPass()
{
	if(m_RenderPass == nullptr)
		return;
	wgpuRenderPassEncoderEnd(m_RenderPass);
	wgpuRenderPassEncoderRelease(m_RenderPass);
	m_RenderPass = nullptr;
}

// ---------------------------------------------------------------------------
// Readback: textures and the screen back into the client.
// ---------------------------------------------------------------------------

bool CCommandProcessorFragment_WebGpu::StartTextureReadback(WGPUTexture Texture, WGPUOrigin3D Origin, uint32_t Width, uint32_t Height, bool BGRA, bool OpaqueAlpha, CCommandBuffer::SImageReadbackResult *pResult)
{
	// One more in flight than the video export keeps slots would only
	// buy memory, so the oldest is waited out instead.
	while(m_vPendingReadbacks.size() >= READBACK_SLOT_COUNT)
	{
		if(!FinishOldestReadback())
			return false;
	}

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

	SPendingReadback Pending;
	Pending.m_Buffer = Buffer;
	Pending.m_BufferSize = BufferSize;
	Pending.m_BytesPerRow = BytesPerRow;
	Pending.m_Width = Width;
	Pending.m_Height = Height;
	Pending.m_BGRA = BGRA;
	Pending.m_OpaqueAlpha = OpaqueAlpha;
	Pending.m_pResult = pResult;
	Pending.m_pMapResult = std::make_shared<SMapResult>();

	WGPUBufferMapCallbackInfo CallbackInfo = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	CallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
	CallbackInfo.callback = MapCallback;
	CallbackInfo.userdata1 = new std::shared_ptr<SMapResult>(Pending.m_pMapResult);
	wgpuBufferMapAsync(Buffer, WGPUMapMode_Read, 0, BufferSize, CallbackInfo);
	m_vPendingReadbacks.push_back(std::move(Pending));
	return true;
}

void CCommandProcessorFragment_WebGpu::FinishReadback(SPendingReadback &Pending)
{
	CCommandBuffer::SImageReadbackResult *pResult = Pending.m_pResult;
	if(Pending.m_pMapResult->m_Status == WGPUMapAsyncStatus_Success)
	{
		const auto *pMappedData = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(Pending.m_Buffer, 0, Pending.m_BufferSize));
		if(pMappedData != nullptr && pResult->m_Image.TryReuse(Pending.m_Width, Pending.m_Height, CImageInfo::FORMAT_RGBA))
		{
			for(uint32_t Y = 0; Y < Pending.m_Height; ++Y)
			{
				const uint8_t *pSource = pMappedData + static_cast<size_t>(Y) * Pending.m_BytesPerRow;
				uint8_t *pDestination = pResult->m_Image.m_pData + static_cast<size_t>(Y) * Pending.m_Width * 4;
				for(uint32_t X = 0; X < Pending.m_Width; ++X)
				{
					pDestination[X * 4] = pSource[X * 4 + (Pending.m_BGRA ? 2 : 0)];
					pDestination[X * 4 + 1] = pSource[X * 4 + 1];
					pDestination[X * 4 + 2] = pSource[X * 4 + (Pending.m_BGRA ? 0 : 2)];
					pDestination[X * 4 + 3] = Pending.m_OpaqueAlpha ? 255 : pSource[X * 4 + 3];
				}
			}
			pResult->m_Ok = true;
		}
		wgpuBufferUnmap(Pending.m_Buffer);
	}
	wgpuBufferRelease(Pending.m_Buffer);
	Pending.m_Buffer = nullptr;
	if(!pResult->m_Ok)
		log_warn("gfx/webgpu", "texture readback failed");
	pResult->Signal();
}

bool CCommandProcessorFragment_WebGpu::FinishOldestReadback()
{
	if(m_vPendingReadbacks.empty())
		return true;
	SPendingReadback Pending = std::move(m_vPendingReadbacks.front());
	m_vPendingReadbacks.erase(m_vPendingReadbacks.begin());
	const bool Mapped = ProcessUntilDone(*Pending.m_pMapResult, "map texture readback");
	FinishReadback(Pending);
	return Mapped;
}

void CCommandProcessorFragment_WebGpu::CollectFinishedReadbacks()
{
	if(m_vPendingReadbacks.empty())
		return;
	wgpuInstanceProcessEvents(m_Instance);
	auto It = m_vPendingReadbacks.begin();
	while(It != m_vPendingReadbacks.end())
	{
		if(!It->m_pMapResult->m_Done)
		{
			// The queue finishes them in order, so a readback that is
			// not there yet means none behind it is either.
			break;
		}
		FinishReadback(*It);
		It = m_vPendingReadbacks.erase(It);
	}
}

bool CCommandProcessorFragment_WebGpu::FinishReadbacks()
{
	bool Ok = true;
	while(!m_vPendingReadbacks.empty())
	{
		if(!FinishOldestReadback())
			Ok = false;
	}
	return Ok;
}

void CCommandProcessorFragment_WebGpu::AbandonReadbacks()
{
	for(SPendingReadback &Pending : m_vPendingReadbacks)
	{
		if(Pending.m_Buffer != nullptr)
			wgpuBufferRelease(Pending.m_Buffer);
		Pending.m_pResult->Signal();
	}
	m_vPendingReadbacks.clear();
}

void CCommandProcessorFragment_WebGpu::PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand)
{
	auto &Result = *pCommand->m_pResult;
	Result.m_Ok = false;
	const bool BGRA = m_SurfaceFormat == WGPUTextureFormat_BGRA8Unorm || m_SurfaceFormat == WGPUTextureFormat_BGRA8UnormSrgb;
	if(!BGRA && m_SurfaceFormat != WGPUTextureFormat_RGBA8Unorm && m_SurfaceFormat != WGPUTextureFormat_RGBA8UnormSrgb)
	{
		log_warn("gfx/webgpu", "presentation target readback does not support surface format %d", static_cast<int>(m_SurfaceFormat));
		return;
	}
	// What the screen shows is the image the frame was drawn into, and
	// that one can be read at any point in the frame.
	WGPUTexture SourceTexture = m_ScreenTexture;
	if(SourceTexture == nullptr)
	{
		log_warn("gfx/webgpu", "presentation target readback found nothing drawn");
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
	if(!StartTextureReadback(SourceTexture, Origin, Width, Height, BGRA, true, pCommand->m_pResult))
	{
		log_warn("gfx/webgpu", "presentation target readback failed");
		return;
	}
	// The result is signalled when the mapping lands, not when this
	// command buffer ends.
	pCommand->m_pCompletion = nullptr;
}

void CCommandProcessorFragment_WebGpu::TextureReadback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
{
	auto &Result = *pCommand->m_pResult;
	Result.m_Ok = false;
	if(!m_TextureHandles.IsActive(pCommand->m_Texture) || static_cast<size_t>(pCommand->m_Texture.Id()) >= m_vTextures.size())
		return;
	const auto &Texture = m_vTextures[pCommand->m_Texture.Id()];
	if(Texture.m_Texture == nullptr || (Texture.m_Usage & (IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE)) != (IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE))
		return;
	if(!StartTextureReadback(Texture.m_Texture, {}, static_cast<uint32_t>(Texture.m_Width), static_cast<uint32_t>(Texture.m_Height), false, false, pCommand->m_pResult))
	{
		log_warn("gfx/webgpu", "texture readback failed");
		return;
	}
	// The result is signalled when the mapping lands, not when this
	// command buffer ends.
	pCommand->m_pCompletion = nullptr;
}

CCommandProcessorFragment_Renderer *CreateWebGpuCommandProcessorFragment(EWebGpuBackendType BackendType)
{
	return new CCommandProcessorFragment_WebGpu(BackendType);
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
