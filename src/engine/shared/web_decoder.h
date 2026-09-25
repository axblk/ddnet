/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_WEB_DECODER_H
#define ENGINE_SHARED_WEB_DECODER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

/**
 * A decode the browser does beside the program, in a worker of its own
 * (web_decoder.js) that the page's thread starts and that writes straight
 * into the memory the caller provides. For the switch that leaves sounds to
 * the browser (`WEB_OPUS`).
 *
 * The input and the output must live until the decode has finished; the
 * destructor waits for one that has not.
 */
class CWebDecode
{
	enum
	{
		STATE_IDLE,
		STATE_PENDING,
		STATE_DONE,
		STATE_FAILED,
	};
	// Written by the decoder, which wakes whoever waits on it
	std::atomic<int32_t> m_State{STATE_IDLE};

public:
	CWebDecode() = default;
	~CWebDecode();

	CWebDecode(const CWebDecode &) = delete;
	CWebDecode &operator=(const CWebDecode &) = delete;

	/**
	 * Decodes the packets of an Ogg Opus file with the browser's Opus
	 * decoder, to 16-bit samples at 48 kHz like libopusfile gives them:
	 * the samples to skip at the start skipped, soft clipped where the
	 * sound goes beyond full scale.
	 *
	 * @param pFile The bytes of the whole file, for a browser that decodes
	 * only whole files.
	 * @param FileSize The number of bytes of the file.
	 * @param pHead The identification header (OpusHead).
	 * @param HeadSize The size of the header.
	 * @param pPackets The sound packets one after the other.
	 * @param pPacketSizes The size of each packet.
	 * @param NumPackets The number of packets.
	 * @param Channels 1 or 2.
	 * @param pSamples Where the samples go, the channels interleaved.
	 * @param NumFrames How many samples per channel fit into `pSamples`,
	 * where the sound ends.
	 * @param pDecodedFrames Set to how many samples per channel were written.
	 */
	void StartOpus(const uint8_t *pFile, size_t FileSize, const uint8_t *pHead, size_t HeadSize, const uint8_t *pPackets, const uint32_t *pPacketSizes, size_t NumPackets, int Channels, int16_t *pSamples, int64_t NumFrames, int32_t *pDecodedFrames);

	bool Started() const { return m_State.load() != STATE_IDLE; }
	bool Finished() const { return m_State.load() >= STATE_DONE; }

	/**
	 * Waits until the decode has finished.
	 *
	 * @return `true` if it succeeded.
	 */
	bool Wait();
};

#endif
