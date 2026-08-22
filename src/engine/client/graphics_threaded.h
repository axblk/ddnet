#ifndef ENGINE_CLIENT_GRAPHICS_THREADED_H
#define ENGINE_CLIENT_GRAPHICS_THREADED_H

#include <base/dbg.h>
#include <base/sphore.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

constexpr int CMD_BUFFER_DATA_BUFFER_SIZE = 1024 * 1024 * 2;
constexpr int CMD_BUFFER_CMD_BUFFER_SIZE = 1024 * 256;
constexpr size_t RELIABLE_QUEUE_MAX_EXTERNAL_DATA_SIZE = 64 * 1024 * 1024;
constexpr size_t GPU_INFO_STRING_SIZE = 256;

struct SGfxErrorContainer;
struct SGfxWarningContainer;

enum class EPrimitiveType
{
	LINES,
	QUADS,
	TRIANGLES,
};

[[nodiscard]] constexpr uint32_t VerticesPerPrimitive(EPrimitiveType PrimitiveType)
{
	switch(PrimitiveType)
	{
	case EPrimitiveType::LINES: return 2;
	case EPrimitiveType::QUADS: return 4;
	case EPrimitiveType::TRIANGLES: return 3;
	}
	return 0;
}

enum class EBlendMode
{
	NONE,
	ALPHA,
	ADDITIVE,
};

enum class EWrapMode
{
	REPEAT,
	CLAMP,
};

// The small built-in shader program catalog used to create pipeline resources.
// Texture, blend and clip variants remain part of SState during the migration.
enum class EPipelineProgram : uint8_t
{
	PRIMITIVE,
	PRIMITIVE_TEXTURE_ARRAY,
	PRIMITIVE_UNIFORM_COLOR,
	PRIMITIVE_INSTANCED,
	QUAD_PER_ITEM,
	QUAD_SHARED,
	ARRAY_COLOR,
	ARRAY_COLOR_TRANSFORM,
	DUAL_ATLAS_COMPOSITE,
	BLUR,
	PLANAR_YUV,
	COUNT,
};

class CCommandBuffer
{
	class CBuffer
	{
		unsigned char *m_pData;
		unsigned m_Size;
		unsigned m_Used;

	public:
		CBuffer(unsigned BufferSize)
		{
			m_Size = BufferSize;
			m_pData = new unsigned char[m_Size];
			m_Used = 0;
		}
		CBuffer(const CBuffer &) = delete;
		CBuffer &operator=(const CBuffer &) = delete;

		~CBuffer()
		{
			delete[] m_pData;
			m_pData = nullptr;
			m_Used = 0;
			m_Size = 0;
		}

		void Reset()
		{
			m_Used = 0;
		}

		void *Alloc(unsigned Requested, unsigned Alignment = alignof(std::max_align_t))
		{
			size_t Offset = reinterpret_cast<uintptr_t>(m_pData + m_Used) % Alignment;
			if(Offset)
				Offset = Alignment - Offset;

			if(Requested + Offset + m_Used > m_Size)
				return nullptr;

			void *pPtr = &m_pData[m_Used + Offset];
			m_Used += Requested + Offset;
			return pPtr;
		}

		unsigned char *DataPtr() { return m_pData; }

		void Swap(CBuffer &Other) noexcept
		{
			std::swap(m_pData, Other.m_pData);
			std::swap(m_Size, Other.m_Size);
			std::swap(m_Used, Other.m_Used);
		}
	};

public:
	CBuffer m_CmdBuffer;
	size_t m_ExternalDataSize = 0;

	CBuffer m_DataBuffer;

	enum
	{
		MAX_TEXTURES = 1024 * 8,
		MAX_VERTICES = 32 * 1024,
	};

	struct STextureBindingHandleTag;
	using CTextureBindingHandle = CGenerationHandle<STextureBindingHandleTag>;
	struct STextureBindingDesc
	{
		std::array<IGraphics::CTextureHandle, 2> m_aTextures;
	};
	struct SPipelineHandleTag;
	using CPipelineHandle = CGenerationHandle<SPipelineHandleTag>;
	struct SPipelineDesc
	{
		EPipelineProgram m_Program = EPipelineProgram::PRIMITIVE;
	};

	enum
	{
		// command groups
		CMDGROUP_CORE = 0, // commands that everyone has to implement
		CMDGROUP_RENDERER = 10000, // commands specific to a renderer backend
		CMDGROUP_PLATFORM_SDL = 20000,

		CMD_FIRST = CMDGROUP_CORE,
	};

	enum ECommandBufferCMD
	{
		// synchronization
		CMD_SIGNAL = CMD_FIRST,

		// texture commands
		CMD_TEXTURE_CREATE,
		CMD_TEXTURE_DESTROY,
		CMD_TEXTURE_BINDING_CREATE,
		CMD_TEXTURE_BINDING_DESTROY,
		CMD_TEXTURE_UPDATE,
		CMD_TEXTURE_READBACK,
		CMD_PIPELINE_CREATE,
		CMD_PIPELINE_DESTROY,

		// rendering
		CMD_CLEAR,
		CMD_BEGIN_RENDER_PASS,
		CMD_END_RENDER_PASS,
		CMD_FLUSH_RENDER_PASS,
		CMD_DRAW, // generic draw with frame-owned transient vertex data

		// opengl 2.0+ commands (some are just emulated and only exist in opengl 3.3+)
		CMD_CREATE_BUFFER_OBJECT, // create vbo
		CMD_RECREATE_BUFFER_OBJECT, // recreate vbo
		CMD_UPDATE_BUFFER_OBJECT, // update vbo
		CMD_COPY_BUFFER_OBJECT, // copy vbo to another
		CMD_DELETE_BUFFER_OBJECT, // delete vbo

		CMD_CREATE_BUFFER_CONTAINER, // create vao
		CMD_DELETE_BUFFER_CONTAINER, // delete vao
		CMD_UPDATE_BUFFER_CONTAINER, // update vao

		CMD_DRAW_INDEXED, // generic indexed draw through the transitional buffer-container adapter

		// swap
		CMD_SWAP,

		// misc
		CMD_MULTISAMPLING,
		CMD_VSYNC,
		CMD_PRESENTATION_TARGET_READBACK,
		CMD_UPDATE_VIEWPORT,

		// in Android a window that minimizes gets destroyed
		CMD_WINDOW_CREATE_NTF,
		CMD_WINDOW_DESTROY_NTF,

		CMD_COUNT,
	};

	enum class ECommandChannel
	{
		FRAME,
		RELIABLE,
	};

	struct SSubmissionInfo
	{
		ECommandChannel m_Channel = ECommandChannel::RELIABLE;
		uint64_t m_SubmissionSerial = 0;
		uint64_t m_FrameSerial = 0;
		uint64_t m_ResourceSerial = 0;
		uint64_t m_RequiredResourceSerial = 0;
		bool m_EndsFrame = false;
	};

	class CSubmissionTracker
	{
		uint64_t m_NextSubmissionSerial = 1;
		uint64_t m_CurrentFrameSerial = 1;
		uint64_t m_CurrentResourceSerial = 0;

	public:
		SSubmissionInfo Prepare(ECommandChannel Channel, bool HasResourceCommands, bool EndsFrame)
		{
			SSubmissionInfo Info;
			Info.m_Channel = Channel;
			Info.m_SubmissionSerial = m_NextSubmissionSerial++;
			if(Channel == ECommandChannel::FRAME)
			{
				Info.m_FrameSerial = m_CurrentFrameSerial;
				Info.m_RequiredResourceSerial = m_CurrentResourceSerial;
				Info.m_EndsFrame = EndsFrame;
			}
			else
			{
				if(HasResourceCommands)
					++m_CurrentResourceSerial;
				Info.m_ResourceSerial = m_CurrentResourceSerial;
			}
			return Info;
		}

		void FinishFrame() { ++m_CurrentFrameSerial; }
	};

	/**
	 * Tells the frontend that the render thread is done with a command.
	 *
	 * The waiting side usually destroys the object as soon as it sees the
	 * completion, so the signalling side must be finished touching it by then.
	 * The lock provides that: a waiter can only leave while the signal holds no
	 * lock anymore, which is after it published the completion.
	 */
	class CCompletion
	{
		mutable std::mutex m_Mutex;
		std::condition_variable m_Condition;
		bool m_Completed = false;

	public:
		void Wait()
		{
			std::unique_lock<std::mutex> Lock(m_Mutex);
			m_Condition.wait(Lock, [this]() { return m_Completed; });
		}
		[[nodiscard]] bool IsComplete() const
		{
			const std::unique_lock<std::mutex> Lock(m_Mutex);
			return m_Completed;
		}
		void Signal()
		{
			const std::unique_lock<std::mutex> Lock(m_Mutex);
			m_Completed = true;
			m_Condition.notify_all();
		}
	};

