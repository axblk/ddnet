#ifndef ENGINE_CLIENT_GRAPHICS_THREADED_H
#define ENGINE_CLIENT_GRAPHICS_THREADED_H

#include <base/dbg.h>
#include <base/log.h>
#include <base/sphore.h>

#include <engine/client/command_buffer.h>
#include <engine/client/graphics_backend.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

class CGraphics_Threaded : public IEngineGraphics
{
	CCommandBuffer::SState m_State;
	// Null until Init; Shutdown after a failed start has nothing to reach.
	IGraphicsBackend *m_pBackend = nullptr;
	// What the backend reported, with the one rule the frontend adds: a
	// conversion to planar YUV needs a target to convert into.
	SBackendCapabilities m_Capabilities;
	mutable std::string m_FatalError;

	// The backend's queue takes the commands and leaves an empty buffer behind,
	// so one of each is enough.
	CCommandBuffer *m_pCommandBuffer = nullptr;
	CCommandBuffer *m_pReliableCommandBuffer = nullptr;
	CCommandBuffer *m_pDeferredDestroyCommandBuffer = nullptr;
	bool m_DropCurrentFrame = false;
	std::vector<CTextureHandle> m_vRetiredTextureHandles;
	struct STextureInfo
	{
		CTextureHandle m_Handle;
		CTextureDesc m_Desc;
	};
	std::vector<STextureInfo> m_vTextureInfos;
	std::vector<CBufferHandle> m_vRetiredBufferHandles;

	//
	class IStorage *m_pStorage = nullptr;
	class IEngine *m_pEngine = nullptr;

	bool m_RenderEnable;
	bool m_RenderPassActive = true;
	CTextureHandle m_RenderPassTarget;
	CTextureHandle m_OffscreenFrameTarget;

	// Program/layout combinations already reported by CheckDraw.
	std::set<uint32_t> m_ReportedDrawInconsistencies;
	bool m_DoScreenshot;
	char m_aScreenshotName[IO_MAX_PATH_LENGTH];
	bool m_RenderStatsEnabled = false;
	CFrameRenderStats m_CurrentFrameRenderStats;
	CFrameRenderStats m_LastFrameRenderStats;

	CTextureHandle m_NullTexture;

	CGenerationHandlePool<CTextureHandle> m_TextureHandles;
	int m_TextureMemoryUsage;

	std::atomic<bool> m_WarnPngliteIncompatibleImages = false;

	std::mutex m_WarningsMutex;
	std::vector<SWarning> m_vWarnings;

	// is a non full windowed (in a sense that the viewport won't include the whole window),
	// forced viewport, so that it justifies our UI ratio needs
	bool m_IsForcedViewport = false;
	// Pixels at the surface's left and right edge that the display cutout covers.
	int m_InsetLeft = 0;
	int m_InsetRight = 0;

	struct SVertexArrayInfo
	{
		// keep a reference to it, so we can free the ID
		CBufferHandle m_AssociatedBuffer;
	};
	std::vector<SVertexArrayInfo> m_vVertexArrayInfo;
	CGenerationHandlePool<CBufferHandle> m_BufferHandles;
	CBufferHandle m_QuadIndexBuffer;
	unsigned int m_QuadIndexCount = 0;

	struct SQuadContainer
	{
		SQuadContainer(bool AutomaticUpload = true)
		{
			m_vQuads.clear();
			m_QuadBuffer.Invalidate();
			m_UploadedQuadCount = 0;
			m_FreeIndex = -1;

			m_AutomaticUpload = AutomaticUpload;
		}

		struct SQuad
		{
			CCommandBuffer::SVertex m_aVertices[4];
		};

		std::vector<SQuad> m_vQuads;

		CBufferHandle m_QuadBuffer;
		size_t m_UploadedQuadCount;

		int m_FreeIndex;

