#ifndef ENGINE_CLIENT_STREAM_H
#define ENGINE_CLIENT_STREAM_H

#include <cstdint>

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

#endif // ENGINE_CLIENT_STREAM_H
