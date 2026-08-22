#include "game_wire.h"

#include <base/mem.h>

#include <bit>
#include <limits>

namespace GameWire
{
	namespace
	{
		constexpr uint64_t SKIPPABLE_FRAME_START = 64;

		size_t FrameLimit(uint64_t Type)
		{
			switch(static_cast<EFrameType>(Type))
			{
			case EFrameType::CLIENT_HELLO:
			case EFrameType::SERVER_HELLO:
				return MAX_HELLO_SIZE;
			case EFrameType::MESSAGE:
				return MAX_CONTROL_MESSAGE_SIZE;
			case EFrameType::DISCONNECT:
				return 256;
			case EFrameType::RESUME:
				return 128;
			case EFrameType::MAP_HEADER:
				return MAX_MAP_HEADER_SIZE;
			case EFrameType::SERVER_IDENTITY:
			case EFrameType::CLIENT_IDENTITY_READY:
				// Reserved for the native transport. They are skippable, so
				// they get the limit every skippable frame gets below.
				return MAX_CONTROL_MESSAGE_SIZE;
			}
			return Type >= SKIPPABLE_FRAME_START ? MAX_CONTROL_MESSAGE_SIZE : 0;
		}

		bool ReadVarInt(CByteView Data, size_t &Offset, uint64_t &Value)
		{
			size_t Consumed;
			if(Offset >= Data.m_Size || DecodeVarInt(Data.m_pData + Offset, Data.m_Size - Offset, Value, Consumed) != EDecodeResult::OK)
				return false;
			Offset += Consumed;
			return true;
		}
	}

	size_t VarIntSize(uint64_t Value)
	{
		if(Value <= 63)
			return 1;
		if(Value <= 16383)
			return 2;
		if(Value <= 1073741823)
			return 4;
		if(Value <= MAX_VARINT)
			return 8;
		return 0;
	}

	bool EncodeVarInt(uint64_t Value, std::vector<unsigned char> &vOut)
	{
		const size_t Length = VarIntSize(Value);
		if(Length == 0)
			return false;
		const size_t Start = vOut.size();
		vOut.resize(Start + Length);
		for(size_t i = 0; i < Length; ++i)
			vOut[Start + Length - i - 1] = static_cast<unsigned char>(Value >> (i * 8));
		vOut[Start] |= static_cast<unsigned char>(std::countr_zero(Length) << 6);
		return true;
	}

	EDecodeResult DecodeVarInt(const unsigned char *pData, size_t Size, uint64_t &Value, size_t &BytesConsumed)
	{
		if(Size == 0)
			return EDecodeResult::NEED_MORE;
		const size_t Length = size_t{1} << (pData[0] >> 6);
		if(Size < Length)
			return EDecodeResult::NEED_MORE;
		Value = pData[0] & 0x3f;
		for(size_t i = 1; i < Length; ++i)
			Value = (Value << 8) | pData[i];
		BytesConsumed = Length;
		return EDecodeResult::OK;
	}

	bool EncodeStreamHeader(EStreamKind Kind, std::vector<unsigned char> &vOut)
	{
		return EncodeVarInt(static_cast<uint64_t>(Kind), vOut) && EncodeVarInt(VERSION_MAJOR, vOut);
	}

	EDecodeResult DecodeStreamHeader(const unsigned char *pData, size_t Size, CStreamHeader &Header)
	{
		uint64_t Kind;
		uint64_t Version;
		size_t KindSize;
		size_t VersionSize;
		EDecodeResult Result = DecodeVarInt(pData, Size, Kind, KindSize);
		if(Result != EDecodeResult::OK)
			return Result;
		Result = DecodeVarInt(pData + KindSize, Size - KindSize, Version, VersionSize);
		if(Result != EDecodeResult::OK)
			return Result;
		if(Version != VERSION_MAJOR)
			return EDecodeResult::VERSION_MISMATCH;
		if(Kind > static_cast<uint64_t>(EStreamKind::MAP))
			return EDecodeResult::UNKNOWN_REQUIRED;
		Header = {static_cast<EStreamKind>(Kind), KindSize + VersionSize};
		return EDecodeResult::OK;
	}

	bool EncodeFrame(EFrameType Type, CByteView Payload, std::vector<unsigned char> &vOut)
	{
		const uint64_t RawType = static_cast<uint64_t>(Type);
		const size_t Limit = FrameLimit(RawType);
		if(Limit == 0 || Payload.m_Size > Limit || (Payload.m_Size > 0 && Payload.m_pData == nullptr))
			return false;
		if(!EncodeVarInt(RawType, vOut) || !EncodeVarInt(Payload.m_Size, vOut))
			return false;
		if(Payload.m_Size > 0)
			vOut.insert(vOut.end(), Payload.m_pData, Payload.m_pData + Payload.m_Size);
		return true;
	}