		bool m_AutomaticUpload;
	};
	std::vector<SQuadContainer> m_vQuadContainers;
	int m_FirstFreeQuadContainer;

	std::vector<WINDOW_RESIZE_FUNC> m_vResizeListeners;

	void *AllocCommandBufferData(size_t AllocSize) { return m_pCommandBuffer->AllocDataChunked(AllocSize); }
	void *AllocReliableCommandBufferData(size_t AllocSize);
	CBufferHandle CreateBufferObjectInternal(size_t UploadDataSize, void *pUploadData, EBufferLifetime Lifetime, bool IsMovedPointer, EBufferUsage Usage);
	bool RecreateBufferObjectInternal(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, EBufferLifetime Lifetime, bool IsMovedPointer, EBufferUsage Usage);
	bool UpdateTextureInternal(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, uint8_t *pData, bool IsMovedPointer);
	bool DrawFullscreenTexture(CTextureHandle Source, EPipelineProgram Program, SGraphicsColor Color, uint8_t RequiredUsage, bool UseCurrentClip = false);
	void UpdateViewportInternal(int X, int Y, int W, int H, bool ByResize, int SurfaceW, int SurfaceH);

	// The frame arena grows, so only a reliable command can be refused: its
	// buffer is over its payload budget or its data arena is full. The buffer
	// is submitted and the command tried once more; FailFunc allocates again
	// what the command points to in the submitted buffer.
	template<typename TName, typename TFailFunc>
	bool AddCmd(TName &Cmd, TFailFunc &&FailFunc)
	{
		if constexpr(std::is_same_v<TName, CCommandBuffer::SCommand_Draw> || std::is_same_v<TName, CCommandBuffer::SCommand_DrawIndexed>)
		{
			if(m_RenderPassTarget.IsValid() && Cmd.SamplesTexture(m_RenderPassTarget))
				return false;
		}
		CCommandBuffer *pCommandBuffer = GetCommandBuffer(Cmd.m_Cmd);
		if(pCommandBuffer == nullptr)
		{
			// Whatever holds a buffer, texture or container has to give it back
			// before the graphics shut down, because giving it back is a command.
			dbg_assert(!m_ShutDown, "graphics: a command arrived after Shutdown(), release every handle before shutting the graphics down");
			return false;
		}
		if(pCommandBuffer->AddCommandUnsafe(Cmd))
			return true;
		dbg_assert(pCommandBuffer != m_pCommandBuffer, "graphics: the frame arena refused a command");
		// The destroy queue is a resource's last stop. Dropping a command there
		// would leave the resource allocated and its handle retired forever.
		const bool Submitted = pCommandBuffer == m_pDeferredDestroyCommandBuffer ? SubmitDeferredDestroys() : SubmitReliableCommandBuffer(pCommandBuffer);
		if(!Submitted || !FailFunc())
			return false;
		return pCommandBuffer->AddCommandUnsafe(Cmd);
	}

	template<typename TName>
	bool AddCmd(TName &Cmd)
	{
		return AddCmd(Cmd, [] { return true; });
	}

	/**
	 * The one place that decides whether an indexed draw may be submitted, so
	 * the backends only have to assert it.
	 *
	 * Each distinct combination is reported once, because the usual cause is a
	 * texture that has not finished loading.
	 */
	bool CheckIndexedDraw(const CCommandBuffer::SCommand_DrawIndexed &Cmd)
	{
		const char *pReason = IndexedDrawInconsistency(Cmd);
		if(pReason == nullptr)
			pReason = TextureKindMismatch(Cmd.m_Program, Cmd.m_State.m_Texture);
		return ReportRejectedDraw(pReason, Cmd.m_Program, (int)Cmd.m_Layout);
	}

