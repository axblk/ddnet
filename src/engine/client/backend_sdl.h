#ifndef ENGINE_CLIENT_BACKEND_SDL_H
#define ENGINE_CLIENT_BACKEND_SDL_H

#include <base/detect.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/backend/webgpu/backend_webgpu.h>
#include <engine/client/backend_threaded.h>
#include <engine/client/graphics_threaded.h>
#include <engine/graphics.h>

#include <SDL_video.h>

#include <cstddef>
#include <cstdint>

class CCommandProcessor_SDL : public CCommandProcessor_Threaded
{
public:
	enum
	{
		CMD_INIT = CCommandBuffer::CMDGROUP_PLATFORM_SDL,
		CMD_SHUTDOWN,
	};

	struct SCommand_Init : public CCommandBuffer::SCommand
	{
		SCommand_Init() :
			SCommand(CMD_INIT) {}
		SDL_Window *m_pWindow;
		SDL_GLContext m_GLContext;
	};

	struct SCommand_Shutdown : public CCommandBuffer::SCommand
	{
		SCommand_Shutdown() :
			SCommand(CMD_SHUTDOWN) {}
	};

private:
	SDL_Window *m_pWindow = nullptr;
	SDL_GLContext m_GLContext = nullptr;

	void Cmd_Init(const SCommand_Init *pCommand);
	void Cmd_Shutdown();
	void Cmd_Swap();
	void Cmd_VSync(const CCommandBuffer::SCommand_VSync *pCommand);
	void Cmd_WindowCreateNtf(const CCommandBuffer::SCommand_WindowCreateNtf *pCommand);
	void Cmd_WindowDestroyNtf();
	bool RunPlatformCommand(const CCommandBuffer::SCommand *pCommand) override;

public:
	CCommandProcessor_SDL(EBackendType BackendType, int GLMajor, int GLMinor, const SWebGpuNativeWindow &WebGpuNativeWindow, EWebGpuBackendType WebGpuBackendType);
};

// graphics backend implemented with SDL and the graphics library @see EBackendType
class CGraphicsBackend_SDL : public CGraphicsBackend_Threaded
{
	SDL_Window *m_pWindow = nullptr;
	SDL_GLContext m_GLContext = nullptr;
	ICommandProcessor *m_pProcessor = nullptr;
	std::atomic<uint64_t> m_TextureMemoryUsage{0};
	std::atomic<uint64_t> m_BufferMemoryUsage{0};
	std::atomic<uint64_t> m_StreamMemoryUsage{0};
	std::atomic<uint64_t> m_StagingMemoryUsage{0};

	TTwGraphicsGpuList m_GpuList;

	int m_NumScreens;

	SBackendCapabilities m_Capabilities{};

	char m_aVendorString[GPU_INFO_STRING_SIZE] = {};
	char m_aVersionString[GPU_INFO_STRING_SIZE] = {};
	char m_aRendererString[GPU_INFO_STRING_SIZE] = {};

	EBackendType m_BackendType = BACKEND_TYPE_AUTO;
	EBackendType m_BackendOverride;
	SWebGpuNativeWindow m_WebGpuNativeWindow;
#if defined(CONF_BACKEND_WEBGPU) && defined(CONF_PLATFORM_MACOS)
	void *m_pWebGpuMetalView = nullptr;
#endif

	char m_aErrorString[256];

	EBackendType DetectBackend() const;
	static void ClampDriverVersion(EBackendType BackendType);
	void DestroyWindow();

public:
	CGraphicsBackend_SDL(EBackendType BackendOverride);
	int Init(const char *pName, int *pScreen, int *pWidth, int *pHeight, int *pRefreshRate, int *pFsaaSamples, int Flags, int *pDesktopWidth, int *pDesktopHeight, int *pCurrentWidth, int *pCurrentHeight, class IStorage *pStorage) override;
	int Shutdown() override;

	uint64_t TextureMemoryUsage() const override;
	uint64_t BufferMemoryUsage() const override;
	uint64_t StreamedMemoryUsage() const override;
	uint64_t StagingMemoryUsage() const override;

	const TTwGraphicsGpuList &GetGpus() const override;

	int GetNumScreens() const override { return m_NumScreens; }
	const char *GetScreenName(int Screen) const override;

	void GetVideoModes(CVideoMode *pModes, int MaxModes, int *pNumModes, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId) override;
	void GetCurrentVideoMode(CVideoMode &CurMode, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int ScreenId) override;

	void Minimize() override;
	void SetWindowParams(int FullscreenMode, bool IsBorderless) override;
	bool SetWindowScreen(int Index, bool MoveToCenter, ivec2 *pDesktopSize) override;
	bool UpdateDisplayMode(int Index, ivec2 *pDesktopSize) override;
	int GetWindowScreen() override;
	int WindowActive() override;
	int WindowOpen() override;
	void SetWindowGrab(bool Grab) override;
	bool ResizeWindow(int w, int h, int RefreshRate) override;
	void GetViewportSize(int &w, int &h) override;
	void NotifyWindow() override;
	bool IsScreenKeyboardShown() override;

	void WindowDestroyNtf(uint32_t WindowId) override;
	void WindowCreateNtf(uint32_t WindowId) override;

	bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) override;
	bool IsConfigModernAPI() override { return IsModernAPI(m_BackendType); }
	SBackendCapabilities GetCapabilities() const override
	{
		return m_Capabilities;
	}

	const char *GetErrorString() override
	{
		if(m_aErrorString[0] != '\0')
			return m_aErrorString;

		return nullptr;
	}

	const char *GetVendorString() override
	{
		return m_aVendorString;
	}

	const char *GetVersionString() override
	{
		return m_aVersionString;
	}

	const char *GetRendererString() override
	{
		return m_aRendererString;
	}

	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override;

	static bool IsModernAPI(EBackendType BackendType);
};

#endif // ENGINE_CLIENT_BACKEND_SDL_H
