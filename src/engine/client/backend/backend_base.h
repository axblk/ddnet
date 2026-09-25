#ifndef ENGINE_CLIENT_BACKEND_BACKEND_BASE_H
#define ENGINE_CLIENT_BACKEND_BACKEND_BASE_H

#include <engine/client/command_buffer.h>
#include <engine/client/graphics_backend.h>
#include <engine/client/presentation_surface.h>
#include <engine/graphics.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum EDebugGfxModes
{
	DEBUG_GFX_MODE_NONE = 0,
	DEBUG_GFX_MODE_MINIMUM,
	DEBUG_GFX_MODE_AFFECTS_PERFORMANCE,
	DEBUG_GFX_MODE_VERBOSE,
	DEBUG_GFX_MODE_ALL,
};

enum ERunCommandReturnTypes
{
	RUN_COMMAND_COMMAND_HANDLED = 0,
	RUN_COMMAND_COMMAND_UNHANDLED,
	RUN_COMMAND_COMMAND_WARNING,
	RUN_COMMAND_COMMAND_ERROR,
	// Waits for the browser: the command is run again once the renderer has
	// called its wake-up. Only in the web tools, whose render thread must
	// return to the event loop for the browser to answer.
	RUN_COMMAND_COMMAND_PENDING,
};

enum EGfxErrorType
{
	GFX_ERROR_TYPE_NONE = 0,
	GFX_ERROR_TYPE_INIT,
	GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE,
	GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER,
	GFX_ERROR_TYPE_OUT_OF_MEMORY_STAGING,
	GFX_ERROR_TYPE_RENDER_RECORDING,
	GFX_ERROR_TYPE_RENDER_CMD_FAILED,
	GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED,
	GFX_ERROR_TYPE_SWAP_FAILED,
	GFX_ERROR_TYPE_UNKNOWN,
};

enum EGfxWarningType
{
	GFX_WARNING_TYPE_NONE = 0,
	GFX_WARNING_TYPE_INIT_FAILED,
	GFX_WARNING_TYPE_INIT_FAILED_MISSING_INTEGRATED_GPU_DRIVER,
	GFX_WARNING_TYPE_INIT_FAILED_NO_DEVICE_WITH_REQUIRED_VERSION,
	GFX_WARNING_MISSING_EXTENSION,
};

struct SGfxErrorContainer
{
	EGfxErrorType m_ErrorType = EGfxErrorType::GFX_ERROR_TYPE_NONE;
	std::vector<std::string> m_vErrors;
};

struct SGfxWarningContainer
{
	EGfxWarningType m_WarningType = EGfxWarningType::GFX_WARNING_TYPE_NONE;
	std::vector<std::string> m_vWarnings;
};

class CCommandProcessorFragment_Renderer
{
protected:
	SGfxErrorContainer m_Error;
	SGfxWarningContainer m_Warning;

	// Which of the frontend's handles are alive on this side.
	CGenerationHandleStore<IGraphics::CTextureHandle> m_TextureHandles;
	CGenerationHandleStore<IGraphics::CBufferHandle> m_BufferHandles;

	/**
	 * Turns the screen rectangle a draw is in into the scale and translation
	 * that map it onto clip space. Vulkan's clip space has y pointing down,
	 * OpenGL's and WebGPU's point up.
	 *
	 * @return false when the rectangle is empty.
	 */
	[[nodiscard]] static bool ScreenToClip(const CCommandBuffer::SPoint &ScreenTL, const CCommandBuffer::SPoint &ScreenBR, bool ClipYUp, vec2 &Scale, vec2 &Translate);

	/**
	 * Turns a 2D atlas into the layer-major image an array or volume texture is
	 * uploaded from, resizing first when the atlas does not divide into whole
	 * layers.
	 *
	 * @return The converted buffer, or nullptr when an allocation failed.
	 */
	static std::unique_ptr<uint8_t, decltype(&free)> PrepareLayeredImage(const uint8_t *pData, int Width, int Height, size_t PixelSize, int LayerColumns, int LayerRows, int &LayerWidth, int &LayerHeight);

