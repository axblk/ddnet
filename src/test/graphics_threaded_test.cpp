#include <base/sphore.h>

#include <engine/client/graphics_threaded.h>
#include <engine/client/render_command_queue.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>

namespace
{
}

TEST(GraphicsThreaded, GenerationHandlesRejectReusedSlots)
{
	CGenerationHandlePool<IGraphics::CTextureHandle> Pool;
	Pool.Reset(1);

	auto First = Pool.Allocate();
	const auto Stale = First;
	ASSERT_TRUE(Pool.IsAllocated(First));
	EXPECT_EQ(First.Id(), 0);
	EXPECT_NE(First.Generation(), 0u);
	CCommandBuffer Buffer(1024, 1024);
	CCommandBuffer::SCommand_Texture_Create CreateCommand;
	CreateCommand.m_Texture = First;
	CreateCommand.m_Desc.m_Width = 32;
	CreateCommand.m_Desc.m_Height = 64;
	CreateCommand.m_Desc.m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
	CreateCommand.m_Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	CreateCommand.m_Desc.m_Layering = IGraphics::ETextureLayering::LAYERED;
	CreateCommand.m_Desc.m_LayerColumns = 4;
	CreateCommand.m_Desc.m_LayerRows = 8;
	CreateCommand.m_Desc.m_Create2D = false;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(CreateCommand));
	CCommandBuffer::SCommand_Draw DrawCommand;
	DrawCommand.m_State.m_Texture = First;
	DrawCommand.m_Program = EPipelineProgram::PRIMITIVE;
	DrawCommand.m_PrimitiveType = EPrimitiveType::TRIANGLES;
	DrawCommand.m_VertexCount = 3;
	auto *pVertices = static_cast<CCommandBuffer::SVertex *>(Buffer.AllocData(3 * sizeof(CCommandBuffer::SVertex)));
	ASSERT_NE(pVertices, nullptr);
	pVertices[0] = {{1.0f, 2.0f}, {0.0f, 0.0f}, {255, 0, 0, 255}};
	pVertices[1] = {{3.0f, 4.0f}, {1.0f, 0.0f}, {0, 255, 0, 255}};
	pVertices[2] = {{5.0f, 6.0f}, {0.0f, 1.0f}, {0, 0, 255, 255}};
	DrawCommand.m_VertexData = {pVertices, 3 * sizeof(CCommandBuffer::SVertex)};
	ASSERT_TRUE(Buffer.AddCommandUnsafe(DrawCommand));

	ASSERT_TRUE(Pool.Release(&First));
	EXPECT_FALSE(First.IsValid());
	EXPECT_FALSE(Pool.IsAllocated(Stale));

	const auto Reused = Pool.Allocate();
	EXPECT_EQ(Reused.Id(), Stale.Id());
	EXPECT_NE(Reused.Generation(), Stale.Generation());
	EXPECT_TRUE(Pool.IsAllocated(Reused));
	EXPECT_FALSE(Pool.IsAllocated(Stale));

	const auto *pStoredCreate = static_cast<const CCommandBuffer::SCommand_Texture_Create *>(Buffer.Head());
	const auto *pStoredDraw = static_cast<const CCommandBuffer::SCommand_Draw *>(pStoredCreate->m_pNext);
	EXPECT_EQ(pStoredCreate->m_Texture, Stale);
	EXPECT_EQ(pStoredCreate->m_Desc.m_Width, 32u);
	EXPECT_EQ(pStoredCreate->m_Desc.m_Height, 64u);
	EXPECT_EQ(pStoredCreate->m_Desc.m_Format, IGraphics::ETextureFormat::RGBA8_UNORM);
	EXPECT_EQ(pStoredCreate->m_Desc.m_Mipmaps, IGraphics::ETextureMipmaps::NONE);
	EXPECT_EQ(pStoredCreate->m_Desc.m_Layering, IGraphics::ETextureLayering::LAYERED);
	EXPECT_EQ(pStoredCreate->m_Desc.m_LayerColumns, 4);
	EXPECT_EQ(pStoredCreate->m_Desc.m_LayerRows, 8);
	EXPECT_EQ(pStoredCreate->m_Desc.m_Usage, IGraphics::TEXTURE_USAGE_SAMPLED);
	EXPECT_FALSE(pStoredCreate->m_Desc.m_Create2D);
	EXPECT_EQ(pStoredDraw->m_State.m_Texture, Stale);
	EXPECT_NE(pStoredDraw->m_State.m_Texture, Reused);
	EXPECT_EQ(pStoredDraw->m_PrimitiveType, EPrimitiveType::TRIANGLES);
	EXPECT_EQ(pStoredDraw->m_VertexCount, 3u);
	const auto *pStoredVertices = pStoredDraw->m_VertexData.Get<CCommandBuffer::SVertex>(3);
	ASSERT_NE(pStoredVertices, nullptr);
	EXPECT_EQ(pStoredVertices[1].m_Pos, vec2(3.0f, 4.0f));
	EXPECT_EQ(pStoredVertices[2].m_Color, SGraphicsColor(0, 0, 255, 255));
}

TEST(GraphicsThreaded, FrameDataGrowsInStableChunks)
{
	CCommandBuffer Buffer(1024, 64);
	void *pFirst = Buffer.AllocDataChunked(48);
	void *pSecond = Buffer.AllocDataChunked(48);
	ASSERT_NE(pFirst, nullptr);
	ASSERT_NE(pSecond, nullptr);
	EXPECT_NE(pFirst, pSecond);

	Buffer.Reset();
	EXPECT_NE(Buffer.AllocDataChunked(96), nullptr);
}

TEST(GraphicsThreaded, IndexedDrawMustMatchItsProgram)
{
	const auto Draw = [](EPipelineProgram Program, IGraphics::EVertexLayout Layout, bool Textured) {
		CCommandBuffer::SCommand_DrawIndexed Cmd;
		Cmd.m_Program = Program;
		Cmd.m_Layout = Layout;
		Cmd.m_IndexCount = 6;
		if(Textured)
		{
			CGenerationHandlePool<IGraphics::CTextureHandle> Pool;
			Pool.Reset(1);
			Cmd.m_State.m_Texture = Pool.Allocate();
		}
		return Cmd;
	};

	// The layout has to match the pipeline the texture state selects, in both
	// directions: Vulkan and WebGPU bake the vertex stride into the pipeline,
	// so an untextured draw over a buffer with texture coordinates reads it at
	// the wrong stride.
	auto Cmd = Draw(EPipelineProgram::QUAD_SHARED, IGraphics::EVertexLayout::QUAD_TEXTURED, false);
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));
	Cmd = Draw(EPipelineProgram::QUAD_SHARED, IGraphics::EVertexLayout::QUAD, false);
	EXPECT_TRUE(IsIndexedDrawConsistent(Cmd));
	Cmd = Draw(EPipelineProgram::QUAD_SHARED, IGraphics::EVertexLayout::QUAD_TEXTURED, true);
	EXPECT_TRUE(IsIndexedDrawConsistent(Cmd));
	Cmd = Draw(EPipelineProgram::QUAD_SHARED, IGraphics::EVertexLayout::QUAD, true);
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));

	// A tile layer does not accept a quad layout either way round.
	Cmd = Draw(EPipelineProgram::ARRAY_COLOR, IGraphics::EVertexLayout::QUAD, false);
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));

	// Text composites two channels of one atlas and has nothing to draw without it.
	Cmd = Draw(EPipelineProgram::DUAL_ATLAS_COMPOSITE, IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, false);
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));
	Cmd = Draw(EPipelineProgram::DUAL_ATLAS_COMPOSITE, IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, true);
	EXPECT_TRUE(IsIndexedDrawConsistent(Cmd));

	// Programs drawing out of the shared quad index buffer need whole quads.
	Cmd = Draw(EPipelineProgram::QUAD_SHARED, IGraphics::EVertexLayout::QUAD, false);
	Cmd.m_IndexCount = 5;
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));
	Cmd.m_IndexCount = 6;
	Cmd.m_IndexOffset = 4;
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));
	Cmd.m_IndexOffset = 6 * sizeof(uint32_t);
	EXPECT_TRUE(IsIndexedDrawConsistent(Cmd));

	// Nothing to draw is not a draw.
	Cmd.m_IndexCount = 0;
	EXPECT_FALSE(IsIndexedDrawConsistent(Cmd));
}