	// A program samples its texture either as a plain 2D image or as a stack of
	// layers, and a texture can be created with only one of the two. Only the
	// frontend has the descriptor to tell.
	[[nodiscard]] const char *TextureKindMismatch(EPipelineProgram Program, CTextureHandle Texture) const
	{
		const CTextureDesc *pDesc = FindTextureDesc(Texture);
		if(pDesc == nullptr)
			return nullptr;
		if(PipelineProgramDesc(Program).m_SamplesLayeredTexture)
		{
			if(pDesc->m_Layering == ETextureLayering::NONE)
				return "the program samples the texture's layers and the texture has none";
		}
		else if(!pDesc->m_Create2D)
			return "the program samples a plain 2D texture and the texture only has layers";
		return nullptr;
	}

	static constexpr int NO_LAYOUT = 0xFF;
	bool ReportRejectedDraw(const char *pReason, EPipelineProgram Program, int Layout)
	{
		if(pReason == nullptr)
			return true;
		const uint32_t Key = (static_cast<uint32_t>(Program) << 16) | static_cast<uint32_t>(Layout);
		if(m_ReportedDrawInconsistencies.insert(Key).second)
			log_error("graphics", "Dropped a draw: %s (program %d, layout %d)", pReason, (int)Program, Layout);
		return false;
	}

	// Fill writes Count payload entries, which the draw reads as its per-draw
	// data or, AsArray, as one entry per quad or instance.
	template<typename TPayload, typename TFill>
	bool SubmitIndexedDraw(CCommandBuffer::SCommand_DrawIndexed &Cmd, size_t Count, bool AsArray, TFill &&Fill)
	{
		if(!CheckIndexedDraw(Cmd))
			return false;
		const size_t Size = Count * sizeof(TPayload);
		auto *pData = static_cast<TPayload *>(AllocCommandBufferData(Size));
		Fill(pData);
		if(AsArray)
			Cmd.m_ArrayData = {pData, Size};
		else
			Cmd.m_DrawData = {pData, Size};
		return AddCmd(Cmd);
	}

	// The descriptor of a live texture, null for anything else.
	[[nodiscard]] const CTextureDesc *FindTextureDesc(CTextureHandle Texture) const
	{
		if(!Texture.IsValid() || !m_TextureHandles.IsAllocated(Texture) || static_cast<size_t>(Texture.Id()) >= m_vTextureInfos.size())
			return nullptr;
		const STextureInfo &Info = m_vTextureInfos[Texture.Id()];
		return Info.m_Handle == Texture ? &Info.m_Desc : nullptr;
	}

	CCommandBuffer *GetCommandBuffer(unsigned Command);
	bool SubmitReliableCommandBuffer(CCommandBuffer *pCommandBuffer);
	bool SubmitFramePacket();
	bool SubmitDeferredDestroys();
	// A destroy that cannot be queued anymore means the backend is gone. Every
	// following destroy hits the same wall, so it is reported only once.
	void ReportLostDestroy(const char *pWhat);
	bool m_ReportedLostDestroy = false;
	// Set once Shutdown() has freed the command buffers.
	bool m_ShutDown = false;
	void RecycleRetiredHandles();
	void DropCurrentFrame();
	void CollectBackendQueueWarnings();
	bool KickCommandBuffer() { return SubmitReliableCommandBuffer(m_pReliableCommandBuffer); }
	// Sends a window notification and waits until the render thread ran it.
	template<typename TCommand>
	void NotifySurface(const char *pWhat);

	void AddBackEndWarningIfExists();

	void AdjustViewport(bool SendViewportChangeToBackend);

	ivec2 m_ReadPixelPosition = ivec2(0, 0);
	ColorRGBA *m_pReadPixelColor = nullptr;
	std::unique_ptr<ITextureReadback> PresentFrame(bool Readback, CImageInfo &&Recycled = CImageInfo());

