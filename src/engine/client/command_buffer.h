#ifndef ENGINE_CLIENT_COMMAND_BUFFER_H
#define ENGINE_CLIENT_COMMAND_BUFFER_H

// The protocol between the graphics frontend and a backend: the commands, the
// buffer that carries them, and the tables that say what a pipeline program
// expects of a draw. Nothing in here knows a window or a thread.

#include <base/dbg.h>
#include <base/log.h>
#include <base/sphore.h>

#include <engine/graphics.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
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

struct SGpuTiming
{
	uint64_t m_TimeNanoseconds = 0;
	uint64_t m_Sample = 0;
	bool m_Supported = false;
};

struct SGpuTimingShared
{
	std::atomic<bool> m_Enabled{false};
	std::atomic<bool> m_Supported{false};
	std::atomic<uint64_t> m_TimeNanoseconds{0};
	std::atomic<uint64_t> m_Sample{0};

	void Publish(uint64_t TimeNanoseconds)
	{
		m_TimeNanoseconds.store(TimeNanoseconds, std::memory_order_relaxed);
		m_Sample.fetch_add(1, std::memory_order_release);
	}

	SGpuTiming Snapshot() const
	{
		const uint64_t Sample = m_Sample.load(std::memory_order_acquire);
		return {m_TimeNanoseconds.load(std::memory_order_relaxed), Sample, m_Supported.load(std::memory_order_relaxed)};
	}
};

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
	PLANAR_YUV,
	COUNT,
};

// What a program expects of the draw that uses it. Backends used to each
// carry their own copy of these rules, which is how the same mistake could be
// caught by one of them and pass silently through the other three.
struct SPipelineProgramDesc
{
	// The vertex layout the program reads, without and with a texture. Equal
	// where the program draws the same shape either way.
	IGraphics::EVertexLayout m_Layout;
	IGraphics::EVertexLayout m_TexturedLayout;
	// Draws six indices per quad out of the shared quad index buffer, so the
	// count and the offset both have to land on a quad.
	bool m_QuadIndices;
	// Reads a texture and has nothing to draw without one.
	bool m_NeedsTexture;
	// Samples the texture layer by layer instead of as a plain 2D image. Whether
	// the layers are an array or a volume is the backend's business.
	bool m_SamplesLayeredTexture;
};

