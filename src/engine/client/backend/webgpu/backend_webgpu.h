#ifndef ENGINE_CLIENT_BACKEND_WEBGPU_BACKEND_WEBGPU_H
#define ENGINE_CLIENT_BACKEND_WEBGPU_BACKEND_WEBGPU_H

#include <cstdint>

class CCommandProcessorFragment_Renderer;

enum class EWebGpuBackendType
{
	AUTO,
	D3D12,
	VULKAN,
	METAL,
	OPENGL,
};

struct SWebGpuNativeWindow
{
	enum class EType
	{
		INVALID,
		CANVAS,
		METAL,
		WINDOWS,
		XLIB,
		WAYLAND,
		ANDROID_WINDOW,
	};

	EType m_Type = EType::INVALID;
	void *m_pDisplay = nullptr;
	void *m_pWindow = nullptr;
	uint64_t m_WindowId = 0;
};

// What a request for multisampling comes to on WebGPU. There is no call that
// asks a device which counts it takes: the specification fixes the set at one
// and four for a render attachment and allows nothing else, so this is the
// whole answer. It rounds down, because handing a request for two the cost of
// four is not what was asked for.
inline uint32_t WebGpuMultiSamplingCount(uint32_t Requested)
{
	return Requested >= 4 ? 4 : 0;
}

#if defined(CONF_BACKEND_WEBGPU)
CCommandProcessorFragment_Renderer *CreateWebGpuCommandProcessorFragment(EWebGpuBackendType BackendType);
EWebGpuBackendType WebGpuBackendTypeFromConfig();
#endif

#endif