	static constexpr ECommandChannel CommandChannel(unsigned Command)
	{
		switch(Command)
		{
		case CMD_CLEAR:
		case CMD_BEGIN_RENDER_PASS:
		case CMD_END_RENDER_PASS:
		case CMD_FLUSH_RENDER_PASS:
		case CMD_DRAW:
		case CMD_DRAW_INDEXED:
		case CMD_PRESENTATION_TARGET_READBACK:
		case CMD_SWAP:
			return ECommandChannel::FRAME;
		default:
			return ECommandChannel::RELIABLE;
		}
	}

	static constexpr bool IsResourceCommand(unsigned Command)
	{
		switch(Command)
		{
		case CMD_TEXTURE_CREATE:
		case CMD_TEXTURE_DESTROY:
		case CMD_TEXTURE_BINDING_CREATE:
		case CMD_TEXTURE_BINDING_DESTROY:
		case CMD_TEXTURE_UPDATE:
		case CMD_PIPELINE_CREATE:
		case CMD_PIPELINE_DESTROY:
		case CMD_CREATE_BUFFER_OBJECT:
		case CMD_RECREATE_BUFFER_OBJECT:
		case CMD_UPDATE_BUFFER_OBJECT:
		case CMD_COPY_BUFFER_OBJECT:
		case CMD_DELETE_BUFFER_OBJECT:
		case CMD_CREATE_BUFFER_CONTAINER:
		case CMD_DELETE_BUFFER_CONTAINER:
		case CMD_UPDATE_BUFFER_CONTAINER:
			return true;
		default:
			return false;
		}
	}

	static constexpr bool UsesReservedReliableBudget(unsigned Command)
	{
		switch(Command)
		{
		case CMD_SIGNAL:
		case CMD_TEXTURE_DESTROY:
		case CMD_TEXTURE_BINDING_DESTROY:
		case CMD_PIPELINE_DESTROY:
		case CMD_DELETE_BUFFER_OBJECT:
		case CMD_DELETE_BUFFER_CONTAINER:
		case CMD_WINDOW_CREATE_NTF:
		case CMD_WINDOW_DESTROY_NTF:
			return true;
		default:
			return Command >= CMDGROUP_RENDERER;
		}
	}

	using SPoint = vec2;
	using STexCoord = vec2;
	using SColorf = ColorRGBA;
	using SColor = SGraphicsColor;
	using SVertex = SGraphicsVertex;
	using SVertexTex3D = SGraphicsVertexTex3D;
	using SVertexTex3DStream = SGraphicsVertexTex3DStream;

	struct SCommand
	{
	public:
		SCommand(unsigned Cmd) :
			m_Cmd(Cmd), m_pNext(nullptr), m_pCompletion(nullptr) {}
		unsigned m_Cmd;
		SCommand *m_pNext;
		CCompletion *m_pCompletion;
	};

	struct SImageReadbackResult : public CCompletion
	{
		CImageInfo m_Image;
		bool m_Ok = false;
	};

	struct SState
	{
		EBlendMode m_BlendMode;
		EWrapMode m_WrapMode;
		IGraphics::CTextureHandle m_Texture;
		SPoint m_ScreenTL;
		SPoint m_ScreenBR;

		// clip
		bool m_ClipEnable;
		int m_ClipX;
		int m_ClipY;
		int m_ClipW;
		int m_ClipH;
	};

	struct SCommand_Clear : public SCommand
	{
		SCommand_Clear() :
			SCommand(CMD_CLEAR) {}
		SColorf m_Color;
		bool m_ForceClear;
	};

	struct SCommand_BeginRenderPass : public SCommand
	{
		SCommand_BeginRenderPass() :
			SCommand(CMD_BEGIN_RENDER_PASS) {}
		IGraphics::CRenderPassDesc m_Desc;
	};

	struct SCommand_EndRenderPass : public SCommand
	{
		SCommand_EndRenderPass() :
			SCommand(CMD_END_RENDER_PASS) {}
	};

	struct SCommand_FlushRenderPass : public SCommand
	{
		SCommand_FlushRenderPass() :
			SCommand(CMD_FLUSH_RENDER_PASS) {}
	};

	struct SCommand_Signal : public SCommand
	{
		SCommand_Signal() :
			SCommand(CMD_SIGNAL) {}
		CSemaphore *m_pSemaphore;

		void Signal() const { m_pSemaphore->Signal(); }
	};

	struct SCommand_CreateBufferObject : public SCommand
	{
		SCommand_CreateBufferObject() :
			SCommand(CMD_CREATE_BUFFER_OBJECT), m_DeletePointer(false), m_pUploadData(nullptr) {}

		IGraphics::CBufferHandle m_Buffer;
		IGraphics::CBufferDesc m_Desc;

		bool m_DeletePointer;
		void *m_pUploadData;
	};

	struct SCommand_RecreateBufferObject : public SCommand
	{
		SCommand_RecreateBufferObject() :
			SCommand(CMD_RECREATE_BUFFER_OBJECT), m_DeletePointer(false), m_pUploadData(nullptr) {}

		IGraphics::CBufferHandle m_Buffer;
		IGraphics::CBufferDesc m_Desc;

		bool m_DeletePointer;
		void *m_pUploadData;
	};

	struct SCommand_UpdateBufferObject : public SCommand
	{
		SCommand_UpdateBufferObject() :
			SCommand(CMD_UPDATE_BUFFER_OBJECT), m_DeletePointer(false), m_pUploadData(nullptr), m_DataSize(0), m_Offset(0) {}

		IGraphics::CBufferHandle m_Buffer;

		bool m_DeletePointer;
		void *m_pUploadData;
		size_t m_DataSize;
		size_t m_Offset;
	};

	struct SCommand_CopyBufferObject : public SCommand
	{
		SCommand_CopyBufferObject() :
			SCommand(CMD_COPY_BUFFER_OBJECT) {}

		IGraphics::CBufferHandle m_WriteBuffer;
		IGraphics::CBufferHandle m_ReadBuffer;

		size_t m_ReadOffset;
		size_t m_WriteOffset;
		size_t m_CopySize;
	};

	struct SCommand_DeleteBufferObject : public SCommand
	{
		SCommand_DeleteBufferObject() :
			SCommand(CMD_DELETE_BUFFER_OBJECT) {}

		IGraphics::CBufferHandle m_Buffer;
	};

	struct SCommand_CreateBufferContainer : public SCommand
	{
		SCommand_CreateBufferContainer() :
			SCommand(CMD_CREATE_BUFFER_CONTAINER) {}

		IGraphics::CBufferContainerHandle m_BufferContainer;

		size_t m_Stride;
		IGraphics::CBufferHandle m_VertBufferBinding;

		size_t m_AttrCount;
		SBufferContainerInfo::SAttribute *m_pAttributes;
	};

	struct SCommand_UpdateBufferContainer : public SCommand
	{
		SCommand_UpdateBufferContainer() :
			SCommand(CMD_UPDATE_BUFFER_CONTAINER) {}

		IGraphics::CBufferContainerHandle m_BufferContainer;

		size_t m_Stride;
		IGraphics::CBufferHandle m_VertBufferBinding;

		size_t m_AttrCount;
		SBufferContainerInfo::SAttribute *m_pAttributes;
	};

	struct SCommand_DeleteBufferContainer : public SCommand
	{
		SCommand_DeleteBufferContainer() :
			SCommand(CMD_DELETE_BUFFER_CONTAINER) {}

		IGraphics::CBufferContainerHandle m_BufferContainer;
		bool m_DestroyAllBO;
	};

	static constexpr size_t MAX_DRAW_DATA_SIZE = 128;

	struct SDrawDataPrimitiveUniformColor
	{
		float m_Rotation;
		vec2 m_RotationCenter;
		ColorRGBA m_Color;
	};
	static_assert(sizeof(SDrawDataPrimitiveUniformColor) <= MAX_DRAW_DATA_SIZE);

	struct SDrawDataPrimitiveInstanced
	{
		vec2 m_RotationCenter;
		ColorRGBA m_Color;
	};
	static_assert(sizeof(SDrawDataPrimitiveInstanced) <= MAX_DRAW_DATA_SIZE);

	struct SInstanceDataPositionScaleRotation
	{
		vec2 m_Position;
		float m_Scale;
		float m_Rotation;
	};
	static_assert(sizeof(SInstanceDataPositionScaleRotation) == sizeof(float) * 4);

	struct SDrawDataQuadTransform
	{
		ColorRGBA m_Color;
		vec2 m_Offset;
		float m_Rotation;
		float m_Padding;
	};
	static_assert(sizeof(SDrawDataQuadTransform) == sizeof(float) * 8);

	struct SDrawDataArrayColor
	{
		ColorRGBA m_Color;
	};
	static_assert(sizeof(SDrawDataArrayColor) <= MAX_DRAW_DATA_SIZE);