inline const SPipelineProgramDesc &PipelineProgramDesc(EPipelineProgram Program)
{
	using EL = IGraphics::EVertexLayout;
	static const std::array<SPipelineProgramDesc, (size_t)EPipelineProgram::COUNT> s_aDescs = {{
		{EL::POSITION_TEXCOORD_COLOR, EL::POSITION_TEXCOORD_COLOR, true, false, false}, // PRIMITIVE
		{EL::POSITION_TEXCOORD_COLOR, EL::POSITION_TEXCOORD_COLOR, true, false, true}, // PRIMITIVE_TEXTURE_ARRAY
		{EL::POSITION_TEXCOORD_COLOR, EL::POSITION_TEXCOORD_COLOR, true, false, false}, // PRIMITIVE_UNIFORM_COLOR
		{EL::POSITION_TEXCOORD_COLOR, EL::POSITION_TEXCOORD_COLOR, true, true, false}, // PRIMITIVE_INSTANCED - only a textured pipeline exists
		{EL::QUAD, EL::QUAD_TEXTURED, true, false, false}, // QUAD_PER_ITEM
		{EL::QUAD, EL::QUAD_TEXTURED, true, false, false}, // QUAD_SHARED
		{EL::TILE, EL::TILE_TEXTURED, true, false, true}, // ARRAY_COLOR
		{EL::TILE, EL::TILE_TEXTURED, true, false, true}, // ARRAY_COLOR_TRANSFORM
		{EL::POSITION_TEXCOORD_COLOR, EL::POSITION_TEXCOORD_COLOR, true, true, false}, // DUAL_ATLAS_COMPOSITE
		{EL::POSITION_TEXCOORD_COLOR, EL::POSITION_TEXCOORD_COLOR, false, true, false}, // PLANAR_YUV
	}};
	dbg_assert(Program < EPipelineProgram::COUNT, "Unknown pipeline program");
	return s_aDescs[(size_t)Program];
}

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
		unsigned Size() const { return m_Size; }

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

	// Commands are a linked list, so they do not need to be contiguous either.
	// Without these a frame that needs more commands than the arena holds was
	// dropped whole - including its CMD_SWAP, which froze the picture on the
	// last frame that still fit.
	std::vector<std::unique_ptr<CBuffer>> m_vExtraCmdBuffers;
	size_t m_ExtraCmdBufferIndex = 0;

	CBuffer m_DataBuffer;
	std::vector<std::unique_ptr<CBuffer>> m_vExtraDataBuffers;
	size_t m_ExtraDataBufferIndex = 0;

	enum
	{
		MAX_TEXTURES = 1024 * 8,
		MAX_VERTICES = IGraphics::MAX_VERTICES,
	};

	enum
	{
		// command groups
		CMDGROUP_CORE = 0, // commands that everyone has to implement
		CMDGROUP_RENDERER = 10000, // commands specific to a renderer backend
		CMDGROUP_PLATFORM = 20000, // commands for the surface the renderer draws into

		CMD_FIRST = CMDGROUP_CORE,
	};

	enum ECommandBufferCMD
	{
		// synchronization
		CMD_SIGNAL = CMD_FIRST,

		// texture commands
		CMD_TEXTURE_CREATE,
		CMD_TEXTURE_DESTROY,
		CMD_TEXTURE_UPDATE,
		CMD_TEXTURE_READBACK,

		// rendering
		CMD_CLEAR,
		CMD_BEGIN_RENDER_PASS,
		CMD_END_RENDER_PASS,
		CMD_FLUSH_RENDER_PASS,
		CMD_DRAW, // generic draw with frame-owned transient vertex data

		// opengl 2.0+ commands (some are just emulated and only exist in opengl 3.3+)
		CMD_CREATE_BUFFER_OBJECT, // create vbo
		CMD_RECREATE_BUFFER_OBJECT, // recreate vbo
		CMD_DELETE_BUFFER_OBJECT, // delete vbo

		CMD_DRAW_INDEXED, // indexed draw from buffers the frontend keeps

		// swap
		CMD_SWAP,

		// misc
		CMD_MULTISAMPLING,
		CMD_VSYNC,
		CMD_PRESENTATION_TARGET_READBACK,
		// Wait out every readback the backend still owes. Sent when the
		// frontend has to have a picture that the device may not be finished
		// with, so that waiting for it can never outlast the render thread.
		CMD_FINISH_READBACKS,
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

	/**
	 * Where a command buffer sits in the order of submissions.
	 *
	 * Only `m_SubmissionSerial` is read outside of tests, and only as "this
	 * buffer has not been assigned yet". The frame and resource serials record
	 * that a frame packet must not be executed before the uploads it depends on:
	 * a reliable buffer carrying resource commands raises the resource serial,
	 * and every frame packet remembers the serial it needs. The backend does not
	 * consume that yet, it is the hook for dropping or reordering frames without
	 * losing their uploads, and the unit tests pin the ordering down so it stays
	 * correct until then.
	 */
	struct SSubmissionInfo
	{
		ECommandChannel m_Channel = ECommandChannel::RELIABLE;
		uint64_t m_SubmissionSerial = 0;
		uint64_t m_FrameSerial = 0;
		uint64_t m_ResourceSerial = 0;
		uint64_t m_RequiredResourceSerial = 0;
		bool m_EndsFrame = false;
		// Whether the mailbox may throw this frame away when a newer one
		// arrives. Only a frame that is actually presented may be: a newer one
		// then shows in its place. A frame that goes into a target is read back
		// or is the only output a surface-less client has, so dropping it loses
		// a picture that nothing else is going to produce.
		bool m_Replaceable = false;
	};

	class CSubmissionTracker
	{
		uint64_t m_NextSubmissionSerial = 1;
		uint64_t m_CurrentFrameSerial = 1;
		uint64_t m_CurrentResourceSerial = 0;

	public:
		SSubmissionInfo Prepare(ECommandChannel Channel, bool HasResourceCommands, bool EndsFrame, bool Replaceable = false)
		{
			SSubmissionInfo Info;
			Info.m_Channel = Channel;
			Info.m_SubmissionSerial = m_NextSubmissionSerial++;
			if(Channel == ECommandChannel::FRAME)
			{
				Info.m_FrameSerial = m_CurrentFrameSerial;
				Info.m_RequiredResourceSerial = m_CurrentResourceSerial;
				Info.m_EndsFrame = EndsFrame;
				Info.m_Replaceable = Replaceable;
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
		case CMD_TEXTURE_UPDATE:
		case CMD_CREATE_BUFFER_OBJECT:
		case CMD_RECREATE_BUFFER_OBJECT:
		case CMD_DELETE_BUFFER_OBJECT:
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
		case CMD_FINISH_READBACKS:
		case CMD_TEXTURE_DESTROY:
		case CMD_DELETE_BUFFER_OBJECT:
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
		// Signalled once the buffer this command travelled in has been run. A
		// backend that only starts the work here - an image readback left to
		// the device - clears this and signals the completion itself when the
		// result is really there, which is what lets readbacks overlap.
		mutable CCompletion *m_pCompletion;
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

	struct SCommand_DeleteBufferObject : public SCommand
	{
		SCommand_DeleteBufferObject() :
			SCommand(CMD_DELETE_BUFFER_OBJECT) {}

		IGraphics::CBufferHandle m_Buffer;
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
		EPipelineProgram m_Program = EPipelineProgram::PRIMITIVE;
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

		IGraphics::CBufferHandle m_VertexBuffer;
		IGraphics::EVertexLayout m_Layout = IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR;
		IGraphics::CBufferHandle m_IndexBuffer;
		EPipelineProgram m_Program = EPipelineProgram::PRIMITIVE;
		SDrawData m_DrawData;
		SArrayData m_ArrayData;

		uint32_t m_IndexCount = 0;
		size_t m_IndexOffset = 0;
		uint32_t m_InstanceCount = 1;

		IGraphics::EIndexType m_IndexType = IGraphics::EIndexType::UINT32;

		[[nodiscard]] bool SamplesTexture(IGraphics::CTextureHandle Texture) const { return m_State.m_Texture == Texture; }
	};

	struct SCommand_FinishReadbacks : public SCommand
	{
		SCommand_FinishReadbacks() :
			SCommand(CMD_FINISH_READBACKS) {}
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
		case CMD_DELETE_BUFFER_OBJECT:
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

	void *AllocDataChunked(unsigned WantedSize)
	{
		if(void *pData = AllocData(WantedSize))
			return pData;
		while(m_ExtraDataBufferIndex < m_vExtraDataBuffers.size())
		{
			if(void *pData = m_vExtraDataBuffers[m_ExtraDataBufferIndex]->Alloc(WantedSize))
				return pData;
			m_ExtraDataBufferIndex++;
		}
		m_vExtraDataBuffers.emplace_back(std::make_unique<CBuffer>(std::max(m_DataBuffer.Size(), WantedSize)));
		return m_vExtraDataBuffers.back()->Alloc(WantedSize);
	}

	void *AllocCommandChunked(unsigned WantedSize, unsigned Alignment)
	{
		if(void *pCommand = m_CmdBuffer.Alloc(WantedSize, Alignment))
			return pCommand;
		while(m_ExtraCmdBufferIndex < m_vExtraCmdBuffers.size())
		{
			if(void *pCommand = m_vExtraCmdBuffers[m_ExtraCmdBufferIndex]->Alloc(WantedSize, Alignment))
				return pCommand;
			m_ExtraCmdBufferIndex++;
		}
		m_vExtraCmdBuffers.emplace_back(std::make_unique<CBuffer>(std::max<unsigned>(m_CmdBuffer.Size(), WantedSize + Alignment)));
		return m_vExtraCmdBuffers.back()->Alloc(WantedSize, Alignment);
	}

	template<class T>
	bool AddCommandUnsafe(const T &Command)
	{
		// make sure that we don't do something stupid like ->AddCommand(&Cmd);
		(void)static_cast<const SCommand *>(&Command);

		// The payload budget limits how much is queued behind a payload, not how
		// large one payload may be: a texture bigger than the whole budget would
		// otherwise never fit into any buffer and could never be uploaded at all.
		const size_t CommandExternalDataSize = ExternalDataSize(Command);
		if(m_ExternalDataSize != 0 && (m_ExternalDataSize >= m_MaxExternalDataSize || CommandExternalDataSize > m_MaxExternalDataSize - m_ExternalDataSize))
			return false;

		// allocate and copy the command into the buffer
		T *pCmd = (T *)AllocCommandChunked(sizeof(T), alignof(T));
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
	IGraphics::CFrameRenderStats RenderStats() const;
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
		return m_SubmissionInfo.m_Channel == ECommandChannel::FRAME && m_SubmissionInfo.m_EndsFrame && m_SubmissionInfo.m_Replaceable && !ContainsCompletions();
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
		m_vExtraCmdBuffers.swap(Other.m_vExtraCmdBuffers);
		std::swap(m_ExtraCmdBufferIndex, Other.m_ExtraCmdBufferIndex);
		m_DataBuffer.Swap(Other.m_DataBuffer);
		m_vExtraDataBuffers.swap(Other.m_vExtraDataBuffers);
		std::swap(m_ExtraDataBufferIndex, Other.m_ExtraDataBufferIndex);
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
		for(auto &pBuffer : m_vExtraCmdBuffers)
			pBuffer->Reset();
		m_ExtraCmdBufferIndex = 0;
		m_DataBuffer.Reset();
		for(auto &pBuffer : m_vExtraDataBuffers)
			pBuffer->Reset();
		m_ExtraDataBufferIndex = 0;

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
			const size_t PixelSize = IGraphics::PixelSize(Command.m_Desc.m_Format);
			return ImageDataSize(Command.m_Desc.m_Width, Command.m_Desc.m_Height, PixelSize);
		}
		else if constexpr(std::is_same_v<T, SCommand_Texture_Update>)
		{
			if(Command.m_pData == nullptr)
				return 0;
			const size_t PixelSize = IGraphics::PixelSize(Command.m_Format);
			return ImageDataSize(Command.m_Region.m_Width, Command.m_Region.m_Height, PixelSize);
		}
		else if constexpr(std::is_same_v<T, SCommand_CreateBufferObject> || std::is_same_v<T, SCommand_RecreateBufferObject>)
		{
			return Command.m_DeletePointer && Command.m_pUploadData != nullptr ? Command.m_Desc.m_Size : 0;
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

inline IGraphics::CFrameRenderStats CCommandBuffer::RenderStats() const
{
	IGraphics::CFrameRenderStats Stats;
	for(const SCommand *pCommand = Head(); pCommand != nullptr; pCommand = pCommand->m_pNext)
	{
		++Stats.m_Commands;
		Stats.m_ResourceCommands += IsResourceCommand(pCommand->m_Cmd);
		switch(pCommand->m_Cmd)
		{
		case CMD_BEGIN_RENDER_PASS:
			++Stats.m_RenderPasses;
			break;
		case CMD_DRAW:
		{
			const auto *pDraw = static_cast<const SCommand_Draw *>(pCommand);
			++Stats.m_DrawCommands;
			++Stats.m_DrawCalls;
			++Stats.m_Instances;
			Stats.m_StreamedBytes += pDraw->m_VertexData.m_Size;
			if(pDraw->m_PrimitiveType == EPrimitiveType::QUADS)
				Stats.m_Triangles += pDraw->m_VertexCount / 4 * 2;
			else if(pDraw->m_PrimitiveType == EPrimitiveType::TRIANGLES)
				Stats.m_Triangles += pDraw->m_VertexCount / 3;
			break;
		}
		case CMD_DRAW_INDEXED:
		{
			const auto *pDraw = static_cast<const SCommand_DrawIndexed *>(pCommand);
			++Stats.m_DrawCommands;
			Stats.m_DrawCalls += 1;
			Stats.m_Instances += pDraw->m_InstanceCount;
			Stats.m_StreamedBytes += pDraw->m_DrawData.m_Size + pDraw->m_ArrayData.m_Size;
			Stats.m_Triangles += (uint64_t)pDraw->m_IndexCount / 3 * pDraw->m_InstanceCount;
			break;
		}
		case CMD_CREATE_BUFFER_OBJECT:
		{
			const auto *pCreate = static_cast<const SCommand_CreateBufferObject *>(pCommand);
			++Stats.m_BufferCreates;
			if(pCreate->m_pUploadData != nullptr)
				Stats.m_UploadBytes += pCreate->m_Desc.m_Size;
			break;
		}
		case CMD_RECREATE_BUFFER_OBJECT:
		{
			const auto *pRecreate = static_cast<const SCommand_RecreateBufferObject *>(pCommand);
			++Stats.m_BufferRecreates;
			if(pRecreate->m_pUploadData != nullptr)
				Stats.m_UploadBytes += pRecreate->m_Desc.m_Size;
			break;
		}
		case CMD_TEXTURE_CREATE:
		{
			const auto *pCreate = static_cast<const SCommand_Texture_Create *>(pCommand);
			++Stats.m_TextureCreates;
			if(pCreate->m_pData != nullptr)
				Stats.m_UploadBytes += ImageDataSize(pCreate->m_Desc.m_Width, pCreate->m_Desc.m_Height, IGraphics::PixelSize(pCreate->m_Desc.m_Format));
			break;
		}
		case CMD_TEXTURE_UPDATE:
		{
			const auto *pUpdate = static_cast<const SCommand_Texture_Update *>(pCommand);
			++Stats.m_TextureUpdates;
			if(pUpdate->m_pData != nullptr)
				Stats.m_UploadBytes += ImageDataSize(pUpdate->m_Region.m_Width, pUpdate->m_Region.m_Height, IGraphics::PixelSize(pUpdate->m_Format));
			break;
		}
		default:
			break;
		}
	}
	return Stats;
}

// What any backend can reject about an indexed draw without knowing which
// backend it is. Resource lifetimes and buffer bounds stay with the backend;
// this is only the part where all four had written down the same rule.
[[nodiscard]] inline const char *IndexedDrawInconsistency(const CCommandBuffer::SCommand_DrawIndexed &Cmd)
{
	const SPipelineProgramDesc &Desc = PipelineProgramDesc(Cmd.m_Program);
	if(Cmd.m_IndexCount == 0)
		return "index count is zero";
	if(Desc.m_NeedsTexture && !Cmd.m_State.m_Texture.IsValid())
		return "the program only exists as a textured pipeline and the draw has no texture";
	// The layout has to be the one the program's pipeline was built for, in
	// both directions. It is tempting to let an untextured draw read a buffer
	// that carries texture coordinates - they would just go unused - but
	// Vulkan and WebGPU bake the vertex stride into the pipeline, and the
	// untextured one reads eight bytes per vertex out of a twelve byte buffer.
	// OpenGL happens to survive that because its vertex array carries the real
	// stride; the rule is the strict one so that all four behave the same.
	if(Cmd.m_Layout != (Cmd.m_State.m_Texture.IsValid() ? Desc.m_TexturedLayout : Desc.m_Layout))
		return "the vertex layout is not the one the program's pipeline was built for";
	if(Desc.m_QuadIndices)
	{
		constexpr size_t QuadIndexBytes = 6 * sizeof(uint32_t);
		if(Cmd.m_IndexType != IGraphics::EIndexType::UINT32 || Cmd.m_IndexCount % 6 != 0 || Cmd.m_IndexOffset % QuadIndexBytes != 0)
			return "the draw does not sit on quad indices the way its program requires";
	}
	return nullptr;
}

[[nodiscard]] inline bool IsIndexedDrawConsistent(const CCommandBuffer::SCommand_DrawIndexed &Cmd)
{
	return IndexedDrawInconsistency(Cmd) == nullptr;
}

#endif // ENGINE_CLIENT_COMMAND_BUFFER_H