TEST(GraphicsThreaded, TextureDescriptorsValidateRenderTargetUsage)
{
	IGraphics::CTextureDesc Desc;
	Desc.m_Width = 320;
	Desc.m_Height = 180;
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Desc.m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED | IGraphics::TEXTURE_USAGE_COLOR_TARGET | IGraphics::TEXTURE_USAGE_COPY_SOURCE;
	EXPECT_TRUE(Desc.IsValid());

	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::GENERATE;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Desc.m_Layering = IGraphics::ETextureLayering::LAYERED;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_Layering = IGraphics::ETextureLayering::NONE;
	Desc.m_Create2D = false;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_Create2D = true;
	Desc.m_Format = IGraphics::ETextureFormat::R8_UNORM;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
	Desc.m_Usage = IGraphics::TEXTURE_USAGE_COLOR_TARGET;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_Usage = 1 << 7;
	EXPECT_FALSE(Desc.IsValid());

	CCommandBuffer Buffer(1024, 1024, 1);
	CCommandBuffer::SCommand_Texture_Create Create;
	Create.m_Desc.m_Width = 320;
	Create.m_Desc.m_Height = 180;
	Create.m_Desc.m_Mipmaps = IGraphics::ETextureMipmaps::NONE;
	Create.m_Desc.m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED | IGraphics::TEXTURE_USAGE_COLOR_TARGET;
	Create.m_pData = nullptr;
	EXPECT_TRUE(Buffer.AddCommandUnsafe(Create));
	EXPECT_EQ(Buffer.m_ExternalDataSize, 0u);
}

TEST(GraphicsThreaded, RenderPassCommandsRetainTargetGenerationAndDescriptor)
{
	CGenerationHandlePool<IGraphics::CTextureHandle> Pool;
	Pool.Reset(1);
	IGraphics::CTextureHandle Target = Pool.Allocate();
	const IGraphics::CTextureHandle StoredTarget = Target;

	CCommandBuffer Buffer(1024, 1024);
	CCommandBuffer::SCommand_BeginRenderPass Begin;
	Begin.m_Desc.m_ColorTarget = Target;
	Begin.m_Desc.m_LoadOp = IGraphics::ERenderPassLoadOp::CLEAR;
	Begin.m_Desc.m_ClearColor = ColorRGBA(0.1f, 0.2f, 0.3f, 0.4f);
	CCommandBuffer::SCommand_FlushRenderPass Flush;
	CCommandBuffer::SCommand_EndRenderPass End;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Begin));
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Flush));
	ASSERT_TRUE(Buffer.AddCommandUnsafe(End));

	ASSERT_TRUE(Pool.Release(&Target));
	const IGraphics::CTextureHandle Reused = Pool.Allocate();
	EXPECT_EQ(Reused.Id(), StoredTarget.Id());
	EXPECT_NE(Reused.Generation(), StoredTarget.Generation());

	const auto *pStoredBegin = static_cast<const CCommandBuffer::SCommand_BeginRenderPass *>(Buffer.Head());
	ASSERT_NE(pStoredBegin, nullptr);
	EXPECT_EQ(pStoredBegin->m_Desc.m_ColorTarget, StoredTarget);
	EXPECT_EQ(pStoredBegin->m_Desc.m_LoadOp, IGraphics::ERenderPassLoadOp::CLEAR);
	EXPECT_EQ(pStoredBegin->m_Desc.m_ClearColor, ColorRGBA(0.1f, 0.2f, 0.3f, 0.4f));
	ASSERT_NE(pStoredBegin->m_pNext, nullptr);
	EXPECT_EQ(pStoredBegin->m_pNext->m_Cmd, CCommandBuffer::CMD_FLUSH_RENDER_PASS);
	ASSERT_NE(pStoredBegin->m_pNext->m_pNext, nullptr);
	EXPECT_EQ(pStoredBegin->m_pNext->m_pNext->m_Cmd, CCommandBuffer::CMD_END_RENDER_PASS);
}

TEST(GraphicsThreaded, TextureReadbackRetainsGenerationAndCompletion)
{
	CGenerationHandlePool<IGraphics::CTextureHandle> Pool;
	Pool.Reset(1);
	IGraphics::CTextureHandle Texture = Pool.Allocate();
	const IGraphics::CTextureHandle StoredTexture = Texture;
	CCommandBuffer::SImageReadbackResult Result;
	CCommandBuffer::SCommand_Texture_Readback Readback;
	Readback.m_Texture = Texture;
	Readback.m_pResult = &Result;
	Readback.m_pCompletion = &Result;

	CCommandBuffer Buffer(1024, 1024);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Readback));
	EXPECT_TRUE(Buffer.ContainsCompletions());
	ASSERT_TRUE(Pool.Release(&Texture));
	EXPECT_NE(Pool.Allocate().Generation(), StoredTexture.Generation());

	const auto *pStored = static_cast<const CCommandBuffer::SCommand_Texture_Readback *>(Buffer.Head());
	ASSERT_NE(pStored, nullptr);
	EXPECT_EQ(pStored->m_Texture, StoredTexture);
	EXPECT_EQ(pStored->m_pResult, &Result);
	EXPECT_EQ(pStored->m_pCompletion, &Result);
}

TEST(GraphicsThreaded, RetiredHandlesAreNotReusedBeforeRecycle)
{
	CGenerationHandlePool<IGraphics::CBufferHandle> Pool;
	Pool.Reset(1);
	auto Retired = Pool.Allocate();
	auto CallerHandle = Retired;
	ASSERT_TRUE(Pool.Retire(&CallerHandle));
	EXPECT_FALSE(CallerHandle.IsValid());

	const auto Other = Pool.Allocate();
	EXPECT_NE(Other.Id(), Retired.Id());
	ASSERT_TRUE(Pool.Recycle(Retired));
	const auto Reused = Pool.Allocate();
	EXPECT_EQ(Reused.Id(), Retired.Id());
	EXPECT_NE(Reused.Generation(), Retired.Generation());
}

TEST(GraphicsThreaded, GenerationStoreRejectsStaleResources)
{
	CGenerationHandlePool<IGraphics::CBufferHandle> Pool;
	CGenerationHandleStore<IGraphics::CBufferHandle> Store;
	Pool.Reset(1);
	auto First = Pool.Allocate();
	const auto Stale = First;
	ASSERT_TRUE(Store.Activate(First));
	EXPECT_TRUE(Store.IsActive(First));
	EXPECT_FALSE(Store.Activate(First));
	ASSERT_TRUE(Store.Release(First));
	ASSERT_TRUE(Pool.Release(&First));

	const auto Reused = Pool.Allocate();
	ASSERT_TRUE(Store.Activate(Reused));
	EXPECT_TRUE(Store.IsActive(Reused));
	EXPECT_FALSE(Store.IsActive(Stale));
	EXPECT_FALSE(Store.Release(Stale));
	EXPECT_TRUE(Store.IsActive(Reused));
	Store.Clear();
	EXPECT_FALSE(Store.IsActive(Reused));
}