	struct SDrawDataArrayColorTransform
	{
		ColorRGBA m_Color;
		vec2 m_Offset;
		vec2 m_Scale;
	};
	static_assert(sizeof(SDrawDataArrayColorTransform) <= MAX_DRAW_DATA_SIZE);

	struct SDrawDataDualAtlas
	{
		float m_TextureSize;
		ColorRGBA m_PrimaryColor;
		ColorRGBA m_SecondaryColor;
	};
	static_assert(sizeof(SDrawDataDualAtlas) <= MAX_DRAW_DATA_SIZE);

	struct SDrawData
	{
		const void *m_pData = nullptr;
		size_t m_Size = 0;

		template<typename T>
		[[nodiscard]] const T *Get() const
		{
			static_assert(sizeof(T) <= MAX_DRAW_DATA_SIZE);
			static_assert(std::is_trivially_copyable_v<T>);
			return m_pData != nullptr && m_Size == sizeof(T) ? static_cast<const T *>(m_pData) : nullptr;
		}
	};

	struct SArrayData
	{
		const void *m_pData = nullptr;
		size_t m_Size = 0;

		template<typename T>
		[[nodiscard]] const T *Get(size_t ElementCount) const
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return m_pData != nullptr && m_Size % sizeof(T) == 0 && m_Size / sizeof(T) == ElementCount ? static_cast<const T *>(m_pData) : nullptr;
		}
	};

	struct SCommand_Draw : public SCommand
	{
		SCommand_Draw() :
			SCommand(CMD_DRAW) {}
		SState m_State;
		CPipelineHandle m_Pipeline;
		EPrimitiveType m_PrimitiveType = EPrimitiveType::TRIANGLES;
		IGraphics::CBufferHandle m_IndexBuffer;
		SArrayData m_VertexData;
		uint32_t m_VertexCount = 0;

		[[nodiscard]] bool SamplesTexture(IGraphics::CTextureHandle Texture) const { return m_State.m_Texture == Texture; }
	};

	struct SCommand_DrawIndexed : public SCommand
	{
		struct SIndexedDrawRange
		{
			SState m_State;
			uint32_t m_FirstIndex = 0;
			uint32_t m_IndexCount = 0;
			uint32_t m_VertexOffset = 0;
		};

		SCommand_DrawIndexed() :
			SCommand(CMD_DRAW_INDEXED) {}
		SState m_State;

		IGraphics::CBufferContainerHandle m_BufferContainer;
		IGraphics::CBufferHandle m_IndexBuffer;
		CTextureBindingHandle m_TextureBinding;
		CPipelineHandle m_Pipeline;
		SDrawData m_DrawData;
		SArrayData m_ArrayData;

		uint32_t m_IndexCount = 0;
		size_t m_IndexOffset = 0;
		uint32_t m_InstanceCount = 1;

		IGraphics::EIndexType m_IndexType = IGraphics::EIndexType::UINT32;
		SArrayData m_VertexData;
		SArrayData m_IndexData;
		SArrayData m_RangeData;
		uint32_t m_VertexCount = 0;
		uint32_t m_RangeCount = 0;

		[[nodiscard]] bool IsTransient() const { return m_RangeCount != 0; }
		[[nodiscard]] bool SamplesTexture(IGraphics::CTextureHandle Texture) const
		{
			if(m_State.m_Texture == Texture)
				return true;
			const auto *pRanges = m_RangeData.Get<SIndexedDrawRange>(m_RangeCount);
			if(!IsTransient() || pRanges == nullptr)
				return false;
			for(uint32_t i = 0; i < m_RangeCount; ++i)
			{
				if(pRanges[i].m_State.m_Texture == Texture)
					return true;
			}
			return false;
		}
		[[nodiscard]] bool ValidateTransient() const
		{
			if(!IsTransient() || m_BufferContainer.IsValid() || m_IndexBuffer.IsValid() || m_TextureBinding.IsValid() || !m_Pipeline.IsValid() || m_IndexOffset != 0 || m_InstanceCount != 1 || m_DrawData.m_pData != nullptr || m_DrawData.m_Size != 0 || m_ArrayData.m_pData != nullptr || m_ArrayData.m_Size != 0 || m_VertexCount == 0 || m_IndexCount == 0)
				return false;
			const auto *pVertices = m_VertexData.Get<SVertex>(m_VertexCount);
			const auto *pRanges = m_RangeData.Get<SIndexedDrawRange>(m_RangeCount);
			if(pVertices == nullptr || pRanges == nullptr)
				return false;
			auto ValidateIndices = [&](const auto *pIndices) {
				if(pIndices == nullptr)
					return false;
				for(uint32_t RangeIndex = 0; RangeIndex < m_RangeCount; ++RangeIndex)
				{
					const auto &Range = pRanges[RangeIndex];
					if(Range.m_IndexCount == 0 || Range.m_FirstIndex > m_IndexCount || Range.m_IndexCount > m_IndexCount - Range.m_FirstIndex || Range.m_VertexOffset > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) || Range.m_IndexCount > static_cast<uint32_t>(std::numeric_limits<int>::max()))
						return false;
					for(uint32_t Index = 0; Index < Range.m_IndexCount; ++Index)
					{
						if(static_cast<uint64_t>(Range.m_VertexOffset) + pIndices[Range.m_FirstIndex + Index] >= m_VertexCount)
							return false;
					}
				}
				return true;
			};
			switch(m_IndexType)
			{
			case IGraphics::EIndexType::UINT16: return ValidateIndices(m_IndexData.Get<uint16_t>(m_IndexCount));
			case IGraphics::EIndexType::UINT32: return ValidateIndices(m_IndexData.Get<uint32_t>(m_IndexCount));
			}
			return false;
		}
	};

	struct SCommand_PresentationTarget_Readback : public SCommand
	{
		SCommand_PresentationTarget_Readback() :
			SCommand(CMD_PRESENTATION_TARGET_READBACK) {}
		bool m_ReadPixel = false;
		ivec2 m_Position;
		SImageReadbackResult *m_pResult;
	};

	struct SCommand_Swap : public SCommand
	{
		SCommand_Swap() :
			SCommand(CMD_SWAP) {}
	};

	struct SCommand_VSync : public SCommand
	{
		struct SResult : public CCompletion
		{
			bool m_Ok = false;
		};

		SCommand_VSync() :
			SCommand(CMD_VSYNC) {}

		int m_VSync;
		SResult *m_pResult;
	};

	struct SCommand_MultiSampling : public SCommand
	{
		struct SResult : public CCompletion
		{
			uint32_t m_MultiSamplingCount = 0;
			bool m_Ok = false;
		};

		SCommand_MultiSampling() :
			SCommand(CMD_MULTISAMPLING) {}

		uint32_t m_RequestedMultiSamplingCount;
		SResult *m_pResult;
	};

	struct SCommand_Update_Viewport : public SCommand
	{
		SCommand_Update_Viewport() :
			SCommand(CMD_UPDATE_VIEWPORT) {}

		int m_X;
		int m_Y;
		int m_Width;
		int m_Height;
		int m_SurfaceWidth;
		int m_SurfaceHeight;
		bool m_ByResize; // resized by an resize event.. a hint to make clear that the viewport update can be deferred if wanted
	};

	struct SCommand_Texture_Create : public SCommand
	{
		SCommand_Texture_Create() :
			SCommand(CMD_TEXTURE_CREATE), m_pData(nullptr) {}

		// texture information
		IGraphics::CTextureHandle m_Texture;
		IGraphics::CTextureDesc m_Desc;
		// Data must match the descriptor format.
		uint8_t *m_pData; // will be freed by the command processor
	};

	struct SCommand_Texture_Destroy : public SCommand
	{
		SCommand_Texture_Destroy() :
			SCommand(CMD_TEXTURE_DESTROY) {}

		// texture information
		IGraphics::CTextureHandle m_Texture;
	};

	struct SCommand_TextureBinding_Create : public SCommand
	{
		SCommand_TextureBinding_Create() :
			SCommand(CMD_TEXTURE_BINDING_CREATE) {}

		CTextureBindingHandle m_Binding;
		STextureBindingDesc m_Desc;
	};

	struct SCommand_TextureBinding_Destroy : public SCommand
	{
		SCommand_TextureBinding_Destroy() :
			SCommand(CMD_TEXTURE_BINDING_DESTROY) {}

		CTextureBindingHandle m_Binding;
	};

	struct SCommand_Pipeline_Create : public SCommand
	{
		SCommand_Pipeline_Create() :
			SCommand(CMD_PIPELINE_CREATE) {}

		CPipelineHandle m_Pipeline;
		SPipelineDesc m_Desc;
	};

	struct SCommand_Pipeline_Destroy : public SCommand
	{
		SCommand_Pipeline_Destroy() :
			SCommand(CMD_PIPELINE_DESTROY) {}

		CPipelineHandle m_Pipeline;
	};

	struct SCommand_Texture_Update : public SCommand
	{
		SCommand_Texture_Update() :
			SCommand(CMD_TEXTURE_UPDATE), m_pData(nullptr) {}

		IGraphics::CTextureHandle m_Texture;
		IGraphics::CTextureRegion m_Region;
		IGraphics::ETextureFormat m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
		uint8_t *m_pData; // will be freed by the command processor
	};

	struct SCommand_Texture_Readback : public SCommand
	{
		SCommand_Texture_Readback() :
			SCommand(CMD_TEXTURE_READBACK) {}
		IGraphics::CTextureHandle m_Texture;
		SImageReadbackResult *m_pResult;
	};

	struct SCommand_WindowCreateNtf : public CCommandBuffer::SCommand
	{
		SCommand_WindowCreateNtf() :
			SCommand(CMD_WINDOW_CREATE_NTF) {}

		uint32_t m_WindowId;
	};

	struct SCommand_WindowDestroyNtf : public CCommandBuffer::SCommand
	{
		SCommand_WindowDestroyNtf() :
			SCommand(CMD_WINDOW_DESTROY_NTF) {}

		uint32_t m_WindowId;
	};

	//
	CCommandBuffer(unsigned CmdBufferSize, unsigned DataBufferSize, size_t MaxExternalDataSize = std::numeric_limits<size_t>::max()) :
		m_CmdBuffer(CmdBufferSize), m_DataBuffer(DataBufferSize), m_MaxExternalDataSize(MaxExternalDataSize), m_pCmdBufferHead(nullptr), m_pCmdBufferTail(nullptr)
	{
	}

	static constexpr bool IsDeferredDestroyCommand(unsigned Command)
	{
		switch(Command)
		{
		case CMD_TEXTURE_DESTROY:
		case CMD_TEXTURE_BINDING_DESTROY:
		case CMD_PIPELINE_DESTROY:
		case CMD_DELETE_BUFFER_OBJECT:
		case CMD_DELETE_BUFFER_CONTAINER:
			return true;
		default:
			return false;
		}
	}
	CCommandBuffer(const CCommandBuffer &) = delete;
	CCommandBuffer &operator=(const CCommandBuffer &) = delete;

	void *AllocData(unsigned WantedSize)
	{
		return m_DataBuffer.Alloc(WantedSize);
	}

	template<class T>
	bool AddCommandUnsafe(const T &Command)
	{
		// make sure that we don't do something stupid like ->AddCommand(&Cmd);
		(void)static_cast<const SCommand *>(&Command);

		const size_t CommandExternalDataSize = ExternalDataSize(Command);
		if(CommandExternalDataSize > m_MaxExternalDataSize - m_ExternalDataSize)
			return false;

		// allocate and copy the command into the buffer
		T *pCmd = (T *)m_CmdBuffer.Alloc(sizeof(*pCmd), alignof(T));
		if(!pCmd)
			return false;
		*pCmd = Command;
		pCmd->m_pNext = nullptr;

		if(m_pCmdBufferTail)
			m_pCmdBufferTail->m_pNext = pCmd;
		if(!m_pCmdBufferHead)
			m_pCmdBufferHead = pCmd;
		m_pCmdBufferTail = pCmd;

		m_ExternalDataSize += CommandExternalDataSize;

		return true;
	}

	const SCommand *Head() const { return m_pCmdBufferHead; }
	SCommand *Head() { return m_pCmdBufferHead; }
	bool IsEmpty() const { return m_pCmdBufferHead == nullptr; }
	bool ContainsCommand(unsigned Command) const
	{
		for(const SCommand *pCommand = Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
		{
			if(pCommand->m_Cmd == Command)
				return true;
		}
		return false;
	}
	bool ContainsResourceCommands() const
	{
		for(const SCommand *pCommand = Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
		{
			if(IsResourceCommand(pCommand->m_Cmd))
				return true;
		}
		return false;
	}
	bool IsReplaceableFramePacket() const
	{
		return m_SubmissionInfo.m_Channel == ECommandChannel::FRAME && m_SubmissionInfo.m_EndsFrame && !ContainsCompletions();
	}
	bool UsesReservedReliableBudget() const
	{
		for(const SCommand *pCommand = Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
		{
			if(CCommandBuffer::UsesReservedReliableBudget(pCommand->m_Cmd))
				return true;
		}
		return false;
	}
	void SignalCompletions() const
	{
		for(const SCommand *pCommand = Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
		{
			if(pCommand->m_pCompletion != nullptr)
				pCommand->m_pCompletion->Signal();
		}
	}
	void Swap(CCommandBuffer &Other) noexcept
	{
		m_CmdBuffer.Swap(Other.m_CmdBuffer);
		m_DataBuffer.Swap(Other.m_DataBuffer);
		std::swap(m_ExternalDataSize, Other.m_ExternalDataSize);
		std::swap(m_SubmissionInfo, Other.m_SubmissionInfo);
		std::swap(m_pCmdBufferHead, Other.m_pCmdBufferHead);
		std::swap(m_pCmdBufferTail, Other.m_pCmdBufferTail);
	}
	const SSubmissionInfo &SubmissionInfo() const { return m_SubmissionInfo; }
	void SetSubmissionInfo(const SSubmissionInfo &Info) { m_SubmissionInfo = Info; }

	void Reset()
	{
		m_pCmdBufferHead = m_pCmdBufferTail = nullptr;
		m_CmdBuffer.Reset();
		m_DataBuffer.Reset();

		m_ExternalDataSize = 0;
		m_SubmissionInfo = {};
	}
	bool ContainsCompletions() const
	{
		for(const SCommand *pCommand = Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
		{
			if(pCommand->m_pCompletion != nullptr)
				return true;
		}
		return false;
	}

private:
	static size_t ImageDataSize(size_t Width, size_t Height, size_t PixelSize)
	{
		if(Width != 0 && (PixelSize > std::numeric_limits<size_t>::max() / Width || Height > std::numeric_limits<size_t>::max() / (Width * PixelSize)))
			return std::numeric_limits<size_t>::max();
		return Width * Height * PixelSize;
	}
	template<class T>
	static size_t ExternalDataSize(const T &Command)
	{
		if constexpr(std::is_same_v<T, SCommand_Texture_Create>)
		{
			if(Command.m_pData == nullptr)
				return 0;
			const size_t PixelSize = Command.m_Desc.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? 4 : 1;
			return ImageDataSize(Command.m_Desc.m_Width, Command.m_Desc.m_Height, PixelSize);
		}
		else if constexpr(std::is_same_v<T, SCommand_Texture_Update>)
		{
			if(Command.m_pData == nullptr)
				return 0;
			const size_t PixelSize = Command.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? 4 : 1;
			return ImageDataSize(Command.m_Region.m_Width, Command.m_Region.m_Height, PixelSize);
		}
		else if constexpr(std::is_same_v<T, SCommand_CreateBufferObject> || std::is_same_v<T, SCommand_RecreateBufferObject>)
		{
			return Command.m_DeletePointer && Command.m_pUploadData != nullptr ? Command.m_Desc.m_Size : 0;
		}
		else if constexpr(std::is_same_v<T, SCommand_UpdateBufferObject>)
		{
			return Command.m_DeletePointer && Command.m_pUploadData != nullptr ? Command.m_DataSize : 0;
		}
		else
			return 0;
	}

public:
	static void FreeExternalData(SCommand *pCommand)
	{
		switch(pCommand->m_Cmd)
		{
		case CMD_TEXTURE_CREATE:
		{
			auto *pTyped = static_cast<SCommand_Texture_Create *>(pCommand);
			free(pTyped->m_pData);
			pTyped->m_pData = nullptr;
			break;
		}
		case CMD_TEXTURE_UPDATE:
		{
			auto *pTyped = static_cast<SCommand_Texture_Update *>(pCommand);
			free(pTyped->m_pData);
			pTyped->m_pData = nullptr;
			break;
		}
		case CMD_CREATE_BUFFER_OBJECT:
		{
			auto *pTyped = static_cast<SCommand_CreateBufferObject *>(pCommand);
			if(pTyped->m_DeletePointer)
				free(pTyped->m_pUploadData);
			pTyped->m_pUploadData = nullptr;
			break;
		}
		case CMD_RECREATE_BUFFER_OBJECT:
		{
			auto *pTyped = static_cast<SCommand_RecreateBufferObject *>(pCommand);
			if(pTyped->m_DeletePointer)
				free(pTyped->m_pUploadData);
			pTyped->m_pUploadData = nullptr;
			break;
		}
		case CMD_UPDATE_BUFFER_OBJECT:
		{
			auto *pTyped = static_cast<SCommand_UpdateBufferObject *>(pCommand);
			if(pTyped->m_DeletePointer)
				free(pTyped->m_pUploadData);
			pTyped->m_pUploadData = nullptr;
			break;
		}
		default: break;
		}
	}

	void FreeExternalDataFrom(SCommand *pCommand)
	{
		for(; pCommand != nullptr; pCommand = pCommand->m_pNext)
			FreeExternalData(pCommand);
	}

	void FreeExternalData()
	{
		FreeExternalDataFrom(Head());
		m_ExternalDataSize = 0;
	}

private:
	size_t m_MaxExternalDataSize;
	SSubmissionInfo m_SubmissionInfo;
	SCommand *m_pCmdBufferHead;
	SCommand *m_pCmdBufferTail;
};

enum EGraphicsBackendErrorCodes
{
	GRAPHICS_BACKEND_ERROR_CODE_NONE = 0,
	GRAPHICS_BACKEND_ERROR_CODE_CONTEXT_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_VERSION_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_INTERFACE_INIT_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SDL_INIT_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SDL_SCREEN_REQUEST_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SDL_SCREEN_INFO_REQUEST_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SDL_SCREEN_RESOLUTION_REQUEST_FAILED,
	GRAPHICS_BACKEND_ERROR_CODE_SDL_WINDOW_CREATE_FAILED,
};

// Technical renderer features discovered by the backend.
struct SBackendCapabilities
{
	bool m_ArrayColorPipelines = false;
	bool m_QuadPipelines = false;
	bool m_DualAtlasPipeline = false;
	bool m_BufferedPrimitivePipelines = false;

	bool m_MipMapping = false;
	bool m_NPOTTextures = false;
	bool m_3DTextures = false;
	bool m_2DArrayTextures = false;
	bool m_ShaderSupport = false;
	bool m_RenderTargets = false;
	// The backend can turn a rendered frame into the planar YUV layout an
	// encoder takes, so that a frame crosses the bus at one and a half
	// bytes per pixel instead of four, and already converted.
	bool m_PlanarYuvConversion = false;

	bool m_TrianglesAsQuads = false;

	int m_ContextMajor = 0;
	int m_ContextMinor = 0;
	int m_ContextPatch = 0;
};

// Effective renderer features exposed to the graphics frontend.
struct CRenderCapabilities
{
	bool m_TileBuffering = false;
	bool m_QuadBuffering = false;
	bool m_TextBuffering = false;
	bool m_QuadContainerBuffering = false;
	bool m_TextureArrays = false;
	bool m_2DTextureArrays = false;
	bool m_QuadToTriangleConversion = false;
	bool m_RenderTargets = false;
	bool m_PlanarYuvConversion = false;
};

// interface for the graphics backend
// all these functions are called on the main thread
class IGraphicsBackend
{
public:
	using SFrameMailboxStats = IGraphics::SFrameMailboxStats;

	enum
	{
		INITFLAG_FULLSCREEN = 1 << 0,
		INITFLAG_VSYNC = 1 << 1,
		INITFLAG_RESIZABLE = 1 << 2,
		INITFLAG_BORDERLESS = 1 << 3,
		INITFLAG_DESKTOP_FULLSCREEN = 1 << 4,
		INITFLAG_HIDDEN = 1 << 5,
	};

	virtual ~IGraphicsBackend() = default;

	virtual int Init(const char *pName, int *pScreen, int *pWidth, int *pHeight, int *pRefreshRate, int *pFsaaSamples, int Flags, int *pDesktopWidth, int *pDesktopHeight, int *pCurrentWidth, int *pCurrentHeight, class IStorage *pStorage) = 0;
	virtual int Shutdown() = 0;

	virtual uint64_t TextureMemoryUsage() const = 0;
	virtual uint64_t BufferMemoryUsage() const = 0;
	virtual uint64_t StreamedMemoryUsage() const = 0;
	virtual uint64_t StagingMemoryUsage() const = 0;

	virtual const TTwGraphicsGpuList &GetGpus() const = 0;

	virtual void GetVideoModes(CVideoMode *pModes, int MaxModes, int *pNumModes, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int Screen) = 0;
	virtual void GetCurrentVideoMode(CVideoMode &CurMode, float HiDPIScale, int MaxWindowWidth, int MaxWindowHeight, int Screen) = 0;

	virtual int GetNumScreens() const = 0;
	virtual const char *GetScreenName(int Screen) const = 0;

	virtual void Minimize() = 0;
	virtual void SetWindowParams(int FullscreenMode, bool IsBorderless) = 0;
	virtual bool SetWindowScreen(int Index, bool MoveToCenter, ivec2 *pDesktopSize) = 0;
	virtual bool UpdateDisplayMode(int Index, ivec2 *pDesktopSize) = 0;
	virtual int GetWindowScreen() = 0;
	virtual int WindowActive() = 0;
	virtual int WindowOpen() = 0;
	virtual void SetWindowGrab(bool Grab) = 0;
	// returns true, if the video mode changed
	virtual bool ResizeWindow(int w, int h, int RefreshRate) = 0;
	virtual void GetViewportSize(int &w, int &h) = 0;
	virtual void NotifyWindow() = 0;
	virtual bool IsScreenKeyboardShown() = 0;

	virtual void WindowDestroyNtf(uint32_t WindowId) = 0;
	virtual void WindowCreateNtf(uint32_t WindowId) = 0;

	virtual void RunBuffer(CCommandBuffer *pBuffer) = 0;
	// Transfers reliable payload storage into a bounded backend-owned queue.
	virtual bool RunBufferQueued(CCommandBuffer *pBuffer, bool WaitForCapacity = false) = 0;
	// Publishes one complete immutable frame. The buffer is empty on success.
	virtual bool RunFramePacket(CCommandBuffer *pBuffer, bool WaitForCapacity = false) = 0;
	virtual SFrameMailboxStats GetFrameMailboxStats() const = 0;
	virtual void RunBufferSingleThreadedUnsafe(CCommandBuffer *pBuffer) = 0;
	virtual bool IsIdle() const = 0;
	virtual void WaitForIdle() = 0;

	virtual bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) = 0;
	// checks if the current values of the config are a graphics modern API
	virtual bool IsConfigModernAPI() { return false; }
	virtual SBackendCapabilities GetCapabilities() const = 0;
	virtual const char *GetErrorString() { return nullptr; }

	virtual const char *GetVendorString() = 0;
	virtual const char *GetVersionString() = 0;
	virtual const char *GetRendererString() = 0;

	virtual const SGfxErrorContainer &GetError() const = 0;
	virtual bool GetWarning(SGfxWarningContainer &Warning) = 0;

	/**
	 * @see IGraphics::ShowMessageBox
	 */
	virtual std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) = 0;
};

class CGraphics_Threaded : public IEngineGraphics
{
	enum class EDrawing
	{
		NONE,
		QUADS,
		LINES,
		TRIANGLES,
	};

	CCommandBuffer::SState m_State;
	IGraphicsBackend *m_pBackend;
	CRenderCapabilities m_Capabilities;
	mutable std::string m_FatalError;

	CCommandBuffer *m_apCommandBuffers[2];
	CCommandBuffer *m_pCommandBuffer;
	unsigned m_CurrentCommandBuffer;
	CCommandBuffer *m_apReliableCommandBuffers[2];
	CCommandBuffer *m_pReliableCommandBuffer;
	unsigned m_CurrentReliableCommandBuffer;
	CCommandBuffer *m_pDeferredDestroyCommandBuffer;
	bool m_DropCurrentFrame;
	CCommandBuffer::CSubmissionTracker m_SubmissionTracker;
	std::vector<CTextureHandle> m_vRetiredTextureHandles;
	struct STextureInfo
	{
		CTextureHandle m_Handle;
		CTextureDesc m_Desc;
	};
	std::vector<STextureInfo> m_vTextureInfos;
	struct STextureBindingInfo
	{
		CCommandBuffer::STextureBindingDesc m_Desc;
		CCommandBuffer::CTextureBindingHandle m_Binding;
	};
	std::vector<STextureBindingInfo> m_vTextureBindingInfos;
	CGenerationHandlePool<CCommandBuffer::CTextureBindingHandle> m_TextureBindingHandles;
	std::vector<CCommandBuffer::CTextureBindingHandle> m_vRetiredTextureBindingHandles;
	CGenerationHandlePool<CCommandBuffer::CPipelineHandle> m_PipelineHandles;
	std::array<CCommandBuffer::CPipelineHandle, static_cast<size_t>(EPipelineProgram::COUNT)> m_aPipelines;
	std::vector<CCommandBuffer::CPipelineHandle> m_vRetiredPipelineHandles;
	std::vector<CBufferHandle> m_vRetiredBufferHandles;
	std::vector<CBufferContainerHandle> m_vRetiredBufferContainerHandles;

	//
	class IStorage *m_pStorage;
	class IEngine *m_pEngine;

	int m_CurIndex;

	CCommandBuffer::SVertex m_aVertices[CCommandBuffer::MAX_VERTICES];
	CCommandBuffer::SVertexTex3DStream m_aVerticesTex3D[CCommandBuffer::MAX_VERTICES];
	int m_NumVertices;

	CCommandBuffer::SColor m_aColor[4];
	CCommandBuffer::STexCoord m_aTexture[4];

	bool m_RenderEnable;
	bool m_RenderPassActive = true;
	CTextureHandle m_RenderPassTarget;
	CTextureHandle m_OffscreenFrameTarget;

	float m_Rotation;
	EDrawing m_Drawing;
	bool m_DoScreenshot;
	char m_aScreenshotName[IO_MAX_PATH_LENGTH];

	CTextureHandle m_NullTexture;

	CGenerationHandlePool<CTextureHandle> m_TextureHandles;
	int m_TextureMemoryUsage;

	std::atomic<bool> m_WarnPngliteIncompatibleImages = false;

	std::mutex m_WarningsMutex;
	std::vector<SWarning> m_vWarnings;

	// is a non full windowed (in a sense that the viewport won't include the whole window),
	// forced viewport, so that it justifies our UI ratio needs
	bool m_IsForcedViewport = false;

	struct SVertexArrayInfo
	{
		// keep a reference to it, so we can free the ID
		CBufferHandle m_AssociatedBuffer;
	};
	std::vector<SVertexArrayInfo> m_vVertexArrayInfo;
	CGenerationHandlePool<CBufferContainerHandle> m_BufferContainerHandles;
	CGenerationHandlePool<CBufferHandle> m_BufferHandles;
	CBufferHandle m_QuadIndexBuffer;
	unsigned int m_QuadIndexCount = 0;

	struct SQuadContainer
	{
		SQuadContainer(bool AutomaticUpload = true)
		{
			m_vQuads.clear();
			m_QuadBuffer.Invalidate();
			m_QuadBufferContainer.Invalidate();
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
		CBufferContainerHandle m_QuadBufferContainer;
		size_t m_UploadedQuadCount;

		int m_FreeIndex;

		bool m_AutomaticUpload;
	};
	std::vector<SQuadContainer> m_vQuadContainers;
	int m_FirstFreeQuadContainer;

	std::vector<WINDOW_RESIZE_FUNC> m_vResizeListeners;
	std::vector<WINDOW_PROPS_CHANGED_FUNC> m_vPropChangeListeners;

	void *AllocCommandBufferData(size_t AllocSize);
	void *AllocReliableCommandBufferData(size_t AllocSize);
	CCommandBuffer::CTextureBindingHandle CreateTextureBinding(CTextureHandle PrimaryTexture, CTextureHandle SecondaryTexture);
	bool DeleteTextureBinding(CTextureHandle PrimaryTexture, CTextureHandle SecondaryTexture);
	CCommandBuffer::CTextureBindingHandle FindTextureBinding(CTextureHandle PrimaryTexture, CTextureHandle SecondaryTexture) const;
	void CreatePipelines();
	bool DestroyPipelines();
	CCommandBuffer::CPipelineHandle Pipeline(EPipelineProgram Program) const { return m_aPipelines[static_cast<size_t>(Program)]; }
	CBufferHandle CreateBufferObjectInternal(size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer, EBufferUsage Usage);
	bool RecreateBufferObjectInternal(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer, EBufferUsage Usage);
	bool UpdateTextureInternal(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, uint8_t *pData, bool IsMovedPointer);
	bool DrawFullscreenTexture(CTextureHandle Source, CCommandBuffer::CPipelineHandle Pipeline, SGraphicsColor Color, uint8_t RequiredUsage, bool UseCurrentClip = false);
	void UpdateViewportInternal(int X, int Y, int W, int H, bool ByResize, int SurfaceW, int SurfaceH);

	void AddVertices(int Count);
	void AddVertices(int Count, CCommandBuffer::SVertex *pVertices);
	void AddVertices(int Count, CCommandBuffer::SVertexTex3DStream *pVertices);

	template<typename TName>
	void Rotate(const CCommandBuffer::SPoint &Center, TName *pPoints, int NumPoints)
	{
		float c = std::cos(m_Rotation);
		float s = std::sin(m_Rotation);
		float x, y;
		int i;

		TName *pVertices = pPoints;
		for(i = 0; i < NumPoints; i++)
		{
			x = pVertices[i].m_Pos.x - Center.x;
			y = pVertices[i].m_Pos.y - Center.y;
			pVertices[i].m_Pos.x = x * c - y * s + Center.x;
			pVertices[i].m_Pos.y = x * s + y * c + Center.y;
		}
	}

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
			return false;
		if(pCommandBuffer->AddCommandUnsafe(Cmd))
			return true;
		if(CCommandBuffer::CommandChannel(Cmd.m_Cmd) == CCommandBuffer::ECommandChannel::FRAME)
		{
			DropCurrentFrame();
			if(!FailFunc())
				return false;
			return pCommandBuffer->AddCommandUnsafe(Cmd);
		}
		if(pCommandBuffer == m_pDeferredDestroyCommandBuffer)
			return false;

		if(!SubmitReliableCommandBuffer(pCommandBuffer))
			return false;
		pCommandBuffer = GetCommandBuffer(Cmd.m_Cmd);
		if(pCommandBuffer == nullptr)
			return false;

		if(!FailFunc())
			return false;
		return pCommandBuffer->AddCommandUnsafe(Cmd);
	}

	template<typename TName>
	bool AddCmd(TName &Cmd)
	{
		return AddCmd(Cmd, [] { return true; });
	}

	template<typename TName>
	bool AddCmdBlocking(TName &Cmd)
	{
		dbg_assert(CCommandBuffer::CommandChannel(Cmd.m_Cmd) == CCommandBuffer::ECommandChannel::RELIABLE, "graphics: blocking command used the frame channel");
		if(AddCmd(Cmd))
			return true;
		// Commands with caller-owned completion state are synchronous by contract.
		if(!SubmitReliableCommandBuffer(m_pReliableCommandBuffer))
			return false;
		return AddCmd(Cmd);
	}

	CCommandBuffer *GetCommandBuffer(unsigned Command);
	bool SubmitReliableCommandBuffer(CCommandBuffer *pCommandBuffer);
	bool SubmitFramePacket();
	bool SubmitDeferredDestroys();
	void RecycleRetiredHandles();
	void DropCurrentFrame();
	void CollectBackendQueueWarnings();
	bool KickCommandBuffer();

	void AddBackEndWarningIfExists();

	void AdjustViewport(bool SendViewportChangeToBackend);

	ivec2 m_ReadPixelPosition = ivec2(0, 0);
	ColorRGBA *m_pReadPixelColor = nullptr;
	std::unique_ptr<ITextureReadback> PresentFrame(bool Readback, CImageInfo &&Recycled = CImageInfo());

	int IssueInit();
	int InitWindow();
	EGraphicsBackendMode m_BackendMode;
	bool m_HiddenWindow;

public:
	CGraphics_Threaded(EGraphicsBackendMode BackendMode, bool HiddenWindow);

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

	const TTwGraphicsGpuList &GetGpus() const override;

	void MapScreen(const CScreenRect &ScreenRect) override;
	CScreenRect GetScreen() const override;

	void LinesBegin() override;
	void LinesEnd() override;
	void LinesDraw(const CLineItem *pArray, size_t Num) override;

	void LinesBatchBegin(CLineItemBatch *pBatch) override;
	void LinesBatchEnd(CLineItemBatch *pBatch) override;
	void LinesBatchDraw(CLineItemBatch *pBatch, const CLineItem *pArray, size_t Num) override;

	IGraphics::CTextureHandle FindFreeTextureIndex();
	void FreeTextureIndex(CTextureHandle *pIndex);
	bool IsTextureLayeringSupported(ETextureLayering Layering) const;
	void StoreTextureInfo(CTextureHandle Texture, const CTextureDesc &Desc);
	void UnloadTexture(IGraphics::CTextureHandle *pIndex) override;
	void LoadTextureAddWarning(const CTextureDesc &Desc, const char *pTexName);
	IGraphics::CTextureHandle LoadTextureRaw(const CImageInfo &Image, int Flags, const char *pTexName = nullptr) override;
	IGraphics::CTextureHandle LoadTextureRawMove(CImageInfo &Image, int Flags, const char *pTexName = nullptr) override;
	IGraphics::CTextureHandle CreateTexture(const CTextureDesc &Desc, const void *pInitialData = nullptr) override;
	std::unique_ptr<ITextureReadback> ReadTextureAsync(CTextureHandle Texture, CImageInfo &&Recycled = CImageInfo()) override;
	bool BeginOffscreenFrame(CTextureHandle Texture) override;
	std::unique_ptr<ITextureReadback> EndOffscreenFrame(CImageInfo &&Recycled = CImageInfo()) override;
	std::unique_ptr<ITextureReadback> PresentAndReadbackAsync(CImageInfo &&Recycled = CImageInfo()) override;
	bool PlanarYuvConversionSupported() const override { return m_Capabilities.m_PlanarYuvConversion; }
	bool ConvertTextureToPlanarYuv(CTextureHandle Source, EPlanarYuvFormat Format) override;
	bool UpdateTexture(CTextureHandle Texture, const CTextureRegion &Region, ETextureFormat Format, const void *pData) override;

	bool LoadTextTextures(size_t Width, size_t Height, CTextureHandle &TextTexture, CTextureHandle &TextOutlineTexture, uint8_t *pTextData, uint8_t *pTextOutlineData) override;
	bool UnloadTextTextures(CTextureHandle &TextTexture, CTextureHandle &TextOutlineTexture) override;
	bool UpdateTextTexture(CTextureHandle TextureId, int x, int y, size_t Width, size_t Height, uint8_t *pData, bool IsMovedPointer) override;

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
	bool BlurTexture(CTextureHandle Source, EBlurDirection Direction) override;

	void QuadsBegin() override;
	void QuadsEnd() override;
	void QuadsTex3DBegin() override;
	void QuadsTex3DEnd() override;
	void TrianglesBegin() override;
	void TrianglesEnd() override;
	void QuadsEndKeepVertices() override;
	void QuadsDrawCurrentVertices(bool KeepVertices = true) override;
	void QuadsSetRotation(float Angle) override;

	template<typename TName>
	void SetColor(TName *pVertex, int ColorIndex)
	{
		TName *pVert = pVertex;
		pVert->m_Color = m_aColor[ColorIndex];
	}

	void SetColorVertex(const CColorVertex *pArray, size_t Num) override;
	void SetColor(float r, float g, float b, float a) override;
	void SetColor(ColorRGBA Color) override;
	void SetColor4(ColorRGBA TopLeft, ColorRGBA TopRight, ColorRGBA BottomLeft, ColorRGBA BottomRight) override;

	// go through all vertices and change their color (only works for quads)
	void ChangeColorOfCurrentQuadVertices(float r, float g, float b, float a) override;
	void ChangeColorOfQuadVertices(size_t QuadOffset, unsigned char r, unsigned char g, unsigned char b, unsigned char a) override;

	void QuadsSetSubset(float TlU, float TlV, float BrU, float BrV) override;
	void QuadsSetSubsetFree(
		float x0, float y0, float x1, float y1,
		float x2, float y2, float x3, float y3, int Index = -1) override;

	void QuadsDraw(CQuadItem *pArray, int Num) override;

	template<typename TName>
	void QuadsDrawTLImpl(TName *pVertices, const CQuadItem *pArray, int Num)
	{
		CCommandBuffer::SPoint Center;

		dbg_assert(m_Drawing == EDrawing::QUADS, "called Graphics()->QuadsDrawTL without begin");

		if(g_Config.m_GfxQuadAsTriangle && !m_Capabilities.m_QuadToTriangleConversion)
		{
			for(int i = 0; i < Num; ++i)
			{
				// first triangle
				pVertices[m_NumVertices + 6 * i].m_Pos.x = pArray[i].m_X;
				pVertices[m_NumVertices + 6 * i].m_Pos.y = pArray[i].m_Y;
				pVertices[m_NumVertices + 6 * i].m_Tex = m_aTexture[0];
				SetColor(&pVertices[m_NumVertices + 6 * i], 0);

				pVertices[m_NumVertices + 6 * i + 1].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
				pVertices[m_NumVertices + 6 * i + 1].m_Pos.y = pArray[i].m_Y;
				pVertices[m_NumVertices + 6 * i + 1].m_Tex = m_aTexture[1];
				SetColor(&pVertices[m_NumVertices + 6 * i + 1], 1);

				pVertices[m_NumVertices + 6 * i + 2].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
				pVertices[m_NumVertices + 6 * i + 2].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
				pVertices[m_NumVertices + 6 * i + 2].m_Tex = m_aTexture[2];
				SetColor(&pVertices[m_NumVertices + 6 * i + 2], 2);

				// second triangle
				pVertices[m_NumVertices + 6 * i + 3].m_Pos.x = pArray[i].m_X;
				pVertices[m_NumVertices + 6 * i + 3].m_Pos.y = pArray[i].m_Y;
				pVertices[m_NumVertices + 6 * i + 3].m_Tex = m_aTexture[0];
				SetColor(&pVertices[m_NumVertices + 6 * i + 3], 0);

				pVertices[m_NumVertices + 6 * i + 4].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
				pVertices[m_NumVertices + 6 * i + 4].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
				pVertices[m_NumVertices + 6 * i + 4].m_Tex = m_aTexture[2];
				SetColor(&pVertices[m_NumVertices + 6 * i + 4], 2);

				pVertices[m_NumVertices + 6 * i + 5].m_Pos.x = pArray[i].m_X;
				pVertices[m_NumVertices + 6 * i + 5].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
				pVertices[m_NumVertices + 6 * i + 5].m_Tex = m_aTexture[3];
				SetColor(&pVertices[m_NumVertices + 6 * i + 5], 3);

				if(m_Rotation != 0)
				{
					Center.x = pArray[i].m_X + pArray[i].m_Width / 2;
					Center.y = pArray[i].m_Y + pArray[i].m_Height / 2;

					Rotate(Center, &pVertices[m_NumVertices + 6 * i], 6);
				}
			}

			AddVertices(3 * 2 * Num, pVertices);
		}
		else
		{
			for(int i = 0; i < Num; ++i)
			{
				pVertices[m_NumVertices + 4 * i].m_Pos.x = pArray[i].m_X;
				pVertices[m_NumVertices + 4 * i].m_Pos.y = pArray[i].m_Y;
				pVertices[m_NumVertices + 4 * i].m_Tex = m_aTexture[0];
				SetColor(&pVertices[m_NumVertices + 4 * i], 0);

				pVertices[m_NumVertices + 4 * i + 1].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
				pVertices[m_NumVertices + 4 * i + 1].m_Pos.y = pArray[i].m_Y;
				pVertices[m_NumVertices + 4 * i + 1].m_Tex = m_aTexture[1];
				SetColor(&pVertices[m_NumVertices + 4 * i + 1], 1);

				pVertices[m_NumVertices + 4 * i + 2].m_Pos.x = pArray[i].m_X + pArray[i].m_Width;
				pVertices[m_NumVertices + 4 * i + 2].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
				pVertices[m_NumVertices + 4 * i + 2].m_Tex = m_aTexture[2];
				SetColor(&pVertices[m_NumVertices + 4 * i + 2], 2);

				pVertices[m_NumVertices + 4 * i + 3].m_Pos.x = pArray[i].m_X;
				pVertices[m_NumVertices + 4 * i + 3].m_Pos.y = pArray[i].m_Y + pArray[i].m_Height;
				pVertices[m_NumVertices + 4 * i + 3].m_Tex = m_aTexture[3];
				SetColor(&pVertices[m_NumVertices + 4 * i + 3], 3);

				if(m_Rotation != 0)
				{
					Center.x = pArray[i].m_X + pArray[i].m_Width / 2;
					Center.y = pArray[i].m_Y + pArray[i].m_Height / 2;

					Rotate(Center, &pVertices[m_NumVertices + 4 * i], 4);
				}
			}

			AddVertices(4 * Num, pVertices);
		}
	}

	void QuadsDrawTL(const CQuadItem *pArray, int Num) override;

	void QuadsTex3DDrawTL(const CQuadItem *pArray, int Num) override;

	void QuadsDrawFreeform(const CFreeformItem *pArray, int Num) override;
	void QuadsText(float x, float y, float Size, const char *pText) override;

	void DrawRectExt(float x, float y, float w, float h, float r, int Corners) override;
	void DrawRectExt4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, float r, int Corners) override;
	int CreateRectQuadContainer(float x, float y, float w, float h, float r, int Corners) override;
	void DrawRect(float x, float y, float w, float h, ColorRGBA Color, int Corners, float Rounding) override;
	void DrawRect4(float x, float y, float w, float h, ColorRGBA ColorTopLeft, ColorRGBA ColorTopRight, ColorRGBA ColorBottomLeft, ColorRGBA ColorBottomRight, int Corners, float Rounding) override;
	void DrawCircle(float CenterX, float CenterY, float Radius, int Segments) override;

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

	// sprites
private:
	vec2 m_SpriteScale = vec2(-1.0f, -1.0f);

protected:
	void SelectSprite(const CDataSprite *pSprite, int Flags);

public:
	void SelectSprite(int Id, int Flags = 0) override;
	void SelectSprite7(int Id, int Flags = 0) override;

	void GetSpriteScale(const CDataSprite *pSprite, float &ScaleX, float &ScaleY) const override;
	void GetSpriteScale(int Id, float &ScaleX, float &ScaleY) const override;
	void GetSpriteScaleImpl(int Width, int Height, float &ScaleX, float &ScaleY) const override;

	void DrawSprite(float x, float y, float Size) override;
	void DrawSprite(float x, float y, float ScaledWidth, float ScaledHeight) override;

	int QuadContainerAddSprite(int QuadContainerIndex, float x, float y, float Size) override;
	int QuadContainerAddSprite(int QuadContainerIndex, float Size) override;
	int QuadContainerAddSprite(int QuadContainerIndex, float Width, float Height) override;
	int QuadContainerAddSprite(int QuadContainerIndex, float X, float Y, float Width, float Height) override;

	template<typename TVertex>
	void FlushVerticesImpl(bool KeepVertices, CCommandBuffer::CPipelineHandle Pipeline, const TVertex *pVertices)
	{
		if(m_NumVertices == 0)
			return;

		const size_t VertexCount = m_NumVertices;

		if(!KeepVertices)
			m_NumVertices = 0;

		CCommandBuffer::SCommand_Draw Command;
		Command.m_State = m_State;
		Command.m_Pipeline = Pipeline;
		Command.m_VertexCount = static_cast<uint32_t>(VertexCount);

		if(m_Drawing == EDrawing::QUADS)
		{
			if(g_Config.m_GfxQuadAsTriangle && !m_Capabilities.m_QuadToTriangleConversion)
				Command.m_PrimitiveType = EPrimitiveType::TRIANGLES;
			else
			{
				Command.m_PrimitiveType = EPrimitiveType::QUADS;
				if(m_Capabilities.m_QuadToTriangleConversion)
					Command.m_IndexBuffer = m_QuadIndexBuffer;
			}
		}
		else if(m_Drawing == EDrawing::LINES)
			Command.m_PrimitiveType = EPrimitiveType::LINES;
		else if(m_Drawing == EDrawing::TRIANGLES)
			Command.m_PrimitiveType = EPrimitiveType::TRIANGLES;
		else
			return;

		Command.m_VertexData.m_Size = sizeof(TVertex) * VertexCount;
		Command.m_VertexData.m_pData = AllocCommandBufferData(Command.m_VertexData.m_Size);
		if(!AddCmd(Command, [&] {
			   Command.m_VertexData.m_pData = m_pCommandBuffer->AllocData(Command.m_VertexData.m_Size);
			   return Command.m_VertexData.m_pData != nullptr;
		   }))
			return;

		mem_copy(const_cast<void *>(Command.m_VertexData.m_pData), pVertices, Command.m_VertexData.m_Size);
	}

	void FlushVertices(bool KeepVertices = false) override;
	void FlushVerticesTex3D() override;

	void RenderTileLayer(CBufferContainerHandle BufferContainer, const ColorRGBA &Color, char **pOffsets, unsigned int *pIndicedVertexDrawNum, size_t NumIndicesOffset) override;
	void RenderBorderTiles(CBufferContainerHandle BufferContainer, const ColorRGBA &Color, char *pIndexBufferOffset, const vec2 &Offset, const vec2 &Scale, uint32_t DrawNum) override;
	void RenderQuadLayer(CBufferContainerHandle BufferContainer, SQuadRenderInfo *pQuadInfo, size_t QuadNum, int QuadOffset, bool Grouped = false) override;
	void RenderText(CBufferContainerHandle BufferContainer, int TextQuadNum, int TextureSize, CTextureHandle TextTexture, CTextureHandle TextOutlineTexture, const ColorRGBA &TextColor, const ColorRGBA &TextOutlineColor) override;
	[[nodiscard]] bool RenderTransientIndexed(const SGraphicsVertex *pVertices, uint32_t VertexCount, const void *pIndices, uint32_t IndexCount, EIndexType IndexType, const CTransientIndexedDrawRange *pRanges, uint32_t RangeCount) override;

	// modern GL functions
	CBufferHandle CreateBufferObject(size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer = false) override;
	bool RecreateBufferObject(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, int CreateFlags, bool IsMovedPointer = false) override;
	bool UpdateBufferObjectInternal(CBufferHandle Buffer, size_t UploadDataSize, void *pUploadData, size_t Offset, bool IsMovedPointer = false);
	void CopyBufferObjectInternal(CBufferHandle WriteBuffer, CBufferHandle ReadBuffer, size_t WriteOffset, size_t ReadOffset, size_t CopyDataSize);
	void DeleteBufferObject(CBufferHandle &Buffer) override;

	CBufferContainerHandle CreateBufferContainer(SBufferContainerInfo *pContainerInfo) override;
	// destroying all buffer objects means, that all referenced VBOs are destroyed automatically, so the user does not need to save references to them
	void DeleteBufferContainer(CBufferContainerHandle &Container, bool DestroyAllBO = true) override;
	void UpdateBufferContainerInternal(CBufferContainerHandle Container, SBufferContainerInfo *pContainerInfo);
	[[nodiscard]] bool IndicesNumRequiredNotify(unsigned int RequiredIndicesCount) override;

	int GetNumScreens() const override;
	const char *GetScreenName(int Screen) const override;

	void Minimize() override;
	void WarnPngliteIncompatibleImages(bool Warn) override;
	void SetWindowParams(int FullscreenMode, bool IsBorderless) override;
	bool SetWindowScreen(int Index, bool MoveToCenter) override;
	bool SwitchWindowScreen(int Index, bool MoveToCenter) override;
	void Move(int x, int y) override;
	bool Resize(int w, int h, int RefreshRate) override;
	void ResizeToScreen() override;
	void GotResized(int w, int h, int RefreshRate) override;
	void UpdateViewport(int X, int Y, int W, int H, bool ByResize) override;
	bool IsScreenKeyboardShown() override;

	void AddWindowResizeListener(WINDOW_RESIZE_FUNC pFunc) override;
	void AddWindowPropChangeListener(WINDOW_PROPS_CHANGED_FUNC pFunc) override;
	int GetWindowScreen() override;

	void WindowDestroyNtf(uint32_t WindowId) override;
	void WindowCreateNtf(uint32_t WindowId) override;

	int WindowActive() override;
	int WindowOpen() override;

	void SetWindowGrab(bool Grab) override;
	void NotifyWindow() override;

	int Init() override;
	void Shutdown() override;

	void ReadPixel(ivec2 Position, ColorRGBA *pColor) override;
	void TakeScreenshot(const char *pFilename) override;
	void TakeCustomScreenshot(const char *pFilename) override;
	void Swap() override;
	bool SetVSync(bool State) override;
	bool SetMultiSampling(uint32_t ReqMultiSamplingCount, uint32_t &MultiSamplingCountBackend) override;

	int GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen) override;
	void GetCurrentVideoMode(CVideoMode &CurMode, int Screen) override;

	// synchronization
	void InsertSignal(CSemaphore *pSemaphore) override;
	bool IsIdle() const override;
	void WaitForIdle() override;

	void AddWarning(const SWarning &Warning);
	std::optional<SWarning> CurrentWarning() override;

	std::optional<int> ShowMessageBox(const CMessageBox &MessageBox) override;

	bool IsBackendInitialized() override;

	bool GetDriverVersion(EGraphicsDriverAgeType DriverAgeType, int &Major, int &Minor, int &Patch, const char *&pName, EBackendType BackendType) override { return m_pBackend->GetDriverVersion(DriverAgeType, Major, Minor, Patch, pName, BackendType); }
	bool IsConfigModernAPI() override { return m_pBackend->IsConfigModernAPI(); }
	bool IsTileBufferingEnabled() override { return m_Capabilities.m_TileBuffering; }
	bool IsQuadBufferingEnabled() override { return m_Capabilities.m_QuadBuffering; }
	bool IsTextBufferingEnabled() override { return m_Capabilities.m_TextBuffering; }
	bool IsQuadContainerBufferingEnabled() override { return m_Capabilities.m_QuadContainerBuffering; }
	bool Uses2DTextureArrays() override { return m_Capabilities.m_2DTextureArrays; }
	int TextureLoadFlags() override
	{
		if(!HasTextureArraysSupport())
			return 0;
		return Uses2DTextureArrays() ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE : IGraphics::TEXLOAD_TO_3D_TEXTURE;
	}
	bool HasTextureArraysSupport() override { return m_Capabilities.m_TextureArrays; }

	const char *GetVendorString() override;
	const char *GetVersionString() override;
	const char *GetRendererString() override;
	const char *GetFatalError() const override;
};

extern IGraphicsBackend *CreateGraphicsBackend(EBackendType BackendOverride, EGraphicsBackendMode BackendMode);

#endif // ENGINE_CLIENT_GRAPHICS_THREADED_H