	// The screen a surface-less client draws into: a canvas-sized target that
	// every frame opens and PresentFrame closes.
	CTextureHandle m_VirtualScreen;
	// A frame drawn into a target has no swap to end it, so it says so here,
	// or its render statistics never roll over.
	bool m_FramePacketEndsFrame = false;
	[[nodiscard]] bool EnsureVirtualScreen();
	[[nodiscard]] bool BeginVirtualScreenFrame();
	std::unique_ptr<ITextureReadback> FinishOffscreenFrame(bool WantImage, CImageInfo &&Recycled, CTextureHandle YuvTarget, EPlanarYuvFormat YuvFormat);
	static void MakeScreenshotOpaque(CImageInfo &Image);
	std::unique_ptr<ITextureReadback> PresentVirtualFrame(bool Readback, CImageInfo &&Recycled);
	// Queues a readback of a color target. Destroying the returned handle waits
	// for the result, so its command storage stays valid.
	std::unique_ptr<ITextureReadback> ReadTextureAsync(CTextureHandle Texture, CImageInfo &&Recycled);
	// Draws Source into the current render target in the planar YUV layout an
	// encoder takes, see EndOffscreenFrame.
	bool ConvertTextureToPlanarYuv(CTextureHandle Source, EPlanarYuvFormat Format);

public:
	// Tells the backend to finish every readback it still owes, so a wait for
	// one always has something that will end it.
	void FinishReadbacks();

private:
	// Without a surface, swaps, vsync, multisampling, viewports and window
	// notifications are not produced at all.
	bool m_Presentable = false;
	void TakeSurfaceInfo(const SGraphicsSurfaceInfo &Surface);
	[[nodiscard]] bool HasPresentationSurface() const { return m_Presentable; }

public:
	CGraphics_Threaded();

	void ClipEnable(int x, int y, int w, int h) override;
	void ClipDisable() override;

	void BlendNone() override;
	void BlendNormal() override;
	void BlendAdditive() override;

	void WrapNormal() override;
	void WrapClamp() override;

	uint64_t TextureMemoryUsage() const override;
	uint64_t BufferMemoryUsage() const override;
	uint64_t StreamedMemoryUsage() const override;
	uint64_t StagingMemoryUsage() const override;
	SFrameMailboxStats FrameMailboxStats() const override;
	CFrameRenderStats FrameRenderStats() const override;
	void SetRenderStatsEnabled(bool Enabled) override;

	const TTwGraphicsGpuList &GetGpus() const override;

	void MapScreen(const CScreenRect &ScreenRect) override;
	CScreenRect GetScreen() const override;

	IGraphics::CTextureHandle FindFreeTextureIndex();
	void FreeTextureIndex(CTextureHandle *pIndex);
	void StoreTextureInfo(CTextureHandle Texture, const CTextureDesc &Desc);
	void UnloadTexture(IGraphics::CTextureHandle *pIndex) override;
	void LoadTextureAddWarning(const CTextureDesc &Desc, const char *pTexName);
	IGraphics::CTextureHandle LoadTextureRaw(const CImageInfo &Image, int Flags, const char *pTexName = nullptr) override;
	IGraphics::CTextureHandle LoadTextureRawMove(CImageInfo &Image, int Flags, const char *pTexName = nullptr) override;
	IGraphics::CTextureHandle CreateTexture(const CTextureDesc &Desc, const void *pInitialData = nullptr) override;
	bool BeginOffscreenFrame(CTextureHandle Texture) override;
	std::unique_ptr<ITextureReadback> EndOffscreenFrame(CImageInfo &&Recycled = CImageInfo(), CTextureHandle YuvTarget = CTextureHandle(), EPlanarYuvFormat YuvFormat = EPlanarYuvFormat::NV12) override;
	std::unique_ptr<ITextureReadback> PresentAndReadbackAsync(CImageInfo &&Recycled = CImageInfo()) override;
	bool PlanarYuvConversionSupported() const override { return m_Capabilities.m_PlanarYuvConversion; }
	bool UpdateTexture(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, const void *pData) override;

