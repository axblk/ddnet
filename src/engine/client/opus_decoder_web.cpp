/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
// Decoding Opus with the browser, for browser builds with WEB_OPUS. The Ogg
// container is read here: the header with the channels and the samples to
// skip, the comments, and the packets, which the browser's Opus decoder
// (WebCodecs) takes one by one. The length comes from the granule positions,
// the way libopusfile counts it.
#include "opus_decoder.h"

#include <base/log.h>
#include <base/str.h>

#include <engine/shared/web_decoder.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
	constexpr size_t PAGE_HEADER_SIZE = 27;
	constexpr uint8_t PAGE_CONTINUED = 1 << 0;
	constexpr uint8_t PAGE_FIRST = 1 << 1;
	constexpr uint8_t PAGE_LAST = 1 << 2;
	// The granule position of a page on which no packet ends
	constexpr int64_t NO_GRANULE = -1;
	// Where Opus puts its samples
	constexpr int64_t OPUS_RATE = 48000;

	uint32_t ReadLe32(const uint8_t *pData)
	{
		return (uint32_t)pData[0] | (uint32_t)pData[1] << 8 | (uint32_t)pData[2] << 16 | (uint32_t)pData[3] << 24;
	}

	// Ogg's CRC: polynomial 0x04c11db7, not reflected, starting at zero
	constexpr std::array<uint32_t, 256> OGG_CRC_TABLE = []() {
		std::array<uint32_t, 256> aTable{};
		for(uint32_t i = 0; i < 256; ++i)
		{
			uint32_t Crc = i << 24;
			for(int Bit = 0; Bit < 8; ++Bit)
				Crc = Crc & 0x80000000 ? Crc << 1 ^ 0x04c11db7 : Crc << 1;
			aTable[i] = Crc;
		}
		return aTable;
	}();

	uint32_t OggCrc(uint32_t Crc, const uint8_t *pData, size_t Size)
	{
		for(size_t i = 0; i < Size; ++i)
			Crc = Crc << 8 ^ OGG_CRC_TABLE[(Crc >> 24 ^ pData[i]) & 0xff];
		return Crc;
	}

	// The checksum of a page is taken with its own field zero
	uint32_t PageCrc(const uint8_t *pPage, size_t Size)
	{
		constexpr uint8_t ZERO[4] = {};
		uint32_t Crc = OggCrc(0, pPage, 22);
		Crc = OggCrc(Crc, ZERO, sizeof(ZERO));
		return OggCrc(Crc, pPage + 26, Size - 26);
	}

	// How many samples at 48 kHz a packet holds, from its first byte (the
	// TOC) and, for several frames of any size, its second. -1 if the packet
	// is not one.
	int PacketDuration(const uint8_t *pPacket, size_t Size)
	{
		if(Size == 0)
			return -1;
		const uint8_t Toc = pPacket[0];
		int FrameSize;
		if(Toc & 0x80)
			FrameSize = 120 << (Toc >> 3 & 3); // CELT: 2.5 to 20 ms
		else if((Toc & 0x60) == 0x60)
			FrameSize = Toc & 0x08 ? 960 : 480; // hybrid: 10 or 20 ms
		else
			FrameSize = (Toc >> 3 & 3) == 3 ? 2880 : 480 << (Toc >> 3 & 3); // SILK: 10 to 60 ms
		int NumFrames;
		switch(Toc & 3)
		{
		case 0: NumFrames = 1; break;
		case 1:
		case 2: NumFrames = 2; break;
		default:
			if(Size < 2)
				return -1;
			NumFrames = pPacket[1] & 0x3f;
		}
		const int Duration = NumFrames * FrameSize;
		// At most 120 ms
		return Duration <= 5760 ? Duration : -1;
	}

	/**
	 * The packets of the first logical stream of an Ogg file, which is
	 * where an Opus file has its sound.
	 */
	class COggOpusStream
	{
	public:
		std::vector<uint8_t> m_vHead;
		std::vector<uint8_t> m_vTags;
		// The sound packets one after the other and how long each one is
		std::vector<uint8_t> m_vPackets;
		std::vector<uint32_t> m_vPacketSizes;
		// Where the sound starts and ends, in samples at 48 kHz
		int64_t m_Start = 0;
		int64_t m_End = 0;

		// The size of the page at the start of the bytes, 0 if there is no
		// whole page with the right checksum.
		static size_t PageSize(const uint8_t *pPage, size_t Size)
		{
			if(Size < PAGE_HEADER_SIZE || std::memcmp(pPage, "OggS", 4) != 0 || pPage[4] != 0)
				return 0;
			const size_t NumSegments = pPage[26];
			if(Size - PAGE_HEADER_SIZE < NumSegments)
				return 0;
			size_t PageSize = PAGE_HEADER_SIZE + NumSegments;
			for(size_t i = 0; i < NumSegments; ++i)
				PageSize += pPage[PAGE_HEADER_SIZE + i];
			if(Size < PageSize || PageCrc(pPage, PageSize) != ReadLe32(pPage + 22))
				return 0;
			return PageSize;
		}

		const char *Read(const uint8_t *pData, size_t Size)
		{
			size_t Offset = 0;
			bool First = true;
			bool StartKnown = false;
			uint32_t Serial = 0;
			uint32_t NextSequence = 0;
			// Whether a page is missing, whose packets are lost
			bool Lost = false;
			size_t NumPackets = 0;
			// The duration of the sound packets that ended before the start
			// is known
			int64_t Duration = 0;
			std::vector<uint8_t> vPacket;
			bool Ended = false;
			while(!Ended && Offset < Size)
			{
				const uint8_t *pHeader = pData + Offset;
				const size_t ThisPageSize = PageSize(pHeader, Size - Offset);
				if(ThisPageSize == 0)
				{
					if(First)
						return "not an Ogg file";
					// A broken or cut off page is skipped like libogg does,
					// on to the next one there is.
					constexpr char CAPTURE[] = "OggS";
					const uint8_t *pNext = std::search(pHeader + 1, pData + Size, CAPTURE, CAPTURE + 4);
					Offset = pNext - pData;
					Lost = true;
					continue;
				}
				Offset += ThisPageSize;
				const uint8_t Flags = pHeader[5];
				const int64_t Granule = (int64_t)((uint64_t)ReadLe32(pHeader + 6) | (uint64_t)ReadLe32(pHeader + 10) << 32);
				const uint32_t PageSerial = ReadLe32(pHeader + 14);
				const uint32_t Sequence = ReadLe32(pHeader + 18);
				const size_t NumSegments = pHeader[26];
				const uint8_t *pSegments = pHeader + PAGE_HEADER_SIZE;

				if(First)
				{
					if(!(Flags & PAGE_FIRST))
						return "no start of stream";
					Serial = PageSerial;
					NextSequence = Sequence;
					First = false;
				}
				// Another stream in the same file
				if(PageSerial != Serial)
					continue;
				Ended = Flags & PAGE_LAST;
				Lost = Lost || Sequence != NextSequence;
				NextSequence = Sequence + 1;

				const uint8_t *pBody = pSegments + NumSegments;
				size_t Segment = 0;
				if(Lost || !(Flags & PAGE_CONTINUED))
					vPacket.clear();
				// The rest of a packet whose start is lost
				if(Lost && (Flags & PAGE_CONTINUED))
				{
					while(Segment < NumSegments && pSegments[Segment] == 255)
						pBody += pSegments[Segment++];
					if(Segment < NumSegments)
						pBody += pSegments[Segment++];
				}
				Lost = false;

				bool EndsSound = false;
				for(; Segment < NumSegments; ++Segment)
				{
					vPacket.insert(vPacket.end(), pBody, pBody + pSegments[Segment]);
					pBody += pSegments[Segment];
					if(pSegments[Segment] == 255)
						continue;
					// A packet ends here
					if(NumPackets == 0)
						m_vHead = std::move(vPacket);
					else if(NumPackets == 1)
						m_vTags = std::move(vPacket);
					else
					{
						const int PacketSamples = PacketDuration(vPacket.data(), vPacket.size());
						if(PacketSamples < 0)
							return "invalid packet";
						if(!StartKnown)
							Duration += PacketSamples;
						m_vPackets.insert(m_vPackets.end(), vPacket.begin(), vPacket.end());
						m_vPacketSizes.push_back(vPacket.size());
						EndsSound = true;
					}
					vPacket.clear();
					++NumPackets;
				}
				if(!EndsSound || Granule == NO_GRANULE)
					continue;
				// The first page with sound says where the sound starts: its
				// granule less the samples of its packets. A last page may
				// have fewer, the end is cut off.
				if(!StartKnown)
				{
					m_Start = Granule - Duration;
					if(m_Start < 0)
					{
						if(!Ended)
							return "bad timestamp";
						m_Start = 0;
					}
					StartKnown = true;
				}
				m_End = Granule;
			}
			if(NumPackets < 2)
				return "no header";
			return nullptr;
		}
	};

	// The comments of an OpusTags packet: a vendor string, then the comments,
	// each with its length first.
	const char *ReadLoopStart(const std::vector<uint8_t> &vTags, std::optional<std::string> &LoopStart)
	{
		const size_t Size = vTags.size();
		if(Size < 16 || std::memcmp(vTags.data(), "OpusTags", 8) != 0)
			return "no comments";
		size_t Offset = 8;
		const uint32_t VendorLength = ReadLe32(vTags.data() + Offset);
		Offset += 4;
		if(Size - Offset < (size_t)VendorLength + 4)
			return "comments cut short";
		Offset += VendorLength;
		const uint32_t NumComments = ReadLe32(vTags.data() + Offset);
		Offset += 4;
		for(uint32_t i = 0; i < NumComments; ++i)
		{
			if(Size - Offset < 4)
				return "comments cut short";
			const uint32_t Length = ReadLe32(vTags.data() + Offset);
			Offset += 4;
			if(Size - Offset < Length)
				return "comments cut short";
			const std::string Comment(vTags.begin() + Offset, vTags.begin() + Offset + Length);
			Offset += Length;
			if(!LoopStart.has_value() && str_startswith(Comment.c_str(), "LOOP_START="))
				LoopStart = Comment.substr(str_length("LOOP_START="));
		}
		return nullptr;
	}
} // namespace