	// Whether the handles a draw names are alive and its program exists. True
	// for anything that is not a draw.
	[[nodiscard]] bool IsDrawCommandValid(const CCommandBuffer::SCommand *pCommand) const;

	// Copies a mapped readback whose rows are Pitch bytes apart into a tight
	// RGBA image, swapping red and blue for BGRA.
	static void CopyReadbackRows(const uint8_t *pSource, size_t Pitch, uint32_t Width, uint32_t Height, bool BGRA, bool OpaqueAlpha, uint8_t *pDestination);

	// A timestamp difference in nanoseconds, rounded and saturated.
	[[nodiscard]] static uint64_t TicksToNanoseconds(uint64_t Ticks, float PeriodNanoseconds);

	// Reports a command the backend cannot carry out, once per reason.
	void DropCommand(const char *pReason) const;

private:
	mutable std::vector<std::string> m_vDroppedCommandReasons;

public:
	virtual ~CCommandProcessorFragment_Renderer() = default;
	virtual ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) = 0;

	const SGfxErrorContainer &GetError() { return m_Error; }
	virtual void ErroneousCleanup() {}

#if defined(CONF_WEB_PLATFORM)
	/**
	 * What a command that answered `RUN_COMMAND_COMMAND_PENDING` calls when
	 * it may go on, from the browser's callback on the render thread.
	 */
	void SetWakeUp(std::function<void()> &&WakeUp) { m_WakeUp = std::move(WakeUp); }

protected:
	void WakeUp() const
	{
		if(m_WakeUp)
			m_WakeUp();
	}

private:
	std::function<void()> m_WakeUp;

public:
#endif

	const SGfxWarningContainer &GetWarning() { return m_Warning; }

	enum
	{
		CMD_PRE_INIT = CCommandBuffer::CMDGROUP_RENDERER,
		CMD_INIT,
		CMD_SHUTDOWN,
		CMD_POST_SHUTDOWN,
	};

	// Whether there is a surface. Without one there is no swapchain, present
	// queue, vsync or swap.
	struct SPresentationSurface
	{
		// Who owns the window, asked for a Vulkan surface, a native window
		// handle or a buffer swap. Null without a surface.
		IPresentationSurface *m_pSurface = nullptr;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;

		[[nodiscard]] bool IsPresentable() const { return m_pSurface != nullptr; }
	};

	struct SCommand_PreInit : public CCommandBuffer::SCommand
	{
		SCommand_PreInit() :
			SCommand(CMD_PRE_INIT) {}

		SPresentationSurface m_Surface;

		char *m_pVendorString;
		char *m_pVersionString;
		char *m_pRendererString;

		TTwGraphicsGpuList *m_pGpuList;
	};

	struct SCommand_Init : public CCommandBuffer::SCommand
	{
		SCommand_Init() :
			SCommand(CMD_INIT) {}

		SPresentationSurface m_Surface;

		class IStorage *m_pStorage;
		std::atomic<uint64_t> *m_pTextureMemoryUsage;
		std::atomic<uint64_t> *m_pBufferMemoryUsage;
		std::atomic<uint64_t> *m_pStreamMemoryUsage;
		std::atomic<uint64_t> *m_pStagingMemoryUsage;
		SGpuTimingShared *m_pGpuTiming = nullptr;

		TTwGraphicsGpuList *m_pGpuList;

		SBackendCapabilities *m_pCapabilities;
		int *m_pInitError;

		const char **m_pErrStringPtr;

		char *m_pVendorString;
		char *m_pVersionString;
		char *m_pRendererString;

		int m_RequestedMajor;
		int m_RequestedMinor;
		int m_RequestedPatch;
		bool m_VSync;
		uint32_t m_RequestedMultiSamplingCount;

		EBackendType m_RequestedBackend;

		int m_GlewMajor;
		int m_GlewMinor;
		int m_GlewPatch;
	};

	struct SCommand_Shutdown : public CCommandBuffer::SCommand
	{
		SCommand_Shutdown() :
			SCommand(CMD_SHUTDOWN) {}
	};

	struct SCommand_PostShutdown : public CCommandBuffer::SCommand
	{
		SCommand_PostShutdown() :
			SCommand(CMD_POST_SHUTDOWN) {}
	};
};

#endif