	CTextureHandle LoadSpriteTexture(const CImageInfo &FromImageInfo, const std::optional<CImageInfo> &FallbackImageInfo, const struct CDataSprite *pSprite) override;

	bool IsImageSubFullyTransparent(const CImageInfo &FromImageInfo, int x, int y, int w, int h) override;
	bool IsSpriteTextureFullyTransparent(const CImageInfo &FromImageInfo, const struct CDataSprite *pSprite) override;

	// simple uncompressed RGBA loaders
	IGraphics::CTextureHandle LoadTexture(const char *pFilename, int StorageType, int Flags = 0) override;
	bool LoadPng(CImageInfo &Image, const char *pFilename, int StorageType) override;
	bool LoadPng(CImageInfo &Image, const uint8_t *pData, size_t DataSize, const char *pContextName) override;

	bool CheckImageDivisibility(const char *pContextName, CImageInfo &Image, int DivX, int DivY, bool AllowResize) override;
	bool IsImageFormatRgba(const char *pContextName, const CImageInfo &Image) override;

	void TextureSet(CTextureHandle TextureId) override;

	void Clear(float r, float g, float b, bool ForceClearNow = false) override;
	bool BeginRenderPass(const CRenderPassDesc &Desc) override;
	bool EndRenderPass() override;
	bool BlitTexture(CTextureHandle Source, bool UseCurrentClip = false) override;

	void QuadsTex3DDrawTL(const CQuadItem *pArray, int Num) override;

	int CreateQuadContainer(bool AutomaticUpload = true) override;
	void QuadContainerChangeAutomaticUpload(int ContainerIndex, bool AutomaticUpload) override;
	void QuadContainerUpload(int ContainerIndex) override;
	int QuadContainerAddQuads(int ContainerIndex, CQuadItem *pArray, int Num) override;
	int QuadContainerAddQuads(int ContainerIndex, CFreeformItem *pArray, int Num) override;
	void QuadContainerReset(int ContainerIndex) override;
	void DeleteQuadContainer(int &ContainerIndex) override;
	void RenderQuadContainer(int ContainerIndex, int QuadDrawNum) override;
	void RenderQuadContainer(int ContainerIndex, int QuadOffset, int QuadDrawNum, bool ChangeWrapMode = true) override;
	void RenderQuadContainerEx(int ContainerIndex, int QuadOffset, int QuadDrawNum, float X, float Y, float ScaleX = 1.f, float ScaleY = 1.f) override;
	void RenderQuadContainerAsSprite(int ContainerIndex, int QuadOffset, float X, float Y, float ScaleX = 1.f, float ScaleY = 1.f) override;
	void RenderQuadContainerAsSpriteMultiple(int ContainerIndex, int QuadOffset, int DrawCount, SRenderSpriteInfo *pRenderInfo) override;

	void FlushVertices(bool KeepVertices = false) override;
	void FlushVerticesTex3D() override;

	template<typename TVertex>
	void FlushVerticesImpl(bool KeepVertices, EPipelineProgram Program, const TVertex *pVertices)
	{
		if(m_NumVertices == 0)
			return;

		const size_t VertexCount = m_NumVertices;

		if(!KeepVertices)
			m_NumVertices = 0;

		// NO_LAYOUT: a streamed draw has no vertex buffer whose layout could disagree.
		if(!ReportRejectedDraw(TextureKindMismatch(Program, m_State.m_Texture), Program, NO_LAYOUT))
			return;

		CCommandBuffer::SCommand_Draw Command;
		Command.m_State = m_State;
		Command.m_Program = Program;
		Command.m_VertexCount = static_cast<uint32_t>(VertexCount);

		if(m_Drawing == EDrawing::QUADS)
		{
			Command.m_PrimitiveType = EPrimitiveType::QUADS;
			Command.m_IndexBuffer = m_QuadIndexBuffer;
		}
		else if(m_Drawing == EDrawing::LINES)
			Command.m_PrimitiveType = EPrimitiveType::LINES;
		else if(m_Drawing == EDrawing::TRIANGLES)
			Command.m_PrimitiveType = EPrimitiveType::TRIANGLES;
		else
			return;

		Command.m_VertexData.m_Size = sizeof(TVertex) * VertexCount;
		void *pData = AllocCommandBufferData(Command.m_VertexData.m_Size);
		mem_copy(pData, pVertices, Command.m_VertexData.m_Size);
		Command.m_VertexData.m_pData = pData;
		AddCmd(Command);
	}

