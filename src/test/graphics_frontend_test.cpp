#include "test.h"

#include <base/str.h>

#include <engine/client/graphics_backend.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/kernel.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

// The graphics without a window: the frontend, the backend host and the
// null renderer, opened through the window-less window. This runs wherever
// the test runner runs - there is no SDL, no display and no GPU in it - and
// so it proves that the graphics build and link without the window.
class GraphicsFrontend : public ::testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CTestInfo m_TestInfo;
	std::unique_ptr<IKernel> m_pKernel;
	std::unique_ptr<IStorage> m_pStorage;
	IEngineGraphicsWindow *m_pWindow = nullptr;
	IEngineGraphics *m_pGraphics = nullptr;
	char m_aSavedBackend[sizeof(g_Config.m_GfxBackend)];

	void SetUp() override
	{
		// The null renderer is picked through the same config the client
		// reads; a GFX_BACKEND in the environment would win over it, so
		// the test does not run under one.
		if(std::getenv("GFX_BACKEND") != nullptr)
			GTEST_SKIP() << "GFX_BACKEND is set";
		str_copy(m_aSavedBackend, g_Config.m_GfxBackend);
		str_copy(g_Config.m_GfxBackend, "Null");
		g_Config.m_GfxScreenWidth = 320;
		g_Config.m_GfxScreenHeight = 240;

		m_pKernel = std::unique_ptr<IKernel>(IKernel::Create());
		m_pKernel->RegisterInterface(CreateTestEngine("graphics-test"));
		m_TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_TestInfo.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
		m_pKernel->RegisterInterface(m_pStorage.get(), false);

		m_pWindow = CreateOffscreenGraphicsWindow();
		m_pKernel->RegisterInterface(m_pWindow);
		m_pKernel->RegisterInterface(static_cast<IGraphicsWindow *>(m_pWindow), false);
		m_pGraphics = CreateEngineGraphicsThreaded();
		m_pKernel->RegisterInterface(m_pGraphics);
		m_pKernel->RegisterInterface(static_cast<IGraphics *>(m_pGraphics), false);
	}

	void TearDown() override
	{
		if(m_pKernel != nullptr)
			m_pKernel->Shutdown();
		m_pKernel.reset();
		str_copy(g_Config.m_GfxBackend, m_aSavedBackend);
	}
};

TEST_F(GraphicsFrontend, OpensDrawsAndShutsDownWithoutWindow)
{
#if defined(CONF_HEADLESS_CLIENT)
	// The headless client's screen has a fixed size, whatever the config says.
	const int ExpectedWidth = 800;
	const int ExpectedHeight = 600;
#else
	const int ExpectedWidth = 320;
	const int ExpectedHeight = 240;
#endif
	IGraphicsBackend *pBackend = m_pWindow->Open(false);
	ASSERT_NE(pBackend, nullptr);
	EXPECT_EQ(pBackend->BackendType(), BACKEND_TYPE_NULL);
	EXPECT_FALSE(m_pWindow->Surface().m_Presentable);
	EXPECT_EQ(m_pWindow->Surface().m_DrawableWidth, ExpectedWidth);
	EXPECT_EQ(m_pWindow->Surface().m_DrawableHeight, ExpectedHeight);

	ASSERT_EQ(m_pGraphics->Init(pBackend, m_pWindow->Surface()), 0);
	EXPECT_TRUE(m_pGraphics->IsBackendInitialized());
	EXPECT_EQ(m_pGraphics->ScreenWidth(), ExpectedWidth);
	EXPECT_EQ(m_pGraphics->ScreenHeight(), ExpectedHeight);
	EXPECT_EQ(m_pWindow->WindowOpen(), 1);
	EXPECT_EQ(m_pWindow->WindowActive(), 1);

	// A few frames the way the client draws them: clear, a quad, swap.
	for(int Frame = 0; Frame < 3; Frame++)
	{
		m_pGraphics->Clear(0.0f, 0.0f, 0.0f);
		m_pGraphics->TextureClear();
		m_pGraphics->QuadsBegin();
		m_pGraphics->SetColor(1.0f, 0.5f, 0.25f, 1.0f);
		IGraphics::CQuadItem Quad(10.0f, 10.0f, 100.0f, 50.0f);
		m_pGraphics->QuadsDrawTL(&Quad, 1);
		m_pGraphics->QuadsEnd();
		m_pGraphics->Swap();
	}
	m_pGraphics->WaitForIdle();

	// A resize through the window reaches the graphics.
	EXPECT_TRUE(m_pWindow->Resize(640, 480, 0));
	EXPECT_EQ(m_pGraphics->ScreenWidth(), 640);
	EXPECT_EQ(m_pGraphics->ScreenHeight(), 480);
	m_pGraphics->Clear(0.0f, 0.0f, 0.0f);
	m_pGraphics->Swap();
	m_pGraphics->WaitForIdle();
}

TEST_F(GraphicsFrontend, ReadsBackTheVirtualScreen)
{
	IGraphicsBackend *pBackend = m_pWindow->Open(false);
	ASSERT_NE(pBackend, nullptr);
	ASSERT_EQ(m_pGraphics->Init(pBackend, m_pWindow->Surface()), 0);

	m_pGraphics->Clear(0.0f, 0.0f, 0.0f);
	std::unique_ptr<IGraphics::ITextureReadback> pReadback = m_pGraphics->PresentAndReadbackAsync();
	ASSERT_NE(pReadback, nullptr);
	CImageInfo Image;
	// The null backend draws nothing.
	EXPECT_FALSE(pReadback->Wait(Image));
	Image.Free();
}

TEST_F(GraphicsFrontend, RegistersGpuRenderZonesByName)
{
	const IGraphics::CGpuRenderZone World = m_pGraphics->RegisterGpuRenderZone("world");
	const IGraphics::CGpuRenderZone Hud = m_pGraphics->RegisterGpuRenderZone("hud");
	EXPECT_TRUE(World.IsValid());
	EXPECT_NE(World, Hud);
	EXPECT_EQ(m_pGraphics->RegisterGpuRenderZone("world"), World);
	for(size_t i = m_pGraphics->GpuRenderZoneNames().size(); i < IGraphics::MAX_GPU_RENDER_ZONES; ++i)
		EXPECT_TRUE(m_pGraphics->RegisterGpuRenderZone(std::to_string(i).c_str()).IsValid());
	EXPECT_FALSE(m_pGraphics->RegisterGpuRenderZone("one too many").IsValid());
	ASSERT_EQ(m_pGraphics->GpuRenderZoneNames().size(), IGraphics::MAX_GPU_RENDER_ZONES);
	EXPECT_EQ(m_pGraphics->GpuRenderZoneNames()[Hud.Index()], "hud");
}