TEST(GraphicsThreaded, BufferCommandsRetainGenerations)
{
	CGenerationHandlePool<IGraphics::CBufferHandle> BufferPool;
	BufferPool.Reset(2);
	constexpr EPipelineProgram TransformProgram = EPipelineProgram::ARRAY_COLOR_TRANSFORM;
	constexpr EPipelineProgram QuadProgram = EPipelineProgram::QUAD_PER_ITEM;
	auto Buffer = BufferPool.Allocate();
	auto IndexBuffer = BufferPool.Allocate();
	const auto StaleBuffer = Buffer;
	const auto StaleIndexBuffer = IndexBuffer;

	CCommandBuffer CommandBuffer(1024, 1024);
	CCommandBuffer::SCommand_CreateBufferObject CreateBuffer;
	CreateBuffer.m_Buffer = Buffer;
	CreateBuffer.m_Desc = {256, IGraphics::EBufferLifetime::FRAME};
	ASSERT_TRUE(CommandBuffer.AddCommandUnsafe(CreateBuffer));
	CCommandBuffer::SCommand_DrawIndexed Render;
	Render.m_VertexBuffer = Buffer;
	Render.m_Layout = IGraphics::EVertexLayout::QUAD;
	Render.m_IndexBuffer = IndexBuffer;
	Render.m_Program = TransformProgram;
	Render.m_IndexCount = 18;
	Render.m_IndexOffset = 24;
	auto *pDrawData = static_cast<CCommandBuffer::SDrawDataPrimitiveUniformColor *>(CommandBuffer.AllocData(sizeof(CCommandBuffer::SDrawDataPrimitiveUniformColor)));
	ASSERT_NE(pDrawData, nullptr);
	pDrawData->m_Rotation = 0.25f;
	pDrawData->m_RotationCenter = {12.0f, 13.0f};
	pDrawData->m_Color = {0.1f, 0.2f, 0.3f, 0.4f};
	Render.m_DrawData = {pDrawData, sizeof(CCommandBuffer::SDrawDataPrimitiveUniformColor)};
	ASSERT_TRUE(CommandBuffer.AddCommandUnsafe(Render));
	CCommandBuffer::SCommand_DrawIndexed QuadRender;
	QuadRender.m_VertexBuffer = Buffer;
	QuadRender.m_Layout = IGraphics::EVertexLayout::QUAD_TEXTURED;
	QuadRender.m_IndexBuffer = IndexBuffer;
	QuadRender.m_Program = QuadProgram;
	QuadRender.m_IndexCount = 12;
	QuadRender.m_IndexOffset = 48;
	auto *pQuadData = static_cast<CCommandBuffer::SDrawDataQuadTransform *>(CommandBuffer.AllocData(2 * sizeof(CCommandBuffer::SDrawDataQuadTransform)));
	ASSERT_NE(pQuadData, nullptr);
	pQuadData[0] = {ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f), {2.0f, 3.0f}, 0.25f, 0.0f};
	pQuadData[1] = {ColorRGBA(0.0f, 1.0f, 0.0f, 1.0f), {4.0f, 5.0f}, 0.5f, 0.0f};
	QuadRender.m_ArrayData = {pQuadData, 2 * sizeof(CCommandBuffer::SDrawDataQuadTransform)};
	ASSERT_TRUE(CommandBuffer.AddCommandUnsafe(QuadRender));

	ASSERT_TRUE(BufferPool.Release(&Buffer));
	const auto ReusedBuffer = BufferPool.Allocate();
	EXPECT_EQ(ReusedBuffer.Id(), StaleBuffer.Id());
	EXPECT_NE(ReusedBuffer.Generation(), StaleBuffer.Generation());

	const auto *pCreateBuffer = static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(CommandBuffer.Head());
	const auto *pRender = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCreateBuffer->m_pNext);
	const auto *pQuadRender = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pRender->m_pNext);
	EXPECT_EQ(pCreateBuffer->m_Buffer, StaleBuffer);
	EXPECT_EQ(pCreateBuffer->m_Desc.m_Size, 256u);
	EXPECT_EQ(pCreateBuffer->m_Desc.m_Lifetime, IGraphics::EBufferLifetime::FRAME);
	EXPECT_EQ(pCreateBuffer->m_Desc.m_Usage, IGraphics::EBufferUsage::VERTEX);
	EXPECT_EQ(pRender->m_VertexBuffer, StaleBuffer);
	EXPECT_EQ(pRender->m_Layout, IGraphics::EVertexLayout::QUAD);
	EXPECT_EQ(pRender->m_IndexBuffer, StaleIndexBuffer);
	EXPECT_EQ(pRender->m_Program, TransformProgram);
	EXPECT_EQ(pRender->m_IndexCount, 18u);
	EXPECT_EQ(pRender->m_IndexOffset, 24u);
	const auto *pStoredDrawData = pRender->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>();
	ASSERT_NE(pStoredDrawData, nullptr);
	EXPECT_FLOAT_EQ(pStoredDrawData->m_Rotation, 0.25f);
	EXPECT_EQ(pStoredDrawData->m_RotationCenter, vec2(12.0f, 13.0f));
	EXPECT_EQ(pStoredDrawData->m_Color, ColorRGBA(0.1f, 0.2f, 0.3f, 0.4f));
	auto InvalidDrawData = pRender->m_DrawData;
	--InvalidDrawData.m_Size;
	EXPECT_EQ(InvalidDrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>(), nullptr);
	EXPECT_EQ(pQuadRender->m_VertexBuffer, StaleBuffer);
	EXPECT_EQ(pQuadRender->m_Layout, IGraphics::EVertexLayout::QUAD_TEXTURED);
	EXPECT_EQ(pQuadRender->m_IndexBuffer, StaleIndexBuffer);
	EXPECT_EQ(pQuadRender->m_Program, QuadProgram);
	EXPECT_EQ(pQuadRender->m_IndexCount, 12u);
	EXPECT_EQ(pQuadRender->m_IndexOffset, 48u);
	const auto *pStoredQuadData = pQuadRender->m_ArrayData.Get<CCommandBuffer::SDrawDataQuadTransform>(2);
	ASSERT_NE(pStoredQuadData, nullptr);
	EXPECT_EQ(pStoredQuadData[0].m_Color, ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
	EXPECT_EQ(pStoredQuadData[0].m_Offset, vec2(2.0f, 3.0f));
	EXPECT_FLOAT_EQ(pStoredQuadData[1].m_Rotation, 0.5f);
}

TEST(GraphicsThreaded, ArrayDataRequiresExactTypedSpan)
{
	const CCommandBuffer::SInstanceDataPositionScaleRotation aInstances[2] = {
		{{1.0f, 2.0f}, 0.5f, 0.25f},
		{{3.0f, 4.0f}, 1.5f, 0.75f},
	};
	const CCommandBuffer::SArrayData Span{aInstances, sizeof(aInstances)};
	EXPECT_EQ(Span.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(2), aInstances);
	EXPECT_EQ(Span.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(1), nullptr);
	EXPECT_EQ(CCommandBuffer::SArrayData{}.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(0), nullptr);

	const CCommandBuffer::SDrawDataQuadTransform aQuads[2] = {
		{ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f), {2.0f, 3.0f}, 0.25f, 0.0f},
		{ColorRGBA(0.0f, 1.0f, 0.0f, 1.0f), {4.0f, 5.0f}, 0.5f, 0.0f},
	};
	const CCommandBuffer::SArrayData QuadSpan{aQuads, sizeof(aQuads)};
	EXPECT_EQ(QuadSpan.Get<CCommandBuffer::SDrawDataQuadTransform>(2), aQuads);
	EXPECT_EQ(QuadSpan.Get<CCommandBuffer::SDrawDataQuadTransform>(1), nullptr);
}