	void RenderTileLayer(CBufferHandle VertexBuffer, EVertexLayout Layout, const ColorRGBA &Color, const uint32_t *pFirstIndices, const uint32_t *pIndexCounts, size_t RangeCount) override;
	void RenderBorderTiles(CBufferHandle VertexBuffer, EVertexLayout Layout, const ColorRGBA &Color, uint32_t FirstIndex, const vec2 &Offset, const vec2 &Scale, uint32_t DrawNum) override;
	void RenderQuadLayer(CBufferHandle VertexBuffer, EVertexLayout Layout, SQuadRenderInfo *pQuadInfo, size_t QuadNum, int QuadOffset, bool Grouped = false) override;
	void RenderText(CBufferHandle VertexBuffer, int TextQuadNum, int TextureSize, CTextureHandle Texture, const ColorRGBA &TextColor, const ColorRGBA &TextOutlineColor) override;

	// modern GL functions
	CBufferHandle CreateBufferObject(std::span<const uint8_t> Data, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) override;
	bool RecreateBufferObject(CBufferHandle Buffer, std::span<const uint8_t> Data, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) override;
	CBufferHandle CreateBufferObjectMoved(void *pData, size_t Size, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) override;
	bool RecreateBufferObjectMoved(CBufferHandle Buffer, void *pData, size_t Size, EBufferLifetime Lifetime = EBufferLifetime::PERSISTENT) override;
	void DeleteBufferObject(CBufferHandle &Buffer) override;

	// destroying all buffer objects means, that all referenced VBOs are destroyed automatically, so the user does not need to save references to them
	[[nodiscard]] bool IndicesNumRequiredNotify(unsigned int RequiredIndicesCount) override;

	void WarnPngliteIncompatibleImages(bool Warn) override;
	void UpdateViewport(int X, int Y, int W, int H, bool ByResize) override;

	void AddWindowResizeListener(WINDOW_RESIZE_FUNC pFunc) override;

	int Init(IGraphicsBackend *pBackend, const SGraphicsSurfaceInfo &Surface) override;
	void Shutdown() override;
	bool Resized(const SGraphicsSurfaceInfo &Surface) override;
	void PresentationSurfaceLost() override;
	void PresentationSurfaceRestored() override;
	void ReleaseSurfaceForMessageBox() override;

	void ReadPixel(ivec2 Position, ColorRGBA *pColor) override;
	void TakeScreenshot(const char *pFilename) override;
	void TakeCustomScreenshot(const char *pFilename) override;
	void ReadFramebuffer(CImageInfo &Image);
	void SetScreenSize(int Width, int Height);
	void Swap() override;
	bool SetVSync(bool State) override;
	bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) override;

	// synchronization
	void InsertSignal(CSemaphore *pSemaphore) override;
	bool IsIdle() const override;
	void WaitForIdle() override;

	void AddWarning(const SWarning &Warning) override;
	std::optional<SWarning> CurrentWarning() override;

	bool IsBackendInitialized() override;

	std::vector<SRendererChoice> RendererChoices() const override { return GraphicsBackendRendererChoices(); }

	const char *GetVendorString() override;
	const char *GetVersionString() override;
	const char *GetRendererString() override;
	const char *GetFatalError() const override;
};

#endif // ENGINE_CLIENT_GRAPHICS_THREADED_H
