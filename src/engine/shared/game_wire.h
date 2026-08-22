#ifndef ENGINE_SHARED_GAME_WIRE_H
#define ENGINE_SHARED_GAME_WIRE_H

#include <cstddef>
#include <cstdint>
#include <vector>

// This is one of two implementations of the same framing. The other one is
// `game_wire.rs`, which the native QUIC transport uses; this one is the whole
// protocol stack of the emscripten WebTransport client, which has no Rust.
// They are not layered on each other, they are held together by the golden
// vectors in `src/test/game_wire_test.cpp` and in the `#[cfg(test)]` block of
// `game_wire.rs`, which assert the same literal bytes. Change one, change both,
// including the values reserved below.
namespace GameWire
{
	constexpr uint64_t VERSION_MAJOR = 1;
	constexpr uint64_t VERSION_MINOR = 0;
	constexpr size_t NONCE_SIZE = 32;
	constexpr size_t MAX_RESUME_TOKEN_SIZE = 64;
	constexpr size_t MAX_HELLO_SIZE = 512;
	constexpr size_t MAX_CONTROL_MESSAGE_SIZE = 64 * 1024;
	constexpr size_t MAX_MAP_HEADER_SIZE = 4 * 1024;
	constexpr size_t MAX_MAP_NAME_SIZE = 255;
	constexpr size_t MAP_SHA256_SIZE = 32;
	constexpr uint64_t MAX_MAP_SIZE = 1024 * 1024 * 1024;
	constexpr size_t MAX_DATAGRAM_SIZE = 1000;
	constexpr size_t MAX_DATAGRAM_MESSAGE_SIZE = 960;
	constexpr uint64_t MAX_DATAGRAM_MESSAGES = 64;
	constexpr uint64_t MAX_VARINT = (uint64_t{1} << 62) - 1;

	enum class EDecodeResult
	{
		OK,
		NEED_MORE,
		MALFORMED,
		LIMIT_EXCEEDED,
		UNKNOWN_REQUIRED,
		VERSION_MISMATCH,
	};

	enum class EStreamKind : uint64_t
	{
		CONTROL = 0,
		MAP = 1,
		// Used by the native transport only, reserved so that this side does
		// not hand the number out twice.
		MASTER_CHALLENGE = 64,
	};

	enum class EFrameType : uint64_t
	{
		CLIENT_HELLO = 0,
		SERVER_HELLO = 1,
		MESSAGE = 2,
		DISCONNECT = 3,
		RESUME = 4,
		MAP_HEADER = 5,
		// Reserved as above. Both are skippable, so this side ignores them
		// instead of failing the connection.
		SERVER_IDENTITY = 64,
		CLIENT_IDENTITY_READY = 65,
	};

	enum class EDatagramType : uint64_t
	{
		MESSAGES = 0,
	};

	enum class EMapFormat : uint64_t
	{
		DATAFILE = 0,
	};

	enum ECapability : uint64_t
	{
		CAPABILITY_DATAGRAM = 1 << 0,
		CAPABILITY_MAP_STREAM = 1 << 1,
		CAPABILITY_RESUME = 1 << 2,
		// Reserved as above; this side never announces it.
		CAPABILITY_SERVER_IDENTITY = 1 << 3,
		CAPABILITY_GAME_PROTOCOL_7 = 1 << 4,
	};

	struct CByteView
	{
		const unsigned char *m_pData;
		size_t m_Size;
	};

	struct CStreamHeader
	{
		EStreamKind m_Kind;
		size_t m_BytesConsumed;
	};

	struct CFrameView
	{
		uint64_t m_Type;
		CByteView m_Payload;
		size_t m_BytesConsumed;
		bool m_Skippable;
	};

	struct CHelloView
	{
		uint64_t m_Major;
		uint64_t m_Minor;
		uint64_t m_ProtocolVersion;
		uint64_t m_Capabilities;
		uint64_t m_MaxDatagramSize;
		unsigned char m_aNonce[NONCE_SIZE];
		CByteView m_ResumeToken;
	};

	struct CMapHeaderView
	{
		EMapFormat m_Format;
		uint64_t m_Size;
		uint32_t m_Crc;
		unsigned char m_aSha256[MAP_SHA256_SIZE];
		CByteView m_Name;
	};

	struct CResumeView
	{
		uint64_t m_SessionId;
		CByteView m_Token;
	};

	struct CDatagramView
	{
		uint64_t m_Sequence;
		const unsigned char *m_pData;
		size_t m_Size;
		size_t m_Offset;
		uint64_t m_MessagesRemaining;
	};

	size_t VarIntSize(uint64_t Value);
	bool EncodeVarInt(uint64_t Value, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeVarInt(const unsigned char *pData, size_t Size, uint64_t &Value, size_t &BytesConsumed);

	bool EncodeStreamHeader(EStreamKind Kind, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeStreamHeader(const unsigned char *pData, size_t Size, CStreamHeader &Header);

	bool EncodeFrame(EFrameType Type, CByteView Payload, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeFrame(const unsigned char *pData, size_t Size, CFrameView &Frame);

	bool EncodeHello(const CHelloView &Hello, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeHello(CByteView Payload, CHelloView &Hello);

	bool EncodeMapHeader(const CMapHeaderView &Header, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeMapHeader(CByteView Payload, CMapHeaderView &Header);

	bool EncodeResume(const CResumeView &Resume, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeResume(CByteView Payload, CResumeView &Resume);

	bool EncodeDatagram(uint64_t Sequence, const CByteView *pMessages, size_t NumMessages, std::vector<unsigned char> &vOut);
	EDecodeResult DecodeDatagram(const unsigned char *pData, size_t Size, CDatagramView &Datagram);
	bool NextDatagramMessage(CDatagramView &Datagram, CByteView &Message);
}

#endif