TEST(GraphicsThreaded, CommandChannelsCoverAllCoreCommands)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	std::array<bool, CCommandBuffer::CMD_COUNT> aSeen{};

	const unsigned aFrameCommands[] = {
		CCommandBuffer::CMD_CLEAR,
		CCommandBuffer::CMD_BEGIN_RENDER_PASS,
		CCommandBuffer::CMD_END_RENDER_PASS,
		CCommandBuffer::CMD_FLUSH_RENDER_PASS,
		CCommandBuffer::CMD_DRAW,
		CCommandBuffer::CMD_DRAW_INDEXED,
		CCommandBuffer::CMD_PRESENTATION_TARGET_READBACK,
		CCommandBuffer::CMD_SWAP,
	};
	for(const unsigned Command : aFrameCommands)
	{
		EXPECT_EQ(CCommandBuffer::CommandChannel(Command), EChannel::FRAME);
		EXPECT_FALSE(aSeen[Command]);
		aSeen[Command] = true;
	}

	const unsigned aReliableCommands[] = {
		CCommandBuffer::CMD_SIGNAL,
		CCommandBuffer::CMD_TEXTURE_CREATE,
		CCommandBuffer::CMD_TEXTURE_DESTROY,
		CCommandBuffer::CMD_TEXTURE_UPDATE,
		CCommandBuffer::CMD_TEXTURE_READBACK,
		CCommandBuffer::CMD_FINISH_READBACKS,
		CCommandBuffer::CMD_CREATE_BUFFER_OBJECT,
		CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT,
		CCommandBuffer::CMD_DELETE_BUFFER_OBJECT,
		CCommandBuffer::CMD_MULTISAMPLING,
		CCommandBuffer::CMD_VSYNC,
		CCommandBuffer::CMD_UPDATE_VIEWPORT,
		CCommandBuffer::CMD_WINDOW_CREATE_NTF,
		CCommandBuffer::CMD_WINDOW_DESTROY_NTF,
	};
	for(const unsigned Command : aReliableCommands)
	{
		EXPECT_EQ(CCommandBuffer::CommandChannel(Command), EChannel::RELIABLE);
		EXPECT_FALSE(aSeen[Command]);
		aSeen[Command] = true;
	}

	for(unsigned Command = CCommandBuffer::CMD_FIRST; Command < CCommandBuffer::CMD_COUNT; ++Command)
		EXPECT_TRUE(aSeen[Command]) << "missing command " << Command;
	EXPECT_EQ(CCommandBuffer::CommandChannel(CCommandBuffer::CMDGROUP_RENDERER), EChannel::RELIABLE);
	EXPECT_EQ(CCommandBuffer::CommandChannel(CCommandBuffer::CMDGROUP_PLATFORM), EChannel::RELIABLE);
	EXPECT_FALSE(CCommandBuffer::UsesReservedReliableBudget(CCommandBuffer::CMD_TEXTURE_CREATE));
	EXPECT_TRUE(CCommandBuffer::UsesReservedReliableBudget(CCommandBuffer::CMD_TEXTURE_DESTROY));
	EXPECT_TRUE(CCommandBuffer::UsesReservedReliableBudget(CCommandBuffer::CMD_WINDOW_DESTROY_NTF));
	// A drain that cannot be queued because the buffer is full would leave the
	// caller waiting for a readback nothing is going to finish.
	EXPECT_TRUE(CCommandBuffer::UsesReservedReliableBudget(CCommandBuffer::CMD_FINISH_READBACKS));
	EXPECT_TRUE(CCommandBuffer::UsesReservedReliableBudget(CCommandBuffer::CMDGROUP_RENDERER));
}

TEST(GraphicsThreaded, DrawCommandsIdentifySampledTextures)
{
	CGenerationHandlePool<IGraphics::CTextureHandle> Pool;
	Pool.Reset(2);
	const auto Target = Pool.Allocate();
	const auto Other = Pool.Allocate();

	CCommandBuffer::SCommand_Draw Draw;
	Draw.m_State.m_Texture = Target;
	EXPECT_TRUE(Draw.SamplesTexture(Target));
	EXPECT_FALSE(Draw.SamplesTexture(Other));

	CCommandBuffer::SCommand_DrawIndexed DrawIndexed;
	DrawIndexed.m_State.m_Texture = Target;
	EXPECT_TRUE(DrawIndexed.SamplesTexture(Target));
	EXPECT_FALSE(DrawIndexed.SamplesTexture(Other));
}

TEST(GraphicsThreaded, TextureDescriptorsValidateLayerGrid)
{
	IGraphics::CTextureDesc Desc;
	Desc.m_Width = 30;
	Desc.m_Height = 50;
	Desc.m_Layering = IGraphics::ETextureLayering::LAYERED;
	Desc.m_LayerColumns = 3;
	Desc.m_LayerRows = 5;
	EXPECT_TRUE(Desc.IsValid());
	EXPECT_EQ(Desc.LayerCount(), 15u);

	Desc.m_LayerColumns = 0;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_LayerColumns = IGraphics::MAX_TEXTURE_LAYERS + 1;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_LayerColumns = std::numeric_limits<int>::max();
	Desc.m_LayerRows = 2;
	EXPECT_FALSE(Desc.IsValid());
	Desc.m_LayerColumns = 2;
	Desc.m_LayerRows = 2;
	Desc.m_Layering = IGraphics::ETextureLayering::NONE;
	EXPECT_FALSE(Desc.IsValid());
}

TEST(GraphicsThreaded, SignalCommandSignalsSemaphore)
{
	CSemaphore Semaphore;
	CCommandBuffer::SCommand_Signal Signal;
	Signal.m_pSemaphore = &Semaphore;

	Signal.Signal();
	EXPECT_EQ(Semaphore.GetApproximateValue(), 1);
	Semaphore.Wait();
}

TEST(GraphicsThreaded, ReliableDataHasIndependentLifetime)
{
	CCommandBuffer FrameBuffer(1024, 1024);
	CCommandBuffer ReliableBuffer(1024, 1024);

	auto *pUploadData = static_cast<int *>(ReliableBuffer.AllocData(sizeof(int)));
	ASSERT_NE(pUploadData, nullptr);
	*pUploadData = 42;

	CCommandBuffer::SCommand_RecreateBufferObject UpdateCommand;
	UpdateCommand.m_DeletePointer = false;
	UpdateCommand.m_pUploadData = pUploadData;
	UpdateCommand.m_Desc.m_Size = sizeof(*pUploadData);
	ASSERT_TRUE(ReliableBuffer.AddCommandUnsafe(UpdateCommand));

	CCommandBuffer::SCommand_Clear ClearCommand;
	ASSERT_TRUE(FrameBuffer.AddCommandUnsafe(ClearCommand));
	EXPECT_FALSE(FrameBuffer.ContainsResourceCommands());
	EXPECT_TRUE(ReliableBuffer.ContainsResourceCommands());
	EXPECT_FALSE(ReliableBuffer.UsesReservedReliableBudget());
	FrameBuffer.Reset();

	ASSERT_FALSE(ReliableBuffer.IsEmpty());
	const auto *pStoredCommand = static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(ReliableBuffer.Head());
	EXPECT_EQ(*static_cast<const int *>(pStoredCommand->m_pUploadData), 42);
}

TEST(GraphicsThreaded, CommandBufferStorageTransfersWithoutCopyingPayload)
{
	CCommandBuffer FrontendBuffer(1024, 1024);
	CCommandBuffer BackendBuffer(1024, 1024);
	auto *pVertices = static_cast<CCommandBuffer::SVertex *>(FrontendBuffer.AllocData(3 * sizeof(CCommandBuffer::SVertex)));
	ASSERT_NE(pVertices, nullptr);
	pVertices[0] = {{1.0f, 2.0f}, {0.0f, 0.0f}, {255, 255, 255, 255}};
	pVertices[1] = {{3.0f, 4.0f}, {1.0f, 0.0f}, {255, 255, 255, 255}};
	pVertices[2] = {{5.0f, 6.0f}, {0.0f, 1.0f}, {255, 255, 255, 255}};

	CCommandBuffer::SCommand_Draw DrawCommand;
	DrawCommand.m_Program = EPipelineProgram::PRIMITIVE;
	DrawCommand.m_VertexCount = 3;
	DrawCommand.m_VertexData = {pVertices, 3 * sizeof(CCommandBuffer::SVertex)};
	ASSERT_TRUE(FrontendBuffer.AddCommandUnsafe(DrawCommand));
	CCommandBuffer::SSubmissionInfo Info;
	Info.m_SubmissionSerial = 7;
	FrontendBuffer.SetSubmissionInfo(Info);

	BackendBuffer.Swap(FrontendBuffer);
	EXPECT_TRUE(FrontendBuffer.IsEmpty());
	ASSERT_FALSE(BackendBuffer.IsEmpty());
	EXPECT_EQ(BackendBuffer.SubmissionInfo().m_SubmissionSerial, 7u);
	const auto *pStoredCommand = static_cast<const CCommandBuffer::SCommand_Draw *>(BackendBuffer.Head());
	EXPECT_EQ(pStoredCommand->m_VertexData.m_pData, pVertices);
	const auto *pStoredVertices = pStoredCommand->m_VertexData.Get<CCommandBuffer::SVertex>(3);
	ASSERT_NE(pStoredVertices, nullptr);
	EXPECT_EQ(pStoredVertices[2].m_Pos, vec2(5.0f, 6.0f));
}

