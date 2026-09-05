#ifndef ENGINE_CLIENT_BACKEND_VULKAN_BACKEND_VULKAN_PROCESSOR_H
#define ENGINE_CLIENT_BACKEND_VULKAN_BACKEND_VULKAN_PROCESSOR_H

#if defined(CONF_BACKEND_VULKAN)

// The Vulkan backend's one class. The declaration is here; the definitions
// are in backend_vulkan.cpp, in sections by what they concern.

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/backend/vulkan/backend_vulkan.h>
#include <engine/client/command_buffer.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/localization.h>
#include <engine/storage.h>

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
constexpr uint64_t TimestampTickDelta(uint64_t Start, uint64_t End, uint32_t ValidBits)
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

constexpr VkPipelineColorBlendAttachmentState CreateColorBlendAttachment(EVulkanBackendBlendModes BlendMode)
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

	class IStorage *m_pStorage;

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
	// Every pipeline is built through this, and what it learned is written out
	// at shutdown and read back at the next start - see EnsurePipelineCache.
	VkPipelineCache m_PipelineCache = VK_NULL_HANDLE;
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
			if(vkCreateGraphicsPipelines(m_VKDevice, m_PipelineCache, 1, &PipelineInfo, nullptr, &Pipeline) != VK_SUCCESS)
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

	static constexpr const char *PIPELINE_CACHE_FILE = "vulkan_pipeline_cache.bin";

	// Pipelines are compiled by the driver, and there are sixty-odd of them
	// at every start. A pipeline cache keeps what the driver made of them, and
	// on disk it survives to the next start, which is then as long as reading
	// the file. The header says which device and driver the data is for; one
	// that does not match is not handed to the driver at all.
	void EnsurePipelineCache();

	void SavePipelineCache();

	void DestroyPipelineCache();

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

#endif

#endif
