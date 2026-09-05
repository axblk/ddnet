#ifndef ENGINE_CLIENT_PRESENTATION_SURFACE_H
#define ENGINE_CLIENT_PRESENTATION_SURFACE_H

#include <engine/client/backend/webgpu/backend_webgpu.h>

#include <string>
#include <vector>

// What a renderer needs from whatever owns the window, without knowing what
// that is. The window system creates the window and, for OpenGL, the context;
// the renderer runs on its own thread and asks here for the pieces that only
// the window can give it. Every method is called from the render thread.
//
// The handles are opaque on purpose: a Vulkan instance and surface are passed
// as pointers to the real types so that this header pulls in neither Vulkan
// nor the window system. The two implementations cast them back.
class IPresentationSurface
{
public:
	virtual ~IPresentationSurface() = default;

	// The size of the surface in pixels, which on a HiDPI screen is not the
	// size of the window.
	virtual void DrawableSize(int &Width, int &Height) const = 0;

	// OpenGL. The context belongs to the window and is bound to the render
	// thread for as long as the renderer lives.
	virtual bool BindGlContext() = 0;
	virtual void UnbindGlContext() = 0;
	virtual void SwapGlBuffers() = 0;
	virtual bool SetGlSwapInterval(bool VSync) = 0;

	// Vulkan. pInstance points at a VkInstance, pSurface at the VkSurfaceKHR
	// to fill.
	virtual bool VulkanInstanceExtensions(std::vector<std::string> &vExtensions) = 0;
	virtual bool CreateVulkanSurface(const void *pInstance, void *pSurface) = 0;

	// WebGPU builds its surface from the native window handle. Read again
	// after the window came back, because it may be a different one.
	virtual const SWebGpuNativeWindow &WebGpuNativeWindow() const = 0;
};

#endif