TEST(GraphicsThreaded, CommandBufferReportsRenderWork)
{
	CCommandBuffer Buffer(2048, 2048);

	auto *pVertices = static_cast<CCommandBuffer::SVertex *>(Buffer.AllocData(8 * sizeof(CCommandBuffer::SVertex)));
	ASSERT_NE(pVertices, nullptr);
	CCommandBuffer::SCommand_Draw Draw;
	Draw.m_PrimitiveType = EPrimitiveType::QUADS;
	Draw.m_VertexCount = 8;
	Draw.m_VertexData = {pVertices, 8 * sizeof(*pVertices)};
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Draw));

	CCommandBuffer::SCommand_DrawIndexed Indexed;
	Indexed.m_IndexCount = 12;
	Indexed.m_InstanceCount = 3;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Indexed));

	int UploadData[8];
	CCommandBuffer::SCommand_RecreateBufferObject Update;
	Update.m_pUploadData = UploadData;
	Update.m_Desc.m_Size = sizeof(UploadData);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Update));

	const IGraphics::CFrameRenderStats Stats = Buffer.RenderStats();
	EXPECT_EQ(Stats.m_Commands, 3u);
	EXPECT_EQ(Stats.m_ResourceCommands, 1u);
	EXPECT_EQ(Stats.m_DrawCommands, 2u);
	EXPECT_EQ(Stats.m_DrawCalls, 2u);
	EXPECT_EQ(Stats.m_Triangles, 16u);
	EXPECT_EQ(Stats.m_Instances, 4u);
	EXPECT_EQ(Stats.m_BufferRecreates, 1u);
	EXPECT_EQ(Stats.m_UploadBytes, sizeof(UploadData));
	EXPECT_EQ(Stats.m_StreamedBytes, 8 * sizeof(*pVertices));
}

TEST(GraphicsThreaded, ReliablePayloadBudgetRejectsWholeCommand)
{
	CCommandBuffer Buffer(1024, 1024, 7);
	CCommandBuffer::SCommand_CreateBufferObject Create;
	Create.m_DeletePointer = true;
	Create.m_Desc.m_Size = 4;
	Create.m_pUploadData = reinterpret_cast<void *>(1);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Create));
	EXPECT_EQ(Buffer.m_ExternalDataSize, 4u);

	// The budget limits what is queued behind a payload that is already there.
	Create.m_Desc.m_Size = 8;
	EXPECT_FALSE(Buffer.AddCommandUnsafe(Create));
	EXPECT_EQ(Buffer.m_ExternalDataSize, 4u);

	Create.m_Desc.m_Size = 3;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Create));
	EXPECT_EQ(Buffer.m_ExternalDataSize, 7u);

	CCommandBuffer BackendBuffer(1024, 1024, 7);
	BackendBuffer.Swap(Buffer);
	EXPECT_EQ(Buffer.m_ExternalDataSize, 0u);
	EXPECT_EQ(BackendBuffer.m_ExternalDataSize, 7u);
	BackendBuffer.Reset();
	EXPECT_EQ(BackendBuffer.m_ExternalDataSize, 0u);
}

TEST(GraphicsThreaded, ReliablePayloadBudgetStillTakesOneOversizedCommand)
{
	// A texture that is larger than the whole payload budget would never fit into
	// any buffer. It has to go through on its own instead of failing to upload.
	CCommandBuffer Buffer(1024, 1024, 7);
	CCommandBuffer::SCommand_CreateBufferObject Create;
	Create.m_DeletePointer = true;
	Create.m_Desc.m_Size = 4096;
	Create.m_pUploadData = reinterpret_cast<void *>(1);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Create));
	EXPECT_EQ(Buffer.m_ExternalDataSize, 4096u);

	// but nothing may queue up behind it
	Create.m_Desc.m_Size = 1;
	EXPECT_FALSE(Buffer.AddCommandUnsafe(Create));
	EXPECT_EQ(Buffer.m_ExternalDataSize, 4096u);
}

TEST(GraphicsThreaded, TextureUpdatesUseTypedUploadSizes)
{
	CCommandBuffer UpdateBuffer(1024, 1024, 5);
	CCommandBuffer::SCommand_Texture_Update Update;
	Update.m_Region = {3, 4, 2, 2};
	Update.m_Format = IGraphics::ETextureFormat::R8_UNORM;
	Update.m_pData = reinterpret_cast<uint8_t *>(3);
	ASSERT_TRUE(UpdateBuffer.AddCommandUnsafe(Update));
	EXPECT_EQ(UpdateBuffer.m_ExternalDataSize, 4u);
	Update.m_Region.m_Height = 3;
	EXPECT_FALSE(UpdateBuffer.AddCommandUnsafe(Update));
	EXPECT_EQ(UpdateBuffer.m_ExternalDataSize, 4u);
	const auto *pStoredUpdate = static_cast<const CCommandBuffer::SCommand_Texture_Update *>(UpdateBuffer.Head());
	EXPECT_EQ(pStoredUpdate->m_Region.m_X, 3u);
	EXPECT_EQ(pStoredUpdate->m_Region.m_Y, 4u);
	EXPECT_EQ(pStoredUpdate->m_Format, IGraphics::ETextureFormat::R8_UNORM);

	CCommandBuffer RgbaUpdateBuffer(1024, 1024, 16);
	Update.m_Region = {1, 2, 2, 2};
	Update.m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
	ASSERT_TRUE(RgbaUpdateBuffer.AddCommandUnsafe(Update));
	EXPECT_EQ(RgbaUpdateBuffer.m_ExternalDataSize, 16u);
}

TEST(GraphicsThreaded, ExternalDataCanBeReleasedPerCommand)
{
	CCommandBuffer Buffer(1024, 1024, 12);
	CCommandBuffer::SCommand_Texture_Update Update;
	Update.m_Region = {0, 0, 2, 2};
	Update.m_Format = IGraphics::ETextureFormat::R8_UNORM;
	std::unique_ptr<uint8_t, decltype(&free)> pFirstData(static_cast<uint8_t *>(malloc(4)), free);
	Update.m_pData = pFirstData.get();
	ASSERT_NE(Update.m_pData, nullptr);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Update));
	ASSERT_EQ(pFirstData.release(), Update.m_pData);
	std::unique_ptr<uint8_t, decltype(&free)> pSecondData(static_cast<uint8_t *>(malloc(4)), free);
	Update.m_pData = pSecondData.get();
	ASSERT_NE(Update.m_pData, nullptr);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Update));
	ASSERT_EQ(pSecondData.release(), Update.m_pData);
	CCommandBuffer::SCommand_CreateBufferObject Create;
	Create.m_DeletePointer = true;
	Create.m_Desc.m_Size = 4;
	std::unique_ptr<void, decltype(&free)> pCreateData(malloc(Create.m_Desc.m_Size), free);
	Create.m_pUploadData = pCreateData.get();
	ASSERT_NE(Create.m_pUploadData, nullptr);
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Create));
	ASSERT_EQ(pCreateData.release(), Create.m_pUploadData);
	ASSERT_EQ(Buffer.m_ExternalDataSize, 12u);

	auto *pFirst = static_cast<CCommandBuffer::SCommand_Texture_Update *>(Buffer.Head());
	CCommandBuffer::FreeExternalData(pFirst);
	// In-flight accounting stays unchanged until the queue recycles the whole buffer.
	EXPECT_EQ(Buffer.m_ExternalDataSize, 12u);
	EXPECT_EQ(pFirst->m_pData, nullptr);
	auto *pSecond = static_cast<CCommandBuffer::SCommand_Texture_Update *>(pFirst->m_pNext);
	EXPECT_NE(pSecond->m_pData, nullptr);
	auto *pCreate = static_cast<CCommandBuffer::SCommand_CreateBufferObject *>(pSecond->m_pNext);
	EXPECT_NE(pCreate->m_pUploadData, nullptr);

	Buffer.FreeExternalDataFrom(pSecond);
	EXPECT_EQ(Buffer.m_ExternalDataSize, 12u);
	EXPECT_EQ(pSecond->m_pData, nullptr);
	EXPECT_EQ(pCreate->m_pUploadData, nullptr);

	Buffer.FreeExternalData();
	EXPECT_EQ(Buffer.m_ExternalDataSize, 0u);
}

