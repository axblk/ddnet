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
		ANDROID,
	};

	EType m_Type = EType::INVALID;
	void *m_pDisplay = nullptr;
	void *m_pWindow = nullptr;
	uint64_t m_WindowId = 0;
};

#if defined(CONF_BACKEND_WEBGPU)
CCommandProcessorFragment_Renderer *CreateWebGpuCommandProcessorFragment(EWebGpuBackendType BackendType);
EWebGpuBackendType WebGpuBackendTypeFromConfig();
#endif

#endif
