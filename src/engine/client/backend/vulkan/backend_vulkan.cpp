#if defined(CONF_BACKEND_VULKAN)

// The Vulkan backend's one class. The declaration comes first; the
// definitions follow, in sections by what they concern.

#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/backend/embedded_shaders.h>
#include <engine/client/backend/vulkan/backend_vulkan.h>
#include <engine/client/command_buffer.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/localization.h>

#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef VK_API_VERSION_MAJOR
#define VK_API_VERSION_MAJOR VK_VERSION_MAJOR
#define VK_API_VERSION_MINOR VK_VERSION_MINOR
#define VK_API_VERSION_PATCH VK_VERSION_PATCH
#endif

// Helpers the backend uses in more than one section.
static constexpr uint64_t TimestampTickDelta(uint64_t Start, uint64_t End, uint32_t ValidBits)
{
	return ValidBits == 64 ? End - Start : (End - Start) & ((uint64_t{1} << ValidBits) - 1);
}

static_assert(TimestampTickDelta(100, 150, 64) == 50);
static_assert(TimestampTickDelta(250, 10, 8) == 16);

enum EVulkanBackendBlendModes
{
	VULKAN_BACKEND_BLEND_MODE_ALPHA = 0,
	VULKAN_BACKEND_BLEND_MODE_NONE,
	VULKAN_BACKEND_BLEND_MODE_ADDITATIVE,

	VULKAN_BACKEND_BLEND_MODE_COUNT,
};

static constexpr VkPipelineColorBlendAttachmentState CreateColorBlendAttachment(EVulkanBackendBlendModes BlendMode)
{
	VkPipelineColorBlendAttachmentState Result{};
	Result.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	Result.colorBlendOp = VK_BLEND_OP_ADD;
	Result.alphaBlendOp = VK_BLEND_OP_ADD;

	switch(BlendMode)
	{
	case VULKAN_BACKEND_BLEND_MODE_NONE:
		Result.blendEnable = VK_FALSE;
		Result.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		Result.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		Result.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		Result.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		break;
	case VULKAN_BACKEND_BLEND_MODE_ALPHA:
		Result.blendEnable = VK_TRUE;
		Result.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		Result.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		Result.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		Result.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case VULKAN_BACKEND_BLEND_MODE_ADDITATIVE:
		Result.blendEnable = VK_TRUE;
		Result.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		Result.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		Result.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		Result.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		break;
	default:
		dbg_assert_failed("Invalid Vulkan blend mode: %d", static_cast<int>(BlendMode));
	}
	return Result;
}

static_assert(CreateColorBlendAttachment(VULKAN_BACKEND_BLEND_MODE_NONE).blendEnable == VK_FALSE);
static_assert(CreateColorBlendAttachment(VULKAN_BACKEND_BLEND_MODE_ALPHA).dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
static_assert(CreateColorBlendAttachment(VULKAN_BACKEND_BLEND_MODE_ADDITATIVE).dstColorBlendFactor == VK_BLEND_FACTOR_ONE);

class CCommandProcessorFragment_Vulkan : public CCommandProcessorFragment_Renderer
{
	enum class EMemoryBlockUsage
	{
		TEXTURE,
		BUFFER,
		STREAM,
		STAGING,
	};

	[[nodiscard]] bool IsVerbose();

	static const char *MemoryUsageName(EMemoryBlockUsage MemUsage);

	void VerboseAllocatedMemory(VkDeviceSize Size, size_t FrameImageIndex, EMemoryBlockUsage MemUsage) const;

	void VerboseDeallocatedMemory(VkDeviceSize Size, size_t FrameImageIndex, EMemoryBlockUsage MemUsage) const;

	/************************
	 * STRUCT DEFINITIONS
	 ************************/

	static constexpr size_t STAGING_BUFFER_CACHE_ID = 0;
	static constexpr size_t BUFFER_OBJECT_CACHE_ID = 1;
	static constexpr size_t IMAGE_BUFFER_CACHE_ID = 2;

	struct SDeviceMemoryBlock
	{
		VkDeviceMemory m_Mem = VK_NULL_HANDLE;
		VkDeviceSize m_Size = 0;
		EMemoryBlockUsage m_UsageType;
	};

	struct SDeviceDescriptorPools;

	struct SDeviceDescriptorSet
	{
		VkDescriptorSet m_Descriptor = VK_NULL_HANDLE;
		SDeviceDescriptorPools *m_pPools = nullptr;
		size_t m_PoolIndex = std::numeric_limits<size_t>::max();
	};

	struct SDeviceDescriptorPool
	{
		VkDescriptorPool m_Pool = VK_NULL_HANDLE;
		VkDeviceSize m_Size = 0;
		VkDeviceSize m_CurSize = 0;
	};

	struct SDeviceDescriptorPools
	{
		std::vector<SDeviceDescriptorPool> m_vPools;
		VkDeviceSize m_DefaultAllocSize = 0;
		uint32_t m_DescriptorsPerSet = 1;
		bool m_IsUniformPool = false;
	};

	// some mix of queue and binary tree
	struct SMemoryHeap
	{
		struct SMemoryHeapElement;
		struct SMemoryHeapQueueElement
		{
			size_t m_AllocationSize;
			// only useful information for the heap
			size_t m_OffsetInHeap;
			// useful for the user of this element
			size_t m_OffsetToAlign;
			SMemoryHeapElement *m_pElementInHeap;
			[[nodiscard]] bool operator>(const SMemoryHeapQueueElement &Other) const { return m_AllocationSize > Other.m_AllocationSize; }
			// respects alignment requirements
			constexpr bool CanFitAllocation(size_t AllocSize, size_t AllocAlignment) const
			{
				size_t ExtraSizeAlign = m_OffsetInHeap % AllocAlignment;
				if(ExtraSizeAlign != 0)
					ExtraSizeAlign = AllocAlignment - ExtraSizeAlign;
				size_t RealAllocSize = AllocSize + ExtraSizeAlign;
				return m_AllocationSize >= RealAllocSize;
			}
		};

		typedef std::multiset<SMemoryHeapQueueElement, std::greater<>> TMemoryHeapQueue;

		struct SMemoryHeapElement
		{
			size_t m_AllocationSize;
			size_t m_Offset;
			SMemoryHeapElement *m_pParent;
			std::unique_ptr<SMemoryHeapElement> m_pLeft;
			std::unique_ptr<SMemoryHeapElement> m_pRight;

			bool m_InUse;
			TMemoryHeapQueue::iterator m_InQueue;
		};

		SMemoryHeapElement m_Root;
		TMemoryHeapQueue m_Elements;

		void Init(size_t Size, size_t Offset)
		{
			m_Root.m_AllocationSize = Size;
			m_Root.m_Offset = Offset;
			m_Root.m_pParent = nullptr;
			m_Root.m_InUse = false;

			SMemoryHeapQueueElement QueueEl;
			QueueEl.m_AllocationSize = Size;
			QueueEl.m_OffsetInHeap = Offset;
			QueueEl.m_OffsetToAlign = Offset;
			QueueEl.m_pElementInHeap = &m_Root;
			m_Root.m_InQueue = m_Elements.insert(QueueEl);
		}

		[[nodiscard]] bool Allocate(size_t AllocSize, size_t AllocAlignment, SMemoryHeapQueueElement &AllocatedMemory)
		{
			if(m_Elements.empty())
			{
				return false;
			}
			else
			{
				// check if there is enough space in this instance
				if(!m_Elements.begin()->CanFitAllocation(AllocSize, AllocAlignment))
				{
					return false;
				}
				else
				{
					// see SMemoryHeapQueueElement::operator>
					SMemoryHeapQueueElement FindAllocSize;
					FindAllocSize.m_AllocationSize = AllocSize;
					// find upper bound for a allocation size
					auto Upper = m_Elements.upper_bound(FindAllocSize);
					// then find the first entry that respects alignment, this is a linear search!
					auto FoundEl = m_Elements.rend();
					for(auto AllocIterator = std::make_reverse_iterator(Upper); AllocIterator != m_Elements.rend(); ++AllocIterator)
					{
						if(AllocIterator->CanFitAllocation(AllocSize, AllocAlignment))
						{
							FoundEl = AllocIterator;
							break;
						}
					}

					auto TopEl = *FoundEl;
					m_Elements.erase(TopEl.m_pElementInHeap->m_InQueue);

					TopEl.m_pElementInHeap->m_InUse = true;

					// calculate the real alloc size + alignment offset
					size_t ExtraSizeAlign = TopEl.m_OffsetInHeap % AllocAlignment;
					if(ExtraSizeAlign != 0)
						ExtraSizeAlign = AllocAlignment - ExtraSizeAlign;
					size_t RealAllocSize = AllocSize + ExtraSizeAlign;

					// the heap element gets children
					TopEl.m_pElementInHeap->m_pLeft = std::make_unique<SMemoryHeapElement>();
					TopEl.m_pElementInHeap->m_pLeft->m_AllocationSize = RealAllocSize;
					TopEl.m_pElementInHeap->m_pLeft->m_Offset = TopEl.m_OffsetInHeap;
					TopEl.m_pElementInHeap->m_pLeft->m_pParent = TopEl.m_pElementInHeap;
					TopEl.m_pElementInHeap->m_pLeft->m_InUse = true;

					if(RealAllocSize < TopEl.m_AllocationSize)
					{
						SMemoryHeapQueueElement RemainingEl;
						RemainingEl.m_OffsetInHeap = TopEl.m_OffsetInHeap + RealAllocSize;
						RemainingEl.m_AllocationSize = TopEl.m_AllocationSize - RealAllocSize;

						TopEl.m_pElementInHeap->m_pRight = std::make_unique<SMemoryHeapElement>();
						TopEl.m_pElementInHeap->m_pRight->m_AllocationSize = RemainingEl.m_AllocationSize;
						TopEl.m_pElementInHeap->m_pRight->m_Offset = RemainingEl.m_OffsetInHeap;
						TopEl.m_pElementInHeap->m_pRight->m_pParent = TopEl.m_pElementInHeap;
						TopEl.m_pElementInHeap->m_pRight->m_InUse = false;

						RemainingEl.m_pElementInHeap = TopEl.m_pElementInHeap->m_pRight.get();
						RemainingEl.m_pElementInHeap->m_InQueue = m_Elements.insert(RemainingEl);
					}

					AllocatedMemory.m_pElementInHeap = TopEl.m_pElementInHeap->m_pLeft.get();
					AllocatedMemory.m_AllocationSize = RealAllocSize;
					AllocatedMemory.m_OffsetInHeap = TopEl.m_OffsetInHeap;
					AllocatedMemory.m_OffsetToAlign = TopEl.m_OffsetInHeap + ExtraSizeAlign;
					return true;
				}
			}
		}

		void Free(const SMemoryHeapQueueElement &AllocatedMemory)
		{
			bool ContinueFree = true;
			SMemoryHeapQueueElement ThisEl = AllocatedMemory;
			while(ContinueFree)
			{
				// first check if the other block is in use, if not merge them again
				SMemoryHeapElement *pThisHeapObj = ThisEl.m_pElementInHeap;
				SMemoryHeapElement *pThisParent = pThisHeapObj->m_pParent;
				pThisHeapObj->m_InUse = false;
				SMemoryHeapElement *pOtherHeapObj = nullptr;
				if(pThisParent != nullptr && pThisHeapObj == pThisParent->m_pLeft.get())
					pOtherHeapObj = pThisHeapObj->m_pParent->m_pRight.get();
				else if(pThisParent != nullptr)
					pOtherHeapObj = pThisHeapObj->m_pParent->m_pLeft.get();

				if((pThisParent != nullptr && pOtherHeapObj == nullptr) || (pOtherHeapObj != nullptr && !pOtherHeapObj->m_InUse))
				{
					// merge them
					if(pOtherHeapObj != nullptr)
					{
						m_Elements.erase(pOtherHeapObj->m_InQueue);
						pOtherHeapObj->m_InUse = false;
					}

					SMemoryHeapQueueElement ParentEl;
					ParentEl.m_OffsetInHeap = pThisParent->m_Offset;
					ParentEl.m_AllocationSize = pThisParent->m_AllocationSize;
					ParentEl.m_pElementInHeap = pThisParent;

					pThisParent->m_pLeft = nullptr;
					pThisParent->m_pRight = nullptr;

					ThisEl = ParentEl;
				}
				else
				{
					// else just put this back into queue
					ThisEl.m_pElementInHeap->m_InQueue = m_Elements.insert(ThisEl);
					ContinueFree = false;
				}
			}
		}

		[[nodiscard]] bool IsUnused() const
		{
			return !m_Root.m_InUse;
		}
	};

	template<size_t Id>
	struct SMemoryBlock
	{
		SMemoryHeap::SMemoryHeapQueueElement m_HeapData;

		VkDeviceSize m_UsedSize;

		// optional
		VkBuffer m_Buffer;

		SDeviceMemoryBlock m_BufferMem;
		void *m_pMappedBuffer;

		bool m_IsCached;
		SMemoryHeap *m_pHeap;
	};

	template<size_t Id>
	struct SMemoryImageBlock : public SMemoryBlock<Id>
	{
		uint32_t m_ImageMemoryBits;
	};

	template<size_t Id>
	struct SMemoryBlockCache
	{
		struct SMemoryCacheHeap
		{
			SMemoryHeap m_Heap;
			VkBuffer m_Buffer;

			SDeviceMemoryBlock m_BufferMem;
			void *m_pMappedBuffer;
		};
		std::vector<std::unique_ptr<SMemoryCacheHeap>> m_vpMemoryHeaps;
		std::vector<std::vector<SMemoryBlock<Id>>> m_vvFrameDelayedCachedBufferCleanup;

		bool m_CanShrink = false;

		void Init(size_t SwapChainImageCount)
		{
			m_vvFrameDelayedCachedBufferCleanup.resize(SwapChainImageCount);
		}

		void DestroyFrameData(size_t ImageCount)
		{
			for(size_t i = 0; i < ImageCount; ++i)
				Cleanup(i);
			m_vvFrameDelayedCachedBufferCleanup.clear();
		}

		void Destroy(VkDevice &Device)
		{
			for(const auto &pHeap : m_vpMemoryHeaps)
			{
				if(pHeap->m_pMappedBuffer != nullptr)
					vkUnmapMemory(Device, pHeap->m_BufferMem.m_Mem);
				if(pHeap->m_Buffer != VK_NULL_HANDLE)
					vkDestroyBuffer(Device, pHeap->m_Buffer, nullptr);
				vkFreeMemory(Device, pHeap->m_BufferMem.m_Mem, nullptr);
			}

			m_vpMemoryHeaps.clear();
			m_vvFrameDelayedCachedBufferCleanup.clear();
		}

		void Cleanup(size_t ImgIndex)
		{
			for(auto &MemBlock : m_vvFrameDelayedCachedBufferCleanup[ImgIndex])
			{
				MemBlock.m_UsedSize = 0;
				MemBlock.m_pHeap->Free(MemBlock.m_HeapData);

				m_CanShrink = true;
			}
			m_vvFrameDelayedCachedBufferCleanup[ImgIndex].clear();
		}

		void FreeMemBlock(SMemoryBlock<Id> &Block, size_t ImgIndex)
		{
			m_vvFrameDelayedCachedBufferCleanup[ImgIndex].push_back(Block);
		}

		// returns the total free'd memory
		size_t Shrink(VkDevice &Device)
		{
			size_t FreedMemory = 0;
			if(m_CanShrink)
			{
				m_CanShrink = false;
				if(m_vpMemoryHeaps.size() > 1)
				{
					for(auto HeapIterator = m_vpMemoryHeaps.begin(); HeapIterator != m_vpMemoryHeaps.end();)
					{
						auto &pHeap = *HeapIterator;
						if(pHeap->m_Heap.IsUnused())
						{
							if(pHeap->m_pMappedBuffer != nullptr)
								vkUnmapMemory(Device, pHeap->m_BufferMem.m_Mem);
							if(pHeap->m_Buffer != VK_NULL_HANDLE)
								vkDestroyBuffer(Device, pHeap->m_Buffer, nullptr);
							vkFreeMemory(Device, pHeap->m_BufferMem.m_Mem, nullptr);
							FreedMemory += pHeap->m_BufferMem.m_Size;

							HeapIterator = m_vpMemoryHeaps.erase(HeapIterator);
							if(m_vpMemoryHeaps.size() == 1)
								break;
						}
						else
						{
							++HeapIterator;
						}
					}
				}
			}

			return FreedMemory;
		}
	};

	struct CTexture
	{
		VkImage m_Img = VK_NULL_HANDLE;
		SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> m_ImgMem;
		VkImageView m_ImgView = VK_NULL_HANDLE;
		VkSampler m_aSamplers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

		VkImage m_Img3D = VK_NULL_HANDLE;
		SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> m_Img3DMem;
		VkImageView m_Img3DView = VK_NULL_HANDLE;
		VkSampler m_Sampler3D = VK_NULL_HANDLE;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		size_t m_SourceWidth = 0;
		size_t m_SourceHeight = 0;
		IGraphics::ETextureFormat m_Format = IGraphics::ETextureFormat::RGBA8_UNORM;
		uint8_t m_Usage = IGraphics::TEXTURE_USAGE_SAMPLED;
		VkImageLayout m_Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkFormat m_ImageFormat = VK_FORMAT_UNDEFINED;
		uint32_t m_RescaleCount = 0;

		VkFramebuffer m_TargetFramebuffer = VK_NULL_HANDLE;
		VkImage m_TargetMultiSampleImage = VK_NULL_HANDLE;
		SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> m_TargetMultiSampleImageMem;
		VkImageView m_TargetMultiSampleImageView = VK_NULL_HANDLE;
		VkSampleCountFlagBits m_TargetSampleCount = VK_SAMPLE_COUNT_1_BIT;

		uint32_t m_MipMapCount = 1;

		std::array<SDeviceDescriptorSet, 2> m_aVKStandardTexturedDescrSets;
		SDeviceDescriptorSet m_VKStandard3DTexturedDescrSet;
	};

	struct SBufferObject
	{
		SMemoryBlock<BUFFER_OBJECT_CACHE_ID> m_Mem;
	};

	struct SBufferObjectFrame
	{
		SBufferObject m_BufferObject;
		IGraphics::EBufferUsage m_Usage = IGraphics::EBufferUsage::VERTEX;

		// since stream buffers can be used the cur buffer should always be used for rendering
		bool m_IsStreamedBuffer = false;
		VkBuffer m_CurBuffer = VK_NULL_HANDLE;
		size_t m_CurBufferOffset = 0;
	};

	struct SFrameBuffers
	{
		VkBuffer m_Buffer;
		SDeviceMemoryBlock m_BufferMem;
		size_t m_OffsetInBuffer = 0;
		size_t m_Size;
		size_t m_UsedSize;
		uint8_t *m_pMappedBufferData;

		SFrameBuffers(VkBuffer Buffer, SDeviceMemoryBlock BufferMem, size_t OffsetInBuffer, size_t Size, size_t UsedSize, uint8_t *pMappedBufferData) :
			m_Buffer(Buffer), m_BufferMem(BufferMem), m_OffsetInBuffer(OffsetInBuffer), m_Size(Size), m_UsedSize(UsedSize), m_pMappedBufferData(pMappedBufferData)
		{
		}
	};

	struct SFrameUniformBuffers : public SFrameBuffers
	{
		std::array<SDeviceDescriptorSet, 2> m_aUniformSets;

		SFrameUniformBuffers(VkBuffer Buffer, SDeviceMemoryBlock BufferMem, size_t OffsetInBuffer, size_t Size, size_t UsedSize, uint8_t *pMappedBufferData) :
			SFrameBuffers(Buffer, BufferMem, OffsetInBuffer, Size, UsedSize, pMappedBufferData) {}
	};

	template<typename TName>
	struct SStreamMemory
	{
		typedef std::vector<std::vector<TName>> TBufferObjectsOfFrame;
		typedef std::vector<std::vector<VkMappedMemoryRange>> TMemoryMapRangesOfFrame;
		typedef std::vector<size_t> TStreamUseCount;
		TBufferObjectsOfFrame m_vvBufferObjectsOfFrame;
		TMemoryMapRangesOfFrame m_vvBufferObjectsOfFrameRangeData;
		TStreamUseCount m_vCurrentUsedCount;

		std::vector<TName> &GetBuffers(size_t FrameImageIndex)
		{
			return m_vvBufferObjectsOfFrame[FrameImageIndex];
		}

		std::vector<VkMappedMemoryRange> &GetRanges(size_t FrameImageIndex)
		{
			return m_vvBufferObjectsOfFrameRangeData[FrameImageIndex];
		}

		size_t GetUsedCount(size_t FrameImageIndex)
		{
			return m_vCurrentUsedCount[FrameImageIndex];
		}

		void IncreaseUsedCount(size_t FrameImageIndex)
		{
			++m_vCurrentUsedCount[FrameImageIndex];
		}

		[[nodiscard]] bool IsUsed(size_t FrameImageIndex)
		{
			return GetUsedCount(FrameImageIndex) > 0;
		}

		void ResetFrame(size_t FrameImageIndex)
		{
			m_vCurrentUsedCount[FrameImageIndex] = 0;
		}

		void Init(size_t FrameImageCount)
		{
			m_vvBufferObjectsOfFrame.resize(FrameImageCount);
			m_vvBufferObjectsOfFrameRangeData.resize(FrameImageCount);
			m_vCurrentUsedCount.resize(FrameImageCount);
		}

		typedef std::function<void(size_t, TName &)> TDestroyBufferFunc;

		void Destroy(TDestroyBufferFunc &&DestroyBuffer)
		{
			size_t ImageIndex = 0;
			for(auto &vBuffersOfFrame : m_vvBufferObjectsOfFrame)
			{
				for(auto &BufferOfFrame : vBuffersOfFrame)
				{
					VkDeviceMemory BufferMem = BufferOfFrame.m_BufferMem.m_Mem;
					DestroyBuffer(ImageIndex, BufferOfFrame);

					// delete similar buffers
					for(auto &BufferOfFrameDel : vBuffersOfFrame)
					{
						if(BufferOfFrameDel.m_BufferMem.m_Mem == BufferMem)
						{
							BufferOfFrameDel.m_Buffer = VK_NULL_HANDLE;
							BufferOfFrameDel.m_BufferMem.m_Mem = VK_NULL_HANDLE;
						}
					}
				}
				++ImageIndex;
			}
			m_vvBufferObjectsOfFrame.clear();
			m_vvBufferObjectsOfFrameRangeData.clear();
			m_vCurrentUsedCount.clear();
		}
	};

	struct SShaderModule
	{
		VkShaderModule m_VertShaderModule = VK_NULL_HANDLE;
		VkShaderModule m_FragShaderModule = VK_NULL_HANDLE;

		VkDevice m_VKDevice = VK_NULL_HANDLE;

		~SShaderModule()
		{
			if(m_VKDevice != VK_NULL_HANDLE)
			{
				if(m_VertShaderModule != VK_NULL_HANDLE)
					vkDestroyShaderModule(m_VKDevice, m_VertShaderModule, nullptr);

				if(m_FragShaderModule != VK_NULL_HANDLE)
					vkDestroyShaderModule(m_VKDevice, m_FragShaderModule, nullptr);
			}
		}
	};

	enum EVulkanBackendAddressModes
	{
		VULKAN_BACKEND_ADDRESS_MODE_REPEAT = 0,
		VULKAN_BACKEND_ADDRESS_MODE_CLAMP_EDGES,

		VULKAN_BACKEND_ADDRESS_MODE_COUNT,
	};

	enum EVulkanBackendTextureModes
	{
		VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED = 0,
		VULKAN_BACKEND_TEXTURE_MODE_TEXTURED,

		VULKAN_BACKEND_TEXTURE_MODE_COUNT,
	};

	// Render targets are made in this format regardless of the surface's, so
	// that a target outlives a display or colour space change. The price is a
	// second set of pipelines for the target pass.
	static constexpr VkFormat RENDER_TARGET_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

	// A pipeline is built against one render pass, and the screen pass and
	// the render target pass differ in format: the screen has the surface's,
	// a target always has RENDER_TARGET_FORMAT. So there is one pipeline per
	// pass kind; the layout is the same for both.
	enum EPipelinePass
	{
		PIPELINE_PASS_SCREEN = 0,
		PIPELINE_PASS_TARGET,
		PIPELINE_PASS_COUNT,
	};

	struct SPipelineContainer
	{
		// 3 blend modes - 2 texture modes
		std::array<std::array<VkPipelineLayout, VULKAN_BACKEND_TEXTURE_MODE_COUNT>, VULKAN_BACKEND_BLEND_MODE_COUNT> m_aaPipelineLayouts{};
		std::array<std::array<std::array<VkPipeline, VULKAN_BACKEND_TEXTURE_MODE_COUNT>, VULKAN_BACKEND_BLEND_MODE_COUNT>, PIPELINE_PASS_COUNT> m_aaaPipelines{};

		void Destroy(VkDevice &Device)
		{
			for(auto &aPipeLayouts : m_aaPipelineLayouts)
			{
				for(auto &PipeLayout : aPipeLayouts)
				{
					if(PipeLayout != VK_NULL_HANDLE)
						vkDestroyPipelineLayout(Device, PipeLayout, nullptr);
					PipeLayout = VK_NULL_HANDLE;
				}
			}
			for(auto &aaPipe : m_aaaPipelines)
			{
				for(auto &aPipe : aaPipe)
				{
					for(auto &Pipe : aPipe)
					{
						if(Pipe != VK_NULL_HANDLE)
							vkDestroyPipeline(Device, Pipe, nullptr);
						Pipe = VK_NULL_HANDLE;
					}
				}
			}
		}
	};

	/*******************************
	 * UNIFORM PUSH CONSTANT LAYOUTS
	 ********************************/

	struct SUniformGPos
	{
		float m_aPos[4 * 2];
	};

	struct SUniformGTextPos
	{
		float m_aPos[4 * 2];
		float m_TextureSize;
	};

	typedef vec3 SUniformTextGFragmentOffset;

	struct SUniformTextGFragmentConstants
	{
		ColorRGBA m_TextColor;
		ColorRGBA m_TextOutlineColor;
	};

	struct SUniformTileGPos
	{
		float m_aPos[4 * 2];
	};

	struct SUniformTileGPosBorder : public SUniformTileGPos
	{
		vec2 m_Offset;
		vec2 m_Scale;
	};

	typedef ColorRGBA SUniformTileGVertColor;

	struct SUniformTileGVertColorAlign
	{
		float m_aPad[(64 - 48) / 4];
	};

	struct SUniformPrimExGPos
	{
		float m_aPos[4 * 2];
		vec2 m_Center;
		float m_Rotation;
	};

	typedef ColorRGBA SUniformPrimExGVertColor;

	struct SUniformPrimExGVertColorAlign
	{
		float m_aPad[(48 - 44) / 4];
	};

	struct SUniformSpriteMultiGPos
	{
		float m_aPos[4 * 2];
		vec2 m_Center;
	};

	typedef ColorRGBA SUniformSpriteMultiGVertColor;

	struct SUniformSpriteMultiGVertColorAlign
	{
		float m_aPad[(48 - 40) / 4];
	};

	struct SUniformSpriteMultiPushGPosBase
	{
		float m_aPos[4 * 2];
		vec2 m_Center;
		vec2 m_Padding;
	};

	struct SUniformSpriteMultiPushGPos : public SUniformSpriteMultiPushGPosBase
	{
		vec4 m_aPSR[1];
	};

	typedef ColorRGBA SUniformSpriteMultiPushGVertColor;

	struct SUniformQuadPushGBufferObject
	{
		ColorRGBA m_VertColor;
		vec2 m_Offset;
		float m_Rotation;
		float m_Padding;
	};

	struct SUniformQuadGroupedGPos
	{
		float m_aPos[4 * 2];
		SUniformQuadPushGBufferObject m_BOPush;
	};

	struct SUniformQuadGPos
	{
		float m_aPos[4 * 2];
		int32_t m_QuadOffset;
	};

	enum ESupportedSamplerTypes
	{
		SUPPORTED_SAMPLER_TYPE_REPEAT = 0,
		SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE,
		SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY,

		SUPPORTED_SAMPLER_TYPE_COUNT,
	};

	struct SSwapImgViewportExtent
	{
		VkExtent2D m_SwapImageViewport;
		bool m_HasForcedViewport = false;
		VkExtent2D m_ForcedViewport;

		// the viewport of the resulting presented image on the screen
		// if there is a forced viewport the resulting image is smaller
		// than the full swap image size
		VkExtent2D GetPresentedImageViewport() const
		{
			uint32_t ViewportWidth = m_SwapImageViewport.width;
			uint32_t ViewportHeight = m_SwapImageViewport.height;
			if(m_HasForcedViewport)
			{
				ViewportWidth = m_ForcedViewport.width;
				ViewportHeight = m_ForcedViewport.height;
			}

			return {ViewportWidth, ViewportHeight};
		}
	};

	struct SSwapChainMultiSampleImage
	{
		VkImage m_Image = VK_NULL_HANDLE;
		SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> m_ImgMem;
		VkImageView m_ImgView = VK_NULL_HANDLE;
	};

	/************************
	 * MEMBER VARIABLES
	 ************************/

	SMemoryBlockCache<STAGING_BUFFER_CACHE_ID> m_StagingBufferCache;
	SMemoryBlockCache<BUFFER_OBJECT_CACHE_ID> m_BufferObjectCache;
	std::map<uint32_t, SMemoryBlockCache<IMAGE_BUFFER_CACHE_ID>> m_ImageBufferCaches;

	std::vector<VkMappedMemoryRange> m_vNonFlushedStagingBufferRange;

	std::vector<CTexture> m_vTextures;
	CGenerationHandleStore<IGraphics::CTextureHandle> m_TextureHandles;
	CGenerationHandleStore<IGraphics::CBufferHandle> m_BufferHandles;

	std::atomic<uint64_t> *m_pTextureMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pBufferMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pStreamMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pStagingMemoryUsage = nullptr;
	// So that a target that is skipped every frame until the frontend has made
	// it again says so once instead of once per frame.
	SGpuTimingShared *m_pGpuTiming = nullptr;

	TTwGraphicsGpuList *m_pGpuList;

	int m_GlobalTextureLodBIAS;
	uint32_t m_MultiSamplingCount = 1;

	uint32_t m_NextMultiSamplingCount = std::numeric_limits<uint32_t>::max();

	bool m_RecreateSwapChain = false;
	bool m_SwapchainCreated = false;
	bool m_SwapchainRecreationDeferred = false;
	bool m_RenderingPaused = false;
	bool m_FrameCommandsRecording = false;
	bool m_AcquireSemaphorePending = false;
	bool m_HasDynamicViewport = false;
	VkOffset2D m_DynamicViewportOffset;
	VkExtent2D m_DynamicViewportSize;

	bool m_AllowsLinearBlitting = false;
	bool m_OptimalSwapChainImageBlitting = false;
	bool m_OptimalRGBAImageBlitting = false;
	bool m_LinearRGBAImageBlitting = false;

	VkDeviceSize m_NonCoherentMemAlignment;
	VkDeviceSize m_OptimalImageCopyMemAlignment;
	uint32_t m_MaxTextureSize;
	uint32_t m_MaxSamplerAnisotropy;
	VkSampleCountFlags m_MaxMultiSample;

	uint32_t m_MinUniformAlign;

	// One readback per frame slot. The copy that reads a frame back is recorded
	// into that frame's command buffer, so the frame's own fence already says
	// when the pixels have arrived - and a slot is reused only after that fence
	// was waited for, which is what bounds the ring without a second one.
	struct SReadbackSlot
	{
		SDeviceMemoryBlock m_Mem;
		VkImage m_Image = VK_NULL_HANDLE;
		uint8_t *m_pMappedMemory = nullptr;
		VkDeviceSize m_MappedLayoutOffset = 0;
		VkDeviceSize m_MappedLayoutPitch = 0;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		// What a copy in flight still owes its caller. Null while the slot has
		// no readback pending, which is what makes it collectable.
		CCommandBuffer::SImageReadbackResult *m_pResult = nullptr;
		bool m_IsB8G8R8A8 = false;
		bool m_ResetAlpha = false;
	};
	std::vector<SReadbackSlot> m_vReadbackSlots;

	std::array<VkSampler, SUPPORTED_SAMPLER_TYPE_COUNT> m_aSamplers;

	struct SDelayedBufferCleanupItem
	{
		VkBuffer m_Buffer;
		SDeviceMemoryBlock m_Mem;
		void *m_pMappedData = nullptr;
	};

	// Each slot is released only after the matching swapchain-image fence was
	// observed signaled in PrepareFrame. Resource commands may retire handles
	// immediately, but Vulkan objects and memory stay alive for in-flight draws.
	std::vector<std::vector<SDelayedBufferCleanupItem>> m_vvFrameDelayedBufferCleanup;
	std::vector<std::vector<CTexture>> m_vvFrameDelayedTextureCleanup;

private:
	std::vector<VkImageView> m_vSwapChainImageViewList;
	std::vector<SSwapChainMultiSampleImage> m_vSwapChainMultiSamplingImages;
	std::vector<VkFramebuffer> m_vFramebufferList;
	std::vector<VkCommandBuffer> m_vMainDrawCommandBuffers;

	std::vector<VkCommandBuffer> m_vMemoryCommandBuffers;
	std::vector<bool> m_vUsedMemoryCommandBuffer;
	// A memory command buffer submitted on its own, outside the frame, is
	// tracked by a fence of its own: the buffer is recorded into again only
	// once the fence says the device is through with it, and the slot's
	// memory is not given back before then either. Nothing waits at the
	// submit, and nothing waits for the whole queue.
	std::vector<VkFence> m_vMemoryCommandBufferFences;
	std::vector<bool> m_vMemoryCommandBufferPending;

	std::vector<VkSemaphore> m_vQueueSubmitSemaphores;
	std::vector<VkSemaphore> m_vBusyAcquireImageSemaphores;
	VkSemaphore m_AcquireImageSemaphore;

	std::vector<VkFence> m_vQueueSubmitFences;
	VkQueryPool m_GpuTimestampQueryPool = VK_NULL_HANDLE;
	std::vector<bool> m_vGpuTimestampPending;
	float m_GpuTimestampPeriod = 0.0f;
	uint32_t m_GpuTimestampValidBits = 0;
	bool m_GpuTimestampRecording = false;
	bool m_GpuTimestampNotReadyWarningLogged = false;

	uint64_t m_CurFrame = 0;
	std::vector<uint64_t> m_vImageLastFrameCheck;

	std::vector<SBufferObjectFrame> m_vBufferObjects;

	VkInstance m_VKInstance;
	VkPhysicalDevice m_VKGPU;
	uint32_t m_VKGraphicsQueueIndex = std::numeric_limits<uint32_t>::max();
	VkDevice m_VKDevice;
	VkQueue m_VKGraphicsQueue, m_VKPresentQueue;
	VkSurfaceKHR m_VKPresentSurface = VK_NULL_HANDLE;
	SSwapImgViewportExtent m_VKSwapImgAndViewportExtent;

#ifdef VK_EXT_debug_utils
	VkDebugUtilsMessengerEXT m_DebugMessenger;
#endif

#ifdef VK_EXT_device_fault
	// Optional VK_EXT_device_fault support. When the driver exposes the extension
	// we enable it so that a VK_ERROR_DEVICE_LOST can be followed up with detailed
	// fault information (faulting GPU addresses and vendor specific fault codes).
	bool m_DeviceFaultAvailable = false;
	PFN_vkGetDeviceFaultInfoEXT m_pfnGetDeviceFaultInfoEXT = nullptr;
#endif

	VkDescriptorSetLayout m_StandardTexturedDescriptorSetLayout;
	VkDescriptorSetLayout m_Standard3DTexturedDescriptorSetLayout;

	VkDescriptorSetLayout m_SpriteMultiUniformDescriptorSetLayout;
	VkDescriptorSetLayout m_QuadUniformDescriptorSetLayout;

	SPipelineContainer m_PrimitivePipeline;
	SPipelineContainer m_PrimitiveLinePipeline;
	SPipelineContainer m_PrimitiveTextureArrayPipeline;
	SPipelineContainer m_DualAtlasPipeline;
	SPipelineContainer m_ArrayColorPipeline;
	SPipelineContainer m_ArrayColorTransformPipeline;
	SPipelineContainer m_PrimitiveUniformColorPipeline;
	SPipelineContainer m_PrimitiveInstancedPipeline;
	SPipelineContainer m_PrimitiveInstancedPushPipeline;
	SPipelineContainer m_QuadPerItemPipeline;
	SPipelineContainer m_QuadSharedPipeline;

	VkPipeline m_LastPipeline = VK_NULL_HANDLE;
	// Consecutive draws bind the same texture far more often than not - a tile
	// layer, a run of UI icons, a HUD - and binding it again costs a driver
	// call for nothing. Two slots because dual atlas text binds a second
	// sampler and grouped quads bind a second uniform set.
	std::array<VkDescriptorSet, 2> m_aLastDescriptorSets = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	void BindDescriptorSet(VkCommandBuffer CommandBuffer, VkPipelineLayout PipeLayout, uint32_t Slot, VkDescriptorSet Descriptor);

	VkCommandPool m_CommandPool = VK_NULL_HANDLE;

	VkRenderPass m_VKRenderPass = VK_NULL_HANDLE;
	VkRenderPass m_VKRenderPassDiscard = VK_NULL_HANDLE;
	VkRenderPass m_VKRenderTargetPass = VK_NULL_HANDLE;
	VkRenderPass m_VKRenderTargetPassDiscard = VK_NULL_HANDLE;
	VkRenderPass m_CurrentRenderPass = VK_NULL_HANDLE;
	VkFramebuffer m_CurrentFramebuffer = VK_NULL_HANDLE;
	VkExtent2D m_CurrentRenderExtent = {0, 0};
	IGraphics::CTextureHandle m_CurrentRenderTarget;
	bool m_RenderPassActive = false;

	VkSurfaceFormatKHR m_VKSurfFormat;

	SDeviceDescriptorPools m_StandardTextureDescrPool;

	SDeviceDescriptorPools m_UniformBufferDescrPools;

	VkSwapchainKHR m_VKSwapChain = VK_NULL_HANDLE;
	std::vector<VkImage> m_vSwapChainImages;
	// How many frames a surface-less backend keeps in flight. Matches what the
	// video export holds in readback slots, so an export never has to wait for
	// a frame it already asked the device to finish.
	static constexpr uint32_t OFFSCREEN_FRAME_SLOT_COUNT = 3;
	uint32_t m_SwapChainImageCount = 0;

	SStreamMemory<SFrameBuffers> m_StreamedBuffers;
	SStreamMemory<SFrameUniformBuffers> m_StreamedUniformBuffers;

	uint32_t m_CurImageIndex = 0;

	uint32_t m_CanvasWidth;
	uint32_t m_CanvasHeight;

	// The surface a frame is presented to. Everything that exists only because
	// there is one - the swapchain, the present queue, the surface format, the
	// screen render pass, vsync, frame timing - hangs off this. Without a
	// window this backend still draws; it draws into render targets that
	// somebody else reads back, and it is asked for none of the above.
	CCommandProcessorFragment_Renderer::SPresentationSurface m_Presentation;

	std::array<float, 4> m_aClearColor = {0, 0, 0, 0};

	struct SRenderCommandExecuteBuffer
	{
		// useful data
		VkBuffer m_Buffer = VK_NULL_HANDLE;
		size_t m_BufferOff = 0;
		std::array<SDeviceDescriptorSet, 2> m_aDescriptors;

		VkBuffer m_IndexBuffer = VK_NULL_HANDLE;
		size_t m_IndexBufferOff = 0;

		bool m_ClearColor = false;

		VkViewport m_Viewport;
		VkRect2D m_Scissor;
	};

protected:
	/************************
	 * ERROR MANAGEMENT
	 ************************/
	std::string m_ErrorHelper;

	bool m_HasError = false;
	bool m_CanAssert = false;

	static EGfxErrorType MemoryErrorType(VkResult Result, EGfxErrorType OutOfMemoryType);

	/**
	 * After an error occurred, the rendering stop as soon as possible
	 * Always stop the current code execution after a call to this function (e.g. return false)
	 */
	void SetError(EGfxErrorType ErrType, const char *pErr, const char *pErrStrExtra = nullptr);

	void SetWarningPreMsg(const char *pWarningPre);

	void SetWarning(EGfxWarningType WarningType, const char *pWarning);

#ifdef VK_EXT_device_fault
	static const char *DeviceFaultAddressTypeName(VkDeviceFaultAddressTypeEXT Type);

	// Queries and logs VK_EXT_device_fault information. Safe to call unconditionally:
	// it is a no-op unless the extension was enabled at device creation.
	void LogDeviceFaultInfo();
#endif

	const char *CheckVulkanCriticalError(VkResult CallResult);

	void ErroneousCleanup() override;

	/*****************************
	 * VIDEO AND SCREENSHOT HELPER
	 ******************************/
	// Everything the frame still owes the queue before a copy can be recorded
	// after it: the render pass ends, the streams go up, and there is a command
	// buffer to record into even when a minimized window left none.
	// The slot's command buffer may still be running the frame that was submitted
	// on it last, and resetting a command buffer that is in flight is not allowed.
	// The frame path waits here for the same reason.
	[[nodiscard]] bool WaitForMemoryCommandBuffer(size_t Slot);

	[[nodiscard]] bool WaitForFrameSlot();

	[[nodiscard]] bool PrepareReadbackRecording();

	// One submission for the frame, the copy that reads it back, and whatever the
	// memory command buffer collected on the way. This takes the frame's place:
	// it carries the frame's fence and semaphores, so nothing is submitted for
	// it afterwards and nothing here has to wait for the device.
	[[nodiscard]] bool SubmitReadbackRecording(bool WithFrame, VkCommandBuffer &CommandBuffer);

	/**
	 * Puts the main command buffer back into recording state so the readback can
	 * record its copy commands.
	 */
	[[nodiscard]] bool RestartReadbackCommandBuffer(VkCommandBuffer CommandBuffer);

	// Gives the frame slot a linear image of the requested size to copy into.
	// The size only changes when the canvas does, so this reallocates about as
	// often as the window is resized.
	[[nodiscard]] bool PrepareReadbackSlotImage(SReadbackSlot &Slot, uint32_t Width, uint32_t Height);

	void DeleteReadbackSlotImage(SReadbackSlot &Slot);

	// Hands the copied pixels to the caller. The frame this slot rode on has
	// finished by the time this runs, which is the only thing the caller was
	// waiting for.
	[[nodiscard]] bool CollectReadbackSlot(size_t Index);

	// Releases a caller that will never get its picture. Anything that took a
	// readback out of the command buffer's hands has to do this, or the wait on
	// the other side outlives the backend.
	void AbandonReadbackSlot(SReadbackSlot &Slot);

	// Collects every readback whose frame has already finished, without waiting
	// for any that has not. Run before each command so that a caller polling
	// with IsReady sees the result as soon as the device is done with it.
	[[nodiscard]] bool CollectFinishedReadbacks();

	// Waits out every readback still in flight. This is what the frontend sends
	// when it has to have a picture now.
	[[nodiscard]] bool FinishReadbacks();

	void DestroyReadbackSlots();

	// Records the copy that reads an image back and submits it with the frame it
	// belongs to. Nothing waits here: the result is handed over in
	// CollectReadbackSlot once the frame's fence says the pixels have landed,
	// which is what lets the caller keep several readbacks in flight.
	[[nodiscard]] bool StartImageReadback(VkImage SourceImage, VkFormat SourceFormat, VkImageLayout SourceLayout, uint32_t SourceWidth, uint32_t SourceHeight, CCommandBuffer::SImageReadbackResult *pResult, bool ResetAlpha, const std::optional<ivec2> &PixelOffset, bool SubmitPendingGraphics);

	/************************
	 * MEMORY MANAGEMENT
	 ************************/

	[[nodiscard]] bool GetBufferImpl(VkDeviceSize RequiredSize, EMemoryBlockUsage MemUsage, VkBuffer &Buffer, SDeviceMemoryBlock &BufferMemory, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags BufferProperties);

	template<size_t Id,
		int64_t MemoryBlockSize, size_t BlockCount,
		bool RequiresMapping>
	[[nodiscard]] bool GetBufferBlockImpl(SMemoryBlock<Id> &RetBlock, SMemoryBlockCache<Id> &MemoryCache, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags BufferProperties, const void *pBufferData, VkDeviceSize RequiredSize, VkDeviceSize TargetAlignment)
	{
		bool Res = true;

		auto &&CreateCacheBlock = [&]() -> bool {
			SMemoryHeap::SMemoryHeapQueueElement AllocatedMem;
			SDeviceMemoryBlock TmpBufferMemory;
			typename SMemoryBlockCache<Id>::SMemoryCacheHeap *pCacheHeap = nullptr;
			auto &Heaps = MemoryCache.m_vpMemoryHeaps;
			for(const auto &pHeap : Heaps)
			{
				if(pHeap->m_Heap.Allocate(RequiredSize, TargetAlignment, AllocatedMem))
				{
					TmpBufferMemory = pHeap->m_BufferMem;
					pCacheHeap = pHeap.get();
					break;
				}
			}
			if(pCacheHeap == nullptr)
			{
				auto pNewHeap = std::make_unique<typename SMemoryBlockCache<Id>::SMemoryCacheHeap>();

				VkBuffer TmpBuffer;
				if(!GetBufferImpl(MemoryBlockSize * BlockCount, RequiresMapping ? EMemoryBlockUsage::STAGING : EMemoryBlockUsage::BUFFER, TmpBuffer, TmpBufferMemory, BufferUsage, BufferProperties))
					return false;

				void *pMapData = nullptr;

				if(RequiresMapping)
				{
					const VkResult MapResult = vkMapMemory(m_VKDevice, TmpBufferMemory.m_Mem, 0, VK_WHOLE_SIZE, 0, &pMapData);
					if(MapResult != VK_SUCCESS)
					{
						SetError(MemoryErrorType(MapResult, RequiresMapping ? EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_STAGING : EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER), "Failed to map buffer block memory.");
						CleanBufferPair(m_CurImageIndex, TmpBuffer, TmpBufferMemory);
						return false;
					}
				}

				pNewHeap->m_Buffer = TmpBuffer;
				pNewHeap->m_BufferMem = TmpBufferMemory;
				pNewHeap->m_pMappedBuffer = pMapData;

				pCacheHeap = pNewHeap.get();
				Heaps.emplace_back(std::move(pNewHeap));
				Heaps.back()->m_Heap.Init(MemoryBlockSize * BlockCount, 0);
				if(!Heaps.back()->m_Heap.Allocate(RequiredSize, TargetAlignment, AllocatedMem))
				{
					SetError(RequiresMapping ? EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_STAGING : EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "Heap allocation failed directly after creating fresh heap.");
					return false;
				}
			}

			RetBlock.m_Buffer = pCacheHeap->m_Buffer;
			RetBlock.m_BufferMem = TmpBufferMemory;
			if(RequiresMapping)
				RetBlock.m_pMappedBuffer = ((uint8_t *)pCacheHeap->m_pMappedBuffer) + AllocatedMem.m_OffsetToAlign;
			else
				RetBlock.m_pMappedBuffer = nullptr;
			RetBlock.m_IsCached = true;
			RetBlock.m_pHeap = &pCacheHeap->m_Heap;
			RetBlock.m_HeapData = AllocatedMem;
			RetBlock.m_UsedSize = RequiredSize;

			if(RequiresMapping)
				mem_copy(RetBlock.m_pMappedBuffer, pBufferData, RequiredSize);

			return true;
		};

		if(RequiredSize < (VkDeviceSize)MemoryBlockSize)
		{
			Res = CreateCacheBlock();
		}
		else
		{
			VkBuffer TmpBuffer;
			SDeviceMemoryBlock TmpBufferMemory;
			if(!GetBufferImpl(RequiredSize, RequiresMapping ? EMemoryBlockUsage::STAGING : EMemoryBlockUsage::BUFFER, TmpBuffer, TmpBufferMemory, BufferUsage, BufferProperties))
				return false;

			void *pMapData = nullptr;
			if(RequiresMapping)
			{
				const VkResult MapResult = vkMapMemory(m_VKDevice, TmpBufferMemory.m_Mem, 0, VK_WHOLE_SIZE, 0, &pMapData);
				if(MapResult != VK_SUCCESS)
				{
					SetError(MemoryErrorType(MapResult, RequiresMapping ? EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_STAGING : EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER), "Failed to map buffer memory.");
					CleanBufferPair(m_CurImageIndex, TmpBuffer, TmpBufferMemory);
					return false;
				}
				mem_copy(pMapData, pBufferData, static_cast<size_t>(RequiredSize));
			}

			RetBlock.m_Buffer = TmpBuffer;
			RetBlock.m_BufferMem = TmpBufferMemory;
			RetBlock.m_pMappedBuffer = pMapData;
			RetBlock.m_pHeap = nullptr;
			RetBlock.m_IsCached = false;
			RetBlock.m_HeapData.m_OffsetToAlign = 0;
			RetBlock.m_HeapData.m_AllocationSize = RequiredSize;
			RetBlock.m_UsedSize = RequiredSize;
		}

		return Res;
	}

	[[nodiscard]] bool GetStagingBuffer(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &ResBlock, const void *pBufferData, VkDeviceSize RequiredSize);

	[[nodiscard]] bool GetStagingBufferImage(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &ResBlock, const void *pBufferData, VkDeviceSize RequiredSize);

	template<size_t Id>
	void PrepareStagingMemRange(SMemoryBlock<Id> &Block)
	{
		VkMappedMemoryRange UploadRange{};
		UploadRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		UploadRange.memory = Block.m_BufferMem.m_Mem;
		UploadRange.offset = Block.m_HeapData.m_OffsetToAlign;

		auto AlignmentMod = ((VkDeviceSize)Block.m_HeapData.m_AllocationSize % m_NonCoherentMemAlignment);
		auto AlignmentReq = (m_NonCoherentMemAlignment - AlignmentMod);
		if(AlignmentMod == 0)
			AlignmentReq = 0;
		UploadRange.size = Block.m_HeapData.m_AllocationSize + AlignmentReq;

		if(UploadRange.offset + UploadRange.size > Block.m_BufferMem.m_Size)
			UploadRange.size = VK_WHOLE_SIZE;

		m_vNonFlushedStagingBufferRange.push_back(UploadRange);
	}

	// A staging block too large for the cache has an allocation of its own.
	// Uploading it right away instead of at the end of the frame keeps those
	// allocations from piling up while a map with several large layers loads.
	// The block is given back with the slot, once the upload's fence has
	// signalled; nothing waits for it here.
	void UploadAndFreeLargeStagingMemBlock(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &Block);

	void UploadAndFreeStagingMemBlock(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &Block);

	[[nodiscard]] bool GetBufferObjectMemory(SMemoryBlock<BUFFER_OBJECT_CACHE_ID> &ResBlock, VkDeviceSize RequiredSize);

	void FreeBufferObjectMemory(SMemoryBlock<BUFFER_OBJECT_CACHE_ID> &Block);

	static size_t ImageMipLevelCount(size_t Width, size_t Height, size_t Depth);

	static size_t ImageMipLevelCount(const VkExtent3D &ImgExtent);

	// good approximation of 1024x1024 image with mipmaps
	static constexpr int64_t IMAGE_SIZE_1024X1024_APPROXIMATION = (1024 * 1024 * 4) * 2;

	[[nodiscard]] bool GetImageMemoryImpl(VkDeviceSize RequiredSize, uint32_t RequiredMemoryTypeBits, SDeviceMemoryBlock &BufferMemory, VkMemoryPropertyFlags BufferProperties);

	template<size_t Id,
		int64_t MemoryBlockSize, size_t BlockCount>
	[[nodiscard]] bool GetImageMemoryBlockImpl(SMemoryImageBlock<Id> &RetBlock, SMemoryBlockCache<Id> &MemoryCache, VkMemoryPropertyFlags BufferProperties, VkDeviceSize RequiredSize, VkDeviceSize RequiredAlignment, uint32_t RequiredMemoryTypeBits)
	{
		auto &&CreateCacheBlock = [&]() -> bool {
			SMemoryHeap::SMemoryHeapQueueElement AllocatedMem;
			SDeviceMemoryBlock TmpBufferMemory;
			typename SMemoryBlockCache<Id>::SMemoryCacheHeap *pCacheHeap = nullptr;
			for(const auto &pHeap : MemoryCache.m_vpMemoryHeaps)
			{
				if(pHeap->m_Heap.Allocate(RequiredSize, RequiredAlignment, AllocatedMem))
				{
					TmpBufferMemory = pHeap->m_BufferMem;
					pCacheHeap = pHeap.get();
					break;
				}
			}
			if(pCacheHeap == nullptr)
			{
				auto pNewHeap = std::make_unique<typename SMemoryBlockCache<Id>::SMemoryCacheHeap>();

				if(!GetImageMemoryImpl(MemoryBlockSize * BlockCount, RequiredMemoryTypeBits, TmpBufferMemory, BufferProperties))
					return false;

				pNewHeap->m_Buffer = VK_NULL_HANDLE;
				pNewHeap->m_BufferMem = TmpBufferMemory;
				pNewHeap->m_pMappedBuffer = nullptr;

				auto &Heaps = MemoryCache.m_vpMemoryHeaps;
				pCacheHeap = pNewHeap.get();
				Heaps.emplace_back(std::move(pNewHeap));
				Heaps.back()->m_Heap.Init(MemoryBlockSize * BlockCount, 0);
				if(!Heaps.back()->m_Heap.Allocate(RequiredSize, RequiredAlignment, AllocatedMem))
				{
					dbg_assert_failed("Heap allocation failed directly after creating fresh heap for image");
				}
			}

			RetBlock.m_Buffer = VK_NULL_HANDLE;
			RetBlock.m_BufferMem = TmpBufferMemory;
			RetBlock.m_pMappedBuffer = nullptr;
			RetBlock.m_IsCached = true;
			RetBlock.m_pHeap = &pCacheHeap->m_Heap;
			RetBlock.m_HeapData = AllocatedMem;
			RetBlock.m_UsedSize = RequiredSize;

			return true;
		};

		if(RequiredSize < (VkDeviceSize)MemoryBlockSize)
		{
			if(!CreateCacheBlock())
				return false;
		}
		else
		{
			SDeviceMemoryBlock TmpBufferMemory;
			if(!GetImageMemoryImpl(RequiredSize, RequiredMemoryTypeBits, TmpBufferMemory, BufferProperties))
				return false;

			RetBlock.m_Buffer = VK_NULL_HANDLE;
			RetBlock.m_BufferMem = TmpBufferMemory;
			RetBlock.m_pMappedBuffer = nullptr;
			RetBlock.m_IsCached = false;
			RetBlock.m_pHeap = nullptr;
			RetBlock.m_HeapData.m_OffsetToAlign = 0;
			RetBlock.m_HeapData.m_AllocationSize = RequiredSize;
			RetBlock.m_UsedSize = RequiredSize;
		}

		RetBlock.m_ImageMemoryBits = RequiredMemoryTypeBits;

		return true;
	}

	[[nodiscard]] bool GetImageMemory(SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &RetBlock, VkDeviceSize RequiredSize, VkDeviceSize RequiredAlignment, uint32_t RequiredMemoryTypeBits);

	void FreeImageMemBlock(SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &Block);

	template<bool FlushForRendering, typename TName>
	void UploadStreamedBuffer(SStreamMemory<TName> &StreamedBuffer)
	{
		size_t RangeUpdateCount = 0;
		if(StreamedBuffer.IsUsed(m_CurImageIndex))
		{
			for(size_t i = 0; i < StreamedBuffer.GetUsedCount(m_CurImageIndex); ++i)
			{
				auto &BufferOfFrame = StreamedBuffer.GetBuffers(m_CurImageIndex)[i];
				auto &MemRange = StreamedBuffer.GetRanges(m_CurImageIndex)[RangeUpdateCount++];
				MemRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
				MemRange.memory = BufferOfFrame.m_BufferMem.m_Mem;
				MemRange.offset = BufferOfFrame.m_OffsetInBuffer;
				auto AlignmentMod = ((VkDeviceSize)BufferOfFrame.m_UsedSize % m_NonCoherentMemAlignment);
				auto AlignmentReq = (m_NonCoherentMemAlignment - AlignmentMod);
				if(AlignmentMod == 0)
					AlignmentReq = 0;
				MemRange.size = BufferOfFrame.m_UsedSize + AlignmentReq;

				if(MemRange.offset + MemRange.size > BufferOfFrame.m_BufferMem.m_Size)
					MemRange.size = VK_WHOLE_SIZE;

				BufferOfFrame.m_UsedSize = 0;
			}
			if(RangeUpdateCount > 0 && FlushForRendering)
			{
				vkFlushMappedMemoryRanges(m_VKDevice, RangeUpdateCount, StreamedBuffer.GetRanges(m_CurImageIndex).data());
			}
		}
		StreamedBuffer.ResetFrame(m_CurImageIndex);
	}

	void CleanBufferPair(size_t ImageIndex, VkBuffer &Buffer, SDeviceMemoryBlock &BufferMem);

	void DestroyTextureTarget(CTexture &Texture);

	void DestroyTexture(CTexture &Texture);

	void DestroyAllTextureTargets();

	void ClearFrameData(size_t FrameImageIndex);

	void ShrinkUnusedCaches();

	[[nodiscard]] bool MemoryBarrier(VkBuffer Buffer, VkDeviceSize Offset, VkDeviceSize Size, VkAccessFlags BufferAccessType, bool BeforeCommand);

	/************************
	 * SWAPPING MECHANISM
	 ************************/

	void ExecuteMemoryCommandBuffer();

	void ClearFrameMemoryUsage();

	[[nodiscard]] bool FlushRenderCommands();

	[[nodiscard]] bool EndCurrentRenderPass();

	[[nodiscard]] bool BeginCurrentRenderPass(const IGraphics::CRenderPassDesc &Desc);

	[[nodiscard]] bool CollectGpuTimestamp(uint32_t ImageIndex);

	[[nodiscard]] bool BeginGpuTimestamp();

	bool EndGpuTimestamp(VkCommandBuffer CommandBuffer);

	/**
	 * Ends the recorded command buffer and submits it. Everything that needs a
	 * swapchain image lives in @link WaitFrame @endlink instead, so a frame that
	 * only rendered into a render target can be submitted without one.
	 */
	[[nodiscard]] bool SubmitFrameCommands();

	[[nodiscard]] bool WaitFrame();

	[[nodiscard]] bool PrepareFrame();

	/**
	 * Starts recording the next frame. Holds everything that works without a
	 * swapchain image, so a paused frame can record render target work.
	 */
	[[nodiscard]] bool BeginFrameCommands();

	// Ends a frame that has no swapchain to present to: what was recorded goes
	// to the queue and the next frame records into the next slot.
	[[nodiscard]] bool SubmitOffscreenFrame();

	[[nodiscard]] bool PrepareOffscreenCommands();

	void UploadStagingBuffers();

	template<bool FlushForRendering>
	void UploadNonFlushedBuffers()
	{
		UploadStreamedBuffer<FlushForRendering>(m_StreamedBuffers);
		UploadStreamedBuffer<FlushForRendering>(m_StreamedUniformBuffers);

		UploadStagingBuffers();
	}

	[[nodiscard]] bool PureMemoryFrame();

	[[nodiscard]] bool ResumeRendering();

	[[nodiscard]] bool NextFrame();

	/************************
	 * TEXTURES
	 ************************/

	// The one place that turns an interface format into a Vulkan one. How many
	// bytes a pixel of it costs is IGraphics::PixelSize's answer, not a second
	// table's, so that a format added to one is never missing from the other.
	static VkFormat ToVkFormat(IGraphics::ETextureFormat Format);

	void ConvertRgbaToBgra(uint8_t *pData, size_t PixelCount);

	[[nodiscard]] bool UpdateTexture(size_t TextureSlot, IGraphics::ETextureFormat TextureFormat, uint8_t *pData, int64_t XOff, int64_t YOff, size_t Width, size_t Height);

	[[nodiscard]] bool CreateTextureCMD(
		int Slot,
		const IGraphics::CTextureDesc &Desc,
		uint8_t *pData);

	[[nodiscard]] bool BuildMipmaps(VkImage Image, VkFormat ImageFormat, size_t Width, size_t Height, size_t Depth, size_t MipMapLevelCount);

	[[nodiscard]] bool CreateTextureImage(size_t ImageIndex, VkImage &NewImage, SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &NewImgMem, const uint8_t *pData, VkFormat Format, size_t Width, size_t Height, size_t Depth, size_t PixelSize, size_t MipMapLevelCount, VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	VkImageView CreateTextureImageView(VkImage TexImage, VkFormat ImgFormat, VkImageViewType ViewType, size_t Depth, size_t MipMapLevelCount);

	[[nodiscard]] bool CreateTextureSamplersImpl(VkSampler &CreatedSampler, VkSamplerAddressMode AddrModeU, VkSamplerAddressMode AddrModeV, VkSamplerAddressMode AddrModeW);

	[[nodiscard]] bool CreateTextureSamplers();

	void DestroyTextureSamplers();

	VkSampler GetTextureSampler(ESupportedSamplerTypes SamplerType);

	VkImageView CreateImageView(VkImage Image, VkFormat Format, VkImageViewType ViewType, size_t Depth, size_t MipMapLevelCount);

	[[nodiscard]] bool CreateImage(uint32_t Width, uint32_t Height, uint32_t Depth, size_t MipMapLevelCount, VkFormat Format, VkImageTiling Tiling, VkImage &Image, SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &ImageMemory, VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT);

	[[nodiscard]] bool ImageBarrier(const VkImage &Image, size_t MipMapBase, size_t MipMapCount, size_t LayerBase, size_t LayerCount, VkFormat Format, VkImageLayout OldLayout, VkImageLayout NewLayout);

	// A barrier belongs in the same command buffer as the work it stands
	// between, which for a readback is the buffer the frame was recorded into.
	[[nodiscard]] bool ImageBarrierIn(VkCommandBuffer &MemCommandBuffer, const VkImage &Image, size_t MipMapBase, size_t MipMapCount, size_t LayerBase, size_t LayerCount, VkFormat Format, VkImageLayout OldLayout, VkImageLayout NewLayout);

	[[nodiscard]] bool CopyBufferToImage(VkBuffer Buffer, VkDeviceSize BufferOffset, VkImage Image, int32_t X, int32_t Y, uint32_t Width, uint32_t Height, size_t Depth);

	/************************
	 * BUFFERS
	 ************************/

	[[nodiscard]] static VkAccessFlags BufferReadAccess(IGraphics::EBufferUsage Usage);

	[[nodiscard]] bool CreateBufferObject(size_t BufferIndex, const void *pUploadData, VkDeviceSize BufferDataSize, bool IsOneFrameBuffer, IGraphics::EBufferUsage Usage);

	void DeleteBufferObject(size_t BufferIndex);

	[[nodiscard]] bool CopyBuffer(VkBuffer SrcBuffer, VkBuffer DstBuffer, VkDeviceSize SrcOffset, VkDeviceSize DstOffset, VkDeviceSize CopySize);

	/************************
	 * RENDER STATES
	 ************************/

	void GetStateMatrix(const CCommandBuffer::SState &State, std::array<float, (size_t)4 * 2> &Matrix);

	[[nodiscard]] bool GetIsTextured(const CCommandBuffer::SState &State);

	size_t GetAddressModeIndex(const CCommandBuffer::SState &State);

	size_t GetBlendModeIndex(const CCommandBuffer::SState &State);

	VkPipeline &GetPipeline(SPipelineContainer &Container, bool IsTextured, size_t BlendModeIndex);

	VkPipeline &GetPipeline(SPipelineContainer &Container, EPipelinePass Pass, bool IsTextured, size_t BlendModeIndex);

	EPipelinePass CurrentPipelinePass() const;

	VkPipelineLayout &GetPipeLayout(SPipelineContainer &Container, bool IsTextured, size_t BlendModeIndex);

	VkPipelineLayout &GetStandardPipeLayout(bool IsLineGeometry, bool IsTextured, size_t BlendModeIndex);

	VkPipeline &GetStandardPipe(bool IsLineGeometry, bool IsTextured, size_t BlendModeIndex);

	VkPipelineLayout &GetArrayColorPipeLayout(bool HasTransform, bool IsTextured, size_t BlendModeIndex);

	VkPipeline &GetArrayColorPipe(bool HasTransform, bool IsTextured, size_t BlendModeIndex);

	void GetStateIndices(const CCommandBuffer::SState &State, bool &IsTextured, size_t &BlendModeIndex, size_t &AddressModeIndex);

	void ExecBufferFillDynamicStates(const CCommandBuffer::SState &State, SRenderCommandExecuteBuffer &ExecBuffer);

	void BindPipeline(VkCommandBuffer &CommandBuffer, const SRenderCommandExecuteBuffer &ExecBuffer, VkPipeline &BindingPipe);

	/**************************
	 * RENDERING IMPLEMENTATION
	 ***************************/

	void RenderArrayColor_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, size_t BufferObjectIndex);

	[[nodiscard]] bool RenderArrayColor(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, bool HasTransform, const ColorRGBA &Color, const vec2 &Scale, const vec2 &Off, uint32_t IndexCount, size_t IndexOffset);

	template<typename TName, bool Is3DTextured>
	[[nodiscard]] bool RenderStandard(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, EPrimitiveType PrimitiveType, const TName *pVertices, uint32_t VertexCount, SPipelineContainer *pPipelineContainer = nullptr)
	{
		const uint32_t VerticesPerPrim = VerticesPerPrimitive(PrimitiveType);
		if(pVertices == nullptr || VerticesPerPrim == 0 || VertexCount == 0 || VertexCount % VerticesPerPrim != 0)
			return true;
		const uint32_t PrimitiveCount = VertexCount / VerticesPerPrim;

		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(State, m);

		bool IsLineGeometry = PrimitiveType == EPrimitiveType::LINES;

		bool IsTextured;
		size_t BlendModeIndex;
		size_t AddressModeIndex;
		GetStateIndices(State, IsTextured, BlendModeIndex, AddressModeIndex);
		auto &PipeLayout = pPipelineContainer != nullptr ? GetPipeLayout(*pPipelineContainer, IsTextured, BlendModeIndex) : (Is3DTextured ? GetPipeLayout(m_PrimitiveTextureArrayPipeline, IsTextured, BlendModeIndex) : GetStandardPipeLayout(IsLineGeometry, IsTextured, BlendModeIndex));
		auto &PipeLine = pPipelineContainer != nullptr ? GetPipeline(*pPipelineContainer, IsTextured, BlendModeIndex) : (Is3DTextured ? GetPipeline(m_PrimitiveTextureArrayPipeline, IsTextured, BlendModeIndex) : GetStandardPipe(IsLineGeometry, IsTextured, BlendModeIndex));

		auto &CommandBuffer = GetMainGraphicCommandBuffer();

		BindPipeline(CommandBuffer, ExecBuffer, PipeLine);

		size_t VertPerPrim = 2;
		bool IsIndexed = false;
		if(PrimitiveType == EPrimitiveType::QUADS)
		{
			VertPerPrim = 4;
			IsIndexed = true;
		}
		else if(PrimitiveType == EPrimitiveType::TRIANGLES)
		{
			VertPerPrim = 3;
		}

		VkBuffer VKBuffer;
		SDeviceMemoryBlock VKBufferMem;
		size_t BufferOff = 0;
		if(!CreateStreamBuffer(VKBuffer, VKBufferMem, BufferOff, pVertices, VertPerPrim * sizeof(TName) * PrimitiveCount))
			return false;

		std::array<VkBuffer, 1> aVertexBuffers = {VKBuffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		if(IsIndexed)
			vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, ExecBuffer.m_IndexBufferOff, VK_INDEX_TYPE_UINT32);

		if(IsTextured)
		{
			BindDescriptorSet(CommandBuffer, PipeLayout, 0, ExecBuffer.m_aDescriptors[0].m_Descriptor);
		}

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos), m.data());

		if(IsIndexed)
			vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(PrimitiveCount * 6), 1, 0, 0, 0);
		else
			vkCmdDraw(CommandBuffer, static_cast<uint32_t>(PrimitiveCount * VertPerPrim), 1, 0, 0);

		return true;
	}

public:
	CCommandProcessorFragment_Vulkan()
	{
		m_vTextures.reserve(CCommandBuffer::MAX_TEXTURES);
	}

	/************************
	 * VULKAN SETUP CODE
	 ************************/

#if !defined(CONF_DEMO_RENDER_TOOL)
	[[nodiscard]] bool GetVulkanExtensions(std::vector<std::string> &vVKExtensions);
#endif

	std::set<std::string> OurVKLayers();

	std::set<std::string> OurDeviceExtensions() const;

	[[nodiscard]] bool GetVulkanLayers(std::vector<std::string> &vVKLayers);

	bool IsGpuDenied(uint32_t Vendor, uint32_t DriverVersion, uint32_t ApiMajor, uint32_t ApiMinor, uint32_t ApiPatch);

	[[nodiscard]] bool CreateVulkanInstance(const std::vector<std::string> &vVKLayers, const std::vector<std::string> &vVKExtensions, bool TryDebugExtensions);

	STWGraphicGpu::ETWGraphicsGpuType VKGPUTypeToGraphicsGpuType(VkPhysicalDeviceType VKGPUType);

	static void GetVendorString(uint32_t VendorId, char *pVendorStr, size_t Size);

	// from: https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5c3986798afc39d736b825bf8a5fbf92b8d9ed49/includes/functions.php#L364
	void FormatDriverVersion(char (&aDriverVersion)[256], uint32_t DriverVersion, uint32_t VendorId);

	[[nodiscard]] bool SelectGpu(char *pRendererName, char *pVendorName, char *pVersionName);

	[[nodiscard]] bool CreateLogicalDevice(const std::vector<std::string> &vVKLayers);

#if !defined(CONF_DEMO_RENDER_TOOL)
	[[nodiscard]] bool CreateSurface();
#endif

	void DestroySurface();

	[[nodiscard]] bool GetPresentationMode(VkPresentModeKHR &VKIOMode);

	[[nodiscard]] bool GetSurfaceProperties(VkSurfaceCapabilitiesKHR &VKSurfCapabilities);

	uint32_t GetNumberOfSwapImages(const VkSurfaceCapabilitiesKHR &VKCapabilities);

	SSwapImgViewportExtent GetSwapImageSize(const VkSurfaceCapabilitiesKHR &VKCapabilities);

	[[nodiscard]] bool GetImageUsage(const VkSurfaceCapabilitiesKHR &VKCapabilities, VkImageUsageFlags &VKOutUsage);

	VkSurfaceTransformFlagBitsKHR GetTransform(const VkSurfaceCapabilitiesKHR &VKCapabilities);

	[[nodiscard]] bool GetFormat();

	[[nodiscard]] bool CreateSwapChain(VkSwapchainKHR &OldSwapChain, const VkSurfaceCapabilitiesKHR *pSurfaceCapabilities = nullptr);

	void DestroySwapChain(bool ForceDestroy);

	[[nodiscard]] bool GetSwapChainImageHandles();

#ifdef VK_EXT_debug_utils
	static VKAPI_ATTR VkBool32 VKAPI_CALL VKDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT MessageSeverity, VkDebugUtilsMessageTypeFlagsEXT MessageType, const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData);

	VkResult CreateDebugUtilsMessengerEXT(const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDebugUtilsMessengerEXT *pDebugMessenger);

	void DestroyDebugUtilsMessengerEXT(VkDebugUtilsMessengerEXT &DebugMessenger);
#endif

	void SetupDebugCallback();

	void UnregisterDebugCallback();

	[[nodiscard]] bool CreateImageViews();

	void DestroyImageViews();

	[[nodiscard]] bool CreateMultiSamplerImageAttachments();

	void DestroyMultiSamplerImageAttachments();

	[[nodiscard]] bool CreateRenderPass(VkRenderPass &RenderPass, VkFormat Format, bool ClearAttachments, VkImageLayout FinalLayout);

	void DestroyRenderPass();

	[[nodiscard]] bool EnsureTargetFramebuffer(CTexture &Texture);

	[[nodiscard]] bool CreateFramebuffers();

	void DestroyFramebuffers();

	[[nodiscard]] bool CreateShaderModule(const char *pName, VkShaderModule &ShaderModule);

	[[nodiscard]] bool CreateDescriptorSetLayouts();

	void DestroyDescriptorSetLayouts();

	[[nodiscard]] bool CreateShaders(const char *pVertName, const char *pFragName, VkPipelineShaderStageCreateInfo (&aShaderStages)[2], SShaderModule &ShaderModule);

	bool GetStandardPipelineInfo(VkPipelineInputAssemblyStateCreateInfo &InputAssembly,
		VkViewport &Viewport,
		VkRect2D &Scissor,
		VkPipelineViewportStateCreateInfo &ViewportState,
		VkPipelineRasterizationStateCreateInfo &Rasterizer,
		VkPipelineMultisampleStateCreateInfo &Multisampling,
		VkPipelineColorBlendAttachmentState &ColorBlendAttachment,
		VkPipelineColorBlendStateCreateInfo &ColorBlending,
		EVulkanBackendBlendModes BlendMode) const;

	template<bool ForceRequireDescriptors, size_t ArraySize, size_t DescrArraySize, size_t PushArraySize>
	[[nodiscard]] bool CreateGraphicsPipeline(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, uint32_t Stride, std::array<VkVertexInputAttributeDescription, ArraySize> &aInputAttr,
		std::array<VkDescriptorSetLayout, DescrArraySize> &aSetLayouts, std::array<VkPushConstantRange, PushArraySize> &aPushConstants, EVulkanBackendTextureModes TexMode,
		EVulkanBackendBlendModes BlendMode, bool IsLinePrim = false)
	{
		VkPipelineShaderStageCreateInfo aShaderStages[2];
		SShaderModule Module;
		if(!CreateShaders(pVertName, pFragName, aShaderStages, Module))
			return false;

		bool HasSampler = TexMode == VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

		VkPipelineVertexInputStateCreateInfo VertexInputInfo{};
		VertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		VkVertexInputBindingDescription BindingDescription{};
		BindingDescription.binding = 0;
		BindingDescription.stride = Stride;
		BindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VertexInputInfo.vertexBindingDescriptionCount = 1;
		VertexInputInfo.vertexAttributeDescriptionCount = aInputAttr.size();
		VertexInputInfo.pVertexBindingDescriptions = &BindingDescription;
		VertexInputInfo.pVertexAttributeDescriptions = aInputAttr.data();

		VkPipelineInputAssemblyStateCreateInfo InputAssembly{};
		VkViewport Viewport{};
		VkRect2D Scissor{};
		VkPipelineViewportStateCreateInfo ViewportState{};
		VkPipelineRasterizationStateCreateInfo Rasterizer{};
		VkPipelineMultisampleStateCreateInfo Multisampling{};
		VkPipelineColorBlendAttachmentState ColorBlendAttachment{};
		VkPipelineColorBlendStateCreateInfo ColorBlending{};

		GetStandardPipelineInfo(InputAssembly, Viewport, Scissor, ViewportState, Rasterizer, Multisampling, ColorBlendAttachment, ColorBlending, BlendMode);
		InputAssembly.topology = IsLinePrim ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineLayoutCreateInfo PipelineLayoutInfo{};
		PipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		PipelineLayoutInfo.setLayoutCount = (HasSampler || ForceRequireDescriptors) ? aSetLayouts.size() : 0;
		PipelineLayoutInfo.pSetLayouts = (HasSampler || ForceRequireDescriptors) && !aSetLayouts.empty() ? aSetLayouts.data() : nullptr;

		PipelineLayoutInfo.pushConstantRangeCount = aPushConstants.size();
		PipelineLayoutInfo.pPushConstantRanges = !aPushConstants.empty() ? aPushConstants.data() : nullptr;

		VkPipelineLayout &PipeLayout = GetPipeLayout(PipeContainer, HasSampler, size_t(BlendMode));

		if(vkCreatePipelineLayout(m_VKDevice, &PipelineLayoutInfo, nullptr, &PipeLayout) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating pipeline layout failed.");
			return false;
		}

		VkGraphicsPipelineCreateInfo PipelineInfo{};
		PipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		PipelineInfo.stageCount = 2;
		PipelineInfo.pStages = aShaderStages;
		PipelineInfo.pVertexInputState = &VertexInputInfo;
		PipelineInfo.pInputAssemblyState = &InputAssembly;
		PipelineInfo.pViewportState = &ViewportState;
		PipelineInfo.pRasterizationState = &Rasterizer;
		PipelineInfo.pMultisampleState = &Multisampling;
		PipelineInfo.pColorBlendState = &ColorBlending;
		PipelineInfo.layout = PipeLayout;
		PipelineInfo.subpass = 0;
		PipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

		std::array<VkDynamicState, 2> aDynamicStates = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
		};

		VkPipelineDynamicStateCreateInfo DynamicStateCreate{};
		DynamicStateCreate.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		DynamicStateCreate.dynamicStateCount = aDynamicStates.size();
		DynamicStateCreate.pDynamicStates = aDynamicStates.data();
		PipelineInfo.pDynamicState = &DynamicStateCreate;

		// Without a surface the screen pass was never created, and nothing
		// would draw with a pipeline built against it.
		for(size_t Pass = m_Presentation.IsPresentable() ? PIPELINE_PASS_SCREEN : PIPELINE_PASS_TARGET; Pass < PIPELINE_PASS_COUNT; ++Pass)
		{
			PipelineInfo.renderPass = Pass == PIPELINE_PASS_SCREEN ? m_VKRenderPass : m_VKRenderTargetPass;
			VkPipeline &Pipeline = GetPipeline(PipeContainer, EPipelinePass(Pass), HasSampler, size_t(BlendMode));
			if(vkCreateGraphicsPipelines(m_VKDevice, VK_NULL_HANDLE, 1, &PipelineInfo, nullptr, &Pipeline) != VK_SUCCESS)
			{
				SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the graphic pipeline failed.");
				return false;
			}
		}

		return true;
	}

	// The vertex input a pipeline is built for comes from IGraphics::VertexLayout,
	// the same table CGraphics_Threaded tags every draw with. Writing the stride
	// out by hand here is how a pipeline ends up reading eight bytes per vertex
	// out of a twelve byte buffer.
	static VkFormat VertexAttributeFormat(const IGraphics::CVertexAttributeDesc &Attribute);

	template<size_t ArraySize>
	static uint32_t FillVertexInput(IGraphics::EVertexLayout Layout, std::array<VkVertexInputAttributeDescription, ArraySize> &aAttributes)
	{
		const IGraphics::SVertexLayoutDesc &Desc = IGraphics::VertexLayout(Layout);
		dbg_assert(Desc.m_AttributeCount == ArraySize, "Pipeline and vertex layout disagree about the attribute count");
		for(uint32_t Index = 0; Index < Desc.m_AttributeCount; ++Index)
		{
			const IGraphics::CVertexAttributeDesc &Attribute = Desc.m_aAttributes[Index];
			aAttributes[Index] = {Index, 0, VertexAttributeFormat(Attribute), static_cast<uint32_t>(Attribute.m_Offset)};
		}
		return static_cast<uint32_t>(Desc.m_Stride);
	}

	[[nodiscard]] bool CreateStandardGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, bool IsLinePrim);

	[[nodiscard]] bool CreateStandardGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler, bool IsLinePipe);

	[[nodiscard]] bool CreateStandard3DGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode);

	[[nodiscard]] bool CreateStandard3DGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler);

	[[nodiscard]] bool CreateTextGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode);

	[[nodiscard]] bool CreateTextGraphicsPipeline(const char *pVertName, const char *pFragName);

	template<bool HasSampler>
	[[nodiscard]] bool CreateTileGraphicsPipelineImpl(const char *pVertName, const char *pFragName, bool IsBorder, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
	{
		std::array<VkVertexInputAttributeDescription, HasSampler ? 2 : 1> aAttributeDescriptions = {};
		const uint32_t Stride = FillVertexInput(HasSampler ? IGraphics::EVertexLayout::TILE_TEXTURED : IGraphics::EVertexLayout::TILE, aAttributeDescriptions);

		std::array<VkDescriptorSetLayout, 1> aSetLayouts;
		aSetLayouts[0] = m_Standard3DTexturedDescriptorSetLayout;

		uint32_t VertPushConstantSize = sizeof(SUniformTileGPos);
		if(IsBorder)
			VertPushConstantSize = sizeof(SUniformTileGPosBorder);

		uint32_t FragPushConstantSize = sizeof(SUniformTileGVertColor);

		std::array<VkPushConstantRange, 2> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
		aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformTileGPosBorder) + sizeof(SUniformTileGVertColorAlign), FragPushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
	}

	template<bool HasSampler>
	[[nodiscard]] bool CreateTileGraphicsPipeline(const char *pVertName, const char *pFragName, bool IsBorder)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
			Ret &= CreateTileGraphicsPipelineImpl<HasSampler>(pVertName, pFragName, IsBorder, !IsBorder ? m_ArrayColorPipeline : m_ArrayColorTransformPipeline, TexMode, EVulkanBackendBlendModes(i));

		return Ret;
	}

	[[nodiscard]] bool CreatePrimExGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode);

	[[nodiscard]] bool CreatePrimExGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler);

	[[nodiscard]] bool CreateUniformDescriptorSetLayout(VkDescriptorSetLayout &SetLayout, VkShaderStageFlags StageFlags);

	[[nodiscard]] bool CreateSpriteMultiUniformDescriptorSetLayout();

	[[nodiscard]] bool CreateQuadUniformDescriptorSetLayout();

	void DestroyUniformDescriptorSetLayouts();

	[[nodiscard]] bool CreateUniformDescriptorSet(VkDescriptorSetLayout &SetLayout, SDeviceDescriptorSet &Set, VkBuffer BindBuffer, size_t BufferSize, VkDeviceSize MemoryOffset);

	[[nodiscard]] bool CreateSpriteMultiGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode);

	[[nodiscard]] bool CreateSpriteMultiGraphicsPipeline(const char *pVertName, const char *pFragName);

	[[nodiscard]] bool CreateSpriteMultiPushGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode);

	[[nodiscard]] bool CreateSpriteMultiPushGraphicsPipeline(const char *pVertName, const char *pFragName);

	template<bool IsTextured>
	[[nodiscard]] bool CreateQuadGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
	{
		std::array<VkVertexInputAttributeDescription, IsTextured ? 3 : 2> aAttributeDescriptions = {};
		const uint32_t Stride = FillVertexInput(IsTextured ? IGraphics::EVertexLayout::QUAD_TEXTURED : IGraphics::EVertexLayout::QUAD, aAttributeDescriptions);

		std::array<VkDescriptorSetLayout, IsTextured ? 2 : 1> aSetLayouts;
		if(IsTextured)
		{
			aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;
			aSetLayouts[1] = m_QuadUniformDescriptorSetLayout;
		}
		else
		{
			aSetLayouts[0] = m_QuadUniformDescriptorSetLayout;
		}

		uint32_t PushConstantSize = sizeof(SUniformQuadGPos);

		std::array<VkPushConstantRange, 1> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, PushConstantSize};

		return CreateGraphicsPipeline<true>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
	}

	template<bool HasSampler>
	[[nodiscard]] bool CreateQuadGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
			Ret &= CreateQuadGraphicsPipelineImpl<HasSampler>(pVertName, pFragName, m_QuadPerItemPipeline, TexMode, EVulkanBackendBlendModes(i));

		return Ret;
	}

	template<bool IsTextured>
	[[nodiscard]] bool CreateQuadGroupedGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
	{
		std::array<VkVertexInputAttributeDescription, IsTextured ? 3 : 2> aAttributeDescriptions = {};
		const uint32_t Stride = FillVertexInput(IsTextured ? IGraphics::EVertexLayout::QUAD_TEXTURED : IGraphics::EVertexLayout::QUAD, aAttributeDescriptions);

		std::array<VkDescriptorSetLayout, 1> aSetLayouts;
		aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;

		uint32_t PushConstantSize = sizeof(SUniformQuadGroupedGPos);

		std::array<VkPushConstantRange, 1> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
	}

	template<bool HasSampler>
	[[nodiscard]] bool CreateQuadGroupedGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
			Ret &= CreateQuadGroupedGraphicsPipelineImpl<HasSampler>(pVertName, pFragName, m_QuadSharedPipeline, TexMode, EVulkanBackendBlendModes(i));

		return Ret;
	}

	[[nodiscard]] bool CreateCommandPool();

	void DestroyCommandPool();

	[[nodiscard]] bool CreateCommandBuffers();

	void DestroyCommandBuffer();

	[[nodiscard]] bool CreateSyncObjects();

	void DestroySyncObjects();

	void CreateGpuTimestampQueries();

	void DestroyGpuTimestampQueries();

	void DestroyBufferOfFrame(size_t ImageIndex, SFrameBuffers &Buffer);

	void DestroyUniBufferOfFrame(size_t ImageIndex, SFrameUniformBuffers &Buffer);

	/*************
	 * SWAP CHAIN
	 **************/

	void CleanupVulkanSwapChain(bool ForceSwapChainDestruct);

	template<bool IsLastCleanup>
	void CleanupVulkan(size_t SwapchainCount)
	{
		if(IsLastCleanup)
		{
			if(m_SwapchainCreated || !m_Presentation.IsPresentable())
				CleanupVulkanSwapChain(true);

			// clean all images, buffers, buffer containers
			for(auto &Texture : m_vTextures)
				DestroyTexture(Texture);

			for(auto &BufferObject : m_vBufferObjects)
			{
				if(!BufferObject.m_IsStreamedBuffer)
					FreeBufferObjectMemory(BufferObject.m_BufferObject.m_Mem);
			}
		}

		m_vImageLastFrameCheck.clear();

		m_LastPipeline = VK_NULL_HANDLE;
		m_aLastDescriptorSets = {VK_NULL_HANDLE, VK_NULL_HANDLE};

		m_StreamedBuffers.Destroy([&](size_t ImageIndex, SFrameBuffers &Buffer) { DestroyBufferOfFrame(ImageIndex, Buffer); });
		m_StreamedUniformBuffers.Destroy([&](size_t ImageIndex, SFrameUniformBuffers &Buffer) { DestroyUniBufferOfFrame(ImageIndex, Buffer); });

		for(size_t i = 0; i < SwapchainCount; ++i)
		{
			ClearFrameData(i);
		}

		m_vvFrameDelayedBufferCleanup.clear();
		m_vvFrameDelayedTextureCleanup.clear();

		m_StagingBufferCache.DestroyFrameData(SwapchainCount);
		m_BufferObjectCache.DestroyFrameData(SwapchainCount);
		for(auto &ImageBufferCache : m_ImageBufferCaches)
			ImageBufferCache.second.DestroyFrameData(SwapchainCount);

		if(IsLastCleanup)
		{
			m_StagingBufferCache.Destroy(m_VKDevice);
			m_BufferObjectCache.Destroy(m_VKDevice);
			for(auto &ImageBufferCache : m_ImageBufferCaches)
				ImageBufferCache.second.Destroy(m_VKDevice);

			m_ImageBufferCaches.clear();

			DestroyTextureSamplers();
			DestroyDescriptorPools();

			DestroyReadbackSlots();
		}

		// The fences a pending readback is waiting on are about to go. The device
		// is idle by now, so the pixels are there and the caller still gets them.
		vkDeviceWaitIdle(m_VKDevice);
		for(size_t Index = 0; Index < m_vReadbackSlots.size(); ++Index)
			(void)CollectReadbackSlot(Index);

		DestroyGpuTimestampQueries();
		DestroySyncObjects();
		DestroyCommandBuffer();

		if(IsLastCleanup)
		{
			DestroyCommandPool();
		}

		if(IsLastCleanup)
		{
			DestroyUniformDescriptorSetLayouts();
			DestroyDescriptorSetLayouts();
		}
	}

	void CleanupVulkanDevice();

	int RecreateSwapChain();

	int InitVulkanDevice(const CCommandProcessorFragment_Renderer::SPresentationSurface &Surface, char *pRendererString, char *pVendorString, char *pVersionString);

	/************************
	 * MEMORY MANAGEMENT
	 ************************/

	uint32_t FindMemoryType(VkPhysicalDevice PhyDevice, uint32_t TypeFilter, VkMemoryPropertyFlags Properties);

	[[nodiscard]] bool CreateBuffer(VkDeviceSize BufferSize, EMemoryBlockUsage MemUsage, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags MemoryProperties, VkBuffer &VKBuffer, SDeviceMemoryBlock &VKBufferMemory);

	[[nodiscard]] bool AllocateDescriptorPool(SDeviceDescriptorPools &DescriptorPools, size_t AllocPoolSize);

	[[nodiscard]] bool CreateDescriptorPools();

	void DestroyDescriptorPools();

	[[nodiscard]] bool ReserveDescriptorSet(SDeviceDescriptorPools &DescriptorPools, SDeviceDescriptorSet &Set);

	void FreeDescriptorSetFromPool(SDeviceDescriptorSet &DescrSet);

	[[nodiscard]] bool CreateNewTexturedStandardDescriptorSets(size_t TextureSlot, size_t DescrIndex);

	void DestroyTexturedStandardDescriptorSets(CTexture &Texture, size_t DescrIndex);

	[[nodiscard]] bool CreateNew3DTexturedStandardDescriptorSets(size_t TextureSlot);

	void DestroyTextured3DStandardDescriptorSets(CTexture &Texture);

	[[nodiscard]] bool HasMultiSampling() const;

	VkSampleCountFlagBits GetMaxSampleCount() const;

	VkSampleCountFlagBits GetSampleCount() const;

	[[nodiscard]] bool CreateGraphicsPipelines();

	int InitVulkanSwapChain(VkSwapchainKHR &OldSwapChain, const VkSurfaceCapabilitiesKHR *pSurfaceCapabilities = nullptr);

	int InitVulkanOffscreenResources();

	template<bool IsFirstInitialization>
	int InitVulkan()
	{
		if(IsFirstInitialization)
		{
			if(!CreateDescriptorSetLayouts())
				return -1;

			if(!CreateSpriteMultiUniformDescriptorSetLayout())
				return -1;

			if(!CreateQuadUniformDescriptorSetLayout())
				return -1;

			if(!m_Presentation.IsPresentable())
			{
				if(InitVulkanOffscreenResources() != 0)
					return -1;
			}
			else
			{
				VkSwapchainKHR OldSwapChain = VK_NULL_HANDLE;
				if(InitVulkanSwapChain(OldSwapChain) != 0)
					return -1;
			}
		}

		if(IsFirstInitialization)
		{
			if(!CreateCommandPool())
				return -1;
		}

		if(!CreateCommandBuffers())
			return -1;

		if(!CreateSyncObjects())
			return -1;
		CreateGpuTimestampQueries();

		if(IsFirstInitialization)
		{
			if(!CreateDescriptorPools())
				return -1;

			if(!CreateTextureSamplers())
				return -1;
		}

		m_StreamedBuffers.Init(m_SwapChainImageCount);
		m_StreamedUniformBuffers.Init(m_SwapChainImageCount);

		m_LastPipeline = VK_NULL_HANDLE;
		m_aLastDescriptorSets = {VK_NULL_HANDLE, VK_NULL_HANDLE};

		m_vvFrameDelayedBufferCleanup.resize(m_SwapChainImageCount);
		m_vvFrameDelayedTextureCleanup.resize(m_SwapChainImageCount);
		m_StagingBufferCache.Init(m_SwapChainImageCount);
		m_BufferObjectCache.Init(m_SwapChainImageCount);
		for(auto &ImageBufferCache : m_ImageBufferCaches)
			ImageBufferCache.second.Init(m_SwapChainImageCount);

		m_vImageLastFrameCheck.resize(m_SwapChainImageCount, 0);
		m_vReadbackSlots.resize(m_SwapChainImageCount);

		if(IsFirstInitialization)
		{
			// check if image format supports linear blitting
			VkFormatProperties FormatProperties;
			vkGetPhysicalDeviceFormatProperties(m_VKGPU, VK_FORMAT_R8G8B8A8_UNORM, &FormatProperties);
			if((FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0)
			{
				m_AllowsLinearBlitting = true;
			}
			if((FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 && (FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0)
			{
				m_OptimalRGBAImageBlitting = true;
			}
			// check if image format supports blitting to linear tiled images
			if((FormatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0)
			{
				m_LinearRGBAImageBlitting = true;
			}

			vkGetPhysicalDeviceFormatProperties(m_VKGPU, m_VKSurfFormat.format, &FormatProperties);
			if((FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0)
			{
				m_OptimalSwapChainImageBlitting = true;
			}
		}

		return 0;
	}

	[[nodiscard]] bool GetMemoryCommandBuffer(VkCommandBuffer *&pMemCommandBuffer);

	VkCommandBuffer &GetMainGraphicCommandBuffer();

	/************************
	 * STREAM BUFFERS SETUP
	 ************************/

	typedef std::function<bool(SFrameBuffers &, VkBuffer, VkDeviceSize)> TNewMemFunc;

	// returns true, if the stream memory was just allocated
	template<typename TStreamMemName, typename TInstanceTypeName, size_t InstanceTypeCount, size_t BufferCreateCount, bool UsesCurrentCountOffset>
	[[nodiscard]] bool CreateStreamBuffer(TStreamMemName *&pBufferMem, TNewMemFunc &&NewMemFunc, SStreamMemory<TStreamMemName> &StreamUniformBuffer, VkBufferUsageFlags Usage, VkBuffer &NewBuffer, SDeviceMemoryBlock &NewBufferMem, size_t &BufferOffset, const void *pData, size_t DataSize, size_t Alignment = 1)
	{
		dbg_assert(Alignment > 0, "Vulkan stream buffer alignment must be positive");
		VkBuffer Buffer = VK_NULL_HANDLE;
		SDeviceMemoryBlock BufferMem;
		size_t Offset = 0;

		uint8_t *pMem = nullptr;

		size_t BufferCountOffset = 0;
		if(UsesCurrentCountOffset)
			BufferCountOffset = StreamUniformBuffer.GetUsedCount(m_CurImageIndex);
		for(; BufferCountOffset < StreamUniformBuffer.GetBuffers(m_CurImageIndex).size(); ++BufferCountOffset)
		{
			auto &BufferOfFrame = StreamUniformBuffer.GetBuffers(m_CurImageIndex)[BufferCountOffset];
			const size_t AlignmentRemainder = BufferOfFrame.m_UsedSize % Alignment;
			const size_t AlignmentPadding = AlignmentRemainder == 0 ? 0 : Alignment - AlignmentRemainder;
			if(BufferOfFrame.m_UsedSize <= BufferOfFrame.m_Size && AlignmentPadding <= BufferOfFrame.m_Size - BufferOfFrame.m_UsedSize && DataSize <= BufferOfFrame.m_Size - BufferOfFrame.m_UsedSize - AlignmentPadding)
			{
				if(BufferOfFrame.m_UsedSize == 0)
					StreamUniformBuffer.IncreaseUsedCount(m_CurImageIndex);
				Buffer = BufferOfFrame.m_Buffer;
				BufferMem = BufferOfFrame.m_BufferMem;
				BufferOfFrame.m_UsedSize += AlignmentPadding;
				Offset = BufferOfFrame.m_UsedSize;
				BufferOfFrame.m_UsedSize += DataSize;
				pMem = BufferOfFrame.m_pMappedBufferData;
				pBufferMem = &BufferOfFrame;
				break;
			}
		}

		if(BufferMem.m_Mem == VK_NULL_HANDLE)
		{
			// create memory
			VkBuffer StreamBuffer;
			SDeviceMemoryBlock StreamBufferMemory;
			const VkDeviceSize NewBufferSingleSize = sizeof(TInstanceTypeName) * InstanceTypeCount;
			const VkDeviceSize NewBufferSize = NewBufferSingleSize * BufferCreateCount;
			if(!CreateBuffer(NewBufferSize, EMemoryBlockUsage::STREAM, Usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, StreamBuffer, StreamBufferMemory))
				return false;

			void *pMappedData = nullptr;
			const VkResult MapResult = vkMapMemory(m_VKDevice, StreamBufferMemory.m_Mem, 0, VK_WHOLE_SIZE, 0, &pMappedData);
			if(MapResult != VK_SUCCESS)
			{
				SetError(MemoryErrorType(MapResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER), "Failed to map stream buffer memory.");
				CleanBufferPair(m_CurImageIndex, StreamBuffer, StreamBufferMemory);
				return false;
			}

			size_t NewBufferIndex = StreamUniformBuffer.GetBuffers(m_CurImageIndex).size();
			for(size_t i = 0; i < BufferCreateCount; ++i)
			{
				StreamUniformBuffer.GetBuffers(m_CurImageIndex).push_back(TStreamMemName(StreamBuffer, StreamBufferMemory, NewBufferSingleSize * i, NewBufferSingleSize, 0, ((uint8_t *)pMappedData) + (NewBufferSingleSize * i)));
				StreamUniformBuffer.GetRanges(m_CurImageIndex).push_back({});
				if(!NewMemFunc(StreamUniformBuffer.GetBuffers(m_CurImageIndex).back(), StreamBuffer, NewBufferSingleSize * i))
				{
					StreamUniformBuffer.GetBuffers(m_CurImageIndex).pop_back();
					StreamUniformBuffer.GetRanges(m_CurImageIndex).pop_back();
					if(i == 0)
					{
						vkUnmapMemory(m_VKDevice, StreamBufferMemory.m_Mem);
						CleanBufferPair(m_CurImageIndex, StreamBuffer, StreamBufferMemory);
					}
					return false;
				}
			}
			auto &NewStreamBuffer = StreamUniformBuffer.GetBuffers(m_CurImageIndex)[NewBufferIndex];

			Buffer = StreamBuffer;
			BufferMem = StreamBufferMemory;

			pBufferMem = &NewStreamBuffer;
			pMem = NewStreamBuffer.m_pMappedBufferData;
			Offset = NewStreamBuffer.m_OffsetInBuffer;
			NewStreamBuffer.m_UsedSize += DataSize;

			StreamUniformBuffer.IncreaseUsedCount(m_CurImageIndex);
		}

		// Offset here is the offset in the buffer
		if(BufferMem.m_Size - Offset < DataSize)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER, "Stream buffers are limited to CCommandBuffer::MAX_VERTICES. Exceeding it is a bug in the high level code.");
			return false;
		}

		{
			mem_copy(pMem + Offset, pData, DataSize);
		}

		NewBuffer = Buffer;
		NewBufferMem = BufferMem;
		BufferOffset = Offset;

		return true;
	}

	[[nodiscard]] bool CreateStreamBuffer(VkBuffer &NewBuffer, SDeviceMemoryBlock &NewBufferMem, size_t &BufferOffset, const void *pData, size_t DataSize);

	template<typename TName, size_t InstanceMaxParticleCount, size_t MaxInstances>
	[[nodiscard]] bool GetUniformBufferObjectImpl(bool RequiresSharedStagesDescriptor, SStreamMemory<SFrameUniformBuffers> &StreamUniformBuffer, SDeviceDescriptorSet &DescrSet, const void *pData, size_t DataSize)
	{
		VkBuffer NewBuffer;
		SDeviceMemoryBlock NewBufferMem;
		size_t BufferOffset;
		SFrameUniformBuffers *pMem;
		if(!CreateStreamBuffer<SFrameUniformBuffers, TName, InstanceMaxParticleCount, MaxInstances, true>(
			   pMem,
			   [this](SFrameBuffers &Mem, VkBuffer Buffer, VkDeviceSize MemOffset) {
				   auto &UniformSets = ((SFrameUniformBuffers &)Mem).m_aUniformSets;
				   if(!CreateUniformDescriptorSet(m_SpriteMultiUniformDescriptorSetLayout, UniformSets[0], Buffer, InstanceMaxParticleCount * sizeof(TName), MemOffset))
					   return false;
				   if(!CreateUniformDescriptorSet(m_QuadUniformDescriptorSetLayout, UniformSets[1], Buffer, InstanceMaxParticleCount * sizeof(TName), MemOffset))
				   {
					   FreeDescriptorSetFromPool(UniformSets[0]);
					   return false;
				   }
				   return true;
			   },
			   StreamUniformBuffer, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, NewBuffer, NewBufferMem, BufferOffset, pData, DataSize))
			return false;

		DescrSet = pMem->m_aUniformSets[RequiresSharedStagesDescriptor ? 1 : 0];
		return true;
	}

	[[nodiscard]] bool GetUniformBufferObject(bool RequiresSharedStagesDescriptor, SDeviceDescriptorSet &DescrSet, const void *pData, size_t DataSize);

	/************************
	 * COMMAND IMPLEMENTATION
	 ************************/
	[[nodiscard]] static const CCommandBuffer::SState *RenderCommandState(const CCommandBuffer::SCommand *pCommand);

	[[nodiscard]] static const IGraphics::CBufferHandle *RenderCommandVertexBuffer(const CCommandBuffer::SCommand *pCommand);

	[[nodiscard]] static const IGraphics::CBufferHandle *RenderCommandIndexBuffer(const CCommandBuffer::SCommand *pCommand);

	// Empty for a command that draws nothing, so that an invalid program on a
	// draw is not mistaken for the absence of one.
	[[nodiscard]] static std::optional<EPipelineProgram> RenderCommandProgram(const CCommandBuffer::SCommand *pCommand);

	[[nodiscard]] bool IsRenderCommandValid(const CCommandBuffer::SCommand *pCommand) const;

	[[nodiscard]] ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override;

	[[nodiscard]] bool Cmd_Init(const SCommand_Init *pCommand);

	[[nodiscard]] bool Cmd_Shutdown(const SCommand_Shutdown *pCommand);

	[[nodiscard]] bool Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand);

	[[nodiscard]] bool Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand);

	[[nodiscard]] bool Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand);

	[[nodiscard]] bool Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand);

	[[nodiscard]] bool Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand);

	[[nodiscard]] bool Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand);

	[[nodiscard]] bool Cmd_FlushRenderPass(const CCommandBuffer::SCommand_FlushRenderPass *pCommand);

	[[nodiscard]] bool Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand);

	// What a draw needs to be recorded, collected where it is recorded. This
	// used to be filled on one thread and executed on another; there is one
	// thread now, so it is a local.
	SRenderCommandExecuteBuffer CollectDrawState(const CCommandBuffer::SCommand_Draw *pCommand);

	[[nodiscard]] bool Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand);

	[[nodiscard]] bool Cmd_PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand);

	[[nodiscard]] bool Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand);

	[[nodiscard]] bool Cmd_VSync(const CCommandBuffer::SCommand_VSync *pCommand);

	[[nodiscard]] bool Cmd_MultiSampling(const CCommandBuffer::SCommand_MultiSampling *pCommand);

	[[nodiscard]] bool Cmd_Swap(const CCommandBuffer::SCommand_Swap *pCommand);

	[[nodiscard]] bool Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand);

	[[nodiscard]] bool Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand);

	[[nodiscard]] bool Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand);

	void VertexBuffer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, size_t BufferObjectIndex);

	SRenderCommandExecuteBuffer CollectIndexedDrawState(const CCommandBuffer::SCommand_DrawIndexed *pCommand);

	[[nodiscard]] bool Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand);

	[[nodiscard]] bool Cmd_DrawIndexedDualAtlas(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer);

	[[nodiscard]] bool Cmd_DrawIndexedArrayColor(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer);

	[[nodiscard]] bool Cmd_DrawIndexedQuadRecords(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer);

	[[nodiscard]] bool Cmd_DrawIndexedInstanced(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer);

	[[nodiscard]] bool Cmd_WindowCreateNtf(const CCommandBuffer::SCommand_WindowCreateNtf *pCommand);

	[[nodiscard]] bool Cmd_WindowDestroyNtf(const CCommandBuffer::SCommand_WindowDestroyNtf *pCommand);

	[[nodiscard]] bool Cmd_PreInit(const CCommandProcessorFragment_Renderer::SCommand_PreInit *pCommand);

	[[nodiscard]] bool Cmd_PostShutdown(const CCommandProcessorFragment_Renderer::SCommand_PostShutdown *pCommand);
};

// ---------------------------------------------------------------------------
// Core: device faults, frame slots, command buffers, timestamps and the
// command dispatch.
// ---------------------------------------------------------------------------

bool CCommandProcessorFragment_Vulkan::IsVerbose()
{
	return g_Config.m_DbgGfx == DEBUG_GFX_MODE_VERBOSE || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL;
}

void CCommandProcessorFragment_Vulkan::SetError(EGfxErrorType ErrType, const char *pErr, const char *pErrStrExtra)
{
	if(std::find(m_Error.m_vErrors.begin(), m_Error.m_vErrors.end(), pErr) == m_Error.m_vErrors.end())
		m_Error.m_vErrors.emplace_back(pErr);
	if(pErrStrExtra != nullptr)
	{
		if(std::find(m_Error.m_vErrors.begin(), m_Error.m_vErrors.end(), pErrStrExtra) == m_Error.m_vErrors.end())
			m_Error.m_vErrors.emplace_back(pErrStrExtra);
	}
	if(m_CanAssert)
	{
		if(pErrStrExtra != nullptr)
			log_error("gfx/vulkan", "%s: %s", pErr, pErrStrExtra);
		else
			log_error("gfx/vulkan", "%s", pErr);
		if(!m_HasError)
		{
			m_Error.m_ErrorType = ErrType;
			m_HasError = true;
		}
	}
	else
	{
		// during initialization vulkan should not throw any errors but warnings instead
		// since most code in the swapchain is shared with runtime code, add this extra code path
		SetWarning(EGfxWarningType::GFX_WARNING_TYPE_INIT_FAILED, pErr);
	}
}

void CCommandProcessorFragment_Vulkan::SetWarningPreMsg(const char *pWarningPre)
{
	if(std::find(m_Warning.m_vWarnings.begin(), m_Warning.m_vWarnings.end(), pWarningPre) == m_Warning.m_vWarnings.end())
		m_Warning.m_vWarnings.emplace(m_Warning.m_vWarnings.begin(), pWarningPre);
}

void CCommandProcessorFragment_Vulkan::SetWarning(EGfxWarningType WarningType, const char *pWarning)
{
	log_warn("gfx/vulkan", "%s", pWarning);
	if(std::find(m_Warning.m_vWarnings.begin(), m_Warning.m_vWarnings.end(), pWarning) == m_Warning.m_vWarnings.end())
		m_Warning.m_vWarnings.emplace_back(pWarning);
	m_Warning.m_WarningType = WarningType;
}

const char *CCommandProcessorFragment_Vulkan::DeviceFaultAddressTypeName(VkDeviceFaultAddressTypeEXT Type)
{
	switch(Type)
	{
	case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read_invalid";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write_invalid";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute_invalid";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction_pointer_unknown";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction_pointer_invalid";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction_pointer_fault";
	default: return "unknown";
	}
}

void CCommandProcessorFragment_Vulkan::LogDeviceFaultInfo()
{
	if(!m_DeviceFaultAvailable || m_pfnGetDeviceFaultInfoEXT == nullptr)
		return;

	VkDeviceFaultCountsEXT FaultCounts = {};
	FaultCounts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
	if(m_pfnGetDeviceFaultInfoEXT(m_VKDevice, &FaultCounts, nullptr) != VK_SUCCESS)
		return;

	std::vector<VkDeviceFaultAddressInfoEXT> vAddressInfos(FaultCounts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT> vVendorInfos(FaultCounts.vendorInfoCount);

	VkDeviceFaultInfoEXT FaultInfo = {};
	FaultInfo.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
	FaultInfo.pAddressInfos = vAddressInfos.data();
	FaultInfo.pVendorInfos = vVendorInfos.data();
	// We do not request the (potentially large) vendor binary crash dump here.
	// pVendorBinaryData stays null, so the size passed to the driver must be zero.
	FaultCounts.vendorBinarySize = 0;
	if(m_pfnGetDeviceFaultInfoEXT(m_VKDevice, &FaultCounts, &FaultInfo) != VK_SUCCESS)
		return;

	log_error("gfx/vulkan", "Device fault info (VK_EXT_device_fault): %s", FaultInfo.description);
	for(uint32_t i = 0; i < FaultCounts.addressInfoCount; ++i)
	{
		const VkDeviceFaultAddressInfoEXT &Info = vAddressInfos[i];
		log_error("gfx/vulkan", "  address fault: type=%s reportedAddress=0x%" PRIx64 " precision=0x%" PRIx64,
			DeviceFaultAddressTypeName(Info.addressType), (uint64_t)Info.reportedAddress, (uint64_t)Info.addressPrecision);
	}
	for(uint32_t i = 0; i < FaultCounts.vendorInfoCount; ++i)
	{
		const VkDeviceFaultVendorInfoEXT &Info = vVendorInfos[i];
		log_error("gfx/vulkan", "  vendor fault: %s code=0x%" PRIx64 " data=0x%" PRIx64,
			Info.description, (uint64_t)Info.vendorFaultCode, (uint64_t)Info.vendorFaultData);
	}
}

const char *CCommandProcessorFragment_Vulkan::CheckVulkanCriticalError(VkResult CallResult)
{
	const char *pCriticalError = nullptr;
	switch(CallResult)
	{
	case VK_ERROR_OUT_OF_HOST_MEMORY:
		pCriticalError = "Host ran out of memory.";
		log_error("gfx/vulkan", "%s", pCriticalError);
		break;
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		pCriticalError = "Device ran out of memory.";
		log_error("gfx/vulkan", "%s", pCriticalError);
		break;
	case VK_ERROR_DEVICE_LOST:
		pCriticalError = "Device lost.";
		log_error("gfx/vulkan", "%s", pCriticalError);
#ifdef VK_EXT_device_fault
		LogDeviceFaultInfo();
#else
		log_error("gfx/vulkan", "Detailed fault info unavailable: built without VK_EXT_device_fault support (Vulkan headers too old).");
#endif
		break;
	case VK_ERROR_OUT_OF_DATE_KHR:
	{
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Queueing swap chain recreation because the current is out of date.");
		}
		m_RecreateSwapChain = true;
		break;
	}
	case VK_ERROR_SURFACE_LOST_KHR:
		log_error("gfx/vulkan", "Surface lost.");
		break;
	case VK_ERROR_INCOMPATIBLE_DRIVER:
		pCriticalError = "No compatible driver found. Vulkan 1.1 is required.";
		log_error("gfx/vulkan", "%s", pCriticalError);
		break;
	case VK_ERROR_INITIALIZATION_FAILED:
		pCriticalError = "Initialization failed for unknown reason.";
		log_error("gfx/vulkan", "%s", pCriticalError);
		break;
	case VK_ERROR_LAYER_NOT_PRESENT:
		SetWarning(EGfxWarningType::GFX_WARNING_MISSING_EXTENSION, "At least one Vulkan layer was not present. (Try to disable them.)");
		break;
	case VK_ERROR_EXTENSION_NOT_PRESENT:
		SetWarning(EGfxWarningType::GFX_WARNING_MISSING_EXTENSION, "At least one Vulkan extension was not present. (Try to disable them.)");
		break;
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
		log_error("gfx/vulkan", "Native window in use.");
		break;
	case VK_SUCCESS:
		break;
	case VK_SUBOPTIMAL_KHR:
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Queueing swap chain recreation because the current is suboptimal.");
		}
		m_RecreateSwapChain = true;
		break;
	default:
		m_ErrorHelper = "Unknown error: ";
		m_ErrorHelper.append(std::to_string(CallResult));
		pCriticalError = m_ErrorHelper.c_str();
		log_error("gfx/vulkan", "%s", pCriticalError);
		break;
	}

	return pCriticalError;
}

void CCommandProcessorFragment_Vulkan::ErroneousCleanup()
{
	// Nothing will finish these anymore, and the caller of a readback waits
	// until it is told one way or the other.
	for(SReadbackSlot &Slot : m_vReadbackSlots)
		AbandonReadbackSlot(Slot);
	CleanupVulkanDevice();
}

bool CCommandProcessorFragment_Vulkan::WaitForFrameSlot()
{
	if(m_CurImageIndex >= m_vQueueSubmitFences.size())
		return true;
	const VkResult WaitResult = vkWaitForFences(m_VKDevice, 1, &m_vQueueSubmitFences[m_CurImageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max());
	if(WaitResult != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Waiting for the previous frame failed.", CheckVulkanCriticalError(WaitResult));
		return false;
	}
	// The slot's staging memory is about to be given back; an upload sent
	// on its own may still be reading it.
	if(!WaitForMemoryCommandBuffer(m_CurImageIndex))
		return false;
	// The readback that rode on this slot is finished with it, and the slot
	// is about to be overwritten, so this is the last moment to read it.
	return CollectReadbackSlot(m_CurImageIndex);
}

bool CCommandProcessorFragment_Vulkan::FlushRenderCommands()
{
	if(m_HasError)
		return false;
	m_LastPipeline = VK_NULL_HANDLE;
	m_aLastDescriptorSets = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	return true;
}

bool CCommandProcessorFragment_Vulkan::CollectGpuTimestamp(uint32_t ImageIndex)
{
	if(m_GpuTimestampQueryPool == VK_NULL_HANDLE || !m_vGpuTimestampPending[ImageIndex])
		return true;
	m_vGpuTimestampPending[ImageIndex] = false;

	std::array<uint64_t, 2> aTimestamps;
	const VkResult Result = vkGetQueryPoolResults(
		m_VKDevice,
		m_GpuTimestampQueryPool,
		ImageIndex * 2,
		aTimestamps.size(),
		sizeof(aTimestamps),
		aTimestamps.data(),
		sizeof(aTimestamps[0]),
		VK_QUERY_RESULT_64_BIT);
	if(Result == VK_NOT_READY)
	{
		if(!m_GpuTimestampNotReadyWarningLogged)
		{
			log_warn("gfx/vulkan", "GPU timestamp query was not ready after its frame fence completed.");
			m_GpuTimestampNotReadyWarningLogged = true;
		}
		return true;
	}
	if(Result != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Reading GPU timestamp queries failed.", CheckVulkanCriticalError(Result));
		return false;
	}

	const uint64_t DeltaTicks = TimestampTickDelta(aTimestamps[0], aTimestamps[1], m_GpuTimestampValidBits);
	const long double Nanoseconds = static_cast<long double>(DeltaTicks) * m_GpuTimestampPeriod;
	const uint64_t TimeNanoseconds = Nanoseconds >= static_cast<long double>(std::numeric_limits<uint64_t>::max()) ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(Nanoseconds + 0.5L);
	m_pGpuTiming->Publish(TimeNanoseconds);
	return true;
}

bool CCommandProcessorFragment_Vulkan::BeginGpuTimestamp()
{
	m_GpuTimestampRecording = false;
	if(m_GpuTimestampQueryPool == VK_NULL_HANDLE || !m_pGpuTiming->m_Enabled.load(std::memory_order_relaxed))
		return true;

	VkCommandBuffer *pMemoryCommandBuffer;
	if(!GetMemoryCommandBuffer(pMemoryCommandBuffer))
		return false;
	const uint32_t FirstQuery = m_CurImageIndex * 2;
	vkCmdResetQueryPool(*pMemoryCommandBuffer, m_GpuTimestampQueryPool, FirstQuery, 2);
	vkCmdWriteTimestamp(*pMemoryCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_GpuTimestampQueryPool, FirstQuery);
	m_GpuTimestampRecording = true;
	return true;
}

bool CCommandProcessorFragment_Vulkan::EndGpuTimestamp(VkCommandBuffer CommandBuffer)
{
	if(!m_GpuTimestampRecording)
		return false;
	vkCmdWriteTimestamp(CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_GpuTimestampQueryPool, m_CurImageIndex * 2 + 1);
	m_GpuTimestampRecording = false;
	return true;
}

bool CCommandProcessorFragment_Vulkan::SubmitFrameCommands()
{
	if(m_RenderPassActive)
	{
		if(!EndCurrentRenderPass())
			return false;
	}
	else if(!FlushRenderCommands())
		return false;
	// Stream allocations back every recorded pass segment and are reset only at submission.
	UploadNonFlushedBuffers<true>();
	auto &CommandBuffer = GetMainGraphicCommandBuffer();
	const bool HasGpuTimestamp = EndGpuTimestamp(CommandBuffer);

	if(vkEndCommandBuffer(CommandBuffer) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Command buffer cannot be ended anymore.");
		return false;
	}

	VkSubmitInfo SubmitInfo{};
	SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	SubmitInfo.commandBufferCount = 1;
	SubmitInfo.pCommandBuffers = &CommandBuffer;

	std::array<VkCommandBuffer, 2> aCommandBuffers = {};

	if(m_vUsedMemoryCommandBuffer[m_CurImageIndex])
	{
		auto &MemoryCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
		vkEndCommandBuffer(MemoryCommandBuffer);

		aCommandBuffers[0] = MemoryCommandBuffer;
		aCommandBuffers[1] = CommandBuffer;
		SubmitInfo.commandBufferCount = 2;
		SubmitInfo.pCommandBuffers = aCommandBuffers.data();

		m_vUsedMemoryCommandBuffer[m_CurImageIndex] = false;
	}

	std::array<VkSemaphore, 1> aWaitSemaphores = {m_AcquireImageSemaphore};
	std::array<VkPipelineStageFlags, 1> aWaitStages = {(VkPipelineStageFlags)VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	SubmitInfo.waitSemaphoreCount = m_AcquireSemaphorePending ? aWaitSemaphores.size() : 0;
	SubmitInfo.pWaitSemaphores = aWaitSemaphores.data();
	SubmitInfo.pWaitDstStageMask = aWaitStages.data();

	std::array<VkSemaphore, 1> aSignalSemaphores = {m_vQueueSubmitSemaphores[m_CurImageIndex]};
	// Nothing presents this frame while the swapchain is unusable - or when
	// there is no surface at all - so the semaphore would stay signalled and
	// the next submit would signal it again.
	SubmitInfo.signalSemaphoreCount = m_RenderingPaused || !m_Presentation.IsPresentable() ? 0 : aSignalSemaphores.size();
	SubmitInfo.pSignalSemaphores = aSignalSemaphores.data();

	vkResetFences(m_VKDevice, 1, &m_vQueueSubmitFences[m_CurImageIndex]);

	VkResult QueueSubmitRes = vkQueueSubmit(m_VKGraphicsQueue, 1, &SubmitInfo, m_vQueueSubmitFences[m_CurImageIndex]);
	if(QueueSubmitRes != VK_SUCCESS)
	{
		const char *pCritErrorMsg = CheckVulkanCriticalError(QueueSubmitRes);
		if(pCritErrorMsg != nullptr)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Submitting to graphics queue failed.", pCritErrorMsg);
			return false;
		}
	}
	if(QueueSubmitRes == VK_SUCCESS && HasGpuTimestamp)
		m_vGpuTimestampPending[m_CurImageIndex] = true;
	m_AcquireSemaphorePending = false;
	m_FrameCommandsRecording = false;

	return true;
}

bool CCommandProcessorFragment_Vulkan::WaitFrame()
{
	// A readback submits the frame itself, fence and semaphores included, so
	// there is nothing left to submit when one ran.
	if(m_FrameCommandsRecording && !SubmitFrameCommands())
		return false;

	std::swap(m_vBusyAcquireImageSemaphores[m_CurImageIndex], m_AcquireImageSemaphore);

	const std::array<VkSemaphore, 1> aSignalSemaphores = {m_vQueueSubmitSemaphores[m_CurImageIndex]};
	VkPresentInfoKHR PresentInfo{};
	PresentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	PresentInfo.waitSemaphoreCount = aSignalSemaphores.size();
	PresentInfo.pWaitSemaphores = aSignalSemaphores.data();

	std::array<VkSwapchainKHR, 1> aSwapChains = {m_VKSwapChain};
	PresentInfo.swapchainCount = aSwapChains.size();
	PresentInfo.pSwapchains = aSwapChains.data();

	PresentInfo.pImageIndices = &m_CurImageIndex;

	VkResult QueuePresentRes = vkQueuePresentKHR(m_VKPresentQueue, &PresentInfo);
	if(QueuePresentRes != VK_SUCCESS && QueuePresentRes != VK_SUBOPTIMAL_KHR)
	{
		const char *pCritErrorMsg = CheckVulkanCriticalError(QueuePresentRes);
		if(pCritErrorMsg != nullptr)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_SWAP_FAILED, "Presenting graphics queue failed.", pCritErrorMsg);
			return false;
		}
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::PrepareFrame()
{
	if(m_RecreateSwapChain)
	{
		m_RecreateSwapChain = false;
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Recreating swap chain requested by user (prepare frame).");
		}
		if(RecreateSwapChain() != 0)
			return false;
		// Start recording even though there is no image. A paused frame draws
		// into render targets, and without this the first one would record
		// into a command buffer that the last submit already ended.
		if(m_RenderingPaused)
			return !m_SwapchainCreated || BeginFrameCommands();
	}

	auto AcqResult = vkAcquireNextImageKHR(m_VKDevice, m_VKSwapChain, std::numeric_limits<uint64_t>::max(), m_AcquireImageSemaphore, VK_NULL_HANDLE, &m_CurImageIndex);
	if(AcqResult != VK_SUCCESS)
	{
		if(AcqResult == VK_ERROR_OUT_OF_DATE_KHR || m_RecreateSwapChain)
		{
			m_RecreateSwapChain = false;
			if(IsVerbose())
			{
				log_debug("gfx/vulkan", "Recreating swap chain requested by acquire next image (prepare frame).");
			}
			if(RecreateSwapChain() != 0)
				return false;
			if(m_RenderingPaused)
				return !m_SwapchainCreated || BeginFrameCommands();
			return PrepareFrame();
		}
		else
		{
			const char *pCritErrorMsg = CheckVulkanCriticalError(AcqResult);
			if(pCritErrorMsg != nullptr)
			{
				SetError(EGfxErrorType::GFX_ERROR_TYPE_SWAP_FAILED, "Acquiring next image failed.", pCritErrorMsg);
				return false;
			}
			else if(AcqResult == VK_ERROR_SURFACE_LOST_KHR)
			{
				m_RenderingPaused = true;
				return !m_SwapchainCreated || BeginFrameCommands();
			}
		}
	}

	m_AcquireSemaphorePending = true;
	if(!BeginFrameCommands())
		return false;

	IGraphics::CRenderPassDesc Pass;
	Pass.m_LoadOp = IGraphics::ERenderPassLoadOp::CLEAR;
	Pass.m_ClearColor = {m_aClearColor[0], m_aClearColor[1], m_aClearColor[2], m_aClearColor[3]};
	return BeginCurrentRenderPass(Pass);
}

bool CCommandProcessorFragment_Vulkan::BeginFrameCommands()
{
	if(!WaitForFrameSlot())
		return false;
	if(!CollectGpuTimestamp(m_CurImageIndex))
		return false;

	// next frame
	m_CurFrame++;
	m_vImageLastFrameCheck[m_CurImageIndex] = m_CurFrame;

	// check if older frames weren't used in a long time
	for(size_t FrameImageIndex = 0; FrameImageIndex < m_vImageLastFrameCheck.size(); ++FrameImageIndex)
	{
		auto LastFrame = m_vImageLastFrameCheck[FrameImageIndex];
		if(m_CurFrame - LastFrame > (uint64_t)m_SwapChainImageCount)
		{
			vkWaitForFences(m_VKDevice, 1, &m_vQueueSubmitFences[FrameImageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max());
			if(!WaitForMemoryCommandBuffer(FrameImageIndex) || !CollectReadbackSlot(FrameImageIndex))
				return false;
			ClearFrameData(FrameImageIndex);
			m_vImageLastFrameCheck[FrameImageIndex] = m_CurFrame;
		}
	}

	// This slot's previous GPU use is complete, so its retired resources can
	// now be destroyed or returned to the backend caches.
	ClearFrameMemoryUsage();

	// clear frame
	vkResetCommandBuffer(GetMainGraphicCommandBuffer(), VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

	auto &CommandBuffer = GetMainGraphicCommandBuffer();
	VkCommandBufferBeginInfo BeginInfo{};
	BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	if(vkBeginCommandBuffer(CommandBuffer, &BeginInfo) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Command buffer cannot be filled anymore.");
		return false;
	}
	if(!BeginGpuTimestamp())
		return false;
	m_FrameCommandsRecording = true;
	return true;
}

bool CCommandProcessorFragment_Vulkan::SubmitOffscreenFrame()
{
	if(!m_FrameCommandsRecording)
		return true;
	if(!SubmitFrameCommands())
		return false;
	m_CurImageIndex = (m_CurImageIndex + 1) % m_SwapChainImageCount;
	return PrepareOffscreenCommands();
}

bool CCommandProcessorFragment_Vulkan::PrepareOffscreenCommands()
{
	if(!WaitForFrameSlot())
		return false;
	// This slot's previous GPU use is complete, so its retired resources can
	// now be destroyed or returned to the backend caches.
	ClearFrameMemoryUsage();
	auto &CommandBuffer = GetMainGraphicCommandBuffer();
	if(vkResetCommandBuffer(CommandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Resetting the offscreen command buffer failed.");
		return false;
	}
	VkCommandBufferBeginInfo BeginInfo{};
	BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if(vkBeginCommandBuffer(CommandBuffer, &BeginInfo) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Beginning the offscreen command buffer failed.");
		return false;
	}
	m_FrameCommandsRecording = true;
	return true;
}

bool CCommandProcessorFragment_Vulkan::ResumeRendering()
{
	if(!m_RenderingPaused)
		return true;
	m_RenderingPaused = false;
	if(!PureMemoryFrame())
		return false;
	return PrepareFrame();
}

bool CCommandProcessorFragment_Vulkan::NextFrame()
{
	if(!m_RenderingPaused)
	{
		if(!WaitFrame())
			return false;
		return PrepareFrame();
	}

	// A minimized window has no swapchain image to draw into, but the device
	// keeps working. Render target work is submitted anyway, so a video
	// export or a screenshot still finishes while minimized; only the
	// swapchain image and the present are skipped.
	if(m_FrameCommandsRecording && !SubmitFrameCommands())
		return false;
	if(m_SwapchainRecreationDeferred)
	{
		if(!ResumeRendering())
			return false;
		if(!m_RenderingPaused)
			return true;
	}
	if(!PureMemoryFrame())
		return false;
	// A resume that paused again already started recording, and the window
	// destroy path can leave the swapchain and its per image command buffers
	// gone, which is what a frame records into.
	if(m_FrameCommandsRecording || !m_SwapchainCreated)
		return true;
	return BeginFrameCommands();
}

VKAPI_ATTR VkBool32 VKAPI_CALL CCommandProcessorFragment_Vulkan::VKDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT MessageSeverity, VkDebugUtilsMessageTypeFlagsEXT MessageType, const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
	if((MessageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
	{
		log_error("gfx/vulkan", "Validation error: %s", pCallbackData->pMessage);
	}
	else
	{
		log_info("gfx/vulkan", "Validation info: %s", pCallbackData->pMessage);
	}

	return VK_FALSE;
}

VkResult CCommandProcessorFragment_Vulkan::CreateDebugUtilsMessengerEXT(const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDebugUtilsMessengerEXT *pDebugMessenger)
{
	auto pfnVulkanCreateDebugUtilsFunction = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_VKInstance, "vkCreateDebugUtilsMessengerEXT");
	if(pfnVulkanCreateDebugUtilsFunction != nullptr)
	{
		return pfnVulkanCreateDebugUtilsFunction(m_VKInstance, pCreateInfo, pAllocator, pDebugMessenger);
	}
	else
	{
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
}

void CCommandProcessorFragment_Vulkan::DestroyDebugUtilsMessengerEXT(VkDebugUtilsMessengerEXT &DebugMessenger)
{
	auto pfnVulkanDestroyDebugUtilsFunction = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_VKInstance, "vkDestroyDebugUtilsMessengerEXT");
	if(pfnVulkanDestroyDebugUtilsFunction != nullptr)
	{
		pfnVulkanDestroyDebugUtilsFunction(m_VKInstance, DebugMessenger, nullptr);
	}
}

void CCommandProcessorFragment_Vulkan::SetupDebugCallback()
{
#ifdef VK_EXT_debug_utils
	VkDebugUtilsMessengerCreateInfoEXT CreateInfo = {};
	CreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	CreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	CreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; // | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT <- too annoying
	CreateInfo.pfnUserCallback = VKDebugCallback;

	if(CreateDebugUtilsMessengerEXT(&CreateInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS)
	{
		m_DebugMessenger = VK_NULL_HANDLE;
		log_warn("gfx/vulkan", "Could not find Vulkan debug layer.");
	}
	else
	{
		log_info("gfx/vulkan", "Enabled Vulkan debug context.");
	}
#endif
}

void CCommandProcessorFragment_Vulkan::UnregisterDebugCallback()
{
#ifdef VK_EXT_debug_utils
	if(m_DebugMessenger != VK_NULL_HANDLE)
		DestroyDebugUtilsMessengerEXT(m_DebugMessenger);
#endif
}

bool CCommandProcessorFragment_Vulkan::CreateCommandPool()
{
	VkCommandPoolCreateInfo CreatePoolInfo{};
	CreatePoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	CreatePoolInfo.queueFamilyIndex = m_VKGraphicsQueueIndex;
	CreatePoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	if(vkCreateCommandPool(m_VKDevice, &CreatePoolInfo, nullptr, &m_CommandPool) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the command pool failed.");
		return false;
	}
	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyCommandPool()
{
	vkDestroyCommandPool(m_VKDevice, m_CommandPool, nullptr);
	m_CommandPool = VK_NULL_HANDLE;
}

bool CCommandProcessorFragment_Vulkan::CreateCommandBuffers()
{
	m_vMainDrawCommandBuffers.resize(m_SwapChainImageCount);
	m_vMemoryCommandBuffers.resize(m_SwapChainImageCount);
	m_vUsedMemoryCommandBuffer.resize(m_SwapChainImageCount, false);

	VkCommandBufferAllocateInfo AllocInfo{};
	AllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	AllocInfo.commandPool = m_CommandPool;
	AllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	AllocInfo.commandBufferCount = (uint32_t)m_vMainDrawCommandBuffers.size();

	if(vkAllocateCommandBuffers(m_VKDevice, &AllocInfo, m_vMainDrawCommandBuffers.data()) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Allocating command buffers failed.");
		return false;
	}

	AllocInfo.commandBufferCount = (uint32_t)m_vMemoryCommandBuffers.size();

	if(vkAllocateCommandBuffers(m_VKDevice, &AllocInfo, m_vMemoryCommandBuffers.data()) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Allocating memory command buffers failed.");
		return false;
	}

	m_vMemoryCommandBufferFences.resize(m_SwapChainImageCount, VK_NULL_HANDLE);
	m_vMemoryCommandBufferPending.resize(m_SwapChainImageCount, false);
	VkFenceCreateInfo FenceInfo{};
	FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for(auto &Fence : m_vMemoryCommandBufferFences)
	{
		if(vkCreateFence(m_VKDevice, &FenceInfo, nullptr, &Fence) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the memory command buffer fences failed.");
			return false;
		}
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyCommandBuffer()
{
	// Nothing is in flight here: the device was waited for before.
	for(auto &Fence : m_vMemoryCommandBufferFences)
	{
		if(Fence != VK_NULL_HANDLE)
			vkDestroyFence(m_VKDevice, Fence, nullptr);
	}
	m_vMemoryCommandBufferFences.clear();
	m_vMemoryCommandBufferPending.clear();

	vkFreeCommandBuffers(m_VKDevice, m_CommandPool, static_cast<uint32_t>(m_vMemoryCommandBuffers.size()), m_vMemoryCommandBuffers.data());
	vkFreeCommandBuffers(m_VKDevice, m_CommandPool, static_cast<uint32_t>(m_vMainDrawCommandBuffers.size()), m_vMainDrawCommandBuffers.data());

	m_vMainDrawCommandBuffers.clear();
	m_vMemoryCommandBuffers.clear();
	m_vUsedMemoryCommandBuffer.clear();
}

bool CCommandProcessorFragment_Vulkan::CreateSyncObjects()
{
	auto SyncObjectCount = m_SwapChainImageCount;
	m_vQueueSubmitSemaphores.resize(SyncObjectCount);
	m_vBusyAcquireImageSemaphores.resize(SyncObjectCount);

	m_vQueueSubmitFences.resize(SyncObjectCount);

	VkSemaphoreCreateInfo CreateSemaphoreInfo{};
	CreateSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo FenceInfo{};
	FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	if(vkCreateSemaphore(m_VKDevice, &CreateSemaphoreInfo, nullptr, &m_AcquireImageSemaphore) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating acquire next image semaphore failed.");
		return false;
	}
	for(size_t i = 0; i < SyncObjectCount; i++)
	{
		if(vkCreateSemaphore(m_VKDevice, &CreateSemaphoreInfo, nullptr, &m_vQueueSubmitSemaphores[i]) != VK_SUCCESS ||
			vkCreateSemaphore(m_VKDevice, &CreateSemaphoreInfo, nullptr, &m_vBusyAcquireImageSemaphores[i]) != VK_SUCCESS ||
			vkCreateFence(m_VKDevice, &FenceInfo, nullptr, &m_vQueueSubmitFences[i]) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating swap chain sync objects(fences, semaphores) failed.");
			return false;
		}
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroySyncObjects()
{
	for(size_t i = 0; i < m_vBusyAcquireImageSemaphores.size(); i++)
	{
		vkDestroySemaphore(m_VKDevice, m_vBusyAcquireImageSemaphores[i], nullptr);
		vkDestroySemaphore(m_VKDevice, m_vQueueSubmitSemaphores[i], nullptr);
		vkDestroyFence(m_VKDevice, m_vQueueSubmitFences[i], nullptr);
	}
	vkDestroySemaphore(m_VKDevice, m_AcquireImageSemaphore, nullptr);

	m_vBusyAcquireImageSemaphores.clear();
	m_vQueueSubmitSemaphores.clear();

	m_vQueueSubmitFences.clear();
}

void CCommandProcessorFragment_Vulkan::CreateGpuTimestampQueries()
{
	m_vGpuTimestampPending.assign(m_SwapChainImageCount, false);
	m_GpuTimestampRecording = false;
	m_GpuTimestampNotReadyWarningLogged = false;
	if(m_pGpuTiming == nullptr || !m_Presentation.IsPresentable() || m_GpuTimestampValidBits == 0 || !(m_GpuTimestampPeriod > 0.0f))
		return;

	VkQueryPoolCreateInfo CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	CreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	CreateInfo.queryCount = m_SwapChainImageCount * 2;
	const VkResult Result = vkCreateQueryPool(m_VKDevice, &CreateInfo, nullptr, &m_GpuTimestampQueryPool);
	if(Result != VK_SUCCESS)
	{
		log_warn("gfx/vulkan", "Creating GPU timestamp query pool failed (%d).", static_cast<int>(Result));
		return;
	}
	m_pGpuTiming->m_Supported.store(true, std::memory_order_relaxed);
}

void CCommandProcessorFragment_Vulkan::DestroyGpuTimestampQueries()
{
	if(m_GpuTimestampQueryPool != VK_NULL_HANDLE)
		vkDestroyQueryPool(m_VKDevice, m_GpuTimestampQueryPool, nullptr);
	m_GpuTimestampQueryPool = VK_NULL_HANDLE;
	m_vGpuTimestampPending.clear();
	m_GpuTimestampRecording = false;
	if(m_pGpuTiming != nullptr)
		m_pGpuTiming->m_Supported.store(false, std::memory_order_relaxed);
}

int CCommandProcessorFragment_Vulkan::InitVulkanOffscreenResources()
{
	// Frames pipeline here as they do on a swapchain: a readback submits the
	// frame it belongs to and moves on, so the next one records into another
	// slot instead of waiting for the copy of the last.
	m_SwapChainImageCount = OFFSCREEN_FRAME_SLOT_COUNT;
	m_CurImageIndex = 0;
	if(!CreateRenderPass(m_VKRenderTargetPass, RENDER_TARGET_FORMAT, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
		!CreateRenderPass(m_VKRenderTargetPassDiscard, RENDER_TARGET_FORMAT, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
		!CreateGraphicsPipelines())
		return -1;
	return 0;
}

VkCommandBuffer &CCommandProcessorFragment_Vulkan::GetMainGraphicCommandBuffer()
{
	return m_vMainDrawCommandBuffers[m_CurImageIndex];
}

bool CCommandProcessorFragment_Vulkan::IsRenderCommandValid(const CCommandBuffer::SCommand *pCommand) const
{
	const CCommandBuffer::SState *pState = RenderCommandState(pCommand);
	if(pState != nullptr && pState->m_Texture.IsValid() && !m_TextureHandles.IsActive(pState->m_Texture))
		return false;

	const IGraphics::CBufferHandle *pVertexBuffer = RenderCommandVertexBuffer(pCommand);
	if(pVertexBuffer != nullptr && !m_BufferHandles.IsActive(*pVertexBuffer))
		return false;

	const IGraphics::CBufferHandle *pIndexBuffer = RenderCommandIndexBuffer(pCommand);
	if(pIndexBuffer != nullptr && (!m_BufferHandles.IsActive(*pIndexBuffer) || m_vBufferObjects[pIndexBuffer->Id()].m_Usage != IGraphics::EBufferUsage::INDEX))
		return false;

	const std::optional<EPipelineProgram> Program = RenderCommandProgram(pCommand);
	if(Program.has_value() && *Program >= EPipelineProgram::COUNT)
		return false;

	if(pCommand->m_Cmd == CCommandBuffer::CMD_DRAW_INDEXED)
	{
		const auto *pDrawCommand = static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand);
		if(pDrawCommand->m_Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE && !pDrawCommand->m_State.m_Texture.IsValid())
			return false;
	}
	return true;
}

ERunCommandReturnTypes CCommandProcessorFragment_Vulkan::RunCommand(const CCommandBuffer::SCommand *pBaseCommand)
{
	if(m_HasError)
	{
		// ignore all further commands
		return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_ERROR;
	}

	auto CommandResult = [this](bool Success, bool Handled = true) {
		if(!Success)
		{
			if(!m_HasError)
				SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_CMD_FAILED, "Executing a render command failed.");
			return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_ERROR;
		}
		return Handled ? ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED : ERunCommandReturnTypes::RUN_COMMAND_COMMAND_UNHANDLED;
	};

	// Cheap and unconditional: a readback whose frame is already finished is
	// handed over here, so a caller polling with IsReady never has to send
	// anything to find out.
	if(!CollectFinishedReadbacks())
		return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_ERROR;

	if(!m_Presentation.IsPresentable())
	{
		switch(pBaseCommand->m_Cmd)
		{
		// A swap without a surface presents nothing, but it is still where
		// the frame ends: the recording is submitted and the next frame
		// starts on the next slot. Without that the surface-less client
		// records every frame it ever drew into one command buffer.
		case CCommandBuffer::CMD_SWAP:
			return CommandResult(SubmitOffscreenFrame());
		// Without a surface these have nothing to act on, but they are
		// still commands this backend knows. Reporting them as unhandled
		// made the command processor abort with "Unknown graphics command"
		// as soon as anything set a viewport or asked for vsync.
		case CCommandBuffer::CMD_MULTISAMPLING:
		case CCommandBuffer::CMD_VSYNC:
		case CCommandBuffer::CMD_PRESENTATION_TARGET_READBACK:
		case CCommandBuffer::CMD_UPDATE_VIEWPORT:
		case CCommandBuffer::CMD_WINDOW_CREATE_NTF:
		case CCommandBuffer::CMD_WINDOW_DESTROY_NTF:
			return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED;
		}
	}

	switch(pBaseCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_SIGNAL: return RUN_COMMAND_COMMAND_HANDLED;
	case CCommandBuffer::CMD_FINISH_READBACKS: return CommandResult(FinishReadbacks());
	case CCommandBuffer::CMD_TEXTURE_CREATE: return CommandResult(Cmd_Texture_Create(static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand)));
	case CCommandBuffer::CMD_TEXTURE_DESTROY: return CommandResult(Cmd_Texture_Destroy(static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand)));
	case CCommandBuffer::CMD_TEXTURE_UPDATE: return CommandResult(Cmd_Texture_Update(static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand)));
	case CCommandBuffer::CMD_TEXTURE_READBACK: return CommandResult(Cmd_Texture_Readback(static_cast<const CCommandBuffer::SCommand_Texture_Readback *>(pBaseCommand)));
	case CCommandBuffer::CMD_BEGIN_RENDER_PASS: return CommandResult(Cmd_BeginRenderPass(static_cast<const CCommandBuffer::SCommand_BeginRenderPass *>(pBaseCommand)));
	case CCommandBuffer::CMD_END_RENDER_PASS: return CommandResult(Cmd_EndRenderPass(static_cast<const CCommandBuffer::SCommand_EndRenderPass *>(pBaseCommand)));
	case CCommandBuffer::CMD_FLUSH_RENDER_PASS: return CommandResult(Cmd_FlushRenderPass(static_cast<const CCommandBuffer::SCommand_FlushRenderPass *>(pBaseCommand)));
	case CCommandBuffer::CMD_CLEAR:
	{
		if(!m_RenderPassActive || !IsRenderCommandValid(pBaseCommand))
			return RUN_COMMAND_COMMAND_HANDLED;
		return CommandResult(Cmd_Clear(static_cast<const CCommandBuffer::SCommand_Clear *>(pBaseCommand)));
	}
	case CCommandBuffer::CMD_DRAW:
	{
		if(!m_RenderPassActive || !IsRenderCommandValid(pBaseCommand))
			return RUN_COMMAND_COMMAND_HANDLED;
		return CommandResult(Cmd_Draw(static_cast<const CCommandBuffer::SCommand_Draw *>(pBaseCommand)));
	}
	case CCommandBuffer::CMD_CREATE_BUFFER_OBJECT: return CommandResult(Cmd_CreateBufferObject(static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(pBaseCommand)));
	case CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT: return CommandResult(Cmd_RecreateBufferObject(static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand)));
	case CCommandBuffer::CMD_DELETE_BUFFER_OBJECT: return CommandResult(Cmd_DeleteBufferObject(static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand)));
	case CCommandBuffer::CMD_DRAW_INDEXED:
	{
		if(!m_RenderPassActive || !IsRenderCommandValid(pBaseCommand))
			return RUN_COMMAND_COMMAND_HANDLED;
		return CommandResult(Cmd_DrawIndexed(static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pBaseCommand)));
	}
	case CCommandBuffer::CMD_SWAP: return CommandResult(Cmd_Swap(static_cast<const CCommandBuffer::SCommand_Swap *>(pBaseCommand)));
	case CCommandBuffer::CMD_MULTISAMPLING: return CommandResult(Cmd_MultiSampling(static_cast<const CCommandBuffer::SCommand_MultiSampling *>(pBaseCommand)));
	case CCommandBuffer::CMD_VSYNC: return CommandResult(Cmd_VSync(static_cast<const CCommandBuffer::SCommand_VSync *>(pBaseCommand)));
	case CCommandBuffer::CMD_PRESENTATION_TARGET_READBACK: return CommandResult(Cmd_PresentationTargetReadback(static_cast<const CCommandBuffer::SCommand_PresentationTarget_Readback *>(pBaseCommand)));
	case CCommandBuffer::CMD_UPDATE_VIEWPORT: return CommandResult(Cmd_Update_Viewport(static_cast<const CCommandBuffer::SCommand_Update_Viewport *>(pBaseCommand)));
	case CCommandBuffer::CMD_WINDOW_CREATE_NTF: return CommandResult(Cmd_WindowCreateNtf(static_cast<const CCommandBuffer::SCommand_WindowCreateNtf *>(pBaseCommand)), false);
	case CCommandBuffer::CMD_WINDOW_DESTROY_NTF: return CommandResult(Cmd_WindowDestroyNtf(static_cast<const CCommandBuffer::SCommand_WindowDestroyNtf *>(pBaseCommand)), false);
	}

	switch(pBaseCommand->m_Cmd)
	{
	case CCommandProcessorFragment_Renderer::CMD_INIT:
		if(!Cmd_Init(static_cast<const SCommand_Init *>(pBaseCommand)))
		{
			SetWarningPreMsg("Could not initialize Vulkan: ");
			return RUN_COMMAND_COMMAND_WARNING;
		}
		break;
	case CCommandProcessorFragment_Renderer::CMD_SHUTDOWN:
		if(!Cmd_Shutdown(static_cast<const SCommand_Shutdown *>(pBaseCommand)))
		{
			SetWarningPreMsg("Could not shutdown Vulkan: ");
			return RUN_COMMAND_COMMAND_WARNING;
		}
		break;

	case CCommandProcessorFragment_Renderer::CMD_PRE_INIT:
		if(!Cmd_PreInit(static_cast<const CCommandProcessorFragment_Renderer::SCommand_PreInit *>(pBaseCommand)))
		{
			SetWarningPreMsg("Could not initialize Vulkan: ");
			return RUN_COMMAND_COMMAND_WARNING;
		}
		break;
	case CCommandProcessorFragment_Renderer::CMD_POST_SHUTDOWN:
		if(!Cmd_PostShutdown(static_cast<const CCommandProcessorFragment_Renderer::SCommand_PostShutdown *>(pBaseCommand)))
		{
			SetWarningPreMsg("Could not shutdown Vulkan: ");
			return RUN_COMMAND_COMMAND_WARNING;
		}
		break;
	default:
		return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_UNHANDLED;
	}

	return ERunCommandReturnTypes::RUN_COMMAND_COMMAND_HANDLED;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Init(const SCommand_Init *pCommand)
{
	m_pGpuTiming = pCommand->m_pGpuTiming;
	if(m_pGpuTiming != nullptr)
	{
		m_pGpuTiming->m_Supported.store(false, std::memory_order_relaxed);
		m_pGpuTiming->m_TimeNanoseconds.store(0, std::memory_order_relaxed);
		m_pGpuTiming->m_Sample.store(0, std::memory_order_relaxed);
	}
	m_TextureHandles.Clear();
	m_BufferHandles.Clear();

	pCommand->m_pCapabilities->m_RenderTargets = true;

	pCommand->m_pCapabilities->m_2DArrayTextures = true;

	pCommand->m_pCapabilities->m_ContextMajor = 1;
	pCommand->m_pCapabilities->m_ContextMinor = 1;
	pCommand->m_pCapabilities->m_ContextPatch = 0;

	m_GlobalTextureLodBIAS = g_Config.m_GfxGLTextureLODBIAS;
	m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
	m_pBufferMemoryUsage = pCommand->m_pBufferMemoryUsage;
	m_pStreamMemoryUsage = pCommand->m_pStreamMemoryUsage;
	m_pStagingMemoryUsage = pCommand->m_pStagingMemoryUsage;
	m_pTextureMemoryUsage->store(0, std::memory_order_relaxed);
	m_pBufferMemoryUsage->store(0, std::memory_order_relaxed);
	m_pStreamMemoryUsage->store(0, std::memory_order_relaxed);
	m_pStagingMemoryUsage->store(0, std::memory_order_relaxed);

	m_MultiSamplingCount = (g_Config.m_GfxFsaaSamples & 0xFFFFFFFE); // ignore the uneven bit, only even multi sampling works

	*pCommand->m_pInitError = m_VKInstance != VK_NULL_HANDLE ? 0 : -1;

	if(m_VKInstance == VK_NULL_HANDLE)
	{
		*pCommand->m_pInitError = -2;
		return false;
	}

	if(InitVulkan<true>() != 0)
	{
		*pCommand->m_pInitError = -2;
		return false;
	}

	if(!(m_Presentation.IsPresentable() ? PrepareFrame() : PrepareOffscreenCommands()))
		return false;
	if(m_HasError)
	{
		*pCommand->m_pInitError = -2;
		return false;
	}

	m_CanAssert = true;

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Shutdown(const SCommand_Shutdown *pCommand)
{
	vkDeviceWaitIdle(m_VKDevice);
	CleanupVulkan<true>(m_SwapChainImageCount);
	m_TextureHandles.Clear();
	m_BufferHandles.Clear();

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Swap(const CCommandBuffer::SCommand_Swap *pCommand)
{
	return NextFrame();
}

bool CCommandProcessorFragment_Vulkan::Cmd_PreInit(const CCommandProcessorFragment_Renderer::SCommand_PreInit *pCommand)
{
	m_pGpuList = pCommand->m_pGpuList;
	if(InitVulkanDevice(pCommand->m_Surface, pCommand->m_pRendererString, pCommand->m_pVendorString, pCommand->m_pVersionString) != 0)
	{
		m_VKInstance = VK_NULL_HANDLE;
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_PostShutdown(const CCommandProcessorFragment_Renderer::SCommand_PostShutdown *pCommand)
{
	CleanupVulkanDevice();

	return true;
}

// ---------------------------------------------------------------------------
// Instance, device, surface and swapchain: everything a window changes.
// ---------------------------------------------------------------------------

bool CCommandProcessorFragment_Vulkan::GetVulkanExtensions(std::vector<std::string> &vVKExtensions)
{
	// The window system knows which extensions its surface needs.
	if(!m_Presentation.m_pSurface->VulkanInstanceExtensions(vVKExtensions))
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Could not get the instance extensions the window needs.");
		return false;
	}
	return true;
}

std::set<std::string> CCommandProcessorFragment_Vulkan::OurVKLayers()
{
	std::set<std::string> OurLayers;

	if(g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL)
	{
		OurLayers.emplace("VK_LAYER_KHRONOS_validation");
		// deprecated, but VK_LAYER_KHRONOS_validation was released after vulkan 1.1
		OurLayers.emplace("VK_LAYER_LUNARG_standard_validation");
	}

	return OurLayers;
}

std::set<std::string> CCommandProcessorFragment_Vulkan::OurDeviceExtensions() const
{
	std::set<std::string> OurExt;
	if(m_Presentation.IsPresentable())
		OurExt.emplace(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef VK_EXT_device_fault
	// Only used when actually supported by the device (see device creation);
	// enables detailed diagnostics after a VK_ERROR_DEVICE_LOST.
	OurExt.emplace(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
#endif
	return OurExt;
}

bool CCommandProcessorFragment_Vulkan::GetVulkanLayers(std::vector<std::string> &vVKLayers)
{
	uint32_t LayerCount = 0;
	VkResult Res = vkEnumerateInstanceLayerProperties(&LayerCount, nullptr);
	if(Res != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Could not get Vulkan layers.");
		return false;
	}

	std::vector<VkLayerProperties> vVKInstanceLayers(LayerCount);
	Res = vkEnumerateInstanceLayerProperties(&LayerCount, vVKInstanceLayers.data());
	if(Res != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Could not get Vulkan layers.");
		return false;
	}

	std::set<std::string> ReqLayerNames = OurVKLayers();
	vVKLayers.clear();
	for(const auto &LayerName : vVKInstanceLayers)
	{
		if(ReqLayerNames.contains(std::string(LayerName.layerName)))
		{
			vVKLayers.emplace_back(LayerName.layerName);
		}
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::IsGpuDenied(uint32_t Vendor, uint32_t DriverVersion, uint32_t ApiMajor, uint32_t ApiMinor, uint32_t ApiPatch)
{
#ifdef CONF_FAMILY_WINDOWS
	// AMD
	if(0x1002 == Vendor)
	{
		auto Major = (DriverVersion >> 22);
		auto Minor = (DriverVersion >> 12) & 0x3ff;
		auto Patch = DriverVersion & 0xfff;

		return Major == 2 && Minor == 0 && Patch > 137 && Patch < 220 && ((ApiMajor <= 1 && ApiMinor < 3) || (ApiMajor <= 1 && ApiMinor == 3 && ApiPatch < 206));
	}
#endif
	return false;
}

bool CCommandProcessorFragment_Vulkan::CreateVulkanInstance(const std::vector<std::string> &vVKLayers, const std::vector<std::string> &vVKExtensions, bool TryDebugExtensions)
{
	std::vector<const char *> vLayersCStr;
	vLayersCStr.reserve(vVKLayers.size());
	for(const auto &Layer : vVKLayers)
		vLayersCStr.emplace_back(Layer.c_str());

	std::vector<const char *> vExtCStr;
	vExtCStr.reserve(vVKExtensions.size() + 1);
	for(const auto &Ext : vVKExtensions)
		vExtCStr.emplace_back(Ext.c_str());

#ifdef VK_EXT_debug_utils
	if(TryDebugExtensions && (g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL))
	{
		// debug message support
		vExtCStr.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}
#endif

	VkApplicationInfo VKAppInfo = {};
	VKAppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	VKAppInfo.pNext = nullptr;
	VKAppInfo.pApplicationName = "DDNet";
	VKAppInfo.applicationVersion = 1;
	VKAppInfo.pEngineName = "DDNet-Vulkan";
	VKAppInfo.engineVersion = 1;
	VKAppInfo.apiVersion = VK_API_VERSION_1_1;

	void *pExt = nullptr;
#if defined(VK_EXT_validation_features) && VK_EXT_VALIDATION_FEATURES_SPEC_VERSION >= 5
	VkValidationFeaturesEXT Features = {};
	std::array<VkValidationFeatureEnableEXT, 2> aEnables = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT, VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
	if(TryDebugExtensions && (g_Config.m_DbgGfx == DEBUG_GFX_MODE_AFFECTS_PERFORMANCE || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL))
	{
		Features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
		Features.enabledValidationFeatureCount = aEnables.size();
		Features.pEnabledValidationFeatures = aEnables.data();

		pExt = &Features;
	}
#endif

	VkInstanceCreateInfo VKInstanceInfo = {};
	VKInstanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	VKInstanceInfo.pNext = pExt;
	VKInstanceInfo.flags = 0;
	VKInstanceInfo.pApplicationInfo = &VKAppInfo;
	VKInstanceInfo.enabledExtensionCount = static_cast<uint32_t>(vExtCStr.size());
	VKInstanceInfo.ppEnabledExtensionNames = vExtCStr.data();
	VKInstanceInfo.enabledLayerCount = static_cast<uint32_t>(vLayersCStr.size());
	VKInstanceInfo.ppEnabledLayerNames = vLayersCStr.data();

	bool TryAgain = false;

	VkResult Res = vkCreateInstance(&VKInstanceInfo, nullptr, &m_VKInstance);
	const char *pCritErrorMsg = CheckVulkanCriticalError(Res);
	if(pCritErrorMsg != nullptr)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating instance failed.", pCritErrorMsg);
		return false;
	}
	else if(Res == VK_ERROR_LAYER_NOT_PRESENT || Res == VK_ERROR_EXTENSION_NOT_PRESENT)
	{
		TryAgain = true;
	}

	if(TryAgain && TryDebugExtensions)
		return CreateVulkanInstance(vVKLayers, vVKExtensions, false);

	return true;
}

STWGraphicGpu::ETWGraphicsGpuType CCommandProcessorFragment_Vulkan::VKGPUTypeToGraphicsGpuType(VkPhysicalDeviceType VKGPUType)
{
	if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		return STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_DISCRETE;
	else if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
		return STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_INTEGRATED;
	else if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
		return STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_VIRTUAL;
	else if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_CPU)
		return STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_CPU;

	return STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_CPU;
}

void CCommandProcessorFragment_Vulkan::GetVendorString(uint32_t VendorId, char *pVendorStr, size_t Size)
{
	switch(VendorId)
	{
	case 0x1002:
	case 0x1022:
		str_copy(pVendorStr, "AMD", Size);
		break;
	case 0x1010:
		str_copy(pVendorStr, "ImgTec", Size);
		break;
	case 0x106B:
		str_copy(pVendorStr, "Apple", Size);
		break;
	case 0x10DE:
		str_copy(pVendorStr, "NVIDIA", Size);
		break;
	case 0x13B5:
		str_copy(pVendorStr, "ARM", Size);
		break;
	case 0x5143:
		str_copy(pVendorStr, "Qualcomm", Size);
		break;
	case 0x8086:
		str_copy(pVendorStr, "Intel", Size);
		break;
	case 0x10005:
		str_copy(pVendorStr, "Mesa", Size);
		break;
	default:
		log_warn("gfx/vulkan", "Unknown GPU vendor ID %08X.", VendorId);
		str_format(pVendorStr, Size, "Unknown (%08X)", VendorId);
		break;
	}
}

void CCommandProcessorFragment_Vulkan::FormatDriverVersion(char (&aDriverVersion)[256], uint32_t DriverVersion, uint32_t VendorId)
{
	if(VendorId == 0x10DE) // NVIDIA
	{
		str_format(aDriverVersion, std::size(aDriverVersion), "%d.%d.%d.%d",
			(DriverVersion >> 22) & 0x3ff,
			(DriverVersion >> 14) & 0x0ff,
			(DriverVersion >> 6) & 0x0ff,
			(DriverVersion) & 0x003f);
	}
#ifdef CONF_FAMILY_WINDOWS
	else if(VendorId == 0x8086) // Windows with Intel only
	{
		str_format(aDriverVersion, std::size(aDriverVersion),
			"%d.%d",
			(DriverVersion >> 14),
			(DriverVersion) & 0x3fff);
	}
#endif
	else
	{
		// Use Vulkan version conventions if vendor mapping is not available
		str_format(aDriverVersion, std::size(aDriverVersion),
			"%d.%d.%d",
			(DriverVersion >> 22),
			(DriverVersion >> 12) & 0x3ff,
			DriverVersion & 0xfff);
	}
}

bool CCommandProcessorFragment_Vulkan::SelectGpu(char *pRendererName, char *pVendorName, char *pVersionName)
{
	uint32_t DevicesCount = 0;
	auto Res = vkEnumeratePhysicalDevices(m_VKInstance, &DevicesCount, nullptr);
	if(Res != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, CheckVulkanCriticalError(Res));
		return false;
	}
	if(DevicesCount == 0)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "No Vulkan compatible devices found.");
		return false;
	}

	std::vector<VkPhysicalDevice> vDeviceList(DevicesCount);
	Res = vkEnumeratePhysicalDevices(m_VKInstance, &DevicesCount, vDeviceList.data());
	if(Res != VK_SUCCESS && Res != VK_INCOMPLETE)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, CheckVulkanCriticalError(Res));
		return false;
	}
	if(DevicesCount == 0)
	{
		SetWarning(EGfxWarningType::GFX_WARNING_TYPE_INIT_FAILED_MISSING_INTEGRATED_GPU_DRIVER, "No Vulkan compatible devices found.");
		return false;
	}
	// make sure to use the correct amount of devices available
	// the amount of physical devices can be smaller than the amount of devices reported
	// see vkEnumeratePhysicalDevices for details
	vDeviceList.resize(DevicesCount);

	size_t Index = 0;
	std::vector<VkPhysicalDeviceProperties> vDevicePropList(vDeviceList.size());
	m_pGpuList->m_vGpus.reserve(vDeviceList.size());

	size_t FoundDeviceIndex = 0;

	STWGraphicGpu::ETWGraphicsGpuType AutoGpuType = STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_INVALID;

	bool IsAutoGpu = str_comp(g_Config.m_GfxGpuName, "auto") == 0;

	bool UserSelectedGpuChosen = false;
	for(auto &CurDevice : vDeviceList)
	{
		vkGetPhysicalDeviceProperties(CurDevice, &(vDevicePropList[Index]));

		auto &DeviceProp = vDevicePropList[Index];

		STWGraphicGpu::ETWGraphicsGpuType GPUType = VKGPUTypeToGraphicsGpuType(DeviceProp.deviceType);

		int DevApiMajor = (int)VK_API_VERSION_MAJOR(DeviceProp.apiVersion);
		int DevApiMinor = (int)VK_API_VERSION_MINOR(DeviceProp.apiVersion);
		int DevApiPatch = (int)VK_API_VERSION_PATCH(DeviceProp.apiVersion);

		auto IsDenied = CCommandProcessorFragment_Vulkan::IsGpuDenied(DeviceProp.vendorID, DeviceProp.driverVersion, DevApiMajor, DevApiMinor, DevApiPatch);
		if((DevApiMajor > BACKEND_VULKAN_VERSION_MAJOR || (DevApiMajor == BACKEND_VULKAN_VERSION_MAJOR && DevApiMinor >= BACKEND_VULKAN_VERSION_MINOR)) && !IsDenied)
		{
			STWGraphicGpu::STWGraphicGpuItem NewGpu;
			str_copy(NewGpu.m_aName, DeviceProp.deviceName);
			NewGpu.m_GpuType = GPUType;
			m_pGpuList->m_vGpus.push_back(NewGpu);

			// We always decide what the 'auto' GPU would be, even if user is forcing a GPU by name in config
			// Reminder: A worse GPU enumeration has a higher value than a better GPU enumeration, thus the '>'
			if(AutoGpuType > STWGraphicGpu::ETWGraphicsGpuType::GRAPHICS_GPU_TYPE_INTEGRATED)
			{
				str_copy(m_pGpuList->m_AutoGpu.m_aName, DeviceProp.deviceName);
				m_pGpuList->m_AutoGpu.m_GpuType = GPUType;

				AutoGpuType = GPUType;

				if(IsAutoGpu)
					FoundDeviceIndex = Index;
			}
			// We only select the first GPU that matches, because it comes first in the enumeration array, it's preferred by the system
			// Reminder: We can't break the cycle here if the name matches because we need to choose the best GPU for 'auto' mode
			if(!IsAutoGpu && !UserSelectedGpuChosen && str_comp(DeviceProp.deviceName, g_Config.m_GfxGpuName) == 0)
			{
				FoundDeviceIndex = Index;
				UserSelectedGpuChosen = true;
			}
		}
		Index++;
	}

	if(m_pGpuList->m_vGpus.empty())
	{
		SetWarning(EGfxWarningType::GFX_WARNING_TYPE_INIT_FAILED_NO_DEVICE_WITH_REQUIRED_VERSION, "No devices with required Vulkan version found.");
		return false;
	}

	{
		auto &DeviceProp = vDevicePropList[FoundDeviceIndex];

		int DevApiMajor = (int)VK_API_VERSION_MAJOR(DeviceProp.apiVersion);
		int DevApiMinor = (int)VK_API_VERSION_MINOR(DeviceProp.apiVersion);
		int DevApiPatch = (int)VK_API_VERSION_PATCH(DeviceProp.apiVersion);

		str_copy(pRendererName, DeviceProp.deviceName, GPU_INFO_STRING_SIZE);
		GetVendorString(DeviceProp.vendorID, pVendorName, GPU_INFO_STRING_SIZE);
		char aDriverVersion[256];
		FormatDriverVersion(aDriverVersion, DeviceProp.driverVersion, DeviceProp.vendorID);
		str_format(pVersionName, GPU_INFO_STRING_SIZE, "Vulkan %d.%d.%d (driver: %s)",
			DevApiMajor, DevApiMinor, DevApiPatch, aDriverVersion);

		// get important device limits
		m_NonCoherentMemAlignment = DeviceProp.limits.nonCoherentAtomSize;
		m_OptimalImageCopyMemAlignment = DeviceProp.limits.optimalBufferCopyOffsetAlignment;
		m_MaxTextureSize = DeviceProp.limits.maxImageDimension2D;
		m_MaxSamplerAnisotropy = DeviceProp.limits.maxSamplerAnisotropy;

		m_MinUniformAlign = DeviceProp.limits.minUniformBufferOffsetAlignment;
		m_MaxMultiSample = DeviceProp.limits.framebufferColorSampleCounts;
		m_GpuTimestampPeriod = DeviceProp.limits.timestampPeriod;

		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Device prop: non-coherent align: %" PRIzu ", optimal image copy align: %" PRIzu ", max texture size: %u, max sampler anisotropy: %u",
				(size_t)m_NonCoherentMemAlignment, (size_t)m_OptimalImageCopyMemAlignment, m_MaxTextureSize, m_MaxSamplerAnisotropy);
			log_debug("gfx/vulkan", "Device prop: min uniform align: %u, multi sample: %u",
				m_MinUniformAlign, (uint32_t)m_MaxMultiSample);
		}
	}

	VkPhysicalDevice CurDevice = vDeviceList[FoundDeviceIndex];

	uint32_t FamQueueCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(CurDevice, &FamQueueCount, nullptr);
	if(FamQueueCount == 0)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "No Vulkan queue family properties found.");
		return false;
	}

	std::vector<VkQueueFamilyProperties> vQueuePropList(FamQueueCount);
	vkGetPhysicalDeviceQueueFamilyProperties(CurDevice, &FamQueueCount, vQueuePropList.data());

	uint32_t QueueNodeIndex = std::numeric_limits<uint32_t>::max();
	for(uint32_t i = 0; i < FamQueueCount; i++)
	{
		if(vQueuePropList[i].queueCount > 0 && (vQueuePropList[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		{
			QueueNodeIndex = i;
		}
		/*if(vQueuePropList[i].queueCount > 0 && (vQueuePropList[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
		{
			QueueNodeIndex = i;
		}*/
	}

	if(QueueNodeIndex == std::numeric_limits<uint32_t>::max())
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "No Vulkan queue found that matches the requirements: graphics queue.");
		return false;
	}

	m_VKGPU = CurDevice;
	m_VKGraphicsQueueIndex = QueueNodeIndex;
	m_GpuTimestampValidBits = vQueuePropList[QueueNodeIndex].timestampValidBits;
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateLogicalDevice(const std::vector<std::string> &vVKLayers)
{
	std::vector<const char *> vLayerCNames;
	vLayerCNames.reserve(vVKLayers.size());
	for(const auto &Layer : vVKLayers)
		vLayerCNames.emplace_back(Layer.c_str());

	uint32_t DevPropCount = 0;
	if(vkEnumerateDeviceExtensionProperties(m_VKGPU, nullptr, &DevPropCount, nullptr) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Querying logical device extension properties failed.");
		return false;
	}

	std::vector<VkExtensionProperties> vDevPropList(DevPropCount);
	if(vkEnumerateDeviceExtensionProperties(m_VKGPU, nullptr, &DevPropCount, vDevPropList.data()) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Querying logical device extension properties failed.");
		return false;
	}

	std::vector<const char *> vDevPropCNames;
	std::set<std::string> OurDevExt = OurDeviceExtensions();

	for(const auto &CurExtProp : vDevPropList)
	{
		if(OurDevExt.contains(std::string(CurExtProp.extensionName)))
		{
			vDevPropCNames.emplace_back(CurExtProp.extensionName);
		}
	}

#ifdef VK_EXT_device_fault
	bool DeviceFaultRequested = false;
	for(const char *pDevExt : vDevPropCNames)
	{
		if(str_comp(pDevExt, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0)
		{
			DeviceFaultRequested = true;
			break;
		}
	}

	VkPhysicalDeviceFaultFeaturesEXT FaultFeatures = {};
	FaultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
	if(DeviceFaultRequested)
	{
		auto pfnGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(m_VKInstance, "vkGetPhysicalDeviceFeatures2");
		if(pfnGetPhysicalDeviceFeatures2 != nullptr)
		{
			// The extension's core deviceFault feature must be enabled explicitly.
			VkPhysicalDeviceFeatures2 PhysFeatures2 = {};
			PhysFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			PhysFeatures2.pNext = &FaultFeatures;
			pfnGetPhysicalDeviceFeatures2(m_VKGPU, &PhysFeatures2);
		}
	}
#endif

	VkDeviceQueueCreateInfo VKQueueCreateInfo;
	VKQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	VKQueueCreateInfo.queueFamilyIndex = m_VKGraphicsQueueIndex;
	VKQueueCreateInfo.queueCount = 1;
	float QueuePrio = 1.0f;
	VKQueueCreateInfo.pQueuePriorities = &QueuePrio;
	VKQueueCreateInfo.pNext = nullptr;
	VKQueueCreateInfo.flags = 0;

	VkDeviceCreateInfo VKCreateInfo;
	VKCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	VKCreateInfo.queueCreateInfoCount = 1;
	VKCreateInfo.pQueueCreateInfos = &VKQueueCreateInfo;
	VKCreateInfo.ppEnabledLayerNames = vLayerCNames.data();
	VKCreateInfo.enabledLayerCount = static_cast<uint32_t>(vLayerCNames.size());
	VKCreateInfo.ppEnabledExtensionNames = vDevPropCNames.data();
	VKCreateInfo.enabledExtensionCount = static_cast<uint32_t>(vDevPropCNames.size());
	VKCreateInfo.pNext = nullptr;
	VKCreateInfo.pEnabledFeatures = nullptr;
	VKCreateInfo.flags = 0;

#ifdef VK_EXT_device_fault
	if(DeviceFaultRequested && FaultFeatures.deviceFault)
	{
		FaultFeatures.pNext = nullptr;
		// We never read the vendor binary crash dump, so do not opt into generating it.
		FaultFeatures.deviceFaultVendorBinary = VK_FALSE;
		VKCreateInfo.pNext = &FaultFeatures;
	}
#endif

	if(vkCreateDevice(m_VKGPU, &VKCreateInfo, nullptr, &m_VKDevice) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Logical device could not be created.");
		return false;
	}

#ifdef VK_EXT_device_fault
	if(DeviceFaultRequested && FaultFeatures.deviceFault)
	{
		m_pfnGetDeviceFaultInfoEXT = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(m_VKDevice, "vkGetDeviceFaultInfoEXT");
		m_DeviceFaultAvailable = m_pfnGetDeviceFaultInfoEXT != nullptr;
		if(m_DeviceFaultAvailable)
			log_debug("gfx/vulkan", "VK_EXT_device_fault enabled; detailed fault info will be logged on device loss.");
	}
#endif

	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateSurface()
{
	if(!m_Presentation.m_pSurface->CreateVulkanSurface(&m_VKInstance, &m_VKPresentSurface))
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating a Vulkan surface for the window failed.");
		return false;
	}

	VkBool32 IsSupported = false;
	vkGetPhysicalDeviceSurfaceSupportKHR(m_VKGPU, m_VKGraphicsQueueIndex, m_VKPresentSurface, &IsSupported);
	if(!IsSupported)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "The device surface does not support presenting the framebuffer to a screen. Maybe the wrong GPU was selected?");
		return false;
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroySurface()
{
	if(m_VKPresentSurface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_VKInstance, m_VKPresentSurface, nullptr);
		m_VKPresentSurface = VK_NULL_HANDLE;
	}
}

bool CCommandProcessorFragment_Vulkan::GetPresentationMode(VkPresentModeKHR &VKIOMode)
{
	uint32_t PresentModeCount = 0;
	if(vkGetPhysicalDeviceSurfacePresentModesKHR(m_VKGPU, m_VKPresentSurface, &PresentModeCount, nullptr) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "The device surface presentation modes could not be fetched.");
		return false;
	}

	std::vector<VkPresentModeKHR> vPresentModeList(PresentModeCount);
	if(vkGetPhysicalDeviceSurfacePresentModesKHR(m_VKGPU, m_VKPresentSurface, &PresentModeCount, vPresentModeList.data()) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "The device surface presentation modes could not be fetched.");
		return false;
	}

	VKIOMode = g_Config.m_GfxVsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
	for(const auto &Mode : vPresentModeList)
	{
		if(Mode == VKIOMode)
			return true;
	}

	log_warn("gfx/vulkan", "Requested presentation mode was not available. Falling back to mailbox / FIFO relaxed.");
	VKIOMode = g_Config.m_GfxVsync ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
	for(const auto &Mode : vPresentModeList)
	{
		if(Mode == VKIOMode)
			return true;
	}

	log_warn("gfx/vulkan", "Requested presentation mode was not available. Using first available.");
	if(PresentModeCount > 0)
		VKIOMode = vPresentModeList[0];

	return true;
}

bool CCommandProcessorFragment_Vulkan::GetSurfaceProperties(VkSurfaceCapabilitiesKHR &VKSurfCapabilities)
{
	if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_VKGPU, m_VKPresentSurface, &VKSurfCapabilities) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "The device surface capabilities could not be fetched.");
		return false;
	}
	return true;
}

uint32_t CCommandProcessorFragment_Vulkan::GetNumberOfSwapImages(const VkSurfaceCapabilitiesKHR &VKCapabilities)
{
	uint32_t ImgNumber = VKCapabilities.minImageCount + 1;
	if(IsVerbose())
	{
		log_debug("gfx/vulkan", "Minimal swap image count: %u", VKCapabilities.minImageCount);
	}
	return (VKCapabilities.maxImageCount > 0 && ImgNumber > VKCapabilities.maxImageCount) ? VKCapabilities.maxImageCount : ImgNumber;
}

CCommandProcessorFragment_Vulkan::SSwapImgViewportExtent CCommandProcessorFragment_Vulkan::GetSwapImageSize(const VkSurfaceCapabilitiesKHR &VKCapabilities)
{
	VkExtent2D RetSize = {m_CanvasWidth, m_CanvasHeight};

	if(VKCapabilities.currentExtent.width == std::numeric_limits<uint32_t>::max())
	{
		RetSize.width = std::clamp<uint32_t>(RetSize.width, VKCapabilities.minImageExtent.width, VKCapabilities.maxImageExtent.width);
		RetSize.height = std::clamp<uint32_t>(RetSize.height, VKCapabilities.minImageExtent.height, VKCapabilities.maxImageExtent.height);
	}
	else
	{
		RetSize = VKCapabilities.currentExtent;
	}

	VkExtent2D AutoViewportExtent = RetSize;
	bool UsesForcedViewport = false;
	// keep this in sync with graphics_threaded AdjustViewport's check
	if(AutoViewportExtent.height > 4 * AutoViewportExtent.width / 5)
	{
		AutoViewportExtent.height = 4 * AutoViewportExtent.width / 5;
		UsesForcedViewport = true;
	}

	SSwapImgViewportExtent Ext;
	Ext.m_SwapImageViewport = RetSize;
	Ext.m_ForcedViewport = AutoViewportExtent;
	Ext.m_HasForcedViewport = UsesForcedViewport;

	return Ext;
}

bool CCommandProcessorFragment_Vulkan::GetImageUsage(const VkSurfaceCapabilitiesKHR &VKCapabilities, VkImageUsageFlags &VKOutUsage)
{
	constexpr VkImageUsageFlags RequiredImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if((VKCapabilities.supportedUsageFlags & RequiredImageUsage) != RequiredImageUsage)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Framebuffer image attachment types not supported.");
		return false;
	}

	VKOutUsage = RequiredImageUsage;
	return true;
}

VkSurfaceTransformFlagBitsKHR CCommandProcessorFragment_Vulkan::GetTransform(const VkSurfaceCapabilitiesKHR &VKCapabilities)
{
	if(VKCapabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	return VKCapabilities.currentTransform;
}

bool CCommandProcessorFragment_Vulkan::GetFormat()
{
	uint32_t SurfFormats = 0;
	VkResult Res = vkGetPhysicalDeviceSurfaceFormatsKHR(m_VKGPU, m_VKPresentSurface, &SurfFormats, nullptr);
	if(Res != VK_SUCCESS && Res != VK_INCOMPLETE)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "The device surface format fetching failed.");
		return false;
	}

	std::vector<VkSurfaceFormatKHR> vSurfFormatList(SurfFormats);
	Res = vkGetPhysicalDeviceSurfaceFormatsKHR(m_VKGPU, m_VKPresentSurface, &SurfFormats, vSurfFormatList.data());
	if(Res != VK_SUCCESS && Res != VK_INCOMPLETE)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "The device surface format fetching failed.");
		return false;
	}

	if(Res == VK_INCOMPLETE)
	{
		log_warn("gfx/vulkan", "Not all surface formats are requestable with your current settings.");
	}

	if(vSurfFormatList.size() == 1 && vSurfFormatList[0].format == VK_FORMAT_UNDEFINED)
	{
		m_VKSurfFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
		m_VKSurfFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		log_warn("gfx/vulkan", "Surface format was undefined. This can potentially cause bugs.");
		return true;
	}

	for(const auto &FindFormat : vSurfFormatList)
	{
		if(FindFormat.format == VK_FORMAT_B8G8R8A8_UNORM && FindFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			m_VKSurfFormat = FindFormat;
			return true;
		}
		else if(FindFormat.format == VK_FORMAT_R8G8B8A8_UNORM && FindFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			m_VKSurfFormat = FindFormat;
			return true;
		}
	}

	log_warn("gfx/vulkan", "Surface format was not RGBA (or variants of it). This can potentially cause weird looking images (too bright etc.).");
	m_VKSurfFormat = vSurfFormatList[0];
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateSwapChain(VkSwapchainKHR &OldSwapChain, const VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
	m_SwapchainRecreationDeferred = false;
	VkSurfaceCapabilitiesKHR QueriedSurfaceCapabilities;
	if(pSurfaceCapabilities == nullptr)
	{
		if(!GetSurfaceProperties(QueriedSurfaceCapabilities))
			return false;
		pSurfaceCapabilities = &QueriedSurfaceCapabilities;
	}
	const VkSurfaceCapabilitiesKHR &VKSurfCap = *pSurfaceCapabilities;

	VkPresentModeKHR PresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	if(!GetPresentationMode(PresentMode))
		return false;

	uint32_t SwapImgCount = GetNumberOfSwapImages(VKSurfCap);

	m_VKSwapImgAndViewportExtent = GetSwapImageSize(VKSurfCap);

	VkImageUsageFlags UsageFlags;
	if(!GetImageUsage(VKSurfCap, UsageFlags))
		return false;

	VkSurfaceTransformFlagBitsKHR TransformFlagBits = GetTransform(VKSurfCap);

	if(!GetFormat())
		return false;

	OldSwapChain = m_VKSwapChain;

	VkSwapchainCreateInfoKHR SwapInfo;
	SwapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	SwapInfo.pNext = nullptr;
	SwapInfo.flags = 0;
	SwapInfo.surface = m_VKPresentSurface;
	SwapInfo.minImageCount = SwapImgCount;
	SwapInfo.imageFormat = m_VKSurfFormat.format;
	SwapInfo.imageColorSpace = m_VKSurfFormat.colorSpace;
	SwapInfo.imageExtent = m_VKSwapImgAndViewportExtent.m_SwapImageViewport;
	SwapInfo.imageArrayLayers = 1;
	SwapInfo.imageUsage = UsageFlags;
	SwapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	SwapInfo.queueFamilyIndexCount = 0;
	SwapInfo.pQueueFamilyIndices = nullptr;
	SwapInfo.preTransform = TransformFlagBits;
	SwapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	SwapInfo.presentMode = PresentMode;
	SwapInfo.clipped = true;
	SwapInfo.oldSwapchain = OldSwapChain;

	m_VKSwapChain = VK_NULL_HANDLE;
	VkResult SwapchainCreateRes = vkCreateSwapchainKHR(m_VKDevice, &SwapInfo, nullptr, &m_VKSwapChain);
	if(SwapchainCreateRes == VK_ERROR_OUT_OF_DATE_KHR)
	{
		m_VKSwapChain = OldSwapChain;
		m_RecreateSwapChain = true;
		m_SwapchainRecreationDeferred = true;
		return false;
	}
	const char *pCritErrorMsg = CheckVulkanCriticalError(SwapchainCreateRes);
	if(SwapchainCreateRes != VK_SUCCESS)
	{
		if(SwapchainCreateRes != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
			SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the swap chain failed.", pCritErrorMsg);
		return false;
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroySwapChain(bool ForceDestroy)
{
	if(ForceDestroy && m_VKSwapChain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(m_VKDevice, m_VKSwapChain, nullptr);
		m_VKSwapChain = VK_NULL_HANDLE;
	}
}

bool CCommandProcessorFragment_Vulkan::GetSwapChainImageHandles()
{
	uint32_t ImgCount = 0;
	if(vkGetSwapchainImagesKHR(m_VKDevice, m_VKSwapChain, &ImgCount, nullptr) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Could not get swap chain images.");
		return false;
	}

	m_SwapChainImageCount = ImgCount;

	m_vSwapChainImages.resize(ImgCount);
	if(vkGetSwapchainImagesKHR(m_VKDevice, m_VKSwapChain, &ImgCount, m_vSwapChainImages.data()) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Could not get swap chain images.");
		return false;
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateImageViews()
{
	m_vSwapChainImageViewList.resize(m_SwapChainImageCount);

	for(size_t i = 0; i < m_SwapChainImageCount; i++)
	{
		VkImageViewCreateInfo CreateInfo{};
		CreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		CreateInfo.image = m_vSwapChainImages[i];
		CreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		CreateInfo.format = m_VKSurfFormat.format;
		CreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		CreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		CreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		CreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		CreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		CreateInfo.subresourceRange.baseMipLevel = 0;
		CreateInfo.subresourceRange.levelCount = 1;
		CreateInfo.subresourceRange.baseArrayLayer = 0;
		CreateInfo.subresourceRange.layerCount = 1;

		if(vkCreateImageView(m_VKDevice, &CreateInfo, nullptr, &m_vSwapChainImageViewList[i]) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Could not create image views for the swap chain framebuffers.");
			return false;
		}
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyImageViews()
{
	for(auto &ImageView : m_vSwapChainImageViewList)
	{
		vkDestroyImageView(m_VKDevice, ImageView, nullptr);
	}

	m_vSwapChainImageViewList.clear();
}

bool CCommandProcessorFragment_Vulkan::CreateMultiSamplerImageAttachments()
{
	m_vSwapChainMultiSamplingImages.resize(m_SwapChainImageCount);
	if(HasMultiSampling())
	{
		for(size_t i = 0; i < m_SwapChainImageCount; ++i)
		{
			if(!CreateImage(m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width, m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height, 1, 1, m_VKSurfFormat.format, VK_IMAGE_TILING_OPTIMAL, m_vSwapChainMultiSamplingImages[i].m_Image, m_vSwapChainMultiSamplingImages[i].m_ImgMem, VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, GetSampleCount()))
				return false;
			m_vSwapChainMultiSamplingImages[i].m_ImgView = CreateImageView(m_vSwapChainMultiSamplingImages[i].m_Image, m_VKSurfFormat.format, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
		}
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyMultiSamplerImageAttachments()
{
	if(HasMultiSampling() && !m_vSwapChainMultiSamplingImages.empty())
	{
		for(auto &Image : m_vSwapChainMultiSamplingImages)
		{
			vkDestroyImageView(m_VKDevice, Image.m_ImgView, nullptr);
			vkDestroyImage(m_VKDevice, Image.m_Image, nullptr);
			FreeImageMemBlock(Image.m_ImgMem);
		}
	}
	m_vSwapChainMultiSamplingImages.clear();
}

bool CCommandProcessorFragment_Vulkan::CreateFramebuffers()
{
	m_vFramebufferList.resize(m_SwapChainImageCount);

	for(size_t i = 0; i < m_SwapChainImageCount; i++)
	{
		std::array<VkImageView, 2> aAttachments = {
			m_vSwapChainMultiSamplingImages[i].m_ImgView,
			m_vSwapChainImageViewList[i]};

		bool HasMultiSamplingTargets = HasMultiSampling();

		VkFramebufferCreateInfo FramebufferInfo{};
		FramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		FramebufferInfo.renderPass = m_VKRenderPass;
		FramebufferInfo.attachmentCount = HasMultiSamplingTargets ? aAttachments.size() : aAttachments.size() - 1;
		FramebufferInfo.pAttachments = HasMultiSamplingTargets ? aAttachments.data() : aAttachments.data() + 1;
		FramebufferInfo.width = m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width;
		FramebufferInfo.height = m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height;
		FramebufferInfo.layers = 1;

		if(vkCreateFramebuffer(m_VKDevice, &FramebufferInfo, nullptr, &m_vFramebufferList[i]) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the framebuffers failed.");
			return false;
		}
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyFramebuffers()
{
	for(auto &FrameBuffer : m_vFramebufferList)
	{
		vkDestroyFramebuffer(m_VKDevice, FrameBuffer, nullptr);
	}

	m_vFramebufferList.clear();
}

void CCommandProcessorFragment_Vulkan::CleanupVulkanSwapChain(bool ForceSwapChainDestruct)
{
	m_PrimitivePipeline.Destroy(m_VKDevice);
	m_PrimitiveLinePipeline.Destroy(m_VKDevice);
	m_PrimitiveTextureArrayPipeline.Destroy(m_VKDevice);
	m_DualAtlasPipeline.Destroy(m_VKDevice);
	m_ArrayColorPipeline.Destroy(m_VKDevice);
	m_ArrayColorTransformPipeline.Destroy(m_VKDevice);
	m_PrimitiveUniformColorPipeline.Destroy(m_VKDevice);
	m_PrimitiveInstancedPipeline.Destroy(m_VKDevice);
	m_PrimitiveInstancedPushPipeline.Destroy(m_VKDevice);
	m_QuadPerItemPipeline.Destroy(m_VKDevice);
	m_QuadSharedPipeline.Destroy(m_VKDevice);

	DestroyFramebuffers();
	DestroyAllTextureTargets();

	DestroyRenderPass();
	m_CurrentRenderPass = VK_NULL_HANDLE;
	m_CurrentFramebuffer = VK_NULL_HANDLE;
	m_CurrentRenderTarget.Invalidate();
	m_RenderPassActive = false;
	m_AcquireSemaphorePending = false;

	DestroyMultiSamplerImageAttachments();

	DestroyImageViews();
	m_vSwapChainImages.clear();

	DestroySwapChain(ForceSwapChainDestruct);

	m_SwapchainCreated = false;
}

void CCommandProcessorFragment_Vulkan::CleanupVulkanDevice()
{
	if(m_VKInstance != VK_NULL_HANDLE)
	{
		DestroySurface();
		vkDestroyDevice(m_VKDevice, nullptr);

		if(g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL)
		{
			UnregisterDebugCallback();
		}
		vkDestroyInstance(m_VKInstance, nullptr);
		m_VKInstance = VK_NULL_HANDLE;
	}
}

int CCommandProcessorFragment_Vulkan::RecreateSwapChain()
{
	vkDeviceWaitIdle(m_VKDevice);

	VkSurfaceCapabilitiesKHR SurfaceCapabilities;
	if(!GetSurfaceProperties(SurfaceCapabilities))
		return -1;
	const VkExtent2D SurfaceExtent = GetSwapImageSize(SurfaceCapabilities).m_SwapImageViewport;
	if(SurfaceExtent.width == 0 || SurfaceExtent.height == 0)
	{
		m_RenderingPaused = true;
		m_RecreateSwapChain = true;
		m_SwapchainRecreationDeferred = true;
		return 0;
	}

	int Ret = 0;

	if(IsVerbose())
	{
		log_info("gfx/vulkan", "Recreating swap chain.");
	}

	VkSwapchainKHR OldSwapChain = VK_NULL_HANDLE;
	uint32_t OldSwapChainImageCount = m_SwapChainImageCount;

	if(m_SwapchainCreated)
		CleanupVulkanSwapChain(false);

	// set new multi sampling if it was requested
	if(m_NextMultiSamplingCount != std::numeric_limits<uint32_t>::max())
	{
		m_MultiSamplingCount = m_NextMultiSamplingCount;
		m_NextMultiSamplingCount = std::numeric_limits<uint32_t>::max();
	}

	if(!m_SwapchainCreated)
		Ret = InitVulkanSwapChain(OldSwapChain, &SurfaceCapabilities);
	if(Ret > 0)
	{
		m_RenderingPaused = true;
		return 0;
	}

	if(OldSwapChainImageCount != m_SwapChainImageCount)
	{
		CleanupVulkan<false>(OldSwapChainImageCount);
		InitVulkan<false>();
	}

	if(OldSwapChain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(m_VKDevice, OldSwapChain, nullptr);
	}

	if(Ret != 0 && IsVerbose())
	{
		log_warn("gfx/vulkan", "Recreating swap chain failed.");
	}
	if(Ret == 0)
		m_SwapchainRecreationDeferred = false;

	return Ret;
}

int CCommandProcessorFragment_Vulkan::InitVulkanDevice(const CCommandProcessorFragment_Renderer::SPresentationSurface &Surface, char *pRendererString, char *pVendorString, char *pVersionString)
{
	std::vector<std::string> vVKExtensions;
	std::vector<std::string> vVKLayers;

	m_Presentation = Surface;
	m_CanvasWidth = Surface.m_Width;
	m_CanvasHeight = Surface.m_Height;
	m_VKSwapImgAndViewportExtent.m_SwapImageViewport = {Surface.m_Width, Surface.m_Height};

	if(m_Presentation.IsPresentable())
	{
		if(!GetVulkanExtensions(vVKExtensions))
			return -1;
	}

	if(!GetVulkanLayers(vVKLayers))
		return -1;

	if(!CreateVulkanInstance(vVKLayers, vVKExtensions, true))
		return -1;

	if(g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL)
	{
		SetupDebugCallback();

		for(auto &VKLayer : vVKLayers)
		{
			log_info("gfx/vulkan", "Validation layer: %s", VKLayer.c_str());
		}
	}

	if(!SelectGpu(pRendererString, pVendorString, pVersionString))
		return -1;

	if(!CreateLogicalDevice(vVKLayers))
		return -1;

	vkGetDeviceQueue(m_VKDevice, m_VKGraphicsQueueIndex, 0, &m_VKGraphicsQueue);
	if(!m_Presentation.IsPresentable())
	{
		// No surface to ask, so the export format is the one the readback
		// wants anyway.
		m_VKSurfFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
		m_VKSurfFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}
	else
	{
		vkGetDeviceQueue(m_VKDevice, m_VKGraphicsQueueIndex, 0, &m_VKPresentQueue);
		if(!CreateSurface())
			return -1;
	}

	return 0;
}

bool CCommandProcessorFragment_Vulkan::HasMultiSampling() const
{
	return GetSampleCount() != VK_SAMPLE_COUNT_1_BIT;
}

VkSampleCountFlagBits CCommandProcessorFragment_Vulkan::GetMaxSampleCount() const
{
	if(m_MaxMultiSample & VK_SAMPLE_COUNT_64_BIT)
		return VK_SAMPLE_COUNT_64_BIT;
	else if(m_MaxMultiSample & VK_SAMPLE_COUNT_32_BIT)
		return VK_SAMPLE_COUNT_32_BIT;
	else if(m_MaxMultiSample & VK_SAMPLE_COUNT_16_BIT)
		return VK_SAMPLE_COUNT_16_BIT;
	else if(m_MaxMultiSample & VK_SAMPLE_COUNT_8_BIT)
		return VK_SAMPLE_COUNT_8_BIT;
	else if(m_MaxMultiSample & VK_SAMPLE_COUNT_4_BIT)
		return VK_SAMPLE_COUNT_4_BIT;
	else if(m_MaxMultiSample & VK_SAMPLE_COUNT_2_BIT)
		return VK_SAMPLE_COUNT_2_BIT;

	return VK_SAMPLE_COUNT_1_BIT;
}

VkSampleCountFlagBits CCommandProcessorFragment_Vulkan::GetSampleCount() const
{
	auto MaxSampleCount = GetMaxSampleCount();
	if(m_MultiSamplingCount >= 64 && MaxSampleCount >= VK_SAMPLE_COUNT_64_BIT)
		return VK_SAMPLE_COUNT_64_BIT;
	else if(m_MultiSamplingCount >= 32 && MaxSampleCount >= VK_SAMPLE_COUNT_32_BIT)
		return VK_SAMPLE_COUNT_32_BIT;
	else if(m_MultiSamplingCount >= 16 && MaxSampleCount >= VK_SAMPLE_COUNT_16_BIT)
		return VK_SAMPLE_COUNT_16_BIT;
	else if(m_MultiSamplingCount >= 8 && MaxSampleCount >= VK_SAMPLE_COUNT_8_BIT)
		return VK_SAMPLE_COUNT_8_BIT;
	else if(m_MultiSamplingCount >= 4 && MaxSampleCount >= VK_SAMPLE_COUNT_4_BIT)
		return VK_SAMPLE_COUNT_4_BIT;
	else if(m_MultiSamplingCount >= 2 && MaxSampleCount >= VK_SAMPLE_COUNT_2_BIT)
		return VK_SAMPLE_COUNT_2_BIT;

	return VK_SAMPLE_COUNT_1_BIT;
}

int CCommandProcessorFragment_Vulkan::InitVulkanSwapChain(VkSwapchainKHR &OldSwapChain, const VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
	OldSwapChain = VK_NULL_HANDLE;
	if(!CreateSwapChain(OldSwapChain, pSurfaceCapabilities))
		return m_SwapchainRecreationDeferred ? 1 : -1;

	if(!GetSwapChainImageHandles())
		return -1;

	if(!CreateImageViews())
		return -1;

	if(!CreateMultiSamplerImageAttachments())
		return -1;

	if(!CreateRenderPass(m_VKRenderPass, m_VKSurfFormat.format, true, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) ||
		!CreateRenderPass(m_VKRenderPassDiscard, m_VKSurfFormat.format, false, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) ||
		!CreateRenderPass(m_VKRenderTargetPass, RENDER_TARGET_FORMAT, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
		!CreateRenderPass(m_VKRenderTargetPassDiscard, RENDER_TARGET_FORMAT, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
		return -1;

	if(!CreateFramebuffers() || !CreateGraphicsPipelines())
		return -1;

	m_SwapchainCreated = true;
	return 0;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand)
{
	if(pCommand->m_ByResize)
	{
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Got resize event.");
		}
		m_CanvasWidth = pCommand->m_SurfaceWidth > 0 ? static_cast<uint32_t>(pCommand->m_SurfaceWidth) : 0;
		m_CanvasHeight = pCommand->m_SurfaceHeight > 0 ? static_cast<uint32_t>(pCommand->m_SurfaceHeight) : 0;
#ifndef CONF_PLATFORM_MACOS
		m_RecreateSwapChain = true;
#endif
#ifndef CONF_PLATFORM_ANDROID
		if(m_CanvasWidth > 0 && m_CanvasHeight > 0 && !ResumeRendering())
			return false;
#endif
	}
	else
	{
		auto Viewport = m_VKSwapImgAndViewportExtent.GetPresentedImageViewport();
		if(pCommand->m_X != 0 || pCommand->m_Y != 0 || (uint32_t)pCommand->m_Width != Viewport.width || (uint32_t)pCommand->m_Height != Viewport.height)
		{
			m_HasDynamicViewport = true;

			// The viewport rectangle and Vulkan both use a top left origin.
			m_DynamicViewportOffset = {(int32_t)pCommand->m_X, (int32_t)pCommand->m_Y};
			m_DynamicViewportSize = {(uint32_t)pCommand->m_Width, (uint32_t)pCommand->m_Height};
		}
		else
		{
			m_HasDynamicViewport = false;
		}
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_VSync(const CCommandBuffer::SCommand_VSync *pCommand)
{
	if(IsVerbose())
	{
		log_info("gfx/vulkan", "Queueing swap chain recreation because V-Sync was changed.");
	}
	m_RecreateSwapChain = true;
	pCommand->m_pResult->m_Ok = true;

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_MultiSampling(const CCommandBuffer::SCommand_MultiSampling *pCommand)
{
	if(IsVerbose())
	{
		log_info("gfx/vulkan", "Queueing swap chain recreation because multi sampling was changed.");
	}
	m_RecreateSwapChain = true;

	uint32_t MSCount = (std::min(pCommand->m_RequestedMultiSamplingCount, (uint32_t)GetMaxSampleCount()) & 0xFFFFFFFE); // ignore the uneven bits
	m_NextMultiSamplingCount = MSCount;

	pCommand->m_pResult->m_MultiSamplingCount = MSCount;
	pCommand->m_pResult->m_Ok = true;

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_WindowCreateNtf(const CCommandBuffer::SCommand_WindowCreateNtf *pCommand)
{
	if(IsVerbose())
	{
		log_debug("gfx/vulkan", "Creating new surface.");
	}
	// The window may be a different one now; the surface owner already
	// points at it, so asking it for a surface again is enough.
	if(m_RenderingPaused)
	{
#ifdef CONF_PLATFORM_ANDROID
		if(!CreateSurface())
			return false;
		m_RecreateSwapChain = true;
#endif
		if(!ResumeRendering())
			return false;
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_WindowDestroyNtf(const CCommandBuffer::SCommand_WindowDestroyNtf *pCommand)
{
	if(IsVerbose())
	{
		log_debug("gfx/vulkan", "Surface got destroyed.");
	}
	if(!m_RenderingPaused)
	{
		if(!WaitFrame())
			return false;
		m_RenderingPaused = true;
	}
	// A paused frame is already recording, and the cleanup below frees what it
	// references. Submitting first keeps that buffer from outliving them.
	else if(m_FrameCommandsRecording && !SubmitFrameCommands())
		return false;
	m_SwapchainRecreationDeferred = false;
	// The surface is gone once this returns, so everything still referencing it
	// has to have finished. This is not Android specific, the window is
	// destroyed on every platform that can minimize.
	vkDeviceWaitIdle(m_VKDevice);
#ifdef CONF_PLATFORM_ANDROID
	if(m_SwapchainCreated)
		CleanupVulkanSwapChain(true);
	else if(m_VKSwapChain != VK_NULL_HANDLE)
		DestroySwapChain(true);
#endif

	return true;
}

// ---------------------------------------------------------------------------
// Memory: heaps, staging, stream and uniform buffers, and what a frame slot
// gives back.
// ---------------------------------------------------------------------------

const char *CCommandProcessorFragment_Vulkan::MemoryUsageName(EMemoryBlockUsage MemUsage)
{
	switch(MemUsage)
	{
	case EMemoryBlockUsage::TEXTURE:
		return "texture";
	case EMemoryBlockUsage::BUFFER:
		return "buffer";
	case EMemoryBlockUsage::STREAM:
		return "stream";
	case EMemoryBlockUsage::STAGING:
		return "staging buffer";
	default:
		dbg_assert_failed("Invalid MemUsage: %d", (int)MemUsage);
	}
}

void CCommandProcessorFragment_Vulkan::VerboseAllocatedMemory(VkDeviceSize Size, size_t FrameImageIndex, EMemoryBlockUsage MemUsage) const
{
	log_debug("gfx/vulkan", "Allocated chunk of memory with size %" PRIzu " for frame %" PRIzu " (%s).",
		(size_t)Size, (size_t)m_CurImageIndex, MemoryUsageName(MemUsage));
}

void CCommandProcessorFragment_Vulkan::VerboseDeallocatedMemory(VkDeviceSize Size, size_t FrameImageIndex, EMemoryBlockUsage MemUsage) const
{
	log_debug("gfx/vulkan", "Deallocated chunk of memory with size %" PRIzu " for frame %" PRIzu " (%s).",
		(size_t)Size, (size_t)m_CurImageIndex, MemoryUsageName(MemUsage));
}

EGfxErrorType CCommandProcessorFragment_Vulkan::MemoryErrorType(VkResult Result, EGfxErrorType OutOfMemoryType)
{
	return Result == VK_ERROR_OUT_OF_HOST_MEMORY || Result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? OutOfMemoryType : GFX_ERROR_TYPE_UNKNOWN;
}

bool CCommandProcessorFragment_Vulkan::WaitForMemoryCommandBuffer(size_t Slot)
{
	if(Slot >= m_vMemoryCommandBufferPending.size() || !m_vMemoryCommandBufferPending[Slot])
		return true;
	m_vMemoryCommandBufferPending[Slot] = false;
	const VkResult WaitResult = vkWaitForFences(m_VKDevice, 1, &m_vMemoryCommandBufferFences[Slot], VK_TRUE, std::numeric_limits<uint64_t>::max());
	if(WaitResult != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Waiting for a memory upload failed.", CheckVulkanCriticalError(WaitResult));
		return false;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::GetBufferImpl(VkDeviceSize RequiredSize, EMemoryBlockUsage MemUsage, VkBuffer &Buffer, SDeviceMemoryBlock &BufferMemory, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags BufferProperties)
{
	return CreateBuffer(RequiredSize, MemUsage, BufferUsage, BufferProperties, Buffer, BufferMemory);
}

bool CCommandProcessorFragment_Vulkan::GetStagingBuffer(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &ResBlock, const void *pBufferData, VkDeviceSize RequiredSize)
{
	return GetBufferBlockImpl<STAGING_BUFFER_CACHE_ID, 8 * 1024 * 1024, 3, true>(ResBlock, m_StagingBufferCache, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, pBufferData, RequiredSize, std::max(m_NonCoherentMemAlignment, (VkDeviceSize)16));
}

bool CCommandProcessorFragment_Vulkan::GetStagingBufferImage(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &ResBlock, const void *pBufferData, VkDeviceSize RequiredSize)
{
	return GetBufferBlockImpl<STAGING_BUFFER_CACHE_ID, 8 * 1024 * 1024, 3, true>(ResBlock, m_StagingBufferCache, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, pBufferData, RequiredSize, std::max({m_OptimalImageCopyMemAlignment, m_NonCoherentMemAlignment, (VkDeviceSize)16}));
}

void CCommandProcessorFragment_Vulkan::UploadAndFreeLargeStagingMemBlock(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &Block)
{
	UploadStagingBuffers();
	ExecuteMemoryCommandBuffer();
	if(Block.m_pMappedBuffer != nullptr)
		vkUnmapMemory(m_VKDevice, Block.m_BufferMem.m_Mem);
	// The device may still be reading it; the slot frees it after the wait.
	m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, nullptr});
	Block.m_Buffer = VK_NULL_HANDLE;
	Block.m_BufferMem = SDeviceMemoryBlock{};
}

void CCommandProcessorFragment_Vulkan::UploadAndFreeStagingMemBlock(SMemoryBlock<STAGING_BUFFER_CACHE_ID> &Block)
{
	PrepareStagingMemRange(Block);
	if(!Block.m_IsCached)
	{
		UploadAndFreeLargeStagingMemBlock(Block);
	}
	else
	{
		m_StagingBufferCache.FreeMemBlock(Block, m_CurImageIndex);
	}
}

bool CCommandProcessorFragment_Vulkan::GetBufferObjectMemory(SMemoryBlock<BUFFER_OBJECT_CACHE_ID> &ResBlock, VkDeviceSize RequiredSize)
{
	return GetBufferBlockImpl<BUFFER_OBJECT_CACHE_ID, 8 * 1024 * 1024, 3, false>(ResBlock, m_BufferObjectCache, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, RequiredSize, 16);
}

void CCommandProcessorFragment_Vulkan::FreeBufferObjectMemory(SMemoryBlock<BUFFER_OBJECT_CACHE_ID> &Block)
{
	if(!Block.m_IsCached)
	{
		m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, nullptr});
	}
	else
	{
		m_BufferObjectCache.FreeMemBlock(Block, m_CurImageIndex);
	}
}

bool CCommandProcessorFragment_Vulkan::GetImageMemoryImpl(VkDeviceSize RequiredSize, uint32_t RequiredMemoryTypeBits, SDeviceMemoryBlock &BufferMemory, VkMemoryPropertyFlags BufferProperties)
{
	VkMemoryAllocateInfo MemAllocInfo{};
	MemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	MemAllocInfo.allocationSize = RequiredSize;
	MemAllocInfo.memoryTypeIndex = FindMemoryType(m_VKGPU, RequiredMemoryTypeBits, BufferProperties);

	const VkResult AllocateResult = vkAllocateMemory(m_VKDevice, &MemAllocInfo, nullptr, &BufferMemory.m_Mem);
	if(AllocateResult != VK_SUCCESS)
	{
		BufferMemory = {};
		SetError(MemoryErrorType(AllocateResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE), "Allocation for image memory failed.");
		return false;
	}

	BufferMemory.m_Size = RequiredSize;
	BufferMemory.m_UsageType = EMemoryBlockUsage::TEXTURE;
	m_pTextureMemoryUsage->fetch_add(RequiredSize, std::memory_order_relaxed);

	if(IsVerbose())
	{
		VerboseAllocatedMemory(RequiredSize, m_CurImageIndex, EMemoryBlockUsage::TEXTURE);
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::GetImageMemory(SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &RetBlock, VkDeviceSize RequiredSize, VkDeviceSize RequiredAlignment, uint32_t RequiredMemoryTypeBits)
{
	auto BufferCacheIterator = m_ImageBufferCaches.find(RequiredMemoryTypeBits);
	if(BufferCacheIterator == m_ImageBufferCaches.end())
	{
		BufferCacheIterator = m_ImageBufferCaches.try_emplace(RequiredMemoryTypeBits).first;

		BufferCacheIterator->second.Init(m_SwapChainImageCount);
	}
	return GetImageMemoryBlockImpl<IMAGE_BUFFER_CACHE_ID, IMAGE_SIZE_1024X1024_APPROXIMATION, 2>(RetBlock, BufferCacheIterator->second, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, RequiredSize, RequiredAlignment, RequiredMemoryTypeBits);
}

void CCommandProcessorFragment_Vulkan::FreeImageMemBlock(SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &Block)
{
	if(!Block.m_IsCached)
	{
		m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, nullptr});
	}
	else
	{
		m_ImageBufferCaches[Block.m_ImageMemoryBits].FreeMemBlock(Block, m_CurImageIndex);
	}
}

void CCommandProcessorFragment_Vulkan::CleanBufferPair(size_t ImageIndex, VkBuffer &Buffer, SDeviceMemoryBlock &BufferMem)
{
	bool IsBuffer = Buffer != VK_NULL_HANDLE;
	if(IsBuffer)
	{
		vkDestroyBuffer(m_VKDevice, Buffer, nullptr);

		Buffer = VK_NULL_HANDLE;
	}
	if(BufferMem.m_Mem != VK_NULL_HANDLE)
	{
		vkFreeMemory(m_VKDevice, BufferMem.m_Mem, nullptr);
		if(BufferMem.m_UsageType == EMemoryBlockUsage::BUFFER)
			m_pBufferMemoryUsage->fetch_sub(BufferMem.m_Size, std::memory_order_relaxed);
		else if(BufferMem.m_UsageType == EMemoryBlockUsage::TEXTURE)
			m_pTextureMemoryUsage->fetch_sub(BufferMem.m_Size, std::memory_order_relaxed);
		else if(BufferMem.m_UsageType == EMemoryBlockUsage::STREAM)
			m_pStreamMemoryUsage->fetch_sub(BufferMem.m_Size, std::memory_order_relaxed);
		else if(BufferMem.m_UsageType == EMemoryBlockUsage::STAGING)
			m_pStagingMemoryUsage->fetch_sub(BufferMem.m_Size, std::memory_order_relaxed);

		if(IsVerbose())
		{
			VerboseDeallocatedMemory(BufferMem.m_Size, ImageIndex, BufferMem.m_UsageType);
		}

		BufferMem.m_Mem = VK_NULL_HANDLE;
	}
}

void CCommandProcessorFragment_Vulkan::ClearFrameData(size_t FrameImageIndex)
{
	UploadStagingBuffers();

	// clear pending buffers, that require deletion
	for(auto &BufferPair : m_vvFrameDelayedBufferCleanup[FrameImageIndex])
	{
		if(BufferPair.m_pMappedData != nullptr)
		{
			vkUnmapMemory(m_VKDevice, BufferPair.m_Mem.m_Mem);
		}
		CleanBufferPair(FrameImageIndex, BufferPair.m_Buffer, BufferPair.m_Mem);
	}
	m_vvFrameDelayedBufferCleanup[FrameImageIndex].clear();

	// clear pending textures, that require deletion
	for(auto &Texture : m_vvFrameDelayedTextureCleanup[FrameImageIndex])
	{
		DestroyTexture(Texture);
	}
	m_vvFrameDelayedTextureCleanup[FrameImageIndex].clear();

	m_StagingBufferCache.Cleanup(FrameImageIndex);
	m_BufferObjectCache.Cleanup(FrameImageIndex);
	for(auto &ImageBufferCache : m_ImageBufferCaches)
		ImageBufferCache.second.Cleanup(FrameImageIndex);
}

void CCommandProcessorFragment_Vulkan::ShrinkUnusedCaches()
{
	size_t FreedMemory = 0;
	FreedMemory += m_StagingBufferCache.Shrink(m_VKDevice);
	if(FreedMemory > 0)
	{
		m_pStagingMemoryUsage->fetch_sub(FreedMemory, std::memory_order_relaxed);
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Deallocated chunks of memory with size %" PRIzu " from all frames (staging buffer).", FreedMemory);
		}
	}
	FreedMemory = 0;
	FreedMemory += m_BufferObjectCache.Shrink(m_VKDevice);
	if(FreedMemory > 0)
	{
		m_pBufferMemoryUsage->fetch_sub(FreedMemory, std::memory_order_relaxed);
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Deallocated chunks of memory with size %" PRIzu " from all frames (buffer).", FreedMemory);
		}
	}
	FreedMemory = 0;
	for(auto &ImageBufferCache : m_ImageBufferCaches)
		FreedMemory += ImageBufferCache.second.Shrink(m_VKDevice);
	if(FreedMemory > 0)
	{
		m_pTextureMemoryUsage->fetch_sub(FreedMemory, std::memory_order_relaxed);
		if(IsVerbose())
		{
			log_debug("gfx/vulkan", "Deallocated chunks of memory with size %" PRIzu " from all frames (texture).", FreedMemory);
		}
	}
}

bool CCommandProcessorFragment_Vulkan::MemoryBarrier(VkBuffer Buffer, VkDeviceSize Offset, VkDeviceSize Size, VkAccessFlags BufferAccessType, bool BeforeCommand)
{
	VkCommandBuffer *pMemCommandBuffer;
	if(!GetMemoryCommandBuffer(pMemCommandBuffer))
		return false;
	auto &MemCommandBuffer = *pMemCommandBuffer;

	VkBufferMemoryBarrier Barrier{};
	Barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	Barrier.buffer = Buffer;
	Barrier.offset = Offset;
	Barrier.size = Size;

	VkPipelineStageFlags SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

	if(BeforeCommand)
	{
		Barrier.srcAccessMask = BufferAccessType;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		SourceStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else
	{
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		Barrier.dstAccessMask = BufferAccessType;

		SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
	}

	vkCmdPipelineBarrier(
		MemCommandBuffer,
		SourceStage, DestinationStage,
		0,
		0, nullptr,
		1, &Barrier,
		0, nullptr);

	return true;
}

void CCommandProcessorFragment_Vulkan::ExecuteMemoryCommandBuffer()
{
	if(m_vUsedMemoryCommandBuffer[m_CurImageIndex])
	{
		auto &MemoryCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
		vkEndCommandBuffer(MemoryCommandBuffer);

		VkSubmitInfo SubmitInfo{};
		SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		SubmitInfo.commandBufferCount = 1;
		SubmitInfo.pCommandBuffers = &MemoryCommandBuffer;
		VkFence Fence = VK_NULL_HANDLE;
		if(m_CurImageIndex < m_vMemoryCommandBufferFences.size() && vkResetFences(m_VKDevice, 1, &m_vMemoryCommandBufferFences[m_CurImageIndex]) == VK_SUCCESS)
			Fence = m_vMemoryCommandBufferFences[m_CurImageIndex];
		vkQueueSubmit(m_VKGraphicsQueue, 1, &SubmitInfo, Fence);
		if(Fence != VK_NULL_HANDLE)
			m_vMemoryCommandBufferPending[m_CurImageIndex] = true;
		else
			vkQueueWaitIdle(m_VKGraphicsQueue);

		m_vUsedMemoryCommandBuffer[m_CurImageIndex] = false;
	}
}

void CCommandProcessorFragment_Vulkan::ClearFrameMemoryUsage()
{
	ClearFrameData(m_CurImageIndex);
	ShrinkUnusedCaches();
}

void CCommandProcessorFragment_Vulkan::UploadStagingBuffers()
{
	if(!m_vNonFlushedStagingBufferRange.empty())
	{
		vkFlushMappedMemoryRanges(m_VKDevice, m_vNonFlushedStagingBufferRange.size(), m_vNonFlushedStagingBufferRange.data());

		m_vNonFlushedStagingBufferRange.clear();
	}
}

bool CCommandProcessorFragment_Vulkan::PureMemoryFrame()
{
	ExecuteMemoryCommandBuffer();
	// The slot's memory is cleared below, so this frame's upload has to be
	// through with it - the wait is the frame's, not the whole queue's.
	if(!WaitForMemoryCommandBuffer(m_CurImageIndex))
		return false;

	// reset streamed data
	UploadNonFlushedBuffers<false>();

	ClearFrameMemoryUsage();

	return true;
}

void CCommandProcessorFragment_Vulkan::ConvertRgbaToBgra(uint8_t *pData, size_t PixelCount)
{
	for(size_t i = 0; i < PixelCount; ++i)
		std::swap(pData[i * 4], pData[i * 4 + 2]);
}

bool CCommandProcessorFragment_Vulkan::CopyBufferToImage(VkBuffer Buffer, VkDeviceSize BufferOffset, VkImage Image, int32_t X, int32_t Y, uint32_t Width, uint32_t Height, size_t Depth)
{
	VkCommandBuffer *pCommandBuffer;
	if(!GetMemoryCommandBuffer(pCommandBuffer))
		return false;
	auto &CommandBuffer = *pCommandBuffer;

	VkBufferImageCopy Region{};
	Region.bufferOffset = BufferOffset;
	Region.bufferRowLength = 0;
	Region.bufferImageHeight = 0;
	Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	Region.imageSubresource.mipLevel = 0;
	Region.imageSubresource.baseArrayLayer = 0;
	Region.imageSubresource.layerCount = Depth;
	Region.imageOffset = {X, Y, 0};
	Region.imageExtent = {
		Width,
		Height,
		1};

	vkCmdCopyBufferToImage(CommandBuffer, Buffer, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);

	return true;
}

VkAccessFlags CCommandProcessorFragment_Vulkan::BufferReadAccess(IGraphics::EBufferUsage Usage)
{
	return Usage == IGraphics::EBufferUsage::INDEX ? VK_ACCESS_INDEX_READ_BIT : VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
}

bool CCommandProcessorFragment_Vulkan::CreateBufferObject(size_t BufferIndex, const void *pUploadData, VkDeviceSize BufferDataSize, bool IsOneFrameBuffer, IGraphics::EBufferUsage Usage)
{
	std::vector<uint8_t> UploadDataTmp;
	if(pUploadData == nullptr)
	{
		UploadDataTmp.resize(BufferDataSize);
		pUploadData = UploadDataTmp.data();
	}

	while(BufferIndex >= m_vBufferObjects.size())
	{
		m_vBufferObjects.resize((m_vBufferObjects.size() * 2) + 1);
	}
	auto &BufferObject = m_vBufferObjects[BufferIndex];

	VkBuffer Buffer;
	size_t BufferOffset = 0;
	if(!IsOneFrameBuffer)
	{
		SMemoryBlock<STAGING_BUFFER_CACHE_ID> StagingBuffer;
		if(!GetStagingBuffer(StagingBuffer, pUploadData, BufferDataSize))
			return false;

		SMemoryBlock<BUFFER_OBJECT_CACHE_ID> Mem;
		if(!GetBufferObjectMemory(Mem, BufferDataSize))
		{
			UploadAndFreeStagingMemBlock(StagingBuffer);
			return false;
		}

		Buffer = Mem.m_Buffer;
		BufferOffset = Mem.m_HeapData.m_OffsetToAlign;

		const VkAccessFlags ReadAccess = BufferReadAccess(Usage);
		const bool Uploaded = MemoryBarrier(Buffer, Mem.m_HeapData.m_OffsetToAlign, BufferDataSize, ReadAccess, true) &&
				      CopyBuffer(StagingBuffer.m_Buffer, Buffer, StagingBuffer.m_HeapData.m_OffsetToAlign, Mem.m_HeapData.m_OffsetToAlign, BufferDataSize) &&
				      MemoryBarrier(Buffer, Mem.m_HeapData.m_OffsetToAlign, BufferDataSize, ReadAccess, false);
		UploadAndFreeStagingMemBlock(StagingBuffer);
		if(!Uploaded)
		{
			FreeBufferObjectMemory(Mem);
			return false;
		}

		BufferObject.m_BufferObject.m_Mem = Mem;
	}
	else
	{
		SDeviceMemoryBlock BufferMemory;
		if(!CreateStreamBuffer(Buffer, BufferMemory, BufferOffset, pUploadData, BufferDataSize))
			return false;
	}
	BufferObject.m_Usage = Usage;
	BufferObject.m_IsStreamedBuffer = IsOneFrameBuffer;
	BufferObject.m_CurBuffer = Buffer;
	BufferObject.m_CurBufferOffset = BufferOffset;

	return true;
}

void CCommandProcessorFragment_Vulkan::DeleteBufferObject(size_t BufferIndex)
{
	auto &BufferObject = m_vBufferObjects[BufferIndex];
	if(!BufferObject.m_IsStreamedBuffer)
	{
		FreeBufferObjectMemory(BufferObject.m_BufferObject.m_Mem);
	}
	BufferObject = {};
}

bool CCommandProcessorFragment_Vulkan::CopyBuffer(VkBuffer SrcBuffer, VkBuffer DstBuffer, VkDeviceSize SrcOffset, VkDeviceSize DstOffset, VkDeviceSize CopySize)
{
	VkCommandBuffer *pCommandBuffer;
	if(!GetMemoryCommandBuffer(pCommandBuffer))
		return false;
	auto &CommandBuffer = *pCommandBuffer;
	VkBufferCopy CopyRegion{};
	CopyRegion.srcOffset = SrcOffset;
	CopyRegion.dstOffset = DstOffset;
	CopyRegion.size = CopySize;
	vkCmdCopyBuffer(CommandBuffer, SrcBuffer, DstBuffer, 1, &CopyRegion);

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyBufferOfFrame(size_t ImageIndex, SFrameBuffers &Buffer)
{
	if(Buffer.m_BufferMem.m_Mem != VK_NULL_HANDLE)
		vkUnmapMemory(m_VKDevice, Buffer.m_BufferMem.m_Mem);
	CleanBufferPair(ImageIndex, Buffer.m_Buffer, Buffer.m_BufferMem);
}

void CCommandProcessorFragment_Vulkan::DestroyUniBufferOfFrame(size_t ImageIndex, SFrameUniformBuffers &Buffer)
{
	if(Buffer.m_BufferMem.m_Mem != VK_NULL_HANDLE)
		vkUnmapMemory(m_VKDevice, Buffer.m_BufferMem.m_Mem);
	CleanBufferPair(ImageIndex, Buffer.m_Buffer, Buffer.m_BufferMem);
	for(auto &DescrSet : Buffer.m_aUniformSets)
		FreeDescriptorSetFromPool(DescrSet);
}

uint32_t CCommandProcessorFragment_Vulkan::FindMemoryType(VkPhysicalDevice PhyDevice, uint32_t TypeFilter, VkMemoryPropertyFlags Properties)
{
	VkPhysicalDeviceMemoryProperties MemProperties;
	vkGetPhysicalDeviceMemoryProperties(PhyDevice, &MemProperties);

	for(uint32_t i = 0; i < MemProperties.memoryTypeCount; i++)
	{
		if((TypeFilter & (1 << i)) && (MemProperties.memoryTypes[i].propertyFlags & Properties) == Properties)
		{
			return i;
		}
	}

	return 0;
}

bool CCommandProcessorFragment_Vulkan::CreateBuffer(VkDeviceSize BufferSize, EMemoryBlockUsage MemUsage, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags MemoryProperties, VkBuffer &VKBuffer, SDeviceMemoryBlock &VKBufferMemory)
{
	VKBuffer = VK_NULL_HANDLE;
	VKBufferMemory = {};

	VkBufferCreateInfo BufferInfo{};
	BufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	BufferInfo.size = BufferSize;
	BufferInfo.usage = BufferUsage;
	BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	const VkResult CreateResult = vkCreateBuffer(m_VKDevice, &BufferInfo, nullptr, &VKBuffer);
	if(CreateResult != VK_SUCCESS)
	{
		VKBuffer = VK_NULL_HANDLE;
		SetError(MemoryErrorType(CreateResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER), "Buffer creation failed.");
		return false;
	}

	VkMemoryRequirements MemRequirements;
	vkGetBufferMemoryRequirements(m_VKDevice, VKBuffer, &MemRequirements);

	VkMemoryAllocateInfo MemAllocInfo{};
	MemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	MemAllocInfo.allocationSize = MemRequirements.size;
	MemAllocInfo.memoryTypeIndex = FindMemoryType(m_VKGPU, MemRequirements.memoryTypeBits, MemoryProperties);

	const VkResult AllocateResult = vkAllocateMemory(m_VKDevice, &MemAllocInfo, nullptr, &VKBufferMemory.m_Mem);
	if(AllocateResult != VK_SUCCESS)
	{
		VKBufferMemory = {};
		SetError(MemoryErrorType(AllocateResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER), "Allocation for buffer object failed.");
		vkDestroyBuffer(m_VKDevice, VKBuffer, nullptr);
		VKBuffer = VK_NULL_HANDLE;
		return false;
	}

	const VkResult BindResult = vkBindBufferMemory(m_VKDevice, VKBuffer, VKBufferMemory.m_Mem, 0);
	if(BindResult != VK_SUCCESS)
	{
		SetError(MemoryErrorType(BindResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER), "Binding memory to buffer failed.");
		vkFreeMemory(m_VKDevice, VKBufferMemory.m_Mem, nullptr);
		vkDestroyBuffer(m_VKDevice, VKBuffer, nullptr);
		VKBuffer = VK_NULL_HANDLE;
		VKBufferMemory = {};
		return false;
	}

	VKBufferMemory.m_Size = MemRequirements.size;
	VKBufferMemory.m_UsageType = MemUsage;
	if(MemUsage == EMemoryBlockUsage::BUFFER)
		m_pBufferMemoryUsage->fetch_add(MemRequirements.size, std::memory_order_relaxed);
	else if(MemUsage == EMemoryBlockUsage::STAGING)
		m_pStagingMemoryUsage->fetch_add(MemRequirements.size, std::memory_order_relaxed);
	else if(MemUsage == EMemoryBlockUsage::STREAM)
		m_pStreamMemoryUsage->fetch_add(MemRequirements.size, std::memory_order_relaxed);

	if(IsVerbose())
	{
		VerboseAllocatedMemory(MemRequirements.size, m_CurImageIndex, MemUsage);
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::GetMemoryCommandBuffer(VkCommandBuffer *&pMemCommandBuffer)
{
	auto &MemCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
	if(!m_vUsedMemoryCommandBuffer[m_CurImageIndex])
	{
		// Recorded into again only once its last submission on its own is
		// through; one that went with a frame is covered by the frame's
		// fence, which the slot waited for.
		if(!WaitForMemoryCommandBuffer(m_CurImageIndex))
			return false;
		m_vUsedMemoryCommandBuffer[m_CurImageIndex] = true;

		vkResetCommandBuffer(MemCommandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

		VkCommandBufferBeginInfo BeginInfo{};
		BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if(vkBeginCommandBuffer(MemCommandBuffer, &BeginInfo) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Command buffer cannot be filled anymore.");
			return false;
		}
	}
	pMemCommandBuffer = &MemCommandBuffer;
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateStreamBuffer(VkBuffer &NewBuffer, SDeviceMemoryBlock &NewBufferMem, size_t &BufferOffset, const void *pData, size_t DataSize)
{
	SFrameBuffers *pStreamBuffer;
	return CreateStreamBuffer<SFrameBuffers, uint8_t, CMD_BUFFER_DATA_BUFFER_SIZE, 1, false>(
		pStreamBuffer, [](SFrameBuffers &, VkBuffer, VkDeviceSize) { return true; }, m_StreamedBuffers, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, NewBuffer, NewBufferMem, BufferOffset, pData, DataSize, 4);
}

bool CCommandProcessorFragment_Vulkan::GetUniformBufferObject(bool RequiresSharedStagesDescriptor, SDeviceDescriptorSet &DescrSet, const void *pData, size_t DataSize)
{
	return GetUniformBufferObjectImpl<CCommandBuffer::SInstanceDataPositionScaleRotation, 512, 128>(RequiresSharedStagesDescriptor, m_StreamedUniformBuffers, DescrSet, pData, DataSize);
}

bool CCommandProcessorFragment_Vulkan::Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
{
	if(!m_BufferHandles.Activate(pCommand->m_Buffer))
		return true;
	bool IsOneFrameBuffer = pCommand->m_Desc.m_Lifetime == IGraphics::EBufferLifetime::FRAME;
	const bool Created = CreateBufferObject((size_t)pCommand->m_Buffer.Id(), pCommand->m_pUploadData, (VkDeviceSize)pCommand->m_Desc.m_Size, IsOneFrameBuffer, pCommand->m_Desc.m_Usage);
	if(!Created)
		m_BufferHandles.Release(pCommand->m_Buffer);
	return Created;
}

bool CCommandProcessorFragment_Vulkan::Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
{
	if(!m_BufferHandles.IsActive(pCommand->m_Buffer))
		return true;
	DeleteBufferObject((size_t)pCommand->m_Buffer.Id());
	bool IsOneFrameBuffer = pCommand->m_Desc.m_Lifetime == IGraphics::EBufferLifetime::FRAME;
	const bool Created = CreateBufferObject((size_t)pCommand->m_Buffer.Id(), pCommand->m_pUploadData, (VkDeviceSize)pCommand->m_Desc.m_Size, IsOneFrameBuffer, pCommand->m_Desc.m_Usage);
	if(!Created)
		m_BufferHandles.Release(pCommand->m_Buffer);
	return Created;
}

bool CCommandProcessorFragment_Vulkan::Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand)
{
	if(!m_BufferHandles.IsActive(pCommand->m_Buffer))
		return true;
	size_t BufferIndex = (size_t)pCommand->m_Buffer.Id();
	DeleteBufferObject(BufferIndex);
	m_BufferHandles.Release(pCommand->m_Buffer);

	return true;
}

// ---------------------------------------------------------------------------
// Textures: images, uploads, samplers, mipmaps and the descriptor sets that
// sample them.
// ---------------------------------------------------------------------------

size_t CCommandProcessorFragment_Vulkan::ImageMipLevelCount(size_t Width, size_t Height, size_t Depth)
{
	return std::floor(std::log2(std::max({Width, Height, Depth}))) + 1;
}

size_t CCommandProcessorFragment_Vulkan::ImageMipLevelCount(const VkExtent3D &ImgExtent)
{
	return ImageMipLevelCount(ImgExtent.width, ImgExtent.height, ImgExtent.depth);
}

void CCommandProcessorFragment_Vulkan::DestroyTextureTarget(CTexture &Texture)
{
	if(Texture.m_TargetFramebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(m_VKDevice, Texture.m_TargetFramebuffer, nullptr);
	if(Texture.m_TargetMultiSampleImageView != VK_NULL_HANDLE)
		vkDestroyImageView(m_VKDevice, Texture.m_TargetMultiSampleImageView, nullptr);
	if(Texture.m_TargetMultiSampleImage != VK_NULL_HANDLE)
	{
		vkDestroyImage(m_VKDevice, Texture.m_TargetMultiSampleImage, nullptr);
		FreeImageMemBlock(Texture.m_TargetMultiSampleImageMem);
	}
	Texture.m_TargetFramebuffer = VK_NULL_HANDLE;
	Texture.m_TargetMultiSampleImage = VK_NULL_HANDLE;
	Texture.m_TargetMultiSampleImageView = VK_NULL_HANDLE;
	Texture.m_TargetSampleCount = VK_SAMPLE_COUNT_1_BIT;
}

void CCommandProcessorFragment_Vulkan::DestroyTexture(CTexture &Texture)
{
	DestroyTextureTarget(Texture);
	if(Texture.m_Img != VK_NULL_HANDLE)
	{
		FreeImageMemBlock(Texture.m_ImgMem);
		vkDestroyImage(m_VKDevice, Texture.m_Img, nullptr);

		vkDestroyImageView(m_VKDevice, Texture.m_ImgView, nullptr);
	}

	if(Texture.m_Img3D != VK_NULL_HANDLE)
	{
		FreeImageMemBlock(Texture.m_Img3DMem);
		vkDestroyImage(m_VKDevice, Texture.m_Img3D, nullptr);

		vkDestroyImageView(m_VKDevice, Texture.m_Img3DView, nullptr);
	}

	DestroyTexturedStandardDescriptorSets(Texture, 0);
	DestroyTexturedStandardDescriptorSets(Texture, 1);

	DestroyTextured3DStandardDescriptorSets(Texture);
}

void CCommandProcessorFragment_Vulkan::DestroyAllTextureTargets()
{
	for(auto &Texture : m_vTextures)
		DestroyTextureTarget(Texture);
	for(auto &vTextures : m_vvFrameDelayedTextureCleanup)
		for(auto &Texture : vTextures)
			DestroyTextureTarget(Texture);
}

VkFormat CCommandProcessorFragment_Vulkan::ToVkFormat(IGraphics::ETextureFormat Format)
{
	switch(Format)
	{
	case IGraphics::ETextureFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
	case IGraphics::ETextureFormat::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
	case IGraphics::ETextureFormat::R8_UNORM: return VK_FORMAT_R8_UNORM;
	}
	dbg_assert(false, "Unknown texture format");
	dbg_break();
}

bool CCommandProcessorFragment_Vulkan::UpdateTexture(size_t TextureSlot, IGraphics::ETextureFormat TextureFormat, uint8_t *pData, int64_t XOff, int64_t YOff, size_t Width, size_t Height)
{
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedData(nullptr, free);
	auto &Tex = m_vTextures[TextureSlot];
	if(ToVkFormat(TextureFormat) == VK_FORMAT_R8G8B8A8_UNORM && Tex.m_ImageFormat == VK_FORMAT_B8G8R8A8_UNORM)
		ConvertRgbaToBgra(pData, Width * Height);

	if(Tex.m_RescaleCount > 0)
	{
		const size_t SourceWidth = Width;
		const size_t SourceHeight = Height;
		for(uint32_t i = 0; i < Tex.m_RescaleCount; ++i)
		{
			Width >>= 1;
			Height >>= 1;

			XOff /= 2;
			YOff /= 2;
		}

		pOwnedData.reset(ResizeImage(pData, SourceWidth, SourceHeight, Width, Height, IGraphics::PixelSize(TextureFormat)));
		pData = pOwnedData.get();
	}
	const size_t ImageSize = Width * Height * IGraphics::PixelSize(TextureFormat);
	SMemoryBlock<STAGING_BUFFER_CACHE_ID> StagingBuffer;
	if(!GetStagingBufferImage(StagingBuffer, pData, ImageSize))
		return false;

	if(!ImageBarrier(Tex.m_Img, 0, Tex.m_MipMapCount, 0, 1, Tex.m_ImageFormat, Tex.m_Layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
		!CopyBufferToImage(StagingBuffer.m_Buffer, StagingBuffer.m_HeapData.m_OffsetToAlign, Tex.m_Img, XOff, YOff, Width, Height, 1))
	{
		UploadAndFreeStagingMemBlock(StagingBuffer);
		return false;
	}

	UploadAndFreeStagingMemBlock(StagingBuffer);

	if(Tex.m_MipMapCount > 1)
	{
		if(!BuildMipmaps(Tex.m_Img, Tex.m_ImageFormat, Width, Height, 1, Tex.m_MipMapCount))
			return false;
	}
	else
	{
		if(!ImageBarrier(Tex.m_Img, 0, 1, 0, 1, Tex.m_ImageFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
			return false;
	}

	Tex.m_Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateTextureCMD(
	int Slot,
	const IGraphics::CTextureDesc &Desc,
	uint8_t *pData)
{
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedData(nullptr, free);
	int Width = Desc.m_Width;
	int Height = Desc.m_Height;
	const VkFormat Format = ToVkFormat(Desc.m_Format);
	// A target has the format its render pass was created for, whatever
	// the surface's is; a display change cannot make it unusable.
	const VkFormat ImageFormat = Desc.HasUsage(IGraphics::TEXTURE_USAGE_COLOR_TARGET) ? RENDER_TARGET_FORMAT : Format;
	size_t ImageIndex = (size_t)Slot;
	const size_t PixelSize = IGraphics::PixelSize(Desc.m_Format);

	while(ImageIndex >= m_vTextures.size())
	{
		m_vTextures.resize((m_vTextures.size() * 2) + 1);
	}

	// resample if needed
	uint32_t RescaleCount = 0;
	if(pData != nullptr && ((size_t)Width > m_MaxTextureSize || (size_t)Height > m_MaxTextureSize))
	{
		const size_t OldWidth = Width;
		const size_t OldHeight = Height;
		do
		{
			Width >>= 1;
			Height >>= 1;
			++RescaleCount;
		} while((size_t)Width > m_MaxTextureSize || (size_t)Height > m_MaxTextureSize);

		pOwnedData.reset(ResizeImage(pData, OldWidth, OldHeight, Width, Height, PixelSize));
		pData = pOwnedData.get();
	}

	const bool Requires2DTexture = Desc.m_Create2D;
	const bool Requires2DTextureArray = Desc.m_Layering == IGraphics::ETextureLayering::LAYERED;
	const bool RequiresMipMaps = Desc.m_Mipmaps == IGraphics::ETextureMipmaps::GENERATE;
	size_t MipMapLevelCount = 1;
	if(RequiresMipMaps)
	{
		VkExtent3D ImgSize{(uint32_t)Width, (uint32_t)Height, 1};
		MipMapLevelCount = ImageMipLevelCount(ImgSize);
		if(!m_OptimalRGBAImageBlitting)
			MipMapLevelCount = 1;
	}

	CTexture &Texture = m_vTextures[ImageIndex];

	Texture.m_Width = Width;
	Texture.m_Height = Height;
	Texture.m_SourceWidth = Desc.m_Width;
	Texture.m_SourceHeight = Desc.m_Height;
	Texture.m_Format = Desc.m_Format;
	Texture.m_Usage = Desc.m_Usage;
	Texture.m_Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	Texture.m_ImageFormat = ImageFormat;
	Texture.m_RescaleCount = RescaleCount;
	Texture.m_MipMapCount = MipMapLevelCount;

	if(Requires2DTexture)
	{
		if(pData != nullptr && Format == VK_FORMAT_R8G8B8A8_UNORM && ImageFormat == VK_FORMAT_B8G8R8A8_UNORM)
			ConvertRgbaToBgra(pData, static_cast<size_t>(Width) * Height);
		VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if(Desc.HasUsage(IGraphics::TEXTURE_USAGE_SAMPLED))
			ImageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if(Desc.HasUsage(IGraphics::TEXTURE_USAGE_COPY_SOURCE) || RequiresMipMaps)
			ImageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if(Desc.HasUsage(IGraphics::TEXTURE_USAGE_COLOR_TARGET))
			ImageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

		if(!CreateTextureImage(ImageIndex, Texture.m_Img, Texture.m_ImgMem, pData, ImageFormat, Width, Height, 1, PixelSize, MipMapLevelCount, ImageUsage))
			return false;
		if(pData != nullptr)
			Texture.m_Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkFormat ImgFormat = ImageFormat;
		VkImageView ImgView = CreateTextureImageView(Texture.m_Img, ImgFormat, VK_IMAGE_VIEW_TYPE_2D, 1, MipMapLevelCount);
		if(ImgView == VK_NULL_HANDLE)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE, "Creating a 2D texture image view failed.");
			return false;
		}
		Texture.m_ImgView = ImgView;
		if(Desc.HasUsage(IGraphics::TEXTURE_USAGE_SAMPLED))
		{
			VkSampler ImgSampler = GetTextureSampler(SUPPORTED_SAMPLER_TYPE_REPEAT);
			Texture.m_aSamplers[0] = ImgSampler;
			ImgSampler = GetTextureSampler(SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE);
			Texture.m_aSamplers[1] = ImgSampler;

			if(!CreateNewTexturedStandardDescriptorSets(ImageIndex, 0))
				return false;
			if(!CreateNewTexturedStandardDescriptorSets(ImageIndex, 1))
				return false;
		}
	}

	if(Requires2DTextureArray)
	{
		const int LayerColumns = Desc.m_LayerColumns;
		const int LayerRows = Desc.m_LayerRows;
		int Image3DWidth, Image3DHeight;
		std::unique_ptr<uint8_t, decltype(&free)> pTexData3D = PrepareLayeredImage(pData, Width, Height, PixelSize, LayerColumns, LayerRows, Image3DWidth, Image3DHeight);
		if(pTexData3D == nullptr)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE, "Allocating 2D array texture conversion memory failed.");
			return false;
		}

		const size_t ImageDepth2DArray = Desc.LayerCount();
		VkExtent3D ImgSize{(uint32_t)Image3DWidth, (uint32_t)Image3DHeight, 1};
		if(RequiresMipMaps)
		{
			MipMapLevelCount = ImageMipLevelCount(ImgSize);
			if(!m_OptimalRGBAImageBlitting)
				MipMapLevelCount = 1;
		}

		if(!CreateTextureImage(ImageIndex, Texture.m_Img3D, Texture.m_Img3DMem, pTexData3D.get(), Format, Image3DWidth, Image3DHeight, ImageDepth2DArray, PixelSize, MipMapLevelCount))
			return false;
		VkFormat ImgFormat = Format;
		VkImageView ImgView = CreateTextureImageView(Texture.m_Img3D, ImgFormat, VK_IMAGE_VIEW_TYPE_2D_ARRAY, ImageDepth2DArray, MipMapLevelCount);
		if(ImgView == VK_NULL_HANDLE)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE, "Creating a 2D array texture image view failed.");
			return false;
		}
		Texture.m_Img3DView = ImgView;
		VkSampler ImgSampler = GetTextureSampler(SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY);
		Texture.m_Sampler3D = ImgSampler;

		if(!CreateNew3DTexturedStandardDescriptorSets(ImageIndex))
			return false;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::BuildMipmaps(VkImage Image, VkFormat ImageFormat, size_t Width, size_t Height, size_t Depth, size_t MipMapLevelCount)
{
	VkCommandBuffer *pMemCommandBuffer;
	if(!GetMemoryCommandBuffer(pMemCommandBuffer))
		return false;
	auto &MemCommandBuffer = *pMemCommandBuffer;

	VkImageMemoryBarrier Barrier{};
	Barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	Barrier.image = Image;
	Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	Barrier.subresourceRange.levelCount = 1;
	Barrier.subresourceRange.baseArrayLayer = 0;
	Barrier.subresourceRange.layerCount = Depth;

	int32_t TmpMipWidth = (int32_t)Width;
	int32_t TmpMipHeight = (int32_t)Height;

	for(size_t i = 1; i < MipMapLevelCount; ++i)
	{
		Barrier.subresourceRange.baseMipLevel = i - 1;
		Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		Barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		vkCmdPipelineBarrier(MemCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &Barrier);

		VkImageBlit Blit{};
		Blit.srcOffsets[0] = {0, 0, 0};
		Blit.srcOffsets[1] = {TmpMipWidth, TmpMipHeight, 1};
		Blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Blit.srcSubresource.mipLevel = i - 1;
		Blit.srcSubresource.baseArrayLayer = 0;
		Blit.srcSubresource.layerCount = Depth;
		Blit.dstOffsets[0] = {0, 0, 0};
		Blit.dstOffsets[1] = {TmpMipWidth > 1 ? TmpMipWidth / 2 : 1, TmpMipHeight > 1 ? TmpMipHeight / 2 : 1, 1};
		Blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Blit.dstSubresource.mipLevel = i;
		Blit.dstSubresource.baseArrayLayer = 0;
		Blit.dstSubresource.layerCount = Depth;

		vkCmdBlitImage(MemCommandBuffer,
			Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &Blit,
			m_AllowsLinearBlitting ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

		Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		Barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(MemCommandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &Barrier);

		if(TmpMipWidth > 1)
			TmpMipWidth /= 2;
		if(TmpMipHeight > 1)
			TmpMipHeight /= 2;
	}

	Barrier.subresourceRange.baseMipLevel = MipMapLevelCount - 1;
	Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	Barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(MemCommandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		0, nullptr,
		0, nullptr,
		1, &Barrier);

	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateTextureImage(size_t ImageIndex, VkImage &NewImage, SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &NewImgMem, const uint8_t *pData, VkFormat Format, size_t Width, size_t Height, size_t Depth, size_t PixelSize, size_t MipMapLevelCount, VkImageUsageFlags ImageUsage)
{
	VkFormat ImgFormat = Format;

	if(!CreateImage(Width, Height, Depth, MipMapLevelCount, ImgFormat, VK_IMAGE_TILING_OPTIMAL, NewImage, NewImgMem, ImageUsage))
		return false;
	if(pData == nullptr)
		return true;

	VkDeviceSize ImageSize = Width * Height * Depth * PixelSize;
	SMemoryBlock<STAGING_BUFFER_CACHE_ID> StagingBuffer;
	if(!GetStagingBufferImage(StagingBuffer, pData, ImageSize))
		return false;

	if(!ImageBarrier(NewImage, 0, MipMapLevelCount, 0, Depth, ImgFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
		!CopyBufferToImage(StagingBuffer.m_Buffer, StagingBuffer.m_HeapData.m_OffsetToAlign, NewImage, 0, 0, static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), Depth))
	{
		UploadAndFreeStagingMemBlock(StagingBuffer);
		return false;
	}

	UploadAndFreeStagingMemBlock(StagingBuffer);

	if(MipMapLevelCount > 1)
	{
		if(!BuildMipmaps(NewImage, ImgFormat, Width, Height, Depth, MipMapLevelCount))
			return false;
	}
	else
	{
		if(!ImageBarrier(NewImage, 0, 1, 0, Depth, ImgFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
			return false;
	}

	return true;
}

VkImageView CCommandProcessorFragment_Vulkan::CreateTextureImageView(VkImage TexImage, VkFormat ImgFormat, VkImageViewType ViewType, size_t Depth, size_t MipMapLevelCount)
{
	return CreateImageView(TexImage, ImgFormat, ViewType, Depth, MipMapLevelCount);
}

bool CCommandProcessorFragment_Vulkan::CreateTextureSamplersImpl(VkSampler &CreatedSampler, VkSamplerAddressMode AddrModeU, VkSamplerAddressMode AddrModeV, VkSamplerAddressMode AddrModeW)
{
	VkSamplerCreateInfo SamplerInfo{};
	SamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	SamplerInfo.magFilter = VK_FILTER_LINEAR;
	SamplerInfo.minFilter = VK_FILTER_LINEAR;
	SamplerInfo.addressModeU = AddrModeU;
	SamplerInfo.addressModeV = AddrModeV;
	SamplerInfo.addressModeW = AddrModeW;
	SamplerInfo.anisotropyEnable = VK_FALSE;
	SamplerInfo.maxAnisotropy = m_MaxSamplerAnisotropy;
	SamplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	SamplerInfo.unnormalizedCoordinates = VK_FALSE;
	SamplerInfo.compareEnable = VK_FALSE;
	SamplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	SamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	SamplerInfo.mipLodBias = (m_GlobalTextureLodBIAS / 1000.0f);
	SamplerInfo.minLod = -1000;
	SamplerInfo.maxLod = 1000;

	if(vkCreateSampler(m_VKDevice, &SamplerInfo, nullptr, &CreatedSampler) != VK_SUCCESS)
	{
		log_error("gfx/vulkan", "Failed to create texture sampler.");
		return false;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateTextureSamplers()
{
	bool Ret = true;
	Ret &= CreateTextureSamplersImpl(m_aSamplers[SUPPORTED_SAMPLER_TYPE_REPEAT], VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT);
	Ret &= CreateTextureSamplersImpl(m_aSamplers[SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE], VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	Ret &= CreateTextureSamplersImpl(m_aSamplers[SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY], VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
	return Ret;
}

void CCommandProcessorFragment_Vulkan::DestroyTextureSamplers()
{
	vkDestroySampler(m_VKDevice, m_aSamplers[SUPPORTED_SAMPLER_TYPE_REPEAT], nullptr);
	vkDestroySampler(m_VKDevice, m_aSamplers[SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE], nullptr);
	vkDestroySampler(m_VKDevice, m_aSamplers[SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY], nullptr);
}

VkSampler CCommandProcessorFragment_Vulkan::GetTextureSampler(ESupportedSamplerTypes SamplerType)
{
	return m_aSamplers[SamplerType];
}

VkImageView CCommandProcessorFragment_Vulkan::CreateImageView(VkImage Image, VkFormat Format, VkImageViewType ViewType, size_t Depth, size_t MipMapLevelCount)
{
	VkImageViewCreateInfo ViewCreateInfo{};
	ViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ViewCreateInfo.image = Image;
	ViewCreateInfo.viewType = ViewType;
	ViewCreateInfo.format = Format;
	ViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ViewCreateInfo.subresourceRange.baseMipLevel = 0;
	ViewCreateInfo.subresourceRange.levelCount = MipMapLevelCount;
	ViewCreateInfo.subresourceRange.baseArrayLayer = 0;
	ViewCreateInfo.subresourceRange.layerCount = Depth;

	VkImageView ImageView;
	if(vkCreateImageView(m_VKDevice, &ViewCreateInfo, nullptr, &ImageView) != VK_SUCCESS)
	{
		return VK_NULL_HANDLE;
	}

	return ImageView;
}

bool CCommandProcessorFragment_Vulkan::CreateImage(uint32_t Width, uint32_t Height, uint32_t Depth, size_t MipMapLevelCount, VkFormat Format, VkImageTiling Tiling, VkImage &Image, SMemoryImageBlock<IMAGE_BUFFER_CACHE_ID> &ImageMemory, VkImageUsageFlags ImageUsage, VkSampleCountFlagBits SampleCount)
{
	Image = VK_NULL_HANDLE;
	ImageMemory = {};

	VkImageCreateInfo ImageInfo{};
	ImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ImageInfo.imageType = VK_IMAGE_TYPE_2D;
	ImageInfo.extent.width = Width;
	ImageInfo.extent.height = Height;
	ImageInfo.extent.depth = 1;
	ImageInfo.mipLevels = MipMapLevelCount;
	ImageInfo.arrayLayers = Depth;
	ImageInfo.format = Format;
	ImageInfo.tiling = Tiling;
	ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ImageInfo.usage = ImageUsage;
	ImageInfo.samples = SampleCount;
	ImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	const VkResult CreateResult = vkCreateImage(m_VKDevice, &ImageInfo, nullptr, &Image);
	if(CreateResult != VK_SUCCESS)
	{
		Image = VK_NULL_HANDLE;
		SetError(MemoryErrorType(CreateResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE), "Image creation failed.");
		return false;
	}

	VkMemoryRequirements MemRequirements;
	vkGetImageMemoryRequirements(m_VKDevice, Image, &MemRequirements);

	if(!GetImageMemory(ImageMemory, MemRequirements.size, MemRequirements.alignment, MemRequirements.memoryTypeBits))
	{
		vkDestroyImage(m_VKDevice, Image, nullptr);
		Image = VK_NULL_HANDLE;
		return false;
	}

	const VkResult BindResult = vkBindImageMemory(m_VKDevice, Image, ImageMemory.m_BufferMem.m_Mem, ImageMemory.m_HeapData.m_OffsetToAlign);
	if(BindResult != VK_SUCCESS)
	{
		SetError(MemoryErrorType(BindResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE), "Binding memory to image failed.");
		vkDestroyImage(m_VKDevice, Image, nullptr);
		Image = VK_NULL_HANDLE;
		if(ImageMemory.m_IsCached)
		{
			ImageMemory.m_pHeap->Free(ImageMemory.m_HeapData);
			m_ImageBufferCaches[ImageMemory.m_ImageMemoryBits].m_CanShrink = true;
		}
		else
		{
			CleanBufferPair(m_CurImageIndex, ImageMemory.m_Buffer, ImageMemory.m_BufferMem);
		}
		ImageMemory = {};
		return false;
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::ImageBarrier(const VkImage &Image, size_t MipMapBase, size_t MipMapCount, size_t LayerBase, size_t LayerCount, VkFormat Format, VkImageLayout OldLayout, VkImageLayout NewLayout)
{
	VkCommandBuffer *pMemCommandBuffer;
	if(!GetMemoryCommandBuffer(pMemCommandBuffer))
		return false;
	return ImageBarrierIn(*pMemCommandBuffer, Image, MipMapBase, MipMapCount, LayerBase, LayerCount, Format, OldLayout, NewLayout);
}

bool CCommandProcessorFragment_Vulkan::ImageBarrierIn(VkCommandBuffer &MemCommandBuffer, const VkImage &Image, size_t MipMapBase, size_t MipMapCount, size_t LayerBase, size_t LayerCount, VkFormat Format, VkImageLayout OldLayout, VkImageLayout NewLayout)
{
	VkImageMemoryBarrier Barrier{};
	Barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	Barrier.oldLayout = OldLayout;
	Barrier.newLayout = NewLayout;
	Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	Barrier.image = Image;
	Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	Barrier.subresourceRange.baseMipLevel = MipMapBase;
	Barrier.subresourceRange.levelCount = MipMapCount;
	Barrier.subresourceRange.baseArrayLayer = LayerBase;
	Barrier.subresourceRange.layerCount = LayerCount;

	VkPipelineStageFlags SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

	if(OldLayout == VK_IMAGE_LAYOUT_UNDEFINED && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		Barrier.srcAccessMask = 0;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		SourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_UNDEFINED && NewLayout == VK_IMAGE_LAYOUT_GENERAL)
	{
		Barrier.srcAccessMask = 0;
		Barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_GENERAL && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_GENERAL)
	{
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		Barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

		SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else
	{
		dbg_assert_failed("Unsupported layout transition. OldLayout=%d NewLayout=%d", (int)OldLayout, (int)NewLayout);
	}

	vkCmdPipelineBarrier(
		MemCommandBuffer,
		SourceStage, DestinationStage,
		0,
		0, nullptr,
		0, nullptr,
		1, &Barrier);

	return true;
}

bool CCommandProcessorFragment_Vulkan::EnsureTargetFramebuffer(CTexture &Texture)
{
	if(Texture.m_TargetFramebuffer != VK_NULL_HANDLE && Texture.m_TargetSampleCount == GetSampleCount())
		return true;
	DestroyTextureTarget(Texture);
	if(Texture.m_ImgView == VK_NULL_HANDLE)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "The render target has no image to draw into.");
		return false;
	}
	const bool MultiSampling = HasMultiSampling();
	if(MultiSampling)
	{
		if(!CreateImage(Texture.m_Width, Texture.m_Height, 1, 1, Texture.m_ImageFormat, VK_IMAGE_TILING_OPTIMAL, Texture.m_TargetMultiSampleImage, Texture.m_TargetMultiSampleImageMem, VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, GetSampleCount()))
			return false;
		Texture.m_TargetMultiSampleImageView = CreateImageView(Texture.m_TargetMultiSampleImage, Texture.m_ImageFormat, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
		if(Texture.m_TargetMultiSampleImageView == VK_NULL_HANDLE)
		{
			DestroyTextureTarget(Texture);
			return false;
		}
	}

	const std::array<VkImageView, 2> aAttachments = {Texture.m_TargetMultiSampleImageView, Texture.m_ImgView};
	VkFramebufferCreateInfo FramebufferInfo{};
	FramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	FramebufferInfo.renderPass = m_VKRenderTargetPass;
	FramebufferInfo.attachmentCount = MultiSampling ? 2 : 1;
	FramebufferInfo.pAttachments = MultiSampling ? aAttachments.data() : aAttachments.data() + 1;
	FramebufferInfo.width = Texture.m_Width;
	FramebufferInfo.height = Texture.m_Height;
	FramebufferInfo.layers = 1;
	if(vkCreateFramebuffer(m_VKDevice, &FramebufferInfo, nullptr, &Texture.m_TargetFramebuffer) != VK_SUCCESS)
	{
		DestroyTextureTarget(Texture);
		return false;
	}
	Texture.m_TargetSampleCount = GetSampleCount();
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateNewTexturedStandardDescriptorSets(size_t TextureSlot, size_t DescrIndex)
{
	auto &Texture = m_vTextures[TextureSlot];

	auto &DescrSet = Texture.m_aVKStandardTexturedDescrSets[DescrIndex];

	VkDescriptorSetAllocateInfo DesAllocInfo{};
	DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	if(!ReserveDescriptorSet(m_StandardTextureDescrPool, DescrSet))
		return false;
	DesAllocInfo.descriptorPool = DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_Pool;
	DesAllocInfo.descriptorSetCount = 1;
	DesAllocInfo.pSetLayouts = &m_StandardTexturedDescriptorSetLayout;

	if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &DescrSet.m_Descriptor) != VK_SUCCESS)
	{
		DescrSet.m_Descriptor = VK_NULL_HANDLE;
		FreeDescriptorSetFromPool(DescrSet);
		return false;
	}

	VkDescriptorImageInfo ImageInfo{};
	ImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	ImageInfo.imageView = Texture.m_ImgView;
	ImageInfo.sampler = Texture.m_aSamplers[DescrIndex];

	std::array<VkWriteDescriptorSet, 1> aDescriptorWrites{};

	aDescriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	aDescriptorWrites[0].dstSet = DescrSet.m_Descriptor;
	aDescriptorWrites[0].dstBinding = 0;
	aDescriptorWrites[0].dstArrayElement = 0;
	aDescriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	aDescriptorWrites[0].descriptorCount = 1;
	aDescriptorWrites[0].pImageInfo = &ImageInfo;

	vkUpdateDescriptorSets(m_VKDevice, static_cast<uint32_t>(aDescriptorWrites.size()), aDescriptorWrites.data(), 0, nullptr);

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyTexturedStandardDescriptorSets(CTexture &Texture, size_t DescrIndex)
{
	auto &DescrSet = Texture.m_aVKStandardTexturedDescrSets[DescrIndex];
	FreeDescriptorSetFromPool(DescrSet);
}

bool CCommandProcessorFragment_Vulkan::CreateNew3DTexturedStandardDescriptorSets(size_t TextureSlot)
{
	auto &Texture = m_vTextures[TextureSlot];

	auto &DescrSet = Texture.m_VKStandard3DTexturedDescrSet;

	VkDescriptorSetAllocateInfo DesAllocInfo{};
	DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	if(!ReserveDescriptorSet(m_StandardTextureDescrPool, DescrSet))
		return false;
	DesAllocInfo.descriptorPool = DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_Pool;
	DesAllocInfo.descriptorSetCount = 1;
	DesAllocInfo.pSetLayouts = &m_Standard3DTexturedDescriptorSetLayout;

	if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &DescrSet.m_Descriptor) != VK_SUCCESS)
	{
		DescrSet.m_Descriptor = VK_NULL_HANDLE;
		FreeDescriptorSetFromPool(DescrSet);
		return false;
	}

	VkDescriptorImageInfo ImageInfo{};
	ImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	ImageInfo.imageView = Texture.m_Img3DView;
	ImageInfo.sampler = Texture.m_Sampler3D;

	std::array<VkWriteDescriptorSet, 1> aDescriptorWrites{};

	aDescriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	aDescriptorWrites[0].dstSet = DescrSet.m_Descriptor;
	aDescriptorWrites[0].dstBinding = 0;
	aDescriptorWrites[0].dstArrayElement = 0;
	aDescriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	aDescriptorWrites[0].descriptorCount = 1;
	aDescriptorWrites[0].pImageInfo = &ImageInfo;

	vkUpdateDescriptorSets(m_VKDevice, static_cast<uint32_t>(aDescriptorWrites.size()), aDescriptorWrites.data(), 0, nullptr);

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyTextured3DStandardDescriptorSets(CTexture &Texture)
{
	auto &DescrSet = Texture.m_VKStandard3DTexturedDescrSet;
	FreeDescriptorSetFromPool(DescrSet);
}

bool CCommandProcessorFragment_Vulkan::Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand)
{
	if(!m_TextureHandles.IsActive(pCommand->m_Texture))
		return true;
	size_t ImageIndex = (size_t)pCommand->m_Texture.Id();
	auto &Texture = m_vTextures[ImageIndex];

	m_vvFrameDelayedTextureCleanup[m_CurImageIndex].push_back(Texture);

	Texture = CTexture{};
	m_TextureHandles.Release(pCommand->m_Texture);

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand)
{
	if(!m_TextureHandles.Activate(pCommand->m_Texture))
		return true;
	int Slot = pCommand->m_Texture.Id();

	const bool Created = CreateTextureCMD(Slot, pCommand->m_Desc, pCommand->m_pData);
	if(!Created)
	{
		DestroyTexture(m_vTextures[Slot]);
		m_vTextures[Slot] = {};
		m_TextureHandles.Release(pCommand->m_Texture);
	}
	return Created;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand)
{
	if(!m_TextureHandles.IsActive(pCommand->m_Texture))
		return true;
	size_t IndexTex = pCommand->m_Texture.Id();
	const CTexture &Texture = m_vTextures[IndexTex];
	const IGraphics::CTextureRegion &Region = pCommand->m_Region;
	if(pCommand->m_Format != Texture.m_Format ||
		Region.m_X > Texture.m_SourceWidth || Region.m_Width > Texture.m_SourceWidth - Region.m_X ||
		Region.m_Y > Texture.m_SourceHeight || Region.m_Height > Texture.m_SourceHeight - Region.m_Y)
	{
		return true;
	}

	return UpdateTexture(IndexTex, pCommand->m_Format, pCommand->m_pData, pCommand->m_Region.m_X, pCommand->m_Region.m_Y, pCommand->m_Region.m_Width, pCommand->m_Region.m_Height);
}

// ---------------------------------------------------------------------------
// Pipelines: render passes, shaders, descriptor layouts and the pipeline
// cache.
// ---------------------------------------------------------------------------

void CCommandProcessorFragment_Vulkan::BindDescriptorSet(VkCommandBuffer CommandBuffer, VkPipelineLayout PipeLayout, uint32_t Slot, VkDescriptorSet Descriptor)
{
	if(m_aLastDescriptorSets[Slot] == Descriptor)
		return;
	vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, Slot, 1, &Descriptor, 0, nullptr);
	m_aLastDescriptorSets[Slot] = Descriptor;
}

VkPipeline &CCommandProcessorFragment_Vulkan::GetPipeline(SPipelineContainer &Container, bool IsTextured, size_t BlendModeIndex)
{
	return GetPipeline(Container, CurrentPipelinePass(), IsTextured, BlendModeIndex);
}

VkPipeline &CCommandProcessorFragment_Vulkan::GetPipeline(SPipelineContainer &Container, EPipelinePass Pass, bool IsTextured, size_t BlendModeIndex)
{
	return Container.m_aaaPipelines[Pass][BlendModeIndex][(size_t)IsTextured];
}

CCommandProcessorFragment_Vulkan::EPipelinePass CCommandProcessorFragment_Vulkan::CurrentPipelinePass() const
{
	return m_CurrentRenderTarget.IsValid() ? PIPELINE_PASS_TARGET : PIPELINE_PASS_SCREEN;
}

VkPipelineLayout &CCommandProcessorFragment_Vulkan::GetPipeLayout(SPipelineContainer &Container, bool IsTextured, size_t BlendModeIndex)
{
	return Container.m_aaPipelineLayouts[BlendModeIndex][(size_t)IsTextured];
}

VkPipelineLayout &CCommandProcessorFragment_Vulkan::GetStandardPipeLayout(bool IsLineGeometry, bool IsTextured, size_t BlendModeIndex)
{
	if(IsLineGeometry)
		return GetPipeLayout(m_PrimitiveLinePipeline, IsTextured, BlendModeIndex);
	else
		return GetPipeLayout(m_PrimitivePipeline, IsTextured, BlendModeIndex);
}

VkPipeline &CCommandProcessorFragment_Vulkan::GetStandardPipe(bool IsLineGeometry, bool IsTextured, size_t BlendModeIndex)
{
	if(IsLineGeometry)
		return GetPipeline(m_PrimitiveLinePipeline, IsTextured, BlendModeIndex);
	else
		return GetPipeline(m_PrimitivePipeline, IsTextured, BlendModeIndex);
}

VkPipelineLayout &CCommandProcessorFragment_Vulkan::GetArrayColorPipeLayout(bool HasTransform, bool IsTextured, size_t BlendModeIndex)
{
	if(!HasTransform)
		return GetPipeLayout(m_ArrayColorPipeline, IsTextured, BlendModeIndex);
	else
		return GetPipeLayout(m_ArrayColorTransformPipeline, IsTextured, BlendModeIndex);
}

VkPipeline &CCommandProcessorFragment_Vulkan::GetArrayColorPipe(bool HasTransform, bool IsTextured, size_t BlendModeIndex)
{
	if(!HasTransform)
		return GetPipeline(m_ArrayColorPipeline, IsTextured, BlendModeIndex);
	else
		return GetPipeline(m_ArrayColorTransformPipeline, IsTextured, BlendModeIndex);
}

void CCommandProcessorFragment_Vulkan::BindPipeline(VkCommandBuffer &CommandBuffer, const SRenderCommandExecuteBuffer &ExecBuffer, VkPipeline &BindingPipe)
{
	if(m_LastPipeline != BindingPipe)
	{
		vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, BindingPipe);
		m_LastPipeline = BindingPipe;
		// A descriptor set already bound at a slot is only still valid if
		// the new pipeline's layout is compatible for that slot with the
		// one it was bound against - two different pipelines binding what
		// happens to be the same descriptor set object at slot 0 does not
		// mean the second one can skip its own bind.
		m_aLastDescriptorSets = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	}

	vkCmdSetViewport(CommandBuffer, 0, 1, &ExecBuffer.m_Viewport);
	vkCmdSetScissor(CommandBuffer, 0, 1, &ExecBuffer.m_Scissor);
}

bool CCommandProcessorFragment_Vulkan::CreateRenderPass(VkRenderPass &RenderPass, VkFormat Format, bool ClearAttachments, VkImageLayout FinalLayout)
{
	bool HasMultiSamplingTargets = HasMultiSampling();
	VkAttachmentDescription MultiSamplingColorAttachment{};
	MultiSamplingColorAttachment.format = Format;
	MultiSamplingColorAttachment.samples = GetSampleCount();
	MultiSamplingColorAttachment.loadOp = ClearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	MultiSamplingColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	MultiSamplingColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	MultiSamplingColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	MultiSamplingColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	MultiSamplingColorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentDescription ColorAttachment{};
	ColorAttachment.format = Format;
	ColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	ColorAttachment.loadOp = ClearAttachments && !HasMultiSamplingTargets ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	ColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	ColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	ColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ColorAttachment.finalLayout = FinalLayout;

	VkAttachmentReference MultiSamplingColorAttachmentRef{};
	MultiSamplingColorAttachmentRef.attachment = 0;
	MultiSamplingColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference ColorAttachmentRef{};
	ColorAttachmentRef.attachment = HasMultiSamplingTargets ? 1 : 0;
	ColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription Subpass{};
	Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	Subpass.colorAttachmentCount = 1;
	Subpass.pColorAttachments = HasMultiSamplingTargets ? &MultiSamplingColorAttachmentRef : &ColorAttachmentRef;
	Subpass.pResolveAttachments = HasMultiSamplingTargets ? &ColorAttachmentRef : nullptr;

	std::array<VkAttachmentDescription, 2> aAttachments;
	aAttachments[0] = MultiSamplingColorAttachment;
	aAttachments[1] = ColorAttachment;

	std::array<VkSubpassDependency, 2> aDependencies{};
	aDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	aDependencies[0].dstSubpass = 0;
	// The fragment shader stage belongs in here: a render target is sampled
	// by the pass that comes after it - a post-process reads the frame it
	// was drawn into - and the next pass writing that same attachment has
	// to wait for those reads. Ordering only against earlier colour writes
	// leaves that write-after-read unsynchronised. No access bits are
	// needed for it; a write-after-read hazard wants execution order, not
	// a cache flush.
	aDependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	aDependencies[0].srcAccessMask = 0;
	aDependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	aDependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	aDependencies[1].srcSubpass = 0;
	aDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	aDependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	aDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	aDependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	aDependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo CreateRenderPassInfo{};
	CreateRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	CreateRenderPassInfo.attachmentCount = HasMultiSamplingTargets ? 2 : 1;
	CreateRenderPassInfo.pAttachments = HasMultiSamplingTargets ? aAttachments.data() : aAttachments.data() + 1;
	CreateRenderPassInfo.subpassCount = 1;
	CreateRenderPassInfo.pSubpasses = &Subpass;
	CreateRenderPassInfo.dependencyCount = aDependencies.size();
	CreateRenderPassInfo.pDependencies = aDependencies.data();

	if(vkCreateRenderPass(m_VKDevice, &CreateRenderPassInfo, nullptr, &RenderPass) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the render pass failed.");
		return false;
	}

	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyRenderPass()
{
	vkDestroyRenderPass(m_VKDevice, m_VKRenderTargetPassDiscard, nullptr);
	vkDestroyRenderPass(m_VKDevice, m_VKRenderTargetPass, nullptr);
	vkDestroyRenderPass(m_VKDevice, m_VKRenderPassDiscard, nullptr);
	vkDestroyRenderPass(m_VKDevice, m_VKRenderPass, nullptr);
	m_VKRenderTargetPassDiscard = VK_NULL_HANDLE;
	m_VKRenderTargetPass = VK_NULL_HANDLE;
	m_VKRenderPassDiscard = VK_NULL_HANDLE;
	m_VKRenderPass = VK_NULL_HANDLE;
}

bool CCommandProcessorFragment_Vulkan::CreateShaderModule(const char *pName, VkShaderModule &ShaderModule)
{
	const SEmbeddedShader *pShader = FindEmbeddedShader(pName);
	if(pShader == nullptr)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "There is no embedded shader of this name.", pName);
		return false;
	}

	VkShaderModuleCreateInfo CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	CreateInfo.codeSize = pShader->m_Size;
	// The table keeps SPIR-V as words, so the pointer is aligned.
	CreateInfo.pCode = reinterpret_cast<const uint32_t *>(pShader->m_pData);

	if(vkCreateShaderModule(m_VKDevice, &CreateInfo, nullptr, &ShaderModule) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Shader module was not created.");
		return false;
	}

	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateDescriptorSetLayouts()
{
	VkDescriptorSetLayoutBinding SamplerLayoutBinding{};
	SamplerLayoutBinding.binding = 0;
	SamplerLayoutBinding.descriptorCount = 1;
	SamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	SamplerLayoutBinding.pImmutableSamplers = nullptr;
	SamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	std::array<VkDescriptorSetLayoutBinding, 1> aBindings = {SamplerLayoutBinding};
	VkDescriptorSetLayoutCreateInfo LayoutInfo{};
	LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	LayoutInfo.bindingCount = aBindings.size();
	LayoutInfo.pBindings = aBindings.data();

	if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &m_StandardTexturedDescriptorSetLayout) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating descriptor layout failed.");
		return false;
	}

	if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &m_Standard3DTexturedDescriptorSetLayout) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating descriptor layout failed.");
		return false;
	}
	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyDescriptorSetLayouts()
{
	vkDestroyDescriptorSetLayout(m_VKDevice, m_StandardTexturedDescriptorSetLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_VKDevice, m_Standard3DTexturedDescriptorSetLayout, nullptr);
}

bool CCommandProcessorFragment_Vulkan::CreateShaders(const char *pVertName, const char *pFragName, VkPipelineShaderStageCreateInfo (&aShaderStages)[2], SShaderModule &ShaderModule)
{
	ShaderModule.m_VKDevice = m_VKDevice;

	if(!CreateShaderModule(pVertName, ShaderModule.m_VertShaderModule))
		return false;

	if(!CreateShaderModule(pFragName, ShaderModule.m_FragShaderModule))
		return false;

	VkPipelineShaderStageCreateInfo &VertShaderStageInfo = aShaderStages[0];
	VertShaderStageInfo = {};
	VertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	VertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	VertShaderStageInfo.module = ShaderModule.m_VertShaderModule;
	VertShaderStageInfo.pName = "main";

	VkPipelineShaderStageCreateInfo &FragShaderStageInfo = aShaderStages[1];
	FragShaderStageInfo = {};
	FragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	FragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	FragShaderStageInfo.module = ShaderModule.m_FragShaderModule;
	FragShaderStageInfo.pName = "main";
	return true;
}

bool CCommandProcessorFragment_Vulkan::GetStandardPipelineInfo(VkPipelineInputAssemblyStateCreateInfo &InputAssembly,
	VkViewport &Viewport,
	VkRect2D &Scissor,
	VkPipelineViewportStateCreateInfo &ViewportState,
	VkPipelineRasterizationStateCreateInfo &Rasterizer,
	VkPipelineMultisampleStateCreateInfo &Multisampling,
	VkPipelineColorBlendAttachmentState &ColorBlendAttachment,
	VkPipelineColorBlendStateCreateInfo &ColorBlending,
	EVulkanBackendBlendModes BlendMode) const
{
	InputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	InputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	InputAssembly.primitiveRestartEnable = VK_FALSE;

	Viewport.x = 0.0f;
	Viewport.y = 0.0f;
	Viewport.width = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width;
	Viewport.height = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height;
	Viewport.minDepth = 0.0f;
	Viewport.maxDepth = 1.0f;

	Scissor.offset = {0, 0};
	Scissor.extent = m_VKSwapImgAndViewportExtent.m_SwapImageViewport;

	ViewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	ViewportState.viewportCount = 1;
	ViewportState.pViewports = &Viewport;
	ViewportState.scissorCount = 1;
	ViewportState.pScissors = &Scissor;

	Rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	Rasterizer.depthClampEnable = VK_FALSE;
	Rasterizer.rasterizerDiscardEnable = VK_FALSE;
	Rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	Rasterizer.lineWidth = 1.0f;
	Rasterizer.cullMode = VK_CULL_MODE_NONE;
	Rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
	Rasterizer.depthBiasEnable = VK_FALSE;

	Multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	Multisampling.sampleShadingEnable = VK_FALSE;
	Multisampling.rasterizationSamples = GetSampleCount();

	ColorBlendAttachment = CreateColorBlendAttachment(BlendMode);

	ColorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	ColorBlending.logicOpEnable = VK_FALSE;
	ColorBlending.logicOp = VK_LOGIC_OP_COPY;
	ColorBlending.attachmentCount = 1;
	ColorBlending.pAttachments = &ColorBlendAttachment;
	ColorBlending.blendConstants[0] = 0.0f;
	ColorBlending.blendConstants[1] = 0.0f;
	ColorBlending.blendConstants[2] = 0.0f;
	ColorBlending.blendConstants[3] = 0.0f;

	return true;
}

VkFormat CCommandProcessorFragment_Vulkan::VertexAttributeFormat(const IGraphics::CVertexAttributeDesc &Attribute)
{
	switch(Attribute.m_Type)
	{
	case IGraphics::EVertexAttributeType::FLOAT32:
		switch(Attribute.m_ComponentCount)
		{
		case 1: return VK_FORMAT_R32_SFLOAT;
		case 2: return VK_FORMAT_R32G32_SFLOAT;
		case 3: return VK_FORMAT_R32G32B32_SFLOAT;
		case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
		default: break;
		}
		break;
	case IGraphics::EVertexAttributeType::UINT8:
		if(Attribute.m_ComponentCount == 4)
			return Attribute.m_Mode == IGraphics::EVertexAttributeMode::INTEGER ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM;
		break;
	default:
		break;
	}
	dbg_assert(false, "Vertex attribute has no Vulkan format");
	return VK_FORMAT_UNDEFINED;
}

bool CCommandProcessorFragment_Vulkan::CreateStandardGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, bool IsLinePrim)
{
	std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};

	const uint32_t Stride = FillVertexInput(IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, aAttributeDescriptions);

	std::array<VkDescriptorSetLayout, 1> aSetLayouts = {m_StandardTexturedDescriptorSetLayout};

	std::array<VkPushConstantRange, 1> aPushConstants{};
	aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos)};

	return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, IsLinePrim);
}

bool CCommandProcessorFragment_Vulkan::CreateStandardGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler, bool IsLinePipe)
{
	bool Ret = true;

	EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

	for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		Ret &= CreateStandardGraphicsPipelineImpl(pVertName, pFragName, IsLinePipe ? m_PrimitiveLinePipeline : m_PrimitivePipeline, TexMode, EVulkanBackendBlendModes(i), IsLinePipe);

	return Ret;
}

bool CCommandProcessorFragment_Vulkan::CreateStandard3DGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
{
	std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};

	aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
	aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * 2};
	aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 2 + sizeof(uint8_t) * 4};

	std::array<VkDescriptorSetLayout, 1> aSetLayouts = {m_Standard3DTexturedDescriptorSetLayout};

	std::array<VkPushConstantRange, 1> aPushConstants{};
	aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos)};

	return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * 2 + sizeof(uint8_t) * 4 + sizeof(float) * 3, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
}

bool CCommandProcessorFragment_Vulkan::CreateStandard3DGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler)
{
	bool Ret = true;

	EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

	for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		Ret &= CreateStandard3DGraphicsPipelineImpl(pVertName, pFragName, m_PrimitiveTextureArrayPipeline, TexMode, EVulkanBackendBlendModes(i));

	return Ret;
}

bool CCommandProcessorFragment_Vulkan::CreateTextGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
{
	std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
	const uint32_t Stride = FillVertexInput(IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, aAttributeDescriptions);

	std::array<VkDescriptorSetLayout, 1> aSetLayouts = {m_StandardTexturedDescriptorSetLayout};

	std::array<VkPushConstantRange, 2> aPushConstants{};
	aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGTextPos)};
	aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformGTextPos) + sizeof(SUniformTextGFragmentOffset), sizeof(SUniformTextGFragmentConstants)};

	return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
}

bool CCommandProcessorFragment_Vulkan::CreateTextGraphicsPipeline(const char *pVertName, const char *pFragName)
{
	bool Ret = true;

	EVulkanBackendTextureModes TexMode = VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

	for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		Ret &= CreateTextGraphicsPipelineImpl(pVertName, pFragName, m_DualAtlasPipeline, TexMode, EVulkanBackendBlendModes(i));

	return Ret;
}

bool CCommandProcessorFragment_Vulkan::CreatePrimExGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
{
	std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
	const uint32_t Stride = FillVertexInput(IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, aAttributeDescriptions);

	std::array<VkDescriptorSetLayout, 1> aSetLayouts;
	aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;
	uint32_t FragPushConstantSize = sizeof(SUniformPrimExGVertColor);

	std::array<VkPushConstantRange, 2> aPushConstants{};
	aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformPrimExGPos)};
	aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformPrimExGPos) + sizeof(SUniformPrimExGVertColorAlign), FragPushConstantSize};

	return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
}

bool CCommandProcessorFragment_Vulkan::CreatePrimExGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler)
{
	bool Ret = true;

	EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

	for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		Ret &= CreatePrimExGraphicsPipelineImpl(pVertName, pFragName, m_PrimitiveUniformColorPipeline, TexMode, EVulkanBackendBlendModes(i));

	return Ret;
}

bool CCommandProcessorFragment_Vulkan::CreateUniformDescriptorSetLayout(VkDescriptorSetLayout &SetLayout, VkShaderStageFlags StageFlags)
{
	VkDescriptorSetLayoutBinding SamplerLayoutBinding{};
	SamplerLayoutBinding.binding = 1;
	SamplerLayoutBinding.descriptorCount = 1;
	SamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	SamplerLayoutBinding.pImmutableSamplers = nullptr;
	SamplerLayoutBinding.stageFlags = StageFlags;

	std::array<VkDescriptorSetLayoutBinding, 1> aBindings = {SamplerLayoutBinding};
	VkDescriptorSetLayoutCreateInfo LayoutInfo{};
	LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	LayoutInfo.bindingCount = aBindings.size();
	LayoutInfo.pBindings = aBindings.data();

	if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &SetLayout) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating descriptor layout failed.");
		return false;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateSpriteMultiUniformDescriptorSetLayout()
{
	return CreateUniformDescriptorSetLayout(m_SpriteMultiUniformDescriptorSetLayout, VK_SHADER_STAGE_VERTEX_BIT);
}

bool CCommandProcessorFragment_Vulkan::CreateQuadUniformDescriptorSetLayout()
{
	return CreateUniformDescriptorSetLayout(m_QuadUniformDescriptorSetLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
}

void CCommandProcessorFragment_Vulkan::DestroyUniformDescriptorSetLayouts()
{
	vkDestroyDescriptorSetLayout(m_VKDevice, m_QuadUniformDescriptorSetLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_VKDevice, m_SpriteMultiUniformDescriptorSetLayout, nullptr);
}

bool CCommandProcessorFragment_Vulkan::CreateUniformDescriptorSet(VkDescriptorSetLayout &SetLayout, SDeviceDescriptorSet &Set, VkBuffer BindBuffer, size_t BufferSize, VkDeviceSize MemoryOffset)
{
	if(!ReserveDescriptorSet(m_UniformBufferDescrPools, Set))
		return false;
	VkDescriptorSetAllocateInfo DesAllocInfo{};
	DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	DesAllocInfo.descriptorSetCount = 1;
	DesAllocInfo.pSetLayouts = &SetLayout;
	DesAllocInfo.descriptorPool = Set.m_pPools->m_vPools[Set.m_PoolIndex].m_Pool;
	if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &Set.m_Descriptor) != VK_SUCCESS)
	{
		Set.m_Descriptor = VK_NULL_HANDLE;
		FreeDescriptorSetFromPool(Set);
		return false;
	}

	VkDescriptorBufferInfo BufferInfo{};
	BufferInfo.buffer = BindBuffer;
	BufferInfo.offset = MemoryOffset;
	BufferInfo.range = BufferSize;

	VkWriteDescriptorSet DescriptorWrite{};
	DescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	DescriptorWrite.dstSet = Set.m_Descriptor;
	DescriptorWrite.dstBinding = 1;
	DescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	DescriptorWrite.descriptorCount = 1;
	DescriptorWrite.pBufferInfo = &BufferInfo;

	vkUpdateDescriptorSets(m_VKDevice, 1, &DescriptorWrite, 0, nullptr);
	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateSpriteMultiGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
{
	std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
	const uint32_t Stride = FillVertexInput(IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, aAttributeDescriptions);

	std::array<VkDescriptorSetLayout, 2> aSetLayouts;
	aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;
	aSetLayouts[1] = m_SpriteMultiUniformDescriptorSetLayout;

	uint32_t VertPushConstantSize = sizeof(SUniformSpriteMultiGPos);
	uint32_t FragPushConstantSize = sizeof(SUniformSpriteMultiGVertColor);

	std::array<VkPushConstantRange, 2> aPushConstants{};
	aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
	aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiGPos) + sizeof(SUniformSpriteMultiGVertColorAlign), FragPushConstantSize};

	return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
}

bool CCommandProcessorFragment_Vulkan::CreateSpriteMultiGraphicsPipeline(const char *pVertName, const char *pFragName)
{
	bool Ret = true;

	EVulkanBackendTextureModes TexMode = VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

	for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		Ret &= CreateSpriteMultiGraphicsPipelineImpl(pVertName, pFragName, m_PrimitiveInstancedPipeline, TexMode, EVulkanBackendBlendModes(i));

	return Ret;
}

bool CCommandProcessorFragment_Vulkan::CreateSpriteMultiPushGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode)
{
	std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
	const uint32_t Stride = FillVertexInput(IGraphics::EVertexLayout::POSITION_TEXCOORD_COLOR, aAttributeDescriptions);

	std::array<VkDescriptorSetLayout, 1> aSetLayouts;
	aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;

	uint32_t VertPushConstantSize = sizeof(SUniformSpriteMultiPushGPos);
	uint32_t FragPushConstantSize = sizeof(SUniformSpriteMultiPushGVertColor);

	std::array<VkPushConstantRange, 2> aPushConstants{};
	aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
	aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiPushGPos), FragPushConstantSize};

	return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, Stride, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode);
}

bool CCommandProcessorFragment_Vulkan::CreateSpriteMultiPushGraphicsPipeline(const char *pVertName, const char *pFragName)
{
	bool Ret = true;

	EVulkanBackendTextureModes TexMode = VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

	for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		Ret &= CreateSpriteMultiPushGraphicsPipelineImpl(pVertName, pFragName, m_PrimitiveInstancedPushPipeline, TexMode, EVulkanBackendBlendModes(i));

	return Ret;
}

bool CCommandProcessorFragment_Vulkan::AllocateDescriptorPool(SDeviceDescriptorPools &DescriptorPools, size_t AllocPoolSize)
{
	SDeviceDescriptorPool NewPool;
	NewPool.m_Size = AllocPoolSize;

	VkDescriptorPoolSize PoolSize{};
	if(DescriptorPools.m_IsUniformPool)
		PoolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	else
		PoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	PoolSize.descriptorCount = static_cast<uint32_t>(AllocPoolSize * DescriptorPools.m_DescriptorsPerSet);

	VkDescriptorPoolCreateInfo PoolInfo{};
	PoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	PoolInfo.poolSizeCount = 1;
	PoolInfo.pPoolSizes = &PoolSize;
	PoolInfo.maxSets = AllocPoolSize;
	PoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

	if(vkCreateDescriptorPool(m_VKDevice, &PoolInfo, nullptr, &NewPool.m_Pool) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_INIT, "Creating the descriptor pool failed.");
		return false;
	}

	DescriptorPools.m_vPools.push_back(NewPool);

	return true;
}

bool CCommandProcessorFragment_Vulkan::CreateDescriptorPools()
{
	m_StandardTextureDescrPool.m_IsUniformPool = false;
	m_StandardTextureDescrPool.m_DefaultAllocSize = 1024;

	m_UniformBufferDescrPools.m_IsUniformPool = true;
	m_UniformBufferDescrPools.m_DefaultAllocSize = 512;

	bool Success = true;
	Success &= AllocateDescriptorPool(m_StandardTextureDescrPool, CCommandBuffer::MAX_TEXTURES);
	Success &= AllocateDescriptorPool(m_UniformBufferDescrPools, 64);
	return Success;
}

void CCommandProcessorFragment_Vulkan::DestroyDescriptorPools()
{
	for(auto &DescrPool : m_StandardTextureDescrPool.m_vPools)
		vkDestroyDescriptorPool(m_VKDevice, DescrPool.m_Pool, nullptr);
	m_StandardTextureDescrPool.m_vPools.clear();
	for(auto &DescrPool : m_UniformBufferDescrPools.m_vPools)
		vkDestroyDescriptorPool(m_VKDevice, DescrPool.m_Pool, nullptr);
	m_UniformBufferDescrPools.m_vPools.clear();
}

bool CCommandProcessorFragment_Vulkan::ReserveDescriptorSet(SDeviceDescriptorPools &DescriptorPools, SDeviceDescriptorSet &Set)
{
	size_t PoolIndex = 0;
	for(; PoolIndex < DescriptorPools.m_vPools.size(); ++PoolIndex)
	{
		if(DescriptorPools.m_vPools[PoolIndex].m_CurSize < DescriptorPools.m_vPools[PoolIndex].m_Size)
			break;
	}

	if(PoolIndex == DescriptorPools.m_vPools.size() && !AllocateDescriptorPool(DescriptorPools, DescriptorPools.m_DefaultAllocSize))
		return false;

	++DescriptorPools.m_vPools[PoolIndex].m_CurSize;
	Set.m_pPools = &DescriptorPools;
	Set.m_PoolIndex = PoolIndex;
	return true;
}

void CCommandProcessorFragment_Vulkan::FreeDescriptorSetFromPool(SDeviceDescriptorSet &DescrSet)
{
	if(DescrSet.m_PoolIndex != std::numeric_limits<size_t>::max())
	{
		if(DescrSet.m_Descriptor != VK_NULL_HANDLE)
			vkFreeDescriptorSets(m_VKDevice, DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_Pool, 1, &DescrSet.m_Descriptor);
		DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_CurSize -= 1;
	}
	DescrSet = {};
}

bool CCommandProcessorFragment_Vulkan::CreateGraphicsPipelines()
{
	if(!CreateStandardGraphicsPipeline("vulkan/prim.vert.spv", "vulkan/prim.frag.spv", false, false))
		return false;

	if(!CreateStandardGraphicsPipeline("vulkan/prim_textured.vert.spv", "vulkan/prim_textured.frag.spv", true, false))
		return false;

	if(!CreateStandardGraphicsPipeline("vulkan/prim.vert.spv", "vulkan/prim.frag.spv", false, true))
		return false;

	if(!CreateStandard3DGraphicsPipeline("vulkan/prim3d.vert.spv", "vulkan/prim3d.frag.spv", false))
		return false;

	if(!CreateStandard3DGraphicsPipeline("vulkan/prim3d_textured.vert.spv", "vulkan/prim3d_textured.frag.spv", true))
		return false;

	if(!CreateTextGraphicsPipeline("vulkan/text.vert.spv", "vulkan/text.frag.spv"))
		return false;

	if(!CreateTileGraphicsPipeline<false>("vulkan/tile.vert.spv", "vulkan/tile.frag.spv", false))
		return false;

	if(!CreateTileGraphicsPipeline<true>("vulkan/tile_textured.vert.spv", "vulkan/tile_textured.frag.spv", false))
		return false;

	if(!CreateTileGraphicsPipeline<false>("vulkan/tile_border.vert.spv", "vulkan/tile_border.frag.spv", true))
		return false;

	if(!CreateTileGraphicsPipeline<true>("vulkan/tile_border_textured.vert.spv", "vulkan/tile_border_textured.frag.spv", true))
		return false;

	if(!CreatePrimExGraphicsPipeline("vulkan/primex.vert.spv", "vulkan/primex.frag.spv", false))
		return false;

	if(!CreatePrimExGraphicsPipeline("vulkan/primex_tex.vert.spv", "vulkan/primex_tex.frag.spv", true))
		return false;

	if(!CreateSpriteMultiGraphicsPipeline("vulkan/spritemulti.vert.spv", "vulkan/spritemulti.frag.spv"))
		return false;

	if(!CreateSpriteMultiPushGraphicsPipeline("vulkan/spritemulti_push.vert.spv", "vulkan/spritemulti_push.frag.spv"))
		return false;

	if(!CreateQuadGraphicsPipeline<false>("vulkan/quad.vert.spv", "vulkan/quad.frag.spv"))
		return false;

	if(!CreateQuadGraphicsPipeline<true>("vulkan/quad_textured.vert.spv", "vulkan/quad_textured.frag.spv"))
		return false;

	if(!CreateQuadGroupedGraphicsPipeline<false>("vulkan/quad_grouped.vert.spv", "vulkan/quad_grouped.frag.spv"))
		return false;

	if(!CreateQuadGroupedGraphicsPipeline<true>("vulkan/quad_grouped_textured.vert.spv", "vulkan/quad_grouped_textured.frag.spv"))
		return false;

	return true;
}

// ---------------------------------------------------------------------------
// Drawing: render state, render passes and the draw commands.
// ---------------------------------------------------------------------------

bool CCommandProcessorFragment_Vulkan::EndCurrentRenderPass()
{
	if(!m_RenderPassActive)
		return true;
	if(!FlushRenderCommands())
		return false;
	vkCmdEndRenderPass(GetMainGraphicCommandBuffer());
	if(m_CurrentRenderTarget.IsValid() && m_TextureHandles.IsActive(m_CurrentRenderTarget))
		m_vTextures[m_CurrentRenderTarget.Id()].m_Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	m_RenderPassActive = false;
	return true;
}

bool CCommandProcessorFragment_Vulkan::BeginCurrentRenderPass(const IGraphics::CRenderPassDesc &Desc)
{
	// A render target does not need the swapchain, so it is still drawn
	// while the window is minimized. Only the screen pass has nowhere to go.
	// Losing the swapchain takes the render passes and framebuffers with it,
	// so nothing is recorded until a frame was started again.
	if(!m_FrameCommandsRecording || (m_RenderingPaused && !Desc.m_ColorTarget.IsValid()))
		return true;
	if(!EndCurrentRenderPass())
		return false;
	const bool Clear = Desc.m_LoadOp == IGraphics::ERenderPassLoadOp::CLEAR;
	if(Desc.m_ColorTarget.IsValid())
	{
		CTexture &Texture = m_vTextures[Desc.m_ColorTarget.Id()];
		if(!EnsureTargetFramebuffer(Texture))
			return false;
		m_CurrentRenderPass = Clear ? m_VKRenderTargetPass : m_VKRenderTargetPassDiscard;
		m_CurrentFramebuffer = Texture.m_TargetFramebuffer;
		m_CurrentRenderExtent = {Texture.m_Width, Texture.m_Height};
		m_CurrentRenderTarget = Desc.m_ColorTarget;
		Texture.m_Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	else
	{
		m_CurrentRenderPass = Clear ? m_VKRenderPass : m_VKRenderPassDiscard;
		m_CurrentFramebuffer = m_vFramebufferList[m_CurImageIndex];
		m_CurrentRenderExtent = m_VKSwapImgAndViewportExtent.m_SwapImageViewport;
		m_CurrentRenderTarget.Invalidate();
	}

	VkRenderPassBeginInfo RenderPassInfo{};
	RenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	RenderPassInfo.renderPass = m_CurrentRenderPass;
	RenderPassInfo.framebuffer = m_CurrentFramebuffer;
	RenderPassInfo.renderArea.offset = {0, 0};
	RenderPassInfo.renderArea.extent = m_CurrentRenderExtent;
	VkClearValue ClearColor = {{{Desc.m_ClearColor.r, Desc.m_ClearColor.g, Desc.m_ClearColor.b, Desc.m_ClearColor.a}}};
	RenderPassInfo.clearValueCount = Clear ? 1 : 0;
	RenderPassInfo.pClearValues = Clear ? &ClearColor : nullptr;
	vkCmdBeginRenderPass(GetMainGraphicCommandBuffer(), &RenderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_RenderPassActive = true;
	return true;
}

void CCommandProcessorFragment_Vulkan::GetStateMatrix(const CCommandBuffer::SState &State, std::array<float, (size_t)4 * 2> &Matrix)
{
	vec2 Scale, Translate;
	// Vulkan's clip space has y pointing down.
	if(!ScreenToClip(State.m_ScreenTL, State.m_ScreenBR, false, Scale, Translate))
	{
		Matrix = {};
		return;
	}
	Matrix = {
		// column 1
		Scale.x, 0,
		// column 2
		0, Scale.y,
		// column 3
		0, 0,
		// column 4
		Translate.x, Translate.y};
}

bool CCommandProcessorFragment_Vulkan::GetIsTextured(const CCommandBuffer::SState &State)
{
	return State.m_Texture.IsValid();
}

size_t CCommandProcessorFragment_Vulkan::GetAddressModeIndex(const CCommandBuffer::SState &State)
{
	switch(State.m_WrapMode)
	{
	case EWrapMode::REPEAT:
		return VULKAN_BACKEND_ADDRESS_MODE_REPEAT;
	case EWrapMode::CLAMP:
		return VULKAN_BACKEND_ADDRESS_MODE_CLAMP_EDGES;
	default:
		dbg_assert_failed("Invalid wrap mode: %d", (int)State.m_WrapMode);
	};
}

size_t CCommandProcessorFragment_Vulkan::GetBlendModeIndex(const CCommandBuffer::SState &State)
{
	switch(State.m_BlendMode)
	{
	case EBlendMode::NONE:
		return VULKAN_BACKEND_BLEND_MODE_NONE;
	case EBlendMode::ALPHA:
		return VULKAN_BACKEND_BLEND_MODE_ALPHA;
	case EBlendMode::ADDITIVE:
		return VULKAN_BACKEND_BLEND_MODE_ADDITATIVE;
	default:
		dbg_assert_failed("Invalid blend mode: %d", (int)State.m_BlendMode);
	};
}

void CCommandProcessorFragment_Vulkan::GetStateIndices(const CCommandBuffer::SState &State, bool &IsTextured, size_t &BlendModeIndex, size_t &AddressModeIndex)
{
	IsTextured = GetIsTextured(State);
	AddressModeIndex = GetAddressModeIndex(State);
	BlendModeIndex = GetBlendModeIndex(State);
}

void CCommandProcessorFragment_Vulkan::ExecBufferFillDynamicStates(const CCommandBuffer::SState &State, SRenderCommandExecuteBuffer &ExecBuffer)
{
	VkViewport Viewport;
	if(m_CurrentRenderTarget.IsValid())
	{
		Viewport.x = 0.0f;
		Viewport.y = 0.0f;
		Viewport.width = static_cast<float>(m_CurrentRenderExtent.width);
		Viewport.height = static_cast<float>(m_CurrentRenderExtent.height);
		Viewport.minDepth = 0.0f;
		Viewport.maxDepth = 1.0f;
	}
	else if(m_HasDynamicViewport)
	{
		Viewport.x = (float)m_DynamicViewportOffset.x;
		Viewport.y = (float)m_DynamicViewportOffset.y;
		Viewport.width = (float)m_DynamicViewportSize.width;
		Viewport.height = (float)m_DynamicViewportSize.height;
		Viewport.minDepth = 0.0f;
		Viewport.maxDepth = 1.0f;
	}
	// else check if there is a forced viewport
	else if(m_VKSwapImgAndViewportExtent.m_HasForcedViewport)
	{
		Viewport.x = 0.0f;
		Viewport.y = 0.0f;
		Viewport.width = (float)m_VKSwapImgAndViewportExtent.m_ForcedViewport.width;
		Viewport.height = (float)m_VKSwapImgAndViewportExtent.m_ForcedViewport.height;
		Viewport.minDepth = 0.0f;
		Viewport.maxDepth = 1.0f;
	}
	else
	{
		Viewport.x = 0.0f;
		Viewport.y = 0.0f;
		Viewport.width = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width;
		Viewport.height = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height;
		Viewport.minDepth = 0.0f;
		Viewport.maxDepth = 1.0f;
	}

	VkRect2D Scissor;
	// convert from OGL to vulkan clip

	// While a render target is bound the front-end already expresses the clip in the
	// target's pixels, because ScreenWidth()/ScreenHeight() report the target's size
	// for as long as the offscreen frame is open. Otherwise the clip is in presented
	// viewport pixels, because the front-end keeps the calculation for the forced
	// viewport in sync with that.
	const bool RenderToTarget = m_CurrentRenderTarget.IsValid();
	const VkExtent2D ClipSpace = RenderToTarget ? m_CurrentRenderExtent : m_VKSwapImgAndViewportExtent.GetPresentedImageViewport();
	if(State.m_ClipEnable)
	{
		int32_t ScissorY = (int32_t)ClipSpace.height - ((int32_t)State.m_ClipY + (int32_t)State.m_ClipH);
		Scissor.offset = {(int32_t)State.m_ClipX, ScissorY};
		Scissor.extent = {(uint32_t)State.m_ClipW, (uint32_t)State.m_ClipH};
	}
	else
	{
		Scissor.offset = {0, 0};
		Scissor.extent = {ClipSpace.width, ClipSpace.height};
	}

	// if there is a dynamic viewport make sure the scissor data is scaled down to that.
	// A zero-sized viewport can be reached while the window is minimized, and dividing
	// by it would turn the whole scissor into NaN.
	if(!RenderToTarget && m_HasDynamicViewport && ClipSpace.width > 0 && ClipSpace.height > 0)
	{
		Scissor.offset.x = (int32_t)(((float)Scissor.offset.x / (float)ClipSpace.width) * (float)m_DynamicViewportSize.width) + m_DynamicViewportOffset.x;
		Scissor.offset.y = (int32_t)(((float)Scissor.offset.y / (float)ClipSpace.height) * (float)m_DynamicViewportSize.height) + m_DynamicViewportOffset.y;
		Scissor.extent.width = (uint32_t)(((float)Scissor.extent.width / (float)ClipSpace.width) * (float)m_DynamicViewportSize.width);
		Scissor.extent.height = (uint32_t)(((float)Scissor.extent.height / (float)ClipSpace.height) * (float)m_DynamicViewportSize.height);
	}

	Viewport.x = std::clamp(Viewport.x, 0.0f, std::numeric_limits<decltype(Viewport.x)>::max());
	Viewport.y = std::clamp(Viewport.y, 0.0f, std::numeric_limits<decltype(Viewport.y)>::max());

	Scissor.offset.x = std::clamp(Scissor.offset.x, 0, std::numeric_limits<decltype(Scissor.offset.x)>::max());
	Scissor.offset.y = std::clamp(Scissor.offset.y, 0, std::numeric_limits<decltype(Scissor.offset.y)>::max());

	ExecBuffer.m_Viewport = Viewport;
	ExecBuffer.m_Scissor = Scissor;
}

void CCommandProcessorFragment_Vulkan::RenderArrayColor_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, size_t BufferObjectIndex)
{
	const auto &BufferObject = m_vBufferObjects[BufferObjectIndex];

	ExecBuffer.m_Buffer = BufferObject.m_CurBuffer;
	ExecBuffer.m_BufferOff = BufferObject.m_CurBufferOffset;

	bool IsTextured = GetIsTextured(State);
	if(IsTextured)
	{
		ExecBuffer.m_aDescriptors[0] = m_vTextures[State.m_Texture.Id()].m_VKStandard3DTexturedDescrSet;
	}

	ExecBufferFillDynamicStates(State, ExecBuffer);
}

bool CCommandProcessorFragment_Vulkan::RenderArrayColor(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, bool HasTransform, const ColorRGBA &Color, const vec2 &Scale, const vec2 &Off, uint32_t IndexCount, size_t IndexOffset)
{
	std::array<float, (size_t)4 * 2> m;
	GetStateMatrix(State, m);

	bool IsTextured;
	size_t BlendModeIndex;
	size_t AddressModeIndex;
	GetStateIndices(State, IsTextured, BlendModeIndex, AddressModeIndex);
	auto &PipeLayout = GetArrayColorPipeLayout(HasTransform, IsTextured, BlendModeIndex);
	auto &PipeLine = GetArrayColorPipe(HasTransform, IsTextured, BlendModeIndex);

	auto &CommandBuffer = GetMainGraphicCommandBuffer();

	BindPipeline(CommandBuffer, ExecBuffer, PipeLine);

	std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
	std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
	vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

	if(IsTextured)
	{
		BindDescriptorSet(CommandBuffer, PipeLayout, 0, ExecBuffer.m_aDescriptors[0].m_Descriptor);
	}

	SUniformTileGPosBorder VertexPushConstants;
	size_t VertexPushConstantSize = sizeof(SUniformTileGPos);
	SUniformTileGVertColor FragPushConstants;
	size_t FragPushConstantSize = sizeof(SUniformTileGVertColor);

	mem_copy(VertexPushConstants.m_aPos, m.data(), m.size() * sizeof(float));
	FragPushConstants = Color;

	if(HasTransform)
	{
		VertexPushConstants.m_Scale = Scale;
		VertexPushConstants.m_Offset = Off;
		VertexPushConstantSize = sizeof(SUniformTileGPosBorder);
	}

	vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, VertexPushConstantSize, &VertexPushConstants);
	vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformTileGPosBorder) + sizeof(SUniformTileGVertColorAlign), FragPushConstantSize, &FragPushConstants);

	vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, static_cast<VkDeviceSize>(ExecBuffer.m_IndexBufferOff + IndexOffset), VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(CommandBuffer, IndexCount, 1, 0, 0, 0);

	return true;
}

const CCommandBuffer::SState *CCommandProcessorFragment_Vulkan::RenderCommandState(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW: return &static_cast<const CCommandBuffer::SCommand_Draw *>(pCommand)->m_State;
	case CCommandBuffer::CMD_DRAW_INDEXED: return &static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand)->m_State;
	default: return nullptr;
	}
}

const IGraphics::CBufferHandle *CCommandProcessorFragment_Vulkan::RenderCommandVertexBuffer(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW_INDEXED: return &static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand)->m_VertexBuffer;
	default: return nullptr;
	}
}

const IGraphics::CBufferHandle *CCommandProcessorFragment_Vulkan::RenderCommandIndexBuffer(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW:
	{
		const auto *pDrawCommand = static_cast<const CCommandBuffer::SCommand_Draw *>(pCommand);
		return pDrawCommand->m_PrimitiveType == EPrimitiveType::QUADS ? &pDrawCommand->m_IndexBuffer : nullptr;
	}
	case CCommandBuffer::CMD_DRAW_INDEXED: return &static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand)->m_IndexBuffer;
	default: return nullptr;
	}
}

std::optional<EPipelineProgram> CCommandProcessorFragment_Vulkan::RenderCommandProgram(const CCommandBuffer::SCommand *pCommand)
{
	switch(pCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_DRAW: return static_cast<const CCommandBuffer::SCommand_Draw *>(pCommand)->m_Program;
	case CCommandBuffer::CMD_DRAW_INDEXED: return static_cast<const CCommandBuffer::SCommand_DrawIndexed *>(pCommand)->m_Program;
	default: return std::nullopt;
	}
}

bool CCommandProcessorFragment_Vulkan::Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand)
{
	const auto Target = pCommand->m_Desc.m_ColorTarget;
	if(!m_Presentation.IsPresentable() && !Target.IsValid())
		return false;
	if(Target.IsValid())
	{
		if(!m_TextureHandles.IsActive(Target) || static_cast<size_t>(Target.Id()) >= m_vTextures.size())
			return true;
		const CTexture &Texture = m_vTextures[Target.Id()];
		if((Texture.m_Usage & IGraphics::TEXTURE_USAGE_COLOR_TARGET) == 0)
			return true;
	}
	return BeginCurrentRenderPass(pCommand->m_Desc);
}

bool CCommandProcessorFragment_Vulkan::Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand)
{
	return EndCurrentRenderPass();
}

bool CCommandProcessorFragment_Vulkan::Cmd_FlushRenderPass(const CCommandBuffer::SCommand_FlushRenderPass *pCommand)
{
	return !m_RenderPassActive || FlushRenderCommands();
}

bool CCommandProcessorFragment_Vulkan::Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand)
{
	// The swapchain pass opens by clearing to the colour of the last clear
	// command, so a clear that repeats that colour has already happened
	// when the frame began. A render target's pass opens with whatever
	// the frontend asked for, and the surface-less client draws every
	// frame into one: there the clear has to be recorded each time, or
	// the frame before shows through everything that is translucent.
	bool ClearColor = pCommand->m_ForceClear || m_CurrentRenderTarget.IsValid();
	if(!pCommand->m_ForceClear)
	{
		ClearColor = ClearColor || m_aClearColor[0] != pCommand->m_Color.r || m_aClearColor[1] != pCommand->m_Color.g ||
			     m_aClearColor[2] != pCommand->m_Color.b || m_aClearColor[3] != pCommand->m_Color.a;
		m_aClearColor[0] = pCommand->m_Color.r;
		m_aClearColor[1] = pCommand->m_Color.g;
		m_aClearColor[2] = pCommand->m_Color.b;
		m_aClearColor[3] = pCommand->m_Color.a;
	}
	if(ClearColor)
	{
		std::array<VkClearAttachment, 1> aAttachments = {VkClearAttachment{VK_IMAGE_ASPECT_COLOR_BIT, 0, VkClearValue{VkClearColorValue{{pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a}}}}};
		std::array<VkClearRect, 1> aClearRects = {VkClearRect{{{0, 0}, m_CurrentRenderExtent}, 0, 1}};

		auto &CommandBuffer = GetMainGraphicCommandBuffer();
		vkCmdClearAttachments(CommandBuffer, aAttachments.size(), aAttachments.data(), aClearRects.size(), aClearRects.data());
	}

	return true;
}

CCommandProcessorFragment_Vulkan::SRenderCommandExecuteBuffer CCommandProcessorFragment_Vulkan::CollectDrawState(const CCommandBuffer::SCommand_Draw *pCommand)
{
	SRenderCommandExecuteBuffer ExecBuffer;
	bool IsTextured = GetIsTextured(pCommand->m_State);
	if(IsTextured)
	{
		if(pCommand->m_Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY)
			ExecBuffer.m_aDescriptors[0] = m_vTextures[pCommand->m_State.m_Texture.Id()].m_VKStandard3DTexturedDescrSet;
		else
		{
			size_t AddressModeIndex = GetAddressModeIndex(pCommand->m_State);
			ExecBuffer.m_aDescriptors[0] = m_vTextures[pCommand->m_State.m_Texture.Id()].m_aVKStandardTexturedDescrSets[AddressModeIndex];
		}
	}

	if(pCommand->m_IndexBuffer.IsValid())
	{
		const auto &IndexBuffer = m_vBufferObjects[pCommand->m_IndexBuffer.Id()];
		ExecBuffer.m_IndexBuffer = IndexBuffer.m_CurBuffer;
		ExecBuffer.m_IndexBufferOff = IndexBuffer.m_CurBufferOffset;
	}

	ExecBufferFillDynamicStates(pCommand->m_State, ExecBuffer);
	return ExecBuffer;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand)
{
	SRenderCommandExecuteBuffer ExecBuffer = CollectDrawState(pCommand);
	const EPipelineProgram Program = pCommand->m_Program;
	if(Program == EPipelineProgram::PRIMITIVE)
		return RenderStandard<CCommandBuffer::SVertex, false>(ExecBuffer, pCommand->m_State, pCommand->m_PrimitiveType, pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount), pCommand->m_VertexCount);
	if(Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY)
		return RenderStandard<CCommandBuffer::SVertexTex3DStream, true>(ExecBuffer, pCommand->m_State, pCommand->m_PrimitiveType, pCommand->m_VertexData.Get<CCommandBuffer::SVertexTex3DStream>(pCommand->m_VertexCount), pCommand->m_VertexCount);
	return true;
}

void CCommandProcessorFragment_Vulkan::VertexBuffer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, size_t BufferObjectIndex)
{
	const auto &BufferObject = m_vBufferObjects[BufferObjectIndex];

	ExecBuffer.m_Buffer = BufferObject.m_CurBuffer;
	ExecBuffer.m_BufferOff = BufferObject.m_CurBufferOffset;

	bool IsTextured = GetIsTextured(State);
	if(IsTextured)
	{
		size_t AddressModeIndex = GetAddressModeIndex(State);
		ExecBuffer.m_aDescriptors[0] = m_vTextures[State.m_Texture.Id()].m_aVKStandardTexturedDescrSets[AddressModeIndex];
	}

	ExecBufferFillDynamicStates(State, ExecBuffer);
}

CCommandProcessorFragment_Vulkan::SRenderCommandExecuteBuffer CCommandProcessorFragment_Vulkan::CollectIndexedDrawState(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
	SRenderCommandExecuteBuffer ExecBuffer;
	const EPipelineProgram Program = pCommand->m_Program;
	if(Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
	{
		VertexBuffer_FillExecuteBuffer(ExecBuffer, pCommand->m_State, (size_t)pCommand->m_VertexBuffer.Id());
	}
	else if(Program == EPipelineProgram::ARRAY_COLOR || Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM)
		RenderArrayColor_FillExecuteBuffer(ExecBuffer, pCommand->m_State, (size_t)pCommand->m_VertexBuffer.Id());
	else
		VertexBuffer_FillExecuteBuffer(ExecBuffer, pCommand->m_State, (size_t)pCommand->m_VertexBuffer.Id());

	const auto &IndexBuffer = m_vBufferObjects[pCommand->m_IndexBuffer.Id()];
	ExecBuffer.m_IndexBuffer = IndexBuffer.m_CurBuffer;
	ExecBuffer.m_IndexBufferOff = IndexBuffer.m_CurBufferOffset;
	return ExecBuffer;
}

bool CCommandProcessorFragment_Vulkan::Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
	// CGraphics_Threaded::CheckIndexedDraw has already rejected everything
	// this would catch; the assert is here so a new producer notices at
	// once instead of binding a pipeline the buffer does not fit.
	dbg_assert(IsIndexedDrawConsistent(*pCommand), "Backend received an inconsistent indexed draw");
	// The fixed function backend already refuses a draw that reaches past
	// the end of its index buffer; this one used to hand it to the driver. A
	// stale quad count on a shrunk buffer is enough.
	{
		const auto &IndexBufferObject = m_vBufferObjects[pCommand->m_IndexBuffer.Id()];
		const size_t IndexBufferSize = IndexBufferObject.m_BufferObject.m_Mem.m_UsedSize;
		const size_t IndexSize = pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? sizeof(uint16_t) : sizeof(uint32_t);
		if(pCommand->m_IndexOffset % IndexSize != 0 || pCommand->m_IndexOffset > IndexBufferSize ||
			static_cast<uint64_t>(pCommand->m_IndexCount) * IndexSize > IndexBufferSize - pCommand->m_IndexOffset)
		{
			DropCommand("an indexed draw that reaches past the end of its index buffer");
			return true;
		}
	}
	SRenderCommandExecuteBuffer ExecBuffer = CollectIndexedDrawState(pCommand);
	const EPipelineProgram Program = pCommand->m_Program;
	if(Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
		return Cmd_DrawIndexedDualAtlas(pCommand, ExecBuffer);
	if(Program == EPipelineProgram::PRIMITIVE_INSTANCED)
		return Cmd_DrawIndexedInstanced(pCommand, ExecBuffer);
	if(Program == EPipelineProgram::ARRAY_COLOR || Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM)
		return Cmd_DrawIndexedArrayColor(pCommand, ExecBuffer);
	if(Program == EPipelineProgram::QUAD_PER_ITEM || Program == EPipelineProgram::QUAD_SHARED)
		return Cmd_DrawIndexedQuadRecords(pCommand, ExecBuffer);

	std::array<float, (size_t)4 * 2> m;
	GetStateMatrix(pCommand->m_State, m);

	const CCommandBuffer::SDrawDataPrimitiveUniformColor *pDrawData = nullptr;
	bool IsTextured;
	size_t BlendModeIndex;
	size_t AddressModeIndex;
	GetStateIndices(pCommand->m_State, IsTextured, BlendModeIndex, AddressModeIndex);

	VkPipelineLayout PipeLayout;
	VkPipeline PipeLine;
	if(Program == EPipelineProgram::PRIMITIVE)
	{
		PipeLayout = GetStandardPipeLayout(false, IsTextured, BlendModeIndex);
		PipeLine = GetStandardPipe(false, IsTextured, BlendModeIndex);
	}
	else if(Program == EPipelineProgram::PRIMITIVE_UNIFORM_COLOR)
	{
		pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>();
		if(pDrawData == nullptr)
			return true;
		PipeLayout = GetPipeLayout(m_PrimitiveUniformColorPipeline, IsTextured, BlendModeIndex);
		PipeLine = GetPipeline(m_PrimitiveUniformColorPipeline, IsTextured, BlendModeIndex);
	}
	else
	{
		return true;
	}

	auto &CommandBuffer = GetMainGraphicCommandBuffer();

	BindPipeline(CommandBuffer, ExecBuffer, PipeLine);

	std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
	std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
	vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

	VkDeviceSize IndexOffset = static_cast<VkDeviceSize>(ExecBuffer.m_IndexBufferOff + pCommand->m_IndexOffset);

	vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, IndexOffset, VK_INDEX_TYPE_UINT32);

	if(IsTextured)
	{
		BindDescriptorSet(CommandBuffer, PipeLayout, 0, ExecBuffer.m_aDescriptors[0].m_Descriptor);
	}

	if(pDrawData == nullptr)
	{
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos), m.data());
	}
	else
	{
		SUniformPrimExGVertColor PushConstantColor = pDrawData->m_Color;
		SUniformPrimExGPos PushConstantVertex;
		mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
		PushConstantVertex.m_Rotation = pDrawData->m_Rotation;
		PushConstantVertex.m_Center = {pDrawData->m_RotationCenter.x, pDrawData->m_RotationCenter.y};

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstantVertex), &PushConstantVertex);
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformPrimExGPos) + sizeof(SUniformPrimExGVertColorAlign), sizeof(PushConstantColor), &PushConstantColor);
	}

	vkCmdDrawIndexed(CommandBuffer, pCommand->m_IndexCount, 1, 0, 0, 0);

	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_DrawIndexedDualAtlas(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
{
	const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataDualAtlas>();
	if(pDrawData == nullptr)
		return true;

	std::array<float, (size_t)4 * 2> m;
	GetStateMatrix(pCommand->m_State, m);

	bool IsTextured;
	size_t BlendModeIndex;
	size_t AddressModeIndex;
	GetStateIndices(pCommand->m_State, IsTextured, BlendModeIndex, AddressModeIndex);
	IsTextured = true;
	auto &PipeLayout = GetPipeLayout(m_DualAtlasPipeline, IsTextured, BlendModeIndex);
	auto &PipeLine = GetPipeline(m_DualAtlasPipeline, IsTextured, BlendModeIndex);

	auto &CommandBuffer = GetMainGraphicCommandBuffer();

	BindPipeline(CommandBuffer, ExecBuffer, PipeLine);
	std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
	std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
	vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());
	vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, static_cast<VkDeviceSize>(ExecBuffer.m_IndexBufferOff + pCommand->m_IndexOffset), VK_INDEX_TYPE_UINT32);
	BindDescriptorSet(CommandBuffer, PipeLayout, 0, ExecBuffer.m_aDescriptors[0].m_Descriptor);

	SUniformGTextPos PosTexSizeConstant;
	mem_copy(PosTexSizeConstant.m_aPos, m.data(), m.size() * sizeof(float));
	PosTexSizeConstant.m_TextureSize = pDrawData->m_TextureSize;
	vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGTextPos), &PosTexSizeConstant);

	SUniformTextGFragmentConstants FragmentConstants;
	FragmentConstants.m_TextColor = pDrawData->m_PrimaryColor;
	FragmentConstants.m_TextOutlineColor = pDrawData->m_SecondaryColor;
	vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformGTextPos) + sizeof(SUniformTextGFragmentOffset), sizeof(FragmentConstants), &FragmentConstants);
	vkCmdDrawIndexed(CommandBuffer, pCommand->m_IndexCount, 1, 0, 0, 0);
	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_DrawIndexedArrayColor(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
{
	const bool HasTransform = pCommand->m_Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM;
	const auto *pColorData = HasTransform ? nullptr : pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColor>();
	const auto *pTransformData = HasTransform ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColorTransform>() : nullptr;
	if((HasTransform && pTransformData == nullptr) || (!HasTransform && pColorData == nullptr))
		return true;
	const ColorRGBA &Color = HasTransform ? pTransformData->m_Color : pColorData->m_Color;
	const vec2 Scale = HasTransform ? pTransformData->m_Scale : vec2();
	const vec2 Offset = HasTransform ? pTransformData->m_Offset : vec2();
	return RenderArrayColor(ExecBuffer, pCommand->m_State, HasTransform, Color, Scale, Offset, pCommand->m_IndexCount, pCommand->m_IndexOffset);
}

bool CCommandProcessorFragment_Vulkan::Cmd_DrawIndexedQuadRecords(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
{
	const bool Grouped = pCommand->m_Program == EPipelineProgram::QUAD_SHARED;
	const uint32_t QuadCount = pCommand->m_IndexCount / 6;
	const auto *pQuadData = Grouped ?
					pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataQuadTransform>() :
					pCommand->m_ArrayData.Get<CCommandBuffer::SDrawDataQuadTransform>(QuadCount);
	if(pQuadData == nullptr)
		return true;

	std::array<float, (size_t)4 * 2> m;
	GetStateMatrix(pCommand->m_State, m);
	const bool CanBeGrouped = Grouped || QuadCount == 1;

	bool IsTextured;
	size_t BlendModeIndex;
	size_t AddressModeIndex;
	GetStateIndices(pCommand->m_State, IsTextured, BlendModeIndex, AddressModeIndex);
	auto &PipeLayout = GetPipeLayout(CanBeGrouped ? m_QuadSharedPipeline : m_QuadPerItemPipeline, IsTextured, BlendModeIndex);
	auto &PipeLine = GetPipeline(CanBeGrouped ? m_QuadSharedPipeline : m_QuadPerItemPipeline, IsTextured, BlendModeIndex);

	auto &CommandBuffer = GetMainGraphicCommandBuffer();

	BindPipeline(CommandBuffer, ExecBuffer, PipeLine);
	std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
	std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
	vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());
	vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, static_cast<VkDeviceSize>(ExecBuffer.m_IndexBufferOff + pCommand->m_IndexOffset), VK_INDEX_TYPE_UINT32);

	if(IsTextured)
		BindDescriptorSet(CommandBuffer, PipeLayout, 0, ExecBuffer.m_aDescriptors[0].m_Descriptor);

	const size_t BaseQuadOffset = pCommand->m_IndexOffset / (6 * sizeof(uint32_t));
	if(CanBeGrouped)
	{
		static_assert(sizeof(CCommandBuffer::SDrawDataQuadTransform) == sizeof(SUniformQuadPushGBufferObject));
		SUniformQuadGroupedGPos PushConstantVertex;
		mem_copy(&PushConstantVertex.m_BOPush, pQuadData, sizeof(PushConstantVertex.m_BOPush));
		mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SUniformQuadGroupedGPos), &PushConstantVertex);
		vkCmdDrawIndexed(CommandBuffer, pCommand->m_IndexCount, 1, 0, 0, 0);
		return true;
	}

	SUniformQuadGPos PushConstantVertex;
	mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
	PushConstantVertex.m_QuadOffset = static_cast<int32_t>(BaseQuadOffset);
	vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstantVertex), &PushConstantVertex);

	uint32_t QuadsLeft = QuadCount;
	size_t RenderOffset = 0;
	while(QuadsLeft > 0)
	{
		const uint32_t RealDrawCount = std::min<uint32_t>(QuadsLeft, GRAPHICS_MAX_QUADS_RENDER_COUNT);
		SDeviceDescriptorSet UniDescrSet;
		if(!GetUniformBufferObject(true, UniDescrSet, reinterpret_cast<const float *>(pQuadData + RenderOffset), RealDrawCount * sizeof(CCommandBuffer::SDrawDataQuadTransform)))
			return false;
		BindDescriptorSet(CommandBuffer, PipeLayout, IsTextured ? 1 : 0, UniDescrSet.m_Descriptor);
		if(RenderOffset > 0)
		{
			const int32_t QuadOffset = static_cast<int32_t>(BaseQuadOffset + RenderOffset);
			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(SUniformQuadGPos) - sizeof(int32_t), sizeof(int32_t), &QuadOffset);
		}
		vkCmdDrawIndexed(CommandBuffer, RealDrawCount * 6, 1, static_cast<uint32_t>(RenderOffset * 6), 0, 0);
		RenderOffset += RealDrawCount;
		QuadsLeft -= RealDrawCount;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_DrawIndexedInstanced(const CCommandBuffer::SCommand_DrawIndexed *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
{
	const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveInstanced>();
	const auto *pInstanceData = pCommand->m_ArrayData.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(pCommand->m_InstanceCount);
	if(pDrawData == nullptr || pInstanceData == nullptr || pCommand->m_InstanceCount == 0)
		return true;

	std::array<float, (size_t)4 * 2> m;
	GetStateMatrix(pCommand->m_State, m);

	bool CanBePushed = pCommand->m_InstanceCount <= 1;

	bool IsTextured;
	size_t BlendModeIndex;
	size_t AddressModeIndex;
	GetStateIndices(pCommand->m_State, IsTextured, BlendModeIndex, AddressModeIndex);
	auto &PipeLayout = GetPipeLayout(CanBePushed ? m_PrimitiveInstancedPushPipeline : m_PrimitiveInstancedPipeline, IsTextured, BlendModeIndex);
	auto &PipeLine = GetPipeline(CanBePushed ? m_PrimitiveInstancedPushPipeline : m_PrimitiveInstancedPipeline, IsTextured, BlendModeIndex);

	auto &CommandBuffer = GetMainGraphicCommandBuffer();

	BindPipeline(CommandBuffer, ExecBuffer, PipeLine);

	std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
	std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
	vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

	VkDeviceSize IndexOffset = static_cast<VkDeviceSize>(ExecBuffer.m_IndexBufferOff + pCommand->m_IndexOffset);
	vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, IndexOffset, VK_INDEX_TYPE_UINT32);

	BindDescriptorSet(CommandBuffer, PipeLayout, 0, ExecBuffer.m_aDescriptors[0].m_Descriptor);

	if(CanBePushed)
	{
		SUniformSpriteMultiPushGVertColor PushConstantColor;
		SUniformSpriteMultiPushGPos PushConstantVertex;

		PushConstantColor = pDrawData->m_Color;

		mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
		PushConstantVertex.m_Center = pDrawData->m_RotationCenter;

		for(size_t i = 0; i < pCommand->m_InstanceCount; ++i)
			PushConstantVertex.m_aPSR[i] = vec4(pInstanceData[i].m_Position.x, pInstanceData[i].m_Position.y, pInstanceData[i].m_Scale, pInstanceData[i].m_Rotation);

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformSpriteMultiPushGPosBase) + sizeof(vec4) * pCommand->m_InstanceCount, &PushConstantVertex);
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiPushGPos), sizeof(PushConstantColor), &PushConstantColor);
	}
	else
	{
		SUniformSpriteMultiGVertColor PushConstantColor;
		SUniformSpriteMultiGPos PushConstantVertex;

		PushConstantColor = pDrawData->m_Color;

		mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
		PushConstantVertex.m_Center = pDrawData->m_RotationCenter;

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstantVertex), &PushConstantVertex);
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiGPos) + sizeof(SUniformSpriteMultiGVertColorAlign), sizeof(PushConstantColor), &PushConstantColor);
	}

	const uint32_t InstancesPerDraw = GRAPHICS_MAX_PARTICLES_RENDER_COUNT;
	uint32_t InstanceCount = pCommand->m_InstanceCount;
	size_t RenderOffset = 0;

	while(InstanceCount > 0)
	{
		const uint32_t BatchInstanceCount = std::min(InstanceCount, InstancesPerDraw);

		if(!CanBePushed)
		{
			// create uniform buffer
			SDeviceDescriptorSet UniDescrSet;
			if(!GetUniformBufferObject(false, UniDescrSet, reinterpret_cast<const float *>(pInstanceData + RenderOffset), BatchInstanceCount * sizeof(*pInstanceData)))
				return false;

			BindDescriptorSet(CommandBuffer, PipeLayout, 1, UniDescrSet.m_Descriptor);
		}

		vkCmdDrawIndexed(CommandBuffer, pCommand->m_IndexCount, BatchInstanceCount, 0, 0, 0);

		RenderOffset += BatchInstanceCount;
		InstanceCount -= BatchInstanceCount;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Readback: a frame or a render target back into the client.
// ---------------------------------------------------------------------------

bool CCommandProcessorFragment_Vulkan::PrepareReadbackRecording()
{
	if(m_RenderPassActive || !FlushRenderCommands())
		return false;
	UploadNonFlushedBuffers<true>();
	if(!m_FrameCommandsRecording)
		return WaitForFrameSlot() && RestartReadbackCommandBuffer(GetMainGraphicCommandBuffer());
	return true;
}

bool CCommandProcessorFragment_Vulkan::SubmitReadbackRecording(bool WithFrame, VkCommandBuffer &CommandBuffer)
{
	bool HasGpuTimestamp = false;
	if(WithFrame)
		HasGpuTimestamp = EndGpuTimestamp(CommandBuffer);
	if(vkEndCommandBuffer(CommandBuffer) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Ending the image readback command buffer failed.");
		return false;
	}

	std::array<VkCommandBuffer, 2> aCommandBuffers{};
	VkSubmitInfo SubmitInfo{};
	SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	SubmitInfo.commandBufferCount = 1;
	SubmitInfo.pCommandBuffers = &CommandBuffer;
	if(WithFrame && m_vUsedMemoryCommandBuffer[m_CurImageIndex])
	{
		auto &MemoryCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
		if(vkEndCommandBuffer(MemoryCommandBuffer) != VK_SUCCESS)
		{
			SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Ending the pending readback memory command buffer failed.");
			return false;
		}
		aCommandBuffers = {MemoryCommandBuffer, CommandBuffer};
		SubmitInfo.commandBufferCount = aCommandBuffers.size();
		SubmitInfo.pCommandBuffers = aCommandBuffers.data();
	}
	m_vUsedMemoryCommandBuffer[m_CurImageIndex] = false;

	const VkPipelineStageFlags WaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	if(WithFrame && m_AcquireSemaphorePending)
	{
		SubmitInfo.waitSemaphoreCount = 1;
		SubmitInfo.pWaitSemaphores = &m_AcquireImageSemaphore;
		SubmitInfo.pWaitDstStageMask = &WaitStage;
	}
	// The present that follows waits on this, so it has to be signalled here
	// exactly as SubmitFrameCommands would have signalled it. Without a
	// surface nothing ever waits on it, and a semaphore that is signalled
	// twice without a wait in between is not allowed.
	if(WithFrame && m_Presentation.IsPresentable() && !m_RenderingPaused && m_CurImageIndex < m_vQueueSubmitSemaphores.size())
	{
		SubmitInfo.signalSemaphoreCount = 1;
		SubmitInfo.pSignalSemaphores = &m_vQueueSubmitSemaphores[m_CurImageIndex];
	}

	if(vkResetFences(m_VKDevice, 1, &m_vQueueSubmitFences[m_CurImageIndex]) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Resetting the image readback fence failed.");
		return false;
	}
	const VkResult SubmitResult = vkQueueSubmit(m_VKGraphicsQueue, 1, &SubmitInfo, m_vQueueSubmitFences[m_CurImageIndex]);
	if(SubmitResult != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Submitting the image readback failed.", CheckVulkanCriticalError(SubmitResult));
		return false;
	}
	if(WithFrame)
	{
		if(HasGpuTimestamp)
			m_vGpuTimestampPending[m_CurImageIndex] = true;
		m_AcquireSemaphorePending = false;
		m_FrameCommandsRecording = false;
		// A presented frame gets its next slot from the swapchain, a
		// surface-less one has no swap to take it there.
		if(!m_Presentation.IsPresentable())
		{
			m_CurImageIndex = (m_CurImageIndex + 1) % m_SwapChainImageCount;
			return PrepareOffscreenCommands();
		}
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::RestartReadbackCommandBuffer(VkCommandBuffer CommandBuffer)
{
	if(vkResetCommandBuffer(CommandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Resetting the readback command buffer failed.");
		return false;
	}
	VkCommandBufferBeginInfo BeginInfo{};
	BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if(vkBeginCommandBuffer(CommandBuffer, &BeginInfo) != VK_SUCCESS)
	{
		SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_RECORDING, "Beginning the readback command buffer failed.");
		return false;
	}
	m_FrameCommandsRecording = true;
	return true;
}

bool CCommandProcessorFragment_Vulkan::PrepareReadbackSlotImage(SReadbackSlot &Slot, uint32_t Width, uint32_t Height)
{
	if(Slot.m_Image != VK_NULL_HANDLE && Width == Slot.m_Width && Height == Slot.m_Height)
		return true;
	DeleteReadbackSlotImage(Slot);

	VkImageCreateInfo ImageInfo{};
	ImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ImageInfo.imageType = VK_IMAGE_TYPE_2D;
	ImageInfo.extent.width = Width;
	ImageInfo.extent.height = Height;
	ImageInfo.extent.depth = 1;
	ImageInfo.mipLevels = 1;
	ImageInfo.arrayLayers = 1;
	ImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	ImageInfo.tiling = VK_IMAGE_TILING_LINEAR;
	ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	ImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	const VkResult CreateResult = vkCreateImage(m_VKDevice, &ImageInfo, nullptr, &Slot.m_Image);
	if(CreateResult != VK_SUCCESS)
	{
		Slot.m_Image = VK_NULL_HANDLE;
		SetError(MemoryErrorType(CreateResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE), "Creating the presented image readback helper failed.");
		return false;
	}
	// Create memory to back up the image
	VkMemoryRequirements MemRequirements;
	vkGetImageMemoryRequirements(m_VKDevice, Slot.m_Image, &MemRequirements);

	VkMemoryAllocateInfo MemAllocInfo{};
	MemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	MemAllocInfo.allocationSize = MemRequirements.size;
	MemAllocInfo.memoryTypeIndex = FindMemoryType(m_VKGPU, MemRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

	const VkResult AllocateResult = vkAllocateMemory(m_VKDevice, &MemAllocInfo, nullptr, &Slot.m_Mem.m_Mem);
	if(AllocateResult != VK_SUCCESS)
	{
		Slot.m_Mem.m_Mem = VK_NULL_HANDLE;
		SetError(MemoryErrorType(AllocateResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE), "Allocating presented image readback memory failed.");
		DeleteReadbackSlotImage(Slot);
		return false;
	}
	const VkResult BindResult = vkBindImageMemory(m_VKDevice, Slot.m_Image, Slot.m_Mem.m_Mem, 0);
	if(BindResult != VK_SUCCESS)
	{
		SetError(MemoryErrorType(BindResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE), "Binding presented image readback memory failed.");
		DeleteReadbackSlotImage(Slot);
		return false;
	}

	VkImageSubresource SubResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
	VkSubresourceLayout SubResourceLayout;
	vkGetImageSubresourceLayout(m_VKDevice, Slot.m_Image, &SubResource, &SubResourceLayout);

	const VkResult MapResult = vkMapMemory(m_VKDevice, Slot.m_Mem.m_Mem, 0, VK_WHOLE_SIZE, 0, (void **)&Slot.m_pMappedMemory);
	if(MapResult != VK_SUCCESS)
	{
		Slot.m_pMappedMemory = nullptr;
		SetError(MemoryErrorType(MapResult, GFX_ERROR_TYPE_OUT_OF_MEMORY_STAGING), "Mapping presented image readback memory failed.");
		DeleteReadbackSlotImage(Slot);
		return false;
	}
	Slot.m_MappedLayoutOffset = SubResourceLayout.offset;
	Slot.m_MappedLayoutPitch = SubResourceLayout.rowPitch;
	Slot.m_pMappedMemory += Slot.m_MappedLayoutOffset;

	if(!ImageBarrier(Slot.m_Image, 0, 1, 0, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL))
	{
		DeleteReadbackSlotImage(Slot);
		return false;
	}

	Slot.m_Width = Width;
	Slot.m_Height = Height;
	return true;
}

void CCommandProcessorFragment_Vulkan::DeleteReadbackSlotImage(SReadbackSlot &Slot)
{
	if(Slot.m_Image != VK_NULL_HANDLE)
		vkDestroyImage(m_VKDevice, Slot.m_Image, nullptr);
	if(Slot.m_pMappedMemory != nullptr)
		vkUnmapMemory(m_VKDevice, Slot.m_Mem.m_Mem);
	if(Slot.m_Mem.m_Mem != VK_NULL_HANDLE)
		vkFreeMemory(m_VKDevice, Slot.m_Mem.m_Mem, nullptr);

	Slot.m_Image = VK_NULL_HANDLE;
	Slot.m_Mem = {};
	Slot.m_pMappedMemory = nullptr;
	Slot.m_MappedLayoutOffset = 0;
	Slot.m_MappedLayoutPitch = 0;
	Slot.m_Width = 0;
	Slot.m_Height = 0;
}

bool CCommandProcessorFragment_Vulkan::CollectReadbackSlot(size_t Index)
{
	if(Index >= m_vReadbackSlots.size())
		return true;
	SReadbackSlot &Slot = m_vReadbackSlots[Index];
	CCommandBuffer::SImageReadbackResult *pResult = Slot.m_pResult;
	if(pResult == nullptr)
		return true;
	Slot.m_pResult = nullptr;

	VkMappedMemoryRange MemRange{};
	MemRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	MemRange.memory = Slot.m_Mem.m_Mem;
	MemRange.offset = Slot.m_MappedLayoutOffset;
	MemRange.size = VK_WHOLE_SIZE;
	if(vkInvalidateMappedMemoryRanges(m_VKDevice, 1, &MemRange) != VK_SUCCESS)
	{
		pResult->Signal();
		SetError(EGfxErrorType::GFX_ERROR_TYPE_UNKNOWN, "Invalidating the image readback memory failed.");
		return false;
	}

	CImageInfo &Image = pResult->m_Image;
	const uint32_t Width = Slot.m_Width;
	const uint32_t Height = Slot.m_Height;
	if(!Image.TryReuse(Width, Height, CImageInfo::FORMAT_RGBA))
	{
		pResult->Signal();
		SetError(EGfxErrorType::GFX_ERROR_TYPE_OUT_OF_MEMORY_IMAGE, "Allocating the image readback destination failed.");
		return false;
	}

	// The driver may lay the rows out wider than the image is.
	const size_t RowSize = (size_t)Width * 4;
	const size_t Pitch = Slot.m_MappedLayoutPitch;
	if(Pitch == RowSize)
	{
		mem_copy(Image.m_pData, Slot.m_pMappedMemory, RowSize * Height);
	}
	else
	{
		for(uint32_t Y = 0; Y < Height; ++Y)
			mem_copy(Image.m_pData + Y * RowSize, Slot.m_pMappedMemory + (size_t)Y * Pitch, RowSize);
	}

	if(Slot.m_IsB8G8R8A8 || Slot.m_ResetAlpha)
	{
		for(uint32_t Y = 0; Y < Height; ++Y)
		{
			for(uint32_t X = 0; X < Width; ++X)
			{
				const size_t ImgOff = (Y * RowSize) + (X * 4);
				if(Slot.m_IsB8G8R8A8)
				{
					std::swap(Image.m_pData[ImgOff], Image.m_pData[ImgOff + 2]);
				}
				if(Slot.m_ResetAlpha)
					Image.m_pData[ImgOff + 3] = 255;
			}
		}
	}

	pResult->m_Ok = true;
	pResult->Signal();
	return true;
}

void CCommandProcessorFragment_Vulkan::AbandonReadbackSlot(SReadbackSlot &Slot)
{
	if(Slot.m_pResult == nullptr)
		return;
	Slot.m_pResult->Signal();
	Slot.m_pResult = nullptr;
}

bool CCommandProcessorFragment_Vulkan::CollectFinishedReadbacks()
{
	for(size_t Index = 0; Index < m_vReadbackSlots.size(); ++Index)
	{
		if(m_vReadbackSlots[Index].m_pResult == nullptr || Index >= m_vQueueSubmitFences.size())
			continue;
		if(vkGetFenceStatus(m_VKDevice, m_vQueueSubmitFences[Index]) != VK_SUCCESS)
			continue;
		if(!CollectReadbackSlot(Index))
			return false;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::FinishReadbacks()
{
	for(size_t Index = 0; Index < m_vReadbackSlots.size(); ++Index)
	{
		if(m_vReadbackSlots[Index].m_pResult == nullptr)
			continue;
		if(Index >= m_vQueueSubmitFences.size())
		{
			AbandonReadbackSlot(m_vReadbackSlots[Index]);
			continue;
		}
		const VkResult WaitResult = vkWaitForFences(m_VKDevice, 1, &m_vQueueSubmitFences[Index], VK_TRUE, std::numeric_limits<uint64_t>::max());
		if(WaitResult != VK_SUCCESS)
		{
			AbandonReadbackSlot(m_vReadbackSlots[Index]);
			SetError(EGfxErrorType::GFX_ERROR_TYPE_RENDER_SUBMIT_FAILED, "Waiting for the image readback failed.", CheckVulkanCriticalError(WaitResult));
			return false;
		}
		if(!CollectReadbackSlot(Index))
			return false;
	}
	return true;
}

void CCommandProcessorFragment_Vulkan::DestroyReadbackSlots()
{
	for(SReadbackSlot &Slot : m_vReadbackSlots)
	{
		AbandonReadbackSlot(Slot);
		DeleteReadbackSlotImage(Slot);
	}
	m_vReadbackSlots.clear();
}

bool CCommandProcessorFragment_Vulkan::StartImageReadback(VkImage SourceImage, VkFormat SourceFormat, VkImageLayout SourceLayout, uint32_t SourceWidth, uint32_t SourceHeight, CCommandBuffer::SImageReadbackResult *pResult, bool ResetAlpha, const std::optional<ivec2> &PixelOffset, bool SubmitPendingGraphics)
{
	bool IsB8G8R8A8 = SourceFormat == VK_FORMAT_B8G8R8A8_UNORM;
	const bool UsesRGBALikeFormat = SourceFormat == VK_FORMAT_R8G8B8A8_UNORM || IsB8G8R8A8;
	if(!UsesRGBALikeFormat)
	{
		log_error("gfx/vulkan", "Source image was not in an RGBA-like format.");
		return false;
	}

	uint32_t Width;
	uint32_t Height;
	VkOffset3D SrcOffset;
	if(PixelOffset.has_value())
	{
		SrcOffset.x = PixelOffset.value().x;
		SrcOffset.y = PixelOffset.value().y;
		Width = 1;
		Height = 1;
	}
	else
	{
		SrcOffset.x = 0;
		SrcOffset.y = 0;
		Width = SourceWidth;
		Height = SourceHeight;
	}
	SrcOffset.z = 0;

	// The frame that is being read back has just been recorded, and the
	// copy that reads it can go into the same command buffer. Submitting
	// the frame first and the copy after it means waiting for the queue
	// twice per frame, which for a video export is twice per frame of
	// the video.
	VkCommandBuffer *pCommandBuffer;
	if(SubmitPendingGraphics)
	{
		if(!PrepareReadbackRecording())
			return false;
		pCommandBuffer = &GetMainGraphicCommandBuffer();
	}
	else if(!GetMemoryCommandBuffer(pCommandBuffer))
		return false;
	VkCommandBuffer &CommandBuffer = *pCommandBuffer;

	// The slot belongs to the frame the copy rides on, and that frame's
	// previous readback was collected when the slot was waited for.
	SReadbackSlot &Slot = m_vReadbackSlots[m_CurImageIndex];
	dbg_assert(Slot.m_pResult == nullptr, "graphics: frame slot still owed a readback");
	if(!PrepareReadbackSlotImage(Slot, Width, Height))
		return false;

	if(!ImageBarrierIn(CommandBuffer, Slot.m_Image, 0, 1, 0, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
		return false;
	if(!ImageBarrierIn(CommandBuffer, SourceImage, 0, 1, 0, 1, SourceFormat, SourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
		return false;

	// If source and destination support blit we'll blit as this also does automatic format conversion (e.g. from BGR to RGB)
	const bool SourceCanBlit = SourceFormat == m_VKSurfFormat.format ? m_OptimalSwapChainImageBlitting : m_OptimalRGBAImageBlitting;
	if(SourceCanBlit && m_LinearRGBAImageBlitting)
	{
		VkOffset3D BlitSize;
		BlitSize.x = Width;
		BlitSize.y = Height;
		BlitSize.z = 1;

		VkImageBlit ImageBlitRegion{};
		ImageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ImageBlitRegion.srcSubresource.layerCount = 1;
		ImageBlitRegion.srcOffsets[0] = SrcOffset;
		ImageBlitRegion.srcOffsets[1] = {SrcOffset.x + BlitSize.x, SrcOffset.y + BlitSize.y, SrcOffset.z + BlitSize.z};
		ImageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ImageBlitRegion.dstSubresource.layerCount = 1;
		ImageBlitRegion.dstOffsets[1] = BlitSize;

		// Issue the blit command
		vkCmdBlitImage(CommandBuffer, SourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			Slot.m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &ImageBlitRegion, VK_FILTER_NEAREST);

		// transformed to RGBA
		IsB8G8R8A8 = false;
	}
	else
	{
		// Otherwise use image copy (requires us to manually flip components)
		VkImageCopy ImageCopyRegion{};
		ImageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ImageCopyRegion.srcSubresource.layerCount = 1;
		ImageCopyRegion.srcOffset = SrcOffset;
		ImageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ImageCopyRegion.dstSubresource.layerCount = 1;
		ImageCopyRegion.extent.width = Width;
		ImageCopyRegion.extent.height = Height;
		ImageCopyRegion.extent.depth = 1;

		// Issue the copy command
		vkCmdCopyImage(CommandBuffer, SourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			Slot.m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &ImageCopyRegion);
	}

	if(!ImageBarrierIn(CommandBuffer, Slot.m_Image, 0, 1, 0, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL))
		return false;
	if(!ImageBarrierIn(CommandBuffer, SourceImage, 0, 1, 0, 1, SourceFormat, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SourceLayout))
		return false;

	Slot.m_IsB8G8R8A8 = IsB8G8R8A8;
	Slot.m_ResetAlpha = ResetAlpha;
	Slot.m_pResult = pResult;
	if(!SubmitReadbackRecording(SubmitPendingGraphics, CommandBuffer))
	{
		AbandonReadbackSlot(Slot);
		return false;
	}
	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
	if(!m_TextureHandles.IsActive(pCommand->m_Texture))
		return true;
	const CTexture &Texture = m_vTextures[pCommand->m_Texture.Id()];
	if((Texture.m_Usage & IGraphics::TEXTURE_USAGE_COLOR_TARGET) == 0 || (Texture.m_Usage & IGraphics::TEXTURE_USAGE_COPY_SOURCE) == 0 || Texture.m_Layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		return true;

	if(!StartImageReadback(Texture.m_Img, Texture.m_ImageFormat, Texture.m_Layout, Texture.m_Width, Texture.m_Height, pCommand->m_pResult, false, {}, true))
		return false;
	// The result is signalled when the copy lands, not when this buffer ends.
	pCommand->m_pCompletion = nullptr;
	return true;
}

bool CCommandProcessorFragment_Vulkan::Cmd_PresentationTargetReadback(const CCommandBuffer::SCommand_PresentationTarget_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
	if(m_RenderingPaused || m_RenderPassActive)
		return true;
	const auto Viewport = m_VKSwapImgAndViewportExtent.GetPresentedImageViewport();
	if(pCommand->m_ReadPixel && (pCommand->m_Position.x < 0 || pCommand->m_Position.x >= static_cast<int>(Viewport.width) || pCommand->m_Position.y < 0 || pCommand->m_Position.y >= static_cast<int>(Viewport.height)))
		return true;
	const std::optional<ivec2> PixelOffset = pCommand->m_ReadPixel ? std::optional<ivec2>(pCommand->m_Position) : std::nullopt;
	if(!StartImageReadback(m_vSwapChainImages[m_CurImageIndex], m_VKSurfFormat.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, Viewport.width, Viewport.height, pCommand->m_pResult, true, PixelOffset, true))
		return true;
	// The result is signalled when the copy lands, not when this buffer ends.
	pCommand->m_pCompletion = nullptr;

	return true;
}

CCommandProcessorFragment_Renderer *CreateVulkanCommandProcessorFragment()
{
	return new CCommandProcessorFragment_Vulkan();
}

#endif