TEST(GraphicsThreaded, SubmissionSerialsPreserveFrameResourceDependencies)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CCommandBuffer::CSubmissionTracker Tracker;

	const auto Resource = Tracker.Prepare(EChannel::RELIABLE, true, false);
	EXPECT_EQ(Resource.m_SubmissionSerial, 1u);
	EXPECT_EQ(Resource.m_ResourceSerial, 1u);

	const auto Control = Tracker.Prepare(EChannel::RELIABLE, false, false);
	EXPECT_EQ(Control.m_SubmissionSerial, 2u);
	EXPECT_EQ(Control.m_ResourceSerial, 1u);

	const auto FramePacket = Tracker.Prepare(EChannel::FRAME, false, true);
	EXPECT_EQ(FramePacket.m_SubmissionSerial, 3u);
	EXPECT_EQ(FramePacket.m_FrameSerial, 1u);
	EXPECT_EQ(FramePacket.m_RequiredResourceSerial, 1u);
	EXPECT_TRUE(FramePacket.m_EndsFrame);

	// Deferred destroys are post-frame ordering, not a resource prerequisite for
	// the next packet. Their retired handle slots cannot be reused yet.
	const auto DeferredDestroy = Tracker.Prepare(EChannel::RELIABLE, false, false);
	EXPECT_EQ(DeferredDestroy.m_ResourceSerial, 1u);

	Tracker.FinishFrame();
	const auto LaterResource = Tracker.Prepare(EChannel::RELIABLE, true, false);
	EXPECT_EQ(LaterResource.m_ResourceSerial, 2u);
	const auto NextFrame = Tracker.Prepare(EChannel::FRAME, false, true);
	EXPECT_EQ(NextFrame.m_FrameSerial, 2u);
	EXPECT_EQ(NextFrame.m_RequiredResourceSerial, 2u);
}

TEST(GraphicsThreaded, OnlyResourceDestroysUseTheDeferredChannel)
{
	EXPECT_TRUE(CCommandBuffer::IsDeferredDestroyCommand(CCommandBuffer::CMD_TEXTURE_DESTROY));
	EXPECT_TRUE(CCommandBuffer::IsDeferredDestroyCommand(CCommandBuffer::CMD_DELETE_BUFFER_OBJECT));
	EXPECT_FALSE(CCommandBuffer::IsDeferredDestroyCommand(CCommandBuffer::CMD_TEXTURE_CREATE));
	EXPECT_FALSE(CCommandBuffer::IsDeferredDestroyCommand(CCommandBuffer::CMD_DRAW));
	EXPECT_FALSE(CCommandBuffer::IsDeferredDestroyCommand(CCommandBuffer::CMD_SWAP));
}

TEST(GraphicsThreaded, OnlyCompleteNormalFramesAreMailboxReplaceable)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CCommandBuffer Buffer(1024, 1024);
	CCommandBuffer::SSubmissionInfo Submission;
	Submission.m_Channel = EChannel::FRAME;
	Submission.m_EndsFrame = true;
	Submission.m_Replaceable = true;
	Buffer.SetSubmissionInfo(Submission);
	EXPECT_TRUE(Buffer.IsReplaceableFramePacket());

	// A frame that ends but is not presented - one that went into a target, or
	// one drawn without a surface - has no newer frame that could show instead.
	Submission.m_Replaceable = false;
	Buffer.SetSubmissionInfo(Submission);
	EXPECT_FALSE(Buffer.IsReplaceableFramePacket());
	Submission.m_Replaceable = true;

	Submission.m_EndsFrame = false;
	Buffer.SetSubmissionInfo(Submission);
	EXPECT_FALSE(Buffer.IsReplaceableFramePacket());

	Submission.m_Channel = EChannel::RELIABLE;
	Submission.m_EndsFrame = true;
	Buffer.SetSubmissionInfo(Submission);
	EXPECT_FALSE(Buffer.IsReplaceableFramePacket());

	Buffer.Reset();
	Submission.m_Channel = EChannel::FRAME;
	Buffer.SetSubmissionInfo(Submission);
	CCommandBuffer::CCompletion Completion;
	CCommandBuffer::SCommand_Swap Swap;
	Swap.m_pCompletion = &Completion;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Swap));
	EXPECT_FALSE(Buffer.IsReplaceableFramePacket());

	Buffer.Reset();
	Buffer.SetSubmissionInfo(Submission);
	CCommandBuffer::SImageReadbackResult ReadbackResult;
	CCommandBuffer::SCommand_PresentationTarget_Readback Readback;
	Readback.m_pResult = &ReadbackResult;
	Readback.m_pCompletion = &ReadbackResult;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Readback));
	EXPECT_FALSE(Buffer.IsReplaceableFramePacket());
}

TEST(GraphicsThreaded, PresentationReadbackCompletionIsIdempotent)
{
	CCommandBuffer Buffer(1024, 1024);
	CCommandBuffer::SImageReadbackResult Result;
	CCommandBuffer::SCommand_PresentationTarget_Readback Readback;
	Readback.m_pResult = &Result;
	Readback.m_pCompletion = &Result;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(Readback));
	EXPECT_FALSE(Result.IsComplete());
	Buffer.SignalCompletions();
	EXPECT_TRUE(Result.IsComplete());
	Result.Wait();
	Result.Wait();
}

TEST(GraphicsThreaded, BufferSignalsOwnedCompletions)
{
	CCommandBuffer Buffer(1024, 1024);
	CCommandBuffer::SCommand_VSync::SResult Result;
	CCommandBuffer::SCommand_VSync VSync;
	VSync.m_pResult = &Result;
	VSync.m_pCompletion = &Result;
	ASSERT_TRUE(Buffer.AddCommandUnsafe(VSync));

	EXPECT_FALSE(Result.IsComplete());
	Buffer.SignalCompletions();
	EXPECT_TRUE(Result.IsComplete());
	Result.Wait();
}

namespace
{
	void SetSubmission(CCommandBuffer &Buffer, CCommandBuffer::ECommandChannel Channel, uint64_t Serial, bool EndsFrame = false)
	{
		CCommandBuffer::SSubmissionInfo Submission;
		Submission.m_Channel = Channel;
		Submission.m_SubmissionSerial = Serial;
		Submission.m_EndsFrame = EndsFrame;
		// These check the mailbox, which only ever sees presented frames.
		Submission.m_Replaceable = EndsFrame;
		Buffer.SetSubmissionInfo(Submission);
	}
}

