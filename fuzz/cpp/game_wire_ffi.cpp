// C entry points into the C++ framing, so that the differential fuzz target can
// feed the same bytes to it and to `game_wire.rs`. The two implementations are
// independent on purpose, see the comment at the top of `game_wire.h`; the
// golden vectors only cover the cases somebody wrote down, this covers the rest.
#include <engine/shared/game_wire.h>

#include <cstring>

// game_wire.cpp is the only file linked here, and the one base function it uses
// is defined in system.cpp, which would drag in the whole engine.
void mem_copy(void *pDest, const void *pSource, size_t Size)
{
	memcpy(pDest, pSource, Size);
}

namespace {
int ResultCode(GameWire::EDecodeResult Result)
{
	switch(Result)
	{
	case GameWire::EDecodeResult::OK: return 0;
	case GameWire::EDecodeResult::NEED_MORE: return 1;
	case GameWire::EDecodeResult::MALFORMED: return 2;
	case GameWire::EDecodeResult::LIMIT_EXCEEDED: return 3;
	case GameWire::EDecodeResult::UNKNOWN_REQUIRED: return 4;
	case GameWire::EDecodeResult::VERSION_MISMATCH: return 5;
	}
	return 2;
}
} // namespace

extern "C" int ddnet_fuzz_decode_varint(const unsigned char *pData, size_t Size, uint64_t *pValue, size_t *pBytesConsumed)
{
	uint64_t Value = 0;
	size_t BytesConsumed = 0;
	const int Code = ResultCode(GameWire::DecodeVarInt(pData, Size, Value, BytesConsumed));
	*pValue = Value;
	*pBytesConsumed = BytesConsumed;
	return Code;
}

extern "C" int ddnet_fuzz_decode_frame(const unsigned char *pData, size_t Size, uint64_t *pType, int *pSkippable, size_t *pPayloadOffset, size_t *pPayloadSize, size_t *pBytesConsumed)
{
	GameWire::CFrameView Frame = {};
	const int Code = ResultCode(GameWire::DecodeFrame(pData, Size, Frame));
	*pType = Frame.m_Type;
	*pSkippable = Frame.m_Skippable ? 1 : 0;
	*pPayloadOffset = Frame.m_Payload.m_pData == nullptr ? 0 : (size_t)(Frame.m_Payload.m_pData - pData);
	*pPayloadSize = Frame.m_Payload.m_Size;
	*pBytesConsumed = Frame.m_BytesConsumed;
	return Code;
}
