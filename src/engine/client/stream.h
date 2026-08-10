#ifndef ENGINE_CLIENT_STREAM_H
#define ENGINE_CLIENT_STREAM_H

#include <base/dbg.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

class CStreamId
{
	uint64_t m_Value = 0;

public:
	CStreamId() = default;
	explicit CStreamId(uint64_t Value) :
		m_Value(Value)
	{
	}

	bool IsValid() const { return m_Value != 0; }
	uint64_t Value() const { return m_Value; }
	bool operator==(const CStreamId &Other) const { return m_Value == Other.m_Value; }
	bool operator!=(const CStreamId &Other) const { return !(*this == Other); }
};

template<typename T>
class CStreamStorage
{
	std::vector<T> m_vValues;

public:
	explicit CStreamStorage(size_t InitialSize = 0) :
		m_vValues(InitialSize)
	{
	}

	decltype(auto) operator[](int Index)
	{
		dbg_assert(Index >= 0, "invalid stream storage index");
		if(static_cast<size_t>(Index) >= m_vValues.size())
			m_vValues.resize(Index + 1);
		return m_vValues[Index];
	}

	decltype(auto) operator[](int Index) const
	{
		dbg_assert(Index >= 0 && static_cast<size_t>(Index) < m_vValues.size(), "invalid stream storage index");
		return m_vValues[Index];
	}

	size_t size() const { return m_vValues.size(); }
	auto begin() { return m_vValues.begin(); }
	auto end() { return m_vValues.end(); }
	auto begin() const { return m_vValues.begin(); }
	auto end() const { return m_vValues.end(); }
	void Fill(const T &Value) { std::fill(m_vValues.begin(), m_vValues.end(), Value); }
};

#endif // ENGINE_CLIENT_STREAM_H