bool DecodeOpus(const void *pData, unsigned DataSize, const char *pContextName, COpusSound &Sound)
{
	COggOpusStream Stream;
	if(const char *pError = Stream.Read(static_cast<const uint8_t *>(pData), DataSize))
	{
		log_error("sound/opus", "Failed to decode sample, %s. Filename='%s'", pError, pContextName);
		return false;
	}

	// The identification header: version, channels, samples to skip, rate of
	// the original, gain and how the channels are laid out
	const std::vector<uint8_t> &vHead = Stream.m_vHead;
	if(vHead.size() < 19 || std::memcmp(vHead.data(), "OpusHead", 8) != 0 || (vHead[8] & 0xf0) != 0 || vHead[9] == 0)
	{
		log_error("sound/opus", "Failed to decode sample, bad header. Filename='%s'", pContextName);
		return false;
	}
	const int NumChannels = vHead[9];
	const int PreSkip = vHead[10] | vHead[11] << 8;
	if(NumChannels > 2)
	{
		log_error("sound/opus", "File is not mono or stereo. Filename='%s'", pContextName);
		return false;
	}

	std::optional<std::string> LoopStart;
	if(const char *pError = ReadLoopStart(Stream.m_vTags, LoopStart))
	{
		log_error("sound/opus", "Failed to decode sample, %s. Filename='%s'", pError, pContextName);
		return false;
	}

	const int64_t NumSamples = Stream.m_End - Stream.m_Start - PreSkip;
	if(NumSamples < 0 || NumSamples > 60 * 60 * OPUS_RATE)
	{
		log_error("sound/opus", "Failed to get number of samples, error %d. Filename='%s'", (int)NumSamples, pContextName);
		return false;
	}

	short *pSampleData = (short *)calloc((size_t)NumSamples * NumChannels, sizeof(short));
	int32_t Decoded = 0;
	{
		CWebDecode Decode;
		Decode.StartOpus(static_cast<const uint8_t *>(pData), DataSize, vHead.data(), vHead.size(), Stream.m_vPackets.data(), Stream.m_vPacketSizes.data(), Stream.m_vPacketSizes.size(), NumChannels, pSampleData, NumSamples, &Decoded);
		if(!Decode.Wait())
		{
			free(pSampleData);
			log_error("sound/opus", "Failed to decode sample, the browser could not decode it. Filename='%s'", pContextName);
			return false;
		}
	}

	Sound.m_pData = pSampleData;
	Sound.m_NumFrames = Decoded;
	Sound.m_Channels = NumChannels;
	Sound.m_LoopStart = std::move(LoopStart);
	return true;
}
