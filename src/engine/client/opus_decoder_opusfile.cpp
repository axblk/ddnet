/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "opus_decoder.h"

#include <base/log.h>
#include <base/str.h>

extern "C" {
#include <opusfile.h>
}

#include <cstdlib>

bool DecodeOpus(const void *pData, unsigned DataSize, const char *pContextName, COpusSound &Sound)
{
	int OpusError = 0;
	OggOpusFile *pOpusFile = op_open_memory((const unsigned char *)pData, DataSize, &OpusError);
	if(!pOpusFile)
	{
		log_error("sound/opus", "Failed to decode sample, error %d. Filename='%s'", OpusError, pContextName);
		return false;
	}

	const int NumChannels = op_channel_count(pOpusFile, -1);
	if(NumChannels > 2)
	{
		op_free(pOpusFile);
		log_error("sound/opus", "File is not mono or stereo. Filename='%s'", pContextName);
		return false;
	}

	const int NumSamples = op_pcm_total(pOpusFile, -1); // per channel!
	if(NumSamples < 0)
	{
		op_free(pOpusFile);
		log_error("sound/opus", "Failed to get number of samples, error %d. Filename='%s'", NumSamples, pContextName);
		return false;
	}

	short *pSampleData = (short *)calloc((size_t)NumSamples * NumChannels, sizeof(short));

	int Pos = 0;
	while(Pos < NumSamples)
	{
		const int Read = op_read(pOpusFile, pSampleData + Pos * NumChannels, (NumSamples - Pos) * NumChannels, nullptr);
		if(Read < 0)
		{
			free(pSampleData);
			op_free(pOpusFile);
			log_error("sound/opus", "op_read error %d at %d. Filename='%s'", Read, Pos, pContextName);
			return false;
		}
		else if(Read == 0) // EOF
			break;
		Pos += Read;
	}

	Sound.m_pData = pSampleData;
	Sound.m_NumFrames = Pos;
	Sound.m_Channels = NumChannels;
	Sound.m_LoopStart.reset();

	const OpusTags *pTags = op_tags(pOpusFile, -1);
	if(pTags)
	{
		for(int i = 0; i < pTags->comments; ++i)
		{
			const char *pComment = pTags->user_comments[i];
			if(pComment && str_startswith(pComment, "LOOP_START="))
			{
				Sound.m_LoopStart = pComment + str_length("LOOP_START=");
				break;
			}
		}
	}

	op_free(pOpusFile);
	return true;
}
