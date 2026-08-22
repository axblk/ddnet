#include <engine/shared/game_wire.h>

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace GameWire;

TEST(GameWire, VarIntGoldenVectors)
{
	struct CVector
	{
		uint64_t m_Value;
		std::vector<unsigned char> m_vEncoded;
	};
	const CVector aVectors[] = {
		{0, {0x00}},
		{63, {0x3f}},
		{64, {0x40, 0x40}},
		{16383, {0x7f, 0xff}},
		{16384, {0x80, 0x00, 0x40, 0x00}},
		{1073741823, {0xbf, 0xff, 0xff, 0xff}},
		{1073741824, {0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}},
		{MAX_VARINT, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
	};

	for(const CVector &Vector : aVectors)
	{
		std::vector<unsigned char> vEncoded;
		ASSERT_TRUE(EncodeVarInt(Vector.m_Value, vEncoded));
		EXPECT_EQ(vEncoded, Vector.m_vEncoded);
		uint64_t Decoded;
		size_t Consumed;
		ASSERT_EQ(DecodeVarInt(vEncoded.data(), vEncoded.size(), Decoded, Consumed), EDecodeResult::OK);
		EXPECT_EQ(Decoded, Vector.m_Value);
		EXPECT_EQ(Consumed, vEncoded.size());
	}
	std::vector<unsigned char> vEncoded;
	EXPECT_FALSE(EncodeVarInt(MAX_VARINT + 1, vEncoded));
}

TEST(GameWire, StreamAndFrameGoldenVectors)
{
	std::vector<unsigned char> vStream;
	ASSERT_TRUE(EncodeStreamHeader(EStreamKind::CONTROL, vStream));
	EXPECT_EQ(vStream, (std::vector<unsigned char>{0x00, 0x01}));

	CStreamHeader Header;
	ASSERT_EQ(DecodeStreamHeader(vStream.data(), vStream.size(), Header), EDecodeResult::OK);
	EXPECT_EQ(Header.m_Kind, EStreamKind::CONTROL);
	EXPECT_EQ(Header.m_BytesConsumed, 2);

	const unsigned char aPayload[] = {1, 2, 3};
	std::vector<unsigned char> vFrame;
	ASSERT_TRUE(EncodeFrame(EFrameType::MESSAGE, {aPayload, sizeof(aPayload)}, vFrame));
	EXPECT_EQ(vFrame, (std::vector<unsigned char>{0x02, 0x03, 0x01, 0x02, 0x03}));

	CFrameView Frame;
	EXPECT_EQ(DecodeFrame(vFrame.data(), vFrame.size() - 1, Frame), EDecodeResult::NEED_MORE);
	ASSERT_EQ(DecodeFrame(vFrame.data(), vFrame.size(), Frame), EDecodeResult::OK);
	EXPECT_EQ(Frame.m_Type, static_cast<uint64_t>(EFrameType::MESSAGE));
	EXPECT_EQ(Frame.m_Payload.m_Size, sizeof(aPayload));
	EXPECT_FALSE(Frame.m_Skippable);

	const unsigned char aUnknownRequired[] = {0x06, 0x00};
	EXPECT_EQ(DecodeFrame(aUnknownRequired, sizeof(aUnknownRequired), Frame), EDecodeResult::UNKNOWN_REQUIRED);
	const unsigned char aUnknownSkippable[] = {0x40, 0x40, 0x00};
	ASSERT_EQ(DecodeFrame(aUnknownSkippable, sizeof(aUnknownSkippable), Frame), EDecodeResult::OK);
	EXPECT_TRUE(Frame.m_Skippable);

	std::vector<unsigned char> vOversized = {static_cast<unsigned char>(EFrameType::MESSAGE)};
	ASSERT_TRUE(EncodeVarInt(MAX_CONTROL_MESSAGE_SIZE + 1, vOversized));
	EXPECT_EQ(DecodeFrame(vOversized.data(), vOversized.size(), Frame), EDecodeResult::LIMIT_EXCEEDED);
}

TEST(GameWire, HelloRoundtripAndLimits)
{
	CHelloView Hello = {};
	Hello.m_Major = VERSION_MAJOR;
	Hello.m_Minor = VERSION_MINOR;
	Hello.m_ProtocolVersion = 19000;
	Hello.m_Capabilities = CAPABILITY_DATAGRAM | CAPABILITY_MAP_STREAM | CAPABILITY_RESUME;
	Hello.m_MaxDatagramSize = MAX_DATAGRAM_SIZE;
	for(size_t i = 0; i < NONCE_SIZE; ++i)
		Hello.m_aNonce[i] = static_cast<unsigned char>(i);
	const unsigned char aResume[] = {9, 8};
	Hello.m_ResumeToken = {aResume, sizeof(aResume)};

	std::vector<unsigned char> vEncoded;
	ASSERT_TRUE(EncodeHello(Hello, vEncoded));
	CHelloView Decoded = {};
	ASSERT_EQ(DecodeHello({vEncoded.data(), vEncoded.size()}, Decoded), EDecodeResult::OK);
	EXPECT_EQ(Decoded.m_ProtocolVersion, Hello.m_ProtocolVersion);
	EXPECT_EQ(Decoded.m_Capabilities, Hello.m_Capabilities);
	EXPECT_EQ(Decoded.m_MaxDatagramSize, Hello.m_MaxDatagramSize);
	EXPECT_EQ(Decoded.m_ResumeToken.m_Size, sizeof(aResume));
	EXPECT_EQ(Decoded.m_ResumeToken.m_pData[0], 9);

	vEncoded[0] = 2;
	EXPECT_EQ(DecodeHello({vEncoded.data(), vEncoded.size()}, Decoded), EDecodeResult::VERSION_MISMATCH);
}

TEST(GameWire, DatagramGoldenVector)
{
	const unsigned char aFirst[] = {0xaa, 0xbb};
	const unsigned char aSecond[] = {0xcc};
	std::vector<unsigned char> vEncoded;
	const CByteView aMessages[] = {{aFirst, sizeof(aFirst)}, {aSecond, sizeof(aSecond)}};
	ASSERT_TRUE(EncodeDatagram(300, aMessages, std::size(aMessages), vEncoded));
	EXPECT_EQ(vEncoded, (std::vector<unsigned char>{0x01, 0x00, 0x41, 0x2c, 0x02, 0x02, 0xaa, 0xbb, 0x01, 0xcc}));

	CDatagramView Datagram;
	ASSERT_EQ(DecodeDatagram(vEncoded.data(), vEncoded.size(), Datagram), EDecodeResult::OK);
	EXPECT_EQ(Datagram.m_Sequence, 300);
	CByteView Message;
	ASSERT_TRUE(NextDatagramMessage(Datagram, Message));
	EXPECT_EQ(Message.m_Size, 2);
	EXPECT_EQ(Message.m_pData[0], 0xaa);
	ASSERT_TRUE(NextDatagramMessage(Datagram, Message));
	EXPECT_EQ(Message.m_Size, 1);
	EXPECT_EQ(Message.m_pData[0], 0xcc);
	EXPECT_FALSE(NextDatagramMessage(Datagram, Message));

	vEncoded.pop_back();
	EXPECT_EQ(DecodeDatagram(vEncoded.data(), vEncoded.size(), Datagram), EDecodeResult::MALFORMED);
	std::array<unsigned char, MAX_DATAGRAM_SIZE + 1> aOversized = {};
	EXPECT_EQ(DecodeDatagram(aOversized.data(), aOversized.size(), Datagram), EDecodeResult::LIMIT_EXCEEDED);
}

TEST(GameWire, MapHeaderAndResumeGoldenVectors)
{
	CMapHeaderView Header = {};
	Header.m_Format = EMapFormat::DATAFILE;
	Header.m_Size = 300;
	Header.m_Crc = 42;
	for(size_t i = 0; i < MAP_SHA256_SIZE; ++i)
		Header.m_aSha256[i] = static_cast<unsigned char>(i);
	const unsigned char aName[] = {'m', 'a', 'p'};
	Header.m_Name = {aName, sizeof(aName)};
	std::vector<unsigned char> vEncoded;
	ASSERT_TRUE(EncodeMapHeader(Header, vEncoded));
	std::vector<unsigned char> vExpected = {0x00, 0x41, 0x2c, 0x2a};
	for(size_t i = 0; i < MAP_SHA256_SIZE; ++i)
		vExpected.push_back(static_cast<unsigned char>(i));
	vExpected.push_back(0x03);
	vExpected.push_back('m');
	vExpected.push_back('a');
	vExpected.push_back('p');
	EXPECT_EQ(vEncoded, vExpected);

	CMapHeaderView Decoded = {};
	ASSERT_EQ(DecodeMapHeader({vEncoded.data(), vEncoded.size()}, Decoded), EDecodeResult::OK);
	EXPECT_EQ(Decoded.m_Size, 300);
	EXPECT_EQ(Decoded.m_Crc, 42);
	EXPECT_EQ(Decoded.m_aSha256[31], 31);
	EXPECT_EQ(Decoded.m_Name.m_Size, 3);
	EXPECT_EQ(Decoded.m_Name.m_pData[0], 'm');

	const unsigned char aToken[] = {9, 8, 7};
	vEncoded.clear();
	ASSERT_TRUE(EncodeResume({300, {aToken, sizeof(aToken)}}, vEncoded));
	EXPECT_EQ(vEncoded, (std::vector<unsigned char>{0x41, 0x2c, 0x03, 9, 8, 7}));
	CResumeView Resume = {};
	ASSERT_EQ(DecodeResume({vEncoded.data(), vEncoded.size()}, Resume), EDecodeResult::OK);
	EXPECT_EQ(Resume.m_SessionId, 300);
	EXPECT_EQ(Resume.m_Token.m_Size, 3);

	Header.m_Size = 0;
	EXPECT_FALSE(EncodeMapHeader(Header, vEncoded));
	EXPECT_EQ(DecodeResume({vEncoded.data(), vEncoded.size() - 1}, Resume), EDecodeResult::MALFORMED);
	const unsigned char aUnknownMapFormat[] = {0x01, 0x01, 0x00};
	EXPECT_EQ(DecodeMapHeader({aUnknownMapFormat, sizeof(aUnknownMapFormat)}, Decoded), EDecodeResult::UNKNOWN_REQUIRED);
	vExpected.pop_back();
	EXPECT_EQ(DecodeMapHeader({vExpected.data(), vExpected.size()}, Decoded), EDecodeResult::MALFORMED);
}

TEST(GameWire, DeterministicParserFuzz)
{
	std::array<unsigned char, MAX_DATAGRAM_SIZE> aData = {};
	uint32_t State = 0x4d595df4;
	for(size_t Size = 0; Size <= aData.size(); ++Size)
	{
		if(Size > 0)
		{
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			aData[Size - 1] = static_cast<unsigned char>(State);
		}
		CFrameView Frame;
		DecodeFrame(aData.data(), Size, Frame);
		CHelloView Hello;
		DecodeHello({aData.data(), Size}, Hello);
		CMapHeaderView MapHeader;
		DecodeMapHeader({aData.data(), Size}, MapHeader);
		CResumeView Resume;
		DecodeResume({aData.data(), Size}, Resume);
		CDatagramView Datagram;
		DecodeDatagram(aData.data(), Size, Datagram);
	}
}
