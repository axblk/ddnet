#ifndef ENGINE_CLIENT_GRAPHICS_BACKEND_H
#define ENGINE_CLIENT_GRAPHICS_BACKEND_H

// What the frontend asks of a backend: the interface, its capabilities and
// its error codes. Everything here is called on the main thread. A backend
// draws into a surface it is given, or into nothing but render targets; the
// window that owns the surface is not its business.

#include <engine/client/command_buffer.h>
#include <engine/graphics.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

class IPresentationSurface;
class IStorage;

enum EGraphicsBackendErrorCodes
{
	GRAPHICS_BACKEND_ERROR_CODE_NONE = 0,
	GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_INTERFACE_INIT_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_WINDOW_SYSTEM_INIT_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SCREEN_REQUEST_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SCREEN_INFO_REQUEST_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SCREEN_RESOLUTION_REQUEST_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_WINDOW_CREATE_FAILED,
};

// Technical renderer features discovered by the backend.
struct SBackendCapabilities
{
	// Whether a layered texture is an array or a volume: the frontend has to
	// know, because it decides how a layer is addressed. Mipmaps, non-power-of
	// two sizes, programs and which pipelines have a program of their own are
	// the backend's own business - a backend that has no program for something
	// stands in for it.
	bool m_2DArrayTextures = false;
	bool m_RenderTargets = false;
	// The backend can turn a rendered frame into the planar YUV layout an
	// encoder takes, so that a frame crosses the bus at one and a half
	// bytes per pixel instead of four, and already converted.
	bool m_PlanarYuvConversion = false;

	int m_ContextMajor = 0;
	int m_ContextMinor = 0;
	int m_ContextPatch = 0;
};

// Everything a backend needs to start: where to draw, how big that is, and
// what the window found out about the context it made.
struct SGraphicsBackendInit
{
	// Null for a backend that draws into render targets only.
	IPresentationSurface *m_pSurface = nullptr;
	// Whether the surface came with an OpenGL context the render thread has
	// to bind, swap and set the swap interval on.
	bool m_GlContext = false;
	int m_Width = 0;
	int m_Height = 0;
	bool m_VSync = false;
	int m_FsaaSamples = 0;
	// The version asked of the API. For OpenGL the context version, for the
	// others what the backend header says.
	int m_RequestedMajor = 0;
	int m_RequestedMinor = 0;
	int m_RequestedPatch = 0;
	// What GLEW found in the context, for OpenGL.
	int m_GlewMajor = 0;
	int m_GlewMinor = 0;
	int m_GlewPatch = 0;
	IStorage *m_pStorage = nullptr;
};

// interface for the graphics backend
// all these functions are called on the main thread
class IGraphicsBackend
{
public:
	using SFrameMailboxStats = IGraphics::SFrameMailboxStats;

	virtual ~IGraphicsBackend() = default;

	virtual int Init(const SGraphicsBackendInit &Params) = 0;
	virtual int Shutdown() = 0;

	virtual EBackendType BackendType() const = 0;

	virtual uint64_t TextureMemoryUsage() const = 0;
	virtual uint64_t BufferMemoryUsage() const = 0;
	virtual uint64_t StreamedMemoryUsage() const = 0;
	virtual uint64_t StagingMemoryUsage() const = 0;

	virtual const TTwGraphicsGpuList &GetGpus() const = 0;

	virtual void RunBuffer(CCommandBuffer *pBuffer) = 0;
	// Transfers reliable payload storage into a bounded backend-owned queue.
	virtual bool RunBufferQueued(CCommandBuffer *pBuffer, bool WaitForCapacity = false) = 0;
	// Publishes one complete immutable frame. The buffer is empty on success.
	virtual bool RunFramePacket(CCommandBuffer *pBuffer, bool WaitForCapacity = false) = 0;
	virtual SFrameMailboxStats GetFrameMailboxStats() const = 0;
	virtual SGpuTiming GpuTiming() const = 0;
	virtual void SetGpuTimingEnabled(bool Enabled) = 0;
	virtual void RunBufferSingleThreadedUnsafe(CCommandBuffer *pBuffer) = 0;
	virtual bool IsIdle() const = 0;
	virtual void WaitForIdle() = 0;

	// Gives up what the renderer holds on the surface, for a window that is
	// about to go away under a message box. Only after WaitForIdle().
	virtual void ErroneousCleanup() = 0;

	virtual bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) = 0;
	// checks if the current values of the config are a graphics modern API
	virtual bool IsConfigModernAPI() = 0;
	virtual SBackendCapabilities GetCapabilities() const = 0;
	virtual const char *GetErrorString() { return nullptr; }

	virtual const char *GetVendorString() = 0;
	virtual const char *GetVersionString() = 0;
	virtual const char *GetRendererString() = 0;

	virtual const SGfxErrorContainer &GetError() const = 0;
	virtual bool GetWarning(SGfxWarningContainer &Warning) = 0;
};

// Whether the backend the config names uses programs for everything, which
// decides what the settings menu offers.
bool IsModernGraphicsApi(EBackendType BackendType);

// The backend named in the environment (GFX_BACKEND, or the older
// DDNET_DRIVER), or AUTO to go by the config.
EBackendType GraphicsBackendOverrideFromEnvironment();

// Which version the backend named in the config supports, from the
// backend's own constants rather than a context.
bool GraphicsBackendDriverVersion(EBackendType BackendType, EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName);

// A backend for the given surface, not yet initialized. Null when the type
// is not compiled in.
IGraphicsBackend *CreateGraphicsBackend(EBackendType BackendType, int GlMajor, int GlMinor);

#endif // ENGINE_CLIENT_GRAPHICS_BACKEND_H