TEST(GraphicsThreaded, RenderCommandQueueDoesNotStarveFrameBehindReliableWork)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CRenderCommandQueue Queue;
	Queue.Start();
	CCommandBuffer Reliable1(1024, 1024);
	CCommandBuffer Frame1(1024, 1024);
	CCommandBuffer Reliable2(1024, 1024);
	CCommandBuffer Frame2(1024, 1024);
	SetSubmission(Reliable1, EChannel::RELIABLE, 1);
	SetSubmission(Frame1, EChannel::FRAME, 2, true);
	SetSubmission(Reliable2, EChannel::RELIABLE, 3);
	SetSubmission(Frame2, EChannel::FRAME, 4, true);
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable1));
	ASSERT_EQ(Queue.EnqueueFrame(&Frame1), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable2));
	ASSERT_EQ(Queue.EnqueueFrame(&Frame2), CRenderCommandQueue::EFrameEnqueueResult::DROPPED);

	for(const uint64_t ExpectedSerial : {1u, 2u, 3u})
	{
		CRenderCommandQueue::SEntry Entry;
		ASSERT_TRUE(Queue.WaitDequeue(Entry));
		EXPECT_EQ(Entry.m_pBuffer->SubmissionInfo().m_SubmissionSerial, ExpectedSerial);
		Queue.Recycle(std::move(Entry), true);
	}
	EXPECT_TRUE(Queue.IsIdle());
	const auto Stats = Queue.GetFrameMailboxStats();
	EXPECT_EQ(Stats.m_Produced, 2u);
	EXPECT_EQ(Stats.m_Rendered, 1u);
	EXPECT_EQ(Stats.m_Dropped, 1u);
	Queue.Stop();
}

TEST(GraphicsThreaded, RenderCommandQueueReplacesFrameWhileRenderIsBlocked)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CRenderCommandQueue Queue;
	Queue.Start();
	CCommandBuffer Frame1(1024, 1024);
	CCommandBuffer Frame2(1024, 1024);
	CCommandBuffer Frame3(1024, 1024);
	SetSubmission(Frame1, EChannel::FRAME, 1, true);
	SetSubmission(Frame2, EChannel::FRAME, 2, true);
	SetSubmission(Frame3, EChannel::FRAME, 3, true);
	ASSERT_EQ(Queue.EnqueueFrame(&Frame1), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);

	CSemaphore ProcessingStarted;
	CSemaphore FinishProcessing;
	std::thread Worker([&] {
		CRenderCommandQueue::SEntry Entry;
		if(Queue.WaitDequeue(Entry))
		{
			ProcessingStarted.Signal();
			FinishProcessing.Wait();
			Queue.Recycle(std::move(Entry), true);
		}
	});
	ProcessingStarted.Wait();
	const auto Frame2Result = Queue.EnqueueFrame(&Frame2);
	const auto Frame3Result = Queue.EnqueueFrame(&Frame3);
	FinishProcessing.Signal();
	Worker.join();
	ASSERT_EQ(Frame2Result, CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	ASSERT_EQ(Frame3Result, CRenderCommandQueue::EFrameEnqueueResult::QUEUED);

	CRenderCommandQueue::SEntry Latest;
	ASSERT_TRUE(Queue.WaitDequeue(Latest));
	EXPECT_EQ(Latest.m_pBuffer->SubmissionInfo().m_SubmissionSerial, 3u);
	Queue.Recycle(std::move(Latest), true);
	const auto Stats = Queue.GetFrameMailboxStats();
	EXPECT_EQ(Stats.m_Produced, 3u);
	EXPECT_EQ(Stats.m_Rendered, 2u);
	EXPECT_EQ(Stats.m_Dropped, 1u);
	Queue.Stop();
}

TEST(GraphicsThreaded, RenderCommandQueueStopWakesWaitingConsumer)
{
	CRenderCommandQueue Queue;
	Queue.Start();
	CSemaphore ConsumerStarted;
	bool Dequeued = true;
	std::thread Worker([&] {
		CRenderCommandQueue::SEntry Entry;
		ConsumerStarted.Signal();
		Dequeued = Queue.WaitDequeue(Entry);
	});
	ConsumerStarted.Wait();
	Queue.Stop();
	Worker.join();
	EXPECT_FALSE(Dequeued);
}

TEST(GraphicsThreaded, RenderCommandQueueWaitsOnlyForReliableCapacity)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CRenderCommandQueue Queue;
	Queue.Start();
	CCommandBuffer Reliable1(1024, 1024);
	CCommandBuffer Reliable2(1024, 1024);
	CCommandBuffer Reliable3(1024, 1024);
	CCommandBuffer Reliable4(1024, 1024);
	SetSubmission(Reliable1, EChannel::RELIABLE, 1);
	SetSubmission(Reliable2, EChannel::RELIABLE, 2);
	SetSubmission(Reliable3, EChannel::RELIABLE, 3);
	SetSubmission(Reliable4, EChannel::RELIABLE, 4);
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable1));
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable2));
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable3));
	EXPECT_FALSE(Queue.EnqueueReliable(&Reliable4));

	CSemaphore ProducerStarted;
	bool Enqueued = false;
	std::thread Producer([&] {
		ProducerStarted.Signal();
		Enqueued = Queue.WaitEnqueueReliable(&Reliable4);
	});
	ProducerStarted.Wait();
	CRenderCommandQueue::SEntry First;
	ASSERT_TRUE(Queue.WaitDequeue(First));
	EXPECT_EQ(First.m_pBuffer->SubmissionInfo().m_SubmissionSerial, 1u);
	Queue.Recycle(std::move(First), true);
	Producer.join();
	ASSERT_TRUE(Enqueued);
	EXPECT_FALSE(Queue.IsIdle());

	for(const uint64_t ExpectedSerial : {2u, 3u, 4u})
	{
		CRenderCommandQueue::SEntry Entry;
		ASSERT_TRUE(Queue.WaitDequeue(Entry));
		EXPECT_EQ(Entry.m_pBuffer->SubmissionInfo().m_SubmissionSerial, ExpectedSerial);
		Queue.Recycle(std::move(Entry), true);
	}
	EXPECT_TRUE(Queue.IsIdle());
	Queue.Stop();
}

TEST(GraphicsThreaded, RenderCommandQueueWaitsOnlyForPinnedFrameCapacity)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CRenderCommandQueue Queue;
	Queue.Start();
	CCommandBuffer Frame1(1024, 1024);
	CCommandBuffer Frame2(1024, 1024);
	CCommandBuffer Frame3(1024, 1024);
	CCommandBuffer Frame4(1024, 1024);
	SetSubmission(Frame1, EChannel::FRAME, 1);
	SetSubmission(Frame2, EChannel::FRAME, 2);
	SetSubmission(Frame3, EChannel::FRAME, 3);
	SetSubmission(Frame4, EChannel::FRAME, 4);
	ASSERT_EQ(Queue.EnqueueFrame(&Frame1), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	ASSERT_EQ(Queue.EnqueueFrame(&Frame2), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	ASSERT_EQ(Queue.EnqueueFrame(&Frame3), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	EXPECT_EQ(Queue.EnqueueFrame(&Frame4), CRenderCommandQueue::EFrameEnqueueResult::RETRY);

	CSemaphore ProducerStarted;
	bool Enqueued = false;
	std::thread Producer([&] {
		ProducerStarted.Signal();
		Enqueued = Queue.WaitEnqueuePinnedFrame(&Frame4);
	});
	ProducerStarted.Wait();
	CRenderCommandQueue::SEntry First;
	ASSERT_TRUE(Queue.WaitDequeue(First));
	EXPECT_EQ(First.m_pBuffer->SubmissionInfo().m_SubmissionSerial, 1u);
	Queue.Recycle(std::move(First), true);
	Producer.join();
	ASSERT_TRUE(Enqueued);
	EXPECT_FALSE(Queue.IsIdle());

	for(const uint64_t ExpectedSerial : {2u, 3u, 4u})
	{
		CRenderCommandQueue::SEntry Entry;
		ASSERT_TRUE(Queue.WaitDequeue(Entry));
		EXPECT_EQ(Entry.m_pBuffer->SubmissionInfo().m_SubmissionSerial, ExpectedSerial);
		Queue.Recycle(std::move(Entry), true);
	}
	const auto Stats = Queue.GetFrameMailboxStats();
	EXPECT_EQ(Stats.m_Produced, 4u);
	EXPECT_EQ(Stats.m_Rendered, 4u);
	EXPECT_EQ(Stats.m_Dropped, 0u);
	EXPECT_TRUE(Queue.IsIdle());
	Queue.Stop();
}

TEST(GraphicsThreaded, RenderCommandQueueStopWakesWaitingProducers)
{
	using EChannel = CCommandBuffer::ECommandChannel;
	CRenderCommandQueue Queue;
	Queue.Start();
	CCommandBuffer Reliable1(1024, 1024);
	CCommandBuffer Reliable2(1024, 1024);
	CCommandBuffer Reliable3(1024, 1024);
	CCommandBuffer Reliable4(1024, 1024);
	CCommandBuffer Frame1(1024, 1024);
	CCommandBuffer Frame2(1024, 1024);
	CCommandBuffer Frame3(1024, 1024);
	CCommandBuffer Frame4(1024, 1024);
	SetSubmission(Reliable1, EChannel::RELIABLE, 1);
	SetSubmission(Reliable2, EChannel::RELIABLE, 2);
	SetSubmission(Reliable3, EChannel::RELIABLE, 3);
	SetSubmission(Reliable4, EChannel::RELIABLE, 4);
	SetSubmission(Frame1, EChannel::FRAME, 5);
	SetSubmission(Frame2, EChannel::FRAME, 6);
	SetSubmission(Frame3, EChannel::FRAME, 7);
	SetSubmission(Frame4, EChannel::FRAME, 8);
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable1));
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable2));
	ASSERT_TRUE(Queue.EnqueueReliable(&Reliable3));
	ASSERT_EQ(Queue.EnqueueFrame(&Frame1), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	ASSERT_EQ(Queue.EnqueueFrame(&Frame2), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);
	ASSERT_EQ(Queue.EnqueueFrame(&Frame3), CRenderCommandQueue::EFrameEnqueueResult::QUEUED);

	CSemaphore ReliableProducerStarted;
	CSemaphore FrameProducerStarted;
	bool ReliableEnqueued = true;
	bool FrameEnqueued = true;
	std::thread ReliableProducer([&] {
		ReliableProducerStarted.Signal();
		ReliableEnqueued = Queue.WaitEnqueueReliable(&Reliable4);
	});
	std::thread FrameProducer([&] {
		FrameProducerStarted.Signal();
		FrameEnqueued = Queue.WaitEnqueuePinnedFrame(&Frame4);
	});
	ReliableProducerStarted.Wait();
	FrameProducerStarted.Wait();
	Queue.Stop();
	ReliableProducer.join();
	FrameProducer.join();
	EXPECT_FALSE(ReliableEnqueued);
	EXPECT_FALSE(FrameEnqueued);

	CRenderCommandQueue::SEntry Entry;
	while(Queue.WaitDequeue(Entry))
		Queue.Recycle(std::move(Entry), false);
}

