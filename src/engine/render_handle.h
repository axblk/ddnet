#ifndef ENGINE_RENDER_HANDLE_H
#define ENGINE_RENDER_HANDLE_H

#include <cstddef>
#include <cstdint>
#include <vector>

template<typename THandle>
class CGenerationHandlePool;

template<typename TTag>
class CGenerationHandle
{
	template<typename THandle>
	friend class CGenerationHandlePool;

	int m_Id = -1;
	uint32_t m_Generation = 0;

protected:
	CGenerationHandle(int Id, uint32_t Generation) :
		m_Id(Id),
		m_Generation(Generation)
	{
	}

public:
	CGenerationHandle() = default;

	bool IsValid() const { return m_Id >= 0 && m_Generation != 0; }
	int Id() const { return m_Id; }
	uint32_t Generation() const { return m_Generation; }
	void Invalidate()
	{
		m_Id = -1;
		m_Generation = 0;
	}

	bool operator==(const CGenerationHandle &Other) const
	{
		return m_Id == Other.m_Id && m_Generation == Other.m_Generation;
	}
	bool operator!=(const CGenerationHandle &Other) const { return !(*this == Other); }
};

template<typename THandle>
class CGenerationHandlePool
{
	struct SSlot
	{
		int m_NextFree = -1;
		uint32_t m_Generation = 1;
		bool m_Allocated = false;
		bool m_Retired = false;
	};

	std::vector<SSlot> m_vSlots;
	int m_FirstFree = -1;

public:
	void Reset(size_t InitialSize)
	{
		m_vSlots.clear();
		m_vSlots.resize(InitialSize);
		for(size_t i = 0; i < InitialSize; ++i)
			m_vSlots[i].m_NextFree = i + 1 < InitialSize ? static_cast<int>(i + 1) : -1;
		m_FirstFree = InitialSize == 0 ? -1 : 0;
	}

	THandle Allocate()
	{
		if(m_FirstFree == -1)
		{
			m_FirstFree = static_cast<int>(m_vSlots.size());
			m_vSlots.emplace_back();
		}

		const int Id = m_FirstFree;
		SSlot &Slot = m_vSlots[Id];
		m_FirstFree = Slot.m_NextFree;
		Slot.m_NextFree = -1;
		Slot.m_Allocated = true;
		Slot.m_Retired = false;
		return THandle(Id, Slot.m_Generation);
	}

	bool IsAllocated(THandle Handle) const
	{
		return Handle.IsValid() && static_cast<size_t>(Handle.Id()) < m_vSlots.size() &&
		       m_vSlots[Handle.Id()].m_Allocated && m_vSlots[Handle.Id()].m_Generation == Handle.Generation();
	}

	bool Release(THandle *pHandle)
	{
		if(pHandle == nullptr)
			return false;
		const THandle Handle = *pHandle;
		if(!Retire(pHandle))
			return false;
		return Recycle(Handle);
	}

	bool Retire(THandle *pHandle)
	{
		if(pHandle == nullptr || !IsAllocated(*pHandle))
			return false;

		SSlot &Slot = m_vSlots[pHandle->Id()];
		Slot.m_Allocated = false;
		Slot.m_Retired = true;
		if(++Slot.m_Generation == 0)
			++Slot.m_Generation;
		pHandle->Invalidate();
		return true;
	}

	bool Recycle(THandle RetiredHandle)
	{
		if(!RetiredHandle.IsValid() || static_cast<size_t>(RetiredHandle.Id()) >= m_vSlots.size())
			return false;
		SSlot &Slot = m_vSlots[RetiredHandle.Id()];
		uint32_t ExpectedGeneration = RetiredHandle.Generation() + 1;
		if(ExpectedGeneration == 0)
			++ExpectedGeneration;
		if(!Slot.m_Retired || Slot.m_Allocated || Slot.m_Generation != ExpectedGeneration)
			return false;
		Slot.m_Retired = false;
		Slot.m_NextFree = m_FirstFree;
		m_FirstFree = RetiredHandle.Id();
		return true;
	}
};

template<typename THandle>
class CGenerationHandleStore
{
	std::vector<uint32_t> m_vGenerations;

public:
	bool Activate(THandle Handle)
	{
		if(!Handle.IsValid())
			return false;
		if(static_cast<size_t>(Handle.Id()) >= m_vGenerations.size())
			m_vGenerations.resize(Handle.Id() + 1);
		uint32_t &Generation = m_vGenerations[Handle.Id()];
		if(Generation != 0)
			return false;
		Generation = Handle.Generation();
		return true;
	}

	bool IsActive(THandle Handle) const
	{
		return Handle.IsValid() && static_cast<size_t>(Handle.Id()) < m_vGenerations.size() &&
		       m_vGenerations[Handle.Id()] == Handle.Generation();
	}

	bool Release(THandle Handle)
	{
		if(!IsActive(Handle))
			return false;
		m_vGenerations[Handle.Id()] = 0;
		return true;
	}

	void Clear() { m_vGenerations.clear(); }
};

#endif