	EDecodeResult DecodeFrame(const unsigned char *pData, size_t Size, CFrameView &Frame)
	{
		uint64_t Type;
		uint64_t PayloadSize;
		size_t TypeSize;
		size_t LengthSize;
		EDecodeResult Result = DecodeVarInt(pData, Size, Type, TypeSize);
		if(Result != EDecodeResult::OK)
			return Result;
		Result = DecodeVarInt(pData + TypeSize, Size - TypeSize, PayloadSize, LengthSize);
		if(Result != EDecodeResult::OK)
			return Result;
		const size_t Limit = FrameLimit(Type);
		if(Limit == 0)
			return EDecodeResult::UNKNOWN_REQUIRED;
		if(PayloadSize > Limit || PayloadSize > std::numeric_limits<size_t>::max() - TypeSize - LengthSize)
			return EDecodeResult::LIMIT_EXCEEDED;
		const size_t TotalSize = TypeSize + LengthSize + static_cast<size_t>(PayloadSize);
		if(Size < TotalSize)
			return EDecodeResult::NEED_MORE;
		Frame = {Type, {pData + TypeSize + LengthSize, static_cast<size_t>(PayloadSize)}, TotalSize, Type >= SKIPPABLE_FRAME_START};
		return EDecodeResult::OK;
	}

	bool EncodeHello(const CHelloView &Hello, std::vector<unsigned char> &vOut)
	{
		if(Hello.m_Major != VERSION_MAJOR ||
			Hello.m_ResumeToken.m_Size > MAX_RESUME_TOKEN_SIZE ||
			(Hello.m_ResumeToken.m_Size > 0 && Hello.m_ResumeToken.m_pData == nullptr) ||
			Hello.m_MaxDatagramSize > MAX_DATAGRAM_SIZE)
			return false;
		std::vector<unsigned char> vHello;
		if(!EncodeVarInt(Hello.m_Major, vHello) ||
			!EncodeVarInt(Hello.m_Minor, vHello) ||
			!EncodeVarInt(Hello.m_ProtocolVersion, vHello) ||
			!EncodeVarInt(Hello.m_Capabilities, vHello) ||
			!EncodeVarInt(Hello.m_MaxDatagramSize, vHello))
			return false;
		vHello.insert(vHello.end(), Hello.m_aNonce, Hello.m_aNonce + NONCE_SIZE);
		if(!EncodeVarInt(Hello.m_ResumeToken.m_Size, vHello))
			return false;
		if(Hello.m_ResumeToken.m_Size > 0)
			vHello.insert(vHello.end(), Hello.m_ResumeToken.m_pData, Hello.m_ResumeToken.m_pData + Hello.m_ResumeToken.m_Size);
		if(vHello.size() > MAX_HELLO_SIZE)
			return false;
		vOut.insert(vOut.end(), vHello.begin(), vHello.end());
		return true;
	}