// The strides in this table are baked into every Vulkan and WebGPU pipeline
// that is built from it. A change here that nobody meant is a pipeline reading
// the wrong number of bytes per vertex, which is invisible until it is looked
// at, so the values are pinned.
TEST(GraphicsThreaded, VertexLayoutTableIsPinned)
{
	using EType = IGraphics::EVertexAttributeType;
	using EMode = IGraphics::EVertexAttributeMode;
	using EVertexLayout = IGraphics::EVertexLayout;

	struct SExpectedAttribute
	{
		uint32_t m_ComponentCount;
		EType m_Type;
		bool m_Normalized;
		size_t m_Offset;
		EMode m_Mode;
	};
	struct SExpectedLayout
	{
		EVertexLayout m_Layout;
		size_t m_Stride;
		std::vector<SExpectedAttribute> m_vAttributes;
	};

	const std::vector<SExpectedLayout> vExpected = {
		{EVertexLayout::TILE, 8, {{2, EType::FLOAT32, false, 0, EMode::FLOAT}}},
		{EVertexLayout::TILE_TEXTURED, 12, {{2, EType::FLOAT32, false, 0, EMode::FLOAT}, {4, EType::UINT8, false, 8, EMode::INTEGER}}},
		{EVertexLayout::QUAD, 20, {{4, EType::FLOAT32, false, 0, EMode::FLOAT}, {4, EType::UINT8, true, 16, EMode::FLOAT}}},
		{EVertexLayout::QUAD_TEXTURED, 28, {{4, EType::FLOAT32, false, 0, EMode::FLOAT}, {4, EType::UINT8, true, 16, EMode::FLOAT}, {2, EType::FLOAT32, false, 20, EMode::FLOAT}}},
		{EVertexLayout::POSITION_TEXCOORD_COLOR, 20, {{2, EType::FLOAT32, false, 0, EMode::FLOAT}, {2, EType::FLOAT32, false, 8, EMode::FLOAT}, {4, EType::UINT8, true, 16, EMode::FLOAT}}},
	};
	ASSERT_EQ(vExpected.size(), (size_t)EVertexLayout::COUNT);

	for(const SExpectedLayout &Expected : vExpected)
	{
		const IGraphics::SVertexLayoutDesc &Desc = IGraphics::VertexLayout(Expected.m_Layout);
		EXPECT_EQ(Desc.m_Stride, Expected.m_Stride) << "stride of layout " << (int)Expected.m_Layout;
		ASSERT_EQ(Desc.m_AttributeCount, Expected.m_vAttributes.size()) << "attribute count of layout " << (int)Expected.m_Layout;
		for(size_t Index = 0; Index < Expected.m_vAttributes.size(); ++Index)
		{
			const IGraphics::CVertexAttributeDesc &Attribute = Desc.m_aAttributes[Index];
			const SExpectedAttribute &ExpectedAttribute = Expected.m_vAttributes[Index];
			EXPECT_EQ(Attribute.m_ComponentCount, ExpectedAttribute.m_ComponentCount);
			EXPECT_EQ((int)Attribute.m_Type, (int)ExpectedAttribute.m_Type);
			EXPECT_EQ(Attribute.m_Normalized, ExpectedAttribute.m_Normalized);
			EXPECT_EQ(Attribute.m_Offset, ExpectedAttribute.m_Offset);
			EXPECT_EQ((int)Attribute.m_Mode, (int)ExpectedAttribute.m_Mode);
		}
		// Every attribute has to fit inside the stride it is read with.
		for(uint32_t Index = 0; Index < Desc.m_AttributeCount; ++Index)
			EXPECT_LE(Desc.m_aAttributes[Index].m_Offset, Desc.m_Stride) << "attribute " << Index << " of layout " << (int)Expected.m_Layout;
	}
}

// A frame that needs more commands than the arena holds used to be dropped
// whole, CMD_SWAP included, which left the last frame that still fit on the
// screen until the view got cheaper again. Commands are a linked list, so the
// arena grows in chunks the way the data arena already did.
TEST(GraphicsThreaded, CommandArenaGrowsInsteadOfDroppingTheFrame)
{
	// Room for a handful of commands, so the growth path is reached quickly.
	CCommandBuffer Buffer(sizeof(CCommandBuffer::SCommand_Clear) * 4, 1024);

	constexpr int NumCommands = 500;
	for(int i = 0; i < NumCommands; ++i)
	{
		CCommandBuffer::SCommand_Clear Command;
		Command.m_Color = ColorRGBA(i / 1000.0f, 0.0f, 0.0f, 1.0f);
		ASSERT_TRUE(Buffer.AddCommandUnsafe(Command)) << "command " << i;
	}

	int Seen = 0;
	for(const CCommandBuffer::SCommand *pCommand = Buffer.Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
	{
		ASSERT_EQ(pCommand->m_Cmd, (unsigned)CCommandBuffer::CMD_CLEAR);
		const auto *pClear = static_cast<const CCommandBuffer::SCommand_Clear *>(pCommand);
		EXPECT_FLOAT_EQ(pClear->m_Color.r, Seen / 1000.0f) << "command " << Seen;
		++Seen;
	}
	EXPECT_EQ(Seen, NumCommands);

	// The chunks are kept and reused instead of reallocated per frame.
	Buffer.Reset();
	EXPECT_EQ(Buffer.Head(), nullptr);
	for(int i = 0; i < NumCommands; ++i)
	{
		CCommandBuffer::SCommand_Clear Command;
		ASSERT_TRUE(Buffer.AddCommandUnsafe(Command)) << "command " << i << " after reset";
	}
}
