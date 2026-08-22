#ifndef ENGINE_CLIENT_BACKEND_BACKEND_BASE_H
#define ENGINE_CLIENT_BACKEND_BACKEND_BASE_H

#include <engine/client/graphics_threaded.h>
#include <engine/graphics.h>

#include <SDL_video.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
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

	static void Texture2DTo3D(uint8_t *pImageBuffer, int ImageWidth, int ImageHeight, size_t PixelSize, int SplitCountWidth, int SplitCountHeight, uint8_t *pTarget3DImageData, int &Target3DImageWidth, int &Target3DImageHeight);

public:
	virtual ~CCommandProcessorFragment_Renderer() = default;
	virtual ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) = 0;

	const SGfxErrorContainer &GetError() { return m_Error; }
	virtual void ErroneousCleanup() {}

	const SGfxWarningContainer &GetWarning() { return m_Warning; }

	enum
	{
		CMD_PRE_INIT = CCommandBuffer::CMDGROUP_RENDERER,
		CMD_INIT,
		CMD_SHUTDOWN,
		CMD_POST_SHUTDOWN,
	};

	struct SCommand_PreInit : public CCommandBuffer::SCommand
	{
		SCommand_PreInit() :
			SCommand(CMD_PRE_INIT) {}

		SDL_Window *m_pWindow;
		uint32_t m_Width;
		uint32_t m_Height;

		char *m_pVendorString;
		char *m_pVersionString;
		char *m_pRendererString;

		TTwGraphicsGpuList *m_pGpuList;
	};

	struct SCommand_Init : public CCommandBuffer::SCommand
	{
		SCommand_Init() :
			SCommand(CMD_INIT) {}

		SDL_Window *m_pWindow;
		uint32_t m_Width;
		uint32_t m_Height;

		class IStorage *m_pStorage;
		std::atomic<uint64_t> *m_pTextureMemoryUsage;
		std::atomic<uint64_t> *m_pBufferMemoryUsage;
		std::atomic<uint64_t> *m_pStreamMemoryUsage;
		std::atomic<uint64_t> *m_pStagingMemoryUsage;

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
