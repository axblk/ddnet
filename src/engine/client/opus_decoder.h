/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_OPUS_DECODER_H
#define ENGINE_CLIENT_OPUS_DECODER_H

#include <optional>
#include <string>

/**
 * A sound decoded from an Ogg Opus file: 16-bit samples at 48 kHz, which is
 * what Opus decodes to whatever the file was made from, the channels
 * interleaved.
 */
class COpusSound
{
public:
	/**
	 * The samples, allocated with `malloc`, for the caller to take.
	 */
	short *m_pData = nullptr;
	int m_NumFrames = 0;
	int m_Channels = 0;
	/**
	 * The value of the first `LOOP_START` comment, if there is one.
	 */
	std::optional<std::string> m_LoopStart;
};

/**
 * Decodes an Ogg Opus file with libopusfile, or with the browser in browser
 * builds with WEB_OPUS (opus_decoder_opusfile.cpp, opus_decoder_web.cpp).
 * Only mono and stereo are taken.
 *
 * @param pData The bytes of the file.
 * @param DataSize The number of bytes.
 * @param pContextName The name of the file, for error messages.
 * @param Sound Where the result goes.
 *
 * @return `true` on success.
 */
bool DecodeOpus(const void *pData, unsigned DataSize, const char *pContextName, COpusSound &Sound);

#endif