	EDecodeResult DecodeHello(CByteView Payload, CHelloView &Hello)
	{
		if(Payload.m_Size > MAX_HELLO_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		size_t Offset = 0;
		if(!ReadVarInt(Payload, Offset, Hello.m_Major) ||
			!ReadVarInt(Payload, Offset, Hello.m_Minor) ||
			!ReadVarInt(Payload, Offset, Hello.m_ProtocolVersion) ||
			!ReadVarInt(Payload, Offset, Hello.m_Capabilities) ||
			!ReadVarInt(Payload, Offset, Hello.m_MaxDatagramSize))
			return EDecodeResult::MALFORMED;
		if(Hello.m_Major != VERSION_MAJOR)
			return EDecodeResult::VERSION_MISMATCH;
		if(Hello.m_MaxDatagramSize > MAX_DATAGRAM_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		if(Offset + NONCE_SIZE > Payload.m_Size)
			return EDecodeResult::MALFORMED;
		mem_copy(Hello.m_aNonce, Payload.m_pData + Offset, NONCE_SIZE);
		Offset += NONCE_SIZE;
		uint64_t ResumeSize;
		if(!ReadVarInt(Payload, Offset, ResumeSize))
			return EDecodeResult::MALFORMED;
		if(ResumeSize > MAX_RESUME_TOKEN_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		if(ResumeSize != Payload.m_Size - Offset)
			return EDecodeResult::MALFORMED;
		Hello.m_ResumeToken = {Payload.m_pData + Offset, static_cast<size_t>(ResumeSize)};
		return EDecodeResult::OK;
	}

	bool EncodeMapHeader(const CMapHeaderView &Header, std::vector<unsigned char> &vOut)
	{
		if(Header.m_Format != EMapFormat::DATAFILE || Header.m_Size == 0 || Header.m_Size > MAX_MAP_SIZE ||
			Header.m_Name.m_Size == 0 || Header.m_Name.m_Size > MAX_MAP_NAME_SIZE || Header.m_Name.m_pData == nullptr)
			return false;
		std::vector<unsigned char> vHeader;
		vHeader.reserve(4 * 8 + MAP_SHA256_SIZE + Header.m_Name.m_Size);
		if(!EncodeVarInt(static_cast<uint64_t>(Header.m_Format), vHeader) ||
			!EncodeVarInt(Header.m_Size, vHeader) ||
			!EncodeVarInt(Header.m_Crc, vHeader))
			return false;
		vHeader.insert(vHeader.end(), Header.m_aSha256, Header.m_aSha256 + MAP_SHA256_SIZE);
		if(!EncodeVarInt(Header.m_Name.m_Size, vHeader))
			return false;
		vHeader.insert(vHeader.end(), Header.m_Name.m_pData, Header.m_Name.m_pData + Header.m_Name.m_Size);
		if(vHeader.size() > MAX_MAP_HEADER_SIZE)
			return false;
		vOut.insert(vOut.end(), vHeader.begin(), vHeader.end());
		return true;
	}

	EDecodeResult DecodeMapHeader(CByteView Payload, CMapHeaderView &Header)
	{
		if(Payload.m_Size > MAX_MAP_HEADER_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		size_t Offset = 0;
		uint64_t Format;
		uint64_t Crc;
		uint64_t NameSize;
		if(!ReadVarInt(Payload, Offset, Format) || !ReadVarInt(Payload, Offset, Header.m_Size) || !ReadVarInt(Payload, Offset, Crc))
			return EDecodeResult::MALFORMED;
		if(Format != static_cast<uint64_t>(EMapFormat::DATAFILE))
			return EDecodeResult::UNKNOWN_REQUIRED;
		if(Header.m_Size == 0 || Header.m_Size > MAX_MAP_SIZE || Crc > std::numeric_limits<uint32_t>::max())
			return EDecodeResult::LIMIT_EXCEEDED;
		if(Offset > Payload.m_Size || MAP_SHA256_SIZE > Payload.m_Size - Offset)
			return EDecodeResult::MALFORMED;
		Header.m_Format = static_cast<EMapFormat>(Format);
		Header.m_Crc = static_cast<uint32_t>(Crc);
		mem_copy(Header.m_aSha256, Payload.m_pData + Offset, MAP_SHA256_SIZE);
		Offset += MAP_SHA256_SIZE;
		if(!ReadVarInt(Payload, Offset, NameSize))
			return EDecodeResult::MALFORMED;
		if(NameSize == 0 || NameSize > MAX_MAP_NAME_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		if(NameSize != Payload.m_Size - Offset)
			return EDecodeResult::MALFORMED;
		Header.m_Name = {Payload.m_pData + Offset, static_cast<size_t>(NameSize)};
		return EDecodeResult::OK;
	}

	bool EncodeResume(const CResumeView &Resume, std::vector<unsigned char> &vOut)
	{
		if(Resume.m_SessionId == 0 || Resume.m_Token.m_Size == 0 || Resume.m_Token.m_Size > MAX_RESUME_TOKEN_SIZE || Resume.m_Token.m_pData == nullptr)
			return false;
		if(!EncodeVarInt(Resume.m_SessionId, vOut) || !EncodeVarInt(Resume.m_Token.m_Size, vOut))
			return false;
		vOut.insert(vOut.end(), Resume.m_Token.m_pData, Resume.m_Token.m_pData + Resume.m_Token.m_Size);
		return true;
	}

	EDecodeResult DecodeResume(CByteView Payload, CResumeView &Resume)
	{
		size_t Offset = 0;
		uint64_t TokenSize;
		if(!ReadVarInt(Payload, Offset, Resume.m_SessionId) || !ReadVarInt(Payload, Offset, TokenSize))
			return EDecodeResult::MALFORMED;
		if(Resume.m_SessionId == 0 || TokenSize == 0 || TokenSize > MAX_RESUME_TOKEN_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		if(TokenSize != Payload.m_Size - Offset)
			return EDecodeResult::MALFORMED;
		Resume.m_Token = {Payload.m_pData + Offset, static_cast<size_t>(TokenSize)};
		return EDecodeResult::OK;
	}

	bool EncodeDatagram(uint64_t Sequence, const CByteView *pMessages, size_t NumMessages, std::vector<unsigned char> &vOut)
	{
		// Written straight into the output and rolled back on failure, so a packet
		// on the send path costs no allocation of its own.
		const size_t Start = vOut.size();
		auto Fail = [&]() {
			vOut.resize(Start);
			return false;
		};
		if(NumMessages == 0 || NumMessages > MAX_DATAGRAM_MESSAGES)
			return false;
		if(!EncodeVarInt(VERSION_MAJOR, vOut) ||
			!EncodeVarInt(static_cast<uint64_t>(EDatagramType::MESSAGES), vOut) ||
			!EncodeVarInt(Sequence, vOut) ||
			!EncodeVarInt(NumMessages, vOut))
			return Fail();
		for(size_t i = 0; i < NumMessages; i++)
		{
			const CByteView Message = pMessages[i];
			if(Message.m_Size == 0 || Message.m_Size > MAX_DATAGRAM_MESSAGE_SIZE || Message.m_pData == nullptr || !EncodeVarInt(Message.m_Size, vOut))
				return Fail();
			vOut.insert(vOut.end(), Message.m_pData, Message.m_pData + Message.m_Size);
		}
		if(vOut.size() - Start > MAX_DATAGRAM_SIZE)
			return Fail();
		return true;
	}

	EDecodeResult DecodeDatagram(const unsigned char *pData, size_t Size, CDatagramView &Datagram)
	{
		if(Size > MAX_DATAGRAM_SIZE)
			return EDecodeResult::LIMIT_EXCEEDED;
		CByteView Data = {pData, Size};
		size_t Offset = 0;
		uint64_t Version;
		uint64_t Type;
		uint64_t Sequence;
		uint64_t MessageCount;
		if(!ReadVarInt(Data, Offset, Version) || !ReadVarInt(Data, Offset, Type) || !ReadVarInt(Data, Offset, Sequence) || !ReadVarInt(Data, Offset, MessageCount))
			return EDecodeResult::MALFORMED;
		if(Version != VERSION_MAJOR)
			return EDecodeResult::VERSION_MISMATCH;
		if(Type != static_cast<uint64_t>(EDatagramType::MESSAGES))
			return EDecodeResult::UNKNOWN_REQUIRED;
		if(MessageCount == 0 || MessageCount > MAX_DATAGRAM_MESSAGES)
			return EDecodeResult::LIMIT_EXCEEDED;
		const size_t MessagesOffset = Offset;
		for(uint64_t i = 0; i < MessageCount; ++i)
		{
			uint64_t MessageSize;
			if(!ReadVarInt(Data, Offset, MessageSize))
				return EDecodeResult::MALFORMED;
			if(MessageSize == 0 || MessageSize > MAX_DATAGRAM_MESSAGE_SIZE)
				return EDecodeResult::LIMIT_EXCEEDED;
			if(MessageSize > Data.m_Size - Offset)
				return EDecodeResult::MALFORMED;
			Offset += static_cast<size_t>(MessageSize);
		}
		if(Offset != Size)
			return EDecodeResult::MALFORMED;
		Datagram = {Sequence, pData, Size, MessagesOffset, MessageCount};
		return EDecodeResult::OK;
	}

	bool NextDatagramMessage(CDatagramView &Datagram, CByteView &Message)
	{
		if(Datagram.m_MessagesRemaining == 0 || Datagram.m_Offset >= Datagram.m_Size)
			return false;
		uint64_t MessageSize;
		size_t LengthSize;
		if(DecodeVarInt(Datagram.m_pData + Datagram.m_Offset, Datagram.m_Size - Datagram.m_Offset, MessageSize, LengthSize) != EDecodeResult::OK)
			return false;
		Datagram.m_Offset += LengthSize;
		if(MessageSize > Datagram.m_Size - Datagram.m_Offset)
			return false;
		Message = {Datagram.m_pData + Datagram.m_Offset, static_cast<size_t>(MessageSize)};
		Datagram.m_Offset += static_cast<size_t>(MessageSize);
		--Datagram.m_MessagesRemaining;
		return true;
	}
}
